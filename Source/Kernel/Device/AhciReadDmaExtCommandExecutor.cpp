// SPDX-License-Identifier: Apache-2.0

#include "Kernel/Device/AhciReadDmaExtCommandExecutor.hpp"

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
constexpr Foundation::Size CommandHeaderByteCountOffsetBytes{4U};
constexpr Foundation::Size DeviceToHostFisOffsetBytes{0x40U};
constexpr Foundation::UInt8 DeviceToHostFisType{0x34U};
// Some HBAs (VMware's, and real ones) clear PxCI before PxTFD reflects the
// completion's D2H FIS, so BSY or DRQ can still read set just after CI
// clears. The final task-file read is repeated a bounded number of times
// until both clear or an error shows.
constexpr Foundation::Size CompletionTaskFileSettleReads{4096U};

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
    AhciReadDmaExtExecutionFailure Failure;
    OriginalPortState Original;
    Foundation::UInt64 PortBaseAddress;
    Foundation::UInt32 Capabilities;
    Foundation::UInt32 PortsImplemented;
    Foundation::UInt32 CurrentCommand;
    Foundation::UInt32 CompletionInterruptStatus;
    Foundation::UInt32 CompletionTaskFileData;
    Foundation::UInt64 TransferSizeBytes;
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
    constexpr PollBudget(const Hal::Time::IMonotonicClock& clock, const AhciReadDmaExtPollingPolicy& policy) noexcept
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

void SetPrimaryError(ExecutionState& state, const AhciReadDmaExtExecutionError error,
                     const AhciReadDmaExtExecutionPhase phase,
                     const AhciReadDmaExtRegister reg = AhciReadDmaExtRegister::None) noexcept
{
    if (state.Failure.Error == AhciReadDmaExtExecutionError::None)
    {
        state.Failure.Error = error;
        state.Failure.Phase = phase;
        state.Failure.Register = reg;
    }
}

void SetRecoveryError(ExecutionState& state, const AhciReadDmaExtExecutionError error,
                      const AhciReadDmaExtRegister reg = AhciReadDmaExtRegister::None) noexcept
{
    if (state.Failure.Error == AhciReadDmaExtExecutionError::None)
    {
        state.Failure.Error = AhciReadDmaExtExecutionError::EngineRestoreFailed;
        state.Failure.Phase = AhciReadDmaExtExecutionPhase::PortRestoration;
        state.Failure.Register = reg;
    }
    if (state.Failure.RecoveryError == AhciReadDmaExtExecutionError::None)
    {
        state.Failure.RecoveryError = error;
        state.Failure.RecoveryRegister = reg;
    }
}

[[nodiscard]] bool ReadRegister(Hal::Device::IMemoryMappedIo& memoryMappedIo, ExecutionState& state,
                                const Foundation::UInt64 physicalAddress, const AhciReadDmaExtRegister reg,
                                const AhciReadDmaExtExecutionPhase phase, Foundation::UInt32& output,
                                const bool recovery = false) noexcept
{
    Foundation::UInt64 value{0U};
    const Hal::Device::MemoryMappedIoReadError readError =
        memoryMappedIo.Read(physicalAddress, Hal::Device::MemoryMappedIoWidth::DoubleWord, value);
    if (readError != Hal::Device::MemoryMappedIoReadError::None)
    {
        if (recovery)
        {
            SetRecoveryError(state, AhciReadDmaExtExecutionError::MemoryMappedIoReadFailed, reg);
            state.Failure.RecoveryMemoryMappedIoRead = readError;
        }
        else
        {
            SetPrimaryError(state, AhciReadDmaExtExecutionError::MemoryMappedIoReadFailed, phase, reg);
            state.Failure.MemoryMappedIoRead = readError;
        }
        return false;
    }
    if (value > MaximumUInt32)
    {
        if (recovery)
        {
            SetRecoveryError(state, AhciReadDmaExtExecutionError::InvalidRegisterValue, reg);
        }
        else
        {
            SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidRegisterValue, phase, reg);
        }
        return false;
    }
    output = static_cast<Foundation::UInt32>(value);
    return true;
}

[[nodiscard]] bool WriteRegister(Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter, ExecutionState& state,
                                 const Foundation::UInt64 physicalAddress, const AhciReadDmaExtRegister reg,
                                 const AhciReadDmaExtExecutionPhase phase, const Foundation::UInt32 value,
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
        SetRecoveryError(state, AhciReadDmaExtExecutionError::MemoryMappedIoWriteFailed, reg);
        state.Failure.RecoveryMemoryMappedIoWrite = writeError;
    }
    else
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::MemoryMappedIoWriteFailed, phase, reg);
        state.Failure.MemoryMappedIoWrite = writeError;
    }
    return false;
}

