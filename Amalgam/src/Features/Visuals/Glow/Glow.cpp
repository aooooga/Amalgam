#include "Glow.h"

#include "../Groups/Groups.h"
#include "../Materials/Materials.h"
#include "../FakeAngle/FakeAngle.h"
#include "../FlexFOV/FlexFOV.h"
#include "../../Backtrack/Backtrack.h"

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

void CGlow::FirstBegin(IMatRenderContext* pRenderContext)
{
	Begin();

	pRenderContext->SetStencilEnable(true);
	pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_ALWAYS);
	pRenderContext->SetStencilPassOperation(STENCILOPERATION_REPLACE);
	pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilZFailOperation(STENCILOPERATION_REPLACE);
	pRenderContext->SetStencilReferenceValue(1);
	pRenderContext->SetStencilWriteMask(0xFF);
	pRenderContext->SetStencilTestMask(0x0);
}
void CGlow::FirstEnd(IMatRenderContext* pRenderContext)
{
	pRenderContext->SetStencilEnable(false);

	End();
}

void CGlow::SecondBegin(IMatRenderContext* pRenderContext, int w, int h)
{
	Begin();

	pRenderContext->PushRenderTargetAndViewport();
	pRenderContext->SetRenderTarget(m_pRenderBuffer1);
	pRenderContext->Viewport(0, 0, w, h);
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	pRenderContext->ClearBuffers(true, false, false);
}
void CGlow::SecondEnd(Glow_t tGlow, IMatRenderContext* pRenderContext, int w, int h)
{
	pRenderContext->PopRenderTargetAndViewport();

	if (tGlow.Blur)
	{
		m_pBloomAmount->SetFloatValue(tGlow.Blur);

		pRenderContext->PushRenderTargetAndViewport();
		{
			pRenderContext->Viewport(0, 0, w, h);
			pRenderContext->SetRenderTarget(m_pRenderBuffer2);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatBlurX, 0, 0, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
			pRenderContext->SetRenderTarget(m_pRenderBuffer1);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatBlurY, 0, 0, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
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
		int iSide = (tGlow.Stencil + 1) / 2.f;
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, -iSide, 0, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, 0, -iSide, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, iSide, 0, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, 0, iSide, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
		if (int iCorner = tGlow.Stencil / 2.f)
		{
			pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, -iCorner, -iCorner, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, iCorner, iCorner, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, iCorner, -iCorner, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
			pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, -iCorner, iCorner, w, h, 0.f, 0.f, w - 1, h - 1, w, h);
		}
	}
	if (tGlow.Blur)
		pRenderContext->DrawScreenSpaceRectangle(m_pMatHaloAddToScreen, 0, 0, w, h, 0.f, 0.f, w - 1, h - 1, w, h);

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

// Glow color: the glow's own color option (not the group color), or the health
// gradient for entities with HP when enabled.
static Color_t GetGlowColor(CBaseEntity* pEntity, const Glow_t& tGlow)
{
	return tGlow.GetColor(GetHealthFraction(pEntity));
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
			m_mEntities[pGroup->m_tGlow].emplace_back(pEntity, GetGlowColor(pEntity, pGroup->m_tGlow));

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
					m_mEntities[pGroup->m_tBacktrackGlow].emplace_back(pEntity, GetGlowColor(pEntity, pGroup->m_tBacktrackGlow), pGroup->m_iBacktrack);
			}
		}
	}

	Group_t* pGroup = nullptr;
	if (F::FakeAngle.bDrawChams && F::FakeAngle.bBonesSetup
		&& F::Groups.GetGroup(TargetsEnum::FakeAngle, pGroup) && pGroup->m_tGlow())
	{	// fakeangle
		m_mEntities[pGroup->m_tGlow].emplace_back(pLocal, GetGlowColor(pLocal, pGroup->m_tGlow), 1);
	}
}

