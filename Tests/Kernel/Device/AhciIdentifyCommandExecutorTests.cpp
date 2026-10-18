// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "Kernel/Device/AhciIdentifyCommandExecutor.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Device::IMemoryMappedIo;
using Hal::Device::IMemoryMappedIoWriter;
using Hal::Device::MemoryMappedIoReadError;
using Hal::Device::MemoryMappedIoWidth;
using Hal::Device::MemoryMappedIoWriteError;
using Hal::Time::IMonotonicClock;
using Kernel::Device::AhciDmaPageWritableView;
using Kernel::Device::AhciHbaRegisterRange;
using Kernel::Device::AhciIdentifyCommandError;
using Kernel::Device::AhciIdentifyCommandExecutor;
using Kernel::Device::AhciIdentifyCommandFailure;
using Kernel::Device::AhciIdentifyCommandReceipt;
using Kernel::Device::AhciIdentifyCommandRequest;
using Kernel::Device::AhciIdentifyDmaPageBuilder;
using Kernel::Device::AhciIdentifyDmaPageError;
using Kernel::Device::AhciIdentifyPortDisposition;
using Kernel::Device::AhciIdentifyRegister;

constexpr UInt64 AbarBase{0x0010'0000U};
constexpr UInt64 AbarLength{0x1100U};
constexpr UInt64 DmaPhysicalAddress{0x0000'0001'2345'6000ULL};
constexpr UInt32 SelectedPort{2U};
constexpr UInt64 PortBase{AbarBase + 0x100U + (SelectedPort * 0x80U)};
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

alignas(4096) UInt8 g_dmaPage[AhciIdentifyDmaPageBuilder::PageSizeBytes]{};
alignas(4096) UInt8 g_beforePage[AhciIdentifyDmaPageBuilder::PageSizeBytes]{};

struct alignas(4096) RequestBackedDmaPage
{
    AhciIdentifyCommandRequest Request;
    UInt8 Remaining[AhciIdentifyDmaPageBuilder::PageSizeBytes - sizeof(AhciIdentifyCommandRequest)];
};

struct alignas(4096) ReceiptBackedDmaPage
{
    AhciIdentifyCommandReceipt Receipt;
    UInt8 Remaining[AhciIdentifyDmaPageBuilder::PageSizeBytes - sizeof(AhciIdentifyCommandReceipt)];
};

RequestBackedDmaPage g_requestBackedDmaPage{};
ReceiptBackedDmaPage g_receiptBackedDmaPage{};

static_assert(std::is_aggregate_v<AhciIdentifyCommandRequest>);
static_assert(sizeof(AhciIdentifyCommandError) == 1U);
static_assert(sizeof(AhciIdentifyRegister) == 1U);
static_assert(sizeof(AhciIdentifyPortDisposition) == 1U);
static_assert(sizeof(AhciIdentifyCommandRequest) == 64U);
static_assert(alignof(AhciIdentifyCommandRequest) == 8U);
static_assert(sizeof(AhciIdentifyCommandReceipt) == 168U);
static_assert(alignof(AhciIdentifyCommandReceipt) == 8U);
static_assert(sizeof(AhciIdentifyCommandFailure) == 56U);
static_assert(alignof(AhciIdentifyCommandFailure) == 8U);
static_assert(sizeof(RequestBackedDmaPage) == AhciIdentifyDmaPageBuilder::PageSizeBytes);
static_assert(sizeof(ReceiptBackedDmaPage) == AhciIdentifyDmaPageBuilder::PageSizeBytes);

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

[[nodiscard]] bool TextEquals(const char* const left, const char* const right) noexcept
{
    Size index{0U};
    while ((left[index] != '\0') || (right[index] != '\0'))
    {
        if (left[index] != right[index])
        {
            return false;
        }
        ++index;
    }
    return true;
}

void WriteWord(UInt8* const identifyData, const Size wordIndex, const UInt16 value) noexcept
{
    identifyData[wordIndex * 2U] = static_cast<UInt8>(value & 0xFFU);
    identifyData[(wordIndex * 2U) + 1U] = static_cast<UInt8>(value >> 8U);
}

void WriteLittleEndian32(UInt8* const bytes, const UInt32 value) noexcept
{
    for (UInt32 index{0U}; index < 4U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

void WriteAtaString(UInt8* const identifyData, const Size firstWord, const Size wordCount,
                    const char* const value) noexcept
{
    const Size characterCount = wordCount * 2U;
    Size length{0U};
    while ((length < characterCount) && (value[length] != '\0'))
    {
        ++length;
    }
    for (Size index{0U}; index < characterCount; ++index)
    {
        const UInt8 character = (index < length) ? static_cast<UInt8>(value[index]) : static_cast<UInt8>(' ');
        const Size word = firstWord + (index / 2U);
        const Size byte = ((index % 2U) == 0U) ? 1U : 0U;
        identifyData[(word * 2U) + byte] = character;
    }
}

void BuildIdentifyPayload(UInt8* const dmaPage) noexcept
{
    UInt8* const identifyData = dmaPage + AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes;
    WriteAtaString(identifyData, 10U, 10U, "SN123456");
    WriteAtaString(identifyData, 27U, 20U, "ASTERIA IA64 TEST DISK");
    WriteWord(identifyData, 49U, UInt16{1U} << 9U);
    WriteWord(identifyData, 83U, 0x4000U | (UInt16{1U} << 10U));
    constexpr UInt64 sectorCount{0x0000'0001'2345'6789ULL};
    for (Size index{0U}; index < 4U; ++index)
    {
        WriteWord(identifyData, 100U + index, static_cast<UInt16>(sectorCount >> (index * 16U)));
    }
    WriteWord(identifyData, 106U, 0x7002U);
    WriteWord(identifyData, 117U, 2048U);
    WriteWord(identifyData, 118U, 0U);
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

enum class IdentifyMutation : UInt8
{
    None,
    InvalidLba48HighWord,
    InvalidLba48Validity,
    Lba28Boundary,
    Lba28FirstInvalid,
    Lba28Zero,
    InvalidLogicalSectorSize,
    CapacityOverflow,
    EmbeddedNullSerial,
    EmbeddedNullModel,
};

class FakeClock final : public IMonotonicClock
{
public:
    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        const UInt64 result = m_nextTicks;
        m_nextTicks += m_step;
        ++m_readCount;
        return result;
    }

    void Set(const UInt64 nextTicks, const UInt64 step) noexcept
    {
        m_nextTicks = nextTicks;
        m_step = step;
        m_readCount = 0U;
    }

    [[nodiscard]] Size GetReadCount() const noexcept
    {
        return m_readCount;
    }

private:
    mutable UInt64 m_nextTicks{1U};
    UInt64 m_step{1U};
    mutable Size m_readCount{0U};
};

class FakeAhciMmio final : public IMemoryMappedIo, public IMemoryMappedIoWriter
{
public:
    explicit FakeAhciMmio(UInt8* const dmaPage) noexcept : m_dmaPage{dmaPage}
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
        m_completionPrdbc = 512U;
        m_completionFisType = 0x34U;
        m_completionFisStatus = 0x50U;
        m_completionFisError = 0U;
        m_identifyMutation = IdentifyMutation::None;
        m_commandStopDelayReads = 0U;
        m_fisStopDelayReads = 0U;
        m_recoveryCommandStopDelayReads = 0U;
        m_recoveryFisStopDelayReads = 0U;
        m_commandStopReadsRemaining = 0U;
        m_fisStopReadsRemaining = 0U;
        m_failedWriteAddress = 0U;
        m_failWrite = false;
        m_applyFailedWrite = false;
        m_injectLateFatalOnClearCommandIssue = false;
        m_failedReadAfterIssueAddress = 0U;
        m_failReadsAfterIssue = false;
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
        SetPort(SelectedPort, 0x0103U, 0x0000'0101U);
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

    void SetPort(const UInt32 port, const UInt32 sataStatus, const UInt32 signature) noexcept
    {
        const UInt64 portBase = AbarBase + 0x100U + (port * 0x80U);
        SetRegister(portBase + 0x28U, sataStatus);
        SetRegister(portBase + 0x24U, signature);
    }

    void ConfigureCompletion(const bool completeCommand, const Size completionAfterPolls, const UInt32 interruptStatus,
                             const UInt32 taskFileData) noexcept
    {
        m_completeCommand = completeCommand;
        m_completionAfterPolls = completionAfterPolls;
        m_completionInterruptStatus = interruptStatus;
        m_completionTaskFileData = taskFileData;
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

    void ConfigureDmaCompletion(const UInt32 prdbc, const UInt8 fisType, const UInt8 fisStatus,
                                const UInt8 fisError) noexcept
    {
        m_completionPrdbc = prdbc;
        m_completionFisType = fisType;
        m_completionFisStatus = fisStatus;
        m_completionFisError = fisError;
    }

    void SetIdentifyMutation(const IdentifyMutation mutation) noexcept
    {
        m_identifyMutation = mutation;
    }

    void FailWriteAt(const UInt64 address) noexcept
    {
        m_failedWriteAddress = address;
        m_failWrite = true;
        m_applyFailedWrite = false;
    }

    void ApplyThenFailWriteAt(const UInt64 address) noexcept
    {
        m_failedWriteAddress = address;
        m_failWrite = true;
        m_applyFailedWrite = true;
    }

    void InjectLateFatalOnClearCommandIssue() noexcept
    {
        m_injectLateFatalOnClearCommandIssue = true;
    }

    void FailReadsAfterCommandIssueAt(const UInt64 address) noexcept
    {
        m_failedReadAfterIssueAddress = address;
        m_failReadsAfterIssue = true;
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
        if (m_failReadsAfterIssue && m_commandWasIssued && (physicalAddress == m_failedReadAfterIssueAddress))
        {
            return MemoryMappedIoReadError::HardwareFailure;
        }
        if ((physicalAddress == CommandIssueAddress) && m_injectLateFatalOnClearCommandIssue && m_commandWasIssued &&
            ((GetRegister(CommandIssueAddress) & 1U) == 0U))
        {
            SetRegister(PortInterruptStatusAddress, GetRegister(PortInterruptStatusAddress) | 0x4000'0000U);
            m_injectLateFatalOnClearCommandIssue = false;
        }
        UInt32 value = GetRegister(physicalAddress);
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

        BuildIdentifyPayload(m_dmaPage);
        UInt8* const identifyData = m_dmaPage + AhciIdentifyDmaPageBuilder::IdentifyDataOffsetBytes;
        switch (m_identifyMutation)
        {
        case IdentifyMutation::None:
            break;
        case IdentifyMutation::InvalidLba48HighWord:
            WriteWord(identifyData, 103U, 1U);
            break;
        case IdentifyMutation::InvalidLba48Validity:
            WriteWord(identifyData, 83U, UInt16{1U} << 10U);
            WriteWord(identifyData, 60U, 0x5678U);
            WriteWord(identifyData, 61U, 0x0234U);
            break;
        case IdentifyMutation::Lba28Boundary:
            WriteWord(identifyData, 83U, 0U);
            WriteWord(identifyData, 60U, 0xFFFFU);
            WriteWord(identifyData, 61U, 0x0FFFU);
            break;
        case IdentifyMutation::Lba28FirstInvalid:
            WriteWord(identifyData, 83U, 0U);
            WriteWord(identifyData, 60U, 0U);
            WriteWord(identifyData, 61U, 0x1000U);
            break;
        case IdentifyMutation::Lba28Zero:
            WriteWord(identifyData, 83U, 0U);
            WriteWord(identifyData, 60U, 0U);
            WriteWord(identifyData, 61U, 0U);
            break;
        case IdentifyMutation::InvalidLogicalSectorSize:
            WriteWord(identifyData, 106U, 0x5000U);
            WriteWord(identifyData, 117U, 128U);
            WriteWord(identifyData, 118U, 0U);
            break;
        case IdentifyMutation::CapacityOverflow:
            WriteWord(identifyData, 100U, 0xFFFFU);
            WriteWord(identifyData, 101U, 0xFFFFU);
            WriteWord(identifyData, 102U, 0xFFFFU);
            WriteWord(identifyData, 103U, 0U);
            WriteWord(identifyData, 106U, 0x5000U);
            WriteWord(identifyData, 117U, 0xFFFFU);
            WriteWord(identifyData, 118U, 0xFFFFU);
            break;
        case IdentifyMutation::EmbeddedNullSerial:
            identifyData[(11U * 2U) + 1U] = 0U;
            break;
        case IdentifyMutation::EmbeddedNullModel:
            identifyData[(30U * 2U) + 0U] = 0U;
            break;
        }
        WriteLittleEndian32(m_dmaPage + 4U, m_completionPrdbc);
        UInt8* const receivedFis = m_dmaPage + AhciIdentifyDmaPageBuilder::ReceivedFisOffsetBytes;
        if (m_completionFisType == 0x5FU)
        {
            // A PIO Setup FIS, as real drives end PIO data-in commands:
            // the initial status at byte 2, E_Status (final) at byte 15.
            receivedFis[0x20U] = 0x5FU;
            receivedFis[0x20U + 2U] = 0x58U;
            receivedFis[0x20U + 3U] = m_completionFisError;
            receivedFis[0x20U + 15U] = m_completionFisStatus;
        }
        else
        {
            UInt8* const deviceToHostFis = receivedFis + 0x40U;
            deviceToHostFis[0U] = m_completionFisType;
            deviceToHostFis[2U] = m_completionFisStatus;
            deviceToHostFis[3U] = m_completionFisError;
        }
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

    UInt8* m_dmaPage;
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
    UInt32 m_completionPrdbc{512U};
    UInt8 m_completionFisType{0x34U};
    UInt8 m_completionFisStatus{0x50U};
    UInt8 m_completionFisError{0U};
    IdentifyMutation m_identifyMutation{IdentifyMutation::None};
    Size m_commandStopDelayReads{0U};
    Size m_fisStopDelayReads{0U};
    Size m_recoveryCommandStopDelayReads{0U};
    Size m_recoveryFisStopDelayReads{0U};
    Size m_commandStopReadsRemaining{0U};
    Size m_fisStopReadsRemaining{0U};
    UInt64 m_failedWriteAddress{0U};
    bool m_failWrite{false};
    bool m_applyFailedWrite{false};
    bool m_injectLateFatalOnClearCommandIssue{false};
    UInt64 m_failedReadAfterIssueAddress{0U};
    bool m_failReadsAfterIssue{false};
    bool m_clearCommandIssueOnStop{true};
    UInt32 m_taskFileDataOnCommandStop{0U};
    UInt32 m_sataActiveOnCommandStop{0U};
    UInt32 m_sataStatusOnCommandStop{0x0103U};
    UInt32 m_taskFileDataOnOriginalBaseRestore{0U};
    bool m_changeTaskFileDataOnOriginalBaseRestore{false};
};

[[nodiscard]] AhciIdentifyCommandRequest MakeRequest(const UInt64 dmaPhysicalAddress = DmaPhysicalAddress,
                                                     const UInt64 abarLength = AbarLength,
                                                     const Size maximumPollCount = 32U,
                                                     const Size maximumRecoveryPollCount = 32U) noexcept
{
    return AhciIdentifyCommandRequest{
        .Abar =
            AhciHbaRegisterRange{
                .BaseAddress = AbarBase,
                .LengthBytes = abarLength,
            },
        .DmaPage =
            AhciDmaPageWritableView{
                .Data = g_dmaPage,
                .SizeBytes = AhciIdentifyDmaPageBuilder::PageSizeBytes,
                .PhysicalAddress = dmaPhysicalAddress,
            },
        .Polling =
            {
                .DeadlineTicks = 1'000U,
                .MaximumPollCount = maximumPollCount,
                .MaximumRecoveryPollCount = maximumRecoveryPollCount,
            },
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

[[nodiscard]] bool ReadPrefixIsExact(const FakeAhciMmio& mmio) noexcept
{
    constexpr UInt64 expected[14U]{
        CapabilitiesAddress,  GlobalHostControlAddress,   PortsImplementedAddress,     SataStatusAddress,
        PortSignatureAddress, PortCommandAddress,         TaskFileDataAddress,         SataActiveAddress,
        CommandIssueAddress,  CommandListBaseAddress,     CommandListBaseUpperAddress, FisBaseAddress,
        FisBaseUpperAddress,  PortInterruptEnableAddress,
    };
    Size readIndex{0U};
    for (Size index{0U}; index < mmio.GetAccessCount(); ++index)
    {
        const Access& access = mmio.GetAccess(index);
        if (access.IsWrite)
        {
            continue;
        }
        if ((readIndex >= 14U) || (access.Address != expected[readIndex]))
        {
            return readIndex >= 14U;
        }
        ++readIndex;
        if (readIndex == 14U)
        {
            return true;
        }
    }
    return false;
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

void TestSuccessfulExecutionAndExactTransaction(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio mmio{g_dmaPage};
    FakeClock clock;
    AhciIdentifyCommandExecutor executor{mmio, mmio, clock};
    AhciIdentifyCommandReceipt receipt{};
    const auto failure = executor.Execute(MakeRequest(), receipt);
    ASTERIA_EXPECT(runner, "IDENTIFY completes on one active ATA disk",
                   failure.Error == AhciIdentifyCommandError::None);
    if (failure.Error != AhciIdentifyCommandError::None)
    {
        return;
    }

    ASTERIA_EXPECT(runner, "success receipt identifies the selected port and restored state",
                   receipt.PortNumber == SelectedPort, receipt.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   receipt.PortSignature == 0x0000'0101U);
    ASTERIA_EXPECT(runner, "ATA word-swapped serial and model fields are parsed and trimmed",
                   TextEquals(receipt.Device.SerialNumber, "SN123456"), receipt.Device.SerialNumberLength == 8U,
                   TextEquals(receipt.Device.ModelNumber, "ASTERIA IA64 TEST DISK"),
                   receipt.Device.ModelNumberLength == 22U);
    ASTERIA_EXPECT(runner, "LBA48 capacity and logical/physical sector sizes are parsed",
                   receipt.Device.Uses48BitAddressing, receipt.Device.SectorCount == 0x0000'0001'2345'6789ULL,
                   receipt.Device.CapacityBytes == (0x0000'0001'2345'6789ULL * 4096U),
                   receipt.Device.LogicalSectorSizeBytes == 4096U, receipt.Device.PhysicalSectorSizeBytes == 16'384U,
                   receipt.DeviceToHostFisStatus == 0x50U, receipt.DeviceToHostFisError == 0U);
    ASTERIA_EXPECT(runner, "executor and DMA builder preserve both unused page gaps",
                   RegionHasValue(g_dmaPage, 0x590U, 0x70U, Sentinel),
                   RegionHasValue(g_dmaPage, 0x800U, 0x800U, Sentinel));
    ASTERIA_EXPECT(runner, "success restores original DMA bases, interrupt enable, and engine",
                   mmio.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   mmio.GetRegister(CommandListBaseUpperAddress) == InitialCommandListBaseUpper,
                   mmio.GetRegister(FisBaseAddress) == InitialFisBase,
                   mmio.GetRegister(FisBaseUpperAddress) == InitialFisBaseUpper,
                   mmio.GetRegister(PortInterruptEnableAddress) == InitialInterruptEnable,
                   (mmio.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) ==
                       (CommandStartMask | FisReceiveEnableMask));
    ASTERIA_EXPECT(runner, "W1C writes clear observed selected-port status without collateral bits",
                   mmio.GetRegister(PortInterruptStatusAddress) == 0U, mmio.GetRegister(SataErrorAddress) == 0U,
                   mmio.GetRegister(GlobalInterruptStatusAddress) == 0x80U);
    ASTERIA_EXPECT(runner, "selection and preflight registers are read in the exact contract order",
                   ReadPrefixIsExact(mmio));

    constexpr UInt32 dmaLow = static_cast<UInt32>(DmaPhysicalAddress & 0xFFFF'FFFFULL);
    constexpr UInt32 dmaHigh = static_cast<UInt32>(DmaPhysicalAddress >> 32U);
    constexpr ExpectedWrite expected[25U]{
        {PortCommandAddress, CommandConfiguration | FisReceiveEnableMask},
        {PortCommandAddress, CommandConfiguration},
        {PortInterruptEnableAddress, 0U},
        {CommandListBaseAddress, dmaLow},
        {CommandListBaseUpperAddress, dmaHigh},
        {FisBaseAddress, dmaLow + 0x400U},
        {FisBaseUpperAddress, dmaHigh},
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
    ASTERIA_EXPECT(runner, "all success-path register writes have exact order and values", writeCount == 25U,
                   WritesEqual(actual, expected, 25U));
}

void CheckSelectionFailure(TestContext& runner, FakeAhciMmio& mmio, FakeClock& clock,
                           const AhciIdentifyCommandError expected, const char* const description) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    CopyBytes(g_beforePage, g_dmaPage, sizeof(g_dmaPage));
    AhciIdentifyCommandExecutor executor{mmio, mmio, clock};
    AhciIdentifyCommandReceipt receipt{};
    receipt.Device.SectorCount = 0xA5A5'A5A5'A5A5'A5A5ULL;
    receipt.PortNumber = 0x5AU;
    const auto failure = executor.Execute(MakeRequest(), receipt);
    ASTERIA_EXPECT(runner, description, failure.Error == expected,
                   failure.PortDisposition == AhciIdentifyPortDisposition::Unchanged,
                   receipt.Device.SectorCount == 0xA5A5'A5A5'A5A5'A5A5ULL, receipt.PortNumber == 0x5AU,
                   BytesEqual(g_dmaPage, g_beforePage, sizeof(g_dmaPage)));
}

void TestPortSelectionFailuresAreAtomic(TestContext& runner) noexcept
{
    FakeClock clock;

    FakeAhciMmio noPort{g_dmaPage};
    noPort.SetRegister(PortsImplementedAddress, 0U);
    CheckSelectionFailure(runner, noPort, clock, AhciIdentifyCommandError::NoActiveDiskPort,
                          "no PI-advertised disk fails without modifying DMA output");

    FakeAhciMmio inactive{g_dmaPage};
    inactive.SetRegister(SataStatusAddress, 0U);
    CheckSelectionFailure(runner, inactive, clock, AhciIdentifyCommandError::NoActiveDiskPort,
                          "an inactive PI port is not selected and leaves output atomic");

    FakeAhciMmio ambiguous{g_dmaPage};
    ambiguous.SetRegister(CapabilitiesAddress, Supports64BitAddressingMask | 3U);
    ambiguous.SetRegister(PortsImplementedAddress, (UInt32{1U} << 1U) | (UInt32{1U} << SelectedPort));
    ambiguous.SetPort(1U, 0x0103U, 0x0000'0101U);
    CheckSelectionFailure(runner, ambiguous, clock, AhciIdentifyCommandError::AmbiguousActiveDiskPorts,
                          "multiple active ATA disks are rejected without selection accident");

    FakeAhciMmio mixedAtapi{g_dmaPage};
    mixedAtapi.SetRegister(CapabilitiesAddress, Supports64BitAddressingMask | 3U);
    mixedAtapi.SetRegister(PortsImplementedAddress, (UInt32{1U} << 1U) | (UInt32{1U} << SelectedPort));
    mixedAtapi.SetPort(1U, 0x0103U, 0xEB14'0101U);
    CheckSelectionFailure(runner, mixedAtapi, clock, AhciIdentifyCommandError::AtapiDeviceNotSupported,
                          "one ATA disk cannot hide a second active ATAPI device");

    FakeAhciMmio mixedUnknown{g_dmaPage};
    mixedUnknown.SetRegister(CapabilitiesAddress, Supports64BitAddressingMask | 3U);
    mixedUnknown.SetRegister(PortsImplementedAddress, (UInt32{1U} << 1U) | (UInt32{1U} << SelectedPort));
    mixedUnknown.SetPort(1U, 0x0103U, 0x9669'0101U);
    CheckSelectionFailure(runner, mixedUnknown, clock, AhciIdentifyCommandError::UnsupportedDeviceSignature,
                          "one ATA disk cannot hide a second active unknown signature");

    FakeAhciMmio atapi{g_dmaPage};
    atapi.SetRegister(PortSignatureAddress, 0xEB14'0101U);
    CheckSelectionFailure(runner, atapi, clock, AhciIdentifyCommandError::AtapiDeviceNotSupported,
                          "an active ATAPI device receives its distinct typed rejection");

    FakeAhciMmio unsupported{g_dmaPage};
    unsupported.SetRegister(PortSignatureAddress, 0x9669'0101U);
    CheckSelectionFailure(runner, unsupported, clock, AhciIdentifyCommandError::UnsupportedDeviceSignature,
                          "an active unsupported signature is rejected distinctly");
}

void TestDmaPageCannotAliasLiveCallObjects(TestContext& runner) noexcept
{
    UInt8* const requestPage = reinterpret_cast<UInt8*>(&g_requestBackedDmaPage);
    FillBytes(requestPage, AhciIdentifyDmaPageBuilder::PageSizeBytes, Sentinel);
    g_requestBackedDmaPage.Request = MakeRequest();
    g_requestBackedDmaPage.Request.DmaPage.Data = requestPage;
    CopyBytes(g_beforePage, requestPage, AhciIdentifyDmaPageBuilder::PageSizeBytes);
    FakeAhciMmio requestMmio{requestPage};
    FakeClock requestClock;
    AhciIdentifyCommandExecutor requestExecutor{requestMmio, requestMmio, requestClock};
    AhciIdentifyCommandReceipt requestReceipt{};
    const auto requestFailure = requestExecutor.Execute(g_requestBackedDmaPage.Request, requestReceipt);
    ASTERIA_EXPECT(runner, "a request stored in the DMA page is rejected before page mutation",
                   requestFailure.Error == AhciIdentifyCommandError::DmaPageOverlapsRequest,
                   requestFailure.PortDisposition == AhciIdentifyPortDisposition::Unchanged,
                   requestMmio.GetAccessCount() == 0U,
                   BytesEqual(requestPage, g_beforePage, AhciIdentifyDmaPageBuilder::PageSizeBytes));

    UInt8* const receiptPage = reinterpret_cast<UInt8*>(&g_receiptBackedDmaPage);
    FillBytes(receiptPage, AhciIdentifyDmaPageBuilder::PageSizeBytes, Sentinel);
    g_receiptBackedDmaPage.Receipt = AhciIdentifyCommandReceipt{};
    g_receiptBackedDmaPage.Receipt.PortNumber = 0x5AU;
    CopyBytes(g_beforePage, receiptPage, AhciIdentifyDmaPageBuilder::PageSizeBytes);
    FakeAhciMmio receiptMmio{receiptPage};
    FakeClock receiptClock;
    AhciIdentifyCommandExecutor receiptExecutor{receiptMmio, receiptMmio, receiptClock};
    AhciIdentifyCommandRequest receiptRequest = MakeRequest();
    receiptRequest.DmaPage.Data = receiptPage;
    const auto receiptFailure = receiptExecutor.Execute(receiptRequest, g_receiptBackedDmaPage.Receipt);
    ASTERIA_EXPECT(runner, "a receipt overlapping the full DMA page is rejected atomically",
                   receiptFailure.Error == AhciIdentifyCommandError::DmaPageOverlapsReceipt,
                   receiptFailure.PortDisposition == AhciIdentifyPortDisposition::Unchanged,
                   receiptMmio.GetAccessCount() == 0U, g_receiptBackedDmaPage.Receipt.PortNumber == 0x5AU,
                   BytesEqual(receiptPage, g_beforePage, AhciIdentifyDmaPageBuilder::PageSizeBytes));
}

void TestRangeAddressWidthAndBusyFailures(TestContext& runner) noexcept
{
    FakeClock clock;

    FakeAhciMmio invalidPi{g_dmaPage};
    invalidPi.SetRegister(CapabilitiesAddress, Supports64BitAddressingMask);
    CheckSelectionFailure(runner, invalidPi, clock, AhciIdentifyCommandError::PortsImplementedOutOfRange,
                          "PI bits above CAP.NP are rejected before port MMIO");

    FakeAhciMmio shortAbar{g_dmaPage};
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    CopyBytes(g_beforePage, g_dmaPage, sizeof(g_dmaPage));
    AhciIdentifyCommandExecutor shortExecutor{shortAbar, shortAbar, clock};
    AhciIdentifyCommandReceipt shortReceipt{};
    const auto shortFailure = shortExecutor.Execute(MakeRequest(DmaPhysicalAddress, 0x200U), shortReceipt);
    ASTERIA_EXPECT(runner, "ABAR extent is checked against every advertised port register",
                   shortFailure.Error == AhciIdentifyCommandError::InvalidAbarRange,
                   BytesEqual(g_dmaPage, g_beforePage, sizeof(g_dmaPage)));

    FakeAhciMmio noS64a{g_dmaPage};
    noS64a.SetRegister(CapabilitiesAddress, 2U);
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    CopyBytes(g_beforePage, g_dmaPage, sizeof(g_dmaPage));
    AhciIdentifyCommandExecutor noS64aExecutor{noS64a, noS64a, clock};
    AhciIdentifyCommandReceipt noS64aReceipt{};
    const auto noS64aFailure = noS64aExecutor.Execute(MakeRequest(), noS64aReceipt);
    ASTERIA_EXPECT(runner, "CAP.S64A bounds the entire DMA page before mutation or MMIO writes",
                   noS64aFailure.Error == AhciIdentifyCommandError::DmaPageRejected,
                   noS64aFailure.DmaPage == AhciIdentifyDmaPageError::ControllerAddressWidthExceeded,
                   BytesEqual(g_dmaPage, g_beforePage, sizeof(g_dmaPage)));

    FakeAhciMmio busy{g_dmaPage};
    busy.SetRegister(TaskFileDataAddress, 0x80U);
    CheckSelectionFailure(runner, busy, clock, AhciIdentifyCommandError::PortBusy,
                          "PxTFD.BSY rejects the port before DMA or engine mutation");
}

void TestCommandErrorsAndTransactionalRestore(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio taskFile{g_dmaPage};
    taskFile.ConfigureCompletion(true, 1U, 0U, 1U);
    FakeClock taskClock;
    AhciIdentifyCommandExecutor taskExecutor{taskFile, taskFile, taskClock};
    AhciIdentifyCommandReceipt taskReceipt{};
    const auto taskFailure = taskExecutor.Execute(MakeRequest(), taskReceipt);
    ASTERIA_EXPECT(runner, "PxTFD error is reported and a quiescent command is restored",
                   taskFailure.Error == AhciIdentifyCommandError::TaskFileError, taskFailure.TaskFileData == 1U,
                   taskFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   !taskFailure.DmaPageMustRemainLive,
                   taskFile.GetRegister(CommandListBaseAddress) == InitialCommandListBase);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio interrupt{g_dmaPage};
    interrupt.ConfigureCompletion(true, 1U, 0x4000'0000U, 0U);
    FakeClock interruptClock;
    AhciIdentifyCommandExecutor interruptExecutor{interrupt, interrupt, interruptClock};
    AhciIdentifyCommandReceipt interruptReceipt{};
    const auto interruptFailure = interruptExecutor.Execute(MakeRequest(), interruptReceipt);
    ASTERIA_EXPECT(runner, "fatal PxIS status is diagnosed and restored transactionally",
                   interruptFailure.Error == AhciIdentifyCommandError::PortInterruptError,
                   interruptFailure.PortInterruptStatus == 0x4000'0000U,
                   interruptFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   !interruptFailure.DmaPageMustRemainLive);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio lateFatal{g_dmaPage};
    lateFatal.ConfigureCompletion(true, 1U, 1U, 0U);
    lateFatal.InjectLateFatalOnClearCommandIssue();
    FakeClock lateFatalClock;
    AhciIdentifyCommandExecutor lateFatalExecutor{lateFatal, lateFatal, lateFatalClock};
    AhciIdentifyCommandReceipt lateFatalReceipt{};
    lateFatalReceipt.PortNumber = 0x5AU;
    const auto lateFatalFailure = lateFatalExecutor.Execute(MakeRequest(), lateFatalReceipt);
    ASTERIA_EXPECT(runner, "a fatal PxIS arriving at the CI-clear read is caught by fresh snapshots",
                   lateFatalFailure.Error == AhciIdentifyCommandError::PortInterruptError,
                   (lateFatalFailure.PortInterruptStatus & 0x4000'0000U) != 0U,
                   lateFatalFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   !lateFatalFailure.DmaPageMustRemainLive, lateFatalReceipt.PortNumber == 0x5AU,
                   HasFreshFinalStatusSequence(lateFatal));

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio failedWrite{g_dmaPage};
    failedWrite.FailWriteAt(CommandListBaseUpperAddress);
    FakeClock writeClock;
    AhciIdentifyCommandExecutor writeExecutor{failedWrite, failedWrite, writeClock};
    AhciIdentifyCommandReceipt writeReceipt{};
    const auto writeFailure = writeExecutor.Execute(MakeRequest(), writeReceipt);
    ASTERIA_EXPECT(runner, "a programming write failure restores every already-touched register",
                   writeFailure.Error == AhciIdentifyCommandError::MemoryMappedIoWriteFailed,
                   writeFailure.Register == AhciIdentifyRegister::PortCommandListBaseUpper,
                   writeFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   !writeFailure.DmaPageMustRemainLive,
                   failedWrite.GetRegister(CommandListBaseAddress) == InitialCommandListBase);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio appliedIssue{g_dmaPage};
    appliedIssue.ConfigureCompletion(false, 1U, 0U, 0U);
    appliedIssue.ApplyThenFailWriteAt(CommandIssueAddress);
    FakeClock appliedIssueClock;
    AhciIdentifyCommandExecutor appliedIssueExecutor{appliedIssue, appliedIssue, appliedIssueClock};
    AhciIdentifyCommandReceipt appliedIssueReceipt{};
    appliedIssueReceipt.PortNumber = 0x5AU;
    const auto appliedIssueFailure = appliedIssueExecutor.Execute(MakeRequest(), appliedIssueReceipt);
    ASTERIA_EXPECT(runner, "a compliant engine stop retires an applied-but-error PxCI write safely",
                   appliedIssueFailure.Error == AhciIdentifyCommandError::MemoryMappedIoWriteFailed,
                   appliedIssueFailure.Register == AhciIdentifyRegister::PortCommandIssue,
                   appliedIssueFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   !appliedIssueFailure.DmaPageMustRemainLive, appliedIssueFailure.CommandIssue == 0U,
                   HasCommandIssueReadAfterIssueWrite(appliedIssue, 0U), HasRestartSafetySequence(appliedIssue),
                   appliedIssue.GetRegister(CommandIssueAddress) == 0U,
                   appliedIssue.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   (appliedIssue.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) ==
                       (CommandStartMask | FisReceiveEnableMask),
                   appliedIssueReceipt.PortNumber == 0x5AU);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio uncertainRead{g_dmaPage};
    uncertainRead.ConfigureCompletion(false, 1U, 0U, 0U);
    uncertainRead.FailReadsAfterCommandIssueAt(CommandIssueAddress);
    FakeClock uncertainReadClock;
    AhciIdentifyCommandExecutor uncertainReadExecutor{uncertainRead, uncertainRead, uncertainReadClock};
    AhciIdentifyCommandReceipt uncertainReadReceipt{};
    uncertainReadReceipt.PortNumber = 0x5AU;
    const auto uncertainReadFailure = uncertainReadExecutor.Execute(MakeRequest(), uncertainReadReceipt);
    ASTERIA_EXPECT(runner, "an unreadable retired PxCI conservatively retains the DMA page",
                   uncertainReadFailure.Error == AhciIdentifyCommandError::MemoryMappedIoReadFailed,
                   uncertainReadFailure.Register == AhciIdentifyRegister::PortCommandIssue,
                   uncertainReadFailure.RecoveryError == AhciIdentifyCommandError::MemoryMappedIoReadFailed,
                   uncertainReadFailure.RecoveryRegister == AhciIdentifyRegister::PortCommandIssue,
                   uncertainReadFailure.PortDisposition == AhciIdentifyPortDisposition::Uncertain,
                   uncertainReadFailure.DmaPageMustRemainLive, uncertainRead.GetRegister(CommandIssueAddress) == 0U,
                   uncertainRead.GetRegister(CommandListBaseAddress) ==
                       static_cast<UInt32>(DmaPhysicalAddress & 0xFFFF'FFFFULL),
                   uncertainReadReceipt.PortNumber == 0x5AU);
}

void TestCompletionEvidenceAndIdentifyValidity(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio partial{g_dmaPage};
    partial.ConfigureDmaCompletion(256U, 0x34U, 0x50U, 0U);
    FakeClock partialClock;
    AhciIdentifyCommandExecutor partialExecutor{partial, partial, partialClock};
    AhciIdentifyCommandReceipt partialReceipt{};
    partialReceipt.PortNumber = 0x5AU;
    const auto partialFailure = partialExecutor.Execute(MakeRequest(), partialReceipt);
    ASTERIA_EXPECT(runner, "CI clear cannot publish a partial PRDBC transfer or receipt",
                   partialFailure.Error == AhciIdentifyCommandError::IncompleteDataTransfer,
                   partialFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   partialReceipt.PortNumber == 0x5AU);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio invalidFis{g_dmaPage};
    invalidFis.ConfigureDmaCompletion(512U, 0x00U, 0x50U, 0U);
    FakeClock fisClock;
    AhciIdentifyCommandExecutor fisExecutor{invalidFis, invalidFis, fisClock};
    AhciIdentifyCommandReceipt fisReceipt{};
    const auto fisFailure = fisExecutor.Execute(MakeRequest(), fisReceipt);
    ASTERIA_EXPECT(runner, "CI clear requires a register D2H or PIO Setup completion FIS",
                   fisFailure.Error == AhciIdentifyCommandError::InvalidCompletionFis,
                   fisFailure.PortDisposition == AhciIdentifyPortDisposition::Restored);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio pioSetup{g_dmaPage};
    pioSetup.ConfigureDmaCompletion(512U, 0x5FU, 0x50U, 0U);
    FakeClock pioClock;
    AhciIdentifyCommandExecutor pioExecutor{pioSetup, pioSetup, pioClock};
    AhciIdentifyCommandReceipt pioReceipt{};
    const auto pioFailure = pioExecutor.Execute(MakeRequest(), pioReceipt);
    ASTERIA_EXPECT(runner, "a PIO Setup completion FIS without a register D2H FIS is accepted by its ending status",
                   pioFailure.Error == AhciIdentifyCommandError::None,
                   pioReceipt.PortDisposition == AhciIdentifyPortDisposition::Restored);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio pioError{g_dmaPage};
    pioError.ConfigureDmaCompletion(512U, 0x5FU, 0x51U, 0x04U);
    FakeClock pioErrorClock;
    AhciIdentifyCommandExecutor pioErrorExecutor{pioError, pioError, pioErrorClock};
    AhciIdentifyCommandReceipt pioErrorReceipt{};
    const auto pioErrorFailure = pioErrorExecutor.Execute(MakeRequest(), pioErrorReceipt);
    ASTERIA_EXPECT(runner, "a PIO Setup ending status with ERR rejects the command",
                   pioErrorFailure.Error == AhciIdentifyCommandError::TaskFileError,
                   pioErrorFailure.TaskFileData == 0x451U);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio d2hError{g_dmaPage};
    d2hError.ConfigureDmaCompletion(512U, 0x34U, 0x51U, 0x04U);
    FakeClock d2hClock;
    AhciIdentifyCommandExecutor d2hExecutor{d2hError, d2hError, d2hClock};
    AhciIdentifyCommandReceipt d2hReceipt{};
    const auto d2hFailure = d2hExecutor.Execute(MakeRequest(), d2hReceipt);
    ASTERIA_EXPECT(runner, "D2H status and error bytes independently reject ATA failure",
                   d2hFailure.Error == AhciIdentifyCommandError::TaskFileError, d2hFailure.TaskFileData == 0x451U,
                   d2hFailure.PortDisposition == AhciIdentifyPortDisposition::Restored);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio invalidLba{g_dmaPage};
    invalidLba.SetIdentifyMutation(IdentifyMutation::InvalidLba48HighWord);
    FakeClock invalidLbaClock;
    AhciIdentifyCommandExecutor invalidLbaExecutor{invalidLba, invalidLba, invalidLbaClock};
    AhciIdentifyCommandReceipt invalidLbaReceipt{};
    const auto invalidLbaFailure = invalidLbaExecutor.Execute(MakeRequest(), invalidLbaReceipt);
    ASTERIA_EXPECT(runner, "nonzero bits above the ATA LBA48 count are rejected",
                   invalidLbaFailure.Error == AhciIdentifyCommandError::InvalidIdentifyData);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio invalidSector{g_dmaPage};
    invalidSector.SetIdentifyMutation(IdentifyMutation::InvalidLogicalSectorSize);
    FakeClock invalidSectorClock;
    AhciIdentifyCommandExecutor invalidSectorExecutor{invalidSector, invalidSector, invalidSectorClock};
    AhciIdentifyCommandReceipt invalidSectorReceipt{};
    const auto invalidSectorFailure = invalidSectorExecutor.Execute(MakeRequest(), invalidSectorReceipt);
    ASTERIA_EXPECT(runner, "word 106 cannot advertise a long logical sector of 256 words or less",
                   invalidSectorFailure.Error == AhciIdentifyCommandError::InvalidIdentifyData);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio overflow{g_dmaPage};
    overflow.SetIdentifyMutation(IdentifyMutation::CapacityOverflow);
    FakeClock overflowClock;
    AhciIdentifyCommandExecutor overflowExecutor{overflow, overflow, overflowClock};
    AhciIdentifyCommandReceipt overflowReceipt{};
    const auto overflowFailure = overflowExecutor.Execute(MakeRequest(), overflowReceipt);
    ASTERIA_EXPECT(runner, "sector-count and logical-size multiplication is overflow checked",
                   overflowFailure.Error == AhciIdentifyCommandError::IdentifyCapacityOverflow);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio invalidValidity{g_dmaPage};
    invalidValidity.SetIdentifyMutation(IdentifyMutation::InvalidLba48Validity);
    FakeClock validityClock;
    AhciIdentifyCommandExecutor validityExecutor{invalidValidity, invalidValidity, validityClock};
    AhciIdentifyCommandReceipt validityReceipt{};
    const auto validityFailure = validityExecutor.Execute(MakeRequest(), validityReceipt);
    ASTERIA_EXPECT(runner, "invalid word-83 validity falls back to supported LBA28 capacity",
                   validityFailure.Error == AhciIdentifyCommandError::None, !validityReceipt.Device.Uses48BitAddressing,
                   validityReceipt.Device.SectorCount == 0x0234'5678ULL);
}

void TestLba28BoundsAndEmbeddedNullStrings(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio boundary{g_dmaPage};
    boundary.SetIdentifyMutation(IdentifyMutation::Lba28Boundary);
    FakeClock boundaryClock;
    AhciIdentifyCommandExecutor boundaryExecutor{boundary, boundary, boundaryClock};
    AhciIdentifyCommandReceipt boundaryReceipt{};
    const auto boundaryFailure = boundaryExecutor.Execute(MakeRequest(), boundaryReceipt);
    ASTERIA_EXPECT(runner, "the exact ATA LBA28 sector-count ceiling is accepted",
                   boundaryFailure.Error == AhciIdentifyCommandError::None, !boundaryReceipt.Device.Uses48BitAddressing,
                   boundaryReceipt.Device.SectorCount == 0x0FFF'FFFFULL);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio firstInvalid{g_dmaPage};
    firstInvalid.SetIdentifyMutation(IdentifyMutation::Lba28FirstInvalid);
    FakeClock firstInvalidClock;
    AhciIdentifyCommandExecutor firstInvalidExecutor{firstInvalid, firstInvalid, firstInvalidClock};
    AhciIdentifyCommandReceipt firstInvalidReceipt{};
    const auto firstInvalidFailure = firstInvalidExecutor.Execute(MakeRequest(), firstInvalidReceipt);
    ASTERIA_EXPECT(runner, "the first sector count above the LBA28 ceiling is rejected",
                   firstInvalidFailure.Error == AhciIdentifyCommandError::InvalidIdentifyData);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio zero{g_dmaPage};
    zero.SetIdentifyMutation(IdentifyMutation::Lba28Zero);
    FakeClock zeroClock;
    AhciIdentifyCommandExecutor zeroExecutor{zero, zero, zeroClock};
    AhciIdentifyCommandReceipt zeroReceipt{};
    const auto zeroFailure = zeroExecutor.Execute(MakeRequest(), zeroReceipt);
    ASTERIA_EXPECT(runner, "LBA support with no user-addressable sector is invalid ATA data",
                   zeroFailure.Error == AhciIdentifyCommandError::InvalidIdentifyData);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio serialNull{g_dmaPage};
    serialNull.SetIdentifyMutation(IdentifyMutation::EmbeddedNullSerial);
    FakeClock serialNullClock;
    AhciIdentifyCommandExecutor serialNullExecutor{serialNull, serialNull, serialNullClock};
    AhciIdentifyCommandReceipt serialNullReceipt{};
    const auto serialNullFailure = serialNullExecutor.Execute(MakeRequest(), serialNullReceipt);
    ASTERIA_EXPECT(runner, "an embedded serial-number NUL truncates the published ATA string",
                   serialNullFailure.Error == AhciIdentifyCommandError::None,
                   TextEquals(serialNullReceipt.Device.SerialNumber, "SN"),
                   serialNullReceipt.Device.SerialNumberLength == 2U,
                   serialNullReceipt.Device.SerialNumber[2U] == '\0');

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio modelNull{g_dmaPage};
    modelNull.SetIdentifyMutation(IdentifyMutation::EmbeddedNullModel);
    FakeClock modelNullClock;
    AhciIdentifyCommandExecutor modelNullExecutor{modelNull, modelNull, modelNullClock};
    AhciIdentifyCommandReceipt modelNullReceipt{};
    const auto modelNullFailure = modelNullExecutor.Execute(MakeRequest(), modelNullReceipt);
    ASTERIA_EXPECT(runner, "an embedded model-number NUL truncates the published ATA string",
                   modelNullFailure.Error == AhciIdentifyCommandError::None,
                   TextEquals(modelNullReceipt.Device.ModelNumber, "ASTERIA"),
                   modelNullReceipt.Device.ModelNumberLength == 7U, modelNullReceipt.Device.ModelNumber[7U] == '\0');
}

void TestAbsoluteDeadlineCheckpoints(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    CopyBytes(g_beforePage, g_dmaPage, sizeof(g_dmaPage));
    FakeAhciMmio beforeMutation{g_dmaPage};
    FakeClock mutationClock;
    mutationClock.Set(1U, 999U);
    AhciIdentifyCommandExecutor mutationExecutor{beforeMutation, beforeMutation, mutationClock};
    AhciIdentifyCommandReceipt mutationReceipt{};
    const auto mutationFailure = mutationExecutor.Execute(MakeRequest(), mutationReceipt);
    ASTERIA_EXPECT(runner, "absolute deadline is rechecked immediately before DMA mutation",
                   mutationFailure.Error == AhciIdentifyCommandError::DeadlineReached,
                   mutationFailure.PortDisposition == AhciIdentifyPortDisposition::Unchanged,
                   BytesEqual(g_dmaPage, g_beforePage, sizeof(g_dmaPage)));

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio beforeIssue{g_dmaPage};
    beforeIssue.SetRegister(PortCommandAddress, CommandConfiguration);
    FakeClock issueClock;
    issueClock.Set(1U, 500U);
    AhciIdentifyCommandExecutor issueExecutor{beforeIssue, beforeIssue, issueClock};
    AhciIdentifyCommandReceipt issueReceipt{};
    issueReceipt.PortNumber = 0x5AU;
    const auto issueFailure = issueExecutor.Execute(MakeRequest(), issueReceipt);
    ASTERIA_EXPECT(runner, "deadline is rechecked before PxCI issue and failure stays atomic",
                   issueFailure.Error == AhciIdentifyCommandError::DeadlineReached,
                   issueFailure.PortDisposition == AhciIdentifyPortDisposition::Restored,
                   beforeIssue.GetRegister(CommandIssueAddress) == 0U, issueReceipt.PortNumber == 0x5AU);
}

void TestTimeoutRecoveryRequiresRestartSafeState(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio mmio{g_dmaPage};
    mmio.ConfigureCompletion(false, 1U, 0U, 0U);
    FakeClock clock;
    AhciIdentifyCommandExecutor executor{mmio, mmio, clock};
    AhciIdentifyCommandReceipt receipt{};
    const auto failure = executor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 4U, 8U), receipt);
    ASTERIA_EXPECT(runner, "a compliant ST clear retires a timed-out command before restart",
                   failure.Error == AhciIdentifyCommandError::CommandTimedOut,
                   failure.Register == AhciIdentifyRegister::PortCommandIssue,
                   failure.PortDisposition == AhciIdentifyPortDisposition::Restored, failure.CommandIssue == 0U,
                   !failure.DmaPageMustRemainLive, failure.RecoveryError == AhciIdentifyCommandError::None,
                   HasRestartSafetySequence(mmio));
    ASTERIA_EXPECT(runner, "safe timeout recovery restores bases, interrupts, and engine state",
                   (mmio.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) ==
                       (CommandStartMask | FisReceiveEnableMask),
                   mmio.GetRegister(PortInterruptEnableAddress) == InitialInterruptEnable,
                   mmio.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   mmio.GetRegister(CommandIssueAddress) == 0U);
    ASTERIA_EXPECT(runner, "stalled-command polling consumes only the caller's exact clock budget",
                   clock.GetReadCount() == 7U);
}

void TestUnsafeRecoveryNeverRestartsCommandEngine(TestContext& runner) noexcept
{
    struct TaskFileCase
    {
        UInt32 Value;
        const char* Description;
    };
    constexpr TaskFileCase taskFileCases[]{
        {0x80U, "IDENTIFY retained BSY restores inert bases without ST"},
        {0x08U, "IDENTIFY retained DRQ restores inert bases without ST"},
    };
    for (const TaskFileCase& taskFileCase : taskFileCases)
    {
        FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
        FakeAhciMmio mmio{g_dmaPage};
        mmio.ConfigureCompletion(false, 1U, 0U, 0U);
        mmio.SetCommandStopStatus(taskFileCase.Value);
        FakeClock clock;
        AhciIdentifyCommandExecutor executor{mmio, mmio, clock};
        AhciIdentifyCommandReceipt receipt{};
        receipt.PortNumber = 0x5AU;
        const AhciIdentifyCommandFailure failure =
            executor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 4U, 8U), receipt);
        ASTERIA_EXPECT(
            runner, taskFileCase.Description, failure.Error == AhciIdentifyCommandError::CommandTimedOut,
            failure.RecoveryError == AhciIdentifyCommandError::PortBusy,
            failure.RecoveryRegister == AhciIdentifyRegister::PortTaskFileData,
            failure.TaskFileData == taskFileCase.Value,
            failure.PortDisposition == AhciIdentifyPortDisposition::CommandStopped, !failure.DmaPageMustRemainLive,
            mmio.GetRegister(CommandIssueAddress) == 0U,
            mmio.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
            (mmio.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) == FisReceiveEnableMask,
            HasOriginalBaseWriteAfterIssue(mmio), !HasRestartWriteAfterIssue(mmio), receipt.PortNumber == 0x5AU);
    }

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio retainedCommand{g_dmaPage};
    retainedCommand.ConfigureCompletion(false, 1U, 0U, 0U);
    retainedCommand.KeepCommandIssueSetOnStop();
    FakeClock retainedCommandClock;
    AhciIdentifyCommandExecutor retainedCommandExecutor{retainedCommand, retainedCommand, retainedCommandClock};
    AhciIdentifyCommandReceipt retainedCommandReceipt{};
    const AhciIdentifyCommandFailure retainedCommandFailure =
        retainedCommandExecutor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 4U, 8U), retainedCommandReceipt);
    ASTERIA_EXPECT(runner, "a noncompliant retained IDENTIFY PxCI slot keeps the DMA page live",
                   retainedCommandFailure.Error == AhciIdentifyCommandError::CommandTimedOut,
                   retainedCommandFailure.RecoveryError == AhciIdentifyCommandError::PortBusy,
                   retainedCommandFailure.RecoveryRegister == AhciIdentifyRegister::PortCommandIssue,
                   retainedCommandFailure.PortDisposition == AhciIdentifyPortDisposition::CommandStopped,
                   retainedCommandFailure.DmaPageMustRemainLive, retainedCommand.GetRegister(CommandIssueAddress) == 1U,
                   !HasOriginalBaseWriteAfterIssue(retainedCommand), !HasRestartWriteAfterIssue(retainedCommand));

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio retainedSataActive{g_dmaPage};
    retainedSataActive.ConfigureCompletion(false, 1U, 0U, 0U);
    retainedSataActive.SetCommandStopStatus(0U, 1U);
    FakeClock retainedSataActiveClock;
    AhciIdentifyCommandExecutor retainedSataActiveExecutor{retainedSataActive, retainedSataActive,
                                                           retainedSataActiveClock};
    AhciIdentifyCommandReceipt retainedSataActiveReceipt{};
    const AhciIdentifyCommandFailure retainedSataActiveFailure = retainedSataActiveExecutor.Execute(
        MakeRequest(DmaPhysicalAddress, AbarLength, 4U, 8U), retainedSataActiveReceipt);
    ASTERIA_EXPECT(runner, "a retained IDENTIFY PxSACT slot suppresses restoration and restart",
                   retainedSataActiveFailure.Error == AhciIdentifyCommandError::CommandTimedOut,
                   retainedSataActiveFailure.RecoveryError == AhciIdentifyCommandError::PortBusy,
                   retainedSataActiveFailure.RecoveryRegister == AhciIdentifyRegister::PortSataActive,
                   retainedSataActiveFailure.DmaPageMustRemainLive, !HasOriginalBaseWriteAfterIssue(retainedSataActive),
                   !HasRestartWriteAfterIssue(retainedSataActive));

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio lateBusy{g_dmaPage};
    lateBusy.ConfigureCompletion(false, 1U, 0U, 0U);
    lateBusy.SetTaskFileDataOnOriginalBaseRestore(0x80U);
    FakeClock lateBusyClock;
    AhciIdentifyCommandExecutor lateBusyExecutor{lateBusy, lateBusy, lateBusyClock};
    AhciIdentifyCommandReceipt lateBusyReceipt{};
    const AhciIdentifyCommandFailure lateBusyFailure =
        lateBusyExecutor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 4U, 8U), lateBusyReceipt);
    ASTERIA_EXPECT(runner, "IDENTIFY PxTFD is resampled immediately before restart",
                   lateBusyFailure.Error == AhciIdentifyCommandError::CommandTimedOut,
                   lateBusyFailure.RecoveryError == AhciIdentifyCommandError::PortBusy,
                   lateBusyFailure.RecoveryRegister == AhciIdentifyRegister::PortTaskFileData,
                   lateBusyFailure.TaskFileData == 0x80U,
                   lateBusyFailure.PortDisposition == AhciIdentifyPortDisposition::CommandStopped,
                   !lateBusyFailure.DmaPageMustRemainLive, HasOriginalBaseWriteAfterIssue(lateBusy),
                   !HasRestartWriteAfterIssue(lateBusy));

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio inactiveLink{g_dmaPage};
    inactiveLink.ConfigureCompletion(false, 1U, 0U, 0U);
    inactiveLink.SetCommandStopStatus(0U, 0U, 0U);
    FakeClock inactiveLinkClock;
    AhciIdentifyCommandExecutor inactiveLinkExecutor{inactiveLink, inactiveLink, inactiveLinkClock};
    AhciIdentifyCommandReceipt inactiveLinkReceipt{};
    const AhciIdentifyCommandFailure inactiveLinkFailure =
        inactiveLinkExecutor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 4U, 8U), inactiveLinkReceipt);
    ASTERIA_EXPECT(runner, "IDENTIFY link loss immediately before restart leaves ST clear",
                   inactiveLinkFailure.Error == AhciIdentifyCommandError::CommandTimedOut,
                   inactiveLinkFailure.RecoveryError == AhciIdentifyCommandError::NoActiveDiskPort,
                   inactiveLinkFailure.RecoveryRegister == AhciIdentifyRegister::PortSataStatus,
                   inactiveLinkFailure.PortDisposition == AhciIdentifyPortDisposition::CommandStopped,
                   !inactiveLinkFailure.DmaPageMustRemainLive,
                   inactiveLink.GetRegister(CommandListBaseAddress) == InitialCommandListBase,
                   HasOriginalBaseWriteAfterIssue(inactiveLink), !HasRestartWriteAfterIssue(inactiveLink));
}

void TestRecoveryExhaustionRetainsDmaPage(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio commandRunning{g_dmaPage};
    commandRunning.ConfigureCompletion(false, 1U, 0U, 0U);
    commandRunning.SetRecoveryEngineStopDelays(8U, 0U);
    FakeClock commandRunningClock;
    AhciIdentifyCommandExecutor commandRunningExecutor{commandRunning, commandRunning, commandRunningClock};
    AhciIdentifyCommandReceipt commandRunningReceipt{};
    const auto commandRunningFailure =
        commandRunningExecutor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 3U, 2U), commandRunningReceipt);
    ASTERIA_EXPECT(
        runner, "post-issue CR exhaustion keeps the page live without clearing FRE",
        commandRunningFailure.Error == AhciIdentifyCommandError::CommandTimedOut,
        commandRunningFailure.RecoveryError == AhciIdentifyCommandError::EngineRestoreFailed,
        commandRunningFailure.RecoveryRegister == AhciIdentifyRegister::PortCommand,
        commandRunningFailure.PortDisposition == AhciIdentifyPortDisposition::StopRequested,
        commandRunningFailure.DmaPageMustRemainLive, (commandRunningFailure.PortCommand & CommandListRunningMask) != 0U,
        (commandRunning.GetRegister(PortCommandAddress) & CommandStartMask) == 0U,
        (commandRunning.GetRegister(PortCommandAddress) & FisReceiveEnableMask) != 0U,
        commandRunning.GetRegister(CommandIssueAddress) == 1U,
        commandRunning.GetRegister(CommandListBaseAddress) == static_cast<UInt32>(DmaPhysicalAddress & 0xFFFF'FFFFULL));

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio fisRunning{g_dmaPage};
    fisRunning.ConfigureCompletion(false, 1U, 0U, 0U);
    fisRunning.SetRecoveryEngineStopDelays(0U, 8U);
    FakeClock fisRunningClock;
    AhciIdentifyCommandExecutor fisRunningExecutor{fisRunning, fisRunning, fisRunningClock};
    AhciIdentifyCommandReceipt fisRunningReceipt{};
    const auto fisRunningFailure =
        fisRunningExecutor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 3U, 2U), fisRunningReceipt);
    ASTERIA_EXPECT(
        runner, "post-issue FR exhaustion keeps the page live after ordered stop requests",
        fisRunningFailure.Error == AhciIdentifyCommandError::CommandTimedOut,
        fisRunningFailure.RecoveryError == AhciIdentifyCommandError::EngineRestoreFailed,
        fisRunningFailure.RecoveryRegister == AhciIdentifyRegister::PortCommand,
        fisRunningFailure.PortDisposition == AhciIdentifyPortDisposition::StopRequested,
        fisRunningFailure.DmaPageMustRemainLive, (fisRunningFailure.PortCommand & FisReceiveRunningMask) != 0U,
        (fisRunning.GetRegister(PortCommandAddress) & (CommandStartMask | FisReceiveEnableMask)) == 0U,
        fisRunning.GetRegister(CommandIssueAddress) == 0U,
        fisRunning.GetRegister(CommandListBaseAddress) == static_cast<UInt32>(DmaPhysicalAddress & 0xFFFF'FFFFULL));
}

void TestPortCommandWritesNeverReplayActionFields(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio cloMmio{g_dmaPage};
    cloMmio.SetRegister(PortCommandAddress, InitialCommand | CommandListOverrideMask);
    FakeClock cloClock;
    AhciIdentifyCommandExecutor cloExecutor{cloMmio, cloMmio, cloClock};
    AhciIdentifyCommandReceipt cloReceipt{};
    const auto cloFailure = cloExecutor.Execute(MakeRequest(), cloReceipt);
    ASTERIA_EXPECT(runner, "every PxCMD write masks the read-write-one CLO action bit",
                   cloFailure.Error == AhciIdentifyCommandError::None,
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
        FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
        FakeAhciMmio iccMmio{g_dmaPage};
        iccMmio.SetRegister(PortCommandAddress, transientIccCommand);
        FakeClock iccClock;
        AhciIdentifyCommandExecutor iccExecutor{iccMmio, iccMmio, iccClock};
        AhciIdentifyCommandReceipt iccReceipt{};
        const auto iccFailure = iccExecutor.Execute(MakeRequest(), iccReceipt);
        ASTERIA_EXPECT(runner, iccCase.Description, iccFailure.Error == AhciIdentifyCommandError::None,
                       iccReceipt.InitialPortCommand == transientIccCommand,
                       iccReceipt.RestoredPortCommand == InitialCommand,
                       iccMmio.GetRegister(PortCommandAddress) == InitialCommand,
                       EveryPortCommandWritePreservesConfiguration(iccMmio, CommandConfiguration));
    }

    constexpr UInt32 stoppedActionCommand{CommandConfiguration | CommandListOverrideMask | (8U << 28U)};
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio stoppedMmio{g_dmaPage};
    stoppedMmio.SetRegister(PortCommandAddress, stoppedActionCommand);
    FakeClock stoppedClock;
    AhciIdentifyCommandExecutor stoppedExecutor{stoppedMmio, stoppedMmio, stoppedClock};
    AhciIdentifyCommandReceipt stoppedReceipt{};
    const auto stoppedFailure = stoppedExecutor.Execute(MakeRequest(), stoppedReceipt);
    ASTERIA_EXPECT(runner, "stopped-engine restoration masks transient CLO and ICC actions",
                   stoppedFailure.Error == AhciIdentifyCommandError::None,
                   stoppedReceipt.InitialPortCommand == stoppedActionCommand,
                   stoppedReceipt.RestoredPortCommand == CommandConfiguration,
                   stoppedMmio.GetRegister(PortCommandAddress) == CommandConfiguration,
                   EveryPortCommandWritePreservesConfiguration(stoppedMmio, CommandConfiguration));
}

void TestDelayedEngineTransitionsRemainBounded(TestContext& runner) noexcept
{
    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio mmio{g_dmaPage};
    mmio.SetEngineStopDelays(2U, 1U);
    FakeClock clock;
    AhciIdentifyCommandExecutor executor{mmio, mmio, clock};
    AhciIdentifyCommandReceipt receipt{};
    const auto failure = executor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 32U, 32U), receipt);
    ASTERIA_EXPECT(runner, "CR/FR transitions are polled to completion with explicit bounds",
                   failure.Error == AhciIdentifyCommandError::None,
                   receipt.PortDisposition == AhciIdentifyPortDisposition::Restored, receipt.PollCount >= 9U);

    FillBytes(g_dmaPage, sizeof(g_dmaPage), Sentinel);
    FakeAhciMmio stuck{g_dmaPage};
    stuck.SetEngineStopDelays(20U, 0U);
    FakeClock stuckClock;
    AhciIdentifyCommandExecutor stuckExecutor{stuck, stuck, stuckClock};
    AhciIdentifyCommandReceipt stuckReceipt{};
    const auto stuckFailure = stuckExecutor.Execute(MakeRequest(DmaPhysicalAddress, AbarLength, 2U, 2U), stuckReceipt);
    ASTERIA_EXPECT(runner, "a stuck PxCMD.CR transition has a distinct bounded stop timeout",
                   stuckFailure.Error == AhciIdentifyCommandError::EngineStopTimedOut,
                   stuckFailure.PortDisposition == AhciIdentifyPortDisposition::StopRequested);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"AhciIdentifyCommandExecutorTests"};
    Asteria::Test::TestSuccessfulExecutionAndExactTransaction(runner);
    Asteria::Test::TestPortSelectionFailuresAreAtomic(runner);
    Asteria::Test::TestDmaPageCannotAliasLiveCallObjects(runner);
    Asteria::Test::TestRangeAddressWidthAndBusyFailures(runner);
    Asteria::Test::TestCommandErrorsAndTransactionalRestore(runner);
    Asteria::Test::TestCompletionEvidenceAndIdentifyValidity(runner);
    Asteria::Test::TestLba28BoundsAndEmbeddedNullStrings(runner);
    Asteria::Test::TestAbsoluteDeadlineCheckpoints(runner);
    Asteria::Test::TestTimeoutRecoveryRequiresRestartSafeState(runner);
    Asteria::Test::TestUnsafeRecoveryNeverRestartsCommandEngine(runner);
    Asteria::Test::TestRecoveryExhaustionRetainsDmaPage(runner);
    Asteria::Test::TestPortCommandWritesNeverReplayActionFields(runner);
    Asteria::Test::TestDelayedEngineTransitionsRemainBounded(runner);
    return runner.Finish();
}
