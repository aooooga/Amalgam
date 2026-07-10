#include "DoubleSticky.h"

#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"

static constexpr float MAX_SIM_TIME = 8.f; // a max-charge mortar arc stays airborne for ~6.5s
static constexpr float ACCEPT_MISS = 75.f; // 3D, so a lob that lands at the wrong height is rejected
static constexpr float MIN_ARC_GAP = 0.2f; // lob must fly this much longer than the direct shot to be worth syncing

bool CDoubleSticky::Simulate(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vAngles, SimResult_t& tResult, float flMaxDist2DSqr)
{
	ProjectileInfo tProjInfo = {};
	if (!F::ProjSim.GetInfo(pLocal, pWeapon, vAngles, tProjInfo, ProjSimEnum::InitCheck | ProjSimEnum::NoRandomAngles)
		|| !F::ProjSim.Initialize(tProjInfo))
		return false;

	CGameTrace trace = {};
	CTraceFilterCollideable filter = {};
	filter.pSkip = pLocal;
	int nMask = MASK_SOLID;
	F::ProjSim.SetupTrace(filter, nMask, pWeapon);

	Vec3 vSource = F::ProjSim.GetOrigin();
	Vec3 vNew = vSource;
	int iTicks = TIME_TO_TICKS(MAX_SIM_TIME);
	for (int n = 1; n <= iTicks; n++)
	{
		F::ProjSim.RunTick(tProjInfo);

		Vec3 vOld = vNew; vNew = F::ProjSim.GetOrigin();
		SDK::TraceHull(vOld, vNew, -tProjInfo.m_vHull, tProjInfo.m_vHull, nMask, &filter, &trace);
		if (trace.DidHit())
		{
			tProjInfo.m_vPath.push_back(trace.endpos);
			tResult = { trace.endpos, TICKS_TO_TIME(n), true, std::move(tProjInfo.m_vPath) };
			return true;
		}
		if (flMaxDist2DSqr && vNew.DistTo2DSqr(vSource) > flMaxDist2DSqr)
			break; // flew well past the target, no point simulating further
	}

	// didn't land in time / overshot: still report where it got to so the solver can
	// classify the pitch as short or far instead of discarding the sample
	tProjInfo.m_vPath.push_back(vNew);
	tResult = { vNew, MAX_SIM_TIME, false, std::move(tProjInfo.m_vPath) };
	return true;
}

// Bisect pitch inside [flLow, flHigh], driving on the landing point's signed projection
// onto the horizontal direction to the target (so blocked / backwards arcs classify
// correctly as short or far), tracking the best full-3D miss seen.
bool CDoubleSticky::BisectPitch(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vSource, const Vec3& vTarget, float flYaw, float flLow, float flHigh, int iIterations, float flMaxDist2DSqr, SimResult_t& tBest, float& flBestPitch, float& flClosest)
{
	Vec3 vDelta = vTarget - vSource;
	float flTargetDist = vDelta.Length2D();
	Vec3 vDir = vDelta.Normalized2D();

	for (int i = 0; i < iIterations; i++)
	{
		float flMid = (flLow + flHigh) / 2;

		SimResult_t tSim = {};
		if (!Simulate(pLocal, pWeapon, { flMid, flYaw, 0.f }, tSim, flMaxDist2DSqr))
			return false;

		Vec3 vEndPos = tSim.m_vEndPos;
		if (tSim.m_bHit)
		{
			float flMiss = vEndPos.DistTo(vTarget);
			if (flMiss < flClosest)
			{
				flClosest = flMiss;
				flBestPitch = flMid;
				tBest = std::move(tSim);
			}
		}

		Vec3 vLand = vEndPos - vSource;
		float flDist = vLand.x * vDir.x + vLand.y * vDir.y;
		if (flDist < flTargetDist)
			flLow = flMid; // fell short, flatten
		else
			flHigh = flMid; // went far, steepen
	}

	return flClosest < ACCEPT_MISS;
}

