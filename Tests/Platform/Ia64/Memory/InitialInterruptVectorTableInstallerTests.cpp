// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Memory/InitialInterruptVectorTableInstaller.hpp"

#include "Support/Ia64RuntimeMemoryReservation.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_installCallCount{0U};
Foundation::UInt64 g_installPhysicalStart{0U};
Foundation::UInt64 g_installStatus{0U};
Foundation::Size g_applicationProcessorInstallCallCount{0U};
Foundation::UInt64 g_applicationProcessorInstallPhysicalStart{0U};
Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus g_applicationProcessorInstallStatus{
    Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus::Installed};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallInitialInterruptVectorTable(const Asteria::Foundation::UInt64 physicalStart) noexcept
{
    ++Asteria::Test::g_installCallCount;
    Asteria::Test::g_installPhysicalStart = physicalStart;
    return Asteria::Test::g_installStatus;
}

extern "C" Asteria::Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus
AsteriaIa64InstallApplicationProcessorInitialInterruptVectorTable(
    const Asteria::Foundation::UInt64 physicalStart) noexcept
{
    ++Asteria::Test::g_applicationProcessorInstallCallCount;
    Asteria::Test::g_applicationProcessorInstallPhysicalStart = physicalStart;
    return Asteria::Test::g_applicationProcessorInstallStatus;
}

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::PhysicalPageRange;
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageSection;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialInterruptVectorTableApInstallStatus;
using Platform::Ia64::Memory::InitialInterruptVectorTableInstallError;
using Platform::Ia64::Memory::InitialInterruptVectorTableInstaller;
using Platform::Ia64::Memory::InitialInterruptVectorTablePreparationError;
using Platform::Ia64::Memory::InitialInterruptVectorTableInstallSummary;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;

constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constinit InitialAddressSpacePreparation g_preparation;
constinit InitialAddressSpacePreparation g_freshPreparation;
alignas(32U * 1024U) Foundation::UInt8 g_interruptVectorTable[InitialInterruptVectorTableSizeBytes];
alignas(4096) Foundation::UInt8 g_rseBackingStore[InitialRseBackingStoreSizeBytes];
Foundation::UInt8 g_tableSnapshot[InitialInterruptVectorTableSizeBytes];
Foundation::UInt8 g_preparationSnapshot[sizeof(InitialAddressSpacePreparation)];
Foundation::UInt8 g_freshPreparationSnapshot[sizeof(InitialAddressSpacePreparation)];

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

[[nodiscard]] constexpr InitialInterruptVectorTableInstallSummary ValidInstallSummary() noexcept
{
    return InitialInterruptVectorTableInstallSummary{
        .PhysicalStart = ReservedBase + 0x1'0000U,
        .SizeBytes = static_cast<Foundation::UInt32>(InitialInterruptVectorTableSizeBytes),
        .BundleCount = 2048U,
        .Reserved = 0U,
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

[[nodiscard]] bool TableViewsEqual(const WritableInterruptVectorTableView& left,
                                   const WritableInterruptVectorTableView& right) noexcept
{
    return (left.Data == right.Data) && (left.SizeBytes == right.SizeBytes) &&
           (left.PhysicalStart == right.PhysicalStart);
}

[[nodiscard]] bool PrepareFixture() noexcept
{
    const BootInfo bootInfo = ValidBootInfo();
    const auto result = g_preparation.Prepare(
        ValidImage(), bootInfo, ValidReservedPages(), TestSupport::Ia64FixtureRuntimeMemoryPages(ReservedBase),
        ValidCapabilities(), ValidFirmware(), ValidRegions(), ValidTableView(), ValidRseView());
    return result.HasValue();
}

void SnapshotState() noexcept
{
    const auto* const preparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_preparation);
    for (Foundation::Size index{0U}; index < sizeof(g_preparationSnapshot); ++index)
    {
        g_preparationSnapshot[index] = preparationBytes[index];
    }
    for (Foundation::Size index{0U}; index < sizeof(g_tableSnapshot); ++index)
    {
        g_tableSnapshot[index] = g_interruptVectorTable[index];
    }
    const auto* const freshPreparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_freshPreparation);
    for (Foundation::Size index{0U}; index < sizeof(g_freshPreparationSnapshot); ++index)
    {
        g_freshPreparationSnapshot[index] = freshPreparationBytes[index];
    }
}

