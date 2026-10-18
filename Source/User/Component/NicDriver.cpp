// SPDX-License-Identifier: Apache-2.0

#include "User/Component/NicDriver.hpp"

#include "Abi/Clock.hpp"

#include "Abi/SystemCallAbi.hpp"
#include "User/Component/DriverSetup.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/NetFrame.hpp"
#include "User/Component/ServiceCapabilities.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
namespace
{
// The rings: 16 legacy descriptors each in the descriptor page, receive at offset 0 and transmit at
// TransmitRingOffset; every receive descriptor owns the receive buffer of its index, a transmit
// descriptor the transmit buffer of its index modulo TransmitBuffers, so at most that many frames
// are in flight. Every buffer is 2 KiB at offset 0 or 2048 of its page, so no DMA access crosses a
// page.
constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::Size RingDescriptors{16U};
constexpr Foundation::Size TransmitRingOffset{2048U};
constexpr Foundation::Size BufferBytes{2048U};
constexpr Foundation::Size TransmitBuffers{8U};
constexpr Foundation::UInt32 RingBytes{RingDescriptors * System::E1000eDescriptorBytes};
static_assert((RingBytes % 128U) == 0U, "RDLEN and TDLEN are multiples of 128 bytes");
static_assert((RingDescriptors * BufferBytes) == (Abi::NicReceiveBufferPages * PageBytes));
static_assert((TransmitBuffers * BufferBytes) == (Abi::NicTransmitBufferPages * PageBytes));
// The causes the device signals: a frame received, the receive
// ring running low, a transmit descriptor written back, a link change.
constexpr Foundation::UInt32 SignalledCauses{System::E1000eIntRxt0 | System::E1000eIntRxdmt0 | System::E1000eIntTxdw |
                                             System::E1000eIntLsc};
constexpr Foundation::UInt8 TransmitCommand{System::E1000eCommandEop | System::E1000eCommandIfcs |
                                            System::E1000eCommandRs};
// The wait's backstop: a lost interrupt costs at most this long.
constexpr Foundation::UInt64 BackstopMicroseconds{10'000U};
// The clock budgets, in multiples of the block's tick count (three time
// Slices): the reset clears and the link comes up within 16.
constexpr Foundation::UInt64 DeviceBudgetShift{4U};
// A delivery's sender identity (IpcDeliveryCodec), its client's credit key.
constexpr Foundation::Size DeliverySenderOffset{24U};
// The requests the driver holds: two clients' full credit.
constexpr Foundation::Size RequestCapacity{16U};

// Where a request is: a ReceiveCredit waiting for a frame, a Send waiting for
// a transmit buffer, or a Send whose frame is in the ring (on Descriptor).
enum class NicRequestState : Foundation::UInt8
{
    Free = 0U,
    Credit = 1U,
    Queued = 2U,
    Sending = 3U,
};

// One request. Sender is its delivery's sender identity; Reply and Data are
// the handles in the driver's space, Data closed (zero) once a Send's frame
// is copied; Arrival orders the requests of a state (a wrapping counter).
struct NicRequest
{
    Foundation::UInt64 Sender;
    Foundation::UInt64 Transaction;
    System::CapabilityHandle Reply;
    System::CapabilityHandle Data;
    Foundation::UInt32 Arrival;
    Foundation::UInt16 Length;
    NicRequestState State;
    Foundation::UInt8 Descriptor;
};

// The instance: its device and window, what its image gave it, its
// capabilities, its rings' positions and its counters. Device is false on a
// kernel without an IOMMU, which serves every Send and ReceiveCredit
// NicFailed.
struct NicDriverState
{
    volatile Foundation::UInt32* Registers;
    Foundation::UInt8* Window;
    NicDriverPlatform Platform;
    System::CapabilityHandle Endpoint;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Space;
    System::CapabilityHandle Iommu;
    System::CapabilityHandle Interrupt;
    System::CapabilityHandle ClientMemory;
    Foundation::UInt64 Generation;
    Foundation::UInt64 SpinTicks;
    Foundation::UInt64 Mac;
    // Frames that arrived with no credit outstanding or
    // spoiled, and replies the kernel refused (a client that broke its
    // credit): dropped and counted.
    Foundation::UInt64 DroppedFrames;
    Foundation::UInt64 DroppedReplies;
    // The device addresses of the descriptor page and the receive and transmit buffers, as IommuMap
    // answered them (Abi::Nic*Device asked for; the physical ones on the trusted-driver profile,
    // Documentation/Architecture/IommuDomain.md ).
    Foundation::UInt64 DescriptorDevice;
    Foundation::UInt64 ReceiveDevice;
    Foundation::UInt64 TransmitDevice;
    Foundation::UInt32 NextArrival;
    Foundation::UInt8 ReceiveNext;
    Foundation::UInt8 TransmitClean;
    Foundation::UInt8 TransmitTail;
    bool Device;
};

// The instance's state, its requests and the buffer it receives into:
// static, not on its stack, zero-initialized (the image's.data stays
// empty).
constinit NicRequest g_requests[RequestCapacity]{};
constinit NicDriverState g_driver{};
alignas(8) constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};

[[nodiscard]] Foundation::UInt32 Read(const NicDriverState& driver, const Foundation::Size word) noexcept
{
    return driver.Platform.ReadRegister(driver.Registers, word);
}

void Write(const NicDriverState& driver, const Foundation::Size word, const Foundation::UInt32 value) noexcept
{
    driver.Platform.WriteRegister(driver.Registers, word, value);
}

[[nodiscard]] Foundation::UInt8* WindowPage(const NicDriverState& driver, const Foundation::UInt64 page) noexcept
{
    return driver.Window + (page * PageBytes);
}

// A descriptor the device writes back, read and cleared through volatile
// accesses.
[[nodiscard]] volatile Foundation::UInt8* ReceiveDescriptor(const NicDriverState& driver,
                                                            const Foundation::Size index) noexcept
{
    return WindowPage(driver, Abi::NicDescriptorPage) + (index * System::E1000eDescriptorBytes);
}

[[nodiscard]] volatile Foundation::UInt8* TransmitDescriptor(const NicDriverState& driver,
                                                             const Foundation::Size index) noexcept
{
    return WindowPage(driver, Abi::NicDescriptorPage) + TransmitRingOffset + (index * System::E1000eDescriptorBytes);
}

void StoreDescriptorWord(volatile Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<Foundation::UInt8>(value >> (index << 3U));
    }
}

