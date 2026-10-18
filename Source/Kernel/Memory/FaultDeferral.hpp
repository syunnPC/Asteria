// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_DEFERRAL_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_DEFERRAL_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Memory
{
enum class FaultDelivery : Foundation::UInt8
{
    /// A resolution woke the thread, which runs again.
    Resolved,
    /// The supervisor holds another fault; nothing changed.
    Busy,
    /// A kernel invariant failed.
    Failed,
};

/// What a supervised fault's trap does once the supervisor answered.
enum class FaultOutcome : Foundation::UInt8
{
    /// Return to the faulting instruction, which now succeeds.
    Resume,
    /// Yield the processor; the thread faults again when it next runs.
    Yield,
    /// Stop the machine.
    Stop,
};

/// The outcome of a delivery, counting the thread's consecutive deferrals in
/// deferrals: a resolution resumes and clears them, a busy supervisor always
/// yields, and only a failure stops.
/// A fault that finds its supervisor busy yields and faults again when its thread next runs, on
/// every ISA, however often. The supervisor is user code, and how long it holds another fault is
/// its own affair: it may wait on a timer, run out of budget until its next period or serve other
/// faults first. The thread's trips through the ready queue measure none of that; when the
/// supervisor cannot be admitted, the yield selects the faulting thread again at once. The count is
/// evidence only. It saturates instead of wrapping, which no realistic run reaches.
[[nodiscard]] constexpr FaultOutcome DecideFaultOutcome(const FaultDelivery delivery,
                                                        Foundation::UInt64& deferrals) noexcept
{
    if (delivery == FaultDelivery::Resolved)
    {
        deferrals = 0U;
        return FaultOutcome::Resume;
    }
    if (delivery == FaultDelivery::Busy)
    {
        if (deferrals != ~Foundation::UInt64{0U})
        {
            ++deferrals;
        }
        return FaultOutcome::Yield;
    }
    return FaultOutcome::Stop;
}
} // Namespace Asteria::Kernel::Memory

#endif // ASTERIA_KERNEL_MEMORY_FAULT_DEFERRAL_HPP_INCLUDED
