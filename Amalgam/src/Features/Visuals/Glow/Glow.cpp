#include "Glow.h"

#include "../Groups/Groups.h"
#include "../Materials/Materials.h"
#include "../FakeAngle/FakeAngle.h"
#include "../FlexFOV/FlexFOV.h"
#include "../../Backtrack/Backtrack.h"
#include "../../Aimbot/TrajectoryGhost/TrajectoryGhost.h"

#include <algorithm>
#include <cmath>

void CGlow::Begin()
{
	m_tOriginalColor = I::RenderView->GetColorModulation();
	m_flOriginalBlend = I::RenderView->GetBlend();
	I::ModelRender->GetMaterialOverride(&m_pOriginalMaterial, &m_iOriginalOverride);

	I::RenderView->SetBlend(0.f);
	I::RenderView->SetColorModulation(1.f, 1.f, 1.f);
	I::ModelRender->ForcedMaterialOverride(m_pMatGlowColor);
}
void CGlow::End()
{
	I::RenderView->SetColorModulation(m_tOriginalColor);
	I::RenderView->SetBlend(m_flOriginalBlend);
	I::ModelRender->ForcedMaterialOverride(m_pOriginalMaterial, m_iOriginalOverride);
}

void CGlow::StampStencilBegin(IMatRenderContext* pRenderContext)
{
	pRenderContext->SetStencilEnable(true);
	pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_ALWAYS);
	pRenderContext->SetStencilPassOperation(STENCILOPERATION_REPLACE);
	pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilZFailOperation(STENCILOPERATION_REPLACE);
	pRenderContext->SetStencilReferenceValue(1);
	pRenderContext->SetStencilWriteMask(0xFF);
	pRenderContext->SetStencilTestMask(0x0);
}
void CGlow::StampStencilEnd(IMatRenderContext* pRenderContext)
{
	pRenderContext->SetStencilEnable(false);
}

void CGlow::FirstBegin(IMatRenderContext* pRenderContext)
{
	Begin();
	StampStencilBegin(pRenderContext);
}
void CGlow::FirstEnd(IMatRenderContext* pRenderContext)
{
	StampStencilEnd(pRenderContext);

	End();
}

void CGlow::SecondBegin(IMatRenderContext* pRenderContext)
{
	Begin();

	pRenderContext->PushRenderTargetAndViewport();
	pRenderContext->SetRenderTarget(m_pRenderBuffer1);
	pRenderContext->Viewport(0, 0, m_iBufW, m_iBufH);
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	// Scaled buffer: its own depth, cleared per batch, so the silhouettes are
	// through-walls by construction (like the FlexFOV face path). Shared buffer
	// (scale 1): the depth IS the scene's - never clear it.
	pRenderContext->ClearBuffers(true, !m_bBufShared, false);
}
void CGlow::SecondEnd(Glow_t tGlow, IMatRenderContext* pRenderContext, int w, int h)
{
	pRenderContext->PopRenderTargetAndViewport();

	// Buffer-space dims: the silhouette lives at bw x bh (screen * GlowResolution);
	// the halo blits at the bottom upscale it to the w x h screen.
	const int bw = m_iBufW, bh = m_iBufH;
	// RenderBuffer2 is half the silhouette buffer: BlurX downsamples into it,
	// BlurY upscales the blurred result back into RenderBuffer1 (which the halo
	// blits below sample). Halving the blur intermediate is imperceptible on an
	// already-blurred outline and cuts that pass's fill roughly in half.
	const int b2w = bw / 2, b2h = bh / 2;

	if (tGlow.Blur)
	{
		m_pBloomAmount->SetFloatValue(tGlow.Blur);

		pRenderContext->PushRenderTargetAndViewport();
		{
			pRenderContext->SetRenderTarget(m_pRenderBuffer2);
			pRenderContext->Viewport(0, 0, b2w, b2h);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatBlurX, 0, 0, b2w, b2h, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
			pRenderContext->SetRenderTarget(m_pRenderBuffer1);
			pRenderContext->Viewport(0, 0, bw, bh);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatBlurY, 0, 0, bw, bh, 0.f, 0.f, b2w - 1, b2h - 1, b2w, b2h);
		}
		pRenderContext->PopRenderTargetAndViewport();
	}

	pRenderContext->SetStencilEnable(true);
	pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_EQUAL);
	pRenderContext->SetStencilPassOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilReferenceValue(0);
	pRenderContext->SetStencilWriteMask(0x0);
	pRenderContext->SetStencilTestMask(0xFF);

	if (tGlow.Stencil)
	{
		// 4 cardinal offset blits only. The 4 diagonal corner blits were dropped:
		// each is a full-screen additive pass, and at the outline widths in use the
		// cardinal blits' overlap already fills the diagonals - the same trim the
		// FlexFOV face path relies on. This is the main fill-rate win for glow.
		int iSide = (tGlow.Stencil + 1) / 2.f;
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, -iSide, 0, w, h, 0.f, 0.f, bw - 1.f, bh - 1.f, bw, bh);
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, 0, -iSide, w, h, 0.f, 0.f, bw - 1.f, bh - 1.f, bw, bh);
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, iSide, 0, w, h, 0.f, 0.f, bw - 1.f, bh - 1.f, bw, bh);
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, 0, iSide, w, h, 0.f, 0.f, bw - 1.f, bh - 1.f, bw, bh);
	}
	if (tGlow.Blur)
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, 0, 0, w, h, 0.f, 0.f, bw - 1.f, bh - 1.f, bw, bh);

	pRenderContext->SetStencilEnable(false);

	End();
}