[[nodiscard]] bool IsHandle(const System::CapabilityHandle handle) noexcept
{
    return (handle.Selector != 0U) || (handle.Authenticator != 0U);
}

// First arrived before second; correct across the counter's wrap while fewer than 2^31 arrivals
// separate them (at most RequestCapacity are held).
[[nodiscard]] constexpr bool Precedes(const Foundation::UInt32 first, const Foundation::UInt32 second) noexcept
{
    return static_cast<Foundation::UInt32>(first - second) >= 0x8000'0000U;
}

// The oldest request in state, or RequestCapacity.
[[nodiscard]] Foundation::Size Oldest(const NicRequestState state) noexcept
{
    Foundation::Size oldest{RequestCapacity};
    for (Foundation::Size index{0U}; index < RequestCapacity; ++index)
    {
        if ((g_requests[index].State == state) &&
            ((oldest == RequestCapacity) || Precedes(g_requests[index].Arrival, g_requests[oldest].Arrival)))
        {
            oldest = index;
        }
    }
    return oldest;
}

// The frames in the transmit ring.
[[nodiscard]] Foundation::Size InFlight(const NicDriverState& driver) noexcept
{
    return (static_cast<Foundation::Size>(driver.TransmitTail) + RingDescriptors -
            static_cast<Foundation::Size>(driver.TransmitClean)) &
           (RingDescriptors - 1U);
}

// Polls word until its masked bits equal expected, within budget ticks of
// the clock; false when they did not.
[[nodiscard]] bool AwaitRegister(const NicDriverState& driver, const Foundation::Size word,
                                 const Foundation::UInt32 mask, const Foundation::UInt32 expected,
                                 const Foundation::UInt64 budget) noexcept
{
    const Foundation::UInt64 start = driver.Platform.Clock();
    while ((Read(driver, word) & mask) != expected)
    {
        if ((Abi::ElapsedClockTicks(driver.Platform.Clock(), start)) >= budget)
        {
            return false;
        }
    }
    return true;
}

// Takes the device from whoever left it (a dead instance, the firmware):
// every cause masked, CTRL.RST until it clears, masked again and the causes
// read (ICR clears as it is read); then the link set up and awaited (a link
// still down is not a failure: Query reports it), and the MAC address the
// reset restored read from RAL0 and RAH0. Registers reading all ones (no
// device behind the mapping) fail the identification, a reset that does not
// clear the quiesce.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ResetDevice(NicDriverState& driver) noexcept
{
    if (Read(driver, System::E1000eStatusWord) == ~Foundation::UInt32{0U})
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::None);
    }
    const Foundation::UInt64 budget = driver.SpinTicks << DeviceBudgetShift;
    Write(driver, System::E1000eImcWord, ~Foundation::UInt32{0U});
    Write(driver, System::E1000eCtrlWord, Read(driver, System::E1000eCtrlWord) | System::E1000eCtrlRst);
    if (!AwaitRegister(driver, System::E1000eCtrlWord, System::E1000eCtrlRst, 0U, budget))
    {
        return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::None);
    }
    Write(driver, System::E1000eImcWord, ~Foundation::UInt32{0U});
    static_cast<void>(Read(driver, System::E1000eIcrWord));
    Write(driver, System::E1000eCtrlWord,
          Read(driver, System::E1000eCtrlWord) | System::E1000eCtrlSlu | System::E1000eCtrlAsde);
    static_cast<void>(
        AwaitRegister(driver, System::E1000eStatusWord, System::E1000eStatusLu, System::E1000eStatusLu, budget));
    const Foundation::UInt32 low = Read(driver, System::E1000eRal0Word);
    const Foundation::UInt32 high = Read(driver, System::E1000eRah0Word) & System::E1000eRahAddressMask;
    driver.Mac = static_cast<Foundation::UInt64>(low) | (static_cast<Foundation::UInt64>(high) << 32U);
    return 0U;
}

