// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_TIME_PROCESSOR_CLOCK_HPP_INCLUDED
#define ASTERIA_KERNEL_TIME_PROCESSOR_CLOCK_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Time/ILocalTimer.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Kernel::Time
{
using Scheduler::MaximumProcessorCount;
inline constexpr Foundation::UInt64 ProcessorClockEpsilonMicroseconds{50U};
inline constexpr Foundation::UInt64 ProcessorClockMeasured{1U};
inline constexpr Foundation::UInt64 ProcessorClockAdjustable{2U};
inline constexpr Foundation::UInt64 ProcessorClockInvariant{4U};
inline constexpr Foundation::UInt64 ProcessorClockHypervisor{8U};
struct ProcessorClockResidual
{
    Foundation::Int64 Lower;
    Foundation::Int64 Upper;
    Foundation::UInt64 RoundTrip;
    Foundation::UInt64 Flags;
};
struct ProcessorClockEvidence
{
    ProcessorClockResidual Processors[MaximumProcessorCount];
    Foundation::ProcessorMask ExcludedMask;
    Foundation::UInt64 Frequency;
    Foundation::UInt64 EpsilonTicks;
    Foundation::UInt64 Published;
};
static_assert(sizeof(ProcessorClockResidual) == 32U);
static_assert(__builtin_offsetof(ProcessorClockResidual, Lower) == 0U);
static_assert(__builtin_offsetof(ProcessorClockResidual, Upper) == 8U);
static_assert(__builtin_offsetof(ProcessorClockResidual, RoundTrip) == 16U);
static_assert(__builtin_offsetof(ProcessorClockResidual, Flags) == 24U);
static_assert(sizeof(ProcessorClockEvidence) == 160U);
static_assert(__builtin_offsetof(ProcessorClockEvidence, Processors) == 0U);
static_assert(__builtin_offsetof(ProcessorClockEvidence, ExcludedMask) == 128U);
static_assert(__builtin_offsetof(ProcessorClockEvidence, Frequency) == 136U);
static_assert(__builtin_offsetof(ProcessorClockEvidence, EpsilonTicks) == 144U);
static_assert(__builtin_offsetof(ProcessorClockEvidence, Published) == 152U);
static_assert(sizeof(ProcessorClockEvidence) ==
              sizeof(ProcessorClockResidual) * Scheduler::MaximumProcessorCount + 4U * sizeof(Foundation::UInt64));

// Uncertainty alone never excludes a processor.
[[nodiscard]] constexpr bool ClockResidualExcluded(const ProcessorClockResidual residual,
                                                   const Foundation::UInt64 epsilon) noexcept
{
    return residual.Lower > static_cast<Foundation::Int64>(epsilon) ||
           residual.Upper < -static_cast<Foundation::Int64>(epsilon);
}
// Bare metal requires an invariant counter; a hypervisor supplies the counter, so its measured
// residual decides admission. Certain excessive skew excludes either kind.
[[nodiscard]] constexpr bool ClockPlacementExcluded(const ProcessorClockResidual residual,
                                                    const Foundation::UInt64 epsilon) noexcept
{
    return (residual.Flags & (ProcessorClockInvariant | ProcessorClockHypervisor)) == 0U ||
           ClockResidualExcluded(residual, epsilon);
}
[[nodiscard]] constexpr Foundation::UInt64 ClockEpsilonTicks(const Foundation::UInt64 frequency) noexcept
{
    return (frequency / 1'000'000U) * ProcessorClockEpsilonMicroseconds +
           ((frequency % 1'000'000U) * ProcessorClockEpsilonMicroseconds + 999'999U) / 1'000'000U;
}
[[nodiscard]] constexpr Foundation::Int64 ClockDifference(const Foundation::UInt64 a,
                                                          const Foundation::UInt64 b) noexcept
{
    constexpr Foundation::UInt64 maximum{0x7FFF'FFFF'FFFF'FFFFULL};
    const auto magnitude = a >= b ? a - b : b - a;
    const auto bounded = static_cast<Foundation::Int64>(magnitude <= maximum ? magnitude : maximum);
    return a >= b ? bounded : -bounded;
}
[[nodiscard]] constexpr ProcessorClockResidual ClockResidual(const Foundation::UInt64 before,
                                                             const Foundation::UInt64 reference,
                                                             const Foundation::UInt64 after) noexcept
{
    return {ClockDifference(before, reference), ClockDifference(after, reference), after - before, 1U};
}

// Shared release/acquire ping-pong. Each AP owns its request; CPU 0 responds.
class ProcessorClockRendezvous final
{
public:
    [[nodiscard]] Foundation::UInt64 Request(const Foundation::Size cpu) noexcept
    {
        const auto sequence = m_slots[cpu].Request.LoadAcquire() + 1U;
        m_slots[cpu].Request.StoreRelease(sequence);
        return sequence;
    }
    [[nodiscard]] bool Pending(const Foundation::Size cpu) const noexcept
    {
        return m_slots[cpu].Request.LoadAcquire() != m_slots[cpu].Completed.LoadAcquire();
    }
    void Respond(const Foundation::Size cpu, const Foundation::UInt64 now) noexcept
    {
        auto& slot = m_slots[cpu];
        const auto request = slot.Request.LoadAcquire();
        if (request == slot.Completed.LoadAcquire())
        {
            return;
        }
        slot.Reference = now;
        slot.Completed.StoreRelease(request);
    }
    [[nodiscard]] bool Completed(const Foundation::Size cpu, const Foundation::UInt64 sequence) const noexcept
    {
        return m_slots[cpu].Completed.LoadAcquire() == sequence;
    }
    [[nodiscard]] Foundation::UInt64 Reference(const Foundation::Size cpu) const noexcept
    {
        return m_slots[cpu].Reference;
    }

private:
    struct Slot
    {
        Platform::Synchronization::AtomicUInt64 Request{};
        Platform::Synchronization::AtomicUInt64 Completed{};
        Foundation::UInt64 Reference{0U};
    };
    Slot m_slots[MaximumProcessorCount]{};
};

// The native online path and host order test use this sequence.
// Itanium SDM Vol. 2 rev. 1.1 §3.3.4.2: the write adapter performs mov ar.itc followed by srlz.d,
// before any arm.
template <typename Timer, typename Write>
[[nodiscard]] bool InitializeOnlineCounter(Timer& timer, Write write, const Foundation::UInt64 value) noexcept
{
    if (timer.CancelDeadline() != Hal::Time::LocalTimerError::None)
    {
        return false;
    }
    write(value);
    return true;
}
} // Namespace Asteria::Kernel::Time
#endif
