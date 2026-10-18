// SPDX-License-Identifier: Apache-2.0
#include "User/Component/AhciDriver.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/ComponentProgram.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/System/AhciPort.hpp"

#include "Support/TestContext.hpp"
#include "User/System/FakeAhciHba.hpp"

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

// The driver's window: its register page, which mirrors the model's ABAR
// after every write and tick (the model's register file spans 0x1100 bytes,
// more than the page), its DMA page and a spare page.
struct alignas(4096) DriverWindow
{
    UInt32 Registers[1024U];
    UInt8 Dma[4096U];
    UInt8 Bounce[Abi::AhciBouncePages * 4096U];
    UInt8 Client[Abi::AhciBouncePages * 4096U];
};

DriverWindow g_window{};
TestSupport::FakeAhciHba g_hba;
UInt64 g_crashes{0U};
UInt64 g_crashKind{0U};

// Each command the driver issued: slot 0's command FIS and PRDT entry as
// they were when it wrote PxCI.
struct IssuedCommand
{
    UInt8 Fis[16U];
    UInt8 Prdt[16U];
};

IssuedCommand g_commands[8U]{};
Size g_commandCount{0U};
bool g_delayFlush{false};
bool g_errorFlush{false};

// QEMU's ich9-ahci: six ports, the disk on port 2 (FakeAhciHba::Clear).
constexpr UInt32 DiskPort{2U};
constexpr Size PortBase{(0x100U >> 2U) + (DiskPort << 5U)};
constexpr Size PortIs{PortBase + 4U};
constexpr Size PortIe{PortBase + 5U};
constexpr Size PortCmd{PortBase + 6U};
constexpr Size PortTfd{PortBase + 8U};
constexpr Size PortCi{PortBase + 14U};
constexpr UInt32 Tfes{1U << 30U};
constexpr UInt64 Generation{7U};
constexpr UInt64 Capacity{4096U};
// The kernel's sixth creation: the device Frame, the MemoryObject over it,
// the IommuDomain, the DMA page's Frame and MemoryObject, the Interrupt.
constexpr UInt64 InterruptHandle{0x1006U};

void Mirror() noexcept
{
    for (Size word{0U}; word < 1024U; ++word)
    {
        g_window.Registers[word] = g_hba.Words[word];
    }
}

[[nodiscard]] UInt64 Clock() noexcept
{
    const UInt64 now = g_hba.Tick();
    Mirror();
    return now;
}

void WriteRegister(volatile UInt32* const registers, const Size word, const UInt32 value) noexcept
{
    static_cast<void>(registers);
    if ((word == ((0x100U >> 2U) + (2U << 5U) + 14U)) && (g_commandCount < 8U))
    {
        for (Size index{0U}; index < 16U; ++index)
        {
            g_commands[g_commandCount].Fis[index] = g_window.Dma[0x500U + index];
            g_commands[g_commandCount].Prdt[index] = g_window.Dma[0x580U + index];
        }
        ++g_commandCount;
    }
    g_hba.Write(word, value);
    if ((word == PortCi) && (g_window.Dma[0x502U] == 0xEAU))
    {
        if (g_delayFlush)
        {
            g_hba.Due = g_hba.Now + 10'000U;
        }
        if (g_errorFlush)
        {
            g_hba.Words[PortIs] |= Tfes;
        }
    }
    Mirror();
}

void Crash(const UInt64 kind) noexcept
{
    ++g_crashes;
    g_crashKind = kind;
}

// How often the driver wrote word, as far as the model logs writes.
[[nodiscard]] Size CountWrites(const Size word) noexcept
{
    Size count{0U};
    for (Size index{0U}; (index < g_hba.WriteCount) && (index < 512U); ++index)
    {
        count += (g_hba.Writes[index].Word == word) ? 1U : 0U;
    }
    return count;
}

