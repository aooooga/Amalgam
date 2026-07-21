#include "Render.h"

#include "../../Hooks/Direct3DDevice9.h"
#include <ImGui/imgui_impl_win32.h>
#include "Fonts/MaterialDesign/MaterialIcons.h"
#include "Fonts/MaterialDesign/IconDefinitions.h"
#include "Fonts/CascadiaMono/CascadiaMono.h"
#include "Fonts/Roboto/RobotoMedium.h"
#include "Fonts/Roboto/RobotoBlack.h"
#include "Menu/Menu.h"
#include "Menu/Components.h"
#include "Notifications/Notifications.h"
#include "../Binds/Binds.h"

void CRender::Render(IDirect3DDevice9* pDevice)
{
	static std::once_flag tFlag; std::call_once(tFlag, [&]
	{
		Initialize(pDevice);
	});

	// Menu toggle keys are polled outside the ImGui frame so the frame itself
	// can be skipped: with the menu closed and nothing ImGui-drawn pending
	// (binds display, notifications), NewFrame/Render/RenderDrawData are a fixed
	// per-Present cost for an empty draw list - pure frame time (= input
	// latency) for nothing. ImGui is immediate-mode, so resuming after skipped
	// frames is seamless.
	F::Menu.HandleToggle();
	if (!F::Menu.m_bIsOpen && !F::Notifications.Active()
		&& !(F::Binds.m_bDisplay && !F::Binds.m_vBinds.empty()))
		return;

	LoadColors();
	{
		static float flStaticScale = Vars::Menu::Scale.Value;
		float flOldScale = flStaticScale;
		float flNewScale = flStaticScale = Vars::Menu::Scale.Value;
		if (flNewScale != flOldScale)
			Reload();
	}

	DWORD dwOldRGB; pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &dwOldRGB);
	pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, false);
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();

	F::Menu.Render();

	ImGui::EndFrame();
	ImGui::Render();
	ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
	pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, dwOldRGB);
}

