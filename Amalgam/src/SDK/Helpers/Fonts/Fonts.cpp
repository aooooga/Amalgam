#include "Fonts.h"

#include "../../Definitions/Interfaces/IMatSystemSurface.h"

void CFonts::Reload(float flDPI, bool bOutline)
{
	int iFlags = !bOutline ? FONTFLAG_ANTIALIAS : FONTFLAG_ANTIALIAS | FONTFLAG_DROPSHADOW;

	m_aFonts[FONT_ESP] = { "Verdana", int(12.f * flDPI), iFlags, 0 };
	m_aFonts[FONT_INDICATORS] = { "Verdana", int(13.f * flDPI), iFlags, 0 };

	for (auto& fFont : m_aFonts)
	{
		if (fFont.m_dwFont = I::MatSystemSurface->CreateFont())
			I::MatSystemSurface->SetFontGlyphSet(fFont.m_dwFont, fFont.m_szName, fFont.m_nTall, fFont.m_nWeight, 0, 0, fFont.m_nFlags);
	}
}

const Font_t& CFonts::GetFont(EFonts eFont)
{
	return m_aFonts[eFont];
}