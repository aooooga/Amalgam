#include "Chams.h"

#include "../Groups/Groups.h"
#include "../Materials/Materials.h"
#include "../FakeAngle/FakeAngle.h"
#include "../FlexFOV/FlexFOV.h"
#include "../../Backtrack/Backtrack.h"

void CChams::Begin()
{
	m_tOriginalColor = I::RenderView->GetColorModulation();
	m_flOriginalBlend = I::RenderView->GetBlend();
	I::ModelRender->GetMaterialOverride(&m_pOriginalMaterial, &m_iOriginalOverride);
}
void CChams::End()
{
	I::RenderView->SetColorModulation(m_tOriginalColor);
	I::RenderView->SetBlend(m_flOriginalBlend);
	I::ModelRender->ForcedMaterialOverride(m_pOriginalMaterial, m_iOriginalOverride);
}

void CChams::DrawModel(CBaseEntity* pEntity, const Chams_t& tChams, IMatRenderContext* pRenderContext, int iModel, bool bTwoModel)
{
	// Held-weapon entities follow each layer's Weapon body-part bit; when no
	// layer includes it, leave the original weapon model untouched entirely
	// (no suppression, no cham).
	const bool bWeaponEntity = pEntity->IsBaseCombatWeapon();
	if (bWeaponEntity)
	{
		const auto HasWeapon = [](const std::vector<std::pair<std::string, MaterialColor_t>>& v)
		{
			return std::ranges::any_of(v, [](const auto& tPair) { return tPair.second.BodyParts & BODYPART_WEAPON; });
		};
		if (!HasWeapon(tChams.Visible) && !HasWeapon(tChams.Occluded))
			return;
	}

	if (!m_iFlags && iModel == ModelEnum::Visible)
		m_mEntities[pEntity->entindex()];

	// Local player's distance to the entity, for distance-based material colors.
	// Resolved lazily: this runs per entity per render pass (x6 under FlexFOV's cube
	// rig), while distance colors are off by default, so the GetLocal virtual calls
	// and the sqrt would otherwise be spent for a value nothing reads. -1 means
	// "unknown", which is what GetColor already falls back to.
	float flDistance = -1.f;
	bool bDistanceValid = false;
	const auto GetDistance = [&]() -> float
	{
		if (!bDistanceValid)
		{
			bDistanceValid = true;
			if (auto pLocal = H::Entities.GetLocal())
				flDistance = pLocal->GetAbsOrigin().DistTo(pEntity->GetAbsOrigin());
		}
		return flDistance;
	};
	// Matches GetColor exactly: with the gradient off it resolves to the flat color
	// regardless of distance, so there is no reason to go and measure one.
	const auto ResolveColor = [&](const MaterialColor_t& tColor) -> Color_t
	{
		return tColor.Distance.Enabled ? tColor.GetColor(GetDistance()) : tColor.Color;
	};

	bool bOccluded = !tChams.Occluded.empty();
	bool bSame = tChams.Visible == tChams.Occluded;
	bTwoModel &= bOccluded && !bSame;

	// Passes that draw nothing return below without touching render state, so
	// Begin()'s 3 virtual snapshot calls would be pure waste. Skip it up front:
	// non-two-model Visible with Visible==Occluded, and non-two-model Occluded
	// with no occluded layers, both early-return before any draw or End().
	if (!bTwoModel && (iModel == ModelEnum::Visible ? bSame : !bOccluded))
		return;

	Begin();
	switch (iModel)
	{
	case ModelEnum::Visible:
	{
		if (!bTwoModel)
		{
			if (bSame)
				return;
		}
		else
		{
			pRenderContext->SetStencilEnable(true);
			pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_ALWAYS);
			pRenderContext->SetStencilPassOperation(STENCILOPERATION_REPLACE);
			pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilReferenceValue(1);
			pRenderContext->SetStencilWriteMask(0xFF);
			pRenderContext->SetStencilTestMask(0x0);
		}

		auto& vMaterials = tChams.GetVisible();
		for (auto& [sName, tColor] : vMaterials)
		{
			if (bWeaponEntity && !(tColor.BodyParts & BODYPART_WEAPON))
				continue;

			auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));

			F::Materials.SetColor(pMaterial, ResolveColor(tColor));
			I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);
			if (pMaterial)
			{
				if (pMaterial->m_bInvertCull)
					pRenderContext->CullMode(MATERIAL_CULLMODE_CW);
				if (pMaterial->m_bBlockOccluded)
					pRenderContext->SetStencilZFailOperation(STENCILOPERATION_REPLACE);
			}

			// Ignore Z: compress the layer into the near depth range so it
			// passes the depth test over the world and the model itself.
			if (tColor.IgnoreZ)
				pRenderContext->DepthRange(0.f, 0.2f);

			// Fullbright Original: no override material (the model draws its own).
			// Suppressing engine lighting alone just leaves studiorender on stale
			// lighting state, so explicitly feed it a full-white ambient cube and
			// no local lights for these draws.
			const bool bFullbright = !pMaterial && tColor.Fullbright;
			if (bFullbright)
			{
				static const Vector vWhiteCube[6] = { {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1} };
				I::ModelRender->SuppressEngineLighting(true);
				I::StudioRender->SetAmbientLightColors(vWhiteCube);
				I::StudioRender->SetLocalLights(0, nullptr);
			}

			m_iActiveBodyParts = tColor.BodyParts;
			m_bRendering = true;
			pEntity->DrawModel(STUDIO_RENDER);
			// Weapon option: also cham the held weapon with this material and
			// suppress its original model, like the player's.
			if (tColor.BodyParts & BODYPART_WEAPON && pEntity->IsPlayer())
			{
				if (auto pWeapon = pEntity->As<CTFPlayer>()->m_hActiveWeapon()->As<CTFWeaponBase>())
				{
					if (iModel == ModelEnum::Visible)
						m_mEntities[pWeapon->entindex()];
					pWeapon->DrawModel(STUDIO_RENDER);
				}
			}
			m_bRendering = false;
			m_iActiveBodyParts = BODYPART_ALL;

			if (bFullbright)
				I::ModelRender->SuppressEngineLighting(false);

			if (tColor.IgnoreZ)
				pRenderContext->DepthRange(0.f, 1.f);

			if (pMaterial)
			{
				if (pMaterial->m_bInvertCull)
					pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);
				if (pMaterial->m_bBlockOccluded)
					pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
			}
		}

		if (bTwoModel)
			pRenderContext->SetStencilEnable(false);
		break;
	}
	case ModelEnum::Occluded:
	{
		if (!bTwoModel)
		{
			if (!bOccluded)
				return;
		}
		else
		{
			pRenderContext->SetStencilEnable(true);
			pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_EQUAL);
			pRenderContext->SetStencilPassOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
			pRenderContext->SetStencilReferenceValue(0);
			pRenderContext->SetStencilWriteMask(0x0);
			pRenderContext->SetStencilTestMask(0xFF);
		}
		pRenderContext->DepthRange(0.f, 0.2f);

		auto& vMaterials = tChams.GetOccluded();
		for (auto& [sName, tColor] : vMaterials)
		{
			if (bWeaponEntity && !(tColor.BodyParts & BODYPART_WEAPON))
				continue;

			auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));

			F::Materials.SetColor(pMaterial, ResolveColor(tColor));
			I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);
			if (pMaterial && pMaterial->m_bInvertCull)
				pRenderContext->CullMode(MATERIAL_CULLMODE_CW);

			// Fullbright Original: see the visible pass.
			const bool bFullbright = !pMaterial && tColor.Fullbright;
			if (bFullbright)
			{
				static const Vector vWhiteCube[6] = { {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1} };
				I::ModelRender->SuppressEngineLighting(true);
				I::StudioRender->SetAmbientLightColors(vWhiteCube);
				I::StudioRender->SetLocalLights(0, nullptr);
			}

			m_iActiveBodyParts = tColor.BodyParts;
			m_bRendering = true;
			pEntity->DrawModel(STUDIO_RENDER);
			// Weapon option: also cham the held weapon with this material and
			// suppress its original model, like the player's.
			if (tColor.BodyParts & BODYPART_WEAPON && pEntity->IsPlayer())
			{
				if (auto pWeapon = pEntity->As<CTFPlayer>()->m_hActiveWeapon()->As<CTFWeaponBase>())
				{
					if (iModel == ModelEnum::Visible)
						m_mEntities[pWeapon->entindex()];
					pWeapon->DrawModel(STUDIO_RENDER);
				}
			}
			m_bRendering = false;
			m_iActiveBodyParts = BODYPART_ALL;

			if (bFullbright)
				I::ModelRender->SuppressEngineLighting(false);

			if (pMaterial && pMaterial->m_bInvertCull)
				pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);
		}

		if (bTwoModel)
			pRenderContext->SetStencilEnable(false);
		pRenderContext->DepthRange(0.f, 1.f);
	}
	}
	End();
}



