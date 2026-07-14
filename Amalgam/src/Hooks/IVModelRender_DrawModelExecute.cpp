#include "../SDK/SDK.h"

#include "../Features/Visuals/Chams/Chams.h"
#include "../Features/Visuals/Glow/Glow.h"
#include "../Features/Visuals/Materials/Materials.h"
#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"

MAKE_SIGNATURE(CBaseAnimating_InternalDrawModel, "client.dll", "48 8B C4 55 56 48 8D 6C 24 ? 48 81 EC ? ? ? ? 44 8B 81", 0x0);
MAKE_SIGNATURE(CBaseViewModel_DrawModel, "client.dll", "40 53 55 56 48 83 EC ? 80 B9", 0x0);

static bool s_bDrawingViewmodel = false;

MAKE_HOOK(IVModelRender_DrawModelExecute, U::Memory.GetVirtual(I::ModelRender, 19), void,
	void* rcx, const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	DEBUG_RETURN(IVModelRender_DrawModelExecute, rcx, pState, pInfo, pBoneToWorld);

	// Mini-vprof "models" bucket for the FlexFOVDebug overlay: one sample per
	// model draw, including the chams/glow handling below (that IS per-model
	// cost). RAII so every early return still records; free with debug off.
	struct SModelTimer
	{
		bool m_bOn; double m_flStart;
		SModelTimer() : m_bOn(Vars::Visuals::UI::FlexFOVDebug.Value), m_flStart(m_bOn ? SDK::PlatFloatTime() : 0.0) {}
		~SModelTimer() { if (m_bOn) F::FlexFOV.AddStageMs(CFlexFOV::PROF_MODELS, float((SDK::PlatFloatTime() - m_flStart) * 1000.0), 1); }
	} tModelTimer;

	// Note: FlexFOV face captures (m_bDrawing) intentionally go through the full
	// chams path below, so cham'd models are replaced in the cube faces exactly
	// like in the main view (the composite covers the main view's chams).
	if (I::EngineVGui->IsGameUIVisible() || SDK::CleanScreenshot()
		|| F::CameraWindow.m_bDrawing || F::FlexFOV.m_bReplacingView || !F::Materials.m_bLoaded || G::Unload)
		return CALL_ORIGINAL(rcx, pState, pInfo, pBoneToWorld);

	// FlexFOV cheap peripheral face: cosmetics are unreadable in the warped
	// periphery and each is a full model draw with bone merging - skip them
	// outright. Small visual-only entities (projectiles, pickups, dropped and
	// held weapons, ragdolls) are culled past 1500u, where the peripheral
	// compression leaves them sub-pixel anyway. Players, buildings and
	// objectives always draw.
	if (F::FlexFOV.m_bCheapFace)
	{
		auto pEntity = I::ClientEntityList->GetClientEntity(pInfo.entity_index)->As<CBaseEntity>();
		if (pEntity)
		{
			if (pEntity->IsWearable())
				return;
			if ((pEntity->IsProjectile() || pEntity->IsPickup() || pEntity->IsBaseCombatWeapon()
				|| pEntity->GetClassID() == ETFClassID::CTFRagdoll || pEntity->GetClassID() == ETFClassID::CTFDroppedWeapon)
				&& pInfo.origin.DistToSqr(F::FlexFOV.m_vEyeOrigin) > 1500.f * 1500.f)
				return;
		}
	}

	if (F::Chams.m_bRendering)
		return F::Chams.RenderHandler(pState, pInfo, pBoneToWorld);
	if (F::Glow.m_bRendering)
		return F::Glow.RenderHandler(pState, pInfo, pBoneToWorld);

	if (F::Chams.m_mEntities.contains(pInfo.entity_index))
		return;

	auto pEntity = I::ClientEntityList->GetClientEntity(pInfo.entity_index)->As<CBaseEntity>();
	if (pEntity && pEntity->IsWearableVM() /*pEntity->IsViewmodel()*/)
	{
		F::Glow.RenderViewmodel(pState, pInfo, pBoneToWorld);
		if (F::Chams.RenderViewmodel(pState, pInfo, pBoneToWorld))
			return;
	}

	CALL_ORIGINAL(rcx, pState, pInfo, pBoneToWorld);
}

MAKE_HOOK(CBaseAnimating_InternalDrawModel, S::CBaseAnimating_InternalDrawModel(), int,
	void* rcx, int flags)
{
	DEBUG_RETURN(CBaseAnimating_InternalDrawModel, rcx, flags);

	if (!s_bDrawingViewmodel /*|| !(flags & STUDIO_RENDER)*/)
		return CALL_ORIGINAL(rcx, flags);

	int iReturn;
	F::Glow.RenderViewmodel(rcx, flags);
	if (F::Chams.RenderViewmodel(rcx, flags, &iReturn))
		return iReturn;

	return CALL_ORIGINAL(rcx, 1);
}

MAKE_HOOK(CBaseViewModel_DrawModel, S::CBaseViewModel_DrawModel(), int,
	void* rcx, int flags)
{
	DEBUG_RETURN(CBaseAnimating_DrawModel, rcx, flags);

	if (s_bDrawingViewmodel || I::EngineVGui->IsGameUIVisible() || SDK::CleanScreenshot()
		|| F::CameraWindow.m_bDrawing || F::FlexFOV.m_bDrawing || !F::Materials.m_bLoaded || G::Unload)
		return CALL_ORIGINAL(rcx, flags);

	s_bDrawingViewmodel = true;
	int iReturn = CALL_ORIGINAL(rcx, flags);
	s_bDrawingViewmodel = false;
	return iReturn;
}