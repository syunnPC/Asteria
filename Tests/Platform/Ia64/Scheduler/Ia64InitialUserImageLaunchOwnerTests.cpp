// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/BootstrapPageAllocator.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Ia64UserVirtualLayout.hpp"
#include "Platform/Ia64/Memory/BootstrapMappingPlan.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"
#include "Platform/Ia64/Scheduler/Ia64InitialUserImageLaunchOwner.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/Ia64HostedBootGeometry.hpp"
#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
constexpr Asteria::Foundation::Size TestImageSizeBytes{0x5000U};
constexpr Asteria::Foundation::Size TestBackingPageCount{136U};
constexpr Asteria::Foundation::Size TestBackingSizeBytes{TestBackingPageCount * 4096U};
static_assert(TestBackingSizeBytes == 557056U);
} // namespace

extern "C"
{
extern Asteria::Foundation::UInt8 AsteriaIa64InitialUserImageLaunchOwnerTestStorage[TestBackingSizeBytes];
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
AsteriaIa64LaunchUserMode(const Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan*) noexcept
{
    return 0U;
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
    ".global AsteriaIa64InitialUserImageLaunchOwnerTestStorage\n"
    ".type AsteriaIa64InitialUserImageLaunchOwnerTestStorage, @object\n"
    "AsteriaIa64InitialUserImageLaunchOwnerTestStorage:\n"
    ".zero 557056\n"
    ".size AsteriaIa64InitialUserImageLaunchOwnerTestStorage, "
    ". - AsteriaIa64InitialUserImageLaunchOwnerTestStorage\n"
    ".popsection\n");

namespace Asteria::Test
{
namespace
{
[[nodiscard]] Boot::BootFileRegion SourceRegion() noexcept
{
    Boot::BootFileRegion region{};
    region.PhysicalStart = reinterpret_cast<Foundation::UInt64>(AsteriaIa64InitialUserImageLaunchOwnerTestStorage);
    region.PageCount = (20480U + 4095U) / 4096U;
    return region;
}

using Boot::BootInfo;
using Boot::PhysicalMemoryKind;
using Boot::PhysicalMemoryRegion;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceSnapshot;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::BootstrapPageAllocator;
using Kernel::Memory::BootstrapPageAllocatorError;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RevocationError;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainError;
using Kernel::Protection::ProtectionDomainId;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Executable::Ia64InitialUserImageGuardPolicy;
using Platform::Ia64::Executable::Ia64InitialUserImageMappingOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryOwner;
using Platform::Ia64::Executable::Ia64InitialUserImageMemoryPurpose;
using Platform::Ia64::Executable::Ia64InitialUserImageNoSegment;
using Platform::Ia64::Executable::Ia64InitialUserImageOwner;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
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
using Platform::Ia64::Scheduler::Ia64InitialUserImageLaunchAccessError;
using Platform::Ia64::Scheduler::Ia64InitialUserImageBootstrapGrant;
using Platform::Ia64::Scheduler::Ia64InitialUserImageLaunchOwner;
using Platform::Ia64::Scheduler::Ia64InitialUserImageLaunchOwnerError;
using Platform::Ia64::Scheduler::Ia64InitialUserImageLaunchOwnerState;
using Platform::Ia64::Scheduler::Ia64UserModeLaunch;
using Platform::Ia64::Scheduler::Ia64UserModeLaunchError;
using Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState;
using Platform::Ia64::Scheduler::Ia64UserModeVirtualRange;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

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
constexpr Foundation::UInt64 BootstrapLeaseDeadline{100U};
constexpr Foundation::UInt64 BootstrapSealedSelector{0xA57E'21A6'4000'0001ULL};
constexpr Foundation::UInt64 DiagnosticLeaseDeadline{200U};
constexpr Foundation::UInt64 DiagnosticSealedSelector{0xA57E'21A6'4000'0002ULL};

class TestBootstrapObject final : public IKernelObject
{
public:
    constexpr TestBootstrapObject(const ObjectType type, const ResourceDomainId chargeOwner) noexcept
        : m_type{type}, m_chargeOwner{chargeOwner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_chargeOwner;
    }

private:
    ObjectType m_type{ObjectType::Invalid};
    ResourceDomainId m_chargeOwner{};
};

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
alignas(PageSizeBytes) constinit Foundation::UInt8 g_materializedArena[TestBackingSizeBytes]{};
alignas(PageSizeBytes) constinit Foundation::UInt8 g_supervisorMemoryStack[16U * 1024U]{};
alignas(PageSizeBytes) constinit Foundation::UInt8 g_supervisorRseBackingStore[16U * 1024U]{};

static_assert(alignof(RuntimeArena) == PageSizeBytes);
static_assert((sizeof(RuntimeArena) & (PageSizeBytes - 1U)) == 0U);
static_assert(TestSupport::Ia64HostedRuntimeMemoryPageBytes(sizeof(RuntimeArena)) == 0x40'0000U);
static_assert(!std::is_copy_constructible_v<Ia64InitialUserImageLaunchOwner>);
static_assert(!std::is_move_constructible_v<Ia64InitialUserImageLaunchOwner>);

[[nodiscard]] Foundation::UInt64 AddressOf(const void* const value) noexcept
{
    return static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(value));
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
    Foundation::UInt8* const header = AsteriaIa64InitialUserImageLaunchOwnerTestStorage + GetProgramHeaderOffset(index);
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
        AsteriaIa64InitialUserImageLaunchOwnerTestStorage[static_cast<Foundation::Size>(fileOffset + index)] =
            static_cast<Foundation::UInt8>(seed + static_cast<Foundation::UInt8>(index));
    }
}

void MakeCurrentImage() noexcept
{
    static_cast<void>(std::memset(AsteriaIa64InitialUserImageLaunchOwnerTestStorage, 0, TestImageSizeBytes));
    Foundation::UInt8* const image = AsteriaIa64InitialUserImageLaunchOwnerTestStorage;
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

[[nodiscard]] bool PrepareRuntimeFoundation(ResourceDomainObjectId& resourceDomainIdentity) noexcept
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
    resourceDomainIdentity = ResourceDomainObjectId{g_registry, registered.GetValue()};
    const auto runtime = g_runtimeArena.Owner.PrepareAndAdmit(CpuId{0U}, Capabilities(), g_preparation, chargeOwner,
                                                              g_registry, g_issuer);
    return runtime.HasValue() && (g_runtimeArena.Owner.GetSnapshot().State == Ia64BspRuntimeMemoryState::Ready);
}

[[nodiscard]] bool PrepareImageOwners(const ResourceDomainObjectId resourceDomainIdentity) noexcept
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
                            {AsteriaIa64InitialUserImageLaunchOwnerTestStorage, 20480U}, SourceRegion());
    if (!staged.HasValue())
    {
        return false;
    }
    const auto materialized =
        g_memoryOwner.Materialize(g_stagedOwner, g_registry, g_issuer, resourceDomainIdentity, g_resourceDomain);
    if (!materialized.HasValue())
    {
        return false;
    }
    return g_mappingOwner.Install(g_memoryOwner, g_runtimeArena.Owner, g_registry).HasValue();
}

[[nodiscard]] constexpr bool UsageIsEqual(const Kernel::Resource::ResourceDomainUsage& first,
                                          const Kernel::Resource::ResourceDomainUsage& second) noexcept
{
    return (first.CommittedMemoryPages == second.CommittedMemoryPages) &&
           (first.ResidentMemoryPages == second.ResidentMemoryPages) &&
           (first.KernelMetadataBytes == second.KernelMetadataBytes) &&
           (first.CapabilityHandles == second.CapabilityHandles) && (first.IpcQueueBytes == second.IpcQueueBytes) &&
           (first.Threads == second.Threads) && (first.ChildDomains == second.ChildDomains);
}

[[nodiscard]] constexpr bool AddressSpaceSnapshotsAreEqual(const AddressSpaceSnapshot& first,
                                                           const AddressSpaceSnapshot& second) noexcept
{
    return first.Identity.IsEqualTo(second.Identity) && (first.MappingCount == second.MappingCount) &&
           (first.ShootdownSequence == second.ShootdownSequence) &&
           (first.TransitionSequence == second.TransitionSequence) && (first.State == second.State) &&
           (first.ResidentProcessorMask == second.ResidentProcessorMask) &&
           (first.ActivatingProcessor.GetValue() == second.ActivatingProcessor.GetValue()) &&
           (first.TransitionProcessor.GetValue() == second.TransitionProcessor.GetValue()) &&
           (first.TargetProcessorMask == second.TargetProcessorMask) &&
           (first.AcknowledgedProcessorMask == second.AcknowledgedProcessorMask);
}

[[nodiscard]] Ia64UserModeVirtualRange SupervisorMemoryStack() noexcept
{
    return Ia64UserModeVirtualRange{
        .VirtualStart = AddressOf(g_supervisorMemoryStack),
        .SizeBytes = sizeof(g_supervisorMemoryStack),
    };
}

[[nodiscard]] Ia64UserModeVirtualRange SupervisorRseBackingStore() noexcept
{
    return Ia64UserModeVirtualRange{
        .VirtualStart = AddressOf(g_supervisorRseBackingStore),
        .SizeBytes = sizeof(g_supervisorRseBackingStore),
    };
}

[[nodiscard]] constexpr CapabilityRights BootstrapRights() noexcept
{
    return CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Invoke).With(CapabilityRight::Read);
}

[[nodiscard]] constexpr Ia64InitialUserImageBootstrapGrant
BootstrapGrant(const ObjectRegistry& registry, const ObjectId target,
               const ObjectType expectedType = ObjectType::Endpoint, const CapabilityRights rights = BootstrapRights(),
               const MonotonicTime leaseDeadline = MonotonicTime{BootstrapLeaseDeadline},
               const Foundation::UInt64 sealedObjectSelector = BootstrapSealedSelector) noexcept
{
    return Ia64InitialUserImageBootstrapGrant{
        .Registry = &registry,
        .Target = target,
        .ExpectedType = expectedType,
        .Rights = rights,
        .LeaseDeadline = leaseDeadline,
        .SealedObjectSelector = sealedObjectSelector,
    };
}

[[nodiscard]] constexpr Ia64InitialUserImageBootstrapGrant DiagnosticGrant(const ObjectRegistry& registry,
                                                                           const ObjectId target) noexcept
{
    return BootstrapGrant(registry, target, ObjectType::DiagnosticChannel,
                          CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Write),
                          MonotonicTime{DiagnosticLeaseDeadline}, DiagnosticSealedSelector);
}

