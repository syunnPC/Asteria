// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Kernel/Time/ProcessorDeadlineController.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Hal::Time::ILocalTimer;
using Hal::Time::LocalTimerError;
using Kernel::Time::ProcessorDeadlineController;
using Kernel::Time::ProcessorDeadlineError;
using Kernel::Time::ProcessorDeadlineFailure;
using Kernel::Time::ProcessorDeadlineReceipt;
using Kernel::Time::ProcessorDeadlineSnapshot;
using Kernel::Time::ProcessorDeadlineSource;
using Kernel::Time::ProcessorDeadlineSources;
using Kernel::Time::ProcessorDeadlineState;

class MockLocalTimer final : public ILocalTimer
{
public:
    [[nodiscard]] Foundation::UInt64 GetTicks() const noexcept override
    {
        ++ReadCount;
        return Ticks;
    }

    [[nodiscard]] LocalTimerError ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        ++ProgramCount;
        LastProgrammedTicks = deadlineTicks;
        if (ProgramResult == LocalTimerError::DeadlineReached)
        {
            Ticks = ReachedTicks == 0U ? deadlineTicks : ReachedTicks;
        }
        if (ProgramResult == LocalTimerError::None)
        {
            ArmedTicks = deadlineTicks;
        }
        return ProgramResult;
    }

    [[nodiscard]] LocalTimerError CancelDeadline() noexcept override
    {
        ++CancelCount;
        if ((CancelResult == LocalTimerError::DeadlineReached) && (ReachedTicks != 0U))
        {
            Ticks = ReachedTicks;
        }
        if (CancelResult == LocalTimerError::None)
        {
            ArmedTicks = 0U;
        }
        return CancelResult;
    }

    [[nodiscard]] LocalTimerError
    AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept override
    {
        ++AcknowledgeCount;
        LastAcknowledgedTicks = deadlineTicks;
        if (AcknowledgeResult != LocalTimerError::None)
        {
            return AcknowledgeResult;
        }
        if ((deadlineTicks == 0U) || (deadlineTicks != ArmedTicks))
        {
            return LocalTimerError::InvalidDeadline;
        }
        ArmedTicks = 0U;
        return LocalTimerError::None;
    }

    mutable Foundation::UInt64 Ticks{0U};
    mutable Foundation::Size ReadCount{0U};
    Foundation::Size ProgramCount{0U};
    Foundation::Size CancelCount{0U};
    Foundation::Size AcknowledgeCount{0U};
    Foundation::UInt64 LastProgrammedTicks{0U};
    Foundation::UInt64 LastAcknowledgedTicks{0U};
    Foundation::UInt64 ArmedTicks{0U};
    Foundation::UInt64 ReachedTicks{0U};
    LocalTimerError ProgramResult{LocalTimerError::None};
    LocalTimerError CancelResult{LocalTimerError::None};
    LocalTimerError AcknowledgeResult{LocalTimerError::None};
};

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

[[nodiscard]] constexpr Foundation::UInt8 Mask(const ProcessorDeadlineSource source) noexcept
{
    return static_cast<Foundation::UInt8>(source);
}

[[nodiscard]] bool SnapshotIs(const ProcessorDeadlineSnapshot& snapshot, const ProcessorDeadlineState state,
                              const Foundation::UInt64 deadlineTicks, const Foundation::UInt8 sources) noexcept
{
    return (snapshot.State == state) && (snapshot.SelectedDeadlineTicks == deadlineTicks) &&
           (snapshot.SelectedSources == sources) && BytesAreZero(snapshot.Reserved, sizeof(snapshot.Reserved));
}

[[nodiscard]] bool ReceiptIs(const ProcessorDeadlineReceipt& receipt, const ProcessorDeadlineState state,
                             const Foundation::UInt64 deadlineTicks, const Foundation::UInt64 observedTicks,
                             const Foundation::UInt8 sources, const bool reprogrammed) noexcept
{
    return (receipt.State == state) && (receipt.SelectedDeadlineTicks == deadlineTicks) &&
           (receipt.ObservedTicks == observedTicks) && (receipt.SelectedSources == sources) &&
           (receipt.Reprogrammed == static_cast<Foundation::UInt8>(reprogrammed ? 1U : 0U)) &&
           BytesAreZero(receipt.Reserved, sizeof(receipt.Reserved));
}

