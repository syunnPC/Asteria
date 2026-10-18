// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Memory/InitialExecutionContextSwitcher.hpp"

#include "Support/Ia64RuntimeMemoryReservation.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_switchCallCount{0U};
Foundation::UInt64 g_switchStackPointer{0U};
Foundation::UInt64 g_switchRseStart{0U};
Foundation::UInt64 g_switchStatus{1U};
Foundation::UInt8* g_observedStackData{nullptr};
Foundation::Size g_observedStackSize{0U};
Foundation::UInt8* g_observedRseData{nullptr};
Foundation::Size g_observedRseSize{0U};
bool g_switchObservedZeroStorage{false};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64SwitchInitialExecutionContext(const Asteria::Foundation::UInt64 initialStackPointer,
                                         const Asteria::Foundation::UInt64 rseBackingStorePhysicalStart) noexcept
{
    bool storageIsZero =
        (Asteria::Test::g_observedStackData != nullptr) && (Asteria::Test::g_observedRseData != nullptr);
    for (Asteria::Foundation::Size offset{0U}; offset < Asteria::Test::g_observedStackSize; ++offset)
    {
        storageIsZero = (Asteria::Test::g_observedStackData[offset] == 0U) && storageIsZero;
    }
    for (Asteria::Foundation::Size offset{0U}; offset < Asteria::Test::g_observedRseSize; ++offset)
    {
        storageIsZero = (Asteria::Test::g_observedRseData[offset] == 0U) && storageIsZero;
    }
    ++Asteria::Test::g_switchCallCount;
    Asteria::Test::g_switchStackPointer = initialStackPointer;
    Asteria::Test::g_switchRseStart = rseBackingStorePhysicalStart;
    Asteria::Test::g_switchObservedZeroStorage = storageIsZero;
    return Asteria::Test::g_switchStatus;
}

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::PhysicalPageRange;
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageSection;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialExecutionContextSwitchError;
using Platform::Ia64::Memory::InitialExecutionContextSwitcher;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialMemoryStackPreparationError;
using Platform::Ia64::Memory::InitialMemoryStackScratchSizeBytes;
using Platform::Ia64::Memory::InitialMemoryStackSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStorePreparationError;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::WritableInitialMemoryStackView;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;

constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constinit InitialAddressSpacePreparation g_preparation;
constinit InitialAddressSpacePreparation g_freshPreparation;
alignas(32U * 1024U) Foundation::UInt8 g_interruptVectorTable[InitialInterruptVectorTableSizeBytes];
alignas(4096) Foundation::UInt8 g_memoryStack[InitialMemoryStackSizeBytes];
alignas(4096) Foundation::UInt8 g_rseBackingStore[InitialRseBackingStoreSizeBytes];

struct alignas(4096) StackViewAliasBlock
{
    WritableInitialMemoryStackView View;
    Foundation::UInt8 Tail[InitialMemoryStackSizeBytes - sizeof(WritableInitialMemoryStackView)];
};

struct alignas(4096) RseViewAliasBlock
{
    WritableRseBackingStoreView View;
    Foundation::UInt8 Tail[InitialRseBackingStoreSizeBytes - sizeof(WritableRseBackingStoreView)];
};

struct alignas(4096) StackContainsRseViewAliasBlock
{
    WritableRseBackingStoreView View;
    Foundation::UInt8 Tail[InitialMemoryStackSizeBytes - sizeof(WritableRseBackingStoreView)];
};

struct alignas(4096) RseContainsStackViewAliasBlock
{
    WritableInitialMemoryStackView View;
    Foundation::UInt8 Tail[InitialRseBackingStoreSizeBytes - sizeof(WritableInitialMemoryStackView)];
};

static_assert(sizeof(StackViewAliasBlock) == InitialMemoryStackSizeBytes);
static_assert(sizeof(RseViewAliasBlock) == InitialRseBackingStoreSizeBytes);
static_assert(sizeof(StackContainsRseViewAliasBlock) == InitialMemoryStackSizeBytes);
static_assert(sizeof(RseContainsStackViewAliasBlock) == InitialRseBackingStoreSizeBytes);

