#include "Tracker.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ostream>
#include <iomanip>
#include <mutex>
#include <algorithm>
#include <cstring>

namespace Perf
{

// ---- report budget ------------------------------------------------------------
// These files are read by an LLM, several at a time, so every row costs context.
// The cutoffs are set where rows stop changing a decision: a zone under 0.005
// ms/frame cannot be worth optimizing, and the tail of near-zero rows was most of
// the file. Anything pruned here is still visible live in the tracker overlay.
static constexpr int    ZONE_TOP = 18;       // zones in #amalgam_zones
static constexpr double ZONE_MIN_MS = 0.005; // ...and the floor to appear at all
static constexpr int    HEAVY_TOP_Z = 10;    // zones in #amalgam_heavy
static constexpr double HEAVY_MIN_MS = 0.01;
static constexpr int    COUNTER_TOP = 8;     // rows in #amalgam_counters
static constexpr double INCL_KEEP_MS = 0.25; // keep a cheap zone wrapping a costly subtree
static constexpr int    INCL_KEEP_MAX = 4;   // ...at most this many extra rows

// ---- registry -----------------------------------------------------------------
// Zone 0 is reserved: engine work charged while no zone is open lands there, so
// the counter columns always sum to the true per-frame total instead of quietly
// losing the calls made outside instrumented code.
struct ZoneDef_t
{
	const char* m_pszName = "";
	uint32_t m_uHash = 0;
	EZoneGroup m_eGroup = GROUP_MISC;
};

static ZoneDef_t s_aZoneDefs[MAX_ZONES];
static int s_iZoneCount = 0;
static Bank_t s_aBanks[MAX_BANKS];
static volatile long s_iBankCount = 0;

static const char* const s_pszGroupNames[GROUP_COUNT] = { "createmove", "netupdate", "scene", "paint", "engine", "misc" };
static const char* const s_pszCounterNames[COUNTER_COUNT] = { "traces", "w2s", "bones", "modeldraws", "matoverrides", "simticks", "primitives" };

const char* GroupName(EZoneGroup eGroup) { return uint8_t(eGroup) < GROUP_COUNT ? s_pszGroupNames[eGroup] : "misc"; }
const char* CounterName(ECounter eCounter) { return uint8_t(eCounter) < COUNTER_COUNT ? s_pszCounterNames[eCounter] : "?"; }

static uint32_t HashName(const char* p)
{
	uint32_t u = 0x811C9DC5u;
	for (; *p; p++)
		u = (u ^ uint8_t(*p)) * 0x01000193u;
	return u;
}

// Cold path: runs once per instrumented site, behind that site's function-local
// static. Deduped by name so a shared helper instrumented in several
// translation units still reports as one zone.
int RegisterZone(const char* pszName, EZoneGroup eGroup)
{
	static std::mutex s_tLock;
	std::lock_guard<std::mutex> tGuard(s_tLock);

	if (!s_iZoneCount)
	{
		s_aZoneDefs[0] = { "(unattributed)", HashName("(unattributed)"), GROUP_MISC };
		s_iZoneCount = 1;
	}

	const uint32_t uHash = HashName(pszName);
	for (int i = 0; i < s_iZoneCount; i++)
	{
		if (s_aZoneDefs[i].m_uHash == uHash && !std::strcmp(s_aZoneDefs[i].m_pszName, pszName))
			return i;
	}

	if (s_iZoneCount >= MAX_ZONES)
		return 0; // registry full: fold into the unattributed bucket rather than corrupt state

	s_aZoneDefs[s_iZoneCount] = { pszName, uHash, eGroup };
	return s_iZoneCount++;
}

Bank_t* AcquireBank()
{
	const long iIndex = InterlockedIncrement(&s_iBankCount) - 1;
	if (iIndex >= MAX_BANKS)
		return nullptr; // more threads than banks: those threads simply go unprofiled

	Bank_t* pBank = &s_aBanks[iIndex];
	pBank->m_uInUse = 1;
	g_pBank = pBank;
	return pBank;
}

// ---- aggregate ----------------------------------------------------------------
void Aggregate_t::Reset(int iZones)
{
	m_iFrames = 0;
	m_flFrameMs = 0.0;
	m_flTotalMs = 0.0;
	m_vZones.assign(iZones > 0 ? iZones : MAX_ZONES, ZoneAgg_t());
}

void Aggregate_t::Add(int iZone, double flSelfMs, double flInclMs, double flCalls, double flPeakMs, const uint32_t* pCounters)
{
	if (iZone < 0 || iZone >= int(m_vZones.size()))
		return;

	auto& tAgg = m_vZones[iZone];
	tAgg.m_flSelfMs += flSelfMs;
	tAgg.m_flInclMs += flInclMs;
	tAgg.m_flCalls += flCalls;
	tAgg.m_flPeakMs = std::max(tAgg.m_flPeakMs, flPeakMs);
	for (int i = 0; i < COUNTER_COUNT; i++)
		tAgg.m_aCounters[i] += pCounters[i];
}

// ---- calibration --------------------------------------------------------------
// rdtsc counts cycles, not time. Rather than trusting a nominal clock we fit the
// conversion against QPC over the whole session: the ratio converges within a
// second and then keeps self-correcting, which also absorbs frequency scaling on
// invariant-TSC parts (every x86 CPU that can run TF2 at a playable rate).
void CTracker::Calibrate(uint64_t uTsc, int64_t iQpc)
{
	if (!m_iQpcFreq)
	{
		LARGE_INTEGER liFreq{};
		QueryPerformanceFrequency(&liFreq);
		m_iQpcFreq = liFreq.QuadPart;
	}

	if (!m_uTscBase)
	{
		m_uTscBase = uTsc;
		m_iQpcBase = iQpc;
		return;
	}

	const int64_t iQpcDelta = iQpc - m_iQpcBase;
	const uint64_t uTscDelta = uTsc - m_uTscBase;
	if (iQpcDelta <= 0 || !uTscDelta || !m_iQpcFreq)
		return;

	// need a quarter second of span before the ratio means anything
	if (double(iQpcDelta) / double(m_iQpcFreq) < 0.25)
		return;

	const double flMs = 1000.0 * double(iQpcDelta) / double(m_iQpcFreq);
	m_flMsPerCycle = flMs / double(uTscDelta);
	m_bCalibrated = true;
}

// Time an empty open/close pair so the report can state the tracker's own cost
// instead of asking anyone to take "it's cheap" on faith.
void CTracker::MeasureScopeCost()
{
	static const int iZone = RegisterZone("Perf::Overhead", GROUP_MISC);

	const bool bWas = g_bEnabled;
	g_bEnabled = true;

	constexpr int ITERS = 4096;
	const uint64_t uStart = __rdtsc();
	for (int i = 0; i < ITERS; i++)
	{
		CScope tScope(iZone);
	}
	const uint64_t uEnd = __rdtsc();

	m_flScopeCycles = double(uEnd - uStart) / ITERS;
	g_bEnabled = bWas;

	// wipe the measurement out of the live banks so it never reaches a report
	for (int i = 0; i < MAX_BANKS; i++)
	{
		std::memset(s_aBanks[i].m_aZones, 0, sizeof(s_aBanks[i].m_aZones));
		s_aBanks[i].m_uScopes = 0;
		s_aBanks[i].m_pCur = nullptr;
	}
}

// ---- driving ------------------------------------------------------------------
void CTracker::SetEnabled(bool bEnabled)
{
	if (bEnabled == m_bActive)
	{
		g_bEnabled = bEnabled;
		return;
	}

	m_bActive = bEnabled;
	if (bEnabled)
	{
		if (m_flScopeCycles <= 0.0)
			MeasureScopeCost();

		// stale partial frames from a previous enable must not leak into the first
		// committed frame
		for (int i = 0; i < MAX_BANKS; i++)
		{
			std::memset(s_aBanks[i].m_aZones, 0, sizeof(s_aBanks[i].m_aZones));
			s_aBanks[i].m_uScopes = 0;
			s_aBanks[i].m_pCur = nullptr;
		}
		m_iQpcLast = 0;
	}

	g_bEnabled = bEnabled;
}

void CTracker::BeginMatch()
{
	m_tMatch.Reset(MAX_ZONES);
	m_tHeavy.Reset(MAX_ZONES);
	m_vFrameMs.clear();
	m_vFrameMs.reserve(8192);
	m_flHeavyThresholdMs = 0.0;
	m_flOverheadMsSum = 0.0;
	m_iScopesTotal = 0;
	m_iIntervalFrames = 0;
	m_bLive = false;
}

void CTracker::SetLive(bool bLive)
{
	m_bLive = bLive;
}

void CTracker::EndMatch()
{
	m_bLive = false;
	m_tMatch.Reset(MAX_ZONES);
	m_tHeavy.Reset(MAX_ZONES);
	m_vFrameMs.clear();
}

void CTracker::EndFrame()
{
	if (!m_bActive)
		return;

	LARGE_INTEGER liNow{};
	QueryPerformanceCounter(&liNow);
	const uint64_t uTsc = __rdtsc();
	Calibrate(uTsc, liNow.QuadPart);

	double flFrameMs = 0.0;
	if (m_iQpcLast && m_iQpcFreq)
		flFrameMs = 1000.0 * double(liNow.QuadPart - m_iQpcLast) / double(m_iQpcFreq);
	m_iQpcLast = liNow.QuadPart;

	// ignore load hitches / alt-tab, matching how Auto vprof filters frame times
	if (flFrameMs < 0.0 || flFrameMs > 1000.0)
		flFrameMs = 0.0;

	CommitFrame(flFrameMs);
}

void CTracker::CommitFrame(double flFrameMs)
{
	const int iZones = s_iZoneCount;
	if (iZones <= 0)
		return;

	// ---- sum the per-thread banks and clear them for the next frame ----
	static std::vector<ZoneLive_t> s_vFrame; // reused; commit must not allocate per frame
	s_vFrame.assign(iZones, ZoneLive_t());

	uint32_t uScopes = 0;
	for (int iBank = 0; iBank < MAX_BANKS; iBank++)
	{
		Bank_t& tBank = s_aBanks[iBank];
		if (!tBank.m_uInUse)
			continue;

		for (int i = 0; i < iZones; i++)
		{
			const ZoneLive_t& tSrc = tBank.m_aZones[i];
			if (!tSrc.m_uCalls && !tSrc.m_uIncl)
			{
				bool bAnyCounter = false;
				for (int c = 0; c < COUNTER_COUNT && !bAnyCounter; c++)
					bAnyCounter = tSrc.m_aCounters[c] != 0;
				if (!bAnyCounter)
					continue;
			}

			ZoneLive_t& tDst = s_vFrame[i];
			tDst.m_uIncl += tSrc.m_uIncl;
			tDst.m_uSelf += tSrc.m_uSelf;
			tDst.m_uCalls += tSrc.m_uCalls;
			tDst.m_uPeak = std::max(tDst.m_uPeak, tSrc.m_uPeak);
			for (int c = 0; c < COUNTER_COUNT; c++)
				tDst.m_aCounters[c] += tSrc.m_aCounters[c];
		}

		uScopes += tBank.m_uScopes;
		std::memset(tBank.m_aZones, 0, sizeof(ZoneLive_t) * iZones);
		tBank.m_uScopes = 0;
		// A scope left open across the boundary (there should be none) would
		// otherwise dangle into the next frame's stack.
		tBank.m_pCur = nullptr;
	}

	if (!m_bCalibrated || m_flMsPerCycle <= 0.0)
		return; // no usable conversion yet; the frame's counts are dropped, not misreported

	// ---- convert and fold into the aggregates ----
	static std::vector<double> s_vSelfMs;
	s_vSelfMs.assign(iZones, 0.0);

	double flTotalMs = 0.0;
	for (int i = 0; i < iZones; i++)
	{
		const ZoneLive_t& tSrc = s_vFrame[i];
		const double flSelfMs = double(tSrc.m_uSelf) * m_flMsPerCycle;
		s_vSelfMs[i] = flSelfMs;
		flTotalMs += flSelfMs;
	}

	const double flOverheadMs = double(uScopes) * m_flScopeCycles * m_flMsPerCycle;

	// ---- live overlay smoothing (runs whether or not a match is being recorded) ----
	{
		if (int(m_vSmoothSelfMs.size()) < iZones)
		{
			m_vSmoothSelfMs.resize(iZones, 0.0);
			m_vSmoothCalls.resize(iZones, 0.0);
		}
		constexpr double A = 0.1;
		for (int i = 0; i < iZones; i++)
		{
			m_vSmoothSelfMs[i] += (s_vSelfMs[i] - m_vSmoothSelfMs[i]) * A;
			m_vSmoothCalls[i] += (double(s_vFrame[i].m_uCalls) - m_vSmoothCalls[i]) * A;
		}
		for (int c = 0; c < COUNTER_COUNT; c++)
		{
			double flSum = 0.0;
			for (int i = 0; i < iZones; i++)
				flSum += s_vFrame[i].m_aCounters[c];
			m_aSmoothCounters[c] += (flSum - m_aSmoothCounters[c]) * A;
		}
		m_flSmoothTotalMs += (flTotalMs - m_flSmoothTotalMs) * A;
		m_flSmoothOverheadMs += (flOverheadMs - m_flSmoothOverheadMs) * A;
		if (flFrameMs > 0.0)
			m_flSmoothFrameMs += (flFrameMs - m_flSmoothFrameMs) * A;

		// rebuild the sorted overlay rows a few times a second, not every frame
		if (++m_iOverlayFrame >= 15)
		{
			m_iOverlayFrame = 0;
			m_vOverlay.clear();
			for (int i = 0; i < iZones; i++)
			{
				if (m_vSmoothSelfMs[i] < 0.001)
					continue;
				m_vOverlay.push_back({ s_aZoneDefs[i].m_pszName, m_vSmoothSelfMs[i], m_vSmoothCalls[i], s_aZoneDefs[i].m_eGroup });
			}
			std::sort(m_vOverlay.begin(), m_vOverlay.end(),
				[](const OverlayRow_t& a, const OverlayRow_t& b) { return a.m_flSelfMs > b.m_flSelfMs; });
		}
	}

	if (!m_bLive || flFrameMs <= 0.0)
		return;

	// ---- match aggregate ----
	m_tMatch.m_iFrames++;
	m_tMatch.m_flFrameMs += flFrameMs;
	m_tMatch.m_flTotalMs += flTotalMs;
	m_flOverheadMsSum += flOverheadMs;
	m_iScopesTotal += uScopes;
	m_vFrameMs.push_back(float(flFrameMs));

	for (int i = 0; i < iZones; i++)
	{
		const ZoneLive_t& tSrc = s_vFrame[i];
		m_tMatch.Add(i, s_vSelfMs[i], double(tSrc.m_uIncl) * m_flMsPerCycle, double(tSrc.m_uCalls),
			double(tSrc.m_uPeak) * m_flMsPerCycle, tSrc.m_aCounters);
	}

	// ---- the 5%-low half: same ranking, restricted to the frames that hurt ----
	if (++m_iIntervalFrames >= 120)
	{
		m_iIntervalFrames = 0;
		RefreshHeavyThreshold();
	}

	if (m_flHeavyThresholdMs > 0.0 && flFrameMs >= m_flHeavyThresholdMs)
	{
		m_tHeavy.m_iFrames++;
		m_tHeavy.m_flFrameMs += flFrameMs;
		m_tHeavy.m_flTotalMs += flTotalMs;
		for (int i = 0; i < iZones; i++)
		{
			const ZoneLive_t& tSrc = s_vFrame[i];
			m_tHeavy.Add(i, s_vSelfMs[i], double(tSrc.m_uIncl) * m_flMsPerCycle, double(tSrc.m_uCalls),
				double(tSrc.m_uPeak) * m_flMsPerCycle, tSrc.m_aCounters);
		}
	}

}

// The threshold that defines "a heavy frame" is the match's own 95th-percentile
// frame time, recomputed from every live frame so far. Deriving it from the whole
// match (rather than per interval) makes the tracker's heavy set the same set of
// moments the Auto vprof report calls the 5% lows.
void CTracker::RefreshHeavyThreshold()
{
	if (m_vFrameMs.size() < 100)
		return;

	static std::vector<float> s_vSorted;
	s_vSorted = m_vFrameMs;
	const size_t iIndex = size_t(s_vSorted.size() * 0.95);
	std::nth_element(s_vSorted.begin(), s_vSorted.begin() + iIndex, s_vSorted.end());
	m_flHeavyThresholdMs = s_vSorted[iIndex];
}

// ---- report -------------------------------------------------------------------
namespace
{
struct ReportRow_t
{
	int m_iZone = 0;
	double m_flSelfMs = 0.0;
	double m_flInclMs = 0.0;
	double m_flCalls = 0.0;
	double m_flPeakMs = 0.0;
};

std::vector<ReportRow_t> RankZones(const Aggregate_t& tAgg, double flMinSelfMs)
{
	std::vector<ReportRow_t> vRows;
	if (tAgg.m_iFrames <= 0)
		return vRows;

	const double flFrames = double(tAgg.m_iFrames);
	for (int i = 0; i < int(tAgg.m_vZones.size()); i++)
	{
		const auto& tZone = tAgg.m_vZones[i];
		if (tZone.m_flCalls <= 0.0 && tZone.m_flSelfMs <= 0.0)
			continue;

		ReportRow_t tRow;
		tRow.m_iZone = i;
		tRow.m_flSelfMs = tZone.m_flSelfMs / flFrames;
		tRow.m_flInclMs = tZone.m_flInclMs / flFrames;
		tRow.m_flCalls = tZone.m_flCalls / flFrames;
		tRow.m_flPeakMs = tZone.m_flPeakMs;
		if (tRow.m_flSelfMs < flMinSelfMs)
			continue;
		vRows.push_back(tRow);
	}

	std::sort(vRows.begin(), vRows.end(), [](const ReportRow_t& a, const ReportRow_t& b) { return a.m_flSelfMs > b.m_flSelfMs; });
	return vRows;
}
}

void CTracker::WriteMatchSections(std::ostream& sOut)
{
	if (m_tMatch.m_iFrames <= 0)
		return;

	const double flFrames = double(m_tMatch.m_iFrames);
	const double flAvgFrameMs = m_tMatch.m_flFrameMs / flFrames;
	const double flAvgTotalMs = m_tMatch.m_flTotalMs / flFrames;
	const double flShare = flAvgFrameMs > 0.0 ? 100.0 * flAvgTotalMs / flAvgFrameMs : 0.0;
	const double flOverheadMs = m_flOverheadMsSum / flFrames;

	sOut << std::fixed;
	sOut << std::setprecision(3);
	sOut << "#amalgam frames=" << m_tMatch.m_iFrames
	     << " selfms=" << flAvgTotalMs
	     << " frame=" << flAvgFrameMs
	     << " share=" << std::setprecision(1) << flShare << "%"
	     << std::setprecision(3) << " overhead=" << flOverheadMs
	     << " scopes=" << (m_iScopesTotal / m_tMatch.m_iFrames) << "\n";
	// ---- per-group summary: input path vs render path vs overlay ----
	{
		double aGroup[GROUP_COUNT] = {};
		for (int i = 0; i < int(m_tMatch.m_vZones.size()); i++)
			aGroup[s_aZoneDefs[i].m_eGroup] += m_tMatch.m_vZones[i].m_flSelfMs / flFrames;
		sOut << "#amalgam_groups";
		for (int g = 0; g < GROUP_COUNT; g++)
			sOut << " " << s_pszGroupNames[g] << "=" << aGroup[g];
		sOut << "\n";
	}

	// ---- zones ----
	auto vRows = RankZones(m_tMatch, ZONE_MIN_MS);
	if (int(vRows.size()) > ZONE_TOP)
	{
		// A zone can matter for what it *contains* rather than what it spends: the
		// per-model-draw hook costs 0.02ms of its own but wraps 0.6ms of engine
		// draw, and dropping it would hide whether our overhead per draw is a
		// problem. Keep the heaviest subtrees that the selfms cut would lose.
		std::vector<ReportRow_t> vKeep(vRows.begin() + ZONE_TOP, vRows.end());
		vRows.resize(ZONE_TOP);
		std::sort(vKeep.begin(), vKeep.end(),
			[](const ReportRow_t& a, const ReportRow_t& b) { return a.m_flInclMs > b.m_flInclMs; });
		for (const auto& tRow : vKeep)
		{
			if (tRow.m_flInclMs < INCL_KEEP_MS || int(vRows.size()) >= ZONE_TOP + INCL_KEEP_MAX)
				break;
			vRows.push_back(tRow);
		}
	}
	sOut << "#amalgam_zones selfms inclms calls/f peakms group name\n";
	for (const auto& tRow : vRows)
	{
		sOut << tRow.m_flSelfMs << " " << tRow.m_flInclMs << " "
		     << std::setprecision(1) << tRow.m_flCalls << " "
		     << std::setprecision(2) << tRow.m_flPeakMs << " "
		     << std::setprecision(3)
		     << s_pszGroupNames[s_aZoneDefs[tRow.m_iZone].m_eGroup] << " "
		     << s_aZoneDefs[tRow.m_iZone].m_pszName << "\n";
	}

	// ---- the same ranking over the frames that hurt ----
	if (m_tHeavy.m_iFrames > 0)
	{
		const double flHeavyFrames = double(m_tHeavy.m_iFrames);
		sOut << std::setprecision(2);
		sOut << "#amalgam_heavy frames=" << m_tHeavy.m_iFrames
		     << " thresholdms=" << m_flHeavyThresholdMs
		     << " avgms=" << m_tHeavy.m_flFrameMs / flHeavyFrames
		     << " selfms=" << m_tHeavy.m_flTotalMs / flHeavyFrames << "\n";
		// calls/f is already in #amalgam_zones; only the selfms shift matters here
		sOut << std::setprecision(3);
		auto vHeavy = RankZones(m_tHeavy, HEAVY_MIN_MS);
		if (int(vHeavy.size()) > HEAVY_TOP_Z)
			vHeavy.resize(HEAVY_TOP_Z);
		for (const auto& tRow : vHeavy)
			sOut << tRow.m_flSelfMs << " " << s_aZoneDefs[tRow.m_iZone].m_pszName << "\n";
	}

	// ---- who asked the engine for the expensive work ----
	{
		// Sparse by nature - most zones request one kind of work, if any - so this
		// is emitted as key=value pairs rather than a mostly-zero column grid.
		sOut << std::setprecision(1);
		sOut << "#amalgam_counters per frame, charged to the caller\n";

		struct CRow { int iZone; double flTotal; };
		std::vector<CRow> vCRows;
		for (int i = 0; i < int(m_tMatch.m_vZones.size()); i++)
		{
			double flTotal = 0.0;
			for (int c = 0; c < COUNTER_COUNT; c++)
				flTotal += m_tMatch.m_vZones[i].m_aCounters[c];
			if (flTotal / flFrames >= 0.5) // sub-1-per-frame requests decide nothing
				vCRows.push_back({ i, flTotal });
		}
		std::sort(vCRows.begin(), vCRows.end(), [](const CRow& a, const CRow& b) { return a.flTotal > b.flTotal; });
		if (int(vCRows.size()) > COUNTER_TOP)
			vCRows.resize(COUNTER_TOP);
		for (const auto& tRow : vCRows)
		{
			sOut << s_aZoneDefs[tRow.iZone].m_pszName;
			for (int c = 0; c < COUNTER_COUNT; c++)
			{
				const double flPerFrame = m_tMatch.m_vZones[tRow.iZone].m_aCounters[c] / flFrames;
				if (flPerFrame >= 0.05)
					sOut << " " << s_pszCounterNames[c] << "=" << flPerFrame;
			}
			sOut << "\n";
		}
	}

}

} // namespace Perf
