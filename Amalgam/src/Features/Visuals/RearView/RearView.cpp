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
		// the enemy silhouettes blend over the frame when tiled.
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", sTex.c_str());
		kv->SetInt("$translucent", 1);
		kv->SetInt("$vertexalpha", 0);
		const std::string sMat = "RearViewMat" + std::to_string(i);
		IMaterial* pMat = F::Materials.Create(sMat.c_str(), kv);
		if (!pMat)
			return false;
		m_vMaterials.push_back(pMat);
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
}

void CRearView::DrawEnemies(CTFPlayer* pLocal)
{
	if (!m_pChams)
		return;

	const Vec3 vMyEye = pLocal->GetShootPos();

	// Set the override with m_bRendering false so it goes through the
	// ForcedMaterialOverride hook, then flip m_bRendering per model so the engine's
	// own per-draw overrides can't stomp our chams (same dance as CChams).
	I::ModelRender->ForcedMaterialOverride(m_pChams);
	for (auto pEntity : H::Entities.GetGroup(EntityEnum::PlayerEnemy))
	{
		auto pPlayer = pEntity->As<CTFPlayer>();
		if (!pPlayer || !pPlayer->IsAlive() || pPlayer->IsDormant())
			continue;

		// Only enemies with a clear line of sight, matching the Lua's IsLineClear.
		if (!SDK::VisPos(pLocal, pPlayer, vMyEye, pPlayer->GetShootPos()))
			continue;

		m_bRendering = true;
		pPlayer->DrawModel(STUDIO_RENDER);
		m_bRendering = false;
	}
	I::ModelRender->ForcedMaterialOverride(nullptr);
}

void CRearView::RenderSideCamera(const CViewSetup& tView, float flYawOffset, float flSideFOV, ITexture* pTexture, int iCamW, int iScrH, CTFPlayer* pLocal)
{
	if (!pTexture)
		return;

	CViewSetup tCam = tView;
	tCam.x = 0;
	tCam.y = 0;
	tCam.width = iCamW;
	tCam.height = iScrH;
	tCam.m_flAspectRatio = float(iCamW) / float(iScrH);
	tCam.fov = flSideFOV;
	tCam.angles = Vec3(tView.angles.x, tView.angles.y + flYawOffset, tView.angles.z);
	// origin left at the player's eye (tView.origin)

	Frustum frustum;
	I::RenderView->Push3DView(tCam, VIEW_CLEAR_COLOR | VIEW_CLEAR_DEPTH, pTexture, frustum);

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	pRenderContext->ClearColor4ub(0, 0, 0, 0);
	pRenderContext->ClearBuffers(true, true, false);
	pRenderContext->Release();

	DrawEnemies(pLocal);

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
	// composite owns the screen, otherwise the actual rendered view fov. The rest
	// of the 360 is what we fill.
	const float flFrontFOV = SanitizeFOV(F::FlexFOV.m_bComposite ? F::FlexFOV.m_flFovX : tView.fov, 100.f);
	const float flRemaining = 360.f - flFrontFOV;
	if (flRemaining <= 0.f)
		return;

	const int iCamW = iScrW / iCams;
	if (iCamW < 10)
		return;

	const float flPerCam = flRemaining / iCams;
	const float flSideFOV = SanitizeFOV(flPerCam - 1.25f, 125.f); // small seam overlap (OVERLAP_DEG)

	m_bCapturing = true;
	for (int i = 0; i < iCams; i++)
	{
		// Sweep from the edge of the front cone around the back: each camera centred
		// one coverage step further round than the last.
		const float flYaw = flFrontFOV * 0.5f + (i + 0.5f) * flPerCam;
		RenderSideCamera(tView, flYaw, flSideFOV, m_vTextures[i], iCamW, iScrH, pLocal);
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

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	for (int i = 0; i < iCams; i++)
	{
		if (!m_vMaterials[i])
			continue;

		const int iDestX = i * iCamW;
		// Mirrored horizontally (src u runs texW -> 0) so it reads like a rear-view mirror.
		pRenderContext->DrawScreenSpaceRectangle(m_vMaterials[i], iDestX, 0, iCamW, iScrH, float(iCamW), 0.f, 0.f, float(iScrH), iCamW, iScrH);
	}
	pRenderContext->Release();
}

void CRearView::Initialize()
{
	if (!m_pChams)
	{
		KeyValues* kv = new KeyValues("UnlitGeneric");
		kv->SetString("$basetexture", "white");
		kv->SetString("$color", "[1 0.30 0.34]"); // bright red silhouette
		kv->SetInt("$model", 1);
		m_pChams = F::Materials.Create("RearViewChams", kv);
	}
}

void CRearView::Unload()
{
	FreeTargets();
	m_iLastW = m_iLastH = m_iLastCams = 0;

	if (m_pChams)
	{
		m_pChams->DecrementReferenceCount();
		m_pChams->DeleteIfUnreferenced();
		m_pChams = nullptr;
	}
}
