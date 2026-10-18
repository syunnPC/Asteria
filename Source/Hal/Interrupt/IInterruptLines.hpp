// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_INTERRUPT_I_INTERRUPT_LINES_HPP_INCLUDED
#define ASTERIA_HAL_INTERRUPT_I_INTERRUPT_LINES_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Interrupt/IInterruptSource.hpp"

namespace Asteria::Kernel::Interrupt
{
class Interrupt;
}

namespace Asteria::Hal::Interrupt
{
/// A reserved interrupt line: its source, unclaimed, and the processor
/// vector it arrives on; a null Source when none could be reserved.
struct LineReservation
{
    IInterruptSource* Source;
    Foundation::UInt64 Vector;
};

/// The interrupt lines a platform routes PCI INTx to. An Interrupt takes a line in two phases:
/// Reserve gives the line's source before the Interrupt is built on it, Publish binds the line's
/// vector to the Interrupt once it is, and Release frees both once the Interrupt ended. One
/// Interrupt per line. Normal context; the delivery path is the platform's.
class IInterruptLines
{
public:
    /// The source of the global system interrupt line, delivered on a
    /// vector of its own; a null Source for a line the platform does not
    /// route or one another Interrupt holds.
    [[nodiscard]] virtual LineReservation Reserve(Foundation::UInt32 line) noexcept = 0;

    /// Binds vector, a reservation's, to interrupt, whose handler the vector
    /// enters from now on.
    [[nodiscard]] virtual bool Publish(Foundation::UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept = 0;

    /// Frees vector and its line, published or not, once its Interrupt can
    /// take no more deliveries.
    virtual void Release(Foundation::UInt64 vector) noexcept = 0;

protected:
    constexpr IInterruptLines() noexcept = default;
    ~IInterruptLines() noexcept = default;
};

static_assert(sizeof(LineReservation) == 16U);
} // Namespace Asteria::Hal::Interrupt

#endif // ASTERIA_HAL_INTERRUPT_I_INTERRUPT_LINES_HPP_INCLUDED