// Entity whose hitboxes the crosshair is on: an eye trace along the aim
// direction, exactly like a fired bullet (same mask/filter), so it respects
// walls (a blocked crosshair returns nothing) and hits at hitbox precision.
// Screen-center = camera forward, so this is correct under FlexFOV too.
int CChams::GetCrosshairTarget(CTFPlayer* pLocal)
{
	if (!pLocal || !pLocal->IsAlive())
		return 0;

	const Vec3 vEyePos = pLocal->GetShootPos();
	Vec3 vForward; Math::AngleVectors(I::EngineClient->GetViewAngles(), &vForward);

	CGameTrace trace = {};
	CTraceFilterHitscan filter = {};
	filter.pSkip = pLocal;
	SDK::Trace(vEyePos, vEyePos + vForward * 8192.f, MASK_SHOT | CONTENTS_GRATE, &filter, &trace);

	if (auto pEnt = trace.m_pEnt; pEnt && pEnt->IsPlayer())
	{
		switch (trace.hitgroup)
		{
		case HITGROUP_HEAD: m_iTargetedPart = BODYPART_HEAD; break;
		case HITGROUP_CHEST: case HITGROUP_STOMACH: m_iTargetedPart = BODYPART_SPINE; break;
		case HITGROUP_LEFTARM: m_iTargetedPart = BODYPART_LEFT_ARM; break;
		case HITGROUP_RIGHTARM: m_iTargetedPart = BODYPART_RIGHT_ARM; break;
		case HITGROUP_LEFTLEG: m_iTargetedPart = BODYPART_LEFT_LEG; break;
		case HITGROUP_RIGHTLEG: m_iTargetedPart = BODYPART_RIGHT_LEG; break;
		default: m_iTargetedPart = 0; break;
		}
		return pEnt->entindex();
	}
	m_iTargetedPart = 0;
	return 0;
}