void CGlow::DrawModel(CBaseEntity* pEntity)
{
	m_bRendering = true;

	if (pEntity->IsPlayer())
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		float flOldInvisibility = pPlayer->m_flInvisibility();
		pPlayer->m_flInvisibility() = 0.f;
		pEntity->DrawModel(STUDIO_RENDER | STUDIO_NOSHADOWS);
		pPlayer->m_flInvisibility() = flOldInvisibility;
	}
	else
		pEntity->DrawModel(STUDIO_RENDER | STUDIO_NOSHADOWS);

	m_bRendering = false;
}



// Health fraction (0..1) for entities that have HP, -1 for those that don't;
// feeds the glow health gradient. Weapons and wearables use their carrier's HP
// so a player's whole silhouette (model + held weapon + cosmetics) matches.
static float GetHealthFraction(CBaseEntity* pEntity)
{
	if (pEntity->IsBaseCombatWeapon())
	{
		if (auto pOwner = pEntity->As<CBaseCombatWeapon>()->m_hOwner().Get())
			pEntity = reinterpret_cast<CBaseEntity*>(pOwner);
	}
	else if (pEntity->IsWearable())
	{
		if (auto pOwner = pEntity->m_hOwnerEntity().Get())
			pEntity = reinterpret_cast<CBaseEntity*>(pOwner);
	}

	float flHealth = -1.f, flMaxHealth = 0.f;
	if (pEntity->IsPlayer())
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		flHealth = pPlayer->m_iHealth();
		flMaxHealth = pPlayer->GetMaxHealth();
	}
	else if (pEntity->IsBuilding())
	{
		auto pBuilding = pEntity->As<CBaseObject>();
		flHealth = pBuilding->m_iHealth();
		flMaxHealth = pBuilding->m_iMaxHealth();
	}
	if (flHealth < 0.f || flMaxHealth <= 0.f)
		return -1.f;
	return std::clamp(flHealth / flMaxHealth, 0.f, 1.f);
}

// Glow color: the glow's own color option (not the group color), the health
// gradient for entities with HP when enabled, or the distance gradient
// evaluated at the local player's distance to the entity.
static Color_t GetGlowColor(CBaseEntity* pEntity, const Glow_t& tGlow, CTFPlayer* pLocal)
{
	const float flDistance = pLocal ? pLocal->GetAbsOrigin().DistTo(pEntity->GetAbsOrigin()) : -1.f;
	return tGlow.GetColor(GetHealthFraction(pEntity), flDistance);
}