// One run of DMA pages: its first window page, its pages, its device
// address and what the device may do there.
struct DmaRun
{
    Foundation::UInt64 Page;
    Foundation::UInt64 Pages;
    Foundation::UInt64 Device;
    Foundation::UInt8 Permissions;
};

// Confines the function in an IommuDomain, creates the descriptor page and the buffers as Frames of
// the driver's own domain, maps each in the window and for the device (the device writes the
// Receive buffers and reads the transmit buffers), creates the MemoryObject a client's Frame is
// mapped through, and lets the function master the bus. Zero, DriverWithoutIommu, or the failure's
// exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
PrepareDma(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
           NicDriverState& driver) noexcept
{
    const Foundation::UInt64 confined =
        ConfineFunction(client, startBlock, driver.Iommu, 64U);
    if (confined != 0U)
    {
        return confined;
    }
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle function{startBlock[Abi::ComponentDeviceFunctionWord],
                                            startBlock[Abi::ComponentDeviceFunctionWord + 1U]};
    constexpr DmaRun runs[3U]{
        {.Page = Abi::NicDescriptorPage,
         .Pages = 1U,
         .Device = Abi::NicDescriptorDevice,
         .Permissions = Abi::IommuMapDeviceRead | Abi::IommuMapDeviceWrite},
        {.Page = Abi::NicReceiveBufferPage,
         .Pages = Abi::NicReceiveBufferPages,
         .Device = Abi::NicReceiveBufferDevice,
         .Permissions = Abi::IommuMapDeviceWrite},
        {.Page = Abi::NicTransmitBufferPage,
         .Pages = Abi::NicTransmitBufferPages,
         .Device = Abi::NicTransmitBufferDevice,
         .Permissions = Abi::IommuMapDeviceRead},
    };
    Foundation::UInt64* const devices[3U]{&driver.DescriptorDevice, &driver.ReceiveDevice, &driver.TransmitDevice};
    for (Foundation::Size index{0U}; index < 3U; ++index)
    {
        const DmaRun& run = runs[index];
        System::CapabilityHandle frame{};
        const Abi::SystemCallError mapped =
            MapDmaPage(client, domain, driver.Space, System::AddressOf(WindowPage(driver, run.Page)), frame, run.Pages);
        if (mapped != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverPagesStep, mapped);
        }
        const DeviceMapping exposed = MapForDevice(client, driver.Iommu, frame, run.Pages, run.Device, run.Permissions);
        if (exposed.Error != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverDeviceMapStep, exposed.Error);
        }
        *devices[index] = exposed.Address;
    }
    const Abi::SystemCallResponse memory = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, 1U);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMemoryObjectStep, memory.Error);
    }
    driver.ClientMemory = System::CapabilityHandle{memory.Value0, memory.Value1};
    const Abi::SystemCallError mastering = client.ControlDevice(function, Abi::DeviceEnableBusMaster).Error;
    return (mastering == Abi::SystemCallError::None) ? 0U : DriverFailure(Abi::ComponentDriverMasterStep, mastering);
}

// Programs the rings (82574L section 14: legacy descriptors, RFCTL.EXTEN
// clear) and enables transmit, then receive: every receive descriptor holds
// its buffer's device address, the device owns all but one (RDT = 15), no
// transmit descriptor is posted; no interrupt delay or throttle. The device
// then runs.
[[gnu::noinline]] void StartRings(NicDriverState& driver) noexcept
{
    for (Foundation::Size index{0U}; index < RingDescriptors; ++index)
    {
        volatile Foundation::UInt8* const receive = ReceiveDescriptor(driver, index);
        volatile Foundation::UInt8* const transmit = TransmitDescriptor(driver, index);
        StoreDescriptorWord(receive, driver.ReceiveDevice + (index * BufferBytes));
        StoreDescriptorWord(receive + 8U, 0U);
        StoreDescriptorWord(transmit, 0U);
        StoreDescriptorWord(transmit + 8U, 0U);
    }
    System::OrderDmaStorage();
    const Foundation::UInt64 transmitRing{driver.DescriptorDevice + TransmitRingOffset};
    Write(driver, System::E1000eRfctlWord, Read(driver, System::E1000eRfctlWord) & ~System::E1000eRfctlExten);
    Write(driver, System::E1000eItrWord, 0U);
    Write(driver, System::E1000eRdtrWord, 0U);
    Write(driver, System::E1000eRadvWord, 0U);
    // Intel 82574 sections 10.2.5.11 and 10.2.6.11: zero WTHRESH writes
    // descriptors back immediately; nonzero WTHRESH requires nonzero timers.
    // RXDCTL reserved bits stay zero. TXDCTL bit 22 is explicitly required.
    Write(driver, System::E1000eRxdctlWord, 0U);
    Write(driver, System::E1000eTxdctlWord, System::E1000eTxdctlRequired);
    Write(driver, System::E1000eRdbalWord, static_cast<Foundation::UInt32>(driver.DescriptorDevice & 0xFFFF'FFFFU));
    Write(driver, System::E1000eRdbahWord, static_cast<Foundation::UInt32>(driver.DescriptorDevice >> 32U));
    Write(driver, System::E1000eRdlenWord, RingBytes);
    Write(driver, System::E1000eRdhWord, 0U);
    Write(driver, System::E1000eRdtWord, static_cast<Foundation::UInt32>(RingDescriptors - 1U));
    Write(driver, System::E1000eTdbalWord, static_cast<Foundation::UInt32>(transmitRing & 0xFFFF'FFFFU));
    Write(driver, System::E1000eTdbahWord, static_cast<Foundation::UInt32>(transmitRing >> 32U));
    Write(driver, System::E1000eTdlenWord, RingBytes);
    Write(driver, System::E1000eTdhWord, 0U);
    Write(driver, System::E1000eTdtWord, 0U);
    Write(driver, System::E1000eTctlWord,
          System::E1000eTctlEn | System::E1000eTctlPsp | System::E1000eTctlCt | System::E1000eTctlCold);
    Write(driver, System::E1000eRctlWord, System::E1000eRctlEn | System::E1000eRctlBam | System::E1000eRctlSecrc);
    driver.ReceiveNext = 0U;
    driver.TransmitClean = 0U;
    driver.TransmitTail = 0U;
    driver.Device = true;
}

// An Interrupt for the function's MSI in the driver's own domain, bound to the service WaitSet
// beside its Endpoint and armed; then the causes the bring-up left are read and the device's causes
// unmasked, so the first message finds the Interrupt armed. A kernel that refuses the Interrupt
// ends the instance: receive is interrupt-driven, the backstop no silent poll. Zero, or the
// failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
ConnectInterrupt(const System::SystemCallClient& client,
                 const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords], NicDriverState& driver) noexcept
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
        return DriverFailure(Abi::ComponentDriverInterruptStep, created.Error);
    }
    driver.Interrupt = System::CapabilityHandle{created.Value0, created.Value1};
    Abi::SystemCallError connected = client.BindWaitSet(driver.WaitSet, driver.Interrupt, NicInterruptTag).Error;
    connected = (connected == Abi::SystemCallError::None)
                    ? client.ControlInterrupt(driver.Interrupt, Abi::InterruptRearm).Error
                    : connected;
    if (connected != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverInterruptStep, connected);
    }
    static_cast<void>(Read(driver, System::E1000eIcrWord));
    Write(driver, System::E1000eImsWord, SignalledCauses);
    return 0U;
}

// Sends the reply of kind, detail and extra answering transaction on reply.
// A reply the kernel refuses is dropped and counted.
void Reply(const System::SystemCallClient& client, NicDriverState& driver, const System::CapabilityHandle reply,
           const Foundation::UInt64 transaction, const Foundation::UInt64 kind, const Foundation::UInt64 detail,
           const Foundation::UInt64 extra) noexcept
{
    alignas(8) Foundation::UInt8 frame[NicReplyBytes]{};
    const Foundation::UInt64 words[NicPayloadWords]{transaction, kind, detail, extra};
    const Foundation::Size bytes = WriteNicFrame(frame, NicReplyMethod, transaction, words, nullptr, nullptr);
    if (client.Send(reply, System::AddressOf(frame), bytes).Error != Abi::SystemCallError::None)
    {
        ++driver.DroppedReplies;
    }
}

