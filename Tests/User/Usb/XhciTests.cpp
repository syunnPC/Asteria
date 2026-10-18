// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Xhci.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Usb/FakeXhci.hpp"

namespace
{
namespace X = Asteria::User::Usb;
namespace In = Asteria::User::Input;
namespace Test = Asteria::Test::Usb;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::TestSupport::TestContext;

void Start(TestContext& context, Test::Fixture& fixture) noexcept
{
    ASTERIA_EXPECT(context, "initialize injected BAR and caller DMA",
                   fixture.Controller.Initialize(fixture.Memory, 0x2100U));
    ASTERIA_EXPECT(
        context, "run, interrupt and ring registers", fixture.Model.Good, fixture.Controller.PortCount() == 2U,
        fixture.Model.Read64(Test::FakeXhci::Operational + 0x30U) == fixture.Memory.Dcbaa.Address,
        fixture.Model.Read64(Test::FakeXhci::Operational + 0x18U) == (fixture.Memory.CommandRing.Address | 1U),
        fixture.Model.Read32(Test::FakeXhci::Runtime + 0x20U) == 2U,
        fixture.Model.Read32(Test::FakeXhci::Runtime + 0x24U) == 4000U);
    ASTERIA_EXPECT(context, "scratchpads requested by capability",
                   X::Load64(fixture.Dcbaa) == fixture.Memory.ScratchpadArray.Address,
                   X::Load64(fixture.ScratchArray) == fixture.ScratchPages[0U].Address);
    const In::UsbRequest child{.Operation = In::UsbOperation::ResetPort,
                               .Token = 1U,
                               .Speed = In::UsbSpeed::Full,
                               .Address = 0U,
                               .PacketSize0 = 8U,
                               .Setup = {},
                               .Endpoint = {},
                               .Data = nullptr,
                               .Length = 0U,
                               .Route = {.HubAddress = 2U, .HubPort = 1U, .Depth = 1U, .Speed = In::UsbSpeed::Full}};
    ASTERIA_EXPECT(context, "unsupported child cannot reset parent xHCI slot", !fixture.Controller.Submit(1U, child),
                   fixture.Controller.GetState() == X::ControllerState::Running);
}
void Tick(TestContext& context, Test::Fixture& fixture, In::UsbDevice& device, const UInt8 number, const UInt64 now,
          In::EventBatch& output) noexcept
{
    fixture.Controller.Poll(now);
    X::Completion completion{};
    output.Count = 0U;
    if (fixture.Controller.TakeCompletion(number, completion))
    {
        ASTERIA_EXPECT(context, "core accepts matched token/length",
                       device.Complete(completion.Token, completion.Status, completion.ActualLength, output));
    }
}
void Enumerate(TestContext& context, Test::Fixture& fixture, In::UsbDevice& device, const UInt8 number,
               UInt64& now) noexcept
{
    X::XhciPort port{fixture.Controller, number};
    ASTERIA_EXPECT(context, "attach a logical address", device.Attach(static_cast<UInt8>(number + 20U)));
    In::EventBatch events{};
    // A hot-plugged device waits the 100 ms attach debounce before its reset.
    for (Size step{0U}; (step < 250U) && (device.GetState() != In::UsbState::Polling); ++step)
    {
        device.Step(port);
        Tick(context, fixture, device, number, ++now, events);
    }
    ASTERIA_EXPECT(context, "real core enumerates through xHCI", device.GetState() == In::UsbState::Polling,
                   fixture.Model.Good);
}
void Report(TestContext& context, Test::Fixture& fixture, In::UsbDevice& device, const UInt8 number, UInt64& now,
            In::EventBatch& output) noexcept
{
    X::XhciPort port{fixture.Controller, number};
    device.Step(port);
    Tick(context, fixture, device, number, ++now, output);
}
void EndToEnd(TestContext& context, const bool large, const UInt64 base) noexcept
{
    Test::Fixture fixture{large, 1U, base};
    Start(context, fixture);
    X::PortChange change{};
    ASTERIA_EXPECT(context, "initial connect events for both root ports", fixture.Controller.TakePortChange(change),
                   change.Port == 1U, change.Status.Connected, fixture.Controller.TakePortChange(change),
                   change.Port == 2U, change.Status.Connected, !fixture.Controller.TakePortChange(change));
    In::UsbDevice keyboard{17U};
    In::UsbDevice mouse{18U};
    UInt64 now{0U};
    Enumerate(context, fixture, keyboard, 1U, now);
    Enumerate(context, fixture, mouse, 2U, now);
    ASTERIA_EXPECT(context, "all native commands and FS interval", fixture.Model.DefaultAddresses == 2U,
                   fixture.Model.Addresses == 2U, fixture.Model.Evaluations == 2U, fixture.Model.Configurations == 2U,
                   fixture.Model.LastInterval == 6U, fixture.Model.LastPacket == 4U, fixture.Model.LastBurst == 0U);
    In::EventBatch output{};
    Report(context, fixture, keyboard, 1U, now, output);
    ASTERIA_EXPECT(context, "synthetic keyboard becomes key events", output.Count == 2U,
                   output.Events[0U].Kind == In::EventKind::KeyDown, output.Events[0U].Usage == 0xE1U,
                   output.Events[1U].Usage == 4U, output.Events[1U].Source == 17U);
    Report(context, fixture, mouse, 2U, now, output);
    ASTERIA_EXPECT(context, "synthetic mouse becomes motion/button events", output.Count == 2U,
                   output.Events[0U].Kind == In::EventKind::Motion, output.Events[0U].Dx == 5,
                   output.Events[0U].Dy == -3, output.Events[1U].Kind == In::EventKind::Buttons);
    fixture.Model.StallNext = true;
    Report(context, fixture, keyboard, 1U, now, output);
    ASTERIA_EXPECT(context, "stall is reported without automatic class recovery", !keyboard.IsOutstanding(),
                   output.Count == 0U, keyboard.GetState() == In::UsbState::Polling, fixture.Model.ClearHalts == 0U);
    ASTERIA_EXPECT(context, "explicit pipe clear halt accepted", fixture.Controller.ClearHalt(1U, 0x81U, 10000U));
    X::Completion cleared{};
    for (Size i{0U}; i < 32U; ++i)
    {
        fixture.Controller.Poll(++now);
        if (fixture.Controller.TakeCompletion(1U, 0U, cleared))
        {
            break;
        }
    }
    ASTERIA_EXPECT(context, "reset endpoint, set dequeue and clear device halt", cleared.Token == 10000U,
                   cleared.Status == In::TransferStatus::Success, fixture.Model.EndpointResets == 1U,
                   fixture.Model.DequeueSets == 1U, fixture.Model.ClearHalts == 1U);
    Report(context, fixture, keyboard, 1U, now, output);
    ASTERIA_EXPECT(context, "core retries recovered endpoint", keyboard.GetState() == In::UsbState::Polling,
                   !fixture.Model.Slots[0U].DeviceHalt, fixture.Model.Good);
    fixture.Model.ShortNext = true;
    Report(context, fixture, keyboard, 1U, now, output);
    ASTERIA_EXPECT(context, "short report ignored without losing polling", output.Count == 0U,
                   keyboard.GetState() == In::UsbState::Polling);
    fixture.Model.HoldTransfers = true;
    X::XhciPort keyboardPort{fixture.Controller, 1U};
    keyboard.Step(keyboardPort);
    fixture.Model.HoldCommands = true;
    fixture.Model.Disconnect(1U);
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "disconnect hidden while DMA can run", keyboardPort.GetPortStatus().Connected,
                   keyboard.IsOutstanding(), fixture.Model.Disabled == 0U);
    fixture.Model.HoldCommands = false;
    fixture.Model.Commands();
    Tick(context, fixture, keyboard, 1U, ++now, output);
    ASTERIA_EXPECT(context, "disable slot proves disconnect quiescence", !keyboardPort.GetPortStatus().Connected,
                   keyboard.GetState() == In::UsbState::Detached, fixture.Model.Disabled == 1U);
    ASTERIA_EXPECT(context, "disconnect change follows quiescence", fixture.Controller.TakePortChange(change),
                   change.Port == 1U, !change.Status.Connected);
    fixture.Model.HoldTransfers = false;
    fixture.Model.SetPort(1U, true, 1U);
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "reconnect event", fixture.Controller.TakePortChange(change), change.Port == 1U,
                   change.Status.Connected);
    Enumerate(context, fixture, keyboard, 1U, now);
    ASTERIA_EXPECT(context, "reattach reuses only a disabled slot", fixture.Model.DefaultAddresses == 3U,
                   fixture.Model.Good);
    ASTERIA_EXPECT(context, "EHB acknowledges valid next dequeue", (fixture.Model.LastErdp & 8U) != 0U,
                   (fixture.Model.LastErdp & ~UInt64{15U}) >= fixture.Memory.EventRing.Address,
                   (fixture.Model.LastErdp & ~UInt64{15U}) <
                       fixture.Memory.EventRing.Address + fixture.Memory.EventRing.Bytes,
                   fixture.Model.Observes != 0U, fixture.Model.Publishes != 0U);
    ASTERIA_EXPECT(context, "bounded halt releases ownership", fixture.Controller.Halt());
}