[[nodiscard]] UInt64 Load(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
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

// One wake of the driver. The controller's clock ticks Ticks times first
// (time passes while the driver blocks), Action (if any) changes the
// controller, then Arrivals more deliveries reach the Endpoint. The kernel
// answers the Interrupt's tag when the controller interrupts (GHC.IE and HBA
// IS) and the Interrupt is armed, unless the interrupt is Lost; else the
// Endpoint's tag when it holds messages; else TimedOut for a timed wait. End
// fails the wait (InvalidHandle), which ends the run. MapAnswer is what
// IommuMap answers from this wake on.
using ControllerAction = void (*)() noexcept;

struct WaitStep
{
    UInt64 Ticks;
    ControllerAction Action;
    Size Arrivals;
    bool Lost;
    bool End;
    SystemCallError MapAnswer;
};

[[nodiscard]] constexpr WaitStep Arrive(const Size arrivals) noexcept
{
    return WaitStep{.Ticks = 0U,
                    .Action = nullptr,
                    .Arrivals = arrivals,
                    .Lost = false,
                    .End = false,
                    .MapAnswer = SystemCallError::None};
}

[[nodiscard]] constexpr WaitStep Elapse(const UInt64 ticks, const bool lost = false) noexcept
{
    return WaitStep{.Ticks = ticks,
                    .Action = nullptr,
                    .Arrivals = 0U,
                    .Lost = lost,
                    .End = false,
                    .MapAnswer = SystemCallError::None};
}

[[nodiscard]] constexpr WaitStep Act(const ControllerAction action) noexcept
{
    return WaitStep{
        .Ticks = 0U, .Action = action, .Arrivals = 0U, .Lost = false, .End = false, .MapAnswer = SystemCallError::None};
}

[[nodiscard]] constexpr WaitStep End() noexcept
{
    return WaitStep{
        .Ticks = 0U, .Action = nullptr, .Arrivals = 0U, .Lost = false, .End = true, .MapAnswer = SystemCallError::None};
}

struct ReplyRecord
{
    UInt64 Method;
    UInt64 First;
    UInt64 Extra;
    UInt64 Endpoint;
    UInt64 Transaction;
    UInt64 Kind;
    UInt64 Detail;
};

struct MappingRecord
{
    UInt64 Frame;
    UInt64 Pages;
    UInt64 DeviceAddress;
    UInt8 Permissions;
};

constexpr Size DeliveryCapacity{Component::EchoDeliveryHeaderBytes + Component::EchoMaximumFrameBytes};

// A scripted kernel for the driver: answers every call it makes, records
// what each named, and runs the wakes of its script.
struct BlockKernel
{
    UInt8 Pool[16U][DeliveryCapacity];
    Size PoolBytes[16U];
    Size Built;
    Size Released;
    Size Received;
    WaitStep Steps[16U];
    Size StepCount;
    Size Waits;
    UInt64 Timeouts[16U];
    bool Armed;
    UInt8 DmaBits;
    SystemCallError IommuAnswer;
    SystemCallError InterruptAnswer;
    SystemCallError MapAnswer;
    // The first calls, in order.
    UInt64 Numbers[64U];
    Size Calls;
    UInt64 Types[8U];
    Size Creates;
    UInt64 MapStarts[4U];
    Size Maps;
    MappingRecord Mappings[16U];
    Size IommuMaps;
    // Zero: IommuMap answers the device address asked for; otherwise the
    // identity backend's answer modelled as DeviceBase plus the Frame handle
    // in pages.
    UInt64 DeviceBase;
    UInt64 Unmapped[16U];
    Size Unmaps;
    Size Attaches;
    UInt64 BindTag;
    UInt64 BindSource;
    Size Binds;
    Size Rearms;
    UInt64 DeviceOperations[4U];
    Size Controls;
    ReplyRecord Replies[32U];
    Size ReplyCount;
    UInt64 Closed[64U];
    Size Closes;

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<BlockKernel*>(context)->Answer(*request, *response);
    }

    void Answer(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        if (Calls < 64U)
        {
            Numbers[Calls] = request.Number;
        }
        ++Calls;
        const UInt8* const bytes =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U]));
        switch (static_cast<SystemCallNumber>(request.Number))
        {
        case SystemCallNumber::ObjectCreate:
            Create(request, response);
            return;
        case SystemCallNumber::MemoryMap:
            MapStarts[Maps % 4U] = Load(bytes + Abi::FaultResolveVirtualStartOffset);
            ++Maps;
            return;
        case SystemCallNumber::MemoryUnmap:
            return;
        case SystemCallNumber::IommuControl:
            Iommu(request, response);
            return;
        case SystemCallNumber::DeviceControl:
            DeviceOperations[Controls % 4U] = request.Arguments[2U];
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
            Closed[Closes % 64U] = request.Arguments[0U];
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
        Types[Creates % 8U] = type;
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
            Mappings[IommuMaps % 16U] = MappingRecord{.Frame = Load(bytes + Abi::IommuMapFrameOffset),
                                                      .Pages = Load(bytes + Abi::IommuMapPagesOffset),
                                                      .DeviceAddress = Load(bytes + Abi::IommuMapDeviceAddressOffset),
                                                      .Permissions = bytes[Abi::IommuMapPermissionsOffset]};
            ++IommuMaps;
            // The driver's own page is always mapped; a client's as the
            // script says. The device address answered is the one asked for
            // (a translating IOMMU), or DeviceBase plus the Frame's handle in
            // pages (the trusted-driver profile's identity backend).
            response.Error =
                (Load(bytes + Abi::IommuMapDeviceAddressOffset) == 0x0010'0000U) ? SystemCallError::None : MapAnswer;
            response.Value0 = (DeviceBase == 0U) ? Load(bytes + Abi::IommuMapDeviceAddressOffset)
                                                 : DeviceBase + (Load(bytes + Abi::IommuMapFrameOffset) << 12U);
            return;
        case Abi::IommuUnmap:
            Unmapped[Unmaps % 16U] = request.Arguments[3U];
            ++Unmaps;
            return;
        default:
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
    }

    void Wait(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        Timeouts[Waits % 16U] = request.Arguments[2U];
        if (Waits >= StepCount)
        {
            response.Error = SystemCallError::InvalidHandle;
            return;
        }
        const WaitStep& step = Steps[Waits];
        ++Waits;
        MapAnswer = step.MapAnswer;
        for (UInt64 tick{0U}; tick < step.Ticks; ++tick)
        {
            static_cast<void>(Clock());
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
        const bool interrupting = Armed && !step.Lost && ((g_hba.Words[1U] & 2U) != 0U) && (g_hba.Words[2U] != 0U);
        const bool queued = Received < Released;
        if (interrupting)
        {
            Armed = false;
            response.Value0 = Component::BlockInterruptTag;
            return;
        }
        if (queued)
        {
            response.Value0 = Component::BlockServiceWaitTag;
            return;
        }
        response.Error = (request.Arguments[2U] != 0U) ? SystemCallError::TimedOut : SystemCallError::InvalidHandle;
    }

    void Receive(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        if (Received >= Released)
        {
            response.Error = SystemCallError::WouldBlock;
            return;
        }
        UInt8* const target = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        for (Size index{0U}; (index < PoolBytes[Received]) && (index < request.Arguments[3U]); ++index)
        {
            target[index] = Pool[Received][index];
        }
        response.Value1 = PoolBytes[Received];
        ++Received;
    }

    void Send(const Abi::SystemCallRequest& request) noexcept
    {
        const UInt8* const frame =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        UInt8 delivery[Component::EchoDeliveryHeaderBytes + Component::BlockReplyBytes]{};
        for (Size index{0U}; (index < request.Arguments[3U]) && (index < Component::BlockReplyBytes); ++index)
        {
            delivery[Component::EchoDeliveryHeaderBytes + index] = frame[index];
        }
        Component::BlockMessage reply{};
        const bool decoded = Component::ReadBlockFrame(
            delivery, Component::EchoDeliveryHeaderBytes + static_cast<Size>(request.Arguments[3U]), reply);
        Replies[ReplyCount % 32U] = ReplyRecord{.Method = reply.Method,
                                                .First = reply.Words[0U],
                                                .Extra = reply.Words[3U],
                                                .Endpoint = request.Arguments[0U],
                                                .Transaction = decoded ? reply.Transaction : ~UInt64{0U},
                                                .Kind = reply.Words[1U],
                                                .Detail = reply.Words[2U]};
        ++ReplyCount;
    }

    // Adds a delivery to the pool: a frame of method from sender through a
    // capability sealed with selector. A request's reply capability is
    // 0x7000 + its transaction, its data Frame 0x8000 + its transaction; a
    // Cancel's reply capability 0x9000 + its transaction.
    void Deliver(const UInt32 method, const UInt64 transaction, const UInt64 (&words)[Component::BlockPayloadWords],
                 const UInt64 sender = 0x5100U, const UInt64 selector = Component::BlockSealedSelector,
                 const UInt64 medium = Generation) noexcept
    {
        UInt8* const delivery = Pool[Built % 16U];
        const User::System::CapabilityHandle reply{
            ((method == Component::BlockCancelMethod) ? 0x9000U : 0x7000U) + transaction, 1U};
        const User::System::CapabilityHandle data{0x8000U + transaction, 2U};
        const UInt64 io[5U]{words[0U], words[1U], words[2U], words[3U], medium};
        const bool transfer = (method == Component::BlockReadMethod) || (method == Component::BlockWriteMethod);
        const Size bytes = transfer ? Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes, method,
                                                                 transaction, io, &reply, &data)
                                    : Component::WriteBlockFrame(delivery + Component::EchoDeliveryHeaderBytes, method,
                                                                 transaction, words, &reply, &data);
        Store(delivery + 24U, sender);
        Store(delivery + Component::EchoDeliverySealedSelectorOffset, selector);
        PoolBytes[Built % 16U] = Component::EchoDeliveryHeaderBytes + bytes;
        ++Built;
    }

    void Read(const UInt64 transaction, const UInt64 lba, const UInt64 sectors, const UInt64 sender = 0x5100U,
              const UInt64 flags = 0U, const UInt64 generation = Generation) noexcept
    {
        Deliver(Component::BlockReadMethod, transaction, {generation, lba, sectors, flags}, sender);
    }

    void Cancel(const UInt64 transaction, const UInt64 target, const UInt64 sender = 0x5100U,
                const UInt64 generation = Generation) noexcept
    {
        Deliver(Component::BlockCancelMethod, transaction, {generation, target, 0U, 0U}, sender);
    }

    [[nodiscard]] bool Replied(const Size index, const UInt64 endpoint, const UInt64 transaction, const UInt64 kind,
                               const UInt64 detail) const noexcept
    {
        const ReplyRecord& record = Replies[index % 32U];
        return (index < ReplyCount) && (record.Endpoint == endpoint) && (record.Transaction == transaction) &&
               (record.Kind == kind) && (record.Detail == detail);
    }

    [[nodiscard]] bool ClosedHandle(const UInt64 selector) const noexcept
    {
        bool found{false};
        for (Size index{0U}; (index < Closes) && (index < 64U); ++index)
        {
            found = found || (Closed[index] == selector);
        }
        return found;
    }
};

