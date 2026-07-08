#pragma once
#include "../../../SDK/SDK.h"

// Wide-angle FOV via globe reprojection (blinky / flex-fov technique).
//
// Phase 1: capture the scene as a world-axis-aligned cube of 6 faces rendered
// from the player's eye, then blit those faces to screen as debug thumbnails to
// verify the globe captures correctly. No reprojection yet - that is Phase 3.

class CFlexFOV
{
public:
	// World-axis-aligned cube faces. Order matches m_pFaceTextures / m_pFaceMaterials.
	enum EFace
	{
		FACE_FRONT = 0,	// look +X (yaw 0)
		FACE_BACK,		// look -X (yaw 180)
		FACE_LEFT,		// look +Y (yaw 90)
		FACE_RIGHT,		// look -Y (yaw -90)
		FACE_UP,		// look +Z (pitch -90)
		FACE_DOWN,		// look -Z (pitch 90)
		FACE_COUNT
	};

private:
	ITexture* m_pFaceTextures[FACE_COUNT] = {};
	IMaterial* m_pFaceMaterials[FACE_COUNT] = {};

	int m_iFaceSize = 0; // square face resolution (px), set at Initialize from screen height

	void RenderFace(void* rcx, const CViewSetup& pViewSetup, EFace eFace);

public:
	// Captures all 6 faces into their render targets. Called from the
	// CViewRender_RenderView hook (after the original main-view render).
	void CaptureGlobe(void* rcx, const CViewSetup& pViewSetup);

	// Blits the 6 captured faces as debug thumbnails over the current screen.
	// Called from IEngineVGui_Paint.
	void DrawDebug();

	void Initialize();
	void Unload();

	// True while we are re-rendering the scene into a face RT, so other hooks
	// (ESP, chams, glow, post fx) can skip themselves during the capture pass.
	bool m_bDrawing = false;

	// Whether the wide-FOV pipeline is active this frame (FOV above passthrough
	// threshold). Set by CVisuals::FOV. Phase 1 only reads a debug toggle.
	bool m_bActive = false;
};

ADD_FEATURE(CFlexFOV, FlexFOV);