void SeededReports(TestContext& context) noexcept
{
    const auto seeds = Asteria::TestSupport::GetSeedRange(0x58484349U, 8U);
    for (UInt64 seed{seeds.First}; seed - seeds.First < seeds.Count; ++seed)
    {
        const Size failures = context.GetFailureCount();
        Test::Fixture fixture;
        Start(context, fixture);
        In::UsbDevice keyboard{7U};
        In::UsbDevice mouse{8U};
        UInt64 now{0U};
        Enumerate(context, fixture, keyboard, 1U, now);
        Enumerate(context, fixture, mouse, 2U, now);
        Asteria::TestSupport::Random random{seed};
        bool held{false};
        UInt8 buttons{0U};
        fixture.Model.KeyboardReport[0U] = 0U;
        In::EventBatch output{};
        for (Size step{0U}; step < 300U; ++step)
        {
            const bool key = (random.Next() & 1U) != 0U;
            fixture.Model.KeyboardReport[2U] = key ? 4U : 0U;
            Report(context, fixture, keyboard, 1U, now, output);
            ASTERIA_EXPECT(context, "pressed-set reference model", output.Count == (key == held ? 0U : 1U));
            if (output.Count == 1U)
            {
                ASTERIA_EXPECT(context, "key transition and source", output.Events[0U].Usage == 4U,
                               output.Events[0U].Source == 7U,
                               output.Events[0U].Kind == (key ? In::EventKind::KeyDown : In::EventKind::KeyUp));
            }
            held = key;
            const UInt8 nextButtons = static_cast<UInt8>(random.Next() & 7U);
            const UInt8 dx = static_cast<UInt8>(random.Next() % 21U);
            fixture.Model.MouseReport[0U] = nextButtons;
            fixture.Model.MouseReport[1U] = dx;
            fixture.Model.MouseReport[2U] = 0U;
            Report(context, fixture, mouse, 2U, now, output);
            const Size motion = dx == 0U ? 0U : 1U;
            const Size change = nextButtons == buttons ? 0U : 1U;
            ASTERIA_EXPECT(context, "mouse reference snapshot", output.Count == motion + change);
            if (motion != 0U)
            {
                ASTERIA_EXPECT(context, "reference motion", output.Events[0U].Kind == In::EventKind::Motion,
                               output.Events[0U].Dx == dx, output.Events[0U].Dy == 0);
            }
            if (change != 0U)
            {
                ASTERIA_EXPECT(context, "reference button state", output.Events[motion].Kind == In::EventKind::Buttons,
                               output.Events[motion].Buttons == nextButtons);
            }
            buttons = nextButtons;
            ASTERIA_EXPECT(context, "model resource counts and states", fixture.Model.Good,
                           fixture.Model.Slots[0U].Enabled, fixture.Model.Slots[1U].Enabled,
                           keyboard.GetState() == In::UsbState::Polling, mouse.GetState() == In::UsbState::Polling);
            if (context.GetFailureCount() != failures)
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu operation=reports\n", seed, step));
                break;
            }
        }
        ASTERIA_EXPECT(context, "transfer and event rings wrap repeatedly", fixture.Model.LinkFetches >= 35U,
                       fixture.Model.EventWraps >= 35U, fixture.Controller.RejectedEvents() == 0U);
        ASTERIA_EXPECT(context, "seed cleanup", fixture.Controller.Halt());
    }
}
[[nodiscard]] In::UsbRequest Request(const In::UsbOperation operation, const In::UsbSpeed speed,
                                     const UInt64 token = 1U) noexcept
{
    return {.Operation = operation,
            .Token = token,
            .Speed = speed,
            .Address = 23U,
            .PacketSize0 =
                speed == In::UsbSpeed::Super ? UInt16{512U} : (speed == In::UsbSpeed::High ? UInt16{64U} : UInt16{8U}),
            .Setup = {},
            .Endpoint = {},
            .Data = nullptr,
            .Length = 0U};
}
void DirectComplete(TestContext& context, Test::Fixture& fixture, UInt64& now) noexcept
{
    X::Completion completion{};
    bool completed{false};
    for (Size i{0U}; (i < 32U) && !completed; ++i)
    {
        fixture.Controller.Poll(++now);
        completed = fixture.Controller.TakeCompletion(1U, completion);
    }
    ASTERIA_EXPECT(context, "direct operation completes exactly once", completed,
                   completion.Status == In::TransferStatus::Success,
                   !fixture.Controller.TakeCompletion(1U, completion));
}
void Speeds(TestContext& context) noexcept
{
    constexpr In::UsbSpeed speeds[]{In::UsbSpeed::Low, In::UsbSpeed::Full, In::UsbSpeed::High, In::UsbSpeed::Super};
    constexpr UInt32 ids[]{2U, 1U, 3U, 4U};
    constexpr UInt32 intervals[]{6U, 6U, 6U, 6U};
    for (Size i{0U}; i < 4U; ++i)
    {
        Test::Fixture fixture;
        fixture.Model.SetPort(1U, true, ids[i]);
        if (speeds[i] == In::UsbSpeed::Super)
        {
            fixture.Model.Registers[0x40U / 4U] = (3U << 24U) | 2U;
        }
        Start(context, fixture);
        auto request = Request(In::UsbOperation::ResetPort, speeds[i]);
        ASTERIA_EXPECT(context, "speed-specific reset request", fixture.Controller.Submit(1U, request));
        UInt64 now{0U};
        DirectComplete(context, fixture, now);
        ASTERIA_EXPECT(context, "USB2 reset versus automatically trained SuperSpeed",
                       fixture.Model.LastReset == (speeds[i] == In::UsbSpeed::Super ? 0U : 0x210U), fixture.Model.Good);
        ASTERIA_EXPECT(context, "speed in slot context", ((X::Load32(fixture.Outputs[0U]) >> 20U) & 15U) == ids[i],
                       (X::Load32(fixture.Outputs[0U] + 36U) >> 16U) == request.PacketSize0);
        request.Operation = In::UsbOperation::AssignAddress;
        request.Token = 2U;
        ASTERIA_EXPECT(context, "physical address is controller-owned", fixture.Controller.Submit(1U, request));
        DirectComplete(context, fixture, now);
        ASTERIA_EXPECT(context, "physical address differs from logical tag",
                       (X::Load32(fixture.Outputs[0U] + 12U) & 0xFFU) == 1U);
        request.Operation = In::UsbOperation::ConfigureEndpoint;
        request.Token = 3U;
        request.Endpoint = {.Address = 0x81U,
                            .Attributes = 3U,
                            .MaxPacketSize = 8U,
                            .Interval = i < 2U ? UInt8{10U} : UInt8{7U},
                            .MaxBurst = 0U,
                            .CompanionAttributes = 0U,
                            .BytesPerInterval = 8U,
                            .HasCompanion = speeds[i] == In::UsbSpeed::Super};
        if (speeds[i] == In::UsbSpeed::High)
        {
            request.Endpoint.MaxPacketSize = 0x1008U;
        }
        if (speeds[i] == In::UsbSpeed::Super)
        {
            request.Endpoint.MaxBurst = 2U;
            request.Endpoint.BytesPerInterval = 24U;
        }
        ASTERIA_EXPECT(context, "endpoint supplied without host allocation", fixture.Controller.Submit(1U, request));
        DirectComplete(context, fixture, now);
        ASTERIA_EXPECT(context, "interval, packet, burst and ESIT payload", fixture.Model.LastInterval == intervals[i],
                       fixture.Model.LastPacket == 8U, fixture.Model.LastBurst == (i >= 2U ? 2U : 0U),
                       fixture.Model.LastPayload == (i >= 2U ? 24U : 8U));
        request.Operation = In::UsbOperation::UpdateEndpointZero;
        if (speeds[i] == In::UsbSpeed::Full)
        {
            request.PacketSize0 = 16U;
        }
        for (Size repeat{0U}; repeat < 32U; ++repeat)
        {
            ++request.Token;
            ASTERIA_EXPECT(context, "command ring wrap submission", fixture.Controller.Submit(1U, request));
            DirectComplete(context, fixture, now);
        }
        ASTERIA_EXPECT(context, "command Link and cycle wrap", fixture.Model.CommandCount == 36U,
                       fixture.Model.LinkFetches >= 2U, fixture.Model.Evaluations == 32U,
                       (X::Load32(fixture.Outputs[0U] + 36U) >> 16U) == request.PacketSize0,
                       (X::Load32(fixture.Outputs[0U] + 36U) & 0x3EU) == ((4U << 3U) | 6U));
        request.Operation = In::UsbOperation::ConfigureEndpoint;
        request.Endpoint.Interval = 0U;
        ASTERIA_EXPECT(context, "invalid interval refused without retaining request",
                       !fixture.Controller.Submit(1U, request));
        ASTERIA_EXPECT(context, "speed cleanup", fixture.Controller.Halt());
    }
}
void Events(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    UInt64 now{0U};
    fixture.Model.HoldCommands = true;
    auto request = Request(In::UsbOperation::ResetPort, In::UsbSpeed::Full);
    ASTERIA_EXPECT(context, "reset starts asynchronously", fixture.Controller.Submit(1U, request));
    fixture.Controller.Poll(++now);
    fixture.Controller.Poll(11U);
    now = 11U;
    X::Completion result{};
    fixture.Model.Emit({.Parameter = fixture.Memory.CommandRing.Address + 16U,
                        .Status = 1U << 24U,
                        .Control = (33U << 10U) | (1U << 24U)});
    fixture.Model.Emit(
        {.Parameter = fixture.Memory.CommandRing.Address, .Status = 1U << 24U, .Control = (33U << 10U) | (3U << 24U)});
    fixture.Model.Emit(
        {.Parameter = fixture.Memory.CommandRing.Address, .Status = 13U << 24U, .Control = (33U << 10U) | (1U << 24U)});
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "wrong command pointer/slot/code refused", fixture.Controller.RejectedEvents() == 3U,
                   !fixture.Controller.TakeCompletion(1U, result));
    fixture.Model.HoldCommands = false;
    fixture.Model.Commands();
    DirectComplete(context, fixture, now);
    fixture.Model.Emit(fixture.Model.LastCommand);
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "duplicate command completion refused", fixture.Controller.RejectedEvents() == 4U);
    UInt8 buffer[64U]{};
    std::memset(buffer, 0xA5, sizeof(buffer));
    request.Operation = In::UsbOperation::Control;
    request.Token = 9U;
    request.Length = sizeof(buffer);
    request.Data = buffer;
    request.Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, sizeof(buffer));
    fixture.Model.HoldTransfers = true;
    const UInt64 start = fixture.Model.Slots[0U].Endpoints[1U].Pointer;
    ASTERIA_EXPECT(context, "control borrow accepted", fixture.Controller.Submit(1U, request));
    constexpr UInt32 flags = (32U << 10U) | (1U << 16U) | (1U << 24U);
    fixture.Model.Emit({.Parameter = start + 0x10000U, .Status = 1U << 24U, .Control = flags});
    fixture.Model.Emit({.Parameter = start, .Status = 1U << 24U, .Control = (flags & ~(31U << 16U)) | (2U << 16U)});
    fixture.Model.Emit({.Parameter = start + 16U, .Status = (13U << 24U) | 65U, .Control = flags});
    fixture.Model.Emit({.Parameter = start + 16U, .Status = 1U << 24U, .Control = flags});
    fixture.Model.Emit({.Parameter = start + 16U, .Status = 1U << 24U, .Control = flags | 4U});
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "malformed/foreign/premature transfers refused", fixture.Controller.RejectedEvents() == 9U,
                   !fixture.Controller.TakeCompletion(1U, result), buffer[0U] == 0xA5U);
    fixture.Model.HoldTransfers = false;
    fixture.Model.Transfers(1U, 1U);
    fixture.Controller.Poll(++now, 1U);
    ASTERIA_EXPECT(context, "short Data event waits for Status IOC", !fixture.Controller.TakeCompletion(1U, result),
                   buffer[0U] == 0xA5U);
    fixture.Controller.Poll(++now, 1U);
    ASTERIA_EXPECT(context, "short control actual length is Data residual",
                   fixture.Controller.TakeCompletion(1U, result), result.Token == 9U,
                   result.Status == In::TransferStatus::Success, result.ActualLength == 18U, buffer[0U] == 18U,
                   buffer[18U] == 0xA5U);
    // HID SET_REPORT: the setup is immediate, the LED byte is OUT DMA data,
    // and the no-payload Status stage runs IN.
    buffer[0U] = 3U;
    request.Token = 10U;
    request.Length = 1U;
    request.Setup = {.Bytes = {0x21U, 9U, 0U, 2U, 0U, 0U, 1U, 0U}};
    ASTERIA_EXPECT(context, "OUT control transfer accepted", fixture.Controller.Submit(1U, request));
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "OUT data bounced and directions checked by model",
                   fixture.Controller.TakeCompletion(1U, result), fixture.Buffers[0U][0U] == 3U, fixture.Model.Good);
    fixture.Model.Emit({.Parameter = start + 32U, .Status = 1U << 24U, .Control = flags});
    fixture.Model.Emit({.Parameter = UInt64{3U} << 24U, .Status = 1U << 24U, .Control = 34U << 10U});
    fixture.Model.Emit({.Parameter = 0U, .Status = 0U, .Control = 0U});
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "retired TD, foreign port and zero event refused",
                   fixture.Controller.RejectedEvents() == 12U);
    ASTERIA_EXPECT(context, "event cleanup", fixture.Controller.Halt());
}
void EndpointZeroRecovery(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    In::UsbDevice device;
    X::XhciPort port{fixture.Controller, 1U};
    ASTERIA_EXPECT(context, "attach before EP0 transaction error", device.Attach(7U));
    UInt64 now{0U};
    In::EventBatch output{};
    device.Step(port);
    for (Size i{0U}; (i < 32U) && device.IsOutstanding(); ++i)
    {
        Tick(context, fixture, device, 1U, ++now, output);
    }
    ASTERIA_EXPECT(context, "first descriptor ready", device.GetState() == In::UsbState::FirstDescriptor);
    fixture.Model.NextCode = 4U;
    device.Step(port);
    for (Size i{0U}; (i < 32U) && device.IsOutstanding(); ++i)
    {
        Tick(context, fixture, device, 1U, ++now, output);
    }
    ASTERIA_EXPECT(context, "EP0 transaction error retried after ring recovery",
                   device.GetState() == In::UsbState::FirstDescriptor, fixture.Model.EndpointResets == 1U,
                   fixture.Model.DequeueSets == 1U, fixture.Model.ClearHalts == 0U);
    for (Size i{0U}; (i < 50U) && (device.GetState() != In::UsbState::Polling); ++i)
    {
        device.Step(port);
        Tick(context, fixture, device, 1U, ++now, output);
    }
    ASTERIA_EXPECT(context, "enumeration survives EP0 recovery", device.GetState() == In::UsbState::Polling,
                   fixture.Model.Good, fixture.Controller.Halt());
}
void FailureCase0(TestContext& context) noexcept
{
    Test::Fixture fixture;
    fixture.Model.HoldReset = true;
    const UInt32 before = fixture.Model.Reads;
    ASTERIA_EXPECT(context, "bounded reset timeout", !fixture.Controller.Initialize(fixture.Memory, 0x2100U, 4U),
                   fixture.Model.Reads - before < 40U, fixture.Controller.GetState() == X::ControllerState::Faulted);
}

void FailureCase1(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    fixture.Model.HoldPortReset = true;
    const auto request = Request(In::UsbOperation::ResetPort, In::UsbSpeed::Full);
    ASTERIA_EXPECT(context, "port reset accepted", fixture.Controller.Submit(1U, request));
    fixture.Controller.Poll(1000U);
    X::Completion result{};
    ASTERIA_EXPECT(context, "port reset deadline ends request", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::TimedOut,
                   fixture.Controller.GetState() == X::ControllerState::Running);
}

void FailureCase2(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    fixture.Model.HoldCommands = true;
    const auto request = Request(In::UsbOperation::ResetPort, In::UsbSpeed::Full);
    ASTERIA_EXPECT(context, "command request accepted", fixture.Controller.Submit(1U, request));
    fixture.Controller.Poll(1U);
    fixture.Controller.Poll(11U);
    fixture.Model.HoldHalt = true;
    fixture.Controller.Poll(1011U);
    X::Completion result{};
    ASTERIA_EXPECT(context, "failed halt retains borrows until caller quiesces",
                   !fixture.Controller.TakeCompletion(1U, result),
                   fixture.Controller.GetState() == X::ControllerState::Faulted);
    fixture.Model.HoldHalt = false;
    ASTERIA_EXPECT(context, "later successful halt revokes borrows", fixture.Controller.Halt(4U),
                   fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Retryable);
}

