// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_RUNTIME_MEMORY_HPP_INCLUDED
#define ASTERIA_RUNTIME_MEMORY_HPP_INCLUDED

#include "Foundation/Types.hpp"

extern "C"
{
/// These routines allocate nothing and provide no synchronization. Each pointer must designate
/// countBytes accessible bytes when countBytes is nonzero. Work is linear in countBytes.

/// Copies non-overlapping ranges and returns destination.
void* memcpy(void* destination, const void* source, Asteria::Foundation::Size countBytes) noexcept;

/// Copies ranges that may overlap and returns destination.
void* memmove(void* destination, const void* source, Asteria::Foundation::Size countBytes) noexcept;

/// Fills destination's writable range with the low eight bits of value and returns destination.
void* memset(void* destination, int value, Asteria::Foundation::Size countBytes) noexcept;

/// Compares readable ranges lexicographically in unsigned byte order; returns negative, zero,
/// or positive for first less than, equal to, or greater than second.
int memcmp(const void* first, const void* second, Asteria::Foundation::Size countBytes) noexcept;
}

#endif // ASTERIA_RUNTIME_MEMORY_HPP_INCLUDED
