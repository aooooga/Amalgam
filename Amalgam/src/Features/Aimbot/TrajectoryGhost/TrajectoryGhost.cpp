#include "TrajectoryGhost.h"

#include "../../Simulation/MovementSimulation/MovementSimulation.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"
#include "../../Backtrack/Backtrack.h"
#include "../../Visuals/Materials/Materials.h"
#include "../../Visuals/FlexFOV/FlexFOV.h"
#include "../../Visuals/CameraWindow/CameraWindow.h"

// Reused across players and frames: a fresh MoveStorage per player per pass
// constructed (and Initialize reseated) its m_vPath vector, a heap alloc/free
// pair per player for a path this feature never reads. Reset field-by-field
// below so the vector keeps its capacity while every value Initialize does not
// itself overwrite (move input, average yaw, failure flags) still starts clean.
static MoveStorage s_tStorage = {};

static inline void ResetStorage()
{
	s_tStorage.m_pPlayer = nullptr;
	s_tStorage.m_MoveData = {};
	s_tStorage.m_pData = nullptr;
	s_tStorage.m_flAverageYaw = 0.f;
	s_tStorage.m_bBunnyHop = false;
	s_tStorage.m_flSimTime = 0.f;
	s_tStorage.m_flPredictedDelta = 0.f;
	s_tStorage.m_flPredictedSimTime = 0.f;
	s_tStorage.m_bDirectMove = true;
	s_tStorage.m_bPredictNetworked = true;
	s_tStorage.m_vPredictedOrigin = {};
	s_tStorage.m_vPath.clear(); // keeps the buffer; Initialize assigns into it
	s_tStorage.m_bFailed = false;
	s_tStorage.m_bInitFailed = false;
}

bool CTrajectoryGhost::Active()
{
	return Vars::Aimbot::Draw::Trajectory.Value
		&& (Vars::Aimbot::Draw::TrajectoryTeams.Value & (Vars::Aimbot::Draw::TrajectoryTeamsEnum::Enemies | Vars::Aimbot::Draw::TrajectoryTeamsEnum::Teammates));
}

void CTrajectoryGhost::ClearGhosts()
{
	// Only the slots flagged last pass need resetting; the index list records them.
	for (int iIndex : m_vGhostIndices)
		m_aValid[iIndex] = false;
	m_vGhostIndices.clear();
}

