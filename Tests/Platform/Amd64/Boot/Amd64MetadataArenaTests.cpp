// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <memory>

#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Platform/Amd64/Boot/Amd64MetadataArena.hpp"
#include "Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp"
#include "Platform/Amd64/Memory/InitialPageTablePreparation.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Platform::Amd64::Bootstrap::AddMetadataArenaMappings;
using Platform::Amd64::Bootstrap::GetMetadataArenaPages;
using Platform::Amd64::Bootstrap::MetadataArenaAlignmentPages;
using Platform::Amd64::Bootstrap::MetadataArenaBitmapBytesPerGiB;
using Platform::Amd64::Bootstrap::MetadataArenaBitmapFirstPage;
using Platform::Amd64::Bootstrap::MetadataArenaMaximumPages;
using Platform::Amd64::Bootstrap::MetadataArenaMaximumSpanGiB;
using Platform::Amd64::Bootstrap::MetadataArenaReceiptError;
using Platform::Amd64::Bootstrap::MetadataArenaTablePages;
using Platform::Amd64::Bootstrap::RootArenaPages;
using Platform::Amd64::Bootstrap::ValidateMetadataArenaReceipt;
using Platform::Amd64::Memory::InitialPageTablePreparation;
using Platform::Amd64::Memory::PageTableStorage;
using Platform::Amd64::Memory::PagingCapabilities;

