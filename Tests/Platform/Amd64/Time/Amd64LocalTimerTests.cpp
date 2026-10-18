// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Platform/Amd64/Time/Amd64LocalTimer.hpp"

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

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ReadTimeStampCounter() noexcept
{
    ++Asteria::Test::g_readCount;
    return Asteria::Test::g_ticks;
}

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64InitializeLocalTimer(
    const Asteria::Foundation::UInt64 maskedTimerVector,
    [[maybe_unused]] Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept
{
    ++Asteria::Test::g_initializeCount;
    Asteria::Test::g_initializeArgument = maskedTimerVector;
    return Asteria::Test::g_initializeStatus;
}

extern "C" Asteria::Foundation::UInt64 AsteriaAmd64ProgramLocalTimer(
    const Asteria::Foundation::UInt64 deadlineTicks, const Asteria::Foundation::UInt64 timerVector,
    const Asteria::Foundation::UInt64 rollbackDeadlineTicks, const Asteria::Foundation::UInt64 rollbackTimerVector,
    const Asteria::Foundation::UInt64 maskedTimerVector,
    [[maybe_unused]] Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept
{
    ++Asteria::Test::g_programCount;
    Asteria::Test::g_programDeadline = deadlineTicks;
    Asteria::Test::g_programVector = timerVector;
    Asteria::Test::g_programRollbackDeadline = rollbackDeadlineTicks;
    Asteria::Test::g_programRollbackVector = rollbackTimerVector;
    Asteria::Test::g_programMaskedVector = maskedTimerVector;
    return Asteria::Test::g_programStatus;
}

extern "C" Asteria::Foundation::UInt64
AsteriaAmd64CancelLocalTimer(const Asteria::Foundation::UInt64 maskedTimerVector,
                             [[maybe_unused]] Asteria::Platform::Amd64::Interrupt::Amd64LocalApic* apicCache) noexcept
{
    ++Asteria::Test::g_cancelCount;
    Asteria::Test::g_cancelArgument = maskedTimerVector;
    return Asteria::Test::g_cancelStatus;
}

namespace Asteria::Test
{
namespace
{
using Hal::Time::LocalTimerError;
using Platform::Amd64::Time::Amd64LocalTimer;
using Platform::Amd64::Time::Amd64LocalTimerInitializationError;
using Platform::Amd64::Time::Amd64LocalTimerSnapshot;
using Platform::Amd64::Time::Amd64LocalTimerState;

constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaskBit{Foundation::UInt64{1U} << 16U};
constexpr Foundation::UInt64 DeadlineModeBit{Foundation::UInt64{1U} << 18U};

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

[[nodiscard]] bool SnapshotIs(const Amd64LocalTimerSnapshot& snapshot, const Amd64LocalTimerState state,
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
    Amd64LocalTimer timer;
    ASTERIA_EXPECT(runner, "an uninitialized timer is inert and rejects programming",
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Uninitialized, 0U, 0U, false),
                   timer.ProgramDeadline(20U) == LocalTimerError::NotReady,
                   timer.CancelDeadline() == LocalTimerError::NotReady,
                   timer.AcknowledgeExternallyMaskedDeadline(20U) == LocalTimerError::NotReady, g_readCount == 0U,
                   g_initializeCount == 0U);

    ASTERIA_EXPECT(runner, "exception vectors are rejected before hardware access",
                   timer.Initialize(0U) == Amd64LocalTimerInitializationError::InvalidVector,
                   timer.Initialize(31U) == Amd64LocalTimerInitializationError::InvalidVector,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Uninitialized, 0U, 0U, false),
                   g_initializeCount == 0U);

    constexpr Foundation::UInt8 Vector{0xE8U};
    ASTERIA_EXPECT(
        runner, "initialization configures one masked TSC-deadline vector",
        timer.Initialize(Vector) == Amd64LocalTimerInitializationError::None,
        SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 0U, false), g_initializeCount == 1U,
        g_initializeArgument == (static_cast<Foundation::UInt64>(Vector) | DeadlineModeBit | MaskBit),
        timer.Initialize(Vector) == Amd64LocalTimerInitializationError::AlreadyInitialized, g_initializeCount == 1U);

    g_ticks = 1234U;
    ASTERIA_EXPECT(runner, "GetTicks reads the ordered time-stamp counter adapter", timer.GetTicks() == 1234U,
                   g_readCount == 1U);
}

void TestExternalMaskAcknowledgement(TestContext& runner) noexcept
{
    ResetAdapter();
    constexpr Foundation::UInt8 Vector{0xE8U};
    Amd64LocalTimer timer;
    const auto initialized = timer.Initialize(Vector);
    g_ticks = 10U;
    const auto armed = timer.ProgramDeadline(40U);
    const Foundation::Size reads = g_readCount;
    const Foundation::Size programs = g_programCount;
    const Foundation::Size cancels = g_cancelCount;

    ASTERIA_EXPECT(runner, "external acknowledgement rejects a non-exact arm without register access",
                   initialized == Amd64LocalTimerInitializationError::None, armed == LocalTimerError::None,
                   timer.AcknowledgeExternallyMaskedDeadline(0U) == LocalTimerError::InvalidDeadline,
                   timer.AcknowledgeExternallyMaskedDeadline(41U) == LocalTimerError::InvalidDeadline,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 40U, true),
                   g_readCount == reads, g_programCount == programs, g_cancelCount == cancels);

    ASTERIA_EXPECT(runner, "external acknowledgement clears only the exact software arm without a hardware write",
                   timer.AcknowledgeExternallyMaskedDeadline(40U) == LocalTimerError::None,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 0U, false),
                   timer.AcknowledgeExternallyMaskedDeadline(40U) == LocalTimerError::InvalidDeadline,
                   g_readCount == reads, g_programCount == programs, g_cancelCount == cancels);
}

