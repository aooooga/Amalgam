#include "AutoVprof.h"

#include "../../../SDK/Definitions/Interfaces/ICVar.h"
#include "../../../Utils/Perf/Tracker.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <set>
#include <map>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <functional>

// Linker-provided base of this module — lets us find our own DLL path without knowing its name.
extern "C" IMAGE_DOS_HEADER __ImageBase;

// ---- tunables -----------------------------------------------------------------
static constexpr double INTERVAL = 1.0;        // seconds of accumulation per report dump
static constexpr double DUMP_TIMEOUT = 0.75;   // max wait for a report to appear in the log
static constexpr int    TOP_N = 30;            // nodes kept in per-match report
static constexpr double PRUNE_PCT = 0.1;       // drop nodes below this % of frame time
static constexpr int    ROLLUP_N = 5;          // matches averaged in the per-map rollup
static constexpr int    INTERVAL_TOP = 48;     // nodes retained per interval for 5%-low re-agg
static constexpr int    HEAVY_TOP = 15;        // nodes in the per-match #heavy (5% low) section
static constexpr double HEAVY_FRAC = 0.05;     // fraction of frames the heavy section covers
// Joining a server mid-round means teamplay_round_start already fired, so the
// event-driven start never happens and the whole session captures nothing. After
// this long in game with no round event, start capturing anyway.
static constexpr double MIDJOIN_DELAY = 20.0;
// ...but don't leave junk files behind for a few seconds of capture.
static constexpr double MIN_MATCH_SEC = 10.0;
// Rollup budget for the tracker's rows — these files are read several at a time,
// so the rollup keeps a shorter list than the per-match report.
static constexpr int AMALGAM_TOP = 15;
static constexpr int AMALGAM_HEAVY_TOP = 8;

// vprof_generate_report node line = <name (may contain spaces)> then 10 numeric columns:
//   0 Calls  1 Calls/Frame  2 Time+Child  3 Pct(incl)  4 Time  5 Pct(self)
//   6 Avg/Frame(ms incl)  7 Avg/Call  8 Avg-NoChild  9 Peak
// We parse the numbers from the right so multi-word scope names stay intact.
static constexpr int VPROF_COLS = 10;
static constexpr int COL_CALLS_FRAME = 1;
static constexpr int COL_INCL_PCT = 3;
static constexpr int COL_SELF_PCT = 5;
static constexpr int COL_INCL_MS = 6;

// vprof brackets its own report with these; parsing on them (rather than our own echo
// markers) is robust to the report output being deferred a frame after the command.
static const char* MARK_BEGIN = "BEGIN VPROF REPORT";
static const char* MARK_END = "END VPROF REPORT";
static const char* CAPTURE_NAME = "amalgam_vprof_capture.log";

// ---- small helpers ------------------------------------------------------------
static bool ParseNum(std::string sTok, double& flOut)
{
	sTok.erase(std::remove(sTok.begin(), sTok.end(), ','), sTok.end());
	while (!sTok.empty() && (std::isalpha(static_cast<unsigned char>(sTok.back())) || sTok.back() == '%'))
		sTok.pop_back();
	if (sTok.empty())
		return false;

	char* pEnd = nullptr;
	double v = std::strtod(sTok.c_str(), &pEnd);
	if (pEnd == sTok.c_str())
		return false;

	flOut = v;
	return true;
}

static std::vector<std::string> Tokenize(const std::string& sLine)
{
	std::vector<std::string> v;
	std::istringstream ss(sLine);
	std::string s;
	while (ss >> s)
		v.push_back(s);
	return v;
}

static std::string NowStamp()
{
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
	localtime_s(&tmv, &t);
	char b[32];
	std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &tmv);
	return b;
}

// ---- paths / build id ---------------------------------------------------------
void CAutoVprof::EnsurePaths()
{
	if (m_bPathsReady)
		return;

	try
	{
		std::string sRoot = std::filesystem::current_path().string();
		m_sDir = sRoot + "\\Amalgam\\vprof\\";
		m_sMatchesDir = m_sDir + "matches\\";
		m_sExclusionsFile = m_sDir + "excluded_builds.txt";
		m_sManifestFile = m_sDir + "builds.txt";
		// con_logfile is resolved by the engine relative to the mod dir (tf/), so the
		// capture file must live there — a bare filename lands in tf/ directly.
		m_sCaptureLog = sRoot + "\\tf\\" + CAPTURE_NAME;

		std::filesystem::create_directories(m_sDir);
		std::filesystem::create_directories(m_sMatchesDir);

		// Seed excluded_builds.txt (Claude-curated) with a commented template so the format
		// is self-documenting. Never overwrite an existing one.
		if (!std::filesystem::exists(m_sExclusionsFile))
		{
			std::ofstream f(m_sExclusionsFile);
			f << "# Amalgam vprof build exclusions - curated by Claude, not the user.\n"
			     "# Matches recorded on excluded builds are dropped from the per-map rollups (<map>.txt).\n"
			     "# See builds.txt for the inventory of builds that have recorded matches.\n"
			     "# One build id per line; '#' starts a comment/reason. Two forms:\n"
			     "#   20260717-1440         # exclude just this build\n"
			     "#   before 20260718-2000  # exclude every build older than this id\n";
		}

		m_bPathsReady = true;
	}
	catch (...) {}
}