void CGlow::Store(CTFPlayer* pLocal)
{
	m_mEntities.clear();
	if (!pLocal || !F::Groups.GroupsActive())
		return;

	for (auto& [pEntity, pGroup] : F::Groups.GetGroup())
	{
		if (pEntity->IsDormant() || !pEntity->ShouldDraw())
			continue;

		if (pGroup->m_tGlow() && !pEntity->IsWearableVM()
			&& SDK::IsOnScreen(pEntity, pEntity->IsBaseCombatWeapon() || pEntity->IsWearable()))
			m_mEntities[pGroup->m_tGlow].emplace_back(pEntity, GetGlowColor(pEntity, pGroup->m_tGlow, pLocal));

		if (pEntity->IsPlayer() && pEntity != pLocal && pGroup->m_iBacktrack & BacktrackEnum::Enabled && pGroup->m_tBacktrackGlow()
			&& (F::Backtrack.GetFakeLatency() || F::Backtrack.GetFakeInterp() > G::Lerp || F::Backtrack.GetWindow()))
		{
			auto pWeapon = H::Entities.GetWeapon();
			if (pWeapon && (pGroup->m_iBacktrack & BacktrackEnum::Always || G::PrimaryWeaponType != EWeaponType::PROJECTILE))
			{
				bool bShowFriendly = false, bShowEnemy = true;
				if (G::PrimaryWeaponType == EWeaponType::MELEE && SDK::AttribHookValue(0, "speed_buff_ally", pWeapon) > 0)
					bShowFriendly = true;
				else if (pWeapon->GetWeaponID() == TF_WEAPON_MEDIGUN)
					bShowFriendly = true, bShowEnemy = false;

				if (bShowEnemy && pEntity->m_iTeamNum() != pLocal->m_iTeamNum() || bShowFriendly && pEntity->m_iTeamNum() == pLocal->m_iTeamNum())
					m_mEntities[pGroup->m_tBacktrackGlow].emplace_back(pEntity, GetGlowColor(pEntity, pGroup->m_tBacktrackGlow, pLocal), pGroup->m_iBacktrack);
			}
		}
	}

	Group_t* pGroup = nullptr;
	if (F::FakeAngle.bDrawChams && F::FakeAngle.bBonesSetup
		&& F::Groups.GetGroup(TargetsEnum::FakeAngle, pGroup) && pGroup->m_tGlow())
	{	// fakeangle
		m_mEntities[pGroup->m_tGlow].emplace_back(pLocal, GetGlowColor(pLocal, pGroup->m_tGlow, pLocal), 1);
	}

	if (F::TrajectoryGhost.Active())
	{	// trajectory ghost glow (translated bones handled in RenderTrajectory);
		// enemy and team each carry a full Glow_t (shape + flat/distance/health).
		const Glow_t& tEnemy = Vars::Aimbot::Draw::TrajectoryGlowEnemy.Value;
		const Glow_t& tTeam = Vars::Aimbot::Draw::TrajectoryGlowTeam.Value;
		const bool bEnemyOn = tEnemy();
		const bool bTeamOn = tTeam();
		if (bEnemyOn || bTeamOn)
		{
			for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerAll))
			{
				auto pPlayer = pEntity->As<CTFPlayer>();
				Vec3 vDelta;
				if (!F::TrajectoryGhost.ShouldRender(pLocal, pPlayer, vDelta))
					continue;

				const bool bEnemy = pPlayer->m_iTeamNum() != pLocal->m_iTeamNum();
				if (bEnemy && bEnemyOn)
					m_mEntities[tEnemy].emplace_back(pPlayer, GetGlowColor(pPlayer, tEnemy, pLocal), TRAJECTORY_GHOST_FLAG);
				else if (!bEnemy && bTeamOn)
					m_mEntities[tTeam].emplace_back(pPlayer, GetGlowColor(pPlayer, tTeam, pLocal), TRAJECTORY_GHOST_FLAG);
			}
		}
	}
}

void CGlow::RenderFirst()
{
	EnsureScreenBuffers();

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor || !m_pMatBlurX || !m_pMatBlurY || !m_pMatHaloAddToScreen)
		return F::Materials.ReloadMaterials();

	// Single batch on the shared-depth buffer: RenderSecond's silhouette pass
	// stamps the halo mask itself through the shared depth-stencil, making this
	// whole pass (one model draw per glowing entity) redundant. Multiple batches
	// keep the global pre-stamp so an earlier batch's halo can't bleed over a
	// later batch's not-yet-stamped interior.
	if (UseInlineStamp())
		return;

	FirstBegin(pRenderContext);
	for (auto& [tGlow, vInfo] : m_mEntities)
	{
		for (auto& tInfo : vInfo)
		{
			m_iFlags = tInfo.m_iFlags;
			DrawModel(tInfo.m_pEntity);
			m_iFlags = false;
		}
	}
	FirstEnd(pRenderContext);
}

