#include "RearView.h"

#include "../Materials/Materials.h"
#include "../FlexFOV/FlexFOV.h"
#include "../CameraWindow/CameraWindow.h"

#include <algorithm>

// Reject NaNs and degenerate/too-wide FOVs a single linear projection can't
// represent; fall back to a sane value instead.
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
		// Own (separate) depth per RT so Push3DView has a depth-stencil matching the
		// sub-screen-sized target; the flank pass draws only enemy models so it never
		// needs the main scene's depth.
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

		// Translucent so the transparent (cleared) background shows nothing and only
		// the enemy silhouettes blend over the frame; $alpha is driven live from the
		// alpha slider for the whole overlay's opacity.
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

	return true;
}

void CRearView::FreeTargets()
{
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

void CRearView::GatherVisibleEnemies(CTFPlayer* pLocal)
{
	m_vVisible.clear();

	const Vec3 vMyEye = pLocal->GetShootPos();
	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant())
			continue;

		// Only enemies with a clear line of sight, matching the Lua's IsLineClear.
		if (!SDK::VisPos(pLocal, pPlayer, vMyEye, pPlayer->GetShootPos()))
			continue;

		m_vVisible.push_back(pPlayer);
	}
}

void CRearView::DrawEnemies(IMaterial* pBase)
{
	if (m_vVisible.empty())
		return;

	const Color_t cColor = Vars::Visuals::UI::RearViewColor.Value;
	const bool bGlow = Vars::Visuals::UI::RearViewGlow.Value;

	// Pass 0: the chosen (flat/shaded) silhouette. Pass 1 (glow on): an additive
	// re-draw on top so the silhouettes light up. Color is applied through the
	// studio color modulation, tinting whatever material is bound (same mechanism
	// CGlow uses), with m_bRendering flipped per model so the engine's own
	// per-draw overrides can't stomp ours.
	for (int iPass = 0; iPass < (bGlow ? 2 : 1); iPass++)
	{
		IMaterial* pMat = iPass == 0 ? pBase : m_pMatGlow;
		if (!pMat)
			continue;

		I::ModelRender->ForcedMaterialOverride(pMat);
		I::RenderView->SetColorModulation(cColor);
		I::RenderView->SetBlend(1.f);

		for (auto pPlayer : m_vVisible)
		{
			m_bRendering = true;
			pPlayer->DrawModel(STUDIO_RENDER);
			m_bRendering = false;
		}
	}
	I::ModelRender->ForcedMaterialOverride(nullptr);
}

void CRearView::RenderSideCamera(const CViewSetup& tView, float flYawOffset, float flSideFOV, ITexture* pTexture, int iCamW, int iScrH)
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

	IMaterial* pBase = Vars::Visuals::UI::RearViewMaterial.Value == Vars::Visuals::UI::RearViewMaterialEnum::Shaded ? m_pMatShaded : m_pMatFlat;

	Frustum frustum;
	I::RenderView->Push3DView(tCam, VIEW_CLEAR_COLOR | VIEW_CLEAR_DEPTH, pTexture, frustum);

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	pRenderContext->ClearBuffers(true, true, false);
	pRenderContext->Release();

	DrawEnemies(pBase);

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

	// Front coverage the player already sees: FlexFOV's wide FOV while the
	// composite owns the screen, otherwise the actual rendered view fov, plus the
	// user's manual offset (m_flFovX is only an estimate of the effective warp FOV,
	// so the slider trims the seam until on-screen enemies stop ghosting behind).
	const float flBaseFOV = F::FlexFOV.m_bComposite ? F::FlexFOV.m_flFovX : tView.fov;
	const float flFrontFOV = SanitizeFOV(flBaseFOV + Vars::Visuals::UI::RearViewFOVOffset.Value, 100.f);
	const float flRemaining = 360.f - flFrontFOV;
	if (flRemaining <= 0.f)
		return;

	const int iCamW = iScrW / iCams;
	if (iCamW < 10)
		return;

	const float flPerCam = flRemaining / iCams;
	const float flSideFOV = SanitizeFOV(flPerCam - 1.25f, 125.f); // small seam overlap (OVERLAP_DEG)

	GatherVisibleEnemies(pLocal);

	m_bCapturing = true;
	for (int i = 0; i < iCams; i++)
	{
		// Sweep from the edge of the front cone around the back: each camera centred
		// one coverage step further round than the last.
		const float flYaw = flFrontFOV * 0.5f + (i + 0.5f) * flPerCam;
		RenderSideCamera(tView, flYaw, flSideFOV, m_vTextures[i], iCamW, iScrH);
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
	if (!m_pMatFlat)
	{
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "white");
		kv->SetInt("$model", 1);
		m_pMatFlat = F::Materials.Create("RearViewMatFlat", kv);
	}
	if (!m_pMatShaded)
	{
		KeyValues* kv = new KeyValues("VertexLitGeneric");
		kv->SetString("$basetexture", "white");
		kv->SetInt("$model", 1);
		m_pMatShaded = F::Materials.Create("RearViewMatShaded", kv);
	}
	if (!m_pMatGlow)
	{
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "white");
		kv->SetInt("$model", 1);
		kv->SetInt("$additive", 1);
		m_pMatGlow = F::Materials.Create("RearViewMatGlow", kv);
	}
}

void CRearView::Unload()
{
	FreeTargets();
	m_iLastW = m_iLastH = m_iLastCams = 0;

	for (IMaterial** ppMat : { &m_pMatFlat, &m_pMatShaded, &m_pMatGlow })
	{
		if (*ppMat)
		{
			(*ppMat)->DecrementReferenceCount();
			(*ppMat)->DeleteIfUnreferenced();
			*ppMat = nullptr;
		}
	}
}
