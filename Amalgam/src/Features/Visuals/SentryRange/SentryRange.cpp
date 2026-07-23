#include "SentryRange.h"

#include "../FlexFOV/FlexFOV.h"
#include "../Materials/Materials.h"
#include "../../../SDK/Definitions/Main/IMesh.h"

#include <deque>

static constexpr float SENTRY_RANGE = 1100.f;
static constexpr int MAX_LAYERS = 3;
// The grid samples the full 2*SENTRY_RANGE span, so the cell count is quadratic
// in 1/step - at tiny steps that is millions of cells, hundreds of MB of grid
// state and a minutes-long trace backlog. Cap the working dimension so the grid
// stays bounded no matter how low the slider goes; the effective step is floored
// to 2*SENTRY_RANGE / MAX_DIM (~4.3u), which is already sub-player resolution and
// finer than the polygon simplification cares about.
static constexpr int MAX_DIM = 512;

Vec3 CSentryRange::GetSentryEye(CObjectSentrygun* pSentry)
{
	// SENTRYGUN_EYE_OFFSET_LEVEL_1/2/3; minis are 0.75-scale level 1 models
	static constexpr float flOffsets[] = { 32.f, 40.f, 46.f };
	float flOffset = flOffsets[std::clamp(pSentry->m_iUpgradeLevel(), 1, 3) - 1];
	if (pSentry->m_bMiniBuilding())
		flOffset *= 0.75f;
	return pSentry->m_vecOrigin() + Vec3(0, 0, flOffset);
}

// Samples cell i: finds the walkable floor layers inside the range sphere's
// vertical band at the cell's XY (top-down traces, punching through each floor
// to reach the levels below it), then runs the sentry's actual targeting test
// on every layer - is the eye-height point above the floor within range of the
// sentry eye with a clear line of sight, the same check FindTarget makes
// against origin + view offset? Layers that pass are stored for drawing; a
// cell with none renders as a hole. Returns the number of traces used.
int CSentryRange::TraceCell(SentryCache_t& tCache, int gx, int gy)
{
	auto& tCell = tCache.m_vCells[size_t(gy) * tCache.m_iDim + gx];
	const int nOld = tCell.m_iCount;
	float aOldZ[MAX_LAYERS];
	for (int j = 0; j < nOld; j++)
		aOldZ[j] = tCell.m_aLayers[j].m_flZ;
	tCell.m_iCount = 0;

	const Vec3 vEye = tCache.m_vEye;
	const float flX = tCache.m_flMinX + (gx + 0.5f) * tCache.m_flStep;
	const float flY = tCache.m_flMinY + (gy + 0.5f) * tCache.m_flStep;

	auto bChanged = [&]
	{
		if (nOld != tCell.m_iCount)
			return true;
		for (int j = 0; j < tCell.m_iCount; j++)
		{
			if (fabsf(aOldZ[j] - tCell.m_aLayers[j].m_flZ) > 0.5f)
				return true;
		}
		return false;
	};

	// vertical band of the range sphere at this XY; no floor outside it can
	// hold an in-range player regardless of height
	const float flDX = flX - vEye.x, flDY = flY - vEye.y;
	float flVertSqr = SENTRY_RANGE * SENTRY_RANGE - (flDX * flDX + flDY * flDY);
	if (flVertSqr <= 0.f)
	{
		// outside the range circle entirely: no traces, and the zero return is the
		// caller's signal that this cell cost nothing (so it doesn't burn a slot)
		if (nOld)
			tCache.m_bListDirty = true;
		return 0;
	}
	float flVert = sqrtf(flVertSqr);
	const float flHeight = Vars::Visuals::SentryRange::TargetHeight.Value;
	float flZ = vEye.z + std::min(flVert, 450.f); // capped: floors further above a sentry than this are fringe
	const float flZBottom = vEye.z - flVert - flHeight;

	CTraceFilterWorldAndPropsOnly filter = {};
	int nTraces = 0;
	for (int nDown = 0; nDown < 8 && tCell.m_iCount < MAX_LAYERS && flZ > flZBottom; nDown++)
	{
		CGameTrace trace = {};
		SDK::Trace({ flX, flY, flZ }, { flX, flY, flZBottom }, MASK_SHOT, &filter, &trace); nTraces++;
		if (trace.allsolid)
			break; // the entire rest of the band is solid rock: nothing can be below
		if (trace.startsolid)
		{
			flZ -= 64.f; // inside a solid (roof, thick floor): step down through it
			continue;
		}
		if (trace.fraction >= 1.f)
			break; // fell through the rest of the band: no more floors
		flZ = trace.endpos.z - 18.f; // resume below this surface for lower levels
		if (trace.plane.normal.z < 0.7f)
			continue; // too steep to stand on

		Vec3 vTarget = { trace.endpos.x, trace.endpos.y, trace.endpos.z + flHeight };
		if (vTarget.DistTo(vEye) > SENTRY_RANGE)
			continue;
		CGameTrace tSight = {};
		SDK::Trace(vEye, vTarget, MASK_SHOT, &filter, &tSight); nTraces++;
		if (tSight.startsolid || tSight.fraction < 1.f)
			continue;
		tCell.m_aLayers[tCell.m_iCount++] = { trace.endpos.z, trace.plane.normal };
	}
	if (bChanged())
		tCache.m_bListDirty = true;
	return nTraces;
}