void TestProgramming(TestContext& runner) noexcept
{
    ResetAdapter();
    constexpr Foundation::UInt8 Vector{0xD1U};
    constexpr Foundation::UInt64 UnmaskedVector{static_cast<Foundation::UInt64>(Vector) | DeadlineModeBit};
    constexpr Foundation::UInt64 MaskedVector{UnmaskedVector | MaskBit};
    Amd64LocalTimer timer;
    const auto initialized = timer.Initialize(Vector);
    g_ticks = 100U;

    ASTERIA_EXPECT(runner, "zero and infinite deadlines are rejected without hardware access",
                   initialized == Amd64LocalTimerInitializationError::None,
                   timer.ProgramDeadline(0U) == LocalTimerError::InvalidDeadline,
                   timer.ProgramDeadline(MaximumUInt64) == LocalTimerError::InvalidDeadline, g_readCount == 0U,
                   g_programCount == 0U,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 0U, false));

    ASTERIA_EXPECT(runner, "deadlines at or before TSC are handled synchronously",
                   timer.ProgramDeadline(100U) == LocalTimerError::DeadlineReached,
                   timer.ProgramDeadline(99U) == LocalTimerError::DeadlineReached, g_readCount == 2U,
                   g_programCount == 0U,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 0U, false));

    ASTERIA_EXPECT(runner, "a future deadline programs the MSR and unmasks its LVT entry",
                   timer.ProgramDeadline(150U) == LocalTimerError::None, g_programCount == 1U,
                   g_programDeadline == 150U, g_programVector == UnmaskedVector, g_programRollbackDeadline == 0U,
                   g_programRollbackVector == MaskedVector, g_programMaskedVector == MaskedVector,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 150U, true));

    ASTERIA_EXPECT(runner, "a later authoritative deadline replaces an existing arm",
                   timer.ProgramDeadline(200U) == LocalTimerError::None, g_programCount == 2U,
                   g_programDeadline == 200U, g_programRollbackDeadline == 150U,
                   g_programRollbackVector == UnmaskedVector,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 200U, true));

    g_programStatus = 2U;
    ASTERIA_EXPECT(runner, "a deadline crossed while programming restores the previous arm",
                   timer.ProgramDeadline(250U) == LocalTimerError::DeadlineReached, g_programCount == 3U,
                   g_programRollbackDeadline == 200U, g_programRollbackVector == UnmaskedVector,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 200U, true));
    g_programStatus = 0U;

    ASTERIA_EXPECT(runner, "cancellation masks LVT, clears the deadline, and is idempotent",
                   timer.CancelDeadline() == LocalTimerError::None, g_cancelCount == 1U,
                   g_cancelArgument == MaskedVector,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 0U, false),
                   timer.CancelDeadline() == LocalTimerError::None, g_cancelCount == 1U);
}