void CGlow::RenderSecond()
{
	EnsureScreenBuffers();

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor || !m_pMatBlurX || !m_pMatBlurY || !m_pMatHaloAddToScreen)
		return F::Materials.ReloadMaterials();

	// See RenderFirst: the silhouette draws below double as the interior stamp
	// (the buffer's depth-stencil is the screen's, and the stamp ops mark every
	// covered pixel regardless of the color output going to the buffer).
	const bool bInlineStamp = UseInlineStamp();

	const int w = H::Draw.m_nScreenW, h = H::Draw.m_nScreenH;
	for (auto& [tGlow, vInfo] : m_mEntities)
	{
		SecondBegin(pRenderContext);
		if (bInlineStamp)
			StampStencilBegin(pRenderContext);
		for (auto& tInfo : vInfo)
		{
			I::RenderView->SetColorModulation(tInfo.m_cColor);
			I::RenderView->SetBlend(tInfo.m_cColor.a / 255.f);

			m_iFlags = tInfo.m_iFlags;
			DrawModel(tInfo.m_pEntity);
			m_iFlags = false;
		}
		if (bInlineStamp)
			StampStencilEnd(pRenderContext);
		SecondEnd(tGlow, pRenderContext, w, h);
	}
}

void CGlow::InitFlexBuffers(int iW, int iH)
{
	// GlowResolution scales these like the screen buffers; face glow already has
	// its own separate cleared depth, so only the buffer dims change.
	const float flScale = std::clamp(Vars::Misc::Game::GlowResolution.Value, 0.3f, 1.5f);
	const int iSW = std::max(int(iW * flScale), 64);
	const int iSH = std::max(int(iH * flScale), 64);
	if (m_pFlexBuffer1 && iSW == m_iFlexW && iSH == m_iFlexH)
		return;
	UnloadFlexBuffers();
	m_iFlexBaseW = iW;
	m_iFlexBaseH = iH;
	m_iFlexW = iSW;
	m_iFlexH = iSH;

	// Mirrors the RenderBuffer setup but face-sized and with its own depth (the
	// framebuffer-shared depth is smaller than a face). The cleared separate
	// depth means the silhouette pass has no world occlusion - face glow is
	// through-walls by construction.
	for (int i = 0; i < 2; i++)
	{
		ITexture*& pBuffer = i ? m_pFlexBuffer2 : m_pFlexBuffer1;
		pBuffer = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
			i ? "FlexFOVGlow2" : "FlexFOVGlow1",
			m_iFlexW, m_iFlexH,
			RT_SIZE_LITERAL,
			IMAGE_FORMAT_RGB888,
			MATERIAL_RT_DEPTH_SEPARATE,
			TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
			CREATERENDERTARGETFLAGS_HDR
		);
		pBuffer->IncrementReferenceCount();
	}

	{
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "FlexFOVGlow1");
		kv->SetString("$additive", "1");
		m_pFlexHalo = F::Materials.Create("FlexFOVGlowHalo", kv);
	}
	{
		KeyValues* kv = new KeyValues("BlurFilterX");
		kv->SetString("$basetexture", "FlexFOVGlow1");
		m_pFlexBlurX = F::Materials.Create("FlexFOVGlowBlurX", kv);
	}
	{
		KeyValues* kv = new KeyValues("BlurFilterY");
		kv->SetString("$basetexture", "FlexFOVGlow2");
		m_pFlexBlurY = F::Materials.Create("FlexFOVGlowBlurY", kv);
		m_pFlexBloomAmount = m_pFlexBlurY->FindVar("$bloomamount", nullptr);
	}
}

void CGlow::UnloadFlexBuffers()
{
	for (IMaterial** ppMat : { &m_pFlexHalo, &m_pFlexBlurX, &m_pFlexBlurY })
	{
		if (*ppMat)
		{
			// Through F::Materials.Remove so the m_mMatList entry is erased too -
			// a raw release leaves a dangling pointer there, and the
			// CMaterial_Uncache hook blocks teardown for listed materials.
			F::Materials.Remove(*ppMat);
			*ppMat = nullptr;
		}
	}
	m_pFlexBloomAmount = nullptr;

	for (ITexture** ppTex : { &m_pFlexBuffer1, &m_pFlexBuffer2 })
	{
		if (*ppTex)
		{
			(*ppTex)->DecrementReferenceCount();
			(*ppTex)->DeleteIfUnreferenced();
			*ppTex = nullptr;
		}
	}
	m_iFlexW = m_iFlexH = 0;
	m_iFlexBaseW = m_iFlexBaseH = 0;
}

// Runs the full glow pipeline against the currently-bound FlexFOV face RT.
// Called from DoPostScreenSpaceEffects during a face capture pass, where the
// face camera's matrices are active, so the silhouettes project into the face
// and the composite warps the outlines exactly like the scene. Structure
// mirrors RenderFirst + RenderSecond with face-sized buffers/dims; the interior
// stencil mask lands in the face RT's own depth-stencil.
void CGlow::RenderOnFlexFace()
{
	// Pick up GlowResolution changes: re-run the init with the caller's original
	// dims (InitFlexBuffers early-outs when the scaled dims are unchanged).
	if (m_pFlexBuffer1 && m_iFlexBaseW && m_iFlexBaseH)
		InitFlexBuffers(m_iFlexBaseW, m_iFlexBaseH);

	if (m_mEntities.empty() || !m_pFlexBuffer1 || !m_pFlexBuffer2 || !m_pFlexHalo || !m_pFlexBlurX || !m_pFlexBlurY)
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor)
		return;

	// Silhouettes/blur render into the screen-sized flex buffers (bw x bh); the
	// halo blits upscale them into the face RT (fw x fh). Face-sized buffers cost
	// ~3x the fill for no visible gain on a low-frequency outline.
	const int bw = m_iFlexW, bh = m_iFlexH;
	const int fw = F::FlexFOV.m_iFaceW, fh = F::FlexFOV.m_iFaceH;
	if (fw <= 0 || fh <= 0)
		return;

	// Skip entities the face being captured can't see: everything here (stencil
	// mask + colored silhouette) is 2 model draws per entity per face, and at
	// 245 fov half the glow set is behind any given face. Shared angular cull
	// with the chams face pass.
	auto FaceVisible = [&](CBaseEntity* pEntity)
	{
		return F::FlexFOV.FaceCanSee(pEntity->GetAbsOrigin());
	};

	// The face RT's stencil survives across frames (the capture pass clears only
	// color+depth) and FirstBegin stamps 1s into it every frame, so without this
	// clear the accumulated mask eventually blocks the halo almost everywhere
	// (glow "sometimes shining through" was the leftover unstamped pixels).
	pRenderContext->ClearBuffers(false, false, true);

	FirstBegin(pRenderContext);
	for (auto& [tGlow, vInfo] : m_mEntities)
	{
		for (auto& tInfo : vInfo)
		{
			if (!FaceVisible(tInfo.m_pEntity))
				continue;
			m_iFlags = tInfo.m_iFlags;
			DrawModel(tInfo.m_pEntity);
			m_iFlags = false;
		}
	}
	FirstEnd(pRenderContext);

	for (auto& [tGlow, vInfo] : m_mEntities)
	{
		// Skip the whole silhouette + blit pass when nothing in this group is
		// visible in the face (clears + up to 5 full-face blits saved).
		bool bAnyVisible = false;
		for (auto& tInfo : vInfo)
		{
			if (FaceVisible(tInfo.m_pEntity))
			{
				bAnyVisible = true;
				break;
			}
		}
		if (!bAnyVisible)
			continue;

		// Colored silhouettes into the flex buffer. Depth is cleared, not shared
		// with the scene: face glow is through-walls.
		Begin();
		pRenderContext->PushRenderTargetAndViewport();
		pRenderContext->SetRenderTarget(m_pFlexBuffer1);
		pRenderContext->Viewport(0, 0, bw, bh);
		pRenderContext->ClearColor4ub(0, 0, 0, 0);
		pRenderContext->ClearBuffers(true, true, false);
		for (auto& tInfo : vInfo)
		{
			if (!FaceVisible(tInfo.m_pEntity))
				continue;

			I::RenderView->SetColorModulation(tInfo.m_cColor);
			I::RenderView->SetBlend(tInfo.m_cColor.a / 255.f);

			m_iFlags = tInfo.m_iFlags;
			DrawModel(tInfo.m_pEntity);
			m_iFlags = false;
		}
		pRenderContext->PopRenderTargetAndViewport();

		if (tGlow.Blur)
		{
			m_pFlexBloomAmount->SetFloatValue(tGlow.Blur);

			pRenderContext->PushRenderTargetAndViewport();
			{
				pRenderContext->Viewport(0, 0, bw, bh);
				pRenderContext->SetRenderTarget(m_pFlexBuffer2);
				pRenderContext->DrawScreenSpaceRectangle(m_pFlexBlurX, 0, 0, bw, bh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
				pRenderContext->SetRenderTarget(m_pFlexBuffer1);
				pRenderContext->DrawScreenSpaceRectangle(m_pFlexBlurY, 0, 0, bw, bh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
			}
			pRenderContext->PopRenderTargetAndViewport();
		}

		pRenderContext->SetStencilEnable(true);
		pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_EQUAL);
		pRenderContext->SetStencilPassOperation(STENCILOPERATION_KEEP);
		pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
		pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
		pRenderContext->SetStencilReferenceValue(0);
		pRenderContext->SetStencilWriteMask(0x0);
		pRenderContext->SetStencilTestMask(0xFF);

		if (tGlow.Stencil)
		{
			// 4 cardinal offset blits only (the 4 corner blits of the screen path
			// are dropped - at face scale the difference is invisible and each
			// blit is a full-face additive fill). Offsets are in face pixels, so
			// scale by the face/buffer ratio to keep the on-screen width.
			const float flScale = float(fh) / float(bh);
			int iSide = std::max(1, static_cast<int>((tGlow.Stencil + 1) / 2.f * flScale));
			pRenderContext->DrawScreenSpaceRectangle(m_pFlexHalo, -iSide, 0, fw, fh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
			pRenderContext->DrawScreenSpaceRectangle(m_pFlexHalo, 0, -iSide, fw, fh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
			pRenderContext->DrawScreenSpaceRectangle(m_pFlexHalo, iSide, 0, fw, fh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
			pRenderContext->DrawScreenSpaceRectangle(m_pFlexHalo, 0, iSide, fw, fh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);
		}
		if (tGlow.Blur)
			pRenderContext->DrawScreenSpaceRectangle(m_pFlexHalo, 0, 0, fw, fh, 0.f, 0.f, bw - 1, bh - 1, bw, bh);

		pRenderContext->SetStencilEnable(false);

		End();
	}
}

void CGlow::RenderBacktrack(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo)
{
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

	//float flOriginalBlend = I::RenderView->GetBlend();
	auto fDrawModel = [&](Vec3& vOrigin, const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld, float flBlend)
	{
		if (!SDK::IsOnScreen(pEntity, vOrigin))
			return;

		//I::RenderView->SetBlend(flBlend * flOriginalBlend);
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
	//I::RenderView->SetBlend(flOriginalBlend);
}
void CGlow::RenderFakeAngle(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo)
{
	static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, F::FakeAngle.aBones);
}
void CGlow::RenderTrajectory(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];

	const int nBones = pState.m_pStudioHdr ? pState.m_pStudioHdr->numbones : 0;
	Vec3 vDelta;
	if (!pBoneToWorld || nBones < 1 || nBones > MAXSTUDIOBONES
		|| !F::TrajectoryGhost.GetDelta(pInfo.entity_index, vDelta))
		return IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);

	static matrix3x4 s_aBones[MAXSTUDIOBONES];
	memcpy(s_aBones, pBoneToWorld, sizeof(matrix3x4) * nBones);
	for (int i = 0; i < nBones; i++)
	{
		s_aBones[i][0][3] += vDelta.x;
		s_aBones[i][1][3] += vDelta.y;
		s_aBones[i][2][3] += vDelta.z;
	}
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, s_aBones);
}
void CGlow::RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	if (!m_iFlags)
	{
		static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	}
	else if (m_iFlags & TRAJECTORY_GHOST_FLAG)
		RenderTrajectory(pState, pInfo, pBoneToWorld);
	else
	{
		if (pInfo.entity_index != I::EngineClient->GetLocalPlayer())
			RenderBacktrack(pState, pInfo);
		else
			RenderFakeAngle(pState, pInfo);
	}
}

