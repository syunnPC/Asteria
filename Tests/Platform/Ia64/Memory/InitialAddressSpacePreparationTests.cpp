// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

#include "Platform/Ia64/Memory/InitialExecutionContextSwitcher.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/Ia64RuntimeMemoryReservation.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
constinit Foundation::Size g_nativeAdapterCallCount{0U};
} // namespace Asteria::Test

// Link-time stand-ins: this preparation test may validate native operands,
// but must never cross an architectural commit boundary.
extern "C" Asteria::Foundation::UInt64
AsteriaIa64SwitchInitialExecutionContext(const Asteria::Foundation::UInt64, const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_nativeAdapterCallCount;
    return 1U;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallInitialInterruptVectorTable(const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_nativeAdapterCallCount;
    return 1U;
}

extern "C" Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus
AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable(const Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_nativeAdapterCallCount;
    return Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState;
}

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MemoryCachePolicy;
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageSection;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapInitialMappingCount;
using Platform::Ia64::Memory::BootstrapMappingPlanError;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialAddressSpacePreparationError;
using Platform::Ia64::Memory::InitialAddressSpacePreparationStatus;
using Platform::Ia64::Memory::InitialInterruptVectorTablePreparationError;
using Platform::Ia64::Memory::InitialInterruptVectorTablePreparer;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStorePreparationError;
using Platform::Ia64::Memory::InitialRseBackingStorePreparer;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::InitialTranslationEncodingError;
using Platform::Ia64::Memory::InitialTranslationPageShift;
using Platform::Ia64::Memory::InitialTranslationResourcePlan;
using Platform::Ia64::Memory::InitialTranslationResourcePlanError;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::WritableRseBackingStoreView;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;

constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt64 HighImageBase{0x1'0400'0000U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::Size PreparationBytes{sizeof(InitialAddressSpacePreparation)};
constexpr Foundation::UInt8 ExpectedInterruptVectorBundle[16U]{
    0x11U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U,
};

constinit InitialAddressSpacePreparation g_consolidatedPreparation;
constinit BootInfo g_consolidatedBootInfo{};
constinit TestSupport::Ia64HostedBootGeometry g_consolidatedGeometry{};
alignas(4096U) Foundation::UInt8 g_consolidatedStack[32U * 1024U]{};
constinit InitialAddressSpacePreparation g_successPreparation;
constinit InitialAddressSpacePreparation g_deterministicPreparation;
constinit InitialAddressSpacePreparation g_mappingFailurePreparation;
constinit InitialAddressSpacePreparation g_resourceFailurePreparation;
constinit InitialAddressSpacePreparation g_priorityPreparation;
constinit InitialAddressSpacePreparation g_alternateMappingFailurePreparation;
constinit InitialAddressSpacePreparation g_encodingFailurePreparation;
constinit InitialAddressSpacePreparation g_capacityFailurePreparation;
constinit InitialAddressSpacePreparation g_rseFailurePreparation;
constinit InitialAddressSpacePreparation g_nullRseFailurePreparation;
constinit InitialAddressSpacePreparation g_wrappingRseFailurePreparation;
constinit InitialAddressSpacePreparation g_rseAddressFailurePreparation;
constinit InitialAddressSpacePreparation g_ivtFailurePreparation;
constinit InitialAddressSpacePreparation g_ivtAddressFailurePreparation;
constinit InitialAddressSpacePreparation g_ivtInputAliasPreparation;
constinit InitialAddressSpacePreparation g_ivtOverlapPreparation;
constinit InitialAddressSpacePreparation g_inputAliasPreparation;
constinit InitialAddressSpacePreparation g_preflightAliasPreparation;
alignas(4096) Foundation::UInt8 g_rseBackingStore[InitialRseBackingStoreSizeBytes];
alignas(32U * 1024U) Foundation::UInt8 g_interruptVectorTable[InitialInterruptVectorTableSizeBytes];

template <typename TValue> struct alignas(4096) InputAliasBlock
{
    TValue Value;
    Foundation::UInt8 Tail[InitialRseBackingStoreSizeBytes];
};

template <typename TValue> struct alignas(32U * 1024U) InterruptVectorInputAliasBlock
{
    TValue Value;
    Foundation::UInt8 Tail[InitialInterruptVectorTableSizeBytes];
};

constinit InputAliasBlock<Ia64LoadedImageLayout> g_imageAliasBlock{};
constinit InputAliasBlock<BootInfo> g_bootInfoAliasBlock{};
constinit InputAliasBlock<PhysicalPageRange> g_reservedPagesAliasBlock{};
constinit InputAliasBlock<PhysicalPageRange> g_runtimeMemoryPagesAliasBlock{};
constinit InputAliasBlock<PalVirtualMemoryCapabilities> g_capabilitiesAliasBlock{};
constinit InputAliasBlock<SalSystemTableInformation> g_firmwareAliasBlock{};
constinit InputAliasBlock<InitialTranslationRegionSet> g_regionsAliasBlock{};
constinit InputAliasBlock<WritableRseBackingStoreView> g_storageViewAliasBlock{};
constinit InputAliasBlock<InitialAddressSpacePreparation> g_ownerAliasBlock{};
constinit InterruptVectorInputAliasBlock<PalVirtualMemoryCapabilities> g_ivtCapabilitiesAliasBlock{};
constinit InterruptVectorInputAliasBlock<Ia64LoadedImageLayout> g_ivtImageAliasBlock{};
constinit InterruptVectorInputAliasBlock<BootInfo> g_ivtBootInfoAliasBlock{};
constinit InterruptVectorInputAliasBlock<PhysicalPageRange> g_ivtReservedPagesAliasBlock{};
constinit InterruptVectorInputAliasBlock<PhysicalPageRange> g_ivtRuntimeMemoryPagesAliasBlock{};
constinit InterruptVectorInputAliasBlock<SalSystemTableInformation> g_ivtFirmwareAliasBlock{};
constinit InterruptVectorInputAliasBlock<InitialTranslationRegionSet> g_ivtRegionsAliasBlock{};
constinit InterruptVectorInputAliasBlock<WritableInterruptVectorTableView> g_ivtStorageViewAliasBlock{};
constinit InterruptVectorInputAliasBlock<WritableRseBackingStoreView> g_ivtRseViewAliasBlock{};
constinit InputAliasBlock<WritableInterruptVectorTableView> g_rseIvtViewAliasBlock{};
constinit InterruptVectorInputAliasBlock<InitialAddressSpacePreparation> g_ivtOwnerAliasBlock{};
Foundation::UInt8 g_aliasSnapshot[InitialRseBackingStoreSizeBytes];
Foundation::UInt8 g_ivtAliasSnapshot[InitialInterruptVectorTableSizeBytes];

[[nodiscard]] WritableRseBackingStoreView ValidRseBackingStoreView() noexcept
{
    return WritableRseBackingStoreView{
        .Data = g_rseBackingStore,
        .SizeBytes = sizeof(g_rseBackingStore),
        .PhysicalStart = ReservedBase + 0x1'8000U,
    };
}

[[nodiscard]] WritableInterruptVectorTableView ValidInterruptVectorTableView() noexcept
{
    return WritableInterruptVectorTableView{
        .Data = g_interruptVectorTable,
        .SizeBytes = sizeof(g_interruptVectorTable),
        .PhysicalStart = ReservedBase + 0x1'0000U,
    };
}

void FillRseBackingStore(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_rseBackingStore); ++index)
    {
        g_rseBackingStore[index] = value;
    }
}

void FillInterruptVectorTable(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_interruptVectorTable); ++index)
    {
        g_interruptVectorTable[index] = value;
    }
}

