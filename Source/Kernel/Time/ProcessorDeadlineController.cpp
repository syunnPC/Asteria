// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Time/ProcessorDeadlineController.hpp"

namespace Asteria::Kernel::Time
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};

void IncludeDeadline(const Foundation::UInt64 deadlineTicks, const Foundation::UInt8 source,
                     Foundation::UInt64& selectedDeadlineTicks, Foundation::UInt8& selectedSources) noexcept
{
    if (deadlineTicks == 0U)
    {
        return;
    }
    if ((selectedDeadlineTicks == 0U) || (deadlineTicks < selectedDeadlineTicks))
    {
        selectedDeadlineTicks = deadlineTicks;
        selectedSources = source;
        return;
    }
    if (deadlineTicks == selectedDeadlineTicks)
    {
        selectedSources = static_cast<Foundation::UInt8>(selectedSources | source);
    }
}
} // namespace

ProcessorDeadlineFailure ProcessorDeadlineController::MakeFailure(
    const ProcessorDeadlineError error, const Foundation::UInt64 selectedDeadlineTicks,
    const Foundation::UInt64 observedTicks, const Foundation::UInt8 affectedSources,
    const Hal::Time::LocalTimerError hardwareError) noexcept
{
    return ProcessorDeadlineFailure{
        .SelectedDeadlineTicks = selectedDeadlineTicks,
        .ObservedTicks = observedTicks,
        .Error = error,
        .HardwareError = hardwareError,
        .AffectedSources = affectedSources,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    };
}

Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
ProcessorDeadlineController::ValidateCurrent(const ProcessorDeadlineReceipt& receipt,
                                             const Foundation::UInt64 minimumObservedTicks) noexcept
{
    using ResultType = Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>;

    if (m_state == ProcessorDeadlineState::Faulted)
    {
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::Faulted, m_selectedDeadlineTicks, 0U, m_selectedSources));
    }

    bool reservedIsZero{true};
    for (Foundation::Size index{0U}; index < sizeof(receipt.Reserved); ++index)
    {
        reservedIsZero = reservedIsZero && (receipt.Reserved[index] == 0U);
    }
    if ((m_state != ProcessorDeadlineState::Armed) || (m_selectedDeadlineTicks == 0U) || (m_selectedSources == 0U) ||
        (receipt.State != m_state) || (receipt.SelectedDeadlineTicks != m_selectedDeadlineTicks) ||
        (receipt.SelectedSources != m_selectedSources) || (receipt.ObservedTicks >= receipt.SelectedDeadlineTicks) ||
        (receipt.Reprogrammed > 1U) || !reservedIsZero)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::InvalidDeadline, m_selectedDeadlineTicks, 0U, m_selectedSources));
    }

    if (receipt.ObservedTicks < minimumObservedTicks)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockReversed, m_selectedDeadlineTicks,
                                               receipt.ObservedTicks, m_selectedSources));
    }

    const Foundation::UInt64 observedTicks = m_localTimer.GetTicks();
    if (observedTicks == MaximumUInt64)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockExhausted, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }
    if ((observedTicks < receipt.ObservedTicks) || (observedTicks < m_lastObservedTicks))
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockReversed, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }
    m_lastObservedTicks = observedTicks;
    if (m_selectedDeadlineTicks <= observedTicks)
    {
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::DeadlineReached, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }

    return ResultType::Success(ProcessorDeadlineReceipt{
        .SelectedDeadlineTicks = m_selectedDeadlineTicks,
        .ObservedTicks = observedTicks,
        .State = m_state,
        .SelectedSources = m_selectedSources,
        .Reprogrammed = 0U,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
ProcessorDeadlineController::ConsumeExternallyMaskedExpiration(const ProcessorDeadlineReceipt& receipt,
                                                               const Foundation::UInt64 observedTicks) noexcept
{
    using ResultType = Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>;

    if (m_state == ProcessorDeadlineState::Faulted)
    {
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::Faulted, m_selectedDeadlineTicks, observedTicks, m_selectedSources));
    }

    bool reservedIsZero{true};
    for (Foundation::Size index{0U}; index < sizeof(receipt.Reserved); ++index)
    {
        reservedIsZero = reservedIsZero && (receipt.Reserved[index] == 0U);
    }
    if ((m_state != ProcessorDeadlineState::Armed) || (m_selectedDeadlineTicks == 0U) ||
        (m_selectedDeadlineTicks == MaximumUInt64) || (m_selectedSources == 0U) ||
        (receipt.State != ProcessorDeadlineState::Armed) ||
        (receipt.SelectedDeadlineTicks != m_selectedDeadlineTicks) || (receipt.SelectedSources != m_selectedSources) ||
        (receipt.ObservedTicks >= receipt.SelectedDeadlineTicks) || (receipt.Reprogrammed > 1U) || !reservedIsZero)
    {
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::InvalidDeadline, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }
    if (observedTicks == MaximumUInt64)
    {
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockExhausted, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }
    if ((observedTicks < receipt.ObservedTicks) || (observedTicks < m_lastObservedTicks))
    {
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockReversed, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }
    if (observedTicks < m_selectedDeadlineTicks)
    {
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::InvalidDeadline, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources));
    }

    const Hal::Time::LocalTimerError hardwareError =
        m_localTimer.AcknowledgeExternallyMaskedDeadline(m_selectedDeadlineTicks);
    if (hardwareError != Hal::Time::LocalTimerError::None)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::HardwareRejected, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources, hardwareError));
    }

    m_lastObservedTicks = observedTicks;
    m_selectedDeadlineTicks = 0U;
    m_selectedSources = 0U;
    m_state = ProcessorDeadlineState::Disarmed;
    return ResultType::Success(ProcessorDeadlineReceipt{
        .SelectedDeadlineTicks = 0U,
        .ObservedTicks = observedTicks,
        .State = ProcessorDeadlineState::Disarmed,
        .SelectedSources = 0U,
        .Reprogrammed = 0U,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
ProcessorDeadlineController::FinishNoDeadlineProgram(const Foundation::UInt64 observedTicks) noexcept
{
    using ResultType = Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>;

    if (m_state == ProcessorDeadlineState::Disarmed)
    {
        return ResultType::Success(ProcessorDeadlineReceipt{
            .SelectedDeadlineTicks = 0U,
            .ObservedTicks = observedTicks,
            .State = ProcessorDeadlineState::Disarmed,
            .SelectedSources = 0U,
            .Reprogrammed = 0U,
            .Reserved = {0U, 0U, 0U, 0U, 0U},
        });
    }

    const Hal::Time::LocalTimerError hardwareError = m_localTimer.CancelDeadline();
    if (hardwareError == Hal::Time::LocalTimerError::DeadlineReached)
    {
        const Foundation::UInt64 laterObservedTicks = m_localTimer.GetTicks();
        if (laterObservedTicks == MaximumUInt64)
        {
            m_state = ProcessorDeadlineState::Faulted;
            return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockExhausted, m_selectedDeadlineTicks,
                                                   laterObservedTicks, m_selectedSources, hardwareError));
        }
        if (laterObservedTicks < m_lastObservedTicks)
        {
            m_state = ProcessorDeadlineState::Faulted;
            return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockReversed, m_selectedDeadlineTicks,
                                                   laterObservedTicks, m_selectedSources, hardwareError));
        }
        m_lastObservedTicks = laterObservedTicks;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::DeadlineReached, m_selectedDeadlineTicks,
                                               laterObservedTicks, m_selectedSources, hardwareError));
    }
    if (hardwareError != Hal::Time::LocalTimerError::None)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::HardwareRejected, m_selectedDeadlineTicks,
                                               observedTicks, m_selectedSources, hardwareError));
    }

    m_selectedDeadlineTicks = 0U;
    m_selectedSources = 0U;
    m_state = ProcessorDeadlineState::Disarmed;
    return ResultType::Success(ProcessorDeadlineReceipt{
        .SelectedDeadlineTicks = 0U,
        .ObservedTicks = observedTicks,
        .State = m_state,
        .SelectedSources = 0U,
        .Reprogrammed = 1U,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
ProcessorDeadlineController::FinishSelectedDeadlineProgram(const Foundation::UInt64 selectedDeadlineTicks,
                                                           const Foundation::UInt8 selectedSources,
                                                           const Foundation::UInt64 observedTicks) noexcept
{
    using ResultType = Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>;

    if ((m_state == ProcessorDeadlineState::Armed) && (m_selectedDeadlineTicks == selectedDeadlineTicks))
    {
        m_selectedSources = selectedSources;
        return ResultType::Success(ProcessorDeadlineReceipt{
            .SelectedDeadlineTicks = selectedDeadlineTicks,
            .ObservedTicks = observedTicks,
            .State = m_state,
            .SelectedSources = selectedSources,
            .Reprogrammed = 0U,
            .Reserved = {0U, 0U, 0U, 0U, 0U},
        });
    }

    const Hal::Time::LocalTimerError hardwareError = m_localTimer.ProgramDeadline(selectedDeadlineTicks);
    if (hardwareError == Hal::Time::LocalTimerError::DeadlineReached)
    {
        const Foundation::UInt64 laterObservedTicks = m_localTimer.GetTicks();
        if (laterObservedTicks == MaximumUInt64)
        {
            m_state = ProcessorDeadlineState::Faulted;
            return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockExhausted, selectedDeadlineTicks,
                                                   laterObservedTicks, selectedSources, hardwareError));
        }
        if (laterObservedTicks < m_lastObservedTicks)
        {
            m_state = ProcessorDeadlineState::Faulted;
            return ResultType::Failure(MakeFailure(ProcessorDeadlineError::ClockReversed, selectedDeadlineTicks,
                                                   laterObservedTicks, selectedSources, hardwareError));
        }
        m_lastObservedTicks = laterObservedTicks;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::DeadlineReached, selectedDeadlineTicks,
                                               laterObservedTicks, selectedSources, hardwareError));
    }
    if (hardwareError != Hal::Time::LocalTimerError::None)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::HardwareRejected, selectedDeadlineTicks,
                                               observedTicks, selectedSources, hardwareError));
    }

    m_selectedDeadlineTicks = selectedDeadlineTicks;
    m_selectedSources = selectedSources;
    m_state = ProcessorDeadlineState::Armed;
    return ResultType::Success(ProcessorDeadlineReceipt{
        .SelectedDeadlineTicks = selectedDeadlineTicks,
        .ObservedTicks = observedTicks,
        .State = m_state,
        .SelectedSources = selectedSources,
        .Reprogrammed = 1U,
        .Reserved = {0U, 0U, 0U, 0U, 0U},
    });
}

Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
ProcessorDeadlineController::Program(const ProcessorDeadlineSources& sources,
                                     const Foundation::UInt64 minimumObservedTicks) noexcept
{
    using ResultType = Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>;

    if (m_state == ProcessorDeadlineState::Faulted)
    {
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::Faulted, m_selectedDeadlineTicks, 0U, m_selectedSources));
    }

    Foundation::UInt8 invalidSources{0U};
    if (sources.RunStopTicks == MaximumUInt64)
    {
        invalidSources = static_cast<Foundation::UInt8>(invalidSources | SourceMask(ProcessorDeadlineSource::RunStop));
    }
    if (sources.TimerQueueTicks == MaximumUInt64)
    {
        invalidSources =
            static_cast<Foundation::UInt8>(invalidSources | SourceMask(ProcessorDeadlineSource::TimerQueue));
    }
    if (sources.SchedulerRetryTicks == MaximumUInt64)
    {
        invalidSources =
            static_cast<Foundation::UInt8>(invalidSources | SourceMask(ProcessorDeadlineSource::SchedulerRetry));
    }
    if (invalidSources != 0U)
    {
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::InvalidDeadline, MaximumUInt64, 0U, invalidSources));
    }

    Foundation::UInt64 selectedDeadlineTicks{0U};
    Foundation::UInt8 selectedSources{0U};
    IncludeDeadline(sources.RunStopTicks, SourceMask(ProcessorDeadlineSource::RunStop), selectedDeadlineTicks,
                    selectedSources);
    IncludeDeadline(sources.TimerQueueTicks, SourceMask(ProcessorDeadlineSource::TimerQueue), selectedDeadlineTicks,
                    selectedSources);
    IncludeDeadline(sources.SchedulerRetryTicks, SourceMask(ProcessorDeadlineSource::SchedulerRetry),
                    selectedDeadlineTicks, selectedSources);

    const Foundation::UInt64 observedTicks = m_localTimer.GetTicks();
    if (observedTicks == MaximumUInt64)
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::ClockExhausted, selectedDeadlineTicks, observedTicks, selectedSources));
    }
    if ((observedTicks < minimumObservedTicks) || (observedTicks < m_lastObservedTicks))
    {
        m_state = ProcessorDeadlineState::Faulted;
        return ResultType::Failure(
            MakeFailure(ProcessorDeadlineError::ClockReversed, selectedDeadlineTicks, observedTicks, selectedSources));
    }
    m_lastObservedTicks = observedTicks;
    if ((selectedDeadlineTicks != 0U) && (selectedDeadlineTicks <= observedTicks))
    {
        return ResultType::Failure(MakeFailure(ProcessorDeadlineError::DeadlineReached, selectedDeadlineTicks,
                                               observedTicks, selectedSources));
    }

    return selectedDeadlineTicks == 0U
               ? FinishNoDeadlineProgram(observedTicks)
               : FinishSelectedDeadlineProgram(selectedDeadlineTicks, selectedSources, observedTicks);
}
} // namespace Asteria::Kernel::Time