[[nodiscard]] bool FailureIs(const ProcessorDeadlineFailure& failure, const ProcessorDeadlineError error,
                             const Foundation::UInt64 deadlineTicks, const Foundation::UInt64 observedTicks,
                             const Foundation::UInt8 sources,
                             const LocalTimerError hardwareError = LocalTimerError::None) noexcept
{
    return (failure.Error == error) && (failure.SelectedDeadlineTicks == deadlineTicks) &&
           (failure.ObservedTicks == observedTicks) && (failure.AffectedSources == sources) &&
           (failure.HardwareError == hardwareError) && BytesAreZero(failure.Reserved, sizeof(failure.Reserved));
}

void TestSelectionAndProgramming(TestContext& runner) noexcept
{
    MockLocalTimer timer;
    timer.Ticks = 10U;
    ProcessorDeadlineController controller{timer};
    ASTERIA_EXPECT(runner, "controller begins disarmed without touching hardware",
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));

    const auto empty = controller.Program({});
    ASTERIA_EXPECT(runner, "an empty source set leaves an already-disarmed timer untouched", empty.HasValue(),
                   ReceiptIs(empty.GetValue(), ProcessorDeadlineState::Disarmed, 0U, 10U, 0U, false),
                   timer.ProgramCount == 0U, timer.CancelCount == 0U);

    const auto earliest = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 50U,
        .TimerQueueTicks = 40U,
        .SchedulerRetryTicks = 60U,
    });
    ASTERIA_EXPECT(runner, "the earliest finite source is programmed in the shared tick domain", earliest.HasValue(),
                   ReceiptIs(earliest.GetValue(), ProcessorDeadlineState::Armed, 40U, 10U,
                             Mask(ProcessorDeadlineSource::TimerQueue), true),
                   timer.ProgramCount == 1U, timer.LastProgrammedTicks == 40U,
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Armed, 40U,
                              Mask(ProcessorDeadlineSource::TimerQueue)));

    const Foundation::UInt8 tiedSources = static_cast<Foundation::UInt8>(Mask(ProcessorDeadlineSource::RunStop) |
                                                                         Mask(ProcessorDeadlineSource::TimerQueue));
    const auto tied = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 40U,
        .SchedulerRetryTicks = 70U,
    });
    ASTERIA_EXPECT(runner, "equal boundaries merge their causes without redundant programming", tied.HasValue(),
                   ReceiptIs(tied.GetValue(), ProcessorDeadlineState::Armed, 40U, 10U, tiedSources, false),
                   timer.ProgramCount == 1U,
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Armed, 40U, tiedSources));

    const auto earlier = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 50U,
        .TimerQueueTicks = 40U,
        .SchedulerRetryTicks = 30U,
    });
    const auto later = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 70U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(runner, "both earlier and later authoritative boundaries replace the timer", earlier.HasValue(),
                   ReceiptIs(earlier.GetValue(), ProcessorDeadlineState::Armed, 30U, 10U,
                             Mask(ProcessorDeadlineSource::SchedulerRetry), true),
                   later.HasValue(),
                   ReceiptIs(later.GetValue(), ProcessorDeadlineState::Armed, 70U, 10U,
                             Mask(ProcessorDeadlineSource::RunStop), true),
                   timer.ProgramCount == 3U, timer.LastProgrammedTicks == 70U);

    const auto disarmed = controller.Program({});
    const auto stillDisarmed = controller.Program({});
    ASTERIA_EXPECT(
        runner, "removing the final source cancels hardware exactly once", disarmed.HasValue(),
        ReceiptIs(disarmed.GetValue(), ProcessorDeadlineState::Disarmed, 0U, 10U, 0U, true), stillDisarmed.HasValue(),
        ReceiptIs(stillDisarmed.GetValue(), ProcessorDeadlineState::Disarmed, 0U, 10U, 0U, false),
        timer.CancelCount == 1U, SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));
}

