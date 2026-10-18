// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_ACPI_PCI_TOPOLOGY_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_ACPI_PCI_TOPOLOGY_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot::Acpi
{
constexpr Foundation::Size MaximumAcpiPciConfigurationRegions{8U};

/// One normalized MCFG allocation. BaseAddress is the segment's bus-zero
/// ECAM base; FirstBus and LastBus are inclusive.
struct AcpiPciConfigurationRegion
{
    Foundation::UInt64 BaseAddress;
    Foundation::UInt16 Segment;
    Foundation::UInt8 FirstBus;
    Foundation::UInt8 LastBus;
    Foundation::UInt32 Reserved;
};

static_assert(sizeof(AcpiPciConfigurationRegion) == 16U);
static_assert(alignof(AcpiPciConfigurationRegion) == 8U);
} // namespace Asteria::Boot::Acpi

#endif // ASTERIA_BOOT_ACPI_ACPI_PCI_TOPOLOGY_HPP_INCLUDED
