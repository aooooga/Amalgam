#include "AutoVprof.h"

#include "../../../SDK/Definitions/Interfaces/ICVar.h"

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
static constexpr double SPIKE_FACTOR = 2.0;    // frame > factor * EMA => spike
static constexpr double SPIKE_FLOOR_MS = 8.0;  // ...and above this absolute floor
static constexpr int    MAX_SNAPSHOTS = 5;     // retained spike snapshots per match
static constexpr int    TOP_N = 40;            // nodes kept in per-match report
static constexpr int    SNAPSHOT_TOP = 12;     // nodes kept per spike snapshot
static constexpr double PRUNE_PCT = 0.1;       // drop nodes below this % of frame time
static constexpr int    ROLLUP_N = 5;          // matches averaged in the per-map rollup

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

	// Enable vprof once for the whole match; per-interval we only reset.
	UnlockVprof();
	EnableVprof();
}

void CAutoVprof::EnterLive()
{
	m_iIntervalFrames = 0;
	m_iWindowFrames = 0;
	m_flWindowSumMs = 0.0;
	m_flWindowWorstMs = 0.0;
	m_bWindowSpike = false;

	// NB: issue ONLY vprof_reset here. vprof_* are deferred commands sharing one global
	// slot (executed once/frame in PreUpdateProfile); issuing a second deferred vprof
	// command the same frame silently drops the first. Enabling is done once in StartMatch,
	// a separate frame, and vprof stays enabled for the whole match.
	I::EngineClient->ClientCmd_Unrestricted("vprof_reset\n");
	m_flNextDumpTime = SDK::PlatFloatTime() + INTERVAL;
	m_ePhase = EPhase::Live;
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
		ApplyInterval(vNodes, m_iIntervalFrames, m_bWindowSpike, m_flWindowWorstMs, flAvgMs);
	}
	else if (SDK::PlatFloatTime() < m_flDumpDeadline)
	{
		return; // keep waiting
	}

	// advance regardless (either applied, or timed out)
	switch (m_eAfter)
	{
	case EAfter::Continue: EnterLive(); break;
	case EAfter::Pause:    m_ePhase = EPhase::Idle; break; // leave vprof on; just stop dumping
	case EAfter::EndMatch: FinishMatch(); break;           // FinishMatch disables vprof
	}
}

void CAutoVprof::ResetMatch()
{
	m_ePhase = EPhase::Idle;
	m_iIntervalFrames = 0;
	m_iWindowFrames = 0;
	m_flWindowSumMs = 0.0;
	m_flWindowWorstMs = 0.0;
	m_bWindowSpike = false;
	m_mAgg.clear();
	m_vFrameMs.clear();
	m_vSpikes.clear();
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

	if (m_bMatchActive && !m_vFrameMs.empty() && !m_mAgg.empty())
	{
		WriteMatch();
		LoadExclusions(); // re-read fresh so Claude's edits apply without re-injecting
		UpdateRollup();
		UpdateManifest();
	}

	ResetMatch();
	m_bMatchActive = false;
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

void CAutoVprof::ApplyInterval(const std::vector<VprofNode_t>& vNodes, long long iFrames, bool bSpike, double flWorstMs, double flAvgMs)
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

	if (bSpike && static_cast<int>(m_vSpikes.size()) < MAX_SNAPSHOTS)
	{
		VprofSpike_t tS;
		tS.m_flWorstMs = flWorstMs;
		tS.m_flAvgMs = flAvgMs;
		tS.m_vTopNodes = vNodes;
		std::sort(tS.m_vTopNodes.begin(), tS.m_vTopNodes.end(),
			[](const VprofNode_t& a, const VprofNode_t& b) { return a.SelfMs() > b.SelfMs(); });
		if (static_cast<int>(tS.m_vTopNodes.size()) > SNAPSHOT_TOP)
			tS.m_vTopNodes.resize(SNAPSHOT_TOP);
		m_vSpikes.push_back(std::move(tS));
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
		f << std::setprecision(3);
		for (const auto& r : vRows)
			f << r.selfPct << " " << r.selfMs << " " << r.inclMs << " " << r.calls << " " << r.name << "\n";

		f.precision(1);
		f << "#spikes count=" << m_vSpikes.size() << "\n";
		for (const auto& s : m_vSpikes)
		{
			f << "worst=" << s.m_flWorstMs << " avg=" << s.m_flAvgMs << " nodes=";
			for (size_t i = 0; i < s.m_vTopNodes.size(); i++)
			{
				if (i) f << ",";
				f << s.m_vTopNodes[i].m_sName << ":" << std::setprecision(3) << s.m_vTopNodes[i].SelfMs() << std::setprecision(1);
			}
			f << "\n";
		}
	}
	catch (...) {}
}

