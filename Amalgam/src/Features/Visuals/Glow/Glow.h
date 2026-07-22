#pragma once
#include "../../../SDK/SDK.h"

#include <boost/functional/hash.hpp>

class CGlow
{
private:
	void Begin();
	void End();
	// The interior stamp's stencil state (mark every covered pixel, through
	// walls); shared by FirstBegin and the inline-stamp silhouette pass.
	void StampStencilBegin(IMatRenderContext* pRenderContext);
	void StampStencilEnd(IMatRenderContext* pRenderContext);

	void FirstBegin(IMatRenderContext* pRenderContext);
	void FirstEnd(IMatRenderContext* pRenderContext);
	void SecondBegin(IMatRenderContext* pRenderContext);
	void SecondEnd(Glow_t tGlow, IMatRenderContext* pRenderContext, int w, int h);

	void DrawModel(CBaseEntity* pEntity);

	void RenderBacktrack(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);
	void RenderFakeAngle(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo);
	void RenderTrajectory(const DrawModelState_t& pState, const ModelRenderInfo_t& pInfo, matrix3x4* pBoneToWorld);

	// (Re)creates the screen glow buffers + their materials at the current
	// GlowResolution scale; early-outs when nothing changed. At exactly screen
	// size the silhouette buffer shares the engine's depth-stencil (m_bBufShared,
	// which enables the inline halo-mask stamping); scaled buffers get their own
	// cleared depth, making the silhouettes through-walls by construction like
	// the FlexFOV face path.
	void EnsureScreenBuffers();
	void FreeScreenBuffers();
	// The single-batch fast path: the silhouette pass writes the halo's stencil
	// mask through the shared depth-stencil, so RenderFirst's per-entity stamp
	// draws are skipped entirely. Needs the shared (unscaled) buffer, and one
	// glow batch so no halo blits before every interior is stamped.
	bool UseInlineStamp() const { return m_bBufShared && m_mEntities.size() == 1 && m_mWorld.empty(); }

	// Draws one registered world glow's geometry into the currently bound
	// silhouette buffer, in the batch's glow colour. See AddWorldGlow.
	void DrawWorldGlow(int iType, const Color_t& tColor);
	// Blur + stencil-tested halo blits closing out one FlexFOV face batch.
	void FlexFaceEnd(const Glow_t& tGlow, IMatRenderContext* pRenderContext, int bw, int bh, int fw, int fh);

	IMaterial* m_pMatGlowColor = nullptr;
	ITexture* m_pRenderBuffer1 = nullptr;
	ITexture* m_pRenderBuffer2 = nullptr;
	IMaterial* m_pMatHaloAddToScreen = nullptr;
	IMaterial* m_pMatBlurX = nullptr;
	IMaterial* m_pMatBlurY = nullptr;
	IMaterialVar* m_pBloomAmount = nullptr;
	int m_iBufW = 0, m_iBufH = 0; // silhouette buffer content dims (screen * scale)
	bool m_bBufShared = true;     // buffer depth-stencil is the engine's (scale == 1)

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
	int m_iFlexW = 0, m_iFlexH = 0;         // created (scaled) buffer dims
	int m_iFlexBaseW = 0, m_iFlexBaseH = 0; // dims the caller asked for, pre-GlowResolution



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
	// World (non-entity) glows, batched by Glow_t exactly like the entities so
	// each shape gets its own silhouette + blur + halo pass.
	std::unordered_map<Glow_t, std::vector<int>, GlowHasher_t> m_mWorld = {};

	Color_t m_tOriginalColor = {};
	float m_flOriginalBlend = 1.f;
	IMaterial* m_pOriginalMaterial = nullptr;
	OverrideType_t m_iOriginalOverride = OVERRIDE_NORMAL;

	int m_iFlags = false;

public:
	// Geometry that isn't an entity but wants the real glow. The pipeline is
	// built entirely around model silhouettes, so a world draw registers its
	// Glow_t and a type here and CGlow calls the owning feature back
	// (DrawWorldGlow) while the silhouette buffer is bound - the geometry then
	// goes through the same blur and halo blits every model glow does.
	//
	// These get no interior stamp: on a line the silhouette IS the thing meant
	// to light up, and stamping it would punch the halo back out of it.
	// Registered per frame (cleared by Store) by whoever draws the geometry.
	enum EWorldGlow
	{
		WORLDGLOW_HEALRADIUS_CONNECT,
		WORLDGLOW_HEALRADIUS_DISCONNECT
	};
	void AddWorldGlow(const Glow_t& tGlow, int iType) { m_mWorld[tGlow].push_back(iType); }

	void Store(CTFPlayer* pLocal);
	void RenderFirst();
	void RenderSecond();

	// FlexFOV support: renders the glow outlines into the currently-bound cube
	// face RT during a capture pass (face camera matrices active), so outlines
	// are baked into the faces and warped by the composite like the scene.
	void InitFlexBuffers(int iW, int iH);
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