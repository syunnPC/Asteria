// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::PhysicalPageRange;
using Platform::Amd64::Firmware::Amd64LoadedImageLayout;
using Platform::Amd64::Firmware::Amd64LoadedImageSection;
using Platform::Amd64::Firmware::Amd64LoadedImageSectionPurpose;
using Platform::Amd64::Memory::BootstrapGuardPageCount;
using Platform::Amd64::Memory::BootstrapDescriptorTableStoragePageCount;
using Platform::Amd64::Memory::BootstrapInitialMappingCount;
using Platform::Amd64::Memory::BootstrapMappingPlanBuilder;
using Platform::Amd64::Memory::BootstrapMappingPlanError;
using Platform::Amd64::Memory::BootstrapMappingPlanSummary;
using Platform::Amd64::Memory::BootstrapMaximumRequiredPageTablePages;
using Platform::Amd64::Memory::BootstrapPageTableStoragePageCount;
using Platform::Amd64::Memory::BootstrapReservedPageCount;
using Platform::Amd64::Memory::BootstrapStackPageCount;
using Platform::Amd64::Memory::GetBootstrapGuardPhysicalStart;
using Platform::Amd64::Memory::GetBootstrapDescriptorTableStoragePhysicalEnd;
using Platform::Amd64::Memory::GetBootstrapPageTableStoragePhysicalEnd;
using Platform::Amd64::Memory::GetBootstrapReservedPhysicalEnd;
using Platform::Amd64::Memory::GetBootstrapStackPhysicalEnd;
using Platform::Amd64::Memory::InitialPageTableBuildError;
using Platform::Amd64::Memory::InitialPageTableBuildResult;
using Platform::Amd64::Memory::InitialPageTableBuilder;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;

constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 WriteBackAttribute{0x8U};
constexpr Foundation::UInt64 ExecuteProtectAttribute{Foundation::UInt64{1U} << 14U};
constexpr Foundation::UInt64 ReadOnlyAttribute{Foundation::UInt64{1U} << 17U};
constexpr Foundation::UInt64 RuntimeAttribute{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 LowerCanonicalAddressLimit{0x0000'8000'0000'0000ULL};
constexpr Foundation::UInt64 PrefixImageBoundary{0x0000'0080'0000'0000ULL};
constexpr Foundation::UInt64 PrefixReservedBoundary{0x0000'0180'0000'0000ULL};

constinit InitialMappingPlan g_output;
constinit InitialMappingPlan g_secondOutput;
constinit InitialMappingPlan g_dirtyOutput;
constinit InitialMappingPlan g_failureOutput;
constinit InitialMappingPlan g_provenanceOutput;
constinit InitialMappingPlan g_loaderOutput;
constinit InitialMappingPlan g_attributeOutput;
constinit InitialMappingPlan g_permittedProvenanceOutput;
constinit InitialMappingPlan g_boundaryOutput;
constinit InitialMappingPlan g_leftAdjacentOutput;
constinit InitialMappingPlan g_rightAdjacentOutput;
constinit InitialMappingPlan g_prefixOutput;
alignas(4096) Foundation::UInt64
    g_tableEntries[BootstrapPageTableStoragePageCount * Platform::Amd64::Memory::PageTableEntriesPerPage];
alignas(4096) Foundation::UInt64
    g_prefixTableEntries[BootstrapPageTableStoragePageCount * Platform::Amd64::Memory::PageTableEntriesPerPage];
constinit BootInfo g_bootInfo{
    .Signature = Boot::BootInfoSignature,
    .Version = Boot::BootInfoVersion,
    .SizeBytes = sizeof(BootInfo),
    .MemoryRegionCount = 2U,
    .FirmwareDescriptorVersion = 1U,
    .FirmwareDescriptorSize = 40U,
    .Reserved = 0U,
    .FirmwareMemoryMapSizeBytes = 80U,
    .MemoryRegions =
        {
            PhysicalMemoryRegion{
                .PhysicalStart = ReservedBase,
                .VirtualStart = 0U,
                .PageCount = BootstrapReservedPageCount,
                .Attributes = WriteBackAttribute,
                .FirmwareType = 2U,
                .Kind = PhysicalMemoryKind::Loader,
            },
            PhysicalMemoryRegion{
                .PhysicalStart = ImageBase,
                .VirtualStart = 0U,
                .PageCount = 10U,
                .Attributes = WriteBackAttribute,
                .FirmwareType = 2U,
                .Kind = PhysicalMemoryKind::Loader,
            },
        },
    .Acpi = {},
    .Entropy = {},
    .Display = {},
    .Inventory = {},
    .Files = {},
    .Crc32c = 0U,
    .ReservedAbi = 0U,
    .Kernel = {},
    .InventoryRecord = {},
    .DebugPort = {},
    .Trust = {},
    .DmaExclusions = {},
    .LowMemoryBlocks = 0U,
    .WatchdogSeconds = Asteria::Abi::WatchdogDefaultSeconds,
    .Power = {},
    .SessionMode = 0U,
    .LoginEnabled = 0U,
};
constinit BootInfo g_bootInfoSnapshot{};
constinit BootInfo g_dynamicBootInfo{};
constinit BootInfo g_prefixBootInfo{
    .Signature = Boot::BootInfoSignature,
    .Version = Boot::BootInfoVersion,
    .SizeBytes = sizeof(BootInfo),
    .MemoryRegionCount = 2U,
    .FirmwareDescriptorVersion = 1U,
    .FirmwareDescriptorSize = 40U,
    .Reserved = 0U,
    .FirmwareMemoryMapSizeBytes = 80U,
    .MemoryRegions =
        {
            PhysicalMemoryRegion{
                .PhysicalStart = PrefixImageBoundary - 0x2000U,
                .VirtualStart = 0U,
                .PageCount = 512U,
                .Attributes = WriteBackAttribute,
                .FirmwareType = 2U,
                .Kind = PhysicalMemoryKind::Loader,
            },
            PhysicalMemoryRegion{
                .PhysicalStart = PrefixReservedBoundary - 0x2000U,
                .VirtualStart = 0U,
                .PageCount = BootstrapReservedPageCount,
                .Attributes = WriteBackAttribute,
                .FirmwareType = 2U,
                .Kind = PhysicalMemoryKind::Loader,
            },
        },
    .Acpi = {},
    .Entropy = {},
    .Display = {},
    .Inventory = {},
    .Files = {},
    .Crc32c = 0U,
    .ReservedAbi = 0U,
    .Kernel = {},
    .InventoryRecord = {},
    .DebugPort = {},
    .Trust = {},
    .DmaExclusions = {},
    .LowMemoryBlocks = 0U,
    .WatchdogSeconds = Asteria::Abi::WatchdogDefaultSeconds,
    .Power = {},
    .SessionMode = 0U,
    .LoginEnabled = 0U,
};

[[nodiscard]] constexpr Amd64LoadedImageLayout ValidImage() noexcept
{
    return Amd64LoadedImageLayout{
        .ImageBase = ImageBase,
        .ImageSize = 0xA000U,
        .Sections =
            {
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x1000U,
                    .PageCount = 2U,
                    .Purpose = Amd64LoadedImageSectionPurpose::Code,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x3000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x4000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x5000U,
                    .PageCount = 2U,
                    .Purpose = Amd64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x7000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x8000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x9000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
            },
    };
}

[[nodiscard]] constexpr Amd64LoadedImageLayout PrefixBoundaryImage() noexcept
{
    constexpr Foundation::UInt64 base{PrefixImageBoundary - 0x2000U};
    return Amd64LoadedImageLayout{
        .ImageBase = base,
        .ImageSize = 0x20'0000U,
        .Sections =
            {
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x1000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::Code,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x2000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x3000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x4000U,
                    .PageCount = 505U,
                    .Purpose = Amd64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x1FD000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x1FE000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Amd64LoadedImageSection{
                    .VirtualStart = base + 0x1FF000U,
                    .PageCount = 1U,
                    .Purpose = Amd64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
            },
    };
}

[[nodiscard]] constexpr PhysicalPageRange ValidReservedPages() noexcept
{
    return PhysicalPageRange{
        .PhysicalStart = ReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 2U,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr BootstrapMappingPlanSummary SummarySentinel() noexcept
{
    return BootstrapMappingPlanSummary{
        .ReservedPhysicalStart = 0x1122'3344'5566'7788ULL,
        .StackPhysicalStart = 0x8877'6655'4433'2211ULL,
        .PageTableStoragePhysicalStart = 0xAA55'AA55'AA55'AA55ULL,
        .DescriptorTableStoragePhysicalStart = 0x55AA'55AA'55AA'55AAULL,
        .MappingCount = 0x1234U,
        .Reserved16 = 0x5678U,
        .Reserved32 = 0x89AB'CDEFU,
    };
}

void MoveImage(Amd64LoadedImageLayout& image, const Foundation::UInt64 newBase) noexcept
{
    const Foundation::UInt64 oldBase = image.ImageBase;
    image.ImageBase = newBase;
    for (Foundation::Size index{0U}; index < Platform::Amd64::Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        image.Sections[index].VirtualStart = newBase + (image.Sections[index].VirtualStart - oldBase);
    }
}

void ConfigureDynamicBootInfo(const Amd64LoadedImageLayout& image, const PhysicalPageRange& reserved) noexcept
{
    g_dynamicBootInfo.Signature = Boot::BootInfoSignature;
    g_dynamicBootInfo.Version = Boot::BootInfoVersion;
    g_dynamicBootInfo.SizeBytes = sizeof(BootInfo);
    g_dynamicBootInfo.MemoryRegionCount = 2U;
    g_dynamicBootInfo.FirmwareDescriptorVersion = 1U;
    g_dynamicBootInfo.FirmwareDescriptorSize = 40U;
    g_dynamicBootInfo.Reserved = 0U;
    g_dynamicBootInfo.FirmwareMemoryMapSizeBytes = 80U;
    const PhysicalMemoryRegion imageRegion{
        .PhysicalStart = image.ImageBase,
        .VirtualStart = 0U,
        .PageCount = image.ImageSize / Kernel::Memory::MappingPageSizeBytes,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    const PhysicalMemoryRegion reservedRegion{
        .PhysicalStart = reserved.PhysicalStart,
        .VirtualStart = 0U,
        .PageCount = reserved.PageCount,
        .Attributes = reserved.Attributes,
        .FirmwareType = reserved.FirmwareType,
        .Kind = PhysicalMemoryKind::Loader,
    };
    if (reserved.PhysicalStart < image.ImageBase)
    {
        g_dynamicBootInfo.MemoryRegions[0U] = reservedRegion;
        g_dynamicBootInfo.MemoryRegions[1U] = imageRegion;
    }
    else
    {
        g_dynamicBootInfo.MemoryRegions[0U] = imageRegion;
        g_dynamicBootInfo.MemoryRegions[1U] = reservedRegion;
    }
}

[[nodiscard]] constexpr bool SummariesEqual(const BootstrapMappingPlanSummary& first,
                                            const BootstrapMappingPlanSummary& second) noexcept
{
    return (first.ReservedPhysicalStart == second.ReservedPhysicalStart) &&
           (first.StackPhysicalStart == second.StackPhysicalStart) &&
           (first.PageTableStoragePhysicalStart == second.PageTableStoragePhysicalStart) &&
           (first.DescriptorTableStoragePhysicalStart == second.DescriptorTableStoragePhysicalStart) &&
           (first.MappingCount == second.MappingCount) && (first.Reserved16 == second.Reserved16) &&
           (first.Reserved32 == second.Reserved32);
}

[[nodiscard]] constexpr bool ImageLayoutsEqual(const Amd64LoadedImageLayout& first,
                                               const Amd64LoadedImageLayout& second) noexcept
{
    if ((first.ImageBase != second.ImageBase) || (first.ImageSize != second.ImageSize))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Platform::Amd64::Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        const Amd64LoadedImageSection& firstSection = first.Sections[index];
        const Amd64LoadedImageSection& secondSection = second.Sections[index];
        if ((firstSection.VirtualStart != secondSection.VirtualStart) ||
            (firstSection.PageCount != secondSection.PageCount) || (firstSection.Purpose != secondSection.Purpose) ||
            (firstSection.Reserved != secondSection.Reserved))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool PhysicalPageRangesEqual(const PhysicalPageRange& first,
                                                     const PhysicalPageRange& second) noexcept
{
    return (first.PhysicalStart == second.PhysicalStart) && (first.PageCount == second.PageCount) &&
           (first.Attributes == second.Attributes) && (first.FirmwareType == second.FirmwareType) &&
           (first.Reserved == second.Reserved);
}

[[nodiscard]] bool BootInfosEqual(const BootInfo& first, const BootInfo& second) noexcept
{
    const auto* const firstBytes = reinterpret_cast<const Foundation::UInt8*>(&first);
    const auto* const secondBytes = reinterpret_cast<const Foundation::UInt8*>(&second);
    for (Foundation::Size index{0U}; index < sizeof(BootInfo); ++index)
    {
        if (firstBytes[index] != secondBytes[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsExactMapping(const InitialMappingPlan& plan, const Foundation::Size index,
                                  const Foundation::UInt64 start, const Foundation::UInt64 pages,
                                  const InitialMappingOwner owner, const bool writable, const bool executable) noexcept
{
    const auto result = plan.GetMapping(index);
    if (!result.HasValue())
    {
        return false;
    }
    const InitialMapping mapping = result.GetValue();
    return (mapping.VirtualStart.GetValue() == start) && (mapping.PhysicalStart.GetValue() == start) &&
           (mapping.Pages.GetValue() == pages) && (mapping.Owner == owner) &&
           (mapping.CachePolicy == Kernel::Memory::MemoryCachePolicy::WriteBack) &&
           mapping.Permissions.Contains(MappingPermission::Read) &&
           (mapping.Permissions.Contains(MappingPermission::Write) == writable) &&
           (mapping.Permissions.Contains(MappingPermission::Execute) == executable) &&
           !mapping.Permissions.Contains(MappingPermission::Global) && (mapping.Reserved == 0U);
}

[[nodiscard]] bool PlansEqual(const InitialMappingPlan& first, const InitialMappingPlan& second) noexcept
{
    if ((first.GetMappingCount() != second.GetMappingCount()) || (first.IsSealed() != second.IsSealed()))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < first.GetMappingCount(); ++index)
    {
        const auto firstMapping = first.GetMapping(index);
        const auto secondMapping = second.GetMapping(index);
        if (!firstMapping.HasValue() || !secondMapping.HasValue())
        {
            return false;
        }
        const InitialMapping firstValue = firstMapping.GetValue();
        const InitialMapping secondValue = secondMapping.GetValue();
        if ((firstValue.VirtualStart.GetValue() != secondValue.VirtualStart.GetValue()) ||
            (firstValue.PhysicalStart.GetValue() != secondValue.PhysicalStart.GetValue()) ||
            (firstValue.Pages.GetValue() != secondValue.Pages.GetValue()) ||
            (firstValue.Permissions.GetBits() != secondValue.Permissions.GetBits()) ||
            (firstValue.CachePolicy != secondValue.CachePolicy) || (firstValue.Owner != secondValue.Owner) ||
            (firstValue.Reserved != secondValue.Reserved))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool PlanOverlapsPhysicalRange(const InitialMappingPlan& plan, const Foundation::UInt64 start,
                                             const Foundation::UInt64 pages) noexcept
{
    const Foundation::UInt64 end = start + (pages * Kernel::Memory::MappingPageSizeBytes);
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto mappingResult = plan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return true;
        }
        const InitialMapping mapping = mappingResult.GetValue();
        const Foundation::UInt64 mappingStart = mapping.PhysicalStart.GetValue();
        const Foundation::UInt64 mappingEnd =
            mappingStart + (mapping.Pages.GetValue() * Kernel::Memory::MappingPageSizeBytes);
        if ((mappingStart < end) && (start < mappingEnd))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool FailsWithoutMutation(const Amd64LoadedImageLayout& image, const BootInfo& bootInfo,
                                        const PhysicalPageRange& reserved, const BootstrapMappingPlanError expected,
                                        InitialMappingPlan& output, BootstrapMappingPlanSummary& summary) noexcept
{
    const Amd64LoadedImageLayout expectedImage = image;
    const PhysicalPageRange expectedReserved = reserved;
    const BootstrapMappingPlanSummary expectedSummary = summary;
    g_bootInfoSnapshot = bootInfo;
    const BootstrapMappingPlanError error =
        BootstrapMappingPlanBuilder::Build(image, bootInfo, reserved, output, summary);
    return (error == expected) && (output.GetMappingCount() == 0U) && !output.IsSealed() &&
           SummariesEqual(summary, expectedSummary) && ImageLayoutsEqual(image, expectedImage) &&
           (reserved.PhysicalStart == expectedReserved.PhysicalStart) &&
           (reserved.PageCount == expectedReserved.PageCount) && (reserved.Attributes == expectedReserved.Attributes) &&
           (reserved.FirmwareType == expectedReserved.FirmwareType) &&
           (reserved.Reserved == expectedReserved.Reserved) && BootInfosEqual(bootInfo, g_bootInfoSnapshot);
}

[[nodiscard]] bool FailsWithoutMutation(const Amd64LoadedImageLayout& image, const PhysicalPageRange& reserved,
                                        const BootstrapMappingPlanError expected, InitialMappingPlan& output,
                                        BootstrapMappingPlanSummary& summary) noexcept
{
    return FailsWithoutMutation(image, g_bootInfo, reserved, expected, output, summary);
}

[[nodiscard]] constexpr PagingCapabilities ValidCapabilities() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 52U,
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = false,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
}

void TestExactPlan(TestContext& runner) noexcept
{
    Amd64LoadedImageLayout image = ValidImage();
    PhysicalPageRange reserved = ValidReservedPages();
    const Amd64LoadedImageLayout expectedImage = image;
    const PhysicalPageRange expectedReserved = reserved;
    g_bootInfoSnapshot = g_bootInfo;
    BootstrapMappingPlanSummary summary = SummarySentinel();
    const BootstrapMappingPlanError error =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, g_output, summary);
    ASTERIA_EXPECT(runner, "the canonical image and 23-page reservation build one sealed plan",
                   error == BootstrapMappingPlanError::None, summary.ReservedPhysicalStart == ReservedBase,
                   summary.StackPhysicalStart == (ReservedBase + 0x1000U),
                   summary.PageTableStoragePhysicalStart == (ReservedBase + 0x5000U),
                   summary.DescriptorTableStoragePhysicalStart == (ReservedBase + 0x15000U),
                   GetBootstrapGuardPhysicalStart(summary) == ReservedBase,
                   GetBootstrapStackPhysicalEnd(summary) == (ReservedBase + 0x5000U),
                   GetBootstrapPageTableStoragePhysicalEnd(summary) == (ReservedBase + 0x15000U),
                   GetBootstrapDescriptorTableStoragePhysicalEnd(summary) == (ReservedBase + 0x17000U),
                   GetBootstrapReservedPhysicalEnd(summary) == (ReservedBase + 0x17000U),
                   summary.MappingCount == BootstrapInitialMappingCount, summary.Reserved16 == 0U,
                   summary.Reserved32 == 0U, g_output.IsSealed(),
                   g_output.GetMappingCount() == BootstrapInitialMappingCount, ImageLayoutsEqual(image, expectedImage),
                   PhysicalPageRangesEqual(reserved, expectedReserved), BootInfosEqual(g_bootInfo, g_bootInfoSnapshot));

    ASTERIA_EXPECT(
        runner, "all mappings retain exact identity ranges, owners, and W^X permissions",
        IsExactMapping(g_output, 0U, ReservedBase + 0x1000U, BootstrapStackPageCount, InitialMappingOwner::NucleusStack,
                       true, false),
        IsExactMapping(g_output, 1U, ReservedBase + 0x5000U, BootstrapPageTableStoragePageCount,
                       InitialMappingOwner::PageTable, true, false),
        IsExactMapping(g_output, 2U, ReservedBase + 0x15000U, BootstrapDescriptorTableStoragePageCount,
                       InitialMappingOwner::NucleusDescriptorTables, true, false),
        IsExactMapping(g_output, 3U, ImageBase + 0x1000U, 2U, InitialMappingOwner::NucleusCode, false, true),
        IsExactMapping(g_output, 4U, ImageBase + 0x3000U, 1U, InitialMappingOwner::NucleusReadOnlyData, false, false),
        IsExactMapping(g_output, 5U, ImageBase + 0x4000U, 1U, InitialMappingOwner::NucleusReadOnlyData, false, false),
        IsExactMapping(g_output, 6U, ImageBase + 0x5000U, 2U, InitialMappingOwner::NucleusWritableData, true, false),
        IsExactMapping(g_output, 7U, ImageBase + 0x7000U, 1U, InitialMappingOwner::NucleusReadOnlyData, false, false),
        IsExactMapping(g_output, 8U, ImageBase + 0x8000U, 1U, InitialMappingOwner::NucleusReadOnlyData, false, false));

    ASTERIA_EXPECT(
        runner, "the guard stays unmapped while stack, page tables, and descriptor tables are owned",
        !PlanOverlapsPhysicalRange(g_output, GetBootstrapGuardPhysicalStart(summary), BootstrapGuardPageCount),
        PlanOverlapsPhysicalRange(g_output, summary.StackPhysicalStart, BootstrapStackPageCount),
        PlanOverlapsPhysicalRange(g_output, summary.PageTableStoragePhysicalStart, BootstrapPageTableStoragePageCount),
        PlanOverlapsPhysicalRange(g_output, summary.DescriptorTableStoragePhysicalStart,
                                  BootstrapDescriptorTableStoragePageCount));

    const PageTableStorage storage{
        .PhysicalStart = Kernel::Memory::PhysicalAddress{summary.PageTableStoragePhysicalStart},
        .Entries = g_tableEntries,
        .PageCapacity = BootstrapPageTableStoragePageCount,
        .Reserved = 0U,
    };
    InitialPageTableBuildResult buildResult{
        .RootPhysicalAddress = Kernel::Memory::PhysicalAddress{0xDEAD'0000U},
        .UsedTablePages = 0x11U,
        .MappedPages = 0x22U,
        .Reserved = 0x33U,
    };
    const InitialPageTableBuildError buildError =
        InitialPageTableBuilder::Build(g_output, ValidCapabilities(), storage, buildResult);
    ASTERIA_EXPECT(runner, "the native encoder fits the fixed storage and reports exact demand",
                   buildError == InitialPageTableBuildError::None,
                   buildResult.RootPhysicalAddress.GetValue() == summary.PageTableStoragePhysicalStart,
                   buildResult.UsedTablePages == 5U,
                   buildResult.UsedTablePages <= BootstrapMaximumRequiredPageTablePages, buildResult.MappedPages == 31U,
                   buildResult.Reserved == 0U);

    BootstrapMappingPlanSummary repeatedSummary = SummarySentinel();
    const BootstrapMappingPlanError repeated =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, g_secondOutput, repeatedSummary);
    ASTERIA_EXPECT(runner, "equal inputs produce byte-equivalent semantic plans",
                   repeated == BootstrapMappingPlanError::None, SummariesEqual(summary, repeatedSummary),
                   PlansEqual(g_output, g_secondOutput));

    BootstrapMappingPlanSummary validatedSummary = SummarySentinel();
    const BootstrapMappingPlanError validated =
        BootstrapMappingPlanBuilder::Validate(image, g_bootInfo, reserved, validatedSummary);
    ASTERIA_EXPECT(runner, "validation reproduces the exact summary without constructing a plan",
                   validated == BootstrapMappingPlanError::None, SummariesEqual(summary, validatedSummary),
                   ImageLayoutsEqual(image, expectedImage), PhysicalPageRangesEqual(reserved, expectedReserved),
                   BootInfosEqual(g_bootInfo, g_bootInfoSnapshot));

    BootInfo allocatorBootInfo = g_bootInfo;
    allocatorBootInfo.MemoryRegionCount = 3U;
    allocatorBootInfo.FirmwareMemoryMapSizeBytes = 120U;
    allocatorBootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = 0x1000'0000U,
        .VirtualStart = 0U,
        .PageCount = 1U,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    Kernel::Memory::BootstrapPageAllocator allocator;
    const auto allocatorError = allocator.Initialize(allocatorBootInfo);
    const auto reservation = allocator.Allocate(1U, 1U);
    ASTERIA_EXPECT(runner, "the allocator never reissues the LoaderData bootstrap reservation",
                   allocatorError == Kernel::Memory::BootstrapPageAllocatorError::None, reservation.HasValue(),
                   reservation.GetValue().PhysicalStart == 0x1000'0000U,
                   reservation.GetValue().PhysicalStart != ReservedBase);
}

void TestInputFailures(TestContext& runner) noexcept
{
    Amd64LoadedImageLayout image = ValidImage();
    PhysicalPageRange reserved = ValidReservedPages();
    BootstrapMappingPlanSummary summary = SummarySentinel();
    const InitialMappingPlanError dirtyError = g_dirtyOutput.AddMapping(InitialMapping{
        .VirtualStart = Kernel::Memory::VirtualAddress{0x2000U},
        .PhysicalStart = Kernel::Memory::PhysicalAddress{0x2000U},
        .Pages = Kernel::Memory::PageCount{1U},
        .Permissions = Kernel::Memory::MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
        .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
        .Owner = InitialMappingOwner::NucleusCode,
        .Reserved = 0U,
    });
    const BootstrapMappingPlanSummary expectedSummary = summary;
    const BootstrapMappingPlanError dirty =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, g_dirtyOutput, summary);
    ASTERIA_EXPECT(runner, "a nonempty plan is rejected without further output mutation",
                   dirtyError == InitialMappingPlanError::None, dirty == BootstrapMappingPlanError::OutputNotEmpty,
                   g_dirtyOutput.GetMappingCount() == 1U, !g_dirtyOutput.IsSealed(),
                   SummariesEqual(summary, expectedSummary));

    image.Sections[1U].VirtualStart += 0x1000U;
    ASTERIA_EXPECT(
        runner, "invalid image geometry leaves both outputs and every input unchanged",
        FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidImageLayout, g_failureOutput, summary));

    image = ValidImage();
    bool allPurposesRejected{true};
    for (Foundation::Size index{0U}; index < Platform::Amd64::Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        const Amd64LoadedImageSectionPurpose saved = image.Sections[index].Purpose;
        image.Sections[index].Purpose = saved == Amd64LoadedImageSectionPurpose::Code
                                            ? Amd64LoadedImageSectionPurpose::WritableData
                                            : Amd64LoadedImageSectionPurpose::Code;
        allPurposesRejected =
            allPurposesRejected && FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidImageLayout,
                                                        g_failureOutput, summary);
        image.Sections[index].Purpose = static_cast<Amd64LoadedImageSectionPurpose>(0xFFU);
        allPurposesRejected =
            allPurposesRejected && FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidImageLayout,
                                                        g_failureOutput, summary);
        image.Sections[index].Purpose = saved;
    }
    ASTERIA_EXPECT(runner, "every misplaced and unknown section purpose is rejected atomically", allPurposesRejected);

    reserved.PageCount = BootstrapReservedPageCount - 1U;
    const bool wrongCount = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                                 g_failureOutput, summary);
    reserved = ValidReservedPages();
    reserved.PhysicalStart = 0U;
    const bool pageZero = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                               g_failureOutput, summary);
    reserved = ValidReservedPages();
    reserved.PhysicalStart =
        LowerCanonicalAddressLimit - ((BootstrapReservedPageCount - 1U) * Kernel::Memory::MappingPageSizeBytes);
    const bool noncanonicalEnd = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                                      g_failureOutput, summary);
    ASTERIA_EXPECT(runner, "reserved count, page-zero exclusion, and canonical extent are exact", wrongCount, pageZero,
                   noncanonicalEnd);

    reserved = ValidReservedPages();
    reserved.PhysicalStart = ImageBase;
    ASTERIA_EXPECT(runner, "reserved bootstrap pages may not overlap the image header or sections",
                   FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::ReservedPagesOverlapImage,
                                        g_failureOutput, summary));
}

void TestValidationMatrix(TestContext& runner) noexcept
{
    Amd64LoadedImageLayout image = ValidImage();
    const PhysicalPageRange validReserved = ValidReservedPages();
    BootstrapMappingPlanSummary summary = SummarySentinel();

    MoveImage(image, 0U);
    const bool pageZeroImage = FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidImageLayout,
                                                    g_failureOutput, summary);

    image = ValidImage();
    image.ImageSize = Platform::Amd64::Firmware::MaximumAmd64LoadedImageBytes + 0x1000U;
    image.Sections[3U].PageCount = 506U;
    image.Sections[4U].VirtualStart = ImageBase + 0x1FF000U;
    image.Sections[5U].VirtualStart = ImageBase + 0x200000U;
    const bool oversizedImage = FailsWithoutMutation(
        image, validReserved, BootstrapMappingPlanError::InvalidImageLayout, g_failureOutput, summary);

    image = ValidImage();
    ++image.ImageSize;
    const bool unalignedImageSize = FailsWithoutMutation(
        image, validReserved, BootstrapMappingPlanError::InvalidImageLayout, g_failureOutput, summary);

    image = ValidImage();
    MoveImage(image, LowerCanonicalAddressLimit - image.ImageSize + 0x1000U);
    const bool noncanonicalImage = FailsWithoutMutation(
        image, validReserved, BootstrapMappingPlanError::InvalidImageLayout, g_failureOutput, summary);

    image = ValidImage();
    bool allSectionReservedFieldsRejected{true};
    for (Foundation::Size index{0U}; index < Platform::Amd64::Firmware::Amd64LoadedImageSectionCount; ++index)
    {
        image.Sections[index].Reserved = 1U;
        allSectionReservedFieldsRejected =
            allSectionReservedFieldsRejected &&
            FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidImageLayout, g_failureOutput,
                                 summary);
        image.Sections[index].Reserved = 0U;
    }

    image.Sections[0U].PageCount = 0U;
    image.Sections[1U].VirtualStart = ImageBase + 0x1000U;
    image.Sections[2U].VirtualStart = ImageBase + 0x2000U;
    image.Sections[3U].VirtualStart = ImageBase + 0x3000U;
    image.Sections[3U].PageCount = 4U;
    const bool zeroSection = FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidImageLayout,
                                                  g_failureOutput, summary);

    image = ValidImage();
    image.ImageSize += 0x1000U;
    const bool incompleteSectionExtent = FailsWithoutMutation(
        image, validReserved, BootstrapMappingPlanError::InvalidImageLayout, g_failureOutput, summary);
    ASTERIA_EXPECT(runner, "image bounds, section extents, and every reserved field are exact", pageZeroImage,
                   oversizedImage, unalignedImageSize, noncanonicalImage, allSectionReservedFieldsRejected, zeroSection,
                   incompleteSectionExtent);

    image = ValidImage();
    const Foundation::UInt64 savedSignature = g_bootInfo.Signature;
    const Foundation::UInt32 savedVersion = g_bootInfo.Version;
    const Foundation::UInt32 savedSizeBytes = g_bootInfo.SizeBytes;
    const Foundation::UInt32 savedCount = g_bootInfo.MemoryRegionCount;
    const Foundation::UInt32 savedDescriptorVersion = g_bootInfo.FirmwareDescriptorVersion;
    const Foundation::UInt32 savedDescriptorSize = g_bootInfo.FirmwareDescriptorSize;
    const Foundation::UInt32 savedReserved = g_bootInfo.Reserved;
    const Foundation::UInt64 savedMapBytes = g_bootInfo.FirmwareMemoryMapSizeBytes;
    bool allHeaderFieldsRejected{true};

    ++g_bootInfo.Signature;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.Signature = savedSignature;
    ++g_bootInfo.Version;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.Version = savedVersion;
    --g_bootInfo.SizeBytes;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.SizeBytes = savedSizeBytes;
    BootInfo oversizedBootInfo = g_bootInfo;
    for (Foundation::Size index{0U}; index < Boot::MaximumMemoryRegions; ++index)
    {
        oversizedBootInfo.MemoryRegions[index] = PhysicalMemoryRegion{
            .PhysicalStart = (static_cast<Foundation::UInt64>(index) + 1U) * Kernel::Memory::MappingPageSizeBytes,
            .VirtualStart = 0U,
            .PageCount = 1U,
            .Attributes = 0U,
            .FirmwareType = 8U,
            .Kind = PhysicalMemoryKind::Unusable,
        };
    }
    oversizedBootInfo.MemoryRegionCount = static_cast<Foundation::UInt32>(Boot::MaximumMemoryRegions + 1U);
    oversizedBootInfo.FirmwareMemoryMapSizeBytes =
        static_cast<Foundation::UInt64>(oversizedBootInfo.MemoryRegionCount) * oversizedBootInfo.FirmwareDescriptorSize;
    allHeaderFieldsRejected =
        allHeaderFieldsRejected &&
        FailsWithoutMutation(image, oversizedBootInfo, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                             g_failureOutput, summary);
    ++g_bootInfo.FirmwareDescriptorVersion;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.FirmwareDescriptorVersion = savedDescriptorVersion;
    g_bootInfo.FirmwareDescriptorSize = 39U;
    g_bootInfo.FirmwareMemoryMapSizeBytes =
        static_cast<Foundation::UInt64>(g_bootInfo.MemoryRegionCount) * g_bootInfo.FirmwareDescriptorSize;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.FirmwareDescriptorSize = savedDescriptorSize;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.Reserved = 1U;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.Reserved = savedReserved;
    ++g_bootInfo.FirmwareMemoryMapSizeBytes;
    allHeaderFieldsRejected = allHeaderFieldsRejected &&
                              FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                   g_failureOutput, summary);
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    ASTERIA_EXPECT(runner, "every retained BootInfo header field is revalidated atomically", allHeaderFieldsRejected);

    const PhysicalMemoryRegion savedThird = g_bootInfo.MemoryRegions[2U];
    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = 0x1000'0000U,
        .VirtualStart = 1U,
        .PageCount = 1U,
        .Attributes = 0U,
        .FirmwareType = 8U,
        .Kind = PhysicalMemoryKind::Unusable,
    };
    const bool invalidVirtual = FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                     g_failureOutput, summary);
    g_bootInfo.MemoryRegions[2U].VirtualStart = 0U;
    g_bootInfo.MemoryRegions[2U].FirmwareType = 16U;
    g_bootInfo.MemoryRegions[2U].Kind = PhysicalMemoryKind::Reserved;
    const bool invalidType = FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                  g_failureOutput, summary);
    g_bootInfo.MemoryRegions[2U].FirmwareType = 8U;
    const bool mismatchedKind = FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                     g_failureOutput, summary);
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = 0x1000'0000U,
        .VirtualStart = 0U,
        .PageCount = 1U,
        .Attributes = WriteBackAttribute | (Foundation::UInt64{1U} << 63U),
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    const bool invalidRuntimeConventional = FailsWithoutMutation(
        image, validReserved, BootstrapMappingPlanError::InvalidBootInfo, g_failureOutput, summary);
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase,
        .VirtualStart = 0U,
        .PageCount = 1U,
        .Attributes = 0U,
        .FirmwareType = 8U,
        .Kind = PhysicalMemoryKind::Unusable,
    };
    const bool invalidOrdering = FailsWithoutMutation(image, validReserved, BootstrapMappingPlanError::InvalidBootInfo,
                                                      g_failureOutput, summary);
    ASTERIA_EXPECT(runner, "unrelated descriptors receive complete shape and ordering validation", invalidVirtual,
                   invalidType, mismatchedKind, invalidRuntimeConventional, invalidOrdering);
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[2U] = savedThird;

    const PhysicalMemoryRegion savedImageRegion = g_bootInfo.MemoryRegions[1U];
    const PhysicalMemoryRegion savedReservedRegion = g_bootInfo.MemoryRegions[0U];
    PhysicalPageRange reserved = validReserved;
    g_bootInfo.MemoryRegions[1U].Attributes = WriteBackAttribute | (Foundation::UInt64{1U} << 63U);
    const bool runtimeImage = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::InvalidImageMemoryProvenance, g_failureOutput, summary);
    g_bootInfo.MemoryRegions[1U] = savedImageRegion;
    reserved.Reserved = 1U;
    const bool receiptReserved = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                                      g_failureOutput, summary);
    reserved = validReserved;
    reserved.FirmwareType = 6U;
    const bool receiptType = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                                  g_failureOutput, summary);
    reserved = validReserved;
    reserved.Attributes = WriteBackAttribute | (Foundation::UInt64{1U} << 63U);
    const bool receiptRuntime = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                                     g_failureOutput, summary);
    g_bootInfo.MemoryRegions[0U].Attributes = reserved.Attributes;
    const bool matchingRuntimeReservation = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::InvalidReservedPages, g_failureOutput, summary);
    ASTERIA_EXPECT(runner, "runtime ownership and every allocation-receipt field are fail-closed", runtimeImage,
                   receiptReserved, receiptType, receiptRuntime, matchingRuntimeReservation);
    g_bootInfo.MemoryRegions[0U] = savedReservedRegion;

    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 40U;
    g_bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = 0U,
        .VirtualStart = 0U,
        .PageCount = Foundation::UInt64{1U} << 52U,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 1U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    ASTERIA_EXPECT(runner, "the architecturally full-width descriptor is shape-valid without wrap",
                   FailsWithoutMutation(image, g_bootInfo, validReserved,
                                        BootstrapMappingPlanError::InvalidReservedMemoryProvenance, g_failureOutput,
                                        summary));
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[0U] = savedReservedRegion;
    g_bootInfo.MemoryRegions[1U] = savedImageRegion;
}

