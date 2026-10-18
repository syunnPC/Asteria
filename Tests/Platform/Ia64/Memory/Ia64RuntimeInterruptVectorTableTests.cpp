// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeRegionSet.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_installCallCount{0U};
Foundation::UInt64 g_installStatus{0U};
Foundation::UInt64 g_installedContextAddress{0U};
Foundation::UInt64 g_installedTableAddress{0U};
Foundation::UInt64 g_processorZeroRuntimeContextAddress{0U};
Foundation::Size g_activationCallCount{0U};
Foundation::UInt64 g_activationStatus{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallRuntimeInterruptVectorTable(const Asteria::Foundation::UInt64 contextAddress,
                                              const Asteria::Foundation::UInt64 tableAddress) noexcept
{
    ++Asteria::Test::g_installCallCount;
    Asteria::Test::g_installedContextAddress = contextAddress;
    Asteria::Test::g_installedTableAddress = tableAddress;
    return Asteria::Test::g_installStatus;
}

extern "C" [[noreturn]] void AsteriaIa64FailStopRuntimeInterruptVectorTableInstall() noexcept
{
    __builtin_trap();
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64ActivateRuntimeRegions(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                  Asteria::Foundation::UInt64, Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                  Asteria::Foundation::UInt64) noexcept
{
    ++Asteria::Test::g_activationCallCount;
    return Asteria::Test::g_activationStatus;
}

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::VirtualAddress;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingError;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindings;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingsWireStorage;
using Platform::Ia64::Memory::Ia64BootstrapPreferredPageShift;
using Platform::Ia64::Memory::Ia64BootstrapRegionRegisterWords;
using Platform::Ia64::Memory::Ia64RuntimeAlternateDataTlbOffset;
using Platform::Ia64::Memory::Ia64RuntimeAlternateInstructionTlbOffset;
using Platform::Ia64::Memory::Ia64RuntimeBreakOffset;
using Platform::Ia64::Memory::Ia64RuntimeDataAccessRightsOffset;
using Platform::Ia64::Memory::Ia64RuntimeExternalInterruptOffset;
using Platform::Ia64::Memory::Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes;
using Platform::Ia64::Memory::Ia64RuntimeInstructionAccessRightsOffset;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorSlotSizeBytes;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTable;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableImage;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableInstallError;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTablePublicationError;
using Platform::Ia64::Memory::Ia64RuntimeExternalInterruptPublicationError;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::Ia64RuntimeKeyPermissionOffset;
using Platform::Ia64::Memory::Ia64RuntimeObjectRange;
using Platform::Ia64::Memory::Ia64RuntimePageNotPresentOffset;
using Platform::Ia64::Memory::Ia64RuntimePageFaultPublicationError;
using Platform::Ia64::Memory::Ia64RuntimeRefillError;
using Platform::Ia64::Memory::Ia64RuntimeRefillKind;
using Platform::Ia64::Memory::Ia64RuntimeRefillOutcome;
using Platform::Ia64::Memory::Ia64RuntimeRefillProcessorState;
using Platform::Ia64::Memory::Ia64RuntimeRegionCount;
using Platform::Ia64::Memory::Ia64RuntimeRegionSet;
using Platform::Ia64::Memory::Ia64RuntimeRegionSetError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationEncodingError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationInsertion;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedger;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerFailure;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerWireStorage;
using Platform::Ia64::Memory::Ia64SharedBootstrapRegionIdentifier;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::TranslationCapabilities;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;
using Platform::Ia64::System::Ia64SystemCallContextValidationError;
using Platform::Ia64::System::Ia64SystemCallEntryContext;
using Platform::Ia64::System::Ia64SystemCallEntryFrame;
using Platform::Ia64::System::Ia64SystemCallMemoryStackSizeBytes;
using Platform::Ia64::System::Ia64SystemCallRseBackingStoreSizeBytes;
using Platform::Ia64::System::ValidateIa64SystemCallTrustedContext;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext;
using Platform::Ia64::Interrupt::Ia64UserFaultEntryContext;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence;
using Platform::Ia64::Interrupt::ConfigureIa64PageFaultEntryControl;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorStackSizeBytes;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryContext;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControl;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlConfiguration;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlError;
using Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Platform::Synchronization::AtomicUInt64Reference;

constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 RegionFiveBase{0xA000'0000'0010'0000ULL};
constexpr Foundation::UInt64 PhysicalBase{0x0000'0000'0020'0000ULL};
constexpr Foundation::UInt64 TranslationMask{(Foundation::UInt64{1U} << 36U) | (Foundation::UInt64{1U} << 27U) |
                                             (Foundation::UInt64{1U} << 17U)};
constexpr Foundation::UInt64 InterruptedRequired{TranslationMask | (Foundation::UInt64{1U} << 44U) |
                                                 (Foundation::UInt64{1U} << 13U)};
constexpr Foundation::UInt64 FailStopLow{0x0000'0001'0000'0011ULL};
constexpr Foundation::UInt64 FailStopHigh{0x4000'0000'0000'0200ULL};

struct alignas(PageBytes) RuntimeArena
{
    Ia64RuntimeInterruptVectorTable Owners[4U];
    Ia64ActiveRuntimeTranslationBindings Bindings[4U];
    Ia64RuntimeRegionSet RegionSets[4U];
    Ia64RuntimeTranslationLedger Ledgers[4U];
    Ia64ExternalInterruptEntryContext ExternalInterruptContexts[4U];
    Ia64LocalTimerInterruptEvidence ExternalInterruptEvidence[4U];
    alignas(64U) Ia64PageFaultEntryContext PageFaultContexts[4U];
    Ia64PageFaultEntryControl PageFaultControls[4U];
    Ia64PageFaultEntryControl UnconfiguredPageFaultControl;
    Ia64PageFaultEntryControl PageFaultOverlapControl;
    Ia64PageFaultFrame PageFaultOverlapFrame;
    Ia64PageFaultFrame PageFaultFrames[4U];
    alignas(PageBytes) Foundation::UInt8 PageFaultCoordinatorStacks[4U][Ia64PageFaultCoordinatorStackSizeBytes];
    alignas(PageBytes)
        Foundation::UInt8 PageFaultCoordinatorRseBackingStores[4U][Ia64PageFaultCoordinatorRseBackingStoreSizeBytes];
    alignas(PageBytes) Foundation::UInt8 PageFaultOverlapCoordinatorStack[Ia64PageFaultCoordinatorStackSizeBytes];
    alignas(PageBytes)
        Foundation::UInt8 PageFaultOverlapCoordinatorRseBackingStore[Ia64PageFaultCoordinatorRseBackingStoreSizeBytes];
    Ia64SystemCallEntryContext SystemCallContexts[4U];
    Ia64SystemCallEntryFrame SystemCallFrames[4U];
    Ia64SystemCallEntryContext SecondThreadSystemCallContext;
    Ia64UserFaultEntryContext UserFaultContexts[2U];
    alignas(PageBytes) Foundation::UInt8 SystemCallMemoryStacks[4U][Ia64SystemCallMemoryStackSizeBytes];
    alignas(PageBytes) Foundation::UInt8 SystemCallRseBackingStores[4U][Ia64SystemCallRseBackingStoreSizeBytes];
    alignas(PageBytes) Ia64PageFaultEntryContext PageFaultOverlapContext;
    Foundation::UInt8 PageFaultOverlapContextRseTail[(16U * 1024U) - sizeof(Ia64PageFaultEntryContext)];
};

// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<RuntimeArena> g_arenaStorage{};
RuntimeArena& g_arena = g_arenaStorage.Construct();
// CPU0's installed ar.k3 root in Owners[0], for the selection test.
constinit Foundation::UInt64 g_installedContextAddressCpu0{0U};
constinit InitialAddressSpacePreparation g_preparation;
constinit InitialAddressSpacePreparation g_unprepared;
alignas(32U * 1024U) constinit Foundation::UInt8 g_initialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageBytes) constinit Foundation::UInt8 g_initialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
alignas(16U) constinit Ia64SystemCallEntryFrame g_outsideSystemCallFrame{};
alignas(PageBytes) constinit Foundation::UInt8 g_outsideSystemCallMemoryStack[Ia64SystemCallMemoryStackSizeBytes]{};
alignas(PageBytes) constinit Foundation::UInt8
    g_outsideSystemCallRseBackingStore[Ia64SystemCallRseBackingStoreSizeBytes]{};
constinit Ia64LocalTimerInterruptEvidence g_outsideExternalInterruptEvidence{};
alignas(64U) constinit Ia64PageFaultFrame g_outsidePageFaultFrame{};
alignas(PageBytes) constinit Foundation::UInt8
    g_outsidePageFaultCoordinatorStack[Ia64PageFaultCoordinatorStackSizeBytes]{};

static_assert((sizeof(RuntimeArena) & (PageBytes - 1U)) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(RuntimeArena)) == 0x10'0000U);
static_assert(sizeof(Ia64RuntimeInterruptVectorTableContext) == 64U);
static_assert(alignof(Ia64RuntimeInterruptVectorTableContext) == 16U);
static_assert(offsetof(Ia64RuntimeInterruptVectorTableContext, BindingsWireAddress) == 0U);
static_assert(offsetof(Ia64RuntimeInterruptVectorTableContext, ProcessorIndex) == 8U);
static_assert(offsetof(Ia64RuntimeInterruptVectorTableContext, SystemCallEntryContextAddress) == 16U);
static_assert(offsetof(Ia64RuntimeInterruptVectorTableContext, ExternalInterruptEntryContextAddress) == 24U);
static_assert(offsetof(Ia64RuntimeInterruptVectorTableContext, PageFaultEntryContextAddress) == 32U);
static_assert(offsetof(Ia64RuntimeInterruptVectorTableContext, UserFaultEntryContextAddress) == 40U);
static_assert(sizeof(Ia64ExternalInterruptEntryContext) == 32U);
static_assert(sizeof(Ia64LocalTimerInterruptEvidence) == 64U);
static_assert(offsetof(Ia64LocalTimerInterruptEvidence, AcknowledgedSequence) == 56U);
static_assert(sizeof(Ia64SystemCallEntryContext) == 64U);
static_assert(sizeof(Ia64PageFaultEntryContext) == 64U);
static_assert(alignof(Ia64PageFaultEntryContext) == 16U);
static_assert(sizeof(Ia64PageFaultEntryControl) == 192U);
static_assert(alignof(Ia64PageFaultEntryControl) == 64U);
static_assert(sizeof(Ia64PageFaultFrame) == 2688U);
static_assert(alignof(Ia64PageFaultFrame) == 64U);
static_assert((offsetof(RuntimeArena, PageFaultCoordinatorStacks) & (PageBytes - 1U)) == 0U);
static_assert((offsetof(RuntimeArena, PageFaultCoordinatorRseBackingStores) & (PageBytes - 1U)) == 0U);
static_assert((offsetof(RuntimeArena, PageFaultOverlapContext) & (PageBytes - 1U)) == 0U);
static_assert(offsetof(RuntimeArena, PageFaultOverlapContextRseTail) ==
              offsetof(RuntimeArena, PageFaultOverlapContext) + sizeof(Ia64PageFaultEntryContext));
static_assert(alignof(Ia64SystemCallEntryContext) == 16U);
static_assert(offsetof(Ia64SystemCallEntryContext, ProcessorIndex) == 48U);
static_assert(offsetof(Ia64SystemCallEntryContext, Reserved) == 56U);
static_assert(sizeof(Ia64SystemCallEntryFrame) == 416U);
static_assert(alignof(Ia64SystemCallEntryFrame) == 16U);
static_assert((offsetof(RuntimeArena, SystemCallMemoryStacks) & (PageBytes - 1U)) == 0U);
static_assert((offsetof(RuntimeArena, SystemCallRseBackingStores) & (PageBytes - 1U)) == 0U);
static_assert(sizeof(Ia64ActiveRuntimeTranslationBindingsWireStorage) == 56U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, Slots) == 0U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, RuntimeObjectStart) == 32U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, RuntimeObjectEnd) == 40U);
static_assert(offsetof(Ia64ActiveRuntimeTranslationBindingsWireStorage, AdmissionOwnerAddress) == 48U);

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities Capabilities() noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = 24U,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = 0x71U,
                .HardwareWalkerImplemented = true,
                .Reserved16 = 0U,
                .Reserved64 = 0U,
            },
        .PurgePageSizeMask = ArchitectedMinimumPurgePageSizeMask,
        .InstructionTranslationRegisterCount = 32U,
        .DataTranslationRegisterCount = 32U,
        .ProtectionKeyRegisterCount = 16U,
        .MaximumConcurrentPurgesEncoding = 1U,
        .HashTagIdentifier = 0U,
        .UniqueTranslationCacheCount = 1U,
        .TranslationCacheLevelCount = 1U,
        .Reserved8 = 0U,
        .Reserved32 = 0U,
    };
}

