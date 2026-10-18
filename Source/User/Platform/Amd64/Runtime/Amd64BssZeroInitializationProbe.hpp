// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_PLATFORM_AMD64_RUNTIME_AMD64_BSS_ZERO_INITIALIZATION_PROBE_HPP_INCLUDED
#define ASTERIA_USER_PLATFORM_AMD64_RUNTIME_AMD64_BSS_ZERO_INITIALIZATION_PROBE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Platform::Amd64::Runtime
{
inline constexpr Foundation::UInt64 BssZeroInitializationPassedSentinel{0x4253'5350'4153'5301U};
inline constexpr Foundation::UInt64 BssZeroInitializationFailedSentinel{0x4253'5346'4149'4C01U};

// Volatile makes the loader zero-fill diagnostic observable; it is not a
// synchronization primitive or an MMIO object.
extern volatile Foundation::UInt64 g_bssZeroInitializationProbe;

/// Reads the exact image BSS probe once and replaces it with the corresponding
/// pass or fail sentinel. This helper allocates no memory and never blocks.
[[gnu::noinline]] void ObserveBssZeroInitialization() noexcept;
} // namespace Asteria::User::Platform::Amd64::Runtime

#endif // ASTERIA_USER_PLATFORM_AMD64_RUNTIME_AMD64_BSS_ZERO_INITIALIZATION_PROBE_HPP_INCLUDED
