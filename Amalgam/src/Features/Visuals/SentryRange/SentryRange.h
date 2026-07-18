#pragma once
#include "../../../SDK/SDK.h"

// Answers "where would a standing player be inside this sentry's sight line?"
// by sampling the game's own targeting test over a ground grid instead of
// hunting for a boundary curve: every cell's walkable floor layers (a bridge
// deck and the ground beneath it each count) get an eye-height point range-
// and line-of-sight-checked against the sentry eye, and the cells that pass
// are painted as a ground-conforming lit carpet with bright edges wherever lit
// meets safe. Shadow pockets behind cover, safe islands, and stacked floors
// render as holes and layers - regions a single ring per azimuth can never
// express, which is why the previous boundary-based attempts all failed.
//
// Rendering is fully retained: the grid bakes into a handful of static meshes
// per sentry (corner-welded heightfield quads, with coplanar flat runs
// collapsed into single rects), so a render pass costs one material bind and
// a few IMesh::Draw calls regardless of grid resolution. Per-primitive
// submission through the engine's RenderTriangle/RenderLine is banned here:
// each call locks its own dynamic mesh and the pool crashes the game at high
// call volume, with FlexFOV's face captures multiplying every pass.
// Grids are cached per sentry (static once built) and traced incrementally
// under a global per-frame trace budget, with a periodic in-place refresh so
// doors/dynamic props fold in. Customizable via Vars::Visuals::SentryRange +
// Vars::Colors.
class CSentryRange
{
	struct Layer_t
	{
		float m_flZ = 0.f;
		Vec3 m_vNormal = { 0, 0, 1 };
	};
	struct Cell_t
	{
		int m_iCount = 0; // lit floor layers at this XY, topmost first
		Layer_t m_aLayers[3] = {};
	};
	struct Chunk_t // one bakeable mesh worth of fill geometry (16-bit indices)
	{
		std::vector<Vec3> m_vVerts = {};
		std::vector<unsigned short> m_vIndices = {};
	};
	struct Edge_t
	{
		Vec3 m_vA = {}, m_vB = {};
	};
	struct SentryCache_t
	{
		// invalidation stamps
		Vec3 m_vOrigin = {};
		float m_flEyeZ = 0.f; // catches upgrades/construction raising the eye

		// derived once per (re)build
		Vec3 m_vEye = {};
		float m_flStep = 0.f;
		int m_iDim = 0;
		float m_flMinX = 0.f, m_flMinY = 0.f;

		// grid, updated in place cell by cell; only lit layers are stored, so
		// an empty cell is a safe spot (or a void)
		std::vector<Cell_t> m_vCells = {};
		// adaptive coarse-to-fine sampling: a K-strided lattice is fully traced
		// (phase 0), then interior fine cells are filled from their enclosing
		// coarse block for free where it is uniform, and only boundary/uneven
		// blocks are traced at full resolution (phase 1). K == 1 disables it and
		// traces every cell, matching the plain uniform grid.
		int m_iK = 1;
		int m_iNCX = 0, m_iNCY = 0; // coarse lattice dimensions
		int m_iPhase = 0;           // 0 = coarse lattice, 1 = refine interior
		int m_iCursor = 0;          // next work index within the current phase
		bool m_bComplete = false;   // first full two-phase pass done -> drawable
		float m_flCompleteTime = 0.f;

		// baked CPU geometry, rebuilt only when cells change
		std::vector<Chunk_t> m_vFillChunks = {};
		std::vector<Edge_t> m_vEdges = {};
		bool m_bListDirty = false;

		// retained GPU meshes, rebaked when geometry/colors/offset change;
		// colors are baked into the vertices because material modulation is
		// not safe under the queued material system
		std::vector<IMesh*> m_vFillMeshes = {};        // z-tested pass
		std::vector<IMesh*> m_vFillMeshesIgnoreZ = {}; // through-wall ghost pass
		std::vector<IMesh*> m_vEdgeMeshes = {};
		std::vector<IMesh*> m_vEdgeMeshesIgnoreZ = {};
		bool m_bBakeValid = false;
		Color_t m_tBakedFill = {}, m_tBakedFillIgnoreZ = {}, m_tBakedEdge = {}, m_tBakedEdgeIgnoreZ = {};
		float m_flBakedOffset = -1.f;

		// cheap per-frame state
		bool m_bEnemy = false;
		bool m_bLocal = false;
		bool m_bDisabled = false;
		bool m_bPlayerInside = false;
		bool m_bDraw = false; // beyond MaxDistance: cache kept, no budget spent
	};
	struct RetiredMesh_t // deferred destroy: queued draws may still reference the mesh
	{
		IMesh* m_pMesh = nullptr;
		int m_iFrame = 0;
	};

	std::unordered_map<CBaseEntity*, SentryCache_t> m_mCache = {};
	size_t m_iSentryCursor = 0;
	int m_iLastFrame = -1;
	float m_flLastStep = 0.f, m_flLastHeight = 0.f, m_flLastSmoothing = 0.f;

	std::vector<RetiredMesh_t> m_vRetired = {};
	IMaterial* m_pFillMaterial = nullptr;
	IMaterial* m_pFillMaterialIgnoreZ = nullptr;
	IMaterial* m_pEdgeMaterial = nullptr;
	IMaterial* m_pEdgeMaterialIgnoreZ = nullptr;

	Vec3 GetSentryEye(CObjectSentrygun* pSentry);
	int TraceCell(SentryCache_t& tCache, int i);         // full trace of one cell; returns traces used
	int RefineCell(SentryCache_t& tCache, int gx, int gy); // fill from coarse block, tracing only if uneven
	int StepCell(SentryCache_t& tCache, bool& bWrapped); // advance the phase machine one cell; returns traces used
	size_t RemainingWork(const SentryCache_t& tCache) const;
	void BuildDrawList(SentryCache_t& tCache); // cell grid -> merged fill chunks + boundary edges
	void GetColors(const SentryCache_t& tCache, Color_t& tFill, Color_t& tFillIgnoreZ, Color_t& tEdge, Color_t& tEdgeIgnoreZ);
	void BakeMeshes(SentryCache_t& tCache); // CPU chunks -> retained static meshes
	void RetireMeshes(SentryCache_t& tCache);
	void DrainRetired(bool bAll = false);
	void ClearCaches();
	void Update(CTFPlayer* pLocal); // once per frame: cache maintenance + budget spend + baking

public:
	void Draw(); // every render pass, from CClientModeShared_DoPostScreenSpaceEffects
	void Unload(); // from CMaterials::UnloadMaterials: meshes + material pointers die with the material system
};

ADD_FEATURE(CSentryRange, SentryRange);
