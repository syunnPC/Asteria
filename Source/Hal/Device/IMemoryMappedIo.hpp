// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_MEMORY_MAPPED_IO_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_MEMORY_MAPPED_IO_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Hal::Device
{
enum class MemoryMappedIoWidth : Foundation::UInt8
{
    Byte = 1U,
    Word = 2U,
    DoubleWord = 4U,
    QuadWord = 8U,
};

enum class MemoryMappedIoReadError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidAddress,
    UnsupportedWidth,
    HardwareFailure,
};

/// Performs one checked, architecture-ordered MMIO read.
///
/// Implementations accept only storage whose mapping or physical-access
/// authority remains live for the operation. Success returns a host integer;
/// failure leaves output unchanged. Calls are bounded, allocation-free,
/// callback-free, and forbidden from interrupt context or same-CPU reentry.
class IMemoryMappedIo
{
public:
    [[nodiscard]] virtual MemoryMappedIoReadError Read(Foundation::UInt64 physicalAddress, MemoryMappedIoWidth width,
                                                       Foundation::UInt64& output) noexcept = 0;

protected:
    constexpr IMemoryMappedIo() noexcept = default;
    ~IMemoryMappedIo() noexcept = default;
};

static_assert(sizeof(MemoryMappedIoWidth) == 1U);
static_assert(sizeof(MemoryMappedIoReadError) == 1U);
} // namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_MEMORY_MAPPED_IO_HPP_INCLUDED