// Incremental version of the full lofted-pitch solve. The steep branch of the range
// curve lives between -88 and roughly the apex pitch: coarse-sample it to bracket the
// target distance, then bisect inside that bracket. Runs at most iBudget simulations
// per call and persists its progress in m_tSolve, so the expensive first solve is
// spread over a few ticks instead of hitching one frame.
// Returns 1 = solved (tBest/flBestPitch filled), 0 = still working, -1 = no solution.
int CDoubleSticky::StepSolve(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vSource, const Vec3& vTarget, float flYaw, int iBudget, SimResult_t& tBest, float& flBestPitch)
{
	static constexpr float flLowBound = -88.f, flHighBound = -30.f;
	static constexpr int iCoarseSteps = 9, iBisectSteps = 8;

	auto& tS = m_tSolve;
	if (!tS.m_iStage || tS.m_vTarget.DistTo(vTarget) > 150.f)
	{
		tS = {};
		tS.m_iStage = 1;
		tS.m_vTarget = vTarget;
	}

	Vec3 vDelta = vTarget - vSource;
	float flTargetDist = vDelta.Length2D();
	Vec3 vDir = vDelta.Normalized2D();
	float flMaxDist2DSqr = powf(flTargetDist + 300.f, 2);

	auto Finish = [&]() -> int
	{
		tS.m_iStage = 0;
		if (tS.m_flClosest >= ACCEPT_MISS)
			return -1;
		tBest = std::move(tS.m_tBest);
		flBestPitch = tS.m_flBestPitch;
		return 1;
	};

	while (iBudget-- > 0)
	{
		float flPitch = tS.m_iStage == 1
			? flLowBound + (flHighBound - flLowBound) * tS.m_iStep / (iCoarseSteps - 1)
			: (tS.m_flLow + tS.m_flHigh) / 2;

		SimResult_t tSim = {};
		if (!Simulate(pLocal, pWeapon, { flPitch, flYaw, 0.f }, tSim, flMaxDist2DSqr))
		{
			tS.m_iStage = 0;
			return -1;
		}

		Vec3 vEndPos = tSim.m_vEndPos;
		if (tSim.m_bHit)
		{
			float flMiss = vEndPos.DistTo(vTarget);
			if (flMiss < tS.m_flClosest)
			{
				tS.m_flClosest = flMiss;
				tS.m_flBestPitch = flPitch;
				tS.m_tBest = std::move(tSim);
			}
		}
		Vec3 vLand = vEndPos - vSource;
		float flDist = vLand.x * vDir.x + vLand.y * vDir.y;

		if (tS.m_iStage == 1)
		{
			// coarse scan, steepest first; the first bracket straddling the target
			// distance is the loftiest solution
			if (tS.m_bHavePrev && tS.m_flPrevDist <= flTargetDist && flTargetDist <= flDist)
			{
				tS.m_flLow = tS.m_flPrevPitch;
				tS.m_flHigh = flPitch;
				tS.m_iStage = 2;
				tS.m_iStep = 0;
				continue;
			}
			tS.m_flPrevPitch = flPitch;
			tS.m_flPrevDist = flDist;
			tS.m_bHavePrev = true;
			if (++tS.m_iStep >= iCoarseSteps)
				return Finish(); // no bracket, target sits outside the steep branch
		}
		else
		{
			if (flDist < flTargetDist)
				tS.m_flLow = flPitch; // fell short, flatten
			else
				tS.m_flHigh = flPitch; // went far, steepen
			if (++tS.m_iStep >= iBisectSteps)
				return Finish();
		}
	}

	return 0; // budget spent, resume next tick
}

