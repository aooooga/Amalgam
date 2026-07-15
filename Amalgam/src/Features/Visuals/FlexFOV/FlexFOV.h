#pragma once
#include "../../../SDK/SDK.h"
#include "../../../SDK/Definitions/Misc/CViewSetup.h"

#include <vector>

class IMesh;

// Wide-angle FOV via globe reprojection (blinky / flex-fov technique).
//
// Phase 1: capture the scene as a view-aligned cube of 6 faces rendered from
// the player's eye, then blit those faces to screen as debug thumbnails to
// verify the globe captures correctly. No reprojection yet - that is Phase 3.

class CFlexFOV
{
public:
	// View-aligned cube faces (oriented relative to the player's view each frame,
	// so FRONT is centered on the crosshair). Order matches the texture arrays.
	enum EFace
	{
		FACE_FRONT = 0,	// player's view direction
		FACE_BACK,		// directly behind
		FACE_LEFT,		// 90 deg to the left
		FACE_RIGHT,		// 90 deg to the right
		FACE_UP,		// 90 deg up
		FACE_DOWN,		// 90 deg down
		FACE_COUNT
	};

	// Mini-vprof buckets for the FlexFOVDebug overlay: the scene-stage hooks in
	// CViewRender_RenderView.cpp (world / translucent / 3d-skybox, located via
	// their VPROF strings) and the DrawModelExecute hook time themselves into
	// these, attributed to the render context they ran under. "Scene" is the
	// whole ViewDrawScene call, so scene - (world + translucent + sky3d) is the
	// unaccounted remainder; model draws happen *inside* the world / translucent
	// stages, so they're a breakdown of those, not additive.
	enum EProfStage
	{
		PROF_SCENE = 0,		// whole ViewDrawScene call
		PROF_WORLD,			// CSimpleWorldView::Draw (list building + world + opaques)
		PROF_TRANSLUCENT,	// CRendering3dView::DrawTranslucentRenderables
		PROF_SKY3D,			// 3d skybox view
		PROF_MODELS,		// IVModelRender::DrawModelExecute (subset of world/translucent)
		PROF_COUNT
	};
	enum EProfCtx
	{
		CTX_MAIN = 0,	// the real main pass (full or scene-stripped)
		CTX_FACES,		// FlexFOV face captures
		CTX_AUX,		// camera window / rear view captures
		CTX_COUNT
	};

private:
	// Face RT slots. The cube rig uses all 6; the wide rig (two tall faces yawed
	// left/right, used for fov <= ~245 on widescreen) uses slots 0..1 only.
	ITexture* m_pFaceTextures[FACE_COUNT] = {};
	IMaterial* m_pFaceMaterials[FACE_COUNT] = {};

	bool m_bWideRig = false;        // rig the current textures were built for

	// Mini-vprof accumulators; latched into the public m_*Frame snapshots once
	// per frame by SnapshotProfFrame.
	float m_flProfAccum[PROF_COUNT][CTX_COUNT] = {};
	int m_nProfModelsAccum[CTX_COUNT] = {};
	int m_nFaceModelsAccum[FACE_COUNT] = {}; // PROF_MODELS split per capture face
	int m_nW2SAccum = 0;                     // composite WorldToScreen calls

	// Deferred RT destruction. The queued material system renders on a worker
	// thread 1-2 frames behind the main thread (the crash logs show it faulting
	// inside shaderapidx9 on a vstdlib pool thread), so a texture/material we
	// destroy mid-frame can still be referenced by queued draw commands. On a
	// rig/size rebuild the old faces are therefore RETIRED (kept alive, just
	// unhooked) and only actually destroyed once they are several capture frames
	// old - by then no queued command can still reference them.
	struct RetiredFace
	{
		IMaterial* m_pMaterial;
		ITexture* m_pTexture;
		unsigned int m_uFrame;
	};
	std::vector<RetiredFace> m_vRetired;
	unsigned int m_uCaptureFrame = 0;

	// Retained composite meshes. The triangle buckets are cached across frames,
	// but the dynamic-mesh draw path still re-uploaded them every frame (~tens
	// of thousands of identical vertex writes whenever the camera was still).
	// One frame after the buckets stabilize they're baked into per-face static
	// meshes and drawn with zero per-frame upload; frames where the buckets
	// rebuilt (param change, stagger while turning) draw through the dynamic
	// mesh as before, so the per-frame-rebuild case never churns static
	// buffers. m_uCompBucketGen bumps on every bucket rebuild; m_uCompMeshGen
	// is the generation the retained meshes were baked from (0 = none) - the
	// retained path only draws when they match.
	IMesh* m_pCompMesh[FACE_COUNT] = {};
	unsigned int m_uCompBucketGen = 1;
	unsigned int m_uCompMeshGen = 0;

