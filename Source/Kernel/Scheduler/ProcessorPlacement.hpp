// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_PROCESSOR_PLACEMENT_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_PROCESSOR_PLACEMENT_HPP_INCLUDED

#include "Kernel/Scheduler/ReadyQueue.hpp"

namespace Asteria::Kernel::Scheduler
{
// Minimum live bindings plus reserved admissions; equal loads choose the first processor after the
// last successful choice. The ThreadPool lock serializes selection and the subsequent reservation.
[[nodiscard]] constexpr CpuId SelectProcessorHome(const Foundation::ProcessorMask eligible,
                                                  const Foundation::Size (&loads)[MaximumProcessorCount],
                                                  const CpuId previous) noexcept
{
    CpuId selected{};
    Foundation::Size minimum{~Foundation::Size{0U}};
    const Foundation::Size last = previous.IsValid() ? previous.GetValue() : MaximumProcessorCount - 1U;
    for (Foundation::Size step{1U}; step <= MaximumProcessorCount; ++step)
    {
        const auto index = (last + step) % MaximumProcessorCount;
        if ((eligible & (Foundation::ProcessorMask{1U} << index)) != 0U && loads[index] < MaximumReadyQueueEntries &&
            loads[index] < minimum)
        {
            minimum = loads[index];
            selected = CpuId{static_cast<Foundation::UInt8>(index)};
        }
    }
    return selected;
}
}
#endif
