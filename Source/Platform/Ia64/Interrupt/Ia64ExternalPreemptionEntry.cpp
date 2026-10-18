// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp"

#include "Platform/Synchronization/AtomicUInt64.hpp"

extern "C" Asteria::Foundation::UInt64
AsteriaIa64GetExternalPreemptionCodeRangeWords(Asteria::Foundation::UInt64 outputAddress) noexcept;

namespace Asteria::Platform::Ia64::Interrupt
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 RegionZeroLimit{Foundation::UInt64{1U} << 61U};
constexpr Foundation::UInt64 InterruptionEnableMask{Foundation::UInt64{1U} << 14U};

[[nodiscard]] constexpr bool IsPowerOfTwo(const Foundation::UInt64 value) noexcept
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

[[nodiscard]] constexpr bool IsRangeValid(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                          const Foundation::UInt64 alignment) noexcept
{
    return IsPowerOfTwo(alignment) && (start != 0U) && (start < end) && (end <= RegionZeroLimit) &&
           ((start & (alignment - 1U)) == 0U);
}

[[nodiscard]] constexpr bool HasExactSize(const Foundation::UInt64 start, const Foundation::UInt64 end,
                                          const Foundation::UInt64 sizeBytes) noexcept
{
    return (start <= end) && ((end - start) == sizeBytes);
}

[[nodiscard]] constexpr bool RangesOverlap(const Foundation::UInt64 firstStart, const Foundation::UInt64 firstEnd,
                                           const Foundation::UInt64 secondStart,
                                           const Foundation::UInt64 secondEnd) noexcept
{
    return (firstStart < secondEnd) && (secondStart < firstEnd);
}