// Fills a fine (non-lattice) cell from its enclosing coarse block instead of
// tracing it, wherever the block is uniform enough that the interior is known
// from its four corners:
//   - all four corners agree on layer count (a boundary/merge would disagree), and
//   - every layer is one planar surface across the block: its four corner normals
//     near-parallel and its corner heights untwisted (coplanar; |z00+z11-z10-z01|
//     ~0, exactly 0 for a plane no matter how steep)
// then each layer is bilinearly interpolated for free. This covers flat ground,
// planar ramps AND stacked planar floors (a bridge over ground). Anything else -
// a lit/unlit boundary, a curved or stepped surface, layers merging - falls
// through to a full trace, so non-uniform regions stay sampled at full
// resolution. This is what turns a fine grid's cost from O(area) into ~O(boundary):
// the large all-lit and all-unlit interiors cost nothing.
// Uniformity is a property of the *block*, not of the cell, so it is decided once
// here and then applied to all K*K interior cells - the old per-cell version
// re-read the four corners and re-ran the plane fit up to K*K times over for the
// same answer, which was the bulk of the refine sweep's cost.
//
// nWork counts only the cells that actually cost something (an interpolation or a
// trace); a uniformly-unlit block that is already clear is free and must not eat
// the caller's per-frame scan allowance, which is what lets the huge dead regions
// (outside the range circle, inside walls) sweep past in a single frame.
int CSentryRange::RefineBlock(SentryCache_t& tCache, int bx, int by, int& nWork)
{
	const int iDim = tCache.m_iDim, K = tCache.m_iK;
	const int bx0 = bx * K, by0 = by * K;
	if (bx0 >= iDim || by0 >= iDim)
		return 0; // the lattice has one more line than there are blocks
	// the block spans corners [bx0, bx0+K] (clamped) and owns cells [bx0, bx0+K-1]
	const int bx1 = std::min(bx0 + K, iDim - 1), by1 = std::min(by0 + K, iDim - 1);
	const int xEnd = std::min(bx0 + K - 1, iDim - 1), yEnd = std::min(by0 + K - 1, iDim - 1);

	const Cell_t& c00 = tCache.m_vCells[size_t(by0) * iDim + bx0];
	const Cell_t& c10 = tCache.m_vCells[size_t(by0) * iDim + bx1];
	const Cell_t& c01 = tCache.m_vCells[size_t(by1) * iDim + bx0];
	const Cell_t& c11 = tCache.m_vCells[size_t(by1) * iDim + bx1];

	auto Parallel = [](const Vec3& a, const Vec3& b) // unit normals: dot > cos(~8 deg)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z > 0.99f;
	};

	// a differing layer count means a boundary or a merge runs through the block;
	// otherwise every layer must be one planar, untwisted surface across it
	constexpr float flTwist = 4.f;
	const int nc = c00.m_iCount;
	bool bUniform = c10.m_iCount == nc && c01.m_iCount == nc && c11.m_iCount == nc;
	float aZ[MAX_LAYERS][4] = {};
	Vec3 aNormal[MAX_LAYERS] = {};
	for (int L = 0; bUniform && L < nc; L++)
	{
		const Layer_t& l00 = c00.m_aLayers[L]; const Layer_t& l10 = c10.m_aLayers[L];
		const Layer_t& l01 = c01.m_aLayers[L]; const Layer_t& l11 = c11.m_aLayers[L];
		if (fabsf(l00.m_flZ + l11.m_flZ - l10.m_flZ - l01.m_flZ) > flTwist
			|| !Parallel(l00.m_vNormal, l10.m_vNormal)
			|| !Parallel(l00.m_vNormal, l01.m_vNormal)
			|| !Parallel(l00.m_vNormal, l11.m_vNormal))
		{
			bUniform = false;
			break;
		}
		aZ[L][0] = l00.m_flZ; aZ[L][1] = l10.m_flZ; aZ[L][2] = l01.m_flZ; aZ[L][3] = l11.m_flZ;
		Vec3 vN = l00.m_vNormal + l10.m_vNormal + l01.m_vNormal + l11.m_vNormal;
		const float flLen = sqrtf(vN.x * vN.x + vN.y * vN.y + vN.z * vN.z);
		aNormal[L] = flLen > 1e-4f ? Vec3(vN.x / flLen, vN.y / flLen, vN.z / flLen) : Vec3(0, 0, 1);
	}

	const float flInvDX = bx1 > bx0 ? 1.f / (bx1 - bx0) : 0.f;
	const float flInvDY = by1 > by0 ? 1.f / (by1 - by0) : 0.f;

	int nTraces = 0;
	for (int gy = by0; gy <= yEnd; gy++)
	{
		const bool bCoarseY = gy == by0 || gy == iDim - 1;
		const float flFY = (gy - by0) * flInvDY;
		Cell_t* pRow = &tCache.m_vCells[size_t(gy) * iDim];
		for (int gx = bx0; gx <= xEnd; gx++)
		{
			if (bCoarseY && (gx == bx0 || gx == iDim - 1))
				continue; // lattice cell: phase 0 already traced it
			if (!bUniform)
			{
				nTraces += TraceCell(tCache, gx, gy);
				nWork++;
				continue;
			}
			auto& tCell = pRow[gx];
			if (!nc)
			{
				if (tCell.m_iCount) // uniformly unlit, and it used to be lit
				{
					tCell.m_iCount = 0;
					tCache.m_bListDirty = true;
					nWork++;
				}
				continue; // already clear: free
			}
			const float flFX = (gx - bx0) * flInvDX;
			bool bChanged = tCell.m_iCount != nc;
			for (int L = 0; L < nc; L++)
			{
				const float flZ = (aZ[L][0] * (1.f - flFX) + aZ[L][1] * flFX) * (1.f - flFY)
					+ (aZ[L][2] * (1.f - flFX) + aZ[L][3] * flFX) * flFY;
				bChanged |= fabsf(flZ - tCell.m_aLayers[L].m_flZ) > 0.5f;
				tCell.m_aLayers[L] = { flZ, aNormal[L] };
			}
			tCell.m_iCount = nc;
			if (bChanged)
				tCache.m_bListDirty = true;
			nWork++;
		}
	}
	return nTraces;
}

// Advances the per-cache build one cell and returns the traces it spent. Phase 0
// fully traces the coarse K-lattice; once that wraps, phase 1 sweeps the whole
// grid, refining every non-lattice cell (lattice cells are already done) from its
// coarse block. bWrapped is set on the call that finishes a full two-phase pass.
// Both phases spin forward over free entries (cells outside the range circle,
// blocks that are uniformly unlit and already clear) inside a single call rather
// than burning one caller slot each, so the dead majority of a big grid costs a
// pointer walk instead of frames.
int CSentryRange::StepCell(SentryCache_t& tCache, bool& bWrapped, int& nWork)
{
	bWrapped = false;
	const int iDim = tCache.m_iDim, K = tCache.m_iK;
	const int nCX = tCache.m_iNCX, nCoarse = nCX * tCache.m_iNCY;
	int nTraces = 0;

	if (tCache.m_iPhase == 0)
	{
		while (tCache.m_iCursor < nCoarse)
		{
			const int cx = tCache.m_iCursor % nCX, cy = tCache.m_iCursor / nCX;
			const int gx = std::min(cx * K, iDim - 1), gy = std::min(cy * K, iDim - 1);
			tCache.m_iCursor++;
			nTraces = TraceCell(tCache, gx, gy);
			if (nTraces) // 0 means the cell is outside the range circle: free, keep going
			{
				nWork++;
				break;
			}
		}
		if (tCache.m_iCursor >= nCoarse)
		{
			tCache.m_iCursor = 0;
			if (K <= 1)
				bWrapped = true; // no interior to refine: the lattice is the whole grid
			else
				tCache.m_iPhase = 1;
		}
		return nTraces;
	}

	// phase 1: refine block by block; lattice cells are skipped inside RefineBlock
	while (tCache.m_iCursor < nCoarse)
	{
		const int bx = tCache.m_iCursor % nCX, by = tCache.m_iCursor / nCX;
		tCache.m_iCursor++;
		const int nBefore = nWork;
		nTraces += RefineBlock(tCache, bx, by, nWork);
		if (nWork != nBefore)
			break;
	}
	if (tCache.m_iCursor >= nCoarse)
	{
		tCache.m_iCursor = 0;
		tCache.m_iPhase = 0;
		bWrapped = true;
	}
	return nTraces;
}

// Remaining cells before this cache finishes its current pass; used only to size
// the per-frame budget, so an overcount (free cells included) just finishes sooner.
size_t CSentryRange::RemainingWork(const SentryCache_t& tCache) const
{
	const size_t iGrid = tCache.m_vCells.size();
	const size_t iBlocks = size_t(tCache.m_iNCX) * tCache.m_iNCY;
	if (tCache.m_iPhase == 0)
		return iBlocks - tCache.m_iCursor + (tCache.m_iK > 1 ? iGrid : 0);
	return (iBlocks - tCache.m_iCursor) * tCache.m_iK * tCache.m_iK;
}

