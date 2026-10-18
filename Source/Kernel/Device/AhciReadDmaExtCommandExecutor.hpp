// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_READ_DMA_EXT_COMMAND_EXECUTOR_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_READ_DMA_EXT_COMMAND_EXECUTOR_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"
#include "Hal/Device/IMemoryMappedIoWriter.hpp"
#include "Hal/Time/IMonotonicClock.hpp"
#include "Kernel/Device/AhciHbaRegisterSnapshotReader.hpp"
#include "Kernel/Device/AhciReadDmaExtCommandBuilder.hpp"

namespace Asteria::Kernel::Device
{
enum class AhciReadDmaExtExecutionError : Foundation::UInt8
{
    None,
    InvalidAbarRange,
    InvalidPortNumber,
    InvalidPollingPolicy,
    ClockExhausted,
    ClockReversed,
    DeadlineReached,
    MemoryMappedIoReadFailed,
    InvalidRegisterValue,
    MemoryMappedIoWriteFailed,
    AhciModeDisabled,
    PortsImplementedOutOfRange,
    PortNotImplemented,
    InactiveSataLink,
    AtapiDeviceNotSupported,
    UnsupportedDeviceSignature,
    CommandRejected,
    PortBusy,
    PortInterruptError,
    TaskFileError,
    IncompleteDataTransfer,
    InvalidCompletionFis,
    EngineStopTimedOut,
    CommandTimedOut,
    EngineRestoreFailed,
    CommandPageOverlapsRequest,
    CommandPageOverlapsReceipt,
};

enum class AhciReadDmaExtExecutionPhase : Foundation::UInt8
{
    Validation,
    PortSelection,
    PortPreparation,
    CommandExecution,
    PortRestoration,
};

enum class AhciReadDmaExtRegister : Foundation::UInt8
{
    None,
    Capabilities,
    GlobalHostControl,
    GlobalInterruptStatus,
    PortsImplemented,
    PortCommandListBase,
    PortCommandListBaseUpper,
    PortFisBase,
    PortFisBaseUpper,
    PortInterruptStatus,
    PortInterruptEnable,
    PortCommand,
    PortTaskFileData,
    PortSignature,
    PortSataStatus,
    PortSataError,
    PortSataActive,
    PortCommandIssue,
};

enum class AhciReadDmaExtPortDisposition : Foundation::UInt8
{
    Unchanged,
    StopRequested,
    CommandStopped,
    Restored,
    Uncertain,
};

struct AhciReadDmaExtPollingPolicy
{
    /// Absolute deadline in the supplied clock's domain. Zero and all-ones
    /// are invalid.
    Foundation::UInt64 DeadlineTicks;
    /// Total unsuccessful polls allowed before command completion. This also
    /// bounds engine-stop polling before the command is issued.
    Foundation::Size MaximumPollCount;
    /// Separate bounded allowance used only while making the port safe and
    /// restoring its original configuration after command issue or failure.
    Foundation::Size MaximumRecoveryPollCount;
};

struct AhciReadDmaExtExecutionRequest
{
    AhciHbaRegisterRange Abar;
    AhciReadDmaExtCommandPageWritableView CommandPage;
    AhciReadDmaExtDataBufferSpan DataBuffer;
    Foundation::UInt64 DeviceSectorCount;
    Foundation::UInt64 StartingLba;
    Foundation::UInt64 LogicalSectorSizeBytes;
    Foundation::UInt64 SectorCount;
    AhciReadDmaExtPollingPolicy Polling;
    Foundation::UInt8 PortNumber;
    bool Supports48BitAddressing;
    Foundation::UInt8 Reserved[6];
};

struct AhciReadDmaExtExecutionReceipt
{
    Foundation::UInt64 StartTicks;
    Foundation::UInt64 CompletionTicks;
    Foundation::Size PollCount;
    Foundation::UInt64 DeviceSectorCount;
    Foundation::UInt64 StartingLba;
    Foundation::UInt64 LogicalSectorSizeBytes;
    Foundation::UInt64 SectorCount;
    Foundation::UInt64 TransferSizeBytes;
    Foundation::UInt32 Capabilities;
    Foundation::UInt32 PortsImplemented;
    Foundation::UInt32 PortSignature;
    Foundation::UInt32 CompletionInterruptStatus;
    Foundation::UInt32 CompletionTaskFileData;
    Foundation::UInt32 InitialPortCommand;
    Foundation::UInt32 RestoredPortCommand;
    Foundation::UInt8 PortNumber;
    AhciReadDmaExtPortDisposition PortDisposition;
    Foundation::UInt8 DeviceToHostFisStatus;
    Foundation::UInt8 DeviceToHostFisError;
    Foundation::UInt8 Reserved[4];
};

struct AhciReadDmaExtExecutionFailure
{
    Foundation::UInt64 ObservedTicks;
    Foundation::Size PollCount;
    Foundation::UInt32 PortInterruptStatus;
    Foundation::UInt32 TaskFileData;
    Foundation::UInt32 CommandIssue;
    Foundation::UInt32 PortCommand;
    AhciReadDmaExtExecutionError Error;
    AhciReadDmaExtExecutionError RecoveryError;
    AhciReadDmaExtExecutionPhase Phase;
    AhciReadDmaExtRegister Register;
    AhciReadDmaExtRegister RecoveryRegister;
    AhciReadDmaExtPortDisposition PortDisposition;
    Hal::Device::MemoryMappedIoReadError MemoryMappedIoRead;
    Hal::Device::MemoryMappedIoWriteError MemoryMappedIoWrite;
    Hal::Device::MemoryMappedIoReadError RecoveryMemoryMappedIoRead;
    Hal::Device::MemoryMappedIoWriteError RecoveryMemoryMappedIoWrite;
    AhciReadDmaExtCommandError Command;
    Foundation::UInt8 PortNumber;
    /// True requires retaining the exact command page until external recovery
    /// proves PxCI/PxSACT clear and both engines quiescent, or resets the
    /// controller.
    bool CommandPageMustRemainLive;
    /// True requires retaining the exact data buffer under the same condition.
    /// The executor never dereferences or clears this external buffer.
    bool DataBufferMustRemainLive;
    Foundation::UInt8 Reserved[3];
};

/// Executes one polling ATA READ DMA EXT transaction through AHCI slot 0.
///
/// The caller selects one ATA port and supplies exact controller-visible spans,
/// device geometry, LBA48 capability, and a bounded polling policy. The reader,
/// writer, and clock are independent non-owning capabilities and must outlive
/// this object and Execute. Both DMA spans must be coherent and remain live for
/// the complete call. The MMIO writer must order prior normal-memory command
/// writes before PxCI issue; successful MMIO reads must make completed DMA
/// writes visible before command-page completion fields are examined.
///
/// The selected port must be within CAP.NP, set in PI, have an active SATA link
/// and an ATA disk signature, and have no PxCI/PxSACT or BSY/DRQ state. The
/// command engine is stopped before CLB/FB replacement. Polling interrupts are
/// disabled, then slot 0 is issued. After any issue attempt, the temporary bases
/// are retained until CR/FR are stopped and PxCI/PxSACT are positively clear.
/// At that point the original bases, interrupt enable, and FIS engine may be
/// restored without retaining either DMA span. Original ST is restored only
/// after a fresh PxTFD proves BSY/DRQ clear and an immediately preceding PxSSTS
/// read proves the link active. Otherwise ST remains clear and the recovery
/// error identifies the failed prerequisite. W1C writes contain only previously
/// read bits. PxCMD.CLO and ICC are action fields, so every PxCMD write masks
/// them.
///
/// CommandPageMustRemainLive and DataBufferMustRemainLive are authoritative on
/// failure. A true flag requires retention until reset or external recovery has
/// proved PxCI/PxSACT clear and both engines quiescent. False is authoritative
/// even when restart is suppressed: stopped engines and clear slots can no
/// longer access the temporary spans, and recovery restores the original bases
/// when MMIO remains usable. PortDisposition only reports observed recovery
/// progress. Failure leaves output unchanged; success is published only after
/// safe restoration. The external data buffer is never dereferenced or cleared
/// by this class.
///
/// Calls are allocation-free and bounded by MaximumPollCount plus
/// MaximumRecoveryPollCount. The caller serializes the HBA and prevents ISR
/// use, concurrent entry, and same-CPU reentry through any supplied backend.
class AhciReadDmaExtCommandExecutor final
{
public:
    constexpr AhciReadDmaExtCommandExecutor(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                            Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter,
                                            const Hal::Time::IMonotonicClock& clock) noexcept
        : m_memoryMappedIo{memoryMappedIo}, m_memoryMappedIoWriter{memoryMappedIoWriter}, m_clock{clock}
    {
    }

