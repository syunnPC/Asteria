// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/System/Ia64SystemCallWaitContinuation.hpp"

#include "Kernel/Scheduler/Thread.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"

namespace Asteria::Platform::Ia64::System
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};

struct ValidatedRange
{
    Foundation::UInt64 Start;
    Foundation::UInt64 End;
};

[[nodiscard]] constexpr bool TryGetExclusiveEnd(const Foundation::UInt64 start, const Foundation::UInt64 size,
                                                Foundation::UInt64& end) noexcept
{
    if ((size == 0U) || (start > (MaximumUInt64 - size)))
    {
        return false;
    }
    end = start + size;
    return true;
}

[[nodiscard]] constexpr bool RangesOverlap(const ValidatedRange first, const ValidatedRange second) noexcept
{
    return (first.Start < second.End) && (second.Start < first.End);
}

[[nodiscard]] constexpr bool AddressIsInRange(const Foundation::UInt64 address, const ValidatedRange range) noexcept
{
    return (address >= range.Start) && (address < range.End);
}

[[nodiscard]] constexpr bool ValidateRange(const Scheduler::Ia64UserModeVirtualRange range,
                                           const Foundation::UInt64 minimumSize, const Foundation::UInt64 maximumSize,
                                           ValidatedRange& validated) noexcept
{
    Foundation::UInt64 end{0U};
    if ((range.VirtualStart == 0U) || ((range.VirtualStart & (Ia64SystemCallStoragePageSizeBytes - 1U)) != 0U) ||
        ((range.SizeBytes & (Ia64SystemCallStoragePageSizeBytes - 1U)) != 0U) || (range.SizeBytes < minimumSize) ||
        (range.SizeBytes > maximumSize) || !TryGetExclusiveEnd(range.VirtualStart, range.SizeBytes, end) ||
        (end > RegionZeroLimit))
    {
        return false;
    }
    validated = ValidatedRange{
        .Start = range.VirtualStart,
        .End = end,
    };
    return true;
}

[[nodiscard]] constexpr bool ValidateObjectRange(const void* const object, const Foundation::UInt64 size,
                                                 ValidatedRange& validated) noexcept
{
    if (object == nullptr)
    {
        return false;
    }
    const Foundation::UInt64 start = static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(object));
    Foundation::UInt64 end{0U};
    if (!TryGetExclusiveEnd(start, size, end) || (end > RegionZeroLimit))
    {
        return false;
    }
    validated = ValidatedRange{
        .Start = start,
        .End = end,
    };
    return true;
}