void TestValidationAndDueBoundaries(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 maximumUInt64{~Foundation::UInt64{0U}};
    MockLocalTimer timer;
    timer.Ticks = 25U;
    ProcessorDeadlineController controller{timer};

    const Foundation::UInt8 invalidSources = static_cast<Foundation::UInt8>(Mask(ProcessorDeadlineSource::RunStop) |
                                                                            Mask(ProcessorDeadlineSource::TimerQueue));
    const auto invalid = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = maximumUInt64,
        .TimerQueueTicks = maximumUInt64,
        .SchedulerRetryTicks = 30U,
    });
    ASTERIA_EXPECT(
        runner, "the reserved infinite timestamp is rejected before hardware access", !invalid.HasValue(),
        FailureIs(invalid.GetError(), ProcessorDeadlineError::InvalidDeadline, maximumUInt64, 0U, invalidSources),
        timer.ReadCount == 0U, timer.ProgramCount == 0U,
        SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));

    const auto invalidRetry = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 30U,
        .TimerQueueTicks = 40U,
        .SchedulerRetryTicks = maximumUInt64,
    });
    ASTERIA_EXPECT(runner, "the scheduler-retry source independently rejects the infinite timestamp",
                   !invalidRetry.HasValue(),
                   FailureIs(invalidRetry.GetError(), ProcessorDeadlineError::InvalidDeadline, maximumUInt64, 0U,
                             Mask(ProcessorDeadlineSource::SchedulerRetry)),
                   timer.ReadCount == 0U, timer.ProgramCount == 0U,
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));

    const auto due = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 25U,
        .TimerQueueTicks = 20U,
        .SchedulerRetryTicks = 40U,
    });
    ASTERIA_EXPECT(runner, "an already-due earliest source is returned for synchronous service", !due.HasValue(),
                   FailureIs(due.GetError(), ProcessorDeadlineError::DeadlineReached, 20U, 25U,
                             Mask(ProcessorDeadlineSource::TimerQueue)),
                   timer.ProgramCount == 0U,
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));

    timer.Ticks = 30U;
    timer.ProgramResult = LocalTimerError::DeadlineReached;
    timer.ReachedTicks = 43U;
    const auto crossed = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(runner, "a boundary crossed inside the HAL is reported without false arming", !crossed.HasValue(),
                   FailureIs(crossed.GetError(), ProcessorDeadlineError::DeadlineReached, 40U, 43U,
                             Mask(ProcessorDeadlineSource::RunStop), LocalTimerError::DeadlineReached),
                   timer.ProgramCount == 1U,
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));

    MockLocalTimer baselineTimer;
    baselineTimer.Ticks = 90U;
    ProcessorDeadlineController baselineController{baselineTimer};
    const auto baselineReversed = baselineController.Program(
        ProcessorDeadlineSources{
            .RunStopTicks = 120U,
            .TimerQueueTicks = 0U,
            .SchedulerRetryTicks = 0U,
        },
        100U);
    ASTERIA_EXPECT(runner, "a sample older than its caller baseline faults before programming",
                   !baselineReversed.HasValue(),
                   FailureIs(baselineReversed.GetError(), ProcessorDeadlineError::ClockReversed, 120U, 90U,
                             Mask(ProcessorDeadlineSource::RunStop)),
                   baselineTimer.ProgramCount == 0U,
                   SnapshotIs(baselineController.GetSnapshot(), ProcessorDeadlineState::Faulted, 0U, 0U));

    MockLocalTimer validationTimer;
    validationTimer.Ticks = 10U;
    ProcessorDeadlineController validationController{validationTimer};
    const auto validationArmed = validationController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    validationTimer.Ticks = 9U;
    const auto validationReversed =
        validationArmed.HasValue()
            ? validationController.ValidateCurrent(validationArmed.GetValue())
            : Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>::Failure({});
    ASTERIA_EXPECT(runner, "final validation faults when the timer moves behind its receipt",
                   validationArmed.HasValue(), !validationReversed.HasValue(),
                   FailureIs(validationReversed.GetError(), ProcessorDeadlineError::ClockReversed, 40U, 9U,
                             Mask(ProcessorDeadlineSource::RunStop)),
                   SnapshotIs(validationController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U,
                              Mask(ProcessorDeadlineSource::RunStop)));

    MockLocalTimer laterProgramTimer;
    laterProgramTimer.Ticks = 100U;
    ProcessorDeadlineController laterProgramController{laterProgramTimer};
    const auto laterProgramArmed = laterProgramController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 140U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    laterProgramTimer.Ticks = 90U;
    const auto laterProgramReversed = laterProgramController.Program({});
    ASTERIA_EXPECT(runner, "a later programming transaction cannot move behind the prior sample",
                   laterProgramArmed.HasValue(), !laterProgramReversed.HasValue(),
                   FailureIs(laterProgramReversed.GetError(), ProcessorDeadlineError::ClockReversed, 0U, 90U, 0U),
                   laterProgramTimer.CancelCount == 0U,
                   SnapshotIs(laterProgramController.GetSnapshot(), ProcessorDeadlineState::Faulted, 140U,
                              Mask(ProcessorDeadlineSource::RunStop)));
}

