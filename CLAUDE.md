# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Amalgam is a TF2 (Team Fortress 2) DLL cheat written in C++20, targeting `tf_win64.exe` (64-bit). It must be externally injected — this repo only produces the payload DLL.

This is a **fork** (`origin` = `github.com/aooooga/Amalgam`, `upstream` = `github.com/rei-2/Amalgam`). The only meaningful divergence from upstream is the **FlexFOV** feature (see below), which enables wide-angle FOV up to 360°.

## Build

**Prerequisite:** NuGet must be installed (`nuget` on PATH) and run before the first build:
```
nuget restore
msbuild Amalgam.sln /p:Platform=x64 /p:Configuration=Release
```

Output lands in `output/x64/<Configuration>/Amalgam.dll`.

**Configurations** (`/p:Configuration=<name>`):
- `ReleaseAVX2` / `Release` / `ReleaseFreetype` / `ReleaseFreetypeAVX2`
- `Debug` / `DebugAVX2` / `DebugFreetype` / `DebugFreetypeAVX2`

AVX2 variants use AVX2 intrinsics (faster, not universally compatible). Freetype variants enable `IMGUI_ENABLE_FREETYPE` and `AMALGAM_CUSTOM_FONTS` and produce a larger DLL with better text rendering.

Alternatively: open `Amalgam.sln` in Visual Studio 2022 and build from the IDE.

**On this machine, `nuget`/`msbuild` are not on PATH.** Only VS Build Tools 2022 is installed (no full VS IDE with the C++ workload on PATH either). Use full paths:
```
# One-time: download nuget.exe (any writable folder), e.g.:
curl -sL -o C:\temp\nuget-tool\nuget.exe https://dist.nuget.org/win-x86-commandline/latest/nuget.exe

# Restore (from repo root):
C:\temp\nuget-tool\nuget.exe restore Amalgam.sln

# Build:
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Amalgam.sln /p:Platform=x64 /p:Configuration=Release /m
```
Note: there's also a newer VS install under `C:\Program Files\Microsoft Visual Studio\18\...` — its MSBuild auto-detects first but lacks the v143 C++ toolset (`VCTargetsPath` for `v180` doesn't exist), so builds with it fail. Always invoke the 2022 BuildTools MSBuild explicitly by full path as above.

Use the `build-dll` skill (`/build-dll`) to run this end-to-end and get the resulting DLL path back.

## Testing

No automated tests. To test: build the DLL, then inject it into a running TF2 instance with an external injector and observe behavior in-game.

## Runtime behavior