void FailureCase3(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    In::UsbDevice device;
    UInt64 now{0U};
    Enumerate(context, fixture, device, 1U, now);
    fixture.Model.HoldTransfers = true;
    X::XhciPort port{fixture.Controller, 1U};
    device.Step(port);
    now += 60000U;
    fixture.Controller.Poll(now);
    X::Completion result{};
    ASTERIA_EXPECT(context, "idle interrupt IN retains request indefinitely",
                   !fixture.Controller.TakeCompletion(1U, result), device.IsOutstanding(),
                   device.GetState() == In::UsbState::Polling,
                   fixture.Controller.GetState() == X::ControllerState::Running);
    fixture.Model.HoldTransfers = false;
    fixture.Model.Transfers(1U, 3U);
    In::EventBatch output{};
    Tick(context, fixture, device, 1U, ++now, output);
    ASTERIA_EXPECT(context, "first report after long idle reaches HID", output.Count == 2U,
                   device.GetState() == In::UsbState::Polling, fixture.Model.Good);
}

void FailureCase4(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    In::UsbDevice device;
    UInt64 now{0U};
    Enumerate(context, fixture, device, 1U, now);
    fixture.Model.HoldTransfers = true;
    X::XhciPort port{fixture.Controller, 1U};
    device.Step(port);
    fixture.Model.HoldCommands = true;
    fixture.Model.Disconnect(1U);
    fixture.Controller.Poll(++now);
    fixture.Controller.Poll(now + 1000U);
    X::Completion result{};
    ASTERIA_EXPECT(context, "disconnect deadline halt proves quiescence", !port.GetPortStatus().Connected,
                   fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Disconnected);
}