// Targeted-material layers that apply to the body part under the crosshair;
// an unknown part matches every layer.
static std::vector<std::pair<std::string, MaterialColor_t>> FilterTargetLayers(const std::vector<std::pair<std::string, MaterialColor_t>>& vLayers, int iPart)
{
	std::vector<std::pair<std::string, MaterialColor_t>> vOut;
	for (auto& tPair : vLayers)
	{
		if (!iPart || tPair.second.BodyParts & iPart)
			vOut.push_back(tPair);
	}
	return vOut;
}

void CChams::Store(CTFPlayer* pLocal)
{
	m_vEntities.clear();
	if (!pLocal || !F::Groups.GroupsActive())
		return;

	for (auto& [pEntity, pGroup] : F::Groups.GetGroup())
	{
		if (pEntity->IsDormant() || !pEntity->ShouldDraw())
			continue;

		const bool bChams = pGroup->m_tChams();
		// Targeted material candidates: any entity whose group has it enabled. Which
		// one is actually under the crosshair is resolved per frame in UpdateTarget(),
		// after interpolation, so the highlight matches the rendered model.
		const bool bTarget = pGroup->m_tTargetChams(false);
		if ((bChams || bTarget) && !pEntity->IsWearableVM()
			&& SDK::IsOnScreen(pEntity, pEntity->IsBaseCombatWeapon() || pEntity->IsWearable()))
			m_vEntities.emplace_back(pEntity, bChams ? &pGroup->m_tChams : nullptr, 0, bTarget ? &pGroup->m_tTargetChams : nullptr);

		if (pEntity->IsPlayer() && pEntity != pLocal && pGroup->m_iBacktrack & BacktrackEnum::Enabled && pGroup->m_tBacktrackChams(false)
			&& (F::Backtrack.GetFakeLatency() || F::Backtrack.GetFakeInterp() > G::Lerp || F::Backtrack.GetWindow()))
		{	// backtrack
			auto pWeapon = H::Entities.GetWeapon();
			if (pWeapon && (pGroup->m_iBacktrack & BacktrackEnum::Always || G::PrimaryWeaponType != EWeaponType::PROJECTILE))
			{
				bool bShowFriendly = false, bShowEnemy = true;
				if (G::PrimaryWeaponType == EWeaponType::MELEE && SDK::AttribHookValue(0, "speed_buff_ally", pWeapon) > 0)
					bShowFriendly = true;
				else if (pWeapon->GetWeaponID() == TF_WEAPON_MEDIGUN)
					bShowFriendly = true, bShowEnemy = false;

				if (bShowEnemy && pEntity->m_iTeamNum() != pLocal->m_iTeamNum() || bShowFriendly && pEntity->m_iTeamNum() == pLocal->m_iTeamNum())
					m_vEntities.emplace_back(pEntity, &pGroup->m_tBacktrackChams, pGroup->m_iBacktrack);
			}
		}
	}

	Group_t* pGroup = nullptr;
	if (F::FakeAngle.bDrawChams && F::FakeAngle.bBonesSetup
		&& F::Groups.GetGroup(TargetsEnum::FakeAngle, pGroup) && pGroup->m_tChams(false))
	{	// fakeangle
		m_vEntities.emplace_back(pLocal, &pGroup->m_tChams, 1);
	}
}