void TestArmedFailureAtomicity(TestContext& runner) noexcept
{
    constexpr Foundation::UInt64 maximumUInt64{~Foundation::UInt64{0U}};
    const Foundation::UInt8 oldSource = Mask(ProcessorDeadlineSource::RunStop);
    const Foundation::UInt8 replacementSource = Mask(ProcessorDeadlineSource::TimerQueue);

    MockLocalTimer invalidTimer;
    invalidTimer.Ticks = 10U;
    ProcessorDeadlineController invalidController{invalidTimer};
    const auto invalidArmed = invalidController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    const auto invalidReplacement = invalidController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 0U,
        .TimerQueueTicks = 30U,
        .SchedulerRetryTicks = maximumUInt64,
    });
    ASTERIA_EXPECT(runner, "invalid replacement input is rejected without erasing the previous arm",
                   invalidArmed.HasValue(), !invalidReplacement.HasValue(),
                   FailureIs(invalidReplacement.GetError(), ProcessorDeadlineError::InvalidDeadline, maximumUInt64, 0U,
                             Mask(ProcessorDeadlineSource::SchedulerRetry)),
                   invalidTimer.ReadCount == 1U, invalidTimer.ProgramCount == 1U,
                   SnapshotIs(invalidController.GetSnapshot(), ProcessorDeadlineState::Armed, 40U, oldSource));

    MockLocalTimer precheckTimer;
    precheckTimer.Ticks = 10U;
    ProcessorDeadlineController precheckController{precheckTimer};
    const auto precheckArmed = precheckController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    precheckTimer.Ticks = 35U;
    const auto precheckDue = precheckController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 0U,
        .TimerQueueTicks = 30U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(
        runner, "a pre-program due replacement leaves the previous arm authoritative", precheckArmed.HasValue(),
        !precheckDue.HasValue(),
        FailureIs(precheckDue.GetError(), ProcessorDeadlineError::DeadlineReached, 30U, 35U, replacementSource),
        precheckTimer.ProgramCount == 1U,
        SnapshotIs(precheckController.GetSnapshot(), ProcessorDeadlineState::Armed, 40U, oldSource));

    MockLocalTimer crossedTimer;
    crossedTimer.Ticks = 10U;
    ProcessorDeadlineController crossedController{crossedTimer};
    const auto crossedArmed = crossedController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    crossedTimer.ProgramResult = LocalTimerError::DeadlineReached;
    crossedTimer.ReachedTicks = 33U;
    const auto crossed = crossedController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 0U,
        .TimerQueueTicks = 30U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(runner, "a replacement crossed inside the HAL keeps the prior arm and reports the resample",
                   crossedArmed.HasValue(), !crossed.HasValue(),
                   FailureIs(crossed.GetError(), ProcessorDeadlineError::DeadlineReached, 30U, 33U, replacementSource,
                             LocalTimerError::DeadlineReached),
                   crossedTimer.ProgramCount == 2U,
                   SnapshotIs(crossedController.GetSnapshot(), ProcessorDeadlineState::Armed, 40U, oldSource));

    MockLocalTimer secondSampleTimer;
    secondSampleTimer.Ticks = 10U;
    ProcessorDeadlineController secondSampleController{secondSampleTimer};
    const auto secondSampleArmed = secondSampleController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    secondSampleTimer.ProgramResult = LocalTimerError::DeadlineReached;
    secondSampleTimer.ReachedTicks = maximumUInt64;
    const auto secondSampleExhausted = secondSampleController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 0U,
        .TimerQueueTicks = 30U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(runner, "clock exhaustion on the HAL resample faults without erasing the prior arm",
                   secondSampleArmed.HasValue(), !secondSampleExhausted.HasValue(),
                   FailureIs(secondSampleExhausted.GetError(), ProcessorDeadlineError::ClockExhausted, 30U,
                             maximumUInt64, replacementSource, LocalTimerError::DeadlineReached),
                   SnapshotIs(secondSampleController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U, oldSource));

    MockLocalTimer firstSampleTimer;
    firstSampleTimer.Ticks = 10U;
    ProcessorDeadlineController firstSampleController{firstSampleTimer};
    const auto firstSampleArmed = firstSampleController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    firstSampleTimer.Ticks = maximumUInt64;
    const auto firstSampleExhausted = firstSampleController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 0U,
        .TimerQueueTicks = 30U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(runner, "clock exhaustion before replacement faults without erasing the prior arm",
                   firstSampleArmed.HasValue(), !firstSampleExhausted.HasValue(),
                   FailureIs(firstSampleExhausted.GetError(), ProcessorDeadlineError::ClockExhausted, 30U,
                             maximumUInt64, replacementSource),
                   firstSampleTimer.ProgramCount == 1U,
                   SnapshotIs(firstSampleController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U, oldSource));

    MockLocalTimer replacementFailureTimer;
    replacementFailureTimer.Ticks = 10U;
    ProcessorDeadlineController replacementFailureController{replacementFailureTimer};
    const auto replacementFailureArmed = replacementFailureController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    replacementFailureTimer.ProgramResult = LocalTimerError::HardwareFailure;
    const auto replacementFailure = replacementFailureController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 0U,
        .TimerQueueTicks = 30U,
        .SchedulerRetryTicks = 0U,
    });
    const auto replacementFailureRetry = replacementFailureController.Program({});
    ASTERIA_EXPECT(
        runner, "a terminal replacement failure and every retry retain the prior arm diagnostically",
        replacementFailureArmed.HasValue(), !replacementFailure.HasValue(),
        FailureIs(replacementFailure.GetError(), ProcessorDeadlineError::HardwareRejected, 30U, 10U, replacementSource,
                  LocalTimerError::HardwareFailure),
        !replacementFailureRetry.HasValue(),
        FailureIs(replacementFailureRetry.GetError(), ProcessorDeadlineError::Faulted, 40U, 0U, oldSource),
        replacementFailureTimer.ProgramCount == 2U,
        SnapshotIs(replacementFailureController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U, oldSource));

    MockLocalTimer cancellationRaceTimer;
    cancellationRaceTimer.Ticks = 10U;
    ProcessorDeadlineController cancellationRaceController{cancellationRaceTimer};
    const auto cancellationRaceArmed = cancellationRaceController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    cancellationRaceTimer.Ticks = 30U;
    cancellationRaceTimer.CancelResult = LocalTimerError::DeadlineReached;
    cancellationRaceTimer.ReachedTicks = 45U;
    const auto cancellationRace = cancellationRaceController.Program({});
    cancellationRaceTimer.CancelResult = LocalTimerError::None;
    const auto cancellationRetry = cancellationRaceController.Program({});
    ASTERIA_EXPECT(runner, "a deadline reached while cancelling is serviced synchronously before a successful retry",
                   cancellationRaceArmed.HasValue(), !cancellationRace.HasValue(),
                   FailureIs(cancellationRace.GetError(), ProcessorDeadlineError::DeadlineReached, 40U, 45U, oldSource,
                             LocalTimerError::DeadlineReached),
                   cancellationRetry.HasValue(),
                   ReceiptIs(cancellationRetry.GetValue(), ProcessorDeadlineState::Disarmed, 0U, 45U, 0U, true),
                   cancellationRaceTimer.CancelCount == 2U,
                   SnapshotIs(cancellationRaceController.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U));

    MockLocalTimer cancellationExhaustedTimer;
    cancellationExhaustedTimer.Ticks = 10U;
    ProcessorDeadlineController cancellationExhaustedController{cancellationExhaustedTimer};
    const auto cancellationExhaustedArmed = cancellationExhaustedController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    cancellationExhaustedTimer.CancelResult = LocalTimerError::DeadlineReached;
    cancellationExhaustedTimer.ReachedTicks = maximumUInt64;
    const auto cancellationExhausted = cancellationExhaustedController.Program({});
    ASTERIA_EXPECT(
        runner, "clock exhaustion after a cancellation race faults without erasing the prior arm",
        cancellationExhaustedArmed.HasValue(), !cancellationExhausted.HasValue(),
        FailureIs(cancellationExhausted.GetError(), ProcessorDeadlineError::ClockExhausted, 40U, maximumUInt64,
                  oldSource, LocalTimerError::DeadlineReached),
        SnapshotIs(cancellationExhaustedController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U, oldSource));
}