void FailureCase5(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    In::UsbDevice device;
    UInt64 now{0U};
    Enumerate(context, fixture, device, 1U, now);
    fixture.Model.NextCode = 4U;
    In::EventBatch output{};
    Report(context, fixture, device, 1U, now, output);
    for (Size i{0U}; (i < 32U) && device.IsOutstanding(); ++i)
    {
        Tick(context, fixture, device, 1U, ++now, output);
    }
    Report(context, fixture, device, 1U, now, output);
    ASTERIA_EXPECT(context, "transaction error recovery and retry", device.GetState() == In::UsbState::Polling,
                   fixture.Model.EndpointResets == 1U, fixture.Model.DequeueSets == 1U, fixture.Model.Good);
    for (Size i{0U}; i < 15U; ++i)
    {
        fixture.Model.Emit({.Parameter = UInt64{2U} << 24U, .Status = 1U << 24U, .Control = 34U << 10U});
    }
    ASTERIA_EXPECT(context, "model detects full event ring", fixture.Model.EventRingFullCount == 1U);
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "event-ring failure halts controller",
                   fixture.Controller.GetState() == X::ControllerState::Halted);
}

void Failures(TestContext& context) noexcept
{
    using FailureTest = void (*)(TestContext&) noexcept;
    constexpr FailureTest cases[]{FailureCase0, FailureCase1, FailureCase2, FailureCase3, FailureCase4, FailureCase5};
    for (const FailureTest run : cases)
    {
        run(context);
    }
    constexpr UInt8 codes[]{5U, 11U, 12U, 14U, 15U, 255U};
    for (const UInt8 code : codes)
    {
        Test::Fixture fixture;
        Start(context, fixture);
        In::UsbDevice device;
        UInt64 now{0U};
        Enumerate(context, fixture, device, 1U, now);
        fixture.Model.NextCode = code;
        In::EventBatch output{};
        Report(context, fixture, device, 1U, now, output);
        ASTERIA_EXPECT(context, "fatal completion codes mapped to core Error",
                       device.GetState() == In::UsbState::Failed, output.Count == 0U, fixture.Model.Good);
        ASTERIA_EXPECT(context, "error cleanup", fixture.Controller.Halt());
    }
}
void RecoveryTiming(TestContext& context) noexcept
{
    Test::Fixture fixture;
    Start(context, fixture);
    auto request = Request(In::UsbOperation::ResetPort, In::UsbSpeed::Full);
    ASTERIA_EXPECT(context, "root reset accepted for timing", fixture.Controller.Submit(1U, request));
    fixture.Controller.Poll(1U);
    fixture.Controller.Poll(10U);
    ASTERIA_EXPECT(context, "no command before USB reset recovery", fixture.Model.CommandCount == 0U);
    fixture.Controller.Poll(11U);
    fixture.Controller.Poll(12U);
    X::Completion result{};
    ASTERIA_EXPECT(context, "reset completes after ten milliseconds", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::Success);
    request.Operation = In::UsbOperation::AssignAddress;
    request.Token = 2U;
    ASTERIA_EXPECT(context, "address request accepted for timing", fixture.Controller.Submit(1U, request));
    fixture.Controller.Poll(13U);
    fixture.Controller.Poll(14U);
    ASTERIA_EXPECT(context, "address completion waits settling interval",
                   !fixture.Controller.TakeCompletion(1U, result));
    fixture.Controller.Poll(15U);
    ASTERIA_EXPECT(context, "address completes after two milliseconds", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::Success, fixture.Controller.Halt());
}
void RingPublication(TestContext& context) noexcept
{
    UInt8 bytes[256U]{};
    X::XhciRing ring;
    ring.Initialize({.Address = 0x100000U, .Cpu = bytes, .Bytes = sizeof(bytes)});
    for (Size iteration{0U}; iteration < 45U; ++iteration)
    {
        const Size index = iteration % 15U;
        const UInt32 cycle = ((iteration / 15U) & 1U) == 0U ? 1U : 0U;
        ring.Begin();
        const UInt64 pointer = ring.Push({.Parameter = iteration, .Status = 0U, .Control = 9U << 10U});
        ASTERIA_EXPECT(context, "producer leaves first TRB unowned before publish", pointer == 0x100000U + index * 16U,
                       (X::Load32(bytes + index * 16U + 12U) & 1U) == (cycle ^ 1U));
        ring.Commit();
        ASTERIA_EXPECT(context, "commit publishes expected cycle",
                       (X::Load32(bytes + index * 16U + 12U) & 1U) == cycle);
        if (index == 14U)
        {
            ASTERIA_EXPECT(context, "Link is aligned and toggles cycle", X::Load64(bytes + 240U) == 0x100000U,
                           X::Load32(bytes + 252U) == ((6U << 10U) | 2U | cycle),
                           ring.Dequeue() == (0x100000U | (cycle ^ 1U)));
        }
    }
}
void Validation(TestContext& context) noexcept
{
    for (Size fault{0U}; fault < 17U; ++fault)
    {
        Test::Fixture fixture;
        switch (fault)
        {
        case 0U:
            fixture.Memory.Dcbaa.Address += 1U;
            break;
        case 1U:
            fixture.Ports[0U].InputContext.Bytes = 32U;
            break;
        case 2U:
            fixture.Memory.EventRing.Bytes = 240U;
            break;
        case 3U:
            fixture.Memory.CommandRing.Address += 0xFF80U;
            break;
        case 4U:
            fixture.Ports[0U].Buffer.Cpu = nullptr;
            break;
        case 5U:
            fixture.Ports[0U].Buffer.Address = ~UInt64{63U};
            break;
        case 6U:
            fixture.Memory.ScratchpadCount = 0U;
            break;
        case 7U:
            fixture.ScratchPages[0U].Address += 64U;
            break;
        case 8U:
            fixture.Ports[0U].Buffer.Address = fixture.Memory.CommandRing.Address;
            break;
        case 9U:
            fixture.Ports[0U].Buffer.Cpu = fixture.Command;
            break;
        case 10U:
            fixture.Model.Registers[6U] = 0xFFF0U;
            break;
        case 11U:
            fixture.Model.Registers[2U] = 1U << 21U;
            break; // scratchpad high bits => 32, not zero
        case 12U:
            fixture.Model.Registers[4U] = (0xFFF0U << 16U) | 1U;
            break; // out-of-BAR extended capability
        case 13U:
            fixture.Model.Registers[0x48U / 4U] |= 1U << 28U;
            break; // missing PSI encoding
        case 14U:
            fixture.Model.Registers[Test::FakeXhci::Operational / 4U + 2U] = 0U;
            break;
        case 15U:
            fixture.Model.Registers[1U] = (17U << 24U) | (1U << 8U);
            break;
        case 16U:
            fixture.Model.Registers[2U] = 2U << 21U;
            break; // scratchpad capacity mismatch
        default:
            break;
        }
        ASTERIA_EXPECT(context, "invalid memory/capabilities refused before reset",
                       !fixture.Controller.Initialize(fixture.Memory, 0x2100U), fixture.Model.Resets == 0U,
                       fixture.Model.DoorbellCount == 0U, !fixture.Controller.Halt());
    }
    {
        Test::Fixture fixture{false, 1U, 0x100000000U};
        fixture.Model.Registers[4U] &= ~1U;
        ASTERIA_EXPECT(context, "32-bit DMA controller rejects high mappings",
                       !fixture.Controller.Initialize(fixture.Memory, 0x2100U));
    }
    {
        Test::Fixture fixture{false, 0U};
        ASTERIA_EXPECT(context, "zero scratchpads needs no array",
                       fixture.Controller.Initialize(fixture.Memory, 0x2100U), X::Load64(fixture.Dcbaa) == 0U,
                       fixture.Controller.Halt());
    }
    {
        Test::Fixture fixture;
        // Exercise the high five bits with real caller storage, not a refusal.
        UInt8 pages[32U][4096U]{};
        X::DmaMemory mappings[32U]{};
        fixture.Model.Registers[2U] = 1U << 21U;
        for (Size i{0U}; i < 32U; ++i)
        {
            mappings[i] = fixture.Map(0x400000U + i * 4096U, pages[i], sizeof(pages[i]));
        }
        fixture.Memory.Scratchpads = mappings;
        fixture.Memory.ScratchpadCount = 32U;
        ASTERIA_EXPECT(context, "all requested scratchpads supplied with high count bits",
                       fixture.Controller.Initialize(fixture.Memory, 0x2100U));
        for (Size i{0U}; i < 32U; ++i)
        {
            ASTERIA_EXPECT(context, "scratchpad array addresses",
                           X::Load64(fixture.ScratchArray + i * 8U) == mappings[i].Address);
        }
        ASTERIA_EXPECT(context, "scratchpad cleanup", fixture.Controller.Halt(), fixture.Model.Good);
    }
    ASTERIA_EXPECT(
        context, "completion status codec", X::CompletionStatus(1U) == In::TransferStatus::Success,
        X::CompletionStatus(13U) == In::TransferStatus::Success, X::CompletionStatus(6U) == In::TransferStatus::Stall,
        X::CompletionStatus(4U) == In::TransferStatus::Retryable, X::CompletionStatus(3U) == In::TransferStatus::Error);
}
void DeadlineAbortRace(TestContext& context) noexcept
{
    // Both event orders, including completion and a stall that wins Stop.
    for (UInt32 order{0U}; order < 4U; ++order)
    {
        Test::Fixture fixture;
        Start(context, fixture);
        In::UsbDevice device;
        X::XhciPort port{fixture.Controller, 1U};
        UInt64 now{0U};
        ASTERIA_EXPECT(context, "attach timeout core", device.Attach(7U));
        In::EventBatch events{};
        for (Size i{0U}; i < 40U && device.GetState() != In::UsbState::FirstDescriptor; ++i)
        {
            device.Step(port);
            Tick(context, fixture, device, 1U, ++now, events);
        }
        fixture.Model.HoldTransfers = true;
        fixture.Model.StopEventAfterCommand = order != 0U;
        fixture.Model.StopCode = order == 2U ? 1U : 26U;
        device.Step(port);
        const UInt64 oldPointer = fixture.Model.Slots[0U].Endpoints[1U].Pointer;
        if (order == 3U)
        {
            // Stall already reached DMA context, but its event is not consumed
            // until after software initiates cancellation.
            fixture.Model.Slots[0U].Halted[1U] = true;
            X::Store32(fixture.Outputs[0U] + fixture.Model.ContextBytes, 2U);
        }
        now += 1000U;
        fixture.Controller.Poll(now, 0U);
        X::Completion result{};
        ASTERIA_EXPECT(context, "timeout waits for stop, never reports early",
                       !fixture.Controller.TakeCompletion(1U, result), device.IsOutstanding(),
                       fixture.Controller.GetState() == X::ControllerState::Running, !port.Abort(99999U));
        for (Size i{0U}; i < 10U && device.IsOutstanding(); ++i)
        {
            // Split Stop command / late transfer events over separate polls.
            fixture.Controller.Poll(++now, 1U);
            const bool completed = fixture.Controller.TakeCompletion(1U, result);
            ASTERIA_EXPECT(context, "cancelled TD stays owned until queued events drain",
                           !completed || ((fixture.Model.LastErdp & ~UInt64{15U}) ==
                                          fixture.Model.EventBase + fixture.Model.EventIndex * 16U));
            if (completed)
            {
                ASTERIA_EXPECT(context, "timeout completes once after dequeue",
                               result.Status == In::TransferStatus::TimedOut, fixture.Model.DequeueSets == 1U,
                               fixture.Model.EndpointStops == (order == 3U ? 0U : 1U),
                               device.Complete(result.Token, result.Status, result.ActualLength, events));
            }
        }
        ASTERIA_EXPECT(context, "timeout leaves descriptor step retryable", !device.IsOutstanding(),
                       device.GetState() == In::UsbState::FirstDescriptor, fixture.Model.Good,
                       !fixture.Controller.TakeCompletion(1U, result));
        fixture.Model.Emit(
            {.Parameter = oldPointer, .Status = 1U << 24U, .Control = (32U << 10U) | (1U << 16U) | (1U << 24U)});
        fixture.Model.HoldTransfers = false;
        device.Step(port);
        Tick(context, fixture, device, 1U, ++now, events);
        ASTERIA_EXPECT(context, "next enumeration step ignores old TD event and succeeds",
                       device.GetState() == In::UsbState::EndpointZero, fixture.Model.Good,
                       fixture.Controller.GetState() == X::ControllerState::Running);
        for (Size i{0U}; i < 100U && device.GetState() != In::UsbState::Polling; ++i)
        {
            device.Step(port);
            Tick(context, fixture, device, 1U, ++now, events);
        }
        fixture.Model.HoldTransfers = true;
        device.Step(port);
        ASTERIA_EXPECT(context, "core aborts poll by current token", device.Abort(port), !device.Abort(port));
        for (Size i{0U}; i < 10U && device.IsOutstanding(); ++i)
        {
            fixture.Controller.Poll(++now);
            if (fixture.Controller.TakeCompletion(1U, result))
            {
                ASTERIA_EXPECT(context, "explicit cancellation is distinct",
                               result.Status == In::TransferStatus::Cancelled,
                               device.Complete(result.Token, result.Status, result.ActualLength, events));
            }
        }
        ASTERIA_EXPECT(context, "cancellation leaves polling and controller running", !device.IsOutstanding(),
                       device.GetState() == In::UsbState::Polling,
                       fixture.Controller.GetState() == X::ControllerState::Running);
        device.Step(port);
        fixture.Model.Registers[(Test::FakeXhci::Operational + 4U) / 4U] |= 4U;
        fixture.Controller.Poll(++now);
        ASTERIA_EXPECT(context, "host system error still halts with an Error completion",
                       fixture.Controller.GetState() == X::ControllerState::Halted,
                       fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Error,
                       !fixture.Controller.TakeCompletion(1U, result));
    }
}

