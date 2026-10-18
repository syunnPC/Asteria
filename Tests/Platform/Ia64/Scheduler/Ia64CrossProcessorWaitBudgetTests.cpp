// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Scheduler/Ia64CrossProcessorWaitBudget.hpp"

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

#include <cstdint>
#include <cstdio>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Foundation::UInt64;
using Platform::Ia64::Scheduler::Ia64CrossProcessorWaitBudget;

namespace
{
constexpr UInt64 StartTicks{1'000'000U};

/// Polls `polls` times at `ticks` and reports whether every poll was admitted.
[[nodiscard]] constexpr bool AdmitsAll(Ia64CrossProcessorWaitBudget& budget, const Size polls,
                                       const UInt64 ticks) noexcept
{
    for (Size poll{0U}; poll < polls; ++poll)
    {
        if (!budget.TryPoll(ticks))
        {
            return false;
        }
    }
    return true;
}

void TestBothMinimums(TestContext& runner) noexcept
{
    {
        // A fast host: the polls run out long before the interval passes.
        Ia64CrossProcessorWaitBudget budget{StartTicks, 3U, 100U};
        const bool polled = AdmitsAll(budget, 3U, StartTicks + 1U);
        const bool beforeInterval = budget.TryPoll(StartTicks + 99U);
        const bool atInterval = budget.TryPoll(StartTicks + 100U);
        ASTERIA_EXPECT(runner, "a waiter that has polled enough keeps waiting until the interval has passed", polled,
                       beforeInterval, !atInterval, budget.GetPolls() == 4U);
    }
    {
        // A stalled host: ITC passed the interval while the waiter did not run.
        Ia64CrossProcessorWaitBudget budget{StartTicks, 3U, 100U};
        const bool stalled = AdmitsAll(budget, 3U, StartTicks + 1'000'000U);
        const bool exhausted = budget.TryPoll(StartTicks + 1'000'000U);
        ASTERIA_EXPECT(runner, "a waiter that has seen the interval keeps waiting until it has polled enough", stalled,
                       !exhausted, budget.GetPolls() == 3U);
    }
    {
        Ia64CrossProcessorWaitBudget budget{StartTicks, 2U, 100U};
        const bool polled = AdmitsAll(budget, 2U, StartTicks + 200U);
        const bool reversed = budget.TryPoll(StartTicks - 1U);
        const bool expired = budget.TryPoll(StartTicks + 200U);
        ASTERIA_EXPECT(runner, "an interval time counter below the starting sample counts as no elapsed time", polled,
                       reversed, !expired);
    }
    {
        Ia64CrossProcessorWaitBudget budget{StartTicks, 0U, 0U};
        ASTERIA_EXPECT(runner, "zero minimums admit no poll", !budget.TryPoll(StartTicks), budget.GetPolls() == 0U);
    }
}

/// Seeded model: random polls at random interval times against the budget's
/// definition, an independent count of the admitted polls and the elapsed
/// time of the current sample.
void TestModel(TestContext& runner) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(0x1A64'0B0DU, 64U);
    for (std::uint64_t seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        TestSupport::Random random{seed};
        const Size minimumPolls = static_cast<Size>(random.Below(40U));
        const UInt64 minimumTicks = random.Below(1'000U);
        const UInt64 start = random.Below(2U) == 0U ? 0U : StartTicks;
        Ia64CrossProcessorWaitBudget budget{start, minimumPolls, minimumTicks};
        UInt64 now = start;
        Size admitted{0U};
        bool matches{true};
        for (Size step{0U}; (step < 200U) && matches; ++step)
        {
            const UInt64 choice = random.Below(8U);
            if (choice == 0U)
            {
                now = start - ((start == 0U) ? 0U : random.Below(start));
            }
            else if (choice == 1U)
            {
                now += random.Below(10'000U);
            }
            else
            {
                now += random.Below(8U);
            }
            const UInt64 elapsed = (now >= start) ? (now - start) : 0U;
            const bool expected = (admitted < minimumPolls) || (elapsed < minimumTicks);
            const bool observed = budget.TryPoll(now);
            if (expected)
            {
                ++admitted;
            }
            matches = (observed == expected) && (budget.GetPolls() == admitted);
        }
        if (!matches)
        {
            static_cast<void>(std::fprintf(stderr, "  reproduce with ASTERIA_TEST_SEED=%llu\n",
                                           static_cast<unsigned long long>(seed)));
        }
        ASTERIA_EXPECT(runner, "the budget admits exactly the polls its definition admits", matches);
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64CrossProcessorWaitBudgetTests"};
    Asteria::Test::TestBothMinimums(runner);
    Asteria::Test::TestModel(runner);
    return runner.Finish();
}