[[nodiscard]] bool IsInterruptVectorTableFilled(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_interruptVectorTable); ++index)
    {
        if (g_interruptVectorTable[index] != value)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsFailStopInterruptVectorTable() noexcept
{
    for (Foundation::Size offset{0U}; offset < sizeof(g_interruptVectorTable);
         offset += sizeof(ExpectedInterruptVectorBundle))
    {
        for (Foundation::Size index{0U}; index < sizeof(ExpectedInterruptVectorBundle); ++index)
        {
            if (g_interruptVectorTable[offset + index] != ExpectedInterruptVectorBundle[index])
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool IsRseBackingStoreFilled(const Foundation::UInt8 value) noexcept
{
    for (Foundation::Size index{0U}; index < sizeof(g_rseBackingStore); ++index)
    {
        if (g_rseBackingStore[index] != value)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr BootInfo ValidBootInfo(const Foundation::UInt64 imageBase = ImageBase) noexcept
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
        .PhysicalStart = imageBase,
        .VirtualStart = 0U,
        .PageCount = 9U,
        .Attributes = 0x8U,
        .FirmwareType = 2U,
        .Kind = PhysicalMemoryKind::Loader,
    };
    return bootInfo;
}

constinit const BootInfo g_bootInfo{ValidBootInfo()};
constinit const BootInfo g_expectedBootInfo{ValidBootInfo()};
constinit const BootInfo g_highBootInfo{ValidBootInfo(HighImageBase)};

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
    return TestSupport::Ia64FixtureRuntimeMemoryPages(ReservedBase);
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

[[nodiscard]] bool HasExactRegionZeroTranslationCoverage(const InitialMappingPlan& mappingPlan,
                                                         const InitialTranslationRegionSet& regions,
                                                         const InitialTranslationResourcePlan& translationPlan) noexcept
{
    constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
    constexpr Foundation::UInt64 PageSizeBytes{Foundation::UInt64{1U} << InitialTranslationPageShift};
    constexpr Foundation::UInt64 ExpectedRegionZeroRegister{(Foundation::UInt64{0x2000U} << 8U) | 0x30U};

    if (!mappingPlan.IsSealed() || !translationPlan.IsBuilt() ||
        (mappingPlan.GetMappingCount() != BootstrapInitialMappingCount))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < Platform::Ia64::Memory::InitialTranslationRegionCount; ++index)
    {
        const RegionConfiguration& region = regions.Regions[index];
        const auto retainedRegion = translationPlan.GetRegionRegister(index);
        const Foundation::UInt64 expectedRegionRegister = ((Foundation::UInt64{0x2000U} + index) << 8U) | 0x30U;
        if ((region.RegionIdentifier != (0x2000U + index)) || (region.ProtectionKey != 0U) ||
            region.VhptWalkerEnabled || (region.SharedAcrossAddressSpaces != (index == 0U)) ||
            (region.Reserved16 != 0U) || (region.Reserved32 != 0U) || !retainedRegion.HasValue() ||
            (retainedRegion.GetValue() != expectedRegionRegister))
        {
            return false;
        }
    }

    Foundation::Size entryIndex{0U};
    Foundation::UInt16 nextInstructionRegister{1U};
    for (Foundation::Size mappingIndex{0U}; mappingIndex < mappingPlan.GetMappingCount(); ++mappingIndex)
    {
        const auto mappingAccess = mappingPlan.GetMapping(mappingIndex);
        if (!mappingAccess.HasValue())
        {
            return false;
        }
        const auto mapping = mappingAccess.GetValue();
        const Foundation::UInt64 virtualStart = mapping.VirtualStart.GetValue();
        const Foundation::UInt64 pageCount = mapping.Pages.GetValue();
        if (!mapping.Permissions.Contains(MappingPermission::Global) || (pageCount == 0U) ||
            (pageCount > (MaximumUInt64 / PageSizeBytes)))
        {
            return false;
        }
        const Foundation::UInt64 sizeBytes = pageCount * PageSizeBytes;
        if ((virtualStart > (MaximumUInt64 - (sizeBytes - 1U))) || ((virtualStart >> 61U) != 0U) ||
            (((virtualStart + sizeBytes - 1U) >> 61U) != 0U))
        {
            return false;
        }

        Foundation::UInt64 coveredPageCount{0U};
        const bool executable = mapping.Permissions.Contains(MappingPermission::Execute);
        while (coveredPageCount < pageCount)
        {
            const auto entryAccess = translationPlan.GetEntry(entryIndex);
            if (!entryAccess.HasValue())
            {
                return false;
            }
            const auto entry = entryAccess.GetValue();
            if (entry.PageShift < InitialTranslationPageShift)
            {
                return false;
            }
            const Foundation::UInt8 pageShiftDifference =
                static_cast<Foundation::UInt8>(entry.PageShift - InitialTranslationPageShift);
            if (pageShiftDifference >= Foundation::UInt8{64U})
            {
                return false;
            }
            const Foundation::UInt64 entryPageCount = Foundation::UInt64{1U} << pageShiftDifference;
            const Foundation::UInt64 expectedFaultingAddress =
                virtualStart + (coveredPageCount << InitialTranslationPageShift);
            if ((entry.MappingIndex != mappingIndex) || (entry.MappingPageIndex != coveredPageCount) ||
                (entry.DataRegisterIndex != entryIndex) || (entryPageCount > (pageCount - coveredPageCount)) ||
                (entry.Translation.FaultingAddress != expectedFaultingAddress) ||
                ((entry.Translation.FaultingAddress >> 61U) != 0U) ||
                (entry.Translation.RegionRegister != ExpectedRegionZeroRegister) ||
                (entry.HasInstructionRegister != executable) ||
                (entry.InstructionRegisterIndex != (executable ? nextInstructionRegister : 0U)))
            {
                return false;
            }
            if (executable)
            {
                ++nextInstructionRegister;
            }
            coveredPageCount += entryPageCount;
            ++entryIndex;
        }
    }
    return (entryIndex == translationPlan.GetEntryCount()) && (translationPlan.GetDataRegisterCount() == entryIndex) &&
           (translationPlan.GetInstructionRegisterCount() == nextInstructionRegister);
}

[[nodiscard]] bool BytesEqual(const void* const first, const void* const second,
                              const Foundation::Size byteCount) noexcept
{
    const auto* const firstBytes = static_cast<const Foundation::UInt8*>(first);
    const auto* const secondBytes = static_cast<const Foundation::UInt8*>(second);
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        if (firstBytes[index] != secondBytes[index])
        {
            return false;
        }
    }
    return true;
}

void SnapshotAliasStorage(const void* const storage) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(storage);
    for (Foundation::Size index{0U}; index < sizeof(g_aliasSnapshot); ++index)
    {
        g_aliasSnapshot[index] = bytes[index];
    }
}

[[nodiscard]] bool AliasStorageMatchesSnapshot(const void* const storage) noexcept
{
    return BytesEqual(storage, g_aliasSnapshot, sizeof(g_aliasSnapshot));
}

void SnapshotInterruptVectorAliasStorage(const void* const storage) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(storage);
    for (Foundation::Size index{0U}; index < sizeof(g_ivtAliasSnapshot); ++index)
    {
        g_ivtAliasSnapshot[index] = bytes[index];
    }
}

[[nodiscard]] bool InterruptVectorAliasStorageMatchesSnapshot(const void* const storage) noexcept
{
    return BytesEqual(storage, g_ivtAliasSnapshot, sizeof(g_ivtAliasSnapshot));
}

[[nodiscard]] WritableRseBackingStoreView AliasedStorage(void* const storage) noexcept
{
    return WritableRseBackingStoreView{
        .Data = static_cast<Foundation::UInt8*>(storage),
        .SizeBytes = InitialRseBackingStoreSizeBytes,
        .PhysicalStart = ReservedBase + 0x1'8000U,
    };
}

[[nodiscard]] WritableInterruptVectorTableView AliasedInterruptVectorStorage(void* const storage) noexcept
{
    return WritableInterruptVectorTableView{
        .Data = static_cast<Foundation::UInt8*>(storage),
        .SizeBytes = InitialInterruptVectorTableSizeBytes,
        .PhysicalStart = ReservedBase + 0x1'0000U,
    };
}