// Per-frame prediction: for the players that can plausibly be seen, walk movement
// simulation the number of ticks the held weapon needs to reach them and cache the
// resulting origin delta. Runs inside CreateMove so movement simulation has a valid
// prediction context, exactly like the projectile aimbot's lead loop - but unlike
// the aimbot this is a visual, so it culls hard up front and refreshes on a stagger
// rather than simulating every player every tick.
void CTrajectoryGhost::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	// CreateMove fires per command. Every consumer of the result is a render
	// path, so anything computed more than once per frame is thrown away.
	const int iFrame = I::GlobalVars->framecount;
	if (m_iRunFrame == iFrame)
		return;
	m_iRunFrame = iFrame;

	ClearGhosts();

	if (!Active() || !pLocal || !pLocal->IsAlive() || !pWeapon)
		return;

	// Held-weapon projectile speed (0 = hitscan / melee, treated as near-instant
	// so the ghost sits at the latency-lead position).
	float flSpeed = 0.f;
	ProjectileInfo tProjInfo = {};
	if (G::PrimaryWeaponType == EWeaponType::PROJECTILE
		&& F::ProjSim.GetInfo(pLocal, pWeapon, {}, tProjInfo, ProjSimEnum::NoRandomAngles | ProjSimEnum::PredictCmdNum))
		flSpeed = F::ProjSim.GetVelocity().Length();

	const float flLatency = F::Backtrack.GetReal() + TICKS_TO_TIME(F::Backtrack.GetAnticipatedChoke());
	const float flScale = Vars::Aimbot::Draw::TrajectoryLeadScale.Value / 100.f;
	const float flOffset = Vars::Aimbot::Draw::TrajectoryOffset.Value / 1000.f;
	const float flMaxTime = Vars::Aimbot::Draw::TrajectoryMaxTime.Value;
	const float flMinDist = Vars::Aimbot::Draw::TrajectoryMinDistance.Value;

	const Vec3 vShoot = pLocal->GetShootPos();
	const int iTeams = Vars::Aimbot::Draw::TrajectoryTeams.Value;
	const int iLocalTeam = pLocal->m_iTeamNum();

	// Angular pre-cull, evaluated BEFORE any simulation. Every reject here used
	// to cost a full Initialize (two whole-datamap prediction transfers plus an
	// alloc) and up to TrajectoryMaxTime worth of ProcessMovement, only to be
	// discarded by ShouldRender at draw time. The cone is deliberately far wider
	// than any screen so a player walking into view is never a frame late; the
	// exact on-screen / FOV test still runs at render time. FlexFOV's periphery
	// can show almost anything, so the cull is dropped entirely while it is on.
	const float flCullAngle = Vars::Aimbot::Draw::TrajectoryFOV.Value
		? std::min(Vars::Aimbot::Draw::TrajectoryFOV.Value + 25.f, 180.f)
		: 100.f;
	const bool bCull = !F::FlexFOV.m_bActive && flCullAngle < 180.f;
	const float flCullCos = cosf(Math::Deg2Rad(flCullAngle));
	Vec3 vViewFwd; Math::AngleVectors(I::EngineClient->GetViewAngles(), &vViewFwd);
	const Vec3 vViewEye = pLocal->GetEyePosition();

	// Eligible players and their tick counts, collected before any simulation so
	// the refresh budget can be spent on the stalest entries rather than on
	// whoever happens to come first in entity order.
	struct Candidate_t { CTFPlayer* m_pPlayer; int m_iIndex; int m_iTicks; int m_iAge; };
	Candidate_t aCandidates[MAX_PLAYERS_ARRAY_SAFE];
	int nCandidates = 0;

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerAll))
	{
		if (nCandidates >= MAX_PLAYERS_ARRAY_SAFE)
			break;

		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || pPlayer == pLocal || pPlayer->IsDormant() || !pPlayer->IsAlive())
			continue;

		const bool bEnemy = pPlayer->m_iTeamNum() != iLocalTeam;
		if (bEnemy && !(iTeams & Vars::Aimbot::Draw::TrajectoryTeamsEnum::Enemies))
			continue;
		if (!bEnemy && !(iTeams & Vars::Aimbot::Draw::TrajectoryTeamsEnum::Teammates))
			continue;

		const int iIndex = pPlayer->entindex();
		if (iIndex < 0 || iIndex >= MAX_PLAYERS_ARRAY_SAFE)
			continue;

		// Ineligible from here on: drop any cached delta so a slot that is later
		// reused (respawn, entindex reuse on reconnect) can never resurrect a
		// stale offset inside the stagger window.
		const Vec3 vTarget = pPlayer->GetShootPos();
		if (bCull)
		{
			const Vec3 vTo = vTarget - vViewEye;
			const float flLen = vTo.Length();
			if (flLen > 0.f && vViewFwd.Dot(vTo) < flCullCos * flLen)
			{
				m_aGhosts[iIndex].m_iFrame = -1;
				continue;
			}
		}

		// Lead time: shot travel time to the target's current position plus the
		// backtrack latency, tuned by the scale/offset options and capped.
		const float flTravel = flSpeed > 1.f ? vShoot.DistTo(vTarget) / flSpeed : 0.f;
		float flLead = (flLatency + flTravel) * flScale + flOffset;
		flLead = std::clamp(flLead, 0.f, flMaxTime);

		const int iTicks = TIME_TO_TICKS(flLead);
		if (iTicks < 1)
		{
			m_aGhosts[iIndex].m_iFrame = -1;
			continue;
		}

		// Exact stationary reject: the delta is discarded below unless it clears
		// TrajectoryMinDistance in 2D, and a player cannot cover more than their
		// speed times the lead. Movement simulation feeds non-local players their
		// recorded direction, so a player at rest genuinely produces ~no delta;
		// the 1.5x headroom covers what acceleration inside the sim can add.
		if (flMinDist > 0.f)
		{
			auto pAvgVelocity = H::Entities.GetAvgVelocity(iIndex);
			const float flSpeed2D = (pAvgVelocity ? *pAvgVelocity : pPlayer->m_vecVelocity()).Length2D();
			if (flSpeed2D * flLead * 1.5f < flMinDist)
			{
				m_aGhosts[iIndex].m_iFrame = -1;
				continue;
			}
		}

		const int iCached = m_aGhosts[iIndex].m_iFrame;
		// Never simulated (or invalidated): maximally stale, so it is served first.
		aCandidates[nCandidates++] = { pPlayer, iIndex, iTicks, iCached < 0 ? 0x7FFFFFFF : iFrame - iCached };
	}

	// Spend the refresh budget oldest-first. Anything not refreshed this frame
	// keeps its cached delta, which stays anchored to the player's live origin.
	int nRefreshed = 0;
	while (nRefreshed < REFRESH_BUDGET)
	{
		int iBest = -1;
		for (int i = 0; i < nCandidates; i++)
		{
			if (aCandidates[i].m_iAge < REFRESH_FRAMES)
				continue;
			if (iBest < 0 || aCandidates[i].m_iAge > aCandidates[iBest].m_iAge)
				iBest = i;
		}
		if (iBest < 0)
			break;

		auto& tCandidate = aCandidates[iBest];
		tCandidate.m_iAge = 0; // served

		ResetStorage();
		// bHitchance / bPredict off: hitchance is an aimbot concept that walks the
		// whole record deque, and the choke catch-up ticks are the same expensive
		// ProcessMovement calls. Neither is visible in a ghost silhouette.
		if (!F::MoveSim.Initialize(tCandidate.m_pPlayer, s_tStorage, false, true, false))
		{
			F::MoveSim.Restore(s_tStorage);
			m_aGhosts[tCandidate.m_iIndex].m_iFrame = -1;
			nRefreshed++;
			continue;
		}
		for (int i = 0; i < tCandidate.m_iTicks && !s_tStorage.m_bFailed; i++)
			F::MoveSim.RunTick(s_tStorage);
		const Vec3 vPredicted = s_tStorage.m_vPredictedOrigin;
		F::MoveSim.Restore(s_tStorage);

		m_aGhosts[tCandidate.m_iIndex].m_vDelta = vPredicted - tCandidate.m_pPlayer->m_vecOrigin();
		m_aGhosts[tCandidate.m_iIndex].m_iFrame = iFrame;
		nRefreshed++;
	}

	for (int i = 0; i < nCandidates; i++)
	{
		const int iIndex = aCandidates[i].m_iIndex;
		if (m_aGhosts[iIndex].m_iFrame < 0)
			continue;
		if (m_aGhosts[iIndex].m_vDelta.Length2D() < flMinDist)
			continue;

		m_aValid[iIndex] = true;
		m_vGhostIndices.push_back(iIndex);
	}
}