const std::string& CAutoVprof::GetBuildID()
{
	if (!m_sBuildID.empty())
		return m_sBuildID;

	m_sBuildID = "unknown";
	try
	{
		char sPath[MAX_PATH] = {};
		if (GetModuleFileNameA(reinterpret_cast<HMODULE>(&__ImageBase), sPath, MAX_PATH))
		{
			auto tFile = std::filesystem::last_write_time(sPath);
			auto tSys = std::chrono::clock_cast<std::chrono::system_clock>(tFile);
			std::time_t t = std::chrono::system_clock::to_time_t(tSys);
			std::tm tmv{};
			localtime_s(&tmv, &t);
			char b[24];
			std::strftime(b, sizeof(b), "%Y%m%d-%H%M", &tmv);
			m_sBuildID = b;
		}
	}
	catch (...) {}

	return m_sBuildID;
}

// ---- exclusions (curated by Claude via excluded_builds.txt; the tool only reads it) ----
void CAutoVprof::LoadExclusions()
{
	m_sExcluded.clear();
	m_vExcludeBefore.clear();
	try
	{
		std::ifstream f(m_sExclusionsFile);
		std::string sLine;
		while (std::getline(f, sLine))
		{
			if (auto p = sLine.find('#'); p != std::string::npos) // strip inline comments/reasons
				sLine = sLine.substr(0, p);
			auto v = Tokenize(sLine);
			if (v.empty())
				continue;
			if (v[0] == "before" && v.size() >= 2)
				m_vExcludeBefore.push_back(v[1]); // exclude all builds older than this id
			else
				m_sExcluded.insert(FNV1A::Hash32(v[0].c_str()));
		}
	}
	catch (...) {}
}

bool CAutoVprof::IsBuildExcluded(const std::string& sID)
{
	if (m_sExcluded.contains(FNV1A::Hash32(sID.c_str())))
		return true;
	// build ids are sortable timestamps (YYYYMMDD-HHMM), so a lexical compare is chronological
	for (const auto& sBefore : m_vExcludeBefore)
		if (sID < sBefore)
			return true;
	return false;
}

// ---- map helpers --------------------------------------------------------------
std::string CAutoVprof::SanitizeMap(const char* sLevelName)
{
	std::string s = sLevelName ? sLevelName : "unknown";
	if (auto p = s.find_last_of("/\\"); p != std::string::npos)
		s = s.substr(p + 1);
	if (s.size() > 4 && s.compare(s.size() - 4, 4, ".bsp") == 0)
		s = s.substr(0, s.size() - 4);
	for (auto& c : s)
	{
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
			c = '_';
	}
	if (s.empty())
		s = "unknown";
	return s;
}

std::string CAutoVprof::GameModeOf(const std::string& sMap)
{
	auto p = sMap.find('_');
	if (p == std::string::npos)
		return "other";
	std::string sPrefix = sMap.substr(0, p);
	static const std::unordered_map<std::string, const char*> s_mModes = {
		{ "koth", "koth" }, { "pl", "payload" }, { "plr", "payloadrace" }, { "cp", "controlpoint" },
		{ "ctf", "ctf" }, { "mvm", "mvm" }, { "pd", "playerdestruction" }, { "sd", "specialdelivery" },
		{ "tc", "territorial" }, { "arena", "arena" }, { "vsh", "vsh" }, { "rd", "robotdestruction" },
		{ "pass", "passtime" }, { "tr", "training" }, { "mge", "mge" }, { "jump", "jump" }, { "surf", "surf" }
	};
	if (auto it = s_mModes.find(sPrefix); it != s_mModes.end())
		return it->second;
	return sPrefix;
}

// ---- vprof command handling ---------------------------------------------------
void CAutoVprof::UnlockVprof()
{
	if (m_bVprofUnlocked)
		return;
	m_bVprofUnlocked = true;

	bool bHasOn = false, bHasOff = false;
	for (ConCommandBase* p = I::CVar->GetCommands(); p; p = p->m_pNext)
	{
		const char* n = p->m_pszName;
		if (!n || std::strncmp(n, "vprof", 5) != 0)
			continue;
		// vprof_* are typically FCVAR_CHEAT/DEVELOPMENTONLY, which silently no-ops the
		// enable command while report/reset still run — hence "No samples". Clear them.
		p->m_nFlags &= ~(FCVAR_HIDDEN | FCVAR_DEVELOPMENTONLY | FCVAR_CHEAT | FCVAR_NOT_CONNECTED);
		if (std::strcmp(n, "vprof_on") == 0) bHasOn = true;
		if (std::strcmp(n, "vprof_off") == 0) bHasOff = true;
	}

	// Prefer explicit on/off (idempotent); fall back to the `vprof` toggle if absent.
	if (bHasOn && bHasOff) { m_sEnableCmd = "vprof_on\n"; m_sDisableCmd = "vprof_off\n"; }
	else { m_sEnableCmd = "vprof\n"; m_sDisableCmd = "vprof\n"; }
}

void CAutoVprof::EnableVprof()
{
	if (m_bVprofEnabled)
		return;
	I::EngineClient->ClientCmd_Unrestricted(m_sEnableCmd.c_str());
	m_bVprofEnabled = true;
}

void CAutoVprof::DisableVprof()
{
	if (!m_bVprofEnabled)
		return;
	I::EngineClient->ClientCmd_Unrestricted(m_sDisableCmd.c_str());
	m_bVprofEnabled = false;
}

// ---- lifecycle ----------------------------------------------------------------
void CAutoVprof::StartMatch()
{
	EnsurePaths();

	// A match still active here means its teardown never reached us; write it
	// before its data is overwritten by the new one.
	if (m_bMatchActive)
		FinishMatch();

	ResetMatch();

	m_bMatchActive = true;
	m_ePhase = EPhase::Idle;
	m_sMap = SanitizeMap(I::EngineClient->GetLevelName());
	m_sMode = GameModeOf(m_sMap);
	m_iSlots = I::EngineClient->GetMaxClients();
	m_flMatchStart = SDK::PlatFloatTime();

	// Point con_logfile at our capture file and clear it so offsets start fresh.
	try { std::ofstream(m_sCaptureLog, std::ios::trunc).close(); } catch (...) {}
	if (auto pCVar = I::CVar->FindVar("con_logfile"))
		pCVar->SetValue(CAPTURE_NAME);

	// The process tracker records over exactly the same live frames, so its
	// `#amalgam` sections in the report describe the same match the vprof nodes
	// above them do.
	Perf::Tracker.BeginMatch();

	// Enable vprof once for the whole match; per-interval we only reset.
	UnlockVprof();
	EnableVprof();
}

