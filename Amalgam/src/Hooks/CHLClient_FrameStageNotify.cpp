#include "../SDK/SDK.h"

#include "../Features/Aimbot/Aimbot.h"
#include "../Features/Backtrack/Backtrack.h"
#include "../Features/Binds/Binds.h"
#include "../Features/CheatDetection/CheatDetection.h"
#include "../Features/CritHack/CritHack.h"
#include "../Features/Players/PlayerUtils.h"
#include "../Features/Simulation/MovementSimulation/MovementSimulation.h"
#include "../Features/Spectate/Spectate.h"
#include "../Features/Visuals/Visuals.h"
#include "../Features/Visuals/ESP/ESP.h"
#include "../Features/Visuals/Chams/Chams.h"
#include "../Features/Visuals/Glow/Glow.h"
#include "../Features/Visuals/Groups/Groups.h"
#include "../Features/Visuals/OffscreenArrows/OffscreenArrows.h"
#include "../Utils/Perf/Tracker.h"

MAKE_HOOK(CHLClient_FrameStageNotify, U::Memory.GetVirtual(I::Client, 35), void,
	void* rcx, ClientFrameStage_t curStage)
{
	DEBUG_RETURN(CHLClient_FrameStageNotify, rcx, curStage);

	if (G::Unload)
		return CALL_ORIGINAL(rcx, curStage);

	CALL_ORIGINAL(rcx, curStage);

	switch (curStage)
	{
	case FRAME_NET_UPDATE_START:
	{
		PROF_ZONE("NetUpdateStart (all)", Perf::GROUP_NETUPDATE);

		auto pLocal = H::Entities.GetLocal();
		F::Spectate.NetUpdateStart(pLocal);

		H::Entities.Clear();
		break;
	}
	case FRAME_NET_UPDATE_END:
	{
		// The per-net-update rebuild - every feature's Store(). This runs at the
		// server tick rate, not the frame rate, so a zone here that looks cheap
		// per frame can still be expensive per call (watch peakms/calls).
		PROF_ZONE("NetUpdateEnd (all)", Perf::GROUP_NETUPDATE);

		PROF_CALL("Entities::Store", Perf::GROUP_NETUPDATE, H::Entities.Store());
		PROF_CALL("PlayerUtils::Store", Perf::GROUP_NETUPDATE, F::PlayerUtils.Store());

		PROF_CALL("Backtrack::Store", Perf::GROUP_NETUPDATE, F::Backtrack.Store());
		PROF_CALL("MoveSim::Store", Perf::GROUP_NETUPDATE, F::MoveSim.Store());
		PROF_CALL("CritHack::Store", Perf::GROUP_NETUPDATE, F::CritHack.Store());
		PROF_CALL("Aimbot::Store", Perf::GROUP_NETUPDATE, F::Aimbot.Store());

		auto pLocal = H::Entities.GetLocal();
		PROF_CALL("Groups::Store", Perf::GROUP_NETUPDATE, F::Groups.Store(pLocal));
		PROF_CALL("ESP::Store", Perf::GROUP_NETUPDATE, F::ESP.Store(pLocal));
		PROF_CALL("Chams::Store", Perf::GROUP_NETUPDATE, F::Chams.Store(pLocal));
		PROF_CALL("Glow::Store", Perf::GROUP_NETUPDATE, F::Glow.Store(pLocal));
		PROF_CALL("OffscreenArrows::Store", Perf::GROUP_NETUPDATE, F::OffscreenArrows.Store());
		PROF_CALL("Visuals::Store", Perf::GROUP_NETUPDATE, F::Visuals.Store());

		PROF_CALL("CheatDetection::Run", Perf::GROUP_NETUPDATE, F::CheatDetection.Run());
		PROF_CALL("Spectate::NetUpdateEnd", Perf::GROUP_NETUPDATE, F::Spectate.NetUpdateEnd(pLocal));

		PROF_CALL("Visuals::Modulate", Perf::GROUP_NETUPDATE, F::Visuals.Modulate());
		PROF_CALL("Visuals::DrawHitboxes", Perf::GROUP_NETUPDATE, F::Visuals.DrawHitboxes(1));
		break;
	}
	case FRAME_RENDER_START:
	{
		// Post-interpolation, pre-scene: the crosshair-target trace hits the
		// hitbox pose this frame renders, and the targeted-chams suppression
		// set updates before the scene consumes it (see CChams::UpdateTarget).
		PROF_ZONE("RenderStart (all)", Perf::GROUP_NETUPDATE);
		PROF_CALL("Chams::UpdateTarget", Perf::GROUP_NETUPDATE, F::Chams.UpdateTarget());

		for (auto& tBind : F::Binds.m_vBinds)
		{	// don't drop inputs for binds
			if (tBind.m_iType != BindEnum::Key)
				continue;

			auto& tKey = tBind.m_tKeyStorage;

			bool bOldIsDown = tKey.m_bIsDown;
			bool bOldIsPressed = tKey.m_bIsPressed;
			bool bOldIsDouble = tKey.m_bIsDouble;
			bool bOldIsReleased = tKey.m_bIsReleased;

			U::KeyHandler.StoreKey(tBind.m_iKey, &tKey);

			tKey.m_bIsDown = tKey.m_bIsDown || bOldIsDown;
			tKey.m_bIsPressed = tKey.m_bIsPressed || bOldIsPressed;
			tKey.m_bIsDouble = tKey.m_bIsDouble || bOldIsDouble;
			tKey.m_bIsReleased = tKey.m_bIsReleased || bOldIsReleased;
		}
		break;
	}
	}
}