[[nodiscard]] constexpr SalSystemTableInformation Firmware() noexcept
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

[[nodiscard]] constexpr InitialTranslationRegionSet Regions() noexcept
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

[[nodiscard]] bool PrepareFixture() noexcept
{
    const PhysicalPageRange reserved{
        .PhysicalStart = ReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    if (!TestSupport::MakeIa64HostedBootGeometry(
            AddressOf(&Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage), AddressOf(&g_arena),
            sizeof(g_arena), reserved, geometry))
    {
        return false;
    }
    BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 3U,
        .FirmwareDescriptorVersion = 1U,
        .FirmwareDescriptorSize = 40U,
        .Reserved = 0U,
        .FirmwareMemoryMapSizeBytes = 0U,
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
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[1U] = geometry.ImageRegion;
    bootInfo.MemoryRegions[2U] = geometry.RuntimeMemoryRegion;
    if (!TestSupport::SortIa64HostedBootRegions(bootInfo) ||
        !g_preparation
             .Prepare(geometry.Image, bootInfo, reserved, geometry.RuntimeMemoryPages, Capabilities(), Firmware(),
                      Regions(),
                      WritableInterruptVectorTableView{
                          .Data = g_initialInterruptVectorTable,
                          .SizeBytes = InitialInterruptVectorTableSizeBytes,
                          .PhysicalStart = ReservedBase + 0x1'0000U,
                      },
                      WritableRseBackingStoreView{
                          .Data = g_initialRseBackingStore,
                          .SizeBytes = InitialRseBackingStoreSizeBytes,
                          .PhysicalStart = ReservedBase + 0x1'8000U,
                      })
             .HasValue())
    {
        return false;
    }
    // The hosted geometry's plan is the same at every host layout.
    const auto summary = g_preparation.TryGetSummary();
    return summary.HasValue() &&
           (summary.GetValue()->Translation.DataRegisterCount == TestSupport::Ia64HostedDataTranslationRegisters) &&
           (summary.GetValue()->Translation.InstructionRegisterCount ==
            TestSupport::Ia64HostedInstructionTranslationRegisters);
}

[[nodiscard]] constexpr TranslationCapabilities LedgerCapabilities() noexcept
{
    return TranslationCapabilities{
        .InsertablePageSizeMask = Foundation::UInt64{1U} << 12U,
        .PhysicalAddressBits = 50U,
        .ImplementedVirtualAddressMostSignificantBit = 60U,
        .RegionIdentifierBits = 24U,
        .ProtectionKeyBits = 24U,
        .MemoryAttributeMask = 0x71U,
        .HardwareWalkerImplemented = true,
        .Reserved16 = 0U,
        .Reserved64 = 0U,
    };
}

[[nodiscard]] constexpr AddressSpaceMappingRecord Mapping() noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = {},
        .DeviceRegion = {},
        .Frame = {},
        .VirtualStart = VirtualAddress{RegionFiveBase},
        .PhysicalStart = PhysicalAddress{PhysicalBase},
        .Pages = PageCount{2U},
        .MemoryObjectOffsetPages = 0U,
        .Permissions = PagePermissions{PagePermission::Read},
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Kind = FrameKind::Ram,
        .Commitment = {},
        .RuntimeMapping = true,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr bool Succeeded(const Ia64RuntimeTranslationLedgerFailure failure) noexcept
{
    return (failure.Code == Ia64RuntimeTranslationLedgerError::None) &&
           (failure.EncodingError == Ia64RuntimeTranslationEncodingError::None);
}

[[nodiscard]] constexpr Foundation::UInt64 RegionRegisterWord(const Foundation::UInt32 regionIdentifier) noexcept
{
    return (static_cast<Foundation::UInt64>(regionIdentifier) << 8U) |
           (static_cast<Foundation::UInt64>(Ia64BootstrapPreferredPageShift) << 2U);
}

[[nodiscard]] bool BuildRuntimeObjects() noexcept
{
    Ia64BootstrapRegionRegisterWords words{};
    for (Foundation::Size index{0U}; index < Ia64RuntimeRegionCount; ++index)
    {
        words.RegionRegisters[index] =
            RegionRegisterWord(Ia64SharedBootstrapRegionIdentifier + static_cast<Foundation::UInt32>(index));
    }
    TranslationCapabilities regionCapabilities{LedgerCapabilities()};
    regionCapabilities.PhysicalAddressBits = 0U;
    regionCapabilities.ImplementedVirtualAddressMostSignificantBit = 0U;
    regionCapabilities.ProtectionKeyBits = 0U;
    regionCapabilities.MemoryAttributeMask = 0U;
    regionCapabilities.HardwareWalkerImplemented = false;
    return Ia64RuntimeRegionSet::AdoptBootstrap(words, regionCapabilities, g_arena.RegionSets[0U]) ==
               Ia64RuntimeRegionSetError::None &&
           Succeeded(g_arena.Ledgers[0U].Initialize(LedgerCapabilities())) &&
           Succeeded(g_arena.Ledgers[0U].Append(Mapping()));
}

[[nodiscard]] bool ConfigureSystemCallContexts() noexcept
{
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        const Foundation::UInt64 memoryStackStart = AddressOf(&g_arena.SystemCallMemoryStacks[index][0U]);
        const Foundation::UInt64 rseBackingStoreStart = AddressOf(&g_arena.SystemCallRseBackingStores[index][0U]);
        g_arena.SystemCallContexts[index] = Ia64SystemCallEntryContext{
            .FrameAddress = AddressOf(&g_arena.SystemCallFrames[index]),
            .MemoryStackStart = memoryStackStart,
            .MemoryStackEnd = memoryStackStart + Ia64SystemCallMemoryStackSizeBytes,
            .RseBackingStoreStart = rseBackingStoreStart,
            .RseBackingStoreEnd = rseBackingStoreStart + Ia64SystemCallRseBackingStoreSizeBytes,
            .BridgeContextAddress = AddressOf(&g_arena.Owners[index]),
            .ProcessorIndex = index,
            .Reserved = 0U,
        };
        if (ValidateIa64SystemCallTrustedContext(g_arena.SystemCallContexts[index]) !=
            Ia64SystemCallContextValidationError::None)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ConfigurePageFaultContexts() noexcept
{
    constexpr Foundation::UInt64 interruptedRseBase{0xA000'0000'1000'0000ULL};
    for (Foundation::Size index{0U}; index < 4U; ++index)
    {
        const Foundation::UInt64 stackStart = AddressOf(&g_arena.PageFaultCoordinatorStacks[index][0U]);
        const Foundation::UInt64 coordinatorRseStart =
            AddressOf(&g_arena.PageFaultCoordinatorRseBackingStores[index][0U]);
        const Foundation::UInt64 interruptedRseStart = interruptedRseBase + (index * 64U * 1024U);
        const Ia64PageFaultEntryControlError configured = ConfigureIa64PageFaultEntryControl(
            g_arena.PageFaultControls[index],
            Ia64PageFaultEntryControlConfiguration{
                .ProcessorIndex = index,
                .FrameAddress = AddressOf(&g_arena.PageFaultFrames[index]),
                .InterruptedRseBackingStoreStart = interruptedRseStart,
                .InterruptedRseBackingStoreEnd = interruptedRseStart + (64U * 1024U),
                .CoordinatorMemoryStackStart = stackStart,
                .CoordinatorMemoryStackEnd = stackStart + Ia64PageFaultCoordinatorStackSizeBytes,
                .CoordinatorRseBackingStoreStart = coordinatorRseStart,
                .CoordinatorRseBackingStoreEnd = coordinatorRseStart + Ia64PageFaultCoordinatorRseBackingStoreSizeBytes,
                .CoordinatorEntryPoint = 0x10'0000U + (index * 16U),
                .CoordinatorGlobalPointer = 0U,
                .CoordinatorArgument = AddressOf(&g_arena.Owners[index]),
            });
        if (configured != Ia64PageFaultEntryControlError::None)
        {
            return false;
        }
        g_arena.PageFaultContexts[index] = Ia64PageFaultEntryContext{
            .ControlAddress = AddressOf(&g_arena.PageFaultControls[index]),
            .ProcessorIndex = index,
            .PageNotPresentVectorOffset = Ia64RuntimePageNotPresentOffset,
            .KeyPermissionVectorOffset = Ia64RuntimeKeyPermissionOffset,
            .InstructionAccessRightsVectorOffset = Ia64RuntimeInstructionAccessRightsOffset,
            .DataAccessRightsVectorOffset = Ia64RuntimeDataAccessRightsOffset,
            .Reserved = {},
        };
    }
    const Foundation::UInt64 overlapStart = AddressOf(&g_arena.PageFaultOverlapContext);
    const Foundation::UInt64 stackStart = AddressOf(&g_arena.PageFaultOverlapCoordinatorStack[0U]);
    const Foundation::UInt64 coordinatorRseStart = AddressOf(&g_arena.PageFaultOverlapCoordinatorRseBackingStore[0U]);
    if (ConfigureIa64PageFaultEntryControl(
            g_arena.PageFaultOverlapControl,
            Ia64PageFaultEntryControlConfiguration{
                .ProcessorIndex = 0U,
                .FrameAddress = AddressOf(&g_arena.PageFaultOverlapFrame),
                .InterruptedRseBackingStoreStart = overlapStart,
                .InterruptedRseBackingStoreEnd = overlapStart + (16U * 1024U),
                .CoordinatorMemoryStackStart = stackStart,
                .CoordinatorMemoryStackEnd = stackStart + Ia64PageFaultCoordinatorStackSizeBytes,
                .CoordinatorRseBackingStoreStart = coordinatorRseStart,
                .CoordinatorRseBackingStoreEnd = coordinatorRseStart + Ia64PageFaultCoordinatorRseBackingStoreSizeBytes,
                .CoordinatorEntryPoint = 0x10'1000U,
                .CoordinatorGlobalPointer = 0U,
                .CoordinatorArgument = AddressOf(&g_arena.Owners[0U]),
            }) != Ia64PageFaultEntryControlError::None)
    {
        return false;
    }
    g_arena.PageFaultOverlapContext = Ia64PageFaultEntryContext{
        .ControlAddress = AddressOf(&g_arena.PageFaultOverlapControl),
        .ProcessorIndex = 0U,
        .PageNotPresentVectorOffset = Ia64RuntimePageNotPresentOffset,
        .KeyPermissionVectorOffset = Ia64RuntimeKeyPermissionOffset,
        .InstructionAccessRightsVectorOffset = Ia64RuntimeInstructionAccessRightsOffset,
        .DataAccessRightsVectorOffset = Ia64RuntimeDataAccessRightsOffset,
        .Reserved = {},
    };
    return true;
}

void ResetNative(const Foundation::UInt64 status = 0U) noexcept
{
    g_installCallCount = 0U;
    g_installStatus = status;
    g_installedContextAddress = 0U;
    g_installedTableAddress = 0U;
}

[[nodiscard]] bool IsFailStopBundle(const Ia64RuntimeInterruptVectorTableImage& image,
                                    const Foundation::Size offset) noexcept
{
    Foundation::UInt64 low{0U};
    Foundation::UInt64 high{0U};
    for (Foundation::Size byte{0U}; byte < 8U; ++byte)
    {
        low |= static_cast<Foundation::UInt64>(image.Bytes[offset + byte]) << (byte * 8U);
        high |= static_cast<Foundation::UInt64>(image.Bytes[offset + 8U + byte]) << (byte * 8U);
    }
    return (low == FailStopLow) && (high == FailStopHigh);
}

[[nodiscard]] constexpr bool
HasReservedPageFaultWordsZero(const Ia64RuntimeInterruptVectorTableContext& context) noexcept
{
    return (context.PageFaultEntryContextAddress == 0U) && (context.UserFaultEntryContextAddress == 0U);
}

[[nodiscard]] bool HasReservedPageFaultWordsZeroAt(const Foundation::UInt64 contextAddress) noexcept
{
    if (contextAddress == 0U)
    {
        return false;
    }
    const auto* const context = reinterpret_cast<const Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(contextAddress));
    return HasReservedPageFaultWordsZero(*context);
}

void TestTablePolicy(TestContext& runner) noexcept
{
    const auto& image = Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage;
    const Foundation::UInt64 address = AddressOf(&image);
    ASTERIA_EXPECT(runner, "the immutable table has exact geometry and unreversed vectors",
                   sizeof(image) == 32U * 1024U, (address & ((32U * 1024U) - 1U)) == 0U,
                   Ia64RuntimeAlternateInstructionTlbOffset == 0x0C00U, Ia64RuntimeAlternateDataTlbOffset == 0x1000U,
                   Ia64RuntimeBreakOffset == 0x2C00U, Ia64RuntimeExternalInterruptOffset == 0x3000U,
                   Ia64RuntimeInterruptVectorSlotSizeBytes == 0x400U,
                   Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes == 0x100U,
                   Ia64RuntimePageNotPresentOffset == 0x5000U, Ia64RuntimeKeyPermissionOffset == 0x5100U,
                   Ia64RuntimeInstructionAccessRightsOffset == 0x5200U, Ia64RuntimeDataAccessRightsOffset == 0x5300U);

    bool allNonMarkerBundlesAreFailStop{true};
    bool instructionPaddingIsFailStop{true};
    bool dataPaddingIsFailStop{true};
    bool breakPaddingIsFailStop{true};
    bool externalInterruptPaddingIsFailStop{true};
    bool pageNotPresentPaddingIsFailStop{true};
    bool keyPermissionPaddingIsFailStop{true};
    bool instructionAccessRightsPaddingIsFailStop{true};
    bool dataAccessRightsPaddingIsFailStop{true};
    for (Foundation::Size offset{0U}; offset < Ia64RuntimeInterruptVectorTableSizeBytes; offset += 16U)
    {
        const bool instructionMarker = offset == Ia64RuntimeAlternateInstructionTlbOffset;
        const bool dataMarker = offset == Ia64RuntimeAlternateDataTlbOffset;
        const bool breakMarker = offset == Ia64RuntimeBreakOffset;
        const bool externalInterruptMarker = offset == Ia64RuntimeExternalInterruptOffset;
        const bool pageNotPresentMarker = offset == Ia64RuntimePageNotPresentOffset;
        const bool keyPermissionMarker = offset == Ia64RuntimeKeyPermissionOffset;
        const bool instructionAccessRightsMarker = offset == Ia64RuntimeInstructionAccessRightsOffset;
        const bool dataAccessRightsMarker = offset == Ia64RuntimeDataAccessRightsOffset;
        const bool failStop = IsFailStopBundle(image, offset);
        allNonMarkerBundlesAreFailStop =
            allNonMarkerBundlesAreFailStop &&
            (instructionMarker || dataMarker || breakMarker || externalInterruptMarker || pageNotPresentMarker ||
             keyPermissionMarker || instructionAccessRightsMarker || dataAccessRightsMarker || failStop);

        const bool instructionPadding =
            (offset > Ia64RuntimeAlternateInstructionTlbOffset) &&
            (offset < (Ia64RuntimeAlternateInstructionTlbOffset + Ia64RuntimeInterruptVectorSlotSizeBytes));
        const bool dataPadding =
            (offset > Ia64RuntimeAlternateDataTlbOffset) &&
            (offset < (Ia64RuntimeAlternateDataTlbOffset + Ia64RuntimeInterruptVectorSlotSizeBytes));
        const bool breakPadding = (offset > Ia64RuntimeBreakOffset) &&
                                  (offset < (Ia64RuntimeBreakOffset + Ia64RuntimeInterruptVectorSlotSizeBytes));
        const bool externalInterruptPadding =
            (offset > Ia64RuntimeExternalInterruptOffset) &&
            (offset < (Ia64RuntimeExternalInterruptOffset + Ia64RuntimeInterruptVectorSlotSizeBytes));
        instructionPaddingIsFailStop = instructionPaddingIsFailStop && (!instructionPadding || failStop);
        dataPaddingIsFailStop = dataPaddingIsFailStop && (!dataPadding || failStop);
        breakPaddingIsFailStop = breakPaddingIsFailStop && (!breakPadding || failStop);
        externalInterruptPaddingIsFailStop =
            externalInterruptPaddingIsFailStop && (!externalInterruptPadding || failStop);
        const bool pageNotPresentPadding =
            (offset > Ia64RuntimePageNotPresentOffset) &&
            (offset < (Ia64RuntimePageNotPresentOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes));
        const bool keyPermissionPadding =
            (offset > Ia64RuntimeKeyPermissionOffset) &&
            (offset < (Ia64RuntimeKeyPermissionOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes));
        const bool instructionAccessRightsPadding =
            (offset > Ia64RuntimeInstructionAccessRightsOffset) &&
            (offset < (Ia64RuntimeInstructionAccessRightsOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes));
        const bool dataAccessRightsPadding =
            (offset > Ia64RuntimeDataAccessRightsOffset) &&
            (offset < (Ia64RuntimeDataAccessRightsOffset + Ia64RuntimeHeavyweightInterruptVectorSlotSizeBytes));
        pageNotPresentPaddingIsFailStop = pageNotPresentPaddingIsFailStop && (!pageNotPresentPadding || failStop);
        keyPermissionPaddingIsFailStop = keyPermissionPaddingIsFailStop && (!keyPermissionPadding || failStop);
        instructionAccessRightsPaddingIsFailStop =
            instructionAccessRightsPaddingIsFailStop && (!instructionAccessRightsPadding || failStop);
        dataAccessRightsPaddingIsFailStop = dataAccessRightsPaddingIsFailStop && (!dataAccessRightsPadding || failStop);
    }

    bool instructionDiffersFromData{false};
    bool breakDiffersFromInstruction{false};
    bool breakDiffersFromData{false};
    bool externalInterruptDiffersFromInstruction{false};
    bool externalInterruptDiffersFromData{false};
    bool externalInterruptDiffersFromBreak{false};
    bool pageNotPresentDiffersFromKeyPermission{false};
    bool pageNotPresentDiffersFromInstructionAccessRights{false};
    bool pageNotPresentDiffersFromDataAccessRights{false};
    bool keyPermissionDiffersFromInstructionAccessRights{false};
    bool keyPermissionDiffersFromDataAccessRights{false};
    bool instructionAccessRightsDiffersFromDataAccessRights{false};
    for (Foundation::Size byte{0U}; byte < 16U; ++byte)
    {
        instructionDiffersFromData =
            instructionDiffersFromData || (image.Bytes[Ia64RuntimeAlternateInstructionTlbOffset + byte] !=
                                           image.Bytes[Ia64RuntimeAlternateDataTlbOffset + byte]);
        breakDiffersFromInstruction =
            breakDiffersFromInstruction || (image.Bytes[Ia64RuntimeBreakOffset + byte] !=
                                            image.Bytes[Ia64RuntimeAlternateInstructionTlbOffset + byte]);
        breakDiffersFromData = breakDiffersFromData || (image.Bytes[Ia64RuntimeBreakOffset + byte] !=
                                                        image.Bytes[Ia64RuntimeAlternateDataTlbOffset + byte]);
        externalInterruptDiffersFromInstruction =
            externalInterruptDiffersFromInstruction || (image.Bytes[Ia64RuntimeExternalInterruptOffset + byte] !=
                                                        image.Bytes[Ia64RuntimeAlternateInstructionTlbOffset + byte]);
        externalInterruptDiffersFromData =
            externalInterruptDiffersFromData || (image.Bytes[Ia64RuntimeExternalInterruptOffset + byte] !=
                                                 image.Bytes[Ia64RuntimeAlternateDataTlbOffset + byte]);
        externalInterruptDiffersFromBreak =
            externalInterruptDiffersFromBreak ||
            (image.Bytes[Ia64RuntimeExternalInterruptOffset + byte] != image.Bytes[Ia64RuntimeBreakOffset + byte]);
        pageNotPresentDiffersFromKeyPermission =
            pageNotPresentDiffersFromKeyPermission ||
            (image.Bytes[Ia64RuntimePageNotPresentOffset + byte] != image.Bytes[Ia64RuntimeKeyPermissionOffset + byte]);
        pageNotPresentDiffersFromInstructionAccessRights =
            pageNotPresentDiffersFromInstructionAccessRights ||
            (image.Bytes[Ia64RuntimePageNotPresentOffset + byte] !=
             image.Bytes[Ia64RuntimeInstructionAccessRightsOffset + byte]);
        pageNotPresentDiffersFromDataAccessRights =
            pageNotPresentDiffersFromDataAccessRights || (image.Bytes[Ia64RuntimePageNotPresentOffset + byte] !=
                                                          image.Bytes[Ia64RuntimeDataAccessRightsOffset + byte]);
        keyPermissionDiffersFromInstructionAccessRights =
            keyPermissionDiffersFromInstructionAccessRights ||
            (image.Bytes[Ia64RuntimeKeyPermissionOffset + byte] !=
             image.Bytes[Ia64RuntimeInstructionAccessRightsOffset + byte]);
        keyPermissionDiffersFromDataAccessRights =
            keyPermissionDiffersFromDataAccessRights || (image.Bytes[Ia64RuntimeKeyPermissionOffset + byte] !=
                                                         image.Bytes[Ia64RuntimeDataAccessRightsOffset + byte]);
        instructionAccessRightsDiffersFromDataAccessRights =
            instructionAccessRightsDiffersFromDataAccessRights ||
            (image.Bytes[Ia64RuntimeInstructionAccessRightsOffset + byte] !=
             image.Bytes[Ia64RuntimeDataAccessRightsOffset + byte]);
    }
    ASTERIA_EXPECT(runner,
                   "all hosted entry markers are present and page-fault selectors are "
                   "distinct",
                   !IsFailStopBundle(image, Ia64RuntimeAlternateInstructionTlbOffset),
                   !IsFailStopBundle(image, Ia64RuntimeAlternateDataTlbOffset),
                   !IsFailStopBundle(image, Ia64RuntimeBreakOffset),
                   !IsFailStopBundle(image, Ia64RuntimeExternalInterruptOffset),
                   !IsFailStopBundle(image, Ia64RuntimePageNotPresentOffset),
                   !IsFailStopBundle(image, Ia64RuntimeKeyPermissionOffset),
                   !IsFailStopBundle(image, Ia64RuntimeInstructionAccessRightsOffset),
                   !IsFailStopBundle(image, Ia64RuntimeDataAccessRightsOffset), instructionDiffersFromData,
                   breakDiffersFromInstruction, breakDiffersFromData, externalInterruptDiffersFromInstruction,
                   externalInterruptDiffersFromData, externalInterruptDiffersFromBreak,
                   pageNotPresentDiffersFromKeyPermission, pageNotPresentDiffersFromInstructionAccessRights,
                   pageNotPresentDiffersFromDataAccessRights, keyPermissionDiffersFromInstructionAccessRights,
                   keyPermissionDiffersFromDataAccessRights, instructionAccessRightsDiffersFromDataAccessRights);
    ASTERIA_EXPECT(runner, "both refill tails and both 63-bundle trampoline tails are fail-stop",
                   instructionPaddingIsFailStop, dataPaddingIsFailStop, breakPaddingIsFailStop,
                   externalInterruptPaddingIsFailStop);
    ASTERIA_EXPECT(runner, "every hosted bundle outside the eight structural markers is fail-stop",
                   allNonMarkerBundlesAreFailStop);
    ASTERIA_EXPECT(runner, "all four heavyweight page-fault trampoline tails remain fail-stop",
                   pageNotPresentPaddingIsFailStop, keyPermissionPaddingIsFailStop,
                   instructionAccessRightsPaddingIsFailStop, dataAccessRightsPaddingIsFailStop);
}

void TestInstallValidationAndSuccess(TestContext& runner) noexcept
{
    const Ia64RuntimeObjectRange arenaRange{
        .VirtualStart = AddressOf(&g_arena),
        .SizeBytes = sizeof(RuntimeArena),
    };
    ResetNative();
    const auto invalidCpu =
        g_arena.Owners[0U].ValidateInstall(CpuId{4U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    const auto unprepared =
        g_arena.Owners[0U].ValidateInstall(CpuId{0U}, g_unprepared, g_arena.Bindings[0U], arenaRange);
    const auto misaligned = g_arena.Owners[0U].ValidateInstall(CpuId{0U}, g_preparation, g_arena.Bindings[0U],
                                                               Ia64RuntimeObjectRange{
                                                                   .VirtualStart = arenaRange.VirtualStart + 1U,
                                                                   .SizeBytes = arenaRange.SizeBytes,
                                                               });
    const auto overflow = g_arena.Owners[0U].ValidateInstall(CpuId{0U}, g_preparation, g_arena.Bindings[0U],
                                                             Ia64RuntimeObjectRange{
                                                                 .VirtualStart = ~Foundation::UInt64{0U} - 0xFFFU,
                                                                 .SizeBytes = PageBytes,
                                                             });
    ASTERIA_EXPECT(runner, "invalid CPU, readiness, alignment, and overflow are failure-atomic", !invalidCpu.HasValue(),
                   invalidCpu.GetError() == Ia64RuntimeInterruptVectorTableInstallError::InvalidProcessor,
                   !unprepared.HasValue(),
                   unprepared.GetError() == Ia64RuntimeInterruptVectorTableInstallError::PreparationNotReady,
                   !misaligned.HasValue(),
                   misaligned.GetError() == Ia64RuntimeInterruptVectorTableInstallError::InvalidRuntimeObjectRange,
                   !overflow.HasValue(),
                   overflow.GetError() == Ia64RuntimeInterruptVectorTableInstallError::InvalidRuntimeObjectRange,
                   g_installCallCount == 0U);

    const auto outsideOwner =
        g_arena.Owners[0U].ValidateInstall(CpuId{0U}, g_preparation, g_arena.Bindings[0U],
                                           Ia64RuntimeObjectRange{
                                               .VirtualStart = arenaRange.VirtualStart + PageBytes,
                                               .SizeBytes = arenaRange.SizeBytes - PageBytes,
                                           });
    ASTERIA_EXPECT(
        runner, "a corridor excluding the complete owner is rejected without native work", !outsideOwner.HasValue(),
        outsideOwner.GetError() == Ia64RuntimeInterruptVectorTableInstallError::OwnerOutsideRuntimeObjectRange,
        g_installCallCount == 0U);

    g_arena.Owners[0U].ConfigureStopStorage(arenaRange.VirtualStart + 1U);
    const auto misalignedStop =
        g_arena.Owners[0U].ValidateInstall(CpuId{0U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    g_arena.Owners[0U].ConfigureStopStorage(arenaRange.VirtualStart + arenaRange.SizeBytes - 16U);
    const auto crossingStop =
        g_arena.Owners[0U].ValidateInstall(CpuId{0U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    g_arena.Owners[0U].ConfigureStopStorage(0U);
    ASTERIA_EXPECT(
        runner, "stop storage must be aligned and completely inside the locked corridor", !misalignedStop.HasValue(),
        misalignedStop.GetError() == Ia64RuntimeInterruptVectorTableInstallError::StopStorageOutsideRuntimeObjectRange,
        !crossingStop.HasValue(),
        crossingStop.GetError() == Ia64RuntimeInterruptVectorTableInstallError::StopStorageOutsideRuntimeObjectRange,
        g_installCallCount == 0U);

    ResetNative(1U);
    const auto nativeRejected =
        g_arena.Owners[1U].InstallAndAdmit(CpuId{0U}, g_preparation, g_arena.Bindings[1U], arenaRange);
    Foundation::UInt64 rejectedPublicationValue{~Foundation::UInt64{0U}};
    bool rejectedContextIsZero{false};
    if (g_installedContextAddress != 0U)
    {
        auto* const rejectedContext = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
            static_cast<Foundation::UIntPointer>(g_installedContextAddress));
        AtomicUInt64Reference rejectedPublication{rejectedContext->SystemCallEntryContextAddress};
        rejectedPublicationValue = rejectedPublication.LoadAcquire();
        rejectedContextIsZero = (rejectedContext->BindingsWireAddress == 0U) &&
                                (rejectedContext->ProcessorIndex == 0U) &&
                                (rejectedContext->SystemCallEntryContextAddress == 0U) &&
                                (rejectedContext->ExternalInterruptEntryContextAddress == 0U) &&
                                HasReservedPageFaultWordsZero(*rejectedContext);
    }
    const auto rejectedPublish =
        g_arena.Owners[1U].PublishSystemCallEntryContext(CpuId{0U}, &g_arena.SystemCallContexts[0U]);
    const auto retryWithDifferentRange =
        g_arena.Owners[1U].ValidateInstall(CpuId{0U}, g_preparation, g_arena.Bindings[1U],
                                           Ia64RuntimeObjectRange{
                                               .VirtualStart = arenaRange.VirtualStart,
                                               .SizeBytes = arenaRange.SizeBytes - PageBytes,
                                           });
    const auto* const rejectedWire =
        reinterpret_cast<const Ia64ActiveRuntimeTranslationBindingsWireStorage*>(&g_arena.Bindings[1U]);
    ASTERIA_EXPECT(runner, "native rejection rolls back its context and commits no owner or binding state",
                   !nativeRejected.HasValue(),
                   nativeRejected.GetError() == Ia64RuntimeInterruptVectorTableInstallError::UnsupportedProcessorState,
                   g_installCallCount == 1U,
                   rejectedPublish == Ia64RuntimeInterruptVectorTablePublicationError::ProcessorNotInstalled,
                   retryWithDifferentRange.HasValue(), rejectedWire->Slots[0U].LoadAcquire() == 0U,
                   rejectedWire->RuntimeObjectStart == 0U, rejectedWire->RuntimeObjectEnd == 0U,
                   rejectedWire->AdmissionOwnerAddress == 0U, rejectedPublicationValue == 0U, rejectedContextIsZero);

    ResetNative();
    const auto retryWithDifferentBindings =
        g_arena.Owners[1U].InstallAndAdmit(CpuId{0U}, g_preparation, g_arena.Bindings[2U], arenaRange);
    const auto* const retryWire =
        reinterpret_cast<const Ia64ActiveRuntimeTranslationBindingsWireStorage*>(&g_arena.Bindings[2U]);
    const auto* const retryContext = reinterpret_cast<const Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_installedContextAddress));
    ASTERIA_EXPECT(runner, "a rejected install can retry the same context with a different binding identity",
                   retryWithDifferentBindings.HasValue(), g_installCallCount == 1U,
                   g_installedContextAddress == retryWithDifferentBindings.GetValue().ContextAddress,
                   retryContext->BindingsWireAddress == AddressOf(retryWire), retryContext->ProcessorIndex == 0U,
                   retryContext->SystemCallEntryContextAddress == 0U,
                   retryContext->ExternalInterruptEntryContextAddress == 0U,
                   HasReservedPageFaultWordsZero(*retryContext), retryWire->Slots[0U].LoadAcquire() == 1U,
                   rejectedWire->Slots[0U].LoadAcquire() == 0U, rejectedWire->RuntimeObjectStart == 0U,
                   rejectedWire->RuntimeObjectEnd == 0U, rejectedWire->AdmissionOwnerAddress == 0U);

    ResetNative();
    const auto installed =
        g_arena.Owners[0U].InstallAndAdmit(CpuId{0U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    if (installed.HasValue())
    {
        g_processorZeroRuntimeContextAddress = installed.GetValue().ContextAddress;
    }
    ASTERIA_EXPECT(runner, "exact validation forwards one context/table pair to one native call", installed.HasValue(),
                   g_installCallCount == 1U, g_installedTableAddress == installed.GetValue().TableAddress,
                   g_installedContextAddress == installed.GetValue().ContextAddress,
                   installed.GetValue().SizeBytes == Ia64RuntimeInterruptVectorTableSizeBytes,
                   installed.GetValue().ProcessorIndex == 0U);
    const auto empty = g_arena.Bindings[0U].AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "native success release-admits the exact slot as sentinel one", !empty.HasValue(),
                   empty.GetError() == Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger);

    const auto* const admittedWire =
        reinterpret_cast<const Ia64ActiveRuntimeTranslationBindingsWireStorage*>(&g_arena.Bindings[0U]);
    const Foundation::UInt64 configuredStart = admittedWire->RuntimeObjectStart;
    const Foundation::UInt64 configuredEnd = admittedWire->RuntimeObjectEnd;
    const Foundation::UInt64 configuredOwner = admittedWire->AdmissionOwnerAddress;
    ResetNative();
    const auto retainedRangeMismatch =
        g_arena.Owners[0U].InstallAndAdmit(CpuId{1U}, g_preparation, g_arena.Bindings[0U],
                                           Ia64RuntimeObjectRange{
                                               .VirtualStart = arenaRange.VirtualStart,
                                               .SizeBytes = arenaRange.SizeBytes - PageBytes,
                                           });
    ASTERIA_EXPECT(
        runner, "later installs must use the exact retained runtime-object range", !retainedRangeMismatch.HasValue(),
        retainedRangeMismatch.GetError() == Ia64RuntimeInterruptVectorTableInstallError::RuntimeObjectRangeMismatch,
        g_installCallCount == 0U);

    ResetNative();
    const auto installedOne =
        g_arena.Owners[0U].InstallAndAdmit(CpuId{1U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    const Foundation::UInt64 processorOneNativeContext = g_installedContextAddress;
    const auto installedThree =
        g_arena.Owners[0U].InstallAndAdmit(CpuId{3U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    ASTERIA_EXPECT(runner, "CPU0 then CPU1 then CPU3 use distinct contexts and immutable shared configuration",
                   installed.HasValue(), installedOne.HasValue(), installedThree.HasValue(), g_installCallCount == 2U,
                   processorOneNativeContext == installedOne.GetValue().ContextAddress,
                   g_installedContextAddress == installedThree.GetValue().ContextAddress,
                   installed.GetValue().ContextAddress != installedOne.GetValue().ContextAddress,
                   installedOne.GetValue().ContextAddress != installedThree.GetValue().ContextAddress,
                   HasReservedPageFaultWordsZeroAt(installed.GetValue().ContextAddress),
                   HasReservedPageFaultWordsZeroAt(installedOne.GetValue().ContextAddress),
                   HasReservedPageFaultWordsZeroAt(installedThree.GetValue().ContextAddress),
                   admittedWire->Slots[0U].LoadAcquire() == 1U, admittedWire->Slots[1U].LoadAcquire() == 1U,
                   admittedWire->Slots[2U].LoadAcquire() == 0U, admittedWire->Slots[3U].LoadAcquire() == 1U,
                   admittedWire->RuntimeObjectStart == configuredStart, admittedWire->RuntimeObjectEnd == configuredEnd,
                   admittedWire->AdmissionOwnerAddress == configuredOwner);

    ResetNative();
    const auto repeat = g_arena.Owners[0U].InstallAndAdmit(CpuId{0U}, g_preparation, g_arena.Bindings[0U], arenaRange);
    ASTERIA_EXPECT(runner, "repeat installation is rejected before a second native call", !repeat.HasValue(),
                   repeat.GetError() == Ia64RuntimeInterruptVectorTableInstallError::ProcessorAlreadyInstalled,
                   g_installCallCount == 0U);

    Ia64SystemCallEntryContext& systemCallContext = g_arena.SystemCallContexts[0U];
    g_installedContextAddressCpu0 = installed.GetValue().ContextAddress;
    auto* const installedContext = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(installed.GetValue().ContextAddress));
    AtomicUInt64Reference publication{installedContext->SystemCallEntryContextAddress};

    const auto invalidPublicationCpu = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{4U}, &systemCallContext);
    const auto notInstalled =
        g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{2U}, &g_arena.SystemCallContexts[2U]);
    const auto nullContext = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, nullptr);
    const auto* const misalignedContext = reinterpret_cast<const Ia64SystemCallEntryContext*>(
        static_cast<Foundation::UIntPointer>(AddressOf(&systemCallContext) + 1U));
    const auto misalignedPublication = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, misalignedContext);
    const auto* const nonRegionZeroContext = reinterpret_cast<const Ia64SystemCallEntryContext*>(
        static_cast<Foundation::UIntPointer>((Foundation::UInt64{1U} << 61U) - 16U));
    const auto nonRegionZero = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, nonRegionZeroContext);
    alignas(16U) Ia64SystemCallEntryContext outsideContext{};
    outsideContext.ProcessorIndex = 0U;
    const auto outsideRange = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &outsideContext);
    ASTERIA_EXPECT(runner, "invalid CPU, install state, pointer, region, and retained range do not publish",
                   invalidPublicationCpu == Ia64RuntimeInterruptVectorTablePublicationError::InvalidProcessor,
                   notInstalled == Ia64RuntimeInterruptVectorTablePublicationError::ProcessorNotInstalled,
                   nullContext == Ia64RuntimeInterruptVectorTablePublicationError::NullContext,
                   misalignedPublication == Ia64RuntimeInterruptVectorTablePublicationError::MisalignedContext,
                   nonRegionZero == Ia64RuntimeInterruptVectorTablePublicationError::ContextOutsideRegionZero,
                   outsideRange == Ia64RuntimeInterruptVectorTablePublicationError::ContextOutsideRuntimeObjectRange,
                   publication.LoadAcquire() == 0U);

    systemCallContext.ProcessorIndex = 1U;
    const auto processorMismatch = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext.ProcessorIndex = 0U;
    systemCallContext.Reserved = 1U;
    const auto reservedNotZero = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext.Reserved = 0U;
    ASTERIA_EXPECT(runner, "processor and reserved-field mismatches are failure-atomic",
                   processorMismatch == Ia64RuntimeInterruptVectorTablePublicationError::ProcessorMismatch,
                   reservedNotZero == Ia64RuntimeInterruptVectorTablePublicationError::ReservedNotZero,
                   publication.LoadAcquire() == 0U);

    const Ia64SystemCallEntryContext validSystemCallContext{systemCallContext};
    systemCallContext.BridgeContextAddress = 1U;
    const auto semanticInvalid = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;

    systemCallContext.FrameAddress = AddressOf(&g_outsideSystemCallFrame);
    const auto frameOutside = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;

    const Foundation::UInt64 outsideMemoryStackStart = AddressOf(&g_outsideSystemCallMemoryStack[0U]);
    systemCallContext.MemoryStackStart = outsideMemoryStackStart;
    systemCallContext.MemoryStackEnd = outsideMemoryStackStart + Ia64SystemCallMemoryStackSizeBytes;
    const auto memoryStackOutside = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;

    const Foundation::UInt64 outsideRseBackingStoreStart = AddressOf(&g_outsideSystemCallRseBackingStore[0U]);
    systemCallContext.RseBackingStoreStart = outsideRseBackingStoreStart;
    systemCallContext.RseBackingStoreEnd = outsideRseBackingStoreStart + Ia64SystemCallRseBackingStoreSizeBytes;
    const auto rseBackingStoreOutside = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;

    systemCallContext.FrameAddress = arenaRange.VirtualStart + arenaRange.SizeBytes - 16U;
    const auto frameCrossesCorridorEnd =
        g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;
    ASTERIA_EXPECT(runner, "semantic and referenced-storage failures leave publication zero",
                   semanticInvalid == Ia64RuntimeInterruptVectorTablePublicationError::InvalidSystemCallContext,
                   frameOutside ==
                       Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOutsideRuntimeObjectRange,
                   memoryStackOutside ==
                       Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOutsideRuntimeObjectRange,
                   rseBackingStoreOutside ==
                       Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOutsideRuntimeObjectRange,
                   frameCrossesCorridorEnd ==
                       Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOutsideRuntimeObjectRange,
                   publication.LoadAcquire() == 0U);

    systemCallContext.FrameAddress = AddressOf(&systemCallContext);
    const auto frameOverlapsContext = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;

    const Foundation::UInt64 contextStoragePage = AddressOf(&systemCallContext) & ~(PageBytes - 1U);
    systemCallContext.FrameAddress = AddressOf(&g_arena.Owners[0U]);
    systemCallContext.MemoryStackStart = contextStoragePage;
    systemCallContext.MemoryStackEnd = contextStoragePage + Ia64SystemCallMemoryStackSizeBytes;
    const auto memoryStackOverlapsContext =
        g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;

    const Foundation::UInt64 distantMemoryStackStart = AddressOf(&g_arena.SystemCallMemoryStacks[3U][0U]);
    systemCallContext.FrameAddress = AddressOf(&g_arena.Owners[0U]);
    systemCallContext.MemoryStackStart = distantMemoryStackStart;
    systemCallContext.MemoryStackEnd = distantMemoryStackStart + Ia64SystemCallMemoryStackSizeBytes;
    systemCallContext.RseBackingStoreStart = contextStoragePage;
    systemCallContext.RseBackingStoreEnd = contextStoragePage + Ia64SystemCallRseBackingStoreSizeBytes;
    const auto rseBackingStoreOverlapsContext =
        g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    systemCallContext = validSystemCallContext;
    ASTERIA_EXPECT(
        runner, "frame, memory-stack, and RSE aliases of the immutable context do not publish",
        frameOverlapsContext == Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOverlapsContext,
        memoryStackOverlapsContext == Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOverlapsContext,
        rseBackingStoreOverlapsContext ==
            Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOverlapsContext,
        publication.LoadAcquire() == 0U);

    const auto published = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    const Foundation::UInt64 observedAddress = publication.LoadAcquire();
    const auto* const observedContext =
        reinterpret_cast<const Ia64SystemCallEntryContext*>(static_cast<Foundation::UIntPointer>(observedAddress));
    const auto repeatedPublication = g_arena.Owners[0U].PublishSystemCallEntryContext(CpuId{0U}, &systemCallContext);
    ASTERIA_EXPECT(runner, "one release publication is acquire-observable and cannot be replaced",
                   published == Ia64RuntimeInterruptVectorTablePublicationError::None,
                   observedAddress == AddressOf(&systemCallContext), observedContext->ProcessorIndex == 0U,
                   observedContext->Reserved == 0U, HasReservedPageFaultWordsZero(*installedContext),
                   repeatedPublication == Ia64RuntimeInterruptVectorTablePublicationError::AlreadyPublished,
                   publication.LoadAcquire() == observedAddress);

    Ia64ExternalInterruptEntryContext& externalContext = g_arena.ExternalInterruptContexts[0U];
    Ia64LocalTimerInterruptEvidence& externalEvidence = g_arena.ExternalInterruptEvidence[0U];
    externalContext = Ia64ExternalInterruptEntryContext{
        .EvidenceAddress = AddressOf(&externalEvidence),
        .ProcessorIndex = 0U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .ExternalPreemptionControlAddress = 0U,
    };
    AtomicUInt64Reference externalPublication{installedContext->ExternalInterruptEntryContextAddress};

    const auto invalidExternalCpu =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{4U}, &externalContext);
    const auto externalProcessorNotInstalled =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{2U}, &g_arena.ExternalInterruptContexts[2U]);
    const auto nullExternalContext = g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, nullptr);
    const auto* const misalignedExternalContext = reinterpret_cast<const Ia64ExternalInterruptEntryContext*>(
        static_cast<Foundation::UIntPointer>(AddressOf(&externalContext) + 1U));
    const auto misalignedExternal =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, misalignedExternalContext);
    const auto* const nonRegionZeroExternalContext = reinterpret_cast<const Ia64ExternalInterruptEntryContext*>(
        static_cast<Foundation::UIntPointer>((Foundation::UInt64{1U} << 61U) - 16U));
    const auto nonRegionZeroExternal =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, nonRegionZeroExternalContext);
    alignas(16U) Ia64ExternalInterruptEntryContext outsideExternalContext{};
    const auto externalContextOutside =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &outsideExternalContext);
    ASTERIA_EXPECT(runner, "external publication rejects CPU, installation, context pointer, and corridor errors",
                   invalidExternalCpu == Ia64RuntimeExternalInterruptPublicationError::InvalidProcessor,
                   externalProcessorNotInstalled == Ia64RuntimeExternalInterruptPublicationError::ProcessorNotInstalled,
                   nullExternalContext == Ia64RuntimeExternalInterruptPublicationError::NullContext,
                   misalignedExternal == Ia64RuntimeExternalInterruptPublicationError::MisalignedContext,
                   nonRegionZeroExternal == Ia64RuntimeExternalInterruptPublicationError::ContextOutsideRegionZero,
                   externalContextOutside ==
                       Ia64RuntimeExternalInterruptPublicationError::ContextOutsideRuntimeObjectRange,
                   externalPublication.LoadAcquire() == 0U);

    externalContext.ProcessorIndex = 1U;
    const auto externalProcessorMismatch =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.ProcessorIndex = 0U;
    externalContext.TimerVector = 0xE7U;
    const auto externalVectorMismatch =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.TimerVector = Ia64ExternalInterruptTimerVector;
    externalContext.ExternalPreemptionControlAddress = 1U;
    const auto externalReserved = g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.ExternalPreemptionControlAddress = 0U;
    ASTERIA_EXPECT(runner, "external processor, vector, and reserved policy is failure-atomic",
                   externalProcessorMismatch == Ia64RuntimeExternalInterruptPublicationError::ProcessorMismatch,
                   externalVectorMismatch == Ia64RuntimeExternalInterruptPublicationError::InvalidVector,
                   externalReserved == Ia64RuntimeExternalInterruptPublicationError::ReservedNotZero,
                   externalPublication.LoadAcquire() == 0U);

    externalContext.EvidenceAddress = 0U;
    const auto nullEvidence = g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.EvidenceAddress = AddressOf(&externalEvidence) + 1U;
    const auto misalignedEvidence =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.EvidenceAddress = (Foundation::UInt64{1U} << 61U) - 56U;
    const auto evidenceOutsideRegionZero =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.EvidenceAddress = AddressOf(&g_outsideExternalInterruptEvidence);
    const auto evidenceOutsideCorridor =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.EvidenceAddress = AddressOf(&externalContext);
    const auto overlappingEvidence =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalContext.EvidenceAddress = AddressOf(&externalEvidence);
    externalEvidence.Sequence = 1U;
    const auto nonzeroEvidence = g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalEvidence = {};
    externalEvidence.AcknowledgedSequence = 1U;
    const auto nonzeroAcknowledgement =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    externalEvidence = {};
    ASTERIA_EXPECT(runner, "the full append-only evidence range and initial state are validated before release",
                   nullEvidence == Ia64RuntimeExternalInterruptPublicationError::InvalidEvidenceAddress,
                   misalignedEvidence == Ia64RuntimeExternalInterruptPublicationError::InvalidEvidenceAddress,
                   evidenceOutsideRegionZero == Ia64RuntimeExternalInterruptPublicationError::EvidenceOutsideRegionZero,
                   evidenceOutsideCorridor ==
                       Ia64RuntimeExternalInterruptPublicationError::EvidenceOutsideRuntimeObjectRange,
                   overlappingEvidence == Ia64RuntimeExternalInterruptPublicationError::EvidenceOverlapsContext,
                   nonzeroEvidence == Ia64RuntimeExternalInterruptPublicationError::EvidenceNotZero,
                   nonzeroAcknowledgement == Ia64RuntimeExternalInterruptPublicationError::EvidenceNotZero,
                   externalPublication.LoadAcquire() == 0U);

    const auto externalPublished = g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    const Foundation::UInt64 externalObservedAddress = externalPublication.LoadAcquire();
    const auto repeatedExternalPublication =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{0U}, &externalContext);
    ASTERIA_EXPECT(
        runner, "external context and zero evidence release-publish once at ar.k3 offset 24",
        externalPublished == Ia64RuntimeExternalInterruptPublicationError::None,
        externalObservedAddress == AddressOf(&externalContext),
        externalContext.EvidenceAddress == AddressOf(&externalEvidence), externalContext.ProcessorIndex == 0U,
        externalContext.TimerVector == Ia64ExternalInterruptTimerVector,
        externalContext.ExternalPreemptionControlAddress == 0U, HasReservedPageFaultWordsZero(*installedContext),
        repeatedExternalPublication == Ia64RuntimeExternalInterruptPublicationError::AlreadyPublished,
        externalPublication.LoadAcquire() == externalObservedAddress);

    Ia64ExternalInterruptEntryContext& processorOneExternalContext = g_arena.ExternalInterruptContexts[1U];
    Ia64LocalTimerInterruptEvidence& processorOneExternalEvidence = g_arena.ExternalInterruptEvidence[1U];
    processorOneExternalContext = Ia64ExternalInterruptEntryContext{
        .EvidenceAddress = AddressOf(&processorOneExternalEvidence),
        .ProcessorIndex = 1U,
        .TimerVector = Ia64ExternalInterruptTimerVector,
        .ExternalPreemptionControlAddress = 0U,
    };
    auto* const processorOneInstalledContext = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(installedOne.GetValue().ContextAddress));
    AtomicUInt64Reference processorOneExternalPublication{
        processorOneInstalledContext->ExternalInterruptEntryContextAddress};
    const auto processorOneExternalPublished =
        g_arena.Owners[0U].PublishExternalInterruptEntryContext(CpuId{1U}, &processorOneExternalContext);
    ASTERIA_EXPECT(runner, "CPU 0 and CPU 1 retain distinct acquire-visible offset-24 publications",
                   processorOneExternalPublished == Ia64RuntimeExternalInterruptPublicationError::None,
                   externalPublication.LoadAcquire() == AddressOf(&externalContext),
                   processorOneExternalPublication.LoadAcquire() == AddressOf(&processorOneExternalContext),
                   externalPublication.LoadAcquire() != processorOneExternalPublication.LoadAcquire(),
                   installedContext->ExternalInterruptEntryContextAddress == AddressOf(&externalContext),
                   processorOneInstalledContext->ExternalInterruptEntryContextAddress ==
                       AddressOf(&processorOneExternalContext),
                   HasReservedPageFaultWordsZero(*processorOneInstalledContext));

    g_activationStatus = 0U;
    g_activationCallCount = 0U;
    const auto activated = g_arena.Bindings[0U].ActivateAndBind(CpuId{0U}, g_arena.RegionSets[0U], g_arena.Ledgers[0U]);
    const auto acquired = g_arena.Bindings[0U].AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "only admitted in-corridor region and ledger objects can bind",
                   activated == Ia64ActiveRuntimeTranslationBindingError::None, g_activationCallCount == 1U,
                   acquired.HasValue(), acquired.GetValue() == &g_arena.Ledgers[0U]);

    Ia64RuntimeRegionSet outsideRegionSet;
    Ia64RuntimeTranslationLedger outsideLedger;
    const auto outsideRegion = g_arena.Bindings[0U].ActivateAndBind(CpuId{0U}, outsideRegionSet, g_arena.Ledgers[0U]);
    const auto outsideLedgerResult =
        g_arena.Bindings[0U].ActivateAndBind(CpuId{0U}, g_arena.RegionSets[0U], outsideLedger);
    ASTERIA_EXPECT(runner, "objects outside or at/above the configured end cannot reach activation",
                   outsideRegion == Ia64ActiveRuntimeTranslationBindingError::RegionSetOutsideRuntimeObjectRange,
                   outsideLedgerResult == Ia64ActiveRuntimeTranslationBindingError::LedgerOutsideRuntimeObjectRange,
                   g_activationCallCount == 1U);
}

