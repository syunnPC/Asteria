// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeMetadataLayout.hpp"

#include "Support/Ia64RuntimeMemoryReservation.hpp"
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
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageSection;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Memory::BootstrapInitialMappingCount;
using Platform::Ia64::Memory::BootstrapActivationGuardPageCount;
using Platform::Ia64::Memory::BootstrapInterruptVectorTablePageCount;
using Platform::Ia64::Memory::BootstrapMappingPlanBuilder;
using Platform::Ia64::Memory::BootstrapMappingPlanError;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::BootstrapRseBackingStorePageCount;
using Platform::Ia64::Memory::GetBootstrapActivationGuardPhysicalStart;
using Platform::Ia64::Memory::GetBootstrapInterruptVectorTablePhysicalStart;
using Platform::Ia64::Memory::GetBootstrapReservedPhysicalEnd;
using Platform::Ia64::Memory::GetBootstrapRseBackingStorePhysicalStart;
using Platform::Ia64::Memory::RuntimeMemoryReservationPageShifts;
using Platform::Ia64::Memory::RuntimeMemoryReservationPreferredBytes;
using Platform::Ia64::Memory::SelectRuntimeMemoryReservationPageShifts;
using TestSupport::Ia64FixtureReservationRegionPageCount;
using TestSupport::Ia64FixtureRuntimeMemoryPageCount;
using TestSupport::Ia64FixtureRuntimeMemoryPages;

constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt64 LargeImageBytes{0x240000U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
// The run-time memory reservation, one preferred page above the bootstrap
// reservation; the first final-map region covers both.
constexpr Foundation::UInt64 RuntimeMemoryBase{0x0200'0000U};
constexpr Foundation::UInt64 Ia64RegionOneStart{Foundation::UInt64{1U} << 61U};
constinit InitialMappingPlan g_output;
constinit InitialMappingPlan g_secondOutput;
constinit InitialMappingPlan g_dirtyOutput;
constinit InitialMappingPlan g_failureOutput;
constinit InitialMappingPlan g_provenanceOutput;
constinit InitialMappingPlan g_loaderOutput;
constinit InitialMappingPlan g_boundaryOutput;
constinit InitialMappingPlan g_boundaryFailureOutput;
constinit InitialMappingPlan g_regionBoundaryOutput;
constinit InitialMappingPlan g_alignedCodeOutput;
constinit InitialMappingPlan g_largeImageOutput;
constinit InitialMappingPlan g_runtimeRegionOutput;
constinit InitialMappingPlan g_runtimeFallbackOutput;
constinit InitialMappingPlan g_runtimeSmallestOutput;
constinit InitialMappingPlan g_consolidatedOutput;
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
                .PageCount = Ia64FixtureReservationRegionPageCount,
                .Attributes = 0x8U,
                .FirmwareType = 7U,
                .Kind = PhysicalMemoryKind::Conventional,
            },
            PhysicalMemoryRegion{
                .PhysicalStart = ImageBase,
                .VirtualStart = 0U,
                .PageCount = 9U,
                .Attributes = 0x8U,
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

[[nodiscard]] constexpr Ia64LoadedImageLayout ValidImage(const Foundation::UInt64 imageBase = ImageBase) noexcept
{
    return Ia64LoadedImageLayout{
        .ImageBase = imageBase,
        .ImageSize = 0x9000U,
        .Sections =
            {
                Ia64LoadedImageSection{
                    .VirtualStart = imageBase + 0x1000U,
                    .PageCount = 2U,
                    .Purpose = Ia64LoadedImageSectionPurpose::Code,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = imageBase + 0x3000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = imageBase + 0x4000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = imageBase + 0x5000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = imageBase + 0x6000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = imageBase + 0x7000U,
                    .PageCount = 2U,
                    .Purpose = Ia64LoadedImageSectionPurpose::WritableData,
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
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr PhysicalPageRange ValidRuntimeMemoryPages() noexcept
{
    return Ia64FixtureRuntimeMemoryPages(ReservedBase);
}

static_assert(Ia64FixtureRuntimeMemoryPages(ReservedBase).PhysicalStart == RuntimeMemoryBase);
static_assert(RuntimeMemoryBase + RuntimeMemoryReservationPreferredBytes <= ImageBase);

[[nodiscard]] constexpr Ia64LoadedImageLayout LargeImage() noexcept
{
    Ia64LoadedImageLayout image = ValidImage();
    image.ImageSize = LargeImageBytes;
    image.Sections[5U].PageCount = (LargeImageBytes - 0x7000U) / Kernel::Memory::MappingPageSizeBytes;
    return image;
}

[[nodiscard]] constexpr Ia64LoadedImageLayout AlignedCodeImage() noexcept
{
    Ia64LoadedImageLayout image = ValidImage();
    image.ImageSize += 0x7000U;
    for (Foundation::Size index{0U}; index < Platform::Ia64::Firmware::Ia64LoadedImageSectionCount; ++index)
    {
        image.Sections[index].VirtualStart += 0x7000U;
    }
    return image;
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
           mapping.Permissions.Contains(MappingPermission::Global) && (mapping.Reserved == 0U);
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

void TestExactPlan(TestContext& runner) noexcept
{
    const Ia64LoadedImageLayout image = ValidImage();
    const PhysicalPageRange reserved = ValidReservedPages();
    const auto result =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_output);
    ASTERIA_EXPECT(runner, "the canonical image and reserved range build one sealed plan", result.HasValue(),
                   result.GetValue().StackPhysicalStart == ReservedBase,
                   result.GetValue().TranslationStoragePhysicalStart == (ReservedBase + 0x8000U),
                   GetBootstrapActivationGuardPhysicalStart(result.GetValue()) == (ReservedBase + 0x9000U),
                   GetBootstrapInterruptVectorTablePhysicalStart(result.GetValue()) == (ReservedBase + 0x1'0000U),
                   GetBootstrapRseBackingStorePhysicalStart(result.GetValue()) == (ReservedBase + 0x1'8000U),
                   GetBootstrapReservedPhysicalEnd(result.GetValue()) == (ReservedBase + 0x2'0000U),
                   result.GetValue().MappingCount == BootstrapInitialMappingCount, result.GetValue().Reserved16 == 0U,
                   result.GetValue().Reserved32 == 0U, g_output.IsSealed(),
                   g_output.GetMappingCount() == BootstrapInitialMappingCount);

    ASTERIA_EXPECT(
        runner, "all eight mappings retain exact identity, ownership, W^X, and Global",
        IsExactMapping(g_output, 0U, ReservedBase, 8U, InitialMappingOwner::NucleusStack, true, false),
        IsExactMapping(g_output, 1U, ReservedBase + 0x8000U, 1U, InitialMappingOwner::PageTable, true, false),
        IsExactMapping(g_output, 2U, ReservedBase + 0x1'0000U, BootstrapInterruptVectorTablePageCount,
                       InitialMappingOwner::NucleusInterruptVectorTable, false, true),
        IsExactMapping(g_output, 3U, ReservedBase + 0x1'8000U, 8U, InitialMappingOwner::NucleusRegisterBackingStore,
                       true, false),
        IsExactMapping(g_output, 4U, RuntimeMemoryBase, Ia64FixtureRuntimeMemoryPageCount,
                       InitialMappingOwner::NucleusWritableData, true, false),
        IsExactMapping(g_output, 5U, ImageBase + 0x1000U, 2U, InitialMappingOwner::NucleusCode, false, true),
        IsExactMapping(g_output, 6U, ImageBase + 0x3000U, 3U, InitialMappingOwner::NucleusReadOnlyData, false, false),
        IsExactMapping(g_output, 7U, ImageBase + 0x6000U, 3U, InitialMappingOwner::NucleusWritableData, true, false));

    ASTERIA_EXPECT(runner, "the guard stays unmapped while IVT and RSE storage are mapped", result.HasValue(),
                   !PlanOverlapsPhysicalRange(g_output, GetBootstrapActivationGuardPhysicalStart(result.GetValue()),
                                              BootstrapActivationGuardPageCount),
                   PlanOverlapsPhysicalRange(g_output, GetBootstrapInterruptVectorTablePhysicalStart(result.GetValue()),
                                             BootstrapInterruptVectorTablePageCount),
                   PlanOverlapsPhysicalRange(g_output, GetBootstrapRseBackingStorePhysicalStart(result.GetValue()),
                                             BootstrapRseBackingStorePageCount));

    const auto repeated =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_secondOutput);
    ASTERIA_EXPECT(runner, "equal inputs produce byte-equivalent semantic plans", repeated.HasValue(),
                   PlansEqual(g_output, g_secondOutput));

    Kernel::Memory::BootstrapPageAllocator allocator;
    const auto allocatorError = allocator.Initialize(g_bootInfo);
    const auto reservedAllocation =
        allocator.Allocate(BootstrapReservedPageCount, Platform::Ia64::Memory::BootstrapReservedAlignmentPages);
    const auto runtimeAllocation =
        allocator.Allocate(Ia64FixtureRuntimeMemoryPageCount, Ia64FixtureRuntimeMemoryPageCount);
    constexpr Foundation::UInt64 RemainingPages{Ia64FixtureReservationRegionPageCount - BootstrapReservedPageCount -
                                                Ia64FixtureRuntimeMemoryPageCount};
    const Foundation::UInt64 freePages = allocator.GetFreePageCount();
    const auto remainingAllocation = allocator.Allocate(RemainingPages, 1U);
    const auto subsequentAllocation = allocator.Allocate(1U, 1U);
    ASTERIA_EXPECT(runner, "Loader-owned image pages never enter the allocator free table",
                   allocatorError == Kernel::Memory::BootstrapPageAllocatorError::None, reservedAllocation.HasValue(),
                   reservedAllocation.GetValue().PhysicalStart == ReservedBase, runtimeAllocation.HasValue(),
                   runtimeAllocation.GetValue().PhysicalStart == RuntimeMemoryBase, freePages == RemainingPages,
                   remainingAllocation.HasValue(), !subsequentAllocation.HasValue(),
                   subsequentAllocation.GetError() == Kernel::Memory::BootstrapPageAllocatorError::InsufficientMemory);
}

void TestInputFailures(TestContext& runner) noexcept
{
    Ia64LoadedImageLayout image = ValidImage();
    PhysicalPageRange reserved = ValidReservedPages();
    const InitialMappingPlanError dirtyError = g_dirtyOutput.AddMapping(InitialMapping{
        .VirtualStart = Kernel::Memory::VirtualAddress{0x2000U},
        .PhysicalStart = Kernel::Memory::PhysicalAddress{0x2000U},
        .Pages = Kernel::Memory::PageCount{1U},
        .Permissions = Kernel::Memory::MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
        .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
        .Owner = InitialMappingOwner::NucleusCode,
        .Reserved = 0U,
    });
    const auto dirty =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_dirtyOutput);
    ASTERIA_EXPECT(runner, "a nonempty output is rejected without further mutation",
                   dirtyError == InitialMappingPlanError::None, !dirty.HasValue(),
                   dirty.GetError().Error == BootstrapMappingPlanError::OutputNotEmpty,
                   g_dirtyOutput.GetMappingCount() == 1U, !g_dirtyOutput.IsSealed());

    image = ValidImage();
    image.Sections[1U].VirtualStart += 0x1000U;
    const auto invalidImage =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
    ASTERIA_EXPECT(runner, "invalid image geometry leaves a fresh output unchanged", !invalidImage.HasValue(),
                   invalidImage.GetError().Error == BootstrapMappingPlanError::InvalidImageLayout,
                   g_failureOutput.GetMappingCount() == 0U, !g_failureOutput.IsSealed());

    image = ValidImage(Ia64RegionOneStart);
    const auto imageOutsideRegionZero =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
    ASTERIA_EXPECT(runner, "an image starting outside virtual region 0 is rejected atomically",
                   !imageOutsideRegionZero.HasValue(),
                   imageOutsideRegionZero.GetError().Error == BootstrapMappingPlanError::InvalidImageLayout,
                   g_failureOutput.GetMappingCount() == 0U, !g_failureOutput.IsSealed());

    image = ValidImage(Ia64RegionOneStart - 0x8000U);
    const auto imageCrossingRegionZero =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
    ASTERIA_EXPECT(runner, "an image crossing the virtual-region-0 boundary is rejected atomically",
                   !imageCrossingRegionZero.HasValue(),
                   imageCrossingRegionZero.GetError().Error == BootstrapMappingPlanError::InvalidImageLayout,
                   g_failureOutput.GetMappingCount() == 0U, !g_failureOutput.IsSealed());

    image = ValidImage();
    bool allPurposesRejected{true};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Firmware::Ia64LoadedImageSectionCount; ++index)
    {
        const Ia64LoadedImageSectionPurpose savedPurpose = image.Sections[index].Purpose;
        image.Sections[index].Purpose = savedPurpose == Ia64LoadedImageSectionPurpose::Code
                                            ? Ia64LoadedImageSectionPurpose::WritableData
                                            : Ia64LoadedImageSectionPurpose::Code;
        const auto wrongPurpose =
            BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
        allPurposesRejected = allPurposesRejected && !wrongPurpose.HasValue() &&
                              (wrongPurpose.GetError().Error == BootstrapMappingPlanError::InvalidImageLayout) &&
                              (g_failureOutput.GetMappingCount() == 0U) && !g_failureOutput.IsSealed();
        image.Sections[index].Purpose = static_cast<Ia64LoadedImageSectionPurpose>(0xFFU);
        const auto unknownPurpose =
            BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
        allPurposesRejected = allPurposesRejected && !unknownPurpose.HasValue() &&
                              (unknownPurpose.GetError().Error == BootstrapMappingPlanError::InvalidImageLayout) &&
                              (g_failureOutput.GetMappingCount() == 0U) && !g_failureOutput.IsSealed();
        image.Sections[index].Purpose = savedPurpose;
    }
    ASTERIA_EXPECT(runner, "every misplaced or unknown section purpose is rejected atomically", allPurposesRejected);

    image = ValidImage();
    reserved = ValidReservedPages();
    reserved.PageCount = BootstrapReservedPageCount - 1U;
    const auto invalidReserved =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
    ASTERIA_EXPECT(runner, "an incomplete reserved range is rejected before output mutation",
                   !invalidReserved.HasValue(),
                   invalidReserved.GetError().Error == BootstrapMappingPlanError::InvalidReservedPages,
                   g_failureOutput.GetMappingCount() == 0U);

    reserved = ValidReservedPages();
    reserved.PhysicalStart += 0x4000U;
    const auto misalignedReserved =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
    ASTERIA_EXPECT(runner, "four-page alignment cannot substitute for the IVT's eight-page boundary",
                   !misalignedReserved.HasValue(),
                   misalignedReserved.GetError().Error == BootstrapMappingPlanError::InvalidReservedPages,
                   g_failureOutput.GetMappingCount() == 0U, !g_failureOutput.IsSealed());

    reserved = ValidReservedPages();
    reserved.PhysicalStart = ImageBase;
    const auto overlap =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_failureOutput);
    ASTERIA_EXPECT(runner, "reserved bootstrap pages may not alias the loaded image", !overlap.HasValue(),
                   overlap.GetError().Error == BootstrapMappingPlanError::ReservedPagesOverlapImage,
                   g_failureOutput.GetMappingCount() == 0U);
}

void TestAlignedCodeGap(TestContext& runner) noexcept
{
    const PhysicalMemoryRegion savedImage = g_bootInfo.MemoryRegions[1U];
    g_bootInfo.MemoryRegions[1U].PageCount = 16U;
    const auto result = BootstrapMappingPlanBuilder::Build(AlignedCodeImage(), g_bootInfo, ValidReservedPages(),
                                                           ValidRuntimeMemoryPages(), g_alignedCodeOutput);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    ASTERIA_EXPECT(runner, "a 32-KiB-aligned code section leaves its header padding unmapped", result.HasValue(),
                   g_alignedCodeOutput.IsSealed(),
                   g_alignedCodeOutput.GetMappingCount() == BootstrapInitialMappingCount,
                   !PlanOverlapsPhysicalRange(g_alignedCodeOutput, ImageBase, 8U),
                   IsExactMapping(g_alignedCodeOutput, 5U, ImageBase + 0x8000U, 2U, InitialMappingOwner::NucleusCode,
                                  false, true));
}

void TestLargeImage(TestContext& runner) noexcept
{
    const PhysicalMemoryRegion savedImage = g_bootInfo.MemoryRegions[1U];
    g_bootInfo.MemoryRegions[1U].PageCount = LargeImageBytes / Kernel::Memory::MappingPageSizeBytes;
    const auto result = BootstrapMappingPlanBuilder::Build(LargeImage(), g_bootInfo, ValidReservedPages(),
                                                           ValidRuntimeMemoryPages(), g_largeImageOutput);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    ASTERIA_EXPECT(runner, "a valid image above two MiB is bounded by geometry and memory coverage", result.HasValue(),
                   g_largeImageOutput.IsSealed(), g_largeImageOutput.GetMappingCount() == BootstrapInitialMappingCount,
                   IsExactMapping(g_largeImageOutput, 7U, ImageBase + 0x6000U,
                                  (LargeImageBytes / Kernel::Memory::MappingPageSizeBytes) - 6U,
                                  InitialMappingOwner::NucleusWritableData, true, false));
}

void TestMemoryProvenance(TestContext& runner) noexcept
{
    const PhysicalMemoryRegion savedReserved = g_bootInfo.MemoryRegions[0U];
    const PhysicalMemoryRegion savedImage = g_bootInfo.MemoryRegions[1U];
    const Foundation::UInt32 savedCount = g_bootInfo.MemoryRegionCount;
    const Foundation::UInt64 savedMapBytes = g_bootInfo.FirmwareMemoryMapSizeBytes;
    Ia64LoadedImageLayout image = ValidImage();
    PhysicalPageRange reserved = ValidReservedPages();

    g_bootInfo.MemoryRegionCount = 0U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 0U;
    const auto invalidBootInfo =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "an invalid final BootInfo is rejected before provenance lookup",
                   !invalidBootInfo.HasValue(),
                   invalidBootInfo.GetError().Error == BootstrapMappingPlanError::InvalidBootInfo,
                   g_provenanceOutput.GetMappingCount() == 0U);
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;

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
    const auto invalidUnrelatedVirtual =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    g_bootInfo.MemoryRegions[2U].VirtualStart = 0U;
    g_bootInfo.MemoryRegions[2U].Kind = PhysicalMemoryKind::Reserved;
    const auto mismatchedUnrelatedKind =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    g_bootInfo.MemoryRegions[2U].FirmwareType = 16U;
    const auto undefinedUnrelatedType =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "unrelated final-map descriptors receive complete shape validation",
                   !invalidUnrelatedVirtual.HasValue(),
                   invalidUnrelatedVirtual.GetError().Error == BootstrapMappingPlanError::InvalidBootInfo,
                   !mismatchedUnrelatedKind.HasValue(),
                   mismatchedUnrelatedKind.GetError().Error == BootstrapMappingPlanError::InvalidBootInfo,
                   !undefinedUnrelatedType.HasValue(),
                   undefinedUnrelatedType.GetError().Error == BootstrapMappingPlanError::InvalidBootInfo,
                   g_provenanceOutput.GetMappingCount() == 0U);
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[2U] = savedThird;

    g_bootInfo.MemoryRegions[1U].Attributes = 0x1U;
    const auto uncachedImage =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "an image without write-back capability is rejected", !uncachedImage.HasValue(),
                   uncachedImage.GetError().Error == BootstrapMappingPlanError::InvalidImageMemoryProvenance,
                   g_provenanceOutput.GetMappingCount() == 0U);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    g_bootInfo.MemoryRegions[1U].FirmwareType = 7U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::Conventional;
    const auto freeableImage =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "freeable Conventional memory cannot own the executing image", !freeableImage.HasValue(),
                   freeableImage.GetError().Error == BootstrapMappingPlanError::InvalidImageMemoryProvenance);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    g_bootInfo.MemoryRegions[1U].FirmwareType = 4U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::BootServices;
    const auto bootServicesImage =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "BootServices memory cannot substitute for loaded-image ownership",
                   !bootServicesImage.HasValue(),
                   bootServicesImage.GetError().Error == BootstrapMappingPlanError::InvalidImageMemoryProvenance);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    g_bootInfo.MemoryRegions[1U].PhysicalStart += 0x1000U;
    --g_bootInfo.MemoryRegions[1U].PageCount;
    const auto imageGap =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "the final map must cover the image header and every section page", !imageGap.HasValue(),
                   imageGap.GetError().Error == BootstrapMappingPlanError::InvalidImageMemoryProvenance);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    g_bootInfo.MemoryRegions[1U].FirmwareType = 11U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::MemoryMappedIo;
    const auto deviceImage =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "device memory cannot provide nucleus image provenance", !deviceImage.HasValue(),
                   deviceImage.GetError().Error == BootstrapMappingPlanError::InvalidImageMemoryProvenance);
    g_bootInfo.MemoryRegions[1U] = savedImage;

    reserved.Attributes = 0x1U;
    g_bootInfo.MemoryRegions[0U].Attributes = 0x1U;
    const auto uncachedReserved =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "reserved storage without write-back capability is rejected", !uncachedReserved.HasValue(),
                   uncachedReserved.GetError().Error == BootstrapMappingPlanError::InvalidReservedPages);
    reserved = ValidReservedPages();
    g_bootInfo.MemoryRegions[0U] = savedReserved;

    g_bootInfo.MemoryRegions[0U].Attributes = 0x9U;
    const auto reservedMismatch =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "the allocation receipt must match final-map attributes", !reservedMismatch.HasValue(),
                   reservedMismatch.GetError().Error == BootstrapMappingPlanError::InvalidReservedMemoryProvenance);
    g_bootInfo.MemoryRegions[0U] = savedReserved;

    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[0U].PageCount = 5U;
    g_bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ReservedBase + 0x5000U,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount - 5U,
        .Attributes = 0x1U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    g_bootInfo.MemoryRegions[2U] = savedImage;
    const auto invalidReservedTail =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "guard, IVT, and RSE pages require the same full-range provenance",
                   !invalidReservedTail.HasValue(),
                   invalidReservedTail.GetError().Error == BootstrapMappingPlanError::InvalidReservedMemoryProvenance,
                   g_provenanceOutput.GetMappingCount() == 0U, !g_provenanceOutput.IsSealed());
    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[2U] = savedThird;

    reserved.PhysicalStart = 0U;
    g_bootInfo.MemoryRegions[0U].PhysicalStart = 0U;
    const auto pageZero =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "page zero cannot back the complete bootstrap reservation", !pageZero.HasValue(),
                   pageZero.GetError().Error == BootstrapMappingPlanError::InvalidReservedPages);
    reserved = ValidReservedPages();
    g_bootInfo.MemoryRegions[0U] = savedReserved;

    reserved.PhysicalStart = ImageBase - 0x8000U;
    g_bootInfo.MemoryRegionCount = 1U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 40U;
    g_bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase - 0x8000U,
        .VirtualStart = 0U,
        .PageCount = 13U,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    const auto headerOverlap =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_provenanceOutput);
    ASTERIA_EXPECT(runner, "reserved storage cannot overlap the image header page", !headerOverlap.HasValue(),
                   headerOverlap.GetError().Error == BootstrapMappingPlanError::ReservedPagesOverlapImage);

    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase,
        .VirtualStart = 0U,
        .PageCount = 3U,
        .Attributes = 0x9U,
        .FirmwareType = 1U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase + 0x3000U,
        .VirtualStart = 0U,
        .PageCount = 6U,
        .Attributes = 0x8U,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    reserved = ValidReservedPages();
    const auto splitLoaderImage =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_loaderOutput);
    ASTERIA_EXPECT(runner, "adjacent write-back Loader descriptors may jointly cover the image",
                   splitLoaderImage.HasValue(), g_loaderOutput.IsSealed(),
                   g_loaderOutput.GetMappingCount() == BootstrapInitialMappingCount);

    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{};
}

