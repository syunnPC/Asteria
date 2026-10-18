// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Acpi/Device/AcpiPciEcamTopologyAdapter.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Platform::Acpi::Device::AcpiPciEcamTopology;
using Platform::Acpi::Device::AcpiPciEcamTopologyAdapter;
using Platform::Acpi::Device::AcpiPciEcamTopologyError;

[[nodiscard]] Boot::BootInfo MakeBootInfo() noexcept
{
    Boot::BootInfo bootInfo{};
    bootInfo.Signature = Boot::BootInfoSignature;
    bootInfo.Version = Boot::BootInfoVersion;
    bootInfo.SizeBytes = sizeof(Boot::BootInfo);
    return bootInfo;
}

void TestAbsentAndNormalizedTopology(TestContext& runner) noexcept
{
    Boot::BootInfo bootInfo = MakeBootInfo();
    AcpiPciEcamTopology topology{};
    topology.RegionCount = 7U;
    const auto absent = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    ASTERIA_EXPECT(runner, "a fully absent optional MCFG topology succeeds empty",
                   absent == AcpiPciEcamTopologyError::None, topology.RegionCount == 0U);

    bootInfo.Acpi.McfgPhysicalAddress = 0x2000U;
    bootInfo.Acpi.McfgRevision = 1U;
    topology.RegionCount = 7U;
    const auto inertMetadata = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    ASTERIA_EXPECT(runner, "inert MCFG metadata does not change absent allocation semantics",
                   inertMetadata == AcpiPciEcamTopologyError::None, topology.RegionCount == 0U);

    bootInfo.Acpi.McfgPhysicalAddress = 0x1000U;
    bootInfo.Acpi.McfgRevision = 1U;
    bootInfo.Acpi.PciConfigurationRegionCount = 2U;
    bootInfo.Acpi.PciConfigurationRegions[0U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = 0x4000'0000ULL,
        .Segment = 2U,
        .FirstBus = 32U,
        .LastBus = 47U,
        .Reserved = 0U,
    };
    bootInfo.Acpi.PciConfigurationRegions[1U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = 0x8000'0000ULL,
        .Segment = 9U,
        .FirstBus = 64U,
        .LastBus = 95U,
        .Reserved = 0U,
    };
    const auto normalized = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    ASTERIA_EXPECT(runner, "MCFG bus-zero bases normalize to generic FirstBus starts",
                   normalized == AcpiPciEcamTopologyError::None, topology.RegionCount == 2U,
                   topology.Regions[0U].PhysicalStart == 0x4200'0000ULL, topology.Regions[0U].FirstBus == 32U,
                   topology.Regions[1U].PhysicalStart == 0x8400'0000ULL, topology.BusRanges[1U].Segment == 9U,
                   topology.BusRanges[1U].LastBus == 95U);

    bootInfo.Acpi.McfgPhysicalAddress = 0U;
    bootInfo.Acpi.McfgRevision = 0U;
    const auto metadataIndependent = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    ASTERIA_EXPECT(runner, "region adaptation does not impose a firmware-provenance gate",
                   metadataIndependent == AcpiPciEcamTopologyError::None, topology.RegionCount == 2U,
                   topology.Regions[0U].PhysicalStart == 0x4200'0000ULL);
}

void TestFailureAtomicityAndOverflow(TestContext& runner) noexcept
{
    Boot::BootInfo bootInfo = MakeBootInfo();
    bootInfo.Acpi.McfgPhysicalAddress = 0x1000U;
    bootInfo.Acpi.McfgRevision = 1U;
    bootInfo.Acpi.PciConfigurationRegionCount = 1U;
    bootInfo.Acpi.PciConfigurationRegions[0U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = 0xFFFF'FFFF'FFF0'0000ULL,
        .Segment = 0U,
        .FirstBus = 1U,
        .LastBus = 1U,
        .Reserved = 0U,
    };
    AcpiPciEcamTopology topology{};
    topology.RegionCount = 5U;
    topology.Regions[0U].PhysicalStart = 0xDEAD'BEEFULL;
    const auto overflow = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    ASTERIA_EXPECT(runner, "normalization overflow leaves the output unchanged",
                   overflow == AcpiPciEcamTopologyError::AddressOverflow, topology.RegionCount == 5U,
                   topology.Regions[0U].PhysicalStart == 0xDEAD'BEEFULL);

    bootInfo.Signature = 0U;
    ASTERIA_EXPECT(runner, "invalid BootInfo metadata fails without publishing topology",
                   AcpiPciEcamTopologyAdapter::Build(bootInfo, topology) == AcpiPciEcamTopologyError::InvalidBootInfo,
                   topology.RegionCount == 5U);
}

void TestOverlapAndMalformedOptionalData(TestContext& runner) noexcept
{
    Boot::BootInfo bootInfo = MakeBootInfo();
    bootInfo.Acpi.McfgPhysicalAddress = 0x2000U;
    bootInfo.Acpi.McfgRevision = 1U;
    bootInfo.Acpi.PciConfigurationRegionCount = 2U;
    bootInfo.Acpi.PciConfigurationRegions[0U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = 0x4000'0000ULL,
        .Segment = 3U,
        .FirstBus = 0U,
        .LastBus = 7U,
        .Reserved = 0U,
    };
    bootInfo.Acpi.PciConfigurationRegions[1U] = Boot::Acpi::AcpiPciConfigurationRegion{
        .BaseAddress = 0x5000'0000ULL,
        .Segment = 3U,
        .FirstBus = 7U,
        .LastBus = 8U,
        .Reserved = 0U,
    };
    AcpiPciEcamTopology topology{};
    const auto busOverlap = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    bootInfo.Acpi.PciConfigurationRegions[1U].Segment = 4U;
    bootInfo.Acpi.PciConfigurationRegions[1U].BaseAddress = 0x3F90'0000ULL;
    const auto physicalOverlap = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    bootInfo.Acpi.PciConfigurationRegionCount = 0U;
    const auto partialAbsence = AcpiPciEcamTopologyAdapter::Build(bootInfo, topology);
    ASTERIA_EXPECT(runner, "overlap and partially absent optional records are rejected",
                   busOverlap == AcpiPciEcamTopologyError::OverlappingBusRange,
                   physicalOverlap == AcpiPciEcamTopologyError::OverlappingPhysicalRange,
                   partialAbsence == AcpiPciEcamTopologyError::InvalidOptionalTopology);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AcpiPciEcamTopologyAdapterTests"};
    Asteria::Test::TestAbsentAndNormalizedTopology(runner);
    Asteria::Test::TestFailureAtomicityAndOverflow(runner);
    Asteria::Test::TestOverlapAndMalformedOptionalData(runner);
    return runner.Finish();
}