[[nodiscard]] bool IsEmptyMappingFailure(const Platform::Ia64::Memory::BootstrapMappingPlanFailure& failure) noexcept
{
    return (failure.Error == BootstrapMappingPlanError::None) &&
           (failure.MappingError == InitialMappingPlanError::None) && (failure.MappingIndex == 0U);
}

[[nodiscard]] bool
IsEmptyTranslationFailure(const Platform::Ia64::Memory::InitialTranslationResourcePlanFailure& failure) noexcept
{
    return (failure.Error == InitialTranslationResourcePlanError::None) &&
           (failure.EncodingError == InitialTranslationEncodingError::None) && (failure.MappingIndex == 0U) &&
           (failure.MappingPageIndex == 0U);
}

struct PreparationInputSnapshot
{
    Ia64LoadedImageLayout Image;
    PhysicalPageRange ReservedPages;
    PhysicalPageRange RuntimeMemoryPages;
    PalVirtualMemoryCapabilities Capabilities;
    SalSystemTableInformation Firmware;
    InitialTranslationRegionSet Regions;
};

[[nodiscard]] bool InputsAreUnchanged(const Ia64LoadedImageLayout& image, const PhysicalPageRange& reservedPages,
                                      const PhysicalPageRange& runtimeMemoryPages,
                                      const PalVirtualMemoryCapabilities& capabilities,
                                      const SalSystemTableInformation& firmware,
                                      const InitialTranslationRegionSet& regions,
                                      const PreparationInputSnapshot& snapshot) noexcept
{
    return BytesEqual(&image, &snapshot.Image, sizeof(image)) &&
           BytesEqual(&g_bootInfo, &g_expectedBootInfo, sizeof(g_bootInfo)) &&
           BytesEqual(&reservedPages, &snapshot.ReservedPages, sizeof(reservedPages)) &&
           BytesEqual(&runtimeMemoryPages, &snapshot.RuntimeMemoryPages, sizeof(runtimeMemoryPages)) &&
           BytesEqual(&capabilities, &snapshot.Capabilities, sizeof(capabilities)) &&
           BytesEqual(&firmware, &snapshot.Firmware, sizeof(firmware)) &&
           BytesEqual(&regions, &snapshot.Regions, sizeof(regions));
}

void TestConsolidatedPreparation(TestContext& runner) noexcept
{
    const Foundation::UInt64 table = Platform::Ia64::Memory::GetStaticBootstrapInterruptVectorTableAddress();
    PhysicalPageRange dummy = ValidReservedPages();
    dummy.PhysicalStart = 0x03000000U;
    ASTERIA_EXPECT(runner, "hosted consolidated storage geometry is independent of host load bias",
                   TestSupport::MakeIa64HostedBootGeometry(table, ReservedBase, 16U * 1024U * 1024U, dummy,
                                                           g_consolidatedGeometry));
    g_consolidatedBootInfo = ValidBootInfo();
    g_consolidatedBootInfo.MemoryRegions[0U] = g_consolidatedGeometry.RuntimeMemoryRegion;
    g_consolidatedBootInfo.MemoryRegions[1U] = g_consolidatedGeometry.ImageRegion;
    ASTERIA_EXPECT(runner, "the consolidated final map has no overlap",
                   TestSupport::SortIa64HostedBootRegions(g_consolidatedBootInfo));
    PalVirtualMemoryCapabilities capabilities = ValidCapabilities();
    capabilities.DataTranslationRegisterCount = 8U;
    capabilities.InstructionTranslationRegisterCount = 8U;
    const WritableInterruptVectorTableView ivt{
        .Data = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(table)),
        .SizeBytes = InitialInterruptVectorTableSizeBytes,
        .PhysicalStart = table,
    };
    WritableRseBackingStoreView rse = ValidRseBackingStoreView();
    rse.PhysicalStart = ReservedBase + (16U * 1024U * 1024U) - InitialRseBackingStoreSizeBytes;
    FillRseBackingStore(0xA5U);
    const auto result = g_consolidatedPreparation.Prepare(
        g_consolidatedGeometry.Image, g_consolidatedBootInfo, ValidReservedPages(),
        g_consolidatedGeometry.RuntimeMemoryPages, capabilities, ValidFirmware(), ValidRegions(), ivt, rse);
    ASTERIA_EXPECT(
        runner, "consolidated preparation publishes the static IVT without modifying its literals", result.HasValue(),
        g_consolidatedPreparation.IsPrepared(), IsRseBackingStoreFilled(0U),
        Platform::Ia64::Memory::InitialInterruptVectorTableInstaller::Validate(g_consolidatedPreparation, ivt)
            .HasValue());
    const Platform::Ia64::Memory::WritableInitialMemoryStackView stack{
        .Data = g_consolidatedStack,
        .SizeBytes = sizeof(g_consolidatedStack),
        .PhysicalStart = ReservedBase,
    };
    ASTERIA_EXPECT(
        runner, "one runtime DTR covers the exact bootstrap stack and RSE subranges",
        Platform::Ia64::Memory::InitialExecutionContextSwitcher::Validate(g_consolidatedPreparation, stack, rse)
            .HasValue());
    const auto summary = g_consolidatedPreparation.TryGetSummary();
    ASTERIA_EXPECT(runner, "the upward-growing RSE ends at the runtime page upper guard", summary.HasValue(),
                   Platform::Ia64::Memory::GetBootstrapRseBackingStorePhysicalStart(summary.GetValue()->Mapping) +
                           InitialRseBackingStoreSizeBytes ==
                       ReservedBase + (16U * 1024U * 1024U));
    WritableRseBackingStoreView shiftedRse = rse;
    shiftedRse.PhysicalStart += 4096U;
    ASTERIA_EXPECT(
        runner, "containment in the runtime page cannot substitute an inexact RSE address",
        !Platform::Ia64::Memory::InitialExecutionContextSwitcher::Validate(g_consolidatedPreparation, stack, shiftedRse)
             .HasValue(),
        g_nativeAdapterCallCount == 0U);
}