void RecordPollFailure(ExecutionState& state, const PollDecision decision,
                       const AhciReadDmaExtExecutionError timeoutError, const AhciReadDmaExtExecutionPhase phase,
                       const AhciReadDmaExtRegister reg, const bool recovery) noexcept
{
    AhciReadDmaExtExecutionError error{timeoutError};
    switch (decision)
    {
    case PollDecision::Continue:
        return;
    case PollDecision::LimitReached:
    case PollDecision::DeadlineReached:
        error = timeoutError;
        break;
    case PollDecision::ClockExhausted:
        error = AhciReadDmaExtExecutionError::ClockExhausted;
        break;
    case PollDecision::ClockReversed:
        error = AhciReadDmaExtExecutionError::ClockReversed;
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

void RecordDeadlineFailure(ExecutionState& state, const PollDecision decision, const AhciReadDmaExtExecutionPhase phase,
                           const AhciReadDmaExtRegister reg = AhciReadDmaExtRegister::None) noexcept
{
    AhciReadDmaExtExecutionError error{AhciReadDmaExtExecutionError::DeadlineReached};
    if (decision == PollDecision::ClockExhausted)
    {
        error = AhciReadDmaExtExecutionError::ClockExhausted;
    }
    else if (decision == PollDecision::ClockReversed)
    {
        error = AhciReadDmaExtExecutionError::ClockReversed;
    }
    SetPrimaryError(state, error, phase, reg);
}

[[nodiscard]] bool PollCommandMask(Hal::Device::IMemoryMappedIo& memoryMappedIo, ExecutionState& state,
                                   PollBudget& budget, const Foundation::UInt32 mask, const Foundation::UInt32 expected,
                                   const AhciReadDmaExtExecutionError timeoutError,
                                   const AhciReadDmaExtExecutionPhase phase, const bool recovery) noexcept
{
    while ((state.CurrentCommand & mask) != expected)
    {
        const PollDecision decision = recovery ? budget.BeginRecoveryPoll() : budget.BeginPoll();
        if (decision != PollDecision::Continue)
        {
            RecordPollFailure(state, decision, timeoutError, phase, AhciReadDmaExtRegister::PortCommand, recovery);
            return false;
        }
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortCommandOffset,
                          AhciReadDmaExtRegister::PortCommand, phase, state.CurrentCommand, recovery))
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
    const AhciReadDmaExtExecutionPhase phase =
        recovery ? AhciReadDmaExtExecutionPhase::PortRestoration : AhciReadDmaExtExecutionPhase::PortPreparation;
    const AhciReadDmaExtExecutionError timeoutError =
        recovery ? AhciReadDmaExtExecutionError::EngineRestoreFailed : AhciReadDmaExtExecutionError::EngineStopTimedOut;
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortCommandOffset,
                      AhciReadDmaExtRegister::PortCommand, phase, state.CurrentCommand, recovery))
    {
        state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
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
                           AhciReadDmaExtRegister::PortCommand, phase, stoppedCommand, recovery))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::StopRequested;
        state.CurrentCommand = stoppedCommand | (state.CurrentCommand & PortCommandReadOnlyEngineMask);
    }
    if (!PollCommandMask(memoryMappedIo, state, budget, PortCommandListRunningMask, 0U, timeoutError, phase, recovery))
    {
        return false;
    }

    if ((state.CurrentCommand & PortCommandFisReceiveEnableMask) != 0U)
    {
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciReadDmaExtRegister::PortCommand, phase, configuration, recovery))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::StopRequested;
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
    const AhciReadDmaExtExecutionPhase phase =
        recovery ? AhciReadDmaExtExecutionPhase::PortRestoration : AhciReadDmaExtExecutionPhase::PortPreparation;
    Foundation::UInt32 interruptStatus{0U};
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortInterruptStatusOffset,
                      AhciReadDmaExtRegister::PortInterruptStatus, phase, interruptStatus, recovery) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortInterruptStatusOffset,
                       AhciReadDmaExtRegister::PortInterruptStatus, phase, interruptStatus, recovery))
    {
        return false;
    }

    Foundation::UInt32 sataError{0U};
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortSataErrorOffset,
                      AhciReadDmaExtRegister::PortSataError, phase, sataError, recovery) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortSataErrorOffset,
                       AhciReadDmaExtRegister::PortSataError, phase, sataError, recovery))
    {
        return false;
    }

    Foundation::UInt32 globalInterruptStatus{0U};
    const Foundation::UInt32 selectedPortMask = Foundation::UInt32{1U} << state.Failure.PortNumber;
    return ReadRegister(memoryMappedIo, state, range.BaseAddress + GlobalInterruptStatusOffset,
                        AhciReadDmaExtRegister::GlobalInterruptStatus, phase, globalInterruptStatus, recovery) &&
           WriteRegister(memoryMappedIoWriter, state, range.BaseAddress + GlobalInterruptStatusOffset,
                         AhciReadDmaExtRegister::GlobalInterruptStatus, phase, globalInterruptStatus & selectedPortMask,
                         recovery);
}