// Resolve the crosshair target and fix up the original-model suppression set
// before the scene renders. The trace runs here, per frame, rather than in
// Store(): Store() runs at FRAME_NET_UPDATE_END where entities sit at their
// latest networked positions, but models render interpolated (~cl_interp
// behind), so a tick-time trace visibly desyncs from moving players.
// FRAME_RENDER_START runs after interpolation, so the trace hits the hitbox
// pose this frame actually renders.
//
// It must NOT run in RenderMain(): that draws after the scene, so the
// suppression set (m_mEntities) it rebuilds only takes effect on the NEXT
// frame's scene pass. On the frame the targeted state flips, a target-chams-
// only entity (null m_pChams, registered only while targeted) would then have
// its original model suppressed with no cham drawn over it - and with the
// trace flickering through hitbox gaps as the crosshair settles on a player,
// that repeats for several frames: the "no material" flash. Reconciling
// membership here keeps the scene's suppression in lockstep with what
// RenderMain() draws this same frame.
void CChams::UpdateTarget()
{
	m_iTargetedEntity = GetCrosshairTarget(H::Entities.GetLocal());

	for (auto& tInfo : m_vEntities)
	{
		// Entities with regular chams (or backtrack/fakeangle flags) draw every
		// frame regardless of targeting - their membership never flips.
		if (tInfo.m_iFlags || tInfo.m_pChams || !tInfo.m_pTargetChams)
			continue;

		const int iIndex = tInfo.m_pEntity->entindex();
		const bool bDraws = m_iTargetedEntity && iIndex == m_iTargetedEntity
			&& std::ranges::any_of(tInfo.m_pTargetChams->Visible, [&](const auto& tPair) { return !m_iTargetedPart || tPair.second.BodyParts & m_iTargetedPart; });
		if (bDraws)
			m_mEntities[iIndex];
		else
			m_mEntities.erase(iIndex);
	}
}