void TestSuccessfulAtomicPublication(TestContext& runner) noexcept
{
    FillRseBackingStore(0xA5U);
    const Ia64LoadedImageLayout image = ValidImage();
    const PhysicalPageRange reservedPages = ValidReservedPages();
    const PhysicalPageRange runtimeMemoryPages = ValidRuntimeMemoryPages();
    const PalVirtualMemoryCapabilities capabilities = ValidCapabilities();
    const SalSystemTableInformation firmware = ValidFirmware();
    const InitialTranslationRegionSet regions = ValidRegions();
    const PreparationInputSnapshot inputSnapshot{
        image, reservedPages, runtimeMemoryPages, capabilities, firmware, regions,
    };

    const auto freshSummary = g_successPreparation.TryGetSummary();
    const auto freshMappingPlan = g_successPreparation.TryGetMappingPlan();
    const auto freshTranslationPlan = g_successPreparation.TryGetTranslationResourcePlan();

    ASTERIA_EXPECT(runner, "a fresh owner exposes none of its unpublished child state",
                   !g_successPreparation.IsPrepared(), !g_successPreparation.HasFailed(), !freshSummary.HasValue(),
                   freshSummary.GetError() == InitialAddressSpacePreparationError::NotPrepared,
                   !freshMappingPlan.HasValue(),
                   freshMappingPlan.GetError() == InitialAddressSpacePreparationError::NotPrepared,
                   !freshTranslationPlan.HasValue(),
                   freshTranslationPlan.GetError() == InitialAddressSpacePreparationError::NotPrepared);

    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    const auto result =
        g_successPreparation.Prepare(image, g_bootInfo, reservedPages, runtimeMemoryPages, capabilities, firmware,
                                     regions, ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "both child plans publish as one prepared state", result.HasValue(),
                   result.GetValue() == InitialAddressSpacePreparationStatus::Prepared,
                   g_successPreparation.IsPrepared(), !g_successPreparation.HasFailed());
    ASTERIA_EXPECT(runner, "prepared publication initializes the complete IVT and RSE storage",
                   IsFailStopInterruptVectorTable(), IsRseBackingStoreFilled(0U));

    const auto summaryAccess = g_successPreparation.TryGetSummary();
    const auto mappingPlanAccess = g_successPreparation.TryGetMappingPlan();
    const auto translationPlanAccess = g_successPreparation.TryGetTranslationResourcePlan();
    const bool allViewsPublished =
        summaryAccess.HasValue() && mappingPlanAccess.HasValue() && translationPlanAccess.HasValue();
    ASTERIA_EXPECT(runner, "a prepared owner publishes all three read-only views together", allViewsPublished);
    if (!allViewsPublished)
    {
        return;
    }
    const auto& summary = *summaryAccess.GetValue();
    const auto& mappingPlan = *mappingPlanAccess.GetValue();
    const auto& translationPlan = *translationPlanAccess.GetValue();
    ASTERIA_EXPECT(runner, "the prepared view retains the exact sealed mapping summary",
                   summary.Mapping.StackPhysicalStart == ReservedBase,
                   summary.Mapping.TranslationStoragePhysicalStart == (ReservedBase + 0x8000U),
                   summary.Mapping.MappingCount == BootstrapInitialMappingCount, summary.Mapping.Reserved16 == 0U,
                   summary.Mapping.Reserved32 == 0U, mappingPlan.IsSealed(),
                   mappingPlan.GetMappingCount() == BootstrapInitialMappingCount);
    const auto interruptVectorTable = mappingPlan.GetMapping(2U);
    ASTERIA_EXPECT(runner, "the prepared mapping view owns the exact fail-stop IVT range",
                   interruptVectorTable.HasValue(),
                   interruptVectorTable.GetValue().VirtualStart.GetValue() == (ReservedBase + 0x1'0000U),
                   interruptVectorTable.GetValue().PhysicalStart.GetValue() == (ReservedBase + 0x1'0000U),
                   interruptVectorTable.GetValue().Pages.GetValue() == 8U,
                   interruptVectorTable.GetValue().Permissions.Contains(MappingPermission::Read),
                   !interruptVectorTable.GetValue().Permissions.Contains(MappingPermission::Write),
                   interruptVectorTable.GetValue().Permissions.Contains(MappingPermission::Execute),
                   interruptVectorTable.GetValue().Permissions.Contains(MappingPermission::Global),
                   interruptVectorTable.GetValue().CachePolicy == MemoryCachePolicy::WriteBack,
                   interruptVectorTable.GetValue().Owner == InitialMappingOwner::NucleusInterruptVectorTable);
    const auto rseBackingStore = mappingPlan.GetMapping(3U);
    ASTERIA_EXPECT(runner, "the prepared mapping view owns the exact inert RSE backing-store range",
                   rseBackingStore.HasValue(),
                   rseBackingStore.GetValue().VirtualStart.GetValue() == (ReservedBase + 0x1'8000U),
                   rseBackingStore.GetValue().PhysicalStart.GetValue() == (ReservedBase + 0x1'8000U),
                   rseBackingStore.GetValue().Pages.GetValue() == 8U,
                   rseBackingStore.GetValue().Permissions.Contains(MappingPermission::Read),
                   rseBackingStore.GetValue().Permissions.Contains(MappingPermission::Write),
                   !rseBackingStore.GetValue().Permissions.Contains(MappingPermission::Execute),
                   rseBackingStore.GetValue().Permissions.Contains(MappingPermission::Global),
                   rseBackingStore.GetValue().CachePolicy == MemoryCachePolicy::WriteBack,
                   rseBackingStore.GetValue().Owner == InitialMappingOwner::NucleusRegisterBackingStore);
    ASTERIA_EXPECT(runner, "the prepared view retains the exact built translation summary",
                   summary.Translation.EntryCount == 14U, summary.Translation.DataRegisterCount == 14U,
                   summary.Translation.InstructionRegisterCount == 5U, summary.Translation.Reserved == 0U,
                   translationPlan.IsBuilt(), translationPlan.GetEntryCount() == summary.Translation.EntryCount,
                   translationPlan.GetDataRegisterCount() == summary.Translation.DataRegisterCount,
                   translationPlan.GetInstructionRegisterCount() == summary.Translation.InstructionRegisterCount);
    ASTERIA_EXPECT(runner, "all eight Global RR0 mappings have contiguous DTR and executable ITR coverage",
                   HasExactRegionZeroTranslationCoverage(mappingPlan, regions, translationPlan));

    FillInterruptVectorTable(0x5AU);
    FillRseBackingStore(0x5AU);
    const auto repeated =
        g_successPreparation.Prepare(image, g_bootInfo, reservedPages, runtimeMemoryPages, capabilities, firmware,
                                     regions, ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "a prepared owner rejects replacement without changing publication or storage",
                   !repeated.HasValue(), repeated.GetError() == InitialAddressSpacePreparationError::AlreadyPrepared,
                   g_successPreparation.IsPrepared(), summary.Translation.EntryCount == 14U,
                   IsInterruptVectorTableFilled(0x5AU), IsRseBackingStoreFilled(0x5AU));

    ASTERIA_EXPECT(
        runner, "successful preparation leaves every input byte unchanged",
        InputsAreUnchanged(image, reservedPages, runtimeMemoryPages, capabilities, firmware, regions, inputSnapshot));

    FillInterruptVectorTable(0x5AU);
    FillRseBackingStore(0x5AU);
    const auto deterministic =
        g_deterministicPreparation.Prepare(image, g_bootInfo, reservedPages, runtimeMemoryPages, capabilities, firmware,
                                           regions, ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "identical inputs produce byte-identical owned preparation state", deterministic.HasValue(),
                   IsFailStopInterruptVectorTable(), IsRseBackingStoreFilled(0U),
                   BytesEqual(&g_successPreparation, &g_deterministicPreparation, PreparationBytes));
}

void TestMappingFailureIsTerminal(TestContext& runner) noexcept
{
    const Ia64LoadedImageLayout image = ValidImage();
    PhysicalPageRange invalidReservedPages = ValidReservedPages();
    invalidReservedPages.PageCount = BootstrapReservedPageCount - 1U;
    const PhysicalPageRange runtimeMemoryPages = ValidRuntimeMemoryPages();
    const PalVirtualMemoryCapabilities capabilities = ValidCapabilities();
    const SalSystemTableInformation firmware = ValidFirmware();
    const InitialTranslationRegionSet regions = ValidRegions();
    const PreparationInputSnapshot inputSnapshot{
        image, invalidReservedPages, runtimeMemoryPages, capabilities, firmware, regions,
    };

    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    const auto result = g_mappingFailurePreparation.Prepare(
        image, g_bootInfo, invalidReservedPages, runtimeMemoryPages, capabilities, firmware, regions,
        ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "mapping rejection enters the terminal failed state", !result.HasValue(),
                   result.GetError() == InitialAddressSpacePreparationError::MappingPlanRejected,
                   !g_mappingFailurePreparation.IsPrepared(), g_mappingFailurePreparation.HasFailed(),
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    const auto& failure = g_mappingFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "mapping rejection preserves the exact nested child failure",
                   failure.Error == InitialAddressSpacePreparationError::MappingPlanRejected,
                   failure.RseBackingStoreError == InitialRseBackingStorePreparationError::None,
                   failure.MappingFailure.Error == BootstrapMappingPlanError::InvalidReservedPages,
                   failure.MappingFailure.MappingError == InitialMappingPlanError::None,
                   failure.MappingFailure.MappingIndex == 0U, IsEmptyTranslationFailure(failure.TranslationFailure));

    const auto unavailableSummary = g_mappingFailurePreparation.TryGetSummary();
    const auto unavailableMapping = g_mappingFailurePreparation.TryGetMappingPlan();
    const auto unavailableTranslation = g_mappingFailurePreparation.TryGetTranslationResourcePlan();
    ASTERIA_EXPECT(runner, "a terminal mapping failure exposes no partial child publication",
                   !unavailableSummary.HasValue(),
                   unavailableSummary.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   !unavailableMapping.HasValue(),
                   unavailableMapping.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   !unavailableTranslation.HasValue(),
                   unavailableTranslation.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed);

    ASTERIA_EXPECT(runner, "mapping rejection leaves every input byte unchanged",
                   InputsAreUnchanged(image, invalidReservedPages, runtimeMemoryPages, capabilities, firmware, regions,
                                      inputSnapshot));

    const auto repeated = g_mappingFailurePreparation.Prepare(
        image, g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), capabilities, firmware, regions,
        ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "a failed mapping preparation cannot be retried or have its cause replaced",
                   !repeated.HasValue(),
                   repeated.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   g_mappingFailurePreparation.GetFailure().MappingFailure.Error ==
                       BootstrapMappingPlanError::InvalidReservedPages);
}

void TestResourceFailureIsTerminal(TestContext& runner) noexcept
{
    const Ia64LoadedImageLayout image = ValidImage();
    const PhysicalPageRange reservedPages = ValidReservedPages();
    const PhysicalPageRange runtimeMemoryPages = ValidRuntimeMemoryPages();
    PalVirtualMemoryCapabilities invalidCapabilities = ValidCapabilities();
    invalidCapabilities.Translation.PhysicalAddressBits = 31U;
    const SalSystemTableInformation firmware = ValidFirmware();
    const InitialTranslationRegionSet regions = ValidRegions();
    const PreparationInputSnapshot inputSnapshot{
        image, reservedPages, runtimeMemoryPages, invalidCapabilities, firmware, regions,
    };

    WritableRseBackingStoreView shortStorage = ValidRseBackingStoreView();
    --shortStorage.SizeBytes;
    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    const auto result =
        g_resourceFailurePreparation.Prepare(image, g_bootInfo, reservedPages, runtimeMemoryPages, invalidCapabilities,
                                             firmware, regions, ValidInterruptVectorTableView(), shortStorage);
    ASTERIA_EXPECT(runner, "resource rejection precedes invalid RSE storage without writing it", !result.HasValue(),
                   result.GetError() == InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
                   !g_resourceFailurePreparation.IsPrepared(), g_resourceFailurePreparation.HasFailed(),
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    const auto& failure = g_resourceFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "resource rejection preserves the exact nested child failure",
                   failure.Error == InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
                   failure.RseBackingStoreError == InitialRseBackingStorePreparationError::None,
                   IsEmptyMappingFailure(failure.MappingFailure),
                   failure.TranslationFailure.Error == InitialTranslationResourcePlanError::InvalidCapabilities,
                   failure.TranslationFailure.EncodingError == InitialTranslationEncodingError::None,
                   failure.TranslationFailure.MappingIndex == ~Foundation::Size{0U},
                   failure.TranslationFailure.MappingPageIndex == 0U);

    const auto unavailableSummary = g_resourceFailurePreparation.TryGetSummary();
    const auto unavailableMapping = g_resourceFailurePreparation.TryGetMappingPlan();
    const auto unavailableTranslation = g_resourceFailurePreparation.TryGetTranslationResourcePlan();
    ASTERIA_EXPECT(runner, "a terminal resource failure exposes no intermediate child view",
                   !unavailableSummary.HasValue(),
                   unavailableSummary.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   !unavailableMapping.HasValue(),
                   unavailableMapping.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   !unavailableTranslation.HasValue(),
                   unavailableTranslation.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed);

    ASTERIA_EXPECT(runner, "resource rejection leaves every input byte unchanged",
                   InputsAreUnchanged(image, reservedPages, runtimeMemoryPages, invalidCapabilities, firmware, regions,
                                      inputSnapshot));

    const auto repeated = g_resourceFailurePreparation.Prepare(
        image, g_bootInfo, reservedPages, runtimeMemoryPages, ValidCapabilities(), firmware, regions,
        ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "resource failure is terminal even when a later capability set is valid",
                   !repeated.HasValue(),
                   repeated.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   g_resourceFailurePreparation.GetFailure().TranslationFailure.Error ==
                       InitialTranslationResourcePlanError::InvalidCapabilities);
}

void TestFailurePriority(TestContext& runner) noexcept
{
    PhysicalPageRange invalidReservedPages = ValidReservedPages();
    invalidReservedPages.PageCount = BootstrapReservedPageCount - 1U;
    PalVirtualMemoryCapabilities invalidCapabilities = ValidCapabilities();
    invalidCapabilities.Translation.PhysicalAddressBits = 31U;

    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    const auto result = g_priorityPreparation.Prepare(ValidImage(), g_bootInfo, invalidReservedPages,
                                                      ValidRuntimeMemoryPages(), invalidCapabilities, ValidFirmware(),
                                                      ValidRegions(), ValidInterruptVectorTableView(),
                                                      WritableRseBackingStoreView{
                                                          .Data = nullptr,
                                                          .SizeBytes = 0U,
                                                          .PhysicalStart = 0U,
                                                      });
    ASTERIA_EXPECT(
        runner, "mapping validation deterministically precedes resource validation", !result.HasValue(),
        result.GetError() == InitialAddressSpacePreparationError::MappingPlanRejected,
        g_priorityPreparation.GetFailure().MappingFailure.Error == BootstrapMappingPlanError::InvalidReservedPages,
        g_priorityPreparation.GetFailure().RseBackingStoreError == InitialRseBackingStorePreparationError::None,
        IsEmptyTranslationFailure(g_priorityPreparation.GetFailure().TranslationFailure),
        IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));
}

void TestRseFailureIsTerminal(TestContext& runner) noexcept
{
    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    WritableRseBackingStoreView shortStorage = ValidRseBackingStoreView();
    --shortStorage.SizeBytes;
    const auto result = g_rseFailurePreparation.Prepare(ValidImage(), g_bootInfo, ValidReservedPages(),
                                                        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
                                                        ValidRegions(), ValidInterruptVectorTableView(), shortStorage);
    ASTERIA_EXPECT(runner, "invalid RSE storage fails atomically after both child plans", !result.HasValue(),
                   result.GetError() == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   !g_rseFailurePreparation.IsPrepared(), g_rseFailurePreparation.HasFailed(),
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    const auto& failure = g_rseFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "RSE storage rejection preserves its exact typed cause",
                   failure.Error == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   failure.RseBackingStoreError == InitialRseBackingStorePreparationError::InvalidStorageView,
                   IsEmptyMappingFailure(failure.MappingFailure),
                   IsEmptyTranslationFailure(failure.TranslationFailure));

    const auto repeated = g_rseFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "an RSE storage failure is terminal and cannot later zero memory", !repeated.HasValue(),
                   repeated.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   IsRseBackingStoreFilled(0xA5U));

    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    WritableRseBackingStoreView nullStorage = ValidRseBackingStoreView();
    nullStorage.Data = nullptr;
    const auto nullResult = g_nullRseFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), nullStorage);
    const auto& nullFailure = g_nullRseFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "a null RSE view retains RSE-owned error provenance atomically", !nullResult.HasValue(),
                   nullResult.GetError() == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   !g_nullRseFailurePreparation.IsPrepared(), g_nullRseFailurePreparation.HasFailed(),
                   nullFailure.Error == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   nullFailure.InterruptVectorTableError == InitialInterruptVectorTablePreparationError::None,
                   nullFailure.RseBackingStoreError == InitialRseBackingStorePreparationError::InvalidStorageView,
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    WritableRseBackingStoreView wrappingStorage = ValidRseBackingStoreView();
    wrappingStorage.Data = reinterpret_cast<Foundation::UInt8*>(~Foundation::UIntPointer{0U} &
                                                                ~static_cast<Foundation::UIntPointer>(0xFFFU));
    const auto wrappingResult = g_wrappingRseFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), wrappingStorage);
    const auto& wrappingFailure = g_wrappingRseFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "a wrapping RSE view retains RSE-owned error provenance atomically",
                   !wrappingResult.HasValue(),
                   wrappingResult.GetError() == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   !g_wrappingRseFailurePreparation.IsPrepared(), g_wrappingRseFailurePreparation.HasFailed(),
                   wrappingFailure.Error == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   wrappingFailure.InterruptVectorTableError == InitialInterruptVectorTablePreparationError::None,
                   wrappingFailure.RseBackingStoreError == InitialRseBackingStorePreparationError::InvalidStorageView,
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));
}