constinit StackViewAliasBlock g_stackViewAliasBlock{};
constinit RseViewAliasBlock g_rseViewAliasBlock{};
constinit StackContainsRseViewAliasBlock g_stackContainsRseViewAliasBlock{};
constinit RseContainsStackViewAliasBlock g_rseContainsStackViewAliasBlock{};
Foundation::UInt8 g_preparationSnapshot[sizeof(InitialAddressSpacePreparation)];
Foundation::UInt8 g_freshPreparationSnapshot[sizeof(InitialAddressSpacePreparation)];
Foundation::UInt8 g_stackSnapshot[InitialMemoryStackSizeBytes];
Foundation::UInt8 g_rseSnapshot[InitialRseBackingStoreSizeBytes];

[[nodiscard]] constexpr BootInfo ValidBootInfo() noexcept
{
    BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 2U,
        .FirmwareDescriptorVersion = 1U,
        .FirmwareDescriptorSize = 40U,
        .Reserved = 0U,
        .FirmwareMemoryMapSizeBytes = 80U,
        .MemoryRegions = {},
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
    bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = ReservedBase,
        .VirtualStart = 0U,
        .PageCount = TestSupport::Ia64FixtureReservationRegionPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ImageBase,
        .VirtualStart = 0U,
        .PageCount = 9U,
        .Attributes = 0x8U,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    return bootInfo;
}

