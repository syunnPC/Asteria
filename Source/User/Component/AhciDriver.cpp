// SPDX-License-Identifier: Apache-2.0

#include "User/Component/AhciDriver.hpp"

#include "Abi/Clock.hpp"

#include "Abi/SystemCallAbi.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/BlockRequests.hpp"
#include "User/Component/DriverSetup.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/ServiceCapabilities.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
namespace
{
using System::ReadRegister;

// The driver's DMA page, the second page of its window: the command list at 0x000 (slot 0 only: one
// command in flight), the received FIS at 0x400, slot 0's command table at 0x500 (AhciPort.hpp's
// layout) and IDENTIFY DEVICE's 512 bytes at 0xA00. The data window holds the command in flight's
// part of the client's Frame, at most BlockMaximumSectors sectors (16 pages), mapped as the command
// issues and unmapped once it completed. The driver asks for these device addresses, which the VT-d
// backend keeps table pages for until the domain is released; the device is given the ones IommuMap
// answers (on the trusted-driver profile, the physical addresses:
// Documentation/Architecture/IommuDomain.md).
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt64 DmaPageWindow{0x0010'0000U};
constexpr Foundation::Size IdentifyOffset{0xA00U};
constexpr Foundation::UInt64 DataWindow{0x0020'0000U};
constexpr Foundation::UInt32 CommandSlot{0U};
// The ports whose registers lie in the one page of registers the driver
// maps: 0x100 + 30 * 0x80 bytes fill it (QEMU's ich9-ahci implements six).
constexpr Foundation::UInt32 MappedPorts{30U};
// PxIS and PxIE (AHCI 1.3.1 section 3.3.5): a D2H Register FIS arrived; and
// the errors that end a command: task file, host bus data, host bus fatal,
// interface fatal.
constexpr Foundation::UInt32 AhciDhrs{1U};
constexpr Foundation::UInt32 CommandErrors{System::AhciTfes | (1U << 29U) | (1U << 28U) | (1U << 27U)};
// How long a command in flight goes unpolled at most: the loop checks it at
// each wake, so a lost interrupt, or none when the kernel refused the
// Interrupt, costs at most this long.
constexpr Foundation::UInt64 CommandPollMicroseconds{20'000U};
// The clock budgets, in multiples of the block's tick count (three of the
// Kernel's time slices, 120M TSC ticks on AMD64, 30 to 60 ms at the rates QEMU runs at): a port
// stops, starts and becomes ready within 16 (AHCI gives PxCMD.CR 500 ms to clear), IDENTIFY DEVICE
// completes within 64.
constexpr Foundation::UInt64 PortBudgetShift{4U};
constexpr Foundation::UInt64 IdentifyBudgetShift{6U};
// IDENTIFY DEVICE's words (ATA8-ACS): word 83 valid (bits 15-14 01) with the
// 48-bit Address feature set (bit 10), and the 48-bit capacity in words
// 100-102 (word 103 zero).
constexpr Foundation::Size IdentifyCommandSetWord{83U};
constexpr Foundation::Size IdentifyCapacityWord{100U};
constexpr Foundation::UInt16 IdentifyValidMask{0xC000U};
constexpr Foundation::UInt16 IdentifyValid{0x4000U};
constexpr Foundation::UInt16 IdentifyAddress48{0x0400U};
// A delivery's sender identity (IpcDeliveryCodec), its client's credit key.
constexpr Foundation::Size DeliverySenderOffset{24U};

using RequestTable = BlockRequestTable<1U>;

// The instance: its device and window, what its image gave it, its
// capabilities, its disk and its requests. Port is MappedPorts while it
// has no disk to serve; Capacity is the disk's sectors.
struct AhciDriverState
{
    volatile Foundation::UInt32* Registers;
    Foundation::UInt8* DmaPage;
    AhciDriverPlatform Platform;
    RequestTable* Requests;
    System::CapabilityHandle Endpoint;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Iommu;
    System::CapabilityHandle Interrupt;
    Foundation::UInt64 Generation;
    Foundation::UInt64 CrashKind;
    Foundation::UInt64 SpinTicks;
    Foundation::UInt64 Capacity;
    Foundation::UInt64 MediumGeneration;
    Foundation::UInt64 Refusal;
    // Replies refused by the kernel are dropped and counted.
    Foundation::UInt64 DroppedReplies;
    /// The DMA page's device address, as IommuMap answered it.
    Foundation::UInt64 DmaPageDevice;
    /// The data window's device address while a command is in flight.
    Foundation::UInt64 DataDevice;
    System::CapabilityHandle Domain;
    System::CapabilityHandle Space;
    System::CapabilityHandle BounceFrame;
    System::CapabilityHandle ClientMemory;
    Foundation::UInt64 Window;
    bool Address64;
    Foundation::UInt32 Port;
    bool Interrupts;
};

// The instance's state, its request table and the buffer it receives into:
// static, not on its stack, and inside one mapping of its
// space, its.bss, as the kernel's record-backed access requires.
constinit RequestTable g_requests{};
constinit AhciDriverState g_driver{};
alignas(8) constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};

[[nodiscard]] bool HasDisk(const AhciDriverState& driver) noexcept
{
    return driver.Port < MappedPorts;
}

[[nodiscard]] bool IsBusy(const AhciDriverState& driver) noexcept
{
    return driver.Requests->CountState(BlockRequestState::Issued) != 0U;
}

// The first implemented port whose registers the driver maps with a device
// present and its link up (PxSSTS.DET 3), or MappedPorts. PxSIG is not read:
// QEMU restores it only once FIS reception restarts after a COMRESET.
[[nodiscard]] Foundation::UInt32 FindDiskPort(const volatile Foundation::UInt32* const registers,
                                              const Foundation::UInt32 implemented) noexcept
{
    for (Foundation::UInt32 port{0U}; port < MappedPorts; ++port)
    {
        if (((implemented & (1U << port)) != 0U) &&
            ((ReadRegister(registers, System::AhciPortWord(port) + System::AhciSstsWord) & System::AhciDetMask) ==
             System::AhciDevicePresent))
        {
            return port;
        }
    }
    return MappedPorts;
}

// Starts the stopped port on the DMA page and waits for the device to be
// ready (PxTFD BSY and DRQ clear); after a Device Manager's COMRESET the
// device reports so only once FIS reception runs.
[[nodiscard, gnu::noinline]] bool StartPort(const AhciDriverState& driver, const Foundation::UInt32 port) noexcept
{
    const Foundation::UInt64 budget = driver.SpinTicks << PortBudgetShift;
    return System::StartAhciPort(driver.Registers, port, driver.DmaPageDevice,
                                 driver.DmaPageDevice + System::AhciReceivedFisOffset, driver.Platform.WriteRegister) &&
           System::WaitAhciRegisterClear(driver.Registers, System::AhciPortWord(port) + System::AhciTfdWord,
                                         System::AhciBsy | System::AhciDrq, driver.Platform.Clock,
                                         driver.Platform.Clock(), budget);
}

// Masks the controller's interrupts (every mapped port's and the global
// enable), takes the port from whoever left it running (a dead instance or
// the boot's diagnostic), confines the function with the DMA page mapped for
// it, lets it master the bus and starts the port. Zero, DriverWithoutIommu,
// or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
PreparePort(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
            AhciDriverState& driver, const Foundation::UInt32 port, const Foundation::UInt32 implemented) noexcept
{
    volatile Foundation::UInt32* const registers = driver.Registers;
    const System::AhciRegisterWriter write = driver.Platform.WriteRegister;
    for (Foundation::UInt32 other{0U}; other < MappedPorts; ++other)
    {
        if ((implemented & (1U << other)) != 0U)
        {
            write(registers, System::AhciPortWord(other) + System::AhciPortIeWord, 0U);
        }
    }
    write(registers, System::AhciGhcWord, ReadRegister(registers, System::AhciGhcWord) & ~System::AhciIe);
    const Foundation::UInt32 engines = System::AhciSt | System::AhciCr | System::AhciFre | System::AhciFr;
    if (((ReadRegister(registers, System::AhciPortWord(port) + System::AhciCmdWord) & engines) != 0U) &&
        !System::StopAhciPort(registers, port, driver.Platform.Clock, driver.SpinTicks << PortBudgetShift, write))
    {
        return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::None);
    }
    // Reset before enabling bus mastering, also for the trusted identity profile. AHCI 1.3.1
    // section 10.4.2.
    if ((implemented >> MappedPorts) != 0U)
    {
        return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::InvalidConfiguration);
    }
    for (Foundation::UInt32 other{0U}; other < MappedPorts; ++other)
    {
        if ((implemented & (1U << other)) != 0U &&
            (!System::ResetAhciPort(registers, other, driver.Platform.Clock, driver.SpinTicks << PortBudgetShift,
                                    driver.SpinTicks, write) ||
             (ReadRegister(registers, System::AhciPortWord(other) + System::AhciCmdWord) & engines) != 0U))
        {
            return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::TimedOut);
        }
    }
    const Foundation::UInt64 confined =
        ConfineFunction(client, startBlock, driver.Iommu,
                        (ReadRegister(registers, System::AhciCapWord) & System::AhciS64a) != 0U ? 64U : 32U);
    if (confined != 0U)
    {
        return confined;
    }
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{startBlock[Abi::ComponentAccountSpaceWord],
                                         startBlock[Abi::ComponentAccountSpaceWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    System::CapabilityHandle page{};
    const Abi::SystemCallError mapped =
        MapDmaPage(client, domain, space, startBlock[Abi::ComponentWindowStartWord] + PageBytes, page);
    if (mapped != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverPagesStep, mapped);
    }
    const DeviceMapping exposed =
        MapForDevice(client, driver.Iommu, page, 1U, DmaPageWindow, Abi::IommuMapDeviceRead | Abi::IommuMapDeviceWrite);
    if (exposed.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverDeviceMapStep, exposed.Error);
    }
    driver.DmaPageDevice = exposed.Address;
    if (!driver.Address64)
    {
        const auto bounce = MapDmaPage(client, domain, space, driver.Window + Abi::AhciBouncePage * PageBytes,
                                       driver.BounceFrame, Abi::AhciBouncePages);
        if (bounce != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverPagesStep, bounce);
        }
        const auto memory = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, Abi::AhciBouncePages);
        if (memory.Error != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverMemoryObjectStep, memory.Error);
        }
        driver.ClientMemory = {memory.Value0, memory.Value1};
    }
    const Abi::SystemCallError mastering = client.ControlDevice(function, Abi::DeviceEnableBusMaster).Error;
    if (mastering != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMasterStep, mastering);
    }
    return StartPort(driver, port) ? 0U : DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
}

