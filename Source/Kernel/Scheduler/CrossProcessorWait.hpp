// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_CROSS_PROCESSOR_WAIT_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_CROSS_PROCESSOR_WAIT_HPP_INCLUDED
#include "Kernel/Scheduler/CrossProcessorWaitBudget.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

namespace Asteria::Kernel::Scheduler
{
// All cross-CPU polling enters here without a lock. Platform progress drains this CPU, polls its
// stop latch and serves posted requests except a terminate of its currently running
// thread.
class CrossProcessorWait final
{
public:
    CrossProcessorWait() noexcept
        : m_budget{Platform::Synchronization::ProcessorWaitTicks(), CrossProcessorMinimumPolls,
                   Platform::Synchronization::ProcessorWaitMinimumTicks()}
    {
    }
    [[nodiscard]] bool Poll() noexcept
    {
        Platform::Synchronization::ProcessorWaitProgress();
        return m_budget.TryPoll(Platform::Synchronization::ProcessorWaitTicks());
    }
    static void Timeout(const Foundation::ProcessorMask silent) noexcept
    {
        const auto* set = Platform::Synchronization::CurrentProcessorSet();
        const auto fatal = set != nullptr ? set->SilentOnline(silent) : silent;
        if (fatal != 0U)
        {
            Platform::Synchronization::StopProcessorWait(Diagnostics::StopCode::ShootdownTimeout, fatal);
        }
        // Only the offliner acknowledges and clears residency. A waiter
        // continues to drain until that already-committed retirement completes.
    }

private:
    CrossProcessorWaitBudget m_budget;
};
}
#endif
