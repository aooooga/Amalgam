# Amalgam codebase map

All paths relative to `Amalgam/src/`.

## Top level

| Path | Purpose |
|---|---|
| `DllMain.cpp` | Injection entry: process-name check (tf_win64.exe), 60s client.dll poll, F11 unload |
| `Core/` | Init/unload orchestration |
| `BytePatches/` | Raw byte patches applied at load |
| `SDK/` | Game classes, interfaces, helpers, Vars.h |
| `Hooks/` | One flat `.cpp` per hooked function: `<Class>_<Method>.cpp` |
| `Features/` | All cheat features (instances in `F::`) |
| `Utils/` | Memory, Signatures, Hooks(MinHook wrapper), Interfaces, NetVars, Math, Hash(FNV1A), KeyHandler, Timer, ExceptionHandler |

## SDK/

- `SDK.h/.cpp` — `namespace SDK` utility functions (see sdk-api.md)
- `Vars.h` — ConfigVar template + ALL config vars (namespace index below)
- `Globals.h` — `G::` state: `G::Unload`, draw queues (`DrawLine_t/DrawPath_t/DrawBox_t/DrawSphere_t/DrawSwept_t`), aim targets, current cmd
- `Definitions/Main/` — entity classes: `CBaseEntity` → `CBaseAnimating` → `CBaseCombatCharacter` → `CTFPlayer`; `CTFWeaponBase`, `CBaseObject` (buildings, incl. sentry), `CBaseProjectile`, `CTFPlayerResource`, `CUserCmd`, `CGameTrace`, `CCollisionProperty`, `CEconEntity`
- `Definitions/Interfaces/` — one header per engine interface (declared with `MAKE_INTERFACE_*` macros → pointer in `I::`)
- `Definitions/Types.h` — `Vec3`(:342), `IntRange_t`, `FloatRange_t`, `Color_t`(:913), `Gradient_t`, `Chams_t`, `Glow_t`, `DragBox_t`, `WindowBox_t` (line nums approximate)
- `Helpers/` — `H::Draw` (Draw/), `H::Entities` (Entities/), `H::ConVars`, `H::Fonts`, `H::Particles`, TraceFilters/
- `Events/` — game event listener (`H::Events`)

## Hooks/ — where to call feature code

| You want | Hook file | Notes |
|---|---|---|
| 2D HUD / screen-space draw | `IEngineVGui_Paint.cpp` | Inside `H::Draw.Start(true)`..`End()`, has `pLocal`. FlexFOV composite repaints whole screen FIRST — add calls after `F::FlexFOV.DrawComposite()` |
| World-space geometry, chams/glow/material passes | `CClientModeShared_DoPostScreenSpaceEffects.cpp` | Runs once per render pass INCLUDING FlexFOV face captures and the scene-stripped cheap pass — see patterns.md |
| Per-tick logic w/ CUserCmd (aim, movement, crit) | `CHLClient_CreateMove.cpp` | Order: Backtrack → Misc.RunPre → Ticks.Start → Aimbot.Run → CritHack → NoSpread → Misc.RunPost → PacketManip → AntiAim → EnginePrediction.End |
| Per-net-update caching (`Store()`) | `CHLClient_FrameStageNotify.cpp` | `H::Entities.Store` then PlayerUtils/Backtrack/CritHack/Aimbot/Groups/ESP/Chams/Glow `.Store()` |
| Model draw / material override | `IVModelRender_DrawModelExecute.cpp`, `IVModelRender_ForcedMaterialOverride.cpp` | Chams internals |
| View/camera | `CViewRender_RenderView.cpp` (FlexFOV capture, RearView), `CClientModeShared_OverrideView.cpp` (thirdperson) |
| Viewmodel-layer glow | `CViewRender_DrawViewModels` hook (inside `CClientModeShared_DoPostScreenSpaceEffects.cpp`) |
| Crit logic | `CTFWeaponBase_CalcIsAttackCritical.cpp`, `_CanFireRandomCriticalShot.cpp` |
| Bullet/spread | `CTFPlayer_FireBullet.cpp`, `FX_FireBullets.cpp` |
| Bone/animation | `CBaseAnimating_SetupBones.cpp`, `CTFPlayer_BuildTransformations.cpp`, `_UpdateClientSideAnimation.cpp` |
| Net channel / packets | `CNetChannel_SendDatagram.cpp` (fakelag/DT), `CL_Move.cpp`, `CL_ReadPackets.cpp` |
| Menu render + input | `Direct3DDevice9.cpp` (ImGui present/reset) |