// Bakes the cell grid into CPU geometry.
//
// The lit/safe border is built first: per lit layer, any side whose neighbor
// cell has no layer continuing the surface is a boundary segment (including
// around interior shadow pockets). The raw border is a grid-aligned staircase;
// its segments are welded at grid corners (per surface height), traced into
// connected contours and Douglas-Peucker simplified - in XY *and* Z, so a
// straight edge is only drawn where the ground actually stays near it, never
// floating a chord over an intervening dip or step. Each staircase corner the
// simplifier drops is recorded snapped onto its chord (`vSnap`). If Smoothing is
// on, the whole contour is Laplacian corner-rounded in place (no new verts) so
// both the edges and the fill read the rounded positions.
//
// Fill then reuses that: coplanar interior flats collapse into single rects
// (greedy 2D merge), everything else becomes per-cell quads whose corners are
// welded with their neighbors' - and any corner sitting on a contour is placed
// at its snapped position, so the fill's outer edge follows the same polygon as
// the lines while its interior keeps the heightfield. Two things keep the fill
// seam-free on uneven ground: every non-boundary corner takes one shared,
// averaged surface height (so neighbouring quads meet at the exact same vertex
// instead of each other's slightly-off plane extrapolation), and rects only
// merge fully-coplanar 3x3 neighbourhoods (an unwelded rect against a tilted
// cell was the main seam source). Output is chunked to stay inside 16-bit mesh
// indices. GroundOffset is applied at bake time (rebake on change).
void CSentryRange::BuildDrawList(SentryCache_t& tCache)
{
	tCache.m_vFillChunks.clear();
	tCache.m_vEdges.clear();

	const int iDim = tCache.m_iDim;
	const float flStep = tCache.m_flStep, flHalf = flStep * 0.5f;
	const float flJoin = flStep * 1.25f; // max z gap for neighbor layers to count as the same surface (45 degree slopes join)

	// Every pass below only ever emits from lit cells, so clip the whole bake to
	// their bounding box: the square grid's dead corners (the range circle alone
	// leaves >20% dead, an indoor sentry far more) stop being iterated, and the
	// three per-corner tables - by far the biggest allocations here, and cleared
	// again on every chunk flush - shrink to the box instead of (iDim+1)^2.
	int iX0 = iDim, iY0 = iDim, iX1 = -1, iY1 = -1;
	for (int y = 0; y < iDim; y++)
	{
		const Cell_t* pRow = &tCache.m_vCells[size_t(y) * iDim];
		for (int x = 0; x < iDim; x++)
		{
			if (!pRow[x].m_iCount)
				continue;
			if (x < iX0) iX0 = x;
			if (x > iX1) iX1 = x;
			if (y < iY0) iY0 = y;
			if (y > iY1) iY1 = y;
		}
	}
	if (iX1 < 0)
		return; // nothing lit at all
	// corner lattice spanning the box: cx in [iX0, iX1 + 1], cy in [iY0, iY1 + 1]
	const int iCW = iX1 - iX0 + 2, iCH = iY1 - iY0 + 2;
	auto iCorner = [&](int cx, int cy) { return size_t(cy - iY0) * iCW + (cx - iX0); };

	auto flPlaneZ = [](const Layer_t& tLayer, float flDX, float flDY)
	{
		return tLayer.m_flZ - (tLayer.m_vNormal.x * flDX + tLayer.m_vNormal.y * flDY) / tLayer.m_vNormal.z;
	};
	auto bOpen = [&](int x, int y, float flZ)
	{
		if (x < 0 || x >= iDim || y < 0 || y >= iDim)
			return true;
		auto& tOther = tCache.m_vCells[y * iDim + x];
		for (int j = 0; j < tOther.m_iCount; j++)
		{
			if (fabsf(tOther.m_aLayers[j].m_flZ - flZ) < flJoin)
				return false;
		}
		return true;
	};
	// --- boundary contours (built before the fill so the fill can follow them) ---
	// weld each grid-aligned boundary segment's endpoints to a node at their grid
	// corner, grouped by surface height (a bridge deck and the ground beneath it
	// share an XY corner but stay distinct contours).
	struct BNode_t { float m_flX, m_flY, m_flZSum; int m_nRefs; };
	std::vector<BNode_t> vNodes;
	struct BCorner_t { int m_nCount = 0; struct { float m_flZ; int m_iNode; } m_aGroups[4] = {}; };
	std::vector<BCorner_t> vBCorners(size_t(iCW) * iCH);
	auto iBoundNode = [&](int cx, int cy, float flZ)
	{
		auto& tC = vBCorners[iCorner(cx, cy)];
		for (int g = 0; g < tC.m_nCount; g++)
		{
			if (fabsf(tC.m_aGroups[g].m_flZ - flZ) < flJoin)
			{
				auto& tN = vNodes[tC.m_aGroups[g].m_iNode];
				tN.m_flZSum += flZ; tN.m_nRefs++;
				return tC.m_aGroups[g].m_iNode;
			}
		}
		int iNode = int(vNodes.size());
		vNodes.push_back({ tCache.m_flMinX + cx * flStep, tCache.m_flMinY + cy * flStep, flZ, 1 });
		if (tC.m_nCount < 4)
			tC.m_aGroups[tC.m_nCount++] = { flZ, iNode };
		return iNode;
	};
	std::vector<std::pair<int, int>> vSegs;
	for (int y = iY0; y <= iY1; y++)
	{
		for (int x = iX0; x <= iX1; x++)
		{
			auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
			for (int iLayer = 0; iLayer < tCell.m_iCount; iLayer++)
			{
				auto& tLayer = tCell.m_aLayers[iLayer];
				if (bOpen(x, y - 1, tLayer.m_flZ))
					vSegs.emplace_back(iBoundNode(x, y, flPlaneZ(tLayer, -flHalf, -flHalf)), iBoundNode(x + 1, y, flPlaneZ(tLayer, flHalf, -flHalf)));
				if (bOpen(x, y + 1, tLayer.m_flZ))
					vSegs.emplace_back(iBoundNode(x, y + 1, flPlaneZ(tLayer, -flHalf, flHalf)), iBoundNode(x + 1, y + 1, flPlaneZ(tLayer, flHalf, flHalf)));
				if (bOpen(x - 1, y, tLayer.m_flZ))
					vSegs.emplace_back(iBoundNode(x, y, flPlaneZ(tLayer, -flHalf, -flHalf)), iBoundNode(x, y + 1, flPlaneZ(tLayer, -flHalf, flHalf)));
				if (bOpen(x + 1, y, tLayer.m_flZ))
					vSegs.emplace_back(iBoundNode(x + 1, y, flPlaneZ(tLayer, flHalf, -flHalf)), iBoundNode(x + 1, y + 1, flPlaneZ(tLayer, flHalf, flHalf)));
			}
		}
	}

	// resolve nodes to points (welded corners average their contributing heights);
	// snapped positions start as identity, overwritten for simplified-away corners
	std::vector<Vec3> vPts(vNodes.size());
	for (size_t i = 0; i < vNodes.size(); i++)
		vPts[i] = { vNodes[i].m_flX, vNodes[i].m_flY, vNodes[i].m_flZSum / float(vNodes[i].m_nRefs) };
	std::vector<Vec3> vSnap = vPts;

	// trace segments into polylines via node->incident-segment adjacency
	std::vector<std::vector<int>> vAdj(vNodes.size());
	for (int s = 0; s < int(vSegs.size()); s++)
	{
		vAdj[vSegs[s].first].push_back(s);
		vAdj[vSegs[s].second].push_back(s);
	}
	std::vector<bool> vUsed(vSegs.size(), false);
	auto iOther = [&](int s, int n) { return vSegs[s].first == n ? vSegs[s].second : vSegs[s].first; };
	auto iWalk = [&](int n) // consume+return an unused segment incident to n (its other end), or -1
	{
		for (int s : vAdj[n])
		{
			if (!vUsed[s])
			{
				vUsed[s] = true;
				return iOther(s, n);
			}
		}
		return -1;
	};

	// Douglas-Peucker keep-mask over an open polyline of node ids. A vertex is
	// kept when it strays from the current chord either sideways (> flEps, the
	// staircase jaggedness) OR vertically (> flZEps, terrain the straight chord
	// would otherwise float over); the split goes to whichever violates most.
	const float flEps = flStep; // ~one cell: enough to swallow full stair steps
	const float flZEps = 4.f;   // keep the border within a few units of the ground
	auto Simplify = [&](const std::vector<int>& vIds)
	{
		const int n = int(vIds.size());
		std::vector<char> vKeep(n, 0);
		if (n == 0)
			return vKeep;
		vKeep[0] = vKeep[n - 1] = 1;
		std::vector<std::pair<int, int>> vStack = { { 0, n - 1 } };
		while (!vStack.empty())
		{
			auto [i0, i1] = vStack.back(); vStack.pop_back();
			const Vec3& vA = vPts[vIds[i0]];
			const Vec3& vB = vPts[vIds[i1]];
			const float flDx = vB.x - vA.x, flDy = vB.y - vA.y;
			const float flLen2 = flDx * flDx + flDy * flDy, flLen = sqrtf(flLen2);
			float flWorst = 1.f; int iFar = -1;
			for (int i = i0 + 1; i < i1; i++)
			{
				const Vec3& vP = vPts[vIds[i]];
				const float flPx = vP.x - vA.x, flPy = vP.y - vA.y;
				const float flDistXY = flLen > 1e-4f ? fabsf(flPx * flDy - flPy * flDx) / flLen : sqrtf(flPx * flPx + flPy * flPy);
				const float flT = flLen2 > 1e-4f ? std::clamp((flPx * flDx + flPy * flDy) / flLen2, 0.f, 1.f) : 0.f;
				const float flZDev = fabsf(vP.z - (vA.z + flT * (vB.z - vA.z)));
				const float flViolate = std::max(flDistXY / flEps, flZDev / flZEps);
				if (flViolate > flWorst)
				{
					flWorst = flViolate; iFar = i;
				}
			}
			if (iFar >= 0)
			{
				vKeep[iFar] = 1;
				vStack.push_back({ i0, iFar });
				vStack.push_back({ iFar, i1 });
			}
		}
		return vKeep;
	};
	auto ProjectOntoChord = [](const Vec3& vP, const Vec3& vA, const Vec3& vB)
	{
		const float flDx = vB.x - vA.x, flDy = vB.y - vA.y;
		const float flLen2 = flDx * flDx + flDy * flDy;
		const float flT = flLen2 > 1e-4f ? std::clamp(((vP.x - vA.x) * flDx + (vP.y - vA.y) * flDy) / flLen2, 0.f, 1.f) : 0.f;
		return Vec3(vA.x + flT * flDx, vA.y + flT * flDy, vA.z + flT * (vB.z - vA.z));
	};
	auto SnapChain = [&](const std::vector<int>& vIds) // DP an open chain: snap dropped corners onto their chord; return the keep mask
	{
		auto vKeep = Simplify(vIds);
		int iPrev = -1;
		for (int i = 0; i < int(vIds.size()); i++)
		{
			if (!vKeep[i])
				continue;
			if (iPrev >= 0)
			{
				const Vec3& vA = vPts[vIds[iPrev]];
				const Vec3& vB = vPts[vIds[i]];
				for (int j = iPrev + 1; j < i; j++)
					vSnap[vIds[j]] = ProjectOntoChord(vPts[vIds[j]], vA, vB);
			}
			iPrev = i;
		}
		return vKeep;
	};

	// cheap corner rounding: Laplacian-average each contour corner toward its two
	// neighbours a few times, in place on vSnap (no new vertices). Straight runs
	// are collinear so they don't move - only the polygon's corners round off - and
	// both the edges (emitted from vSnap below) and the fill (reads vSnap) follow.
	const int iSmooth = int(Vars::Visuals::SentryRange::Smoothing.Value / 100.f * 8.f + 0.5f);
	auto SmoothLoop = [&](const std::vector<int>& vOrder, bool bClosed)
	{
		const int n = int(vOrder.size());
		if (n < 3)
			return;
		std::vector<Vec3> vTmp(n);
		for (int it = 0; it < iSmooth; it++)
		{
			for (int i = 0; i < n; i++)
			{
				if (!bClosed && (i == 0 || i == n - 1))
				{
					vTmp[i] = vSnap[vOrder[i]];
					continue;
				}
				const Vec3& vP = vSnap[vOrder[(i - 1 + n) % n]];
				const Vec3& vN = vSnap[vOrder[(i + 1) % n]];
				vTmp[i] = vSnap[vOrder[i]] * 0.5f + vP * 0.25f + vN * 0.25f;
			}
			for (int i = 0; i < n; i++)
				vSnap[vOrder[i]] = vTmp[i];
		}
	};

	for (int s0 = 0; s0 < int(vSegs.size()); s0++)
	{
		if (vUsed[s0])
			continue;
		vUsed[s0] = true;
		std::deque<int> dq = { vSegs[s0].first, vSegs[s0].second };
		for (int nNext = iWalk(dq.back()); nNext >= 0; nNext = iWalk(dq.back()))
			dq.push_back(nNext);
		for (int nNext = iWalk(dq.front()); nNext >= 0; nNext = iWalk(dq.front()))
			dq.push_front(nNext);

		std::vector<int> vOrder(dq.begin(), dq.end());
		const bool bClosed = vOrder.size() > 2 && vOrder.front() == vOrder.back();
		std::vector<char> vKeep;
		if (bClosed)
		{
			// closed loop: no endpoints to pin DP, so split at the vertex farthest
			// from the start (in XY) into two open chains and merge their keep masks
			vOrder.pop_back();
			const int n = int(vOrder.size());
			int iFar = 0; float flBest = -1.f;
			for (int i = 1; i < n; i++)
			{
				const float flDx = vPts[vOrder[i]].x - vPts[vOrder[0]].x, flDy = vPts[vOrder[i]].y - vPts[vOrder[0]].y;
				const float flD = flDx * flDx + flDy * flDy;
				if (flD > flBest) { flBest = flD; iFar = i; }
			}
			std::vector<int> vHalfA(vOrder.begin(), vOrder.begin() + iFar + 1);
			std::vector<int> vHalfB(vOrder.begin() + iFar, vOrder.end());
			vHalfB.push_back(vOrder.front());
			auto vKeepA = SnapChain(vHalfA);
			auto vKeepB = SnapChain(vHalfB);
			vKeep.assign(n, 0);
			for (int i = 0; i <= iFar; i++)
				vKeep[i] = vKeepA[i];
			for (int t = 0; t < int(vHalfB.size()); t++)
				if (vKeepB[t]) vKeep[(iFar + t) % n] = 1;
		}
		else
			vKeep = SnapChain(vOrder);

		if (iSmooth > 0)
			SmoothLoop(vOrder, bClosed);

		const int n = int(vOrder.size());
		if (iSmooth > 0)
		{
			// rounded: emit every corner from the smoothed positions
			const int nEmit = bClosed ? n : n - 1;
			for (int i = 0; i < nEmit; i++)
				tCache.m_vEdges.emplace_back(vSnap[vOrder[i]], vSnap[vOrder[(i + 1) % n]]);
		}
		else
		{
			// crisp: connect the kept polygon vertices only
			int iPrev = -1, iFirst = -1;
			for (int i = 0; i < n; i++)
			{
				if (!vKeep[i])
					continue;
				if (iPrev >= 0)
					tCache.m_vEdges.emplace_back(vSnap[vOrder[iPrev]], vSnap[vOrder[i]]);
				if (iFirst < 0)
					iFirst = i;
				iPrev = i;
			}
			if (bClosed && iFirst >= 0 && iPrev != iFirst)
				tCache.m_vEdges.emplace_back(vSnap[vOrder[iPrev]], vSnap[vOrder[iFirst]]);
		}
	}

	// --- continuous corner heights (kills fill seams on uneven ground) ---
	// Every grid corner gets one shared height per surface by averaging the plane-
	// extrapolated height of each adjacent cell that reaches it, so neighbouring
	// quads meet at the exact same vertex instead of each other's slightly-off
	// extrapolation. Clustered within flJoin so genuinely separate floors stay
	// apart. Interior fill corners read this; boundary corners use vSnap instead.
	struct CHeight_t { int m_nCount = 0; struct { float m_flSum; int m_nCnt; } m_a[6] = {}; };
	std::vector<CHeight_t> vCH(size_t(iCW) * iCH);
	auto AddCornerZ = [&](int cx, int cy, float flZ)
	{
		auto& tCH = vCH[iCorner(cx, cy)];
		for (int g = 0; g < tCH.m_nCount; g++)
		{
			if (fabsf(tCH.m_a[g].m_flSum / tCH.m_a[g].m_nCnt - flZ) < flJoin)
			{
				tCH.m_a[g].m_flSum += flZ; tCH.m_a[g].m_nCnt++;
				return;
			}
		}
		if (tCH.m_nCount < 6)
			tCH.m_a[tCH.m_nCount++] = { flZ, 1 };
	};
	for (int y = iY0; y <= iY1; y++)
	{
		for (int x = iX0; x <= iX1; x++)
		{
			auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
			for (int iLayer = 0; iLayer < tCell.m_iCount; iLayer++)
			{
				auto& tLayer = tCell.m_aLayers[iLayer];
				AddCornerZ(x, y, flPlaneZ(tLayer, -flHalf, -flHalf));
				AddCornerZ(x + 1, y, flPlaneZ(tLayer, flHalf, -flHalf));
				AddCornerZ(x + 1, y + 1, flPlaneZ(tLayer, flHalf, flHalf));
				AddCornerZ(x, y + 1, flPlaneZ(tLayer, -flHalf, flHalf));
			}
		}
	}
	auto flCornerZ = [&](int cx, int cy, float flZ) // shared averaged height of the surface nearest flZ
	{
		auto& tCH = vCH[iCorner(cx, cy)];
		for (int g = 0; g < tCH.m_nCount; g++)
		{
			float flAvg = tCH.m_a[g].m_flSum / tCH.m_a[g].m_nCnt;
			if (fabsf(flAvg - flZ) < flJoin)
				return flAvg;
		}
		return flZ;
	};

	// greedy rects only merge cells whose whole 3x3 neighbourhood is coplanar-flat
	// at the same height: an unwelded rect abutting even a slightly-tilted cell
	// leaves a T-junction crack, and with normal.z > 0.999 being so strict those
	// flat/near-flat borders are exactly where the common seams came from. Confined
	// this way, every flat<->uneven transition is welded per-cell quads instead.
	auto bFlatLit = [&](int x, int y, float flZ0)
	{
		if (x < 0 || x >= iDim || y < 0 || y >= iDim)
			return false;
		auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
		for (int j = 0; j < tCell.m_iCount; j++)
			if (tCell.m_aLayers[j].m_vNormal.z > 0.999f && fabsf(tCell.m_aLayers[j].m_flZ - flZ0) <= 1.f)
				return true;
		return false;
	};
	auto bRectSafe = [&](int x, int y, float flZ0)
	{
		for (int dy = -1; dy <= 1; dy++)
			for (int dx = -1; dx <= 1; dx++)
				if (!bFlatLit(x + dx, y + dy, flZ0))
					return false;
		return true;
	};

	// chunked emitter with corner welding; the weld map resets on chunk flush
	// (boundary corners duplicate across chunks at identical positions)
	struct Corner_t
	{
		int m_nCount = 0;
		struct { float m_flZ; int m_iVert; } m_aGroups[4] = {};
	};
	std::vector<Corner_t> vCorners(size_t(iCW) * iCH);
	auto& vChunks = tCache.m_vFillChunks;
	vChunks.emplace_back();
	auto FlushIfFull = [&]
	{
		auto& tChunk = vChunks.back();
		if (tChunk.m_vVerts.size() > 30000 || tChunk.m_vIndices.size() > 59000)
		{
			vChunks.emplace_back();
			std::fill(vCorners.begin(), vCorners.end(), Corner_t{});
		}
	};
	auto AddQuad = [&](const Vec3& vA, const Vec3& vB, const Vec3& vC, const Vec3& vD)
	{
		FlushIfFull();
		auto& tChunk = vChunks.back();
		auto iBase = (unsigned short)tChunk.m_vVerts.size();
		tChunk.m_vVerts.insert(tChunk.m_vVerts.end(), { vA, vB, vC, vD });
		tChunk.m_vIndices.insert(tChunk.m_vIndices.end(),
			{ iBase, (unsigned short)(iBase + 1), (unsigned short)(iBase + 2),
			  iBase, (unsigned short)(iBase + 2), (unsigned short)(iBase + 3) });
	};
	auto iCornerVert = [&](int cx, int cy, float flZ)
	{
		auto& tCorner = vCorners[iCorner(cx, cy)];
		for (int g = 0; g < tCorner.m_nCount; g++)
		{
			if (fabsf(tCorner.m_aGroups[g].m_flZ - flZ) < flJoin)
				return (unsigned short)tCorner.m_aGroups[g].m_iVert;
		}
		// a corner on a contour rides its snapped (polygon-conforming) position;
		// interior corners take the shared averaged surface height so neighbouring
		// quads meet exactly and never crack into a seam
		Vec3 vPos = { tCache.m_flMinX + cx * flStep, tCache.m_flMinY + cy * flStep, flZ };
		bool bBoundary = false;
		auto& tBC = vBCorners[iCorner(cx, cy)];
		for (int g = 0; g < tBC.m_nCount; g++)
		{
			if (fabsf(tBC.m_aGroups[g].m_flZ - flZ) < flJoin)
			{
				vPos = vSnap[tBC.m_aGroups[g].m_iNode];
				bBoundary = true;
				break;
			}
		}
		if (!bBoundary)
			vPos.z = flCornerZ(cx, cy, flZ);
		auto& tChunk = vChunks.back();
		int iVert = int(tChunk.m_vVerts.size());
		tChunk.m_vVerts.push_back(vPos);
		if (tCorner.m_nCount < 4)
			tCorner.m_aGroups[tCorner.m_nCount++] = { flZ, iVert };
		return (unsigned short)iVert;
	};

	// pass 1: greedy rects over coplanar flat cells; T-junctions against the
	// welded neighbors are invisible because everything involved shares a plane
	std::vector<uint8_t> vMerged(size_t(iDim) * iDim, 0); // bit per layer index
	auto iFlatAt = [&](int x, int y, float flZ0) // -1 or the matching layer index
	{
		auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
		for (int j = 0; j < tCell.m_iCount; j++)
		{
			if (vMerged[size_t(y) * iDim + x] & (1 << j))
				continue;
			if (tCell.m_aLayers[j].m_vNormal.z > 0.999f && fabsf(tCell.m_aLayers[j].m_flZ - flZ0) <= 1.f)
				return bRectSafe(x, y, tCell.m_aLayers[j].m_flZ) ? j : -1; // only merge fully-coplanar cells
		}
		return -1;
	};
	for (int y = iY0; y <= iY1; y++)
	{
		for (int x = iX0; x <= iX1; x++)
		{
			auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
			for (int iLayer = 0; iLayer < tCell.m_iCount; iLayer++)
			{
				if (vMerged[size_t(y) * iDim + x] & (1 << iLayer))
					continue;
				auto& tLayer = tCell.m_aLayers[iLayer];
				if (tLayer.m_vNormal.z <= 0.999f)
					continue;
				const float flZ0 = tLayer.m_flZ;
				if (!bRectSafe(x, y, flZ0))
					continue; // borders a non-coplanar cell (or the boundary): pass 2 welds it per-cell

				int x1 = x;
				while (x1 + 1 < iDim && iFlatAt(x1 + 1, y, flZ0) != -1)
					x1++;
				int y1 = y;
				while (y1 + 1 < iDim)
				{
					bool bRow = true;
					for (int xx = x; xx <= x1 && bRow; xx++)
						bRow = iFlatAt(xx, y1 + 1, flZ0) != -1;
					if (!bRow)
						break;
					y1++;
				}
				if (x1 == x && y1 == y)
					continue; // single cell: the welded pass handles it

				for (int yy = y; yy <= y1; yy++)
				{
					for (int xx = x; xx <= x1; xx++)
					{
						int j = iFlatAt(xx, yy, flZ0);
						if (j != -1)
							vMerged[size_t(yy) * iDim + xx] |= 1 << j;
					}
				}
				const float flX0 = tCache.m_flMinX + x * flStep, flX1 = tCache.m_flMinX + (x1 + 1) * flStep;
				const float flY0 = tCache.m_flMinY + y * flStep, flY1 = tCache.m_flMinY + (y1 + 1) * flStep;
				AddQuad({ flX0, flY0, flZ0 }, { flX1, flY0, flZ0 }, { flX1, flY1, flZ0 }, { flX0, flY1, flZ0 });
			}
		}
	}

	// pass 2: welded per-cell quads for everything the rects didn't swallow;
	// corners on a contour land at their snapped position (see iCornerVert)
	for (int y = iY0; y <= iY1; y++)
	{
		for (int x = iX0; x <= iX1; x++)
		{
			auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
			for (int iLayer = 0; iLayer < tCell.m_iCount; iLayer++)
			{
				auto& tLayer = tCell.m_aLayers[iLayer];
				if (vMerged[size_t(y) * iDim + x] & (1 << iLayer))
					continue;
				FlushIfFull(); // before welding: corner verts must land in the live chunk
				auto& tChunk = vChunks.back();
				unsigned short i00 = iCornerVert(x, y, flPlaneZ(tLayer, -flHalf, -flHalf));
				unsigned short i10 = iCornerVert(x + 1, y, flPlaneZ(tLayer, flHalf, -flHalf));
				unsigned short i11 = iCornerVert(x + 1, y + 1, flPlaneZ(tLayer, flHalf, flHalf));
				unsigned short i01 = iCornerVert(x, y + 1, flPlaneZ(tLayer, -flHalf, flHalf));
				tChunk.m_vIndices.insert(tChunk.m_vIndices.end(), { i00, i10, i11, i00, i11, i01 });
			}
		}
	}
	if (vChunks.back().m_vIndices.empty())
		vChunks.pop_back();
}