bool CTrajectoryGhost::GetDelta(int iIndex, Vec3& vDelta)
{
	if (iIndex < 0 || iIndex >= MAX_PLAYERS_ARRAY_SAFE || !m_aValid[iIndex])
		return false;
	vDelta = m_aGhosts[iIndex].m_vDelta;
	return true;
}

// The view basis and FOV cone are identical for every entity in a frame; the old
// per-entity GetViewAngles + AngleVectors + normalize + acos + Rad2Deg is a lot of
// transcendental work to repeat 20-odd times per pass.
void CTrajectoryGhost::EnsureViewCache()
{
	const int iFrame = I::GlobalVars->framecount;
	if (m_iViewFrame == iFrame)
		return;
	m_iViewFrame = iFrame;

	Math::AngleVectors(I::EngineClient->GetViewAngles(), &m_vViewFwd);
	if (auto pLocal = H::Entities.GetLocal())
		m_vViewEye = pLocal->GetEyePosition();

	const float flFOV = Vars::Aimbot::Draw::TrajectoryFOV.Value;
	m_bFOVGate = flFOV > 0.f;
	// cos is monotonically decreasing, so "angle > fov" becomes "dot < cos(fov)"
	// once scaled by the vector length - no acos, no normalize.
	m_flFOVCos = m_bFOVGate ? cosf(Math::Deg2Rad(flFOV)) : -1.f;
}

bool CTrajectoryGhost::EvaluateRender(CTFPlayer* pLocal, CTFPlayer* pEntity, bool bFace)
{
	if (pEntity->IsDormant() || !pEntity->IsAlive() || !pEntity->ShouldDraw())
		return false;

	// FlexFOV capture passes: SDK::IsOnScreen tests the MAIN view, so on a face
	// pass it is both wasted (6 W2S transforms) and wrong - it can cull a ghost
	// the periphery would legitimately show. Use the same angular face cull the
	// chams / glow face passes use.
	if (bFace)
	{
		if (!F::FlexFOV.FaceCanSee(pEntity->GetAbsOrigin()))
			return false;
	}
	else if (!SDK::IsOnScreen(pEntity))
		return false;

	if (m_bFOVGate)
	{
		const Vec3 vTo = pEntity->GetShootPos() - m_vViewEye;
		const float flLen = vTo.Length();
		if (flLen > 0.f && m_vViewFwd.Dot(vTo) < m_flFOVCos * flLen)
			return false;
	}
	return true;
}

