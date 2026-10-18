// SPDX-License-Identifier: Apache-2.0

#include "User/Component/XhciDriver.hpp"

#include "Abi/Clock.hpp"

#include "User/Component/DriverSetup.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Input/Ps2EventQueue.hpp"
#include "User/Component/XhciStorage.hpp"
#include "User/Component/ServiceScratch.hpp"

namespace Asteria::User::Component
{
namespace
{
// The device address the driver asks for its DMA pages, chunk after chunk (DmaChunkPages pages a
// Frame). Each chunk's device address is the one IommuMap answers: contiguous from DmaAddress
// behind a translating IOMMU, the chunk's physical address on the trusted-driver profile, so no
// structure crosses a chunk.
constexpr Foundation::UInt64 DmaAddress{0x0010'0000U};
constexpr Foundation::UInt64 DmaChunkPages{32U};
constexpr Foundation::Size DmaChunkBytes{DmaChunkPages * 4096U};
constexpr Foundation::Size DmaChunks{(Abi::XhciDmaPages + DmaChunkPages - 1U) / DmaChunkPages};
constexpr Foundation::Size Scratchpads{Abi::XhciScratchpadBudget};
// The header's four pages and each port's four, from page 4 on, are
// 16 KiB-aligned; a chunk of 32 pages holds whole ones, and a scratchpad is
// one page. Their rings and buffers keep within 64 KiB only from a 16 KiB-
// aligned device address: a chunk's Frame asks for that physical alignment,
// Which the identity backend's address is, and the driver checks the answer.
static_assert((DmaChunkBytes % 16384U) == 0U);
constexpr Foundation::UInt8 ChunkAlignment{4U};
XhciCore* g_core{nullptr};
constinit Input::Ps2EventQueue g_queue{};
constinit Input::EventBatch g_events{};
constinit Foundation::UInt8 g_frame[InputMaximumFrameBytes]{};
constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + InputMaximumFrameBytes]{};
struct Driver
{
    const System::SystemCallClient* Client;
    System::CapabilityHandle Source;
    System::CapabilityHandle Reply;
    System::CapabilityHandle Receive;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Root;
    System::CapabilityHandle Interrupt;
    System::CapabilityHandle Iommu;
    System::CapabilityHandle Query;
    Foundation::UInt64 Epoch;
    Foundation::UInt64 Transaction;
    Foundation::UInt64 Events;
    Foundation::UInt64 TicksPerMillisecond;
    Foundation::UInt64 LastClock;
    Foundation::UInt64 Flags;
    ComponentClock Clock;
    Foundation::UInt32 BarBytes;
    Foundation::UInt32 Operational;
    Foundation::UInt8* Dma;
    bool PoweredOff[Usb::MaximumPorts];
    Foundation::Size DmaPages;
    Foundation::UInt64 ChunkDevice[DmaChunks];
};
constinit Driver g_driver{};
[[nodiscard]] System::CapabilityHandle Handle(const Foundation::UInt64* block, Foundation::Size word) noexcept
{
    return System::CapabilityHandle{block[word], block[word + 1U]};
}
// The DMA pages at offset, bytes long, at the device address of their chunk; a range across chunks
// gets device address zero, which every user of the memory refuses (Usb::ValidMemory).
[[nodiscard]] Usb::DmaMemory Memory(Foundation::Size offset, Foundation::Size bytes) noexcept
{
    const Foundation::Size chunk = offset / DmaChunkBytes;
    const Foundation::Size within = offset % DmaChunkBytes;
    const bool whole = (chunk < DmaChunks) && (bytes <= (DmaChunkBytes - within));
    return Usb::DmaMemory{whole ? (g_driver.ChunkDevice[chunk] + within) : 0U, g_driver.Dma + offset, bytes};
}
void BuildPortMemory(Foundation::Size index, Foundation::Size context) noexcept
{
    const Foundation::Size start = 16384U + index * 16384U;
    auto& port = g_core->Ports[index];
    port.InputContext = Memory(start, 33U * context);
    port.DeviceContext = Memory(start + 2176U, 32U * context);
    port.ControlRing = Memory(start + 4224U, 256U);
    port.Buffer = Memory(start + 4480U, 1024U);
    port.Pipes = g_core->Pipes[index];
    port.PipeCount = Usb::MaximumEndpointPipes;
    for (Foundation::Size pipe{0U}; pipe < Usb::MaximumEndpointPipes; ++pipe)
    {
        const Foundation::Size offset = start + 5504U + pipe * 1280U;
        g_core->Pipes[index][pipe] = Usb::EndpointMemory{Memory(offset, 256U), Memory(offset + 256U, 1024U)};
    }
}
[[nodiscard]] bool BuildMemory() noexcept
{
    const Foundation::UInt32 hcs = g_core->Storage.Registers().Read32(4U);
    const Foundation::UInt32 hcs2 = g_core->Storage.Registers().Read32(8U);
    const Foundation::Size ports = hcs >> 24U;
    const Foundation::Size scratchpads = ((hcs2 >> 27U) & 31U) | (((hcs2 >> 21U) & 31U) << 5U);
    if ((ports == 0U) || (ports > Usb::MaximumPorts) || (scratchpads > Scratchpads))
    {
        return false;
    }
    const Foundation::Size context = (g_core->Storage.Registers().Read32(16U) & 4U) != 0U ? 64U : 32U;
    g_core->Memory.Dcbaa = Memory(0U, 256U);
    g_core->Memory.CommandRing = Memory(256U, 512U);
    // 512 events cover one change per root plus the bounded device pipes.
    // The component only admits 256 scratchpads, so its address array fits
    // beside this larger event ring in the existing four header pages.
    static_assert(9024U + Scratchpads * 8U <= 16384U);
    g_core->Memory.EventRing = Memory(768U, 8192U);
    g_core->Memory.Erst = Memory(8960U, 64U);
    g_core->Memory.ScratchpadArray = Memory(9024U, Scratchpads * 8U);
    g_core->Memory.Scratchpads = g_core->Scratchpads;
    g_core->Memory.ScratchpadCount = scratchpads;
    g_core->Memory.Ports = g_core->Ports;
    const Foundation::Size slots = (hcs & 255U) < Usb::MaximumDevices ? (hcs & 255U) : Usb::MaximumDevices;
    g_core->Memory.PortCount = slots;
    for (Foundation::Size port{0U}; port < slots; ++port)
    {
        BuildPortMemory(port, context);
    }
    for (Foundation::Size page{0U}; page < scratchpads; ++page)
    {
        g_core->Scratchpads[page] = Memory((68U + page) * 4096U, 4096U);
    }
    return true;
}
[[nodiscard]] Foundation::UInt64 Connect(const Foundation::UInt64* block) noexcept
{
    const auto& client = *g_driver.Client;
    alignas(8) Foundation::UInt8 request[Abi::ObjectCreateInterruptRequestSizeBytes]{};
    System::StoreHandle(request + Abi::ObjectCreateInterruptFunctionOffset,
                        Handle(block, Abi::ComponentDeviceFunctionWord));
    const auto created = client.CreateObject(Handle(block, Abi::ComponentAccountWord), Abi::ObjectTypeInterrupt,
                                             System::AddressOf(request), sizeof(request));
    if (created.Error != Abi::SystemCallError::None)
    {
        // MSI is optional on xHCI. An MSI-X-only function has no MSI
        // capability; the existing timed wait still drains its event ring.
        // No interrupt object or device interrupt authority is then retained.
        if (created.Error == Abi::SystemCallError::InvalidArguments)
        {
            return 0U;
        }
        return DriverFailure(Abi::ComponentDriverInterruptStep, created.Error);
    }
    g_driver.Interrupt = System::CapabilityHandle{created.Value0, created.Value1};
    const auto error = client.BindWaitSet(g_driver.WaitSet, g_driver.Interrupt, XhciInterruptTag).Error;
    if (error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverInterruptStep, error);
    }
    return 0U;
}
[[nodiscard]] Foundation::UInt64 Prepare(const Foundation::UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const auto& client = *g_driver.Client;
    Foundation::UInt64 failure{0U};
    for (Foundation::UInt64 pages{Abi::XhciRegisterWindowPages}; pages != 0U; pages >>= 1U)
    {
        failure = MapRegisters(client, block, pages);
        if (failure == 0U)
        {
            g_driver.BarBytes = static_cast<Foundation::UInt32>(pages * 4096U);
            break;
        }
        if (failure != DriverFailure(Abi::ComponentDriverFrameStep, Abi::SystemCallError::InvalidArguments))
        {
            return failure;
        }
    }
    if (failure != 0U)
    {
        return failure;
    }
    const auto domain = Handle(block, Abi::ComponentAccountWord);
    const auto space = Handle(block, Abi::ComponentAccountSpaceWord);
    const Foundation::UInt64 stateAddress =
        block[Abi::ComponentWindowStartWord] + (Abi::XhciRegisterWindowPages + Abi::XhciDmaPages) * 4096U;
    constexpr Foundation::UInt64 statePages[2U]{Abi::XhciStateFirstPages, Abi::XhciStateLastPages};
    Foundation::UInt64 stateOffset{0U};
    for (const auto pages : statePages)
    {
        System::CapabilityHandle stateFrame{};
        const auto stateMapped =
            MapDmaPage(client, domain, space, stateAddress + stateOffset * 4096U, stateFrame, pages);
        if (stateMapped != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverPagesStep, stateMapped);
        }
        stateOffset += pages;
    }
    // Memory-owner boundary: aligned Frames map contiguous virtual arenas;
    // CPU state is never mapped into the IOMMU. Every subobject fits one Frame.
    auto* const state = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(stateAddress));
    g_core->Storage.Initialize(state, state + Abi::XhciControllerStatePages * 4096U,
                               state + Abi::XhciStateFirstPages * 4096U);
    if ((g_driver.Flags & Abi::XhciReportProgressFlag) != 0U)
    {
        // The root service that set the flag prints these as `# usb:` lines.
        g_core->Storage.Progress().Configure(
            static_cast<Foundation::UInt8>(g_driver.Flags & Abi::XhciControllerIndexMask), true);
        g_core->Storage.Controller().SetDiagnostics(&g_core->Storage.Progress());
        for (Foundation::Size index{0U}; index < Usb::MaximumDevices; ++index)
        {
            g_core->Storage.Device(index).SetDiagnostics(&g_core->Storage.Progress());
        }
    }
    g_core->Storage.Registers().Configure(RegistersOf(block), g_driver.Clock, g_driver.TicksPerMillisecond);
    g_driver.Operational = g_core->Storage.Registers().Read32(0U) & 0xFFU;
    if (!g_core->Storage.Controller().Handoff(g_driver.BarBytes, 1000U) ||
        !g_core->Storage.Controller().QuiesceFirmware(g_driver.BarBytes, 1000U))
    {
        return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::TimedOut);
    }
    failure = ConfineFunction(
        client, block, g_driver.Iommu,
        (g_core->Storage.Registers().Read32(0x10U) & 1U) != 0U ? 64U : 32U); // XHCI 1.2 section 5.3.6, AC64.
    if (failure != 0U)
    {
        return DriverFailure(Abi::ComponentDriverIommuStep, Abi::SystemCallError::InvalidConfiguration);
    }
    const Foundation::UInt32 hcs2 = g_core->Storage.Registers().Read32(8U);
    const Foundation::Size scratchpads = ((hcs2 >> 27U) & 31U) | (((hcs2 >> 21U) & 31U) << 5U);
    if ((hcs2 == 0xFFFF'FFFFU) || (scratchpads > Scratchpads))
    {
        return DriverFailure(Abi::ComponentDriverScratchpadStep, Abi::SystemCallError::ResourceExhausted);
    }
    if ((g_driver.Operational < 32U) || (g_driver.Operational + 12U > g_driver.BarBytes) ||
        ((g_core->Storage.Registers().Read32(g_driver.Operational + 8U) & 0xFFFFU) != 1U))
    {
        return DriverFailure(Abi::ComponentDriverPageSizeStep, Abi::SystemCallError::InvalidConfiguration);
    }
    g_driver.DmaPages = 68U + scratchpads;
    const Foundation::UInt64 address = block[Abi::ComponentWindowStartWord] + Abi::XhciRegisterWindowPages * 4096U;
    for (Foundation::UInt64 offset{0U}; offset < g_driver.DmaPages;)
    {
        const Foundation::UInt64 remaining = g_driver.DmaPages - offset;
        const Foundation::UInt64 pages = remaining < DmaChunkPages ? remaining : DmaChunkPages;
        System::CapabilityHandle frame{};
        const auto mapped = MapDmaPage(client, domain, space, address + offset * 4096U, frame, pages, ChunkAlignment);
        if (mapped != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverPagesStep, mapped);
        }
        const DeviceMapping dma = MapForDevice(client, g_driver.Iommu, frame, pages, DmaAddress + offset * 4096U,
                                               Abi::IommuMapDeviceRead | Abi::IommuMapDeviceWrite);
        if (dma.Error != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverDeviceMapStep, dma.Error);
        }
        if ((dma.Address % (ChunkAlignment * 4096U)) != 0U)
        {
            return DriverFailure(Abi::ComponentDriverDeviceMapStep, Abi::SystemCallError::InvalidArguments);
        }
        g_driver.ChunkDevice[offset / DmaChunkPages] = dma.Address;
        offset += pages;
    }
    g_driver.Dma = reinterpret_cast<Foundation::UInt8*>(static_cast<Foundation::UIntPointer>(address));
    if (!BuildMemory())
    {
        return DriverFailure(Abi::ComponentDriverDeviceMapStep, Abi::SystemCallError::InvalidConfiguration);
    }
    failure = Connect(block);
    if (failure != 0U)
    {
        return failure;
    }
    const auto master =
        client.ControlDevice(Handle(block, Abi::ComponentDeviceFunctionWord), Abi::DeviceEnableBusMaster).Error;
    if (master != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMasterStep, master);
    }
    const auto delivery = g_driver.Interrupt.Selector == 0U ? Usb::EventDelivery::Poll : Usb::EventDelivery::Interrupt;
    if (!g_core->Storage.Controller().Initialize(g_core->Memory, g_driver.BarBytes, 1000U, delivery))
    {
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::InvalidConfiguration);
    }
    g_core->Storage.Controller().PowerPorts();
    const auto powerWait = client.Wait(g_driver.WaitSet, 20'000U);
    if ((powerWait.Error != Abi::SystemCallError::None) && (powerWait.Error != Abi::SystemCallError::TimedOut))
    {
        return DriverFailure(Abi::ComponentDriverDeviceStep, powerWait.Error);
    }
    return 0U;
}
[[nodiscard]] bool Publish() noexcept
{
    const Foundation::Size count = g_queue.Prepare();
    if (count == 0U)
    {
        return true;
    }
    const Foundation::UInt64 words[4U]{g_driver.Epoch, count, 0U, 0U};
    const auto bytes = WriteInputFrame(g_frame, InputPublishMethod, g_queue.GetTransaction(), words,
                                       g_queue.GetEvents(), count, &g_driver.Reply);
    const auto error = g_driver.Client->Send(g_driver.Source, System::AddressOf(g_frame), bytes).Error;
    return (error == Abi::SystemCallError::None) || (error == Abi::SystemCallError::WouldBlock) ||
           (error == Abi::SystemCallError::ResourceExhausted);
}
[[nodiscard]] bool Replies() noexcept
{
    for (Foundation::Size count{0U}; count < 16U; ++count)
    {
        const auto received =
            g_driver.Client->Receive(g_driver.Receive, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            return true;
        }
        InputMessage message{};
        if ((received.Error != Abi::SystemCallError::None) ||
            !ReadInputFrame(g_delivery, static_cast<Foundation::Size>(received.Value1), message))
        {
            return false;
        }
        if ((message.Method == InputQueryMethod) && (g_driver.Query.Selector == 0U))
        {
            g_driver.Query = message.Reply;
            g_driver.Transaction = message.Transaction;
            continue;
        }
        if (message.Reply.Selector != 0U)
        {
            static_cast<void>(g_driver.Client->CloseCapability(message.Reply));
        }
        if ((message.Method == InputReplyMethod) && (message.Transaction == g_queue.GetTransaction()) &&
            (message.Words[0U] == g_driver.Epoch))
        {
            if ((message.Words[1U] < static_cast<Foundation::UInt64>(InputAnswer::Done)) ||
                (message.Words[1U] > static_cast<Foundation::UInt64>(InputAnswer::Restarted)) ||
                !g_queue.Answer(static_cast<InputAnswer>(message.Words[1U])))
            {
                return false;
            }
        }
    }
    return true;
}
[[nodiscard]] bool ReleaseKeys(const Foundation::UInt8 port) noexcept
{
    auto& device = g_core->Storage.Device(g_core->Storage.Controller().DeviceIndex(port));
    for (Foundation::Size interface{0U}; interface < device.InterfaceCount(); ++interface)
    {
        device.ReleaseKeys(interface, g_events);
        for (Foundation::Size index{0U}; index < g_events.Count; ++index)
        {
            if (!g_queue.Append(g_events.Events[index]))
            {
                return false;
            }
        }
    }
    g_events.Count = 0U;
    return true;
}

