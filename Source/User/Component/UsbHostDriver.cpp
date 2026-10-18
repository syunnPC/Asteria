// SPDX-License-Identifier: Apache-2.0

#include "User/Component/UsbHostDriver.hpp"

#include "Abi/Clock.hpp"

#include "User/Component/DriverSetup.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Component/UsbProgressFrame.hpp"
#include "User/Component/XhciDriver.hpp"
#include "User/Component/XhciProgress.hpp"
#include "User/Input/Ps2EventQueue.hpp"
#include "User/System/UsbHostRegisters.hpp"

namespace Asteria::User::Component
{
namespace
{
struct UsbHostPlacement
{
    void* Address;
};
} // Namespace
} // Namespace Asteria::User::Component

// Placement into the image's zero-filled storage: the component ABI has no
// initialized writable data, so objects with vtables or non-zero initial
// state are built here after entry.
[[nodiscard]] inline void* operator new(Asteria::Foundation::Size,
                                        const Asteria::User::Component::UsbHostPlacement place) noexcept
{
    return place.Address;
}

namespace Asteria::User::Component
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

constexpr Size Hosts{Abi::UsbHostControllers};
// Devices at once across the group; each UsbDevice holds its descriptor and
// report buffers (about 10 KiB).
constexpr Size Devices{6U};
static_assert(Devices * Input::MaximumBootInterfaces <= 0xFFFF'FFFFU);
// Logical pipes per controller: the EHCI library's 16, and Usb1's.
constexpr UInt8 Pipes{16U};
static_assert(Pipes == Usb::Usb1MaximumPipes);
// Interrupt IN endpoints a device polls, each on a pipe of its own.
constexpr Size EndpointPipes{3U};
// The register block's page and the poll counts: a firmware handoff is
// Paced at a millisecond a read (one second); register waits are reads.
constexpr UInt32 HandoffPolls{1000U};
constexpr UInt32 RegisterPolls{100000U};
// An idle interrupt poll keeps its controller this long before it yields
// to another device's or interface's request.
constexpr UInt64 SliceMilliseconds{24U};
// A device whose poll returned a report keeps the controller for this many
// polls in a row, so a burst its device queued drains, before others' turn.
constexpr UInt8 StreakPolls{4U};
// While devices report, or a request other than an idle poll is in flight,
// the loop looks every millisecond (the libraries' cadence while DMA is
// active); after this long without a report it rests on the ten-millisecond
// backstop, which the controllers' interrupts shorten where they have them.
constexpr UInt64 ActiveMilliseconds{100U};
constexpr UInt64 IdleWaitMicroseconds{10'000U};
constexpr UInt64 BusyWaitMicroseconds{1'000U};
// Companions report a released device after their own debounce: readiness
// waits at least this long after start, and at most the xHCI driver's five
// seconds of enumeration.
constexpr UInt64 SettleMilliseconds{1500U};
constexpr Size PrimeRounds{500U};
constexpr UInt64 DmaAddress{0x0010'0000U};
constexpr UInt64 InterruptTag{0x5553'4248'0000'0000ULL};

enum class HostKind : UInt8
{
    None,
    Ehci,
    Ohci,
    Uhci,
};

struct Host
{
    System::CapabilityHandle Region;
    System::CapabilityHandle Function;
    System::CapabilityHandle Interrupt;
    System::UsbHostMmio* Mmio;
    System::UsbHostPorts* Io;
    Usb::EhciController* Ehci;
    Usb::OhciController* Ohci;
    Usb::UhciController* Uhci;
    XhciProgress* Progress;
    UInt32 Operational;
    UInt32 Bytes;
    UInt16 PipesInUse;
    HostKind Kind;
    UInt8 Index;
    UInt8 Ports;
    UInt8 Next;
    bool Failed;
    bool Seen[15];
};

class SlotView;

struct Slot
{
    Input::UsbDevice* Device;
    SlotView* View;
    UInt64 Since;
    UInt8 Host;
    UInt8 Port;
    UInt8 Control;
    UInt8 EndpointCount;
    UInt8 Endpoint[EndpointPipes];
    UInt8 Address[EndpointPipes];
    // Polls in a row that returned a report; nonzero gives it the next turn.
    UInt8 Streak;
};

struct Driver
{
    const System::SystemCallClient* Client;
    System::CapabilityHandle Source;
    System::CapabilityHandle Reply;
    System::CapabilityHandle Receive;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Root;
    System::CapabilityHandle Iommu;
    System::CapabilityHandle Query;
    UInt64 Epoch;
    UInt64 Transaction;
    UInt64 Events;
    UInt64 TicksPerMillisecond;
    UInt64 Flags;
    UInt64 Started;
    UInt64 LastClock;
    UInt64 ActiveUntil;
    ComponentClock Clock;
    UInt8* Dma;
    UInt64 DmaDevice;
    Size HostCount;
};

constinit Driver g_driver{};
constinit Host g_hosts[Hosts]{};
constinit Slot g_slots[Devices]{};
constinit Input::Ps2EventQueue g_queue{};
constinit Input::EventBatch g_events{};
constinit UInt8 g_frame[InputMaximumFrameBytes]{};
constinit UInt8 g_delivery[EchoDeliveryHeaderBytes + InputMaximumFrameBytes]{};
alignas(System::UsbHostMmio) constinit UInt8 g_mmio[Hosts][sizeof(System::UsbHostMmio)]{};
alignas(System::UsbHostPorts) constinit UInt8 g_ports[Hosts][sizeof(System::UsbHostPorts)]{};
alignas(Usb::EhciController) constinit UInt8 g_ehci[Hosts][sizeof(Usb::EhciController)]{};
alignas(Usb::OhciController) constinit UInt8 g_ohci[Hosts][sizeof(Usb::OhciController)]{};
alignas(Usb::UhciController) constinit UInt8 g_uhci[Hosts][sizeof(Usb::UhciController)]{};
alignas(XhciProgress) constinit UInt8 g_progress[Hosts][sizeof(XhciProgress)]{};
alignas(Input::UsbDevice) constinit UInt8 g_devices[Devices][sizeof(Input::UsbDevice)]{};

[[nodiscard]] UInt64 Now() noexcept
{
    g_driver.LastClock = Abi::ClampClockTicks(g_driver.Clock() / g_driver.TicksPerMillisecond, g_driver.LastClock);
    return g_driver.LastClock;
}

[[nodiscard]] System::CapabilityHandle Handle(const UInt64* const block, const Size word) noexcept
{
    return System::CapabilityHandle{block[word], block[word + 1U]};
}

// The library behind one controller, by kind.
void HostPoll(Host& host, const UInt64 milliseconds) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        host.Ehci->Poll(milliseconds);
        return;
    case HostKind::Ohci:
        host.Ohci->Poll(milliseconds);
        return;
    case HostKind::Uhci:
        host.Uhci->Poll(milliseconds);
        return;
    case HostKind::None:
        return;
    }
}

[[nodiscard]] bool HostRunning(const Host& host) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Ehci->GetState() == Usb::EhciState::Running;
    case HostKind::Ohci:
        return host.Ohci->GetState() == Usb::Usb1State::Running;
    case HostKind::Uhci:
        return (host.Uhci->GetState() == Usb::Usb1State::Running) && !host.Io->Refused();
    case HostKind::None:
        return false;
    }
    return false;
}

[[nodiscard]] bool HostBusy(const Host& host) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Ehci->IsBusy();
    case HostKind::Ohci:
        return host.Ohci->IsBusy();
    case HostKind::Uhci:
        return host.Uhci->IsBusy();
    case HostKind::None:
        return false;
    }
    return false;
}

