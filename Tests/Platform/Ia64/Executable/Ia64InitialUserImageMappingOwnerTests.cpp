// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Memory/PinnedFrameMapping.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
constexpr Asteria::Foundation::Size TestImageSizeBytes{0x5000U};
constexpr Asteria::Foundation::Size TestBackingPageCount{152U};
constexpr Asteria::Foundation::Size TestBackingSizeBytes{TestBackingPageCount * 4096U};
static_assert(TestBackingSizeBytes == 622592U);
} // namespace

extern "C"
{
extern Asteria::Foundation::UInt8 AsteriaIa64InitialUserImageMappingOwnerTestStorage[TestBackingSizeBytes];
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

asm(".pushsection .bss\n"
    ".balign 4096\n"
    ".global AsteriaIa64InitialUserImageMappingOwnerTestStorage\n"
    ".type AsteriaIa64InitialUserImageMappingOwnerTestStorage, @object\n"
    "AsteriaIa64InitialUserImageMappingOwnerTestStorage:\n"
    ".zero 557056\n"
    ".size AsteriaIa64InitialUserImageMappingOwnerTestStorage, "
    ". - AsteriaIa64InitialUserImageMappingOwnerTestStorage\n"
    ".popsection\n");

namespace Asteria::Test
{
namespace
{
[[nodiscard]] Boot::BootFileRegion SourceRegion() noexcept
{
    Boot::BootFileRegion region{};
    region.PhysicalStart = reinterpret_cast<Foundation::UInt64>(AsteriaIa64InitialUserImageMappingOwnerTestStorage);
    region.PageCount = (20480U + 4095U) / 4096U;
    return region;
}

using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectDescriptor;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::MemoryObjectId;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::PinnedFrameMappingError;
using Kernel::Memory::PinnedFrameMappingPreparation;
using Kernel::Memory::PinnedFrameMappingRequest;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Executable::Ia64InitialUserImageGuardPolicy;
using Platform::Ia64::Executable::Ia64InitialUserImageMappingAccessError;
using Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageMappingOwnerError;
using Platform::Ia64::Executable::Ia64InitialUserImageMappingOwnerState;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageOwner;
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
using Platform::Synchronization::SpinLock;

constexpr Foundation::Size ProgramHeaderTableOffset{64U};
constexpr Foundation::Size ProgramHeaderSizeBytes{56U};
constexpr Foundation::UInt64 PageSizeBytes{4096U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 RegionOneBase{0x2000'0000'0000'0000ULL};
constexpr Foundation::UInt64 CodeVirtualStart{RegionOneBase + 0x0400'1100U};
constexpr Foundation::UInt64 ReadOnlyVirtualStart{RegionOneBase + 0x0400'3100U};
constexpr Foundation::UInt64 DataVirtualStart{RegionOneBase + 0x0400'8100U};
constexpr Foundation::UInt64 CodeFileOffset{0x1100U};
constexpr Foundation::UInt64 ReadOnlyFileOffset{0x2100U};
constexpr Foundation::UInt64 DataFileOffset{0x3100U};
constexpr Foundation::UInt64 DescriptorAddress{ReadOnlyVirtualStart + 0x20U};
constexpr Foundation::UInt64 EntryBundleAddress{CodeVirtualStart + 0x20U};
constexpr Foundation::UInt64 GlobalPointer{DataVirtualStart + 0x80U};
constexpr Foundation::UInt64 FirmwareWriteBackAttribute{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt8 ValidPhysicalAddressBits{48U};
constexpr Foundation::UInt8 ValidVirtualAddressMostSignificantBit{60U};
constexpr Foundation::UInt64 ChargeOwnerValue{0x4A64U};
constexpr Foundation::Size RuntimeOwnerCount{3U};

struct RuntimeArena
{
    Ia64BspRuntimeMemory Owners[RuntimeOwnerCount];
};

constinit SpinLock g_registrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_registry{g_registrySynchronization};
constinit RootCapabilityIssuer g_issuer{g_registry};
constinit SpinLock g_otherRegistrySynchronization;
Asteria::TestSupport::TestObjectRegistry g_otherRegistry{g_otherRegistrySynchronization};
// The arena is the run-time memory reservation: it fills its own architected
// page, so one DTR maps it wherever the host loads the test and no other host
// object shares that page (Ia64HostedBootGeometry).
constinit TestSupport::Ia64HostedRuntimeMemoryArena<RuntimeArena> g_runtimeArenaStorage{};
RuntimeArena& g_runtimeArena = g_runtimeArenaStorage.Construct();
constinit InitialAddressSpacePreparation g_preparation;
alignas(32U * 1024U) constinit Foundation::UInt8 g_initialInterruptVectorTable[InitialInterruptVectorTableSizeBytes]{};
alignas(PageSizeBytes) constinit Foundation::UInt8 g_initialRseBackingStore[InitialRseBackingStoreSizeBytes]{};
constinit ResourceDomainLedger g_resourceDomainLedger;
constinit ResourceDomain g_resourceDomain{g_resourceDomainLedger};
constinit BootstrapPageAllocator g_imageAllocator;
constinit Ia64InitialUserImageOwner g_stagedOwner;
constinit Ia64InitialUserImageMemoryOwner g_memoryOwner;
constinit Ia64InitialUserImageMemoryOwner g_unreadyMemoryOwner;
constinit Ia64InitialUserImageMappingOwner g_mappingOwner;
constinit Ia64InitialUserImageMappingOwner g_conflictOwner;
constinit Ia64InitialUserImageMappingOwner g_terminalOwner;
constinit BootFrame g_conflictFrame;
constinit MemoryObject g_conflictMemoryObject;
constinit MemoryObject g_terminalConflictMemoryObject;
alignas(PageSizeBytes) constinit Foundation::UInt8 g_materializedArena[TestBackingSizeBytes]{};

static_assert(alignof(RuntimeArena) == PageSizeBytes);
static_assert((sizeof(RuntimeArena) & (PageSizeBytes - 1U)) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(RuntimeArena)) == 0x100'0000U);
static_assert(alignof(Ia64BspRuntimeMemory) == PageSizeBytes);
static_assert((sizeof(Ia64BspRuntimeMemory) & (PageSizeBytes - 1U)) == 0U);
static_assert(!std::is_copy_constructible_v<Ia64InitialUserImageMappingOwner>);
static_assert(!std::is_move_constructible_v<Ia64InitialUserImageMappingOwner>);
} // namespace
} // namespace Asteria::Test

namespace Asteria::Test
{
namespace
{

class PrivateRootQuiescer final
{
public:
    ~PrivateRootQuiescer() noexcept
    {
        for (Foundation::Size index{0U}; index < m_rootCount; ++index)
        {
            AddressSpace* const root = m_roots[index];
            const auto snapshot = root->GetSnapshot();
            if ((snapshot.State != AddressSpaceState::Quiesced) && (snapshot.ResidentProcessorMask == 0U))
            {
                static_cast<void>(root->Quiesce());
            }
        }
    }

    void Add(AddressSpace& root) noexcept
    {
        if (m_rootCount < RuntimeOwnerCount)
        {
            m_roots[m_rootCount] = &root;
            ++m_rootCount;
        }
    }

private:
    AddressSpace* m_roots[RuntimeOwnerCount]{};
    Foundation::Size m_rootCount{0U};
};

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

void WriteLittleEndian16(Foundation::UInt8* const bytes, const Foundation::UInt16 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

void WriteLittleEndian32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

void WriteLittleEndian64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::UInt32 index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] constexpr Foundation::Size GetProgramHeaderOffset(const Foundation::Size index) noexcept
{
    return ProgramHeaderTableOffset + (index * ProgramHeaderSizeBytes);
}

void SetProgramHeader(const Foundation::Size index, const Foundation::UInt32 flags, const Foundation::UInt64 fileOffset,
                      const Foundation::UInt64 virtualStart, const Foundation::UInt64 fileSizeBytes,
                      const Foundation::UInt64 memorySizeBytes) noexcept
{
    Foundation::UInt8* const header =
        AsteriaIa64InitialUserImageMappingOwnerTestStorage + GetProgramHeaderOffset(index);
    WriteLittleEndian32(header, 1U);
    WriteLittleEndian32(header + 4U, flags);
    WriteLittleEndian64(header + 8U, fileOffset);
    WriteLittleEndian64(header + 16U, virtualStart);
    WriteLittleEndian64(header + 24U, virtualStart);
    WriteLittleEndian64(header + 32U, fileSizeBytes);
    WriteLittleEndian64(header + 40U, memorySizeBytes);
    WriteLittleEndian64(header + 48U, PageSizeBytes);
}

void FillPayload(const Foundation::UInt64 fileOffset, const Foundation::UInt64 sizeBytes,
                 const Foundation::UInt8 seed) noexcept
{
    for (Foundation::UInt64 index{0U}; index < sizeBytes; ++index)
    {
        AsteriaIa64InitialUserImageMappingOwnerTestStorage[static_cast<Foundation::Size>(fileOffset + index)] =
            static_cast<Foundation::UInt8>(seed + static_cast<Foundation::UInt8>(index));
    }
}

void MakeCurrentImage() noexcept
{
    static_cast<void>(std::memset(AsteriaIa64InitialUserImageMappingOwnerTestStorage, 0, TestImageSizeBytes));
    Foundation::UInt8* const image = AsteriaIa64InitialUserImageMappingOwnerTestStorage;
    image[0U] = 0x7FU;
    image[1U] = static_cast<Foundation::UInt8>('E');
    image[2U] = static_cast<Foundation::UInt8>('L');
    image[3U] = static_cast<Foundation::UInt8>('F');
    image[4U] = 2U;
    image[5U] = 1U;
    image[6U] = 1U;
    WriteLittleEndian16(image + 16U, 2U);
    WriteLittleEndian16(image + 18U, Platform::Ia64::Executable::Ia64ElfMachine);
    WriteLittleEndian32(image + 20U, 1U);
    WriteLittleEndian64(image + 24U, DescriptorAddress);
    WriteLittleEndian64(image + 32U, ProgramHeaderTableOffset);
    WriteLittleEndian32(image + 48U, Platform::Ia64::Executable::Ia64P64ElfFlags);
    WriteLittleEndian16(image + 52U, 64U);
    WriteLittleEndian16(image + 54U, 56U);
    WriteLittleEndian16(image + 56U, 3U);
    SetProgramHeader(0U, 5U, CodeFileOffset, CodeVirtualStart, 0x180U, 0x300U);
    SetProgramHeader(1U, 4U, ReadOnlyFileOffset, ReadOnlyVirtualStart, 0x300U, 0x1100U);
    SetProgramHeader(2U, 6U, DataFileOffset, DataVirtualStart, 0x900U, 0x3F00U);
    FillPayload(CodeFileOffset, 0x180U, 0x31U);
    FillPayload(ReadOnlyFileOffset, 0x300U, 0x52U);
    FillPayload(DataFileOffset, 0x900U, 0x73U);
    const Foundation::Size descriptorOffset =
        static_cast<Foundation::Size>(ReadOnlyFileOffset + (DescriptorAddress - ReadOnlyVirtualStart));
    WriteLittleEndian64(image + descriptorOffset, EntryBundleAddress);
    WriteLittleEndian64(image + descriptorOffset + 8U, GlobalPointer);
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

[[nodiscard]] bool PrepareRuntimeFoundation(ResourceDomainObjectId& domainIdentity) noexcept
{
    const PhysicalPageRange reserved{
        .PhysicalStart = ReservedBase,
        .PageCount = BootstrapReservedPageCount,
        .Attributes = FirmwareWriteBackAttribute,
        .FirmwareType = 7U,
        .Reserved = 0U,
    };
    TestSupport::Ia64HostedBootGeometry geometry{};
    if (!TestSupport::MakeIa64HostedBootGeometry(
            AddressOf(&Platform::Ia64::Memory::AsteriaIa64RuntimeInterruptVectorTableImage), AddressOf(&g_runtimeArena),
            sizeof(g_runtimeArena), reserved, geometry, FirmwareWriteBackAttribute))
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
        .Attributes = FirmwareWriteBackAttribute,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    bootInfo.MemoryRegions[1U] = geometry.ImageRegion;
    bootInfo.MemoryRegions[2U] = geometry.RuntimeMemoryRegion;
    if (!TestSupport::SortIa64HostedBootRegions(bootInfo))
    {
        return false;
    }
    const auto prepared = g_preparation.Prepare(geometry.Image, bootInfo, reserved, geometry.RuntimeMemoryPages,
                                                Capabilities(), Firmware(), Regions(),
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

    constexpr ResourceDomainLimits limits{
        .CommittedMemoryPages = 1024U,
        .ResidentMemoryPages = 1024U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = ObjectRegistry::MaximumObjects,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
    const ResourceDomainId chargeOwner{ChargeOwnerValue};
    if (g_resourceDomain.InitializeRoot(chargeOwner, limits).Error != ResourceDomainError::None)
    {
        return false;
    }
    const auto registered = g_issuer.CreateObject(g_resourceDomain);
    if (!registered.HasValue())
    {
        return false;
    }
    domainIdentity = ResourceDomainObjectId{g_registry, registered.GetValue()};
    const auto runtime = g_runtimeArena.Owners[0U].PrepareAndAdmit(CpuId{0U}, Capabilities(), g_preparation,
                                                                   chargeOwner, g_registry, g_issuer);
    return runtime.HasValue() && (g_runtimeArena.Owners[0U].GetSnapshot().State == Ia64BspRuntimeMemoryState::Ready);
}

[[nodiscard]] bool MaterializeCurrentImage(const ResourceDomainObjectId domainIdentity) noexcept
{
    MakeCurrentImage();
    BootInfo bootInfo{};
    bootInfo.Signature = Boot::BootInfoSignature;
    bootInfo.Version = Boot::BootInfoVersion;
    bootInfo.SizeBytes = static_cast<Foundation::UInt32>(sizeof(bootInfo));
    bootInfo.MemoryRegionCount = 1U;
    bootInfo.FirmwareDescriptorVersion = 1U;
    bootInfo.FirmwareDescriptorSize = 40U;
    bootInfo.FirmwareMemoryMapSizeBytes = 40U;
    bootInfo.MemoryRegions[0U] = PhysicalMemoryRegion{
        .PhysicalStart = AddressOf(g_materializedArena),
        .VirtualStart = 0U,
        .PageCount = TestBackingPageCount,
        .Attributes = FirmwareWriteBackAttribute,
        .FirmwareType = 7U,
        .Kind = PhysicalMemoryKind::Conventional,
    };
    if (g_imageAllocator.Initialize(bootInfo) != BootstrapPageAllocatorError::None)
    {
        return false;
    }
    const auto staged =
        g_stagedOwner.Stage(g_imageAllocator, ValidPhysicalAddressBits, ValidVirtualAddressMostSignificantBit,
                            {AsteriaIa64InitialUserImageMappingOwnerTestStorage, 20480U}, SourceRegion());
    if (!staged.HasValue())
    {
        return false;
    }
    return g_memoryOwner.Materialize(g_stagedOwner, g_registry, g_issuer, domainIdentity, g_resourceDomain).HasValue();
}

[[nodiscard]] constexpr bool ResourceUsageIsEqual(const Kernel::Resource::ResourceDomainUsage& first,
                                                  const Kernel::Resource::ResourceDomainUsage& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

[[nodiscard]] bool RecordIsExact(const AddressSpaceMappingRecord& record,
                                 const Platform::Ia64::Executable::Ia64InitialUserImageMemoryRangePlan& plan,
                                 const Platform::Ia64::Executable::Ia64InitialUserImageMemorySummary& summary) noexcept
{
    const Foundation::UInt64 physicalStart =
        summary.AggregateArena.PhysicalStart + (plan.FrameOffsetPages * PageSizeBytes);
    return record.MemoryObject.IsEqualTo(plan.MemoryObject) && !record.DeviceRegion.IsValid() &&
           record.Frame.IsEqualTo(summary.AggregateFrame) && (record.VirtualStart.GetValue() == plan.VirtualStart) &&
           (record.PhysicalStart.GetValue() == physicalStart) && (record.Pages.GetValue() == plan.Pages.GetValue()) &&
           (record.MemoryObjectOffsetPages == 0U) && (record.Permissions.GetBits() == plan.Permissions.GetBits()) &&
           record.Permissions.Contains(PagePermission::User) && (record.CachePolicy == MemoryCachePolicy::WriteBack) &&
           (record.Kind == FrameKind::Ram) && (record.Commitment == MemoryCommitment::Pinned) &&
           record.RuntimeMapping && (record.Reserved[0U] == 0U) && (record.Reserved[1U] == 0U) &&
           (record.Reserved[2U] == 0U);
}

[[nodiscard]] bool GuardsAreVacant(AddressSpace& root, const Foundation::Size mappingCount) noexcept
{
    Foundation::UInt64 guardStarts[4U]{};
    Foundation::Size guardCount{0U};
    for (Foundation::Size rangeIndex{0U}; rangeIndex < 5U; ++rangeIndex)
    {
        const auto plan = g_memoryOwner.GetRangePlan(rangeIndex);
        if (!plan.HasValue())
        {
            return false;
        }
        if (plan.GetValue().GuardPolicy != Ia64InitialUserImageGuardPolicy::OnePageBeforeAndAfter)
        {
            continue;
        }
        if (guardCount > 2U)
        {
            return false;
        }
        guardStarts[guardCount] = plan.GetValue().VirtualStart - PageSizeBytes;
        ++guardCount;
        guardStarts[guardCount] = plan.GetValue().VirtualStart + (plan.GetValue().Pages.GetValue() * PageSizeBytes);
        ++guardCount;
    }
    if (guardCount != 4U)
    {
        return false;
    }
    for (Foundation::Size mappingIndex{0U}; mappingIndex < mappingCount; ++mappingIndex)
    {
        const auto record = root.TryGetMapping(mappingIndex);
        if (!record.HasValue())
        {
            return false;
        }
        const Foundation::UInt64 recordStart = record.GetValue().VirtualStart.GetValue();
        const Foundation::UInt64 recordEnd = recordStart + (record.GetValue().Pages.GetValue() * PageSizeBytes);
        for (Foundation::Size guardIndex{0U}; guardIndex < guardCount; ++guardIndex)
        {
            if (RangesOverlap(recordStart, recordEnd, guardStarts[guardIndex], guardStarts[guardIndex] + PageSizeBytes))
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool PrepareAndInstallConflictMapping(AddressSpace& root,
                                                    const ResourceDomainObjectId domainIdentity) noexcept
{
    const auto range = g_memoryOwner.GetRangePlan(0U);
    if (!range.HasValue())
    {
        return false;
    }
    const ResourceDomainId chargeOwner{ChargeOwnerValue};
    const FrameError frameInitialized = g_conflictFrame.Initialize(
        chargeOwner, FrameDescriptor{
                         .PhysicalStart = PhysicalAddress{AddressOf(g_materializedArena) + (100U * PageSizeBytes)},
                         .Pages = PageCount{1U},
                         .CachePolicy = MemoryCachePolicy::WriteBack,
                         .Kind = FrameKind::Ram,
                         .Reserved = {},
                     });
    const PagePermissions permissions = PagePermissions{PagePermission::Read}.With(PagePermission::User);
    const auto memoryInitialized = g_conflictMemoryObject.Initialize(g_registry, domainIdentity, g_resourceDomain,
                                                                     MemoryObjectDescriptor{
                                                                         .Pages = PageCount{1U},
                                                                         .CachePolicy = MemoryCachePolicy::WriteBack,
                                                                         .MaximumPermissions = permissions,
                                                                         .Commitment = MemoryCommitment::Pinned,
                                                                         .Reserved = {},
                                                                     });
    if ((frameInitialized != FrameError::None) || (memoryInitialized.Error != MemoryObjectError::None))
    {
        return false;
    }
    const auto registeredFrame = g_issuer.CreateObject(g_conflictFrame);
    const auto registeredMemoryObject = g_issuer.CreateObject(g_conflictMemoryObject);
    if (!registeredFrame.HasValue() || !registeredMemoryObject.HasValue())
    {
        return false;
    }
    const FrameId frame{g_registry, registeredFrame.GetValue()};
    const MemoryObjectId memoryObject{g_registry, registeredMemoryObject.GetValue()};
    PinnedFrameMappingPreparation preparation;
    const PinnedFrameMappingError prepared =
        preparation.Prepare(g_registry, memoryObject, g_conflictMemoryObject, frame, g_conflictFrame,
                            PinnedFrameMappingRequest{
                                .VirtualStart = Kernel::Memory::VirtualAddress{range.GetValue().VirtualStart},
                                .Pages = PageCount{1U},
                                .MemoryObjectOffsetPages = 0U,
                                .FrameOffsetPages = 0U,
                                .Permissions = permissions,
                                .Reserved = {},
                            });
    if (prepared != PinnedFrameMappingError::None)
    {
        return false;
    }
    const auto begin = root.BeginPinnedFrameMapping(root.GetSnapshot().Identity, preparation);
    return begin.HasValue() && root.CompleteMapping(begin.GetValue().GetToken()).HasValue();
}

[[nodiscard]] bool PrepareAndInstallExecutableDataAlias(AddressSpace& root,
                                                        const ResourceDomainObjectId domainIdentity) noexcept
{
    const auto plan = g_memoryOwner.GetRangePlan(2U);
    const auto range = g_memoryOwner.BorrowRange(2U);
    const auto summary = g_memoryOwner.GetSummary();
    if (!plan.HasValue() || !range.HasValue() || !summary.HasValue() || (range.GetValue().FrameStorage == nullptr))
    {
        return false;
    }

    const PagePermissions permissions =
        PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
    const auto memoryInitialized =
        g_terminalConflictMemoryObject.Initialize(g_registry, domainIdentity, g_resourceDomain,
                                                  MemoryObjectDescriptor{
                                                      .Pages = plan.GetValue().Pages,
                                                      .CachePolicy = MemoryCachePolicy::WriteBack,
                                                      .MaximumPermissions = permissions,
                                                      .Commitment = MemoryCommitment::Pinned,
                                                      .Reserved = {},
                                                  });
    if (memoryInitialized.Error != MemoryObjectError::None)
    {
        return false;
    }
    const auto registered = g_issuer.CreateObject(g_terminalConflictMemoryObject);
    if (!registered.HasValue())
    {
        return false;
    }

    PinnedFrameMappingPreparation preparation;
    const PinnedFrameMappingError prepared = preparation.Prepare(
        g_registry, MemoryObjectId{g_registry, registered.GetValue()}, g_terminalConflictMemoryObject,
        summary.GetValue().AggregateFrame, *range.GetValue().FrameStorage,
        PinnedFrameMappingRequest{
            .VirtualStart = Kernel::Memory::VirtualAddress{RegionOneBase + 0x0800'0000U},
            .Pages = plan.GetValue().Pages,
            .MemoryObjectOffsetPages = 0U,
            .FrameOffsetPages = plan.GetValue().FrameOffsetPages,
            .Permissions = permissions,
            .Reserved = {},
        });
    if (prepared != PinnedFrameMappingError::None)
    {
        return false;
    }
    const auto begin = root.BeginPinnedFrameMapping(root.GetSnapshot().Identity, preparation);
    return begin.HasValue() && root.CompleteMapping(begin.GetValue().GetToken()).HasValue();
}

void TestMappingOwner(TestContext& runner) noexcept
{
    PrivateRootQuiescer quiescer;
    ResourceDomainObjectId domainIdentity{};
    ASTERIA_EXPECT(runner, "fixture prepares the hosted IA-64 runtime and private root",
                   PrepareRuntimeFoundation(domainIdentity));
    if (!domainIdentity.IsValid())
    {
        return;
    }

    const auto privateRoot = g_runtimeArena.Owners[0U].BorrowReadyInitialComponentRoot();
    const auto privateSnapshot = g_runtimeArena.Owners[0U].GetInitialComponentAddressSpaceSnapshot();
    ASTERIA_EXPECT(runner, "the exact private initial-component root starts ready and unresident",
                   privateRoot.HasValue(), privateRoot.GetValue().AddressSpaceStorage != nullptr,
                   privateRoot.GetValue().Processor.GetValue() == 0U,
                   privateRoot.GetValue().AddressSpaceIdentity.IsEqualTo(privateSnapshot.Summary.AddressSpaceIdentity),
                   privateSnapshot.AddressSpace.State == AddressSpaceState::Ready,
                   privateSnapshot.AddressSpace.MappingCount == BootstrapInitialMappingCount,
                   privateSnapshot.AddressSpace.ResidentProcessorMask == 0U);
    if (!privateRoot.HasValue() || (privateRoot.GetValue().AddressSpaceStorage == nullptr))
    {
        return;
    }
    AddressSpace& root = *privateRoot.GetValue().AddressSpaceStorage;
    quiescer.Add(root);

    ASTERIA_EXPECT(runner, "fresh accessors distinguish unavailable state from failure state",
                   g_mappingOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Fresh,
                   !g_mappingOwner.GetSummary().HasValue(),
                   g_mappingOwner.GetSummary().GetError() == Ia64InitialUserImageMappingAccessError::NotReady,
                   !g_mappingOwner.GetMapping(0U).HasValue(),
                   g_mappingOwner.GetMapping(0U).GetError() == Ia64InitialUserImageMappingAccessError::NotReady,
                   !g_mappingOwner.GetFailure().HasValue(),
                   g_mappingOwner.GetFailure().GetError() == Ia64InitialUserImageMappingAccessError::NoFailure);

    const Foundation::Size pristineMappingCount = root.GetSnapshot().MappingCount;
    const auto memoryNotReady = g_mappingOwner.Install(g_unreadyMemoryOwner, g_runtimeArena.Owners[0U], g_registry);
    ASTERIA_EXPECT(runner, "a not-ready memory owner is rejected without crossing the retry cut",
                   !memoryNotReady.HasValue(),
                   memoryNotReady.GetError().Error == Ia64InitialUserImageMappingOwnerError::MemoryOwnerNotReady,
                   g_mappingOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Fresh,
                   g_mappingOwner.GetFailure().HasValue(), root.GetSnapshot().MappingCount == pristineMappingCount);

    ASTERIA_EXPECT(runner, "the embedded-like P64 fixture stages and materializes five ranges",
                   MaterializeCurrentImage(domainIdentity));
    const auto memorySummary = g_memoryOwner.GetSummary();
    if (!memorySummary.HasValue())
    {
        ASTERIA_EXPECT(runner, "the materialized fixture exposes its summary", false);
        return;
    }

    const auto bspNotReady = g_mappingOwner.Install(g_memoryOwner, g_runtimeArena.Owners[1U], g_registry);
    ASTERIA_EXPECT(runner, "a not-ready BSP dependency preserves a retryable owner and root", !bspNotReady.HasValue(),
                   bspNotReady.GetError().Error == Ia64InitialUserImageMappingOwnerError::BspRuntimeMemoryNotReady,
                   g_mappingOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Fresh,
                   root.GetSnapshot().MappingCount == pristineMappingCount);

    const auto wrongRegistry = g_mappingOwner.Install(g_memoryOwner, g_runtimeArena.Owners[0U], g_otherRegistry);
    ASTERIA_EXPECT(runner, "a foreign registry capability fails preflight without mutation", !wrongRegistry.HasValue(),
                   wrongRegistry.GetError().Error == Ia64InitialUserImageMappingOwnerError::InvalidMemorySummary,
                   g_mappingOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Fresh,
                   root.GetSnapshot().MappingCount == pristineMappingCount, g_otherRegistry.GetActiveCount() == 0U);

    const auto conflictRuntime = g_runtimeArena.Owners[1U].PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_preparation, ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const auto conflictRoot = g_runtimeArena.Owners[1U].BorrowReadyInitialComponentRoot();
    bool conflictPrepared{false};
    Foundation::Size conflictMappingCount{0U};
    if (conflictRuntime.HasValue() && conflictRoot.HasValue() &&
        (conflictRoot.GetValue().AddressSpaceStorage != nullptr))
    {
        quiescer.Add(*conflictRoot.GetValue().AddressSpaceStorage);
        conflictPrepared =
            PrepareAndInstallConflictMapping(*conflictRoot.GetValue().AddressSpaceStorage, domainIdentity);
        conflictMappingCount = conflictRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount;
    }
    const auto conflictUsage = g_resourceDomain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "the conflict fixture installs one exact range into a second private root",
                   conflictPrepared);
    if (!conflictPrepared)
    {
        return;
    }

    const auto conflict = g_conflictOwner.Install(g_memoryOwner, g_runtimeArena.Owners[1U], g_registry);
    ASTERIA_EXPECT(runner, "an existing virtual conflict is diagnosed entirely in retryable preflight",
                   !conflict.HasValue(),
                   conflict.GetError().Error == Ia64InitialUserImageMappingOwnerError::GuardedVirtualRangeOccupied,
                   g_conflictOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Fresh,
                   g_conflictOwner.GetFailure().HasValue(),
                   conflictRoot.GetValue().AddressSpaceStorage->GetSnapshot().MappingCount == conflictMappingCount,
                   ResourceUsageIsEqual(conflictUsage, g_resourceDomain.GetSnapshot().Usage));

    const bool terminalAliasPrepared =
        PrepareAndInstallExecutableDataAlias(*conflictRoot.GetValue().AddressSpaceStorage, domainIdentity);
    const auto terminalRuntime = g_runtimeArena.Owners[2U].PrepareAndAdmit(
        CpuId{0U}, Capabilities(), g_preparation, ResourceDomainId{ChargeOwnerValue}, g_registry, g_issuer);
    const auto terminalRoot = g_runtimeArena.Owners[2U].BorrowReadyInitialComponentRoot();
    const bool terminalFixtureReady = terminalAliasPrepared && terminalRuntime.HasValue() && terminalRoot.HasValue() &&
                                      (terminalRoot.GetValue().AddressSpaceStorage != nullptr);
    ASTERIA_EXPECT(runner, "a third private root exposes the foreign physical-policy failure fixture",
                   terminalFixtureReady);
    if (!terminalFixtureReady)
    {
        return;
    }
    AddressSpace& terminalSpace = *terminalRoot.GetValue().AddressSpaceStorage;
    quiescer.Add(terminalSpace);
    const Foundation::Size terminalInitialCount = terminalSpace.GetSnapshot().MappingCount;
    const auto terminal = g_terminalOwner.Install(g_memoryOwner, g_runtimeArena.Owners[2U], g_registry);
    const auto terminalFailure = g_terminalOwner.GetFailure();
    const auto retainedCode = terminalSpace.TryGetMapping(terminalInitialCount);
    const auto retainedReadOnly = terminalSpace.TryGetMapping(terminalInitialCount + 1U);
    const auto codePlan = g_memoryOwner.GetRangePlan(0U);
    const auto readOnlyPlan = g_memoryOwner.GetRangePlan(1U);
    const auto retainedCodeRange = g_memoryOwner.BorrowRange(0U);
    const auto retainedReadOnlyRange = g_memoryOwner.BorrowRange(1U);
    const auto rejectedDataRange = g_memoryOwner.BorrowRange(2U);
    const auto terminalSnapshot = terminalSpace.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "a post-cut Frame policy failure retains the first two completed mappings", !terminal.HasValue(),
        terminal.GetError().Error == Ia64InitialUserImageMappingOwnerError::AddressSpaceMappingRejected,
        terminal.GetError().AddressSpace.Error == AddressSpaceError::PhysicalWriteExecuteAlias,
        terminal.GetError().RangeIndex == 2U, !terminal.GetError().RootMayBeFaulted,
        g_terminalOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Failed, terminalFailure.HasValue(),
        terminalFailure.GetValue().Error == terminal.GetError().Error,
        terminalSnapshot.State == AddressSpaceState::Ready, terminalSnapshot.MappingCount == terminalInitialCount + 2U,
        codePlan.HasValue(), readOnlyPlan.HasValue(), retainedCode.HasValue(), retainedReadOnly.HasValue(),
        retainedCodeRange.HasValue(), retainedReadOnlyRange.HasValue(), rejectedDataRange.HasValue(),
        retainedCodeRange.GetValue().MemoryObjectStorage != nullptr,
        retainedReadOnlyRange.GetValue().MemoryObjectStorage != nullptr,
        rejectedDataRange.GetValue().MemoryObjectStorage != nullptr,
        retainedCodeRange.GetValue().MemoryObjectStorage->GetSnapshot().ActiveMappings == 1U,
        retainedReadOnlyRange.GetValue().MemoryObjectStorage->GetSnapshot().ActiveMappings == 1U,
        rejectedDataRange.GetValue().MemoryObjectStorage->GetSnapshot().ActiveMappings == 0U,
        RecordIsExact(retainedCode.GetValue(), codePlan.GetValue(), memorySummary.GetValue()),
        RecordIsExact(retainedReadOnly.GetValue(), readOnlyPlan.GetValue(), memorySummary.GetValue()));
    const auto terminalReplay = g_terminalOwner.Install(g_memoryOwner, g_runtimeArena.Owners[2U], g_registry);
    ASTERIA_EXPECT(runner, "the terminal owner rejects replay without discarding retained mappings",
                   !terminalReplay.HasValue(),
                   terminalReplay.GetError().Error == Ia64InitialUserImageMappingOwnerError::AlreadyAttempted,
                   terminalSpace.GetSnapshot().MappingCount == terminalInitialCount + 2U);

    const AddressSpaceError terminalQuiesce = terminalSpace.Quiesce();
    const AddressSpaceError conflictQuiesce = conflictRoot.GetValue().AddressSpaceStorage->Quiesce();
    bool sourceMappingsReleased{true};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        const auto range = g_memoryOwner.BorrowRange(index);
        sourceMappingsReleased = sourceMappingsReleased && range.HasValue() &&
                                 (range.GetValue().MemoryObjectStorage != nullptr) &&
                                 (range.GetValue().MemoryObjectStorage->GetSnapshot().ActiveMappings == 0U);
    }
    ASTERIA_EXPECT(runner, "failed and conflict private roots quiesce before the success case",
                   terminalQuiesce == AddressSpaceError::None, conflictQuiesce == AddressSpaceError::None,
                   sourceMappingsReleased, g_terminalConflictMemoryObject.GetSnapshot().ActiveMappings == 0U);

    const auto resourceBefore = g_resourceDomain.GetSnapshot();
    const Foundation::Size registryBefore = g_registry.GetActiveCount();
    const auto installed = g_mappingOwner.Install(g_memoryOwner, g_runtimeArena.Owners[0U], g_registry);
    const auto resourceAfter = g_resourceDomain.GetSnapshot();
    const auto finalRoot = root.GetSnapshot();
    ASTERIA_EXPECT(runner, "zero-resident direct completion maps without registry or resource accounting changes",
                   installed.HasValue(), g_mappingOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Ready,
                   finalRoot.State == AddressSpaceState::Ready, finalRoot.ResidentProcessorMask == 0U,
                   finalRoot.TargetProcessorMask == 0U, finalRoot.AcknowledgedProcessorMask == 0U,
                   !finalRoot.ActivatingProcessor.IsValid(), !finalRoot.TransitionProcessor.IsValid(),
                   g_registry.GetActiveCount() == registryBefore,
                   ResourceUsageIsEqual(resourceBefore.Usage, resourceAfter.Usage));
    if (!installed.HasValue())
    {
        return;
    }

    const auto summaryResult = g_mappingOwner.GetSummary();
    ASTERIA_EXPECT(runner, "the owner publishes the exact private-root identity, counts, and sequence interval",
                   summaryResult.HasValue(),
                   summaryResult.GetValue().AddressSpaceIdentity.IsEqualTo(privateRoot.GetValue().AddressSpaceIdentity),
                   summaryResult.GetValue().AggregateFrame.IsEqualTo(memorySummary.GetValue().AggregateFrame),
                   summaryResult.GetValue().InitialMappingCount == pristineMappingCount,
                   summaryResult.GetValue().FinalMappingCount == pristineMappingCount + 5U,
                   summaryResult.GetValue().RangeCount == 5U, summaryResult.GetValue().FirstMappingSequence == 1U,
                   summaryResult.GetValue().LastMappingSequence == 5U,
                   finalRoot.MappingCount == summaryResult.GetValue().FinalMappingCount);
    if (!summaryResult.HasValue())
    {
        return;
    }

    constexpr Foundation::UInt64 expectedVirtualStarts[]{
        CodeVirtualStart & ~(PageSizeBytes - 1U),
        ReadOnlyVirtualStart & ~(PageSizeBytes - 1U),
        DataVirtualStart & ~(PageSizeBytes - 1U),
        Platform::Ia64::Ia64InitialUserMemoryStackVirtualStart,
        Platform::Ia64::Ia64InitialUserRseBackingStoreVirtualStart,
    };
    constexpr Foundation::UInt64 expectedPages[]{1U, 2U, 4U, 4U, 4U};
    constexpr Foundation::UInt64 expectedFrameOffsets[]{0U, 1U, 3U, 7U, 11U};
    constexpr Foundation::UInt8 expectedPermissions[]{13U, 9U, 11U, 11U, 11U};
    bool mappingsAreExact{true};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        const auto memoryPlan = g_memoryOwner.GetRangePlan(index);
        const auto installedMapping = g_mappingOwner.GetMapping(index);
        const auto rootRecord = root.TryGetMapping(pristineMappingCount + index);
        if (!memoryPlan.HasValue() || !installedMapping.HasValue() || !rootRecord.HasValue())
        {
            mappingsAreExact = false;
            continue;
        }
        const auto& plan = memoryPlan.GetValue();
        const auto& mapping = installedMapping.GetValue();
        const auto memoryRange = g_memoryOwner.BorrowRange(index);
        mappingsAreExact =
            mappingsAreExact && (plan.VirtualStart == expectedVirtualStarts[index]) &&
            (plan.Pages.GetValue() == expectedPages[index]) && (plan.FrameOffsetPages == expectedFrameOffsets[index]) &&
            (plan.Permissions.GetBits() == expectedPermissions[index]) &&
            plan.Permissions.Contains(PagePermission::User) && (mapping.Plan.VirtualStart == plan.VirtualStart) &&
            (mapping.Plan.Pages.GetValue() == plan.Pages.GetValue()) &&
            (mapping.Plan.FrameOffsetPages == plan.FrameOffsetPages) &&
            mapping.Plan.MemoryObject.IsEqualTo(plan.MemoryObject) &&
            (mapping.Plan.Permissions.GetBits() == plan.Permissions.GetBits()) &&
            (mapping.Receipt.MappingSequence == index + 1U) &&
            (mapping.Receipt.MappingCount == pristineMappingCount + index + 1U) &&
            (mapping.Receipt.VirtualStart.GetValue() == plan.VirtualStart) &&
            (mapping.Receipt.Pages.GetValue() == plan.Pages.GetValue()) &&
            RecordIsExact(rootRecord.GetValue(), plan, memorySummary.GetValue()) && memoryRange.HasValue() &&
            (memoryRange.GetValue().MemoryObjectStorage != nullptr) &&
            (memoryRange.GetValue().MemoryObjectStorage->GetSnapshot().ActiveMappings == 1U);
    }
    ASTERIA_EXPECT(runner,
                   "all mappings preserve order, user permissions, physical slices, logical offset zero, and receipts",
                   mappingsAreExact);
    ASTERIA_EXPECT(runner, "stack and RSE guards stay absent and both ledgers end at their exact counts",
                   GuardsAreVacant(root, finalRoot.MappingCount),
                   !root.TryGetMapping(finalRoot.MappingCount).HasValue(), !g_mappingOwner.GetMapping(5U).HasValue(),
                   g_mappingOwner.GetMapping(5U).GetError() == Ia64InitialUserImageMappingAccessError::IndexOutOfBounds,
                   !g_mappingOwner.GetFailure().HasValue(),
                   g_mappingOwner.GetFailure().GetError() == Ia64InitialUserImageMappingAccessError::NoFailure);

    const Foundation::Size beforeReplay = root.GetSnapshot().MappingCount;
    const auto replay = g_mappingOwner.Install(g_memoryOwner, g_runtimeArena.Owners[0U], g_registry);
    ASTERIA_EXPECT(runner, "a ready owner rejects replay without changing its mappings", !replay.HasValue(),
                   replay.GetError().Error == Ia64InitialUserImageMappingOwnerError::AlreadyAttempted,
                   g_mappingOwner.GetState() == Ia64InitialUserImageMappingOwnerState::Ready,
                   root.GetSnapshot().MappingCount == beforeReplay);

    const AddressSpaceError successQuiesce = root.Quiesce();
    bool successMappingsReleased{true};
    for (Foundation::Size index{0U}; index < 5U; ++index)
    {
        const auto range = g_memoryOwner.BorrowRange(index);
        successMappingsReleased = successMappingsReleased && range.HasValue() &&
                                  (range.GetValue().MemoryObjectStorage != nullptr) &&
                                  (range.GetValue().MemoryObjectStorage->GetSnapshot().ActiveMappings == 0U);
    }
    ASTERIA_EXPECT(runner, "the successful mapped private root quiesces before process exit",
                   successQuiesce == AddressSpaceError::None, root.GetSnapshot().State == AddressSpaceState::Quiesced,
                   successMappingsReleased);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64InitialUserImageMappingOwnerTests"};
    Asteria::Test::TestMappingOwner(runner);
    return runner.Finish();
}