void TestExternallyMaskedExpiration(TestContext& runner) noexcept
{
    constexpr Foundation::UInt8 RunStopSource{Mask(ProcessorDeadlineSource::RunStop)};

    MockLocalTimer timer;
    timer.Ticks = 10U;
    ProcessorDeadlineController controller{timer};
    const auto armed = controller.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    ProcessorDeadlineReceipt foreign = armed.GetValue();
    foreign.SelectedDeadlineTicks = 41U;
    const auto foreignResult = controller.ConsumeExternallyMaskedExpiration(foreign, 40U);
    const auto premature = controller.ConsumeExternallyMaskedExpiration(armed.GetValue(), 39U);

    ASTERIA_EXPECT(
        runner, "external expiration mismatches preserve the exact armed transaction", armed.HasValue(),
        !foreignResult.HasValue(),
        FailureIs(foreignResult.GetError(), ProcessorDeadlineError::InvalidDeadline, 40U, 40U, RunStopSource),
        !premature.HasValue(),
        FailureIs(premature.GetError(), ProcessorDeadlineError::InvalidDeadline, 40U, 39U, RunStopSource),
        SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Armed, 40U, RunStopSource),
        timer.AcknowledgeCount == 0U, timer.ArmedTicks == 40U);

    const Foundation::Size readCount = timer.ReadCount;
    const Foundation::Size programCount = timer.ProgramCount;
    const Foundation::Size cancelCount = timer.CancelCount;
    const auto consumed = controller.ConsumeExternallyMaskedExpiration(armed.GetValue(), 43U);
    ASTERIA_EXPECT(runner,
                   "external expiration consumes backend and controller software state "
                   "without a hardware write or clock read",
                   consumed.HasValue(),
                   ReceiptIs(consumed.GetValue(), ProcessorDeadlineState::Disarmed, 0U, 43U, 0U, false),
                   SnapshotIs(controller.GetSnapshot(), ProcessorDeadlineState::Disarmed, 0U, 0U),
                   timer.AcknowledgeCount == 1U, timer.LastAcknowledgedTicks == 40U, timer.ArmedTicks == 0U,
                   timer.ReadCount == readCount, timer.ProgramCount == programCount, timer.CancelCount == cancelCount);

    MockLocalTimer rejectedTimer;
    rejectedTimer.Ticks = 10U;
    ProcessorDeadlineController rejectedController{rejectedTimer};
    const auto rejectedArmed = rejectedController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 40U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    rejectedTimer.AcknowledgeResult = LocalTimerError::InvalidDeadline;
    const auto rejected = rejectedController.ConsumeExternallyMaskedExpiration(rejectedArmed.GetValue(), 40U);
    ASTERIA_EXPECT(runner,
                   "a backend acknowledgement contradiction faults while retaining the "
                   "armed deadline diagnostically",
                   rejectedArmed.HasValue(), !rejected.HasValue(),
                   FailureIs(rejected.GetError(), ProcessorDeadlineError::HardwareRejected, 40U, 40U, RunStopSource,
                             LocalTimerError::InvalidDeadline),
                   SnapshotIs(rejectedController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U, RunStopSource),
                   rejectedTimer.ArmedTicks == 40U);
}