[[nodiscard]] Input::PortStatus HostStatus(Host& host, const UInt8 port) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Ehci->GetPortStatus(port);
    case HostKind::Ohci:
        return host.Ohci->GetPortStatus(port);
    case HostKind::Uhci:
        return host.Uhci->GetPortStatus(port);
    case HostKind::None:
        break;
    }
    return {.Connected = false, .Speed = Input::UsbSpeed::Full};
}

[[nodiscard]] bool HostSubmit(Host& host, const UInt8 pipe, const UInt8 port, const Input::UsbRequest& request) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Ehci->Submit(pipe, port, request);
    case HostKind::Ohci:
        return host.Ohci->Submit(pipe, port, request);
    case HostKind::Uhci:
        return host.Uhci->Submit(pipe, port, request);
    case HostKind::None:
        return false;
    }
    return false;
}

[[nodiscard]] bool HostAbort(Host& host, const UInt8 pipe, const UInt64 token) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Ehci->Abort(pipe, token);
    case HostKind::Ohci:
        return host.Ohci->Abort(pipe, token);
    case HostKind::Uhci:
        return host.Uhci->Abort(pipe, token);
    case HostKind::None:
        return false;
    }
    return false;
}

[[nodiscard]] bool HostTake(Host& host, const UInt8 pipe, Usb::Usb1Completion& output) noexcept
{
    if (host.Kind == HostKind::Ehci)
    {
        Usb::EhciCompletion completion{};
        if (!host.Ehci->TakeCompletion(pipe, completion))
        {
            return false;
        }
        output = {.Token = completion.Token, .Status = completion.Status, .ActualLength = completion.ActualLength};
        return true;
    }
    return (host.Kind == HostKind::Ohci) ? host.Ohci->TakeCompletion(pipe, output)
                                         : ((host.Kind == HostKind::Uhci) && host.Uhci->TakeCompletion(pipe, output));
}

// The raw port status register, for diagnostics: EHCI PORTSC (EHCI 1.0
// section 2.3.9), OHCI HcRhPortStatus (OpenHCI 1.0a section 7.4.4), UHCI
// PORTSC (UHCI 1.1 section 2.1.7).
[[nodiscard]] UInt32 PortRegister(Host& host, const UInt8 port) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Mmio->Read32(host.Operational + 0x44U + (UInt32{port} - 1U) * 4U);
    case HostKind::Ohci:
        return host.Mmio->Read32(0x54U + (UInt32{port} - 1U) * 4U);
    case HostKind::Uhci:
        return host.Io->Read16(0x10U + (UInt32{port} - 1U) * 2U);
    case HostKind::None:
        return 0U;
    }
    return 0U;
}

// The controller's status register for a fault record.
[[nodiscard]] UInt32 StatusRegister(Host& host) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
        return host.Mmio->Read32(host.Operational + 4U);
    case HostKind::Ohci:
        return host.Mmio->Read32(0x0CU);
    case HostKind::Uhci:
        return host.Io->Read16(2U);
    case HostKind::None:
        return 0U;
    }
    return 0U;
}

void Trace(Host& host, const Input::UsbDiagnosticKind kind, const UInt8 port, const UInt8 detail,
           const UInt32 value) noexcept
{
    host.Progress->Record(
        {.Kind = kind, .Port = port, .Detail = detail, .Code = 0U, .Value0 = value, .Value1 = 0U, .Data = 0U});
}

[[nodiscard]] UInt8 AllocatePipe(Host& host) noexcept
{
    for (UInt8 pipe{1U}; pipe <= Pipes; ++pipe)
    {
        const UInt16 bit = static_cast<UInt16>(1U << (pipe - 1U));
        if ((host.PipesInUse & bit) == 0U)
        {
            host.PipesInUse = static_cast<UInt16>(host.PipesInUse | bit);
            return pipe;
        }
    }
    return 0U;
}

// One device's view of its controller (Input::IUsbHostController): control requests on its control
// pipe, each interrupt IN endpoint's configuration and polls on a pipe of its own, so a composite
// device keeps one DATA toggle per endpoint (USB 2.0 section 8.6).
class SlotView final : public Input::IUsbHostController
{
public:
    explicit SlotView(Slot& slot) noexcept : m_slot{slot}
    {
    }
    [[nodiscard]] Input::PortStatus GetPortStatus() noexcept override
    {
        return HostStatus(g_hosts[m_slot.Host], m_slot.Port);
    }
    [[nodiscard]] bool Submit(const Input::UsbRequest& request) noexcept override
    {
        Host& host = g_hosts[m_slot.Host];
        UInt8 pipe = m_slot.Control;
        if ((request.Operation == Input::UsbOperation::ConfigureEndpoint) ||
            (request.Operation == Input::UsbOperation::InterruptIn))
        {
            pipe = EndpointPipe(host, request.Endpoint.Address,
                                request.Operation == Input::UsbOperation::ConfigureEndpoint);
        }
        if ((pipe != 0U) && HostSubmit(host, pipe, m_slot.Port, request))
        {
            m_slot.Since = Now();
            return true;
        }
        return false;
    }
    [[nodiscard]] bool Abort(const UInt64 token) noexcept override
    {
        Host& host = g_hosts[m_slot.Host];
        if (HostAbort(host, m_slot.Control, token))
        {
            return true;
        }
        for (Size index{0U}; index < m_slot.EndpointCount; ++index)
        {
            if (HostAbort(host, m_slot.Endpoint[index], token))
            {
                return true;
            }
        }
        return false;
    }

private:
    // The endpoint's pipe; ConfigureEndpoint takes a new one for an endpoint
    // the device has not configured yet. Zero when none is left.
    [[nodiscard]] UInt8 EndpointPipe(Host& host, const UInt8 address, const bool configure) noexcept
    {
        for (Size index{0U}; index < m_slot.EndpointCount; ++index)
        {
            if (m_slot.Address[index] == address)
            {
                return m_slot.Endpoint[index];
            }
        }
        if (!configure || (m_slot.EndpointCount == EndpointPipes))
        {
            return 0U;
        }
        const UInt8 pipe = AllocatePipe(host);
        if (pipe != 0U)
        {
            m_slot.Endpoint[m_slot.EndpointCount] = pipe;
            m_slot.Address[m_slot.EndpointCount] = address;
            ++m_slot.EndpointCount;
        }
        return pipe;
    }

    Slot& m_slot;
};

alignas(SlotView) constinit UInt8 g_views[Devices][sizeof(SlotView)]{};

