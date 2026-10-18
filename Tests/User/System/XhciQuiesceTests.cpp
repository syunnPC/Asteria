// SPDX-License-Identifier: Apache-2.0
#include "User/System/XhciQuiesce.hpp"

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt32;
using Asteria::TestSupport::TestContext;
namespace S = Asteria::User::System;

// Independent register schedule: a halted controller may reset only after CNR
// clears, and no DMA page may be released until reset finishes while halted.
void RecoverySchedules(TestContext& runner) noexcept
{
    for (UInt32 seed{1U}; seed <= 128U; ++seed)
    {
        alignas(8) UInt32 registers[64U]{};
        registers[0U] = ((seed & 1U) != 0U ? 0x0096'0000U : 0x0100'0000U) | 32U;
        registers[8U] = 5U;
        S::XhciQuiesceState state{};
        ASTERIA_EXPECT(runner, "running controller stops and masks interrupts", S::BeginXhciQuiesce(registers, state),
                       registers[8U] == 0U);
        for (UInt32 step{0U}; step < (seed % 7U) + 1U; ++step)
        {
            registers[9U] = (step & 1U) != 0U ? 0x800U : 0U;
            ASTERIA_EXPECT(runner, "running or not ready cannot reset or release DMA",
                           !S::PollXhciQuiesce(registers, state), registers[8U] == 0U);
        }
        registers[9U] = 1U;
        ASTERIA_EXPECT(runner, "halt starts reset but retains DMA", !S::PollXhciQuiesce(registers, state),
                       registers[8U] == 2U);
        ASTERIA_EXPECT(runner, "outstanding reset retains DMA", !S::PollXhciQuiesce(registers, state));
        registers[8U] = 0U;
        registers[9U] = 0x801U;
        ASTERIA_EXPECT(runner, "CNR retains DMA after reset clears", !S::PollXhciQuiesce(registers, state));
        registers[9U] = 0U;
        ASTERIA_EXPECT(runner, "lost halted proof retains DMA", !S::PollXhciQuiesce(registers, state));
        registers[9U] = 1U;
        ASTERIA_EXPECT(runner, "halted reset and ready proves quiet", S::PollXhciQuiesce(registers, state),
                       state.Phase == S::XhciQuietPhase::Quiet);
        registers[8U] = 1U;
        ASTERIA_EXPECT(runner, "final quiet check detects a restarted controller",
                       !S::PollXhciQuiesce(registers, state));
    }
    alignas(8) UInt32 bad[64U]{};
    bad[0U] = 0x0097'0020U;
    S::XhciQuiesceState state{};
    ASTERIA_EXPECT(runner, "reserved revision rejects recovery without writes", !S::BeginXhciQuiesce(bad, state),
                   !S::PollXhciQuiesce(bad, state), bad[8U] == 0U);
    bad[0U] = 0x0096'0020U;
    bad[4U] = 16U << 16U;
    bad[16U] = 1U | 0x10000U;
    bad[8U] = 5U;
    ASTERIA_EXPECT(runner, "BIOS ownership timeout retains quarantine without reset", !S::BeginXhciQuiesce(bad, state),
                   bad[8U] == 5U, !S::PollXhciQuiesce(bad, state));
    bad[16U] = 1U | 0x01000000U;
    ASTERIA_EXPECT(runner, "released firmware permits manager recovery", S::BeginXhciQuiesce(bad, state),
                   bad[8U] == 0U);
    bad[8U] = 5U;
    bad[9U] = 0x800U;
    ASTERIA_EXPECT(runner, "initial CNR retains firmware operational registers", !S::BeginXhciQuiesce(bad, state),
                   bad[8U] == 5U, state.Phase == S::XhciQuietPhase::WaitingReady, !S::PollXhciQuiesce(bad, state),
                   bad[8U] == 5U);
    bad[9U] = 0U;
    ASTERIA_EXPECT(runner, "ready transition masks the firmware schedule before halt/reset",
                   !S::PollXhciQuiesce(bad, state), bad[8U] == 0U, state.Phase == S::XhciQuietPhase::Halting);
    bad[4U] = 1024U << 16U;
    bad[8U] = 5U;
    ASTERIA_EXPECT(runner, "out-of-mapping capability cannot authorize reset", !S::BeginXhciQuiesce(bad, state),
                   bad[8U] == 5U);
}
} // namespace
int main()
{
    TestContext runner{"xHCI recovery model"};
    RecoverySchedules(runner);
    return runner.Finish();
}
