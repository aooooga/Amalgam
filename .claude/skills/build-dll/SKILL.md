---
name: build-dll
description: Build the Amalgam TF2 cheat DLL via nuget restore + msbuild and report the resulting output DLL path. Use when asked to build, compile, or produce a fresh DLL (e.g. to share in Discord), or to verify the repo still builds after a change.
---

Build the Amalgam DLL for this repo (`C:\Users\Honaaa\Desktop\Amalgam`).

1. Determine the configuration to build. Default to `Release` unless the user specifies one of: `Release`, `ReleaseAVX2`, `ReleaseFreetype`, `ReleaseFreetypeAVX2`, `Debug`, `DebugAVX2`, `DebugFreetype`, `DebugFreetypeAVX2`. Platform is always `x64`.

2. Locate `nuget.exe`. It is not on PATH on this machine. Check `C:\temp\nuget-tool\nuget.exe` first; if missing, download it:
   ```
   curl -sL -o C:\temp\nuget-tool\nuget.exe https://dist.nuget.org/win-x86-commandline/latest/nuget.exe
   ```

3. Restore packages (only needs to happen once, but cheap to re-run):
   ```
   C:\temp\nuget-tool\nuget.exe restore Amalgam.sln
   ```
   (run from the repo root)

4. Build with the 2022 BuildTools MSBuild explicitly by full path — do NOT let PATH resolve a different MSBuild (there's also a VS "18" install on this machine whose MSBuild lacks the v143 C++ toolset and will fail with a `VCTargetsPath` error for `v180`):
   ```
   "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" Amalgam.sln /p:Platform=x64 /p:Configuration=<Configuration> /m
   ```

5. On success, report the exact output path: `output/x64/<Configuration>/Amalgam.dll` (and the sibling `.pdb` if present). This is the file to attach when sharing a build in Discord.

6. On failure, surface the actual MSBuild error output (don't just say "build failed") — most likely causes are a stale NuGet restore or a missing/renamed source file after a merge.

Do not commit built DLLs into the repo — the `build_v2`...`build_v10` folders are a legacy convention that is no longer used (see root `CLAUDE.md`). Built artifacts stay in `output/` and are shared directly (e.g. as a Discord attachment).