// ---- per-map rolling rollup ---------------------------------------------------
void CAutoVprof::UpdateRollup()
{
	struct Node { double selfPct; double selfMs; std::string name; };
	struct Rec { std::string build; double fps1 = 0, fps5 = 0, fpsAvg = 0; std::vector<Node> nodes; std::vector<std::string> spikeNodes; std::string fname; };

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
			int iSect = 0; // 0 header, 1 nodes, 2 spikes
			while (std::getline(f, sLine))
			{
				while (!sLine.empty() && sLine.back() == '\r')
					sLine.pop_back();
				if (sLine.rfind("#nodes", 0) == 0) { iSect = 1; continue; }
				if (sLine.rfind("#spikes", 0) == 0) { iSect = 2; continue; }

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
				else if (iSect == 2)
				{
					auto p = sLine.find("nodes=");
					if (p != std::string::npos)
					{
						std::string list = sLine.substr(p + 6);
						std::stringstream ls(list);
						std::string item;
						while (std::getline(ls, item, ','))
						{
							auto c = item.find(':');
							std::string name = c == std::string::npos ? item : item.substr(0, c);
							if (!name.empty())
								r.spikeNodes.push_back(name);
						}
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
	std::unordered_map<std::string, int> mSpike;
	for (auto* r : vUsed)
	{
		for (const auto& n : r->nodes)
		{
			auto& a = mAgg[n.name];
			a.first += n.selfPct;
			a.second += n.selfMs;
		}
		std::unordered_set<std::string> seen;
		for (const auto& s : r->spikeNodes)
			if (seen.insert(s).second)
				mSpike[s]++;
	}

	struct ARow { double selfPct; double selfMs; std::string name; };
	std::vector<ARow> vRows;
	int iUsed = static_cast<int>(vUsed.size());
	for (const auto& [name, a] : mAgg)
		vRows.push_back({ a.first / iUsed, a.second / iUsed, name });
	std::sort(vRows.begin(), vRows.end(), [](const ARow& a, const ARow& b) { return a.selfMs > b.selfMs; });
	if (static_cast<int>(vRows.size()) > TOP_N)
		vRows.resize(TOP_N);

	std::vector<std::pair<std::string, int>> vSpk(mSpike.begin(), mSpike.end());
	std::sort(vSpk.begin(), vSpk.end(), [](auto& a, auto& b) { return a.second > b.second; });

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
		f << std::setprecision(3);
		for (const auto& r : vRows)
			f << r.selfPct << " " << r.selfMs << " " << r.name << "\n";
		f.precision(1);
		f << "#recurring_spikes\n";
		for (const auto& [name, cnt] : vSpk)
			f << name << "(" << cnt << "/" << iUsed << ") ";
		f << "\n";
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
		if (m_bMatchActive)
		{
			DisableVprof();
			if (auto pCVar = I::CVar->FindVar("con_logfile"))
				pCVar->SetValue("");
			ResetMatch();
			m_bMatchActive = false;
		}
		return;
	}

	EnsurePaths();

	double flNow = SDK::PlatFloatTime();
	double flDtMs = m_flLastFrame > 0.0 ? (flNow - m_flLastFrame) * 1000.0 : 0.0;
	m_flLastFrame = flNow;
	if (flDtMs < 0.0 || flDtMs > 1000.0)
		flDtMs = 0.0; // ignore load hitches / alt-tab
	if (flDtMs > 0.0)
	{
		if (!m_bEMASeeded) { m_flEMAms = flDtMs; m_bEMASeeded = true; }
		else m_flEMAms = m_flEMAms * 0.9 + flDtMs * 0.1;
	}

	// Match-end finalize is handled synchronously in Event() (disconnect stops in-game
	// painting, so Run() can't be relied on to flush).
	if (!I::EngineClient->IsInGame())
		return;

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
			if (flDtMs > m_flWindowWorstMs)
				m_flWindowWorstMs = flDtMs;
			if (m_bEMASeeded && flDtMs > SPIKE_FACTOR * m_flEMAms && flDtMs > SPIKE_FLOOR_MS)
				m_bWindowSpike = true;
		}
		if (flNow >= m_flNextDumpTime && m_iIntervalFrames > 0)
			RequestDump(EAfter::Continue);
	}
	else if (m_ePhase == EPhase::WaitDump)
	{
		PollDump();
	}
}
