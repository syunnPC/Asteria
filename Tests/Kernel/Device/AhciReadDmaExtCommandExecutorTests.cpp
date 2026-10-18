// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "Kernel/Device/AhciReadDmaExtCommandExecutor.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::IMemoryMappedIo;
using Hal::Device::IMemoryMappedIoWriter;
using Hal::Device::MemoryMappedIoReadError;
using Hal::Device::MemoryMappedIoWidth;
using Hal::Device::MemoryMappedIoWriteError;
using Hal::Time::IMonotonicClock;
using Kernel::Device::AhciHbaRegisterRange;
using Kernel::Device::AhciReadDmaExtCommandBuilder;
using Kernel::Device::AhciReadDmaExtCommandError;
using Kernel::Device::AhciReadDmaExtCommandExecutor;
using Kernel::Device::AhciReadDmaExtCommandPageWritableView;
using Kernel::Device::AhciReadDmaExtDataBufferSpan;
using Kernel::Device::AhciReadDmaExtExecutionError;
using Kernel::Device::AhciReadDmaExtExecutionFailure;
using Kernel::Device::AhciReadDmaExtExecutionPhase;
using Kernel::Device::AhciReadDmaExtExecutionReceipt;
using Kernel::Device::AhciReadDmaExtExecutionRequest;
using Kernel::Device::AhciReadDmaExtPortDisposition;
using Kernel::Device::AhciReadDmaExtRegister;