void OverCurrent(TestContext& context) noexcept
{
    Test::Fixture fixture{};
    Start(context, fixture);
    constexpr UInt32 offset = Test::FakeXhci::Operational + 0x400U;
    fixture.Model.Registers[4U] |= 8U;
    fixture.Model.Registers[offset / 4U] &= ~0x200U;
    fixture.Controller.PowerPorts();
    ASTERIA_EXPECT(context, "root power preserves connection and change bits",
                   (fixture.Model.Registers[offset / 4U] & 0x200U) != 0U,
                   (fixture.Model.Registers[offset / 4U] & 1U) != 0U);
    fixture.Model.Registers[offset / 4U] |= 8U | (1U << 20U);
    fixture.Controller.Poll(1U);
    ASTERIA_EXPECT(context, "over-current removes port power and reports a disconnected port",
                   fixture.Controller.IsPortPoweredOff(1U), !fixture.Controller.GetPortStatus(1U).Connected,
                   (fixture.Model.Registers[offset / 4U] & 0x200U) == 0U,
                   (fixture.Model.Registers[offset / 4U] & (1U << 20U)) == 0U);
    fixture.Model.Registers[offset / 4U] &= ~8U;
    fixture.Controller.PowerPorts();
    ASTERIA_EXPECT(context, "latched over-current never repowers a port",
                   (fixture.Model.Registers[offset / 4U] & 0x200U) == 0U);
    fixture.Model.SetPort(1U, true, 1U);
    fixture.Controller.Poll(2U);
    ASTERIA_EXPECT(context, "a faulted port stays disabled until controller ownership is reset",
                   fixture.Controller.IsPortPoweredOff(1U), !fixture.Controller.GetPortStatus(1U).Connected);
}