void CGlow::RenderFirst()
{
	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor || !m_pMatBlurX || !m_pMatBlurY || !m_pMatHaloAddToScreen)
		return F::Materials.ReloadMaterials();

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
	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	if (!pRenderContext || !m_pMatGlowColor || !m_pMatBlurX || !m_pMatBlurY || !m_pMatHaloAddToScreen)
		return F::Materials.ReloadMaterials();

	const int w = H::Draw.m_nScreenW, h = H::Draw.m_nScreenH;
	for (auto& [tGlow, vInfo] : m_mEntities)
	{
		SecondBegin(pRenderContext, w, h);
		for (auto& tInfo : vInfo)
		{
			I::RenderView->SetColorModulation(tInfo.m_cColor);
			I::RenderView->SetBlend(tInfo.m_cColor.a / 255.f);

			m_iFlags = tInfo.m_iFlags;
			DrawModel(tInfo.m_pEntity);
			m_iFlags = false;
		}
		SecondEnd(tGlow, pRenderContext, w, h);
	}
}

void CGlow::InitFlexBuffers(int iW, int iH)
{
	if (m_pFlexBuffer1 && iW == m_iFlexW && iH == m_iFlexH)
		return;
	UnloadFlexBuffers();
	m_iFlexW = iW;
	m_iFlexH = iH;

	// Mirrors the RenderBuffer setup but face-sized and with its own depth (the
	// framebuffer-shared depth is smaller than a face). The cleared separate
	// depth means the silhouette pass has no world occlusion - face glow is
	// through-walls by construction.
	for (int i = 0; i < 2; i++)
	{
		ITexture*& pBuffer = i ? m_pFlexBuffer2 : m_pFlexBuffer1;
		pBuffer = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
			i ? "FlexFOVGlow2" : "FlexFOVGlow1",
			iW, iH,
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
			(*ppMat)->DecrementReferenceCount();
			(*ppMat)->DeleteIfUnreferenced();
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
}

// Runs the full glow pipeline against the currently-bound FlexFOV face RT.
// Called from DoPostScreenSpaceEffects during a face capture pass, where the
// face camera's matrices are active, so the silhouettes project into the face
// and the composite warps the outlines exactly like the scene. Structure
// mirrors RenderFirst + RenderSecond with face-sized buffers/dims; the interior
// stencil mask lands in the face RT's own depth-stencil.
void CGlow::RenderOnFlexFace()
{
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
	// 245 fov half the glow set is behind any given face. Angular test against
	// the face axis with slack for the entity's own extent.
	auto FaceVisible = [&](CBaseEntity* pEntity)
	{
		Vec3 vDelta = pEntity->GetAbsOrigin() - F::FlexFOV.m_vEyeOrigin;
		const float flDist = vDelta.Length();
		if (flDist < 150.f) // close enough that the bbox can wrap any frustum
			return true;
		const float flSlack = std::asin(std::min(1.f, 100.f / flDist));
		const float flCos = std::cos(std::min(3.14159f, F::FlexFOV.m_flCaptureHalfAngle + flSlack));
		return vDelta.Dot(F::FlexFOV.m_vCaptureFwd) > flCos * flDist;
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
void CGlow::RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld)
{
	if (!m_iFlags)
	{
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

	const int w = H::Draw.m_nScreenW, h = H::Draw.m_nScreenH;

	pRenderContext->CullMode(MATERIAL_CULLMODE_CCW); // glow won't work properly with MATERIAL_CULLMODE_CW
	FirstBegin(pRenderContext);
	CBaseAnimating_InternalDrawModel->Call<int>(rcx, flags);
	FirstEnd(pRenderContext);
	SecondBegin(pRenderContext, w, h);
	const Color_t tGlowColor = pGroup->m_tGlow.GetColor();
	I::RenderView->SetColorModulation(tGlowColor);
	I::RenderView->SetBlend(tGlowColor.a / 255.f);
	CBaseAnimating_InternalDrawModel->Call<int>(rcx, flags);
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

	const int w = H::Draw.m_nScreenW, h = H::Draw.m_nScreenH;

	FirstBegin(pRenderContext);
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	FirstEnd(pRenderContext);
	SecondBegin(pRenderContext, w, h);
	const Color_t tGlowColor = pGroup->m_tGlow.GetColor();
	I::RenderView->SetColorModulation(tGlowColor);
	I::RenderView->SetBlend(tGlowColor.a / 255.f);
	IVModelRender_DrawModelExecute->Call<void>(I::ModelRender, pState, pInfo, pBoneToWorld);
	SecondEnd(pGroup->m_tGlow, pRenderContext, w, h);
}



void CGlow::Initialize()
{
	int nWidth, nHeight; I::MatSystemSurface->GetScreenSize(nWidth, nHeight);

	if (!m_pMatGlowColor)
	{
		m_pMatGlowColor = I::MaterialSystem->FindMaterial("dev/glow_color", TEXTURE_GROUP_OTHER);
		m_pMatGlowColor->IncrementReferenceCount();
		F::Materials.m_mMatList[m_pMatGlowColor];
	}

	if (!m_pRenderBuffer1)
	{
		m_pRenderBuffer1 = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
			"RenderBuffer1",
			nWidth, nHeight,
			RT_SIZE_LITERAL,
			IMAGE_FORMAT_RGB888,
			MATERIAL_RT_DEPTH_SHARED,
			TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
			CREATERENDERTARGETFLAGS_HDR
		);
		m_pRenderBuffer1->IncrementReferenceCount();
	}

	if (!m_pRenderBuffer2)
	{
		m_pRenderBuffer2 = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
			"RenderBuffer2",
			nWidth, nHeight,
			RT_SIZE_LITERAL,
			IMAGE_FORMAT_RGB888,
			MATERIAL_RT_DEPTH_SHARED,
			TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
			CREATERENDERTARGETFLAGS_HDR
		);
		m_pRenderBuffer2->IncrementReferenceCount();
	}

	if (!m_pMatHaloAddToScreen)
	{
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "RenderBuffer1");
		kv->SetString("$additive", "1");
		m_pMatHaloAddToScreen = F::Materials.Create("MatHaloAddToScreen", kv);
	}

	if (!m_pMatBlurX)
	{
		KeyValues* kv = new KeyValues("BlurFilterX");
		kv->SetString("$basetexture", "RenderBuffer1");
		m_pMatBlurX = F::Materials.Create("MatBlurX", kv);
	}

	if (!m_pMatBlurY)
	{
		KeyValues* kv = new KeyValues("BlurFilterY");
		kv->SetString("$basetexture", "RenderBuffer2");
		m_pMatBlurY = F::Materials.Create("MatBlurY", kv);
		m_pBloomAmount = m_pMatBlurY->FindVar("$bloomamount", nullptr);
	}
}