void CGlow::RenderViewmodel(void* rcx, int flags)
{
	if (!F::Groups.GroupsActive())
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor || !m_pMatBlurX || !m_pMatBlurY || !m_pMatHaloAddToScreen)
		return F::Materials.ReloadMaterials();

	Group_t* pGroup = nullptr;
	if (!F::Groups.GetGroup(reinterpret_cast<CBaseAnimating*>(rcx)->IsValid() ? TargetsEnum::ViewmodelHands : TargetsEnum::ViewmodelWeapon, pGroup) || !pGroup->m_tGlow())
		return;

	static auto CBaseAnimating_InternalDrawModel = U::Hooks.m_mHooks["CBaseAnimating_InternalDrawModel"];

	EnsureScreenBuffers();
	const int w = H::Draw.m_nScreenW, h = H::Draw.m_nScreenH;

	pRenderContext->CullMode(MATERIAL_CULLMODE_CCW); // glow won't work properly with MATERIAL_CULLMODE_CW
	// Shared-depth buffer: the silhouette draw below stamps the interior itself
	// (one glow config here, so the multi-batch ordering concern never applies);
	// scaled buffers need the separate screen-stencil stamp pass.
	if (!m_bBufShared)
	{
		FirstBegin(pRenderContext);
		CBaseAnimating_InternalDrawModel->Call<int>(rcx, flags);
		FirstEnd(pRenderContext);
	}
	SecondBegin(pRenderContext);
	if (m_bBufShared)
		StampStencilBegin(pRenderContext);
	const Color_t tGlowColor = pGroup->m_tGlow.GetColor(-1.f, 0.f); // viewmodel = distance 0
	I::RenderView->SetColorModulation(tGlowColor);
	I::RenderView->SetBlend(tGlowColor.a / 255.f);
	CBaseAnimating_InternalDrawModel->Call<int>(rcx, flags);
	if (m_bBufShared)
		StampStencilEnd(pRenderContext);
	SecondEnd(pGroup->m_tGlow, pRenderContext, w, h);
	pRenderContext->CullMode(G::FlipViewmodels ? MATERIAL_CULLMODE_CW : MATERIAL_CULLMODE_CCW);
}
void CGlow::RenderViewmodel(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	if (!F::Groups.GroupsActive())
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor || !m_pMatBlurX || !m_pMatBlurY || !m_pMatHaloAddToScreen)
		return F::Materials.ReloadMaterials();

	Group_t* pGroup = nullptr;
	if (!F::Groups.GetGroup(TargetsEnum::ViewmodelWeapon, pGroup) || !pGroup->m_tGlow())
		return;

	static auto IVModelRender_DrawModelExecute = U::Hooks.m_mHooks["IVModelRender_DrawModelExecute"];

	EnsureScreenBuffers();
	const int w = H::Draw.m_nScreenW, h = H::Draw.m_nScreenH;

	// See the InternalDrawModel overload: inline stamp on the shared buffer,
	// separate stamp pass on scaled ones.
	if (!m_bBufShared)
	{
		FirstBegin(pRenderContext);
		IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
		FirstEnd(pRenderContext);
	}
	SecondBegin(pRenderContext);
	if (m_bBufShared)
		StampStencilBegin(pRenderContext);
	const Color_t tGlowColor = pGroup->m_tGlow.GetColor(-1.f, 0.f); // viewmodel = distance 0
	I::RenderView->SetColorModulation(tGlowColor);
	I::RenderView->SetBlend(tGlowColor.a / 255.f);
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	if (m_bBufShared)
		StampStencilEnd(pRenderContext);
	SecondEnd(pGroup->m_tGlow, pRenderContext, w, h);
}



