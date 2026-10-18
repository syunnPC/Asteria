// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_DEVICE_IA64_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_DEVICE_IA64_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Device/IPhysicalMemoryAccess.hpp"

namespace Asteria::Platform::Ia64::Device
{
/// IA-64 uncached physical byte, word and DWORD access. Each operation enters a leaf assembly
/// corridor that quiesces the RSE, disables interruption delivery and PSR.dt, uses the bit-63
/// uncacheable physical address form, and restores the exact entry state before returning. A
/// physical address is therefore at most 63 bits wide, and no range may include the alias bit
/// itself. Native operation preconditions: - The caller executes at PL0 in native mode (PSR.vm is
/// zero) with little-endian data access (PSR.be is zero). - The call runs with PSR.ic set on a
/// kernel stack: in normal context, or in the C++ bridge a PL3 interruption's capture enters, never
/// in the stackless claim or legacy interrupt entries; and the caller prevents same-CPU preemption
/// or re-entry through the backend that issues it.
class Ia64PhysicalMemoryAccess final : public Platform::Device::IPhysicalMemoryAccess
{
public:
    static constexpr Foundation::UInt8 MaximumPhysicalAddressBits{63U};

    constexpr Ia64PhysicalMemoryAccess() noexcept = default;
    ~Ia64PhysicalMemoryAccess() noexcept = default;

    Ia64PhysicalMemoryAccess(const Ia64PhysicalMemoryAccess&) = delete;
    Ia64PhysicalMemoryAccess& operator=(const Ia64PhysicalMemoryAccess&) = delete;
    Ia64PhysicalMemoryAccess(Ia64PhysicalMemoryAccess&&) = delete;
    Ia64PhysicalMemoryAccess& operator=(Ia64PhysicalMemoryAccess&&) = delete;

    [[nodiscard]] Foundation::UInt8 GetMaximumPhysicalAddressBits() const noexcept override;

    [[nodiscard]] bool IsPhysicalRangeImplemented(Foundation::UInt64 start, Foundation::UInt64 length,
                                                  Foundation::UInt8 physicalAddressBits) const noexcept override;

    [[nodiscard]] Foundation::UInt8 ReadByte(Foundation::UInt64 physicalAddress) const noexcept;
    [[nodiscard]] Foundation::UInt16 ReadWord(Foundation::UInt64 physicalAddress) const noexcept;
    void WriteByte(Foundation::UInt64 physicalAddress, Foundation::UInt8 value) const noexcept;
    void WriteWord(Foundation::UInt64 physicalAddress, Foundation::UInt16 value) const noexcept;

    [[nodiscard]] Foundation::UInt64 ReadDoubleWord(Foundation::UInt64 physicalAddress) const noexcept override;

    void WriteDoubleWord(Foundation::UInt64 physicalAddress, Foundation::UInt32 value) const noexcept override;

    /// One naturally aligned 8-byte transaction, never split: a device
    /// register whose upper half carries a command bit (VT-d revision 1.3
    /// section 10.2) is read or written whole.
    [[nodiscard]] Foundation::UInt64 ReadQuadWord(Foundation::UInt64 physicalAddress) const noexcept;
    void WriteQuadWord(Foundation::UInt64 physicalAddress, Foundation::UInt64 value) const noexcept;
};
} // Namespace Asteria::Platform::Ia64::Device

#endif // ASTERIA_PLATFORM_IA64_DEVICE_IA64_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED
