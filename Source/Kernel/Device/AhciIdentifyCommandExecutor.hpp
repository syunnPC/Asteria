// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DEVICE_AHCI_IDENTIFY_COMMAND_EXECUTOR_HPP_INCLUDED
#define ASTERIA_KERNEL_DEVICE_AHCI_IDENTIFY_COMMAND_EXECUTOR_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Hal/Device/IMemoryMappedIo.hpp"
#include "Hal/Device/IMemoryMappedIoWriter.hpp"
#include "Hal/Time/IMonotonicClock.hpp"
#include "Kernel/Device/AhciHbaRegisterSnapshotReader.hpp"
#include "Kernel/Device/AhciIdentifyDmaPageBuilder.hpp"

namespace Asteria::Kernel::Device
{
enum class AhciIdentifyCommandError : Foundation::UInt8
{
    None,
    InvalidAbarRange,
    InvalidPollingPolicy,
    ClockExhausted,
    ClockReversed,
    DeadlineReached,
    MemoryMappedIoReadFailed,
    InvalidRegisterValue,
    MemoryMappedIoWriteFailed,
    AhciModeDisabled,
    PortsImplementedOutOfRange,
    NoActiveDiskPort,
    AmbiguousActiveDiskPorts,
    AtapiDeviceNotSupported,
    UnsupportedDeviceSignature,
    DmaPageRejected,
    PortBusy,
    PortInterruptError,
    TaskFileError,
    IncompleteDataTransfer,
    InvalidCompletionFis,
    InvalidIdentifyData,
    IdentifyCapacityOverflow,
    EngineStopTimedOut,
    CommandTimedOut,
    EngineRestoreFailed,
    DmaPageOverlapsRequest,
    DmaPageOverlapsReceipt,
};

enum class AhciIdentifyCommandPhase : Foundation::UInt8
{
    Validation,
    PortSelection,
    PortPreparation,
    CommandExecution,
    PortRestoration,
};

enum class AhciIdentifyRegister : Foundation::UInt8
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

enum class AhciIdentifyPortDisposition : Foundation::UInt8
{
    Unchanged,
    StopRequested,
    CommandStopped,
    Restored,
    Uncertain,
};

struct AhciIdentifyPollingPolicy
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

struct AhciIdentifyCommandRequest
{
    AhciHbaRegisterRange Abar;
    AhciDmaPageWritableView DmaPage;
    AhciIdentifyPollingPolicy Polling;
};

struct AhciIdentifyDeviceInformation
{
    char SerialNumber[21];
    char ModelNumber[41];
    Foundation::UInt64 SectorCount;
    Foundation::UInt64 CapacityBytes;
    Foundation::UInt64 LogicalSectorSizeBytes;
    Foundation::UInt64 PhysicalSectorSizeBytes;
    Foundation::UInt8 SerialNumberLength;
    Foundation::UInt8 ModelNumberLength;
    bool Uses48BitAddressing;
    Foundation::UInt8 Reserved[5];
};

struct AhciIdentifyCommandReceipt
{
    AhciIdentifyDeviceInformation Device;
    Foundation::UInt64 StartTicks;
    Foundation::UInt64 CompletionTicks;
    Foundation::Size PollCount;
    Foundation::UInt32 Capabilities;
    Foundation::UInt32 PortsImplemented;
    Foundation::UInt32 PortSignature;
    Foundation::UInt32 CompletionInterruptStatus;
    Foundation::UInt32 CompletionTaskFileData;
    Foundation::UInt32 InitialPortCommand;
    Foundation::UInt32 RestoredPortCommand;
    Foundation::UInt8 PortNumber;
    AhciIdentifyPortDisposition PortDisposition;
    Foundation::UInt8 DeviceToHostFisStatus;
    Foundation::UInt8 DeviceToHostFisError;
    Foundation::UInt8 Reserved[4];
};

struct AhciIdentifyCommandFailure
{
    Foundation::UInt64 ObservedTicks;
    Foundation::Size PollCount;
    Foundation::UInt32 PortInterruptStatus;
    Foundation::UInt32 TaskFileData;
    Foundation::UInt32 CommandIssue;
    Foundation::UInt32 PortCommand;
    AhciIdentifyCommandError Error;
    AhciIdentifyCommandError RecoveryError;
    AhciIdentifyCommandPhase Phase;
    AhciIdentifyRegister Register;
    AhciIdentifyRegister RecoveryRegister;
    AhciIdentifyPortDisposition PortDisposition;
    Hal::Device::MemoryMappedIoReadError MemoryMappedIoRead;
    Hal::Device::MemoryMappedIoWriteError MemoryMappedIoWrite;
    Hal::Device::MemoryMappedIoReadError RecoveryMemoryMappedIoRead;
    Hal::Device::MemoryMappedIoWriteError RecoveryMemoryMappedIoWrite;
    AhciIdentifyDmaPageError DmaPage;
    Foundation::UInt8 PortNumber;
    Foundation::UInt8 MatchingDiskPortCount;
    Foundation::UInt8 ActiveAtapiPortCount;
    Foundation::UInt8 ActiveUnsupportedPortCount;
    /// True requires the caller to retain the exact DMA page until external
    /// recovery proves the command and FIS engines quiescent and PxCI/PxSACT
    /// clear, or resets the controller. No page byte may be reused while true.
    bool DmaPageMustRemainLive;
    Foundation::UInt8 Reserved[2];
};

/// Executes one polling ATA IDENTIFY DEVICE transaction through AHCI slot 0.
///
/// The reader, writer, and clock are independent non-owning capabilities and
/// must outlive this object and Execute. The ABAR range and DMA span remain
/// caller-owned. The DMA span must be coherent for the controller, remain live
/// for the full call, and use the exact controller-visible address supplied in
/// DmaPage.PhysicalAddress. The MMIO writer must order prior normal-memory DMA
/// writes before command issue; successful MMIO reads must make completed DMA
/// writes visible before this class parses the IDENTIFY payload.
///
/// Execute accepts exactly one active ATA disk port and no other active storage
/// signature. PI bits outside CAP.NP, any active ATAPI or unknown signature,
/// and multiple active ATA disks are deterministic failures. The selected port
/// must have no command in PxCI/PxSACT and no BSY/DRQ task-file state.
///
/// The command engine is stopped before CLB/FB replacement. Polling interrupts
/// are disabled, slot 0 is issued, and the original bases, interrupt enable,
/// and engine state are restored after a quiescent completion. W1C status is
/// acknowledged by writing only values previously read from the corresponding
/// status register. Recovery always requests ST clear and waits for CR clear
/// before requesting FRE clear and waiting for FR clear. A timeout or uncertain
/// MMIO result can prevent proof that either engine stopped. After a possible
/// command-issue write, recovery retains the temporary bases until PxCI and
/// PxSACT are positively clear. It may then restore the original bases and FIS
/// engine without retaining the DMA page, but restores original ST only after a
/// fresh PxTFD proves BSY/DRQ clear and an immediately preceding PxSSTS read
/// proves the link active. A failed prerequisite leaves ST clear and records a
/// precise recovery error.
/// PxCMD.CLO is a read-write-one action and nonzero PxCMD.ICC requests a link
/// transition. Neither is restorable configuration, so every PxCMD write masks
/// both fields even if a transient read snapshot contains them.
///
/// DmaPageMustRemainLive is the authoritative failure-side lifetime contract.
/// When true, the page remains controller-owned even if ST or FRE was requested
/// clear; the caller must retain it until controller reset or external recovery
/// proves PxCI/PxSACT clear and both engines quiescent. False is authoritative
/// even if restart is suppressed: stopped engines and clear slots can no longer
/// access the temporary page, and recovery restores original bases when MMIO
/// remains usable. PortDisposition describes observed restoration progress but
/// does not replace that lifetime flag. Failure leaves the caller's receipt
/// unchanged, so parsed device identity is published atomically only after the
/// original port configuration is safely restored.
///
/// Calls are allocation-free and bounded by MaximumPollCount plus
/// MaximumRecoveryPollCount. The caller serializes the HBA and prevents ISR
/// use, concurrent entry, and same-CPU reentry through any supplied backend.
class AhciIdentifyCommandExecutor final
{
public:
    constexpr AhciIdentifyCommandExecutor(Hal::Device::IMemoryMappedIo& memoryMappedIo,
                                          Hal::Device::IMemoryMappedIoWriter& memoryMappedIoWriter,
                                          const Hal::Time::IMonotonicClock& clock) noexcept
        : m_memoryMappedIo{memoryMappedIo}, m_memoryMappedIoWriter{memoryMappedIoWriter}, m_clock{clock}
    {
    }