void CAutoVprof::EnterLive()
{
	m_iIntervalFrames = 0;
	m_iWindowFrames = 0;
	m_flWindowSumMs = 0.0;

	// NB: issue ONLY vprof_reset here. vprof_* are deferred commands sharing one global
	// slot (executed once/frame in PreUpdateProfile); issuing a second deferred vprof
	// command the same frame silently drops the first. Enabling is done once in StartMatch,
	// a separate frame, and vprof stays enabled for the whole match.
	I::EngineClient->ClientCmd_Unrestricted("vprof_reset\n");
	m_flNextDumpTime = SDK::PlatFloatTime() + INTERVAL;
	m_ePhase = EPhase::Live;
	Perf::Tracker.SetLive(true);
}

void CAutoVprof::RequestDump(EAfter eAfter)
{
	m_eAfter = eAfter;
	m_iLogOffset = 0;
	try
	{
		if (std::filesystem::exists(m_sCaptureLog))
			m_iLogOffset = std::filesystem::file_size(m_sCaptureLog);
	}
	catch (...) {}

	I::EngineClient->ClientCmd_Unrestricted("vprof_generate_report\n");
	m_flDumpDeadline = SDK::PlatFloatTime() + DUMP_TIMEOUT;
	m_ePhase = EPhase::WaitDump;
}

void CAutoVprof::PollDump()
{
	std::string sSection;
	bool bComplete = false;

	try
	{
		std::ifstream f(m_sCaptureLog, std::ios::binary);
		if (f)
		{
			f.seekg(0, std::ios::end);
			uintmax_t iEnd = static_cast<uintmax_t>(f.tellg());
			uintmax_t iFrom = m_iLogOffset <= iEnd ? m_iLogOffset : 0;
			f.seekg(static_cast<std::streamoff>(iFrom));
			std::stringstream ss;
			ss << f.rdbuf();
			std::string sBuf = ss.str();

			auto iB = sBuf.find(MARK_BEGIN);
			auto iE = sBuf.find(MARK_END);
			if (iB != std::string::npos && iE != std::string::npos && iE > iB)
			{
				iB = sBuf.find('\n', iB);
				sSection = (iB == std::string::npos) ? "" : sBuf.substr(iB + 1, iE - iB - 1);
				bComplete = true;
			}
		}
	}
	catch (...) {}

	if (bComplete)
	{
		auto vNodes = ParseInterval(sSection);
		double flAvgMs = m_iWindowFrames ? m_flWindowSumMs / m_iWindowFrames : 0.0;
		ApplyInterval(vNodes, m_iIntervalFrames, flAvgMs);
	}
	else if (SDK::PlatFloatTime() < m_flDumpDeadline)
	{
		return; // keep waiting
	}

	// advance regardless (either applied, or timed out)
	switch (m_eAfter)
	{
	case EAfter::Continue: EnterLive(); break;
	case EAfter::Pause:    m_ePhase = EPhase::Idle; Perf::Tracker.SetLive(false); break; // leave vprof on; just stop dumping
	case EAfter::EndMatch: FinishMatch(); break;           // FinishMatch disables vprof
	}
}

void CAutoVprof::ResetMatch()
{
	m_ePhase = EPhase::Idle;
	m_iIntervalFrames = 0;
	m_iWindowFrames = 0;
	m_flWindowSumMs = 0.0;
	m_mAgg.clear();
	m_vFrameMs.clear();
	m_vIntervals.clear();
	m_sMap.clear();
	m_sMode.clear();
	m_iSlots = 0;
	m_flMatchStart = 0.0;
}

void CAutoVprof::FinishMatch()
{
	DisableVprof();
	if (auto pCVar = I::CVar->FindVar("con_logfile"))
		pCVar->SetValue("");

	Perf::Tracker.SetLive(false);

	double flDurSec = 0.0;
	for (float m : m_vFrameMs)
		flDurSec += m;
	flDurSec /= 1000.0;

	// The tracker can have usable data even when the vprof side came back empty
	// (a short round, a dropped dump), so write the file if either half has
	// something to say.
	if (m_bMatchActive && flDurSec >= MIN_MATCH_SEC && (!m_mAgg.empty() || Perf::Tracker.HasMatchData()))
	{
		WriteMatch();
		LoadExclusions(); // re-read fresh so Claude's edits apply without re-injecting
		UpdateRollup();
		UpdateManifest();
	}
	Perf::Tracker.EndMatch();

	ResetMatch();
	m_bMatchActive = false;
	m_bAutoLive = false;
}

// ---- finalize paths -----------------------------------------------------------
// A match's data is in memory only until FinishMatch writes it, and the one path
// that used to reach it - the client_disconnect game event - is not reliably
// delivered to the client that is itself leaving. Anything that ends a match now
// goes through one of these instead.

// IBaseClientDLL::LevelShutdown: the engine's own teardown. Runs on disconnect,
// map change and quit, before the interfaces we write with go away.
void CAutoVprof::LevelShutdown()
{
	if (!m_bMatchActive)
		return;

	m_bLiveRequested = false;
	FinishMatch();
}

