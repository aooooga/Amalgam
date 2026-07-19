# Performance analysis — Auto vprof tool

Amalgam ships a **debug feature that auto-captures TF2's `vprof` profiler** and writes
token-minimal, LLM-facing performance reports. Read this when asked to analyze/optimize
Amalgam runtime performance, or to reason about the vprof reports it produces.

## What it is
`F::AutoVprof` (`Features/Debug/AutoVprof/`), toggle: **Debug → Auto vprof**. While enabled
it enables vprof once per match, dumps + resets a report every ~1s during live rounds (via
`con_logfile` capture), and on match end writes a cleaned per-match report + updates a
per-map rolling rollup. See [[vprof-deferred-commands]] for the engine constraint it works
around (vprof_* are deferred, one-per-frame).

## Output files — `Team Fortress 2/Amalgam/vprof/`
- `matches/<map>_<YYYYMMDD-HHMMSS>.txt` — one per match. Header (map, mode, slots, duration,
  frames, fps avg/1%low/5%low, `build=`), then top-30 nodes ranked by **self ms/frame**
  (pruned < 0.1% self): `self%  selfms  inclms  calls/f  name`; then a `#heavy` section —
  the same self-ms ranking but computed **only over the heaviest ~5% of frames** (the match's
  laggy moments; `selfms name`, top 15) — compare a node's heavy selfms vs its overall selfms
  to see what disproportionately drives the lows; then up to 3 spike snapshots
  (`worst=/avg= ms`, top self-ms nodes).
- `<map>.txt` — rolling rollup over the **last 5 non-excluded matches** on that map: fps
  trend + nodes averaged (`self%  selfms  name`) + `#heavy` averaged 5%-low nodes +
  recurring spike contributors.
- `builds.txt` — **auto-generated build inventory** (do not hand-edit): every build id that
  has recorded matches, with match count, maps, date range, and excluded status.
- `excluded_builds.txt` — **curated by Claude** (see below).

## Reading the numbers
- `self%` = share of the **main-thread CPU time vprof accounts for** (self, excl. children).
  This is where CPU actually burns — the real optimization target (DrawModel, SetupBones…).
- `selfms` = **ms/frame** self cost (derived from the engine's `Avg/Frame` incl-ms column).
  Ranking is by this. `self%` looks larger than `selfms/frametime` because its denominator is
  vprof-accounted CPU (~half of wall frame time; the rest is GPU wait / other threads).
- `inclms` = ms/frame including children. Build id = the DLL's link timestamp (`YYYYMMDD-HHMM`).

## Build exclusion is Claude's job (not the user's)
The user has **no in-game control** over exclusions by design — curating which builds are
still perf-relevant is an analysis task for Claude. Workflow when analyzing performance:
1. Read `builds.txt` for the inventory of builds with data.
2. Correlate build ids (link timestamps) with `git log --format='%h %cd %s'` to see what each
   build contained; decide which are **stale** (superseded by a perf-affecting change) vs still
   representative (a perf-neutral build keeps aggregating with its neighbours — don't exclude it).
3. Edit `excluded_builds.txt` (Edit/Write tools) to drop stale builds from rollups. Two forms:
   - `20260717-1440   # reason` — exclude just that build.
   - `before 20260718-2000   # reason` — exclude every build older than that id.
   `#` starts a comment; the tool re-reads the file at each match end, so edits apply to the
   next rollup without re-injecting. The tool only ever **reads** this file.