void TestInterruptVectorTableFailureIsTerminal(TestContext& runner) noexcept
{
    FillInterruptVectorTable(0xA5U);
    FillRseBackingStore(0xA5U);
    WritableInterruptVectorTableView shortStorage = ValidInterruptVectorTableView();
    --shortStorage.SizeBytes;
    const auto result = g_ivtFailurePreparation.Prepare(ValidImage(), g_bootInfo, ValidReservedPages(),
                                                        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
                                                        ValidRegions(), shortStorage, ValidRseBackingStoreView());
    const auto& failure = g_ivtFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "invalid IVT storage fails without publishing either storage range", !result.HasValue(),
                   result.GetError() == InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
                   !g_ivtFailurePreparation.IsPrepared(), g_ivtFailurePreparation.HasFailed(),
                   failure.Error == InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
                   failure.InterruptVectorTableError == InitialInterruptVectorTablePreparationError::InvalidStorageView,
                   failure.RseBackingStoreError == InitialRseBackingStorePreparationError::None,
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    const auto repeated = g_ivtFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "an IVT storage failure is terminal and cannot later publish memory", !repeated.HasValue(),
                   repeated.GetError() == InitialAddressSpacePreparationError::PreviousPreparationFailed,
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    WritableInterruptVectorTableView mismatched = ValidInterruptVectorTableView();
    mismatched.PhysicalStart += 0x8000U;
    const auto addressResult = g_ivtAddressFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), mismatched, ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "parent preparation forwards an IVT physical mismatch exactly", !addressResult.HasValue(),
                   addressResult.GetError() ==
                       InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
                   g_ivtAddressFailurePreparation.GetFailure().InterruptVectorTableError ==
                       InitialInterruptVectorTablePreparationError::StorageAddressMismatch,
                   IsInterruptVectorTableFilled(0xA5U), IsRseBackingStoreFilled(0xA5U));

    auto* const aliasedBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtCapabilitiesAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        aliasedBytes[index] = 0xA5U;
    }
    g_ivtCapabilitiesAliasBlock.Value = ValidCapabilities();
    SnapshotInterruptVectorAliasStorage(&g_ivtCapabilitiesAliasBlock);
    const auto inputAliasResult =
        g_ivtInputAliasPreparation.Prepare(ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
                                           g_ivtCapabilitiesAliasBlock.Value, ValidFirmware(), ValidRegions(),
                                           WritableInterruptVectorTableView{
                                               .Data = aliasedBytes,
                                               .SizeBytes = InitialInterruptVectorTableSizeBytes,
                                               .PhysicalStart = ReservedBase + 0x1'0000U,
                                           },
                                           ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT storage cannot alias a retained const input", !inputAliasResult.HasValue(),
                   inputAliasResult.GetError() ==
                       InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
                   g_ivtInputAliasPreparation.GetFailure().InterruptVectorTableError ==
                       InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtCapabilitiesAliasBlock));

    WritableRseBackingStoreView overlappingRse = ValidRseBackingStoreView();
    overlappingRse.Data = g_interruptVectorTable + 0x4000U;
    FillInterruptVectorTable(0xA5U);
    const auto overlapResult = g_ivtOverlapPreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), overlappingRse);
    ASTERIA_EXPECT(
        runner, "overlapping IVT and RSE writable views are rejected without writes", !overlapResult.HasValue(),
        overlapResult.GetError() == InitialAddressSpacePreparationError::InterruptVectorTablePreparationRejected,
        g_ivtOverlapPreparation.GetFailure().InterruptVectorTableError ==
            InitialInterruptVectorTablePreparationError::StorageAliasesInput,
        IsInterruptVectorTableFilled(0xA5U));

    auto* const ownerBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtOwnerAliasBlock.Value);
    SnapshotInterruptVectorAliasStorage(ownerBytes);
    const auto ownerAliasResult =
        g_ivtOwnerAliasBlock.Value.Prepare(ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
                                           ValidCapabilities(), ValidFirmware(), ValidRegions(),
                                           WritableInterruptVectorTableView{
                                               .Data = ownerBytes,
                                               .SizeBytes = InitialInterruptVectorTableSizeBytes,
                                               .PhysicalStart = ReservedBase + 0x1'0000U,
                                           },
                                           ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "an IVT owner alias is rejected before the transaction begins", !ownerAliasResult.HasValue(),
                   ownerAliasResult.GetError() ==
                       InitialAddressSpacePreparationError::InterruptVectorTablePreflightRejected,
                   !g_ivtOwnerAliasBlock.Value.IsPrepared(), !g_ivtOwnerAliasBlock.Value.HasFailed(),
                   InterruptVectorAliasStorageMatchesSnapshot(ownerBytes));

    FillInterruptVectorTable(0x5AU);
    FillRseBackingStore(0x5AU);
    const auto ownerRetry = g_ivtOwnerAliasBlock.Value.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "a non-consuming IVT owner-alias rejection permits one valid retry", ownerRetry.HasValue(),
                   g_ivtOwnerAliasBlock.Value.IsPrepared(), IsFailStopInterruptVectorTable(),
                   IsRseBackingStoreFilled(0U));
}

