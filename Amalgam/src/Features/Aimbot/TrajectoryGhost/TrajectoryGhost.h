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
// backtrack latency). The predicted origin delta is computed in CreateMove
// (where movement simulation is valid) and consumed at render time by
// translating the model's live render bones.
class CTrajectoryGhost
{
public:
	struct Ghost_t
	{
		Vec3 m_vDelta = {};	// predicted future origin - current networked origin
		int m_iFrame = -1;	// framecount the delta was simulated on (-1 = no cache)
	};

private:
	// entindex -> predicted delta, refreshed on the stagger schedule below. A
	// fixed array indexed by entindex (players are 1..MAX_PLAYERS) plus a
	// companion valid flag gives O(1), allocation-free lookup in the
	// per-model-draw hot paths (RenderHandler / CGlow::RenderTrajectory) without
	// the per-tick bucket churn of an unordered_map. m_vGhostIndices holds the
	// renderable entindices in insertion order so RenderMain iterates only the
	// populated slots; its capacity is reused across ticks (clear() keeps the
	// buffer).
	Ghost_t m_aGhosts[MAX_PLAYERS_ARRAY_SAFE] = {};
	bool m_aValid[MAX_PLAYERS_ARRAY_SAFE] = {};
	std::vector<int> m_vGhostIndices = {};

	// Run is driven from CreateMove, which fires per command - below tickrate fps
	// that simulates work no frame ever draws. Latched to one pass per frame.
	int m_iRunFrame = -1;

	// Stagger schedule. A ghost's delta is an offset applied to the player's
	// LIVE origin every frame, so a delta computed a few frames ago still tracks
	// the player exactly - only the shape of their predicted path is stale, which
	// is imperceptible over ~45ms. Refreshing every REFRESH_FRAMES frames with a
	// per-frame cap turns the simulation cost from O(players) per tick into a
	// bounded constant.
	static constexpr int REFRESH_FRAMES = 3;
	static constexpr int REFRESH_BUDGET = 4;

	// Per-frame render-gate cache (ShouldRender is called from RenderMain and
	// CGlow::Store, and RenderMain runs once per FlexFOV capture face).
	int m_iViewFrame = -1;
	Vec3 m_vViewFwd = {};
	Vec3 m_vViewEye = {};
	bool m_bFOVGate = false;
	float m_flFOVCos = -1.f;
	int m_aVerdictFrame[MAX_PLAYERS_ARRAY_SAFE] = {};
	bool m_aVerdict[MAX_PLAYERS_ARRAY_SAFE] = {};

	// Which model the re-entered draw hook is currently producing.
	enum EPass { PASS_GHOST, PASS_MASK };
	EPass m_ePass = PASS_GHOST;

	// The ghost the render pass is currently drawing, so RenderHandler (re-entered
	// through the model-draw hook) knows which delta to translate the bones by.
	const Ghost_t* m_pActive = nullptr;
	bool m_bActiveIgnoreZ = false;

	// Reset every slot flagged last pass, then drop the index list.
	void ClearGhosts();
	// Refresh the per-frame view basis / FOV cone used by the render gate.
	void EnsureViewCache();
	// The uncached body of ShouldRender. bFace: a FlexFOV capture pass, where the
	// main-view on-screen test is both wrong and wasted.
	bool EvaluateRender(CTFPlayer* pLocal, CTFPlayer* pEntity, bool bFace);

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