[[nodiscard]] constexpr bool IsValidConfiguration(const Ia64ExternalPreemptionControlConfiguration& configuration,
                                                  const Foundation::UInt64 controlAddress) noexcept
{
    const Foundation::UInt64 frameEnd = configuration.FrameAddress + sizeof(Ia64ExternalPreemptionFrame);
    const Foundation::UInt64 controlEnd = controlAddress + sizeof(Ia64ExternalPreemptionControl);
    const bool additionsDidNotWrap = (frameEnd >= configuration.FrameAddress) && (controlEnd >= controlAddress);
    if (!additionsDidNotWrap || (configuration.ProcessorIndex != 0U) ||
        (configuration.TimerVector != Ia64ExternalInterruptTimerVector) ||
        !IsRangeValid(configuration.FrameAddress, frameEnd, alignof(Ia64ExternalPreemptionFrame)) ||
        !IsRangeValid(configuration.WorkerMemoryStackStart, configuration.WorkerMemoryStackEnd,
                      Ia64ExternalPreemptionStoragePageSizeBytes) ||
        !IsRangeValid(configuration.WorkerRseBackingStoreStart, configuration.WorkerRseBackingStoreEnd,
                      Ia64ExternalPreemptionStoragePageSizeBytes) ||
        !IsRangeValid(configuration.CoordinatorMemoryStackStart, configuration.CoordinatorMemoryStackEnd,
                      Ia64ExternalPreemptionStoragePageSizeBytes) ||
        !HasExactSize(configuration.CoordinatorMemoryStackStart, configuration.CoordinatorMemoryStackEnd,
                      Ia64ExternalPreemptionCoordinatorStackSizeBytes) ||
        !IsRangeValid(configuration.CoordinatorRseBackingStoreStart, configuration.CoordinatorRseBackingStoreEnd,
                      Ia64ExternalPreemptionStoragePageSizeBytes) ||
        !HasExactSize(configuration.CoordinatorRseBackingStoreStart, configuration.CoordinatorRseBackingStoreEnd,
                      Ia64ExternalPreemptionCoordinatorRseBackingStoreSizeBytes) ||
        !IsRangeValid(controlAddress, controlEnd, 64U) || (configuration.CoordinatorEntryPoint == 0U) ||
        ((configuration.CoordinatorEntryPoint & 15U) != 0U) ||
        (configuration.CoordinatorEntryPoint >= RegionZeroLimit) ||
        (configuration.CoordinatorGlobalPointer >= RegionZeroLimit) || (configuration.CoordinatorArgument == 0U) ||
        (configuration.CoordinatorArgument >= RegionZeroLimit) || (configuration.SentinelEntryPoint == 0U) ||
        ((configuration.SentinelEntryPoint & 15U) != 0U) ||
        !HasExactSize(configuration.SentinelEntryPoint, configuration.SentinelEnd, 16U) ||
        (configuration.SentinelEnd > RegionZeroLimit))
    {
        return false;
    }

    const Foundation::UInt64 workerStackSize =
        configuration.WorkerMemoryStackEnd - configuration.WorkerMemoryStackStart;
    const Foundation::UInt64 workerRseSize =
        configuration.WorkerRseBackingStoreEnd - configuration.WorkerRseBackingStoreStart;
    if ((workerStackSize < 16U * 1024U) || (workerStackSize > 64U * 1024U) ||
        ((workerStackSize & (Ia64ExternalPreemptionStoragePageSizeBytes - 1U)) != 0U) ||
        (workerRseSize < 16U * 1024U) || (workerRseSize > 64U * 1024U) ||
        ((workerRseSize & (Ia64ExternalPreemptionStoragePageSizeBytes - 1U)) != 0U))
    {
        return false;
    }

    const Foundation::UInt64 starts[5U]{
        configuration.FrameAddress,
        configuration.WorkerMemoryStackStart,
        configuration.WorkerRseBackingStoreStart,
        configuration.CoordinatorMemoryStackStart,
        configuration.CoordinatorRseBackingStoreStart,
    };
    const Foundation::UInt64 ends[5U]{
        frameEnd,
        configuration.WorkerMemoryStackEnd,
        configuration.WorkerRseBackingStoreEnd,
        configuration.CoordinatorMemoryStackEnd,
        configuration.CoordinatorRseBackingStoreEnd,
    };
    for (Foundation::Size first{0U}; first < 5U; ++first)
    {
        if (RangesOverlap(controlAddress, controlEnd, starts[first], ends[first]))
        {
            return false;
        }
        for (Foundation::Size second{first + 1U}; second < 5U; ++second)
        {
            if (RangesOverlap(starts[first], ends[first], starts[second], ends[second]))
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] constexpr Ia64ExternalPreemptionControlConfiguration
ReadConfiguration(const Ia64ExternalPreemptionControl& control) noexcept
{
    return Ia64ExternalPreemptionControlConfiguration{
        .ProcessorIndex = control.ProcessorIndex,
        .TimerVector = control.TimerVector,
        .FrameAddress = control.FrameAddress,
        .WorkerMemoryStackStart = control.WorkerMemoryStackStart,
        .WorkerMemoryStackEnd = control.WorkerMemoryStackEnd,
        .WorkerRseBackingStoreStart = control.WorkerRseBackingStoreStart,
        .WorkerRseBackingStoreEnd = control.WorkerRseBackingStoreEnd,
        .CoordinatorMemoryStackStart = control.CoordinatorMemoryStackStart,
        .CoordinatorMemoryStackEnd = control.CoordinatorMemoryStackEnd,
        .CoordinatorRseBackingStoreStart = control.CoordinatorRseBackingStoreStart,
        .CoordinatorRseBackingStoreEnd = control.CoordinatorRseBackingStoreEnd,
        .CoordinatorEntryPoint = control.CoordinatorEntryPoint,
        .CoordinatorGlobalPointer = control.CoordinatorGlobalPointer,
        .CoordinatorArgument = control.CoordinatorArgument,
        .SentinelEntryPoint = control.SentinelEntryPoint,
        .SentinelEnd = control.SentinelEnd,
    };
}

[[nodiscard]] constexpr bool ConfigurationMatchesFrame(const Ia64ExternalPreemptionControl& control,
                                                       const Ia64ExternalPreemptionFrame& frame) noexcept
{
    return control.FrameAddress == static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&frame));
}

[[nodiscard]] constexpr bool TryAdvanceRsePointer(const Foundation::UInt64 pointer,
                                                  const Foundation::UInt64 registerCount,
                                                  Foundation::UInt64& advanced) noexcept
{
    if ((pointer == 0U) || ((pointer & 7U) != 0U) || (registerCount > 96U))
    {
        return false;
    }
    const Foundation::UInt64 slot = (pointer >> 3U) & 63U;
    if (slot == 63U)
    {
        return false;
    }
    const Foundation::UInt64 slotSum = slot + registerCount;
    const Foundation::UInt64 skippedNatSlots = (slotSum >= 63U ? 1U : 0U) + (slotSum >= 126U ? 1U : 0U);
    const Foundation::UInt64 byteCount = (registerCount + skippedNatSlots) * 8U;
    if (pointer > (MaximumUInt64 - byteCount))
    {
        return false;
    }
    advanced = pointer + byteCount;
    return true;
}
} // namespace

Ia64ExternalPreemptionControlError
ConfigureIa64ExternalPreemptionControl(Ia64ExternalPreemptionControl& control,
                                       const Ia64ExternalPreemptionControlConfiguration& configuration) noexcept
{
    if (AcquireIa64ExternalPreemptionControlState(control) != Ia64ExternalPreemptionControlState::Fresh ||
        AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::LegacyEvidence)
    {
        return Ia64ExternalPreemptionControlError::AlreadyConfigured;
    }
    const Foundation::UInt64 controlAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&control));
    if (!IsValidConfiguration(configuration, controlAddress))
    {
        return Ia64ExternalPreemptionControlError::InvalidConfiguration;
    }

    control.ProcessorIndex = configuration.ProcessorIndex;
    control.TimerVector = configuration.TimerVector;
    control.FrameAddress = configuration.FrameAddress;
    control.WorkerMemoryStackStart = configuration.WorkerMemoryStackStart;
    control.WorkerMemoryStackEnd = configuration.WorkerMemoryStackEnd;
    control.WorkerRseBackingStoreStart = configuration.WorkerRseBackingStoreStart;
    control.WorkerRseBackingStoreEnd = configuration.WorkerRseBackingStoreEnd;
    control.CoordinatorMemoryStackStart = configuration.CoordinatorMemoryStackStart;
    control.CoordinatorMemoryStackEnd = configuration.CoordinatorMemoryStackEnd;
    control.CoordinatorRseBackingStoreStart = configuration.CoordinatorRseBackingStoreStart;
    control.CoordinatorRseBackingStoreEnd = configuration.CoordinatorRseBackingStoreEnd;
    control.CoordinatorEntryPoint = configuration.CoordinatorEntryPoint;
    control.CoordinatorGlobalPointer = configuration.CoordinatorGlobalPointer;
    control.CoordinatorArgument = configuration.CoordinatorArgument;
    control.SentinelEntryPoint = configuration.SentinelEntryPoint;
    control.SentinelEnd = configuration.SentinelEnd;
    control.ArmedDeadlineTicks = 0U;
    control.CapturedFrameToken = 0U;
    control.IdleContinuationGeneration = 0U;
    control.CaptureCount = 0U;
    control.RestoreCount = 0U;
    control.LastFailure = 0U;
    Synchronization::AtomicUInt64Reference state{control.State};
    state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::Configured));
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionControlError
ArmIa64ExternalPreemptionControlForSentinel(Ia64ExternalPreemptionControl& control,
                                            const Ia64ExternalPreemptionFrame& frame,
                                            const Ia64ExternalPreemptionSentinelObservation& observation) noexcept
{
    if (AcquireIa64ExternalPreemptionControlState(control) != Ia64ExternalPreemptionControlState::Configured ||
        AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::LegacyEvidence)
    {
        return Ia64ExternalPreemptionControlError::InvalidState;
    }
    if (!ConfigurationMatchesFrame(control, frame))
    {
        return Ia64ExternalPreemptionControlError::InvalidConfiguration;
    }
    if (Ia64ExternalPreemptionFrameIsOwned(frame))
    {
        return Ia64ExternalPreemptionControlError::FrameOwned;
    }
    if ((observation.InstructionPointer != control.SentinelEntryPoint) ||
        (observation.StackPointer < control.WorkerMemoryStackStart) ||
        (observation.StackPointer >= control.WorkerMemoryStackEnd) ||
        ((observation.CurrentProcessorStatus & Ia64ExternalInterruptPollProcessorStatusMask) !=
         Ia64ExternalInterruptPollProcessorStatusValue) ||
        (observation.RseConfiguration != 0U) ||
        (observation.RseBackingStoreStorePointer < control.WorkerRseBackingStoreStart) ||
        (observation.RseBackingStoreStorePointer >= control.WorkerRseBackingStoreEnd) ||
        (observation.RseBackingStorePointer < observation.RseBackingStoreStorePointer) ||
        (observation.RseBackingStorePointer >= control.WorkerRseBackingStoreEnd) ||
        (observation.ProgrammedDeadlineTicks == 0U) || (observation.ProgrammedDeadlineTicks == MaximumUInt64))
    {
        return Ia64ExternalPreemptionControlError::InvalidSentinelState;
    }
    if ((observation.RseBackingStorePointer - observation.RseBackingStoreStorePointer) >=
        Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes)
    {
        return Ia64ExternalPreemptionControlError::DirtyPartitionTooLarge;
    }

    control.ArmedDeadlineTicks = observation.ProgrammedDeadlineTicks;
    control.CapturedFrameToken = 0U;
    control.IdleContinuationGeneration = 0U;
    control.LastFailure = 0U;
    Synchronization::AtomicUInt64Reference state{control.State};
    state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::Armed));
    Synchronization::AtomicUInt64Reference mode{control.Mode};
    mode.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionMode::BspOneShotPreemption));
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionControlError
ClaimIa64ExternalPreemptionControlCapture(Ia64ExternalPreemptionControl& control,
                                          const Foundation::UInt64 interruptedInstructionPointer,
                                          const Foundation::UInt64 interruptedProcessorStatus) noexcept
{
    const Foundation::UInt64 interruptedInstructionSlot =
        (interruptedProcessorStatus & Ia64ExternalInterruptInstructionSlotMask) >> 41U;
    if (AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::BspOneShotPreemption ||
        (interruptedInstructionPointer != control.SentinelEntryPoint) ||
        !IsIa64ExternalPreemptionSentinelRestartInstructionSlot(interruptedInstructionSlot) ||
        ((control.SentinelEnd - control.SentinelEntryPoint) != 16U))
    {
        return Ia64ExternalPreemptionControlError::InvalidSentinelState;
    }

    Foundation::UInt64 expected = static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::Armed);
    const Foundation::UInt64 desired = static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::Capturing);
    if (!__atomic_compare_exchange_n(&control.State, &expected, desired, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
        return Ia64ExternalPreemptionControlError::InvalidState;
    }
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionControlError
ModelPublishIa64ExternalPreemptionControlCapture(Ia64ExternalPreemptionControl& control,
                                                 const Ia64ExternalPreemptionFrame& frame) noexcept
{
    if (AcquireIa64ExternalPreemptionControlState(control) != Ia64ExternalPreemptionControlState::Capturing ||
        AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::BspOneShotPreemption)
    {
        return Ia64ExternalPreemptionControlError::InvalidState;
    }
    if (!ConfigurationMatchesFrame(control, frame))
    {
        return Ia64ExternalPreemptionControlError::InvalidConfiguration;
    }
    if (ValidateIa64ExternalPreemptionCoveredRseState(control, frame) != Ia64ExternalPreemptionCoveredRseError::None)
    {
        return Ia64ExternalPreemptionControlError::FrameStateMismatch;
    }
    const Foundation::UInt64 token = AcquireIa64ExternalPreemptionToken(frame);
    if ((token == 0U) || (frame.CaptureToken != token) || (frame.State != Ia64ExternalPreemptionFrameState::Retained) ||
        (frame.ProcessorIndex != control.ProcessorIndex) || (frame.TimerVector != control.TimerVector) ||
        (frame.ProgrammedDeadlineTicks != control.ArmedDeadlineTicks))
    {
        return Ia64ExternalPreemptionControlError::FrameStateMismatch;
    }

    control.CapturedFrameToken = token;
    ++control.CaptureCount;
    Synchronization::AtomicUInt64Reference state{control.State};
    state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::Coordinator));
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionControlError CommitIa64ExternalPreemptionControlForRestore(
    Ia64ExternalPreemptionControl& control, const Ia64ExternalPreemptionFrame& frame,
    const Foundation::UInt64 frameToken, const Foundation::UInt64 idleContinuationGeneration) noexcept
{
    if (AcquireIa64ExternalPreemptionControlState(control) != Ia64ExternalPreemptionControlState::Coordinator ||
        AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::BspOneShotPreemption)
    {
        return Ia64ExternalPreemptionControlError::InvalidState;
    }
    if (!ConfigurationMatchesFrame(control, frame))
    {
        return Ia64ExternalPreemptionControlError::InvalidConfiguration;
    }
    if ((frameToken == 0U) || (idleContinuationGeneration == 0U) || (control.CapturedFrameToken != frameToken) ||
        (AcquireIa64ExternalPreemptionToken(frame) != frameToken))
    {
        return Ia64ExternalPreemptionControlError::TokenMismatch;
    }
    if (frame.State != Ia64ExternalPreemptionFrameState::ResumeCommitted)
    {
        return Ia64ExternalPreemptionControlError::FrameStateMismatch;
    }

    control.IdleContinuationGeneration = idleContinuationGeneration;
    Synchronization::AtomicUInt64Reference mode{control.Mode};
    mode.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionMode::LegacyEvidence));
    Synchronization::AtomicUInt64Reference state{control.State};
    state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::ResumeCommitted));
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionControlError
PrepareIa64ExternalPreemptionSentinelResumeProcessorStatus(const Ia64ExternalPreemptionControl& control,
                                                           const Ia64ExternalPreemptionFrame& frame,
                                                           Foundation::UInt64& resumeProcessorStatus) noexcept
{
    if (AcquireIa64ExternalPreemptionControlState(control) != Ia64ExternalPreemptionControlState::ResumeCommitted ||
        AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::LegacyEvidence)
    {
        return Ia64ExternalPreemptionControlError::InvalidState;
    }
    if (!ConfigurationMatchesFrame(control, frame))
    {
        return Ia64ExternalPreemptionControlError::InvalidConfiguration;
    }
    const Foundation::UInt64 token = AcquireIa64ExternalPreemptionToken(frame);
    if ((token == 0U) || (frame.State != Ia64ExternalPreemptionFrameState::ResumeCommitted) ||
        (frame.CaptureToken != token) || (control.CapturedFrameToken != token))
    {
        return Ia64ExternalPreemptionControlError::TokenMismatch;
    }
    const Foundation::UInt64 interruptedInstructionSlot =
        (frame.InterruptedProcessorStatus & Ia64ExternalInterruptInstructionSlotMask) >> 41U;
    if ((frame.InterruptedInstructionPointer != control.SentinelEntryPoint) ||
        !IsIa64ExternalPreemptionSentinelRestartInstructionSlot(interruptedInstructionSlot) ||
        ((frame.InterruptedProcessorStatus & InterruptionEnableMask) == 0U))
    {
        return Ia64ExternalPreemptionControlError::InvalidSentinelState;
    }

    resumeProcessorStatus = frame.InterruptedProcessorStatus & ~InterruptionEnableMask;
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionControlError
ModelCompleteIa64ExternalPreemptionControlForRfi(Ia64ExternalPreemptionControl& control,
                                                 const Foundation::UInt64 frameToken,
                                                 const Foundation::UInt64 idleContinuationGeneration) noexcept
{
    if (AcquireIa64ExternalPreemptionControlState(control) != Ia64ExternalPreemptionControlState::ResumeCommitted ||
        AcquireIa64ExternalPreemptionMode(control) != Ia64ExternalPreemptionMode::LegacyEvidence)
    {
        return Ia64ExternalPreemptionControlError::InvalidState;
    }
    if ((frameToken == 0U) || (idleContinuationGeneration == 0U) || (control.CapturedFrameToken != frameToken) ||
        (control.IdleContinuationGeneration != idleContinuationGeneration))
    {
        return Ia64ExternalPreemptionControlError::TokenMismatch;
    }

    control.ArmedDeadlineTicks = 0U;
    ++control.RestoreCount;
    Synchronization::AtomicUInt64Reference state{control.State};
    state.StoreRelease(static_cast<Foundation::UInt64>(Ia64ExternalPreemptionControlState::Complete));
    return Ia64ExternalPreemptionControlError::None;
}

Ia64ExternalPreemptionMode AcquireIa64ExternalPreemptionMode(const Ia64ExternalPreemptionControl& control) noexcept
{
    return static_cast<Ia64ExternalPreemptionMode>(Synchronization::AtomicUInt64Reference::LoadAcquire(control.Mode));
}

Ia64ExternalPreemptionControlState
AcquireIa64ExternalPreemptionControlState(const Ia64ExternalPreemptionControl& control) noexcept
{
    return static_cast<Ia64ExternalPreemptionControlState>(
        Synchronization::AtomicUInt64Reference::LoadAcquire(control.State));
}

Ia64ExternalPreemptionControlSnapshot
AcquireIa64ExternalPreemptionControlSnapshot(const Ia64ExternalPreemptionControl& control) noexcept
{
    const Ia64ExternalPreemptionMode mode = AcquireIa64ExternalPreemptionMode(control);
    const Ia64ExternalPreemptionControlState state = AcquireIa64ExternalPreemptionControlState(control);
    return Ia64ExternalPreemptionControlSnapshot{
        .Configuration = ReadConfiguration(control),
        .ArmedDeadlineTicks = control.ArmedDeadlineTicks,
        .CapturedFrameToken = control.CapturedFrameToken,
        .IdleContinuationGeneration = control.IdleContinuationGeneration,
        .CaptureCount = control.CaptureCount,
        .RestoreCount = control.RestoreCount,
        .LastFailure = control.LastFailure,
        .Mode = mode,
        .State = state,
    };
}

Ia64ExternalPreemptionCodeRange GetIa64ExternalPreemptionCodeRange() noexcept
{
    Ia64ExternalPreemptionCodeRange range{};
    const Foundation::UInt64 outputAddress =
        static_cast<Foundation::UInt64>(reinterpret_cast<Foundation::UIntPointer>(&range));
    if (AsteriaIa64GetExternalPreemptionCodeRangeWords(outputAddress) != 0U)
    {
        return Ia64ExternalPreemptionCodeRange{};
    }
    return range;
}

Ia64ExternalPreemptionCoveredRseError
ValidateIa64ExternalPreemptionCoveredRseState(const Ia64ExternalPreemptionControl& control,
                                              const Ia64ExternalPreemptionFrame& frame) noexcept
{
    constexpr Foundation::UInt64 ifsValidBit{Foundation::UInt64{1U} << 63U};
    constexpr Foundation::UInt64 cfmMask{(Foundation::UInt64{1U} << 38U) - 1U};
    const Foundation::UInt64 ifs = frame.CoveredInterruptionFunctionState;
    if ((ifs & ~(ifsValidBit | cfmMask)) != 0U || (ifs & ifsValidBit) == 0U)
    {
        return Ia64ExternalPreemptionCoveredRseError::InvalidFunctionState;
    }
    const Foundation::UInt64 sof = ifs & 0x7FU;
    const Foundation::UInt64 sol = (ifs >> 7U) & 0x7FU;
    const Foundation::UInt64 sor = ((ifs >> 14U) & 0xFU) * 8U;
    const Foundation::UInt64 rrbGr = (ifs >> 18U) & 0x7FU;
    const Foundation::UInt64 rrbFr = (ifs >> 25U) & 0x7FU;
    const Foundation::UInt64 rrbPr = (ifs >> 32U) & 0x3FU;
    if ((sof > 96U) || (sol > sof) || (sor > sof) || ((sor == 0U) ? (rrbGr != 0U) : (rrbGr >= sor)) || (rrbFr >= 96U) ||
        (rrbPr >= 48U))
    {
        return Ia64ExternalPreemptionCoveredRseError::InvalidFunctionState;
    }

    Foundation::UInt64 expectedCoveredPointer{0U};
    if (!TryAdvanceRsePointer(frame.RseBackingStorePointer, sof, expectedCoveredPointer) ||
        ((frame.CoveredRseBackingStorePointer & 7U) != 0U))
    {
        return Ia64ExternalPreemptionCoveredRseError::InvalidPointer;
    }
    if (frame.CoveredRseBackingStorePointer != expectedCoveredPointer)
    {
        return Ia64ExternalPreemptionCoveredRseError::CoveredPointerMismatch;
    }
    if ((frame.RseBackingStorePointer < control.WorkerRseBackingStoreStart) ||
        (frame.RseBackingStorePointer >= control.WorkerRseBackingStoreEnd) ||
        (frame.CoveredRseBackingStorePointer < frame.RseBackingStorePointer) ||
        (frame.CoveredRseBackingStorePointer > control.WorkerRseBackingStoreEnd))
    {
        return Ia64ExternalPreemptionCoveredRseError::CoveredPointerOutsideWorkerRange;
    }

    const Foundation::UInt64 encoded = frame.RseDirtyPartitionLoadConfiguration;
    const Foundation::UInt64 loadBytes = encoded >> 16U;
    if (((encoded & 0xFFFFU) != 0U) || ((loadBytes & 7U) != 0U) ||
        (loadBytes >= Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes))
    {
        return Ia64ExternalPreemptionCoveredRseError::InvalidLoadConfiguration;
    }
    return Ia64ExternalPreemptionCoveredRseError::None;
}
} // namespace Asteria::Platform::Ia64::Interrupt
// The target routines are in Ia64ExternalPreemptionEntry.S.
#if !defined(__ia64__)
extern "C" [[noreturn]] void AsteriaIa64ExternalInterruptDispatchEntry() noexcept
{
    __builtin_trap();
}

extern "C" [[noreturn]] void AsteriaIa64ExternalPreemptionNativeEntry() noexcept
{
    __builtin_trap();
}

extern "C" Asteria::Foundation::UInt64
AsteriaIa64GetExternalPreemptionCodeRangeWords(const Asteria::Foundation::UInt64 outputAddress) noexcept
{
    static_cast<void>(outputAddress);
    return 1U;
}
#endif
