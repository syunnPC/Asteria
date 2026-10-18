// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <new>
#include <thread>

#include "Kernel/Memory/FaultFrameMapping.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Platform/Amd64/Memory/RuntimePageTableMapper.hpp"
#include "Platform/Amd64/Memory/RuntimeAddressSpacePageTable.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FaultFrameMappingPreparation;
using Kernel::Memory::FaultFrameMappingRequest;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
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
using Kernel::Memory::VirtualAddress;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
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
using Kernel::Scheduler::ThreadId;
using Hal::Memory::AddressSpacePageTableActivationError;
using Platform::Amd64::Memory::InitialPageTableBuildError;
using Platform::Amd64::Memory::InitialPageTableBuildResult;
using Platform::Amd64::Memory::InitialPageTableBuilder;
using Platform::Amd64::Memory::InitialPageTablePreparation;
using Platform::Amd64::Memory::InitialPageTablePreparationSummary;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;
using Platform::Amd64::Memory::RuntimePageTableMapError;
using Platform::Amd64::Memory::RuntimePageTableMapper;
using Platform::Amd64::Memory::RuntimePageTableMapResult;
using Platform::Amd64::Memory::RuntimePageTableStorage;
using Platform::Amd64::Memory::RuntimeAddressSpacePageTable;
using Platform::Amd64::Memory::RuntimeAddressSpacePageTableInitializationError;
using Platform::Amd64::Memory::RuntimeAddressSpacePageTableState;
using Platform::Synchronization::SpinLock;