constexpr UInt64 AbarBase{0x0010'0000U};
constexpr UInt64 AbarLength{0x1100U};
constexpr UInt64 CommandPhysicalAddress{0x0000'0001'2345'6000ULL};
constexpr UInt64 DataPhysicalAddress{0x0000'0001'3456'8000ULL};
constexpr UInt64 LowCommandPhysicalAddress{0x0020'0000U};
constexpr UInt64 LowDataPhysicalAddress{0x0030'0000U};
constexpr UInt64 DeviceSectorCount{0x0020'0000U};
constexpr UInt64 StartingLba{0x0012'3456U};
constexpr UInt64 LogicalSectorSizeBytes{512U};
constexpr UInt64 SectorCount{8U};
constexpr UInt64 TransferSizeBytes{LogicalSectorSizeBytes * SectorCount};
constexpr UInt8 SelectedPort{2U};
constexpr UInt64 PortBase{AbarBase + 0x100U + (static_cast<UInt64>(SelectedPort) * 0x80U)};
constexpr UInt64 CapabilitiesAddress{AbarBase + 0x00U};
constexpr UInt64 GlobalHostControlAddress{AbarBase + 0x04U};
constexpr UInt64 GlobalInterruptStatusAddress{AbarBase + 0x08U};
constexpr UInt64 PortsImplementedAddress{AbarBase + 0x0CU};
constexpr UInt64 CommandListBaseAddress{PortBase + 0x00U};
constexpr UInt64 CommandListBaseUpperAddress{PortBase + 0x04U};
constexpr UInt64 FisBaseAddress{PortBase + 0x08U};
constexpr UInt64 FisBaseUpperAddress{PortBase + 0x0CU};
constexpr UInt64 PortInterruptStatusAddress{PortBase + 0x10U};
constexpr UInt64 PortInterruptEnableAddress{PortBase + 0x14U};
constexpr UInt64 PortCommandAddress{PortBase + 0x18U};
constexpr UInt64 TaskFileDataAddress{PortBase + 0x20U};
constexpr UInt64 PortSignatureAddress{PortBase + 0x24U};
constexpr UInt64 SataStatusAddress{PortBase + 0x28U};
constexpr UInt64 SataErrorAddress{PortBase + 0x30U};
constexpr UInt64 SataActiveAddress{PortBase + 0x34U};
constexpr UInt64 CommandIssueAddress{PortBase + 0x38U};
constexpr UInt32 AhciEnableMask{UInt32{1U} << 31U};
constexpr UInt32 Supports64BitAddressingMask{UInt32{1U} << 31U};
constexpr UInt32 CommandStartMask{1U};
constexpr UInt32 CommandListOverrideMask{UInt32{1U} << 3U};
constexpr UInt32 FisReceiveEnableMask{UInt32{1U} << 4U};
constexpr UInt32 FisReceiveRunningMask{UInt32{1U} << 14U};
constexpr UInt32 CommandListRunningMask{UInt32{1U} << 15U};
constexpr UInt32 PortCommandWritableEngineMask{CommandStartMask | FisReceiveEnableMask};
constexpr UInt32 CommandConfiguration{0x100U};
constexpr UInt32 InitialCommand{CommandConfiguration | CommandStartMask | FisReceiveEnableMask | FisReceiveRunningMask |
                                CommandListRunningMask};
constexpr UInt32 InitialCommandListBase{0x1111'2000U};
constexpr UInt32 InitialCommandListBaseUpper{0x0000'0002U};
constexpr UInt32 InitialFisBase{0x3333'4400U};
constexpr UInt32 InitialFisBaseUpper{0x0000'0004U};
constexpr UInt32 InitialInterruptEnable{0x55AA'0F0FU};
constexpr UInt8 Sentinel{0xA5U};
constexpr Size RegisterCount{static_cast<Size>(AbarLength / 4U)};
constexpr Size MaximumAccesses{512U};

alignas(4096) UInt8 g_commandPage[AhciReadDmaExtCommandBuilder::CommandPageSizeBytes]{};
alignas(4096) UInt8 g_beforePage[AhciReadDmaExtCommandBuilder::CommandPageSizeBytes]{};
alignas(4096) UInt8 g_dataBuffer[TransferSizeBytes]{};
alignas(4096) UInt8 g_beforeDataBuffer[TransferSizeBytes]{};

struct alignas(4096) RequestBackedCommandPage
{
    AhciReadDmaExtExecutionRequest Request;
    UInt8 Remaining[AhciReadDmaExtCommandBuilder::CommandPageSizeBytes - sizeof(AhciReadDmaExtExecutionRequest)];
};

struct alignas(4096) ReceiptBackedCommandPage
{
    AhciReadDmaExtExecutionReceipt Receipt;
    UInt8 Remaining[AhciReadDmaExtCommandBuilder::CommandPageSizeBytes - sizeof(AhciReadDmaExtExecutionReceipt)];
};

RequestBackedCommandPage g_requestBackedCommandPage{};
ReceiptBackedCommandPage g_receiptBackedCommandPage{};

static_assert(std::is_aggregate_v<AhciReadDmaExtExecutionRequest>);
static_assert(sizeof(AhciReadDmaExtExecutionError) == 1U);
static_assert(sizeof(AhciReadDmaExtRegister) == 1U);
static_assert(sizeof(AhciReadDmaExtPortDisposition) == 1U);
static_assert(sizeof(AhciReadDmaExtExecutionRequest) == 120U);
static_assert(sizeof(AhciReadDmaExtExecutionReceipt) == 104U);
static_assert(sizeof(AhciReadDmaExtExecutionFailure) == 56U);
static_assert(sizeof(RequestBackedCommandPage) == AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
static_assert(sizeof(ReceiptBackedCommandPage) == AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);

void FillBytes(UInt8* const bytes, const Size sizeBytes, const UInt8 value) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        bytes[index] = value;
    }
}

void CopyBytes(UInt8* const destination, const UInt8* const source, const Size sizeBytes) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        destination[index] = source[index];
    }
}

[[nodiscard]] bool BytesEqual(const UInt8* const first, const UInt8* const second, const Size sizeBytes) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        if (first[index] != second[index])
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool RegionHasValue(const UInt8* const bytes, const Size offset, const Size sizeBytes,
                                  const UInt8 value) noexcept
{
    for (Size index{0U}; index < sizeBytes; ++index)
    {
        if (bytes[offset + index] != value)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] UInt32 ReadLittleEndian32(const UInt8* const bytes) noexcept
{
    UInt32 value{0U};
    for (UInt32 index{0U}; index < 4U; ++index)
    {
        value |= static_cast<UInt32>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] UInt64 ReadLittleEndian64(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void WriteLittleEndian32(UInt8* const bytes, const UInt32 value) noexcept
{
    for (UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

struct Access
{
    UInt64 Address;
    UInt64 Value;
    bool IsWrite;
    UInt8 Reserved[7];
};

struct ExpectedWrite
{
    UInt64 Address;
    UInt32 Value;
};

class FakeClock final : public IMonotonicClock
{
public:
    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        UInt64 result{m_nextTicks};
        if (m_scriptIndex < m_scriptCount)
        {
            result = m_script[m_scriptIndex];
            ++m_scriptIndex;
        }
        else
        {
            m_nextTicks += m_step;
        }
        ++m_readCount;
        return result;
    }

    void Set(const UInt64 nextTicks, const UInt64 step) noexcept
    {
        m_nextTicks = nextTicks;
        m_step = step;
        m_readCount = 0U;
        m_scriptCount = 0U;
        m_scriptIndex = 0U;
    }

    void SetScript(const UInt64* const ticks, const Size count) noexcept
    {
        m_scriptCount = (count < MaximumScriptTicks) ? count : MaximumScriptTicks;
        for (Size index{0U}; index < m_scriptCount; ++index)
        {
            m_script[index] = ticks[index];
        }
        m_scriptIndex = 0U;
        m_readCount = 0U;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

private:
    static constexpr Size MaximumScriptTicks{32U};
    mutable UInt64 m_nextTicks{1U};
    UInt64 m_step{1U};
    mutable Size m_readCount{0U};
    UInt64 m_script[MaximumScriptTicks]{};
    Size m_scriptCount{0U};
    mutable Size m_scriptIndex{0U};
};

class FakeAhciMmio final : public IMemoryMappedIo, public IMemoryMappedIoWriter
{
public:
    explicit FakeAhciMmio(UInt8* const commandPage) noexcept : m_commandPage{commandPage}
    {
        Reset();
    }

    void Reset() noexcept
    {
        for (Size index{0U}; index < RegisterCount; ++index)
        {
            m_registers[index] = 0U;
        }
        m_accessCount = 0U;
        m_commandIssued = false;
        m_commandWasIssued = false;
        m_completeCommand = true;
        m_completionAfterPolls = 1U;
        m_commandPollCount = 0U;
        m_completionInterruptStatus = 1U;
        m_completionTaskFileData = 0U;
        m_staleBusyTaskFileReads = 0U;
        m_completionPrdbc = static_cast<UInt32>(TransferSizeBytes);
        m_completionFisType = 0x34U;
        m_completionFisStatus = 0x50U;
        m_completionFisError = 0U;
        m_commandStopDelayReads = 0U;
        m_fisStopDelayReads = 0U;
        m_recoveryCommandStopDelayReads = 0U;
        m_recoveryFisStopDelayReads = 0U;
        m_commandStopReadsRemaining = 0U;
        m_fisStopReadsRemaining = 0U;
        m_failedWriteAddress = 0U;
        m_failWrite = false;
        m_applyFailedWrite = false;
        m_failedReadAddress = 0U;
        m_failRead = false;
        m_failReadAfterIssue = false;
        m_persistentReadFailure = false;
        m_invalidReadAddress = 0U;
        m_returnInvalidRead = false;
        m_injectLateFatalOnClearCommandIssue = false;
        m_clearCommandIssueOnStop = true;
        m_taskFileDataOnCommandStop = 0U;
        m_sataActiveOnCommandStop = 0U;
        m_sataStatusOnCommandStop = 0x0103U;
        m_taskFileDataOnOriginalBaseRestore = 0U;
        m_changeTaskFileDataOnOriginalBaseRestore = false;

        SetRegister(CapabilitiesAddress, Supports64BitAddressingMask | 2U);
        SetRegister(GlobalHostControlAddress, AhciEnableMask);
        SetRegister(GlobalInterruptStatusAddress, 0x84U);
        SetRegister(PortsImplementedAddress, UInt32{1U} << SelectedPort);
        SetRegister(SataStatusAddress, 0x0103U);
        SetRegister(PortSignatureAddress, 0x0000'0101U);
        SetRegister(PortCommandAddress, InitialCommand);
        SetRegister(TaskFileDataAddress, 0U);
        SetRegister(SataActiveAddress, 0U);
        SetRegister(CommandIssueAddress, 0U);
        SetRegister(CommandListBaseAddress, InitialCommandListBase);
        SetRegister(CommandListBaseUpperAddress, InitialCommandListBaseUpper);
        SetRegister(FisBaseAddress, InitialFisBase);
        SetRegister(FisBaseUpperAddress, InitialFisBaseUpper);
        SetRegister(PortInterruptEnableAddress, InitialInterruptEnable);
        SetRegister(PortInterruptStatusAddress, 0x81U);
        SetRegister(SataErrorAddress, 0xA5A5U);
    }

    void SetRegister(const UInt64 address, const UInt32 value) noexcept
    {
        m_registers[ToIndex(address)] = value;
    }

    [[nodiscard]] UInt32 GetRegister(const UInt64 address) const noexcept
    {
        return m_registers[ToIndex(address)];
    }

    void SetStaleBusyTaskFileReads(const Size reads) noexcept
    {
        m_staleBusyTaskFileReads = reads;
    }

    void ConfigureCompletion(const bool completeCommand, const Size completionAfterPolls, const UInt32 interruptStatus,
                             const UInt32 taskFileData) noexcept
    {
        m_completeCommand = completeCommand;
        m_completionAfterPolls = completionAfterPolls;
        m_completionInterruptStatus = interruptStatus;
        m_completionTaskFileData = taskFileData;
    }

    void ConfigureDmaCompletion(const UInt32 prdbc, const UInt8 fisType, const UInt8 fisStatus,
                                const UInt8 fisError) noexcept
    {
        m_completionPrdbc = prdbc;
        m_completionFisType = fisType;
        m_completionFisStatus = fisStatus;
        m_completionFisError = fisError;
    }

    void SetEngineStopDelays(const Size commandReads, const Size fisReads) noexcept
    {
        m_commandStopDelayReads = commandReads;
        m_fisStopDelayReads = fisReads;
        m_recoveryCommandStopDelayReads = commandReads;
        m_recoveryFisStopDelayReads = fisReads;
    }

    void SetRecoveryEngineStopDelays(const Size commandReads, const Size fisReads) noexcept
    {
        m_recoveryCommandStopDelayReads = commandReads;
        m_recoveryFisStopDelayReads = fisReads;
    }

    void FailNextWriteAt(const UInt64 address, const bool applyWrite = false) noexcept
    {
        m_failedWriteAddress = address;
        m_failWrite = true;
        m_applyFailedWrite = applyWrite;
    }

    void FailNextReadAt(const UInt64 address) noexcept
    {
        m_failedReadAddress = address;
        m_failRead = true;
        m_failReadAfterIssue = false;
        m_persistentReadFailure = false;
    }

    void FailReadsAfterIssueAt(const UInt64 address, const bool persistent = true) noexcept
    {
        m_failedReadAddress = address;
        m_failRead = true;
        m_failReadAfterIssue = true;
        m_persistentReadFailure = persistent;
    }

    void ReturnInvalidValueAt(const UInt64 address) noexcept
    {
        m_invalidReadAddress = address;
        m_returnInvalidRead = true;
    }

    void InjectLateFatalOnClearCommandIssue() noexcept
    {
        m_injectLateFatalOnClearCommandIssue = true;
    }

    void KeepCommandIssueSetOnStop() noexcept
    {
        m_clearCommandIssueOnStop = false;
    }

    void SetCommandStopStatus(const UInt32 taskFileData, const UInt32 sataActive = 0U,
                              const UInt32 sataStatus = 0x0103U) noexcept
    {
        m_taskFileDataOnCommandStop = taskFileData;
        m_sataActiveOnCommandStop = sataActive;
        m_sataStatusOnCommandStop = sataStatus;
    }

    void SetTaskFileDataOnOriginalBaseRestore(const UInt32 taskFileData) noexcept
    {
        m_taskFileDataOnOriginalBaseRestore = taskFileData;
        m_changeTaskFileDataOnOriginalBaseRestore = true;
    }

    [[nodiscard]] Size GetAccessCount() const noexcept
    {
        return m_accessCount;
    }

    [[nodiscard]] const Access& GetAccess(const Size index) const noexcept
    {
        return m_accesses[index];
    }

    [[nodiscard]] MemoryMappedIoReadError Read(const UInt64 physicalAddress, const MemoryMappedIoWidth width,
                                               UInt64& output) noexcept override
    {
        if ((width != MemoryMappedIoWidth::DoubleWord) || !IsAddressValid(physicalAddress))
        {
            return MemoryMappedIoReadError::InvalidAddress;
        }

        MaybeCompleteCommand(physicalAddress);
        const bool eligibleReadFailure =
            m_failRead && (!m_failReadAfterIssue || m_commandWasIssued) && (physicalAddress == m_failedReadAddress);
        if (eligibleReadFailure)
        {
            if (!m_persistentReadFailure)
            {
                m_failRead = false;
            }
            return MemoryMappedIoReadError::HardwareFailure;
        }
        if (m_returnInvalidRead && (physicalAddress == m_invalidReadAddress))
        {
            m_returnInvalidRead = false;
            output = 0x0000'0001'0000'0000ULL;
            return MemoryMappedIoReadError::None;
        }
        if ((physicalAddress == CommandIssueAddress) && m_injectLateFatalOnClearCommandIssue && m_commandWasIssued &&
            ((GetRegister(CommandIssueAddress) & 1U) == 0U))
        {
            SetRegister(PortInterruptStatusAddress, GetRegister(PortInterruptStatusAddress) | 0x4000'0000U);
            m_injectLateFatalOnClearCommandIssue = false;
        }
        UInt32 value = GetRegister(physicalAddress);
        if ((physicalAddress == TaskFileDataAddress) && m_commandWasIssued && !m_commandIssued &&
            (m_staleBusyTaskFileReads != 0U))
        {
            // PxTFD not yet updated from the completion's D2H FIS.
            --m_staleBusyTaskFileReads;
            value = 0xC0U;
        }
        if (physicalAddress == PortCommandAddress)
        {
            UpdateStoppedEngine(value);
        }
        Record(physicalAddress, value, false);
        output = value;
        return MemoryMappedIoReadError::None;
    }

    [[nodiscard]] MemoryMappedIoWriteError Write(const UInt64 physicalAddress, const MemoryMappedIoWidth width,
                                                 const UInt64 value) noexcept override
    {
        if ((width != MemoryMappedIoWidth::DoubleWord) || !IsAddressValid(physicalAddress))
        {
            return MemoryMappedIoWriteError::InvalidAddress;
        }
        if (value > 0xFFFF'FFFFULL)
        {
            return MemoryMappedIoWriteError::ValueOutOfRange;
        }

        Record(physicalAddress, value, true);
        const bool writeFails = m_failWrite && (physicalAddress == m_failedWriteAddress);
        const bool writeApplies = !writeFails || m_applyFailedWrite;
        if (writeFails)
        {
            m_failWrite = false;
            m_applyFailedWrite = false;
        }
        if (!writeApplies)
        {
            return MemoryMappedIoWriteError::HardwareFailure;
        }

        const UInt32 narrowed = static_cast<UInt32>(value);
        if ((physicalAddress == PortInterruptStatusAddress) || (physicalAddress == SataErrorAddress) ||
            (physicalAddress == GlobalInterruptStatusAddress))
        {
            SetRegister(physicalAddress, GetRegister(physicalAddress) & ~narrowed);
        }
        else if (physicalAddress == PortCommandAddress)
        {
            WritePortCommand(narrowed);
        }
        else
        {
            SetRegister(physicalAddress, narrowed);
        }

        if (m_changeTaskFileDataOnOriginalBaseRestore && (physicalAddress == CommandListBaseAddress) &&
            (narrowed == InitialCommandListBase))
        {
            SetRegister(TaskFileDataAddress, m_taskFileDataOnOriginalBaseRestore);
            m_changeTaskFileDataOnOriginalBaseRestore = false;
        }

        if (physicalAddress == CommandIssueAddress)
        {
            m_commandIssued = (narrowed & 1U) != 0U;
            m_commandWasIssued = m_commandWasIssued || m_commandIssued;
            m_commandPollCount = 0U;
        }
        return writeFails ? MemoryMappedIoWriteError::HardwareFailure : MemoryMappedIoWriteError::None;
    }

private:
    [[nodiscard]] static constexpr bool IsAddressValid(const UInt64 address) noexcept
    {
        return (address >= AbarBase) && (address <= ((AbarBase + AbarLength) - 4U)) && ((address & 3U) == 0U);
    }

    [[nodiscard]] static constexpr Size ToIndex(const UInt64 address) noexcept
    {
        return static_cast<Size>((address - AbarBase) / 4U);
    }

    void Record(const UInt64 address, const UInt64 value, const bool isWrite) noexcept
    {
        if (m_accessCount < MaximumAccesses)
        {
            m_accesses[m_accessCount] = Access{
                .Address = address,
                .Value = value,
                .IsWrite = isWrite,
                .Reserved = {},
            };
            ++m_accessCount;
        }
    }

    void MaybeCompleteCommand(const UInt64 address) noexcept
    {
        if (!m_commandIssued || (address != PortInterruptStatusAddress))
        {
            return;
        }
        ++m_commandPollCount;
        if (!m_completeCommand || (m_commandPollCount < m_completionAfterPolls))
        {
            return;
        }

        WriteLittleEndian32(m_commandPage + 4U, m_completionPrdbc);
        UInt8* const deviceToHostFis = m_commandPage + AhciReadDmaExtCommandBuilder::ReceivedFisOffsetBytes + 0x40U;
        deviceToHostFis[0U] = m_completionFisType;
        deviceToHostFis[2U] = m_completionFisStatus;
        deviceToHostFis[3U] = m_completionFisError;
        SetRegister(CommandIssueAddress, 0U);
        SetRegister(PortInterruptStatusAddress, GetRegister(PortInterruptStatusAddress) | m_completionInterruptStatus);
        SetRegister(TaskFileDataAddress, m_completionTaskFileData);
        SetRegister(GlobalInterruptStatusAddress,
                    GetRegister(GlobalInterruptStatusAddress) | (UInt32{1U} << SelectedPort));
        m_commandIssued = false;
    }

    void WritePortCommand(const UInt32 value) noexcept
    {
        const UInt32 previous = GetRegister(PortCommandAddress);
        const Size commandStopDelayReads =
            m_commandWasIssued ? m_recoveryCommandStopDelayReads : m_commandStopDelayReads;
        const Size fisStopDelayReads = m_commandWasIssued ? m_recoveryFisStopDelayReads : m_fisStopDelayReads;
        bool commandEngineStopped{false};
        UInt32 next = value & ~(CommandListRunningMask | FisReceiveRunningMask);
        if ((value & CommandStartMask) != 0U)
        {
            next |= CommandListRunningMask;
            m_commandStopReadsRemaining = 0U;
        }
        else if ((previous & CommandListRunningMask) != 0U)
        {
            next |= CommandListRunningMask;
            m_commandStopReadsRemaining = commandStopDelayReads;
            if (commandStopDelayReads == 0U)
            {
                next &= ~CommandListRunningMask;
                commandEngineStopped = true;
            }
        }

        if ((value & FisReceiveEnableMask) != 0U)
        {
            next |= FisReceiveRunningMask;
            m_fisStopReadsRemaining = 0U;
        }
        else if ((previous & FisReceiveRunningMask) != 0U)
        {
            next |= FisReceiveRunningMask;
            m_fisStopReadsRemaining = fisStopDelayReads;
            if (fisStopDelayReads == 0U)
            {
                next &= ~FisReceiveRunningMask;
            }
        }
        SetRegister(PortCommandAddress, next);
        if (commandEngineStopped)
        {
            CompleteStoppedCommand();
        }
    }

    void UpdateStoppedEngine(UInt32& returnedValue) noexcept
    {
        bool commandEngineStopped{false};
        UInt32 stored = GetRegister(PortCommandAddress);
        if (((stored & CommandStartMask) == 0U) && ((stored & CommandListRunningMask) != 0U))
        {
            if (m_commandStopReadsRemaining == 0U)
            {
                stored &= ~CommandListRunningMask;
                returnedValue = stored;
                commandEngineStopped = true;
            }
            else
            {
                --m_commandStopReadsRemaining;
            }
        }
        if (((stored & FisReceiveEnableMask) == 0U) && ((stored & FisReceiveRunningMask) != 0U))
        {
            if (m_fisStopReadsRemaining == 0U)
            {
                stored &= ~FisReceiveRunningMask;
                returnedValue = stored;
            }
            else
            {
                --m_fisStopReadsRemaining;
            }
        }
        SetRegister(PortCommandAddress, stored);
        if (commandEngineStopped)
        {
            CompleteStoppedCommand();
        }
    }

    void CompleteStoppedCommand() noexcept
    {
        if (!m_commandIssued)
        {
            return;
        }
        // A compliant HBA retires issued commands when ST clears and CR
        // reaches zero. Fault modes below can deliberately violate that rule.
        SetRegister(TaskFileDataAddress, m_taskFileDataOnCommandStop);
        SetRegister(SataActiveAddress, m_sataActiveOnCommandStop);
        SetRegister(SataStatusAddress, m_sataStatusOnCommandStop);
        if (m_clearCommandIssueOnStop)
        {
            SetRegister(CommandIssueAddress, 0U);
            m_commandIssued = false;
        }
    }

    UInt8* m_commandPage;
    UInt32 m_registers[RegisterCount]{};
    Access m_accesses[MaximumAccesses]{};
    Size m_accessCount{0U};
    bool m_commandIssued{false};
    bool m_commandWasIssued{false};
    bool m_completeCommand{true};
    Size m_completionAfterPolls{1U};
    Size m_commandPollCount{0U};
    UInt32 m_completionInterruptStatus{1U};
    UInt32 m_completionTaskFileData{0U};
    Size m_staleBusyTaskFileReads{0U};
    UInt32 m_completionPrdbc{static_cast<UInt32>(TransferSizeBytes)};
    UInt8 m_completionFisType{0x34U};
    UInt8 m_completionFisStatus{0x50U};
    UInt8 m_completionFisError{0U};
    Size m_commandStopDelayReads{0U};
    Size m_fisStopDelayReads{0U};
    Size m_recoveryCommandStopDelayReads{0U};
    Size m_recoveryFisStopDelayReads{0U};
    Size m_commandStopReadsRemaining{0U};
    Size m_fisStopReadsRemaining{0U};
    UInt64 m_failedWriteAddress{0U};
    bool m_failWrite{false};
    bool m_applyFailedWrite{false};
    UInt64 m_failedReadAddress{0U};
    bool m_failRead{false};
    bool m_failReadAfterIssue{false};
    bool m_persistentReadFailure{false};
    UInt64 m_invalidReadAddress{0U};
    bool m_returnInvalidRead{false};
    bool m_injectLateFatalOnClearCommandIssue{false};
    bool m_clearCommandIssueOnStop{true};
    UInt32 m_taskFileDataOnCommandStop{0U};
    UInt32 m_sataActiveOnCommandStop{0U};
    UInt32 m_sataStatusOnCommandStop{0x0103U};
    UInt32 m_taskFileDataOnOriginalBaseRestore{0U};
    bool m_changeTaskFileDataOnOriginalBaseRestore{false};
};

[[nodiscard]] AhciReadDmaExtExecutionRequest MakeRequest(const UInt64 commandPhysicalAddress = CommandPhysicalAddress,
                                                         const UInt64 dataPhysicalAddress = DataPhysicalAddress,
                                                         const UInt64 abarLength = AbarLength,
                                                         const Size maximumPollCount = 32U,
                                                         const Size maximumRecoveryPollCount = 32U) noexcept
{
    return AhciReadDmaExtExecutionRequest{
        .Abar =
            AhciHbaRegisterRange{
                .BaseAddress = AbarBase,
                .LengthBytes = abarLength,
            },
        .CommandPage =
            AhciReadDmaExtCommandPageWritableView{
                .Data = g_commandPage,
                .SizeBytes = AhciReadDmaExtCommandBuilder::CommandPageSizeBytes,
                .PhysicalAddress = commandPhysicalAddress,
            },
        .DataBuffer =
            AhciReadDmaExtDataBufferSpan{
                .PhysicalAddress = dataPhysicalAddress,
                .SizeBytes = TransferSizeBytes,
            },
        .DeviceSectorCount = DeviceSectorCount,
        .StartingLba = StartingLba,
        .LogicalSectorSizeBytes = LogicalSectorSizeBytes,
        .SectorCount = SectorCount,
        .Polling =
            {
                .DeadlineTicks = 1'000U,
                .MaximumPollCount = maximumPollCount,
                .MaximumRecoveryPollCount = maximumRecoveryPollCount,
            },
        .PortNumber = SelectedPort,
        .Supports48BitAddressing = true,
        .Reserved = {},
    };
}

[[nodiscard]] Size CollectWrites(const FakeAhciMmio& mmio, ExpectedWrite* const writes, const Size capacity) noexcept
{
    Size count{0U};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite && (count < capacity))
        {
            writes[count] = ExpectedWrite{
                .Address = access.Address,
                .Value = static_cast<UInt32>(access.Value),
            };
            ++count;
        }
    }
    return count;
}

[[nodiscard]] bool WritesEqual(const ExpectedWrite* const left, const ExpectedWrite* const right,
                               const Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        if ((left[index].Address != right[index].Address) || (left[index].Value != right[index].Value))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool HasCommandIssueReadAfterIssueWrite(const FakeAhciMmio& mmio, const UInt32 expectedValue) noexcept
{
    bool issueWriteObserved{false};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite && (access.Address == CommandIssueAddress) && (access.Value == 1U))
        {
            issueWriteObserved = true;
        }
        else if (issueWriteObserved && !access.IsWrite && (access.Address == CommandIssueAddress) &&
                 (access.Value == expectedValue))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool HasFreshFinalStatusSequence(const FakeAhciMmio& mmio) noexcept
{
    bool issueWriteObserved{false};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite && (access.Address == CommandIssueAddress) && (access.Value == 1U))
        {
            issueWriteObserved = true;
            continue;
        }
        if (issueWriteObserved && !access.IsWrite && (access.Address == CommandIssueAddress) &&
            ((access.Value & 1U) == 0U) && ((index + 2U) < mmio.GetAccessCount()))
        {
            const Access& interruptStatus = mmio.GetAccess(index + 1U);
            const Access& taskFileData = mmio.GetAccess(index + 2U);
            return !interruptStatus.IsWrite && (interruptStatus.Address == PortInterruptStatusAddress) &&
                   !taskFileData.IsWrite && (taskFileData.Address == TaskFileDataAddress);
        }
    }
    return false;
}

[[nodiscard]] bool HasRestartWriteAfterIssue(const FakeAhciMmio& mmio) noexcept
{
    bool issueWriteObserved{false};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite && (access.Address == CommandIssueAddress) && (access.Value == 1U))
        {
            issueWriteObserved = true;
        }
        else if (issueWriteObserved && access.IsWrite && (access.Address == PortCommandAddress) &&
                 ((access.Value & CommandStartMask) != 0U))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool HasOriginalBaseWriteAfterIssue(const FakeAhciMmio& mmio) noexcept
{
    bool issueWriteObserved{false};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite && (access.Address == CommandIssueAddress) && (access.Value == 1U))
        {
            issueWriteObserved = true;
        }
        else if (issueWriteObserved && access.IsWrite && (access.Address == CommandListBaseAddress) &&
                 (access.Value == InitialCommandListBase))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool HasRestartSafetySequence(const FakeAhciMmio& mmio) noexcept
{
    enum class Stage : UInt8
    {
        Issue,
        Stop,
        CommandIssueClear,
        SataActiveClear,
        TaskFileIdle,
        OriginalBase,
        RestartTaskFileIdle,
        ActiveLink,
        Restart,
    };
    Stage stage{Stage::Issue};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if ((stage == Stage::Issue) && access.IsWrite && (access.Address == CommandIssueAddress) &&
            (access.Value == 1U))
        {
            stage = Stage::Stop;
        }
        else if ((stage == Stage::Stop) && access.IsWrite && (access.Address == PortCommandAddress) &&
                 ((access.Value & CommandStartMask) == 0U))
        {
            stage = Stage::CommandIssueClear;
        }
        else if ((stage == Stage::CommandIssueClear) && !access.IsWrite && (access.Address == CommandIssueAddress) &&
                 (access.Value == 0U))
        {
            stage = Stage::SataActiveClear;
        }
        else if ((stage == Stage::SataActiveClear) && !access.IsWrite && (access.Address == SataActiveAddress) &&
                 (access.Value == 0U))
        {
            stage = Stage::TaskFileIdle;
        }
        else if ((stage == Stage::TaskFileIdle) && !access.IsWrite && (access.Address == TaskFileDataAddress) &&
                 ((access.Value & 0x88U) == 0U))
        {
            stage = Stage::OriginalBase;
        }
        else if ((stage == Stage::OriginalBase) && access.IsWrite && (access.Address == CommandListBaseAddress) &&
                 (access.Value == InitialCommandListBase))
        {
            stage = Stage::RestartTaskFileIdle;
        }
        else if ((stage == Stage::RestartTaskFileIdle) && !access.IsWrite && (access.Address == TaskFileDataAddress) &&
                 ((access.Value & 0x88U) == 0U))
        {
            stage = Stage::ActiveLink;
        }
        else if ((stage == Stage::ActiveLink) && !access.IsWrite && (access.Address == SataStatusAddress) &&
                 ((access.Value & 0x0F0FU) == 0x0103U))
        {
            stage = Stage::Restart;
        }
        else if ((stage == Stage::Restart) && access.IsWrite && (access.Address == PortCommandAddress) &&
                 ((access.Value & CommandStartMask) != 0U))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool EveryPortCommandWritePreservesConfiguration(const FakeAhciMmio& mmio,
                                                               const UInt32 expectedConfiguration) noexcept
{
    bool commandWriteObserved{false};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite && (access.Address == PortCommandAddress))
        {
            commandWriteObserved = true;
            const UInt32 writtenCommand = static_cast<UInt32>(access.Value);
            if ((writtenCommand & ~PortCommandWritableEngineMask) != expectedConfiguration)
            {
                return false;
            }
        }
    }
    return commandWriteObserved;
}

void PrepareSentinels() noexcept
{
    FillBytes(g_commandPage, sizeof(g_commandPage), Sentinel);
    FillBytes(g_dataBuffer, sizeof(g_dataBuffer), Sentinel);
    CopyBytes(g_beforePage, g_commandPage, sizeof(g_commandPage));
    CopyBytes(g_beforeDataBuffer, g_dataBuffer, sizeof(g_dataBuffer));
}

void TestSuccessfulExecutionAndExactTransaction(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio mmio{g_commandPage};
    FakeClock clock;
    AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
    AhciReadDmaExtExecutionReceipt receipt{};
    const AhciReadDmaExtExecutionFailure failure = executor.Execute(MakeRequest(), receipt);
    ASTERIA_EXPECT(runner, "READ DMA EXT completes on the caller-selected ATA port",
                   failure.Error == AhciReadDmaExtExecutionError::None);
    if (failure.Error != AhciReadDmaExtExecutionError::None)
    {
        return;
    }

    ASTERIA_EXPECT(
        runner, "success receipt records exact transfer facts and completion evidence",
        receipt.PortNumber == SelectedPort, receipt.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
        receipt.PortSignature == 0x0000'0101U, receipt.TransferSizeBytes == TransferSizeBytes,
        receipt.StartingLba == StartingLba, receipt.SectorCount == SectorCount,
        receipt.LogicalSectorSizeBytes == LogicalSectorSizeBytes, receipt.DeviceSectorCount == DeviceSectorCount,
        receipt.DeviceToHostFisStatus == 0x50U, receipt.DeviceToHostFisError == 0U);
    ASTERIA_EXPECT(runner, "slot-zero header has one PRDT and exact CTBA/PRDBC values",
                   ReadLittleEndian32(g_commandPage) == 0x0001'0005U,
                   ReadLittleEndian32(g_commandPage + 4U) == TransferSizeBytes,
                   ReadLittleEndian64(g_commandPage + 8U) == (CommandPhysicalAddress + 0x500U));
    const UInt8* const commandFis = g_commandPage + AhciReadDmaExtCommandBuilder::CommandTableOffsetBytes;
    ASTERIA_EXPECT(runner, "executor forwards exact LBA48 and sector-count facts to the builder",
                   commandFis[0U] == 0x27U, commandFis[1U] == 0x80U, commandFis[2U] == 0x25U, commandFis[4U] == 0x56U,
                   commandFis[5U] == 0x34U, commandFis[6U] == 0x12U, commandFis[7U] == 0x40U, commandFis[8U] == 0U,
                   commandFis[9U] == 0U, commandFis[10U] == 0U, commandFis[12U] == 8U, commandFis[13U] == 0U);
    const UInt8* const prdt = commandFis + 128U;
    ASTERIA_EXPECT(runner, "PRDT carries the exact external destination span",
                   ReadLittleEndian64(prdt) == DataPhysicalAddress,
                   ReadLittleEndian32(prdt + 12U) == (TransferSizeBytes - 1U));
    ASTERIA_EXPECT(runner, "unused command bytes and the external data buffer are never touched",
                   RegionHasValue(g_commandPage, 0x590U, 0xA70U, Sentinel),
                   BytesEqual(g_dataBuffer, g_beforeDataBuffer, sizeof(g_dataBuffer)));
    ASTERIA_EXPECT(runner, "success restores bases, interrupt enable, and original engine state",
                   mmio.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   mmio.GetRegister(CommandListBaseUpperAddress) == InitialCommandListBaseUpper,
                   mmio.GetRegister(FisBaseAddress) == InitialFisBase,
                   mmio.GetRegister(FisBaseUpperAddress) == InitialFisBaseUpper,
                   mmio.GetRegister(PortInterruptEnableAddress) == InitialInterruptEnable,
                   (mmio.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) ==
                       (CommandStartMask | FisReceiveEnableMask));
    ASTERIA_EXPECT(runner, "W1C writes clear only observed selected-port status",
                   mmio.GetRegister(PortInterruptStatusAddress) == 0U, mmio.GetRegister(SataErrorAddress) == 0U,
                   mmio.GetRegister(GlobalInterruptStatusAddress) == 0x80U);

    constexpr UInt32 commandLow = static_cast<UInt32>(CommandPhysicalAddress & 0xFFFF'FFFFULL);
    constexpr UInt32 commandHigh = static_cast<UInt32>(CommandPhysicalAddress >> 32U);
    constexpr ExpectedWrite expected[25U]{
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask},
        {PortCommandAddress, CommandConfiguration},
        {PortInterruptEnableAddress, 0U},
        {CommandListBaseAddress, commandLow},
        {CommandListBaseUpperAddress, commandHigh},
        {FisBaseAddress, commandLow + 0x400U},
        {FisBaseUpperAddress, commandHigh},
        {PortInterruptStatusAddress, 0x81U},
        {SataErrorAddress, 0xA5A5U},
        {GlobalInterruptStatusAddress, UInt32{1U} << SelectedPort},
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask},
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask | CommandStartMask},
        {CommandIssueAddress, 1U},
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask},
        {PortCommandAddress, CommandConfiguration},
        {PortInterruptStatusAddress, 1U},
        {SataErrorAddress, 0U},
        {GlobalInterruptStatusAddress, UInt32{1U} << SelectedPort},
        {CommandListBaseAddress, InitialCommandListBase},
        {CommandListBaseUpperAddress, InitialCommandListBaseUpper},
        {FisBaseAddress, InitialFisBase},
        {FisBaseUpperAddress, InitialFisBaseUpper},
        {PortInterruptEnableAddress, InitialInterruptEnable},
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask},
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask | CommandStartMask},
    };
    ExpectedWrite actual[32U]{};
    const Size writeCount = CollectWrites(mmio, actual, 32U);
    ASTERIA_EXPECT(runner, "success-path MMIO writes have exact order, addresses, and values", writeCount == 25U,
                   WritesEqual(actual, expected, 25U));
    ASTERIA_EXPECT(runner, "PxCI clear is followed by fresh PxIS and PxTFD snapshots",
                   HasFreshFinalStatusSequence(mmio));
}

void CheckEarlyFailure(TestContext& runner, FakeAhciMmio& mmio, FakeClock& clock,
                       const AhciReadDmaExtExecutionRequest& request, const AhciReadDmaExtExecutionError expected,
                       const char* const description,
                       const AhciReadDmaExtRegister expectedRegister = AhciReadDmaExtRegister::None) noexcept
{
    PrepareSentinels();
    AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
    AhciReadDmaExtExecutionReceipt receipt{};
    receipt.StartingLba = 0xA5A5'A5A5'A5A5'A5A5ULL;
    receipt.PortNumber = 0x5AU;
    const AhciReadDmaExtExecutionFailure failure = executor.Execute(request, receipt);
    ASTERIA_EXPECT(runner, description, failure.Error == expected,
                   (expectedRegister == AhciReadDmaExtRegister::None) || (failure.Register == expectedRegister),
                   failure.PortDisposition == AhciReadDmaExtPortDisposition::Unchanged,
                   !failure.CommandPageMustRemainLive, !failure.DataBufferMustRemainLive,
                   receipt.StartingLba == 0xA5A5'A5A5'A5A5'A5A5ULL, receipt.PortNumber == 0x5AU,
                   BytesEqual(g_commandPage, g_beforePage, sizeof(g_commandPage)),
                   BytesEqual(g_dataBuffer, g_beforeDataBuffer, sizeof(g_dataBuffer)));
}

void TestValidationSelectionAndBusyFailures(TestContext& runner) noexcept
{
    FakeClock clock;

    FakeAhciMmio badAbar{g_commandPage};
    AhciReadDmaExtExecutionRequest badAbarRequest = MakeRequest();
    badAbarRequest.Abar.BaseAddress = 0U;
    CheckEarlyFailure(runner, badAbar, clock, badAbarRequest, AhciReadDmaExtExecutionError::InvalidAbarRange,
                      "zero ABAR is rejected before MMIO or DMA mutation");

    FakeAhciMmio invalidPort{g_commandPage};
    AhciReadDmaExtExecutionRequest invalidPortRequest = MakeRequest();
    invalidPortRequest.PortNumber = 32U;
    CheckEarlyFailure(runner, invalidPort, clock, invalidPortRequest, AhciReadDmaExtExecutionError::InvalidPortNumber,
                      "port numbers outside the AHCI 32-port space are rejected");

    FakeAhciMmio beyondNp{g_commandPage};
    beyondNp.SetRegister(CapabilitiesAddress, Supports64BitAddressingMask | 1U);
    beyondNp.SetRegister(PortsImplementedAddress, 1U);
    CheckEarlyFailure(runner, beyondNp, clock, MakeRequest(), AhciReadDmaExtExecutionError::InvalidPortNumber,
                      "selected port must be within CAP.NP");

    FakeAhciMmio invalidPi{g_commandPage};
    invalidPi.SetRegister(CapabilitiesAddress, Supports64BitAddressingMask);
    CheckEarlyFailure(runner, invalidPi, clock, MakeRequest(), AhciReadDmaExtExecutionError::PortsImplementedOutOfRange,
                      "PI bits above CAP.NP are rejected before port access");

    FakeAhciMmio notImplemented{g_commandPage};
    notImplemented.SetRegister(PortsImplementedAddress, 1U);
    CheckEarlyFailure(runner, notImplemented, clock, MakeRequest(), AhciReadDmaExtExecutionError::PortNotImplemented,
                      "the selected port must be present in PI");

    FakeAhciMmio shortAbar{g_commandPage};
    CheckEarlyFailure(runner, shortAbar, clock, MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, 0x200U),
                      AhciReadDmaExtExecutionError::InvalidAbarRange,
                      "ABAR extent must contain every selected-port register accessed");

    FakeAhciMmio ahciDisabled{g_commandPage};
    ahciDisabled.SetRegister(GlobalHostControlAddress, 0U);
    CheckEarlyFailure(runner, ahciDisabled, clock, MakeRequest(), AhciReadDmaExtExecutionError::AhciModeDisabled,
                      "GHC.AE must be enabled");

    FakeAhciMmio inactive{g_commandPage};
    inactive.SetRegister(SataStatusAddress, 0x0101U);
    CheckEarlyFailure(runner, inactive, clock, MakeRequest(), AhciReadDmaExtExecutionError::InactiveSataLink,
                      "selected port requires DET=3 and IPM=1");

    FakeAhciMmio atapi{g_commandPage};
    atapi.SetRegister(PortSignatureAddress, 0xEB14'0101U);
    CheckEarlyFailure(runner, atapi, clock, MakeRequest(), AhciReadDmaExtExecutionError::AtapiDeviceNotSupported,
                      "ATAPI signature has a distinct rejection");

    FakeAhciMmio unknown{g_commandPage};
    unknown.SetRegister(PortSignatureAddress, 0x9669'0101U);
    CheckEarlyFailure(runner, unknown, clock, MakeRequest(), AhciReadDmaExtExecutionError::UnsupportedDeviceSignature,
                      "unknown selected-port signature is rejected");

    struct BusyCase
    {
        UInt64 Address;
        UInt32 Value;
        AhciReadDmaExtExecutionError Error;
        AhciReadDmaExtRegister Register;
        const char* Description;
    };
    constexpr BusyCase busyCases[]{
        {TaskFileDataAddress, 0x80U, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtRegister::PortTaskFileData,
         "PxTFD.BSY rejects the command"},
        {TaskFileDataAddress, 0x08U, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtRegister::PortTaskFileData,
         "PxTFD.DRQ rejects the command"},
        {TaskFileDataAddress, 0x01U, AhciReadDmaExtExecutionError::TaskFileError,
         AhciReadDmaExtRegister::PortTaskFileData, "preexisting PxTFD.ERR is typed"},
        {TaskFileDataAddress, 0x20U, AhciReadDmaExtExecutionError::TaskFileError,
         AhciReadDmaExtRegister::PortTaskFileData, "preexisting PxTFD.DF is typed"},
        {SataActiveAddress, 1U, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtRegister::PortSataActive,
         "nonzero PxSACT rejects slot ownership"},
        {CommandIssueAddress, 2U, AhciReadDmaExtExecutionError::PortBusy, AhciReadDmaExtRegister::PortCommandIssue,
         "any preexisting PxCI owner rejects execution"},
    };
    for (const BusyCase& busyCase : busyCases)
    {
        FakeAhciMmio busy{g_commandPage};
        busy.SetRegister(busyCase.Address, busyCase.Value);
        CheckEarlyFailure(runner, busy, clock, MakeRequest(), busyCase.Error, busyCase.Description, busyCase.Register);
    }

    FakeAhciMmio invalidEngine{g_commandPage};
    invalidEngine.SetRegister(PortCommandAddress, CommandConfiguration | CommandStartMask | CommandListRunningMask);
    CheckEarlyFailure(runner, invalidEngine, clock, MakeRequest(), AhciReadDmaExtExecutionError::PortBusy,
                      "ST without FRE is rejected as an inconsistent active engine");
}

void TestAliasingPolicyAndBuilderBoundaries(TestContext& runner) noexcept
{
    UInt8* const requestPage = reinterpret_cast<UInt8*>(&g_requestBackedCommandPage);
    FillBytes(requestPage, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes, Sentinel);
    g_requestBackedCommandPage.Request = MakeRequest();
    g_requestBackedCommandPage.Request.CommandPage.Data = requestPage;
    CopyBytes(g_beforePage, requestPage, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
    FakeAhciMmio requestMmio{requestPage};
    FakeClock requestClock;
    AhciReadDmaExtCommandExecutor requestExecutor{requestMmio, requestMmio, requestClock};
    AhciReadDmaExtExecutionReceipt requestReceipt{};
    const AhciReadDmaExtExecutionFailure requestFailure =
        requestExecutor.Execute(g_requestBackedCommandPage.Request, requestReceipt);
    ASTERIA_EXPECT(runner, "a request inside the command page is rejected before mutation",
                   requestFailure.Error == AhciReadDmaExtExecutionError::CommandPageOverlapsRequest,
                   requestMmio.GetAccessCount() == 0U,
                   BytesEqual(requestPage, g_beforePage, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes));

    UInt8* const receiptPage = reinterpret_cast<UInt8*>(&g_receiptBackedCommandPage);
    FillBytes(receiptPage, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes, Sentinel);
    g_receiptBackedCommandPage.Receipt = AhciReadDmaExtExecutionReceipt{};
    g_receiptBackedCommandPage.Receipt.PortNumber = 0x5AU;
    CopyBytes(g_beforePage, receiptPage, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes);
    FakeAhciMmio receiptMmio{receiptPage};
    FakeClock receiptClock;
    AhciReadDmaExtCommandExecutor receiptExecutor{receiptMmio, receiptMmio, receiptClock};
    AhciReadDmaExtExecutionRequest receiptRequest = MakeRequest();
    receiptRequest.CommandPage.Data = receiptPage;
    const AhciReadDmaExtExecutionFailure receiptFailure =
        receiptExecutor.Execute(receiptRequest, g_receiptBackedCommandPage.Receipt);
    ASTERIA_EXPECT(runner, "a receipt inside the command page is rejected atomically",
                   receiptFailure.Error == AhciReadDmaExtExecutionError::CommandPageOverlapsReceipt,
                   receiptMmio.GetAccessCount() == 0U, g_receiptBackedCommandPage.Receipt.PortNumber == 0x5AU,
                   BytesEqual(receiptPage, g_beforePage, AhciReadDmaExtCommandBuilder::CommandPageSizeBytes));

    struct PolicyCase
    {
        UInt64 Deadline;
        Size Polls;
        Size RecoveryPolls;
        const char* Description;
    };
    constexpr Size maximumSize{~Size{0U}};
    constexpr PolicyCase policyCases[]{
        {0U, 1U, 1U, "zero deadline is invalid"},
        {~UInt64{0U}, 1U, 1U, "all-ones deadline is invalid"},
        {100U, 0U, 1U, "zero execution poll allowance is invalid"},
        {100U, 1U, 0U, "zero recovery poll allowance is invalid"},
        {100U, maximumSize, 1U, "combined poll-count overflow is invalid"},
    };
    for (const PolicyCase& policyCase : policyCases)
    {
        FakeAhciMmio mmio{g_commandPage};
        FakeClock clock;
        AhciReadDmaExtExecutionRequest request = MakeRequest();
        request.Polling.DeadlineTicks = policyCase.Deadline;
        request.Polling.MaximumPollCount = policyCase.Polls;
        request.Polling.MaximumRecoveryPollCount = policyCase.RecoveryPolls;
        CheckEarlyFailure(runner, mmio, clock, request, AhciReadDmaExtExecutionError::InvalidPollingPolicy,
                          policyCase.Description);
    }

    struct BuilderCase
    {
        enum class Kind : UInt8
        {
            NoLba48,
            OutsideCapacity,
            WrongDataSize,
            PhysicalOverlap,
        } KindValue;
        AhciReadDmaExtCommandError Expected;
        const char* Description;
    };
    constexpr BuilderCase builderCases[]{
        {BuilderCase::Kind::NoLba48, AhciReadDmaExtCommandError::DeviceDoesNotSupport48BitAddressing,
         "device LBA48 capability is forwarded exactly"},
        {BuilderCase::Kind::OutsideCapacity, AhciReadDmaExtCommandError::TransferOutsideDeviceCapacity,
         "device capacity and transfer range are forwarded exactly"},
        {BuilderCase::Kind::WrongDataSize, AhciReadDmaExtCommandError::InvalidDataBufferSize,
         "external destination length must match exact transfer bytes"},
        {BuilderCase::Kind::PhysicalOverlap, AhciReadDmaExtCommandError::CommandPageOverlapsDataBuffer,
         "controller-visible command and data spans cannot overlap"},
    };
    for (const BuilderCase& builderCase : builderCases)
    {
        PrepareSentinels();
        FakeAhciMmio mmio{g_commandPage};
        FakeClock clock;
        AhciReadDmaExtExecutionRequest request = MakeRequest();
        switch (builderCase.KindValue)
        {
        case BuilderCase::Kind::NoLba48:
            request.Supports48BitAddressing = false;
            break;
        case BuilderCase::Kind::OutsideCapacity:
            request.DeviceSectorCount = request.StartingLba;
            break;
        case BuilderCase::Kind::WrongDataSize:
            --request.DataBuffer.SizeBytes;
            break;
        case BuilderCase::Kind::PhysicalOverlap:
            request.DataBuffer.PhysicalAddress = request.CommandPage.PhysicalAddress;
            break;
        }
        AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
        AhciReadDmaExtExecutionReceipt receipt{};
        receipt.StartingLba = 0xA5A5U;
        const AhciReadDmaExtExecutionFailure failure = executor.Execute(request, receipt);
        ASTERIA_EXPECT(runner, builderCase.Description, failure.Error == AhciReadDmaExtExecutionError::CommandRejected,
                       failure.Command == builderCase.Expected,
                       failure.Phase == AhciReadDmaExtExecutionPhase::PortPreparation,
                       failure.PortDisposition == AhciReadDmaExtPortDisposition::Unchanged,
                       !failure.CommandPageMustRemainLive, !failure.DataBufferMustRemainLive,
                       receipt.StartingLba == 0xA5A5U, BytesEqual(g_commandPage, g_beforePage, sizeof(g_commandPage)),
                       BytesEqual(g_dataBuffer, g_beforeDataBuffer, sizeof(g_dataBuffer)));
    }

    PrepareSentinels();
    FakeAhciMmio noS64a{g_commandPage};
    noS64a.SetRegister(CapabilitiesAddress, 2U);
    FakeClock noS64aClock;
    AhciReadDmaExtCommandExecutor noS64aExecutor{noS64a, noS64a, noS64aClock};
    AhciReadDmaExtExecutionReceipt noS64aReceipt{};
    const AhciReadDmaExtExecutionFailure noS64aFailure = noS64aExecutor.Execute(MakeRequest(), noS64aReceipt);
    ASTERIA_EXPECT(runner, "CAP.S64A bounds both command and external data spans",
                   noS64aFailure.Error == AhciReadDmaExtExecutionError::CommandRejected,
                   noS64aFailure.Command == AhciReadDmaExtCommandError::ControllerAddressWidthExceeded,
                   BytesEqual(g_commandPage, g_beforePage, sizeof(g_commandPage)));

    PrepareSentinels();
    FakeAhciMmio low32{g_commandPage};
    low32.SetRegister(CapabilitiesAddress, 2U);
    FakeClock low32Clock;
    AhciReadDmaExtCommandExecutor low32Executor{low32, low32, low32Clock};
    AhciReadDmaExtExecutionReceipt low32Receipt{};
    const AhciReadDmaExtExecutionFailure low32Failure =
        low32Executor.Execute(MakeRequest(LowCommandPhysicalAddress, LowDataPhysicalAddress), low32Receipt);
    ASTERIA_EXPECT(runner, "a 32-bit controller accepts exact below-4-GiB DMA extents",
                   low32Failure.Error == AhciReadDmaExtExecutionError::None,
                   low32.GetRegister(CommandListBaseUpperAddress) == InitialCommandListBaseUpper,
                   low32.GetRegister(FisBaseUpperAddress) == InitialFisBaseUpper);
}

void TestMmioReadAndValueFailures(TestContext& runner) noexcept
{
    struct ReadCase
    {
        UInt64 Address;
        AhciReadDmaExtRegister Register;
        const char* Description;
    };
    constexpr ReadCase readCases[]{
        {CapabilitiesAddress, AhciReadDmaExtRegister::Capabilities, "CAP read failure is typed"},
        {GlobalHostControlAddress, AhciReadDmaExtRegister::GlobalHostControl, "GHC read failure is typed"},
        {PortsImplementedAddress, AhciReadDmaExtRegister::PortsImplemented, "PI read failure is typed"},
        {SataStatusAddress, AhciReadDmaExtRegister::PortSataStatus, "PxSSTS read failure is typed"},
        {PortSignatureAddress, AhciReadDmaExtRegister::PortSignature, "PxSIG read failure is typed"},
        {PortCommandAddress, AhciReadDmaExtRegister::PortCommand, "PxCMD preflight read failure is typed"},
        {TaskFileDataAddress, AhciReadDmaExtRegister::PortTaskFileData, "PxTFD preflight read failure is typed"},
        {SataActiveAddress, AhciReadDmaExtRegister::PortSataActive, "PxSACT read failure is typed"},
        {CommandIssueAddress, AhciReadDmaExtRegister::PortCommandIssue, "PxCI preflight read failure is typed"},
        {CommandListBaseAddress, AhciReadDmaExtRegister::PortCommandListBase, "PxCLB snapshot read failure is typed"},
        {CommandListBaseUpperAddress, AhciReadDmaExtRegister::PortCommandListBaseUpper,
         "PxCLBU snapshot read failure is typed"},
        {FisBaseAddress, AhciReadDmaExtRegister::PortFisBase, "PxFB snapshot read failure is typed"},
        {FisBaseUpperAddress, AhciReadDmaExtRegister::PortFisBaseUpper, "PxFBU snapshot read failure is typed"},
        {PortInterruptEnableAddress, AhciReadDmaExtRegister::PortInterruptEnable,
         "PxIE snapshot read failure is typed"},
    };
    for (const ReadCase& readCase : readCases)
    {
        PrepareSentinels();
        FakeAhciMmio mmio{g_commandPage};
        mmio.FailNextReadAt(readCase.Address);
        FakeClock clock;
        AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
        AhciReadDmaExtExecutionReceipt receipt{};
        receipt.StartingLba = 0xA5A5U;
        const AhciReadDmaExtExecutionFailure failure = executor.Execute(MakeRequest(), receipt);
        ASTERIA_EXPECT(runner, readCase.Description,
                       failure.Error == AhciReadDmaExtExecutionError::MemoryMappedIoReadFailed,
                       failure.Register == readCase.Register,
                       failure.MemoryMappedIoRead == MemoryMappedIoReadError::HardwareFailure,
                       failure.PortDisposition == AhciReadDmaExtPortDisposition::Unchanged,
                       receipt.StartingLba == 0xA5A5U, BytesEqual(g_commandPage, g_beforePage, sizeof(g_commandPage)));
    }

    PrepareSentinels();
    FakeAhciMmio invalidValue{g_commandPage};
    invalidValue.ReturnInvalidValueAt(CapabilitiesAddress);
    FakeClock invalidClock;
    AhciReadDmaExtCommandExecutor invalidExecutor{invalidValue, invalidValue, invalidClock};
    AhciReadDmaExtExecutionReceipt invalidReceipt{};
    const AhciReadDmaExtExecutionFailure invalidFailure = invalidExecutor.Execute(MakeRequest(), invalidReceipt);
    ASTERIA_EXPECT(runner, "successful MMIO reads wider than a dword are rejected",
                   invalidFailure.Error == AhciReadDmaExtExecutionError::InvalidRegisterValue,
                   invalidFailure.Register == AhciReadDmaExtRegister::Capabilities);
}

void TestCommandFailuresAndTransactionalRestore(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio taskFile{g_commandPage};
    taskFile.ConfigureCompletion(true, 1U, 0U, 1U);
    FakeClock taskClock;
    AhciReadDmaExtCommandExecutor taskExecutor{taskFile, taskFile, taskClock};
    AhciReadDmaExtExecutionReceipt taskReceipt{};
    const AhciReadDmaExtExecutionFailure taskFailure = taskExecutor.Execute(MakeRequest(), taskReceipt);
    ASTERIA_EXPECT(runner, "PxTFD error is reported and a quiescent command is restored",
                   taskFailure.Error == AhciReadDmaExtExecutionError::TaskFileError, taskFailure.TaskFileData == 1U,
                   taskFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !taskFailure.CommandPageMustRemainLive, !taskFailure.DataBufferMustRemainLive,
                   taskFile.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   BytesEqual(g_dataBuffer, g_beforeDataBuffer, sizeof(g_dataBuffer)));

    PrepareSentinels();
    FakeAhciMmio interrupt{g_commandPage};
    interrupt.ConfigureCompletion(true, 1U, 0x4000'0000U, 0U);
    FakeClock interruptClock;
    AhciReadDmaExtCommandExecutor interruptExecutor{interrupt, interrupt, interruptClock};
    AhciReadDmaExtExecutionReceipt interruptReceipt{};
    const AhciReadDmaExtExecutionFailure interruptFailure = interruptExecutor.Execute(MakeRequest(), interruptReceipt);
    ASTERIA_EXPECT(runner, "fatal PxIS status is diagnosed and restored transactionally",
                   interruptFailure.Error == AhciReadDmaExtExecutionError::PortInterruptError,
                   interruptFailure.PortInterruptStatus == 0x4000'0000U,
                   interruptFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !interruptFailure.CommandPageMustRemainLive, !interruptFailure.DataBufferMustRemainLive);

    PrepareSentinels();
    FakeAhciMmio lateFatal{g_commandPage};
    lateFatal.ConfigureCompletion(true, 1U, 1U, 0U);
    lateFatal.InjectLateFatalOnClearCommandIssue();
    FakeClock lateFatalClock;
    AhciReadDmaExtCommandExecutor lateFatalExecutor{lateFatal, lateFatal, lateFatalClock};
    AhciReadDmaExtExecutionReceipt lateFatalReceipt{};
    lateFatalReceipt.PortNumber = 0x5AU;
    const AhciReadDmaExtExecutionFailure lateFatalFailure = lateFatalExecutor.Execute(MakeRequest(), lateFatalReceipt);
    ASTERIA_EXPECT(runner, "a fatal PxIS arriving at CI clear is caught by fresh snapshots",
                   lateFatalFailure.Error == AhciReadDmaExtExecutionError::PortInterruptError,
                   (lateFatalFailure.PortInterruptStatus & 0x4000'0000U) != 0U,
                   lateFatalFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !lateFatalFailure.CommandPageMustRemainLive, !lateFatalFailure.DataBufferMustRemainLive,
                   lateFatalReceipt.PortNumber == 0x5AU, HasFreshFinalStatusSequence(lateFatal));

    PrepareSentinels();
    FakeAhciMmio failedWrite{g_commandPage};
    failedWrite.FailNextWriteAt(CommandListBaseUpperAddress);
    FakeClock writeClock;
    AhciReadDmaExtCommandExecutor writeExecutor{failedWrite, failedWrite, writeClock};
    AhciReadDmaExtExecutionReceipt writeReceipt{};
    const AhciReadDmaExtExecutionFailure writeFailure = writeExecutor.Execute(MakeRequest(), writeReceipt);
    ASTERIA_EXPECT(runner, "a programming write failure restores already-touched registers",
                   writeFailure.Error == AhciReadDmaExtExecutionError::MemoryMappedIoWriteFailed,
                   writeFailure.Register == AhciReadDmaExtRegister::PortCommandListBaseUpper,
                   writeFailure.MemoryMappedIoWrite == MemoryMappedIoWriteError::HardwareFailure,
                   writeFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !writeFailure.CommandPageMustRemainLive, !writeFailure.DataBufferMustRemainLive,
                   failedWrite.GetRegister(CommandListBaseAddress) == InitialCommandListBase);

    PrepareSentinels();
    FakeAhciMmio appliedIssue{g_commandPage};
    appliedIssue.ConfigureCompletion(false, 1U, 0U, 0U);
    appliedIssue.FailNextWriteAt(CommandIssueAddress, true);
    FakeClock appliedIssueClock;
    AhciReadDmaExtCommandExecutor appliedIssueExecutor{appliedIssue, appliedIssue, appliedIssueClock};
    AhciReadDmaExtExecutionReceipt appliedIssueReceipt{};
    appliedIssueReceipt.PortNumber = 0x5AU;
    const AhciReadDmaExtExecutionFailure appliedIssueFailure =
        appliedIssueExecutor.Execute(MakeRequest(), appliedIssueReceipt);
    ASTERIA_EXPECT(runner, "a compliant engine stop retires an applied-but-error PxCI write safely",
                   appliedIssueFailure.Error == AhciReadDmaExtExecutionError::MemoryMappedIoWriteFailed,
                   appliedIssueFailure.Register == AhciReadDmaExtRegister::PortCommandIssue,
                   appliedIssueFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !appliedIssueFailure.CommandPageMustRemainLive, !appliedIssueFailure.DataBufferMustRemainLive,
                   appliedIssueFailure.CommandIssue == 0U, HasCommandIssueReadAfterIssueWrite(appliedIssue, 0U),
                   HasRestartSafetySequence(appliedIssue), appliedIssue.GetRegister(CommandIssueAddress) == 0U,
                   appliedIssue.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   (appliedIssue.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) ==
                       (CommandStartMask | FisReceiveEnableMask),
                   appliedIssueReceipt.PortNumber == 0x5AU);

    PrepareSentinels();
    FakeAhciMmio uncertainRead{g_commandPage};
    uncertainRead.ConfigureCompletion(false, 1U, 0U, 0U);
    uncertainRead.FailReadsAfterIssueAt(CommandIssueAddress);
    FakeClock uncertainReadClock;
    AhciReadDmaExtCommandExecutor uncertainReadExecutor{uncertainRead, uncertainRead, uncertainReadClock};
    AhciReadDmaExtExecutionReceipt uncertainReadReceipt{};
    uncertainReadReceipt.PortNumber = 0x5AU;
    const AhciReadDmaExtExecutionFailure uncertainReadFailure =
        uncertainReadExecutor.Execute(MakeRequest(), uncertainReadReceipt);
    ASTERIA_EXPECT(runner, "an unreadable retired PxCI conservatively retains both DMA spans",
                   uncertainReadFailure.Error == AhciReadDmaExtExecutionError::MemoryMappedIoReadFailed,
                   uncertainReadFailure.Register == AhciReadDmaExtRegister::PortCommandIssue,
                   uncertainReadFailure.RecoveryError == AhciReadDmaExtExecutionError::MemoryMappedIoReadFailed,
                   uncertainReadFailure.RecoveryRegister == AhciReadDmaExtRegister::PortCommandIssue,
                   uncertainReadFailure.PortDisposition == AhciReadDmaExtPortDisposition::Uncertain,
                   uncertainReadFailure.CommandPageMustRemainLive, uncertainReadFailure.DataBufferMustRemainLive,
                   uncertainRead.GetRegister(CommandIssueAddress) == 0U, uncertainReadReceipt.PortNumber == 0x5AU);
}

void TestLateTaskFileUpdateAfterCompletion(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio late{g_commandPage};
    late.SetStaleBusyTaskFileReads(3U);
    FakeClock lateClock;
    AhciReadDmaExtCommandExecutor lateExecutor{late, late, lateClock};
    AhciReadDmaExtExecutionReceipt lateReceipt{};
    const AhciReadDmaExtExecutionFailure lateFailure = lateExecutor.Execute(MakeRequest(), lateReceipt);
    ASTERIA_EXPECT(runner, "a PxTFD that still shows BSY just after CI clears settles within the bound",
                   lateFailure.Error == AhciReadDmaExtExecutionError::None);

    PrepareSentinels();
    FakeAhciMmio stuck{g_commandPage};
    stuck.SetStaleBusyTaskFileReads(1U << 20U);
    FakeClock stuckClock;
    AhciReadDmaExtCommandExecutor stuckExecutor{stuck, stuck, stuckClock};
    AhciReadDmaExtExecutionReceipt stuckReceipt{};
    const AhciReadDmaExtExecutionFailure stuckFailure = stuckExecutor.Execute(MakeRequest(), stuckReceipt);
    ASTERIA_EXPECT(runner, "a PxTFD that stays BSY past the bound is still PortBusy",
                   stuckFailure.Error == AhciReadDmaExtExecutionError::PortBusy,
                   stuckFailure.Phase == AhciReadDmaExtExecutionPhase::CommandExecution);
}

void TestCompletionEvidenceOracle(TestContext& runner) noexcept
{
    struct CompletionCase
    {
        UInt32 Prdbc;
        UInt8 FisType;
        UInt8 FisStatus;
        UInt8 FisError;
        AhciReadDmaExtExecutionError Expected;
        UInt32 ExpectedTaskFileData;
        const char* Description;
    };
    constexpr CompletionCase cases[]{
        {static_cast<UInt32>(TransferSizeBytes - 1U), 0x34U, 0x50U, 0U,
         AhciReadDmaExtExecutionError::IncompleteDataTransfer, 0U, "PRDBC one byte short is rejected"},
        {static_cast<UInt32>(TransferSizeBytes + 1U), 0x34U, 0x50U, 0U,
         AhciReadDmaExtExecutionError::IncompleteDataTransfer, 0U, "PRDBC one byte long is rejected"},
        {static_cast<UInt32>(TransferSizeBytes), 0x00U, 0x50U, 0U, AhciReadDmaExtExecutionError::InvalidCompletionFis,
         0U, "CI clear requires a register D2H FIS"},
        {static_cast<UInt32>(TransferSizeBytes), 0x34U, 0x80U, 0U, AhciReadDmaExtExecutionError::TaskFileError, 0x80U,
         "D2H BSY rejects completion"},
        {static_cast<UInt32>(TransferSizeBytes), 0x34U, 0x08U, 0U, AhciReadDmaExtExecutionError::TaskFileError, 0x08U,
         "D2H DRQ rejects completion"},
        {static_cast<UInt32>(TransferSizeBytes), 0x34U, 0x20U, 0U, AhciReadDmaExtExecutionError::TaskFileError, 0x20U,
         "D2H DF rejects completion"},
        {static_cast<UInt32>(TransferSizeBytes), 0x34U, 0x01U, 0U, AhciReadDmaExtExecutionError::TaskFileError, 0x01U,
         "D2H ERR rejects completion"},
        {static_cast<UInt32>(TransferSizeBytes), 0x34U, 0x50U, 0x04U, AhciReadDmaExtExecutionError::TaskFileError,
         0x450U, "nonzero D2H error rejects completion independently"},
    };

    for (const CompletionCase& completionCase : cases)
    {
        PrepareSentinels();
        FakeAhciMmio mmio{g_commandPage};
        mmio.ConfigureDmaCompletion(completionCase.Prdbc, completionCase.FisType, completionCase.FisStatus,
                                    completionCase.FisError);
        FakeClock clock;
        AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
        AhciReadDmaExtExecutionReceipt receipt{};
        receipt.StartingLba = 0xA5A5U;
        const AhciReadDmaExtExecutionFailure failure = executor.Execute(MakeRequest(), receipt);
        ASTERIA_EXPECT(runner, completionCase.Description, failure.Error == completionCase.Expected,
                       (completionCase.Expected != AhciReadDmaExtExecutionError::TaskFileError) ||
                           (failure.TaskFileData == completionCase.ExpectedTaskFileData),
                       failure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                       !failure.CommandPageMustRemainLive, !failure.DataBufferMustRemainLive,
                       receipt.StartingLba == 0xA5A5U,
                       BytesEqual(g_dataBuffer, g_beforeDataBuffer, sizeof(g_dataBuffer)));
    }
}

void TestClockAndAbsoluteDeadlineBoundaries(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio exhausted{g_commandPage};
    FakeClock exhaustedClock;
    exhaustedClock.Set(~UInt64{0U}, 0U);
    AhciReadDmaExtCommandExecutor exhaustedExecutor{exhausted, exhausted, exhaustedClock};
    AhciReadDmaExtExecutionReceipt exhaustedReceipt{};
    const AhciReadDmaExtExecutionFailure exhaustedFailure = exhaustedExecutor.Execute(MakeRequest(), exhaustedReceipt);
    ASTERIA_EXPECT(runner, "an exhausted clock is rejected before MMIO and DMA mutation",
                   exhaustedFailure.Error == AhciReadDmaExtExecutionError::ClockExhausted,
                   exhaustedFailure.Phase == AhciReadDmaExtExecutionPhase::Validation, exhausted.GetAccessCount() == 0U,
                   BytesEqual(g_commandPage, g_beforePage, sizeof(g_commandPage)));

    PrepareSentinels();
    FakeAhciMmio alreadyExpired{g_commandPage};
    FakeClock expiredClock;
    expiredClock.Set(1'000U, 0U);
    AhciReadDmaExtCommandExecutor expiredExecutor{alreadyExpired, alreadyExpired, expiredClock};
    AhciReadDmaExtExecutionReceipt expiredReceipt{};
    const AhciReadDmaExtExecutionFailure expiredFailure = expiredExecutor.Execute(MakeRequest(), expiredReceipt);
    ASTERIA_EXPECT(runner, "a deadline equal to the initial tick is already expired",
                   expiredFailure.Error == AhciReadDmaExtExecutionError::DeadlineReached,
                   expiredFailure.ObservedTicks == 1'000U, alreadyExpired.GetAccessCount() == 0U);

    PrepareSentinels();
    FakeAhciMmio beforeMutation{g_commandPage};
    FakeClock mutationClock;
    mutationClock.Set(1U, 999U);
    AhciReadDmaExtCommandExecutor mutationExecutor{beforeMutation, beforeMutation, mutationClock};
    AhciReadDmaExtExecutionReceipt mutationReceipt{};
    const AhciReadDmaExtExecutionFailure mutationFailure = mutationExecutor.Execute(MakeRequest(), mutationReceipt);
    ASTERIA_EXPECT(runner, "absolute deadline is rechecked immediately before page mutation",
                   mutationFailure.Error == AhciReadDmaExtExecutionError::DeadlineReached,
                   mutationFailure.PortDisposition == AhciReadDmaExtPortDisposition::Unchanged,
                   !mutationFailure.CommandPageMustRemainLive, !mutationFailure.DataBufferMustRemainLive,
                   BytesEqual(g_commandPage, g_beforePage, sizeof(g_commandPage)));

    PrepareSentinels();
    FakeAhciMmio beforeIssue{g_commandPage};
    beforeIssue.SetRegister(PortCommandAddress, CommandConfiguration);
    FakeClock issueClock;
    issueClock.Set(1U, 500U);
    AhciReadDmaExtCommandExecutor issueExecutor{beforeIssue, beforeIssue, issueClock};
    AhciReadDmaExtExecutionReceipt issueReceipt{};
    issueReceipt.PortNumber = 0x5AU;
    const AhciReadDmaExtExecutionFailure issueFailure = issueExecutor.Execute(MakeRequest(), issueReceipt);
    ASTERIA_EXPECT(runner, "deadline is rechecked before PxCI issue and failure stays atomic",
                   issueFailure.Error == AhciReadDmaExtExecutionError::DeadlineReached,
                   issueFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !issueFailure.CommandPageMustRemainLive, !issueFailure.DataBufferMustRemainLive,
                   beforeIssue.GetRegister(CommandIssueAddress) == 0U, issueReceipt.PortNumber == 0x5AU);

    PrepareSentinels();
    FakeAhciMmio reversed{g_commandPage};
    reversed.ConfigureCompletion(false, 1U, 0U, 0U);
    reversed.SetRegister(PortCommandAddress, CommandConfiguration);
    FakeClock reversedClock;
    constexpr UInt64 reversedTicks[]{1U, 2U, 3U, 2U};
    reversedClock.SetScript(reversedTicks, 4U);
    AhciReadDmaExtCommandExecutor reversedExecutor{reversed, reversed, reversedClock};
    AhciReadDmaExtExecutionReceipt reversedReceipt{};
    const AhciReadDmaExtExecutionFailure reversedFailure = reversedExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), reversedReceipt);
    ASTERIA_EXPECT(runner, "clock reversal preserves the primary error after compliant retirement",
                   reversedFailure.Error == AhciReadDmaExtExecutionError::ClockReversed,
                   reversedFailure.Phase == AhciReadDmaExtExecutionPhase::CommandExecution,
                   reversedFailure.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   !reversedFailure.CommandPageMustRemainLive, !reversedFailure.DataBufferMustRemainLive,
                   reversed.GetRegister(CommandIssueAddress) == 0U);
}

void TestTimeoutRecoveryRequiresRestartSafeState(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio mmio{g_commandPage};
    mmio.ConfigureCompletion(false, 1U, 0U, 0U);
    FakeClock clock;
    AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
    AhciReadDmaExtExecutionReceipt receipt{};
    receipt.StartingLba = 0xA5A5U;
    const AhciReadDmaExtExecutionFailure failure =
        executor.Execute(MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), receipt);
    ASTERIA_EXPECT(runner, "a compliant ST clear retires a timed-out command before restart",
                   failure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   failure.Register == AhciReadDmaExtRegister::PortCommandIssue,
                   failure.PortDisposition == AhciReadDmaExtPortDisposition::Restored, failure.CommandIssue == 0U,
                   !failure.CommandPageMustRemainLive, !failure.DataBufferMustRemainLive,
                   failure.RecoveryError == AhciReadDmaExtExecutionError::None, HasRestartSafetySequence(mmio),
                   receipt.StartingLba == 0xA5A5U);
    ASTERIA_EXPECT(runner, "safe timeout recovery restores bases, interrupts, and engine state",
                   (mmio.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) ==
                       (CommandStartMask | FisReceiveEnableMask),
                   mmio.GetRegister(PortInterruptEnableAddress) == InitialInterruptEnable,
                   mmio.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   mmio.GetRegister(CommandIssueAddress) == 0U);
    ASTERIA_EXPECT(runner, "stalled-command polling consumes the exact clock budget", clock.GetReadCount() == 7U);
}

void TestUnsafeRecoveryNeverRestartsCommandEngine(TestContext& runner) noexcept
{
    struct TaskFileCase
    {
        UInt32 Value;
        const char* Description;
    };
    constexpr TaskFileCase taskFileCases[]{
        {0x80U, "retained BSY restores inert bases without restarting ST"},
        {0x08U, "retained DRQ restores inert bases without restarting ST"},
    };
    for (const TaskFileCase& taskFileCase : taskFileCases)
    {
        PrepareSentinels();
        FakeAhciMmio mmio{g_commandPage};
        mmio.ConfigureCompletion(false, 1U, 0U, 0U);
        mmio.SetCommandStopStatus(taskFileCase.Value);
        FakeClock clock;
        AhciReadDmaExtCommandExecutor executor{mmio, mmio, clock};
        AhciReadDmaExtExecutionReceipt receipt{};
        receipt.PortNumber = 0x5AU;
        const AhciReadDmaExtExecutionFailure failure =
            executor.Execute(MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), receipt);
        ASTERIA_EXPECT(
            runner, taskFileCase.Description, failure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
            failure.RecoveryError == AhciReadDmaExtExecutionError::PortBusy,
            failure.RecoveryRegister == AhciReadDmaExtRegister::PortTaskFileData,
            failure.TaskFileData == taskFileCase.Value,
            failure.PortDisposition == AhciReadDmaExtPortDisposition::CommandStopped,
            !failure.CommandPageMustRemainLive, !failure.DataBufferMustRemainLive,
            mmio.GetRegister(CommandIssueAddress) == 0U,
            mmio.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
            (mmio.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) == FisReceiveEnableMask,
            HasOriginalBaseWriteAfterIssue(mmio), !HasRestartWriteAfterIssue(mmio), receipt.PortNumber == 0x5AU);
    }

    PrepareSentinels();
    FakeAhciMmio retainedCommand{g_commandPage};
    retainedCommand.ConfigureCompletion(false, 1U, 0U, 0U);
    retainedCommand.KeepCommandIssueSetOnStop();
    FakeClock retainedCommandClock;
    AhciReadDmaExtCommandExecutor retainedCommandExecutor{retainedCommand, retainedCommand, retainedCommandClock};
    AhciReadDmaExtExecutionReceipt retainedCommandReceipt{};
    const AhciReadDmaExtExecutionFailure retainedCommandFailure = retainedCommandExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), retainedCommandReceipt);
    ASTERIA_EXPECT(runner, "a noncompliant retained PxCI slot keeps both DMA spans live",
                   retainedCommandFailure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   retainedCommandFailure.RecoveryError == AhciReadDmaExtExecutionError::PortBusy,
                   retainedCommandFailure.RecoveryRegister == AhciReadDmaExtRegister::PortCommandIssue,
                   retainedCommandFailure.PortDisposition == AhciReadDmaExtPortDisposition::CommandStopped,
                   retainedCommandFailure.CommandPageMustRemainLive, retainedCommandFailure.DataBufferMustRemainLive,
                   retainedCommand.GetRegister(CommandIssueAddress) == 1U,
                   !HasOriginalBaseWriteAfterIssue(retainedCommand), !HasRestartWriteAfterIssue(retainedCommand));

    PrepareSentinels();
    FakeAhciMmio retainedSataActive{g_commandPage};
    retainedSataActive.ConfigureCompletion(false, 1U, 0U, 0U);
    retainedSataActive.SetCommandStopStatus(0U, 1U);
    FakeClock retainedSataActiveClock;
    AhciReadDmaExtCommandExecutor retainedSataActiveExecutor{retainedSataActive, retainedSataActive,
                                                             retainedSataActiveClock};
    AhciReadDmaExtExecutionReceipt retainedSataActiveReceipt{};
    const AhciReadDmaExtExecutionFailure retainedSataActiveFailure = retainedSataActiveExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), retainedSataActiveReceipt);
    ASTERIA_EXPECT(runner, "a retained PxSACT slot preserves DMA ownership and suppresses restart",
                   retainedSataActiveFailure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   retainedSataActiveFailure.RecoveryError == AhciReadDmaExtExecutionError::PortBusy,
                   retainedSataActiveFailure.RecoveryRegister == AhciReadDmaExtRegister::PortSataActive,
                   retainedSataActiveFailure.CommandPageMustRemainLive,
                   retainedSataActiveFailure.DataBufferMustRemainLive,
                   !HasOriginalBaseWriteAfterIssue(retainedSataActive), !HasRestartWriteAfterIssue(retainedSataActive));

    PrepareSentinels();
    FakeAhciMmio lateBusy{g_commandPage};
    lateBusy.ConfigureCompletion(false, 1U, 0U, 0U);
    lateBusy.SetTaskFileDataOnOriginalBaseRestore(0x80U);
    FakeClock lateBusyClock;
    AhciReadDmaExtCommandExecutor lateBusyExecutor{lateBusy, lateBusy, lateBusyClock};
    AhciReadDmaExtExecutionReceipt lateBusyReceipt{};
    const AhciReadDmaExtExecutionFailure lateBusyFailure = lateBusyExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), lateBusyReceipt);
    ASTERIA_EXPECT(runner, "PxTFD is sampled again after inert restoration and before restart",
                   lateBusyFailure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   lateBusyFailure.RecoveryError == AhciReadDmaExtExecutionError::PortBusy,
                   lateBusyFailure.RecoveryRegister == AhciReadDmaExtRegister::PortTaskFileData,
                   lateBusyFailure.TaskFileData == 0x80U,
                   lateBusyFailure.PortDisposition == AhciReadDmaExtPortDisposition::CommandStopped,
                   !lateBusyFailure.CommandPageMustRemainLive, !lateBusyFailure.DataBufferMustRemainLive,
                   HasOriginalBaseWriteAfterIssue(lateBusy), !HasRestartWriteAfterIssue(lateBusy));

    PrepareSentinels();
    FakeAhciMmio inactiveLink{g_commandPage};
    inactiveLink.ConfigureCompletion(false, 1U, 0U, 0U);
    inactiveLink.SetCommandStopStatus(0U, 0U, 0U);
    FakeClock inactiveLinkClock;
    AhciReadDmaExtCommandExecutor inactiveLinkExecutor{inactiveLink, inactiveLink, inactiveLinkClock};
    AhciReadDmaExtExecutionReceipt inactiveLinkReceipt{};
    const AhciReadDmaExtExecutionFailure inactiveLinkFailure = inactiveLinkExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 4U, 8U), inactiveLinkReceipt);
    ASTERIA_EXPECT(runner, "a link loss observed immediately before restart leaves ST clear",
                   inactiveLinkFailure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   inactiveLinkFailure.RecoveryError == AhciReadDmaExtExecutionError::InactiveSataLink,
                   inactiveLinkFailure.RecoveryRegister == AhciReadDmaExtRegister::PortSataStatus,
                   inactiveLinkFailure.PortDisposition == AhciReadDmaExtPortDisposition::CommandStopped,
                   !inactiveLinkFailure.CommandPageMustRemainLive, !inactiveLinkFailure.DataBufferMustRemainLive,
                   inactiveLink.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   HasOriginalBaseWriteAfterIssue(inactiveLink), !HasRestartWriteAfterIssue(inactiveLink));
}

void TestRecoveryExhaustionRetainsBothDmaSpans(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio commandRunning{g_commandPage};
    commandRunning.ConfigureCompletion(false, 1U, 0U, 0U);
    commandRunning.SetRecoveryEngineStopDelays(8U, 0U);
    FakeClock commandRunningClock;
    AhciReadDmaExtCommandExecutor commandRunningExecutor{commandRunning, commandRunning, commandRunningClock};
    AhciReadDmaExtExecutionReceipt commandRunningReceipt{};
    const AhciReadDmaExtExecutionFailure commandRunningFailure = commandRunningExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 3U, 2U), commandRunningReceipt);
    ASTERIA_EXPECT(runner, "post-issue CR exhaustion retains both DMA spans and leaves FRE set",
                   commandRunningFailure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   commandRunningFailure.RecoveryError == AhciReadDmaExtExecutionError::EngineRestoreFailed,
                   commandRunningFailure.RecoveryRegister == AhciReadDmaExtRegister::PortCommand,
                   commandRunningFailure.PortDisposition == AhciReadDmaExtPortDisposition::StopRequested,
                   commandRunningFailure.CommandPageMustRemainLive, commandRunningFailure.DataBufferMustRemainLive,
                   (commandRunningFailure.PortCommand & CommandListRunningMask) != 0U,
                   (commandRunning.GetRegister(PortCommandAddress) & CommandStartMask) == 0U,
                   (commandRunning.GetRegister(PortCommandAddress) & FisReceiveEnableMask) != 0U,
                   commandRunning.GetRegister(CommandIssueAddress) == 1U);

    PrepareSentinels();
    FakeAhciMmio fisRunning{g_commandPage};
    fisRunning.ConfigureCompletion(false, 1U, 0U, 0U);
    fisRunning.SetRecoveryEngineStopDelays(0U, 8U);
    FakeClock fisRunningClock;
    AhciReadDmaExtCommandExecutor fisRunningExecutor{fisRunning, fisRunning, fisRunningClock};
    AhciReadDmaExtExecutionReceipt fisRunningReceipt{};
    const AhciReadDmaExtExecutionFailure fisRunningFailure = fisRunningExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 3U, 2U), fisRunningReceipt);
    ASTERIA_EXPECT(runner, "post-issue FR exhaustion retains both spans after ordered stops",
                   fisRunningFailure.Error == AhciReadDmaExtExecutionError::CommandTimedOut,
                   fisRunningFailure.RecoveryError == AhciReadDmaExtExecutionError::EngineRestoreFailed,
                   fisRunningFailure.RecoveryRegister == AhciReadDmaExtRegister::PortCommand,
                   fisRunningFailure.PortDisposition == AhciReadDmaExtPortDisposition::StopRequested,
                   fisRunningFailure.CommandPageMustRemainLive, fisRunningFailure.DataBufferMustRemainLive,
                   (fisRunningFailure.PortCommand & FisReceiveRunningMask) != 0U,
                   (fisRunning.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) == 0U,
                   fisRunning.GetRegister(CommandIssueAddress) == 0U);
}

