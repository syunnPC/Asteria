// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_MEMORY_FAULT_CHANNEL_TRANSITION_HPP_INCLUDED
#define ASTERIA_KERNEL_MEMORY_FAULT_CHANNEL_TRANSITION_HPP_INCLUDED
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Kernel::Memory
{
enum class PagerChannelState : Foundation::UInt8
{
    Unconfigured,
    Idle,
    FaultPending,
    Faulted,
    Resolving,
};
// A CAS grants exclusive transaction-field access; losers never spin. The occupied bit also
// serializes Pending's Commit/Retry and diagnostic readers without a spin lock. Reusable by
class FaultChannelTransition final
{
public:
    [[nodiscard]] bool Claim(const PagerChannelState from, const PagerChannelState to) const noexcept
    {
        Foundation::UInt64 expected = static_cast<Foundation::UInt64>(from);
        return m_word.CompareExchangeStrongAcquireRelease(expected, static_cast<Foundation::UInt64>(to) | Occupied);
    }
    void Publish(const PagerChannelState state) const noexcept
    {
        Foundation::UInt64 expected = m_word.LoadAcquire();
        if ((expected & Occupied) == 0U ||
            !m_word.CompareExchangeStrongAcquireRelease(expected, static_cast<Foundation::UInt64>(state)))
        {
            __builtin_trap();
        }
    }
    [[nodiscard]] PagerChannelState State() const noexcept
    {
        return static_cast<PagerChannelState>(m_word.LoadAcquire() & ~Occupied);
    }

private:
    static constexpr Foundation::UInt64 Occupied{1U << 8U};
    mutable Platform::Synchronization::AtomicUInt64 m_word{};
};
}
#endif