constexpr UInt64 PageBytes{4096U};
constexpr UInt64 ArenaBlockBytes{MetadataArenaAlignmentPages * PageBytes};
constexpr UInt64 UserRangesStart{0x4000'0000U};
constexpr UInt64 ArenaStart{0x1C00'0000U};
constexpr UInt64 StartupPhysical{0x8000U};
// One root's 64-page table arena, in the first 2 MiB block as no arena is.
constexpr UInt64 StoragePhysical{0x0010'0000U};

alignas(4096) UInt64 g_storage[RootArenaPages * 512U]{};
Boot::BootInfo g_info{};
Boot::BootInfo g_before{};

// The query (KernelQuery::MetadataArenaPages): whole 2 MiB blocks that hold
// the slabs and the span's bitmaps, never above the cap, for any index.
void TestQuery(TestContext& runner) noexcept
{
    bool shaped{true};
    bool covering{true};
    UInt64 previous{0U};
    for (UInt64 span{0U}; span <= MetadataArenaMaximumSpanGiB + 64U; ++span)
    {
        const UInt64 pages = GetMetadataArenaPages(span);
        shaped = shaped && ((pages % MetadataArenaAlignmentPages) == 0U) && (pages >= MetadataArenaBitmapFirstPage) &&
                 (pages <= MetadataArenaMaximumPages) && (pages >= previous);
        // Within the span the cap answers for, the bitmaps fit.
        covering = covering &&
                   ((span > MetadataArenaMaximumSpanGiB) ||
                    (((pages - MetadataArenaBitmapFirstPage) * PageBytes) >= span * MetadataArenaBitmapBytesPerGiB));
        previous = pages;
    }
    ASTERIA_EXPECT(runner, "the query answers whole 2 MiB blocks under the cap that hold the slabs and bitmaps", shaped,
                   covering, GetMetadataArenaPages(0U) < MetadataArenaMaximumPages,
                   GetMetadataArenaPages(0xFFFF'FFFFU) == MetadataArenaMaximumPages,
                   GetMetadataArenaPages(~UInt64{0U}) == MetadataArenaMaximumPages,
                   MetadataArenaMaximumPages == MetadataArenaTablePages * MetadataArenaAlignmentPages);
}

[[nodiscard]] constexpr PagingCapabilities FullCapabilities() noexcept
{
    return PagingCapabilities{
        .PhysicalAddressBits = 48U,
        .FourLevelPagingEnabled = true,
        .ExecuteDisableEnabled = true,
        .SupervisorWriteProtectEnabled = true,
        .GlobalPagesEnabled = true,
        .PatLayoutConfigured = true,
        .Reserved16 = 0U,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool AddIdentity(InitialMappingPlan& plan, const UInt64 start, const UInt64 pages,
                               const MappingPermissions permissions, const InitialMappingOwner owner) noexcept
{
    return plan.AddMapping(InitialMapping{
               .VirtualStart = VirtualAddress{start},
               .PhysicalStart = PhysicalAddress{start},
               .Pages = PageCount{pages},
               .Permissions = permissions,
               .CachePolicy = MemoryCachePolicy::WriteBack,
               .Owner = owner,
               .Reserved = 0U,
           }) == InitialMappingPlanError::None;
}

// The table pages one root's seed takes: the startup pages (which need the
// First GiB's page directory in every seed), the root's own table arena, a kernel stack (a sealed
// plan has one), and the arena of arenaPages pages at arenaStart when arenaPages is nonzero. Zero
// when the seed does not prepare.
[[nodiscard]] Size SeedTablePages(const UInt64 arenaStart, const UInt64 arenaPages) noexcept
{
    const auto plan = std::make_unique<InitialMappingPlan>();
    const auto preparation = std::make_unique<InitialPageTablePreparation>();
    const MappingPermissions readWrite = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
    bool built =
        AddIdentity(*plan, StartupPhysical, 1U,
                    MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
                    InitialMappingOwner::NucleusCode) &&
        AddIdentity(*plan, StartupPhysical + PageBytes, 1U, readWrite, InitialMappingOwner::NucleusWritableData) &&
        AddIdentity(*plan, StoragePhysical, RootArenaPages, readWrite, InitialMappingOwner::PageTable) &&
        AddIdentity(*plan, StoragePhysical + ((RootArenaPages + 1U) * PageBytes), 4U, readWrite,
                    InitialMappingOwner::NucleusStack);
    if (built && (arenaPages != 0U))
    {
        built = AddMetadataArenaMappings(*plan, arenaStart, arenaPages);
    }
    if (!built || (plan->Seal() != InitialMappingPlanError::None))
    {
        return 0U;
    }
    const PageTableStorage storage{
        .PhysicalStart = PhysicalAddress{StoragePhysical},
        .Entries = g_storage,
        .PageCapacity = RootArenaPages,
        .Reserved = 0U,
    };
    if (!preparation->Prepare(*plan, FullCapabilities(), storage).HasValue())
    {
        return 0U;
    }
    const auto summary = preparation->TryGetSummary();
    return summary.HasValue() ? summary.GetValue()->Storage.UsedTablePages : 0U;
}

// The arena's 4 KiB seed leaves cost one page table per 2 MiB block beyond
// the seed's other tables, whatever 2 MiB-aligned place below 1 GiB the loader
// picked, so the cap's blocks are the MetadataArenaTablePages the layout
// keeps spare in each root's arena. The rest of the proof is the boot's: the
// AMD64 evidence reports the shared root's seed tables with the arena, which
// the runner checks against the 64-page arena with the arena at the cap.
void TestSeedTableBudget(TestContext& runner) noexcept
{
    const Size base = SeedTablePages(0U, 0U);
    constexpr UInt64 Starts[]{ArenaBlockBytes, ArenaStart, UserRangesStart - (MetadataArenaMaximumPages * PageBytes)};
    const UInt64 sizes[]{GetMetadataArenaPages(0U), GetMetadataArenaPages(16U), MetadataArenaMaximumPages};
    bool exact{base != 0U};
    for (const UInt64 start : Starts)
    {
        for (const UInt64 pages : sizes)
        {
            const Size used = SeedTablePages(start, pages);
            if (used != base + (pages / MetadataArenaAlignmentPages))
            {
                std::fprintf(stderr, "  arena %#llx of %llu pages: %zu table pages over a base of %zu\n",
                             static_cast<unsigned long long>(start), static_cast<unsigned long long>(pages), used,
                             base);
                exact = false;
            }
        }
    }
    ASTERIA_EXPECT(runner, "the arena's seed leaves take one table page per 2 MiB, at most the layout's spare count",
                   exact, SeedTablePages(ArenaStart, MetadataArenaMaximumPages) == base + MetadataArenaTablePages,
                   base + MetadataArenaTablePages <= RootArenaPages);
    ASTERIA_EXPECT(runner, "the arena's mappings refuse a size without slabs or above the cap",
                   SeedTablePages(ArenaStart, MetadataArenaAlignmentPages / 2U) == 0U,
                   SeedTablePages(ArenaStart, MetadataArenaMaximumPages + MetadataArenaAlignmentPages) == 0U);
}

constexpr UInt64 WriteBack{8U};
constexpr UInt64 Runtime{UInt64{1U} << 63U};

// A boot whose final map has one LoaderData region of exactly the arena, and
// whose other receipts lie elsewhere.
void ResetInfo(const UInt64 arenaPages = 2048U) noexcept
{
    g_info = {};
    g_info.MemoryRegionCount = 1U;
    g_info.MemoryRegions[0U] = {ArenaStart, 0U, arenaPages, WriteBack, 2U, Boot::PhysicalMemoryKind::Loader};
    g_info.Kernel.MetadataArena = {ArenaStart, arenaPages, 0U, 0U, 0U};
    g_info.Kernel.Image.PhysicalStart = 0x0100'0000U;
    g_info.Kernel.Image.SizeBytes = 0x0040'0000U;
    g_info.Kernel.Bootstrap = {0x0150'0000U, 32U, 0U, 0U, 0U};
    g_info.Kernel.RuntimePool = {0x0200'0000U, 4361U, 0U, 0U, 0U};
    g_info.Kernel.Startup = {StartupPhysical, 2U, 0U, 0U, 0U};
    g_info.Files.PhysicalStart = 0x0400'0000U;
    g_info.Files.PageCount = 64U;
    g_info.Inventory.PhysicalStart = 0x0500'0000U;
    g_info.Inventory.Pages = 4U;
}

[[nodiscard]] MetadataArenaReceiptError Validate() noexcept
{
    return ValidateMetadataArenaReceipt(g_info);
}

// The receipt: 2 MiB blocks of LoaderData that one write-back,
// non-runtime region of the final map covers, below the fixed user ranges,
// overlapping no other receipt.
void TestReceipt(TestContext& runner) noexcept
{
    ResetInfo();
    const MetadataArenaReceiptError valid = Validate();
    g_info.Kernel.MetadataArena.PhysicalStart = 0U;
    const MetadataArenaReceiptError absent = Validate();
    ResetInfo();
    g_info.Kernel.MetadataArena.PhysicalStart += PageBytes;
    g_info.MemoryRegions[0U].PhysicalStart += PageBytes;
    const MetadataArenaReceiptError misaligned = Validate();
    ResetInfo();
    g_info.Kernel.MetadataArena.PageCount = 2048U + 1U;
    const MetadataArenaReceiptError partial = Validate();
    ResetInfo();
    g_info.Kernel.MetadataArena.Reserved = 1U;
    const MetadataArenaReceiptError reserved = Validate();
    ASTERIA_EXPECT(runner, "the receipt is whole, aligned 2 MiB blocks with a zero reserved word",
                   valid == MetadataArenaReceiptError::None, absent == MetadataArenaReceiptError::InvalidExtent,
                   misaligned == MetadataArenaReceiptError::InvalidExtent,
                   partial == MetadataArenaReceiptError::InvalidExtent,
                   reserved == MetadataArenaReceiptError::InvalidExtent);

    ResetInfo(MetadataArenaAlignmentPages);
    const MetadataArenaReceiptError small = Validate();
    ResetInfo(MetadataArenaMaximumPages + MetadataArenaAlignmentPages);
    const MetadataArenaReceiptError large = Validate();
    ResetInfo(MetadataArenaMaximumPages);
    const MetadataArenaReceiptError largest = Validate();
    ASTERIA_EXPECT(runner, "the receipt holds the slabs and stays under the cap",
                   small == MetadataArenaReceiptError::InvalidSize, large == MetadataArenaReceiptError::InvalidSize,
                   largest == MetadataArenaReceiptError::None);

    ResetInfo();
    g_info.Kernel.MetadataArena.PhysicalStart = UserRangesStart;
    g_info.MemoryRegions[0U].PhysicalStart = UserRangesStart;
    const MetadataArenaReceiptError atUser = Validate();
    ResetInfo();
    g_info.Kernel.MetadataArena.PhysicalStart = UserRangesStart - ArenaBlockBytes;
    g_info.MemoryRegions[0U].PhysicalStart = UserRangesStart - ArenaBlockBytes;
    const MetadataArenaReceiptError acrossUser = Validate();
    ResetInfo();
    g_info.Kernel.MetadataArena.PhysicalStart = UserRangesStart - (2048U * PageBytes);
    g_info.MemoryRegions[0U].PhysicalStart = UserRangesStart - (2048U * PageBytes);
    const MetadataArenaReceiptError belowUser = Validate();
    ASTERIA_EXPECT(runner, "the receipt ends at or below the first fixed user range",
                   atUser == MetadataArenaReceiptError::UserRange, acrossUser == MetadataArenaReceiptError::UserRange,
                   belowUser == MetadataArenaReceiptError::None);

    ResetInfo();
    g_info.MemoryRegions[0U].FirmwareType = 4U;
    const MetadataArenaReceiptError bootServices = Validate();
    ResetInfo();
    g_info.MemoryRegions[0U].Kind = Boot::PhysicalMemoryKind::Conventional;
    const MetadataArenaReceiptError conventional = Validate();
    ResetInfo();
    g_info.MemoryRegions[0U].Attributes = 0U;
    const MetadataArenaReceiptError uncached = Validate();
    ResetInfo();
    g_info.MemoryRegions[0U].Attributes |= Runtime;
    const MetadataArenaReceiptError runtime = Validate();
    ResetInfo();
    g_info.MemoryRegions[0U].PageCount -= 1U;
    const MetadataArenaReceiptError truncated = Validate();
    ResetInfo();
    g_info.MemoryRegions[0U].PageCount = 1024U;
    g_info.MemoryRegionCount = 2U;
    g_info.MemoryRegions[1U] = g_info.MemoryRegions[0U];
    g_info.MemoryRegions[1U].PhysicalStart += 1024U * PageBytes;
    const MetadataArenaReceiptError split = Validate();
    ResetInfo();
    g_info.MemoryRegionCount = 0U;
    const MetadataArenaReceiptError unmapped = Validate();
    ResetInfo();
    g_info.MemoryRegions[0U].PhysicalStart -= ArenaBlockBytes;
    g_info.MemoryRegions[0U].PageCount += 2U * MetadataArenaAlignmentPages;
    const MetadataArenaReceiptError within = Validate();
    ASTERIA_EXPECT(runner, "one write-back, non-runtime LoaderData region of the final map covers the receipt",
                   bootServices == MetadataArenaReceiptError::InvalidProvenance,
                   conventional == MetadataArenaReceiptError::InvalidProvenance,
                   uncached == MetadataArenaReceiptError::InvalidProvenance,
                   runtime == MetadataArenaReceiptError::InvalidProvenance,
                   truncated == MetadataArenaReceiptError::InvalidProvenance,
                   split == MetadataArenaReceiptError::InvalidProvenance,
                   unmapped == MetadataArenaReceiptError::InvalidProvenance, within == MetadataArenaReceiptError::None);

    const auto overlapping = [](const auto& change) noexcept
    {
        ResetInfo();
        change();
        return Validate() == MetadataArenaReceiptError::Overlap;
    };
    ASTERIA_EXPECT(runner, "the receipt overlaps no other receipt",
                   overlapping(
                       []
                       {
                           g_info.Kernel.Image.PhysicalStart = ArenaStart + (2047U * PageBytes);
                       }),
                   overlapping(
                       []
                       {
                           g_info.Kernel.Bootstrap.PhysicalStart = ArenaStart - PageBytes;
                       }),
                   overlapping(
                       []
                       {
                           g_info.Kernel.RuntimePool.PhysicalStart = ArenaStart + ArenaBlockBytes;
                       }),
                   overlapping(
                       []
                       {
                           g_info.Kernel.Startup.PhysicalStart = ArenaStart;
                       }),
                   overlapping(
                       []
                       {
                           g_info.Kernel.Ia64Runtime = {ArenaStart, 1U, 0U, 0U, 0U};
                       }),
                   overlapping(
                       []
                       {
                           g_info.Files.PhysicalStart = ArenaStart + PageBytes;
                       }),
                   overlapping(
                       []
                       {
                           g_info.Inventory.PhysicalStart = ArenaStart + (100U * PageBytes);
                       }),
                   overlapping(
                       []
                       {
                           g_info.Kernel.RuntimePool.PageCount = ~UInt64{0U};
                       }));
    // Validation reads only its argument.
    ResetInfo();
    g_before = g_info;
    static_cast<void>(Validate());
    ASTERIA_EXPECT(runner, "validation changes nothing", __builtin_memcmp(&g_before, &g_info, sizeof(g_info)) == 0);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64MetadataArenaTests"};
    Asteria::Test::TestQuery(runner);
    Asteria::Test::TestSeedTableBudget(runner);
    Asteria::Test::TestReceipt(runner);
    return runner.Finish();
}
