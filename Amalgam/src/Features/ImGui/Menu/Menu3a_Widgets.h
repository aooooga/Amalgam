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
		ImColor tTrack = bOn ? F::Render.SwitchOn : F::Render.SwitchOff;
		ImColor tKnob  = bOn ? F::Render.SwitchKnobOn : F::Render.SwitchKnobOff;
		tTrack.Value.w *= flAlpha; tKnob.Value.w *= flAlpha;
		dl->AddRectFilled(vPos, vPos + ImVec2(w, h), tTrack, r);
		float kx = bOn ? w - r : r;
		dl->AddCircleFilled(vPos + ImVec2(kx, r), r - H::Draw.Scale(2), tKnob);
	}

	// g_SliderColumn / SliderPairOverride live in Components.h so Section() and
	// SubGroup() -- which are defined there, before this header -- can reset the
	// column run at each group boundary.

	// RAII: force full-width sliders for a block, then restore.
	struct StackedSliders
	{
		bool m_bPrev;
		StackedSliders() : m_bPrev(SliderPairOverride) { SliderPairOverride = true; ResetSliderColumns(); }
		~StackedSliders() { SliderPairOverride = m_bPrev; ResetSliderColumns(); }
	};

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
		ResetSliderColumns(); // close any half-open slider row before this block
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

		// A toggle ends any half-open slider row, so it never lands beside a stranded
		// left-hand slider.
		ResetSliderColumns();

		bool bFull = !(iFlags & (FToggleEnum::Left | FToggleEnum::Right));
		float flW = GetWindowWidth();
		if (bFull)
			flW -= GetStyle().WindowPadding.x * 2;
		else
			flW = flW / 2 - GetStyle().WindowPadding.x * 1.5f;
		if (iFlags & FToggleEnum::Right)
			SameLine(flW + GetStyle().WindowPadding.x * 2);

		// Fixed row height: the description is a real hover tooltip now, so the row
		// never grows and the list stops shifting under the cursor.
		float flRowH = H::Draw.Scale(30);

		ImVec2 vStart = GetCursorPos();
		ImVec2 vDraw  = GetDrawPos() + vStart;
		bool bHovered = !Disabled && IsWindowHovered() && IsMouseWithin(vDraw.x, vDraw.y, flW, flRowH);

		ImDrawList* dl = GetWindowDrawList();
		if (vStart.y > GetStyle().WindowPadding.y + H::Draw.Scale(1)) // divider, not on first row
			dl->AddRectFilled(vDraw, vDraw + ImVec2(flW, H::Draw.Scale(1)), F::Render.RowDivider);

		if (InvisibleButton(std::format("##{}", tVar.Name()).c_str(), { flW, flRowH }))
		{
			bVal = !bVal;
			FSet(tVar, bVal);
		}
		if (bHovered) SetMouseCursor(ImGuiMouseCursor_Hand);
		float flAlpha = (Transparent || Disabled) ? 0.5f : 1.f;

		PushFont(F::Render.FontRegular);
		ImColor tCol = bVal ? F::Render.Active : F::Render.Inactive; tCol.Value.w *= flAlpha;
		// leave room for the right-pinned switch (34px + gutter)
		float flTextW = flW - H::Draw.Scale(34 + 14);
		dl->AddText({ vDraw.x + H::Draw.Scale(4), vDraw.y + (flRowH - GetFontSize()) / 2.f }, tCol,
			TruncateText(sText, flTextW, F::Render.FontRegular).c_str());
		PopFont();

		float swH = H::Draw.Scale(18);
		DrawSwitch(vDraw + ImVec2(flW - H::Draw.Scale(34), (flRowH - swH) / 2.f), bVal, flAlpha);

		DescTooltip(&tVar, bHovered, sText.c_str());

		SetCursorPos(vStart);
		DebugDummy({ flW, flRowH });
		if (pHovered) *pHovered = bHovered;
		return false;
	}

	// Widest string this slider can ever show, measured in the font the value is
	// actually drawn in. Sampling the endpoints (plus a mid value, which can be the
	// longest for "%g"-style formats) keeps the label's budget stable while dragging
	// -- budgeting off the *current* value would make the layout shift as it changes.
	template <class T>
	inline float SliderValueWidth(T tMin, T tMax, const char* fmt)
	{
		auto Measure = [&](T tVal) -> float
		{
			char szBuf[32];
			if constexpr (std::is_integral_v<T>)
				snprintf(szBuf, sizeof(szBuf), fmt ? fmt : "%i", int(tVal));
			else
				snprintf(szBuf, sizeof(szBuf), fmt ? fmt : "%g", double(tVal));
			return FCalcTextSize(szBuf, F::Render.FontRegular).x;
		};
		T tMid = T(double(tMin) + (double(tMax) - double(tMin)) * 0.5);
		return std::max({ Measure(tMin), Measure(tMax), Measure(tMid) });
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

		// Auto two-column packing. Two rules matter here, and getting either wrong
		// produces the "sometimes one column, sometimes two" flapping:
		//
		//  1. The decision must be made for the PAIR, not per slider. If the left
		//     slider pairs and the right one then decides it needs full width, the
		//     left is stranded as a half-width orphan with a gap beside it. So once
		//     a row is opened as paired, its partner is committed to the same row.
		//  2. The value column must be measured, not guessed. It is drawn right-
		//     aligned at its true text width; budgeting a fixed fraction here let a
		//     wide value ("-180.00") overrun the space the label was promised.
		float flAvail = GetWindowWidth() - GetStyle().WindowPadding.x * 2;
		float flHalf = GetWindowWidth() / 2 - GetStyle().WindowPadding.x * 1.5f;
		float flLabelPx = FCalcTextSize(sText.c_str(), F::Render.FontRegular).x;
		float flValPx = SliderValueWidth<T>(tMin, tMax, fmt);

		// what this row needs to show its label in full, unpaired-clipping free
		float flNeeds = flLabelPx + flValPx + H::Draw.Scale(30) + H::Draw.Scale(50);

		// Pairing is opt-in: the caller must pass Left/Right to signal "this row has
		// a partner". Without that, the row is never halved, no matter how narrow its
		// content is -- otherwise a lone slider that happens to measure short (e.g.
		// "Scale" with a "1.00" value) gets crammed into the left column with nothing
		// beside it, which reads as a layout bug even though the content fit the math.
		bool bPairIntent = (iFlags & (FSliderEnum::Left | FSliderEnum::Right)) != 0;

		bool bFull;
		if (g_SliderColumn)
		{
			// committed: we are the right half of a row already opened as paired
			bFull = false;
		}
		else if (!bPairIntent)
		{
			bFull = true;
		}
		else
		{
			bFull = SliderPairOverride || flHalf < flNeeds;
		}

		float flW = bFull ? flAvail : flHalf;
		if (!bFull)
		{
			if (g_SliderColumn)
				SameLine(flW + GetStyle().WindowPadding.x * 2);
			g_SliderColumn ^= 1;
		}
		else
			g_SliderColumn = 0;

		ImVec2 vStart = GetCursorPos();
		ImVec2 vDraw  = GetDrawPos() + vStart;
		float flRowH  = H::Draw.Scale(34);

		bool bRowHover = !Disabled && IsWindowHovered() && IsMouseWithin(vDraw.x, vDraw.y, flW, flRowH);

		ImDrawList* dl = GetWindowDrawList();
		if (vStart.y > GetStyle().WindowPadding.y + H::Draw.Scale(1))
			dl->AddRectFilled(vDraw, vDraw + ImVec2(flW, H::Draw.Scale(1)), F::Render.RowDivider);

		float flAlpha = (Transparent || Disabled) ? 0.5f : 1.f;

		// geometry: [label ...][track][value]. Uses the SAME measured value width as
		// the pairing test above, so the space the label was promised is the space it
		// actually gets. The track absorbs the slack and never claims a minimum that
		// would push its left edge back over the label.
		float flTrkMax = flW - flValPx - H::Draw.Scale(30) - flLabelPx;
		float flTrkW = std::min(std::max(flTrkMax, H::Draw.Scale(50)), H::Draw.Scale(190));
		float flTrkX = vDraw.x + flW - flValPx - H::Draw.Scale(10) - flTrkW;
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

		// track + fill + knob. AddSteppedRect draws the track broken by a gap at each
		// increment (the same stroke the classic FSlider uses), so the step size stays
		// legible; it falls back to a solid bar when the steps get too dense to show.
		float flTrkH = H::Draw.Scale(4);
		float pct = float((double(tVal) - double(tMin)) / std::max(1e-6, double(tMax - tMin)));
		pct = std::clamp(pct, 0.f, 1.f);
		ImColor tFill = F::Render.SliderFill; tFill.Value.w *= flAlpha;
		ImColor tRest = F::Render.SliderTrack; tRest.Value.w *= flAlpha;

		ImVec2 vMins = { flTrkX, flMidY - flTrkH / 2.f };
		ImVec2 vMaxs = { flTrkX + flTrkW, flMidY + flTrkH / 2.f };
		float flFillX = flTrkX + flTrkW * pct;
		// filled portion, then the remainder, each clipped to its own span
		AddSteppedRect({ 0, 0 }, vMins, vMaxs, vMins, { flFillX, vMaxs.y },
			float(tMin), float(tMax), float(tStep) != 0.f ? float(tStep) : 1.f, tFill, tRest, H::Draw.Scale(2));
		AddSteppedRect({ 0, 0 }, vMins, vMaxs, { flFillX, vMins.y }, vMaxs,
			float(tMin), float(tMax), float(tStep) != 0.f ? float(tStep) : 1.f, tRest, tRest, H::Draw.Scale(2));

		dl->AddRectFilled({ flFillX - H::Draw.Scale(1), flMidY - H::Draw.Scale(6) },
						  { flFillX + H::Draw.Scale(1), flMidY + H::Draw.Scale(6) }, F::Render.SliderKnob);

		// value (right-aligned in its fixed column)
		// The var's stored format string is written for the native type ("%i" for int,
		// "%g"/"%.2f" for float), so feed it the native value rather than a double.
		char szVal[32];
		if constexpr (std::is_integral_v<T>)
			snprintf(szVal, sizeof(szVal), fmt ? fmt : "%i", int(tVal));
		else
			snprintf(szVal, sizeof(szVal), fmt ? fmt : "%g", double(tVal));
		PushFont(F::Render.FontRegular);
		float flvw = CalcTextSize(szVal).x;
		ImColor tValCol = F::Render.SliderValueText; tValCol.Value.w *= flAlpha;
		dl->AddText({ vDraw.x + flW - flvw, flMidY - GetFontSize() / 2.f }, tValCol, szVal);
		PopFont();

		DescTooltip(&tVar, bRowHover, sText.c_str());

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
