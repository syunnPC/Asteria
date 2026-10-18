// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_USER_SCHEDULING_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_USER_SCHEDULING_HPP_INCLUDED

#include "Kernel/Scheduler/SchedulingContext.hpp"

namespace Asteria::Platform::Amd64::System
{
/// The scheduling configuration of every AMD64 boot user thread: a time slice whose budget is its
/// whole period, below the wait fixture's timer delay. A CPU-bound thread is preempted at its run
/// stop and, since its budget is replenished at that boundary, queued behind every other Ready
/// thread; a lone thread is never left idle. The period is in TSC ticks, long enough that an
/// emulated processor (QEMU TCG, whose TSC runs at host speed) still enters and runs a thread
/// within it.
inline constexpr Foundation::UInt64 Amd64UserSchedulingPeriodTicks{40'000'000U};

[[nodiscard]] constexpr Kernel::Scheduler::SchedulingContextConfiguration
MakeAmd64UserSchedulingConfiguration() noexcept
{
    return Kernel::Scheduler::SchedulingContextConfiguration{Amd64UserSchedulingPeriodTicks,
                                                             Amd64UserSchedulingPeriodTicks,
                                                             Amd64UserSchedulingPeriodTicks,
                                                             1U,
                                                             0xFFU,
                                                             Kernel::Scheduler::SchedulingCriticality::Emergency,
                                                             1U,
                                                             Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
                                                             0U};
}
} // Namespace Asteria::Platform::Amd64::System

#endif // ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_USER_SCHEDULING_HPP_INCLUDED