void TestParentStorageIsolation(TestContext& runner) noexcept
{
    FillRseBackingStore(0xA5U);
    WritableRseBackingStoreView mismatched = ValidRseBackingStoreView();
    mismatched.PhysicalStart += 0x1000U;
    const auto addressResult = g_rseAddressFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), mismatched);
    ASTERIA_EXPECT(runner, "parent preparation forwards a physical-address mismatch exactly", !addressResult.HasValue(),
                   addressResult.GetError() == InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   g_rseAddressFailurePreparation.HasFailed(),
                   g_rseAddressFailurePreparation.GetFailure().RseBackingStoreError ==
                       InitialRseBackingStorePreparationError::StorageAddressMismatch,
                   IsRseBackingStoreFilled(0xA5U));

    auto* const aliasedBytes = reinterpret_cast<Foundation::UInt8*>(&g_capabilitiesAliasBlock);
    for (Foundation::Size index{0U}; index < InitialRseBackingStoreSizeBytes; ++index)
    {
        aliasedBytes[index] = 0xA5U;
    }
    g_capabilitiesAliasBlock.Value = ValidCapabilities();
    SnapshotAliasStorage(&g_capabilitiesAliasBlock);
    const auto inputAliasResult = g_inputAliasPreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), g_capabilitiesAliasBlock.Value,
        ValidFirmware(), ValidRegions(), ValidInterruptVectorTableView(),
        WritableRseBackingStoreView{
            .Data = aliasedBytes,
            .SizeBytes = InitialRseBackingStoreSizeBytes,
            .PhysicalStart = ReservedBase + 0x1'8000U,
        });
    ASTERIA_EXPECT(runner, "RSE storage cannot alias a const preparation input", !inputAliasResult.HasValue(),
                   inputAliasResult.GetError() ==
                       InitialAddressSpacePreparationError::RseBackingStorePreparationRejected,
                   g_inputAliasPreparation.HasFailed(),
                   g_inputAliasPreparation.GetFailure().RseBackingStoreError ==
                       InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_capabilitiesAliasBlock));

    auto* const ownerBytes = reinterpret_cast<Foundation::UInt8*>(&g_ownerAliasBlock.Value);
    for (Foundation::Size index{0U}; index < InitialRseBackingStoreSizeBytes; ++index)
    {
        g_aliasSnapshot[index] = ownerBytes[index];
    }
    const auto ownerAliasResult = g_ownerAliasBlock.Value.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(),
        WritableRseBackingStoreView{
            .Data = ownerBytes,
            .SizeBytes = InitialRseBackingStoreSizeBytes,
            .PhysicalStart = ReservedBase + 0x1'8000U,
        });
    ASTERIA_EXPECT(runner, "an owner alias is rejected without starting or modifying preparation",
                   !ownerAliasResult.HasValue(),
                   ownerAliasResult.GetError() == InitialAddressSpacePreparationError::RseBackingStorePreflightRejected,
                   !g_ownerAliasBlock.Value.IsPrepared(), !g_ownerAliasBlock.Value.HasFailed(),
                   BytesEqual(ownerBytes, g_aliasSnapshot, InitialRseBackingStoreSizeBytes));

    FillRseBackingStore(0x5AU);
    const auto ownerRetry = g_ownerAliasBlock.Value.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "a non-consuming owner-alias rejection permits one valid preparation", ownerRetry.HasValue(),
                   g_ownerAliasBlock.Value.IsPrepared(), IsRseBackingStoreFilled(0U));
}