    AhciReadDmaExtCommandExecutor(const AhciReadDmaExtCommandExecutor&) = delete;
    AhciReadDmaExtCommandExecutor& operator=(const AhciReadDmaExtCommandExecutor&) = delete;
    AhciReadDmaExtCommandExecutor(AhciReadDmaExtCommandExecutor&&) = delete;
    AhciReadDmaExtCommandExecutor& operator=(AhciReadDmaExtCommandExecutor&&) = delete;

    [[nodiscard]] AhciReadDmaExtExecutionFailure Execute(const AhciReadDmaExtExecutionRequest& request,
                                                         AhciReadDmaExtExecutionReceipt& output) noexcept;

private:
    Hal::Device::IMemoryMappedIo& m_memoryMappedIo;
    Hal::Device::IMemoryMappedIoWriter& m_memoryMappedIoWriter;
    const Hal::Time::IMonotonicClock& m_clock;
};

static_assert(sizeof(AhciReadDmaExtExecutionError) == 1U);
static_assert(sizeof(AhciReadDmaExtExecutionPhase) == 1U);
static_assert(sizeof(AhciReadDmaExtRegister) == 1U);
static_assert(sizeof(AhciReadDmaExtPortDisposition) == 1U);
static_assert(sizeof(AhciReadDmaExtPollingPolicy) == 24U);
static_assert(alignof(AhciReadDmaExtPollingPolicy) == 8U);
static_assert(sizeof(AhciReadDmaExtExecutionRequest) == 120U);
static_assert(alignof(AhciReadDmaExtExecutionRequest) == 8U);
static_assert(sizeof(AhciReadDmaExtExecutionReceipt) == 104U);
static_assert(alignof(AhciReadDmaExtExecutionReceipt) == 8U);
static_assert(sizeof(AhciReadDmaExtExecutionFailure) == 56U);
static_assert(alignof(AhciReadDmaExtExecutionFailure) == 8U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_READ_DMA_EXT_COMMAND_EXECUTOR_HPP_INCLUDED
