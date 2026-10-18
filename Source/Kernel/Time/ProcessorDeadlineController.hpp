// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_TIME_PROCESSOR_DEADLINE_CONTROLLER_HPP_INCLUDED
#define ASTERIA_KERNEL_TIME_PROCESSOR_DEADLINE_CONTROLLER_HPP_INCLUDED

#include "Foundation/Result.hpp"
#include "Foundation/Types.hpp"
#include "Hal/Time/ILocalTimer.hpp"

namespace Asteria::Kernel::Time
{
enum class ProcessorDeadlineState : Foundation::UInt8
{
    Disarmed,
    Armed,
    Faulted,
};

enum class ProcessorDeadlineError : Foundation::UInt8
{
    None,
    InvalidDeadline,
    ClockExhausted,
    ClockReversed,
    DeadlineReached,
    HardwareRejected,
    Faulted,
};

enum class ProcessorDeadlineSource : Foundation::UInt8
{
    None = 0U,
    RunStop = 1U,
    TimerQueue = 2U,
    SchedulerRetry = 4U,
};

/// Zero means that the corresponding source has no pending deadline. Every
/// nonzero value is an absolute tick in the exact local-timer clock domain.
struct ProcessorDeadlineSources
{
    Foundation::UInt64 RunStopTicks;
    Foundation::UInt64 TimerQueueTicks;
    Foundation::UInt64 SchedulerRetryTicks;
};

struct ProcessorDeadlineFailure
{
    Foundation::UInt64 SelectedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    ProcessorDeadlineError Error;
    Hal::Time::LocalTimerError HardwareError;
    Foundation::UInt8 AffectedSources;
    Foundation::UInt8 Reserved[5];
};

struct ProcessorDeadlineReceipt
{
    Foundation::UInt64 SelectedDeadlineTicks;
    Foundation::UInt64 ObservedTicks;
    ProcessorDeadlineState State;
    Foundation::UInt8 SelectedSources;
    Foundation::UInt8 Reprogrammed;
    Foundation::UInt8 Reserved[5];
};

struct ProcessorDeadlineSnapshot
{
    Foundation::UInt64 SelectedDeadlineTicks;
    ProcessorDeadlineState State;
    Foundation::UInt8 SelectedSources;
    Foundation::UInt8 Reserved[6];
};

/// Selects and programs the earliest processor-local deadline from scheduler, TimerQueue, and
/// deferred-admission sources. Program is an O(1) processor-policy operation. The caller serializes
/// it on the owning CPU with local interrupts disabled; this class intentionally has no lock. The
/// supplied local timer is initialized and disarmed before this controller is constructed and
/// outlives it. A HardwareRejected result makes the controller terminally Faulted because the newly
/// authoritative boundary was not installed; HardwareFailure additionally means the interrupt
/// source can no longer be trusted.
class ProcessorDeadlineController final
{
public:
    explicit constexpr ProcessorDeadlineController(Hal::Time::ILocalTimer& localTimer) noexcept
        : m_localTimer{localTimer}
    {
    }

    /// Preconditions:
    /// - State is Disarmed and no operation is concurrent, or the owning CPU
    /// has entered a non-returning fail-stop/teardown path that independently
    /// prevents this interrupt source from executing.
    ~ProcessorDeadlineController() noexcept = default;

    ProcessorDeadlineController(const ProcessorDeadlineController&) = delete;
    ProcessorDeadlineController& operator=(const ProcessorDeadlineController&) = delete;
    ProcessorDeadlineController(ProcessorDeadlineController&&) = delete;
    ProcessorDeadlineController& operator=(ProcessorDeadlineController&&) = delete;

    [[nodiscard]] Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
    Program(const ProcessorDeadlineSources& sources, Foundation::UInt64 minimumObservedTicks = 0U) noexcept;

