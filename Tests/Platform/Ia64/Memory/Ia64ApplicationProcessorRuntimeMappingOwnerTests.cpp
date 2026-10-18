// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#include "Boot/BootInfo.hpp"
#include "Hal/Memory/ILocalTlbInvalidator.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/ObjectPin.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Interrupt/Ia64LocalSapicIpiController.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStorageOwner.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeMappingOwner.hpp"
#include "Platform/Ia64/Memory/Ia64ApplicationProcessorRuntimeWorkerStorageOwner.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

extern "C" void AsteriaIa64WriteProcessorInterruptBlock(Asteria::Foundation::UInt64,
                                                        Asteria::Foundation::UInt64) noexcept
{
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InstallRuntimeInterruptVectorTable(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64) noexcept
{
    return 0U;
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
    return 0U;
}

namespace Asteria::Test
{
namespace
{
using Boot::Acpi::AcpiProcessorControllerKind;
using Boot::Acpi::AcpiProcessorTopology;
using Boot::Acpi::AcpiTopology;
using Hal::Memory::ILocalTlbInvalidator;
using Hal::Memory::LocalTlbInvalidationError;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameKind;
using Kernel::Memory::FrameState;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectState;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiController;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerInitializationError;
using Platform::Ia64::Interrupt::Ia64LocalSapicIpiControllerSnapshot;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockAccess;
using Platform::Ia64::Interrupt::Ia64ProcessorInterruptBlockBaseKind;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwner;
using Platform::Ia64::Memory::ApplicationProcessorExecutionStorageOwnerState;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeFirstGuardSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeIdleAliasOffset;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeIdleAliasSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeLayoutPlanner;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingAccessError;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwner;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwnerError;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeMappingOwnerState;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeSecondGuardSizeBytes;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerAliasOffset;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageOwner;
using Platform::Ia64::Memory::Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState;
using Platform::Ia64::Memory::Ia64BspRuntimeMemory;
using Platform::Ia64::Memory::Ia64BspRuntimeApplicationProcessorAdmissionError;
using Platform::Ia64::Memory::Ia64BspRuntimeApplicationProcessorAdmissionState;
using Platform::Ia64::Memory::Ia64BspRuntimeApplicationProcessorPostconditionError;
using Platform::Ia64::Memory::Ia64BspRuntimeMemoryState;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::TranslationCapabilities;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;
using Platform::Synchronization::SpinLock;

constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 ChargeOwnerValue{0x4A64U};
constexpr Foundation::UInt64 PhysicalAddressUncachedBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PibPhysicalBase{0x0000'0000'FEE0'0000ULL};
constexpr Foundation::UInt64 PibAccessBase{PibPhysicalBase | PhysicalAddressUncachedBit};
constexpr Foundation::UInt32 EnabledProcessorFlag{1U};
constexpr Foundation::UInt32 FirmwareDescriptorSize{40U};
constexpr Foundation::Size ApplicationProcessorStoragePages{128U};

struct RuntimeArena
{
    Ia64BspRuntimeMemory Owner;
};

constinit SpinLock g_registrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_registry{g_registrySynchronization};
constinit RootCapabilityIssuer g_issuer{g_registry};
constinit SpinLock g_otherRegistrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_otherRegistry{g_otherRegistrySynchronization};
constinit RootCapabilityIssuer g_otherIssuer{g_otherRegistry};
constinit BootstrapPageAllocator g_allocator;
// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<RuntimeArena> g_runtimeArenaStorage{};
RuntimeArena& g_runtimeArena = g_runtimeArenaStorage.Construct();
constinit Ia64ApplicationProcessorRuntimeMappingOwner g_issuerFailureOwner;
constinit Ia64ApplicationProcessorRuntimeMappingOwner g_preflightFailureOwner;
constinit Ia64ApplicationProcessorRuntimeMappingOwner g_mappingOwner;
constinit ApplicationProcessorExecutionStorageOwner g_executionStorageOwner;
constinit Ia64ApplicationProcessorRuntimeWorkerStorageOwner g_workerStorageOwner;
alignas(32U * 1024U) constinit Foundation::UInt8
    g_applicationProcessorStorage[ApplicationProcessorStoragePages * PageSizeBytes]{};
constinit InitialAddressSpacePreparation g_preparation;
alignas(32U * 1024U) constinit Foundation::UInt8 g_initialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageSizeBytes) constinit Foundation::UInt8 g_initialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
constinit ResourceDomainLedger g_resourceDomainLedger;
constinit ResourceDomain g_resourceDomain{g_resourceDomainLedger};

static_assert(alignof(RuntimeArena) == PageSizeBytes);
static_assert((sizeof(RuntimeArena) & (PageSizeBytes - 1U)) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(RuntimeArena)) == 0x40'0000U);
static_assert(alignof(Ia64BspRuntimeMemory) == PageSizeBytes);
static_assert((sizeof(Ia64BspRuntimeMemory) & (PageSizeBytes - 1U)) == 0U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingOwnerState) == 1U);
static_assert(sizeof(Ia64ApplicationProcessorRuntimeMappingOwnerError) == 4U);

class RecordingInvalidator final : public ILocalTlbInvalidator
{
public:
    [[nodiscard]] LocalTlbInvalidationError Invalidate(const VirtualAddress start,
                                                       const PageCount pages) noexcept override
    {
        if (CallCount < 6U)
        {
            Starts[CallCount] = start;
            Pages[CallCount] = pages;
        }
        ++CallCount;
        return Failure;
    }

    LocalTlbInvalidationError Failure{LocalTlbInvalidationError::None};
    Foundation::Size CallCount{0U};
    VirtualAddress Starts[6U]{};
    PageCount Pages[6U]{};
};

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr Foundation::UInt8 ExpectedMask(const Foundation::UInt8 processorCount) noexcept
{
    return static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << processorCount) - 1U);
}

