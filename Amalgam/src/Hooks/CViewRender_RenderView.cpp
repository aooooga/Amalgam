#include "../SDK/SDK.h"

#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/Chams/Chams.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"
#include "../Features/Visuals/RearView/RearView.h"
#include "../Utils/Perf/Tracker.h"

// CViewRender::ViewDrawScene - the 3D scene portion of RenderView. Located via
// its VPROF node string ("CViewRender::ViewDrawScene", single xref).
MAKE_SIGNATURE(CViewRender_ViewDrawScene, "client.dll", "4C 8B DC 49 89 5B ? 55 56 57 41 54 41 55 41 56 41 57 48 83 EC ? 48 8B 35 ? ? ? ? 45 33 FF 8B 9C 24", 0x0);

// The scene-stripped (replaced) main pass zeroes the draw cvars, but those only
// gate the *draw* half of the scene: the engine still pays the CPU-side setup
// every frame - BuildWorldLists BSP traversal, renderable list building over
// the PVS, water reflection/refraction view setup - because all of it runs in
// ViewDrawScene before any r_draw* check. Since the composite repaints every
// pixel anyway, cut the whole scene off here. Everything RenderView does around
// it (HUD paint, which draws the composite) still runs.
//
// Face captures (m_bDrawing) call RenderView's original directly and land here
// with m_bReplacingView false, so they render normally.
MAKE_HOOK(CViewRender_ViewDrawScene, S::CViewRender_ViewDrawScene(), void,
	void* rcx, bool bDrew3dSkybox, int nSkyboxVisible, const CViewSetup& view, int nClearFlags, int viewID, bool bDrawViewModel, int baseDrawFlags, void* pCustomVisibility)
{
	DEBUG_RETURN(CViewRender_ViewDrawScene, rcx, bDrew3dSkybox, nSkyboxVisible, view, nClearFlags, viewID, bDrawViewModel, baseDrawFlags, pCustomVisibility);

	if (F::FlexFOV.m_bReplacingView)
	{
		F::FlexFOV.m_bSceneSkipped = true;
		return;
	}

	// Plain-Original chams passthrough: entities registered for the stencil
	// wrap mark this scene's stencil as they draw, and the occluded cham pass
	// (post-scene) tests those marks - so the buffer must start the scene
	// clean. RenderMain skips its own pre-clear while these marks are live.
	if (F::Chams.m_bScenePassthrough)
	{
		if (auto pRenderContext = I::MaterialSystem->GetRenderContext())
			pRenderContext->ClearBuffers(false, false, true);
	}

	const double flStart = SDK::PlatFloatTime();
	CALL_ORIGINAL(rcx, bDrew3dSkybox, nSkyboxVisible, view, nClearFlags, viewID, bDrawViewModel, baseDrawFlags, pCustomVisibility);
	const float flMs = float((SDK::PlatFloatTime() - flStart) * 1000.0);
	// Attribute only the real main pass: face captures are timed per-face in
	// CaptureGlobe, and the camera-window / rear-view captures re-enter here
	// too. Accumulated (not assigned) because RTT monitors re-enter per frame.
	if (F::FlexFOV.m_bActive && !F::FlexFOV.m_bDrawing
		&& !F::CameraWindow.m_bDrawing && !F::RearView.m_bCapturing)
		F::FlexFOV.m_flSceneMs += flMs;
	if (Vars::Visuals::UI::FlexFOVDebug.Value)
		F::FlexFOV.AddStageMs(CFlexFOV::PROF_SCENE, flMs);
}

// Scene-stage profiling for the FlexFOVDebug overlay (the "mini-vprof"): each
// hook times its stage into F::FlexFOV's per-context buckets. All three were
// located via their VPROF strings, like ViewDrawScene above. Zero overhead
// with the debug toggle off.

