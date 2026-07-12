#include "FlexFOV.h"

#include "../Materials/Materials.h"
#include "../Glow/Glow.h"
#include "../../../SDK/Definitions/Main/IMesh.h"

#include <cmath>
#include <algorithm>
#include <vector>

// Face fov. Originally 95 (just enough overlap to hide seams); widened to 130
// because the frame cost is CPU-bound on the NUMBER of scene passes, not their
// resolution (lowering face resolution to 0.35x changed nothing in testing).
// At 130 the front face alone contains everything up to ~115 total fov (ONE
// scene pass), and front+left+right cover ~250 fov (three passes) - the
// composite's triangle assignment prefers the fewest faces and CaptureGlobe
// skips unsampled ones. The wider frustum costs center resolution, which
// DesiredFaceSize compensates by scaling the face RTs up (GPU fill is free per
// the same test). Stitching invariant: d = 0.5/tan(FLEXFOV_FACE_FOV/2)
// everywhere, no UV clamp - fully parametric in this constant.
static constexpr float FLEXFOV_FACE_FOV = 130.f;

// Reference fidelity: a 95deg face at size == screen height (the original
// tuning, confirmed sharp). DesiredFaceSize keeps center pixels-per-radian
// equal to that reference for any FLEXFOV_FACE_FOV.
static constexpr float FLEXFOV_REF_FACE_FOV = 95.f;

// Wide rig: two 130(h) x 145(v) faces yawed +-WideYawDeg around the view up
// axis (a single centered face when the yaw is 0, i.e. fov <= 120). Two passes
// instead of the cube's three in the 130-245 range; the extra vertical fov is
// what lets one face row cover the screen corners that forced the cube's
// up/down faces at high fov.
static constexpr float FLEXFOV_WIDE_FOV_V = 145.f;

static constexpr float FLEXFOV_DEG2RAD = 3.14159265f / 180.f;

static float CurrentFov()
{
	float flFovX = Vars::Visuals::UI::FieldOfView.Value;
	if (flFovX < 90.f)
		flFovX = 140.f;
	return std::min(flFovX, 360.f);
}

// Yaw of the two wide faces. Face half-width is 65deg; keep a 5deg containment
// margin inside it, so each face reaches fov/2 with room for boundary triangles.
static float WideYawDeg(float flFovX)
{
	return std::max(0.f, flFovX * 0.5f - 60.f);
}

static inline float Dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// --- Stereographic projection --------------------------------------------------
// Projection from the point opposite the view axis onto the tangent plane at the
// view axis (unit sphere, plane at z = 1): plane radius r = 2*tan(theta/2) for a
// ray theta off-axis, so it stays conformal (no shape smear) all the way out and
// only diverges at theta -> 180. Scaled like PaniniRay/MercatorRay so sx = +-1
// lands exactly on lon = +-fov/2 along the horizontal axis.
// Native frame here: forward = +z (callers negate z per the flex-fov convention).

// Stereographic is singular at fov 360 (plane radius -> inf); clamp just short.
static float StereoPlaneScale(float flFovXDeg)
{
	const float flHalf = std::min(flFovXDeg, 355.f) * FLEXFOV_DEG2RAD * 0.5f;
	return 2.f * std::tan(flHalf * 0.5f); // r(theta) = 2*tan(theta/2) at theta = fov/2
}

static Vec3 StereographicRay(float sx, float sy, float flFovXDeg)
{
	const float flScale = StereoPlaneScale(flFovXDeg);
	const float x = sx * flScale;
	const float y = sy * flScale;
	const float r2 = x * x + y * y;
	Vec3 vRay(4.f * x / (r2 + 4.f), 4.f * y / (r2 + 4.f), (4.f - r2) / (r2 + 4.f));
	vRay.z = -vRay.z;
	return vRay;
}

static bool StereographicForwardScreen(float fx, float fy, float fz, float flFovXDeg, float& sx, float& sy)
{
	// Singular at the exact anti-axis point (fz = -1); reject just short of it.
	if (fz <= -0.999f)
		return false;
	const float flScale = StereoPlaneScale(flFovXDeg);
	if (flScale < 1e-6f)
		return false;
	sx = (2.f * fx / (1.f + fz)) / flScale;
	sy = (2.f * fy / (1.f + fz)) / flScale;
	return true;
}