// Menu paint: a match left active while out of game means no teardown reached us
// (or one ran before we were hooked). Cheap enough to check every menu frame.
void CAutoVprof::FlushOrphaned()
{
	if (!m_bMatchActive || I::EngineClient->IsInGame())
		return;

	m_bLiveRequested = false;
	FinishMatch();
}

// DLL unload (F11, host shutdown/restart). Last chance: after this the hooks are
// gone and the capture is lost.
void CAutoVprof::Shutdown()
{
	if (!m_bMatchActive)
		return;

	m_bLiveRequested = false;
	FinishMatch();
}

// ---- parsing / aggregation ----------------------------------------------------
std::vector<VprofNode_t> CAutoVprof::ParseInterval(const std::string& sText)
{
	std::vector<VprofNode_t> vOut;
	std::unordered_set<std::string> seen; // dedupe within one report
	std::istringstream ss(sText);
	std::string sLine;
	while (std::getline(ss, sLine))
	{
		if (sLine.find(MARK_BEGIN) != std::string::npos || sLine.find(MARK_END) != std::string::npos)
			continue;

		auto vTok = Tokenize(sLine);
		if (static_cast<int>(vTok.size()) < VPROF_COLS + 1)
			continue; // need name + all numeric columns

		// Collect the trailing numeric columns from the right; the rest is the name.
		std::vector<double> vNums; // will be right-to-left, reversed after
		int i = static_cast<int>(vTok.size()) - 1;
		for (; i >= 0 && static_cast<int>(vNums.size()) < VPROF_COLS; i--)
		{
			double v;
			if (!ParseNum(vTok[i], v))
				break;
			vNums.push_back(v);
		}
		if (static_cast<int>(vNums.size()) < VPROF_COLS || i < 0)
			continue;
		std::reverse(vNums.begin(), vNums.end());

		std::string sName;
		for (int j = 0; j <= i; j++)
			sName += (j ? " " : "") + vTok[j];
		if (sName.empty() || std::none_of(sName.begin(), sName.end(), [](unsigned char c) { return std::isalpha(c); }))
			continue;
		if (!seen.insert(sName).second)
			continue;

		VprofNode_t tNode;
		tNode.m_sName = sName;
		tNode.m_flCallsFrame = vNums[COL_CALLS_FRAME];
		tNode.m_flInclPct = vNums[COL_INCL_PCT];
		tNode.m_flSelfPct = vNums[COL_SELF_PCT];
		tNode.m_flInclMs = vNums[COL_INCL_MS];
		vOut.push_back(std::move(tNode));
	}
	return vOut;
}

void CAutoVprof::ApplyInterval(const std::vector<VprofNode_t>& vNodes, long long iFrames, double flAvgMs)
{
	long long w = iFrames > 0 ? iFrames : 1;
	for (const auto& n : vNodes)
	{
		auto& a = m_mAgg[n.m_sName];
		a.m_flSumCalls += n.m_flCallsFrame * static_cast<double>(w);
		a.m_flSumSelfPct += n.m_flSelfPct * static_cast<double>(w);
		a.m_flSumInclPct += n.m_flInclPct * static_cast<double>(w);
		a.m_flSumInclMs += n.m_flInclMs * static_cast<double>(w);
		a.m_iFrames += w;
	}

	// Retain the interval (trimmed to the heaviest nodes) so the match's worst ~5% of
	// frames can be re-aggregated into their own node ranking at match end.
	{
		VprofInterval_t tI;
		tI.m_iFrames = w;
		tI.m_flAvgMs = flAvgMs;
		tI.m_vNodes = vNodes;
		std::sort(tI.m_vNodes.begin(), tI.m_vNodes.end(),
			[](const VprofNode_t& a, const VprofNode_t& b) { return a.SelfMs() > b.SelfMs(); });
		if (static_cast<int>(tI.m_vNodes.size()) > INTERVAL_TOP)
			tI.m_vNodes.resize(INTERVAL_TOP);
		m_vIntervals.push_back(std::move(tI));
	}
}

// ---- fps stats ----------------------------------------------------------------
static void FpsStats(std::vector<float> vMs, double& flAvg, double& fl1, double& fl5)
{
	flAvg = fl1 = fl5 = 0.0;
	if (vMs.empty())
		return;

	double flSum = 0.0;
	for (float m : vMs)
		flSum += m;
	flAvg = flSum > 0.0 ? 1000.0 / (flSum / vMs.size()) : 0.0;

	// worst frames = largest frame times
	std::sort(vMs.begin(), vMs.end(), std::greater<float>());
	auto low = [&](double flPct) -> double
	{
		size_t n = std::max<size_t>(1, static_cast<size_t>(vMs.size() * flPct));
		double s = 0.0;
		for (size_t i = 0; i < n; i++)
			s += vMs[i];
		double flMean = s / n;
		return flMean > 0.0 ? 1000.0 / flMean : 0.0;
	};
	fl1 = low(0.01);
	fl5 = low(0.05);
}

