#include "RearView.h"

#include "../Materials/Materials.h"
#include "../FlexFOV/FlexFOV.h"
#include "../CameraWindow/CameraWindow.h"

#include <algorithm>

// Reject NaNs and degenerate/too-wide FOVs a single linear projection can't
// represent; fall back to a sane value instead. Used only for the per-camera
// side FOV - the front coverage is a full-circle angle and is clamped separately.
static float SanitizeFOV(float flFov, float flFallback)
{
	if (flFov != flFov || flFov <= 1.f || flFov >= 179.f)
		return flFallback;
	return flFov;
}

bool CRearView::SetupTargets(int iScrW, int iScrH, int iCams)
{
	if (!m_vTextures.empty() && iScrW == m_iLastW && iScrH == m_iLastH && iCams == m_iLastCams)
		return true;

	FreeTargets();
	m_iLastW = iScrW; m_iLastH = iScrH; m_iLastCams = iCams;

	const int iCamW = iScrW / iCams;
	if (iCamW <= 0 || iScrH <= 0)
		return false;

	for (int i = 0; i < iCams; i++)
	{
		// Own (separate) depth+stencil per RT so Push3DView has a depth-stencil
		// matching the sub-screen-sized target, and so the glow stencil has somewhere
		// to land; the flank pass draws only enemy models, never the world.
		const std::string sTex = "RearViewTex" + std::to_string(i);
		ITexture* pTex = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
			sTex.c_str(),
			iCamW, iScrH,
			RT_SIZE_LITERAL,
			IMAGE_FORMAT_RGBA8888,
			MATERIAL_RT_DEPTH_SEPARATE,
			TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
			CREATERENDERTARGETFLAGS_HDR
		);
		if (!pTex)
			return false;
		pTex->IncrementReferenceCount();
		m_vTextures.push_back(pTex);

		// Translucent tile: the transparent (cleared) background shows nothing and
		// only the enemy silhouettes blend over the frame; $alpha is driven live from
		// the alpha slider for the whole overlay's opacity.
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", sTex.c_str());
		kv->SetInt("$translucent", 1);
		kv->SetInt("$vertexalpha", 0);
		kv->SetString("$alpha", "1");
		const std::string sMat = "RearViewMat" + std::to_string(i);
		IMaterial* pMat = F::Materials.Create(sMat.c_str(), kv);
		if (!pMat)
			return false;
		m_vMaterials.push_back(pMat);
		m_vAlphaVars.push_back(pMat->FindVar("$alpha", nullptr));
	}

	// Shared glow scratch buffers, sized to one flank tile so all dims line up.
	for (int i = 0; i < 2; i++)
	{
		ITexture*& pBuf = i ? m_pGlowBlur : m_pGlowSil;
		pBuf = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
			i ? "RearViewGlowBlur" : "RearViewGlowSil",
			iCamW, iScrH,
			RT_SIZE_LITERAL,
			IMAGE_FORMAT_RGBA8888,
			MATERIAL_RT_DEPTH_SEPARATE,
			TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT | TEXTUREFLAGS_EIGHTBITALPHA,
			CREATERENDERTARGETFLAGS_HDR
		);
		if (!pBuf)
			return false;
		pBuf->IncrementReferenceCount();
	}
	{
		KeyValues* kv = new KeyValues("BlurFilterX");
		kv->SetString("$basetexture", "RearViewGlowSil");
		m_pGlowBlurX = F::Materials.Create("RearViewGlowBlurX", kv);
	}
	{
		KeyValues* kv = new KeyValues("BlurFilterY");
		kv->SetString("$basetexture", "RearViewGlowBlur");
		m_pGlowBlurY = F::Materials.Create("RearViewGlowBlurY", kv);
		m_pGlowBloom = m_pGlowBlurY->FindVar("$bloomamount", nullptr);
	}
	{
		// Translucent (not additive like the on-screen glow): the halo is baked into
		// the flank RT and must write alpha so the tile composite shows it.
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "RearViewGlowSil");
		kv->SetInt("$translucent", 1);
		m_pGlowHalo = F::Materials.Create("RearViewGlowHalo", kv);
	}

	return true;
}

void CRearView::FreeTargets()
{
	for (IMaterial** ppMat : { &m_pGlowBlurX, &m_pGlowBlurY, &m_pGlowHalo })
	{
		if (*ppMat)
		{
			(*ppMat)->DecrementReferenceCount();
			(*ppMat)->DeleteIfUnreferenced();
			*ppMat = nullptr;
		}
	}
	m_pGlowBloom = nullptr;
	for (ITexture** ppTex : { &m_pGlowSil, &m_pGlowBlur })
	{
		if (*ppTex)
		{
			(*ppTex)->DecrementReferenceCount();
			(*ppTex)->DeleteIfUnreferenced();
			*ppTex = nullptr;
		}
	}

	for (auto pMat : m_vMaterials)
	{
		if (pMat)
		{
			pMat->DecrementReferenceCount();
			pMat->DeleteIfUnreferenced();
		}
	}
	for (auto pTex : m_vTextures)
	{
		if (pTex)
		{
			pTex->DecrementReferenceCount();
			pTex->DeleteIfUnreferenced();
		}
	}
	m_vMaterials.clear();
	m_vTextures.clear();
	m_vAlphaVars.clear();
}

void CRearView::GatherVisibleEnemies(CTFPlayer* pLocal, float flViewYaw, float flFrontFOV, float flPerCam, int iCams, uint32_t uUpdateMask)
{
	m_vVisible.clear();

	const Vec3 vMyEye = pLocal->GetShootPos();
	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant())
			continue;

		// Which flank cameras can actually contain this enemy. Yaw-only test
		// (the tall tiles have a huge vertical FOV): camera i is centred
		// flFrontFOV/2 + (i + 0.5) * flPerCam left of view forward. Very close
		// enemies span large angles across seams - keep them in every camera.
		uint32_t uCamMask = 0;
		const Vec3 vDelta = pPlayer->GetShootPos() - vMyEye;
		if (vDelta.Length2DSqr() < 400.f * 400.f)
			uCamMask = (1u << iCams) - 1;
		else
		{
			const float flEnemyYaw = Math::NormalizeAngle(Math::Rad2Deg(atan2f(vDelta.y, vDelta.x)) - flViewYaw);
			for (int i = 0; i < iCams; i++)
			{
				const float flCamYaw = flFrontFOV * 0.5f + (i + 0.5f) * flPerCam;
				if (fabsf(Math::NormalizeAngle(flEnemyYaw - flCamYaw)) <= flPerCam * 0.5f + 15.f)
					uCamMask |= 1u << i;
			}
		}
		// No camera covers them (frontal enemies, mostly), or only cameras that
		// aren't refreshing this frame - skip the LOS trace too.
		if (!(uCamMask & uUpdateMask))
			continue;

		// Only enemies with a clear line of sight, matching the Lua's IsLineClear.
		if (!SDK::VisPos(pLocal, pPlayer, vMyEye, pPlayer->GetShootPos()))
			continue;

		m_vVisible.push_back({ pPlayer, uCamMask });
	}
}

void CRearView::DrawEnemies(uint32_t uCamBit)
{
	// Only the enemies this camera's yaw slice can contain; the rest would be
	// full-cost model draws the frustum then discards.
	bool bAny = false;
	for (auto& tEnemy : m_vVisible)
	{
		if (tEnemy.m_uCamMask & uCamBit)
		{
			bAny = true;
			break;
		}
	}
	if (!bAny)
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	// Draw the visible enemies with the user's material list (same list/handling as
	// chams: each entry is a named material + color), m_bRendering flipped per model
	// so the engine's own per-draw overrides can't stomp ours.
	auto pLocal = H::Entities.GetLocal();
	for (auto& [sName, tColor] : Vars::Visuals::UI::RearViewMaterial.Value)
	{
		auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));
		I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);

		if (pMaterial && pMaterial->m_bInvertCull)
			pRenderContext->CullMode(MATERIAL_CULLMODE_CW);

		for (auto& tEnemy : m_vVisible)
		{
			if (!(tEnemy.m_uCamMask & uCamBit))
				continue;
			auto pPlayer = tEnemy.m_pPlayer;

			// Per player so distance-based colors track each enemy's range.
			F::Materials.SetColor(pMaterial, tColor.GetColor(pLocal ? pLocal->GetAbsOrigin().DistTo(pPlayer->GetAbsOrigin()) : -1.f));

			m_bRendering = true;
			pPlayer->DrawModel(STUDIO_RENDER);
			m_bRendering = false;
		}

		if (pMaterial && pMaterial->m_bInvertCull)
			pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);
	}
	I::ModelRender->ForcedMaterialOverride(nullptr);

	pRenderContext->Release();
}

// Outline glow into the currently-bound flank RT, mirroring CGlow: stencil the
// interior, render a colored silhouette into a scratch buffer, blur it, then blit
// the halo back offset+masked to the stencil so only the outline lands. Uses a
// translucent (alpha-writing) halo so the baked outline survives the tile blend.
void CRearView::RenderGlow(int iCamW, int iScrH, uint32_t uCamBit)
{
	if (m_vVisible.empty() || !m_pGlowColor || !m_pGlowSil || !m_pGlowBlur || !m_pGlowBlurX || !m_pGlowBlurY || !m_pGlowHalo)
		return;

	const int iStencil = Vars::Visuals::UI::RearViewGlowStencil.Value;
	const float flBlur = Vars::Visuals::UI::RearViewGlowBlur.Value;
	if (!iStencil && !flBlur)
		return;

	// Empty camera: skip the whole pipeline (2 model draws per enemy plus the
	// full-tile blur and halo blits), not just the draws.
	bool bAny = false;
	for (auto& tEnemy : m_vVisible)
	{
		if (tEnemy.m_uCamMask & uCamBit)
		{
			bAny = true;
			break;
		}
	}
	if (!bAny)
		return;

	const Color_t cGlow = Vars::Visuals::UI::RearViewGlowColor.Value;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	const Color_t cOldMod = I::RenderView->GetColorModulation();
	const float flOldBlend = I::RenderView->GetBlend();
	IMaterial* pOldMat = nullptr; OverrideType_t iOldOverride = OVERRIDE_NORMAL;
	I::ModelRender->GetMaterialOverride(&pOldMat, &iOldOverride);

	auto GlowBegin = [&]()
	{
		I::RenderView->SetBlend(0.f);
		I::RenderView->SetColorModulation(1.f, 1.f, 1.f);
		I::ModelRender->ForcedMaterialOverride(m_pGlowColor);
	};
	auto Draw = [&](CTFPlayer* p)
	{
		m_bRendering = true;
		p->DrawModel(STUDIO_RENDER | STUDIO_NOSHADOWS);
		m_bRendering = false;
	};

	// The flank RT's stencil persists across frames; clear it before stamping.
	pRenderContext->ClearBuffers(false, false, true);

	// 1) Stamp the interior into the flank RT stencil (models drawn invisibly).
	GlowBegin();
	pRenderContext->SetStencilEnable(true);
	pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_ALWAYS);
	pRenderContext->SetStencilPassOperation(STENCILOPERATION_REPLACE);
	pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilZFailOperation(STENCILOPERATION_REPLACE);
	pRenderContext->SetStencilReferenceValue(1);
	pRenderContext->SetStencilWriteMask(0xFF);
	pRenderContext->SetStencilTestMask(0x0);
	for (auto& tEnemy : m_vVisible)
	{
		if (tEnemy.m_uCamMask & uCamBit)
			Draw(tEnemy.m_pPlayer);
	}
	pRenderContext->SetStencilEnable(false);

	// 2) Colored silhouette into the scratch buffer.
	pRenderContext->PushRenderTargetAndViewport();
	pRenderContext->SetRenderTarget(m_pGlowSil);
	pRenderContext->Viewport(0, 0, iCamW, iScrH);
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	pRenderContext->ClearBuffers(true, true, false);
	for (auto& tEnemy : m_vVisible)
	{
		if (!(tEnemy.m_uCamMask & uCamBit))
			continue;
		I::RenderView->SetColorModulation(cGlow);
		I::RenderView->SetBlend(cGlow.a / 255.f);
		Draw(tEnemy.m_pPlayer);
	}
	pRenderContext->PopRenderTargetAndViewport(); // back to the flank RT

	// 3) Separable blur (silhouette -> blur -> silhouette).
	if (flBlur && m_pGlowBloom)
	{
		m_pGlowBloom->SetFloatValue(flBlur);
		pRenderContext->PushRenderTargetAndViewport();
		pRenderContext->SetRenderTarget(m_pGlowBlur);
		pRenderContext->Viewport(0, 0, iCamW, iScrH);
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowBlurX, 0, 0, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
		pRenderContext->SetRenderTarget(m_pGlowSil);
		pRenderContext->Viewport(0, 0, iCamW, iScrH);
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowBlurY, 0, 0, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
		pRenderContext->PopRenderTargetAndViewport();
	}

	// 4) Halo: blit the (blurred) silhouette back into the flank RT where the
	// stencil is 0 (outside the interior), so only the outline shows.
	pRenderContext->SetStencilEnable(true);
	pRenderContext->SetStencilCompareFunction(STENCILCOMPARISONFUNCTION_EQUAL);
	pRenderContext->SetStencilPassOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilZFailOperation(STENCILOPERATION_KEEP);
	pRenderContext->SetStencilReferenceValue(0);
	pRenderContext->SetStencilWriteMask(0x0);
	pRenderContext->SetStencilTestMask(0xFF);
	if (iStencil)
	{
		const int s = std::max(1, (iStencil + 1) / 2);
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowHalo, -s, 0, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowHalo, 0, -s, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowHalo, s, 0, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowHalo, 0, s, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
	}
	if (flBlur)
		pRenderContext->DrawScreenSpaceRectangle(m_pGlowHalo, 0, 0, iCamW, iScrH, 0.f, 0.f, iCamW - 1, iScrH - 1, iCamW, iScrH);
	pRenderContext->SetStencilEnable(false);

	I::RenderView->SetColorModulation(cOldMod);
	I::RenderView->SetBlend(flOldBlend);
	I::ModelRender->ForcedMaterialOverride(pOldMat, iOldOverride);

	pRenderContext->Release();
}