[[nodiscard]] Foundation::UInt64 ReadIdentifyWord(const Foundation::UInt8* const data,
                                                  const Foundation::Size word) noexcept
{
    return static_cast<Foundation::UInt64>(data[word << 1U]) |
           (static_cast<Foundation::UInt64>(data[(word << 1U) + 1U]) << 8U);
}

// IDENTIFY DEVICE on slot 0 into the DMA page, polled within its budget. The
// disk must report a 48-bit capacity, since every transfer is a READ or
// WRITE DMA EXT. Zero with driver.Capacity set, or the failure's exit code:
// a command that does not complete is left to the drain and the Device
// Manager's reset.
[[nodiscard, gnu::noinline]] Foundation::UInt64 IdentifyDisk(AhciDriverState& driver,
                                                             const Foundation::UInt32 port) noexcept
{
    constexpr Foundation::UInt64 failure{DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::None)};
    if (!System::BuildAhciIdentify(driver.DmaPage, CommandSlot, driver.DmaPageDevice + IdentifyOffset,
                                   driver.DmaPageDevice + System::AhciCommandTablesOffset))
    {
        return failure;
    }
    System::OrderDmaStorage();
    System::IssueAhciCommand(driver.Registers, port, CommandSlot, driver.Platform.WriteRegister);
    const Foundation::Size status = System::AhciPortWord(port) + System::AhciPortIsWord;
    const Foundation::UInt64 budget = driver.SpinTicks << IdentifyBudgetShift;
    const Foundation::UInt64 start = driver.Platform.Clock();
    while (!System::IsAhciCommandDone(driver.Registers, port, CommandSlot))
    {
        if (((ReadRegister(driver.Registers, status) & CommandErrors) != 0U) ||
            ((Abi::ElapsedClockTicks(driver.Platform.Clock(), start)) >= budget))
        {
            return failure;
        }
    }
    System::OrderDmaStorage();
    const Foundation::UInt8* const data = driver.DmaPage + IdentifyOffset;
    const Foundation::UInt64 commandSet = ReadIdentifyWord(data, IdentifyCommandSetWord);
    driver.Capacity = ReadIdentifyWord(data, IdentifyCapacityWord) |
                      (ReadIdentifyWord(data, IdentifyCapacityWord + 1U) << 16U) |
                      (ReadIdentifyWord(data, IdentifyCapacityWord + 2U) << 32U);
    const Foundation::UInt64 sectorInfo = ReadIdentifyWord(data, 106U);
    if (((sectorInfo & IdentifyValidMask) == IdentifyValid) && ((sectorInfo & (1U << 12U)) != 0U) &&
        ((ReadIdentifyWord(data, 117U) != 256U) || (ReadIdentifyWord(data, 118U) != 0U)))
    {
        driver.Refusal = BlockUnsupportedSectorSize;
    }
    const bool addressed = ((commandSet & IdentifyValidMask) == IdentifyValid) &&
                           ((commandSet & IdentifyAddress48) != 0U) &&
                           (ReadIdentifyWord(data, IdentifyCapacityWord + 3U) == 0U);
    return (((ReadRegister(driver.Registers, status) & CommandErrors) == 0U) && addressed && (driver.Capacity != 0U))
               ? 0U
               : failure;
}

