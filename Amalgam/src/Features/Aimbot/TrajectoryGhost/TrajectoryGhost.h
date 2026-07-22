#pragma once
#include "../../../SDK/SDK.h"

// Flag stamped into CGlow's per-entity flags so a glow silhouette is routed
// through the trajectory ghost's translated bones (parallel to backtrack /
// fakeangle). A high bit so it never collides with BacktrackEnum (1..8) or the
// fakeangle path's flag value of 1.
constexpr int TRAJECTORY_GHOST_FLAG = 1 << 10;

// Aimbot > Draw "Trajectory ghost": renders a duplicate of a player's own model
// at the position they are predicted to reach by the time the local player's
// currently-held weapon arrives (projectile travel time; hitscan/melee ~ the
// backtrack latency). The predicted origin delta is computed per tick in
// CreateMove (where movement simulation is valid) and consumed at render time by
// translating the model's live render bones.
class CTrajectoryGhost
{
public:
	struct Ghost_t
	{
		Vec3 m_vDelta = {}; // predicted future origin - current networked origin
	};

private:
	// entindex -> predicted delta, rebuilt every CreateMove tick. A fixed array
	// indexed by entindex (players are 1..MAX_PLAYERS) plus a companion valid flag
	// gives O(1), allocation-free lookup in the per-model-draw hot paths
	// (RenderHandler / CGlow::RenderTrajectory) without the per-tick bucket churn
	// of an unordered_map. m_vGhostIndices holds the active entindices in insertion
	// order so RenderMain iterates only the populated slots; its capacity is reused
	// across ticks (clear() keeps the buffer).
	Ghost_t m_aGhosts[MAX_PLAYERS_ARRAY_SAFE] = {};
	bool m_aValid[MAX_PLAYERS_ARRAY_SAFE] = {};
	std::vector<int> m_vGhostIndices = {};

	// The ghost the render pass is currently drawing, so RenderHandler (re-entered
	// through the model-draw hook) knows which delta to translate the bones by.
	const Ghost_t* m_pActive = nullptr;
	bool m_bActiveIgnoreZ = false;

	// Reset every slot flagged last tick, then drop the index list.
	void ClearGhosts();

public:
	void Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon);
	void RenderMain();
	void RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	bool Active();
	// Shared enemy/team + FOV + on-screen + has-ghost gate, used by RenderMain and
	// CGlow::Store. Fills vDelta on success.
	bool ShouldRender(CTFPlayer* pLocal, CTFPlayer* pEntity, Vec3& vDelta);
	bool GetDelta(int iIndex, Vec3& vDelta);

	bool m_bRendering = false;
};

ADD_FEATURE(CTrajectoryGhost, TrajectoryGhost);