[[nodiscard]] bool RegisterStateIsZero(const Scheduler::Ia64KernelRegisterState& registers) noexcept
{
    const auto* const bytes = reinterpret_cast<const Foundation::UInt8*>(&registers);
    for (Foundation::Size index{0U}; index < sizeof(registers); ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool RegisterStateUsesStorage(const Scheduler::Ia64KernelRegisterState& registers,
                                                      const ValidatedRange memoryStack,
                                                      const ValidatedRange rseBackingStore,
                                                      const bool requireSystemCallRseConfiguration) noexcept
{
    return (registers.InstructionPointer != 0U) && AddressIsInRange(registers.StackPointer, memoryStack) &&
           AddressIsInRange(registers.RseBackingStorePointer, rseBackingStore) &&
           (!requireSystemCallRseConfiguration || (registers.RseConfiguration == Ia64SystemCallKernelRseConfiguration));
}

[[nodiscard]] bool ConfigurationIsValid(const Ia64SystemCallWaitContinuation& owner,
                                        const Ia64SystemCallWaitContinuationConfiguration& configuration,
                                        Ia64SystemCallWaitContinuationError& error) noexcept
{
    if (!configuration.Processor.IsValid())
    {
        error = Ia64SystemCallWaitContinuationError::InvalidProcessor;
        return false;
    }
    if ((configuration.EntryFrame == nullptr) || ((reinterpret_cast<Foundation::UIntPointer>(configuration.EntryFrame) &
                                                   (alignof(Ia64SystemCallEntryFrame) - 1U)) != 0U))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidEntryFrame;
        return false;
    }
    if ((configuration.SupervisorContinuation == nullptr) ||
        ((reinterpret_cast<Foundation::UIntPointer>(configuration.SupervisorContinuation) &
          (alignof(Scheduler::Ia64UserModeSupervisorContinuation) - 1U)) != 0U) ||
        ((configuration.SupervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Fresh) &&
         (configuration.SupervisorContinuation->State !=
          Scheduler::Ia64UserModeSupervisorContinuationState::Retained)) ||
        (configuration.SupervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::None))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidSupervisorContinuation;
        return false;
    }

    ValidatedRange systemCallMemoryStack{};
    if (!ValidateRange(configuration.SystemCallMemoryStack, Ia64SystemCallMemoryStackSizeBytes,
                       Ia64SystemCallMemoryStackSizeBytes, systemCallMemoryStack))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidMemoryStack;
        return false;
    }
    ValidatedRange systemCallRseBackingStore{};
    if (!ValidateRange(configuration.SystemCallRseBackingStore, Ia64SystemCallRseBackingStoreSizeBytes,
                       Ia64SystemCallRseBackingStoreSizeBytes, systemCallRseBackingStore))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidRseBackingStore;
        return false;
    }

    ValidatedRange supervisorMemoryStack{};
    if (!ValidateRange(configuration.SupervisorContinuation->MemoryStack, Scheduler::MinimumIa64UserModeStackSizeBytes,
                       Scheduler::MaximumIa64UserModeStackSizeBytes, supervisorMemoryStack))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidSupervisorContinuation;
        return false;
    }
    ValidatedRange supervisorRseBackingStore{};
    if (!ValidateRange(configuration.SupervisorContinuation->RseBackingStore,
                       Scheduler::MinimumIa64UserModeStackSizeBytes, Scheduler::MaximumIa64UserModeStackSizeBytes,
                       supervisorRseBackingStore))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidSupervisorContinuation;
        return false;
    }

    if (RangesOverlap(systemCallMemoryStack, systemCallRseBackingStore) ||
        RangesOverlap(systemCallMemoryStack, supervisorMemoryStack) ||
        RangesOverlap(systemCallMemoryStack, supervisorRseBackingStore) ||
        RangesOverlap(systemCallRseBackingStore, supervisorMemoryStack) ||
        RangesOverlap(systemCallRseBackingStore, supervisorRseBackingStore) ||
        RangesOverlap(supervisorMemoryStack, supervisorRseBackingStore))
    {
        error = Ia64SystemCallWaitContinuationError::StorageRangesOverlap;
        return false;
    }

    ValidatedRange ownerRange{};
    ValidatedRange frameRange{};
    ValidatedRange continuationRange{};
    if (!ValidateObjectRange(&owner, sizeof(owner), ownerRange) ||
        !ValidateObjectRange(configuration.EntryFrame, sizeof(*configuration.EntryFrame), frameRange) ||
        !ValidateObjectRange(configuration.SupervisorContinuation, sizeof(*configuration.SupervisorContinuation),
                             continuationRange))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidConfiguration;
        return false;
    }

    const ValidatedRange storageRanges[]{
        systemCallMemoryStack,
        systemCallRseBackingStore,
        supervisorMemoryStack,
        supervisorRseBackingStore,
    };
    for (const ValidatedRange storage : storageRanges)
    {
        if (RangesOverlap(ownerRange, storage) || RangesOverlap(frameRange, storage) ||
            RangesOverlap(continuationRange, storage))
        {
            error = Ia64SystemCallWaitContinuationError::StorageRangesOverlap;
            return false;
        }
    }
    if (RangesOverlap(ownerRange, frameRange) || RangesOverlap(ownerRange, continuationRange) ||
        RangesOverlap(frameRange, continuationRange))
    {
        error = Ia64SystemCallWaitContinuationError::StorageRangesOverlap;
        return false;
    }

    if ((configuration.EntryFrame->State != Ia64SystemCallEntryFrameState::Inactive) ||
        (configuration.EntryFrame->Reserved != 0U))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidEntryFrame;
        return false;
    }
    if ((configuration.SupervisorContinuation->State == Scheduler::Ia64UserModeSupervisorContinuationState::Fresh) &&
        !RegisterStateIsZero(configuration.SupervisorContinuation->Registers))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidSupervisorContinuation;
        return false;
    }
    if ((configuration.SupervisorContinuation->State == Scheduler::Ia64UserModeSupervisorContinuationState::Retained) &&
        !RegisterStateUsesStorage(configuration.SupervisorContinuation->Registers, supervisorMemoryStack,
                                  supervisorRseBackingStore, false))
    {
        error = Ia64SystemCallWaitContinuationError::InvalidSupervisorRegisterState;
        return false;
    }
    return true;
}
} // namespace