// A request's reply on its reply capability, then the Close of that and of
// its data Frame, if it still holds one. False only when a Close failed
// other than on a stale handle.
[[nodiscard, gnu::noinline]] bool Answer(const System::SystemCallClient& client, NicDriverState& driver,
                                         const System::CapabilityHandle reply, const System::CapabilityHandle data,
                                         const Foundation::UInt64 transaction, const Foundation::UInt64 kind,
                                         const Foundation::UInt64 detail) noexcept
{
    Reply(client, driver, reply, transaction, kind, detail, 0U);
    const bool replyClosed = Closed(client, reply);
    return (!IsHandle(data) || Closed(client, data)) && replyClosed;
}

// What a sender holds: its Sends (queued or in the ring), its credits, and
// whether one has transaction; and a free entry, or RequestCapacity.
struct HeldRequests
{
    Foundation::Size Sends;
    Foundation::Size Credits;
    Foundation::Size Free;
    bool Duplicate;
};

[[nodiscard]] HeldRequests Inspect(const Foundation::UInt64 sender, const Foundation::UInt64 transaction) noexcept
{
    HeldRequests held{.Sends = 0U, .Credits = 0U, .Free = RequestCapacity, .Duplicate = false};
    for (Foundation::Size index{0U}; index < RequestCapacity; ++index)
    {
        const NicRequest& entry = g_requests[index];
        if (entry.State == NicRequestState::Free)
        {
            held.Free = index;
        }
        else if (entry.Sender == sender)
        {
            held.Credits += (entry.State == NicRequestState::Credit) ? 1U : 0U;
            held.Sends += (entry.State == NicRequestState::Credit) ? 0U : 1U;
            held.Duplicate = held.Duplicate || (entry.Transaction == transaction);
        }
    }
    return held;
}

// A Query (it counts against the Send credit, so the client's reply Endpoint
// holds its answer): Done with the MAC address, the link state and the
// frames dropped. False only when a Close failed other than on a stale
// handle.
[[nodiscard, gnu::noinline]] bool ServeQuery(const System::SystemCallClient& client, NicDriverState& driver,
                                             const Foundation::UInt64 sender, const NicMessage& message) noexcept
{
    const HeldRequests held = Inspect(sender, message.Transaction);
    if (held.Duplicate || (held.Sends >= NicMaximumSends))
    {
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, NicRefused, NicMaximumSends);
    }
    const Foundation::UInt64 link = ((Read(driver, System::E1000eStatusWord) & System::E1000eStatusLu) != 0U) ? 1U : 0U;
    const Foundation::UInt64 dropped = (driver.DroppedFrames > 0xFFFF'FFFFU) ? 0xFFFF'FFFFU : driver.DroppedFrames;
    Reply(client, driver, message.Reply, message.Transaction, NicDone, driver.Mac, link | (dropped << 32U));
    return Closed(client, message.Reply);
}

// A Send or ReceiveCredit of the running generation: queued, or refused
// beyond its sender's credit of its class, for a transaction the sender has
// outstanding, or with the table full (NicRefused with the credit). Without
// a device it is answered NicFailed with NicNoDevice. False only when a
// Close failed other than on a stale handle.
[[nodiscard, gnu::noinline]] bool Admit(const System::SystemCallClient& client, NicDriverState& driver,
                                        const Foundation::UInt64 sender, const NicMessage& message) noexcept
{
    if (!driver.Device)
    {
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, NicFailed, NicNoDevice);
    }
    const bool credit = message.Method == NicReceiveMethod;
    const Foundation::Size limit = credit ? NicMaximumReceiveCredits : NicMaximumSends;
    const HeldRequests held = Inspect(sender, message.Transaction);
    if (held.Duplicate || ((credit ? held.Credits : held.Sends) >= limit) || (held.Free == RequestCapacity))
    {
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, NicRefused, limit);
    }
    g_requests[held.Free] = NicRequest{.Sender = sender,
                                       .Transaction = message.Transaction,
                                       .Reply = message.Reply,
                                       .Data = message.Data,
                                       .Arrival = driver.NextArrival,
                                       .Length = static_cast<Foundation::UInt16>(credit ? 0U : message.Words[1U]),
                                       .State = credit ? NicRequestState::Credit : NicRequestState::Queued,
                                       .Descriptor = 0U};
    ++driver.NextArrival;
    return true;
}

