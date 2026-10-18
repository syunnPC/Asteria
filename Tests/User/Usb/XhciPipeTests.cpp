// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Xhci.hpp"

#include <memory>

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Input/UsbCompositeFixture.hpp"
#include "User/Usb/FakeXhci.hpp"

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
// Constant-initialized client storage is separate from model DMA mappings.
constinit UInt8 g_data[65536U]{};

struct PipeFixture
{
    explicit PipeFixture(const bool large = false) noexcept : Base{large}
    {
        Mappings[0U] = Base.EndpointMappings[0U][0U];
        for (Size i{1U}; i < 4U; ++i)
        {
            Mappings[i] = {.Ring = Base.Map(0x300000U + i * 0x20000U, Rings[i - 1U], 256U),
                           .Buffer = Base.Map(0x310000U + i * 0x20000U, i == 1U ? MouseBuffer : BulkBuffers[i - 2U],
                                              i == 1U ? 1024U : 65536U)};
        }
        Base.Ports[0U].Pipes = Mappings;
        Base.Ports[0U].PipeCount = 4U;
        std::memcpy(Configuration, Asteria::Test::Input::CompositeConfiguration, sizeof(Configuration));
        // Full-speed bulk maximum packet size, USB 2.0 table 9-13.
        Configuration[105U] = 64U;
        Configuration[106U] = 0U;
        Configuration[112U] = 64U;
        Configuration[113U] = 0U;
        Base.Model.ConfigurationBytes = Configuration;
        Base.Model.ConfigurationLength = sizeof(Configuration);
    }
    Test::Fixture Base;
    X::EndpointMemory Mappings[4U]{};
    UInt8 Rings[3U][256U]{};
    UInt8 MouseBuffer[1024U]{};
    UInt8 BulkBuffers[2U][65536U]{};
    UInt8 Configuration[115U]{};
};
// The large test owner stays in main's frame; helpers do not copy it.
void Enumerate(TestContext& context, PipeFixture& fixture, In::UsbDevice& core, UInt64& now) noexcept
{
    X::XhciPort port{fixture.Base.Controller, 1U};
    ASTERIA_EXPECT(context, "initialize composite model",
                   fixture.Base.Controller.Initialize(fixture.Base.Memory, 0x2100U), core.Attach(19U));
    In::EventBatch events{};
    for (Size i{0U}; i < 150U && core.GetState() != In::UsbState::Polling; ++i)
    {
        core.Step(port);
        fixture.Base.Controller.Poll(++now);
        X::Completion result{};
        if (fixture.Base.Controller.TakeCompletion(1U, result))
        {
            ASTERIA_EXPECT(context, "enumeration completion routed",
                           core.Complete(result.Token, result.Status, result.ActualLength, events));
        }
    }
    ASTERIA_EXPECT(context, "both HID interfaces configured once", core.GetState() == In::UsbState::Polling,
                   core.InterfaceCount() == 2U, fixture.Base.Model.Configurations == 2U,
                   fixture.Base.Model.ProtocolMask == 3U, fixture.Base.Model.IdleMask == 3U, fixture.Base.Model.Good);
}
[[nodiscard]] In::UsbRequest Request(const In::UsbOperation operation, const UInt64 token, const UInt8 address) noexcept
{
    return {.Operation = operation,
            .Token = token,
            .Speed = In::UsbSpeed::Full,
            .Address = 19U,
            .PacketSize0 = 8U,
            .Setup = {},
            .Endpoint = {.Address = address,
                         .Attributes = 2U,
                         .MaxPacketSize = 64U,
                         .Interval = 0U,
                         .MaxBurst = 0U,
                         .CompanionAttributes = 0U,
                         .BytesPerInterval = 0U,
                         .HasCompanion = false},
            .Data = g_data,
            .Length = 65536U};
}
void ConfigureBulk(TestContext& context, PipeFixture& fixture, UInt64& now) noexcept
{
    constexpr UInt8 addresses[]{0x83U, 3U};
    for (const UInt8 address : addresses)
    {
        auto request = Request(In::UsbOperation::ConfigureEndpoint, 100U + address, address);
        request.Length = 0U;
        ASTERIA_EXPECT(context, "configure bulk endpoint alongside HID", fixture.Base.Controller.Submit(1U, request));
        fixture.Base.Controller.Poll(++now);
        X::Completion result{};
        ASTERIA_EXPECT(context, "bulk context completion", fixture.Base.Controller.TakeCompletion(1U, 0U, result),
                       result.Status == In::TransferStatus::Success, fixture.Base.Model.LastPayload == 0U,
                       fixture.Base.Model.Good);
    }
    ASTERIA_EXPECT(
        context, "adding lower OUT DCI preserves highest context entry",
        X::Load32(fixture.Base.Outputs[0U]) >> 27U == 7U,
        fixture.Base.Model.Slots[0U].Endpoints[3U].Pointer != fixture.Base.Model.Slots[0U].Endpoints[5U].Pointer,
        fixture.Base.Model.Slots[0U].Endpoints[6U].Pointer != fixture.Base.Model.Slots[0U].Endpoints[7U].Pointer);
}
void ReportsAndBulk(TestContext& context, PipeFixture& fixture, In::UsbDevice& core, UInt64& now) noexcept
{
    auto& controller = fixture.Base.Controller;
    auto& model = fixture.Base.Model;
    X::XhciPort port{controller, 1U};
    model.HoldTransfers = true;
    core.Step(port);
    core.Step(port);
    controller.Poll(now + 60000U);
    now += 60000U;
    X::Completion result{};
    ASTERIA_EXPECT(context, "two independent idle HID polls retain their borrows", core.IsOutstanding(),
                   !controller.TakeCompletion(1U, result), controller.GetState() == X::ControllerState::Running);
    auto request = Request(In::UsbOperation::BulkIn, 1000U, 0x83U);
    ASTERIA_EXPECT(context, "bulk IN queues while both HID pipes NAK", controller.Submit(1U, request));
    model.HoldTransfers = false;
    model.Transfers(1U, 7U);
    model.Transfers(1U, 5U);
    model.Transfers(1U, 3U);
    controller.Poll(++now);
    ASTERIA_EXPECT(context, "full 64KiB bulk IN completion", controller.TakeCompletion(1U, 0x83U, result),
                   result.Token == 1000U, result.ActualLength == 65536U, result.Status == In::TransferStatus::Success,
                   g_data[65535U] == 255U);
    In::EventBatch events{};
    ASTERIA_EXPECT(context, "composite mouse report becomes input events", controller.TakeCompletion(1U, 0x82U, result),
                   core.Complete(result.Token, result.Status, result.ActualLength, events), events.Count == 2U,
                   events.Events[0U].Kind == In::EventKind::Motion, events.Events[0U].Dx == 5,
                   events.Events[0U].Dy == -3, events.Events[1U].Kind == In::EventKind::Buttons);
    ASTERIA_EXPECT(context, "composite keyboard report becomes key events",
                   controller.TakeCompletion(1U, 0x81U, result),
                   core.Complete(result.Token, result.Status, result.ActualLength, events), events.Count == 2U,
                   events.Events[1U].Kind == In::EventKind::KeyDown, events.Events[1U].Usage == 4U,
                   events.Events[1U].Source == 37U, !core.IsOutstanding());
    request = Request(In::UsbOperation::BulkOut, 1001U, 3U);
    ASTERIA_EXPECT(context, "full 64KiB bulk OUT accepted", controller.Submit(1U, request));
    controller.Poll(++now);
    ASTERIA_EXPECT(context, "OUT completion routes to correct pipe", controller.TakeCompletion(1U, 3U, result),
                   result.ActualLength == 65536U, model.BulkOutBytes == 65536U, model.Good);
}
void BulkRecovery(TestContext& context, PipeFixture& fixture, UInt64& now) noexcept
{
    auto& controller = fixture.Base.Controller;
    auto& model = fixture.Base.Model;
    auto out = Request(In::UsbOperation::BulkOut, 1100U, 3U);
    model.StallNext = true;
    ASTERIA_EXPECT(context, "OUT stall submission", controller.Submit(1U, out));
    controller.Poll(++now);
    X::Completion result{};
    ASTERIA_EXPECT(context, "OUT stall reported unchanged", controller.TakeCompletion(1U, 3U, result),
                   result.Status == In::TransferStatus::Stall, model.EndpointResets == 0U, model.ClearHalts == 0U);
    const auto in = Request(In::UsbOperation::BulkIn, 1101U, 0x83U);
    ASTERIA_EXPECT(context, "other bulk pipe progresses through stall", controller.Submit(1U, in));
    controller.Poll(++now);
    ASTERIA_EXPECT(context, "IN unaffected by OUT device halt", controller.TakeCompletion(1U, 0x83U, result),
                   result.Status == In::TransferStatus::Success, model.Good);
    model.SuppressStoppedEvent = true;
    ASTERIA_EXPECT(context, "clear OUT device/host halt", controller.ClearHalt(1U, 3U, 1102U));
    for (Size i{0U}; i < 10U; ++i)
    {
        controller.Poll(++now);
    }
    ASTERIA_EXPECT(context, "clear targets OUT DCI/dequeue", controller.TakeCompletion(1U, 0U, result),
                   result.Token == 1102U, result.Status == In::TransferStatus::Success, model.ClearHalts == 1U,
                   model.EndpointResets == 1U, !model.Slots[0U].DeviceHalts[6U]);
    out.Token = 1103U;
    ASTERIA_EXPECT(context, "OUT reusable after explicit clear", controller.Submit(1U, out));
    controller.Poll(++now);
    ASTERIA_EXPECT(context, "OUT buffer retained intact", controller.TakeCompletion(1U, 3U, result),
                   result.Status == In::TransferStatus::Success, model.Good);
    model.HoldTransfers = true;
    out.Token = 1104U;
    out.TimeoutMilliseconds = 20U;
    ASTERIA_EXPECT(context, "finite OUT timeout accepted", controller.Submit(1U, out));
    now += 20U;
    controller.Poll(now, 0U);
    ASTERIA_EXPECT(context, "timeout retains borrow until stop/dequeue", !controller.TakeCompletion(1U, 3U, result));
    for (Size i{0U}; i < 10U; ++i)
    {
        controller.Poll(++now, 1U);
    }
    ASTERIA_EXPECT(context, "finite timeout retires only OUT pipe", controller.TakeCompletion(1U, 3U, result),
                   result.Status == In::TransferStatus::TimedOut, controller.GetState() == X::ControllerState::Running);
    model.HoldTransfers = false;
}
void PipeStateModel(TestContext& context, PipeFixture& fixture, UInt64& now) noexcept
{
    auto& controller = fixture.Base.Controller;
    auto& model = fixture.Base.Model;
    const auto seeds = Asteria::TestSupport::GetSeedRange(0x50495045U, 8U);
    UInt64 token{2000U};
    for (UInt64 seed{seeds.First}; seed - seeds.First < seeds.Count; ++seed)
    {
        Asteria::TestSupport::Random random{seed};
        // Reference: accepted requests/completions, one per each bulk pipe.
        bool pending[2U]{};
        bool ready[2U]{};
        UInt64 tokens[2U]{};
        for (Size step{0U}; step < 100U; ++step)
        {
            const Size before = context.GetFailureCount();
            const Size pipe = random.Next() & 1U;
            const UInt8 address = pipe == 0U ? 0x83U : 3U;
            const UInt32 operation = static_cast<UInt32>(random.Next() % 3U);
            if (operation == 0U)
            {
                model.HoldTransfers = true;
                auto request =
                    Request(pipe == 0U ? In::UsbOperation::BulkIn : In::UsbOperation::BulkOut, ++token, address);
                request.Length = 0U; // ZLP is a valid bulk transfer in either direction.
                const bool accepted = controller.Submit(1U, request);
                ASTERIA_EXPECT(context, "per-pipe admission matches reference",
                               accepted == (!pending[pipe] && !ready[pipe]));
                if (accepted)
                {
                    pending[pipe] = true;
                    tokens[pipe] = token;
                }
            }
            else if (operation == 1U)
            {
                model.HoldTransfers = false;
                if (pending[pipe])
                {
                    model.Transfers(1U, pipe == 0U ? 7U : 6U);
                    pending[pipe] = false;
                    ready[pipe] = true;
                }
                controller.Poll(++now);
            }
            else
            {
                X::Completion result{};
                const bool completed = controller.TakeCompletion(1U, address, result);
                ASTERIA_EXPECT(context, "retained completion matches reference", completed == ready[pipe]);
                if (completed)
                {
                    ASTERIA_EXPECT(context, "pipe token/length/status", result.Token == tokens[pipe],
                                   result.ActualLength == 0U, result.Status == In::TransferStatus::Success);
                    ready[pipe] = false;
                }
            }
            ASTERIA_EXPECT(context, "model resource state remains live", model.Good,
                           controller.GetState() == X::ControllerState::Running, model.Slots[0U].Enabled);
            if (context.GetFailureCount() != before)
            {
                static_cast<void>(
                    std::fprintf(stderr, "seed=%llu step=%zu pipe=%zu operation=%u\n", seed, step, pipe, operation));
                return;
            }
        }
        for (Size pipe{0U}; pipe < 2U; ++pipe)
        {
            if (pending[pipe])
            {
                model.Transfers(1U, pipe == 0U ? 7U : 6U);
            }
            controller.Poll(++now);
            X::Completion result{};
            static_cast<void>(controller.TakeCompletion(1U, pipe == 0U ? 0x83U : 3U, result));
        }
    }
}
void AbortAndDisconnect(TestContext& context, PipeFixture& fixture, UInt64& now) noexcept
{
    auto& controller = fixture.Base.Controller;
    auto& model = fixture.Base.Model;
    model.HoldTransfers = true;
    model.SuppressStoppedEvent = true;
    auto request = Request(In::UsbOperation::BulkIn, 10000U, 0x83U);
    ASTERIA_EXPECT(context, "abort test accepts IN", controller.Submit(1U, request));
    request.Operation = In::UsbOperation::BulkOut;
    request.Endpoint.Address = 3U;
    request.Token = 10001U;
    ASTERIA_EXPECT(context, "abort test accepts OUT", controller.Submit(1U, request), !controller.Abort(1U, 3U, 10000U),
                   controller.Abort(1U, 0x83U, 10000U));
    X::Completion result{};
    for (Size i{0U}; i < 10U; ++i)
    {
        controller.Poll(++now, 1U);
    }
    ASTERIA_EXPECT(context, "0.96 cancellation needs no forced stopped event",
                   controller.TakeCompletion(1U, 0x83U, result), result.Token == 10000U,
                   result.Status == In::TransferStatus::Cancelled, !controller.TakeCompletion(1U, 3U, result),
                   controller.GetState() == X::ControllerState::Running);
    model.HoldCommands = true;
    model.Disconnect(1U);
    controller.Poll(++now);
    ASTERIA_EXPECT(context, "disconnect parks every outstanding pipe until Disable Slot",
                   controller.GetPortStatus(1U).Connected, !controller.TakeCompletion(1U, 3U, result));
    model.HoldCommands = false;
    model.Commands();
    controller.Poll(++now);
    ASTERIA_EXPECT(context, "Disable Slot completes remaining pipe once", !controller.GetPortStatus(1U).Connected,
                   controller.TakeCompletion(1U, 3U, result), result.Token == 10001U,
                   result.Status == In::TransferStatus::Disconnected, !controller.TakeCompletion(1U, 3U, result),
                   model.Good);
}
} // namespace
int main()
{
    TestContext context{"xHCI endpoint pipes"};
    auto owner = std::make_unique<PipeFixture>();
    PipeFixture& fixture = *owner;
    fixture.Base.Model.Registers[0U] = (0x96U << 16U) | Test::FakeXhci::Operational;
    In::UsbDevice core{37U};
    UInt64 now{0U};
    Enumerate(context, fixture, core, now);
    ASTERIA_EXPECT(context, "HCIVERSION 0.96 retained", fixture.Base.Controller.Version() == 0x96U);
    ConfigureBulk(context, fixture, now);
    ReportsAndBulk(context, fixture, core, now);
    BulkRecovery(context, fixture, now);
    PipeStateModel(context, fixture, now);
    AbortAndDisconnect(context, fixture, now);
    ASTERIA_EXPECT(context, "quiet teardown", fixture.Base.Controller.Halt());
    return context.Finish();
}
