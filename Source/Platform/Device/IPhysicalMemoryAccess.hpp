// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_DEVICE_I_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED
#define ASTERIA_PLATFORM_DEVICE_I_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Device
{
/// One ISA's uncached physical DWORD access and the physical addresses it can
/// reach.
///
/// PhysicalMemoryMappedIo validates authority, width, alignment, and range
/// before it calls ReadDoubleWord or WriteDoubleWord, so an implementation
/// receives only an aligned DWORD inside a registered DeviceRegion. Each
/// implementation states its own native preconditions; both operations are
/// bounded, allocation-free, callback-free, and forbidden from interrupt
/// context or same-CPU reentry.
class IPhysicalMemoryAccess
{
public:
    /// The widest physical address this access can name; Initialize and the
    /// PCI runtime reject wider firmware-reported widths.
    [[nodiscard]] virtual Foundation::UInt8 GetMaximumPhysicalAddressBits() const noexcept = 0;

    /// Whether every byte of [start, start + length) is a physical address
    /// this access can reach on a processor with physicalAddressBits.
    [[nodiscard]] virtual bool IsPhysicalRangeImplemented(Foundation::UInt64 start, Foundation::UInt64 length,
                                                          Foundation::UInt8 physicalAddressBits) const noexcept = 0;

    /// Reads the DWORD at physicalAddress; the value is zero-extended.
    [[nodiscard]] virtual Foundation::UInt64 ReadDoubleWord(Foundation::UInt64 physicalAddress) const noexcept = 0;

    virtual void WriteDoubleWord(Foundation::UInt64 physicalAddress, Foundation::UInt32 value) const noexcept = 0;

protected:
    constexpr IPhysicalMemoryAccess() noexcept = default;
    ~IPhysicalMemoryAccess() noexcept = default;
};
} // namespace Asteria::Platform::Device

#endif // ASTERIA_PLATFORM_DEVICE_I_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED
