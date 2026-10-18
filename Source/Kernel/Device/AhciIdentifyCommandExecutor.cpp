// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/AhciIdentifyCommandExecutor.hpp"
#include "Kernel/Device/Detail/AhciIdentifyDeviceInformationParser.hpp"

namespace Asteria::Kernel::Device
{
namespace
{
constexpr Foundation::UInt64 MaximumUInt64{~Foundation::UInt64{0U}};
constexpr Foundation::UInt64 MaximumUInt32{0xFFFF'FFFFULL};
constexpr Foundation::UIntPointer MaximumUIntPointer{~Foundation::UIntPointer{0U}};
constexpr Foundation::UInt64 RegisterWidthBytes{4U};
constexpr Foundation::UInt64 CapabilitiesOffset{0x00U};
constexpr Foundation::UInt64 GlobalHostControlOffset{0x04U};
constexpr Foundation::UInt64 GlobalInterruptStatusOffset{0x08U};
constexpr Foundation::UInt64 PortsImplementedOffset{0x0CU};
constexpr Foundation::UInt64 PortRegistersOffset{0x100U};
constexpr Foundation::UInt64 PortRegisterStrideBytes{0x80U};
constexpr Foundation::UInt64 PortCommandListBaseOffset{0x00U};
constexpr Foundation::UInt64 PortCommandListBaseUpperOffset{0x04U};
constexpr Foundation::UInt64 PortFisBaseOffset{0x08U};
constexpr Foundation::UInt64 PortFisBaseUpperOffset{0x0CU};
constexpr Foundation::UInt64 PortInterruptStatusOffset{0x10U};
constexpr Foundation::UInt64 PortInterruptEnableOffset{0x14U};
constexpr Foundation::UInt64 PortCommandOffset{0x18U};
constexpr Foundation::UInt64 PortTaskFileDataOffset{0x20U};
constexpr Foundation::UInt64 PortSignatureOffset{0x24U};
constexpr Foundation::UInt64 PortSataStatusOffset{0x28U};
constexpr Foundation::UInt64 PortSataErrorOffset{0x30U};
constexpr Foundation::UInt64 PortSataActiveOffset{0x34U};
constexpr Foundation::UInt64 PortCommandIssueOffset{0x38U};

constexpr Foundation::UInt32 AhciEnableMask{Foundation::UInt32{1U} << 31U};
constexpr Foundation::UInt32 Supports64BitAddressingMask{Foundation::UInt32{1U} << 31U};
constexpr Foundation::UInt32 PortCountMask{0x1FU};
constexpr Foundation::UInt32 PortCommandStartMask{Foundation::UInt32{1U}};
constexpr Foundation::UInt32 PortCommandListOverrideMask{Foundation::UInt32{1U} << 3U};
constexpr Foundation::UInt32 PortCommandInterfaceCommunicationControlMask{0xFU << 28U};
constexpr Foundation::UInt32 PortCommandActionMask{PortCommandListOverrideMask |
                                                   PortCommandInterfaceCommunicationControlMask};
constexpr Foundation::UInt32 PortCommandFisReceiveEnableMask{Foundation::UInt32{1U} << 4U};
constexpr Foundation::UInt32 PortCommandFisReceiveRunningMask{Foundation::UInt32{1U} << 14U};
constexpr Foundation::UInt32 PortCommandListRunningMask{Foundation::UInt32{1U} << 15U};
constexpr Foundation::UInt32 PortCommandReadOnlyEngineMask{PortCommandFisReceiveRunningMask |
                                                           PortCommandListRunningMask};
constexpr Foundation::UInt32 TaskFileBusyMask{Foundation::UInt32{1U} << 7U};
constexpr Foundation::UInt32 TaskFileDataRequestMask{Foundation::UInt32{1U} << 3U};
constexpr Foundation::UInt32 TaskFileDeviceFaultMask{Foundation::UInt32{1U} << 5U};
constexpr Foundation::UInt32 TaskFileErrorMask{Foundation::UInt32{1U}};
constexpr Foundation::UInt32 TaskFileFailureMask{TaskFileDeviceFaultMask | TaskFileErrorMask};
constexpr Foundation::UInt32 PortInterruptFatalMask{0x7D00'0010U};
constexpr Foundation::UInt32 SataStatusDetectionMask{0x0FU};
constexpr Foundation::UInt32 SataStatusInterfacePowerMask{0x0F00U};
constexpr Foundation::UInt32 SataStatusActive{0x0103U};
constexpr Foundation::UInt32 AtaDeviceSignature{0x0000'0101U};
constexpr Foundation::UInt32 AtapiDeviceSignature{0xEB14'0101U};
constexpr Foundation::UInt8 InvalidPortNumber{0xFFU};
constexpr Foundation::Size CommandHeaderByteCountOffsetBytes{4U};
constexpr Foundation::UInt32 IdentifyTransferSizeBytes{512U};
constexpr Foundation::Size DeviceToHostFisOffsetBytes{0x40U};
constexpr Foundation::UInt8 DeviceToHostFisType{0x34U};
// IDENTIFY DEVICE is a PIO data-in command: real drives and other hypervisors
// end it with a PIO Setup FIS whose E_Status byte is the final status, and
// send no Register D2H FIS (QEMU sends both).
constexpr Foundation::Size PioSetupFisOffsetBytes{0x20U};
constexpr Foundation::UInt8 PioSetupFisType{0x5FU};
constexpr Foundation::Size PioSetupEndStatusOffsetBytes{15U};

enum class PollDecision : Foundation::UInt8
{
    Continue,
    LimitReached,
    DeadlineReached,
    ClockExhausted,
    ClockReversed,
};

struct OriginalPortState
{
    Foundation::UInt32 CommandListBase;
    Foundation::UInt32 CommandListBaseUpper;
    Foundation::UInt32 FisBase;
    Foundation::UInt32 FisBaseUpper;
    Foundation::UInt32 InterruptEnable;
    Foundation::UInt32 Command;
};

struct ExecutionState
{
    AhciIdentifyCommandFailure Failure;
    OriginalPortState Original;
    Foundation::UInt64 PortBaseAddress;
    Foundation::UInt32 Capabilities;
    Foundation::UInt32 PortsImplemented;
    Foundation::UInt32 CurrentCommand;
    Foundation::UInt32 CompletionInterruptStatus;
    Foundation::UInt32 CompletionTaskFileData;
    Foundation::UInt8 DeviceToHostFisStatus;
    Foundation::UInt8 DeviceToHostFisError;
    bool ConfigurationInstalled;
    bool CommandIssueAttempted;
    bool CommandCompleted;
    bool CompletionStatusValidated;
};

class PollBudget final
{
public:
    constexpr PollBudget(const Hal::Time::IMonotonicClock& clock, const AhciIdentifyPollingPolicy& policy) noexcept
        : m_clock{clock}, m_deadlineTicks{policy.DeadlineTicks}, m_maximumPollCount{policy.MaximumPollCount},
          m_maximumRecoveryPollCount{policy.MaximumRecoveryPollCount}
    {
    }

    [[nodiscard]] PollDecision Start() noexcept
    {
        const Foundation::UInt64 observed = m_clock.GetTicks();
        m_lastObservedTicks = observed;
        m_startTicks = observed;
        if (observed == MaximumUInt64)
        {
            return PollDecision::ClockExhausted;
        }
        if (observed >= m_deadlineTicks)
        {
            return PollDecision::DeadlineReached;
        }
        return PollDecision::Continue;
    }

    [[nodiscard]] PollDecision BeginPoll() noexcept
    {
        if (m_pollCount >= m_maximumPollCount)
        {
            return PollDecision::LimitReached;
        }
        ++m_pollCount;
        const Foundation::UInt64 observed = m_clock.GetTicks();
        if (observed == MaximumUInt64)
        {
            m_lastObservedTicks = observed;
            return PollDecision::ClockExhausted;
        }
        if (observed < m_lastObservedTicks)
        {
            m_lastObservedTicks = observed;
            return PollDecision::ClockReversed;
        }
        m_lastObservedTicks = observed;
        if (observed >= m_deadlineTicks)
        {
            return PollDecision::DeadlineReached;
        }
        return PollDecision::Continue;
    }

    [[nodiscard]] PollDecision Checkpoint() noexcept
    {
        const Foundation::UInt64 observed = m_clock.GetTicks();
        if (observed == MaximumUInt64)
        {
            m_lastObservedTicks = observed;
            return PollDecision::ClockExhausted;
        }
        if (observed < m_lastObservedTicks)
        {
            m_lastObservedTicks = observed;
            return PollDecision::ClockReversed;
        }
        m_lastObservedTicks = observed;
        if (observed >= m_deadlineTicks)
        {
            return PollDecision::DeadlineReached;
        }
        return PollDecision::Continue;
    }

    [[nodiscard]] PollDecision BeginRecoveryPoll() noexcept
    {
        if (m_recoveryPollCount >= m_maximumRecoveryPollCount)
        {
            return PollDecision::LimitReached;
        }
        ++m_recoveryPollCount;
        return PollDecision::Continue;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetStartTicks() const noexcept
    {
        return m_startTicks;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetLastObservedTicks() const noexcept
    {
        return m_lastObservedTicks;
    }

    [[nodiscard]] constexpr Foundation::Size GetPollCount() const noexcept
    {
        return m_pollCount + m_recoveryPollCount;
    }

private:
    const Hal::Time::IMonotonicClock& m_clock;
    Foundation::UInt64 m_deadlineTicks;
    Foundation::Size m_maximumPollCount;
    Foundation::Size m_maximumRecoveryPollCount;
    Foundation::UInt64 m_startTicks{0U};
    Foundation::UInt64 m_lastObservedTicks{0U};
    Foundation::Size m_pollCount{0U};
    Foundation::Size m_recoveryPollCount{0U};
};

[[nodiscard]] constexpr Foundation::UInt64 PortRegisterOffset(const Foundation::UInt8 portNumber,
                                                              const Foundation::UInt64 registerOffset) noexcept
{
    return PortRegistersOffset + (static_cast<Foundation::UInt64>(portNumber) * PortRegisterStrideBytes) +
           registerOffset;
}

[[nodiscard]] constexpr bool ContainsRegister(const AhciHbaRegisterRange range,
                                              const Foundation::UInt64 offset) noexcept
{
    return (range.LengthBytes >= RegisterWidthBytes) && (offset <= (range.LengthBytes - RegisterWidthBytes));
}

[[nodiscard]] constexpr bool IsValidRange(const AhciHbaRegisterRange range) noexcept
{
    return (range.BaseAddress != 0U) && ((range.BaseAddress & (RegisterWidthBytes - 1U)) == 0U) &&
           (range.LengthBytes != 0U) && (range.BaseAddress <= (MaximumUInt64 - range.LengthBytes)) &&
           ContainsRegister(range, PortsImplementedOffset);
}

[[nodiscard]] constexpr bool IsActiveSataLink(const Foundation::UInt32 sataStatus) noexcept
{
    return (sataStatus & (SataStatusDetectionMask | SataStatusInterfacePowerMask)) == SataStatusActive;
}

[[nodiscard]] bool RangesOverlap(const void* const first, const Foundation::Size firstSize, const void* const second,
                                 const Foundation::Size secondSize) noexcept
{
    if ((firstSize == 0U) || (secondSize == 0U))
    {
        return false;
    }

    const Foundation::UIntPointer firstStart = reinterpret_cast<Foundation::UIntPointer>(first);
    const Foundation::UIntPointer secondStart = reinterpret_cast<Foundation::UIntPointer>(second);
    const Foundation::UIntPointer firstLength = static_cast<Foundation::UIntPointer>(firstSize - 1U);
    const Foundation::UIntPointer secondLength = static_cast<Foundation::UIntPointer>(secondSize - 1U);
    if ((firstStart > (MaximumUIntPointer - firstLength)) || (secondStart > (MaximumUIntPointer - secondLength)))
    {
        return true;
    }

    const Foundation::UIntPointer firstEnd = firstStart + firstLength;
    const Foundation::UIntPointer secondEnd = secondStart + secondLength;
    return (firstStart <= secondEnd) && (secondStart <= firstEnd);
}

void SetPrimaryError(ExecutionState& state, const AhciIdentifyCommandError error, const AhciIdentifyCommandPhase phase,
                     const AhciIdentifyRegister reg = AhciIdentifyRegister::None) noexcept
{
    if (state.Failure.Error == AhciIdentifyCommandError::None)
    {
        state.Failure.Error = error;
        state.Failure.Phase = phase;
        state.Failure.Register = reg;
    }
}

void SetRecoveryError(ExecutionState& state, const AhciIdentifyCommandError error,
                      const AhciIdentifyRegister reg = AhciIdentifyRegister::None) noexcept
{
    if (state.Failure.Error == AhciIdentifyCommandError::None)
    {
        state.Failure.Error = AhciIdentifyCommandError::EngineRestoreFailed;
        state.Failure.Phase = AhciIdentifyCommandPhase::PortRestoration;
        state.Failure.Register = reg;
    }
    if (state.Failure.RecoveryError == AhciIdentifyCommandError::None)
    {
        state.Failure.RecoveryError = error;
        state.Failure.RecoveryRegister = reg;
    }
}

[[nodiscard]] bool ReadRegister(Hal::Device::IMemoryMappedIo& memoryMappedIo, ExecutionState& state,
                                const Foundation::UInt64 physicalAddress, const AhciIdentifyRegister reg,
                                const AhciIdentifyCommandPhase phase, Foundation::UInt32& output,
                                const bool recovery = false) noexcept
{
    Foundation::UInt64 value{0U};
    const Hal::Device::MemoryMappedIoReadError readError =
        memoryMappedIo.Read(physicalAddress, Hal::Device::MemoryMappedIoWidth::DoubleWord, value);
    if (readError != Hal::Device::MemoryMappedIoReadError::None)
    {
        if (recovery)
        {
            SetRecoveryError(state, AhciIdentifyCommandError::MemoryMappedIoReadFailed, reg);
            state.Failure.RecoveryMemoryMappedIoRead = readError;
        }
        else
        {
            SetPrimaryError(state, AhciIdentifyCommandError::MemoryMappedIoReadFailed, phase, reg);
            state.Failure.MemoryMappedIoRead = readError;
        }
        return false;
    }
    if (value > MaximumUInt32)
    {
        if (recovery)
        {
            SetRecoveryError(state, AhciIdentifyCommandError::InvalidRegisterValue, reg);
        }
        else
        {
            SetPrimaryError(state, AhciIdentifyCommandError::InvalidRegisterValue, phase, reg);
        }
        return false;
    }
    output = static_cast<Foundation::UInt32>(value);
    return true;
}

[[nodiscard]] bool WriteRegister(Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter, ExecutionState& state,
                                 const Foundation::UInt64 physicalAddress, const AhciIdentifyRegister reg,
                                 const AhciIdentifyCommandPhase phase, const Foundation::UInt32 value,
                                 const bool recovery = false) noexcept
{
    const Hal::Device::MemoryMappedIoWriteError writeError =
        memoryMappedIoWriter.Write(physicalAddress, Hal::Device::MemoryMappedIoWidth::DoubleWord, value);
    if (writeError == Hal::Device::MemoryMappedIoWriteError::None)
    {
        return true;
    }

    if (recovery)
    {
        SetRecoveryError(state, AhciIdentifyCommandError::MemoryMappedIoWriteFailed, reg);
        state.Failure.RecoveryMemoryMappedIoWrite = writeError;
    }
    else
    {
        SetPrimaryError(state, AhciIdentifyCommandError::MemoryMappedIoWriteFailed, phase, reg);
        state.Failure.MemoryMappedIoWrite = writeError;
    }
    return false;
}

void RecordPollFailure(ExecutionState& state, const PollDecision decision, const AhciIdentifyCommandError timeoutError,
                       const AhciIdentifyCommandPhase phase, const AhciIdentifyRegister reg,
                       const bool recovery) noexcept
{
    AhciIdentifyCommandError error{timeoutError};
    switch (decision)
    {
    case PollDecision::Continue:
        return;
    case PollDecision::LimitReached:
    case PollDecision::DeadlineReached:
        error = timeoutError;
        break;
    case PollDecision::ClockExhausted:
        error = AhciIdentifyCommandError::ClockExhausted;
        break;
    case PollDecision::ClockReversed:
        error = AhciIdentifyCommandError::ClockReversed;
        break;
    }

    if (recovery)
    {
        SetRecoveryError(state, error, reg);
    }
    else
    {
        SetPrimaryError(state, error, phase, reg);
    }
}

void RecordDeadlineFailure(ExecutionState& state, const PollDecision decision, const AhciIdentifyCommandPhase phase,
                           const AhciIdentifyRegister reg = AhciIdentifyRegister::None) noexcept
{
    AhciIdentifyCommandError error{AhciIdentifyCommandError::DeadlineReached};
    if (decision == PollDecision::ClockExhausted)
    {
        error = AhciIdentifyCommandError::ClockExhausted;
    }
    else if (decision == PollDecision::ClockReversed)
    {
        error = AhciIdentifyCommandError::ClockReversed;
    }
    SetPrimaryError(state, error, phase, reg);
}

[[nodiscard]] bool PollCommandMask(Hal::Device::IMemoryMappedIo& memoryMappedIo, ExecutionState& state,
                                   PollBudget& budget, const Foundation::UInt32 mask, const Foundation::UInt32 expected,
                                   const AhciIdentifyCommandError timeoutError, const AhciIdentifyCommandPhase phase,
                                   const bool recovery) noexcept
{
    while ((state.CurrentCommand & mask) != expected)
    {
        const PollDecision decision = recovery ? budget.BeginRecoveryPoll() : budget.BeginPoll();
        if (decision != PollDecision::Continue)
        {
            RecordPollFailure(state, decision, timeoutError, phase, AhciIdentifyRegister::PortCommand, recovery);
            return false;
        }
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortCommandOffset,
                          AhciIdentifyRegister::PortCommand, phase, state.CurrentCommand, recovery))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool StopEngine(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                              Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter, ExecutionState& state,
                              PollBudget& budget, const bool recovery) noexcept
{
    const AhciIdentifyCommandPhase phase =
        recovery ? AhciIdentifyCommandPhase::PortRestoration : AhciIdentifyCommandPhase::PortPreparation;
    const AhciIdentifyCommandError timeoutError =
        recovery ? AhciIdentifyCommandError::EngineRestoreFailed : AhciIdentifyCommandError::EngineStopTimedOut;
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortCommandOffset,
                      AhciIdentifyRegister::PortCommand, phase, state.CurrentCommand, recovery))
    {
        state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
        return false;
    }

    const Foundation::UInt32 configuration =
        state.Original.Command & ~(PortCommandStartMask | PortCommandActionMask | PortCommandFisReceiveEnableMask |
                                   PortCommandReadOnlyEngineMask);
    if ((state.CurrentCommand & PortCommandStartMask) != 0U)
    {
        const Foundation::UInt32 stoppedCommand =
            configuration | (state.CurrentCommand & PortCommandFisReceiveEnableMask);
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciIdentifyRegister::PortCommand, phase, stoppedCommand, recovery))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        state.Failure.PortDisposition = AhciIdentifyPortDisposition::StopRequested;
        state.CurrentCommand = stoppedCommand | (state.CurrentCommand & PortCommandReadOnlyEngineMask);
    }
    if (!PollCommandMask(memoryMappedIo, state, budget, PortCommandListRunningMask, 0U, timeoutError, phase, recovery))
    {
        return false;
    }

    if ((state.CurrentCommand & PortCommandFisReceiveEnableMask) != 0U)
    {
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciIdentifyRegister::PortCommand, phase, configuration, recovery))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        state.Failure.PortDisposition = AhciIdentifyPortDisposition::StopRequested;
        state.CurrentCommand = configuration | (state.CurrentCommand & PortCommandReadOnlyEngineMask);
    }
    if (!PollCommandMask(memoryMappedIo, state, budget, PortCommandFisReceiveRunningMask, 0U, timeoutError, phase,
                         recovery))
    {
        return false;
    }

    state.CurrentCommand = configuration;
    return true;
}

[[nodiscard]] bool AcknowledgeStatus(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                     Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter, ExecutionState& state,
                                     const AhciHbaRegisterRange range, const bool recovery) noexcept
{
    const AhciIdentifyCommandPhase phase =
        recovery ? AhciIdentifyCommandPhase::PortRestoration : AhciIdentifyCommandPhase::PortPreparation;
    Foundation::UInt32 interruptStatus{0U};
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortInterruptStatusOffset,
                      AhciIdentifyRegister::PortInterruptStatus, phase, interruptStatus, recovery) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortInterruptStatusOffset,
                       AhciIdentifyRegister::PortInterruptStatus, phase, interruptStatus, recovery))
    {
        return false;
    }

    Foundation::UInt32 sataError{0U};
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortSataErrorOffset,
                      AhciIdentifyRegister::PortSataError, phase, sataError, recovery) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortSataErrorOffset,
                       AhciIdentifyRegister::PortSataError, phase, sataError, recovery))
    {
        return false;
    }

    Foundation::UInt32 globalInterruptStatus{0U};
    const Foundation::UInt32 selectedPortMask = Foundation::UInt32{1U} << state.Failure.PortNumber;
    return ReadRegister(memoryMappedIo, state, range.BaseAddress + GlobalInterruptStatusOffset,
                        AhciIdentifyRegister::GlobalInterruptStatus, phase, globalInterruptStatus, recovery) &&
           WriteRegister(memoryMappedIoWriter, state, range.BaseAddress + GlobalInterruptStatusOffset,
                         AhciIdentifyRegister::GlobalInterruptStatus, phase, globalInterruptStatus & selectedPortMask,
                         recovery);
}