void TestHalfOpenAndCanonicalBoundaries(TestContext& runner) noexcept
{
    Amd64LoadedImageLayout image = ValidImage();
    const Amd64LoadedImageLayout expectedImage = image;
    BootstrapMappingPlanSummary leftSummary = SummarySentinel();
    PhysicalPageRange reserved = ValidReservedPages();
    reserved.PhysicalStart = ImageBase - (BootstrapReservedPageCount * Kernel::Memory::MappingPageSizeBytes);
    ConfigureDynamicBootInfo(image, reserved);
    const PhysicalPageRange expectedLeftReserved = reserved;
    g_bootInfoSnapshot = g_dynamicBootInfo;
    const BootstrapMappingPlanError left =
        BootstrapMappingPlanBuilder::Build(image, g_dynamicBootInfo, reserved, g_leftAdjacentOutput, leftSummary);
    const bool leftAdjacent = (left == BootstrapMappingPlanError::None) && g_leftAdjacentOutput.IsSealed() &&
                              (GetBootstrapReservedPhysicalEnd(leftSummary) == ImageBase) &&
                              ImageLayoutsEqual(image, expectedImage) &&
                              PhysicalPageRangesEqual(reserved, expectedLeftReserved) &&
                              BootInfosEqual(g_dynamicBootInfo, g_bootInfoSnapshot);

    BootstrapMappingPlanSummary rightSummary = SummarySentinel();
    reserved.PhysicalStart = ImageBase + image.ImageSize;
    ConfigureDynamicBootInfo(image, reserved);
    const PhysicalPageRange expectedRightReserved = reserved;
    g_bootInfoSnapshot = g_dynamicBootInfo;
    const BootstrapMappingPlanError right =
        BootstrapMappingPlanBuilder::Build(image, g_dynamicBootInfo, reserved, g_rightAdjacentOutput, rightSummary);
    const bool rightAdjacent = (right == BootstrapMappingPlanError::None) && g_rightAdjacentOutput.IsSealed() &&
                               (rightSummary.ReservedPhysicalStart == (ImageBase + image.ImageSize)) &&
                               ImageLayoutsEqual(image, expectedImage) &&
                               PhysicalPageRangesEqual(reserved, expectedRightReserved) &&
                               BootInfosEqual(g_dynamicBootInfo, g_bootInfoSnapshot);
    ASTERIA_EXPECT(runner, "both exact half-open image boundaries remain non-overlapping", leftAdjacent, rightAdjacent);

    BootstrapMappingPlanSummary failureSummary = SummarySentinel();
    reserved = ValidReservedPages();
    reserved.PhysicalStart = ImageBase - ((BootstrapReservedPageCount - 1U) * Kernel::Memory::MappingPageSizeBytes);
    const bool overlapsFromBelow = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::ReservedPagesOverlapImage, g_failureOutput, failureSummary);
    reserved.PhysicalStart = ImageBase + 0x1000U;
    const bool overlapsFromInside = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::ReservedPagesOverlapImage, g_failureOutput, failureSummary);
    ASTERIA_EXPECT(runner, "partial overlap is rejected from both range directions", overlapsFromBelow,
                   overlapsFromInside);

    reserved = ValidReservedPages();
    reserved.PhysicalStart =
        LowerCanonicalAddressLimit - (BootstrapReservedPageCount * Kernel::Memory::MappingPageSizeBytes);
    ConfigureDynamicBootInfo(image, reserved);
    const PhysicalPageRange expectedBoundaryReserved = reserved;
    BootstrapMappingPlanSummary boundarySummary = SummarySentinel();
    g_bootInfoSnapshot = g_dynamicBootInfo;
    const BootstrapMappingPlanError boundary =
        BootstrapMappingPlanBuilder::Build(image, g_dynamicBootInfo, reserved, g_boundaryOutput, boundarySummary);
    ASTERIA_EXPECT(runner, "a reservation ending exactly at the lower-canonical limit succeeds",
                   boundary == BootstrapMappingPlanError::None, g_boundaryOutput.IsSealed(),
                   GetBootstrapReservedPhysicalEnd(boundarySummary) == LowerCanonicalAddressLimit,
                   ImageLayoutsEqual(image, expectedImage), PhysicalPageRangesEqual(reserved, expectedBoundaryReserved),
                   BootInfosEqual(g_dynamicBootInfo, g_bootInfoSnapshot));
}

void TestMemoryProvenance(TestContext& runner) noexcept
{
    const PhysicalMemoryRegion savedReserved = g_bootInfo.MemoryRegions[0U];
    const PhysicalMemoryRegion savedImage = g_bootInfo.MemoryRegions[1U];
    const Foundation::UInt32 savedCount = g_bootInfo.MemoryRegionCount;
    const Foundation::UInt64 savedMapBytes = g_bootInfo.FirmwareMemoryMapSizeBytes;
    const Amd64LoadedImageLayout image = ValidImage();
    PhysicalPageRange reserved = ValidReservedPages();
    BootstrapMappingPlanSummary summary = SummarySentinel();

    g_bootInfo.MemoryRegionCount = 0U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 0U;
    ASTERIA_EXPECT(
        runner, "an invalid final BootInfo is rejected before provenance lookup",
        FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidBootInfo, g_provenanceOutput, summary));
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;

    g_bootInfo.MemoryRegions[1U].Attributes = 0x1U;
    const bool uncachedImage = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::InvalidImageMemoryProvenance, g_provenanceOutput, summary);
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[1U].FirmwareType = 7U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::Conventional;
    const bool freeableImage = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::InvalidImageMemoryProvenance, g_provenanceOutput, summary);
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[1U].PhysicalStart += 0x1000U;
    --g_bootInfo.MemoryRegions[1U].PageCount;
    const bool missingHeader = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::InvalidImageMemoryProvenance, g_provenanceOutput, summary);
    ASTERIA_EXPECT(runner, "the complete image requires write-back, nonfreeable Loader provenance", uncachedImage,
                   freeableImage, missingHeader);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    reserved.Attributes = 0x1U;
    g_bootInfo.MemoryRegions[0U].Attributes = 0x1U;
    const bool uncachedReserved = FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedPages,
                                                       g_provenanceOutput, summary);
    reserved = ValidReservedPages();
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    reserved.Attributes |= ReadOnlyAttribute;
    g_bootInfo.MemoryRegions[0U].Attributes = reserved.Attributes;
    BootstrapMappingPlanSummary readOnlySummary = SummarySentinel();
    const BootstrapMappingPlanError readOnlyReserved =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, g_attributeOutput, readOnlySummary);
    reserved = ValidReservedPages();
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[0U].Attributes = 0x9U;
    const bool receiptMismatch = FailsWithoutMutation(
        image, reserved, BootstrapMappingPlanError::InvalidReservedMemoryProvenance, g_provenanceOutput, summary);
    ASTERIA_EXPECT(runner, "reserved pages require write-back and exact allocation provenance", uncachedReserved,
                   readOnlyReserved == BootstrapMappingPlanError::None, g_attributeOutput.IsSealed(), receiptMismatch);
    g_bootInfo.MemoryRegions[0U] = savedReserved;

    const PhysicalMemoryRegion savedThird = g_bootInfo.MemoryRegions[2U];
    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[0U].PageCount = 5U;
    g_bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ReservedBase + 0x5000U,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount - 5U,
        .Attributes = 0x1U,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    g_bootInfo.MemoryRegions[2U] = savedImage;
    ASTERIA_EXPECT(runner, "every guard, stack, and table page needs matching provenance",
                   FailsWithoutMutation(image, reserved, BootstrapMappingPlanError::InvalidReservedMemoryProvenance,
                                        g_provenanceOutput, summary));
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[2U] = savedThird;

    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase,
        .VirtualStart = 0U,
        .PageCount = 3U,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 1U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase + 0x3000U,
        .VirtualStart = 0U,
        .PageCount = 7U,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    BootstrapMappingPlanSummary splitSummary = SummarySentinel();
    const BootstrapMappingPlanError split =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, g_loaderOutput, splitSummary);
    ASTERIA_EXPECT(runner, "adjacent write-back Loader descriptors may jointly cover the image",
                   split == BootstrapMappingPlanError::None, g_loaderOutput.IsSealed(),
                   g_loaderOutput.GetMappingCount() == BootstrapInitialMappingCount);

    g_bootInfo.MemoryRegionCount = 4U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 160U;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[0U].Attributes = WriteBackAttribute | ExecuteProtectAttribute;
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[1U].Attributes = WriteBackAttribute | ReadOnlyAttribute;
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = 0x1000'0000U,
        .VirtualStart = 0U,
        .PageCount = 1U,
        .Attributes = WriteBackAttribute | RuntimeAttribute,
        .FirmwareType = 5U,
        .Kind = PhysicalMemoryKind::RuntimeServices,
    };
    g_bootInfo.MemoryRegions[3U] = PhysicalMemoryRegion{
        .PhysicalStart = 0x2000'0000U,
        .VirtualStart = 0U,
        .PageCount = 1U,
        .Attributes = RuntimeAttribute,
        .FirmwareType = 11U,
        .Kind = PhysicalMemoryKind::MemoryMappedIo,
    };
    reserved.Attributes = g_bootInfo.MemoryRegions[0U].Attributes;
    BootstrapMappingPlanSummary permittedSummary = SummarySentinel();
    const BootstrapMappingPlanError permitted =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, g_permittedProvenanceOutput, permittedSummary);
    ASTERIA_EXPECT(runner, "capability attributes and unrelated runtime descriptors remain valid",
                   permitted == BootstrapMappingPlanError::None, g_permittedProvenanceOutput.IsSealed(),
                   g_permittedProvenanceOutput.GetMappingCount() == BootstrapInitialMappingCount);

    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[2U] = savedThird;
}

