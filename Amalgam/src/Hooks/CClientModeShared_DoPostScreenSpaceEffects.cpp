#include "../SDK/SDK.h"

#include "../Features/Visuals/Chams/Chams.h"
#include "../Features/Visuals/Glow/Glow.h"
#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"
#include "../Features/Visuals/Visuals.h"
#include "../Features/Visuals/Materials/Materials.h"
#include "../Features/Spectate/Spectate.h"
#include "../Features/Aimbot/DoubleSticky/DoubleSticky.h"

MAKE_SIGNATURE(CViewRender_DrawViewModels, "client.dll", "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 8B FA", 0x0);

MAKE_HOOK(CClientModeShared_DoPostScreenSpaceEffects, U::Memory.GetVirtual(I::ClientModeShared, 39), bool,
	void* rcx, const CViewSetup* pSetup)
{
	DEBUG_RETURN(CClientModeShared_DoPostScreenSpaceEffects, rcx, pSetup);

	if (SDK::CleanScreenshot() || G::Unload)
		return CALL_ORIGINAL(rcx, pSetup);
	
	F::Visuals.ProjectileTrace(H::Entities.GetLocal(), H::Entities.GetWeapon());
	// Before the FlexFOV early-returns (like ProjectileTrace) so the arc also renders
	// into the capture faces and survives the composite.
	F::DoubleSticky.Draw();
	F::Visuals.DrawStickyRadius();
	// Cheap (scene-stripped) main pass: the composite paints over everything this
	// pass produces, so chams / glow / effects here are pure wasted work.
	if (F::CameraWindow.m_bDrawing || F::FlexFOV.m_bReplacingView)
		return CALL_ORIGINAL(rcx, pSetup);

	// FlexFOV face capture: render chams and glow outlines into the cube faces
	// so they survive the composite (otherwise they only exist in the covered
	// main view). Glow uses its face-sized flex buffers here.
	if (F::FlexFOV.m_bDrawing)
	{
		if (!I::EngineVGui->IsGameUIVisible() && F::Materials.m_bLoaded)
		{
			F::Chams.RenderMain();
			F::Glow.RenderOnFlexFace();
		}
		return CALL_ORIGINAL(rcx, pSetup);
	}

	F::Visuals.DrawEffects();
	if (I::EngineVGui->IsGameUIVisible() || !F::Materials.m_bLoaded)
		return CALL_ORIGINAL(rcx, pSetup);

	F::Chams.RenderMain();
	F::Glow.RenderFirst();
	return CALL_ORIGINAL(rcx, pSetup);
}

MAKE_HOOK(CViewRender_DrawViewModels, S::CViewRender_DrawViewModels(), void,
	void* rcx, const CViewSetup& viewRender, bool drawViewmodel)
{
	DEBUG_RETURN(CViewRender_DrawViewModels, rcx, viewRender, drawViewmodel);

	CALL_ORIGINAL(rcx, viewRender, F::Spectate.HasTarget() && !I::EngineClient->IsHLTV() ? false : drawViewmodel);
	if (SDK::CleanScreenshot() || F::CameraWindow.m_bDrawing || F::FlexFOV.m_bDrawing || F::FlexFOV.m_bReplacingView || I::EngineVGui->IsGameUIVisible() || !F::Materials.m_bLoaded)
		return;

	F::Glow.RenderSecond();
}