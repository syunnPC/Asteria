// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Kernel/Memory/PhysicalPageAllocator.hpp"
#include "Kernel/Memory/PhysicalPageImporter.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace
{
namespace Boot = Asteria::Boot;
namespace Foundation = Asteria::Foundation;
namespace Kernel = Asteria::Kernel;
namespace TestSupport = Asteria::TestSupport;
using Asteria::Kernel::Memory::MappingPageSizeBytes;
using Asteria::Kernel::Memory::PhysicalAddress;
using Asteria::Kernel::Memory::PhysicalPageAllocator;
using Asteria::Kernel::Memory::PhysicalPageAllocatorError;
using Asteria::Kernel::Memory::PhysicalPageImportRange;
using Asteria::Kernel::Memory::PhysicalPageImporter;
using Asteria::Kernel::Memory::PhysicalPageRun;
using Foundation::Size;
using Foundation::UInt64;
using Error = PhysicalPageAllocatorError;
constexpr Size Pages{PhysicalPageAllocator::MaximumManagedPages};
constexpr UInt64 PageBytes{MappingPageSizeBytes};
struct Page
{
    bool Free;
    bool Dirty;
    bool Claimed;
    bool Retired;
};

// Deliberately no buddy bitmap: derive maximal aligned blocks from page
// states, then predict the exact selected block and split direction.
class Model final
{
public:
    explicit Model(const UInt64 base, const bool dirty)
        : Base{base}, States(Pages, Page{true, dirty, false, false}), Bytes(Pages * PageBytes, dirty ? 0xA5U : 0U)
    {
    }
    [[nodiscard]] bool Free(const UInt64 first, const UInt64 count) const
    {
        if ((first < Base) || ((first + count) > (Base + Pages)))
        {
            return false;
        }
        for (UInt64 page{first}; page < (first + count); ++page)
        {
            if (!States[static_cast<Size>(page - Base)].Free)
            {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool Clean(const UInt64 first, const UInt64 count) const
    {
        for (UInt64 page{first}; page < (first + count); ++page)
        {
            if (States[static_cast<Size>(page - Base)].Dirty)
            {
                return false;
            }
        }
        return true;
    }
    [[nodiscard]] bool Block(const UInt64 first, const Size order) const
    {
        const UInt64 length = UInt64{1U} << order;
        return Free(first, length) &&
               ((order == PhysicalPageAllocator::MaximumOrder) || !Free(first & ~(2U * length - 1U), 2U * length));
    }
    [[nodiscard]] UInt64 Predict(const UInt64 pages, const Size alignment, const bool low, const bool cleanOnly) const
    {
        Size required{alignment};
        while ((UInt64{1U} << required) < pages)
        {
            ++required;
        }
        for (Size order{required}; order <= PhysicalPageAllocator::MaximumOrder; ++order)
        {
            const UInt64 length = UInt64{1U} << order;
            UInt64 chosen{0U};
            Size candidates{0U};
            for (UInt64 first{Base & ~(length - 1U)}; first < (Base + Pages); first += length)
            {
                if (!Block(first, order) || (low && ((first + pages) > ((UInt64{1U} << 32U) / PageBytes))))
                {
                    continue;
                }
                UInt64 selected{first};
                if (cleanOnly)
                {
                    while (!Clean(selected, pages) && ((selected + (UInt64{1U} << required)) < (first + length)))
                    {
                        selected += UInt64{1U} << required;
                    }
                    if (!Clean(selected, pages))
                    {
                        continue;
                    }
                }
                if (chosen == 0U)
                {
                    chosen = selected;
                }
                if (Clean(selected, pages))
                {
                    return selected;
                }
                if (++candidates == 4U)
                {
                    break;
                }
            }
            if (chosen != 0U)
            {
                return chosen;
            }
        }
        return 0U;
    }
    void Zero(PhysicalPageRun& run)
    {
        const Size first = static_cast<Size>(run.PhysicalStart.GetValue() / PageBytes - Base);
        for (Size page{0U}; page < run.Pages; ++page)
        {
            if ((run.DirtyMask[page / 64U] & (UInt64{1U} << (page % 64U))) != 0U)
            {
                std::memset(&Bytes[(first + page) * PageBytes], 0, PageBytes);
            }
        }
        for (auto& word : run.DirtyMask)
        {
            word = 0U;
        }
    }
    [[nodiscard]] bool NewOwner(const PhysicalPageRun& run)
    {
        const Size first = static_cast<Size>(run.PhysicalStart.GetValue() / PageBytes - Base);
        bool zero{true};
        for (Size byte{first * PageBytes}; byte < ((first + run.Pages) * PageBytes); ++byte)
        {
            zero = zero && (Bytes[byte] == 0U);
            Bytes[byte] = 0x5AU;
        }
        return zero;
    }
    void Return(const PhysicalPageRun& run, const bool dirty)
    {
        const Size first = static_cast<Size>(run.PhysicalStart.GetValue() / PageBytes - Base);
        for (Size page{first}; page < (first + run.Pages); ++page)
        {
            States[page] = Page{true, dirty, false, false};
        }
    }
    UInt64 Base;
    std::vector<Page> States;
    std::vector<unsigned char> Bytes;
};

[[nodiscard]] bool RunSeed(const UInt64 seed)
{
    TestSupport::Random random{seed};
    const UInt64 base = (seed % 2U == 0U ? UInt64{1024U} : ((UInt64{1U} << 32U) / PageBytes)) + random.Below(512U);
    const bool initialDirty = random.Chance(1U, 2U);
    Model model{base, initialDirty};
    PhysicalPageAllocator allocator;
    if (allocator.Initialize(PhysicalAddress{base * PageBytes}, Pages, initialDirty) != Error::None)
    {
        return false;
    }
    std::vector<PhysicalPageRun> held;
    for (Size step{0U}; step < 1500U; ++step)
    {
        const UInt64 operation = random.Below(4U);
        if (operation == 0U)
        {
            const UInt64 pages = 1U + random.Below(512U);
            const auto alignment = static_cast<Foundation::UInt8>(random.Below(10U));
            const bool low = random.Chance(1U, 2U);
            const bool clean = random.Chance(1U, 2U);
            const UInt64 predicted = model.Predict(pages, alignment, low, clean);
            const auto allocated = allocator.Allocate(pages, alignment, low ? 32U : 0U, clean);
            if (allocated.HasValue() != (predicted != 0U))
            {
                return false;
            }
            if (allocated.HasValue())
            {
                PhysicalPageRun run = allocated.GetValue();
                if (run.PhysicalStart.GetValue() != predicted * PageBytes)
                {
                    return false;
                }
                const Size first = static_cast<Size>(predicted - base);
                for (Size page{0U}; page < pages; ++page)
                {
                    if (((run.DirtyMask[page / 64U] & (UInt64{1U} << (page % 64U))) != 0U) !=
                        model.States[first + page].Dirty)
                    {
                        return false;
                    }
                    model.States[first + page].Free = false;
                }
                model.Zero(run);
                if (!model.NewOwner(run))
                {
                    return false;
                }
                held.push_back(run);
            }
            else if (allocated.GetError() != Error::Exhausted)
            {
                return false;
            }
        }
        else if ((operation == 1U) && !held.empty())
        {
            const Size selected = static_cast<Size>(random.Below(held.size()));
            auto run = held[selected];
            // Simulate post-zero rollback versus a previously published owner.
            const bool dirty = random.Chance(1U, 2U);
            if (!dirty)
            {
                for (Size page{0U}; page < run.Pages; ++page)
                {
                    run.DirtyMask[page / 64U] |= UInt64{1U} << (page % 64U);
                }
                if (allocator.ReturnVerified(run) != Error::Unverified)
                {
                    return false;
                }
                model.Zero(run);
            }
            if ((dirty ? allocator.Release(run) : allocator.ReturnVerified(run)) != Error::None)
            {
                return false;
            }
            model.Return(run, dirty);
            if (allocator.Release(run) != Error::NotAllocated)
            {
                return false;
            }
            held.erase(held.begin() + static_cast<std::ptrdiff_t>(selected));
        }
        else if (operation >= 2U)
        {
            const auto maximum = static_cast<Foundation::UInt8>(random.Below(5U));
            Size lowest{Pages};
            for (Size page{0U}; page < Pages; ++page)
            {
                if (model.States[page].Free && model.States[page].Dirty)
                {
                    lowest = page;
                    break;
                }
            }
            const auto claim = allocator.ClaimDirty(maximum);
            if (claim.HasValue() != (lowest != Pages))
            {
                return false;
            }
            if (claim.HasValue())
            {
                auto run = claim.GetValue();
                Size order{0U};
                while (!model.Block((base + lowest) & ~((UInt64{1U} << order) - 1U), order))
                {
                    ++order;
                }
                order = std::min(order, static_cast<Size>(maximum));
                const UInt64 predicted = (base + lowest) & ~((UInt64{1U} << order) - 1U);
                if ((run.PhysicalStart.GetValue() != predicted * PageBytes) || (run.Pages != (UInt64{1U} << order)))
                {
                    return false;
                }
                if (run.Pages > 1U)
                {
                    auto partial = run;
                    partial.Pages /= 2U;
                    if (allocator.AbortScrub(partial) != Error::NotClaimed)
                    {
                        return false;
                    }
                }
                const auto action = random.Below(3U);
                const auto error = action == 0U   ? allocator.FinishScrub(run)
                                   : action == 1U ? allocator.AbortScrub(run)
                                                  : allocator.Retire(run);
                if (error != Error::None)
                {
                    return false;
                }
                if (action == 0U)
                {
                    model.Zero(run);
                    model.Return(run, false);
                }
                if (action == 2U)
                {
                    for (Size page{static_cast<Size>(predicted - base)}; page < (predicted - base + run.Pages); ++page)
                    {
                        model.States[page].Free = false;
                        model.States[page].Retired = true;
                    }
                    if (allocator.AbortScrub(run) != Error::NotClaimed)
                    {
                        return false;
                    }
                }
            }
        }
        const auto snapshot = allocator.GetSnapshot();
        UInt64 clean{0U};
        UInt64 dirty{0U};
        UInt64 allocated{0U};
        UInt64 retired{0U};
        for (const Page page : model.States)
        {
            clean += (page.Free && !page.Dirty) ? 1U : 0U;
            dirty += (page.Free && page.Dirty) ? 1U : 0U;
            retired += page.Retired ? 1U : 0U;
            allocated += (!page.Free && !page.Retired) ? 1U : 0U;
        }
        if ((snapshot.CleanPages != clean) || (snapshot.DirtyPages != dirty) ||
            (snapshot.AllocatedPages != allocated) || (snapshot.RetiredPages != retired) ||
            (snapshot.ClaimedPages != 0U) || (snapshot.AllocatablePages != Pages - retired))
        {
            return false;
        }
        UInt64 lowBlocks{0U};
        for (UInt64 first{(base + 511U) & ~UInt64{511U}}; first + 512U <= base + Pages; first += 512U)
        {
            lowBlocks += model.Free(first, 512U) && first + 512U <= (UInt64{1U} << 32U) / PageBytes ? 1U : 0U;
        }
        if (snapshot.FreeLowBlocks != lowBlocks)
        {
            return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"PhysicalPageAllocatorModelTests"};
    const auto seeds = Asteria::TestSupport::GetSeedRange(0x032C00U, 12U);
    for (UInt64 index{0U}; index < seeds.Count; ++index)
    {
        const UInt64 seed = seeds.First + index;
        const bool passed = RunSeed(seed);
        if (!passed)
        {
            static_cast<void>(std::fprintf(stderr, "replay: ASTERIA_TEST_SEED=%llu\n", seed));
        }
        ASTERIA_EXPECT(context, "1500 stateful operations predict exact blocks, masks and zeroed new owners", passed);
    }
    {
        static PhysicalPageAllocator allocator;
        constexpr Size span{1U << 18U};
        std::vector<UInt64> metadata((PhysicalPageAllocator::MetadataBytes(span) * 2U + 7U) / 8U);
        const PhysicalPageImportRange ranges[]{{PhysicalAddress{0x0020'0000U}, span},
                                               {PhysicalAddress{0x1'0020'0000ULL}, span}};
        const auto imported = allocator.Import(ranges, 2U, metadata.data(), metadata.size() * sizeof(UInt64));
        ASTERIA_EXPECT(context, "half a million dirty pages occupy two externally budgeted segments",
                       imported.HasValue() && imported.GetValue().ManagedPages == 2U * span &&
                           imported.GetValue().Segments == 2U);
        const auto high = allocator.Allocate(512U, 9U, 0U);
        const auto low = allocator.Allocate(512U, 9U, 32U);
        ASTERIA_EXPECT(context, "unlimited placement is top-down and DMA32 placement bottom-up",
                       high.HasValue() && high.GetValue().PhysicalStart.GetValue() == 0x1'4000'0000ULL &&
                           low.HasValue() && low.GetValue().PhysicalStart.GetValue() == 0x0020'0000U);
        if (high.HasValue())
        {
            auto clean = high.GetValue();
            for (auto& word : clean.DirtyMask)
            {
                word = 0U;
            }
            ASTERIA_EXPECT(context, "verified high run becomes clean", allocator.ReturnVerified(clean) == Error::None);
            const auto selected = allocator.Allocate(300U, 0U, 0U, true);
            ASTERIA_EXPECT(context, "clean-only finds the distant prefix through its index",
                           selected.HasValue() &&
                               selected.GetValue().PhysicalStart.GetValue() == clean.PhysicalStart.GetValue());
        }
        ASTERIA_EXPECT(context, "padding is not managed", !allocator.Contains(PhysicalAddress{0x001F'F000U}, 1U));
    }
    {
        static PhysicalPageAllocator allocator;
        std::vector<UInt64> metadata(100000U);
        std::vector<PhysicalPageImportRange> ranges;
        for (Size i{0U}; i < 65U; ++i)
        {
            ranges.push_back({PhysicalAddress{(2ULL << 20U) + i * (128ULL << 20U)}, 512U});
        }
        const auto result =
            allocator.Import(ranges.data(), ranges.size(), metadata.data(), metadata.size() * sizeof(UInt64));
        ASTERIA_EXPECT(context, "segment capacity leaves the highest range unmanaged",
                       result.HasValue() && result.GetValue().Segments == 64U &&
                           result.GetValue().UnmanagedPages == 512U);
    }
    {
        static PhysicalPageAllocator allocator;
        const PhysicalPageImportRange range{PhysicalAddress{0x200000U}, 2048U};
        std::vector<UInt64> metadata((PhysicalPageAllocator::MetadataBytes(2048U) + 7U) / 8U);
        const auto result = allocator.Import(&range, 1U, metadata.data(), metadata.size() * sizeof(UInt64), 1U);
        bool consumed{result.HasValue()};
        for (Size i{0U}; i < 3U; ++i)
        {
            consumed = allocator.Allocate(512U, 9U, 0U).HasValue() && consumed;
        }
        const auto stopped = allocator.Allocate(1U, 0U, 0U);
        const auto low = allocator.GetSnapshot().FreeLowBlocks;
        const auto dma = allocator.Allocate(1U, 0U, 32U);
        ASTERIA_EXPECT(context, "limit zero preserves the overridden low-block watermark for DMA32", consumed,
                       !stopped.HasValue(), low == 1U,
                       dma.HasValue() && dma.GetValue().PhysicalStart.GetValue() == 0x200000U);
        static PhysicalPageAllocator invalid;
        ASTERIA_EXPECT(context, "lowmemory beyond the admitted low blocks is refused",
                       !invalid.Import(&range, 1U, metadata.data(), metadata.size() * sizeof(UInt64), 5U).HasValue());
    }
    {
        static PhysicalPageAllocator allocator;
        const PhysicalPageImportRange ranges[]{{PhysicalAddress{0x200000U}, 512U},
                                               {PhysicalAddress{0x100000000ULL}, 512U}};
        std::vector<UInt64> metadata((PhysicalPageAllocator::MetadataBytes(512U) + 7U) / 8U);
        const auto result = allocator.Import(ranges, 2U, metadata.data(), metadata.size() * sizeof(UInt64));
        ASTERIA_EXPECT(context, "metadata admission preserves low memory and reports the higher segment",
                       result.HasValue() && result.GetValue().ManagedPages == 512U &&
                           result.GetValue().UnmanagedPages == 512U && result.GetValue().Segments == 1U);
    }
    {
        static Boot::BootInfo info{};
        info.MemoryRegionCount = 1U;
        info.MemoryRegions[0U] = {0U, 0U, 1024U, 8U, 4U, Boot::PhysicalMemoryKind::BootServices};
        info.DmaExclusions.Count = 1U;
        info.DmaExclusions.DeclaredCount = 1U;
        info.DmaExclusions.Ranges[0U] = {0x100000U, 0x101FFFU, 0U, Boot::BootDmaExclusionRmrr, 0U, 0U};
        static PhysicalPageImporter importer;
        ASTERIA_EXPECT(context, "v18 reservation in BootServices is subtracted",
                       importer.ExcludeBootInfo(info) && importer.Build(info, true));
        static PhysicalPageAllocator allocator;
        std::vector<UInt64> metadata(10000U);
        const auto result =
            allocator.Import(importer.Ranges(), importer.Count(), metadata.data(), metadata.size() * sizeof(UInt64));
        ASTERIA_EXPECT(context, "no page zero or RMRR enters management",
                       result.HasValue() && result.GetValue().ManagedPages == 1021U &&
                           !allocator.Contains(PhysicalAddress{0U}, 1U) &&
                           !allocator.Contains(PhysicalAddress{0x100000U}, 2U));
    }
    return context.Finish();
}
