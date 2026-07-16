#pragma once
#include "../../Vars.h"
#include <array>

enum EFonts
{
	FONT_ESP,
	FONT_INDICATORS,
	FONT_COUNT
};

struct Font_t
{
	const char* m_szName;
	int m_nTall, m_nFlags, m_nWeight;
	unsigned long m_dwFont;
};

class CFonts
{
private:
	// Indexed by EFonts. A hash lookup per drawn text element bought nothing here.
	std::array<Font_t, FONT_COUNT> m_aFonts = {};

public:
	void Reload(float flDPI = Vars::Menu::Scale[DEFAULT_BIND], bool bOutline = Vars::Menu::CheapText[DEFAULT_BIND]);
	const Font_t& GetFont(EFonts eFont);
};

ADD_FEATURE_CUSTOM(CFonts, Fonts, H);