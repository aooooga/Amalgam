#include "../SDK/SDK.h"

#include "../Features/Visuals/ESP/ESP.h"
#include "../Features/Visuals/OffscreenArrows/OffscreenArrows.h"
#include "../Features/Visuals/CameraWindow/CameraWindow.h"
#include "../Features/Visuals/FlexFOV/FlexFOV.h"
#include "../Features/Visuals/RearView/RearView.h"
#include "../Features/Visuals/Visuals.h"
#include "../Features/Ticks/Ticks.h"
#include "../Features/CritHack/CritHack.h"
#include "../Features/Visuals/CritBar/CritBar.h"
#include "../Features/Visuals/Alerts/Alerts.h"
#include "../Features/Visuals/SpectatorList/SpectatorList.h"
#include "../Features/Backtrack/Backtrack.h"
#include "../Features/Visuals/PlayerConditions/PlayerConditions.h"
#include "../Features/NoSpread/NoSpreadHitscan/NoSpreadHitscan.h"
#include "../Features/Aimbot/Aimbot.h"
#include "../Features/PacketManip/AntiAim/AntiAim.h"
#include "../Features/Aimbot/AutoHeal/AutoHeal.h"
#include "../Features/Debug/Debug.h"
#include "../Features/Debug/AutoVprof/AutoVprof.h"
#include "../Features/Debug/ProcessTracker/TrackerOverlay.h"
#include "../Utils/Perf/Tracker.h"

MAKE_HOOK(IEngineVGui_Paint, U::Memory.GetVirtual(I::EngineVGui, 14), void,
	void* rcx, int iMode)
{
	DEBUG_RETURN(IEngineVGui_Paint, rcx, iMode);

	if (G::Unload)
		return CALL_ORIGINAL(rcx, iMode);

	if (iMode & PAINT_UIPANELS)
	{
		H::Draw.UpdateKeyStrings();
		// We're at the menu: if a capture is still open, nothing told us the match
		// ended. Write it rather than lose it.
		F::AutoVprof.FlushOrphaned();
	}
	else if (iMode & PAINT_INGAMEPANELS && !SDK::CleanScreenshot())
	{
		F::AutoVprof.Run();

		// The 2D overlay pass. Its cost lands inside the engine's HUD paint node
		// in vprof, so without these zones it is invisible in a report.
		PROF_ZONE("Paint (all)", Perf::GROUP_PAINT);

		H::Draw.UpdateScreenSize();
		H::Draw.UpdateW2SMatrix();

		H::Draw.Start(true);
		if (auto pLocal = H::Entities.GetLocal())
		{
			// Composite first: it repaints the whole screen, so everything that
			// should stay visible (camera window, debug tiles, ESP below) must
			// draw after it.
			PROF_CALL("FlexFOV::DrawComposite", Perf::GROUP_PAINT, F::FlexFOV.DrawComposite());
			PROF_CALL("FlexFOV::DrawViewmodel", Perf::GROUP_PAINT, F::FlexFOV.DrawViewmodel());
			PROF_CALL("FlexFOV::DrawDebug", Perf::GROUP_PAINT, F::FlexFOV.DrawDebug());
			// After the composite (it repaints every pixel) so the rear-view enemy
			// overlay stays visible with or without FlexFOV.
			PROF_CALL("RearView::DrawOverlay", Perf::GROUP_PAINT, F::RearView.DrawOverlay());
			PROF_CALL("CameraWindow::Draw", Perf::GROUP_PAINT, F::CameraWindow.Draw());

			PROF_CALL("AntiAim::Draw", Perf::GROUP_PAINT, F::AntiAim.Draw(pLocal));
			PROF_CALL("Visuals::PickupTimers", Perf::GROUP_PAINT, F::Visuals.DrawPickupTimers());
			PROF_CALL("ESP::Draw", Perf::GROUP_PAINT, F::ESP.Draw());
			PROF_CALL("OffscreenArrows::Draw", Perf::GROUP_PAINT, F::OffscreenArrows.Draw(pLocal));
			PROF_CALL("Aimbot::Draw", Perf::GROUP_PAINT, F::Aimbot.Draw(pLocal));

#ifdef DEBUG_VACCINATOR
			F::AutoHeal.Draw(pLocal);
#endif
			PROF_CALL("NoSpreadHitscan::Draw", Perf::GROUP_PAINT, F::NoSpreadHitscan.Draw(pLocal));
			PROF_CALL("PlayerConditions::Draw", Perf::GROUP_PAINT, F::PlayerConditions.Draw(pLocal));
			PROF_CALL("Backtrack::Draw", Perf::GROUP_PAINT, F::Backtrack.Draw(pLocal));
			PROF_CALL("SpectatorList::Draw", Perf::GROUP_PAINT, F::SpectatorList.Draw(pLocal));
			PROF_CALL("CritHack::Draw", Perf::GROUP_PAINT, F::CritHack.Draw(pLocal));
			PROF_CALL("CritBar::Draw", Perf::GROUP_PAINT, F::CritBar.Draw(pLocal));
			PROF_CALL("Alerts::Draw", Perf::GROUP_PAINT, F::Alerts.Draw(pLocal));
			PROF_CALL("Ticks::Draw", Perf::GROUP_PAINT, F::Ticks.Draw(pLocal));

#ifdef DEBUG_INFO
			F::Debug.Draw(pLocal);
#endif
			F::TrackerOverlay.Draw();
		}
		H::Draw.End();
	}

	CALL_ORIGINAL(rcx, iMode);

}