// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Firmware/Ia64DebugUart.hpp"

#include "Platform/Ia64/Device/Ia64PortIo.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt64;

constexpr UInt64 TransmitHoldingRegister{0U};
constexpr UInt64 LineStatusRegister{5U};
// The sparse encoding spreads the 16-bit port space over 64 MiB.
constexpr UInt64 PortSpaceWindowBytes{0x0400'0000U};
constexpr UInt64 PageBytes{4096U};
} // namespace

Ia64PortSpaceWindow FindIa64PortSpaceWindow(const Boot::PhysicalMemoryRegion* const regions, const Size count) noexcept
{
    Ia64PortSpaceWindow window{};
    Size found{0U};
    for (Size index{0U}; (regions != nullptr) && (index < count); ++index)
    {
        const Boot::PhysicalMemoryRegion& region = regions[index];
        if (region.FirmwareType != Ia64PortSpaceFirmwareType)
        {
            continue;
        }
        ++found;
        window = {region.PhysicalStart,
                  region.PageCount <= (~UInt64{0U} / PageBytes) ? region.PageCount * PageBytes : 0U};
    }
    return found == 1U ? window : Ia64PortSpaceWindow{};
}

Ia64DebugUart ResolveIa64DebugUart(const Boot::BootDebugPort& port, const Ia64PortSpaceWindow& window) noexcept
{
    if ((port.Source == static_cast<Foundation::UInt8>(Boot::BootDebugPortSource::None)) ||
        !Boot::IsValidBootDebugPort(port))
    {
        return {};
    }
    const UInt64 stride = port.RegisterStrideBytes;
    if (port.AddressSpace == static_cast<Foundation::UInt8>(Boot::BootDebugPortSpace::SystemMemory))
    {
        return {port.Address + (TransmitHoldingRegister * stride), port.Address + (LineStatusRegister * stride)};
    }
    // The window must be the whole aligned sparse space below the
    // uncacheable alias bit, as Ia64PortIo::Configure requires.
    if ((window.Base == 0U) || ((window.Base & (PortSpaceWindowBytes - 1U)) != 0U) ||
        (window.Bytes < PortSpaceWindowBytes) || (window.Base > ((UInt64{1U} << 63U) - PortSpaceWindowBytes)))
    {
        return {};
    }
    return {
        Device::Ia64PortIo::SparseAddress(window.Base,
                                          static_cast<UInt16>(port.Address + (TransmitHoldingRegister * stride))),
        Device::Ia64PortIo::SparseAddress(window.Base,
                                          static_cast<UInt16>(port.Address + (LineStatusRegister * stride))),
    };
}
} // namespace Asteria::Platform::Ia64::Firmware
