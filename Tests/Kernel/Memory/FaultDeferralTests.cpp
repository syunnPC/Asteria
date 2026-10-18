// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Memory/FaultDeferral.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Kernel::Memory::FaultDelivery;
using Asteria::Kernel::Memory::FaultOutcome;
using Asteria::Kernel::Memory::DecideFaultOutcome;

// Far past the 512 consecutive deferrals that once stopped the machine: a busy supervisor that a
// slice, a timed hold or other faults keep from the fault is not a kernel failure.
constexpr UInt64 LongDeferral{1U << 16U};

// A busy supervisor defers a fault however often, counting each deferral; a
// resolution resumes and clears the count, and only a failure stops.
void TestDeferralWithoutBound(TestContext& runner) noexcept
{
    UInt64 deferrals{0U};
    bool yielded{true};
    for (UInt64 round{0U}; round < LongDeferral; ++round)
    {
        yielded = yielded && (DecideFaultOutcome(FaultDelivery::Busy, deferrals) == FaultOutcome::Yield);
    }
    const UInt64 counted = deferrals;
    const FaultOutcome resolved = DecideFaultOutcome(FaultDelivery::Resolved, deferrals);
    const UInt64 cleared = deferrals;
    const FaultOutcome again = DecideFaultOutcome(FaultDelivery::Busy, deferrals);
    const UInt64 restarted = deferrals;
    const FaultOutcome failed = DecideFaultOutcome(FaultDelivery::Failed, deferrals);
    ASTERIA_EXPECT(runner, "a busy supervisor defers without bound, a resolution clears the count, a failure stops",
                   yielded, counted == LongDeferral, resolved == FaultOutcome::Resume, cleared == 0U,
                   again == FaultOutcome::Yield, restarted == 1U, failed == FaultOutcome::Stop, deferrals == 1U);
}

// The count saturates rather than wrapping to zero, so it never reads as a
// fresh fault, and the fault still yields.
void TestDeferralCountSaturates(TestContext& runner) noexcept
{
    constexpr UInt64 Maximum{~UInt64{0U}};
    UInt64 deferrals{Maximum - 1U};
    const FaultOutcome last = DecideFaultOutcome(FaultDelivery::Busy, deferrals);
    const UInt64 reached = deferrals;
    const FaultOutcome beyond = DecideFaultOutcome(FaultDelivery::Busy, deferrals);
    ASTERIA_EXPECT(runner, "the deferral count saturates and the fault keeps yielding", last == FaultOutcome::Yield,
                   reached == Maximum, beyond == FaultOutcome::Yield, deferrals == Maximum);
}
} // Namespace

int main()
{
    TestContext runner{"FaultDeferralTests"};
    TestDeferralWithoutBound(runner);
    TestDeferralCountSaturates(runner);
    return runner.Finish();
}
