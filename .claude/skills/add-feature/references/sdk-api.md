# Amalgam API surface (verified signatures)

## Config vars (`SDK/Vars.h`)

`ConfigVar<T>` holds `Value` (current, post-bind), `Default`, and `Map` (per-bind values; `Map[DEFAULT_BIND]`=`var[-1]` is the unbound base — use `var[DEFAULT_BIND]` in menu code, `.Value` everywhere else). Binds (`Features/Binds`) rewrite `Value` from `Map` each frame.

```cpp
CVar(Name, "Menu title", defaultValue, flags...);              // type = decltype(default)
CVar(Name, "Title", 50, SLIDER_CLAMP, 0, 100, 1, "%i%%");      // int slider w/ min/max/step/fmt
CVarEnum(Name, "Title", 0, DROPDOWN_MULTI, nullptr, "Off\0A\0B\0", Off, A, B); // + NameEnum::A
```
Flags: `VISUAL` (cosmetic, exempt from anticheat-compat), `NOSAVE`, `NOBIND`, `DEBUGVAR`, `SLIDER_CLAMP/MIN/MAX/PRECISION`, `DROPDOWN_MULTI/MODIFIABLE/CUSTOM/AUTOUPDATE`.
Types usable: `bool,int,float,std::string,Color_t,Gradient_t,IntRange_t,FloatRange_t,DragBox_t,WindowBox_t,std::vector<std::pair<std::string,Color_t>>`…

## Entities — `H::Entities` (`SDK/Helpers/Entities/Entities.h`)

```cpp
CTFPlayer* GetLocal();  CTFWeaponBase* GetWeapon();  CTFPlayerResource* GetResource();
const std::vector<CBaseEntity*>& GetGroup(EGroupType);
// EGroupType (EntityEnum): PlayerAll/Enemy/Team, BuildingAll/Enemy/Team,
//   WorldProjectile, WorldNPC, WorldBomb, LocalStickies, LocalFlares, SniperDots
float GetDeltaTime(i); int GetChoke(i); Vec3 GetEyeAngles(i); bool GetLagCompensation(i);
bool IsFriend(i); int GetPriority(i); DormantData* GetDormancy(i);
```
Casts: `pEntity->As<CObjectSentrygun>()` (unchecked static). Class id: `pEntity->GetClassID() == ETFClassID::CObjectSentrygun`. Netvars are generated accessors: `m_iHealth()`, `m_vecOrigin()`, `m_hOwnerEntity()` — declared with `NETVAR(name, type, table, prop)` (+`_OFF/_EMBED/_ARRAY` variants, `Utils/NetVars/NetVars.h`) inside class headers in `SDK/Definitions/Main/`.

## SDK:: (`SDK/SDK.h`)

```cpp
bool  W2S(const Vec3& vOrigin, Vec3& vScreen, bool bAlways=false); // FlexFOV-aware; never use raw matrix
bool  IsOnScreen(CBaseEntity*, ...);
void  Trace(vStart, vEnd, nMask, ITraceFilter*, CGameTrace*);      // filters: SDK/Helpers/TraceFilters
void  TraceHull(vStart, vEnd, vHullMin, vHullMax, nMask, pFilter, pTrace);
bool  VisPos / VisPosCollideable / VisPosWorld(pSkip, pEntity, vFrom, vTo, nMask=MASK_SHOT|CONTENTS_GRATE);
Vec3  PredictOrigin(vOrigin, vVelocity, flLatency, bTrace=true, ...);
float GetGravity();  float MaxSpeed(pPlayer, ...);  float AttribHookValue(val, "attr_name", pEconEnt);
EWeaponType GetWeaponType(pWeapon);  int IsAttacking(pLocal, pWeapon, pCmd);
void  FixMovement(pCmd, vTargetAngle);  void WalkTo(pCmd, pLocal, vTo);
void  GetProjectileFireSetup(pPlayer, vAngIn, vOffset, vPosOut, vAngOut, ...);
int   RandomInt(min,max); float RandomFloat(min,max); double PlatFloatTime();
bool  CleanScreenshot();  // gate: skip drawing when true
```

## 2D drawing — `H::Draw` (`SDK/Helpers/Draw/Draw.h`)