[[nodiscard]] bool StateMatchesSnapshot() noexcept
{
    const auto* const preparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_preparation);
    for (Foundation::Size index{0U}; index < sizeof(g_preparationSnapshot); ++index)
    {
        if (g_preparationSnapshot[index] != preparationBytes[index])
        {
            return false;
        }
    }
    for (Foundation::Size index{0U}; index < sizeof(g_tableSnapshot); ++index)
    {
        if (g_tableSnapshot[index] != g_interruptVectorTable[index])
        {
            return false;
        }
    }
    const auto* const freshPreparationBytes = reinterpret_cast<const Foundation::UInt8*>(&g_freshPreparation);
    for (Foundation::Size index{0U}; index < sizeof(g_freshPreparationSnapshot); ++index)
    {
        if (g_freshPreparationSnapshot[index] != freshPreparationBytes[index])
        {
            return false;
        }
    }
    return true;
}

void ResetAdapter(const Foundation::UInt64 status = 0U) noexcept
{
    g_installCallCount = 0U;
    g_installPhysicalStart = 0U;
    g_installStatus = status;
}

void ResetApplicationProcessorAdapter(const InitialInterruptVectorTableApInstallStatus status =
                                          InitialInterruptVectorTableApInstallStatus::Installed) noexcept
{
    g_applicationProcessorInstallCallCount = 0U;
    g_applicationProcessorInstallPhysicalStart = 0U;
    g_applicationProcessorInstallStatus = status;
}

void CheckInvalidApplicationProcessorSummary(TestContext& runner,
                                             const InitialInterruptVectorTableInstallSummary summary,
                                             const char* const description) noexcept
{
    ResetAdapter();
    ResetApplicationProcessorAdapter();
    const InitialInterruptVectorTableApInstallStatus status =
        InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(summary);
    ASTERIA_EXPECT(runner, description, status == InitialInterruptVectorTableApInstallStatus::InvalidSummary,
                   g_installCallCount == 0U, g_applicationProcessorInstallCallCount == 0U,
                   g_applicationProcessorInstallPhysicalStart == 0U, StateMatchesSnapshot());
}

void CheckValidApplicationProcessorSummary(TestContext& runner, const InitialInterruptVectorTableInstallSummary summary,
                                           const char* const description) noexcept
{
    ResetAdapter();
    ResetApplicationProcessorAdapter();
    const InitialInterruptVectorTableApInstallStatus status =
        InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(summary);
    ASTERIA_EXPECT(runner, description, status == InitialInterruptVectorTableApInstallStatus::Installed,
                   g_installCallCount == 0U, g_applicationProcessorInstallCallCount == 1U,
                   g_applicationProcessorInstallPhysicalStart == summary.PhysicalStart, StateMatchesSnapshot());
}

void TestValidationBeforeCommit(TestContext& runner) noexcept
{
    ResetAdapter();
    SnapshotState();
    WritableInterruptVectorTableView table = ValidTableView();
    const WritableInterruptVectorTableView tableSnapshot = table;
    auto result = InitialInterruptVectorTableInstaller::Validate(g_freshPreparation, table);
    ASTERIA_EXPECT(runner, "an unpublished owner cannot reach the privileged adapter", !result.HasValue(),
                   result.GetError().Error == InitialInterruptVectorTableInstallError::PreparationNotReady,
                   result.GetError().PreparationError == InitialInterruptVectorTablePreparationError::None,
                   result.GetError().ByteOffset == ~Foundation::Size{0U}, TableViewsEqual(table, tableSnapshot),
                   StateMatchesSnapshot(), g_installCallCount == 0U);

    WritableInterruptVectorTableView invalidView = ValidTableView();
    --invalidView.SizeBytes;
    WritableInterruptVectorTableView invalidViewSnapshot = invalidView;
    result = InitialInterruptVectorTableInstaller::Install(g_preparation, invalidView);
    ASTERIA_EXPECT(runner, "an invalid prepared view is rejected before privileged state changes", !result.HasValue(),
                   result.GetError().Error == InitialInterruptVectorTableInstallError::PreparedTableRejected,
                   result.GetError().PreparationError ==
                       InitialInterruptVectorTablePreparationError::InvalidStorageView,
                   result.GetError().ByteOffset == ~Foundation::Size{0U},
                   TableViewsEqual(invalidView, invalidViewSnapshot), StateMatchesSnapshot(), g_installCallCount == 0U);

    invalidView = ValidTableView();
    ++invalidView.PhysicalStart;
    invalidViewSnapshot = invalidView;
    result = InitialInterruptVectorTableInstaller::Install(g_preparation, invalidView);
    ASTERIA_EXPECT(
        runner, "a mismatched physical address is rejected before privileged state changes", !result.HasValue(),
        result.GetError().Error == InitialInterruptVectorTableInstallError::PreparedTableRejected,
        result.GetError().PreparationError == InitialInterruptVectorTablePreparationError::StorageAddressMismatch,
        result.GetError().ByteOffset == ~Foundation::Size{0U}, TableViewsEqual(invalidView, invalidViewSnapshot),
        StateMatchesSnapshot(), g_installCallCount == 0U);
}

