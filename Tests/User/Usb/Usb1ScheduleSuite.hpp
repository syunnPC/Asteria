// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_USB1SCHEDULESUITE_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_USB1SCHEDULESUITE_HPP_INCLUDED

#include "User/Usb/FakeUsb1.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Test::Usb
{
using TestSupport::TestContext;
template <class T> void Tick(T& fixture, UInt64& now) noexcept
{
    fixture.Model.Tick();
    fixture.Controller.Poll(++now);
}
template <class T>
[[nodiscard]] U::Usb1Completion Complete(TestContext& context, T& fixture, const UInt8 port, UInt64& now,
                                         const UInt32 limit = 1200U) noexcept
{
    U::Usb1Completion result{};
    for (UInt32 i{0U}; i < limit; ++i)
    {
        Tick(fixture, now);
        if (fixture.Controller.TakeCompletion(port, result))
        {
            return result;
        }
    }
    ASTERIA_EXPECT(context, "accepted operation reaches a finite completion", false);
    return {.Token = 0U, .Status = In::TransferStatus::Error, .ActualLength = 0U};
}
[[nodiscard]] inline In::UsbRequest Request(const In::UsbOperation operation, const UInt64 token,
                                            UInt8* const data = nullptr, const Size length = 0U) noexcept
{
    return {.Operation = operation,
            .Token = token,
            .Speed = In::UsbSpeed::Full,
            .Address = 7U,
            .PacketSize0 = 8U,
            .Setup = {},
            .Endpoint = {0x81U, 3U, 8U, 10U, 0U, 0U, 0U, false},
            .Data = data,
            .Length = length};
}
template <class T>
void Enumerate(TestContext& context, T& fixture, const UInt8 port, In::UsbDevice& device, UInt64& now) noexcept
{
    U::Usb1Port view{fixture.Controller, port};
    ASTERIA_EXPECT(context, "root device attaches", device.Attach(static_cast<UInt8>(port + 6U)));
    In::EventBatch output{};
    for (Size i{0U}; (i < 800U) && (device.GetState() != In::UsbState::Polling); ++i)
    {
        device.Step(view);
        Tick(fixture, now);
        U::Usb1Completion result{};
        if (fixture.Controller.TakeCompletion(port, result))
        {
            ASTERIA_EXPECT(context, "USB core accepts real schedule completion",
                           device.Complete(result.Token, result.Status, result.ActualLength, output));
        }
    }
    ASTERIA_EXPECT(context, "synthetic descriptors enumerate boot endpoint through real core",
                   device.GetState() == In::UsbState::Polling, fixture.Model.Good,
                   fixture.Model.Devices[port - 1U].Good, fixture.Model.Devices[port - 1U].Address == UInt32{port + 6U},
                   fixture.Model.Devices[port - 1U].Setups == fixture.Model.Devices[port - 1U].Statuses);
}
template <class T> void EndToEnd(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "handoff and DMA start", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice keyboard{10U};
    In::UsbDevice mouse{11U};
    Enumerate(context, fixture, 1U, keyboard, now);
    Enumerate(context, fixture, 2U, mouse, now);
    In::EventBatch output{};
    U::Usb1Port keyboardPort{fixture.Controller, 1U};
    keyboard.Step(keyboardPort);
    const auto key = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "keyboard transfer returns fixture HID usage",
                   keyboard.Complete(key.Token, key.Status, key.ActualLength, output), output.Count == 2U);
    U::Usb1Port mousePort{fixture.Controller, 2U};
    mouse.Step(mousePort);
    const auto motion = Complete(context, fixture, 2U, now);
    ASTERIA_EXPECT(context, "mouse transfer delivers fixture motion and button",
                   mouse.Complete(motion.Token, motion.Status, motion.ActualLength, output), output.Count == 2U,
                   fixture.Model.Good, fixture.Model.Devices[0U].Good, fixture.Model.Devices[1U].Good,
                   fixture.Controller.Halt(), fixture.Controller.GetState() == U::Usb1State::Halted);
}
template <class T> void DeadlinesAndAbort(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "deadline fixture starts", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice device{};
    Enumerate(context, fixture, 1U, device, now);
    U::Usb1Port port{fixture.Controller, 1U};
    fixture.Model.Devices[0U].Fault = WireFault::Nak;
    device.Step(port);
    U::Usb1Completion result{};
    for (Size i{0U}; i < 6000U; ++i)
    {
        Tick(fixture, now);
        ASTERIA_EXPECT(context, "idle NAK keeps one borrow without failure",
                       !fixture.Controller.TakeCompletion(1U, result), device.IsOutstanding(),
                       device.GetState() == In::UsbState::Polling,
                       fixture.Controller.GetState() == U::Usb1State::Running);
    }
    fixture.Model.Devices[0U].Fault = WireFault::None;
    result = Complete(context, fixture, 1U, now);
    In::EventBatch output{};
    ASTERIA_EXPECT(context, "next real HID report survives long idle",
                   device.Complete(result.Token, result.Status, result.ActualLength, output), output.Count == 2U,
                   device.GetState() == In::UsbState::Polling);
    fixture.Model.Devices[0U].Fault = WireFault::Nak;
    fixture.Model.AsyncHold = true;
    device.Step(port);
    for (Size i{0U}; i < 12U; ++i)
    {
        Tick(fixture, now);
    }
    const UInt64 abortedAt = now;
    ASTERIA_EXPECT(context, "core explicitly cancels held poll", fixture.Model.PendingTd != 0U, device.Abort(port));
    ASTERIA_EXPECT(context, "abort retains borrow through retirement", device.IsOutstanding(),
                   !fixture.Controller.TakeCompletion(1U, result), !device.Abort(port));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "cancel retires packet once without failing core",
                   result.Status == In::TransferStatus::Cancelled, now - abortedAt >= T::RetirementFrames,
                   fixture.Model.PendingTd == 0U, fixture.Model.Canceled == 1U,
                   device.Complete(result.Token, result.Status, 0U, output), device.GetState() == In::UsbState::Polling,
                   !fixture.Controller.TakeCompletion(1U, result));
    fixture.Model.AsyncHold = false;
    fixture.Model.Devices[0U].Fault = WireFault::None;
    UInt8 data[8U]{0xA5U};
    auto control = Request(In::UsbOperation::Control, 1301U, data, sizeof(data));
    control.Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, sizeof(data));
    control.TimeoutMilliseconds = 25U;
    fixture.Model.AsyncHold = true;
    fixture.Model.Devices[0U].HoldControl = true;
    ASTERIA_EXPECT(context, "finite control accepted", port.Submit(control), !port.Abort(control.Token + 1U),
                   !fixture.Controller.Abort(2U, control.Token));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "control deadline retires only request", result.Token == control.Token,
                   result.Status == In::TransferStatus::TimedOut, data[0U] == 0xA5U, fixture.Model.PendingTd == 0U,
                   fixture.Model.Canceled == 2U, fixture.Controller.GetState() == U::Usb1State::Running,
                   !port.Abort(control.Token));
    fixture.Model.Devices[0U].HoldControl = false;
    ++control.Token;
    ASTERIA_EXPECT(context, "next descriptor enumeration step accepted", port.Submit(control));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "next descriptor succeeds after timeout", result.Status == In::TransferStatus::Success,
                   result.ActualLength == 8U, data[0U] == 18U, fixture.Model.Good);
    fixture.Model.HostError();
    Tick(fixture, now);
    ASTERIA_EXPECT(context, "host system error still stops controller",
                   fixture.Controller.GetState() == U::Usb1State::Faulted, fixture.Controller.Halt());
}
template <class T> void Boundaries(TestContext& context) noexcept
{
    T fixture{};
    fixture.Model.Devices[0U].Low = true;
    ASTERIA_EXPECT(context, "low speed start", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice keyboard{};
    Enumerate(context, fixture, 1U, keyboard, now);
    UInt8 data[1024U]{};
    auto request = Request(In::UsbOperation::Control, 77U, data, 64U);
    request.Speed = In::UsbSpeed::Low;
    request.Setup = In::UsbDescriptors::GetDescriptor(In::DescriptorKind::Device, 0U, 64U);
    ASTERIA_EXPECT(context, "short control accepted", fixture.Controller.Submit(1U, request));
    auto result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "short control completes only after status stage", result.Token == 77U,
                   result.Status == In::TransferStatus::Success, result.ActualLength == 18U, data[0U] == 18U,
                   fixture.Model.Devices[0U].Setups == fixture.Model.Devices[0U].Statuses);
    request.Token = 78U;
    request.Length = 1024U;
    request.Setup = {.Bytes = {0x40U, 0x55U, 0U, 0U, 0U, 0U, 0U, 4U}};
    ASTERIA_EXPECT(context, "maximal control OUT accepted", fixture.Controller.Submit(1U, request));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "128 packets plus setup and status complete", result.Token == 78U,
                   result.Status == In::TransferStatus::Success, result.ActualLength == 1024U,
                   fixture.Model.Devices[0U].Good, fixture.Model.Good);
    request.Token = 79U;
    request.Length = 1025U;
    ASTERIA_EXPECT(context, "invalid caller length gets conservative completion",
                   fixture.Controller.Submit(1U, request), fixture.Controller.TakeCompletion(1U, result),
                   result.Status == In::TransferStatus::Error);
    request.Operation = static_cast<In::UsbOperation>(255U);
    request.Length = 0U;
    ASTERIA_EXPECT(context, "invalid operation rejected", fixture.Controller.Submit(1U, request),
                   fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Error);
    request.Operation = In::UsbOperation::Control;
    request.Route.Depth = 1U;
    ASTERIA_EXPECT(context, "hub routing explicitly unsupported", fixture.Controller.Submit(1U, request),
                   fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Error);
    ASTERIA_EXPECT(context, "invalid port and empty completion do not retain", !fixture.Controller.Submit(0U, request),
                   !fixture.Controller.TakeCompletion(1U, result), fixture.Controller.Halt());
}
template <class T> void RandomSchedules(TestContext& context) noexcept
{
    const auto seeds = TestSupport::GetSeedRange(0x023500U, 16U);
    for (UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        TestSupport::Random random{seed};
        T fixture{};
        ASTERIA_EXPECT(context, "random schedule starts", fixture.Start());
        UInt64 now{0U};
        In::UsbDevice device{};
        Enumerate(context, fixture, 1U, device, now);
        UInt8 buffer[8U]{};
        bool expectedToggle{false};
        for (UInt64 step{0U}; step < 40U; ++step)
        {
            const UInt64 operation = random.Below(8U);
            constexpr WireFault faults[]{WireFault::None,  WireFault::Crc,    WireFault::Babble, WireFault::Buffer,
                                         WireFault::Stuff, WireFault::Toggle, WireFault::Stall,  WireFault::Nak};
            constexpr In::TransferStatus statuses[]{In::TransferStatus::Success,   In::TransferStatus::Retryable,
                                                    In::TransferStatus::Error,     In::TransferStatus::Error,
                                                    In::TransferStatus::Retryable, In::TransferStatus::Retryable,
                                                    In::TransferStatus::Stall,     In::TransferStatus::Success};
            auto configure = Request(In::UsbOperation::ConfigureEndpoint, 100U + step * 3U);
            configure.Endpoint.Interval = static_cast<UInt8>(1U + random.Below(255U));
            // Reconfigure represents a reset pipe, which starts DATA0.
            fixture.Model.Devices[0U].ReportToggle = false;
            expectedToggle = false;
            U::Usb1Completion result{};
            ASTERIA_EXPECT(context, "configure one endpoint", fixture.Controller.Submit(1U, configure),
                           fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Success);
            auto report = Request(In::UsbOperation::InterruptIn, configure.Token + 1U, buffer, sizeof(buffer));
            report.Endpoint = configure.Endpoint;
            fixture.Model.Devices[0U].Fault = faults[operation];
            const UInt32 before = fixture.Model.Devices[0U].Attempts;
            const UInt32 reportsBefore = fixture.Model.Devices[0U].Reports;
            ASTERIA_EXPECT(context, "report accepts one outstanding borrow", fixture.Controller.Submit(1U, report),
                           !fixture.Controller.Submit(2U, report));
            if (operation == 7U)
            {
                UInt64 last{0U};
                UInt32 attempts = before;
                for (Size ticks{0U}; ticks < 600U; ++ticks)
                {
                    Tick(fixture, now);
                    const UInt64 attempt = fixture.Model.Devices[0U].LastAttempt;
                    if (fixture.Model.Devices[0U].Attempts != attempts)
                    {
                        attempts = fixture.Model.Devices[0U].Attempts;
                        if (last != 0U)
                        {
                            ASTERIA_EXPECT(context, "NAK retry respects full 1..255 interval",
                                           attempt - last == report.Endpoint.Interval);
                        }
                        last = attempt;
                    }
                }
                fixture.Model.Devices[0U].Fault = WireFault::None;
            }
            result = Complete(context, fixture, 1U, now);
            if ((result.Token != report.Token) || (result.Status != statuses[operation]))
            {
                static_cast<void>(
                    std::fprintf(stderr, "seed=%llu step=%llu operation=%llu\n", static_cast<unsigned long long>(seed),
                                 static_cast<unsigned long long>(step), static_cast<unsigned long long>(operation)));
            }
            if (operation == 0U || operation == 7U)
            {
                expectedToggle = !expectedToggle;
            }
            ASTERIA_EXPECT(context, "seeded reference result, resource count and toggle agree",
                           result.Token == report.Token, result.Status == statuses[operation],
                           result.ActualLength == (statuses[operation] == In::TransferStatus::Success ? 8U : 0U),
                           fixture.Model.Devices[0U].Attempts > before,
                           fixture.Model.Devices[0U].Reports == reportsBefore + (expectedToggle ? 1U : 0U),
                           fixture.Model.Devices[0U].ReportToggle == expectedToggle,
                           !fixture.Controller.TakeCompletion(1U, result), fixture.Model.Good,
                           fixture.Model.Devices[0U].Good);
            fixture.Model.Devices[0U].Fault = WireFault::None;
            if (operation == 6U)
            {
                auto clear = Request(In::UsbOperation::Control, configure.Token + 2U);
                clear.Setup = {.Bytes = {2U, 1U, 0U, 0U, 0x81U, 0U, 0U, 0U}};
                ASTERIA_EXPECT(context, "CLEAR_FEATURE recovers device halt", fixture.Controller.Submit(1U, clear));
                result = Complete(context, fixture, 1U, now);
                ASTERIA_EXPECT(context, "halt recovery succeeds", result.Status == In::TransferStatus::Success,
                               !fixture.Model.Devices[0U].Halted);
            }
        }
        ASTERIA_EXPECT(context, "random schedule halt", fixture.Controller.Halt());
    }
}
template <class T> void Disconnects(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "disconnect fixture starts", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice device{};
    Enumerate(context, fixture, 1U, device, now);
    UInt8 buffer[8U]{0xA5U};
    auto report = Request(In::UsbOperation::InterruptIn, 501U, buffer, sizeof(buffer));
    fixture.Model.Devices[0U].Fault = WireFault::Nak;
    ASTERIA_EXPECT(context, "pending disconnect request", fixture.Controller.Submit(1U, report));
    Tick(fixture, now);
    fixture.Model.Devices[0U].Connected = false;
    static_cast<void>(fixture.Controller.GetPortStatus(1U));
    auto result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "disconnect revokes borrow after quiescence", result.Token == 501U,
                   result.Status == In::TransferStatus::Disconnected, result.ActualLength == 0U, buffer[0U] == 0xA5U,
                   !fixture.Controller.GetPortStatus(1U).Connected,
                   fixture.Controller.GetState() == U::Usb1State::Running);
    fixture.Model.Devices[0U].Connected = true;
    fixture.Model.Devices[0U].Fault = WireFault::None;
    device.Disconnect();
    Enumerate(context, fixture, 1U, device, now);
    fixture.Model.Devices[0U].Fault = WireFault::Nak;
    report.TimeoutMilliseconds = 1000U;
    ASTERIA_EXPECT(context, "timeout accepts report", fixture.Controller.Submit(1U, report));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "selected NAK deadline times out once", result.Status == In::TransferStatus::TimedOut,
                   !fixture.Controller.TakeCompletion(1U, result));
    ASTERIA_EXPECT(context, "halt fixture", fixture.Controller.Halt());
}
template <class T> void Recovery(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "recovery fixture starts", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice device{};
    Enumerate(context, fixture, 1U, device, now);
    UInt8 data[8U]{};
    auto report = Request(In::UsbOperation::InterruptIn, 900U, data, sizeof(data));
    ASTERIA_EXPECT(context, "first report accepts", fixture.Controller.Submit(1U, report));
    auto result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "success moves pipe to DATA1", result.Status == In::TransferStatus::Success,
                   fixture.Model.Devices[0U].ReportToggle);
    fixture.Model.Devices[0U].Fault = WireFault::Stall;
    ++report.Token;
    ASTERIA_EXPECT(context, "stalled endpoint accepts", fixture.Controller.Submit(1U, report));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "stall retains carry", result.Status == In::TransferStatus::Stall,
                   fixture.Model.Devices[0U].ReportToggle);
    auto clear = Request(In::UsbOperation::Control, 902U);
    clear.Setup = {.Bytes = {2U, 1U, 0U, 0U, 0x81U, 0U, 0U, 0U}};
    fixture.Model.Devices[0U].Fault = WireFault::None;
    ASTERIA_EXPECT(context, "device halt clear accepted", fixture.Controller.Submit(1U, clear));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "device clear resets toggle", result.Status == In::TransferStatus::Success,
                   !fixture.Model.Devices[0U].ReportToggle);
    report.Token = 903U;
    ASTERIA_EXPECT(context, "halted schedule reused after recovery", fixture.Controller.Submit(1U, report));
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "hardware and device toggle agree after recovery",
                   result.Status == In::TransferStatus::Success, fixture.Model.Devices[0U].Good, fixture.Model.Good);
    fixture.Model.Devices[0U].Fault = WireFault::Nak;
    report.Token = 904U;
    ASTERIA_EXPECT(context, "halt with outstanding request", fixture.Controller.Submit(1U, report));
    ASTERIA_EXPECT(context, "halt revokes borrow exactly once", fixture.Controller.Halt(),
                   fixture.Controller.TakeCompletion(1U, result), result.Token == 904U,
                   result.Status == In::TransferStatus::Retryable, !fixture.Controller.TakeCompletion(1U, result),
                   !fixture.Controller.Submit(1U, report));
}
template <class T> void ResetAndFrameTimeout(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "port timeout fixture starts", fixture.Start());
    fixture.Model.HoldPortReset = true;
    UInt64 now{0U};
    auto reset = Request(In::UsbOperation::ResetPort, 1001U);
    ASTERIA_EXPECT(context, "stuck port reset accepted", fixture.Controller.Submit(1U, reset));
    const auto result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "port reset timeout remains retryable", result.Token == 1001U,
                   result.Status == In::TransferStatus::Retryable, now <= 1002U, fixture.Controller.Halt());
    T frozen{};
    ASTERIA_EXPECT(context, "frame timeout fixture starts", frozen.Start());
    now = 0U;
    In::UsbDevice device{};
    Enumerate(context, frozen, 1U, device, now);
    UInt8 data[8U]{0xA5U};
    auto report = Request(In::UsbOperation::InterruptIn, 1002U, data, sizeof(data));
    report.TimeoutMilliseconds = 25U;
    ASTERIA_EXPECT(context, "frozen controller request accepted", frozen.Controller.Submit(1U, report));
    frozen.Model.Freeze = true;
    const auto failure = Complete(context, frozen, 1U, now);
    ASTERIA_EXPECT(context, "missing SOF quiescence uses bounded halt/reset", failure.Token == 1002U,
                   failure.Status == In::TransferStatus::Error, data[0U] == 0xA5U,
                   frozen.Controller.GetState() == U::Usb1State::Halted, frozen.Controller.Halt());
}

