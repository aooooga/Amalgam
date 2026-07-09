#include "FlexFOV.h"

#include "../Materials/Materials.h"
#include "../../../SDK/Definitions/Main/IMesh.h"

#include <cmath>
#include <algorithm>
#include <vector>

// Faces are rendered slightly wider than 90 deg so adjacent faces overlap and
// triangles straddling a face boundary still sample valid UVs (hides seams).
static constexpr float FLEXFOV_FACE_FOV = 95.f;

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

// Panini inverse with compression parameter d (0 = rectilinear, 1 = Panini,
// higher = more cylindrical). lenscoord already scaled to the fov.
static Vec3 PaniniInverse(float x, float y, float d)
{
	const float k = x * x / ((d + 1.f) * (d + 1.f));
	const float dscr = k * k * d * d - (k + 1.f) * (k * d * d - 1.f);
	const float clon = (-k * d + std::sqrt(dscr)) / (k + 1.f);
	const float S = (d + 1.f) / (d + clon);
	const float flLon = std::atan2(x, S * clon);
	const float flLat = std::atan2(y, S);
	return LatLonToRay(flLat, flLon);
}

// screen (sx in [-1,1], sy = clip.y/aspect) -> view-space ray (forward = -z).
static Vec3 PaniniRay(float sx, float sy, float flFovXDeg, float d)
{
	const float flLon = flFovXDeg * (3.14159265f / 180.f) * 0.5f; // half-fov, radians
	const float S = (d + 1.f) / (d + std::cos(flLon));
	const float flScale = S * std::sin(flLon); // panini_forward(0, fovx/2).x
	Vec3 vRay = PaniniInverse(sx * flScale, sy * flScale, d);
	vRay.z = -vRay.z;
	return vRay;
}

// Mercator inverse (cylindrical). Used for the 180-270 tier. Scale = fovx/2 rad.
static Vec3 MercatorRay(float sx, float sy, float flFovXDeg)
{
	const float flScale = flFovXDeg * (3.14159265f / 180.f) * 0.5f;
	const float flLon = sx * flScale;
	const float flLat = std::atan(std::sinh(sy * flScale));
	Vec3 vRay = LatLonToRay(flLat, flLon);
	vRay.z = -vRay.z;
	return vRay;
}