[[nodiscard]] bool Enumerating(const Input::UsbState state) noexcept
{
    switch (state)
    {
    case Input::UsbState::Detached:
    case Input::UsbState::Polling:
    case Input::UsbState::Failed:
    case Input::UsbState::UnsupportedHubDepth:
        return false;
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
    return false;
}

[[nodiscard]] bool AppendEvents() noexcept
{
    for (Size index{0U}; index < g_events.Count; ++index)
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

// Every completion of the slot's pipes reaches its USB core.
[[nodiscard]] bool Deliver(Slot& slot) noexcept
{
    Host& host = g_hosts[slot.Host];
    for (Size index{0U}; index <= slot.EndpointCount; ++index)
    {
        const UInt8 pipe = (index == 0U) ? slot.Control : slot.Endpoint[index - 1U];
        Usb::Usb1Completion completion{};
        while (HostTake(host, pipe, completion))
        {
            if ((host.Kind == HostKind::Ehci) && (completion.Status == Input::TransferStatus::Disconnected) &&
                (slot.Device->GetState() == Input::UsbState::Reset) &&
                ((PortRegister(host, slot.Port) & (1U << 13U)) != 0U))
            {
                // EHCI 1.0 section 4.2.2: reset left a full- or low-speed
                // device disabled; Port Owner released it to a companion.
                Trace(host, Input::UsbDiagnosticKind::Companion, slot.Port, 0U, PortRegister(host, slot.Port));
            }
            const bool polling = slot.Device->GetState() == Input::UsbState::Polling;
            const bool report =
                polling && (completion.Status == Input::TransferStatus::Success) && (completion.ActualLength != 0U);
            slot.Streak = report ? static_cast<UInt8>(slot.Streak + 1U) : 0U;
            if (report || !polling)
            {
                g_driver.ActiveUntil = Now() + ActiveMilliseconds;
            }
            static_cast<void>(
                slot.Device->Complete(completion.Token, completion.Status, completion.ActualLength, g_events));
            if (!AppendEvents())
            {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool ReleaseKeys(Slot& slot) noexcept
{
    for (Size interface{0U}; interface < slot.Device->InterfaceCount(); ++interface)
    {
        slot.Device->ReleaseKeys(interface, g_events);
        if (!AppendEvents())
        {
            return false;
        }
    }
    return true;
}

// The slot's device left: its held keys are released, its pipes return
// to the controller (their completions already delivered) and it is free.
[[nodiscard]] bool Free(Slot& slot) noexcept
{
    Host& host = g_hosts[slot.Host];
    if (!Deliver(slot) || !ReleaseKeys(slot))
    {
        return false;
    }
    slot.Device->Disconnect();
    host.PipesInUse = static_cast<UInt16>(host.PipesInUse & ~(1U << (slot.Control - 1U)));
    for (Size index{0U}; index < slot.EndpointCount; ++index)
    {
        host.PipesInUse = static_cast<UInt16>(host.PipesInUse & ~(1U << (slot.Endpoint[index] - 1U)));
    }
    slot.Port = 0U;
    slot.EndpointCount = 0U;
    slot.Control = 0U;
    return true;
}

[[nodiscard]] Slot* SlotOf(const Size host, const UInt8 port) noexcept
{
    for (Slot& slot : g_slots)
    {
        if ((slot.Port == port) && (slot.Host == host))
        {
            return &slot;
        }
    }
    return nullptr;
}

// A device attached to port, if a slot and a control pipe are free; one
// that finds none waits for the next pass. Its USB address is its root
// port: each controller is a bus of its own.
void Attach(Host& host, const UInt8 port) noexcept
{
    for (Slot& slot : g_slots)
    {
        if (slot.Port != 0U)
        {
            continue;
        }
        const UInt8 pipe = AllocatePipe(host);
        if ((pipe == 0U) || !slot.Device->Attach(port))
        {
            host.PipesInUse =
                static_cast<UInt16>(pipe == 0U ? host.PipesInUse : host.PipesInUse & ~(1U << (pipe - 1U)));
            return;
        }
        slot.Host = host.Index;
        slot.Port = port;
        slot.Control = pipe;
        slot.EndpointCount = 0U;
        slot.Streak = 0U;
        slot.Device->SetDiagnostics(host.Progress);
        return;
    }
}

// Whether a device other than active on host wants the controller, or
// active itself has another interface to poll.
[[nodiscard]] bool Contended(const Size host, const Slot& active) noexcept
{
    if (active.Device->InterfaceCount() > 1U)
    {
        return true;
    }
    for (const Slot& slot : g_slots)
    {
        if ((&slot != &active) && (slot.Port != 0U) && (slot.Host == host) &&
            (Enumerating(slot.Device->GetState()) || (slot.Device->GetState() == Input::UsbState::Polling)))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool Wants(const Slot& slot, const Size host) noexcept
{
    return (slot.Port != 0U) && (slot.Host == host) && !slot.Device->IsOutstanding() &&
           (Enumerating(slot.Device->GetState()) || (slot.Device->GetState() == Input::UsbState::Polling));
}

// One request at a time per controller: an idle controller takes the next
// slot (round robin) with a request to make, but a device whose last polls
// returned reports keeps it for StreakPolls polls; an idle interrupt poll
// that held it a slice yields when another request waits. An aborted poll
// completes Cancelled, or Success with a report that won the race.
void Schedule(Host& host, const UInt64 now) noexcept
{
    if (HostBusy(host))
    {
        for (Slot& slot : g_slots)
        {
            if ((slot.Port != 0U) && (slot.Host == host.Index) && slot.Device->IsOutstanding() &&
                (slot.Device->GetState() == Input::UsbState::Polling) &&
                ((Abi::ElapsedClockTicks(now, slot.Since)) >= SliceMilliseconds) && Contended(host.Index, slot))
            {
                // Its retirement completes on the next passes.
                g_driver.ActiveUntil = now + ActiveMilliseconds;
                static_cast<void>(slot.Device->Abort(*slot.View));
            }
        }
        return;
    }
    for (Slot& slot : g_slots)
    {
        if (Wants(slot, host.Index) && (slot.Streak != 0U) && (slot.Streak < StreakPolls))
        {
            slot.Device->Step(*slot.View);
            if (HostBusy(host))
            {
                return;
            }
        }
    }
    for (Size offset{0U}; offset < Devices; ++offset)
    {
        const Size index = (host.Next + offset) % Devices;
        Slot& slot = g_slots[index];
        if (!Wants(slot, host.Index))
        {
            continue;
        }
        slot.Streak = 0U;
        slot.Device->Step(*slot.View);
        if (HostBusy(host))
        {
            host.Next = static_cast<UInt8>((index + 1U) % Devices);
            return;
        }
    }
}

// One controller's pass: its library first, then every completion to its
// core, then port changes, then the next request.
[[nodiscard]] bool Service(Host& host, const UInt64 now) noexcept
{
    if (host.Failed)
    {
        return true;
    }
    HostPoll(host, now);
    for (Slot& slot : g_slots)
    {
        if ((slot.Port != 0U) && (slot.Host == host.Index) && !Deliver(slot))
        {
            return false;
        }
    }
    if (!HostRunning(host))
    {
        // A faulted controller stops; its devices are gone, the group's
        // others continue.
        Trace(host, Input::UsbDiagnosticKind::Controller, 0U, 0U, StatusRegister(host));
        host.Failed = true;
        for (Slot& slot : g_slots)
        {
            if ((slot.Port != 0U) && (slot.Host == host.Index) && !Free(slot))
            {
                return false;
            }
        }
        return true;
    }
    for (UInt8 port{1U}; port <= host.Ports; ++port)
    {
        const bool connected = HostStatus(host, port).Connected;
        Slot* const slot = SlotOf(host.Index, port);
        if (connected != host.Seen[port - 1U])
        {
            Trace(host, connected ? Input::UsbDiagnosticKind::Connect : Input::UsbDiagnosticKind::Disconnect, port,
                  (host.Kind == HostKind::Ehci) ? 2U : 1U, PortRegister(host, port));
            host.Seen[port - 1U] = connected;
        }
        if (!connected && (slot != nullptr))
        {
            if (!Free(*slot))
            {
                return false;
            }
            continue;
        }
        if (connected && (slot == nullptr))
        {
            Attach(host, port);
        }
    }
    Schedule(host, now);
    return true;
}

[[nodiscard]] bool Progress(const UInt64 now) noexcept
{
    bool running{false};
    for (Size index{0U}; index < g_driver.HostCount; ++index)
    {
        if (!Service(g_hosts[index], now))
        {
            return false;
        }
        running = running || !g_hosts[index].Failed;
        g_hosts[index].Progress->Flush(*g_driver.Client, g_driver.Root, g_frame);
    }
    return running;
}

[[nodiscard]] bool AnyEnumerating() noexcept
{
    for (const Slot& slot : g_slots)
    {
        if ((slot.Port != 0U) && Enumerating(slot.Device->GetState()))
        {
            return true;
        }
    }
    return false;
}

// Whether a request other than an idle interrupt poll is in flight, which
// the next pass should look at sooner.
[[nodiscard]] bool Hurried() noexcept
{
    for (const Slot& slot : g_slots)
    {
        if ((slot.Port != 0U) && slot.Device->IsOutstanding() && (slot.Device->GetState() != Input::UsbState::Polling))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool Rearm() noexcept
{
    for (Size index{0U}; index < g_driver.HostCount; ++index)
    {
        const System::CapabilityHandle interrupt = g_hosts[index].Interrupt;
        if ((interrupt.Selector != 0U) &&
            (g_driver.Client->ControlInterrupt(interrupt, Abi::InterruptRearm).Error != Abi::SystemCallError::None))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Abi::SystemCallError Pause() noexcept
{
    const bool active = Hurried() || AnyEnumerating() || (Now() < g_driver.ActiveUntil);
    const Abi::SystemCallError error =
        g_driver.Client->Wait(g_driver.WaitSet, active ? BusyWaitMicroseconds : IdleWaitMicroseconds).Error;
    return (error == Abi::SystemCallError::TimedOut) ? Abi::SystemCallError::None : error;
}

[[nodiscard]] bool Publish() noexcept
{
    const Size count = g_queue.Prepare();
    if (count == 0U)
    {
        return true;
    }
    const UInt64 words[4U]{g_driver.Epoch, count, 0U, 0U};
    const auto bytes = WriteInputFrame(g_frame, InputPublishMethod, g_queue.GetTransaction(), words,
                                       g_queue.GetEvents(), count, &g_driver.Reply);
    const auto error = g_driver.Client->Send(g_driver.Source, System::AddressOf(g_frame), bytes).Error;
    return (error == Abi::SystemCallError::None) || (error == Abi::SystemCallError::WouldBlock) ||
           (error == Abi::SystemCallError::ResourceExhausted);
}

[[nodiscard]] bool Replies() noexcept
{
    for (Size count{0U}; count < 16U; ++count)
    {
        const auto received =
            g_driver.Client->Receive(g_driver.Receive, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            return true;
        }
        InputMessage message{};
        if ((received.Error != Abi::SystemCallError::None) ||
            !ReadInputFrame(g_delivery, static_cast<Size>(received.Value1), message))
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
            if ((message.Words[1U] < static_cast<UInt64>(InputAnswer::Done)) ||
                (message.Words[1U] > static_cast<UInt64>(InputAnswer::Restarted)) ||
                !g_queue.Answer(static_cast<InputAnswer>(message.Words[1U])))
            {
                return false;
            }
        }
    }
    return true;
}

// Query is a publication fence: word 2 has the polling interfaces of every
// controller, word 3 the decoded events.
[[nodiscard]] bool Query() noexcept
{
    if ((g_driver.Query.Selector == 0U) || (g_queue.GetCount() != 0U))
    {
        return true;
    }
    UInt32 bound{0U};
    for (const Slot& slot : g_slots)
    {
        bound += ((slot.Port != 0U) && (slot.Device->GetState() == Input::UsbState::Polling))
                     ? static_cast<UInt32>(slot.Device->InterfaceCount())
                     : 0U;
    }
    const auto size =
        WriteInputDriverQueryReply(g_frame, g_driver.Transaction, g_driver.Epoch, bound, 0U, g_driver.Events);
    const auto error = g_driver.Client->Send(g_driver.Query, System::AddressOf(g_frame), size).Error;
    if (error == Abi::SystemCallError::None)
    {
        static_cast<void>(g_driver.Client->CloseCapability(g_driver.Query));
        g_driver.Query = {};
    }
    return (error == Abi::SystemCallError::None) || (error == Abi::SystemCallError::WouldBlock);
}

// The register block of host k: a memory BAR's page mapped into the window
// at page k, at the BAR's offset within that page.
[[nodiscard]] UInt64 MapBlock(const UInt64 (&block)[Abi::ComponentBlockWords], Host& host, const Size k) noexcept
{
    const auto& client = *g_driver.Client;
    UInt32 bar{0U};
    if (!host.Mmio->Access().ReadConfiguration(0x10U, bar) || ((bar & 1U) != 0U))
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::InvalidConfiguration);
    }
    const auto domain = Handle(block, Abi::ComponentAccountWord);
    const Abi::SystemCallResponse frame = System::CreateDeviceFrame(client, domain, host.Region, 1U);
    if (frame.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverFrameStep, frame.Error);
    }
    const Abi::SystemCallResponse memory =
        System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, 1U, Abi::ObjectCreateCacheUncached);
    if (memory.Error != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMemoryObjectStep, memory.Error);
    }
    const UInt64 address = block[Abi::ComponentWindowStartWord] + k * 4096U;
    const Abi::SystemCallError mapped = System::MapWindow(
        client, Handle(block, Abi::ComponentAccountSpaceWord), System::CapabilityHandle{memory.Value0, memory.Value1},
        System::CapabilityHandle{frame.Value0, frame.Value1}, address, 1U);
    if (mapped != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverMapStep, mapped);
    }
    // The kernel published the BAR's whole page (UsbHostPublication.hpp).
    const UInt32 within = (bar & ~UInt32{0xFU}) & 0xFFFU;
    host.Bytes = 4096U - within;
    // Memory-owner boundary: an uncached MemoryObject maps the device Frame
    // at address for the instance's lifetime.
    host.Mmio->Configure(reinterpret_cast<volatile UInt32*>(static_cast<Foundation::UIntPointer>(address + within)));
    return 0U;
}

// Builds host k's adapter and controller for the function at words of its
// own, maps its registers and takes it from firmware.
[[nodiscard]] UInt64 Prepare(const UInt64 (&block)[Abi::ComponentBlockWords], const Size k) noexcept
{
    Host& host = g_hosts[k];
    host.Region = Handle(block, Abi::ComponentUsbHostRegionWord(k));
    host.Function = Handle(block, Abi::ComponentUsbHostFunctionWord(k));
    host.Index = static_cast<UInt8>(k);
    host.Mmio = new (UsbHostPlacement{g_mmio[k]}) System::UsbHostMmio{};
    host.Io = new (UsbHostPlacement{g_ports[k]}) System::UsbHostPorts{};
    host.Mmio->Access().Configure(*g_driver.Client, host.Function, g_driver.Clock, g_driver.TicksPerMillisecond);
    host.Io->Access().Configure(*g_driver.Client, host.Function, g_driver.Clock, g_driver.TicksPerMillisecond);
    host.Io->Configure(*g_driver.Client, host.Region);
    UInt32 identity{0U};
    if (!host.Mmio->Access().ReadConfiguration(0x08U, identity) || ((identity >> 16U) != 0x0C03U))
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::InvalidConfiguration);
    }
    const UInt32 interface = (identity >> 8U) & 0xFFU;
    host.Kind =
        (interface == 0x20U)
            ? HostKind::Ehci
            : ((interface == 0x10U) ? HostKind::Ohci : ((interface == 0x00U) ? HostKind::Uhci : HostKind::None));
    const UInt8 kind = static_cast<UInt8>(
        (host.Kind == HostKind::Ehci) ? UsbController::Ehci
                                      : ((host.Kind == HostKind::Ohci) ? UsbController::Ohci : UsbController::Uhci));
    host.Progress = new (UsbHostPlacement{g_progress[k]}) XhciProgress{};
    host.Progress->Configure(UsbControllerByte(static_cast<UsbController>(kind), static_cast<UInt8>(k)),
                             (g_driver.Flags & Abi::XhciReportProgressFlag) != 0U);
    if (host.Kind == HostKind::None)
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::InvalidConfiguration);
    }
    if (host.Kind == HostKind::Uhci)
    {
        host.Uhci = new (UsbHostPlacement{g_uhci[k]}) Usb::UhciController{*host.Io, *host.Io};
        host.Io->Access().SetPacing(true);
        const Usb::Usb1Handoff handoff = host.Uhci->Handoff(HandoffPolls);
        host.Io->Access().SetPacing(false);
        host.Ports = 2U;
        return (handoff == Usb::Usb1Handoff::Success)
                   ? 0U
                   : DriverFailure(Abi::ComponentDriverQuiesceStep, (handoff == Usb::Usb1Handoff::BiosTimeout)
                                                                        ? Abi::SystemCallError::TimedOut
                                                                        : Abi::SystemCallError::RightsDenied);
    }
    const UInt64 mapped = MapBlock(block, host, k);
    if (mapped != 0U)
    {
        return mapped;
    }
    host.Mmio->Access().SetPacing(true);
    bool taken{false};
    if (host.Kind == HostKind::Ehci)
    {
        host.Ehci = new (UsbHostPlacement{g_ehci[k]}) Usb::EhciController{*host.Mmio};
        taken = host.Ehci->Handoff(host.Bytes, HandoffPolls) == Usb::HandoffStatus::Success;
    }
    else
    {
        host.Ohci = new (UsbHostPlacement{g_ohci[k]}) Usb::OhciController{*host.Mmio};
        taken = host.Ohci->Handoff(host.Bytes, HandoffPolls) == Usb::Usb1Handoff::Success;
    }
    host.Mmio->Access().SetPacing(false);
    return (taken && !host.Mmio->Access().Refused())
               ? 0U
               : DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::TimedOut);
}