    AhciIdentifyCommandExecutor(const AhciIdentifyCommandExecutor&) = delete;
    AhciIdentifyCommandExecutor& operator=(const AhciIdentifyCommandExecutor&) = delete;
    AhciIdentifyCommandExecutor(AhciIdentifyCommandExecutor&&) = delete;
    AhciIdentifyCommandExecutor& operator=(AhciIdentifyCommandExecutor&&) = delete;

    [[nodiscard]] AhciIdentifyCommandFailure Execute(const AhciIdentifyCommandRequest& request,
                                                     AhciIdentifyCommandReceipt& output) noexcept;

private:
    Hal::Device::IMemoryMappedIo& m_memoryMappedIo;
    Hal::Device::IMemoryMappedIoWriter& m_memoryMappedIoWriter;
    const Hal::Time::IMonotonicClock& m_clock;
};

static_assert(sizeof(AhciIdentifyCommandError) == 1U);
static_assert(sizeof(AhciIdentifyCommandPhase) == 1U);
static_assert(sizeof(AhciIdentifyRegister) == 1U);
static_assert(sizeof(AhciIdentifyPortDisposition) == 1U);
static_assert(sizeof(AhciIdentifyPollingPolicy) == 24U);
static_assert(alignof(AhciIdentifyPollingPolicy) == 8U);
static_assert(sizeof(AhciIdentifyCommandRequest) == 64U);
static_assert(alignof(AhciIdentifyCommandRequest) == 8U);
static_assert(sizeof(AhciIdentifyDeviceInformation) == 104U);
static_assert(alignof(AhciIdentifyDeviceInformation) == 8U);
static_assert(sizeof(AhciIdentifyCommandReceipt) == 168U);
static_assert(alignof(AhciIdentifyCommandReceipt) == 8U);
static_assert(sizeof(AhciIdentifyCommandFailure) == 56U);
static_assert(alignof(AhciIdentifyCommandFailure) == 8U);
} // namespace Asteria::Kernel::Device

#endif // ASTERIA_KERNEL_DEVICE_AHCI_IDENTIFY_COMMAND_EXECUTOR_HPP_INCLUDED