void TestVirtualRegionZeroBoundary(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 HighestReservedBase{Ia64RegionOneStart - 0x2'0000U};
    constexpr Foundation::UInt64 CrossingReservedBase{Ia64RegionOneStart - 0x1'8000U};
    constexpr Foundation::UInt64 HighestImageBase{Ia64RegionOneStart - 0x9000U};
    const PhysicalMemoryRegion savedReserved = g_bootInfo.MemoryRegions[0U];
    const PhysicalMemoryRegion savedImage = g_bootInfo.MemoryRegions[1U];
    const Foundation::UInt32 savedCount = g_bootInfo.MemoryRegionCount;
    const Foundation::UInt64 savedMapBytes = g_bootInfo.FirmwareMemoryMapSizeBytes;
    const Ia64LoadedImageLayout image = ValidImage();

    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = RuntimeMemoryBase,
        .VirtualStart = 0U,
        .PageCount = Ia64FixtureRuntimeMemoryPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{
        .PhysicalStart = HighestReservedBase,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    PhysicalPageRange reserved{
        .PhysicalStart = HighestReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    const auto highest =
        BootstrapMappingPlanBuilder::Build(image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_boundaryOutput);
    ASTERIA_EXPECT(runner, "the highest aligned region-0 bootstrap reservation is representable", highest.HasValue(),
                   GetBootstrapInterruptVectorTablePhysicalStart(highest.GetValue()) ==
                       (Ia64RegionOneStart - 0x1'0000U),
                   GetBootstrapRseBackingStorePhysicalStart(highest.GetValue()) == (Ia64RegionOneStart - 0x8000U),
                   GetBootstrapReservedPhysicalEnd(highest.GetValue()) == Ia64RegionOneStart,
                   g_boundaryOutput.IsSealed(), g_boundaryOutput.GetMappingCount() == BootstrapInitialMappingCount);

    reserved.PhysicalStart = CrossingReservedBase;
    const auto crossingReserved = BootstrapMappingPlanBuilder::Build(
        image, g_bootInfo, reserved, ValidRuntimeMemoryPages(), g_boundaryFailureOutput);
    ASTERIA_EXPECT(runner, "an aligned reservation crossing region 0 is rejected atomically",
                   !crossingReserved.HasValue(),
                   crossingReserved.GetError().Error == BootstrapMappingPlanError::InvalidReservedPages,
                   g_boundaryFailureOutput.GetMappingCount() == 0U, !g_boundaryFailureOutput.IsSealed());

    g_bootInfo.MemoryRegionCount = savedCount;
    g_bootInfo.FirmwareMemoryMapSizeBytes = savedMapBytes;
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{};
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = HighestImageBase,
        .VirtualStart = 0U,
        .PageCount = 9U,
        .Attributes = 0x8U,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    const Ia64LoadedImageLayout highestImage = ValidImage(HighestImageBase);
    const auto imageAtBoundary = BootstrapMappingPlanBuilder::Build(highestImage, g_bootInfo, ValidReservedPages(),
                                                                    ValidRuntimeMemoryPages(), g_regionBoundaryOutput);
    ASTERIA_EXPECT(runner, "an image whose exclusive end equals the region-1 boundary is valid",
                   imageAtBoundary.HasValue(), g_regionBoundaryOutput.IsSealed(),
                   g_regionBoundaryOutput.GetMappingCount() == BootstrapInitialMappingCount);

    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = savedImage;
}
constexpr const char* InvalidRuntimeMemoryShapes[]{
    "no pages",
    "three pages",
    "a 32 KiB page, which IA-64 cannot insert",
    "a 16 MiB page on an 8 MiB boundary",
    "page zero",
    "LoaderData",
    "no write-back attribute",
    "the runtime attribute",
    "a nonzero reserved field",
    "region 1",
};

/// The valid run-time memory reservation with the shape defect named by
/// InvalidRuntimeMemoryShapes[index].
[[nodiscard]] constexpr PhysicalPageRange InvalidRuntimeMemoryPages(const Foundation::Size index) noexcept
{
    PhysicalPageRange range = ValidRuntimeMemoryPages();
    switch (index)
    {
    case 0U:
        range.PageCount = 0U;
        break;
    case 1U:
        range.PageCount = 3U;
        break;
    case 2U:
        range.PageCount = 8U;
        break;
    case 3U:
        range.PhysicalStart = RuntimeMemoryBase + 0x80'0000U;
        break;
    case 4U:
        range.PhysicalStart = 0U;
        break;
    case 5U:
        range.FirmwareType = 2U;
        break;
    case 6U:
        range.Attributes = 0x1U;
        break;
    case 7U:
        range.Attributes = 0x8U | (Foundation::UInt64{1U} << 63U);
        break;
    case 8U:
        range.Reserved = 1U;
        break;
    default:
        range.PhysicalStart = Ia64RegionOneStart;
        break;
    }
    return range;
}

[[nodiscard]] BootstrapMappingPlanError BuildWithRuntimeMemory(const PhysicalPageRange& runtimeMemoryPages,
                                                               InitialMappingPlan& output) noexcept
{
    const auto result =
        BootstrapMappingPlanBuilder::Build(ValidImage(), g_bootInfo, ValidReservedPages(), runtimeMemoryPages, output);
    return result.HasValue() ? BootstrapMappingPlanError::None : result.GetError().Error;
}

[[nodiscard]] constexpr PhysicalPageRange RuntimeMemoryPagesAt(const Foundation::UInt64 start,
                                                               const Foundation::UInt64 attributes = 0x8U) noexcept
{
    PhysicalPageRange range = ValidRuntimeMemoryPages();
    range.PhysicalStart = start;
    range.Attributes = attributes;
    return range;
}

void TestRuntimeMemoryReservationShape(TestContext& runner) noexcept
{
    for (Foundation::Size index{0U};
         index < (sizeof(InvalidRuntimeMemoryShapes) / sizeof(InvalidRuntimeMemoryShapes[0U])); ++index)
    {
        ASTERIA_EXPECT(runner, InvalidRuntimeMemoryShapes[index],
                       BuildWithRuntimeMemory(InvalidRuntimeMemoryPages(index), g_failureOutput) ==
                           BootstrapMappingPlanError::InvalidRuntimeMemoryPages,
                       g_failureOutput.GetMappingCount() == 0U, !g_failureOutput.IsSealed());
    }
    ASTERIA_EXPECT(runner, "image overlap is rejected; the runtime page may own the exact bootstrap prefix",
                   BuildWithRuntimeMemory(RuntimeMemoryPagesAt(ImageBase), g_failureOutput) ==
                       BootstrapMappingPlanError::RuntimeMemoryPagesOverlap,
                   BuildWithRuntimeMemory(RuntimeMemoryPagesAt(ReservedBase), g_consolidatedOutput) ==
                       BootstrapMappingPlanError::None,
                   g_consolidatedOutput.GetMappingCount() == 4U, g_failureOutput.GetMappingCount() == 0U);
    ASTERIA_EXPECT(
        runner, "the run-time memory page needs final-map provenance with its own attributes",
        BuildWithRuntimeMemory(RuntimeMemoryPagesAt(RuntimeMemoryBase, 0x9U), g_provenanceOutput) ==
            BootstrapMappingPlanError::InvalidRuntimeMemoryProvenance,
        BuildWithRuntimeMemory(RuntimeMemoryPagesAt(RuntimeMemoryBase + RuntimeMemoryReservationPreferredBytes),
                               g_provenanceOutput) == BootstrapMappingPlanError::InvalidRuntimeMemoryProvenance,
        g_provenanceOutput.GetMappingCount() == 0U);
}

void TestRuntimeMemoryReservationProvenance(TestContext& runner) noexcept
{
    const PhysicalMemoryRegion savedReserved = g_bootInfo.MemoryRegions[0U];
    const PhysicalMemoryRegion savedImage = g_bootInfo.MemoryRegions[1U];
    g_bootInfo.MemoryRegionCount = 3U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 120U;
    g_bootInfo.MemoryRegions[0U].PageCount = BootstrapReservedPageCount;
    g_bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = RuntimeMemoryBase,
        .VirtualStart = 0U,
        .PageCount = Ia64FixtureRuntimeMemoryPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    g_bootInfo.MemoryRegions[2U] = savedImage;
    const BootstrapMappingPlanError loaderOwned = BuildWithRuntimeMemory(ValidRuntimeMemoryPages(), g_provenanceOutput);
    g_bootInfo.MemoryRegions[1U].FirmwareType = 7U;
    g_bootInfo.MemoryRegions[1U].Kind = PhysicalMemoryKind::Conventional;
    const BootstrapMappingPlanError separateRegion =
        BuildWithRuntimeMemory(ValidRuntimeMemoryPages(), g_runtimeRegionOutput);
    g_bootInfo.MemoryRegionCount = 2U;
    g_bootInfo.FirmwareMemoryMapSizeBytes = 80U;
    g_bootInfo.MemoryRegions[0U] = savedReserved;
    g_bootInfo.MemoryRegions[1U] = savedImage;
    g_bootInfo.MemoryRegions[2U] = PhysicalMemoryRegion{};
    ASTERIA_EXPECT(runner, "Loader memory cannot hold the run-time memory page; its own Conventional region can",
                   loaderOwned == BootstrapMappingPlanError::InvalidRuntimeMemoryProvenance,
                   g_provenanceOutput.GetMappingCount() == 0U, separateRegion == BootstrapMappingPlanError::None,
                   g_runtimeRegionOutput.GetMappingCount() == BootstrapInitialMappingCount,
                   IsExactMapping(g_runtimeRegionOutput, 4U, RuntimeMemoryBase, Ia64FixtureRuntimeMemoryPageCount,
                                  InitialMappingOwner::NucleusWritableData, true, false));

    // A fallback page: the smallest architected size that holds the object.
    PhysicalPageRange fallback = RuntimeMemoryPagesAt(RuntimeMemoryBase + 0x10'0000U);
    fallback.PageCount = 0x100U;
    ASTERIA_EXPECT(runner, "a 1 MiB fallback page becomes the one writable run-time memory mapping",
                   BuildWithRuntimeMemory(fallback, g_runtimeFallbackOutput) == BootstrapMappingPlanError::None,
                   g_runtimeFallbackOutput.GetMappingCount() == BootstrapInitialMappingCount,
                   IsExactMapping(g_runtimeFallbackOutput, 4U, RuntimeMemoryBase + 0x10'0000U, 0x100U,
                                  InitialMappingOwner::NucleusWritableData, true, false));

    // The smallest architected page is one 4 KiB page on any page boundary.
    PhysicalPageRange smallest = RuntimeMemoryPagesAt(RuntimeMemoryBase + 0x1000U);
    smallest.PageCount = 1U;
    ASTERIA_EXPECT(runner, "a 4 KiB run-time memory page is the smallest one the shape admits",
                   BuildWithRuntimeMemory(smallest, g_runtimeSmallestOutput) == BootstrapMappingPlanError::None,
                   IsExactMapping(g_runtimeSmallestOutput, 4U, RuntimeMemoryBase + 0x1000U, 1U,
                                  InitialMappingOwner::NucleusWritableData, true, false));
}

void TestRuntimeMemoryPageSelection(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 Architected{Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask};
    constexpr Foundation::UInt64 Object{970'752U};
    // Physical memory management budgets the complete owner and slabs, rather than reducing the
    // slot counts to fit a 4 MiB fallback.
    constexpr auto Metadata =
        SelectRuntimeMemoryReservationPageShifts(Architected, Platform::Ia64::Memory::Ia64RuntimeRequiredBytes);
    constexpr auto FourMiBOnly = SelectRuntimeMemoryReservationPageShifts(
        Foundation::UInt64{1U} << 22U, Platform::Ia64::Memory::Ia64RuntimeRequiredBytes);
    ASTERIA_EXPECT(runner, "the complete metadata layout fits 16 MiB and refuses a 4 MiB-only reservation",
                   Metadata.Preferred == 24U, Metadata.Fallback == 0U, FourMiBOnly.Preferred == 0U,
                   FourMiBOnly.Fallback == 0U, Platform::Ia64::Memory::Ia64RuntimeBitmapBudgetBytes >= (1U << 20U));
    constexpr RuntimeMemoryReservationPageShifts Montecito =
        SelectRuntimeMemoryReservationPageShifts(Architected, Object);
    constexpr RuntimeMemoryReservationPageShifts NoSixteenMebibytes =
        SelectRuntimeMemoryReservationPageShifts(Architected & ~RuntimeMemoryReservationPreferredBytes, Object);
    constexpr RuntimeMemoryReservationPageShifts Exact =
        SelectRuntimeMemoryReservationPageShifts(Architected, RuntimeMemoryReservationPreferredBytes);
    constexpr RuntimeMemoryReservationPageShifts Larger =
        SelectRuntimeMemoryReservationPageShifts(Architected, RuntimeMemoryReservationPreferredBytes + 1U);
    constexpr RuntimeMemoryReservationPageShifts SmallObject =
        SelectRuntimeMemoryReservationPageShifts(Architected, 0x1'0000U);
    constexpr RuntimeMemoryReservationPageShifts TooLarge =
        SelectRuntimeMemoryReservationPageShifts(Architected, (Foundation::UInt64{1U} << 28U) + 1U);
    constexpr RuntimeMemoryReservationPageShifts NoObject = SelectRuntimeMemoryReservationPageShifts(Architected, 0U);
    // 32 KiB, 128 KiB and 2 MiB are not architected sizes: a mask of only
    // them leaves nothing every processor can insert.
    constexpr RuntimeMemoryReservationPageShifts Unarchitected = SelectRuntimeMemoryReservationPageShifts(
        (Foundation::UInt64{1U} << 15U) | (Foundation::UInt64{1U} << 17U) | (Foundation::UInt64{1U} << 21U), Object);
    ASTERIA_EXPECT(
        runner,
        "the run-time memory page prefers 16 MiB and falls back to the smallest architected size that holds the object",
        (Montecito.Preferred == 24U) && (Montecito.Fallback == 20U),
        (NoSixteenMebibytes.Preferred == 20U) && (NoSixteenMebibytes.Fallback == 0U),
        (Exact.Preferred == 24U) && (Exact.Fallback == 0U), (Larger.Preferred == 26U) && (Larger.Fallback == 0U),
        (SmallObject.Preferred == 24U) && (SmallObject.Fallback == 16U),
        (TooLarge.Preferred == 0U) && (TooLarge.Fallback == 0U),
        (NoObject.Preferred == 0U) && (NoObject.Fallback == 0U),
        (Unarchitected.Preferred == 0U) && (Unarchitected.Fallback == 0U));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"BootstrapMappingPlanTests"};
    Asteria::Test::TestExactPlan(runner);
    Asteria::Test::TestInputFailures(runner);
    Asteria::Test::TestAlignedCodeGap(runner);
    Asteria::Test::TestLargeImage(runner);
    Asteria::Test::TestMemoryProvenance(runner);
    Asteria::Test::TestVirtualRegionZeroBoundary(runner);
    Asteria::Test::TestRuntimeMemoryReservationShape(runner);
    Asteria::Test::TestRuntimeMemoryReservationProvenance(runner);
    Asteria::Test::TestRuntimeMemoryPageSelection(runner);
    return runner.Finish();
}
