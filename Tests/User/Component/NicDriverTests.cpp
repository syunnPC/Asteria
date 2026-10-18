// SPDX-License-Identifier: Apache-2.0
#include "User/Component/NicDriver.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/NetFrame.hpp"

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/System/FakeE1000e.hpp"

#include <cstdio>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
namespace Component = User::Component;
using Abi::SystemCallError;
using Abi::SystemCallNumber;
using Nic = TestSupport::FakeE1000e;

constexpr UInt64 PageBytes{4096U};
constexpr UInt64 Generation{7U};
constexpr UInt64 Alpha{0x5100U};
constexpr UInt64 Beta{0x5200U};
constexpr UInt8 MacBytes[6U]{0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
constexpr UInt64 Mac{0x5634'1200'5452ULL};
// The kernel's creations in bring-up order: the register Frame and its
// MemoryObject, the IommuDomain, the descriptor, receive and transmit
// Frames each with its MemoryObject, the client MemoryObject, the
// Interrupt.
constexpr UInt64 DescriptorFrame{0x1004U};
constexpr UInt64 ReceiveFrame{0x1006U};
constexpr UInt64 TransmitFrame{0x1008U};
constexpr UInt64 InterruptHandle{0x100BU};
constexpr UInt64 Backstop{10'000U};

// The driver's window: the register pages (the model answers the driver's
// register accesses; RunComponent's own reader and writer use them), the
// Descriptor page, the buffers and the client page.
struct alignas(4096) NicWindow
{
    UInt8 Pages[Abi::NicWindowPages][PageBytes];
};

NicWindow g_window{};
Nic g_nic;

[[nodiscard]] UInt8* WindowPage(const UInt64 page) noexcept
{
    return g_window.Pages[page];
}

[[nodiscard]] UInt64 Load(const UInt8* const bytes, const Size count = 8U) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < count; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index << 3U);
    }
    return value;
}

void Store(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index << 3U));
    }
}

// The bytes of a test frame: a function of its seed and position.
[[nodiscard]] constexpr UInt8 PatternByte(const UInt64 seed, const Size index) noexcept
{
    return static_cast<UInt8>((seed * 31U) + (index * 7U) + (index >> 8U));
}

void Fill(UInt8* const bytes, const Size length, const UInt64 seed) noexcept
{
    for (Size index{0U}; index < length; ++index)
    {
        bytes[index] = PatternByte(seed, index);
    }
}

[[nodiscard]] bool Holds(const UInt8* const bytes, const Size length, const UInt64 seed) noexcept
{
    bool same{true};
    for (Size index{0U}; index < length; ++index)
    {
        same = same && (bytes[index] == PatternByte(seed, index));
    }
    return same;
}

// Every register access the driver made, with the count of system calls
// before it.
struct Access
{
    Size Word;
    UInt32 Value;
    bool Write;
    Size Stamp;
};

constexpr Size AccessCapacity{1024U};
Access g_accesses[AccessCapacity]{};
Size g_accessCount{0U};

// One wake of the driver: the clock ticks Ticks times, Action (if any)
// changes the device, then Arrivals more deliveries reach the Endpoint. The
// device's messages since the last wait reach the Interrupt unless Lost;
// the kernel answers the Interrupt's tag when it is armed and holds a
// message, else the Endpoint's when it holds deliveries, else TimedOut for a
// timed wait. End fails the wait (InvalidHandle), which ends the run.
using DeviceAction = void (*)() noexcept;

struct WaitStep
{
    UInt64 Ticks;
    DeviceAction Action;
    Size Arrivals;
    bool Lost;
    bool End;
};

[[nodiscard]] constexpr WaitStep Arrive(const Size arrivals) noexcept
{
    return WaitStep{.Ticks = 0U, .Action = nullptr, .Arrivals = arrivals, .Lost = false, .End = false};
}

[[nodiscard]] constexpr WaitStep Act(const DeviceAction action, const bool lost = false) noexcept
{
    return WaitStep{.Ticks = 0U, .Action = action, .Arrivals = 0U, .Lost = lost, .End = false};
}

[[nodiscard]] constexpr WaitStep Idle() noexcept
{
    return WaitStep{.Ticks = 0U, .Action = nullptr, .Arrivals = 0U, .Lost = false, .End = false};
}

[[nodiscard]] constexpr WaitStep End() noexcept
{
    return WaitStep{.Ticks = 0U, .Action = nullptr, .Arrivals = 0U, .Lost = false, .End = true};
}

struct ReplyRecord
{
    UInt64 Endpoint;
    UInt64 Transaction;
    UInt64 Kind;
    UInt64 Detail;
    UInt64 Extra;
    Size Wake;
};

struct WindowMapping
{
    UInt64 Frame;
    UInt64 Start;
    UInt64 Pages;
};

struct DeviceMapping
{
    UInt64 Frame;
    UInt64 Pages;
    UInt64 DeviceAddress;
    UInt8 Permissions;
};

constexpr Size DeliveryCapacity{Component::EchoDeliveryHeaderBytes + Component::EchoMaximumFrameBytes};
constexpr Size PoolCapacity{32U};
constexpr Size StepCapacity{32U};
constexpr Size ClientFrames{16U};

// The deliveries the kernel holds for the driver's Endpoint and the clients'
// one-page data Frames, by the low four bits of their selector: large, so
// apart from the kernel's records.
UInt8 g_pool[PoolCapacity][DeliveryCapacity]{};
Size g_poolBytes[PoolCapacity]{};
UInt8 g_clients[ClientFrames][PageBytes]{};

// Decides a wake in place of the script (the randomized test): returns the
// step to take.
using Director = WaitStep (*)() noexcept;

// A scripted kernel for the driver: answers every call it makes, records
// what each named, and runs the wakes of its script.
struct NicKernel
{
    Size Built;
    Size Released;
    Size Received;
    WaitStep Steps[StepCapacity];
    Size StepCount;
    Director Direct;
    Size Waits;
    UInt64 Timeouts[StepCapacity];
    bool Armed;
    bool Pending;
    Size SeenMessages;
    UInt8 DmaBits;
    SystemCallError IommuAnswer;
    SystemCallError InterruptAnswer;
    UInt64 RefusedFrames[2U];
    // The first calls, in order.
    UInt64 Numbers[128U];
    Size Calls;
    UInt64 Types[16U];
    Size Creates;
    WindowMapping Windows[8U];
    Size Maps;
    DeviceMapping Mappings[8U];
    Size IommuMaps;
    // Zero: IommuMap answers the device address asked for; otherwise
    // DeviceBase plus the Frame's handle in pages.
    UInt64 DeviceBase;
    Size Attaches;
    UInt64 BindTag;
    UInt64 BindSource;
    Size Binds;
    Size Rearms;
    bool Mastering;
    Size Controls;
    UInt64 ClientMapped;
    Size ClientMaps;
    Size Unmaps;
    ReplyRecord Replies[64U];
    Size ReplyCount;
    UInt64 Closed[128U];
    Size Closes;

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<NicKernel*>(context)->Answer(*request, *response);
    }

    void Answer(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        if (Calls < 128U)
        {
            Numbers[Calls] = request.Number;
        }
        ++Calls;
        switch (static_cast<SystemCallNumber>(request.Number))
        {
        case SystemCallNumber::ObjectCreate:
            Create(request, response);
            return;
        case SystemCallNumber::MemoryMap:
            Map(request, response);
            return;
        case SystemCallNumber::MemoryUnmap:
            Unmap(request, response);
            return;
        case SystemCallNumber::IommuControl:
            Iommu(request, response);
            return;
        case SystemCallNumber::DeviceControl:
            Mastering = Mastering || (request.Arguments[2U] == Abi::DeviceEnableBusMaster);
            ++Controls;
            return;
        case SystemCallNumber::WaitSetBind:
            BindSource = request.Arguments[2U];
            BindTag = request.Arguments[4U];
            ++Binds;
            return;
        case SystemCallNumber::InterruptControl:
            Armed = Armed || (request.Arguments[2U] == Abi::InterruptRearm);
            ++Rearms;
            return;
        case SystemCallNumber::WaitSetWait:
            Wait(request, response);
            return;
        case SystemCallNumber::IpcReceive:
            Receive(request, response);
            return;
        case SystemCallNumber::IpcSend:
            Send(request);
            return;
        case SystemCallNumber::CapabilityClose:
            Closed[Closes % 128U] = request.Arguments[0U];
            ++Closes;
            return;
        default:
            response.Error = SystemCallError::UnknownSystemCall;
            return;
        }
    }

    void Create(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        const UInt64 type = request.Arguments[2U];
        Types[Creates % 16U] = type;
        ++Creates;
        response.Value0 = 0x1000U + Creates;
        response.Value1 = 0x2000U + Creates;
        if (type == Abi::ObjectTypeIommuDomain)
        {
            const auto* const bytes =
                reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U]));
            DmaBits = bytes[Abi::ObjectCreateIommuAddressBitsOffset];
            response.Error = IommuAnswer;
        }
        else if (type == Abi::ObjectTypeInterrupt)
        {
            response.Error = InterruptAnswer;
        }
    }

    // A mapping at the client page maps the client's Frame there (its bytes
    // copied in), unless it is one the kernel refuses; any other is recorded
    // for the IOMMU's translation.
    void Map(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        const UInt8* const bytes =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U]));
        const UInt64 frame = Load(bytes + Abi::FaultResolveFrameOffset);
        const UInt64 start = Load(bytes + Abi::FaultResolveVirtualStartOffset);
        const UInt64 pages = Load(bytes + Abi::FaultResolvePageCountOffset);
        if (start != reinterpret_cast<Foundation::UIntPointer>(WindowPage(Abi::NicClientPage)))
        {
            Windows[Maps % 8U] = WindowMapping{.Frame = frame, .Start = start, .Pages = pages};
            ++Maps;
            return;
        }
        if ((frame == RefusedFrames[0U]) || (frame == RefusedFrames[1U]) || (ClientMapped != 0U) || (pages != 1U))
        {
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
        ClientMapped = frame;
        ++ClientMaps;
        for (Size index{0U}; index < PageBytes; ++index)
        {
            WindowPage(Abi::NicClientPage)[index] = g_clients[frame % ClientFrames][index];
        }
    }

    // Unmapping the client page copies its bytes back into the client's
    // Frame and leaves the page unreadable (zero).
    void Unmap(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        if ((request.Arguments[2U] != reinterpret_cast<Foundation::UIntPointer>(WindowPage(Abi::NicClientPage))) ||
            (request.Arguments[3U] != 1U) || (ClientMapped == 0U))
        {
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
        for (Size index{0U}; index < PageBytes; ++index)
        {
            g_clients[ClientMapped % ClientFrames][index] = WindowPage(Abi::NicClientPage)[index];
            WindowPage(Abi::NicClientPage)[index] = 0U;
        }
        ClientMapped = 0U;
        ++Unmaps;
    }

    void Iommu(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        const UInt8* const bytes =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U]));
        switch (request.Arguments[2U])
        {
        case Abi::IommuAttach:
            ++Attaches;
            return;
        case Abi::IommuMap:
        {
            // The device address answered: the one asked for (a translating
            // IOMMU), or DeviceBase plus the Frame's handle in pages (the
            // trusted-driver profile's identity backend), where the device
            // model then reaches the Frame.
            const UInt64 answered = (DeviceBase == 0U) ? Load(bytes + Abi::IommuMapDeviceAddressOffset)
                                                       : DeviceBase + (Load(bytes + Abi::IommuMapFrameOffset) << 12U);
            Mappings[IommuMaps % 8U] = DeviceMapping{.Frame = Load(bytes + Abi::IommuMapFrameOffset),
                                                     .Pages = Load(bytes + Abi::IommuMapPagesOffset),
                                                     .DeviceAddress = answered,
                                                     .Permissions = bytes[Abi::IommuMapPermissionsOffset]};
            ++IommuMaps;
            response.Value0 = answered;
            return;
        }
        default:
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
    }

    void Wait(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept;

    void Receive(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        if (Received >= Released)
        {
            response.Error = SystemCallError::WouldBlock;
            return;
        }
        UInt8* const target = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        const Size slot = Received % PoolCapacity;
        for (Size index{0U}; (index < g_poolBytes[slot]) && (index < request.Arguments[3U]); ++index)
        {
            target[index] = g_pool[slot][index];
        }
        response.Value1 = g_poolBytes[slot];
        ++Received;
    }

    void Send(const Abi::SystemCallRequest& request) noexcept
    {
        const UInt8* const frame =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        UInt8 delivery[Component::EchoDeliveryHeaderBytes + Component::NicReplyBytes]{};
        for (Size index{0U}; (index < request.Arguments[3U]) && (index < Component::NicReplyBytes); ++index)
        {
            delivery[Component::EchoDeliveryHeaderBytes + index] = frame[index];
        }
        Component::NicMessage reply{};
        const bool decoded = Component::ReadNicFrame(
            delivery, Component::EchoDeliveryHeaderBytes + static_cast<Size>(request.Arguments[3U]), reply);
        const bool matched =
            decoded && (reply.Method == Component::NicReplyMethod) && (reply.Words[0U] == reply.Transaction);
        Replies[ReplyCount % 64U] = ReplyRecord{.Endpoint = request.Arguments[0U],
                                                .Transaction = matched ? reply.Transaction : ~UInt64{0U},
                                                .Kind = reply.Words[1U],
                                                .Detail = reply.Words[2U],
                                                .Extra = reply.Words[3U],
                                                .Wake = Waits};
        ++ReplyCount;
    }

    // Adds a delivery to the pool: a frame of method from sender through a
    // capability sealed with selector, with the reply and data handles given.
    void Deliver(const UInt32 method, const UInt64 transaction, const UInt64 (&words)[Component::NicPayloadWords],
                 const UInt64 sender, const UInt64 selector, const UInt64 reply, const UInt64 data) noexcept
    {
        UInt8* const delivery = g_pool[Built % PoolCapacity];
        const User::System::CapabilityHandle replyHandle{reply, 1U};
        const User::System::CapabilityHandle dataHandle{data, 2U};
        const Size bytes = Component::WriteNicFrame(delivery + Component::EchoDeliveryHeaderBytes, method, transaction,
                                                    words, &replyHandle, &dataHandle);
        Store(delivery + 24U, sender);
        Store(delivery + Component::EchoDeliverySealedSelectorOffset, selector);
        g_poolBytes[Built % PoolCapacity] = Component::EchoDeliveryHeaderBytes + bytes;
        ++Built;
    }

    // A request's reply capability is 0x7000 + its transaction, its data
    // Frame 0x8000 + its transaction (its bytes the client Frame of the
    // transaction's low four bits).
    void SendFrame(const UInt64 transaction, const UInt64 length, const UInt64 sender = Alpha,
                   const UInt64 generation = Generation, const UInt64 selector = Component::NicSealedSelector) noexcept
    {
        Deliver(Component::NicSendMethod, transaction, {generation, length, 0U, 0U}, sender, selector,
                0x7000U + transaction, 0x8000U + transaction);
    }

    void Credit(const UInt64 transaction, const UInt64 sender = Alpha, const UInt64 generation = Generation,
                const UInt64 reply = 0U) noexcept
    {
        Deliver(Component::NicReceiveMethod, transaction, {generation, 0U, 0U, 0U}, sender,
                Component::NicSealedSelector, (reply != 0U) ? reply : (0x7000U + transaction), 0x8000U + transaction);
    }

    void Query(const UInt64 transaction, const UInt64 sender = Alpha, const UInt64 generation = Generation) noexcept
    {
        Deliver(Component::NicQueryMethod, transaction, {generation, 0U, 0U, 0U}, sender, Component::NicSealedSelector,
                0x7000U + transaction, 0U);
    }

    [[nodiscard]] bool Replied(const Size index, const UInt64 transaction, const UInt64 kind,
                               const UInt64 detail) const noexcept
    {
        const ReplyRecord& record = Replies[index % 64U];
        return (index < ReplyCount) && (record.Endpoint == (0x7000U + transaction)) &&
               (record.Transaction == transaction) && (record.Kind == kind) && (record.Detail == detail);
    }

    [[nodiscard]] bool ClosedHandle(const UInt64 selector) const noexcept
    {
        bool found{false};
        for (Size index{0U}; (index < Closes) && (index < 128U); ++index)
        {
            found = found || (Closed[index] == selector);
        }
        return found;
    }
};

