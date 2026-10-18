// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Platform/Device/IPhysicalMemoryAccess.hpp"

namespace Asteria::Platform::Amd64::Device
{
/// AMD64 uncached physical DWORD access through the BSP's owned UC/NX
/// aperture. The native leaf temporarily maps one physical page, performs
/// ordered I/O, and restores the sealed seed leaf before returning. A physical
/// address is at most 52 bits wide. Call only on CPU0's shared root with
/// interrupts disabled, outside an ISR.
class Amd64PhysicalMemoryAccess final : public Platform::Device::IPhysicalMemoryAccess
{
public:
    static constexpr Foundation::UInt8 MaximumPhysicalAddressBits{52U};

    constexpr Amd64PhysicalMemoryAccess() noexcept = default;
    ~Amd64PhysicalMemoryAccess() noexcept = default;

    Amd64PhysicalMemoryAccess(const Amd64PhysicalMemoryAccess&) = delete;
    Amd64PhysicalMemoryAccess& operator=(const Amd64PhysicalMemoryAccess&) = delete;
    Amd64PhysicalMemoryAccess(Amd64PhysicalMemoryAccess&&) = delete;
    Amd64PhysicalMemoryAccess& operator=(Amd64PhysicalMemoryAccess&&) = delete;

    [[nodiscard]] Foundation::UInt8 GetMaximumPhysicalAddressBits() const noexcept override;

    [[nodiscard]] bool IsPhysicalRangeImplemented(Foundation::UInt64 start, Foundation::UInt64 length,
                                                  Foundation::UInt8 physicalAddressBits) const noexcept override;

    [[nodiscard]] Foundation::UInt64 ReadDoubleWord(Foundation::UInt64 physicalAddress) const noexcept override;

    void WriteDoubleWord(Foundation::UInt64 physicalAddress, Foundation::UInt32 value) const noexcept override;
};
} // namespace Asteria::Platform::Amd64::Device

#endif // ASTERIA_PLATFORM_AMD64_DEVICE_AMD64_PHYSICAL_MEMORY_ACCESS_HPP_INCLUDED
