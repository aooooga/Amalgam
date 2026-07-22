#pragma once
#include "../SDK/Definitions/Types.h"
#include "../Utils/Macros/Macros.h"
#include <windows.h>
#include <typeinfo>

#define DEFAULT_BIND -1

template <class T>
class ConfigVar;

class BaseVar
{
public:
	std::vector<const char*> m_vNames;
	int m_iFlags = 0;
	union {
		int i = 0;
		float f;
	} m_unMin;
	union {
		int i = 0;
		float f;
	} m_unMax;
	union {
		int i = 0;
		float f;
	} m_unStep;
	std::vector<const char*> m_vValues = {};
	const char* m_sExtra = nullptr;

protected:
	std::string m_sName;
	const char* m_sSection;

public:
	constexpr const char* Name() const
	{
		return m_sName.c_str();
	}
	constexpr const char* Section() const
	{
		return m_sSection;
	}

public:
	size_t m_iType;

	template <class T>
	inline ConfigVar<T>* As()
	{
		if (typeid(T).hash_code() != m_iType)
			return nullptr;

		return reinterpret_cast<ConfigVar<T>*>(this);
	}
};

namespace G
{
	inline std::vector<BaseVar*> Vars = {};
};

template <class T>
class ConfigVar : public BaseVar
{
public:
	T Value;
	T Default;
	std::unordered_map<int, T> Map = {};

	ConfigVar(T tValue, std::vector<const char*> vNames, const char* sName, const char* sSection, int iFlags = 0, std::vector<const char*> vValues = {}, const char* sNone = nullptr)
	{
		Value = Default = Map[DEFAULT_BIND] = tValue;
		m_iType = typeid(T).hash_code();

		m_vNames = vNames;
		m_sName = std::string(sName).replace(strlen(sName) - 1, 1, "");
		m_sSection = sSection;

		m_iFlags = iFlags;
		m_vValues = vValues;
		m_sExtra = sNone;

		G::Vars.push_back(this);
	}
	ConfigVar(T tValue, std::vector<const char*> vNames, const char* sName, const char* sSection, int iFlags, int iMin, int iMax, int iStep = 1, const char* sFormat = "%i")
	{
		Value = Default = Map[DEFAULT_BIND] = tValue;
		m_iType = typeid(T).hash_code();

		m_vNames = vNames;
		m_sName = std::string(sName).replace(strlen(sName) - 1, 1, "");
		m_sSection = sSection;

		m_iFlags = iFlags;
		m_unMin.i = iMin;
		m_unMax.i = iMax;
		m_unStep.i = iStep;
		m_sExtra = sFormat;

		G::Vars.push_back(this);
	}
	ConfigVar(T tValue, std::vector<const char*> vNames, const char* sName, const char* sSection, int iFlags, float flMin, float flMax, float flStep = 1.f, const char* sFormat = "%g")
	{
		Value = Default = Map[DEFAULT_BIND] = tValue;
		m_iType = typeid(T).hash_code();

		m_vNames = vNames;
		m_sName = std::string(sName).replace(strlen(sName) - 1, 1, "");
		m_sSection = sSection;

		m_iFlags = iFlags;
		m_unMin.f = flMin;
		m_unMax.f = flMax;
		m_unStep.f = flStep;
		m_sExtra = sFormat;

		G::Vars.push_back(this);
	}

	inline T& operator[](int i)
	{
		return Map[i];
	}
	inline bool contains(int i) const
	{
		return Map.contains(i);
	}
};

