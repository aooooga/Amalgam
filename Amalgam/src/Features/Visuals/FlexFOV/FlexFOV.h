#pragma once
#include "../../../SDK/SDK.h"

// Wide-angle FOV via globe reprojection (blinky / flex-fov technique).
//
// Phase 1: capture the scene as a view-aligned cube of 6 faces rendered from
// the player's eye, then blit those faces to screen as debug thumbnails to
// verify the globe captures correctly. No reprojection yet - that is Phase 3.

class CFlexFOV
{
public:
	// View-aligned cube faces (oriented relative to the player's view each frame,
	// so FRONT is centered on the crosshair). Order matches the texture arrays.
	enum EFace
	{
		FACE_FRONT = 0,	// player's view direction
		FACE_BACK,		// directly behind
		FACE_LEFT,		// 90 deg to the left
		FACE_RIGHT,		// 90 deg to the right
		FACE_UP,		// 90 deg up
		FACE_DOWN,		// 90 deg down
		FACE_COUNT
	};

private:
	ITexture* m_pFaceTextures[FACE_COUNT] = {};
	IMaterial* m_pFaceMaterials[FACE_COUNT] = {};

	int m_iFaceSize = 0; // square face resolution (px), set at Initialize from screen height

	void RenderFace(void* rcx, const CViewSetup& pViewSetup, EFace eFace, const Vec3& vAngles);
	void ComputeFaceAngles(const Vec3& vViewAngles, Vec3 vOut[FACE_COUNT]);

public:
	// Captures all 6 faces into their render targets. Called from the
	// CViewRender_RenderView hook (after the original main-view render).
	void CaptureGlobe(void* rcx, const CViewSetup& pViewSetup);

	// Blits the 6 captured faces as debug thumbnails over the current screen.
	// Called from IEngineVGui_Paint.
	void DrawDebug();

	// Mesh-ladder composite (currently M1: fullscreen FRONT-face quad).
	// Called from IEngineVGui_Paint.
	void DrawComposite();

	void Initialize();
	void Unload();

	// True while we are re-rendering the scene into a face RT, so other hooks
	// (ESP, chams, glow, post fx) can skip themselves during the capture pass.
	bool m_bDrawing = false;

	// Whether the wide-FOV pipeline is active this frame (needs the globe
	// captured). Set by CVisuals::FOV from the debug/composite toggles for now.
	bool m_bActive = false;

	// Draw the mesh composite (M-ladder) instead of / on top of the tiles.
	bool m_bComposite = false;
};

ADD_FEATURE(CFlexFOV, FlexFOV);
