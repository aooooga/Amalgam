#include "../SDK/SDK.h"

#include "../Features/Aimbot/Aimbot.h"
#include "../Features/Backtrack/Backtrack.h"
#include "../Features/CritHack/CritHack.h"
#include "../Features/EnginePrediction/EnginePrediction.h"
#include "../Features/Misc/Misc.h"
#include "../Features/NoSpread/NoSpread.h"
#include "../Features/NoSpread/NoSpreadHitscan/NoSpreadHitscan.h"
#include "../Features/PacketManip/PacketManip.h"
#include "../Features/Resolver/Resolver.h"
#include "../Features/Ticks/Ticks.h"
#include "../Features/Visuals/Visuals.h"
#include "../Features/Visuals/FakeAngle/FakeAngle.h"
#include "../Features/Aimbot/TrajectoryGhost/TrajectoryGhost.h"
#include "../Features/Spectate/Spectate.h"
#include "../Features/AntiCheatCompatibility/AntiCheatCompatibility.h"
#include "../Utils/Perf/Tracker.h"

static no_inline void UpdateInfo(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	G::PSilentAngles = G::SilentAngles = G::Attacking = G::Throwing = false;
	G::LastUserCmd = G::CurrentUserCmd ? G::CurrentUserCmd : pCmd;
	G::CurrentUserCmd = pCmd;
	G::OriginalCmd = *pCmd;

	if (!pWeapon)
		return;

	SDK::CanAttack(pLocal, pWeapon, pCmd, G::CanPrimaryAttack, G::CanSecondaryAttack, G::Reloading);
	G::Attacking = SDK::IsAttacking(pLocal, pWeapon, pCmd);
	G::PrimaryWeaponType = SDK::GetWeaponType(pWeapon, &G::SecondaryWeaponType);
	G::CanHeadshot = pWeapon->CanHeadshot() || pWeapon->AmbassadorCanHeadshot(TICKS_TO_TIME(pLocal->m_nTickBase()));
}

MAKE_HOOK(CHLClient_CreateMove, U::Memory.GetVirtual(I::Client, 21), void,
	void* rcx, int sequence_number, float input_sample_frametime, bool active)
{
	DEBUG_RETURN(CHLClient_CreateMove, rcx, sequence_number, input_sample_frametime, active);

	CALL_ORIGINAL(rcx, sequence_number, input_sample_frametime, active);

	auto pLocal = H::Entities.GetLocal();
	auto pWeapon = H::Entities.GetWeapon();
	if (!pLocal)
		return;

	bool* pSendPacket = reinterpret_cast<bool*>(uintptr_t(_AddressOfReturnAddress()) + 0x20);
	CUserCmd* pCmd = &I::Input->m_pCommands[sequence_number % MULTIPLAYER_BACKUP];

	// Everything below is Amalgam's input path; the tracker bills it per feature
	// (see Utils/Perf/Tracker.h). The zone opens after the engine's own
	// CreateMove and prediction update so their cost stays theirs.
	PROF_ZONE("CreateMove (all)", Perf::GROUP_CREATEMOVE);

	PROF_CALL("IPrediction::Update", Perf::GROUP_ENGINE, I::Prediction->Update(I::ClientState->m_nDeltaTick, I::ClientState->m_nDeltaTick > 0, I::ClientState->last_command_ack, I::ClientState->lastoutgoingcommand + I::ClientState->chokedcommands));

	PROF_CALL("CreateMove::UpdateInfo", Perf::GROUP_CREATEMOVE, UpdateInfo(pLocal, pWeapon, pCmd));
		PROF_CALL("Spectate::CreateMove", Perf::GROUP_CREATEMOVE, F::Spectate.CreateMove(pCmd));
		PROF_CALL("Backtrack::CreateMove", Perf::GROUP_CREATEMOVE, F::Backtrack.CreateMove(pCmd));
		PROF_CALL("Misc::RunPre", Perf::GROUP_CREATEMOVE, F::Misc.RunPre(pLocal, pCmd));
	PROF_CALL("Ticks::Start", Perf::GROUP_CREATEMOVE, F::Ticks.Start(pLocal, pCmd));
		PROF_CALL("Aimbot::Run", Perf::GROUP_CREATEMOVE, F::Aimbot.Run(pLocal, pWeapon, pCmd));
	PROF_CALL("Ticks::End", Perf::GROUP_CREATEMOVE, F::Ticks.End(pLocal, pCmd));
		PROF_CALL("CritHack::Run", Perf::GROUP_CREATEMOVE, F::CritHack.Run(pLocal, pWeapon, pCmd));
		PROF_CALL("NoSpread::Run", Perf::GROUP_CREATEMOVE, F::NoSpread.Run(pLocal, pWeapon, pCmd));
		PROF_CALL("Misc::RunPost", Perf::GROUP_CREATEMOVE, F::Misc.RunPost(pLocal, pCmd));
		PROF_CALL("PacketManip::Run", Perf::GROUP_CREATEMOVE, F::PacketManip.Run(pLocal, pWeapon, pCmd, pSendPacket));
		PROF_CALL("Ticks::CreateMove", Perf::GROUP_CREATEMOVE, F::Ticks.CreateMove(pLocal, pWeapon, pCmd, pSendPacket));
		PROF_CALL("AntiAim::Run", Perf::GROUP_CREATEMOVE, F::AntiAim.Run(pLocal, pWeapon, pCmd, *pSendPacket));
		PROF_CALL("AntiCheatCompat::CreateMove", Perf::GROUP_CREATEMOVE, F::AntiCheatCompatibility.CreateMove(pCmd, pSendPacket));
		PROF_CALL("Visuals::CreateMove", Perf::GROUP_CREATEMOVE, F::Visuals.CreateMove(pLocal, pWeapon));
		PROF_CALL("TrajectoryGhost::Run", Perf::GROUP_CREATEMOVE, F::TrajectoryGhost.Run(pLocal, pWeapon));
		PROF_CALL("Visuals::LocalAnimations", Perf::GROUP_CREATEMOVE, F::Visuals.LocalAnimations(pLocal, pWeapon, pCmd, *pSendPacket));
	PROF_CALL("EnginePrediction::End", Perf::GROUP_ENGINE, F::EnginePrediction.End(pLocal, pCmd));
		PROF_CALL("Resolver::CreateMove", Perf::GROUP_CREATEMOVE, F::Resolver.CreateMove());
		PROF_CALL("NoSpreadHitscan::AskForPerf", Perf::GROUP_CREATEMOVE, F::NoSpreadHitscan.AskForPlayerPerf());
	G::Choking = !*pSendPacket, G::LastUserCmd = pCmd;
}