Ia64SystemCallWaitContinuationError
Ia64SystemCallWaitContinuation::Initialize(const Ia64SystemCallWaitContinuationConfiguration& configuration) noexcept
{
    if (m_state != Ia64SystemCallWaitContinuationState::Uninitialized)
    {
        return Ia64SystemCallWaitContinuationError::AlreadyInitialized;
    }

    Ia64SystemCallWaitContinuationError error{Ia64SystemCallWaitContinuationError::None};
    if (!ConfigurationIsValid(*this, configuration, error))
    {
        return error;
    }

    m_expectedFrame = configuration.EntryFrame;
    m_supervisorContinuation = configuration.SupervisorContinuation;
    m_systemCallMemoryStack = configuration.SystemCallMemoryStack;
    m_systemCallRseBackingStore = configuration.SystemCallRseBackingStore;
    m_processor = configuration.Processor;
    m_state = Ia64SystemCallWaitContinuationState::Idle;
    return Ia64SystemCallWaitContinuationError::None;
}

Ia64SystemCallWaitReservationResult
Ia64SystemCallWaitContinuation::ReserveEntry(Ia64SystemCallEntryFrame& frame,
                                             const Ia64SystemCallWaitCorrelation& correlation) noexcept
{
    using ResultType = Ia64SystemCallWaitReservationResult;
    if (m_state == Ia64SystemCallWaitContinuationState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::NotInitialized));
    }
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::Faulted));
    }
    if (m_state != Ia64SystemCallWaitContinuationState::Idle)
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::InvalidState));
    }
    if (!FrameIsExactAndActive(frame))
    {
        return ResultType::Failure(MakeFailure((&frame == m_expectedFrame)
                                                   ? Ia64SystemCallWaitContinuationError::EntryFrameNotActive
                                                   : Ia64SystemCallWaitContinuationError::InvalidEntryFrame));
    }
    if (!CorrelationIsValid(correlation) || (correlation.Processor.GetValue() != m_processor.GetValue()))
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::InvalidCorrelation));
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Retained) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::None))
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::InvalidSupervisorState));
    }
    if (m_systemCallRecordActionable || !RegisterStateIsZero(m_systemCallRegisters))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::Faulted));
    }

    Foundation::UInt64 pendingEpoch{0U};
    if (!Ia64SystemCallWaitEpochPolicy::TryAdvance(m_epoch, pendingEpoch))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::EpochExhausted);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::EpochExhausted));
    }

    m_activeFrame = &frame;
    m_preparedCorrelation = correlation;
    m_pendingEpoch = pendingEpoch;
    m_supervisorContinuation->Owner = Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait;
    m_state = Ia64SystemCallWaitContinuationState::EntryReserved;
    return ResultType::Success(Ia64SystemCallWaitReservationReceipt{
        .Correlation = correlation,
        .PendingEpoch = pendingEpoch,
    });
}

Ia64SystemCallWaitContinuationError
Ia64SystemCallWaitContinuation::CancelReservedEntry(Ia64SystemCallEntryFrame& frame,
                                                    const Ia64SystemCallWaitCorrelation& correlation) noexcept
{
    if (m_state == Ia64SystemCallWaitContinuationState::Uninitialized)
    {
        return Ia64SystemCallWaitContinuationError::NotInitialized;
    }
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        return Ia64SystemCallWaitContinuationError::Faulted;
    }
    if (m_state != Ia64SystemCallWaitContinuationState::EntryReserved)
    {
        return Ia64SystemCallWaitContinuationError::InvalidState;
    }
    if ((m_activeFrame != &frame) || !FrameIsExactAndActive(frame))
    {
        return (&frame == m_expectedFrame) ? Ia64SystemCallWaitContinuationError::EntryFrameNotActive
                                           : Ia64SystemCallWaitContinuationError::InvalidEntryFrame;
    }
    if (!CorrelationMatches(correlation))
    {
        return Ia64SystemCallWaitContinuationError::InvalidCorrelation;
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Retained) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSupervisorState);
        return Ia64SystemCallWaitContinuationError::InvalidSupervisorState;
    }

    m_supervisorContinuation->Owner = Scheduler::Ia64UserModeSupervisorContinuationOwner::None;
    ClearTransaction();
    m_state = Ia64SystemCallWaitContinuationState::Idle;
    return Ia64SystemCallWaitContinuationError::None;
}

