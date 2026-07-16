#pragma once
#include <cstdint>
#include <cstddef>
#include <string.h>

namespace FNV1A
{
	inline constexpr uint32_t uHash32 = 0x811C9DC5;
	inline constexpr uint32_t uPrime32 = 0x1000193;
	inline constexpr uint64_t uHash64 = 0xcbf29ce484222325;
	inline constexpr uint64_t uPrime64 = 0x100000001b3;

	// compile-time hashes
	constexpr uint32_t Hash32Const(const char* szString, const uint32_t uValue = uHash32) noexcept
	{
		return (szString[0] == '\0') ? uValue : Hash32Const(&szString[1], (uValue ^ uint32_t(szString[0])) * uPrime32);
	}
	constexpr uint64_t Hash64Const(const char* szString, const uint64_t uValue = uHash64) noexcept
	{
		return (szString[0] == '\0') ? uValue : Hash64Const(&szString[1], (uValue ^ uint64_t(szString[0])) * uPrime64);
	}

	// runtime hashes
	// The char is sign-extended on purpose, matching Hash32Const/Hash64Const - hashes
	// are compared against those and persisted in configs, so it must not change.
	inline uint32_t Hash32(const char* szString)
	{
		uint32_t uHashed = uHash32;

		for (; *szString; ++szString)
		{
			uHashed ^= uint32_t(*szString);
			uHashed *= uPrime32;
		}

		return uHashed;
	}
	inline uint64_t Hash64(const char* szString)
	{
		uint64_t uHashed = uHash64;

		for (; *szString; ++szString)
		{
			uHashed ^= uint64_t(*szString);
			uHashed *= uPrime64;
		}

		return uHashed;
	}
}