#include "../SDK/SDK.h"

#include "../Features/Visuals/Chams/Chams.h"
#include "../Features/Visuals/Glow/Glow.h"
#include "../Features/Aimbot/TrajectoryGhost/TrajectoryGhost.h"
#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"
#include "../Features/Visuals/Visuals.h"
#include "../Features/Visuals/SentryRange/SentryRange.h"
#include "../Features/Visuals/Materials/Materials.h"
#include "../Features/Spectate/Spectate.h"
#include "../Features/Aimbot/DoubleSticky/DoubleSticky.h"
#include "../Utils/Perf/Tracker.h"

MAKE_SIGNATURE(CViewRender_DrawViewModels, "client.dll", "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 8B FA", 0x0);

MAKE_HOOK(CClientModeShared_DoPostScreenSpaceEffects, U::Memory.GetVirtual(I::ClientModeShared, 39), bool,
	void* rcx, const CViewSetup* pSetup)
{
	DEBUG_RETURN(CClientModeShared_DoPostScreenSpaceEffects, rcx, pSetup);

	if (SDK::CleanScreenshot() || G::Unload)
		return CALL_ORIGINAL(rcx, pSetup);
	
	// The in-scene draw pass. It re-enters once per scene pass (main view, each
	// FlexFOV face, the rear-view flanks), so calls/f here is the pass count -
	// the number that multiplies every cost below it.
	PROF_ZONE("PostScreenSpace (all)", Perf::GROUP_SCENE);

	PROF_CALL("Materials::DrainRetired", Perf::GROUP_SCENE, F::Materials.DrainRetired());
	PROF_CALL("Visuals::ProjectileTrace", Perf::GROUP_SCENE, F::Visuals.ProjectileTrace(H::Entities.GetLocal(), H::Entities.GetWeapon()));
	// Before the FlexFOV early-returns (like ProjectileTrace) so the arc also renders
	// into the capture faces and survives the composite. The scene-stripped
	// (replaced) main pass is the exception: the composite paints over it, so
	// its draws are skipped (ProjectileTrace / DrawStickyRadius do so
	// internally, after refreshing their per-frame caches).
	if (!F::FlexFOV.m_bReplacingView)
		PROF_CALL("DoubleSticky::Draw", Perf::GROUP_SCENE, F::DoubleSticky.Draw());
	PROF_CALL("Visuals::StickyRadius", Perf::GROUP_SCENE, F::Visuals.DrawStickyRadius());
	PROF_CALL("Visuals::HealRadius", Perf::GROUP_SCENE, F::Visuals.DrawHealRadius());
	PROF_CALL("SentryRange::Draw", Perf::GROUP_SCENE, F::SentryRange.Draw());
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
			PROF_CALL("Chams::RenderMain", Perf::GROUP_SCENE, F::Chams.RenderMain());
			PROF_CALL("TrajectoryGhost::RenderMain", Perf::GROUP_SCENE, F::TrajectoryGhost.RenderMain());
			PROF_CALL("Glow::RenderOnFlexFace", Perf::GROUP_SCENE, F::Glow.RenderOnFlexFace());
		}
		return CALL_ORIGINAL(rcx, pSetup);
	}

	PROF_CALL("Visuals::DrawEffects", Perf::GROUP_SCENE, F::Visuals.DrawEffects());
	if (I::EngineVGui->IsGameUIVisible() || !F::Materials.m_bLoaded)
		return CALL_ORIGINAL(rcx, pSetup);

	PROF_CALL("Chams::RenderMain", Perf::GROUP_SCENE, F::Chams.RenderMain());
	PROF_CALL("TrajectoryGhost::RenderMain", Perf::GROUP_SCENE, F::TrajectoryGhost.RenderMain());
	PROF_CALL("Glow::RenderFirst", Perf::GROUP_SCENE, F::Glow.RenderFirst());
	return CALL_ORIGINAL(rcx, pSetup);
}

MAKE_HOOK(CViewRender_DrawViewModels, S::CViewRender_DrawViewModels(), void,
	void* rcx, const CViewSetup& viewRender, bool drawViewmodel)
{
	DEBUG_RETURN(CViewRender_DrawViewModels, rcx, viewRender, drawViewmodel);

	CALL_ORIGINAL(rcx, viewRender, F::Spectate.HasTarget() && !I::EngineClient->IsHLTV() ? false : drawViewmodel);
	if (SDK::CleanScreenshot() || F::CameraWindow.m_bDrawing || F::FlexFOV.m_bDrawing || F::FlexFOV.m_bReplacingView || I::EngineVGui->IsGameUIVisible() || !F::Materials.m_bLoaded)
		return;

	PROF_CALL("Glow::RenderSecond", Perf::GROUP_SCENE, F::Glow.RenderSecond());
}