Ia64SystemCallWaitCommitResult
Ia64SystemCallWaitContinuation::CommitPrepared(Ia64SystemCallEntryFrame& frame,
                                               const Ia64SystemCallWaitCorrelation& correlation) noexcept
{
    using ResultType = Ia64SystemCallWaitCommitResult;
    if (m_state == Ia64SystemCallWaitContinuationState::Uninitialized)
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::NotInitialized));
    }
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::Faulted));
    }
    if (m_state != Ia64SystemCallWaitContinuationState::EntryReserved)
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::InvalidState));
    }
    if ((m_activeFrame != &frame) || !FrameIsExactAndActive(frame))
    {
        return ResultType::Failure(MakeFailure((&frame == m_expectedFrame)
                                                   ? Ia64SystemCallWaitContinuationError::EntryFrameNotActive
                                                   : Ia64SystemCallWaitContinuationError::InvalidEntryFrame));
    }
    if (!CorrelationMatches(correlation) || (m_pendingEpoch == 0U))
    {
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::InvalidCorrelation));
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Retained) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSupervisorState);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::InvalidSupervisorState));
    }

    m_epoch = m_pendingEpoch;
    m_pendingEpoch = 0U;
    m_state = Ia64SystemCallWaitContinuationState::WaitPrepared;
    return ResultType::Success(Ia64SystemCallWaitCommitReceipt{
        .Correlation = m_preparedCorrelation,
        .Epoch = m_epoch,
    });
}

Ia64SystemCallWaitResponseResult
Ia64SystemCallWaitContinuation::SuspendToSupervisor(Ia64SystemCallEntryFrame& frame) noexcept
{
    using ResultType = Ia64SystemCallWaitResponseResult;
    Ia64SystemCallWaitContinuationFailure failure{};
    if (!BeginSupervisorSwitch(frame, failure))
    {
        return ResultType::Failure(failure);
    }

    const Scheduler::Ia64SynchronousContinuationSwitchError switched =
        Scheduler::SwitchIa64SynchronousContinuation(m_systemCallRegisters, m_supervisorContinuation->Registers);
    if (switched != Scheduler::Ia64SynchronousContinuationSwitchError::None)
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable));
    }
    if (m_state == Ia64SystemCallWaitContinuationState::SwitchingToSupervisor)
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable));
    }
    return FinishSystemCallResume();
}

Ia64SystemCallWaitSupervisorResult Ia64SystemCallWaitContinuation::AcceptInitialSupervisorResume() noexcept
{
    return FinishSupervisorResume(Ia64SystemCallWaitSupervisorResumeSite::InitialLaunch);
}

Ia64SystemCallWaitContinuationError Ia64SystemCallWaitContinuation::BeginWaitingForReadmission() noexcept
{
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        return Ia64SystemCallWaitContinuationError::Faulted;
    }
    if (m_state != Ia64SystemCallWaitContinuationState::SupervisorActive)
    {
        return Ia64SystemCallWaitContinuationError::InvalidState;
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Active) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSupervisorState);
        return Ia64SystemCallWaitContinuationError::InvalidSupervisorState;
    }
    if ((m_activeFrame == nullptr) || !FrameIsExactAndActive(*m_activeFrame))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::EntryFrameNotActive);
        return Ia64SystemCallWaitContinuationError::EntryFrameNotActive;
    }
    if (!m_systemCallRecordActionable || !SystemCallRegistersAreValid())
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState);
        return Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState;
    }

    m_state = Ia64SystemCallWaitContinuationState::WaitingReadmission;
    return Ia64SystemCallWaitContinuationError::None;
}

