#include "Menu.h"

#include <mutex>
#include "Components.h"
#include "Menu3a_Widgets.h"
#include "../Notifications/Notifications.h"
#include "../../Configs/Configs.h"
#include "../../Binds/Binds.h"
#include "../../Visuals/Groups/Groups.h"
#include "../../Players/PlayerUtils.h"
#include "../../Spectate/Spectate.h"
#include "../../Resolver/Resolver.h"
#include "../../Visuals/Visuals.h"
#include "../../Misc/Misc.h"
#include "../../Output/Output.h"
#include "../../World/World.h"
#include "../../Simulation/ProjectileSimulation/ProjectileSimulation.h"
#include "../../Debug/AutoVprof/AutoVprof.h"
#include "../../../Utils/Perf/Tracker.h"

// Phase 4 help system: curated descriptions for the genuinely cryptic options.
// Keyed by ConfigVar address (compile-checked) so no Vars.h line needs touching.
// WRAPPER auto-shows these as an FTooltip after a short hover dwell.
static void InitDescriptions()
{
	auto& m = ImGui::g_Descriptions;
	auto Add = [&](const void* p, const char* s) { m[p] = s; };

	// Flex FOV cluster
	Add(&Vars::Visuals::UI::FlexFOVStrength, "How strongly the wide-angle Flex FOV lens warps the view. 0 = off, 1 = default, higher exaggerates the fisheye.");
	Add(&Vars::Visuals::UI::FlexFOVTransition, "The FOV range over which the flat center blends into the warped edges. Lower = a smaller flat sweet-spot.");
	Add(&Vars::Visuals::UI::FlexFOVStereographic, "Use stereographic (conformal) projection instead of the default. Keeps shapes locally round at the cost of more edge stretch.");
	Add(&Vars::Visuals::UI::FlexFOVVertStereo, "Apply the stereographic curve on the vertical axis too, not just horizontally.");
	Add(&Vars::Visuals::UI::FlexFOVComposite, "Composite the warped edge passes back over the main view instead of replacing it.");
	Add(&Vars::Visuals::UI::FlexFOVSkipMainView, "Skip re-rendering the flat center view when Flex FOV owns the whole frame. Cheaper; disable if the center looks wrong.");
	Add(&Vars::Visuals::UI::FlexFOVQuality, "Internal render scale for the Flex FOV edge passes. Below 1 trades edge sharpness for frame time.");
	Add(&Vars::Visuals::UI::FlexFOVStagger, "Spread the extra Flex FOV face passes across multiple frames to cut per-frame cost. Higher = cheaper but more edge lag.");
	Add(&Vars::Visuals::UI::FlexFOVStaggerFront, "Include the forward-facing pass in the stagger rotation instead of always drawing it every frame.");

	// Exploits
	Add(&Vars::Misc::Exploits::PureBypass, "Defeat sv_pure so custom/replacement game files load on pure servers.");
	Add(&Vars::Misc::Exploits::CheatsBypass, "Enable sv_cheats-gated commands locally without the server having cheats on.");
	Add(&Vars::Misc::Exploits::UnlockCVars, "Remove the hidden/cheat flags on console variables so they can be changed.");
	Add(&Vars::Misc::Exploits::EquipRegionUnlock, "Bypass equip-region conflicts so normally-incompatible cosmetics can be worn together.");
	Add(&Vars::Misc::Exploits::PingReducer, "Lowers your reported command rate to reduce ping. May feel less responsive.");

	// Game optimizations
	Add(&Vars::Misc::Game::NetworkFix, "Smooths choppy networking on unstable connections.");
	Add(&Vars::Misc::Game::SetupBonesOptimization, "Cache and reuse skeleton bone setups within a frame instead of recomputing per system.");
	Add(&Vars::Misc::Game::AttributeCacheOptimization, "Memoize item-attribute lookups for the rest of each frame. Attributes are static per item, so this is a pure win (~2-3% frame time on full servers).");
	Add(&Vars::Misc::Game::CosmeticCullDistance, "Stop drawing other players' cosmetics past this distance (0 = never). Players and weapons are unaffected; saves a full model draw per pass.");
	Add(&Vars::Misc::Game::OriginalChamsOptimization, "For groups whose only visible chams are the plain 'Original' layer, keep the engine's own draw instead of a suppress-and-redraw round trip.");
	Add(&Vars::Misc::Game::GlowResolution, "Resolution of the glow silhouette/blur buffers as a fraction of the screen. Below 1 is cheaper (the halo is blurred anyway); at exactly 1 a single-batch glow skips an entire model pass.");
	Add(&Vars::Misc::Game::AntiCheatCompatibility, "Reduce detectable behaviour for stricter anti-cheat environments.");

	// Doubletap / fakelag HVH cluster
	Add(&Vars::Doubletap::Doubletap, "Fire two usable commands in one server tick by charging then releasing shifted ticks. The core 'double tap' exploit.");
	Add(&Vars::Doubletap::Warp, "Bank ticks while held, then release them in a burst to teleport a short distance instantly.");
	Add(&Vars::Doubletap::AntiWarp, "Keep your recharge/tick bank stable so warp and doubletap stay usable without desync.");
	Add(&Vars::Doubletap::RechargeTicks, "Actively rebuild the shifted-tick bank between shots.");
	Add(&Vars::Doubletap::WarpRate, "How many ticks are spent per warp burst. Higher = a longer teleport, drained faster.");

	// Clearer on-screen display labels (config/bind keys are untouched).
	auto& L = ImGui::g_DisplayLabels;
	auto Label = [&](const void* p, const char* s) { L[p] = s; };
	Label(&Vars::Misc::Exploits::PureBypass, "Bypass sv_pure");
	Label(&Vars::Misc::Exploits::CheatsBypass, "Bypass sv_cheats");
	Label(&Vars::Misc::Exploits::UnlockCVars, "Unlock console variables");
	Label(&Vars::Misc::Exploits::EquipRegionUnlock, "Unlock equip regions");
	Label(&Vars::Misc::Game::NetworkFix, "Network smoothing");
	Label(&Vars::Misc::Game::SetupBonesOptimization, "Bone-setup cache");
	Label(&Vars::Misc::Game::AttributeCacheOptimization, "Attribute cache");
	Label(&Vars::Misc::Game::OriginalChamsOptimization, "Original-chams fast path");
	Label(&Vars::Misc::Game::CosmeticCullDistance, "Cull cosmetics past");
	Label(&Vars::Misc::Game::GlowResolution, "Glow buffer resolution");
	Label(&Vars::Visuals::UI::FlexFOVStagger, "Stagger passes");
	Label(&Vars::Visuals::UI::FlexFOVStaggerFront, "Stagger front pass");
	Label(&Vars::Visuals::UI::FlexFOVTightFaces, "Tight face culling");
	Label(&Vars::Visuals::UI::FlexFOVCheapPeriphery, "Cheap periphery");
	Label(&Vars::Visuals::UI::FlexFOVCheapSky, "Cheap sky");
	Label(&Vars::Visuals::UI::FlexFOVQuality, "Edge render scale");
	Label(&Vars::Doubletap::Warp, "Warp (teleport)");
}

// Gradient editor for the glow health-color stops: the shared stop editor
// (ImGui::FGradientStops) spanning 0-100% HP.
static void FGlowGradient(const char* sLabel, std::vector<GlowStop_t>& vStops)
{
	ImGui::FGradientStops(sLabel, vStops, 0.f, 0.f, 100.f, "%.0f%%");
}

void CMenu::DrawMenu()
{
	using namespace ImGui;

	if (static bool bSetPosition = false; !bSetPosition)
	{
		SetNextWindowPos((GetIO().DisplaySize - ImVec2(H::Draw.Scale(750), H::Draw.Scale(500))) / 2, ImGuiCond_FirstUseEver);
		SetNextWindowSize({ H::Draw.Scale(750), H::Draw.Scale(500) }, ImGuiCond_FirstUseEver);
		bSetPosition = true;
	}

	PushStyleVar(ImGuiStyleVar_WindowMinSize, { H::Draw.Scale(750), H::Draw.Scale(500) });
	if (Begin("Main", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground))
	{
		ImVec2 vWindowSize = GetWindowSize();
		ImVec2 vDrawPos = GetDrawPos();
		auto pDrawList = GetWindowDrawList();
		// Top navigation bar: title/bind block on the left, the two tab rows stacked
		// beneath it, search pinned to the right of the title band.
		float flSize = H::Draw.Scale(72);        // total height of the nav band
		float flTitleH = H::Draw.Scale(36);      // upper band (title + search)
		float flInset = H::Draw.Scale();
		// Title block and search box shrink as the window narrows so the tab row keeps
		// usable width; both are clamped so they stay legible. Recomputed every frame,
		// so resizing the menu reflows the bar instead of pushing tabs off the edge.
		float flTitleW = std::clamp(vWindowSize.x * 0.20f, H::Draw.Scale(96), H::Draw.Scale(200));
		float flSearchW = std::clamp(vWindowSize.x * 0.18f, H::Draw.Scale(90), H::Draw.Scale(170));

		Bind_t tBind;
		if (!F::Binds.GetBind(CurrentBind, &tBind))
			CurrentBind = DEFAULT_BIND;

		// Whole-window background: nav band on top, page body below, with the border
		// tone between them. This paints the page area too -- without it every panel
		// renders over nothing and the menu reads as transparent.
		pDrawList->PushClipRect({ 0, 0 }, { GetIO().DisplaySize.x, GetIO().DisplaySize.y }, false);
		RenderTwoToneBackground(flSize, F::Render.NavBackground, F::Render.WindowBackground, F::Render.NavDivider);
		pDrawList->PopClipRect();

		// divider between the title band and the tab row
		pDrawList->AddRectFilled(vDrawPos + ImVec2(flInset, flTitleH - flInset), vDrawPos + ImVec2(vWindowSize.x - flInset, flTitleH), F::Render.NavDivider);

		if (CurrentBind != DEFAULT_BIND) // bind
		{
			SetCursorPos({ H::Draw.Scale(12), H::Draw.Scale(10) });
			PushStyleColor(ImGuiCol_Text, F::Render.Accent.Value);
			FText(TruncateText(std::format("Editing bind: {}", tBind.m_sName), flTitleW - H::Draw.Scale(40), F::Render.FontBold).c_str(), {}, 0, F::Render.FontBold);
			PopStyleColor();

			SetCursorPos({ flTitleW - H::Draw.Scale(31), H::Draw.Scale(4) });
			if (IconButton(ICON_MD_CANCEL))
				CurrentBind = DEFAULT_BIND;
		}
		else if (!Vars::Menu::CheatTitle.Value.empty()) // title
		{
			SetCursorPos({ H::Draw.Scale(12), H::Draw.Scale(10) });
			PushStyleColor(ImGuiCol_Text, F::Render.Accent.Value);
			FText(TruncateText(Vars::Menu::CheatTitle.Value, flTitleW - H::Draw.Scale(24), F::Render.FontBold).c_str(), {}, 0, F::Render.FontBold);
			PopStyleColor();
		}

		static int iTab = 0, iAimbotTab = 0, iVisualsTab = 0, iMiscTab = 0, iLogsTab = 0, iSettingsTab = 0;

		// Search sits at the right end of the title band.
		static std::string sSearch = "";
		SetCursorPos({ vWindowSize.x - flSearchW - H::Draw.Scale(8), H::Draw.Scale(6) });
		FInputText("Search...", sSearch, flSearchW - H::Draw.Scale(17), ImGuiInputTextFlags_None);
		bool bSearch = !sSearch.empty();
		if (!bSearch || FCalcTextSize(sSearch.c_str()).x < flSearchW - H::Draw.Scale(37))
		{
			SetCursorPos({ vWindowSize.x - H::Draw.Scale(29), H::Draw.Scale(12) });
			IconImage(ICON_MD_SEARCH);
		}

		// Main tabs occupy the title band between the title block and the search box;
		// sub-tabs get the lower band, full width.
		// NOTE: with FTabsEnum::Fit, vSize.x is per-tab PADDING added to the measured
		// text width -- not the width of the whole row. Passing the row width here
		// makes every tab that wide and pushes all but the first off screen.
		PushFont(F::Render.FontBold);
		FTabs(
			std::vector<const char*>{ "AIMBOT", "HVH", "VISUALS", "MISC", "LOGS", "SETTINGS" },
			&iTab,
			{ H::Draw.Scale(26), flTitleH - H::Draw.Scale(6) },
			{ flTitleW, H::Draw.Scale(3) },
			FTabsEnum::Horizontal | FTabsEnum::HorizontalIcons | FTabsEnum::AlignCenter | FTabsEnum::BarBottom | FTabsEnum::Fit,
			std::vector<const char*>{ ICON_MD_PERSON, ICON_MD_BOLT, ICON_MD_VISIBILITY, ICON_MD_ARTICLE, ICON_MD_IMPORT_CONTACTS, ICON_MD_SETTINGS },
			{ H::Draw.Scale(8), 0 }, {},
			{}, { H::Draw.Scale(18), 0 }
		);

		// Sub-tabs for the active main tab (HVH has none).
		static const std::vector<std::vector<const char*>> vSubTabs =
		{
			{ "GENERAL", "DRAW" },
			{},
			{ "ESP", "VIEW", "WORLD", "MENU" },
			{ "PLAYER", "GAME", "MISC##" },
			{ "PLAYERLIST", "SETTINGS##", "OUTPUT" },
			{ "CONFIG", "BINDS", "MATERIALS", "MISC##" }
		};
		int* pSubTabs[] = { &iAimbotTab, nullptr, &iVisualsTab, &iMiscTab, &iLogsTab, &iSettingsTab };
		if (iTab >= 0 && iTab < int(vSubTabs.size()) && !vSubTabs[iTab].empty())
		{
			FTabs(
				vSubTabs[iTab],
				pSubTabs[iTab],
				{ H::Draw.Scale(20), flSize - flTitleH - H::Draw.Scale(6) },
				{ flTitleW, flTitleH + H::Draw.Scale(3) },
				FTabsEnum::Horizontal | FTabsEnum::AlignCenter | FTabsEnum::BarBottom | FTabsEnum::Fit,
				{}, {}, {}, {}, {}
			);
		}
		PopFont();

		if (bSearch && IsMouseReleased(ImGuiMouseButton_Left) && IsMouseWithin(vDrawPos.x, vDrawPos.y, vWindowSize.x, flSize))
			sSearch = "";

		SetCursorPos({ 0, flSize });
		PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
		PushStyleVar(ImGuiStyleVar_WindowPadding, { H::Draw.Scale(8), H::Draw.Scale(8) });
		if (BeginChild("Page", { vWindowSize.x, vWindowSize.y - flSize }, ImGuiChildFlags_AlwaysUseWindowPadding))
		{
			if (!bSearch)
			{
				switch (iTab)
				{
				case 0: MenuAimbot(iAimbotTab); break;
				case 1: MenuHVH(); break;
				case 2: MenuVisuals(iVisualsTab); break;
				case 3: MenuMisc(iMiscTab); break;
				case 4: MenuLogs(iLogsTab); break;
				case 5: MenuSettings(iSettingsTab); break;
				}
			}
			else
				MenuSearch(sSearch);
		} EndChild();
		PopStyleVar(2);

		End();
	}
	PopStyleVar();
}