// Stereographic blend factor for the current frame: 1 with the full-stereographic
// toggle, otherwise (vertical-stereographic toggle) a smoothstep of |pitch|/90 so
// Panini/Mercator hold when looking at the horizon and stereographic takes over
// toward straight up/down (the original flex-fov pitch behavior), else 0.
static float CurrentStereoBlend(float flPitchDeg)
{
	if (Vars::Visuals::UI::FlexFOVStereographic.Value)
		return 1.f;
	if (!Vars::Visuals::UI::FlexFOVVertStereo.Value)
		return 0.f;
	const float t = std::clamp(std::fabs(flPitchDeg) / 90.f, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

// Worst-case blend the current toggles can reach, for rig selection: the rig must
// stay stable while the player pitches (a rig switch tears down / rebuilds the
// RTs), so it is picked for the extreme of the enabled mode, not the frame value.
static float MaxStereoBlend()
{
	return Vars::Visuals::UI::FlexFOVStereographic.Value || Vars::Visuals::UI::FlexFOVVertStereo.Value ? 1.f : 0.f;
}

// The wide rig covers up to 2*(yaw+65) = 260 horizontally (capped 245 for
// margin), but the screen corners also need vertical room: at a face's edge
// its vertical reach shrinks to atan(tan(72.5)*cos(65)) ~ 57.5deg of latitude,
// and the corner latitude of a Mercator view is atan(sinh((fov/2 rad)/aspect)).
// That works out to roughly fov <= aspect*141deg (16:9 -> ~250). Narrower
// aspects (4:3) fall back to the cube rig earlier.
//
// Stereographic pushes border rays further off-axis than Panini/Mercator at the
// same fov, so when either stereographic toggle is on the wide rig is only kept
// if it still contains the whole screen border at full stereographic (the worst
// case the mode can reach). Blended rays are positive combinations of the two
// endpoint rays, and a face frustum is a convex cone, so checking the endpoints
// covers every intermediate blend. This keeps the cheaper 2-face rig for as long
// as it actually works instead of pessimistically jumping to the cube.
static bool UseWideRig(float flFovX, float flAspect)
{
	if (flFovX > std::min(245.f, flAspect * 141.f))
		return false;
	if (MaxStereoBlend() <= 0.f)
		return true;

	const float flYaw = WideYawDeg(flFovX) * FLEXFOV_DEG2RAD;
	const float s = std::sin(flYaw), c = std::cos(flYaw);
	// Same containment test DrawComposite uses, slightly tighter margin (0.97 vs
	// 0.99) to leave room for boundary triangles.
	const float flLimW = std::tan(FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD) * 0.97f;
	const float flLimH = std::tan(FLEXFOV_WIDE_FOV_V * 0.5f * FLEXFOV_DEG2RAD) * 0.97f;

	const int nSamples = 64; // walk the [-1,1]^2 screen border
	for (int i = 0; i < nSamples; i++)
	{
		const float t = 8.f * i / nSamples; // perimeter param, [0,8)
		float cx, cy;
		if (t < 2.f)      { cx = t - 1.f;  cy = 1.f; }        // top edge
		else if (t < 4.f) { cx = 1.f;      cy = 3.f - t; }    // right edge
		else if (t < 6.f) { cx = 5.f - t;  cy = -1.f; }       // bottom edge
		else              { cx = -1.f;     cy = t - 7.f; }    // left edge

		const Vec3 r = StereographicRay(cx, cy / flAspect, flFovX);
		bool bContained = false;
		for (int f = 0; f < 2 && !bContained; f++)
		{
			const float sgn = f == 0 ? 1.f : -1.f;
			const Vec3 vBack(-sgn * s, 0.f, c);
			const Vec3 vRight(c, 0.f, sgn * s);
			const float lz = Dot3(r, vBack);
			bContained = lz < -0.05f
				&& std::fabs(Dot3(r, vRight)) <= -lz * flLimW
				&& std::fabs(r.y) <= -lz * flLimH;
		}
		if (!bContained)
			return false;
	}
	return true;
}

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
static Vec3 BaseRay(float sx, float sy, float flFovXDeg, float flStrength)
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

// Base (Panini/Mercator) ray blended toward stereographic by flStereo (0 = base,
// 1 = pure stereographic), same blend-and-renormalize as the fov tier band.
static Vec3 ScreenToRay(float sx, float sy, float flFovXDeg, float flStrength, float flStereo)
{
	if (flStereo >= 1.f)
		return StereographicRay(sx, sy, flFovXDeg);
	const Vec3 a = BaseRay(sx, sy, flFovXDeg, flStrength);
	if (flStereo <= 0.f)
		return a;

	const Vec3 b = StereographicRay(sx, sy, flFovXDeg);
	Vec3 m(a.x + (b.x - a.x) * flStereo, a.y + (b.y - a.y) * flStereo, a.z + (b.z - a.z) * flStereo);
	const float len = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
	if (len > 1e-6f) { m.x /= len; m.y /= len; m.z /= len; }
	return m;
}

// --- Forward projection (inverse of ScreenToRay) -----------------------------
// Given a ray in the flex-fov native frame (fx = right, fy = up, fz = forward,
// forward = +z), returns the normalized screen coords (sx, sy) that ScreenToRay
// consumes (sy is the pre-aspect value, i.e. clip.y / aspect). Mirrors the closed
// forms in PaniniRay / MercatorRay exactly so the round-trip is the identity.

static bool PaniniForwardScreen(float fx, float fy, float fz, float flFovXDeg, float d, float& sx, float& sy)
{
	const float flLon = std::atan2(fx, fz);
	const float flClon = std::cos(flLon);
	// S = (d+1)/(d+cos lon) blows up / flips sign as the point swings behind; reject.
	if (flClon <= -d + 1e-3f)
		return false;
	const float flLat = std::asin(std::clamp(fy, -0.9999f, 0.9999f));
	const float S = (d + 1.f) / (d + flClon);
	const float lx = S * std::sin(flLon);
	const float ly = S * std::tan(flLat);

	// flScale exactly as PaniniRay derives it from the half-fov (panini_forward(0,fovx/2).x).
	const float flHalfLon = flFovXDeg * (3.14159265f / 180.f) * 0.5f;
	const float Sedge = (d + 1.f) / (d + std::cos(flHalfLon));
	const float flScale = Sedge * std::sin(flHalfLon);
	if (flScale < 1e-6f)
		return false;

	sx = lx / flScale;
	sy = ly / flScale;
	return true;
}

static bool MercatorForwardScreen(float fx, float fy, float fz, float flFovXDeg, float& sx, float& sy)
{
	const float flScale = flFovXDeg * (3.14159265f / 180.f) * 0.5f;
	if (flScale < 1e-6f)
		return false;
	const float flLon = std::atan2(fx, fz);
	const float flLat = std::asin(std::clamp(fy, -0.9999f, 0.9999f));
	sx = flLon / flScale;
	sy = std::asinh(std::tan(flLat)) / flScale; // inverse of flLat = atan(sinh(sy*scale))
	return true;
}

// Tier selection mirroring BaseRay: Panini below 180, Mercator above, blended
// across 170-190 (blending forward screen-coords is a sub-pixel approximation of
// inverting the ray-blend; fine for point overlays). Fills the pre-aspect (sx,sy).
static bool BaseForwardProject(float fx, float fy, float fz, float flFovXDeg, float flStrength, float& sx, float& sy)
{
	const float flLo = 170.f, flHi = 190.f;

	float sxP = 0.f, syP = 0.f, sxM = 0.f, syM = 0.f;
	const bool bP = (flFovXDeg <= flHi) && PaniniForwardScreen(fx, fy, fz, flFovXDeg, flStrength, sxP, syP);
	const bool bM = (flFovXDeg >= flLo) && MercatorForwardScreen(fx, fy, fz, flFovXDeg, sxM, syM);

	if (flFovXDeg <= flLo)
	{
		if (!bP) return false;
		sx = sxP; sy = syP;
	}
	else if (flFovXDeg >= flHi)
	{
		if (!bM) return false;
		sx = sxM; sy = syM;
	}
	else
	{
		if (!bP || !bM) return false;
		const float t = (flFovXDeg - flLo) / (flHi - flLo);
		sx = sxP + (sxM - sxP) * t;
		sy = syP + (syM - syP) * t;
	}
	return true;
}

// Forward projection mirroring ScreenToRay's stereographic blend (same screen-coord
// lerp approximation as the fov tier band) so overlays track the warped mesh.
static bool ForwardProject(float fx, float fy, float fz, float flFovXDeg, float flStrength, float flStereo, float& sx, float& sy)
{
	if (flStereo >= 1.f)
		return StereographicForwardScreen(fx, fy, fz, flFovXDeg, sx, sy);
	if (flStereo <= 0.f)
		return BaseForwardProject(fx, fy, fz, flFovXDeg, flStrength, sx, sy);

	float sxB = 0.f, syB = 0.f, sxS = 0.f, syS = 0.f;
	if (!BaseForwardProject(fx, fy, fz, flFovXDeg, flStrength, sxB, syB)
		|| !StereographicForwardScreen(fx, fy, fz, flFovXDeg, sxS, syS))
		return false;
	sx = sxB + (sxS - sxB) * flStereo;
	sy = syB + (syS - syB) * flStereo;
	return true;
}

// Cube-face bases in view-local ray coords (x = right, y = up, forward = -z),
// consistent with CFlexFOV::ComputeFaceAngles. Order: FRONT,BACK,LEFT,RIGHT,UP,DOWN.
// Fwd = look direction; Right/Up/Back define the face camera (looks along -Back).
static const Vec3 s_FaceFwd[6]   = { {0,0,-1}, {0,0,1},  {-1,0,0}, {1,0,0},  {0,1,0},  {0,-1,0} };
static const Vec3 s_FaceRight[6] = { {1,0,0},  {-1,0,0}, {0,0,-1}, {0,0,1},  {1,0,0},  {1,0,0}  };
static const Vec3 s_FaceUp[6]    = { {0,1,0},  {0,1,0},  {0,1,0},  {0,1,0},  {0,0,1},  {0,0,-1} };
static const Vec3 s_FaceBack[6]  = { {0,0,1},  {0,0,-1}, {1,0,0},  {-1,0,0}, {0,-1,0}, {0,1,0}  };

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

// The two wide-face orientations: the view basis yawed +-flYawDeg around the
// view up axis (face 0 to the right, face 1 to the left).
void CFlexFOV::ComputeWideAngles(const Vec3& vViewAngles, float flYawDeg, Vec3 vOut[2])
{
	Vec3 vF, vR, vU;
	Math::AngleVectors(vViewAngles, &vF, &vR, &vU);
	const Vec3 vL = -vR;

	const float s = std::sin(flYawDeg * FLEXFOV_DEG2RAD);
	const float c = std::cos(flYawDeg * FLEXFOV_DEG2RAD);
	// fwd' = F*c +- R*s; left' = -(R*c -+ F*s) = L*c +- F*s
	vOut[0] = AnglesFromBasis(vF * c + vR * s, vL * c + vF * s, vU);
	vOut[1] = AnglesFromBasis(vF * c - vR * s, vL * c - vF * s, vU);
}

// Renders the scene into a single face render target. fov is horizontal; the
// engine derives the vertical fov from the aspect (tan(v/2) = tan(h/2)/aspect),
// so the cube's square faces get 130x130 and the wide faces 130x145.
void CFlexFOV::RenderFace(void* rcx, const CViewSetup& pViewSetup, int iFace, const Vec3& vAngles)
{
	ITexture* pTexture = m_pFaceTextures[iFace];
	if (!pTexture)
		return;

	CViewSetup tViewSetup = pViewSetup;
	tViewSetup.x = 0;
	tViewSetup.y = 0;
	tViewSetup.width = m_iFaceW;
	tViewSetup.height = m_iFaceH;
	tViewSetup.m_flAspectRatio = float(m_iFaceW) / float(m_iFaceH);
	tViewSetup.fov = FLEXFOV_FACE_FOV;	// oversized for seam overlap
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

// Only claim to replace the main view when the composite is on AND everything it
// needs to paint a full-screen result exists; otherwise the caller must still
// render the normal view or the frame would be left blank. The GetLocal check
// mirrors the Paint-hook gate: if it fails there, DrawComposite never runs and a
// scene-stripped main pass would leave a stale frame under the HUD.
bool CFlexFOV::ShouldReplaceView()
{
	if (!m_bComposite
		|| !Vars::Visuals::UI::FlexFOVSkipMainView.Value
		|| !m_pFaceTextures[FACE_FRONT] || !m_pFaceMaterials[FACE_FRONT]
		|| !I::EngineClient->IsInGame()
		|| !H::Entities.GetLocal())
		return false;

	// On a rig/size switch frame the composite skips drawing (textures are stale
	// until CaptureGlobe rebuilds them at the end of the frame), so the main view
	// must render normally or the screen would show a stale frame under the HUD.
	bool bWide; int iW, iH;
	ComputeRig(bWide, iW, iH);
	return bWide == m_bWideRig && iW == m_iFaceW && iH == m_iFaceH;
}

// Scene-draw cvars zeroed for the cheap main pass. All are plain render gates
// (some FCVAR_CHEAT, which direct SetValue bypasses); missing ones are skipped.
static const char* s_szSceneCvars[] =
{
	"r_drawworld", "r_drawentities", "r_drawstaticprops",
	"r_drawopaquerenderables", "r_drawtranslucentrenderables", "r_drawtranslucentworld",
	"r_drawviewmodel", "r_skybox", "r_3dsky", "r_drawparticles", "r_shadows",
};
static constexpr int SCENE_CVAR_COUNT = sizeof(s_szSceneCvars) / sizeof(s_szSceneCvars[0]);
static int s_iSavedSceneCvars[SCENE_CVAR_COUNT] = {};

void CFlexFOV::BeginCheapMainView()
{
	m_bReplacingView = true;
	for (int i = 0; i < SCENE_CVAR_COUNT; i++)
	{
		if (auto pVar = H::ConVars.FindVar(s_szSceneCvars[i]))
		{
			s_iSavedSceneCvars[i] = pVar->GetInt();
			pVar->SetValue(0);
		}
	}
}

void CFlexFOV::EndCheapMainView()
{
	for (int i = 0; i < SCENE_CVAR_COUNT; i++)
	{
		if (auto pVar = H::ConVars.FindVar(s_szSceneCvars[i]))
			pVar->SetValue(s_iSavedSceneCvars[i]);
	}
	m_bReplacingView = false;
}

void CFlexFOV::CaptureGlobe(void* rcx, const CViewSetup& pViewSetup)
{
	if (!m_bActive || m_bDrawing || !I::EngineClient->IsInGame())
		return;

	// Destroy faces retired by an earlier rebuild once they're old enough that
	// the queued render thread can no longer hold commands referencing them.
	m_uCaptureFrame++;
	DrainRetired(false);

	// If the fov crossed the rig boundary or the quality slider changed the face
	// size, rebuild the RTs before capturing. Debounced: while the user drags a
	// slider the desired rig can change every frame, and tearing down / creating
	// render targets mid-frame repeatedly is what caused the random crashes on
	// fov changes - so only rebuild after the mismatch has been stable for a
	// moment (the composite falls back to the normal view meanwhile).
	bool bWide; int iW, iH;
	ComputeRig(bWide, iW, iH);
	const bool bMismatch = bWide != m_bWideRig || iW != m_iFaceW || iH != m_iFaceH || !m_pFaceTextures[0];
	static float s_flMismatchStart = -1.f;
	if (!bMismatch)
		s_flMismatchStart = -1.f;
	else
	{
		const float flNow = static_cast<float>(SDK::PlatFloatTime());
		if (s_flMismatchStart < 0.f)
			s_flMismatchStart = flNow;
		if (flNow - s_flMismatchStart > 0.4f)
		{
			// Retire (don't destroy) the old faces: the queued render thread may
			// still replay this/last frame's commands that bind them. They are
			// destroyed by DrainRetired a few frames from now. The glow flex
			// buffers are untouched here - they're screen-sized, so
			// InitFlexBuffers early-outs, and rebuilding them per rig switch was
			// pure create/destroy churn on fixed-name RTs.
			RetireFaces();
			Initialize();
			s_flMismatchStart = -1.f;
		}
		else if (!m_pFaceTextures[0])
			return; // nothing usable yet; wait out the debounce
		// else: keep capturing with the old rig until the rebuild fires
	}
	if (!m_pFaceTextures[0])
		return;

	m_bDrawing = true;

	// Latch the exact eye/angles this composite frame is built from so the forward
	// projection (WorldToScreen) reprojects overlays with the same view basis, and
	// cache the basis vectors so WorldToScreen doesn't redo AngleVectors per call.
	m_vEyeOrigin = pViewSetup.origin;
	m_vViewAngles = pViewSetup.angles;
	Math::AngleVectors(m_vViewAngles, &m_vViewFwd, &m_vViewRight, &m_vViewUp);

	// Latch the whole view setup for DrawViewmodel (same vintage as the faces
	// the next composite will show, so the viewmodel tracks the warped world).
	m_tViewSetup = pViewSetup;
	m_bViewSetupValid = true;

	Vec3 vFaceAngles[FACE_COUNT];
	int nFaces;
	if (m_bWideRig)
	{
		nFaces = 2;
		ComputeWideAngles(pViewSetup.angles, WideYawDeg(CurrentFov()), vFaceAngles);
	}
	else
	{
		nFaces = FACE_COUNT;
		ComputeFaceAngles(pViewSetup.angles, vFaceAngles);
	}

	// Per-pass work that's expensive and barely visible in the warp, dropped for
	// the capture passes only: dynamic entity shadows (CPU per pass) and water
	// reflection (a whole extra scene render per face on water maps).
	static auto r_shadows = H::ConVars.FindVar("r_shadows");
	static auto r_WaterDrawReflection = H::ConVars.FindVar("r_WaterDrawReflection");
	const int iShadows = r_shadows ? r_shadows->GetInt() : 0;
	const int iWaterReflect = r_WaterDrawReflection ? r_WaterDrawReflection->GetInt() : 0;
	if (r_shadows)
		r_shadows->SetValue(0);
	if (r_WaterDrawReflection)
		r_WaterDrawReflection->SetValue(0);

	// Half-diagonal of the face frustum (corner angle from the face axis), used
	// by the glow pass to cull entities this face can't see. Same for every
	// face of a rig: atan(sqrt(tan(hfov/2)^2 + tan(vfov/2)^2)).
	{
		const float tw = std::tan(FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD);
		const float th = m_bWideRig ? std::tan(FLEXFOV_WIDE_FOV_V * 0.5f * FLEXFOV_DEG2RAD) : tw;
		m_flCaptureHalfAngle = std::atan(std::sqrt(tw * tw + th * th));
	}

	// Only render the faces the composite mesh actually samples at the current
	// fov (recorded when the mesh was built). The debug tiles want all of them.
	const bool bAllFaces = !m_bComposite || Vars::Visuals::UI::FlexFOVDebug.Value;
	for (int i = 0; i < nFaces; i++)
	{
		if (bAllFaces || m_bFaceNeeded[i])
		{
			Math::AngleVectors(vFaceAngles[i], &m_vCaptureFwd);
			RenderFace(rcx, pViewSetup, i, vFaceAngles[i]);
		}
	}

	if (r_shadows)
		r_shadows->SetValue(iShadows);
	if (r_WaterDrawReflection)
		r_WaterDrawReflection->SetValue(iWaterReflect);

	m_bDrawing = false;
}

// Debug: blit the 6 faces as a 3x2 grid of thumbnails in the top-left corner.
// Gated on the debug toggle specifically (NOT m_bActive, which is also set by the
// composite) so the tiles don't show whenever the composite alone is enabled.
void CFlexFOV::DrawDebug()
{
	if (!Vars::Visuals::UI::FlexFOVDebug.Value || !m_pFaceMaterials[0] || !I::EngineClient->IsInGame())
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
			0, 0, m_iFaceW, m_iFaceH,
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
	m_bPaintedComposite = false;
	if (!m_bComposite || !m_pFaceMaterials[FACE_FRONT] || !I::EngineClient->IsInGame())
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();

	int sw = 0, sh = 0;
	pRenderContext->GetWindowSize(sw, sh);
	const float flAspect = sh ? float(sw) / float(sh) : (16.f / 9.f);
	// Driven by the FOV slider; default to 140 when the slider is low/off.
	const float flFovX = CurrentFov();
	const float flStrength = Vars::Visuals::UI::FlexFOVStrength.Value; // Panini compression d

	// The mesh must match the rig the textures were captured with; on a rig
	// switch frame skip drawing (CaptureGlobe rebuilds at the end of the frame,
	// and ShouldReplaceView already fell back to the normal render).
	if (UseWideRig(flFovX, flAspect) != m_bWideRig)
	{
		pRenderContext->Release();
		return;
	}

	// Stereographic blend for this frame, from the pitch the faces were captured
	// at. Quantized so the mesh cache below only rebuilds every ~1.5deg of pitch
	// in vertical-stereographic mode instead of every frame while pitching (the
	// per-step warp change is far below a pixel at this granularity).
	const float flStereo = std::round(CurrentStereoBlend(m_vViewAngles.x) * 64.f) / 64.f;

	// Snapshot the projection inputs for WorldToScreen (single source of truth so
	// overlays reproject with the exact same parameters as the mesh warp).
	m_flAspect = flAspect;
	m_flFovX = flFovX;
	m_flStrength = flStrength;
	m_flStereoBlend = flStereo;

	// The warp mesh depends only on (fov, strength, aspect, stereo blend) - never
	// on the view angles directly, because the cube faces are view-aligned (the
	// warp is done entirely in view-local ray space). So the whole grid - inverse
	// projection, triangle face assignment, UVs - is rebuilt only when one of
	// those inputs changes, not per frame. Per frame we just upload the cached
	// buckets.
	// The rig is part of the key: the stereo toggles can flip it while every
	// float input stays identical (e.g. toggling vertical-stereo while looking
	// level), and a cube-rig mesh drawn with wide-rig textures is garbage.
	static std::vector<FVert> vBuckets[FACE_COUNT];
	static float s_flCacheFov = -1.f, s_flCacheStrength = -1.f, s_flCacheAspect = -1.f, s_flCacheStereo = -1.f;
	static int s_iCacheWide = -1;
	if (flFovX != s_flCacheFov || flStrength != s_flCacheStrength || flAspect != s_flCacheAspect || flStereo != s_flCacheStereo
		|| int(m_bWideRig) != s_iCacheWide)
	{
		s_flCacheFov = flFovX; s_flCacheStrength = flStrength; s_flCacheAspect = flAspect; s_flCacheStereo = flStereo;
		s_iCacheWide = int(m_bWideRig);

		// Per-rig face set: bases in view-local ray space, per-axis UV scales
		// (d = 0.5/tan(halfFov), the stitching invariant - per axis now that wide
		// faces are taller than wide) and containment limits (tan(halfFov)*0.99).
		Vec3 vBFwd[FACE_COUNT], vBRight[FACE_COUNT], vBUp[FACE_COUNT], vBBack[FACE_COUNT];
		int iPriority[FACE_COUNT];
		int nFaces;
		const float flHalfW = FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD;
		float flHalfH = flHalfW;
		if (m_bWideRig)
		{
			flHalfH = FLEXFOV_WIDE_FOV_V * 0.5f * FLEXFOV_DEG2RAD;
			const float flYaw = WideYawDeg(flFovX);
			nFaces = flYaw > 0.f ? 2 : 1;
			const float s = std::sin(flYaw * FLEXFOV_DEG2RAD);
			const float c = std::cos(flYaw * FLEXFOV_DEG2RAD);
			for (int f = 0; f < nFaces; f++)
			{
				const float sgn = f == 0 ? 1.f : -1.f; // face 0 right, face 1 left
				vBFwd[f]   = Vec3(sgn * s, 0.f, -c);
				vBRight[f] = Vec3(c, 0.f, sgn * s);
				vBUp[f]    = Vec3(0.f, 1.f, 0.f);
				vBBack[f]  = Vec3(-sgn * s, 0.f, c);
				iPriority[f] = f;
			}
		}
		else
		{
			nFaces = FACE_COUNT;
			// Priority: front first, then sides, back last - pack triangles into
			// the fewest (and cheapest-to-have) faces.
			static const int s_iCubePriority[FACE_COUNT] =
				{ FACE_FRONT, FACE_LEFT, FACE_RIGHT, FACE_UP, FACE_DOWN, FACE_BACK };
			for (int f = 0; f < FACE_COUNT; f++)
			{
				vBFwd[f] = s_FaceFwd[f]; vBRight[f] = s_FaceRight[f];
				vBUp[f] = s_FaceUp[f]; vBBack[f] = s_FaceBack[f];
				iPriority[f] = s_iCubePriority[f];
			}
		}
		const float dU = 0.5f / std::tan(flHalfW);
		const float dV = 0.5f / std::tan(flHalfH);
		const float flLimW = std::tan(flHalfW) * 0.99f;
		const float flLimH = std::tan(flHalfH) * 0.99f;

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
				vRays[j * nSide + i] = ScreenToRay(cx, cy / flAspect, flFovX, flStrength, flStereo);
			}
		}

		// Bucket each triangle into a face.
		for (int f = 0; f < FACE_COUNT; f++)
			vBuckets[f].clear();

		auto ClipX = [&](int idx) { return -1.f + 2.f * (idx % nSide) / nGrid; };
		auto ClipY = [&](int idx) { return -1.f + 2.f * (idx / nSide) / nGrid; };

		auto EmitTri = [&](int a, int b, int c)
		{
			const int tri[3] = { a, b, c };

			// Assign the triangle to the highest-priority face that fully contains
			// all three vertices. Nearest-face assignment would spread triangles
			// across faces that the wide face fov makes unnecessary.
			int iFace = -1;
			for (int p = 0; p < nFaces && iFace < 0; p++)
			{
				const int f = iPriority[p];
				bool bContained = true;
				for (int t = 0; t < 3 && bContained; t++)
				{
					const Vec3& r = vRays[tri[t]];
					const float lz = Dot3(r, vBBack[f]);
					if (lz > -0.05f
						|| std::fabs(Dot3(r, vBRight[f])) > -lz * flLimW
						|| std::fabs(Dot3(r, vBUp[f])) > -lz * flLimH)
						bContained = false;
				}
				if (bContained)
					iFace = f;
			}

			// Fallback for triangles no face fully contains (extreme-periphery
			// smear regions): the face that best contains the *worst* vertex
			// (max over faces of the min per-vertex alignment).
			if (iFace < 0)
			{
				float flBestMin = -1e30f;
				for (int f = 0; f < nFaces; f++)
				{
					float flMin = 1e30f;
					for (int t = 0; t < 3; t++)
						flMin = std::min(flMin, Dot3(vRays[tri[t]], vBFwd[f]));
					if (flMin > flBestMin) { flBestMin = flMin; iFace = f; }
				}
			}

			for (int t = 0; t < 3; t++)
			{
				const Vec3& r = vRays[tri[t]];
				float lz = Dot3(r, vBBack[iFace]); // < 0 for the owning face
				if (lz > -0.2f)
					lz = -0.2f; // guard: never let a straddling vertex blow the UV to infinity
				const float lx = Dot3(r, vBRight[iFace]);
				const float ly = Dot3(r, vBUp[iFace]);
				FVert fv;
				fv.x = ClipX(tri[t]); fv.y = ClipY(tri[t]); fv.z = 0.5f;
				// No UV clamp: faces are captured oversized (fov > the area the
				// containment assigns them) so boundary triangles sample the
				// overlap region and stitch seamlessly.
				fv.u = -lx / lz * dU + 0.5f;
				fv.v = 0.5f + ly / lz * dV; // v flipped for D3D top-left origin
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

		// Record which faces the mesh actually samples so CaptureGlobe only
		// renders those. Face 0 (front / wide-right) stays on as the keystone.
		for (int f = 0; f < FACE_COUNT; f++)
			m_bFaceNeeded[f] = !vBuckets[f].empty();
		m_bFaceNeeded[0] = true;
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

	m_bPaintedComposite = true;
}

// The composite covers every pixel of the main pass, including its viewmodels
// (and the cheap pass strips r_drawviewmodel anyway), so redraw them here on
// top of the composite at their native rectilinear projection - visually
// correct too, since viewmodels shouldn't take part in the globe warp. Depth is
// cleared first: the backbuffer depth still holds whatever the main pass left
// (composite draws with $ignorez), and viewmodels render in the near depth
// range expecting a fresh buffer.
void CFlexFOV::DrawViewmodel()
{
	if (!m_bPaintedComposite || !m_bViewSetupValid)
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	pRenderContext->ClearBuffers(false, true, false);
	pRenderContext->Release();

	// This runs inside the cheap main pass, where BeginCheapMainView zeroed
	// r_drawviewmodel - and DrawViewModels re-checks that cvar internally
	// (ShouldDrawViewModel), so without forcing it back on nothing draws.
	static auto r_drawviewmodel = H::ConVars.FindVar("r_drawviewmodel");
	const int iOld = r_drawviewmodel ? r_drawviewmodel->GetInt() : 1;
	if (r_drawviewmodel)
		r_drawviewmodel->SetValue(1);

	static auto CViewRender_DrawViewModels = U::Hooks.m_mHooks["CViewRender_DrawViewModels"];
	CViewRender_DrawViewModels->Call<void>(I::ViewRender, m_tViewSetup, true);

	if (r_drawviewmodel)
		r_drawviewmodel->SetValue(iOld);
}

// Forward flex-FOV projection used by SDK::W2S while the composite is active.
// world point -> view-local ray -> forward Panini/Mercator -> clip -> pixel.
bool CFlexFOV::WorldToScreen(const Vec3& vWorld, Vec3& vScreen, bool bAlways)
{
	vScreen.z = 0.f;

	// Ray from the (snapshotted) eye to the target, in the cached view basis.
	Vec3 vDir = vWorld - m_vEyeOrigin;
	const float flLen = std::sqrt(Dot3(vDir, vDir));
	if (flLen < 1e-4f)
		return false;
	vDir.x /= flLen; vDir.y /= flLen; vDir.z /= flLen;

	// Native flex-fov frame: fx = right, fy = up, fz = forward (forward = +z).
	const float fx = Dot3(vDir, m_vViewRight);
	const float fy = Dot3(vDir, m_vViewUp);
	const float fz = Dot3(vDir, m_vViewFwd);

	float sx = 0.f, sy = 0.f;
	if (ForwardProject(fx, fy, fz, m_flFovX, m_flStrength, m_flStereoBlend, sx, sy))
	{
		const float cx = sx;                 // clip x (-1..1)
		const float cy = sy * m_flAspect;    // undo ScreenToRay's cy/aspect
		// clip -> pixel, matching SDK::W2S's screen convention (y down).
		vScreen.x = (0.5f + 0.5f * cx) * H::Draw.m_nScreenW;
		vScreen.y = (0.5f - 0.5f * cy) * H::Draw.m_nScreenH;
		return cx >= -1.f && cx <= 1.f && cy >= -1.f && cy <= 1.f;
	}

	// Behind / outside the projectable domain. For bAlways (offscreen arrows) emit
	// a best-effort off-screen direction from the raw angles so the arrow still points.
	if (bAlways)
	{
		const float flLon = std::atan2(fx, fz);
		const float flLat = std::asin(std::clamp(fy, -0.9999f, 0.9999f));
		const float flHalf = std::max(m_flFovX, 1.f) * (3.14159265f / 180.f) * 0.5f;
		const float cx = std::clamp(flLon / flHalf, -8.f, 8.f);
		const float cy = std::clamp(flLat / flHalf * m_flAspect, -8.f, 8.f);
		vScreen.x = (0.5f + 0.5f * cx) * H::Draw.m_nScreenW;
		vScreen.y = (0.5f - 0.5f * cy) * H::Draw.m_nScreenH;
	}
	return false;
}

// Desired face width. Pixels-per-radian at a face center is
// size * 0.5/tan(faceFov/2), so the wide 130deg faces scale their size up by
// tan(faceFov/2)/tan(ref/2) (~1.96x at 130) to keep the same center fidelity
// the original 95deg screen-height faces had. That extra size is GPU fill,
// which testing showed is free (the cost is CPU per scene pass); the quality
// slider still scales it down for GPU-limited machines.
int CFlexFOV::DesiredFaceSize()
{
	int iScreenW = 0, iScreenH = 0;
	I::MaterialSystem->GetBackBufferDimensions(iScreenW, iScreenH);
	const int iBase = iScreenH > 0 ? iScreenH : 1024;
	const float flFidelity = std::tan(FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD)
		/ std::tan(FLEXFOV_REF_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD);
	const float flQuality = std::clamp(Vars::Visuals::UI::FlexFOVQuality.Value, 0.2f, 1.f);
	return std::clamp(static_cast<int>(iBase * flFidelity * flQuality), 256, 4096);
}

void CFlexFOV::ComputeRig(bool& bWide, int& iW, int& iH)
{
	int iScreenW = 0, iScreenH = 0;
	I::MaterialSystem->GetBackBufferDimensions(iScreenW, iScreenH);
	const float flAspect = iScreenH > 0 ? float(iScreenW) / float(iScreenH) : (16.f / 9.f);

	bWide = UseWideRig(CurrentFov(), flAspect);
	iW = iH = DesiredFaceSize();
	if (bWide)
	{
		// Square texels at the face center: H/W = tan(vfov/2)/tan(hfov/2)
		// (~1.48). If that overflows the safe RT ceiling, shrink both axes so
		// texels stay square.
		float flW = float(iW);
		float flH = flW * std::tan(FLEXFOV_WIDE_FOV_V * 0.5f * FLEXFOV_DEG2RAD)
			/ std::tan(FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD);
		if (flH > 4096.f)
		{
			flW *= 4096.f / flH;
			flH = 4096.f;
		}
		iW = static_cast<int>(flW);
		iH = static_cast<int>(flH);
	}
}

void CFlexFOV::Initialize()
{
	ComputeRig(m_bWideRig, m_iFaceW, m_iFaceH);

	// RT creation outside the engine's alloc window needs the explicit
	// begin/end bracket, or the driver-side reallocation can crash when this
	// runs mid-frame (fov/quality change rebuilds).
	I::MaterialSystem->BeginRenderTargetAllocation();

	// Glow silhouette/blur buffers for the face passes. Screen-sized, NOT
	// face-sized: outlines are low-frequency, so rendering them at face
	// resolution (~2x screen per axis) tripled the fill cost of every blur and
	// halo blit for no visible gain. The halo blit upscales into the face.
	{
		int iScreenW = 0, iScreenH = 0;
		I::MaterialSystem->GetBackBufferDimensions(iScreenW, iScreenH);
		F::Glow.InitFlexBuffers(std::max(iScreenW, 640), std::max(iScreenH, 480));
	}

	// A rig/size switch means the mesh (and its recorded face usage) no longer
	// matches; capture everything until DrawComposite rebuilds the mesh and
	// records the real set, or a switched-index face could stay stale.
	for (int i = 0; i < FACE_COUNT; i++)
		m_bFaceNeeded[i] = true;

	// Generation-suffixed names: the old faces are deliberately kept alive for
	// several frames after a rebuild (see RetireFaces/DrainRetired), and
	// CreateNamedRenderTargetTextureEx resolves by name - a recurring name
	// ("FlexFOV_Front") would hand back the still-alive retired RT of the OLD
	// size while m_iFaceW/H describe the new one, making RenderFace set a
	// viewport larger than the target (crash). A generation counter makes every
	// rebuild's names unique.
	static unsigned int s_uGeneration = 0;
	s_uGeneration++;

	const int nFaces = m_bWideRig ? 2 : FACE_COUNT;
	for (int i = 0; i < nFaces; i++)
	{
		char szName[64];
		snprintf(szName, sizeof(szName), "%s_g%u_%dx%d", s_szFaceNames[i], s_uGeneration, m_iFaceW, m_iFaceH);

		if (!m_pFaceTextures[i])
		{
			m_pFaceTextures[i] = I::MaterialSystem->CreateNamedRenderTargetTextureEx(
				szName,
				m_iFaceW,
				m_iFaceH,
				RT_SIZE_LITERAL,
				IMAGE_FORMAT_RGB888,
				// Own depth per face: the fidelity-scaled faces are larger than
				// the screen, so the shared (framebuffer-sized) depth no longer fits.
				MATERIAL_RT_DEPTH_SEPARATE,
				TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
				// Auto-mipmap so the reprojection samples with trilinear filtering
				// (reduces aliasing where faces are minified toward the edges).
				CREATERENDERTARGETFLAGS_HDR | CREATERENDERTARGETFLAGS_AUTOMIPMAP
			);
			if (!m_pFaceTextures[i]) // allocation failure (VRAM) - skip, don't deref
				continue;
			// Undersized result = an aliased stale RT or a driver clamp; using it
			// would overrun the viewport. Drop it and leave the face disabled.
			if (m_pFaceTextures[i]->GetActualWidth() < m_iFaceW || m_pFaceTextures[i]->GetActualHeight() < m_iFaceH)
			{
				m_pFaceTextures[i] = nullptr;
				continue;
			}
			m_pFaceTextures[i]->IncrementReferenceCount();
		}

		if (!m_pFaceMaterials[i])
		{
			KeyValues* kv = new KeyValues("UnlitGeneric");
			kv->SetString("$basetexture", szName);
			kv->SetInt("$ignorez", 1);
			kv->SetInt("$nofog", 1);
			kv->SetInt("$nocull", 1);
			m_pFaceMaterials[i] = F::Materials.Create(szName, kv);
		}
	}

	I::MaterialSystem->EndRenderTargetAllocation();
}

void CFlexFOV::RetireFaces()
{
	for (int i = 0; i < FACE_COUNT; i++)
	{
		if (m_pFaceMaterials[i] || m_pFaceTextures[i])
			m_vRetired.push_back({ m_pFaceMaterials[i], m_pFaceTextures[i], m_uCaptureFrame });
		m_pFaceMaterials[i] = nullptr;
		m_pFaceTextures[i] = nullptr;
	}
}

void CFlexFOV::DrainRetired(bool bAll)
{
	// The queued material system runs at most a couple of frames behind; 8
	// capture frames is a comfortable margin before really destroying anything.
	constexpr unsigned int uSafeAge = 8;

	for (auto it = m_vRetired.begin(); it != m_vRetired.end();)
	{
		if (!bAll && m_uCaptureFrame - it->m_uFrame < uSafeAge)
		{
			++it;
			continue;
		}

		if (it->m_pMaterial)
		{
			// Through F::Materials.Remove, NOT a raw release: Create() registered
			// the material in m_mMatList, and the CMaterial_Uncache hook blocks
			// uncaching (and thus real teardown) for anything still in that list -
			// a raw release both leaves a dangling pointer there and keeps the
			// material (and its $basetexture ref) alive forever.
			F::Materials.Remove(it->m_pMaterial);
		}

		if (it->m_pTexture)
		{
			it->m_pTexture->DecrementReferenceCount();
			it->m_pTexture->DeleteIfUnreferenced();
		}

		it = m_vRetired.erase(it);
	}
}

void CFlexFOV::Unload()
{
	F::Glow.UnloadFlexBuffers();

	RetireFaces();
	DrainRetired(true);
}