void CGlow::EnsureScreenBuffers()
{
	int nWidth, nHeight; I::MatSystemSurface->GetScreenSize(nWidth, nHeight);
	if (nWidth <= 0 || nHeight <= 0)
		return;

	const float flScale = std::clamp(Vars::Misc::Game::GlowResolution.Value, 0.3f, 1.5f);
	const int iBufW = std::max(int(nWidth * flScale), 64);
	const int iBufH = std::max(int(nHeight * flScale), 64);
	if (m_pRenderBuffer1 && iBufW == m_iBufW && iBufH == m_iBufH)
		return;

	FreeScreenBuffers();
	m_iBufW = iBufW;
	m_iBufH = iBufH;
	// At exactly screen size the buffer shares the engine's depth-stencil, which
	// keeps the classic depth behavior AND lets the silhouette pass stamp the
	// halo mask inline (see UseInlineStamp). Any other size must carry its own
	// depth (a shared depth can't be larger than the framebuffer, and a scaled
	// viewport would misalign against it anyway) - it is cleared per batch, so
	// scaled silhouettes are through-walls by construction like the FlexFOV
	// face path.
	m_bBufShared = iBufW == nWidth && iBufH == nHeight;

	m_pRenderBuffer1 = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
		"RenderBuffer1",
		iBufW, iBufH,
		RT_SIZE_LITERAL,
		IMAGE_FORMAT_RGB888,
		m_bBufShared ? MATERIAL_RT_DEPTH_SHARED : MATERIAL_RT_DEPTH_SEPARATE,
		TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
		CREATERENDERTARGETFLAGS_HDR
	);
	m_pRenderBuffer1->IncrementReferenceCount();

	// Half the silhouette buffer: this only ever holds the blur intermediate (a
	// screen-space pass with no depth test), so quartering its fill is free. Never
	// larger than the framebuffer (max scale 1.5 -> 0.75x screen), so shared
	// depth stays legal.
	m_pRenderBuffer2 = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
		"RenderBuffer2",
		iBufW / 2, iBufH / 2,
		RT_SIZE_LITERAL,
		IMAGE_FORMAT_RGB888,
		MATERIAL_RT_DEPTH_SHARED,
		TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
		CREATERENDERTARGETFLAGS_HDR
	);
	m_pRenderBuffer2->IncrementReferenceCount();

	{
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "RenderBuffer1");
		kv->SetString("$additive", "1");
		m_pMatHaloAddToScreen = F::Materials.Create("MatHaloAddToScreen", kv);
	}
	{
		KeyValues* kv = new KeyValues("BlurFilterX");
		kv->SetString("$basetexture", "RenderBuffer1");
		m_pMatBlurX = F::Materials.Create("MatBlurX", kv);
	}
	{
		KeyValues* kv = new KeyValues("BlurFilterY");
		kv->SetString("$basetexture", "RenderBuffer2");
		m_pMatBlurY = F::Materials.Create("MatBlurY", kv);
		m_pBloomAmount = m_pMatBlurY->FindVar("$bloomamount", nullptr);
	}
}

