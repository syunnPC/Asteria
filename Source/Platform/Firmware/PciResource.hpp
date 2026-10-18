// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_FIRMWARE_PCI_RESOURCE_HPP_INCLUDED
#define ASTERIA_PLATFORM_FIRMWARE_PCI_RESOURCE_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Platform::Firmware
{
/// One captured 32-bit memory BAR: the AHCI controller's ABAR, or the BAR a
/// capture by identifier selects (Abar* name the selected BAR there too).
struct AhciPciResource
{
    Foundation::UInt64 AbarBaseAddress;
    Foundation::UInt64 AbarLengthBytes;
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt8 BarIndex;
    Foundation::UInt8 Reserved[2];
};

static_assert(sizeof(AhciPciResource) == 24U);
static_assert(alignof(AhciPciResource) == 8U);
} // namespace Asteria::Platform::Firmware

#endif // ASTERIA_PLATFORM_FIRMWARE_PCI_RESOURCE_HPP_INCLUDED