void CRender::LoadColors()
{
	using namespace ImGui;

	// Phase 5: the theme colours only change when the user edits them, but the
	// derived table + ImGui style colours were rebuilt every frame. Skip the
	// rebuild while the four source colours are unchanged.
	{
		auto uPack = [](Color_t c) -> uint32_t { return uint32_t(c.r) << 24 | uint32_t(c.g) << 16 | uint32_t(c.b) << 8 | uint32_t(c.a); };
		uint32_t uNew = uPack(Vars::Menu::Theme::Accent.Value) * 2654435761u
					  ^ uPack(Vars::Menu::Theme::Background.Value) * 40503u
					  ^ uPack(Vars::Menu::Theme::Inactive.Value) * 131u
					  ^ uPack(Vars::Menu::Theme::Active.Value);
		// the per-step panel overrides feed the same derived table, so they must
		// invalidate this cache too or edits won't take until another colour moves
		uNew = uNew * 16777619u ^ uint32_t(Vars::Menu::Theme::BackgroundOverride.Value);
		uNew = uNew * 16777619u ^ uint32_t(Vars::Menu::Theme::AccentOverride.Value) * 3u;
		for (auto* pOverride : { &Vars::Menu::Theme::Background0, &Vars::Menu::Theme::Background0p5,
								 &Vars::Menu::Theme::Background1, &Vars::Menu::Theme::Background1p5,
								 &Vars::Menu::Theme::Background2,
								 &Vars::Menu::Theme::TextDim, &Vars::Menu::Theme::TextDisabled,
								 &Vars::Menu::Theme::AccentMuted, &Vars::Menu::Theme::AccentWashed,
								 &Vars::Menu::Theme::WindowBackground, &Vars::Menu::Theme::NavBackground,
								 &Vars::Menu::Theme::NavDivider,
								 &Vars::Menu::Theme::PanelBackground, &Vars::Menu::Theme::PanelHeader,
								 &Vars::Menu::Theme::PanelBorder, &Vars::Menu::Theme::PanelAccent,
								 &Vars::Menu::Theme::PanelTitle,
								 &Vars::Menu::Theme::PanelCollapsedBackground,
								 &Vars::Menu::Theme::PanelCollapsedHeader,
								 &Vars::Menu::Theme::PanelCollapsedTitle,
								 &Vars::Menu::Theme::RowDivider, &Vars::Menu::Theme::SubGroupText,
								 &Vars::Menu::Theme::SubGroupRule,
								 &Vars::Menu::Theme::ControlBackground, &Vars::Menu::Theme::ControlHovered,
								 &Vars::Menu::Theme::SwitchOn, &Vars::Menu::Theme::SwitchOff,
								 &Vars::Menu::Theme::SwitchKnobOn, &Vars::Menu::Theme::SwitchKnobOff,
								 &Vars::Menu::Theme::SliderTrack, &Vars::Menu::Theme::SliderFill,
								 &Vars::Menu::Theme::SliderKnob, &Vars::Menu::Theme::SliderValueText,
								 &Vars::Menu::Theme::TabActive, &Vars::Menu::Theme::TabInactive,
								 &Vars::Menu::Theme::TabBar,
								 &Vars::Menu::Theme::PopupBackground,
								 &Vars::Menu::Theme::TooltipBackground, &Vars::Menu::Theme::TooltipText })
			uNew = uNew * 16777619u ^ uPack(pOverride->Value);
		static uint32_t uCache = 0;
		static bool bFirst = true;
		if (!bFirst && uNew == uCache)
			return;
		bFirst = false;
		uCache = uNew;
	}

	Accent = ColorByteToFloat(Vars::Menu::Theme::Accent.Value);
	// Each ramp step is the Lerp-derived shade off Background unless the user has
	// enabled overrides, in which case the per-step colour is used verbatim.
	// The override colour is NOT alpha-gated: a user who deliberately sets a step
	// fully transparent must get a transparent panel, and an alpha-0 default is
	// indistinguishable from "unset" in the picker. Unset steps are instead seeded
	// with the derived shade below, so enabling overrides changes nothing at first.
	auto Derived = [](float flStep) -> Color_t
	{
		return flStep == 0.f
			? Vars::Menu::Theme::Background.Value
			: Vars::Menu::Theme::Background.Value.Lerp({ 127, 127, 127 }, flStep / 9, LerpEnum::NoAlpha);
	};
	auto Ramp = [&](ConfigVar<Color_t>& tOverride, float flStep) -> ImColor
	{
		if (!Vars::Menu::Theme::BackgroundOverride.Value)
			return ColorByteToFloat(Derived(flStep));
		return ColorByteToFloat(tOverride.Value);
	};

	// Seed any still-unset (all-zero) override with its derived shade, so the
	// pickers open showing the current colours instead of transparent black.
	for (auto& [pVar, flStep] : std::initializer_list<std::pair<ConfigVar<Color_t>*, float>>{
			{ &Vars::Menu::Theme::Background0,   0.f }, { &Vars::Menu::Theme::Background0p5, 0.5f },
			{ &Vars::Menu::Theme::Background1,   1.f }, { &Vars::Menu::Theme::Background1p5, 1.5f },
			{ &Vars::Menu::Theme::Background2,   2.f } })
	{
		auto& tVal = pVar->Value;
		if (!tVal.r && !tVal.g && !tVal.b && !tVal.a)
			tVal = Derived(flStep);
	}
	Background0 = Ramp(Vars::Menu::Theme::Background0, 0.f);
	Background0p5 = Ramp(Vars::Menu::Theme::Background0p5, 0.5f);
	Background1 = Ramp(Vars::Menu::Theme::Background1, 1.f);
	Background1p5 = Ramp(Vars::Menu::Theme::Background1p5, 1.5f);
	Background1p5L = { Background1p5.Value.x * 1.1f, Background1p5.Value.y * 1.1f, Background1p5.Value.z * 1.1f, Background1p5.Value.w };
	Background2 = Ramp(Vars::Menu::Theme::Background2, 2.f);
	Inactive = ColorByteToFloat(Vars::Menu::Theme::Inactive.Value);
	Active = ColorByteToFloat(Vars::Menu::Theme::Active.Value);

	// Phase 1: raise base label contrast without hardcoding (theme-derived).
	// Nudge the muted "inactive" text toward the active text so off-state labels
	// read more clearly, and build a mid tier for subgroup headers.
	auto Mix = [](const ImColor& a, const ImColor& b, float t) -> ImColor
	{
		return { a.Value.x + (b.Value.x - a.Value.x) * t,
				 a.Value.y + (b.Value.y - a.Value.y) * t,
				 a.Value.z + (b.Value.z - a.Value.z) * t,
				 a.Value.w + (b.Value.w - a.Value.w) * t };
	};
	Inactive = Mix(Inactive, Active, 0.22f);
	TextDim = Mix(Inactive, Active, 0.55f);

	// Optional per-element overrides. Alpha 0 means "unset" -> keep the derived
	// value, so an untouched theme behaves exactly as before.
	auto Override = [](ConfigVar<Color_t>& tVar, const ImColor& tDerived) -> ImColor
	{
		return tVar.Value.a ? ColorByteToFloat(tVar.Value) : tDerived;
	};
	TextDim = Override(Vars::Menu::Theme::TextDim, TextDim);
	TextDisabled = Override(Vars::Menu::Theme::TextDisabled, Mix(Inactive, Background2, 0.4f));

	// Accent variants: the muted/washed alpha steps the widgets use.
	ImColor tMuted = Accent, tWashed = Accent;
	tMuted.Value.w *= 0.8f, tWashed.Value.w *= 0.4f;
	bool bAccentOverride = Vars::Menu::Theme::AccentOverride.Value;
	AccentMuted = bAccentOverride ? Override(Vars::Menu::Theme::AccentMuted, tMuted) : tMuted;
	AccentWashed = bAccentOverride ? Override(Vars::Menu::Theme::AccentWashed, tWashed) : tWashed;

	// Window chrome. WindowBackground and PanelHeader used to both resolve to
	// Background1, which is why editing the panel header moved the page background
	// too -- they are independent surfaces now.
	WindowBackground = Override(Vars::Menu::Theme::WindowBackground, Background1);
	NavBackground = Override(Vars::Menu::Theme::NavBackground, Background0);
	NavDivider = Override(Vars::Menu::Theme::NavDivider, Background2);

	// Panels.
	PanelBackground = Override(Vars::Menu::Theme::PanelBackground, Background0p5);
	PanelHeader = Override(Vars::Menu::Theme::PanelHeader, Background1);
	PanelBorder = Override(Vars::Menu::Theme::PanelBorder, Background2);
	PanelAccent = Override(Vars::Menu::Theme::PanelAccent, Accent);
	PanelTitle = Override(Vars::Menu::Theme::PanelTitle, Accent);
	PanelCollapsedBackground = Override(Vars::Menu::Theme::PanelCollapsedBackground, Background0);
	PanelCollapsedHeader = Override(Vars::Menu::Theme::PanelCollapsedHeader, Background0);
	PanelCollapsedTitle = Override(Vars::Menu::Theme::PanelCollapsedTitle, Accent);
	// Row dividers were pinned to Background1p5, the same var as control fills.
	RowDivider = Override(Vars::Menu::Theme::RowDivider, Background1p5);
	SubGroupText = Override(Vars::Menu::Theme::SubGroupText, TextDim);
	SubGroupRule = Override(Vars::Menu::Theme::SubGroupRule, Background2);

	// Controls.
	ControlBackground = Override(Vars::Menu::Theme::ControlBackground, Background1p5);
	ControlHovered = Override(Vars::Menu::Theme::ControlHovered, Background1p5L);
	SwitchOn = Override(Vars::Menu::Theme::SwitchOn, Accent);
	SwitchOff = Override(Vars::Menu::Theme::SwitchOff, Background2);
	SwitchKnobOn = Override(Vars::Menu::Theme::SwitchKnobOn, Background0);
	SwitchKnobOff = Override(Vars::Menu::Theme::SwitchKnobOff, Active);
	SliderTrack = Override(Vars::Menu::Theme::SliderTrack, Background2);
	SliderFill = Override(Vars::Menu::Theme::SliderFill, Accent);
	SliderKnob = Override(Vars::Menu::Theme::SliderKnob, SliderFill);
	SliderValueText = Override(Vars::Menu::Theme::SliderValueText, Active);

	// Tabs.
	TabActive = Override(Vars::Menu::Theme::TabActive, Active);
	TabInactive = Override(Vars::Menu::Theme::TabInactive, Inactive);
	TabBar = Override(Vars::Menu::Theme::TabBar, Accent);

	// Popups / tooltips.
	PopupBackground = Override(Vars::Menu::Theme::PopupBackground, Background1p5L);
	TooltipBackground = Override(Vars::Menu::Theme::TooltipBackground, Background1p5);
	TooltipText = Override(Vars::Menu::Theme::TooltipText, Active);

	ImVec4* colors = GetStyle().Colors;
	colors[ImGuiCol_Border] = PanelBorder;
	colors[ImGuiCol_Button] = {};
	colors[ImGuiCol_ButtonHovered] = {};
	colors[ImGuiCol_ButtonActive] = {};
	colors[ImGuiCol_FrameBg] = ControlBackground;
	colors[ImGuiCol_FrameBgHovered] = ControlHovered;
	colors[ImGuiCol_FrameBgActive] = ControlBackground;
	colors[ImGuiCol_Header] = {};
	colors[ImGuiCol_HeaderHovered] = { Background1p5L.Value.x * 1.1f, Background1p5L.Value.y * 1.1f, Background1p5L.Value.z * 1.1f, Background1p5.Value.w }; // divd by 1.1
	colors[ImGuiCol_HeaderActive] = Background1p5;
	colors[ImGuiCol_ModalWindowDimBg] = { Background0.Value.x, Background0.Value.y, Background0.Value.z, 0.4f };
	colors[ImGuiCol_PopupBg] = PopupBackground;
	colors[ImGuiCol_ResizeGrip] = {};
	colors[ImGuiCol_ResizeGripActive] = {};
	colors[ImGuiCol_ResizeGripHovered] = {};
	colors[ImGuiCol_ScrollbarBg] = {};
	colors[ImGuiCol_Text] = Active;
	colors[ImGuiCol_WindowBg] = {};
}

