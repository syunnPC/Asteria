// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <new>
#include <thread>

#include "Boot/BootInfo.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/AddressSpacePool.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Ia64/Firmware/Ia64LoadedImageLayout.hpp"
#include "Platform/Ia64/Firmware/SalSystemTableParser.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpacePageTable.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeAddressSpaceSlots.hpp"
#include "Platform/Ia64/Memory/InitialAddressSpacePreparation.hpp"

#include "Support/ManualClock.hpp"
#include "Support/Ia64RuntimeMemoryReservation.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::Size g_activationCallCount{0U};
Foundation::UInt64 g_activationStatus{0U};
Foundation::UInt64 g_activationWords[7U]{};
} // Namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ActivateRuntimeRegions(
    const Asteria::Foundation::UInt64 regionRegister1, const Asteria::Foundation::UInt64 regionRegister2,
    const Asteria::Foundation::UInt64 regionRegister3, const Asteria::Foundation::UInt64 regionRegister4,
    const Asteria::Foundation::UInt64 regionRegister5, const Asteria::Foundation::UInt64 regionRegister6,
    const Asteria::Foundation::UInt64 regionRegister7) noexcept
{
    ++Asteria::Test::g_activationCallCount;
    Asteria::Test::g_activationWords[0U] = regionRegister1;
    Asteria::Test::g_activationWords[1U] = regionRegister2;
    Asteria::Test::g_activationWords[2U] = regionRegister3;
    Asteria::Test::g_activationWords[3U] = regionRegister4;
    Asteria::Test::g_activationWords[4U] = regionRegister5;
    Asteria::Test::g_activationWords[5U] = regionRegister6;
    Asteria::Test::g_activationWords[6U] = regionRegister7;
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
using Hal::Memory::AddressSpacePageTableError;
using Hal::Memory::AddressSpacePageTableMapResult;
using Hal::Memory::AddressSpacePageTableOwnershipError;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FrameKind;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PhysicalPageRange;
using Kernel::Memory::VirtualAddress;
using Kernel::Scheduler::CpuId;
using Platform::Ia64::Firmware::Ia64LoadedImageLayout;
using Platform::Ia64::Firmware::Ia64LoadedImageSection;
using Platform::Ia64::Firmware::Ia64LoadedImageSectionPurpose;
using Platform::Ia64::Firmware::SalSystemTableInformation;
using Platform::Ia64::Memory::ArchitectedMinimumInsertablePageSizeMask;
using Platform::Ia64::Memory::ArchitectedMinimumPurgePageSizeMask;
using Platform::Ia64::Memory::BootstrapReservedPageCount;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingError;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindings;
using Platform::Ia64::Memory::Ia64ActiveRuntimeTranslationBindingsWireStorage;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocationReceipt;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocator;
using Platform::Ia64::Memory::Ia64RegionIdentifierAllocatorError;
using Platform::Ia64::Memory::Ia64RuntimeAddressSpacePageTable;
using Platform::Ia64::Memory::Ia64RuntimeAddressSpacePageTableInitializationError;
using Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilities;
using Platform::Ia64::Memory::Ia64RuntimeProcessorCapabilitiesError;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedger;
using Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerError;
using Platform::Ia64::Memory::InitialAddressSpacePreparation;
using Platform::Ia64::Memory::InitialInterruptVectorTableSizeBytes;
using Platform::Ia64::Memory::InitialRseBackingStoreSizeBytes;
using Platform::Ia64::Memory::InitialTranslationRegionSet;
using Platform::Ia64::Memory::PalVirtualMemoryCapabilities;
using Platform::Ia64::Memory::RegionConfiguration;
using Platform::Ia64::Memory::TranslationCapabilities;
using Platform::Ia64::Memory::WritableInterruptVectorTableView;
using Platform::Ia64::Memory::WritableRseBackingStoreView;

constexpr Foundation::UInt64 AdapterSuccess{0U};
constexpr Foundation::UInt64 AdapterRejectedBeforeCommit{1U};
constexpr Foundation::UInt64 AdapterCommittedStateUnknown{2U};
constexpr Foundation::UInt64 ImageBase{0x0400'0000U};
constexpr Foundation::UInt64 ReservedBase{0x0100'0000U};
constexpr Foundation::UInt64 RegionOneBase{0x2000'0000'0020'0000ULL};
constexpr Foundation::UInt64 RegionTwoBase{0x4000'0000'0040'0000ULL};
constexpr Foundation::UInt64 PhysicalBase{0x0000'0000'0080'0000ULL};
constexpr Foundation::Size BackendCount{15U};
constexpr Foundation::Size OwnerCount{17U};

constinit InitialAddressSpacePreparation g_preparation;
constinit InitialAddressSpacePreparation g_equivalentPreparation;
constinit InitialAddressSpacePreparation g_freshPreparation;
constinit InitialMappingPlan g_differentSeed;
alignas(32U * 1024U) constinit Foundation::UInt8 g_interruptVectorTables[2U][InitialInterruptVectorTableSizeBytes]{};
alignas(4096U) constinit Foundation::UInt8 g_rseBackingStores[2U][InitialRseBackingStoreSizeBytes]{};

constinit Ia64RuntimeProcessorCapabilities g_profile;
constinit Ia64RuntimeProcessorCapabilities g_defaultProfile;
constinit PalVirtualMemoryCapabilities g_profileInputs[Kernel::Scheduler::MaximumProcessorCount]{};
constinit Ia64RegionIdentifierAllocator g_allocator;
constinit Ia64RegionIdentifierAllocationReceipt g_receipts[BackendCount]{};

constinit Ia64ActiveRuntimeTranslationBindings g_bindings;
constinit Ia64ActiveRuntimeTranslationBindings g_zeroBindings;
constinit Ia64ActiveRuntimeTranslationBindings g_physicalBindings;
alignas(4096U) constinit Ia64ActiveRuntimeTranslationBindings g_outsideCorridorBindings;
constinit Ia64RuntimeAddressSpacePageTable g_retryBackend;
constinit Ia64RuntimeAddressSpacePageTable g_capacityBackend;
constinit Ia64RuntimeAddressSpacePageTable g_activationBackend;
constinit Ia64RuntimeAddressSpacePageTable g_precommitBackend;
constinit Ia64RuntimeAddressSpacePageTable g_uncertaintyBackend;
constinit Ia64RuntimeAddressSpacePageTable g_zeroBindingsBackend;
constinit Ia64RuntimeAddressSpacePageTable g_recoveryBackend;
constinit Ia64RuntimeAddressSpacePageTable g_concurrentBackend;
constinit Ia64RuntimeAddressSpacePageTable g_physicalBackend;
constinit Ia64RuntimeAddressSpacePageTable g_physicalMismatchBackend;
constinit Ia64RuntimeAddressSpacePageTable g_physicalUncertaintyBackend;
alignas(4096U) constinit Ia64RuntimeAddressSpacePageTable g_outsideCorridorBackend;
constinit Ia64ActiveRuntimeTranslationBindings g_purgeBindings;
constinit Ia64RuntimeAddressSpacePageTable g_purgedBackend;
constinit Ia64RuntimeAddressSpacePageTable g_switchBackend;
constinit Ia64RuntimeAddressSpacePageTable g_unpurgedBackend;
constinit Ia64ActiveRuntimeTranslationBindings g_slotBindings;
constinit Platform::Ia64::Memory::Ia64RuntimeAddressSpaceSlots g_slots;

alignas(AddressSpace) constinit Foundation::UInt8 g_ownerStorage[OwnerCount][sizeof(AddressSpace)]{};
constinit AddressSpace* g_owners[OwnerCount]{};
constinit AddressSpacePageTableOwnershipError g_concurrentResults[2U]{
    AddressSpacePageTableOwnershipError::NotReady,
    AddressSpacePageTableOwnershipError::NotReady,
};

template <typename... Objects>
void ConfigureInstalledBindingFixture(Ia64ActiveRuntimeTranslationBindings& bindings, const CpuId processor,
                                      const Objects&... objects) noexcept
{
    Foundation::UInt64 start = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&bindings));
    Foundation::UInt64 end = start + sizeof(bindings);
    if constexpr (sizeof...(Objects) != 0U)
    {
        const auto include = [&start, &end](const auto& object) noexcept
        {
            const Foundation::UInt64 objectStart =
                static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&object));
            const Foundation::UInt64 objectEnd = objectStart + sizeof(object);
            start = objectStart < start ? objectStart : start;
            end = objectEnd > end ? objectEnd : end;
        };
        (include(objects), ...);
    }
    constexpr Foundation::UInt64 PageMask{4095U};
    start &= ~PageMask;
    end = (end + PageMask) & ~PageMask;

    // Private admission itself is covered by the focused runtime-IVT test.
    // This backend test consumes the explicitly named installed wire state.
    auto* const wire = reinterpret_cast<Ia64ActiveRuntimeTranslationBindingsWireStorage*>(&bindings);
    wire->RuntimeObjectStart = start;
    wire->RuntimeObjectEnd = end;
    wire->AdmissionOwnerAddress = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&bindings));
    wire->Slots[processor.GetValue()].StoreRelease(1U);
}

