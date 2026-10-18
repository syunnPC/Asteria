// SPDX-License-Identifier: Apache-2.0
#include "Platform/Amd64/Interrupt/Amd64ApplicationProcessorStartup.hpp"

#include <cstring>
#include <initializer_list>

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace
{
using Asteria::Platform::Amd64::Interrupt::Amd64ApplicationProcessorStartup;
using Asteria::Platform::Amd64::Interrupt::Amd64StartupMailbox;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Platform::Amd64::Time::Amd64ApicTimer;
alignas(4096U) UInt8 g_code[8192U]{};
} // namespace
int main()
{
    TestContext runner{"Amd64ApplicationProcessorStartupTests"};
    constexpr UInt8 source[]{0xFAU, 0xFCU, 0xF4U};
    ASTERIA_EXPECT(runner, "startup copy",
                   Amd64ApplicationProcessorStartup::PrepareCode(g_code, 0x90000U, source, sizeof(source)));
    bool zero{true};
    for (unsigned i{3U}; i < 4096U; ++i)
    {
        zero = zero && g_code[i] == 0U;
    }
    ASTERIA_EXPECT(runner, "copy and zero padding", std::memcmp(g_code, source, 3U) == 0, zero);
    for (UInt64 address : {UInt64{0U}, UInt64{0x90001U}, UInt64{0xFF000U}, ~UInt64{0U}})
    {
        ASTERIA_EXPECT(runner, "invalid low-memory extent",
                       !Amd64ApplicationProcessorStartup::PrepareCode(g_code, address, source, sizeof(source)));
    }
    ASTERIA_EXPECT(runner, "overlapping source",
                   !Amd64ApplicationProcessorStartup::PrepareCode(g_code, 0x90000U, g_code + 1U, 2U));
    ASTERIA_EXPECT(runner, "oversized source",
                   !Amd64ApplicationProcessorStartup::PrepareCode(g_code, 0x90000U, source, 4097U));
    ASTERIA_EXPECT(runner, "rejected copy preserves code", g_code[0U] == 0xFAU);
    Amd64StartupMailbox mailbox{};
    ASTERIA_EXPECT(runner, "mailbox valid",
                   Amd64ApplicationProcessorStartup::PrepareMailbox(mailbox, 0x90000U, 0x100000U, 128U, 0x200000U,
                                                                    0x300000U, 3U, 0x400000U));
    ASTERIA_EXPECT(runner, "real-mode and long-mode ABI", mailbox.Root == 0x100000U, mailbox.LongEntry == 0x90080U,
                   mailbox.Selector == 8U, mailbox.GdtLimit == 23U, mailbox.GdtBase[0U] == 0x40U,
                   mailbox.GdtBase[1U] == 0x10U, mailbox.GdtBase[2U] == 9U, mailbox.Processor == 3U);
    const auto before = mailbox;
    for (unsigned field{0U}; field < 7U; ++field)
    {
        ASTERIA_EXPECT(runner, "invalid mailbox",
                       !Amd64ApplicationProcessorStartup::PrepareMailbox(
                           mailbox, field == 0U ? 0xFF000U : 0x90000U, field == 1U ? 0x1'0000'0000ULL : 0x100000U,
                           field == 2U ? 4096U : 128U, field == 3U ? 0x200001U : 0x200000U,
                           field == 4U ? 0U : 0x300000U, field == 5U ? 4U : 1U,
                           field == 6U ? 0x8000'0000'0000ULL : 0x400000U));
        ASTERIA_EXPECT(runner, "mailbox failure atomicity", std::memcmp(&mailbox, &before, sizeof(mailbox)) == 0);
    }
    ASTERIA_EXPECT(runner, "sub-count deadline rounds up", Amd64ApicTimer::CountdownFor(10U, 11U, 8U) == 1U);
    ASTERIA_EXPECT(runner, "fractional deadline rounds up", Amd64ApicTimer::CountdownFor(10U, 27U, 8U) == 3U);
    ASTERIA_EXPECT(runner, "long deadline chunks", Amd64ApicTimer::CountdownFor(0U, ~UInt64{0U}, 1U) == 0xFFFF'FFFFU);
    ASTERIA_EXPECT(runner, "no ceiling overflow", Amd64ApicTimer::CountdownFor(0U, ~UInt64{0U}, ~UInt64{0U}) == 1U);
    ASTERIA_EXPECT(runner, "due and uncalibrated deadlines", Amd64ApicTimer::CountdownFor(10U, 10U, 8U) == 0U,
                   Amd64ApicTimer::CountdownFor(10U, 9U, 8U) == 0U, Amd64ApicTimer::CountdownFor(10U, 11U, 0U) == 0U);
    return runner.Finish();
}