template <class T> void Intervals(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "interval fixture starts", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice device{};
    Enumerate(context, fixture, 1U, device, now);
    constexpr UInt8 intervals[]{1U, 2U, 10U, 32U, 33U, 128U, 255U};
    UInt64 token{1100U};
    for (const UInt8 interval : intervals)
    {
        auto configure = Request(In::UsbOperation::ConfigureEndpoint, ++token);
        configure.Endpoint.Interval = interval;
        U::Usb1Completion result{};
        ASTERIA_EXPECT(context, "explicit interval configured", fixture.Controller.Submit(1U, configure),
                       fixture.Controller.TakeCompletion(1U, result), result.Status == In::TransferStatus::Success);
        fixture.Model.Devices[0U].ReportToggle = false;
        fixture.Model.Devices[0U].Fault = WireFault::Nak;
        UInt8 data[8U]{};
        auto report = Request(In::UsbOperation::InterruptIn, ++token, data, sizeof(data));
        report.Endpoint = configure.Endpoint;
        ASTERIA_EXPECT(context, "interval request accepted", fixture.Controller.Submit(1U, report));
        UInt32 attempts = fixture.Model.Devices[0U].Attempts;
        UInt64 previous{0U};
        for (Size ticks{0U}; ticks < 600U; ++ticks)
        {
            Tick(fixture, now);
            if (attempts != fixture.Model.Devices[0U].Attempts)
            {
                attempts = fixture.Model.Devices[0U].Attempts;
                const UInt64 frame = fixture.Model.Devices[0U].LastAttempt;
                if (previous != 0U)
                {
                    ASTERIA_EXPECT(context, "exact interval survives schedule table wrap",
                                   frame - previous == interval);
                }
                previous = frame;
            }
        }
        fixture.Model.Devices[0U].Fault = WireFault::None;
        result = Complete(context, fixture, 1U, now);
        ASTERIA_EXPECT(context, "NAK interval resumes and completes once", result.Status == In::TransferStatus::Success,
                       result.Token == token, result.ActualLength == 8U, fixture.Model.Devices[0U].Good,
                       fixture.Model.Good);
    }
    ASTERIA_EXPECT(context, "interval halt", fixture.Controller.Halt());
}

