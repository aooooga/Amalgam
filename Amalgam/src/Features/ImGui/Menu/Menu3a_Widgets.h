// ============================================================================
// Menu3a_Widgets.h  —  "Tiered Rows" (design 3a) widgets for Amalgam's ImGui menu
// ----------------------------------------------------------------------------
// Drop-in ADDITIVE widgets. They do not replace FToggle/FSlider/FDropdown, so
// every other tab is untouched. They mirror the existing WRAPPER mechanism in
// Components.h: label via DisplayLabelFor(&var, var.m_vNames.front()), value via
// FGet/FSet (bind-aware), description via g_Descriptions[&var]. Colors are all
// theme-derived (F::Render.*) so they follow the user's Accent/Background/etc.
//
// Requires the two tweak vars from Vars_and_Section_patches.md:
//   Vars::Menu::CompactColumns      (bool) - pack toggle-heavy cards into columns
//   Vars::Menu::DescriptionsOnHover (bool) - hide a row's helper text until hover
//
// Include this AFTER Components.h (it uses ImGui::FGet/FSet/g_Descriptions/etc.):
//   #include "Menu/Menu3a_Widgets.h"
// ============================================================================
#pragma once
#include "Components.h"

namespace ImGui
{
	// --- the 3a signature switch (replaces the checkbox for on/off controls) ---
	// track 34x18 rounded pill; ON -> Accent track + dark knob; OFF -> muted track + light knob.
	inline void DrawSwitch(ImVec2 vPos, bool bOn, float flAlpha = 1.f)
	{
		ImDrawList* dl = GetWindowDrawList();
		float w = H::Draw.Scale(34), h = H::Draw.Scale(18), r = h / 2.f;
		ImColor tTrack = bOn ? F::Render.Accent : F::Render.Background2;
		ImColor tKnob  = bOn ? F::Render.Background0 : F::Render.Active;
		tTrack.Value.w *= flAlpha; tKnob.Value.w *= flAlpha;
		dl->AddRectFilled(vPos, vPos + ImVec2(w, h), tTrack, r);
		float kx = bOn ? w - r : r;
		dl->AddCircleFilled(vPos + ImVec2(kx, r), r - H::Draw.Scale(2), tKnob);
	}

	// How many columns a toggle-heavy card should use right now.
	// CompactColumns OFF -> 1 (classic divided list). ON -> as many ~158px cells as fit.
	inline int Toggle3aColumns()
	{
		if (!Vars::Menu::CompactColumns.Value)
			return 1;
		float flInner = GetWindowWidth() - GetStyle().WindowPadding.x * 2;
		return std::max(1, int(flInner / H::Draw.Scale(158)));
	}

	// One toggle drawn inside an explicit cell rect (used by FToggleGrid).
	// Compact form: [switch] label, no divider. Description shown via tooltip only.
	inline void FToggleCell(ConfigVar<bool>& tVar, float flCellW, float flRowH)
	{
		const char* sLabel = DisplayLabelFor(&tVar, tVar.m_vNames.front());
		std::string sText = StripDoubleHash(sLabel);
		bool bVal = FGet(tVar, true);

		ImVec2 vStart = GetCursorPos();
		ImVec2 vDraw  = GetDrawPos() + vStart;
		bool bHovered = !Disabled && IsWindowHovered() && IsMouseWithin(vDraw.x, vDraw.y, flCellW, flRowH);

		if (InvisibleButton(std::format("##{}", tVar.Name()).c_str(), { flCellW, flRowH }))
		{
			bVal = !bVal;
			FSet(tVar, bVal);
		}
		if (bHovered) SetMouseCursor(ImGuiMouseCursor_Hand);

		float flAlpha = (Transparent || Disabled) ? 0.5f : 1.f;
		float swH = H::Draw.Scale(18);
		ImVec2 swPos = vDraw + ImVec2(H::Draw.Scale(2), (flRowH - swH) / 2.f);
		DrawSwitch(swPos, bVal, flAlpha);

		PushFont(F::Render.FontRegular);
		ImColor tCol = bVal ? F::Render.Active : F::Render.Inactive; tCol.Value.w *= flAlpha;
		float flTextX = swPos.x + H::Draw.Scale(34 + 9);
		GetWindowDrawList()->AddText({ flTextX, vDraw.y + (flRowH - GetFontSize()) / 2.f },
			tCol, TruncateText(sText, flCellW - H::Draw.Scale(43 + 6), F::Render.FontRegular).c_str());
		PopFont();

		DescTooltip(&tVar, bHovered, sText.c_str()); // compact cells have no inline room -> tooltip
		SetCursorPos(vStart); // caller positions the next cell explicitly
	}

