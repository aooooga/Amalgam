#pragma once
#include "../../../SDK/SDK.h"

Enum(Model, Visible, Occluded);

class CChams
{
private:
	void Begin();
	void End();

	void DrawModel(CBaseEntity* pEntity, const Chams_t& tChams, IMatRenderContext* pRenderContext, int iModel = ModelEnum::Visible, bool bTwoModel = false);

	void RenderBacktrack(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);
	void RenderFakeAngle(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);

	// Entindex of the entity the local player's crosshair is currently on (eye
	// trace through the aim direction), or 0. Resolved per frame in RenderMain()
	// so it tracks interpolated (rendered) hitbox positions, not net-tick ones.
	int GetCrosshairTarget(CTFPlayer* pLocal);
	int m_iTargetedEntity = 0;

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
	void RenderMain();
	void RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	bool RenderViewmodel(void* rcx, int flags, int* iReturn);
	bool RenderViewmodel(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	bool m_bRendering = false;

	std::unordered_mapset<int> m_mEntities = {};
};

ADD_FEATURE(CChams, Chams);