#pragma region Tabs
void CMenu::MenuAimbot(int iTab)
{
	using namespace ImGui;

	switch (iTab)
	{
	// General
	case 0:
	{
		if (BeginTable("AimbotTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("General"))
				{
					FDropdown(Vars::Aimbot::General::AimType, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::General::TargetSelection, FDropdownEnum::Right);
					FDropdown(Vars::Aimbot::General::Target, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::General::Ignore, FDropdownEnum::Right);
					FSliderRow(Vars::Aimbot::General::AimFOV);
					FSliderRow(Vars::Aimbot::General::MaxTargets, FSliderEnum::Left);
					PushTransparent(!(Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Invisible));
					{
						FSliderRow(Vars::Aimbot::General::IgnoreInvisible, FSliderEnum::Right);
					}
					PopTransparent();
					FSliderRow(Vars::Aimbot::General::AssistStrength, FSliderEnum::Left);
					PushTransparent(!(Vars::Aimbot::General::Ignore.Value & Vars::Aimbot::General::IgnoreEnum::Unsimulated));
					{
						FSliderRow(Vars::Aimbot::General::TickTolerance, FSliderEnum::Right);
					}
					PopTransparent();
					FColorPicker(Vars::Colors::FOVCircle);
					FToggleRow(Vars::Aimbot::General::AutoShoot, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::General::FOVCircle, FToggleEnum::Right);
					FToggleRow(Vars::Aimbot::General::LeadAndRestrict, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::General::NoSpread, FToggleEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Aimbot"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }, -8))
						{
							FDropdown(Vars::Aimbot::General::AimHoldsFire);
							FSliderRow(Vars::Aimbot::General::NoSpreadOffset);
							FSliderRow(Vars::Aimbot::General::NoSpreadAverage);
							FSliderRow(Vars::Aimbot::General::NoSpreadInterval);
							FSliderRow(Vars::Aimbot::General::NoSpreadBackupInterval);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Backtrack", 8))
				{
					FSliderRow(Vars::Backtrack::Latency);
					FSliderRow(Vars::Backtrack::Interp);
					FSliderRow(Vars::Backtrack::Window);
					//FToggleRow(Vars::Backtrack::PreferOnShot);
				} EndSection();
				if (Section("Crit Hack", 8))
				{
					FToggleRow(Vars::CritHack::ForceCrits, FToggleEnum::Left);
					FToggleRow(Vars::CritHack::AvoidRandomCrits, FToggleEnum::Right);
					FToggleRow(Vars::CritHack::AlwaysMeleeCrit, FToggleEnum::Left);
					FToggleRow(Vars::CritHack::CritEffects, FToggleEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Backtrack"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FSliderRow(Vars::Backtrack::Offset);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Healing"))
				{
					FDropdown(Vars::Aimbot::Healing::HealPriority, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::Healing::DangerIgnore, FDropdownEnum::Right);

					FToggleRow(Vars::Aimbot::Healing::AutoHeal, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::Healing::AutoArrow, FToggleEnum::Right);
					FToggleRow(Vars::Aimbot::Healing::AutoRepair, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::Healing::AutoSandvich, FToggleEnum::Right);
					FToggleRow(Vars::Aimbot::Healing::AutoVaccinator, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::Healing::ActivateOnVoice, FToggleEnum::Right);

					FDropdown(Vars::Aimbot::Healing::HealRadius);
					FToggleRow(Vars::Aimbot::Healing::HealRadiusHeal);
					FSliderRow(Vars::Aimbot::Healing::HealRadiusVertices, FSliderEnum::Left);
					FSliderRow(Vars::Aimbot::Healing::HealRadiusRounding, FSliderEnum::Right);
					FSliderRow(Vars::Aimbot::Healing::HealRadiusHeight);

					// each range gets its own popup: the ring's edge and fill (ignore Z
					// left, Z-buffered right), then the standard glow block
					FText("Connect", { 5, 5 });
					if (FPopupButton("Connect", { 0, -5 }))
					{
						FColorPicker(Vars::Colors::HealRadiusConnectIgnoreZ, FColorPickerEnum::Left);
						FColorPicker(Vars::Colors::HealRadiusConnect, FColorPickerEnum::Right);
						FColorPicker(Vars::Colors::HealRadiusConnectFillIgnoreZ, FColorPickerEnum::Left);
						FColorPicker(Vars::Colors::HealRadiusConnectFill, FColorPickerEnum::Right);

						Divider();
						{
							auto& tGlow = Vars::Aimbot::Healing::HealRadiusGlowConnect[DEFAULT_BIND];
							FSlider("Stencil scale## HealRadiusConnect", &tGlow.Stencil, 0, 10, 1, "%i", FSliderEnum::Left | FSliderEnum::Min);
							FSlider("Blur scale## HealRadiusConnect", &tGlow.Blur, 0.f, 10.f, 1.f, "%g", FSliderEnum::Right | FSliderEnum::Min | FSliderEnum::Precision);
							FColorPicker("Glow color## HealRadiusConnect", &tGlow.Color, FColorPickerEnum::Left);
						}

						EndPopup();
					}

					FText("Disconnect", { 5, 5 });
					if (FPopupButton("Disconnect", { 0, -5 }))
					{
						FColorPicker(Vars::Colors::HealRadiusDisconnectIgnoreZ, FColorPickerEnum::Left);
						FColorPicker(Vars::Colors::HealRadiusDisconnect, FColorPickerEnum::Right);
						FColorPicker(Vars::Colors::HealRadiusDisconnectFillIgnoreZ, FColorPickerEnum::Left);
						FColorPicker(Vars::Colors::HealRadiusDisconnectFill, FColorPickerEnum::Right);

						Divider();
						{
							auto& tGlow = Vars::Aimbot::Healing::HealRadiusGlowDisconnect[DEFAULT_BIND];
							FSlider("Stencil scale## HealRadiusDisconnect", &tGlow.Stencil, 0, 10, 1, "%i", FSliderEnum::Left | FSliderEnum::Min);
							FSlider("Blur scale## HealRadiusDisconnect", &tGlow.Blur, 0.f, 10.f, 1.f, "%g", FSliderEnum::Right | FSliderEnum::Min | FSliderEnum::Precision);
							FColorPicker("Glow color## HealRadiusDisconnect", &tGlow.Color, FColorPickerEnum::Left);
						}

						EndPopup();
					}
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Healing"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FSliderRow(Vars::Aimbot::Healing::AutoVaccinatorBulletScale);
							FSliderRow(Vars::Aimbot::Healing::AutoVaccinatorBlastScale);
							FSliderRow(Vars::Aimbot::Healing::AutoVaccinatorFireScale);
							FToggleRow(Vars::Aimbot::Healing::AutoVaccinatorFlamethrowerDamageOnly);

							EndPopup();
						}
					} EndSection();
				}
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Hitscan"))
				{
					FDropdown(Vars::Aimbot::Hitscan::Hitboxes, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::Hitscan::MultipointHitboxes, FDropdownEnum::Right);
					FDropdown(Vars::Aimbot::Hitscan::Modifiers);
					FSliderRow(Vars::Aimbot::Hitscan::MultipointScale);
					PushTransparent(!(Vars::Aimbot::Hitscan::Modifiers.Value & Vars::Aimbot::Hitscan::ModifiersEnum::Tapfire));
					{
						FSliderRow(Vars::Aimbot::Hitscan::TapfireDistance);
					}
					PopTransparent();
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Hitscan"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }, -8))
						{
							FDropdown(Vars::Aimbot::Hitscan::PeekCheck, FDropdownEnum::None, 0, &Hovered); FTooltip("This should stay as doubletap only or off if you want to be able to target hitboxes other than the highest priority one", Hovered);
							FSliderRow(Vars::Aimbot::Hitscan::PeekAmount);
							FSliderRow(Vars::Aimbot::Hitscan::BoneSizeSubtract);
							FSliderRow(Vars::Aimbot::Hitscan::BoneSizeMinimumScale);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Projectile"))
				{
					FDropdown(Vars::Aimbot::Projectile::StrafePrediction, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::Projectile::SplashPrediction, FDropdownEnum::Right);
					FDropdown(Vars::Aimbot::Projectile::AutoDetonate, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::Projectile::AutoAirblast, FDropdownEnum::Right);
					FDropdown(Vars::Aimbot::Projectile::Hitboxes, FDropdownEnum::Left);
					FDropdown(Vars::Aimbot::Projectile::Modifiers, FDropdownEnum::Right);

					SubGroup("Tuning");
					FSliderRow(Vars::Aimbot::Projectile::MaxSimulationTime, FSliderEnum::Left);
					PushTransparent(!Vars::Aimbot::Projectile::StrafePrediction.Value);
					{
						FSliderRow(Vars::Aimbot::Projectile::HitChance, FSliderEnum::Right);
					}
					PopTransparent();
					FSliderRow(Vars::Aimbot::Projectile::AutodetRadius, FSliderEnum::Left);
					FSliderRow(Vars::Aimbot::Projectile::SplashRadius, FSliderEnum::Right);
					PushTransparent(!Vars::Aimbot::Projectile::AutoRelease.Value);
					{
						FSliderRow(Vars::Aimbot::Projectile::AutoRelease);
					}
					PopTransparent();

					SubGroup("Stickies");
					FToggleRow(Vars::Aimbot::Projectile::DoubleSticky, FToggleEnum::Left);
					FColorPicker(Vars::Colors::DoubleStickyPath, FColorPickerEnum::SameLine);
					FKeybind(Vars::Aimbot::Projectile::DoubleStickyKey, FButtonEnum::Right | FButtonEnum::SameLine, { Vars::Menu::PrimaryKey[DEFAULT_BIND], Vars::Menu::SecondaryKey[DEFAULT_BIND] });
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Projectile"))
					{
						FText("General", { 5, 5 });
						if (FPopupButton("General", { 0, -5 }))
						{
							FSliderRow(Vars::Aimbot::Projectile::VelocityAverageCount, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::VerticalShift, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::DragOverride, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::TimeOverride, FSliderEnum::Right);
							FToggleRow(Vars::Aimbot::Projectile::LobAnglesUnderpredict);

							Divider();
							FSliderRow(Vars::Aimbot::Projectile::HuntsmanLerp, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::HuntsmanLerpLow, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::HuntsmanAdd, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::HuntsmanAddLow, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::HuntsmanClamp, FSliderEnum::Left);
							FToggleRow(Vars::Aimbot::Projectile::HuntsmanPullPoint, FSliderEnum::Left);
							FToggleRow(Vars::Aimbot::Projectile::HuntsmanPullNoZ, FToggleEnum::Right);

							EndPopup();
						}

						FText("Splash", { 5, 5 });
						if (FPopupButton("Splash", { 0, -5 }, -8))
						{
							FDropdown(Vars::Aimbot::Projectile::SplashMode);
							PushTransparent(Vars::Aimbot::Projectile::SplashMode.Value != Vars::Aimbot::Projectile::SplashModeEnum::Trace);
							{
								FSliderRow(Vars::Aimbot::Projectile::SplashPointsDirect, FSliderEnum::Left);
								FSliderRow(Vars::Aimbot::Projectile::SplashPointsArc, FSliderEnum::Right);
								FSliderRow(Vars::Aimbot::Projectile::SplashRotateX, FSliderEnum::Left, Vars::Aimbot::Projectile::SplashRotateX[DEFAULT_BIND] < 0.f ? "random" : "%g");
								FSliderRow(Vars::Aimbot::Projectile::SplashRotateY, FSliderEnum::Right, Vars::Aimbot::Projectile::SplashRotateY[DEFAULT_BIND] < 0.f ? "random" : "%g");
							}
							PopTransparent();
							PushTransparent(Vars::Aimbot::Projectile::SplashMode.Value != Vars::Aimbot::Projectile::SplashModeEnum::Face);
							{
								FSliderRow(Vars::Aimbot::Projectile::SplashDensityDirect, FSliderEnum::Left);
								FSliderRow(Vars::Aimbot::Projectile::SplashDensityArc, FSliderEnum::Right);
								FSliderRow(Vars::Aimbot::Projectile::SplashSamplesCutoff);
							}
							PopTransparent();
							FSliderRow(Vars::Aimbot::Projectile::SplashAirCount, FSliderEnum::None, !Vars::Aimbot::Projectile::SplashAirCount[DEFAULT_BIND] ? "random" : "%i");

							Divider();
							FSliderRow(Vars::Aimbot::Projectile::SplashRestrictDirect);
							FSliderRow(Vars::Aimbot::Projectile::SplashRestrictArc);
							FSliderRow(Vars::Aimbot::Projectile::SplashRestrictFirst);

							Divider();
							FSliderRow(Vars::Aimbot::Projectile::DirectTraceInterval);
							FSliderRow(Vars::Aimbot::Projectile::SplashTraceInterval);
							FSliderRow(Vars::Aimbot::Projectile::LobTraceInterval);
							FToggleRow(Vars::Aimbot::Projectile::IntervalRetest);

							EndPopup();
						}

						FText("Ground", { 5, 5 });
						if (FPopupButton("Ground", { 0, -5 }))
						{
							FSliderRow(Vars::Aimbot::Projectile::GroundSamples, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::GroundStraightFuzzyValue, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::GroundLowMinimumSamples, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::GroundHighMinimumSamples, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::GroundLowMinimumDistance, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::GroundHighMinimumDistance, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::GroundMaxChanges, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::GroundMaxChangeTime, FSliderEnum::Right);

							EndPopup();
						}

						FText("Air", { 5, 5 });
						if (FPopupButton("Air", { 0, -5 }))
						{
							FSliderRow(Vars::Aimbot::Projectile::AirSamples, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::AirStraightFuzzyValue, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::AirLowMinimumSamples, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::AirHighMinimumSamples, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::AirLowMinimumDistance, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::AirHighMinimumDistance, FSliderEnum::Right);
							FSliderRow(Vars::Aimbot::Projectile::AirMaxChanges, FSliderEnum::Left);
							FSliderRow(Vars::Aimbot::Projectile::AirMaxChangeTime, FSliderEnum::Right);

							EndPopup();
						}

						FText("Misc", { 5, 5 });
						if (FPopupButton("Misc", { 0, -5 }))
						{
							FSliderRow(Vars::Aimbot::Projectile::DeltaCount, FSliderEnum::Left);
							FDropdown(Vars::Aimbot::Projectile::DeltaMode, FDropdownEnum::Right);
							FDropdown(Vars::Aimbot::Projectile::MovesimFrictionFlags);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Melee", 8))
				{
					FToggleRow(Vars::Aimbot::Melee::AutoBackstab, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::Melee::IgnoreRazorback, FToggleEnum::Right);
					FToggleRow(Vars::Aimbot::Melee::SwingPrediction, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::Melee::WhipTeam, FToggleEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Melee"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FSliderRow(Vars::Aimbot::Melee::SwingTicks, FSliderEnum::Left);
							FToggleRow(Vars::Aimbot::Melee::SwingPredictLag, FToggleEnum::Right);
							FDropdown(Vars::Aimbot::Melee::SwingValidateMode, FDropdownEnum::Left);
							FDropdown(Vars::Aimbot::Melee::BackstabFlags, FDropdownEnum::Right);

							EndPopup();
						}
					} EndSection();
				}
			}
			EndTable();
		}
		break;
	}
	// Draw
	case 1:
	{
		if (BeginTable("DrawTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Line", 8))
				{
					FColorPicker(Vars::Colors::LineIgnoreZ, FColorPickerEnum::None, { 0, H::Draw.Scale(6) }, { H::Draw.Scale(12), H::Draw.Scale(6) });
					FColorPicker(Vars::Colors::Line, FColorPickerEnum::None, {}, { H::Draw.Scale(12), H::Draw.Scale(6) });
					FToggleRow(Vars::Visuals::Line::TracersEnabled);
					FSliderRow(Vars::Visuals::Line::DrawDuration);
				} EndSection();
				if (Section("Hitbox"))
				{
					FDropdown(Vars::Visuals::Hitbox::BonesEnabled, FDropdownEnum::None, -20);
					FColorPicker(Vars::Colors::TargetHitboxEdgeIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(30) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::TargetHitboxEdge, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::BoneHitboxEdgeIgnoreZ, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::BoneHitboxEdge, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::TargetHitboxFaceIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(30) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::TargetHitboxFace, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::BoneHitboxFaceIgnoreZ, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::BoneHitboxFace, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					SameLine(); DebugDummy({ 2, H::Draw.Scale(48) });

					FDropdown(Vars::Visuals::Hitbox::BoundsEnabled, FDropdownEnum::None, -20);
					FColorPicker(Vars::Colors::BoundHitboxEdgeIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::BoundHitboxEdge, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::BoundHitboxFaceIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, {H::Draw.Scale(10), H::Draw.Scale(20)});
					FColorPicker(Vars::Colors::BoundHitboxFace, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });

					FSliderRow(Vars::Visuals::Hitbox::DrawDuration);
				} EndSection();
				if (Section("Trajectory"))
				{
					FToggleRow(Vars::Aimbot::Draw::Trajectory);
					FDropdown(Vars::Aimbot::Draw::TrajectoryTeams);
					FToggleRow(Vars::Aimbot::Draw::TrajectoryBehindOnly, FToggleEnum::Left);
					FToggleRow(Vars::Aimbot::Draw::TrajectoryIgnoreZ, FToggleEnum::Right);

					SubGroup("Lead");
					FSliderRow(Vars::Aimbot::Draw::TrajectoryLeadScale, FSliderEnum::Left);
					FSliderRow(Vars::Aimbot::Draw::TrajectoryOffset, FSliderEnum::Right);
					FSliderRow(Vars::Aimbot::Draw::TrajectoryMaxTime, FSliderEnum::Left);
					FSliderRow(Vars::Aimbot::Draw::TrajectoryMinDistance, FSliderEnum::Right);
					FSliderRow(Vars::Aimbot::Draw::TrajectoryFOV);

					SubGroup("Enemy");
					FMDropdown(Vars::Aimbot::Draw::TrajectoryMaterialEnemy);
					{
						auto& tGlow = Vars::Aimbot::Draw::TrajectoryGlowEnemy[DEFAULT_BIND];
						FSlider("Stencil scale## TrajectoryEnemy", &tGlow.Stencil, 0, 10, 1, "%i", FSliderEnum::Left | FSliderEnum::Min);
						FSlider("Blur scale## TrajectoryEnemy", &tGlow.Blur, 0.f, 10.f, 1.f, "%g", FSliderEnum::Right | FSliderEnum::Min | FSliderEnum::Precision);
						FColorPicker("Enemy color", &tGlow.Color, &tGlow.DistanceColor, FColorPickerEnum::Left);
						FToggle("Health color## TrajectoryEnemy", &tGlow.HealthColor, FToggleEnum::Right);
						if (tGlow.HealthColor)
							FGlowGradient("TrajectoryEnemyHealthGradient", tGlow.Stops);
					}

					SubGroup("Team");
					FMDropdown(Vars::Aimbot::Draw::TrajectoryMaterialTeam);
					{
						auto& tGlow = Vars::Aimbot::Draw::TrajectoryGlowTeam[DEFAULT_BIND];
						FSlider("Stencil scale## TrajectoryTeam", &tGlow.Stencil, 0, 10, 1, "%i", FSliderEnum::Left | FSliderEnum::Min);
						FSlider("Blur scale## TrajectoryTeam", &tGlow.Blur, 0.f, 10.f, 1.f, "%g", FSliderEnum::Right | FSliderEnum::Min | FSliderEnum::Precision);
						FColorPicker("Team color", &tGlow.Color, &tGlow.DistanceColor, FColorPickerEnum::Left);
						FToggle("Health color## TrajectoryTeam", &tGlow.HealthColor, FToggleEnum::Right);
						if (tGlow.HealthColor)
							FGlowGradient("TrajectoryTeamHealthGradient", tGlow.Stops);
					}
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Prediction"))
				{
					FDropdown(Vars::Visuals::Prediction::PlayerPath, FDropdownEnum::Left, -10);
					FColorPicker(Vars::Colors::PlayerPathIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::PlayerPath, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });
					FDropdown(Vars::Visuals::Prediction::ProjectilePath, FDropdownEnum::Right, -10);
					FColorPicker(Vars::Colors::ProjectilePathIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::ProjectilePath, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });
					FToggleRow(Vars::Visuals::Prediction::SwingLines);
					FSliderRow(Vars::Visuals::Prediction::PlayerDrawDuration, FSliderEnum::Left, !Vars::Visuals::Prediction::PlayerDrawDuration[DEFAULT_BIND] ? "timed" : "%g");
					FSliderRow(Vars::Visuals::Prediction::ProjectileDrawDuration, FSliderEnum::Right, !Vars::Visuals::Prediction::ProjectileDrawDuration[DEFAULT_BIND] ? "timed" : "%g");
				} EndSection();
				if (Section("Simulation"))
				{
					FDropdown(Vars::Visuals::Simulation::TrajectoryPath, FDropdownEnum::Left, -10);
					FColorPicker(Vars::Colors::TrajectoryPathIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::TrajectoryPath, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });
					FDropdown(Vars::Visuals::Simulation::ShotPath, FDropdownEnum::Right, -10);
					FColorPicker(Vars::Colors::ShotPathIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::ShotPath, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });
					FDropdown(Vars::Visuals::Simulation::SplashRadius, FDropdownEnum::None, -10);
					FColorPicker(Vars::Colors::SplashRadiusIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					FColorPicker(Vars::Colors::SplashRadius, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });
					FDropdown(Vars::Visuals::Simulation::StickyRadius, FDropdownEnum::None, -10);
					FColorPicker(Vars::Colors::StickyRadiusPlayerInsideIgnoreZ, FColorPickerEnum::SameLine, { 0, H::Draw.Scale(30) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::StickyRadiusPlayerInside, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::StickyRadiusIgnoreZ, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					FColorPicker(Vars::Colors::StickyRadius, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(-10) }, { H::Draw.Scale(10), H::Draw.Scale(10) });
					SameLine(); DebugDummy({ 0, H::Draw.Scale(48) });
					FToggleRow(Vars::Visuals::Simulation::ProjectileCamera, FToggleEnum::Left);
					FToggleRow(Vars::Visuals::Simulation::Box, FToggleEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug"))
					{
						FText("Extra", { 5, 5 });
						if (FPopupButton("Extra", { 0, -5 }, -8))
						{
							FDropdown(Vars::Visuals::Prediction::RealPath, FDropdownEnum::None, -10);
							FColorPicker(Vars::Colors::RealPath, FColorPickerEnum::SameLine, {}, { H::Draw.Scale(10), H::Draw.Scale(20) });
							FColorPicker(Vars::Colors::RealPathIgnoreZ, FColorPickerEnum::SameLine, { H::Draw.Scale(-10), H::Draw.Scale(20) }, { H::Draw.Scale(10), H::Draw.Scale(20) });

							FSliderRow(Vars::Visuals::Path::SeparatorSpacing, FSliderEnum::Left);
							FSliderRow(Vars::Visuals::Path::SeparatorLength, FSliderEnum::Right);

							EndPopup();
						}

						FText("Simulation", { 5, 5 });
						if (FPopupButton("Simulation", { 0, -5 }))
						{
							FToggleRow(Vars::Visuals::Trajectory::Override);
							bool bApply = FButton("Apply current");
							FSDropdown(Vars::Visuals::Trajectory::Type);
							FSliderRow(Vars::Visuals::Trajectory::OffsetX);
							FSliderRow(Vars::Visuals::Trajectory::OffsetY);
							FSliderRow(Vars::Visuals::Trajectory::OffsetZ);
							FSliderRow(Vars::Visuals::Trajectory::ForwardRedirect);
							FSliderRow(Vars::Visuals::Trajectory::ForwardCutoff);
							FSliderRow(Vars::Visuals::Trajectory::Hull);
							FSliderRow(Vars::Visuals::Trajectory::Speed);
							FSliderRow(Vars::Visuals::Trajectory::Gravity);
							FSliderRow(Vars::Visuals::Trajectory::LifeTime);
							FSliderRow(Vars::Visuals::Trajectory::UpVelocity);
							FSliderRow(Vars::Visuals::Trajectory::AngularVelocityX);
							FSliderRow(Vars::Visuals::Trajectory::AngularVelocityY);
							FSliderRow(Vars::Visuals::Trajectory::AngularVelocityZ);
							FSliderRow(Vars::Visuals::Trajectory::Drag);
							FSliderRow(Vars::Visuals::Trajectory::DragX);
							FSliderRow(Vars::Visuals::Trajectory::DragY);
							FSliderRow(Vars::Visuals::Trajectory::DragZ);
							FSliderRow(Vars::Visuals::Trajectory::AngularDragX);
							FSliderRow(Vars::Visuals::Trajectory::AngularDragY);
							FSliderRow(Vars::Visuals::Trajectory::AngularDragZ);
							FSliderRow(Vars::Visuals::Trajectory::MaxVelocity);
							FSliderRow(Vars::Visuals::Trajectory::MaxAngularVelocity);

							if (bApply)
							{
								auto pLocal = H::Entities.GetLocal();
								auto pWeapon = H::Entities.GetWeapon();
								if (pLocal && pWeapon)
								{
									ProjectileInfo tProjInfo = {};
									bool bOriginal = Vars::Visuals::Trajectory::Override.Value;
									Vars::Visuals::Trajectory::Override.Value = false;
									bool bSetup = F::ProjSim.GetInfo(pLocal, pWeapon, {}, tProjInfo, ProjSimEnum::Interp)
											   && F::ProjSim.Initialize(tProjInfo, false);
									Vars::Visuals::Trajectory::Override.Value = bOriginal;
									if (bSetup)
									{
										Vec3 vLocalEye = pLocal->GetEyePosition();
										Vec3 vOffset = tProjInfo.m_vPos - vLocalEye; vOffset.y *= -1;
										float flForwardRedirect = 0.f, flForwardCutoff = 0.f;
										switch (tProjInfo.m_uType)
										{
										case FNV1A::Hash32Const("models/weapons/w_models/w_rocket.mdl"):
										case FNV1A::Hash32Const("models/weapons/w_models/w_drg_ball.mdl"):
										case FNV1A::Hash32Const("models/weapons/w_models/w_flaregun_shell.mdl"):
										case FNV1A::Hash32Const("models/weapons/w_models/w_arrow.mdl"):
										case FNV1A::Hash32Const("models/weapons/w_models/w_syringe_proj.mdl"):
										case FNV1A::Hash32Const("models/weapons/w_models/w_repair_claw.mdl"):
											flForwardRedirect = 2000.f, flForwardCutoff = 0.1f; break;
										case FNV1A::Hash32Const("models/weapons/c_models/c_flameball/c_flameball.mdl"):
											flForwardRedirect = H::ConVars.FindVar("tf_fireball_distance")->GetFloat(), flForwardCutoff = 1.f;
										}
										physics_performanceparams_t params = {}; F::ProjSim.m_pEnv->GetPerformanceSettings(&params);
										Vec3 vVelocity, vAngVelocity; F::ProjSim.m_pObj->GetVelocity(&vVelocity, &vAngVelocity);

										Vars::Visuals::Trajectory::OffsetX[DEFAULT_BIND] = vOffset.x;
										Vars::Visuals::Trajectory::OffsetY[DEFAULT_BIND] = vOffset.y;
										Vars::Visuals::Trajectory::OffsetZ[DEFAULT_BIND] = vOffset.z;
										Vars::Visuals::Trajectory::ForwardRedirect[DEFAULT_BIND] = flForwardRedirect;
										Vars::Visuals::Trajectory::ForwardCutoff[DEFAULT_BIND] = flForwardCutoff;
										Vars::Visuals::Trajectory::Hull[DEFAULT_BIND] = tProjInfo.m_vHull.x;
										Vars::Visuals::Trajectory::Speed[DEFAULT_BIND] = vVelocity.x;
										Vars::Visuals::Trajectory::Gravity[DEFAULT_BIND] = tProjInfo.m_flGravity;
										Vars::Visuals::Trajectory::LifeTime[DEFAULT_BIND] = tProjInfo.m_flLifetime;
										Vars::Visuals::Trajectory::UpVelocity[DEFAULT_BIND] = vVelocity.z;
										Vars::Visuals::Trajectory::AngularVelocityX[DEFAULT_BIND] = vAngVelocity.x;
										Vars::Visuals::Trajectory::AngularVelocityY[DEFAULT_BIND] = vAngVelocity.y;
										Vars::Visuals::Trajectory::AngularVelocityZ[DEFAULT_BIND] = vAngVelocity.z;
										Vars::Visuals::Trajectory::Drag[DEFAULT_BIND] = F::ProjSim.m_pObj->m_dragCoefficient;
										Vars::Visuals::Trajectory::DragX[DEFAULT_BIND] = F::ProjSim.m_pObj->m_dragBasis.x;
										Vars::Visuals::Trajectory::DragY[DEFAULT_BIND] = F::ProjSim.m_pObj->m_dragBasis.y;
										Vars::Visuals::Trajectory::DragZ[DEFAULT_BIND] = F::ProjSim.m_pObj->m_dragBasis.z;
										Vars::Visuals::Trajectory::AngularDragX[DEFAULT_BIND] = F::ProjSim.m_pObj->m_angDragBasis.x;
										Vars::Visuals::Trajectory::AngularDragY[DEFAULT_BIND] = F::ProjSim.m_pObj->m_angDragBasis.y;
										Vars::Visuals::Trajectory::AngularDragZ[DEFAULT_BIND] = F::ProjSim.m_pObj->m_angDragBasis.z;
										Vars::Visuals::Trajectory::MaxVelocity[DEFAULT_BIND] = params.maxVelocity;
										Vars::Visuals::Trajectory::MaxAngularVelocity[DEFAULT_BIND] = params.maxAngularVelocity;
									}
								}
							}

							EndPopup();
						}
					} EndSection();
				}
			}
			EndTable();
		}
		break;
	}
	}
}

void CMenu::MenuHVH(int iTab)
{
	using namespace ImGui;

	switch (iTab)
	{
	// HvH
	case 0:
	{
		if (BeginTable("HvHTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Antiaim", 8))
				{
					bool bAAOn = SectionToggle(Vars::AntiAim::Enabled);
					PushTransparent(!bAAOn, true);
					FToggleRow(Vars::AntiAim::HidePitchOnShot);
					SubGroup("Pitch");
					FDropdown(Vars::AntiAim::PitchReal, FDropdownEnum::Left);
					FDropdown(Vars::AntiAim::PitchFake, FDropdownEnum::Right);
					SubGroup("Yaw");
					FDropdown(Vars::AntiAim::YawReal, FDropdownEnum::Left);
					FDropdown(Vars::AntiAim::YawFake, FDropdownEnum::Right);
					FDropdown(Vars::AntiAim::RealYawBase, FDropdownEnum::Left);
					FDropdown(Vars::AntiAim::FakeYawBase, FDropdownEnum::Right);
					FSliderRow(Vars::AntiAim::RealYawOffset, FSliderEnum::Left);
					FSliderRow(Vars::AntiAim::FakeYawOffset, FSliderEnum::Right);
					PushTransparent(Vars::AntiAim::YawReal.Value != Vars::AntiAim::YawEnum::Edge && Vars::AntiAim::YawReal.Value != Vars::AntiAim::YawEnum::Jitter);
					{
						FSliderRow(Vars::AntiAim::RealYawValue, FSliderEnum::Left);
					}
					PopTransparent();
					PushTransparent(Vars::AntiAim::YawFake.Value != Vars::AntiAim::YawEnum::Edge && Vars::AntiAim::YawFake.Value != Vars::AntiAim::YawEnum::Jitter);
					{
						FSliderRow(Vars::AntiAim::FakeYawValue, FSliderEnum::Right);
					}
					PopTransparent();
					PushTransparent(Vars::AntiAim::YawFake.Value != Vars::AntiAim::YawEnum::Spin && Vars::AntiAim::YawReal.Value != Vars::AntiAim::YawEnum::Spin);
					{
						FSliderRow(Vars::AntiAim::SpinSpeed, FSliderEnum::Left);
					}
					PopTransparent();
					SetCursorPos({ GetWindowWidth() / 2 + GetStyle().WindowPadding.x / 2, GetRowPos() + H::Draw.Scale(8) });
					FToggleRow(Vars::AntiAim::MinWalk, FToggleEnum::Left);
					PopTransparent(1, 1);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Antiaim"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FToggleRow(Vars::AntiAim::AntiAimLines);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Resolver", 8))
				{
					bool bResolverOn = SectionToggle(Vars::Resolver::Enabled);
					PushTransparent(!bResolverOn, true);
					{
						FToggleRow(Vars::Resolver::AutoResolve);
						PushTransparent(Transparent || !Vars::Resolver::AutoResolve.Value);
						{
							SubGroup("Auto-resolve");
							FToggleRow(Vars::Resolver::AutoResolveCheatersOnly, FToggleEnum::Left);
							FToggleRow(Vars::Resolver::AutoResolveHeadshotOnly, FToggleEnum::Right);
							PushTransparent(Transparent || !Vars::Resolver::AutoResolveYawAmount.Value);
							{
								FSliderRow(Vars::Resolver::AutoResolveYawAmount, FSliderEnum::Left);
							}
							PopTransparent();
							PushTransparent(Transparent || !Vars::Resolver::AutoResolvePitchAmount.Value);
							{
								FSliderRow(Vars::Resolver::AutoResolvePitchAmount, FSliderEnum::Right);
							}
							PopTransparent();
						}
						PopTransparent();

						SubGroup("Cycle");
						FSliderRow(Vars::Resolver::CycleYaw, FSliderEnum::Left);
						FSliderRow(Vars::Resolver::CyclePitch, FSliderEnum::Right);
						FToggleRow(Vars::Resolver::CycleView, FToggleEnum::Left);
						FToggleRow(Vars::Resolver::CycleMinwalk, FToggleEnum::Right);
					}
					PopTransparent(1, 1);
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Doubletap", 8))
				{
					FToggleRow(Vars::Doubletap::Doubletap, FToggleEnum::Left);
					FToggleRow(Vars::Doubletap::Warp, FToggleEnum::Right);
					FToggleRow(Vars::Doubletap::RechargeTicks, FToggleEnum::Left);
					FToggleRow(Vars::Doubletap::AntiWarp, FToggleEnum::Right);

					SubGroup("Tuning");
					FSliderRow(Vars::Doubletap::TickLimit, FSliderEnum::Left);
					FSliderRow(Vars::Doubletap::WarpRate, FSliderEnum::Right);
					FSliderRow(Vars::Doubletap::RechargeLimit, FSliderEnum::Left);
					FSliderRow(Vars::Doubletap::PassiveRecharge, FSliderEnum::Right);
				} EndSection();
				if (Section("Fakelag"))
				{
					FDropdown(Vars::Fakelag::Fakelag, FSliderEnum::Left);
					FDropdown(Vars::Fakelag::Options, FDropdownEnum::Right);
					PushTransparent(Vars::Fakelag::Fakelag.Value != Vars::Fakelag::FakelagEnum::Plain);
					{
						FSliderRow(Vars::Fakelag::PlainTicks, FSliderEnum::Left);
					}
					PopTransparent();
					PushTransparent(Vars::Fakelag::Fakelag.Value != Vars::Fakelag::FakelagEnum::Random);
					{
						FSlider(Vars::Fakelag::RandomTicks, FSliderEnum::Right);
					}
					PopTransparent();
					FToggleRow(Vars::Fakelag::UnchokeOnAttack, FToggleEnum::Left);
					FToggleRow(Vars::Fakelag::RetainBlastJump, FToggleEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug Fakelag"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FToggleRow(Vars::Fakelag::RetainSoldierOnly);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Auto Peek", 8))
				{
					FToggleRow(Vars::AutoPeek::Enabled);
				} EndSection();
				if (Section("Speedhack", 8))
				{
					PushTransparent(Vars::Speedhack::Scale.Value == 1);
					{
						FSliderRow(Vars::Speedhack::Scale);
					}
					PopTransparent();
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	}
}

void CMenu::MenuVisuals(int iTab)
{
	using namespace ImGui;

	switch (iTab)
	{
	// ESP
	case 0:
	{
		// fake angle/viewmodel visuals, pickup timers?
		static size_t iCurrentGroup = 0;

		if (Section("Groups"))
		{
			static std::string sStaticName;

			PushDisabled(F::Groups.m_vGroups.size() >= sizeof(int) * 8); // for active groups flags
			{
				FSDropdown("Name", &sStaticName, {}, FDropdownEnum::Left | FSDropdownEnum::AutoUpdate, -H::Draw.Unscale(FCalcTextSize("CREATE").x) - 36);

				PushDisabled(Disabled || sStaticName.empty());
				{
					if (FButton("Create", FButtonEnum::Fit | FButtonEnum::SameLine, { 0, 40 }))
					{
						F::Groups.m_vGroups.emplace_back(sStaticName);
						F::Groups.m_vGroups.back().m_iSlot = F::Groups.NextSlot();
						sStaticName.clear();

						iCurrentGroup = F::Groups.m_vGroups.size() - 1;
					}
				}
				PopDisabled();
			}
			PopDisabled();

			{
				// Bit values keyed by each group's stable slot (not its position in
				// m_vGroups) so reordering/duplicating groups can't remap keybinds.
				std::vector<int> vSlotValues;
				for (auto& tGroup : F::Groups.m_vGroups)
					vSlotValues.push_back(1 << tGroup.m_iSlot);
				FDropdown(Vars::ESP::ActiveGroups, Vars::ESP::ActiveGroups.m_vValues, vSlotValues, FDropdownEnum::Right | FDropdownEnum::Multi);
			}

			PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
			SetCursorPos({ H::Draw.Scale(13), H::Draw.Scale(80) });
			FText("Groups");
			SetCursorPosY(GetCursorPosY() - H::Draw.Scale(5));
			PopStyleColor();

			auto fPositionToIndex = [](ImVec2 vPos)
			{
				int iIndex = floorf((vPos.y - GetCursorPosY() - H::Draw.Scale(4)) / H::Draw.Scale(36)) * 2
					+ (vPos.x > GetWindowWidth() / 2 ? 1 : 0);
				iIndex = std::clamp(iIndex, 0, int(F::Groups.m_vGroups.size() - 1));
				return iIndex;
			};

			static int iDragging = -1;
			if (!IsMouseDown(ImGuiMouseButton_Left))
				iDragging = -1;
			else if (iDragging != -1)
			{
				int iTo = fPositionToIndex(GetMousePos() - GetDrawPos());
				if (iDragging != iTo)
				{
					F::Groups.Move(iDragging, iTo);
					if (iCurrentGroup == iDragging)
						iCurrentGroup = iTo;
					else if (iCurrentGroup < iDragging && iCurrentGroup >= iTo)
						iCurrentGroup++;
					else if (iCurrentGroup > iDragging && iCurrentGroup <= iTo)
						iCurrentGroup--;
					iDragging = iTo;
				}
			}

			for (auto it = F::Groups.m_vGroups.begin(); it < F::Groups.m_vGroups.end();)
			{
				int iGroup = std::distance(F::Groups.m_vGroups.begin(), it);
				auto& tGroup = *it;

				ImVec2 vOriginalPos = !(iGroup % 2)
					? ImVec2(GetStyle().WindowPadding.x, GetCursorPosY() + H::Draw.Scale(8))
					: ImVec2(GetWindowWidth() / 2 + GetStyle().WindowPadding.x / 2, GetCursorPosY() - H::Draw.Scale(28));

				// background (Phase 3: neutral card + left color bar for legibility)
				float flWidth = GetWindowWidth() / 2 - GetStyle().WindowPadding.x * 1.5f;
				float flHeight = H::Draw.Scale(28);
				float flBar = H::Draw.Scale(4);
				ImColor tBar = ColorByteToFloat(tGroup.m_tColor);
				ImColor tFill = iCurrentGroup != iGroup ? F::Render.Background1p5 : F::Render.Background1p5L;
				ImVec2 vDrawPos = GetDrawPos() + vOriginalPos;
				GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flWidth, flHeight), tFill, H::Draw.Scale(4));
				GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flBar, flHeight), tBar, H::Draw.Scale(4), ImDrawFlags_RoundCornersLeft);
				if (iCurrentGroup == iGroup)
				{
					float flInset = H::Draw.Scale(0.5f) - 0.5f;
					GetWindowDrawList()->AddRect(vDrawPos + ImVec2(flInset, flInset), vDrawPos + ImVec2(flWidth - flInset, flHeight - flInset), tBar, H::Draw.Scale(4), ImDrawFlags_None, H::Draw.Scale());
				}

				// text + icons
				float flTextWidth = flWidth - H::Draw.Scale(36);
				SetCursorPos(vOriginalPos + ImVec2(H::Draw.Scale(9), H::Draw.Scale(7)));
				PushTransparent(!(Vars::ESP::ActiveGroups.Value & 1 << tGroup.m_iSlot), true);
				{
					FText(TruncateText(tGroup.m_sName, flTextWidth).c_str());
				}
				PopTransparent(1, 1);

				SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(26), H::Draw.Scale(2)));
				bool bDelete = IconButton(ICON_MD_DELETE), bDuplicate = false;

				SetCursorPos(vOriginalPos);
				bool bClicked = Button(std::format("##{}", iGroup).c_str(), { flWidth, flHeight });
				bool bPopup = IsItemClicked(ImGuiMouseButton_Right);

				if (bClicked)
					iCurrentGroup = iGroup;
				else if (bPopup)
					OpenPopup(std::format("RightClicked{}", iGroup).c_str());
				else if (iDragging == -1 && IsItemHovered() && IsMouseDown(ImGuiMouseButton_Left))
					iDragging = iGroup;

				if (FBeginPopup(std::format("RightClicked{}", iGroup).c_str()))
				{
					PushStyleVar(ImGuiStyleVar_ItemSpacing, { H::Draw.Scale(8), 0 });

					{
						static std::string sInput = "";

						bool bEnter = FInputText("Name...", sInput, H::Draw.Scale(284), ImGuiInputTextFlags_EnterReturnsTrue);
						if (!IsItemFocused())
							sInput = tGroup.m_sName;
						if (bEnter)
							tGroup.m_sName = sInput;
					}

					PushDisabled(F::Groups.m_vGroups.size() >= sizeof(int) * 8);
					{
						bDuplicate = FButton("Duplicate");
					}
					PopDisabled();

					PopStyleVar();
					EndPopup();
				}

				if (bDelete)
					it = F::Groups.m_vGroups.erase(it);
				else if (bDuplicate)
				{
					it = F::Groups.m_vGroups.insert(it + 1, tGroup);
					it->m_sName += " duplicate";
					it->m_iSlot = F::Groups.NextSlot();
				}
				else
					++it;
			}
		} EndSection();

		if (!F::Groups.m_vGroups.empty()
			&& BeginTable("VisualsESPTable", 2))
		{
			iCurrentGroup = std::clamp(iCurrentGroup, 0ui64, F::Groups.m_vGroups.size() - 1);
			auto& tGroup = F::Groups.m_vGroups[iCurrentGroup];

			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Color", 8))
				{
					FColorPicker("Group color", &tGroup.m_tColor, FColorPickerEnum::Left);
					FToggle("Tags override color", &tGroup.m_bTagsOverrideColor, FToggleEnum::Right);
				} EndSection();
				if (Section("Targets"))
				{
					FDropdown("Targets", &tGroup.m_iTargets, { "Players", "Buildings", "Projectiles", "Ragdolls", "Objective", "NPCs", "Health", "Ammo", "Money", "Powerups", "Spellbook", "Bombs", "Gargoyle", "##Divider", "Fake angle", "Viewmodel weapon", "Viewmodel hands" }, {}, FDropdownEnum::Multi);
				} EndSection();
				if (Section("Conditions"))
				{
					FDropdown("Conditions", &tGroup.m_iConditions, { "Enemy", "Team", "BLU", "RED", "##Divider", "Local", "Friends", "Party", "Priority", "Target", "##Divider", "Dormant" }, {}, FDropdownEnum::Multi);
					Divider(H::Draw.Scale(8), H::Draw.Scale(-1));
					PushTransparent(!(tGroup.m_iTargets & TargetsEnum::Players));
					{
						FDropdown("Players", &tGroup.m_iPlayers, { "Scout", "Soldier", "Pyro", "Demoman", "Heavy", "Engineer", "Medic", "Sniper", "Spy", "##Divider", "Invulnerable", "Crits", "Invisible", "Disguise", "Hurt", "Crit Heals" }, {}, FDropdownEnum::Multi, 0, "All");
					}
					PopTransparent();
					PushTransparent(!(tGroup.m_iTargets & TargetsEnum::Buildings));
					{
						FDropdown("Buildings", &tGroup.m_iBuildings, { "Sentry", "Dispenser", "Teleporter", "##Divider", "Hurt" }, {}, FDropdownEnum::Multi, 0, "All");
					}
					PopTransparent();
					PushTransparent(!(tGroup.m_iTargets & TargetsEnum::Projectiles));
					{
						FDropdown("Projectiles", &tGroup.m_iProjectiles, { "Rocket", "Sticky", "Pipe", "Arrow", "Heal", "Flare", "Fire", "Repair", "Cleaver", "Milk", "Jarate", "Gas", "Bauble", "Baseball", "Energy", "Short circuit", "Meteor shower", "Lightning", "Fireball", "Bomb", "Bats", "Pumpkin", "Monoculus", "Skeleton", "Misc", "##Divider", "Crit", "Minicrit" }, {}, FDropdownEnum::Multi, 0, "All");
					}
					PopTransparent();
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("ESP"))
				{
					std::vector<const char*> vEntries = { "Name", "Box", "Distance" };
					std::vector<int> vValues = { ESPEnum::Name, ESPEnum::Box, ESPEnum::Distance };
					if (tGroup.m_iTargets & TargetsEnum::Players)
					{
						vEntries.insert(vEntries.end(), { "Bones" });
						vValues.insert(vValues.end(), { ESPEnum::Bones });
					}
					if (tGroup.m_iTargets & (TargetsEnum::Players | TargetsEnum::Buildings))
					{
						vEntries.insert(vEntries.end(), { "Health bar", "Health text" });
						vValues.insert(vValues.end(), { ESPEnum::HealthBar, ESPEnum::HealthText });
					}
					if (tGroup.m_iTargets & TargetsEnum::Players)
					{
						vEntries.insert(vEntries.end(), { "Uber bar", "Uber text", "Class icon", "Class text", "Weapon icon", "Weapon text", "Priority", "Labels", "Buffs", "Debuffs" });
						vValues.insert(vValues.end(), { ESPEnum::UberBar, ESPEnum::UberText, ESPEnum::ClassIcon, ESPEnum::ClassText, ESPEnum::WeaponIcon, ESPEnum::WeaponText, ESPEnum::Priority, ESPEnum::Labels, ESPEnum::Buffs, ESPEnum::Debuffs });
					}
					if (tGroup.m_iTargets & (TargetsEnum::Players | TargetsEnum::Buildings | TargetsEnum::Projectiles | TargetsEnum::Objective))
					{
						vEntries.insert(vEntries.end(), { "Flags" });
						vValues.insert(vValues.end(), { ESPEnum::Flags });
					}
					if (tGroup.m_iTargets & TargetsEnum::Players)
					{
						vEntries.insert(vEntries.end(), { "Lag compensation", "Ping", "KDR" });
						vValues.insert(vValues.end(), { ESPEnum::LagCompensation, ESPEnum::Ping, ESPEnum::KDR });
					}
					if (tGroup.m_iTargets & (TargetsEnum::Buildings | TargetsEnum::Projectiles))
					{
						vEntries.insert(vEntries.end(), { "Owner" });
						vValues.insert(vValues.end(), { ESPEnum::Owner });
					}
					if (tGroup.m_iTargets & TargetsEnum::Buildings)
					{
						vEntries.insert(vEntries.end(), { "Level", "Ammo bars", "Ammo text" });
						vValues.insert(vValues.end(), { ESPEnum::Level, ESPEnum::AmmoBars, ESPEnum::AmmoText });
					}
					if (tGroup.m_iTargets & TargetsEnum::Objective)
					{
						vEntries.insert(vEntries.end(), { "Intel return time" });
						vValues.insert(vValues.end(), { ESPEnum::IntelReturnTime });
					}

					PushTransparent(tGroup.m_iTargets && !(tGroup.m_iTargets & TargetsEnum::ESP));
					{
						FDropdown("Draw", &tGroup.m_iESP, vEntries, vValues, FDropdownEnum::Multi);
					}
					PopTransparent();
				} EndSection();
				if (Section("Chams"))
				{
					// Body-part pickers only make sense for player models.
					const bool bBodyParts = !tGroup.m_iTargets || tGroup.m_iTargets & TargetsEnum::Players;
					if (!tGroup.m_iTargets || tGroup.m_iTargets & TargetsEnum::Occluded)
					{
						FMDropdown("Visible material", &tGroup.m_tChams.Visible, FDropdownEnum::Left, 0, nullptr, bBodyParts);
						FMDropdown("Occluded material", &tGroup.m_tChams.Occluded, FDropdownEnum::Right, 0, nullptr, bBodyParts);
					}
					else
						FMDropdown("Material", &tGroup.m_tChams.Visible, FDropdownEnum::None, 0, nullptr, bBodyParts);

					// Rendered only while the crosshair is on this group's entity.
					FMDropdown("Targeted material", &tGroup.m_tTargetChams.Visible, FDropdownEnum::None, 0, nullptr, bBodyParts);
				} EndSection();
				if (Section("Glow", 8))
				{
					PushTransparent(!tGroup.m_tGlow.Stencil);
					{
						FSlider("Stencil scale", &tGroup.m_tGlow.Stencil, 0, 10, 1, "%i", FSliderEnum::Left | FSliderEnum::Min);
					}
					PopTransparent();
					PushTransparent(!tGroup.m_tGlow.Blur);
					{
						FSlider("Blur scale", &tGroup.m_tGlow.Blur, 0.f, 10.f, 1.f, "%g", FSliderEnum::Right | FSliderEnum::Min | FSliderEnum::Precision);
					}
					PopTransparent();
					PushTransparent(!tGroup.m_tGlow());
					{
						FColorPicker("Glow color", &tGroup.m_tGlow.Color, &tGroup.m_tGlow.DistanceColor, FColorPickerEnum::Left);
						FToggle("Health color", &tGroup.m_tGlow.HealthColor, FToggleEnum::Right);
						if (tGroup.m_tGlow.HealthColor)
							FGlowGradient("GlowHealthGradient", tGroup.m_tGlow.Stops);
					}
					PopTransparent();
				} EndSection();
				if (Section("Misc", 8))
				{
					FToggle("Offscreen arrows", &tGroup.m_bOffscreenArrows, FToggleEnum::Left);
					if (FPopupButton("OffscreenArrows"))
					{
						FSlider("Offset", &tGroup.m_iOffscreenArrowsOffset, 0, 1000, 25, "%i", FSliderEnum::Precision);
						FSlider("Max distance", &tGroup.m_flOffscreenArrowsMaxDistance, 0.f, 5000.f, 50.f, "%g", FSliderEnum::Min | FSliderEnum::Precision);

						EndPopup();
					}

					FToggle("Pickup timer", &tGroup.m_bPickupTimer);

					FToggle("Backtrack", &tGroup.m_iBacktrack, BacktrackEnum::Enabled, FToggleEnum::Left);
					if (FPopupButton("Backtrack", {}, -8))
					{
						FDropdown("##Draw", &tGroup.m_iBacktrack, { "Last", "First", "##Divider", "Always" }, { BacktrackEnum::Last, BacktrackEnum::First, BacktrackEnum::Always }, FDropdownEnum::Multi | FDropdownEnum::NoSanitization, 0, "All");

						FMDropdown("Visible material", &tGroup.m_tBacktrackChams.Visible, FDropdownEnum::Left);
						FMDropdown("Occluded material", &tGroup.m_tBacktrackChams.Occluded, FDropdownEnum::Right);

						SetCursorPosY(GetCursorPosY() + H::Draw.Scale(8));
						PushTransparent(!tGroup.m_tBacktrackGlow.Stencil);
						{
							FSlider("Stencil scale## Backtrack", &tGroup.m_tBacktrackGlow.Stencil, 0, 10, 1, "%i", FSliderEnum::Left | FSliderEnum::Min);
						}
						PopTransparent();
						PushTransparent(!tGroup.m_tBacktrackGlow.Blur);
						{
							FSlider("Blur scale## Backtrack", &tGroup.m_tBacktrackGlow.Blur, 0.f, 10.f, 1.f, "%g", FSliderEnum::Right | FSliderEnum::Min | FSliderEnum::Precision);
						}
						PopTransparent();
						PushTransparent(!tGroup.m_tBacktrackGlow());
						{
							FColorPicker("Glow color## Backtrack", &tGroup.m_tBacktrackGlow.Color, &tGroup.m_tBacktrackGlow.DistanceColor, FColorPickerEnum::Left);
							FToggle("Health color## Backtrack", &tGroup.m_tBacktrackGlow.HealthColor, FToggleEnum::Right);
							if (tGroup.m_tBacktrackGlow.HealthColor)
								FGlowGradient("BacktrackGlowHealthGradient", tGroup.m_tBacktrackGlow.Stops);
						}
						PopTransparent();

						EndPopup();
					}

					FToggle("Trajectory", &tGroup.m_iTrajectory, TrajectoryEnum::Enabled, FToggleEnum::Left);
					if (FPopupButton("Trajectory", {}, -8))
					{
						FDropdown("Flags", &tGroup.m_iTrajectory, { "Predict", "##Divider", "Radius", "Trace", "Sphere", "##Divider", "Path" }, { TrajectoryEnum::Predict, TrajectoryEnum::Radius, TrajectoryEnum::Trace, TrajectoryEnum::Sphere, TrajectoryEnum::Path }, FDropdownEnum::Multi | FDropdownEnum::NoSanitization);
						FToggle("Ignore Z", &tGroup.m_iTrajectory, SightlinesEnum::IgnoreZ);

						EndPopup();
					}

					FToggle("Sightlines", &tGroup.m_iSightlines, SightlinesEnum::Enabled, FToggleEnum::Left);
					if (FPopupButton("Sightlines"))
					{
						FToggle("Ignore Z", &tGroup.m_iSightlines, SightlinesEnum::IgnoreZ);

						EndPopup();
					}
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	// View
	case 1:
	{
		// 3a: "Field of view" is promoted out of the table as a full-width feature card.
		if (Section("Field of view", 8))
		{
			PushTransparent(!Vars::Visuals::UI::FieldOfView.Value);
			{
				FSliderRow(Vars::Visuals::UI::FieldOfView);
			}
			PopTransparent();
			PushTransparent(!Vars::Visuals::UI::ZoomFieldOfView.Value);
			{
				FSliderRow(Vars::Visuals::UI::ZoomFieldOfView);
			}
			PopTransparent();
			PushTransparent(!Vars::Visuals::UI::ViewmodelFOV.Value);
			{
				FSliderRow(Vars::Visuals::UI::ViewmodelFOV);
			}
			PopTransparent();
		} EndSection();

		if (BeginTable("VisualsViewTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Interface"))
				{
					FDropdown(Vars::Visuals::UI::StreamerMode, FDropdownEnum::Left);
					FDropdown(Vars::Visuals::UI::ChatTags, FDropdownEnum::Right, -10);
					FColorPicker(Vars::Colors::Local, FColorPickerEnum::SameLine, {}, { H::Draw.Scale(10), H::Draw.Scale(40) });
				} EndSection();
				if (Section("Flex FOV", 8))
				{
					// FlexFOVComposite gates the whole cluster, so it becomes the header master toggle.
					bool bFlex = SectionToggle(Vars::Visuals::UI::FlexFOVComposite);
					PushTransparent(!bFlex, true);
					{
						FSliderRow(Vars::Visuals::UI::FlexFOVStrength);
						FSlider(Vars::Visuals::UI::FlexFOVTransition); // FloatRange_t: two-handle range, no 3a row form
						FSliderRow(Vars::Visuals::UI::FlexFOVQuality);

						SubGroup("Performance");
						FSliderRow(Vars::Visuals::UI::FlexFOVStagger);
						FToggleGrid({
							&Vars::Visuals::UI::FlexFOVStaggerFront,
							&Vars::Visuals::UI::FlexFOVCheapPeriphery,
							&Vars::Visuals::UI::FlexFOVCheapSky,
						});

						SubGroup("Projection");
						FToggleGrid({
							&Vars::Visuals::UI::FlexFOVTightFaces,
							&Vars::Visuals::UI::FlexFOVStereographic,
							&Vars::Visuals::UI::FlexFOVVertStereo,
						});
					}
					PopTransparent(1, 1);
				} EndSection();
				if (Section("Thirdperson", 8))
				{
					bool bOn = SectionToggle(Vars::Visuals::Thirdperson::Enabled);
					PushTransparent(!bOn, true);
					{
						FToggleRow(Vars::Visuals::Thirdperson::Crosshair);
						FSliderRow(Vars::Visuals::Thirdperson::Distance);
						FSliderRow(Vars::Visuals::Thirdperson::Right);
						FSliderRow(Vars::Visuals::Thirdperson::Up);
					}
					PopTransparent(1, 1);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FToggleRow(Vars::Visuals::Thirdperson::Scale, FToggleEnum::Left);
							FToggleRow(Vars::Visuals::Thirdperson::Collide, FToggleEnum::Right);

							EndPopup();
						}
					} EndSection();
				}
				if (Section("Rear view"))
				{
					bool bRearViewOn = SectionToggle(Vars::Visuals::UI::RearView);
					PushTransparent(!bRearViewOn, true);
					{
						FSliderRow(Vars::Visuals::UI::RearViewCameras);
						FSliderRow(Vars::Visuals::UI::RearViewFOVOffset);
						FToggleRow(Vars::Visuals::UI::RearViewFlipPitch);
						FSliderRow(Vars::Visuals::UI::RearViewAlpha);
						FToggleRow(Vars::Visuals::UI::RearViewHalfRate);
						FMDropdown(Vars::Visuals::UI::RearViewMaterial);

						SubGroup("Glow");
						FSliderRow(Vars::Visuals::UI::RearViewGlowStencil);
						FSliderRow(Vars::Visuals::UI::RearViewGlowBlur);
						PushTransparent(!(Vars::Visuals::UI::RearViewGlowStencil.Value || Vars::Visuals::UI::RearViewGlowBlur.Value));
						{
							FColorPicker(Vars::Visuals::UI::RearViewGlowColor, FColorPickerEnum::Full);
						}
						PopTransparent();
					}
					PopTransparent(1, 1);
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Scoreboard"))
				{
					FToggleGrid({
						&Vars::Visuals::UI::RevealScoreboard,
						&Vars::Visuals::UI::ScoreboardUtility,
						&Vars::Visuals::UI::ScoreboardColors,
						&Vars::Visuals::UI::CleanScreenshots,
					});
				} EndSection();
				if (Section("Viewmodel", 8))
				{
					FToggleGrid({
						&Vars::Visuals::Viewmodel::CrosshairAim,
						&Vars::Visuals::Viewmodel::ViewmodelAim,
					});
					// Offsets and rotations each read as a vertical X/Y/Z run; auto-pairing
					// would interleave them (OffsetX beside Pitch) and break the grouping.
					{
						StackedSliders tStacked;
						FSliderRow(Vars::Visuals::Viewmodel::OffsetX);
						FSliderRow(Vars::Visuals::Viewmodel::OffsetY);
						FSliderRow(Vars::Visuals::Viewmodel::OffsetZ);

						FSliderRow(Vars::Visuals::Viewmodel::Pitch);
						FSliderRow(Vars::Visuals::Viewmodel::Yaw);
						FSliderRow(Vars::Visuals::Viewmodel::Roll);
					}

					SubGroup("Sway");
					PushTransparent(!Vars::Visuals::Viewmodel::SwayScale.Value || !Vars::Visuals::Viewmodel::SwayInterp.Value);
					{
						FSliderRow(Vars::Visuals::Viewmodel::SwayScale);
						FSliderRow(Vars::Visuals::Viewmodel::SwayInterp);
					}
					PopTransparent();
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	// World
	case 2:
	{
		if (BeginTable("VisualsWorldTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("World"))
				{
					FDropdown(Vars::Visuals::World::Modulations);
					FSDropdown(Vars::Visuals::World::WorldTexture, FDropdownEnum::Left);
					FSDropdown(Vars::Visuals::World::SkyboxChanger, FDropdownEnum::Right);
					PushTransparent(!(Vars::Visuals::World::Modulations.Value & Vars::Visuals::World::ModulationsEnum::World));
					{
						FColorPicker(Vars::Colors::WorldModulation, FColorPickerEnum::Left);
					}
					PopTransparent();
					PushTransparent(!(Vars::Visuals::World::Modulations.Value & Vars::Visuals::World::ModulationsEnum::Sky));
					{
						FColorPicker(Vars::Colors::SkyModulation, FColorPickerEnum::Right);
					}
					PopTransparent();
					PushTransparent(!(Vars::Visuals::World::Modulations.Value & Vars::Visuals::World::ModulationsEnum::Prop));
					{
						FColorPicker(Vars::Colors::PropModulation, FColorPickerEnum::Left);
					}
					PopTransparent();
					PushTransparent(!(Vars::Visuals::World::Modulations.Value & Vars::Visuals::World::ModulationsEnum::Particle));
					{
						FColorPicker(Vars::Colors::ParticleModulation, FColorPickerEnum::Right);
					}
					PopTransparent();
					PushTransparent(!(Vars::Visuals::World::Modulations.Value & Vars::Visuals::World::ModulationsEnum::Fog));
					{
						FColorPicker(Vars::Colors::FogModulation, FColorPickerEnum::Left);
					}
					PopTransparent();

					SubGroup("Prop fade");
					FToggleRow(Vars::Visuals::World::NearPropFade, FToggleEnum::Left);
					FToggleRow(Vars::Visuals::World::NoPropFade, FToggleEnum::Right);
				} EndSection();
				if (Section("Effects"))
				{
					// https://developer.valvesoftware.com/wiki/Team_Fortress_2/Particles
					// https://forums.alliedmods.net/showthread.php?t=127111
					FSDropdown(Vars::Visuals::Effects::BulletTracer, FDropdownEnum::Left);
					FSDropdown(Vars::Visuals::Effects::CritTracer, FDropdownEnum::Right);
					FSDropdown(Vars::Visuals::Effects::MedigunBeam, FDropdownEnum::Left);
					FSDropdown(Vars::Visuals::Effects::MedigunCharge, FDropdownEnum::Right);
					FSDropdown(Vars::Visuals::Effects::ProjectileTrail, FDropdownEnum::Left);
					FDropdown(Vars::Visuals::Effects::SpellFootsteps, FDropdownEnum::Right, -10);
					FColorPicker(Vars::Colors::SpellFootstep, FColorPickerEnum::SameLine | FColorPickerEnum::NoTooltip, {}, { H::Draw.Scale(10), H::Draw.Scale(40) });
					FDropdown(Vars::Visuals::Effects::RagdollEffects);

					SubGroup("Through walls");
					FToggleRow(Vars::Visuals::Effects::DrawIconsThroughWalls, FToggleEnum::Left);
					FToggleRow(Vars::Visuals::Effects::DrawDamageNumbersThroughWalls, FToggleEnum::Right);
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Sentry Range"))
				{
					// header row: what to draw + how (style dropdown doubles as the
					// "which controls matter" gate via the transparency block below)
					FDropdown(Vars::Visuals::SentryRange::Draw, FDropdownEnum::Left, -10);
					FDropdown(Vars::Visuals::SentryRange::Style, FDropdownEnum::Right, -10);

					SubGroup("Colors", true);
					// A 4-column swatch grid: rows are the target category, columns are
					// fill / fill-through-wall / edge / edge-through-wall. The column
					// header row is what makes the swatches self-describing -- without it
					// they're four anonymous squares.
					const auto vSwatch = ImVec2{ H::Draw.Scale(13), H::Draw.Scale(13) };
					const float flLabelW = H::Draw.Scale(52);
					const float flCol = H::Draw.Scale(23);
					{
						ImVec2 vHdr = GetDrawPos() + GetCursorPos();
						PushFont(F::Render.FontSmall);
						const char* szCols[] = { "FILL", "FILL Z", "EDGE", "EDGE Z" };
						for (int i = 0; i < 4; i++)
						{
							// centre each caption over its swatch column
							float flX = vHdr.x + flLabelW + flCol * i + (vSwatch.x - CalcTextSize(szCols[i]).x) / 2.f;
							GetWindowDrawList()->AddText({ flX, vHdr.y }, F::Render.TextDim, szCols[i]);
						}
						PopFont();
						DebugDummy({ 0, H::Draw.Scale(14) });
					}
					auto ColorRow = [&](const char* sLabel, auto& tFill, auto& tFillIgnoreZ, auto& tEdge, auto& tEdgeIgnoreZ)
					{
						FText(sLabel, { 5, 4 });
						FColorPicker(tFill, FColorPickerEnum::SameLine, { H::Draw.Scale(4), H::Draw.Scale(-2) }, vSwatch);
						FColorPicker(tFillIgnoreZ, FColorPickerEnum::SameLine, {}, vSwatch);
						FColorPicker(tEdge, FColorPickerEnum::SameLine, {}, vSwatch);
						FColorPicker(tEdgeIgnoreZ, FColorPickerEnum::SameLine, {}, vSwatch);
					};
					ColorRow("Enemy", Vars::Colors::SentryRangeFillEnemy, Vars::Colors::SentryRangeFillEnemyIgnoreZ,
						Vars::Colors::SentryRangeEnemy, Vars::Colors::SentryRangeEnemyIgnoreZ);
					ColorRow("Team", Vars::Colors::SentryRangeFillTeam, Vars::Colors::SentryRangeFillTeamIgnoreZ,
						Vars::Colors::SentryRangeTeam, Vars::Colors::SentryRangeTeamIgnoreZ);
					ColorRow("Local", Vars::Colors::SentryRangeFillLocal, Vars::Colors::SentryRangeFillLocalIgnoreZ,
						Vars::Colors::SentryRangeLocal, Vars::Colors::SentryRangeLocalIgnoreZ);
					ColorRow("Inside", Vars::Colors::SentryRangeFillPlayerInside, Vars::Colors::SentryRangeFillPlayerInsideIgnoreZ,
						Vars::Colors::SentryRangePlayerInside, Vars::Colors::SentryRangePlayerInsideIgnoreZ);

					SubGroup("Sampling");
					FSliderRow(Vars::Visuals::SentryRange::GridStep, FSliderEnum::Left);
					FSliderRow(Vars::Visuals::SentryRange::Smoothing, FSliderEnum::Right, !Vars::Visuals::SentryRange::Smoothing[DEFAULT_BIND] ? "off" : "%g%%");
					FSliderRow(Vars::Visuals::SentryRange::GroundOffset, FSliderEnum::Left);
					FSliderRow(Vars::Visuals::SentryRange::MaxDistance, FSliderEnum::Right, !Vars::Visuals::SentryRange::MaxDistance[DEFAULT_BIND] ? "off" : "%g");
					FSliderRow(Vars::Visuals::SentryRange::RefreshInterval, FSliderEnum::Left);
					FSliderRow(Vars::Visuals::SentryRange::DisabledAlpha, FSliderEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug SentryRange"))
					{
						FText("Extra", { 5, 5 });
						if (FPopupButton("Extra", { 0, -5 }, -8))
						{
							FSliderRow(Vars::Visuals::SentryRange::TraceBudget, FSliderEnum::Left);
							FSliderRow(Vars::Visuals::SentryRange::TargetHeight, FSliderEnum::Right);

							EndPopup();
						}
					} EndSection();
				}
			}
			EndTable();
		}
		break;
	}
	// Menu
	case 3:
	{
		// The theme colour list used to be one giant Section() carrying every
		// sub-group as a SubGroup() heading -- a single tall column the user had to
		// scroll through. Split into one panel per sub-group, laid across the
		// standard 2-column table used by every other tab, so it reads as a grid
		// of cards instead of one long list.
		if (BeginTable("MenuThemeTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Theme"))
				{
					FColorPicker(Vars::Menu::Theme::Accent, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::Background, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::Active, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::Inactive, FColorPickerEnum::Right);
				} EndSection();
				// Every picker below drives exactly one surface; leaving one at alpha 0
				// keeps its derived value.
				if (Section("Base ramp"))
				{
					FToggleRow(Vars::Menu::Theme::BackgroundOverride);
					PushTransparent(!Vars::Menu::Theme::BackgroundOverride.Value);
					{
						FColorPicker(Vars::Menu::Theme::Background0, FColorPickerEnum::Left);
						FColorPicker(Vars::Menu::Theme::Background0p5, FColorPickerEnum::Right);
						FColorPicker(Vars::Menu::Theme::Background1, FColorPickerEnum::Left);
						FColorPicker(Vars::Menu::Theme::Background1p5, FColorPickerEnum::Right);
						FColorPicker(Vars::Menu::Theme::Background2, FColorPickerEnum::Left);
					}
					PopTransparent();
				} EndSection();
				if (Section("Panels"))
				{
					FColorPicker(Vars::Menu::Theme::PanelBackground, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::PanelHeader, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::PanelBorder, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::PanelAccent, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::PanelTitle, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::RowDivider, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::SubGroupText, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::SubGroupRule, FColorPickerEnum::Right);
				} EndSection();
				if (Section("Tabs"))
				{
					FColorPicker(Vars::Menu::Theme::TabActive, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::TabInactive, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::TabBar, FColorPickerEnum::Left);
				} EndSection();
				if (Section("General"))
				{
					FSDropdown(Vars::Menu::CheatTitle, FDropdownEnum::Left);
					FSDropdown(Vars::Menu::CheatTag, FDropdownEnum::Right);
					FKeybind(Vars::Menu::PrimaryKey, FButtonEnum::Left, { Vars::Menu::SecondaryKey[DEFAULT_BIND], VK_LBUTTON, VK_RBUTTON });
					FKeybind(Vars::Menu::SecondaryKey, FButtonEnum::Right | FButtonEnum::SameLine, { Vars::Menu::PrimaryKey[DEFAULT_BIND], VK_LBUTTON, VK_RBUTTON });
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Window"))
				{
					FColorPicker(Vars::Menu::Theme::WindowBackground, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::NavBackground, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::NavDivider, FColorPickerEnum::Left);
				} EndSection();
				if (Section("Collapsed panels"))
				{
					FColorPicker(Vars::Menu::Theme::PanelCollapsedHeader, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::PanelCollapsedBackground, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::PanelCollapsedTitle, FColorPickerEnum::Left);
				} EndSection();
				if (Section("Text"))
				{
					FColorPicker(Vars::Menu::Theme::TextDim, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::TextDisabled, FColorPickerEnum::Right);
				} EndSection();
				if (Section("Accent variants"))
				{
					FToggleRow(Vars::Menu::Theme::AccentOverride);
					PushTransparent(!Vars::Menu::Theme::AccentOverride.Value);
					{
						FColorPicker(Vars::Menu::Theme::AccentMuted, FColorPickerEnum::Left);
						FColorPicker(Vars::Menu::Theme::AccentWashed, FColorPickerEnum::Right);
					}
					PopTransparent();
				} EndSection();
				if (Section("Controls"))
				{
					FColorPicker(Vars::Menu::Theme::ControlBackground, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::ControlHovered, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::SwitchOn, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::SwitchOff, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::SwitchKnobOn, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::SwitchKnobOff, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::SliderFill, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::SliderTrack, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::SliderKnob, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::SliderValueText, FColorPickerEnum::Right);
				} EndSection();
				if (Section("Popups"))
				{
					FColorPicker(Vars::Menu::Theme::PopupBackground, FColorPickerEnum::Left);
					FColorPicker(Vars::Menu::Theme::TooltipBackground, FColorPickerEnum::Right);
					FColorPicker(Vars::Menu::Theme::TooltipText, FColorPickerEnum::Left);
				} EndSection();
			}
			EndTable();
		}

		if (BeginTable("MenuTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Indicators"))
				{
					FDropdown(Vars::Menu::Indicators);
					if (FSliderRow(Vars::Menu::Scale))
						H::Fonts.Reload();
					if (FToggleRow(Vars::Menu::CheapText))
						H::Fonts.Reload();
					FToggleRow(Vars::Menu::CompactColumns, FToggleEnum::Left);
					FToggleRow(Vars::Menu::DescriptionsOnHover, FToggleEnum::Right);
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Crit Bar"))
				{
					bool bCritBarOn = SectionToggle(Vars::Visuals::CritBar::Enabled);
					PushTransparent(!bCritBarOn, true);
					FToggleRow(Vars::Visuals::CritBar::Text);
					FSliderRow(Vars::Visuals::CritBar::Width, FSliderEnum::Left);
					FSliderRow(Vars::Visuals::CritBar::Height, FSliderEnum::Right);
					FSliderRow(Vars::Visuals::CritBar::Border, FSliderEnum::Left);

					SubGroup("Colors");
					FColorPicker(Vars::Visuals::CritBar::CellColor, FColorPickerEnum::Left);
					FColorPicker(Vars::Visuals::CritBar::ProgressColor, FColorPickerEnum::Right);
					FColorPicker(Vars::Visuals::CritBar::BannedCellColor, FColorPickerEnum::Left);
					FColorPicker(Vars::Visuals::CritBar::BannedProgressColor, FColorPickerEnum::Right);
					FColorPicker(Vars::Visuals::CritBar::BorderColor, FColorPickerEnum::Left);
					FColorPicker(Vars::Visuals::CritBar::BackgroundColor, FColorPickerEnum::Right);
					PopTransparent(1, 1);
				} EndSection();
				if (Section("Alerts"))
				{
					bool bAlertsOn = SectionToggle(Vars::Visuals::Alerts::Enabled);
					PushTransparent(!bAlertsOn, true);
					FSDropdown(Vars::Visuals::Alerts::FontName, FDropdownEnum::Left);
					FSliderRow(Vars::Visuals::Alerts::FontSize, FSliderEnum::Right);
					FToggleRow(Vars::Visuals::Alerts::FontBold, FToggleEnum::Left);
					FToggleRow(Vars::Visuals::Alerts::Outline, FToggleEnum::Right);
					FSliderRow(Vars::Visuals::Alerts::Spacing, FSliderEnum::Left);
					FColorPicker(Vars::Visuals::Alerts::Color, FColorPickerEnum::Left);
					FColorPicker(Vars::Visuals::Alerts::OutlineColor, FColorPickerEnum::Right);

					SubGroup("Sniper sightline");
					FToggleRow(Vars::Visuals::Alerts::SniperSightlineEnabled, FToggleEnum::Left);
					FSliderRow(Vars::Visuals::Alerts::SniperSightlineRadius, FSliderEnum::Right);
					FSDropdown(Vars::Visuals::Alerts::SniperSightlineText);

					SubGroup("Enemy near");
					FToggleRow(Vars::Visuals::Alerts::EnemyNearEnabled, FToggleEnum::Left);
					FToggleRow(Vars::Visuals::Alerts::EnemyNearLineOfSight, FToggleEnum::Right);
					FSliderRow(Vars::Visuals::Alerts::EnemyNearDistance);
					FSDropdown(Vars::Visuals::Alerts::EnemyNearText);
					PopTransparent(1, 1);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FColorPicker(Vars::Colors::IndicatorGood, FColorPickerEnum::Left);
							FColorPicker(Vars::Colors::IndicatorTextGood, FColorPickerEnum::Right);
							FColorPicker(Vars::Colors::IndicatorBad, FColorPickerEnum::Left);
							FColorPicker(Vars::Colors::IndicatorTextBad, FColorPickerEnum::Right);
							FColorPicker(Vars::Colors::IndicatorMid, FColorPickerEnum::Left);
							FColorPicker(Vars::Colors::IndicatorTextMid, FColorPickerEnum::Right);
							FColorPicker(Vars::Colors::IndicatorMisc, FColorPickerEnum::Left);
							FColorPicker(Vars::Colors::IndicatorTextMisc, FColorPickerEnum::Right);

							EndPopup();
						}
					}
					EndSection();
				}
			}
			EndTable();
		}
		break;
	}
	}
}