Ia64SystemCallWaitContinuationError
Ia64SystemCallWaitContinuation::PrepareSystemCallResume(Ia64SystemCallEntryFrame& frame,
                                                        const Ia64SystemCallWaitCorrelation& readmitted,
                                                        const Kernel::System::SystemCallResponse& response) noexcept
{
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        return Ia64SystemCallWaitContinuationError::Faulted;
    }
    if (m_state != Ia64SystemCallWaitContinuationState::WaitingReadmission)
    {
        return Ia64SystemCallWaitContinuationError::InvalidState;
    }
    if ((m_activeFrame != &frame) || !FrameIsExactAndActive(frame))
    {
        EnterFaulted((&frame == m_expectedFrame) ? Ia64SystemCallWaitContinuationError::EntryFrameNotActive
                                                 : Ia64SystemCallWaitContinuationError::InvalidEntryFrame);
        return m_terminalError;
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Active) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSupervisorState);
        return Ia64SystemCallWaitContinuationError::InvalidSupervisorState;
    }
    Foundation::UInt64 expectedRunSequence{0U};
    if (!Kernel::Scheduler::ThreadRunSequencePolicy::TryAdvance(m_preparedCorrelation.RunSequence, expectedRunSequence))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::RunSequenceExhausted);
        return Ia64SystemCallWaitContinuationError::RunSequenceExhausted;
    }
    if (!CorrelationIsValid(readmitted) || !readmitted.Thread.IsEqualTo(m_preparedCorrelation.Thread) ||
        !readmitted.SchedulingContext.IsEqualTo(m_preparedCorrelation.SchedulingContext) ||
        (readmitted.Processor.GetValue() != m_preparedCorrelation.Processor.GetValue()) ||
        (readmitted.RunSequence != expectedRunSequence))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidCorrelation);
        return Ia64SystemCallWaitContinuationError::InvalidCorrelation;
    }

    m_readmittedRunSequence = readmitted.RunSequence;
    m_response = response;
    m_state = Ia64SystemCallWaitContinuationState::ResumeReady;
    return Ia64SystemCallWaitContinuationError::None;
}

Ia64SystemCallWaitSupervisorResult Ia64SystemCallWaitContinuation::ResumeSystemCall() noexcept
{
    using ResultType = Ia64SystemCallWaitSupervisorResult;
    Ia64SystemCallWaitContinuationFailure failure{};
    if (!BeginSystemCallSwitch(failure))
    {
        return ResultType::Failure(failure);
    }

    const Scheduler::Ia64SynchronousContinuationSwitchError switched =
        Scheduler::SwitchIa64SynchronousContinuation(m_supervisorContinuation->Registers, m_systemCallRegisters);
    if (switched != Scheduler::Ia64SynchronousContinuationSwitchError::None)
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable));
    }
    if (m_state == Ia64SystemCallWaitContinuationState::SwitchingToSystemCall)
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable);
        return ResultType::Failure(MakeFailure(Ia64SystemCallWaitContinuationError::NativeTransitionUnavailable));
    }
    return FinishSupervisorResume(Ia64SystemCallWaitSupervisorResumeSite::RepeatedSwitch);
}

Ia64SystemCallWaitContinuationSnapshot Ia64SystemCallWaitContinuation::GetSnapshot() const noexcept
{
    return Ia64SystemCallWaitContinuationSnapshot{
        .Thread = m_preparedCorrelation.Thread,
        .SchedulingContext = m_preparedCorrelation.SchedulingContext,
        .Epoch = m_epoch,
        .PendingEpoch = m_pendingEpoch,
        .PreparedRunSequence = m_preparedCorrelation.RunSequence,
        .ReadmittedRunSequence = m_readmittedRunSequence,
        .State = m_state,
        .TerminalError = m_terminalError,
        .SupervisorState = m_supervisorContinuation == nullptr
                               ? Scheduler::Ia64UserModeSupervisorContinuationState::Fresh
                               : m_supervisorContinuation->State,
        .SystemCallRecordActionable = static_cast<Foundation::UInt8>(m_systemCallRecordActionable ? 1U : 0U),
        .EntryFrameRetained = static_cast<Foundation::UInt8>(m_activeFrame != nullptr ? 1U : 0U),
    };
}

Ia64SystemCallWaitContinuationFailure
Ia64SystemCallWaitContinuation::MakeFailure(const Ia64SystemCallWaitContinuationError error) const noexcept
{
    return Ia64SystemCallWaitContinuationFailure{
        .Epoch = m_pendingEpoch != 0U ? m_pendingEpoch : m_epoch,
        .Error = error,
        .State = m_state,
    };
}

bool Ia64SystemCallWaitContinuation::FrameIsExactAndActive(const Ia64SystemCallEntryFrame& frame) const noexcept
{
    return (&frame == m_expectedFrame) && (frame.State == Ia64SystemCallEntryFrameState::Active) &&
           (frame.Reserved == 0U) &&
           (ValidateIa64SystemCallInterruptedState(frame) == Ia64SystemCallInterruptedStateValidationError::None);
}