void CSentryRange::GetColors(const SentryCache_t& tCache, Color_t& tFill, Color_t& tFillIgnoreZ, Color_t& tEdge, Color_t& tEdgeIgnoreZ)
{
	bool bInside = tCache.m_bEnemy && tCache.m_bPlayerInside;
	tEdge = bInside ? Vars::Colors::SentryRangePlayerInside.Value
		: tCache.m_bEnemy ? Vars::Colors::SentryRangeEnemy.Value
		: tCache.m_bLocal ? Vars::Colors::SentryRangeLocal.Value : Vars::Colors::SentryRangeTeam.Value;
	tEdgeIgnoreZ = bInside ? Vars::Colors::SentryRangePlayerInsideIgnoreZ.Value
		: tCache.m_bEnemy ? Vars::Colors::SentryRangeEnemyIgnoreZ.Value
		: tCache.m_bLocal ? Vars::Colors::SentryRangeLocalIgnoreZ.Value : Vars::Colors::SentryRangeTeamIgnoreZ.Value;
	tFill = bInside ? Vars::Colors::SentryRangeFillPlayerInside.Value
		: tCache.m_bEnemy ? Vars::Colors::SentryRangeFillEnemy.Value
		: tCache.m_bLocal ? Vars::Colors::SentryRangeFillLocal.Value : Vars::Colors::SentryRangeFillTeam.Value;
	tFillIgnoreZ = bInside ? Vars::Colors::SentryRangeFillPlayerInsideIgnoreZ.Value
		: tCache.m_bEnemy ? Vars::Colors::SentryRangeFillEnemyIgnoreZ.Value
		: tCache.m_bLocal ? Vars::Colors::SentryRangeFillLocalIgnoreZ.Value : Vars::Colors::SentryRangeFillTeamIgnoreZ.Value;

	// fill opacity comes from the fill colors' own alphas; disabled sentries fade
	// everything by DisabledAlpha
	float flScale = tCache.m_bDisabled ? Vars::Visuals::SentryRange::DisabledAlpha.Value / 100.f : 1.f;
	tFill.a = byte(tFill.a * flScale);
	tFillIgnoreZ.a = byte(tFillIgnoreZ.a * flScale);
	tEdge.a = byte(tEdge.a * flScale);
	tEdgeIgnoreZ.a = byte(tEdgeIgnoreZ.a * flScale);

	const int iStyle = Vars::Visuals::SentryRange::Style.Value; // Fill, Edges, Fill + Edges
	if (!(iStyle & 1))
		tFill.a = tFillIgnoreZ.a = 0;
	if (!(iStyle & 2))
		tEdge.a = tEdgeIgnoreZ.a = 0;
}