BlockKernel g_kernel{};

// A fresh controller with its disk on port 2, an IDENTIFY answer of
// Capacity sectors staged in the DMA page (the model writes no data), a
// fresh kernel with the script given, and the start block.
void Prepare(UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    g_hba.Clear();
    g_window = DriverWindow{};
    g_kernel = BlockKernel{};
    g_crashes = 0U;
    g_crashKind = 0U;
    g_commandCount = 0U;
    g_delayFlush = false;
    g_errorFlush = false;
    UInt8* const identify = g_window.Dma + 0xA00U;
    identify[83U << 1U] = 0x00U;
    identify[(83U << 1U) + 1U] = 0x44U;
    identify[100U << 1U] = static_cast<UInt8>(Capacity);
    identify[(100U << 1U) + 1U] = static_cast<UInt8>(Capacity >> 8U);
    g_hba.Words[0U] |= 1U << 31U; // Default fixture: S64A, as ich9-ahci.
    Mirror();
    for (UInt64& word : block)
    {
        word = 0U;
    }
    block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
    block[Abi::ComponentIncarnationWord] = Generation;
    block[Abi::ComponentSpinTicksWord] = 100U;
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorBlockDriver;
    block[Abi::ComponentWaitSetWord] = 0x31U;
    block[Abi::ComponentWaitSetWord + 1U] = 0x32U;
    block[Abi::ComponentReceiveWord] = 0x41U;
    block[Abi::ComponentReceiveWord + 1U] = 0x42U;
    block[Abi::ComponentCrashKindWord] = Abi::ComponentCrashFloatingPoint;
    block[Abi::ComponentAccountWord] = 0x61U;
    block[Abi::ComponentAccountSpaceWord] = 0x63U;
    block[Abi::ComponentWindowStartWord] = reinterpret_cast<Foundation::UIntPointer>(&g_window);
    block[Abi::ComponentWindowPagesWord] = 3U;
    block[Abi::ComponentDeviceRegionWord] = 0x71U;
    block[Abi::ComponentDeviceFunctionWord] = 0x73U;
}

template <Size Count> void Script(const WaitStep (&steps)[Count]) noexcept
{
    static_assert(Count <= 16U);
    for (Size index{0U}; index < Count; ++index)
    {
        g_kernel.Steps[index] = steps[index];
    }
    g_kernel.StepCount = Count;
}

[[nodiscard]] UInt64 Run(const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const User::System::SystemCallClient client{User::System::SystemCallTransport{&BlockKernel::Transport, &g_kernel}};
    const Component::AhciDriverPlatform platform{.Clock = Clock, .Crash = Crash, .WriteRegister = WriteRegister};
    return Component::ServeAhciDevice(client, block, platform);
}

[[nodiscard]] constexpr UInt64 Failure(const UInt64 step, const SystemCallError error = SystemCallError::None) noexcept
{
    return Abi::ComponentDriverFailedExitCode | (step << 32U) | (static_cast<UInt64>(error) << 40U);
}

// The fixture's disk answers a command after 100 ticks, so 150 complete it.
constexpr UInt64 CommandTicks{150U};

