// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_GUEST_GUEST_TESTS_HPP_INCLUDED
#define ASTERIA_TESTS_GUEST_GUEST_TESTS_HPP_INCLUDED

// The guest test program: the initial user image of the test firmware. It
// checks, through the system-call ABI only, what a user program can observe
// of the kernel, and reports TAP 14 through its DiagnosticChannel. The
// kernel prints no verdict for any of these behaviours.

#include "Guest/Tap.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::Guest
{
/// Capabilities the kernel hands to the initial user image.
struct Bootstrap
{
    User::System::CapabilityHandle Endpoint;   // self-receiving loopback Endpoint
    User::System::CapabilityHandle WaitSet;    // absent (zero) where the ISA grants none yet
    User::System::CapabilityHandle Diagnostic; // Write-only DiagnosticChannel
    bool HasWaitSet;
};

/// ISA-specific checks of user-visible processor state, run after the
/// ISA-neutral ones.
using PlatformChecks = void (*)(Tap& tap) noexcept;

/// The zero-initialized storage the `bss.zero` point reads before anything
/// writes it. It has external linkage so that Tools/Verify can prove it lies
/// in the loader-zeroed tail of the image, not in file-backed data.
extern volatile Foundation::UInt64 g_bssProbe[4];

/// Runs every check and prints the TAP stream; returns the number of
/// failed checks and lost lines (zero on success). Runs once per image: the
/// writer's state is static.
[[nodiscard]] Foundation::UInt64 RunGuestTests(const Bootstrap& bootstrap, User::System::SystemCallTransport transport,
                                               PlatformChecks platformChecks) noexcept;
} // namespace Asteria::Guest

#endif // ASTERIA_TESTS_GUEST_GUEST_TESTS_HPP_INCLUDED