// A visible set that reproduces the engine's own draw exactly: one "Original"
// layer, untinted, opaque, no fullbright / Ignore Z / body-part subset. For
// these the suppress-and-redraw round trip is pure waste - the engine's scene
// draw IS the visible cham (see OriginalChamsOptimization).
static bool IsPlainOriginal(const std::vector<std::pair<std::string, MaterialColor_t>>& vVisible)
{
	if (vVisible.size() != 1)
		return false;

	auto& [sName, tColor] = vVisible.front();
	return FNV1A::Hash32(sName.c_str()) == FNV1A::Hash32Const("Original")
		&& !tColor.Fullbright && !tColor.IgnoreZ
		&& (tColor.BodyParts & BODYPART_ALL) == BODYPART_ALL
		&& !tColor.Distance.Enabled
		&& tColor.Color == Color_t(255, 255, 255, 255);
}

void CChams::RenderMain()
{
	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext)
		return;

	// Whether this pass' scene carries live passthrough stencil marks (cleared
	// at scene start by the ViewDrawScene hook, written by the wrapped engine
	// draws); they must survive here for the occluded pass to test against.
	const bool bSceneMarks = m_bScenePassthrough;
	m_bScenePassthrough = false;

	m_mEntities.clear();
	if (m_vEntities.empty())
	{
		if (bSceneMarks)
			pRenderContext->ClearBuffers(false, false, true);
		return;
	}

	if (!bSceneMarks)
		pRenderContext->ClearBuffers(false, false, true);

	// The crosshair-targeted entity draws with its group's targeted material on
	// both the visible and occluded passes (a solid, always-on-top highlight)
	// instead of its regular chams. Built once - only one entity is ever targeted.
	Chams_t tTarget = {};
	auto IsTargeted = [&](const ChamsInfo_t& tInfo)
	{
		return tInfo.m_pTargetChams && m_iTargetedEntity && tInfo.m_pEntity->entindex() == m_iTargetedEntity;
	};
	for (auto& tInfo : m_vEntities)
	{
		if (IsTargeted(tInfo))
		{
			// Only the layers assigned to the body part under the crosshair.
			// Occluded stays empty: a crosshair target is visible by definition,
			// and the visible pass honors each layer's Ignore Z (the occluded
			// pass would force always-on-top regardless of it).
			tTarget.Visible = FilterTargetLayers(tInfo.m_pTargetChams->Visible, m_iTargetedPart);
			break;
		}
	}
	auto GetChams = [&](const ChamsInfo_t& tInfo) -> const Chams_t*
	{
		// No layer for the targeted part: fall back to the regular chams.
		return IsTargeted(tInfo) && !tTarget.Visible.empty() ? &tTarget : tInfo.m_pChams;
	};

	// Passthrough registration: with occluded layers the engine draw must
	// stencil-mark its visible pixels (mapped value true -> the hook wraps it),
	// and the held weapon draws through the same wrap when the layer covers it
	// (matching the redraw path, which draws the weapon alongside its owner).
	// Without occluded layers nothing tests the mask, so the entity isn't
	// registered at all and the engine draw needs no help.
	auto RegisterPassthrough = [&](const ChamsInfo_t& tInfo, const Chams_t& tChams)
	{
		if (tChams.Occluded.empty())
			return;

		m_mEntities[tInfo.m_pEntity->entindex()] = true;
		m_bScenePassthrough = true;
		if (tChams.Visible.front().second.BodyParts & BODYPART_WEAPON && tInfo.m_pEntity->IsPlayer())
		{
			if (auto pWeapon = tInfo.m_pEntity->As<CTFPlayer>()->m_hActiveWeapon()->As<CTFWeaponBase>())
				m_mEntities[pWeapon->entindex()] = true;
		}
	};

	for (int iModel : { ModelEnum::Visible, ModelEnum::Occluded })
	{
		for (auto& tInfo : m_vEntities)
		{
			const Chams_t* pChams = GetChams(tInfo);
			if (!pChams)
				continue;

			// Plain-Original visible set: the engine's scene draw already
			// rendered exactly this image, so skip the visible redraw (and the
			// suppression that forces it). The Visible == Occluded case keeps
			// the redraw path: it draws the occluded set always-on-top instead
			// and must keep suppressing the original.
			const bool bPassthrough = Vars::Misc::Game::OriginalChamsOptimization.Value
				&& !tInfo.m_iFlags && IsPlainOriginal(pChams->Visible)
				&& (pChams->Occluded.empty() || pChams->Occluded != pChams->Visible);

			// FlexFOV face capture: skip entities this face can't see (the same
			// angular cull the glow face pass uses) - at wide fov half the cham
			// set is behind any given face, and each skip saves 2+ model draws.
			// Still register the entity (DrawModel's side effect) so the
			// original-model suppression set stays identical across passes.
			// This MUST stay below the pChams gate: registering an entity chams
			// never redraws (null pChams, e.g. target-chams-only groups) would
			// suppress its original model everywhere and turn it invisible.
			if (F::FlexFOV.m_bDrawing && !F::FlexFOV.FaceCanSee(tInfo.m_pEntity->GetAbsOrigin()))
			{
				if (!tInfo.m_iFlags && iModel == ModelEnum::Visible)
				{
					if (bPassthrough)
						RegisterPassthrough(tInfo, *pChams);
					else
						m_mEntities[tInfo.m_pEntity->entindex()];
				}
				continue;
			}

			if (bPassthrough)
			{
				if (iModel == ModelEnum::Visible)
					RegisterPassthrough(tInfo, *pChams);
				else if (!pChams->Occluded.empty())
					DrawModel(tInfo.m_pEntity, *pChams, pRenderContext, iModel, true);
				continue;
			}

			if (!tInfo.m_iFlags)
				DrawModel(tInfo.m_pEntity, *pChams, pRenderContext, iModel, true);
			else
			{
				m_iFlags = tInfo.m_iFlags;

				auto pPlayer = tInfo.m_pEntity->As<CTFPlayer>();
				const float flOldInvisibility = pPlayer->m_flInvisibility();
				pPlayer->m_flInvisibility() = 0.f;
				DrawModel(tInfo.m_pEntity, *pChams, pRenderContext, iModel, true);
				pPlayer->m_flInvisibility() = flOldInvisibility;

				m_iFlags = false;
			}
		}
	}

	pRenderContext->ClearBuffers(false, false, true);
}