void TestTerminalHardwareFailures(TestContext& runner) noexcept
{
    MockLocalTimer programmingTimer;
    programmingTimer.Ticks = 10U;
    programmingTimer.ProgramResult = LocalTimerError::HardwareFailure;
    ProcessorDeadlineController programmingController{programmingTimer};
    const auto programmingFailure = programmingController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 20U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    const auto programmingRetry = programmingController.Program({});
    ASTERIA_EXPECT(runner, "a programming failure makes the controller terminally fail-stop",
                   !programmingFailure.HasValue(),
                   FailureIs(programmingFailure.GetError(), ProcessorDeadlineError::HardwareRejected, 20U, 10U,
                             Mask(ProcessorDeadlineSource::RunStop), LocalTimerError::HardwareFailure),
                   !programmingRetry.HasValue(),
                   FailureIs(programmingRetry.GetError(), ProcessorDeadlineError::Faulted, 0U, 0U, 0U),
                   SnapshotIs(programmingController.GetSnapshot(), ProcessorDeadlineState::Faulted, 0U, 0U),
                   programmingTimer.ProgramCount == 1U, programmingTimer.CancelCount == 0U);

    MockLocalTimer cancellationTimer;
    cancellationTimer.Ticks = 10U;
    ProcessorDeadlineController cancellationController{cancellationTimer};
    const auto armed = cancellationController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 20U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    cancellationTimer.CancelResult = LocalTimerError::HardwareFailure;
    const auto cancellationFailure = cancellationController.Program({});
    ASTERIA_EXPECT(runner, "a cancellation failure retains the last known deadline diagnostically", armed.HasValue(),
                   !cancellationFailure.HasValue(),
                   FailureIs(cancellationFailure.GetError(), ProcessorDeadlineError::HardwareRejected, 20U, 10U,
                             Mask(ProcessorDeadlineSource::RunStop), LocalTimerError::HardwareFailure),
                   SnapshotIs(cancellationController.GetSnapshot(), ProcessorDeadlineState::Faulted, 20U,
                              Mask(ProcessorDeadlineSource::RunStop)),
                   cancellationTimer.CancelCount == 1U);

    MockLocalTimer exhaustedTimer;
    exhaustedTimer.Ticks = ~Foundation::UInt64{0U};
    ProcessorDeadlineController exhaustedController{exhaustedTimer};
    const auto exhausted = exhaustedController.Program(ProcessorDeadlineSources{
        .RunStopTicks = 20U,
        .TimerQueueTicks = 0U,
        .SchedulerRetryTicks = 0U,
    });
    ASTERIA_EXPECT(runner, "a broken clock contract faults before publishing an unobservable arm",
                   !exhausted.HasValue(),
                   FailureIs(exhausted.GetError(), ProcessorDeadlineError::ClockExhausted, 20U, ~Foundation::UInt64{0U},
                             Mask(ProcessorDeadlineSource::RunStop)),
                   SnapshotIs(exhaustedController.GetSnapshot(), ProcessorDeadlineState::Faulted, 0U, 0U),
                   exhaustedTimer.ProgramCount == 0U);
}

