// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_installCallCount{0U};
Foundation::UInt64 g_installStatus{0U};
Foundation::UInt64 g_installedContextAddress{0U};
Foundation::UInt64 g_installedTableAddress{0U};
Foundation::Size g_activationCallCount{0U};
Foundation::UInt64 g_activationStatus{0U};
Foundation::UInt64 g_activationWords[7U]{};
Foundation::Size g_nativeSequence{0U};
Foundation::Size g_installSequence{0U};
Foundation::Size g_activationSequence{0U};
Foundation::UInt8 g_nativeEvents[16U]{};
Kernel::Object::ObjectRegistry* g_observedRegistry{nullptr};
Foundation::Size g_registryCountAtInstall{~Foundation::Size{0U}};
Foundation::Size g_registryCountAtActivation{~Foundation::Size{0U}};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallRuntimeInterruptVectorTable(const Asteria::Foundation::UInt64 contextAddress,
                                              const Asteria::Foundation::UInt64 tableAddress) noexcept
{
    ++Asteria::Test::g_installCallCount;
    ++Asteria::Test::g_nativeSequence;
    Asteria::Test::g_installSequence = Asteria::Test::g_nativeSequence;
    if (Asteria::Test::g_nativeSequence <= sizeof(Asteria::Test::g_nativeEvents))
    {
        Asteria::Test::g_nativeEvents[Asteria::Test::g_nativeSequence - 1U] = 1U;
    }
    Asteria::Test::g_installedContextAddress = contextAddress;
    Asteria::Test::g_installedTableAddress = tableAddress;
    if (Asteria::Test::g_observedRegistry != nullptr)
    {
        Asteria::Test::g_registryCountAtInstall = Asteria::Test::g_observedRegistry->GetActiveCount();
    }
    return Asteria::Test::g_installStatus;
}

extern "C" [[noreturn]] void AsteriaIa64FailStopRuntimeInterruptVectorTableInstall() noexcept
{
    __builtin_trap();
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateRuntimeRegions(
    const Asteria::Foundation::UInt64 regionRegister1, const Asteria::Foundation::UInt64 regionRegister2,
    const Asteria::Foundation::UInt64 regionRegister3, const Asteria::Foundation::UInt64 regionRegister4,
    const Asteria::Foundation::UInt64 regionRegister5, const Asteria::Foundation::UInt64 regionRegister6,
    const Asteria::Foundation::UInt64 regionRegister7) noexcept
{
    ++Asteria::Test::g_activationCallCount;
    ++Asteria::Test::g_nativeSequence;
    Asteria::Test::g_activationSequence = Asteria::Test::g_nativeSequence;
    if (Asteria::Test::g_nativeSequence <= sizeof(Asteria::Test::g_nativeEvents))
    {
        Asteria::Test::g_nativeEvents[Asteria::Test::g_nativeSequence - 1U] = 2U;
    }
    Asteria::Test::g_activationWords[0U] = regionRegister1;
    Asteria::Test::g_activationWords[1U] = regionRegister2;
    Asteria::Test::g_activationWords[2U] = regionRegister3;
    Asteria::Test::g_activationWords[3U] = regionRegister4;
    Asteria::Test::g_activationWords[4U] = regionRegister5;
    Asteria::Test::g_activationWords[5U] = regionRegister6;
    Asteria::Test::g_activationWords[6U] = regionRegister7;
    if (Asteria::Test::g_observedRegistry != nullptr)
    {
        Asteria::Test::g_registryCountAtActivation = Asteria::Test::g_observedRegistry->GetActiveCount();
    }
    return Asteria::Test::g_activationStatus;
}

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Hal::Memory::AddressSpacePageTableActivationError;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::ProcessorAddressSpaceControllerError;
using Kernel::Memory::ProcessorAddressSpaceControllerState;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapInitialMappingCount;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64BspRuntimeBindingState;
using Platform::Ia64::Memory::Ia64BspRuntimeApplicationProcessorAdmissionError;
using Platform::Ia64::Memory::Ia64BspRuntimeApplicationProcessorAdmissionState;
using Platform::Ia64::Memory::Ia64BspRuntimeMemory;
using Platform::Ia64::Memory::Ia64BspRuntimeMemoryError;
using Platform::Ia64::Memory::Ia64BspRuntimeMemorySnapshot;
using Platform::Ia64::Memory::Ia64BspRuntimeMemoryState;
using Platform::Ia64::Memory::Ia64BspRuntimePageFaultEntryError;
using Platform::Ia64::Memory::Ia64BspRuntimePageFaultEntryState;
using Platform::Ia64::Memory::Ia64BspRuntimeRootBorrowError;
using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptAccessError;
using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptAcknowledgementError;
using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptPublicationError;
using Platform::Ia64::Memory::Ia64BspRuntimeExternalInterruptTransportSlot;
using Platform::Ia64::Memory::Ia64BspRuntimeSystemCallPublicationError;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableContext;
using Platform::Ia64::Memory::Ia64RuntimeDataAccessRightsOffset;
using Platform::Ia64::Memory::Ia64RuntimeExternalInterruptPublicationError;
using Platform::Ia64::Memory::Ia64RuntimeInstructionAccessRightsOffset;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableInstallError;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTablePublicationError;
using Platform::Ia64::Memory::Ia64RuntimeKeyPermissionOffset;
using Platform::Ia64::Memory::Ia64RuntimePageNotPresentOffset;
using Platform::Ia64::Memory::Ia64RuntimePageFaultPublicationError;
using Platform::Ia64::Memory::Ia64FirstRuntimeRegionIdentifier;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocatorState;
using Platform::Ia64::Memory::Ia64RuntimeAddressSpaceOwnerSnapshot;
using Platform::Ia64::Memory::Ia64RuntimeAddressSpaceOwnerState;
using Platform::Ia64::Memory::Ia64RuntimeInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilitiesError;
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
using Platform::Ia64::System::Ia64SystemCallEntryFrameState;
using Platform::Ia64::System::Ia64SystemCallMemoryStackSizeBytes;
using Platform::Ia64::System::Ia64SystemCallRseBackingStoreSizeBytes;
using Platform::Ia64::System::Ia64SystemCallRseTransitionSizeBytes;
using Platform::Ia64::System::ValidateIa64SystemCallTrustedContext;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptEntryContext;
using Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Platform::Ia64::Interrupt::Ia64LocalTimerInterruptEvidence;
using Platform::Ia64::Interrupt::AcquireIa64PageFaultEntryControlSnapshot;
using Platform::Ia64::Interrupt::ConfigureIa64PageFaultEntryControl;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorRseBackingStoreSizeBytes;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorRseTransitionSizeBytes;
using Platform::Ia64::Interrupt::Ia64PageFaultCoordinatorStackSizeBytes;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryContext;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControl;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlConfiguration;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlError;
using Platform::Ia64::Interrupt::Ia64PageFaultEntryControlState;
using Platform::Ia64::Interrupt::Ia64PageFaultCaptureStage;
using Platform::Ia64::Interrupt::Ia64PageFaultFrame;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameState;
using Platform::Ia64::Interrupt::Ia64PageFaultFrameIsPristine;
using Platform::Synchronization::AtomicUInt64Reference;
using Platform::Synchronization::SpinLock;

constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterRejectedBeforeCommit{1U};
constexpr Foundation::UInt64 AdapterCommittedStateUnknown{2U};
constexpr Foundation::UInt64 ChargeOwnerValue{0x4A64U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::Size RuntimeOwnerCount{3U};
constexpr Foundation::Size UnobservedRegistryCount{~Foundation::Size{0U}};

struct RuntimeArena
{
    Ia64BspRuntimeMemory Owners[RuntimeOwnerCount];
};

constinit SpinLock g_registrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_registry{g_registrySynchronization};
constinit RootCapabilityIssuer g_issuer{g_registry};
// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<RuntimeArena> g_arenaStorage{};
RuntimeArena& g_arena = g_arenaStorage.Construct();
// Byte snapshots of the owner, which contains a polymorphic page table; the
// comparisons read it through const void* and include its vtable pointer.
alignas(Ia64BspRuntimeMemory) constinit Foundation::UInt8 g_ownerByteSnapshot[sizeof(Ia64BspRuntimeMemory)]{};
constinit InitialAddressSpacePreparation g_preparation;
constinit InitialAddressSpacePreparation g_unprepared;
alignas(32U * 1024U) constinit Foundation::UInt8 g_initialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageBytes) constinit Foundation::UInt8 g_initialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
alignas(8U) constinit Foundation::UInt64 g_alignedBridgeContextFixture[3U]{};
alignas(Ia64PageFaultEntryControl) constinit Ia64PageFaultEntryControl g_foreignPageFaultControl{};
alignas(Ia64PageFaultEntryContext) constinit Ia64PageFaultEntryContext g_foreignPageFaultContext{};
alignas(Ia64PageFaultFrame) constinit Ia64PageFaultFrame g_foreignPageFaultFrame{};

static_assert(alignof(RuntimeArena) == PageBytes);
static_assert((sizeof(RuntimeArena) & (PageBytes - 1U)) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(RuntimeArena)) == 0x100'0000U);
static_assert(alignof(Ia64BspRuntimeMemory) == PageBytes);
static_assert((sizeof(Ia64BspRuntimeMemory) & (PageBytes - 1U)) == 0U);
static_assert(sizeof(Ia64ExternalInterruptEntryContext) == 32U);
static_assert(sizeof(Ia64LocalTimerInterruptEvidence) == 64U);
static_assert(sizeof(Ia64BspRuntimeExternalInterruptTransportSlot) == 0x70U);
static_assert(alignof(Ia64BspRuntimeExternalInterruptTransportSlot) == 16U);
static_assert(offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Context) == 0x00U);
static_assert(offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence) == 0x20U);
static_assert(offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, LastError) == 0x60U);
static_assert(offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Published) == 0x63U);
static_assert(offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Reserved) == 0x64U);
static_assert(offsetof(Ia64LocalTimerInterruptEvidence, AcknowledgedSequence) == 56U);
static_assert(Ia64SystemCallMemoryStackSizeBytes == 4U * PageBytes);
static_assert(Ia64SystemCallRseTransitionSizeBytes == 4U * PageBytes);
static_assert(Ia64SystemCallRseBackingStoreSizeBytes == 8U * PageBytes);
static_assert(Ia64PageFaultCoordinatorStackSizeBytes == 4U * PageBytes);
static_assert(Ia64PageFaultCoordinatorRseTransitionSizeBytes == 4U * PageBytes);
static_assert(Ia64PageFaultCoordinatorRseBackingStoreSizeBytes == 8U * PageBytes);

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr bool ContextIsZero(const Ia64SystemCallEntryContext& context) noexcept
{
    return (context.FrameAddress == 0U) && (context.MemoryStackStart == 0U) && (context.MemoryStackEnd == 0U) &&
           (context.RseBackingStoreStart == 0U) && (context.RseBackingStoreEnd == 0U) &&
           (context.BridgeContextAddress == 0U) && (context.ProcessorIndex == 0U) && (context.Reserved == 0U);
}

[[nodiscard]] constexpr bool ExternalContextIsZero(const Ia64ExternalInterruptEntryContext& context) noexcept
{
    return (context.EvidenceAddress == 0U) && (context.ProcessorIndex == 0U) && (context.TimerVector == 0U) &&
           (context.ExternalPreemptionControlAddress == 0U);
}

[[nodiscard]] constexpr bool PageFaultContextIsZero(const Ia64PageFaultEntryContext& context) noexcept
{
    return (context.ControlAddress == 0U) && (context.ProcessorIndex == 0U) &&
           (context.PageNotPresentVectorOffset == 0U) && (context.KeyPermissionVectorOffset == 0U) &&
           (context.InstructionAccessRightsVectorOffset == 0U) && (context.DataAccessRightsVectorOffset == 0U) &&
           (context.Reserved[0U] == 0U) && (context.Reserved[1U] == 0U);
}

[[nodiscard]] constexpr bool ExternalEvidenceIsZero(const Ia64LocalTimerInterruptEvidence& evidence) noexcept
{
    if ((evidence.Sequence != 0U) || (evidence.ProgrammedDeadlineTicks != 0U) ||
        (evidence.ObservedIntervalTimeCounter != 0U) || (evidence.InterruptedInstructionPointer != 0U) ||
        (evidence.InterruptedProcessorStatus != 0U) || (evidence.InterruptedIfsValid != 0U) ||
        (evidence.ProcessorIndex != 0U) || (evidence.Vector != 0U) || (evidence.AcknowledgedSequence != 0U))
    {
        return false;
    }
    for (const Foundation::UInt8 value : evidence.Reserved)
    {
        if (value != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool RangeIsInside(const Foundation::UInt64 corridorStart,
                                           const Foundation::UInt64 corridorSize, const Foundation::UInt64 rangeStart,
                                           const Foundation::UInt64 rangeSize) noexcept
{
    return (corridorSize != 0U) && (rangeSize != 0U) && (corridorStart <= (MaximumUInt64 - corridorSize)) &&
           (rangeStart <= (MaximumUInt64 - rangeSize)) && (rangeStart >= corridorStart) &&
           ((rangeStart + rangeSize) <= (corridorStart + corridorSize));
}

[[nodiscard]] Foundation::UInt64 AcquireSystemCallPublication() noexcept
{
    if (g_installedContextAddress == 0U)
    {
        return MaximumUInt64;
    }
    auto* const context = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_installedContextAddress));
    AtomicUInt64Reference publication{context->SystemCallEntryContextAddress};
    return publication.LoadAcquire();
}

[[nodiscard]] Foundation::UInt64 AcquireExternalInterruptPublication() noexcept
{
    if (g_installedContextAddress == 0U)
    {
        return MaximumUInt64;
    }
    auto* const context = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_installedContextAddress));
    AtomicUInt64Reference publication{context->ExternalInterruptEntryContextAddress};
    return publication.LoadAcquire();
}

[[nodiscard]] Foundation::UInt64 AcquirePageFaultPublication() noexcept
{
    if (g_installedContextAddress == 0U)
    {
        return MaximumUInt64;
    }
    auto* const context = reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(
        static_cast<Foundation::UIntPointer>(g_installedContextAddress));
    AtomicUInt64Reference publication{context->PageFaultEntryContextAddress};
    return publication.LoadAcquire();
}

[[nodiscard]] Foundation::UInt64 AcquireExternalInterruptPublicationAt(const Foundation::UInt64 contextAddress) noexcept
{
    if (contextAddress == 0U)
    {
        return MaximumUInt64;
    }
    auto* const context =
        reinterpret_cast<Ia64RuntimeInterruptVectorTableContext*>(static_cast<Foundation::UIntPointer>(contextAddress));
    AtomicUInt64Reference publication{context->ExternalInterruptEntryContextAddress};
    return publication.LoadAcquire();
}