void CChams::RenderBacktrack(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo)
{
	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext)
		return;

	auto pEntity = I::ClientEntityList->GetClientEntity(pInfo.entity_index)->As<CTFPlayer>();
	if (!pEntity || !pEntity->IsPlayer())
		return;

	std::vector<TickRecord*> vRecords = {};
	if (!F::Backtrack.GetRecords(pEntity, vRecords))
		return;
	vRecords = F::Backtrack.GetValidRecords(vRecords);
	if (!vRecords.size())
		return;

	bool bDrawLast = m_iFlags & BacktrackEnum::Last;
	bool bDrawFirst = m_iFlags & BacktrackEnum::First;

	float flOriginalBlend = I::RenderView->GetBlend();
	auto fDrawModel = [&](Vec3& vOrigin, const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld, float flBlend)
	{
		if (!SDK::IsOnScreen(pEntity, vOrigin))
			return;

		I::RenderView->SetBlend(flBlend * flOriginalBlend);
		static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	};
	if (!bDrawLast && !bDrawFirst)
	{
		for (auto pRecord : vRecords)
		{
			if (float flBlend = Math::RemapVal(pEntity->GetAbsOrigin().DistToSqr(pRecord->m_vOrigin), 1.f, 576.f, 0.f, 1.f))
				fDrawModel(pRecord->m_vOrigin, pState, pInfo, pRecord->m_aBones, flBlend);
		}
	}
	else
	{
		if (bDrawLast)
		{
			auto pRecord = vRecords.back();
			if (float flBlend = Math::RemapVal(pEntity->GetAbsOrigin().DistToSqr(pRecord->m_vOrigin), 1.f, 576.f, 0.f, 1.f))
				fDrawModel(pRecord->m_vOrigin, pState, pInfo, pRecord->m_aBones, flBlend);
		}
		if (bDrawFirst)
		{
			auto pRecord = vRecords.front();
			if (float flBlend = Math::RemapVal(pEntity->GetAbsOrigin().DistToSqr(pRecord->m_vOrigin), 1.f, 576.f, 0.f, 1.f))
				fDrawModel(pRecord->m_vOrigin, pState, pInfo, pRecord->m_aBones, flBlend);
		}
	}
	I::RenderView->SetBlend(flOriginalBlend);
}
void CChams::RenderFakeAngle(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo)
{
	static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, F::FakeAngle.aBones);
}
// Classify every bone of a player model into an EBodyParts bit by name, children
// inheriting from their parent (covers fingers, toes, weapon/attachment bones).
// Bones that classify to nothing (root helpers etc.) always draw.
static int ClassifyBone(const char* sName, int iParentPart)
{
	std::string sLower = sName;
	std::transform(sLower.begin(), sLower.end(), sLower.begin(), ::tolower);
	const auto Has = [&](const char* s) { return sLower.find(s) != std::string::npos; };
	const bool bLeft = sLower.ends_with("_l");

	if (Has("head") || Has("neck"))
		return BODYPART_HEAD;
	if (Has("spine") || Has("pelvis"))
		return BODYPART_SPINE;
	if (Has("arm") || Has("hand") || Has("collar"))
		return bLeft ? BODYPART_LEFT_ARM : BODYPART_RIGHT_ARM;
	if (Has("hip") || Has("knee") || Has("foot") || Has("toe"))
		return bLeft ? BODYPART_LEFT_LEG : BODYPART_RIGHT_LEG;
	return iParentPart;
}