// enemy/team already filtered at Run time; here we re-check the frame-time
// concerns: the entity still has a ghost, is on screen and (optionally) inside
// the crosshair FOV cone.
bool CTrajectoryGhost::ShouldRender(CTFPlayer* pLocal, CTFPlayer* pEntity, Vec3& vDelta)
{
	if (!pLocal || !pEntity || !pEntity->IsPlayer())
		return false;
	// Cheapest gate first, and it bounds-checks the entindex for the memo below.
	if (!GetDelta(pEntity->entindex(), vDelta))
		return false;

	EnsureViewCache();

	// Memoize the verdict per frame: this is called from RenderMain and from
	// CGlow::Store, and the expensive part (IsOnScreen, 6 W2S transforms) is
	// view-state that cannot change inside a frame. Face passes are excluded -
	// their verdict is per-face - but there the test is already the cheap
	// FaceCanSee. +1 so the zero-initialised stamp never matches frame 0.
	const bool bFace = F::FlexFOV.m_bDrawing;
	const int iKey = I::GlobalVars->framecount + 1;
	const int iIndex = pEntity->entindex();
	if (!bFace && m_aVerdictFrame[iIndex] == iKey)
		return m_aVerdict[iIndex];

	const bool bResult = EvaluateRender(pLocal, pEntity, bFace);
	if (!bFace)
	{
		m_aVerdictFrame[iIndex] = iKey;
		m_aVerdict[iIndex] = bResult;
	}
	return bResult;
}

void CTrajectoryGhost::RenderMain()
{
	// Mirror the DrawModelExecute hook's own guards: under any of these the hook
	// early-returns to the engine draw, so triggering pEntity->DrawModel here
	// would render an extra untranslated real model instead of the ghost.
	if (!Active() || m_vGhostIndices.empty()
		|| I::EngineVGui->IsGameUIVisible() || SDK::CleanScreenshot()
		|| F::CameraWindow.m_bDrawing || F::FlexFOV.m_bReplacingView
		|| !F::Materials.m_bLoaded || G::Unload)
		return;

	auto pLocal = H::Entities.GetLocal();
	if (!pLocal)
		return;
	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext)
		return;

	const int iLocalTeam = pLocal->m_iTeamNum();

	const Color_t tOldColor = I::RenderView->GetColorModulation();
	const float flOldBlend = I::RenderView->GetBlend();
	IMaterial* pOldMaterial = nullptr; OverrideType_t iOldOverride = OVERRIDE_NORMAL;
	I::ModelRender->GetMaterialOverride(&pOldMaterial, &iOldOverride);

	// Peek-out mode marks the target's real model into the stencil and draws the
	// ghost only where that mask is set. The clear is a FULL-SCREEN operation, so
	// it happens once here rather than once per model draw (which is what doing
	// it inside the draw hook amounted to); each entity then owns a distinct
	// reference value so masks cannot bleed between them without re-clearing.
	const bool bBehindOnly = Vars::Aimbot::Draw::TrajectoryBehindOnly.Value;
	int iStencilRef = 0;
	if (bBehindOnly)
		pRenderContext->ClearBuffers(false, false, true);

	for (int iIndex : m_vGhostIndices)
	{
		Ghost_t& tGhost = m_aGhosts[iIndex];
		auto pEntity = I::ClientEntityList->GetClientEntity(iIndex);
		if (!pEntity)
			continue;
		auto pPlayer = pEntity->As<CTFPlayer>();
		Vec3 vDelta;
		if (!ShouldRender(pLocal, pPlayer, vDelta))
			continue;

		// Enemy and team ghosts carry independently-configured material layers.
		const bool bEnemy = pPlayer->m_iTeamNum() != iLocalTeam;
		auto& vLayers = bEnemy ? Vars::Aimbot::Draw::TrajectoryMaterialEnemy.Value : Vars::Aimbot::Draw::TrajectoryMaterialTeam.Value;
		if (vLayers.empty())
			continue;

		// The team's glow config doubles as the ghost's colour config: its health
		// gradient (when Health colour is on) tints the material fill too, so the
		// ghost matches the glow and the ESP tab's health colouring.
		const Glow_t& tGlow = bEnemy ? Vars::Aimbot::Draw::TrajectoryGlowEnemy.Value : Vars::Aimbot::Draw::TrajectoryGlowTeam.Value;
		const float flDistance = pLocal->GetAbsOrigin().DistTo(pPlayer->GetAbsOrigin());
		float flHealthFrac = -1.f;
		if (const float flMax = pPlayer->GetMaxHealth(); flMax > 0.f)
			flHealthFrac = std::clamp(float(pPlayer->m_iHealth()) / flMax, 0.f, 1.f);
		const bool bHealth = tGlow.HealthColor && flHealthFrac >= 0.f && !tGlow.Stops.empty();

		m_pActive = &tGhost;
		const float flOldInvis = pPlayer->m_flInvisibility();
		pPlayer->m_flInvisibility() = 0.f;

		if (bBehindOnly)
		{
			// One mask draw per entity, not per layer: the mask is the same real
			// model whatever the layers on top of it are.
			if (++iStencilRef > 255)
			{
				pRenderContext->ClearBuffers(false, false, true);
				iStencilRef = 1;
			}

			I::ModelRender->ForcedMaterialOverride(nullptr);
			pRenderContext->OverrideColorWriteEnable(true, false);
			pRenderContext->SetStencilEnable(true);
			pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_ALWAYS);
			pRenderContext->SetStencilPassOperation(STENCILOPERATION_REPLACE);
			pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilReferenceValue(iStencilRef);
			pRenderContext->SetStencilWriteMask(0xFF);
			pRenderContext->SetStencilTestMask(0x0);

			m_ePass = PASS_MASK;
			m_bRendering = true;
			pPlayer->DrawModel(STUDIO_RENDER);
			m_bRendering = false;
			m_ePass = PASS_GHOST;

			pRenderContext->OverrideColorWriteEnable(false, false);

			// Every layer below draws only where this entity's mask landed.
			pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_EQUAL);
			pRenderContext->SetStencilPassOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilReferenceValue(iStencilRef);
			pRenderContext->SetStencilWriteMask(0x0);
			pRenderContext->SetStencilTestMask(0xFF);
		}

		for (auto& [sName, tColor] : vLayers)
		{
			// Per-layer flat/distance colour, overridden by the health gradient
			// when the team's Health colour is enabled.
			const Color_t cFill = bHealth ? EvalGlowStops(tGlow.Stops, flHealthFrac) : tColor.GetColor(flDistance);
			// A fully transparent layer is a full model draw that contributes
			// nothing - the blend below is zero.
			if (!cFill.a)
				continue;

			auto pMaterial = F::Materials.GetMaterial(tColor.NameHash(sName));
			F::Materials.SetColor(pMaterial, cFill);
			I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);
			I::RenderView->SetColorModulation(cFill);
			I::RenderView->SetBlend(cFill.a / 255.f);

			// Peek-out always drew its ghost pass on top of the depth it is
			// masked against; outside it the per-layer / global option decides.
			m_bActiveIgnoreZ = bBehindOnly || tColor.IgnoreZ || Vars::Aimbot::Draw::TrajectoryIgnoreZ.Value;
			m_bRendering = true;
			pPlayer->DrawModel(STUDIO_RENDER);
			m_bRendering = false;
		}

		if (bBehindOnly)
			pRenderContext->SetStencilEnable(false);

		pPlayer->m_flInvisibility() = flOldInvis;
	}

	m_pActive = nullptr;
	I::RenderView->SetColorModulation(tOldColor);
	I::RenderView->SetBlend(flOldBlend);
	I::ModelRender->ForcedMaterialOverride(pOldMaterial, iOldOverride);
}