[[nodiscard]] bool InstallCommandConfiguration(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                               Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter,
                                               ExecutionState& state, const AhciHbaRegisterRange range,
                                               const AhciReadDmaExtCommandLayout& commandLayout) noexcept
{
    const AhciReadDmaExtExecutionPhase phase = AhciReadDmaExtExecutionPhase::PortPreparation;
    const Foundation::UInt32 commandListLow =
        static_cast<Foundation::UInt32>(commandLayout.CommandListPhysicalAddress & MaximumUInt32);
    const Foundation::UInt32 commandListHigh =
        static_cast<Foundation::UInt32>(commandLayout.CommandListPhysicalAddress >> 32U);
    const Foundation::UInt32 fisLow =
        static_cast<Foundation::UInt32>(commandLayout.ReceivedFisPhysicalAddress & MaximumUInt32);
    const Foundation::UInt32 fisHigh = static_cast<Foundation::UInt32>(commandLayout.ReceivedFisPhysicalAddress >> 32U);

    // A failed backend write need not prove that no device-side change was
    // observed, so conservative recovery begins with the first attempt.
    state.ConfigurationInstalled = true;
    if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortInterruptEnableOffset,
                       AhciReadDmaExtRegister::PortInterruptEnable, phase, 0U))
    {
        return false;
    }

    // From the first CLB write attempt onward, an error can leave a DMA page
    // address installed even when the backend reports that the write failed.
    state.Failure.CommandPageMustRemainLive = true;
    if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseOffset,
                       AhciReadDmaExtRegister::PortCommandListBase, phase, commandListLow) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseUpperOffset,
                       AhciReadDmaExtRegister::PortCommandListBaseUpper, phase, commandListHigh) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseOffset,
                       AhciReadDmaExtRegister::PortFisBase, phase, fisLow) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseUpperOffset,
                       AhciReadDmaExtRegister::PortFisBaseUpper, phase, fisHigh))
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
                       AhciReadDmaExtRegister::PortCommand, AhciReadDmaExtExecutionPhase::PortPreparation,
                       fisRunning) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                       AhciReadDmaExtRegister::PortCommand, AhciReadDmaExtExecutionPhase::PortPreparation,
                       commandRunning))
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
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortInterruptError,
                        AhciReadDmaExtExecutionPhase::CommandExecution, AhciReadDmaExtRegister::PortInterruptStatus);
    }
    if ((state.CompletionTaskFileData & TaskFileFailureMask) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::TaskFileError,
                        AhciReadDmaExtExecutionPhase::CommandExecution, AhciReadDmaExtRegister::PortTaskFileData);
    }
    else if ((state.CompletionTaskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtExecutionPhase::CommandExecution,
                        AhciReadDmaExtRegister::PortTaskFileData);
    }
}

