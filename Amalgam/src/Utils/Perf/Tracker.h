#pragma once
#include <cstdint>
#include <intrin.h>
#include <iosfwd>
#include <vector>

// Process tracker — Amalgam's own always-affordable profiler.
//
// TF2's vprof tells us where the *engine* burns CPU (DrawModel, SetupBones,
// traces). It cannot tell us which part of Amalgam caused that work, and it
// cannot see inside our own code at all: everything we run inside a hook is
// billed to whatever engine node happens to be open. The tracker closes that
// gap. It is designed to be read *next to* an Auto vprof report — the reports
// carry an `#amalgam*` section produced by this file, so one match file answers
// both "what did the engine spend the frame on" and "which of our features
// asked for it".
//
// Three things it records, per zone:
//   - self / inclusive ms per frame, calls per frame, worst single call
//   - counters charged to whichever zone was open when the work was requested
//     (traces, W2S, bone setups, model draws, material overrides, allocations)
//   - the same self-ms ranking restricted to the worst ~5% of frames, plus
//     per-frame snapshots of the very worst frames
//
// Cost. A zone is two `rdtsc`s, one predictable branch on a global bool and
// about six dependent adds into a per-thread slot - ~40 cycles, ~12ns. The
// hottest instrumented site (the per-model-draw hook, 200-600 calls/frame)
// therefore costs ~0.01ms/frame, and the tracker measures and reports its own
// overhead so that number never has to be taken on trust. With the tracker off
// every scope collapses to a load, a test and a jump.
//
// Threading. The material system can run queued rendering on a worker thread,
// so the hot state lives in per-thread banks and the zone stack is thread
// local; banks are summed at the frame boundary. No locks, no atomics, no
// allocation on the hot path.
namespace Perf
{

inline constexpr int MAX_ZONES = 192;   // registration is per source site, deduped by name
inline constexpr int MAX_BANKS = 4;     // main thread + material-system workers

// Where in the frame a zone lives. Grouping keeps the report readable and lets
// a rollup answer "how much of the frame is the input path vs the render path"
// without knowing every zone name.
enum EZoneGroup : uint8_t
{
	GROUP_CREATEMOVE, // input path: aimbot, ticks, packet manipulation, prediction
	GROUP_NETUPDATE,  // FrameStageNotify: entity cache, per-feature Store()
	GROUP_SCENE,      // 3D render passes, per-model-draw work
	GROUP_PAINT,      // 2D overlay pass
	GROUP_ENGINE,     // engine calls Amalgam makes (traces, bone setup)
	GROUP_MISC,
	GROUP_COUNT
};

// Requests for engine work, charged to the innermost open zone. This is the
// bridge to the vprof side of the report: vprof says SetupBones cost 1.8ms,
// the counters say who asked for those bone setups.
enum ECounter : uint8_t
{
	COUNTER_TRACE,       // SDK::Trace / TraceHull
	COUNTER_W2S,         // world-to-screen projections
	COUNTER_BONES,       // SetupBones / bone-cache invalidations forced by us
	COUNTER_MODELDRAW,   // model draws we issued or re-issued (chams/glow passes)
	COUNTER_MATOVERRIDE, // forced material overrides
	COUNTER_SIMTICK,     // simulated movement ticks (prediction, backtrack, ghost)
	COUNTER_PRIMITIVE,   // mesh primitives pushed to the engine renderer
	COUNTER_COUNT
};

const char* GroupName(EZoneGroup eGroup);
const char* CounterName(ECounter eCounter);

// ---- hot-path state -----------------------------------------------------------
// Live accumulation for one zone in one bank, in raw TSC cycles. Reset (memset)
// every frame; converted to ms only at the frame boundary.
struct ZoneLive_t
{
	uint64_t m_uIncl = 0;
	uint64_t m_uSelf = 0;
	uint64_t m_uPeak = 0; // worst single call this frame
	uint32_t m_uCalls = 0;
	uint32_t m_aCounters[COUNTER_COUNT] = {};
};

class CScope;

// One per thread that ever opens a zone. Cache-line aligned: the whole point of
// per-thread banks is that two threads never write the same line.
struct alignas(64) Bank_t
{
	CScope* m_pCur = nullptr;   // innermost open scope on this thread
	uint32_t m_uScopes = 0;     // scopes closed this frame (drives the overhead estimate)
	uint32_t m_uInUse = 0;
	ZoneLive_t m_aZones[MAX_ZONES] = {};
};

// The single branch every scope pays when the tracker is off.
inline bool g_bEnabled = false;
inline thread_local Bank_t* g_pBank = nullptr;

Bank_t* AcquireBank(); // cold: first zone on a thread

// RAII zone. Inclusive time is the full bracket; self time subtracts whatever
// nested zones reported, so a parent's self time is exactly the code it runs
// itself. A zone entered recursively still reports correct self time (its
// inclusive time is then counted per level, which is the usual convention).
class CScope
{
	Bank_t* m_pBank = nullptr;
	CScope* m_pParent = nullptr;
	uint64_t m_uStart = 0;
	uint64_t m_uChild = 0;
	int m_iZone = 0;

public:
	__forceinline CScope(int iZone)
	{
		if (!g_bEnabled || uint32_t(iZone) >= uint32_t(MAX_ZONES))
			return;

		Bank_t* pBank = g_pBank;
		if (!pBank && !(pBank = AcquireBank()))
			return;

		m_pBank = pBank;
		m_iZone = iZone;
		m_pParent = pBank->m_pCur;
		pBank->m_pCur = this;
		m_uStart = __rdtsc();
	}

