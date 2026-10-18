// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_HAL_DEVICE_I_MEMORY_MAPPED_IO_WRITER_HPP_INCLUDED
#define ASTERIA_HAL_DEVICE_I_MEMORY_MAPPED_IO_WRITER_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"

namespace Asteria::Hal::Device
{
enum class MemoryMappedIoWriteError : Foundation::UInt8
{
    None,
    NotReady,
    InvalidAddress,
    UnsupportedWidth,
    ValueOutOfRange,
    AccessDenied,
    HardwareFailure,
};

/// Performs one checked, architecture-ordered MMIO write.
///
/// Implementations accept only storage whose mapping or physical-access
/// authority remains live for the operation. Calls are bounded,
/// allocation-free, callback-free, and forbidden from interrupt context or
/// same-CPU reentry.
class IMemoryMappedIoWriter
{
public:
    [[nodiscard]] virtual MemoryMappedIoWriteError Write(Foundation::UInt64 physicalAddress, MemoryMappedIoWidth width,
                                                         Foundation::UInt64 value) noexcept = 0;

protected:
    constexpr IMemoryMappedIoWriter() noexcept = default;
    ~IMemoryMappedIoWriter() noexcept = default;
};

static_assert(sizeof(MemoryMappedIoWriteError) == 1U);
} // namespace Asteria::Hal::Device

#endif // ASTERIA_HAL_DEVICE_I_MEMORY_MAPPED_IO_WRITER_HPP_INCLUDED