// Stops what firmware left running before the function masters the bus
// behind the new domain, so no firmware schedule runs against its mappings
// (EHCI 1.0 section 2.3.1, UHCI 1.1 section 2.1.1: clear Run/Stop, wait for
// HCHalted; OpenHCI 1.0a section 7.1.2: USBRESET stops list processing).
[[nodiscard]] bool Stop(Host& host) noexcept
{
    UInt32 halted{0U};
    switch (host.Kind)
    {
    case HostKind::Ehci:
    {
        host.Operational = host.Mmio->Read32(0U) & 0xFFU;
        if ((host.Operational < 0x10U) || (host.Operational + 0x48U > host.Bytes))
        {
            return false;
        }
        host.Mmio->Write32(host.Operational, host.Mmio->Read32(host.Operational) & ~UInt32{0x31U});
        for (UInt32 poll{0U}; (poll < RegisterPolls) && (halted == 0U); ++poll)
        {
            halted = host.Mmio->Read32(host.Operational + 4U) & (1U << 12U);
        }
        if (halted == 0U)
        {
            return false;
        }
        // Reset before requesting BME, including trusted identity domains. EHCI 1.0 section 2.3.1
        // HCRESET.
        host.Mmio->Write32(host.Operational, 2U);
        for (UInt32 poll{0U}; poll < RegisterPolls; ++poll)
        {
            if ((host.Mmio->Read32(host.Operational) & 2U) == 0U)
            {
                return true;
            }
        }
        return false;
    }
    case HostKind::Ohci:
        host.Mmio->Write32(4U, host.Mmio->Read32(4U) & ~UInt32{0xFCU});
        // OpenHCI 1.0a section 7.1.3, HostControllerReset.
        host.Mmio->Write32(8U, 1U);
        for (UInt32 poll{0U}; poll < RegisterPolls; ++poll)
        {
            if ((host.Mmio->Read32(8U) & 1U) == 0U)
            {
                return true;
            }
        }
        return false;
    case HostKind::Uhci:
        host.Io->Write16(0U, static_cast<UInt16>(host.Io->Read16(0U) & ~1U));
        for (UInt32 poll{0U}; (poll < RegisterPolls) && (halted == 0U); ++poll)
        {
            halted = host.Io->Read16(2U) & 0x20U;
        }
        if (halted == 0U || host.Io->Refused())
        {
            return false;
        }
        // UHCI 1.1 section 2.1.1, HostControllerReset.
        host.Io->Write16(0U, 2U);
        for (UInt32 poll{0U}; poll < RegisterPolls; ++poll)
        {
            if ((host.Io->Read16(0U) & 2U) == 0U)
            {
                return !host.Io->Refused();
            }
        }
        return false;
    case HostKind::None:
        return false;
    }
    return false;
}