	__forceinline ~CScope()
	{
		if (!m_pBank)
			return;

		const uint64_t uIncl = __rdtsc() - m_uStart;
		auto& tZone = m_pBank->m_aZones[m_iZone];
		tZone.m_uIncl += uIncl;
		tZone.m_uSelf += uIncl - m_uChild;
		tZone.m_uCalls++;
		if (uIncl > tZone.m_uPeak)
			tZone.m_uPeak = uIncl;

		if (m_pParent)
			m_pParent->m_uChild += uIncl;
		m_pBank->m_pCur = m_pParent;
		m_pBank->m_uScopes++;
	}

	CScope(const CScope&) = delete;
	CScope& operator=(const CScope&) = delete;

	int Zone() const { return m_iZone; }
};

// Registration is a cold, once-per-site call behind a function-local static.
// Names are deduped, so the same name used at several sites merges into one
// zone (which is what you want for e.g. a shared helper).
int RegisterZone(const char* pszName, EZoneGroup eGroup);

// Charge engine work to the innermost open zone. Work requested outside any
// zone lands in the "unattributed" bucket rather than being dropped, so the
// counters always sum to the real total.
__forceinline void Charge(ECounter eCounter, uint32_t uCount = 1)
{
	if (!g_bEnabled)
		return;

	Bank_t* pBank = g_pBank;
	if (!pBank && !(pBank = AcquireBank()))
		return;

	pBank->m_aZones[pBank->m_pCur ? pBank->m_pCur->Zone() : 0].m_aCounters[eCounter] += uCount;
}

// ---- reporting types ----------------------------------------------------------
// Frame-summed aggregate for one zone over some set of frames. Everything is a
// sum; the writer divides by m_iFrames.
struct ZoneAgg_t
{
	double m_flSelfMs = 0.0;
	double m_flInclMs = 0.0;
	double m_flCalls = 0.0;
	double m_flPeakMs = 0.0; // max, not summed
	double m_aCounters[COUNTER_COUNT] = {};
};

struct Aggregate_t
{
	long long m_iFrames = 0;
	double m_flFrameMs = 0.0;  // summed wall frame time
	double m_flTotalMs = 0.0;  // summed Amalgam self time across all zones
	std::vector<ZoneAgg_t> m_vZones;

