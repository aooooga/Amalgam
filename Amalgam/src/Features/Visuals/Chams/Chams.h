#pragma once
#include "../../../SDK/SDK.h"

Enum(Model, Visible, Occluded);

class CChams
{
private:
	void Begin();
	void End();

	void DrawModel(CBaseEntity* pEntity, const Chams_t& tChams, IMatRenderContext* pRenderContext, int iModel = ModelEnum::Visible, bool bTwoModel = false);

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

	struct ChamsInfo_t
	{
		CBaseEntity* m_pEntity;
		Chams_t* m_pChams;      // regular chams, or nullptr if the group has none
		int m_iFlags = 0;
		Chams_t* m_pTargetChams = nullptr; // group's targeted material, or nullptr
	};
	std::vector<ChamsInfo_t> m_vEntities = {};

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

	std::unordered_mapset<int> m_mEntities = {};
};

ADD_FEATURE(CChams, Chams);