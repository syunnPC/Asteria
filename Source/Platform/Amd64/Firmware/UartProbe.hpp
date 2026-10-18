// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_FIRMWARE_UART_PROBE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_FIRMWARE_UART_PROBE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
class IUartRegisters
{
public:
    [[nodiscard]] virtual Foundation::UInt8 Read(Foundation::UInt8 offset) noexcept = 0;
    virtual void Write(Foundation::UInt8 offset, Foundation::UInt8 value) noexcept = 0;

protected:
    constexpr IUartRegisters() noexcept = default;
    ~IUartRegisters() noexcept = default;
};

/// Boot only, exclusive access. Detects scratch storage and modem loopback,
/// then leaves a responding UART at divisor 1, 8N1, FIFO enabled, IER/OUT2
/// clear, DTR/RTS set. Failure restores scratch/LCR/MCR/IER. Bounded, no DMA,
/// allocation or interrupts. PC16550D datasheet sections 8.1, 8.3-8.10.
[[nodiscard]] bool ProbeUart(IUartRegisters& registers) noexcept;
} // namespace Asteria::Platform::Amd64::Firmware

#endif