void TestPageFaultContextPublication(TestContext& runner) noexcept
{
    if (g_processorZeroRuntimeContextAddress == 0U)
    {
        ASTERIA_EXPECT(runner, "page-fault publication has an installed CPU 0 root", false);
        return;
    }
    auto* const installedContext = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_processorZeroRuntimeContextAddress));
    AtomicUInt64Reference publication{installedContext->PageFaultEntryContextAddress};
    Ia64PageFaultEntryContext& context = g_arena.PageFaultContexts[0U];
    Ia64PageFaultEntryControl& control = g_arena.PageFaultControls[0U];

    const auto invalidProcessor = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{4U}, &context);
    const auto processorNotInstalled =
        g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{2U}, &g_arena.PageFaultContexts[2U]);
    const auto nullContext = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, nullptr);
    const auto* const misalignedContext = reinterpret_cast<const Ia64PageFaultEntryContext*>(
        static_cast<Foundation::UIntPointer>(AddressOf(&context) + 1U));
    const auto misaligned = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, misalignedContext);
    alignas(16U) Ia64PageFaultEntryContext outsideContext{};
    const auto contextOutside = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &outsideContext);
    ASTERIA_EXPECT(runner, "page-fault publication rejects invalid CPU and context identities atomically",
                   invalidProcessor == Ia64RuntimePageFaultPublicationError::InvalidProcessor,
                   processorNotInstalled == Ia64RuntimePageFaultPublicationError::ProcessorNotInstalled,
                   nullContext == Ia64RuntimePageFaultPublicationError::NullContext,
                   misaligned == Ia64RuntimePageFaultPublicationError::MisalignedContext,
                   contextOutside == Ia64RuntimePageFaultPublicationError::ContextOutsideRuntimeObjectRange,
                   publication.LoadAcquire() == 0U);

    const auto interruptedRseOverlapsContext =
        g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &g_arena.PageFaultOverlapContext);
    ASTERIA_EXPECT(runner,
                   "an interrupted user RBS may be outside Region 0 but cannot overlap "
                   "the immutable entry context",
                   interruptedRseOverlapsContext == Ia64RuntimePageFaultPublicationError::ReferencedStorageOverlaps,
                   publication.LoadAcquire() == 0U);

    context.ProcessorIndex = 1U;
    const auto processorMismatch = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    context.ProcessorIndex = 0U;
    ++context.InstructionAccessRightsVectorOffset;
    const auto vectorMismatch = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    --context.InstructionAccessRightsVectorOffset;
    context.Reserved[1U] = 1U;
    const auto reservedNotZero = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    context.Reserved[1U] = 0U;
    ASTERIA_EXPECT(runner, "processor, exact vector, and reserved context fields are immutable policy",
                   processorMismatch == Ia64RuntimePageFaultPublicationError::ProcessorMismatch,
                   vectorMismatch == Ia64RuntimePageFaultPublicationError::InvalidVectorIdentity,
                   reservedNotZero == Ia64RuntimePageFaultPublicationError::ReservedNotZero,
                   publication.LoadAcquire() == 0U);

    const Foundation::UInt64 configuredControlAddress = context.ControlAddress;
    context.ControlAddress = AddressOf(&g_arena.UnconfiguredPageFaultControl);
    const auto unconfiguredControl = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    context.ControlAddress = configuredControlAddress;

    const Foundation::UInt64 retainedFrameAddress = control.FrameAddress;
    control.FrameAddress = AddressOf(&g_outsidePageFaultFrame);
    const auto frameOutside = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    control.FrameAddress = retainedFrameAddress;

    Ia64PageFaultFrame& retainedFrame = g_arena.PageFaultFrames[0U];
    retainedFrame.State = Platform::Ia64::Interrupt::Ia64PageFaultFrameState::Capturing;
    const auto frameOwned = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    retainedFrame.State = Platform::Ia64::Interrupt::Ia64PageFaultFrameState::Inactive;
    retainedFrame.NextToken = 2U;
    const auto frameHasStaleGeneration = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    retainedFrame.NextToken = 0U;
    retainedFrame.InterruptionStatus = 1U;
    const auto frameHasStalePayload = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    retainedFrame.InterruptionStatus = 0U;

    const Foundation::UInt64 retainedStackStart = control.CoordinatorMemoryStackStart;
    const Foundation::UInt64 retainedStackEnd = control.CoordinatorMemoryStackEnd;
    control.CoordinatorMemoryStackStart = AddressOf(&g_outsidePageFaultCoordinatorStack[0U]);
    control.CoordinatorMemoryStackEnd = control.CoordinatorMemoryStackStart + Ia64PageFaultCoordinatorStackSizeBytes;
    const auto stackOutside = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    control.CoordinatorMemoryStackStart = retainedStackStart;
    control.CoordinatorMemoryStackEnd = retainedStackEnd;
    ASTERIA_EXPECT(runner,
                   "control, pristine frame, and coordinator storage require exact "
                   "retained admission",
                   unconfiguredControl == Ia64RuntimePageFaultPublicationError::ControlNotConfigured,
                   frameOutside == Ia64RuntimePageFaultPublicationError::FrameOutsideRuntimeObjectRange,
                   frameOwned == Ia64RuntimePageFaultPublicationError::FrameNotPristine,
                   frameHasStaleGeneration == Ia64RuntimePageFaultPublicationError::FrameNotPristine,
                   frameHasStalePayload == Ia64RuntimePageFaultPublicationError::FrameNotPristine,
                   stackOutside ==
                       Ia64RuntimePageFaultPublicationError::CoordinatorMemoryStackOutsideRuntimeObjectRange,
                   publication.LoadAcquire() == 0U);

    const auto published = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    const Foundation::UInt64 observed = publication.LoadAcquire();
    const auto repeated = g_arena.Owners[0U].PublishPageFaultEntryContext(CpuId{0U}, &context);
    ASTERIA_EXPECT(
        runner, "one complete page-fault corridor release-publishes at ar.k3 offset 32",
        published == Ia64RuntimePageFaultPublicationError::None, observed == AddressOf(&context),
        context.ControlAddress == AddressOf(&control), control.FrameAddress == AddressOf(&g_arena.PageFaultFrames[0U]),
        repeated == Ia64RuntimePageFaultPublicationError::AlreadyPublished, publication.LoadAcquire() == observed);
}