[[nodiscard]] constexpr Foundation::UInt64 MakeCrLid(const Foundation::UInt8 hardwareId,
                                                     const Foundation::UInt8 hardwareEid) noexcept
{
    return (static_cast<Foundation::UInt64>(hardwareId) << 24U) | (static_cast<Foundation::UInt64>(hardwareEid) << 16U);
}

[[nodiscard]] constexpr AcpiProcessorTopology MakeProcessor(const Foundation::UInt32 acpiUid,
                                                            const Foundation::UInt8 hardwareId,
                                                            const Foundation::UInt8 hardwareEid) noexcept
{
    return AcpiProcessorTopology{
        .AcpiUid = acpiUid,
        .HardwareId = hardwareId,
        .HardwareEid = hardwareEid,
        .Flags = EnabledProcessorFlag,
        .ControllerKind = AcpiProcessorControllerKind::LocalSapic,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool MakeProcessorSnapshot(const Foundation::UInt8 processorCount,
                                         Ia64LocalSapicIpiControllerSnapshot& snapshot) noexcept
{
    AcpiTopology topology{};
    topology.LocalControllerPhysicalAddress = PibPhysicalBase;
    topology.ProcessorCount = processorCount;
    for (Foundation::UInt8 index{0U}; index < processorCount; ++index)
    {
        topology.Processors[index] = MakeProcessor(100U + index, static_cast<Foundation::UInt8>(0x10U + index),
                                                   static_cast<Foundation::UInt8>(0x20U + index));
    }
    Ia64LocalSapicIpiController controller;
    const Ia64ProcessorInterruptBlockAccess access{
        .BaseAddress = PibAccessBase,
        .Kind = Ia64ProcessorInterruptBlockBaseKind::BootstrapPhysical,
        .Reserved = {},
    };
    if (controller.Initialize(topology, MakeCrLid(0x10U, 0x20U), access) !=
        Ia64LocalSapicIpiControllerInitializationError::None)
    {
        return false;
    }
    snapshot = controller.GetSnapshot();
    return true;
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

struct FixtureState
{
    ResourceDomainObjectId ResourceDomainIdentity;
    Foundation::Size InitialRegistryCount;
    Foundation::Size InitialMappingCount;
    Kernel::Resource::ResourceDomainSnapshot InitialResourceDomain;
};

[[nodiscard]] bool PrepareFixture(const Foundation::UInt8 processorCount, FixtureState& state) noexcept
{
    const Kernel::Memory::PhysicalPageRange bootstrapReservation{
        .PhysicalStart = ReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    const Foundation::UInt64 apStorageStart = AddressOf(g_applicationProcessorStorage);
    if (((apStorageStart & ((32U * 1024U) - 1U)) != 0U) ||
        !TestSupport::MakeIa64HostedBootGeometry(
            AddressOf(&Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage), AddressOf(&g_runtimeArena),
            sizeof(g_runtimeArena), bootstrapReservation, geometry))
    {
        return false;
    }

    Boot::BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(Boot::BootInfo),
        .MemoryRegionCount = 4U,
        .FirmwareDescriptorVersion = 1U,
        .FirmwareDescriptorSize = FirmwareDescriptorSize,
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
    bootInfo.MemoryRegions[0U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = ReservedBase,
        .VirtualStart = 0U,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[1U] = geometry.ImageRegion;
    bootInfo.MemoryRegions[2U] = geometry.RuntimeMemoryRegion;
    bootInfo.MemoryRegions[3U] = Boot::PhysicalMemoryRegion{
        .PhysicalStart = apStorageStart,
        .VirtualStart = 0U,
        .PageCount = ApplicationProcessorStoragePages,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = Boot::PhysicalMemoryKind::Conventional,
    };
    if (!TestSupport::SortIa64HostedBootRegions(bootInfo) ||
        (g_allocator.Initialize(bootInfo) != BootstrapPageAllocatorError::None))
    {
        return false;
    }
    const auto reserved = g_allocator.Allocate(BootstrapReservedPageCount, 1U);
    // The boot allocates the run-time memory reservation right after the
    // bootstrap one, aligned to its size; here that is the arena's page.
    const auto runtimeReservation =
        g_allocator.Allocate(geometry.RuntimeMemoryPages.PageCount, geometry.RuntimeMemoryPages.PageCount);
    if (!reserved.HasValue() || !runtimeReservation.HasValue() ||
        !TestSupport::Ia64HostedPagesEqual(runtimeReservation.GetValue(), geometry.RuntimeMemoryPages))
    {
        return false;
    }
    const auto prepared = g_preparation.Prepare(geometry.Image, bootInfo, reserved.GetValue(),
                                                runtimeReservation.GetValue(), Capabilities(), Firmware(), Regions(),
                                                WritableInterruptVectorTableView{
                                                    .Data = g_initialInterruptVectorTable,
                                                    .SizeBytes = InitialInterruptVectorTableSizeBytes,
                                                    .PhysicalStart = ReservedBase + 0x1'0000U,
                                                },
                                                WritableRseBackingStoreView{
                                                    .Data = g_initialRseBackingStore,
                                                    .SizeBytes = InitialRseBackingStoreSizeBytes,
                                                    .PhysicalStart = ReservedBase + 0x1'8000U,
                                                });
    if (!prepared.HasValue())
    {
        return false;
    }
    // The hosted geometry's plan is the same at every host layout.
    const auto summary = g_preparation.TryGetSummary();
    if (!summary.HasValue() ||
        (summary.GetValue()->Translation.DataRegisterCount != TestSupport::Ia64HostedDataTranslationRegisters) ||
        (summary.GetValue()->Translation.InstructionRegisterCount !=
         TestSupport::Ia64HostedInstructionTranslationRegisters))
    {
        return false;
    }

    const ResourceDomainLimits limits{
        .CommittedMemoryPages = 4096U,
        .ResidentMemoryPages = 4096U,
        .KernelMetadataBytes = 4U * 1024U * 1024U,
        .CapabilityHandles = 4096U,
        .IpcQueueBytes = 4U * 1024U * 1024U,
        .Threads = 256U,
        .ChildDomains = 64U,
    };
    const auto domainInitialized = g_resourceDomain.InitializeRoot(ResourceDomainId{ChargeOwnerValue}, limits);
    if (domainInitialized.Error != ResourceDomainError::None)
    {
        return false;
    }
    const auto domainRegistered = g_issuer.CreateObject(g_resourceDomain);
    if (!domainRegistered.HasValue())
    {
        return false;
    }
    state.ResourceDomainIdentity = ResourceDomainObjectId{g_registry, domainRegistered.GetValue()};

    PalVirtualMemoryCapabilities capabilities[Kernel::Scheduler::MaximumProcessorCount]{};
    for (Foundation::UInt8 index{0U}; index < processorCount; ++index)
    {
        capabilities[index] = Capabilities();
    }
    const auto runtimePrepared =
        g_runtimeArena.Owner.PrepareAndAdmit(CpuId{0U}, ExpectedMask(processorCount), capabilities, g_preparation,
                                             ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    if (!runtimePrepared.HasValue() || (g_runtimeArena.Owner.GetSnapshot().State != Ia64BspRuntimeMemoryState::Ready))
    {
        return false;
    }

    Ia64LocalSapicIpiControllerSnapshot processorSnapshot{};
    if (!MakeProcessorSnapshot(processorCount, processorSnapshot) ||
        !g_executionStorageOwner.Prepare(g_allocator, processorSnapshot).HasValue() ||
        (g_executionStorageOwner.GetState() != ApplicationProcessorExecutionStorageOwnerState::Ready) ||
        !g_workerStorageOwner.Prepare(g_allocator, g_executionStorageOwner).HasValue() ||
        (g_workerStorageOwner.GetState() != Ia64ApplicationProcessorRuntimeWorkerStorageOwnerState::Ready))
    {
        return false;
    }

    const auto root = g_runtimeArena.Owner.BorrowReadyRoot();
    if (!root.HasValue() || (root.GetValue().AddressSpaceStorage == nullptr))
    {
        return false;
    }
    state.InitialRegistryCount = g_registry.GetActiveCount();
    state.InitialMappingCount = root.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount;
    state.InitialResourceDomain = g_resourceDomain.GetSnapshot();
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size count) noexcept
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

[[nodiscard]] bool RecordIsExact(const AddressSpaceMappingRecord& record, const Foundation::UInt64 virtualStart,
                                 const Foundation::UInt64 physicalStart, const Foundation::UInt64 pages,
                                 const Kernel::Memory::MemoryObjectId memoryObject,
                                 const Kernel::Memory::FrameId frame) noexcept
{
    return record.MemoryObject.IsEqualTo(memoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(frame) && (record.VirtualStart.GetValue() == virtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == pages) &&
           (record.MemoryObjectOffsetPages == 0U) && record.Permissions.Contains(PagePermission::Read) &&
           record.Permissions.Contains(PagePermission::Write) &&
           !record.Permissions.Contains(PagePermission::Execute) &&
           !record.Permissions.Contains(PagePermission::User) && (record.CachePolicy == MemoryCachePolicy::WriteBack) &&
           (record.Kind == FrameKind::Ram) && (record.Commitment == MemoryCommitment::Pinned) &&
           record.RuntimeMapping && (record.Reserved[0U] == 0U) && (record.Reserved[1U] == 0U) &&
           (record.Reserved[2U] == 0U);
}

[[nodiscard]] bool RegisteredFrameIsRetained(const Kernel::Memory::FrameId identity,
                                             const Foundation::UInt64 physicalStart,
                                             const Foundation::UInt64 pages) noexcept
{
    ObjectPin pin;
    const ObjectPinError pinError = g_registry.PinObject(identity.GetObjectId(), ObjectType::Frame, pin);
    if ((pinError != ObjectPinError::Ok) || !pin.IsValid())
    {
        return false;
    }
    const Frame& frame = static_cast<const Frame&>(pin.GetObject());
    const auto snapshot = frame.GetSnapshot();
    return (snapshot.State == FrameState::Ready) && (snapshot.Descriptor.PhysicalStart.GetValue() == physicalStart) &&
           (snapshot.Descriptor.Pages.GetValue() == pages) &&
           (snapshot.Descriptor.CachePolicy == MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.Kind == FrameKind::Ram);
}

[[nodiscard]] bool RegisteredMemoryObjectIsRetained(const Kernel::Memory::MemoryObjectId identity,
                                                    const Foundation::UInt64 pages,
                                                    const ResourceDomainObjectId resourceDomainIdentity) noexcept
{
    ObjectPin pin;
    const ObjectPinError pinError = g_registry.PinObject(identity.GetObjectId(), ObjectType::MemoryObject, pin);
    if ((pinError != ObjectPinError::Ok) || !pin.IsValid())
    {
        return false;
    }
    const MemoryObject& memoryObject = static_cast<const MemoryObject&>(pin.GetObject());
    const auto snapshot = memoryObject.GetSnapshot();
    return (snapshot.State == MemoryObjectState::Ready) && (snapshot.Descriptor.Pages.GetValue() == pages) &&
           (snapshot.Descriptor.CachePolicy == MemoryCachePolicy::WriteBack) &&
           (snapshot.Descriptor.Commitment == MemoryCommitment::Pinned) &&
           snapshot.Descriptor.MaximumPermissions.Contains(PagePermission::Read) &&
           snapshot.Descriptor.MaximumPermissions.Contains(PagePermission::Write) &&
           !snapshot.Descriptor.MaximumPermissions.Contains(PagePermission::User) &&
           !snapshot.Descriptor.MaximumPermissions.Contains(PagePermission::Execute) &&
           snapshot.ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity) && (snapshot.ActiveMappings == 1U);
}

void TestMappingOwner(TestContext& runner, const Foundation::UInt8 processorCount) noexcept
{
    FixtureState fixture{};
    ASTERIA_EXPECT(runner, "SMP fixture prepares one BSP-only runtime and exact AP storage owners",
                   PrepareFixture(processorCount, fixture));

    const auto initialRoot = g_runtimeArena.Owner.BorrowReadyRoot();
    ASTERIA_EXPECT(runner, "fixture exposes the exact BSP-bound ready root", initialRoot.HasValue(),
                   initialRoot.GetValue().Processor.GetValue() == 0U);
    if (!initialRoot.HasValue())
    {
        return;
    }
    Kernel::Memory::AddressSpace& addressSpace = *initialRoot.GetValue().AddressSpaceStorage;

    RecordingInvalidator invalidator;
    TlbShootdownDispatcher dispatcher{CpuId{0U}, invalidator};
    const auto issuerRejected =
        g_issuerFailureOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                                     g_otherIssuer, fixture.ResourceDomainIdentity, g_resourceDomain, dispatcher);
    const auto issuerRejectedSnapshot = g_issuerFailureOwner.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "issuer/registry mismatch is rejected before every persistent side effect",
        g_issuer.IsFromRegistry(g_registry), !g_otherIssuer.IsFromRegistry(g_registry), !issuerRejected.HasValue(),
        issuerRejected.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::IssuerRegistryMismatch,
        issuerRejectedSnapshot.State == Ia64ApplicationProcessorRuntimeMappingOwnerState::Failed,
        issuerRejectedSnapshot.InitializedFrameCount == 0U, issuerRejectedSnapshot.InitializedMemoryObjectCount == 0U,
        issuerRejectedSnapshot.RegisteredObjectCount == 0U, issuerRejectedSnapshot.RemotePublishAttemptCount == 0U,
        g_registry.GetActiveCount() == fixture.InitialRegistryCount, g_otherRegistry.GetActiveCount() == 0U,
        addressSpace.GetSnapshot().MappingCount == fixture.InitialMappingCount,
        g_resourceDomain.GetSnapshot().Usage.CommittedMemoryPages ==
            fixture.InitialResourceDomain.Usage.CommittedMemoryPages,
        invalidator.CallCount == 0U);
    const auto issuerRejectedRepeat =
        g_issuerFailureOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                                     g_otherIssuer, fixture.ResourceDomainIdentity, g_resourceDomain, dispatcher);
    ASTERIA_EXPECT(
        runner, "issuer/registry mismatch is terminal and retains its diagnostic", !issuerRejectedRepeat.HasValue(),
        issuerRejectedRepeat.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::AlreadyPrepared,
        g_issuerFailureOwner.GetSnapshot().Failure.Error ==
            Ia64ApplicationProcessorRuntimeMappingOwnerError::IssuerRegistryMismatch);

    TlbShootdownDispatcher wrongDispatcher{CpuId{1U}, invalidator};
    const auto rejected =
        g_preflightFailureOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                                        g_issuer, fixture.ResourceDomainIdentity, g_resourceDomain, wrongDispatcher);
    const auto rejectedSnapshot = g_preflightFailureOwner.GetSnapshot();
    ASTERIA_EXPECT(runner, "dispatcher mismatch is a terminal preflight rejection with zero persistent side effects",
                   !rejected.HasValue(),
                   rejected.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::InvalidDispatcher,
                   rejectedSnapshot.State == Ia64ApplicationProcessorRuntimeMappingOwnerState::Failed,
                   rejectedSnapshot.InitializedFrameCount == 0U, rejectedSnapshot.InitializedMemoryObjectCount == 0U,
                   rejectedSnapshot.RegisteredObjectCount == 0U, rejectedSnapshot.RemotePublishAttemptCount == 0U,
                   g_registry.GetActiveCount() == fixture.InitialRegistryCount,
                   addressSpace.GetSnapshot().MappingCount == fixture.InitialMappingCount, invalidator.CallCount == 0U);
    const auto rejectedRepeat =
        g_preflightFailureOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                                        g_issuer, fixture.ResourceDomainIdentity, g_resourceDomain, wrongDispatcher);
    ASTERIA_EXPECT(runner, "failed owner is single-use and retains the original diagnostic", !rejectedRepeat.HasValue(),
                   rejectedRepeat.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::AlreadyPrepared,
                   g_preflightFailureOwner.GetSnapshot().Failure.Error ==
                       Ia64ApplicationProcessorRuntimeMappingOwnerError::InvalidDispatcher);

    const auto prepared =
        g_mappingOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                               g_issuer, fixture.ResourceDomainIdentity, g_resourceDomain, dispatcher);
    const Foundation::UInt8 applicationProcessorCount = static_cast<Foundation::UInt8>(processorCount - 1U);
    const Foundation::Size expectedMappings = static_cast<Foundation::Size>(applicationProcessorCount) * 2U;
    const Foundation::Size expectedFrames =
        applicationProcessorCount == 0U ? 0U : static_cast<Foundation::Size>(applicationProcessorCount) + 1U;
    const Foundation::Size expectedObjects = expectedFrames + expectedMappings;
    ASTERIA_EXPECT(runner, "valid SMP topology publishes Ready only after the complete mapping set",
                   prepared.HasValue(),
                   g_mappingOwner.GetState() == Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready);
    if (!prepared.HasValue())
    {
        return;
    }

    const auto summaryResult = g_mappingOwner.GetSummary();
    const auto planResult = g_mappingOwner.GetPlan();
    ASTERIA_EXPECT(
        runner, "Ready accessors retain the exact planner output and ownership counts", summaryResult.HasValue(),
        planResult.HasValue(), summaryResult.GetValue().ProcessorCount == processorCount,
        summaryResult.GetValue().EnabledProcessorMask == ExpectedMask(processorCount),
        summaryResult.GetValue().ApplicationProcessorCount == applicationProcessorCount,
        summaryResult.GetValue().InstalledMappingCount == expectedMappings,
        summaryResult.GetValue().OwnedFrameCount == expectedFrames,
        summaryResult.GetValue().OwnedMemoryObjectCount == expectedMappings,
        planResult.GetValue().LayoutCount == applicationProcessorCount, summaryResult.GetValue().Reserved == 0U);
    if (!summaryResult.HasValue() || !planResult.HasValue())
    {
        return;
    }
    const auto summary = summaryResult.GetValue();
    const auto plan = planResult.GetValue();
    const auto ownerSnapshot = g_mappingOwner.GetSnapshot();
    const auto finalRoot = addressSpace.GetSnapshot();
    const auto finalResource = g_resourceDomain.GetSnapshot();
    const Foundation::UInt64 expectedResourcePages = static_cast<Foundation::UInt64>(applicationProcessorCount) * 24U;
    const Foundation::UInt64 expectedMetadataBytes =
        static_cast<Foundation::UInt64>(expectedMappings) * sizeof(MemoryObject);
    ASTERIA_EXPECT(runner, "final root remains BSP-only with no pending transition or remote shootdown",
                   summary.InitialMappingCount == fixture.InitialMappingCount,
                   summary.FinalMappingCount == (fixture.InitialMappingCount + expectedMappings),
                   finalRoot.MappingCount == summary.FinalMappingCount, finalRoot.State == AddressSpaceState::Ready,
                   finalRoot.ResidentProcessorMask == 0x01U, finalRoot.TargetProcessorMask == 0U,
                   finalRoot.AcknowledgedProcessorMask == 0U, !finalRoot.ActivatingProcessor.IsValid(),
                   !finalRoot.TransitionProcessor.IsValid(), ownerSnapshot.RemotePublishAttemptCount == 0U,
                   BytesAreZero(ownerSnapshot.Failure.Reserved, sizeof(ownerSnapshot.Failure.Reserved)),
                   BytesAreZero(ownerSnapshot.Reserved, sizeof(ownerSnapshot.Reserved)),
                   invalidator.CallCount == expectedMappings);
    ASTERIA_EXPECT(runner, "registry and root resource domain retain exactly the owned generations and pins",
                   g_registry.GetActiveCount() == (fixture.InitialRegistryCount + expectedObjects),
                   ownerSnapshot.RegisteredObjectCount == expectedObjects,
                   finalResource.Usage.CommittedMemoryPages ==
                       (fixture.InitialResourceDomain.Usage.CommittedMemoryPages + expectedResourcePages),
                   finalResource.Usage.ResidentMemoryPages ==
                       (fixture.InitialResourceDomain.Usage.ResidentMemoryPages + expectedResourcePages),
                   finalResource.Usage.KernelMetadataBytes ==
                       (fixture.InitialResourceDomain.Usage.KernelMetadataBytes + expectedMetadataBytes));

    if (applicationProcessorCount == 0U)
    {
        ASTERIA_EXPECT(runner, "SMP1 is the canonical zero-AP result with no owned objects or mapping work",
                       !summary.WorkerFrame.IsValid(), ownerSnapshot.InitializedFrameCount == 0U,
                       ownerSnapshot.InitializedMemoryObjectCount == 0U, ownerSnapshot.RegisteredObjectCount == 0U,
                       summary.InitialMappingCount == summary.FinalMappingCount,
                       g_mappingOwner.GetOwnership(CpuId{1U}).GetError() ==
                           Ia64ApplicationProcessorRuntimeMappingAccessError::InvalidProcessor);
    }

    Foundation::Size installedIndex{0U};
    for (Foundation::Size index{0U}; index < applicationProcessorCount; ++index)
    {
        const auto ownershipResult = g_mappingOwner.GetOwnership(CpuId{static_cast<Foundation::UInt8>(index + 1U)});
        ASTERIA_EXPECT(runner, "each selected AP has one immutable Ready ownership record", ownershipResult.HasValue());
        if (!ownershipResult.HasValue())
        {
            continue;
        }
        const auto ownership = ownershipResult.GetValue();
        const auto& layout = plan.Processors[index];
        const auto idleRecord = addressSpace.TryGetMapping(fixture.InitialMappingCount + installedIndex);
        ++installedIndex;
        const auto workerRecord = addressSpace.TryGetMapping(fixture.InitialMappingCount + installedIndex);
        ++installedIndex;
        ASTERIA_EXPECT(runner,
                       "mapping order, addresses, permissions, object identities, and shared worker Frame are exact",
                       idleRecord.HasValue(), workerRecord.HasValue(),
                       RecordIsExact(idleRecord.GetValue(),
                                     layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeIdleAliasOffset,
                                     layout.IdlePhysicalBase, 16U, ownership.IdleMemoryObject, ownership.IdleFrame),
                       RecordIsExact(workerRecord.GetValue(),
                                     layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeWorkerAliasOffset,
                                     layout.WorkerPhysicalBase, Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
                                     ownership.WorkerMemoryObject, summary.WorkerFrame),
                       BytesAreZero(ownership.Reserved, sizeof(ownership.Reserved)));
        ASTERIA_EXPECT(
            runner, "all Frame and MemoryObject generations remain registered, pinned, RAM/WB, and Ready",
            RegisteredFrameIsRetained(ownership.IdleFrame, layout.IdlePhysicalBase, 16U),
            RegisteredFrameIsRetained(summary.WorkerFrame, plan.Processors[0U].WorkerPhysicalBase,
                                      static_cast<Foundation::UInt64>(applicationProcessorCount) *
                                          Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor),
            RegisteredMemoryObjectIsRetained(ownership.IdleMemoryObject, 16U, fixture.ResourceDomainIdentity),
            RegisteredMemoryObjectIsRetained(ownership.WorkerMemoryObject,
                                             Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor,
                                             fixture.ResourceDomainIdentity));
        ASTERIA_EXPECT(runner, "receipts and local invalidations are exact and consecutive for idle then worker",
                       ownership.IdleMapping.MappingCount == (fixture.InitialMappingCount + (2U * index) + 1U),
                       ownership.WorkerMapping.MappingCount == (fixture.InitialMappingCount + (2U * index) + 2U),
                       ownership.WorkerMapping.MappingSequence == (ownership.IdleMapping.MappingSequence + 1U),
                       invalidator.Starts[2U * index].GetValue() == layout.SlotVirtualBase,
                       invalidator.Pages[2U * index].GetValue() == 16U,
                       invalidator.Starts[(2U * index) + 1U].GetValue() ==
                           layout.SlotVirtualBase + Ia64ApplicationProcessorRuntimeWorkerAliasOffset,
                       invalidator.Pages[(2U * index) + 1U].GetValue() ==
                           Ia64ApplicationProcessorRuntimeWorkerPagesPerProcessor);
    }

    bool guardsVacant{true};
    for (Foundation::Size recordIndex{0U}; recordIndex < finalRoot.MappingCount; ++recordIndex)
    {
        const auto recordResult = addressSpace.TryGetMapping(recordIndex);
        if (!recordResult.HasValue())
        {
            guardsVacant = false;
            continue;
        }
        const auto& record = recordResult.GetValue();
        const Foundation::UInt64 recordEnd = record.VirtualStart.GetValue() + (record.Pages.GetValue() * PageSizeBytes);
        for (Foundation::Size layoutIndex{0U}; layoutIndex < applicationProcessorCount; ++layoutIndex)
        {
            const auto& layout = plan.Processors[layoutIndex];
            const Foundation::UInt64 firstGuard =
                Ia64ApplicationProcessorRuntimeLayoutPlanner::GetFirstGuardVirtualStart(layout);
            const Foundation::UInt64 secondGuard =
                Ia64ApplicationProcessorRuntimeLayoutPlanner::GetSecondGuardVirtualStart(layout);
            guardsVacant = guardsVacant &&
                           !RangesOverlap(record.VirtualStart.GetValue(), recordEnd, firstGuard,
                                          firstGuard + Ia64ApplicationProcessorRuntimeFirstGuardSizeBytes) &&
                           !RangesOverlap(record.VirtualStart.GetValue(), recordEnd, secondGuard,
                                          secondGuard + Ia64ApplicationProcessorRuntimeSecondGuardSizeBytes);
        }
    }
    ASTERIA_EXPECT(runner,
                   "both 16-KiB guards per AP remain wholly absent and the ledger ends exactly at the final count",
                   guardsVacant, !addressSpace.TryGetMapping(finalRoot.MappingCount).HasValue());

    const Foundation::Size countBeforeRepeat = addressSpace.GetSnapshot().MappingCount;
    const auto repeat =
        g_mappingOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                               g_issuer, fixture.ResourceDomainIdentity, g_resourceDomain, dispatcher);
    ASTERIA_EXPECT(runner, "repeat preparation cannot change a published Ready owner or root", !repeat.HasValue(),
                   repeat.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::AlreadyPrepared,
                   g_mappingOwner.GetState() == Ia64ApplicationProcessorRuntimeMappingOwnerState::Ready,
                   addressSpace.GetSnapshot().MappingCount == countBeforeRepeat,
                   invalidator.CallCount == expectedMappings);

    bool admissionsExact{true};
    for (Foundation::UInt8 processor{1U}; processor < processorCount; ++processor)
    {
        const CpuId applicationProcessor{processor};
        const auto admitted = g_runtimeArena.Owner.AdmitApplicationProcessorPhysicalRoot(applicationProcessor);
        const auto admission = g_runtimeArena.Owner.GetApplicationProcessorAdmissionSnapshot(applicationProcessor);
        const Foundation::ProcessorMask expectedResidentMask =
            static_cast<Foundation::UInt8>((Foundation::UInt32{1U} << (processor + 1U)) - 1U);
        admissionsExact =
            admissionsExact && admitted.HasValue() && admission.HasValue() &&
            (admission.GetValue().State == Ia64BspRuntimeApplicationProcessorAdmissionState::Ready) &&
            (admission.GetValue().Failure.Error == Ia64BspRuntimeApplicationProcessorAdmissionError::None) &&
            (admission.GetValue().Failure.PostconditionError ==
             Ia64BspRuntimeApplicationProcessorPostconditionError::None) &&
            (admission.GetValue().Summary.ResidentProcessorMask == expectedResidentMask) &&
            (addressSpace.GetSnapshot().ResidentProcessorMask == expectedResidentMask) &&
            (addressSpace.GetSnapshot().MappingCount == summary.FinalMappingCount);
    }
    ASTERIA_EXPECT(
        runner,
        "AP root admission preserves the complete runtime mapping set and advances residency in processor order",
        admissionsExact);
}

