// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Memory/Ia64PolledTlbShootdownNotifier.hpp"

namespace Asteria::Platform::Ia64::Memory
{
namespace
{
constexpr Foundation::ProcessorMask ValidProcessorMask{static_cast<Foundation::ProcessorMask>(
    (Foundation::ProcessorMask{1U} << Kernel::Scheduler::MaximumProcessorCount) - 1U)};

[[nodiscard]] constexpr Foundation::ProcessorMask ProcessorBit(const Foundation::UInt8 processor) noexcept
{
    return static_cast<Foundation::ProcessorMask>(Foundation::ProcessorMask{1U} << processor);
}
} // namespace

Ia64PolledTlbShootdownNotifierInitializationError
Ia64PolledTlbShootdownNotifier::Initialize(const Foundation::ProcessorMask profiledProcessorMask) noexcept
{
    if (m_state != Ia64PolledTlbShootdownNotifierState::Uninitialized)
    {
        return Ia64PolledTlbShootdownNotifierInitializationError::AlreadyInitialized;
    }
    if ((profiledProcessorMask == 0U) ||
        ((profiledProcessorMask & static_cast<Foundation::ProcessorMask>(~ValidProcessorMask)) != 0U))
    {
        return Ia64PolledTlbShootdownNotifierInitializationError::InvalidProcessorMask;
    }

    m_profiledProcessorMask = profiledProcessorMask;
    m_state = Ia64PolledTlbShootdownNotifierState::Ready;
    return Ia64PolledTlbShootdownNotifierInitializationError::None;
}

void Ia64PolledTlbShootdownNotifier::Notify(const Foundation::ProcessorMask targetProcessorMask) noexcept
{
    if ((m_state != Ia64PolledTlbShootdownNotifierState::Ready) || (targetProcessorMask == 0U) ||
        ((targetProcessorMask & static_cast<Foundation::ProcessorMask>(~m_profiledProcessorMask)) != 0U))
    {
        return;
    }

    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        if ((targetProcessorMask & ProcessorBit(processor)) == 0U)
        {
            continue;
        }

        // Each acquire/release exchange extends the synchronization chain
        // across overlapping publishers. Poll's matching exchange therefore
        // acquires every coalesced notifier publication that precedes it; the
        // drain still acquire-loads each authoritative mailbox slot directly.
        static_cast<void>(m_pending[processor].ExchangeAcquireRelease(1U));
    }
}

Ia64PolledTlbShootdownPollResult Ia64PolledTlbShootdownNotifier::Poll(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (m_state != Ia64PolledTlbShootdownNotifierState::Ready)
    {
        return Ia64PolledTlbShootdownPollResult::NotReady;
    }
    if (!processor.IsValid())
    {
        return Ia64PolledTlbShootdownPollResult::InvalidProcessor;
    }

    const Foundation::UInt8 processorValue = processor.GetValue();
    if ((m_profiledProcessorMask & ProcessorBit(processorValue)) == 0U)
    {
        return Ia64PolledTlbShootdownPollResult::InvalidProcessor;
    }

    // Clearing before the caller drains prevents a concurrent late
    // publication from being erased after its mailbox slot was missed.
    const Foundation::UInt64 pending = m_pending[processorValue].ExchangeAcquireRelease(0U);
    return (pending == 0U) ? Ia64PolledTlbShootdownPollResult::Empty : Ia64PolledTlbShootdownPollResult::Pending;
}

Ia64PolledTlbShootdownNotifierSnapshot Ia64PolledTlbShootdownNotifier::GetSnapshot() const noexcept
{
    Foundation::ProcessorMask pendingProcessorMask{0U};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        if (m_pending[processor].LoadAcquire() != 0U)
        {
            pendingProcessorMask =
                static_cast<Foundation::ProcessorMask>(pendingProcessorMask | ProcessorBit(processor));
        }
    }

    return Ia64PolledTlbShootdownNotifierSnapshot{
        .ProfiledProcessorMask = m_profiledProcessorMask,
        .PendingProcessorMask = pendingProcessorMask,
        .State = m_state,
        .Reserved = 0U,
    };
}
} // namespace Asteria::Platform::Ia64::Memory