[[nodiscard]] constexpr bool TryGetByteRangeEnd(const Foundation::UInt64 start, const Foundation::UInt64 sizeBytes,
                                                Foundation::UInt64& end) noexcept
{
    if ((sizeBytes == 0U) || (start > (MaximumUInt64 - sizeBytes)))
    {
        return false;
    }
    end = start + sizeBytes;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

template <Foundation::Size Count>
[[nodiscard]] constexpr bool RangesArePairwiseDisjoint(const Foundation::UInt64 (&starts)[Count],
                                                       const Foundation::UInt64 (&sizes)[Count]) noexcept
{
    Foundation::UInt64 ends[Count]{};
    for (Foundation::Size index{0U}; index < Count; ++index)
    {
        if (!TryGetByteRangeEnd(starts[index], sizes[index], ends[index]))
        {
            return false;
        }
    }
    for (Foundation::Size first{0U}; first < Count; ++first)
    {
        for (Foundation::Size second{first + 1U}; second < Count; ++second)
        {
            if (RangesOverlap(starts[first], ends[first], starts[second], ends[second]))
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr PalVirtualMemoryCapabilities
Capabilities(const Foundation::UInt8 regionIdentifierBits = 24U) noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = 50U,
                .ImplementedVirtualAddressMostSignificantBit = 60U,
                .RegionIdentifierBits = regionIdentifierBits,
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

void ResetNative(const Foundation::UInt64 activationStatus) noexcept
{
    g_installCallCount = 0U;
    g_installStatus = AdapterSuccess;
    g_installedContextAddress = 0U;
    g_installedTableAddress = 0U;
    g_activationCallCount = 0U;
    g_activationStatus = activationStatus;
    g_nativeSequence = 0U;
    g_installSequence = 0U;
    g_activationSequence = 0U;
    g_observedRegistry = &g_registry;
    g_registryCountAtInstall = UnobservedRegistryCount;
    g_registryCountAtActivation = UnobservedRegistryCount;
    for (Foundation::Size index{0U}; index < sizeof(g_nativeEvents); ++index)
    {
        g_nativeEvents[index] = 0U;
    }
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        g_activationWords[index] = 0U;
    }
}

[[nodiscard]] bool AllBindingsAre(const Ia64BspRuntimeMemorySnapshot& snapshot,
                                  const Ia64BspRuntimeBindingState state) noexcept
{
    for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        if (snapshot.Bindings[index] != state)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool OnlyBspBindingIs(const Ia64BspRuntimeMemorySnapshot& snapshot, const CpuId bspProcessor,
                                    const Ia64BspRuntimeBindingState state) noexcept
{
    for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumProcessorCount; ++index)
    {
        const Ia64BspRuntimeBindingState expected =
            index == bspProcessor.GetValue() ? state : Ia64BspRuntimeBindingState::Unadmitted;
        if (snapshot.Bindings[index] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ActivationWordsAreExact() noexcept
{
    for (Foundation::Size index{1U}; index < 8U; ++index)
    {
        const Foundation::UInt64 regionIdentifier =
            static_cast<Foundation::UInt64>(Ia64FirstRuntimeRegionIdentifier + index);
        const Foundation::UInt64 expected = (regionIdentifier << 8U) | 0x30U;
        if (g_activationWords[index - 1U] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool NativeEventsAlternateExactly(const Foundation::Size pairCount) noexcept
{
    if (g_nativeSequence != (pairCount * 2U))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < pairCount; ++index)
    {
        if ((g_nativeEvents[index * 2U] != 1U) || (g_nativeEvents[(index * 2U) + 1U] != 2U))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AllocatorHasOneExactReceipt(const Ia64BspRuntimeMemorySnapshot& snapshot,
                                               const Foundation::UInt8 expectedRegionIdentifierBits = 24U) noexcept
{
    return (snapshot.RegionIdentifierAllocator.State == Ia64RegionIdentifierAllocatorState::Ready) &&
           (snapshot.RegionIdentifierAllocator.NextRegionIdentifier == 0x2010U) &&
           (snapshot.RegionIdentifierAllocator.AllocatedAddressSpaceCount == 1U) &&
           (snapshot.RegionIdentifierAllocator.RegionIdentifierBits == expectedRegionIdentifierBits);
}

// The common root, the initial component's and the boot pager's.
[[nodiscard]] bool AllocatorHasThreeExactReceipts(const Ia64BspRuntimeMemorySnapshot& snapshot,
                                                  const Foundation::UInt8 expectedRegionIdentifierBits = 24U) noexcept
{
    return (snapshot.RegionIdentifierAllocator.State == Ia64RegionIdentifierAllocatorState::Ready) &&
           (snapshot.RegionIdentifierAllocator.NextRegionIdentifier == 0x2020U) &&
           (snapshot.RegionIdentifierAllocator.AllocatedAddressSpaceCount == 3U) &&
           (snapshot.RegionIdentifierAllocator.RegionIdentifierBits == expectedRegionIdentifierBits);
}

[[nodiscard]] bool RegisteredRootIsExact(const Ia64BspRuntimeMemorySnapshot& snapshot,
                                         const ResourceDomainId chargeOwner) noexcept
{
    if ((snapshot.AddressSpaceStorage == nullptr) || !snapshot.Summary.AddressSpaceIdentity.IsValid() ||
        !snapshot.Summary.AddressSpaceIdentity.IsFromRegistry(g_registry))
    {
        return false;
    }
    ObjectPin pin;
    if (g_registry.PinObject(snapshot.Summary.AddressSpaceIdentity.GetObjectId(), ObjectType::AddressSpace, pin) !=
        ObjectPinError::Ok)
    {
        return false;
    }
    const IKernelObject& object = pin.GetObject();
    return pin.IsValid() && (pin.GetObjectId().GetValue() == snapshot.Summary.AddressSpaceIdentity.GetValue()) &&
           (pin.GetObjectType() == ObjectType::AddressSpace) &&
           (&object == static_cast<const IKernelObject*>(snapshot.AddressSpaceStorage)) &&
           (object.GetObjectType() == ObjectType::AddressSpace) &&
           (object.GetChargeOwner().GetValue() == chargeOwner.GetValue());
}

void TestRetryablePreflightAndSuccess(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{2U};
    const ResourceDomainId chargeOwner{ChargeOwnerValue};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[0U];
    ResetNative(AdapterSuccess);
    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto invalidFreshExternalPublication = owner.PublishExternalInterruptEntryContext(CpuId{4U});
    const auto invalidFreshExternalAcknowledgement = owner.AcknowledgeExternalInterruptEvidence(CpuId{4U}, 1U);
    const auto freshPageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    const bool invalidFreshExternalPreservedOwner =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    const auto freshPublication =
        owner.PublishSystemCallEntryContext(bspProcessor, AddressOf(&g_alignedBridgeContextFixture));
    const auto freshExternalInterruptPublication = owner.PublishExternalInterruptEntryContext(bspProcessor);
    const auto freshExternalInterruptAcknowledgement = owner.AcknowledgeExternalInterruptEvidence(1U);
    const auto freshExternalTypedSnapshot = owner.GetExternalInterruptPublicationSnapshot(bspProcessor);
    const auto freshExternalTypedView = owner.GetPublishedExternalInterruptEvidence(bspProcessor);
    const Ia64BspRuntimeMemorySnapshot afterFreshPublication = owner.GetSnapshot();
    const auto initialBorrow = owner.BorrowReadyRoot();
    const auto initialApplicationProcessorBorrow = owner.BorrowReadyApplicationProcessorRoot(CpuId{1U});
    ASTERIA_EXPECT(
        runner, "Fresh owner isolates valid diagnostics while invalid CPU access mutates no transport or root byte",
        invalidFreshExternalPublication == Ia64BspRuntimeExternalInterruptPublicationError::InvalidProcessor,
        invalidFreshExternalAcknowledgement == Ia64BspRuntimeExternalInterruptAcknowledgementError::InvalidProcessor,
        invalidFreshExternalPreservedOwner, freshPublication == Ia64BspRuntimeSystemCallPublicationError::NotReady,
        !afterFreshPublication.SystemCallPublication.Published,
        ContextIsZero(afterFreshPublication.SystemCallPublication.Context),
        afterFreshPublication.SystemCallPublication.LastError == Ia64BspRuntimeSystemCallPublicationError::NotReady,
        afterFreshPublication.SystemCallPublication.LastContextValidationError ==
            Ia64SystemCallContextValidationError::None,
        afterFreshPublication.SystemCallPublication.LastInterruptVectorTableError ==
            Ia64RuntimeInterruptVectorTablePublicationError::None,
        freshExternalInterruptPublication == Ia64BspRuntimeExternalInterruptPublicationError::NotReady,
        freshExternalInterruptAcknowledgement == Ia64BspRuntimeExternalInterruptAcknowledgementError::NotReady,
        !freshPageFaultStorage.HasValue(),
        freshPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::NotReady,
        afterFreshPublication.PageFaultEntry.State == Ia64BspRuntimePageFaultEntryState::Fresh,
        afterFreshPublication.PageFaultEntry.LastError == Ia64BspRuntimePageFaultEntryError::None,
        afterFreshPublication.PageFaultEntry.LastInterruptVectorTableError ==
            Ia64RuntimePageFaultPublicationError::None,
        PageFaultContextIsZero(afterFreshPublication.PageFaultEntry.Context),
        AcquirePageFaultPublication() == MaximumUInt64, !afterFreshPublication.ExternalInterruptPublication.Published,
        afterFreshPublication.ExternalInterruptPublication.LastError ==
            Ia64BspRuntimeExternalInterruptPublicationError::None,
        afterFreshPublication.ExternalInterruptPublication.LastInterruptVectorTableError ==
            Ia64RuntimeExternalInterruptPublicationError::None,
        afterFreshPublication.ExternalInterruptPublication.LastAcknowledgementError ==
            Ia64BspRuntimeExternalInterruptAcknowledgementError::NotReady,
        afterFreshPublication.ExternalInterruptPublication.Evidence.Sequence == 0U,
        !freshExternalTypedSnapshot.HasValue(),
        freshExternalTypedSnapshot.GetError() == Ia64BspRuntimeExternalInterruptAccessError::NotReady,
        !freshExternalTypedView.HasValue(),
        freshExternalTypedView.GetError() == Ia64BspRuntimeExternalInterruptAccessError::NotReady,
        !initialBorrow.HasValue(), initialBorrow.GetError() == Ia64BspRuntimeRootBorrowError::NotReady,
        !initialApplicationProcessorBorrow.HasValue(),
        initialApplicationProcessorBorrow.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::NotReady);
    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto rejected =
        owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_unprepared, chargeOwner, g_registry, g_issuer);
    const bool rejectedOwnerUnchanged =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    const Ia64BspRuntimeMemorySnapshot fresh = owner.GetSnapshot();
    const auto retryableFreshBorrow = owner.BorrowReadyRoot();
    ASTERIA_EXPECT(
        runner, "invalid IVT preflight leaves every owner byte Fresh and retryable", !rejected.HasValue(),
        rejected.GetError() == Ia64BspRuntimeMemoryError::InterruptVectorTablePreflightRejected,
        fresh.State == Ia64BspRuntimeMemoryState::Fresh, fresh.Failure.Error == Ia64BspRuntimeMemoryError::None,
        fresh.RegionIdentifierAllocator.State == Ia64RegionIdentifierAllocatorState::Uninitialized,
        fresh.RegionIdentifierAllocator.NextRegionIdentifier == 0U,
        fresh.RegionIdentifierAllocator.AllocatedAddressSpaceCount == 0U,
        fresh.AddressSpace.State == AddressSpaceState::Uninitialized,
        fresh.Controller.State == ProcessorAddressSpaceControllerState::Uninitialized,
        AllBindingsAre(fresh, Ia64BspRuntimeBindingState::Unadmitted), g_registry.GetActiveCount() == 0U,
        g_installCallCount == 0U, g_activationCallCount == 0U, g_registryCountAtInstall == UnobservedRegistryCount,
        g_registryCountAtActivation == UnobservedRegistryCount, g_nativeSequence == 0U, rejectedOwnerUnchanged,
        !retryableFreshBorrow.HasValue(), retryableFreshBorrow.GetError() == Ia64BspRuntimeRootBorrowError::NotReady);

    const PalVirtualMemoryCapabilities bspCapabilities = Capabilities();
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    processorCapabilities[0U] = Capabilities(18U);
    processorCapabilities[bspProcessor.GetValue()] = bspCapabilities;
    processorCapabilities[3U] = Capabilities(20U);
    constexpr Foundation::UInt8 profiledProcessorMask{0x0DU};
    const auto prepared = owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities,
                                                g_preparation, chargeOwner, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot ready = owner.GetSnapshot();
    const auto borrowed = owner.BorrowReadyRoot();
    const auto borrowedAgain = owner.BorrowReadyRoot();
    const auto initialComponentRoot = owner.BorrowReadyInitialComponentRoot();
    const Ia64RuntimeAddressSpaceOwnerSnapshot initialComponent = owner.GetInitialComponentAddressSpaceSnapshot();
    const auto invalidApplicationProcessorBorrow = owner.BorrowReadyApplicationProcessorRoot(CpuId{4U});
    const auto bspApplicationProcessorBorrow = owner.BorrowReadyApplicationProcessorRoot(bspProcessor);
    const auto unprofiledApplicationProcessorBorrow = owner.BorrowReadyApplicationProcessorRoot(CpuId{1U});
    const auto unadmittedApplicationProcessorBorrow = owner.BorrowReadyApplicationProcessorRoot(CpuId{0U});
    const Foundation::UInt64 ownerAddress = AddressOf(&owner);
    ASTERIA_EXPECT(
        runner, "the full profiled mask is retained while only the nonzero BSP becomes resident", prepared.HasValue(),
        ready.State == Ia64BspRuntimeMemoryState::Ready, ready.Failure.Error == Ia64BspRuntimeMemoryError::None,
        ready.Summary.RuntimeObjects.VirtualStart == ownerAddress,
        ready.Summary.RuntimeObjects.SizeBytes == sizeof(owner),
        (ready.Summary.RuntimeObjects.VirtualStart & (PageBytes - 1U)) == 0U,
        (ready.Summary.RuntimeObjects.SizeBytes & (PageBytes - 1U)) == 0U,
        ready.Summary.BspProcessorIndex == bspProcessor.GetValue(),
        ready.Summary.ProfiledProcessorMask == profiledProcessorMask, ready.Summary.ResidentProcessorMask == 0x04U,
        ready.Summary.FirstRegionIdentifier == 0x2008U, ready.Summary.LastRegionIdentifier == 0x200FU,
        ready.Summary.BackendClaimed, ready.Summary.ExactLedgerVerified);
    ASTERIA_EXPECT(
        runner, "AP root borrowing distinguishes invalid, BSP, unprofiled, and profiled-unadmitted requests",
        !invalidApplicationProcessorBorrow.HasValue(),
        invalidApplicationProcessorBorrow.GetError() ==
            Ia64BspRuntimeApplicationProcessorAdmissionError::InvalidProcessor,
        !bspApplicationProcessorBorrow.HasValue(),
        bspApplicationProcessorBorrow.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::BspProcessor,
        !unprofiledApplicationProcessorBorrow.HasValue(),
        unprofiledApplicationProcessorBorrow.GetError() ==
            Ia64BspRuntimeApplicationProcessorAdmissionError::ProcessorNotProfiled,
        !unadmittedApplicationProcessorBorrow.HasValue(),
        unadmittedApplicationProcessorBorrow.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::NotReady);
    ASTERIA_EXPECT(
        runner, "the exact registered common AddressSpace owns the canonical seed mappings and BSP residency",
        ready.AddressSpaceStorage != nullptr, ready.AddressSpace.Identity.IsEqualTo(ready.Summary.AddressSpaceIdentity),
        ready.AddressSpace.State == AddressSpaceState::Ready,
        ready.AddressSpace.MappingCount == BootstrapInitialMappingCount,
        ready.Summary.MappingCount == BootstrapInitialMappingCount, ready.AddressSpace.ResidentProcessorMask == 0x04U,
        ready.AddressSpace.TransitionSequence == 0U, !ready.AddressSpace.ActivatingProcessor.IsValid(),
        !ready.AddressSpace.TransitionProcessor.IsValid(), ready.AddressSpace.TargetProcessorMask == 0U,
        ready.AddressSpace.AcknowledgedProcessorMask == 0U, g_registry.GetActiveCount() == 3U,
        RegisteredRootIsExact(ready, chargeOwner));
    ASTERIA_EXPECT(
        runner,
        "one distinct registered initial-component AddressSpace retains the second full RID receipt and starts unresident",
        initialComponentRoot.HasValue(), borrowed.HasValue(),
        initialComponent.State == Ia64RuntimeAddressSpaceOwnerState::Ready,
        initialComponent.Summary.State == Ia64RuntimeAddressSpaceOwnerState::Ready,
        initialComponent.Summary.FirstRegionIdentifier == 0x2010U,
        initialComponent.Summary.LastRegionIdentifier == 0x2017U,
        initialComponent.Summary.AllocatedAddressSpaceCount == 2U,
        initialComponent.Summary.MappingCount == BootstrapInitialMappingCount, initialComponent.Summary.BackendClaimed,
        initialComponent.Summary.AddressSpaceIdentity.IsValid(),
        !initialComponent.Summary.AddressSpaceIdentity.IsEqualTo(ready.Summary.AddressSpaceIdentity),
        initialComponent.AddressSpaceStorage == initialComponentRoot.GetValue().AddressSpaceStorage,
        initialComponent.AddressSpace.Identity.IsEqualTo(initialComponent.Summary.AddressSpaceIdentity),
        initialComponent.AddressSpace.State == AddressSpaceState::Ready,
        initialComponent.AddressSpace.MappingCount == BootstrapInitialMappingCount,
        initialComponent.AddressSpace.ResidentProcessorMask == 0U,
        initialComponentRoot.GetValue().ControllerStorage == borrowed.GetValue().ControllerStorage,
        initialComponentRoot.GetValue().Processor.GetValue() == bspProcessor.GetValue());
    ASTERIA_EXPECT(
        runner, "controller Ready state and initial receipt name only the exact embedded root",
        ready.Summary.ControllerState == ProcessorAddressSpaceControllerState::Ready,
        ready.Controller.State == ProcessorAddressSpaceControllerState::Ready,
        ready.Controller.IdleStorage == ready.AddressSpaceStorage,
        ready.Controller.ActiveStorage == ready.AddressSpaceStorage, ready.Controller.PreviousStorage == nullptr,
        ready.Controller.Idle.IsEqualTo(ready.Summary.AddressSpaceIdentity),
        ready.Controller.Active.IsEqualTo(ready.Summary.AddressSpaceIdentity), !ready.Controller.Previous.IsValid(),
        !ready.Controller.SourceTransition.IsValid(), !ready.Controller.DestinationTransition.IsValid(),
        ready.Controller.Processor.GetValue() == bspProcessor.GetValue(),
        !ready.Summary.ControllerReceipt.Previous.IsValid(),
        ready.Summary.ControllerReceipt.Active.IsEqualTo(ready.Summary.AddressSpaceIdentity),
        ready.Summary.ControllerReceipt.Processor.GetValue() == bspProcessor.GetValue(),
        ready.Summary.ControllerReceipt.NativeActivationPerformed);
    ASTERIA_EXPECT(
        runner, "Ready borrow exposes the stable exact root, identity, controller, and BSP", borrowed.HasValue(),
        borrowedAgain.HasValue(), borrowed.GetValue().AddressSpaceStorage == ready.AddressSpaceStorage,
        borrowed.GetValue().AddressSpaceIdentity.IsEqualTo(ready.Summary.AddressSpaceIdentity),
        borrowed.GetValue().ControllerStorage != nullptr,
        borrowed.GetValue().ControllerStorage == borrowedAgain.GetValue().ControllerStorage,
        borrowed.GetValue().AddressSpaceStorage == borrowedAgain.GetValue().AddressSpaceStorage,
        borrowed.GetValue().AddressSpaceIdentity.IsEqualTo(borrowedAgain.GetValue().AddressSpaceIdentity),
        borrowed.GetValue().Processor.GetValue() == bspProcessor.GetValue(),
        borrowed.GetValue().ControllerStorage->GetSnapshot().IdleStorage == borrowed.GetValue().AddressSpaceStorage,
        borrowed.GetValue().ControllerStorage->GetSnapshot().Idle.IsEqualTo(borrowed.GetValue().AddressSpaceIdentity));
    ASTERIA_EXPECT(runner, "the BSP binding publishes the exact backend-private empty runtime ledger",
                   OnlyBspBindingIs(ready, bspProcessor, Ia64BspRuntimeBindingState::Active),
                   ready.Summary.ExactLedgerVerified);
    ASTERIA_EXPECT(runner, "heterogeneous RID widths reduce to the common minimum used by the allocator",
                   AllocatorHasThreeExactReceipts(ready, 18U),
                   ready.Summary.CommonCapabilities.RegionIdentifierBits ==
                       processorCapabilities[0U].Translation.RegionIdentifierBits,
                   ready.Summary.CommonCapabilities.PhysicalAddressBits ==
                       bspCapabilities.Translation.PhysicalAddressBits);
    ASTERIA_EXPECT(runner, "one IVT install precedes registration and one exact RR1-RR7 activation",
                   g_installCallCount == 1U, g_activationCallCount == 1U, g_installSequence == 1U,
                   g_activationSequence == 2U, g_registryCountAtInstall == 0U, g_registryCountAtActivation == 1U,
                   g_installedTableAddress == ready.Summary.InterruptVectorTable.TableAddress,
                   g_installedContextAddress == ready.Summary.InterruptVectorTable.ContextAddress,
                   ready.Summary.InterruptVectorTable.ProcessorIndex == bspProcessor.GetValue(),
                   ready.Summary.InterruptVectorTable.InstalledProcessorMask == 0x04U,
                   ready.Summary.InterruptVectorTable.SizeBytes == Ia64RuntimeInterruptVectorTableSizeBytes,
                   ActivationWordsAreExact());

    auto* const pristineAdmissionFrame =
        reinterpret_cast<Ia64PageFaultFrame*>(static_cast<Foundation::UIntPointer>(ready.PageFaultEntry.FrameAddress));
    pristineAdmissionFrame->CaptureToken = 7U;
    const auto staleTokenPageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    pristineAdmissionFrame->CaptureToken = 0U;
    pristineAdmissionFrame->CaptureStage = Ia64PageFaultCaptureStage::Decoded;
    const auto staleStagePageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    pristineAdmissionFrame->CaptureStage = Ia64PageFaultCaptureStage::None;
    pristineAdmissionFrame->GeneralRegisters[0U] = 0xA5U;
    const auto staleRegisterPageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    pristineAdmissionFrame->GeneralRegisters[0U] = 0U;
    pristineAdmissionFrame->CapturedEntry.ResumeTarget.InstructionPointer = 0x1000U;
    const auto stalePayloadPageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    const auto afterStalePageFaultStorage = owner.GetPageFaultEntrySnapshot();
    pristineAdmissionFrame->CapturedEntry.ResumeTarget.InstructionPointer = 0U;
    const bool restoredAdmissionFrameIsPristine = Ia64PageFaultFrameIsPristine(*pristineAdmissionFrame);
    const auto invalidPageFaultStorage = owner.BorrowPageFaultEntryStorage(CpuId{4U});
    const auto foreignPageFaultStorage = owner.BorrowPageFaultEntryStorage(CpuId{1U});
    const auto pageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    const auto repeatedPageFaultStorage = owner.BorrowPageFaultEntryStorage(bspProcessor);
    ASTERIA_EXPECT(
        runner, "stale token, stage, register, and payload bytes reject without consuming the BSP's retryable borrow",
        !staleTokenPageFaultStorage.HasValue(),
        staleTokenPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::FrameNotPristine,
        !staleStagePageFaultStorage.HasValue(),
        staleStagePageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::FrameNotPristine,
        !staleRegisterPageFaultStorage.HasValue(),
        staleRegisterPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::FrameNotPristine,
        !stalePayloadPageFaultStorage.HasValue(),
        stalePayloadPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::FrameNotPristine,
        afterStalePageFaultStorage.State == Ia64BspRuntimePageFaultEntryState::Fresh,
        PageFaultContextIsZero(afterStalePageFaultStorage.Context), restoredAdmissionFrameIsPristine,
        !invalidPageFaultStorage.HasValue(),
        invalidPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::InvalidProcessor,
        !foreignPageFaultStorage.HasValue(),
        foreignPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::NotBspProcessor,
        pageFaultStorage.HasValue(), !repeatedPageFaultStorage.HasValue(),
        repeatedPageFaultStorage.GetError() == Ia64BspRuntimePageFaultEntryError::StorageAlreadyBorrowed);
    if (pageFaultStorage.HasValue())
    {
        const auto storage = pageFaultStorage.GetValue();
        const Foundation::UInt64 starts[5U]{
            storage.ContextAddress,
            storage.ControlAddress,
            storage.FrameAddress,
            storage.CoordinatorMemoryStackAddress,
            storage.CoordinatorRseBackingStoreAddress,
        };
        const Foundation::UInt64 sizes[5U]{
            sizeof(Ia64PageFaultEntryContext),
            sizeof(Ia64PageFaultEntryControl),
            sizeof(Ia64PageFaultFrame),
            storage.CoordinatorMemoryStackSizeBytes,
            storage.CoordinatorRseBackingStoreSizeBytes,
        };
        bool allInside{true};
        bool allLocked{true};
        for (Foundation::Size index{0U}; index < 5U; ++index)
        {
            allInside = allInside && RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart,
                                                   ready.Summary.RuntimeObjects.SizeBytes, starts[index], sizes[index]);
            allLocked = allLocked && owner.HasLockedDataTranslationCoverage(starts[index], sizes[index]);
        }
        const auto borrowedPageFaultSnapshot = owner.GetPageFaultEntrySnapshot();
        ASTERIA_EXPECT(
            runner, "the page-fault view exposes five aligned disjoint owner-backed ranges with fresh frame ownership",
            storage.Control != nullptr, storage.Context != nullptr, storage.Frame != nullptr,
            storage.CoordinatorMemoryStack != nullptr, storage.CoordinatorRseBackingStore != nullptr,
            storage.ControlAddress == AddressOf(storage.Control), storage.ContextAddress == AddressOf(storage.Context),
            storage.FrameAddress == AddressOf(storage.Frame),
            storage.CoordinatorMemoryStackAddress == AddressOf(storage.CoordinatorMemoryStack),
            storage.CoordinatorRseBackingStoreAddress == AddressOf(storage.CoordinatorRseBackingStore),
            storage.CoordinatorMemoryStackSizeBytes == Ia64PageFaultCoordinatorStackSizeBytes,
            storage.CoordinatorRseTransitionSizeBytes == Ia64PageFaultCoordinatorRseTransitionSizeBytes,
            storage.CoordinatorRseBackingStoreSizeBytes == Ia64PageFaultCoordinatorRseBackingStoreSizeBytes,
            (storage.ControlAddress & (alignof(Ia64PageFaultEntryControl) - 1U)) == 0U,
            (storage.ContextAddress & (alignof(Ia64PageFaultEntryContext) - 1U)) == 0U,
            (storage.FrameAddress & (PageBytes - 1U)) == 0U,
            (storage.CoordinatorMemoryStackAddress & (PageBytes - 1U)) == 0U,
            (storage.CoordinatorRseBackingStoreAddress & (PageBytes - 1U)) == 0U, allInside, allLocked,
            RangesArePairwiseDisjoint(starts, sizes), storage.Frame->State == Ia64PageFaultFrameState::Inactive,
            storage.Frame->PublishedToken == 0U, storage.Frame->NextToken == 0U,
            Ia64PageFaultFrameIsPristine(*storage.Frame),
            AcquireIa64PageFaultEntryControlSnapshot(*storage.Control).State == Ia64PageFaultEntryControlState::Fresh,
            PageFaultContextIsZero(*storage.Context),
            borrowedPageFaultSnapshot.State == Ia64BspRuntimePageFaultEntryState::Borrowed,
            borrowedPageFaultSnapshot.LastError == Ia64BspRuntimePageFaultEntryError::None);

        const auto externalPreemptionStorage = owner.BorrowExternalPreemptionStorage(bspProcessor);
        bool disjointFromExternalPreemption{false};
        if (externalPreemptionStorage.HasValue())
        {
            const auto external = externalPreemptionStorage.GetValue();
            const Foundation::UInt64 combinedStarts[8U]{
                starts[0U],
                starts[1U],
                starts[2U],
                starts[3U],
                starts[4U],
                AddressOf(external.Control),
                external.CoordinatorMemoryStackAddress,
                external.CoordinatorRseBackingStoreAddress,
            };
            const Foundation::UInt64 combinedSizes[8U]{
                sizes[0U],
                sizes[1U],
                sizes[2U],
                sizes[3U],
                sizes[4U],
                sizeof(*external.Control),
                external.CoordinatorMemoryStackSizeBytes,
                external.CoordinatorRseBackingStoreSizeBytes,
            };
            disjointFromExternalPreemption = RangesArePairwiseDisjoint(combinedStarts, combinedSizes);
        }
        ASTERIA_EXPECT(runner,
                       "page-fault storage remains disjoint from the separately owned external-preemption corridor",
                       externalPreemptionStorage.HasValue(), disjointFromExternalPreemption);

        const auto substitutedControl = owner.PublishPageFaultEntryContext(bspProcessor, g_foreignPageFaultControl,
                                                                           *storage.Context, *storage.Frame);
        const auto substitutedContext = owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control,
                                                                           g_foreignPageFaultContext, *storage.Frame);
        const auto substitutedFrame = owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control,
                                                                         *storage.Context, g_foreignPageFaultFrame);
        const auto unconfigured =
            owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control, *storage.Context, *storage.Frame);
        const auto afterUnconfigured = owner.GetPageFaultEntrySnapshot();
        ASTERIA_EXPECT(
            runner, "substituted or unconfigured page-fault storage is rejected without consuming the retryable borrow",
            substitutedControl == Ia64BspRuntimePageFaultEntryError::SubstitutedStorage,
            substitutedContext == Ia64BspRuntimePageFaultEntryError::SubstitutedStorage,
            substitutedFrame == Ia64BspRuntimePageFaultEntryError::SubstitutedStorage,
            unconfigured == Ia64BspRuntimePageFaultEntryError::ControlNotConfigured,
            afterUnconfigured.State == Ia64BspRuntimePageFaultEntryState::Borrowed,
            PageFaultContextIsZero(afterUnconfigured.Context), AcquirePageFaultPublication() == 0U);

        const Foundation::UInt64 interruptedRseStart = AddressOf(g_initialRseBackingStore);
        const Ia64PageFaultEntryControlError configurationError = ConfigureIa64PageFaultEntryControl(
            *storage.Control,
            Ia64PageFaultEntryControlConfiguration{
                .ProcessorIndex = bspProcessor.GetValue(),
                .FrameAddress = storage.FrameAddress,
                .InterruptedRseBackingStoreStart = interruptedRseStart,
                .InterruptedRseBackingStoreEnd = interruptedRseStart + InitialRseBackingStoreSizeBytes,
                .CoordinatorMemoryStackStart = storage.CoordinatorMemoryStackAddress,
                .CoordinatorMemoryStackEnd =
                    storage.CoordinatorMemoryStackAddress + storage.CoordinatorMemoryStackSizeBytes,
                .CoordinatorRseBackingStoreStart = storage.CoordinatorRseBackingStoreAddress,
                .CoordinatorRseBackingStoreEnd =
                    storage.CoordinatorRseBackingStoreAddress + storage.CoordinatorRseBackingStoreSizeBytes,
                .CoordinatorEntryPoint = ready.Summary.InterruptVectorTable.TableAddress,
                .CoordinatorGlobalPointer = 0U,
                .CoordinatorArgument = ownerAddress,
            });
        storage.Frame->NextToken = 2U;
        const auto staleFramePublication =
            owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control, *storage.Context, *storage.Frame);
        const auto afterStaleFramePublication = owner.GetPageFaultEntrySnapshot();
        const auto controlAfterStaleFramePublication = AcquireIa64PageFaultEntryControlSnapshot(*storage.Control);
        storage.Frame->NextToken = 0U;
        const bool restoredBorrowedFrameIsPristine = Ia64PageFaultFrameIsPristine(*storage.Frame);
        ASTERIA_EXPECT(
            runner, "a stale frame leaves an exact configured control immutable and repairable for publication retry",
            configurationError == Ia64PageFaultEntryControlError::None,
            staleFramePublication == Ia64BspRuntimePageFaultEntryError::FrameNotPristine,
            afterStaleFramePublication.State == Ia64BspRuntimePageFaultEntryState::Borrowed,
            afterStaleFramePublication.LastError == Ia64BspRuntimePageFaultEntryError::FrameNotPristine,
            controlAfterStaleFramePublication.State == Ia64PageFaultEntryControlState::Configured,
            PageFaultContextIsZero(afterStaleFramePublication.Context), restoredBorrowedFrameIsPristine,
            AcquirePageFaultPublication() == 0U);
        const auto pageFaultPublication =
            owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control, *storage.Context, *storage.Frame);
        const auto publishedPageFaultSnapshot = owner.GetPageFaultEntrySnapshot();
        const auto repeatedPageFaultPublication =
            owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control, *storage.Context, *storage.Frame);
        const auto afterRepeatedPageFaultPublication = owner.GetPageFaultEntrySnapshot();
        const auto controlSnapshot = AcquireIa64PageFaultEntryControlSnapshot(*storage.Control);
        ASTERIA_EXPECT(
            runner, "configured page-fault control publishes once with distinct control, frame, and resume generations",
            configurationError == Ia64PageFaultEntryControlError::None,
            pageFaultPublication == Ia64BspRuntimePageFaultEntryError::None,
            publishedPageFaultSnapshot.State == Ia64BspRuntimePageFaultEntryState::Published,
            publishedPageFaultSnapshot.LastError == Ia64BspRuntimePageFaultEntryError::None,
            publishedPageFaultSnapshot.LastInterruptVectorTableError == Ia64RuntimePageFaultPublicationError::None,
            publishedPageFaultSnapshot.ContextAddress == AcquirePageFaultPublication(),
            publishedPageFaultSnapshot.Context.ControlAddress == storage.ControlAddress,
            publishedPageFaultSnapshot.Context.ProcessorIndex == bspProcessor.GetValue(),
            publishedPageFaultSnapshot.Context.PageNotPresentVectorOffset == Ia64RuntimePageNotPresentOffset,
            publishedPageFaultSnapshot.Context.KeyPermissionVectorOffset == Ia64RuntimeKeyPermissionOffset,
            publishedPageFaultSnapshot.Context.InstructionAccessRightsVectorOffset ==
                Ia64RuntimeInstructionAccessRightsOffset,
            publishedPageFaultSnapshot.Context.DataAccessRightsVectorOffset == Ia64RuntimeDataAccessRightsOffset,
            publishedPageFaultSnapshot.Context.Reserved[0U] == 0U,
            publishedPageFaultSnapshot.Context.Reserved[1U] == 0U,
            controlSnapshot.Configuration.FrameAddress == storage.FrameAddress, controlSnapshot.EntryCaptureToken == 0U,
            controlSnapshot.CapturedFrameToken == 0U, controlSnapshot.ResumeGeneration == 0U,
            controlSnapshot.State == Ia64PageFaultEntryControlState::Configured,
            repeatedPageFaultPublication == Ia64BspRuntimePageFaultEntryError::AlreadyPublished,
            afterRepeatedPageFaultPublication.State == Ia64BspRuntimePageFaultEntryState::Published,
            AcquirePageFaultPublication() == publishedPageFaultSnapshot.ContextAddress);
    }

    const auto acknowledgementBeforeExternalPublication = owner.AcknowledgeExternalInterruptEvidence(1U);
    const auto invalidExternalProcessor = owner.PublishExternalInterruptEntryContext(CpuId{4U});
    const auto wrongExternalProcessor = owner.PublishExternalInterruptEntryContext(CpuId{1U});
    const auto wrongExternalSnapshot = owner.GetExternalInterruptPublicationSnapshot(CpuId{1U});
    const auto unpublishedBspSnapshot = owner.GetExternalInterruptPublicationSnapshot(bspProcessor);
    const Ia64BspRuntimeMemorySnapshot afterRejectedExternal = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "invalid and unprofiled external publication preserve the BSP slot, evidence, and IVT word",
                   invalidExternalProcessor == Ia64BspRuntimeExternalInterruptPublicationError::InvalidProcessor,
                   wrongExternalProcessor == Ia64BspRuntimeExternalInterruptPublicationError::ProcessorMismatch,
                   !afterRejectedExternal.ExternalInterruptPublication.Published,
                   ExternalContextIsZero(afterRejectedExternal.ExternalInterruptPublication.Context),
                   ExternalEvidenceIsZero(afterRejectedExternal.ExternalInterruptPublication.Evidence),
                   AcquireExternalInterruptPublication() == 0U,
                   afterRejectedExternal.ExternalInterruptPublication.LastError ==
                       Ia64BspRuntimeExternalInterruptPublicationError::NotReady,
                   afterRejectedExternal.ExternalInterruptPublication.LastInterruptVectorTableError ==
                       Ia64RuntimeExternalInterruptPublicationError::None,
                   acknowledgementBeforeExternalPublication ==
                       Ia64BspRuntimeExternalInterruptAcknowledgementError::NotPublished,
                   afterRejectedExternal.ExternalInterruptPublication.LastAcknowledgementError ==
                       Ia64BspRuntimeExternalInterruptAcknowledgementError::NotPublished,
                   !wrongExternalSnapshot.HasValue(),
                   wrongExternalSnapshot.GetError() == Ia64BspRuntimeExternalInterruptAccessError::NotProfiled,
                   !unpublishedBspSnapshot.HasValue(),
                   unpublishedBspSnapshot.GetError() == Ia64BspRuntimeExternalInterruptAccessError::NotPublished);

    const auto externalPublished = owner.PublishExternalInterruptEntryContext(bspProcessor);
    const Ia64BspRuntimeMemorySnapshot externalPublication = owner.GetSnapshot();
    const auto typedExternalPublication = owner.GetExternalInterruptPublicationSnapshot(bspProcessor);
    const auto publishedExternalEvidenceView = owner.GetPublishedExternalInterruptEvidence(bspProcessor);
    auto* const retainedExternalEvidence = reinterpret_cast<Ia64LocalTimerInterruptEvidence*>(
        static_cast<Foundation::UIntPointer>(externalPublication.ExternalInterruptPublication.EvidenceAddress));
    retainedExternalEvidence->ProgrammedDeadlineTicks = 0x1111'2222'3333'4444ULL;
    retainedExternalEvidence->ObservedIntervalTimeCounter = 0x5555'6666'7777'8888ULL;
    retainedExternalEvidence->InterruptedInstructionPointer = 0x0000'0000'1234'5670ULL;
    retainedExternalEvidence->InterruptedProcessorStatus = 0x9999'AAAA'BBBB'CCCCULL;
    retainedExternalEvidence->InterruptedIfsValid = 0xDDDD'EEEE'FFFF'0001ULL;
    retainedExternalEvidence->ProcessorIndex = bspProcessor.GetValue();
    retainedExternalEvidence->Vector = Ia64ExternalInterruptTimerVector;
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        retainedExternalEvidence->Reserved[index] = static_cast<Foundation::UInt8>(0xA0U + index);
    }
    AtomicUInt64Reference evidencePublication{retainedExternalEvidence->Sequence};
    evidencePublication.StoreRelease(1U);
    const auto directExternalPublication = owner.GetExternalInterruptPublicationSnapshot();
    const auto externalAcknowledged = owner.AcknowledgeExternalInterruptEvidence(1U);
    const auto afterExternalAcknowledgement = owner.GetExternalInterruptPublicationSnapshot();
    const auto externalDoubleAcknowledgement = owner.AcknowledgeExternalInterruptEvidence(1U);
    const auto afterExternalDoubleAcknowledgement = owner.GetExternalInterruptPublicationSnapshot();
    retainedExternalEvidence->ProgrammedDeadlineTicks = 0x2222'3333'4444'5555ULL;
    retainedExternalEvidence->ObservedIntervalTimeCounter = 0x6666'7777'8888'9999ULL;
    retainedExternalEvidence->InterruptedInstructionPointer = 0x0000'0000'2345'6780ULL;
    retainedExternalEvidence->InterruptedProcessorStatus = 0xAAAA'BBBB'CCCC'DDDDULL;
    retainedExternalEvidence->InterruptedIfsValid = 0xEEEE'FFFF'0001'1112ULL;
    retainedExternalEvidence->ProcessorIndex = bspProcessor.GetValue();
    retainedExternalEvidence->Vector = Ia64ExternalInterruptTimerVector;
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        retainedExternalEvidence->Reserved[index] = static_cast<Foundation::UInt8>(0xB0U + index);
    }
    evidencePublication.StoreRelease(2U);
    const auto generationTwoPublication = owner.GetExternalInterruptPublicationSnapshot();
    const auto generationTwoAcknowledged = owner.AcknowledgeExternalInterruptEvidence(2U);
    const auto afterGenerationTwoAcknowledgement = owner.GetExternalInterruptPublicationSnapshot();
    const auto externalRepeated = owner.PublishExternalInterruptEntryContext(bspProcessor);
    const Ia64BspRuntimeMemorySnapshot afterExternalRepeat = owner.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "one BSP external context names exact zero evidence and release-publishes at ar.k3 offset 24",
        externalPublished == Ia64BspRuntimeExternalInterruptPublicationError::None, typedExternalPublication.HasValue(),
        publishedExternalEvidenceView.HasValue(),
        publishedExternalEvidenceView.GetValue() ==
            reinterpret_cast<const Ia64LocalTimerInterruptEvidence*>(
                static_cast<Foundation::UIntPointer>(externalPublication.ExternalInterruptPublication.EvidenceAddress)),
        externalPublication.ExternalInterruptPublication.Published,
        externalPublication.ExternalInterruptPublication.LastError ==
            Ia64BspRuntimeExternalInterruptPublicationError::None,
        externalPublication.ExternalInterruptPublication.LastInterruptVectorTableError ==
            Ia64RuntimeExternalInterruptPublicationError::None,
        externalPublication.ExternalInterruptPublication.LastAcknowledgementError ==
            Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        externalPublication.ExternalInterruptPublication.ContextAddress == AcquireExternalInterruptPublication(),
        externalPublication.ExternalInterruptPublication.Context.EvidenceAddress ==
            externalPublication.ExternalInterruptPublication.EvidenceAddress,
        externalPublication.ExternalInterruptPublication.Context.ProcessorIndex == bspProcessor.GetValue(),
        externalPublication.ExternalInterruptPublication.Context.TimerVector == Ia64ExternalInterruptTimerVector,
        externalPublication.ExternalInterruptPublication.Context.ExternalPreemptionControlAddress == 0U,
        ExternalEvidenceIsZero(externalPublication.ExternalInterruptPublication.Evidence));
    ASTERIA_EXPECT(
        runner, "dedicated external getter acquire-observes every completed evidence field",
        directExternalPublication.Published,
        directExternalPublication.ContextAddress == externalPublication.ExternalInterruptPublication.ContextAddress,
        directExternalPublication.EvidenceAddress == externalPublication.ExternalInterruptPublication.EvidenceAddress,
        directExternalPublication.Context.EvidenceAddress == directExternalPublication.EvidenceAddress,
        directExternalPublication.Context.ProcessorIndex == bspProcessor.GetValue(),
        directExternalPublication.Context.TimerVector == Ia64ExternalInterruptTimerVector,
        directExternalPublication.Context.ExternalPreemptionControlAddress == 0U,
        directExternalPublication.LastError == Ia64BspRuntimeExternalInterruptPublicationError::None,
        directExternalPublication.LastInterruptVectorTableError == Ia64RuntimeExternalInterruptPublicationError::None,
        directExternalPublication.LastAcknowledgementError == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        directExternalPublication.Reserved[0U] == 0U, directExternalPublication.Reserved[1U] == 0U,
        directExternalPublication.Reserved[2U] == 0U, directExternalPublication.Reserved[3U] == 0U,
        directExternalPublication.Evidence.Sequence == 1U,
        directExternalPublication.Evidence.ProgrammedDeadlineTicks == 0x1111'2222'3333'4444ULL,
        directExternalPublication.Evidence.ObservedIntervalTimeCounter == 0x5555'6666'7777'8888ULL,
        directExternalPublication.Evidence.InterruptedInstructionPointer == 0x0000'0000'1234'5670ULL,
        directExternalPublication.Evidence.InterruptedProcessorStatus == 0x9999'AAAA'BBBB'CCCCULL,
        directExternalPublication.Evidence.InterruptedIfsValid == 0xDDDD'EEEE'FFFF'0001ULL,
        directExternalPublication.Evidence.ProcessorIndex == bspProcessor.GetValue(),
        directExternalPublication.Evidence.Vector == Ia64ExternalInterruptTimerVector,
        directExternalPublication.Evidence.Reserved[0U] == 0xA0U,
        directExternalPublication.Evidence.Reserved[1U] == 0xA1U,
        directExternalPublication.Evidence.Reserved[2U] == 0xA2U,
        directExternalPublication.Evidence.Reserved[3U] == 0xA3U,
        directExternalPublication.Evidence.Reserved[4U] == 0xA4U,
        directExternalPublication.Evidence.Reserved[5U] == 0xA5U,
        directExternalPublication.Evidence.AcknowledgedSequence == 0U);
    ASTERIA_EXPECT(
        runner,
        "normal context release-acknowledges the exact retained generation and diagnoses a double acknowledgement",
        externalAcknowledged == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        afterExternalAcknowledgement.LastAcknowledgementError ==
            Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        afterExternalAcknowledgement.Evidence.Sequence == 1U,
        afterExternalAcknowledgement.Evidence.AcknowledgedSequence == 1U,
        afterExternalAcknowledgement.Evidence.ProgrammedDeadlineTicks == 0U,
        externalDoubleAcknowledgement == Ia64BspRuntimeExternalInterruptAcknowledgementError::AlreadyAcknowledged,
        afterExternalDoubleAcknowledgement.LastAcknowledgementError ==
            Ia64BspRuntimeExternalInterruptAcknowledgementError::AlreadyAcknowledged,
        afterExternalDoubleAcknowledgement.Evidence.Sequence == 1U,
        afterExternalDoubleAcknowledgement.Evidence.AcknowledgedSequence == 1U);
    ASTERIA_EXPECT(
        runner,
        "the runtime snapshot acquire-observes and release-acknowledges generation two without exposing stale payload",
        generationTwoPublication.Evidence.Sequence == 2U, generationTwoPublication.Evidence.AcknowledgedSequence == 1U,
        generationTwoPublication.Evidence.ProgrammedDeadlineTicks == 0x2222'3333'4444'5555ULL,
        generationTwoPublication.Evidence.ObservedIntervalTimeCounter == 0x6666'7777'8888'9999ULL,
        generationTwoPublication.Evidence.InterruptedInstructionPointer == 0x0000'0000'2345'6780ULL,
        generationTwoPublication.Evidence.InterruptedProcessorStatus == 0xAAAA'BBBB'CCCC'DDDDULL,
        generationTwoPublication.Evidence.InterruptedIfsValid == 0xEEEE'FFFF'0001'1112ULL,
        generationTwoPublication.Evidence.ProcessorIndex == bspProcessor.GetValue(),
        generationTwoPublication.Evidence.Vector == Ia64ExternalInterruptTimerVector,
        generationTwoPublication.Evidence.Reserved[0U] == 0xB0U,
        generationTwoPublication.Evidence.Reserved[5U] == 0xB5U,
        generationTwoAcknowledged == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        afterGenerationTwoAcknowledgement.LastAcknowledgementError ==
            Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
        afterGenerationTwoAcknowledgement.Evidence.Sequence == 2U,
        afterGenerationTwoAcknowledgement.Evidence.AcknowledgedSequence == 2U,
        afterGenerationTwoAcknowledgement.Evidence.ProgrammedDeadlineTicks == 0U);
    ASTERIA_EXPECT(runner, "the BSP external slot retains aligned context and evidence inside the runtime corridor",
                   RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart, ready.Summary.RuntimeObjects.SizeBytes,
                                 externalPublication.ExternalInterruptPublication.ContextAddress,
                                 sizeof(Ia64ExternalInterruptEntryContext)),
                   RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart, ready.Summary.RuntimeObjects.SizeBytes,
                                 externalPublication.ExternalInterruptPublication.EvidenceAddress,
                                 sizeof(Ia64LocalTimerInterruptEvidence)),
                   (externalPublication.ExternalInterruptPublication.ContextAddress & 15U) == 0U,
                   (externalPublication.ExternalInterruptPublication.EvidenceAddress & 7U) == 0U,
                   externalPublication.ExternalInterruptPublication.ContextAddress <
                       externalPublication.ExternalInterruptPublication.EvidenceAddress,
                   externalPublication.ExternalInterruptPublication.EvidenceAddress +
                           sizeof(Ia64LocalTimerInterruptEvidence) <=
                       ready.Summary.RuntimeObjects.VirtualStart + ready.Summary.RuntimeObjects.SizeBytes,
                   externalPublication.ExternalInterruptPublication.EvidenceAddress ==
                       externalPublication.ExternalInterruptPublication.ContextAddress +
                           offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence));
    ASTERIA_EXPECT(runner, "repeated external publication preserves the one retained context and IVT word",
                   externalRepeated == Ia64BspRuntimeExternalInterruptPublicationError::AlreadyPublished,
                   afterExternalRepeat.ExternalInterruptPublication.Published,
                   afterExternalRepeat.ExternalInterruptPublication.LastError ==
                       Ia64BspRuntimeExternalInterruptPublicationError::AlreadyPublished,
                   afterExternalRepeat.ExternalInterruptPublication.ContextAddress ==
                       externalPublication.ExternalInterruptPublication.ContextAddress,
                   AcquireExternalInterruptPublication() ==
                       externalPublication.ExternalInterruptPublication.ContextAddress);

    const Ia64BspRuntimeMemorySnapshot beforePublication = owner.GetSnapshot();
    const auto invalidProcessorPublication =
        owner.PublishSystemCallEntryContext(CpuId{4U}, AddressOf(&g_alignedBridgeContextFixture));
    const auto wrongProcessorPublication =
        owner.PublishSystemCallEntryContext(CpuId{1U}, AddressOf(&g_alignedBridgeContextFixture));
    const auto zeroBridgeContextPublication = owner.PublishSystemCallEntryContext(bspProcessor, 0U);
    const auto misalignedBridgeContextPublication =
        owner.PublishSystemCallEntryContext(bspProcessor, AddressOf(&g_alignedBridgeContextFixture) + 1U);
    const auto nonRegionZeroBridgeContextPublication =
        owner.PublishSystemCallEntryContext(bspProcessor, Foundation::UInt64{1U} << 61U);
    const auto crossingRegionZeroBridgeContextPublication =
        owner.PublishSystemCallEntryContext(bspProcessor, (Foundation::UInt64{1U} << 61U) - 16U);
    const Ia64BspRuntimeMemorySnapshot afterRejectedPublications = owner.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "invalid CPU and bridge-context inputs retain zero transport storage and zero IVT publication",
        invalidProcessorPublication == Ia64BspRuntimeSystemCallPublicationError::InvalidProcessor,
        wrongProcessorPublication == Ia64BspRuntimeSystemCallPublicationError::ProcessorMismatch,
        zeroBridgeContextPublication == Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress,
        misalignedBridgeContextPublication == Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress,
        nonRegionZeroBridgeContextPublication == Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress,
        crossingRegionZeroBridgeContextPublication ==
            Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress,
        !afterRejectedPublications.SystemCallPublication.Published,
        ContextIsZero(beforePublication.SystemCallPublication.Context),
        ContextIsZero(afterRejectedPublications.SystemCallPublication.Context), AcquireSystemCallPublication() == 0U,
        afterRejectedPublications.SystemCallPublication.LastError ==
            Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress);

    const Foundation::UInt64 bridgeContextAddress{AddressOf(&g_alignedBridgeContextFixture)};
    const auto published = owner.PublishSystemCallEntryContext(bspProcessor, bridgeContextAddress);
    const Ia64BspRuntimeMemorySnapshot publication = owner.GetSnapshot();
    const Ia64SystemCallEntryContext publishedContext = publication.SystemCallPublication.Context;
    const auto* const frame = reinterpret_cast<const Ia64SystemCallEntryFrame*>(
        static_cast<Foundation::UIntPointer>(publishedContext.FrameAddress));
    ASTERIA_EXPECT(
        runner, "one valid BSP call release-publishes the exact validated immutable context",
        published == Ia64BspRuntimeSystemCallPublicationError::None, publication.SystemCallPublication.Published,
        publication.SystemCallPublication.LastError == Ia64BspRuntimeSystemCallPublicationError::None,
        publication.SystemCallPublication.LastContextValidationError == Ia64SystemCallContextValidationError::None,
        publication.SystemCallPublication.LastInterruptVectorTableError ==
            Ia64RuntimeInterruptVectorTablePublicationError::None,
        publication.SystemCallPublication.ContextAddress == AcquireSystemCallPublication(),
        publishedContext.BridgeContextAddress == bridgeContextAddress,
        publishedContext.ProcessorIndex == bspProcessor.GetValue(), publishedContext.Reserved == 0U,
        ValidateIa64SystemCallTrustedContext(publishedContext) == Ia64SystemCallContextValidationError::None);
    ASTERIA_EXPECT(
        runner,
        "context, exact frame, page-aligned stack, and transition-capable RSE store stay in the retained corridor",
        (publication.SystemCallPublication.ContextAddress & 15U) == 0U,
        RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart, ready.Summary.RuntimeObjects.SizeBytes,
                      publication.SystemCallPublication.ContextAddress, sizeof(Ia64SystemCallEntryContext)),
        RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart, ready.Summary.RuntimeObjects.SizeBytes,
                      publishedContext.FrameAddress, sizeof(Ia64SystemCallEntryFrame)),
        RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart, ready.Summary.RuntimeObjects.SizeBytes,
                      publishedContext.MemoryStackStart, Ia64SystemCallMemoryStackSizeBytes),
        RangeIsInside(ready.Summary.RuntimeObjects.VirtualStart, ready.Summary.RuntimeObjects.SizeBytes,
                      publishedContext.RseBackingStoreStart, Ia64SystemCallRseBackingStoreSizeBytes),
        (publishedContext.MemoryStackStart & (PageBytes - 1U)) == 0U,
        (publishedContext.RseBackingStoreStart & (PageBytes - 1U)) == 0U,
        (publishedContext.MemoryStackEnd - publishedContext.MemoryStackStart) == Ia64SystemCallMemoryStackSizeBytes,
        (publishedContext.RseBackingStoreEnd - publishedContext.RseBackingStoreStart) ==
            Ia64SystemCallRseBackingStoreSizeBytes,
        !RangesOverlap(publishedContext.MemoryStackStart, publishedContext.MemoryStackEnd,
                       publishedContext.RseBackingStoreStart, publishedContext.RseBackingStoreEnd),
        frame != nullptr, frame->State == Ia64SystemCallEntryFrameState::Inactive, frame->Reserved == 0U);
    if (pageFaultStorage.HasValue())
    {
        const auto pageFault = pageFaultStorage.GetValue();
        const Foundation::UInt64 combinedStarts[9U]{
            pageFault.ContextAddress,
            pageFault.ControlAddress,
            pageFault.FrameAddress,
            pageFault.CoordinatorMemoryStackAddress,
            pageFault.CoordinatorRseBackingStoreAddress,
            publication.SystemCallPublication.ContextAddress,
            publishedContext.FrameAddress,
            publishedContext.MemoryStackStart,
            publishedContext.RseBackingStoreStart,
        };
        const Foundation::UInt64 combinedSizes[9U]{
            sizeof(Ia64PageFaultEntryContext),
            sizeof(Ia64PageFaultEntryControl),
            sizeof(Ia64PageFaultFrame),
            pageFault.CoordinatorMemoryStackSizeBytes,
            pageFault.CoordinatorRseBackingStoreSizeBytes,
            sizeof(Ia64SystemCallEntryContext),
            sizeof(Ia64SystemCallEntryFrame),
            Ia64SystemCallMemoryStackSizeBytes,
            Ia64SystemCallRseBackingStoreSizeBytes,
        };
        ASTERIA_EXPECT(
            runner,
            "page-fault context, control, frame, stack, and RSE storage remain disjoint from system-call ownership",
            RangesArePairwiseDisjoint(combinedStarts, combinedSizes));
    }

    const auto invalidAfterSuccess = owner.PublishSystemCallEntryContext(bspProcessor, bridgeContextAddress + 1U);
    const auto repeatedPublication = owner.PublishSystemCallEntryContext(bspProcessor, bridgeContextAddress);
    const Ia64BspRuntimeMemorySnapshot afterRepeatedPublication = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "invalid and repeated calls after success preserve the one exact publication",
                   invalidAfterSuccess == Ia64BspRuntimeSystemCallPublicationError::InvalidBridgeContextAddress,
                   repeatedPublication == Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished,
                   afterRepeatedPublication.SystemCallPublication.Published,
                   afterRepeatedPublication.SystemCallPublication.LastError ==
                       Ia64BspRuntimeSystemCallPublicationError::AlreadyPublished,
                   std::memcmp(&publication.SystemCallPublication.Context,
                               &afterRepeatedPublication.SystemCallPublication.Context,
                               sizeof(Ia64SystemCallEntryContext)) == 0,
                   AcquireSystemCallPublication() == publication.SystemCallPublication.ContextAddress);

    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto repeated =
        owner.PrepareAndAdmit(bspProcessor, bspCapabilities, g_preparation, chargeOwner, g_registry, g_issuer);
    const bool readyOwnerUnchanged =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    const Ia64BspRuntimeMemorySnapshot afterRepeat = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "compatibility repeat after success changes no owner byte or retained work",
                   !repeated.HasValue(), repeated.GetError() == Ia64BspRuntimeMemoryError::AlreadyPrepared,
                   g_registry.GetActiveCount() == 3U, g_installCallCount == 1U, g_activationCallCount == 1U,
                   g_registryCountAtInstall == 0U, g_registryCountAtActivation == 1U, g_nativeSequence == 2U,
                   readyOwnerUnchanged, AllocatorHasThreeExactReceipts(afterRepeat, 18U),
                   afterRepeat.Summary.ExactLedgerVerified);
}

