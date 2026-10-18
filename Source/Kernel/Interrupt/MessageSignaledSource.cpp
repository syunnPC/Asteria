// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Interrupt/MessageSignaledSource.hpp"

namespace Asteria::Kernel::Interrupt
{
using Hal::Interrupt::InterruptSourceError;

// A successful claim leaves the source masked.
InterruptSourceError MessageSignaledSource::Claim() noexcept
{
    Foundation::UInt64 expected{0U};
    return m_state.CompareExchangeStrongAcquireRelease(expected, ClaimedBit | MaskedBit)
               ? InterruptSourceError::None
               : InterruptSourceError::AlreadyClaimed;
}

InterruptSourceError MessageSignaledSource::Release() noexcept
{
    Foundation::UInt64 expected = m_state.LoadAcquire();
    while ((expected & ClaimedBit) != 0U)
    {
        if (m_state.CompareExchangeStrongAcquireRelease(expected, 0U))
        {
            return InterruptSourceError::None;
        }
    }
    return InterruptSourceError::NotClaimed;
}

InterruptSourceError MessageSignaledSource::Unmask() noexcept
{
    Foundation::UInt64 expected = m_state.LoadAcquire();
    while ((expected & ClaimedBit) != 0U)
    {
        if (m_state.CompareExchangeStrongAcquireRelease(expected, ClaimedBit))
        {
            return InterruptSourceError::None;
        }
    }
    return InterruptSourceError::NotClaimed;
}

InterruptSourceError MessageSignaledSource::Mask() noexcept
{
    Foundation::UInt64 expected = m_state.LoadAcquire();
    while ((expected & ClaimedBit) != 0U)
    {
        if (m_state.CompareExchangeStrongAcquireRelease(expected, ClaimedBit | MaskedBit))
        {
            return InterruptSourceError::None;
        }
    }
    return InterruptSourceError::NotClaimed;
}

// The message is already delivered and the processor acknowledges it; only
// the software mask is left to set.
InterruptSourceError MessageSignaledSource::MaskAndAcknowledge() noexcept
{
    return Mask();
}

bool MessageSignaledSource::IsClaimed() const noexcept
{
    return (m_state.LoadAcquire() & ClaimedBit) != 0U;
}

bool MessageSignaledSource::IsMasked() const noexcept
{
    return (m_state.LoadAcquire() & MaskedBit) != 0U;
}
} // namespace Asteria::Kernel::Interrupt
