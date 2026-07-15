#pragma once
#include "../../../SDK/SDK.h"

// Rear-view enemy overlay (ported from RearView.lua; the WARP_STRIPS cylindrical
// remap path is intentionally left out).
//
// Fills the slice of the full 360 the player cannot see (360 - front FOV) with a
// set of yawed "flank" cameras that render ONLY enemy players (with a clear line
// of sight) into render targets, then tiles those mirrored across the screen as a
// translucent overlay. Works at any FOV and cooperates with FlexFOV: when the
// composite owns the screen the front coverage is FlexFOV's wide FOV, and the
// FOV-offset slider trims the seam so on-screen enemies stop doubling as ghosts.
//
// Enemies are drawn with the player's chosen material list (same list as chams,
// with per-material colors). An optional stencil outline glow (its own stencil
// width / blur / color, mirroring the Glow feature's pipeline) is baked into each
// flank RT so it composites with the tiles.
class CRearView
{
private:
	std::vector<ITexture*> m_vTextures = {};      // one flank RT per camera
	std::vector<IMaterial*> m_vMaterials = {};    // screen-tile material per RT
	std::vector<IMaterialVar*> m_vAlphaVars = {}; // $alpha var of each tile material

	// Glow pipeline (mirrors CGlow), sized to a flank tile and shared across
	// cameras. Baked into the flank RT so the outline composites with the tile.
	IMaterial* m_pGlowColor = nullptr; // dev/glow_color (solid color for stencil/silhouette)
	ITexture* m_pGlowSil = nullptr;    // colored silhouette buffer
	ITexture* m_pGlowBlur = nullptr;   // blur ping buffer
	IMaterial* m_pGlowBlurX = nullptr;
	IMaterial* m_pGlowBlurY = nullptr;
	IMaterialVar* m_pGlowBloom = nullptr;
	IMaterial* m_pGlowHalo = nullptr;  // translucent (writes alpha, so it composites)

	std::vector<CTFPlayer*> m_vVisible = {}; // enemies with LOS, gathered once per frame

	int m_iLastW = 0, m_iLastH = 0, m_iLastCams = 0;

	bool SetupTargets(int iScrW, int iScrH, int iCams);
	void FreeTargets();
	void GatherVisibleEnemies(CTFPlayer* pLocal);
	void RenderSideCamera(const CViewSetup& tView, float flYawOffset, float flSideFOV, ITexture* pTexture, int iCamW, int iScrH);
	void DrawEnemies();
	void RenderGlow(int iCamW, int iScrH); // outline glow into the currently-bound flank RT

public:
	// True while the flank cameras re-render the scene, so other hooks (e.g.
	// FlexFOV's scene timing) can tell these sub-renders from the main pass.
	bool m_bCapturing = false;

	// Re-renders the flank cameras into their RTs. Called from the
	// CViewRender_RenderView hook after the main-view render (guards against the
	// FlexFOV / camera-window sub-renders that re-enter that same hook).
	void Capture(void* rcx, const CViewSetup& tView);

	// Tiles the captured flank RTs across the screen (mirrored). Called from
	// IEngineVGui_Paint after the FlexFOV composite so it stays visible.
	void DrawOverlay();

	void Initialize();
	void Unload();

	// True only while a flank model is being drawn, so the ForcedMaterialOverride
	// hook keeps our material bound (mirrors CChams::m_bRendering).
	bool m_bRendering = false;
};

ADD_FEATURE(CRearView, RearView);