void TestNonzeroBspApplicationProcessorRootBorrows(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{2U};
    constexpr CpuId lowerApplicationProcessor{0U};
    constexpr CpuId upperApplicationProcessor{3U};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[0U];

    ResetNative(AdapterSuccess);
    const Foundation::Size registryCountBefore = g_registry.GetActiveCount();
    const auto admittedLower = owner.AdmitApplicationProcessorPhysicalRoot(lowerApplicationProcessor);
    const auto borrowedLower = owner.BorrowReadyApplicationProcessorRoot(lowerApplicationProcessor);
    const auto admittedUpper = owner.AdmitApplicationProcessorPhysicalRoot(upperApplicationProcessor);
    const auto borrowedLowerAgain = owner.BorrowReadyApplicationProcessorRoot(lowerApplicationProcessor);
    const auto borrowedUpper = owner.BorrowReadyApplicationProcessorRoot(upperApplicationProcessor);
    const Ia64BspRuntimeMemorySnapshot complete = owner.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "successful AP admissions span both numeric sides of a nonzero BSP", admittedLower.HasValue(),
        admittedUpper.HasValue(), borrowedLower.HasValue(), borrowedLowerAgain.HasValue(), borrowedUpper.HasValue(),
        admittedLower.GetValue().Processor.GetValue() == lowerApplicationProcessor.GetValue(),
        admittedLower.GetValue().ResidentProcessorMask == 0x05U,
        admittedLower.GetValue().InstalledProcessorMask == 0x05U,
        admittedUpper.GetValue().Processor.GetValue() == upperApplicationProcessor.GetValue(),
        admittedUpper.GetValue().ResidentProcessorMask == 0x0DU,
        admittedUpper.GetValue().InstalledProcessorMask == 0x0DU, complete.State == Ia64BspRuntimeMemoryState::Ready,
        complete.Summary.BspProcessorIndex == bspProcessor.GetValue(), complete.Summary.ProfiledProcessorMask == 0x0DU,
        complete.Summary.ResidentProcessorMask == 0x0DU, complete.AddressSpace.ResidentProcessorMask == 0x0DU,
        complete.Summary.InterruptVectorTable.InstalledProcessorMask == 0x0DU,
        complete.Summary.InterruptVectorTable.ProcessorIndex == upperApplicationProcessor.GetValue(),
        complete.Bindings[0U] == Ia64BspRuntimeBindingState::Active,
        complete.Bindings[1U] == Ia64BspRuntimeBindingState::Unadmitted,
        complete.Bindings[2U] == Ia64BspRuntimeBindingState::Active,
        complete.Bindings[3U] == Ia64BspRuntimeBindingState::Active);
    ASTERIA_EXPECT(runner, "nonzero-BSP AP borrows select stable dedicated controller indexes on both sides",
                   borrowedLower.HasValue(), borrowedLowerAgain.HasValue(), borrowedUpper.HasValue(),
                   borrowedLower.GetValue().AddressSpaceStorage == complete.AddressSpaceStorage,
                   borrowedUpper.GetValue().AddressSpaceStorage == complete.AddressSpaceStorage,
                   borrowedLower.GetValue().AddressSpaceIdentity.IsEqualTo(complete.Summary.AddressSpaceIdentity),
                   borrowedUpper.GetValue().AddressSpaceIdentity.IsEqualTo(complete.Summary.AddressSpaceIdentity),
                   borrowedLower.GetValue().Processor.GetValue() == lowerApplicationProcessor.GetValue(),
                   borrowedUpper.GetValue().Processor.GetValue() == upperApplicationProcessor.GetValue(),
                   borrowedLower.GetValue().ControllerStorage == borrowedLowerAgain.GetValue().ControllerStorage,
                   borrowedLower.GetValue().ControllerStorage != borrowedUpper.GetValue().ControllerStorage,
                   AddressOf(borrowedUpper.GetValue().ControllerStorage) ==
                       (AddressOf(borrowedLower.GetValue().ControllerStorage) +
                        (2U * sizeof(Kernel::Memory::ProcessorAddressSpaceController))),
                   borrowedLower.GetValue().ControllerStorage->GetSnapshot().Processor.GetValue() ==
                       lowerApplicationProcessor.GetValue(),
                   borrowedUpper.GetValue().ControllerStorage->GetSnapshot().Processor.GetValue() ==
                       upperApplicationProcessor.GetValue(),
                   g_registry.GetActiveCount() == registryCountBefore, g_installCallCount == 2U,
                   g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));
}

