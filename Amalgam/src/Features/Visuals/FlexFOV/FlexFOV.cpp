#include "FlexFOV.h"

#include "../Materials/Materials.h"

// World-axis-aligned cube face camera angles (pitch, yaw, roll), indexed by EFace.
static const Vec3 s_vFaceAngles[CFlexFOV::FACE_COUNT] =
{
	{   0.f,    0.f, 0.f }, // FACE_FRONT  -> +X
	{   0.f,  180.f, 0.f }, // FACE_BACK   -> -X
	{   0.f,   90.f, 0.f }, // FACE_LEFT   -> +Y
	{   0.f,  -90.f, 0.f }, // FACE_RIGHT  -> -Y
	{ -90.f,    0.f, 0.f }, // FACE_UP     -> +Z
	{  90.f,    0.f, 0.f }, // FACE_DOWN   -> -Z
};

static const char* s_szFaceNames[CFlexFOV::FACE_COUNT] =
{
	"FlexFOV_Front", "FlexFOV_Back", "FlexFOV_Left", "FlexFOV_Right", "FlexFOV_Up", "FlexFOV_Down"
};

// Renders the scene into a single cube face render target.
void CFlexFOV::RenderFace(void* rcx, const CViewSetup& pViewSetup, EFace eFace)
{
	ITexture* pTexture = m_pFaceTextures[eFace];
	if (!pTexture)
		return;

	CViewSetup tViewSetup = pViewSetup;
	tViewSetup.x = 0;
	tViewSetup.y = 0;
	tViewSetup.width = m_iFaceSize;
	tViewSetup.height = m_iFaceSize;
	tViewSetup.m_flAspectRatio = 1.f;
	tViewSetup.fov = 90.f;			// exact cube face; seam overlap comes later
	tViewSetup.angles = s_vFaceAngles[eFace];
	// origin left as the player's eye (pViewSetup.origin)

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	pRenderContext->PushRenderTargetAndViewport();
	pRenderContext->SetRenderTarget(pTexture);

	static auto CViewRender_RenderView = U::Hooks.m_mHooks["CViewRender_RenderView"];
	CViewRender_RenderView->Call<void>(rcx, tViewSetup, VIEW_CLEAR_COLOR | VIEW_CLEAR_DEPTH, RENDERVIEW_UNSPECIFIED);

	pRenderContext->PopRenderTargetAndViewport();
	pRenderContext->Release();
}

void CFlexFOV::CaptureGlobe(void* rcx, const CViewSetup& pViewSetup)
{
	if (!m_bActive || m_bDrawing || !m_pFaceTextures[0] || !I::EngineClient->IsInGame())
		return;

	m_bDrawing = true;

	for (int i = 0; i < FACE_COUNT; i++)
		RenderFace(rcx, pViewSetup, static_cast<EFace>(i));

	m_bDrawing = false;
}

// Debug: blit the 6 faces as a 3x2 grid of thumbnails in the top-left corner.
void CFlexFOV::DrawDebug()
{
	if (!m_bActive || !m_pFaceMaterials[0] || !I::EngineClient->IsInGame())
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	int iScreenW = 0, iScreenH = 0;
	pRenderContext->GetWindowSize(iScreenW, iScreenH);

	const int iTile = iScreenW / 8;	// thumbnail edge length
	const int iPad = 4;

	for (int i = 0; i < FACE_COUNT; i++)
	{
		if (!m_pFaceMaterials[i])
			continue;

		const int iCol = i % 3;
		const int iRow = i / 3;
		const int iX = iPad + iCol * (iTile + iPad);
		const int iY = iPad + iRow * (iTile + iPad);

		// Pass the texture's *actual* dimensions (not the requested face size) so
		// UVs stay correct even if the engine pads a non-pow2 render target.
		pRenderContext->DrawScreenSpaceRectangle(
			m_pFaceMaterials[i],
			iX, iY, iTile, iTile,
			0, 0, m_iFaceSize, m_iFaceSize,
			m_pFaceTextures[i]->GetActualWidth(), m_pFaceTextures[i]->GetActualHeight(),
			nullptr, 1, 1
		);
	}

	pRenderContext->Release();
}

void CFlexFOV::Initialize()
{
	// Square faces sized to screen height.
	int iScreenW = 0, iScreenH = 0;
	I::MaterialSystem->GetBackBufferDimensions(iScreenW, iScreenH);
	m_iFaceSize = iScreenH > 0 ? iScreenH : 1024;

	for (int i = 0; i < FACE_COUNT; i++)
	{
		if (!m_pFaceTextures[i])
		{
			m_pFaceTextures[i] = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
				s_szFaceNames[i],
				m_iFaceSize,
				m_iFaceSize,
				RT_SIZE_LITERAL,
				IMAGE_FORMAT_RGB888,
				MATERIAL_RT_DEPTH_SHARED,
				TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
				CREATERENDERTARGETFLAGS_HDR
			);
			m_pFaceTextures[i]->IncrementReferenceCount();
		}

		if (!m_pFaceMaterials[i])
		{
			KeyValues* kv = new KeyValues("UnlitGeneric");
			kv->SetString("$basetexture", s_szFaceNames[i]);
			kv->SetInt("$ignorez", 1);
			kv->SetInt("$nofog", 1);
			m_pFaceMaterials[i] = F::Materials.Create(s_szFaceNames[i], kv);
		}
	}
}

void CFlexFOV::Unload()
{
	for (int i = 0; i < FACE_COUNT; i++)
	{
		if (m_pFaceMaterials[i])
		{
			m_pFaceMaterials[i]->DecrementReferenceCount();
			m_pFaceMaterials[i]->DeleteIfUnreferenced();
			m_pFaceMaterials[i] = nullptr;
		}

		if (m_pFaceTextures[i])
		{
			m_pFaceTextures[i]->DecrementReferenceCount();
			m_pFaceTextures[i]->DeleteIfUnreferenced();
			m_pFaceTextures[i] = nullptr;
		}
	}
}