// Tier selection (per the project's chosen boundaries): Panini below 180 deg,
// Mercator above, blended across a band around 180 to avoid a visible pop.
// (270-360 clamps to Mercator for now; Winkel-Tripel deferred.)
static Vec3 ScreenToRay(float sx, float sy, float flFovXDeg, float flStrength)
{
	const float flLo = 170.f, flHi = 190.f;
	if (flFovXDeg <= flLo)
		return PaniniRay(sx, sy, flFovXDeg, flStrength);
	if (flFovXDeg >= flHi)
		return MercatorRay(sx, sy, flFovXDeg);

	// Blend the two ray directions and renormalize.
	const float t = (flFovXDeg - flLo) / (flHi - flLo);
	const Vec3 a = PaniniRay(sx, sy, flFovXDeg, flStrength);
	const Vec3 b = MercatorRay(sx, sy, flFovXDeg);
	Vec3 m(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
	const float len = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
	if (len > 1e-6f) { m.x /= len; m.y /= len; m.z /= len; }
	return m;
}

// Cube-face bases in view-local ray coords (x = right, y = up, forward = -z),
// consistent with CFlexFOV::ComputeFaceAngles. Order: FRONT,BACK,LEFT,RIGHT,UP,DOWN.
// Fwd = look direction; Right/Up/Back define the face camera (looks along -Back).
static const Vec3 s_FaceFwd[6]   = { {0,0,-1}, {0,0,1},  {-1,0,0}, {1,0,0},  {0,1,0},  {0,-1,0} };
static const Vec3 s_FaceRight[6] = { {1,0,0},  {-1,0,0}, {0,0,-1}, {0,0,1},  {1,0,0},  {1,0,0}  };
static const Vec3 s_FaceUp[6]    = { {0,1,0},  {0,1,0},  {0,1,0},  {0,1,0},  {0,0,1},  {0,0,-1} };
static const Vec3 s_FaceBack[6]  = { {0,0,1},  {0,0,-1}, {1,0,0},  {-1,0,0}, {0,-1,0}, {0,1,0}  };

static inline float Dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct FVert { float x, y, z, u, v; };

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
	tViewSetup.fov = FLEXFOV_FACE_FOV;	// slightly oversized for seam overlap
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

// M4: full reprojection. Tessellate the screen into a grid, map each vertex
// through the inverse projection to a view ray, assign each triangle to the
// cube face its centroid looks at, and draw the triangles in per-face passes
// (each with that face's texture, UVs in that face's frame). Faces are captured
// slightly oversized so boundary triangles still have valid UVs -> no seams.
void CFlexFOV::DrawComposite()
{
	if (!m_bComposite || !m_pFaceMaterials[FACE_FRONT] || !I::EngineClient->IsInGame())
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	int sw = 0, sh = 0;
	pRenderContext->GetWindowSize(sw, sh);
	const float flAspect = sh ? float(sw) / float(sh) : (16.f / 9.f);
	// Driven by the FOV slider; default to 140 when the slider is low/off.
	float flFovX = Vars::Visuals::UI::FieldOfView.Value;
	if (flFovX < 90.f)
		flFovX = 140.f;
	flFovX = std::min(flFovX, 360.f);
	const float flStrength = Vars::Visuals::UI::FlexFOVStrength.Value; // Panini compression d

	// UV scale for a face. Source renders a CViewSetup's fov as a 4:3-referenced
	// horizontal fov scaled by (aspect / (4/3)); our faces are square (aspect 1),
	// so the effective horizontal fov is narrower than the nominal value. Match d
	// to the *effective* fov, else edge rays read central content (zoom-in crop).
	const float flFaceHalf = FLEXFOV_FACE_FOV * 0.5f * (3.14159265f / 180.f);
	const float flEffTan = std::tan(flFaceHalf) * (1.f / (4.f / 3.f));
	const float d = 0.5f / flEffTan;

	const int nGrid = 96; // finer grid: peripheral triangles span less angle at wide FOV
	const int nSide = nGrid + 1;

	// Per-vertex rays and clip positions.
	static std::vector<Vec3> vRays;
	vRays.resize(nSide * nSide);
	for (int j = 0; j < nSide; j++)
	{
		for (int i = 0; i < nSide; i++)
		{
			const float cx = -1.f + 2.f * i / nGrid;
			const float cy = -1.f + 2.f * j / nGrid;
			vRays[j * nSide + i] = ScreenToRay(cx, cy / flAspect, flFovX, flStrength);
		}
	}

	// Bucket each triangle into the face its centroid looks at.
	static std::vector<FVert> vBuckets[FACE_COUNT];
	for (int f = 0; f < FACE_COUNT; f++)
		vBuckets[f].clear();

	auto ClipX = [&](int idx) { return -1.f + 2.f * (idx % nSide) / nGrid; };
	auto ClipY = [&](int idx) { return -1.f + 2.f * (idx / nSide) / nGrid; };

	auto EmitTri = [&](int a, int b, int c)
	{
		const int tri[3] = { a, b, c };

		// Assign the triangle to the face that best contains its *worst* vertex
		// (max over faces of the min per-vertex alignment). Far more robust than
		// a centroid test when a triangle straddles a face boundary at wide FOV.
		int iFace = 0; float flBestMin = -1e30f;
		for (int f = 0; f < FACE_COUNT; f++)
		{
			float flMin = 1e30f;
			for (int t = 0; t < 3; t++)
				flMin = std::min(flMin, Dot3(vRays[tri[t]], s_FaceFwd[f]));
			if (flMin > flBestMin) { flBestMin = flMin; iFace = f; }
		}

		for (int t = 0; t < 3; t++)
		{
			const Vec3& r = vRays[tri[t]];
			float lz = Dot3(r, s_FaceBack[iFace]); // < 0 for the owning face
			if (lz > -0.2f)
				lz = -0.2f; // guard: never let a straddling vertex blow the UV up
			const float lx = Dot3(r, s_FaceRight[iFace]);
			const float ly = Dot3(r, s_FaceUp[iFace]);
			FVert fv;
			fv.x = ClipX(tri[t]); fv.y = ClipY(tri[t]); fv.z = 0.5f;
			fv.u = std::clamp(-lx / lz * d + 0.5f, 0.f, 1.f);
			fv.v = std::clamp(0.5f + ly / lz * d, 0.f, 1.f); // v flipped for D3D top-left origin
			vBuckets[iFace].push_back(fv);
		}
	};

	for (int j = 0; j < nGrid; j++)
	{
		for (int i = 0; i < nGrid; i++)
		{
			const int v00 = j * nSide + i;
			const int v10 = v00 + 1;
			const int v01 = v00 + nSide;
			const int v11 = v01 + 1;
			EmitTri(v00, v01, v11);
			EmitTri(v00, v11, v10);
		}
	}

	// Identity model/view/projection so vertex positions are clip coords.
	pRenderContext->MatrixMode(MATERIAL_PROJECTION);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->MatrixMode(MATERIAL_VIEW);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->MatrixMode(MATERIAL_MODEL);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();

	// One draw pass per face. Chunk each bucket to stay within dynamic limits.
	for (int f = 0; f < FACE_COUNT; f++)
	{
		IMaterial* pMat = m_pFaceMaterials[f];
		if (!pMat || vBuckets[f].empty())
			continue;

		pRenderContext->Bind(pMat);

		int nMaxVerts = pRenderContext->GetMaxVerticesToRender(pMat);
		int nMaxIndices = pRenderContext->GetMaxIndicesToRender();
		int nChunk = std::min(nMaxVerts, nMaxIndices);
		nChunk -= nChunk % 3; // whole triangles
		if (nChunk < 3)
			continue;

		const int nTotal = static_cast<int>(vBuckets[f].size());
		for (int base = 0; base < nTotal; base += nChunk)
		{
			const int nCount = std::min(nChunk, nTotal - base);

			IMesh* pMesh = pRenderContext->GetDynamicMesh(true, nullptr, nullptr, pMat);
			pMesh->SetPrimitiveType(MATERIAL_TRIANGLES);

			MeshDesc_t desc;
			pMesh->LockMesh(nCount, nCount, desc);

			for (int n = 0; n < nCount; n++)
			{
				const FVert& fv = vBuckets[f][base + n];

				float* pPos = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(desc.m_pPosition) + n * desc.m_VertexSize_Position);
				pPos[0] = fv.x; pPos[1] = fv.y; pPos[2] = fv.z;

				float* pUV = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(desc.m_pTexCoord[0]) + n * desc.m_VertexSize_TexCoord[0]);
				pUV[0] = fv.u; pUV[1] = fv.v;

				if (desc.m_VertexSize_Color)
				{
					unsigned char* pCol = desc.m_pColor + n * desc.m_VertexSize_Color;
					pCol[0] = pCol[1] = pCol[2] = pCol[3] = 255;
				}

				desc.m_pIndices[n] = static_cast<unsigned short>(desc.m_nFirstVertex + n);
			}

			pMesh->UnlockMesh(nCount, nCount, desc);
			pMesh->Draw();
		}
	}

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
				// Auto-mipmap so the reprojection samples with trilinear filtering
				// (reduces aliasing where faces are minified toward the edges).
				CREATERENDERTARGETFLAGS_HDR | CREATERENDERTARGETFLAGS_AUTOMIPMAP
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