void CRender::LoadFonts()
{
	using namespace ImGui;

	auto& io = GetIO();

	if (static bool bLoaded = false; !bLoaded)
		bLoaded = true;
	else
		io.Fonts->Clear();

	ImFontConfig tFontConfig;
	tFontConfig.OversampleH = 2;
#ifndef AMALGAM_CUSTOM_FONTS
	FontSmall = io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\verdana.ttf)", H::Draw.Scale(11), &tFontConfig);
	FontRegular = io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\verdana.ttf)", H::Draw.Scale(13), &tFontConfig);
	FontBold = io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\verdanab.ttf)", H::Draw.Scale(13), &tFontConfig);
	FontLarge = io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\verdana.ttf)", H::Draw.Scale(14), &tFontConfig);
	FontTitle = io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\verdanab.ttf)", H::Draw.Scale(16), &tFontConfig);
	FontMono = io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\cour.ttf)", H::Draw.Scale(16), &tFontConfig); // windows mono font installed by default
#else
	FontSmall = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoMedium_compressed_data, RobotoMedium_compressed_size, H::Draw.Scale(12), &tFontConfig);
	FontRegular = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoMedium_compressed_data, RobotoMedium_compressed_size, H::Draw.Scale(13), &tFontConfig);
	FontBold = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoBlack_compressed_data, RobotoBlack_compressed_size, H::Draw.Scale(13), &tFontConfig);
	FontLarge = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoMedium_compressed_data, RobotoMedium_compressed_size, H::Draw.Scale(15), &tFontConfig);
	FontTitle = io.Fonts->AddFontFromMemoryCompressedTTF(RobotoBlack_compressed_data, RobotoBlack_compressed_size, H::Draw.Scale(16), &tFontConfig);
	FontMono = io.Fonts->AddFontFromMemoryCompressedTTF(CascadiaMono_compressed_data, CascadiaMono_compressed_size, H::Draw.Scale(15), &tFontConfig);
