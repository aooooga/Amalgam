#pragma once
#include "../../../SDK/SDK.h"

// Stacked on-screen text warnings for configurable danger conditions - multiple
// can be active and drawn at once, each on its own line below a shared
// draggable anchor. Owns a small ISurface font created/rebuilt directly (not
// through the shared CFonts enum) so family/size/weight are user-editable per
// Vars::Visuals::Alerts settings.
class CAlerts
{
	unsigned long m_dwFont = 0;
	std::string m_sFontName = {};
	int m_iFontSize = 0;
	bool m_bFontBold = false;

	void EnsureFont();
	bool SniperSightline(CTFPlayer* pLocal);
	bool EnemyNear(CTFPlayer* pLocal);

public:
	void Draw(CTFPlayer* pLocal);
};

ADD_FEATURE(CAlerts, Alerts);