[[nodiscard]] Ia64RuntimeRefillProcessorState ValidState() noexcept
{
    return Ia64RuntimeRefillProcessorState{
        .CurrentProcessorStatus = TranslationMask,
        .InterruptedProcessorStatus = InterruptedRequired,
        .InterruptionStatus = 0U,
    };
}

/// A second user thread's contexts on CPU0: admitted once, then selected
/// whenever that thread is about to enter PL3.
void TestEntryContextSelection(TestContext& runner) noexcept
{
    auto* const root = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_installedContextAddressCpu0));
    AtomicUInt64Reference systemCallWord{root->SystemCallEntryContextAddress};
    AtomicUInt64Reference userFaultWord{root->UserFaultEntryContextAddress};
    const Foundation::UInt64 published = systemCallWord.LoadAcquire();

    const Foundation::UInt64 memoryStackStart = AddressOf(&g_arena.SystemCallMemoryStacks[2U][0U]);
    const Foundation::UInt64 rseStart = AddressOf(&g_arena.SystemCallRseBackingStores[2U][0U]);
    Ia64SystemCallEntryContext& second = g_arena.SecondThreadSystemCallContext;
    second = Ia64SystemCallEntryContext{
        .FrameAddress = AddressOf(&g_arena.SystemCallFrames[2U]),
        .MemoryStackStart = memoryStackStart,
        .MemoryStackEnd = memoryStackStart + Ia64SystemCallMemoryStackSizeBytes,
        .RseBackingStoreStart = rseStart,
        .RseBackingStoreEnd = rseStart + Ia64SystemCallRseBackingStoreSizeBytes,
        .BridgeContextAddress = AddressOf(&g_arena.Owners[2U]),
        .ProcessorIndex = 0U,
        .Reserved = 0U,
    };
    const auto unadmitted = g_arena.Owners[0U].SelectSystemCallEntryContext(CpuId{0U}, &second);
    second.ProcessorIndex = 1U;
    const auto mismatch = g_arena.Owners[0U].AdmitSystemCallEntryContext(CpuId{0U}, &second);
    second.ProcessorIndex = 0U;
    const auto admitted = g_arena.Owners[0U].AdmitSystemCallEntryContext(CpuId{0U}, &second);
    const auto readmitted = g_arena.Owners[0U].AdmitSystemCallEntryContext(CpuId{0U}, &second);
    const Foundation::UInt64 afterAdmission = systemCallWord.LoadAcquire();
    const auto selectedSecond = g_arena.Owners[0U].SelectSystemCallEntryContext(CpuId{0U}, &second);
    const Foundation::UInt64 secondWord = systemCallWord.LoadAcquire();
    const auto selectedFirst =
        g_arena.Owners[0U].SelectSystemCallEntryContext(CpuId{0U}, &g_arena.SystemCallContexts[0U]);
    const auto wrongProcessor = g_arena.Owners[0U].SelectSystemCallEntryContext(CpuId{1U}, &second);
    ASTERIA_EXPECT(runner, "a second system-call context is admitted once and then selected and deselected",
                   published == AddressOf(&g_arena.SystemCallContexts[0U]),
                   unadmitted == Ia64RuntimeInterruptVectorTablePublicationError::NotAdmitted,
                   mismatch == Ia64RuntimeInterruptVectorTablePublicationError::ProcessorMismatch,
                   admitted == Ia64RuntimeInterruptVectorTablePublicationError::None,
                   readmitted == Ia64RuntimeInterruptVectorTablePublicationError::None, afterAdmission == published,
                   selectedSecond == Ia64RuntimeInterruptVectorTablePublicationError::None,
                   secondWord == AddressOf(&second),
                   selectedFirst == Ia64RuntimeInterruptVectorTablePublicationError::None,
                   wrongProcessor == Ia64RuntimeInterruptVectorTablePublicationError::NotAdmitted,
                   systemCallWord.LoadAcquire() == published);

    for (Foundation::Size index{0U}; index < 2U; ++index)
    {
        const Foundation::Size storage = index == 0U ? 0U : 2U;
        const Foundation::UInt64 stack = AddressOf(&g_arena.SystemCallMemoryStacks[storage][0U]);
        const Foundation::UInt64 rse = AddressOf(&g_arena.SystemCallRseBackingStores[storage][0U]);
        g_arena.UserFaultContexts[index] = Ia64UserFaultEntryContext{
            .FrameAddress = AddressOf(&g_arena.PageFaultFrames[2U + index]),
            .MemoryStackStart = stack,
            .MemoryStackEnd = stack + Ia64SystemCallMemoryStackSizeBytes,
            .RseBackingStoreStart = rse,
            .RseBackingStoreEnd = rse + Ia64SystemCallRseBackingStoreSizeBytes,
            .HandlerAddress = AddressOf(&g_arena.Bindings[index]),
            .ProcessorIndex = 0U,
            .Reserved = 0U,
        };
    }
    Ia64UserFaultEntryContext& fault = g_arena.UserFaultContexts[0U];
    fault.Reserved = 1U;
    const auto reserved = g_arena.Owners[0U].AdmitUserFaultEntryContext(CpuId{0U}, &fault);
    fault.Reserved = 0U;
    fault.HandlerAddress = 4U;
    const auto badHandler = g_arena.Owners[0U].AdmitUserFaultEntryContext(CpuId{0U}, &fault);
    fault.HandlerAddress = AddressOf(&g_arena.Bindings[0U]);
    fault.FrameAddress = AddressOf(&g_outsideSystemCallFrame);
    const auto outside = g_arena.Owners[0U].AdmitUserFaultEntryContext(CpuId{0U}, &fault);
    fault.FrameAddress = AddressOf(&g_arena.PageFaultFrames[2U]);
    const auto faultAdmitted = g_arena.Owners[0U].AdmitUserFaultEntryContext(CpuId{0U}, &fault);
    const auto otherUnadmitted =
        g_arena.Owners[0U].SelectUserFaultEntryContext(CpuId{0U}, &g_arena.UserFaultContexts[1U]);
    const Foundation::UInt64 beforeSelection = userFaultWord.LoadAcquire();
    const auto faultSelected = g_arena.Owners[0U].SelectUserFaultEntryContext(CpuId{0U}, &fault);
    ASTERIA_EXPECT(runner, "a user-fault context is validated, admitted and selected at offset 40",
                   reserved == Ia64RuntimeInterruptVectorTablePublicationError::ReservedNotZero,
                   badHandler == Ia64RuntimeInterruptVectorTablePublicationError::InvalidUserFaultContext,
                   outside ==
                       Ia64RuntimeInterruptVectorTablePublicationError::ReferencedStorageOutsideRuntimeObjectRange,
                   faultAdmitted == Ia64RuntimeInterruptVectorTablePublicationError::None,
                   otherUnadmitted == Ia64RuntimeInterruptVectorTablePublicationError::NotAdmitted,
                   beforeSelection == 0U, faultSelected == Ia64RuntimeInterruptVectorTablePublicationError::None,
                   userFaultWord.LoadAcquire() == AddressOf(&fault));
}