void CGlow::Unload()
{
	UnloadFlexBuffers();

	if (m_pMatGlowColor)
	{
		m_pMatGlowColor->DecrementReferenceCount();
		m_pMatGlowColor->DeleteIfUnreferenced();
		m_pMatGlowColor = nullptr;
	}

	if (m_pMatBlurX)
	{
		m_pMatBlurX->DecrementReferenceCount();
		m_pMatBlurX->DeleteIfUnreferenced();
		m_pMatBlurX = nullptr;
	}

	if (m_pMatBlurY)
	{
		m_pMatBlurY->DecrementReferenceCount();
		m_pMatBlurY->DeleteIfUnreferenced();
		m_pMatBlurY = nullptr;
	}

	if (m_pMatHaloAddToScreen)
	{
		m_pMatHaloAddToScreen->DecrementReferenceCount();
		m_pMatHaloAddToScreen->DeleteIfUnreferenced();
		m_pMatHaloAddToScreen = nullptr;
	}

	if (m_pRenderBuffer1)
	{
		m_pRenderBuffer1->DecrementReferenceCount();
		m_pRenderBuffer1->DeleteIfUnreferenced();
		m_pRenderBuffer1 = nullptr;
	}

	if (m_pRenderBuffer2)
	{
		m_pRenderBuffer2->DecrementReferenceCount();
		m_pRenderBuffer2->DeleteIfUnreferenced();
		m_pRenderBuffer2 = nullptr;
	}
}