// The driver stops the port it finds running, confines its function with its DMA page, starts the
// port, identifies the disk and connects its Interrupt, all before it reads a request; then a Read,
// a second Read and a Cancel of the second that arrive together find the second still Queued
// (Cancelled, and the Cancel answered Cancelled; a Cancel for another generation is TooLate) while
// the first runs and completes (Done); a Cancel after that is TooLate, and a request for another
// generation DriverRestarted. A Read and its Cancel that arrive together with nothing in flight
// find it Queued too: the whole Endpoint is drained before the next command issues.
void FillBounce() noexcept
{
    for (Size byte{0U}; byte < 512U; ++byte)
    {
        g_window.Bounce[byte] = 0xA5U;
    }
}

void TestNarrowBounce(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_hba.Words[0U] &= ~(UInt32{1U} << 31U);
    Mirror();
    block[Abi::ComponentWindowPagesWord] = Abi::AhciWindowPages;
    for (Size byte{0U}; byte < 512U; ++byte)
    {
        g_window.Client[byte] = 0x3CU;
    }
    g_kernel.Read(1U, 8U, 1U);
    Script({Arrive(1U),
            WaitStep{.Ticks = CommandTicks,
                     .Action = FillBounce,
                     .Arrivals = 0U,
                     .Lost = false,
                     .End = false,
                     .MapAnswer = SystemCallError::None},
            End()});
    static_cast<void>(Run(block));
    bool copied{true};
    for (Size byte{0U}; byte < 512U; ++byte)
    {
        copied = copied && g_window.Client[byte] == 0xA5U;
    }
    ASTERIA_EXPECT(runner, "S64A clear uses a 32-bit domain and only the driver's bounce Frame for client DMA",
                   g_kernel.DmaBits == 32U, g_kernel.IommuMaps == 2U, g_kernel.Mappings[1U].Frame == 0x1006U, copied,
                   g_kernel.Replied(0U, 0x7001U, 1U, Component::BlockDone, 1U), g_hba.ResetCount == 6U);
    Prepare(block);
    g_hba.Words[0U] &= ~(UInt32{1U} << 31U);
    Mirror();
    block[Abi::ComponentWindowPagesWord] = Abi::AhciWindowPages;
    for (Size byte{0U}; byte < 512U; ++byte)
    {
        g_window.Client[byte] = 0x3CU;
    }
    g_kernel.Deliver(Component::BlockWriteMethod, 2U, {Generation, 8U, 1U, 0U});
    Script({Arrive(1U), Elapse(CommandTicks), End()});
    static_cast<void>(Run(block));
    copied = true;
    for (Size byte{0U}; byte < 512U; ++byte)
    {
        copied = copied && g_window.Bounce[byte] == 0x3CU;
    }
    ASTERIA_EXPECT(runner, "32-bit Writes copy the client before exposing only the low driver-owned Frame", copied,
                   g_kernel.Mappings[1U].Frame == 0x1006U,
                   g_kernel.Mappings[1U].Permissions == Abi::IommuMapDeviceRead);
}

