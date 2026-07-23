#pragma once
#include <cstdint>
#include <cstring>

// Shared performance primitives for the per-frame / per-render-pass hot paths.
//
// The visual features all share the same shape of problem: a small set of
// entities is rebuilt every net update, consulted several times per frame (once
// per scene pass - main view, rearview flanks, the six FlexFOV cube faces), and
// probed once per engine model draw (200+ per frame). std::unordered_map is the
// wrong tool there: every probe hashes and chases a node pointer, and every
// per-pass rebuild frees and reallocates nodes. These containers trade a fixed
// slab of memory for allocation-free O(1) access with no hashing.
namespace Perf
{

// Flat entindex -> T map with epoch-stamped slots.
//
// Clear() is O(1) (it bumps the epoch, invalidating every slot at once) and
// never touches the heap, so a container that is cleared and refilled on every
// render pass costs nothing to reset. Lookup is a bounds check plus one stamp
// compare - no hash, no indirection - which is what makes it safe to probe from
// inside the DrawModelExecute hook, where it runs once per model draw and its
// cost lands in C_BaseAnimating::DrawModel's self time.
//
// N covers the engine's edict range; out-of-range indices (invalid entities)
// are silently treated as "not present" rather than asserting, matching how the
// map-based code behaved when handed a bogus index.
template <typename T, int N = 2048>
class CEntitySlots
{
	uint32_t m_uEpoch = 1;
	uint32_t m_aStamp[N] = {};
	T m_aValue[N] = {};
	int m_iCount = 0;

public:
	void Clear()
	{
		// Wrapping the epoch would make stale slots from the previous wrap look
		// live again; the wipe happens once every 4 billion clears.
		if (++m_uEpoch == 0)
		{
			std::memset(m_aStamp, 0, sizeof(m_aStamp));
			m_uEpoch = 1;
		}
		m_iCount = 0;
	}

	bool Contains(int iIndex) const
	{
		return uint32_t(iIndex) < uint32_t(N) && m_aStamp[iIndex] == m_uEpoch;
	}

	// Pointer to the stored value, or nullptr when the index isn't present.
	const T* Find(int iIndex) const
	{
		return Contains(iIndex) ? &m_aValue[iIndex] : nullptr;
	}

	void Add(int iIndex, const T& tValue = T())
	{
		if (uint32_t(iIndex) >= uint32_t(N))
			return;

		if (m_aStamp[iIndex] != m_uEpoch)
		{
			m_aStamp[iIndex] = m_uEpoch;
			m_iCount++;
		}
		m_aValue[iIndex] = tValue;
	}

	// Insert with a default value only if absent, leaving an existing entry's
	// value alone (the map idiom `m[i];`).
	void Touch(int iIndex)
	{
		if (uint32_t(iIndex) >= uint32_t(N) || m_aStamp[iIndex] == m_uEpoch)
			return;

		m_aStamp[iIndex] = m_uEpoch;
		m_aValue[iIndex] = T();
		m_iCount++;
	}

	void Erase(int iIndex)
	{
		if (uint32_t(iIndex) >= uint32_t(N) || m_aStamp[iIndex] != m_uEpoch)
			return;

		m_aStamp[iIndex] = 0;
		m_iCount--;
	}

	int Size() const { return m_iCount; }
	bool Empty() const { return !m_iCount; }
};

// A value recomputed at most once per rendered frame.
//
// Several hot paths need something moderately expensive but frame-constant (the
// local player's eye position, a config-derived flag) while being reached from
// code that runs per entity, per material layer or per model draw. Get() takes
// the producer and the current framecount and only re-runs the producer when
// the frame changed.
template <typename T>
class CFrameValue
{
	int m_iFrame = -1;
	T m_tValue = {};

public:
	template <typename F>
	const T& Get(int iFrame, F&& fnProduce)
	{
		if (m_iFrame != iFrame)
		{
			m_iFrame = iFrame;
			m_tValue = fnProduce();
		}
		return m_tValue;
	}

	// Force the next Get() to recompute (level change, config edit).
	void Invalidate() { m_iFrame = -1; }
};

}
