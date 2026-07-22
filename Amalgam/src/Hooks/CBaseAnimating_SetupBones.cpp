#include "../SDK/SDK.h"

#include "../Features/Backtrack/Backtrack.h"

MAKE_SIGNATURE(CBaseAnimating_SetupBones, "client.dll", "48 8B C4 44 89 40 ? 48 89 50 ? 55 53", 0x0);

MAKE_HOOK(CBaseAnimating_SetupBones, S::CBaseAnimating_SetupBones(), bool,
	void* rcx, matrix3x4* pBoneToWorldOut, int nMaxBones, int boneMask, float currentTime)
{
	DEBUG_RETURN(CBaseAnimating_SetupBones, rcx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	if (!Vars::Misc::Game::SetupBonesOptimization.Value || F::Backtrack.IsSettingUpBones())
		return CALL_ORIGINAL(rcx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	auto pAnimating = reinterpret_cast<CBaseEntity*>(uintptr_t(rcx) - 8);
	if (!pAnimating)
		return CALL_ORIGINAL(rcx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	auto pOwner = pAnimating->GetRootMoveParent();
	auto pEntity = pOwner ? pOwner : pAnimating;
	if (!pEntity->IsPlayer() || pEntity->entindex() == I::EngineClient->GetLocalPlayer())
		return CALL_ORIGINAL(rcx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	auto pAnimatingEntity = pEntity->As<CBaseAnimating>();

	// Freshness gate: only serve the cache when the engine actually recomputed
	// this entity's bones on THIS frame (it stamps m_flLastBoneSetupTime with
	// curtime whenever it does). Without this, a redraw of a player the engine
	// hasn't drawn yet this frame would copy last frame's pose - visible bone
	// lag/ghosting on chams/glow, which is why the optimization was previously
	// left off by default. On a stale/missing cache, fall through to the real
	// SetupBones so correctness is preserved rather than serving old bones.
	if (pAnimatingEntity->m_flLastBoneSetupTime() != I::GlobalVars->curtime)
		return CALL_ORIGINAL(rcx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);

	if (pBoneToWorldOut)
	{
		auto& aBones = pAnimatingEntity->m_CachedBoneData();
		if (aBones.Count() < 1 || nMaxBones < aBones.Count())
			return CALL_ORIGINAL(rcx, pBoneToWorldOut, nMaxBones, boneMask, currentTime);
		memcpy(pBoneToWorldOut, aBones.Base(), sizeof(matrix3x4) * aBones.Count());
	}

	return true;
}