void TestValidationOnly(TestContext& runner) noexcept
{
    const Amd64LoadedImageLayout image = ValidImage();
    const PhysicalPageRange reserved = ValidReservedPages();
    BootstrapMappingPlanSummary summary = SummarySentinel();
    const BootstrapMappingPlanSummary before = summary;

    g_dynamicBootInfo = g_bootInfo;
    g_dynamicBootInfo.MemoryRegions[0U].Attributes = WriteBackAttribute | ExecuteProtectAttribute;
    const bool changedAttributes =
        (BootstrapMappingPlanBuilder::Validate(image, g_dynamicBootInfo, reserved, summary) ==
         BootstrapMappingPlanError::InvalidReservedMemoryProvenance) &&
        SummariesEqual(summary, before);

    g_dynamicBootInfo = g_bootInfo;
    g_dynamicBootInfo.MemoryRegions[0U].FirmwareType = 7U;
    g_dynamicBootInfo.MemoryRegions[0U].Kind = PhysicalMemoryKind::Conventional;
    const bool changedType = (BootstrapMappingPlanBuilder::Validate(image, g_dynamicBootInfo, reserved, summary) ==
                              BootstrapMappingPlanError::InvalidReservedMemoryProvenance) &&
                             SummariesEqual(summary, before);

    g_dynamicBootInfo = g_bootInfo;
    g_dynamicBootInfo.MemoryRegions[0U].PhysicalStart += 0x1000U;
    --g_dynamicBootInfo.MemoryRegions[0U].PageCount;
    const bool changedStart = (BootstrapMappingPlanBuilder::Validate(image, g_dynamicBootInfo, reserved, summary) ==
                               BootstrapMappingPlanError::InvalidReservedMemoryProvenance) &&
                              SummariesEqual(summary, before);

    ASTERIA_EXPECT(runner, "validation rejects changed final-map provenance without publication", changedAttributes,
                   changedType, changedStart);
}