// Logical pipes (Usb1.hpp): the composite device on root port 1 (address 7)
// polls endpoint 0x81 on pipe 3 and 0x82 on pipe 4, the mouse on port 2
// (address 8) its 0x82 on pipe 5. Each pipe keeps its own DATA toggle and
// completion; CLEAR_FEATURE(ENDPOINT_HALT) on the control pipe returns only
// the pipe polling that endpoint of that device to DATA0; a disconnect
// unconfigures every pipe of its port and no other.
template <class T> void LogicalPipes(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "logical pipe fixture starts", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice keyboard{};
    In::UsbDevice mouse{};
    Enumerate(context, fixture, 1U, keyboard, now);
    Enumerate(context, fixture, 2U, mouse, now);
    UInt8 data[8U]{};
    UInt64 token{2000U};
    const auto request =
        [&data, &token](const In::UsbOperation operation, const UInt8 port, const UInt8 endpoint) noexcept
    {
        auto made = Request(operation, ++token, operation == In::UsbOperation::InterruptIn ? data : nullptr,
                            operation == In::UsbOperation::InterruptIn ? sizeof(data) : 0U);
        made.Address = static_cast<UInt8>(port + 6U);
        made.Endpoint.Address = endpoint;
        return made;
    };
    U::Usb1Completion result{};
    const auto configure = [&](const UInt8 pipe, const UInt8 port, const UInt8 endpoint) noexcept
    {
        return fixture.Controller.Submit(pipe, port, request(In::UsbOperation::ConfigureEndpoint, port, endpoint)) &&
               fixture.Controller.TakeCompletion(pipe, result) && (result.Status == In::TransferStatus::Success);
    };
    // One report on pipe through port; true when it delivers report's bytes.
    const auto poll = [&](const UInt8 pipe, const UInt8 port, const UInt8 endpoint, const UInt8* const report,
                          const Size length) noexcept
    {
        const auto submitted = request(In::UsbOperation::InterruptIn, port, endpoint);
        if (!fixture.Controller.Submit(pipe, port, submitted))
        {
            return false;
        }
        result = Complete(context, fixture, pipe, now);
        return (result.Token == submitted.Token) && (result.Status == In::TransferStatus::Success) &&
               (result.ActualLength == length) && (std::memcmp(data, report, length) == 0);
    };
    ASTERIA_EXPECT(context, "each logical pipe configures its own endpoint", configure(3U, 1U, 0x81U),
                   configure(4U, 1U, 0x82U), configure(5U, 2U, 0x82U));
    constexpr UInt8 Last{U::Usb1MaximumPipes};
    ASTERIA_EXPECT(context, "logical pipes are numbered from 1 to the maximum", configure(Last, 1U, 0x81U),
                   !fixture.Controller.Submit(0U, 1U, request(In::UsbOperation::ConfigureEndpoint, 1U, 0x81U)),
                   !fixture.Controller.Submit(Last + 1U, 1U, request(In::UsbOperation::ConfigureEndpoint, 1U, 0x81U)),
                   !fixture.Controller.TakeCompletion(0U, result),
                   !fixture.Controller.TakeCompletion(Last + 1U, result));
    const auto keys = request(In::UsbOperation::InterruptIn, 1U, 0x81U);
    const bool accepted = fixture.Controller.Submit(3U, 1U, keys);
    const bool busy = fixture.Controller.IsBusy();
    const bool refused = !fixture.Controller.Submit(4U, 1U, request(In::UsbOperation::InterruptIn, 1U, 0x82U));
    result = Complete(context, fixture, 3U, now);
    ASTERIA_EXPECT(context, "a request keeps the controller busy until it completes", accepted, busy, refused,
                   result.Token == keys.Token, result.Status == In::TransferStatus::Success,
                   std::memcmp(data, Fx::UsbKeyboardReport, 8U) == 0, !fixture.Controller.IsBusy());
    ASTERIA_EXPECT(context, "pipes of one device and of another port keep their own DATA toggles",
                   poll(4U, 1U, 0x82U, Fx::UsbMouseReport, 4U), poll(5U, 2U, 0x82U, Fx::UsbMouseReport, 4U),
                   fixture.Model.Devices[0U].ReportToggle, fixture.Model.Devices[0U].SecondToggle,
                   fixture.Model.Devices[1U].SecondToggle, fixture.Model.Good, fixture.Model.Devices[0U].Good,
                   fixture.Model.Devices[1U].Good);
    // Pipe 3's completion waits in its slot while pipe 4 runs.
    const auto held = request(In::UsbOperation::InterruptIn, 1U, 0x81U);
    const bool heldAccepted = fixture.Controller.Submit(3U, 1U, held);
    for (Size i{0U}; (i < 1200U) && fixture.Controller.IsBusy(); ++i)
    {
        Tick(fixture, now);
    }
    const bool slotFull = !fixture.Controller.Submit(3U, 1U, held);
    const bool beside = poll(4U, 1U, 0x82U, Fx::UsbMouseReport, 4U);
    const bool taken = fixture.Controller.TakeCompletion(3U, result);
    ASTERIA_EXPECT(context, "each pipe retains its own completion", heldAccepted, slotFull, beside, taken,
                   result.Token == held.Token, result.Status == In::TransferStatus::Success, result.ActualLength == 8U);
    // Pipes 3 and 4 are back at DATA0, pipe 5 at DATA1: one more report on
    // each of 3 and 4 puts every pipe at DATA1, where resetting the wrong
    // pipe, or not the right one, breaks the device's toggle sequence.
    ASTERIA_EXPECT(context, "every pipe at DATA1 before the halt clear", poll(3U, 1U, 0x81U, Fx::UsbKeyboardReport, 8U),
                   poll(4U, 1U, 0x82U, Fx::UsbMouseReport, 4U), fixture.Model.Devices[0U].ReportToggle,
                   fixture.Model.Devices[0U].SecondToggle, fixture.Model.Devices[1U].SecondToggle);
    auto clear = request(In::UsbOperation::Control, 1U, 0x81U);
    clear.Setup = {.Bytes = {2U, 1U, 0U, 0U, 0x82U, 0U, 0U, 0U}};
    const bool cleared = fixture.Controller.Submit(1U, 1U, clear);
    result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "the halt clear resets only the pipe of that endpoint of that device", cleared,
                   result.Status == In::TransferStatus::Success, !fixture.Model.Devices[0U].SecondToggle,
                   poll(4U, 1U, 0x82U, Fx::UsbMouseReport, 4U), poll(3U, 1U, 0x81U, Fx::UsbKeyboardReport, 8U),
                   poll(5U, 2U, 0x82U, Fx::UsbMouseReport, 4U), fixture.Model.Good, fixture.Model.Devices[0U].Good,
                   fixture.Model.Devices[1U].Good);
    fixture.Model.Devices[0U].Connected = false;
    const bool gone = !fixture.Controller.GetPortStatus(1U).Connected;
    fixture.Model.Devices[0U].Connected = true;
    U::Usb1Completion first{};
    U::Usb1Completion second{};
    const bool firstAccepted = fixture.Controller.Submit(3U, 1U, request(In::UsbOperation::InterruptIn, 1U, 0x81U));
    const bool firstDone = fixture.Controller.TakeCompletion(3U, first);
    const bool secondAccepted = fixture.Controller.Submit(4U, 1U, request(In::UsbOperation::InterruptIn, 1U, 0x82U));
    const bool secondDone = fixture.Controller.TakeCompletion(4U, second);
    ASTERIA_EXPECT(context, "a disconnect unconfigures every pipe of its port and no other", gone, firstAccepted,
                   firstDone, first.Status == In::TransferStatus::Error, secondAccepted, secondDone,
                   second.Status == In::TransferStatus::Error, poll(5U, 2U, 0x82U, Fx::UsbMouseReport, 4U),
                   fixture.Model.Good, fixture.Controller.Halt());
}