void CMenu::MenuMisc(int iTab)
{
	using namespace ImGui;

	switch (iTab)
	{
	// Player
	case 0:
	{
		if (BeginTable("MiscPlayerTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Movement"))
				{
					FDropdown(Vars::Misc::Movement::AutoStrafe);
					PushTransparent(Vars::Misc::Movement::AutoStrafe.Value != Vars::Misc::Movement::AutoStrafeEnum::Directional);
					{
						FSliderRow(Vars::Misc::Movement::AutoStrafeTurnScale, FSliderEnum::Left);
						FSliderRow(Vars::Misc::Movement::AutoStrafeMaxDelta, FSliderEnum::Right);
					}
					PopTransparent();

					SubGroup("Jumps");
					FToggleRow(Vars::Misc::Movement::Bunnyhop, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Movement::EdgeJump, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Movement::AutoJumpbug, FToggleEnum::Left); // this is unreliable without setups, do not depend on it!
					FToggleRow(Vars::Misc::Movement::BreakJump, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Movement::AutoRocketJump, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Movement::AutoFaNJump, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Movement::AutoRevJump, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Movement::AutoCTap, FToggleEnum::Right);

					SubGroup("Speed");
					FToggleRow(Vars::Misc::Movement::FastStop, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Movement::FastAccelerate, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Movement::DuckSpeed, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Movement::AirCrouch, FToggleEnum::Right);

					SubGroup("Misc");
					FToggleRow(Vars::Misc::Movement::ShieldTurnRate, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Movement::NoPush, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Movement::MovementLock, FToggleEnum::Left);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FSliderRow(Vars::Misc::Movement::AutoRocketJumpChokeGrounded, FToggleEnum::Left);
							FSliderRow(Vars::Misc::Movement::AutoRocketJumpChokeAir, FToggleEnum::Right);
							FSliderRow(Vars::Misc::Movement::AutoRocketJumpSkipGround, FToggleEnum::Left);
							FSliderRow(Vars::Misc::Movement::AutoRocketJumpSkipAir, FToggleEnum::Right);
							FSliderRow(Vars::Misc::Movement::AutoRocketJumpTimingOffset, FToggleEnum::Left);
							FSliderRow(Vars::Misc::Movement::AutoRocketJumpApplyAbove, FToggleEnum::Right);

							EndPopup();
						}
					} EndSection();
				}
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Automation"))
				{
					SubGroup("Protection", true);
					FDropdown(Vars::Misc::Automation::AntiBackstab); // pitch/fake _might_ slip up some auto backstabs
					FToggleRow(Vars::Misc::Automation::AntiAutobalance, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Automation::AntiAFK, FToggleEnum::Right);

					SubGroup("Auto actions");
					FToggleRow(Vars::Misc::Automation::TauntControl, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Automation::KartControl, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Automation::AutoF2Ignored, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Automation::AutoF1Priority, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Automation::AcceptItemDrops);
				} EndSection();
				if (Section("Mann vs. Machine", 8))
				{
					FToggleRow(Vars::Misc::MannVsMachine::InstantRespawn, FToggleEnum::Left);
					FToggleRow(Vars::Misc::MannVsMachine::InstantRevive, FToggleEnum::Right);
					FToggleRow(Vars::Misc::MannVsMachine::AllowInspect);
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	// Game
	case 1:
	{
		if (BeginTable("MiscGameTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Performance"))
				{
					FToggleRow(Vars::Misc::Game::SetupBonesOptimization, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Game::AttributeCacheOptimization, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Game::OriginalChamsOptimization, FToggleEnum::Left);
					FSliderRow(Vars::Misc::Game::CosmeticCullDistance, FSliderEnum::Right, !Vars::Misc::Game::CosmeticCullDistance[DEFAULT_BIND] ? "off" : "%d");
					FSliderRow(Vars::Misc::Game::GlowResolution);
				} EndSection();
				if (Section("Network", 8))
				{
					FToggleRow(Vars::Misc::Game::NetworkFix, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Game::AntiCheatCompatibility, FToggleEnum::Right);
				} EndSection();
				if (Vars::Debug::Options.Value)
				{
					if (Section("##Debug AntiCheat"))
					{
						FText("Debug", { 5, 5 });
						if (FPopupButton("Debug", { 0, -5 }))
						{
							FToggleRow(Vars::Misc::Game::AntiCheatCritHack);

							EndPopup();
						}
					} EndSection();
				}
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Exploits", 8))
				{
					FToggleRow(Vars::Misc::Exploits::PureBypass, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Exploits::CheatsBypass, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Exploits::UnlockCVars, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Exploits::EquipRegionUnlock, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Exploits::BackpackExpander, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Exploits::NoisemakerSpam, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Exploits::PingReducer, FToggleEnum::Left);
					PushTransparent(!Vars::Misc::Exploits::PingReducer.Value);
					{
						FSliderRow(Vars::Misc::Exploits::PingTarget, FSliderEnum::Right);
					}
					PopTransparent();
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	// Misc
	case 2:
	{
		if (BeginTable("MiscMiscTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Queueing"))
				{
					FDropdown(Vars::Misc::Queueing::ForceRegions);
					FToggleRow(Vars::Misc::Queueing::ExtendQueue, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Queueing::AutoCasualQueue, FToggleEnum::Right);
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Sound"))
				{
					FDropdown(Vars::Misc::Sound::Block);
					FToggleRow(Vars::Misc::Sound::HitsoundAlways, FToggleEnum::Left);
					FToggleRow(Vars::Misc::Sound::RemoveDSP, FToggleEnum::Right);
					FToggleRow(Vars::Misc::Sound::GiantWeaponSounds);
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	}
}

void CMenu::MenuLogs(int iTab)
{
	using namespace ImGui;

	switch (iTab)
	{
	// PlayerList
	case 0:
	{
		if (Section("Players"))
		{
			if (I::EngineClient->IsInGame())
			{
				std::lock_guard tLock(m_tMutex);
				const auto& vPlayers = F::PlayerUtils.m_vPlayerCache;

				std::unordered_map<uint64_t, std::vector<const ListPlayer*>> mParties = {};
				int iPartyCount = 0;
				for (auto& tPlayer : vPlayers)
				{
					if (tPlayer.m_iParty)
					{
						mParties[tPlayer.m_iParty].push_back(&tPlayer);
						iPartyCount = std::max(iPartyCount, tPlayer.m_iParty);
					}
				}

				auto fGetTeamColor = [&](int iTeam, bool bAlive)
				{
					switch (iTeam)
					{
					case 3: return Color_t(100, 150, 200, bAlive ? 255 : 127).Lerp(Vars::Menu::Theme::Background.Value, 0.5f, LerpEnum::NoAlpha);
					case 2: return Color_t(255, 100, 100, bAlive ? 255 : 127).Lerp(Vars::Menu::Theme::Background.Value, 0.5f, LerpEnum::NoAlpha);
					}
					return Color_t(127, 127, 127, 255).Lerp(Vars::Menu::Theme::Background.Value, 0.5f, LerpEnum::NoAlpha);
				};
				auto fDrawPlayer = [&](const ListPlayer& tPlayer, int x, int y)
				{
					ImVec2 vOriginalPos = { !x ? GetStyle().WindowPadding.x : GetWindowWidth() / 2 + GetStyle().WindowPadding.x / 2, H::Draw.Scale(35 + 36 * y) };

					// background
					float flWidth = GetWindowWidth() / 2 - GetStyle().WindowPadding.x * 1.5f;
					float flHeight = H::Draw.Scale(28);
					ImColor tColor = ColorByteToFloat(fGetTeamColor(tPlayer.m_iTeam, tPlayer.m_bAlive));
					ImVec2 vDrawPos = GetDrawPos() + vOriginalPos;
					GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flWidth, flHeight), tColor, H::Draw.Scale(4));

					// tag bar
					bool bPopup = false;
					bool bIcon = tPlayer.m_bLocal || F::Spectate.GetTarget(true) == tPlayer.m_iUserID || tPlayer.m_bFriend || tPlayer.m_bParty;
					float flBarWidth = 0.f;

					if (!tPlayer.m_bFake)
					{
						std::vector<PriorityLabel_t> vLabels = {};
						std::vector<std::pair<PriorityLabel_t*, int>> vTags = {};
						if (int iParty = tPlayer.m_iParty)
						{
							auto pTag = &F::PlayerUtils.m_vTags[F::PlayerUtils.TagToIndex(PARTY_TAG)];
							if (!--iParty)
								vTags.emplace_back(pTag, 0);
							else
								vLabels.emplace_back(std::format("{}: {}", pTag->m_sName, iParty), pTag->m_tColor.HueShift(iParty * 360.f / iPartyCount));
						}
						if (tPlayer.m_bF2P)
						{
							auto pTag = &F::PlayerUtils.m_vTags[F::PlayerUtils.TagToIndex(F2P_TAG)];
							vTags.emplace_back(pTag, 0);
						}
						for (auto& iID : F::PlayerUtils.GetPlayerTags(tPlayer.m_uAccountID))
						{
							if (auto pTag = F::PlayerUtils.GetTag(iID))
								vTags.emplace_back(pTag, iID);
						}

						if (!vLabels.empty() || !vTags.empty())
						{
							PushFont(F::Render.FontSmall);
							flBarWidth = H::Draw.Scale(4);
							for (auto& tTag : vLabels)
								flBarWidth += FCalcTextSize(tTag.m_sName.c_str()).x + H::Draw.Scale(14);
							for (auto& [pTag, iID] : vTags)
								flBarWidth += FCalcTextSize(pTag->m_sName.c_str()).x + H::Draw.Scale(iID ? 29 : 14);
							flBarWidth = std::min(flBarWidth, std::max(flWidth - FCalcTextSize(tPlayer.m_sName.c_str(), F::Render.FontRegular).x - H::Draw.Scale(bIcon ? 33 : 14), flWidth / 2));

							SetCursorPos(vOriginalPos + ImVec2(flWidth - floorf(flBarWidth), 0));
							if (BeginChild(std::format("TagBar{}", tPlayer.m_iUserID).c_str(), { flBarWidth, flHeight }, ImGuiWindowFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground))
							{
								const auto vDrawPos = GetDrawPos();
								float flTagOffset = H::Draw.Scale(4);
								auto fDrawTag = [&](PriorityLabel_t& tTag, int iID)
								{
									ImColor tTagColor = ColorByteToFloat(tTag.m_tColor);
									float flTagWidth = FCalcTextSize(tTag.m_sName.c_str()).x + H::Draw.Scale(!iID ? 10 : 25);
									float flTagHeight = H::Draw.Scale(20);
									ImVec2 vTagPos = { flTagOffset, H::Draw.Scale(4) };

									GetWindowDrawList()->AddRectFilled(vDrawPos + vTagPos, vDrawPos + vTagPos + ImVec2(flTagWidth, flTagHeight), tTagColor, H::Draw.Scale(4));
									SetCursorPos(vTagPos + ImVec2(H::Draw.Scale(5), H::Draw.Scale(3)));
									TextColored(ColorFloatToByte(tColor).Blend(tTag.m_tColor).IsColorBright() ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 1), tTag.m_sName.c_str());
									if (iID)
									{
										SetCursorPos(vTagPos + ImVec2(flTagWidth - H::Draw.Scale(22), H::Draw.Scale(-2)));
										if (IconButton(ICON_MD_CANCEL))
											F::PlayerUtils.RemoveTag(tPlayer.m_uAccountID, iID, true, tPlayer.m_sName.c_str());
									}

									flTagOffset += flTagWidth + H::Draw.Scale(4);
								};

								for (auto& tTag : vLabels)
									fDrawTag(tTag, 0);
								for (auto& [pTag, iID] : vTags)
									fDrawTag(*pTag, iID);
								SetCursorPosX(flTagOffset); DebugDummy({});
							} EndChild();
							PopFont();

							bPopup = IsItemHovered() && IsMouseReleased(ImGuiMouseButton_Right);
						}
					}

					// text + icons
					int lOffset = H::Draw.Scale(10);
					if (bIcon)
					{
						lOffset += H::Draw.Scale(19);
						SetCursorPos(vOriginalPos + ImVec2(H::Draw.Scale(7), H::Draw.Scale(6)));
						if (tPlayer.m_bLocal)
							IconImage(ICON_MD_PERSON);
						else if (F::Spectate.GetTarget(true) == tPlayer.m_iUserID)
							IconImage(ICON_MD_VISIBILITY);
						else if (tPlayer.m_bFriend)
							IconImage(ICON_MD_GROUP);
						else if (tPlayer.m_bParty)
							IconImage(ICON_MD_GROUPS);
					}
					SetCursorPos(vOriginalPos + ImVec2(lOffset, H::Draw.Scale(7)));
					auto sName = TruncateText(tPlayer.m_sName, flWidth - lOffset - flBarWidth);
					FText(sName.c_str());
					lOffset += FCalcTextSize(sName.c_str()).x + H::Draw.Scale(8);

					// buttons
					SetCursorPos(vOriginalPos);
					Button(std::format("##{}", tPlayer.m_iUserID).c_str(), { flWidth, flHeight });
					bPopup |= IsItemHovered() && IsMouseReleased(ImGuiMouseButton_Right);

					// popups
					if (bPopup)
						OpenPopup(std::format("RightClicked{}", tPlayer.m_iUserID).c_str());

					if (FBeginPopup(std::format("RightClicked{}", tPlayer.m_iUserID).c_str()))
					{
						PushStyleVar(ImGuiStyleVar_ItemSpacing, { H::Draw.Scale(8), H::Draw.Scale(8) });

						if (!tPlayer.m_bFake)
						{
							if (FSelectable("Profile"))
								I::SteamFriends->ActivateGameOverlayToUser("steamid", CSteamID(tPlayer.m_uAccountID, k_EUniversePublic, k_EAccountTypeIndividual));
							if (FSelectable("History"))
								I::SteamFriends->ActivateGameOverlayToWebPage(std::format("https://steamhistory.net/id/{}", CSteamID(tPlayer.m_uAccountID, k_EUniversePublic, k_EAccountTypeIndividual).ConvertToUint64()).c_str());
						}

						if (FSelectable(F::Spectate.GetTarget(true) == tPlayer.m_iUserID ? "Unspectate" : "Spectate"))
							F::Spectate.SetTarget(tPlayer.m_iUserID);

						if (!I::EngineClient->IsPlayingDemo() && FBeginMenu("Votekick"))
						{
							if (IsItemHovered() && IsMouseDown(ImGuiMouseButton_Left))
							{
								I::ClientState->SendStringCmd(std::format("callvote Kick \"{}\"", tPlayer.m_iUserID).c_str());
								CloseCurrentPopup();
							}
							if (FSelectable("No reason"))
								I::ClientState->SendStringCmd(std::format("callvote Kick \"{} other\"", tPlayer.m_iUserID).c_str());
							if (FSelectable("Cheating"))
								I::ClientState->SendStringCmd(std::format("callvote Kick \"{} cheating\"", tPlayer.m_iUserID).c_str());
							if (FSelectable("Idle"))
								I::ClientState->SendStringCmd(std::format("callvote Kick \"{} idle\"", tPlayer.m_iUserID).c_str());
							if (FSelectable("Scamming"))
								I::ClientState->SendStringCmd(std::format("callvote Kick \"{} scamming\"", tPlayer.m_iUserID).c_str());

							ImGui::EndMenu();
						}

						if (!tPlayer.m_bFake)
						{
							if (FBeginMenu("Add tag"))
							{
								for (auto it = F::PlayerUtils.m_vTags.begin(); it != F::PlayerUtils.m_vTags.end(); it++)
								{
									int iID = std::distance(F::PlayerUtils.m_vTags.begin(), it);
									auto& tTag = *it;
									if (!tTag.m_bAssignable || F::PlayerUtils.HasTag(tPlayer.m_uAccountID, iID))
										continue;

									ImVec4 tColor = ColorByteToVec(tTag.m_tColor);
									PushStyleColor(ImGuiCol_Text, tColor);
									tColor.x /= 3; tColor.y /= 3; tColor.z /= 3;
									if (FSelectable(tTag.m_sName.c_str(), tColor))
										F::PlayerUtils.AddTag(tPlayer.m_uAccountID, iID, true, tPlayer.m_sName.c_str());
									PopStyleColor();
								}

								ImGui::EndMenu();
							}
							if (FBeginMenu("Alias"))
							{
								bool bHasAlias = F::PlayerUtils.m_mPlayerAliases.contains(tPlayer.m_uAccountID);
								static std::string sInput = "";

								bool bEnter = FInputText("Alias...", sInput, H::Draw.Scale(150), ImGuiInputTextFlags_EnterReturnsTrue);
								if (!IsItemFocused())
									sInput = bHasAlias ? F::PlayerUtils.m_mPlayerAliases[tPlayer.m_uAccountID] : "";
								if (bEnter)
								{
									if (sInput.empty() && bHasAlias)
									{
										F::Output.AliasChanged(tPlayer.m_sName.c_str(), "Removed", F::PlayerUtils.m_mPlayerAliases[tPlayer.m_uAccountID].c_str());

										F::PlayerUtils.m_mPlayerAliases.erase(tPlayer.m_uAccountID);
										F::PlayerUtils.m_bSave = true;
									}
									else if (!sInput.empty())
									{
										F::PlayerUtils.m_mPlayerAliases[tPlayer.m_uAccountID] = sInput;
										F::PlayerUtils.m_bSave = true;

										F::Output.AliasChanged(tPlayer.m_sName.c_str(), bHasAlias ? "Changed" : "Added", sInput.c_str());
									}
								}

								ImGui::EndMenu();
							}
						}

						if (Vars::Resolver::Enabled.Value && !tPlayer.m_bLocal && !I::EngineClient->IsPlayingDemo())
						{
							if (FBeginMenu("Set yaw"))
							{
								static std::vector<std::pair<const char*, float>> vYaws = {
									{ "Auto", 0.f },
									{ "Forward", 0.f },
									{ "Left", 90.f },
									{ "Right", -90.f },
									{ "Backwards", 180.f }
								};
								for (auto& [sYaw, flValue] : vYaws)
								{
									if (FSelectable(sYaw))
									{
										switch (FNV1A::Hash32(sYaw))
										{
										case FNV1A::Hash32Const("Auto"):
											F::Resolver.SetYaw(tPlayer.m_iUserID, 0.f, true);
											break;
										default:
											F::Resolver.SetYaw(tPlayer.m_iUserID, flValue);
										}
									}
								}

								ImGui::EndMenu();
							}
							if (FBeginMenu("Set pitch"))
							{
								static std::vector<std::pair<const char*, float>> vPitches = {
									{ "Auto", 0.f },
									{ "Up", -90.f },
									{ "Down", 90.f },
									{ "Zero", 0.f },
									{ "Inverse", 0.f }
								};
								for (auto& [sPitch, flValue] : vPitches)
								{
									if (FSelectable(sPitch))
									{
										switch (FNV1A::Hash32(sPitch))
										{
										case FNV1A::Hash32Const("Auto"):
											F::Resolver.SetPitch(tPlayer.m_iUserID, 0.f, false, true);
											break;
										case FNV1A::Hash32Const("Inverse"):
											F::Resolver.SetPitch(tPlayer.m_iUserID, 0.f, true);
											break;
										default:
											F::Resolver.SetPitch(tPlayer.m_iUserID, flValue);
										}
									}
								}

								ImGui::EndMenu();
							}
							if (FBeginMenu("Set view"))
							{
								static std::vector<std::pair<const char*, bool>> vPitches = {
									{ "Offset from static view", true },
									{ "Offset from view to local", false }
								};
								for (auto& [sPitch, bValue] : vPitches)
								{
									if (FSelectable(sPitch))
										F::Resolver.SetView(tPlayer.m_iUserID, bValue);
								}

								ImGui::EndMenu();
							}
							if (FBeginMenu("Set minwalk"))
							{
								static std::vector<std::pair<const char*, bool>> vPitches = {
									{ "Minwalk on", true },
									{ "Minwalk off", false }
								};
								for (auto& [sPitch, bValue] : vPitches)
								{
									if (FSelectable(sPitch))
										F::Resolver.SetMinwalk(tPlayer.m_iUserID, bValue);
								}

								ImGui::EndMenu();
							}
						}

						if (mParties.contains(tPlayer.m_iParty))
						{
							Divider(H::Draw.Scale(1), 0);

							TextColored(F::Render.Inactive.Value, "Partied:");
							for (auto& pPlayer2 : mParties[tPlayer.m_iParty])
								TextColored(F::Render.Inactive.Value, pPlayer2->m_sName.c_str());
						}

						if (tPlayer.m_iLevel != -2)
						{
							Divider(H::Draw.Scale(1), 0);

							std::string sLevel = "T? L?";
							if (tPlayer.m_iLevel != -1)
							{
								int iTier = std::max(ceilf(tPlayer.m_iLevel / 150.f), 1.f);
								int iLevel = ((tPlayer.m_iLevel - 1) % 150) + 1;
								sLevel = std::format("T{} L{}", iTier, iLevel);
							}
							TextColored(F::Render.Inactive.Value, sLevel.c_str());
						}

						PopStyleVar();
						EndPopup();
					}
				};

				// display players
				std::vector<ListPlayer> vBlu, vRed, vOther;
				for (auto& tPlayer : vPlayers)
				{
					switch (tPlayer.m_iTeam)
					{
					case 3: vBlu.push_back(tPlayer); break;
					case 2: vRed.push_back(tPlayer); break;
					default: vOther.push_back(tPlayer); break;
					}
				}

				int iBlu = 0, iRed = 0;
				for (size_t i = 0; i < vBlu.size(); i++)
				{
					fDrawPlayer(vBlu[i], 0, int(i));
					iBlu++;
				}
				for (size_t i = 0; i < vRed.size(); i++)
				{
					fDrawPlayer(vRed[i], 1, int(i));
					iRed++;
				}
				if (vOther.empty())
				{
					SetCursorPos({ 0, H::Draw.Scale(36 * std::max(iBlu, iRed) - 1) }); DebugDummy({ 0, H::Draw.Scale(28) });
				}
				else
				{
					size_t iMax = std::max(iBlu, iRed);
					for (size_t i = 0; i < vOther.size(); i++)
						fDrawPlayer(vOther[i], i % 2, int(iMax + i / 2));
				}
			}
			else
			{
				SetCursorPos({ H::Draw.Scale(15), H::Draw.Scale(40) });
				FText("Not ingame");
				DebugDummy({ 0, H::Draw.Scale(8) });
			}
		} EndSection();
		if (Section("Tags"))
		{
			static int iID = -1;
			static PriorityLabel_t tTag = {};

			auto vTable = WidgetTable(3, H::Draw.Scale(48), { GetWindowWidth() / 2, GetWindowWidth() / 2 - H::Draw.Scale(90) - GetStyle().WindowPadding.x });

			if (BeginWidgetTable(0, vTable))
			{
				FSDropdown("Name", &tTag.m_sName, {}, FDropdownEnum::Left | FSDropdownEnum::AutoUpdate, -10);
				FColorPicker("Color", &tTag.m_tColor, FColorPickerEnum::SameLine, {}, { H::Draw.Scale(10), H::Draw.Scale(40) });

				PushDisabled(iID == DEFAULT_TAG || iID == IGNORED_TAG);
				{
					int iLabel = Disabled ? 0 : tTag.m_bLabel;
					FDropdown("Type", &iLabel, { "Priority", "Label" }, {}, FDropdownEnum::Right);
					tTag.m_bLabel = iLabel;
					if (Disabled)
						tTag.m_bLabel = false;
				}
				PopDisabled();
			} EndChild();

			if (BeginWidgetTable(1, vTable))
			{
				PushTransparent(tTag.m_bLabel); // transparent if we want a label, user can still use to sort
				{
					SetCursorPosY(GetCursorPos().y + H::Draw.Scale(12));
					FSlider("Priority", &tTag.m_iPriority, -10, 10);
				}
				PopTransparent();
			} EndChild();

			if (BeginWidgetTable(2, vTable))
			{
				// create/modify button
				bool bCreate = false, bClear = false;

				SetCursorPos({ GetWindowWidth() - H::Draw.Scale(95), 0 });
				PushDisabled(tTag.m_sName.empty());
				{
					bCreate = FButton(iID != -1 ? ICON_MD_SETTINGS : ICON_MD_ADD, FButtonEnum::None, { 40, 40 }, 0, F::Render.IconFont);
				}
				PopDisabled();

				// clear button
				SetCursorPos({ GetWindowWidth() - H::Draw.Scale(47), 0 });
				bClear = FButton(ICON_MD_CLEAR, FButtonEnum::None, { 40, 40 }, 0, F::Render.IconFont);

				if (bCreate)
				{
					F::PlayerUtils.m_bSave = true;
					if (iID > -1 || iID < F::PlayerUtils.m_vTags.size())
					{
						F::PlayerUtils.m_vTags[iID].m_sName = tTag.m_sName;
						F::PlayerUtils.m_vTags[iID].m_tColor = tTag.m_tColor;
						F::PlayerUtils.m_vTags[iID].m_iPriority = tTag.m_iPriority;
						F::PlayerUtils.m_vTags[iID].m_bLabel = tTag.m_bLabel;
					}
					else
						F::PlayerUtils.m_vTags.push_back(tTag);
				}
				if (bCreate || bClear)
				{
					iID = -1;
					tTag = {};
				}
			} EndChild();

			auto fDrawTag = [](std::vector<PriorityLabel_t>::iterator it, PriorityLabel_t& _tTag, int y)
			{
				int _iID = std::distance(F::PlayerUtils.m_vTags.begin(), it);

				ImVec2 vOriginalPos = { !_tTag.m_bLabel ? GetStyle().WindowPadding.x : GetWindowWidth() * 2 / 3 + GetStyle().WindowPadding.x / 2, H::Draw.Scale(96 + 36 * y) };

				// background
				float flWidth = GetWindowWidth() * (_tTag.m_bLabel ? 1.f / 3 : 2.f / 3) - GetStyle().WindowPadding.x * 1.5f;
				float flHeight = H::Draw.Scale(28);
				ImColor tColor = ColorByteToFloat(_tTag.m_tColor.Lerp(Vars::Menu::Theme::Background.Value, 0.5f, LerpEnum::NoAlpha));
				ImVec2 vDrawPos = GetDrawPos() + vOriginalPos;
				if (iID != _iID)
					GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flWidth, flHeight), tColor, H::Draw.Scale(4));
				else
				{
					ImColor tColor2 = { tColor.Value.x * 1.1f, tColor.Value.y * 1.1f, tColor.Value.z * 1.1f, tColor.Value.w };
					GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flWidth, flHeight), tColor2, H::Draw.Scale(4));

					tColor2 = ColorByteToFloat(_tTag.m_tColor.Lerp(Vars::Menu::Theme::Background.Value, 0.25f, LerpEnum::NoAlpha));
					float flInset = H::Draw.Scale(0.5f) - 0.5f;
					GetWindowDrawList()->AddRect(vDrawPos + ImVec2(flInset, flInset), vDrawPos + ImVec2(flWidth - flInset, flHeight - flInset), tColor2, H::Draw.Scale(4), ImDrawFlags_None, H::Draw.Scale());
				}

				// text
				SetCursorPos(vOriginalPos + ImVec2(H::Draw.Scale(9), H::Draw.Scale(7)));
				FText(TruncateText(_tTag.m_sName, _tTag.m_bLabel ? flWidth - H::Draw.Scale(38) : flWidth / 2 - H::Draw.Scale(20)).c_str());

				if (!_tTag.m_bLabel)
				{
					SetCursorPos(vOriginalPos + ImVec2(flWidth / 2, H::Draw.Scale(7)));
					FText(std::format("{}", _tTag.m_iPriority).c_str());
				}

				// buttons / icons
				bool bDelete = false;
				if (!_tTag.m_bLocked)
				{
					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(26), H::Draw.Scale(2)));
					bDelete = IconButton(ICON_MD_DELETE);
				}
				else
				{
					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(22), H::Draw.Scale(6)));
					switch (F::PlayerUtils.IndexToTag(_iID))
					{
						//case DEFAULT_TAG: // no image
					case IGNORED_TAG: IconImage(ICON_MD_DO_NOT_DISTURB); break;
					case CHEATER_TAG: IconImage(ICON_MD_FLAG); break;
					case FRIEND_TAG: IconImage(ICON_MD_GROUP); break;
					case PARTY_TAG: IconImage(ICON_MD_GROUPS); break;
					case F2P_TAG: IconImage(ICON_MD_MONEY_OFF); break;
					}
				}

				SetCursorPos(vOriginalPos);
				bool bClicked = Button(std::format("##{}", _tTag.m_sName).c_str(), { flWidth, flHeight });
				bool bPopup = IsItemClicked(ImGuiMouseButton_Right);

				if (bClicked)
				{
					iID = _iID;
					tTag.m_sName = _tTag.m_sName;
					tTag.m_tColor = _tTag.m_tColor;
					tTag.m_iPriority = _tTag.m_iPriority;
					tTag.m_bLabel = _tTag.m_bLabel;
				}
				else if (bPopup)
					OpenPopup(std::format("RightClicked{}", _iID).c_str());
				else if (bDelete)
					OpenPopup(std::format("DeleteTag{}", _iID).c_str());

				if (FBeginPopup(std::format("RightClicked{}", _iID).c_str()))
				{
					PushStyleVar(ImGuiStyleVar_ItemSpacing, { H::Draw.Scale(8), 0 });

					auto& _tTag2 = *it;
					bool bSave = false;

					{
						static std::string sInput = "";

						bool bEnter = FInputText("Name...", sInput, H::Draw.Scale(284), ImGuiInputTextFlags_EnterReturnsTrue);
						if (!IsItemFocused())
							sInput = _tTag2.m_sName;
						if (bEnter)
						{
							_tTag2.m_sName = sInput;
							bSave = true;
						}
					}

					PushDisabled(_iID == DEFAULT_TAG || _iID == IGNORED_TAG);
					{
						int iLabel = Disabled ? 0 : _tTag2.m_bLabel;
						if (FDropdown("Type##", &iLabel, { "Priority", "Label" }))
							bSave = true;
						_tTag2.m_bLabel = iLabel;
						if (Disabled)
							_tTag2.m_bLabel = false;
					}
					PopDisabled();
					if (FSlider("Priority##", &_tTag2.m_iPriority, -10, 10))
						bSave = true;

					if (bSave)
						F::PlayerUtils.m_bSave = true;

					PopStyleVar();
					EndPopup();
				}
				else if (FBeginPopupModal(std::format("DeleteTag{}", _iID).c_str()))
				{
					FText(std::format("Do you really want to delete '{}'?", _tTag.m_sName).c_str());

					if (FButton("Yes", FButtonEnum::Left))
					{
						F::PlayerUtils.m_vTags.erase(it);
						F::PlayerUtils.m_bSave = F::PlayerUtils.m_bSave = true;

						for (auto& vTags : F::PlayerUtils.m_mPlayerTags | std::views::values)
						{
							for (auto it = vTags.begin(); it != vTags.end();)
							{
								if (_iID == *it)
									vTags.erase(it);
								else
								{
									if (_iID < *it)
										(*it)--;
									it++;
								}
							}
						}

						if (iID == _iID)
						{
							iID = -1;
							tTag = {};
						}
						else if (iID > _iID)
							iID--;

						CloseCurrentPopup();
					}
					if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
						CloseCurrentPopup();

					EndPopup();
				}
			};

			PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
			SetCursorPos({ H::Draw.Scale(13), H::Draw.Scale(80) }); FText("Priorities");
			SetCursorPos({ GetWindowWidth() * 2 / 3 + H::Draw.Scale(9), H::Draw.Scale(80) }); FText("Labels");
			PopStyleColor();

			std::vector<std::pair<std::vector<PriorityLabel_t>::iterator, PriorityLabel_t>> vPriorities = {}, vLabels = {};
			for (auto it = F::PlayerUtils.m_vTags.begin(); it != F::PlayerUtils.m_vTags.end(); it++)
			{
				auto& _tTag = *it;

				if (!_tTag.m_bLabel)
					vPriorities.emplace_back(it, _tTag);
				else
					vLabels.emplace_back(it, _tTag);
			}

			std::sort(vPriorities.begin(), vPriorities.end(), [&](const auto& a, const auto& b) -> bool
			{
				// override for default tag
				if (std::distance(F::PlayerUtils.m_vTags.begin(), a.first) == DEFAULT_TAG)
					return true;
				if (std::distance(F::PlayerUtils.m_vTags.begin(), b.first) == DEFAULT_TAG)
					return false;

				// sort by priority if unequal
				if (a.second.m_iPriority != b.second.m_iPriority)
					return a.second.m_iPriority > b.second.m_iPriority;

				return a.second.m_sName < b.second.m_sName;
			});
			std::sort(vLabels.begin(), vLabels.end(), [&](const auto& a, const auto& b) -> bool
			{
				// sort by priority if unequal
				if (a.second.m_iPriority != b.second.m_iPriority)
					return a.second.m_iPriority > b.second.m_iPriority;

				return a.second.m_sName < b.second.m_sName;
			});

			// display tags
			int iPriorities = 0, iLabels = 0;
			for (auto& [it, _tTag] : vPriorities)
			{
				fDrawTag(it, _tTag, iPriorities);
				iPriorities++;
			}
			for (auto& [it, _tTag] : vLabels)
			{
				fDrawTag(it, _tTag, iLabels);
				iLabels++;
			}
			SetCursorPos({ 0, H::Draw.Scale(60 + 36 * std::max(iPriorities, iLabels)) }); DebugDummy({ 0, H::Draw.Scale(28) });
		} EndSection();
		{
			PushDisabled(F::PlayerUtils.m_bLoad);
			{
				SetCursorPosY(GetCursorPosY() - H::Draw.Scale(8));
				if (FButton(ICON_MD_SYNC, FButtonEnum::None, { 30, 30 }, 0, F::Render.IconFont))
					F::PlayerUtils.m_bLoad = true;

				if (FButton(ICON_MD_FOLDER, FButtonEnum::Fit | FButtonEnum::SameLine, { 30, 30 }, 0, F::Render.IconFont))
					ShellExecuteA(NULL, NULL, F::Configs.m_sCorePath.c_str(), NULL, NULL, SW_SHOWNORMAL);

				if (FButton("Export", FButtonEnum::Fit | FButtonEnum::SameLine))
				{
					// this should be up2date anyways
					std::ifstream fStream(F::Configs.m_sCorePath + "Players.json", std::ios_base::app);
					if (fStream.is_open())
					{
						std::string sString;
						{
							std::string line;
							while (std::getline(fStream, line))
								sString += line + "\n";
							if (!sString.empty())
								sString.pop_back();
						}
						fStream.close();

						SDK::SetClipboard(sString);
						SDK::Output("Amalgam", "Copied playerlist to clipboard", INFO_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG, ICON_MD_INFO);
					}
				}

				{
					static std::vector<PriorityLabel_t> vTags = {};
					static std::unordered_map<uint32_t, std::vector<int>> mPlayerTags = {};
					static std::unordered_map<uint32_t, std::string> mPlayerAliases = {};
					static std::unordered_map<int, int> mAs = {};

					if (FButton("Import", FButtonEnum::Fit | FButtonEnum::SameLine))
					{
						try
						{
							// will not directly support older tag systems
							boost::property_tree::ptree tRead;
							std::stringstream ssStream;
							ssStream << SDK::GetClipboard();
							read_json(ssStream, tRead);

							mPlayerTags.clear();
							mPlayerAliases.clear();
							mAs.clear();
							vTags = {
								{ "Default", { 200, 200, 200, 255 }, 0, false, false, true },
								{ "Ignored", { 200, 200, 200, 255 }, -1, false, true, true },
								{ "Cheater", { 255, 100, 100, 255 }, 1, false, true, true },
								{ "Friend", { 100, 255, 100, 255 }, 0, true, false, true },
								{ "Party", { 100, 100, 255, 255 }, 0, true, false, true },
								{ "F2P", { 255, 255, 255, 255 }, 0, true, false, true }
							};

							if (auto tSub = tRead.get_child_optional("Config"))
							{
								for (auto& [sName, tChild] : *tSub)
								{
									PriorityLabel_t tTag = {};
									F::Configs.LoadJson(tChild, "Name", tTag.m_sName);
									F::Configs.LoadJson(tChild, "Color", tTag.m_tColor);
									F::Configs.LoadJson(tChild, "Priority", tTag.m_iPriority);
									F::Configs.LoadJson(tChild, "Label", tTag.m_bLabel);

									int iID = F::PlayerUtils.TagToIndex(std::stoi(sName));
									if (iID > -1 && iID < vTags.size())
									{
										vTags[iID].m_sName = tTag.m_sName;
										vTags[iID].m_tColor = tTag.m_tColor;
										vTags[iID].m_iPriority = tTag.m_iPriority;
										vTags[iID].m_bLabel = tTag.m_bLabel;
									}
									else
										vTags.push_back(tTag);
								}
							}

							if (auto tSub = tRead.get_child_optional("Tags"))
							{
								for (auto& [sName, tChild] : *tSub)
								{
									uint32_t uAccountID = std::stoul(sName);
									for (auto& tTag : tChild | std::views::values)
									{
										const std::string& sTag = tTag.data();

										int iID = F::PlayerUtils.TagToIndex(std::stoi(sTag));
										auto pTag = F::PlayerUtils.GetTag(iID);
										if (!pTag || !pTag->m_bAssignable)
											continue;

										if (!F::PlayerUtils.HasTag(uAccountID, iID, mPlayerTags))
											F::PlayerUtils.AddTag(uAccountID, iID, false, "", mPlayerTags);
									}
								}
							}

							if (auto tSub = tRead.get_child_optional("Aliases"))
							{
								for (auto& [sName, tAlias] : *tSub)
								{
									uint32_t uAccountID = std::stoul(sName);
									const std::string& sAlias = tAlias.data();

									if (!sAlias.empty())
										mPlayerAliases[uAccountID] = sAlias;
								}
							}

							for (int i = 0; i < vTags.size(); i++)
							{
								if (vTags[i].m_bAssignable)
								{
									if (F::PlayerUtils.IndexToTag(i) <= 0)
										mAs[i] = i;
									else
										mAs[i] = -1;
								}
							}
							OpenPopup("ImportPlayerlist");
						}
						catch (...)
						{
							SDK::Output("Amalgam", "Failed to import playerlist", ERROR_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG, ICON_MD_CANCEL);
						}
					}

					SetNextWindowSize({ H::Draw.Scale(300), 0 });
					if (FBeginPopupModal("ImportPlayerlist"))
					{
						FText("Import");
						FText("As", {}, FTextEnum::Right | FTextEnum::SameLine);

						for (int i = 0; i < vTags.size(); i++)
						{
							if (!vTags[i].m_bAssignable)
								continue;

							auto& iIDTo = mAs[i];

							ImVec2 vOriginalPos = GetCursorPos();
							PushStyleColor(ImGuiCol_Text, ColorByteToInt(vTags[i].m_tColor));
							SetCursorPos(vOriginalPos + ImVec2(H::Draw.Scale(8), H::Draw.Scale(5)));
							FText(vTags[i].m_sName.c_str());
							PopStyleColor();
							SetCursorPos(vOriginalPos - ImVec2(0, H::Draw.Scale(8))); DebugDummy({ GetWindowWidth() - GetStyle().WindowPadding.x * 2, H::Draw.Scale(32) });

							std::vector<const char*> vEntries = { "None" };
							std::vector<int> vValues = { 0 };
							for (int i = 0; i < F::PlayerUtils.m_vTags.size(); i++)
							{
								if (F::PlayerUtils.m_vTags[i].m_bAssignable)
								{
									vEntries.push_back(F::PlayerUtils.m_vTags[i].m_sName.c_str());
									vValues.push_back(i + 1);
								}
							}
							PushTransparent(iIDTo == -1);
							{
								int iTo = iIDTo + 1;
								FDropdown(std::format("##{}", i).c_str(), &iTo, vEntries, vValues, FSliderEnum::Right);
								iIDTo = iTo - 1;
							}
							PopTransparent();
						}

						if (FButton("Import", FButtonEnum::Left))
						{
							for (auto& [uAccountID, vTags] : mPlayerTags)
							{
								for (auto& iTag : vTags)
								{
									auto itAs = mAs.find(iTag);
									int iID = itAs != mAs.end() ? itAs->second : -1;
									if (iID != -1 && !F::PlayerUtils.HasTag(uAccountID, iID))
										F::PlayerUtils.AddTag(uAccountID, iID, false);
								}
							}
							for (auto& [uAccountID, sAlias] : mPlayerAliases)
							{
								if (!F::PlayerUtils.m_mPlayerAliases.contains(uAccountID))
									F::PlayerUtils.m_mPlayerAliases[uAccountID] = sAlias;
							}

							F::PlayerUtils.m_bSave = true;
							SDK::Output("Amalgam", "Imported playerlist", INFO_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG, ICON_MD_INFO);

							CloseCurrentPopup();
						}
						if (FButton("Cancel", FButtonEnum::Right | FButtonEnum::SameLine))
							CloseCurrentPopup();

						EndPopup();
					}
				}

				if (FButton("Backup", FButtonEnum::Fit | FButtonEnum::SameLine))
				{
					try
					{
						int iBackupCount = 0;
						for (auto& tEntry : std::filesystem::directory_iterator(F::Configs.m_sCorePath))
						{
							if (!tEntry.is_regular_file() || tEntry.path().extension() != F::Configs.m_sConfigExtension)
								continue;

							std::string sConfigName = tEntry.path().filename().string();
							sConfigName.erase(sConfigName.end() - F::Configs.m_sConfigExtension.size(), sConfigName.end());
							if (sConfigName.find("Backup") != std::string::npos)
								iBackupCount++;
						}
						std::filesystem::copy(
							F::Configs.m_sCorePath + "Players.json",
							F::Configs.m_sCorePath + std::format("Backup{}.json", iBackupCount + 1),
							std::filesystem::copy_options::overwrite_existing
						);
						SDK::Output("Amalgam", "Saved backup playerlist", INFO_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG, ICON_MD_INFO);
					}
					catch (...)
					{
						SDK::Output("Amalgam", "Failed to backup playerlist", ERROR_COLOR, OUTPUT_CONSOLE | OUTPUT_TOAST | OUTPUT_MENU | OUTPUT_DEBUG, ICON_MD_CANCEL);
					}
				}
			}
			PopDisabled();
		}
		break;
	}
	// Settings
	case 1:
	{
		if (BeginTable("ConfigSettingsTable", 2))
		{
			/* Column 1 */
			TableNextColumn();
			{
				if (Section("Logging"))
				{
					FDropdown(Vars::Logging::Logs);
					FDropdown(Vars::Logging::NotificationPosition);
					FSliderRow(Vars::Logging::NotificationTime);
					FSliderRow(Vars::Logging::MaxNotifications);
				} EndSection();
				if (Section("Cheat Detection"))
				{
					FDropdown(Vars::CheatDetection::Methods);
					PushTransparent(!Vars::CheatDetection::DetectionsRequired.Value);
					{
						FSliderRow(Vars::CheatDetection::DetectionsRequired);
					}
					PopTransparent();
					PushTransparent(!(Vars::CheatDetection::Methods.Value & Vars::CheatDetection::MethodsEnum::PacketChoking));
					{
						FSliderRow(Vars::CheatDetection::MinChoking);
					}
					PopTransparent();
					PushTransparent(!(Vars::CheatDetection::Methods.Value & Vars::CheatDetection::MethodsEnum::AimFlicking));
					{
						FSliderRow(Vars::CheatDetection::MinFlick, FSliderEnum::Left);
						FSliderRow(Vars::CheatDetection::MaxNoise, FSliderEnum::Right);
					}
					PopTransparent();
				} EndSection();
			}
			/* Column 2 */
			TableNextColumn();
			{
				if (Section("Log options"))
				{
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::VoteStart));
					{
						FDropdown(Vars::Logging::VoteStart::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::VoteCast));
					{
						FDropdown(Vars::Logging::VoteCast::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::ClassChanges));
					{
						FDropdown(Vars::Logging::ClassChange::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::Damage));
					{
						FDropdown(Vars::Logging::Damage::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::CheatDetection));
					{
						FDropdown(Vars::Logging::CheatDetection::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::Tags));
					{
						FDropdown(Vars::Logging::Tags::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::Aliases));
					{
						FDropdown(Vars::Logging::Aliases::LogTo);
					}
					PopTransparent();
					PushTransparent(!(Vars::Logging::Logs.Value & Vars::Logging::LogsEnum::Resolver));
					{
						FDropdown(Vars::Logging::Resolver::LogTo);
					}
					PopTransparent();
				} EndSection();
			}
			EndTable();
		}
		break;
	}
	// Output
	case 2:
	{
		if (Section("##Output", false, GetWindowHeight() - GetStyle().WindowPadding.y * 2))
		{
			bool bClear = false;

			for (auto& tOutput : m_vOutput)
			{
				ImVec2 vOriginalPos = GetCursorPos();
				size_t iLines = 1;

				float flWidth = GetWindowWidth() - GetStyle().WindowPadding.x * 2;
				if (!tOutput.m_sFunction.empty())
				{
					float flTitleWidth = 0.f;

					auto vWrapped = WrapText(tOutput.m_sFunction, flWidth);
					if (!vWrapped.empty())
					{
						PushStyleColor(ImGuiCol_Text, ColorByteToInt(tOutput.tAccent));

						for (auto& sText : vWrapped)
							FText(sText.c_str());
						iLines = vWrapped.size();
						flTitleWidth = FCalcTextSize(vWrapped.back().c_str()).x + H::Draw.Scale(4);

						PopStyleColor();
					}

					vWrapped = WrapText(tOutput.m_sLog, { int(flWidth - flTitleWidth), int(flWidth) });
					if (!vWrapped.empty())
					{
						SameLine(flTitleWidth + GetStyle().WindowPadding.x);
						for (auto& sText : vWrapped)
							FText(sText.c_str());
						iLines += vWrapped.size() - 1;
					}
				}
				else
				{
					PushStyleColor(ImGuiCol_Text, ColorByteToInt(tOutput.tAccent));

					auto vWrapped = WrapText(tOutput.m_sLog, flWidth);
					for (auto& sText : vWrapped)
						FText(sText.c_str());
					iLines = vWrapped.size();

					PopStyleColor();
				}

				SetCursorPos(vOriginalPos); DebugDummy({ flWidth, H::Draw.Scale(13) * iLines + GetStyle().WindowPadding.y });

				if (IsItemHovered() && IsMouseReleased(ImGuiMouseButton_Right))
					OpenPopup(std::format("Output{}", tOutput.m_iID).c_str());
				if (FBeginPopup(std::format("Output{}", tOutput.m_iID).c_str()))
				{
					PushStyleVar(ImGuiStyleVar_ItemSpacing, { H::Draw.Scale(8), H::Draw.Scale(8) });

					if (FSelectable("Copy"))
						SDK::SetClipboard(std::format("{}{}{}", tOutput.m_sFunction, tOutput.m_sFunction != "" ? " " : "", tOutput.m_sLog));
					if (FSelectable("Clear"))
						bClear = true;

					PopStyleVar();
					EndPopup();
				}
			}

			if (bClear)
				m_vOutput.clear();
		} EndSection();
		break;
	}
	}
}