[[nodiscard]] auto PrepareLaunch(Ia64InitialUserImageLaunchOwner& owner, const AddressSpaceId addressSpaceIdentity,
                                 RootCapabilityIssuer& issuer, RevocationRegistry& revocationRegistry,
                                 CapabilitySpace& capabilitySpace,
                                 const Ia64InitialUserImageBootstrapGrant& bootstrapGrant,
                                 const Ia64InitialUserImageBootstrapGrant& diagnosticGrant,
                                 const ProtectionDomainId protectionDomainIdentity, ProtectionDomain& protectionDomain,
                                 const ResourceDomainObjectId resourceDomainIdentity,
                                 const Ia64UserModeVirtualRange supervisorMemoryStack,
                                 const Ia64UserModeVirtualRange supervisorRseBackingStore,
                                 const Foundation::UInt8 implementedVirtualAddressMostSignificantBit) noexcept
{
    return owner.Prepare(g_memoryOwner, g_mappingOwner, addressSpaceIdentity, g_registry, issuer, revocationRegistry,
                         capabilitySpace, bootstrapGrant, diagnosticGrant, protectionDomainIdentity, protectionDomain,
                         resourceDomainIdentity, g_resourceDomain, supervisorMemoryStack, supervisorRseBackingStore,
                         implementedVirtualAddressMostSignificantBit);
}

