// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Time/ProcessorClock.hpp"
#include "Abi/Clock.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Kernel/Diagnostics/Watchdog.hpp"
#include "Support/TestContext.hpp"

int main()
{
    namespace Abi = Asteria::Abi;
    namespace Foundation = Asteria::Foundation;
    namespace Hal = Asteria::Hal;
    namespace Kernel = Asteria::Kernel;
    namespace TestSupport = Asteria::TestSupport;
    TestSupport::TestContext test{"ProcessorClockTests"};
    constexpr auto epsilon = Kernel::Time::ClockEpsilonTicks(1'000'000U);
    Kernel::Scheduler::ProcessorSet processors;
    const Kernel::Scheduler::CpuId cpu{1U};
    const bool admitted = processors.Admit(cpu);
    const auto bad = Kernel::Time::ClockResidual(1000U, 800U, 1010U);
    if (Kernel::Time::ClockResidualExcluded(bad, epsilon))
    {
        processors.ExcludeUserPlacement(cpu);
    }
    ASTERIA_EXPECT(test, "injected skew above epsilon excludes placement and retains kernel admission", admitted,
                   processors.OnlineMask() == 2U, processors.UserPlacementMask() == 0U,
                   !Kernel::Time::ClockResidualExcluded({-100000, 100000, 200000U, 1U}, epsilon),
                   !Kernel::Time::ClockResidualExcluded({-50, 50, 100U, 1U}, epsilon),
                   Kernel::Time::ClockResidualExcluded({-200, -100, 100U, 1U}, epsilon));
    const Kernel::Time::ProcessorClockResidual within{-10, 10, 20U, Kernel::Time::ProcessorClockMeasured};
    auto virtualWithin = within;
    virtualWithin.Flags |= Kernel::Time::ProcessorClockHypervisor;
    auto virtualBad = bad;
    virtualBad.Flags |= Kernel::Time::ProcessorClockHypervisor;
    auto bareInvariant = within;
    bareInvariant.Flags |= Kernel::Time::ProcessorClockInvariant;
    ASTERIA_EXPECT(test, "bare metal requires invariant TSC; hypervisor placement uses the measured residual",
                   Kernel::Time::ClockPlacementExcluded(within, epsilon),
                   !Kernel::Time::ClockPlacementExcluded(bareInvariant, epsilon),
                   !Kernel::Time::ClockPlacementExcluded(virtualWithin, epsilon),
                   Kernel::Time::ClockPlacementExcluded(virtualBad, epsilon));
    struct Timer
    {
        Foundation::UInt64 Stage{0U};
        Foundation::UInt64 Itc{100U};
        Foundation::UInt64 Deadline{200U};
        [[nodiscard]] Hal::Time::LocalTimerError CancelDeadline() noexcept
        {
            Stage = 1U;
            Deadline = 0U;
            return Hal::Time::LocalTimerError::None;
        }
        [[nodiscard]] bool Arm() noexcept
        {
            if (Stage != 3U)
            {
                return false;
            }
            Deadline = Itc + 100U;
            Stage = 4U;
            return true;
        }
    } timer;
    bool wroteUnarmed{false};
    const bool initialized = Kernel::Time::InitializeOnlineCounter(
        timer,
        [&](const Foundation::UInt64 itc) noexcept
        {
            wroteUnarmed = timer.Stage == 1U && timer.Deadline == 0U;
            timer.Itc = itc;
            timer.Stage = 2U; // mov ar.itc
            timer.Stage = 3U; // srlz.d: checked independently on the linked native leaf.
        },
        1000U);
    const bool armed = timer.Arm();
    ASTERIA_EXPECT(test, "IA-64 online cancels probe, writes and serializes ITC before arming from new time",
                   initialized, wroteUnarmed, armed, timer.Stage == 4U, timer.Deadline == 1100U);
    Kernel::Time::ProcessorClockRendezvous rendezvous;
    const auto request = rendezvous.Request(1U);
    const bool pending = !rendezvous.Completed(1U, request);
    rendezvous.Respond(1U, 1234U);
    ASTERIA_EXPECT(test, "clock ping-pong publishes a single reference sample", pending,
                   rendezvous.Completed(1U, request), rendezvous.Reference(1U) == 1234U);
    ASTERIA_EXPECT(test, "user counters clamp negative elapsed time", Abi::ElapsedClockTicks(90U, 100U) == 0U,
                   Abi::ElapsedClockTicks(110U, 100U) == 10U, Abi::ClampClockTicks(90U, 100U) == 100U,
                   Abi::ClampClockTicks(110U, 100U) == 110U);
    ASTERIA_EXPECT(test, "finite elapsed intervals retain forward counter wrap",
                   Abi::ElapsedClockTicks(3U, ~Foundation::UInt64{0U} - 2U) == 6U,
                   Abi::ClampClockTicks(3U, ~Foundation::UInt64{0U} - 2U) == 3U);
    Kernel::Diagnostics::Watchdog watchdog;
    watchdog.Configure(5U, 1000U);
    watchdog.Online(0U, 0U);
    watchdog.Online(1U, 0U);
    const auto oldClock = watchdog.Check(1U, 10U, 3U);
    watchdog.Online(1U, 0U); // native online clears observations after writing ITC.
    ASTERIA_EXPECT(test, "online counter synchronization grants fresh watchdog observations", oldClock == 0U,
                   watchdog.Check(1U, 1'000'000U, 3U) == 0U);
    return test.Finish();
}
