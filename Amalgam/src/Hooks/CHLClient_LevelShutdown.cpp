#include "../SDK/SDK.h"

#include "../Features/EnginePrediction/EnginePrediction.h"
#include "../Features/Spectate/Spectate.h"
#include "../Features/Debug/AutoVprof/AutoVprof.h"

MAKE_HOOK(CHLClient_LevelShutdown, U::Memory.GetVirtual(I::Client, 7), void,
	void* rcx)
{
	DEBUG_RETURN(CHLClient_LevelShutdown, rcx);

	// Before anything else tears down: this is the one callback the engine
	// guarantees on every way out of a match (disconnect, map change, quit), so
	// it is where a capture in progress gets written.
	F::AutoVprof.LevelShutdown();

	H::Entities.Clear(true);
	F::EnginePrediction.Unload();
	F::Spectate.Reset();

	CALL_ORIGINAL(rcx);
}