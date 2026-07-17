#pragma once
#include "../../../SDK/SDK.h"

// On-screen bar visualizing current crit availability for the active weapon:
// one filled cell per available crit (out of the potential total), followed by
// a single progress segment showing charge toward the next crit. Fully
// player-customizable dimensions/colors via Vars::Visuals::CritBar. Sources its
// state from F::CritHack (GetAvailableCrits/GetPotentialCrits/GetCritProgress).
class CCritBar
{
public:
	void Draw(CTFPlayer* pLocal);
};

ADD_FEATURE(CCritBar, CritBar);