void TestPostPreflightCapabilityFailure(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{3U};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[1U];
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    processorCapabilities[1U] = Capabilities(18U);
    processorCapabilities[1U].Translation.MemoryAttributeMask = 0x01U;
    processorCapabilities[bspProcessor.GetValue()] = Capabilities(24U);
    processorCapabilities[bspProcessor.GetValue()].Translation.MemoryAttributeMask = 0x10U;
    constexpr Foundation::UInt8 profiledProcessorMask{0x0AU};
    ResetNative(AdapterSuccess);
    const auto rejected =
        owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities, g_preparation,
                              ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    const auto failedBorrow = owner.BorrowReadyRoot();
    ASTERIA_EXPECT(
        runner, "an incompatible full processor profile is terminal before RID or native runtime-root writes",
        !rejected.HasValue(), rejected.GetError() == Ia64BspRuntimeMemoryError::ProcessorCapabilitiesRejected,
        failed.State == Ia64BspRuntimeMemoryState::Failed,
        failed.Failure.Error == Ia64BspRuntimeMemoryError::ProcessorCapabilitiesRejected,
        failed.Failure.ProcessorCapabilitiesError ==
            Ia64RuntimeProcessorCapabilitiesError::IncompatibleCommonCapabilities,
        failed.RegionIdentifierAllocator.State == Ia64RegionIdentifierAllocatorState::Uninitialized,
        failed.RegionIdentifierAllocator.NextRegionIdentifier == 0U,
        failed.RegionIdentifierAllocator.AllocatedAddressSpaceCount == 0U,
        failed.AddressSpace.State == AddressSpaceState::Uninitialized,
        AllBindingsAre(failed, Ia64BspRuntimeBindingState::Unadmitted), g_registry.GetActiveCount() == 3U,
        g_installCallCount == 0U, g_activationCallCount == 0U, g_registryCountAtInstall == UnobservedRegistryCount,
        g_registryCountAtActivation == UnobservedRegistryCount, !failedBorrow.HasValue(),
        failedBorrow.GetError() == Ia64BspRuntimeRootBorrowError::NotReady);

    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto repeated = owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation,
                                                ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const bool failedOwnerUnchanged =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    const Ia64BspRuntimeMemorySnapshot afterRepeat = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "compatibility repeat after capability failure changes no owner byte or delayed work",
                   !repeated.HasValue(), repeated.GetError() == Ia64BspRuntimeMemoryError::PreviousPreparationFailed,
                   g_registry.GetActiveCount() == 3U, g_installCallCount == 0U, g_activationCallCount == 0U,
                   g_registryCountAtInstall == UnobservedRegistryCount,
                   g_registryCountAtActivation == UnobservedRegistryCount, failedOwnerUnchanged,
                   afterRepeat.RegionIdentifierAllocator.State == Ia64RegionIdentifierAllocatorState::Uninitialized);
}

