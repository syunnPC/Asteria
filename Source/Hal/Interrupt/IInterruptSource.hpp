// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_INTERRUPT_I_INTERRUPT_SOURCE_HPP_INCLUDED
#define ASTERIA_HAL_INTERRUPT_I_INTERRUPT_SOURCE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Hal::Interrupt
{
enum class InterruptSourceError : Foundation::UInt8
{
    None,
    NotReady,
    AlreadyClaimed,
    NotClaimed,
    HardwareFailure,
};

/// One dedicated hardware interrupt source supplied by the platform owner.
///
/// Claim and Release provide single-owner exclusion when a source instance is
/// accidentally offered to more than one kernel Interrupt object. A successful
/// Claim leaves the source masked. A failed Claim never transfers ownership;
/// after a successful Claim, ownership remains with that caller until Release
/// succeeds, including across later operation failures. Operations other than
/// MaskAndAcknowledge run on the bound processor with local interrupts disabled.
/// MaskAndAcknowledge is bounded, allocation-free, callback-free, lock-free,
/// and ISR-safe. It completes controller and processor interrupt ordering before
/// return; device-specific noncoherent DMA synchronization remains the driver's
/// or IOMMU owner's responsibility.
///
/// NotReady, AlreadyClaimed, and NotClaimed leave hardware unchanged.
/// HardwareFailure means the source is unavailable and its state is no longer
/// trustworthy until the platform owner performs external recovery.
class IInterruptSource
{
public:
    [[nodiscard]] virtual InterruptSourceError Claim() noexcept = 0;
    [[nodiscard]] virtual InterruptSourceError Release() noexcept = 0;
    [[nodiscard]] virtual InterruptSourceError Unmask() noexcept = 0;
    [[nodiscard]] virtual InterruptSourceError Mask() noexcept = 0;
    [[nodiscard]] virtual InterruptSourceError MaskAndAcknowledge() noexcept = 0;

protected:
    constexpr IInterruptSource() noexcept = default;
    ~IInterruptSource() noexcept = default;
};

static_assert(sizeof(InterruptSourceError) == 1U);
} // namespace Asteria::Hal::Interrupt

#endif // ASTERIA_HAL_INTERRUPT_I_INTERRUPT_SOURCE_HPP_INCLUDED
