#pragma once
#include "../../../SDK/SDK.h"

#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

// Auto vprof — automatically drives the engine's vprof profiler across live rounds,
// captures the report + fps lows, and writes token-minimal per-match files plus a
// rolling per-map rollup for LLM-assisted performance analysis.
//
// Driven per rendered frame from IEngineVGui_Paint (Run) and by match events (Event).
// Capture is asynchronous: console commands queue into Cbuf and the vprof report lands
// in con_logfile a few frames later, so Run() implements a small state machine that
// issues a dump, polls the log for our BEGIN/END markers, parses, then continues.

struct VprofNode_t
{
	std::string m_sName = "";
	double m_flCallsFrame = 0.0; // calls/frame
	double m_flSelfPct = 0.0;    // % of frame, self (excluding children)
	double m_flInclPct = 0.0;    // % of frame, including children
	double m_flInclMs = 0.0;     // ms/frame, including children (report "Avg/Frame")

	// self ms/frame, derived from the inclusive ms weighted by self/inclusive share
	double SelfMs() const { return m_flInclPct > 0.0 ? m_flInclMs * m_flSelfPct / m_flInclPct : 0.0; }
};

// Per-node accumulator across all intervals of a match (frame-count weighted).
struct VprofAgg_t
{
	double m_flSumCalls = 0.0;
	double m_flSumSelfPct = 0.0;
	double m_flSumInclPct = 0.0;
	double m_flSumInclMs = 0.0;
	long long m_iFrames = 0; // total frames the node was observed over
};

// A retained capture interval (~1s of accumulation). Kept for the whole match so the
// heaviest intervals (the "5% low" moments) can be re-aggregated separately at match end.
struct VprofInterval_t
{
	long long m_iFrames = 0;              // frames in the interval
	double m_flAvgMs = 0.0;               // average frame time across the interval
	std::vector<VprofNode_t> m_vNodes;    // parsed nodes, trimmed to the heaviest by self ms
};

class CAutoVprof
{
public:
	void Run();                  // per rendered frame (IEngineVGui_Paint, in-game panels)
	void Event(uint32_t uHash);  // round/match lifecycle events

	// Finalize paths. A match's data lives only in memory until one of these
	// runs, so every way out of a match needs to reach one:
	//   LevelShutdown  - the engine's own teardown (disconnect, map change, quit)
	//   FlushOrphaned  - menu paint; catches a match left active with no teardown
	//   Shutdown       - DLL unload (F11, host shutdown/restart)
	// All are idempotent and safe to call when no match is active.
	void LevelShutdown();
	void FlushOrphaned();
	void Shutdown();

	// A "build" is identified by the DLL's link timestamp (YYYYMMDD-HHMM), so every relink
	// is a distinct, chronologically-sortable id. Exposed read-only for the menu display.
	const std::string& GetBuildID();

private:
	enum class EPhase { Idle, Live, WaitDump };
	enum class EAfter { Continue, Pause, EndMatch };

	// ---- lifecycle ----
	void EnsurePaths();
	void UnlockVprof();   // strip cheat flags off vprof commands, pick enable/disable cmds
	void EnableVprof();   // enable once per match
	void DisableVprof();  // disable once per match
	void StartMatch();
	void EnterLive();
	void RequestDump(EAfter eAfter);
	void PollDump();
	void FinishMatch();  // writes files if data present, then resets
	void ResetMatch();

	// ---- io / parse ----
	std::vector<VprofNode_t> ParseInterval(const std::string& sText);
	void ApplyInterval(const std::vector<VprofNode_t>& vNodes, long long iFrames, double flAvgMs);
	void WriteMatch();
	void UpdateRollup();
	void UpdateManifest();                       // regenerate builds.txt (build inventory for Claude)
	void LoadExclusions();                        // (re)read excluded_builds.txt, curated by Claude
	bool IsBuildExcluded(const std::string& sID); // uses the loaded exclusion set + 'before' rules

	static std::string SanitizeMap(const char* sLevelName);
	static std::string GameModeOf(const std::string& sMap);

	// ---- paths ----
	bool m_bPathsReady = false;
	std::string m_sDir = "";        // <game>/Amalgam/vprof/
	std::string m_sMatchesDir = ""; // <game>/Amalgam/vprof/matches/
	std::string m_sCaptureLog = ""; // absolute path we point con_logfile at
	std::string m_sExclusionsFile = "";
	std::string m_sManifestFile = "";

	// ---- vprof command handling ----
	bool m_bVprofUnlocked = false;
	bool m_bVprofEnabled = false;
	std::string m_sEnableCmd = "vprof\n";
	std::string m_sDisableCmd = "vprof\n";

	// ---- build id / exclusions (exclusions are curated by Claude via excluded_builds.txt) ----
	std::string m_sBuildID = "";
	std::unordered_set<uint32_t> m_sExcluded;    // hashes of explicitly-excluded build ids
	std::vector<std::string> m_vExcludeBefore;   // 'before <id>' thresholds; exclude ids < these

	// ---- intent flags set by Event() ----
	bool m_bLiveRequested = false;
	bool m_bAutoLive = false;        // live was inferred (joined mid-round), not event-driven
	double m_flInGameSince = 0.0;    // 0 when not in game

	// ---- state machine ----
	EPhase m_ePhase = EPhase::Idle;
	EAfter m_eAfter = EAfter::Continue;
	bool m_bMatchActive = false;

	// ---- timing ----
	double m_flLastFrame = 0.0;
	double m_flNextDumpTime = 0.0;
	double m_flDumpDeadline = 0.0;

	// ---- capture bookkeeping ----
	uintmax_t m_iLogOffset = 0;
	long long m_iIntervalFrames = 0;
	double m_flWindowSumMs = 0.0;
	long long m_iWindowFrames = 0;

	// ---- match data ----
	std::string m_sMap = "";
	std::string m_sMode = "";
	double m_flMatchStart = 0.0;
	int m_iSlots = 0;
	std::unordered_map<std::string, VprofAgg_t> m_mAgg;
	std::vector<float> m_vFrameMs; // live frame times across the whole match
	std::vector<VprofInterval_t> m_vIntervals; // all intervals, for 5%-low re-aggregation
};

ADD_FEATURE(CAutoVprof, AutoVprof);