	// A block of toggles laid into auto-columns (the multi-column "compact" packing).
	// Removals (13 toggles) is the acceptance test: CompactColumns ON -> ~3 rows.
	inline void FToggleGrid(std::vector<ConfigVar<bool>*> vVars)
	{
		int iCols = Toggle3aColumns();
		float flInner = GetWindowWidth() - GetStyle().WindowPadding.x * 2;
		float flCell  = flInner / iCols;
		float flRowH  = H::Draw.Scale(30);
		ImVec2 vBase  = GetCursorPos();

		for (size_t i = 0; i < vVars.size(); i++)
		{
			int c = int(i) % iCols, r = int(i) / iCols;
			SetCursorPos(vBase + ImVec2(c * flCell, r * flRowH));
			FToggleCell(*vVars[i], flCell, flRowH);
		}
		int iRows = int((vVars.size() + iCols - 1) / iCols);
		SetCursorPos(vBase);
		DebugDummy({ flInner, flRowH * iRows });
	}

	// A single full-width labeled row: label (+ description) left, switch right, top divider.
	// Description visibility follows DescriptionsOnHover (hidden until row hover when ON).
	inline bool FToggleRow(ConfigVar<bool>& tVar, int iFlags = FToggleEnum::None, bool* pHovered = nullptr)
	{
		const char* sLabel = DisplayLabelFor(&tVar, tVar.m_vNames.front());
		std::string sText = StripDoubleHash(sLabel);
		bool bVal = FGet(tVar, true);

		const char* sDesc = nullptr;
		if (auto it = g_Descriptions.find(&tVar); it != g_Descriptions.end()) sDesc = it->second;

		bool bFull = !(iFlags & (FToggleEnum::Left | FToggleEnum::Right));
		float flW = GetWindowWidth();
		if (bFull)
			flW -= GetStyle().WindowPadding.x * 2;
		else
			flW = flW / 2 - GetStyle().WindowPadding.x * 1.5f;
		if (iFlags & FToggleEnum::Right)
			SameLine(flW + GetStyle().WindowPadding.x * 2);

		ImVec2 vStart = GetCursorPos();
		ImVec2 vDraw  = GetDrawPos() + vStart;
		bool bHovered = !Disabled && IsWindowHovered() && IsMouseWithin(vDraw.x, vDraw.y, flW, H::Draw.Scale(34));

		bool bReveal = sDesc && (!Vars::Menu::DescriptionsOnHover.Value || bHovered);
		float flRowH = H::Draw.Scale(bReveal ? 40 : 30);

		ImDrawList* dl = GetWindowDrawList();
		if (vStart.y > GetStyle().WindowPadding.y + H::Draw.Scale(1)) // divider, not on first row
			dl->AddRectFilled(vDraw, vDraw + ImVec2(flW, H::Draw.Scale(1)), F::Render.Background1p5);

		if (InvisibleButton(std::format("##{}", tVar.Name()).c_str(), { flW, flRowH }))
		{
			bVal = !bVal;
			FSet(tVar, bVal);
		}
		if (bHovered) SetMouseCursor(ImGuiMouseCursor_Hand);
		float flAlpha = (Transparent || Disabled) ? 0.5f : 1.f;

		PushFont(F::Render.FontRegular);
		ImColor tCol = bVal ? F::Render.Active : F::Render.Inactive; tCol.Value.w *= flAlpha;
		float flLabelY = bReveal ? vDraw.y + H::Draw.Scale(6) : vDraw.y + (flRowH - GetFontSize()) / 2.f;
		// leave room for the right-pinned switch (34px + gutter)
		float flTextW = flW - H::Draw.Scale(34 + 14);
		dl->AddText({ vDraw.x + H::Draw.Scale(4), flLabelY }, tCol,
			TruncateText(sText, flTextW, F::Render.FontRegular).c_str());
		PopFont();

		if (bReveal)
		{
			PushFont(F::Render.FontSmall);
			dl->AddText({ vDraw.x + H::Draw.Scale(4), vDraw.y + H::Draw.Scale(21) }, F::Render.Inactive,
				TruncateText(sDesc, flTextW, F::Render.FontSmall).c_str());
			PopFont();
		}

		float swH = H::Draw.Scale(18);
		DrawSwitch(vDraw + ImVec2(flW - H::Draw.Scale(34), (flRowH - swH) / 2.f), bVal, flAlpha);

		SetCursorPos(vStart);
		DebugDummy({ flW, flRowH });
		if (pHovered) *pHovered = bHovered;
		return false;
	}

