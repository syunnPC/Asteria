// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Ia64/Time/Ia64LocalTimer.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
Foundation::UInt64 g_ticks{0U};
Foundation::Size g_readCount{0U};
Foundation::Size g_initializeCount{0U};
Foundation::Size g_programCount{0U};
Foundation::Size g_cancelCount{0U};
Foundation::UInt64 g_initializeArgument{0U};
Foundation::UInt64 g_programDeadline{0U};
Foundation::UInt64 g_programVector{0U};
Foundation::UInt64 g_programRollbackDeadline{0U};
Foundation::UInt64 g_programRollbackVector{0U};
Foundation::UInt64 g_programMaskedVector{0U};
Foundation::UInt64 g_cancelArgument{0U};
Foundation::UInt64 g_initializeStatus{0U};
Foundation::UInt64 g_programStatus{0U};
Foundation::UInt64 g_cancelStatus{0U};
} // namespace Asteria::Test

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadIntervalTimeCounter() noexcept
{
    ++Asteria::Test::g_readCount;
    return Asteria::Test::g_ticks;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64InitializeLocalTimer(const Asteria::Foundation::UInt64 maskedInterruptVector) noexcept
{
    ++Asteria::Test::g_initializeCount;
    Asteria::Test::g_initializeArgument = maskedInterruptVector;
    return Asteria::Test::g_initializeStatus;
}

extern "C" Asteria::Foundation::UInt64 AsteriaIa64ProgramLocalTimer(
    const Asteria::Foundation::UInt64 deadlineTicks, const Asteria::Foundation::UInt64 interruptVector,
    const Asteria::Foundation::UInt64 rollbackDeadlineTicks, const Asteria::Foundation::UInt64 rollbackInterruptVector,
    const Asteria::Foundation::UInt64 maskedInterruptVector) noexcept
{
    ++Asteria::Test::g_programCount;
    Asteria::Test::g_programDeadline = deadlineTicks;
    Asteria::Test::g_programVector = interruptVector;
    Asteria::Test::g_programRollbackDeadline = rollbackDeadlineTicks;
    Asteria::Test::g_programRollbackVector = rollbackInterruptVector;
    Asteria::Test::g_programMaskedVector = maskedInterruptVector;
    return Asteria::Test::g_programStatus;
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64CancelLocalTimer(const Asteria::Foundation::UInt64 maskedInterruptVector) noexcept
{
    ++Asteria::Test::g_cancelCount;
    Asteria::Test::g_cancelArgument = maskedInterruptVector;
    return Asteria::Test::g_cancelStatus;
}

namespace Asteria::Test
{
namespace
{
using Hal::Time::LocalTimerError;
using Platform::Ia64::Time::Ia64LocalTimer;
using Platform::Ia64::Time::Ia64LocalTimerInitializationError;
using Platform::Ia64::Time::Ia64LocalTimerSnapshot;
using Platform::Ia64::Time::Ia64LocalTimerState;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaskBit{Foundation::UInt64{1U} << 16U};

void ResetAdapter() noexcept
{
    g_ticks = 0U;
    g_readCount = 0U;
    g_initializeCount = 0U;
    g_programCount = 0U;
    g_cancelCount = 0U;
    g_initializeArgument = 0U;
    g_programDeadline = 0U;
    g_programVector = 0U;
    g_programRollbackDeadline = 0U;
    g_programRollbackVector = 0U;
    g_programMaskedVector = 0U;
    g_cancelArgument = 0U;
    g_initializeStatus = 0U;
    g_programStatus = 0U;
    g_cancelStatus = 0U;
}

[[nodiscard]] bool BytesAreZero(const Foundation::UInt8* const bytes, const Foundation::Size size) noexcept
{
    for (Foundation::Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool SnapshotIs(const Ia64LocalTimerSnapshot& snapshot, const Ia64LocalTimerState state,
                              const Foundation::UInt8 vector, const Foundation::UInt64 deadlineTicks,
                              const bool armed) noexcept
{
    return (snapshot.State == state) && (snapshot.Vector == vector) &&
           (snapshot.ProgrammedDeadlineTicks == deadlineTicks) &&
           (snapshot.Armed == static_cast<Foundation::UInt8>(armed ? 1U : 0U)) &&
           BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

void TestInitialization(TestContext& runner) noexcept
{
    ResetAdapter();
    Ia64LocalTimer timer;
    ASTERIA_EXPECT(runner, "an uninitialized timer is inert and rejects programming",
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Uninitialized, 0U, 0U, false),
                   timer.ProgramDeadline(20U) == LocalTimerError::NotReady,
                   timer.CancelDeadline() == LocalTimerError::NotReady,
                   timer.AcknowledgeExternallyMaskedDeadline(20U) == LocalTimerError::NotReady, g_readCount == 0U,
                   g_initializeCount == 0U);

    ASTERIA_EXPECT(runner, "architecturally reserved vectors are rejected before register access",
                   timer.Initialize(0U) == Ia64LocalTimerInitializationError::InvalidVector,
                   timer.Initialize(15U) == Ia64LocalTimerInitializationError::InvalidVector,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Uninitialized, 0U, 0U, false),
                   g_initializeCount == 0U);

    constexpr Foundation::UInt8 Vector{0xE8U};
    ASTERIA_EXPECT(runner, "initialization masks one valid external vector exactly once",
                   timer.Initialize(Vector) == Ia64LocalTimerInitializationError::None,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 0U, false),
                   g_initializeCount == 1U, g_initializeArgument == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   timer.Initialize(Vector) == Ia64LocalTimerInitializationError::AlreadyInitialized,
                   g_initializeCount == 1U);

    g_ticks = 1234U;
    ASTERIA_EXPECT(runner, "GetTicks reads the native interval time counter adapter", timer.GetTicks() == 1234U,
                   g_readCount == 1U);
}

void TestExternalMaskAcknowledgement(TestContext& runner) noexcept
{
    ResetAdapter();
    constexpr Foundation::UInt8 Vector{0xE8U};
    Ia64LocalTimer timer;
    const auto initialized = timer.Initialize(Vector);
    g_ticks = 10U;
    const auto armed = timer.ProgramDeadline(40U);
    const Foundation::Size reads = g_readCount;
    const Foundation::Size programs = g_programCount;
    const Foundation::Size cancels = g_cancelCount;

    ASTERIA_EXPECT(runner, "external acknowledgement rejects a non-exact arm without register access",
                   initialized == Ia64LocalTimerInitializationError::None, armed == LocalTimerError::None,
                   timer.AcknowledgeExternallyMaskedDeadline(0U) == LocalTimerError::InvalidDeadline,
                   timer.AcknowledgeExternallyMaskedDeadline(41U) == LocalTimerError::InvalidDeadline,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 40U, true), g_readCount == reads,
                   g_programCount == programs, g_cancelCount == cancels);

    ASTERIA_EXPECT(runner, "external acknowledgement clears only the exact software arm without a hardware write",
                   timer.AcknowledgeExternallyMaskedDeadline(40U) == LocalTimerError::None,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 0U, false),
                   timer.AcknowledgeExternallyMaskedDeadline(40U) == LocalTimerError::InvalidDeadline,
                   g_readCount == reads, g_programCount == programs, g_cancelCount == cancels);
}

void TestProgramming(TestContext& runner) noexcept
{
    ResetAdapter();
    constexpr Foundation::UInt8 Vector{0xD1U};
    Ia64LocalTimer timer;
    const auto initialized = timer.Initialize(Vector);
    g_ticks = 100U;

    ASTERIA_EXPECT(runner, "zero and infinite deadlines are rejected without touching hardware",
                   initialized == Ia64LocalTimerInitializationError::None,
                   timer.ProgramDeadline(0U) == LocalTimerError::InvalidDeadline,
                   timer.ProgramDeadline(MaximumUInt64) == LocalTimerError::InvalidDeadline, g_readCount == 0U,
                   g_programCount == 0U,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 0U, false));

    ASTERIA_EXPECT(runner, "deadlines at or before ITC are returned for synchronous service",
                   timer.ProgramDeadline(100U) == LocalTimerError::DeadlineReached,
                   timer.ProgramDeadline(99U) == LocalTimerError::DeadlineReached, g_readCount == 2U,
                   g_programCount == 0U,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 0U, false));

    ASTERIA_EXPECT(runner, "a future deadline programs ITM and unmasks the configured ITV vector",
                   timer.ProgramDeadline(150U) == LocalTimerError::None, g_programCount == 1U,
                   g_programDeadline == 150U, g_programVector == Vector, g_programRollbackDeadline == 0U,
                   g_programRollbackVector == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   g_programMaskedVector == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 150U, true));

    ASTERIA_EXPECT(runner, "a later authoritative deadline replaces an existing arm",
                   timer.ProgramDeadline(200U) == LocalTimerError::None, g_programCount == 2U,
                   g_programDeadline == 200U,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 200U, true));

    ASTERIA_EXPECT(runner, "a rejected replacement preserves the existing hardware arm",
                   timer.ProgramDeadline(MaximumUInt64) == LocalTimerError::InvalidDeadline,
                   timer.ProgramDeadline(100U) == LocalTimerError::DeadlineReached, g_programCount == 2U,
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 200U, true));

    g_programStatus = 2U;
    ASTERIA_EXPECT(runner, "a deadline crossed during register programming restores the prior arm",
                   timer.ProgramDeadline(250U) == LocalTimerError::DeadlineReached, g_programCount == 3U,
                   g_programRollbackDeadline == 200U, g_programRollbackVector == Vector,
                   g_programMaskedVector == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 200U, true));
    g_programStatus = 0U;

    ASTERIA_EXPECT(runner, "cancellation masks ITV and is idempotent after disarming",
                   timer.CancelDeadline() == LocalTimerError::None, g_cancelCount == 1U,
                   g_cancelArgument == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 0U, false),
                   timer.CancelDeadline() == LocalTimerError::None, g_cancelCount == 1U);
}

