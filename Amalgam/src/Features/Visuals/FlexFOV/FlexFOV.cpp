#include "FlexFOV.h"

#include "../Materials/Materials.h"
#include "../Glow/Glow.h"
#include "../CameraWindow/CameraWindow.h"
#include "../RearView/RearView.h"
#include "../../../SDK/Definitions/Main/IMesh.h"

#include <cmath>
#include <algorithm>
#include <format>
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

// Tight face frusta (see FaceFrustum in the header). MARGIN is added around
// the sampled region on every side before capturing: it's what keeps a stale
// (staggered) face's content valid while the camera rotates between refreshes
// - even without stagger a face is one frame old by the time the composite
// samples it. 8 deg covers ~300 deg/s flicks over the couple-frame windows the
// stagger schedule allows. QUANT snaps the rect's sides outward to a coarse
// angular grid so the capture frustum is stable frame-to-frame (a jittering
// frustum would force a recapture + mesh rebuild every frame).
static constexpr float FLEXFOV_TIGHT_MARGIN_DEG = 8.f;
static constexpr float FLEXFOV_TIGHT_QUANT_DEG = 4.f;

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

// --- Projection tiers (ported from flex-fov's screen_to_ray) --------------------
// The original never renders one projection at full strength on its own: the
// composite cross-fades rectilinear -> Panini -> Mercator as fov rises, with a
// fast-in ease (1-(1-t)^2). The rectilinear fade-in runs from 90 up to the
// transition start; the Mercator fade runs across the transition band itself.
// Original boundaries: 160 - 300, player-tunable via FlexFOVTransition.

static void TransitionBand(float& flLo, float& flHi)
{
	flLo = std::clamp(Vars::Visuals::UI::FlexFOVTransition.Value.Min, 90.f, 360.f);
	flHi = std::clamp(Vars::Visuals::UI::FlexFOVTransition.Value.Max, flLo, 360.f);
}

// flex-fov's tier easing: fast in, level off.
static float TierEase(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return 1.f - (1.f - t) * (1.f - t);
}

// Weight of the Panini/vertical-stereo (hybrid) tier at this fov: 0 at 90 (pure
// rectilinear), easing to 1 at the transition start, easing back to 0 at the
// transition end (pure Mercator). The vertical-stereo pitch blend is scaled by
// this envelope so looking straight up converges to the same projection the
// horizon shows at high fov, instead of popping to full stereographic.
static float HybridWeight(float flFovXDeg, float flLo, float flHi)
{
	if (flFovXDeg >= flHi)
		return 0.f;
	if (flFovXDeg >= flLo)
		return 1.f - TierEase((flFovXDeg - flLo) / std::max(flHi - flLo, 1e-3f));
	if (flFovXDeg <= 90.f)
		return 0.f;
	return TierEase((flFovXDeg - 90.f) / std::max(flLo - 90.f, 1e-3f));
}