void TestShootdownFailureRetention(TestContext& runner) noexcept
{
    FixtureState fixture{};
    ASTERIA_EXPECT(runner, "shootdown-failure fixture prepares the SMP2 BSP-only root", PrepareFixture(2U, fixture));
    const auto root = g_runtimeArena.Owner.BorrowReadyRoot();
    if (!root.HasValue())
    {
        ASTERIA_EXPECT(runner, "shootdown-failure fixture exposes its root", false);
        return;
    }

    RecordingInvalidator invalidator;
    invalidator.Failure = LocalTlbInvalidationError::NotReady;
    TlbShootdownDispatcher dispatcher{CpuId{0U}, invalidator};
    const auto prepared =
        g_mappingOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                               g_issuer, fixture.ResourceDomainIdentity, g_resourceDomain, dispatcher);
    const auto snapshot = g_mappingOwner.GetSnapshot();
    const auto rootSnapshot = root.GetValue().AddressSpaceStorage->GetSnapshot();
    ASTERIA_EXPECT(runner, "local shootdown rejection faults the root and retains every prior persistent side effect",
                   !prepared.HasValue(),
                   prepared.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::ShootdownRejected,
                   prepared.GetError().RootMayBeFaulted,
                   snapshot.State == Ia64ApplicationProcessorRuntimeMappingOwnerState::RootFaulted,
                   rootSnapshot.State == AddressSpaceState::Faulted, snapshot.InitializedFrameCount == 2U,
                   snapshot.InitializedMemoryObjectCount == 2U, snapshot.RegisteredObjectCount == 4U,
                   g_registry.GetActiveCount() == (fixture.InitialRegistryCount + 4U), invalidator.CallCount == 1U);
    const auto repeat =
        g_mappingOwner.Prepare(g_runtimeArena.Owner, g_executionStorageOwner, g_workerStorageOwner, g_registry,
                               g_issuer, fixture.ResourceDomainIdentity, g_resourceDomain, dispatcher);
    ASTERIA_EXPECT(runner, "root-faulted owner is terminal and retains its original diagnostic", !repeat.HasValue(),
                   repeat.GetError().Error == Ia64ApplicationProcessorRuntimeMappingOwnerError::AlreadyPrepared,
                   g_mappingOwner.GetSnapshot().Failure.Error ==
                       Ia64ApplicationProcessorRuntimeMappingOwnerError::ShootdownRejected,
                   invalidator.CallCount == 1U);
}

