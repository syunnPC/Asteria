// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_INTERRUPT_I_MESSAGE_VECTORS_HPP_INCLUDED
#define ASTERIA_HAL_INTERRUPT_I_MESSAGE_VECTORS_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Interrupt
{
class Interrupt;
}

namespace Asteria::Hal::Interrupt
{
/// The message a device writes to raise one processor vector: its address
/// and data; a zero Vector when none was free.
struct MessageTarget
{
    Foundation::UInt64 Vector;
    Foundation::UInt64 Address;
    Foundation::UInt16 Data;
    Foundation::UInt8 Reserved[6];
};

/// The processor vectors message-signalled interrupts arrive on, each bound to at most one kernel
/// Interrupt, whose handler the vector enters. Normal context; the delivery path is the platform's.
class IMessageVectors
{
public:
    [[nodiscard]] virtual MessageTarget Bind(Kernel::Interrupt::Interrupt& interrupt) noexcept = 0;

    /// Frees vector once its source can raise it no more; false for a
    /// vector bound to something else.
    [[nodiscard]] virtual bool Unbind(Foundation::UInt64 vector,
                                      const Kernel::Interrupt::Interrupt& interrupt) noexcept = 0;

protected:
    constexpr IMessageVectors() noexcept = default;
    ~IMessageVectors() noexcept = default;
};

static_assert(sizeof(MessageTarget) == 24U);
} // Namespace Asteria::Hal::Interrupt

#endif // ASTERIA_HAL_INTERRUPT_I_MESSAGE_VECTORS_HPP_INCLUDED