	void Reset(int iZones);
	void Add(int iZone, double flSelfMs, double flInclMs, double flCalls, double flPeakMs, const uint32_t* pCounters);
};

// A row the live overlay draws.
struct OverlayRow_t
{
	const char* m_pszName = "";
	double m_flSelfMs = 0.0;
	double m_flCalls = 0.0;
	EZoneGroup m_eGroup = GROUP_MISC;
};

class CTracker
{
public:
	// ---- driving ----
	void SetEnabled(bool bEnabled);            // once per frame, before any zone opens
	void EndFrame();                           // frame boundary (CViewRender::RenderView entry)
	void BeginMatch();                         // Auto vprof match start
	void SetLive(bool bLive);                  // only live-round frames feed the match aggregate
	void EndMatch();                           // clears match state after the report is written

	bool HasMatchData() const { return m_tMatch.m_iFrames > 0; }

	// ---- reporting ----
	// Appends the `#amalgam*` sections to an Auto vprof match report.
	void WriteMatchSections(std::ostream& sOut);

	// ---- live view ----
	const std::vector<OverlayRow_t>& OverlayRows() const { return m_vOverlay; }
	double OverlayTotalMs() const { return m_flSmoothTotalMs; }
	double OverlayFrameMs() const { return m_flSmoothFrameMs; }
	double OverheadMs() const { return m_flSmoothOverheadMs; }
	const double* OverlayCounters() const { return m_aSmoothCounters; }

private:
	void Calibrate(uint64_t uTsc, int64_t iQpc);
	void MeasureScopeCost();
	void CommitFrame(double flFrameMs);
	void RefreshHeavyThreshold();

	bool m_bActive = false;
	bool m_bLive = false;

	// ---- cycle -> ms calibration ----
	bool m_bCalibrated = false;
	uint64_t m_uTscBase = 0;
	int64_t m_iQpcBase = 0;
	int64_t m_iQpcFreq = 0;
	int64_t m_iQpcLast = 0;
	double m_flMsPerCycle = 0.0;
	double m_flScopeCycles = 0.0; // measured cost of one open/close pair

	// ---- match aggregates ----
	Aggregate_t m_tMatch;                 // every live frame
	Aggregate_t m_tHeavy;                 // frames at/above the 5%-low threshold
	double m_flHeavyThresholdMs = 0.0;
	double m_flOverheadMsSum = 0.0;
	long long m_iScopesTotal = 0;
	std::vector<float> m_vFrameMs;        // live frame times, for the threshold
	int m_iIntervalFrames = 0;

	// ---- overlay smoothing ----
	std::vector<OverlayRow_t> m_vOverlay;
	std::vector<double> m_vSmoothSelfMs;
	std::vector<double> m_vSmoothCalls;
	double m_aSmoothCounters[COUNTER_COUNT] = {};
	double m_flSmoothTotalMs = 0.0;
	double m_flSmoothFrameMs = 0.0;
	double m_flSmoothOverheadMs = 0.0;
	int m_iOverlayFrame = 0;
};

inline CTracker Tracker;

} // namespace Perf

// ---- instrumentation macros ---------------------------------------------------
// PROF_ZONE("Aimbot::Run", Perf::GROUP_CREATEMOVE);  -- times to the end of scope
// PROF_CHARGE(Perf::COUNTER_TRACE);                  -- bills work to the open zone
#define PROF_CAT2(a, b) a##b
#define PROF_CAT(a, b) PROF_CAT2(a, b)

#define PROF_ZONE(name, group) \
	static const int PROF_CAT(s_iProfZone, __LINE__) = ::Perf::RegisterZone(name, group); \
	::Perf::CScope PROF_CAT(tProfScope, __LINE__)(PROF_CAT(s_iProfZone, __LINE__))

// One-line form for the aggregator hooks, which are just long lists of feature
// calls: PROF_CALL("Aimbot::Run", Perf::GROUP_CREATEMOVE, F::Aimbot.Run(a, b, c));
// (one per line, since the zone's static is keyed on __LINE__).
#define PROF_CALL(name, group, call) do { PROF_ZONE(name, group); call; } while (false)

#define PROF_CHARGE(counter) ::Perf::Charge(counter)
#define PROF_CHARGE_N(counter, n) ::Perf::Charge(counter, uint32_t(n))