[[nodiscard]] bool Complete(Foundation::UInt8 port) noexcept
{
    Usb::Completion completion{};
    if (!g_core->Storage.Controller().TakeCompletion(port, completion))
    {
        return true;
    }
    if ((completion.Status == Input::TransferStatus::Disconnected) && !ReleaseKeys(port))
    {
        return false;
    }
    static_cast<void>(g_core->Storage.Device(g_core->Storage.Controller().DeviceIndex(port))
                          .Complete(completion.Token, completion.Status, completion.ActualLength, g_events));
    for (Foundation::Size index{0U}; index < g_events.Count; ++index)
    {
        if (!g_queue.Append(g_events.Events[index]))
        {
            return false;
        }
        ++g_driver.Events; // Diagnostic count intentionally wraps modulo 2^64
    }
    g_events.Count = 0U;
    return true;
}
[[nodiscard]] bool Progress(Foundation::UInt64 milliseconds) noexcept
{
    milliseconds = Abi::ClampClockTicks(milliseconds, g_driver.LastClock);
    g_driver.LastClock = milliseconds;
    g_core->Storage.Controller().Poll(milliseconds, 64U);
    for (Foundation::UInt32 number{1U}; number <= g_core->Storage.Controller().PortCount(); ++number)
    {
        const Foundation::UInt8 port = static_cast<Foundation::UInt8>(number);
        g_driver.PoweredOff[port - 1U] = g_core->Storage.Controller().IsPortPoweredOff(port);
        if (g_core->Storage.Controller().DeviceIndex(port) == Usb::MaximumDevices)
        {
            continue;
        }
        // Retire every pipe completion even if EP0 disconnect already cleared
        // the core's interface list; no old Ready pipe may block a new slot.
        for (Foundation::Size index{0U}; index <= Usb::MaximumEndpointPipes; ++index)
        {
            if (!Complete(port))
            {
                return false;
            }
        }
        auto& device = g_core->Storage.Device(g_core->Storage.Controller().DeviceIndex(port));
        g_driver.PoweredOff[port - 1U] = g_core->Storage.Controller().IsPortPoweredOff(port);
        const auto present = g_core->Storage.Controller().GetPortStatus(port);
        if (!present.Connected)
        {
            // Drain final successful reports before releasing decoder state;
            // otherwise an old report could remake a key after its release.
            if (!ReleaseKeys(port))
            {
                return false;
            }
            device.Disconnect();
            static_cast<void>(g_core->Storage.Controller().ReleasePort(port));
            continue;
        }
        if (device.GetState() == Input::UsbState::Detached)
        {
            static_cast<void>(device.Attach(port));
        }
        Usb::XhciPort host{g_core->Storage.Controller(), port};
        for (Foundation::Size step{0U}; step <= device.InterfaceCount(); ++step)
        {
            device.Step(host);
        }
    }
    g_core->Storage.Progress().Flush(*g_driver.Client, g_driver.Root, g_frame);
    return g_core->Storage.Controller().GetState() == Usb::ControllerState::Running;
}
[[nodiscard]] bool Enumerating() noexcept
{
    for (Foundation::Size port{0U}; port < g_core->Memory.PortCount; ++port)
    {
        switch (g_core->Storage.Device(port).GetState())
        {
        case Input::UsbState::Detached:
        case Input::UsbState::Polling:
        case Input::UsbState::Failed:
        case Input::UsbState::UnsupportedHubDepth:
            break;
        case Input::UsbState::Reset:
        case Input::UsbState::FirstDescriptor:
        case Input::UsbState::EndpointZero:
        case Input::UsbState::Address:
        case Input::UsbState::DeviceDescriptor:
        case Input::UsbState::ConfigurationHeader:
        case Input::UsbState::Configuration:
        case Input::UsbState::SetConfiguration:
        case Input::UsbState::ReportDescriptor:
        case Input::UsbState::SetProtocol:
        case Input::UsbState::SetIdle:
        case Input::UsbState::ConfigureEndpoint:
            return true;
        }
    }
    return false;
}
[[nodiscard]] bool Prime() noexcept
{
    for (Foundation::Size attempt{0U}; attempt < 500U; ++attempt)
    {
        if (!Progress(g_driver.Clock() / g_driver.TicksPerMillisecond))
        {
            return false;
        }
        if (!Enumerating())
        {
            return true;
        }
        if ((g_driver.Interrupt.Selector != 0U) &&
            (g_driver.Client->ControlInterrupt(g_driver.Interrupt, Abi::InterruptRearm).Error !=
             Abi::SystemCallError::None))
        {
            return false;
        }
        const auto waited = g_driver.Client->Wait(g_driver.WaitSet, 10'000U);
        if ((waited.Error != Abi::SystemCallError::None) && (waited.Error != Abi::SystemCallError::TimedOut))
        {
            return false;
        }
    }
    // Slow/non-HID devices do not prevent the controller from serving later
    // connections. Each enumeration operation has its own retirement deadline.
    return true;
}
[[nodiscard]] bool Query() noexcept
{
    if ((g_driver.Query.Selector == 0U) || (g_queue.GetCount() != 0U))
    {
        return true;
    }
    static_assert(Usb::MaximumDevices * Input::MaximumBootInterfaces <= 0xFFFF'FFFFU);
    Foundation::UInt32 bound{0U};
    Foundation::UInt32 poweredOff{0U};
    for (Foundation::Size port{0U}; port < g_core->Memory.PortCount; ++port)
    {
        bound += g_core->Storage.Device(port).GetState() == Input::UsbState::Polling
                     ? static_cast<Foundation::UInt32>(g_core->Storage.Device(port).InterfaceCount())
                     : 0U;
    }
    // Query ABI retains the first 32 root-port power bits; high ports remain
    // independently bookkept and cannot cause an out-of-range shift.
    for (Foundation::Size port{0U}; port < InputQueryPowerShift; ++port)
    {
        poweredOff |= g_driver.PoweredOff[port] ? Foundation::UInt32{1U} << port : 0U;
    }
    const auto size =
        WriteInputDriverQueryReply(g_frame, g_driver.Transaction, g_driver.Epoch, bound, poweredOff, g_driver.Events);
    const auto error = g_driver.Client->Send(g_driver.Query, System::AddressOf(g_frame), size).Error;
    if (error == Abi::SystemCallError::None)
    {
        static_cast<void>(g_driver.Client->CloseCapability(g_driver.Query));
        g_driver.Query = {};
    }
    return (error == Abi::SystemCallError::None) || (error == Abi::SystemCallError::WouldBlock);
}
}
Foundation::UInt64 ServeXhciDevice(const System::SystemCallClient& client,
                                   const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                   const ComponentClock clock) noexcept
{
    Scratch().InitializeXhci();
    g_core = &Scratch().Xhci();
    g_driver.Client = &client;
    g_driver.Clock = clock;
    g_driver.Source = Handle(block, Abi::ComponentInputServiceWord);
    g_driver.Reply = Handle(block, Abi::ComponentReplyWord);
    g_driver.Receive = Handle(block, Abi::ComponentReceiveWord);
    g_driver.WaitSet = Handle(block, Abi::ComponentWaitSetWord);
    g_driver.Root = Handle(block, Abi::ComponentReportWord);
    g_driver.Epoch = block[Abi::ComponentInputEpochWord];
    g_driver.Flags = block[Abi::ComponentMountFlagsWord];
    const auto scale = block[Abi::ComponentClockTicksPerMicrosecondWord];
    if ((scale == 0U) || (scale > ~Foundation::UInt64{0U} / 1000U))
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::InvalidConfiguration);
    }
    g_driver.TicksPerMillisecond = scale * 1000U;
    const auto failure = Prepare(block);
    if (failure != 0U)
    {
        return failure;
    }
    if (!Prime())
    {
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::TimedOut);
    }
    // Diagnostics queued on the same report Endpoint may fill this sender's
    // share of it for a moment; the root service drains it while it waits.
    Abi::SystemCallError ready{Abi::SystemCallError::WouldBlock};
    for (Foundation::Size attempt{0U}; (attempt < 500U) && (ready == Abi::SystemCallError::WouldBlock); ++attempt)
    {
        WriteEchoFrame(g_frame, nullptr, 0U);
        Detail::InputStore32(g_frame + 32U, XhciReadyMethod);
        ready = client.Send(g_driver.Root, System::AddressOf(g_frame), EchoHeaderBytes).Error;
        if (ready == Abi::SystemCallError::ResourceExhausted)
        {
            ready = Abi::SystemCallError::WouldBlock;
        }
        if (ready == Abi::SystemCallError::WouldBlock)
        {
            const auto waited = client.Wait(g_driver.WaitSet, 10'000U);
            if ((waited.Error != Abi::SystemCallError::None) && (waited.Error != Abi::SystemCallError::TimedOut))
            {
                return DriverFailure(Abi::ComponentDriverServeStep, waited.Error);
            }
        }
    }
    if (ready != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
    }
    for (;;)
    {
        if (!Progress(clock() / g_driver.TicksPerMillisecond) || !Replies() || !Publish() || !Query())
        {
            return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
        }
        if ((g_driver.Interrupt.Selector != 0U) &&
            (client.ControlInterrupt(g_driver.Interrupt, Abi::InterruptRearm).Error != Abi::SystemCallError::None))
        {
            return DriverFailure(Abi::ComponentDriverInterruptStep, Abi::SystemCallError::None);
        }
        const auto waited = client.Wait(g_driver.WaitSet, 10'000U);
        if ((waited.Error != Abi::SystemCallError::None) && (waited.Error != Abi::SystemCallError::TimedOut))
        {
            return DriverFailure(Abi::ComponentDriverServeStep, waited.Error);
        }
    }
}
} // Namespace Asteria::User::Component
