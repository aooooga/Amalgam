# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Amalgam is a TF2 (Team Fortress 2) DLL cheat written in C++20, targeting `tf_win64.exe` (64-bit). It must be externally injected — this repo only produces the payload DLL.

This is a **fork** (`origin` = `github.com/aooooga/Amalgam`, `upstream` = `github.com/rei-2/Amalgam`).

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
Always use the `build-dll` skill (`/build-dll`) to run this end-to-end and get the resulting DLL path back.

Note: If the build is attempted and the ReleaseAVX2 dll is already injected, have it wait until it is unloaded to continue.

## Testing

No automated tests. To test: build the DLL, then inject it into a running TF2 instance with an external injector and observe behavior in-game.

## Performance analysis

Amalgam has a built-in **Auto vprof** tool (`F::AutoVprof`, Debug → Auto vprof) that captures TF2's `vprof` profiler across live rounds and writes cleaned, LLM-facing reports to `Team Fortress 2/Amalgam/vprof/` (`matches/<map>_<ts>.txt`, per-map `<map>.txt` rollups, `builds.txt` inventory). When asked to analyze or optimize runtime performance, read `.claude/skills/add-feature/references/performance.md` — it covers the report formats, how to read the numbers, and the **Claude-curated build-exclusion workflow** (the user has no in-game control over exclusions; Claude edits `excluded_builds.txt` to drop stale builds from rollups).

## Runtime behavior

- **Process check:** On load, the DLL hashes the host process name (FNV1A) and immediately self-unloads if it isn't `tf_win64.exe`.
- **Load window:** After the process check passes, the DLL polls for `client.dll` readiness for up to 60 seconds. Press `F11` during this window to cancel loading.
- **Unload hotkey:** Press `F11` while TF2 is the focused window to cleanly unload the DLL at any time.
- **Config path:** Config files are stored in `Program Files (x86)/Steam/steamapps/common/Team Fortress 2/Amalgam`; failures are logged to `fail_log.txt` there.

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

**Before building or modifying any feature, invoke the `add-feature` skill** (`.claude/skills/add-feature/SKILL.md`) — it contains the full file checklist, hook call-site table, and verified API signatures (`H::Draw`, `H::Entities`, `SDK::W2S`, traces), so no codebase exploration is needed to start.

Features are declared with the `ADD_FEATURE(ClassName, Name)` macro (`Amalgam/src/Utils/Macros/Macros.h`) at the bottom of the header — this registers `F::Name` as a file-scope inline instance in `namespace F`. See `Aimbot.h` / `Visuals.h` / `FlexFOV.h` for the pattern.

Config variables are declared in `Amalgam/src/SDK/Vars.h` using the `CVar` / `CVarEnum` / `CVarValues` macros inside `NAMESPACE_BEGIN` / `NAMESPACE_END` blocks. The variable is then accessible as `Vars::SectionName::SubSection::VarName`.

Feature methods (`Run`, `Draw`, `Store`, `Tick`, etc.) are called from the relevant hooks in `Amalgam/src/Hooks/` (flat `.cpp` files named `<ClassName>_<MethodName>.cpp`, e.g. `CViewRender_RenderView.cpp`) or from parent feature aggregators.

### Vendored libraries

Libraries in `Amalgam/include/` (ImGui, MinHook, FreeType) are vendored — do not modify them. The Boost 1.87.0 NuGet package is managed via `packages.config`.

### Signatures

Byte signatures in `Utils/Signatures/` target a specific TF2 build. They will break on TF2 updates and must be manually re-scanned. Use `MAKE_SIGNATURE(Name, "module.dll", "pattern", offset)` to declare a new one.

## Discord requests

If a request arrives tagged `<channel source="plugin:discord:discord" ...>`, read `.claude/skills/discord.md` in full before responding or acting — it covers the mandatory Discord-reply behavior, build/commit/push conventions, and reaction conventions for this channel.

## Repo / collaboration workflow

- **Never create branches.** All work is committed directly to `master` — no feature branches, no PRs.
- Both the repo owner (`.aooga`, GitHub `aooooga`) and the contributor (`honaaaa`) are trusted to commit and push directly to `master`, including via Discord requests.
- Built DLLs should stay in `output/x64/<Configuration>/` and be shared directly rather than committed.