// Solve the direct (low-arc) pitch to hit vTarget from the current position; the flat
// branch between ~-45 and straight-at-target is monotonic the other way around.
bool CDoubleSticky::FindDirectPitch(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vTarget, Vec3& vAngleOut, float& flTimeOut)
{
	Vec3 vShoot = pLocal->GetShootPos();
	Vec3 vAngleTo = Math::CalcAngle(vShoot, vTarget);
	float flDist2D = vTarget.DistTo2D(vShoot);
	float flMaxDist2DSqr = powf(flDist2D + 100.f, 2);

	float flLow = -45.f, flHigh = std::min(vAngleTo.x + 10.f, 88.f);
	float flClosest = FLT_MAX;
	float flBestPitch = vAngleTo.x;

	for (int i = 0; i < 8; i++)
	{
		float flMid = (flLow + flHigh) / 2;

		SimResult_t tAlt = {};
		if (Simulate(pLocal, pWeapon, { flMid, vAngleTo.y, 0.f }, tAlt, flMaxDist2DSqr))
		{
			if (tAlt.m_bHit)
			{
				float flMiss = tAlt.m_vEndPos.DistTo(vTarget);
				if (flMiss < flClosest)
				{
					flClosest = flMiss;
					flBestPitch = flMid;
					flTimeOut = tAlt.m_flTime;
				}
			}

			if (tAlt.m_vEndPos.DistTo2D(vShoot) < flDist2D)
				flHigh = flMid; // fell short, aim higher
			else
				flLow = flMid; // went far, aim lower
		}
		else
			flLow = flMid;
	}

	vAngleOut = { flBestPitch, vAngleTo.y, 0.f };
	return flClosest < ACCEPT_MISS;
}

void CDoubleSticky::RunWaiting(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, float flCurTime)
{
	if (flCurTime > m_flSyncTime + 1.f)
	{
		Reset();
		return;
	}

	Vec3 vAim = {};
	float flTravel = 0.f;
	if (!FindDirectPitch(pLocal, pWeapon, m_vTarget, vAim, flTravel))
		flTravel = m_vTarget.DistTo2D(pLocal->GetShootPos()) / 900.f; // rough fallback, vAim still aims at the target

	float flArmTime = SDK::AttribHookValue(0.7f, "sticky_arm_time", pWeapon);
	if (!m_iFireTicks)
	{
		// fire once our sticky would land/arm no earlier than the first one is ready
		if (flCurTime + std::max(flTravel, flArmTime) + TICK_INTERVAL < m_flSyncTime || !G::CanPrimaryAttack)
			return;
		m_iFireTicks = 2;
	}

	SDK::FixMovement(pCmd, vAim);
	pCmd->viewangles = vAim;
	G::PSilentAngles = true;

	if (m_iFireTicks == 2)
		pCmd->buttons |= IN_ATTACK; // start the throw
	else
		pCmd->buttons &= ~IN_ATTACK; // release fires the sticky

	if (--m_iFireTicks <= 0)
		Reset();
}