// Blend two ray directions and renormalize (the mix() of the flex-fov shader).
static Vec3 NormMix(const Vec3& a, const Vec3& b, float t)
{
	Vec3 m(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
	const float flLen = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
	if (flLen > 1e-6f) { m.x /= flLen; m.y /= flLen; m.z /= flLen; }
	return m;
}

// --- Radial ("round") Panini projection ------------------------------------------
// The Panini compression applied radially symmetrically instead of only around
// the vertical axis: plane radius r(theta) = (d+1)*sin(theta)/(d+cos(theta)) for
// a ray theta off-axis. At d = 1 this is exactly stereographic (2*tan(theta/2)),
// the projection the original flex-fov blends toward when pitching; for any
// other d its central magnification matches PaniniRay's with the same d, so the
// horizon <-> zenith transition never visibly changes zoom or warp strength.
// Scaled like PaniniRay/MercatorRay so sx = +-1 lands exactly on lon = +-fov/2
// along the horizontal axis. Native frame here: forward = +z (callers negate z
// per the flex-fov convention).

// Largest usable ray angle: r diverges at cos(theta) = -d, and for d > 1 it
// peaks (stops being invertible) at cos(theta) = -1/d; stay just short of both.
// (At d = 1 this reproduces the old stereographic near-360 clamp.)
static float RadialMaxHalfAngle(float d)
{
	const float flLimit = std::max(-d, d > 1.f ? -1.f / d : -1.f);
	return std::acos(std::clamp(flLimit + 0.0015f, -1.f, 1.f));
}

static float RadialForwardR(float flTheta, float d)
{
	return (d + 1.f) * std::sin(flTheta) / (d + std::cos(flTheta));
}

static float RadialPlaneScale(float flFovXDeg, float d)
{
	const float flHalf = std::min(flFovXDeg * FLEXFOV_DEG2RAD * 0.5f, RadialMaxHalfAngle(d));
	return RadialForwardR(flHalf, d);
}

static Vec3 RadialRay(float sx, float sy, float flFovXDeg, float d)
{
	const float flScale = RadialPlaneScale(flFovXDeg, d);
	const float x = sx * flScale;
	const float y = sy * flScale;
	const float r = std::sqrt(x * x + y * y);
	if (r < 1e-6f)
		return Vec3(0.f, 0.f, -1.f);
	// Solve r = (d+1)sin(t)/(d+cos(t)) for cos(t): the same quadratic as
	// PaniniInverse's longitude solve (it is that equation with y = 0).
	const float k = r * r / ((d + 1.f) * (d + 1.f));
	const float dscr = k * k * d * d - (k + 1.f) * (k * d * d - 1.f);
	const float ct = std::clamp((-k * d + std::sqrt(std::max(dscr, 0.f))) / (k + 1.f), -1.f, 1.f);
	const float st = std::sqrt(1.f - ct * ct);
	return Vec3(x / r * st, y / r * st, -ct);
}

static bool RadialForwardScreen(float fx, float fy, float fz, float flFovXDeg, float d, float& sx, float& sy)
{
	const float flTheta = std::acos(std::clamp(fz, -1.f, 1.f));
	if (flTheta >= RadialMaxHalfAngle(d))
		return false;
	const float flScale = RadialPlaneScale(flFovXDeg, d);
	if (flScale < 1e-6f)
		return false;
	const float flLen = std::sqrt(fx * fx + fy * fy);
	if (flLen < 1e-6f)
	{
		sx = sy = 0.f; // exactly on-axis (the anti-axis was rejected above)
		return true;
	}
	const float r = RadialForwardR(flTheta, d) / flScale;
	sx = fx / flLen * r;
	sy = fy / flLen * r;
	return true;
}

// Radial-projection blend factor for the current frame: 1 with the full-
// stereographic toggle (pure radial, tiers bypassed), otherwise (vertical-
// stereographic toggle) a smoothstep of |pitch|/90 scaled by the fov-tier
// envelope - Panini/Mercator hold when looking at the horizon, the radial
// projection takes over toward straight up/down, and the whole effect fades
// out as Mercator takes over at high fov (the original flex-fov hybrid
// behavior, which never reaches pure stereographic above the transition
// start) - else 0.
static float CurrentStereoBlend(float flPitchDeg, float flFovX)
{
	if (Vars::Visuals::UI::FlexFOVStereographic.Value)
		return 1.f;
	if (!Vars::Visuals::UI::FlexFOVVertStereo.Value)
		return 0.f;
	float flLo, flHi;
	TransitionBand(flLo, flHi);
	const float t = std::clamp(std::fabs(flPitchDeg) / 90.f, 0.f, 1.f);
	return t * t * (3.f - 2.f * t) * HybridWeight(flFovX, flLo, flHi);
}

// Worst-case blend the current toggles can reach at this fov, for rig selection:
// the rig must stay stable while the player pitches (a rig switch tears down /
// rebuilds the RTs), so it is picked for the extreme of the enabled mode, not
// the frame value.
static float MaxStereoBlend(float flFovX)
{
	if (Vars::Visuals::UI::FlexFOVStereographic.Value)
		return 1.f;
	if (!Vars::Visuals::UI::FlexFOVVertStereo.Value)
		return 0.f;
	float flLo, flHi;
	TransitionBand(flLo, flHi);
	return HybridWeight(flFovX, flLo, flHi);
}

// Full inverse projection, defined below (the tier structure needs the Panini /
// Mercator helpers that follow); UseWideRig walks the screen border through it.
static Vec3 ScreenToRay(float sx, float sy, float flFovXDeg, float flStrength, float flStereo, float flLo, float flHi);

// The wide rig covers up to 2*(yaw+65) = 260 horizontally (capped 245 for
// margin), but the screen corners also need vertical room: at a face's edge
// its vertical reach shrinks to atan(tan(72.5)*cos(65)) ~ 57.5deg of latitude,
// and the corner latitude of a Mercator view is atan(sinh((fov/2 rad)/aspect)).
// That works out to roughly fov <= aspect*141deg (16:9 -> ~250). Narrower
// aspects (4:3) fall back to the cube rig earlier.
//
// The transition band and the stereo toggles both reshape where the border rays
// actually land, so past the coarse gate the whole screen border is walked
// through the real inverse projection. The blended ray is affine in the stereo
// blend and a face frustum is a convex cone, so checking the blend endpoints
// (0 and the worst case the mode can reach at this fov) covers every pitch.
// This keeps the cheaper 2-face rig for as long as it actually works instead of
// pessimistically jumping to the cube.
static bool UseWideRig(float flFovX, float flAspect)
{
	if (flFovX > std::min(245.f, flAspect * 141.f))
		return false;

	float flLo, flHi;
	TransitionBand(flLo, flHi);
	const float flStrength = Vars::Visuals::UI::FlexFOVStrength.Value;
	const float flMaxStereo = MaxStereoBlend(flFovX);

	// The border walk below is ~130 transcendental-heavy ScreenToRay calls and
	// runs several times per frame (ShouldReplaceView / CaptureGlobe /
	// DrawComposite all consult the rig), but every input only changes when a
	// slider or toggle moves - cache the verdict. flMaxStereo folds the stereo
	// toggles in.
	static bool s_bResult = false;
	static float s_flFov = -1.f, s_flAspect = -1.f, s_flStrength = -1.f;
	static float s_flLo = -1.f, s_flHi = -1.f, s_flMaxStereo = -1.f;
	if (flFovX == s_flFov && flAspect == s_flAspect && flStrength == s_flStrength
		&& flLo == s_flLo && flHi == s_flHi && flMaxStereo == s_flMaxStereo)
		return s_bResult;
	s_flFov = flFovX; s_flAspect = flAspect; s_flStrength = flStrength;
	s_flLo = flLo; s_flHi = flHi; s_flMaxStereo = flMaxStereo;
	s_bResult = false;

	const float flYaw = WideYawDeg(flFovX) * FLEXFOV_DEG2RAD;
	const float s = std::sin(flYaw), c = std::cos(flYaw);
	// Same containment test DrawComposite uses, slightly tighter margin (0.97 vs
	// 0.99) to leave room for boundary triangles.
	const float flLimW = std::tan(FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD) * 0.97f;
	const float flLimH = std::tan(FLEXFOV_WIDE_FOV_V * 0.5f * FLEXFOV_DEG2RAD) * 0.97f;

	auto Contained = [&](const Vec3& r)
	{
		for (int f = 0; f < 2; f++)
		{
			const float sgn = f == 0 ? 1.f : -1.f;
			const Vec3 vBack(-sgn * s, 0.f, c);
			const Vec3 vRight(c, 0.f, sgn * s);
			const float lz = Dot3(r, vBack);
			if (lz < -0.05f
				&& std::fabs(Dot3(r, vRight)) <= -lz * flLimW
				&& std::fabs(r.y) <= -lz * flLimH)
				return true;
		}
		return false;
	};

	const int nSamples = 64; // walk the [-1,1]^2 screen border
	for (int i = 0; i < nSamples; i++)
	{
		const float t = 8.f * i / nSamples; // perimeter param, [0,8)
		float cx, cy;
		if (t < 2.f)      { cx = t - 1.f;  cy = 1.f; }        // top edge
		else if (t < 4.f) { cx = 1.f;      cy = 3.f - t; }    // right edge
		else if (t < 6.f) { cx = 5.f - t;  cy = -1.f; }       // bottom edge
		else              { cx = -1.f;     cy = t - 7.f; }    // left edge

		if (!Contained(ScreenToRay(cx, cy / flAspect, flFovX, flStrength, 0.f, flLo, flHi)))
			return false;
		if (flMaxStereo > 0.f
			&& !Contained(ScreenToRay(cx, cy / flAspect, flFovX, flStrength, flMaxStereo, flLo, flHi)))
			return false;
	}
	s_bResult = true;
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

// Mercator inverse (cylindrical). Takes over across the transition band.
// Scale = fovx/2 rad.
static Vec3 MercatorRay(float sx, float sy, float flFovXDeg)
{
	const float flScale = flFovXDeg * (3.14159265f / 180.f) * 0.5f;
	const float flLon = sx * flScale;
	const float flLat = std::atan(std::sinh(sy * flScale));
	Vec3 vRay = LatLonToRay(flLat, flLon);
	vRay.z = -vRay.z;
	return vRay;
}

// Rectilinear ("standard"), the projection the fov slider fades out of between
// 90 and the transition start. tan(fov/2) diverges at 180, so the scale fov is
// clamped: a transition start pushed past ~175 just stops adding rectilinear
// reach instead of flipping the image.
static float StandardPlaneScale(float flFovXDeg)
{
	return std::tan(std::min(flFovXDeg, 175.f) * FLEXFOV_DEG2RAD * 0.5f);
}

static Vec3 StandardRay(float sx, float sy, float flFovXDeg)
{
	const float flScale = StandardPlaneScale(flFovXDeg);
	const float x = sx * flScale;
	const float y = sy * flScale;
	const float flInv = 1.f / std::sqrt(x * x + y * y + 1.f);
	return Vec3(x * flInv, y * flInv, -flInv);
}

// Tier selection (ported from flex-fov's screen_to_ray): rectilinear at 90
// easing into Panini by the transition start, easing into Mercator across the
// transition band, pure Mercator past its end. Every boundary is a smooth
// eased cross-fade, so dragging the fov slider never visibly changes gears -
// and at low fov the warp fades toward a normal view like the original.
static Vec3 BaseRay(float sx, float sy, float flFovXDeg, float flStrength, float flLo, float flHi)
{
	if (flFovXDeg >= flHi)
		return MercatorRay(sx, sy, flFovXDeg);
	if (flFovXDeg > flLo)
		return NormMix(PaniniRay(sx, sy, flFovXDeg, flStrength), MercatorRay(sx, sy, flFovXDeg),
			TierEase((flFovXDeg - flLo) / std::max(flHi - flLo, 1e-3f)));
	if (flFovXDeg <= 90.f)
		return StandardRay(sx, sy, flFovXDeg);
	const float w = TierEase((flFovXDeg - 90.f) / std::max(flLo - 90.f, 1e-3f));
	if (w >= 1.f)
		return PaniniRay(sx, sy, flFovXDeg, flStrength);
	return NormMix(StandardRay(sx, sy, flFovXDeg), PaniniRay(sx, sy, flFovXDeg, flStrength), w);
}

// Base (rectilinear/Panini/Mercator) ray blended toward the radial projection
// by flStereo (0 = base, 1 = pure radial). flStereo arrives already scaled by
// the fov-tier envelope (CurrentStereoBlend); 1 only ever means the full-
// stereographic toggle, which bypasses the tiers entirely.
static Vec3 ScreenToRay(float sx, float sy, float flFovXDeg, float flStrength, float flStereo, float flLo, float flHi)
{
	if (flStereo >= 1.f)
		return RadialRay(sx, sy, flFovXDeg, flStrength);
	const Vec3 a = BaseRay(sx, sy, flFovXDeg, flStrength, flLo, flHi);
	if (flStereo <= 0.f)
		return a;
	return NormMix(a, RadialRay(sx, sy, flFovXDeg, flStrength), flStereo);
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

static bool StandardForwardScreen(float fx, float fy, float fz, float flFovXDeg, float& sx, float& sy)
{
	// Rectilinear only covers the front hemisphere.
	if (fz < 1e-3f)
		return false;
	const float flScale = StandardPlaneScale(flFovXDeg);
	if (flScale < 1e-6f)
		return false;
	sx = fx / (fz * flScale);
	sy = fy / (fz * flScale);
	return true;
}

// Blend forward screen-coords, tolerating one side being unprojectable (a point
// far off-axis can fail one sub-projection while the other still covers it; the
// tier weights make the surviving side the dominant term in that region).
static bool LerpScreen(bool bA, float sxA, float syA, bool bB, float sxB, float syB, float t, float& sx, float& sy)
{
	if (bA && bB) { sx = sxA + (sxB - sxA) * t; sy = syA + (syB - syA) * t; return true; }
	if (bA) { sx = sxA; sy = syA; return true; }
	if (bB) { sx = sxB; sy = syB; return true; }
	return false;
}

// Tier selection mirroring BaseRay (blending forward screen-coords is a
// sub-pixel approximation of inverting the ray-blend; fine for point overlays).
// Fills the pre-aspect (sx,sy).
static bool BaseForwardProject(float fx, float fy, float fz, float flFovXDeg, float flStrength, float flLo, float flHi, float& sx, float& sy)
{
	if (flFovXDeg >= flHi)
		return MercatorForwardScreen(fx, fy, fz, flFovXDeg, sx, sy);

	float sxA = 0.f, syA = 0.f, sxB = 0.f, syB = 0.f;
	if (flFovXDeg > flLo)
	{
		const bool bP = PaniniForwardScreen(fx, fy, fz, flFovXDeg, flStrength, sxA, syA);
		const bool bM = MercatorForwardScreen(fx, fy, fz, flFovXDeg, sxB, syB);
		return LerpScreen(bP, sxA, syA, bM, sxB, syB,
			TierEase((flFovXDeg - flLo) / std::max(flHi - flLo, 1e-3f)), sx, sy);
	}
	if (flFovXDeg <= 90.f)
		return StandardForwardScreen(fx, fy, fz, flFovXDeg, sx, sy);

	const float w = TierEase((flFovXDeg - 90.f) / std::max(flLo - 90.f, 1e-3f));
	const bool bS = w < 1.f && StandardForwardScreen(fx, fy, fz, flFovXDeg, sxA, syA);
	const bool bP = PaniniForwardScreen(fx, fy, fz, flFovXDeg, flStrength, sxB, syB);
	return LerpScreen(bS, sxA, syA, bP, sxB, syB, w, sx, sy);
}

// Forward projection mirroring ScreenToRay's radial blend (same screen-coord
// lerp approximation as the fov tier bands) so overlays track the warped mesh.
static bool ForwardProject(float fx, float fy, float fz, float flFovXDeg, float flStrength, float flStereo, float flLo, float flHi, float& sx, float& sy)
{
	if (flStereo >= 1.f)
		return RadialForwardScreen(fx, fy, fz, flFovXDeg, flStrength, sx, sy);
	if (flStereo <= 0.f)
		return BaseForwardProject(fx, fy, fz, flFovXDeg, flStrength, flLo, flHi, sx, sy);

	float sxB = 0.f, syB = 0.f, sxR = 0.f, syR = 0.f;
	const bool bB = BaseForwardProject(fx, fy, fz, flFovXDeg, flStrength, flLo, flHi, sxB, syB);
	const bool bR = RadialForwardScreen(fx, fy, fz, flFovXDeg, flStrength, sxR, syR);
	return LerpScreen(bB, sxB, syB, bR, sxR, syR, flStereo, sx, sy);
}

// Cube-face bases in view-local ray coords (x = right, y = up, forward = -z).
// Order: FRONT,BACK,LEFT,RIGHT,UP,DOWN. Fwd = look direction; Right/Up complete
// the face camera basis.
static const Vec3 s_FaceFwd[6]   = { {0,0,-1}, {0,0,1},  {-1,0,0}, {1,0,0},  {0,1,0},  {0,-1,0} };
static const Vec3 s_FaceRight[6] = { {1,0,0},  {-1,0,0}, {0,0,-1}, {0,0,1},  {1,0,0},  {1,0,0}  };
static const Vec3 s_FaceUp[6]    = { {0,1,0},  {0,1,0},  {0,1,0},  {0,1,0},  {0,0,1},  {0,0,-1} };

// Canonical (full-fov) face-camera bases for a rig, in view-local ray space
// (x = right, y = up, z = -forward), plus the full per-axis half-fov in
// radians. Shared by the composite's wanted-rect pass and CaptureGlobe so both
// sides agree on what "the whole face" means. Returns the rig's face count.
static int CanonicalRigBases(bool bWide, float flFovX, Vec3 vFwd[], Vec3 vRight[], Vec3 vUp[], float& flHalfW, float& flHalfH)
{
	flHalfW = FLEXFOV_FACE_FOV * 0.5f * FLEXFOV_DEG2RAD;
	flHalfH = flHalfW;
	if (bWide)
	{
		flHalfH = FLEXFOV_WIDE_FOV_V * 0.5f * FLEXFOV_DEG2RAD;
		const float flYaw = WideYawDeg(flFovX);
		const int nFaces = flYaw > 0.f ? 2 : 1;
		const float s = std::sin(flYaw * FLEXFOV_DEG2RAD);
		const float c = std::cos(flYaw * FLEXFOV_DEG2RAD);
		for (int f = 0; f < nFaces; f++)
		{
			const float sgn = f == 0 ? 1.f : -1.f; // face 0 right, face 1 left
			vFwd[f]   = Vec3(sgn * s, 0.f, -c);
			vRight[f] = Vec3(c, 0.f, sgn * s);
			vUp[f]    = Vec3(0.f, 1.f, 0.f);
		}
		return nFaces;
	}
	for (int f = 0; f < CFlexFOV::FACE_COUNT; f++)
	{
		vFwd[f] = s_FaceFwd[f];
		vRight[f] = s_FaceRight[f];
		vUp[f] = s_FaceUp[f];
	}
	return CFlexFOV::FACE_COUNT;
}

// Symmetric capture frustum covering a tangent-space rect on a canonical face:
// re-aim the face camera through the rect's angular center, then take the
// smallest symmetric half-tangents containing the rect's corners in the new
// frame (central projection maps the rect to a convex quad there, so the
// corners are the extremes). A full-face rect reproduces the canonical camera
// and half-tangents exactly.
static CFlexFOV::FaceFrustum FrustumFromRect(const Vec3& vFwd, const Vec3& vRight, const Vec3& vUp,
	float x0, float x1, float y0, float y1)
{
	CFlexFOV::FaceFrustum tOut;
	const float cx = std::tan((std::atan(x0) + std::atan(x1)) * 0.5f);
	const float cy = std::tan((std::atan(y0) + std::atan(y1)) * 0.5f);
	tOut.m_vFwd = (vFwd + vRight * cx + vUp * cy).Normalized();
	tOut.m_vRight = (vRight - tOut.m_vFwd * Dot3(vRight, tOut.m_vFwd)).Normalized();
	tOut.m_vUp = tOut.m_vRight.Cross(tOut.m_vFwd);

	const float xs[2] = { x0, x1 }, ys[2] = { y0, y1 };
	for (int i = 0; i < 2; i++)
	{
		for (int j = 0; j < 2; j++)
		{
			const Vec3 d = vFwd + vRight * xs[i] + vUp * ys[j];
			const float w = std::max(Dot3(d, tOut.m_vFwd), 1e-3f);
			tOut.m_flTanX = std::max(tOut.m_flTanX, std::fabs(Dot3(d, tOut.m_vRight)) / w);
			tOut.m_flTanY = std::max(tOut.m_flTanY, std::fabs(Dot3(d, tOut.m_vUp)) / w);
		}
	}
	return tOut;
}

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

// Renders the scene into a single face render target. fov is horizontal; the
// engine derives the vertical fov from the aspect (tan(v/2) = tan(h/2)/aspect).
// The frustum is the caller's (possibly tightened) one; the RT stays full size
// regardless - a narrower frustum in the same RT just means denser texels,
// and per the resolution testing GPU fill is free (the cost is CPU per pass).
void CFlexFOV::RenderFace(void* rcx, const CViewSetup& pViewSetup, int iFace, const Vec3& vAngles, float flFovX, float flAspect)
{
	ITexture* pTexture = m_pFaceTextures[iFace];
	if (!pTexture)
		return;

	CViewSetup tViewSetup = pViewSetup;
	tViewSetup.x = 0;
	tViewSetup.y = 0;
	tViewSetup.width = m_iFaceW;
	tViewSetup.height = m_iFaceH;
	tViewSetup.m_flAspectRatio = flAspect;
	tViewSetup.fov = flFovX;
	tViewSetup.angles = vAngles;
	// origin left as the player's eye (pViewSetup.origin)
	// No bloom/tonemap chain per face: it's a downsample+blur+tonemap pass on
	// every capture for a subtle effect the warp washes out anyway (the
	// DoEnginePostProcessing hook skips the engine post pass during captures
	// too; this flag drops the CViewRender-side setup for it).
	tViewSetup.m_bDoBloomAndToneMapping = false;

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

void CFlexFOV::AddStageMs(int iStage, float flMs, int nCount)
{
	int iCtx = CTX_MAIN;
	if (m_bDrawing)
		iCtx = CTX_FACES;
	else if (F::CameraWindow.m_bDrawing || F::RearView.m_bCapturing)
		iCtx = CTX_AUX;

	m_flProfAccum[iStage][iCtx] += flMs;
	if (iStage == PROF_MODELS)
		m_nProfModelsAccum[iCtx] += nCount;
}

void CFlexFOV::SnapshotProfFrame()
{
	memcpy(m_flProfFrame, m_flProfAccum, sizeof(m_flProfFrame));
	memcpy(m_nProfModelsFrame, m_nProfModelsAccum, sizeof(m_nProfModelsFrame));
	memset(m_flProfAccum, 0, sizeof(m_flProfAccum));
	memset(m_nProfModelsAccum, 0, sizeof(m_nProfModelsAccum));
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

	// Canonical rig bases and, per face, the frustum this capture should use:
	// derived from the composite's wanted rect when tight faces apply, the full
	// oversized face otherwise (bootstrap, debug-only tiles, toggle off).
	Vec3 vCanFwd[FACE_COUNT], vCanRight[FACE_COUNT], vCanUp[FACE_COUNT];
	float flHalfW, flHalfH;
	const int nFaces = CanonicalRigBases(m_bWideRig, CurrentFov(), vCanFwd, vCanRight, vCanUp, flHalfW, flHalfH);

	const bool bTight = m_bComposite && Vars::Visuals::UI::FlexFOVTightFaces.Value;
	FaceFrustum tWant[FACE_COUNT];
	bool bFrusChanged[FACE_COUNT] = {};
	for (int i = 0; i < nFaces; i++)
	{
		if (bTight && m_bWantValid[i])
			tWant[i] = FrustumFromRect(vCanFwd[i], vCanRight[i], vCanUp[i],
				m_flWantX0[i], m_flWantX1[i], m_flWantY0[i], m_flWantY1[i]);
		else
		{
			tWant[i].m_vFwd = vCanFwd[i]; tWant[i].m_vRight = vCanRight[i]; tWant[i].m_vUp = vCanUp[i];
			tWant[i].m_flTanX = std::tan(flHalfW);
			tWant[i].m_flTanY = std::tan(flHalfH);
		}
		// A face whose next capture uses a different frustum than its texture
		// holds must recapture NOW (stagger bypass below): the mesh has already
		// moved to the new wanted region and the old content doesn't cover it.
		// Fwd + tangents identify the frustum fully (right/up are derived).
		bFrusChanged[i] = m_bFaceCapValid[i]
			&& (std::fabs(tWant[i].m_flTanX - m_tFaceCapFrustum[i].m_flTanX) > 1e-4f
				|| std::fabs(tWant[i].m_flTanY - m_tFaceCapFrustum[i].m_flTanY) > 1e-4f
				|| tWant[i].m_vFwd.Dot(m_tFaceCapFrustum[i].m_vFwd) < 0.9999995f);
	}

	// Per-pass work that's expensive and barely visible in the warp, dropped for
	// the capture passes only: dynamic entity shadows (CPU per pass), water
	// reflection/refraction (extra scene renders / framebuffer copies per face
	// on water maps), the CPU occlusion system (per-view software tests tuned
	// for a single main view), detail props (per-view sprite list rebuilds) and
	// dynamic lights (a muzzle flash dlight re-lights and re-renders the world
	// surfaces it touches in EVERY face pass - the main cause of fps dips while
	// shooting).
	static const char* s_szCaptureCvars[] =
	{
		"r_shadows", "r_WaterDrawReflection", "r_WaterDrawRefraction",
		"r_occlusion", "r_drawdetailprops", "r_dynamic",
	};
	constexpr int nCaptureCvars = sizeof(s_szCaptureCvars) / sizeof(s_szCaptureCvars[0]);
	int iSavedCaptureCvars[nCaptureCvars] = {};
	for (int i = 0; i < nCaptureCvars; i++)
	{
		if (auto pVar = H::ConVars.FindVar(s_szCaptureCvars[i]))
		{
			iSavedCaptureCvars[i] = pVar->GetInt();
			pVar->SetValue(0);
		}
	}

	// Only render the faces the composite mesh actually samples at the current
	// fov (recorded when the mesh was built). With the composite off the debug
	// tiles are the only consumer and want every face live; with it on, the
	// tiles just show whatever the stagger schedule leaves so the debug view
	// reflects the real capture cost.
	//
	// Staggering (FlexFOVStagger > 0): the keystone face 0 (front / wide-right)
	// refreshes every frame, the remaining needed faces refresh round-robin,
	// budget per frame. DrawComposite rotation-compensates stale faces from
	// their latched capture basis, so camera rotation stays exact and only
	// translation/animation lag a few frames in the periphery. Faces with no
	// valid capture yet (fresh RTs after a rebuild) and faces that sat unneeded
	// long enough to hold ancient content are captured unconditionally.
	const bool bAllFaces = !m_bComposite;
	const int iBudget = bAllFaces ? 0 : Vars::Visuals::UI::FlexFOVStagger.Value;
	bool bCapture[FACE_COUNT] = {};
	for (int i = 0; i < nFaces; i++)
	{
		if (!bAllFaces && !m_bFaceNeeded[i])
			continue;
		bCapture[i] = i == 0 || iBudget <= 0 || !m_bFaceCapValid[i]
			|| bFrusChanged[i] || m_uCaptureFrame - m_uFaceCapFrame[i] > 8;
	}
	// Front at half rate (FlexFOVStaggerFront): the keystone is the most
	// expensive face (it sees the action), and rotation compensation keeps
	// turning smooth while it's stale, so alternate-frame refresh only costs a
	// frame of animation/translation lag at the crosshair. Never skipped when
	// invalid (fresh RTs) so rebuild safety is unchanged.
	//
	// Also engages automatically above 160 fps, where a half-rate crosshair
	// refresh (~3ms of world-content lag) is imperceptible. Smoothed fps with
	// hysteresis (on > 160, off < 145) so it doesn't flap at the boundary -
	// note the loop is self-reinforcing in the good direction: engaging raises
	// fps, which keeps it engaged.
	static float s_flAvgFrameTime = 0.f;
	static double s_flLastFrameTime = 0.0;
	static bool s_bAutoFront = false;
	const double flNow = SDK::PlatFloatTime();
	if (s_flLastFrameTime > 0.0)
	{
		const float flDt = float(flNow - s_flLastFrameTime);
		if (flDt > 0.f && flDt < 1.f)
			s_flAvgFrameTime = s_flAvgFrameTime <= 0.f ? flDt : s_flAvgFrameTime + (flDt - s_flAvgFrameTime) * 0.05f;
	}
	s_flLastFrameTime = flNow;
	const float flFps = s_flAvgFrameTime > 0.f ? 1.f / s_flAvgFrameTime : 0.f;
	if (!s_bAutoFront && flFps > 160.f)
		s_bAutoFront = true;
	else if (s_bAutoFront && flFps < 145.f)
		s_bAutoFront = false;

	if (iBudget > 0 && (Vars::Visuals::UI::FlexFOVStaggerFront.Value || s_bAutoFront)
		&& bCapture[0] && m_bFaceCapValid[0] && !bFrusChanged[0]
		&& m_uCaptureFrame - m_uFaceCapFrame[0] < 2)
		bCapture[0] = false;
	if (iBudget > 0 && nFaces > 1)
	{
		for (int k = 0, nLeft = iBudget; k < nFaces - 1 && nLeft > 0; k++)
		{
			const int i = 1 + (m_iStaggerCursor + k) % (nFaces - 1);
			if (bCapture[i] || !m_bFaceNeeded[i])
				continue;
			bCapture[i] = true;
			if (--nLeft == 0)
				m_iStaggerCursor = (m_iStaggerCursor + k + 1) % (nFaces - 1);
		}
	}

	// Cheap-periphery detail cvars: per-model CPU spent on features nobody can
	// resolve off the front face (eye/flex/teeth rendering, jiggle bones) plus
	// forced low model/static-prop LOD. Pushed around individual peripheral
	// face renders only, so the front face keeps full quality.
	static const struct { const char* m_szName; int m_iCheap; } s_tCheapFaceCvars[] =
	{
		{ "r_eyes", 0 }, { "r_teeth", 0 }, { "r_flex", 0 }, { "r_jiggle_bones", 0 },
		{ "r_lod", 2 }, { "r_staticprop_lod", 3 },
	};
	constexpr int nCheapFaceCvars = sizeof(s_tCheapFaceCvars) / sizeof(s_tCheapFaceCvars[0]);

	m_flCaptureMs = 0.f;
	m_nFacesCaptured = 0;
	for (int i = 0; i < nFaces; i++)
	{
		m_flFaceMs[i] = 0.f;
		if (!bCapture[i])
			continue;

		const double flFaceStart = SDK::PlatFloatTime();

		// Face camera world basis from the (possibly tightened) frustum; ray
		// space is x = view-right, y = view-up, z = -view-forward. The per-face
		// half-diagonal cone (glow / chams cull via FaceCanSee) tightens along
		// with the frustum, so those passes cull harder too.
		const auto ToWorld = [&](const Vec3& v) { return m_vViewRight * v.x + m_vViewUp * v.y - m_vViewFwd * v.z; };
		const Vec3 vWorldFwd = ToWorld(tWant[i].m_vFwd);
		const Vec3 vAngles = AnglesFromBasis(vWorldFwd, ToWorld(tWant[i].m_vRight) * -1.f, ToWorld(tWant[i].m_vUp));
		m_vCaptureFwd = vWorldFwd;
		m_flCaptureHalfAngle = std::atan(std::sqrt(
			tWant[i].m_flTanX * tWant[i].m_flTanX + tWant[i].m_flTanY * tWant[i].m_flTanY));

		// Cheap periphery: every non-front cube face (the wide rig's faces both
		// cover screen-center regions, so it never applies there).
		const bool bCheapFace = Vars::Visuals::UI::FlexFOVCheapPeriphery.Value
			&& !m_bWideRig && i != FACE_FRONT;
		m_bCheapFace = bCheapFace;
		int iSavedCheapCvars[nCheapFaceCvars] = {};
		if (bCheapFace)
		{
			for (int c = 0; c < nCheapFaceCvars; c++)
			{
				if (auto pVar = H::ConVars.FindVar(s_tCheapFaceCvars[c].m_szName))
				{
					iSavedCheapCvars[c] = pVar->GetInt();
					pVar->SetValue(s_tCheapFaceCvars[c].m_iCheap);
				}
			}
		}

		RenderFace(rcx, pViewSetup, i, vAngles,
			2.f * std::atan(tWant[i].m_flTanX) / FLEXFOV_DEG2RAD,
			tWant[i].m_flTanX / std::max(tWant[i].m_flTanY, 1e-4f));

		if (bCheapFace)
		{
			for (int c = 0; c < nCheapFaceCvars; c++)
			{
				if (auto pVar = H::ConVars.FindVar(s_tCheapFaceCvars[c].m_szName))
					pVar->SetValue(iSavedCheapCvars[c]);
			}
		}
		m_bCheapFace = false;
		// The face now holds this frame's view basis; latch it so the composite
		// can rotate rays into this frame when the face goes stale. The frustum
		// latch is what the composite's containment/UV math reads; when it
		// changed (or the face is brand new) the mesh cache must rebuild.
		if (bFrusChanged[i] || !m_bFaceCapValid[i])
			m_uFrustumGen++;
		m_tFaceCapFrustum[i] = tWant[i];
		m_vFaceCapFwd[i] = m_vViewFwd;
		m_vFaceCapRight[i] = m_vViewRight;
		m_vFaceCapUp[i] = m_vViewUp;
		m_uFaceCapFrame[i] = m_uCaptureFrame;
		m_bFaceCapValid[i] = true;

		m_flFaceMs[i] = float((SDK::PlatFloatTime() - flFaceStart) * 1000.0);
		m_flCaptureMs += m_flFaceMs[i];
		m_nFacesCaptured++;
	}

	for (int i = 0; i < nCaptureCvars; i++)
	{
		if (auto pVar = H::ConVars.FindVar(s_szCaptureCvars[i]))
			pVar->SetValue(iSavedCaptureCvars[i]);
	}

	m_bDrawing = false;
}

// Debug: blit the 6 faces as a 3x2 grid of thumbnails in the top-left corner,
// with the per-stage main-thread timings underneath (the scene passes are
// CPU-bound on submission, so this is the number the stagger/quality knobs
// move). Gated on the debug toggle specifically (NOT m_bActive, which is also
// set by the composite) so the tiles don't show whenever the composite alone
// is enabled.
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

	// Windowed averages: raw per-frame ms values jitter too much to read, so
	// every stat accumulates each frame and the *displayed* numbers only
	// refresh twice a second with the window's mean. Per-face values include
	// the frames a face was skipped (0 ms), so they read as the amortized
	// per-frame cost - the number the stagger knobs actually move.
	struct SAvg
	{
		float m_flSum = 0.f; int m_nSamples = 0; float m_flAvg = 0.f;
		void Add(float v) { m_flSum += v; m_nSamples++; }
		void Flush() { m_flAvg = m_nSamples ? m_flSum / m_nSamples : 0.f; m_flSum = 0.f; m_nSamples = 0; }
	};
	static SAvg s_tFace[FACE_COUNT], s_tCapture, s_tComposite, s_tMain, s_tScene, s_tViewmodel, s_tFaces, s_tFrame;
	static SAvg s_tProf[PROF_COUNT][CTX_COUNT], s_tModelCount[CTX_COUNT];
	static double s_flLastFrame = 0.0, s_flLastFlush = 0.0;
	static bool s_bCheap = false, s_bSkipped = false;

	const double flNow = SDK::PlatFloatTime();
	if (s_flLastFrame > 0.0)
	{
		const float flDt = float(flNow - s_flLastFrame);
		if (flDt > 0.f && flDt < 1.f)
			s_tFrame.Add(flDt * 1000.f);
	}
	s_flLastFrame = flNow;

	for (int i = 0; i < FACE_COUNT; i++)
		s_tFace[i].Add(m_flFaceMs[i]);
	s_tCapture.Add(m_flCaptureMs);
	s_tComposite.Add(m_flCompositeMs);
	s_tMain.Add(m_flMainPassMs);
	s_tScene.Add(m_flSceneMs);
	s_tViewmodel.Add(m_flViewmodelMs);
	s_tFaces.Add(float(m_nFacesCaptured));
	for (int s = 0; s < PROF_COUNT; s++)
		for (int c = 0; c < CTX_COUNT; c++)
			s_tProf[s][c].Add(m_flProfFrame[s][c]);
	for (int c = 0; c < CTX_COUNT; c++)
		s_tModelCount[c].Add(float(m_nProfModelsFrame[c]));

	if (s_flLastFlush <= 0.0 || flNow - s_flLastFlush > 0.5)
	{
		for (int i = 0; i < FACE_COUNT; i++)
			s_tFace[i].Flush();
		s_tCapture.Flush(); s_tComposite.Flush(); s_tMain.Flush();
		s_tScene.Flush(); s_tViewmodel.Flush(); s_tFaces.Flush(); s_tFrame.Flush();
		for (int s = 0; s < PROF_COUNT; s++)
			for (int c = 0; c < CTX_COUNT; c++)
				s_tProf[s][c].Flush();
		for (int c = 0; c < CTX_COUNT; c++)
			s_tModelCount[c].Flush();
		s_bCheap = m_bMainPassCheap;
		s_bSkipped = m_bSceneSkipped;
		s_flLastFlush = flNow;
	}

	const auto& fFont = H::Fonts.GetFont(FONT_INDICATORS);
	const Color_t tText = { 255, 255, 255, 255 };
	const Color_t tOutline = { 0, 0, 0, 255 };
	int iY = iPad * 3 + iTile * 2 + 8; // below the 3x2 tile grid
	const int iLine = H::Draw.GetTextSize("W", fFont).y + 2;
	const auto Line = [&](const std::string& sText)
	{
		H::Draw.StringOutlined(fFont, iPad, iY, tText, tOutline, ALIGN_TOPLEFT, sText.c_str());
		iY += iLine;
	};

	Line(std::format("{} rig {}x{}, {:.1f} face(s)/frame, {:.0f} fps ({:.2f} ms)",
		m_bWideRig ? "wide" : "cube", m_iFaceW, m_iFaceH, s_tFaces.m_flAvg,
		s_tFrame.m_flAvg > 0.f ? 1000.f / s_tFrame.m_flAvg : 0.f, s_tFrame.m_flAvg));

	// Main pass: "full" is the normal render; "replaced" is the scene-stripped
	// pass under the composite - "scene skipped" when the ViewDrawScene hook
	// cut the scene's CPU setup out, "cvars only" on the fallback path. The
	// scene share is what the skip removes (only measurable while not skipped).
	const char* szMode = !s_bCheap ? "full" : s_bSkipped ? "replaced, scene skipped" : "replaced, cvars only";
	Line(s_bSkipped
		? std::format("main {:.2f} ms ({})", s_tMain.m_flAvg, szMode)
		: std::format("main {:.2f} ms ({})  scene {:.2f} ms", s_tMain.m_flAvg, szMode, s_tScene.m_flAvg));

	static const char* s_szFaceShort[FACE_COUNT] = { "F", "B", "L", "R", "U", "D" };
	std::string sFaces;
	for (int i = 0; i < FACE_COUNT; i++)
	{
		if (!m_pFaceMaterials[i])
			continue;
		sFaces += std::format("{}{} {}", sFaces.empty() ? "" : "  ", s_szFaceShort[i],
			s_tFace[i].m_flAvg > 0.f ? std::format("{:.2f}", s_tFace[i].m_flAvg) : std::string("-"));
	}
	Line(std::format("capture {:.2f} ms  [{}]", s_tCapture.m_flAvg, sFaces));

	// Per-face capture frusta (h x v degrees) so the tight-faces narrowing is
	// verifiable in-game: full faces read 130x130 (cube) / 130x145 (wide).
	if (Vars::Visuals::UI::FlexFOVTightFaces.Value)
	{
		std::string sTight;
		for (int i = 0; i < FACE_COUNT; i++)
		{
			if (!m_pFaceMaterials[i] || !m_bFaceCapValid[i])
				continue;
			sTight += std::format("{}{} {:.0f}x{:.0f}", sTight.empty() ? "" : "  ", s_szFaceShort[i],
				2.f * std::atan(m_tFaceCapFrustum[i].m_flTanX) / FLEXFOV_DEG2RAD,
				2.f * std::atan(m_tFaceCapFrustum[i].m_flTanY) / FLEXFOV_DEG2RAD);
		}
		Line(std::format("tight frusta  [{}]", sTight));
	}

	Line(std::format("composite {:.2f} ms  viewmodel {:.2f} ms", s_tComposite.m_flAvg, s_tViewmodel.m_flAvg));

	Line(std::format("flexfov total {:.2f} ms (capture + composite + viewmodel + main)",
		s_tCapture.m_flAvg + s_tComposite.m_flAvg + s_tViewmodel.m_flAvg + s_tMain.m_flAvg));

	// Scene-stage breakdown (the mini-vprof buckets), one row per stage with
	// per-context columns, sorted by total cost so the top row is the thing to
	// optimize next. "scene other" is the ViewDrawScene remainder no stage hook
	// accounts for; "models" runs inside world/translucent (a breakdown, not an
	// addition), so it's listed last with its draw counts instead of sorted in.
	iY += iLine / 2;
	Line("stage ms: main / faces / aux");

	struct SRow { const char* m_szName; const SAvg* m_pCtx; };
	SRow tRows[] =
	{
		{ "world", s_tProf[PROF_WORLD] },
		{ "translucent", s_tProf[PROF_TRANSLUCENT] },
		{ "3d skybox", s_tProf[PROF_SKY3D] },
		{ "scene other", nullptr }, // computed below
	};
	float flOther[CTX_COUNT];
	for (int c = 0; c < CTX_COUNT; c++)
		flOther[c] = std::max(0.f, s_tProf[PROF_SCENE][c].m_flAvg - s_tProf[PROF_WORLD][c].m_flAvg
			- s_tProf[PROF_TRANSLUCENT][c].m_flAvg - s_tProf[PROF_SKY3D][c].m_flAvg);

	const auto Cell = [](float flMs) {
		return flMs >= 0.005f ? std::format("{:.2f}", flMs) : std::string("-");
	};
	const auto RowTotal = [&](const SRow& tRow) {
		float flTotal = 0.f;
		for (int c = 0; c < CTX_COUNT; c++)
			flTotal += tRow.m_pCtx ? tRow.m_pCtx[c].m_flAvg : flOther[c];
		return flTotal;
	};
	std::sort(std::begin(tRows), std::end(tRows),
		[&](const SRow& a, const SRow& b) { return RowTotal(a) > RowTotal(b); });

	for (const auto& tRow : tRows)
	{
		std::string sCells;
		for (int c = 0; c < CTX_COUNT; c++)
			sCells += std::format("{}{}", c ? " / " : "", Cell(tRow.m_pCtx ? tRow.m_pCtx[c].m_flAvg : flOther[c]));
		Line(std::format("{:<12} {}  ({:.2f})", tRow.m_szName, sCells, RowTotal(tRow)));
	}

	std::string sModelCells, sModelCounts;
	for (int c = 0; c < CTX_COUNT; c++)
	{
		sModelCells += std::format("{}{}", c ? " / " : "", Cell(s_tProf[PROF_MODELS][c].m_flAvg));
		sModelCounts += std::format("{}{:.0f}", c ? "/" : "", s_tModelCount[c].m_flAvg);
	}
	Line(std::format("{:<12} {}  ({} draws, inside world+transl)", "models", sModelCells, sModelCounts));
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

	const double flCompositeStart = SDK::PlatFloatTime();

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

	// Panini->Mercator transition band from the player's slider.
	float flLo, flHi;
	TransitionBand(flLo, flHi);

	// Radial-projection blend for this frame, from the pitch the faces were
	// captured at. Quantized so the mesh cache below only rebuilds every ~1.5deg
	// of pitch in vertical-stereographic mode instead of every frame while
	// pitching (the per-step warp change is far below a pixel at this
	// granularity).
	const float flStereo = std::round(CurrentStereoBlend(m_vViewAngles.x, flFovX) * 64.f) / 64.f;

	// Snapshot the projection inputs for WorldToScreen (single source of truth so
	// overlays reproject with the exact same parameters as the mesh warp).
	m_flAspect = flAspect;
	m_flFovX = flFovX;
	m_flStrength = flStrength;
	m_flStereoBlend = flStereo;
	m_flTierLo = flLo;
	m_flTierHi = flHi;

	// Rotation compensation for staggered faces: build, per face, the rotation
	// taking a mesh ray from the current view frame (the one m_vView* was
	// latched with, same vintage as the freshest faces) into the frame the face
	// was CAPTURED in. Rotation-only reprojection of world content is exact (no
	// parallax), so a stale face still shows world-correct orientation. Ray
	// space is x = right, y = up, z = -forward; the columns below are the
	// images of those axes (world-ify with the current basis, project onto the
	// capture basis).
	const int nRigFaces = m_bWideRig ? (WideYawDeg(flFovX) > 0.f ? 2 : 1) : FACE_COUNT;
	Vec3 vCapX[FACE_COUNT], vCapY[FACE_COUNT], vCapZ[FACE_COUNT];
	bool bFaceUsable[FACE_COUNT] = {}, bFaceAligned[FACE_COUNT] = {};
	bool bAllAligned = true;
	for (int f = 0; f < nRigFaces; f++)
	{
		if (!m_bFaceCapValid[f] || !m_pFaceMaterials[f])
			continue; // RT never written since the last rebuild: not a candidate
		bFaceUsable[f] = true;
		bFaceAligned[f] = m_vFaceCapFwd[f].Dot(m_vViewFwd) > 0.9999995f
			&& m_vFaceCapUp[f].Dot(m_vViewUp) > 0.9999995f;
		if (bFaceAligned[f])
			continue;
		// Only faces the cached mesh actually samples can invalidate it: an
		// unneeded stale face (e.g. BACK at moderate fov, never re-captured)
		// still gets its matrix as a rebuild candidate but must not force a
		// bucket rebuild every frame.
		if (m_bFaceNeeded[f])
			bAllAligned = false;
		vCapX[f] = Vec3(m_vViewRight.Dot(m_vFaceCapRight[f]), m_vViewRight.Dot(m_vFaceCapUp[f]), -m_vViewRight.Dot(m_vFaceCapFwd[f]));
		vCapY[f] = Vec3(m_vViewUp.Dot(m_vFaceCapRight[f]), m_vViewUp.Dot(m_vFaceCapUp[f]), -m_vViewUp.Dot(m_vFaceCapFwd[f]));
		vCapZ[f] = Vec3(-m_vViewFwd.Dot(m_vFaceCapRight[f]), -m_vViewFwd.Dot(m_vFaceCapUp[f]), m_vViewFwd.Dot(m_vFaceCapFwd[f]));
	}

	// The screen-space warp (the ray grid) depends only on (fov, strength,
	// aspect, stereo blend) - never on the view angles directly, because the
	// warp is done entirely in view-local ray space. The triangle buckets (face
	// assignment + UVs) additionally depend on the per-face capture alignment:
	// with every face fresh (identity) they're cached until a param changes;
	// with any stale face they rebuild per frame from the cached rays (cheap -
	// dot products and a rotate per vertex, no transcendentals).
	// The rig is part of the key: the stereo toggles can flip it while every
	// float input stays identical (e.g. toggling vertical-stereo while looking
	// level), and a cube-rig mesh drawn with wide-rig textures is garbage. The
	// frustum generation is too: a face recaptured with a different (tightened)
	// frustum invalidates the cached UVs even when every param is identical.
	static std::vector<FVert> vBuckets[FACE_COUNT];
	static float s_flCacheFov = -1.f, s_flCacheStrength = -1.f, s_flCacheAspect = -1.f, s_flCacheStereo = -1.f;
	static float s_flCacheLo = -1.f, s_flCacheHi = -1.f;
	static int s_iCacheWide = -1;
	static unsigned int s_uCacheFrusGen = 0xffffffffu;
	static bool s_bBucketsAligned = true; // cached buckets were built all-identity
	const bool bParamsChanged = flFovX != s_flCacheFov || flStrength != s_flCacheStrength || flAspect != s_flCacheAspect || flStereo != s_flCacheStereo
		|| flLo != s_flCacheLo || flHi != s_flCacheHi
		|| int(m_bWideRig) != s_iCacheWide;
	const bool bFullRebuild = bParamsChanged || m_bWantDirty;
	if (bFullRebuild || m_uFrustumGen != s_uCacheFrusGen || !bAllAligned || !s_bBucketsAligned)
	{
		s_flCacheFov = flFovX; s_flCacheStrength = flStrength; s_flCacheAspect = flAspect; s_flCacheStereo = flStereo;
		s_flCacheLo = flLo; s_flCacheHi = flHi;
		s_iCacheWide = int(m_bWideRig);
		s_uCacheFrusGen = m_uFrustumGen;
		s_bBucketsAligned = bAllAligned;

		// Canonical (full-fov) face set for the rig, plus assignment priority.
		Vec3 vCanFwd[FACE_COUNT], vCanRight[FACE_COUNT], vCanUp[FACE_COUNT];
		float flHalfW, flHalfH;
		const int nFaces = CanonicalRigBases(m_bWideRig, flFovX, vCanFwd, vCanRight, vCanUp, flHalfW, flHalfH);
		int iPriority[FACE_COUNT];
		if (m_bWideRig)
		{
			for (int f = 0; f < nFaces; f++)
				iPriority[f] = f;
		}
		else
		{
			// Priority: front first, then sides, back last - pack triangles into
			// the fewest (and cheapest-to-have) faces.
			static const int s_iCubePriority[FACE_COUNT] =
				{ FACE_FRONT, FACE_LEFT, FACE_RIGHT, FACE_UP, FACE_DOWN, FACE_BACK };
			for (int f = 0; f < FACE_COUNT; f++)
				iPriority[f] = s_iCubePriority[f];
		}
		const float flFullTanX = std::tan(flHalfW);
		const float flFullTanY = std::tan(flHalfH);

		// Per-face frustum the current textures were CAPTURED with: bases in the
		// capture view frame's ray space, containment limits (tan * 0.99) and UV
		// scales (d = 0.5/tan, the stitching invariant - now per face and axis).
		// With tight faces off (or before any tightening) these are exactly the
		// canonical full-face values, reproducing the original behavior.
		Vec3 vBFwd[FACE_COUNT], vBRight[FACE_COUNT], vBUp[FACE_COUNT];
		float flLimX[FACE_COUNT] = {}, flLimY[FACE_COUNT] = {};
		float dU[FACE_COUNT] = {}, dV[FACE_COUNT] = {};
		for (int f = 0; f < nFaces; f++)
		{
			if (!bFaceUsable[f])
				continue;
			const FaceFrustum& tFrus = m_tFaceCapFrustum[f];
			vBFwd[f] = tFrus.m_vFwd; vBRight[f] = tFrus.m_vRight; vBUp[f] = tFrus.m_vUp;
			flLimX[f] = tFrus.m_flTanX * 0.99f;
			flLimY[f] = tFrus.m_flTanY * 0.99f;
			dU[f] = 0.5f / tFrus.m_flTanX;
			dV[f] = 0.5f / tFrus.m_flTanY;
		}

		// Peripheral triangles only span enough angle to need the fine grid at
		// very wide fov; below that a coarser mesh is visually identical and
		// cuts the per-frame rebuild + dynamic upload cost by ~2.25x (the
		// composite's whole cost scales with triangle count). Part of the cache
		// key via flFovX.
		const int nGrid = flFovX > 250.f ? 96 : 64;
		const int nSide = nGrid + 1;

		// Per-vertex rays and clip positions. Param-keyed only (never depend on
		// the capture bases), so they survive the per-frame bucket rebuilds
		// staggering causes while the camera turns.
		static std::vector<Vec3> vRays;
		if (bParamsChanged || int(vRays.size()) != nSide * nSide)
		{
			vRays.resize(nSide * nSide);
			for (int j = 0; j < nSide; j++)
			{
				for (int i = 0; i < nSide; i++)
				{
					const float cx = -1.f + 2.f * i / nGrid;
					const float cy = -1.f + 2.f * j / nGrid;
					vRays[j * nSide + i] = ScreenToRay(cx, cy / flAspect, flFovX, flStrength, flStereo, flLo, flHi);
				}
			}
		}

		// --- Wanted capture rects (param-keyed) --------------------------------
		// Redo the triangle assignment against the CANONICAL full-fov faces -
		// deliberately ignoring the (possibly tightened) frusta the current
		// textures hold - and record, per face, the tangent-space bounds of
		// every vertex that lands on it. CaptureGlobe narrows each face's next
		// capture to this rect plus margin. Computing it against the full faces
		// is what lets a face's rect GROW again when the params change: deriving
		// it from the drawn buckets would shrink-lock, since the drawn mask can
		// only assign what the last (tight) capture contains.
		if (bFullRebuild)
		{
			static std::vector<unsigned char> vIdealMask;
			vIdealMask.assign(vRays.size(), 0);
			for (int f = 0; f < nFaces; f++)
			{
				const unsigned char uBit = 1 << f;
				for (size_t n = 0; n < vRays.size(); n++)
				{
					const Vec3& r = vRays[n];
					const float lz = -Dot3(r, vCanFwd[f]);
					if (lz < -0.05f
						&& std::fabs(Dot3(r, vCanRight[f])) <= -lz * flFullTanX * 0.99f
						&& std::fabs(Dot3(r, vCanUp[f])) <= -lz * flFullTanY * 0.99f)
						vIdealMask[n] |= uBit;
				}
			}

			float flX0[FACE_COUNT], flX1[FACE_COUNT], flY0[FACE_COUNT], flY1[FACE_COUNT];
			for (int f = 0; f < FACE_COUNT; f++)
			{
				flX0[f] = flY0[f] = 1e30f;
				flX1[f] = flY1[f] = -1e30f;
			}
			const auto AccumVert = [&](int iFace, int idx)
			{
				const Vec3& r = vRays[idx];
				float lz = -Dot3(r, vCanFwd[iFace]);
				if (lz > -0.2f)
					lz = -0.2f; // same guard as the UV path (fallback verts can straddle)
				const float tx = std::clamp(-Dot3(r, vCanRight[iFace]) / lz, -flFullTanX, flFullTanX);
				const float ty = std::clamp(-Dot3(r, vCanUp[iFace]) / lz, -flFullTanY, flFullTanY);
				flX0[iFace] = std::min(flX0[iFace], tx); flX1[iFace] = std::max(flX1[iFace], tx);
				flY0[iFace] = std::min(flY0[iFace], ty); flY1[iFace] = std::max(flY1[iFace], ty);
			};
			const auto AccumTri = [&](int a, int b, int c)
			{
				// Same priority-then-fallback assignment as EmitTri below, on the
				// canonical faces with the unrotated rays.
				const unsigned char uMask = vIdealMask[a] & vIdealMask[b] & vIdealMask[c];
				int iFace = -1;
				for (int p = 0; p < nFaces && iFace < 0; p++)
				{
					const int f = iPriority[p];
					if (uMask & (1 << f))
						iFace = f;
				}
				if (iFace < 0)
				{
					const int tri[3] = { a, b, c };
					float flBestMin = -1e30f;
					for (int f = 0; f < nFaces; f++)
					{
						float flMin = 1e30f;
						for (int t = 0; t < 3; t++)
							flMin = std::min(flMin, Dot3(vRays[tri[t]], vCanFwd[f]));
						if (flMin > flBestMin) { flBestMin = flMin; iFace = f; }
					}
				}
				AccumVert(iFace, a); AccumVert(iFace, b); AccumVert(iFace, c);
			};
			for (int j = 0; j < nGrid; j++)
			{
				for (int i = 0; i < nGrid; i++)
				{
					const int v00 = j * nSide + i;
					AccumTri(v00, v00 + nSide, v00 + nSide + 1);
					AccumTri(v00, v00 + nSide + 1, v00 + 1);
				}
			}

			// Margin + outward quantization per side, in angle space, clamped to
			// the full face. Margin keeps a stale (staggered) face's content valid
			// while the camera rotates between refreshes; quantization keeps the
			// frustum stable so tiny mesh jitter doesn't force recaptures.
			const auto Widen = [](float flLoTan, float flHiTan, float flHalfDeg, float& flOut0, float& flOut1)
			{
				float a0 = std::atan(flLoTan) / FLEXFOV_DEG2RAD - FLEXFOV_TIGHT_MARGIN_DEG;
				float a1 = std::atan(flHiTan) / FLEXFOV_DEG2RAD + FLEXFOV_TIGHT_MARGIN_DEG;
				a0 = std::floor(a0 / FLEXFOV_TIGHT_QUANT_DEG) * FLEXFOV_TIGHT_QUANT_DEG;
				a1 = std::ceil(a1 / FLEXFOV_TIGHT_QUANT_DEG) * FLEXFOV_TIGHT_QUANT_DEG;
				flOut0 = std::tan(std::max(a0, -flHalfDeg) * FLEXFOV_DEG2RAD);
				flOut1 = std::tan(std::min(a1, flHalfDeg) * FLEXFOV_DEG2RAD);
			};
			const float flHalfDegW = flHalfW / FLEXFOV_DEG2RAD;
			const float flHalfDegH = flHalfH / FLEXFOV_DEG2RAD;
			for (int f = 0; f < FACE_COUNT; f++)
			{
				m_bWantValid[f] = f < nFaces && flX1[f] >= flX0[f];
				if (!m_bWantValid[f])
					continue;
				Widen(flX0[f], flX1[f], flHalfDegW, m_flWantX0[f], m_flWantX1[f]);
				Widen(flY0[f], flY1[f], flHalfDegH, m_flWantY0[f], m_flWantY1[f]);
			}
		}
		m_bWantDirty = false;

		// Per-face view of the rays: fresh (aligned) faces read the shared grid
		// directly; stale faces get the grid rotated into their capture frame;
		// unusable faces (never captured) are excluded from assignment entirely.
		// Only faces the mesh can sample are candidates: in view-local ray space
		// the needed set is fixed by the projection params (rotation never
		// changes which view-local directions the screen shows), so the
		// previous build's m_bFaceNeeded is the right filter - it keeps the
		// per-vertex work below off faces like UP/DOWN/BACK that hold no
		// triangles at this fov. A boundary sliver a rotated stale face drops
		// can then only land on a needed face via the fallback (worst case a
		// one-frame clamp smear at the extreme periphery, subtler than the
		// ancient content the unneeded face would show).
		//
		// Containment is also resolved here, once per vertex per face, into a
		// bitmask - each grid vertex is shared by up to 6 triangles, so testing
		// at the vertex level instead of inside EmitTri does the same work
		// several times cheaper.
		static std::vector<Vec3> vFaceRays[FACE_COUNT];
		static std::vector<unsigned char> vMask; // bit f: vertex ray inside face f's frustum
		vMask.assign(vRays.size(), 0);
		const Vec3* pRays[FACE_COUNT] = {};
		for (int f = 0; f < nFaces; f++)
		{
			// The previous build's needed-set is only a valid filter while the
			// projection params are unchanged (stagger-driven rebuilds): a param
			// change (fov/strength/transition/stereo/rig) can make a previously
			// unneeded face needed, and filtering it out here would push its
			// triangles onto a neighbor's clamped edge (a stretched smear of that
			// face's border) - and leave its bucket empty, latching m_bFaceNeeded
			// false so it never recovers.
			if (!bFaceUsable[f] || (!bFullRebuild && !m_bFaceNeeded[f]))
				continue;
			if (bFaceAligned[f])
				pRays[f] = vRays.data();
			else
			{
				vFaceRays[f].resize(vRays.size());
				for (size_t n = 0; n < vRays.size(); n++)
				{
					const Vec3& r = vRays[n];
					vFaceRays[f][n] = vCapX[f] * r.x + vCapY[f] * r.y + vCapZ[f] * r.z;
				}
				pRays[f] = vFaceRays[f].data();
			}

			const unsigned char uBit = 1 << f;
			for (size_t n = 0; n < vRays.size(); n++)
			{
				const Vec3& r = pRays[f][n];
				const float lz = -Dot3(r, vBFwd[f]);
				if (lz < -0.05f
					&& std::fabs(Dot3(r, vBRight[f])) <= -lz * flLimX[f]
					&& std::fabs(Dot3(r, vBUp[f])) <= -lz * flLimY[f])
					vMask[n] |= uBit;
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
			// all three vertices (precomputed per-vertex masks - containment was
			// evaluated in each face's CAPTURE frame, so a stale face only claims
			// triangles its content actually covers; as it rotates away, its
			// border triangles migrate to fresher faces automatically). Nearest-
			// face assignment would spread triangles across faces that the wide
			// face fov makes unnecessary.
			const unsigned char uMask = vMask[a] & vMask[b] & vMask[c];
			int iFace = -1;
			for (int p = 0; p < nFaces && iFace < 0; p++)
			{
				const int f = iPriority[p];
				if (uMask & (1 << f))
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
					if (!pRays[f])
						continue;
					float flMin = 1e30f;
					for (int t = 0; t < 3; t++)
						flMin = std::min(flMin, Dot3(pRays[f][tri[t]], vBFwd[f]));
					if (flMin > flBestMin) { flBestMin = flMin; iFace = f; }
				}
			}
			if (iFace < 0)
				return; // no usable face at all (first frames after a rebuild)

			for (int t = 0; t < 3; t++)
			{
				const Vec3& r = pRays[iFace][tri[t]];
				float lz = -Dot3(r, vBFwd[iFace]); // < 0 for the owning face
				if (lz > -0.2f)
					lz = -0.2f; // guard: never let a straddling vertex blow the UV to infinity
				const float lx = Dot3(r, vBRight[iFace]);
				const float ly = Dot3(r, vBUp[iFace]);
				FVert fv;
				fv.x = ClipX(tri[t]); fv.y = ClipY(tri[t]); fv.z = 0.5f;
				// No UV clamp: faces are captured with margin around the containment
				// rect (tight) or oversized outright (full), so boundary triangles
				// sample the overlap region and stitch seamlessly.
				fv.u = -lx / lz * dU[iFace] + 0.5f;
				fv.v = 0.5f + ly / lz * dV[iFace]; // v flipped for D3D top-left origin
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

	m_flCompositeMs = float((SDK::PlatFloatTime() - flCompositeStart) * 1000.0);
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
	m_flViewmodelMs = 0.f;
	if (!m_bPaintedComposite || !m_bViewSetupValid)
		return;
	const double flStart = SDK::PlatFloatTime();

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

	m_flViewmodelMs = float((SDK::PlatFloatTime() - flStart) * 1000.0);
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
	if (ForwardProject(fx, fy, fz, m_flFovX, m_flStrength, m_flStereoBlend, m_flTierLo, m_flTierHi, sx, sy))
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

// Whether the face currently being captured (m_bDrawing) can possibly see a
// point: angular test against the face axis, with slack for the entity's own
// extent. Everything the per-face chams / glow passes draw is 2+ model draws
// per entity per face, and at wide fov half the set is behind any given face.
bool CFlexFOV::FaceCanSee(const Vec3& vOrigin, float flExtent)
{
	Vec3 vDelta = vOrigin - m_vEyeOrigin;
	const float flDist = vDelta.Length();
	if (flDist < 150.f) // close enough that the bbox can wrap any frustum
		return true;
	const float flSlack = std::asin(std::min(1.f, flExtent / flDist));
	const float flCos = std::cos(std::min(3.14159f, m_flCaptureHalfAngle + flSlack));
	return vDelta.Dot(m_vCaptureFwd) > flCos * flDist;
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

	// The new RTs hold no content yet: invalidate the capture snapshots so
	// CaptureGlobe force-captures everything needed (bypassing the stagger
	// schedule) before the composite samples any of them. The wanted rects are
	// stale too (face indices remap across a rig switch): drop them so the
	// first captures take the full faces, and have the next mesh build
	// recompute them even if no projection param changed (size-only rebuilds).
	for (int i = 0; i < FACE_COUNT; i++)
	{
		m_bFaceCapValid[i] = false;
		m_bWantValid[i] = false;
	}
	m_bWantDirty = true;

	// Generation-suffixed names: the old faces are deliberately kept alive for
	// several frames after a rebuild (see RetireFaces/DrainRetired), and
	// CreateNamedRenderTargetTextureEx resolves by name - a recurring name
	// ("FlexFOV_Front") would hand back the still-alive retired RT of the OLD
	// size while m_iFaceW/H describe the new one, making RenderFace set a
	// viewport larger than the target (crash). A generation counter makes every
	// rebuild's names unique.
	static unsigned int s_uGeneration = 0;
	s_uGeneration++;

	// Auto-mipmap gives the reprojection trilinear filtering where faces are
	// minified toward the composite edges, but regenerates the full mip chain
	// on every face write. At low quality the faces render near/below screen
	// density (little minification anywhere), so drop the mips there and save
	// the per-write regeneration on the GPU-limited machines that slider is for.
	const bool bMipmaps = std::clamp(Vars::Visuals::UI::FlexFOVQuality.Value, 0.2f, 1.f) > 0.6f;

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
				CREATERENDERTARGETFLAGS_HDR | (bMipmaps ? CREATERENDERTARGETFLAGS_AUTOMIPMAP : 0)
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