// An Interrupt for the function's MSI in the driver's own domain, bound to the service WaitSet
// beside its Endpoint (the start block's WaitSet carries Manage) and armed; then the port
// interrupts on a D2H Register FIS and a task-file error, the status IDENTIFY left is acknowledged,
// and the controller's interrupt enabled. A kernel that refuses the Interrupt leaves the driver
// polling its command. Zero, or the failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
ConnectInterrupt(const System::SystemCallClient& client,
                 const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords], AhciDriverState& driver,
                 const Foundation::UInt32 port) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateInterruptRequestSizeBytes]{};
    System::StoreHandle(request + Abi::ObjectCreateInterruptFunctionOffset, function);
    const Abi::SystemCallResponse created =
        client.CreateObject(domain, Abi::ObjectTypeInterrupt, System::AddressOf(request), sizeof(request));
    if (created.Error != Abi::SystemCallError::None)
    {
        return 0U;
    }
    driver.Interrupt = System::CapabilityHandle{created.Value0, created.Value1};
    Abi::SystemCallError connected = client.BindWaitSet(driver.WaitSet, driver.Interrupt, BlockInterruptTag).Error;
    connected = (connected == Abi::SystemCallError::None)
                    ? client.ControlInterrupt(driver.Interrupt, Abi::InterruptRearm).Error
                    : connected;
    if (connected != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverInterruptStep, connected);
    }
    driver.Interrupts = true;
    volatile Foundation::UInt32* const registers = driver.Registers;
    const System::AhciRegisterWriter write = driver.Platform.WriteRegister;
    const Foundation::Size base = System::AhciPortWord(port);
    write(registers, base + System::AhciPortIeWord, AhciDhrs | System::AhciTfes);
    write(registers, base + System::AhciPortIsWord, ~Foundation::UInt32{0U});
    write(registers, System::AhciIsWord, ReadRegister(registers, System::AhciIsWord));
    write(registers, System::AhciGhcWord, ReadRegister(registers, System::AhciGhcWord) | System::AhciIe);
    return 0U;
}

