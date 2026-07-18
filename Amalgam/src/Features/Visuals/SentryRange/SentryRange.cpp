#include "SentryRange.h"

#include "../FlexFOV/FlexFOV.h"
#include "../Materials/Materials.h"
#include "../../../SDK/Definitions/Main/IMesh.h"

static constexpr float SENTRY_RANGE = 1100.f;
static constexpr int MAX_LAYERS = 3;

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
int CSentryRange::ComputeCell(SentryCache_t& tCache, int i)
{
	auto& tCell = tCache.m_vCells[i];
	const Cell_t tOld = tCell;
	tCell.m_iCount = 0;

	const Vec3 vEye = tCache.m_vEye;
	const float flX = tCache.m_flMinX + (i % tCache.m_iDim + 0.5f) * tCache.m_flStep;
	const float flY = tCache.m_flMinY + (i / tCache.m_iDim + 0.5f) * tCache.m_flStep;

	auto bChanged = [&]
	{
		if (tOld.m_iCount != tCell.m_iCount)
			return true;
		for (int j = 0; j < tCell.m_iCount; j++)
		{
			if (fabsf(tOld.m_aLayers[j].m_flZ - tCell.m_aLayers[j].m_flZ) > 0.5f)
				return true;
		}
		return false;
	};

	// vertical band of the range sphere at this XY; no floor outside it can
	// hold an in-range player regardless of height
	const float flDX = flX - vEye.x, flDY = flY - vEye.y;
	float flVertSqr = SENTRY_RANGE * SENTRY_RANGE - (flDX * flDX + flDY * flDY);
	if (flVertSqr <= 0.f)
		return 0; // outside the range circle entirely, and was never lit: no dirtying
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

// Bakes the cell grid into CPU geometry. Fill: coplanar flat regions collapse
// into single rects (greedy 2D merge), everything else becomes per-cell quads
// whose corners are welded with their neighbors' - a shared vertex per corner
// keeps displacement terrain watertight instead of a jumble of disjoint tile
// planes. Output is chunked to stay inside 16-bit mesh indices. Boundary
// edges: per lit layer, any side whose neighbor cell has no layer continuing
// the surface is the lit/safe boundary - including around interior shadow
// pockets. GroundOffset is applied at bake time (rebake on change).
void CSentryRange::BuildDrawList(SentryCache_t& tCache)
{
	tCache.m_vFillChunks.clear();
	tCache.m_vEdges.clear();

	const int iDim = tCache.m_iDim;
	const float flStep = tCache.m_flStep, flHalf = flStep * 0.5f;
	const float flJoin = flStep * 1.25f; // max z gap for neighbor layers to count as the same surface (45 degree slopes join)

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

	// chunked emitter with corner welding; the weld map resets on chunk flush
	// (boundary corners duplicate across chunks at identical positions)
	struct Corner_t
	{
		int m_nCount = 0;
		struct { float m_flZ; int m_iVert; } m_aGroups[4] = {};
	};
	std::vector<Corner_t> vCorners(size_t(iDim + 1) * (iDim + 1));
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
		auto& tCorner = vCorners[size_t(cy) * (iDim + 1) + cx];
		for (int g = 0; g < tCorner.m_nCount; g++)
		{
			if (fabsf(tCorner.m_aGroups[g].m_flZ - flZ) < flJoin)
				return (unsigned short)tCorner.m_aGroups[g].m_iVert;
		}
		auto& tChunk = vChunks.back();
		int iVert = int(tChunk.m_vVerts.size());
		tChunk.m_vVerts.emplace_back(tCache.m_flMinX + cx * flStep, tCache.m_flMinY + cy * flStep, flZ);
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
				return j;
		}
		return -1;
	};
	for (int y = 0; y < iDim; y++)
	{
		for (int x = 0; x < iDim; x++)
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

	// pass 2: welded per-cell quads for everything the rects didn't swallow,
	// plus boundary edges for every lit layer
	for (int y = 0; y < iDim; y++)
	{
		for (int x = 0; x < iDim; x++)
		{
			auto& tCell = tCache.m_vCells[size_t(y) * iDim + x];
			for (int iLayer = 0; iLayer < tCell.m_iCount; iLayer++)
			{
				auto& tLayer = tCell.m_aLayers[iLayer];
				const float flX = tCache.m_flMinX + (x + 0.5f) * flStep;
				const float flY = tCache.m_flMinY + (y + 0.5f) * flStep;

				if (bOpen(x, y - 1, tLayer.m_flZ))
					tCache.m_vEdges.emplace_back(Vec3(flX - flHalf, flY - flHalf, flPlaneZ(tLayer, -flHalf, -flHalf)), Vec3(flX + flHalf, flY - flHalf, flPlaneZ(tLayer, flHalf, -flHalf)));
				if (bOpen(x, y + 1, tLayer.m_flZ))
					tCache.m_vEdges.emplace_back(Vec3(flX - flHalf, flY + flHalf, flPlaneZ(tLayer, -flHalf, flHalf)), Vec3(flX + flHalf, flY + flHalf, flPlaneZ(tLayer, flHalf, flHalf)));
				if (bOpen(x - 1, y, tLayer.m_flZ))
					tCache.m_vEdges.emplace_back(Vec3(flX - flHalf, flY - flHalf, flPlaneZ(tLayer, -flHalf, -flHalf)), Vec3(flX - flHalf, flY + flHalf, flPlaneZ(tLayer, -flHalf, flHalf)));
				if (bOpen(x + 1, y, tLayer.m_flZ))
					tCache.m_vEdges.emplace_back(Vec3(flX + flHalf, flY - flHalf, flPlaneZ(tLayer, flHalf, -flHalf)), Vec3(flX + flHalf, flY + flHalf, flPlaneZ(tLayer, flHalf, flHalf)));

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

	// FillAlpha is a master fill opacity on top of the fill colors' own alphas
	float flScale = tCache.m_bDisabled ? Vars::Visuals::SentryRange::DisabledAlpha.Value / 100.f : 1.f;
	float flFill = Vars::Visuals::SentryRange::FillAlpha.Value / 100.f * flScale;
	tFill.a = byte(tFill.a * flFill);
	tFillIgnoreZ.a = byte(tFillIgnoreZ.a * flFill);
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
	float flStep = Vars::Visuals::SentryRange::GridStep.Value;
	float flHeight = Vars::Visuals::SentryRange::TargetHeight.Value;
	if (m_flLastStep != flStep || m_flLastHeight != flHeight)
	{
		ClearCaches();
		m_flLastStep = flStep; m_flLastHeight = flHeight;
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
		if (pSentry->m_bCarried() || pSentry->m_bPlacing())
			continue;

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
		if (it != m_mCache.end() && it->second.m_vOrigin == vOrigin && fabsf(it->second.m_flEyeZ - vEye.z) < 1.f)
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
		}

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
		if (tCache.m_bDraw && !tCache.m_bComplete)
			vWork.push_back(&tCache);
	}
	size_t iIncomplete = vWork.size();
	for (auto& [pEntity, tCache] : m_mCache)
	{
		if (tCache.m_bDraw && tCache.m_bComplete
			&& I::GlobalVars->curtime - tCache.m_flCompleteTime > Vars::Visuals::SentryRange::RefreshInterval.Value)
			vWork.push_back(&tCache);
	}
	if (!vWork.empty())
	{
		int nBudget = Vars::Visuals::SentryRange::TraceBudget.Value;
		size_t iEntry = iIncomplete ? m_iSentryCursor % iIncomplete : m_iSentryCursor % vWork.size();
		for (int nSwitch = 0; nBudget > 0 && !vWork.empty(); )
		{
			iEntry %= vWork.size();
			auto& tCache = *vWork[iEntry];
			const int iCells = int(tCache.m_vCells.size());
			nBudget -= ComputeCell(tCache, tCache.m_iCursor);
			tCache.m_iCursor = (tCache.m_iCursor + 1) % iCells;
			tCache.m_iBuilt = std::min(tCache.m_iBuilt + 1, iCells);
			if (!tCache.m_iCursor && tCache.m_iBuilt == iCells) // full pass wrapped
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
