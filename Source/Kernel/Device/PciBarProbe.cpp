// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/PciBarProbe.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt16 CommandOffset{0x04U};
constexpr Foundation::UInt16 FirstBarOffset{0x10U};
constexpr Foundation::UInt32 MemorySpaceEnable{1U << 1U};
// A memory BAR's low bits: bit 0 clear, type bits 2:1 zero for 32 bits.
constexpr Foundation::UInt32 BarTypeMask{0x7U};
constexpr Foundation::UInt32 BarFlagsMask{0xFU};
} // namespace

Foundation::Result<PciMemoryBar, PciBarProbeError> ProbePciMemoryBar32(Hal::Device::IPciConfigurationSpace& reader,
                                                                       Hal::Device::IPciConfigurationWriter& writer,
                                                                       const PciFunctionAddress address,
                                                                       const Foundation::UInt8 barIndex) noexcept
{
    using ResultType = Foundation::Result<PciMemoryBar, PciBarProbeError>;
    if (barIndex > 5U)
    {
        return ResultType::Failure(PciBarProbeError::InvalidBarIndex);
    }
    const Foundation::UInt16 barOffset = static_cast<Foundation::UInt16>(FirstBarOffset + (barIndex << 2U));
    Foundation::UInt32 command{0U};
    Foundation::UInt32 bar{0U};
    if ((reader.Read32(address, CommandOffset, command) != Hal::Device::PciConfigurationReadError::None) ||
        (reader.Read32(address, barOffset, bar) != Hal::Device::PciConfigurationReadError::None))
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationReadFailed);
    }
    if ((bar & BarTypeMask) != 0U)
    {
        return ResultType::Failure(PciBarProbeError::NotMemory32);
    }
    if ((bar & ~BarFlagsMask) == 0U)
    {
        return ResultType::Failure(PciBarProbeError::Unassigned);
    }
    // The Command register's upper half is the status, whose bits a write of
    // ones would clear: write the lower half's value back as read.
    const Foundation::UInt32 restored = command & 0xFFFFU;
    Foundation::UInt32 mask{0U};
    const bool decodeOff = writer.Write32(address, CommandOffset, restored & ~MemorySpaceEnable) ==
                           Hal::Device::PciConfigurationWriteError::None;
    const bool written = decodeOff && (writer.Write32(address, barOffset, ~Foundation::UInt32{0U}) ==
                                       Hal::Device::PciConfigurationWriteError::None);
    const bool read =
        written && (reader.Read32(address, barOffset, mask) == Hal::Device::PciConfigurationReadError::None);
    const bool barRestored = writer.Write32(address, barOffset, bar) == Hal::Device::PciConfigurationWriteError::None;
    const bool commandRestored =
        writer.Write32(address, CommandOffset, restored) == Hal::Device::PciConfigurationWriteError::None;
    if (!decodeOff || !written || !barRestored || !commandRestored)
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationWriteFailed);
    }
    if (!read)
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationReadFailed);
    }
    const Foundation::UInt32 sizeMask = mask & ~BarFlagsMask;
    const Foundation::UInt64 length = static_cast<Foundation::UInt64>(~sizeMask) + 1U;
    // The decoded size is a power of two the base is aligned to.
    if ((sizeMask == 0U) || ((length & (length - 1U)) != 0U) ||
        ((static_cast<Foundation::UInt64>(bar & ~BarFlagsMask) & (length - 1U)) != 0U))
    {
        return ResultType::Failure(PciBarProbeError::InvalidSize);
    }
    return ResultType::Success(PciMemoryBar{.BaseAddress = bar & ~BarFlagsMask, .LengthBytes = length});
}
Foundation::Result<PciMemoryBar, PciBarProbeError> ProbePciMemoryBar(Hal::Device::IPciConfigurationSpace& reader,
                                                                     Hal::Device::IPciConfigurationWriter& writer,
                                                                     const PciFunctionAddress address,
                                                                     const Foundation::UInt8 barIndex) noexcept
{
    using ResultType = Foundation::Result<PciMemoryBar, PciBarProbeError>;
    if (barIndex > 5U)
    {
        return ResultType::Failure(PciBarProbeError::InvalidBarIndex);
    }
    const Foundation::UInt16 offset = static_cast<Foundation::UInt16>(FirstBarOffset + (barIndex << 2U));
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    Foundation::UInt32 command{0U};
    if (reader.Read32(address, offset, low) != Hal::Device::PciConfigurationReadError::None)
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationReadFailed);
    }
    if ((low & BarTypeMask) == 0U)
    {
        return ProbePciMemoryBar32(reader, writer, address, barIndex);
    }
    if (((low & BarTypeMask) != 4U) || (barIndex == 5U))
    {
        return ResultType::Failure(PciBarProbeError::NotMemory32);
    }
    const Foundation::UInt16 upper = static_cast<Foundation::UInt16>(offset + 4U);
    if ((reader.Read32(address, upper, high) != Hal::Device::PciConfigurationReadError::None) ||
        (reader.Read32(address, CommandOffset, command) != Hal::Device::PciConfigurationReadError::None))
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationReadFailed);
    }
    const Foundation::UInt64 base = (Foundation::UInt64{high} << 32U) | (low & ~BarFlagsMask);
    if ((base == 0U) || ((command & 4U) != 0U))
    {
        return ResultType::Failure(PciBarProbeError::Unassigned);
    }
    const Foundation::UInt32 restored = command & 0xFFFFU;
    Foundation::UInt32 maskLow{0U};
    Foundation::UInt32 maskHigh{0U};
    const bool written =
        (writer.Write32(address, CommandOffset, restored & ~MemorySpaceEnable) ==
         Hal::Device::PciConfigurationWriteError::None) &&
        (writer.Write32(address, offset, ~Foundation::UInt32{0U}) == Hal::Device::PciConfigurationWriteError::None) &&
        (writer.Write32(address, upper, ~Foundation::UInt32{0U}) == Hal::Device::PciConfigurationWriteError::None);
    const bool read = written &&
                      (reader.Read32(address, offset, maskLow) == Hal::Device::PciConfigurationReadError::None) &&
                      (reader.Read32(address, upper, maskHigh) == Hal::Device::PciConfigurationReadError::None);
    const bool lowRestored = writer.Write32(address, offset, low) == Hal::Device::PciConfigurationWriteError::None;
    const bool highRestored = writer.Write32(address, upper, high) == Hal::Device::PciConfigurationWriteError::None;
    const bool decodeRestored =
        writer.Write32(address, CommandOffset, restored) == Hal::Device::PciConfigurationWriteError::None;
    if (!written || !lowRestored || !highRestored || !decodeRestored)
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationWriteFailed);
    }
    if (!read)
    {
        return ResultType::Failure(PciBarProbeError::ConfigurationReadFailed);
    }
    const Foundation::UInt64 mask = (Foundation::UInt64{maskHigh} << 32U) | (maskLow & ~BarFlagsMask);
    // Two's complement modulo subtraction intentionally computes the BAR size.
    const Foundation::UInt64 bytes = 0U - mask;
    if ((bytes == 0U) || ((bytes & (bytes - 1U)) != 0U) || ((base & (bytes - 1U)) != 0U) ||
        (base > ~Foundation::UInt64{0U} - bytes))
    {
        return ResultType::Failure(PciBarProbeError::InvalidSize);
    }
    return ResultType::Success(PciMemoryBar{.BaseAddress = base, .LengthBytes = bytes});
}
} // namespace Asteria::Kernel::Device
