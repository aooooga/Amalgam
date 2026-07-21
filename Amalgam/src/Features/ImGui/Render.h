#pragma once
#include "../../SDK/SDK.h"
#include <ImGui/imgui_impl_dx9.h>
#include <ImGui/imgui.h>

class CRender
{
public:
	void Render(IDirect3DDevice9* pDevice);
	void Initialize(IDirect3DDevice9* pDevice);
	void Reload();

	void LoadColors();
	void LoadFonts();
	void LoadStyle();

	int Cursor = 2;

	// Colors
	ImColor Accent = {};
	ImColor Background0 = {};
	ImColor Background0p5 = {};
	ImColor Background1 = {};
	ImColor Background1p5 = {};
	ImColor Background1p5L = {};
	ImColor Background2 = {};
	ImColor Inactive = {};
	ImColor Active = {};
	ImColor TextDim = {}; // Phase 1: mid tier between Inactive and Active for subgroup labels
	ImColor TextDisabled = {};

	// Accent variants (derived from Accent unless overridden)
	ImColor AccentMuted = {};
	ImColor AccentWashed = {};

	// Window chrome
	ImColor WindowBackground = {};
	ImColor NavBackground = {};
	ImColor NavDivider = {};

	// Panels
	ImColor PanelBackground = {};
	ImColor PanelHeader = {};
	ImColor PanelBorder = {};
	ImColor PanelAccent = {};
	ImColor PanelTitle = {};
	ImColor PanelCollapsedBackground = {};
	ImColor PanelCollapsedHeader = {};
	ImColor PanelCollapsedTitle = {};
	ImColor RowDivider = {};
	ImColor SubGroupText = {};
	ImColor SubGroupRule = {};

	// Controls
	ImColor ControlBackground = {};
	ImColor ControlHovered = {};
	ImColor SwitchOn = {};
	ImColor SwitchOff = {};
	ImColor SwitchKnobOn = {};
	ImColor SwitchKnobOff = {};
	ImColor SliderTrack = {};
	ImColor SliderFill = {};
	ImColor SliderKnob = {};
	ImColor SliderValueText = {};

	// Tabs
	ImColor TabActive = {};
	ImColor TabInactive = {};
	ImColor TabBar = {};

	// Popups / tooltips
	ImColor PopupBackground = {};
	ImColor TooltipBackground = {};
	ImColor TooltipText = {};

	// Fonts
	ImFont* FontSmall = nullptr;
	ImFont* FontRegular = nullptr;
	ImFont* FontBold = nullptr;
	ImFont* FontLarge = nullptr;
	ImFont* FontTitle = nullptr; // Phase 3 redesign: larger bold for section headers
	ImFont* FontMono = nullptr;

	ImFont* IconFont = nullptr;

	bool m_bLoaded = false;
};

ADD_FEATURE(CRender, Render);