// One IommuDomain for the group, every function attached; then the group's
// DMA pages, two for each controller, mapped for the devices at the address
// the kernel answers.
[[nodiscard]] UInt64 Confine(const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const auto& client = *g_driver.Client;
    const auto domain = Handle(block, Abi::ComponentAccountWord);
    alignas(8) UInt8 request[Abi::ObjectCreateIommuDomainRequestSizeBytes]{};
    // A shared domain uses the narrowest member. EHCI 1.0 section 2.2.4, HCCPARAMS bit 0; OHCI/UHCI
    // use 32 bits.
    UInt8 addressBits{64U};
    for (Size k{0U}; k < g_driver.HostCount; ++k)
    {
        if ((g_hosts[k].Kind != HostKind::Ehci) || ((g_hosts[k].Mmio->Read32(8U) & 1U) == 0U))
        {
            addressBits = 32U;
        }
    }
    request[Abi::ObjectCreateIommuAddressBitsOffset] = addressBits;
    const Abi::SystemCallResponse created =
        client.CreateObject(domain, Abi::ObjectTypeIommuDomain, System::AddressOf(request), sizeof(request));
    if (created.Error != Abi::SystemCallError::None)
    {
        // No IOMMU and no trust entry: the controllers never master the bus.
        return DriverFailure(Abi::ComponentDriverIommuStep, created.Error);
    }
    g_driver.Iommu = System::CapabilityHandle{created.Value0, created.Value1};
    for (Size k{0U}; k < g_driver.HostCount; ++k)
    {
        const Abi::SystemCallError attached =
            client
                .ControlIommu(g_driver.Iommu, Abi::IommuAttach, g_hosts[k].Function.Selector,
                              g_hosts[k].Function.Authenticator)
                .Error;
        if (attached != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverAttachStep, attached);
        }
    }
    const UInt64 address = block[Abi::ComponentWindowStartWord] + Abi::UsbHostRegisterPages * 4096U;
    System::CapabilityHandle frame{};
    const Abi::SystemCallError pages = MapDmaPage(client, domain, Handle(block, Abi::ComponentAccountSpaceWord),
                                                  address, frame, Abi::UsbHostDmaPages, 1U);
    if (pages != Abi::SystemCallError::None)
    {
        return DriverFailure(Abi::ComponentDriverPagesStep, pages);
    }
    const DeviceMapping dma = MapForDevice(client, g_driver.Iommu, frame, Abi::UsbHostDmaPages, DmaAddress,
                                           Abi::IommuMapDeviceRead | Abi::IommuMapDeviceWrite);
    if ((dma.Error != Abi::SystemCallError::None) || ((dma.Address % 4096U) != 0U))
    {
        return DriverFailure(Abi::ComponentDriverDeviceMapStep, (dma.Error != Abi::SystemCallError::None)
                                                                    ? dma.Error
                                                                    : Abi::SystemCallError::InvalidArguments);
    }
    g_driver.Dma = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(address));
    g_driver.DmaDevice = dma.Address;
    return 0U;
}