    /// Revalidates the exact installed receipt at the architecture-neutral
    /// admission commit point. This performs one clock read and no hardware
    /// write. It does not promise an exact later instruction-entry timestamp;
    /// after a successful sample, the installed timer and target interrupt
    /// handoff own that boundary. DeadlineReached leaves the controller Armed
    /// so the caller can unwind the admission; a protocol mismatch or
    /// exhausted clock faults it.
    [[nodiscard]] Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
    ValidateCurrent(const ProcessorDeadlineReceipt& receipt, Foundation::UInt64 minimumObservedTicks = 0U) noexcept;

    /// Consumes one exact armed receipt after target interruption entry has
    /// already masked the local timer and verified the mask by readback. The
    /// caller supplies the captured architectural clock sample; this method
    /// performs no clock read and the timer acknowledgement performs no
    /// hardware write. Receipt or time mismatches leave the installed
    /// transaction unchanged. A backend acknowledgement rejection faults the
    /// controller while retaining its diagnostic deadline fields.
    [[nodiscard]] Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
    ConsumeExternallyMaskedExpiration(const ProcessorDeadlineReceipt& receipt,
                                      Foundation::UInt64 observedTicks) noexcept;

    /// Preconditions:
    /// - The caller holds the same owning-CPU serialization domain required
    /// by Program; no Program or GetSnapshot operation is concurrent.
    [[nodiscard]] constexpr ProcessorDeadlineSnapshot GetSnapshot() const noexcept
    {
        return ProcessorDeadlineSnapshot{
            .SelectedDeadlineTicks = m_selectedDeadlineTicks,
            .State = m_state,
            .SelectedSources = m_selectedSources,
            .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
        };
    }

private:
    [[nodiscard]] static constexpr Foundation::UInt8 SourceMask(const ProcessorDeadlineSource source) noexcept
    {
        return static_cast<Foundation::UInt8>(source);
    }

    [[nodiscard]] static ProcessorDeadlineFailure
    MakeFailure(ProcessorDeadlineError error, Foundation::UInt64 selectedDeadlineTicks,
                Foundation::UInt64 observedTicks, Foundation::UInt8 affectedSources,
                Hal::Time::LocalTimerError hardwareError = Hal::Time::LocalTimerError::None) noexcept;
    [[nodiscard]] Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
    FinishNoDeadlineProgram(Foundation::UInt64 observedTicks) noexcept;
    [[nodiscard]] Foundation::Result<ProcessorDeadlineReceipt, ProcessorDeadlineFailure>
    FinishSelectedDeadlineProgram(Foundation::UInt64 selectedDeadlineTicks, Foundation::UInt8 selectedSources,
                                  Foundation::UInt64 observedTicks) noexcept;

    Hal::Time::ILocalTimer& m_localTimer;
    Foundation::UInt64 m_lastObservedTicks{0U};
    Foundation::UInt64 m_selectedDeadlineTicks{0U};
    Foundation::UInt8 m_selectedSources{0U};
    ProcessorDeadlineState m_state{ProcessorDeadlineState::Disarmed};
};

static_assert(sizeof(ProcessorDeadlineState) == 1U);
static_assert(sizeof(ProcessorDeadlineError) == 1U);
static_assert(sizeof(ProcessorDeadlineSource) == 1U);
static_assert(sizeof(ProcessorDeadlineSources) == 24U);
static_assert(alignof(ProcessorDeadlineSources) == 8U);
static_assert(sizeof(ProcessorDeadlineFailure) == 24U);
static_assert(alignof(ProcessorDeadlineFailure) == 8U);
static_assert(sizeof(ProcessorDeadlineReceipt) == 24U);
static_assert(alignof(ProcessorDeadlineReceipt) == 8U);
static_assert(sizeof(ProcessorDeadlineSnapshot) == 16U);
static_assert(alignof(ProcessorDeadlineSnapshot) == 8U);
} // Namespace Asteria::Kernel::Time

#endif // ASTERIA_KERNEL_TIME_PROCESSOR_DEADLINE_CONTROLLER_HPP_INCLUDED