// ---- write per-match file -----------------------------------------------------
void CAutoVprof::WriteMatch()
{
	double flFpsAvg, flFps1, flFps5;
	FpsStats(m_vFrameMs, flFpsAvg, flFps1, flFps5);

	double flDurSec = 0.0;
	for (float m : m_vFrameMs)
		flDurSec += m;
	flDurSec /= 1000.0;

	struct Row { std::string name; double selfPct; double selfMs; double inclMs; double calls; };
	std::vector<Row> vRows;
	for (const auto& [sName, a] : m_mAgg)
	{
		if (a.m_iFrames <= 0)
			continue;
		double calls = a.m_flSumCalls / a.m_iFrames;
		double selfPct = a.m_flSumSelfPct / a.m_iFrames;
		double inclPct = a.m_flSumInclPct / a.m_iFrames;
		double inclMs = a.m_flSumInclMs / a.m_iFrames;
		double selfMs = inclPct > 0.0 ? inclMs * selfPct / inclPct : 0.0;
		if (selfPct < PRUNE_PCT)
			continue;
		vRows.push_back({ sName, selfPct, selfMs, inclMs, calls });
	}
	std::sort(vRows.begin(), vRows.end(), [](const Row& a, const Row& b) { return a.selfMs > b.selfMs; });
	if (static_cast<int>(vRows.size()) > TOP_N)
		vRows.resize(TOP_N);

	// 5%-low re-aggregation: take the heaviest intervals (by avg frame time) until they
	// cover ~HEAVY_FRAC of the match's frames, and rank nodes within just those moments.
	struct HRow { std::string name; double selfMs; };
	std::vector<HRow> vHeavy;
	long long iHeavyFrames = 0;
	double flHeavyAvgMs = 0.0;
	int iHeavyIntervals = 0;
	{
		long long iTotalFrames = 0;
		for (const auto& tI : m_vIntervals)
			iTotalFrames += tI.m_iFrames;

		std::vector<const VprofInterval_t*> vSorted;
		for (const auto& tI : m_vIntervals)
			vSorted.push_back(&tI);
		std::sort(vSorted.begin(), vSorted.end(),
			[](const VprofInterval_t* a, const VprofInterval_t* b) { return a->m_flAvgMs > b->m_flAvgMs; });

		long long iWant = std::max<long long>(1, static_cast<long long>(iTotalFrames * HEAVY_FRAC));
		std::unordered_map<std::string, std::pair<double, double>> mH; // name -> (sum selfms*w, sum w)
		double flMsSum = 0.0;
		for (const auto* pI : vSorted)
		{
			if (iHeavyFrames >= iWant)
				break;
			iHeavyIntervals++;
			iHeavyFrames += pI->m_iFrames;
			flMsSum += pI->m_flAvgMs * pI->m_iFrames;
			for (const auto& n : pI->m_vNodes)
			{
				auto& a = mH[n.m_sName];
				a.first += n.SelfMs() * pI->m_iFrames;
				a.second += pI->m_iFrames;
			}
		}
		flHeavyAvgMs = iHeavyFrames ? flMsSum / iHeavyFrames : 0.0;
		for (const auto& [name, a] : mH)
			if (a.second > 0.0)
				vHeavy.push_back({ name, a.first / static_cast<double>(iHeavyFrames) });
		std::sort(vHeavy.begin(), vHeavy.end(), [](const HRow& a, const HRow& b) { return a.selfMs > b.selfMs; });
		if (static_cast<int>(vHeavy.size()) > HEAVY_TOP)
			vHeavy.resize(HEAVY_TOP);
	}

	std::string sStamp = NowStamp();
	std::string sFile = m_sMatchesDir + m_sMap + "_" + sStamp + ".txt";

	try
	{
		std::ofstream f(sFile, std::ios::trunc);
		if (!f)
			return;
		f << std::fixed;
		f << "map=" << m_sMap << "\n";
		f << "mode=" << m_sMode << "\n";
		f << "slots=" << m_iSlots << "\n";
		f << "dur=" << static_cast<long long>(flDurSec) << "s frames=" << m_vFrameMs.size() << "\n";
		f.precision(1);
		f << "fps avg=" << flFpsAvg << " 1%low=" << flFps1 << " 5%low=" << flFps5 << "\n";
		f << "build=" << GetBuildID() << "\n";
		f << "time=" << sStamp << "\n";
		f << "# ranked by self ms/frame; self%=CPU self, inclms=ms/frame incl children\n";
		f << "#nodes self% selfms inclms calls/f name\n";
		f << std::setprecision(2);
		for (const auto& r : vRows)
			f << r.selfPct << " " << r.selfMs << " " << r.inclMs << " " << r.calls << " " << r.name << "\n";

		// same ranking but restricted to the heaviest ~5% of frames — what the lag is made of
		f.precision(1);
		f << "#heavy 5%low intervals=" << iHeavyIntervals << " frames=" << iHeavyFrames << " avgms=" << flHeavyAvgMs << "\n";
		f << std::setprecision(2);
		for (const auto& r : vHeavy)
			f << r.selfMs << " " << r.name << "\n";

		// The other half of the report: the same match, measured from inside
		// Amalgam. Everything above is the engine's view (including the work we
		// caused); everything below attributes it to our own code.
		Perf::Tracker.WriteMatchSections(f);
	}
	catch (...) {}
}