## Features/ inventory (instance names = dir names unless noted)

| Dir | What |
|---|---|
| `Aimbot/` | `F::Aimbot` aggregator → AimbotGlobal, AimbotHitscan, AimbotProjectile (+MoveSim/ProjSim), AimbotMelee, AutoAirblast, AutoDetonate, AutoHeal, AutoRocketJump, DoubleSticky |
| `Backtrack/` | Lag-comp record rewind; records skipped when nothing consumes them |
| `Binds/` | Key-bound var overrides (writes `ConfigVar::Map[bindIdx]`) |
| `CheatDetection/` | Flags other cheaters |
| `Commands/` | Custom console commands |
| `Configs/` | Save/load all `G::Vars` to Amalgam config dir |
| `CritHack/` | Crit bucket prediction/forcing; feeds `Visuals/CritBar` |
| `EnginePrediction/` | Local-player prediction around CreateMove |
| `ImGui/` | `Render.cpp` (device hook side), `Menu/Menu.cpp` + `Menu/Components.h` (F* widgets), Fonts, Notifications, Easings |
| `Misc/` | Movement (bhop, AirCrouch), Automation, AutoQueue, AutoVote |
| `NetworkFix/`, `NoSpread/`, `PacketManip/` (AntiAim, FakeLag), `Resolver/` | Net-level manipulation |
| `Players/` | PlayerUtils: priorities/tags/friends (`F::PlayerUtils`) |
| `Simulation/` | MovementSimulation (`F::MoveSim`), ProjectileSimulation — shared by aimbot + trajectory vis |
| `Spectate/`, `Ticks/` (doubletap/warp `F::Ticks`), `World/` (skybox/world modulation) | |
| `Output/` | Chat/log output (`F::Output`) |
| `Visuals/` | `Visuals.cpp` aggregator (effects, sticky radius, pickup timers, hitboxes) + ESP, Chams, Glow, Materials (material registry), Groups (entity draw-groups), CritBar, SentryRange, FlexFOV, RearView, CameraWindow, OffscreenArrows, PlayerConditions, SpectatorList, FakeAngle |
| `Debug/` | `DEBUG_INFO`-gated overlays |

## Vars.h namespace index (grep `NAMESPACE_BEGIN(<Name>)`)

`Vars::Menu`, `Vars::Theme`, `Vars::Colors` (ALL colors), `Vars::Aimbot::{General,Hitscan,Projectile,Melee,Healing}`, `Vars::Backtrack`, `Vars::Doubletap`, `Vars::Fakelag`, `Vars::Speedhack`, `Vars::Resolver`, `Vars::ESP`, `Vars::Visuals::{UI,CritBar,Thirdperson,Removals,Effects,Viewmodel,World,Beams,Line,Hitbox,Prediction,Simulation,SentryRange,Path,Trajectory}`, `Vars::Misc::{Movement,Automation,Exploits,Game,Queueing,Sound,Logging}`, `Vars::Debug`.
FlexFOV vars live in `Vars::Visuals::UI` (`FlexFOV*`).

## Menu.cpp layout

Tabs render sequentially in `Menu.cpp`; find placement by grepping an existing `Section("Name")`. Notable: Aimbot sections (General/Backtrack/Crit Hack/Healing/Hitscan/Projectile/Melee), Visuals sections (Line/Hitbox/Prediction/Simulation/Sentry Range/...), debug sections gated by `Vars::Debug::Options.Value`. Widgets from `Components.h`: `FToggle`, `FSlider(var, FSliderEnum::Left|Right, fmt)`, `FDropdown(var, FDropdownEnum::..., offset)`, `FColorPicker(var, FColorPickerEnum::SameLine, offset, size)`, `FText`, `FButton`. Two-column layout via Left/Right enums; `H::Draw.Scale(px)` for DPI.