void TestBringUpAndCancellation(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_hba.Words[PortCmd] = 0x0001U | 0x0010U | 0x4000U | 0x8000U;
    g_hba.Words[1U] = 2U;
    Mirror();
    g_kernel.Read(1U, 8U, 1U);
    g_kernel.Read(2U, 9U, 1U);
    g_kernel.Cancel(8U, 2U, 0x5100U, Generation - 1U);
    g_kernel.Cancel(3U, 2U);
    g_kernel.Cancel(4U, 1U);
    g_kernel.Read(5U, 8U, 1U, 0x5100U, 0U, Generation - 1U);
    g_kernel.Read(6U, 9U, 1U);
    g_kernel.Cancel(7U, 6U);
    Script({Arrive(4U), Elapse(CommandTicks), Arrive(1U), Arrive(1U), Arrive(2U), End()});
    const UInt64 ended = Run(block);
    constexpr UInt64 BringUp[]{
        static_cast<UInt64>(SystemCallNumber::ObjectCreate),     static_cast<UInt64>(SystemCallNumber::ObjectCreate),
        static_cast<UInt64>(SystemCallNumber::MemoryMap),        static_cast<UInt64>(SystemCallNumber::ObjectCreate),
        static_cast<UInt64>(SystemCallNumber::IommuControl),     static_cast<UInt64>(SystemCallNumber::ObjectCreate),
        static_cast<UInt64>(SystemCallNumber::ObjectCreate),     static_cast<UInt64>(SystemCallNumber::MemoryMap),
        static_cast<UInt64>(SystemCallNumber::IommuControl),     static_cast<UInt64>(SystemCallNumber::DeviceControl),
        static_cast<UInt64>(SystemCallNumber::ObjectCreate),     static_cast<UInt64>(SystemCallNumber::WaitSetBind),
        static_cast<UInt64>(SystemCallNumber::InterruptControl), static_cast<UInt64>(SystemCallNumber::WaitSetWait)};
    bool ordered{true};
    for (Size index{0U}; index < 14U; ++index)
    {
        ordered = ordered && (g_kernel.Numbers[index] == BringUp[index]);
    }
    const UInt64 window = block[Abi::ComponentWindowStartWord];
    ASTERIA_EXPECT(runner, "bring-up maps registers and DMA page, confines, masters, then binds the Interrupt", ordered,
                   g_kernel.Types[0U] == Abi::ObjectTypeFrame, g_kernel.Types[1U] == Abi::ObjectTypeMemoryObject,
                   g_kernel.DmaBits == 64U, g_kernel.Types[2U] == Abi::ObjectTypeIommuDomain,
                   g_kernel.Types[3U] == Abi::ObjectTypeFrame, g_kernel.Types[4U] == Abi::ObjectTypeMemoryObject,
                   g_kernel.Types[5U] == Abi::ObjectTypeInterrupt, g_kernel.MapStarts[0U] == window,
                   g_kernel.MapStarts[1U] == (window + 4096U), g_kernel.Attaches == 1U,
                   g_kernel.Mappings[0U].Frame == 0x1004U, g_kernel.Mappings[0U].Pages == 1U,
                   g_kernel.Mappings[0U].DeviceAddress == 0x0010'0000U, g_kernel.Mappings[0U].Permissions == 3U,
                   g_kernel.DeviceOperations[0U] == Abi::DeviceEnableBusMaster, g_kernel.Binds == 1U,
                   g_kernel.BindSource == InterruptHandle, g_kernel.BindTag == Component::BlockInterruptTag);
    ASTERIA_EXPECT(runner, "the port runs on the DMA page with its interrupts enabled after IDENTIFY DEVICE",
                   g_hba.Words[PortBase] == 0x0010'0000U, g_hba.Words[PortBase + 2U] == 0x0010'0400U,
                   (g_hba.Words[PortCmd] & 0x0011U) == 0x0011U, g_hba.Words[PortIe] == (1U | Tfes),
                   (g_hba.Words[1U] & 0x8000'0002U) == 0x8000'0002U, g_hba.ResetCount == 6U,
                   CountWrites(PortBase) == 1U);
    ASTERIA_EXPECT(runner, "a Read and a Cancel of a queued Read that arrive together: Cancelled, then Done",
                   g_kernel.Replied(0U, 0x9008U, 8U, Component::BlockTooLate, 2U),
                   g_kernel.Replied(1U, 0x7002U, 2U, Component::BlockCancelled, 0U),
                   g_kernel.Replied(2U, 0x9003U, 3U, Component::BlockCancelled, 2U),
                   g_kernel.Replied(3U, 0x7001U, 1U, Component::BlockDone, 1U), g_kernel.IommuMaps == 2U,
                   g_kernel.Mappings[1U].Frame == 0x8001U, g_kernel.Mappings[1U].Pages == 1U,
                   g_kernel.Mappings[1U].DeviceAddress == 0x0020'0000U,
                   g_kernel.Mappings[1U].Permissions == Abi::IommuMapDeviceWrite, g_kernel.Unmaps == 1U,
                   g_kernel.Unmapped[0U] == 0x0020'0000U, g_kernel.Timeouts[1U] == 20'000U, g_kernel.Rearms == 2U);
    const IssuedCommand& identify = g_commands[0U];
    const IssuedCommand& read = g_commands[1U];
    ASTERIA_EXPECT(runner, "IDENTIFY DEVICE reads into the DMA page, then a READ DMA EXT of sector 8 into the window",
                   g_commandCount == 2U, identify.Fis[2U] == 0xECU, Load(identify.Prdt) == 0x0010'0A00U,
                   read.Fis[2U] == 0x25U, read.Fis[4U] == 8U, read.Fis[7U] == 0x40U, read.Fis[12U] == 1U,
                   Load(read.Prdt) == 0x0020'0000U, read.Prdt[12U] == 0xFFU, read.Prdt[13U] == 0x01U);
    ASTERIA_EXPECT(runner, "a Cancel after the completion is TooLate; another generation's Read is DriverRestarted",
                   g_kernel.Replied(4U, 0x9004U, 4U, Component::BlockTooLate, 1U),
                   g_kernel.Replied(5U, 0x7005U, 5U, Component::BlockDriverRestarted, 0U),
                   g_kernel.ClosedHandle(0x7002U), g_kernel.ClosedHandle(0x8002U), g_kernel.ClosedHandle(0x9003U),
                   g_kernel.ClosedHandle(0x8001U), g_kernel.ClosedHandle(0x9004U), g_kernel.ClosedHandle(0x8005U),
                   g_kernel.Timeouts[2U] == 0U);
    ASTERIA_EXPECT(runner, "a Read and its Cancel drained together with nothing in flight: Cancelled, no command",
                   g_kernel.Replied(6U, 0x7006U, 6U, Component::BlockCancelled, 0U),
                   g_kernel.Replied(7U, 0x9007U, 7U, Component::BlockCancelled, 6U), g_kernel.ReplyCount == 8U,
                   g_kernel.Closes == 12U, g_kernel.ClosedHandle(0x9008U), g_commandCount == 2U,
                   g_kernel.IommuMaps == 2U,
                   ended == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle), g_crashes == 0U);
}

// The trusted-driver profile: IommuMap answers device addresses other than those asked for (the
// physical ones), and the driver programs and unmaps exactly those.
void TestAnsweredDeviceAddresses(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.DeviceBase = 0x7F00'0000U;
    g_kernel.Read(1U, 8U, 1U);
    Script({Arrive(1U), Elapse(CommandTicks), End()});
    const UInt64 ended = Run(block);
    constexpr UInt64 Page{0x7F00'0000U + (UInt64{0x1004U} << 12U)};
    constexpr UInt64 Data{0x7F00'0000U + (UInt64{0x8001U} << 12U)};
    ASTERIA_EXPECT(
        runner, "the port, IDENTIFY DEVICE, the transfer and its unmap use the answered addresses",
        g_kernel.Mappings[0U].DeviceAddress == 0x0010'0000U, g_kernel.Mappings[1U].DeviceAddress == 0x0020'0000U,
        g_hba.Words[PortBase] == static_cast<UInt32>(Page), g_hba.Words[PortBase + 1U] == 0U,
        g_hba.Words[PortBase + 2U] == static_cast<UInt32>(Page + 0x400U), g_commandCount == 2U,
        Load(g_commands[0U].Prdt) == Page + 0xA00U, Load(g_commands[1U].Prdt) == Data,
        g_kernel.Replied(0U, 0x7001U, 1U, Component::BlockDone, 1U), g_kernel.Unmaps == 1U,
        g_kernel.Unmapped[0U] == Data, ended == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));
}

// a sender's third request is Refused while it holds its
// Credit; a Write of nine sectors maps two pages for the device to read; a completion whose
// interrupt was lost is found when the wait times out, and the interrupt that follows with nothing
// issued is acknowledged; a request beyond the capacity and one whose Frame the kernel refuses to
// map fail at once, without a command.
void TestCreditPollingAndRefusals(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Read(10U, 0U, 1U, 0x5200U);
    g_kernel.Deliver(Component::BlockWriteMethod, 11U, {Generation, 100U, 9U, 0U}, 0x5200U);
    g_kernel.Read(12U, 0U, 1U, 0x5200U);
    g_kernel.Read(13U, Capacity - 1U, 2U);
    g_kernel.Read(14U, 0U, 16U);
    WaitStep refused = Arrive(1U);
    refused.MapAnswer = SystemCallError::InvalidArguments;
    Script({Arrive(3U), Elapse(CommandTicks), Elapse(CommandTicks, true), Elapse(0U), Arrive(1U), refused, End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a sender's third request is Refused with its credit as detail, the first two served",
                   g_kernel.Replied(0U, 0x700CU, 12U, Component::BlockRefused, Component::BlockMaximumOutstanding),
                   g_kernel.Replied(1U, 0x700AU, 10U, Component::BlockDone, 1U),
                   g_kernel.Replied(2U, 0x700BU, 11U, Component::BlockDone, 9U), g_kernel.ClosedHandle(0x800CU));
    ASTERIA_EXPECT(runner, "a Write of nine sectors maps two pages for the device to read",
                   g_kernel.Mappings[2U].Frame == 0x800BU, g_kernel.Mappings[2U].Pages == 2U,
                   g_kernel.Mappings[2U].Permissions == Abi::IommuMapDeviceRead, g_commandCount == 3U,
                   g_commands[2U].Fis[2U] == 0x35U, g_commands[2U].Fis[4U] == 100U, g_commands[2U].Fis[12U] == 9U,
                   g_commands[2U].Prdt[12U] == 0xFFU, g_commands[2U].Prdt[13U] == 0x11U);
    ASTERIA_EXPECT(runner, "a lost interrupt is covered by the timed wait, and a later one with nothing issued rearms",
                   g_kernel.Timeouts[2U] == 20'000U, g_kernel.Timeouts[3U] == 0U, g_kernel.Rearms == 3U,
                   g_kernel.Unmaps == 2U);
    ASTERIA_EXPECT(runner, "beyond the capacity and an IommuMap refusal fail at once, without a command",
                   g_kernel.Replied(3U, 0x700DU, 13U, Component::BlockFailed, Component::BlockOutOfRange),
                   g_kernel.Replied(4U, 0x700EU, 14U, Component::BlockFailed, Component::BlockMapRefused | 2U),
                   g_kernel.ReplyCount == 5U, g_kernel.IommuMaps == 4U, g_kernel.Unmaps == 2U,
                   g_kernel.ClosedHandle(0x800DU), g_kernel.ClosedHandle(0x800EU), g_hba.Words[PortCi] == 0U,
                   ended == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));
}

// The disk fails the command in flight as QEMU reports an error: no
// completion, PxTFD with ERR and the ATA error (ABRT), PxIS.TFES raising the
// port's interrupt, and PxCI still set.
void FailCommand() noexcept
{
    g_hba.Pending = false;
    g_hba.Words[PortIs] |= Tfes;
    g_hba.Words[PortTfd] = 0x0451U;
    g_hba.Words[2U] |= 1U << DiskPort;
    Mirror();
}

// A controller that clears PxCI on an error (QEMU before 8.2), the status
// in PxTFD and the D2H FIS's interrupt raised, but PxIS.TFES not yet set.
void FailAfterCompletion() noexcept
{
    g_hba.Pending = false;
    g_hba.Words[PortCi] &= ~1U;
    g_hba.Words[PortTfd] = 0x0441U;
    g_hba.Words[PortIs] |= 1U;
    g_hba.Words[2U] |= 1U << DiskPort;
    Mirror();
}

// An error still issued requires the manager's reset; an already completed error can restart the
// engines and report failure.
void TestErrorAndCrash(TestContext& runner) noexcept
{
    const ControllerAction failures[]{FailCommand, FailAfterCompletion};
    for (const ControllerAction failure : failures)
    {
        UInt64 block[Abi::ComponentBlockWords]{};
        Prepare(block);
        g_kernel.Read(20U, 1U, 1U);
        g_kernel.Read(22U, 3U, 1U);
        Script({Arrive(2U), Act(failure), End()});
        const UInt64 ended = Run(block);
        if (failure == FailCommand)
        {
            ASTERIA_EXPECT(runner, "a still-issued error retains the slot and mapping for the manager's reset",
                           ended == Failure(Abi::ComponentDriverDeviceStep), g_kernel.ReplyCount == 0U,
                           g_kernel.Unmaps == 0U, g_commandCount == 2U, CountWrites(PortBase) == 1U,
                           !g_kernel.ClosedHandle(0x8014U));
        }
        else
        {
            ASTERIA_EXPECT(runner, "an already completed error reports failure and safely restarts the engines",
                           ended == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle),
                           g_kernel.Replied(0U, 0x7014U, 20U, Component::BlockFailed, 0x0441U), g_kernel.Unmaps == 1U,
                           g_commandCount == 3U, CountWrites(PortBase) == 2U, g_kernel.ClosedHandle(0x8014U));
        }
    }
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Read(21U, 2U, 1U, 0x5100U, Component::BlockCrashAfterIssue);
    Script({Arrive(1U), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a crash after issue retains the slot and data mapping without a completion reply",
                   g_crashes == 1U, g_crashKind == Abi::ComponentCrashFloatingPoint, g_kernel.ReplyCount == 0U,
                   (g_hba.Words[PortCi] & 1U) == 1U, g_hba.Pending, g_kernel.IommuMaps == 2U,
                   ended == Failure(Abi::ComponentDriverDeviceStep));
}

// Without a disk, or without an IOMMU to confine one, the instance serves every Read BlockFailed
// with BlockNoDevice; a frame that did not come through the sealed Endpoint is dropped with its
// capabilities closed. Without an Interrupt the driver polls its command.
void TestWithoutDiskIommuOrInterrupt(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_hba.Ports[DiskPort].Device = false;
    g_hba.Words[PortBase + 10U] = 0U;
    Mirror();
    g_kernel.Read(30U, 0U, 1U);
    g_kernel.Deliver(Component::BlockReadMethod, 31U, {Generation, 0U, 1U, 0U}, 0x5100U, 0x1234U);
    Script({Arrive(2U), End()});
    const UInt64 diskless = Run(block);
    ASTERIA_EXPECT(runner, "a controller without a disk is served without a command: BlockNoDevice",
                   g_kernel.Replied(0U, 0x701EU, 30U, Component::BlockFailed, Component::BlockNoDevice),
                   g_kernel.ReplyCount == 1U, g_kernel.Creates == 2U, g_kernel.IommuMaps == 0U, g_hba.WriteCount == 0U,
                   g_kernel.ClosedHandle(0x701FU), g_kernel.ClosedHandle(0x801FU), g_kernel.Closes == 4U,
                   diskless == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));

    Prepare(block);
    g_kernel.IommuAnswer = SystemCallError::InvalidConfiguration;
    g_kernel.Read(32U, 0U, 1U);
    Script({Arrive(1U), End()});
    const UInt64 unconfined = Run(block);
    ASTERIA_EXPECT(runner, "without an IOMMU the disk is not used: BlockNoDevice, no DMA, no bus mastering",
                   g_kernel.Replied(0U, 0x7020U, 32U, Component::BlockFailed, Component::BlockNoDevice),
                   g_kernel.Creates == 3U, g_kernel.Controls == 0U, g_kernel.IommuMaps == 0U,
                   (g_hba.Words[PortCmd] & 0x0011U) == 0U,
                   unconfined == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));

    Prepare(block);
    g_kernel.InterruptAnswer = SystemCallError::ResourceExhausted;
    g_kernel.Read(33U, 0U, 1U);
    Script({Arrive(1U), Elapse(CommandTicks), End()});
    const UInt64 polled = Run(block);
    ASTERIA_EXPECT(runner, "an Interrupt the kernel refuses leaves the driver polling its command",
                   g_kernel.Replied(0U, 0x7021U, 33U, Component::BlockDone, 1U), g_kernel.Binds == 0U,
                   g_kernel.Rearms == 0U, g_kernel.Timeouts[1U] == 20'000U, g_hba.Words[PortIe] == 0U,
                   (g_hba.Words[1U] & 2U) == 0U,
                   polled == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));
}