static matrix3x4 s_aFilteredBones[MAXSTUDIOBONES];

matrix3x4* CChams::ApplyBodyPartFilter(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	const int iParts = m_iActiveBodyParts & BODYPART_ALL;
	if (iParts == BODYPART_ALL)
		return nullptr;

	auto pHdr = pState.m_pStudioHdr;
	if (!pHdr || pHdr->numbones < 1 || pHdr->numbones > MAXSTUDIOBONES)
		return nullptr;

	auto pEntity = I::ClientEntityList->GetClientEntity(pInfo.entity_index)->As<CTFPlayer>();
	if (!pEntity || !pEntity->IsPlayer())
		return nullptr;

	// Per-model bone->part table, cached by studiohdr (checksum-validated in
	// case the pointer is reused after a model reload).
	static std::unordered_map<const studiohdr_t*, std::pair<int, std::vector<uint8_t>>> mPartCache;
	auto& [iChecksum, vParts] = mPartCache[pHdr];
	if (iChecksum != pHdr->checksum || vParts.size() != size_t(pHdr->numbones))
	{
		iChecksum = pHdr->checksum;
		vParts.assign(pHdr->numbones, 0);
		for (int i = 0; i < pHdr->numbones; i++)
		{
			auto pBone = pHdr->pBone(i);
			int iParentPart = pBone->parent >= 0 && pBone->parent < i ? vParts[pBone->parent] : 0;
			vParts[i] = uint8_t(ClassifyBone(pBone->pszName(), iParentPart));
		}
	}

	// The engine usually calls DrawModelExecute without an explicit bone array;
	// pull the current pose ourselves in that case.
	if (pBoneToWorld)
		memcpy(s_aFilteredBones, pBoneToWorld, sizeof(matrix3x4) * pHdr->numbones);
	else if (!pEntity->SetupBones(s_aFilteredBones, MAXSTUDIOBONES, BONE_USED_BY_ANYTHING, I::GlobalVars->curtime))
		return nullptr;

	// Collapse deselected parts: zero the rotation/scale but keep the bone's
	// position, so skinned vertices degenerate to a point in place instead of
	// spiking toward the world origin.
	for (int i = 0; i < pHdr->numbones; i++)
	{
		if (!vParts[i] || iParts & vParts[i])
			continue;

		auto& m = s_aFilteredBones[i];
		for (int r = 0; r < 3; r++)
			m[r][0] = m[r][1] = m[r][2] = 0.f;
	}
	return s_aFilteredBones;
}

