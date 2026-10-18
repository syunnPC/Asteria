// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Amd64/Memory/Amd64X2ApicTlbShootdownNotifier.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::UInt64 g_commands[Kernel::Scheduler::MaximumProcessorCount]{};
Foundation::Size g_commandCount{0U};
} // namespace Asteria::Test

extern "C" void AsteriaAmd64WriteX2ApicInterruptCommand(const Asteria::Foundation::UInt64 command) noexcept
{
    if (Asteria::Test::g_commandCount < Asteria::Kernel::Scheduler::MaximumProcessorCount)
    {
        Asteria::Test::g_commands[Asteria::Test::g_commandCount] = command;
    }
    ++Asteria::Test::g_commandCount;
}

namespace Asteria::Test
{
namespace
{
using Hal::Memory::ITlbShootdownNotifier;
using Platform::Amd64::Memory::Amd64X2ApicTlbShootdownNotifier;
using Platform::Amd64::Memory::Amd64X2ApicTlbShootdownNotifierInitializationError;
using Platform::Amd64::Memory::Amd64X2ApicTlbShootdownNotifierSnapshot;
using Platform::Amd64::Memory::Amd64X2ApicTlbShootdownNotifierState;

void ResetCommands() noexcept
{
    for (Foundation::UInt64& command : g_commands)
    {
        command = 0U;
    }
    g_commandCount = 0U;
}

[[nodiscard]] bool IsInitialSnapshot(const Amd64X2ApicTlbShootdownNotifierSnapshot& snapshot) noexcept
{
    return (snapshot.X2ApicIds[0U] == 0U) && (snapshot.X2ApicIds[1U] == 0U) && (snapshot.X2ApicIds[2U] == 0U) &&
           (snapshot.X2ApicIds[3U] == 0U) && (snapshot.OnlineProcessorMask == 0U) && (snapshot.Vector == 0U) &&
           (snapshot.State == Amd64X2ApicTlbShootdownNotifierState::Uninitialized) && (snapshot.Reserved == 0U);
}

void TestValidation(TestContext& runner) noexcept
{
    constexpr Foundation::UInt32 X2ApicIds[]{
        0x10U,
        0x20U,
        0x30U,
        0x40U,
    };

    Amd64X2ApicTlbShootdownNotifier notifier;
    ResetCommands();
    notifier.Notify(1U);
    ASTERIA_EXPECT(runner, "an uninitialized notifier performs no native write",
                   IsInitialSnapshot(notifier.GetSnapshot()), g_commandCount == 0U);

    ASTERIA_EXPECT(
        runner, "exception and reserved spurious vectors are rejected atomically",
        notifier.Initialize(31U, 1U, X2ApicIds) == Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidVector,
        notifier.Initialize(255U, 1U, X2ApicIds) == Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidVector,
        IsInitialSnapshot(notifier.GetSnapshot()));

    ASTERIA_EXPECT(runner, "empty and out-of-range processor masks are rejected atomically",
                   notifier.Initialize(0xF0U, 0U, X2ApicIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidProcessorMask,
                   notifier.Initialize(0xF0U, 0x10U, X2ApicIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidProcessorMask,
                   IsInitialSnapshot(notifier.GetSnapshot()));

    constexpr Foundation::UInt32 DuplicateIds[]{
        0x10U,
        0x10U,
        0x30U,
        0x40U,
    };
    constexpr Foundation::UInt32 BroadcastIds[]{
        0x10U,
        0x20U,
        0xFFFF'FFFFU,
        0x40U,
    };
    ASTERIA_EXPECT(runner, "online processors require distinct unicast x2APIC identifiers",
                   notifier.Initialize(0xF0U, 0x03U, DuplicateIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidX2ApicId,
                   notifier.Initialize(0xF0U, 0x04U, BroadcastIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::InvalidX2ApicId,
                   IsInitialSnapshot(notifier.GetSnapshot()));
}

void TestNotification(TestContext& runner) noexcept
{
    constexpr Foundation::UInt32 X2ApicIds[]{
        0U,
        0x1234'5678U,
        0x2222'2222U,
        0xFFFF'FFFEU,
    };
    constexpr Foundation::UInt8 OnlineMask{0x0BU};
    constexpr Foundation::UInt8 Vector{0xF0U};

    Amd64X2ApicTlbShootdownNotifier notifier;
    ASTERIA_EXPECT(runner, "valid topology initialization succeeds once",
                   notifier.Initialize(Vector, OnlineMask, X2ApicIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::None);

    const Amd64X2ApicTlbShootdownNotifierSnapshot snapshot = notifier.GetSnapshot();
    ASTERIA_EXPECT(runner, "successful initialization retains only the online topology", snapshot.X2ApicIds[0U] == 0U,
                   snapshot.X2ApicIds[1U] == 0x1234'5678U, snapshot.X2ApicIds[2U] == 0U,
                   snapshot.X2ApicIds[3U] == 0xFFFF'FFFEU, snapshot.OnlineProcessorMask == OnlineMask,
                   snapshot.Vector == Vector, snapshot.State == Amd64X2ApicTlbShootdownNotifierState::Ready,
                   snapshot.Reserved == 0U,
                   notifier.Initialize(Vector, OnlineMask, X2ApicIds) ==
                       Amd64X2ApicTlbShootdownNotifierInitializationError::AlreadyInitialized);

    ITlbShootdownNotifier& interface = notifier;
    ResetCommands();
    interface.Notify(0x0AU);
    ASTERIA_EXPECT(runner, "one physical fixed edge IPI is encoded for each selected CPU", g_commandCount == 2U,
                   g_commands[0U] == 0x1234'5678'0000'00F0ULL, g_commands[1U] == 0xFFFF'FFFE'0000'00F0ULL);

    ResetCommands();
    interface.Notify(0x01U);
    ASTERIA_EXPECT(runner, "x2APIC identifier zero is encoded as a valid unicast destination", g_commandCount == 1U,
                   g_commands[0U] == 0xF0U);

    ResetCommands();
    interface.Notify(0U);
    interface.Notify(0x04U);
    interface.Notify(0x10U);
    ASTERIA_EXPECT(runner, "contract-violating masks cannot produce a partial notification", g_commandCount == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64X2ApicTlbShootdownNotifierTests"};
    Asteria::Test::TestValidation(runner);
    Asteria::Test::TestNotification(runner);
    return runner.Finish();
}