void TestApplicationProcessorPhysicalRootAdmissions(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{0U};
    constexpr Foundation::UInt8 profiledProcessorMask{0x0FU};
    const ResourceDomainId chargeOwner{ChargeOwnerValue + 1U};
    Ia64BspRuntimeMemory& previousOwner = g_arena.Owners[0U];
    Ia64BspRuntimeMemory& owner = g_arena.Owners[2U];
    ResetNative(AdapterSuccess);
    const auto unprofiled = previousOwner.AdmitApplicationProcessorPhysicalRoot(CpuId{1U});
    ASTERIA_EXPECT(runner, "an unprofiled AP is rejected before any native effect", !unprofiled.HasValue(),
                   unprofiled.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::ProcessorNotProfiled,
                   g_installCallCount == 0U, g_activationCallCount == 0U, g_nativeSequence == 0U);

    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        processorCapabilities[processor] = Capabilities();
    }
    const Foundation::Size registryCountBefore = g_registry.GetActiveCount();
    const auto prepared = owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities,
                                                g_preparation, chargeOwner, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot bspReady = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "the full-profile AP fixture begins with only CPU 0 resident and one BSP native pair",
                   prepared.HasValue(), bspReady.State == Ia64BspRuntimeMemoryState::Ready,
                   bspReady.Summary.ProfiledProcessorMask == profiledProcessorMask,
                   bspReady.Summary.ResidentProcessorMask == 0x01U,
                   bspReady.AddressSpace.ResidentProcessorMask == 0x01U,
                   OnlyBspBindingIs(bspReady, bspProcessor, Ia64BspRuntimeBindingState::Active),
                   g_registry.GetActiveCount() == (registryCountBefore + 3U), g_installCallCount == 1U,
                   g_activationCallCount == 1U, NativeEventsAlternateExactly(1U));
    if (!prepared.HasValue())
    {
        return;
    }

    const auto profiledUnadmittedPublication = owner.PublishExternalInterruptEntryContext(CpuId{1U});
    const auto profiledUnadmittedSnapshot = owner.GetExternalInterruptPublicationSnapshot(CpuId{1U});
    const auto profiledUnadmittedView = owner.GetPublishedExternalInterruptEvidence(CpuId{1U});
    const auto profiledUnadmittedAcknowledgement = owner.AcknowledgeExternalInterruptEvidence(CpuId{1U}, 1U);
    const auto profiledUnadmittedRoot = owner.BorrowReadyApplicationProcessorRoot(CpuId{1U});
    ASTERIA_EXPECT(
        runner, "a profiled but unadmitted CPU changes only its own unavailable transport diagnostics",
        profiledUnadmittedPublication == Ia64BspRuntimeExternalInterruptPublicationError::ProcessorMismatch,
        !profiledUnadmittedSnapshot.HasValue(),
        profiledUnadmittedSnapshot.GetError() == Ia64BspRuntimeExternalInterruptAccessError::NotResident,
        !profiledUnadmittedView.HasValue(),
        profiledUnadmittedView.GetError() == Ia64BspRuntimeExternalInterruptAccessError::NotResident,
        profiledUnadmittedAcknowledgement == Ia64BspRuntimeExternalInterruptAcknowledgementError::NotResident,
        !profiledUnadmittedRoot.HasValue(),
        profiledUnadmittedRoot.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::NotReady,
        !bspReady.ExternalInterruptPublication.Published,
        bspReady.ExternalInterruptPublication.LastError == Ia64BspRuntimeExternalInterruptPublicationError::None);

    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto invalid = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{4U});
    const auto foreignBsp = owner.AdmitApplicationProcessorPhysicalRoot(bspProcessor);
    const auto outOfOrderTwo = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{2U});
    const auto outOfOrderThree = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{3U});
    const bool validationPreservedOwner =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    ASTERIA_EXPECT(
        runner, "invalid, BSP-foreign, and out-of-order AP calls preserve every owner byte and native count",
        !invalid.HasValue(), invalid.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::InvalidProcessor,
        !foreignBsp.HasValue(), foreignBsp.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::BspProcessor,
        !outOfOrderTwo.HasValue(),
        outOfOrderTwo.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::OutOfOrder,
        !outOfOrderThree.HasValue(),
        outOfOrderThree.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::OutOfOrder,
        validationPreservedOwner, g_installCallCount == 1U, g_activationCallCount == 1U,
        NativeEventsAlternateExactly(1U));

    const Foundation::Size nativeBeforeOne = g_nativeSequence;
    const auto admittedOne = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{1U});
    const auto publishedOne = owner.PublishExternalInterruptEntryContext(CpuId{1U});
    const auto publishedOneSnapshot = owner.GetExternalInterruptPublicationSnapshot(CpuId{1U});
    const Ia64BspRuntimeMemorySnapshot afterOne = owner.GetSnapshot();
    const auto oneSnapshot = owner.GetApplicationProcessorAdmissionSnapshot(CpuId{1U});
    const auto borrowedOne = owner.BorrowReadyApplicationProcessorRoot(CpuId{1U});
    ASTERIA_EXPECT(
        runner, "CPU 1 admission immediately enables its isolated external-interrupt publication",
        admittedOne.HasValue(), publishedOne == Ia64BspRuntimeExternalInterruptPublicationError::None,
        publishedOneSnapshot.HasValue(), publishedOneSnapshot.GetValue().Context.ProcessorIndex == 1U,
        oneSnapshot.HasValue(), borrowedOne.HasValue(),
        oneSnapshot.GetValue().State == Ia64BspRuntimeApplicationProcessorAdmissionState::Ready,
        oneSnapshot.GetValue().Failure.Error == Ia64BspRuntimeApplicationProcessorAdmissionError::None,
        admittedOne.GetValue().Processor.GetValue() == 1U, admittedOne.GetValue().ResidentProcessorMask == 0x03U,
        admittedOne.GetValue().InstalledProcessorMask == 0x03U,
        admittedOne.GetValue().PhysicalBootstrapActivationPerformed,
        admittedOne.GetValue().ControllerAcknowledgementPerformed, admittedOne.GetValue().ExactLedgerVerified,
        admittedOne.GetValue().ControllerState == ProcessorAddressSpaceControllerState::Ready,
        afterOne.AddressSpace.ResidentProcessorMask == 0x03U, afterOne.Summary.ResidentProcessorMask == 0x03U,
        afterOne.Summary.InterruptVectorTable.InstalledProcessorMask == 0x03U,
        afterOne.Summary.InterruptVectorTable.ProcessorIndex == 1U,
        afterOne.Bindings[0U] == Ia64BspRuntimeBindingState::Active,
        afterOne.Bindings[1U] == Ia64BspRuntimeBindingState::Active,
        afterOne.Bindings[2U] == Ia64BspRuntimeBindingState::Unadmitted,
        afterOne.Bindings[3U] == Ia64BspRuntimeBindingState::Unadmitted, g_nativeSequence == (nativeBeforeOne + 2U),
        g_installCallCount == 2U, g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));
    if (!admittedOne.HasValue() || !oneSnapshot.HasValue() || !borrowedOne.HasValue())
    {
        return;
    }
    ASTERIA_EXPECT(runner, "CPU 1 owns a dedicated Ready controller on the exact already-claimed root",
                   borrowedOne.HasValue(), borrowedOne.GetValue().AddressSpaceStorage == afterOne.AddressSpaceStorage,
                   borrowedOne.GetValue().AddressSpaceIdentity.IsEqualTo(afterOne.Summary.AddressSpaceIdentity),
                   borrowedOne.GetValue().Processor.GetValue() == 1U,
                   borrowedOne.GetValue().ControllerStorage != nullptr,
                   borrowedOne.GetValue().ControllerStorage->GetSnapshot().Processor.GetValue() == 1U,
                   oneSnapshot.GetValue().Controller.IdleStorage == afterOne.AddressSpaceStorage,
                   oneSnapshot.GetValue().Controller.ActiveStorage == afterOne.AddressSpaceStorage,
                   oneSnapshot.GetValue().Controller.PreviousStorage == nullptr,
                   oneSnapshot.GetValue().Controller.Active.IsEqualTo(afterOne.Summary.AddressSpaceIdentity),
                   !oneSnapshot.GetValue().Controller.Previous.IsValid(),
                   !oneSnapshot.GetValue().Controller.SourceTransition.IsValid(),
                   !oneSnapshot.GetValue().Controller.DestinationTransition.IsValid(),
                   oneSnapshot.GetValue().Controller.Processor.GetValue() == 1U,
                   oneSnapshot.GetValue().Controller.State == ProcessorAddressSpaceControllerState::Ready,
                   admittedOne.GetValue().ControllerReceipt.NativeActivationPerformed);

    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto duplicateOne = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{1U});
    const auto stillOutOfOrderThree = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{3U});
    const bool repeatPreservedOwner =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    ASTERIA_EXPECT(
        runner, "duplicate CPU 1 and premature CPU 3 calls cannot repeat native work", !duplicateOne.HasValue(),
        duplicateOne.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::AlreadyAdmitted,
        !stillOutOfOrderThree.HasValue(),
        stillOutOfOrderThree.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::OutOfOrder,
        repeatPreservedOwner, g_installCallCount == 2U, g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));

    const auto admittedTwo = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{2U});
    const auto publishedTwo = owner.PublishExternalInterruptEntryContext(CpuId{2U});
    const auto publishedTwoSnapshot = owner.GetExternalInterruptPublicationSnapshot(CpuId{2U});
    const auto twoSnapshot = owner.GetApplicationProcessorAdmissionSnapshot(CpuId{2U});
    const auto borrowedOneAfterTwo = owner.BorrowReadyApplicationProcessorRoot(CpuId{1U});
    const auto borrowedTwo = owner.BorrowReadyApplicationProcessorRoot(CpuId{2U});
    const Ia64BspRuntimeMemorySnapshot afterTwo = owner.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "CPU 2 admission extends residency and enables its distinct external transport", admittedTwo.HasValue(),
        publishedTwo == Ia64BspRuntimeExternalInterruptPublicationError::None, publishedTwoSnapshot.HasValue(),
        publishedTwoSnapshot.GetValue().Context.ProcessorIndex == 2U, twoSnapshot.HasValue(),
        borrowedOneAfterTwo.HasValue(), borrowedTwo.HasValue(), admittedTwo.GetValue().ResidentProcessorMask == 0x07U,
        admittedTwo.GetValue().InstalledProcessorMask == 0x07U, afterTwo.AddressSpace.ResidentProcessorMask == 0x07U,
        afterTwo.Summary.ResidentProcessorMask == 0x07U,
        afterTwo.Summary.InterruptVectorTable.InstalledProcessorMask == 0x07U,
        afterTwo.Summary.InterruptVectorTable.ProcessorIndex == 2U,
        afterTwo.Bindings[0U] == Ia64BspRuntimeBindingState::Active,
        afterTwo.Bindings[1U] == Ia64BspRuntimeBindingState::Active,
        afterTwo.Bindings[2U] == Ia64BspRuntimeBindingState::Active,
        afterTwo.Bindings[3U] == Ia64BspRuntimeBindingState::Unadmitted,
        twoSnapshot.GetValue().Controller.Processor.GetValue() == 2U,
        twoSnapshot.GetValue().Controller.State == ProcessorAddressSpaceControllerState::Ready,
        borrowedOneAfterTwo.GetValue().AddressSpaceStorage == borrowedTwo.GetValue().AddressSpaceStorage,
        borrowedOneAfterTwo.GetValue().ControllerStorage != borrowedTwo.GetValue().ControllerStorage,
        borrowedOneAfterTwo.GetValue().ControllerStorage == borrowedOne.GetValue().ControllerStorage,
        g_installCallCount == 3U, g_activationCallCount == 3U, NativeEventsAlternateExactly(3U));
    if (!admittedTwo.HasValue() || !twoSnapshot.HasValue() || !borrowedOneAfterTwo.HasValue() ||
        !borrowedTwo.HasValue())
    {
        return;
    }

    const auto admittedThree = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{3U});
    const auto publishedThree = owner.PublishExternalInterruptEntryContext(CpuId{3U});
    const auto publishedThreeSnapshot = owner.GetExternalInterruptPublicationSnapshot(CpuId{3U});
    const auto threeSnapshot = owner.GetApplicationProcessorAdmissionSnapshot(CpuId{3U});
    const Ia64BspRuntimeMemorySnapshot complete = owner.GetSnapshot();
    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto borrowedOneComplete = owner.BorrowReadyApplicationProcessorRoot(CpuId{1U});
    const auto borrowedTwoComplete = owner.BorrowReadyApplicationProcessorRoot(CpuId{2U});
    const auto borrowedThreeComplete = owner.BorrowReadyApplicationProcessorRoot(CpuId{3U});
    const bool completeBorrowPreservedOwner =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    ASTERIA_EXPECT(
        runner, "CPUs 1-3 finish with cumulative four-CPU residency, exact ledgers, and one native pair each",
        admittedThree.HasValue(), publishedThree == Ia64BspRuntimeExternalInterruptPublicationError::None,
        publishedThreeSnapshot.HasValue(), publishedThreeSnapshot.GetValue().Context.ProcessorIndex == 3U,
        threeSnapshot.HasValue(), borrowedOneComplete.HasValue(), borrowedTwoComplete.HasValue(),
        borrowedThreeComplete.HasValue(), admittedThree.GetValue().ResidentProcessorMask == 0x0FU,
        admittedThree.GetValue().InstalledProcessorMask == 0x0FU, complete.State == Ia64BspRuntimeMemoryState::Ready,
        complete.AddressSpace.State == AddressSpaceState::Ready, complete.AddressSpace.ResidentProcessorMask == 0x0FU,
        complete.Summary.ResidentProcessorMask == 0x0FU, complete.Summary.MappingCount == bspReady.Summary.MappingCount,
        complete.AddressSpace.MappingCount == bspReady.AddressSpace.MappingCount,
        complete.Summary.InterruptVectorTable.InstalledProcessorMask == 0x0FU,
        complete.Summary.InterruptVectorTable.ProcessorIndex == 3U,
        AllBindingsAre(complete, Ia64BspRuntimeBindingState::Active),
        threeSnapshot.GetValue().Controller.Processor.GetValue() == 3U,
        threeSnapshot.GetValue().Controller.State == ProcessorAddressSpaceControllerState::Ready,
        completeBorrowPreservedOwner, g_installCallCount == 4U, g_activationCallCount == 4U,
        NativeEventsAlternateExactly(4U));
    if (!admittedThree.HasValue() || !threeSnapshot.HasValue() || !borrowedOneComplete.HasValue() ||
        !borrowedTwoComplete.HasValue() || !borrowedThreeComplete.HasValue())
    {
        return;
    }
    ASTERIA_EXPECT(
        runner, "each AP borrow isolates its dedicated controller while sharing the exact registered ledger root",
        borrowedOneComplete.GetValue().AddressSpaceStorage == complete.AddressSpaceStorage,
        borrowedTwoComplete.GetValue().AddressSpaceStorage == complete.AddressSpaceStorage,
        borrowedThreeComplete.GetValue().AddressSpaceStorage == complete.AddressSpaceStorage,
        borrowedOneComplete.GetValue().AddressSpaceIdentity.IsEqualTo(complete.Summary.AddressSpaceIdentity),
        borrowedTwoComplete.GetValue().AddressSpaceIdentity.IsEqualTo(complete.Summary.AddressSpaceIdentity),
        borrowedThreeComplete.GetValue().AddressSpaceIdentity.IsEqualTo(complete.Summary.AddressSpaceIdentity),
        borrowedOneComplete.GetValue().Processor.GetValue() == 1U,
        borrowedTwoComplete.GetValue().Processor.GetValue() == 2U,
        borrowedThreeComplete.GetValue().Processor.GetValue() == 3U,
        borrowedOneComplete.GetValue().ControllerStorage != borrowedTwoComplete.GetValue().ControllerStorage,
        borrowedOneComplete.GetValue().ControllerStorage != borrowedThreeComplete.GetValue().ControllerStorage,
        borrowedTwoComplete.GetValue().ControllerStorage != borrowedThreeComplete.GetValue().ControllerStorage,
        borrowedOneComplete.GetValue().ControllerStorage == borrowedOne.GetValue().ControllerStorage,
        borrowedTwoComplete.GetValue().ControllerStorage == borrowedTwo.GetValue().ControllerStorage,
        admittedOne.GetValue().InterruptVectorTable.ContextAddress !=
            admittedTwo.GetValue().InterruptVectorTable.ContextAddress,
        admittedOne.GetValue().InterruptVectorTable.ContextAddress !=
            admittedThree.GetValue().InterruptVectorTable.ContextAddress,
        admittedTwo.GetValue().InterruptVectorTable.ContextAddress !=
            admittedThree.GetValue().InterruptVectorTable.ContextAddress,
        admittedTwo.GetValue().AddressSpaceIdentity.IsEqualTo(admittedOne.GetValue().AddressSpaceIdentity),
        admittedThree.GetValue().AddressSpaceIdentity.IsEqualTo(admittedOne.GetValue().AddressSpaceIdentity),
        oneSnapshot.GetValue().Controller.ActiveStorage == twoSnapshot.GetValue().Controller.ActiveStorage,
        twoSnapshot.GetValue().Controller.ActiveStorage == threeSnapshot.GetValue().Controller.ActiveStorage);

    const auto externalFromProcessorOne = owner.PublishExternalInterruptEntryContext(CpuId{1U});
    const auto externalFromBsp = owner.PublishExternalInterruptEntryContext(bspProcessor);
    const auto processorZeroPublication = owner.GetExternalInterruptPublicationSnapshot(CpuId{0U});
    const auto processorOnePublication = owner.GetExternalInterruptPublicationSnapshot(CpuId{1U});
    const auto processorTwoPublication = owner.GetExternalInterruptPublicationSnapshot(CpuId{2U});
    const auto processorThreePublication = owner.GetExternalInterruptPublicationSnapshot(CpuId{3U});
    const auto systemCallFromProcessorOne =
        owner.PublishSystemCallEntryContext(CpuId{1U}, AddressOf(&g_alignedBridgeContextFixture));
    const auto systemCallFromBsp =
        owner.PublishSystemCallEntryContext(bspProcessor, AddressOf(&g_alignedBridgeContextFixture));
    const Ia64BspRuntimeMemorySnapshot afterPublications = owner.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "every resident CPU owns a one-shot interrupt transport while system-call publication remains BSP-only",
        externalFromProcessorOne == Ia64BspRuntimeExternalInterruptPublicationError::AlreadyPublished,
        externalFromBsp == Ia64BspRuntimeExternalInterruptPublicationError::None, processorZeroPublication.HasValue(),
        processorOnePublication.HasValue(), processorTwoPublication.HasValue(), processorThreePublication.HasValue(),
        systemCallFromProcessorOne == Ia64BspRuntimeSystemCallPublicationError::ProcessorMismatch,
        systemCallFromBsp == Ia64BspRuntimeSystemCallPublicationError::None,
        afterPublications.ExternalInterruptPublication.Published,
        afterPublications.ExternalInterruptPublication.Context.ProcessorIndex == bspProcessor.GetValue(),
        afterPublications.ExternalInterruptPublication.LastError ==
            Ia64BspRuntimeExternalInterruptPublicationError::None,
        afterPublications.SystemCallPublication.Published,
        afterPublications.SystemCallPublication.Context.ProcessorIndex == bspProcessor.GetValue(),
        afterPublications.SystemCallPublication.LastError == Ia64BspRuntimeSystemCallPublicationError::None,
        afterPublications.Summary.ResidentProcessorMask == 0x0FU,
        afterPublications.AddressSpace.ResidentProcessorMask == 0x0FU);

    if (!processorZeroPublication.HasValue() || !processorOnePublication.HasValue() ||
        !processorTwoPublication.HasValue() || !processorThreePublication.HasValue())
    {
        return;
    }
    const auto processorZero = processorZeroPublication.GetValue();
    const auto processorOne = processorOnePublication.GetValue();
    const auto processorTwo = processorTwoPublication.GetValue();
    const auto processorThree = processorThreePublication.GetValue();
    constexpr Foundation::UInt64 transportSlotBytes{sizeof(Ia64BspRuntimeExternalInterruptTransportSlot)};
    const bool slotsAreInside =
        RangeIsInside(bspReady.Summary.RuntimeObjects.VirtualStart, bspReady.Summary.RuntimeObjects.SizeBytes,
                      processorZero.ContextAddress, transportSlotBytes * Kernel::Scheduler::MaximumProcessorCount);
    bool reservedSlotsAreZero{slotsAreInside};
    if (slotsAreInside)
    {
        const auto* const rawSlots = reinterpret_cast<const Ia64BspRuntimeExternalInterruptTransportSlot*>(
            static_cast<Foundation::UIntPointer>(processorZero.ContextAddress));
        for (Foundation::Size processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
        {
            for (const Foundation::UInt8 value : rawSlots[processor].Reserved)
            {
                if (value != 0U)
                {
                    reservedSlotsAreZero = false;
                }
            }
        }
    }
    ASTERIA_EXPECT(
        runner, "four ABI-sized transport slots are contiguous inside the runtime corridor with isolated diagnostics",
        slotsAreInside, processorOne.ContextAddress == processorZero.ContextAddress + transportSlotBytes,
        processorTwo.ContextAddress == processorZero.ContextAddress + (2U * transportSlotBytes),
        processorThree.ContextAddress == processorZero.ContextAddress + (3U * transportSlotBytes),
        processorZero.EvidenceAddress ==
            processorZero.ContextAddress + offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence),
        processorOne.EvidenceAddress ==
            processorOne.ContextAddress + offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence),
        processorTwo.EvidenceAddress ==
            processorTwo.ContextAddress + offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence),
        processorThree.EvidenceAddress ==
            processorThree.ContextAddress + offsetof(Ia64BspRuntimeExternalInterruptTransportSlot, Evidence),
        processorOne.LastError == Ia64BspRuntimeExternalInterruptPublicationError::AlreadyPublished,
        processorTwo.LastError == Ia64BspRuntimeExternalInterruptPublicationError::None,
        processorThree.LastError == Ia64BspRuntimeExternalInterruptPublicationError::None,
        processorZero.LastError == Ia64BspRuntimeExternalInterruptPublicationError::None, reservedSlotsAreZero);
    ASTERIA_EXPECT(runner, "each CPU IVT offset-24 word release-publishes only its matching tail slot",
                   AcquireExternalInterruptPublicationAt(bspReady.Summary.InterruptVectorTable.ContextAddress) ==
                       processorZero.ContextAddress,
                   AcquireExternalInterruptPublicationAt(admittedOne.GetValue().InterruptVectorTable.ContextAddress) ==
                       processorOne.ContextAddress,
                   AcquireExternalInterruptPublicationAt(admittedTwo.GetValue().InterruptVectorTable.ContextAddress) ==
                       processorTwo.ContextAddress,
                   AcquireExternalInterruptPublicationAt(
                       admittedThree.GetValue().InterruptVectorTable.ContextAddress) == processorThree.ContextAddress);

    static_cast<void>(std::memcpy(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)));
    const auto invalidPublication = owner.PublishExternalInterruptEntryContext(CpuId{4U});
    const auto invalidAcknowledgement = owner.AcknowledgeExternalInterruptEvidence(CpuId{4U}, 1U);
    const bool invalidCpuPreservedAllSlots =
        (std::memcmp(g_ownerByteSnapshot, static_cast<const void*>(&owner), sizeof(owner)) == 0);
    ASTERIA_EXPECT(runner, "invalid CpuId publication and acknowledgement are total owner nonmutation",
                   invalidPublication == Ia64BspRuntimeExternalInterruptPublicationError::InvalidProcessor,
                   invalidAcknowledgement == Ia64BspRuntimeExternalInterruptAcknowledgementError::InvalidProcessor,
                   invalidCpuPreservedAllSlots);

    auto* const processorZeroEvidence = reinterpret_cast<Ia64LocalTimerInterruptEvidence*>(
        static_cast<Foundation::UIntPointer>(processorZero.EvidenceAddress));
    auto* const processorOneEvidence = reinterpret_cast<Ia64LocalTimerInterruptEvidence*>(
        static_cast<Foundation::UIntPointer>(processorOne.EvidenceAddress));
    AtomicUInt64Reference processorZeroSequence{processorZeroEvidence->Sequence};
    AtomicUInt64Reference processorOneSequence{processorOneEvidence->Sequence};
    processorZeroSequence.StoreRelease(1U);
    processorOneSequence.StoreRelease(1U);
    const auto acknowledgedZeroOne = owner.AcknowledgeExternalInterruptEvidence(CpuId{0U}, 1U);
    const auto oneBeforeAcknowledgement = owner.GetExternalInterruptPublicationSnapshot(CpuId{1U});
    const auto acknowledgedOneOne = owner.AcknowledgeExternalInterruptEvidence(CpuId{1U}, 1U);
    processorZeroSequence.StoreRelease(2U);
    const auto acknowledgedZeroTwo = owner.AcknowledgeExternalInterruptEvidence(CpuId{0U}, 2U);
    const auto zeroAfterTwo = owner.GetExternalInterruptPublicationSnapshot(CpuId{0U});
    const auto oneAfterOne = owner.GetExternalInterruptPublicationSnapshot(CpuId{1U});
    ASTERIA_EXPECT(runner, "CPU 0 sequence 1/2 and CPU 1 sequence 1 acknowledge independently without cross-slot state",
                   acknowledgedZeroOne == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
                   oneBeforeAcknowledgement.HasValue(), oneBeforeAcknowledgement.GetValue().Evidence.Sequence == 1U,
                   oneBeforeAcknowledgement.GetValue().Evidence.AcknowledgedSequence == 0U,
                   acknowledgedOneOne == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
                   acknowledgedZeroTwo == Ia64BspRuntimeExternalInterruptAcknowledgementError::None,
                   zeroAfterTwo.HasValue(), oneAfterOne.HasValue(), zeroAfterTwo.GetValue().Evidence.Sequence == 2U,
                   zeroAfterTwo.GetValue().Evidence.AcknowledgedSequence == 2U,
                   oneAfterOne.GetValue().Evidence.Sequence == 1U,
                   oneAfterOne.GetValue().Evidence.AcknowledgedSequence == 1U);
}

