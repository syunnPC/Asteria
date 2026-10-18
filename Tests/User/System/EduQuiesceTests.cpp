// SPDX-License-Identifier: Apache-2.0
#include "User/System/EduQuiesce.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt32;
using User::System::AcknowledgeEdu;
using User::System::IsEduQuiet;

// Edu's register page as 32-bit words; the acknowledgement register only records what was written
// to it here.
struct EduRegisters
{
    alignas(8) UInt32 Words[64]{};

    [[nodiscard]] volatile UInt32* Registers() noexcept
    {
        return Words;
    }
};

constexpr Foundation::Size Status{0x20U / 4U};
constexpr Foundation::Size InterruptStatus{0x24U / 4U};
constexpr Foundation::Size Acknowledge{0x64U / 4U};
constexpr Foundation::Size DmaCommand{0x98U / 4U};

// Documentation/Architecture/IommuDomain.md on IA-64: a factorial whose computing bit is already
// clear may still owe the interrupt it asked for, so the device is quiet only once that interrupt
// was raised; a transfer still running keeps it busy too.
void TestQuiet(TestContext& runner) noexcept
{
    EduRegisters idle;
    EduRegisters transferring;
    transferring.Words[DmaCommand] = 1U;
    EduRegisters computing;
    computing.Words[Status] = 0x81U;
    EduRegisters owed;
    owed.Words[Status] = 0x80U;
    EduRegisters raised;
    raised.Words[Status] = 0x80U;
    raised.Words[InterruptStatus] = 0x01U;
    EduRegisters polled;
    polled.Words[Status] = 0x00U;
    polled.Words[InterruptStatus] = 0x100U;
    ASTERIA_EXPECT(runner, "edu is quiet with nothing running and no factorial interrupt owed",
                   IsEduQuiet(idle.Registers()), !IsEduQuiet(transferring.Registers()),
                   !IsEduQuiet(computing.Registers()), !IsEduQuiet(owed.Registers()), IsEduQuiet(raised.Registers()),
                   IsEduQuiet(polled.Registers()));
}

// The status is acknowledged before the factorial interrupt request is
// cleared, and both are written.
void TestAcknowledge(TestContext& runner) noexcept
{
    EduRegisters raised;
    raised.Words[Status] = 0x80U;
    raised.Words[InterruptStatus] = 0x101U;
    const UInt32 found = AcknowledgeEdu(raised.Registers());
    ASTERIA_EXPECT(runner, "acknowledging a quiet edu writes back its status and clears its factorial request",
                   found == 0x101U, raised.Words[Acknowledge] == 0x101U, raised.Words[Status] == 0U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"EduQuiesceTests"};
    Asteria::Test::TestQuiet(runner);
    Asteria::Test::TestAcknowledge(runner);
    return runner.Finish();
}
