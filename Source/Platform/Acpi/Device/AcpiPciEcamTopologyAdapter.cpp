// SPDX-License-Identifier: Apache-2.0

#include "Platform/Acpi/Device/AcpiPciEcamTopologyAdapter.hpp"

namespace Asteria::Platform::Acpi::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 PciBusConfigurationSizeBytes{Foundation::UInt64{1U} << 20U};

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool OptionalTopologyIsEmpty(const Boot::Acpi::AcpiTopology& topology) noexcept
{
    if (topology.PciConfigurationRegionCount != 0U)
    {
        return false;
    }
    return BytesAreZero(reinterpret_cast<const Foundation::UInt8*>(topology.PciConfigurationRegions),
                        sizeof(topology.PciConfigurationRegions));
}

[[nodiscard]] constexpr bool RegionPrecedes(const Boot::Acpi::AcpiPciConfigurationRegion& left,
                                            const Boot::Acpi::AcpiPciConfigurationRegion& right) noexcept
{
    return (left.Segment < right.Segment) ||
           ((left.Segment == right.Segment) &&
            ((left.FirstBus < right.FirstBus) ||
             ((left.FirstBus == right.FirstBus) &&
              ((left.LastBus < right.LastBus) ||
               ((left.LastBus == right.LastBus) && (left.BaseAddress < right.BaseAddress))))));
}

[[nodiscard]] constexpr bool BusRangesOverlap(const Boot::Acpi::AcpiPciConfigurationRegion& left,
                                              const Boot::Acpi::AcpiPciConfigurationRegion& right) noexcept
{
    return (left.Segment == right.Segment) && (left.FirstBus <= right.LastBus) && (right.FirstBus <= left.LastBus);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 leftStart, const Foundation::UInt64 leftEnd,
                                           const Foundation::UInt64 rightStart,
                                           const Foundation::UInt64 rightEnd) noexcept
{
    return (leftStart < rightEnd) && (rightStart < leftEnd);
}

[[nodiscard]] AcpiPciEcamTopologyError NormalizeRegion(const Boot::Acpi::AcpiPciConfigurationRegion& source,
                                                       Kernel::Device::PciEcamRegion& region,
                                                       Kernel::Device::PciBusRange& busRange,
                                                       Foundation::UInt64& physicalEnd) noexcept
{
    if ((source.Reserved != 0U) || (source.FirstBus > source.LastBus) ||
        ((source.BaseAddress & (PciBusConfigurationSizeBytes - 1U)) != 0U))
    {
        return AcpiPciEcamTopologyError::InvalidOptionalTopology;
    }

    const Foundation::UInt64 firstOffset = static_cast<Foundation::UInt64>(source.FirstBus) << 20U;
    const Foundation::UInt64 busCount =
        static_cast<Foundation::UInt64>(source.LastBus) - static_cast<Foundation::UInt64>(source.FirstBus) + 1U;
    const Foundation::UInt64 apertureBytes = busCount * PciBusConfigurationSizeBytes;
    if ((source.BaseAddress > (MaximumUInt64 - firstOffset)) ||
        ((source.BaseAddress + firstOffset) > (MaximumUInt64 - apertureBytes)))
    {
        return AcpiPciEcamTopologyError::AddressOverflow;
    }

    const Foundation::UInt64 physicalStart = source.BaseAddress + firstOffset;
    physicalEnd = physicalStart + apertureBytes;
    region = Kernel::Device::PciEcamRegion{
        .PhysicalStart = physicalStart,
        .Segment = source.Segment,
        .FirstBus = source.FirstBus,
        .LastBus = source.LastBus,
        .Padding = {},
    };
    busRange = Kernel::Device::PciBusRange{
        .Segment = source.Segment,
        .FirstBus = source.FirstBus,
        .LastBus = source.LastBus,
        .Reserved = {},
    };
    return AcpiPciEcamTopologyError::None;
}
} // namespace

AcpiPciEcamTopologyError AcpiPciEcamTopologyAdapter::Build(const Boot::BootInfo& bootInfo,
                                                           AcpiPciEcamTopology& output) noexcept
{
    if ((bootInfo.Signature != Boot::BootInfoSignature) || (bootInfo.Version != Boot::BootInfoVersion) ||
        (bootInfo.SizeBytes != sizeof(Boot::BootInfo)) || (bootInfo.Reserved != 0U))
    {
        return AcpiPciEcamTopologyError::InvalidBootInfo;
    }

    const Boot::Acpi::AcpiTopology& sourceTopology = bootInfo.Acpi;
    if (sourceTopology.PciConfigurationRegionCount == 0U)
    {
        if (!OptionalTopologyIsEmpty(sourceTopology))
        {
            return AcpiPciEcamTopologyError::InvalidOptionalTopology;
        }
        output = AcpiPciEcamTopology{};
        return AcpiPciEcamTopologyError::None;
    }
    if (sourceTopology.PciConfigurationRegionCount > Boot::Acpi::MaximumAcpiPciConfigurationRegions)
    {
        return AcpiPciEcamTopologyError::InvalidOptionalTopology;
    }

    AcpiPciEcamTopology candidate{};
    Foundation::UInt64 physicalEnds[Boot::Acpi::MaximumAcpiPciConfigurationRegions]{};
    const Foundation::Size count = sourceTopology.PciConfigurationRegionCount;
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        const Boot::Acpi::AcpiPciConfigurationRegion& source = sourceTopology.PciConfigurationRegions[index];
        if ((index != 0U) && !RegionPrecedes(sourceTopology.PciConfigurationRegions[index - 1U], source))
        {
            return AcpiPciEcamTopologyError::InvalidOptionalTopology;
        }
        const AcpiPciEcamTopologyError normalization =
            NormalizeRegion(source, candidate.Regions[index], candidate.BusRanges[index], physicalEnds[index]);
        if (normalization != AcpiPciEcamTopologyError::None)
        {
            return normalization;
        }
    }

    for (Foundation::Size left{0U}; left < count; ++left)
    {
        for (Foundation::Size right{left + 1U}; right < count; ++right)
        {
            if (BusRangesOverlap(sourceTopology.PciConfigurationRegions[left],
                                 sourceTopology.PciConfigurationRegions[right]))
            {
                return AcpiPciEcamTopologyError::OverlappingBusRange;
            }
            if (RangesOverlap(candidate.Regions[left].PhysicalStart, physicalEnds[left],
                              candidate.Regions[right].PhysicalStart, physicalEnds[right]))
            {
                return AcpiPciEcamTopologyError::OverlappingPhysicalRange;
            }
        }
    }

    for (Foundation::Size index{count}; index < Boot::Acpi::MaximumAcpiPciConfigurationRegions; ++index)
    {
        const Boot::Acpi::AcpiPciConfigurationRegion& unused = sourceTopology.PciConfigurationRegions[index];
        if ((unused.BaseAddress != 0U) || (unused.Segment != 0U) || (unused.FirstBus != 0U) || (unused.LastBus != 0U) ||
            (unused.Reserved != 0U))
        {
            return AcpiPciEcamTopologyError::InvalidOptionalTopology;
        }
    }

    candidate.RegionCount = count;
    output = candidate;
    return AcpiPciEcamTopologyError::None;
}
} // namespace Asteria::Platform::Acpi::Device