[[nodiscard]] bool CaptureFinalCompletionStatus(Hal::Device::IMemoryMappedIo& memoryMappedIo, ExecutionState& state,
                                                const bool recovery) noexcept
{
    const AhciReadDmaExtExecutionPhase phase =
        recovery ? AhciReadDmaExtExecutionPhase::PortRestoration : AhciReadDmaExtExecutionPhase::CommandExecution;
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortInterruptStatusOffset,
                      AhciReadDmaExtRegister::PortInterruptStatus, phase, state.CompletionInterruptStatus, recovery))
    {
        return false;
    }
    state.Failure.PortInterruptStatus = state.CompletionInterruptStatus;
    if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                      AhciReadDmaExtRegister::PortTaskFileData, phase, state.CompletionTaskFileData, recovery))
    {
        return false;
    }
    for (Foundation::Size settle{0U};
         !recovery && (settle < CompletionTaskFileSettleReads) &&
         ((state.CompletionTaskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U) &&
         ((state.CompletionTaskFileData & TaskFileFailureMask) == 0U);
         ++settle)
    {
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                          AhciReadDmaExtRegister::PortTaskFileData, phase, state.CompletionTaskFileData, recovery))
        {
            return false;
        }
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
                       AhciReadDmaExtRegister::PortCommandListBase, AhciReadDmaExtExecutionPhase::PortRestoration,
                       state.Original.CommandListBase, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandListBaseUpperOffset,
                       AhciReadDmaExtRegister::PortCommandListBaseUpper, AhciReadDmaExtExecutionPhase::PortRestoration,
                       state.Original.CommandListBaseUpper, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseOffset,
                       AhciReadDmaExtRegister::PortFisBase, AhciReadDmaExtExecutionPhase::PortRestoration,
                       state.Original.FisBase, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortFisBaseUpperOffset,
                       AhciReadDmaExtRegister::PortFisBaseUpper, AhciReadDmaExtExecutionPhase::PortRestoration,
                       state.Original.FisBaseUpper, true) ||
        !WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortInterruptEnableOffset,
                       AhciReadDmaExtRegister::PortInterruptEnable, AhciReadDmaExtExecutionPhase::PortRestoration,
                       state.Original.InterruptEnable, true))
    {
        state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
        return false;
    }

    const Foundation::UInt32 configuration =
        state.Original.Command & ~(PortCommandStartMask | PortCommandActionMask | PortCommandFisReceiveEnableMask |
                                   PortCommandReadOnlyEngineMask);
    if ((state.Original.Command & PortCommandFisReceiveEnableMask) != 0U)
    {
        state.CurrentCommand = configuration | PortCommandFisReceiveEnableMask;
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciReadDmaExtRegister::PortCommand, AhciReadDmaExtExecutionPhase::PortRestoration,
                           state.CurrentCommand, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
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
                             PortCommandFisReceiveRunningMask, AhciReadDmaExtExecutionError::EngineRestoreFailed,
                             AhciReadDmaExtExecutionPhase::PortRestoration, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
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
                          AhciReadDmaExtRegister::PortTaskFileData, AhciReadDmaExtExecutionPhase::PortRestoration,
                          taskFileData, true))
        {
            state.ConfigurationInstalled = false;
            state.Failure.CommandPageMustRemainLive = false;
            state.Failure.DataBufferMustRemainLive = false;
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        if ((taskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
        {
            state.Failure.TaskFileData = taskFileData;
            SetRecoveryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtRegister::PortTaskFileData);
            state.ConfigurationInstalled = false;
            state.Failure.CommandPageMustRemainLive = false;
            state.Failure.DataBufferMustRemainLive = false;
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::CommandStopped;
            return false;
        }
        Foundation::UInt32 sataStatus{0U};
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortSataStatusOffset,
                          AhciReadDmaExtRegister::PortSataStatus, AhciReadDmaExtExecutionPhase::PortRestoration,
                          sataStatus, true))
        {
            state.ConfigurationInstalled = false;
            state.Failure.CommandPageMustRemainLive = false;
            state.Failure.DataBufferMustRemainLive = false;
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        if (!IsActiveSataLink(sataStatus))
        {
            SetRecoveryError(state, AhciReadDmaExtExecutionError::InactiveSataLink,
                             AhciReadDmaExtRegister::PortSataStatus);
            state.ConfigurationInstalled = false;
            state.Failure.CommandPageMustRemainLive = false;
            state.Failure.DataBufferMustRemainLive = false;
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::CommandStopped;
            return false;
        }
        state.CurrentCommand &= ~PortCommandReadOnlyEngineMask;
        state.CurrentCommand |= PortCommandStartMask;
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciReadDmaExtRegister::PortCommand, AhciReadDmaExtExecutionPhase::PortRestoration,
                           state.CurrentCommand, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
    }
    else if (!commandEngineWasRunning && ((state.Original.Command & PortCommandFisReceiveEnableMask) == 0U))
    {
        if (!WriteRegister(memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandOffset,
                           AhciReadDmaExtRegister::PortCommand, AhciReadDmaExtExecutionPhase::PortRestoration,
                           state.CurrentCommand, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
    }
    if (commandEngineWasRunning && restartCommandEngine)
    {
        state.CurrentCommand &= ~PortCommandReadOnlyEngineMask;
        if (!PollCommandMask(memoryMappedIo, state, budget, PortCommandListRunningMask, PortCommandListRunningMask,
                             AhciReadDmaExtExecutionError::EngineRestoreFailed,
                             AhciReadDmaExtExecutionPhase::PortRestoration, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
    }

    state.ConfigurationInstalled = false;
    state.Failure.CommandPageMustRemainLive = false;
    state.Failure.DataBufferMustRemainLive = false;
    state.Failure.PortDisposition = commandEngineWasRunning && !restartCommandEngine
                                        ? AhciReadDmaExtPortDisposition::CommandStopped
                                        : AhciReadDmaExtPortDisposition::Restored;
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
                          AhciReadDmaExtRegister::PortCommandIssue, AhciReadDmaExtExecutionPhase::PortRestoration,
                          commandIssue, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        state.Failure.CommandIssue = commandIssue;
        if (!ReadRegister(memoryMappedIo, state, state.PortBaseAddress + PortSataActiveOffset,
                          AhciReadDmaExtRegister::PortSataActive, AhciReadDmaExtExecutionPhase::PortRestoration,
                          sataActive, true))
        {
            state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        if ((commandIssue != 0U) || (sataActive != 0U))
        {
            SetRecoveryError(state, AhciReadDmaExtExecutionError::PortBusy,
                             (commandIssue != 0U) ? AhciReadDmaExtRegister::PortCommandIssue
                                                  : AhciReadDmaExtRegister::PortSataActive);
            const bool acknowledged = AcknowledgeStatus(memoryMappedIo, memoryMappedIoWriter, state, range, true);
            state.Failure.PortDisposition =
                acknowledged ? AhciReadDmaExtPortDisposition::CommandStopped : AhciReadDmaExtPortDisposition::Uncertain;
            return false;
        }
        // CR/FR and all slot ownership are now clear, so the temporary spans
        // can no longer be accessed even if the device is not restart-safe.
        state.Failure.CommandPageMustRemainLive = false;
        state.Failure.DataBufferMustRemainLive = false;
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
                               AhciReadDmaExtRegister::PortTaskFileData, AhciReadDmaExtExecutionPhase::PortRestoration,
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
            SetRecoveryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtRegister::PortTaskFileData);
            // Restore safe original bases and FIS configuration, but leave ST
            // clear while the device still reports BSY or DRQ.
            const bool restoredWithoutRestart =
                RestoreOriginalConfiguration(memoryMappedIo, memoryMappedIoWriter, state, budget, range, false);
            static_cast<void>(restoredWithoutRestart);
            return false;
        }
    }

    // Both DMA engines are stopped and no slot-0 command can still access the
    // installed spans. Subsequent restoration failures cannot revive them.
    state.Failure.CommandPageMustRemainLive = false;
    state.Failure.DataBufferMustRemainLive = false;
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

[[nodiscard]] bool ValidateDmaCompletion(const AhciReadDmaExtExecutionRequest& request, ExecutionState& state) noexcept
{
    const Foundation::UInt8* const commandHeader =
        request.CommandPage.Data + AhciReadDmaExtCommandBuilder::CommandListOffsetBytes;
    const Foundation::UInt32 transferredBytes = ReadLittleEndian32(commandHeader + CommandHeaderByteCountOffsetBytes);
    if (static_cast<Foundation::UInt64>(transferredBytes) != state.TransferSizeBytes)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::IncompleteDataTransfer,
                        AhciReadDmaExtExecutionPhase::CommandExecution);
        return false;
    }

    const Foundation::UInt8* const deviceToHostFis =
        request.CommandPage.Data + AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes + DeviceToHostFisOffsetBytes;
    if (deviceToHostFis[0U] != DeviceToHostFisType)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidCompletionFis,
                        AhciReadDmaExtExecutionPhase::CommandExecution);
        return false;
    }
    state.DeviceToHostFisStatus = deviceToHostFis[2U];
    state.DeviceToHostFisError = deviceToHostFis[3U];
    if ((state.DeviceToHostFisStatus &
         static_cast<Foundation::UInt8>(TaskFileBusyMask | TaskFileDataRequestMask | TaskFileFailureMask)) != 0U ||
        (state.DeviceToHostFisError != 0U))
    {
        state.Failure.TaskFileData = static_cast<Foundation::UInt32>(state.DeviceToHostFisStatus) |
                                     (static_cast<Foundation::UInt32>(state.DeviceToHostFisError) << 8U);
        SetPrimaryError(state, AhciReadDmaExtExecutionError::TaskFileError,
                        AhciReadDmaExtExecutionPhase::CommandExecution, AhciReadDmaExtRegister::PortTaskFileData);
        return false;
    }
    return true;
}