// One delivery of deliveryBytes in the buffer. A frame that is no request of
// the frame protocol through the sealed Endpoint is dropped, every
// capability it carried closed: its reply capability cannot be trusted. One
// for another generation reached an instance that is not the one its client
// knows: NicDriverRestarted. False only when a Close failed other than on a
// stale handle.
[[nodiscard, gnu::noinline]] bool ServeDelivery(const System::SystemCallClient& client, NicDriverState& driver,
                                                const Foundation::Size deliveryBytes) noexcept
{
    NicMessage message{};
    if (!ReadNicFrame(g_delivery, deliveryBytes, message) || (message.Method == NicReplyMethod) ||
        (ReadEchoSealedSelector(g_delivery) != NicSealedSelector))
    {
        return CloseAttachments(client, g_delivery, 0U);
    }
    if (message.Words[0U] != driver.Generation)
    {
        return Answer(client, driver, message.Reply, message.Data, message.Transaction, NicDriverRestarted, 0U);
    }
    const Foundation::UInt64 sender = System::LoadWord(g_delivery + DeliverySenderOffset);
    return (message.Method == NicQueryMethod) ? ServeQuery(client, driver, sender, message)
                                              : Admit(client, driver, sender, message);
}

// Receives and serves every delivery the Endpoint holds, in order. Zero, or
// the exit code of a Receive or Close that failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 DrainEndpoint(const System::SystemCallClient& client,
                                                              NicDriverState& driver) noexcept
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

// Maps a client's data Frame at the window's last page; the kernel's error
// when it refuses (not a Frame of a page the driver may map).
[[nodiscard]] Abi::SystemCallError MapClient(const System::SystemCallClient& client, const NicDriverState& driver,
                                             const System::CapabilityHandle data) noexcept
{
    return System::MapWindow(client, driver.Space, driver.ClientMemory, data,
                             System::AddressOf(WindowPage(driver, Abi::NicClientPage)), 1U);
}

[[nodiscard]] Abi::SystemCallError UnmapClient(const System::SystemCallClient& client,
                                               const NicDriverState& driver) noexcept
{
    return client.UnmapMemory(driver.Space, System::AddressOf(WindowPage(driver, Abi::NicClientPage)), 1U).Error;
}

// Answers every Send whose descriptor the device wrote back, in ring order:
// NicDone with its length, its reply capability closed. False only when a
// Close failed other than on a stale handle.
[[nodiscard, gnu::noinline]] bool ReapTransmits(const System::SystemCallClient& client, NicDriverState& driver) noexcept
{
    bool closed{true};
    while ((driver.TransmitClean != driver.TransmitTail) &&
           ((TransmitDescriptor(driver, driver.TransmitClean)[System::E1000eDescriptorStatusOffset] &
             System::E1000eStatusDd) != 0U))
    {
        for (NicRequest& entry : g_requests)
        {
            if ((entry.State == NicRequestState::Sending) && (entry.Descriptor == driver.TransmitClean))
            {
                entry.State = NicRequestState::Free;
                Reply(client, driver, entry.Reply, entry.Transaction, NicDone, entry.Length, 0U);
                closed = Closed(client, entry.Reply) && closed;
            }
        }
        driver.TransmitClean = static_cast<Foundation::UInt8>((driver.TransmitClean + 1U) & (RingDescriptors - 1U));
    }
    return closed;
}

// Copies a received frame of length bytes into the oldest credit's Frame
// through the window's last page and answers it NicDone with the length. A
// credit whose Frame the kernel refuses to map is answered NicFailed and the
// next one tried; with none outstanding the frame is dropped and counted.
// Zero, or the exit code of an unmap or Close that failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 HandFrame(const System::SystemCallClient& client,
                                                          NicDriverState& driver, const Foundation::UInt8* const frame,
                                                          const Foundation::Size length) noexcept
{
    for (Foundation::Size attempt{0U}; attempt < RequestCapacity; ++attempt)
    {
        const Foundation::Size index = Oldest(NicRequestState::Credit);
        if (index == RequestCapacity)
        {
            break;
        }
        const NicRequest credit = g_requests[index];
        g_requests[index].State = NicRequestState::Free;
        const Abi::SystemCallError mapped = MapClient(client, driver, credit.Data);
        if (mapped != Abi::SystemCallError::None)
        {
            if (!Answer(client, driver, credit.Reply, credit.Data, credit.Transaction, NicFailed,
                        NicMapRefused | (static_cast<Foundation::UInt64>(mapped) & 0xFFU)))
            {
                return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
            }
            continue;
        }
        Foundation::UInt8* const page = WindowPage(driver, Abi::NicClientPage);
        for (Foundation::Size byte{0U}; byte < length; ++byte)
        {
            page[byte] = frame[byte];
        }
        const Abi::SystemCallError unmapped = UnmapClient(client, driver);
        if (unmapped != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverUnmapStep, unmapped);
        }
        return Answer(client, driver, credit.Reply, credit.Data, credit.Transaction, NicDone, length)
                   ? 0U
                   : DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
    }
    ++driver.DroppedFrames;
    return 0U;
}

