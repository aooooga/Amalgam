#include "TrajectoryGhost.h"

#include "../../Simulation/MovementSimulation/MovementSimulation.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"
#include "../../Backtrack/Backtrack.h"
#include "../../Visuals/Materials/Materials.h"
#include "../../Visuals/FlexFOV/FlexFOV.h"
#include "../../Visuals/CameraWindow/CameraWindow.h"

bool CTrajectoryGhost::Active()
{
	return Vars::Aimbot::Draw::Trajectory.Value
		&& (Vars::Aimbot::Draw::TrajectoryTeams.Value & (Vars::Aimbot::Draw::TrajectoryTeamsEnum::Enemies | Vars::Aimbot::Draw::TrajectoryTeamsEnum::Teammates));
}

// Per-tick prediction: for every relevant player, walk movement simulation the
// number of ticks the held weapon needs to reach them and cache the resulting
// origin delta. Runs inside CreateMove so movement simulation has a valid
// prediction context, exactly like the projectile aimbot's lead loop.
void CTrajectoryGhost::Run(CTFPlayer* pLocal, CTFWeaponBase* pWeapon)
{
	m_mGhosts.clear();

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

	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerAll))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || pPlayer == pLocal || pPlayer->IsDormant() || !pPlayer->IsAlive())
			continue;

		const bool bEnemy = pPlayer->m_iTeamNum() != iLocalTeam;
		if (bEnemy && !(iTeams & Vars::Aimbot::Draw::TrajectoryTeamsEnum::Enemies))
			continue;
		if (!bEnemy && !(iTeams & Vars::Aimbot::Draw::TrajectoryTeamsEnum::Teammates))
			continue;

		// Lead time: shot travel time to the target's current position plus the
		// backtrack latency, tuned by the scale/offset options and capped.
		const float flTravel = flSpeed > 1.f ? vShoot.DistTo(pPlayer->GetShootPos()) / flSpeed : 0.f;
		float flLead = (flLatency + flTravel) * flScale + flOffset;
		flLead = std::clamp(flLead, 0.f, flMaxTime);

		const int iTicks = TIME_TO_TICKS(flLead);
		if (iTicks < 1)
			continue;

		MoveStorage tStorage = {};
		if (!F::MoveSim.Initialize(pPlayer, tStorage))
		{
			F::MoveSim.Restore(tStorage);
			continue;
		}
		for (int i = 0; i < iTicks && !tStorage.m_bFailed; i++)
			F::MoveSim.RunTick(tStorage);
		const Vec3 vPredicted = tStorage.m_vPredictedOrigin;
		F::MoveSim.Restore(tStorage);

		const Vec3 vDelta = vPredicted - pPlayer->m_vecOrigin();
		if (vDelta.Length2D() < flMinDist)
			continue;

		m_mGhosts[pPlayer->entindex()] = { vDelta };
	}
}

bool CTrajectoryGhost::GetDelta(int iIndex, Vec3& vDelta)
{
	auto it = m_mGhosts.find(iIndex);
	if (it == m_mGhosts.end())
		return false;
	vDelta = it->second.m_vDelta;
	return true;
}

// enemy/team already filtered at Store time; here we re-check the frame-time
// concerns: the entity still has a ghost, is on screen and (optionally) inside
// the crosshair FOV cone.
bool CTrajectoryGhost::ShouldRender(CTFPlayer* pLocal, CTFPlayer* pEntity, Vec3& vDelta)
{
	if (!pLocal || !pEntity || !pEntity->IsPlayer() || pEntity->IsDormant() || !pEntity->IsAlive() || !pEntity->ShouldDraw())
		return false;
	if (!GetDelta(pEntity->entindex(), vDelta))
		return false;
	if (!SDK::IsOnScreen(pEntity))
		return false;

	if (const float flFOV = Vars::Aimbot::Draw::TrajectoryFOV.Value)
	{
		const Vec3 vEye = pLocal->GetEyePosition();
		Vec3 vForward; Math::AngleVectors(I::EngineClient->GetViewAngles(), &vForward);
		const Vec3 vDir = (pEntity->GetShootPos() - vEye).Normalized();
		const float flAngle = Math::Rad2Deg(acosf(std::clamp(vForward.Dot(vDir), -1.f, 1.f)));
		if (flAngle > flFOV)
			return false;
	}
	return true;
}

void CTrajectoryGhost::RenderMain()
{
	// Mirror the DrawModelExecute hook's own guards: under any of these the hook
	// early-returns to the engine draw, so triggering pEntity->DrawModel here
	// would render an extra untranslated real model instead of the ghost.
	if (!Active() || m_mGhosts.empty()
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

	for (auto& [iIndex, tGhost] : m_mGhosts)
	{
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

		for (auto& [sName, tColor] : vLayers)
		{
			// Per-layer flat/distance colour, overridden by the health gradient
			// when the team's Health colour is enabled.
			const Color_t cFill = bHealth ? EvalGlowStops(tGlow.Stops, flHealthFrac) : tColor.GetColor(flDistance);

			auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));
			F::Materials.SetColor(pMaterial, cFill);
			I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);
			I::RenderView->SetColorModulation(cFill);
			I::RenderView->SetBlend(cFill.a / 255.f);

			m_bActiveIgnoreZ = tColor.IgnoreZ || Vars::Aimbot::Draw::TrajectoryIgnoreZ.Value;
			m_bRendering = true;
			pPlayer->DrawModel(STUDIO_RENDER);
			m_bRendering = false;
		}

		pPlayer->m_flInvisibility() = flOldInvis;
	}

	m_pActive = nullptr;
	I::RenderView->SetColorModulation(tOldColor);
	I::RenderView->SetBlend(flOldBlend);
	I::ModelRender->ForcedMaterialOverride(pOldMaterial, iOldOverride);
}

// Re-entered through the model-draw hook while m_bRendering is set: draw the
// model with its live render bones translated to the predicted position.
void CTrajectoryGhost::RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];

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

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	if (Vars::Aimbot::Draw::TrajectoryBehindOnly.Value && pRenderContext)
	{
		// Peek-out: stencil-mark the target's real model (colour writes off, so it
		// stays invisible), then draw the ghost only where that mask is set, so it
		// is visible solely where the real model covers it on screen.
		pRenderContext->ClearBuffers(false, false, true);

		pRenderContext->OverrideColorWriteEnable(true, false);
		pRenderContext->SetStencilEnable(true);
		pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_ALWAYS);
		pRenderContext->SetStencilPassOperation(STENCILOPERATION_REPLACE);
		pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
		pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
		pRenderContext->SetStencilReferenceValue(1);
		pRenderContext->SetStencilWriteMask(0xFF);
		pRenderContext->SetStencilTestMask(0x0);
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
		pRenderContext->OverrideColorWriteEnable(false, false);

		pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_EQUAL);
		pRenderContext->SetStencilPassOperation(STENCILOPERATION_KEEP);
		pRenderContext->SetStencilReferenceValue(1);
		pRenderContext->SetStencilWriteMask(0x0);
		pRenderContext->SetStencilTestMask(0xFF);
		pRenderContext->DepthRange(0.f, 0.2f);
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, s_aBones);
		pRenderContext->DepthRange(0.f, 1.f);

		pRenderContext->SetStencilEnable(false);
		return;
	}

	if (m_bActiveIgnoreZ && pRenderContext)
		pRenderContext->DepthRange(0.f, 0.2f);
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, s_aBones);
	if (m_bActiveIgnoreZ && pRenderContext)
		pRenderContext->DepthRange(0.f, 1.f);
}