[[nodiscard]] Foundation::UInt8 ParseProcessorCount(const int argumentCount, char** const arguments) noexcept
{
    if (argumentCount != 2)
    {
        return 0U;
    }
    if (std::strcmp(arguments[1], "smp1") == 0)
    {
        return 1U;
    }
    if (std::strcmp(arguments[1], "smp2") == 0)
    {
        return 2U;
    }
    if (std::strcmp(arguments[1], "smp3") == 0)
    {
        return 3U;
    }
    if (std::strcmp(arguments[1], "smp4") == 0)
    {
        return 4U;
    }
    if (std::strcmp(arguments[1], "shootdown-failure") == 0)
    {
        return 0xFFU;
    }
    return 0U;
}
} // namespace
} // namespace Asteria::Test

int main(const int argumentCount, char** const arguments)
{
    const Asteria::Foundation::UInt8 processorCount = Asteria::Test::ParseProcessorCount(argumentCount, arguments);
    if (processorCount == 0U)
    {
        static_cast<void>(std::fprintf(
            stderr, "usage: Ia64ApplicationProcessorRuntimeMappingOwnerTests smp1|smp2|smp3|smp4|shootdown-failure\n"));
        return 2;
    }
    TestContext runner{"Ia64ApplicationProcessorRuntimeMappingOwnerTests"};
    if (processorCount == 0xFFU)
    {
        Asteria::Test::TestShootdownFailureRetention(runner);
    }
    else
    {
        Asteria::Test::TestMappingOwner(runner, processorCount);
    }
    return runner.Finish();
}
