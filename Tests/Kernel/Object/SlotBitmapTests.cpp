// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <vector>

#include "Kernel/Object/SlotBitmap.hpp"

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Object::LowestSetBit;
using Kernel::Object::PoolSlotSets;
using Kernel::Object::PoolSlotState;
using Kernel::Object::SlotBitmap;

void TestLowestSetBit(TestContext& runner) noexcept
{
    bool every{true};
    for (Size bit{0U}; bit < 64U; ++bit)
    {
        const UInt64 single = UInt64{1U} << bit;
        // The lowest bit wins whatever lies above it.
        every = every && (LowestSetBit(single) == bit) && (LowestSetBit(single | (~UInt64{0U} << bit)) == bit);
    }
    ASTERIA_EXPECT(runner, "the de Bruijn table names the lowest set bit of every word", every);
}

// FindFirst against a plain array over a seeded sequence of inserts and
// removes: the lowest member at or above each probe, in at most a summary
// pass and two word reads.
template <Size Capacity> bool MatchesReference(const std::uint64_t seed) noexcept
{
    static SlotBitmap<Capacity> bitmap;
    bitmap = SlotBitmap<Capacity>{};
    std::vector<bool> reference(Capacity, false);
    TestSupport::Random random{seed};
    bool matches{bitmap.IsEmpty() && (bitmap.FindFirst() == Capacity)};
    for (Size step{0U}; matches && (step < 4000U); ++step)
    {
        const Size index = static_cast<Size>(random.Below(Capacity));
        if (random.Chance(1U, 2U))
        {
            bitmap.Insert(index);
            reference[index] = true;
        }
        else
        {
            bitmap.Remove(index);
            reference[index] = false;
        }
        const Size probe = static_cast<Size>(random.Below(Capacity + 1U));
        Size expected{Capacity};
        for (Size candidate{probe}; candidate < Capacity; ++candidate)
        {
            if (reference[candidate])
            {
                expected = candidate;
                break;
            }
        }
        bool empty{true};
        for (const bool member : reference)
        {
            empty = empty && !member;
        }
        matches = (bitmap.FindFirst(probe) == expected) && (bitmap.Contains(index) == reference[index]) &&
                  (bitmap.IsEmpty() == empty);
    }
    return matches;
}

void TestFindFirst(TestContext& runner) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(0x5107'B17AU, 8U);
    for (std::uint64_t seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        ASTERIA_EXPECT(runner, "FindFirst finds the lowest member at or above any index",
                       MatchesReference<16384U>(seed), MatchesReference<4096U>(seed), MatchesReference<100U>(seed),
                       MatchesReference<64U>(seed));
    }
    SlotBitmap<16384U> edges;
    edges.Insert(16383U);
    edges.Insert(4095U);
    edges.Insert(4096U);
    ASTERIA_EXPECT(runner, "summary-word and word boundaries, and indices beyond the capacity",
                   edges.FindFirst() == 4095U, edges.FindFirst(4096U) == 4096U, edges.FindFirst(4097U) == 16383U,
                   edges.FindFirst(16384U) == 16384U, !edges.Contains(16384U));
}

void TestPoolSlotSets(TestContext& runner) noexcept
{
    PoolSlotSets<8U> sets;
    const bool empty = (sets.GetFreeCount() == 0U) && (sets.FindFirst(PoolSlotState::Free) == 8U);
    for (Size index{0U}; index < 8U; ++index)
    {
        sets.AddFree(index);
    }
    sets.Move(0U, PoolSlotState::Free, PoolSlotState::Active);
    sets.Move(5U, PoolSlotState::Free, PoolSlotState::Active);
    sets.Move(5U, PoolSlotState::Active, PoolSlotState::Quiescing);
    ASTERIA_EXPECT(runner, "a pool's slots move between the free, live and quiescing sets with their counts", empty,
                   sets.GetFreeCount() == 6U, sets.GetActiveCount() == 1U, sets.GetQuiescingCount() == 1U,
                   sets.FindFirst(PoolSlotState::Free) == 1U, sets.FindFirst(PoolSlotState::Active) == 0U,
                   sets.FindFirst(PoolSlotState::Quiescing) == 5U, sets.FindFirst(PoolSlotState::Active, 1U) == 8U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SlotBitmapTests"};
    Asteria::Test::TestLowestSetBit(runner);
    Asteria::Test::TestFindFirst(runner);
    Asteria::Test::TestPoolSlotSets(runner);
    return runner.Finish();
}
