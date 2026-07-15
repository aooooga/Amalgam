#include "../SDK/SDK.h"

#include <bit>
#include <unordered_map>

MAKE_SIGNATURE(CAttributeManager_AttribHookInt, "client.dll", "4C 8B DC 49 89 5B ? 49 89 6B ? 49 89 73 ? 57 41 54 41 55 41 56 41 57 48 83 EC ? 48 8B 3D ? ? ? ? 4C 8D 35", 0x0);
MAKE_SIGNATURE(CAttributeManager_AttribHookFloat, "client.dll", "4C 8B DC 49 89 5B ? 49 89 6B ? 56 57 41 54 41 56 41 57 48 83 EC ? 48 8B 3D ? ? ? ? 4C 8D 35", 0x0);
MAKE_SIGNATURE(CTFPlayer_FireEvent_AttribHookValue_Call, "client.dll", "8B F8 83 BE", 0x0);

static inline int ColorToInt(Color_t col)
{
    return col.r << 16 | col.g << 8 | col.b;
}

// Per-frame memoization of AttribHookValue (see the AttributeCacheOptimization
// var). The result is a pure function of (entity's item attributes, attribute
// name, input value); attributes only change on item swaps between frames, so
// caching for one frame is transparent. The input value is part of the key
// because it participates in the result (the hook multiplies/offsets it).
// Skipped when the caller wants the contributing-item list back (buffer) -
// that out-param can't be replayed from a cache.
struct AttribCacheKey
{
	const void* m_pEnt;
	uint64_t m_uName;
	uint32_t m_uValue;
	bool operator==(const AttribCacheKey&) const = default;
};
struct AttribCacheHash
{
	size_t operator()(const AttribCacheKey& tKey) const
	{
		uint64_t uHash = uint64_t(uintptr_t(tKey.m_pEnt)) * 0x9E3779B97F4A7C15ull;
		uHash ^= tKey.m_uName + 0x9E3779B97F4A7C15ull + (uHash << 6) + (uHash >> 2);
		uHash ^= uint64_t(tKey.m_uValue) + 0x9E3779B97F4A7C15ull + (uHash << 6) + (uHash >> 2);
		return size_t(uHash);
	}
};

template <typename T>
class CAttribCache
{
	std::unordered_map<AttribCacheKey, T, AttribCacheHash> m_mCache;
	int m_iFrame = -1;

public:
	// True when caching applies to this call at all (toggle on, no out-param,
	// real entity, frame source available).
	static bool Usable(const void* pEconEnt, const void* pBuffer)
	{
		return Vars::Misc::Game::AttributeCacheOptimization.Value
			&& !pBuffer && pEconEnt && I::GlobalVars;
	}

	// Global-const names are pointer-stable, so the pointer is the identity;
	// otherwise hash the characters. The two key spaces can't collide: code
	// addresses/pointers don't fit in 32 bits.
	static uint64_t NameId(const char* szName, bool bGlobalConstString)
	{
		return bGlobalConstString ? uint64_t(uintptr_t(szName)) : uint64_t(FNV1A::Hash32(szName));
	}

	bool Find(const AttribCacheKey& tKey, T& tOut)
	{
		if (m_iFrame != I::GlobalVars->framecount)
		{
			m_mCache.clear();
			m_iFrame = I::GlobalVars->framecount;
			return false;
		}
		const auto it = m_mCache.find(tKey);
		if (it == m_mCache.end())
			return false;
		tOut = it->second;
		return true;
	}

	void Store(const AttribCacheKey& tKey, T tValue)
	{
		if (m_mCache.size() < 8192) // pathological-frame guard
			m_mCache[tKey] = tValue;
	}
};

static CAttribCache<int> s_tIntCache;
static CAttribCache<float> s_tFloatCache;

MAKE_HOOK(CAttributeManager_AttribHookInt, S::CAttributeManager_AttribHookInt(), int,
	int value, const char* name, void* econent, void* buffer, bool isGlobalConstString)
{
	DEBUG_RETURN(CAttributeManager_AttribHookInt, value, name, econent, buffer, isGlobalConstString);

	const auto dwRetAddr = uintptr_t(_ReturnAddress());
	const auto dwDesired = S::CTFPlayer_FireEvent_AttribHookValue_Call();

	if (dwRetAddr == dwDesired && Vars::Visuals::Effects::SpellFootsteps.Value
		&& econent == H::Entities.GetLocal() && FNV1A::Hash32(name) == FNV1A::Hash32Const("halloween_footstep_type"))
	{
		switch (Vars::Visuals::Effects::SpellFootsteps.Value)
		{
		case Vars::Visuals::Effects::SpellFootstepsEnum::Color: return ColorToInt(Vars::Colors::SpellFootstep.Value);
		case Vars::Visuals::Effects::SpellFootstepsEnum::Team: return 1;
		case Vars::Visuals::Effects::SpellFootstepsEnum::Halloween: return 2;
		}
	}

	if (!CAttribCache<int>::Usable(econent, buffer))
		return CALL_ORIGINAL(value, name, econent, buffer, isGlobalConstString);

	const AttribCacheKey tKey = { econent, CAttribCache<int>::NameId(name, isGlobalConstString), uint32_t(value) };
	int iCached;
	if (s_tIntCache.Find(tKey, iCached))
		return iCached;

	const int iResult = CALL_ORIGINAL(value, name, econent, buffer, isGlobalConstString);
	s_tIntCache.Store(tKey, iResult);
	return iResult;
}

MAKE_HOOK(CAttributeManager_AttribHookFloat, S::CAttributeManager_AttribHookFloat(), float,
	float value, const char* name, void* econent, void* buffer, bool isGlobalConstString)
{
	DEBUG_RETURN(CAttributeManager_AttribHookFloat, value, name, econent, buffer, isGlobalConstString);

	if (!CAttribCache<float>::Usable(econent, buffer))
		return CALL_ORIGINAL(value, name, econent, buffer, isGlobalConstString);

	const AttribCacheKey tKey = { econent, CAttribCache<float>::NameId(name, isGlobalConstString), std::bit_cast<uint32_t>(value) };
	float flCached;
	if (s_tFloatCache.Find(tKey, flCached))
		return flCached;

	const float flResult = CALL_ORIGINAL(value, name, econent, buffer, isGlobalConstString);
	s_tFloatCache.Store(tKey, flResult);
	return flResult;
}
