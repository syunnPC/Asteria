// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Ohci.hpp"

#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Usb/FakeOhci.hpp"
#include "User/Usb/Usb1ScheduleSuite.hpp"

namespace
{
namespace T = Asteria::Test::Usb;
namespace U = Asteria::User::Usb;
using Asteria::TestSupport::TestContext;
void HandoffAndMemory(TestContext& context) noexcept
{
    T::OhciFixture bios{};
    bios.Model.HoldBios = true;
    ASTERIA_EXPECT(context, "BIOS timeout retains ownership and forbids reset",
                   bios.Controller.Handoff(0x100U, 7U) == U::Usb1Handoff::BiosTimeout, bios.Model.OwnershipReads <= 8U,
                   bios.Model.Resets == 0U, !bios.Controller.Initialize(bios.Memory, 0x100U));
    bios.Model.HoldBios = false;
    ASTERIA_EXPECT(context, "later BIOS release succeeds", bios.Controller.Handoff(0x100U) == U::Usb1Handoff::Success);
    auto invalid = bios.Memory;
    invalid.Buffer.Address = 0x100000000U;
    ASTERIA_EXPECT(context, "reject DMA above 4GiB before reset", !bios.Controller.Initialize(invalid, 0x100U),
                   bios.Model.Resets == 0U);
    invalid = bios.Memory;
    invalid.Endpoints.Cpu = bios.Hcca + 16U;
    ASTERIA_EXPECT(context, "reject partial CPU alias", !bios.Controller.Initialize(invalid, 0x100U));
    invalid = bios.Memory;
    invalid.Endpoints.Address = bios.Memory.Hcca.Address + 16U;
    ASTERIA_EXPECT(context, "reject partial device alias", !bios.Controller.Initialize(invalid, 0x100U));
    bios.Model.Registers[0x48U / 4U] = 16U;
    ASTERIA_EXPECT(context, "reject unrepresentable root ports", !bios.Controller.Initialize(bios.Memory, 0x100U));
    bios.Model.Registers[0x48U / 4U] = 2U;
    ASTERIA_EXPECT(context, "frame timing and root power configured", bios.Controller.Initialize(bios.Memory, 0x100U),
                   (bios.Model.Registers[0x34U / 4U] & 0x3FFFU) == 11999U,
                   ((bios.Model.Registers[0x34U / 4U] >> 16U) & 0x7FFFU) == (11999U - 210U) * 6U / 7U,
                   bios.Model.Registers[0x40U / 4U] == 10799U, bios.Model.PowerWrites == 2U, bios.Controller.Halt());
    T::OhciFixture changes{};
    ASTERIA_EXPECT(context, "root changes start", changes.Start());
    changes.Model.Registers[0x54U / 4U] |= 0x1F0000U;
    changes.Model.Registers[0x58U / 4U] |= 0x1F0000U;
    changes.Controller.Poll(1U);
    ASTERIA_EXPECT(context, "idle poll drains all root change bits before INTx rearm",
                   (changes.Model.Registers[0x54U / 4U] & 0x1F0000U) == 0U,
                   (changes.Model.Registers[0x58U / 4U] & 0x1F0000U) == 0U, changes.Controller.Halt());
    T::OhciFixture reset{};
    reset.Model.HoldReset = true;
    ASTERIA_EXPECT(context, "stuck HCR is bounded", reset.Controller.Handoff(0x100U) == U::Usb1Handoff::Success,
                   !reset.Controller.Initialize(reset.Memory, 0x100U, 7U),
                   reset.Controller.GetState() == U::Usb1State::Faulted);
}
void WrapAndDone(TestContext& context) noexcept
{
    T::OhciFixture fixture{};
    ASTERIA_EXPECT(context, "wrap start", fixture.Start());
    T::UInt64 now{0U};
    Asteria::User::Input::UsbDevice device{};
    T::Enumerate(context, fixture, 1U, device, now);
    fixture.Model.Registers[0x3CU / 4U] = 0xFFFEU;
    T::UInt8 data[8U]{};
    auto request = T::Request(Asteria::User::Input::UsbOperation::InterruptIn, 600U, data, sizeof(data));
    ASTERIA_EXPECT(context, "frame wrap report accepted", fixture.Controller.Submit(1U, request));
    const auto result = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "HCCA 16bit wrap preserves done and carry", result.ActualLength == 8U,
                   result.Status == Asteria::User::Input::TransferStatus::Success,
                   fixture.Model.Registers[0x3CU / 4U] < 10U, fixture.Model.Good, fixture.Model.Devices[0U].Good);
    request.Token = 601U;
    fixture.Model.Devices[0U].Fault = T::WireFault::Nak;
    ASTERIA_EXPECT(context, "foreign done test accepts report", fixture.Controller.Submit(1U, request));
    T::Tick(fixture, now);
    U::Store32(fixture.Hcca + 0x84U, 0xABCDE001U);
    fixture.Controller.Poll(now);
    const auto rejected = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "foreign done pointer rejects without dereference", rejected.Token == 601U,
                   rejected.Status == Asteria::User::Input::TransferStatus::Error, fixture.Controller.Halt());
}
void AbortDoneRace(TestContext& context) noexcept
{
    T::OhciFixture fixture{};
    ASTERIA_EXPECT(context, "done race fixture starts", fixture.Start());
    T::UInt64 now{0U};
    Asteria::User::Input::UsbDevice device{};
    T::Enumerate(context, fixture, 1U, device, now);
    T::UInt8 data[8U]{0xA5U};
    auto request = T::Request(Asteria::User::Input::UsbOperation::InterruptIn, 1401U, data, sizeof(data));
    fixture.Model.Devices[0U].Fault = T::WireFault::Nak;
    ASTERIA_EXPECT(context, "done race report accepted", fixture.Controller.Submit(1U, request));
    T::Tick(fixture, now);
    ASTERIA_EXPECT(context, "done race abort accepted", fixture.Controller.Abort(1U, request.Token));
    // A prefetched TD may retire into HcDoneHead while WDH prevents writeback.
    // No TD/buffer may be reused until both hardware and HCCA queues drain.
    fixture.Model.HoldDoneFlush = true;
    fixture.Model.LateDone(static_cast<T::UInt32>(fixture.Memory.Descriptors.Address));
    T::Tick(fixture, now);
    T::Tick(fixture, now);
    T::Tick(fixture, now);
    U::OhciCompletion result{};
    ASTERIA_EXPECT(context, "late hardware done head retains cancelled borrow",
                   !fixture.Controller.TakeCompletion(1U, result), fixture.Model.Read32(0x30U) != 0U);
    fixture.Model.HoldDoneFlush = false;
    result = T::Complete(context, fixture, 1U, now);
    ASTERIA_EXPECT(context, "late done queue drained before cancelled completion",
                   result.Status == Asteria::User::Input::TransferStatus::Cancelled, fixture.Model.Read32(0x30U) == 0U,
                   U::Load32(fixture.Hcca + 0x84U) == 0U, data[0U] == 0xA5U,
                   fixture.Controller.GetState() == U::Usb1State::Running, fixture.Controller.Halt());
}
// A report the hardware retired into its done queue before the Skip took
// effect, unseen by software, is delivered with its data when the request is
// aborted (mode 0) or times out (mode 1); the ED carry keeps the next toggle.
// The mouse on port 2 sends a short report, so the length comes from the TD.
void WonReport(TestContext& context) noexcept
{
    for (T::Size mode{0U}; mode < 2U; ++mode)
    {
        T::OhciFixture fixture{};
        ASTERIA_EXPECT(context, "won report fixture starts", fixture.Start());
        T::UInt64 now{0U};
        Asteria::User::Input::UsbDevice device{};
        T::Enumerate(context, fixture, 2U, device, now);
        T::Usb1Wire& mouse = fixture.Model.Devices[1U];
        // The interrupt pipe takes up to 8 bytes; the mouse sends 4.
        auto configure = T::Request(Asteria::User::Input::UsbOperation::ConfigureEndpoint, 1450U);
        configure.Address = 8U;
        U::OhciCompletion result{};
        ASTERIA_EXPECT(context, "won report pipe configured", fixture.Controller.Submit(2U, configure),
                       fixture.Controller.TakeCompletion(2U, result),
                       result.Status == Asteria::User::Input::TransferStatus::Success);
        T::UInt8 data[8U]{0xA5U};
        auto request = T::Request(Asteria::User::Input::UsbOperation::InterruptIn, 1451U, data, sizeof(data));
        request.Address = 8U;
        request.TimeoutMilliseconds = (mode == 0U) ? 0U : 50U;
        // The done queue never reaches the HCCA, so software never sees the TD.
        fixture.Model.HoldDoneFlush = true;
        const T::UInt64 submitted = now;
        ASTERIA_EXPECT(context, "won report request accepted", fixture.Controller.Submit(2U, request));
        for (T::Size i{0U}; (i < 40U) && (fixture.Model.Read32(0x30U) == 0U); ++i)
        {
            T::Tick(fixture, now);
        }
        const bool retired = fixture.Model.Read32(0x30U) != 0U;
        const bool stopped = (mode != 0U) || fixture.Controller.Abort(2U, request.Token);
        while (now < submitted + request.TimeoutMilliseconds)
        {
            T::Tick(fixture, now);
        }
        fixture.Model.HoldDoneFlush = false;
        result = T::Complete(context, fixture, 2U, now);
        ASTERIA_EXPECT(context, "a report retired before the unlink is delivered with its data", retired, stopped,
                       result.Token == request.Token, result.Status == Asteria::User::Input::TransferStatus::Success,
                       result.ActualLength == 4U, std::memcmp(data, Asteria::Test::Input::UsbMouseReport, 4U) == 0,
                       mouse.ReportToggle);
        ++request.Token;
        request.TimeoutMilliseconds = 0U;
        ASTERIA_EXPECT(context, "next report after the won one accepted", fixture.Controller.Submit(2U, request));
        result = T::Complete(context, fixture, 2U, now);
        ASTERIA_EXPECT(context, "the next report follows the ED carry",
                       result.Status == Asteria::User::Input::TransferStatus::Success, fixture.Model.Good, mouse.Good);
        // A later abort that no report won is cancelled, not the earlier report.
        mouse.Fault = T::WireFault::Nak;
        ++request.Token;
        const T::UInt32 attempts = mouse.Attempts;
        const bool held = fixture.Controller.Submit(2U, request);
        for (T::Size i{0U}; (i < 40U) && (mouse.Attempts == attempts); ++i)
        {
            T::Tick(fixture, now);
        }
        const bool aborted = fixture.Controller.Abort(2U, request.Token);
        result = T::Complete(context, fixture, 2U, now);
        ASTERIA_EXPECT(context, "a later abort that no report won is cancelled", held, aborted,
                       result.Status == Asteria::User::Input::TransferStatus::Cancelled, result.ActualLength == 0U,
                       fixture.Controller.Halt());
    }
}

} // namespace
int main()
{
    TestContext context{"Ohci"};
    AbortDoneRace(context);
    WonReport(context);
    HandoffAndMemory(context);
    T::DeadlinesAndAbort<T::OhciFixture>(context);
    T::EndToEnd<T::OhciFixture>(context);
    T::Boundaries<T::OhciFixture>(context);
    T::RandomSchedules<T::OhciFixture>(context);
    T::Disconnects<T::OhciFixture>(context);
    T::Recovery<T::OhciFixture>(context);
    T::Intervals<T::OhciFixture>(context);
    T::AsyncQuiescence<T::OhciFixture>(context);
    T::LogicalPipes<T::OhciFixture>(context);
    T::ResetAndFrameTimeout<T::OhciFixture>(context);
    WrapAndDone(context);
    return context.Finish();
}