[[nodiscard]] Usb::DmaMemory Memory(const Size k, const Size offset, const Size bytes) noexcept
{
    const Size at = k * 8192U + offset;
    return Usb::DmaMemory{g_driver.DmaDevice + at, g_driver.Dma + at, bytes};
}

// Starts host k on its two DMA pages: the frame list or HCCA on the first,
// the queue heads, descriptors and bounce buffer at the alignments each
// library asks for on the second.
[[nodiscard]] bool Start(Host& host, const Size k) noexcept
{
    switch (host.Kind)
    {
    case HostKind::Ehci:
    {
        const Usb::EhciMemory memory{.Frames = Memory(k, 0U, 4096U),
                                     .AsyncHead = Memory(k, 4096U, 96U),
                                     .Queue = Memory(k, 4096U + 128U, 96U),
                                     .Descriptors = Memory(k, 4096U + 256U, 192U),
                                     .Buffer = Memory(k, 4096U + 512U, 1088U)};
        if (!host.Ehci->Initialize(memory, host.Bytes, RegisterPolls))
        {
            return false;
        }
        host.Ports = static_cast<UInt8>(host.Mmio->Read32(4U) & 15U);
        return true;
    }
    case HostKind::Ohci:
    {
        const Usb::OhciMemory memory{.Hcca = Memory(k, 0U, 256U),
                                     .Endpoints = Memory(k, 4096U, 32U),
                                     .Descriptors = Memory(k, 4096U + 64U, 64U),
                                     .Buffer = Memory(k, 4096U + 1024U, 1088U)};
        if (!host.Ohci->Initialize(memory, host.Bytes, RegisterPolls))
        {
            return false;
        }
        const UInt32 ports = host.Mmio->Read32(0x48U) & 0xFFU;
        host.Ports = static_cast<UInt8>(ports > 15U ? 15U : ports);
        return true;
    }
    case HostKind::Uhci:
    {
        const Usb::UhciMemory memory{.Frames = Memory(k, 0U, 4096U),
                                     .Queues = Memory(k, 4096U, 32U),
                                     .Descriptors = Memory(k, 4096U + 64U, 2080U),
                                     .Buffer = Memory(k, 4096U + 2176U, 1088U)};
        return host.Uhci->Initialize(memory, 32U, RegisterPolls) && !host.Io->Refused();
    }
    case HostKind::None:
        return false;
    }
    return false;
}

// The function's interrupt where the kernel offers one (its INTx on IA-64);
// a controller without one is polled on the timed wait.
void Connect(const UInt64 (&block)[Abi::ComponentBlockWords], Host& host) noexcept
{
    const auto& client = *g_driver.Client;
    alignas(8) UInt8 request[Abi::ObjectCreateInterruptRequestSizeBytes]{};
    System::StoreHandle(request + Abi::ObjectCreateInterruptFunctionOffset, host.Function);
    const auto created = client.CreateObject(Handle(block, Abi::ComponentAccountWord), Abi::ObjectTypeInterrupt,
                                             System::AddressOf(request), sizeof(request));
    if (created.Error != Abi::SystemCallError::None)
    {
        return;
    }
    const System::CapabilityHandle interrupt{created.Value0, created.Value1};
    if (client.BindWaitSet(g_driver.WaitSet, interrupt, InterruptTag | host.Index).Error == Abi::SystemCallError::None)
    {
        host.Interrupt = interrupt;
        return;
    }
    static_cast<void>(client.DestroyObject(interrupt, Abi::ObjectTypeInterrupt));
}

