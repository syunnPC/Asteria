// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Diagnostics/Watchdog.hpp"
#include "Support/TestContext.hpp"

int main()
{
    namespace Abi = Asteria::Abi;
    namespace Foundation = Asteria::Foundation;
    namespace Kernel = Asteria::Kernel;
    namespace TestSupport = Asteria::TestSupport;
    TestSupport::TestContext context{"WatchdogTests"};
    Kernel::Diagnostics::Watchdog watchdog;
    watchdog.Configure(5U, 1000U);
    for (Foundation::Size cpu{0U}; cpu < 4U; ++cpu)
    {
        watchdog.Online(cpu, 0x1234U + cpu);
    }
    Kernel::Diagnostics::WatchdogProcessorEvidence admitted{};
    watchdog.Sample(admitted, 0U);
    ASTERIA_EXPECT(context, "admission retains its site", admitted.HeartbeatSite == 0x1234U);
    ASTERIA_EXPECT(context, "first early-boot observation grants an interval", watchdog.Check(1U, 0U) == 0U);
    // Whole-machine pause: counters continue, but neither observer nor peers
    // had ticks. Even the first resumed observer tick cannot accuse a peer.
    watchdog.Beat(1U, 0U);
    ASTERIA_EXPECT(context, "whole-machine pause cannot manufacture peer silence", watchdog.Check(1U, 7000U) == 0U);
    for (Foundation::Size cpu{0U}; cpu < 4U; ++cpu)
    {
        watchdog.Beat(cpu, cpu);
    }
    ASTERIA_EXPECT(context, "resumed peers renew their observations", watchdog.Check(1U, 7250U) == 0U);
    // Calling Check repeatedly without an observer heartbeat is no progress.
    for (Foundation::UInt64 turn{1U}; turn <= 19U; ++turn)
    {
        ASTERIA_EXPECT(context, "a silent observer cannot age peers", watchdog.Check(1U, 7250U + turn * 250U) == 0U);
    }
    watchdog.Beat(1U, 0U);
    watchdog.Beat(3U, 0U);
    ASTERIA_EXPECT(context, "one resumed tick is not a full period", watchdog.Check(1U, 12250U) == 0U);
    for (Foundation::Size cpu{0U}; cpu < 4U; ++cpu)
    {
        watchdog.Beat(cpu, 0x1234U + cpu);
    }
    ASTERIA_EXPECT(context, "healthy peers reset silence", watchdog.Check(1U, 12500U) == 0U);
    // CPUs 0/2 really stall while the observer and CPU 3 make regular progress.
    for (Foundation::UInt64 turn{1U}; turn <= 20U; ++turn)
    {
        watchdog.Beat(1U, turn);
        watchdog.Beat(3U, turn);
        const auto expected = turn == 20U ? 5U : 0U;
        ASTERIA_EXPECT(context, "progressing observer detects only genuinely silent peers",
                       watchdog.Check(1U, 12500U + turn * 250U) == expected);
    }
    watchdog.Running(0U, 42U, Abi::ComponentBehaviorDisplay);
    Kernel::Diagnostics::WatchdogEvidence evidence{};
    watchdog.Snapshot(evidence, 1U, 17500U, 5U);
    ASTERIA_EXPECT(context, "retained age is observer progress, with site and component",
                   evidence.Processors[0U].AgeMilliseconds == 5000U, evidence.Processors[0U].HeartbeatSite == 0x1234U,
                   evidence.Processors[0U].Thread == 42U,
                   evidence.Processors[0U].Component == Abi::ComponentBehaviorDisplay);
    ASTERIA_EXPECT(context, "parked targets are excluded", watchdog.Check(1U, 17501U, 10U) == 0U);
    ASTERIA_EXPECT(context, "parked observers never accuse peers", watchdog.Check(1U, 99999U, 1U) == 0U);
    watchdog.Online(1U, 0U);
    ASTERIA_EXPECT(context, "readmission discards old observations", watchdog.Check(1U, 99999U) == 0U);
    watchdog.Beat(1U, 0U);
    ASTERIA_EXPECT(context, "clock regression renews observation", watchdog.Check(1U, 10U) == 0U);
    watchdog.Configure(0U, 1000U);
    ASTERIA_EXPECT(context, "off never stops", watchdog.Check(1U, 999999999U) == 0U);

    Kernel::Diagnostics::Watchdog delayed;
    delayed.Configure(5U, 400000000U);
    delayed.Online(0U, 0U);
    delayed.Online(1U, 0U);
    ASTERIA_EXPECT(context, "TCG turns start observation", delayed.Check(1U, 0U) == 0U);
    // A coalesced check earns at most one cadence, never missed ticks.
    for (Foundation::UInt64 turn{1U}; turn <= 19U; ++turn)
    {
        delayed.Beat(1U, turn);
        ASTERIA_EXPECT(context, "late checks do not accumulate fictional missed ticks",
                       delayed.Check(1U, turn * 160000000U) == 0U);
    }
    delayed.Beat(1U, 20U);
    ASTERIA_EXPECT(context, "coalesced real observer progress still detects a stalled CPU",
                   delayed.Check(1U, 3200000000U) == 1U);
    Kernel::Diagnostics::Watchdog bounded;
    bounded.Configure(5U, 1000U);
    bounded.Online(0U, 0U);
    bounded.Online(1U, 0U);
    static_cast<void>(bounded.Check(1U, 0U));
    // Repeated legal IF/PSR.i-clear work remains below the period, even
    // though its aggregate runtime spans many periods (Watchdog.md).
    for (Foundation::UInt64 turn{0U}; turn < 30U; ++turn)
    {
        for (Foundation::UInt64 step{1U}; step <= 19U; ++step)
        {
            bounded.Beat(1U, step);
            ASTERIA_EXPECT(context, "repeated bounded kernel work stays live",
                           bounded.Check(1U, turn * 4900U + step * 250U) == 0U);
        }
        bounded.Beat(0U, turn);
        bounded.Beat(1U, turn);
        ASTERIA_EXPECT(context, "a scheduling point renews bounded-work age",
                       bounded.Check(1U, (turn + 1U) * 4900U) == 0U);
    }
    return context.Finish();
}
