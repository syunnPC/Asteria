// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_INTERRUPT_MESSAGE_SIGNALED_SOURCE_HPP_INCLUDED
#define ASTERIA_KERNEL_INTERRUPT_MESSAGE_SIGNALED_SOURCE_HPP_INCLUDED

#include "Hal/Interrupt/IInterruptSource.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

namespace Asteria::Kernel::Interrupt
{
/// The source of one message-signaled interrupt. Its owner programs the device's MSI capability,
/// its address, data and enable bit with INTx disabled, in normal context and outside the
/// Interrupt's operations; this object only tracks ownership and a software mask, so every
/// operation is lock-free and MaskAndAcknowledge is ISR-safe. A message that arrives while masked
/// still enters the Interrupt, whose state machine treats it as spurious or coalesces it.
class MessageSignaledSource final : public Hal::Interrupt::IInterruptSource
{
public:
    constexpr MessageSignaledSource() noexcept = default;

    MessageSignaledSource(const MessageSignaledSource&) = delete;
    MessageSignaledSource& operator=(const MessageSignaledSource&) = delete;

    [[nodiscard]] Hal::Interrupt::InterruptSourceError Claim() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Release() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Unmask() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Mask() noexcept override;
    [[nodiscard]] Hal::Interrupt::InterruptSourceError MaskAndAcknowledge() noexcept override;

    [[nodiscard]] bool IsClaimed() const noexcept;
    [[nodiscard]] bool IsMasked() const noexcept;

private:
    static constexpr Foundation::UInt64 ClaimedBit{1U};
    static constexpr Foundation::UInt64 MaskedBit{2U};

    Platform::Synchronization::AtomicUInt64 m_state;
};
} // Namespace Asteria::Kernel::Interrupt

#endif // ASTERIA_KERNEL_INTERRUPT_MESSAGE_SIGNALED_SOURCE_HPP_INCLUDED