// A controller that is not there, a port that does not stop and a disk
// without 48-bit addressing end the instance before it reads a request.
void TestBringUpFailures(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_hba.Words[0U] = ~UInt32{0U};
    Mirror();
    Script({End()});
    const UInt64 absent = Run(block);
    const Size absentCreates = g_kernel.Creates;

    Prepare(block);
    g_hba.Words[PortCmd] = 0x0001U | 0x8000U;
    g_hba.Ports[DiskPort].StuckCr = true;
    Mirror();
    Script({End()});
    const UInt64 stuck = Run(block);
    const Size stuckCreates = g_kernel.Creates;

    Prepare(block);
    g_hba.Words[User::System::AhciPortWord(0U) + User::System::AhciCmdWord] =
        User::System::AhciSt | User::System::AhciCr;
    g_hba.Ports[0U].StuckCr = true;
    Mirror();
    Script({End()});
    const UInt64 secondary = Run(block);
    const Size secondaryCreates = g_kernel.Creates;
    const Size secondaryControls = g_kernel.Controls;

    Prepare(block);
    g_window.Dma[0xA00U + (83U << 1U) + 1U] = 0U;
    Script({End()});
    const UInt64 unaddressed = Run(block);
    ASTERIA_EXPECT(runner, "an absent controller, a stuck port or a disk without LBA48 end the instance at once",
                   absent == Failure(Abi::ComponentDriverIdentityStep), absentCreates == 2U,
                   stuck == Failure(Abi::ComponentDriverQuiesceStep), stuckCreates == 2U,
                   secondary == Failure(Abi::ComponentDriverQuiesceStep, SystemCallError::TimedOut),
                   secondaryCreates == 2U, secondaryControls == 0U,
                   unaddressed == Failure(Abi::ComponentDriverIdentityStep), g_kernel.Binds == 0U,
                   g_kernel.Waits == 0U);
}