[[nodiscard]] UInt64 Bring(const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const auto& client = *g_driver.Client;
    for (Size k{0U}; k < Hosts; ++k)
    {
        if (block[Abi::ComponentUsbHostFunctionWord(k)] == 0U)
        {
            break;
        }
        g_driver.HostCount = k + 1U;
        const UInt64 failure = Prepare(block, k);
        if (failure != 0U)
        {
            return failure;
        }
        if (!Stop(g_hosts[k]))
        {
            return DriverFailure(Abi::ComponentDriverQuiesceStep, Abi::SystemCallError::TimedOut);
        }
    }
    if (g_driver.HostCount == 0U)
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::InvalidConfiguration);
    }
    const UInt64 confined = Confine(block);
    if (confined != 0U)
    {
        return confined;
    }
    for (Size k{0U}; k < g_driver.HostCount; ++k)
    {
        Connect(block, g_hosts[k]);
        const auto master = client.ControlDevice(g_hosts[k].Function, Abi::DeviceEnableBusMaster).Error;
        if (master != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverMasterStep, master);
        }
    }
    // EHCI first: CONFIGFLAG routes every port to it, and it releases the
    // full- and low-speed devices to companions that then run.
    for (Size pass{0U}; pass < 2U; ++pass)
    {
        for (Size k{0U}; k < g_driver.HostCount; ++k)
        {
            Host& host = g_hosts[k];
            if ((host.Kind == HostKind::Ehci) != (pass == 0U))
            {
                continue;
            }
            if (!Start(host, k))
            {
                return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::InvalidConfiguration);
            }
            Trace(host, Input::UsbDiagnosticKind::Started, 0U, (host.Interrupt.Selector != 0U) ? 1U : 0U, host.Ports);
        }
    }
    for (Size slot{0U}; slot < Devices; ++slot)
    {
        g_slots[slot].Device = new (UsbHostPlacement{g_devices[slot]}) Input::UsbDevice{};
        g_slots[slot].View = new (UsbHostPlacement{g_views[slot]}) SlotView{g_slots[slot]};
    }
    g_driver.Started = Now();
    return 0U;
}

// Initial enumeration settles, bounded, before readiness.
[[nodiscard]] bool Prime() noexcept
{
    for (Size round{0U}; round < PrimeRounds; ++round)
    {
        const UInt64 now = Now();
        if (!Progress(now))
        {
            return false;
        }
        if (!AnyEnumerating() && ((Abi::ElapsedClockTicks(now, g_driver.Started)) >= SettleMilliseconds))
        {
            return true;
        }
        if (!Rearm() || (Pause() != Abi::SystemCallError::None))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool Ready() noexcept
{
    // Diagnostics queued on the same report Endpoint may fill this sender's
    // share of it for a moment; the root service drains it while it waits.
    Abi::SystemCallError ready{Abi::SystemCallError::WouldBlock};
    for (Size attempt{0U}; (attempt < 500U) && (ready == Abi::SystemCallError::WouldBlock); ++attempt)
    {
        WriteEchoFrame(g_frame, nullptr, 0U);
        Detail::InputStore32(g_frame + 32U, XhciReadyMethod);
        ready = g_driver.Client->Send(g_driver.Root, System::AddressOf(g_frame), EchoHeaderBytes).Error;
        ready = (ready == Abi::SystemCallError::ResourceExhausted) ? Abi::SystemCallError::WouldBlock : ready;
        if ((ready == Abi::SystemCallError::WouldBlock) && (Pause() != Abi::SystemCallError::None))
        {
            return false;
        }
        for (Size k{0U}; k < g_driver.HostCount; ++k)
        {
            g_hosts[k].Progress->Flush(*g_driver.Client, g_driver.Root, g_frame);
        }
    }
    return ready == Abi::SystemCallError::None;
}
} // Namespace

Foundation::UInt64 ServeUsbHost(const System::SystemCallClient& client,
                                const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                const ComponentClock clock) noexcept
{
    g_driver.Client = &client;
    g_driver.Clock = clock;
    g_driver.Source = Handle(block, Abi::ComponentInputServiceWord);
    g_driver.Reply = Handle(block, Abi::ComponentReplyWord);
    g_driver.Receive = Handle(block, Abi::ComponentReceiveWord);
    g_driver.WaitSet = Handle(block, Abi::ComponentWaitSetWord);
    g_driver.Root = Handle(block, Abi::ComponentReportWord);
    g_driver.Epoch = block[Abi::ComponentInputEpochWord];
    g_driver.Flags = block[Abi::ComponentMountFlagsWord];
    const UInt64 scale = block[Abi::ComponentClockTicksPerMicrosecondWord];
    if ((scale == 0U) || (scale > ~UInt64{0U} / 1000U) ||
        (block[Abi::ComponentWindowPagesWord] < Abi::UsbHostWindowPages))
    {
        return DriverFailure(Abi::ComponentDriverIdentityStep, Abi::SystemCallError::InvalidConfiguration);
    }
    g_driver.TicksPerMillisecond = scale * 1000U;
    const UInt64 failure = Bring(block);
    if (failure != 0U)
    {
        return failure;
    }
    if (!Prime())
    {
        return DriverFailure(Abi::ComponentDriverDeviceStep, Abi::SystemCallError::TimedOut);
    }
    if (!Ready())
    {
        return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
    }
    for (;;)
    {
        if (!Progress(Now()) || !Replies() || !Publish() || !Query())
        {
            return DriverFailure(Abi::ComponentDriverServeStep, Abi::SystemCallError::None);
        }
        if (!Rearm())
        {
            return DriverFailure(Abi::ComponentDriverInterruptStep, Abi::SystemCallError::None);
        }
        const Abi::SystemCallError waited = Pause();
        if (waited != Abi::SystemCallError::None)
        {
            return DriverFailure(Abi::ComponentDriverServeStep, waited);
        }
    }
}

Foundation::UInt64 RunUsbHostComponent(const System::SystemCallTransport transport,
                                       const Foundation::UInt64 (&block)[Abi::ComponentBlockWords],
                                       const ComponentClock clock) noexcept
{
    if ((block[Abi::ComponentStartMagicWord] != Abi::ComponentStartMagic) ||
        (block[Abi::ComponentBehaviorWord] != Abi::ComponentBehaviorXhci) ||
        ((block[Abi::ComponentMountFlagsWord] & Abi::UsbHostGroupFlag) == 0U))
    {
        return Abi::ComponentBadStartExitCode;
    }
    const System::SystemCallClient client{transport};
    return ServeUsbHost(client, block, clock);
}
} // Namespace Asteria::User::Component