void TestRefillModel(TestContext& runner) noexcept
{
    alignas(16U) Ia64RuntimeInterruptVectorTableContext context{};
    Ia64ActiveRuntimeTranslationBindingsWireStorage bindings{};
    Ia64RuntimeTranslationLedgerWireStorage ledger{};
    context.BindingsWireAddress = AddressOf(&bindings);
    context.ProcessorIndex = 0U;
    const Foundation::UInt64 ledgerAddress = AddressOf(&ledger);

    auto state = ValidState();
    auto result = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase,
                                                               Ia64RuntimeRefillKind::Instruction, state);
    ASTERIA_EXPECT(runner, "slot encoding zero is rejected before publication access", !result.HasValue(),
                   result.GetError() == Ia64RuntimeRefillError::InvalidBindingSlot);

    bindings.Slots[0U].StoreRelease(1U);
    result = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "slot encoding one is the admitted no-ledger sentinel", !result.HasValue(),
                   result.GetError() == Ia64RuntimeRefillError::NoActiveLedger);

    bindings.Slots[0U].StoreRelease(ledgerAddress);
    ledger.PublicationWord.StoreRelease(0U);
    result = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "publication zero is rejected", !result.HasValue(),
                   result.GetError() == Ia64RuntimeRefillError::InvalidPublication);

    ledger.PublicationWord.StoreRelease(1U);
    result = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "publication one scans an exact empty prefix", !result.HasValue(),
                   result.GetError() == Ia64RuntimeRefillError::MappingNotFound);

    constexpr Foundation::UInt64 baseWord{0x0010'0000'0020'0621ULL};
    ledger.Records[0U] = {
        .VirtualStart = RegionFiveBase,
        .BaseTranslationWord = baseWord,
        .PageCount = 2U,
    };
    ledger.PublicationWord.StoreRelease(2U);
    const auto first = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase + 0x11U,
                                                                    Ia64RuntimeRefillKind::Instruction, state);
    const auto last = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase + 0x1FFFU,
                                                                   Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "first and last mapped pages select exact instruction/data insertions", first.HasValue(),
                   last.HasValue(), first.GetValue().Kind == Ia64RuntimeRefillKind::Instruction,
                   last.GetValue().Kind == Ia64RuntimeRefillKind::Data,
                   first.GetValue().TranslationInsertion == Ia64RuntimeTranslationInsertion,
                   last.GetValue().TranslationInsertion == Ia64RuntimeTranslationInsertion,
                   first.GetValue().FaultingAddress == RegionFiveBase,
                   last.GetValue().FaultingAddress == RegionFiveBase + PageBytes,
                   first.GetValue().Outcome == Ia64RuntimeRefillOutcome::Insert,
                   last.GetValue().Outcome == Ia64RuntimeRefillOutcome::Insert,
                   (last.GetValue().TranslationWord & 0x0003'FFFF'FFFF'F000ULL) == PhysicalBase + PageBytes);

    for (Foundation::Size index{0U}; index < 256U; ++index)
    {
        ledger.Records[index] = {
            .VirtualStart = RegionFiveBase + (index * PageBytes),
            .BaseTranslationWord = baseWord + (index * PageBytes),
            .PageCount = 1U,
        };
    }
    ledger.PublicationWord.StoreRelease(257U);
    const auto finalRecord = Ia64RuntimeInterruptVectorTable::ModelRefill(
        context, RegionFiveBase + (255U * PageBytes) + 0xABU, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "publication 257 exposes the last of exactly 256 records", finalRecord.HasValue(),
                   finalRecord.GetValue().FaultingAddress == RegionFiveBase + (255U * PageBytes));

    ledger.PublicationWord.StoreRelease(258U);
    const auto tooLarge =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "publication 258 is rejected before a scan", !tooLarge.HasValue(),
                   tooLarge.GetError() == Ia64RuntimeRefillError::InvalidPublication);

    ledger.PublicationWord.StoreRelease(2U);
    const auto missing = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase + (300U * PageBytes),
                                                                      Ia64RuntimeRefillKind::Data, state);
    const auto invalidKind = Ia64RuntimeInterruptVectorTable::ModelRefill(
        context, RegionFiveBase, static_cast<Ia64RuntimeRefillKind>(0xFFU), state);
    ASTERIA_EXPECT(runner, "missing mappings and unknown model kinds produce no insertion", !missing.HasValue(),
                   missing.GetError() == Ia64RuntimeRefillError::MappingNotFound, !invalidKind.HasValue(),
                   invalidKind.GetError() == Ia64RuntimeRefillError::InvalidKind);

    state = ValidState();
    state.CurrentProcessorStatus |= (Foundation::UInt64{3U} << 32U) | (Foundation::UInt64{1U} << 34U) |
                                    (Foundation::UInt64{1U} << 44U) | (Foundation::UInt64{1U} << 46U);
    const auto unobservableCurrentFields =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    state = ValidState();
    state.CurrentProcessorStatus |= Foundation::UInt64{1U} << 14U;
    const auto observableCurrentInterrupt =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "the model ignores unobservable high current-PSR fields but enforces observable state",
                   unobservableCurrentFields.HasValue(), !observableCurrentInterrupt.HasValue(),
                   observableCurrentInterrupt.GetError() == Ia64RuntimeRefillError::IncompatibleCurrentState);

    state = ValidState();
    state.CurrentProcessorStatus ^= Foundation::UInt64{1U} << 36U;
    const auto currentInstructionTranslationMismatch =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "current PSR.it mismatch is rejected before any refill action",
                   !currentInstructionTranslationMismatch.HasValue(),
                   currentInstructionTranslationMismatch.GetError() ==
                       Ia64RuntimeRefillError::IncompatibleCurrentState);

    constexpr Foundation::UInt64 rseReferenceMask{Foundation::UInt64{1U} << 37U};
    constexpr Foundation::UInt64 incompleteRegisterFrameMask{Foundation::UInt64{1U} << 38U};
    constexpr Foundation::UInt64 nestedInterruptionMask{Foundation::UInt64{1U} << 39U};

    state = ValidState();
    const auto ordinaryData =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    state = ValidState();
    state.InterruptionStatus = rseReferenceMask;
    const auto rseData =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    state = ValidState();
    state.InterruptionStatus = rseReferenceMask | incompleteRegisterFrameMask;
    const auto incompleteRseData =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    state = ValidState();
    state.InterruptionStatus = incompleteRegisterFrameMask;
    const auto isolatedIncompleteData =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    ASTERIA_EXPECT(runner, "data refill admits ordinary and RSE-origin states only", ordinaryData.HasValue(),
                   rseData.HasValue(), incompleteRseData.HasValue(),
                   rseData.GetValue().TranslationWord == ordinaryData.GetValue().TranslationWord,
                   incompleteRseData.GetValue().TranslationWord == ordinaryData.GetValue().TranslationWord,
                   rseData.GetValue().FaultingAddress == ordinaryData.GetValue().FaultingAddress,
                   incompleteRseData.GetValue().TranslationInsertion == Ia64RuntimeTranslationInsertion,
                   !isolatedIncompleteData.HasValue(),
                   isolatedIncompleteData.GetError() == Ia64RuntimeRefillError::IncompleteRegisterFrame);

    state = ValidState();
    state.InterruptionStatus = rseReferenceMask;
    const auto rseInstruction = Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase,
                                                                             Ia64RuntimeRefillKind::Instruction, state);
    state = ValidState();
    state.InterruptionStatus = incompleteRegisterFrameMask;
    const auto incompleteInstruction = Ia64RuntimeInterruptVectorTable::ModelRefill(
        context, RegionFiveBase, Ia64RuntimeRefillKind::Instruction, state);
    state = ValidState();
    state.InterruptionStatus = rseReferenceMask | incompleteRegisterFrameMask;
    const auto incompleteRseInstruction = Ia64RuntimeInterruptVectorTable::ModelRefill(
        context, RegionFiveBase, Ia64RuntimeRefillKind::Instruction, state);
    state = ValidState();
    state.InterruptionStatus = nestedInterruptionMask;
    const auto nested =
        Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
    const auto nestedInstruction = Ia64RuntimeInterruptVectorTable::ModelRefill(
        context, RegionFiveBase, Ia64RuntimeRefillKind::Instruction, state);
    constexpr Foundation::UInt64 nestedInterruptionCompanions[]{
        rseReferenceMask,
        incompleteRegisterFrameMask,
        rseReferenceMask | incompleteRegisterFrameMask,
    };
    bool nestedInterruptionPrecedence{true};
    for (const Foundation::UInt64 companion : nestedInterruptionCompanions)
    {
        state = ValidState();
        state.InterruptionStatus = nestedInterruptionMask | companion;
        const auto mixedNestedData =
            Ia64RuntimeInterruptVectorTable::ModelRefill(context, RegionFiveBase, Ia64RuntimeRefillKind::Data, state);
        const auto mixedNestedInstruction = Ia64RuntimeInterruptVectorTable::ModelRefill(
            context, RegionFiveBase, Ia64RuntimeRefillKind::Instruction, state);
        nestedInterruptionPrecedence =
            nestedInterruptionPrecedence && !mixedNestedData.HasValue() &&
            (mixedNestedData.GetError() == Ia64RuntimeRefillError::NestedInterruption) &&
            !mixedNestedInstruction.HasValue() &&
            (mixedNestedInstruction.GetError() == Ia64RuntimeRefillError::NestedInterruption);
    }
    ASTERIA_EXPECT(
        runner, "instruction RSE/frame states and all nested misses fail locally", !rseInstruction.HasValue(),
        rseInstruction.GetError() == Ia64RuntimeRefillError::RseReference, !incompleteInstruction.HasValue(),
        incompleteInstruction.GetError() == Ia64RuntimeRefillError::IncompleteRegisterFrame,
        !incompleteRseInstruction.HasValue(),
        incompleteRseInstruction.GetError() == Ia64RuntimeRefillError::IncompleteRegisterFrame, !nested.HasValue(),
        nested.GetError() == Ia64RuntimeRefillError::NestedInterruption, !nestedInstruction.HasValue(),
        nestedInstruction.GetError() == Ia64RuntimeRefillError::NestedInterruption, nestedInterruptionPrecedence);
}

