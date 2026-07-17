#include "CritBar.h"

#include "../../CritHack/CritHack.h"

void CCritBar::Draw(CTFPlayer* pLocal)
{
	if (!Vars::Visuals::CritBar::Enabled.Value || !I::EngineClient->IsInGame())
		return;

	auto pWeapon = H::Entities.GetWeapon();
	if (!pWeapon || !pLocal->IsAlive() || pLocal->IsAGhost() || !F::CritHack.WeaponCanCrit(pWeapon, true))
		return;

	const int iAvailable = F::CritHack.GetAvailableCrits();
	const int iPotential = std::max(F::CritHack.GetPotentialCrits(), 1);
	const bool bBanned = F::CritHack.IsCritBanned(); // crit-banned = not currently allowed to crit
	const int iSafe = std::clamp(F::CritHack.GetSafeCrits(), 0, iAvailable); // crits safe to fire before re-ban

	// Progress segment: charge toward the next crit while allowed, recovery toward being
	// allowed again while banned.
	const float flProgress = bBanned
		? std::clamp(F::CritHack.GetUnbanProgress(), 0.f, 1.f)
		: std::clamp(F::CritHack.GetCritProgress(), 0.f, 1.f);

	const int w = H::Draw.Scale(Vars::Visuals::CritBar::Width.Value, Scale_Round);
	const int h = H::Draw.Scale(Vars::Visuals::CritBar::Height.Value, Scale_Round);
	const int iBorder = H::Draw.Scale(Vars::Visuals::CritBar::Border.Value, Scale_Round);

	// Display anchor is a horizontally-centered drag handle (see CMenu::AddDraggable).
	const int x = Vars::Visuals::CritBar::Display.Value.x - w / 2;
	const int y = Vars::Visuals::CritBar::Display.Value.y;

	const auto& tBorder = Vars::Visuals::CritBar::BorderColor.Value;
	const auto& tBackground = Vars::Visuals::CritBar::BackgroundColor.Value;
	// Separate color per bar type, each with an allowed and a banned variant.
	const auto& tCell = bBanned ? Vars::Visuals::CritBar::BannedCellColor.Value : Vars::Visuals::CritBar::CellColor.Value;
	const auto& tProgress = bBanned ? Vars::Visuals::CritBar::BannedProgressColor.Value : Vars::Visuals::CritBar::ProgressColor.Value;

	// Inner region (inside the border), split into iPotential equal cells.
	const int ix = x + iBorder;
	const int iy = y + iBorder;
	const int iw = std::max(w - iBorder * 2, 1);
	const int ih = std::max(h - iBorder * 2, 1);

	// Background fill spans the whole inner region.
	H::Draw.FillRect(ix, iy, iw, ih, tBackground);

	// Solid cells:
	//  allowed - only the crits that are safe to fire before re-triggering a ban.
	//  banned  - every banked crit, shown locked so you can see what's waiting.
	const int iSolid = bBanned ? iAvailable : iSafe;

	for (int i = 0; i < iPotential; i++)
	{
		const int iCellX = ix + iw * i / iPotential;
		const int iCellW = std::max(ix + iw * (i + 1) / iPotential - iCellX, 1);

		if (i < iSolid)
			H::Draw.FillRect(iCellX, iy, iCellW, ih, tCell);
		else if (i == iSolid && flProgress > 0.f)
			H::Draw.FillRect(iCellX, iy, std::max(int(iCellW * flProgress), 1), ih, tProgress);

		// Cell separator (skip the leading edge, the border covers it).
		if (i > 0 && iBorder > 0)
			H::Draw.FillRect(iCellX, iy, std::max(iBorder / 2, 1), ih, tBorder);
	}

	// Outer border.
	if (iBorder > 0)
	{
		H::Draw.FillRect(x, y, w, iBorder, tBorder);                 // top
		H::Draw.FillRect(x, y + h - iBorder, w, iBorder, tBorder);   // bottom
		H::Draw.FillRect(x, y, iBorder, h, tBorder);                 // left
		H::Draw.FillRect(x + w - iBorder, y, iBorder, h, tBorder);   // right
	}

	// Text label: tell the player what they must do before they can crit again.
	if (Vars::Visuals::CritBar::Text.Value)
	{
		std::string sText;
		if (bBanned)
			sText = std::format("Deal {} damage", int(ceilf(std::max(F::CritHack.GetDamageTilFlip(), 0.f)))); // damage to un-ban
		else if (iSafe > 0)
			sText = std::format("{} crit{} ready", iSafe, iSafe == 1 ? "" : "s");
		else if (iAvailable > 0)
			sText = "Deal damage"; // have crits, but firing one now would re-ban - build headroom first
		else
			sText = "Charging crits"; // token bucket still filling

		const auto& fFont = H::Fonts.GetFont(FONT_INDICATORS);
		H::Draw.StringOutlined(fFont, Vars::Visuals::CritBar::Display.Value.x, y + h + H::Draw.Scale(2),
			tCell, Vars::Menu::Theme::Background.Value, ALIGN_TOP, sText.c_str());
	}
}