// CSimpleWorldView::Draw - identified by its inlined BuildWorldRenderLists /
// BuildRenderableRenderLists / DrawOpaqueRenderables VPROF scopes, so this one
// bucket covers list building + world + opaque renderables. Takes only `this`;
// the extra register args are forwarded defensively and ignored by the callee.
MAKE_SIGNATURE(CSimpleWorldView_Draw, "client.dll", "4C 8B DC 49 89 5B 20 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60 48 8B 05 ? ? ? ? 48 8D 1D ? ? ? ? 45 33 ED 48 8D 15 ? ? ? ? 4D 89 6B 10", 0x0);
MAKE_HOOK(CSimpleWorldView_Draw, S::CSimpleWorldView_Draw(), void,
	void* rcx, void* rdx, void* r8, void* r9)
{
	DEBUG_RETURN(CSimpleWorldView_Draw, rcx, rdx, r8, r9);

	if (!Vars::Visuals::UI::FlexFOVDebug.Value)
		return CALL_ORIGINAL(rcx, rdx, r8, r9);

	const double flStart = SDK::PlatFloatTime();
	CALL_ORIGINAL(rcx, rdx, r8, r9);
	F::FlexFOV.AddStageMs(CFlexFOV::PROF_WORLD, float((SDK::PlatFloatTime() - flStart) * 1000.0));
}

// CRendering3dView::DrawTranslucentRenderables(bool bInSkybox, bool bShadowDepth)
MAKE_SIGNATURE(CRendering3dView_DrawTranslucentRenderables, "client.dll", "44 88 44 24 ? 48 89 4C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8D 6C 24 ? 83 3D", 0x0);
MAKE_HOOK(CRendering3dView_DrawTranslucentRenderables, S::CRendering3dView_DrawTranslucentRenderables(), void,
	void* rcx, bool bInSkybox, bool bShadowDepth)
{
	DEBUG_RETURN(CRendering3dView_DrawTranslucentRenderables, rcx, bInSkybox, bShadowDepth);

	if (!Vars::Visuals::UI::FlexFOVDebug.Value)
		return CALL_ORIGINAL(rcx, bInSkybox, bShadowDepth);

	const double flStart = SDK::PlatFloatTime();
	CALL_ORIGINAL(rcx, bInSkybox, bShadowDepth);
	F::FlexFOV.AddStageMs(CFlexFOV::PROF_TRANSLUCENT, float((SDK::PlatFloatTime() - flStart) * 1000.0));
}

// The 3d-skybox view draw (holds the "CViewRender::Draw3dSkyboxworld" VPROF
// scope). Register args forwarded defensively, as above.
MAKE_SIGNATURE(CSkyboxView_Draw, "client.dll", "4C 8B DC 49 89 5B 10 49 89 6B 18 49 89 73 20 57 41 54 41 56 48 83 EC 60 48 8B 1D ? ? ? ? 4C 8D 35", 0x0);
MAKE_HOOK(CSkyboxView_Draw, S::CSkyboxView_Draw(), void,
	void* rcx, void* rdx, void* r8, void* r9)
{
	DEBUG_RETURN(CSkyboxView_Draw, rcx, rdx, r8, r9);

	if (!Vars::Visuals::UI::FlexFOVDebug.Value)
		return CALL_ORIGINAL(rcx, rdx, r8, r9);

	const double flStart = SDK::PlatFloatTime();
	CALL_ORIGINAL(rcx, rdx, r8, r9);
	F::FlexFOV.AddStageMs(CFlexFOV::PROF_SKY3D, float((SDK::PlatFloatTime() - flStart) * 1000.0));
}