void TestConfiguredPageFaultOwnerMismatchIsTerminal(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{0U};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[2U];
    const Ia64BspRuntimeMemorySnapshot ready = owner.GetSnapshot();
    const auto storageResult = owner.BorrowPageFaultEntryStorage(bspProcessor);
    ASTERIA_EXPECT(runner,
                   "the terminal page-fault mismatch fixture borrows fresh storage from the already-ready BSP owner",
                   ready.State == Ia64BspRuntimeMemoryState::Ready, storageResult.HasValue());
    if (!storageResult.HasValue())
    {
        return;
    }

    const auto storage = storageResult.GetValue();
    const Foundation::UInt64 interruptedRseStart = AddressOf(g_initialRseBackingStore);
    const Ia64PageFaultEntryControlError configurationError = ConfigureIa64PageFaultEntryControl(
        *storage.Control, Ia64PageFaultEntryControlConfiguration{
                              .ProcessorIndex = 1U,
                              .FrameAddress = storage.FrameAddress,
                              .InterruptedRseBackingStoreStart = interruptedRseStart,
                              .InterruptedRseBackingStoreEnd = interruptedRseStart + InitialRseBackingStoreSizeBytes,
                              .CoordinatorMemoryStackStart = storage.CoordinatorMemoryStackAddress,
                              .CoordinatorMemoryStackEnd =
                                  storage.CoordinatorMemoryStackAddress + storage.CoordinatorMemoryStackSizeBytes,
                              .CoordinatorRseBackingStoreStart = storage.CoordinatorRseBackingStoreAddress,
                              .CoordinatorRseBackingStoreEnd = storage.CoordinatorRseBackingStoreAddress +
                                                               storage.CoordinatorRseBackingStoreSizeBytes,
                              .CoordinatorEntryPoint = ready.Summary.InterruptVectorTable.TableAddress,
                              .CoordinatorGlobalPointer = 0U,
                              .CoordinatorArgument = AddressOf(&owner),
                          });
    const auto mismatch =
        owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control, *storage.Context, *storage.Frame);
    const auto afterMismatch = owner.GetPageFaultEntrySnapshot();
    const auto controlAfterMismatch = AcquireIa64PageFaultEntryControlSnapshot(*storage.Control);
    const auto repeated =
        owner.PublishPageFaultEntryContext(bspProcessor, *storage.Control, *storage.Context, *storage.Frame);
    const auto afterRepeated = owner.GetPageFaultEntrySnapshot();
    const auto controlAfterRepeated = AcquireIa64PageFaultEntryControlSnapshot(*storage.Control);
    ASTERIA_EXPECT(
        runner,
        "a valid configured control with immutable foreign processor ownership faults permanently without publication",
        configurationError == Ia64PageFaultEntryControlError::None,
        mismatch == Ia64BspRuntimePageFaultEntryError::ConfigurationMismatch,
        afterMismatch.State == Ia64BspRuntimePageFaultEntryState::Faulted,
        afterMismatch.LastError == Ia64BspRuntimePageFaultEntryError::ConfigurationMismatch,
        controlAfterMismatch.State == Ia64PageFaultEntryControlState::Configured,
        controlAfterMismatch.Configuration.ProcessorIndex == 1U, PageFaultContextIsZero(afterMismatch.Context),
        Ia64PageFaultFrameIsPristine(*storage.Frame),
        repeated == Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch,
        afterRepeated.State == Ia64BspRuntimePageFaultEntryState::Faulted,
        afterRepeated.LastError == Ia64BspRuntimePageFaultEntryError::PublicationStateMismatch,
        controlAfterRepeated.State == Ia64PageFaultEntryControlState::Configured,
        controlAfterRepeated.Configuration.ProcessorIndex == 1U, PageFaultContextIsZero(afterRepeated.Context));
}

