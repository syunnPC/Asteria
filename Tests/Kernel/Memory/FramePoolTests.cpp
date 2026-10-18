// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Memory/FramePool.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Memory::FramePool;
using Kernel::Memory::FramePoolError;
using Kernel::Memory::PhysicalAddress;

constexpr UInt64 Base{0x0010'0000U};
constexpr UInt64 PageBytes{4096U};

[[nodiscard]] PhysicalAddress Page(const UInt64 index) noexcept
{
    return PhysicalAddress{Base + (index * PageBytes)};
}

void TestConfiguration(TestContext& runner) noexcept
{
    static FramePool unready;
    static FramePool misaligned;
    static FramePool oversized;
    static FramePool pool;
    const auto early = unready.Allocate(1U);
    ASTERIA_EXPECT(runner, "a pool takes an aligned, nonempty, bounded range once",
                   !early.HasValue() && (early.GetError() == FramePoolError::NotReady),
                   misaligned.Initialize(PhysicalAddress{Base + 1U}, 4U) == FramePoolError::InvalidConfiguration,
                   oversized.Initialize(PhysicalAddress{Base}, FramePool::MaximumPages + 1U) ==
                       FramePoolError::InvalidConfiguration,
                   pool.Initialize(PhysicalAddress{0U}, 4U) == FramePoolError::InvalidConfiguration,
                   pool.Initialize(PhysicalAddress{Base}, 0U) == FramePoolError::InvalidConfiguration,
                   pool.Initialize(PhysicalAddress{Base}, 8U) == FramePoolError::None,
                   pool.Initialize(PhysicalAddress{Base}, 8U) == FramePoolError::AlreadyInitialized,
                   pool.GetSnapshot().FreePages == 8U, pool.GetSnapshot().Pages == 8U);
}

void TestFacade(TestContext& runner) noexcept
{
    static FramePool pool;
    const bool ready = pool.Initialize(Page(0U), 16U) == FramePoolError::None;
    const auto first = pool.Allocate(3U);
    const auto second = pool.Allocate(2U);
    ASTERIA_EXPECT(runner, "the facade uses the allocator's smallest natural buddy and returns its unused tail", ready,
                   first.HasValue(), first.GetValue().GetValue() == Base, second.HasValue(),
                   second.GetValue().GetValue() == Base + 4U * PageBytes);
    const bool released = pool.Release(first.GetValue(), 3U) == FramePoolError::None;
    const auto before = pool.GetSnapshot();
    const auto claim = pool.BeginScrub(FramePool::MaximumRunPages);
    ASTERIA_EXPECT(runner, "legacy collection combines adjacent dirty order-zero claims, preserving accounting",
                   released, before.DirtyPages == 3U, claim.HasValue(), claim.GetValue().Pages == 3U,
                   pool.GetSnapshot().ScrubbingPages == 3U,
                   pool.EndScrub(claim.GetValue().PhysicalStart, 1U) == FramePoolError::NotScrubbing,
                   pool.AbortScrub(Page(1U), 2U) == FramePoolError::NotScrubbing,
                   pool.EndScrub(claim.GetValue().PhysicalStart, claim.GetValue().Pages) == FramePoolError::None,
                   pool.GetSnapshot().DirtyPages == 0U, pool.GetSnapshot().FreePages == 14U);
    ASTERIA_EXPECT(runner, "only allocated or claimed pages return, and the legacy request bounds remain",
                   pool.Return(second.GetValue(), 2U) == FramePoolError::None,
                   pool.Release(second.GetValue(), 2U) == FramePoolError::NotAllocated,
                   pool.EndScrub(Page(0U), 1U) == FramePoolError::NotScrubbing,
                   !pool.Allocate(FramePool::MaximumRunPages + 1U).HasValue(), !pool.Allocate(1U, 3U).HasValue());
}
void TestBatchedPrefix(TestContext& runner) noexcept
{
    static FramePool pool;
    const bool ready = pool.Initialize(Page(0U), FramePool::MaximumPages) == FramePoolError::None;
    const auto allocated = pool.GetAllocator().Allocate(300U);
    if (!ASTERIA_EXPECT(runner, "batch model has one 300-page dirty run", ready, allocated.HasValue()))
    {
        return;
    }
    const auto run = allocated.GetValue();
    ASTERIA_EXPECT(runner, "batch model releases the allocated run",
                   pool.GetAllocator().Release(run) == Kernel::Memory::PhysicalPageAllocatorError::None);
    UInt64 consumed{0U};
    for (Size visit{0U}; visit < 3U; ++visit)
    {
        const auto claim = pool.BeginScrub(128U);
        const UInt64 expected{visit == 2U ? 44U : 128U};
        if (!ASTERIA_EXPECT(runner, "each visit takes exactly the contiguous dirty prefix",
                            claim.HasValue() &&
                                claim.GetValue().PhysicalStart.GetValue() ==
                                    run.PhysicalStart.GetValue() + consumed * PageBytes &&
                                claim.GetValue().Pages == expected))
        {
            return;
        }
        ASTERIA_EXPECT(runner, "one batch is one claim with the original accounting",
                       pool.GetSnapshot().ScrubbingPages == expected,
                       pool.EndScrub(claim.GetValue().PhysicalStart, expected) == FramePoolError::None);
        consumed += expected;
    }
    ASTERIA_EXPECT(runner, "clean tail is never claimed by collection", consumed == 300U,
                   pool.GetSnapshot().DirtyPages == 0U, pool.GetSnapshot().FreePages == FramePool::MaximumPages,
                   !pool.BeginScrub(128U).HasValue());
}

} // namespace
} // namespace Asteria::Test
int main()
{
    TestContext runner{"FramePoolTests"};
    Asteria::Test::TestConfiguration(runner);
    Asteria::Test::TestFacade(runner);
    Asteria::Test::TestBatchedPrefix(runner);
    return runner.Finish();
}
