---
name: build-dll
description: Build the Amalgam TF2 cheat DLL via nuget restore + msbuild and report the resulting output DLL path. Use when asked to build, compile, or produce a fresh DLL (e.g. to share in Discord), or to verify the repo still builds after a change.
---

Build the Amalgam DLL for this repo (repo root — the folder containing `Amalgam.sln`).

1. Determine the configuration to build. `ReleaseAVX2` shall be the used setting, unless the user specifies one of: `Release`, `ReleaseFreetype`, `ReleaseFreetypeAVX2`, `Debug`, `DebugAVX2`, `DebugFreetype`, `DebugFreetypeAVX2`. Platform is always `x64`.

2. Locate `nuget.exe`. Check if it's on PATH first (`nuget`). If not, check for a previously-downloaded copy in a local tools/temp folder; if none exists, download one:
   ```
   curl -sL -o <local-tools-dir>/nuget.exe https://dist.nuget.org/win-x86-commandline/latest/nuget.exe
   ```

3. Restore packages (only needs to happen once, but cheap to re-run), from the repo root:
   ```
   <path-to-nuget>/nuget.exe restore Amalgam.sln
   ```

4. Build with a Visual Studio 2022 (or newer, with the v143+ C++ toolset) MSBuild. If `msbuild` isn't on PATH, locate one explicitly — on some machines a different/older VS install's MSBuild resolves first and lacks the right C++ toolset, causing a `VCTargetsPath` error, so don't assume the first MSBuild found on PATH is correct if the build fails that way:
   ```
   msbuild Amalgam.sln /p:Platform=x64 /p:Configuration=<Configuration> /m
   ```

5. On success, report the exact output path: `output/x64/<Configuration>/Amalgam.dll` (and the sibling `.pdb` if present). This is the file to attach when sharing a build.

6. On failure, surface the actual MSBuild error output (don't just say "build failed") — most likely causes are a stale NuGet restore or a missing/renamed source file after a merge.

If the build is attempted but the DLL is already injected, have it wait until it is unloaded to continue building.