// Brings the controller up: finds the disk, prepares its port,
// identifies the disk and connects the Interrupt. Zero with driver.Port the
// disk's port; zero or DriverWithoutIommu without a disk to serve; or the
// failure's exit code. Registers reading all ones (no controller behind the
// mapping) fail the identification.
[[nodiscard, gnu::noinline]] Foundation::UInt64
BringUp(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
        AhciDriverState& driver) noexcept
{
    const Foundation::UInt32 capabilities = ReadRegister(driver.Registers, System::AhciCapWord);
    const Foundation::UInt32 implemented = ReadRegister(driver.Registers, System::AhciPiWord);
    if ((capabilities == ~Foundation::UInt32{0U}) || (implemented == ~Foundation::UInt32{0U}))
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::None);
    }
    driver.Address64 = (capabilities & System::AhciS64a) != 0U;
    const Foundation::UInt32 port = FindDiskPort(driver.Registers, implemented);
    if (port == MappedPorts)
    {
        return 0U;
    }
    Foundation::UInt64 failure = PreparePort(client, startBlock, driver, port, implemented);
    failure = (failure != 0U) ? failure : IdentifyDisk(driver, port);
    if ((failure == 0U) && (driver.Refusal == BlockUnsupportedSectorSize))
    {
        return 0U;
    }
    failure = (failure != 0U) ? failure : ConnectInterrupt(client, startBlock, driver, port);
    driver.Port = (failure == 0U) ? port : MappedPorts;
    return failure;
}

// Sends the reply of kind and detail answering transaction (a request's or
// a Cancel's) on reply. A reply the kernel refuses is dropped and counted.
void Reply(const System::SystemCallClient& client, AhciDriverState& driver, const System::CapabilityHandle reply,
           const Foundation::UInt64 transaction, const Foundation::UInt64 kind,
           const Foundation::UInt64 detail) noexcept
{
    alignas(8) Foundation::UInt8 frame[BlockReplyBytes]{};
    const Foundation::UInt64 words[BlockPayloadWords]{transaction, kind, detail, 0U};
    const Foundation::Size bytes = WriteBlockFrame(frame, BlockReplyMethod, transaction, words, nullptr, nullptr);
    if (client.Send(reply, System::AddressOf(frame), bytes).Error != Abi::SystemCallError::None)
    {
        ++driver.DroppedReplies;
    }
}

// A Read's or Write's final reply on its reply capability,
// then the Close of that and of its data Frame. False only when a Close
// failed other than on a stale handle.
[[nodiscard, gnu::noinline]] bool Answer(const System::SystemCallClient& client, AhciDriverState& driver,
                                         const System::CapabilityHandle reply, const System::CapabilityHandle data,
                                         const Foundation::UInt64 transaction, const Foundation::UInt64 kind,
                                         const Foundation::UInt64 detail) noexcept
{
    Reply(client, driver, reply, transaction, kind, detail);
    const bool replyClosed = Closed(client, reply);
    return Closed(client, data) && replyClosed;
}

// After a command failed (AHCI 1.3.1 section 6.2.2: the port stops
// processing commands), after CI proved completion, stops and restarts the port
// once the device
// is ready; false when it does not, which leaves the port to its Device
// Manager's reset.
[[nodiscard, gnu::noinline]] bool RestartPort(const AhciDriverState& driver) noexcept
{
    return System::StopAhciPort(driver.Registers, driver.Port, driver.Platform.Clock,
                                driver.SpinTicks << PortBudgetShift, driver.Platform.WriteRegister) &&
           StartPort(driver, driver.Port);
}