MAKE_HOOK(CViewRender_RenderView, U::Memory.GetVirtual(I::ViewRender, 6), void,
	void* rcx, const CViewSetup& view, ClearFlags_t nClearFlags, RenderViewInfo_t whatToDraw)
{
	DEBUG_RETURN(CViewRender_RenderView, rcx, view, nClearFlags, whatToDraw);

	// Frame boundary for the mini-vprof buckets: everything between two entries
	// here (main pass + face captures + aux views) is one frame's worth.
	F::FlexFOV.SnapshotProfFrame();

	// Same boundary for the process tracker. RenderView is the one call
	// guaranteed once per rendered frame in game, so a frame's worth of tracker
	// data is [this entry, the next) - it carries the previous frame's render
	// plus this frame's CreateMove/net-update work, which is exactly one frame's
	// cost either way. Enabling here means the toggle takes effect from the next
	// zone onward, never mid-frame.
	Perf::Tracker.SetEnabled(Vars::Debug::ProcessTracker.Value || Vars::Debug::AutoVprof.Value);
	Perf::Tracker.EndFrame();

	// A clean screenshot (and unload) must always render the plain view with no
	// composite/overlays; do that and bail before any capture work.
	if (SDK::CleanScreenshot() || G::Unload)
	{
		CALL_ORIGINAL(rcx, view, nClearFlags, whatToDraw);
		return;
	}

	// Viewmodel fov override (independent of FlexFOV): DrawViewModels builds the
	// viewmodel projection from view.fovViewmodel, so patching it here covers the
	// normal pass, and FlexFOV's post-composite redraw latches this same setup.
	CViewSetup tView = view;
	if (Vars::Visuals::UI::ViewmodelFOV.Value)
		tView.fovViewmodel = Vars::Visuals::UI::ViewmodelFOV.Value;

	// Latch THIS frame's setup for the post-composite viewmodel redraw (the HUD
	// paint that runs it happens inside CALL_ORIGINAL below). The viewmodel is
	// screen-anchored and its bones are set up from this frame's eye, so it must
	// render with this frame's view too.
	F::FlexFOV.m_tViewSetup = tView;
	F::FlexFOV.m_bViewSetupValid = true;

	// When the FlexFOV composite owns the frame it repaints every pixel from the
	// captured faces, so the scene part of the main pass is pure wasted work.
	// The pass itself must still run - the in-game HUD paint (which draws the
	// composite) happens inside it - so instead of skipping CALL_ORIGINAL we run
	// it with the scene-draw cvars zeroed (and the ViewDrawScene hook cutting
	// the scene's CPU-side setup out entirely), making the redundant render
	// near-free.
	// Capture BEFORE the main pass. The composite is drawn from the HUD paint,
	// which runs *inside* the main pass' CALL_ORIGINAL below - so capturing after
	// it meant every composited frame was built from faces (and an eye/angle
	// snapshot) captured at the end of the PREVIOUS frame. That is a full extra
	// frame of latency on the whole world view: mouse motion showed up a frame
	// late, on top of whatever the engine already owes. Capturing first makes the
	// faces, the composite, W2S and the viewmodel all this frame's.
	// The extra scene passes. Their inclusive time is mostly engine rendering we
	// asked for, so read inclms here against vprof's scene nodes.
	PROF_CALL("FlexFOV::CaptureGlobe", Perf::GROUP_SCENE, F::FlexFOV.CaptureGlobe(rcx, tView));

	F::FlexFOV.m_flSceneMs = 0.f;
	F::FlexFOV.m_bSceneSkipped = false;
	const double flMainStart = SDK::PlatFloatTime();
	if (F::FlexFOV.ShouldReplaceView())
	{
		F::FlexFOV.BeginCheapMainView();
		// The scene-stripped pass leaves nothing for bloom/tonemap to sample;
		// skip that chain too (the composite repaints every pixel from the face
		// captures, which carry their own consistent exposure).
		CViewSetup tCheap = tView;
		tCheap.m_bDoBloomAndToneMapping = false;
		CALL_ORIGINAL(rcx, tCheap, nClearFlags, whatToDraw);
		F::FlexFOV.EndCheapMainView();
		F::FlexFOV.m_bMainPassCheap = true;
	}
	else
	{
		CALL_ORIGINAL(rcx, tView, nClearFlags, whatToDraw);
		F::FlexFOV.m_bMainPassCheap = false;
	}
	F::FlexFOV.m_flMainPassMs = float((SDK::PlatFloatTime() - flMainStart) * 1000.0);

	PROF_CALL("CameraWindow::RenderView", Perf::GROUP_SCENE, F::CameraWindow.RenderView(rcx, tView));
	PROF_CALL("RearView::Capture", Perf::GROUP_SCENE, F::RearView.Capture(rcx, tView));
}