void TestFailures(TestContext& runner) noexcept
{
    ResetAdapter();
    g_initializeStatus = 1U;
    Ia64LocalTimer initializationFailure;
    ASTERIA_EXPECT(runner, "an initialization readback failure makes the timer terminal",
                   initializationFailure.Initialize(0xE0U) == Ia64LocalTimerInitializationError::HardwareFailure,
                   SnapshotIs(initializationFailure.GetSnapshot(), Ia64LocalTimerState::Faulted, 0U, 0U, false),
                   initializationFailure.ProgramDeadline(20U) == LocalTimerError::HardwareFailure,
                   initializationFailure.CancelDeadline() == LocalTimerError::HardwareFailure, g_programCount == 0U,
                   g_cancelCount == 0U);

    ResetAdapter();
    Ia64LocalTimer programmingFailure;
    const auto programmingInitialized = programmingFailure.Initialize(0xE1U);
    g_ticks = 10U;
    const auto firstArm = programmingFailure.ProgramDeadline(20U);
    // Native status 3 means even the forced-mask readback could not be
    // confirmed; the C++ owner must remain terminal and preserve metadata.
    g_programStatus = 3U;
    const auto failedReplacement = programmingFailure.ProgramDeadline(30U);
    ASTERIA_EXPECT(runner, "a programming failure retains metadata and retries a best-effort mask",
                   programmingInitialized == Ia64LocalTimerInitializationError::None, firstArm == LocalTimerError::None,
                   failedReplacement == LocalTimerError::HardwareFailure,
                   SnapshotIs(programmingFailure.GetSnapshot(), Ia64LocalTimerState::Faulted, 0xE1U, 20U, true),
                   programmingFailure.ProgramDeadline(40U) == LocalTimerError::HardwareFailure,
                   programmingFailure.CancelDeadline() == LocalTimerError::HardwareFailure, g_programCount == 2U,
                   g_cancelCount == 1U, g_cancelArgument == (Foundation::UInt64{0xE1U} | MaskBit),
                   SnapshotIs(programmingFailure.GetSnapshot(), Ia64LocalTimerState::Faulted, 0xE1U, 20U, true));

    ResetAdapter();
    Ia64LocalTimer cancellationFailure;
    const auto cancellationInitialized = cancellationFailure.Initialize(0xE2U);
    g_ticks = 10U;
    const auto cancellationArmed = cancellationFailure.ProgramDeadline(20U);
    g_cancelStatus = 1U;
    const auto failedCancellation = cancellationFailure.CancelDeadline();
    ASTERIA_EXPECT(runner, "a cancellation failure retains the arm and retries the mask once per call",
                   cancellationInitialized == Ia64LocalTimerInitializationError::None,
                   cancellationArmed == LocalTimerError::None, failedCancellation == LocalTimerError::HardwareFailure,
                   SnapshotIs(cancellationFailure.GetSnapshot(), Ia64LocalTimerState::Faulted, 0xE2U, 20U, true),
                   cancellationFailure.CancelDeadline() == LocalTimerError::HardwareFailure, g_cancelCount == 2U,
                   SnapshotIs(cancellationFailure.GetSnapshot(), Ia64LocalTimerState::Faulted, 0xE2U, 20U, true));

    ResetAdapter();
    Ia64LocalTimer exhaustedClock;
    const auto exhaustedInitialized = exhaustedClock.Initialize(0xE3U);
    g_ticks = MaximumUInt64;
    ASTERIA_EXPECT(runner, "a terminal ITC fault also permits one best-effort mask retry",
                   exhaustedInitialized == Ia64LocalTimerInitializationError::None,
                   exhaustedClock.ProgramDeadline(20U) == LocalTimerError::HardwareFailure,
                   SnapshotIs(exhaustedClock.GetSnapshot(), Ia64LocalTimerState::Faulted, 0xE3U, 0U, false),
                   exhaustedClock.CancelDeadline() == LocalTimerError::HardwareFailure, g_programCount == 0U,
                   g_cancelCount == 1U);
}

void TestCrossedFirstArm(TestContext& runner) noexcept
{
    ResetAdapter();
    constexpr Foundation::UInt8 Vector{0xD2U};
    Ia64LocalTimer timer;
    const auto initialized = timer.Initialize(Vector);
    g_ticks = 100U;
    g_programStatus = 2U;

    ASTERIA_EXPECT(runner, "a crossed first arm restores the masked disarmed hardware state",
                   initialized == Ia64LocalTimerInitializationError::None,
                   timer.ProgramDeadline(150U) == LocalTimerError::DeadlineReached, g_programCount == 1U,
                   g_programRollbackDeadline == 0U,
                   g_programRollbackVector == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   g_programMaskedVector == (static_cast<Foundation::UInt64>(Vector) | MaskBit),
                   SnapshotIs(timer.GetSnapshot(), Ia64LocalTimerState::Ready, Vector, 0U, false));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Ia64LocalTimerTests"};
    Asteria::Test::TestInitialization(runner);
    Asteria::Test::TestProgramming(runner);
    Asteria::Test::TestExternalMaskAcknowledgement(runner);
    Asteria::Test::TestFailures(runner);
    Asteria::Test::TestCrossedFirstArm(runner);
    return runner.Finish();
}