	// Like RetiredFace: the queued render thread can still replay draw commands
	// referencing a static mesh for a couple frames after it went stale, so
	// retired meshes wait out the same safety margin before DestroyStaticMesh.
	struct RetiredMesh
	{
		IMesh* m_pMesh;
		unsigned int m_uFrame;
	};
	std::vector<RetiredMesh> m_vRetiredMeshes;

	// Moves the retained composite meshes onto the retired list and marks the
	// retained generation stale (geometry changed or is going away).
	void RetireCompMeshes();

	// Moves the current face materials/textures onto the retired list (no
	// destruction) so Initialize can build the replacement set.
	void RetireFaces();
	// Destroys retired entries older than the safety margin (all of them when
	// bAll, for the real DLL unload).
	void DrainRetired(bool bAll);

	void RenderFace(void* rcx, const CViewSetup& pViewSetup, int iFace, const Vec3& vAngles, float flFovX, float flAspect);

	// Target square face resolution for the current quality slider (screen height
	// * fidelity compensation * quality). Rebuilt when this changes.
	int DesiredFaceSize();

	// Decides the rig (wide vs cube) and face RT dims for the current fov,
	// aspect and quality. Used identically by Initialize / CaptureGlobe /
	// ShouldReplaceView so they always agree.
	void ComputeRig(bool& bWide, int& iW, int& iH);

public:
	// Captures all 6 faces into their render targets. Called from the
	// CViewRender_RenderView hook (after the original main-view render).
	void CaptureGlobe(void* rcx, const CViewSetup& pViewSetup);

	// True when the composite is active and fully able to render this frame, so
	// the CViewRender_RenderView hook can run the normal main-view render as a
	// cheap pass (the composite covers every pixel anyway).
	bool ShouldReplaceView();

	// Wraps the main-view CALL_ORIGINAL when ShouldReplaceView(): zeroes the
	// scene-draw cvars (world / entities / skybox / viewmodel / particles) so the
	// redundant main render costs almost nothing, while the engine still runs the
	// rest of the pass - crucially the in-game HUD paint that lives inside
	// RenderView, which is what draws the composite. (Skipping CALL_ORIGINAL
	// entirely skipped that paint too and froze the screen.) While
	// m_bReplacingView is set the CViewRender_ViewDrawScene hook additionally
	// skips the scene call outright, cutting the CPU-side setup (world/renderable
	// list building, water views) the cvars can't reach; the cvars stay as the
	// fallback (and still gate the 3d-skybox + viewmodel draws, which live
	// outside ViewDrawScene).
	void BeginCheapMainView();
	void EndCheapMainView();

	// Blits the 6 captured faces as debug thumbnails over the current screen.
	// Called from IEngineVGui_Paint.
	void DrawDebug();

	// Mesh-ladder composite (currently M1: fullscreen FRONT-face quad).
	// Called from IEngineVGui_Paint.
	void DrawComposite();

	// Draws the viewmodels on top of the composite at their native (unwarped)
	// projection, since the capture faces don't contain them (a 130 deg face
	// would warp them badly) and the composite paints over the main pass's.
	// Called from IEngineVGui_Paint right after DrawComposite.
	void DrawViewmodel();

	// Forward flex-FOV projection: maps a world position to its pixel position in
	// the reprojected composite (the exact inverse of DrawComposite's ScreenToRay).
	// Used by SDK::W2S while the composite is active so ESP / overlays land on top
	// of the warped view and track enemies revealed outside the normal 90 cone.
	// Returns true when the point falls within the on-screen [-1,1] clip bounds.
	// bAlways fills vScreen with a best-effort direction even when off-screen.
	bool WorldToScreen(const Vec3& vWorld, Vec3& vScreen, bool bAlways = false);

	void Initialize();
	void Unload();

	// True while we are re-rendering the scene into a face RT. Glow and the
	// camera window skip themselves during the capture pass; chams deliberately
	// do NOT (they must be baked into the faces or the composite covers them).
	bool m_bDrawing = false;

	// True while capturing a non-front cube face with FlexFOVCheapPeriphery on.
	// The DrawModelExecute hook skips cosmetics and distance-culls small
	// entities during these passes, and CaptureGlobe strips per-model detail
	// cvars (eyes/flex/jiggle/LOD) around them.
	bool m_bCheapFace = false;

	int m_iFaceW = 0, m_iFaceH = 0; // face RT resolution (cube: square, W == H)

	// Frustum of the face currently being captured (valid while m_bDrawing):
	// world-space forward of the face camera and its half-diagonal angle in
	// radians. Glow uses these to skip entities the face can't see.
	Vec3 m_vCaptureFwd = {};
	float m_flCaptureHalfAngle = 0.f;