[[nodiscard]] constexpr Ia64LoadedImageLayout ValidImage() noexcept
{
    return Ia64LoadedImageLayout{
        .ImageBase = ImageBase,
        .ImageSize = 0x9000U,
        .Sections =
            {
                Ia64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x1000U,
                    .PageCount = 2U,
                    .Purpose = Ia64LoadedImageSectionPurpose::Code,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x3000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x4000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x5000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::ReadOnlyData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x6000U,
                    .PageCount = 1U,
                    .Purpose = Ia64LoadedImageSectionPurpose::WritableData,
                    .Reserved = 0U,
                },
                Ia64LoadedImageSection{
                    .VirtualStart = ImageBase + 0x7000U,
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

[[nodiscard]] constexpr PalVirtualMemoryCapabilities ValidCapabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            Platform::Ia64::Memory::TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask =
                    static_cast<Foundation::UInt8>((Foundation::UInt8{1U} << 0U) | (Foundation::UInt8{1U} << 4U) |
                                                   (Foundation::UInt8{1U} << 5U) | (Foundation::UInt8{1U} << 6U)),
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 32U,
        .DataTranslationRegisterCount = 32U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 8U,
        .UniqueTranslationCacheCount = 2U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr SalSystemTableInformation ValidFirmware() noexcept
{
    return SalSystemTableInformation{
        .PalProcedurePhysicalAddress = 0x0010'0000U,
        .SalProcedurePhysicalAddress = 0x0020'0000U,
        .SalGlobalDataPhysicalAddress = 0x0030'0000U,
        .TranslationRegisterVirtualAddress = 0U,
        .TranslationRegisterEncodedPageSize = 22U << 2U,
        .CoherenceDomainInformationPhysicalAddress = 0U,
        .ApWakeVector = 0xFFU,
        .TotalTableLength = 192U,
        .CoherenceDomainCount = 0U,
        .SalRevision = 0x0340U,
        .EntryCount = 3U,
        .SalAVersion = 0x0100U,
        .SalBVersion = 0x0100U,
        .MemoryDescriptorCount = 0U,
        .PlatformFeatures = 0U,
        .HasPlatformFeatures = true,
        .HasTranslationRegister = true,
        .HasPurgeCoherenceInformation = false,
        .HasApWakeVector = true,
        .Reserved8 = 0U,
    };
}

[[nodiscard]] constexpr InitialTranslationRegionSet ValidRegions() noexcept
{
    InitialTranslationRegionSet regions{};
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        regions.Regions[index] = RegionConfiguration{
            .RegionIdentifier = static_cast<Foundation::UInt32>(0x2000U + index),
            .ProtectionKey = 0U,
            .VhptWalkerEnabled = false,
            .SharedAcrossAddressSpaces = index == 0U,
            .Reserved16 = 0U,
            .Reserved32 = 0U,
        };
    }
    return regions;
}

[[nodiscard]] WritableInterruptVectorTableView ValidTableView() noexcept
{
    return WritableInterruptVectorTableView{
        .Data = g_interruptVectorTable,
        .SizeBytes = sizeof(g_interruptVectorTable),
        .PhysicalStart = ReservedBase + 0x1'0000U,
    };
}

[[nodiscard]] WritableInitialMemoryStackView ValidStackView() noexcept
{
    return WritableInitialMemoryStackView{
        .Data = g_memoryStack,
        .SizeBytes = sizeof(g_memoryStack),
        .PhysicalStart = ReservedBase,
    };
}

[[nodiscard]] WritableRseBackingStoreView ValidRseView() noexcept
{
    return WritableRseBackingStoreView{
        .Data = g_rseBackingStore,
        .SizeBytes = sizeof(g_rseBackingStore),
        .PhysicalStart = ReservedBase + 0x1'8000U,
    };
}

template <typename TView> [[nodiscard]] bool ViewsEqual(const TView& left, const TView& right) noexcept
{
    return (left.Data == right.Data) && (left.SizeBytes == right.SizeBytes) &&
           (left.PhysicalStart == right.PhysicalStart);
}

[[nodiscard]] bool PrepareFixture() noexcept
{
    for (Foundation::Size offset{0U}; offset < sizeof(g_memoryStack); ++offset)
    {
        g_memoryStack[offset] = 0xA5U;
    }
    const BootInfo bootInfo = ValidBootInfo();
    return g_preparation
        .Prepare(ValidImage(), bootInfo, ValidReservedPages(), TestSupport::Ia64FixtureRuntimeMemoryPages(ReservedBase),
                 ValidCapabilities(), ValidFirmware(), ValidRegions(), ValidTableView(), ValidRseView())
        .HasValue();
}

[[nodiscard]] bool HasExactSharedExecutionMappings() noexcept
{
    constexpr Foundation::UInt16 requiredPermissions{static_cast<Foundation::UInt16>(MappingPermission::Read) |
                                                     static_cast<Foundation::UInt16>(MappingPermission::Write) |
                                                     static_cast<Foundation::UInt16>(MappingPermission::Global)};
    const auto planResult = g_preparation.TryGetMappingPlan();
    if (!planResult.HasValue())
    {
        return false;
    }

    const Kernel::Memory::InitialMappingPlan& plan = *planResult.GetValue();
    Foundation::Size stackMappingCount{0U};
    Foundation::Size rseMappingCount{0U};
    for (Foundation::Size index{0U}; index < plan.GetMappingCount(); ++index)
    {
        const auto mappingResult = plan.GetMapping(index);
        if (!mappingResult.HasValue())
        {
            return false;
        }
        const Kernel::Memory::InitialMapping& mapping = mappingResult.GetValue();
        if (mapping.Owner == InitialMappingOwner::NucleusStack)
        {
            ++stackMappingCount;
            if ((mapping.VirtualStart.GetValue() != ReservedBase) ||
                (mapping.PhysicalStart.GetValue() != ReservedBase) || (mapping.Pages.GetValue() != 8U) ||
                (mapping.Permissions.GetBits() != requiredPermissions))
            {
                return false;
            }
        }
        else if (mapping.Owner == InitialMappingOwner::NucleusRegisterBackingStore)
        {
            ++rseMappingCount;
            if ((mapping.VirtualStart.GetValue() != ReservedBase + 0x1'8000U) ||
                (mapping.PhysicalStart.GetValue() != ReservedBase + 0x1'8000U) || (mapping.Pages.GetValue() != 8U) ||
                (mapping.Permissions.GetBits() != requiredPermissions))
            {
                return false;
            }
        }
    }
    return (stackMappingCount == 1U) && (rseMappingCount == 1U);
}

void SnapshotState() noexcept
{
    const auto* const preparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_preparation);
    for (Foundation::Size offset{0U}; offset < sizeof(g_preparationSnapshot); ++offset)
    {
        g_preparationSnapshot[offset] = preparationBytes[offset];
    }
    const auto* const freshPreparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_freshPreparation);
    for (Foundation::Size offset{0U}; offset < sizeof(g_freshPreparationSnapshot); ++offset)
    {
        g_freshPreparationSnapshot[offset] = freshPreparationBytes[offset];
    }
    for (Foundation::Size offset{0U}; offset < sizeof(g_stackSnapshot); ++offset)
    {
        g_stackSnapshot[offset] = g_memoryStack[offset];
    }
    for (Foundation::Size offset{0U}; offset < sizeof(g_rseSnapshot); ++offset)
    {
        g_rseSnapshot[offset] = g_rseBackingStore[offset];
    }
}

[[nodiscard]] bool PreparationOwnersMatchSnapshot() noexcept
{
    const auto* const preparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_preparation);
    for (Foundation::Size offset{0U}; offset < sizeof(g_preparationSnapshot); ++offset)
    {
        if (g_preparationSnapshot[offset] != preparationBytes[offset])
        {
            return false;
        }
    }
    const auto* const freshPreparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_freshPreparation);
    for (Foundation::Size offset{0U}; offset < sizeof(g_freshPreparationSnapshot); ++offset)
    {
        if (g_freshPreparationSnapshot[offset] != freshPreparationBytes[offset])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool StateMatchesSnapshot() noexcept
{
    if (!PreparationOwnersMatchSnapshot())
    {
        return false;
    }
    for (Foundation::Size offset{0U}; offset < sizeof(g_stackSnapshot); ++offset)
    {
        if (g_stackSnapshot[offset] != g_memoryStack[offset])
        {
            return false;
        }
    }
    for (Foundation::Size offset{0U}; offset < sizeof(g_rseSnapshot); ++offset)
    {
        if (g_rseSnapshot[offset] != g_rseBackingStore[offset])
        {
            return false;
        }
    }
    return true;
}

void ResetAdapter(const Foundation::UInt64 status = 1U) noexcept
{
    g_switchCallCount = 0U;
    g_switchStackPointer = 0U;
    g_switchRseStart = 0U;
    g_switchStatus = status;
    g_observedStackData = g_memoryStack;
    g_observedStackSize = sizeof(g_memoryStack);
    g_observedRseData = g_rseBackingStore;
    g_observedRseSize = sizeof(g_rseBackingStore);
    g_switchObservedZeroStorage = false;
}

void TestValidationAndPreparation(TestContext& runner) noexcept
{
    WritableInitialMemoryStackView stack = ValidStackView();
    WritableRseBackingStoreView rse = ValidRseView();
    const WritableInitialMemoryStackView stackSnapshot = stack;
    const WritableRseBackingStoreView rseSnapshot = rse;
    SnapshotState();
    auto result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, rse);
    ASTERIA_EXPECT(runner, "validation derives the exact fresh-context addresses without writes", result.HasValue(),
                   result.GetValue().StackPhysicalStart == ReservedBase,
                   result.GetValue().InitialStackPointer ==
                       (ReservedBase + InitialMemoryStackSizeBytes - InitialMemoryStackScratchSizeBytes),
                   result.GetValue().RseBackingStorePhysicalStart == ReservedBase + 0x1'8000U,
                   result.GetValue().StackSizeBytes == InitialMemoryStackSizeBytes,
                   result.GetValue().RseBackingStoreSizeBytes == InitialRseBackingStoreSizeBytes,
                   ViewsEqual(stack, stackSnapshot), ViewsEqual(rse, rseSnapshot), StateMatchesSnapshot());

    result = InitialExecutionContextSwitcher::Validate(g_freshPreparation, stack, rse);
    ASTERIA_EXPECT(runner, "an unpublished address-space owner is rejected without writes", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::AddressSpaceNotPrepared,
                   result.GetError().MemoryStackError == InitialMemoryStackPreparationError::None,
                   result.GetError().RseBackingStoreError == InitialRseBackingStorePreparationError::None,
                   result.GetError().Reserved == 0U, ViewsEqual(stack, stackSnapshot), ViewsEqual(rse, rseSnapshot),
                   StateMatchesSnapshot());

    stack = ValidStackView();
    --stack.SizeBytes;
    const WritableInitialMemoryStackView validationShortStackSnapshot = stack;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, rse);
    ASTERIA_EXPECT(runner, "a short memory-stack view retains exact error provenance", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::MemoryStackRejected,
                   result.GetError().MemoryStackError == InitialMemoryStackPreparationError::InvalidStorageView,
                   ViewsEqual(stack, validationShortStackSnapshot), ViewsEqual(rse, rseSnapshot),
                   StateMatchesSnapshot());

    stack = ValidStackView();
    ++stack.PhysicalStart;
    const WritableInitialMemoryStackView mismatchedStackSnapshot = stack;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, rse);
    ASTERIA_EXPECT(runner, "a mismatched stack physical address is rejected without writes", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::MemoryStackRejected,
                   result.GetError().MemoryStackError == InitialMemoryStackPreparationError::StorageAddressMismatch,
                   ViewsEqual(stack, mismatchedStackSnapshot), ViewsEqual(rse, rseSnapshot), StateMatchesSnapshot());

    stack = ValidStackView();
    stack.Data = rse.Data;
    const WritableInitialMemoryStackView overlappingStackSnapshot = stack;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, rse);
    ASTERIA_EXPECT(runner, "overlapping memory-stack and RSE destinations are rejected", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::StorageRangesOverlap,
                   ViewsEqual(stack, overlappingStackSnapshot), ViewsEqual(rse, rseSnapshot), StateMatchesSnapshot());

    g_stackViewAliasBlock.View = WritableInitialMemoryStackView{
        .Data = reinterpret_cast<Foundation::UInt8*>(&g_stackViewAliasBlock),
        .SizeBytes = InitialMemoryStackSizeBytes,
        .PhysicalStart = ReservedBase,
    };
    const WritableInitialMemoryStackView stackAliasSnapshot = g_stackViewAliasBlock.View;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, g_stackViewAliasBlock.View, rse);
    ASTERIA_EXPECT(runner, "memory-stack storage cannot alias its own view descriptor", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::StorageRangesOverlap,
                   ViewsEqual(g_stackViewAliasBlock.View, stackAliasSnapshot), ViewsEqual(rse, rseSnapshot),
                   StateMatchesSnapshot());

    g_rseViewAliasBlock.View = WritableRseBackingStoreView{
        .Data = reinterpret_cast<Foundation::UInt8*>(&g_rseViewAliasBlock),
        .SizeBytes = InitialRseBackingStoreSizeBytes,
        .PhysicalStart = ReservedBase + 0x1'8000U,
    };
    const WritableRseBackingStoreView rseAliasSnapshot = g_rseViewAliasBlock.View;
    stack = ValidStackView();
    const WritableInitialMemoryStackView rseAliasStackSnapshot = stack;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, g_rseViewAliasBlock.View);
    ASTERIA_EXPECT(runner, "RSE storage cannot alias its own view descriptor", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::StorageRangesOverlap,
                   ViewsEqual(stack, rseAliasStackSnapshot), ViewsEqual(g_rseViewAliasBlock.View, rseAliasSnapshot),
                   StateMatchesSnapshot());

    g_stackContainsRseViewAliasBlock.View = ValidRseView();
    stack = WritableInitialMemoryStackView{
        .Data = reinterpret_cast<Foundation::UInt8*>(&g_stackContainsRseViewAliasBlock),
        .SizeBytes = InitialMemoryStackSizeBytes,
        .PhysicalStart = ReservedBase,
    };
    const WritableInitialMemoryStackView stackContainingRseViewSnapshot = stack;
    const WritableRseBackingStoreView containedRseViewSnapshot = g_stackContainsRseViewAliasBlock.View;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, g_stackContainsRseViewAliasBlock.View);
    ASTERIA_EXPECT(runner, "memory-stack storage cannot alias the RSE view descriptor", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::StorageRangesOverlap,
                   ViewsEqual(stack, stackContainingRseViewSnapshot),
                   ViewsEqual(g_stackContainsRseViewAliasBlock.View, containedRseViewSnapshot), StateMatchesSnapshot());

    g_rseContainsStackViewAliasBlock.View = ValidStackView();
    rse = WritableRseBackingStoreView{
        .Data = reinterpret_cast<Foundation::UInt8*>(&g_rseContainsStackViewAliasBlock),
        .SizeBytes = InitialRseBackingStoreSizeBytes,
        .PhysicalStart = ReservedBase + 0x1'8000U,
    };
    const WritableInitialMemoryStackView containedStackViewSnapshot = g_rseContainsStackViewAliasBlock.View;
    const WritableRseBackingStoreView rseContainingStackViewSnapshot = rse;
    result = InitialExecutionContextSwitcher::Validate(g_preparation, g_rseContainsStackViewAliasBlock.View, rse);
    ASTERIA_EXPECT(runner, "RSE storage cannot alias the memory-stack view descriptor", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::StorageRangesOverlap,
                   ViewsEqual(g_rseContainsStackViewAliasBlock.View, containedStackViewSnapshot),
                   ViewsEqual(rse, rseContainingStackViewSnapshot), StateMatchesSnapshot());

    stack = ValidStackView();
    rse = ValidRseView();
    g_rseBackingStore[InitialRseBackingStoreSizeBytes - 1U] = 1U;
    const WritableInitialMemoryStackView nonzeroValidationStackSnapshot = stack;
    const WritableRseBackingStoreView nonzeroValidationRseSnapshot = rse;
    SnapshotState();
    result = InitialExecutionContextSwitcher::Validate(g_preparation, stack, rse);
    const bool nonzeroValidationPreserved = ViewsEqual(stack, nonzeroValidationStackSnapshot) &&
                                            ViewsEqual(rse, nonzeroValidationRseSnapshot) && StateMatchesSnapshot();
    g_rseBackingStore[InitialRseBackingStoreSizeBytes - 1U] = 0U;
    ASTERIA_EXPECT(runner, "a nonzero RSE tail byte is rejected without state drift", !result.HasValue(),
                   result.GetError().Error == InitialExecutionContextSwitchError::RseBackingStoreNotZero,
                   nonzeroValidationPreserved);

    bool commitValidationFailuresAreAtomic{true};

    stack = ValidStackView();
    rse = ValidRseView();
    SnapshotState();
    ResetAdapter();
    auto failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_freshPreparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.Error == InitialExecutionContextSwitchError::AddressSpaceNotPrepared) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, stackSnapshot) && ViewsEqual(rse, rseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;

    stack = ValidStackView();
    stack.Data = nullptr;
    const WritableInitialMemoryStackView nullStackSnapshot = stack;
    SnapshotState();
    ResetAdapter();
    failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.Error == InitialExecutionContextSwitchError::MemoryStackRejected) &&
        (failedCommit.MemoryStackError == InitialMemoryStackPreparationError::InvalidStorageView) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, nullStackSnapshot) && ViewsEqual(rse, rseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;

    stack = ValidStackView();
    --stack.SizeBytes;
    const WritableInitialMemoryStackView shortStackSnapshot = stack;
    SnapshotState();
    ResetAdapter();
    failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.MemoryStackError == InitialMemoryStackPreparationError::InvalidStorageView) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, shortStackSnapshot) && ViewsEqual(rse, rseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;

    stack = ValidStackView();
    ++stack.Data;
    const WritableInitialMemoryStackView unalignedStackSnapshot = stack;
    SnapshotState();
    ResetAdapter();
    failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.MemoryStackError == InitialMemoryStackPreparationError::InvalidStorageView) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, unalignedStackSnapshot) && ViewsEqual(rse, rseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;

    stack = ValidStackView();
    constexpr Foundation::UIntPointer maximumPointer{~Foundation::UIntPointer{0U}};
    stack.Data = reinterpret_cast<Foundation::UInt8*>(maximumPointer & ~Foundation::UIntPointer{0xFFFU});
    const WritableInitialMemoryStackView wrappingStackSnapshot = stack;
    SnapshotState();
    ResetAdapter();
    failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.MemoryStackError == InitialMemoryStackPreparationError::InvalidStorageView) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, wrappingStackSnapshot) && ViewsEqual(rse, rseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;

    stack = ValidStackView();
    rse = ValidRseView();
    rse.Data = nullptr;
    const WritableRseBackingStoreView nullRseSnapshot = rse;
    SnapshotState();
    ResetAdapter();
    failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.Error == InitialExecutionContextSwitchError::RseBackingStoreRejected) &&
        (failedCommit.RseBackingStoreError == InitialRseBackingStorePreparationError::InvalidStorageView) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, stackSnapshot) && ViewsEqual(rse, nullRseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;

    stack = ValidStackView();
    rse = ValidRseView();
    g_rseBackingStore[InitialRseBackingStoreSizeBytes - 1U] = 1U;
    const WritableRseBackingStoreView nonzeroRseSnapshot = rse;
    SnapshotState();
    ResetAdapter();
    failedCommit = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    commitValidationFailuresAreAtomic =
        (failedCommit.Error == InitialExecutionContextSwitchError::RseBackingStoreNotZero) &&
        (g_switchCallCount == 0U) && ViewsEqual(stack, stackSnapshot) && ViewsEqual(rse, nonzeroRseSnapshot) &&
        StateMatchesSnapshot() && commitValidationFailuresAreAtomic;
    g_rseBackingStore[InitialRseBackingStoreSizeBytes - 1U] = 0U;

    ASTERIA_EXPECT(runner, "every representative pre-commit rejection preserves all inputs",
                   commitValidationFailuresAreAtomic);
}

