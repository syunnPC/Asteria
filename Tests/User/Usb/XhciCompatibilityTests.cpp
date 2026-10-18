// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Xhci.hpp"

#include "Support/TestContext.hpp"
#include "User/Usb/FakeXhci.hpp"

namespace
{
namespace X = Asteria::User::Usb;
namespace Test = Asteria::Test::Usb;
using Asteria::Foundation::UInt32;
using Asteria::TestSupport::TestContext;
void Legacy(TestContext& context, const bool timeout) noexcept
{
    Test::Fixture fixture;
    fixture.Model.Registers[0U] = (0x96U << 16U) | Test::FakeXhci::Operational;
    // Legacy + a vendor capability + Supported Protocol, all within BAR.
    fixture.Model.Registers[4U] = (0x18U << 16U) | 1U;
    fixture.Model.Registers[0x60U / 4U] = 0x10000U | (2U << 8U) | 1U;
    fixture.Model.Registers[0x64U / 4U] = 0xE011U | 8U;
    fixture.Model.Registers[0x68U / 4U] = (2U << 8U) | 0xC0U;
    fixture.Model.Registers[0x70U / 4U] = (2U << 24U) | 2U;
    fixture.Model.Registers[0x74U / 4U] = 0x20425355U;
    fixture.Model.Registers[0x78U / 4U] = 0x201U;
    fixture.Model.HoldBios = timeout;
    const UInt32 before = fixture.Model.Reads;
    ASTERIA_EXPECT(context, "bounded independent BIOS/OS handoff", fixture.Controller.Handoff(0x2100U, 4U) == !timeout,
                   fixture.Model.Reads - before < 30U, fixture.Model.Resets == 0U,
                   (fixture.Model.Registers[0x60U / 4U] & 0x01000000U) != 0U);
    if (timeout)
    {
        ASTERIA_EXPECT(context, "BIOS timeout permits neither reset nor initialization",
                       !fixture.Controller.Initialize(fixture.Memory, 0x2100U, 4U), fixture.Model.Resets == 0U,
                       (fixture.Model.Registers[0x60U / 4U] & 0x10000U) != 0U);
    }
    else
    {
        ASTERIA_EXPECT(context, "SMIs disabled and reserved field preserved",
                       (fixture.Model.Registers[0x64U / 4U] & 0xE011U) == 0U,
                       (fixture.Model.Registers[0x64U / 4U] & 8U) != 0U,
                       fixture.Controller.Initialize(fixture.Memory, 0x2100U), fixture.Controller.Version() == 0x96U,
                       fixture.Controller.Halt(), fixture.Model.Good);
    }
}
void Versions(TestContext& context) noexcept
{
    constexpr UInt32 versions[]{0x95U, 0x96U, 0x99U, 0x100U, 0x110U, 0x120U, 0x121U};
    for (const UInt32 version : versions)
    {
        constexpr bool contexts[]{false, true};
        for (const bool large : contexts)
        {
            Test::Fixture fixture{large};
            fixture.Model.Registers[0U] = (version << 16U) | Test::FakeXhci::Operational;
            const bool supported = (version == 0x96U) || ((version >= 0x100U) && (version <= 0x120U));
            ASTERIA_EXPECT(context, "HCIVERSION with both CSZ context sizes",
                           fixture.Controller.Initialize(fixture.Memory, 0x2100U) == supported);
            if (supported)
            {
                ASTERIA_EXPECT(context, "context owner quiet", fixture.Controller.Version() == version,
                               fixture.Controller.Halt(), fixture.Model.Good);
            }
        }
    }
}
void BulkContexts(TestContext& context) noexcept
{
    namespace In = Asteria::User::Input;
    using Asteria::Foundation::UInt8;
    using Asteria::Foundation::UInt16;
    using Asteria::Foundation::UInt64;
    constexpr bool speeds[]{false, true};
    for (const bool super : speeds)
    {
        Test::Fixture fixture;
        const UInt32 speedId = super ? 4U : 3U;
        fixture.Model.SetPort(1U, true, speedId);
        fixture.Model.SetPort(2U, true, speedId);
        if (super)
        {
            fixture.Model.Registers[0x40U / 4U] = (3U << 24U) | 2U;
        }
        ASTERIA_EXPECT(context, "HS/SS bulk controller initialized",
                       fixture.Controller.Initialize(fixture.Memory, 0x2100U));
        UInt64 now{0U};
        for (UInt8 port{1U}; port <= 2U; ++port)
        {
            In::UsbRequest request{.Operation = In::UsbOperation::ResetPort,
                                   .Token = 1U,
                                   .Speed = super ? In::UsbSpeed::Super : In::UsbSpeed::High,
                                   .Address = port,
                                   .PacketSize0 = super ? UInt16{512U} : UInt16{64U},
                                   .Setup = {},
                                   .Endpoint = {},
                                   .Data = nullptr,
                                   .Length = 0U};
            for (UInt32 operation{0U}; operation < 3U; ++operation)
            {
                ASTERIA_EXPECT(context, "bulk root/context operation accepted",
                               fixture.Controller.Submit(port, request));
                X::Completion result{};
                bool completed{false};
                for (UInt32 poll{0U}; poll < 50U && !completed; ++poll)
                {
                    fixture.Controller.Poll(++now);
                    completed = fixture.Controller.TakeCompletion(port, result);
                }
                ASTERIA_EXPECT(context, "bulk root/context operation completed", completed,
                               result.Status == In::TransferStatus::Success, fixture.Model.Good);
                ++request.Token;
                if (operation == 0U)
                {
                    request.Operation = In::UsbOperation::AssignAddress;
                }
                else
                {
                    request.Operation = In::UsbOperation::ConfigureEndpoint;
                    request.Endpoint = {.Address = port == 1U ? UInt8{0x81U} : UInt8{1U},
                                        .Attributes = 2U,
                                        .MaxPacketSize = super ? UInt16{1024U} : UInt16{512U},
                                        .Interval = super ? UInt8{0U} : UInt8{7U},
                                        .MaxBurst = super ? UInt8{3U} : UInt8{0U},
                                        .CompanionAttributes = 0U,
                                        .BytesPerInterval = 0U,
                                        .HasCompanion = super};
                }
            }
            const UInt32 interval = !super && (port == 2U) ? 7U : 0U;
            ASTERIA_EXPECT(context, "bulk payload/packet/burst/NAK interval", fixture.Model.LastPayload == 0U,
                           fixture.Model.LastPacket == (super ? 1024U : 512U), fixture.Model.LastInterval == interval,
                           fixture.Model.LastBurst == (super ? 3U : 0U));
        }
        ASTERIA_EXPECT(context, "bulk context cleanup", fixture.Controller.Halt());
    }
}
void PipeMemory(TestContext& context) noexcept
{
    for (UInt32 fault{0U}; fault < 5U; ++fault)
    {
        Test::Fixture fixture;
        switch (fault)
        {
        case 0U:
            fixture.Ports[0U].PipeCount = 0U;
            break;
        case 1U:
            fixture.Ports[0U].PipeCount = X::MaximumEndpointPipes + 1U;
            break;
        case 2U:
            fixture.Ports[0U].Pipes = nullptr;
            break;
        case 3U:
            fixture.EndpointMappings[0U][0U].Buffer = fixture.Ports[0U].Buffer;
            break;
        case 4U:
            fixture.EndpointMappings[0U][0U].Ring = fixture.Ports[0U].ControlRing;
            break;
        default:
            break;
        }
        ASTERIA_EXPECT(context, "pipe DMA/capacity refusal precedes reset",
                       !fixture.Controller.Initialize(fixture.Memory, 0x2100U), fixture.Model.Resets == 0U,
                       fixture.Model.DoorbellCount == 0U);
    }
}
} // namespace
int main()
{
    TestContext context{"xHCI 0.96 capabilities/handoff"};
    Legacy(context, false);
    Legacy(context, true);
    Versions(context);
    PipeMemory(context);
    BulkContexts(context);
    {
        Test::Fixture fixture;
        fixture.Model.Registers[4U] = (0x18U << 16U) | 1U;
        // Legacy next overlaps its control/status dword: refuse before ownership writes.
        fixture.Model.Registers[0x60U / 4U] = (1U << 8U) | 1U;
        ASTERIA_EXPECT(context, "overlapping extended capability", !fixture.Controller.Handoff(0x2100U),
                       fixture.Model.Resets == 0U, (fixture.Model.Registers[0x60U / 4U] & 0x01000000U) == 0U);
    }
    return context.Finish();
}