void TestDestroyedRootIdentityRejectsApplicationProcessorAdmission(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{0U};
    constexpr CpuId applicationProcessor{1U};
    constexpr CpuId laterApplicationProcessor{2U};
    constexpr Foundation::UInt8 profiledProcessorMask{0x07U};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[0U];
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    processorCapabilities[0U] = Capabilities();
    processorCapabilities[1U] = Capabilities();
    processorCapabilities[2U] = Capabilities();

    ResetNative(AdapterSuccess);
    const auto prepared =
        owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities, g_preparation,
                              ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    ASTERIA_EXPECT(runner, "the destroyed-root fixture first publishes one exact BSP root", prepared.HasValue());
    if (!prepared.HasValue())
    {
        return;
    }

    const auto admittedApplicationProcessor = owner.AdmitApplicationProcessorPhysicalRoot(applicationProcessor);
    const auto readyApplicationProcessorRoot = owner.BorrowReadyApplicationProcessorRoot(applicationProcessor);
    ASTERIA_EXPECT(runner, "the destroyed-root fixture first borrows one fully admitted AP slot",
                   admittedApplicationProcessor.HasValue(), readyApplicationProcessorRoot.HasValue(),
                   readyApplicationProcessorRoot.GetValue().AddressSpaceStorage != nullptr,
                   readyApplicationProcessorRoot.GetValue().ControllerStorage != nullptr,
                   readyApplicationProcessorRoot.GetValue().Processor.GetValue() == applicationProcessor.GetValue());
    if (!admittedApplicationProcessor.HasValue() || !readyApplicationProcessorRoot.HasValue())
    {
        return;
    }

    const Kernel::Memory::AddressSpaceId identity = prepared.GetValue().AddressSpaceIdentity;
    const Foundation::Size nativeBefore = g_nativeSequence;
    const ObjectRegistryError destroyed = g_issuer.DestroyObject(identity.GetObjectId());
    const auto prematureCollection = g_issuer.CollectDestroyedObject();
    ObjectPin rejectedPin;
    const ObjectPinError repinError =
        g_registry.PinObject(identity.GetObjectId(), ObjectType::AddressSpace, rejectedPin);
    const auto borrowed = owner.BorrowReadyRoot();
    const auto borrowedApplicationProcessor = owner.BorrowReadyApplicationProcessorRoot(applicationProcessor);
    const auto admitted = owner.AdmitApplicationProcessorPhysicalRoot(laterApplicationProcessor);

    ASTERIA_EXPECT(runner,
                   "a quiescing shared-root generation invalidates an existing AP borrow and blocks later admission",
                   destroyed == ObjectRegistryError::Ok, !prematureCollection.HasValue(),
                   prematureCollection.GetError() == ObjectRegistryError::NoDestructionReady,
                   repinError == ObjectPinError::ObjectNotFound, !rejectedPin.IsValid(), !borrowed.HasValue(),
                   borrowed.GetError() == Ia64BspRuntimeRootBorrowError::InvariantMismatch,
                   !borrowedApplicationProcessor.HasValue(),
                   borrowedApplicationProcessor.GetError() ==
                       Ia64BspRuntimeApplicationProcessorAdmissionError::RootInvariantMismatch,
                   !admitted.HasValue(),
                   admitted.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::RootInvariantMismatch,
                   g_nativeSequence == nativeBefore, g_installCallCount == 2U, g_activationCallCount == 2U);
}

void TestProfileMaskMustContainBsp(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{3U};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[1U];
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    processorCapabilities[0U] = Capabilities(18U);
    processorCapabilities[1U] = Capabilities(20U);
    constexpr Foundation::UInt8 profiledProcessorMask{0x03U};
    ResetNative(AdapterSuccess);
    const auto rejected =
        owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities, g_preparation,
                              ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "the explicit profile mask must contain the BSP before any runtime-root native write",
        !rejected.HasValue(), rejected.GetError() == Ia64BspRuntimeMemoryError::ProcessorCapabilitiesRejected,
        failed.State == Ia64BspRuntimeMemoryState::Failed,
        failed.Failure.ProcessorCapabilitiesError == Ia64RuntimeProcessorCapabilitiesError::ProcessorNotAdmitted,
        failed.RegionIdentifierAllocator.State == Ia64RegionIdentifierAllocatorState::Uninitialized,
        failed.AddressSpace.State == AddressSpaceState::Uninitialized,
        AllBindingsAre(failed, Ia64BspRuntimeBindingState::Unadmitted), g_registry.GetActiveCount() == 0U,
        g_installCallCount == 0U, g_activationCallCount == 0U);
}

void TestInvalidChargeOwner(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{2U};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[0U];
    ResetNative(AdapterSuccess);
    const auto rejected =
        owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation, ResourceDomainId{}, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "invalid charge owner leaves the prepared backend Ready but unclaimed", !rejected.HasValue(),
                   rejected.GetError() == Ia64BspRuntimeMemoryError::AddressSpaceInitializationRejected,
                   failed.State == Ia64BspRuntimeMemoryState::Failed,
                   failed.Failure.Error == Ia64BspRuntimeMemoryError::AddressSpaceInitializationRejected,
                   failed.Failure.AddressSpaceInitializationError == AddressSpaceError::InvalidConfiguration,
                   failed.Failure.AddressSpaceBackendError ==
                       Platform::Ia64::Memory::Ia64RuntimeAddressSpacePageTableInitializationError::None,
                   !failed.Summary.BackendClaimed, !failed.Summary.ExactLedgerVerified,
                   failed.AddressSpace.State == AddressSpaceState::Uninitialized,
                   !failed.AddressSpace.Identity.IsValid(), failed.AddressSpace.MappingCount == 0U);
    ASTERIA_EXPECT(runner, "invalid charge owner consumes one RID receipt before IVT, registration, or activation",
                   AllocatorHasOneExactReceipt(failed), g_registry.GetActiveCount() == 0U, g_installCallCount == 0U,
                   g_activationCallCount == 0U, g_registryCountAtInstall == UnobservedRegistryCount,
                   g_registryCountAtActivation == UnobservedRegistryCount, g_nativeSequence == 0U,
                   AllBindingsAre(failed, Ia64BspRuntimeBindingState::Unadmitted),
                   failed.Controller.State == ProcessorAddressSpaceControllerState::Uninitialized);

    const auto repeated = owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation,
                                                ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot afterRepeat = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "repeat after invalid charge owner duplicates no terminal work", !repeated.HasValue(),
                   repeated.GetError() == Ia64BspRuntimeMemoryError::PreviousPreparationFailed,
                   AllocatorHasOneExactReceipt(afterRepeat), g_registry.GetActiveCount() == 0U,
                   g_installCallCount == 0U, g_activationCallCount == 0U,
                   g_registryCountAtInstall == UnobservedRegistryCount,
                   g_registryCountAtActivation == UnobservedRegistryCount);
}

void TestActivationRejected(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{2U};
    const ResourceDomainId chargeOwner{ChargeOwnerValue};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[0U];
    ResetNative(AdapterRejectedBeforeCommit);
    const auto rejected =
        owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation, chargeOwner, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "native pre-commit rejection is reported without false success", !rejected.HasValue(),
                   rejected.GetError() == Ia64BspRuntimeMemoryError::ProcessorControllerActivationRejected,
                   failed.State == Ia64BspRuntimeMemoryState::Failed,
                   failed.Failure.ControllerFailure.Error == ProcessorAddressSpaceControllerError::ActivationRejected,
                   failed.Failure.ControllerFailure.AdmissionRollbackSafe,
                   !failed.Failure.ControllerFailure.Activation.ProcessorMayBeResident,
                   failed.Failure.ControllerFailure.Activation.Error == AddressSpaceError::PageTableActivationRejected,
                   failed.Failure.ControllerFailure.Activation.PageTable ==
                       AddressSpacePageTableActivationError::RejectedBeforeCommit);
    ASTERIA_EXPECT(runner, "pre-commit rejection retains the exact registered root with zero residency",
                   failed.Summary.BackendClaimed, !failed.Summary.ExactLedgerVerified,
                   failed.AddressSpace.State == AddressSpaceState::Ready,
                   failed.AddressSpace.MappingCount == BootstrapInitialMappingCount,
                   failed.AddressSpace.ResidentProcessorMask == 0U, failed.AddressSpace.TransitionSequence == 0U,
                   !failed.AddressSpace.TransitionProcessor.IsValid(), RegisteredRootIsExact(failed, chargeOwner),
                   g_registry.GetActiveCount() == 1U);
    ASTERIA_EXPECT(runner,
                   "rollback-safe rejection leaves the controller uninitialized and BSP admitted without ledger",
                   failed.Controller.State == ProcessorAddressSpaceControllerState::Uninitialized,
                   failed.Controller.IdleStorage == nullptr, failed.Controller.ActiveStorage == nullptr,
                   failed.Controller.PreviousStorage == nullptr, !failed.Controller.SourceTransition.IsValid(),
                   !failed.Controller.DestinationTransition.IsValid(),
                   OnlyBspBindingIs(failed, bspProcessor, Ia64BspRuntimeBindingState::AdmittedWithoutLedger));
    ASTERIA_EXPECT(runner, "rejected activation follows one install, registration, and exact native attempt",
                   AllocatorHasOneExactReceipt(failed), g_installCallCount == 1U, g_activationCallCount == 1U,
                   g_installSequence == 1U, g_activationSequence == 2U, g_registryCountAtInstall == 0U,
                   g_registryCountAtActivation == 1U, ActivationWordsAreExact());

    const auto repeated =
        owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation, chargeOwner, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot afterRepeat = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "repeat after pre-commit rejection duplicates no retained work", !repeated.HasValue(),
                   repeated.GetError() == Ia64BspRuntimeMemoryError::PreviousPreparationFailed,
                   AllocatorHasOneExactReceipt(afterRepeat), g_registry.GetActiveCount() == 1U,
                   g_installCallCount == 1U, g_activationCallCount == 1U, g_registryCountAtInstall == 0U,
                   g_registryCountAtActivation == 1U, g_nativeSequence == 2U);
}

