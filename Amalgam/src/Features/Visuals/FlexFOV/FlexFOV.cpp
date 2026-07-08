#include "FlexFOV.h"

#include "../Materials/Materials.h"
#include "../../../SDK/Definitions/Main/IMesh.h"

#include <cmath>
#include <algorithm>

// --- Inverse projection math (ported from shaunlebron/flex-fov flex.fs) ------
// Coordinate frame: forward = +z, right = +x, up = +y, then z is negated so the
// front face looks down -z (matching the flex-fov camera-frame convention).

static Vec3 LatLonToRay(float flLat, float flLon)
{
	return Vec3(
		std::sin(flLon) * std::cos(flLat),
		std::sin(flLat),
		std::cos(flLon) * std::cos(flLat)
	);
}

// Panini inverse (compression d = 1). lenscoord already scaled to the fov.
static Vec3 PaniniInverse(float x, float y)
{
	const float d = 1.f;
	const float k = x * x / ((d + 1.f) * (d + 1.f));
	const float dscr = k * k * d * d - (k + 1.f) * (k * d * d - 1.f);
	const float clon = (-k * d + std::sqrt(dscr)) / (k + 1.f);
	const float S = (d + 1.f) / (d + clon);
	const float flLon = std::atan2(x, S * clon);
	const float flLat = std::atan2(y, S);
	return LatLonToRay(flLat, flLon);
}

// screen (sx in [-1,1], sy = clip.y/aspect) -> view-space ray (forward = -z).
static Vec3 PaniniRay(float sx, float sy, float flFovXDeg)
{
	const float flLon = flFovXDeg * (3.14159265f / 180.f) * 0.5f; // half-fov, radians
	const float d = 1.f;
	const float S = (d + 1.f) / (d + std::cos(flLon));
	const float flScale = S * std::sin(flLon); // panini_forward(0, fovx/2).x
	Vec3 vRay = PaniniInverse(sx * flScale, sy * flScale);
	vRay.z = -vRay.z;
	return vRay;
}

static const char* s_szFaceNames[CFlexFOV::FACE_COUNT] =
{
	"FlexFOV_Front", "FlexFOV_Back", "FlexFOV_Left", "FlexFOV_Right", "FlexFOV_Up", "FlexFOV_Down"
};

// Build a QAngle (with roll) from a face's forward/left/up basis vectors.
// Column layout matches Math::AngleMatrix: [forward | left | up].
static Vec3 AnglesFromBasis(const Vec3& vForward, const Vec3& vLeft, const Vec3& vUp)
{
	matrix3x4 m;
	m[0][0] = vForward.x; m[1][0] = vForward.y; m[2][0] = vForward.z;
	m[0][1] = vLeft.x;    m[1][1] = vLeft.y;    m[2][1] = vLeft.z;
	m[0][2] = vUp.x;      m[1][2] = vUp.y;      m[2][2] = vUp.z;
	m[0][3] = m[1][3] = m[2][3] = 0.f;
	return Math::MatrixAngles(m);
}

// Computes the 6 view-aligned cube face orientations from the player's view
// angles, so the front face is always centered on the crosshair (best/most
// consistent resolution where the player looks).
void CFlexFOV::ComputeFaceAngles(const Vec3& vViewAngles, Vec3 vOut[FACE_COUNT])
{
	Vec3 vF, vR, vU;
	Math::AngleVectors(vViewAngles, &vF, &vR, &vU);
	const Vec3 vL = -vR; // matrix "left" column is +Y in Source (= -right)

	vOut[FACE_FRONT] = AnglesFromBasis( vF,  vL,  vU);
	vOut[FACE_BACK]  = AnglesFromBasis(-vF, -vL,  vU);
	vOut[FACE_LEFT]  = AnglesFromBasis( vL, -vF,  vU);
	vOut[FACE_RIGHT] = AnglesFromBasis(-vL,  vF,  vU);
	vOut[FACE_UP]    = AnglesFromBasis( vU,  vL, -vF);
	vOut[FACE_DOWN]  = AnglesFromBasis(-vU,  vL,  vF);
}

// Renders the scene into a single cube face render target.
void CFlexFOV::RenderFace(void* rcx, const CViewSetup& pViewSetup, EFace eFace, const Vec3& vAngles)
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
	tViewSetup.angles = vAngles;
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

	Vec3 vFaceAngles[FACE_COUNT];
	ComputeFaceAngles(pViewSetup.angles, vFaceAngles);

	for (int i = 0; i < FACE_COUNT; i++)
		RenderFace(rcx, pViewSetup, static_cast<EFace>(i), vFaceAngles[i]);

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