void CRearView::RenderSideCamera(const CViewSetup& tView, float flYawOffset, float flSideFOV, ITexture* pTexture, int iCamW, int iScrH, uint32_t uCamBit)
{
	if (!pTexture)
		return;

	const float flPitch = Vars::Visuals::UI::RearViewFlipPitch.Value ? -tView.angles.x : tView.angles.x;

	CViewSetup tCam = tView;
	tCam.x = 0;
	tCam.y = 0;
	tCam.width = iCamW;
	tCam.height = iScrH;
	tCam.m_flAspectRatio = float(iCamW) / float(iScrH);
	tCam.fov = flSideFOV;
	tCam.angles = Vec3(flPitch, tView.angles.y + flYawOffset, tView.angles.z);
	// origin left at the player's eye (tView.origin)

	Frustum frustum;
	I::RenderView->Push3DView(tCam, VIEW_CLEAR_COLOR | VIEW_CLEAR_DEPTH, pTexture, frustum);

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	pRenderContext->ClearBuffers(true, true, false);
	pRenderContext->Release();

	DrawEnemies(uCamBit);
	RenderGlow(iCamW, iScrH, uCamBit); // self-gates: only draws when stencil or blur >= 1

	I::RenderView->PopView(frustum);
}

void CRearView::Capture(void* rcx, const CViewSetup& tView)
{
	if (!Vars::Visuals::UI::RearView.Value)
		return;

	// Skip the sub-renders that re-enter CViewRender_RenderView (FlexFOV faces,
	// camera window) and our own capture, so the flank views are built once per
	// real frame.
	if (m_bCapturing || F::FlexFOV.m_bDrawing || F::CameraWindow.m_bDrawing)
		return;

	if (!I::EngineClient->IsInGame())
		return;

	auto pLocal = H::Entities.GetLocal();
	if (!pLocal || !pLocal->IsAlive())
		return;

	const int iScrW = H::Draw.m_nScreenW, iScrH = H::Draw.m_nScreenH;
	const int iCams = std::clamp(Vars::Visuals::UI::RearViewCameras.Value, 2, 8);
	if (!SetupTargets(iScrW, iScrH, iCams))
		return;

	// Front coverage the player already sees: FlexFOV's wide FOV while the composite
	// owns the screen, otherwise the actual rendered view fov, plus the user's
	// manual offset. This is a full-circle angle (can exceed 179 on wide FlexFOV) so
	// it is CLAMPED, not FOV-sanitized. m_flFovX is only an estimate of the
	// effective warp FOV, so the offset trims the seam until on-screen enemies stop
	// ghosting behind.
	float flBaseFOV = F::FlexFOV.m_bComposite ? F::FlexFOV.m_flFovX : tView.fov;
	if (flBaseFOV != flBaseFOV || flBaseFOV <= 1.f)
		flBaseFOV = 100.f;
	const float flFrontFOV = std::clamp(flBaseFOV + Vars::Visuals::UI::RearViewFOVOffset.Value, 1.f, 350.f);
	const float flRemaining = 360.f - flFrontFOV;
	if (flRemaining <= 0.f)
		return;

	const int iCamW = iScrW / iCams;
	if (iCamW < 10)
		return;

	const float flPerCam = flRemaining / iCams;
	const float flSideFOV = SanitizeFOV(flPerCam - 1.25f, 125.f); // small seam overlap (OVERLAP_DEG)

	// Half rate: refresh alternating cameras each frame (even indices on even
	// frames), halving the per-frame flank draw cost; skipped tiles keep last
	// frame's image.
	uint32_t uUpdateMask = (1u << iCams) - 1;
	if (Vars::Visuals::UI::RearViewHalfRate.Value)
	{
		const uint32_t uParity = I::GlobalVars->framecount & 1;
		for (int i = 0; i < iCams; i++)
		{
			if ((uint32_t(i) & 1) != uParity)
				uUpdateMask &= ~(1u << i);
		}
	}

	GatherVisibleEnemies(pLocal, tView.angles.y, flFrontFOV, flPerCam, iCams, uUpdateMask);

	m_bCapturing = true;
	for (int i = 0; i < iCams; i++)
	{
		if (!(uUpdateMask & (1u << i)))
			continue;

		// Sweep from the edge of the front cone around the back: each camera centred
		// one coverage step further round than the last.
		const float flYaw = flFrontFOV * 0.5f + (i + 0.5f) * flPerCam;
		RenderSideCamera(tView, flYaw, flSideFOV, m_vTextures[i], iCamW, iScrH, 1u << i);
	}
	m_bCapturing = false;
}

