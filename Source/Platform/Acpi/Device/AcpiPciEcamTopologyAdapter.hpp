// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_ACPI_DEVICE_ACPI_PCI_ECAM_TOPOLOGY_ADAPTER_HPP_INCLUDED
#define ASTERIA_PLATFORM_ACPI_DEVICE_ACPI_PCI_ECAM_TOPOLOGY_ADAPTER_HPP_INCLUDED

#include "Boot/Acpi/AcpiPciTopology.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Device/PciEcamConfigurationSpace.hpp"
#include "Kernel/Device/PciTypes.hpp"

namespace Asteria::Platform::Acpi::Device
{
enum class AcpiPciEcamTopologyError : Foundation::UInt8
{
    None,
    InvalidBootInfo,
    InvalidOptionalTopology,
    AddressOverflow,
    OverlappingBusRange,
    OverlappingPhysicalRange,
};

struct AcpiPciEcamTopology
{
    Kernel::Device::PciEcamRegion Regions[Boot::Acpi::MaximumAcpiPciConfigurationRegions];
    Kernel::Device::PciBusRange BusRanges[Boot::Acpi::MaximumAcpiPciConfigurationRegions];
    Foundation::Size RegionCount;
};

/// Converts ACPI MCFG's bus-zero bases into generic FirstBus-relative ECAM
/// regions. It does not inspect EFI memory descriptors, establish mappings,
/// access hardware, enumerate functions, or retain firmware pointers.
class AcpiPciEcamTopologyAdapter final
{
public:
    /// Failure leaves output unchanged. A fully empty optional MCFG subrecord
    /// succeeds with RegionCount zero; callers may treat malformed optional
    /// data as the same platform-unavailable fallback without failing boot.
    [[nodiscard]] static AcpiPciEcamTopologyError Build(const Boot::BootInfo& bootInfo,
                                                        AcpiPciEcamTopology& output) noexcept;

private:
    AcpiPciEcamTopologyAdapter() = delete;
};

static_assert(sizeof(AcpiPciEcamTopologyError) == 1U);
static_assert(sizeof(AcpiPciEcamTopology) == 200U);
static_assert(alignof(AcpiPciEcamTopology) == 8U);
} // namespace Asteria::Platform::Acpi::Device

#endif // ASTERIA_PLATFORM_ACPI_DEVICE_ACPI_PCI_ECAM_TOPOLOGY_ADAPTER_HPP_INCLUDED
