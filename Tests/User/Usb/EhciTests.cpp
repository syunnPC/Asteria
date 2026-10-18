// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Ehci.hpp"
#include "User/Usb/UsbHub.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Usb/FakeEhci.hpp"

namespace
{
namespace E = Asteria::User::Usb;
namespace In = Asteria::User::Input;
namespace Test = Asteria::Test::Usb;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::TestSupport::TestContext;
void Start(TestContext& context, Test::EhciFixture& fixture) noexcept
{
    ASTERIA_EXPECT(context, "BIOS releases ownership before SMI disable",
                   fixture.Controller.Handoff(0x100U) == E::HandoffStatus::Success,
                   (fixture.Model.Legacy & (1U << 24U)) != 0U, (fixture.Model.Legacy & (1U << 16U)) == 0U,
                   (fixture.Model.LegacyControl & 0xE03FU) == 0U);
    ASTERIA_EXPECT(context, "caller DMA initializes EHCI", fixture.Controller.Initialize(fixture.Memory, 0x100U),
                   fixture.Controller.GetState() == E::EhciState::Running,
                   fixture.Model.Read32(Test::FakeEhci::Operational + 0x40U) == 1U,
                   fixture.Model.Read32(Test::FakeEhci::Operational + 0x14U) ==
                       static_cast<UInt32>(fixture.Memory.Frames.Address & 0xFFFFFFFFU),
                   fixture.Model.Good);
}
void HubTick(TestContext& context, Test::EhciFixture& fixture, E::UsbHub& hub, UInt64& now) noexcept
{
    E::EhciPort pipe{fixture.Controller, 1U};
    fixture.Controller.Poll(++now);
    E::EhciCompletion result{};
    if (fixture.Controller.TakeCompletion(1U, result))
    {
        ASTERIA_EXPECT(context, "hub matches controller completion",
                       hub.Complete(result.Token, result.Status, result.ActualLength));
    }
    hub.Step(pipe, now);
}
void HubEnumerate(TestContext& context, Test::EhciFixture& fixture, E::UsbHub& hub, UInt64& now) noexcept
{
    ASTERIA_EXPECT(context, "attach root hub", hub.Attach(2U));
    for (Size i{0U}; i < 500U; ++i)
    {
        HubTick(context, fixture, hub, now);
    }
    // End on an idle point, so ResetChild does not compete with a poll request.
    E::EhciCompletion result{};
    fixture.Controller.Poll(++now);
    if (fixture.Controller.TakeCompletion(1U, result))
    {
        ASTERIA_EXPECT(context, "last hub completion", hub.Complete(result.Token, result.Status, result.ActualLength));
    }
    ASTERIA_EXPECT(context, "Intel hub ports powered and status changes acknowledged",
                   hub.GetState() == E::HubState::Ready, hub.PortCount() == 6U, fixture.Model.PowerWrites == 6U,
                   fixture.Model.ClearChanges >= 2U, hub.GetPortStatus(1U).Connected, hub.GetPortStatus(2U).Connected,
                   fixture.Model.Good);
}
void ChildEnumerate(TestContext& context, Test::EhciFixture& fixture, E::UsbHub& hub, In::UsbDevice& device,
                    const UInt8 child, UInt64& now) noexcept
{
    ASTERIA_EXPECT(context, "request hub port reset", hub.ResetChild(child));
    for (Size i{0U}; i < 100U && !hub.GetPortStatus(child).ResetComplete; ++i)
    {
        HubTick(context, fixture, hub, now);
    }
    // Finish the reset-change clear before switching the serialized schedule to the child.
    for (Size i{0U}; i < 20U && hub.IsOutstanding(); ++i)
    {
        fixture.Controller.Poll(++now);
        E::EhciCompletion result{};
        if (fixture.Controller.TakeCompletion(1U, result))
        {
            ASTERIA_EXPECT(context, "reset change completion",
                           hub.Complete(result.Token, result.Status, result.ActualLength));
        }
    }
    ASTERIA_EXPECT(context, "port reset yields enabled child and TT route", hub.GetPortStatus(child).ResetComplete,
                   hub.Route(child).HubAddress == 2U, hub.Route(child).HubPort == child);
    E::EhciPort pipe{fixture.Controller, static_cast<UInt8>(child + 1U)};
    E::UsbHubPort port{hub, child, pipe};
    ASTERIA_EXPECT(context, "attach one-level child", device.Attach(static_cast<UInt8>(child + 8U), hub.Route(child)));
    In::EventBatch events{};
    for (Size i{0U}; i < 100U && device.GetState() != In::UsbState::Polling &&
                     device.GetState() != In::UsbState::UnsupportedHubDepth;
         ++i)
    {
        device.Step(port);
        fixture.Controller.Poll(++now);
        E::EhciCompletion result{};
        if (fixture.Controller.TakeCompletion(static_cast<UInt8>(child + 1U), result))
        {
            ASTERIA_EXPECT(context, "child accepts token",
                           device.Complete(result.Token, result.Status, result.ActualLength, events));
        }
    }
}
void Report(TestContext& context, Test::EhciFixture& fixture, E::UsbHub& hub, In::UsbDevice& device, const UInt8 child,
            UInt64& now, In::EventBatch& events) noexcept
{
    E::EhciPort pipe{fixture.Controller, static_cast<UInt8>(child + 1U)};
    E::UsbHubPort port{hub, child, pipe};
    events.Count = 0U;
    device.Step(port);
    for (Size i{0U}; i < 20U && device.IsOutstanding(); ++i)
    {
        fixture.Controller.Poll(++now);
        E::EhciCompletion result{};
        if (fixture.Controller.TakeCompletion(static_cast<UInt8>(child + 1U), result))
        {
            ASTERIA_EXPECT(context, "real core accepts periodic qTD completion",
                           device.Complete(result.Token, result.Status, result.ActualLength, events));
        }
    }
}
void EndToEnd(TestContext& context, const UInt64 base, const bool address64, const bool low) noexcept
{
    Test::EhciFixture fixture{base, address64};
    if (low)
    {
        fixture.Model.HubStatus[0U] |= 1U << 9U;
    }
    Start(context, fixture);
    E::UsbHub hub;
    UInt64 now{0U};
    HubEnumerate(context, fixture, hub, now);
    In::UsbDevice keyboard{17U};
    In::UsbDevice mouse{18U};
    ChildEnumerate(context, fixture, hub, keyboard, 1U, now);
    ASTERIA_EXPECT(context, "keyboard enumerates through EHCI TT control", keyboard.GetState() == In::UsbState::Polling,
                   fixture.Model.ChildAddresses[0U] == 9U, fixture.Model.Good);
    ChildEnumerate(context, fixture, hub, mouse, 2U, now);
    ASTERIA_EXPECT(context, "mouse enumerates through EHCI TT control", mouse.GetState() == In::UsbState::Polling,
                   fixture.Model.ChildAddresses[1U] == 10U, fixture.Model.Good);
    In::EventBatch events{};
    E::EhciPort idlePipe{fixture.Controller, 2U};
    E::UsbHubPort idlePort{hub, 1U, idlePipe};
    fixture.Model.HoldTransfers = true;
    keyboard.Step(idlePort);
    now += 60000U;
    fixture.Controller.Poll(now);
    E::EhciCompletion idleResult{};
    ASTERIA_EXPECT(context, "idle interrupt IN has no controller/core deadline",
                   !fixture.Controller.TakeCompletion(2U, idleResult), keyboard.IsOutstanding(),
                   keyboard.GetState() == In::UsbState::Polling,
                   fixture.Controller.GetState() == E::EhciState::Running);
    fixture.Model.HoldTransfers = false;
    Report(context, fixture, hub, keyboard, 1U, now, events);
    ASTERIA_EXPECT(context, "synthetic boot keyboard emits input events", events.Count == 2U,
                   events.Events[0U].Usage == 0xE1U, events.Events[1U].Usage == 4U, events.Events[1U].Source == 17U,
                   fixture.Model.LastMasks == 0x1C01U, fixture.Model.Splits != 0U, fixture.Model.Good);
    const UInt32 firstFrame = fixture.Model.LastFrame;
    Report(context, fixture, hub, keyboard, 1U, now, events);
    ASTERIA_EXPECT(context, "periodic phase survives each new report", events.Count == 0U,
                   ((fixture.Model.LastFrame - firstFrame) & 0x3FFFU) >= 64U, fixture.Model.Good);
    Report(context, fixture, hub, mouse, 2U, now, events);
    ASTERIA_EXPECT(context, "synthetic boot mouse emits motion/buttons", events.Count == 2U,
                   events.Events[0U].Kind == In::EventKind::Motion, events.Events[0U].Dx == 5,
                   events.Events[0U].Dy == -3, events.Events[1U].Kind == In::EventKind::Buttons, fixture.Model.Good);
    fixture.Model.ShortNext = true;
    Report(context, fixture, hub, keyboard, 1U, now, events);
    ASTERIA_EXPECT(context, "short boot report ignored", events.Count == 0U,
                   keyboard.GetState() == In::UsbState::Polling);
    fixture.Model.FaultNext = 0x48U;
    Report(context, fixture, hub, keyboard, 1U, now, events);
    ASTERIA_EXPECT(context, "transaction error retries finitely", keyboard.GetState() == In::UsbState::Polling,
                   !keyboard.IsOutstanding());
    Report(context, fixture, hub, keyboard, 1U, now, events);
    ASTERIA_EXPECT(context, "retry retains data toggle", keyboard.GetState() == In::UsbState::Polling,
                   fixture.Model.Good);
    fixture.Model.FaultNext = 0x40U;
    Report(context, fixture, hub, keyboard, 1U, now, events);
    ASTERIA_EXPECT(context, "stall is reportable without failing polling core",
                   keyboard.GetState() == In::UsbState::Polling);
    fixture.Model.HubStatus[1U] &= static_cast<Asteria::Foundation::UInt16>(~3U);
    fixture.Model.HubChanges[1U] |= 1U;
    for (Size i{0U}; i < 150U; ++i)
    {
        HubTick(context, fixture, hub, now);
    }
    E::EhciPort mousePipe{fixture.Controller, 3U};
    E::UsbHubPort mousePort{hub, 2U, mousePipe};
    mouse.Step(mousePort);
    ASTERIA_EXPECT(context, "polling sees hub-child disconnect", mouse.GetState() == In::UsbState::Detached,
                   !hub.GetPortStatus(2U).Connected, fixture.Model.Good);
    ASTERIA_EXPECT(context, "halt reaches HCHalted", fixture.Controller.Halt());
}
void HandoffAndBounds(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    ASTERIA_EXPECT(context, "handoff must be called first", !fixture.Controller.Initialize(fixture.Memory, 0x100U));
    fixture.Model.BiosStuck = true;
    ASTERIA_EXPECT(context, "unreleased BIOS is a distinct bounded timeout",
                   fixture.Controller.Handoff(0x100U, 7U) == E::HandoffStatus::BiosTimeout,
                   fixture.Model.LegacyReads == 8U, (fixture.Model.LegacyControl & 0xE03FU) == 0xE03FU,
                   !fixture.Controller.Initialize(fixture.Memory, 0x100U));
    fixture.Model.BiosStuck = false;
    ASTERIA_EXPECT(context, "handoff can retry after release",
                   fixture.Controller.Handoff(0x100U) == E::HandoffStatus::Success);
    fixture.Memory.Queue.Address |= UInt64{1U} << 32U;
    ASTERIA_EXPECT(context, "32bit DMA rejects high schedule addresses",
                   !fixture.Controller.Initialize(fixture.Memory, 0x100U));
    fixture.Memory.Queue.Address &= 0xFFFFFFFFU;
    const UInt64 queueAddress = fixture.Memory.Queue.Address;
    fixture.Memory.Queue.Address = fixture.Memory.AsyncHead.Address;
    ASTERIA_EXPECT(context, "DMA overlap rejected", !fixture.Controller.Initialize(fixture.Memory, 0x100U));
    fixture.Memory.Queue.Address = queueAddress;
    ASTERIA_EXPECT(context, "validated memory still initializes",
                   fixture.Controller.Initialize(fixture.Memory, 0x100U));
    ASTERIA_EXPECT(context, "bounded halt", fixture.Controller.Halt(3U));
    Test::EhciFixture malformed;
    malformed.Model.Legacy = 1U | (0x40U << 8U);
    ASTERIA_EXPECT(context, "EECP cycle rejected",
                   malformed.Controller.Handoff(0x100U) == E::HandoffStatus::InvalidCapability);
    E::UsbHub deep;
    ASTERIA_EXPECT(context, "second hub level rejected clearly", !deep.Attach(3U, 1U),
                   deep.GetState() == E::HubState::UnsupportedDepth);
    Test::EhciFixture stuck;
    ASTERIA_EXPECT(context, "handoff for reset failure", stuck.Controller.Handoff(0x100U) == E::HandoffStatus::Success);
    stuck.Model.ResetStuck = true;
    ASTERIA_EXPECT(context, "HC reset bounded", !stuck.Controller.Initialize(stuck.Memory, 0x100U, 4U),
                   stuck.Controller.GetState() == E::EhciState::Faulted);
}
void RootAndTimeouts(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    fixture.Model.RootHighSpeed = false;
    Start(context, fixture);
    UInt8 bytes[8U]{};
    const In::UsbRequest reset{.Operation = In::UsbOperation::ResetPort,
                               .Token = 1U,
                               .Speed = In::UsbSpeed::Full,
                               .Address = 0U,
                               .PacketSize0 = 8U,
                               .Setup = {},
                               .Endpoint = {},
                               .Data = bytes,
                               .Length = 0U};
    ASTERIA_EXPECT(context, "root reset starts", fixture.Controller.Submit(1U, 1U, reset));
    fixture.Controller.Poll(50U);
    fixture.Controller.Poll(51U);
    E::EhciCompletion result{};
    ASTERIA_EXPECT(context, "FS root handed to companion", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::Disconnected,
                   (fixture.Model.Read32(Test::FakeEhci::Operational + 0x44U) & (1U << 13U)) != 0U,
                   !fixture.Controller.GetPortStatus(1U).Connected);
    Test::EhciFixture timeout;
    Start(context, timeout);
    ASTERIA_EXPECT(context, "root reset for timeout", timeout.Controller.Submit(1U, 1U, reset));
    timeout.Controller.Poll(50U);
    timeout.Controller.Poll(51U);
    timeout.Controller.Poll(61U);
    ASTERIA_EXPECT(context, "take reset", timeout.Controller.TakeCompletion(1U, result));
    In::UsbRequest control = reset;
    control.Operation = In::UsbOperation::Control;
    control.Speed = In::UsbSpeed::High;
    control.PacketSize0 = 64U;
    control.Length = 8U;
    control.Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, 8U);
    timeout.Model.HoldTransfers = true;
    ASTERIA_EXPECT(context, "pending control accepted", timeout.Controller.Submit(1U, 1U, control));
    timeout.Model.HaltStuck = true;
    timeout.Model.Registers[(Test::FakeEhci::Operational + 4U) / 4U] |= 1U << 4U;
    timeout.Controller.Poll(1061U);
    ASTERIA_EXPECT(context, "failed halt retains borrow", timeout.Controller.GetState() == E::EhciState::Faulted,
                   !timeout.Controller.TakeCompletion(1U, result));
    timeout.Model.HaltStuck = false;
    ASTERIA_EXPECT(context, "explicit halt releases retained request", timeout.Controller.Halt(),
                   timeout.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Retryable);
    Test::EhciFixture disconnect;
    Start(context, disconnect);
    ASTERIA_EXPECT(context, "disconnect pending root reset", disconnect.Controller.Submit(1U, 1U, reset));
    disconnect.Model.Registers[(Test::FakeEhci::Operational + 0x44U) / 4U] = 0U;
    disconnect.Controller.Poll(1U);
    ASTERIA_EXPECT(context, "root disconnect quiesces schedule before completion",
                   disconnect.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Disconnected);
}
void SeededReports(TestContext& context) noexcept
{
    const auto seeds = Asteria::TestSupport::GetSeedRange(0x45484349U, 4U);
    for (UInt64 seed{seeds.First}; seed - seeds.First < seeds.Count; ++seed)
    {
        const Size failures = context.GetFailureCount();
        Test::EhciFixture fixture;
        Start(context, fixture);
        E::UsbHub hub;
        UInt64 now{0U};
        HubEnumerate(context, fixture, hub, now);
        In::UsbDevice device{27U};
        ChildEnumerate(context, fixture, hub, device, 1U, now);
        Asteria::TestSupport::Random random{seed};
        bool held{false};
        fixture.Model.KeyboardReport[0U] = 0U;
        In::EventBatch events{};
        for (Size step{0U}; step < 100U; ++step)
        {
            const bool next = (random.Next() & 1U) != 0U;
            const UInt32 operation = static_cast<UInt32>(random.Next() % 5U);
            fixture.Model.KeyboardReport[2U] = next ? 4U : 0U;
            fixture.Model.FaultNext = operation == 0U ? 0x48U : (operation == 1U ? 0x44U : 0U);
            Report(context, fixture, hub, device, 1U, now, events);
            const bool success = operation >= 2U;
            ASTERIA_EXPECT(context, "seeded pressed-set and error reference", !device.IsOutstanding(),
                           device.GetState() == In::UsbState::Polling,
                           events.Count == (success && (held != next) ? 1U : 0U), fixture.Model.Good);
            if (events.Count == 1U)
            {
                ASTERIA_EXPECT(context, "reference event snapshot", events.Events[0U].Source == 27U,
                               events.Events[0U].Usage == 4U,
                               events.Events[0U].Kind == (next ? In::EventKind::KeyDown : In::EventKind::KeyUp));
            }
            if (success)
            {
                held = next;
            }
            // Avoid an unrelated three-consecutive-errors core failure; retries are
            // completed successfully between each randomly injected fault.
            if (!success)
            {
                fixture.Model.KeyboardReport[2U] = held ? 4U : 0U;
                Report(context, fixture, hub, device, 1U, now, events);
                ASTERIA_EXPECT(context, "retry preserves pressed snapshot", events.Count == 0U, fixture.Model.Good);
            }
            if (context.GetFailureCount() != failures)
            {
                static_cast<void>(std::fprintf(stderr, "EHCI seed=%llu step=%zu operation=%u\n",
                                               static_cast<unsigned long long>(seed), step, operation));
                return;
            }
        }
        ASTERIA_EXPECT(context, "seeded controller halt", fixture.Controller.Halt());
    }
}
void ErrorMappingsAndPages(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    fixture.Memory.Buffer.Address = 0x102F80U; // data crosses a 4KiB page
    fixture.Model.Memory = fixture.Memory;
    Start(context, fixture);
    E::UsbHub hub;
    UInt64 now{0U};
    HubEnumerate(context, fixture, hub, now);
    In::UsbDevice keyboard;
    ChildEnumerate(context, fixture, hub, keyboard, 1U, now);
    UInt8 data[1024U]{};
    In::UsbRequest request{.Operation = In::UsbOperation::Control,
                           .Token = 500U,
                           .Speed = In::UsbSpeed::Full,
                           .Address = 9U,
                           .PacketSize0 = 8U,
                           .Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Configuration, 0U, 1024U),
                           .Endpoint = keyboard.GetInterface().Endpoint,
                           .Data = data,
                           .Length = 1024U,
                           .Route = hub.Route(1U)};
    ASTERIA_EXPECT(context, "short control crosses requested DMA page", fixture.Controller.Submit(2U, 1U, request));
    E::EhciCompletion result{};
    bool controlDone{false};
    for (Size i{0U}; i < 20U && !controlDone; ++i)
    {
        fixture.Controller.Poll(++now);
        controlDone = fixture.Controller.TakeCompletion(2U, result);
    }
    ASTERIA_EXPECT(context, "alternate data link still runs status", controlDone,
                   result.Status == In::TransferStatus::Success, result.ActualLength == 34U, data[0U] == 9U,
                   fixture.Model.Good);
    ++request.Token;
    fixture.Model.FaultNext = 0x48U;
    ASTERIA_EXPECT(context, "split control transaction fault accepted", fixture.Controller.Submit(2U, 1U, request));
    bool recovered{false};
    for (Size i{0U}; i < 20U && !recovered; ++i)
    {
        fixture.Controller.Poll(++now);
        recovered = fixture.Controller.TakeCompletion(2U, result);
    }
    ASTERIA_EXPECT(context, "CLEAR_TT_BUFFER precedes retry completion", recovered,
                   result.Status == In::TransferStatus::Retryable, fixture.Model.TranslatorClears == 1U,
                   fixture.Model.Good);
    request.Operation = In::UsbOperation::InterruptIn;
    request.Length = 8U;
    constexpr UInt32 faults[]{0x40U, 0x50U, 0x48U, 0x60U, 0x44U};
    constexpr In::TransferStatus statuses[]{In::TransferStatus::Stall, In::TransferStatus::Error,
                                            In::TransferStatus::Retryable, In::TransferStatus::Error,
                                            In::TransferStatus::Retryable};
    for (Size fault{0U}; fault < 5U; ++fault)
    {
        fixture.Model.FaultNext = faults[fault];
        ++request.Token;
        ASTERIA_EXPECT(context, "error transfer accepted", fixture.Controller.Submit(2U, 1U, request));
        bool completed{false};
        for (Size i{0U}; i < 20U && !completed; ++i)
        {
            fixture.Controller.Poll(++now);
            completed = fixture.Controller.TakeCompletion(2U, result);
        }
        ASTERIA_EXPECT(context, "controller maps each qTD error", completed, result.Token == request.Token,
                       result.Status == statuses[fault], fixture.Model.Good);
    }
    request.Operation = In::UsbOperation::ConfigureEndpoint;
    request.Length = 0U;
    request.Endpoint.MaxPacketSize |= 0x0800U;
    ++request.Token;
    ASTERIA_EXPECT(context, "FS multiplier rejected", fixture.Controller.Submit(2U, 1U, request),
                   fixture.Controller.TakeCompletion(2U, result), result.Status == In::TransferStatus::Error);
    request.Endpoint.MaxPacketSize &= 0x07FFU;
    request.Operation = In::UsbOperation::InterruptIn;
    request.Length = 8U;
    ++request.Token;
    ASTERIA_EXPECT(context, "schedule-stop test request", fixture.Controller.Submit(2U, 1U, request));
    fixture.Model.ScheduleStuck = true;
    fixture.Controller.Poll(++now);
    for (Size i{0U}; i < 20U && fixture.Controller.GetState() == E::EhciState::Running; ++i)
    {
        fixture.Controller.Poll(++now);
    }
    ASTERIA_EXPECT(context, "schedule stop must be proved before completion",
                   fixture.Controller.GetState() == E::EhciState::Halted, fixture.Controller.TakeCompletion(2U, result),
                   result.Status == In::TransferStatus::Error);
    ASTERIA_EXPECT(context, "error mappings halt", fixture.Controller.Halt());
}
void HighIntervals(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    fixture.Model.HighChild = true;
    fixture.Model.HubStatus[0U] |= 1U << 10U;
    Start(context, fixture);
    E::UsbHub hub;
    UInt64 now{0U};
    HubEnumerate(context, fixture, hub, now);
    In::UsbDevice keyboard;
    ChildEnumerate(context, fixture, hub, keyboard, 1U, now);
    ASTERIA_EXPECT(context, "high-speed child core enumerates", keyboard.GetState() == In::UsbState::Polling);
    UInt8 bytes[8U]{};
    In::UsbRequest request{.Operation = In::UsbOperation::ConfigureEndpoint,
                           .Token = 600U,
                           .Speed = In::UsbSpeed::High,
                           .Address = 9U,
                           .PacketSize0 = 64U,
                           .Setup = {},
                           .Endpoint = keyboard.GetInterface().Endpoint,
                           .Data = bytes,
                           .Length = 0U,
                           .Route = hub.Route(1U)};
    for (UInt8 interval{1U}; interval <= 16U; ++interval)
    {
        request.Operation = In::UsbOperation::ConfigureEndpoint;
        request.Endpoint.Interval = interval;
        request.Length = 0U;
        ++request.Token;
        ASTERIA_EXPECT(context, "configure HS interval", fixture.Controller.Submit(2U, 1U, request));
        E::EhciCompletion result{};
        ASTERIA_EXPECT(context, "configure completed", fixture.Controller.TakeCompletion(2U, result),
                       result.Status == In::TransferStatus::Success);
        request.Operation = In::UsbOperation::InterruptIn;
        request.Length = 8U;
        ++request.Token;
        ASTERIA_EXPECT(context, "HS interrupt accepted", fixture.Controller.Submit(2U, 1U, request));
        const UInt32 microframes = 1U << (interval - 1U);
        Size populated{0U};
        for (Size frame{0U}; frame < 1024U; ++frame)
        {
            populated += (E::Load32(fixture.Frames + frame * 4U) & 1U) == 0U ? 1U : 0U;
        }
        const Size expected = microframes <= 8U ? 1024U : (microframes > 8192U ? 1U : 8192U / microframes);
        const UInt32 mask = interval == 1U ? 0xFFU : (interval == 2U ? 0x55U : (interval == 3U ? 0x11U : 1U));
        ASTERIA_EXPECT(context, "HS frame period and microframe mask", populated == expected,
                       (E::Load32(fixture.Queue + 8U) & 0xFFFFU) == mask);
        if (interval == 15U)
        {
            fixture.Model.HoldTransfers = true;
            fixture.Controller.Poll(++now);
            fixture.Controller.Poll(++now);
            ASTERIA_EXPECT(context, "long-period NAK gates off its one frame",
                           (fixture.Model.Read32(Test::FakeEhci::Operational) & 0x10U) == 0U,
                           fixture.Controller.GetState() == E::EhciState::Running);
            fixture.Model.HoldTransfers = false;
        }
        bool done{false};
        for (Size i{0U}; i < 4200U && !done; ++i)
        {
            fixture.Controller.Poll(++now);
            done = fixture.Controller.TakeCompletion(2U, result);
        }
        ASTERIA_EXPECT(context, "HS report completes", done, result.Status == In::TransferStatus::Success,
                       fixture.Model.Good);
        if (interval >= 15U)
        {
            const UInt32 publishes = fixture.Model.Publishes;
            ++request.Token;
            ASTERIA_EXPECT(context, "long interval request accepted but timer gated",
                           fixture.Controller.Submit(2U, 1U, request), fixture.Model.Publishes == publishes);
            bool again{false};
            for (Size i{0U}; i < 4200U && !again; ++i)
            {
                fixture.Controller.Poll(++now);
                again = fixture.Controller.TakeCompletion(2U, result);
            }
            ASTERIA_EXPECT(context, "long interval waits before next service", again,
                           result.Status == In::TransferStatus::Success, fixture.Model.Good);
        }
    }
    ASTERIA_EXPECT(context, "HS tests halt", fixture.Controller.Halt());
}
void HubFailures(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    Start(context, fixture);
    E::UsbHub hub;
    UInt64 now{0U};
    HubEnumerate(context, fixture, hub, now);
    fixture.Model.HubResetStuck = true;
    ASTERIA_EXPECT(context, "stuck hub reset starts", hub.ResetChild(1U));
    for (Size i{0U}; i < 1100U; ++i)
    {
        HubTick(context, fixture, hub, now);
    }
    ASTERIA_EXPECT(context, "hub reset times out boundedly", hub.GetState() == E::HubState::Failed,
                   !hub.GetPortStatus(1U).ResetComplete, fixture.Model.Good);
    ASTERIA_EXPECT(context, "stuck hub test halt", fixture.Controller.Halt());
    Test::EhciFixture nested;
    Start(context, nested);
    E::UsbHub parent;
    now = 0U;
    HubEnumerate(context, nested, parent, now);
    nested.Model.NestedHub = true;
    In::UsbDevice child;
    ChildEnumerate(context, nested, parent, child, 1U, now);
    ASTERIA_EXPECT(context, "enumerated second hub reports depth limit",
                   child.GetState() == In::UsbState::UnsupportedHubDepth);
    ASTERIA_EXPECT(context, "nested test halt", nested.Controller.Halt());
}
void MultiTranslator(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    fixture.Model.MultiTt = true;
    Start(context, fixture);
    E::UsbHub hub;
    UInt64 now{0U};
    HubEnumerate(context, fixture, hub, now);
    In::UsbDevice mouse;
    ChildEnumerate(context, fixture, hub, mouse, 2U, now);
    ASTERIA_EXPECT(context, "multi-TT topology parsed", hub.Route(2U).MultiTt,
                   mouse.GetState() == In::UsbState::Polling);
    UInt8 data[18U]{};
    const In::UsbRequest request{.Operation = In::UsbOperation::Control,
                                 .Token = 800U,
                                 .Speed = In::UsbSpeed::Full,
                                 .Address = 10U,
                                 .PacketSize0 = 8U,
                                 .Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, 18U),
                                 .Endpoint = {},
                                 .Data = data,
                                 .Length = 18U,
                                 .Route = hub.Route(2U)};
    fixture.Model.FaultNext = 0x48U;
    ASTERIA_EXPECT(context, "multi-TT error request", fixture.Controller.Submit(3U, 1U, request));
    E::EhciCompletion result{};
    bool done{false};
    for (Size i{0U}; i < 20U && !done; ++i)
    {
        fixture.Controller.Poll(++now);
        done = fixture.Controller.TakeCompletion(3U, result);
    }
    ASTERIA_EXPECT(context, "multi-TT clear targets downstream port two", done,
                   result.Status == In::TransferStatus::Retryable, fixture.Model.TranslatorClears == 1U,
                   fixture.Model.Good);
    ASTERIA_EXPECT(context, "multi-TT halt", fixture.Controller.Halt());
}
void Decoder(TestContext& context) noexcept
{
    constexpr UInt32 tokens[]{0U, 0x80U, 0x40U, 0x50U, 0x48U, 0x60U, 0x44U};
    constexpr E::QtdState states[]{
        E::QtdState::Success,          E::QtdState::Active,      E::QtdState::Halted,          E::QtdState::Babble,
        E::QtdState::TransactionError, E::QtdState::BufferError, E::QtdState::MissedMicroframe};
    for (Size i{0U}; i < sizeof(tokens) / sizeof(tokens[0U]); ++i)
    {
        E::QtdResult result{};
        ASTERIA_EXPECT(context, "all EHCI error bits decode", E::DecodeQtd(tokens[i] | (3U << 16U), 8U, result),
                       result.State == states[i], result.ActualLength == 5U);
    }
    E::QtdResult result{.State = E::QtdState::Babble, .ActualLength = 99U};
    ASTERIA_EXPECT(context, "invalid residual/PID/page failure atomic", !E::DecodeQtd(0U, 0x5001U, result),
                   !E::DecodeQtd(9U << 16U, 8U, result), !E::DecodeQtd(3U << 8U, 8U, result),
                   !E::DecodeQtd(5U << 12U, 8U, result), result.State == E::QtdState::Babble,
                   result.ActualLength == 99U);
}
void DeadlineAbort(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    Start(context, fixture);
    UInt8 data[8U]{0xA5U};
    In::UsbRequest request{.Operation = In::UsbOperation::Control,
                           .Token = 1701U,
                           .Speed = In::UsbSpeed::High,
                           .Address = 0U,
                           .PacketSize0 = 64U,
                           .Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, sizeof(data)),
                           .Endpoint = {},
                           .Data = data,
                           .Length = sizeof(data),
                           .Route = {},
                           .TimeoutMilliseconds = 25U};
    E::EhciPort port{fixture.Controller, 1U};
    fixture.Model.HoldTransfers = true;
    fixture.Model.HoldAdvance = true;
    ASTERIA_EXPECT(context, "control timeout accepted", port.Submit(request));
    fixture.Model.Registers[(Test::FakeEhci::Operational + 4U) / 4U] |= 1U << 5U;
    fixture.Controller.Poll(25U);
    E::EhciCompletion result{};
    ASTERIA_EXPECT(context, "timeout unlinks QH and rings async advance", fixture.Model.Doorbells == 1U,
                   !fixture.Controller.TakeCompletion(1U, result),
                   E::Load32(fixture.Head) == (static_cast<UInt32>(fixture.Memory.AsyncHead.Address) | 2U),
                   !port.Abort(request.Token), !port.Submit(request));
    fixture.Controller.Poll(26U);
    ASTERIA_EXPECT(context, "unacknowledged doorbell retains request and buffer",
                   !fixture.Controller.TakeCompletion(1U, result), data[0U] == 0xA5U,
                   fixture.Controller.GetState() == E::EhciState::Running);
    fixture.Model.HoldAdvance = false;
    fixture.Controller.Poll(27U);
    ASTERIA_EXPECT(context, "IAA permits one TimedOut completion", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::TimedOut, fixture.Model.Advances == 1U,
                   !fixture.Controller.TakeCompletion(1U, result), data[0U] == 0xA5U);
    fixture.Model.HoldTransfers = false;
    ++request.Token;
    ASTERIA_EXPECT(context, "next enumeration descriptor accepted", port.Submit(request));
    fixture.Controller.Poll(28U);
    ASTERIA_EXPECT(context, "next descriptor succeeds after abort", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::Success, data[0U] == 18U, fixture.Model.Good,
                   fixture.Controller.GetState() == E::EhciState::Running);
    fixture.Model.HoldTransfers = true;
    fixture.Model.HoldAdvance = true;
    ++request.Token;
    ASTERIA_EXPECT(context, "explicit async abort request", port.Submit(request));
    fixture.Model.Registers[(Test::FakeEhci::Operational + 4U) / 4U] |= 1U << 5U;
    ASTERIA_EXPECT(context, "explicit async abort ignores stale IAA", port.Abort(request.Token));
    fixture.Controller.Poll(29U);
    ASTERIA_EXPECT(context, "stale IAA cannot release cancelled QH", !fixture.Controller.TakeCompletion(1U, result));
    fixture.Model.HoldAdvance = false;
    fixture.Controller.Poll(30U);
    ASTERIA_EXPECT(context, "new IAA releases explicit async cancellation",
                   fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Cancelled,
                   fixture.Model.Advances == 2U);
    request.Operation = In::UsbOperation::ConfigureEndpoint;
    request.Endpoint = {0x81U, 3U, 8U, 4U, 0U, 0U, 0U, false};
    ASTERIA_EXPECT(context, "configure periodic abort pipe", port.Submit(request),
                   fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Success);
    request.Operation = In::UsbOperation::InterruptIn;
    request.TimeoutMilliseconds = 0U;
    ++request.Token;
    fixture.Model.HoldTransfers = true;
    ASTERIA_EXPECT(context, "periodic request pending", port.Submit(request), !port.Abort(request.Token + 1U),
                   !fixture.Controller.Abort(2U, request.Token));
    fixture.Model.FreezeFrames = true;
    ASTERIA_EXPECT(context, "explicit pipe abort accepted", port.Abort(request.Token));
    fixture.Controller.Poll(31U);
    ASTERIA_EXPECT(context, "periodic frame boundary required", !fixture.Controller.TakeCompletion(1U, result),
                   !port.Abort(request.Token));
    fixture.Model.FreezeFrames = false;
    fixture.Controller.Poll(32U);
    fixture.Controller.Poll(33U);
    ASTERIA_EXPECT(context, "frame retirement reports Cancelled once", fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::Cancelled,
                   fixture.Controller.GetState() == E::EhciState::Running, fixture.Model.Good,
                   !fixture.Controller.TakeCompletion(1U, result));
    fixture.Model.Registers[(Test::FakeEhci::Operational + 4U) / 4U] |= 1U << 4U;
    fixture.Controller.Poll(34U);
    ASTERIA_EXPECT(context, "host system error still halts", fixture.Controller.GetState() == E::EhciState::Halted);
}

