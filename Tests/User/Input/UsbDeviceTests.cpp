// SPDX-License-Identifier: Apache-2.0
#include "User/Input/UsbDevice.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Input/ProtocolFixtures.hpp"

namespace
{
namespace In = Asteria::User::Input;
namespace Fx = Asteria::Test::Input;
// Project-owned USB bulk-only SCSI interface, with full-speed bulk IN/OUT.
constexpr In::UInt8 StorageConfiguration[]{
    9U,    2U, 32U, 0U, 1U,    1U, 0U,  0x80U, 50U, 9U, 4U, 0U,    0U, 2U,  8U, 6U,
    0x50U, 0U, 7U,  5U, 0x81U, 2U, 64U, 0U,    0U,  7U, 5U, 0x02U, 2U, 64U, 0U, 0U,
};
// USB HID 1.11 6.2.2, report-protocol relative Mouse, no report ID.
constexpr In::UInt8 RelativeDescriptor[]{0x05U, 1U,    0x09U, 2U, 0xA1U, 1U, 0x05U, 9U,    0x19U, 1U,    0x29U, 3U,
                                         0x15U, 0U,    0x25U, 1U, 0x75U, 1U, 0x95U, 3U,    0x81U, 2U,    0x75U, 5U,
                                         0x95U, 1U,    0x81U, 1U, 0x05U, 1U, 0x09U, 0x30U, 0x09U, 0x31U, 0x15U, 0x81U,
                                         0x25U, 0x7FU, 0x75U, 8U, 0x95U, 2U, 0x81U, 6U,    0xC0U};
class FakeHost final : public In::IUsbHostController
{
public:
    In::PortStatus Port{.Connected = true, .Speed = In::UsbSpeed::Full};
    In::UsbRequest Request{};
    In::Size Submissions{0U};
    bool Refuse{false};
    bool Storage{false};
    bool Relative{false};
    In::Size ProtocolRequests{0U};
    [[nodiscard]] In::PortStatus GetPortStatus() noexcept override
    {
        return Port;
    }
    [[nodiscard]] bool Submit(const In::UsbRequest& request) noexcept override
    {
        Request = request;
        if (request.TimeoutMilliseconds != (request.Operation == In::UsbOperation::InterruptIn ? 0U : 1000U))
        {
            return false;
        }
        ++Submissions;
        if (Request.Setup.Bytes[1U] == 11U)
        {
            ++ProtocolRequests;
        }
        return !Refuse;
    }
    [[nodiscard]] bool Abort(Asteria::Foundation::UInt64 token) noexcept override
    {
        return Request.Token == token;
    }
    [[nodiscard]] In::Size Fill(const bool mouse) noexcept
    {
        const auto& request = Request;
        if ((request.Operation == In::UsbOperation::Control) && (request.Setup.Bytes[1U] == 6U))
        {
            if (Relative && request.Setup.Bytes[3U] == 0x22U)
            {
                std::memcpy(request.Data, RelativeDescriptor, sizeof(RelativeDescriptor));
                return sizeof(RelativeDescriptor);
            }
            if (Storage && (request.Setup.Bytes[3U] == 2U))
            {
                const In::Size actual =
                    request.Length < sizeof(StorageConfiguration) ? request.Length : sizeof(StorageConfiguration);
                std::memcpy(request.Data, StorageConfiguration, actual);
                return actual;
            }
            const In::UInt8* bytes = request.Setup.Bytes[3U] == 1U
                                         ? (mouse ? Fx::MouseDevice : Fx::KeyboardDevice)
                                         : (mouse ? Fx::MouseConfiguration : Fx::KeyboardConfiguration);
            const bool device = request.Setup.Bytes[3U] == 1U;
            const In::Size total = !device && (Port.Speed == In::UsbSpeed::Super) ? 40U : request.Length;
            std::memcpy(request.Data, bytes, device ? request.Length : (request.Length < 34U ? request.Length : 34U));
            if (Relative && !device && request.Length >= 34U)
            {
                request.Data[15U] = 0U;
                request.Data[16U] = 0U;
                request.Data[25U] = sizeof(RelativeDescriptor);
                request.Data[26U] = 0U;
            }
            if (device && (request.Length >= 8U))
            {
                request.Data[7U] =
                    Port.Speed == In::UsbSpeed::Super ? 9U : (Port.Speed == In::UsbSpeed::High ? 64U : 8U);
            }
            if (!device && (Port.Speed == In::UsbSpeed::Super))
            {
                request.Data[2U] = 40U;
                if (request.Length == total)
                {
                    constexpr In::UInt8 companion[]{6U, 0x30U, 0U, 0U, 8U, 0U};
                    std::memcpy(request.Data + 34U, companion, 6U);
                }
            }
            if (!device && (request.Length >= 34U) &&
                ((Port.Speed == In::UsbSpeed::High) || (Port.Speed == In::UsbSpeed::Super)))
            {
                request.Data[33U] = 7U;
            }
            return request.Length;
        }
        if (request.Operation == In::UsbOperation::InterruptIn)
        {
            const In::UInt8* bytes = mouse ? Fx::UsbMouseReport : Fx::UsbKeyboardReport;
            const In::Size length = mouse ? 4U : 8U;
            std::memcpy(request.Data, bytes, length);
            return length;
        }
        return 0U;
    }
};
In::UsbDevice g_device{17U};
In::EventBatch g_output{};
constexpr In::UsbState States[]{
    In::UsbState::Reset,         In::UsbState::FirstDescriptor,  In::UsbState::EndpointZero,
    In::UsbState::Address,       In::UsbState::DeviceDescriptor, In::UsbState::ConfigurationHeader,
    In::UsbState::Configuration, In::UsbState::SetConfiguration, In::UsbState::SetProtocol,
    In::UsbState::SetIdle,       In::UsbState::ConfigureEndpoint};
void Enumerate(Asteria::TestSupport::TestContext& context, FakeHost& host, const bool mouse)
{
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "valid caller address", g_device.Attach(7U));
    for (const auto state : States)
    {
        ASTERIA_EXPECT(context, "enumeration state order", g_device.GetState() == state);
        g_device.Step(host);
        const In::Size count = host.Submissions;
        g_device.Step(host);
        ASTERIA_EXPECT(context, "one outstanding", g_device.IsOutstanding(), host.Submissions == count);
        if (state == In::UsbState::Address)
        {
            ASTERIA_EXPECT(context, "controller-owned addressing seam",
                           host.Request.Operation == In::UsbOperation::AssignAddress,
                           host.Request.Setup.Bytes[1U] == 5U, host.Request.Setup.Bytes[2U] == 7U,
                           host.Request.PacketSize0 == (host.Port.Speed == In::UsbSpeed::Super
                                                            ? 512U
                                                            : (host.Port.Speed == In::UsbSpeed::High ? 64U : 8U)));
        }
        if (state == In::UsbState::ConfigureEndpoint)
        {
            ASTERIA_EXPECT(
                context, "endpoint context seam", host.Request.Endpoint.Address == 0x81U,
                host.Request.Endpoint.Interval ==
                    ((host.Port.Speed == In::UsbSpeed::High || host.Port.Speed == In::UsbSpeed::Super) ? 7U : 10U),
                host.Request.Endpoint.MaxPacketSize == (mouse ? 4U : 8U));
        }
        ASTERIA_EXPECT(context, "completion advances",
                       g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(mouse), g_output),
                       !g_device.IsOutstanding(), g_output.Count == 0U);
    }
    ASTERIA_EXPECT(context, "enumerated", g_device.GetState() == In::UsbState::Polling);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext context{"UsbDevice"};
    FakeHost host{};
    host.Relative = true;
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "report-protocol mouse attached", g_device.Attach(1U));
    for (In::Size step{0U}; step < 16U && g_device.GetState() != In::UsbState::Polling; ++step)
    {
        g_device.Step(host);
        ASTERIA_EXPECT(context, "report descriptor enumerated", g_device.IsOutstanding(),
                       g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(true), g_output));
    }
    ASTERIA_EXPECT(context, "generic relative mouse polls without SET_PROTOCOL",
                   g_device.GetState() == In::UsbState::Polling, host.ProtocolRequests == 0U);
    g_device.Step(host);
    ASTERIA_EXPECT(context, "relative report reaches the shared event seam",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(true), g_output),
                   g_output.Count == 2U, g_output.Events[0U].Dx == 5, g_output.Events[0U].Dy == -3,
                   g_output.Events[1U].Buttons == 1U);
    host.Relative = false;
    constexpr In::UInt8 unsupportedClasses[]{0U, 9U, 0xFFU};
    for (const In::UInt8 deviceClass : unsupportedClasses)
    {
        g_device.Disconnect();
        host.Storage = deviceClass == 0U;
        host.Submissions = 0U;
        ASTERIA_EXPECT(context, "attach unsupported root device", g_device.Attach(7U));
        for (const auto state : States)
        {
            if (g_device.GetState() == In::UsbState::Failed)
            {
                break;
            }
            ASTERIA_EXPECT(context, "unsupported device stops before configuration",
                           state != In::UsbState::SetConfiguration);
            g_device.Step(host);
            const In::Size length = host.Fill(false);
            if (state == In::UsbState::DeviceDescriptor)
            {
                host.Request.Data[4U] = deviceClass;
            }
            if ((state == In::UsbState::Configuration) && !host.Storage)
            {
                // A project-owned descriptor mutation models a non-HID interface.
                host.Request.Data[14U] = deviceClass == 0U ? 8U : deviceClass;
                host.Request.Data[15U] = 6U;
                host.Request.Data[16U] = 0x50U;
            }
            ASTERIA_EXPECT(context, "unsupported enumeration accepts its completion",
                           g_device.Complete(host.Request.Token, In::TransferStatus::Success, length, g_output));
        }
        const auto submissions = host.Submissions;
        for (In::Size step{0U}; step < 32U; ++step)
        {
            g_device.Step(host);
        }
        ASTERIA_EXPECT(context, "mass storage, root hub and vendor device stay unconfigured and idle",
                       g_device.GetState() == In::UsbState::Failed, !g_device.IsOutstanding(),
                       g_device.InterfaceCount() == 0U, g_output.Count == 0U, host.Submissions == submissions);
    }
    host.Storage = false;
    Enumerate(context, host, false);
    g_device.Step(host);
    ASTERIA_EXPECT(context, "keyboard holds the report before unplug",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(false), g_output),
                   g_output.Count != 0U);
    const In::Size held = g_output.Count;
    g_device.ReleaseKeys(0U, g_output);
    ASTERIA_EXPECT(context, "unplug emits every held key release", g_output.Count == held);
    for (In::Size index{0U}; index < g_output.Count; ++index)
    {
        ASTERIA_EXPECT(context, "unplug releases carry source and clear modifiers",
                       g_output.Events[index].Kind == In::EventKind::KeyUp, g_output.Events[index].Source == 17U,
                       g_output.Events[index].Modifiers == 0U);
    }
    g_device.ReleaseKeys(0U, g_output);
    ASTERIA_EXPECT(context, "repeated device-gone notification is harmless", g_output.Count == 0U);
    g_device.ReleaseKeys(99U, g_output);
    ASTERIA_EXPECT(context, "unknown interface emits no releases", g_output.Count == 0U);
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "invalid addresses", !g_device.Attach(0U), !g_device.Attach(128U));
    Enumerate(context, host, false);
    g_device.Step(host);
    ASTERIA_EXPECT(context, "keyboard report",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(false), g_output),
                   g_output.Count == 2U, g_output.Events[1U].Usage == 4U, g_output.Events[1U].Source == 17U);
    g_device.Step(host);
    ASTERIA_EXPECT(context, "repeat report dropped",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(false), g_output),
                   g_output.Count == 0U);
    g_device.Step(host);
    const auto stale = host.Request.Token;
    host.Port.Connected = false;
    g_device.Step(host);
    ASTERIA_EXPECT(context, "port disconnect resets", g_device.GetState() == In::UsbState::Detached,
                   !g_device.IsOutstanding(), !g_device.Complete(stale, In::TransferStatus::Success, 8U, g_output));
    host.Port.Connected = true;
    Enumerate(context, host, true);
    g_device.Step(host);
    ASTERIA_EXPECT(context, "mouse report",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(true), g_output),
                   g_output.Count == 2U, g_output.Events[0U].Dy == -3);
    ASTERIA_EXPECT(context, "completion disconnect",
                   !g_device.Complete(stale, In::TransferStatus::Disconnected, 0U, g_output));
    g_device.Step(host);
    ASTERIA_EXPECT(context, "polling stall is reportable",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Stall, 0U, g_output),
                   g_device.GetState() == In::UsbState::Polling);
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "reattach", g_device.Attach(7U));
    for (In::Size i = 0U; i < 3U; ++i)
    {
        g_device.Step(host);
        ASTERIA_EXPECT(context, "retry completion",
                       g_device.Complete(host.Request.Token, In::TransferStatus::Retryable, 0U, g_output),
                       g_device.GetState() == (i == 2U ? In::UsbState::Failed : In::UsbState::Reset));
    }
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "attach after exhaustion", g_device.Attach(8U));
    host.Refuse = true;
    for (In::Size i = 0U; i < 3U; ++i)
    {
        g_device.Step(host);
    }
    ASTERIA_EXPECT(context, "submission refusal also bounded", g_device.GetState() == In::UsbState::Failed);
    host.Refuse = false;
    Enumerate(context, host, false);
    g_device.Step(host);
    ASTERIA_EXPECT(
        context, "actual length guarded",
        g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Request.Length + 1U, g_output),
        g_device.GetState() == In::UsbState::Failed);
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "attach malformed", g_device.Attach(9U));
    g_device.Step(host);
    ASTERIA_EXPECT(context, "reset completes",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, 0U, g_output));
    g_device.Step(host);
    static_cast<void>(host.Fill(false));
    ASTERIA_EXPECT(context, "short first descriptor",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, 7U, g_output),
                   g_device.GetState() == In::UsbState::Failed);
    constexpr In::UsbSpeed speeds[]{In::UsbSpeed::Low, In::UsbSpeed::Full, In::UsbSpeed::High, In::UsbSpeed::Super};
    for (const In::UsbSpeed speed : speeds)
    {
        host.Port.Speed = speed;
        Enumerate(context, host, false);
        g_device.Step(host);
        ASTERIA_EXPECT(context, "reports at every speed",
                       g_device.Complete(host.Request.Token, In::TransferStatus::Success, host.Fill(false), g_output),
                       g_output.Count == 2U);
    }
    host.Port.Speed = In::UsbSpeed::Full;
    // Speed becomes known on reset completion (xHCI root-port negotiation).
    g_device.Disconnect();
    ASTERIA_EXPECT(context, "negotiation attach", g_device.Attach(3U));
    g_device.Step(host);
    ASTERIA_EXPECT(context, "reset speed pending",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Success, 0U, g_output));
    host.Port.Speed = In::UsbSpeed::High;
    g_device.Step(host);
    ASTERIA_EXPECT(context, "negotiated speed updates default pipe", host.Request.Speed == In::UsbSpeed::High,
                   host.Request.PacketSize0 == 64U, host.Request.Address == 0U);
    ASTERIA_EXPECT(context, "disconnect completion releases borrow",
                   g_device.Complete(host.Request.Token, In::TransferStatus::Disconnected, 0U, g_output),
                   g_device.GetState() == In::UsbState::Detached);
    host.Port.Speed = In::UsbSpeed::Full;
    // Seeded model runs vary transient errors, stall/disconnect and retries at
    // each state. The expected transition list is independent of core dispatch.
    const auto seeds = Asteria::TestSupport::GetSeedRange(290U, 12U);
    for (In::Size seedAt = 0U; seedAt < seeds.Count; ++seedAt)
    {
        const auto seed = seeds.First + seedAt;
        Asteria::TestSupport::Random random{seed};
        for (In::Size run = 0U; run < 40U; ++run)
        {
            g_device.Disconnect();
            ASTERIA_EXPECT(context, "model attach", g_device.Attach(7U));
            const bool mouse = random.Chance(1U, 2U);
            In::Size stage = 0U;
            In::Size attempts = 0U;
            In::UsbState expected = States[0U];
            for (In::Size step = 0U; step < 80U; ++step)
            {
                if ((expected == In::UsbState::Failed) || (expected == In::UsbState::Detached))
                {
                    break;
                }
                g_device.Step(host);
                ++attempts;
                const auto choice = random.Below(20U);
                In::TransferStatus status = In::TransferStatus::Success;
                if (choice < 4U)
                {
                    status = In::TransferStatus::Retryable;
                }
                else if (choice == 4U)
                {
                    status = In::TransferStatus::Stall;
                }
                else if (choice == 5U)
                {
                    status = In::TransferStatus::Disconnected;
                }
                else if (choice == 6U)
                {
                    status = In::TransferStatus::Error;
                }
                const In::Size length = status == In::TransferStatus::Success ? host.Fill(mouse) : 0U;
                if (status == In::TransferStatus::Disconnected)
                {
                    expected = In::UsbState::Detached;
                }
                else if ((status == In::TransferStatus::Stall) && (expected == In::UsbState::SetIdle))
                {
                    // SET_IDLE is optional for boot devices; a single-interface
                    // device's stalled SET_PROTOCOL drops its only interface.
                    attempts = 0U;
                    ++stage;
                    expected = States[stage];
                }
                else if (status == In::TransferStatus::Stall && expected == In::UsbState::SetProtocol && mouse)
                {
                    attempts = 0U;
                    expected = In::UsbState::ReportDescriptor;
                }
                else if (((status == In::TransferStatus::Stall) && (expected != In::UsbState::Polling)) ||
                         (status == In::TransferStatus::Error) ||
                         ((status == In::TransferStatus::Retryable) && (attempts == 3U)))
                {
                    expected = In::UsbState::Failed;
                }
                else if (status == In::TransferStatus::Stall)
                {
                    attempts = 0U;
                }
                else if (status == In::TransferStatus::Success)
                {
                    attempts = 0U;
                    if (expected == In::UsbState::ReportDescriptor)
                    {
                        expected = In::UsbState::Failed;
                    } // fake descriptor is not a relative Mouse
                    else
                    {
                        if (stage < 11U)
                        {
                            ++stage;
                        }
                        expected = stage < 11U ? States[stage] : In::UsbState::Polling;
                    }
                }
                const bool accepted = g_device.Complete(host.Request.Token, status, length, g_output);
                if (!accepted || (g_device.GetState() != expected) || g_device.IsOutstanding())
                {
                    static_cast<void>(std::fprintf(stderr, "seed=%llu run=%zu step=%zu status=%u\n",
                                                   static_cast<unsigned long long>(seed), run, step,
                                                   static_cast<unsigned int>(status)));
                    ASTERIA_FAIL(context, "USB enumeration model");
                    break;
                }
            }
        }
    }
    return context.Finish();
}
