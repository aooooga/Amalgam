---
name: add-feature
description: Codebase knowledge base + recipe for building or modifying any Amalgam feature (visuals, HUD, aimbot, misc, hooks). Use at the START of every feature/change task instead of exploring. References hold the deep maps — read only the ones the task needs.
---

# Amalgam feature work — start here

This skill replaces codebase exploration. Read the reference file(s) matching your task; do NOT glob/grep for structure they already document.

| Need | Read |
|---|---|
| Where anything lives; hooks index; feature inventory; Vars.h/menu layout | `references/codebase-map.md` |
| API signatures: `SDK::`, `H::Draw`, `H::Entities`, `I::`, macros, netvars, types, ConfigVar/bind semantics | `references/sdk-api.md` |
| End-to-end recipes, FlexFOV coexistence rules, perf/crash landmines | `references/patterns.md` |

## Core checklist (every new feature)

1. Feature class `Features/<Area>/<Name>/<Name>.h+.cpp`, header ends `ADD_FEATURE(C<Name>, <Name>)` → `F::<Name>`. Tiny additions: add a method to an existing aggregator (`Visuals.cpp`, `Misc.cpp`) instead — skips steps 2.
2. Register new files in BOTH `Amalgam/Amalgam.vcxproj` AND `Amalgam.vcxproj.filters` (unregistered files silently don't build).
3. Vars in `SDK/Vars.h` inside the right `NAMESPACE_BEGIN` block; colors always in `Vars::Colors`, never hardcoded.
4. Menu section in `Features/ImGui/Menu/Menu.cpp` (grep a neighboring `Section("` name to find the tab).
5. Call site: pick hook from the table in `codebase-map.md`; `#include` the feature header there. Draw order vs FlexFOV matters — see `patterns.md`.
6. Build with the `/build-dll` skill. No tests; the user verifies in-game.

Reference implementations, smallest→largest: `Visuals/CritBar` (HUD element), `DrawStickyRadius` in `Visuals.cpp` (aggregator method), `Visuals/SentryRange` (world-space geometry + caching), `Aimbot/DoubleSticky` (CreateMove logic), `Visuals/RearView` → `Visuals/FlexFOV` (render-target pipelines).