// The CPU mapping never enters a 32-bit domain. Only the low bounce Frame is exposed to the
// controller. A failed Read never publishes stale bounce bytes to the client.
[[nodiscard, gnu::noinline]] Abi::SystemCallError CopyClient(const System::SystemCallClient& client,
                                                             const AhciDriverState& driver, const BlockRequest& request,
                                                             const bool toClient) noexcept
{
    const Foundation::UInt64 pages = (request.Sectors + 7U) >> 3U;
    const Foundation::UInt64 address = driver.Window + Abi::AhciClientPage * PageBytes;
    const auto mapped = System::MapWindow(client, driver.Space, driver.ClientMemory, request.Data, address, pages);
    if (mapped != Abi::SystemCallError::None)
    {
        return mapped;
    }
    auto* const data = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(address));
    auto* const bounce = reinterpret_cast<Foundation::UInt8*>(
        static_cast<Foundation::UIntPointer>(driver.Window + Abi::AhciBouncePage * PageBytes));
    for (Foundation::Size byte{0U}; byte < request.Sectors * BlockSectorBytes; ++byte)
    {
        if (toClient)
        {
            data[byte] = bounce[byte];
        }
        else
        {
            bounce[byte] = data[byte];
        }
    }
    return client.UnmapMemory(driver.Space, address, pages).Error;
}

// A cleared CI, without an engine stop during this issued command, ends it. Errors with CI still
// set leave the slot and mapping to the manager's reset. A completed command's data window is
// unmapped, then its final reply sent and capabilities closed. Completed ATA errors restart only
// the engines and preserve the medium generation.
[[nodiscard, gnu::noinline]] Foundation::UInt64 CompleteCommand(const System::SystemCallClient& client,
                                                                AhciDriverState& driver,
                                                                const Foundation::UInt32 status) noexcept
{
    if (!IsBusy(driver))
    {
        return 0U;
    }
    const bool stopped = (status & CommandErrors) != 0U;
    if (!stopped && !System::IsAhciCommandDone(driver.Registers, driver.Port, CommandSlot))
    {
        return 0U;
    }
    const Foundation::UInt32 taskFile =
        ReadRegister(driver.Registers, System::AhciPortWord(driver.Port) + System::AhciTfdWord) & 0xFFFFU;
    const bool failed = stopped || ((taskFile & System::AhciErr) != 0U);
    // Stop/start can clear CI without completing retained DMA. Only restart an error whose command
    // had already ended.
    if (failed && (!System::IsAhciCommandDone(driver.Registers, driver.Port, CommandSlot) || !RestartPort(driver)))
    {
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
    }
    BlockRequest completed{};
    if (driver.Requests->Complete(CommandSlot, completed) != BlockCompleteOutcome::Completed)
    {
        return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
    }
    const Abi::SystemCallError unmapped =
        client.ControlIommu(driver.Iommu, Abi::IommuUnmap, driver.DataDevice, 0U).Error;
    if (unmapped == Abi::SystemCallError::None && !driver.Address64 && !failed && completed.Method == BlockReadMethod)
    {
        System::OrderDmaStorage();
        const auto copied = CopyClient(client, driver, completed, true);
        if (copied != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverMapStep, copied);
        }
    }
    const bool answered = Answer(client, driver, completed.Reply, completed.Data, completed.Transaction,
                                 failed ? BlockFailed : BlockDone, failed ? taskFile : completed.Sectors);
    if (unmapped != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverUnmapStep, unmapped);
    }
    return answered ? 0U : DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
}

// A Read or Write. One for another generation was sent to an instance that
// died: the request is not executed, and answered BlockDriverRestarted.
// Otherwise the table queues it, or refuses it beyond its sender's credit
// (BlockRefused). False only when a Close failed other than on a stale
// handle.
[[nodiscard, gnu::noinline]] bool ServeTransfer(const System::SystemCallClient& client, AhciDriverState& driver,
                                                const Foundation::UInt64 sender, const BlockMessage& message) noexcept
{
    if (message.Words[0U] != driver.Generation)
    {
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, BlockDriverRestarted, 0U);
    }
    if (message.Words[4U] != driver.MediumGeneration)
    {
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, BlockMediumChanged, 0U);
    }
    switch (driver.Requests->Admit(sender, message).Outcome)
    {
    case BlockAdmitOutcome::Queued:
        return true;
    case BlockAdmitOutcome::Refused:
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, BlockRefused,
                      BlockMaximumOutstanding);
    case BlockAdmitOutcome::Malformed:
        break;
    }
    const bool replyClosed = Closed(client, message.Reply);
    return Closed(client, message.Data) && replyClosed;
}