void TestPrepareAndSwitchBoundary(TestContext& runner) noexcept
{
    const WritableInitialMemoryStackView stack = ValidStackView();
    const WritableRseBackingStoreView rse = ValidRseView();
    const WritableInitialMemoryStackView stackViewSnapshot = stack;
    const WritableRseBackingStoreView rseViewSnapshot = rse;
    for (Foundation::Size offset{0U}; offset < sizeof(g_memoryStack); ++offset)
    {
        g_memoryStack[offset] = 0xA5U;
    }
    for (Foundation::Size offset{0U}; offset < sizeof(g_rseBackingStore); ++offset)
    {
        g_rseBackingStore[offset] = 0U;
    }
    SnapshotState();
    ResetAdapter(1U);
    auto failure = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    bool stackZero{true};
    for (Foundation::Size offset{0U}; offset < sizeof(g_memoryStack); ++offset)
    {
        stackZero = (g_memoryStack[offset] == 0U) && stackZero;
    }
    for (Foundation::Size offset{0U}; offset < sizeof(g_rseBackingStore); ++offset)
    {
        stackZero = (g_rseBackingStore[offset] == 0U) && stackZero;
    }
    ASTERIA_EXPECT(
        runner, "the inseparable commit clears and fences storage before the adapter",
        failure.Error == InitialExecutionContextSwitchError::UnsupportedProcessorState,
        failure.MemoryStackError == InitialMemoryStackPreparationError::None,
        failure.RseBackingStoreError == InitialRseBackingStorePreparationError::None, failure.Reserved == 0U,
        g_switchCallCount == 1U,
        g_switchStackPointer == ReservedBase + InitialMemoryStackSizeBytes - InitialMemoryStackScratchSizeBytes,
        g_switchRseStart == ReservedBase + 0x1'8000U, g_switchObservedZeroStorage, stackZero,
        ViewsEqual(stack, stackViewSnapshot), ViewsEqual(rse, rseViewSnapshot), PreparationOwnersMatchSnapshot());

    bool exactRse{true};
    for (Foundation::Size offset{0U}; offset < sizeof(g_rseBackingStore); ++offset)
    {
        g_rseBackingStore[offset] = 1U;
        SnapshotState();
        ResetAdapter();
        const auto rseFailure = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
        exactRse = (rseFailure.Error == InitialExecutionContextSwitchError::RseBackingStoreNotZero) &&
                   (g_switchCallCount == 0U) && ViewsEqual(stack, stackViewSnapshot) &&
                   ViewsEqual(rse, rseViewSnapshot) && StateMatchesSnapshot() && exactRse;
        g_rseBackingStore[offset] = 0U;
    }
    ASTERIA_EXPECT(runner, "every RSE byte is validated before the first stack write", exactRse);

    SnapshotState();
    ResetAdapter(0U);
    failure = InitialExecutionContextSwitcher::PrepareAndSwitch(g_preparation, stack, rse);
    ASTERIA_EXPECT(runner, "an impossible successful adapter return is fail-closed",
                   failure.Error == InitialExecutionContextSwitchError::UnexpectedAdapterReturn,
                   g_switchCallCount == 1U, g_switchObservedZeroStorage, ViewsEqual(stack, stackViewSnapshot),
                   ViewsEqual(rse, rseViewSnapshot), StateMatchesSnapshot());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialExecutionContextSwitcherTests"};
    ASTERIA_EXPECT(runner, "the switch fixture publishes the address-space owner and RSE",
                   Asteria::Test::PrepareFixture());
    ASTERIA_EXPECT(runner, "the prepared memory stack and RSE use exact Global permissions",
                   Asteria::Test::HasExactSharedExecutionMappings());
    Asteria::Test::TestValidationAndPreparation(runner);
    Asteria::Test::TestPrepareAndSwitchBoundary(runner);
    return runner.Finish();
}