void TestFailures(TestContext& runner) noexcept
{
    constexpr Foundation::UInt8 Vector{0xE0U};
    const Amd64LocalTimerInitializationError expectedErrors[] = {
        Amd64LocalTimerInitializationError::RequiredFeatureUnavailable,
        Amd64LocalTimerInitializationError::LocalApicUnavailable,
        Amd64LocalTimerInitializationError::HardwareFailure,
    };
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        ResetAdapter();
        g_initializeStatus = static_cast<Foundation::UInt64>(index + 1U);
        Amd64LocalTimer timer;
        ASTERIA_EXPECT(runner, "initialization failures map exactly and make the timer terminal",
                       timer.Initialize(Vector) == expectedErrors[index],
                       SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Faulted, 0U, 0U, false),
                       timer.ProgramDeadline(20U) == LocalTimerError::HardwareFailure,
                       timer.CancelDeadline() == LocalTimerError::HardwareFailure);
    }

    ResetAdapter();
    Amd64LocalTimer programmingFailure;
    const auto programmingInitialized = programmingFailure.Initialize(0xE1U);
    g_ticks = 10U;
    const auto firstArm = programmingFailure.ProgramDeadline(20U);
    g_programStatus = 1U;
    const auto failedReplacement = programmingFailure.ProgramDeadline(30U);
    ASTERIA_EXPECT(runner, "a programming readback failure retains prior diagnostic metadata",
                   programmingInitialized == Amd64LocalTimerInitializationError::None,
                   firstArm == LocalTimerError::None, failedReplacement == LocalTimerError::HardwareFailure,
                   SnapshotIs(programmingFailure.GetSnapshot(), Amd64LocalTimerState::Faulted, 0xE1U, 20U, true),
                   g_programCount == 2U);

    ResetAdapter();
    Amd64LocalTimer cancellationFailure;
    const auto cancellationInitialized = cancellationFailure.Initialize(0xE2U);
    g_ticks = 10U;
    const auto cancellationArmed = cancellationFailure.ProgramDeadline(20U);
    g_cancelStatus = 1U;
    const auto failedCancellation = cancellationFailure.CancelDeadline();
    ASTERIA_EXPECT(runner, "a cancellation readback failure retains the last known arm",
                   cancellationInitialized == Amd64LocalTimerInitializationError::None,
                   cancellationArmed == LocalTimerError::None, failedCancellation == LocalTimerError::HardwareFailure,
                   SnapshotIs(cancellationFailure.GetSnapshot(), Amd64LocalTimerState::Faulted, 0xE2U, 20U, true),
                   g_cancelCount == 1U);

    ResetAdapter();
    Amd64LocalTimer exhaustedClock;
    const auto exhaustedInitialized = exhaustedClock.Initialize(0xE3U);
    g_ticks = MaximumUInt64;
    ASTERIA_EXPECT(runner, "the reserved terminal TSC value faults before programming",
                   exhaustedInitialized == Amd64LocalTimerInitializationError::None,
                   exhaustedClock.ProgramDeadline(20U) == LocalTimerError::HardwareFailure,
                   SnapshotIs(exhaustedClock.GetSnapshot(), Amd64LocalTimerState::Faulted, 0xE3U, 0U, false),
                   g_programCount == 0U);
}

void TestCrossedFirstArm(TestContext& runner) noexcept
{
    ResetAdapter();
    constexpr Foundation::UInt8 Vector{0xD2U};
    constexpr Foundation::UInt64 MaskedVector{static_cast<Foundation::UInt64>(Vector) | DeadlineModeBit | MaskBit};
    Amd64LocalTimer timer;
    const auto initialized = timer.Initialize(Vector);
    g_ticks = 100U;
    g_programStatus = 2U;

    ASTERIA_EXPECT(runner, "a crossed first arm restores the masked disarmed hardware state",
                   initialized == Amd64LocalTimerInitializationError::None,
                   timer.ProgramDeadline(150U) == LocalTimerError::DeadlineReached, g_programRollbackDeadline == 0U,
                   g_programRollbackVector == MaskedVector, g_programMaskedVector == MaskedVector,
                   SnapshotIs(timer.GetSnapshot(), Amd64LocalTimerState::Ready, Vector, 0U, false));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"Amd64LocalTimerTests"};
    Asteria::Test::TestInitialization(runner);
    Asteria::Test::TestProgramming(runner);
    Asteria::Test::TestExternalMaskAcknowledgement(runner);
    Asteria::Test::TestFailures(runner);
    Asteria::Test::TestCrossedFirstArm(runner);
    return runner.Finish();
}