[[nodiscard]] AhciReadDmaExtExecutionFailure FailureResult(ExecutionState& state, const PollBudget& budget) noexcept
{
    state.Failure.ObservedTicks = budget.GetLastObservedTicks();
    state.Failure.PollCount = budget.GetPollCount();
    state.Failure.PortCommand = state.CurrentCommand;
    return state.Failure;
}

[[nodiscard, gnu::noinline]] AhciReadDmaExtExecutionFailure
SuccessResult(const AhciReadDmaExtExecutionRequest& request, ExecutionState& state, const PollBudget& budget,
              AhciReadDmaExtExecutionReceipt& output) noexcept
{
    AhciReadDmaExtExecutionReceipt receipt{};
    receipt.StartTicks = budget.GetStartTicks();
    receipt.CompletionTicks = budget.GetLastObservedTicks();
    receipt.PollCount = budget.GetPollCount();
    receipt.DeviceSectorCount = request.DeviceSectorCount;
    receipt.StartingLba = request.StartingLba;
    receipt.LogicalSectorSizeBytes = request.LogicalSectorSizeBytes;
    receipt.SectorCount = request.SectorCount;
    receipt.TransferSizeBytes = state.TransferSizeBytes;
    receipt.Capabilities = state.Capabilities;
    receipt.PortsImplemented = state.PortsImplemented;
    receipt.PortSignature = AtaDeviceSignature;
    receipt.CompletionInterruptStatus = state.CompletionInterruptStatus;
    receipt.CompletionTaskFileData = state.CompletionTaskFileData;
    receipt.InitialPortCommand = state.Original.Command;
    receipt.RestoredPortCommand = state.CurrentCommand;
    receipt.PortNumber = request.PortNumber;
    receipt.PortDisposition = state.Failure.PortDisposition;
    receipt.DeviceToHostFisStatus = state.DeviceToHostFisStatus;
    receipt.DeviceToHostFisError = state.DeviceToHostFisError;
    output = receipt;
    return AhciReadDmaExtExecutionFailure{};
}

