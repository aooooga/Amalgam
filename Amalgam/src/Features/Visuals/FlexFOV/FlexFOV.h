#pragma once
#include "../../../SDK/SDK.h"

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

private:
	// Face RT slots. The cube rig uses all 6; the wide rig (two tall faces yawed
	// left/right, used for fov <= ~245 on widescreen) uses slots 0..1 only.
	ITexture* m_pFaceTextures[FACE_COUNT] = {};
	IMaterial* m_pFaceMaterials[FACE_COUNT] = {};

	int m_iFaceW = 0, m_iFaceH = 0; // face RT resolution (cube: square, W == H)
	bool m_bWideRig = false;        // rig the current textures were built for

	void RenderFace(void* rcx, const CViewSetup& pViewSetup, int iFace, const Vec3& vAngles);
	void ComputeFaceAngles(const Vec3& vViewAngles, Vec3 vOut[FACE_COUNT]);
	void ComputeWideAngles(const Vec3& vViewAngles, float flYawDeg, Vec3 vOut[2]);

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
	// entirely skipped that paint too and froze the screen.)
	void BeginCheapMainView();
	void EndCheapMainView();

	// Blits the 6 captured faces as debug thumbnails over the current screen.
	// Called from IEngineVGui_Paint.
	void DrawDebug();

	// Mesh-ladder composite (currently M1: fullscreen FRONT-face quad).
	// Called from IEngineVGui_Paint.
	void DrawComposite();

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

	// True while the cheap (scene-stripped) main-view pass is running, so chams /
	// glow / screen effects can skip work the composite would paint over anyway.
	bool m_bReplacingView = false;

	// Which cube faces the current composite mesh actually samples (recorded when
	// the mesh is rebuilt). CaptureGlobe skips rendering the rest - e.g. BACK is
	// never sampled below ~180 fov, UP/DOWN often aren't at moderate fov.
	bool m_bFaceNeeded[FACE_COUNT] = { true, true, true, true, true, true };

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
};

ADD_FEATURE(CFlexFOV, FlexFOV);