void TestPreflightInputAliasMatrix(TestContext& runner) noexcept
{
    g_imageAliasBlock.Value = ValidImage();
    SnapshotAliasStorage(&g_imageAliasBlock);
    const auto imageError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, g_imageAliasBlock.Value, g_bootInfo, ValidReservedPages(),
        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedStorage(&g_imageAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the loaded-image input",
                   imageError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_imageAliasBlock));

    g_bootInfoAliasBlock.Value = ValidBootInfo();
    SnapshotAliasStorage(&g_bootInfoAliasBlock);
    const auto bootInfoError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfoAliasBlock.Value, ValidReservedPages(),
        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedStorage(&g_bootInfoAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the final BootInfo input",
                   bootInfoError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_bootInfoAliasBlock));

    g_reservedPagesAliasBlock.Value = ValidReservedPages();
    SnapshotAliasStorage(&g_reservedPagesAliasBlock);
    const auto reservedPagesError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, g_reservedPagesAliasBlock.Value,
        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedStorage(&g_reservedPagesAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the reservation input",
                   reservedPagesError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_reservedPagesAliasBlock));

    g_runtimeMemoryPagesAliasBlock.Value = ValidRuntimeMemoryPages();
    SnapshotAliasStorage(&g_runtimeMemoryPagesAliasBlock);
    const auto runtimeMemoryPagesError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(),
        g_runtimeMemoryPagesAliasBlock.Value, ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedStorage(&g_runtimeMemoryPagesAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the run-time memory reservation input",
                   runtimeMemoryPagesError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_runtimeMemoryPagesAliasBlock));

    g_capabilitiesAliasBlock.Value = ValidCapabilities();
    SnapshotAliasStorage(&g_capabilitiesAliasBlock);
    const auto capabilitiesError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        g_capabilitiesAliasBlock.Value, ValidFirmware(), ValidRegions(), AliasedStorage(&g_capabilitiesAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the PAL capability input",
                   capabilitiesError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_capabilitiesAliasBlock));

    g_firmwareAliasBlock.Value = ValidFirmware();
    SnapshotAliasStorage(&g_firmwareAliasBlock);
    const auto firmwareError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), g_firmwareAliasBlock.Value, ValidRegions(), AliasedStorage(&g_firmwareAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the SAL firmware input",
                   firmwareError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_firmwareAliasBlock));

    g_regionsAliasBlock.Value = ValidRegions();
    SnapshotAliasStorage(&g_regionsAliasBlock);
    const auto regionsError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), ValidFirmware(), g_regionsAliasBlock.Value, AliasedStorage(&g_regionsAliasBlock));
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases the region-set input",
                   regionsError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_regionsAliasBlock));

    g_storageViewAliasBlock.Value = AliasedStorage(&g_storageViewAliasBlock);
    SnapshotAliasStorage(&g_storageViewAliasBlock);
    const auto storageViewError = InitialRseBackingStorePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), ValidFirmware(), ValidRegions(), g_storageViewAliasBlock.Value);
    ASTERIA_EXPECT(runner, "preflight rejects storage that aliases its own view descriptor",
                   storageViewError == InitialRseBackingStorePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_storageViewAliasBlock));

    auto* const ivtImageBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtImageAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtImageBytes[index] = 0xA5U;
    }
    g_ivtImageAliasBlock.Value = ValidImage();
    SnapshotInterruptVectorAliasStorage(&g_ivtImageAliasBlock);
    const auto ivtImageError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, g_ivtImageAliasBlock.Value, g_bootInfo, ValidReservedPages(),
        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedInterruptVectorStorage(&g_ivtImageAliasBlock), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases the loaded-image input",
                   ivtImageError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtImageAliasBlock));

    auto* const ivtBootInfoBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtBootInfoAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtBootInfoBytes[index] = 0xA5U;
    }
    g_ivtBootInfoAliasBlock.Value = ValidBootInfo();
    SnapshotInterruptVectorAliasStorage(&g_ivtBootInfoAliasBlock);
    const auto ivtBootInfoError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_ivtBootInfoAliasBlock.Value, ValidReservedPages(),
        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedInterruptVectorStorage(&g_ivtBootInfoAliasBlock), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases the BootInfo input",
                   ivtBootInfoError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtBootInfoAliasBlock));

    auto* const ivtReservedPagesBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtReservedPagesAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtReservedPagesBytes[index] = 0xA5U;
    }
    g_ivtReservedPagesAliasBlock.Value = ValidReservedPages();
    SnapshotInterruptVectorAliasStorage(&g_ivtReservedPagesAliasBlock);
    const auto ivtReservedPagesError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, g_ivtReservedPagesAliasBlock.Value,
        ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedInterruptVectorStorage(&g_ivtReservedPagesAliasBlock), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases the reservation input",
                   ivtReservedPagesError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtReservedPagesAliasBlock));

    auto* const ivtRuntimeMemoryPagesBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtRuntimeMemoryPagesAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtRuntimeMemoryPagesBytes[index] = 0xA5U;
    }
    g_ivtRuntimeMemoryPagesAliasBlock.Value = ValidRuntimeMemoryPages();
    SnapshotInterruptVectorAliasStorage(&g_ivtRuntimeMemoryPagesAliasBlock);
    const auto ivtRuntimeMemoryPagesError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(),
        g_ivtRuntimeMemoryPagesAliasBlock.Value, ValidCapabilities(), ValidFirmware(), ValidRegions(),
        AliasedInterruptVectorStorage(&g_ivtRuntimeMemoryPagesAliasBlock), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases the run-time memory reservation input",
                   ivtRuntimeMemoryPagesError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtRuntimeMemoryPagesAliasBlock));

    auto* const ivtFirmwareBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtFirmwareAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtFirmwareBytes[index] = 0xA5U;
    }
    g_ivtFirmwareAliasBlock.Value = ValidFirmware();
    SnapshotInterruptVectorAliasStorage(&g_ivtFirmwareAliasBlock);
    const auto ivtFirmwareError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), g_ivtFirmwareAliasBlock.Value, ValidRegions(),
        AliasedInterruptVectorStorage(&g_ivtFirmwareAliasBlock), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases the SAL firmware input",
                   ivtFirmwareError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtFirmwareAliasBlock));

    auto* const ivtRegionsBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtRegionsAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtRegionsBytes[index] = 0xA5U;
    }
    g_ivtRegionsAliasBlock.Value = ValidRegions();
    SnapshotInterruptVectorAliasStorage(&g_ivtRegionsAliasBlock);
    const auto ivtRegionsError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), ValidFirmware(), g_ivtRegionsAliasBlock.Value,
        AliasedInterruptVectorStorage(&g_ivtRegionsAliasBlock), ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases the region-set input",
                   ivtRegionsError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtRegionsAliasBlock));

    auto* const ivtStorageViewBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtStorageViewAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtStorageViewBytes[index] = 0xA5U;
    }
    g_ivtStorageViewAliasBlock.Value = AliasedInterruptVectorStorage(&g_ivtStorageViewAliasBlock);
    SnapshotInterruptVectorAliasStorage(&g_ivtStorageViewAliasBlock);
    const auto ivtStorageViewError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), ValidFirmware(), ValidRegions(), g_ivtStorageViewAliasBlock.Value,
        ValidRseBackingStoreView());
    ASTERIA_EXPECT(runner, "IVT preflight rejects storage that aliases its own view descriptor",
                   ivtStorageViewError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtStorageViewAliasBlock));

    auto* const ivtRseViewBytes = reinterpret_cast<Foundation::UInt8*>(&g_ivtRseViewAliasBlock);
    for (Foundation::Size index{0U}; index < InitialInterruptVectorTableSizeBytes; ++index)
    {
        ivtRseViewBytes[index] = 0xA5U;
    }
    g_ivtRseViewAliasBlock.Value = ValidRseBackingStoreView();
    SnapshotInterruptVectorAliasStorage(&g_ivtRseViewAliasBlock);
    const auto ivtRseViewError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), ValidFirmware(), ValidRegions(), AliasedInterruptVectorStorage(&g_ivtRseViewAliasBlock),
        g_ivtRseViewAliasBlock.Value);
    ASTERIA_EXPECT(runner, "IVT storage cannot alias the RSE view descriptor",
                   ivtRseViewError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   InterruptVectorAliasStorageMatchesSnapshot(&g_ivtRseViewAliasBlock));

    auto* const rseIvtViewBytes = reinterpret_cast<Foundation::UInt8*>(&g_rseIvtViewAliasBlock);
    for (Foundation::Size index{0U}; index < InitialRseBackingStoreSizeBytes; ++index)
    {
        rseIvtViewBytes[index] = 0xA5U;
    }
    g_rseIvtViewAliasBlock.Value = ValidInterruptVectorTableView();
    SnapshotAliasStorage(&g_rseIvtViewAliasBlock);
    const auto rseIvtViewError = InitialInterruptVectorTablePreparer::ValidatePreparationStorage(
        g_preflightAliasPreparation, ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(),
        ValidCapabilities(), ValidFirmware(), ValidRegions(), g_rseIvtViewAliasBlock.Value,
        AliasedStorage(&g_rseIvtViewAliasBlock));
    ASTERIA_EXPECT(runner, "RSE storage cannot alias the IVT view descriptor",
                   rseIvtViewError == InitialInterruptVectorTablePreparationError::StorageAliasesInput,
                   AliasStorageMatchesSnapshot(&g_rseIvtViewAliasBlock));

    ASTERIA_EXPECT(runner, "non-writing alias validation leaves the preparation owner fresh",
                   !g_preflightAliasPreparation.IsPrepared(), !g_preflightAliasPreparation.HasFailed());
}