void TranslatorTimeout(TestContext& context) noexcept
{
    for (UInt32 mode{0U}; mode < 3U; ++mode)
    {
        Test::EhciFixture fixture;
        Start(context, fixture);
        E::UsbHub hub;
        UInt64 now{0U};
        HubEnumerate(context, fixture, hub, now);
        In::UsbDevice child;
        ChildEnumerate(context, fixture, hub, child, 1U, now);
        UInt8 data[8U]{0xA5U};
        In::UsbRequest request{.Operation = In::UsbOperation::Control,
                               .Token = 1801U,
                               .Speed = In::UsbSpeed::Full,
                               .Address = 9U,
                               .PacketSize0 = 8U,
                               .Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, sizeof(data)),
                               .Endpoint = {},
                               .Data = data,
                               .Length = sizeof(data),
                               .Route = hub.Route(1U),
                               .TimeoutMilliseconds = 25U};
        E::EhciPort port{fixture.Controller, 2U};
        fixture.Model.HoldTransfers = true;
        ASTERIA_EXPECT(context, "split control deadline accepted", port.Submit(request));
        if (mode == 0U)
        {
            fixture.Controller.Poll(now += 25U);
        }
        else
        {
            ASTERIA_EXPECT(context, "routed explicit abort accepted", port.Abort(request.Token));
        }
        fixture.Controller.Poll(++now); // IAA, then internal CLEAR_TT_BUFFER
        E::EhciCompletion result{};
        ASTERIA_EXPECT(context, "TT cleanup retains original borrow", !fixture.Controller.TakeCompletion(2U, result));
        if (mode == 2U)
        {
            fixture.Model.HoldTransfers = false;
            fixture.Model.FaultNext = 0x40U; // cleanup hub stalls
            fixture.Controller.Poll(++now);
        }
        else
        {
            fixture.Controller.Poll(now += 1000U); // TT control itself times out
            fixture.Controller.Poll(++now);        // its IAA
        }
        ASTERIA_EXPECT(context, "USB TT recovery timeout also retires request without halting",
                       fixture.Controller.TakeCompletion(2U, result), result.Token == request.Token,
                       result.Status == (mode == 0U ? In::TransferStatus::TimedOut : In::TransferStatus::Cancelled),
                       data[0U] == 0xA5U, fixture.Controller.GetState() == E::EhciState::Running, fixture.Model.Good);
        fixture.Model.HoldTransfers = false;
        ++request.Token;
        ASTERIA_EXPECT(context, "next routed descriptor accepted", port.Submit(request));
        for (Size i{0U}; i < 20U; ++i)
        {
            fixture.Controller.Poll(++now);
            if (fixture.Controller.TakeCompletion(2U, result))
            {
                break;
            }
        }
        ASTERIA_EXPECT(context, "next routed descriptor succeeds", result.Token == request.Token,
                       result.Status == In::TransferStatus::Success, data[0U] == 18U, fixture.Model.Good,
                       fixture.Controller.Halt());
    }
}
// A periodic report the controller retired while software looked away is
// delivered with its data when the request is aborted (mode 0) or times out
// (mode 1), and the qTD's toggle carries to the next report.
void WonReport(TestContext& context) noexcept
{
    for (UInt32 mode{0U}; mode < 2U; ++mode)
    {
        Test::EhciFixture fixture;
        Start(context, fixture);
        E::UsbHub hub;
        UInt64 now{0U};
        HubEnumerate(context, fixture, hub, now);
        In::UsbDevice keyboard;
        ChildEnumerate(context, fixture, hub, keyboard, 1U, now);
        UInt8 data[8U]{0xA5U};
        In::UsbRequest request{.Operation = In::UsbOperation::InterruptIn,
                               .Token = 2000U,
                               .Speed = In::UsbSpeed::Full,
                               .Address = 9U,
                               .PacketSize0 = 8U,
                               .Setup = {},
                               .Endpoint = keyboard.GetInterface().Endpoint,
                               .Data = data,
                               .Length = sizeof(data),
                               .Route = hub.Route(1U),
                               .TimeoutMilliseconds = (mode == 0U) ? 0U : 5U};
        const UInt64 submitted = now;
        ASTERIA_EXPECT(context, "won report request accepted", fixture.Controller.Submit(2U, 1U, request));
        // The controller runs its periodic schedule unobserved by software.
        for (Size i{0U}; (i < 64U) && ((E::Load32(fixture.Descriptors + 8U) & 0x80U) != 0U); ++i)
        {
            fixture.Model.ObserveDma();
        }
        const bool retired = (E::Load32(fixture.Descriptors + 8U) & 0x80U) == 0U;
        const bool stopped = (mode != 0U) || fixture.Controller.Abort(2U, request.Token);
        if (mode != 0U)
        {
            now = submitted + request.TimeoutMilliseconds;
            fixture.Controller.Poll(now);
        }
        E::EhciCompletion result{};
        bool done{false};
        for (Size i{0U}; (i < 20U) && !done; ++i)
        {
            fixture.Controller.Poll(++now);
            done = fixture.Controller.TakeCompletion(2U, result);
        }
        ASTERIA_EXPECT(context, "a report retired before the unlink is delivered with its data", retired, stopped, done,
                       result.Token == request.Token, result.Status == In::TransferStatus::Success,
                       result.ActualLength == 8U, std::memcmp(data, fixture.Model.KeyboardReport, sizeof(data)) == 0);
        ++request.Token;
        request.TimeoutMilliseconds = 0U;
        ASTERIA_EXPECT(context, "next report after the won one accepted", fixture.Controller.Submit(2U, 1U, request));
        done = false;
        for (Size i{0U}; (i < 20U) && !done; ++i)
        {
            fixture.Controller.Poll(++now);
            done = fixture.Controller.TakeCompletion(2U, result);
        }
        ASTERIA_EXPECT(context, "the next report follows the won report's toggle", done,
                       result.Status == In::TransferStatus::Success, fixture.Model.Good, fixture.Controller.Halt());
    }
}
// USB 2.0 section 7.1.7.5: a root port reset returns every endpoint below
// it to DATA0, so every logical pipe last used on that root port must be
// configured again, whichever pipe sent the reset; a pipe of another root
// port keeps its endpoint and its DATA toggle. The model routes by hub
// address alone, so pipe 4 reaches the keyboard through "root port 2" too.
void RootResetUnconfigures(TestContext& context) noexcept
{
    Test::EhciFixture fixture;
    fixture.Model.Registers[1U] = 0x12U; // two root ports, the second connected
    fixture.Model.Registers[(Test::FakeEhci::Operational + 0x48U) / 4U] = 1U;
    Start(context, fixture);
    E::UsbHub hub;
    UInt64 now{0U};
    HubEnumerate(context, fixture, hub, now);
    In::UsbDevice keyboard;
    ChildEnumerate(context, fixture, hub, keyboard, 1U, now);
    UInt8 data[8U]{};
    In::UsbRequest request{.Operation = In::UsbOperation::ConfigureEndpoint,
                           .Token = 1900U,
                           .Speed = In::UsbSpeed::Full,
                           .Address = 9U,
                           .PacketSize0 = 8U,
                           .Setup = {},
                           .Endpoint = keyboard.GetInterface().Endpoint,
                           .Data = data,
                           .Length = 0U,
                           .Route = hub.Route(1U)};
    E::EhciCompletion result{};
    // Submits request on pipe through root and polls it to its completion.
    const auto run = [&](const In::UsbRequest& submitted, const UInt8 pipe, const UInt8 root) noexcept
    {
        if (!fixture.Controller.Submit(pipe, root, submitted))
        {
            return false;
        }
        for (Size i{0U}; i < 100U; ++i)
        {
            if (fixture.Controller.TakeCompletion(pipe, result))
            {
                return (result.Token == submitted.Token) && (result.Status == In::TransferStatus::Success);
            }
            fixture.Controller.Poll(++now);
        }
        return false;
    };
    const bool configured = run(request, 3U, 1U) && run(request, 4U, 2U);
    request.Operation = In::UsbOperation::InterruptIn;
    request.Length = sizeof(data);
    ++request.Token;
    const bool reported = run(request, 4U, 2U) && fixture.Model.Good;
    In::UsbRequest reset = request;
    reset.Operation = In::UsbOperation::ResetPort;
    reset.Data = nullptr;
    reset.Length = 0U;
    reset.Route = {};
    ++reset.Token;
    ASTERIA_EXPECT(context, "pipe 4 at DATA1 on root port 2, then root port 1 reset through pipe 2", configured,
                   reported, run(reset, 2U, 1U));
    fixture.Model.HoldTransfers = true;
    ++request.Token;
    const bool unconfigured = fixture.Controller.Submit(3U, 1U, request) &&
                              fixture.Controller.TakeCompletion(3U, result) &&
                              (result.Status == In::TransferStatus::Error);
    ++request.Token;
    const bool accepted = fixture.Controller.Submit(4U, 2U, request);
    const bool busy = fixture.Controller.IsBusy() && !fixture.Controller.TakeCompletion(4U, result);
    const bool toggle = (E::Load32(fixture.Descriptors + 8U) >> 31U) == 1U;
    const bool aborted = fixture.Controller.Abort(4U, request.Token);
    bool done{false};
    for (Size i{0U}; (i < 20U) && !done; ++i)
    {
        fixture.Controller.Poll(++now);
        done = fixture.Controller.TakeCompletion(4U, result);
    }
    ASTERIA_EXPECT(context, "the reset unconfigures the pipes of its root port only", unconfigured, accepted, busy,
                   toggle, aborted, done, result.Status == In::TransferStatus::Cancelled, !fixture.Controller.IsBusy(),
                   fixture.Model.Good, fixture.Controller.Halt());
}

} // namespace
int main()
{
    TestContext context{"EHCI"};
    WonReport(context);
    RootResetUnconfigures(context);
    HandoffAndBounds(context);
    TranslatorTimeout(context);
    DeadlineAbort(context);
    RootAndTimeouts(context);
    EndToEnd(context, 0x100000U, false, false);
    EndToEnd(context, 0x100100000U, true, true);
    SeededReports(context);
    ErrorMappingsAndPages(context);
    HighIntervals(context);
    HubFailures(context);
    MultiTranslator(context);
    Decoder(context);
    return context.GetFailureCount() == 0U ? 0 : 1;
}
