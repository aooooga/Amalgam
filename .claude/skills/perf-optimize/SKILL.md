---
name: perf-optimize
description: Data-driven Amalgam performance optimization workflow guided by Auto vprof reports. Use whenever asked to analyze runtime performance, find or fix FPS drops / frame-time cost, optimize a feature's perf, verify a past optimization landed, or interpret files in Team Fortress 2/Amalgam/vprof/. Reports are the ground truth — never optimize on intuition alone.
---

# Perf optimization via Auto vprof reports

The vprof reports are the guideline: every optimization starts by reading them, targets a node
they rank, and ends with a plan to verify the win in a later capture. Do not micro-optimize
code the reports don't implicate.

**First**, read `.claude/skills/add-feature/references/performance.md` — it defines the report
formats (`matches/*.txt`, per-map rollups, `builds.txt`), what `self%`/`selfms`/`inclms` mean,
and the build-exclusion file syntax. Reports live in
`Program Files (x86)/Steam/steamapps/common/Team Fortress 2/Amalgam/vprof/`.

## Workflow

1. **Curate builds before trusting rollups.**
   Read `builds.txt`, correlate build ids (link timestamps, `YYYYMMDD-HHMM`) with
   `git log --format='%h %cd %s'`, and update `excluded_builds.txt` so rollups only average
   builds that are perf-equivalent to HEAD. Perf-neutral builds stay included; builds
   superseded by a perf-affecting change get excluded (usually via one `before <id> # reason`
   line). This is Claude's job — the user has no control over it.

2. **Establish the current picture.**
   Read the per-map rollups for the maps with recent data, plus the newest 1–2 match reports
   per map. Note: fps avg / 1% low trend, top nodes by **selfms** (that's the ranking that
   matters), and how the `#heavy` (5%-low) ranking differs from the overall one — that
   difference is what drives the lows. Check memory (`MEMORY.md` perf entries) for
   prior baselines and the standing optimization backlog before re-deriving conclusions.

3. **Pick targets by expected ms saved, not by how easy the code looks.**
   - Amalgam-owned nodes (hooks, feature code) are directly fixable.
   - Engine nodes (DrawModel, SetupBones, traces) are fixable only by making Amalgam call
     them less: cull, cache, gate by distance/visibility/game-state, reduce per-frame work.
   - A node whose `#heavy` selfms far exceeds its overall selfms matters for the lows even
     when its average is modest.
   - Skip anything under ~0.05 selfms unless several such wins stack.

4. **Check whether cost is user-config-dependent.** The user plays aimbot-off /
   visuals-heavy; a node's cost in reports reflects their settings. Optimizations should help
   that profile first, and never regress correctness (aimbot/vis logic) for speed.

5. **Implement, then build** (`/build-dll`). Typical patterns that have already paid off
   here: per-camera / distance culls, caching across frames, gating stores to when a consumer
   is active, avoiding per-frame allocations. Check the micro-optimization backlog memory for
   known stacking wins and their traps.

6. **Close the loop.**
   - Record in memory: what changed, the build id it shipped in, the baseline numbers it
     should beat, and mark it *unverified*.
   - Exclude the superseded builds in `excluded_builds.txt` so the next rollup measures the
     new build cleanly.
   - The verification itself happens next session: compare the new build's rollup selfms for
     the targeted nodes (and fps/5% lows) against the recorded baseline, then update the
     memory to verified/regressed.

## Rules of thumb

- One optimization round per capture cycle when possible — stacking many changes into one
  build makes per-change attribution impossible.
- `self%` denominators differ between matches; compare **selfms** across builds, not self%.
- A node vanishing from the top-40 is a win; a node's calls/f dropping with flat selfms means
  the per-call cost went up — investigate before celebrating.
- Never issue two `vprof_*` console commands in one frame (engine defers them into one slot).
- Use 5% lows as your most important baseline — Those are the heavy moments in the match the 
  player will feel bad the performance the most, and will need good performance greatly.