// Takes every receive descriptor the device wrote back, in ring order: a
// whole frame (EOP, no frame error) goes to the oldest credit, anything else
// is dropped and counted; each descriptor's status is cleared and the
// descriptors are given back with one RDT write. Zero, or the exit code of
// an unmap or Close that failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 DeliverReceived(const System::SystemCallClient& client,
                                                                NicDriverState& driver) noexcept
{
    bool taken{false};
    Foundation::UInt8 last{0U};
    for (Foundation::Size count{0U}; count < RingDescriptors; ++count)
    {
        const Foundation::UInt8 index = driver.ReceiveNext;
        volatile Foundation::UInt8* const descriptor = ReceiveDescriptor(driver, index);
        const Foundation::UInt8 status = descriptor[System::E1000eDescriptorStatusOffset];
        if ((status & System::E1000eStatusDd) == 0U)
        {
            break;
        }
        System::OrderDmaStorage();
        const Foundation::Size length =
            static_cast<Foundation::Size>(descriptor[System::E1000eDescriptorLengthOffset]) |
            (static_cast<Foundation::Size>(descriptor[System::E1000eDescriptorLengthOffset + 1U]) << 8U);
        const bool whole = ((status & System::E1000eStatusEop) != 0U) &&
                           ((descriptor[System::E1000eDescriptorErrorsOffset] & System::E1000eFrameErrors) == 0U) &&
                           (length >= NicMinimumFrameBytes) && (length <= NicMaximumReceiveBytes);
        const Foundation::UInt64 failure =
            whole ? HandFrame(client, driver, WindowPage(driver, Abi::NicReceiveBufferPage) + (index * BufferBytes),
                              length)
                  : 0U;
        driver.DroppedFrames += whole ? 0U : 1U;
        StoreDescriptorWord(descriptor + 8U, 0U);
        last = index;
        taken = true;
        driver.ReceiveNext = static_cast<Foundation::UInt8>((index + 1U) & (RingDescriptors - 1U));
        if (failure != 0U)
        {
            return failure;
        }
    }
    if (taken)
    {
        System::OrderDmaStorage();
        Write(driver, System::E1000eRdtWord, last);
    }
    return 0U;
}

// Copies the oldest queued Sends into free transmit buffers through the
// window's last page, closes their data Frames and posts their descriptors
// with one TDT write; a Send whose Frame the kernel refuses to map is
// answered NicFailed. Zero, or the exit code of an unmap or Close that
// failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 PostSends(const System::SystemCallClient& client,
                                                          NicDriverState& driver) noexcept
{
    bool posted{false};
    while (InFlight(driver) < TransmitBuffers)
    {
        const Foundation::Size index = Oldest(NicRequestState::Queued);
        if (index == RequestCapacity)
        {
            break;
        }
        NicRequest& send = g_requests[index];
        const Abi::SystemCallError mapped = MapClient(client, driver, send.Data);
        if (mapped != Abi::SystemCallError::None)
        {
            send.State = NicRequestState::Free;
            if (!Answer(client, driver, send.Reply, send.Data, send.Transaction, NicFailed,
                        NicMapRefused | (static_cast<Foundation::UInt64>(mapped) & 0xFFU)))
            {
                return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
            }
            continue;
        }
        const Foundation::UInt8 slot = driver.TransmitTail;
        const Foundation::Size buffer = slot & (TransmitBuffers - 1U);
        Foundation::UInt8* const target = WindowPage(driver, Abi::NicTransmitBufferPage) + (buffer * BufferBytes);
        const Foundation::UInt8* const source = WindowPage(driver, Abi::NicClientPage);
        for (Foundation::Size byte{0U}; byte < send.Length; ++byte)
        {
            target[byte] = source[byte];
        }
        const Abi::SystemCallError unmapped = UnmapClient(client, driver);
        if (unmapped != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverUnmapStep, unmapped);
        }
        const bool closed = Closed(client, send.Data);
        send.Data = System::CapabilityHandle{};
        send.State = NicRequestState::Sending;
        send.Descriptor = slot;
        volatile Foundation::UInt8* const descriptor = TransmitDescriptor(driver, slot);
        StoreDescriptorWord(descriptor, driver.TransmitDevice + (buffer * BufferBytes));
        StoreDescriptorWord(descriptor + 8U, static_cast<Foundation::UInt64>(send.Length) |
                                                 (static_cast<Foundation::UInt64>(TransmitCommand) << 24U));
        driver.TransmitTail = static_cast<Foundation::UInt8>((slot + 1U) & (RingDescriptors - 1U));
        posted = true;
        if (!closed)
        {
            return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
        }
    }
    if (posted)
    {
        System::OrderDmaStorage();
        Write(driver, System::E1000eTdtWord, driver.TransmitTail);
    }
    return 0U;
}