bool Ia64SystemCallWaitContinuation::CorrelationIsValid(const Ia64SystemCallWaitCorrelation& correlation) noexcept
{
    return correlation.Thread.IsValid() && correlation.SchedulingContext.IsValid() && (correlation.RunSequence != 0U) &&
           correlation.Processor.IsValid();
}

bool Ia64SystemCallWaitContinuation::CorrelationMatches(const Ia64SystemCallWaitCorrelation& correlation) const noexcept
{
    return CorrelationIsValid(correlation) && correlation.Thread.IsEqualTo(m_preparedCorrelation.Thread) &&
           correlation.SchedulingContext.IsEqualTo(m_preparedCorrelation.SchedulingContext) &&
           (correlation.RunSequence == m_preparedCorrelation.RunSequence) &&
           (correlation.Processor.GetValue() == m_preparedCorrelation.Processor.GetValue());
}

bool Ia64SystemCallWaitContinuation::SupervisorRegistersAreValid() const noexcept
{
    if (m_supervisorContinuation == nullptr)
    {
        return false;
    }
    ValidatedRange memoryStack{};
    ValidatedRange rseBackingStore{};
    return ValidateRange(m_supervisorContinuation->MemoryStack, Scheduler::MinimumIa64UserModeStackSizeBytes,
                         Scheduler::MaximumIa64UserModeStackSizeBytes, memoryStack) &&
           ValidateRange(m_supervisorContinuation->RseBackingStore, Scheduler::MinimumIa64UserModeStackSizeBytes,
                         Scheduler::MaximumIa64UserModeStackSizeBytes, rseBackingStore) &&
           RegisterStateUsesStorage(m_supervisorContinuation->Registers, memoryStack, rseBackingStore, false);
}

bool Ia64SystemCallWaitContinuation::SystemCallRegistersAreValid() const noexcept
{
    ValidatedRange memoryStack{};
    ValidatedRange rseBackingStore{};
    return ValidateRange(m_systemCallMemoryStack, Ia64SystemCallMemoryStackSizeBytes,
                         Ia64SystemCallMemoryStackSizeBytes, memoryStack) &&
           ValidateRange(m_systemCallRseBackingStore, Ia64SystemCallRseBackingStoreSizeBytes,
                         Ia64SystemCallRseBackingStoreSizeBytes, rseBackingStore) &&
           RegisterStateUsesStorage(m_systemCallRegisters, memoryStack, rseBackingStore, true);
}

bool Ia64SystemCallWaitContinuation::BeginSupervisorSwitch(Ia64SystemCallEntryFrame& frame,
                                                           Ia64SystemCallWaitContinuationFailure& failure) noexcept
{
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        failure = MakeFailure(Ia64SystemCallWaitContinuationError::Faulted);
        return false;
    }
    if (m_state != Ia64SystemCallWaitContinuationState::WaitPrepared)
    {
        failure = MakeFailure(Ia64SystemCallWaitContinuationError::InvalidState);
        return false;
    }
    if ((m_activeFrame != &frame) || !FrameIsExactAndActive(frame))
    {
        EnterFaulted((&frame == m_expectedFrame) ? Ia64SystemCallWaitContinuationError::EntryFrameNotActive
                                                 : Ia64SystemCallWaitContinuationError::InvalidEntryFrame);
        failure = MakeFailure(m_terminalError);
        return false;
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Retained) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait) ||
        !SupervisorRegistersAreValid())
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSupervisorRegisterState);
        failure = MakeFailure(m_terminalError);
        return false;
    }
    if (m_systemCallRecordActionable || !RegisterStateIsZero(m_systemCallRegisters))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState);
        failure = MakeFailure(m_terminalError);
        return false;
    }

    m_state = Ia64SystemCallWaitContinuationState::SwitchingToSupervisor;
    m_supervisorContinuation->State = Scheduler::Ia64UserModeSupervisorContinuationState::ResumePending;
    failure = {};
    return true;
}

bool Ia64SystemCallWaitContinuation::BeginSystemCallSwitch(Ia64SystemCallWaitContinuationFailure& failure) noexcept
{
    if (m_state == Ia64SystemCallWaitContinuationState::Faulted)
    {
        failure = MakeFailure(Ia64SystemCallWaitContinuationError::Faulted);
        return false;
    }
    if (m_state != Ia64SystemCallWaitContinuationState::ResumeReady)
    {
        failure = MakeFailure(Ia64SystemCallWaitContinuationError::InvalidState);
        return false;
    }
    if ((m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Active) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait) ||
        !RegisterStateIsZero(m_supervisorContinuation->Registers))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSupervisorState);
        failure = MakeFailure(m_terminalError);
        return false;
    }
    if ((m_activeFrame == nullptr) || !FrameIsExactAndActive(*m_activeFrame))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::EntryFrameNotActive);
        failure = MakeFailure(m_terminalError);
        return false;
    }
    if (!m_systemCallRecordActionable || !SystemCallRegistersAreValid())
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState);
        failure = MakeFailure(m_terminalError);
        return false;
    }

    m_state = Ia64SystemCallWaitContinuationState::SwitchingToSystemCall;
    failure = {};
    return true;
}