// CPU chunks -> retained static meshes with the current colors baked into the
// vertices. After this, a render pass is Bind + Draw with zero vertex upload.
void CSentryRange::BakeMeshes(SentryCache_t& tCache)
{
	RetireMeshes(tCache);

	if (!m_pFillMaterial)
	{
		auto pKV = new KeyValues("UnlitGeneric");
		pKV->SetInt("$vertexcolor", 1);
		pKV->SetInt("$vertexalpha", 1);
		pKV->SetInt("$translucent", 1);
		pKV->SetInt("$nofog", 1);
		pKV->SetInt("$nocull", 1); // double-sided without doubled geometry
		m_pFillMaterial = F::Materials.Create("SentryRangeFill", pKV);
	}
	if (!m_pFillMaterialIgnoreZ)
	{
		auto pKV = new KeyValues("UnlitGeneric");
		pKV->SetInt("$vertexcolor", 1);
		pKV->SetInt("$vertexalpha", 1);
		pKV->SetInt("$translucent", 1);
		pKV->SetInt("$nofog", 1);
		pKV->SetInt("$nocull", 1);
		pKV->SetInt("$ignorez", 1);
		m_pFillMaterialIgnoreZ = F::Materials.Create("SentryRangeFillIgnoreZ", pKV);
	}
	if (!m_pEdgeMaterial)
	{
		auto pKV = new KeyValues("UnlitGeneric");
		pKV->SetInt("$vertexcolor", 1);
		pKV->SetInt("$vertexalpha", 1);
		pKV->SetInt("$translucent", 1);
		pKV->SetInt("$nofog", 1);
		m_pEdgeMaterial = F::Materials.Create("SentryRangeEdge", pKV);
	}
	if (!m_pEdgeMaterialIgnoreZ)
	{
		auto pKV = new KeyValues("UnlitGeneric");
		pKV->SetInt("$vertexcolor", 1);
		pKV->SetInt("$vertexalpha", 1);
		pKV->SetInt("$translucent", 1);
		pKV->SetInt("$nofog", 1);
		pKV->SetInt("$ignorez", 1);
		m_pEdgeMaterialIgnoreZ = F::Materials.Create("SentryRangeEdgeIgnoreZ", pKV);
	}
	if (!m_pFillMaterial || !m_pFillMaterialIgnoreZ || !m_pEdgeMaterial || !m_pEdgeMaterialIgnoreZ)
		return; // bake stays invalid, retried next frame

	// a freshly created material has no vertex format until precached -
	// GetVertexFormat() would return an empty format and shaderapi divides by
	// the zero vertex size inside CreateStaticMesh/LockMesh
	for (auto pMaterial : { m_pFillMaterial, m_pFillMaterialIgnoreZ, m_pEdgeMaterial, m_pEdgeMaterialIgnoreZ })
	{
		if (!pMaterial->IsPrecached())
			pMaterial->Refresh();
		if (!pMaterial->IsPrecached())
			return; // retry next frame
	}

	Color_t tFill, tFillIgnoreZ, tEdge, tEdgeIgnoreZ;
	GetColors(tCache, tFill, tFillIgnoreZ, tEdge, tEdgeIgnoreZ);
	const float flOffset = Vars::Visuals::SentryRange::GroundOffset.Value;
	const Vec3 vOffset = { 0, 0, flOffset };
	const Vec3 vLift = { 0, 0, flOffset + 0.3f }; // edges above the fill, no z-fighting

	auto WriteVert = [](MeshDesc_t& tDesc, int n, const Vec3& v, Color_t tColor)
	{
		auto pPos = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(tDesc.m_pPosition) + n * tDesc.m_VertexSize_Position);
		pPos[0] = v.x; pPos[1] = v.y; pPos[2] = v.z;
		if (tDesc.m_VertexSize_Color)
		{
			auto pColor = tDesc.m_pColor + n * tDesc.m_VertexSize_Color;
			pColor[0] = tColor.b; pColor[1] = tColor.g; pColor[2] = tColor.r; pColor[3] = tColor.a; // D3D9 BGRA
		}
		if (tDesc.m_VertexSize_TexCoord[0])
		{
			auto pUV = reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(tDesc.m_pTexCoord[0]) + n * tDesc.m_VertexSize_TexCoord[0]);
			pUV[0] = pUV[1] = 0.f;
		}
	};

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	bool bOK = true;

	for (int iPass = 0; iPass < 2 && bOK; iPass++)
	{
		Color_t tColor = iPass ? tFill : tFillIgnoreZ;
		if (!tColor.a)
			continue;
		auto pMaterial = iPass ? m_pFillMaterial : m_pFillMaterialIgnoreZ;
		auto& vMeshes = iPass ? tCache.m_vFillMeshes : tCache.m_vFillMeshesIgnoreZ;
		for (auto& tChunk : tCache.m_vFillChunks)
		{
			if (tChunk.m_vIndices.empty())
				continue;
			IMesh* pMesh = pRenderContext->CreateStaticMesh(pMaterial->GetVertexFormat(), "SentryRangeFill", pMaterial);
			if (!pMesh)
			{
				bOK = false; // OOM etc.: retry next frame
				break;
			}
			pMesh->SetPrimitiveType(MATERIAL_TRIANGLES);
			const int nVerts = int(tChunk.m_vVerts.size()), nIndices = int(tChunk.m_vIndices.size());
			MeshDesc_t tDesc;
			pMesh->LockMesh(nVerts, nIndices, tDesc);
			for (int n = 0; n < nVerts; n++)
				WriteVert(tDesc, n, tChunk.m_vVerts[n] + vOffset, tColor);
			for (int n = 0; n < nIndices; n++)
				tDesc.m_pIndices[n] = (unsigned short)(tDesc.m_nFirstVertex + tChunk.m_vIndices[n]);
			pMesh->UnlockMesh(nVerts, nIndices, tDesc);
			vMeshes.push_back(pMesh);
		}
	}

	if (bOK && !tCache.m_vEdges.empty())
	{
		constexpr int nMaxLines = 15000; // 2 verts/indices per line, inside 16-bit indices
		const int nTotal = int(tCache.m_vEdges.size());
		for (int iPass = 0; iPass < 2 && bOK; iPass++)
		{
			Color_t tColor = iPass ? tEdge : tEdgeIgnoreZ;
			if (!tColor.a)
				continue;
			auto pMaterial = iPass ? m_pEdgeMaterial : m_pEdgeMaterialIgnoreZ;
			auto& vMeshes = iPass ? tCache.m_vEdgeMeshes : tCache.m_vEdgeMeshesIgnoreZ;
			for (int iBase = 0; iBase < nTotal && bOK; iBase += nMaxLines)
			{
				const int nLines = std::min(nMaxLines, nTotal - iBase);
				IMesh* pMesh = pRenderContext->CreateStaticMesh(pMaterial->GetVertexFormat(), "SentryRangeEdge", pMaterial);
				if (!pMesh)
				{
					bOK = false;
					break;
				}
				pMesh->SetPrimitiveType(MATERIAL_LINES);
				MeshDesc_t tDesc;
				pMesh->LockMesh(nLines * 2, nLines * 2, tDesc);
				for (int n = 0; n < nLines; n++)
				{
					auto& tEdgeGeo = tCache.m_vEdges[iBase + n];
					WriteVert(tDesc, n * 2, tEdgeGeo.m_vA + vLift, tColor);
					WriteVert(tDesc, n * 2 + 1, tEdgeGeo.m_vB + vLift, tColor);
				}
				for (int n = 0; n < nLines * 2; n++)
					tDesc.m_pIndices[n] = (unsigned short)(tDesc.m_nFirstVertex + n);
				pMesh->UnlockMesh(nLines * 2, nLines * 2, tDesc);
				vMeshes.push_back(pMesh);
			}
		}
	}

	pRenderContext->Release();
	if (!bOK)
	{
		RetireMeshes(tCache);
		return;
	}
	tCache.m_bBakeValid = true;
	tCache.m_tBakedFill = tFill;
	tCache.m_tBakedFillIgnoreZ = tFillIgnoreZ;
	tCache.m_tBakedEdge = tEdge;
	tCache.m_tBakedEdgeIgnoreZ = tEdgeIgnoreZ;
	tCache.m_flBakedOffset = flOffset;
}