void CGlow::FreeScreenBuffers()
{
	// Materials before textures, through F::Materials.Remove (see
	// UnloadFlexBuffers: a raw release leaves a dangling m_mMatList entry).
	for (IMaterial** ppMat : { &m_pMatHaloAddToScreen, &m_pMatBlurX, &m_pMatBlurY })
	{
		if (*ppMat)
		{
			F::Materials.Remove(*ppMat);
			*ppMat = nullptr;
		}
	}
	m_pBloomAmount = nullptr;

	for (ITexture** ppTex : { &m_pRenderBuffer1, &m_pRenderBuffer2 })
	{
		if (*ppTex)
		{
			(*ppTex)->DecrementReferenceCount();
			(*ppTex)->DeleteIfUnreferenced();
			*ppTex = nullptr;
		}
	}
	m_iBufW = m_iBufH = 0;
	m_bBufShared = true;
}

void CGlow::Initialize()
{
	if (!m_pMatGlowColor)
	{
		m_pMatGlowColor = I::MaterialSystem->FindMaterial("dev/glow_color", TEXTURE_GROUP_OTHER);
		m_pMatGlowColor->IncrementReferenceCount();
		F::Materials.m_mMatList[m_pMatGlowColor];
	}

	EnsureScreenBuffers();
}

void CGlow::Unload()
{
	UnloadFlexBuffers();
	FreeScreenBuffers();

	if (m_pMatGlowColor)
	{
		m_pMatGlowColor->DecrementReferenceCount();
		m_pMatGlowColor->DeleteIfUnreferenced();
		m_pMatGlowColor = nullptr;
	}
}