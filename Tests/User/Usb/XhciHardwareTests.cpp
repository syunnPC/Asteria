// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Xhci.hpp"

#include <cstring>
#include <memory>
#include <vector>

#include "Support/TestContext.hpp"
#include "User/Usb/FakeXhci.hpp"

// Synthetic enumeration cases exercise xHCI 0.96, USB 2.0 and HID 1.11:
// 32-byte contexts, no scratchpads, and a composite full-speed device on a
// USB2 port that firmware left enabled.
namespace
{
namespace X = Asteria::User::Usb;
namespace In = Asteria::User::Input;
namespace Test = Asteria::Test::Usb;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::TestSupport::TestContext;

// Project-owned USB 2.0 / HID 1.11 descriptors of a composite full-speed
// receiver: bMaxPacketSize0 8; interface 0 boot mouse (EP 0x81), interface 1
// boot keyboard (EP 0x82), interface 2 vendor HID (03/00/00, EP 0x83), all
// interrupt IN with bInterval 1.
constexpr UInt8 ReceiverDevice[]{18U, 1U, 0U, 2U, 0U, 0U, 0U, 8U, 0x34U, 0x12U, 0x78U, 0x56U, 0U, 1U, 1U, 2U, 0U, 1U};
constexpr UInt8 ReceiverConfiguration[]{
    9U, 2U, 84U,   0U,    3U,  1U, 0U,    0xA0U, 49U,   9U, 4U, 0U,    0U,    1U, 3U, 1U, 2U,    0U, 9U,  0x21U, 0x11U,
    1U, 0U, 1U,    0x22U, 67U, 0U, 7U,    5U,    0x81U, 3U, 8U, 0U,    1U,    9U, 4U, 1U, 0U,    1U, 3U,  1U,    1U,
    0U, 9U, 0x21U, 0x11U, 1U,  0U, 1U,    0x22U, 59U,   0U, 7U, 5U,    0x82U, 3U, 8U, 0U, 1U,    9U, 4U,  2U,    0U,
    1U, 3U, 0U,    0U,    0U,  9U, 0x21U, 0x11U, 1U,    0U, 1U, 0x22U, 98U,   0U, 7U, 5U, 0x83U, 3U, 32U, 0U,    1U,
};
static_assert(sizeof(ReceiverConfiguration) == 84U);

class Recorder final : public In::IUsbDiagnosticSink
{
public:
    void Record(const In::UsbDiagnostic& record) noexcept override
    {
        Records.push_back(record);
    }
    [[nodiscard]] Size Count(const In::UsbDiagnosticKind kind) const noexcept
    {
        Size count{0U};
        for (const auto& record : Records)
        {
            count += record.Kind == kind ? 1U : 0U;
        }
        return count;
    }
    [[nodiscard]] const In::UsbDiagnostic* First(const In::UsbDiagnosticKind kind) const noexcept
    {
        for (const auto& record : Records)
        {
            if (record.Kind == kind)
            {
                return &record;
            }
        }
        return nullptr;
    }
    std::vector<In::UsbDiagnostic> Records{};
};

struct Receiver
{
    Receiver() noexcept
    {
        auto& model = Base.Model;
        model.Registers[0U] = (0x96U << 16U) | Test::FakeXhci::Operational;
        model.Registers[2U] = 0U; // no scratchpads
        model.DeviceBytes = ReceiverDevice;
        model.ConfigurationBytes = ReceiverConfiguration;
        model.ConfigurationLength = sizeof(ReceiverConfiguration);
        model.MouseEndpoint = 3U; // DCI of EP 0x81
        Base.Memory.ScratchpadCount = 0U;
        Pipes[0U] = Base.EndpointMappings[0U][0U];
        Pipes[1U] = {.Ring = Base.Map(0x380000U, Rings[0U], 256U), .Buffer = Base.Map(0x390000U, Buffers[0U], 1024U)};
        Pipes[2U] = {.Ring = Base.Map(0x3A0000U, Rings[1U], 256U), .Buffer = Base.Map(0x3B0000U, Buffers[1U], 1024U)};
        Base.Ports[0U].Pipes = Pipes;
        Base.Ports[0U].PipeCount = 3U;
        Base.Controller.SetDiagnostics(&Sink);
        Core.SetDiagnostics(&Sink);
    }
    // One component tick: steps, poll, completion delivery.
    void Tick(TestContext& context) noexcept
    {
        X::XhciPort port{Base.Controller, 1U};
        for (Size step{0U}; step <= Core.InterfaceCount(); ++step)
        {
            Core.Step(port);
        }
        Base.Controller.Poll(++Now);
        X::Completion completion{};
        while (Base.Controller.TakeCompletion(1U, completion))
        {
            In::EventBatch events{};
            ASTERIA_EXPECT(context, "completion routed to the core",
                           Core.Complete(completion.Token, completion.Status, completion.ActualLength, events));
            for (Size index{0U}; index < events.Count; ++index)
            {
                Events.push_back(events.Events[index]);
            }
        }
    }
    void Run(TestContext& context, const Size ticks) noexcept
    {
        for (Size tick{0U}; (tick < ticks) && (Core.GetState() != In::UsbState::Polling); ++tick)
        {
            Tick(context);
        }
    }
    Test::Fixture Base{false, 0U};
    X::EndpointMemory Pipes[3U]{};
    UInt8 Rings[2U][256U]{};
    UInt8 Buffers[2U][1024U]{};
    In::UsbDevice Core{9U};
    Recorder Sink{};
    std::vector<In::InputEvent> Events{};
    UInt64 Now{0U};
};

[[nodiscard]] UInt32& PortStatus(Test::FakeXhci& model, const UInt8 number) noexcept
{
    return model.Registers[(Test::FakeXhci::Operational + 0x400U + UInt32{number - 1U} * 16U) / 4U];
}

// The firmware left port 1 enabled with its device addressed; PED survives
// HCRST. The reset must really happen before Enable Slot: PED alone is no
// evidence when the controller has not yet shown PR or PRC.
void FirmwareEnabledComposite(TestContext& context) noexcept
{
    auto owner = std::make_unique<Receiver>();
    Receiver& fixture = *owner;
    auto& model = fixture.Base.Model;
    PortStatus(model, 1U) = 0x603U;
    model.SetPort(2U, false, 0U);
    model.HoldPortReset = true;
    model.StallRequest = 10U; // SET_IDLE, optional for boot devices
    // Boot mouse reports may be longer than their three defined bytes.
    const UInt8 mouse[8U]{1U, 5U, 0xFDU, 0U, 0x40U, 0x41U, 0x42U, 0x43U};
    std::memcpy(model.MouseReport, mouse, sizeof(mouse));
    model.MouseReportLength = 8U;
    ASTERIA_EXPECT(context, "0.96 controller with no scratchpads starts",
                   fixture.Base.Controller.Initialize(fixture.Base.Memory, 0x2100U), fixture.Core.Attach(1U));
    for (Size tick{0U}; tick < 20U; ++tick)
    {
        fixture.Tick(context);
    }
    ASTERIA_EXPECT(context, "reset requested once and awaited although PED was already set", model.PortResets == 1U,
                   model.CommandCount == 0U, fixture.Core.GetState() == In::UsbState::Reset);
    model.HoldPortReset = false;
    model.CompletePortReset(1U);
    fixture.Run(context, 300U);
    ASTERIA_EXPECT(context, "composite boot mouse and keyboard enumerate past stalled SET_IDLEs",
                   fixture.Core.GetState() == In::UsbState::Polling, fixture.Core.InterfaceCount() == 2U,
                   model.ProtocolMask == 3U, model.IdleMask == 0U, model.StalledRequests == 2U,
                   model.Configurations == 2U, model.Good);
    ASTERIA_EXPECT(context, "each stalled control TD reset EP0 and moved its dequeue", model.EndpointResets == 2U,
                   model.DequeueSets == 2U, !model.Slots[0U].Halted[1U]);
    ASTERIA_EXPECT(context, "full-speed bInterval 1 is 2^3 microframes, EP0 keeps 8 bytes", model.LastInterval == 3U,
                   model.LastPacket == 8U, model.LastPayload == 8U,
                   (X::Load32(fixture.Base.Outputs[0U] + 32U + 4U) >> 16U) == 8U,
                   // Evaluate Context presents the current EP0 context (type 4, CErr 3)
                   // with only Max Packet Size changed.
                   model.LastEvaluateEndpoint == ((8U << 16U) | (4U << 3U) | (3U << 1U)));
    for (Size tick{0U}; tick < 4U; ++tick)
    {
        fixture.Tick(context);
    }
    bool motion{false};
    bool key{false};
    for (const auto& event : fixture.Events)
    {
        motion = motion || ((event.Kind == In::EventKind::Motion) && (event.Dx == 5) && (event.Dy == -3));
        key = key || ((event.Kind == In::EventKind::KeyDown) && (event.Usage == 4U));
    }
    ASTERIA_EXPECT(context, "both interfaces report, the long mouse report through its boot fields", motion, key);
    const auto& sink = fixture.Sink;
    const auto* const connect = sink.First(In::UsbDiagnosticKind::Connect);
    const auto* const reset = sink.First(In::UsbDiagnosticKind::ResetDone);
    const auto* const device = sink.First(In::UsbDiagnosticKind::Device);
    const auto* const configuration = sink.First(In::UsbDiagnosticKind::Configuration);
    const auto* const endpoint = sink.First(In::UsbDiagnosticKind::EndpointConfigured);
    const In::UsbDiagnostic* report{nullptr};
    for (const auto& record : sink.Records)
    {
        if ((record.Kind == In::UsbDiagnosticKind::Report) && (record.Detail == 0U) && (report == nullptr))
        {
            report = &record;
        }
    }
    ASTERIA_EXPECT(
        context, "connect and reset records carry PORTSC and the negotiated speed", connect != nullptr,
        (connect != nullptr) && (connect->Port == 1U) && (connect->Value0 == 0x603U) && (connect->Detail == 2U),
        (reset != nullptr) && (reset->Code == static_cast<UInt8>(In::UsbSpeed::Full) + 1U) && (reset->Detail == 1U));
    ASTERIA_EXPECT(context, "slot, address and descriptor records",
                   sink.Count(In::UsbDiagnosticKind::SlotEnabled) == 1U,
                   sink.Count(In::UsbDiagnosticKind::Addressed) == 2U,
                   (sink.First(In::UsbDiagnosticKind::EndpointZero) != nullptr) &&
                       (sink.First(In::UsbDiagnosticKind::EndpointZero)->Value0 == 8U),
                   (device != nullptr) && (device->Value0 == 0x5678'1234U) && (device->Detail == 8U) &&
                       ((device->Value1 & 0xFFFFU) == 0x200U));
    ASTERIA_EXPECT(context, "configuration and every interface, generic HID probed",
                   (configuration != nullptr) && (configuration->Value0 == 84U) &&
                       (configuration->Value1 == (3U | (3U << 8U))),
                   sink.Count(In::UsbDiagnosticKind::Interface) == 3U,
                   (sink.Records.size() > 0U) && (sink.Count(In::UsbDiagnosticKind::Polling) == 1U));
    Size skipped{0U};
    Size stalledIdle{0U};
    for (const auto& record : sink.Records)
    {
        skipped += (record.Kind == In::UsbDiagnosticKind::Interface) && (record.Code == 1U) && (record.Value0 == 3U) &&
                           ((record.Value1 & 0xFFU) == 0x83U) && ((record.Value1 >> 24U) == 1U)
                       ? 1U
                       : 0U;
        stalledIdle += (record.Kind == In::UsbDiagnosticKind::Request) && (record.Detail == 10U) &&
                               (record.Code == static_cast<UInt8>(In::TransferStatus::Stall))
                           ? 1U
                           : 0U;
    }
    ASTERIA_EXPECT(context, "generic HID candidate rejected; both boot SET_IDLE stalls recorded", skipped == 1U,
                   stalledIdle == 2U, sink.Count(In::UsbDiagnosticKind::Transfer) == 2U,
                   sink.Count(In::UsbDiagnosticKind::Failed) == 0U);
    ASTERIA_EXPECT(context, "endpoint record shows the context's interval and payload",
                   (endpoint != nullptr) && (endpoint->Detail == 0x81U) && (endpoint->Code == 3U) &&
                       (endpoint->Value0 == 3U) && (endpoint->Value1 == (8U | (8U << 16U))),
                   (report != nullptr) && (report->Value0 == 8U) && (report->Data == 0x4342'4140'00FD'0501ULL) &&
                       (report->Code == static_cast<UInt8>(In::ReportResult::Accepted)),
                   sink.Count(In::UsbDiagnosticKind::Report) == 2U);
    ASTERIA_EXPECT(context, "teardown", fixture.Base.Controller.Halt());
}

// A hot-plugged USB2 device whose port reports an undefined speed until a
// reset: connection is CCS alone, the reset waits the attach debounce, and
// the speed comes from the enabled port.
void UndefinedSpeedHotPlug(TestContext& context) noexcept
{
    auto owner = std::make_unique<Receiver>();
    Receiver& fixture = *owner;
    auto& model = fixture.Base.Model;
    model.SetPort(1U, false, 0U);
    model.SetPort(2U, false, 0U);
    ASTERIA_EXPECT(context, "start with empty ports", fixture.Base.Controller.Initialize(fixture.Base.Memory, 0x2100U));
    fixture.Base.Controller.Poll(++fixture.Now);
    model.SetPortUnreset(1U, 1U);
    model.Emit({.Parameter = UInt64{1U} << 24U, .Status = 1U << 24U, .Control = 34U << 10U});
    fixture.Base.Controller.Poll(++fixture.Now);
    const UInt64 attached = fixture.Now;
    ASTERIA_EXPECT(context, "CCS with Port Speed 0 is a connection",
                   fixture.Base.Controller.GetPortStatus(1U).Connected, fixture.Core.Attach(1U));
    while (fixture.Now < attached + 99U)
    {
        fixture.Tick(context);
    }
    ASTERIA_EXPECT(context, "no reset during the 100 ms attach debounce", model.PortResets == 0U,
                   fixture.Core.GetState() == In::UsbState::Reset);
    fixture.Run(context, 300U);
    ASTERIA_EXPECT(context, "reset after the debounce, speed from the enabled port", model.PortResets == 1U,
                   fixture.Core.GetState() == In::UsbState::Polling,
                   ((X::Load32(fixture.Base.Outputs[0U]) >> 20U) & 15U) == 1U, model.Good);
    ASTERIA_EXPECT(context, "teardown", fixture.Base.Controller.Halt());
}

// A port whose speed stays undecodable after its reset has no Slot Context
// speed: the reset fails and no slot is enabled.
void UndecodableSpeed(TestContext& context) noexcept
{
    auto owner = std::make_unique<Receiver>();
    Receiver& fixture = *owner;
    auto& model = fixture.Base.Model;
    model.SetPortUnreset(1U, 0U);
    model.SetPort(2U, false, 0U);
    ASTERIA_EXPECT(context, "start", fixture.Base.Controller.Initialize(fixture.Base.Memory, 0x2100U),
                   fixture.Core.Attach(1U));
    for (Size tick{0U}; tick < 40U; ++tick)
    {
        fixture.Tick(context);
    }
    const auto* const reset = fixture.Sink.First(In::UsbDiagnosticKind::ResetDone);
    ASTERIA_EXPECT(context, "reset reported without a speed; never addressed", model.PortResets == 1U,
                   model.CommandCount == 0U, fixture.Core.GetState() == In::UsbState::Failed,
                   (reset != nullptr) && (reset->Code == 0U), fixture.Sink.Count(In::UsbDiagnosticKind::Failed) == 1U);
    ASTERIA_EXPECT(context, "teardown", fixture.Base.Controller.Halt());
}

// SET_PROTOCOL STALL on the keyboard interface drops only that interface;
// interrupt-IN babble halts the endpoint and is recovered, then retried.
void ProtocolStallAndBabble(TestContext& context) noexcept
{
    auto owner = std::make_unique<Receiver>();
    Receiver& fixture = *owner;
    auto& model = fixture.Base.Model;
    PortStatus(model, 1U) = 0x20601U;
    model.SetPort(2U, false, 0U);
    model.StallRequest = 11U;
    model.StallIndex = 1U;
    ASTERIA_EXPECT(context, "start", fixture.Base.Controller.Initialize(fixture.Base.Memory, 0x2100U),
                   fixture.Core.Attach(1U));
    fixture.Run(context, 300U);
    const In::BootInterface* const kept = fixture.Core.GetInterface(0U);
    ASTERIA_EXPECT(context, "mouse kept after the keyboard's SET_PROTOCOL stall",
                   fixture.Core.GetState() == In::UsbState::Polling, fixture.Core.InterfaceCount() == 1U,
                   (kept != nullptr) && (kept->Protocol == In::BootProtocol::Mouse), model.Configurations == 1U,
                   model.ProtocolMask == 1U, model.Good);
    model.NextCode = 3U;
    fixture.Events.clear();
    for (Size tick{0U}; tick < 40U; ++tick)
    {
        fixture.Tick(context);
    }
    ASTERIA_EXPECT(context, "babble recovered by Reset Endpoint and a retried report",
                   fixture.Core.GetState() == In::UsbState::Polling, model.EndpointResets == 2U,
                   !fixture.Events.empty(), model.Good);
    ASTERIA_EXPECT(context, "teardown", fixture.Base.Controller.Halt());
}
} // namespace

int main()
{
    TestContext context{"xHCI 0.96 enumeration"};
    FirmwareEnabledComposite(context);
    UndefinedSpeedHotPlug(context);
    UndecodableSpeed(context);
    ProtocolStallAndBabble(context);
    return context.Finish();
}
