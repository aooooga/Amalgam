#pragma once
#include "../../../SDK/SDK.h"

#include "../AimbotGlobal/AimbotGlobal.h"

struct HitscanHitbox_t
{
	const mstudiobbox_t* m_pBox = nullptr; // bone
	std::optional<Vec3> m_vPos = std::nullopt; // custom position
	// otherwise use entity transform

	int m_iHitbox = 0;
};

class CAimbotHitscan
{
private:
	std::vector<HitscanHitbox_t> GetHitboxes(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pTarget);
	int GetHitboxPriority(int nHitbox, CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CBaseEntity* pTarget);
	void GetHitboxInfo(HitscanHitbox_t& tHitbox, CBaseEntity* pTarget, matrix3x4* aBones = nullptr, const matrix3x4** pTransform = nullptr, Vec3* pMins = nullptr, Vec3* pMaxs = nullptr);
	void GetHullInfo(CBaseEntity* pTarget, TickRecord* pRecord = nullptr, const matrix3x4** pTransform = nullptr, Vec3* pMins = nullptr, Vec3* pMaxs = nullptr, float* pModelScale = nullptr);
	// Fills aPoints and returns the count (1 = centre only). Called per record per
	// hitbox, so it writes into the caller's buffer rather than returning a vector.
	static constexpr int MAX_HITBOX_POINTS = 9;
	int GetHitboxPoints(CBaseEntity* pTarget, CTFWeaponBase* pWeapon, const Vec3& vMins, const Vec3& vMaxs, std::array<Vec3, MAX_HITBOX_POINTS>& aPoints, int nHitbox = 0);
	int CanHit(Target_t& tTarget, CTFPlayer* pLocal, CTFWeaponBase* pWeapon);

	// pLocal is threaded through from CanHit: this runs per record per hitbox per
	// point, and resolving the local player here cost two virtual calls each time.
	bool Aim(const Vec3& vCurAngle, Vec3 vToAngle, Vec3& vOut, CTFPlayer* pLocal, int iMethod = Vars::Aimbot::General::AimType.Value);
	void Aim(CUserCmd* pCmd, Vec3& vAngles, int iMethod = Vars::Aimbot::General::AimType.Value);
	bool ShouldFire(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd, const Target_t& tTarget);

	Vec3 m_vEyePos = {};

	matrix3x4 m_mMatrix = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
	matrix3x4 m_mHullMatrix = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

public:
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon, CUserCmd* pCmd);
};

ADD_FEATURE(CAimbotHitscan, AimbotHitscan);