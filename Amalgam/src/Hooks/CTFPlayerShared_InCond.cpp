#include "../SDK/SDK.h"

MAKE_SIGNATURE(CTFPlayerShared_InCond, "client.dll", "48 89 5C 24 ? 57 48 83 EC ? 8B DA 48 8B F9 83 FA ? 7D", 0x0);
MAKE_SIGNATURE(CTFPlayer_ShouldDraw_InCond_Call, "client.dll", "84 C0 74 ? 32 C0 48 8B 74 24", 0x0);
MAKE_SIGNATURE(CTFWearable_ShouldDraw_InCond_Call, "client.dll", "84 C0 0F 85 ? ? ? ? 41 BF", 0x0);
MAKE_SIGNATURE(CHudScope_ShouldDraw_InCond_Call, "client.dll", "84 C0 74 ? 48 8B CB E8 ? ? ? ? 48 85 C0 74 ? 48 8B CB E8 ? ? ? ? 48 8B C8 48 8B 10 FF 92 ? ? ? ? 83 F8 ? 0F 94 C0", 0x0);
MAKE_SIGNATURE(CTFPlayer_CreateMove_InCondTaunt_Call, "client.dll", "84 C0 75 ? BA ? ? ? ? 48 8D 8E ? ? ? ? E8 ? ? ? ? 84 C0 75 ? 45 32 FF", 0x0);
MAKE_SIGNATURE(CTFPlayer_CreateMove_InCondKart_Call, "client.dll", "84 C0 74 ? 4C 8B C3", 0x0);
MAKE_SIGNATURE(CTFInput_ApplyMouse_InCond_Call, "client.dll", "84 C0 74 ? F3 0F 10 9B", 0x0);

MAKE_HOOK(CTFPlayerShared_InCond, S::CTFPlayerShared_InCond(), bool,
	void* rcx, ETFCond nCond)
{
	DEBUG_RETURN(CTFPlayerShared_InCond, rcx, nCond);

	// InCond is one of the hottest functions in client.dll (every weapon /
	// movement / HUD think checks conditions), so keep the fall-through path
	// lean: no entity-list lookups before the switch - only the two cases that
	// compare against the local player's shared pointer pay for GetLocal, and
	// only after their (rarely-enabled) toggles pass.
	const auto LocalShared = []() -> void*
	{
		auto pLocal = H::Entities.GetLocal();
		return pLocal ? pLocal->m_Shared() : nullptr;
	};

	switch (nCond)
	{
	case TF_COND_ZOOMED:
	{
		const auto dwRetAddr = uintptr_t(_ReturnAddress());
		if (dwRetAddr == S::CTFPlayer_ShouldDraw_InCond_Call() || dwRetAddr == S::CTFWearable_ShouldDraw_InCond_Call()
			|| dwRetAddr == S::CHudScope_ShouldDraw_InCond_Call() && Vars::Visuals::Removals::Scope.Value)
			return false;
		break;
	}
	case TF_COND_DISGUISED:
		if (Vars::Visuals::Removals::Disguises.Value && LocalShared() != rcx)
			return false;
		break;
	case TF_COND_TAUNTING:
		if (uintptr_t(_ReturnAddress()) == S::CTFPlayer_CreateMove_InCondTaunt_Call() && Vars::Misc::Automation::TauntControl.Value)
			return false;
		if (Vars::Visuals::Removals::Taunts.Value && LocalShared() != rcx)
			return false;
		break;
	case TF_COND_HALLOWEEN_KART:
	{
		const auto dwRetAddr = uintptr_t(_ReturnAddress());
		if ((dwRetAddr == S::CTFPlayer_CreateMove_InCondKart_Call() || dwRetAddr == S::CTFInput_ApplyMouse_InCond_Call())
			&& Vars::Misc::Automation::KartControl.Value)
			return false;
		break;
	}
	case TF_COND_FREEZE_INPUT:
		if (!CALL_ORIGINAL(rcx, TF_COND_HALLOWEEN_KART) || Vars::Misc::Automation::KartControl.Value)
			return false;
	}

	return CALL_ORIGINAL(rcx, nCond);
}