void PolledEvents(TestContext& context) noexcept
{
    Test::Fixture fixture{};
    ASTERIA_EXPECT(context, "event ring can run without interrupt authority",
                   fixture.Controller.Initialize(fixture.Memory, 0x2100U, 1000U, X::EventDelivery::Poll),
                   (fixture.Model.Registers[Test::FakeXhci::Operational / 4U] & 5U) == 1U,
                   (fixture.Model.Registers[(Test::FakeXhci::Runtime + 0x20U) / 4U] & 2U) == 0U);
    In::UsbDevice device;
    UInt64 now{0U};
    Enumerate(context, fixture, device, 1U, now);
    In::EventBatch events{};
    Report(context, fixture, device, 1U, now, events);
    ASTERIA_EXPECT(context, "polling enumerates and decodes HID without enabling interrupts", events.Count == 2U,
                   (fixture.Model.Registers[(Test::FakeXhci::Runtime + 0x20U) / 4U] & 2U) == 0U,
                   (fixture.Model.Registers[Test::FakeXhci::Operational / 4U] & 4U) == 0U,
                   fixture.Controller.GetState() == X::ControllerState::Running, fixture.Controller.Halt());
}

void ConcurrentRoots(TestContext& context) noexcept
{
    constexpr UInt8 classes[]{0U, 8U, 9U, 0xFFU};
    for (const UInt8 deviceClass : classes)
    {
        Test::Fixture fixture{};
        fixture.Model.UnsupportedClass = deviceClass;
        Start(context, fixture);
        In::UsbDevice first;
        In::UsbDevice second;
        X::XhciPort firstPort{fixture.Controller, 1U};
        X::XhciPort secondPort{fixture.Controller, 2U};
        ASTERIA_EXPECT(context, "attach concurrent root devices", first.Attach(1U), second.Attach(2U));
        UInt64 now{0U};
        In::EventBatch events{};
        for (Size step{0U}; step < 100U; ++step)
        {
            // The component can step once per bound interface, in the same
            // tick. Shared-command contention must not exhaust core retries.
            for (Size turn{0U}; turn < 3U; ++turn)
            {
                first.Step(firstPort);
                second.Step(secondPort);
            }
            fixture.Controller.Poll(++now);
            X::Completion result{};
            if (fixture.Controller.TakeCompletion(1U, result))
            {
                ASTERIA_EXPECT(context, "first root consumes its completion",
                               first.Complete(result.Token, result.Status, result.ActualLength, events));
            }
            if (fixture.Controller.TakeCompletion(2U, result))
            {
                ASTERIA_EXPECT(context, "second root consumes its completion",
                               second.Complete(result.Token, result.Status, result.ActualLength, events));
            }
        }
        ASTERIA_EXPECT(context, "unsupported root leaves neighbouring HID and controller alive",
                       second.GetState() == In::UsbState::Polling,
                       first.GetState() == (deviceClass == 0U ? In::UsbState::Polling : In::UsbState::Failed),
                       fixture.Model.Configurations == (deviceClass == 0U ? 2U : 1U), fixture.Model.Good,
                       fixture.Controller.GetState() == X::ControllerState::Running, fixture.Controller.Halt());
    }
}

} // namespace
int main()
{
    TestContext context{"xHCI controller/model"};
    PolledEvents(context);
    ConcurrentRoots(context);
    OverCurrent(context);
    DeadlineAbortRace(context);
    EndToEnd(context, false, 0x100000U);
    EndToEnd(context, true, 0x100000000U);
    SeededReports(context);
    Speeds(context);
    Events(context);
    Failures(context);
    EndpointZeroRecovery(context);
    Validation(context);
    RingPublication(context);
    RecoveryTiming(context);
    return context.Finish();
}
