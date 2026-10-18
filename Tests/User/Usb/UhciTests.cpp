// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Uhci.hpp"

#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Usb/FakeUhci.hpp"
#include "User/Usb/Usb1ScheduleSuite.hpp"

namespace
{
namespace T = Asteria::Test::Usb;
namespace U = Asteria::User::Usb;
using Asteria::TestSupport::TestContext;
void HandoffAndMemory(TestContext& context) noexcept
{
    T::UhciFixture bios{};
    bios.Model.HoldBios = true;
    ASTERIA_EXPECT(context, "firmware reasserting traps fails bounded readback",
                   bios.Controller.Handoff(7U) == U::Usb1Handoff::BiosTimeout, bios.Model.OwnershipReads == 7U,
                   bios.Model.Resets == 0U, !bios.Controller.Initialize(bios.Memory, 0x20U));
    bios.Model.HoldBios = false;
    ASTERIA_EXPECT(context, "handoff platform write clears traps/status and enables PIRQ",
                   bios.Controller.Handoff() == U::Usb1Handoff::Success, bios.Model.LegacyWrite == 0xAF00U,
                   bios.Model.Legacy == 0x2000U);
    // Independent LEGSUP bit model from PIIX4 section 6.1.12:
    // all 128 trap combinations, pending RW1C statuses, and both RO bits.
    for (T::UInt16 traps{0U}; traps < 128U; ++traps)
    {
        T::UhciFixture legacy{};
        legacy.Model.Legacy = static_cast<T::UInt16>((traps & 0x3FU) | ((traps & 0x40U) << 1U) | 0x8F00U | 0x1040U);
        ASTERIA_EXPECT(context, "only writable controls decide ownership and RW1C bits clear",
                       legacy.Controller.Handoff(1U) == U::Usb1Handoff::Success, legacy.Model.Legacy == 0x3040U,
                       legacy.Model.LegacyWrite == 0xAF00U);
    }
    auto invalid = bios.Memory;
    invalid.Buffer.Address = 0xFFFFFFC0U;
    ASTERIA_EXPECT(context, "reject DMA crossing 4GiB before reset", !bios.Controller.Initialize(invalid, 0x20U),
                   bios.Model.Resets == 0U);
    invalid = bios.Memory;
    invalid.Queues.Cpu = bios.Frames + 16U;
    ASTERIA_EXPECT(context, "reject partial CPU alias", !bios.Controller.Initialize(invalid, 0x20U));
    invalid = bios.Memory;
    invalid.Descriptors.Bytes = 2079U;
    ASTERIA_EXPECT(context, "full maximal control schedule capacity required",
                   !bios.Controller.Initialize(invalid, 0x20U));
    ASTERIA_EXPECT(context, "port BAR, SOF and frame base configured", bios.Controller.Initialize(bios.Memory, 0x20U),
                   bios.Model.Sof == 64U, bios.Model.Base == bios.Memory.Frames.Address,
                   bios.Model.Registers[2U] == 15U, bios.Controller.Halt());
    T::UhciFixture changes{};
    ASTERIA_EXPECT(context, "root changes start", changes.Start());
    changes.Model.Registers[8U] |= 0xAU;
    changes.Model.Registers[9U] |= 0xAU;
    changes.Controller.Poll(1U);
    ASTERIA_EXPECT(context, "idle poll drains both CSC/PEC ports before INTx rearm",
                   (changes.Model.Registers[8U] & 0xAU) == 0U, (changes.Model.Registers[9U] & 0xAU) == 0U,
                   changes.Controller.Halt());
    T::UhciFixture denied{};
    denied.Model.DenyConfig = true;
    ASTERIA_EXPECT(context, "platform denial propagates", denied.Controller.Handoff() == U::Usb1Handoff::InvalidAccess);
    T::UhciFixture reset{};
    reset.Model.HoldReset = true;
    ASTERIA_EXPECT(context, "stuck UHCI reset bounded", reset.Controller.Handoff() == U::Usb1Handoff::Success,
                   !reset.Controller.Initialize(reset.Memory, 0x20U, 7U),
                   reset.Controller.GetState() == U::Usb1State::Faulted);
}
void Wrap(TestContext& context) noexcept
{
    T::UhciFixture fixture{};
    ASTERIA_EXPECT(context, "wrap start", fixture.Start());
    T::UInt64 now{0U};
    Asteria::User::Input::UsbDevice device{};
    T::Enumerate(context, fixture, 1U, device, now);
    constexpr T::UInt16 frames[]{1022U, 2046U};
    for (const T::UInt16 frame : frames)
    {
        fixture.Model.Registers[3U] = frame;
        T::UInt8 data[8U]{};
        auto request = T::Request(Asteria::User::Input::UsbOperation::InterruptIn, frame, data, sizeof(data));
        ASTERIA_EXPECT(context, "wrap report accepted", fixture.Controller.Submit(1U, request));
        const auto result = T::Complete(context, fixture, 1U, now);
        ASTERIA_EXPECT(context, "1024 frame index and 2048 frame-number wraps", result.ActualLength == 8U,
                       result.Status == Asteria::User::Input::TransferStatus::Success, fixture.Model.Good,
                       fixture.Model.Devices[0U].Good);
    }
    ASTERIA_EXPECT(context, "wrap halt", fixture.Controller.Halt());
}
void AbortPacketRace(TestContext& context) noexcept
{
    T::UhciFixture fixture{};
    ASTERIA_EXPECT(context, "packet race starts", fixture.Start());
    T::UInt64 now{0U};
    Asteria::User::Input::UsbDevice device{};
    T::Enumerate(context, fixture, 1U, device, now);
    T::UInt8 data[8U]{0xA5U};
    auto configure = T::Request(Asteria::User::Input::UsbOperation::ConfigureEndpoint, 1500U);
    configure.Endpoint.Interval = 1U;
    U::UhciCompletion result{};
    ASTERIA_EXPECT(context, "interval one configured", fixture.Controller.Submit(1U, configure),
                   fixture.Controller.TakeCompletion(1U, result));
    auto report = T::Request(Asteria::User::Input::UsbOperation::InterruptIn, 1501U, data, sizeof(data));
    report.Endpoint = configure.Endpoint;
    fixture.Model.Devices[0U].Fault = T::WireFault::Nak;
    fixture.Model.AsyncHold = true;
    ASTERIA_EXPECT(context, "pending packet request", fixture.Controller.Submit(1U, report));
    T::Tick(fixture, now);
    T::Tick(fixture, now);
    ASTERIA_EXPECT(context, "unlink while packet in flight", fixture.Model.PendingTd != 0U,
                   fixture.Controller.Abort(1U, report.Token));
    fixture.Model.CompletePendingZlp();
    result = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "ZLP winning abort is delivered as the report it is and preserves the next toggle",
                   result.Status == Asteria::User::Input::TransferStatus::Success, result.ActualLength == 0U,
                   data[0U] == 0xA5U, fixture.Model.Devices[0U].ReportToggle);
    fixture.Model.AsyncHold = false;
    for (T::Size i{0U}; i < 2U; ++i)
    {
        ++report.Token;
        ASTERIA_EXPECT(context, "next interval one report accepted", fixture.Controller.Submit(1U, report));
        result = T::Complete(context, fixture, 1U, now);
        ASTERIA_EXPECT(context, "interval one toggle follows accepted packet",
                       result.Status == Asteria::User::Input::TransferStatus::Success, result.ActualLength == 8U,
                       fixture.Model.Good, fixture.Model.Devices[0U].Good);
    }
    ASTERIA_EXPECT(context, "packet race halt", fixture.Controller.Halt());
}
// A report that reached the bounce buffer before the unlink took effect is
// delivered with its data, and the device's next toggle is kept.
void AbortReportRace(TestContext& context) noexcept
{
    T::UhciFixture fixture{};
    ASTERIA_EXPECT(context, "report race starts", fixture.Start());
    T::UInt64 now{0U};
    Asteria::User::Input::UsbDevice device{};
    T::Enumerate(context, fixture, 1U, device, now);
    T::UInt8 data[8U]{0xA5U};
    auto report = T::Request(Asteria::User::Input::UsbOperation::InterruptIn, 1551U, data, sizeof(data));
    fixture.Model.Devices[0U].Fault = T::WireFault::Nak;
    fixture.Model.AsyncHold = true;
    ASTERIA_EXPECT(context, "held report request", fixture.Controller.Submit(1U, report));
    for (T::Size i{0U}; (i < 20U) && (fixture.Model.PendingTd == 0U); ++i)
    {
        T::Tick(fixture, now);
    }
    ASTERIA_EXPECT(context, "unlink while the report is in flight", fixture.Model.PendingTd != 0U,
                   fixture.Controller.Abort(1U, report.Token));
    fixture.Model.CompletePendingReport();
    U::UhciCompletion result = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "a report winning the abort is delivered with its data",
                   result.Status == Asteria::User::Input::TransferStatus::Success, result.ActualLength == 8U,
                   std::memcmp(data, Asteria::Test::Input::UsbKeyboardReport, sizeof(data)) == 0,
                   fixture.Model.Devices[0U].ReportToggle);
    fixture.Model.AsyncHold = false;
    ++report.Token;
    ASTERIA_EXPECT(context, "next report after the won one accepted", fixture.Controller.Submit(1U, report));
    result = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "the next report follows the won report's toggle",
                   result.Status == Asteria::User::Input::TransferStatus::Success, fixture.Model.Good,
                   fixture.Model.Devices[0U].Good);
    // A later abort that no report won is cancelled, not the earlier report.
    fixture.Model.Devices[0U].Fault = T::WireFault::Nak;
    ++report.Token;
    const T::UInt32 attempts = fixture.Model.Devices[0U].Attempts;
    const bool held = fixture.Controller.Submit(1U, report);
    for (T::Size i{0U}; (i < 40U) && (fixture.Model.Devices[0U].Attempts == attempts); ++i)
    {
        T::Tick(fixture, now);
    }
    const bool aborted = fixture.Controller.Abort(1U, report.Token);
    result = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "a later abort that no report won is cancelled", held, aborted,
                   result.Status == Asteria::User::Input::TransferStatus::Cancelled, result.ActualLength == 0U,
                   fixture.Controller.Halt());
}
// A report the controller retired needs only a frame boundary (two frames)
// before its descriptors are reused; a completed control transfer still
// waits the 33 frames an unlink does.
void Retirement(TestContext& context) noexcept
{
    T::UhciFixture fixture{};
    ASTERIA_EXPECT(context, "retirement fixture starts", fixture.Start());
    T::UInt64 now{0U};
    Asteria::User::Input::UsbDevice device{};
    T::Enumerate(context, fixture, 1U, device, now);
    T::UInt8 data[8U]{};
    // Ticks from the device's packet to the completion.
    const auto measure = [&](const Asteria::User::Input::UsbRequest& request, const T::UInt32& packets) noexcept
    {
        const T::UInt32 before = packets;
        T::UInt64 sent{0U};
        U::UhciCompletion result{};
        bool done{!fixture.Controller.Submit(1U, request)};
        for (T::Size i{0U}; (i < 200U) && !done; ++i)
        {
            T::Tick(fixture, now);
            if ((sent == 0U) && (packets != before))
            {
                sent = now;
            }
            done = fixture.Controller.TakeCompletion(1U, result);
        }
        return (result.Status == Asteria::User::Input::TransferStatus::Success) && (sent != 0U) ? now - sent : 0U;
    };
    const T::UInt64 report =
        measure(T::Request(Asteria::User::Input::UsbOperation::InterruptIn, 1561U, data, sizeof(data)),
                fixture.Model.Devices[0U].Reports);
    auto control = T::Request(Asteria::User::Input::UsbOperation::Control, 1562U, data, sizeof(data));
    control.Setup = Asteria::User::Input::UsbDescriptors::GetDescriptor(Asteria::User::Input::DescriptorKind::Device,
                                                                        0U, sizeof(data));
    const T::UInt64 transfer = measure(control, fixture.Model.Devices[0U].Statuses);
    ASTERIA_EXPECT(context, "a retired report completes two frames after its packet, a control transfer 33",
                   report == 2U, transfer >= T::UhciFixture::RetirementFrames, fixture.Model.Good,
                   fixture.Controller.Halt());
}

} // namespace
int main()
{
    TestContext context{"Uhci"};
    AbortPacketRace(context);
    AbortReportRace(context);
    Retirement(context);
    HandoffAndMemory(context);
    T::DeadlinesAndAbort<T::UhciFixture>(context);
    T::EndToEnd<T::UhciFixture>(context);
    T::Boundaries<T::UhciFixture>(context);
    T::RandomSchedules<T::UhciFixture>(context);
    T::Disconnects<T::UhciFixture>(context);
    T::Recovery<T::UhciFixture>(context);
    T::Intervals<T::UhciFixture>(context);
    T::AsyncQuiescence<T::UhciFixture>(context);
    T::LogicalPipes<T::UhciFixture>(context);
    T::ResetAndFrameTimeout<T::UhciFixture>(context);
    Wrap(context);
    return context.Finish();
}
