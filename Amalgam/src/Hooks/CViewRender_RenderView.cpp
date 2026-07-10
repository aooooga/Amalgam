#include "../SDK/SDK.h"

#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"
#include "../Features/Visuals/RearView/RearView.h"

MAKE_HOOK(CViewRender_RenderView, U::Memory.GetVirtual(I::ViewRender, 6), void,
	void* rcx, const CViewSetup& view, ClearFlags_t nClearFlags, RenderViewInfo_t whatToDraw)
{
	DEBUG_RETURN(CViewRender_RenderView, rcx, view, nClearFlags, whatToDraw);

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

	// When the FlexFOV composite owns the frame it repaints every pixel from the
	// captured faces, so the scene part of the main pass is pure wasted work.
	// The pass itself must still run - the in-game HUD paint (which draws the
	// composite) happens inside it - so instead of skipping CALL_ORIGINAL we run
	// it with the scene-draw cvars zeroed, making the redundant render near-free.
	if (F::FlexFOV.ShouldReplaceView())
	{
		F::FlexFOV.BeginCheapMainView();
		CALL_ORIGINAL(rcx, tView, nClearFlags, whatToDraw);
		F::FlexFOV.EndCheapMainView();
	}
	else
		CALL_ORIGINAL(rcx, tView, nClearFlags, whatToDraw);

	F::CameraWindow.RenderView(rcx, tView);
	F::FlexFOV.CaptureGlobe(rcx, tView);
	F::RearView.Capture(rcx, tView);
}