void TestActivationUnknown(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{2U};
    const ResourceDomainId chargeOwner{ChargeOwnerValue};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[0U];
    ResetNative(AdapterCommittedStateUnknown);
    const auto rejected =
        owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation, chargeOwner, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "committed-state uncertainty is terminal and never reported as success",
                   !rejected.HasValue(),
                   rejected.GetError() == Ia64BspRuntimeMemoryError::ProcessorControllerActivationRejected,
                   failed.State == Ia64BspRuntimeMemoryState::Failed,
                   failed.Failure.ControllerFailure.Error == ProcessorAddressSpaceControllerError::ActivationRejected,
                   !failed.Failure.ControllerFailure.AdmissionRollbackSafe,
                   failed.Failure.ControllerFailure.Activation.ProcessorMayBeResident,
                   failed.Failure.ControllerFailure.Activation.Error ==
                       AddressSpaceError::PageTableActivationCommittedStateUnknown,
                   failed.Failure.ControllerFailure.Activation.PageTable ==
                       AddressSpacePageTableActivationError::CommittedStateUnknown);
    ASTERIA_EXPECT(runner, "uncertainty retains conservative root residency and the registered identity",
                   failed.Summary.BackendClaimed, !failed.Summary.ExactLedgerVerified,
                   failed.AddressSpace.State == AddressSpaceState::Faulted,
                   failed.AddressSpace.MappingCount == BootstrapInitialMappingCount,
                   failed.AddressSpace.ResidentProcessorMask == 0x04U, failed.AddressSpace.TransitionSequence != 0U,
                   failed.AddressSpace.TransitionProcessor.GetValue() == bspProcessor.GetValue(),
                   RegisteredRootIsExact(failed, chargeOwner), g_registry.GetActiveCount() == 1U);
    ASTERIA_EXPECT(
        runner, "uncertainty faults the controller and retains admitted/no-ledger binding authority",
        failed.Controller.State == ProcessorAddressSpaceControllerState::Faulted,
        failed.Controller.IdleStorage == failed.AddressSpaceStorage,
        failed.Controller.ActiveStorage == failed.AddressSpaceStorage, failed.Controller.PreviousStorage == nullptr,
        failed.Controller.Idle.IsEqualTo(failed.Summary.AddressSpaceIdentity),
        failed.Controller.Active.IsEqualTo(failed.Summary.AddressSpaceIdentity), !failed.Controller.Previous.IsValid(),
        !failed.Controller.SourceTransition.IsValid(), failed.Controller.DestinationTransition.IsValid(),
        failed.Controller.DestinationTransition.IsFor(*failed.AddressSpaceStorage),
        OnlyBspBindingIs(failed, bspProcessor, Ia64BspRuntimeBindingState::AdmittedWithoutLedger));
    ASTERIA_EXPECT(runner, "uncertain activation follows one install, registration, and exact native attempt",
                   AllocatorHasOneExactReceipt(failed), g_installCallCount == 1U, g_activationCallCount == 1U,
                   g_installSequence == 1U, g_activationSequence == 2U, g_registryCountAtInstall == 0U,
                   g_registryCountAtActivation == 1U, ActivationWordsAreExact());

    const auto repeated =
        owner.PrepareAndAdmit(bspProcessor, Capabilities(), g_preparation, chargeOwner, g_registry, g_issuer);
    const Ia64BspRuntimeMemorySnapshot afterRepeat = owner.GetSnapshot();
    ASTERIA_EXPECT(runner, "repeat after committed-state uncertainty duplicates no retained work", !repeated.HasValue(),
                   repeated.GetError() == Ia64BspRuntimeMemoryError::PreviousPreparationFailed,
                   AllocatorHasOneExactReceipt(afterRepeat), g_registry.GetActiveCount() == 1U,
                   g_installCallCount == 1U, g_activationCallCount == 1U, g_registryCountAtInstall == 0U,
                   g_registryCountAtActivation == 1U, g_nativeSequence == 2U, !afterRepeat.Summary.ExactLedgerVerified);
}

void TestApplicationProcessorActivationRejected(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{0U};
    constexpr CpuId applicationProcessor{1U};
    constexpr Foundation::UInt8 profiledProcessorMask{0x0FU};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[1U];
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        processorCapabilities[processor] = Capabilities();
    }

    ResetNative(AdapterSuccess);
    const auto prepared =
        owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities, g_preparation,
                              ResourceDomainId{ChargeOwnerValue + 1U}, g_registry, g_issuer);
    ASTERIA_EXPECT(runner, "the AP pre-commit rejection fixture first commits only CPU 0", prepared.HasValue(),
                   owner.GetSnapshot().State == Ia64BspRuntimeMemoryState::Ready,
                   owner.GetSnapshot().AddressSpace.ResidentProcessorMask == 0x01U, g_installCallCount == 1U,
                   g_activationCallCount == 1U, NativeEventsAlternateExactly(1U));
    if (!prepared.HasValue())
    {
        return;
    }

    g_activationStatus = AdapterRejectedBeforeCommit;
    const auto rejected = owner.AdmitApplicationProcessorPhysicalRoot(applicationProcessor);
    const auto admission = owner.GetApplicationProcessorAdmissionSnapshot(applicationProcessor);
    const auto failedRootBorrow = owner.BorrowReadyApplicationProcessorRoot(applicationProcessor);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    ASTERIA_EXPECT(
        runner,
        "an injected physical pre-commit rejection retains the committed AP IVT and fails closed without residency",
        !rejected.HasValue(),
        rejected.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::PhysicalBootstrapActivationRejected,
        admission.HasValue(), admission.GetValue().State == Ia64BspRuntimeApplicationProcessorAdmissionState::Failed,
        admission.GetValue().Failure.Error ==
            Ia64BspRuntimeApplicationProcessorAdmissionError::PhysicalBootstrapActivationRejected,
        admission.GetValue().Failure.PhysicalBootstrapActivationError ==
            AddressSpacePageTableActivationError::RejectedBeforeCommit,
        admission.GetValue().Failure.InterruptVectorTableCommitted,
        !admission.GetValue().Failure.PhysicalRootMayBeActive,
        admission.GetValue().Controller.State == ProcessorAddressSpaceControllerState::Uninitialized,
        !failedRootBorrow.HasValue(),
        failedRootBorrow.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed,
        failed.State == Ia64BspRuntimeMemoryState::Failed, failed.AddressSpace.State == AddressSpaceState::Ready,
        failed.AddressSpace.ResidentProcessorMask == 0x01U, failed.Bindings[0U] == Ia64BspRuntimeBindingState::Active,
        failed.Bindings[1U] == Ia64BspRuntimeBindingState::AdmittedWithoutLedger,
        failed.Bindings[2U] == Ia64BspRuntimeBindingState::Unadmitted,
        failed.Bindings[3U] == Ia64BspRuntimeBindingState::Unadmitted);
    ASTERIA_EXPECT(runner,
                   "the rejected AP transaction performs exactly one IVT install and one physical native attempt",
                   g_installCallCount == 2U, g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));

    const auto repeated = owner.AdmitApplicationProcessorPhysicalRoot(applicationProcessor);
    const auto later = owner.AdmitApplicationProcessorPhysicalRoot(CpuId{2U});
    ASTERIA_EXPECT(runner, "repeated and later AP calls after a committed-IVT failure perform no further native work",
                   !repeated.HasValue(),
                   repeated.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed,
                   !later.HasValue(), later.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::NotReady,
                   g_installCallCount == 2U, g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));
}

void TestReadyApplicationProcessorBorrowAfterLaterInstallRejection(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{0U};
    constexpr CpuId readyApplicationProcessor{1U};
    constexpr CpuId rejectedApplicationProcessor{2U};
    constexpr CpuId unadmittedApplicationProcessor{3U};
    constexpr Foundation::UInt8 profiledProcessorMask{0x0FU};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[2U];
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        processorCapabilities[processor] = Capabilities();
    }

    ResetNative(AdapterSuccess);
    const Foundation::Size registryCountBefore = g_registry.GetActiveCount();
    const auto prepared =
        owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities, g_preparation,
                              ResourceDomainId{ChargeOwnerValue + 2U}, g_registry, g_issuer);
    const auto admittedReady = owner.AdmitApplicationProcessorPhysicalRoot(readyApplicationProcessor);
    const auto borrowedReady = owner.BorrowReadyApplicationProcessorRoot(readyApplicationProcessor);
    ASTERIA_EXPECT(runner, "the IVT rejection regression first commits BSP 0 and AP 1 Ready", prepared.HasValue(),
                   admittedReady.HasValue(), borrowedReady.HasValue(),
                   owner.GetSnapshot().State == Ia64BspRuntimeMemoryState::Ready,
                   owner.GetSnapshot().AddressSpace.ResidentProcessorMask == 0x03U,
                   g_registry.GetActiveCount() == (registryCountBefore + 3U), g_installCallCount == 2U,
                   g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));
    if (!prepared.HasValue() || !admittedReady.HasValue() || !borrowedReady.HasValue())
    {
        return;
    }

    g_installStatus = AdapterRejectedBeforeCommit;
    const auto rejected = owner.AdmitApplicationProcessorPhysicalRoot(rejectedApplicationProcessor);
    const auto rejectedAdmission = owner.GetApplicationProcessorAdmissionSnapshot(rejectedApplicationProcessor);
    const auto borrowedReadyAgain = owner.BorrowReadyApplicationProcessorRoot(readyApplicationProcessor);
    const auto borrowedRejected = owner.BorrowReadyApplicationProcessorRoot(rejectedApplicationProcessor);
    const auto borrowedUnadmitted = owner.BorrowReadyApplicationProcessorRoot(unadmittedApplicationProcessor);
    const Ia64BspRuntimeMemorySnapshot retained = owner.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "AP 2 IVT rejection retains the exact shared BSP/AP 1 root without global failure",
        !rejected.HasValue(),
        rejected.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::InterruptVectorTableInstallRejected,
        rejectedAdmission.HasValue(),
        rejectedAdmission.GetValue().State == Ia64BspRuntimeApplicationProcessorAdmissionState::Failed,
        rejectedAdmission.GetValue().Failure.Error ==
            Ia64BspRuntimeApplicationProcessorAdmissionError::InterruptVectorTableInstallRejected,
        rejectedAdmission.GetValue().Failure.InterruptVectorTableError ==
            Ia64RuntimeInterruptVectorTableInstallError::UnsupportedProcessorState,
        !rejectedAdmission.GetValue().Failure.InterruptVectorTableCommitted,
        !rejectedAdmission.GetValue().Failure.PhysicalRootMayBeActive,
        rejectedAdmission.GetValue().Controller.State == ProcessorAddressSpaceControllerState::Uninitialized,
        retained.State == Ia64BspRuntimeMemoryState::Ready, retained.Failure.Error == Ia64BspRuntimeMemoryError::None,
        retained.AddressSpace.ResidentProcessorMask == 0x03U, retained.Summary.ResidentProcessorMask == 0x03U,
        retained.Summary.InterruptVectorTable.InstalledProcessorMask == 0x03U,
        retained.Summary.InterruptVectorTable.ProcessorIndex == readyApplicationProcessor.GetValue(),
        retained.Bindings[0U] == Ia64BspRuntimeBindingState::Active,
        retained.Bindings[1U] == Ia64BspRuntimeBindingState::Active,
        retained.Bindings[2U] == Ia64BspRuntimeBindingState::Unadmitted,
        retained.Bindings[3U] == Ia64BspRuntimeBindingState::Unadmitted);
    ASTERIA_EXPECT(
        runner, "AP 1 reborrow remains stable while failed AP 2 and unadmitted AP 3 report their exact slots",
        borrowedReadyAgain.HasValue(),
        borrowedReadyAgain.GetValue().AddressSpaceStorage == borrowedReady.GetValue().AddressSpaceStorage,
        borrowedReadyAgain.GetValue().AddressSpaceIdentity.IsEqualTo(borrowedReady.GetValue().AddressSpaceIdentity),
        borrowedReadyAgain.GetValue().ControllerStorage == borrowedReady.GetValue().ControllerStorage,
        borrowedReadyAgain.GetValue().Processor.GetValue() == readyApplicationProcessor.GetValue(),
        borrowedReadyAgain.GetValue().ControllerStorage->GetSnapshot().Processor.GetValue() ==
            readyApplicationProcessor.GetValue(),
        !borrowedRejected.HasValue(),
        borrowedRejected.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed,
        !borrowedUnadmitted.HasValue(),
        borrowedUnadmitted.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::NotReady,
        g_installCallCount == 3U, g_activationCallCount == 2U, g_nativeSequence == 5U, g_nativeEvents[0U] == 1U,
        g_nativeEvents[1U] == 2U, g_nativeEvents[2U] == 1U, g_nativeEvents[3U] == 2U, g_nativeEvents[4U] == 1U);
}

void TestApplicationProcessorActivationUnknown(TestContext& runner) noexcept
{
    constexpr CpuId bspProcessor{0U};
    constexpr CpuId applicationProcessor{1U};
    constexpr Foundation::UInt8 profiledProcessorMask{0x0FU};
    Ia64BspRuntimeMemory& owner = g_arena.Owners[1U];
    PalVirtualMemoryCapabilities processorCapabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 processor{0U}; processor < Kernel::Scheduler::MaximumProcessorCount; ++processor)
    {
        processorCapabilities[processor] = Capabilities();
    }

    ResetNative(AdapterSuccess);
    const auto prepared =
        owner.PrepareAndAdmit(bspProcessor, profiledProcessorMask, processorCapabilities, g_preparation,
                              ResourceDomainId{ChargeOwnerValue + 1U}, g_registry, g_issuer);
    ASTERIA_EXPECT(runner, "the AP post-commit uncertainty fixture first commits only CPU 0", prepared.HasValue(),
                   owner.GetSnapshot().State == Ia64BspRuntimeMemoryState::Ready, g_installCallCount == 1U,
                   g_activationCallCount == 1U, NativeEventsAlternateExactly(1U));
    if (!prepared.HasValue())
    {
        return;
    }

    g_activationStatus = AdapterCommittedStateUnknown;
    const auto rejected = owner.AdmitApplicationProcessorPhysicalRoot(applicationProcessor);
    const auto admission = owner.GetApplicationProcessorAdmissionSnapshot(applicationProcessor);
    const Ia64BspRuntimeMemorySnapshot failed = owner.GetSnapshot();
    const auto borrowed = owner.BorrowReadyRoot();
    const auto failedApplicationProcessorBorrow = owner.BorrowReadyApplicationProcessorRoot(applicationProcessor);
    ASTERIA_EXPECT(
        runner, "post-commit uncertainty conservatively retains possible AP activity and closes the complete owner",
        !rejected.HasValue(),
        rejected.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::PhysicalBootstrapActivationRejected,
        admission.HasValue(), admission.GetValue().State == Ia64BspRuntimeApplicationProcessorAdmissionState::Failed,
        admission.GetValue().Failure.PhysicalBootstrapActivationError ==
            AddressSpacePageTableActivationError::CommittedStateUnknown,
        admission.GetValue().Failure.InterruptVectorTableCommitted,
        admission.GetValue().Failure.PhysicalRootMayBeActive,
        admission.GetValue().Controller.State == ProcessorAddressSpaceControllerState::Uninitialized,
        failed.State == Ia64BspRuntimeMemoryState::Failed, failed.AddressSpace.State == AddressSpaceState::Ready,
        failed.AddressSpace.ResidentProcessorMask == 0x01U, failed.Bindings[0U] == Ia64BspRuntimeBindingState::Active,
        failed.Bindings[1U] == Ia64BspRuntimeBindingState::AdmittedWithoutLedger, !borrowed.HasValue(),
        borrowed.GetError() == Ia64BspRuntimeRootBorrowError::NotReady, !failedApplicationProcessorBorrow.HasValue(),
        failedApplicationProcessorBorrow.GetError() ==
            Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed);
    ASTERIA_EXPECT(
        runner, "the uncertain AP transaction performs exactly one committed IVT install and one uncertain activation",
        g_installCallCount == 2U, g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));

    const auto repeated = owner.AdmitApplicationProcessorPhysicalRoot(applicationProcessor);
    ASTERIA_EXPECT(runner,
                   "a repeated AP call after native uncertainty cannot pretend rollback or issue another native call",
                   !repeated.HasValue(),
                   repeated.GetError() == Ia64BspRuntimeApplicationProcessorAdmissionError::PreviousAdmissionFailed,
                   g_installCallCount == 2U, g_activationCallCount == 2U, NativeEventsAlternateExactly(2U));
}
} // namespace
} // namespace Asteria::Test

int main(const int argc, char* const argv[])
{
    if ((argc != 2) || (argv[1] == nullptr))
    {
        static_cast<void>(std::fprintf(stderr, "usage: Ia64BspRuntimeMemoryTests "
                                               "<success|invalid-charge-owner|activation-rejected|"
                                               "activation-unknown|destroyed-root>\n"));
        return 1;
    }

    TestContext runner{"Ia64BspRuntimeMemoryTests"};
    const bool prepared = Asteria::Test::PrepareFixture();
    ASTERIA_EXPECT(runner, "real initial address-space fixture covers the page-aligned runtime-owner arena", prepared);
    if (prepared)
    {
        if (std::strcmp(argv[1], "success") == 0)
        {
            Asteria::Test::TestRetryablePreflightAndSuccess(runner);
            Asteria::Test::TestNonzeroBspApplicationProcessorRootBorrows(runner);
            Asteria::Test::TestPostPreflightCapabilityFailure(runner);
            Asteria::Test::TestApplicationProcessorPhysicalRootAdmissions(runner);
            Asteria::Test::TestConfiguredPageFaultOwnerMismatchIsTerminal(runner);
        }
        else if (std::strcmp(argv[1], "invalid-charge-owner") == 0)
        {
            Asteria::Test::TestProfileMaskMustContainBsp(runner);
            Asteria::Test::TestInvalidChargeOwner(runner);
        }
        else if (std::strcmp(argv[1], "activation-rejected") == 0)
        {
            Asteria::Test::TestActivationRejected(runner);
            Asteria::Test::TestApplicationProcessorActivationRejected(runner);
            Asteria::Test::TestReadyApplicationProcessorBorrowAfterLaterInstallRejection(runner);
        }
        else if (std::strcmp(argv[1], "activation-unknown") == 0)
        {
            Asteria::Test::TestActivationUnknown(runner);
            Asteria::Test::TestApplicationProcessorActivationUnknown(runner);
        }
        else if (std::strcmp(argv[1], "destroyed-root") == 0)
        {
            Asteria::Test::TestDestroyedRootIdentityRejectsApplicationProcessorAdmission(runner);
        }
        else
        {
            ASTERIA_EXPECT(runner, "unknown IA-64 BSP runtime-memory test mode", false);
        }
    }
    return runner.Finish(argv[1]);
}