template <class T> void AsyncQuiescence(TestContext& context) noexcept
{
    T fixture{};
    ASTERIA_EXPECT(context, "async fixture starts", fixture.Start());
    UInt64 now{0U};
    In::UsbDevice device{};
    Enumerate(context, fixture, 1U, device, now);
    UInt8 data[8U]{0xA5U};
    fixture.Model.AsyncHold = true;
    fixture.Model.Devices[0U].Fault = WireFault::Nak;
    const auto report = Request(In::UsbOperation::InterruptIn, 1201U, data, sizeof(data));
    ASTERIA_EXPECT(context, "async report accepted", fixture.Controller.Submit(1U, report));
    for (Size i{0U}; i < 5U; ++i)
    {
        Tick(fixture, now);
    }
    ASTERIA_EXPECT(context, "device packet may be held asynchronously", fixture.Model.PendingTd != 0U);
    fixture.Model.Devices[0U].Connected = false;
    const auto result = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "async packet canceled before borrow release and descriptor reuse",
                   result.Status == In::TransferStatus::Disconnected, result.Token == 1201U,
                   fixture.Model.PendingTd == 0U, fixture.Model.Canceled == 1U, data[0U] == 0xA5U, fixture.Model.Good);
    fixture.Model.Devices[0U].Connected = true;
    device.Disconnect();
    Enumerate(context, fixture, 1U, device, now);
    auto timeout = report;
    timeout.Token = 1202U;
    timeout.TimeoutMilliseconds = 1000U;
    ASTERIA_EXPECT(context, "held packet deadline request accepted", fixture.Controller.Submit(1U, timeout));
    const auto retry = Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "timeout cancels held packet before reuse", retry.Status == In::TransferStatus::TimedOut,
                   fixture.Model.PendingTd == 0U, fixture.Model.Canceled == 2U);
    timeout.Token = 1203U;
    ASTERIA_EXPECT(context, "held packet halt request accepted", fixture.Controller.Submit(1U, timeout));
    for (Size i{0U}; i < 5U; ++i)
    {
        Tick(fixture, now);
    }
    U::Usb1Completion halted{};
    ASTERIA_EXPECT(context, "public halt synchronously cancels held packet", fixture.Model.PendingTd != 0U,
                   fixture.Controller.Halt(), fixture.Model.PendingTd == 0U,
                   fixture.Controller.TakeCompletion(1U, halted), halted.Status == In::TransferStatus::Retryable);
}

} // namespace Asteria::Test::Usb
#endif
