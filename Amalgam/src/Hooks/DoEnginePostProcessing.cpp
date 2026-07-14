#include "../SDK/SDK.h"

#include "../Features/Visuals/FlexFOV/FlexFOV.h"

MAKE_SIGNATURE(DoEnginePostProcessing, "client.dll", "48 8B C4 44 89 48 ? 44 89 40 ? 89 50 ? 89 48", 0x0);

MAKE_HOOK(DoEnginePostProcessing, S::DoEnginePostProcessing(), void,
	int x, int y, int w, int h, bool bFlashlightIsOn, bool bPostVGui)
{
	DEBUG_RETURN(DoEnginePostProcessing, x, y, w, h, bFlashlightIsOn, bPostVGui);

	if (SDK::CleanScreenshot())
		return CALL_ORIGINAL(x, y, w, h, bFlashlightIsOn, bPostVGui);

	// FlexFOV face captures / the scene-stripped main pass: bloom+tonemap per
	// scene pass is pure wasted work there (the composite is built from the
	// faces, which skip it uniformly), independent of the removals toggle.
	if (F::FlexFOV.m_bDrawing || F::FlexFOV.m_bReplacingView)
		return;

	if (!Vars::Visuals::Removals::PostProcessing.Value)
		CALL_ORIGINAL(x, y, w, h, bFlashlightIsOn, bPostVGui);
}