- **Process check:** On load, the DLL hashes the host process name (FNV1A) and immediately self-unloads if it isn't `tf_win64.exe`.
- **Load window:** After the process check passes, the DLL polls for `client.dll` readiness for up to 60 seconds. Press `F11` during this window to cancel loading.
- **Unload hotkey:** Hold `F11` while TF2 is the focused window to cleanly unload the DLL at any time.
- **Config path:** Config files are stored at a runtime path (likely `%APPDATA%\Amalgam\`); failures are logged to `fail_log.txt` there.

## Architecture

### Namespaces
| Namespace | Contents |
|-----------|----------|
| `F::` | Feature instances (aimbot, ESP, chams, etc.) |
| `U::` | Utilities (Memory, Signatures, Interfaces, Hooks, KeyHandler) |
| `H::` | SDK helpers (Entities, ConVars, Draw, Fonts, Events) |
| `I::` | Engine/game interface pointers (EngineClient, MatSystemSurface, etc.) |
| `G::` | Global game state (current CUserCmd, aim targets, draw queues, etc.) |
| `Vars::` | All `ConfigVar<T>` declarations |

### Adding a feature

Features are declared with the `ADD_FEATURE(ClassName, Name)` macro (`Amalgam/src/Utils/Macros/Macros.h`) at the bottom of the header — this registers `F::Name` as a file-scope inline instance in `namespace F`. See `Aimbot.h` / `Visuals.h` / `FlexFOV.h` for the pattern.

Config variables are declared in `Amalgam/src/SDK/Vars.h` using the `CVar` / `CVarEnum` / `CVarValues` macros inside `NAMESPACE_BEGIN` / `NAMESPACE_END` blocks. The variable is then accessible as `Vars::SectionName::SubSection::VarName`.

Feature methods (`Run`, `Draw`, `Store`, `Tick`, etc.) are called from the relevant hooks in `Amalgam/src/Hooks/` (flat `.cpp` files named `<ClassName>_<MethodName>.cpp`, e.g. `CViewRender_RenderView.cpp`) or from parent feature aggregators.

### Vendored libraries

Libraries in `Amalgam/include/` (ImGui, MinHook, FreeType) are vendored — do not modify them. The Boost 1.87.0 NuGet package is managed via `packages.config`.

### Signatures

Byte signatures in `Utils/Signatures/` target a specific TF2 build. They will break on TF2 updates and must be manually re-scanned. Use `MAKE_SIGNATURE(Name, "module.dll", "pattern", offset)` to declare a new one.

### FlexFOV (`Features/Visuals/FlexFOV/`)

The fork's headline feature: wide-angle FOV up to 360° via view reprojection. Registered as `F::FlexFOV` (`ADD_FEATURE(CFlexFOV, FlexFOV)`).

TF2's engine renders through one linear perspective projection matrix, which distorts badly and eventually breaks down as FOV approaches/exceeds ~170°. Instead of asking the engine for one giant-FOV frame, FlexFOV captures several narrower-FOV "faces" of the scene each frame and reprojects/stitches them into a single wide composite using non-linear Panini/Mercator inverse projection math (ported from `shaunlebron/flex-fov`).

Pipeline, in order:
1. **Capture** (`CaptureGlobe()`, called from the `CViewRender_RenderView` hook after the original render): re-renders the scene from the player's eye into view-aligned face render targets (`EFace`: FRONT/BACK/LEFT/RIGHT/UP/DOWN, each 130° FOV). `ComputeRig()` picks a **cube rig** (up to 6 faces) for very wide FOV, or a cheaper **wide rig** (2 tall faces, yawed ± around view-up) for FOV ≤ ~245° on widescreen. Only faces actually needed by the current composite mesh are rendered — this is the main perf lever.
2. **Cheap main-view pass** (`BeginCheapMainView()`/`EndCheapMainView()`): the original `CALL_ORIGINAL` main-view render still runs (needed for HUD paint), but scene-draw cvars (world/entities/skybox/viewmodel/particles) are zeroed out first so it costs almost nothing.
3. **Composite** (`DrawComposite()`, called from the `IEngineVGui_Paint` hook): builds the on-screen warp by sampling captured face textures through the inverse (`ScreenToRay`) reprojection. The tier structure matches the original flex-fov: rectilinear at 90° FOV easing into Panini by the transition-band start, easing into Mercator across the band (player-tunable via `FlexFOVTransition`, default 160–300 like the original), with a `1-(1-t)²` ease at every boundary. The vertical-stereographic mode blends toward a radial ("round") Panini — exactly stereographic at strength 1 — scaled by the same FOV-tier envelope so it fades out as Mercator takes over at high FOV. `DrawDebug()` blits the raw face thumbnails instead, for verification.
4. **Viewmodel**: `DrawViewmodel()` redraws the first-person viewmodel at native (unwarped) projection on top of the composite, since the capture faces would badly warp its geometry.
5. **WorldToScreen tie-in**: `CFlexFOV::WorldToScreen()` is the exact inverse of `DrawComposite`'s `ScreenToRay`. `SDK::W2S()` checks `F::FlexFOV.m_bComposite` and routes through it instead of the normal linear matrix when the composite is active — this is what keeps ESP/overlays aligned on the reprojected view. A per-frame snapshot of eye origin/view angles/FOV/aspect/strength is latched once in `CaptureGlobe`/`DrawComposite` so per-call `WorldToScreen` invocations (hundreds/frame for overlays) never redo trig.

Key cvars (`Vars::Visuals::UI`, `Amalgam/src/SDK/Vars.h`): `FlexFOVDebug`, `FlexFOVComposite`, `FlexFOVStrength` (Panini `d`, 0–2, also shapes the radial/stereographic modes), `FlexFOVTransition` (Panini→Mercator band, `FloatRange_t`), `FlexFOVStereographic` / `FlexFOVVertStereo`, `FlexFOVSkipMainView`, `FlexFOVQuality` (face RT resolution scale). See the comments atop `FlexFOV.h`/`FlexFOV.cpp` for the full projection math and rig-selection details.

## Discord requests

If a request arrives tagged `<channel source="plugin:discord:discord" ...>`, read `.claude/skills/discord.md` in full before responding or acting — it covers the mandatory Discord-reply behavior, build/commit/push conventions, and reaction conventions for this channel.

## Repo / collaboration workflow

- **Never create branches.** All work is committed directly to `master` — no feature branches, no PRs.
- Both the repo owner (`.aooga`, GitHub `aooooga`) and the contributor (`honaaaa`) are trusted to commit and push directly to `master`, including via Discord requests.
- The `build_v2` … `build_v10` folders (each holding a single tracked `Amalgamx64Release.dll`) are a **legacy** manual build-versioning convention — do not add new `build_vN` folders. Built DLLs should stay in `output/x64/<Configuration>/` and be shared directly (e.g. attached in Discord) rather than committed.