// A Cancel. A Queued request of its sender and generation is
// answered BlockCancelled and leaves the table, its capabilities closed;
// then the Cancel is answered BlockCancelled with that request's
// transaction. Anything else, an issued request or another generation's
// included, answers the Cancel BlockTooLate with the transaction it named.
// The Cancel's own capability is closed after its answer. False only when a
// Close failed other than on a stale handle.
[[nodiscard, gnu::noinline]] bool ServeCancel(const System::SystemCallClient& client, AhciDriverState& driver,
                                              const Foundation::UInt64 sender, const BlockMessage& message) noexcept
{
    BlockRequest cancelled{};
    const BlockCancelOutcome outcome = (message.Words[0U] == driver.Generation)
                                           ? driver.Requests->Cancel(sender, message.Words[1U], cancelled)
                                           : BlockCancelOutcome::TooLate;
    bool closed{true};
    if (outcome == BlockCancelOutcome::Cancelled)
    {
        closed = Answer(client, driver, cancelled.Reply, cancelled.Data, cancelled.Transaction, BlockCancelled, 0U);
        Reply(client, driver, message.Reply, message.Transaction, BlockCancelled, cancelled.Transaction);
    }
    else
    {
        Reply(client, driver, message.Reply, message.Transaction, BlockTooLate, message.Words[1U]);
    }
    return Closed(client, message.Reply) && closed;
}

// Describe takes no I/O credit and never maps a client data Frame.
[[nodiscard, gnu::noinline]] bool Describe(const System::SystemCallClient& client, AhciDriverState& driver,
                                           const BlockMessage& message) noexcept
{
    if (message.Words[0U] != driver.Generation)
    {
        Reply(client, driver, message.Reply, message.Transaction, BlockDriverRestarted, 0U);
    }
    else if (driver.Refusal == BlockUnsupportedSectorSize)
    {
        Reply(client, driver, message.Reply, message.Transaction, BlockFailed, driver.Refusal);
    }
    else
    {
        alignas(8) Foundation::UInt8 frame[BlockReplyBytes]{};
        const Foundation::UInt64 words[BlockPayloadWords]{BlockSectorBytes, HasDisk(driver) ? driver.Capacity : 0U,
                                                          HasDisk(driver) ? BlockMediumPresent : 0U,
                                                          driver.MediumGeneration};
        const Foundation::Size bytes =
            WriteBlockFrame(frame, BlockDescribeReplyMethod, message.Transaction, words, nullptr, nullptr);
        if (client.Send(message.Reply, System::AddressOf(frame), bytes).Error != Abi::SystemCallError::None)
        {
            ++driver.DroppedReplies;
        }
    }
    return Closed(client, message.Reply);
}

[[nodiscard, gnu::noinline]] bool FlushDevice(const System::SystemCallClient& client, AhciDriverState& driver,
                                              const BlockMessage& message) noexcept
{
    Foundation::UInt64 kind{BlockRefused};
    Foundation::UInt64 detail{0U};
    if (message.Words[0U] != driver.Generation)
    {
        kind = BlockDriverRestarted;
    }
    else if (HasDisk(driver) && driver.Requests->IsIdle() &&
             System::BuildAhciFlush(driver.DmaPage, CommandSlot,
                                    driver.DmaPageDevice + System::AhciCommandTablesOffset))
    {
        System::OrderDmaStorage();
        System::IssueAhciCommand(driver.Registers, driver.Port, CommandSlot, driver.Platform.WriteRegister);
        const auto start = driver.Platform.Clock();
        const auto budget = driver.SpinTicks << IdentifyBudgetShift;
        kind = BlockFailed;
        detail = System::AhciTfes;
        for (Foundation::Size poll{0U}; poll < 20'000'000U; ++poll)
        {
            if (System::AhciCommandFailed(driver.Registers, driver.Port))
            {
                break;
            }
            if (System::IsAhciCommandDone(driver.Registers, driver.Port, CommandSlot))
            {
                kind = BlockDone;
                detail = 0U;
                break;
            }
            if (driver.Platform.Clock() - start >= budget)
            {
                break;
            }
        }
        System::OrderDmaStorage();
    }
    Reply(client, driver, message.Reply, message.Transaction, kind, detail);
    const bool closed = Closed(client, message.Reply);
    // A failed Flush has not released slot 0. End this incarnation before draining another
    // delivery; the manager's reset proves the command and its DMA page quiet before reassignment.
    return closed && (kind != BlockFailed);
}

// One delivery of deliveryBytes in the buffer. A frame that is no Read,
// Write or Cancel of the block protocol through the sealed Endpoint is
// dropped, every capability it carried closed: its reply capability cannot
// be trusted. False only when a Close failed other than on a stale handle.
[[nodiscard, gnu::noinline]] bool ServeDelivery(const System::SystemCallClient& client, AhciDriverState& driver,
                                                const Foundation::Size deliveryBytes) noexcept
{
    BlockMessage message{};
    if (!ReadBlockFrame(g_delivery, deliveryBytes, message) || (message.Method == BlockReplyMethod) ||
        (message.Method == BlockDescribeReplyMethod) || (ReadEchoSealedSelector(g_delivery) != BlockSealedSelector))
    {
        return CloseAttachments(client, g_delivery, 0U);
    }
    if (message.Method == BlockFlushMethod)
    {
        return FlushDevice(client, driver, message);
    }
    if (message.Method == BlockDescribeMethod)
    {
        return Describe(client, driver, message);
    }
    const Foundation::UInt64 sender = System::LoadWord(g_delivery + DeliverySenderOffset);
    return (message.Method == BlockCancelMethod) ? ServeCancel(client, driver, sender, message)
                                                 : ServeTransfer(client, driver, sender, message);
}