NicKernel g_kernel{};

void NicKernel::Wait(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
{
    Timeouts[Waits % StepCapacity] = request.Arguments[2U];
    WaitStep step{End()};
    if (Direct != nullptr)
    {
        step = Direct();
    }
    else if (Waits < StepCount)
    {
        step = Steps[Waits];
    }
    ++Waits;
    for (UInt64 tick{0U}; tick < step.Ticks; ++tick)
    {
        static_cast<void>(g_nic.Tick());
    }
    if (step.Action != nullptr)
    {
        step.Action();
    }
    Released = ((Released + step.Arrivals) < Built) ? (Released + step.Arrivals) : Built;
    if (step.End)
    {
        response.Error = SystemCallError::InvalidHandle;
        return;
    }
    if (g_nic.Messages != SeenMessages)
    {
        SeenMessages = g_nic.Messages;
        Pending = Pending || !step.Lost;
    }
    if (Armed && Pending)
    {
        Armed = false;
        Pending = false;
        response.Value0 = Component::NicInterruptTag;
        return;
    }
    if (Received < Released)
    {
        response.Value0 = Component::NicServiceWaitTag;
        return;
    }
    response.Error = (request.Arguments[2U] != 0U) ? SystemCallError::TimedOut : SystemCallError::InvalidHandle;
}

// The test's IOMMU: a device access lands in the window page a Frame the
// driver mapped for the device is mapped at, within the mapping and its
// permissions, while the function masters the bus.
[[nodiscard]] UInt8* Translate(const UInt64 deviceAddress, const Size bytes, const bool write) noexcept
{
    if (!g_kernel.Mastering)
    {
        return nullptr;
    }
    for (Size index{0U}; (index < g_kernel.IommuMaps) && (index < 8U); ++index)
    {
        const DeviceMapping& mapping = g_kernel.Mappings[index];
        const UInt64 end = mapping.DeviceAddress + (mapping.Pages * PageBytes);
        const UInt8 needed = write ? Abi::IommuMapDeviceWrite : Abi::IommuMapDeviceRead;
        if ((deviceAddress < mapping.DeviceAddress) || ((deviceAddress + bytes) > end) ||
            ((mapping.Permissions & needed) == 0U))
        {
            continue;
        }
        for (Size window{0U}; (window < g_kernel.Maps) && (window < 8U); ++window)
        {
            if ((g_kernel.Windows[window].Frame == mapping.Frame) && (g_kernel.Windows[window].Pages >= mapping.Pages))
            {
                return reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(
                    g_kernel.Windows[window].Start + (deviceAddress - mapping.DeviceAddress)));
            }
        }
    }
    return nullptr;
}

[[nodiscard]] UInt64 Clock() noexcept
{
    return g_nic.Tick();
}

void Crash(const UInt64) noexcept
{
}

[[nodiscard]] UInt32 ReadRegister(const volatile UInt32* const, const Size word) noexcept
{
    const UInt32 value = g_nic.Read(word);
    if (g_accessCount < AccessCapacity)
    {
        g_accesses[g_accessCount] = Access{.Word = word, .Value = value, .Write = false, .Stamp = g_kernel.Calls};
    }
    ++g_accessCount;
    return value;
}

void WriteRegister(volatile UInt32* const, const Size word, const UInt32 value) noexcept
{
    if (g_accessCount < AccessCapacity)
    {
        g_accesses[g_accessCount] = Access{.Word = word, .Value = value, .Write = true, .Stamp = g_kernel.Calls};
    }
    ++g_accessCount;
    g_nic.Write(word, value);
}

// The first access to word of the kind given at or after from, or
// AccessCapacity.
[[nodiscard]] Size FindAccess(const Size word, const bool write, const Size from = 0U) noexcept
{
    for (Size index{from}; (index < g_accessCount) && (index < AccessCapacity); ++index)
    {
        if ((g_accesses[index].Word == word) && (g_accesses[index].Write == write))
        {
            return index;
        }
    }
    return AccessCapacity;
}

[[nodiscard]] Size CountAccesses(const Size word, const bool write) noexcept
{
    Size count{0U};
    for (Size index{0U}; (index < g_accessCount) && (index < AccessCapacity); ++index)
    {
        count += ((g_accesses[index].Word == word) && (g_accesses[index].Write == write)) ? 1U : 0U;
    }
    return count;
}

// The value of the last write to word, or ~0.
[[nodiscard]] UInt32 LastWrite(const Size word) noexcept
{
    UInt32 value{~UInt32{0U}};
    for (Size index{0U}; (index < g_accessCount) && (index < AccessCapacity); ++index)
    {
        value = ((g_accesses[index].Word == word) && g_accesses[index].Write) ? g_accesses[index].Value : value;
    }
    return value;
}

void ClearClients() noexcept
{
    for (UInt8(&frame)[PageBytes] : g_clients)
    {
        for (UInt8& byte : frame)
        {
            byte = 0U;
        }
    }
}

// A fresh device with its link a few ticks after each reset, a fresh
// kernel, and the start block.
void Prepare(UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    for (UInt8(&page)[PageBytes] : g_window.Pages)
    {
        for (UInt8& byte : page)
        {
            byte = 0U;
        }
    }
    ClearClients();
    g_nic.Clear(Translate, MacBytes);
    g_kernel = NicKernel{};
    g_accessCount = 0U;
    for (UInt64& word : block)
    {
        word = 0U;
    }
    block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
    block[Abi::ComponentIncarnationWord] = Generation;
    block[Abi::ComponentSpinTicksWord] = 100U;
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorNicDriver;
    block[Abi::ComponentWaitSetWord] = 0x31U;
    block[Abi::ComponentWaitSetWord + 1U] = 0x32U;
    block[Abi::ComponentReceiveWord] = 0x41U;
    block[Abi::ComponentReceiveWord + 1U] = 0x42U;
    block[Abi::ComponentCrashKindWord] = Abi::ComponentCrashInvalidInstruction;
    block[Abi::ComponentAccountWord] = 0x61U;
    block[Abi::ComponentAccountSpaceWord] = 0x63U;
    block[Abi::ComponentWindowStartWord] = reinterpret_cast<Foundation::UIntPointer>(&g_window);
    block[Abi::ComponentWindowPagesWord] = Abi::NicWindowPages;
    block[Abi::ComponentDeviceRegionWord] = 0x71U;
    block[Abi::ComponentDeviceFunctionWord] = 0x73U;
}

template <Size Count> void Script(const WaitStep (&steps)[Count]) noexcept
{
    static_assert(Count <= StepCapacity);
    for (Size index{0U}; index < Count; ++index)
    {
        g_kernel.Steps[index] = steps[index];
    }
    g_kernel.StepCount = Count;
}

[[nodiscard]] UInt64 Run(const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const User::System::SystemCallClient client{User::System::SystemCallTransport{&NicKernel::Transport, &g_kernel}};
    const Component::NicDriverPlatform platform{
        .Clock = Clock, .Crash = Crash, .ReadRegister = ReadRegister, .WriteRegister = WriteRegister};
    return Component::ServeNicDevice(client, block, platform);
}

[[nodiscard]] constexpr UInt64 Failure(const UInt64 step, const SystemCallError error = SystemCallError::None) noexcept
{
    return Abi::ComponentDriverFailedExitCode | (step << 32U) | (static_cast<UInt64>(error) << 40U);
}

constexpr UInt64 Ended{Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle)};

// Frames from the wire, each of length bytes made from seed.
struct WireFrame
{
    UInt64 Seed;
    Size Length;
};

constexpr WireFrame Inbound[4U]{{.Seed = 0xA1U, .Length = 60U},
                                {.Seed = 0xB2U, .Length = 1514U},
                                {.Seed = 0xC3U, .Length = 98U},
                                {.Seed = 0xD4U, .Length = 342U}};