void CChams::RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	if (!m_iFlags)
	{
		if (auto pFiltered = ApplyBodyPartFilter(pState, pInfo, pBoneToWorld))
			pBoneToWorld = pFiltered;

		static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	}
	else
	{
		if (pInfo.entity_index != I::EngineClient->GetLocalPlayer())
			RenderBacktrack(pState, pInfo);
		else
			RenderFakeAngle(pState, pInfo);
	}
}

bool CChams::RenderViewmodel(void* rcx, int flags, int* iReturn)
{
	if (!F::Groups.GroupsActive())
		return false;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext)
		return false;

	Group_t* pGroup = nullptr;
	if (!F::Groups.GetGroup(reinterpret_cast<CBaseAnimating*>(rcx)->IsValid() ? TargetsEnum::ViewmodelHands : TargetsEnum::ViewmodelWeapon, pGroup) || !pGroup->m_tChams(true))
		return false;

	Begin();
	for (auto& [sName, tColor] : pGroup->m_tChams.Visible)
	{
		auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));

		F::Materials.SetColor(pMaterial, tColor.GetColor(0.f)); // viewmodel = distance 0
		I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);

		bool bFlip = pMaterial && pMaterial->m_bInvertCull ? !G::FlipViewmodels : G::FlipViewmodels;
		pRenderContext->CullMode(bFlip ? MATERIAL_CULLMODE_CW : MATERIAL_CULLMODE_CCW);

		static auto CBaseAnimating_InternalDrawModel = U::Hooks.m_mHooks["CBaseAnimating_InternalDrawModel"];
		*iReturn = CBaseAnimating_InternalDrawModel->Call<int>(rcx, flags);
	}
	pRenderContext->CullMode(G::FlipViewmodels ? MATERIAL_CULLMODE_CW : MATERIAL_CULLMODE_CCW);
	End();

	return true;
}
bool CChams::RenderViewmodel(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	if (!F::Groups.GroupsActive())
		return false;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext)
		return false;

	Group_t* pGroup = nullptr;
	if (!F::Groups.GetGroup(TargetsEnum::ViewmodelWeapon, pGroup) || !pGroup->m_tChams(true))
		return false;

	Begin();
	for (auto& [sName, tColor] : pGroup->m_tChams.Visible)
	{
		auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));

		F::Materials.SetColor(pMaterial, tColor.GetColor(0.f)); // viewmodel = distance 0
		I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);

		bool bFlip = pMaterial && pMaterial->m_bInvertCull ? !G::FlipViewmodels : G::FlipViewmodels;
		pRenderContext->CullMode(bFlip ? MATERIAL_CULLMODE_CW : MATERIAL_CULLMODE_CCW);

		static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	}
	pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);
	End();

	return true;
}