/// Only a PL3, non-RSE data miss leaves the data handler for the user-fault
/// entry; a mapped PL3 page is still inserted.
void TestRefillUserFaultEscape(TestContext& runner) noexcept
{
    alignas(16U) Ia64RuntimeInterruptVectorTableContext context{};
    Ia64ActiveRuntimeTranslationBindingsWireStorage bindings{};
    Ia64RuntimeTranslationLedgerWireStorage ledger{};
    context.BindingsWireAddress = AddressOf(&bindings);
    bindings.Slots[0U].StoreRelease(AddressOf(&ledger));
    ledger.Records[0U] = {
        .VirtualStart = RegionFiveBase,
        .BaseTranslationWord = 0x0010'0000'0020'0621ULL,
        .PageCount = 1U,
    };
    ledger.PublicationWord.StoreRelease(2U);

    constexpr Foundation::UInt64 userPrivilege{Foundation::UInt64{3U} << 32U};
    constexpr Foundation::UInt64 rseReference{Foundation::UInt64{1U} << 37U};
    constexpr Foundation::UInt64 incompleteFrame{Foundation::UInt64{1U} << 38U};
    constexpr Foundation::UInt64 missingAddress{RegionFiveBase + (7U * PageBytes) + 0x18U};
    const auto refill = [&](const Foundation::UInt64 address, const Ia64RuntimeRefillKind kind,
                            const Foundation::UInt64 privilege, const Foundation::UInt64 interruptionStatus) noexcept
    {
        Ia64RuntimeRefillProcessorState state = ValidState();
        state.InterruptedProcessorStatus |= privilege;
        state.InterruptionStatus = interruptionStatus;
        return Ia64RuntimeInterruptVectorTable::ModelRefill(context, address, kind, state);
    };

    const auto userMiss = refill(missingAddress, Ia64RuntimeRefillKind::Data, userPrivilege, 0U);
    const auto userMapped = refill(RegionFiveBase + 8U, Ia64RuntimeRefillKind::Data, userPrivilege, 0U);
    ASTERIA_EXPECT(runner, "a PL3 data miss escapes to the user-fault entry with the unchanged fault address",
                   userMiss.HasValue(), userMiss.GetValue().Outcome == Ia64RuntimeRefillOutcome::UserFault,
                   userMiss.GetValue().Kind == Ia64RuntimeRefillKind::Data,
                   userMiss.GetValue().FaultingAddress == missingAddress, userMiss.GetValue().TranslationWord == 0U,
                   userMiss.GetValue().TranslationInsertion == 0U, userMapped.HasValue(),
                   userMapped.GetValue().Outcome == Ia64RuntimeRefillOutcome::Insert,
                   userMapped.GetValue().FaultingAddress == RegionFiveBase);

    const auto supervisorMiss = refill(missingAddress, Ia64RuntimeRefillKind::Data, 0U, 0U);
    const auto userInstructionMiss = refill(missingAddress, Ia64RuntimeRefillKind::Instruction, userPrivilege, 0U);
    const auto userRseMiss = refill(missingAddress, Ia64RuntimeRefillKind::Data, userPrivilege, rseReference);
    const auto userIncompleteRseMiss =
        refill(missingAddress, Ia64RuntimeRefillKind::Data, userPrivilege, rseReference | incompleteFrame);
    const auto privilegeTwoMiss =
        refill(missingAddress, Ia64RuntimeRefillKind::Data, Foundation::UInt64{2U} << 32U, 0U);
    ASTERIA_EXPECT(
        runner, "kernel misses stop; instruction and RSE-origin PL3 misses escape to the pager",
        !supervisorMiss.HasValue(), supervisorMiss.GetError() == Ia64RuntimeRefillError::MappingNotFound,
        userInstructionMiss.HasValue(), userInstructionMiss.GetValue().Outcome == Ia64RuntimeRefillOutcome::UserFault,
        userRseMiss.HasValue(), userRseMiss.GetValue().Outcome == Ia64RuntimeRefillOutcome::UserFault,
        userIncompleteRseMiss.HasValue(),
        userIncompleteRseMiss.GetValue().Outcome == Ia64RuntimeRefillOutcome::UserFault, !privilegeTwoMiss.HasValue(),
        privilegeTwoMiss.GetError() == Ia64RuntimeRefillError::MappingNotFound);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeInterruptVectorTableTests"};
    ASTERIA_EXPECT(runner, "mapped runtime objects and trusted entry storage initialize",
                   Asteria::Test::PrepareFixture(), Asteria::Test::BuildRuntimeObjects(),
                   Asteria::Test::ConfigureSystemCallContexts(), Asteria::Test::ConfigurePageFaultContexts());
    Asteria::Test::TestTablePolicy(runner);
    Asteria::Test::TestInstallValidationAndSuccess(runner);
    Asteria::Test::TestPageFaultContextPublication(runner);
    Asteria::Test::TestEntryContextSelection(runner);
    Asteria::Test::TestRefillModel(runner);
    Asteria::Test::TestRefillUserFaultEscape(runner);
    return runner.Finish();
}
