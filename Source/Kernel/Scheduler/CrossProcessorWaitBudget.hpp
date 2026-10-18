// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_CROSS_PROCESSOR_WAIT_BUDGET_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_CROSS_PROCESSOR_WAIT_BUDGET_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Scheduler
{
// Both bounds must expire; a descheduled vCPU advancing the clock alone, or a fast poller alone,
// cannot time out a peer.
inline constexpr Foundation::Size CrossProcessorMinimumPolls{10'000'000U};
inline constexpr Foundation::UInt64 Ia64CrossProcessorMinimumTicks{8'000'000'000U};
inline constexpr Foundation::UInt64 Amd64CrossProcessorWaitSeconds{2U};

class CrossProcessorWaitBudget final
{
public:
    constexpr CrossProcessorWaitBudget(const Foundation::UInt64 startTicks, const Foundation::Size minimumPolls,
                                       const Foundation::UInt64 minimumTicks) noexcept
        : m_startTicks{startTicks}, m_minimumTicks{minimumTicks}, m_minimumPolls{minimumPolls}
    {
    }

    [[nodiscard]] constexpr bool TryPoll(const Foundation::UInt64 nowTicks) noexcept
    {
        const Foundation::UInt64 elapsed = nowTicks >= m_startTicks ? nowTicks - m_startTicks : 0U;
        if (m_polls >= m_minimumPolls && elapsed >= m_minimumTicks)
        {
            return false;
        }
        if (m_polls != ~Foundation::Size{0U})
        {
            ++m_polls;
        }
        return true;
    }
    [[nodiscard]] constexpr Foundation::Size GetPolls() const noexcept
    {
        return m_polls;
    }

private:
    Foundation::UInt64 m_startTicks;
    Foundation::UInt64 m_minimumTicks;
    Foundation::Size m_minimumPolls;
    Foundation::Size m_polls{0U};
};
} // Namespace Asteria::Kernel::Scheduler
#endif
