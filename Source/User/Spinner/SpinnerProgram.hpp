// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SPINNER_SPINNER_PROGRAM_HPP_INCLUDED
#define ASTERIA_USER_SPINNER_SPINNER_PROGRAM_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Spinner
{
/// CPU-bound iteration count used to exercise preemption during guest tests.
inline constexpr Foundation::UInt64 SpinnerIterations{1ULL << 31U};

/// The line the spinner writes once it has counted; a TAP comment, so it never
/// breaks the guest test program's stream.
inline constexpr char SpinnerDoneLine[] = "# spinner: done\n";

/// The line the spinner writes instead when the check of its extended state after counting fails.
inline constexpr char SpinnerStateLostLine[] = "# spinner: extended state lost\n";

/// Whether the registers an ISA's entry loaded before RunSpinner still hold
/// their values; RunSpinner itself never touches them.
using SpinnerStateCheck = bool (*)() noexcept;

/// The address the spinner reads after its line: in page zero, which no user root maps, outside the
/// boot pager's lazy region, so the pager, its supervisor, ends the spinner.
inline constexpr Foundation::UInt64 SpinnerFaultAddress{8U};

/// The test boot component that is CPU-bound for a fixed time: counts
/// `iterations` without entering the kernel, writes SpinnerDoneLine through
/// its DiagnosticChannel (SpinnerStateLostLine if `stateCheck`, when given,
/// fails), then reads SpinnerFaultAddress, a fault its supervisor terminates.
/// Should that read ever complete, it waits on its WaitSet, which holds no
/// registration, so it blocks for good; it returns only if that wait fails.
void RunSpinner(System::SystemCallTransport transport, System::CapabilityHandle diagnostic,
                System::CapabilityHandle waitSet, Foundation::UInt64 iterations,
                SpinnerStateCheck stateCheck = nullptr) noexcept;
} // Namespace Asteria::User::Spinner

#endif // ASTERIA_USER_SPINNER_SPINNER_PROGRAM_HPP_INCLUDED