[[nodiscard]] bool InstallCommandConfiguration(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                               Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter,
                                               ExecutionState& state, const AhciHbaRegisterRange range,
                                               const Foundation::UInt64 dmaPhysicalAddress) noexcept
{
    const AhciIdentifyCommandPhase phase = AhciIdentifyCommandPhase::PortPreparation;
    const Foundation::UInt32 commandListLow = static_cast<Foundation::UInt32>(
        (dmaPhysicalAddress + AhciIdentifyDmaPageBuilder::CommandListOffsetBytes) & MaximumUInt32);
    const Foundation::UInt32 commandListHigh = static_cast<Foundation::UInt32>(
        (dmaPhysicalAddress + AhciIdentifyDmaPageBuilder::CommandListOffsetBytes) >> 32U);
    const Foundation::UInt32 fisLow = static_cast<Foundation::UInt32>(
        (dmaPhysicalAddress + AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes) & MaximumUInt32);
    const Foundation::UInt32 fisHigh = static_cast<Foundation::UInt32>(
        (dmaPhysicalAddress + AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes) >> 32U);

    // A failed backend write need not prove that no device-side change was
    // observed, so conservative recovery begins with the first attempt.
    state.ConfigurationInstalled = true;
    if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortInterruptEnableOffset,
                       AhciIdentifyRegister::PortInterruptEnable, phase, 0U))
    {
        return false;
    }

    // From the first CLB write attempt onward, an error can leave a DMA page
    // address installed even when the backend reports that the write failed.
    state.Failure.DmaPageMustRemainLive = true;
    if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseOffset,
                       AhciIdentifyRegister::PortCommandListBase, phase, commandListLow) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseUpperOffset,
                       AhciIdentifyRegister::PortCommandListBaseUpper, phase, commandListHigh) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseOffset,
                       AhciIdentifyRegister::PortFisBase, phase, fisLow) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseUpperOffset,
                       AhciIdentifyRegister::PortFisBaseUpper, phase, fisHigh))
    {
        return false;
    }
    return AcknowledgeStatus(memoryMappedIo, memoryMappedIoWriter, state, range, false);
}