// RunComponent hands the block behaviour to the driver: a register page with
// no port implemented serves BlockNoDevice.
void TestDescribeAndMedium(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Deliver(Component::BlockDescribeMethod, 50U, {Generation, 0U, 0U, 0U});
    g_kernel.Deliver(Component::BlockReadMethod, 51U, {Generation, 0U, 1U, 0U}, 0x5100U, Component::BlockSealedSelector,
                     Generation + 1U);
    g_kernel.Deliver(Component::BlockWriteMethod, 55U, {Generation, 0U, 1U, 0U}, 0x5100U,
                     Component::BlockSealedSelector, Generation + 1U);
    Script({Arrive(3U), End()});
    ASTERIA_EXPECT(
        runner, "Describe reports IDENTIFY capacity and fixed writable media without a data map",
        Run(block) == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle),
        g_kernel.Replies[0U].Method == Component::BlockDescribeReplyMethod, g_kernel.Replies[0U].First == 512U,
        g_kernel.Replies[0U].Kind == Capacity, g_kernel.Replies[0U].Detail == Component::BlockMediumPresent,
        g_kernel.Replies[0U].Extra == Generation, g_kernel.Replied(1U, 0x7033U, 51U, Component::BlockMediumChanged, 0U),
        g_kernel.Replied(2U, 0x7037U, 55U, Component::BlockMediumChanged, 0U), g_kernel.IommuMaps == 1U,
        g_commandCount == 1U, g_kernel.ClosedHandle(0x8033U), g_kernel.ClosedHandle(0x8037U));

    Prepare(block);
    block[Abi::ComponentIncarnationWord] = Generation + 1U;
    g_kernel.Deliver(Component::BlockDescribeMethod, 56U, {Generation + 1U, 0U, 0U, 0U});
    g_kernel.Deliver(Component::BlockReadMethod, 57U, {Generation + 1U, 0U, 1U, 0U});
    Script({Arrive(2U), End()});
    ASTERIA_EXPECT(runner, "a replacement incarnation after state-losing reset advances the medium generation",
                   Run(block) == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle),
                   g_kernel.Replies[0U].Extra == Generation + 1U,
                   g_kernel.Replied(1U, 0x7039U, 57U, Component::BlockMediumChanged, 0U), g_kernel.IommuMaps == 1U);

    Prepare(block);
    g_hba.Ports[DiskPort].Device = false;
    g_hba.Words[PortBase + 10U] = 0U;
    Mirror();
    g_kernel.Deliver(Component::BlockDescribeMethod, 52U, {Generation, 0U, 0U, 0U});
    Script({Arrive(1U), End()});
    ASTERIA_EXPECT(runner, "Describe without a disk reports absence and zero capacity",
                   Run(block) == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle),
                   g_kernel.Replies[0U].Method == Component::BlockDescribeReplyMethod,
                   g_kernel.Replies[0U].First == 512U, g_kernel.Replies[0U].Kind == 0U,
                   g_kernel.Replies[0U].Detail == 0U, g_kernel.Replies[0U].Extra == Generation);

    Prepare(block);
    g_window.Dma[0xA00U + 212U] = 0U;
    g_window.Dma[0xA00U + 213U] = 0x50U;
    g_window.Dma[0xA00U + 235U] = 8U; // 2048 words: 4096-byte logical sectors.
    g_kernel.Deliver(Component::BlockDescribeMethod, 53U, {Generation, 0U, 0U, 0U});
    g_kernel.Read(54U, 0U, 1U);
    Script({Arrive(2U), End()});
    ASTERIA_EXPECT(runner, "non-512 IDENTIFY is refused distinctly by Describe and I/O before client DMA",
                   Run(block) == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle),
                   g_kernel.Replied(0U, 0x7035U, 53U, Component::BlockFailed, Component::BlockUnsupportedSectorSize),
                   g_kernel.Replied(1U, 0x7036U, 54U, Component::BlockFailed, Component::BlockUnsupportedSectorSize),
                   g_kernel.IommuMaps == 1U, g_commandCount == 1U);
}