	// True when DrawComposite actually repainted the screen this paint, so
	// DrawViewmodel knows the viewmodels need re-drawing on top.
	bool m_bPaintedComposite = false;

	// Main-view setup latched at the top of the CViewRender_RenderView hook
	// (current frame, since the HUD paint that redraws the viewmodel runs inside
	// that same RenderView call), used to re-render viewmodels after the
	// composite. Deliberately fresher than the composite's face captures: the
	// viewmodel is screen-anchored, so a frame-old setup makes it jitter.
	CViewSetup m_tViewSetup = {};
	bool m_bViewSetupValid = false;

	// True while the cheap (scene-stripped) main-view pass is running, so chams /
	// glow / screen effects can skip work the composite would paint over anyway.
	bool m_bReplacingView = false;

	// Which cube faces the current composite mesh actually samples (recorded when
	// the mesh is rebuilt). CaptureGlobe skips rendering the rest - e.g. BACK is
	// never sampled below ~180 fov, UP/DOWN often aren't at moderate fov.
	bool m_bFaceNeeded[FACE_COUNT] = { true, true, true, true, true, true };

	// Per-face capture snapshot: the world-space view basis the face was last
	// rendered with, plus its capture frame. With staggering (FlexFOVStagger)
	// peripheral faces refresh round-robin instead of every frame; DrawComposite
	// rotates its sampling rays from the current view frame into each face's
	// capture frame, which is exact for camera rotation (no parallax), so stale
	// faces stay world-aligned and only translation/animation lag. Invalid until
	// the face is first captured after a rebuild - the composite must not sample
	// an RT that was never written.
	Vec3 m_vFaceCapFwd[FACE_COUNT] = {};
	Vec3 m_vFaceCapRight[FACE_COUNT] = {};
	Vec3 m_vFaceCapUp[FACE_COUNT] = {};
	unsigned int m_uFaceCapFrame[FACE_COUNT] = {};
	bool m_bFaceCapValid[FACE_COUNT] = {};

	// Tight face frusta: instead of always rendering the full oversized face
	// (130 x 130/145), each capture is narrowed to just the region the composite
	// mesh actually samples off that face (plus a safety margin), re-aiming the
	// face camera at that region's center. The engine then frustum-culls the
	// rest of the world/entities out of the pass - the scene passes are
	// CPU-bound on that work, so this is a direct perf win whenever a face is
	// only partially sampled (side faces at moderate fov, up/down strips, the
	// single wide face below 120 fov, ...).
	//
	// The frustum a face's texture was CAPTURED with, latched alongside the
	// capture basis above: camera basis in the capture view frame's ray space
	// (x = right, y = up, z = -forward) and symmetric half-tangents. All of the
	// composite's containment / assignment / UV math runs against these (not
	// the canonical full faces), so tightened captures keep seams and UVs exact.
	struct FaceFrustum
	{
		Vec3 m_vFwd = {}, m_vRight = {}, m_vUp = {};
		float m_flTanX = 0.f, m_flTanY = 0.f;
	};
	FaceFrustum m_tFaceCapFrustum[FACE_COUNT] = {};
	// Bumped whenever a capture latches a different frustum than the face held
	// before; part of the composite's mesh cache key (stale UVs otherwise).
	unsigned int m_uFrustumGen = 0;

	// Wanted capture rects, written by the composite's mesh build (param-keyed
	// "ideal" assignment against the canonical full faces) and consumed by the
	// next CaptureGlobe: per face, the tangent-space bounds (canonical face
	// frame, tx = right/fwd, ty = up/fwd) of every mesh vertex that samples it,
	// already margin-expanded and quantized. Invalid = no vertex lands on the
	// face at these params - capture the full face if it's captured at all.
	float m_flWantX0[FACE_COUNT] = {}, m_flWantX1[FACE_COUNT] = {};
	float m_flWantY0[FACE_COUNT] = {}, m_flWantY1[FACE_COUNT] = {};
	bool m_bWantValid[FACE_COUNT] = {};
	// Forces the next mesh build to recompute the wanted rects even when the
	// projection params are unchanged (set on Initialize: rig/size rebuilds
	// invalidate them without necessarily changing any param).
	bool m_bWantDirty = true;
	// Round-robin position over the non-keystone faces for the stagger schedule.
	int m_iStaggerCursor = 0;

	// Main-thread cost of the last frame's passes, for the FlexFOVDebug overlay
	// (the scene passes are CPU-bound on submission, so main-thread ms is the
	// number that matters). Per-face entries are 0 when the face was skipped.
	float m_flFaceMs[FACE_COUNT] = {};
	float m_flCaptureMs = 0.f;
	float m_flCompositeMs = 0.f;
	int m_nFacesCaptured = 0;