void CMenu::MenuSettings(int iTab)
{
	using namespace ImGui;

	switch (iTab)
	{
	// Settings
	case 0:
	{
		if (BeginTable("ConfigSettingsTable", 2))
		{
			/*
			if (Section("Config"))
			{
				static int iCurrentType = 0;
				PushFont(F::Render.FontBold);
				FTabs({ "GENERAL", "VISUAL", }, &iCurrentType, { H::Draw.Scale(20), H::Draw.Scale(28) }, { GetWindowWidth(), 0 }, FTabsEnum::AlignReverse | FTabsEnum::Fit);
				SetCursorPosY(GetCursorPosY() - H::Draw.Scale());
				PopFont();

				switch (iCurrentType)
			*/

			auto fDrawConfigs = [](std::string& sStaticName, bool bVisual = false)
			{
				auto& sPath = !bVisual ? F::Configs.m_sConfigPath : F::Configs.m_sVisualsPath;
				auto& sConfig = !bVisual ? F::Configs.m_sCurrentConfig : F::Configs.m_sCurrentVisuals;
				auto sType = !bVisual ? "Config" : "Visual";
				bool bNoSave = GetAsyncKeyState(VK_SHIFT) & 0x8000;

				FSDropdown("Name", &sStaticName, {}, FSDropdownEnum::AutoUpdate, -H::Draw.Unscale(FCalcTextSize("CREATE").x + H::Draw.Scale(40)) - 44);
				PushDisabled(sStaticName.empty());
				{
					if (FButton("Create", FButtonEnum::Fit | FButtonEnum::SameLine, { 0, 40 }))
					{
						if (!std::filesystem::exists(sPath + sStaticName))
						{
							if (!bVisual)
								F::Configs.SaveConfig(sStaticName);
							else
								F::Configs.SaveVisual(sStaticName);
						}
						sStaticName.clear();
					}
				}
				PopDisabled();

				if (FButton(ICON_MD_FOLDER, FButtonEnum::Fit | FButtonEnum::SameLine, { 40, 40 }, 0, F::Render.IconFont))
					ShellExecuteA(NULL, NULL, sPath.c_str(), NULL, NULL, SW_SHOWNORMAL);

				pRowSizes->clear();

				std::vector<std::pair<std::filesystem::directory_entry, std::string>> vConfigs = {};
				bool bDefaultFound = false;
				for (auto& tEntry : std::filesystem::directory_iterator(sPath))
				{
					if (!tEntry.is_regular_file() || tEntry.path().extension() != F::Configs.m_sConfigExtension)
						continue;

					std::string sName = tEntry.path().filename().string();
					sName.erase(sName.end() - F::Configs.m_sConfigExtension.size(), sName.end());
					if (FNV1A::Hash32(sName.c_str()) == FNV1A::Hash32Const("default"))
						bDefaultFound = true;

					vConfigs.emplace_back(tEntry, sName);
				}
				if (!bVisual)
				{
					if (!bDefaultFound)
						F::Configs.SaveConfig("default");
					std::sort(vConfigs.begin(), vConfigs.end(), [&](const auto& a, const auto& b) -> bool
					{
						// override for default config
						if (FNV1A::Hash32(a.second.c_str()) == FNV1A::Hash32Const("default"))
							return true;
						if (FNV1A::Hash32(b.second.c_str()) == FNV1A::Hash32Const("default"))
							return false;

						return a.second < b.second;
					});
				}

				for (auto& [entry, sConfigName] : vConfigs)
				{
					bool bCurrentConfig = FNV1A::Hash32(sConfigName.c_str()) == FNV1A::Hash32(sConfig.c_str());
					ImVec2 vOriginalPos = GetCursorPos();

					SetCursorPos(vOriginalPos + ImVec2(H::Draw.Scale(2), H::Draw.Scale(9)));
					bool bLoad = IconButton(bCurrentConfig ? ICON_MD_REFRESH : ICON_MD_DOWNLOAD);
					FTooltip(ICON_MD_ADD ICON_MD_FILE_UPLOAD_OFF, bNoSave && IsItemHovered(), 300.f, F::Render.IconFont);

					SetCursorPos({ H::Draw.Scale(43), vOriginalPos.y + H::Draw.Scale(14) });
					TextColored(bCurrentConfig ? F::Render.Active.Value : F::Render.Inactive.Value, TruncateText(sConfigName, GetWindowWidth() - GetStyle().WindowPadding.x * 2 - H::Draw.Scale(80)).c_str());

					int iOffset = 9;
					SetCursorPos({ GetWindowWidth() - H::Draw.Scale(iOffset += 25), vOriginalPos.y + H::Draw.Scale(9) });
					bool bDelete = IconButton(ICON_MD_DELETE);

					SetCursorPos({ GetWindowWidth() - H::Draw.Scale(iOffset += 25), vOriginalPos.y + H::Draw.Scale(9) });
					bool bSave = IconButton(ICON_MD_SAVE);
					FTooltip(ICON_MD_ADD ICON_MD_FILE_DOWNLOAD_OFF, bNoSave && IsItemHovered(), 300.f, F::Render.IconFont);

					if (bLoad)
					{
						if (!bVisual)
							F::Configs.LoadConfig(sConfigName);
						else
							F::Configs.LoadVisual(sConfigName);
					}
					else if (bSave)
					{
						if (!bCurrentConfig || !bVisual && !F::Configs.m_sCurrentVisuals.empty())
							OpenPopup(std::format("Save{}{}", sType, sConfigName).c_str());
						else if (!bVisual)
							F::Configs.SaveConfig(sConfigName);
						else
							F::Configs.SaveVisual(sConfigName);
					}
					else if (bDelete)
						OpenPopup(std::format("Remove{}{}", sType, sConfigName).c_str());

					if (FBeginPopupModal(std::format("Save{}{}", sType, sConfigName).c_str()))
					{
						FText(std::format("Do you really want to override '{}'?", sConfigName).c_str());

						if (FButton("Yes, override", FButtonEnum::Left))
						{
							if (!bVisual)
								F::Configs.SaveConfig(sConfigName);
							else
								F::Configs.SaveVisual(sConfigName);
							CloseCurrentPopup();
						}
						if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
							CloseCurrentPopup();

						EndPopup();
					}
					else if (FBeginPopupModal(std::format("Remove{}{}", sType, sConfigName).c_str()))
					{
						FText(std::format("Do you really want to remove '{}'?", sConfigName).c_str());

						PushDisabled(!bVisual && FNV1A::Hash32(sConfigName.c_str()) == FNV1A::Hash32Const("default"));
						{
							if (FButton("Yes, delete", FButtonEnum::Fit))
							{
								if (!bVisual)
									F::Configs.DeleteConfig(sConfigName);
								else
									F::Configs.DeleteVisual(sConfigName);
								CloseCurrentPopup();
							}
						}
						PopDisabled();
						if (FButton("Yes, reset", FButtonEnum::Fit | FButtonEnum::SameLine))
						{
							if (!bVisual)
								F::Configs.ResetConfig(sConfigName);
							else
								F::Configs.ResetVisual(sConfigName);
							CloseCurrentPopup();
						}
						if (FButton("No", FButtonEnum::Fit | FButtonEnum::SameLine))
							CloseCurrentPopup();

						EndPopup();
					}

					SetCursorPos(vOriginalPos); DebugDummy({ 0, H::Draw.Scale(28) });
				}
				DebugDummy({ 0, H::Draw.Scale(7) });
			};

			/* Column 1 */
			TableNextColumn();
			if (Section("Config"))
			{
				static std::string sStaticName;

				fDrawConfigs(sStaticName);
			} EndSection();
			PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
			SetCursorPosX(GetCursorPosX() + GetStyle().WindowPadding.x);
			FText("Built @ " __DATE__ ", " __TIME__ ", " __CONFIGURATION__);
			//SetCursorPosX(GetCursorPosX() + GetStyle().WindowPadding.x);
			//FText(std::format("Time @ {}, {}", SDK::GetDate(), SDK::GetTime()).c_str());
			PopStyleColor();

			/* Column 2 */
			TableNextColumn();
			if (Section("Visuals"))
			{
				static std::string sStaticName;

				fDrawConfigs(sStaticName, true);
			} EndSection();

			EndTable();
		}
		break;
	}
	// Binds
	case 1:
	{
		if (Section("Settings", 8))
		{
			auto vTable = WidgetTable(3, H::Draw.Scale(16));

			if (BeginWidgetTable(0, vTable))
			{
				FToggleRow(Vars::Menu::BindWindow);
			} EndChild();

			if (BeginWidgetTable(1, vTable))
			{
				FToggleRow(Vars::Menu::BindWindowTitle);
			} EndChild();

			if (BeginWidgetTable(2, vTable))
			{
				FToggleRow(Vars::Menu::MenuShowsBinds);
			} EndChild();
		} EndSection();
		if (Section("Binds"))
		{
			static int iBind = DEFAULT_BIND;
			static Bind_t tBind = {};

			static int bParent = false;
			if (bParent)
				SetMouseCursor(ImGuiMouseCursor_Hand);

			auto vTable = WidgetTable(2, H::Draw.Scale(96));

			if (BeginWidgetTable(0, vTable))
			{
				FSDropdown("Name", &tBind.m_sName, {}, FDropdownEnum::Left | FSDropdownEnum::AutoUpdate);
				{
					auto sParent = bParent ? "..." : tBind.m_iParent != DEFAULT_BIND && tBind.m_iParent < F::Binds.m_vBinds.size() ? F::Binds.m_vBinds[tBind.m_iParent].m_sName : "None";
					if (FButton(std::format("Parent: {}", sParent).c_str(), FButtonEnum::Right | FButtonEnum::SameLine | FButtonEnum::NoUpper, { 0, 40 }))
						bParent = 2;
				}
				FDropdown("Type", &tBind.m_iType, { "Key", "Class", "Weapon type", "Item slot", "Misc" }, {}, FDropdownEnum::Left);
				switch (tBind.m_iType)
				{
				case BindEnum::Key: FDropdown("Behavior", &tBind.m_iInfo, { "Hold", "Toggle", "Double click" }, {}, FDropdownEnum::Right); break;
				case BindEnum::Class: FDropdown("Class", &tBind.m_iInfo, { "Scout", "Soldier", "Pyro", "Demoman", "Heavy", "Engineer", "Medic", "Sniper", "Spy" }, {}, FDropdownEnum::Right); break;
				case BindEnum::WeaponType: FDropdown("Weapon type", &tBind.m_iInfo, { "Hitscan", "Projectile", "Melee", "Throwable" }, {}, FDropdownEnum::Right); break;
				case BindEnum::ItemSlot: FDropdown("Item slot", &tBind.m_iInfo, { "1", "2", "3", "4", "5", "6", "7", "8", "9" }, {}, FDropdownEnum::Right); break;
				case BindEnum::Misc: FDropdown("Misc", &tBind.m_iInfo, { "Spectated", "Spectated 1st", "Spectated 3rd", "##Divider", "Zoomed", "Aiming" }, {}, FDropdownEnum::Right); break;
				}
			} EndChild();

			if (BeginWidgetTable(1, vTable))
			{
				int iNot = tBind.m_bNot;
				FDropdown("While", &iNot, { "Active", "Not active" }, {}, FDropdownEnum::Left);
				tBind.m_bNot = iNot;
				FDropdown("Visibility", &tBind.m_iVisibility, { "Always", "While active", "Hidden" }, {}, FDropdownEnum::Right);
				if (tBind.m_iType == 0)
					FKeybind("Key", tBind.m_iKey, FButtonEnum::None, { Vars::Menu::PrimaryKey[DEFAULT_BIND], Vars::Menu::SecondaryKey[DEFAULT_BIND] }, { 0, 40 }, -96);

				// create/modify button
				bool bCreate = false, bClear = false, bParent = true;
				if (tBind.m_iParent != DEFAULT_BIND)
					bParent = F::Binds.m_vBinds.size() > tBind.m_iParent;

				SetCursorPos({ GetWindowWidth() - H::Draw.Scale(96), H::Draw.Scale(48) });
				PushDisabled(!bParent || !(tBind.m_iType == BindEnum::Key ? tBind.m_iKey : true));
				{
					bool bMatch = iBind != DEFAULT_BIND && F::Binds.m_vBinds.size() > iBind;
					bCreate = FButton(bMatch ? ICON_MD_SETTINGS : ICON_MD_ADD, FButtonEnum::None, { 40, 40 }, 0, F::Render.IconFont);
				}
				PopDisabled();

				// clear button
				SetCursorPos({ GetWindowWidth() - H::Draw.Scale(48), H::Draw.Scale(48) });
				bClear = FButton(ICON_MD_CLEAR, FButtonEnum::None, { 40, 40 }, 0, F::Render.IconFont);

				if (bCreate)
					F::Binds.AddBind(iBind, tBind);
				if (bCreate || bClear)
				{
					iBind = DEFAULT_BIND;
					tBind = {};
				}
			} EndChild();

			PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
			SetCursorPos({ H::Draw.Scale(13), H::Draw.Scale(128) });
			FText("Binds");
			SetCursorPosY(GetCursorPosY() - H::Draw.Scale(5));
			PopStyleColor();

			auto fNumberToIndex = [](int iNumber, int iLayer)
			{
				int iIndex = -1, i = -1;

				std::unordered_mapset<int> mBinds = {};
				std::function<void(int)> fGetBinds = [&](int iParent)
				{
					for (int _iBind = 0; _iBind < F::Binds.m_vBinds.size(); _iBind++)
					{
						auto& _tBind = F::Binds.m_vBinds[_iBind];
						if (iParent != _tBind.m_iParent || mBinds.contains(_iBind))
							continue;

						mBinds[_iBind];

						i++;
						//if (iParent == iLayer && iNumber >= i)
						if (iIndex == -1 && iParent == iLayer && iNumber <= i)
							iIndex = _iBind;
						fGetBinds(_iBind);
					}
				};
				fGetBinds(DEFAULT_BIND);

				return iIndex;
			};
			auto fPositionToIndex = [&](ImVec2 vPos, int iLayer = DEFAULT_BIND)
			{
				int iIndex = floorf((vPos.y - GetCursorPosY() - H::Draw.Scale(4)) / H::Draw.Scale(36));
				iIndex = std::clamp(iIndex, 0, int(F::Binds.m_vBinds.size() - 1));
				iIndex = fNumberToIndex(iIndex, iLayer);
				return iIndex;
			};

			static int iDragging = -1, iLayer = DEFAULT_BIND;
			if (!IsMouseDown(ImGuiMouseButton_Left))
				iDragging = -1;
			else if (iDragging != -1)
			{
				int iTo = fPositionToIndex(GetMousePos() - GetDrawPos(), iLayer);
				if (iTo != -1 && iDragging != iTo)
				{
					F::Binds.Move(iDragging, iTo);
					if (iBind == iDragging)
						iBind = iTo;
					else if (iBind < iDragging && iBind >= iTo)
						iBind++;
					else if (iBind > iDragging && iBind <= iTo)
						iBind--;
					iDragging = iTo;
				}
			}

			std::unordered_mapset<int> mBinds = {};
			std::function<void(int, int)> fGetBinds = [&](int iParent, int x)
			{
				for (int _iBind = 0; _iBind < F::Binds.m_vBinds.size(); _iBind++)
				{
					auto& _tBind = F::Binds.m_vBinds[_iBind];
					if (iParent != DEFAULT_BIND - 1 && iParent != _tBind.m_iParent || mBinds.contains(_iBind))
						continue;

					mBinds[_iBind];

					std::string sType; std::string sInfo;
					switch (_tBind.m_iType)
					{
					case BindEnum::Key:
						switch (_tBind.m_iInfo)
						{
						case BindEnum::KeyEnum::Hold: { sType = "hold"; break; }
						case BindEnum::KeyEnum::Toggle: { sType = "toggle"; break; }
						case BindEnum::KeyEnum::DoubleClick: { sType = "double"; break; }
						}
						sInfo = U::KeyHandler.String(_tBind.m_iKey);
						break;
					case BindEnum::Class:
						sType = "class";
						switch (_tBind.m_iInfo)
						{
						case BindEnum::ClassEnum::Scout: { sInfo = "scout"; break; }
						case BindEnum::ClassEnum::Soldier: { sInfo = "soldier"; break; }
						case BindEnum::ClassEnum::Pyro: { sInfo = "pyro"; break; }
						case BindEnum::ClassEnum::Demoman: { sInfo = "demoman"; break; }
						case BindEnum::ClassEnum::Heavy: { sInfo = "heavy"; break; }
						case BindEnum::ClassEnum::Engineer: { sInfo = "engineer"; break; }
						case BindEnum::ClassEnum::Medic: { sInfo = "medic"; break; }
						case BindEnum::ClassEnum::Sniper: { sInfo = "sniper"; break; }
						case BindEnum::ClassEnum::Spy: { sInfo = "spy"; break; }
						}
						break;
					case BindEnum::WeaponType:
						sType = "weapon";
						switch (_tBind.m_iInfo)
						{
						case BindEnum::WeaponTypeEnum::Hitscan: { sInfo = "hitscan"; break; }
						case BindEnum::WeaponTypeEnum::Projectile: { sInfo = "projectile"; break; }
						case BindEnum::WeaponTypeEnum::Melee: { sInfo = "melee"; break; }
						case BindEnum::WeaponTypeEnum::Throwable: { sInfo = "throwable"; break; }
						}
						break;
					case BindEnum::ItemSlot:
						sType = "slot";
						sInfo = std::format("{}", _tBind.m_iInfo + 1);
						break;
					case BindEnum::Misc:
						switch (_tBind.m_iInfo)
						{
						case BindEnum::MiscEnum::Spectated:
						case BindEnum::MiscEnum::SpectatedFirst:
						case BindEnum::MiscEnum::SpectatedThird:
							sType = "spectated";
							switch (_tBind.m_iInfo)
							{
							case BindEnum::MiscEnum::Spectated: { sInfo = "any"; break; }
							case BindEnum::MiscEnum::SpectatedFirst: { sInfo = "1st"; break; }
							case BindEnum::MiscEnum::SpectatedThird: { sInfo = "3rd"; break; }
							}
							break;
						case BindEnum::MiscEnum::Zoomed:
						case BindEnum::MiscEnum::Aiming:
							sType = "cond";
							switch (_tBind.m_iInfo)
							{
							case BindEnum::MiscEnum::Zoomed: { sInfo = "zoomed"; break; }
							case BindEnum::MiscEnum::Aiming: { sInfo = "aiming"; break; }
							}
							break;
						}
						break;
					}
					if (_tBind.m_bNot && (_tBind.m_iType != BindEnum::Key || _tBind.m_iInfo == BindEnum::KeyEnum::Hold))
						sType = std::format("not {}", sType);

					ImVec2 vOriginalPos = { H::Draw.Scale(8) + H::Draw.Scale(28) * std::min(x, 3), GetCursorPosY() + H::Draw.Scale(8) };

					// background
					float flWidth = GetWindowWidth() - GetStyle().WindowPadding.x * 2 - H::Draw.Scale(28) * std::min(x, 3);
					float flHeight = H::Draw.Scale(28);
					ImVec2 vDrawPos = GetDrawPos() + vOriginalPos;
					if (iBind != _iBind)
						GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flWidth, flHeight), F::Render.Background1p5, H::Draw.Scale(4));
					else
					{
						ImColor tColor = F::Render.Background1p5L;
						GetWindowDrawList()->AddRectFilled(vDrawPos, vDrawPos + ImVec2(flWidth, flHeight), tColor, H::Draw.Scale(4));

						tColor = ColorByteToFloat((ColorFloatToByte(F::Render.Background1p5)).Lerp({ 127, 127, 127 }, 1.f / 9, LerpEnum::NoAlpha));
						float flInset = H::Draw.Scale(0.5f) - 0.5f;
						GetWindowDrawList()->AddRect(vDrawPos + ImVec2(flInset, flInset), vDrawPos + ImVec2(flWidth - flInset, flHeight - flInset), tColor, H::Draw.Scale(4), ImDrawFlags_None, H::Draw.Scale());
					}

					// text
					if (x > 3)
					{	// don't indent too much
						auto sText = std::format("-> {}", x);
						SetCursorPos(vOriginalPos + ImVec2(-FCalcTextSize(sText.c_str()).x - H::Draw.Scale(10), H::Draw.Scale(7)));
						PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
						FText(sText.c_str());
						PopStyleColor();
					}

					float flTextWidth = flWidth - H::Draw.Scale(127);
					PushTransparent(!F::Binds.WillBeEnabled(_iBind), true);

					SetCursorPos(vOriginalPos + ImVec2(H::Draw.Scale(9), H::Draw.Scale(7)));
					PushStyleColor(ImGuiCol_Text, _tBind.m_bActive ? F::Render.Accent.Value : F::Render.Active.Value);
					FText(TruncateText(_tBind.m_sName, flTextWidth * (1.f / 3) - H::Draw.Scale(20)).c_str());
					PopStyleColor();

					SetCursorPos(vOriginalPos + ImVec2(flTextWidth * (1.f / 3), H::Draw.Scale(7)));
					FText(sType.c_str());

					SetCursorPos(vOriginalPos + ImVec2(flTextWidth * (2.f / 3), H::Draw.Scale(7)));
					FText(sInfo.c_str());

					// buttons
					int iOffset = 1;

					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(iOffset += 25), H::Draw.Scale(2)));
					bool bDelete = IconButton(ICON_MD_DELETE);

					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(iOffset += 25), H::Draw.Scale(2)));
					if (IconButton(ICON_MD_EDIT))
						CurrentBind = CurrentBind != _iBind ? _iBind : DEFAULT_BIND;

					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(iOffset += 25), H::Draw.Scale(2)));
					if (IconButton(!_tBind.m_bNot ? ICON_MD_CODE : ICON_MD_CODE_OFF))
						_tBind.m_bNot = !_tBind.m_bNot;

					PushTransparent(Transparent || _tBind.m_iVisibility == BindVisibilityEnum::Hidden, true);
					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(iOffset += 25), H::Draw.Scale(2)));
					if (IconButton(_tBind.m_iVisibility == BindVisibilityEnum::Always ? ICON_MD_VISIBILITY : ICON_MD_VISIBILITY_OFF))
						_tBind.m_iVisibility = (_tBind.m_iVisibility + 1) % 3;
					PopTransparent(1, 1);

					SetCursorPos(vOriginalPos + ImVec2(flWidth - H::Draw.Scale(iOffset += 25), H::Draw.Scale(2)));
					if (IconButton(_tBind.m_bEnabled ? ICON_MD_TOGGLE_ON : ICON_MD_TOGGLE_OFF))
						_tBind.m_bEnabled = !_tBind.m_bEnabled;

					SetCursorPos(vOriginalPos);
					bool bClicked = Button(std::format("##{}", _iBind).c_str(), { flWidth, flHeight });
					bool bPopup = IsItemClicked(ImGuiMouseButton_Right);

					PopTransparent(1, 1);

					if (bClicked)
					{
						if (!bParent)
						{
							iBind = _iBind;
							tBind = _tBind;
						}
						else
						{
							bParent = false;
							tBind.m_iParent = _iBind;

							// make sure bind can't be parented to itself or any of its children
							int _iBind2 = _iBind;
							Bind_t _tBind2;
							while (F::Binds.GetBind(_iBind2, &_tBind2))
							{
								if (_iBind2 == iBind)
									tBind.m_iParent = DEFAULT_BIND;
								_iBind2 = _tBind2.m_iParent;
							}
						}
					}
					else if (bPopup)
						OpenPopup(std::format("RightClicked{}", _iBind).c_str());
					else if (iDragging == -1 && IsItemHovered() && IsMouseDown(ImGuiMouseButton_Left))
						iDragging = _iBind, iLayer = iParent;
					else if (bDelete)
					{
						if (_tBind.m_vVars.size() <= 1 && !F::Binds.HasChildren(_iBind) || U::KeyHandler.Down(VK_SHIFT)) // allow user to quickly remove binds
							F::Binds.RemoveBind(_iBind);
						else
							OpenPopup(std::format("DeleteBind{}", _iBind).c_str());
					}

					if (FBeginPopup(std::format("RightClicked{}", _iBind).c_str()))
					{
						PushStyleVar(ImGuiStyleVar_ItemSpacing, { H::Draw.Scale(8), 0 });

						{
							static std::string sInput = "";

							bool bEnter = FInputText("Name...", sInput, H::Draw.Scale(284), ImGuiInputTextFlags_EnterReturnsTrue);
							if (!IsItemFocused())
								sInput = _tBind.m_sName;
							if (bEnter)
								_tBind.m_sName = sInput;
						}

						FDropdown("Type", &_tBind.m_iType, { "Key", "Class", "Weapon type", "Item slot", "Misc" }, {}, FDropdownEnum::Left);
						switch (_tBind.m_iType)
						{
						case BindEnum::Key: FDropdown("Behavior", &_tBind.m_iInfo, { "Hold", "Toggle", "Double click" }, {}, FDropdownEnum::Right); break;
						case BindEnum::Class: FDropdown("Class", &_tBind.m_iInfo, { "Scout", "Soldier", "Pyro", "Demoman", "Heavy", "Engineer", "Medic", "Sniper", "Spy" }, {}, FDropdownEnum::Right); break;
						case BindEnum::WeaponType: FDropdown("Weapon type", &_tBind.m_iInfo, { "Hitscan", "Projectile", "Melee", "Throwable" }, {}, FDropdownEnum::Right); break;
						case BindEnum::ItemSlot: FDropdown("Item slot", &_tBind.m_iInfo, { "1", "2", "3", "4", "5", "6", "7", "8", "9" }, {}, FDropdownEnum::Right); break;
						case BindEnum::Misc: FDropdown("Misc", &_tBind.m_iInfo, { "Spectated", "Spectated 1st", "Spectated 3rd", "##Divider", "Zoomed", "Aiming" }, {}, FDropdownEnum::Right); break;
						}
						if (_tBind.m_iType == BindEnum::Key)
							FKeybind("Key", _tBind.m_iKey);

						PopStyleVar();
						EndPopup();
					}
					else if (FBeginPopupModal(std::format("DeleteBind{}", _iBind).c_str()))
					{
						FText(std::format("Do you really want to delete '{}'{}?", _tBind.m_sName, F::Binds.HasChildren(_iBind) ? " and all of its children" : "").c_str());

						if (FButton("Yes", FButtonEnum::Left))
						{
							F::Binds.RemoveBind(_iBind);
							CloseCurrentPopup();

							iBind = DEFAULT_BIND;
							tBind = {};
						}
						if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
							CloseCurrentPopup();

						EndPopup();
					}

					if (iParent != DEFAULT_BIND - 1)
						fGetBinds(_iBind, x + 1);
				}
			};
			fGetBinds(DEFAULT_BIND, 0);

			// this should ideally never happen, but failsafe
			if (F::Binds.m_vBinds.size() > mBinds.size())
			{
				PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
				SetCursorPos({ H::Draw.Scale(13), GetCursorPosY() + H::Draw.Scale(5) });
				FText("Dangling");
				SetCursorPosY(GetCursorPosY() - H::Draw.Scale(5));
				PopStyleColor();

				fGetBinds(DEFAULT_BIND - 1, 0);
			}

			if (bParent == 2) // dumb
				bParent = 1;
			else if (bParent && IsMouseReleased(ImGuiMouseButton_Left))
			{
				bParent = false;
				tBind.m_iParent = DEFAULT_BIND;
			}
		} EndSection();
		break;
	}
	// Materials
	case 2:
	{
		static TextEditor tTextEditor;
		static std::string sCurrentMaterial;
		static bool bLockedMaterial;

		bool bTable = false;
		if (!sCurrentMaterial.empty())
			bTable = BeginTable("MaterialsTable", 2);
		{
			if (bTable)
			{
				TableSetupColumn("MaterialsTable1", ImGuiTableColumnFlags_WidthFixed, H::Draw.Scale(288));
				TableSetupColumn("MaterialsTable2", ImGuiTableColumnFlags_WidthFixed, GetWindowWidth());

				/* Column 1 */
				TableNextColumn();
			}

			if (Section("Manager"))
			{
				static std::string sStaticName;

				FSDropdown("Name", &sStaticName, {}, FSDropdownEnum::AutoUpdate, -H::Draw.Unscale(FCalcTextSize("CREATE").x + H::Draw.Scale(40)) - 44);
				PushDisabled(sStaticName.empty());
				{
					if (FButton("Create", FButtonEnum::Fit | FButtonEnum::SameLine, { 0, 40 }))
					{
						F::Materials.AddMaterial(sStaticName.c_str());
						sStaticName.clear();
					}
				}
				PopDisabled();

				if (FButton(ICON_MD_FOLDER, FButtonEnum::Fit | FButtonEnum::SameLine, { 40, 40 }, 0, F::Render.IconFont))
					ShellExecuteA(NULL, NULL, F::Configs.m_sMaterialsPath.c_str(), NULL, NULL, SW_SHOWNORMAL);

				std::vector<Material_t> vMaterials;
				for (auto& tMaterial : F::Materials.m_mMaterials | std::views::values)
					vMaterials.push_back(tMaterial);

				std::sort(vMaterials.begin(), vMaterials.end(), [&](const auto& a, const auto& b) -> bool
				{
					// override for none material
					if (FNV1A::Hash32(a.m_sName.c_str()) == FNV1A::Hash32Const("None"))
						return true;
					if (FNV1A::Hash32(b.m_sName.c_str()) == FNV1A::Hash32Const("None"))
						return false;

					// keep locked materials higher
					if (a.m_bLocked && !b.m_bLocked)
						return true;
					if (!a.m_bLocked && b.m_bLocked)
						return false;

					return a.m_sName < b.m_sName;
				});

				for (auto& tMaterial : vMaterials)
				{
					ImVec2 vOriginalPos = GetCursorPos();

					SetCursorPos({ H::Draw.Scale(17), vOriginalPos.y + H::Draw.Scale(14) });
					TextColored(tMaterial.m_bLocked ? F::Render.Inactive.Value : F::Render.Active.Value, TruncateText(tMaterial.m_sName, GetWindowWidth() - GetStyle().WindowPadding.x * 2 - H::Draw.Scale(56)).c_str());

					int iOffset = 9;
					if (!tMaterial.m_bLocked)
					{
						SetCursorPos({ GetWindowWidth() - H::Draw.Scale(iOffset += 25), vOriginalPos.y + H::Draw.Scale(9) });
						if (IconButton(ICON_MD_DELETE))
							OpenPopup(std::format("DeleteMat{}", tMaterial.m_sName).c_str());
						if (FBeginPopupModal(std::format("DeleteMat{}", tMaterial.m_sName).c_str()))
						{
							FText(std::format("Do you really want to delete '{}'?", tMaterial.m_sName).c_str());

							if (FButton("Yes", FButtonEnum::Left))
							{
								F::Materials.RemoveMaterial(tMaterial.m_sName.c_str());
								CloseCurrentPopup();
							}
							if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
								CloseCurrentPopup();

							EndPopup();
						}
					}

					SetCursorPos({ GetWindowWidth() - H::Draw.Scale(iOffset += 25), vOriginalPos.y + H::Draw.Scale(9) });
					if (IconButton(ICON_MD_EDIT))
					{
						sCurrentMaterial = tMaterial.m_sName;
						bLockedMaterial = tMaterial.m_bLocked;

						tTextEditor.SetText(F::Materials.GetVMT(FNV1A::Hash32(sCurrentMaterial.c_str())));
						tTextEditor.SetReadOnlyEnabled(bLockedMaterial);
					}

					SetCursorPos(vOriginalPos); DebugDummy({ 0, H::Draw.Scale(28) });
				}
				DebugDummy({ 0, H::Draw.Scale(7) });
			}
			else
				SetScrollY(0);
			EndSection();

			if (bTable)
			{
				/* Column 2 */
				TableNextColumn();
				if (sCurrentMaterial.length())
				{
					SetCursorPosY(GetScrollY() + GetStyle().WindowPadding.y);
					if (Section("Editor", 0, GetWindowHeight() - GetStyle().WindowPadding.y * 2, true))
					{
						// Toolbar
						if (!bLockedMaterial)
						{
							if (FButton("Save", FButtonEnum::Fit))
							{
								auto sText = tTextEditor.GetText();
								F::Materials.EditMaterial(sCurrentMaterial.c_str(), sText.c_str());
							}
							SameLine();
						}
						if (FButton("Close", FButtonEnum::Fit))
							sCurrentMaterial = "";
						SetCursorPosY(H::Draw.Scale(52));
						PushStyleColor(ImGuiCol_Text, F::Render.Inactive.Value);
						FText(std::format("{}: {}", bLockedMaterial ? "Viewing" : "Editing", sCurrentMaterial).c_str(), {}, FTextEnum::Right);
						PopStyleColor();

						// Text editor
						DebugDummy({ 0, H::Draw.Scale(8) });

						tTextEditor.SetLanguage(TextEditor::Language::Cpp());
						TextEditor::Palette tPalette = {{
							F::Render.Active,				// text
							F::Render.Active,				// keyword
							F::Render.Active,				// declaration
							F::Render.Active,				// number
							F::Render.Accent,				// string
							F::Render.Inactive,				// punctuation
							F::Render.Inactive,				// preprocessor
							F::Render.Inactive,				// identifier
							F::Render.Inactive,				// known identifier
							F::Render.Inactive,				// comment
							F::Render.Background1,			// background
							F::Render.Active,				// cursor
							ImColor(F::Render.Inactive.Value.x, F::Render.Inactive.Value.y, F::Render.Inactive.Value.z, 0.5f),	// selection
							ImColor(F::Render.Inactive.Value.x, F::Render.Inactive.Value.y, F::Render.Inactive.Value.z, 0.1f),	// whitespace
							IM_COL32( 70,  70,  70, 255),	// matchingBracketBackground
							IM_COL32(140, 140, 140, 255),	// matchingBracketActive
							IM_COL32(246, 222,  36, 255),	// matchingBracketLevel1
							IM_COL32( 66, 120, 198, 255),	// matchingBracketLevel2
							IM_COL32(213,  96, 213, 255),	// matchingBracketLevel3
							IM_COL32(198,   8,  32, 255),	// matchingBracketError
							F::Render.Inactive,				// line number
							F::Render.Active,				// current line number
						}};
						tTextEditor.SetPalette(tPalette);
						//TextEditor.SetShowLineNumbersEnabled(false);

						PushFont(F::Render.FontMono);
						PushStyleVar(ImGuiStyleVar_ChildBorderSize, H::Draw.Scale(1));
						ImVec2 vDrawPos = GetDrawPos() + GetCursorPos();
						tTextEditor.Render("TextEditor");
						ImVec2 vSize = GetItemRectSize();
						float flInset = H::Draw.Scale(0.5f) - 0.5f;
						GetWindowDrawList()->AddRect(vDrawPos + ImVec2(flInset, flInset), vDrawPos + ImVec2(vSize.x - flInset, vSize.y - flInset), F::Render.Background2, H::Draw.Scale(4), ImDrawFlags_None, H::Draw.Scale());
						PopStyleVar();
						PopFont();
					} EndSection();
				}

				EndTable();
			}
		}
		break;
	}
	// Extra
	case 3:
	{
		if (Section("Functions"))
		{
			if (FButton("Fullupdate", FButtonEnum::Left))
				I::EngineClient->ClientCmd_Unrestricted("cl_fullupdate");
			if (FButton("Retry", FButtonEnum::Right | FButtonEnum::SameLine))
				I::EngineClient->ClientCmd_Unrestricted("retry");
			if (FButton("Console", FButtonEnum::Left))
				I::EngineClient->ClientCmd_Unrestricted("toggleconsole");
			if (FButton("Reload materials", FButtonEnum::Right | FButtonEnum::SameLine) && F::Materials.m_bLoaded)
				F::Materials.ReloadMaterials();

			if (!I::EngineClient->IsConnected())
			{
				if (FButton("Unlock achievements", FButtonEnum::Left))
					OpenPopup("UnlockAchievements");
				if (FButton("Lock achievements", FButtonEnum::Right | FButtonEnum::SameLine))
					OpenPopup("LockAchievements");

				if (FBeginPopupModal("UnlockAchievements"))
				{
					FText("Do you really want to unlock all achievements?");

					if (FButton("Yes, unlock", FButtonEnum::Left))
					{
						F::Misc.UnlockAchievements();
						CloseCurrentPopup();
					}
					if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
						CloseCurrentPopup();

					EndPopup();
				}
				else if (FBeginPopupModal("LockAchievements"))
				{
					FText("Do you really want to lock all achievements?");

					if (FButton("Yes, lock", FButtonEnum::Left))
					{
						F::Misc.LockAchievements();
						CloseCurrentPopup();
					}
					if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
						CloseCurrentPopup();

					EndPopup();
				}
			}
		} EndSection();
		if (Section("Debug", 8))
		{
			FToggleRow(Vars::Debug::Info, FToggleEnum::Left);
			FToggleRow(Vars::Debug::Logging, FToggleEnum::Right);
			FToggleRow(Vars::Debug::Options, FToggleEnum::Left);
			FToggleRow(Vars::Debug::CrashLogging, FToggleEnum::Right);

#ifdef DEBUG_TRACES
			FToggleRow(Vars::Debug::VisualizeTraces, FToggleEnum::Left);
			FToggleRow(Vars::Debug::VisualizeTraceHits, FToggleEnum::Right);
#endif

			FToggleRow(Vars::Debug::AutoVprof);
			if (Vars::Debug::AutoVprof.Value)
				FText(std::format("Reports -> Amalgam/vprof/  |  build {}", F::AutoVprof.GetBuildID()).c_str());

			// Amalgam's own profiler. Auto vprof turns it on regardless while it
			// captures, so the toggle only matters for the live overlay.
			FToggleRow(Vars::Debug::ProcessTracker, FToggleEnum::Left);
			FToggleRow(Vars::Debug::ProcessTrackerOverlay, FToggleEnum::Right);
			if (Vars::Debug::ProcessTracker.Value || Vars::Debug::ProcessTrackerOverlay.Value)
				FText(std::format("Amalgam {:.2f}ms/frame  |  tracker overhead {:.3f}ms",
					Perf::Tracker.OverlayTotalMs(), Perf::Tracker.OverheadMs()).c_str());
		} EndSection();
		if (Vars::Debug::Options.Value && I::EngineClient->IsConnected())
		{
			if (Section("##Debug"))
			{
				FToggleRow(Vars::Debug::DrawHitboxes);

				if (FButton("Restore visuals", FButtonEnum::Left))
				{
					for (auto& tLine : G::LineStorage)
						tLine.m_flTime = I::GlobalVars->curtime + 60.f;
					for (auto& tPath : G::PathStorage)
						tPath.m_flTime = I::GlobalVars->curtime + 60.f;
					for (auto& tBox : G::BoxStorage)
						tBox.m_flTime = I::GlobalVars->curtime + 60.f;
					for (auto& tSphere : G::SphereStorage)
						tSphere.m_flTime = I::GlobalVars->curtime + 60.f;
					for (auto& tSwept : G::SweptStorage)
						tSwept.m_flTime = I::GlobalVars->curtime + 60.f;
					for (auto& tTriangle : G::TriangleStorage)
						tTriangle.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear visuals", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::LineStorage.clear();
					G::PathStorage.clear();
					G::BoxStorage.clear();
					G::SphereStorage.clear();
					G::SweptStorage.clear();
					G::TriangleStorage.clear();
				}

				if (FButton("Restore lines", FButtonEnum::Left))
				{
					for (auto& tLine : G::LineStorage)
						tLine.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear lines", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::LineStorage.clear();
				}

				if (FButton("Restore paths", FButtonEnum::Left))
				{
					for (auto& tPath : G::PathStorage)
						tPath.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear paths", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::PathStorage.clear();
				}

				if (FButton("Restore boxes", FButtonEnum::Left))
				{
					for (auto& tBox : G::BoxStorage)
						tBox.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear boxes", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::BoxStorage.clear();
				}

				if (FButton("Restore spheres", FButtonEnum::Left))
				{
					for (auto& tSphere : G::SphereStorage)
						tSphere.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear spheres", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::SphereStorage.clear();
				}

				if (FButton("Restore swept", FButtonEnum::Left))
				{
					for (auto& tSwept : G::SweptStorage)
						tSwept.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear swept", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::SweptStorage.clear();
				}

				if (FButton("Restore triangles", FButtonEnum::Left))
				{
					for (auto& tTriangle : G::TriangleStorage)
						tTriangle.m_flTime = I::GlobalVars->curtime + 60.f;
				}
				if (FButton("Clear triangles", FButtonEnum::Right | FButtonEnum::SameLine))
				{
					G::TriangleStorage.clear();
				}
			} EndSection();
		}
		/*
		if (Vars::Debug::Options.Value)
		{
			if (Section("Convar spoofer"))
			{
				static std::string sName = "", sValue = "";

				FSDropdown("Convar", &sName, {}, FDropdownEnum::Left);
				FSDropdown("Value", &sValue, {}, FDropdownEnum::Right);
				if (FButton("Send"))
				{
					if (auto pNetChan = static_cast<CNetChannel*>(I::EngineClient->GetNetChannelInfo()))
					{
						SDK::Output("Convar", std::format("Sent {} as {}", sName, sValue).c_str(), Vars::Menu::Theme::Accent.Value);
						NET_SetConVar cmd(sName.c_str(), sValue.c_str());
						pNetChan->SendNetMsg(cmd);

						sName = sValue = "";
					}
				}
			} EndSection();
		}
		*/
#ifdef WORLD_DEBUG
		if (Section("World"))
		{
			FDropdown(Vars::World::Faces, FDropdownEnum::Left);
			FDropdown(Vars::World::Draw, FDropdownEnum::Right);
			FSliderRow(Vars::World::Offset, FSliderEnum::Left);
			FSliderRow(Vars::World::Resize, FSliderEnum::Right);
			FColorPicker(Vars::World::BoxBrush, FColorPickerEnum::Left);
			FColorPicker(Vars::World::PlaneBrush, FColorPickerEnum::Right);
			FColorPicker(Vars::World::Displacement, FColorPickerEnum::Left);
			FColorPicker(Vars::World::Prop, FColorPickerEnum::Right);
			FColorPicker(Vars::World::Entity, FColorPickerEnum::Left);
		} EndSection();
#endif
#ifdef DEBUG_HOOKS
		if (Section("Hooks"))
		{
			auto SetAll = [](bool bEnable)
			{
				for (auto& pBase : G::Vars)
				{
					if (std::string(pBase->Name()).find("Vars::Hooks::") == std::string::npos)
						continue;

					switch (FNV1A::Hash32(pBase->Name()))
					{
					case FNV1A::Hash32Const("Vars::Hooks::Direct3DDevice9_Present"):
					case FNV1A::Hash32Const("Vars::Hooks::Direct3DDevice9_Reset"):
						break;
					default:
						pBase->As<bool>()->Map[DEFAULT_BIND] = bEnable;
					}
				}
			};
			if (FButton("Enable all", FButtonEnum::Left))
				SetAll(true);
			if (FButton("Disable all", FButtonEnum::Right | FButtonEnum::SameLine))
				SetAll(false);

			int i = 0; for (auto& pBase : G::Vars)
			{
				if (std::string(pBase->Name()).find("Vars::Hooks::") == std::string::npos)
					continue;

				FToggle(*pBase->As<bool>(), !(i % 2) ? FToggleEnum::Left : FToggleEnum::Right);
				i++;
			}
		} EndSection();
#endif
		break;
	}
	}
}