// One wake, whichever woke it: the causes read (which clears
// them, so the next one signals again), the Sends the device sent answered,
// every request the Endpoint holds taken (so a credit that arrived with a
// frame catches it), the received frames handed out, the queued Sends
// posted, and the Interrupt rearmed after an interrupt wake: a message that
// arrived while it was masked is delivered at the arm. Zero, or the exit
// code of a step that failed.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ServeWake(const System::SystemCallClient& client,
                                                          NicDriverState& driver, const bool interrupt) noexcept
{
    static_cast<void>(Read(driver, System::E1000eIcrWord));
    if (!ReapTransmits(client, driver))
    {
        return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
    }
    Foundation::UInt64 failure = DrainEndpoint(client, driver);
    failure = ((failure != 0U) || !driver.Device) ? failure : DeliverReceived(client, driver);
    failure = ((failure != 0U) || !driver.Device) ? failure : PostSends(client, driver);
    if (failure != 0U)
    {
        return failure;
    }
    const Abi::SystemCallError rearmed =
        interrupt ? client.ControlInterrupt(driver.Interrupt, Abi::InterruptRearm).Error : Abi::SystemCallError::None;
    return (rearmed == Abi::SystemCallError::None) ? 0U : DriverFailure(Abi::ComponentDriverInterruptStep, rearmed);
}

// The service loop: every wake served whole, with the backstop on the wait
// while the device runs. Returns only a failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ServeFrames(const System::SystemCallClient& client,
                                                            NicDriverState& driver) noexcept
{
    for (;;)
    {
        const Abi::SystemCallResponse waited = client.Wait(driver.WaitSet, driver.Device ? BackstopMicroseconds : 0U);
        const bool ready = waited.Error == Abi::SystemCallError::None;
        const bool interrupt = ready && driver.Device && (waited.Value0 == NicInterruptTag);
        if (!interrupt && !(ready && (waited.Value0 == NicServiceWaitTag)) &&
            !(driver.Device && (waited.Error == Abi::SystemCallError::TimedOut)))
        {
            return DriverFailure(Abi::ComponentDriverServeStep, waited.Error);
        }
        const Foundation::UInt64 failure = ServeWake(client, driver, interrupt);
        if (failure != 0U)
        {
            return failure;
        }
    }
}
} // Namespace

Foundation::UInt64 ServeNicDevice(const System::SystemCallClient& client,
                                  const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                  const NicDriverPlatform& platform) noexcept
{
    for (NicRequest& entry : g_requests)
    {
        entry.State = NicRequestState::Free;
    }
    g_driver = NicDriverState{
        .Registers = RegistersOf(startBlock),
        .Window = reinterpret_cast<Foundation::UInt8*>(
            static_cast<Foundation::UIntPointer>(startBlock[Abi::ComponentWindowStartWord])),
        .Platform = platform,
        .Endpoint = {startBlock[Abi::ComponentReceiveWord], startBlock[Abi::ComponentReceiveWord + 1U]},
        .WaitSet = {startBlock[Abi::ComponentWaitSetWord], startBlock[Abi::ComponentWaitSetWord + 1U]},
        .Space = {startBlock[Abi::ComponentAccountSpaceWord], startBlock[Abi::ComponentAccountSpaceWord + 1U]},
        .Iommu = {},
        .Interrupt = {},
        .ClientMemory = {},
        .Generation = startBlock[Abi::ComponentIncarnationWord],
        .SpinTicks = startBlock[Abi::ComponentSpinTicksWord],
        .Mac = 0U,
        .DroppedFrames = 0U,
        .DroppedReplies = 0U,
        .DescriptorDevice = 0U,
        .ReceiveDevice = 0U,
        .TransmitDevice = 0U,
        .NextArrival = 0U,
        .ReceiveNext = 0U,
        .TransmitClean = 0U,
        .TransmitTail = 0U,
        .Device = false};
    if (startBlock[Abi::ComponentWindowPagesWord] < Abi::NicWindowPages)
    {
        return DriverFailure(Abi::ComponentDriverMapStep, Abi::SystemCallError::None);
    }
    const Foundation::UInt64 mapped = MapRegisters(client, startBlock, Abi::NicRegisterPages);
    if (mapped != 0U)
    {
        return mapped;
    }
    const Foundation::UInt64 reset = ResetDevice(g_driver);
    if (reset != 0U)
    {
        return reset;
    }
    const Foundation::UInt64 prepared = PrepareDma(client, startBlock, g_driver);
    if (prepared == DriverWithoutIommu)
    {
        return ServeFrames(client, g_driver);
    }
    if (prepared != 0U)
    {
        return prepared;
    }
    StartRings(g_driver);
    const Foundation::UInt64 connected = ConnectInterrupt(client, startBlock, g_driver);
    return (connected != 0U) ? connected : ServeFrames(client, g_driver);
}
} // Namespace Asteria::User::Component