[[nodiscard, gnu::noinline]] AhciReadDmaExtCommandBuildResult
BuildCommand(const AhciReadDmaExtExecutionRequest& request, const bool supports64BitAddressing) noexcept
{
    const AhciReadDmaExtCommandRequest commandRequest{
        .CommandPage = request.CommandPage,
        .DataBuffer = request.DataBuffer,
        .DeviceSectorCount = request.DeviceSectorCount,
        .StartingLba = request.StartingLba,
        .LogicalSectorSizeBytes = request.LogicalSectorSizeBytes,
        .SectorCount = request.SectorCount,
        .Supports64BitAddressing = supports64BitAddressing,
        .Supports48BitAddressing = request.Supports48BitAddressing,
        .Reserved = {},
    };
    return AhciReadDmaExtCommandBuilder::Build(commandRequest);
}
} // namespace

AhciReadDmaExtExecutionFailure AhciReadDmaExtCommandExecutor::Execute(const AhciReadDmaExtExecutionRequest& request,
                                                                      AhciReadDmaExtExecutionReceipt& output) noexcept
{
    ExecutionState state{};
    state.Failure.PortNumber = request.PortNumber;
    state.Failure.PortDisposition = AhciReadDmaExtPortDisposition::Unchanged;
    PollBudget budget{m_clock, request.Polling};

    if (RangesOverlap(request.CommandPage.Data, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes, &request,
                      sizeof(request)))
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::CommandPageOverlapsRequest,
                        AhciReadDmaExtExecutionPhase::Validation);
        return FailureResult(state, budget);
    }
    if (RangesOverlap(request.CommandPage.Data, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes, &output,
                      sizeof(output)))
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::CommandPageOverlapsReceipt,
                        AhciReadDmaExtExecutionPhase::Validation);
        return FailureResult(state, budget);
    }

    const Foundation::Size maximumSize{~Foundation::Size{0U}};
    if (!IsValidRange(request.Abar))
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidAbarRange,
                        AhciReadDmaExtExecutionPhase::Validation);
        return FailureResult(state, budget);
    }
    if (request.PortNumber >= 32U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidPortNumber,
                        AhciReadDmaExtExecutionPhase::Validation);
        return FailureResult(state, budget);
    }
    if ((request.Polling.DeadlineTicks == 0U) || (request.Polling.DeadlineTicks == MaximumUInt64) ||
        (request.Polling.MaximumPollCount == 0U) || (request.Polling.MaximumRecoveryPollCount == 0U) ||
        (request.Polling.MaximumPollCount > (maximumSize - request.Polling.MaximumRecoveryPollCount)))
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidPollingPolicy,
                        AhciReadDmaExtExecutionPhase::Validation);
        return FailureResult(state, budget);
    }

    const PollDecision startDecision = budget.Start();
    if (startDecision != PollDecision::Continue)
    {
        const AhciReadDmaExtExecutionError error = (startDecision == PollDecision::ClockExhausted)
                                                       ? AhciReadDmaExtExecutionError::ClockExhausted
                                                       : AhciReadDmaExtExecutionError::DeadlineReached;
        SetPrimaryError(state, error, AhciReadDmaExtExecutionPhase::Validation);
        return FailureResult(state, budget);
    }

    Foundation::UInt32 globalHostControl{0U};
    if (!ReadRegister(m_memoryMappedIo, state, request.Abar.BaseAddress + CapabilitiesOffset,
                      AhciReadDmaExtRegister::Capabilities, AhciReadDmaExtExecutionPhase::PortSelection,
                      state.Capabilities) ||
        !ReadRegister(m_memoryMappedIo, state, request.Abar.BaseAddress + GlobalHostControlOffset,
                      AhciReadDmaExtRegister::GlobalHostControl, AhciReadDmaExtExecutionPhase::PortSelection,
                      globalHostControl) ||
        !ReadRegister(m_memoryMappedIo, state, request.Abar.BaseAddress + PortsImplementedOffset,
                      AhciReadDmaExtRegister::PortsImplemented, AhciReadDmaExtExecutionPhase::PortSelection,
                      state.PortsImplemented))
    {
        return FailureResult(state, budget);
    }
    if ((globalHostControl & AhciEnableMask) == 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::AhciModeDisabled,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::GlobalHostControl);
        return FailureResult(state, budget);
    }

    const Foundation::UInt32 portCount = (state.Capabilities & PortCountMask) + 1U;
    const Foundation::UInt32 validPortMask =
        (portCount == 32U) ? 0xFFFF'FFFFU : (Foundation::UInt32{1U} << portCount) - 1U;
    if ((state.PortsImplemented & ~validPortMask) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortsImplementedOutOfRange,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::PortsImplemented);
        return FailureResult(state, budget);
    }

    if (static_cast<Foundation::UInt32>(request.PortNumber) >= portCount)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidPortNumber,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::Capabilities);
        return FailureResult(state, budget);
    }
    const Foundation::UInt32 selectedPortMask = Foundation::UInt32{1U} << request.PortNumber;
    if ((state.PortsImplemented & selectedPortMask) == 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortNotImplemented,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::PortsImplemented);
        return FailureResult(state, budget);
    }
    if (!ContainsRegister(request.Abar, PortRegisterOffset(request.PortNumber, PortCommandIssueOffset)))
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InvalidAbarRange,
                        AhciReadDmaExtExecutionPhase::PortSelection);
        return FailureResult(state, budget);
    }
    state.PortBaseAddress = request.Abar.BaseAddress + PortRegisterOffset(request.PortNumber, 0U);
    Foundation::UInt32 sataStatus{0U};
    if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortSataStatusOffset,
                      AhciReadDmaExtRegister::PortSataStatus, AhciReadDmaExtExecutionPhase::PortSelection, sataStatus))
    {
        return FailureResult(state, budget);
    }
    if (!IsActiveSataLink(sataStatus))
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::InactiveSataLink,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::PortSataStatus);
        return FailureResult(state, budget);
    }
    Foundation::UInt32 signature{0U};
    if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortSignatureOffset,
                      AhciReadDmaExtRegister::PortSignature, AhciReadDmaExtExecutionPhase::PortSelection, signature))
    {
        return FailureResult(state, budget);
    }
    if (signature == AtapiDeviceSignature)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::AtapiDeviceNotSupported,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::PortSignature);
        return FailureResult(state, budget);
    }
    if (signature != AtaDeviceSignature)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::UnsupportedDeviceSignature,
                        AhciReadDmaExtExecutionPhase::PortSelection, AhciReadDmaExtRegister::PortSignature);
        return FailureResult(state, budget);
    }

    Foundation::UInt32 taskFileData{0U};
    Foundation::UInt32 sataActive{0U};
    Foundation::UInt32 commandIssue{0U};
    if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandOffset,
                      AhciReadDmaExtRegister::PortCommand, AhciReadDmaExtExecutionPhase::PortPreparation,
                      state.Original.Command) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                      AhciReadDmaExtRegister::PortTaskFileData, AhciReadDmaExtExecutionPhase::PortPreparation,
                      taskFileData) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortSataActiveOffset,
                      AhciReadDmaExtRegister::PortSataActive, AhciReadDmaExtExecutionPhase::PortPreparation,
                      sataActive) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandIssueOffset,
                      AhciReadDmaExtRegister::PortCommandIssue, AhciReadDmaExtExecutionPhase::PortPreparation,
                      commandIssue) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandListBaseOffset,
                      AhciReadDmaExtRegister::PortCommandListBase, AhciReadDmaExtExecutionPhase::PortPreparation,
                      state.Original.CommandListBase) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandListBaseUpperOffset,
                      AhciReadDmaExtRegister::PortCommandListBaseUpper, AhciReadDmaExtExecutionPhase::PortPreparation,
                      state.Original.CommandListBaseUpper) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortFisBaseOffset,
                      AhciReadDmaExtRegister::PortFisBase, AhciReadDmaExtExecutionPhase::PortPreparation,
                      state.Original.FisBase) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortFisBaseUpperOffset,
                      AhciReadDmaExtRegister::PortFisBaseUpper, AhciReadDmaExtExecutionPhase::PortPreparation,
                      state.Original.FisBaseUpper) ||
        !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortInterruptEnableOffset,
                      AhciReadDmaExtRegister::PortInterruptEnable, AhciReadDmaExtExecutionPhase::PortPreparation,
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
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtExecutionPhase::PortPreparation,
                        AhciReadDmaExtRegister::PortCommand);
        return FailureResult(state, budget);
    }
    if ((taskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtExecutionPhase::PortPreparation,
                        AhciReadDmaExtRegister::PortTaskFileData);
        return FailureResult(state, budget);
    }
    if (sataActive != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtExecutionPhase::PortPreparation,
                        AhciReadDmaExtRegister::PortSataActive);
        return FailureResult(state, budget);
    }
    if (commandIssue != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtExecutionPhase::PortPreparation,
                        AhciReadDmaExtRegister::PortCommandIssue);
        return FailureResult(state, budget);
    }
    if ((taskFileData & TaskFileFailureMask) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::TaskFileError,
                        AhciReadDmaExtExecutionPhase::PortPreparation, AhciReadDmaExtRegister::PortTaskFileData);
        return FailureResult(state, budget);
    }

    const PollDecision mutationDeadline = budget.Checkpoint();
    if (mutationDeadline != PollDecision::Continue)
    {
        RecordDeadlineFailure(state, mutationDeadline, AhciReadDmaExtExecutionPhase::PortPreparation);
        return FailureResult(state, budget);
    }

    const bool supports64BitAddressing = (state.Capabilities & Supports64BitAddressingMask) != 0U;
    const AhciReadDmaExtCommandBuildResult commandBuild = BuildCommand(request, supports64BitAddressing);
    if (!commandBuild.HasValue())
    {
        state.Failure.Command = commandBuild.GetError();
        SetPrimaryError(state, AhciReadDmaExtExecutionError::CommandRejected,
                        AhciReadDmaExtExecutionPhase::PortPreparation);
        return FailureResult(state, budget);
    }
    const AhciReadDmaExtCommandLayout commandLayout = commandBuild.GetValue();
    state.TransferSizeBytes = commandLayout.TransferSizeBytes;
    if (!StopEngine(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, false))
    {
        return FailureResult(state, budget);
    }
    if (!InstallCommandConfiguration(m_memoryMappedIo, m_memoryMappedIoWriter, state, request.Abar, commandLayout) ||
        !StartCommandEngine(m_memoryMappedIoWriter, state))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                      AhciReadDmaExtRegister::PortTaskFileData, AhciReadDmaExtExecutionPhase::CommandExecution,
                      taskFileData))
    {
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }
    state.Failure.TaskFileData = taskFileData;
    if ((taskFileData & (TaskFileBusyMask | TaskFileDataRequestMask)) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtExecutionPhase::CommandExecution,
                        AhciReadDmaExtRegister::PortTaskFileData);
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }
    if ((taskFileData & TaskFileFailureMask) != 0U)
    {
        SetPrimaryError(state, AhciReadDmaExtExecutionError::TaskFileError,
                        AhciReadDmaExtExecutionPhase::CommandExecution, AhciReadDmaExtRegister::PortTaskFileData);
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    const PollDecision issueDeadline = budget.Checkpoint();
    if (issueDeadline != PollDecision::Continue)
    {
        RecordDeadlineFailure(state, issueDeadline, AhciReadDmaExtExecutionPhase::CommandExecution,
                              AhciReadDmaExtRegister::PortCommandIssue);
        const bool recovered = RecoverPort(m_memoryMappedIo, m_memoryMappedIoWriter, state, budget, request.Abar);
        static_cast<void>(recovered);
        return FailureResult(state, budget);
    }

    // A backend error cannot prove that an MMIO write had no device effect.
    state.CommandIssueAttempted = true;
    state.Failure.CommandIssue = 1U;
    state.Failure.DataBufferMustRemainLive = true;
    if (!WriteRegister(m_memoryMappedIoWriter, state, state.PortBaseAddress + PortCommandIssueOffset,
                       AhciReadDmaExtRegister::PortCommandIssue, AhciReadDmaExtExecutionPhase::CommandExecution, 1U))
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
            RecordPollFailure(state, decision, AhciReadDmaExtExecutionError::CommandTimedOut,
                              AhciReadDmaExtExecutionPhase::CommandExecution, AhciReadDmaExtRegister::PortCommandIssue,
                              false);
            break;
        }

        if (!ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortInterruptStatusOffset,
                          AhciReadDmaExtRegister::PortInterruptStatus, AhciReadDmaExtExecutionPhase::CommandExecution,
                          state.CompletionInterruptStatus) ||
            !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortTaskFileDataOffset,
                          AhciReadDmaExtRegister::PortTaskFileData, AhciReadDmaExtExecutionPhase::CommandExecution,
                          state.CompletionTaskFileData) ||
            !ReadRegister(m_memoryMappedIo, state, state.PortBaseAddress + PortCommandIssueOffset,
                          AhciReadDmaExtRegister::PortCommandIssue, AhciReadDmaExtExecutionPhase::CommandExecution,
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

    if (!state.CommandCompleted || (state.Failure.Error != AhciReadDmaExtExecutionError::None))
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

    return SuccessResult(request, state, budget, output);
}
} // namespace Asteria::Kernel::Device