void TestPortCommandWritesNeverReplayActionFields(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio cloMmio{g_commandPage};
    cloMmio.SetRegister(PortCommandAddress, InitialCommand | CommandListOverrideMask);
    FakeClock cloClock;
    AhciReadDmaExtCommandExecutor cloExecutor{cloMmio, cloMmio, cloClock};
    AhciReadDmaExtExecutionReceipt cloReceipt{};
    const AhciReadDmaExtExecutionFailure cloFailure = cloExecutor.Execute(MakeRequest(), cloReceipt);
    ASTERIA_EXPECT(runner, "every PxCMD write masks the read-write-one CLO action bit",
                   cloFailure.Error == AhciReadDmaExtExecutionError::None,
                   (cloReceipt.InitialPortCommand & CommandListOverrideMask) != 0U,
                   (cloReceipt.RestoredPortCommand & CommandListOverrideMask) == 0U,
                   EveryPortCommandWritePreservesConfiguration(cloMmio, CommandConfiguration));

    struct IccCase
    {
        UInt32 Value;
        const char* Description;
    };
    constexpr IccCase iccCases[]{
        {1U, "every PxCMD write masks an Active ICC request"},
        {2U, "every PxCMD write masks a Partial ICC request"},
        {6U, "every PxCMD write masks a Slumber ICC request"},
        {8U, "every PxCMD write masks a DevSleep ICC request"},
    };
    for (const IccCase& iccCase : iccCases)
    {
        const UInt32 transientIccCommand = InitialCommand | (iccCase.Value << 28U);
        PrepareSentinels();
        FakeAhciMmio iccMmio{g_commandPage};
        iccMmio.SetRegister(PortCommandAddress, transientIccCommand);
        FakeClock iccClock;
        AhciReadDmaExtCommandExecutor iccExecutor{iccMmio, iccMmio, iccClock};
        AhciReadDmaExtExecutionReceipt iccReceipt{};
        const AhciReadDmaExtExecutionFailure iccFailure = iccExecutor.Execute(MakeRequest(), iccReceipt);
        ASTERIA_EXPECT(runner, iccCase.Description, iccFailure.Error == AhciReadDmaExtExecutionError::None,
                       iccReceipt.InitialPortCommand == transientIccCommand,
                       iccReceipt.RestoredPortCommand == InitialCommand,
                       iccMmio.GetRegister(PortCommandAddress) == InitialCommand,
                       EveryPortCommandWritePreservesConfiguration(iccMmio, CommandConfiguration));
    }

    constexpr UInt32 stoppedActionCommand{CommandConfiguration | CommandListOverrideMask | (8U << 28U)};
    PrepareSentinels();
    FakeAhciMmio stoppedMmio{g_commandPage};
    stoppedMmio.SetRegister(PortCommandAddress, stoppedActionCommand);
    FakeClock stoppedClock;
    AhciReadDmaExtCommandExecutor stoppedExecutor{stoppedMmio, stoppedMmio, stoppedClock};
    AhciReadDmaExtExecutionReceipt stoppedReceipt{};
    const AhciReadDmaExtExecutionFailure stoppedFailure = stoppedExecutor.Execute(MakeRequest(), stoppedReceipt);
    ASTERIA_EXPECT(runner, "stopped-engine restoration masks transient CLO and ICC actions",
                   stoppedFailure.Error == AhciReadDmaExtExecutionError::None,
                   stoppedReceipt.InitialPortCommand == stoppedActionCommand,
                   stoppedReceipt.RestoredPortCommand == CommandConfiguration,
                   stoppedMmio.GetRegister(PortCommandAddress) == CommandConfiguration,
                   EveryPortCommandWritePreservesConfiguration(stoppedMmio, CommandConfiguration));
}