Ia64SystemCallWaitSupervisorResult
Ia64SystemCallWaitContinuation::FinishSupervisorResume(const Ia64SystemCallWaitSupervisorResumeSite site) noexcept
{
    using ResultType = Ia64SystemCallWaitSupervisorResult;
    if ((m_state != Ia64SystemCallWaitContinuationState::SwitchingToSupervisor) ||
        (m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::ResumePending) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait) ||
        (m_activeFrame == nullptr) || !FrameIsExactAndActive(*m_activeFrame))
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidState);
        return ResultType::Failure(MakeFailure(m_terminalError));
    }

    m_systemCallRecordActionable = true;
    if (!SystemCallRegistersAreValid())
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidSystemCallRegisterState);
        return ResultType::Failure(MakeFailure(m_terminalError));
    }

    m_supervisorContinuation->Registers = {};
    m_supervisorContinuation->State = Scheduler::Ia64UserModeSupervisorContinuationState::Active;
    m_state = Ia64SystemCallWaitContinuationState::SupervisorActive;
    return ResultType::Success(Ia64SystemCallWaitSupervisorReceipt{
        .Correlation = m_preparedCorrelation,
        .Epoch = m_epoch,
        .Site = site,
    });
}

Ia64SystemCallWaitResponseResult Ia64SystemCallWaitContinuation::FinishSystemCallResume() noexcept
{
    using ResultType = Ia64SystemCallWaitResponseResult;
    if ((m_state != Ia64SystemCallWaitContinuationState::SwitchingToSystemCall) ||
        (m_supervisorContinuation == nullptr) ||
        (m_supervisorContinuation->State != Scheduler::Ia64UserModeSupervisorContinuationState::Active) ||
        (m_supervisorContinuation->Owner != Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait) ||
        (m_activeFrame == nullptr) || !FrameIsExactAndActive(*m_activeFrame) || !m_systemCallRecordActionable ||
        (m_readmittedRunSequence == 0U) || !SupervisorRegistersAreValid())
    {
        EnterFaulted(Ia64SystemCallWaitContinuationError::InvalidState);
        return ResultType::Failure(MakeFailure(m_terminalError));
    }

    const Kernel::System::SystemCallResponse response = m_response;
    ClearSystemCallRegisters();
    m_supervisorContinuation->State = Scheduler::Ia64UserModeSupervisorContinuationState::Retained;
    m_supervisorContinuation->Owner = Scheduler::Ia64UserModeSupervisorContinuationOwner::None;
    ClearTransaction();
    m_state = Ia64SystemCallWaitContinuationState::Idle;
    return ResultType::Success(response);
}

void Ia64SystemCallWaitContinuation::ClearSystemCallRegisters() noexcept
{
    m_systemCallRegisters = {};
    m_systemCallRecordActionable = false;
}

void Ia64SystemCallWaitContinuation::ClearTransaction() noexcept
{
    m_activeFrame = nullptr;
    m_preparedCorrelation = {};
    m_response = {};
    m_pendingEpoch = 0U;
    m_readmittedRunSequence = 0U;
}

void Ia64SystemCallWaitContinuation::EnterFaulted(const Ia64SystemCallWaitContinuationError error) noexcept
{
    m_terminalError = error;
    ClearSystemCallRegisters();
    ClearTransaction();
    if ((m_supervisorContinuation != nullptr) &&
        (m_supervisorContinuation->Owner == Scheduler::Ia64UserModeSupervisorContinuationOwner::SystemCallWait))
    {
        m_supervisorContinuation->Registers = {};
        m_supervisorContinuation->State = Scheduler::Ia64UserModeSupervisorContinuationState::Faulted;
    }
    m_state = Ia64SystemCallWaitContinuationState::Faulted;
}
} // namespace Asteria::Platform::Ia64::System