void CRearView::DrawOverlay()
{
	if (!Vars::Visuals::UI::RearView.Value || m_vMaterials.empty() || !I::EngineClient->IsInGame())
		return;

	const int iScrW = H::Draw.m_nScreenW, iScrH = H::Draw.m_nScreenH;
	const int iCams = int(m_vMaterials.size());
	const int iCamW = iScrW / iCams;
	if (iCamW <= 0)
		return;

	const float flAlpha = Vars::Visuals::UI::RearViewAlpha.Value;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	for (int i = 0; i < iCams; i++)
	{
		if (!m_vMaterials[i])
			continue;

		if (i < int(m_vAlphaVars.size()) && m_vAlphaVars[i])
			m_vAlphaVars[i]->SetFloatValue(flAlpha);

		const int iDestX = i * iCamW;
		// Mirrored horizontally (src u runs texW -> 0) so it reads like a rear-view mirror.
		pRenderContext->DrawScreenSpaceRectangle(m_vMaterials[i], iDestX, 0, iCamW, iScrH, float(iCamW), 0.f, 0.f, float(iScrH), iCamW, iScrH);
	}
	pRenderContext->Release();
}

void CRearView::Initialize()
{
	// Enemy materials come from the shared Amalgam material list (F::Materials); the
	// per-RT tile and glow scratch materials are created lazily in SetupTargets.
	if (!m_pGlowColor)
	{
		m_pGlowColor = I::MaterialSystem->FindMaterial("dev/glow_color", TEXTURE_GROUP_OTHER);
		m_pGlowColor->IncrementReferenceCount();
		F::Materials.m_mMatList[m_pGlowColor];
	}
}

void CRearView::Unload()
{
	FreeTargets();
	m_iLastW = m_iLastH = m_iLastCams = 0;

	if (m_pGlowColor)
	{
		m_pGlowColor->DecrementReferenceCount();
		m_pGlowColor->DeleteIfUnreferenced();
		m_pGlowColor = nullptr;
	}
}