void TestCompleteContentValidation(TestContext& runner) noexcept
{
    bool exact{true};
    ResetAdapter();
    SnapshotState();
    WritableInterruptVectorTableView table = ValidTableView();
    const WritableInterruptVectorTableView tableSnapshot = table;
    for (Foundation::Size byteOffset{0U}; byteOffset < InitialInterruptVectorTableSizeBytes; ++byteOffset)
    {
        g_interruptVectorTable[byteOffset] ^= 0x80U;
        const auto result = InitialInterruptVectorTableInstaller::Install(g_preparation, table);
        exact = !result.HasValue() &&
                (result.GetError().Error == InitialInterruptVectorTableInstallError::InvalidTableContents) &&
                (result.GetError().PreparationError == InitialInterruptVectorTablePreparationError::None) &&
                (result.GetError().ByteOffset == byteOffset) && TableViewsEqual(table, tableSnapshot) &&
                (g_installCallCount == 0U) && exact;
        g_interruptVectorTable[byteOffset] ^= 0x80U;
        exact = StateMatchesSnapshot() && exact;
    }
    ASTERIA_EXPECT(runner, "every fail-stop table byte is checked before commit", exact, StateMatchesSnapshot());
}

void TestExactInstallBoundary(TestContext& runner) noexcept
{
    SnapshotState();
    ResetApplicationProcessorAdapter();
    ResetAdapter(1U);
    WritableInterruptVectorTableView table = ValidTableView();
    const WritableInterruptVectorTableView tableSnapshot = table;
    auto result = InitialInterruptVectorTableInstaller::Install(g_preparation, table);
    ASTERIA_EXPECT(runner, "incompatible visible PSR state returns without changing prepared state", !result.HasValue(),
                   result.GetError().Error == InitialInterruptVectorTableInstallError::UnsupportedProcessorState,
                   result.GetError().PreparationError == InitialInterruptVectorTablePreparationError::None,
                   result.GetError().ByteOffset == ~Foundation::Size{0U}, g_installCallCount == 1U,
                   g_installPhysicalStart == ReservedBase + 0x1'0000U, g_applicationProcessorInstallCallCount == 0U,
                   TableViewsEqual(table, tableSnapshot), StateMatchesSnapshot());

    ResetAdapter();
    result = InitialInterruptVectorTableInstaller::Install(g_preparation, table);
    ASTERIA_EXPECT(runner, "a valid table crosses the privileged boundary exactly once", result.HasValue(),
                   result.GetValue().PhysicalStart == ReservedBase + 0x1'0000U,
                   result.GetValue().SizeBytes == InitialInterruptVectorTableSizeBytes,
                   result.GetValue().BundleCount == 2048U, result.GetValue().Reserved == 0U, g_installCallCount == 1U,
                   g_installPhysicalStart == ReservedBase + 0x1'0000U, g_applicationProcessorInstallCallCount == 0U,
                   TableViewsEqual(table, tableSnapshot), StateMatchesSnapshot());
}

