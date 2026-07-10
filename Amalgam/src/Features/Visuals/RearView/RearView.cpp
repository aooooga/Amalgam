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

		// Additive twin of the same RT used for the outline glow: blitting it offset
		// around the silhouette adds a colored halo (same technique CGlow's halo uses,
		// minus the stencil - the sharp tile drawn on top hides the interior add).
		KeyValues* kvG = new KeyValues("UnlitGeneric");
		kvG->SetString("$basetexture", sTex.c_str());
		kvG->SetInt("$additive", 1);
		const std::string sGlow = "RearViewGlowMat" + std::to_string(i);
		m_vGlowMaterials.push_back(F::Materials.Create(sGlow.c_str(), kvG));
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
	for (auto pMat : m_vGlowMaterials)
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
	m_vGlowMaterials.clear();
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

void CRearView::DrawEnemies()
{
	if (m_vVisible.empty())
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	// Draw the visible enemies with the user's material list (same list/handling as
	// chams: each entry is a named material + color), m_bRendering flipped per model
	// so the engine's own per-draw overrides can't stomp ours.
	for (auto& [sName, tColor] : Vars::Visuals::UI::RearViewMaterial.Value)
	{
		auto pMaterial = F::Materials.GetMaterial(FNV1A::Hash32(sName.c_str()));
		F::Materials.SetColor(pMaterial, tColor);
		I::ModelRender->ForcedMaterialOverride(pMaterial ? pMaterial->m_pMaterial : nullptr);

		if (pMaterial && pMaterial->m_bInvertCull)
			pRenderContext->CullMode(MATERIAL_CULLMODE_CW);

		for (auto pPlayer : m_vVisible)
		{
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

	Frustum frustum;
	I::RenderView->Push3DView(tCam, VIEW_CLEAR_COLOR | VIEW_CLEAR_DEPTH, pTexture, frustum);

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	pRenderContext->ClearBuffers(true, true, false);
	pRenderContext->Release();

	DrawEnemies();

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
	// it is CLAMPED, not FOV-sanitized - clamping to <179 was the bug that made the
	// slider dead outside a narrow band. m_flFovX is only an estimate of the
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
	const bool bGlow = Vars::Visuals::UI::RearViewGlow.Value;
	const int iSide = std::max(2, iScrH / 250); // outline halo width, scaled to resolution

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	for (int i = 0; i < iCams; i++)
	{
		if (!m_vMaterials[i])
			continue;

		const int iDestX = i * iCamW;

		// Outline glow: additive blits of the same tile offset around the silhouette
		// (mirrored identically), so a colored halo bleeds past the enemy edges. The
		// sharp tile drawn afterwards covers the interior, leaving the halo as an
		// outline. Src u runs texW -> 0 to mirror like a rear-view mirror.
		if (bGlow && i < int(m_vGlowMaterials.size()) && m_vGlowMaterials[i])
		{
			IMaterial* pGlow = m_vGlowMaterials[i];
			const int aOff[8][2] = { {-iSide,0},{iSide,0},{0,-iSide},{0,iSide},{-iSide,-iSide},{iSide,iSide},{iSide,-iSide},{-iSide,iSide} };
			for (auto& o : aOff)
				pRenderContext->DrawScreenSpaceRectangle(pGlow, iDestX + o[0], o[1], iCamW, iScrH, float(iCamW), 0.f, 0.f, float(iScrH), iCamW, iScrH);
		}

		if (i < int(m_vAlphaVars.size()) && m_vAlphaVars[i])
			m_vAlphaVars[i]->SetFloatValue(flAlpha);

		pRenderContext->DrawScreenSpaceRectangle(m_vMaterials[i], iDestX, 0, iCamW, iScrH, float(iCamW), 0.f, 0.f, float(iScrH), iCamW, iScrH);
	}
	pRenderContext->Release();
}

void CRearView::Initialize()
{
	// Enemy materials come from the shared Amalgam material list (F::Materials);
	// the per-RT tile / glow materials are created lazily in SetupTargets.
}

void CRearView::Unload()
{
	FreeTargets();
	m_iLastW = m_iLastH = m_iLastCams = 0;
}