// A timeout or error cannot release the slot.
void TestFlushFailureKeepsSlot(TestContext& runner) noexcept
{
    for (UInt32 error{0U}; error < 2U; ++error)
    {
        UInt64 block[Abi::ComponentBlockWords]{};
        Prepare(block);
        g_delayFlush = true;
        g_errorFlush = error != 0U;
        g_kernel.Deliver(Component::BlockFlushMethod, 1U, {Generation, 0U, 0U, 0U});
        g_kernel.Read(2U, 8U, 1U);
        Script({Arrive(2U), Elapse(10'100U), End()});
        const UInt64 ended = Run(block);
        ASTERIA_EXPECT(runner, "a failed Flush ends the incarnation with its slot retained for the manager's drain",
                       g_kernel.Replied(0U, 0x7001U, 1U, Component::BlockFailed, Tfes), g_kernel.ReplyCount == 1U,
                       g_commandCount == 2U, g_kernel.IommuMaps == 1U, g_kernel.Unmaps == 0U, g_hba.Pending,
                       (g_hba.Words[PortCi] & 1U) != 0U, g_window.Dma[0x502U] == 0xEAU,
                       ended == Failure(Abi::ComponentDriverServeStep));
    }
}

void FailWithRetainedDma() noexcept
{
    g_hba.Words[PortIs] |= Tfes;
    g_hba.Words[PortTfd] = 0x0451U;
    Mirror();
}

void TestTransferErrorKeepsSlot(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Read(1U, 8U, 1U);
    g_kernel.Read(2U, 9U, 1U);
    Script({Arrive(2U), Act(FailWithRetainedDma), Elapse(CommandTicks), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a transfer error retains the old descriptor and mapping until the manager resets it",
                   ended == Failure(Abi::ComponentDriverDeviceStep), g_commandCount == 2U, g_hba.Pending,
                   g_kernel.Unmaps == 0U, g_kernel.ReplyCount == 0U, g_commands[1U].Fis[4U] == 8U,
                   g_window.Dma[0x504U] == 8U);
}

void TestSuccessfulFlushReleasesSlot(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    g_kernel.Deliver(Component::BlockFlushMethod, 1U, {Generation, 0U, 0U, 0U});
    g_kernel.Read(2U, 8U, 1U);
    Script({Arrive(2U), Elapse(CommandTicks), End()});
    const UInt64 ended = Run(block);
    ASTERIA_EXPECT(runner, "a completed Flush releases the slot for the next transfer",
                   g_kernel.Replied(0U, 0x7001U, 1U, Component::BlockDone, 0U),
                   g_kernel.Replied(1U, 0x7002U, 2U, Component::BlockDone, 1U), g_commandCount == 3U,
                   g_kernel.Unmaps == 1U,
                   ended == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));
}

void TestDispatch(TestContext& runner) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    Prepare(block);
    for (UInt32& word : g_window.Registers)
    {
        word = 0U;
    }
    g_kernel.Read(40U, 0U, 1U);
    Script({Arrive(1U), End()});
    const User::System::SystemCallTransport transport{&BlockKernel::Transport, &g_kernel};
    const UInt64 ended = Component::RunComponent(transport, block, Clock, Crash);
    ASTERIA_EXPECT(runner, "the block behaviour runs the AHCI driver",
                   g_kernel.Replied(0U, 0x7028U, 40U, Component::BlockFailed, Component::BlockNoDevice),
                   ended == Failure(Abi::ComponentDriverServeStep, SystemCallError::InvalidHandle));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"AhciDriverTests"};
    Asteria::Test::TestBringUpAndCancellation(runner);
    Asteria::Test::TestNarrowBounce(runner);
    Asteria::Test::TestCreditPollingAndRefusals(runner);
    Asteria::Test::TestAnsweredDeviceAddresses(runner);
    Asteria::Test::TestErrorAndCrash(runner);
    Asteria::Test::TestWithoutDiskIommuOrInterrupt(runner);
    Asteria::Test::TestBringUpFailures(runner);
    Asteria::Test::TestDispatch(runner);
    Asteria::Test::TestFlushFailureKeepsSlot(runner);
    Asteria::Test::TestTransferErrorKeepsSlot(runner);
    Asteria::Test::TestSuccessfulFlushReleasesSlot(runner);
    Asteria::Test::TestDescribeAndMedium(runner);
    return runner.Finish();
}