// Receives and serves every delivery the Endpoint holds, in order. Zero, or
// the exit code of a Receive or Close that failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 DrainEndpoint(const System::SystemCallClient& client,
                                                              AhciDriverState& driver) noexcept
{
    for (;;)
    {
        const Abi::SystemCallResponse received =
            client.Receive(driver.Endpoint, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            return 0U;
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverServeStep, received.Error);
        }
        if (!ServeDelivery(client, driver, static_cast<Foundation::Size>(received.Value1)))
        {
            return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
        }
    }
}

// Builds request's command in slot 0 and maps its Frame in the data window,
// for the device to write (a Read) or read (a Write). Zero, or the
// BlockFailed detail of a request the driver cannot issue: no disk, sectors
// beyond the capacity, or an IommuMap the kernel refused (a Frame smaller
// than the transfer, say), its error in the low byte.
[[nodiscard, gnu::noinline]] Foundation::UInt64
PrepareTransfer(const System::SystemCallClient& client, AhciDriverState& driver, const BlockRequest& request) noexcept
{
    if (!HasDisk(driver))
    {
        return driver.Refusal;
    }
    const Foundation::UInt32 sectors = request.Sectors;
    const bool write = request.Method == BlockWriteMethod;
    // What BuildAhciReadWrite refuses of a request, checked before the
    // mapping it needs the device address of: no sectors, more than one PRD
    // holds (128), an LBA beyond 48 bits.
    if ((request.Lba > driver.Capacity) || (sectors > (driver.Capacity - request.Lba)) || (sectors == 0U) ||
        (sectors > 128U) || ((request.Lba >> 48U) != 0U))
    {
        return BlockOutOfRange;
    }
    if (!driver.Address64)
    {
        // Validate even a Read's client extent before issuing DMA.
        const Foundation::UInt64 address = driver.Window + Abi::AhciClientPage * PageBytes;
        const Foundation::UInt64 pages = (sectors + 7U) >> 3U;
        const auto checked =
            write ? CopyClient(client, driver, request, false)
                  : System::MapWindow(client, driver.Space, driver.ClientMemory, request.Data, address, pages);
        if (checked != Abi::SystemCallError::None)
        {
            return BlockMapRefused | static_cast<Foundation::UInt64>(checked);
        }
        if (!write && client.UnmapMemory(driver.Space, address, pages).Error != Abi::SystemCallError::None)
        {
            return BlockMapRefused;
        }
    }
    const DeviceMapping mapped =
        MapForDevice(client, driver.Iommu, driver.Address64 ? request.Data : driver.BounceFrame, (sectors + 7U) >> 3U,
                     DataWindow, write ? Abi::IommuMapDeviceRead : Abi::IommuMapDeviceWrite);
    if (mapped.Error != Abi::SystemCallError::None)
    {
        return BlockMapRefused | (static_cast<Foundation::UInt64>(mapped.Error) & 0xFFU);
    }
    driver.DataDevice = mapped.Address;
    if (!System::BuildAhciReadWrite(driver.DmaPage, CommandSlot, write, request.Lba, sectors, mapped.Address,
                                    sectors << 9U, driver.DmaPageDevice + System::AhciCommandTablesOffset))
    {
        // Unreachable after the check above; the mapping goes again.
        static_cast<void>(client.ControlIommu(driver.Iommu, Abi::IommuUnmap, mapped.Address, 0U));
        return BlockOutOfRange;
    }
    return 0U;
}

// With no command in flight, issues the oldest Queued request. The loop
// drained the Endpoint first, so a Cancel that arrived with its request
// found it still Queued. A request the driver cannot issue is
// answered BlockFailed at once and the next one tried. One with
// BlockCrashAfterIssue crashes the instance right after its command issued,
// the device transferring into the client's Frame. Zero, or the
// exit code of a failed Close or of a crash that returned.
[[nodiscard, gnu::noinline]] Foundation::UInt64 IssueQueued(const System::SystemCallClient& client,
                                                            AhciDriverState& driver) noexcept
{
    BlockRequest request{};
    while (!IsBusy(driver) && driver.Requests->NextToIssue(request))
    {
        const bool changed = request.MediumGeneration != driver.MediumGeneration;
        const Foundation::UInt64 refusal = changed ? BlockMediumChanged : PrepareTransfer(client, driver, request);
        if (refusal == 0U)
        {
            System::OrderDmaStorage();
            System::IssueAhciCommand(driver.Registers, driver.Port, CommandSlot, driver.Platform.WriteRegister);
            if ((request.Flags & BlockCrashAfterIssue) != 0U)
            {
                driver.Platform.Crash(driver.CrashKind);
                return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::None);
            }
            return 0U;
        }
        BlockRequest refused{};
        if ((driver.Requests->Complete(CommandSlot, refused) != BlockCompleteOutcome::Completed) ||
            !Answer(client, driver, refused.Reply, refused.Data, refused.Transaction,
                    changed ? BlockMediumChanged : BlockFailed, changed ? 0U : refusal))
        {
            return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
        }
    }
    return 0U;
}

