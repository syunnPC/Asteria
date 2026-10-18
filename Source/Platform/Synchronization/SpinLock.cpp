// SPDX-License-Identifier: Apache-2.0

#include "SpinLock.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Kernel/Scheduler/CrossProcessorWaitBudget.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"

namespace Asteria::Platform::Synchronization
{
static_assert(__atomic_always_lock_free(sizeof(Foundation::UInt64), nullptr));

namespace
{
/// Tells the processor that this is a spin-wait loop, so a waiting hardware
/// thread yields execution resources to its sibling and leaves the loop
/// without a memory-order mis-speculation when the lock is released.
inline void SpinWaitHint() noexcept
{
#if defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__ia64__)
    __asm__ __volatile__("hint @pause" ::: "memory");
#endif
}
} // Namespace

void SpinLock::Acquire() noexcept
{
    const auto processor = CurrentProcessor();
    const Foundation::UInt64 owner = processor.IsValid() ? processor.GetValue() + 1U : ~Foundation::UInt64{0U};
    Foundation::UInt64 expected{0U};
    Kernel::Scheduler::CrossProcessorWaitBudget budget{
        ProcessorWaitTicks(), Kernel::Scheduler::CrossProcessorMinimumPolls, ProcessorWaitMinimumTicks()};
    while (!__atomic_compare_exchange_n(&m_owner, &expected, owner, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        while (__atomic_load_n(&m_owner, __ATOMIC_RELAXED) != 0U)
        {
            SpinWaitHint();
            if (!budget.TryPoll(ProcessorWaitTicks()))
            {
                const auto heldBy = GetOwner();
                const auto* set = CurrentProcessorSet();
                if (heldBy != 0U && heldBy <= Kernel::Scheduler::MaximumProcessorCount && set != nullptr &&
                    (set->OnlineMask() & (Foundation::ProcessorMask{1U} << (heldBy - 1U))) == 0U)
                {
                    StopProcessorWait(Kernel::Diagnostics::StopCode::LockOwnerLost, heldBy - 1U);
                }
                // Unknown owners are possible during firmware preparation. Once identity is
                // authoritative, a critical section that exceeds both bounds is an invariant
                // failure.
                if (processor.IsValid())
                {
                    StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, heldBy);
                }
            }
        }
        expected = 0U;
    }
    EnterProcessorLock();
}

void SpinLock::Release() noexcept
{
    // Release publishes the complete critical section before another CPU acquires the owner word.
    LeaveProcessorLock();
    __atomic_store_n(&m_owner, 0U, __ATOMIC_RELEASE);
}

SpinLockGuard::SpinLockGuard(SpinLock& lock) noexcept : m_lock{lock}
{
    m_lock.Acquire();
}

SpinLockGuard::~SpinLockGuard() noexcept
{
    m_lock.Release();
}
} // Namespace Asteria::Platform::Synchronization