void TestMaximumPrefixDemand(TestContext& runner) noexcept
{
    const Amd64LoadedImageLayout image = PrefixBoundaryImage();
    const PhysicalPageRange reserved{
        .PhysicalStart = PrefixReservedBoundary - 0x2000U,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = WriteBackAttribute,
        .FirmwareType = 2U,
        .Reserved = 0U,
    };
    BootstrapMappingPlanSummary summary = SummarySentinel();
    const BootstrapMappingPlanError planError =
        BootstrapMappingPlanBuilder::Build(image, g_prefixBootInfo, reserved, g_prefixOutput, summary);
    const PageTableStorage storage{
        .PhysicalStart = Kernel::Memory::PhysicalAddress{summary.PageTableStoragePhysicalStart},
        .Entries = g_prefixTableEntries,
        .PageCapacity = BootstrapPageTableStoragePageCount,
        .Reserved = 0U,
    };
    InitialPageTableBuildResult result{};
    const InitialPageTableBuildError buildError =
        planError == BootstrapMappingPlanError::None
            ? InitialPageTableBuilder::Build(g_prefixOutput, ValidCapabilities(), storage, result)
            : InitialPageTableBuildError::InternalInvariantViolation;
    ASTERIA_EXPECT(runner, "two distant boundary-crossing envelopes consume the proven 13-page maximum",
                   planError == BootstrapMappingPlanError::None, buildError == InitialPageTableBuildError::None,
                   result.UsedTablePages == BootstrapMaximumRequiredPageTablePages,
                   result.UsedTablePages <= BootstrapPageTableStoragePageCount);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"BootstrapMappingPlanTests"};
    Asteria::Test::TestExactPlan(runner);
    Asteria::Test::TestInputFailures(runner);
    Asteria::Test::TestValidationMatrix(runner);
    Asteria::Test::TestHalfOpenAndCanonicalBoundaries(runner);
    Asteria::Test::TestMemoryProvenance(runner);
    Asteria::Test::TestValidationOnly(runner);
    Asteria::Test::TestMaximumPrefixDemand(runner);
    return runner.Finish();
}