// The interrupt woke the driver: acknowledges the port status the loop read
// (write one to clear exactly those bits, so an error that arrives meanwhile
// stays for the next pass), then the controller's, and rearms the Interrupt.
// A message that arrived while it was masked is delivered at the arm.
[[nodiscard, gnu::noinline]] Abi::SystemCallError AcknowledgeInterrupt(const System::SystemCallClient& client,
                                                                       const AhciDriverState& driver,
                                                                       const Foundation::UInt32 status) noexcept
{
    volatile Foundation::UInt32* const registers = driver.Registers;
    const System::AhciRegisterWriter write = driver.Platform.WriteRegister;
    write(registers, System::AhciPortWord(driver.Port) + System::AhciPortIsWord, status);
    write(registers, System::AhciIsWord, ReadRegister(registers, System::AhciIsWord));
    return client.ControlInterrupt(driver.Interrupt, Abi::InterruptRearm).Error;
}

// The service loop. Its order is an invariant: first the
// interrupt and the command in flight, then every delivery the Endpoint
// holds, then the next command; all three on every wake, whichever tag woke
// it. With a command in flight the wait ends after CommandPollMicroseconds.
// Returns only a failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ServeBlocks(const System::SystemCallClient& client,
                                                            AhciDriverState& driver) noexcept
{
    for (;;)
    {
        const bool busy = IsBusy(driver);
        const Abi::SystemCallResponse waited = client.Wait(driver.WaitSet, busy ? CommandPollMicroseconds : 0U);
        const bool ready = waited.Error == Abi::SystemCallError::None;
        const bool interrupt = ready && driver.Interrupts && (waited.Value0 == BlockInterruptTag);
        if (!interrupt && !(ready && (waited.Value0 == BlockServiceWaitTag)) &&
            !(busy && (waited.Error == Abi::SystemCallError::TimedOut)))
        {
            return DriverFailure(Abi::ComponentDriverServeStep, waited.Error);
        }
        const Foundation::UInt32 status =
            HasDisk(driver) ? ReadRegister(driver.Registers, System::AhciPortWord(driver.Port) + System::AhciPortIsWord)
                            : 0U;
        const Abi::SystemCallError rearmed =
            interrupt ? AcknowledgeInterrupt(client, driver, status) : Abi::SystemCallError::None;
        if (rearmed != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverInterruptStep, rearmed);
        }
        Foundation::UInt64 failure = CompleteCommand(client, driver, status);
        failure = (failure != 0U) ? failure : DrainEndpoint(client, driver);
        failure = (failure != 0U) ? failure : IssueQueued(client, driver);
        if (failure != 0U)
        {
            return failure;
        }
    }
}
} // Namespace

Foundation::UInt64 ServeAhciDevice(const System::SystemCallClient& client,
                                   const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                   const AhciDriverPlatform& platform) noexcept
{
    g_requests.Clear();
    g_driver = AhciDriverState{
        .Registers = RegistersOf(startBlock),
        .DmaPage = reinterpret_cast<Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(startBlock[Abi::ComponentWindowStartWord] + PageBytes)),
        .Platform = platform,
        .Requests = &g_requests,
        .Endpoint = {startBlock[Abi::ComponentReceiveWord], startBlock[Abi::ComponentReceiveWord + 1U]},
        .WaitSet = {startBlock[Abi::ComponentWaitSetWord], startBlock[Abi::ComponentWaitSetWord + 1U]},
        .Iommu = {},
        .Interrupt = {},
        .Generation = startBlock[Abi::ComponentIncarnationWord],
        .CrashKind = startBlock[Abi::ComponentCrashKindWord],
        .SpinTicks = startBlock[Abi::ComponentSpinTicksWord],
        .Capacity = 0U,
        .MediumGeneration = startBlock[Abi::ComponentIncarnationWord],
        .Refusal = BlockNoDevice,
        .DroppedReplies = 0U,
        .DmaPageDevice = 0U,
        .DataDevice = 0U,
        .Domain = {startBlock[Abi::ComponentAccountWord], startBlock[Abi::ComponentAccountWord + 1U]},
        .Space = {startBlock[Abi::ComponentAccountSpaceWord], startBlock[Abi::ComponentAccountSpaceWord + 1U]},
        .BounceFrame = {},
        .ClientMemory = {},
        .Window = startBlock[Abi::ComponentWindowStartWord],
        .Address64 = false,
        .Port = MappedPorts,
        .Interrupts = false};
    const Foundation::UInt64 mapped = MapRegisters(client, startBlock);
    if (mapped != 0U)
    {
        return mapped;
    }
    const Foundation::UInt64 failure = BringUp(client, startBlock, g_driver);
    if ((failure != 0U) && (failure != DriverWithoutIommu))
    {
        return failure;
    }
    return ServeBlocks(client, g_driver);
}
} // Namespace Asteria::User::Component