#define NAMESPACE_BEGIN(name, ...) \
	namespace name { \
		constexpr inline const char* Section() { return !std::string(#__VA_ARGS__).empty() ? ""#__VA_ARGS__ : #name; }
#define NAMESPACE_END(name) \
	}

#define CVar(name, title, value, ...) \
	constexpr inline const char* name##_() { return __FUNCTION__; } \
	inline ConfigVar<decltype(value)> name = { value, { title }, name##_(), Section(), __VA_ARGS__ }
#define CVarValues(name, title, value, flags, none, ...) \
	constexpr inline const char* name##_() { return __FUNCTION__; } \
	inline ConfigVar<decltype(value)> name = { value, { title }, name##_(), Section(), flags, { __VA_ARGS__ }, none }
#define Enum(name, ...) \
	namespace name##Enum { enum name##Enum { __VA_ARGS__ }; }
#define CVarEnum(name, title, value, flags, none, values, ...) \
	CVarValues(name, title, value, flags, none, values); \
	Enum(name, __VA_ARGS__);

#define NONE 0
#define VISUAL (1 << 31)
#define NOSAVE (1 << 30)
#define NOBIND (1 << 29)
#define DEBUGVAR (1 << 28)

// flags to be automatically used in widgets. keep these as the same values as the flags in components, do not include visual flags
#define SLIDER_CLAMP (1 << 2)
#define SLIDER_MIN (1 << 3)
#define SLIDER_MAX (1 << 4)
#define SLIDER_PRECISION (1 << 5)
#define SLIDER_NOAUTOUPDATE (1 << 6)
#define DROPDOWN_MULTI (1 << 2)
#define DROPDOWN_MODIFIABLE (1 << 3)
#define DROPDOWN_NOSANITIZATION (1 << 4)
#define DROPDOWN_CUSTOM (1 << 2)
#define DROPDOWN_AUTOUPDATE (1 << 3)

NAMESPACE_BEGIN(Vars)
	NAMESPACE_BEGIN(Menu)
		CVar(CheatTitle, "Cheat title", std::string("Amalgam"), VISUAL | DROPDOWN_AUTOUPDATE);
		CVar(CheatTag, "Cheat tag", std::string("[Amalgam]"), VISUAL);
		CVar(PrimaryKey, "Primary key", VK_INSERT, NOBIND);
		CVar(SecondaryKey, "Secondary key", VK_F3, NOBIND);

		CVar(BindWindow, "Bind window", true);
		CVar(BindWindowTitle, "Bind window title", true);
		CVar(MenuShowsBinds, "Menu shows binds", false, NOBIND);

		CVarEnum(Indicators, "Indicators", 0b00000, VISUAL | DROPDOWN_MULTI, nullptr,
			VA_LIST("Ticks", "Crit hack", "Spectators", "Ping", "Conditions", "Seed prediction"),
			Ticks = 1 << 0, CritHack = 1 << 1, Spectators = 1 << 2, Ping = 1 << 3, Conditions = 1 << 4, SeedPrediction = 1 << 5);

		CVar(BindsDisplay, "Binds display", DragBox_t(100, 100), VISUAL | NOBIND);
		CVar(TicksDisplay, "Ticks display", DragBox_t(), VISUAL | NOBIND);
		CVar(CritsDisplay, "Crits display", DragBox_t(), VISUAL | NOBIND);
		CVar(SpectatorsDisplay, "Spectators display", DragBox_t(), VISUAL | NOBIND);
		CVar(PingDisplay, "Ping display", DragBox_t(), VISUAL | NOBIND);
		CVar(ConditionsDisplay, "Conditions display", DragBox_t(), VISUAL | NOBIND);
		CVar(SeedPredictionDisplay, "Seed prediction display", DragBox_t(), VISUAL | NOBIND);

		CVar(Scale, "Scale", 1.f, NOBIND | SLIDER_MIN | SLIDER_PRECISION | SLIDER_NOAUTOUPDATE, 0.75f, 2.f, 0.25f);
		CVar(CheapText, "Cheap text", false, NOBIND);

		// Design 3a — menu content-layout tweaks (UI only)
		CVar(CompactColumns, "Compact columns", true, NOBIND);
		CVar(DescriptionsOnHover, "Descriptions on hover", true, NOBIND);
		CVar(CollapsedPanels, "Collapsed panels", std::string(""), NOBIND);

		NAMESPACE_BEGIN(Theme)
			CVar(Accent, "Accent color", Color_t(175, 150, 255, 255), VISUAL);
			CVar(Background, "Background color", Color_t(0, 0, 0, 250), VISUAL);
			// ---- Per-surface overrides -------------------------------------------
			// Every entry below is alpha-0 = "unset" -> use the derived value, EXCEPT
			// the ramp steps, which are gated by BackgroundOverride and seeded on first
			// use (see CRender::LoadColors). Each var drives exactly ONE surface so
			// editing one never moves another.

			// Base ramp (the shades everything else derives from).
			CVar(BackgroundOverride, "Override base ramp", false, NOBIND);
			CVar(Background0, "Base: darkest", Color_t(0, 0, 0, 0), VISUAL);
			CVar(Background0p5, "Base: dark", Color_t(0, 0, 0, 0), VISUAL);
			CVar(Background1, "Base: mid", Color_t(0, 0, 0, 0), VISUAL);
			CVar(Background1p5, "Base: light", Color_t(0, 0, 0, 0), VISUAL);
			CVar(Background2, "Base: lightest", Color_t(0, 0, 0, 0), VISUAL);

			// Window chrome.
			CVar(WindowBackground, "Window background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(NavBackground, "Nav bar background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(NavDivider, "Nav bar divider", Color_t(0, 0, 0, 0), VISUAL);

			// Panels.
			CVar(PanelBackground, "Panel background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelHeader, "Panel header band", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelBorder, "Panel border", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelAccent, "Panel accent edge", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelTitle, "Panel title text", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelCollapsedBackground, "Collapsed panel background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelCollapsedHeader, "Collapsed panel header", Color_t(0, 0, 0, 0), VISUAL);
			CVar(PanelCollapsedTitle, "Collapsed panel title", Color_t(0, 0, 0, 0), VISUAL);
			CVar(RowDivider, "Row divider", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SubGroupText, "Subgroup label", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SubGroupRule, "Subgroup rule", Color_t(0, 0, 0, 0), VISUAL);

			// Text tiers.
			CVar(TextDim, "Dim text", Color_t(0, 0, 0, 0), VISUAL);
			CVar(TextDisabled, "Disabled text", Color_t(0, 0, 0, 0), VISUAL);

			// Accent variants, normally derived from Accent by alpha.
			CVar(AccentOverride, "Override accent variants", false, NOBIND);
			CVar(AccentMuted, "Accent muted", Color_t(0, 0, 0, 0), VISUAL);
			CVar(AccentWashed, "Accent washed", Color_t(0, 0, 0, 0), VISUAL);

			// Controls.
			CVar(ControlBackground, "Control background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(ControlHovered, "Control hovered", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SwitchOn, "Switch on", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SwitchOff, "Switch off", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SwitchKnobOn, "Switch knob (on)", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SwitchKnobOff, "Switch knob (off)", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SliderTrack, "Slider track", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SliderFill, "Slider fill", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SliderKnob, "Slider knob", Color_t(0, 0, 0, 0), VISUAL);
			CVar(SliderValueText, "Slider value text", Color_t(0, 0, 0, 0), VISUAL);

			// Tabs.
			CVar(TabActive, "Tab active text", Color_t(0, 0, 0, 0), VISUAL);
			CVar(TabInactive, "Tab inactive text", Color_t(0, 0, 0, 0), VISUAL);
			CVar(TabBar, "Tab indicator bar", Color_t(0, 0, 0, 0), VISUAL);

			// Popups / tooltips.
			CVar(PopupBackground, "Popup background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(TooltipBackground, "Tooltip background", Color_t(0, 0, 0, 0), VISUAL);
			CVar(TooltipText, "Tooltip text", Color_t(0, 0, 0, 0), VISUAL);
			CVar(Active, "Active color", Color_t(255, 255, 255, 255), VISUAL);
			CVar(Inactive, "Inactive color", Color_t(150, 150, 150, 255), VISUAL);
		NAMESPACE_END(Theme)
	NAMESPACE_END(Menu)

	NAMESPACE_BEGIN(Colors)
		CVar(IndicatorGood, "Indicator good", Color_t(0, 255, 100, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorMid, "Indicator mid", Color_t(255, 200, 0, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorBad, "Indicator bad", Color_t(255, 0, 0, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorMisc, "Indicator misc", Color_t(75, 175, 255, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorTextGood, "Indicator text good", Color_t(150, 255, 150, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorTextMid, "Indicator text mid", Color_t(255, 200, 0, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorTextBad, "Indicator text bad", Color_t(255, 150, 150, 255), NOSAVE | DEBUGVAR);
		CVar(IndicatorTextMisc, "Indicator text misc", Color_t(100, 255, 255, 255), NOSAVE | DEBUGVAR);

		CVar(Local, "Local color", Color_t(255, 255, 255), VISUAL);
		CVar(FOVCircle, "FOV circle color", Color_t(255, 255, 255, 100), VISUAL);
		CVar(SpellFootstep, "Spell footstep color", Color_t(255, 255, 255, 255), VISUAL);

		CVar(WorldModulation, VA_LIST("World modulation", "World modulation color"), Color_t(255, 255, 255, 255), VISUAL);
		CVar(SkyModulation, VA_LIST("Sky modulation", "Sky modulation color"), Color_t(255, 255, 255, 255), VISUAL);
		CVar(PropModulation, VA_LIST("Prop modulation", "Prop modulation color"), Color_t(255, 255, 255, 255), VISUAL);
		CVar(ParticleModulation, VA_LIST("Particle modulation", "Particle modulation color"), Color_t(255, 255, 255, 255), VISUAL);
		CVar(FogModulation, VA_LIST("Fog modulation", "Fog modulation color"), Color_t(255, 255, 255, 255), VISUAL);

		CVar(Line, "Line color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(LineIgnoreZ, "Line ignore Z color", Color_t(255, 255, 255, 0), VISUAL);

		CVar(BoneHitboxEdge, "Bone hitbox edge color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(BoneHitboxEdgeIgnoreZ, "Bone hitbox edge ignore Z color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(BoneHitboxFace, "Bone hitbox face color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(BoneHitboxFaceIgnoreZ, "Bone hitbox face ignore Z color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(TargetHitboxEdge, "Target hitbox edge color", Color_t(255, 150, 150, 255), VISUAL);
		CVar(TargetHitboxEdgeIgnoreZ, "Target hitbox edge ignore Z color", Color_t(255, 150, 150, 0), VISUAL);
		CVar(TargetHitboxFace, "Target hitbox face color", Color_t(255, 150, 150, 0), VISUAL);
		CVar(TargetHitboxFaceIgnoreZ, "Target hitbox face ignore Z color", Color_t(255, 150, 150, 0), VISUAL);
		CVar(BoundHitboxEdge, "Bound hitbox edge color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(BoundHitboxEdgeIgnoreZ, "Bound hitbox edge ignore Z color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(BoundHitboxFace, "Bound hitbox face color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(BoundHitboxFaceIgnoreZ, "Bound hitbox face ignore Z color", Color_t(255, 255, 255, 0), VISUAL);

		CVar(PlayerPath, "Player path color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(PlayerPathIgnoreZ, "Player path ignore Z color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(ProjectilePath, "Projectile path color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(ProjectilePathIgnoreZ, "Projectile path ignore Z color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(TrajectoryPath, "Trajectory path color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(TrajectoryPathIgnoreZ, "Trajectory path ignore Z color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(ShotPath, "Shot path color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(ShotPathIgnoreZ, "Shot path ignore Z color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(SplashRadius, "Splash radius color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(SplashRadiusIgnoreZ, "Splash radius ignore Z color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(StickyRadius, "Sticky radius color", Color_t(255, 255, 255, 0), VISUAL);
		CVar(StickyRadiusIgnoreZ, "Sticky radius ignore Z color", Color_t(255, 255, 255, 255), VISUAL);
		CVar(StickyRadiusPlayerInside, "Sticky radius player inside color", Color_t(255, 50, 50, 0), VISUAL);
		CVar(StickyRadiusPlayerInsideIgnoreZ, "Sticky radius player inside ignore Z color", Color_t(255, 50, 50, 255), VISUAL);

		CVar(HealRadiusConnect, VA_LIST("Radius edge", "Heal radius connect color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectIgnoreZ, VA_LIST("Radius edge ignore Z", "Heal radius connect ignore Z color"), Color_t(50, 255, 50, 255), VISUAL);
		CVar(HealRadiusConnectFill, VA_LIST("Radius fill", "Heal radius connect fill color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectFillIgnoreZ, VA_LIST("Radius fill ignore Z", "Heal radius connect fill ignore Z color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectBottom, VA_LIST("Cylinder bottom edge", "Heal radius connect bottom color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectBottomIgnoreZ, VA_LIST("Cylinder bottom edge ignore Z", "Heal radius connect bottom ignore Z color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectTop, VA_LIST("Cylinder top edge", "Heal radius connect top color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectTopIgnoreZ, VA_LIST("Cylinder top edge ignore Z", "Heal radius connect top ignore Z color"), Color_t(50, 255, 50, 255), VISUAL);
		CVar(HealRadiusConnectBottomFill, VA_LIST("Cylinder bottom fill", "Heal radius connect bottom fill color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectBottomFillIgnoreZ, VA_LIST("Cylinder bottom fill ignore Z", "Heal radius connect bottom fill ignore Z color"), Color_t(50, 255, 50, 60), VISUAL);
		CVar(HealRadiusConnectTopFill, VA_LIST("Cylinder top fill", "Heal radius connect top fill color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusConnectTopFillIgnoreZ, VA_LIST("Cylinder top fill ignore Z", "Heal radius connect top fill ignore Z color"), Color_t(50, 255, 50, 0), VISUAL);
		CVar(HealRadiusDisconnect, VA_LIST("Radius edge", "Heal radius disconnect color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectIgnoreZ, VA_LIST("Radius edge ignore Z", "Heal radius disconnect ignore Z color"), Color_t(255, 50, 50, 255), VISUAL);
		CVar(HealRadiusDisconnectFill, VA_LIST("Radius fill", "Heal radius disconnect fill color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectFillIgnoreZ, VA_LIST("Radius fill ignore Z", "Heal radius disconnect fill ignore Z color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectBottom, VA_LIST("Cylinder bottom edge", "Heal radius disconnect bottom color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectBottomIgnoreZ, VA_LIST("Cylinder bottom edge ignore Z", "Heal radius disconnect bottom ignore Z color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectTop, VA_LIST("Cylinder top edge", "Heal radius disconnect top color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectTopIgnoreZ, VA_LIST("Cylinder top edge ignore Z", "Heal radius disconnect top ignore Z color"), Color_t(255, 50, 50, 255), VISUAL);
		CVar(HealRadiusDisconnectBottomFill, VA_LIST("Cylinder bottom fill", "Heal radius disconnect bottom fill color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectBottomFillIgnoreZ, VA_LIST("Cylinder bottom fill ignore Z", "Heal radius disconnect bottom fill ignore Z color"), Color_t(255, 50, 50, 60), VISUAL);
		CVar(HealRadiusDisconnectTopFill, VA_LIST("Cylinder top fill", "Heal radius disconnect top fill color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(HealRadiusDisconnectTopFillIgnoreZ, VA_LIST("Cylinder top fill ignore Z", "Heal radius disconnect top fill ignore Z color"), Color_t(255, 50, 50, 0), VISUAL);
		CVar(SentryRangeEnemy, "Enemy edge color", Color_t(255, 100, 80, 255), VISUAL);
		CVar(SentryRangeEnemyIgnoreZ, "Enemy edge ignore Z color", Color_t(255, 100, 80, 60), VISUAL);
		CVar(SentryRangeTeam, "Team edge color", Color_t(80, 160, 255, 255), VISUAL);
		CVar(SentryRangeTeamIgnoreZ, "Team edge ignore Z color", Color_t(80, 160, 255, 60), VISUAL);
		CVar(SentryRangeLocal, "Local edge color", Color_t(100, 255, 120, 255), VISUAL);
		CVar(SentryRangeLocalIgnoreZ, "Local edge ignore Z color", Color_t(100, 255, 120, 60), VISUAL);
		CVar(SentryRangePlayerInside, "Player inside edge color", Color_t(255, 50, 50, 255), VISUAL);
		CVar(SentryRangePlayerInsideIgnoreZ, "Player inside edge ignore Z color", Color_t(255, 50, 50, 80), VISUAL);
		CVar(SentryRangeFillEnemy, "Enemy fill color", Color_t(255, 100, 80, 70), VISUAL);
		CVar(SentryRangeFillEnemyIgnoreZ, "Enemy fill ignore Z color", Color_t(255, 100, 80, 25), VISUAL);
		CVar(SentryRangeFillTeam, "Team fill color", Color_t(80, 160, 255, 70), VISUAL);
		CVar(SentryRangeFillTeamIgnoreZ, "Team fill ignore Z color", Color_t(80, 160, 255, 25), VISUAL);
		CVar(SentryRangeFillLocal, "Local fill color", Color_t(100, 255, 120, 70), VISUAL);
		CVar(SentryRangeFillLocalIgnoreZ, "Local fill ignore Z color", Color_t(100, 255, 120, 25), VISUAL);
		CVar(SentryRangeFillPlayerInside, "Player inside fill color", Color_t(255, 50, 50, 90), VISUAL);
		CVar(SentryRangeFillPlayerInsideIgnoreZ, "Player inside fill ignore Z color", Color_t(255, 50, 50, 35), VISUAL);
		CVar(DoubleStickyPath, "Path color", Color_t(0, 255, 140, 180), VISUAL);
		CVar(RealPath, "Real path color", Color_t(255, 255, 255, 0), NOSAVE | DEBUGVAR);
		CVar(RealPathIgnoreZ, "Real path ignore Z color", Color_t(255, 255, 255, 255), NOSAVE | DEBUGVAR);
	NAMESPACE_END(Colors)

	NAMESPACE_BEGIN(Aimbot)
		NAMESPACE_BEGIN(General, Aimbot)
			CVarEnum(AimType, "Aim type", 0, NONE, nullptr,
				VA_LIST("Off", "Plain", "Smooth", "Silent", "Locking", "Assistive"),
				Off, Plain, Smooth, Silent, Locking, Assistive);
			CVarEnum(TargetSelection, "Target selection", 0, NONE, nullptr,
				VA_LIST("FOV", "Distance", "Hybrid"),
				FOV, Distance, Hybrid);
			CVarEnum(Target, "Target", 0b0000001, DROPDOWN_MULTI, nullptr,
				VA_LIST("Players", "Sentries", "Dispensers", "Teleporters", "Stickies", "NPCs", "Bombs"),
				Players = 1 << 0, Sentry = 1 << 1, Dispenser = 1 << 2, Teleporter = 1 << 3, Stickies = 1 << 4, NPCs = 1 << 5, Bombs = 1 << 6,
				All = Players | Sentry | Dispenser | Teleporter | Stickies | NPCs | Bombs, Building = Sentry | Dispenser | Teleporter);
			CVarEnum(Ignore, "Ignore", 0b00000001000, DROPDOWN_MULTI, nullptr,
				VA_LIST("Friends", "Party", "Unprioritized", "Invulnerable", "Invisible", "Unsimulated", "Dead ringer", "Vaccinator", "Disguised", "Taunting", "Team"),
				Friends = 1 << 0, Party = 1 << 1, Unprioritized = 1 << 2, Invulnerable = 1 << 3, Invisible = 1 << 4, Unsimulated = 1 << 5, DeadRinger = 1 << 6, Vaccinator = 1 << 7, Disguised = 1 << 8, Taunting = 1 << 9, Team = 1 << 10);
			CVar(AimFOV, "Aim FOV", 180.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 180.f);
			CVar(MaxTargets, "Max targets", 2, SLIDER_MIN, 1, 6);
			CVar(IgnoreInvisible, "Ignore invisible", 50.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 10.f, "%g%%");
			CVar(AssistStrength, "Assist strength", 25.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 1.f, "%g%%");
			CVar(TickTolerance, "Tick tolerance", 4, SLIDER_CLAMP, 0, 21);
			CVar(AutoShoot, "Auto shoot", true);
			CVar(FOVCircle, "FOV Circle", true, VISUAL);
			CVar(LeadAndRestrict, "Lead and restrict", false, VISUAL);
			CVar(NoSpread, "No spread", false);

			CVarEnum(AimHoldsFire, "Aim holds fire", 2, NOSAVE | DEBUGVAR, nullptr,
				VA_LIST("False", "Minigun only", "Always"),
				False, MinigunOnly, Always);
			CVar(NoSpreadOffset, "No spread offset", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -1.f, 1.f, 0.1f);
			CVar(NoSpreadAverage, "No spread average", 5, NOSAVE | DEBUGVAR | SLIDER_MIN, 1, 25);
			CVar(NoSpreadInterval, "No spread interval", 0.1f, NOSAVE | DEBUGVAR | SLIDER_MIN, 0.05f, 5.f, 0.1f, "%gs");
			CVar(NoSpreadBackupInterval, "No spread backup interval", 2.f, NOSAVE | DEBUGVAR | SLIDER_MIN, 2.f, 10.f, 0.1f, "%gs");
		NAMESPACE_END(Global)

		NAMESPACE_BEGIN(Hitscan)
			CVarEnum(Hitboxes, VA_LIST("Hitboxes", "Hitscan hitboxes"), 0b000111, DROPDOWN_MULTI, nullptr,
				VA_LIST("Head", "Body", "Pelvis", "Arms", "Legs", "##Divider", "Bodyaim if lethal", "Headshot only"),
				Head = 1 << 0, Body = 1 << 1, Pelvis = 1 << 2, Arms = 1 << 3, Legs = 1 << 4, BodyaimIfLethal = 1 << 5, HeadshotOnly = 1 << 6);
			CVarValues(MultipointHitboxes, "Multipoint hitboxes", 0b00000, DROPDOWN_MULTI, "All",
				VA_LIST("Head", "Body", "Pelvis", "Arms", "Legs"));
			CVarEnum(Modifiers, VA_LIST("Modifiers", "Hitscan modifiers"), 0b0100000, DROPDOWN_MULTI, nullptr,
				VA_LIST("Tapfire", "Wait for headshot", "Wait for charge", "Scoped only", "Auto scope", "Auto rev minigun", "Extinguish team"),
				Tapfire = 1 << 0, WaitForHeadshot = 1 << 1, WaitForCharge = 1 << 2, ScopedOnly = 1 << 3, AutoScope = 1 << 4, AutoRev = 1 << 5, ExtinguishTeam = 1 << 6);
			CVar(MultipointScale, "Multipoint scale", 0.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 5.f, "%g%%");
			CVar(TapfireDistance, "Tapfire distance", 1000.f, SLIDER_MIN | SLIDER_PRECISION, 250.f, 1000.f, 50.f);

			CVarEnum(PeekCheck, "Peek check", 1, NOSAVE | DEBUGVAR, nullptr,
				VA_LIST("Off", "Doubletap only", "Always"),
				Off, DoubletapOnly, Always);
			CVar(PeekAmount, "Peek amount", 1, NOSAVE | DEBUGVAR, 0, 5);
			CVar(BoneSizeSubtract, "Bone size subtract", 1.f, NOSAVE | DEBUGVAR | SLIDER_MIN, 0.f, 4.f, 0.25f);
			CVar(BoneSizeMinimumScale, "Bone size minimum scale", 1.f, NOSAVE | DEBUGVAR | SLIDER_CLAMP, 0.f, 1.f, 0.1f);
		NAMESPACE_END(HITSCAN)

		NAMESPACE_BEGIN(Projectile)
			CVarEnum(StrafePrediction, VA_LIST("Predict", "Strafe prediction"), 0b11, DROPDOWN_MULTI, "Off",
				VA_LIST("Air strafing", "Ground strafing"),
				Air = 1 << 0, Ground = 1 << 1);
			CVarEnum(SplashPrediction, VA_LIST("Splash", "Splash prediction"), 0, NONE, nullptr,
				VA_LIST("Off", "Include", "Prefer", "Only"),
				Off, Include, Prefer, Only);
			CVarEnum(AutoDetonate, "Auto detonate", 0b00, DROPDOWN_MULTI, "Off",
				VA_LIST("Stickies", "Flares", "##Divider", "Prevent self damage", "Ignore invisible"),
				Stickies = 1 << 0, Flares = 1 << 1, PreventSelfDamage = 1 << 2, IgnoreInvisible = 1 << 3);
			CVarEnum(AutoAirblast, "Auto airblast", 0b000, DROPDOWN_MULTI, "Off",
				VA_LIST("Enabled", "##Divider", "Redirect", "Ignore FOV"),
				Enabled = 1 << 0, Redirect = 1 << 1, IgnoreFOV = 1 << 2);
			CVarEnum(Hitboxes, VA_LIST("Hitboxes", "Projectile hitboxes"), 0b001111, DROPDOWN_MULTI, nullptr,
				VA_LIST("Auto", "##Divider", "Head", "Body", "Feet", "##Divider", "Bodyaim if lethal", "Prioritize feet"),
				Auto = 1 << 0, Head = 1 << 1, Body = 1 << 2, Feet = 1 << 3, BodyaimIfLethal = 1 << 4, PrioritizeFeet = 1 << 5);
			CVarEnum(Modifiers, VA_LIST("Modifiers", "Projectile modifiers"), 0b0010, DROPDOWN_MULTI, nullptr,
				VA_LIST("Charge weapon", "Cancel charge", "Use arm time", "Air splash", "Lob angles", "Target dormant"),
				ChargeWeapon = 1 << 0, CancelCharge = 1 << 1, UseArmTime = 1 << 2, AirSplash = 1 << 3, LobAngles = 1 << 4, TargetDormant = 1 << 5);
			CVar(MaxSimulationTime, "Max simulation time", 1.f, SLIDER_MIN | SLIDER_PRECISION, 0.1f, 2.5f, 0.25f, "%gs");
			CVar(HitChance, "Hit chance", 0.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 10.f, "%g%%");
			CVar(AutodetRadius, "Autodet radius", 90.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 10.f, "%g%%");
			CVar(SplashRadius, "Splash radius", 90.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 10.f, "%g%%");
			CVar(AutoRelease, "Auto release", 0.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 5.f, "%g%%");
			CVar(DoubleSticky, "Double sticky", false);
			CVar(DoubleStickyKey, "Double sticky key", 0x0, NOBIND);

			CVar(GroundSamples, "Samples", 33, NOSAVE | DEBUGVAR, 3, 66);
			CVar(GroundStraightFuzzyValue, "Straight fuzzy value", 100.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 500.f, 25.f);
			CVar(GroundLowMinimumSamples, "Low min samples", 16, NOSAVE | DEBUGVAR, 3, 66);
			CVar(GroundHighMinimumSamples, "High min samples", 33, NOSAVE | DEBUGVAR, 3, 66);
			CVar(GroundLowMinimumDistance, "Low min distance", 0.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 2500.f, 100.f);
			CVar(GroundHighMinimumDistance, "High min distance", 1000.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 2500.f, 100.f);
			CVar(GroundMaxChanges, "Max changes", 0, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0, 5);
			CVar(GroundMaxChangeTime, "Max change time", 0, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0, 66);

			CVar(AirSamples, "Samples", 33, NOSAVE | DEBUGVAR, 3, 66);
			CVar(AirStraightFuzzyValue, "Straight fuzzy value", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 500.f, 25.f);
			CVar(AirLowMinimumSamples, "Low min samples", 16, NOSAVE | DEBUGVAR, 3, 66);
			CVar(AirHighMinimumSamples, "High min samples", 16, NOSAVE | DEBUGVAR, 3, 66);
			CVar(AirLowMinimumDistance, "Low min distance", 100000.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 2500.f, 100.f);
			CVar(AirHighMinimumDistance, "High min distance", 100000.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 2500.f, 100.f);
			CVar(AirMaxChanges, "Max changes", 2, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0, 5);
			CVar(AirMaxChangeTime, "Max change time", 16, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0, 66);

			CVar(VelocityAverageCount, "Velocity average count", 5, NOSAVE | DEBUGVAR, 1, 10);
			CVar(VerticalShift, "Vertical shift", 5.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f, 0.5f);
			CVar(DragOverride, "Drag override", 0.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 1.f, 0.01f);
			CVar(TimeOverride, "Time override", 0.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 2.f, 0.01f);
			CVar(LobAnglesUnderpredict, "Lob angles underpredict", true, NOSAVE | DEBUGVAR);
			CVar(HuntsmanLerp, "Huntsman lerp", 50.f, NOSAVE | DEBUGVAR | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 1.f, "%g%%");
			CVar(HuntsmanLerpLow, "Huntsman lerp low", 100.f, NOSAVE | DEBUGVAR | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 1.f, "%g%%");
			CVar(HuntsmanAdd, "Huntsman add", 0.f, NOSAVE | DEBUGVAR | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 20.f);
			CVar(HuntsmanAddLow, "Huntsman add low", 0.f, NOSAVE | DEBUGVAR | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 20.f);
			CVar(HuntsmanClamp, "Huntsman clamp", 5.f, NOSAVE | DEBUGVAR | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 10.f, 0.5f);
			CVar(HuntsmanPullPoint, "Huntsman pull point", false, NOSAVE | DEBUGVAR);
			CVar(HuntsmanPullNoZ, "Pull no Z", false, NOSAVE | DEBUGVAR);

			CVarEnum(SplashMode, "Splash mode", 0, NOSAVE | DEBUGVAR, nullptr,
				VA_LIST("Trace", "Face"),
				Trace, Face);
			CVar(SplashAirCount, "Splash air count", 0, NOSAVE | DEBUGVAR | SLIDER_MIN, 0, 10);
			CVar(SplashPointsDirect, "Splash points direct", 100, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0, 400, 5);
			CVar(SplashPointsArc, "Splash points arc", 100, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0, 400, 5);
			CVar(SplashRotateX, "Splash Rx", -1.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, -1.f, 360.f);
			CVar(SplashRotateY, "Splash Ry", -1.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, -1.f, 360.f);
			CVar(SplashDensityDirect, "Splash density direct", 40.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 100.f, 1.f);
			CVar(SplashDensityArc, "Splash density arc", 40.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 100.f, 1.f);
			CVar(SplashSamplesCutoff, "Splash samples cutoff", 0.0000001f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 0.000001f, 0.00000001f);
			CVar(SplashRestrictDirect, "Splash restrict direct", 100, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 1, 400, 5);
			CVar(SplashRestrictArc, "Splash restrict arc", 5, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 1, 400, 5);
			CVar(SplashRestrictFirst, "Splash restrict first", 25, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 1, 400, 5);
			CVar(DirectTraceInterval, "Direct trace interval", 1, NOSAVE | DEBUGVAR | SLIDER_MIN, 1, 20);
			CVar(SplashTraceInterval, "Splash trace interval", 10, NOSAVE | DEBUGVAR | SLIDER_MIN, 1, 20);
			CVar(LobTraceInterval, "Lob trace interval", 20, NOSAVE | DEBUGVAR | SLIDER_MIN, 1, 20);
			CVar(IntervalRetest, "Interval retest", true, NOSAVE | DEBUGVAR);

			CVar(DeltaCount, "Delta count", 5, NOSAVE | DEBUGVAR, 1, 5);
			CVarEnum(DeltaMode, "Delta mode", 0, NOSAVE | DEBUGVAR, nullptr,
				VA_LIST("Average", "Max"),
				Average, Max);
			CVarEnum(MovesimFrictionFlags, "Movesim friction flags", 0b01, NOSAVE | DEBUGVAR | DROPDOWN_MULTI, nullptr,
				VA_LIST("Run reduce", "Calculate increase"),
				RunReduce = 1 << 0, CalculateIncrease = 1 << 1);
		NAMESPACE_END(Projectile)

		NAMESPACE_BEGIN(Melee)
			CVar(AutoBackstab, "Auto backstab", true);
			CVar(IgnoreRazorback, "Ignore razorback", false);
			CVar(SwingPrediction, "Swing prediction", true);
			CVar(WhipTeam, "Whip team", false);

			CVar(SwingTicks, "Swing ticks", 13, NOSAVE | DEBUGVAR | SLIDER_MIN, 0, 14);
			CVar(SwingPredictLag, "Swing predict lag", true, NOSAVE | DEBUGVAR);
			CVarEnum(SwingValidateMode, "Swing validate mode", 0, NOSAVE | DEBUGVAR, nullptr,
				VA_LIST("Both", "Swing", "Simulated"),
				Both, Swing, Simulated);
			CVarEnum(BackstabFlags, "Backstab flags", 0b11, NOSAVE | DEBUGVAR | DROPDOWN_MULTI, nullptr,
				VA_LIST("Account ping", "Double test"),
				AccountPing = 1 << 0, DoubleTest = 1 << 1);
		NAMESPACE_END(Melee)

		NAMESPACE_BEGIN(Healing)
			CVarEnum(HealPriority, "Heal priority", 0, NONE, nullptr,
				VA_LIST("None", "Prioritize team", "Prioritize friends", "Friends only"),
				None, PrioritizeTeam, PrioritizeFriends, FriendsOnly);
			CVarEnum(DangerIgnore, "Danger ignore", 0b1000, DROPDOWN_MULTI, nullptr,
				VA_LIST("Friends", "Party", "Unprioritized", "Ignored"),
				Friends = 1 << 0, Party = 1 << 1, Unprioritized = 1 << 2, Ignored = 1 << 3,
				Shared = Friends | Party | Unprioritized);
			CVar(AutoHeal, "Auto heal", false);
			CVar(AutoArrow, "Auto arrow", false);
			CVar(AutoRepair, "Auto repair", false);
			CVar(AutoSandvich, "Auto sandvich", false);
			CVar(AutoVaccinator, "Auto vaccinator", false);
			CVar(ActivateOnVoice, "Activate on voice", false);

			CVar(AutoVaccinatorBulletScale, "Auto vaccinator bullet scale", 100.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 200.f, 10.f, "%g%%");
			CVar(AutoVaccinatorBlastScale, "Auto vaccinator blast scale", 100.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 200.f, 10.f, "%g%%");
			CVar(AutoVaccinatorFireScale, "Auto vaccinator fire scale", 100.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 200.f, 10.f, "%g%%");
			CVar(AutoVaccinatorFlamethrowerDamageOnly, "Auto vaccinator flamethrower damage only", false, NOSAVE | DEBUGVAR);

			CVarEnum(HealRadius, "Heal radius", 0b0, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("Connect", "Disconnect", "##Divider", "Cylinder"),
				Connect = 1 << 0, Disconnect = 1 << 1, Cylinder = 1 << 2,
				Enabled = Connect | Disconnect);
			// the wall is filled geometry and can run up to ~7x/frame under FlexFOV,
			// so the vertex count is capped well short of the engine's primitive limit
			CVar(HealRadiusVertices, VA_LIST("Vertices", "Heal radius vertices"), 48, VISUAL | SLIDER_CLAMP, 8, 64);
			CVar(HealRadiusConnectHeight, VA_LIST("Cylinder height", "Heal radius connect cylinder height"), 100.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 500.f, 5.f);
			CVar(HealRadiusDisconnectHeight, VA_LIST("Cylinder height", "Heal radius disconnect cylinder height"), 100.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 500.f, 5.f);
		NAMESPACE_END(Healing)

		NAMESPACE_BEGIN(Draw)
			// Player trajectory ghost: a duplicate of the target's own player model
			// drawn at where they will be when the local player's currently-held
			// weapon reaches them (projectile travel time, hitscan ~ latency).
			CVar(Trajectory, "Trajectory ghost", false, VISUAL);
			CVarEnum(TrajectoryTeams, "Trajectory teams", 0b01, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("Enemies", "Teammates"),
				Enemies = 1 << 0, Teammates = 1 << 1);
			CVar(TrajectoryMaterialEnemy, "Enemy material", (std::vector<std::pair<std::string, MaterialColor_t>>{ { "Flat", Color_t(255, 120, 120, 140) } }), VISUAL);
			CVar(TrajectoryMaterialTeam, "Team material", (std::vector<std::pair<std::string, MaterialColor_t>>{ { "Flat", Color_t(120, 255, 140, 140) } }), VISUAL);
			CVar(TrajectoryLeadScale, "Trajectory lead scale", 100.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 300.f, 5.f, "%g%%");
			CVar(TrajectoryOffset, "Trajectory offset", 0.f, VISUAL | SLIDER_PRECISION, -500.f, 500.f, 10.f, "%gms");
			CVar(TrajectoryMaxTime, "Trajectory max time", 1.5f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.1f, 5.f, 0.1f, "%gs");
			CVar(TrajectoryMinDistance, "Trajectory min distance", 24.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 200.f, 4.f, "%gu");
			CVar(TrajectoryFOV, "Trajectory FOV", 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 180.f, 5.f);
			CVar(TrajectoryBehindOnly, "Trajectory behind only", false, VISUAL);
			CVar(TrajectoryIgnoreZ, "Trajectory ignore Z", false, VISUAL);
			// Full glow config per team (shape + flat/distance/health colour), like
			// the ESP tab's group glow. The health gradient here also tints the
			// material ghost when Health colour is enabled.
			CVar(TrajectoryGlowEnemy, "Enemy glow", (Glow_t{ .Color = Color_t(255, 120, 120, 255) }), VISUAL);
			CVar(TrajectoryGlowTeam, "Team glow", (Glow_t{ .Color = Color_t(120, 255, 140, 255) }), VISUAL);
		NAMESPACE_END(Draw)
	NAMESPACE_END(Aimbot)
	
	NAMESPACE_BEGIN(CritHack, Crit Hack)
		CVar(ForceCrits, "Force crits", false);
		CVar(AvoidRandomCrits, "Avoid random crits", false);
		CVar(AlwaysMeleeCrit, "Always melee crit", false);
		CVar(CritEffects, "Crit effects", false);
	NAMESPACE_END(CritHack)

	NAMESPACE_BEGIN(Backtrack)
		CVar(Latency, "Fake latency", 0, SLIDER_CLAMP, 0, 1000, 5);
		CVar(Interp, "Fake interp", 0, SLIDER_CLAMP | SLIDER_PRECISION, 0, 1000, 5);
		CVar(Window, VA_LIST("Window", "Backtrack window"), 185, SLIDER_CLAMP | SLIDER_PRECISION, 0, 200, 5);
		CVar(PreferOnShot, "Prefer on shot", false);

		CVar(Offset, "Offset", 0, NOSAVE | DEBUGVAR, -1, 1);
	NAMESPACE_END(Backtrack)

	NAMESPACE_BEGIN(Doubletap)
		CVar(Doubletap, "Doubletap", false);
		CVar(Warp, "Warp", false);
		CVar(RechargeTicks, "Recharge ticks", false);
		CVar(AntiWarp, "Anti-warp", true);
		CVar(TickLimit, "Tick limit", 22, SLIDER_CLAMP, 2, 22);
		CVar(WarpRate, "Warp rate", 22, SLIDER_CLAMP, 2, 22);
		CVar(RechargeLimit, "Recharge limit", 24, SLIDER_MIN, 1, 24);
		CVar(PassiveRecharge, "Passive recharge", 0, SLIDER_CLAMP, 0, 67);
	NAMESPACE_END(DoubleTap)

	NAMESPACE_BEGIN(Fakelag)
		CVarEnum(Fakelag, "Fakelag", 0, NONE, nullptr,
			VA_LIST("Off", "Plain", "Random", "Adaptive"),
			Off, Plain, Random, Adaptive);
		CVarEnum(Options, VA_LIST("Options", "Fakelag options"), 0b000, DROPDOWN_MULTI, nullptr,
			VA_LIST("Only moving", "On unduck", "Not airborne"),
			OnlyMoving = 1 << 0, OnUnduck = 1 << 1, NotAirborne = 1 << 2);
		CVar(PlainTicks, "Plain ticks", 12, SLIDER_CLAMP, 1, 22);
		CVar(RandomTicks, "Random ticks", IntRange_t(14, 18), SLIDER_CLAMP, 1, 22, 1, "%i - %i");
		CVar(UnchokeOnAttack, "Unchoke on attack", true);
		CVar(RetainBlastJump, "Retain blastjump", false);

		CVar(RetainSoldierOnly, "Retain blastjump soldier only", true, NOSAVE | DEBUGVAR);
	NAMESPACE_END(FakeLag)

	NAMESPACE_BEGIN(AutoPeek, Auto Peek)
		CVar(Enabled, VA_LIST("Enabled", "Auto peek"), false);
	NAMESPACE_END(AutoPeek)

	NAMESPACE_BEGIN(Speedhack)
		CVar(Scale, VA_LIST("Scale", "SpeedHack scale"), 1, NONE, 1, 50);
	NAMESPACE_END(Speedhack)

	NAMESPACE_BEGIN(AntiAim, Antiaim)
		CVar(Enabled, VA_LIST("Enabled", "Antiaim enabled"), false);
		CVarEnum(PitchReal, "Real pitch", 0, NONE, nullptr,
			VA_LIST("None", "Up", "Down", "Zero", "Jitter", "Reverse jitter"),
			None, Up, Down, Zero, Jitter, ReverseJitter);
		CVarEnum(PitchFake, "Fake pitch", 0, NONE, nullptr,
			VA_LIST("None", "Up", "Down", "Jitter", "Reverse jitter"),
			None, Up, Down, Jitter, ReverseJitter);
		Enum(Yaw, Forward, Left, Right, Backwards, Edge, Jitter, Spin);
		CVarValues(YawReal, "Real yaw", 0, NONE, nullptr,
			"Forward", "Left", "Right", "Backwards", "Edge", "Jitter", "Spin");
		CVarValues(YawFake, "Fake yaw", 0, NONE, nullptr,
			"Forward", "Left", "Right", "Backwards", "Edge", "Jitter", "Spin");
		Enum(YawMode, View, Target);
		CVarValues(RealYawBase, "Real base", 0, NONE, nullptr,
			"View", "Target");
		CVarValues(FakeYawBase, "Fake base", 0, NONE, nullptr,
			"View", "Target");
		CVar(RealYawOffset, "Real offset", 0.f, SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
		CVar(FakeYawOffset, "Fake offset", 0.f, SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
		CVar(RealYawValue, "Real value", 90.f, SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
		CVar(FakeYawValue, "Fake value", -90.f, SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
		CVar(SpinSpeed, "Spin speed", 15.f, SLIDER_PRECISION, -30.f, 30.f);
		CVar(MinWalk, "Minwalk", true);
		CVar(HidePitchOnShot, "Hide pitch on shot", false);

		CVar(AntiAimLines, "Antiaim lines", false, NOSAVE);
	NAMESPACE_END(AntiAim)

	NAMESPACE_BEGIN(Resolver)
		CVar(Enabled, VA_LIST("Enabled", "Resolver enabled"), false);
		CVar(AutoResolve, "Auto resolve", false);
		CVar(AutoResolveCheatersOnly, "Auto resolve cheaters only", false);
		CVar(AutoResolveHeadshotOnly, "Auto resolve headshot only", false);
		CVar(AutoResolveYawAmount, "Auto resolve yaw", 90.f, SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 45.f);
		CVar(AutoResolvePitchAmount, "Auto resolve pitch", 90.f, SLIDER_CLAMP, -180.f, 180.f, 90.f);
		CVar(CycleYaw, "Cycle yaw", 0.f, SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 45.f);
		CVar(CyclePitch, "Cycle pitch", 0.f, SLIDER_CLAMP, -180.f, 180.f, 90.f);
		CVar(CycleView, "Cycle view", false);
		CVar(CycleMinwalk, "Cycle minwalk", false);
	NAMESPACE_END(Resolver)

	NAMESPACE_BEGIN(ESP)
		CVarValues(ActiveGroups, "Active groups", int(0b11111111111111111111111111111111), VISUAL | DROPDOWN_MULTI | DROPDOWN_NOSANITIZATION, nullptr);
	NAMESPACE_END(ESP)

	NAMESPACE_BEGIN(Visuals)
		NAMESPACE_BEGIN(UI)
			CVarEnum(StreamerMode, "Streamer mode", 0, VISUAL, nullptr,
				VA_LIST("Off", "Local", "Friends", "Party", "All"),
				Off, Local, Friends, Party, All);
			CVarEnum(ChatTags, "Chat tags", 0b000, VISUAL | DROPDOWN_MULTI, nullptr,
				VA_LIST("Local", "Friends", "Party", "Assigned"),
				Local = 1 << 0, Friends = 1 << 1, Party = 1 << 2, Assigned = 1 << 3);
			CVar(FieldOfView, "Field of view## FOV", 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 360.f, 2.5f);
			CVar(ZoomFieldOfView, "Zoomed field of view## Zoomed FOV", 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 160.f, 5.f);
			CVar(FlexFOVDebug, "Flex FOV debug## FlexFOVDebug", false, VISUAL);
			CVar(FlexFOVComposite, "Composite## FlexFOVComposite", false, VISUAL);
			CVar(FlexFOVStrength, "Flex FOV strength## FlexFOVStrength", 1.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 2.f, 0.05f);
			CVar(FlexFOVTransition, "Flex FOV transition## FlexFOVTransition", FloatRange_t(160.f, 300.f), VISUAL | SLIDER_CLAMP, 90.f, 360.f, 5.f, "%g - %g");
			CVar(FlexFOVStereographic, "Stereographic## FlexFOVStereographic", false, VISUAL);
			CVar(FlexFOVVertStereo, "Vertical stereographic## FlexFOVVertStereo", false, VISUAL);
			CVar(FlexFOVSkipMainView, "Skip main view## FlexFOVSkipMainView", true, VISUAL);
			CVar(FlexFOVQuality, "Quality## FlexFOVQuality", 1.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.35f, 1.f, 0.05f);
			// Peripheral faces re-captured round-robin, N per frame (0 = every face
			// every frame). The front face always refreshes; stale faces are
			// rotation-compensated by the composite, so only translation/animation
			// lag in the periphery. The main lever against the per-face scene cost.
			CVar(FlexFOVStagger, "Flex FOV stagger## FlexFOVStagger", 2, VISUAL, 0, 5);
			// Also re-capture the front (keystone) face only every 2nd frame. It's
			// the most expensive face; rotation compensation keeps turning smooth,
			// so the cost is one frame of animation/translation lag at the
			// crosshair on alternate frames - below typical interp latency.
			CVar(FlexFOVStaggerFront, "Stagger front## FlexFOVStaggerFront", false, VISUAL);
			// Render the non-front cube faces cheap: skip cosmetics, cull small
			// entities (projectiles/pickups/ragdolls/weapons) past 1500u, and
			// drop per-model detail (eyes/flex/jiggle, forced low LOD). The
			// front face - where the player is actually looking - stays full
			// quality. No effect on the wide (<=2 face) rig.
			CVar(FlexFOVCheapPeriphery, "Cheap periphery## FlexFOVCheapPeriphery", false, VISUAL);
			// Also skip the 3D skybox pass on cheap (non-front cube) faces: it's a
			// whole extra world render per face for distant scenery. The missing
			// skybox geometry makes a visible seam against faces that still draw
			// it, so this is off by default - for fps-starved setups at 245+ fov.
			CVar(FlexFOVCheapSky, "Cheap periphery sky## FlexFOVCheapSky", false, VISUAL);
			// Narrow each face capture to just the region the composite samples
			// (plus margin) so the engine frustum-culls the rest of the scene
			// pass; the main perf lever after face-count reduction.
			CVar(FlexFOVTightFaces, "Tight faces## FlexFOVTightFaces", true, VISUAL);
			CVar(RearView, "Enabled## RearView", false, VISUAL);
			CVar(RearViewCameras, "Cameras## RearViewCameras", 4, VISUAL, 2, 8);
			// Refresh alternating cameras each frame (each tile updates at half
			// framerate), halving the per-frame flank draw cost.
			CVar(RearViewHalfRate, "Half rate## RearViewHalfRate", true, VISUAL);
			CVar(RearViewFOVOffset, "FOV offset## RearViewFOVOffset", 0.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 1.f);
			CVar(RearViewFlipPitch, "Flip pitch## RearViewFlipPitch", false, VISUAL);
			CVar(RearViewGlowStencil, "Glow stencil## RearViewGlowStencil", 0, VISUAL, 0, 10);
			CVar(RearViewGlowBlur, "Glow blur## RearViewGlowBlur", 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f, 1.f);
			CVar(RearViewGlowColor, "Glow color## RearViewGlowColor", Color_t(255, 255, 255, 255), VISUAL);
			CVar(RearViewAlpha, "Alpha## RearViewAlpha", 0.95f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 1.f, 0.05f);
			CVar(RearViewMaterial, "Material## RearViewMaterial", (std::vector<std::pair<std::string, MaterialColor_t>>{ { "Flat", Color_t(255, 80, 90, 255) } }), VISUAL);
			CVar(ViewmodelFOV, "Viewmodel field of view## Viewmodel FOV", 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 160.f, 1.f);
			CVar(AspectRatio, "Aspect ratio", 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 5.f, 0.05f);
			CVar(RevealScoreboard, "Reveal scoreboard", false, VISUAL);
			CVar(ScoreboardUtility, "Scoreboard utility", false);
			CVar(ScoreboardColors, "Scoreboard colors", false, VISUAL);
			CVar(CleanScreenshots, "Clean screenshots", true);
		NAMESPACE_END(UI)

		NAMESPACE_BEGIN(CritBar)
			CVar(Enabled, "Crit bar", false, VISUAL);
			CVar(Text, "Crit bar text", true, VISUAL);
			CVar(Display, "Crit bar display", DragBox_t(), VISUAL | NOBIND);
			CVar(Width, "Width", 160, VISUAL | SLIDER_CLAMP, 20, 600, 5);
			CVar(Height, "Height", 14, VISUAL | SLIDER_CLAMP, 4, 80, 1);
			CVar(Border, "Border", 1, VISUAL | SLIDER_CLAMP, 0, 10, 1);
			CVar(BorderColor, "Border color", Color_t(0, 0, 0, 255), VISUAL);
			CVar(BackgroundColor, "Background color", Color_t(20, 20, 20, 180), VISUAL);
			CVar(CellColor, "Cell color", Color_t(0, 255, 100, 255), VISUAL);
			CVar(ProgressColor, "Progress color", Color_t(120, 220, 160, 255), VISUAL);
			CVar(BannedCellColor, "Banned cell color", Color_t(255, 50, 50, 255), VISUAL);
			CVar(BannedProgressColor, "Banned progress color", Color_t(255, 140, 40, 255), VISUAL);
		NAMESPACE_END(CritBar)

		NAMESPACE_BEGIN(Thirdperson)
			CVar(Enabled, "Thirdperson", false, VISUAL);
			CVar(Crosshair, VA_LIST("Crosshair", "Thirdperson crosshair"), false, VISUAL);
			CVar(Distance, "Distance", 150.f, VISUAL | SLIDER_PRECISION, 0.f, 400.f, 10.f);
			CVar(Right, "Right", 0.f, VISUAL | SLIDER_PRECISION, -100.f, 100.f, 5.f);
			CVar(Up, "Up", 0.f, VISUAL | SLIDER_PRECISION, -100.f, 100.f, 5.f);

			CVar(Scale, "Thirdperson scales", true, NOSAVE | DEBUGVAR);
			CVar(Collide, "Thirdperson collides", true, NOSAVE | DEBUGVAR);
		NAMESPACE_END(ThirdPerson)

		NAMESPACE_BEGIN(Removals)
			CVar(Interpolation, VA_LIST("Interpolation", "Remove interpolation"), false);
			CVar(Lerp, VA_LIST("Lerp", "Remove lerp"), true);
			CVar(Disguises, VA_LIST("Disguises", "Remove disguises"), false, VISUAL);
			CVar(Taunts, VA_LIST("Taunts", "Remove taunts"), false, VISUAL);
			CVar(Scope, VA_LIST("Scope", "Remove scope"), false, VISUAL);
			CVar(PostProcessing, VA_LIST("Post processing", "Remove post processing"), false, VISUAL);
			CVar(ScreenOverlays, VA_LIST("Screen overlays", "Remove screen overlays"), false, VISUAL);
			CVar(ScreenEffects, VA_LIST("Screen effects", "Remove screen effects"), false, VISUAL);
			CVar(ViewPunch, VA_LIST("View punch", "Remove view punch"), false, VISUAL);
			CVar(AngleForcing, VA_LIST("Angle forcing", "Remove angle forcing"), false, VISUAL);
			CVar(Ragdolls, VA_LIST("Ragdolls", "Remove ragdoll"), false, VISUAL);
			CVar(Gibs, VA_LIST("Gibs", "Remove gibs"), false, VISUAL);
			CVar(MOTD, VA_LIST("MOTD", "Remove MOTD"), false, VISUAL);
		NAMESPACE_END(Removals)

		NAMESPACE_BEGIN(Effects)
			CVarValues(BulletTracer, "Bullet tracer", std::string("Default"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				"Default", "None", "Big nasty", "Distortion trail", "Machina", "Sniper rail", "Short circuit", "C.A.P.P.E.R", "Merasmus ZAP", "Merasmus ZAP 2", "Black ink", "Line", "Line ignore Z", "Beam");
			CVarValues(CritTracer, "Crit tracer", std::string("Default"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				"Default", "None", "Big nasty", "Distortion trail", "Machina", "Sniper rail", "Short circuit", "C.A.P.P.E.R", "Merasmus ZAP", "Merasmus ZAP 2", "Black ink", "Line", "Line ignore Z", "Beam");
			CVarValues(MedigunBeam, "Medigun beam", std::string("Default"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				"Default", "None", "Uber", "Dispenser", "Passtime", "Bombonomicon", "White", "Orange");
			CVarValues(MedigunCharge, "Medigun charge", std::string("Default"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				"Default", "None", "Electrocuted", "Halloween", "Fireball", "Teleport", "Burning", "Scorching", "Purple energy", "Green energy", "Nebula", "Purple stars", "Green stars", "Sunbeams", "Spellbound", "Purple sparks", "Yellow sparks", "Green zap", "Yellow zap", "Plasma", "Frostbite", "Time warp", "Purple souls", "Green souls", "Bubbles", "Hearts");
			CVarValues(ProjectileTrail, "Projectile trail", std::string("Default"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				"Default", "None", "Rocket", "Critical", "Energy", "Charged", "Ray", "Fireball", "Teleport", "Fire", "Flame", "Sparks", "Flare", "Trail", "Health", "Smoke", "Bubbles", "Halloween", "Monoculus", "Sparkles", "Rainbow");
			CVarEnum(SpellFootsteps, "Spell footsteps", 0, VISUAL, nullptr,
				VA_LIST("Off", "Color", "Team", "Halloween"),
				Off, Color, Team, Halloween);
			CVarEnum(RagdollEffects, "Ragdoll effects", 0b000000, VISUAL | DROPDOWN_MULTI, nullptr,
				VA_LIST("Burning", "Electrocuted", "Ash", "Dissolve", "##Divider", "Gold", "Ice"),
				Burning = 1 << 0, Electrocuted = 1 << 1, Ash = 1 << 2, Dissolve = 1 << 3, Gold = 1 << 4, Ice = 1 << 5);
			CVar(DrawIconsThroughWalls, "Draw icons through walls", false, VISUAL);
			CVar(DrawDamageNumbersThroughWalls, "Draw damage numbers through walls", false, VISUAL);
		NAMESPACE_END(Tracers)

		NAMESPACE_BEGIN(Viewmodel)
			CVar(CrosshairAim, "Crosshair aim", false, VISUAL);
			CVar(ViewmodelAim, "Viewmodel aim", false, VISUAL);
			CVar(OffsetX, VA_LIST("Offset X", "Viewmodel offset X"), 0.f, VISUAL | SLIDER_PRECISION, -45.f, 45.f, 5.f);
			CVar(OffsetY, VA_LIST("Offset Y", "Viewmodel offset Y"), 0.f, VISUAL | SLIDER_PRECISION, -45.f, 45.f, 5.f);
			CVar(OffsetZ, VA_LIST("Offset Z", "Viewmodel offset Z"), 0.f, VISUAL | SLIDER_PRECISION, -45.f, 45.f, 5.f);
			CVar(Pitch, VA_LIST("Pitch", "Viewmodel pitch"), 0.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
			CVar(Yaw, VA_LIST("Yaw", "Viewmodel yaw"), 0.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
			CVar(Roll, VA_LIST("Roll", "Viewmodel roll"), 0.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, -180.f, 180.f, 5.f);
			CVar(SwayScale, VA_LIST("Sway scale", "Viewmodel sway scale"), 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 5.f, 0.5f);
			CVar(SwayInterp, VA_LIST("Sway interp", "Viewmodel sway interp"), 0.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 1.f, 0.1f);
		NAMESPACE_END(Viewmodel)

		NAMESPACE_BEGIN(World)
			CVarEnum(Modulations, "Modulations", 0b00000, VISUAL | DROPDOWN_MULTI, nullptr,
				VA_LIST("World", "Sky", "Prop", "Particle", "Fog"),
				World = 1 << 0, Sky = 1 << 1, Prop = 1 << 2, Particle = 1 << 3, Fog = 1 << 4);
			CVarValues(SkyboxChanger, "Skybox changer", std::string("Off"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				VA_LIST("Off"));
			CVarValues(WorldTexture, "World texture", std::string("Default"), VISUAL | DROPDOWN_CUSTOM, nullptr,
				"Default", "Dev", "Camo", "Black", "White", "Gray", "Flat");
			CVar(NearPropFade, "Near prop fade", false, VISUAL);
			CVar(NoPropFade, "No prop fade", false, VISUAL);
		NAMESPACE_END(World)

		NAMESPACE_BEGIN(Beams) // as of now, these will stay out of the menu
			CVar(Model, "Model", std::string("sprites/physbeam.vmt"), VISUAL);
			CVar(Life, "Life", 2.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
			CVar(Width, "Width", 2.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
			CVar(EndWidth, "End width", 2.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
			CVar(FadeLength, "Fade length", 10.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 30.f);
			CVar(Amplitude, "Amplitude", 2.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
			CVar(Brightness, "Brightness", 255.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 255.f);
			CVar(Speed, "Speed", 0.2f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 5.f);
			CVar(Segments, "Segments", 2, VISUAL | SLIDER_MIN, 1, 10);
			CVar(Color, "Color", Color_t(255, 255, 255, 255), VISUAL);
			CVarEnum(Flags, "Flags", 0b10000000100000000, VISUAL | DROPDOWN_MULTI, nullptr,
				VA_LIST("Start entity", "End entity", "Fade in", "Fade out", "Sine noise", "Solid", "Shade in", "Shade out", "Only noise once", "No tile", "Use hitboxes", "Start visible", "End visible", "Is active", "Forever", "Halobeam", "Reverse"),
				StartEntity = 1 << 0, EndEntity = 1 << 1, FadeIn = 1 << 2, FadeOut = 1 << 3, SineNoise = 1 << 4, Solid = 1 << 5, ShadeIn = 1 << 6, ShadeOut = 1 << 7, OnlyNoiseOnce = 1 << 8, NoTile = 1 << 9, UseHitboxes = 1 << 10, StartVisible = 1 << 11, EndVisible = 1 << 12, IsActive = 1 << 13, Forever = 1 << 14, Halobeam = 1 << 15, Reverse = 1 << 16);
		NAMESPACE_END(Beams)

		NAMESPACE_BEGIN(Line)
			CVar(TracersEnabled, "Tracers enabled", false, VISUAL);
			CVar(DrawDuration, VA_LIST("Draw duration", "Line draw duration"), 5.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
		NAMESPACE_END(Line)

		NAMESPACE_BEGIN(Hitbox)
			CVarEnum(BonesEnabled, VA_LIST("Bones enabled", "Hitbox bones enabled"), 0b00, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("On shot", "On hit"),
				OnShot = 1 << 0, OnHit = 1 << 1);
			CVarEnum(BoundsEnabled, VA_LIST("Bounds enabled", "Hitbox bounds enabled"), 0b000, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("On shot", "On hit", "Aim point"),
				OnShot = 1 << 0, OnHit = 1 << 1, AimPoint = 1 << 2);
			CVar(DrawDuration, VA_LIST("Draw duration", "Hitbox draw duration"), 5.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
		NAMESPACE_END(Hitbox)

		NAMESPACE_BEGIN(Prediction)
			CVarValues(PlayerPath, "Player path", 0, VISUAL, nullptr,
				"Off", "Line", "Separators", "Spaced", "Arrows", "Boxes");
			CVarValues(ProjectilePath, "Projectile path", 0, VISUAL, nullptr,
				"Off", "Line", "Separators", "Spaced", "Arrows", "Boxes");
			CVar(SwingLines, "Swing lines", false, VISUAL);
			CVar(PlayerDrawDuration, VA_LIST("Draw duration", "Player path draw duration"), 5.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);
			CVar(ProjectileDrawDuration, VA_LIST("Draw duration", "Projectile path draw duration"), 5.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f);

			CVarValues(RealPath, "Real path", 0, NOSAVE | DEBUGVAR, nullptr,
				"Off", "Line", "Separators", "Spaced", "Arrows", "Boxes");
		NAMESPACE_END(Prediction)

		NAMESPACE_BEGIN(Simulation)
			CVarValues(TrajectoryPath, "Trajectory path", 0, VISUAL, nullptr,
				"Off", "Line", "Separators", "Spaced", "Arrows", "Boxes");
			CVarValues(ShotPath, "Shot path", 0, VISUAL, nullptr,
				"Off", "Line", "Separators", "Spaced", "Arrows", "Boxes");
			CVarEnum(SplashRadius, "Splash radius", 0b0, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("Rockets", "Stickies", "Pipes", "Flares", "##Divider", "Conform", "Sphere"),
				Rockets = 1 << 0, Stickies = 1 << 1, Pipes = 1 << 2, Flares = 1 << 3, Trace = 1 << 4, Sphere = 1 << 5,
				Enabled = Rockets | Stickies | Pipes | Flares);
			CVarEnum(StickyRadius, "Sticky radius", 0b0, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("Local", "All", "##Divider", "Conform", "Sphere"),
				Local = 1 << 0, All = 1 << 1, Trace = 1 << 2, Sphere = 1 << 3,
				Enabled = Local | All);
			CVar(ProjectileCamera, "Projectile camera", false, VISUAL);
			CVar(ProjectileWindow, "Projectile window", WindowBox_t(), VISUAL | NOBIND);
			CVar(Box, VA_LIST("Box", "Path box"), true, VISUAL);
		NAMESPACE_END(Simulation)

		NAMESPACE_BEGIN(SentryRange)
			CVarEnum(Draw, "Sentry range", 0b0, VISUAL | DROPDOWN_MULTI, "Off",
				VA_LIST("Enemy", "Team", "Local", "##Divider", "Disabled", "Building"),
				Enemy = 1 << 0, Team = 1 << 1, Local = 1 << 2, Disabled = 1 << 3, Building = 1 << 4,
				Enabled = Enemy | Team | Local);
			CVarValues(Style, "Sentry range style", 3, VISUAL, nullptr,
				"Off", "Fill", "Edges", "Fill + Edges");
			CVar(GridStep, VA_LIST("Grid step", "Sentry range grid step"), 50.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 1.f, 96.f, 2.f);
			CVar(Smoothing, VA_LIST("Smoothing", "Sentry range corner smoothing (fill + edges)"), 0.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 5.f, "%g%%");
			CVar(GroundOffset, VA_LIST("Ground offset", "Sentry range ground offset"), 4.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 32.f, 1.f);
			CVar(MaxDistance, VA_LIST("Max distance", "Sentry range max distance"), 4000.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.f, 8000.f, 250.f);
			CVar(RefreshInterval, VA_LIST("Refresh interval", "Sentry range refresh interval"), 1.f, VISUAL | SLIDER_MIN | SLIDER_PRECISION, 0.25f, 5.f, 0.25f, "%gs");
			CVar(DisabledAlpha, VA_LIST("Disabled alpha", "Sentry range disabled alpha"), 40.f, VISUAL | SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 100.f, 5.f, "%g%%");

			CVar(TraceBudget, "Sentry range trace budget", 300, NOSAVE | DEBUGVAR | SLIDER_MIN, 16, 2048);
			CVar(TargetHeight, "Sentry range target height", 68.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 83.f, 1.f);
		NAMESPACE_END(SentryRange)

		NAMESPACE_BEGIN(Path)
			Enum(Style, Off, Line, Separators, Spaced, Arrows, Boxes);

			CVar(SeparatorSpacing, "Separator spacing", 4, NOSAVE | DEBUGVAR, 1, 16);
			CVar(SeparatorLength, "Separator length", 12.f, NOSAVE | DEBUGVAR, 2.f, 16.f);
		NAMESPACE_END(Path)

		NAMESPACE_BEGIN(Trajectory)
			CVar(Override, "Simulation override", false, NOSAVE | DEBUGVAR);
			CVar(Type, "Type", std::string("custom"), NOSAVE | DEBUGVAR);
			CVar(OffsetX, "Offset X", 23.5f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -25.f, 25.f, 0.5f);
			CVar(OffsetY, "Offset Y", 12.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -25.f, 25.f, 0.5f);
			CVar(OffsetZ, "Offset Z", -3.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -25.f, 25.f, 0.5f);
			CVar(ForwardRedirect, "Forward redirect", 2000.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 2000.f, 100.f);
			CVar(ForwardCutoff, "Forward cutoff", 0.1f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 1.f, 0.1f);
			CVar(Hull, "Hull", 0.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f, 0.5f);
			CVar(Speed, "Speed", 1100.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 5000.f, 50.f);
			CVar(Gravity, "Gravity", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 800.f, 100.f);
			CVar(LifeTime, "Life time", 10.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 10.f, 0.1f);
			CVar(UpVelocity, "Up velocity", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 1000.f, 50.f);
			CVar(AngularVelocityX, "Angular velocity X", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -1000.f, 1000.f, 50.f);
			CVar(AngularVelocityY, "Angular velocity Y", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -1000.f, 1000.f, 50.f);
			CVar(AngularVelocityZ, "Angular velocity Z", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, -1000.f, 1000.f, 50.f);
			CVar(Drag, "Drag", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 2.f, 0.1f);
			CVar(DragX, "Drag X", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 0.1f, 0.01f, "%.15g");
			CVar(DragY, "Drag Y", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 0.1f, 0.01f, "%.15g");
			CVar(DragZ, "Drag Z", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 0.1f, 0.01f, "%.15g");
			CVar(AngularDragX, "Angular drag X", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 0.1f, 0.01f, "%.15g");
			CVar(AngularDragY, "Angular drag Y", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 0.1f, 0.01f, "%.15g");
			CVar(AngularDragZ, "Angular drag Z", 0.f, NOSAVE | DEBUGVAR | SLIDER_PRECISION, 0.f, 0.1f, 0.01f, "%.15g");
			CVar(MaxVelocity, "Max velocity", 0.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 4000.f, 50.f);
			CVar(MaxAngularVelocity, "Max angular velocity", 0.f, NOSAVE | DEBUGVAR | SLIDER_MIN | SLIDER_PRECISION, 0.f, 7200.f, 50.f);
		NAMESPACE_END(ProjectileTrajectory)
	NAMESPACE_END(Visuals)

	NAMESPACE_BEGIN(Misc)
		NAMESPACE_BEGIN(Movement)
			CVarEnum(AutoStrafe, "Auto strafe", 0, NONE, nullptr,
				VA_LIST("Off", "Legit", "Directional"),
				Off, Legit, Directional);
			CVar(AutoStrafeTurnScale, VA_LIST("Turn scale", "Auto strafe turn scale"), 0.5f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 1.f, 0.1f);
			CVar(AutoStrafeMaxDelta, VA_LIST("Max delta", "Auto strafe max delta"), 180.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.f, 180.f, 5.f);
			CVar(Bunnyhop, "Bunnyhop", false);
			CVar(EdgeJump, "Edge jump", false);
			CVar(AirCrouch, "Air crouch", false);
			CVar(AutoJumpbug, "Auto jumpbug", false);
			CVar(BreakJump, "Break jump", false);
			CVar(AutoRocketJump, "Auto rocket jump", false);
			CVar(AutoCTap, "Auto ctap", false);
			CVar(AutoFaNJump, "Auto FaN jump", false);
			CVar(AutoRevJump, "Auto rev jump", false);
			CVar(FastStop, "Fast stop", false);
			CVar(FastAccelerate, "Fast accelerate", false);
			CVar(DuckSpeed, "Duck speed", false);
			CVar(ShieldTurnRate, "Shield turn rate", false);
			CVar(NoPush, "No push", false);
			CVar(MovementLock, "Movement lock", false);

			CVar(AutoRocketJumpChokeGrounded, "Choke grounded", 1, NOSAVE | DEBUGVAR, 0, 3);
			CVar(AutoRocketJumpChokeAir, "Choke air", 1, NOSAVE | DEBUGVAR, 0, 3);
			CVar(AutoRocketJumpSkipGround, "Skip grounded", 0, NOSAVE | DEBUGVAR, 0, 3);
			CVar(AutoRocketJumpSkipAir, "Skip air", 1, NOSAVE | DEBUGVAR, 0, 3);
			CVar(AutoRocketJumpTimingOffset, "Timing offset", 0, NOSAVE | DEBUGVAR, 0, 3);
			CVar(AutoRocketJumpApplyAbove, "Apply offset above", 0, NOSAVE | DEBUGVAR, 0, 10);
		NAMESPACE_END(Movement)

		NAMESPACE_BEGIN(Automation)
			CVarEnum(AntiBackstab, "Anti-backstab", 0, NONE, nullptr,
				VA_LIST("Off", "Yaw", "Pitch", "Fake"),
				Off, Yaw, Pitch, Fake);
			CVar(TauntControl, "Taunt control", false);
			CVar(KartControl, "Kart control", false);
			CVar(AntiAutobalance, "Anti-autobalance", false);
			CVar(AntiAFK, "Anti-AFK", false);
			CVar(AutoF2Ignored, "Auto F2 ignored", false);
			CVar(AutoF1Priority, "Auto F1 priority", false);
			CVar(AcceptItemDrops, "Auto accept item drops", false);
		NAMESPACE_END(Automation)

		NAMESPACE_BEGIN(Exploits)
			CVar(PureBypass, "Pure bypass", false);
			CVar(CheatsBypass, "Cheats bypass", false);
			CVar(UnlockCVars, "Unlock CVars", false);
			CVar(EquipRegionUnlock, "Equip region unlock", false);
			CVar(BackpackExpander, "Backpack expander", false);
			CVar(NoisemakerSpam, "Noisemaker spam", false);
			CVar(PingReducer, "Ping reducer", false);
			CVar(PingTarget, "cl_cmdrate", 1, SLIDER_CLAMP, 1, 66);
		NAMESPACE_END(Exploits)

		NAMESPACE_BEGIN(Game)
			CVar(NetworkFix, "Network fix", false);
			CVar(SetupBonesOptimization, "Bones optimization", false);
			// Memoize CAttributeManager::AttribHookValue results for the rest of
			// the frame. Item attributes are static per item, so within one frame
			// the lookup is pure - but the engine (and our own weapon/aim logic)
			// re-walks the attribute lists hundreds of times per frame (~2-3% of
			// frame time on a full server per vprof).
			CVar(AttributeCacheOptimization, "Attribute cache optimization", true);
			// Skip drawing wearables (cosmetics) past this distance, 0 = off. A hat
			// at 2500u is a few pixels, but still a full model draw with bone
			// merging per render pass - and every extra pass (FlexFOV faces,
			// rearview flanks) multiplies it. Players and weapons are untouched.
			CVar(CosmeticCullDistance, "Cosmetic cull distance", 2500, SLIDER_CLAMP, 0, 10000);
			// Groups whose visible chams are just the untouched "Original" layer
			// skip the suppress-and-redraw round trip: the engine's own scene
			// draw is kept (stencil-marked so occluded layers still mask against
			// it), saving a full model draw per entity per render pass.
			CVar(OriginalChamsOptimization, "Original chams optimization", true);
			// Resolution of the glow silhouette/blur buffers as a fraction of the
			// screen (and of the FlexFOV glow buffers). Below 1 trades outline
			// crispness for fill rate - the halo is blurred anyway; above 1
			// supersamples it. At exactly 1 the screen glow keeps the engine's
			// shared depth-stencil, which lets a single-batch glow config stamp
			// its halo mask during the silhouette pass and skip the whole
			// stencil-stamp model pass (one draw per glowing entity per frame).
			CVar(GlowResolution, "Glow resolution", 1.f, SLIDER_CLAMP | SLIDER_PRECISION, 0.3f, 1.5f, 0.1f);
			CVar(AntiCheatCompatibility, "Anti-cheat compatibility", false);

			CVar(AntiCheatCritHack, "Anti-cheat crit hack", false, NOSAVE | DEBUGVAR);
		NAMESPACE_END(Game)

		NAMESPACE_BEGIN(Queueing)
			CVarEnum(ForceRegions, "Force regions", 0b0, DROPDOWN_MULTI, nullptr, // i'm not sure all of these are actually used for tf2 servers
				VA_LIST("Atlanta", "Chicago", "Dallas", "Los Angeles", "Seattle", "Virginia", "##Divider", "Amsterdam", "Falkenstein", "Frankfurt", "Helsinki", "London", "Madrid", "Paris", "Stockholm", "Vienna", "Warsaw", "##Divider", "Buenos Aires", "Lima", "Santiago", "Sao Paulo", "##Divider", "Chennai", "Dubai", "Hong Kong", "Mumbai", "Seoul", "Singapore", "Tokyo", "##Divider", "Sydney", "##Divider", "Johannesburg"),
				// North America
				ATL = 1 << 0, // Atlanta
				ORD = 1 << 1, // Chicago
				DFW = 1 << 2, // Dallas
				LAX = 1 << 3, // Los Angeles
				SEA = 1 << 4, // Seattle (+DC_EAT?)
				IAD = 1 << 5, // Virginia
				// Europe
				AMS = 1 << 6, // Amsterdam
				FSN = 1 << 7, // Falkenstein
				FRA = 1 << 8, // Frankfurt
				HEL = 1 << 9, // Helsinki
				LHR = 1 << 10, // London
				MAD = 1 << 11, // Madrid
				PAR = 1 << 12, // Paris
				STO = 1 << 13, // Stockholm
				VIE = 1 << 14, // Vienna
				WAW = 1 << 15, // Warsaw
				// South America
				EZE = 1 << 16, // Buenos Aires
				LIM = 1 << 17, // Lima
				SCL = 1 << 18, // Santiago
				GRU = 1 << 19, // Sao Paulo
				// Asia
				MAA = 1 << 20, // Chennai
				DXB = 1 << 21, // Dubai
				HKG = 1 << 22, // Hong Kong
				BOM = 1 << 23, // Mumbai
				SEO = 1 << 24, // Seoul
				SGP = 1 << 25, // Singapore
				TYO = 1 << 26, // Tokyo
				// Australia
				SYD = 1 << 27, // Sydney
				// Africa
				JNB = 1 << 28, // Johannesburg
			);
			CVar(ExtendQueue, "Extend queue", false);
			CVar(AutoCasualQueue, "Auto casual queue", false);
		NAMESPACE_END(Queueing)

		NAMESPACE_BEGIN(MannVsMachine, Mann vs. Machine)
			CVar(InstantRespawn, "Instant respawn", false);
			CVar(InstantRevive, "Instant revive", false);
			CVar(AllowInspect, "Allow inspect", false);
		NAMESPACE_END(Sound)

		NAMESPACE_BEGIN(Sound)
			CVarEnum(Block, VA_LIST("Block", "Sound block"), 0b0000, DROPDOWN_MULTI, nullptr,
				VA_LIST("Footsteps", "Noisemaker", "Frying pan", "Water"),
				Footsteps = 1 << 0, Noisemaker = 1 << 1, FryingPan = 1 << 2, Water = 1 << 3);
			CVar(HitsoundAlways, "Hitsound always", false);
			CVar(RemoveDSP, "Remove DSP", false);
			CVar(GiantWeaponSounds, "Giant weapon sounds", false);
		NAMESPACE_END(Sound)
	NAMESPACE_END(Misc)

	NAMESPACE_BEGIN(Logging)
		CVarEnum(Logs, "Logs", 0b0000011, DROPDOWN_MULTI, "Off",
			VA_LIST("Vote start", "Vote cast", "Class changes", "Damage", "Cheat detection", "Tags", "Aliases", "Resolver"),
			VoteStart = 1 << 0, VoteCast = 1 << 1, ClassChanges = 1 << 2, Damage = 1 << 3, CheatDetection = 1 << 4, Tags = 1 << 5, Aliases = 1 << 6, Resolver = 1 << 7);
		Enum(LogTo, Toasts = 1 << 0, Chat = 1 << 1, Party = 1 << 2, Console = 1 << 3, Menu = 1 << 4, Debug = 1 << 5);
		CVarEnum(NotificationPosition, "Notification position", 0, VISUAL, nullptr,
			VA_LIST("Top left", "Top right", "Bottom left", "Bottom right"),
			TopLeft, TopRight, BottomLeft, BottomRight);
		CVar(NotificationTime, "Notification time", 5.f, VISUAL, 0.5f, 5.f, 0.5f);
		CVar(MaxNotifications, "Max notifications", 10, VISUAL | SLIDER_MIN, 1, 10);

		NAMESPACE_BEGIN(VoteStart, Logging)
			CVarValues(LogTo, "Vote start log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(VoteStart)

		NAMESPACE_BEGIN(VoteCast, Logging)
			CVarValues(LogTo, "Vote cast log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(VoteCast)

		NAMESPACE_BEGIN(ClassChange, Logging)
			CVarValues(LogTo, "Class change log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(ClassChange)

		NAMESPACE_BEGIN(Damage, Logging)
			CVarValues(LogTo, "Damage log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(Damage)

		NAMESPACE_BEGIN(CheatDetection, Logging)
			CVarValues(LogTo, "Cheat detection log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(CheatDetection)

		NAMESPACE_BEGIN(Tags, Logging)
			CVarValues(LogTo, "Tags log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(Tags)

		NAMESPACE_BEGIN(Aliases, Logging)
			CVarValues(LogTo, "Aliases log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(Aliases)

		NAMESPACE_BEGIN(Resolver, Logging)
			CVarValues(LogTo, "Resolver log to", 0b000001, DROPDOWN_MULTI, nullptr,
				"Toasts", "Chat", "Party", "Console", "Menu", "Debug");
		NAMESPACE_END(Resolver)
	NAMESPACE_END(Logging)

	NAMESPACE_BEGIN(CheatDetection, Cheat Detection)
		CVarEnum(Methods, "Detection methods", 0b0000, DROPDOWN_MULTI, nullptr,
			VA_LIST("Invalid pitch", "Packet choking", "Aim flicking", "Duck Speed"),
			InvalidPitch = 1 << 0, PacketChoking = 1 << 1, AimFlicking = 1 << 2, DuckSpeed = 1 << 3);
		CVar(DetectionsRequired, "Detections required", 10, SLIDER_MIN, 0, 50);
		CVar(MinChoking, "Min choking", 20, SLIDER_MIN, 4, 22);
		CVar(MinFlick, "Min flick angle", 20.f, SLIDER_PRECISION, 10.f, 30.f); // min flick size to suspect
		CVar(MaxNoise, "Max flick noise", 1.f, SLIDER_PRECISION, 1.f, 10.f); // max difference between angles before and after flick
	NAMESPACE_END(CheatDetection)

	NAMESPACE_BEGIN(Debug)
		CVar(Info, "Debug info", false, NOSAVE);
		CVar(Logging, "Debug logging", false, NOSAVE);
		CVar(Options, "Debug options", false, NOSAVE);
		CVar(CrashLogging, "Crash logging", true, NOBIND);

#ifdef DEBUG_TRACES
		CVar(VisualizeTraces, "Visualize traces", false, NOSAVE);
		CVar(VisualizeTraceHits, "Visualize trace hits", false, NOSAVE);
#endif

		CVar(DrawHitboxes, "Show hitboxes", false, NOSAVE);
		CVar(AutoVprof, "Auto vprof", false, NOBIND);
	NAMESPACE_END(Debug)
NAMESPACE_END(Vars)