// Re-entered through the model-draw hook while m_bRendering is set: draw the
// model with its live render bones translated to the predicted position. The
// stencil state for the peek-out mode is owned by RenderMain - all this pass
// does is choose the bones.
void CTrajectoryGhost::RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];

	// Mask pass: the target's real model, untranslated, stencil only.
	if (m_ePass == PASS_MASK)
		return IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);

	const int nBones = pState.m_pStudioHdr ? pState.m_pStudioHdr->numbones : 0;
	if (!m_pActive || !pBoneToWorld || nBones < 1 || nBones > MAXSTUDIOBONES)
		return IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);

	static matrix3x4 s_aBones[MAXSTUDIOBONES];
	memcpy(s_aBones, pBoneToWorld, sizeof(matrix3x4) * nBones);
	const Vec3& vDelta = m_pActive->m_vDelta;
	for (int i = 0; i < nBones; i++)
	{
		s_aBones[i][0][3] += vDelta.x;
		s_aBones[i][1][3] += vDelta.y;
		s_aBones[i][2][3] += vDelta.z;
	}

	if (m_bActiveIgnoreZ)
	{
		if (auto pRenderContext = I::MaterialSystem->GetRenderContext())
		{
			pRenderContext->DepthRange(0.f, 0.2f);
			IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, s_aBones);
			pRenderContext->DepthRange(0.f, 1.f);
			return;
		}
	}
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, s_aBones);
}
