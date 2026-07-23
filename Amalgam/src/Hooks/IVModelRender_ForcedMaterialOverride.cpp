#include "../SDK/SDK.h"

#include "../Features/Visuals/Chams/Chams.h"
#include "../Features/Visuals/Glow/Glow.h"
#include "../Features/Visuals/Materials/Materials.h"
#include "../Features/Visuals/RearView/RearView.h"
#include "../Utils/Perf/Tracker.h"

MAKE_HOOK(IVModelRender_ForcedMaterialOverride, U::Memory.GetVirtual(I::ModelRender, 1), void,
	IVModelRender* rcx, IMaterial* mat, OverrideType_t type)
{
	DEBUG_RETURN(IVModelRender_ForcedMaterialOverride, rcx, mat, type);

	PROF_CHARGE(Perf::COUNTER_MATOVERRIDE);

	if (F::Chams.m_bRendering || F::Glow.m_bRendering || F::RearView.m_bRendering)
		return;

	CALL_ORIGINAL(rcx, mat, type);
}