void TestTerminalSoftFailures(TestContext& runner) noexcept
{
    constexpr LocalTimerError softErrors[]{
        LocalTimerError::NotReady,
        LocalTimerError::InvalidDeadline,
    };
    for (const LocalTimerError softError : softErrors)
    {
        MockLocalTimer programmingTimer;
        programmingTimer.Ticks = 10U;
        programmingTimer.ProgramResult = softError;
        ProcessorDeadlineController programmingController{programmingTimer};
        const auto programmingFailure = programmingController.Program(ProcessorDeadlineSources{
            .RunStopTicks = 20U,
            .TimerQueueTicks = 0U,
            .SchedulerRetryTicks = 0U,
        });
        const auto programmingRetry = programmingController.Program({});
        ASTERIA_EXPECT(runner, "a soft programming rejection is terminal because the new boundary was not installed",
                       !programmingFailure.HasValue(),
                       FailureIs(programmingFailure.GetError(), ProcessorDeadlineError::HardwareRejected, 20U, 10U,
                                 Mask(ProcessorDeadlineSource::RunStop), softError),
                       !programmingRetry.HasValue(),
                       programmingRetry.GetError().Error == ProcessorDeadlineError::Faulted,
                       SnapshotIs(programmingController.GetSnapshot(), ProcessorDeadlineState::Faulted, 0U, 0U),
                       programmingTimer.ProgramCount == 1U);

        MockLocalTimer replacementTimer;
        replacementTimer.Ticks = 10U;
        ProcessorDeadlineController replacementController{replacementTimer};
        const auto replacementArmed = replacementController.Program(ProcessorDeadlineSources{
            .RunStopTicks = 40U,
            .TimerQueueTicks = 0U,
            .SchedulerRetryTicks = 0U,
        });
        replacementTimer.ProgramResult = softError;
        const auto replacementFailure = replacementController.Program(ProcessorDeadlineSources{
            .RunStopTicks = 0U,
            .TimerQueueTicks = 30U,
            .SchedulerRetryTicks = 0U,
        });
        const auto replacementRetry = replacementController.Program({});
        ASTERIA_EXPECT(runner, "a soft replacement rejection faults while retaining the prior arm diagnostically",
                       replacementArmed.HasValue(), !replacementFailure.HasValue(),
                       FailureIs(replacementFailure.GetError(), ProcessorDeadlineError::HardwareRejected, 30U, 10U,
                                 Mask(ProcessorDeadlineSource::TimerQueue), softError),
                       !replacementRetry.HasValue(),
                       FailureIs(replacementRetry.GetError(), ProcessorDeadlineError::Faulted, 40U, 0U,
                                 Mask(ProcessorDeadlineSource::RunStop)),
                       SnapshotIs(replacementController.GetSnapshot(), ProcessorDeadlineState::Faulted, 40U,
                                  Mask(ProcessorDeadlineSource::RunStop)),
                       replacementTimer.ProgramCount == 2U);

        MockLocalTimer cancellationTimer;
        cancellationTimer.Ticks = 10U;
        ProcessorDeadlineController cancellationController{cancellationTimer};
        const auto armed = cancellationController.Program(ProcessorDeadlineSources{
            .RunStopTicks = 20U,
            .TimerQueueTicks = 0U,
            .SchedulerRetryTicks = 0U,
        });
        cancellationTimer.CancelResult = softError;
        const auto cancellationFailure = cancellationController.Program({});
        const auto cancellationRetry = cancellationController.Program({});
        ASTERIA_EXPECT(
            runner, "a soft cancellation rejection is terminal while retaining the prior boundary diagnostically",
            armed.HasValue(), !cancellationFailure.HasValue(),
            FailureIs(cancellationFailure.GetError(), ProcessorDeadlineError::HardwareRejected, 20U, 10U,
                      Mask(ProcessorDeadlineSource::RunStop), softError),
            !cancellationRetry.HasValue(), cancellationRetry.GetError().Error == ProcessorDeadlineError::Faulted,
            SnapshotIs(cancellationController.GetSnapshot(), ProcessorDeadlineState::Faulted, 20U,
                       Mask(ProcessorDeadlineSource::RunStop)),
            cancellationTimer.CancelCount == 1U);
    }
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ProcessorDeadlineControllerTests"};
    Asteria::Test::TestSelectionAndProgramming(runner);
    Asteria::Test::TestValidationAndDueBoundaries(runner);
    Asteria::Test::TestArmedFailureAtomicity(runner);
    Asteria::Test::TestExternallyMaskedExpiration(runner);
    Asteria::Test::TestTerminalHardwareFailures(runner);
    Asteria::Test::TestTerminalSoftFailures(runner);
    return runner.Finish();
}
