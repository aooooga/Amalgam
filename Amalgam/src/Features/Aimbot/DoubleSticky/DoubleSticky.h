#pragma once
#include "../../../SDK/SDK.h"

// Port of DoubleSticky.lua: while the key is held, finds a steep "mortar" pitch whose
// sticky lands on the same spot your current view aims at, silently overrides the pitch
// so the first sticky takes the high arc, then auto-fires the second (direct) sticky at
// the moment that makes both land/arm in sync, re-solving from your current position.
class CDoubleSticky
{
private:
	struct SimResult_t
	{
		Vec3 m_vEndPos = {};
		float m_flTime = 0.f;
		bool m_bHit = false; // hit a surface (false = overshoot cutoff / timed out)
		std::vector<Vec3> m_vPath = {};
	};

	bool Simulate(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vAngles, SimResult_t& tResult, float flMaxDist2DSqr = 0.f);
	bool BisectPitch(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vSource, const Vec3& vTarget, float flYaw, float flLow, float flHigh, int iIterations, float flMaxDist2DSqr, SimResult_t& tBest, float& flBestPitch, float& flClosest);
	int StepSolve(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vSource, const Vec3& vTarget, float flYaw, int iBudget, SimResult_t& tBest, float& flBestPitch);
	bool FindDirectPitch(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, const Vec3& vTarget, Vec3& vAngleOut, float& flTimeOut);
	void RunWaiting(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, float flCurTime);
	void Reset();

public:
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
	void Draw();

private:
	KeyStorage m_tKeyStorage = {};

	// last successful high-arc solve (used on the tick the first sticky actually fires)
	bool m_bOverride = false;
	float m_flLastPitch = 0.f;
	float m_flLastHighTime = 0.f;
	Vec3 m_vLastTarget = {};
	float m_flLastCharge = 0.f;

	// waiting for the synced second shot
	bool m_bWaiting = false;
	float m_flSyncTime = 0.f; // curtime at which the first sticky is landed + armed
	Vec3 m_vTarget = {};
	int m_iFireTicks = 0; // 2 = press attack, 1 = release (fires), 0 = idle

	// warm-start cache: refine around last tick's solution instead of re-solving from
	// scratch every tick (full solve = ~20 long physics sims, enough to stutter)
	bool m_bSolved = false;
	float m_flSolvedPitch = 0.f;
	Vec3 m_vSolvedTarget = {};

	// incremental full solve, spread over several ticks under a per-tick sim budget
	struct SolveState_t
	{
		int m_iStage = 0; // 0 = idle, 1 = coarse scan, 2 = bisect
		int m_iStep = 0;
		bool m_bHavePrev = false;
		float m_flPrevPitch = 0.f, m_flPrevDist = 0.f;
		float m_flLow = 0.f, m_flHigh = 0.f;
		float m_flClosest = FLT_MAX;
		float m_flBestPitch = 0.f;
		SimResult_t m_tBest = {};
		Vec3 m_vTarget = {};
	};
	SolveState_t m_tSolve = {};
	bool m_bFailedValid = false; // remember unreachable targets so we don't rescan them every tick
	Vec3 m_vFailedTarget = {};

	// visuals
	bool m_bDrawPath = false;
	std::vector<Vec3> m_vAltPath = {};
};

ADD_FEATURE(CDoubleSticky, DoubleSticky);
