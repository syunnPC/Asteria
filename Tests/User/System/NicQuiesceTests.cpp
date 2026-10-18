// SPDX-License-Identifier: Apache-2.0
#include "User/System/NicQuiesce.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt32;
using User::System::AcknowledgeNic;
using User::System::BeginNicQuiesce;
using User::System::IsNicQuiet;

// The first page of the e1000e's BAR0 as 32-bit words; the mask-clear
// register only records what was written to it here.
struct NicRegisters
{
    alignas(8) UInt32 Words[1024]{};

    [[nodiscard]] volatile UInt32* Registers() noexcept
    {
        return Words;
    }
};

constexpr Foundation::Size Ctrl{0x0000U / 4U};
constexpr Foundation::Size Icr{0x00C0U / 4U};
constexpr Foundation::Size Imc{0x00D8U / 4U};
constexpr Foundation::Size Rctl{0x0100U / 4U};
constexpr Foundation::Size Tctl{0x0400U / 4U};
constexpr UInt32 Reset{1U << 26U};
constexpr UInt32 SetLinkUp{1U << 6U};

// Begin masks every cause and sets the reset bit, keeping CTRL's other bits, and reports whether a
// ring was enabled.
void TestBegin(TestContext& runner) noexcept
{
    NicRegisters receiving;
    receiving.Words[Ctrl] = SetLinkUp;
    receiving.Words[Rctl] = 0x8002U;
    NicRegisters sending;
    sending.Words[Tctl] = 0x10AU;
    NicRegisters idle;
    const bool receivingRan = BeginNicQuiesce(receiving.Registers());
    const bool sendingRan = BeginNicQuiesce(sending.Registers());
    const bool idleRan = BeginNicQuiesce(idle.Registers());
    ASTERIA_EXPECT(runner, "beginning a NIC quiesce masks every cause, resets it and reports an enabled ring",
                   receivingRan, sendingRan, !idleRan, receiving.Words[Imc] == 0xFFFF'FFFFU,
                   receiving.Words[Ctrl] == (SetLinkUp | Reset), idle.Words[Ctrl] == Reset);
}

// The device is quiet once the reset bit reads back clear; acknowledging
// masks every cause again and returns the cause register it read.
void TestQuietAndAcknowledge(TestContext& runner) noexcept
{
    NicRegisters nic;
    nic.Words[Ctrl] = Reset;
    const bool resetting = IsNicQuiet(nic.Registers());
    nic.Words[Ctrl] = SetLinkUp;
    const bool reset = IsNicQuiet(nic.Registers());
    nic.Words[Icr] = 0x80U;
    nic.Words[Imc] = 0U;
    const UInt32 found = AcknowledgeNic(nic.Registers());
    ASTERIA_EXPECT(runner, "the NIC is quiet once its reset bit clears, and acknowledging reads its cause", !resetting,
                   reset, found == 0x80U, nic.Words[Imc] == 0xFFFF'FFFFU);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"NicQuiesceTests"};
    Asteria::Test::TestBegin(runner);
    Asteria::Test::TestQuietAndAcknowledge(runner);
    return runner.Finish();
}