void CSentryRange::RetireMeshes(SentryCache_t& tCache)
{
	const int iFrame = I::GlobalVars->framecount;
	for (auto pMesh : tCache.m_vFillMeshes)
		m_vRetired.emplace_back(pMesh, iFrame);
	for (auto pMesh : tCache.m_vFillMeshesIgnoreZ)
		m_vRetired.emplace_back(pMesh, iFrame);
	for (auto pMesh : tCache.m_vEdgeMeshes)
		m_vRetired.emplace_back(pMesh, iFrame);
	for (auto pMesh : tCache.m_vEdgeMeshesIgnoreZ)
		m_vRetired.emplace_back(pMesh, iFrame);
	tCache.m_vFillMeshes.clear();
	tCache.m_vFillMeshesIgnoreZ.clear();
	tCache.m_vEdgeMeshes.clear();
	tCache.m_vEdgeMeshesIgnoreZ.clear();
	tCache.m_bBakeValid = false;
}

void CSentryRange::DrainRetired(bool bAll)
{
	if (m_vRetired.empty())
		return;

	// the queued material system runs at most a couple of frames behind; 16
	// frames is a comfortable margin before really destroying anything
	constexpr int iSafeAge = 16;
	const int iFrame = I::GlobalVars->framecount;
	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	for (auto it = m_vRetired.begin(); it != m_vRetired.end();)
	{
		if (!bAll && unsigned(iFrame - it->m_iFrame) < iSafeAge)
		{
			++it;
			continue;
		}
		pRenderContext->DestroyStaticMesh(it->m_pMesh);
		it = m_vRetired.erase(it);
	}
	pRenderContext->Release();
}