	// A labeled slider row: label left, right-pinned track + bold value.
	// Real drag via InvisibleButton + IsItemActive() mapping mouse-X -> value (immediate mode).
	// Honours FSliderEnum::Left/Right for half-width pairing, like FSlider does; the track and
	// value column scale with the available width so narrow cards stay usable.
	template <class T>
	inline bool FSliderRow_(ConfigVar<T>& tVar, T tMin, T tMax, T tStep, const char* fmt, int iFlags)
	{
		const char* sLabel = DisplayLabelFor(&tVar, tVar.m_vNames.front());
		std::string sText = StripDoubleHash(sLabel);
		T tVal = FGet(tVar, true);

		const char* sDesc = nullptr;
		if (auto it = g_Descriptions.find(&tVar); it != g_Descriptions.end()) sDesc = it->second;

		// half-width pairing: Left takes the first half, Right sames-line onto the second
		bool bFull = !(iFlags & (FSliderEnum::Left | FSliderEnum::Right));
		float flW = GetWindowWidth();
		if (bFull)
			flW -= GetStyle().WindowPadding.x * 2;
		else
			flW = flW / 2 - GetStyle().WindowPadding.x * 1.5f;
		if (iFlags & FSliderEnum::Right)
			SameLine(flW + GetStyle().WindowPadding.x * 2);

		ImVec2 vStart = GetCursorPos();
		ImVec2 vDraw  = GetDrawPos() + vStart;
		float flRowH  = H::Draw.Scale(34);

		bool bRowHover = !Disabled && IsWindowHovered() && IsMouseWithin(vDraw.x, vDraw.y, flW, flRowH);

		ImDrawList* dl = GetWindowDrawList();
		if (vStart.y > GetStyle().WindowPadding.y + H::Draw.Scale(1))
			dl->AddRectFilled(vDraw, vDraw + ImVec2(flW, H::Draw.Scale(1)), F::Render.Background1p5);

		float flAlpha = (Transparent || Disabled) ? 0.5f : 1.f;

		// geometry: [label ...][track][value] — track/value scale with the row, clamped so
		// the label keeps room on narrow cards and the track stays grabbable on wide ones.
		float flValW = std::clamp(flW * 0.16f, H::Draw.Scale(34), H::Draw.Scale(70));
		float flTrkW = std::clamp(flW * 0.42f, H::Draw.Scale(60), H::Draw.Scale(190));
		float flTrkX = vDraw.x + flW - flValW - H::Draw.Scale(10) - flTrkW;
		float flMidY = vDraw.y + flRowH / 2.f;

		// drag hitbox over the track
		SetCursorPos({ vStart.x + (flTrkX - vDraw.x), vStart.y + flRowH / 2.f - H::Draw.Scale(8) });
		InvisibleButton(std::format("##{}", tVar.Name()).c_str(), { flTrkW, H::Draw.Scale(16) });
		if (!Disabled && IsItemActive())
		{
			float t = std::clamp((GetMousePos().x - flTrkX) / flTrkW, 0.f, 1.f);
			double dRaw = double(tMin) + double(tMax - tMin) * t;
			if (double(tStep) != 0.0) dRaw = std::round(dRaw / double(tStep)) * double(tStep);
			tVal = T(std::clamp(dRaw, double(tMin), double(tMax)));
			FSet(tVar, tVal);
		}
		if (IsItemHovered()) SetMouseCursor(ImGuiMouseCursor_Hand);

		// label
		PushFont(F::Render.FontRegular);
		ImColor tLab = F::Render.Inactive; tLab.Value.w *= flAlpha;
		dl->AddText({ vDraw.x + H::Draw.Scale(4), flMidY - GetFontSize() / 2.f }, tLab,
			TruncateText(sText, flTrkX - vDraw.x - H::Draw.Scale(12), F::Render.FontRegular).c_str());
		PopFont();

		// track + fill + knob
		float flTrkH = H::Draw.Scale(4);
		ImVec2 t0 = { flTrkX, flMidY - flTrkH / 2.f };
		float pct = float((double(tVal) - double(tMin)) / std::max(1e-6, double(tMax - tMin)));
		pct = std::clamp(pct, 0.f, 1.f);
		ImColor tFill = F::Render.Accent; tFill.Value.w *= flAlpha;
		dl->AddRectFilled(t0, t0 + ImVec2(flTrkW, flTrkH), F::Render.Background2, flTrkH / 2.f);
		dl->AddRectFilled(t0, t0 + ImVec2(flTrkW * pct, flTrkH), tFill, flTrkH / 2.f);
		dl->AddRectFilled({ flTrkX + flTrkW * pct - H::Draw.Scale(1), flMidY - H::Draw.Scale(6) },
						  { flTrkX + flTrkW * pct + H::Draw.Scale(1), flMidY + H::Draw.Scale(6) }, tFill);

		// value (right-aligned in its fixed column)
		// The var's stored format string is written for the native type ("%i" for int,
		// "%g"/"%.2f" for float), so feed it the native value rather than a double.
		char szVal[32];
		if constexpr (std::is_integral_v<T>)
			snprintf(szVal, sizeof(szVal), fmt ? fmt : "%i", int(tVal));
		else
			snprintf(szVal, sizeof(szVal), fmt ? fmt : "%g", double(tVal));
		PushFont(F::Render.FontBold);
		float flvw = CalcTextSize(szVal).x;
		ImColor tValCol = F::Render.Active; tValCol.Value.w *= flAlpha;
		dl->AddText({ vDraw.x + flW - flvw, flMidY - GetFontSize() / 2.f }, tValCol, szVal);
		PopFont();

		if (sDesc) DescTooltip(&tVar, bRowHover, sText.c_str());

		SetCursorPos(vStart);
		DebugDummy({ flW, flRowH });
		return false;
	}
	inline bool FSliderRow(ConfigVar<float>& v, int iFlags = FSliderEnum::None, const char* fmtOverride = nullptr)
	{ return FSliderRow_<float>(v, v.m_unMin.f, v.m_unMax.f, v.m_unStep.f, fmtOverride ? fmtOverride : v.m_sExtra, iFlags); }
	inline bool FSliderRow(ConfigVar<int>& v, int iFlags = FSliderEnum::None, const char* fmtOverride = nullptr)
	{ return FSliderRow_<int>(v, v.m_unMin.i, v.m_unMax.i, v.m_unStep.i, fmtOverride ? fmtOverride : (v.m_sExtra ? v.m_sExtra : "%i"), iFlags); }

	// Dropdown row: keep the existing FDropdown (already a labeled value+chevron chip that
	// matches the 3a chip spec). Just call FDropdown(var) full-width inside the card.
}
