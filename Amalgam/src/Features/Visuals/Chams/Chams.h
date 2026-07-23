#pragma once
#include "../../../SDK/SDK.h"
#include "../../../Utils/Perf/Perf.h"

Enum(Model, Visible, Occluded);

// Everything about an entity + its cham set that DrawModel() needs but that
// doesn't change between render passes. All of it used to be re-derived on
// every call: virtual entity queries (entindex/IsPlayer/IsBaseCombatWeapon),
// deep vector comparisons of the layer lists (Visible == Occluded compares
// every string and MaterialColor_t), and a scan for the weapon body-part bit.
// That ran per entity, per model pass, per scene pass - six FlexFOV faces turn
// two dozen entities into hundreds of repeats of an answer that only changes
// when the config or the entity set does. Resolved once in Store() instead
// (which runs per net update) and carried through.
struct ChamsFacts_t
{
	int m_iIndex = 0;              // entindex()
	bool m_bPlayer = false;        // IsPlayer()
	bool m_bWeaponEntity = false;  // IsBaseCombatWeapon()
	bool m_bUseOwner = false;      // weapon/wearable: screen-test the owner instead
	bool m_bOccluded = false;      // has occluded layers
	bool m_bSame = false;          // Visible == Occluded
	bool m_bWeaponInAny = false;   // any layer carries BODYPART_WEAPON
	// Visible set the engine's own scene draw already reproduces exactly, with
	// an occluded set that still needs the redraw path (see IsPlainOriginal /
	// OriginalChamsOptimization). Precomputed with the optimization's config
	// gate folded in.
	bool m_bPassthrough = false;
};

class CChams
{
private:
	void Begin();
	void End();

	void DrawModel(CBaseEntity* pEntity, const Chams_t& tChams, const ChamsFacts_t& tFacts, IMatRenderContext* pRenderContext, int iModel = ModelEnum::Visible, bool bTwoModel = false);

	// Body-part selection: returns a bone set with deselected parts collapsed
	// (chams then skip that geometry), or nullptr when the full model draws.
	matrix3x4* ApplyBodyPartFilter(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);
	// BodyParts mask of the material currently drawing, set around each
	// DrawModel call in DrawModel(); BODYPART_ALL outside cham passes.
	int m_iActiveBodyParts = BODYPART_ALL;

	void RenderBacktrack(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);
	void RenderFakeAngle(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);

	// Entindex of the entity the local player's crosshair is currently on (eye
	// trace through the aim direction), or 0. Resolved per frame in
	// UpdateTarget() (FRAME_RENDER_START, post-interpolation) so it tracks
	// rendered hitbox positions and the scene's original-model suppression
	// matches what RenderMain() draws the same frame.
	int GetCrosshairTarget(CTFPlayer* pLocal);
	int m_iTargetedEntity = 0;
	// EBodyParts bit of the hitgroup under the crosshair (0 = unknown, matches
	// every layer). Selects which targeted-material layers draw.
	int m_iTargetedPart = 0;

	// The targeted entity's filtered material set, built once per frame in
	// UpdateTarget() rather than per render pass: it allocates a vector of
	// strings, and only one entity is ever targeted.
	Chams_t m_tTarget = {};
	ChamsFacts_t m_tTargetFacts = {};
	bool m_bTargetValid = false;

	struct ChamsInfo_t
	{
		CBaseEntity* m_pEntity;
		Chams_t* m_pChams;      // regular chams, or nullptr if the group has none
		int m_iFlags = 0;
		Chams_t* m_pTargetChams = nullptr; // group's targeted material, or nullptr
		ChamsFacts_t m_tFacts = {};        // facts for m_pChams (see above)
		// Set by RenderMain's pre-pass. m_bCulled: outside the FlexFOV face
		// being captured - redraws skipped, suppression still registered.
		// m_bOffScreen: outside the live main-view camera - skipped entirely.
		bool m_bCulled = false;
		bool m_bOffScreen = false;
	};
	std::vector<ChamsInfo_t> m_vEntities = {};
	// Any entity in the set carries occluded layers. When nothing does, the
	// whole occluded model pass is skipped instead of walking the set to have
	// every entity early-out of it.
	bool m_bAnyOccluded = false;

	// Local player origin for the current pass, for distance-based material
	// colors (resolved once per RenderMain instead of once per entity).
	Vec3 m_vLocalOrigin = {};
	bool m_bLocalValid = false;

	Color_t m_tOriginalColor = {};
	float m_flOriginalBlend = 1.f;
	IMaterial* m_pOriginalMaterial = nullptr;
	OverrideType_t m_iOriginalOverride = OVERRIDE_NORMAL;

	int m_iFlags = false;

public:
	void Store(CTFPlayer* pLocal);
	void UpdateTarget();
	void RenderMain();
	void RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	bool RenderViewmodel(void* rcx, int flags, int* iReturn);
	bool RenderViewmodel(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	bool m_bRendering = false;

	// Entities whose engine scene draw the DrawModelExecute hook intercepts.
	// false: suppressed - RenderMain redraws them with cham materials.
	// true: plain-Original passthrough - the engine draw is kept but wrapped in
	// the visible-pass stencil write, so occluded layers still mask against it.
	// Flat epoch-stamped slots, not a hash map: the hook probes this once per
	// engine model draw (200+ per frame) and RenderMain clears it once per
	// render pass.
	Perf::CEntitySlots<bool> m_mEntities = {};
	// Any passthrough entities registered for the upcoming scene: the
	// ViewDrawScene hook clears the stencil at scene start so their marks land
	// on a clean buffer, and RenderMain skips its own pre-clear to keep them.
	bool m_bScenePassthrough = false;
};

ADD_FEATURE(CChams, Chams);