void TestLaunchOwner(TestContext& runner) noexcept
{
    ResourceDomainObjectId resourceDomainIdentity{};
    ASTERIA_EXPECT(runner, "fixture prepares the exact materialized and mapped initial image",
                   PrepareRuntimeFoundation(resourceDomainIdentity), PrepareImageOwners(resourceDomainIdentity));
    const auto root = g_runtimeArena.Owner.BorrowReadyInitialComponentRoot();
    if (!root.HasValue() || (root.GetValue().AddressSpaceStorage == nullptr) || !resourceDomainIdentity.IsValid())
    {
        ASTERIA_EXPECT(runner, "fixture exposes its private root", false);
        return;
    }
    AddressSpace& addressSpace = *root.GetValue().AddressSpaceStorage;
    const AddressSpaceId addressSpaceIdentity = root.GetValue().AddressSpaceIdentity;

    ManualClock clock{1U};
    RevocationRegistry revocationRegistry{g_registrySynchronization};
    RevocationRegistry foreignRevocationRegistry{g_registrySynchronization};
    CapabilitySpace capabilitySpace{g_registry, revocationRegistry, clock, 0x0123'4567'89AB'CDEFULL,
                                    0xFEDC'BA98'7654'3210ULL};
    ProtectionDomain protectionDomain;
    const auto initialized = protectionDomain.Initialize(g_registry, resourceDomainIdentity, g_resourceDomain,
                                                         addressSpaceIdentity, capabilitySpace);
    const auto registered = g_issuer.CreateObject(protectionDomain);
    const ProtectionDomainId protectionDomainIdentity =
        registered.HasValue() ? ProtectionDomainId{g_registry, registered.GetValue()} : ProtectionDomainId{};
    const auto bound = protectionDomain.BindIdentity(g_registry, protectionDomainIdentity);
    TestBootstrapObject bootstrapObject{ObjectType::Endpoint, g_resourceDomain.GetIdentifier()};
    TestBootstrapObject wrongChargeBootstrapObject{ObjectType::Endpoint, ResourceDomainId{ChargeOwnerValue + 1U}};
    TestBootstrapObject expiredLeaseBootstrapObject{ObjectType::Endpoint, g_resourceDomain.GetIdentifier()};
    TestBootstrapObject diagnosticObject{ObjectType::DiagnosticChannel, g_resourceDomain.GetIdentifier()};
    TestBootstrapObject wrongChargeDiagnosticObject{ObjectType::DiagnosticChannel,
                                                    ResourceDomainId{ChargeOwnerValue + 1U}};
    const auto bootstrapRegistered = g_issuer.CreateObject(bootstrapObject);
    const auto wrongChargeBootstrapRegistered = g_issuer.CreateObject(wrongChargeBootstrapObject);
    const auto expiredLeaseBootstrapRegistered = g_issuer.CreateObject(expiredLeaseBootstrapObject);
    const auto diagnosticRegistered = g_issuer.CreateObject(diagnosticObject);
    const auto wrongChargeDiagnosticRegistered = g_issuer.CreateObject(wrongChargeDiagnosticObject);
    ASTERIA_EXPECT(runner, "fixture binds the execution identity and generic bootstrap targets",
                   initialized.Error == ProtectionDomainError::None, registered.HasValue(),
                   bound.Error == ProtectionDomainError::None, bootstrapRegistered.HasValue(),
                   wrongChargeBootstrapRegistered.HasValue(), expiredLeaseBootstrapRegistered.HasValue(),
                   diagnosticRegistered.HasValue(), wrongChargeDiagnosticRegistered.HasValue(),
                   capabilitySpace.IsReady(), capabilitySpace.GetActiveCount() == 0U);
    if (!registered.HasValue() || (bound.Error != ProtectionDomainError::None) || !bootstrapRegistered.HasValue() ||
        !wrongChargeBootstrapRegistered.HasValue() || !expiredLeaseBootstrapRegistered.HasValue() ||
        !diagnosticRegistered.HasValue() || !wrongChargeDiagnosticRegistered.HasValue())
    {
        return;
    }
    const Ia64InitialUserImageBootstrapGrant bootstrapGrant =
        BootstrapGrant(g_registry, bootstrapRegistered.GetValue());
    const Ia64InitialUserImageBootstrapGrant diagnosticGrant =
        DiagnosticGrant(g_registry, diagnosticRegistered.GetValue());

    Ia64InitialUserImageLaunchOwner unreadyOwner;
    const auto unready = unreadyOwner.Prepare(g_unreadyMemoryOwner, g_mappingOwner, addressSpaceIdentity, g_registry,
                                              g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
                                              diagnosticGrant, protectionDomainIdentity, protectionDomain,
                                              resourceDomainIdentity, g_resourceDomain, SupervisorMemoryStack(),
                                              SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "fresh accessors and an unready owner preserve retryable state", !unready.HasValue(),
                   unready.GetError().Error == Ia64InitialUserImageLaunchOwnerError::MemoryOwnerNotReady,
                   unreadyOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
                   unreadyOwner.GetFailure().HasValue(), !unreadyOwner.GetSummary().HasValue(),
                   !unreadyOwner.GetLaunchRequest().HasValue(),
                   unreadyOwner.GetSummary().GetError() == Ia64InitialUserImageLaunchAccessError::NotReady,
                   unreadyOwner.GetLaunchRequest().GetError() == Ia64InitialUserImageLaunchAccessError::NotReady);

    Ia64InitialUserImageLaunchOwner wrongRootOwner;
    const auto wrongRoot =
        PrepareLaunch(wrongRootOwner, AddressSpaceId{g_otherRegistry, addressSpaceIdentity.GetObjectId()}, g_issuer,
                      revocationRegistry, capabilitySpace, bootstrapGrant, diagnosticGrant, protectionDomainIdentity,
                      protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(), SupervisorRseBackingStore(),
                      ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "a foreign expected private root is rejected in preflight", !wrongRoot.HasValue(),
                   wrongRoot.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidExpectedAddressSpace,
                   wrongRootOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh);

    Ia64InitialUserImageLaunchOwner wrongIssuerOwner;
    const auto wrongIssuer = PrepareLaunch(wrongIssuerOwner, addressSpaceIdentity, g_otherIssuer, revocationRegistry,
                                           capabilitySpace, bootstrapGrant, diagnosticGrant, protectionDomainIdentity,
                                           protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
                                           SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "a root issuer from another registry is retryably rejected", !wrongIssuer.HasValue(),
                   wrongIssuer.GetError().Error == Ia64InitialUserImageLaunchOwnerError::IssuerRegistryMismatch,
                   wrongIssuerOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh);

    Ia64InitialUserImageLaunchOwner wrongIdentityOwner;
    const auto wrongIdentity = PrepareLaunch(
        wrongIdentityOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        diagnosticGrant, ProtectionDomainId{g_otherRegistry, protectionDomainIdentity.GetObjectId()}, protectionDomain,
        resourceDomainIdentity, SupervisorMemoryStack(), SupervisorRseBackingStore(),
        ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "a foreign protection-domain identity cannot name runtime storage",
                   !wrongIdentity.HasValue(),
                   wrongIdentity.GetError().Error == Ia64InitialUserImageLaunchOwnerError::ProtectionDomainRejected,
                   wrongIdentityOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh);

    Ia64InitialUserImageLaunchOwner invalidGrantOwner;
    Ia64InitialUserImageBootstrapGrant foreignRegistryGrant = bootstrapGrant;
    foreignRegistryGrant.Registry = &g_otherRegistry;
    const auto foreignGrantRegistry = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, foreignRegistryGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant invalidTargetGrant = bootstrapGrant;
    invalidTargetGrant.Target = ObjectId{};
    const auto invalidGrantTarget = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, invalidTargetGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant unknownTypeGrant = bootstrapGrant;
    unknownTypeGrant.ExpectedType = static_cast<ObjectType>(0xFFU);
    const auto unknownGrantType = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, unknownTypeGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant invalidRightsGrant = bootstrapGrant;
    invalidRightsGrant.Rights = CapabilityRights{static_cast<CapabilityRight>(0x8000U)};
    const auto invalidGrantRights = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, invalidRightsGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant missingInspectGrant = bootstrapGrant;
    missingInspectGrant.Rights = CapabilityRights{CapabilityRight::Invoke};
    const auto missingGrantInspect = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, missingInspectGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant zeroLeaseGrant = bootstrapGrant;
    zeroLeaseGrant.LeaseDeadline = MonotonicTime{};
    const auto zeroGrantLease = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, zeroLeaseGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "malformed bootstrap grants are retryably rejected before authority changes",
                   !foreignGrantRegistry.HasValue(),
                   foreignGrantRegistry.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidBootstrapGrant,
                   !invalidGrantTarget.HasValue(),
                   invalidGrantTarget.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidBootstrapGrant,
                   !unknownGrantType.HasValue(),
                   unknownGrantType.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidBootstrapGrant,
                   !invalidGrantRights.HasValue(),
                   invalidGrantRights.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidBootstrapGrant,
                   !missingGrantInspect.HasValue(),
                   missingGrantInspect.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidBootstrapGrant,
                   !zeroGrantLease.HasValue(),
                   zeroGrantLease.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidBootstrapGrant,
                   invalidGrantOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
                   capabilitySpace.GetActiveCount() == 0U);

    Ia64InitialUserImageBootstrapGrant wrongTypeGrant = bootstrapGrant;
    wrongTypeGrant.ExpectedType = ObjectType::WaitSet;
    const auto wrongGrantType = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, wrongTypeGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const auto wrongGrantCharge =
        PrepareLaunch(invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace,
                      BootstrapGrant(g_registry, wrongChargeBootstrapRegistered.GetValue()), diagnosticGrant,
                      protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
                      SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "bootstrap target type and resource charge ownership are exact preflight invariants",
                   !wrongGrantType.HasValue(),
                   wrongGrantType.GetError().Error == Ia64InitialUserImageLaunchOwnerError::BootstrapTargetRejected,
                   wrongGrantType.GetError().Pin == Kernel::Object::ObjectPinError::TypeMismatch,
                   !wrongGrantCharge.HasValue(),
                   wrongGrantCharge.GetError().Error == Ia64InitialUserImageLaunchOwnerError::BootstrapTargetRejected,
                   wrongGrantCharge.GetError().Pin == Kernel::Object::ObjectPinError::Ok,
                   invalidGrantOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
                   capabilitySpace.GetActiveCount() == 0U);

    const ObjectRegistryError destroyedWrongChargeTarget =
        g_issuer.DestroyObject(wrongChargeBootstrapRegistered.GetValue());
    const auto collectedWrongChargeTarget = g_issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, "a retryable grant rejection releases its provisional target pin",
                   destroyedWrongChargeTarget == ObjectRegistryError::Ok, collectedWrongChargeTarget.HasValue(),
                   collectedWrongChargeTarget.GetValue().Instance ==
                       static_cast<IKernelObject*>(&wrongChargeBootstrapObject));

    // The DiagnosticChannel grant (r6/r7) passes the same checks as the
    // bootstrap grant and must name another object; Write alone cannot be
    // granted because the owner verifies each descriptor through Inspect.
    Ia64InitialUserImageBootstrapGrant writeOnlyDiagnosticGrant = diagnosticGrant;
    writeOnlyDiagnosticGrant.Rights = CapabilityRights{CapabilityRight::Write};
    const auto writeOnlyDiagnostic = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        writeOnlyDiagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
        SupervisorMemoryStack(), SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant foreignDiagnosticGrant = diagnosticGrant;
    foreignDiagnosticGrant.Registry = &g_otherRegistry;
    const auto foreignDiagnostic = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        foreignDiagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
        SupervisorMemoryStack(), SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const auto sharedDiagnosticTarget = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        bootstrapGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64InitialUserImageBootstrapGrant wrongTypeDiagnosticGrant = diagnosticGrant;
    wrongTypeDiagnosticGrant.ExpectedType = ObjectType::WaitSet;
    const auto wrongDiagnosticType = PrepareLaunch(
        invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        wrongTypeDiagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
        SupervisorMemoryStack(), SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const auto wrongDiagnosticCharge =
        PrepareLaunch(invalidGrantOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace,
                      bootstrapGrant, DiagnosticGrant(g_registry, wrongChargeDiagnosticRegistered.GetValue()),
                      protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
                      SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const ObjectRegistryError destroyedWrongChargeDiagnostic =
        g_issuer.DestroyObject(wrongChargeDiagnosticRegistered.GetValue());
    const auto collectedWrongChargeDiagnostic = g_issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(
        runner, "malformed, shared, mistyped, or foreign-charged diagnostic grants are retryably rejected and unpinned",
        !writeOnlyDiagnostic.HasValue(),
        writeOnlyDiagnostic.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidDiagnosticGrant,
        !foreignDiagnostic.HasValue(),
        foreignDiagnostic.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidDiagnosticGrant,
        !sharedDiagnosticTarget.HasValue(),
        sharedDiagnosticTarget.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidDiagnosticGrant,
        !wrongDiagnosticType.HasValue(),
        wrongDiagnosticType.GetError().Error == Ia64InitialUserImageLaunchOwnerError::DiagnosticTargetRejected,
        wrongDiagnosticType.GetError().Pin == Kernel::Object::ObjectPinError::TypeMismatch,
        !wrongDiagnosticCharge.HasValue(),
        wrongDiagnosticCharge.GetError().Error == Ia64InitialUserImageLaunchOwnerError::DiagnosticTargetRejected,
        wrongDiagnosticCharge.GetError().Pin == Kernel::Object::ObjectPinError::Ok,
        destroyedWrongChargeDiagnostic == ObjectRegistryError::Ok, collectedWrongChargeDiagnostic.HasValue(),
        collectedWrongChargeDiagnostic.GetValue().Instance == static_cast<IKernelObject*>(&wrongChargeDiagnosticObject),
        invalidGrantOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
        capabilitySpace.GetActiveCount() == 0U);

    const AddressSpaceSnapshot rootBefore = addressSpace.GetSnapshot();
    const auto resourceBefore = g_resourceDomain.GetSnapshot();
    const Foundation::Size registryBefore = g_registry.GetActiveCount();

    Ia64InitialUserImageLaunchOwner retryOwner;
    const auto badAddressWidth =
        PrepareLaunch(retryOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
                      diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
                      SupervisorMemoryStack(), SupervisorRseBackingStore(), 49U);
    Ia64UserModeVirtualRange badMemoryStack = SupervisorMemoryStack();
    ++badMemoryStack.VirtualStart;
    const auto badStack =
        PrepareLaunch(retryOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
                      diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
                      badMemoryStack, SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    Ia64UserModeVirtualRange overlappingRse = SupervisorMemoryStack();
    const auto badRse =
        PrepareLaunch(retryOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
                      diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
                      SupervisorMemoryStack(), overlappingRse, ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(
        runner, "address-width, supervisor stack, and RSE failures remain retryable", !badAddressWidth.HasValue(),
        badAddressWidth.GetError().Error == Ia64InitialUserImageLaunchOwnerError::InvalidImplementedAddressWidth,
        !badStack.HasValue(), badStack.GetError().Error == Ia64InitialUserImageLaunchOwnerError::LaunchRequestRejected,
        badStack.GetError().Launch == Ia64UserModeLaunchError::InvalidSupervisorMemoryStack,
        badStack.GetError().RangeIndex == Ia64InitialUserImageNoSegment, !badRse.HasValue(),
        badRse.GetError().Launch == Ia64UserModeLaunchError::SupervisorRangesOverlap,
        badRse.GetError().RangeIndex == Ia64InitialUserImageNoSegment,
        retryOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh, capabilitySpace.GetActiveCount() == 0U);

    const auto foreignRevocation = PrepareLaunch(
        retryOwner, addressSpaceIdentity, g_issuer, foreignRevocationRegistry, capabilitySpace, bootstrapGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "a foreign revocation registry is rejected before authority changes",
                   !foreignRevocation.HasValue(),
                   foreignRevocation.GetError().Error == Ia64InitialUserImageLaunchOwnerError::CapabilitySpaceRejected,
                   foreignRevocation.GetError().RangeIndex == Ia64InitialUserImageNoSegment,
                   retryOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Fresh,
                   retryOwner.GetFailure().HasValue(), capabilitySpace.GetActiveCount() == 0U);

    const auto prepared =
        PrepareLaunch(retryOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
                      diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity,
                      SupervisorMemoryStack(), SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "the same owner succeeds after retryable preflight failures", prepared.HasValue(),
                   retryOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Ready,
                   retryOwner.GetSummary().HasValue(), retryOwner.GetLaunchRequest().HasValue(),
                   !retryOwner.GetFailure().HasValue(),
                   retryOwner.GetFailure().GetError() == Ia64InitialUserImageLaunchAccessError::NoFailure,
                   capabilitySpace.GetActiveCount() == 2U);
    if (!prepared.HasValue())
    {
        return;
    }

    const auto summary = retryOwner.GetSummary();
    const auto request = retryOwner.GetLaunchRequest();
    const auto memorySummary = g_memoryOwner.GetSummary();
    const auto mappingSummary = g_mappingOwner.GetSummary();
    if (!summary.HasValue() || !request.HasValue() || !memorySummary.HasValue() || !mappingSummary.HasValue())
    {
        return;
    }

    const auto entryMapping = g_mappingOwner.GetMapping(summary.GetValue().EntryMappingIndex);
    const auto globalPointerMapping = g_mappingOwner.GetMapping(summary.GetValue().GlobalPointerMappingIndex);
    const auto memoryStackMapping = g_mappingOwner.GetMapping(summary.GetValue().MemoryStackMappingIndex);
    const auto rseMapping = g_mappingOwner.GetMapping(summary.GetValue().RseBackingStoreMappingIndex);
    const auto descriptor = capabilitySpace.Query(summary.GetValue().BootstrapCapability);
    const auto revocationEpoch = revocationRegistry.GetCurrentEpoch(summary.GetValue().RevocationDomain);
    const auto launchPlan = Ia64UserModeLaunch::Validate(request.GetValue());
    const CapabilityRights bootstrapRights = BootstrapRights();
    const Foundation::UInt64 codePageStart = CodeVirtualStart & ~(PageSizeBytes - 1U);
    ASTERIA_EXPECT(runner, "summary identifies the exact executable, GP, stack, and RSE mappings", summary.HasValue(),
                   request.HasValue(), memorySummary.HasValue(), mappingSummary.HasValue(), entryMapping.HasValue(),
                   globalPointerMapping.HasValue(), memoryStackMapping.HasValue(), rseMapping.HasValue(),
                   revocationEpoch.HasValue(), revocationEpoch.GetValue() == 1U,
                   summary.GetValue().AddressSpaceIdentity.IsEqualTo(addressSpaceIdentity),
                   summary.GetValue().ProtectionDomainIdentity.IsEqualTo(protectionDomainIdentity),
                   summary.GetValue().ResourceDomainIdentity.IsEqualTo(resourceDomainIdentity),
                   summary.GetValue().CapabilitySpaceIdentity.GetValue() == capabilitySpace.GetIdentifier().GetValue(),
                   summary.GetValue().RangeCount == 5U, summary.GetValue().EntryMappingIndex == 0U,
                   summary.GetValue().GlobalPointerMappingIndex == 2U, summary.GetValue().MemoryStackMappingIndex == 3U,
                   summary.GetValue().RseBackingStoreMappingIndex == 4U,
                   summary.GetValue().FirstMappingSequence == mappingSummary.GetValue().FirstMappingSequence,
                   summary.GetValue().LastMappingSequence == mappingSummary.GetValue().LastMappingSequence);

    ASTERIA_EXPECT(
        runner, "request retains nonzero GP, exact permissions, guarded ranges, and receipts",
        memorySummary.GetValue().Image.GlobalPointer == GlobalPointer, GlobalPointer != 0U,
        request.GetValue().Code.VirtualStart == codePageStart, request.GetValue().Code.SizeBytes == PageSizeBytes,
        request.GetValue().EntryBundleAddress == EntryBundleAddress,
        request.GetValue().InitialGlobalPointer == GlobalPointer,
        request.GetValue().MemoryStack.VirtualStart == Platform::Ia64::Ia64InitialUserMemoryStackVirtualStart,
        request.GetValue().MemoryStack.SizeBytes == 16U * 1024U,
        request.GetValue().RseBackingStore.VirtualStart == Platform::Ia64::Ia64InitialUserRseBackingStoreVirtualStart,
        request.GetValue().RseBackingStore.SizeBytes == 16U * 1024U,
        entryMapping.GetValue().Plan.Purpose == Ia64InitialUserImageMemoryPurpose::LoadSegment,
        entryMapping.GetValue().Plan.Permissions.Contains(PagePermission::Read),
        entryMapping.GetValue().Plan.Permissions.Contains(PagePermission::Execute),
        entryMapping.GetValue().Plan.Permissions.Contains(PagePermission::User),
        !entryMapping.GetValue().Plan.Permissions.Contains(PagePermission::Write),
        globalPointerMapping.GetValue().Plan.Permissions.Contains(PagePermission::Read),
        globalPointerMapping.GetValue().Plan.Permissions.Contains(PagePermission::User),
        memoryStackMapping.GetValue().Plan.GuardPolicy == Ia64InitialUserImageGuardPolicy::OnePageBeforeAndAfter,
        rseMapping.GetValue().Plan.GuardPolicy == Ia64InitialUserImageGuardPolicy::OnePageBeforeAndAfter,
        memoryStackMapping.GetValue().Receipt.MappingSequence == summary.GetValue().FirstMappingSequence + 3U,
        rseMapping.GetValue().Receipt.MappingSequence == summary.GetValue().LastMappingSequence, launchPlan.HasValue());

    const auto diagnosticDescriptor = capabilitySpace.Query(summary.GetValue().DiagnosticCapability);
    const auto diagnosticRevocationEpoch =
        revocationRegistry.GetCurrentEpoch(summary.GetValue().DiagnosticRevocationDomain);
    ASTERIA_EXPECT(
        runner, "r6/r7 carry the exact caller-supplied DiagnosticChannel capability in a root of its own",
        request.GetValue().InitialArguments.Register6 == summary.GetValue().DiagnosticCapability.GetSelector(),
        request.GetValue().InitialArguments.Register7 == summary.GetValue().DiagnosticCapability.GetAuthenticator(),
        summary.GetValue().DiagnosticCapability.GetSelector() != summary.GetValue().BootstrapCapability.GetSelector(),
        diagnosticRevocationEpoch.HasValue(), diagnosticRevocationEpoch.GetValue() == 1U,
        summary.GetValue().DiagnosticRevocationDomain.GetValue() != summary.GetValue().RevocationDomain.GetValue(),
        diagnosticDescriptor.HasValue(),
        diagnosticDescriptor.GetValue().Object.GetValue() == diagnosticRegistered.GetValue().GetValue(),
        diagnosticDescriptor.GetValue().Type == ObjectType::DiagnosticChannel,
        diagnosticDescriptor.GetValue().Rights.IsEqualTo(diagnosticGrant.Rights),
        diagnosticDescriptor.GetValue().RevocationDomain.GetValue() ==
            summary.GetValue().DiagnosticRevocationDomain.GetValue(),
        diagnosticDescriptor.GetValue().LeaseDeadline.GetTicks() == DiagnosticLeaseDeadline,
        diagnosticDescriptor.GetValue().SealedObjectSelector == DiagnosticSealedSelector,
        diagnosticDescriptor.GetValue().ChargeOwner.GetValue() == g_resourceDomain.GetIdentifier().GetValue());

    ASTERIA_EXPECT(
        runner, "r4/r5 carry the exact caller-supplied bootstrap capability",
        request.GetValue().SupervisorContinuation != nullptr,
        request.GetValue().SupervisorContinuation->State == Ia64UserModeSupervisorContinuationState::Fresh,
        request.GetValue().SupervisorContinuation->MemoryStack.VirtualStart == SupervisorMemoryStack().VirtualStart,
        request.GetValue().SupervisorContinuation->MemoryStack.SizeBytes == SupervisorMemoryStack().SizeBytes,
        request.GetValue().SupervisorContinuation->RseBackingStore.VirtualStart ==
            SupervisorRseBackingStore().VirtualStart,
        request.GetValue().SupervisorContinuation->RseBackingStore.SizeBytes == SupervisorRseBackingStore().SizeBytes,
        request.GetValue().InitialArguments.Register4 == summary.GetValue().BootstrapCapability.GetSelector(),
        request.GetValue().InitialArguments.Register5 == summary.GetValue().BootstrapCapability.GetAuthenticator(),
        descriptor.HasValue(), descriptor.GetValue().Object.GetValue() == bootstrapRegistered.GetValue().GetValue(),
        descriptor.GetValue().Type == ObjectType::Endpoint, descriptor.GetValue().Rights.IsEqualTo(bootstrapRights),
        descriptor.GetValue().RevocationDomain.GetValue() == summary.GetValue().RevocationDomain.GetValue(),
        descriptor.GetValue().LeaseDeadline.GetTicks() == BootstrapLeaseDeadline,
        descriptor.GetValue().SealedObjectSelector == BootstrapSealedSelector,
        descriptor.GetValue().ChargeOwner.GetValue() == g_resourceDomain.GetIdentifier().GetValue());

    {
        auto inspectLookup = capabilitySpace.Lookup(summary.GetValue().BootstrapCapability, ObjectType::Endpoint,
                                                    CapabilityOperation::Inspect);
        auto invokeLookup = capabilitySpace.Lookup(summary.GetValue().BootstrapCapability, ObjectType::Endpoint,
                                                   CapabilityOperation::Invoke);
        auto readLookup = capabilitySpace.Lookup(summary.GetValue().BootstrapCapability, ObjectType::Endpoint,
                                                 CapabilityOperation::Read);
        const auto writeLookup = capabilitySpace.Lookup(summary.GetValue().BootstrapCapability, ObjectType::Endpoint,
                                                        CapabilityOperation::Write);
        ASTERIA_EXPECT(
            runner,
            "generic bootstrap authority has exact rights and leaves registry, resource, and root state unchanged",
            inspectLookup.HasValue(),
            &inspectLookup.GetValue().GetObject() == static_cast<Kernel::Object::IKernelObject*>(&bootstrapObject),
            invokeLookup.HasValue(),
            &invokeLookup.GetValue().GetObject() == static_cast<Kernel::Object::IKernelObject*>(&bootstrapObject),
            readLookup.HasValue(),
            &readLookup.GetValue().GetObject() == static_cast<Kernel::Object::IKernelObject*>(&bootstrapObject),
            !writeLookup.HasValue(), writeLookup.GetError() == CapabilityError::RightsDenied,
            g_registry.GetActiveCount() == registryBefore,
            UsageIsEqual(resourceBefore.Usage, g_resourceDomain.GetSnapshot().Usage),
            AddressSpaceSnapshotsAreEqual(rootBefore, addressSpace.GetSnapshot()));
    }

    Ia64InitialUserImageBootstrapGrant replacementReplayGrant = bootstrapGrant;
    ++replacementReplayGrant.SealedObjectSelector;
    const auto replay = PrepareLaunch(retryOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace,
                                      replacementReplayGrant, diagnosticGrant, protectionDomainIdentity,
                                      protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
                                      SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const auto replayDescriptor = capabilitySpace.Query(summary.GetValue().BootstrapCapability);
    ASTERIA_EXPECT(
        runner, "a ready launch owner rejects a replacement grant without minting or changing authority",
        !replay.HasValue(), replay.GetError().Error == Ia64InitialUserImageLaunchOwnerError::AlreadyAttempted,
        retryOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Ready, capabilitySpace.GetActiveCount() == 2U,
        replayDescriptor.HasValue(), replayDescriptor.GetValue().Object.GetValue() == bootstrapGrant.Target.GetValue(),
        replayDescriptor.GetValue().Rights.IsEqualTo(bootstrapGrant.Rights),
        replayDescriptor.GetValue().LeaseDeadline.GetTicks() == bootstrapGrant.LeaseDeadline.GetTicks(),
        replayDescriptor.GetValue().SealedObjectSelector == bootstrapGrant.SealedObjectSelector);

    const bool closedBootstrap =
        (capabilitySpace.Close(summary.GetValue().BootstrapCapability) == CapabilityError::Ok) &&
        (capabilitySpace.Close(summary.GetValue().DiagnosticCapability) == CapabilityError::Ok);
    Ia64InitialUserImageLaunchOwner expiredLeaseOwner;
    Ia64InitialUserImageBootstrapGrant expiredLeaseGrant =
        BootstrapGrant(g_registry, expiredLeaseBootstrapRegistered.GetValue());
    expiredLeaseGrant.LeaseDeadline = MonotonicTime{1U};
    const auto expiredLease = PrepareLaunch(
        expiredLeaseOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, expiredLeaseGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const auto expiredLeaseReplay = PrepareLaunch(
        expiredLeaseOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const ObjectRegistryError destroyedExpiredTarget =
        g_issuer.DestroyObject(expiredLeaseBootstrapRegistered.GetValue());
    const auto retainedExpiredTarget = g_issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(
        runner, "a clock-expired grant fails terminally and retains its target pin after the authority commit cut",
        closedBootstrap, !expiredLease.HasValue(),
        expiredLease.GetError().Error == Ia64InitialUserImageLaunchOwnerError::CapabilityMintRejected,
        expiredLease.GetError().Capability == CapabilityError::Expired,
        expiredLeaseOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Failed, !expiredLeaseReplay.HasValue(),
        expiredLeaseReplay.GetError().Error == Ia64InitialUserImageLaunchOwnerError::AlreadyAttempted,
        destroyedExpiredTarget == ObjectRegistryError::Ok, !retainedExpiredTarget.HasValue(),
        retainedExpiredTarget.GetError() == ObjectRegistryError::NoDestructionReady,
        capabilitySpace.GetActiveCount() == 0U);

    // Four roots exist, two from the successful launch and two from the
    // expired lease. Leave exactly one: the bootstrap root is created, the
    // DiagnosticChannel root is not, and no capability has been minted yet.
    bool filledRevocationRegistry = true;
    for (Foundation::Size index{4U}; index < (RevocationRegistry::MaximumDomains - 1U); ++index)
    {
        filledRevocationRegistry = filledRevocationRegistry && revocationRegistry.CreateRoot().HasValue();
    }
    Ia64InitialUserImageLaunchOwner diagnosticRootOwner;
    const auto diagnosticRoot = PrepareLaunch(
        diagnosticRootOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "a missing DiagnosticChannel root fails terminally before either capability is minted",
                   filledRevocationRegistry, !diagnosticRoot.HasValue(),
                   diagnosticRoot.GetError().Error ==
                       Ia64InitialUserImageLaunchOwnerError::DiagnosticRevocationDomainCreationRejected,
                   diagnosticRoot.GetError().Revocation == RevocationError::DomainExhausted,
                   diagnosticRootOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Failed,
                   capabilitySpace.GetActiveCount() == 0U);

    Ia64InitialUserImageLaunchOwner terminalOwner;
    const auto terminal = PrepareLaunch(terminalOwner, addressSpaceIdentity, g_issuer, revocationRegistry,
                                        capabilitySpace, bootstrapGrant, diagnosticGrant, protectionDomainIdentity,
                                        protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
                                        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    const auto terminalReplay = PrepareLaunch(
        terminalOwner, addressSpaceIdentity, g_issuer, revocationRegistry, capabilitySpace, bootstrapGrant,
        diagnosticGrant, protectionDomainIdentity, protectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
        SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(runner, "authority-phase failure is terminal and preserves the first failure",
                   filledRevocationRegistry, !terminal.HasValue(),
                   terminal.GetError().Error == Ia64InitialUserImageLaunchOwnerError::RevocationDomainCreationRejected,
                   terminal.GetError().Revocation == RevocationError::DomainExhausted,
                   terminal.GetError().RangeIndex == Ia64InitialUserImageNoSegment,
                   terminalOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Failed, !terminalReplay.HasValue(),
                   terminalReplay.GetError().Error == Ia64InitialUserImageLaunchOwnerError::AlreadyAttempted,
                   capabilitySpace.GetActiveCount() == 0U);

    // A second execution identity with its own revocation registry and
    // capability space: the DiagnosticChannel mint expires after the
    // bootstrap capability exists, and the terminal failure retains it.
    RevocationRegistry expiryRevocationRegistry{g_registrySynchronization};
    CapabilitySpace expiryCapabilitySpace{g_registry, expiryRevocationRegistry, clock, 0x1357'9BDF'0246'8ACEULL,
                                          0xECA8'6420'FDB9'7531ULL};
    ProtectionDomain expiryProtectionDomain;
    const auto expiryInitialized = expiryProtectionDomain.Initialize(
        g_registry, resourceDomainIdentity, g_resourceDomain, addressSpaceIdentity, expiryCapabilitySpace);
    const auto expiryRegistered = g_issuer.CreateObject(expiryProtectionDomain);
    const ProtectionDomainId expiryProtectionDomainIdentity =
        expiryRegistered.HasValue() ? ProtectionDomainId{g_registry, expiryRegistered.GetValue()}
                                    : ProtectionDomainId{};
    const auto expiryBound = expiryProtectionDomain.BindIdentity(g_registry, expiryProtectionDomainIdentity);
    Ia64InitialUserImageBootstrapGrant expiredDiagnosticGrant = diagnosticGrant;
    expiredDiagnosticGrant.LeaseDeadline = MonotonicTime{1U};
    Ia64InitialUserImageLaunchOwner expiredDiagnosticOwner;
    const auto expiredDiagnostic =
        PrepareLaunch(expiredDiagnosticOwner, addressSpaceIdentity, g_issuer, expiryRevocationRegistry,
                      expiryCapabilitySpace, bootstrapGrant, expiredDiagnosticGrant, expiryProtectionDomainIdentity,
                      expiryProtectionDomain, resourceDomainIdentity, SupervisorMemoryStack(),
                      SupervisorRseBackingStore(), ValidVirtualAddressMostSignificantBit);
    ASTERIA_EXPECT(
        runner,
        "a clock-expired DiagnosticChannel grant fails terminally after the bootstrap mint and retains that capability",
        expiryInitialized.Error == ProtectionDomainError::None, expiryRegistered.HasValue(),
        expiryBound.Error == ProtectionDomainError::None, !expiredDiagnostic.HasValue(),
        expiredDiagnostic.GetError().Error == Ia64InitialUserImageLaunchOwnerError::DiagnosticCapabilityMintRejected,
        expiredDiagnostic.GetError().Capability == CapabilityError::Expired,
        expiredDiagnosticOwner.GetState() == Ia64InitialUserImageLaunchOwnerState::Failed,
        expiryCapabilitySpace.GetActiveCount() == 1U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64InitialUserImageLaunchOwnerTests"};
    Asteria::Test::TestLaunchOwner(runner);
    return runner.Finish();
}