```cpp
String(tFont, x, y, tColor, ALIGN_TOPLEFT|ALIGN_CENTER|..., "fmt or text");
Line(x1,y1,x2,y2,c); FillRect(x,y,w,h,c); OutlinedRect; GradientRect(x,y,w,h,cTop,cBot,bHorizontal);
Texture(name,x,y,w,h,align);
int Scale(int px, EScale = Scale_Floor|Round|Ceil);   // DPI scale — wrap ALL pixel constants
m_nScreenW / m_nScreenH;  UpdateW2SMatrix();
```
Fonts: `H::Fonts.GetFont(FONT_ESP)` / `FONT_INDICATORS`.
Menu widgets (`ImGui/Menu/Components.h`) have ConfigVar overloads that auto-label from the var's title — `FToggle(Vars::X::Y, FToggleEnum::Left)`, `FSlider(var, FSliderEnum::Right, "fmt")`, `FDropdown(var, ...)`, `FColorPicker(var, FColorPickerEnum::SameLine, offset, size)` — plus raw `("label", &value)` overloads for non-var state.
World-space engine-overlay primitives (call from render-pass hooks, NOT Paint):
`RenderLine(v1,v2,c,bZBuffer)`, `RenderBox(vOrigin,vMins,vMaxs,vAngles,c,bZBuffer,bInsideOut)`, `RenderTriangle(v1,v2,v3,c,bZBuffer)`. ⚠ crashes at ~10k+ calls/pass (see patterns.md).

## Interfaces — `I::` (`SDK/Definitions/Interfaces/*.h`)

`I::EngineClient` (`IsInGame`, `GetLocalPlayer`, `GetViewAngles`, `Time`, `IsHLTV`), `I::ClientEntityList`, `I::EngineTrace`, `I::EngineVGui` (`IsGameUIVisible`), `I::MatSystemSurface`, `I::MaterialSystem`, `I::ModelRender`, `I::RenderView`, `I::ViewRender`, `I::GlobalVars` (`curtime`, `tickcount`, `interval_per_tick`, `frametime`), `I::ClientState`, `I::ClientModeShared`, `I::Input`, `I::Prediction`, `I::GameMovement`, `I::MoveHelper`, `I::CVar`, `I::DirectXDevice`.
ConVar access: `H::ConVars.FindVar("name")` / cached members.

## Macro system (`Utils/`)

```cpp
ADD_FEATURE(CMyThing, MyThing);                      // → F::MyThing        (Macros/Macros.h)
ADD_FEATURE_CUSTOM(CType, Name, H);                  // → H::Name
MAKE_SIGNATURE(Name, "client.dll", "AA BB ?? CC", 0);// → S::Name, .Call<Ret>(args) (Signatures/)
MAKE_HOOK(Name, addr, RetType, Args...) { ... CALL_ORIGINAL(args); } // Hooks/Hooks.h; addr often
//   U::Memory.GetVirtual(I::Iface, vtableIdx) or S::Name()
MAKE_INTERFACE_VERSION(Type, Symbol, "dll", "VersionString001");     // → I::Symbol (Interfaces/)
MAKE_INTERFACE_SIGNATURE(Type, Symbol, "dll", "sig", off, deref);
NETVAR(m_iHealth, int, "DT_BasePlayer", "m_iHealth");                // in class headers
```
Hooks self-register; new hook files need only the vcxproj registration. `DEBUG_RETURN(...)` first line of every hook body.

## Key types (`SDK/Definitions/Types.h`)

`Vec3` (full vector math: `Length/Length2D/Normalized/Dot/Cross/DistTo`, angles via `Math::` in `Utils/Math/`), `Color_t {r,g,b,a}` + `.Alpha(a)`, `Gradient_t` (multi-stop, `.Lerp(t)`), `IntRange_t/FloatRange_t {Min,Max}`, `DragBox_t {x,y}` (draggable on-screen HUD position, e.g. `CritBar::Display.Value.x`), `WindowBox_t {x,y,w,h}`.
`Math::` helpers: `CalcAngle`, `RotatePoint`, `VectorAngles/AngleVectors`, `RemapVal`, `NormalizeAngles`.