void CSentryRange::ClearCaches()
{
	for (auto& [pEntity, tCache] : m_mCache)
		RetireMeshes(tCache);
	m_mCache.clear();
}

void CSentryRange::Update(CTFPlayer* pLocal)
{
	// floor the step so the grid can never exceed MAX_DIM cells per side (see the
	// MAX_DIM note); clamping here keeps the dims, extents and the cache-
	// invalidation compare all consistent, and stops a slider drag through the
	// sub-floor range from rebuilding an identical grid each notch
	float flStep = std::max(Vars::Visuals::SentryRange::GridStep.Value, 2.f * SENTRY_RANGE / MAX_DIM);
	float flHeight = Vars::Visuals::SentryRange::TargetHeight.Value;
	if (m_flLastStep != flStep || m_flLastHeight != flHeight)
	{
		ClearCaches();
		m_flLastStep = flStep; m_flLastHeight = flHeight;
	}
	// smoothing only reshapes the baked draw list, not the traced cells - rebuild
	// the geometry in place rather than dropping the whole grid
	float flSmoothing = Vars::Visuals::SentryRange::Smoothing.Value;
	if (m_flLastSmoothing != flSmoothing)
	{
		m_flLastSmoothing = flSmoothing;
		for (auto& [pEntity, tCache] : m_mCache)
			tCache.m_bListDirty = true;
	}

	// rebuild the map, migrating grids whose sentry didn't move or change eye height
	std::unordered_map<CBaseEntity*, SentryCache_t> mNewCache = {};
	int iValue = Vars::Visuals::SentryRange::Draw.Value;
	Vec3 vLocalEye = pLocal->GetEyePosition();
	float flMaxDist = Vars::Visuals::SentryRange::MaxDistance.Value;
	for (auto pEntity : H::Entities.GetGroup(EntityEnum::BuildingAll))
	{
		if (!pEntity->IsSentrygun())
			continue;
		auto pSentry = pEntity->As<CObjectSentrygun>();
		// A picked-up sentry's origin follows the carrier, so re-deriving the grid
		// would drag the carpet around with them (and re-trace it every step).
		// Instead freeze whatever was already traced at the spot it was built and
		// leave it there; the origin compare below rebuilds it once it is re-placed.
		bool bCarried = pSentry->m_bCarried() || pSentry->m_bPlacing();
		if (bCarried && !m_mCache.contains(pEntity))
			continue; // nothing built yet: nothing to keep in place

		bool bEnemy = pSentry->m_iTeamNum() != pLocal->m_iTeamNum();
		bool bLocal = !bEnemy && pSentry->m_hBuilder().Get() == pLocal;
		if (!(iValue & (bEnemy ? Vars::Visuals::SentryRange::DrawEnum::Enemy
			: bLocal ? Vars::Visuals::SentryRange::DrawEnum::Local : Vars::Visuals::SentryRange::DrawEnum::Team)))
			continue;
		if (pSentry->m_bBuilding() && !(iValue & Vars::Visuals::SentryRange::DrawEnum::Building))
			continue;
		bool bDisabled = pSentry->IsDisabled();
		if (bDisabled && !(iValue & Vars::Visuals::SentryRange::DrawEnum::Disabled))
			continue;

		Vec3 vOrigin = pSentry->m_vecOrigin();
		Vec3 vEye = GetSentryEye(pSentry);

		auto& tCache = mNewCache[pEntity];
		auto it = m_mCache.find(pEntity);
		if (it != m_mCache.end() && (bCarried || (it->second.m_vOrigin == vOrigin && fabsf(it->second.m_flEyeZ - vEye.z) < 1.f)))
			tCache = std::move(it->second); // mesh vectors move along; moved-from ends up empty
		else
		{
			tCache.m_vOrigin = vOrigin;
			tCache.m_flEyeZ = vEye.z;
			tCache.m_vEye = vEye;
			tCache.m_flStep = flStep;
			tCache.m_iDim = int(2.f * SENTRY_RANGE / flStep) + 1;
			tCache.m_flMinX = vEye.x - tCache.m_iDim * flStep * 0.5f;
			tCache.m_flMinY = vEye.y - tCache.m_iDim * flStep * 0.5f;
			tCache.m_vCells.assign(size_t(tCache.m_iDim) * tCache.m_iDim, {});
			// adaptive refinement factor: aim the coarse lattice at ~20u (safely
			// finer than a player) so it only engages once the step is fine, and
			// the coarse grid still resolves everything a moderate step would.
			// K == 1 => lattice is the whole grid, i.e. the plain uniform sample.
			constexpr float flCoarseTarget = 20.f;
			tCache.m_iK = std::clamp(int(flCoarseTarget / flStep + 0.5f), 1, 8);
			tCache.m_iNCX = (tCache.m_iDim - 1 + tCache.m_iK - 1) / tCache.m_iK + 1;
			tCache.m_iNCY = tCache.m_iNCX;
			tCache.m_iPhase = 0;
			tCache.m_iCursor = 0;
		}

		tCache.m_bFrozen = bCarried;
		tCache.m_bEnemy = bEnemy;
		tCache.m_bLocal = bLocal;
		tCache.m_bDisabled = bDisabled;
		tCache.m_bDraw = !flMaxDist || vLocalEye.DistTo(tCache.m_vEye) < flMaxDist;

		tCache.m_bPlayerInside = false;
		if (bEnemy && !bDisabled && tCache.m_bDraw && pLocal->IsAlive()
			&& tCache.m_vEye.DistTo(vLocalEye) <= SENTRY_RANGE)
		{
			CGameTrace trace = {};
			CTraceFilterWorldAndPropsOnly filter = {};
			SDK::Trace(tCache.m_vEye, vLocalEye, MASK_SHOT, &filter, &trace);
			tCache.m_bPlayerInside = trace.fraction == 1.f;
		}
	}
	for (auto& [pEntity, tCache] : m_mCache)
		RetireMeshes(tCache); // dropped entries; migrated (moved-from) ones are a no-op
	m_mCache = std::move(mNewCache);

	// spend the trace budget: unfinished grids first, then grids due a
	// refresh; round-robin so multiple sentries progress in parallel
	std::vector<SentryCache_t*> vWork = {};
	for (auto& [pEntity, tCache] : m_mCache)
	{
		if (tCache.m_bDraw && !tCache.m_bComplete && !tCache.m_bFrozen)
			vWork.push_back(&tCache);
	}
	size_t iIncomplete = vWork.size();
	for (auto& [pEntity, tCache] : m_mCache)
	{
		if (tCache.m_bDraw && tCache.m_bComplete && !tCache.m_bFrozen
			&& I::GlobalVars->curtime - tCache.m_flCompleteTime > Vars::Visuals::SentryRange::RefreshInterval.Value)
			vWork.push_back(&tCache);
	}
	if (!vWork.empty())
	{
		int nBudget = Vars::Visuals::SentryRange::TraceBudget.Value;
		// scale the budget up with the initial-build backlog so a grid finishes in
		// a bounded number of frames (~2s) no matter how fine it is, instead of
		// dribbling out at the base rate for minutes; refreshes coast on the base
		// budget. Capped so a huge backlog can't spike a single frame.
		size_t nPending = 0;
		for (size_t i = 0; i < iIncomplete; i++)
			nPending += RemainingWork(*vWork[i]);
		if (nPending)
			nBudget = std::max(nBudget, std::min(6000, int(nPending / 40)));
		// refined interior cells cost no traces, so budget alone would let a mostly-
		// free grid spin its whole area in one frame; cap the cells scanned per frame
		// too so that stays a brief few-frame settle instead of a hitch. Only cells
		// that were actually written count against it (see StepCell).
		int nScan = 40000;
		size_t iEntry = iIncomplete ? m_iSentryCursor % iIncomplete : m_iSentryCursor % vWork.size();
		for (int nSwitch = 0; nBudget > 0 && nScan > 0 && !vWork.empty(); )
		{
			iEntry %= vWork.size();
			auto& tCache = *vWork[iEntry];
			bool bWrapped = false;
			int nWork = 0;
			nBudget -= StepCell(tCache, bWrapped, nWork);
			nScan -= nWork;
			if (bWrapped) // full two-phase pass finished
			{
				tCache.m_bComplete = true;
				tCache.m_flCompleteTime = I::GlobalVars->curtime;
				vWork.erase(vWork.begin() + iEntry);
				nSwitch = 0;
				continue;
			}
			if (++nSwitch >= 8)
			{
				nSwitch = 0;
				iEntry++;
			}
		}
		m_iSentryCursor = iEntry;
	}

	// rebake geometry/meshes that went stale; on a static map with settled
	// colors this whole block is a no-op
	const float flOffset = Vars::Visuals::SentryRange::GroundOffset.Value;
	for (auto& [pEntity, tCache] : m_mCache)
	{
		if (!tCache.m_bComplete || !tCache.m_bDraw)
			continue;
		if (tCache.m_bListDirty)
		{
			BuildDrawList(tCache);
			tCache.m_bListDirty = false;
			RetireMeshes(tCache);
		}
		Color_t tFill, tFillIgnoreZ, tEdge, tEdgeIgnoreZ;
		GetColors(tCache, tFill, tFillIgnoreZ, tEdge, tEdgeIgnoreZ);
		auto bSame = [](Color_t a, Color_t b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; };
		if (!tCache.m_bBakeValid || !bSame(tCache.m_tBakedFill, tFill) || !bSame(tCache.m_tBakedFillIgnoreZ, tFillIgnoreZ)
			|| !bSame(tCache.m_tBakedEdge, tEdge) || !bSame(tCache.m_tBakedEdgeIgnoreZ, tEdgeIgnoreZ)
			|| tCache.m_flBakedOffset != flOffset)
			BakeMeshes(tCache);
	}
	DrainRetired();
}

