// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_SCHEDULER_SCHEDULER_TYPES_HPP_INCLUDED
#define ASTERIA_KERNEL_SCHEDULER_SCHEDULER_TYPES_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Scheduler
{
constexpr Foundation::UInt8 MaximumProcessorCount{4U};
// Storage stays bounded while masks can widen.
static_assert(MaximumProcessorCount <= sizeof(Foundation::ProcessorMask) * 8U);

class CpuId final
{
public:
    constexpr CpuId() noexcept = default;

    explicit constexpr CpuId(const Foundation::UInt8 value) noexcept : m_value{value}
    {
    }

    [[nodiscard]] constexpr Foundation::UInt8 GetValue() const noexcept
    {
        return m_value;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return m_value < MaximumProcessorCount;
    }

private:
    Foundation::UInt8 m_value{0xFFU};
};

static_assert(sizeof(CpuId) == 1U);
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_KERNEL_SCHEDULER_SCHEDULER_TYPES_HPP_INCLUDED