// ---- per-map rolling rollup ---------------------------------------------------
void CAutoVprof::UpdateRollup()
{
	struct Node { double selfPct; double selfMs; std::string name; };
	struct HNode { double selfMs; std::string name; };
	// Amalgam-side rows, from the tracker's sections of each match file.
	struct ANode { double selfMs; double calls; std::string name; };
	struct Rec
	{
		std::string build; double fps1 = 0, fps5 = 0, fpsAvg = 0;
		std::vector<Node> nodes; std::vector<HNode> heavyNodes;
		double amSelfMs = 0, amShare = 0, amOverhead = 0; bool amPresent = false;
		std::vector<ANode> amZones, amHeavy;
		std::string fname;
	};

	std::vector<Rec> vAll;
	std::string sPrefix = m_sMap + "_";

	try
	{
		for (const auto& e : std::filesystem::directory_iterator(m_sMatchesDir))
		{
			if (!e.is_regular_file())
				continue;
			std::string fn = e.path().filename().string();
			if (fn.rfind(sPrefix, 0) != 0 || fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".txt") != 0)
				continue;

			Rec r;
			r.fname = fn;
			std::ifstream f(e.path());
			std::string sLine;
			// 0 header, 1 nodes, 3 heavy (5% low),
				// 4 amalgam zones, 5 amalgam heavy, 6 tracker sections not rolled up
				int iSect = 0;
			while (std::getline(f, sLine))
			{
				while (!sLine.empty() && sLine.back() == '\r')
					sLine.pop_back();
				if (sLine.rfind("#nodes", 0) == 0) { iSect = 1; continue; }
				// spike snapshots were dropped from the format; older match files
				// still carry the section, so it is recognised and skipped
				if (sLine.rfind("#spikes", 0) == 0) { iSect = 6; continue; }
				if (sLine.rfind("#heavy", 0) == 0) { iSect = 3; continue; }
				// longest first: every tracker section starts with "#amalgam"
				if (sLine.rfind("#amalgam_zones", 0) == 0) { iSect = 4; continue; }
				if (sLine.rfind("#amalgam_heavy", 0) == 0) { iSect = 5; continue; }
				if (sLine.rfind("#amalgam_counters", 0) == 0 || sLine.rfind("#amalgam_worst", 0) == 0
					|| sLine.rfind("#amalgam_groups", 0) == 0) { iSect = 6; continue; }
				if (sLine.rfind("#amalgam", 0) == 0)
				{
					auto grabAm = [&](const char* key) -> double
					{
						auto p = sLine.find(key);
						return p == std::string::npos ? 0.0 : std::strtod(sLine.c_str() + p + std::strlen(key), nullptr);
					};
					r.amSelfMs = grabAm("selfms=");
					r.amShare = grabAm("share=");
					r.amOverhead = grabAm("overhead=");
					r.amPresent = true;
					iSect = 6;
					continue;
				}
				if (!sLine.empty() && sLine[0] == '#')
					continue; // in-section comment

				if (iSect == 0)
				{
					if (sLine.rfind("build=", 0) == 0)
						r.build = sLine.substr(6);
					else if (sLine.rfind("fps ", 0) == 0)
					{
						auto grab = [&](const char* key) -> double
						{
							auto p = sLine.find(key);
							if (p == std::string::npos) return 0.0;
							return std::strtod(sLine.c_str() + p + std::strlen(key), nullptr);
						};
						r.fpsAvg = grab("avg=");
						r.fps1 = grab("1%low=");
						r.fps5 = grab("5%low=");
					}
				}
				else if (iSect == 1)
				{
					auto v = Tokenize(sLine);
					if (v.size() >= 5)
					{
						Node n;
						n.selfPct = std::strtod(v[0].c_str(), nullptr);
						n.selfMs = std::strtod(v[1].c_str(), nullptr);
						for (size_t i = 4; i < v.size(); i++)
							n.name += (i > 4 ? " " : "") + v[i];
						r.nodes.push_back(std::move(n));
					}
				}
				else if (iSect == 3)
				{
					auto v = Tokenize(sLine);
					if (v.size() >= 2)
					{
						HNode n;
						n.selfMs = std::strtod(v[0].c_str(), nullptr);
						for (size_t i = 1; i < v.size(); i++)
							n.name += (i > 1 ? " " : "") + v[i];
						r.heavyNodes.push_back(std::move(n));
					}
				}
				else if (iSect == 4)
					{	// selfms inclms calls/f peakms group name
						auto v = Tokenize(sLine);
						if (v.size() >= 6)
						{
							ANode n;
							n.selfMs = std::strtod(v[0].c_str(), nullptr);
							n.calls = std::strtod(v[2].c_str(), nullptr);
							for (size_t i = 5; i < v.size(); i++)
								n.name += (i > 5 ? " " : "") + v[i];
							r.amZones.push_back(std::move(n));
						}
					}
					else if (iSect == 5)
					{	// selfms name
						auto v = Tokenize(sLine);
						if (v.size() >= 2)
						{
							ANode n;
							n.selfMs = std::strtod(v[0].c_str(), nullptr);
							for (size_t i = 1; i < v.size(); i++)
								n.name += (i > 1 ? " " : "") + v[i];
							r.amHeavy.push_back(std::move(n));
						}
					}
			}
			vAll.push_back(std::move(r));
		}
	}
	catch (...) {}

	// newest last (filenames embed sortable YYYYMMDD-HHMMSS)
	std::sort(vAll.begin(), vAll.end(), [](const Rec& a, const Rec& b) { return a.fname < b.fname; });

	int iTotal = static_cast<int>(vAll.size());
	std::vector<Rec*> vUsed;
	for (auto it = vAll.rbegin(); it != vAll.rend() && static_cast<int>(vUsed.size()) < ROLLUP_N; ++it)
	{
		if (IsBuildExcluded(it->build))
			continue;
		vUsed.push_back(&*it);
	}
	int iExcluded = 0;
	for (auto& r : vAll)
		if (IsBuildExcluded(r.build))
			iExcluded++;

	if (vUsed.empty())
		return;

	std::unordered_map<std::string, std::pair<double, double>> mAgg; // name -> (sum self%, sum selfms)
	std::unordered_map<std::string, double> mHeavy;                  // name -> sum heavy selfms
	int iHeavyMatches = 0;                                           // matches with a #heavy section
	for (auto* r : vUsed)
	{
		for (const auto& n : r->nodes)
		{
			auto& a = mAgg[n.name];
			a.first += n.selfPct;
			a.second += n.selfMs;
		}
		if (!r->heavyNodes.empty())
			iHeavyMatches++;
		for (const auto& n : r->heavyNodes)
			mHeavy[n.name] += n.selfMs;
	}

	// ---- tracker side: average the Amalgam zones over the same matches ----
	std::unordered_map<std::string, std::pair<double, double>> mAmZones; // name -> (sum selfms, sum calls)
	std::unordered_map<std::string, double> mAmHeavy;
	int iAmMatches = 0, iAmHeavyMatches = 0;
	double flAmSelfMs = 0.0, flAmShare = 0.0, flAmOverhead = 0.0;
	for (auto* r : vUsed)
	{
		if (!r->amPresent)
			continue;
		iAmMatches++;
		flAmSelfMs += r->amSelfMs;
		flAmShare += r->amShare;
		flAmOverhead += r->amOverhead;
		for (const auto& n : r->amZones)
		{
			auto& a = mAmZones[n.name];
			a.first += n.selfMs;
			a.second += n.calls;
		}
		if (!r->amHeavy.empty())
			iAmHeavyMatches++;
		for (const auto& n : r->amHeavy)
			mAmHeavy[n.name] += n.selfMs;
	}

	struct ARow { double selfPct; double selfMs; std::string name; };
	std::vector<ARow> vRows;
	int iUsed = static_cast<int>(vUsed.size());
	for (const auto& [name, a] : mAgg)
		vRows.push_back({ a.first / iUsed, a.second / iUsed, name });
	std::sort(vRows.begin(), vRows.end(), [](const ARow& a, const ARow& b) { return a.selfMs > b.selfMs; });
	if (static_cast<int>(vRows.size()) > TOP_N)
		vRows.resize(TOP_N);

	try
	{
		std::ofstream f(m_sDir + m_sMap + ".txt", std::ios::trunc);
		f << std::fixed;
		f.precision(1);
		f << "map=" << m_sMap << " mode=" << m_sMode << "\n";
		f << "matches=" << iUsed << " of " << iTotal << " (" << iExcluded << " excluded-build)\n";
		f << "fps 1%lows:";
		for (auto it = vUsed.rbegin(); it != vUsed.rend(); ++it)
			f << " " << (*it)->fps1;
		f << "\nfps avgs:";
		for (auto it = vUsed.rbegin(); it != vUsed.rend(); ++it)
			f << " " << (*it)->fpsAvg;
		f << "\n#nodes self% selfms name (avg of last " << iUsed << ")\n";
		f << std::setprecision(2);
		for (const auto& r : vRows)
			f << r.selfPct << " " << r.selfMs << " " << r.name << "\n";
		if (iHeavyMatches > 0)
		{
			struct HR { double selfMs; std::string name; };
			std::vector<HR> vH;
			for (const auto& [name, ms] : mHeavy)
				vH.push_back({ ms / iHeavyMatches, name });
			std::sort(vH.begin(), vH.end(), [](const HR& a, const HR& b) { return a.selfMs > b.selfMs; });
			if (static_cast<int>(vH.size()) > HEAVY_TOP)
				vH.resize(HEAVY_TOP);
			f << "#heavy selfms name (5%low frames, avg of " << iHeavyMatches << ")\n";
			for (const auto& r : vH)
				f << r.selfMs << " " << r.name << "\n";
		}
		f.precision(1);

		// ---- Amalgam's own zones, averaged over the same matches ----
		if (iAmMatches > 0)
		{
			f << std::setprecision(3);
			f << "#amalgam selfms=" << flAmSelfMs / iAmMatches
			  << " share=" << std::setprecision(1) << flAmShare / iAmMatches << "%"
			  << std::setprecision(3) << " overhead=" << flAmOverhead / iAmMatches
			  << " (avg of " << iAmMatches << ")\n";

			struct AR { double selfMs; double calls; std::string name; };
			std::vector<AR> vA;
			for (const auto& [name, a] : mAmZones)
				vA.push_back({ a.first / iAmMatches, a.second / iAmMatches, name });
			std::sort(vA.begin(), vA.end(), [](const AR& a, const AR& b) { return a.selfMs > b.selfMs; });
			if (static_cast<int>(vA.size()) > AMALGAM_TOP)
				vA.resize(AMALGAM_TOP);
			f << "#amalgam_zones selfms calls/f name\n";
			for (const auto& r : vA)
				f << r.selfMs << " " << std::setprecision(1) << r.calls << " " << std::setprecision(3) << r.name << "\n";

			if (iAmHeavyMatches > 0)
			{
				std::vector<AR> vH;
				for (const auto& [name, ms] : mAmHeavy)
					vH.push_back({ ms / iAmHeavyMatches, 0.0, name });
				std::sort(vH.begin(), vH.end(), [](const AR& a, const AR& b) { return a.selfMs > b.selfMs; });
				if (static_cast<int>(vH.size()) > AMALGAM_HEAVY_TOP)
					vH.resize(AMALGAM_HEAVY_TOP);
				f << "#amalgam_heavy selfms name (5%low frames, avg of " << iAmHeavyMatches << ")\n";
				for (const auto& r : vH)
					f << r.selfMs << " " << r.name << "\n";
			}
		}
	}
	catch (...) {}
}