[[nodiscard]] bool StartCommandEngine(Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter,
                                      ExecutionState& state) noexcept
{
    const Foundation::UInt32 configuration =
        state.Original.Command & ~(PortCommandStartMask | PortCommandActionMask | PortCommandFisReceiveEnableMask |
                                   PortCommandReadOnlyEngineMask);
    const Foundation::UInt32 fisRunning = configuration | PortCommandFisReceiveEnableMask;
    const Foundation::UInt32 commandRunning = fisRunning | PortCommandStartMask;
    if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                       AhciIdentifyRegister::PortCommand, AhciIdentifyCommandPhase::PortPreparation, fisRunning) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                       AhciIdentifyRegister::PortCommand, AhciIdentifyCommandPhase::PortPreparation, commandRunning))
    {
        return false;
    }
    state.CurrentCommand = commandRunning;
    return true;
}

void ValidateCompletionStatus(ExecutionState& state) noexcept
{
    if ((state.CompletionInterruptStatus & PortInterruptFatalMask) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::PortInterruptError, AhciIdentifyCommandPhase::CommandExecution,
                        AhciIdentifyRegister::PortInterruptStatus);
    }
    if ((state.CompletionTaskFileData & TaskFileFailureMask) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::TaskFileError, AhciIdentifyCommandPhase::CommandExecution,
                        AhciIdentifyRegister::PortTaskFileData);
    }
    else if ((state.CompletionTaskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::PortBusy, AhciIdentifyCommandPhase::CommandExecution,
                        AhciIdentifyRegister::PortTaskFileData);
    }
}