// M1 (mesh-ladder step 1): draw a single fullscreen quad textured with the
// FRONT face, via a dynamic mesh. Validates vtable indices, MeshDesc_t stride,
// material bind, and the identity-matrix screen-space setup before we add
// tessellation (M2) and the inverse projection (M3+).
void CFlexFOV::DrawComposite()
{
	if (!m_bComposite || !m_pFaceMaterials[FACE_FRONT] || !I::EngineClient->IsInGame())
		return;

	IMaterial* pMat = m_pFaceMaterials[FACE_FRONT];

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	// Draw positions directly in clip space: identity model/view/projection.
	pRenderContext->MatrixMode(MATERIAL_PROJECTION);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->MatrixMode(MATERIAL_VIEW);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->MatrixMode(MATERIAL_MODEL);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	pRenderContext->Bind(pMat);
	IMesh* pMesh = pRenderContext->GetDynamicMesh(true, nullptr, nullptr, pMat);

	// Tessellated screen-space grid of nGrid x nGrid quads. For M2 the mapping
	// is trivial (uv = screen position), so it must look identical to M1; the
	// inverse projection replaces that mapping in M3. Clamp the resolution so
	// we stay within the dynamic mesh's vertex/index limits.
	int nGrid = 64;
	const int nMaxVerts = pRenderContext->GetMaxVerticesToRender(pMat);
	const int nMaxIndices = pRenderContext->GetMaxIndicesToRender();
	while (nGrid > 1 && ((nGrid + 1) * (nGrid + 1) > nMaxVerts || nGrid * nGrid * 6 > nMaxIndices))
		nGrid /= 2;

	const int nVerts = (nGrid + 1) * (nGrid + 1);
	const int nIndices = nGrid * nGrid * 6;

	int sw = 0, sh = 0;
	pRenderContext->GetWindowSize(sw, sh);
	const float flAspect = sh ? float(sw) / float(sh) : (16.f / 9.f);
	const float flFovX = 140.f; // M3: fixed Panini preview; slider-driven in Phase 4

	pMesh->SetPrimitiveType(MATERIAL_TRIANGLES);

	MeshDesc_t desc;
	pMesh->LockMesh(nVerts, nIndices, desc);

	for (int j = 0; j <= nGrid; j++)
	{
		for (int i = 0; i <= nGrid; i++)
		{
			const int k = j * (nGrid + 1) + i;
			const float cx = -1.f + 2.f * i / nGrid; // clip x, left -> right
			const float cy = -1.f + 2.f * j / nGrid; // clip y, bottom -> top

			float* pPos = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(desc.m_pPosition) + k * desc.m_VertexSize_Position);
			pPos[0] = cx; pPos[1] = cy; pPos[2] = 0.5f;

			// Inverse projection: screen position -> view ray -> FRONT-face UV.
			// (M3 samples the front face only; directions past its 90 deg edge
			// clamp to the border. All 6 faces come in M4.)
			const Vec3 vRay = PaniniRay(cx, cy / flAspect, flFovX);
			float z = vRay.z;
			if (z > -1e-4f)
				z = -1e-4f; // keep in the forward hemisphere so edges clamp cleanly
			const float d = 0.5f; // 0.5 / tan(90/2)
			float u = -vRay.x / z * d + 0.5f;
			float v = 0.5f + vRay.y / z * d; // v flipped vs flex-fov (D3D top-left origin)
			u = std::clamp(u, 0.f, 1.f);
			v = std::clamp(v, 0.f, 1.f);

			float* pUV = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(desc.m_pTexCoord[0]) + k * desc.m_VertexSize_TexCoord[0]);
			pUV[0] = u;
			pUV[1] = v;

			if (desc.m_VertexSize_Color)
			{
				unsigned char* pCol = desc.m_pColor + k * desc.m_VertexSize_Color;
				pCol[0] = pCol[1] = pCol[2] = pCol[3] = 255;
			}
		}
	}

	// Two triangles per quad. Winding doesn't matter ($nocull on the material).
	// Dynamic-mesh indices are offset by the shared buffer's first vertex.
	int n = 0;
	for (int j = 0; j < nGrid; j++)
	{
		for (int i = 0; i < nGrid; i++)
		{
			const unsigned short v00 = static_cast<unsigned short>(desc.m_nFirstVertex + j * (nGrid + 1) + i);
			const unsigned short v10 = v00 + 1;
			const unsigned short v01 = static_cast<unsigned short>(v00 + (nGrid + 1));
			const unsigned short v11 = static_cast<unsigned short>(v01 + 1);

			desc.m_pIndices[n++] = v00; desc.m_pIndices[n++] = v01; desc.m_pIndices[n++] = v11;
			desc.m_pIndices[n++] = v00; desc.m_pIndices[n++] = v11; desc.m_pIndices[n++] = v10;
		}
	}

	pMesh->UnlockMesh(nVerts, nIndices, desc);
	pMesh->Draw();

	pRenderContext->MatrixMode(MATERIAL_PROJECTION);
	pRenderContext->PopMatrix();
	pRenderContext->MatrixMode(MATERIAL_VIEW);
	pRenderContext->PopMatrix();
	pRenderContext->MatrixMode(MATERIAL_MODEL);
	pRenderContext->PopMatrix();

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
			kv->SetInt("$nocull", 1);
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