void CMenu::MenuSearch(std::string sSearch)
{
	using namespace ImGui;

	if (sSearch.empty())
		return;

	static std::vector<BaseVar*> vVars = {}; // don't string search every single frame

	static uint32_t uStaticHash = 0;
	if (const uint32_t uCurrHash = FNV1A::Hash32(sSearch.c_str());
		uCurrHash != uStaticHash)
	{
		std::string sSearch2 = sSearch;
		std::transform(sSearch2.begin(), sSearch2.end(), sSearch2.begin(), ::tolower);

		vVars.clear();
		for (auto& pBase : G::Vars)
		{
			if (!Vars::Debug::Options[DEFAULT_BIND] && pBase->m_iFlags & DEBUGVAR)
				continue;

			std::vector<const char*> vSearch = { pBase->Name(), pBase->Section() };
			vSearch.insert(vSearch.end(), pBase->m_vNames.begin(), pBase->m_vNames.end());
			vSearch.insert(vSearch.end(), pBase->m_vValues.begin(), pBase->m_vValues.end());
			for (auto pSearch : vSearch)
			{
				std::string sSearch3 = pSearch;
				if (auto iFind = sSearch3.find("Vars::"); iFind != std::string::npos)
					sSearch3 = sSearch3.replace(iFind, strlen("Vars::"), "");
				if (auto iFind = sSearch3.find("##"); iFind != std::string::npos)
					sSearch3 = sSearch3.replace(iFind, strlen("##"), "");
				std::transform(sSearch3.begin(), sSearch3.end(), sSearch3.begin(), ::tolower);
				if (sSearch3.find(sSearch2) != std::string::npos)
				{
					vVars.push_back(pBase);
					break;
				}
			}
		}

		uStaticHash = uCurrHash;
	}

	if (vVars.empty())
		return;

	uint32_t uLastSection = 0;
	int i = 0; for (auto pBase : vVars) // possibly implement tablelike visuals, do away with left right, just switch if current side is higher than other?
	{
		int iWidgetEnum = WidgetEnum::Invalid, iTypeEnum = WidgetEnum::Invalid;
		if (auto pVar = pBase->As<bool>())
			iWidgetEnum = iTypeEnum = WidgetEnum::FToggle;
		else if (auto pVar = pBase->As<int>())
		{
			if (!pVar->m_vValues.empty()
				|| FNV1A::Hash32(pVar->Name()) == FNV1A::Hash32Const("Vars::ESP::ActiveGroups"))
				iWidgetEnum = iTypeEnum = WidgetEnum::FDropdown;
			else if (pVar->m_sExtra)
				iWidgetEnum = WidgetEnum::FISlider, iTypeEnum = WidgetEnum::FSlider;
			else
				iWidgetEnum = iTypeEnum = WidgetEnum::FKeybind;
		}
		else if (auto pVar = pBase->As<float>())
			iWidgetEnum = WidgetEnum::FFSlider, iTypeEnum = WidgetEnum::FSlider;
		else if (auto pVar = pBase->As<IntRange_t>())
			iWidgetEnum = WidgetEnum::FIRSlider, iTypeEnum = WidgetEnum::FSlider;
		else if (auto pVar = pBase->As<FloatRange_t>())
			iWidgetEnum = WidgetEnum::FFRSlider, iTypeEnum = WidgetEnum::FSlider;
		else if (auto pVar = pBase->As<std::string>())
			iWidgetEnum = WidgetEnum::FSDropdown, iTypeEnum = WidgetEnum::FDropdown;
		else if (auto pVar = pBase->As<std::vector<std::pair<std::string, MaterialColor_t>>>())
			iWidgetEnum = WidgetEnum::FMDropdown, iTypeEnum = WidgetEnum::FDropdown;
		else if (auto pVar = pBase->As<Color_t>())
			iWidgetEnum = WidgetEnum::FColorPicker, iTypeEnum = WidgetEnum::FToggle;
		else if (auto pVar = pBase->As<Gradient_t>())
			iWidgetEnum = WidgetEnum::FGColorPicker, iTypeEnum = WidgetEnum::FToggle;
		else
			continue;

		uint32_t uSection = FNV1A::Hash32(pBase->Section());
		if (uSection != uLastSection)
		{
			if (uLastSection)
				EndSection();
			Section(std::format("{}## {}", pBase->Section(), pBase->Name()).c_str());
			i = 0;
		}
		uLastSection = uSection;

		static int iLastEnum = WidgetEnum::Invalid;
		if (!i || iTypeEnum != iLastEnum)
		{
			if (!i)
			{
				switch (iWidgetEnum)
				{
				case WidgetEnum::FToggle:
				case WidgetEnum::FISlider:
				case WidgetEnum::FFSlider:
				case WidgetEnum::FIRSlider:
				case WidgetEnum::FFRSlider:
				case WidgetEnum::FColorPicker:
				case WidgetEnum::FGColorPicker:
					DebugDummy({ 0, H::Draw.Scale(8) });
				}
				i = 2;
			}
			else if (iTypeEnum == WidgetEnum::FToggle && iLastEnum == WidgetEnum::FSlider && (i % 2))
			{
				SetCursorPos({ GetWindowWidth() / 2 + GetStyle().WindowPadding.x / 2, GetRowPos() + H::Draw.Scale(8) });
				i = 0;
			}
			else if (iTypeEnum == WidgetEnum::FSlider && iLastEnum == WidgetEnum::FDropdown && (i % 2) && !pRowSizes->empty())
			{
				auto& tRow = pRowSizes->front();
				tRow.m_vPos.y += H::Draw.Scale(13), tRow.m_vSize.y -= H::Draw.Scale(13);
				SetCursorPos({ GetWindowWidth() / 2 + GetStyle().WindowPadding.x / 2, GetRowPos() });
				i = 0;
			}
			else
				i = 2;
		}
		iLastEnum = iTypeEnum;

		int iOverride = -1;
		switch (iWidgetEnum)
		{
		case WidgetEnum::FToggle:
		{
			auto pVar = pBase->As<bool>();
			if (FToggle(*pVar, !(i % 2) ? FToggleEnum::Left : FToggleEnum::Right, nullptr, iOverride/*, iOverride*/))
			{
				if (FNV1A::Hash32(pVar->Name()) == FNV1A::Hash32Const("Vars::Debug::Options"))
					uStaticHash = 0;
			}
			break;
		}
		case WidgetEnum::FISlider:
		{
			auto pVar = pBase->As<int>();
			const char* sFormat = nullptr;
			switch (FNV1A::Hash32(pVar->Name()))
			{
			case FNV1A::Hash32Const("Vars::Aimbot::Projectile::SplashAirCount"):
				if (!pVar->Map[DEFAULT_BIND])
					sFormat = "random";
			}
			FSlider(*pVar, !(i % 2) ? FSliderEnum::Left : FSliderEnum::Right, sFormat, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FFSlider:
		{
			auto pVar = pBase->As<float>();
			const char* sFormat = nullptr;
			switch (FNV1A::Hash32(pVar->Name()))
			{
			case FNV1A::Hash32Const("Vars::Visuals::Prediction::PlayerDrawDuration"):
			case FNV1A::Hash32Const("Vars::Visuals::Prediction::ProjectileDrawDuration"):
				if (!pVar->Map[DEFAULT_BIND])
					sFormat = "timed";
				break;
			case FNV1A::Hash32Const("Vars::Aimbot::Projectile::SplashRotateX"):
			case FNV1A::Hash32Const("Vars::Aimbot::Projectile::SplashRotateY"):
				if (pVar->Map[DEFAULT_BIND] < 0.f)
					sFormat = "random";
			}
			FSlider(*pVar, !(i % 2) ? FSliderEnum::Left : FSliderEnum::Right, sFormat, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FIRSlider:
		{
			auto pVar = pBase->As<IntRange_t>();
			FSlider(*pVar, !(i % 2) ? FSliderEnum::Left : FSliderEnum::Right, nullptr, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FFRSlider:
		{
			auto pVar = pBase->As<FloatRange_t>();
			FSlider(*pVar, !(i % 2) ? FSliderEnum::Left : FSliderEnum::Right, nullptr, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FDropdown:
		{
			auto pVar = pBase->As<int>();
			FDropdown(*pVar, !(i % 2) ? FDropdownEnum::Left : FDropdownEnum::Right, 0, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FSDropdown:
		{
			auto pVar = pBase->As<std::string>();
			FSDropdown(*pVar, !(i % 2) ? FDropdownEnum::Left : FDropdownEnum::Right, 0, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FMDropdown:
		{
			auto pVar = pBase->As<std::vector<std::pair<std::string, MaterialColor_t>>>();
			FMDropdown(*pVar, !(i % 2) ? FDropdownEnum::Left : FDropdownEnum::Right, 0, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FColorPicker:
		{
			auto pVar = pBase->As<Color_t>();
			FColorPicker(*pVar, !(i % 2) ? FColorPickerEnum::Left : FColorPickerEnum::Right, {}, { H::Draw.Scale(12), H::Draw.Scale(12) }, {}, nullptr, iOverride, iOverride);
			break;
		}
		case WidgetEnum::FGColorPicker:
		{
			auto pVar = pBase->As<Gradient_t>();
			FColorPicker(*pVar, true, !(i % 2) ? FColorPickerEnum::Left : FColorPickerEnum::Right, {}, { H::Draw.Scale(12), H::Draw.Scale(12) }, {}, nullptr, iOverride/*, iOverride*/);
			FColorPicker(*pVar, false, !(++i % 2) ? FColorPickerEnum::Left : FColorPickerEnum::Right, {}, { H::Draw.Scale(12), H::Draw.Scale(12) }, {}, nullptr, iOverride/*, iOverride*/);
			break;
		}
		case WidgetEnum::FKeybind:
		{
			auto pVar = pBase->As<int>();
			std::vector<int> vIgnore;
			switch (FNV1A::Hash32(pVar->Name()))
			{
			case FNV1A::Hash32Const("Vars::Menu::PrimaryKey"):
				vIgnore = { Vars::Menu::SecondaryKey[DEFAULT_BIND], VK_LBUTTON, VK_RBUTTON };
				break;
			case FNV1A::Hash32Const("Vars::Menu::SecondaryKey"):
				vIgnore = { Vars::Menu::PrimaryKey[DEFAULT_BIND], VK_LBUTTON, VK_RBUTTON };
				break;
			default:
				vIgnore = { Vars::Menu::PrimaryKey[DEFAULT_BIND], Vars::Menu::SecondaryKey[DEFAULT_BIND] };
			}
			FKeybind(iOverride != -1 ? pVar->m_vNames[iOverride] : pVar->m_vNames.front(), pVar->Map[DEFAULT_BIND], !(i % 2) ? FButtonEnum::Left : FButtonEnum::Right | FButtonEnum::SameLine, vIgnore);
			break;
		}
		}

		if (iOverride != -2)
			i += i > 1 ? 1 : 2;
	}
	if (uLastSection)
		EndSection();
}
#pragma endregion

#pragma region Draggables
static inline void SquareConstraints(ImGuiSizeCallbackData* data)
{
	//data->DesiredSize.x = data->DesiredSize.y = std::max(data->DesiredSize.x, data->DesiredSize.y);
	data->DesiredSize.x = data->DesiredSize.y = (data->DesiredSize.x + data->DesiredSize.y) / 2;
}

static inline void OptionalConstraints(ImGuiSizeCallbackData* data)
{
	if (U::KeyHandler.Down(VK_SHIFT))
		SquareConstraints(data);
}

struct DragBoxStorage_t
{
	DragBox_t m_tDragBox;
	float m_flScale;
};
static std::unordered_map<uint32_t, DragBoxStorage_t> s_mDragBoxStorage = {};
void CMenu::AddDraggable(const char* sLabel, ConfigVar<DragBox_t>& tVar, bool bShouldDraw, ImVec2 vSize)
{
	using namespace ImGui;

	if (!bShouldDraw)
		return;

	auto tDragBox = FGet(tVar, true);
	auto uHash = FNV1A::Hash32(sLabel);

	bool bContains = s_mDragBoxStorage.contains(uHash);
	auto& tStorage = s_mDragBoxStorage[uHash];

	SetNextWindowSize(vSize, ImGuiCond_Always);
	if (!bContains || tDragBox != tStorage.m_tDragBox || H::Draw.Scale() != tStorage.m_flScale)
		SetNextWindowPos({ float(tDragBox.x - vSize.x / 2), float(tDragBox.y) }, ImGuiCond_Always);

	PushStyleColor(ImGuiCol_WindowBg, {});
	PushStyleColor(ImGuiCol_Border, F::Render.Active.Value);
	PushStyleVar(ImGuiStyleVar_WindowRounding, H::Draw.Scale(3));
	PushStyleVar(ImGuiStyleVar_WindowBorderSize, H::Draw.Scale(1));
	PushStyleVar(ImGuiStyleVar_WindowMinSize, vSize);
	if (Begin(sLabel, nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings))
	{
		ImVec2 vWindowPos = GetWindowPos();

		tDragBox.x = vWindowPos.x + vSize.x / 2, tDragBox.y = vWindowPos.y;
		tStorage = { tDragBox, H::Draw.Scale() };
		FSet(tVar, tDragBox);

		PushFont(F::Render.FontBold);
		ImVec2 vTextSize = FCalcTextSize(sLabel);
		SetCursorPos({ (vSize.x - vTextSize.x) * 0.5f, (vSize.y - vTextSize.y) * 0.5f });
		FText(sLabel);
		PopFont();

		End();
	}
	PopStyleVar(3);
	PopStyleColor(2);
}

struct WindowBoxStorage_t
{
	WindowBox_t m_tWindowBox;
	float m_flScale;
};
static std::unordered_map<uint32_t, WindowBoxStorage_t> s_mWindowBoxStorage = {};
void CMenu::AddResizableDraggable(const char* sLabel, ConfigVar<WindowBox_t>& tVar, bool bShouldDraw, ImGuiSizeCallback fCustomCallback, ImVec2 vMinSize, ImVec2 vMaxSize)
{
	using namespace ImGui;

	if (!bShouldDraw)
		return;

	auto tWindowBox = FGet(tVar, true);
	auto uHash = FNV1A::Hash32(sLabel);

	bool bContains = s_mWindowBoxStorage.contains(uHash);
	auto& tStorage = s_mWindowBoxStorage[uHash];

	SetNextWindowSizeConstraints(vMinSize, vMaxSize, fCustomCallback);
	if (!bContains || tWindowBox != tStorage.m_tWindowBox || H::Draw.Scale() != tStorage.m_flScale)
	{
		SetNextWindowPos({ float(tWindowBox.x - tWindowBox.w / 2), float(tWindowBox.y) }, ImGuiCond_Always);
		SetNextWindowSize({ float(tWindowBox.w), float(tWindowBox.h) }, ImGuiCond_Always);
	}

	PushStyleColor(ImGuiCol_WindowBg, {});
	PushStyleColor(ImGuiCol_Border, F::Render.Active.Value);
	PushStyleVar(ImGuiStyleVar_WindowRounding, H::Draw.Scale(3));
	PushStyleVar(ImGuiStyleVar_WindowBorderSize, H::Draw.Scale(1));
	if (Begin(sLabel, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings))
	{
		ImVec2 vWindowPos = GetWindowPos();
		ImVec2 vWinSize = GetWindowSize();

		tWindowBox.w = vWinSize.x, tWindowBox.h = vWinSize.y;
		tWindowBox.x = vWindowPos.x + tWindowBox.w / 2, tWindowBox.y = vWindowPos.y;
		tStorage = { tWindowBox, H::Draw.Scale() };
		FSet(tVar, tWindowBox);

		PushFont(F::Render.FontBold);
		ImVec2 vTextSize = FCalcTextSize(sLabel);
		SetCursorPos({ (vWinSize.x - vTextSize.x) * 0.5f, (vWinSize.y - vTextSize.y) * 0.5f });
		FText(sLabel);
		PopFont();

		End();
	}
	PopStyleVar(2);
	PopStyleColor(2);
}

struct BindInfo_t
{
	const char* sName;
	std::string sInfo;
	std::string sState;

	int iBind;
	Bind_t& tBind;
};
void CMenu::DrawBinds()
{
	using namespace ImGui;

	if (!F::Binds.m_bDisplay)
		return;

	std::vector<BindInfo_t> vInfo;
	std::function<void(int)> fGetBinds = [&](int iParent)
	{
		for (int iBind = 0; iBind < F::Binds.m_vBinds.size(); iBind++)
		{
			auto& tBind = F::Binds.m_vBinds[iBind];
			if (iParent != tBind.m_iParent || !tBind.m_bEnabled && !m_bIsOpen)
				continue;

			if (tBind.m_iVisibility == BindVisibilityEnum::Always || tBind.m_iVisibility == BindVisibilityEnum::WhileActive && tBind.m_bActive || m_bIsOpen)
			{
				std::string sType; std::string sInfo;
				switch (tBind.m_iType)
				{
				case BindEnum::Key:
					switch (tBind.m_iInfo)
					{
					case BindEnum::KeyEnum::Hold: { sType = "hold"; break; }
					case BindEnum::KeyEnum::Toggle: { sType = "toggle"; break; }
					case BindEnum::KeyEnum::DoubleClick: { sType = "double"; break; }
					}
					sInfo = U::KeyHandler.String(tBind.m_iKey);
					break;
				case BindEnum::Class:
					sType = "class";
					switch (tBind.m_iInfo)
					{
					case BindEnum::ClassEnum::Scout: { sInfo = "scout"; break; }
					case BindEnum::ClassEnum::Soldier: { sInfo = "soldier"; break; }
					case BindEnum::ClassEnum::Pyro: { sInfo = "pyro"; break; }
					case BindEnum::ClassEnum::Demoman: { sInfo = "demoman"; break; }
					case BindEnum::ClassEnum::Heavy: { sInfo = "heavy"; break; }
					case BindEnum::ClassEnum::Engineer: { sInfo = "engineer"; break; }
					case BindEnum::ClassEnum::Medic: { sInfo = "medic"; break; }
					case BindEnum::ClassEnum::Sniper: { sInfo = "sniper"; break; }
					case BindEnum::ClassEnum::Spy: { sInfo = "spy"; break; }
					}
					break;
				case BindEnum::WeaponType:
					sType = "weapon";
					switch (tBind.m_iInfo)
					{
					case BindEnum::WeaponTypeEnum::Hitscan: { sInfo = "hitscan"; break; }
					case BindEnum::WeaponTypeEnum::Projectile: { sInfo = "projectile"; break; }
					case BindEnum::WeaponTypeEnum::Melee: { sInfo = "melee"; break; }
					case BindEnum::WeaponTypeEnum::Throwable: { sInfo = "throwable"; break; }
					}
					break;
				case BindEnum::ItemSlot:
					sType = "slot";
					sInfo = std::format("{}", tBind.m_iInfo + 1);
					break;
				case BindEnum::Misc:
					switch (tBind.m_iInfo)
					{
					case BindEnum::MiscEnum::Spectated:
					case BindEnum::MiscEnum::SpectatedFirst:
					case BindEnum::MiscEnum::SpectatedThird:
						sType = "spectated";
						switch (tBind.m_iInfo)
						{
						case BindEnum::MiscEnum::Spectated: { sInfo = "any"; break; }
						case BindEnum::MiscEnum::SpectatedFirst: { sInfo = "1st"; break; }
						case BindEnum::MiscEnum::SpectatedThird: { sInfo = "3rd"; break; }
						}
						break;
					case BindEnum::MiscEnum::Zoomed:
					case BindEnum::MiscEnum::Aiming:
						sType = "cond";
						switch (tBind.m_iInfo)
						{
						case BindEnum::MiscEnum::Zoomed: { sInfo = "zoomed"; break; }
						case BindEnum::MiscEnum::Aiming: { sInfo = "aiming"; break; }
						}
						break;
					}
					break;
				}
				if (tBind.m_bNot && (tBind.m_iType != BindEnum::Key || tBind.m_iInfo == BindEnum::KeyEnum::Hold))
					sInfo = std::format("not {}", sInfo);

				vInfo.emplace_back(tBind.m_sName.c_str(), sType, sInfo, iBind, tBind);
			}

			if (tBind.m_bActive || m_bIsOpen)
				fGetBinds(iBind);
		}
	};
	fGetBinds(DEFAULT_BIND);
	if (vInfo.empty())
		return;

	static DragBox_t tOld = { -2147483648, -2147483648 };
	DragBox_t tDragBox = m_bIsOpen ? FGet(Vars::Menu::BindsDisplay, true) : Vars::Menu::BindsDisplay.Value;
	if (tDragBox != tOld)
		SetNextWindowPos({ float(tDragBox.x), float(tDragBox.y) }, ImGuiCond_Always);

	float flNameWidth = 0, flInfoWidth = 0, flStateWidth = 0;
	PushFont(F::Render.FontSmall);
	for (auto& [sName, sInfo, sState, iBind, tBind] : vInfo)
	{
		flNameWidth = std::max(flNameWidth, FCalcTextSize(sName).x);
		flInfoWidth = std::max(flInfoWidth, FCalcTextSize(sInfo.c_str()).x);
		flStateWidth = std::max(flStateWidth, FCalcTextSize(sState.c_str()).x);
	}
	PopFont();
	flNameWidth += H::Draw.Scale(9), flInfoWidth += H::Draw.Scale(9), flStateWidth += H::Draw.Scale(9);

	float flWidth = flNameWidth + flInfoWidth + flStateWidth + (m_bIsOpen ? H::Draw.Scale(113) : H::Draw.Scale(14));
	float flHeight = H::Draw.Scale(18 * vInfo.size() + (Vars::Menu::BindWindowTitle.Value ? 42 : 12));
	SetNextWindowSize({ flWidth, flHeight }, ImGuiCond_Always);
	PushStyleVar(ImGuiStyleVar_WindowMinSize, { H::Draw.Scale(40), H::Draw.Scale(40) });
	if (Begin("Binds", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings))
	{
		ImVec2 vWindowPos = GetWindowPos();

		if (Vars::Menu::BindWindowTitle.Value)
			RenderTwoToneBackground(H::Draw.Scale(28), F::Render.Background0, F::Render.Background0p5, F::Render.Background2);
		else
			RenderBackground(F::Render.Background0p5, F::Render.Background2);

		tDragBox.x = vWindowPos.x; tDragBox.y = vWindowPos.y; tOld = tDragBox;
		if (m_bIsOpen)
			FSet(Vars::Menu::BindsDisplay, tDragBox);

		int iListStart = 8;
		if (Vars::Menu::BindWindowTitle.Value)
		{
			SetCursorPos({ H::Draw.Scale(8), H::Draw.Scale(6) });
			IconImage(ICON_MD_KEYBOARD, F::Render.Accent);
			PushFont(F::Render.FontLarge);
			SetCursorPos({ H::Draw.Scale(30), H::Draw.Scale(7) });
			FText("Binds");
			PopFont();

			iListStart = 36;
		}

		PushFont(F::Render.FontSmall);
		int i = 0; for (auto& [sName, sInfo, sState, iBind, tBind] : vInfo)
		{
			float flPosX = 0;

			if (m_bIsOpen)
				PushTransparent(!F::Binds.WillBeEnabled(iBind), true);

			SetCursorPos({ flPosX += H::Draw.Scale(12), H::Draw.Scale(iListStart + 18 * i) });
			PushStyleColor(ImGuiCol_Text, tBind.m_bActive ? F::Render.Accent.Value : F::Render.Inactive.Value);
			FText(sName);
			PopStyleColor();

			SetCursorPos({ flPosX += flNameWidth, H::Draw.Scale(iListStart + 18 * i) });
			PushStyleColor(ImGuiCol_Text, tBind.m_bActive ? F::Render.Active.Value : F::Render.Inactive.Value);
			FText(sInfo.c_str());

			SetCursorPos({ flPosX += flInfoWidth, H::Draw.Scale(iListStart + 18 * i) });
			FText(sState.c_str());
			PopStyleColor();

			if (m_bIsOpen)
			{	// buttons
				SetCursorPos({ flWidth - H::Draw.Scale(26), H::Draw.Scale(iListStart - 2 + 18 * i) });
				bool bDelete = IconButton(ICON_MD_DELETE, H::Draw.Scale(18));

				SetCursorPos({ flWidth - H::Draw.Scale(51), H::Draw.Scale(iListStart - 2 + 18 * i) });
				bool bNot = IconButton(!tBind.m_bNot ? ICON_MD_CODE : ICON_MD_CODE_OFF, H::Draw.Scale(18));

				PushTransparent(Transparent || tBind.m_iVisibility == BindVisibilityEnum::Hidden, true);
				SetCursorPos({ flWidth - H::Draw.Scale(76), H::Draw.Scale(iListStart - 2 + 18 * i) });
				bool bVisibility = IconButton(tBind.m_iVisibility == BindVisibilityEnum::Always ? ICON_MD_VISIBILITY : ICON_MD_VISIBILITY_OFF, H::Draw.Scale(18));
				PopTransparent(1, 1);

				SetCursorPos({ flWidth - H::Draw.Scale(101), H::Draw.Scale(iListStart - 2 + 18 * i) });
				bool bEnable = IconButton(tBind.m_bEnabled ? ICON_MD_TOGGLE_ON : ICON_MD_TOGGLE_OFF, H::Draw.Scale(18));

				PopTransparent(1, 1);

				PushFont(F::Render.FontRegular);
				PushStyleVar(ImGuiStyleVar_WindowPadding, { H::Draw.Scale(8), H::Draw.Scale(8) });

				if (bEnable)
					tBind.m_bEnabled = !tBind.m_bEnabled;
				else if (bVisibility)
					tBind.m_iVisibility = (tBind.m_iVisibility + 1) % 3;
				else if (bNot)
					tBind.m_bNot = !tBind.m_bNot;
				else if (bDelete)
				{
					if (tBind.m_vVars.size() <= 1 && !F::Binds.HasChildren(iBind) || U::KeyHandler.Down(VK_SHIFT)) // allow user to quickly remove binds
						F::Binds.RemoveBind(iBind);
					else
						OpenPopup(std::format("DeleteBind{}", iBind).c_str());
				}

				if (FBeginPopupModal(std::format("DeleteBind{}", iBind).c_str()))
				{
					FText(std::format("Do you really want to delete '{}'{}?", tBind.m_sName, F::Binds.HasChildren(iBind) ? " and all of its children" : "").c_str());

					SetCursorPosY(GetCursorPosY() - 8); // stupid and i don't know why this is needed here
					if (FButton("Yes", FButtonEnum::Left))
					{
						F::Binds.RemoveBind(iBind);
						CloseCurrentPopup();
					}
					if (FButton("No", FButtonEnum::Right | FButtonEnum::SameLine))
						CloseCurrentPopup();

					EndPopup();
				}

				PopStyleVar();
				PopFont();
			}

			i++;
		}
		PopFont();

		End();
	}
	PopStyleVar();
}
#pragma endregion

static inline void ManageVars()
{
	Vars::ESP::ActiveGroups.m_vValues = {};
	for (auto& tGroup : F::Groups.m_vGroups)
		Vars::ESP::ActiveGroups.m_vValues.push_back(tGroup.m_sName.c_str());
}

// Menu-toggle key handling, split out of Render so CRender can poll it every
// Present even on frames where the whole ImGui pass is skipped (menu closed,
// nothing ImGui-drawn pending) - otherwise the menu could never open again.
// No ImGui calls in here.
void CMenu::HandleToggle()
{
	m_bInKeybind = false;
	if (m_bIsOpen)
	{
		for (short iKey = 1; iKey < 255; iKey++)
			U::KeyHandler.StoreKey(iKey);
	}
	else
	{
		U::KeyHandler.StoreKey(Vars::Menu::PrimaryKey.Value);
		U::KeyHandler.StoreKey(Vars::Menu::SecondaryKey.Value);
	}
	if (U::KeyHandler.Pressed(Vars::Menu::PrimaryKey.Value) || U::KeyHandler.Pressed(Vars::Menu::SecondaryKey.Value))
		I::MatSystemSurface->SetCursorAlwaysVisible(m_bIsOpen = !m_bIsOpen);
}

void CMenu::Render()
{
	using namespace ImGui;

	if (!(GetIO().DisplaySize.x > 160.f && GetIO().DisplaySize.y > 28.f))
		return;

	PushFont(F::Render.FontRegular);
	if (m_bIsOpen)
	{
		static std::once_flag tDescFlag; std::call_once(tDescFlag, [] { InitDescriptions(); });
		ManageVars();
		DrawMenu();

		AddDraggable("Ticks", Vars::Menu::TicksDisplay, FGet(Vars::Menu::Indicators) & Vars::Menu::IndicatorsEnum::Ticks);
		AddDraggable("Crit hack", Vars::Menu::CritsDisplay, FGet(Vars::Menu::Indicators) & Vars::Menu::IndicatorsEnum::CritHack);
		AddDraggable("Spectators", Vars::Menu::SpectatorsDisplay, FGet(Vars::Menu::Indicators) & Vars::Menu::IndicatorsEnum::Spectators);
		AddDraggable("Ping", Vars::Menu::PingDisplay, FGet(Vars::Menu::Indicators) & Vars::Menu::IndicatorsEnum::Ping);
		AddDraggable("Conditions", Vars::Menu::ConditionsDisplay, FGet(Vars::Menu::Indicators) & Vars::Menu::IndicatorsEnum::Conditions);
		AddDraggable("Seed prediction", Vars::Menu::SeedPredictionDisplay, FGet(Vars::Menu::Indicators) & Vars::Menu::IndicatorsEnum::SeedPrediction);
		AddDraggable("Crit bar", Vars::Visuals::CritBar::Display, FGet(Vars::Visuals::CritBar::Enabled));
		AddDraggable("Alerts", Vars::Visuals::Alerts::Display, FGet(Vars::Visuals::Alerts::Enabled));
		AddResizableDraggable("Camera", Vars::Visuals::Simulation::ProjectileWindow, FGet(Vars::Visuals::Simulation::ProjectileCamera), OptionalConstraints);

		F::Render.Cursor = GetMouseCursor();
		m_bWindowHovered = IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

		if (!DisabledVec.empty())
		{
			IM_ASSERT_USER_ERROR(0, "Calling PopDisabled() too little times: stack overflow.");
			Disabled = false;
			DisabledVec.clear();
		}
		if (!TransparentVec.empty())
		{
			IM_ASSERT_USER_ERROR(0, "Calling PopTransparent() too little times: stack overflow.");
			Transparent = false;
			TransparentVec.clear();
		}
	}
	else
	{
		ActiveMap.clear();
		m_bWindowHovered = false;
	}
	DrawBinds();
	F::Notifications.Draw();
	PopFont();
}

void CMenu::AddOutput(const char* sFunction, const char* sLog, Color_t tColor)
{
	static size_t iID = 0;

	m_vOutput.emplace_back(sFunction, sLog, iID++, tColor);
	while (m_vOutput.size() > m_iMaxOutputSize)
		m_vOutput.pop_front();
}