constexpr Foundation::Size TablePageCount{16U};
constexpr Foundation::Size EntriesPerPage{512U};
constexpr Foundation::Size EntryCount{TablePageCount * EntriesPerPage};
constexpr Foundation::UInt64 TablePhysicalStart{0x0100'0000U};
constexpr Foundation::UInt64 PresentBit{Foundation::UInt64{1U} << 0U};
constexpr Foundation::UInt64 WritableBit{Foundation::UInt64{1U} << 1U};
constexpr Foundation::UInt64 UserBit{Foundation::UInt64{1U} << 2U};
constexpr Foundation::UInt64 WriteThroughBit{Foundation::UInt64{1U} << 3U};
constexpr Foundation::UInt64 CacheDisableBit{Foundation::UInt64{1U} << 4U};
constexpr Foundation::UInt64 AccessedBit{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 PageAttributeTableBit{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 GlobalBit{Foundation::UInt64{1U} << 8U};
constexpr Foundation::UInt64 ExecuteDisableBit{Foundation::UInt64{1U} << 63U};
constexpr Foundation::UInt64 PhysicalAddressMask{0x000F'FFFF'FFFF'F000ULL};
constexpr Foundation::UInt64 Cr4PhysicalAddressExtension{Foundation::UInt64{1U} << 5U};
constexpr Foundation::UInt64 Cr4PageGlobalEnable{Foundation::UInt64{1U} << 7U};
constexpr Foundation::UInt64 Cr4LinearAddress57{Foundation::UInt64{1U} << 12U};
constexpr Foundation::UInt64 Cr4ProcessContextIdentifierEnable{Foundation::UInt64{1U} << 17U};

alignas(4096) Foundation::UInt64 g_entries[EntryCount]{};
Foundation::UInt64 g_snapshot[EntryCount]{};
Foundation::Size g_cr3LoadCount{0U};
Foundation::UInt64 g_lastRequestedCr3{0U};
Foundation::UInt64 g_forcedCr3Readback{0U};
bool g_echoRequestedCr3{true};
Foundation::Size g_cr4ReadCount{0U};
Foundation::UInt64 g_liveCr4{Cr4PhysicalAddressExtension};

} // Namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64LoadCr3AndReadBack(const Asteria::Foundation::UInt64 rootPhysicalAddress) noexcept
{
    ++Asteria::Test::g_cr3LoadCount;
    Asteria::Test::g_lastRequestedCr3 = rootPhysicalAddress;
    return Asteria::Test::g_echoRequestedCr3 ? rootPhysicalAddress : Asteria::Test::g_forcedCr3Readback;
}

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadCr4() noexcept
{
    ++Asteria::Test::g_cr4ReadCount;
    return Asteria::Test::g_liveCr4;
}

namespace Asteria::Test
{

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject(const ObjectType type, const ResourceDomainId owner) noexcept
        : m_type{type}, m_owner{owner}
    {
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_owner;
    }

private:
    ObjectType m_type;
    ResourceDomainId m_owner;
};

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr PagePermissions ReadExecuteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
}

[[nodiscard]] constexpr MappingPermissions BootstrapReadOnly() noexcept
{
    return MappingPermissions{MappingPermission::Read};
}

[[nodiscard]] constexpr MappingPermissions BootstrapReadWrite() noexcept
{
    return BootstrapReadOnly().With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions BootstrapReadExecute() noexcept
{
    return BootstrapReadOnly().With(MappingPermission::Execute);
}

[[nodiscard]] constexpr InitialMapping MakeBootstrapMapping(const Foundation::UInt64 virtualStart,
                                                            const Foundation::UInt64 physicalStart,
                                                            const Foundation::UInt64 pages,
                                                            const MappingPermissions permissions,
                                                            const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr PagingCapabilities MakeCapabilities() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 48U,
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = false,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr RuntimePageTableStorage
MakeStorage(const Foundation::Size usedPages = 1U, const Foundation::Size capacity = TablePageCount,
            const Foundation::UInt64 physicalStart = TablePhysicalStart) noexcept
{
    return RuntimePageTableStorage{
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Entries = g_entries,
        .PageCapacity = capacity,
        .UsedPageCount = usedPages,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr RuntimePageTableMapResult ResultSentinel() noexcept
{
    return RuntimePageTableMapResult{
        .UsedTablePages = 0x1234U,
        .MappedPages = PageCount{0x5678U},
        .InvalidationStart = VirtualAddress{0xABCD'0000U},
        .Reserved = 0x5A5AU,
    };
}

[[nodiscard]] constexpr bool ResultsEqual(const RuntimePageTableMapResult& first,
                                          const RuntimePageTableMapResult& second) noexcept
{
    return (first.UsedTablePages == second.UsedTablePages) &&
           (first.MappedPages.GetValue() == second.MappedPages.GetValue()) &&
           (first.InvalidationStart.GetValue() == second.InvalidationStart.GetValue()) &&
           (first.Reserved == second.Reserved);
}

void ClearEntries() noexcept
{
    for (Foundation::Size index{0U}; index < EntryCount; ++index)
    {
        g_entries[index] = 0U;
    }
}

void ResetNativeActivationLog() noexcept
{
    g_cr3LoadCount = 0U;
    g_lastRequestedCr3 = 0U;
    g_forcedCr3Readback = 0U;
    g_echoRequestedCr3 = true;
    g_cr4ReadCount = 0U;
    g_liveCr4 = Cr4PhysicalAddressExtension;
}

void SaveEntries() noexcept
{
    for (Foundation::Size index{0U}; index < EntryCount; ++index)
    {
        g_snapshot[index] = g_entries[index];
    }
}

[[nodiscard]] bool EntriesMatchSnapshot() noexcept
{
    for (Foundation::Size index{0U}; index < EntryCount; ++index)
    {
        if (g_entries[index] != g_snapshot[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Foundation::UInt64& Entry(const Foundation::Size page, const Foundation::Size index) noexcept
{
    return g_entries[(page * EntriesPerPage) + index];
}

void ExpectUnmapFailureAtomic(TestContext& runner, const AddressSpaceMappingRecord& mapping,
                              const RuntimePageTableStorage& storage, const RuntimePageTableMapError expected,
                              const char* const description) noexcept;

void ExpectFailureAtomic(TestContext& runner, const AddressSpaceMappingRecord& mapping,
                         const PagingCapabilities& capabilities, const RuntimePageTableStorage& storage,
                         const RuntimePageTableMapError expected, const char* const description) noexcept
{
    SaveEntries();
    RuntimePageTableMapResult output = ResultSentinel();
    const RuntimePageTableMapResult expectedOutput = output;
    const RuntimePageTableMapError error = RuntimePageTableMapper::Map(mapping, capabilities, storage, output);
    ASTERIA_EXPECT(runner, description, error == expected, ResultsEqual(output, expectedOutput),
                   EntriesMatchSnapshot());
}

void ExpectUnmapFailureAtomic(TestContext& runner, const AddressSpaceMappingRecord& mapping,
                              const RuntimePageTableStorage& storage, const RuntimePageTableMapError expected,
                              const char* const description) noexcept
{
    SaveEntries();
    RuntimePageTableMapResult output = ResultSentinel();
    const RuntimePageTableMapResult expectedOutput = output;
    const RuntimePageTableMapError error = RuntimePageTableMapper::Unmap(mapping, MakeCapabilities(), storage, output);
    ASTERIA_EXPECT(runner, description, error == expected, ResultsEqual(output, expectedOutput),
                   EntriesMatchSnapshot());
}

void TestBasicAndIncrementalMapping(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    RuntimePageTableMapResult output = ResultSentinel();
    const RuntimePageTableMapError error =
        RuntimePageTableMapper::Map(validMapping, MakeCapabilities(), MakeStorage(), output);
    const Foundation::UInt64 ancestorFlags = PresentBit | WritableBit | UserBit;
    const Foundation::UInt64 leafFlags = ancestorFlags | ExecuteDisableBit;
    ASTERIA_EXPECT(runner, "an empty root receives exact user RW 4 KiB leaves and three tables",
                   error == RuntimePageTableMapError::None, output.UsedTablePages == 4U,
                   output.MappedPages.GetValue() == 2U, output.InvalidationStart.GetValue() == 0x0040'0000U,
                   output.Reserved == 0U, Entry(0U, 0U) == ((TablePhysicalStart + 0x1000U) | ancestorFlags),
                   Entry(1U, 0U) == ((TablePhysicalStart + 0x2000U) | ancestorFlags),
                   Entry(2U, 2U) == ((TablePhysicalStart + 0x3000U) | ancestorFlags),
                   Entry(3U, 0U) == (0x0020'0000U | leafFlags), Entry(3U, 1U) == (0x0020'1000U | leafFlags));

    AddressSpaceMappingRecord incremental = validMapping;
    incremental.VirtualStart = VirtualAddress{0x0040'2000U};
    incremental.PhysicalStart = PhysicalAddress{0x0020'2000U};
    incremental.Pages = PageCount{1U};
    RuntimePageTableMapResult incrementalOutput = ResultSentinel();
    const RuntimePageTableMapError incrementalError = RuntimePageTableMapper::Map(
        incremental, MakeCapabilities(), MakeStorage(output.UsedTablePages), incrementalOutput);
    ASTERIA_EXPECT(runner, "an adjacent mapping reuses the existing hierarchy",
                   incrementalError == RuntimePageTableMapError::None, incrementalOutput.UsedTablePages == 4U,
                   Entry(3U, 2U) == (0x0020'2000U | leafFlags));

    Entry(0U, 0U) |= AccessedBit;
    Entry(1U, 0U) |= AccessedBit;
    Entry(2U, 2U) |= AccessedBit;
    Entry(3U, 0U) |= AccessedBit | (Foundation::UInt64{1U} << 6U);
    AddressSpaceMappingRecord accessed = incremental;
    accessed.VirtualStart = VirtualAddress{0x0040'3000U};
    accessed.PhysicalStart = PhysicalAddress{0x0020'3000U};
    RuntimePageTableMapResult accessedOutput{};
    ASTERIA_EXPECT(runner, "hardware Accessed bits on ancestors remain valid during extension",
                   RuntimePageTableMapper::Map(accessed, MakeCapabilities(),
                                               MakeStorage(incrementalOutput.UsedTablePages),
                                               accessedOutput) == RuntimePageTableMapError::None,
                   accessedOutput.UsedTablePages == 4U, Entry(3U, 3U) == (0x0020'3000U | leafFlags));
}

// Unmap clears exactly the record's leaves, keeps the tables, and refuses a
// record whose leaves are absent or not the ones Map installed, unchanged.
void TestUnmap(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    RuntimePageTableMapResult mapped{};
    const bool installed = RuntimePageTableMapper::Map(validMapping, MakeCapabilities(), MakeStorage(), mapped) ==
                           RuntimePageTableMapError::None;
    AddressSpaceMappingRecord neighbour = validMapping;
    neighbour.VirtualStart = VirtualAddress{0x0040'2000U};
    neighbour.PhysicalStart = PhysicalAddress{0x0020'2000U};
    neighbour.Pages = PageCount{1U};
    RuntimePageTableMapResult neighbourOutput{};
    const bool neighbourInstalled =
        RuntimePageTableMapper::Map(neighbour, MakeCapabilities(), MakeStorage(mapped.UsedTablePages),
                                    neighbourOutput) == RuntimePageTableMapError::None;
    Entry(3U, 1U) |= AccessedBit | (Foundation::UInt64{1U} << 6U);
    const Foundation::UInt64 ancestor = Entry(2U, 2U);
    RuntimePageTableMapResult output = ResultSentinel();
    const RuntimePageTableMapError error = RuntimePageTableMapper::Unmap(
        validMapping, MakeCapabilities(), MakeStorage(neighbourOutput.UsedTablePages), output);
    ASTERIA_EXPECT(runner, "Unmap clears the record's leaves, keeps the tables and its neighbour, and names the range",
                   installed, neighbourInstalled, error == RuntimePageTableMapError::None,
                   output.UsedTablePages == neighbourOutput.UsedTablePages, output.MappedPages.GetValue() == 2U,
                   output.InvalidationStart.GetValue() == 0x0040'0000U, output.Reserved == 0U, Entry(3U, 0U) == 0U,
                   Entry(3U, 1U) == 0U, Entry(3U, 2U) != 0U, Entry(2U, 2U) == ancestor);

    ExpectUnmapFailureAtomic(runner, validMapping, MakeStorage(neighbourOutput.UsedTablePages),
                             RuntimePageTableMapError::MappingAbsent, "a record already unmapped is absent");
    AddressSpaceMappingRecord unreached = validMapping;
    unreached.VirtualStart = VirtualAddress{0x7000'0000'0000ULL};
    ExpectUnmapFailureAtomic(runner, unreached, MakeStorage(neighbourOutput.UsedTablePages),
                             RuntimePageTableMapError::MappingAbsent, "a record under an absent table is absent");
    AddressSpaceMappingRecord readOnly = neighbour;
    readOnly.Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::User);
    ExpectUnmapFailureAtomic(runner, readOnly, MakeStorage(neighbourOutput.UsedTablePages),
                             RuntimePageTableMapError::MappingMismatch,
                             "a leaf with other permissions is not the record's");
    AddressSpaceMappingRecord elsewhere = neighbour;
    elsewhere.PhysicalStart = PhysicalAddress{0x0020'5000U};
    ExpectUnmapFailureAtomic(runner, elsewhere, MakeStorage(neighbourOutput.UsedTablePages),
                             RuntimePageTableMapError::MappingMismatch, "a leaf of another page is not the record's");

    RuntimePageTableMapResult remapped{};
    ASTERIA_EXPECT(runner, "an unmapped range maps again without new tables",
                   RuntimePageTableMapper::Map(validMapping, MakeCapabilities(),
                                               MakeStorage(neighbourOutput.UsedTablePages),
                                               remapped) == RuntimePageTableMapError::None,
                   remapped.UsedTablePages == neighbourOutput.UsedTablePages,
                   Entry(3U, 0U) == (0x0020'0000U | PresentBit | WritableBit | UserBit | ExecuteDisableBit));
}

void TestBoundariesAndEncoding(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    AddressSpaceMappingRecord crossing = validMapping;
    crossing.VirtualStart = VirtualAddress{0x005F'F000U};
    crossing.PhysicalStart = PhysicalAddress{0x0030'0000U};
    crossing.Pages = PageCount{2U};
    RuntimePageTableMapResult crossingOutput{};
    ASTERIA_EXPECT(runner, "a range crossing 2 MiB allocates exactly two leaf tables",
                   RuntimePageTableMapper::Map(crossing, MakeCapabilities(), MakeStorage(), crossingOutput) ==
                       RuntimePageTableMapError::None,
                   crossingOutput.UsedTablePages == 5U, (Entry(2U, 2U) & ~0xFFFULL) == (TablePhysicalStart + 0x3000U),
                   (Entry(2U, 3U) & ~0xFFFULL) == (TablePhysicalStart + 0x4000U),
                   (Entry(3U, 511U) & ~ExecuteDisableBit & ~0xFFFULL) == 0x0030'0000U,
                   (Entry(4U, 0U) & ~ExecuteDisableBit & ~0xFFFULL) == 0x0030'1000U);

    ClearEntries();
    AddressSpaceMappingRecord oneGibCrossing = validMapping;
    oneGibCrossing.VirtualStart = VirtualAddress{0x0000'0000'3FFF'F000ULL};
    oneGibCrossing.PhysicalStart = PhysicalAddress{0x0030'0000U};
    oneGibCrossing.Pages = PageCount{2U};
    RuntimePageTableMapResult oneGibOutput{};
    ASTERIA_EXPECT(runner, "a range crossing 1 GiB counts two directory and leaf-table branches",
                   RuntimePageTableMapper::Map(oneGibCrossing, MakeCapabilities(), MakeStorage(), oneGibOutput) ==
                       RuntimePageTableMapError::None,
                   oneGibOutput.UsedTablePages == 6U);

    ClearEntries();
    AddressSpaceMappingRecord pml4Crossing = validMapping;
    pml4Crossing.VirtualStart = VirtualAddress{0x0000'007F'FFFF'F000ULL};
    pml4Crossing.PhysicalStart = PhysicalAddress{0x0030'0000U};
    pml4Crossing.Pages = PageCount{2U};
    RuntimePageTableMapResult pml4Output{};
    ASTERIA_EXPECT(runner, "a range crossing 512 GiB counts both complete hierarchy branches",
                   RuntimePageTableMapper::Map(pml4Crossing, MakeCapabilities(), MakeStorage(), pml4Output) ==
                       RuntimePageTableMapError::None,
                   pml4Output.UsedTablePages == 7U);

    ClearEntries();
    AddressSpaceMappingRecord maximum = validMapping;
    maximum.Pages = PageCount{512U};
    RuntimePageTableMapResult maximumOutput{};
    ASTERIA_EXPECT(runner, "the bounded 512-page transaction maps its complete range",
                   RuntimePageTableMapper::Map(maximum, MakeCapabilities(), MakeStorage(), maximumOutput) ==
                       RuntimePageTableMapError::None,
                   maximumOutput.UsedTablePages == 4U, (Entry(3U, 0U) & ~ExecuteDisableBit & ~0xFFFULL) == 0x0020'0000U,
                   (Entry(3U, 511U) & ~ExecuteDisableBit & ~0xFFFULL) == 0x003F'F000U);

    ClearEntries();
    AddressSpaceMappingRecord upper = validMapping;
    upper.VirtualStart = VirtualAddress{0xFFFF'8000'0000'0000ULL};
    upper.Pages = PageCount{1U};
    RuntimePageTableMapResult upperOutput{};
    ExpectFailureAtomic(runner, upper, MakeCapabilities(), MakeStorage(), RuntimePageTableMapError::InvalidMappingPlan,
                        "upper-half user mapping is refused before modifying tables");
    ClearEntries();
    upper.Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::Write);
    ASTERIA_EXPECT(runner, "the first upper-canonical supervisor page is accepted",
                   RuntimePageTableMapper::Map(upper, MakeCapabilities(), MakeStorage(), upperOutput) ==
                       RuntimePageTableMapError::None,
                   Entry(0U, 256U) != 0U);

    ClearEntries();
    AddressSpaceMappingRecord lastUser = validMapping;
    lastUser.VirtualStart = VirtualAddress{0x0000'7FFF'FFFF'F000ULL};
    lastUser.Pages = PageCount{1U};
    RuntimePageTableMapResult lastOutput{};
    ASTERIA_EXPECT(runner, "the complete final lower-canonical user page remains mappable",
                   RuntimePageTableMapper::Map(lastUser, MakeCapabilities(), MakeStorage(), lastOutput) ==
                       RuntimePageTableMapError::None);

    ClearEntries();
    AddressSpaceMappingRecord zero = validMapping;
    zero.VirtualStart = VirtualAddress{};
    zero.Pages = PageCount{1U};
    ExpectFailureAtomic(runner, zero, MakeCapabilities(), MakeStorage(), RuntimePageTableMapError::InvalidMappingPlan,
                        "page-zero user mapping follows the same policy as thread start and return");

    struct EncodingCase
    {
        MemoryCachePolicy CachePolicy;
        PagePermissions Permissions;
        Foundation::UInt64 ExpectedLeafFlags;
        const char* Description;
    };
    const EncodingCase cases[]{
        {MemoryCachePolicy::WriteThrough, ReadWriteUser(),
         PresentBit | WritableBit | UserBit | WriteThroughBit | ExecuteDisableBit,
         "write-through cache bits are encoded"},
        {MemoryCachePolicy::Uncached, ReadWriteUser(),
         PresentBit | WritableBit | UserBit | WriteThroughBit | CacheDisableBit | ExecuteDisableBit,
         "uncached cache bits are encoded"},
        {MemoryCachePolicy::WriteCombining, ReadWriteUser(),
         PresentBit | WritableBit | UserBit | PageAttributeTableBit | ExecuteDisableBit,
         "write-combining PAT bits are encoded"},
        {MemoryCachePolicy::WriteBack, ReadExecuteUser(), PresentBit | UserBit,
         "executable mappings omit NX and writable bits"},
    };
    for (const EncodingCase& encoding : cases)
    {
        ClearEntries();
        AddressSpaceMappingRecord mapping = validMapping;
        mapping.Pages = PageCount{1U};
        mapping.CachePolicy = encoding.CachePolicy;
        mapping.Permissions = encoding.Permissions;
        RuntimePageTableMapResult encodedOutput{};
        const RuntimePageTableMapError encodedError =
            RuntimePageTableMapper::Map(mapping, MakeCapabilities(), MakeStorage(), encodedOutput);
        ASTERIA_EXPECT(runner, encoding.Description, encodedError == RuntimePageTableMapError::None,
                       Entry(3U, 0U) == (mapping.PhysicalStart.GetValue() | encoding.ExpectedLeafFlags));
    }
}

// A broker chunk is 2 MiB aligned but the runtime mapper emits 512 4 KiB
// user leaves, never a PS leaf. This also pins bit 12 as an address bit in PTEs.
void TestScanoutDeviceLeaves(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    AddressSpaceMappingRecord mapping = validMapping;
    mapping.VirtualStart = VirtualAddress{0x8000'0000U};
    mapping.PhysicalStart = PhysicalAddress{0xE000'0000U};
    mapping.Pages = PageCount{512U};
    mapping.Kind = FrameKind::Device;
    mapping.CachePolicy = MemoryCachePolicy::WriteCombining;
    mapping.Permissions = ReadWriteUser();
    RuntimePageTableMapResult output{};
    ASTERIA_EXPECT(runner, "write-combining user device chunk is accepted without a cache-policy downgrade",
                   RuntimePageTableMapper::Map(mapping, MakeCapabilities(), MakeStorage(), output) ==
                       RuntimePageTableMapError::None,
                   output.MappedPages.GetValue() == 512U, output.UsedTablePages == 4U);
    for (Foundation::Size page{0U}; page < 512U; ++page)
    {
        ASTERIA_EXPECT(runner, "every 4 KiB device leaf selects PAT[4]: PAT=1 PCD=PWT=0, user RW NX",
                       Entry(3U, page) == ((0xE000'0000ULL + (page << 12U)) | ExecuteDisableBit | 0x87U));
    }
    ASTERIA_EXPECT(runner, "2 MiB and 1 GiB slots are table pointers with PS/PAT clear",
                   Entry(0U, 0U) == ((TablePhysicalStart + 4096U) | 7U),
                   Entry(1U, 2U) == ((TablePhysicalStart + 8192U) | 7U),
                   Entry(2U, 0U) == ((TablePhysicalStart + 12288U) | 7U));
    ASTERIA_EXPECT(runner, "exact WC leaves unmap with the same encoder",
                   RuntimePageTableMapper::Unmap(mapping, MakeCapabilities(), MakeStorage(output.UsedTablePages),
                                                 output) == RuntimePageTableMapError::None,
                   Entry(3U, 511U) == 0U);
}

void TestFailureAtomicity(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    AddressSpaceMappingRecord invalid = validMapping;
    invalid.Pages = PageCount{};
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::InvalidMappingPlan, "a zero-page mapping is rejected without writes");

    invalid = validMapping;
    invalid.Pages = PageCount{513U};
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(), RuntimePageTableMapError::MappingTooLarge,
                        "a transaction larger than 2 MiB is rejected without writes");

    invalid = validMapping;
    invalid.VirtualStart = VirtualAddress{0x0000'7FFF'FFFF'F000ULL};
    invalid.Pages = PageCount{2U};
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::NonCanonicalVirtualAddress,
                        "a range crossing the canonical hole is rejected");

    invalid = validMapping;
    invalid.Permissions = ReadExecuteUser().With(PagePermission::Write);
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::InvalidMappingPlan,
                        "W+X is rejected again at the native encoder boundary");

    invalid = validMapping;
    invalid.RuntimeMapping = false;
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::InvalidMappingPlan,
                        "a non-runtime record is rejected without writes");

    invalid = validMapping;
    invalid.Reserved[2] = 1U;
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::InvalidMappingPlan, "nonzero mapping reserved bytes are rejected");

    invalid = validMapping;
    invalid.Kind = FrameKind::Device;
    invalid.CachePolicy = MemoryCachePolicy::WriteBack;
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::InvalidMappingPlan,
                        "a device mapping cannot forge a cacheable RAM policy");

    invalid = validMapping;
    invalid.PhysicalStart = PhysicalAddress{0x0001'0000'0000'0000ULL};
    ExpectFailureAtomic(runner, invalid, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::PhysicalAddressOutOfRange,
                        "a physical range beyond the captured width is rejected");

    PagingCapabilities capabilities = MakeCapabilities();
    capabilities.PhysicalAddressBits = 31U;
    ExpectFailureAtomic(runner, validMapping, capabilities, MakeStorage(),
                        RuntimePageTableMapError::InvalidCapabilities, "an invalid physical-width report is rejected");
    capabilities = MakeCapabilities();
    capabilities.FourLevelPagingEnabled = false;
    ExpectFailureAtomic(runner, validMapping, capabilities, MakeStorage(),
                        RuntimePageTableMapError::FourLevelPagingRequired,
                        "five-level or disabled four-level paging is rejected");
    capabilities = MakeCapabilities();
    capabilities.SupervisorWriteProtectEnabled = false;
    ExpectFailureAtomic(runner, validMapping, capabilities, MakeStorage(),
                        RuntimePageTableMapError::SupervisorWriteProtectRequired,
                        "supervisor write protection is required");
    capabilities = MakeCapabilities();
    capabilities.PatLayoutConfigured = false;
    ExpectFailureAtomic(runner, validMapping, capabilities, MakeStorage(),
                        RuntimePageTableMapError::PageAttributeTableRequired, "the committed PAT layout is required");
    capabilities = MakeCapabilities();
    capabilities.ExecuteDisableEnabled = false;
    ExpectFailureAtomic(runner, validMapping, capabilities, MakeStorage(),
                        RuntimePageTableMapError::ExecuteDisableRequired,
                        "NX support is required for a non-executable mapping");
    AddressSpaceMappingRecord executableWithoutNx = validMapping;
    executableWithoutNx.Permissions = ReadExecuteUser();
    ExpectFailureAtomic(runner, executableWithoutNx, capabilities, MakeStorage(),
                        RuntimePageTableMapError::ExecuteDisableRequired,
                        "NX remains required to validate every existing writable leaf");

    RuntimePageTableStorage invalidStorage = MakeStorage();
    invalidStorage.Entries = nullptr;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), invalidStorage,
                        RuntimePageTableMapError::InvalidStorage, "null table storage is rejected");
    invalidStorage = MakeStorage();
    invalidStorage.UsedPageCount = 0U;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), invalidStorage,
                        RuntimePageTableMapError::InvalidStorage, "an arena without a root page is rejected");
    invalidStorage = MakeStorage();
    invalidStorage.Reserved = 1U;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), invalidStorage,
                        RuntimePageTableMapError::InvalidStorage, "nonzero storage reserved state is rejected");

    AddressSpaceMappingRecord aliasesTables = validMapping;
    aliasesTables.PhysicalStart = PhysicalAddress{TablePhysicalStart};
    ExpectFailureAtomic(runner, aliasesTables, MakeCapabilities(), MakeStorage(),
                        RuntimePageTableMapError::MappingAliasesTableStorage,
                        "a requested mapping cannot expose its page-table arena");

    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(1U, 3U),
                        RuntimePageTableMapError::InsufficientTableStorage,
                        "insufficient table pages fail before clearing spare storage");

    ClearEntries();
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | PresentBit | WritableBit | UserBit | PageAttributeTableBit;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(2U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "reserved or large-page ancestor bits are rejected");

    ClearEntries();
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | PresentBit | WritableBit;
    RuntimePageTableMapResult promotedOutput{};
    ASTERIA_EXPECT(runner, "a user mapping promotes reused supervisor ancestors during commit",
                   RuntimePageTableMapper::Map(validMapping, MakeCapabilities(), MakeStorage(2U), promotedOutput) ==
                       RuntimePageTableMapError::None,
                   promotedOutput.UsedTablePages == 4U, (Entry(0U, 0U) & UserBit) != 0U,
                   (Entry(1U, 0U) & UserBit) != 0U, (Entry(2U, 2U) & UserBit) != 0U);

    ClearEntries();
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | PresentBit | WritableBit;
    Entry(1U, 0U) = (TablePhysicalStart + 0x2000U) | PresentBit | WritableBit | UserBit;
    Entry(2U, 2U) = (TablePhysicalStart + 0x3000U) | PresentBit | WritableBit | UserBit;
    Entry(3U, 10U) = 0x0040'0000U | PresentBit | UserBit | ExecuteDisableBit;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(4U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "a user leaf below a supervisor-only ancestor is malformed");

    ClearEntries();
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | PresentBit | WritableBit;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(2U, 3U),
                        RuntimePageTableMapError::InsufficientTableStorage,
                        "capacity failure occurs before a reused ancestor is promoted");

    ClearEntries();
    const Foundation::UInt64 ancestorFlags = PresentBit | WritableBit | UserBit;
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | ancestorFlags;
    Entry(0U, 1U) = (TablePhysicalStart + 0x1000U) | ancestorFlags;
    Entry(1U, 0U) = (TablePhysicalStart + 0x2000U) | ancestorFlags;
    Entry(2U, 2U) = (TablePhysicalStart + 0x3000U) | ancestorFlags;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(4U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "a table page referenced by two parents cannot create a virtual alias");

    ClearEntries();
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(2U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "every used table page must be reachable from the root exactly once");

    ClearEntries();
    Entry(0U, 1U) = (TablePhysicalStart + 0x1000U) | ancestorFlags;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(1U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "an underreported used count cannot clear a referenced sibling table");

    ClearEntries();
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | ancestorFlags;
    Entry(1U, 0U) = (TablePhysicalStart + 0x2000U) | ancestorFlags;
    Entry(2U, 2U) = (TablePhysicalStart + 0x3000U) | ancestorFlags;
    Entry(3U, 10U) = TablePhysicalStart | PresentBit | WritableBit | UserBit | ExecuteDisableBit;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(4U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "an existing user leaf cannot expose the page-table arena");

    ClearEntries();
    Entry(0U, 0U) = (TablePhysicalStart + 0x1000U) | ancestorFlags;
    Entry(1U, 0U) = (TablePhysicalStart + 0x2000U) | ancestorFlags;
    Entry(2U, 2U) = (TablePhysicalStart + 0x3000U) | ancestorFlags;
    Entry(3U, 10U) = 0x0040'0000U | PresentBit | GlobalBit | ExecuteDisableBit;
    ExpectFailureAtomic(runner, validMapping, MakeCapabilities(), MakeStorage(4U),
                        RuntimePageTableMapError::InvalidTableEntry,
                        "a global leaf requires the captured global-pages capability");

    ClearEntries();
    AddressSpaceMappingRecord existing = validMapping;
    existing.VirtualStart = VirtualAddress{0x0040'1000U};
    existing.PhysicalStart = PhysicalAddress{0x0020'1000U};
    existing.Pages = PageCount{1U};
    RuntimePageTableMapResult existingOutput{};
    const RuntimePageTableMapError existingError =
        RuntimePageTableMapper::Map(existing, MakeCapabilities(), MakeStorage(), existingOutput);
    AddressSpaceMappingRecord crossingExisting = validMapping;
    crossingExisting.Pages = PageCount{2U};
    SaveEntries();
    RuntimePageTableMapResult collisionOutput = ResultSentinel();
    const RuntimePageTableMapResult expectedCollisionOutput = collisionOutput;
    const RuntimePageTableMapError collisionError = RuntimePageTableMapper::Map(
        crossingExisting, MakeCapabilities(), MakeStorage(existingOutput.UsedTablePages), collisionOutput);
    ASTERIA_EXPECT(runner, "a later occupied leaf prevents every earlier write in the request",
                   existingError == RuntimePageTableMapError::None,
                   collisionError == RuntimePageTableMapError::MappingAlreadyPresent,
                   ResultsEqual(collisionOutput, expectedCollisionOutput), EntriesMatchSnapshot(), Entry(3U, 0U) == 0U);

    ClearEntries();
    AddressSpaceMappingRecord writableAlias = validMapping;
    writableAlias.Pages = PageCount{1U};
    RuntimePageTableMapResult writableAliasOutput{};
    const RuntimePageTableMapError writableAliasError =
        RuntimePageTableMapper::Map(writableAlias, MakeCapabilities(), MakeStorage(), writableAliasOutput);
    AddressSpaceMappingRecord executableAlias = writableAlias;
    executableAlias.VirtualStart = VirtualAddress{0x0080'0000U};
    executableAlias.Permissions = ReadExecuteUser();
    ExpectFailureAtomic(runner, executableAlias, MakeCapabilities(), MakeStorage(writableAliasOutput.UsedTablePages),
                        RuntimePageTableMapError::PhysicalWriteExecuteAlias,
                        "one physical page cannot acquire separate writable and executable aliases");
    ASTERIA_EXPECT(runner, "the writable half of the physical-alias test is installed first",
                   writableAliasError == RuntimePageTableMapError::None);
}

// A runtime mapping never carries G, even where global pages are enabled for the seed: a CR3 write
// then drops every translation a retired root installed, which a destroyed AddressSpace relies on
// when it quiesces with mappings left.
void TestRuntimeLeavesAreNotGlobal(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    PagingCapabilities global = MakeCapabilities();
    global.GlobalPagesEnabled = true;
    RuntimePageTableMapResult output = ResultSentinel();
    const RuntimePageTableMapError error = RuntimePageTableMapper::Map(validMapping, global, MakeStorage(), output);
    ASTERIA_EXPECT(runner, "runtime leaves stay non-global with global pages enabled",
                   error == RuntimePageTableMapError::None, (Entry(3U, 0U) & PresentBit) != 0U,
                   (Entry(3U, 0U) & GlobalBit) == 0U, (Entry(3U, 1U) & GlobalBit) == 0U);
}

void TestVerifiedBootstrapExtension(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    ClearEntries();
    InitialMappingPlan initialPlan;
    bool planReady = true;
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0010'0000U, 0x0010'0000U, 2U, BootstrapReadExecute(),
                                                    InitialMappingOwner::NucleusCode)) != InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0010'2000U, 0x0010'2000U, 1U, BootstrapReadOnly(),
                                                    InitialMappingOwner::NucleusReadOnlyData)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0010'3000U, 0x0010'3000U, 1U, BootstrapReadOnly(),
                                                    InitialMappingOwner::NucleusReadOnlyData)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0010'4000U, 0x0010'4000U, 2U, BootstrapReadWrite(),
                                                    InitialMappingOwner::NucleusWritableData)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0010'6000U, 0x0010'6000U, 1U, BootstrapReadOnly(),
                                                    InitialMappingOwner::NucleusReadOnlyData)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0010'7000U, 0x0010'7000U, 1U, BootstrapReadOnly(),
                                                    InitialMappingOwner::NucleusReadOnlyData)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0030'0000U, 0x0030'0000U, 4U, BootstrapReadWrite(),
                                                    InitialMappingOwner::NucleusStack)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(0x0110'0000U, 0x0110'0000U, 2U, BootstrapReadWrite(),
                                                    InitialMappingOwner::NucleusDescriptorTables)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (initialPlan.AddMapping(MakeBootstrapMapping(TablePhysicalStart, TablePhysicalStart, TablePageCount,
                                                    BootstrapReadWrite(), InitialMappingOwner::PageTable)) !=
        InitialMappingPlanError::None)
    {
        planReady = false;
    }
    if (planReady && (initialPlan.Seal() != InitialMappingPlanError::None))
    {
        planReady = false;
    }

    // A small verified seed owns a full large arena.
    alignas(4096) static Foundation::UInt64 largeEntries[256U * EntriesPerPage]{};
    static InitialMappingPlan largePlan;
    static InitialPageTablePreparation largePreparation;
    static RuntimeAddressSpacePageTable largeBackend;
    bool largeSeed{planReady};
    for (Foundation::Size i{0U}; largeSeed && i < initialPlan.GetMappingCount(); ++i)
    {
        auto mapping = initialPlan.GetMapping(i).GetValue();
        if (mapping.Owner == InitialMappingOwner::PageTable)
        {
            mapping.Pages = PageCount{256U};
        }
        largeSeed = largePlan.AddMapping(mapping) == InitialMappingPlanError::None;
    }
    largeSeed = largeSeed && largePlan.Seal() == InitialMappingPlanError::None;
    const PageTableStorage largeStorage{PhysicalAddress{TablePhysicalStart}, largeEntries, 256U, 0U};
    auto largeCapabilities = MakeCapabilities();
    largeCapabilities.GlobalPagesEnabled = true;
    const bool largePrepared =
        largeSeed && largePreparation.Prepare(largePlan, largeCapabilities, largeStorage).HasValue();
    ASTERIA_EXPECT(runner, "large seed validation and backend retain the full per-storage capacity", largePrepared,
                   largeBackend.Initialize(largeCapabilities, largePreparation, largeEntries) ==
                       RuntimeAddressSpacePageTableInitializationError::None,
                   largeBackend.IsReady(), largeBackend.GetSnapshot().PageCapacity == 256U,
                   largeBackend.GetSnapshot().UsedTablePages < 64U);

    PagingCapabilities bootstrapCapabilities = MakeCapabilities();
    bootstrapCapabilities.GlobalPagesEnabled = true;
    const PageTableStorage bootstrapStorage{
        .PhysicalStart = PhysicalAddress{TablePhysicalStart},
        .Entries = g_entries,
        .PageCapacity = TablePageCount,
        .Reserved = 0U,
    };
    alignas(InitialPageTablePreparation)
        Foundation::UInt8 bootstrapPreparationStorage[sizeof(InitialPageTablePreparation)]{};
    InitialPageTablePreparation* const bootstrapPreparation =
        ::new (static_cast<void*>(bootstrapPreparationStorage)) InitialPageTablePreparation{};
    bool preparationReady{false};
    if (planReady)
    {
        preparationReady =
            bootstrapPreparation->Prepare(initialPlan, bootstrapCapabilities, bootstrapStorage).HasValue();
    }
    const auto summaryResult = bootstrapPreparation->TryGetSummary();
    const auto preparedPlanResult = bootstrapPreparation->TryGetMappingPlan();
    const auto directVerificationError =
        summaryResult.HasValue() && preparedPlanResult.HasValue()
            ? Platform::Amd64::Memory::InitialPageTableVerifier::Verify(
                  *preparedPlanResult.GetValue(), bootstrapCapabilities, *summaryResult.GetValue())
            : Platform::Amd64::Memory::InitialPageTableVerificationError::InvalidSummary;
    const bool preparationPublished =
        preparationReady && summaryResult.HasValue() && preparedPlanResult.HasValue() &&
        (directVerificationError == Platform::Amd64::Memory::InitialPageTableVerificationError::None);
    InitialPageTableBuildResult buildResult{};
    InitialPageTableBuildError buildError = InitialPageTableBuildError::PlanNotSealed;
    if (summaryResult.HasValue())
    {
        const InitialPageTablePreparationSummary& summary = *summaryResult.GetValue();
        buildResult = InitialPageTableBuildResult{
            .RootPhysicalAddress = summary.Storage.RootPhysicalAddress,
            .UsedTablePages = summary.Storage.UsedTablePages,
            .MappedPages = summary.MappedPages,
            .Reserved = 0U,
        };
        buildError = InitialPageTableBuildError::None;
    }

    AddressSpaceMappingRecord runtimeMapping = validMapping;
    runtimeMapping.Pages = PageCount{1U};
    RuntimeAddressSpacePageTable backend;
    RuntimeAddressSpacePageTable competingBackend;
    RuntimeAddressSpacePageTable incompatibleBackend;
    AddressSpace owner;
    AddressSpace competingOwner;
    Hal::Memory::AddressSpacePageTableMapResult genericOutput{
        .InvalidationStart = VirtualAddress{0xABCD'0000U},
        .InvalidationPages = PageCount{0x1234U},
        .Reserved = 0x55AAU,
    };
    const auto expectedGenericOutput = genericOutput;
    const auto unclaimedError = backend.Map(runtimeMapping, genericOutput);
    const bool unclaimedOutputUnchanged =
        (genericOutput.InvalidationStart.GetValue() == expectedGenericOutput.InvalidationStart.GetValue()) &&
        (genericOutput.InvalidationPages.GetValue() == expectedGenericOutput.InvalidationPages.GetValue()) &&
        (genericOutput.Reserved == expectedGenericOutput.Reserved);
    RuntimeAddressSpacePageTableInitializationError initializationError =
        RuntimeAddressSpacePageTableInitializationError::VerificationRejected;
    RuntimeAddressSpacePageTableInitializationError competingInitializationError =
        RuntimeAddressSpacePageTableInitializationError::VerificationRejected;
    RuntimeAddressSpacePageTableInitializationError incompatibleInitializationError =
        RuntimeAddressSpacePageTableInitializationError::VerificationRejected;
    if (preparationReady)
    {
        PagingCapabilities incompatibleCapabilities = bootstrapCapabilities;
        incompatibleCapabilities.GlobalPagesEnabled = false;
        incompatibleInitializationError =
            incompatibleBackend.Initialize(incompatibleCapabilities, *bootstrapPreparation, g_entries);
        std::thread first{[&]() noexcept
                          {
                              initializationError =
                                  backend.Initialize(bootstrapCapabilities, *bootstrapPreparation, g_entries);
                          }};
        std::thread second{[&]() noexcept
                           {
                               competingInitializationError =
                                   competingBackend.Initialize(bootstrapCapabilities, *bootstrapPreparation, g_entries);
                           }};
        first.join();
        second.join();
    }
    RuntimeAddressSpacePageTable* const activeBackend =
        (initializationError == RuntimeAddressSpacePageTableInitializationError::None)
            ? &backend
            : ((competingInitializationError == RuntimeAddressSpacePageTableInitializationError::None)
                   ? &competingBackend
                   : nullptr);
    Hal::Memory::IAddressSpacePageTable* const activationBackend = activeBackend;
    ResetNativeActivationLog();
    const auto readyActivationError = (activationBackend != nullptr) ? activationBackend->Activate(owner, CpuId{0U})
                                                                     : AddressSpacePageTableActivationError::NotReady;
    const auto wrongSeedError = (activeBackend != nullptr) ? activeBackend->Claim(owner, initialPlan)
                                                           : Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    const auto claimError = (activeBackend != nullptr) && preparedPlanResult.HasValue()
                                ? activeBackend->Claim(owner, *preparedPlanResult.GetValue())
                                : Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    const auto competingClaimError = (activeBackend != nullptr) && preparedPlanResult.HasValue()
                                         ? activeBackend->Claim(competingOwner, *preparedPlanResult.GetValue())
                                         : Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    bootstrapPreparation->~InitialPageTablePreparation();
    for (Foundation::Size index{0U}; index < sizeof(bootstrapPreparationStorage); ++index)
    {
        bootstrapPreparationStorage[index] = 0xA5U;
    }
    bool preparationStorageOverwritten{true};
    for (Foundation::Size index{0U}; index < sizeof(bootstrapPreparationStorage); ++index)
    {
        preparationStorageOverwritten = preparationStorageOverwritten && (bootstrapPreparationStorage[index] == 0xA5U);
    }
    const auto foreignActivationError = (activationBackend != nullptr)
                                            ? activationBackend->Activate(competingOwner, CpuId{0U})
                                            : AddressSpacePageTableActivationError::NotReady;
    const auto invalidProcessorActivationError =
        (activationBackend != nullptr)
            ? activationBackend->Activate(owner, CpuId{Kernel::Scheduler::MaximumProcessorCount})
            : AddressSpacePageTableActivationError::NotReady;
    constexpr Foundation::UInt64 incompatibleCr4Modes[]{
        Cr4PageGlobalEnable,
        Cr4LinearAddress57,
        Cr4ProcessContextIdentifierEnable,
    };
    bool incompatibleCr4ModesRejected{true};
    for (const Foundation::UInt64 incompatibleCr4 : incompatibleCr4Modes)
    {
        g_liveCr4 = Cr4PhysicalAddressExtension | incompatibleCr4;
        const auto incompatibleModeError = (activationBackend != nullptr)
                                               ? activationBackend->Activate(owner, CpuId{0U})
                                               : AddressSpacePageTableActivationError::NotReady;
        incompatibleCr4ModesRejected =
            incompatibleCr4ModesRejected &&
            (incompatibleModeError == AddressSpacePageTableActivationError::UnsupportedConfiguration) &&
            (g_cr3LoadCount == 0U);
    }
    const Foundation::Size incompatibleModeCr4ReadCount = g_cr4ReadCount;
    const auto incompatibleModeSnapshot = (activeBackend != nullptr)
                                              ? activeBackend->GetSnapshot()
                                              : Platform::Amd64::Memory::RuntimeAddressSpacePageTableSnapshot{};
    g_liveCr4 = Cr4PhysicalAddressExtension;
    const auto activationError = (activationBackend != nullptr) ? activationBackend->Activate(owner, CpuId{0U})
                                                                : AddressSpacePageTableActivationError::NotReady;
    const Foundation::Size cr3LoadCountAfterCompatibleMode = g_cr3LoadCount;
    const Foundation::Size cr4ReadCountAfterCompatibleMode = g_cr4ReadCount;
    const auto genericMapError = (activeBackend != nullptr) ? activeBackend->Map(runtimeMapping, genericOutput)
                                                            : Hal::Memory::AddressSpacePageTableError::NotReady;
    Hal::Memory::AddressSpacePageTableMapResult unmapOutput{};
    const auto genericUnmapError = (activeBackend != nullptr) ? activeBackend->Unmap(runtimeMapping, unmapOutput)
                                                              : Hal::Memory::AddressSpacePageTableError::NotReady;
    const auto unmappedSnapshot = (activeBackend != nullptr)
                                      ? activeBackend->GetSnapshot()
                                      : Platform::Amd64::Memory::RuntimeAddressSpacePageTableSnapshot{};
    const auto genericRemapError = (activeBackend != nullptr) ? activeBackend->Map(runtimeMapping, genericOutput)
                                                              : Hal::Memory::AddressSpacePageTableError::NotReady;
    const auto backendSnapshot = (activeBackend != nullptr)
                                     ? activeBackend->GetSnapshot()
                                     : Platform::Amd64::Memory::RuntimeAddressSpacePageTableSnapshot{};
    g_echoRequestedCr3 = false;
    g_forcedCr3Readback = TablePhysicalStart + 0x1000U;
    const auto uncertainActivationError = (activationBackend != nullptr)
                                              ? activationBackend->Activate(owner, CpuId{1U})
                                              : AddressSpacePageTableActivationError::NotReady;
    const auto faultedSnapshot = (activeBackend != nullptr)
                                     ? activeBackend->GetSnapshot()
                                     : Platform::Amd64::Memory::RuntimeAddressSpacePageTableSnapshot{};
    const auto faultedMapError = (activeBackend != nullptr) ? activeBackend->Map(runtimeMapping, genericOutput)
                                                            : Hal::Memory::AddressSpacePageTableError::NotReady;
    const auto releaseError = (activeBackend != nullptr) ? activeBackend->Release(owner)
                                                         : Hal::Memory::AddressSpacePageTableOwnershipError::NotReady;
    const bool exactOneInitialization =
        ((initializationError == RuntimeAddressSpacePageTableInitializationError::None) &&
         (competingInitializationError == RuntimeAddressSpacePageTableInitializationError::StorageAlreadyOwned)) ||
        ((competingInitializationError == RuntimeAddressSpacePageTableInitializationError::None) &&
         (initializationError == RuntimeAddressSpacePageTableInitializationError::StorageAlreadyOwned));
    ASTERIA_EXPECT(runner, "the bootstrap owner publishes one verified seed and storage summary", preparationPublished);
    ASTERIA_EXPECT(runner, "one competing backend completes verification under exclusive ownership",
                   (initializationError == RuntimeAddressSpacePageTableInitializationError::None) ||
                       (competingInitializationError == RuntimeAddressSpacePageTableInitializationError::None));
    ASTERIA_EXPECT(
        runner, "the losing backend cannot inspect or claim consumed storage",
        (initializationError == RuntimeAddressSpacePageTableInitializationError::StorageAlreadyOwned) ||
            (competingInitializationError == RuntimeAddressSpacePageTableInitializationError::StorageAlreadyOwned));
    ASTERIA_EXPECT(runner, "an unclaimed backend is non-writing",
                   unclaimedError == Hal::Memory::AddressSpacePageTableError::NotReady, unclaimedOutputUnchanged);
    ASTERIA_EXPECT(runner, "one preparation initializes exactly one competing backend", exactOneInitialization,
                   activeBackend != nullptr);
    ASTERIA_EXPECT(runner, "unsupported capabilities are rejected before preparation ownership is consumed",
                   incompatibleInitializationError ==
                       RuntimeAddressSpacePageTableInitializationError::InvalidCapabilities,
                   !incompatibleBackend.IsReady());
    ASTERIA_EXPECT(runner, "the initialized backend binds one exact AddressSpace and seed",
                   wrongSeedError == Hal::Memory::AddressSpacePageTableOwnershipError::SeedMismatch,
                   claimError == Hal::Memory::AddressSpacePageTableOwnershipError::None,
                   competingClaimError == Hal::Memory::AddressSpacePageTableOwnershipError::AlreadyOwned);
    ASTERIA_EXPECT(runner, "activation retains no borrowed seed storage after preparation destruction",
                   preparationStorageOverwritten, activationError == AddressSpacePageTableActivationError::None,
                   cr3LoadCountAfterCompatibleMode == 1U);
    ASTERIA_EXPECT(runner, "activation rejects live CR4.PGE, LA57, and PCIDE before any CR3 helper call",
                   incompatibleCr4ModesRejected, incompatibleModeCr4ReadCount == 3U,
                   incompatibleModeSnapshot.State == RuntimeAddressSpacePageTableState::Claimed,
                   cr4ReadCountAfterCompatibleMode == 4U);
    ASTERIA_EXPECT(runner, "activation rejects invalid state, owner, and CPU before loading the verified root",
                   readyActivationError == AddressSpacePageTableActivationError::NotReady,
                   foreignActivationError == AddressSpacePageTableActivationError::InvalidOwner,
                   invalidProcessorActivationError == AddressSpacePageTableActivationError::RejectedBeforeCommit,
                   activationError == AddressSpacePageTableActivationError::None, g_cr3LoadCount == 2U,
                   g_cr4ReadCount == 5U, g_lastRequestedCr3 == TablePhysicalStart);
    ASTERIA_EXPECT(runner,
                   "the claimed backend adopts one native map, unmaps and maps it again, and retires exactly once",
                   genericMapError == Hal::Memory::AddressSpacePageTableError::None,
                   genericUnmapError == Hal::Memory::AddressSpacePageTableError::None,
                   unmapOutput.InvalidationStart.GetValue() == runtimeMapping.VirtualStart.GetValue(),
                   unmapOutput.InvalidationPages.GetValue() == 1U,
                   unmappedSnapshot.State == RuntimeAddressSpacePageTableState::Claimed,
                   genericRemapError == Hal::Memory::AddressSpacePageTableError::None,
                   unmappedSnapshot.UsedTablePages == backendSnapshot.UsedTablePages,
                   genericOutput.InvalidationStart.GetValue() == runtimeMapping.VirtualStart.GetValue(),
                   genericOutput.InvalidationPages.GetValue() == 1U, genericOutput.Reserved == 0U,
                   backendSnapshot.State == RuntimeAddressSpacePageTableState::Claimed,
                   backendSnapshot.UsedTablePages >= buildResult.UsedTablePages,
                   releaseError == Hal::Memory::AddressSpacePageTableOwnershipError::None, activeBackend != nullptr,
                   !activeBackend->IsReady());
    ASTERIA_EXPECT(runner, "a post-write CR3 readback mismatch terminally faults the claimed root",
                   uncertainActivationError == AddressSpacePageTableActivationError::CommittedStateUnknown,
                   faultedSnapshot.State == RuntimeAddressSpacePageTableState::Faulted,
                   faultedMapError == Hal::Memory::AddressSpacePageTableError::CorruptState);

    ClearEntries();
    if (planReady)
    {
        buildError = InitialPageTableBuilder::Build(initialPlan, bootstrapCapabilities, bootstrapStorage, buildResult);
    }

    RuntimePageTableMapResult runtimeResult{};
    RuntimePageTableMapError runtimeError = RuntimePageTableMapError::InvalidStorage;
    if (buildError == InitialPageTableBuildError::None)
    {
        runtimeError = RuntimePageTableMapper::Map(runtimeMapping, MakeCapabilities(),
                                                   MakeStorage(buildResult.UsedTablePages), runtimeResult);
    }

    bool exact = planReady && (buildError == InitialPageTableBuildError::None) &&
                 (runtimeError == RuntimePageTableMapError::None) && (runtimeResult.UsedTablePages <= TablePageCount) &&
                 ((Entry(0U, 0U) & UserBit) != 0U);
    Foundation::Size pdptPage{0U};
    Foundation::Size directoryPage{0U};
    Foundation::Size codeLeafPage{0U};
    Foundation::Size stackLeafPage{0U};
    Foundation::Size runtimeLeafPage{0U};
    if (exact)
    {
        pdptPage = static_cast<Foundation::Size>(((Entry(0U, 0U) & PhysicalAddressMask) - TablePhysicalStart) >> 12U);
        exact = pdptPage < runtimeResult.UsedTablePages;
    }
    if (exact)
    {
        directoryPage =
            static_cast<Foundation::Size>(((Entry(pdptPage, 0U) & PhysicalAddressMask) - TablePhysicalStart) >> 12U);
        exact = (directoryPage < runtimeResult.UsedTablePages) && ((Entry(pdptPage, 0U) & UserBit) != 0U);
    }
    if (exact)
    {
        codeLeafPage = static_cast<Foundation::Size>(
            ((Entry(directoryPage, 0U) & PhysicalAddressMask) - TablePhysicalStart) >> 12U);
        stackLeafPage = static_cast<Foundation::Size>(
            ((Entry(directoryPage, 1U) & PhysicalAddressMask) - TablePhysicalStart) >> 12U);
        runtimeLeafPage = static_cast<Foundation::Size>(
            ((Entry(directoryPage, 2U) & PhysicalAddressMask) - TablePhysicalStart) >> 12U);
        exact = (codeLeafPage < runtimeResult.UsedTablePages) && (stackLeafPage < runtimeResult.UsedTablePages) &&
                (runtimeLeafPage < runtimeResult.UsedTablePages) && ((Entry(directoryPage, 0U) & UserBit) == 0U) &&
                ((Entry(directoryPage, 1U) & UserBit) == 0U) && ((Entry(directoryPage, 2U) & UserBit) != 0U) &&
                ((Entry(codeLeafPage, 256U) & UserBit) == 0U) && ((Entry(stackLeafPage, 256U) & UserBit) == 0U) &&
                ((Entry(runtimeLeafPage, 0U) & UserBit) != 0U);
    }
    ASTERIA_EXPECT(runner, "the verified bootstrap root accepts a user leaf without exposing nucleus leaves", exact);
}

[[nodiscard]] ObjectId RegisterObject(RootCapabilityIssuer& issuer, IKernelObject& object, TestContext& runner,
                                      const char* const description) noexcept
{
    const auto result = issuer.CreateObject(object);
    ASTERIA_EXPECT(runner, description, result.HasValue());
    return result.HasValue() ? result.GetValue() : ObjectId{};
}

void RetireObject(RootCapabilityIssuer& issuer, const ObjectId object, TestContext& runner,
                  const char* const description) noexcept
{
    const ObjectRegistryError destroyError = issuer.DestroyObject(object);
    const auto collected = issuer.CollectDestroyedObject();
    ASTERIA_EXPECT(runner, description, destroyError == ObjectRegistryError::Ok, collected.HasValue(),
                   collected.GetValue().Object.GetValue() == object.GetValue());
}

// Desktop session: full 256-page storage, exact capacity refusal, and fixed-slot reuse without
// allocating another intermediate table.
void TestLargeCapacity(TestContext& runner, const AddressSpaceMappingRecord& validMapping) noexcept
{
    alignas(4096) static Foundation::UInt64 entries[256U * EntriesPerPage]{};
    RuntimePageTableStorage storage{PhysicalAddress{TablePhysicalStart}, entries, 256U, 1U, 0U};
    auto mapping = validMapping;
    RuntimePageTableMapResult output{};
    bool mapped{true};
    for (Foundation::Size table{0U}; table < 253U; ++table)
    {
        mapping.VirtualStart = VirtualAddress{0xC0000000ULL + table * 0x200000U};
        mapping.PhysicalStart = PhysicalAddress{0x02000000ULL + table * 4096U};
        mapping.Pages = PageCount{1U};
        const auto result = RuntimePageTableMapper::Map(mapping, MakeCapabilities(), storage, output);
        mapped = mapped && result == RuntimePageTableMapError::None;
        if (result == RuntimePageTableMapError::None)
        {
            storage.UsedPageCount = output.UsedTablePages;
        }
    }
    mapping.VirtualStart = VirtualAddress{0xC0000000ULL + 253U * 0x200000U};
    const auto exhausted = RuntimePageTableMapper::Map(mapping, MakeCapabilities(), storage, output);
    bool reused{true};
    for (Foundation::Size table{0U}; table < 253U; ++table)
    {
        mapping.VirtualStart = VirtualAddress{0xC0000000ULL + table * 0x200000U};
        mapping.PhysicalStart = PhysicalAddress{0x02000000ULL + table * 4096U};
        reused = reused && RuntimePageTableMapper::Unmap(mapping, MakeCapabilities(), storage, output) ==
                               RuntimePageTableMapError::None;
        reused = reused &&
                 RuntimePageTableMapper::Map(mapping, MakeCapabilities(), storage, output) ==
                     RuntimePageTableMapError::None &&
                 output.UsedTablePages == 256U;
    }
    ASTERIA_EXPECT(runner, "large mapper fills 256 pages and reuses them after unmap", mapped,
                   storage.UsedPageCount == 256U, exhausted == RuntimePageTableMapError::InsufficientTableStorage,
                   reused);
    storage.PageCapacity = 255U;
    ASTERIA_EXPECT(runner, "capacity between the ordinary and large classes is invalid",
                   RuntimePageTableMapper::Map(mapping, MakeCapabilities(), storage, output) ==
                       RuntimePageTableMapError::InvalidStorage);
}
void RunMapperTests(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    TestKernelObject thread{ObjectType::Thread, ResourceDomainId{1U}};
    TestKernelObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{2U}};
    BootFrame frame;
    ResourceDomainLedger domainLedger;
    ResourceDomain domain{domainLedger};
    MemoryObject memoryObject;
    const ResourceDomainLimits domainLimits{
        .CommittedMemoryPages = 2048U,
        .ResidentMemoryPages = 2048U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = 64U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
    const bool domainReady =
        domain.InitializeRoot(ResourceDomainId{5U}, domainLimits).Error == ResourceDomainError::None;
    const ObjectId domainObject =
        domainReady ? RegisterObject(issuer, domain, runner, "runtime mapper fixture registers its resource domain")
                    : ObjectId{};
    const bool memoryReady =
        domainObject.IsValid() && (memoryObject
                                       .Initialize(registry, ResourceDomainObjectId{registry, domainObject}, domain,
                                                   MemoryObjectDescriptor{
                                                       .Pages = PageCount{1024U},
                                                       .CachePolicy = MemoryCachePolicy::WriteBack,
                                                       .MaximumPermissions = ReadWriteUser(),
                                                       .Commitment = MemoryCommitment::BestEffort,
                                                       .Reserved = {},
                                                   })
                                       .Error == MemoryObjectError::None);
    const ObjectId memoryObjectId =
        memoryReady ? RegisterObject(issuer, memoryObject, runner, "runtime mapper fixture registers its MemoryObject")
                    : ObjectId{};
    ASTERIA_EXPECT(runner, "runtime mapper fixture initializes a RAM Frame",
                   frame.Initialize(ResourceDomainId{3U}, FrameDescriptor{
                                                              .PhysicalStart = PhysicalAddress{0x0020'0000U},
                                                              .Pages = PageCount{1024U},
                                                              .CachePolicy = MemoryCachePolicy::WriteBack,
                                                              .Kind = FrameKind::Ram,
                                                              .Reserved = {},
                                                          }) == FrameError::None);
    const ObjectId threadObject = RegisterObject(issuer, thread, runner, "runtime mapper fixture registers its Thread");
    const ObjectId addressSpaceObject =
        RegisterObject(issuer, addressSpace, runner, "runtime mapper fixture registers its AddressSpace");
    const ObjectId frameObject = RegisterObject(issuer, frame, runner, "runtime mapper fixture registers its Frame");
    if (!threadObject.IsValid() || !addressSpaceObject.IsValid() || !frameObject.IsValid() || !memoryObjectId.IsValid())
    {
        return;
    }

    {
        FaultToken faultToken;
        const FaultRecord fault{
            .VirtualAddress = 0x0040'1234U,
            .InstructionPointer = 0x0080'0000U,
            .ArchitectureSyndrome = 0xA55AU,
            .Access = FaultAccess::Write,
            .Cause = FaultCause::MissingMapping,
            .Privilege = FaultPrivilege::User,
            .Reserved = {},
        };
        ASTERIA_EXPECT(runner, "runtime mapper fixture publishes its fault",
                       faultToken.Initialize(registry, ResourceDomainId{4U}, ThreadId{registry, threadObject},
                                             AddressSpaceId{registry, addressSpaceObject},
                                             fault) == FaultTokenError::None);
        const auto claim = faultToken.ClaimResolution();
        ASTERIA_EXPECT(runner, "runtime mapper fixture claims its fault", claim.HasValue());
        if (!claim.HasValue())
        {
            return;
        }

        FaultFrameMappingPreparation preparation;
        const FaultFrameMappingRequest request{
            .VirtualStart = VirtualAddress{0x0040'0000U},
            .Pages = PageCount{2U},
            .MemoryObjectOffsetPages = 0U,
            .FrameOffsetPages = 0U,
            .Permissions = ReadWriteUser(),
            .Reserved = {},
        };
        ASTERIA_EXPECT(runner, "runtime mapper fixture prepares authoritative mapping inputs",
                       preparation.Prepare(registry, faultToken, claim.GetValue().Token,
                                           MemoryObjectId{registry, memoryObjectId}, memoryObject, {}, nullptr,
                                           FrameId{registry, frameObject}, frame,
                                           request) == Kernel::Memory::FaultFrameMappingError::None);
        if (!preparation.IsPrepared())
        {
            return;
        }

        const auto& preparedPlan = preparation.GetPlan();
        const AddressSpaceMappingRecord validMapping{
            .MemoryObject = preparedPlan.MemoryObject,
            .DeviceRegion = preparedPlan.DeviceRegion,
            .Frame = preparedPlan.Frame,
            .VirtualStart = preparedPlan.VirtualStart,
            .PhysicalStart = preparedPlan.PhysicalStart,
            .Pages = preparedPlan.Pages,
            .MemoryObjectOffsetPages = preparedPlan.MemoryObjectOffsetPages,
            .Permissions = preparedPlan.Permissions,
            .CachePolicy = preparedPlan.CachePolicy,
            .Kind = preparedPlan.Kind,
            .Commitment = preparedPlan.Commitment,
            .RuntimeMapping = true,
            .Reserved = {},
        };
        TestLargeCapacity(runner, validMapping);
        TestBasicAndIncrementalMapping(runner, validMapping);
        TestUnmap(runner, validMapping);
        TestBoundariesAndEncoding(runner, validMapping);
        TestScanoutDeviceLeaves(runner, validMapping);
        TestFailureAtomicity(runner, validMapping);
        TestVerifiedBootstrapExtension(runner, validMapping);
        TestRuntimeLeavesAreNotGlobal(runner, validMapping);
        ASTERIA_EXPECT(runner, "the fixture commits its fault only after mapping tests complete",
                       faultToken.CommitResolution(claim.GetValue().Token, FaultResolutionKind::Map) ==
                           FaultTokenError::None);
    }

    RetireObject(issuer, frameObject, runner, "runtime mapper fixture retires its Frame");
    ASTERIA_EXPECT(runner, "runtime mapper fixture quiesces its MemoryObject",
                   memoryObject.Quiesce().Error == MemoryObjectError::None);
    RetireObject(issuer, memoryObjectId, runner, "runtime mapper fixture retires its MemoryObject");
    RetireObject(issuer, addressSpaceObject, runner, "runtime mapper fixture retires its AddressSpace");
    RetireObject(issuer, threadObject, runner, "runtime mapper fixture retires its Thread");
    ASTERIA_EXPECT(runner, "runtime mapper fixture quiesces its resource domain",
                   domain.Quiesce() == ResourceDomainError::None);
    RetireObject(issuer, domainObject, runner, "runtime mapper fixture retires its resource domain");
}
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"RuntimePageTableMapperTests"};
    Asteria::Test::RunMapperTests(runner);
    return runner.Finish();
}