	// Extra overlay instrumentation. m_iCaptureFace names the face currently
	// being rendered (valid while m_bDrawing) so the DrawModelExecute samples
	// can be attributed per face - that's what shows whether stagger / cheap
	// periphery actually removed model draws, and which face is entity-heavy.
	int m_iCaptureFace = -1;
	int m_nFaceModelsFrame[FACE_COUNT] = {}; // model draws per face last frame
	int m_nW2SFrame = 0;         // composite WorldToScreen calls last frame
	bool m_bAutoStaggerFront = false; // fps-driven front-stagger engaged

	// Composite mesh stats: triangles per face bucket, dynamic-mesh chunks
	// drawn, the bucket-rebuild share of the composite cost this frame, and a
	// rebuild counter DrawDebug drains into a rebuilds/sec rate (per-frame
	// rebuilds while staggered are expected; per-frame rebuilds while standing
	// still mean a param is oscillating and the cache never hits).
	int m_nCompTris[FACE_COUNT] = {};
	int m_nCompChunks = 0;
	float m_flMeshBuildMs = 0.f;
	int m_nMeshRebuilds = 0;

	// Accumulate flMs (and a draw count for PROF_MODELS) into the bucket for
	// the currently active render context. Only records while FlexFOVDebug is
	// on - callers gate on that before timing.
	void AddStageMs(int iStage, float flMs, int nCount = 0);
	// Latches the accumulators into the m_*Frame snapshots and clears them.
	// Called once per frame at the top of the CViewRender_RenderView hook, so a
	// snapshot covers exactly one frame: main pass + captures + aux views.
	void SnapshotProfFrame();
	float m_flProfFrame[PROF_COUNT][CTX_COUNT] = {};
	int m_nProfModelsFrame[CTX_COUNT] = {};

	// Timings written by the CViewRender_RenderView / ViewDrawScene hooks:
	// the whole main-pass CALL_ORIGINAL (cheap or full), whether it ran as the
	// scene-stripped replacement, whether the ViewDrawScene hook cut the scene
	// out of it (vs the cvar-only fallback when the signature is missing), the
	// scene (ViewDrawScene) share of a non-replaced main pass, and the
	// post-composite viewmodel redraw.
	float m_flMainPassMs = 0.f;
	bool m_bMainPassCheap = false;
	bool m_bSceneSkipped = false;
	float m_flSceneMs = 0.f;
	float m_flViewmodelMs = 0.f;

	// Whether the face currently being captured (m_bDrawing) can possibly see a
	// point, with slack for the entity's own extent. Shared angular cull for the
	// per-face chams / glow model passes.
	bool FaceCanSee(const Vec3& vOrigin, float flExtent = 100.f);

	// Whether the wide-FOV pipeline is active this frame (needs the globe
	// captured). Set by CVisuals::FOV from the debug/composite toggles for now.
	bool m_bActive = false;

	// Draw the mesh composite (M-ladder) instead of / on top of the tiles.
	bool m_bComposite = false;

	// Per-frame snapshot of the exact projection inputs used to build the
	// composite this frame. WorldToScreen must read the identical values so ESP /
	// overlays stay pixel-aligned with the warp. Eye/angles are latched in
	// CaptureGlobe (from the real view setup); fov/aspect/strength in DrawComposite.
	Vec3 m_vEyeOrigin = {};
	Vec3 m_vViewAngles = {};
	// View basis derived from m_vViewAngles, cached once per frame in CaptureGlobe
	// so WorldToScreen (called hundreds+ times/frame) skips the per-call trig.
	Vec3 m_vViewFwd = { 0, 0, 1 };
	Vec3 m_vViewRight = { 1, 0, 0 };
	Vec3 m_vViewUp = { 0, 0, 1 };
	float m_flFovX = 140.f;
	float m_flAspect = 16.f / 9.f;
	float m_flStrength = 1.f;
	// 0 = base (rectilinear/Panini/Mercator tiers), 1 = pure radial projection
	// (stereographic at strength 1). The full-stereographic toggle pins 1; the
	// vertical-stereographic toggle blends with pitch, scaled by the fov-tier
	// envelope so it fades out as Mercator takes over at high fov.
	float m_flStereoBlend = 0.f;
	// Panini->Mercator transition band (FlexFOVTransition slider), snapshotted
	// alongside the rest so WorldToScreen matches the mesh warp exactly.
	float m_flTierLo = 160.f, m_flTierHi = 300.f;
};

ADD_FEATURE(CFlexFOV, FlexFOV);