// this runs once per render pass (up to ~7x/frame with FlexFOV face capture);
// the traces/baking happen once per frame, every pass just binds and draws the
// retained meshes
void CSentryRange::Draw()
{
	int iValue = Vars::Visuals::SentryRange::Draw.Value;
	int iStyle = Vars::Visuals::SentryRange::Style.Value;
	auto pLocal = H::Entities.GetLocal();
	if (!(iValue & Vars::Visuals::SentryRange::DrawEnum::Enabled) || !iStyle || !pLocal)
	{
		ClearCaches();
		DrainRetired();
		return;
	}

	if (m_iLastFrame != I::GlobalVars->framecount)
	{
		m_iLastFrame = I::GlobalVars->framecount;
		Update(pLocal);
	}

	// Scene-stripped (replaced) main pass: the composite paints over anything
	// drawn here, so skip the draws; the cache above still refreshed, and the
	// face passes - which do need the carpet baked in - reuse it.
	if (F::FlexFOV.m_bReplacingView)
		return;

	auto pRenderContext = I::MaterialSystem->GetRenderContext();
	for (auto& [pEntity, tCache] : m_mCache)
	{
		if (!tCache.m_bComplete || !tCache.m_bDraw || !tCache.m_bBakeValid)
			continue;
		if (F::FlexFOV.m_bDrawing)
		{
			// skip faces whose capture cone misses the whole carpet - but when
			// the viewer stands inside the range sphere every face can see
			// some of it, and the cone test breaks down (asin slack caps at
			// 90 degrees), so it must not run at all in that case
			float flDist = tCache.m_vEye.DistTo(F::FlexFOV.m_vEyeOrigin);
			if (flDist > SENTRY_RANGE + 100.f && !F::FlexFOV.FaceCanSee(tCache.m_vEye, SENTRY_RANGE + 100.f))
				continue;
		}

		if (!tCache.m_vFillMeshesIgnoreZ.empty())
		{
			pRenderContext->Bind(m_pFillMaterialIgnoreZ);
			for (auto pMesh : tCache.m_vFillMeshesIgnoreZ)
				pMesh->Draw();
		}
		if (!tCache.m_vFillMeshes.empty())
		{
			pRenderContext->Bind(m_pFillMaterial);
			for (auto pMesh : tCache.m_vFillMeshes)
				pMesh->Draw();
		}
		if (!tCache.m_vEdgeMeshesIgnoreZ.empty())
		{
			pRenderContext->Bind(m_pEdgeMaterialIgnoreZ);
			for (auto pMesh : tCache.m_vEdgeMeshesIgnoreZ)
				pMesh->Draw();
		}
		if (!tCache.m_vEdgeMeshes.empty())
		{
			pRenderContext->Bind(m_pEdgeMaterial);
			for (auto pMesh : tCache.m_vEdgeMeshes)
				pMesh->Draw();
		}
	}
	pRenderContext->Release();
}

void CSentryRange::Unload()
{
	ClearCaches();
	DrainRetired(true);
	// the materials themselves are registered with (and torn down by) F::Materials
	m_pFillMaterial = m_pFillMaterialIgnoreZ = m_pEdgeMaterial = m_pEdgeMaterialIgnoreZ = nullptr;
}