[[nodiscard]] bool CaptureFinalCompletionStatus(Hal::Device::IMemoryMappedIo& memoryMappedIo, ExecutionState& state,
                                                const bool recovery) noexcept
{
    const AhciIdentifyCommandPhase phase =
        recovery ? AhciIdentifyCommandPhase::PortRestoration : AhciIdentifyCommandPhase::CommandExecution;
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortInterruptStatusOffset,
                      AhciIdentifyRegister::PortInterruptStatus, phase, state.CompletionInterruptStatus, recovery))
    {
        return false;
    }
    state.Failure.PortInterruptStatus = state.CompletionInterruptStatus;
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                      AhciIdentifyRegister::PortTaskFileData, phase, state.CompletionTaskFileData, recovery))
    {
        return false;
    }
    state.Failure.TaskFileData = state.CompletionTaskFileData;
    state.CompletionStatusValidated = true;
    ValidateCompletionStatus(state);
    return true;
}

[[nodiscard]] bool RestoreOriginalConfiguration(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                                Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter,
                                                ExecutionState& state, PollBudget& budget,
                                                const AhciHbaRegisterRange range,
                                                const bool restartCommandEngine) noexcept
{
    if (!AcknowledgeStatus(memoryMappedIo, memoryMappedIoWriter, state, range, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseOffset,
                       AhciIdentifyRegister::PortCommandListBase, AhciIdentifyCommandPhase::PortRestoration,
                       state.Original.CommandListBase, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseUpperOffset,
                       AhciIdentifyRegister::PortCommandListBaseUpper, AhciIdentifyCommandPhase::PortRestoration,
                       state.Original.CommandListBaseUpper, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseOffset,
                       AhciIdentifyRegister::PortFisBase, AhciIdentifyCommandPhase::PortRestoration,
                       state.Original.FisBase, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseUpperOffset,
                       AhciIdentifyRegister::PortFisBaseUpper, AhciIdentifyCommandPhase::PortRestoration,
                       state.Original.FisBaseUpper, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortInterruptEnableOffset,
                       AhciIdentifyRegister::PortInterruptEnable, AhciIdentifyCommandPhase::PortRestoration,
                       state.Original.InterruptEnable, true))
    {
        state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
        return false;
    }

    const Foundation::UInt32 configuration =
        state.Original.Command & ~(PortCommandStartMask | PortCommandActionMask | PortCommandFisReceiveEnableMask |
                                   PortCommandReadOnlyEngineMask);
    if ((state.Original.Command & PortCommandFisReceiveEnableMask) != 0U)
    {
        state.CurrentCommand = configuration | PortCommandFisReceiveEnableMask;
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciIdentifyRegister::PortCommand, AhciIdentifyCommandPhase::PortRestoration,
                           state.CurrentCommand, true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
    }
    else
    {
        state.CurrentCommand = configuration;
    }

    if ((state.Original.Command & PortCommandFisReceiveEnableMask) != 0U)
    {
        state.CurrentCommand &= ~PortCommandReadOnlyEngineMask;
        if (!PollCommandMask(memoryMappedIo, state, budget, PortCommandFisReceiveRunningMask,
                             PortCommandFisReceiveRunningMask, AhciIdentifyCommandError::EngineRestoreFailed,
                             AhciIdentifyCommandPhase::PortRestoration, true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
    }

    const bool commandEngineWasRunning = (state.Original.Command & PortCommandStartMask) != 0U;
    if (commandEngineWasRunning && restartCommandEngine)
    {
        // Bases and the FIS engine now refer only to the original buffers.
        // Re-sample both device prerequisites immediately before setting ST.
        Foundation::UInt32 taskFileData{0U};
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                          AhciIdentifyRegister::PortTaskFileData, AhciIdentifyCommandPhase::PortRestoration,
                          taskFileData, true))
        {
            state.ConfigurationInstalled = false;
            state.Failure.DmaPageMustRemainLive = false;
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        if ((taskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
        {
            state.Failure.TaskFileData = taskFileData;
            SetRecoveryError(state, AhciIdentifyCommandError::PortBusy, AhciIdentifyRegister::PortTaskFileData);
            state.ConfigurationInstalled = false;
            state.Failure.DmaPageMustRemainLive = false;
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::CommandStopped;
            return false;
        }
        Foundation::UInt32 sataStatus{0U};
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortSataStatusOffset,
                          AhciIdentifyRegister::PortSataStatus, AhciIdentifyCommandPhase::PortRestoration, sataStatus,
                          true))
        {
            state.ConfigurationInstalled = false;
            state.Failure.DmaPageMustRemainLive = false;
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        if (!IsActiveSataLink(sataStatus))
        {
            SetRecoveryError(state, AhciIdentifyCommandError::NoActiveDiskPort, AhciIdentifyRegister::PortSataStatus);
            state.ConfigurationInstalled = false;
            state.Failure.DmaPageMustRemainLive = false;
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::CommandStopped;
            return false;
        }
        state.CurrentCommand &= ~PortCommandReadOnlyEngineMask;
        state.CurrentCommand |= PortCommandStartMask;
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciIdentifyRegister::PortCommand, AhciIdentifyCommandPhase::PortRestoration,
                           state.CurrentCommand, true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
    }
    else if (!commandEngineWasRunning && ((state.Original.Command & PortCommandFisReceiveEnableMask) == 0U))
    {
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciIdentifyRegister::PortCommand, AhciIdentifyCommandPhase::PortRestoration,
                           state.CurrentCommand, true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
    }
    if (commandEngineWasRunning && restartCommandEngine)
    {
        state.CurrentCommand &= ~PortCommandReadOnlyEngineMask;
        if (!PollCommandMask(memoryMappedIo, state, budget, PortCommandListRunningMask, PortCommandListRunningMask,
                             AhciIdentifyCommandError::EngineRestoreFailed, AhciIdentifyCommandPhase::PortRestoration,
                             true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
    }

    state.ConfigurationInstalled = false;
    state.Failure.DmaPageMustRemainLive = false;
    state.Failure.PortDisposition = commandEngineWasRunning && !restartCommandEngine
                                        ? AhciIdentifyPortDisposition::CommandStopped
                                        : AhciIdentifyPortDisposition::Restored;
    return true;
}

[[nodiscard]] bool RecoverPort(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                               Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter, ExecutionState& state,
                               PollBudget& budget, const AhciHbaRegisterRange range) noexcept
{
    if (!state.ConfigurationInstalled)
    {
        return true;
    }
    if (!StopEngine(memoryMappedIo, memoryMappedIoWriter, state, budget, true))
    {
        return false;
    }

    Foundation::UInt32 commandIssue{0U};
    Foundation::UInt32 sataActive{0U};
    if (state.CommandIssueAttempted)
    {
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortCommandIssueOffset,
                          AhciIdentifyRegister::PortCommandIssue, AhciIdentifyCommandPhase::PortRestoration,
                          commandIssue, true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        state.Failure.CommandIssue = commandIssue;
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortSataActiveOffset,
                          AhciIdentifyRegister::PortSataActive, AhciIdentifyCommandPhase::PortRestoration, sataActive,
                          true))
        {
            state.Failure.PortDisposition = AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        if ((commandIssue != 0U) || (sataActive != 0U))
        {
            SetRecoveryError(state, AhciIdentifyCommandError::PortBusy,
                             (commandIssue != 0U) ? AhciIdentifyRegister::PortCommandIssue
                                                  : AhciIdentifyRegister::PortSataActive);
            const bool acknowledged = AcknowledgeStatus(memoryMappedIo, memoryMappedIoWriter, state, range, true);
            state.Failure.PortDisposition =
                acknowledged ? AhciIdentifyPortDisposition::CommandStopped : AhciIdentifyPortDisposition::Uncertain;
            return false;
        }
        // CR/FR and all slot ownership are now clear, so the temporary page
        // can no longer be accessed even if the device is not restart-safe.
        state.Failure.DmaPageMustRemainLive = false;
        Foundation::UInt32 recoveryTaskFileData{0U};
        if (!state.CompletionStatusValidated)
        {
            if (!CaptureFinalCompletionStatus(memoryMappedIo, state, true))
            {
                const bool restoredWithoutRestart =
                    RestoreOriginalConfiguration(memoryMappedIo, memoryMappedIoWriter, state, budget, range, false);
                static_cast<void>(restoredWithoutRestart);
                return false;
            }
            recoveryTaskFileData = state.CompletionTaskFileData;
        }
        else if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                               AhciIdentifyRegister::PortTaskFileData, AhciIdentifyCommandPhase::PortRestoration,
                               recoveryTaskFileData, true))
        {
            const bool restoredWithoutRestart =
                RestoreOriginalConfiguration(memoryMappedIo, memoryMappedIoWriter, state, budget, range, false);
            static_cast<void>(restoredWithoutRestart);
            return false;
        }
        if ((recoveryTaskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
        {
            state.Failure.TaskFileData = recoveryTaskFileData;
            SetRecoveryError(state, AhciIdentifyCommandError::PortBusy, AhciIdentifyRegister::PortTaskFileData);
            // Restore safe original bases and FIS configuration, but leave ST
            // clear while the device still reports BSY or DRQ.
            const bool restoredWithoutRestart =
                RestoreOriginalConfiguration(memoryMappedIo, memoryMappedIoWriter, state, budget, range, false);
            static_cast<void>(restoredWithoutRestart);
            return false;
        }
    }

    // Both DMA engines are stopped and no slot-0 command can still access the
    // installed page. Subsequent restoration failures cannot revive that page.
    state.Failure.DmaPageMustRemainLive = false;
    return RestoreOriginalConfiguration(memoryMappedIo, memoryMappedIoWriter, state, budget, range, true);
}

[[nodiscard]] constexpr Foundation::UInt32 ReadLittleEndian32(const Foundation::UInt8* const data) noexcept
{
    Foundation::UInt32 value{0U};
    for (Foundation::UInt32 index{0U}; index < 4U; ++index)
    {
        value |= static_cast<Foundation::UInt32>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool ValidateDmaCompletion(const AhciIdentifyCommandRequest& request, ExecutionState& state) noexcept
{
    const Foundation::UInt8* const commandHeader =
        request.DmaPage.Data + AhciIdentifyDmaPageBuilder::CommandListOffsetBytes;
    const Foundation::UInt32 transferredBytes = ReadLittleEndian32(commandHeader + CommandHeaderByteCountOffsetBytes);
    if (transferredBytes != IdentifyTransferSizeBytes)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::IncompleteDataTransfer,
                        AhciIdentifyCommandPhase::CommandExecution);
        return false;
    }

    const Foundation::UInt8* const receivedFis =
        request.DmaPage.Data + AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes;
    const Foundation::UInt8* const deviceToHostFis = receivedFis + DeviceToHostFisOffsetBytes;
    const Foundation::UInt8* const pioSetupFis = receivedFis + PioSetupFisOffsetBytes;
    if (deviceToHostFis[0U] == DeviceToHostFisType)
    {
        state.DeviceToHostFisStatus = deviceToHostFis[2U];
        state.DeviceToHostFisError = deviceToHostFis[3U];
    }
    else if (pioSetupFis[0U] == PioSetupFisType)
    {
        state.DeviceToHostFisStatus = pioSetupFis[PioSetupEndStatusOffsetBytes];
        state.DeviceToHostFisError = pioSetupFis[3U];
    }
    else
    {
        SetPrimaryError(state, AhciIdentifyCommandError::InvalidCompletionFis,
                        AhciIdentifyCommandPhase::CommandExecution);
        return false;
    }
    if ((state.DeviceToHostFisStatus &
         static_cast<Foundation::UInt8>(TaskFileBusyMask | TaskFileDataRequestMask | TaskFileFailureMask)) != 0U)
    {
        state.Failure.TaskFileData = static_cast<Foundation::UInt32>(state.DeviceToHostFisStatus) |
                                     (static_cast<Foundation::UInt32>(state.DeviceToHostFisError) << 8U);
        SetPrimaryError(state, AhciIdentifyCommandError::TaskFileError, AhciIdentifyCommandPhase::CommandExecution,
                        AhciIdentifyRegister::PortTaskFileData);
        return false;
    }
    return true;
}

[[nodiscard]] AhciIdentifyCommandFailure FailureResult(ExecutionState& state, const PollBudget& budget) noexcept
{
    state.Failure.ObservedTicks = budget.GetLastObservedTicks();
    state.Failure.PollCount = budget.GetPollCount();
    state.Failure.PortCommand = state.CurrentCommand;
    return state.Failure;
}

[[nodiscard, gnu::noinline]] AhciIdentifyCommandFailure SuccessResult(const AhciIdentifyCommandRequest& request,
                                                                      ExecutionState& state, const PollBudget& budget,
                                                                      const Foundation::UInt8 selectedPort,
                                                                      AhciIdentifyCommandReceipt& output) noexcept
{
    const Foundation::UInt8* const identifyData =
        request.DmaPage.Data + AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes;
    AhciIdentifyCommandReceipt receipt{};
    const AhciIdentifyCommandError parseError =
        Detail::ParseAhciIdentifyDeviceInformation(identifyData, receipt.Device);
    if (parseError != AhciIdentifyCommandError::None)
    {
        SetPrimaryError(state, parseError, AhciIdentifyCommandPhase::CommandExecution);
        return FailureResult(state, budget);
    }
    receipt.StartTicks = budget.GetStartTicks();
    receipt.CompletionTicks = budget.GetLastObservedTicks();
    receipt.PollCount = budget.GetPollCount();
    receipt.Capabilities = state.Capabilities;
    receipt.PortsImplemented = state.PortsImplemented;
    receipt.PortSignature = AtaDeviceSignature;
    receipt.CompletionInterruptStatus = state.CompletionInterruptStatus;
    receipt.CompletionTaskFileData = state.CompletionTaskFileData;
    receipt.InitialPortCommand = state.Original.Command;
    receipt.RestoredPortCommand = state.CurrentCommand;
    receipt.PortNumber = selectedPort;
    receipt.PortDisposition = state.Failure.PortDisposition;
    receipt.DeviceToHostFisStatus = state.DeviceToHostFisStatus;
    receipt.DeviceToHostFisError = state.DeviceToHostFisError;
    output = receipt;
    return AhciIdentifyCommandFailure{};
}
} // namespace

AhciIdentifyCommandFailure AhciIdentifyCommandExecutor::Execute(const AhciIdentifyCommandRequest& request,
                                                                AhciIdentifyCommandReceipt& output) noexcept
{
    ExecutionState state{};
    state.Failure.PortNumber = InvalidPortNumber;
    state.Failure.PortDisposition = AhciIdentifyPortDisposition::Unchanged;
    PollBudget budget{m_clock, request.Polling};

    if (RangesOverlap(request.DmaPage.Data, AhciIdentifyDmaPageBuilder::PageSizeBytes, &request, sizeof(request)))
    {
        SetPrimaryError(state, AhciIdentifyCommandError::DmaPageOverlapsRequest, AhciIdentifyCommandPhase::Validation);
        return FailureResult(state, budget);
    }
    if (RangesOverlap(request.DmaPage.Data, AhciIdentifyDmaPageBuilder::PageSizeBytes, &output, sizeof(output)))
    {
        SetPrimaryError(state, AhciIdentifyCommandError::DmaPageOverlapsReceipt, AhciIdentifyCommandPhase::Validation);
        return FailureResult(state, budget);
    }

    const Foundation::Size maximumSize{~Foundation::Size{0U}};
    if (!IsValidRange(request.Abar))
    {
        SetPrimaryError(state, AhciIdentifyCommandError::InvalidAbarRange, AhciIdentifyCommandPhase::Validation);
        return FailureResult(state, budget);
    }
    if ((request.Polling.DeadlineTicks == 0U) || (request.Polling.DeadlineTicks == MaximumUInt64) ||
        (request.Polling.MaximumPollCount == 0U) || (request.Polling.MaximumRecoveryPollCount == 0U) ||
        (request.Polling.MaximumPollCount > (maximumSize - request.Polling.MaximumRecoveryPollCount)))
    {
        SetPrimaryError(state, AhciIdentifyCommandError::InvalidPollingPolicy, AhciIdentifyCommandPhase::Validation);
        return FailureResult(state, budget);
    }

    const PollDecision startDecision = budget.Start();
    if (startDecision != PollDecision::Continue)
    {
        const AhciIdentifyCommandError error = (startDecision == PollDecision::ClockExhausted)
                                                   ? AhciIdentifyCommandError::ClockExhausted
                                                   : AhciIdentifyCommandError::DeadlineReached;
        SetPrimaryError(state, error, AhciIdentifyCommandPhase::Validation);
        return FailureResult(state, budget);
    }

    Foundation::UInt32 globalHostControl{0U};
    if (!ReadRegister(m_memoryMappedIo, state, request.Abar.BaseAddress + CapabilitiesOffset,
                      AhciIdentifyRegister::Capabilities, AhciIdentifyCommandPhase::PortSelection,
                      state.Capabilities) ||
        !ReadRegister(m_memoryMappedIo, state, request.Abar.BaseAddress + GlobalHostControlOffset,
                      AhciIdentifyRegister::GlobalHostControl, AhciIdentifyCommandPhase::PortSelection,
                      globalHostControl) ||
        !ReadRegister(m_memoryMappedIo, state, request.Abar.BaseAddress + PortsImplementedOffset,
                      AhciIdentifyRegister::PortsImplemented, AhciIdentifyCommandPhase::PortSelection,
                      state.PortsImplemented))
    {
        return FailureResult(state, budget);
    }
    if ((globalHostControl & AhciEnableMask) == 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::AhciModeDisabled, AhciIdentifyCommandPhase::PortSelection,
                        AhciIdentifyRegister::GlobalHostControl);
        return FailureResult(state, budget);
    }

    const Foundation::UInt32 portCount = (state.Capabilities & PortCountMask) + 1U;
    const Foundation::UInt32 validPortMask =
        (portCount == 32U) ? 0xFFFF'FFFFU : (Foundation::UInt32{1U} << portCount) - 1U;
    if ((state.PortsImplemented & ~validPortMask) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::PortsImplementedOutOfRange,
                        AhciIdentifyCommandPhase::PortSelection, AhciIdentifyRegister::PortsImplemented);
        return FailureResult(state, budget);
    }

    for (Foundation::UInt32 port{0U}; port < portCount; ++port)
    {
        const Foundation::UInt32 portMask = Foundation::UInt32{1U} << port;
        if ((state.PortsImplemented & portMask) == 0U)
        {
            continue;
        }
        const Foundation::UInt8 portNumber = static_cast<Foundation::UInt8>(port);
        if (!ContainsRegister(request.Abar, PortRegisterOffset(portNumber, PortCommandIssueOffset)))
        {
            SetPrimaryError(state, AhciIdentifyCommandError::InvalidAbarRange, AhciIdentifyCommandPhase::PortSelection);
            return FailureResult(state, budget);
        }
    }

    Foundation::UInt8 selectedPort{InvalidPortNumber};
    for (Foundation::UInt32 port{0U}; port < portCount; ++port)
    {
        const Foundation::UInt32 portMask = Foundation::UInt32{1U} << port;
        if ((state.PortsImplemented & portMask) == 0U)
        {
            continue;
        }
        const Foundation::UInt8 portNumber = static_cast<Foundation::UInt8>(port);
        const Foundation::UInt64 portBaseAddress = request.Abar.BaseAddress + PortRegisterOffset(portNumber, 0U);
        Foundation::UInt32 sataStatus{0U};
        if (!ReadRegister(m_memoryMappedIo, state, portBaseAddress + PortSataStatusOffset,
                          AhciIdentifyRegister::PortSataStatus, AhciIdentifyCommandPhase::PortSelection, sataStatus))
        {
            return FailureResult(state, budget);
        }
        if (!IsActiveSataLink(sataStatus))
        {
            continue;
        }

        Foundation::UInt32 signature{0U};
        if (!ReadRegister(m_memoryMappedIo, state, portBaseAddress + PortSignatureOffset,
                          AhciIdentifyRegister::PortSignature, AhciIdentifyCommandPhase::PortSelection, signature))
        {
            return FailureResult(state, budget);
        }
        if (signature == AtaDeviceSignature)
        {
            ++state.Failure.MatchingDiskPortCount;
            selectedPort = portNumber;
        }
        else if (signature == AtapiDeviceSignature)
        {
            ++state.Failure.ActiveAtapiPortCount;
        }
        else
        {
            ++state.Failure.ActiveUnsupportedPortCount;
        }
    }

    if (state.Failure.ActiveUnsupportedPortCount != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::UnsupportedDeviceSignature,
                        AhciIdentifyCommandPhase::PortSelection, AhciIdentifyRegister::PortSignature);
        return FailureResult(state, budget);
    }
    if (state.Failure.ActiveAtapiPortCount != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::AtapiDeviceNotSupported,
                        AhciIdentifyCommandPhase::PortSelection, AhciIdentifyRegister::PortSignature);
        return FailureResult(state, budget);
    }
    if (state.Failure.MatchingDiskPortCount == 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::NoActiveDiskPort, AhciIdentifyCommandPhase::PortSelection,
                        AhciIdentifyRegister::PortSignature);
        return FailureResult(state, budget);
    }
    if (state.Failure.MatchingDiskPortCount != 1U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::AmbiguousActiveDiskPorts,
                        AhciIdentifyCommandPhase::PortSelection, AhciIdentifyRegister::PortSignature);
        return FailureResult(state, budget);
    }

    state.Failure.PortNumber = selectedPort;
    state.PortBaseAddress = request.Abar.BaseAddress + PortRegisterOffset(selectedPort, 0U);
    Foundation::UInt32 taskFileData{0U};
    Foundation::UInt32 sataActive{0U};
    Foundation::UInt32 commandIssue{0U};
    if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandOffset,
                      AhciIdentifyRegister::PortCommand, AhciIdentifyCommandPhase::PortPreparation,
                      state.Original.Command) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                      AhciIdentifyRegister::PortTaskFileData, AhciIdentifyCommandPhase::PortPreparation,
                      taskFileData) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortSataActiveOffset,
                      AhciIdentifyRegister::PortSataActive, AhciIdentifyCommandPhase::PortPreparation, sataActive) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandIssueOffset,
                      AhciIdentifyRegister::PortCommandIssue, AhciIdentifyCommandPhase::PortPreparation,
                      commandIssue) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandListBaseOffset,
                      AhciIdentifyRegister::PortCommandListBase, AhciIdentifyCommandPhase::PortPreparation,
                      state.Original.CommandListBase) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandListBaseUpperOffset,
                      AhciIdentifyRegister::PortCommandListBaseUpper, AhciIdentifyCommandPhase::PortPreparation,
                      state.Original.CommandListBaseUpper) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortFisBaseOffset,
                      AhciIdentifyRegister::PortFisBase, AhciIdentifyCommandPhase::PortPreparation,
                      state.Original.FisBase) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortFisBaseUpperOffset,
                      AhciIdentifyRegister::PortFisBaseUpper, AhciIdentifyCommandPhase::PortPreparation,
                      state.Original.FisBaseUpper) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortInterruptEnableOffset,
                      AhciIdentifyRegister::PortInterruptEnable, AhciIdentifyCommandPhase::PortPreparation,
                      state.Original.InterruptEnable))
    {
        return FailureResult(state, budget);
    }
    state.CurrentCommand = state.Original.Command;
    state.Failure.TaskFileData = taskFileData;
    state.Failure.CommandIssue = commandIssue;
    if (((state.Original.Command & PortCommandStartMask) != 0U) &&
        ((state.Original.Command & PortCommandFisReceiveEnableMask) == 0U))
    {
        SetPrimaryError(state, AhciIdentifyCommandError::PortBusy, AhciIdentifyCommandPhase::PortPreparation,
                        AhciIdentifyRegister::PortCommand);
        return FailureResult(state, budget);
    }
    if (((taskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U) || (sataActive != 0U) ||
        (commandIssue != 0U))
    {
        SetPrimaryError(state, AhciIdentifyCommandError::PortBusy, AhciIdentifyCommandPhase::PortPreparation,
                        AhciIdentifyRegister::PortTaskFileData);
        return FailureResult(state, budget);
    }
    if ((taskFileData & TaskFileFailureMask) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::TaskFileError, AhciIdentifyCommandPhase::PortPreparation,
                        AhciIdentifyRegister::PortTaskFileData);
        return FailureResult(state, budget);
    }

    const PollDecision mutationDeadline = budget.Checkpoint();
    if (mutationDeadline != PollDecision::Continue)
    {
        RecordDeadlineFailure(state, mutationDeadline, AhciIdentifyCommandPhase::PortPreparation);
        return FailureResult(state, budget);
    }

    const bool supports64BitAddressing = (state.Capabilities & Supports64BitAddressingMask) != 0U;
    const AhciIdentifyDmaPageBuildResult dmaBuild =
        AhciIdentifyDmaPageBuilder::Build(request.DmaPage, supports64BitAddressing);
    if (!dmaBuild.HasValue())
    {
        state.Failure.DmaPage = dmaBuild.GetError();
        SetPrimaryError(state, AhciIdentifyCommandError::DmaPageRejected, AhciIdentifyCommandPhase::PortPreparation);
        return FailureResult(state, budget);
    }
    if (!StopEngine(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, false))
    {
        return FailureResult(state, budget);
    }
    if (!InstallCommandConfiguration(m_memoryMappedIo, m_memoryMappedIoWriter, state, request.Abar,
                                     request.DmaPage.PhysicalAddress) ||
        !StartCommandEngine(m_memoryMappedIoWriter, state))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                      AhciIdentifyRegister::PortTaskFileData, AhciIdentifyCommandPhase::CommandExecution, taskFileData))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }
    state.Failure.TaskFileData = taskFileData;
    if ((taskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::PortBusy, AhciIdentifyCommandPhase::CommandExecution,
                        AhciIdentifyRegister::PortTaskFileData);
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }
    if ((taskFileData & TaskFileFailureMask) != 0U)
    {
        SetPrimaryError(state, AhciIdentifyCommandError::TaskFileError, AhciIdentifyCommandPhase::CommandExecution,
                        AhciIdentifyRegister::PortTaskFileData);
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    const PollDecision issueDeadline = budget.Checkpoint();
    if (issueDeadline != PollDecision::Continue)
    {
        RecordDeadlineFailure(state, issueDeadline, AhciIdentifyCommandPhase::CommandExecution,
                              AhciIdentifyRegister::PortCommandIssue);
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    // A backend error cannot prove that an MMIO write had no device effect.
    state.CommandIssueAttempted = true;
    state.Failure.CommandIssue = 1U;
    if (!WriteRegister(m_memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandIssueOffset,
                       AhciIdentifyRegister::PortCommandIssue, AhciIdentifyCommandPhase::CommandExecution, 1U))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    while (!state.CommandCompleted)
    {
        const PollDecision decision = budget.BeginPoll();
        if (decision != PollDecision::Continue)
        {
            RecordPollFailure(state, decision, AhciIdentifyCommandError::CommandTimedOut,
                              AhciIdentifyCommandPhase::CommandExecution, AhciIdentifyRegister::PortCommandIssue,
                              false);
            break;
        }

        if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortInterruptStatusOffset,
                          AhciIdentifyRegister::PortInterruptStatus, AhciIdentifyCommandPhase::CommandExecution,
                          state.CompletionInterruptStatus) ||
            !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                          AhciIdentifyRegister::PortTaskFileData, AhciIdentifyCommandPhase::CommandExecution,
                          state.CompletionTaskFileData) ||
            !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandIssueOffset,
                          AhciIdentifyRegister::PortCommandIssue, AhciIdentifyCommandPhase::CommandExecution,
                          commandIssue))
        {
            break;
        }
        state.Failure.PortInterruptStatus = state.CompletionInterruptStatus;
        state.Failure.TaskFileData = state.CompletionTaskFileData;
        state.Failure.CommandIssue = commandIssue;
        if ((state.CompletionInterruptStatus & PortInterruptFatalMask) != 0U)
        {
            ValidateCompletionStatus(state);
            break;
        }
        if ((state.CompletionTaskFileData & TaskFileFailureMask) != 0U)
        {
            ValidateCompletionStatus(state);
            break;
        }
        if ((commandIssue & 1U) == 0U)
        {
            state.CommandCompleted = true;
            if (!CaptureFinalCompletionStatus(m_memoryMappedIo, state, false))
            {
                break;
            }
        }
    }

    if (!state.CommandCompleted || (state.Failure.Error != AhciIdentifyCommandError::None))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    if (!ValidateDmaCompletion(request, state))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    if (!RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar))
    {
        return FailureResult(state, budget);
    }

    return SuccessResult(request, state, budget, selectedPort, output);
}
} // namespace Asteria::Kernel::Device
