# Recipes, coexistence rules, landmines

## Recipe: new visual feature (the common case)

Model: `Visuals/CritBar` (~110-line HUD bar) or `Visuals/SentryRange` (world geometry).
1. `Features/Visuals/<Name>/<Name>.h`:
```cpp
#pragma once
#include "../../../SDK/SDK.h"
class C<Name> { public: void Draw(CTFPlayer* pLocal); };
ADD_FEATURE(C<Name>, <Name>)
```
2. `.cpp`: first line of Draw: `if (!Vars::Visuals::<Name>::Enabled.Value || !I::EngineClient->IsInGame()) return;`
3. Both vcxproj files; Vars.h block; Menu section; call from the hook per the map table.

## Recipe: per-tick (aimbot/movement) feature

Model: `Aimbot/DoubleSticky`. Logic runs from `CHLClient_CreateMove.cpp` (has `CUserCmd* pCmd`, `pLocal`, weapon); optional `Draw()` from a draw hook reading state cached at tick time. Cache expensive per-tick results; CreateMove runs at tick rate, draw hooks at frame rate — never trace/simulate in the draw path if the tick path already did.

## FlexFOV coexistence (MANDATORY for any visual)

FlexFOV replaces the screen with a composite built from captured face renders. Two hook files encode the rules in comments — keep them true:
- **Screen-space (Paint)**: `F::FlexFOV.DrawComposite()` repaints every pixel. Anything drawn in `IEngineVGui_Paint.cpp` must be called AFTER it (and after `DrawViewmodel()`/`DrawDebug()`); `SDK::W2S` automatically reprojects, so positions stay correct.
- **World-space (DoPostScreenSpaceEffects)**: three pass kinds hit this hook per frame:
  - face-capture passes (`F::FlexFOV.m_bDrawing`): draw here if the visual should survive into the composite (chams/glow do);
  - the scene-stripped cheap main pass (`F::FlexFOV.m_bReplacingView`): composite paints over it — SKIP all drawing (pure waste);
  - normal pass (FlexFOV off): draw normally.
  World-geometry features (sentry range, sticky radius, trajectories) draw before the early-returns so they render into the faces; they must self-skip during `m_bReplacingView` after refreshing per-frame caches.
- Also gate on `SDK::CleanScreenshot()`, `G::Unload`, `I::EngineVGui->IsGameUIVisible()` as neighbors do.

## Landmines

- **Render primitive volume**: engine `RenderLine/RenderTriangle/RenderBox` crash at roughly 10k+ calls in one pass. Merge geometry, cull (e.g. `FaceCanSee`), hard-cap counts. (SentryRange history.)
- **vcxproj**: forgetting `.filters` or `.vcxproj` = silent omission from build → unresolved externals or dead code.
- **`Vars::X.Value` vs `X[DEFAULT_BIND]`**: menu widgets take the var object; `[DEFAULT_BIND]` reads the un-bound base (used for "off" checks in slider format strings).
- **Materials**: chams-style rendering needs the material registered in `Features/Visuals/Materials/Materials.cpp`; check `F::Materials.m_bLoaded` before use.
- **Injected-DLL rebuild**: if `ReleaseAVX2` DLL is currently injected, the build fails to overwrite — wait for unload (build-dll skill handles this).
- **Signatures break on TF2 updates** — if a `S::Name()` returns null after an update, the pattern needs re-scanning, not code changes.
- **Never modify `Amalgam/include/`** (vendored ImGui/MinHook/FreeType).
- **Perf culture**: this codebase is aggressively optimized (attribute cache, InCond fast path, retained meshes). Don't add per-frame allocations, per-call trig, or ungated bone setups; snapshot/latch per-frame values like FlexFOV's W2S snapshot does.

## Committing

Direct to `master`, no branches. DLLs stay in `output/`, never committed. Commit messages end with the Co-Authored-By line per global rules; use `git commit -F <file>` (PowerShell mangles multiline `-m`).