// ---- build inventory (builds.txt) — the surface Claude curates exclusions against ----
void CAutoVprof::UpdateManifest()
{
	struct Info { int matches = 0; std::set<std::string> maps; std::string first, last; };
	std::map<std::string, Info> mBuilds; // build id -> aggregate, ordered

	try
	{
		for (const auto& e : std::filesystem::directory_iterator(m_sMatchesDir))
		{
			if (!e.is_regular_file() || e.path().extension() != ".txt")
				continue;

			std::string sBuild, sMap, sTime;
			std::ifstream f(e.path());
			std::string sLine;
			while (std::getline(f, sLine) && sLine.rfind("#nodes", 0) != 0)
			{
				while (!sLine.empty() && sLine.back() == '\r')
					sLine.pop_back();
				if (sLine.rfind("build=", 0) == 0) sBuild = sLine.substr(6);
				else if (sLine.rfind("map=", 0) == 0) sMap = sLine.substr(4);
				else if (sLine.rfind("time=", 0) == 0) sTime = sLine.substr(5);
			}
			if (sBuild.empty())
				continue;

			auto& info = mBuilds[sBuild];
			info.matches++;
			if (!sMap.empty()) info.maps.insert(sMap);
			if (!sTime.empty())
			{
				if (info.first.empty() || sTime < info.first) info.first = sTime;
				if (info.last.empty() || sTime > info.last) info.last = sTime;
			}
		}
	}
	catch (...) {}

	try
	{
		std::ofstream f(m_sManifestFile, std::ios::trunc);
		f << "# Amalgam build inventory (auto-generated; do not hand-edit).\n"
		     "# Each build id is the DLL link timestamp. To drop a stale build's matches from the\n"
		     "# per-map rollups, add its id to excluded_builds.txt (or a 'before <id>' line there).\n"
		     "# build            matches  maps  first-match       last-match        excluded\n";
		for (auto it = mBuilds.rbegin(); it != mBuilds.rend(); ++it) // newest first
		{
			const auto& [sBuild, info] = *it;
			std::string sMaps;
			for (const auto& m : info.maps)
				sMaps += (sMaps.empty() ? "" : ",") + m;
			f << sBuild << "  matches=" << info.matches << "  maps=" << (sMaps.empty() ? "-" : sMaps)
			  << "  first=" << (info.first.empty() ? "-" : info.first)
			  << "  last=" << (info.last.empty() ? "-" : info.last)
			  << "  excluded=" << (IsBuildExcluded(sBuild) ? "yes" : "no") << "\n";
		}
	}
	catch (...) {}
}