void TestDetailedFailureForwarding(TestContext& runner) noexcept
{
    Ia64LoadedImageLayout invalidImage = ValidImage();
    invalidImage.Sections[1U].VirtualStart += 0x1000U;
    FillRseBackingStore(0xA5U);
    const auto mappingResult = g_alternateMappingFailurePreparation.Prepare(
        invalidImage, g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), ValidCapabilities(), ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    const auto& mappingFailure = g_alternateMappingFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "an alternate mapping rejection forwards every nested failure field",
                   !mappingResult.HasValue(),
                   mappingResult.GetError() == InitialAddressSpacePreparationError::MappingPlanRejected,
                   mappingFailure.Error == InitialAddressSpacePreparationError::MappingPlanRejected,
                   mappingFailure.RseBackingStoreError == InitialRseBackingStorePreparationError::None,
                   mappingFailure.MappingFailure.Error == BootstrapMappingPlanError::InvalidImageLayout,
                   mappingFailure.MappingFailure.MappingError == InitialMappingPlanError::None,
                   mappingFailure.MappingFailure.MappingIndex == 0U,
                   IsEmptyTranslationFailure(mappingFailure.TranslationFailure), IsRseBackingStoreFilled(0xA5U));

    PalVirtualMemoryCapabilities narrowCapabilities = ValidCapabilities();
    narrowCapabilities.Translation.PhysicalAddressBits = 32U;
    FillRseBackingStore(0xA5U);
    const auto encodingResult = g_encodingFailurePreparation.Prepare(
        ValidImage(HighImageBase), g_highBootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), narrowCapabilities,
        ValidFirmware(), ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    const auto& encodingFailure = g_encodingFailurePreparation.GetFailure();
    ASTERIA_EXPECT(
        runner, "translation encoding failure preserves its typed cause and location", !encodingResult.HasValue(),
        encodingResult.GetError() == InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
        encodingFailure.Error == InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
        encodingFailure.RseBackingStoreError == InitialRseBackingStorePreparationError::None,
        IsEmptyMappingFailure(encodingFailure.MappingFailure),
        encodingFailure.TranslationFailure.Error == InitialTranslationResourcePlanError::TranslationEncodingFailed,
        encodingFailure.TranslationFailure.EncodingError == InitialTranslationEncodingError::PhysicalAddressOutOfRange,
        encodingFailure.TranslationFailure.MappingIndex == 5U,
        encodingFailure.TranslationFailure.MappingPageIndex == 0U, IsRseBackingStoreFilled(0xA5U));

    PalVirtualMemoryCapabilities limitedRegisters = ValidCapabilities();
    limitedRegisters.DataTranslationRegisterCount = 11U;
    FillRseBackingStore(0xA5U);
    const auto capacityResult = g_capacityFailurePreparation.Prepare(
        ValidImage(), g_bootInfo, ValidReservedPages(), ValidRuntimeMemoryPages(), limitedRegisters, ValidFirmware(),
        ValidRegions(), ValidInterruptVectorTableView(), ValidRseBackingStoreView());
    const auto& capacityFailure = g_capacityFailurePreparation.GetFailure();
    ASTERIA_EXPECT(runner, "register exhaustion preserves the exact mapping page that failed",
                   !capacityResult.HasValue(),
                   capacityResult.GetError() == InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
                   capacityFailure.Error == InitialAddressSpacePreparationError::TranslationResourcePlanRejected,
                   capacityFailure.RseBackingStoreError == InitialRseBackingStorePreparationError::None,
                   IsEmptyMappingFailure(capacityFailure.MappingFailure),
                   capacityFailure.TranslationFailure.Error ==
                       InitialTranslationResourcePlanError::DataTranslationRegisterCapacityExceeded,
                   capacityFailure.TranslationFailure.EncodingError == InitialTranslationEncodingError::None,
                   capacityFailure.TranslationFailure.MappingIndex == 6U,
                   capacityFailure.TranslationFailure.MappingPageIndex == 1U, IsRseBackingStoreFilled(0xA5U));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialAddressSpacePreparationTests"};
    Asteria::Test::TestConsolidatedPreparation(runner);
    Asteria::Test::TestSuccessfulAtomicPublication(runner);
    Asteria::Test::TestMappingFailureIsTerminal(runner);
    Asteria::Test::TestResourceFailureIsTerminal(runner);
    Asteria::Test::TestFailurePriority(runner);
    Asteria::Test::TestRseFailureIsTerminal(runner);
    Asteria::Test::TestInterruptVectorTableFailureIsTerminal(runner);
    Asteria::Test::TestParentStorageIsolation(runner);
    Asteria::Test::TestPreflightInputAliasMatrix(runner);
    Asteria::Test::TestDetailedFailureForwarding(runner);
    return runner.Finish();
}