void TestApplicationProcessorSummaryValidation(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 regionZeroSizeBytes{Foundation::UInt64{1U} << 61U};
    constexpr Foundation::UInt64 tableSizeBytes{InitialInterruptVectorTableSizeBytes};
    SnapshotState();

    InitialInterruptVectorTableInstallSummary summary = ValidInstallSummary();
    summary.PhysicalStart = tableSizeBytes;
    CheckValidApplicationProcessorSummary(runner, summary,
                                          "an AP accepts the lowest nonzero 32-KiB-aligned region-zero start");

    summary = ValidInstallSummary();
    summary.PhysicalStart = regionZeroSizeBytes - tableSizeBytes;
    CheckValidApplicationProcessorSummary(runner, summary, "an AP accepts the highest complete region-zero IVT extent");

    summary = ValidInstallSummary();
    summary.PhysicalStart = 0U;
    CheckInvalidApplicationProcessorSummary(runner, summary,
                                            "an AP rejects a zero physical IVT start before privileged entry");

    summary = ValidInstallSummary();
    summary.PhysicalStart = regionZeroSizeBytes;
    CheckInvalidApplicationProcessorSummary(
        runner, summary, "an AP rejects a non-region-zero physical IVT start before privileged entry");

    summary = ValidInstallSummary();
    summary.PhysicalStart += 4096U;
    CheckInvalidApplicationProcessorSummary(runner, summary,
                                            "an AP rejects a non-32-KiB-aligned IVT start before privileged entry");

    summary = ValidInstallSummary();
    --summary.SizeBytes;
    CheckInvalidApplicationProcessorSummary(runner, summary,
                                            "an AP rejects a nonexact IVT size before privileged entry");

    summary = ValidInstallSummary();
    --summary.BundleCount;
    CheckInvalidApplicationProcessorSummary(runner, summary,
                                            "an AP rejects a nonexact IVT bundle count before privileged entry");

    summary = ValidInstallSummary();
    summary.Reserved = 1U;
    CheckInvalidApplicationProcessorSummary(runner, summary,
                                            "an AP rejects nonzero IVT summary reserved state before privileged entry");
}

void TestApplicationProcessorInstallBoundary(TestContext& runner) noexcept
{
    SnapshotState();
    ResetAdapter();
    ResetApplicationProcessorAdapter();
    const InitialInterruptVectorTableInstallSummary summary = ValidInstallSummary();

    g_interruptVectorTable[17U] ^= 0x80U;
    const InitialInterruptVectorTableApInstallStatus installed =
        InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(summary);
    const bool installedWithoutTableScan =
        (installed == InitialInterruptVectorTableApInstallStatus::Installed) &&
        (g_applicationProcessorInstallCallCount == 1U) &&
        (g_applicationProcessorInstallPhysicalStart == summary.PhysicalStart) && (g_installCallCount == 0U) &&
        (g_interruptVectorTable[17U] == static_cast<Foundation::UInt8>(g_tableSnapshot[17U] ^ 0x80U));
    g_interruptVectorTable[17U] ^= 0x80U;
    ASTERIA_EXPECT(runner, "an AP installs the exact retained address without rescanning shared bytes",
                   installedWithoutTableScan, StateMatchesSnapshot());

    ResetApplicationProcessorAdapter(InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState);
    const InitialInterruptVectorTableApInstallStatus unsupported =
        InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(summary);
    ASTERIA_EXPECT(runner, "an AP reports visible processor-state rejection before commit",
                   unsupported == InitialInterruptVectorTableApInstallStatus::UnsupportedProcessorState,
                   g_applicationProcessorInstallCallCount == 1U,
                   g_applicationProcessorInstallPhysicalStart == summary.PhysicalStart, g_installCallCount == 0U,
                   StateMatchesSnapshot());

    ResetApplicationProcessorAdapter(InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch);
    const InitialInterruptVectorTableApInstallStatus mismatch =
        InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(summary);
    ASTERIA_EXPECT(runner, "an AP reports a committed cr.iva readback mismatch distinctly",
                   mismatch == InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch,
                   g_applicationProcessorInstallCallCount == 1U,
                   g_applicationProcessorInstallPhysicalStart == summary.PhysicalStart, g_installCallCount == 0U,
                   StateMatchesSnapshot());

    ResetApplicationProcessorAdapter(static_cast<InitialInterruptVectorTableApInstallStatus>(4U));
    const InitialInterruptVectorTableApInstallStatus unknown =
        InitialInterruptVectorTableInstaller::InstallForApplicationProcessor(summary);
    ASTERIA_EXPECT(runner, "an impossible AP adapter status maps to committed mismatch safely",
                   unknown == InitialInterruptVectorTableApInstallStatus::CommittedReadbackMismatch,
                   g_applicationProcessorInstallCallCount == 1U,
                   g_applicationProcessorInstallPhysicalStart == summary.PhysicalStart, g_installCallCount == 0U,
                   StateMatchesSnapshot());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"InitialInterruptVectorTableInstallerTests"};
    ASTERIA_EXPECT(runner, "the IVT-install fixture publishes a complete address-space plan",
                   Asteria::Test::PrepareFixture());
    Asteria::Test::TestValidationBeforeCommit(runner);
    Asteria::Test::TestCompleteContentValidation(runner);
    Asteria::Test::TestExactInstallBoundary(runner);
    Asteria::Test::TestApplicationProcessorSummaryValidation(runner);
    Asteria::Test::TestApplicationProcessorInstallBoundary(runner);
    return runner.Finish();
}