void TestDelayedEngineTransitionsRemainBounded(TestContext& runner) noexcept
{
    PrepareSentinels();
    FakeAhciMmio delayed{g_commandPage};
    delayed.SetEngineStopDelays(2U, 1U);
    FakeClock delayedClock;
    AhciReadDmaExtCommandExecutor delayedExecutor{delayed, delayed, delayedClock};
    AhciReadDmaExtExecutionReceipt delayedReceipt{};
    const AhciReadDmaExtExecutionFailure delayedFailure = delayedExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 32U, 32U), delayedReceipt);
    ASTERIA_EXPECT(runner, "CR/FR transitions are polled to completion with explicit bounds",
                   delayedFailure.Error == AhciReadDmaExtExecutionError::None,
                   delayedReceipt.PortDisposition == AhciReadDmaExtPortDisposition::Restored,
                   delayedReceipt.PollCount >= 9U);

    PrepareSentinels();
    FakeAhciMmio stuck{g_commandPage};
    stuck.SetEngineStopDelays(20U, 0U);
    FakeClock stuckClock;
    AhciReadDmaExtCommandExecutor stuckExecutor{stuck, stuck, stuckClock};
    AhciReadDmaExtExecutionReceipt stuckReceipt{};
    const AhciReadDmaExtExecutionFailure stuckFailure = stuckExecutor.Execute(
        MakeRequest(CommandPhysicalAddress, DataPhysicalAddress, AbarLength, 2U, 2U), stuckReceipt);
    ASTERIA_EXPECT(runner, "a stuck PxCMD.CR transition has a distinct bounded stop timeout",
                   stuckFailure.Error == AhciReadDmaExtExecutionError::EngineStopTimedOut,
                   stuckFailure.PortDisposition == AhciReadDmaExtPortDisposition::StopRequested,
                   !stuckFailure.CommandPageMustRemainLive, !stuckFailure.DataBufferMustRemainLive);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciReadDmaExtCommandExecutorTests"};
    Asteria::Test::TestSuccessfulExecutionAndExactTransaction(runner);
    Asteria::Test::TestValidationSelectionAndBusyFailures(runner);
    Asteria::Test::TestAliasingPolicyAndBuilderBoundaries(runner);
    Asteria::Test::TestMmioReadAndValueFailures(runner);
    Asteria::Test::TestCommandFailuresAndTransactionalRestore(runner);
    Asteria::Test::TestCompletionEvidenceOracle(runner);
    Asteria::Test::TestLateTaskFileUpdateAfterCompletion(runner);
    Asteria::Test::TestClockAndAbsoluteDeadlineBoundaries(runner);
    Asteria::Test::TestTimeoutRecoveryRequiresRestartSafeState(runner);
    Asteria::Test::TestUnsafeRecoveryNeverRestartsCommandEngine(runner);
    Asteria::Test::TestRecoveryExhaustionRetainsBothDmaSpans(runner);
    Asteria::Test::TestPortCommandWritesNeverReplayActionFields(runner);
    Asteria::Test::TestDelayedEngineTransitionsRemainBounded(runner);
    return runner.Finish();
}
