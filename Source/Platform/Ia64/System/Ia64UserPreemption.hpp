// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SYSTEM_IA64_USER_PREEMPTION_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SYSTEM_IA64_USER_PREEMPTION_HPP_INCLUDED

#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"

namespace Asteria::Platform::Ia64::System
{
/// The time slice of every IA-64 boot user thread: a budget equal to its period, so a CPU-bound
/// thread is preempted at its run stop, replenished at that boundary and queued behind every other
/// Ready thread, while a lone thread continues. The period is in ITC ticks, as AMD64's is in TSC
/// ticks: 50 ms at the 400-MHz ITC of QEMU's IA-64 models.
inline constexpr Foundation::UInt64 Ia64UserSchedulingPeriodTicks{20'000'000U};

[[nodiscard]] constexpr Kernel::Scheduler::SchedulingContextConfiguration
MakeIa64UserSchedulingConfiguration(const Foundation::ProcessorMask affinityMask) noexcept
{
    return Kernel::Scheduler::SchedulingContextConfiguration{
        .BudgetTicks = Ia64UserSchedulingPeriodTicks,
        .PeriodTicks = Ia64UserSchedulingPeriodTicks,
        .RelativeDeadlineTicks = Ia64UserSchedulingPeriodTicks,
        .Weight = 1U,
        .Priority = 0xFFU,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Emergency,
        .CpuAffinityMask = affinityMask,
        .Class = Kernel::Scheduler::SchedulingClass::EmergencyRecovery,
        .Reserved = 0U,
    };
}

/// The BSP runtime's side of a timer tick a boot thread took at PL3: the scheduling point, run on
/// that thread's entry storage after AsteriaIa64UserInterruptClaim masked the timer. PreemptUser
/// ends the run Ready if its deadline fired and returns once the thread runs again, with the
/// domain's entry contexts selected; false on a kernel invariant failure, which it reports.
class IIa64UserPreemption
{
public:
    [[nodiscard]] virtual bool PreemptUser(Memory::Ia64BspRuntimeUserDomain domain) noexcept = 0;

    /// The same scheduling point for the thread of created-thread slot `slot`.
    [[nodiscard]] virtual bool PreemptThread(Foundation::Size slot) noexcept = 0;

    /// A boot thread's fault deferred behind another: ends its run Ready and returns once it runs
    /// again, with the domain's entry contexts selected; false on a kernel invariant failure.
    [[nodiscard]] virtual bool YieldUser(Memory::Ia64BspRuntimeUserDomain domain) noexcept = 0;

protected:
    constexpr IIa64UserPreemption() noexcept = default;
    ~IIa64UserPreemption() noexcept = default;
};
} // Namespace Asteria::Platform::Ia64::System

#endif // ASTERIA_PLATFORM_IA64_SYSTEM_IA64_USER_PREEMPTION_HPP_INCLUDED
