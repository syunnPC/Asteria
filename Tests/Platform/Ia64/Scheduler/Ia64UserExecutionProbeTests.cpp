// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "Boot/BootInfo.hpp"
#include "Hal/Memory/ILocalTlbInvalidator.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/TlbShootdownDispatcher.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/Thread.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbe.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
constexpr Foundation::UInt64 ActivationStatus{0U};
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
    return Asteria::Test::ActivationStatus;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64LaunchUserMode(const Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan*) noexcept
{
    return 0U;
}

namespace Asteria::Test
{
namespace
{
using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Hal::Memory::ILocalTlbInvalidator;
using Hal::Memory::LocalTlbInvalidationError;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapInitialMappingCount;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64BspRuntimeMemory;
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
using Platform::Ia64::Scheduler::GetIa64UserExecutionProbePayload;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbe;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeCapabilityQuerySystemCallNumber;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildFlushInstructionOffset;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildQueryBreakInstructionOffset;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildReceiveBreakInstructionOffset;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildSendBreakInstructionOffset;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildWaitBreakInstructionOffset;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeChildSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeVirtualStart;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeCodeView;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeError;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedCapabilityQueryError;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedCapabilityQueryInformation;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedCapabilityQueryLeaseDeadlineTicks;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedEndpointCapabilitySelector;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitRegistrationSequence;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedWaitTag;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedIpcError;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedIpcInformation;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedReceiveTransferredBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeExpectedSendTransferredBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcCompletionOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcBootstrapVirtualAddress;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcDeliveryCapacityBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcDeliveryVirtualAddress;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcEvidenceSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcEvidenceVirtualAddress;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcReceiveErrorOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcReceiveInformationOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcReceiveSystemCallNumber;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcReceiveTransferredBytesOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcSendErrorOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcSendInformationOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcSendSystemCallNumber;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeIpcSendTransferredBytesOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeMemoryStackVirtualStart;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeParentResumeOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbePayloadSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeQueryResponseVirtualAddress;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseBackingStoreSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseBackingStoreVirtualStart;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeRseSentinelAddress;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeSentinel;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStackedSentinelCount;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStackSentinelOffsetFromEndBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStageError;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeState;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorage;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeStorageRegion;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeTerminalLoopOffsetBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeWaitResponseSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeWaitResponseVirtualAddress;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeWaitSetWaitSystemCallNumber;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeSystemCallResponseOffsetFromStackPointerBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeSystemCallResponseSizeBytes;
using Platform::Ia64::Scheduler::Ia64UserExecutionProbeWritableAlias;
using Platform::Ia64::Scheduler::Ia64UserModeInitialProcessorStatus;
using Platform::Ia64::Scheduler::Ia64UserModeLaunch;
using Platform::Ia64::Scheduler::Ia64UserModeVirtualRange;
using Platform::Synchronization::SpinLock;

constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 ProbePoolBase{0x0400'0000U};
constexpr Foundation::UInt64 ProbePoolPages{16U};
constexpr Foundation::UInt64 ChargeOwnerValue{0x4A64U};
constexpr Foundation::UInt8 CanaryByte{0xA5U};
static_assert(Ia64UserExecutionProbeRseSentinelAddress >= Ia64UserExecutionProbeRseBackingStoreVirtualStart);
static_assert(Ia64UserExecutionProbeRseSentinelAddress + sizeof(Foundation::UInt64) <=
              Ia64UserExecutionProbeRseBackingStoreVirtualStart + Ia64UserExecutionProbeRseBackingStoreSizeBytes);
static_assert((Ia64UserExecutionProbeRseSentinelAddress & 0x1FFU) == 0x1F0U);
static_assert(Ia64UserExecutionProbeStackedSentinelCount == 4U);
static_assert((Ia64UserExecutionProbePayloadSizeBytes & 0xFU) == 0U);
static_assert(Ia64UserExecutionProbeTerminalLoopOffsetBytes == 0x100U);
static_assert(Ia64UserExecutionProbeChildOffsetBytes == 0x110U);
static_assert(Ia64UserExecutionProbeChildOffsetBytes + Ia64UserExecutionProbeChildSizeBytes ==
              Ia64UserExecutionProbePayloadSizeBytes);
static_assert(Ia64UserExecutionProbeParentResumeOffsetBytes == 0x60U);
static_assert(Ia64UserExecutionProbeChildFlushInstructionOffset == 0x16U);
static_assert(Ia64UserExecutionProbeChildQueryBreakInstructionOffset == 0x7CU);
static_assert(Ia64UserExecutionProbeChildSendBreakInstructionOffset == 0x10CU);
static_assert(Ia64UserExecutionProbeChildReceiveBreakInstructionOffset == 0x17CU);
static_assert(Ia64UserExecutionProbeChildWaitBreakInstructionOffset == 0x22CU);
static_assert(Ia64UserExecutionProbePayloadSizeBytes <= Ia64UserExecutionProbeCodeSizeBytes);
static_assert(Ia64UserExecutionProbeCapabilityQuerySystemCallNumber == 2U);
static_assert(Ia64UserExecutionProbeIpcSendSystemCallNumber == 6U);
static_assert(Ia64UserExecutionProbeIpcReceiveSystemCallNumber == 7U);
static_assert(Ia64UserExecutionProbeWaitSetWaitSystemCallNumber == 8U);
static_assert(Ia64UserExecutionProbeExpectedEndpointCapabilitySelector == 0x101U);
static_assert(Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector == 0x102U);
static_assert(Ia64UserExecutionProbeExpectedCapabilityQueryError == 0U);
static_assert(Ia64UserExecutionProbeExpectedCapabilityQueryInformation == 0x0000'0000'0001'0001ULL);
static_assert(Ia64UserExecutionProbeExpectedCapabilityQueryLeaseDeadlineTicks == ~Foundation::UInt64{0U});
static_assert(Ia64UserExecutionProbeIpcEvidenceSizeBytes == 56U);
static_assert(Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes == 16U);
static_assert(Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes == 88U);
static_assert(Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes == 120U);
static_assert(Ia64UserExecutionProbeIpcSendErrorOffsetBytes == 0U);
static_assert(Ia64UserExecutionProbeIpcSendInformationOffsetBytes == 8U);
static_assert(Ia64UserExecutionProbeIpcSendTransferredBytesOffsetBytes == 16U);
static_assert(Ia64UserExecutionProbeIpcReceiveErrorOffsetBytes == 24U);
static_assert(Ia64UserExecutionProbeIpcReceiveInformationOffsetBytes == 32U);
static_assert(Ia64UserExecutionProbeIpcReceiveTransferredBytesOffsetBytes == 40U);
static_assert(Ia64UserExecutionProbeIpcCompletionOffsetBytes == 48U);
static_assert(Ia64UserExecutionProbeIpcDeliveryCapacityBytes == 136U);
static_assert(Ia64UserExecutionProbeWaitResponseSizeBytes == 24U);
static_assert(Ia64UserExecutionProbeExpectedIpcError == 0U);
static_assert(Ia64UserExecutionProbeExpectedIpcInformation == 1U);
static_assert(Ia64UserExecutionProbeExpectedSendTransferredBytes == 0U);
static_assert(Ia64UserExecutionProbeExpectedReceiveTransferredBytes == 136U);
static_assert(Ia64UserExecutionProbeExpectedWaitRegistrationSequence == 1U);

struct RuntimeArena
{
    Ia64BspRuntimeMemory Owner;
};

template <Foundation::Size DataSize> struct alignas(PageBytes) GuardedStorage
{
    Foundation::UInt8 Before[PageBytes];
    Foundation::UInt8 Data[DataSize];
    Foundation::UInt8 After[PageBytes];
};

constinit SpinLock g_registrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_registry{g_registrySynchronization};
constinit RootCapabilityIssuer g_issuer{g_registry};
constinit BootstrapPageAllocator g_allocator;
// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<RuntimeArena> g_runtimeArenaStorage{};
RuntimeArena& g_runtimeArena = g_runtimeArenaStorage.Construct();
constinit InitialAddressSpacePreparation g_preparation;
alignas(32U * 1024U) constinit Foundation::UInt8 g_initialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageBytes) constinit Foundation::UInt8 g_initialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
constinit ResourceDomainLedger g_resourceLedger;
constinit ResourceDomain g_resourceDomain{g_resourceLedger};
constinit Thread g_mappingThread;
constinit Ia64UserExecutionProbe g_probe;
constinit GuardedStorage<Ia64UserExecutionProbeCodeSizeBytes> g_codeStorage{};
constinit GuardedStorage<Ia64UserExecutionProbeMemoryStackSizeBytes> g_memoryStackStorage{};
constinit GuardedStorage<Ia64UserExecutionProbeRseBackingStoreSizeBytes> g_rseStorage{};
alignas(PageBytes) constinit Foundation::UInt8 g_supervisorMemoryStack[16U * 1024U]{};
alignas(PageBytes) constinit Foundation::UInt8 g_supervisorRseBackingStore[16U * 1024U]{};

static_assert(alignof(RuntimeArena) == PageBytes);
static_assert((sizeof(RuntimeArena) & (PageBytes - 1U)) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(RuntimeArena)) == 0x40'0000U);
static_assert(alignof(Ia64BspRuntimeMemory) == PageBytes);
static_assert((sizeof(Ia64BspRuntimeMemory) & (PageBytes - 1U)) == 0U);

class RecordingInvalidator final : public ILocalTlbInvalidator
{
public:
    [[nodiscard]] LocalTlbInvalidationError Invalidate(const VirtualAddress start,
                                                       const PageCount pages) noexcept override
    {
        if (Calls < 3U)
        {
            Starts[Calls] = start;
            Pages[Calls] = pages;
        }
        ++Calls;
        return LocalTlbInvalidationError::None;
    }

    Foundation::Size Calls{0U};
    VirtualAddress Starts[3U]{};
    PageCount Pages[3U]{};
};

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

[[nodiscard]] bool PrepareFixture(Ia64UserExecutionProbeStorage& storage) noexcept
{
    const PhysicalPageRange bootstrapReservation{
        .PhysicalStart = ReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    if (!TestSupport::MakeIa64HostedBootGeometry(
            AddressOf(&Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage), AddressOf(&g_runtimeArena),
            sizeof(g_runtimeArena), bootstrapReservation, geometry))
    {
        return false;
    }
    BootInfo bootInfo{
        .Signature = Boot::BootInfoSignature,
        .Version = Boot::BootInfoVersion,
        .SizeBytes = sizeof(BootInfo),
        .MemoryRegionCount = 4U,
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
    bootInfo.MemoryRegions[1U] = PhysicalMemoryRegion{
        .PhysicalStart = ProbePoolBase,
        .VirtualStart = 0U,
        .PageCount = ProbePoolPages,
        .Attributes = 0x8U,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[2U] = geometry.ImageRegion;
    bootInfo.MemoryRegions[3U] = geometry.RuntimeMemoryRegion;
    if (!TestSupport::SortIa64HostedBootRegions(bootInfo) || (geometry.Image.ImageBase <= ProbePoolBase) ||
        (g_allocator.Initialize(bootInfo) != BootstrapPageAllocatorError::None))
    {
        return false;
    }
    const auto reserved = g_allocator.Allocate(BootstrapReservedPageCount, 1U);
    // The boot allocates the run-time memory reservation right after the
    // bootstrap one, aligned to its size; here that is the arena's page.
    const auto runtimeReservation =
        g_allocator.Allocate(geometry.RuntimeMemoryPages.PageCount, geometry.RuntimeMemoryPages.PageCount);
    const auto code = g_allocator.Allocate(1U, 1U);
    const auto memoryStack = g_allocator.Allocate(4U, 1U);
    const auto rse = g_allocator.Allocate(4U, 1U);
    if (!reserved.HasValue() || !runtimeReservation.HasValue() ||
        !TestSupport::Ia64HostedPagesEqual(runtimeReservation.GetValue(), geometry.RuntimeMemoryPages) ||
        !code.HasValue() || !memoryStack.HasValue() || !rse.HasValue())
    {
        return false;
    }
    storage = Ia64UserExecutionProbeStorage{
        .Code =
            Ia64UserExecutionProbeStorageRegion{
                .Allocation = code.GetValue(),
                .WritableAlias =
                    Ia64UserExecutionProbeWritableAlias{
                        .Data = g_codeStorage.Data,
                        .SizeBytes = sizeof(g_codeStorage.Data),
                        .PhysicalStart = code.GetValue().PhysicalStart,
                    },
            },
        .MemoryStack =
            Ia64UserExecutionProbeStorageRegion{
                .Allocation = memoryStack.GetValue(),
                .WritableAlias =
                    Ia64UserExecutionProbeWritableAlias{
                        .Data = g_memoryStackStorage.Data,
                        .SizeBytes = sizeof(g_memoryStackStorage.Data),
                        .PhysicalStart = memoryStack.GetValue().PhysicalStart,
                    },
            },
        .RseBackingStore =
            Ia64UserExecutionProbeStorageRegion{
                .Allocation = rse.GetValue(),
                .WritableAlias =
                    Ia64UserExecutionProbeWritableAlias{
                        .Data = g_rseStorage.Data,
                        .SizeBytes = sizeof(g_rseStorage.Data),
                        .PhysicalStart = rse.GetValue().PhysicalStart,
                    },
            },
    };
    if (!g_preparation
             .Prepare(geometry.Image, bootInfo, reserved.GetValue(), runtimeReservation.GetValue(), Capabilities(),
                      Firmware(), Regions(),
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

[[nodiscard]] bool BytesAre(const Foundation::UInt8* const bytes, const Foundation::Size count,
                            const Foundation::UInt8 expected) noexcept
{
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != expected)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool MappingMatches(const AddressSpaceMappingRecord& record, const Foundation::UInt64 virtualStart,
                                  const PhysicalPageRange& allocation, const Foundation::UInt64 pages,
                                  const bool executable) noexcept
{
    return (record.VirtualStart.GetValue() == virtualStart) &&
           (record.PhysicalStart.GetValue() == allocation.PhysicalStart) && (record.Pages.GetValue() == pages) &&
           record.Permissions.Contains(PagePermission::Read) && record.Permissions.Contains(PagePermission::User) &&
           (record.Permissions.Contains(PagePermission::Execute) == executable) &&
           (record.Permissions.Contains(PagePermission::Write) == !executable);
}

void TestProbe(TestContext& runner) noexcept
{
    Ia64UserExecutionProbeStorage storage{};
    ASTERIA_EXPECT(runner, "bootstrap fixture returns allocator-backed probe receipts", PrepareFixture(storage));

    static_cast<void>(std::memset(&g_codeStorage, CanaryByte, sizeof(g_codeStorage)));
    static_cast<void>(std::memset(&g_memoryStackStorage, CanaryByte, sizeof(g_memoryStackStorage)));
    static_cast<void>(std::memset(&g_rseStorage, CanaryByte, sizeof(g_rseStorage)));

    const auto firstPayload = GetIa64UserExecutionProbePayload();
    const auto secondPayload = GetIa64UserExecutionProbePayload();
    ASTERIA_EXPECT(runner, "hosted IA-64 spill/fill payload view is bounded, aligned, and stable",
                   firstPayload.Data != nullptr, firstPayload.SizeBytes == Ia64UserExecutionProbePayloadSizeBytes,
                   (firstPayload.SizeBytes & 0xFU) == 0U, firstPayload.SizeBytes <= Ia64UserExecutionProbeCodeSizeBytes,
                   (reinterpret_cast<Foundation::UIntPointer>(firstPayload.Data) & 0xFU) == 0U,
                   secondPayload.Data == firstPayload.Data, secondPayload.SizeBytes == firstPayload.SizeBytes);

    ASTERIA_EXPECT(runner, "all four syscall sites remain within the staged child payload",
                   Ia64UserExecutionProbeChildQueryBreakInstructionOffset < Ia64UserExecutionProbeChildSizeBytes,
                   Ia64UserExecutionProbeChildSendBreakInstructionOffset < Ia64UserExecutionProbeChildSizeBytes,
                   Ia64UserExecutionProbeChildReceiveBreakInstructionOffset < Ia64UserExecutionProbeChildSizeBytes,
                   Ia64UserExecutionProbeChildWaitBreakInstructionOffset < Ia64UserExecutionProbeChildSizeBytes);

    constexpr Foundation::UInt64 initialStackPointer{Ia64UserExecutionProbeMemoryStackVirtualStart +
                                                     Ia64UserExecutionProbeMemoryStackSizeBytes -
                                                     Ia64UserExecutionProbeStackSentinelOffsetFromEndBytes};
    constexpr Foundation::UInt64 responseStart{initialStackPointer -
                                               Ia64UserExecutionProbeSystemCallResponseOffsetFromStackPointerBytes};
    ASTERIA_EXPECT(runner,
                   "spill/fill result preserves the successful CapabilityQuery response at its fixed stack address",
                   responseStart >= Ia64UserExecutionProbeMemoryStackVirtualStart,
                   responseStart + Ia64UserExecutionProbeSystemCallResponseSizeBytes == initialStackPointer,
                   responseStart == Ia64UserExecutionProbeQueryResponseVirtualAddress);
    ASTERIA_EXPECT(runner,
                   "CapabilityQuery expects success, ProtectionDomain/Inspect information, and an infinite lease",
                   Ia64UserExecutionProbeExpectedCapabilityQueryError == 0U,
                   Ia64UserExecutionProbeExpectedCapabilityQueryInformation == 0x0000'0000'0001'0001ULL,
                   Ia64UserExecutionProbeExpectedCapabilityQueryLeaseDeadlineTicks == ~Foundation::UInt64{0U});
    constexpr Foundation::UInt64 memoryStackEnd{Ia64UserExecutionProbeMemoryStackVirtualStart +
                                                Ia64UserExecutionProbeMemoryStackSizeBytes};
    ASTERIA_EXPECT(
        runner,
        "bootstrap, IPC delivery, WaitSet response, and Query response are "
        "ordered and bounded in the stack mapping",
        Ia64UserExecutionProbeIpcBootstrapVirtualAddress >= Ia64UserExecutionProbeMemoryStackVirtualStart,
        Ia64UserExecutionProbeIpcBootstrapVirtualAddress + Ia64UserExecutionProbeIpcBootstrapRecordSizeBytes <=
            Ia64UserExecutionProbeIpcEvidenceVirtualAddress,
        Ia64UserExecutionProbeIpcEvidenceVirtualAddress + Ia64UserExecutionProbeIpcEvidenceSizeBytes <=
            Ia64UserExecutionProbeIpcDeliveryVirtualAddress,
        Ia64UserExecutionProbeIpcDeliveryVirtualAddress + Ia64UserExecutionProbeIpcDeliveryCapacityBytes <=
            Ia64UserExecutionProbeWaitResponseVirtualAddress,
        Ia64UserExecutionProbeWaitResponseVirtualAddress + Ia64UserExecutionProbeWaitResponseSizeBytes <=
            Ia64UserExecutionProbeQueryResponseVirtualAddress,
        Ia64UserExecutionProbeQueryResponseVirtualAddress + Ia64UserExecutionProbeSystemCallResponseSizeBytes <=
            initialStackPointer,
        initialStackPointer + sizeof(Ia64UserExecutionProbeSentinel) <= memoryStackEnd);
    ASTERIA_EXPECT(
        runner, "IPC and WaitSet completion use their staged selectors and response values",
        Ia64UserExecutionProbeExpectedEndpointCapabilitySelector == 0x101U,
        Ia64UserExecutionProbeExpectedWaitSetCapabilitySelector == 0x102U,
        Ia64UserExecutionProbeIpcBootstrapFrameOffsetBytes == 16U,
        Ia64UserExecutionProbeIpcBootstrapFrameSizeBytes == 88U, Ia64UserExecutionProbeExpectedIpcError == 0U,
        Ia64UserExecutionProbeExpectedIpcInformation == 1U, Ia64UserExecutionProbeExpectedSendTransferredBytes == 0U,
        Ia64UserExecutionProbeExpectedReceiveTransferredBytes == Ia64UserExecutionProbeIpcDeliveryCapacityBytes,
        Ia64UserExecutionProbeExpectedWaitTag == 0x5741'4954'5345'5401ULL,
        Ia64UserExecutionProbeExpectedWaitRegistrationSequence == 1U,
        Ia64UserExecutionProbeSentinel == 0x4153'5445'5249'4133ULL);

    Ia64UserExecutionProbeStorage invalidStorage = storage;
    invalidStorage.Code.Allocation.PageCount = 2U;
    const auto invalidStage = g_probe.StageStorage(invalidStorage, Ia64UserExecutionProbeCodeView{
                                                                       .Data = firstPayload.Data,
                                                                       .SizeBytes = firstPayload.SizeBytes,
                                                                   });
    ASTERIA_EXPECT(
        runner, "invalid receipt rejection is failure-atomic before every write", !invalidStage.HasValue(),
        invalidStage.GetError() == Ia64UserExecutionProbeStageError::InvalidCodeAllocation,
        g_probe.GetState() == Ia64UserExecutionProbeState::Fresh,
        BytesAre(reinterpret_cast<const Foundation::UInt8*>(&g_codeStorage), sizeof(g_codeStorage), CanaryByte),
        BytesAre(reinterpret_cast<const Foundation::UInt8*>(&g_memoryStackStorage), sizeof(g_memoryStackStorage),
                 CanaryByte),
        BytesAre(reinterpret_cast<const Foundation::UInt8*>(&g_rseStorage), sizeof(g_rseStorage), CanaryByte));

    const auto staged = g_probe.StageStorage(storage, Ia64UserExecutionProbeCodeView{
                                                          .Data = firstPayload.Data,
                                                          .SizeBytes = firstPayload.SizeBytes,
                                                      });
    ASTERIA_EXPECT(
        runner, "StageStorage accepts the fixed payload and initializes all accepted pages", staged.HasValue(),
        g_probe.GetState() == Ia64UserExecutionProbeState::Staged,
        staged.GetValue().CodeImageSizeBytes == Ia64UserExecutionProbePayloadSizeBytes,
        std::memcmp(g_codeStorage.Data, firstPayload.Data, firstPayload.SizeBytes) == 0,
        BytesAre(g_codeStorage.Data + firstPayload.SizeBytes, sizeof(g_codeStorage.Data) - firstPayload.SizeBytes, 0U),
        BytesAre(g_memoryStackStorage.Data, sizeof(g_memoryStackStorage.Data), 0U),
        BytesAre(g_rseStorage.Data, sizeof(g_rseStorage.Data), 0U));
    ASTERIA_EXPECT(runner, "staging leaves every surrounding guard canary untouched",
                   BytesAre(g_codeStorage.Before, sizeof(g_codeStorage.Before), CanaryByte),
                   BytesAre(g_codeStorage.After, sizeof(g_codeStorage.After), CanaryByte),
                   BytesAre(g_memoryStackStorage.Before, sizeof(g_memoryStackStorage.Before), CanaryByte),
                   BytesAre(g_memoryStackStorage.After, sizeof(g_memoryStackStorage.After), CanaryByte),
                   BytesAre(g_rseStorage.Before, sizeof(g_rseStorage.Before), CanaryByte),
                   BytesAre(g_rseStorage.After, sizeof(g_rseStorage.After), CanaryByte));

    const ResourceDomainLimits limits{
        .CommittedMemoryPages = 64U,
        .ResidentMemoryPages = 64U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = 64U,
        .IpcQueueBytes = 0U,
        .Threads = 1U,
        .ChildDomains = 0U,
    };
    const auto domainInitialized = g_resourceDomain.InitializeRoot(ResourceDomainId{ChargeOwnerValue}, limits);
    const auto domainObject = g_issuer.CreateObject(g_resourceDomain);
    const bool threadInitialized = g_mappingThread.Initialize(ResourceDomainId{ChargeOwnerValue}) == ThreadError::None;
    const auto threadObject = g_issuer.CreateObject(g_mappingThread);
    ASTERIA_EXPECT(runner, "resource-domain and mapping-thread authorities are registered honestly",
                   domainInitialized.Error == ResourceDomainError::None, domainObject.HasValue(), threadInitialized,
                   threadObject.HasValue());
    if (!domainObject.HasValue() || !threadObject.HasValue())
    {
        return;
    }
    const ResourceDomainObjectId domainIdentity{g_registry, domainObject.GetValue()};
    const ThreadId threadIdentity{g_registry, threadObject.GetValue()};

    const auto runtimeReady = g_runtimeArena.Owner.PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_preparation, ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const auto commonRoot = g_runtimeArena.Owner.BorrowReadyRoot();
    const auto borrowed = g_runtimeArena.Owner.BorrowReadyInitialComponentRoot();
    ASTERIA_EXPECT(runner, "BSP runtime owner admits one resident root for the mapping transaction",
                   runtimeReady.HasValue(), commonRoot.HasValue(), borrowed.HasValue(),
                   g_runtimeArena.Owner.GetSnapshot().State == Ia64BspRuntimeMemoryState::Ready);
    if (!runtimeReady.HasValue() || !commonRoot.HasValue() || !borrowed.HasValue())
    {
        return;
    }

    RecordingInvalidator wrongInvalidator;
    TlbShootdownDispatcher wrongDispatcher{CpuId{1U}, wrongInvalidator};
    const Foundation::Size mappingsBefore = borrowed.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount;
    const Foundation::Size commonMappingsBefore = commonRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount;
    const Foundation::Size objectsBefore = g_registry.GetActiveCount();
    const Ia64UserModeVirtualRange supervisorMemoryStack{
        .VirtualStart = AddressOf(g_supervisorMemoryStack),
        .SizeBytes = sizeof(g_supervisorMemoryStack),
    };
    const Ia64UserModeVirtualRange supervisorRse{
        .VirtualStart = AddressOf(g_supervisorRseBackingStore),
        .SizeBytes = sizeof(g_supervisorRseBackingStore),
    };
    const auto rejected = g_probe.PrepareLaunch(
        g_runtimeArena.Owner, g_registry, g_issuer, domainIdentity, g_resourceDomain, threadIdentity, wrongDispatcher,
        supervisorMemoryStack, supervisorRse, Capabilities().Translation.ImplementedVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "PrepareLaunch preflight rejection leaves all external state retryable",
                   !rejected.HasValue(), rejected.GetError().Error == Ia64UserExecutionProbeError::InvalidDispatcher,
                   g_probe.GetState() == Ia64UserExecutionProbeState::Staged,
                   borrowed.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == mappingsBefore,
                   g_registry.GetActiveCount() == objectsBefore, wrongInvalidator.Calls == 0U);

    RecordingInvalidator invalidator;
    TlbShootdownDispatcher dispatcher{CpuId{0U}, invalidator};
    const auto selected = borrowed.GetValue().ControllerStorage->SwitchTo(*borrowed.GetValue().AddressSpaceStorage);
    const auto prepared = g_probe.PrepareLaunch(
        g_runtimeArena.Owner, g_registry, g_issuer, domainIdentity, g_resourceDomain, threadIdentity, dispatcher,
        supervisorMemoryStack, supervisorRse, Capabilities().Translation.ImplementedVirtualAddressMostSignificantBit);
    const auto restored = borrowed.GetValue().ControllerStorage->SwitchToIdle();
    ASTERIA_EXPECT(
        runner,
        "private-root mapping transactions perform three exact local invalidations, commits, and common-root restoration",
        selected.HasValue(), prepared.HasValue(), restored.HasValue(),
        g_probe.GetState() == Ia64UserExecutionProbeState::Ready, g_registry.GetActiveCount() == objectsBefore + 6U,
        borrowed.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == mappingsBefore + 3U,
        commonRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == commonMappingsBefore,
        borrowed.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 0U,
        commonRoot.GetValue().AddressSpaceStorage->GetSnapshot().ResidentProcessorMask == 1U, invalidator.Calls == 3U,
        invalidator.Starts[0U].GetValue() == Ia64UserExecutionProbeCodeVirtualStart,
        invalidator.Pages[0U].GetValue() == 1U,
        invalidator.Starts[1U].GetValue() == Ia64UserExecutionProbeMemoryStackVirtualStart,
        invalidator.Pages[1U].GetValue() == 4U,
        invalidator.Starts[2U].GetValue() == Ia64UserExecutionProbeRseBackingStoreVirtualStart,
        invalidator.Pages[2U].GetValue() == 4U);
    if (!prepared.HasValue())
    {
        return;
    }

    const auto codeRecord = borrowed.GetValue().AddressSpaceStorage->TryGetMapping(mappingsBefore);
    const auto stackRecord = borrowed.GetValue().AddressSpaceStorage->TryGetMapping(mappingsBefore + 1U);
    const auto rseRecord = borrowed.GetValue().AddressSpaceStorage->TryGetMapping(mappingsBefore + 2U);
    ASTERIA_EXPECT(runner, "published mappings are exactly RX code and distinct RW stack/RSE extents",
                   codeRecord.HasValue(),
                   MappingMatches(codeRecord.GetValue(), Ia64UserExecutionProbeCodeVirtualStart,
                                  storage.Code.Allocation, 1U, true),
                   stackRecord.HasValue(),
                   MappingMatches(stackRecord.GetValue(), Ia64UserExecutionProbeMemoryStackVirtualStart,
                                  storage.MemoryStack.Allocation, 4U, false),
                   rseRecord.HasValue(),
                   MappingMatches(rseRecord.GetValue(), Ia64UserExecutionProbeRseBackingStoreVirtualStart,
                                  storage.RseBackingStore.Allocation, 4U, false));

    const auto& request = prepared.GetValue();
    const auto launchPlan = Ia64UserModeLaunch::Validate(request);
    const auto retainedRequest = g_probe.GetLaunchRequest();
    ASTERIA_EXPECT(runner, "owner returns and retains one launch-record accepted by the launch boundary",
                   launchPlan.HasValue(), retainedRequest.HasValue(),
                   request.Code.VirtualStart == Ia64UserExecutionProbeCodeVirtualStart,
                   request.Code.SizeBytes == Ia64UserExecutionProbeCodeSizeBytes,
                   request.MemoryStack.VirtualStart == Ia64UserExecutionProbeMemoryStackVirtualStart,
                   request.MemoryStack.SizeBytes == Ia64UserExecutionProbeMemoryStackSizeBytes,
                   request.RseBackingStore.VirtualStart == Ia64UserExecutionProbeRseBackingStoreVirtualStart,
                   request.RseBackingStore.SizeBytes == Ia64UserExecutionProbeRseBackingStoreSizeBytes,
                   request.EntryBundleAddress == Ia64UserExecutionProbeCodeVirtualStart,
                   request.InitialGlobalPointer == 0U, launchPlan.GetValue().InitialGlobalPointer == 0U,
                   request.Requirements.UserProcessorStatus == Ia64UserModeInitialProcessorStatus,
                   request.SupervisorContinuation != nullptr,
                   retainedRequest.GetValue().SupervisorContinuation == request.SupervisorContinuation);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64UserExecutionProbeTests"};
    Asteria::Test::TestProbe(runner);
    return runner.Finish();
}