void InjectFrame(const WireFrame& frame) noexcept
{
    static UInt8 bytes[Nic::FrameCapacity];
    Fill(bytes, frame.Length, frame.Seed);
    g_nic.Inject(bytes, frame.Length);
}

void InjectThree() noexcept
{
    InjectFrame(Inbound[0U]);
    InjectFrame(Inbound[1U]);
    InjectFrame(Inbound[2U]);
}

void InjectFirst() noexcept
{
    InjectFrame(Inbound[0U]);
}

void InjectLast() noexcept
{
    InjectFrame(Inbound[3U]);
}

// The driver maps its registers, resets the device (IMC, CTRL.RST until it clears, IMC, ICR) before
// anything may master the bus, sets the link up and waits for it, reads the MAC, then confines its
// function with the descriptor page and buffers mapped at their fixed device addresses, masters the
// bus, programs the rings and enables transmit and receive, and unmasks the causes only once the
// Interrupt is bound and armed.
void TestBringUp(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_nic.Words[Nic::Ctrl] = 0x0000'0241U;
    g_nic.Words[Nic::Rctl] = 0x0400'8002U;
    g_nic.Words[Nic::Ims] = 0x95U;
    Script({End()});
    const UInt64 ended = Run(block);
    constexpr SystemCallNumber Create{SystemCallNumber::ObjectCreate};
    constexpr SystemCallNumber Map{SystemCallNumber::MemoryMap};
    constexpr SystemCallNumber Iommu{SystemCallNumber::IommuControl};
    constexpr SystemCallNumber BringUp[]{Create,
                                         Create,
                                         Map,
                                         Create,
                                         Iommu,
                                         Create,
                                         Create,
                                         Map,
                                         Iommu,
                                         Create,
                                         Create,
                                         Map,
                                         Iommu,
                                         Create,
                                         Create,
                                         Map,
                                         Iommu,
                                         Create,
                                         SystemCallNumber::DeviceControl,
                                         Create,
                                         SystemCallNumber::WaitSetBind,
                                         SystemCallNumber::InterruptControl,
                                         SystemCallNumber::WaitSetWait};
    bool ordered{g_kernel.Calls == 23U};
    for (Size index{0U}; index < 23U; ++index)
    {
        ordered = ordered && (g_kernel.Numbers[index] == static_cast<UInt64>(BringUp[index]));
    }
    const UInt64 window = block[Abi::ComponentWindowStartWord];
    ASTERIA_EXPECT(runner, "bring-up maps the registers, confines, maps the DMA runs, masters, binds the Interrupt",
                   ordered, g_kernel.Types[0U] == Abi::ObjectTypeFrame,
                   g_kernel.Types[1U] == Abi::ObjectTypeMemoryObject, g_kernel.DmaBits == 64U,
                   g_kernel.Types[2U] == Abi::ObjectTypeIommuDomain, g_kernel.Types[9U] == Abi::ObjectTypeMemoryObject,
                   g_kernel.Types[10U] == Abi::ObjectTypeInterrupt, g_kernel.Windows[0U].Start == window,
                   g_kernel.Windows[0U].Pages == 8U, g_kernel.Windows[1U].Start == (window + (8U * PageBytes)),
                   g_kernel.Windows[1U].Pages == 1U, g_kernel.Windows[2U].Start == (window + (9U * PageBytes)),
                   g_kernel.Windows[2U].Pages == 8U, g_kernel.Windows[3U].Start == (window + (17U * PageBytes)),
                   g_kernel.Windows[3U].Pages == 4U, g_kernel.Attaches == 1U, g_kernel.Mastering, g_kernel.Binds == 1U,
                   g_kernel.BindSource == InterruptHandle, g_kernel.BindTag == Component::NicInterruptTag,
                   g_kernel.Timeouts[0U] == Backstop, ended == Ended);
    ASTERIA_EXPECT(runner, "the descriptor page, receive and transmit buffers are mapped at their device addresses",
                   g_kernel.IommuMaps == 3U, g_kernel.Mappings[0U].Frame == DescriptorFrame,
                   g_kernel.Mappings[0U].Pages == 1U, g_kernel.Mappings[0U].DeviceAddress == 0x0010'0000U,
                   g_kernel.Mappings[0U].Permissions == (Abi::IommuMapDeviceRead | Abi::IommuMapDeviceWrite),
                   g_kernel.Mappings[1U].Frame == ReceiveFrame, g_kernel.Mappings[1U].Pages == 8U,
                   g_kernel.Mappings[1U].DeviceAddress == 0x0020'0000U,
                   g_kernel.Mappings[1U].Permissions == Abi::IommuMapDeviceWrite,
                   g_kernel.Mappings[2U].Frame == TransmitFrame, g_kernel.Mappings[2U].Pages == 4U,
                   g_kernel.Mappings[2U].DeviceAddress == 0x0030'0000U,
                   g_kernel.Mappings[2U].Permissions == Abi::IommuMapDeviceRead);

    // The reset: the identification, IMC, CTRL.RST, its poll, IMC, ICR, all
    // before the IommuDomain (the third call) exists.
    const Size firstWrite = FindAccess(Nic::Imc, true);
    const Size reset = FindAccess(Nic::Ctrl, true);
    const Size remask = FindAccess(Nic::Imc, true, reset);
    const Size cleared = FindAccess(Nic::Icr, false, remask);
    const Size linkUp = FindAccess(Nic::Ctrl, true, reset + 1U);
    const Size address = FindAccess(Nic::Ral0, false);
    ASTERIA_EXPECT(runner, "reset: IMC, CTRL.RST, IMC again, ICR read, then SLU|ASDE, all before the IommuDomain",
                   g_accesses[0U].Word == Nic::Status, !g_accesses[0U].Write, firstWrite == 1U,
                   g_accesses[firstWrite].Value == ~UInt32{0U}, reset == 3U,
                   g_accesses[reset].Value == (0x0000'0241U | Nic::Rst), FindAccess(Nic::Ctrl, false, reset) < remask,
                   remask < cleared, cleared < linkUp, g_accesses[linkUp].Value == (0x0000'0241U | 0x20U | 0x40U),
                   g_accesses[linkUp].Stamp == 3U, g_nic.Resets == 1U, g_nic.Words[Nic::Rctl] == 0x0400'8002U);
    ASTERIA_EXPECT(runner, "the link is awaited and the MAC read after the reset",
                   CountAccesses(Nic::Status, false) >= 3U, FindAccess(Nic::Status, false, linkUp) < address,
                   linkUp < address, FindAccess(Nic::Rah0, false) == (address + 1U), g_accesses[address].Stamp == 3U,
                   (g_nic.Words[Nic::Status] & Nic::Lu) != 0U);

    const Size receiveOn = FindAccess(Nic::Rctl, true);
    const Size unmask = FindAccess(Nic::Ims, true);
    ASTERIA_EXPECT(runner, "the rings: legacy descriptors, 16 of each, receive all but one, no delay or throttle",
                   LastWrite(Nic::Rdbal) == 0x0010'0000U, LastWrite(Nic::Rdbah) == 0U, LastWrite(Nic::Rdlen) == 256U,
                   LastWrite(Nic::Rdh) == 0U, LastWrite(Nic::Rdt) == 15U, LastWrite(Nic::Tdbal) == 0x0010'0800U,
                   LastWrite(Nic::Tdbah) == 0U, LastWrite(Nic::Tdlen) == 256U, LastWrite(Nic::Tdh) == 0U,
                   LastWrite(Nic::Tdt) == 0U, LastWrite(Nic::Itr) == 0U, LastWrite(Nic::Rdtr) == 0U,
                   LastWrite(Nic::Radv) == 0U, (LastWrite(Nic::Rfctl) & (1U << 15U)) == 0U,
                   // Intel 82574 sections 10.2.5.11/10.2.6.11: immediate write-back,
                   // reserved RX fields zero, required TX bit 22 set, LWTHRESH disabled.
                   LastWrite(Nic::Rxdctl) == 0U, LastWrite(Nic::Txdctl) == 0x0040'0000U,
                   g_nic.InvalidDescriptorControls == 0U, LastWrite(Nic::Tctl) == 0x0003'F0FAU,
                   Load(WindowPage(8U)) == 0x0020'0000U, Load(WindowPage(8U) + (15U * 16U)) == 0x0020'7800U,
                   Load(WindowPage(8U) + 2048U) == 0U);
    ASTERIA_EXPECT(runner, "receive is enabled after bus mastering, the causes unmasked after the Interrupt is armed",
                   receiveOn < AccessCapacity, g_accesses[receiveOn].Value == 0x0400'8002U,
                   g_accesses[receiveOn].Stamp == 19U, FindAccess(Nic::Tctl, true) < receiveOn,
                   unmask == (g_accessCount - 1U), g_accesses[unmask].Value == 0x95U, g_accesses[unmask].Stamp == 22U,
                   g_accesses[unmask - 1U].Word == Nic::Icr, !g_accesses[unmask - 1U].Write, g_nic.IcrReads == 2U,
                   g_kernel.Rearms == 1U, g_nic.DmaFaults == 0U);
}

// a Send's frame is copied from the client's Frame through the
// client page into a transmit buffer and reaches the wire byte-exact; the
// data Frame is closed once copied, and the Send answered NicDone with its
// length only once the device wrote its descriptor back (the next wake).
void TestSendReachesWire(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    Fill(g_clients[1U], 60U, 0x11U);
    Fill(g_clients[2U], 1514U, 0x22U);
    g_kernel.SendFrame(1U, 60U);
    g_kernel.SendFrame(2U, 1514U);
    Script({Arrive(2U), Idle(), End()});
    const UInt64 ended = Run(block);
    const UInt8* const first = WindowPage(8U) + 2048U;
    const UInt8* const second = first + 16U;
    ASTERIA_EXPECT(runner, "two Sends reach the wire byte-exact, in order", g_nic.Transmitted == 2U,
                   g_nic.WireBytes[0U] == 60U, Holds(g_nic.Wire[0U], 60U, 0x11U), g_nic.WireBytes[1U] == 1514U,
                   Holds(g_nic.Wire[1U], 1514U, 0x22U), g_nic.DmaFaults == 0U, LastWrite(Nic::Tdt) == 2U,
                   CountAccesses(Nic::Tdt, true) == 2U);
    ASTERIA_EXPECT(runner, "their descriptors: the transmit buffers, the length, EOP|IFCS|RS, written back",
                   Load(first) == 0x0030'0000U, Load(first + 8U, 2U) == 60U, first[11U] == 0x0BU, first[12U] == 1U,
                   Load(second) == 0x0030'0800U, Load(second + 8U, 2U) == 1514U, second[11U] == 0x0BU);
    ASTERIA_EXPECT(runner, "the data Frames close once copied; the Sends are answered Done after the write-back",
                   g_kernel.ClientMaps == 2U, g_kernel.Unmaps == 2U, g_kernel.ClosedHandle(0x8001U),
                   g_kernel.ClosedHandle(0x8002U), g_kernel.Replied(0U, 1U, Component::NicDone, 60U),
                   g_kernel.Replied(1U, 2U, Component::NicDone, 1514U), g_kernel.Replies[0U].Wake == 2U,
                   g_kernel.Replies[0U].Extra == 0U, g_kernel.ReplyCount == 2U, g_kernel.ClosedHandle(0x7001U),
                   g_kernel.ClosedHandle(0x7002U), g_kernel.Closes == 4U, g_kernel.Rearms == 2U, ended == Ended);
}

// The trusted-driver profile: IommuMap answers other device addresses than those asked for (the
// physical ones); the rings and the transmit descriptors hold exactly those, and frames still reach
// the wire.
void TestAnsweredDeviceAddresses(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.DeviceBase = 0x7F00'0000U;
    Fill(g_clients[1U], 60U, 0x11U);
    g_kernel.SendFrame(1U, 60U);
    Script({Arrive(1U), Idle(), End()});
    const UInt64 ended = Run(block);
    const UInt64 descriptors = g_kernel.Mappings[0U].DeviceAddress;
    const UInt64 receive = g_kernel.Mappings[1U].DeviceAddress;
    const UInt64 transmit = g_kernel.Mappings[2U].DeviceAddress;
    ASTERIA_EXPECT(runner, "the rings, receive buffers and a Send use the answered device addresses",
                   descriptors != 0x0010'0000U, (descriptors & 0xFFFU) == 0U, LastWrite(Nic::Rdbal) == descriptors,
                   LastWrite(Nic::Tdbal) == (descriptors + 2048U), Load(WindowPage(8U)) == receive,
                   Load(WindowPage(8U) + (15U * 16U)) == (receive + 0x7800U), Load(WindowPage(8U) + 2048U) == transmit,
                   g_nic.Transmitted == 1U, Holds(g_nic.Wire[0U], 60U, 0x11U), g_nic.DmaFaults == 0U,
                   g_kernel.Replied(0U, 1U, Component::NicDone, 60U), ended == Ended);
}

// frames are copied into the credits in the order both arrived;
// one with no credit outstanding is dropped, given back to the device and
// counted, as a Query's answer reports beside the MAC and the link.
void TestReceiveInOrder(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Credit(10U);
    g_kernel.Credit(11U);
    g_kernel.Query(12U);
    Script({Arrive(2U), Act(InjectThree), Arrive(1U), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "two frames fill the two credits in order, byte-exact",
                   g_kernel.Replied(0U, 10U, Component::NicDone, 60U),
                   g_kernel.Replied(1U, 11U, Component::NicDone, 1514U), Holds(g_clients[10U], 60U, 0xA1U),
                   Holds(g_clients[11U], 1514U, 0xB2U), g_kernel.Replies[0U].Wake == 2U, g_kernel.ClosedHandle(0x800AU),
                   g_kernel.ClosedHandle(0x800BU), g_kernel.ClosedHandle(0x700AU), g_kernel.Unmaps == 2U,
                   g_nic.Received == 3U);
    ASTERIA_EXPECT(runner, "the third frame is dropped and counted; every descriptor goes back to the device",
                   LastWrite(Nic::Rdt) == 2U, CountAccesses(Nic::Rdt, true) == 2U, g_nic.Words[Nic::Rdh] == 3U,
                   (WindowPage(8U)[(2U * 16U) + 12U]) == 0U, g_kernel.Replied(2U, 12U, Component::NicDone, Mac),
                   g_kernel.Replies[2U].Extra == (1U | (UInt64{1U} << 32U)), g_kernel.ReplyCount == 3U,
                   g_kernel.ClosedHandle(0x700CU), g_kernel.Rearms == 2U, ended == Ended);
}

// Beyond four credits or four Sends and Queries a request is refused with the credit; another
// sender has its own credit; a transaction the sender has outstanding is refused too. The refused
// requests' capabilities are closed and nothing of theirs reaches the wire.
void TestCredit(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    for (UInt64 transaction{20U}; transaction < 25U; ++transaction)
    {
        g_kernel.Credit(transaction);
    }
    for (UInt64 transaction{25U}; transaction < 29U; ++transaction)
    {
        Fill(g_clients[transaction % 16U], 64U, transaction);
        g_kernel.SendFrame(transaction, 64U);
    }
    g_kernel.Query(29U);
    g_kernel.SendFrame(30U, 64U);
    g_kernel.Credit(31U, Beta);
    g_kernel.Credit(31U, Beta, Generation, 0x7100U);
    Script({Arrive(13U), Idle(), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a fifth credit, a Query or Send beyond four, and a duplicate are refused with the credit",
                   g_kernel.Replied(0U, 24U, Component::NicRefused, Component::NicMaximumReceiveCredits),
                   g_kernel.Replied(1U, 29U, Component::NicRefused, Component::NicMaximumSends),
                   g_kernel.Replied(2U, 30U, Component::NicRefused, Component::NicMaximumSends),
                   g_kernel.Replies[3U].Endpoint == 0x7100U, g_kernel.Replies[3U].Transaction == 31U,
                   g_kernel.Replies[3U].Kind == Component::NicRefused, g_kernel.ClosedHandle(0x7018U),
                   g_kernel.ClosedHandle(0x8018U), g_kernel.ClosedHandle(0x701DU), g_kernel.ClosedHandle(0x801EU),
                   g_kernel.ClosedHandle(0x7100U));
    ASTERIA_EXPECT(runner, "the four Sends within the credit go out and are answered", g_nic.Transmitted == 4U,
                   Holds(g_nic.Wire[3U], 64U, 28U), g_kernel.Replied(4U, 25U, Component::NicDone, 64U),
                   g_kernel.Replied(7U, 28U, Component::NicDone, 64U), g_kernel.ReplyCount == 8U, ended == Ended);
}

// Snapshots of the wire at each wait, for tests that need them.
Size g_wireAt[4U]{};
Size g_wireSnapshots{0U};

void SnapshotWire() noexcept
{
    if (g_wireSnapshots < 4U)
    {
        g_wireAt[g_wireSnapshots] = g_nic.Transmitted;
    }
    ++g_wireSnapshots;
}

// At most eight frames are in the transmit ring (its buffers); a Send beyond them queues until the
// device has sent one, then goes out in the order it arrived.
void TestTransmitQueue(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_wireSnapshots = 0U;
    constexpr UInt64 Gamma{0x5300U};
    constexpr UInt64 senders[3U]{Alpha, Beta, Gamma};
    for (UInt64 transaction{0x80U}; transaction < 0x8CU; ++transaction)
    {
        Fill(g_clients[transaction % 16U], 100U, transaction);
        g_kernel.SendFrame(transaction, 100U, senders[(transaction - 0x80U) >> 2U]);
    }
    Script({Arrive(12U), Act(SnapshotWire), Act(SnapshotWire), End()});
    const UInt64 ended = Run(block);
    bool ordered{g_nic.Transmitted == 12U};
    for (Size index{0U}; index < 12U; ++index)
    {
        ordered = ordered && Holds(g_nic.Wire[index], 100U, 0x80U + index);
    }
    ASTERIA_EXPECT(runner, "eight Sends fill the transmit buffers; the rest go out once those are sent, in order",
                   g_wireAt[0U] == 8U, g_wireAt[1U] == 12U, ordered, CountAccesses(Nic::Tdt, true) == 3U,
                   LastWrite(Nic::Tdt) == 12U, g_kernel.Replied(0U, 0x80U, Component::NicDone, 100U),
                   g_kernel.Replied(7U, 0x87U, Component::NicDone, 100U), g_kernel.Replies[7U].Wake == 2U,
                   g_kernel.Replied(11U, 0x8BU, Component::NicDone, 100U), g_kernel.Replies[11U].Wake == 3U,
                   g_kernel.ReplyCount == 12U, ended == Ended);
}

void InjectSpoiled() noexcept
{
    static UInt8 bytes[Nic::FrameCapacity];
    Fill(bytes, 60U, 0xE1U);
    g_nic.Inject(bytes, 60U, 0x03U, 0x01U);
    g_nic.Inject(bytes, 60U, 0x01U, 0U);
    g_nic.Inject(bytes, 60U, 0x03U, 0x60U);
    InjectFrame(Inbound[2U]);
}

// A descriptor without EOP (part of a frame) or with a frame error (CRC) is
// dropped and counted; checksum errors are left to the stack, whose
// software checks decide.
void TestSpoiledFrames(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Credit(0x90U);
    g_kernel.Credit(0x91U);
    g_kernel.Credit(0x92U);
    g_kernel.Query(0x93U);
    Script({Arrive(3U), Act(InjectSpoiled), Arrive(1U), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a CRC error and a frame without EOP are dropped; a checksum error is delivered",
                   g_kernel.Replied(0U, 0x90U, Component::NicDone, 60U), Holds(g_clients[0U], 60U, 0xE1U),
                   g_kernel.Replied(1U, 0x91U, Component::NicDone, 98U), Holds(g_clients[1U], 98U, 0xC3U),
                   g_kernel.Replied(2U, 0x93U, Component::NicDone, Mac),
                   g_kernel.Replies[2U].Extra == (1U | (UInt64{2U} << 32U)), g_kernel.ReplyCount == 3U, ended == Ended);
}

// A request for another generation reached an instance its client does not
// know: NicDriverRestarted, not executed, its capabilities closed. A frame
// that did not come through the sealed Endpoint is dropped, unanswered.
void TestStaleGeneration(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.SendFrame(40U, 60U, Alpha, Generation - 1U);
    g_kernel.Credit(41U, Alpha, Generation - 1U);
    g_kernel.Query(42U, Alpha, Generation + 1U);
    g_kernel.SendFrame(43U, 60U, Alpha, Generation, 0x1234U);
    Script({Arrive(4U), Act(InjectFirst), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "stale requests are answered DriverRestarted and not executed",
                   g_kernel.Replied(0U, 40U, Component::NicDriverRestarted, 0U),
                   g_kernel.Replied(1U, 41U, Component::NicDriverRestarted, 0U),
                   g_kernel.Replied(2U, 42U, Component::NicDriverRestarted, 0U), g_kernel.ReplyCount == 3U,
                   g_nic.Transmitted == 0U, g_kernel.ClientMaps == 0U, g_kernel.ClosedHandle(0x7028U),
                   g_kernel.ClosedHandle(0x8028U), g_kernel.ClosedHandle(0x8029U), g_kernel.ClosedHandle(0x702AU));
    ASTERIA_EXPECT(runner, "a frame through another capability is dropped with its capabilities closed",
                   g_kernel.ClosedHandle(0x702BU), g_kernel.ClosedHandle(0x802BU), g_kernel.Closes == 7U,
                   ended == Ended);
}

// Every wake reads ICR, which clears it, so the next cause signals again; a frame whose message was
// lost is found when the wait's 10 ms backstop ends it, and the next frame interrupts (and rearms)
// as usual.
void TestBackstopAndCauses(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Credit(50U);
    g_kernel.Credit(51U);
    Script({Arrive(2U), Act(InjectFirst, true), Act(InjectLast), Idle(), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a frame whose interrupt was lost is delivered when the backstop ends the wait",
                   g_kernel.Replied(0U, 50U, Component::NicDone, 60U), Holds(g_clients[50U % 16U], 60U, 0xA1U),
                   g_kernel.Replies[0U].Wake == 2U, g_kernel.Timeouts[0U] == Backstop,
                   g_kernel.Timeouts[1U] == Backstop, g_kernel.Timeouts[2U] == Backstop,
                   g_kernel.Timeouts[3U] == Backstop);
    ASTERIA_EXPECT(runner, "every wake reads ICR, so the next frame signals and rearms the Interrupt",
                   g_kernel.Replied(1U, 51U, Component::NicDone, 342U), Holds(g_clients[51U % 16U], 342U, 0xD4U),
                   g_kernel.Replies[1U].Wake == 3U, g_nic.Messages == 2U, g_nic.IcrReads == (2U + 4U),
                   g_kernel.Rearms == 2U, g_kernel.Waits == 5U, ended == Ended);
}

// A data Frame the kernel refuses to map fails its request with the
// kernel's error; a received frame then goes to the next credit.
void TestMapRefused(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.RefusedFrames[0U] = 0x8046U;
    g_kernel.RefusedFrames[1U] = 0x8048U;
    g_kernel.Credit(70U);
    g_kernel.Credit(71U);
    g_kernel.SendFrame(72U, 60U);
    Fill(g_clients[73U % 16U], 61U, 73U);
    g_kernel.SendFrame(73U, 61U);
    Script({Arrive(4U), Act(InjectFirst), End()});
    const UInt64 ended = Run(block);
    constexpr UInt64 refused{Component::NicMapRefused |
                             (static_cast<UInt64>(SystemCallError::InvalidArguments) & 0xFFU)};
    ASTERIA_EXPECT(runner, "a Send whose Frame cannot be mapped fails; the next one goes out",
                   g_kernel.Replied(0U, 72U, Component::NicFailed, refused), g_kernel.ClosedHandle(0x8048U),
                   g_nic.Transmitted == 1U, Holds(g_nic.Wire[0U], 61U, 73U),
                   g_kernel.Replied(1U, 73U, Component::NicDone, 61U));
    ASTERIA_EXPECT(runner, "a credit whose Frame cannot be mapped fails, and the frame fills the next credit",
                   g_kernel.Replied(2U, 70U, Component::NicFailed, refused), g_kernel.ClosedHandle(0x8046U),
                   g_kernel.Replied(3U, 71U, Component::NicDone, 60U), Holds(g_clients[71U % 16U], 60U, 0xA1U),
                   g_kernel.ReplyCount == 4U, ended == Ended);
}

// Of Documentation/Architecture/IommuDomain.md: without an IOMMU the device never masters the bus;
// Sends and credits fail with NicNoDevice and a Query still answers. A link that stays down is
// waited for within the budget, then reported.
void TestWithoutIommuOrLink(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_nic.LinkTicks = 1'000'000U;
    g_kernel.IommuAnswer = SystemCallError::InvalidConfiguration;
    g_kernel.SendFrame(60U, 60U);
    g_kernel.Credit(61U);
    g_kernel.Query(62U);
    Script({Arrive(3U), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "without an IOMMU: no DMA, no bus mastering, NicNoDevice; Query reports the link down",
                   g_kernel.Replied(0U, 60U, Component::NicFailed, Component::NicNoDevice),
                   g_kernel.Replied(1U, 61U, Component::NicFailed, Component::NicNoDevice),
                   g_kernel.Replied(2U, 62U, Component::NicDone, Mac), g_kernel.Replies[2U].Extra == 0U,
                   g_kernel.Creates == 3U, g_kernel.Controls == 0U, g_kernel.IommuMaps == 0U, g_kernel.Binds == 0U,
                   g_kernel.Timeouts[0U] == 0U, FindAccess(Nic::Rctl, true) == AccessCapacity, g_nic.Now >= 1600U,
                   g_kernel.ClosedHandle(0x803CU), g_kernel.ClosedHandle(0x803DU), ended == Ended);
}

// A device that is not there, a reset that does not clear, a refused
// Interrupt (receive is interrupt-driven, never a silent poll) and a window
// too small end the instance before it serves.
void TestBringUpFailures(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_nic.Absent = true;
    Script({End()});
    const UInt64 absent = Run(block);
    const Size absentCreates = g_kernel.Creates;

    Prepare(block);
    g_nic.StuckReset = true;
    Script({End()});
    const UInt64 stuck = Run(block);
    const Size stuckCreates = g_kernel.Creates;

    Prepare(block);
    g_kernel.InterruptAnswer = SystemCallError::ResourceExhausted;
    Script({End()});
    const UInt64 refused = Run(block);
    const bool unmasked = FindAccess(Nic::Ims, true) != AccessCapacity;
    const Size refusedWaits = g_kernel.Waits;

    Prepare(block);
    block[Abi::ComponentWindowPagesWord] = Abi::NicWindowPages - 1U;
    Script({End()});
    const UInt64 small = Run(block);
    ASTERIA_EXPECT(runner, "an absent device, a stuck reset, a refused Interrupt or a small window end the instance",
                   absent == Failure(Abi::ComponentDriverIdentityStep), absentCreates == 2U,
                   stuck == Failure(Abi::ComponentDriverQuiesceStep), stuckCreates == 2U,
                   refused == Failure(Abi::ComponentDriverInterruptStep, SystemCallError::ResourceExhausted), !unmasked,
                   refusedWaits == 0U, small == Failure(Abi::ComponentDriverMapStep), g_kernel.Creates == 0U,
                   g_kernel.Waits == 0U);
}

// RunComponent hands behaviour 14 to the NIC driver with the production
// reader and writer: over plain memory the reset bit stays set, which ends
// the instance at the quiesce.
void TestDispatch(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    Script({End()});
    const User::System::SystemCallTransport transport{&NicKernel::Transport, &g_kernel};
    const UInt64 ended = Component::RunComponent(transport, block, Clock, Crash);
    ASTERIA_EXPECT(runner, "the NIC behaviour runs the NIC driver", ended == Failure(Abi::ComponentDriverQuiesceStep),
                   (Load(WindowPage(0U), 4U) & Nic::Rst) != 0U, Load(WindowPage(0U) + 0xD8U, 4U) == 0xFFFF'FFFFU,
                   g_accessCount == 0U, g_kernel.Creates == 2U);
}

// The randomized model: one client posts credits and Sends (sometimes
// beyond its credit) and Queries while the wire delivers frames and
// messages are lost. After every wake the replies are checked against the
// client's view: credits filled in order with the frames in wire order
// (skipping the dropped ones), Sends on the wire in order and answered, a
// request beyond the credit refused. In the end, after the device has been
// drained, the frames delivered and dropped add up to the frames sent.
struct ModelRequest
{
    UInt64 Transaction;
    UInt64 Seed;
    Size Length;
    Size Slot;
};

struct Model
{
    TestSupport::Random Rng{1U};
    UInt64 Seed;
    UInt64 NextTransaction;
    ModelRequest Credits[8U];
    Size CreditCount;
    ModelRequest Sends[8U];
    Size SendCount;
    UInt64 QueryTransaction;
    UInt64 RefusedTransaction;
    bool SlotUsed[ClientFrames];
    WireFrame Injected[512U];
    Size InjectedCount;
    Size Matched;
    Size Skipped;
    Size Delivered;
    Size Sent;
    Size Wire;
    Size Checked;
    Size Steps;
    Size Drain;
    Size Refusals;
    Size LostMessages;
    UInt64 FinalDropped;
    bool Ok;
    Size FailedStep;
};

Model g_model{};

void Mismatch() noexcept
{
    if (g_model.Ok)
    {
        g_model.Ok = false;
        g_model.FailedStep = g_model.Steps;
        static_cast<void>(std::fprintf(stderr, "NicDriverTests model: seed %llu step %zu reply %zu\n",
                                       static_cast<unsigned long long>(g_model.Seed), g_model.Steps, g_model.Checked));
    }
}

[[nodiscard]] Size TakeSlot() noexcept
{
    for (Size slot{0U}; slot < ClientFrames; ++slot)
    {
        if (!g_model.SlotUsed[slot])
        {
            g_model.SlotUsed[slot] = true;
            return slot;
        }
    }
    return ClientFrames;
}

// Removes the request of transaction from list, its copy in found.
[[nodiscard]] bool Take(ModelRequest (&list)[8U], Size& count, const UInt64 transaction, ModelRequest& found) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        if (list[index].Transaction == transaction)
        {
            found = list[index];
            for (Size next{index + 1U}; next < count; ++next)
            {
                list[next - 1U] = list[next];
            }
            --count;
            return true;
        }
    }
    return false;
}

// A credit's Done: the oldest credit, holding the next frame from the wire
// the driver did not drop.
void CheckReceived(const ReplyRecord& reply) noexcept
{
    ModelRequest credit{};
    const bool oldest = (g_model.CreditCount != 0U) && (g_model.Credits[0U].Transaction == reply.Transaction);
    if (!oldest || !Take(g_model.Credits, g_model.CreditCount, reply.Transaction, credit))
    {
        Mismatch();
        return;
    }
    g_model.SlotUsed[credit.Slot] = false;
    while ((g_model.Matched < g_model.InjectedCount) &&
           !((g_model.Injected[g_model.Matched].Length == reply.Detail) &&
             Holds(g_clients[credit.Slot], g_model.Injected[g_model.Matched].Length,
                   g_model.Injected[g_model.Matched].Seed)))
    {
        ++g_model.Matched;
        ++g_model.Skipped;
    }
    if (g_model.Matched == g_model.InjectedCount)
    {
        Mismatch();
        return;
    }
    ++g_model.Matched;
    ++g_model.Delivered;
}

// Every reply since the last wake against the client's view.
void CheckReplies() noexcept
{
    for (; g_model.Checked < g_kernel.ReplyCount; ++g_model.Checked)
    {
        const ReplyRecord& reply = g_kernel.Replies[g_model.Checked % 64U];
        ModelRequest found{};
        if (reply.Transaction == g_model.RefusedTransaction)
        {
            g_model.RefusedTransaction = 0U;
            ++g_model.Refusals;
            if (reply.Kind != Component::NicRefused)
            {
                Mismatch();
            }
        }
        else if (reply.Transaction == g_model.QueryTransaction)
        {
            g_model.QueryTransaction = 0U;
            g_model.FinalDropped = reply.Extra >> 32U;
            if ((reply.Kind != Component::NicDone) || (reply.Detail != Mac) || ((reply.Extra & 1U) != 1U))
            {
                Mismatch();
            }
        }
        else if (reply.Kind != Component::NicDone)
        {
            Mismatch();
        }
        else if ((g_model.SendCount != 0U) && (g_model.Sends[0U].Transaction == reply.Transaction))
        {
            static_cast<void>(Take(g_model.Sends, g_model.SendCount, reply.Transaction, found));
            g_model.SlotUsed[found.Slot] = false;
            if (reply.Detail != found.Length)
            {
                Mismatch();
            }
        }
        else
        {
            CheckReceived(reply);
        }
    }
}

// The frames the device sent since the last wake, in the order the client
// posted its Sends: the answered ones went out first, so the next frame is
// the outstanding Send after those already on the wire.
void CheckWire() noexcept
{
    for (Size index{0U}; index < g_nic.Transmitted; ++index)
    {
        const Size answered = g_model.Sent - g_model.SendCount;
        const Size position = g_model.Wire - answered;
        if ((g_model.Wire < answered) || (position >= g_model.SendCount) ||
            (g_nic.WireBytes[index] != g_model.Sends[position].Length) ||
            !Holds(g_nic.Wire[index], g_model.Sends[position].Length, g_model.Sends[position].Seed))
        {
            Mismatch();
        }
        ++g_model.Wire;
    }
    g_nic.Transmitted = 0U;
}

void Request(const UInt32 method, const ModelRequest& request) noexcept
{
    const UInt64 data = 0x20'0000U + (request.Transaction << 4U) + request.Slot;
    const UInt64 words[Component::NicPayloadWords]{Generation,
                                                   (method == Component::NicSendMethod) ? request.Length : 0U, 0U, 0U};
    g_kernel.Deliver(method, request.Transaction, words, Alpha, Component::NicSealedSelector,
                     0x10'0000U + request.Transaction, data);
}

// Decides the next wake: checks the last one, then the client acts and the
// wire delivers, at random; the last steps drain the device and query it.
[[nodiscard]] WaitStep Direct() noexcept
{
    CheckReplies();
    CheckWire();
    ++g_model.Steps;
    constexpr Size RandomSteps{600U};
    if (g_model.Steps > RandomSteps)
    {
        ++g_model.Drain;
        if (g_model.Drain == 3U)
        {
            g_model.QueryTransaction = g_model.NextTransaction++;
            g_kernel.Query(g_model.QueryTransaction);
            return Arrive(1U);
        }
        return (g_model.Drain < 5U) ? Idle() : End();
    }
    TestSupport::Random& rng = g_model.Rng;
    Size arrivals{0U};
    const Size actions = static_cast<Size>(rng.Below(4U));
    for (Size action{0U}; action < actions; ++action)
    {
        const UInt64 pick = rng.Below(16U);
        const bool beyond = (pick == 0U) && (g_model.RefusedTransaction == 0U);
        if ((pick < 6U) && ((g_model.CreditCount < Component::NicMaximumReceiveCredits) || beyond))
        {
            ModelRequest credit{.Transaction = g_model.NextTransaction++, .Seed = 0U, .Length = 0U, .Slot = 0U};
            if (g_model.CreditCount >= Component::NicMaximumReceiveCredits)
            {
                g_model.RefusedTransaction = credit.Transaction;
                Request(Component::NicReceiveMethod, credit);
            }
            else
            {
                credit.Slot = TakeSlot();
                g_model.Credits[g_model.CreditCount++] = credit;
                Request(Component::NicReceiveMethod, credit);
            }
            ++arrivals;
        }
        else if ((pick < 11U) && (g_model.SendCount < Component::NicMaximumSends))
        {
            ModelRequest send{.Transaction = g_model.NextTransaction++,
                              .Seed = rng.Next(),
                              .Length = static_cast<Size>(
                                  Component::NicMinimumFrameBytes +
                                  rng.Below(Component::NicMaximumFrameBytes - Component::NicMinimumFrameBytes + 1U)),
                              .Slot = TakeSlot()};
            Fill(g_clients[send.Slot], send.Length, send.Seed);
            g_model.Sends[g_model.SendCount++] = send;
            ++g_model.Sent;
            Request(Component::NicSendMethod, send);
            ++arrivals;
        }
        else if ((pick >= 11U) && (g_model.InjectedCount < 512U) && ((g_nic.InboundTail - g_nic.InboundHead) < 8U))
        {
            const WireFrame frame{.Seed = rng.Next(),
                                  .Length = static_cast<Size>(Component::NicMinimumFrameBytes +
                                                              rng.Below(Component::NicMaximumFrameBytes -
                                                                        Component::NicMinimumFrameBytes + 1U))};
            g_model.Injected[g_model.InjectedCount++] = frame;
            InjectFrame(frame);
        }
    }
    const bool lost = rng.Chance(1U, 6U);
    g_model.LostMessages += lost ? 1U : 0U;
    return WaitStep{.Ticks = 0U, .Action = nullptr, .Arrivals = arrivals, .Lost = lost, .End = false};
}

void TestRandomized(TestContext& runner) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(1U, 8U);
    bool all{true};
    Size delivered{0U};
    UInt64 dropped{0U};
    Size sent{0U};
    Size refusals{0U};
    Size lost{0U};
    for (UInt64 seed{seeds.First}; seed < (seeds.First + seeds.Count); ++seed)
    {
        UInt64 block[Abi::ComponentBlockWords]{};
        Prepare(block);
        g_model = Model{};
        g_model.Rng = TestSupport::Random{seed};
        g_model.Seed = seed;
        g_model.NextTransaction = 1U;
        g_model.Ok = true;
        g_kernel.Direct = Direct;
        const UInt64 ended = Run(block);
        CheckReplies();
        all = all && g_model.Ok && (ended == Ended) && (g_model.SendCount == 0U) && (g_model.Wire == g_model.Sent) &&
              ((g_model.Delivered + g_model.FinalDropped) == g_model.InjectedCount) && (g_nic.DmaFaults == 0U) &&
              (g_model.Skipped <= g_model.FinalDropped) && (g_model.QueryTransaction == 0U) &&
              (g_model.RefusedTransaction == 0U);
        delivered += g_model.Delivered;
        dropped += g_model.FinalDropped;
        sent += g_model.Sent;
        refusals += g_model.Refusals;
        lost += g_model.LostMessages;
        if (!all)
        {
            static_cast<void>(std::fprintf(
                stderr,
                "NicDriverTests model: seed %llu failed (step %zu, delivered %zu, dropped %llu, injected %zu)\n",
                static_cast<unsigned long long>(seed), g_model.FailedStep, g_model.Delivered,
                static_cast<unsigned long long>(g_model.FinalDropped), g_model.InjectedCount));
            break;
        }
    }
    g_kernel.Direct = nullptr;
    const bool exercised = (seeds.Count == 1U) || ((delivered > 400U) && (dropped > 10U) && (sent > 800U) &&
                                                   (refusals > 40U) && (lost > 100U));
    ASTERIA_EXPECT(runner, "random clients and wire traffic agree with the model after every wake", all, exercised);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"NicDriverTests"};
    Asteria::Test::TestBringUp(runner);
    Asteria::Test::TestSendReachesWire(runner);
    Asteria::Test::TestAnsweredDeviceAddresses(runner);
    Asteria::Test::TestReceiveInOrder(runner);
    Asteria::Test::TestCredit(runner);
    Asteria::Test::TestTransmitQueue(runner);
    Asteria::Test::TestSpoiledFrames(runner);
    Asteria::Test::TestStaleGeneration(runner);
    Asteria::Test::TestBackstopAndCauses(runner);
    Asteria::Test::TestMapRefused(runner);
    Asteria::Test::TestWithoutIommuOrLink(runner);
    Asteria::Test::TestBringUpFailures(runner);
    Asteria::Test::TestDispatch(runner);
    Asteria::Test::TestRandomized(runner);
    return runner.Finish();
}