[[nodiscard]] bool ConstructOwners() noexcept
{
    // Placement construction keeps these large real owners in aligned
    // namespace-scope boot-lifetime storage. They acquire no pins or resources;
    // destruction is intentionally omitted only for this hosted fixture.
    for (Foundation::Size index{0U}; index < OwnerCount; ++index)
    {
        g_owners[index] = ::new (static_cast<void*>(g_ownerStorage[index])) AddressSpace{};
    }
    return true;
}

[[nodiscard]] AddressSpace& Owner(const Foundation::Size index) noexcept
{
    return *g_owners[index];
}

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

[[nodiscard]] constexpr PalVirtualMemoryCapabilities
MakeCapabilities(const Foundation::UInt8 physicalAddressBits, const Foundation::UInt8 virtualAddressMostSignificantBit,
                 const Foundation::UInt8 regionIdentifierBits, const Foundation::UInt8 memoryAttributeMask,
                 const bool hardwareWalkerImplemented) noexcept
{
    return PalVirtualMemoryCapabilities{
        .Translation =
            TranslationCapabilities{
                .InsertablePageSizeMask = ArchitectedMinimumInsertablePageSizeMask,
                .PhysicalAddressBits = physicalAddressBits,
                .ImplementedVirtualAddressMostSignificantBit = virtualAddressMostSignificantBit,
                .RegionIdentifierBits = regionIdentifierBits,
                .ProtectionKeyBits = 24U,
                .MemoryAttributeMask = memoryAttributeMask,
                .HardwareWalkerImplemented = hardwareWalkerImplemented,
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

[[nodiscard]] constexpr PalVirtualMemoryCapabilities PreparationCapabilities() noexcept
{
    return MakeCapabilities(50U, 60U, 24U, 0x71U, true);
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

[[nodiscard]] WritableInterruptVectorTableView TableView(const Foundation::Size index) noexcept
{
    return WritableInterruptVectorTableView{
        .Data = g_interruptVectorTables[index],
        .SizeBytes = InitialInterruptVectorTableSizeBytes,
        .PhysicalStart = ReservedBase + 0x1'0000U,
    };
}

[[nodiscard]] WritableRseBackingStoreView RseView(const Foundation::Size index) noexcept
{
    return WritableRseBackingStoreView{
        .Data = g_rseBackingStores[index],
        .SizeBytes = InitialRseBackingStoreSizeBytes,
        .PhysicalStart = ReservedBase + 0x1'8000U,
    };
}

[[nodiscard]] bool Prepare(InitialAddressSpacePreparation& preparation, const Foundation::Size storageIndex) noexcept
{
    const BootInfo bootInfo = ValidBootInfo();
    return preparation
        .Prepare(ValidImage(), bootInfo, ValidReservedPages(), TestSupport::Ia64FixtureRuntimeMemoryPages(ReservedBase),
                 PreparationCapabilities(), ValidFirmware(), ValidRegions(), TableView(storageIndex),
                 RseView(storageIndex))
        .HasValue();
}

[[nodiscard]] bool InitializeProfilesAndReceipts() noexcept
{
    g_profileInputs[0U] = MakeCapabilities(50U, 60U, 24U, 0x51U, true);
    g_profileInputs[3U] = MakeCapabilities(48U, 57U, 20U, 0x11U, false);
    if (g_profile.Initialize(0x09U, g_profileInputs) != Ia64RuntimeProcessorCapabilitiesError::None ||
        (g_allocator.Initialize(g_profileInputs[3U]) != Ia64RegionIdentifierAllocatorError::None))
    {
        return false;
    }
    for (Foundation::Size index{0U}; index < BackendCount; ++index)
    {
        const auto receipt = g_allocator.Allocate();
        if (!receipt.HasValue())
        {
            return false;
        }
        g_receipts[index] = receipt.GetValue();
    }
    return true;
}

[[nodiscard]] constexpr AddressSpaceMappingRecord
Mapping(const Foundation::UInt64 virtualStart, const Foundation::UInt64 physicalStart,
        const Foundation::UInt64 pages = 1U, const MemoryCachePolicy cachePolicy = MemoryCachePolicy::WriteBack,
        const PagePermissions permissions = PagePermissions{PagePermission::Read},
        const FrameKind kind = FrameKind::Ram) noexcept
{
    return AddressSpaceMappingRecord{
        .MemoryObject = {},
        .DeviceRegion = {},
        .Frame = {},
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .MemoryObjectOffsetPages = 0U,
        .Permissions = permissions,
        .CachePolicy = cachePolicy,
        .Kind = kind,
        .Commitment = {},
        .RuntimeMapping = true,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr AddressSpacePageTableMapResult SentinelOutput() noexcept
{
    return AddressSpacePageTableMapResult{
        .InvalidationStart = VirtualAddress{0x1234'5000U},
        .InvalidationPages = PageCount{0x55U},
        .Reserved = 0xA5A5'A5A5'A5A5'A5A5ULL,
    };
}

[[nodiscard]] constexpr bool Equal(const AddressSpacePageTableMapResult& left,
                                   const AddressSpacePageTableMapResult& right) noexcept
{
    return (left.InvalidationStart.GetValue() == right.InvalidationStart.GetValue()) &&
           (left.InvalidationPages.GetValue() == right.InvalidationPages.GetValue()) &&
           (left.Reserved == right.Reserved);
}

[[nodiscard]] const InitialMappingPlan* PreparedSeed() noexcept
{
    const auto seed = g_preparation.TryGetMappingPlan();
    return seed.HasValue() ? seed.GetValue() : nullptr;
}

[[nodiscard]] bool InitializeAndClaim(Ia64RuntimeAddressSpacePageTable& backend,
                                      const Ia64RegionIdentifierAllocationReceipt& receipt, AddressSpace& owner,
                                      Ia64ActiveRuntimeTranslationBindings& bindings) noexcept
{
    const InitialMappingPlan* const seed = PreparedSeed();
    return (seed != nullptr) &&
           (backend.Initialize(g_profile, receipt, g_preparation, bindings) ==
            Ia64RuntimeAddressSpacePageTableInitializationError::None) &&
           (backend.Claim(owner, *seed) == AddressSpacePageTableOwnershipError::None);
}

void ResetAdapter(const Foundation::UInt64 status) noexcept
{
    g_activationCallCount = 0U;
    g_activationStatus = status;
    for (Foundation::Size index{0U}; index < 7U; ++index)
    {
        g_activationWords[index] = 0U;
    }
}

[[nodiscard]] bool HasBindingError(
    const Foundation::Result<const Ia64RuntimeTranslationLedger*, Ia64ActiveRuntimeTranslationBindingError>& result,
    const Ia64ActiveRuntimeTranslationBindingError error) noexcept
{
    return !result.HasValue() && (result.GetError() == error);
}

[[nodiscard]] bool MappingFailsAtomically(Ia64RuntimeAddressSpacePageTable& backend,
                                          const AddressSpaceMappingRecord& plan,
                                          const AddressSpacePageTableError expected) noexcept
{
    AddressSpacePageTableMapResult output = SentinelOutput();
    const AddressSpacePageTableMapResult before = output;
    return (backend.Map(plan, output) == expected) && Equal(output, before);
}

void TestInitializationAndClaim(TestContext& runner) noexcept
{
    AddressSpacePageTableMapResult output = SentinelOutput();
    const AddressSpacePageTableMapResult before = output;
    ASTERIA_EXPECT(
        runner, "the constinit backend begins unready and preclaim Map preserves output", !g_retryBackend.IsReady(),
        g_retryBackend.Map(Mapping(RegionOneBase, PhysicalBase), output) == AddressSpacePageTableError::NotReady,
        Equal(output, before));

    ASTERIA_EXPECT(runner, "an invalid capability profile is rejected retryably",
                   g_retryBackend.Initialize(g_defaultProfile, g_receipts[0U], g_preparation, g_bindings) ==
                       Ia64RuntimeAddressSpacePageTableInitializationError::InvalidCapabilities);
    ASTERIA_EXPECT(
        runner, "a default RID receipt is rejected retryably",
        g_retryBackend.Initialize(g_profile, Ia64RegionIdentifierAllocationReceipt{}, g_preparation, g_bindings) ==
            Ia64RuntimeAddressSpacePageTableInitializationError::InvalidReceipt);
    ASTERIA_EXPECT(runner, "an unpublished preparation is rejected retryably",
                   g_retryBackend.Initialize(g_profile, g_receipts[0U], g_freshPreparation, g_bindings) ==
                       Ia64RuntimeAddressSpacePageTableInitializationError::InvalidPreparation);
    ASTERIA_EXPECT(runner, "valid common capabilities, receipt, and canonical seed initialize once",
                   g_retryBackend.Initialize(g_profile, g_receipts[0U], g_preparation, g_bindings) ==
                       Ia64RuntimeAddressSpacePageTableInitializationError::None,
                   g_retryBackend.IsReady());
    ASTERIA_EXPECT(runner, "duplicate initialization is rejected without rebuilding private RIDs",
                   g_retryBackend.Initialize(g_profile, g_receipts[0U], g_preparation, g_bindings) ==
                       Ia64RuntimeAddressSpacePageTableInitializationError::AlreadyInitialized);

    const auto equivalentSeed = g_equivalentPreparation.TryGetMappingPlan();
    const InitialMappingPlan* const exactSeed = PreparedSeed();
    ASTERIA_EXPECT(runner, "an equivalent but nonidentical prepared seed fails pointer ownership",
                   equivalentSeed.HasValue(),
                   g_retryBackend.Claim(Owner(0U), *equivalentSeed.GetValue()) ==
                       AddressSpacePageTableOwnershipError::SeedMismatch,
                   g_retryBackend.IsReady());
    ASTERIA_EXPECT(runner, "a different seed fails without consuming the pending exact pointer",
                   g_retryBackend.Claim(Owner(0U), g_differentSeed) ==
                       AddressSpacePageTableOwnershipError::SeedMismatch,
                   g_retryBackend.IsReady());
    ASTERIA_EXPECT(runner, "the exact prepared seed and Region-0 owner claim the backend", exactSeed != nullptr,
                   g_retryBackend.Claim(Owner(0U), *exactSeed) == AddressSpacePageTableOwnershipError::None,
                   !g_retryBackend.IsReady());
    ASTERIA_EXPECT(runner, "a claimed backend distinguishes competing ownership from release authority",
                   g_retryBackend.Claim(Owner(1U), g_differentSeed) ==
                       AddressSpacePageTableOwnershipError::AlreadyOwned,
                   g_retryBackend.Release(Owner(1U)) == AddressSpacePageTableOwnershipError::InvalidOwner);
}

void TestMapping(TestContext& runner) noexcept
{
    AddressSpacePageTableMapResult output = SentinelOutput();
    const AddressSpaceMappingRecord first = Mapping(RegionOneBase, PhysicalBase);
    ASTERIA_EXPECT(runner, "a valid private Region-1 mapping publishes its exact invalidation range",
                   g_retryBackend.Map(first, output) == AddressSpacePageTableError::None,
                   output.InvalidationStart.GetValue() == RegionOneBase, output.InvalidationPages.GetValue() == 1U,
                   output.Reserved == 0U);
    output = SentinelOutput();
    ASTERIA_EXPECT(runner, "an adjacent nonoverlapping mapping succeeds",
                   g_retryBackend.Map(Mapping(RegionOneBase + 0x1000U, PhysicalBase + 0x1000U), output) ==
                       AddressSpacePageTableError::None,
                   output.InvalidationStart.GetValue() == RegionOneBase + 0x1000U);
    ASTERIA_EXPECT(runner, "an overlapping virtual range reports MappingConflict atomically",
                   MappingFailsAtomically(g_retryBackend, first, AddressSpacePageTableError::MappingConflict));

    AddressSpaceMappingRecord malformed = Mapping(RegionOneBase + 0x20'0000U, PhysicalBase + 0x20'0000U);
    malformed.Reserved[2U] = 1U;
    ASTERIA_EXPECT(runner, "malformed, Region-0, and cross-region mappings preserve output",
                   MappingFailsAtomically(g_retryBackend, malformed, AddressSpacePageTableError::InvalidPlan),
                   MappingFailsAtomically(g_retryBackend, Mapping(0x0010'0000U, PhysicalBase),
                                          AddressSpacePageTableError::InvalidPlan),
                   MappingFailsAtomically(g_retryBackend, Mapping(0x3FFF'FFFF'FFFF'F000ULL, PhysicalBase, 2U),
                                          AddressSpacePageTableError::InvalidPlan));
    ASTERIA_EXPECT(runner, "oversize and unsupported-permission plans are InvalidPlan atomically",
                   MappingFailsAtomically(g_retryBackend, Mapping(RegionOneBase + 0x30'0000U, PhysicalBase, 513U),
                                          AddressSpacePageTableError::InvalidPlan),
                   MappingFailsAtomically(g_retryBackend,
                                          Mapping(RegionOneBase + 0x40'0000U, PhysicalBase, 1U,
                                                  MemoryCachePolicy::WriteBack, PagePermissions{}),
                                          AddressSpacePageTableError::InvalidPlan));
    ASTERIA_EXPECT(
        runner, "unsupported cache policy and common memory attributes preserve output",
        MappingFailsAtomically(g_retryBackend,
                               Mapping(RegionOneBase + 0x50'0000U, PhysicalBase, 1U, MemoryCachePolicy::WriteThrough),
                               AddressSpacePageTableError::UnsupportedConfiguration),
        MappingFailsAtomically(g_retryBackend,
                               Mapping(RegionOneBase + 0x60'0000U, PhysicalBase, 1U, MemoryCachePolicy::WriteCombining),
                               AddressSpacePageTableError::UnsupportedConfiguration));
    ASTERIA_EXPECT(runner, "the intersected physical width rejects a mapping accepted by CPU0 alone",
                   MappingFailsAtomically(g_retryBackend,
                                          Mapping(RegionOneBase + 0x70'0000U, Foundation::UInt64{1U} << 48U),
                                          AddressSpacePageTableError::InvalidPlan));

    output = SentinelOutput();
    ASTERIA_EXPECT(runner, "the RR7 top page succeeds while a wrapping two-page range is rejected",
                   g_retryBackend.Map(Mapping(0xFFFF'FFFF'FFFF'F000ULL, PhysicalBase + 0x80'0000U), output) ==
                       AddressSpacePageTableError::None,
                   output.InvalidationStart.GetValue() == 0xFFFF'FFFF'FFFF'F000ULL,
                   MappingFailsAtomically(g_retryBackend,
                                          Mapping(0xFFFF'FFFF'FFFF'F000ULL, PhysicalBase + 0x90'0000U, 2U),
                                          AddressSpacePageTableError::InvalidPlan));
}

[[nodiscard]] bool UnmapFailsAtomically(Ia64RuntimeAddressSpacePageTable& backend,
                                        const AddressSpaceMappingRecord& plan,
                                        const AddressSpacePageTableError expected) noexcept
{
    AddressSpacePageTableMapResult output = SentinelOutput();
    const AddressSpacePageTableMapResult before = output;
    return (backend.Unmap(plan, output) == expected) && Equal(output, before);
}

void TestUnmapping(TestContext& runner) noexcept
{
    static constinit Ia64RuntimeAddressSpacePageTable unclaimed;
    ASTERIA_EXPECT(
        runner, "an unclaimed backend unmaps nothing",
        UnmapFailsAtomically(unclaimed, Mapping(RegionOneBase, PhysicalBase), AddressSpacePageTableError::NotReady));

    const AddressSpaceMappingRecord first = Mapping(RegionOneBase, PhysicalBase);
    AddressSpacePageTableMapResult output = SentinelOutput();
    ASTERIA_EXPECT(runner, "unmapping tombstones the record and returns its exact invalidation range",
                   g_retryBackend.Unmap(first, output) == AddressSpacePageTableError::None,
                   output.InvalidationStart.GetValue() == RegionOneBase, output.InvalidationPages.GetValue() == 1U,
                   output.Reserved == 0U);
    output = SentinelOutput();
    ASTERIA_EXPECT(runner, "the unmapped range maps again",
                   g_retryBackend.Map(first, output) == AddressSpacePageTableError::None,
                   output.InvalidationStart.GetValue() == RegionOneBase);
}

void TestCapacity(TestContext& runner) noexcept
{
    bool exact = InitializeAndClaim(g_capacityBackend, g_receipts[1U], Owner(2U), g_zeroBindings);
    for (Foundation::UInt64 index{0U}; index < Kernel::Memory::MaximumRuntimeAddressSpaceMappings; ++index)
    {
        AddressSpacePageTableMapResult output = SentinelOutput();
        const Foundation::UInt64 virtualStart = RegionTwoBase + (index << 12U);
        exact = (g_capacityBackend.Map(Mapping(virtualStart, PhysicalBase + (index << 12U)), output) ==
                 AddressSpacePageTableError::None) &&
                (output.InvalidationStart.GetValue() == virtualStart) && exact;
    }
    ASTERIA_EXPECT(
        runner, "256 runtime mappings publish and the 257th fails atomically at capacity", exact,
        MappingFailsAtomically(g_capacityBackend,
                               Mapping(RegionTwoBase + (Kernel::Memory::MaximumRuntimeAddressSpaceMappings << 12U),
                                       PhysicalBase + 0x0100'0000U),
                               AddressSpacePageTableError::CapacityExceeded));
    AddressSpacePageTableMapResult output = SentinelOutput();
    ASTERIA_EXPECT(
        runner, "an acknowledged unmap permits ledger slot reuse",
        g_capacityBackend.Unmap(Mapping(RegionTwoBase, PhysicalBase), output) == AddressSpacePageTableError::None,
        output.InvalidationStart.GetValue() == RegionTwoBase,
        g_capacityBackend.Map(Mapping(RegionTwoBase, PhysicalBase), output) == AddressSpacePageTableError::None);
    ASTERIA_EXPECT(
        runner, "unmapping a record the ledger does not hold faults the backend",
        UnmapFailsAtomically(g_capacityBackend, Mapping(RegionTwoBase, PhysicalBase + 0x1000U),
                             AddressSpacePageTableError::CorruptState),
        UnmapFailsAtomically(g_capacityBackend, Mapping(RegionTwoBase + 0x1000U, PhysicalBase + 0x1000U),
                             AddressSpacePageTableError::CorruptState),
        MappingFailsAtomically(
            g_capacityBackend,
            Mapping(RegionTwoBase + (Kernel::Memory::MaximumRuntimeAddressSpaceMappings << 12U), PhysicalBase),
            AddressSpacePageTableError::CorruptState));
    ASTERIA_EXPECT(runner, "an unbound claimed ledger retires permanently after authoritative quiescence",
                   g_capacityBackend.Release(Owner(2U)) == AddressSpacePageTableOwnershipError::None);
}

[[nodiscard]] bool ForwardedReceiptWords(const Ia64RegionIdentifierAllocationReceipt& receipt) noexcept
{
    for (Foundation::Size index{1U}; index < 8U; ++index)
    {
        const auto regionIdentifier = receipt.GetRegionIdentifier(index);
        if (!regionIdentifier.HasValue() ||
            (g_activationWords[index - 1U] !=
             ((static_cast<Foundation::UInt64>(regionIdentifier.GetValue()) << 8U) | 0x30U)))
        {
            return false;
        }
    }
    return true;
}

void TestActivationAndRelease(TestContext& runner) noexcept
{
    const bool initialized = InitializeAndClaim(g_activationBackend, g_receipts[2U], Owner(3U), g_bindings);
    AddressSpacePageTableMapResult output = SentinelOutput();
    const AddressSpaceMappingRecord mapping = Mapping(RegionOneBase + 0x1000'0000U, PhysicalBase + 0x1000'0000U);
    const bool mapped = initialized && (g_activationBackend.Map(mapping, output) == AddressSpacePageTableError::None);

    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "wrong owner, invalid CPU, and non-profile CPU cannot reach native activation",
                   g_activationBackend.Activate(Owner(1U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::InvalidOwner,
                   g_activationBackend.Activate(Owner(3U), CpuId{4U}) ==
                       AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   g_activationBackend.Activate(Owner(3U), CpuId{1U}) ==
                       AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_activationCallCount == 0U);
    ASTERIA_EXPECT(runner, "a production-zero CPU binding rejects activation without admitting it",
                   g_activationBackend.Activate(Owner(3U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_activationCallCount == 0U);

    ConfigureInstalledBindingFixture(g_bindings, CpuId{0U}, g_retryBackend, g_capacityBackend, g_activationBackend,
                                     g_precommitBackend, g_uncertaintyBackend, g_recoveryBackend, g_concurrentBackend);
    ResetAdapter(AdapterSuccess);
    const auto activation = g_activationBackend.Activate(Owner(3U), CpuId{0U});
    const auto active = g_bindings.AcquireActiveLedger(CpuId{0U});
    const auto lookup =
        active.HasValue()
            ? active.GetValue()->Lookup(mapping.VirtualStart.GetValue() + 0x42U)
            : Foundation::Result<Platform::Ia64::Memory::EncodedIa64RuntimeTranslation,
                                 Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerFailure>::Failure({});
    const auto seedLookup =
        active.HasValue()
            ? active.GetValue()->Lookup(ImageBase)
            : Foundation::Result<Platform::Ia64::Memory::EncodedIa64RuntimeTranslation,
                                 Platform::Ia64::Memory::Ia64RuntimeTranslationLedgerFailure>::Failure({});
    ASTERIA_EXPECT(runner, "activation forwards private RRs and publishes only runtime ledger mappings", mapped,
                   activation == AddressSpacePageTableActivationError::None, g_activationCallCount == 1U,
                   ForwardedReceiptWords(g_receipts[2U]), active.HasValue(), lookup.HasValue(),
                   lookup.GetValue().FaultingAddress == mapping.VirtualStart.GetValue(), !seedLookup.HasValue(),
                   seedLookup.GetError().Code == Ia64RuntimeTranslationLedgerError::MappingNotFound);
    ASTERIA_EXPECT(runner, "Release refuses a ledger that remains observably bound after quiescence",
                   g_activationBackend.Release(Owner(3U)) == AddressSpacePageTableOwnershipError::NotReady);

    const bool precommitReady = InitializeAndClaim(g_precommitBackend, g_receipts[3U], Owner(4U), g_bindings);
    ResetAdapter(AdapterRejectedBeforeCommit);
    const auto precommit = g_precommitBackend.Activate(Owner(4U), CpuId{0U});
    const auto preserved = g_bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "native precommit rejection preserves Claimed state and the prior binding", precommitReady,
                   precommit == AddressSpacePageTableActivationError::RejectedBeforeCommit, g_activationCallCount == 1U,
                   active.HasValue(), preserved.HasValue(), preserved.GetValue() == active.GetValue());
    ASTERIA_EXPECT(runner, "the never-current precommit-rejected backend retires under modeled quiescence",
                   g_precommitBackend.Release(Owner(4U)) == AddressSpacePageTableOwnershipError::None);

    ASTERIA_EXPECT(runner, "precommit rejection leaves the initial native-current root retained", active.HasValue(),
                   g_bindings.IsBound(*active.GetValue()),
                   g_activationBackend.Release(Owner(3U)) == AddressSpacePageTableOwnershipError::NotReady);

    const bool uncertaintyReady = InitializeAndClaim(g_uncertaintyBackend, g_receipts[4U], Owner(5U), g_bindings);
    ResetAdapter(AdapterCommittedStateUnknown);
    const auto uncertain = g_uncertaintyBackend.Activate(Owner(5U), CpuId{0U});
    const auto afterUncertainty = g_bindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "uncertainty removes publication while both possible current roots stay retained",
                   uncertaintyReady, uncertain == AddressSpacePageTableActivationError::CommittedStateUnknown,
                   g_activationCallCount == 1U, !afterUncertainty.HasValue(),
                   afterUncertainty.GetError() == Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger,
                   g_uncertaintyBackend.Map(mapping, output) == AddressSpacePageTableError::CorruptState);

    const bool recoveryReady = InitializeAndClaim(g_recoveryBackend, g_receipts[6U], Owner(7U), g_bindings);
    ResetAdapter(AdapterSuccess);
    const auto switchAway = g_recoveryBackend.Activate(Owner(7U), CpuId{0U});
    const auto recoveryLedger = g_bindings.AcquireActiveLedger(CpuId{0U});
    // Recovery is now native-current. Its ledger, namespace-scope backend, and
    // namespace-scope owner storage remain retained for the hosted process.
    ASTERIA_EXPECT(runner, "recovery switches away prior roots while its current ledger stays retained", recoveryReady,
                   switchAway == AddressSpacePageTableActivationError::None, g_activationCallCount == 1U,
                   recoveryLedger.HasValue(), g_bindings.IsBound(*recoveryLedger.GetValue()),
                   g_activationBackend.Release(Owner(3U)) == AddressSpacePageTableOwnershipError::None,
                   g_uncertaintyBackend.Release(Owner(5U)) == AddressSpacePageTableOwnershipError::None,
                   g_activationBackend.Map(mapping, output) == AddressSpacePageTableError::NotReady,
                   g_recoveryBackend.Release(Owner(7U)) == AddressSpacePageTableOwnershipError::NotReady);

    const bool zeroReady = InitializeAndClaim(g_zeroBindingsBackend, g_receipts[5U], Owner(6U), g_zeroBindings);
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "a separate zero binding table stays non-runnable and makes no native call", zeroReady,
                   g_zeroBindingsBackend.Activate(Owner(6U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_activationCallCount == 0U,
                   g_zeroBindingsBackend.Release(Owner(6U)) == AddressSpacePageTableOwnershipError::None);
}

void TestPhysicalBootstrapActivation(TestContext& runner) noexcept
{
    const InitialMappingPlan* const seed = PreparedSeed();

    ResetAdapter(AdapterSuccess);
    const auto uninitialized = g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U});
    const bool initialized = (seed != nullptr) && (g_physicalBackend.Initialize(g_profile, g_receipts[9U],
                                                                                g_preparation, g_physicalBindings) ==
                                                   Ia64RuntimeAddressSpacePageTableInitializationError::None);
    const auto readyOnly = g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U});
    ASTERIA_EXPECT(runner, "physical bootstrap requires an already-claimed runtime root",
                   uninitialized == AddressSpacePageTableActivationError::NotReady, initialized,
                   g_physicalBackend.IsReady(), readyOnly == AddressSpacePageTableActivationError::NotReady,
                   g_activationCallCount == 0U);

    const bool claimed =
        (seed != nullptr) &&
        (g_physicalBackend.Claim(Owner(11U), *seed) == AddressSpacePageTableOwnershipError::None) &&
        InitializeAndClaim(g_physicalMismatchBackend, g_receipts[10U], Owner(12U), g_physicalBindings) &&
        InitializeAndClaim(g_physicalUncertaintyBackend, g_receipts[11U], Owner(13U), g_physicalBindings);
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "physical bootstrap rejects wrong ownership, CPU state, and an unadmitted slot", claimed,
                   g_physicalBackend.ActivatePhysicalBootstrap(Owner(10U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::InvalidOwner,
                   g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{4U}) ==
                       AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{1U}) ==
                       AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_activationCallCount == 0U);

    ConfigureInstalledBindingFixture(g_physicalBindings, CpuId{0U}, g_physicalBindings);
    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "physical bootstrap rejects an operand corridor excluding its backend",
                   g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_activationCallCount == 0U,
                   HasBindingError(g_physicalBindings.AcquireActiveLedger(CpuId{0U}),
                                   Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ConfigureInstalledBindingFixture(g_physicalBindings, CpuId{0U}, g_physicalBackend, g_physicalMismatchBackend,
                                     g_physicalUncertaintyBackend);
    ResetAdapter(AdapterRejectedBeforeCommit);
    const auto rejected = g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U});
    ASTERIA_EXPECT(runner, "physical native precommit rejection preserves Claimed and admitted-empty state",
                   rejected == AddressSpacePageTableActivationError::RejectedBeforeCommit, g_activationCallCount == 1U,
                   !g_physicalBackend.IsActiveLedgerBound(CpuId{0U}),
                   HasBindingError(g_physicalBindings.AcquireActiveLedger(CpuId{0U}),
                                   Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ResetAdapter(AdapterSuccess);
    const auto activation = g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U});
    const auto active = g_physicalBindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "physical bootstrap publishes the backend's exact ledger after native success",
                   activation == AddressSpacePageTableActivationError::None, g_activationCallCount == 1U,
                   g_physicalBackend.IsActiveLedgerBound(CpuId{0U}), active.HasValue());

    ResetAdapter(AdapterSuccess);
    const auto repeatedBeforeAdmission = g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U});
    ASTERIA_EXPECT(runner, "physical bootstrap is one-shot while controller acknowledgement is pending",
                   repeatedBeforeAdmission == AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   g_activationCallCount == 0U, g_physicalBackend.IsActiveLedgerBound(CpuId{0U}));

    ResetAdapter(AdapterSuccess);
    const auto rejectedAdmission = g_physicalBackend.Activate(Owner(10U), CpuId{0U});
    const auto acknowledged = g_physicalBackend.Activate(Owner(11U), CpuId{0U});
    ASTERIA_EXPECT(runner, "exact pending controller acknowledgement retries and makes no second RR call",
                   rejectedAdmission == AddressSpacePageTableActivationError::InvalidOwner,
                   acknowledged == AddressSpacePageTableActivationError::None, g_activationCallCount == 0U,
                   g_physicalBackend.IsActiveLedgerBound(CpuId{0U}));

    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "physical bootstrap remains one-shot after controller acknowledgement",
                   g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   g_activationCallCount == 0U, g_physicalBackend.IsActiveLedgerBound(CpuId{0U}));

    const auto current = g_physicalBindings.AcquireActiveLedger(CpuId{0U});
    const bool cleared = current.HasValue() && (g_physicalBindings.ClearActiveLedger(CpuId{0U}, *current.GetValue()) ==
                                                Ia64ActiveRuntimeTranslationBindingError::None);
    ResetAdapter(AdapterSuccess);
    const auto repeatedAfterClear = g_physicalBackend.ActivatePhysicalBootstrap(Owner(11U), CpuId{0U});
    ASTERIA_EXPECT(runner, "the completed bit rejects physical replay even after an external clear", cleared,
                   repeatedAfterClear == AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   g_activationCallCount == 0U,
                   HasBindingError(g_physicalBindings.AcquireActiveLedger(CpuId{0U}),
                                   Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger));

    ResetAdapter(AdapterSuccess);
    const auto ordinaryAfterAcknowledgement = g_physicalBackend.Activate(Owner(11U), CpuId{0U});
    const auto ordinaryLedger = g_physicalBindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "consuming the pending bit restores the ordinary native activation path",
                   ordinaryAfterAcknowledgement == AddressSpacePageTableActivationError::None,
                   g_activationCallCount == 1U, ordinaryLedger.HasValue());

    const bool clearedOrdinary =
        ordinaryLedger.HasValue() && (g_physicalBindings.ClearActiveLedger(CpuId{0U}, *ordinaryLedger.GetValue()) ==
                                      Ia64ActiveRuntimeTranslationBindingError::None);
    ResetAdapter(AdapterSuccess);
    const auto mismatchPhysical = g_physicalMismatchBackend.ActivatePhysicalBootstrap(Owner(12U), CpuId{0U});
    const auto mismatchLedger = g_physicalBindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "a second backend establishes an exact pending physical ledger fixture", clearedOrdinary,
                   mismatchPhysical == AddressSpacePageTableActivationError::None, g_activationCallCount == 1U,
                   mismatchLedger.HasValue());

    ResetAdapter(AdapterSuccess);
    const auto foreignActivation = g_physicalBackend.Activate(Owner(11U), CpuId{0U});
    const auto foreignLedger = g_physicalBindings.AcquireActiveLedger(CpuId{0U});
    const bool differentLedgerInstalled = (foreignActivation == AddressSpacePageTableActivationError::None) &&
                                          (g_activationCallCount == 1U) && mismatchLedger.HasValue() &&
                                          foreignLedger.HasValue() &&
                                          (foreignLedger.GetValue() != mismatchLedger.GetValue());
    ResetAdapter(AdapterSuccess);
    const auto mismatchAcknowledgement = g_physicalMismatchBackend.Activate(Owner(12U), CpuId{0U});
    const auto retainedForeignLedger = g_physicalBindings.AcquireActiveLedger(CpuId{0U});
    AddressSpacePageTableMapResult mismatchOutput = SentinelOutput();
    ASTERIA_EXPECT(runner, "a different pending ledger faults conservatively without native replacement",
                   differentLedgerInstalled,
                   mismatchAcknowledgement == AddressSpacePageTableActivationError::CommittedStateUnknown,
                   g_activationCallCount == 0U, retainedForeignLedger.HasValue(),
                   retainedForeignLedger.GetValue() == foreignLedger.GetValue(),
                   g_physicalMismatchBackend.Map(Mapping(RegionTwoBase + 0x3000'0000U, PhysicalBase + 0x3000'0000U),
                                                 mismatchOutput) == AddressSpacePageTableError::CorruptState);

    const bool clearedForeign = retainedForeignLedger.HasValue() &&
                                (g_physicalBindings.ClearActiveLedger(CpuId{0U}, *retainedForeignLedger.GetValue()) ==
                                 Ia64ActiveRuntimeTranslationBindingError::None);
    ResetAdapter(AdapterCommittedStateUnknown);
    const auto uncertain = g_physicalUncertaintyBackend.ActivatePhysicalBootstrap(Owner(13U), CpuId{0U});
    AddressSpacePageTableMapResult uncertaintyOutput = SentinelOutput();
    ASTERIA_EXPECT(runner, "physical committed uncertainty faults the backend and publishes no ledger", clearedForeign,
                   uncertain == AddressSpacePageTableActivationError::CommittedStateUnknown,
                   g_activationCallCount == 1U,
                   HasBindingError(g_physicalBindings.AcquireActiveLedger(CpuId{0U}),
                                   Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger),
                   g_physicalUncertaintyBackend.Map(Mapping(RegionTwoBase + 0x4000'0000U, PhysicalBase + 0x4000'0000U),
                                                    uncertaintyOutput) == AddressSpacePageTableError::CorruptState);

    ResetAdapter(AdapterSuccess);
    ASTERIA_EXPECT(runner, "a faulted physical-bootstrap backend cannot retry native activation",
                   g_physicalUncertaintyBackend.ActivatePhysicalBootstrap(Owner(13U), CpuId{0U}) ==
                       AddressSpacePageTableActivationError::NotReady,
                   g_activationCallCount == 0U);
}

void TestOutsideCorridorActivation(TestContext& runner) noexcept
{
    const bool initialized =
        InitializeAndClaim(g_outsideCorridorBackend, g_receipts[8U], Owner(10U), g_outsideCorridorBindings);
    ConfigureInstalledBindingFixture(g_outsideCorridorBindings, CpuId{0U}, g_outsideCorridorBindings);

    ResetAdapter(AdapterSuccess);
    const AddressSpacePageTableActivationError outsideCorridor =
        g_outsideCorridorBackend.Activate(Owner(10U), CpuId{0U});
    const auto unbound = g_outsideCorridorBindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(runner, "a corridor excluding the backend rejects before native activation or ledger publication",
                   initialized, outsideCorridor == AddressSpacePageTableActivationError::UnsupportedConfiguration,
                   g_activationCallCount == 0U, !unbound.HasValue(),
                   unbound.GetError() == Ia64ActiveRuntimeTranslationBindingError::NoActiveLedger);

    ConfigureInstalledBindingFixture(g_outsideCorridorBindings, CpuId{0U}, g_outsideCorridorBackend);
    ResetAdapter(AdapterRejectedBeforeCommit);
    const AddressSpacePageTableActivationError laterPrecommit =
        g_outsideCorridorBackend.Activate(Owner(10U), CpuId{0U});
    AddressSpacePageTableMapResult output = SentinelOutput();
    ASTERIA_EXPECT(runner, "outside-corridor rejection preserves Claimed state for a later valid precommit path",
                   laterPrecommit == AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   g_activationCallCount == 1U,
                   g_outsideCorridorBackend.Map(Mapping(RegionTwoBase + 0x2000'0000U, PhysicalBase + 0x2000'0000U),
                                                output) == AddressSpacePageTableError::None,
                   g_outsideCorridorBackend.Release(Owner(10U)) == AddressSpacePageTableOwnershipError::None);
}

[[nodiscard]] Foundation::UInt64 Purges(const Platform::Ia64::Memory::Ia64LocalTranslationCachePurge& purge) noexcept
{
    return purge.GetSnapshot().Purges;
}

void TestTranslationCachePurge(TestContext& runner) noexcept
{
    // The hosted purge leaf is empty; the purge counts its runs.
    Platform::Ia64::Memory::Ia64LocalTranslationCachePurge purge;
    const bool ready =
        purge.Configure(CpuId{0U}, Platform::Ia64::Memory::PalTranslationCachePurgePlan{.BaseAddress = 0U,
                                                                                        .OperationCount = 1U,
                                                                                        .LastPurgeAddress = 0U,
                                                                                        .OuterCount = 1U,
                                                                                        .InnerCount = 1U,
                                                                                        .OuterStride = 0U,
                                                                                        .InnerStride = 0U}) &&
        InitializeAndClaim(g_purgedBackend, g_receipts[12U], Owner(14U), g_purgeBindings) &&
        InitializeAndClaim(g_switchBackend, g_receipts[13U], Owner(15U), g_purgeBindings) &&
        InitializeAndClaim(g_unpurgedBackend, g_receipts[14U], Owner(16U), g_purgeBindings) &&
        g_purgedBackend.ConfigureTranslationCachePurge(purge);
    ConfigureInstalledBindingFixture(g_purgeBindings, CpuId{0U}, g_purgedBackend, g_switchBackend, g_unpurgedBackend);
    const AddressSpaceMappingRecord first = Mapping(RegionOneBase + 0x2000'0000U, PhysicalBase + 0x2000'0000U);
    const AddressSpaceMappingRecord second = Mapping(RegionOneBase + 0x2000'1000U, PhysicalBase + 0x2000'1000U);
    AddressSpacePageTableMapResult output = SentinelOutput();

    ResetAdapter(AdapterSuccess);
    const bool firstRun =
        (g_purgedBackend.Activate(Owner(14U), CpuId{0U}) == AddressSpacePageTableActivationError::None) &&
        (g_purgedBackend.Map(first, output) == AddressSpacePageTableError::None) &&
        (g_purgedBackend.Map(second, output) == AddressSpacePageTableError::None) &&
        (g_switchBackend.Activate(Owner(15U), CpuId{0U}) == AddressSpacePageTableActivationError::None);
    ASTERIA_EXPECT(runner, "a space's first activations and its maps need no purge, and the purge is configured once",
                   ready, firstRun, Purges(purge) == 0U, !g_purgedBackend.ConfigureTranslationCachePurge(purge),
                   g_purgedBackend.GetState() ==
                       Platform::Ia64::Memory::Ia64RuntimeAddressSpacePageTableState::Claimed);

    // Unmapped while another space is current: the processor may still cache
    // the translation under this space's RIDs. A native rejection binds
    // Nothing, so it purges nothing either.
    const bool unmapped = g_purgedBackend.Unmap(first, output) == AddressSpacePageTableError::None;
    ResetAdapter(AdapterRejectedBeforeCommit);
    const auto rejected = g_purgedBackend.Activate(Owner(14U), CpuId{0U});
    const Foundation::UInt64 afterRejection = Purges(purge);
    ResetAdapter(AdapterSuccess);
    const auto purgedActivation = g_purgedBackend.Activate(Owner(14U), CpuId{0U});
    const Foundation::UInt64 afterUnmap = Purges(purge);
    const bool switched = g_switchBackend.Activate(Owner(15U), CpuId{0U}) == AddressSpacePageTableActivationError::None;
    const bool again = g_purgedBackend.Activate(Owner(14U), CpuId{0U}) == AddressSpacePageTableActivationError::None;
    ASTERIA_EXPECT(runner, "the next bound activation after an unmap purges once, and later ones do not", unmapped,
                   rejected == AddressSpacePageTableActivationError::RejectedBeforeCommit, afterRejection == 0U,
                   purgedActivation == AddressSpacePageTableActivationError::None, afterUnmap == 1U, switched, again,
                   Purges(purge) == 1U);

    // An unmap while current also advances the epoch: one redundant purge.
    const bool currentUnmap = g_purgedBackend.Unmap(second, output) == AddressSpacePageTableError::None;
    const bool redundant =
        (g_switchBackend.Activate(Owner(15U), CpuId{0U}) == AddressSpacePageTableActivationError::None) &&
        (g_purgedBackend.Activate(Owner(14U), CpuId{0U}) == AddressSpacePageTableActivationError::None);
    ASTERIA_EXPECT(runner, "an unmap while the space was current costs one redundant purge", currentUnmap, redundant,
                   Purges(purge) == 2U);

    // A space with no purge that unmapped after it ran cannot come back.
    const bool unpurgedRan =
        (g_unpurgedBackend.Activate(Owner(16U), CpuId{0U}) == AddressSpacePageTableActivationError::None) &&
        (g_unpurgedBackend.Map(first, output) == AddressSpacePageTableError::None) &&
        (g_switchBackend.Activate(Owner(15U), CpuId{0U}) == AddressSpacePageTableActivationError::None) &&
        (g_unpurgedBackend.Unmap(first, output) == AddressSpacePageTableError::None);
    ResetAdapter(AdapterSuccess);
    const auto refused = g_unpurgedBackend.Activate(Owner(16U), CpuId{0U});
    const auto stillCurrent = g_purgeBindings.AcquireActiveLedger(CpuId{0U});
    ASTERIA_EXPECT(
        runner, "with no purge for the processor the activation is refused before native work and the binding stays",
        unpurgedRan, refused == AddressSpacePageTableActivationError::UnsupportedConfiguration,
        g_activationCallCount == 0U, stillCurrent.HasValue(), g_switchBackend.IsActiveLedgerBound(CpuId{0U}),
        g_unpurgedBackend.GetState() == Platform::Ia64::Memory::Ia64RuntimeAddressSpacePageTableState::Claimed);

    ASTERIA_EXPECT(runner, "a released backend reports Retired",
                   g_unpurgedBackend.Release(Owner(16U)) == AddressSpacePageTableOwnershipError::None,
                   g_unpurgedBackend.GetState() ==
                       Platform::Ia64::Memory::Ia64RuntimeAddressSpacePageTableState::Retired,
                   !g_unpurgedBackend.ConfigureTranslationCachePurge(purge));
}

/// An AddressSpacePool over the IA-64 slots, with a root ResourceDomain whose
/// Manage capability authorizes each creation.
class SlotPoolFixture final
{
public:
    SlotPoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Pool{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const Kernel::Resource::ResourceDomainLimits limits{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = 1U << 22U,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .ChildDomains = 0U,
        };
        if (Domain.InitializeRoot(Kernel::Object::ResourceDomainId{5U}, limits).Error !=
            Kernel::Resource::ResourceDomainError::None)
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        const auto root = Revocations.CreateRoot();
        if (!registered.HasValue() || !root.HasValue())
        {
            return false;
        }
        const auto authority =
            Issuer.Mint(Space, registered.GetValue(),
                        Kernel::Object::CapabilityRights{Kernel::Object::CapabilityRight::Manage}.With(
                            Kernel::Object::CapabilityRight::Inspect),
                        root.GetValue(), Kernel::Object::MonotonicTime::Infinite(), 0U);
        Authority = authority.HasValue() ? authority.GetValue() : Kernel::Object::CapabilityHandle{};
        return Authority.IsValid();
    }

    [[nodiscard]] Foundation::Result<Kernel::Memory::AddressSpaceCreation, Kernel::Object::ObjectPoolFailure>
    Create() noexcept
    {
        return Pool.Create(Kernel::Memory::AddressSpaceCreateRequest{.Space = &Space, .Authority = Authority});
    }

    Platform::Synchronization::SpinLock Synchronization;
    TestSupport::ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    Kernel::Object::RevocationRegistry Revocations;
    Kernel::Object::RootCapabilityIssuer Issuer;
    Kernel::Object::CapabilitySpace Space;
    Kernel::Resource::ResourceDomainLedger Ledger;
    Kernel::Resource::ResourceDomain Domain;
    Kernel::Memory::AddressSpacePool Pool;
    Kernel::Object::CapabilityHandle Authority{};
};

// The IA-64 run-time AddressSpaces through the kernel's pool: each creation takes a fresh RID
// block, the pool registers what the slot returns, and a destroyed space's slot is released for the
// next creation.
void TestRuntimeAddressSpaceSlots(TestContext& runner) noexcept
{
    using Platform::Ia64::Memory::Ia64RuntimeAddressSpaceSlotState;
    using Platform::Ia64::Memory::Ia64RuntimeAddressSpaceSlots;
    static SlotPoolFixture fixture;
    static Platform::Ia64::Memory::Ia64LocalTranslationCachePurge purge;
    const bool purgeReady =
        purge.Configure(CpuId{0U}, Platform::Ia64::Memory::PalTranslationCachePurgePlan{.BaseAddress = 0U,
                                                                                        .OperationCount = 1U,
                                                                                        .LastPurgeAddress = 0U,
                                                                                        .OuterCount = 1U,
                                                                                        .InnerCount = 1U,
                                                                                        .OuterStride = 0U,
                                                                                        .InnerStride = 0U});
    ConfigureInstalledBindingFixture(g_slotBindings, CpuId{0U}, g_slots);
    const Kernel::Object::ResourceDomainId charge{5U};
    const bool unconfigured = (g_slots.GetSlotCount() == 0U) && (g_slots.InitializeSlot(0U, charge) == nullptr);
    const bool unsealed = !g_slots.Configure(g_profile, g_allocator, g_freshPreparation, g_slotBindings, purge);
    const bool configured = g_slots.Configure(g_profile, g_allocator, g_preparation, g_slotBindings, purge);
    const bool once = !g_slots.Configure(g_profile, g_allocator, g_preparation, g_slotBindings, purge);
    ASTERIA_EXPECT(runner, "the slots are configured once, from the canonical seed, and serve nothing before",
                   purgeReady, unconfigured, unsealed, configured, once,
                   g_slots.GetSlotCount() == Platform::Ia64::Memory::Ia64RuntimeAddressSpaceSlotCount,
                   g_slots.GetSlotBytes() == Ia64RuntimeAddressSpaceSlots::SlotBytes,
                   g_slots.GetSlotBytes(true) == Ia64RuntimeAddressSpaceSlots::SlotBytes,
                   g_slots.InitializeSlot(Platform::Ia64::Memory::Ia64RuntimeAddressSpaceSlotCount, charge) == nullptr);

    // Every slot is held before the pool refuses a creation.
    constexpr Foundation::Size slots{Platform::Ia64::Memory::Ia64RuntimeAddressSpaceSlotCount};
    const auto before = g_allocator.GetSnapshot();
    const bool ready = fixture.Initialize() && fixture.Pool.Configure(g_slots);
    const auto first =
        fixture.Pool.Create({.Space = &fixture.Space, .Authority = fixture.Authority, .LargePageTableArena = true});
    const auto afterFirst = g_allocator.GetSnapshot();
    Foundation::Size created{first.HasValue() ? 1U : 0U};
    for (Foundation::Size slot{1U}; slot < slots; ++slot)
    {
        created += fixture.Create().HasValue() ? 1U : 0U;
    }
    const auto full = fixture.Create();
    const auto afterFull = g_allocator.GetSnapshot();
    const auto held = g_slots.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "each creation takes one fresh RID block and a full pool takes none", ready, created == slots,
        !full.HasValue(), afterFirst.AllocatedAddressSpaceCount == (before.AllocatedAddressSpaceCount + 1U),
        afterFirst.NextRegionIdentifier == (before.NextRegionIdentifier + 8U),
        afterFull.AllocatedAddressSpaceCount == (before.AllocatedAddressSpaceCount + slots),
        afterFull.NextRegionIdentifier == (before.NextRegionIdentifier + (8U * slots)), held.FreeSlots == 0U,
        held.Prepared == slots, held.Spent == 0U, g_slots.GetSlotState(0U) == Ia64RuntimeAddressSpaceSlotState::Held,
        g_slots.GetSlotState(slots - 1U) == Ia64RuntimeAddressSpaceSlotState::Held,
        fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == (slots * Ia64RuntimeAddressSpaceSlots::SlotBytes),
        !g_slots.ReleaseSlot(0U));

    const Kernel::Object::CapabilityHandle handle =
        first.HasValue() ? first.GetValue().Capability : Kernel::Object::CapabilityHandle{};
    const Kernel::Object::ObjectId identity =
        first.HasValue() ? first.GetValue().Space.GetObjectId() : Kernel::Object::ObjectId{};
    const auto destroyed = fixture.Pool.Destroy(fixture.Space, handle);
    const auto collected = fixture.Pool.Collect();
    const auto released = g_slots.GetSnapshot();
    const auto again = fixture.Create();
    const auto afterAgain = g_allocator.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "a destroyed space's slot is released and the next creation builds it again with new RIDs",
        destroyed.Error == Kernel::Object::ObjectPoolError::None, collected.HasValue() && (collected.GetValue() == 1U),
        released.Released == 1U, released.FreeSlots == 1U, again.HasValue(),
        again.HasValue() && (again.GetValue().Space.GetObjectId().GetValue() != identity.GetValue()),
        afterAgain.AllocatedAddressSpaceCount == (before.AllocatedAddressSpaceCount + slots + 1U),
        afterAgain.NextRegionIdentifier == (before.NextRegionIdentifier + (8U * (slots + 1U))),
        g_slots.GetSnapshot().Prepared == (slots + 1U),
        fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == (slots * Ia64RuntimeAddressSpaceSlots::SlotBytes));
}

void TestReceiptsAndConcurrentClaim(TestContext& runner) noexcept
{
    const auto firstStart = g_receipts[0U].GetRegionIdentifier(0U);
    const auto firstEnd = g_receipts[0U].GetRegionIdentifier(7U);
    const auto secondStart = g_receipts[1U].GetRegionIdentifier(0U);
    const Ia64RegionIdentifierAllocationReceipt copied = g_receipts[0U];
    const auto copiedStart = copied.GetRegionIdentifier(0U);
    ASTERIA_EXPECT(runner, "authoritative receipts are disjoint while copied-receipt uniqueness remains external",
                   firstStart.HasValue(), firstEnd.HasValue(), secondStart.HasValue(), copiedStart.HasValue(),
                   firstEnd.GetValue() < secondStart.GetValue(), copiedStart.GetValue() == firstStart.GetValue());

    const InitialMappingPlan* const seed = PreparedSeed();
    const bool initialized = (seed != nullptr) && (g_concurrentBackend.Initialize(g_profile, g_receipts[7U],
                                                                                  g_preparation, g_zeroBindings) ==
                                                   Ia64RuntimeAddressSpacePageTableInitializationError::None);
    const auto claimWorker = [seed](const Foundation::Size index) noexcept
    {
        if (seed != nullptr)
        {
            g_concurrentResults[index] = g_concurrentBackend.Claim(Owner(8U + index), *seed);
        }
    };
    std::thread first{claimWorker, Foundation::Size{0U}};
    std::thread second{claimWorker, Foundation::Size{1U}};
    first.join();
    second.join();
    const bool firstWon = g_concurrentResults[0U] == AddressSpacePageTableOwnershipError::None;
    const bool secondWon = g_concurrentResults[1U] == AddressSpacePageTableOwnershipError::None;
    const Foundation::Size winner = firstWon ? 0U : 1U;
    ASTERIA_EXPECT(runner, "concurrent Claim serializes to one success and one AlreadyOwned", initialized,
                   firstWon != secondWon,
                   (g_concurrentResults[0U] == AddressSpacePageTableOwnershipError::AlreadyOwned) !=
                       (g_concurrentResults[1U] == AddressSpacePageTableOwnershipError::AlreadyOwned));
    ASTERIA_EXPECT(runner, "the concurrent claim winner retires after simulated authoritative quiescence",
                   g_concurrentBackend.Release(Owner(8U + winner)) == AddressSpacePageTableOwnershipError::None);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64RuntimeAddressSpacePageTableTests"};
    const bool setup = Asteria::Test::Prepare(Asteria::Test::g_preparation, Asteria::Foundation::Size{0U}) &&
                       Asteria::Test::Prepare(Asteria::Test::g_equivalentPreparation, Asteria::Foundation::Size{1U}) &&
                       Asteria::Test::InitializeProfilesAndReceipts() && Asteria::Test::ConstructOwners();
    ASTERIA_EXPECT(runner, "real preparation, common profile, and authoritative receipts initialize fixtures", setup);
    if (!setup)
    {
        return runner.Finish();
    }
    Asteria::Test::TestInitializationAndClaim(runner);
    Asteria::Test::TestMapping(runner);
    Asteria::Test::TestUnmapping(runner);
    Asteria::Test::TestCapacity(runner);
    Asteria::Test::TestActivationAndRelease(runner);
    Asteria::Test::TestPhysicalBootstrapActivation(runner);
    Asteria::Test::TestOutsideCorridorActivation(runner);
    Asteria::Test::TestReceiptsAndConcurrentClaim(runner);
    Asteria::Test::TestTranslationCachePurge(runner);
    Asteria::Test::TestRuntimeAddressSpaceSlots(runner);
    return runner.Finish();
}
