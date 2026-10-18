// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_USER_PROCESSOR_EVIDENCE_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_USER_PROCESSOR_EVIDENCE_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Kernel/Scheduler/ReadyQueue.hpp"

namespace Asteria::Kernel::Diagnostics
{
using Scheduler::MaximumProcessorCount;
using Scheduler::MaximumReadyQueueEntries;
// Observations belong to the executing processor; the home is published before its bind request can
// be served.
struct UserProcessorObservation
{
    Foundation::UInt64 Dispatches;
    Foundation::UInt64 Ticks;
    Foundation::UInt64 RunStops;
    Foundation::UInt64 CreatedDispatches;
    Foundation::UInt64 CreatedTicks;
    Foundation::UInt64 Faults;
    Foundation::UInt64 Resolved;
    Foundation::UInt64 IdleRoot;
    Foundation::UInt64 IdleScrubVisits;
    Foundation::UInt64 RequestsServed;
};
struct UserThreadProcessorObservation
{
    Foundation::UInt64 ThreadIdentity;
    Foundation::UInt64 DomainIdentity;
    Foundation::UInt64 Home;
    Foundation::UInt64 SupervisionTag;
    Foundation::UInt64 RanMask;
    Foundation::UInt64 Dispatches;
    Foundation::UInt64 Ticks;
    Foundation::UInt64 Faults;
};
struct UserProcessorEvidence
{
    Foundation::UInt64 Published;
    Foundation::ProcessorMask BootRanMask;
    Foundation::ProcessorMask SelfTestRanMask;
    Foundation::ProcessorMask CreatedRanMask;
    UserProcessorObservation Processors[MaximumProcessorCount];
    UserThreadProcessorObservation Threads[MaximumReadyQueueEntries];
};
static_assert(sizeof(UserProcessorObservation) == 80U);
static_assert(__builtin_offsetof(UserProcessorObservation, Dispatches) == 0U);
static_assert(__builtin_offsetof(UserProcessorObservation, Ticks) == 8U);
static_assert(__builtin_offsetof(UserProcessorObservation, RunStops) == 16U);
static_assert(__builtin_offsetof(UserProcessorObservation, CreatedDispatches) == 24U);
static_assert(__builtin_offsetof(UserProcessorObservation, CreatedTicks) == 32U);
static_assert(__builtin_offsetof(UserProcessorObservation, Faults) == 40U);
static_assert(__builtin_offsetof(UserProcessorObservation, Resolved) == 48U);
static_assert(__builtin_offsetof(UserProcessorObservation, IdleRoot) == 56U);
static_assert(__builtin_offsetof(UserProcessorObservation, IdleScrubVisits) == 64U);
static_assert(__builtin_offsetof(UserProcessorObservation, RequestsServed) == 72U);
static_assert(sizeof(UserThreadProcessorObservation) == 64U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, ThreadIdentity) == 0U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, DomainIdentity) == 8U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, Home) == 16U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, SupervisionTag) == 24U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, RanMask) == 32U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, Dispatches) == 40U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, Ticks) == 48U);
static_assert(__builtin_offsetof(UserThreadProcessorObservation, Faults) == 56U);
static_assert(sizeof(UserProcessorEvidence) == 2400U);
static_assert(__builtin_offsetof(UserProcessorEvidence, Published) == 0U);
static_assert(__builtin_offsetof(UserProcessorEvidence, BootRanMask) == 8U);
static_assert(__builtin_offsetof(UserProcessorEvidence, SelfTestRanMask) == 16U);
static_assert(__builtin_offsetof(UserProcessorEvidence, CreatedRanMask) == 24U);
static_assert(__builtin_offsetof(UserProcessorEvidence, Processors) == 32U);
static_assert(__builtin_offsetof(UserProcessorEvidence, Threads) == 352U);

inline void CountUserProcessor(Foundation::UInt64& counter) noexcept
{
    __atomic_fetch_add(&counter, 1U, __ATOMIC_RELAXED);
}
inline void RecordUserProcessor(UserProcessorEvidence& evidence, const Foundation::Size cpu,
                                const Foundation::Size slot, const bool selfTest, const bool tick) noexcept
{
    if (cpu >= Scheduler::MaximumProcessorCount)
    {
        return;
    }
    auto& processor = evidence.Processors[cpu];
    CountUserProcessor(tick ? processor.Ticks : processor.Dispatches);
    const Foundation::ProcessorMask mask{Foundation::ProcessorMask{1U} << cpu};
    if (slot < Scheduler::MaximumReadyQueueEntries)
    {
        auto& thread = evidence.Threads[slot];
        __atomic_fetch_or(&thread.RanMask, mask, __ATOMIC_RELAXED);
        CountUserProcessor(tick ? thread.Ticks : thread.Dispatches);
        CountUserProcessor(tick ? processor.CreatedTicks : processor.CreatedDispatches);
        __atomic_fetch_or(selfTest ? &evidence.SelfTestRanMask : &evidence.CreatedRanMask, mask, __ATOMIC_RELAXED);
    }
    else
    {
        __atomic_fetch_or(&evidence.BootRanMask, mask, __ATOMIC_RELAXED);
    }
    __atomic_fetch_or(&evidence.Published, mask, __ATOMIC_RELEASE);
}
} // Namespace Asteria::Kernel::Diagnostics
#endif
