#pragma once
#include "../../../SDK/SDK.h"

#include <boost/functional/hash.hpp>

class CGlow
{
private:
	void Begin();
	void End();
	void FirstBegin(IMatRenderContext* pRenderContext);
	void FirstEnd(IMatRenderContext* pRenderContext);
	void SecondBegin(IMatRenderContext* pRenderContext, int w, int h);
	void SecondEnd(Glow_t tGlow, IMatRenderContext* pRenderContext, int w, int h);

	void DrawModel(CBaseEntity* pEntity);

	void RenderBacktrack(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);
	void RenderFakeAngle(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);

	IMaterial* m_pMatGlowColor;
	ITexture* m_pRenderBuffer1;
	ITexture* m_pRenderBuffer2;
	IMaterial* m_pMatHaloAddToScreen;
	IMaterial* m_pMatBlurX;
	IMaterial* m_pMatBlurY;
	IMaterialVar* m_pBloomAmount;

	// Face-sized twins of the glow pipeline for the FlexFOV composite: the
	// fidelity-scaled cube faces are larger than the screen, so the screen-sized
	// RenderBuffers can't hold a face's silhouette pass. Created/destroyed by
	// CFlexFOV alongside the face RTs (InitFlexBuffers/UnloadFlexBuffers).
	ITexture* m_pFlexBuffer1 = nullptr;
	ITexture* m_pFlexBuffer2 = nullptr;
	IMaterial* m_pFlexHalo = nullptr;
	IMaterial* m_pFlexBlurX = nullptr;
	IMaterial* m_pFlexBlurY = nullptr;
	IMaterialVar* m_pFlexBloomAmount = nullptr;
	int m_iFlexSize = 0;



	struct GlowHasher_t
	{
		std::size_t operator()(const Glow_t& k) const
		{
			std::size_t seed = 0;

			boost::hash_combine(seed, boost::hash_value(k.Stencil));
			boost::hash_combine(seed, boost::hash_value(k.Blur));

			return seed;
		}
	};
	struct GlowInfo_t
	{
		CBaseEntity* m_pEntity;
		Color_t m_cColor;
		int m_iFlags = 0;
	};
	std::unordered_map<Glow_t, std::vector<GlowInfo_t>, GlowHasher_t> m_mEntities = {};

	Color_t m_tOriginalColor = {};
	float m_flOriginalBlend = 1.f;
	IMaterial* m_pOriginalMaterial = nullptr;
	OverrideType_t m_iOriginalOverride = OVERRIDE_NORMAL;

	int m_iFlags = false;

public:

	void Store(CTFPlayer* pLocal);
	void RenderFirst();
	void RenderSecond();

	// FlexFOV support: renders the glow outlines into the currently-bound cube
	// face RT during a capture pass (face camera matrices active), so outlines
	// are baked into the faces and warped by the composite like the scene.
	void InitFlexBuffers(int iSize);
	void UnloadFlexBuffers();
	void RenderOnFlexFace();
	void RenderHandler(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	void RenderViewmodel(void* rcx, int flags);
	void RenderViewmodel(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	void Initialize();
	void Unload();

	bool m_bRendering = false;
};

ADD_FEATURE(CGlow, Glow);