// ---- events -------------------------------------------------------------------
void CAutoVprof::Event(uint32_t uHash)
{
	if (!Vars::Debug::AutoVprof.Value)
		return;

	switch (uHash)
	{
	case FNV1A::Hash32Const("teamplay_round_start"):
		m_bLiveRequested = true;
		m_bAutoLive = false; // the real event took over from the mid-join fallback
		break;
	case FNV1A::Hash32Const("teamplay_round_win"):
		m_bLiveRequested = false;
		break;
	case FNV1A::Hash32Const("teamplay_game_over"):
	case FNV1A::Hash32Const("tf_game_over"):
	case FNV1A::Hash32Const("client_disconnect"):
	case FNV1A::Hash32Const("game_newmap"):
		// Finalize synchronously here rather than deferring to Run(): on disconnect the
		// engine stops painting in-game panels, so Run() would never fire again to flush.
		// The match aggregate is complete in memory each interval, so no final dump is needed.
		m_bLiveRequested = false;
		FinishMatch();
		break;
	}
}

// ---- per-frame driver ---------------------------------------------------------
void CAutoVprof::Run()
{
	if (!Vars::Debug::AutoVprof.Value)
	{
		// Toggling the feature off used to discard the capture outright. Write it
		// instead - FinishMatch also disables vprof and releases con_logfile.
		if (m_bMatchActive)
		{
			m_bLiveRequested = false;
			FinishMatch();
		}
		return;
	}

	EnsurePaths();

	double flNow = SDK::PlatFloatTime();
	double flDtMs = m_flLastFrame > 0.0 ? (flNow - m_flLastFrame) * 1000.0 : 0.0;
	m_flLastFrame = flNow;
	if (flDtMs < 0.0 || flDtMs > 1000.0)
		flDtMs = 0.0; // ignore load hitches / alt-tab

	// Match-end finalize happens in Event() / LevelShutdown() / FlushOrphaned():
	// leaving a match stops in-game painting, so this function cannot be relied
	// on to flush.
	if (!I::EngineClient->IsInGame())
	{
		m_flInGameSince = 0.0;
		m_bAutoLive = false;
		return;
	}

	if (!m_flInGameSince)
		m_flInGameSince = flNow;

	// Joined a round already in progress: teamplay_round_start fired before we
	// connected, so nothing would ever start the capture and the entire session
	// - however long it ran - would produce no report at all. Start anyway once
	// we've been in game long enough for the join to have settled.
	if (!m_bLiveRequested && !m_bMatchActive && !m_bAutoLive && flNow - m_flInGameSince > MIDJOIN_DELAY)
		m_bLiveRequested = m_bAutoLive = true;

	if (m_bLiveRequested && !m_bMatchActive)
	{
		StartMatch();  // issues vprof_on this frame; EnterLive (vprof_reset) waits until
		return;        // next frame so the two deferred commands don't clobber each other
	}
	if (!m_bMatchActive)
		return;

	if (m_bLiveRequested && m_ePhase == EPhase::Idle)
		EnterLive();
	else if (!m_bLiveRequested && m_ePhase == EPhase::Live)
		RequestDump(EAfter::Pause);

	// sample every live-ish frame so fps lows have no per-interval gaps
	if ((m_ePhase == EPhase::Live || m_ePhase == EPhase::WaitDump) && flDtMs > 0.0)
		m_vFrameMs.push_back(static_cast<float>(flDtMs));

	if (m_ePhase == EPhase::Live)
	{
		if (flDtMs > 0.0)
		{
			m_iIntervalFrames++;
			m_iWindowFrames++;
			m_flWindowSumMs += flDtMs;
		}
		if (flNow >= m_flNextDumpTime && m_iIntervalFrames > 0)
			RequestDump(EAfter::Continue);
	}
	else if (m_ePhase == EPhase::WaitDump)
	{
		PollDump();
	}
}