void CDoubleSticky::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd)
{
	m_bDrawPath = m_bWaiting; // keep showing the arc while the first sticky flies

	if (!Vars::Aimbot::Projectile::DoubleSticky.Value
		|| !pWeapon || pWeapon->GetWeaponID() != TF_WEAPON_PIPEBOMBLAUNCHER
		|| !pLocal->IsAlive() || pLocal->IsAGhost() || pLocal->IsTaunting())
	{
		Reset();
		m_bOverride = false;
		m_flLastCharge = 0.f;
		return;
	}

	float flCurTime = TICKS_TO_TIME(pLocal->m_nTickBase());
	float flCharge = pWeapon->As<CTFPipebombLauncher>()->m_flChargeBeginTime();

	if (m_bWaiting)
	{
		RunWaiting(pLocal, pWeapon, pCmd, flCurTime);
		m_flLastCharge = flCharge;
		return;
	}

	// charge reset while we were overriding = the high-arc sticky just fired
	if (m_bOverride && m_flLastCharge > 0.f && flCharge <= 0.f)
	{
		// the firing cmd is this one, keep the steep pitch on it
		Vec3 vAim = { m_flLastPitch, pCmd->viewangles.y, 0.f };
		SDK::FixMovement(pCmd, vAim);
		pCmd->viewangles = vAim;
		G::PSilentAngles = true;

		float flArmTime = SDK::AttribHookValue(0.7f, "sticky_arm_time", pWeapon);
		m_bWaiting = true;
		m_flSyncTime = flCurTime + std::max(m_flLastHighTime, flArmTime);
		m_vTarget = m_vLastTarget;
		m_bDrawPath = true;

		m_bOverride = false;
		m_flLastCharge = flCharge;
		return;
	}

	m_bOverride = false;
	if (!U::KeyHandler.Down(Vars::Aimbot::Projectile::DoubleStickyKey.Value, true, &m_tKeyStorage))
	{
		m_flLastCharge = flCharge;
		return;
	}

	// where would a sticky land at the current view angles?
	Vec3 vViewAngles = I::EngineClient->GetViewAngles();
	SimResult_t tPrimary = {};
	if (!Simulate(pLocal, pWeapon, vViewAngles, tPrimary) || !tPrimary.m_bHit || tPrimary.m_vPath.empty())
	{
		m_flLastCharge = flCharge;
		return;
	}

	Vec3 vSource = tPrimary.m_vPath.front();
	if (tPrimary.m_vEndPos.DistTo2D(vSource) <= 100.f)
	{
		m_flLastCharge = flCharge;
		return;
	}

	Vec3 vTarget = tPrimary.m_vEndPos;
	SimResult_t tBest = {};
	float flBestPitch = 0.f;
	bool bHave = false;

	// cheap path: last tick's solution is usually still nearly right, refine around it
	if (m_bSolved && m_vSolvedTarget.DistTo(vTarget) < 150.f)
	{
		float flClosest = FLT_MAX;
		float flMaxDist2DSqr = powf(vTarget.DistTo2D(vSource) + 300.f, 2);
		bHave = BisectPitch(pLocal, pWeapon, vSource, vTarget, vViewAngles.y, m_flSolvedPitch - 4.f, m_flSolvedPitch + 4.f, 4, flMaxDist2DSqr, tBest, flBestPitch, flClosest);
	}
	// full solve, budgeted across ticks; skip targets we already know are unreachable
	if (!bHave && !(m_bFailedValid && m_vFailedTarget.DistTo(vTarget) < 150.f))
	{
		switch (StepSolve(pLocal, pWeapon, vSource, vTarget, vViewAngles.y, 4, tBest, flBestPitch))
		{
		case 1: bHave = true; m_bFailedValid = false; break;
		case -1: m_bFailedValid = true; m_vFailedTarget = vTarget; break;
		}
	}

	m_bSolved = bHave;
	if (bHave)
	{
		m_flSolvedPitch = flBestPitch;
		m_vSolvedTarget = vTarget;
	}

	if (bHave
		&& tBest.m_flTime >= tPrimary.m_flTime + MIN_ARC_GAP) // no point lobbing an arc that isn't meaningfully slower
	{
		Vec3 vAim = { flBestPitch, pCmd->viewangles.y, 0.f };
		SDK::FixMovement(pCmd, vAim);
		pCmd->viewangles = vAim;
		G::PSilentAngles = true;

		m_bOverride = true;
		m_flLastPitch = flBestPitch;
		m_flLastHighTime = tBest.m_flTime;
		m_vLastTarget = tPrimary.m_vEndPos;

		m_vAltPath = std::move(tBest.m_vPath);
		m_bDrawPath = true;
	}

	m_flLastCharge = flCharge;
}

void CDoubleSticky::Draw()
{
	if (!Vars::Aimbot::Projectile::DoubleSticky.Value || !m_bDrawPath || m_vAltPath.empty() || !Vars::Colors::DoubleStickyPath.Value.a)
		return;

	H::Draw.RenderPath(m_vAltPath, Vars::Colors::DoubleStickyPath.Value, true, Vars::Visuals::Path::StyleEnum::Line);
}

void CDoubleSticky::Reset()
{
	m_bWaiting = false;
	m_flSyncTime = 0.f;
	m_iFireTicks = 0;
	m_bSolved = false;
	m_tSolve = {};
	m_bFailedValid = false;
	m_bDrawPath = false;
	m_vAltPath.clear();
}
