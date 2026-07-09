#include "../SDK/SDK.h"

#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"

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

	// When the FlexFOV composite owns the frame it repaints every pixel from the
	// 6 captured faces, so the normal main-view pass is pure wasted work - skip it.
	// (If the composite can't fully render this frame, ShouldReplaceView() is false
	// and we fall back to the normal render so the screen is never left blank.)
	const bool bReplace = F::FlexFOV.ShouldReplaceView();
	if (!bReplace)
		CALL_ORIGINAL(rcx, view, nClearFlags, whatToDraw);

	F::CameraWindow.RenderView(rcx, view);
	F::FlexFOV.CaptureGlobe(rcx, view);
}