#endif

	ImFontConfig tIconConfig;
	tIconConfig.PixelSnapH = true;
	IconFont = io.Fonts->AddFontFromMemoryCompressedTTF(MaterialIcons_compressed_data, MaterialIcons_compressed_size, H::Draw.Scale(16), &tIconConfig);

	io.Fonts->Build();
	io.ConfigDebugHighlightIdConflicts = false;
}

void CRender::LoadStyle()
{
	using namespace ImGui;

	auto& style = GetStyle();
	style.ButtonTextAlign = { 0.5f, 0.5f };
	style.CellPadding = { H::Draw.Scale(4), 0 };
	style.ChildBorderSize = 0.f;
	style.ChildRounding = H::Draw.Scale(4);
	style.FrameBorderSize = 0.f;
	style.FramePadding = { 0, 0 };
	style.FrameRounding = H::Draw.Scale(4);
	style.ItemInnerSpacing = { 0, 0 };
	style.ItemSpacing = { H::Draw.Scale(8), H::Draw.Scale(Tokens::SectionGutter) }; // Phase 1: wider section gutters
	style.PopupBorderSize = 0.f;
	style.PopupRounding = H::Draw.Scale(4);
	style.ScrollbarSize = 6.f + H::Draw.Scale(3);
	style.ScrollbarRounding = 0.f;
	style.WindowBorderSize = 0.f;
	style.WindowPadding = { 0, 0 };
	style.WindowRounding = H::Draw.Scale(4);
}

void CRender::Initialize(IDirect3DDevice9* pDevice)
{
	ImGui::CreateContext();
	ImGui_ImplWin32_Init(WndProc::hwWindow);
	ImGui_ImplDX9_Init(pDevice);

	auto& io = ImGui::GetIO();
	//io.IniFilename = nullptr;
	io.LogFilename = nullptr;

	LoadFonts();
	LoadStyle();

	m_bLoaded = true;
}

void CRender::Reload()
{
	m_bLoaded = false;

	LoadFonts();
	LoadStyle();

	m_bLoaded = true;
}