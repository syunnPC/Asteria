// SPDX-License-Identifier: Apache-2.0
#include "User/Usb/Xhci.hpp"

#include <memory>

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
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::TestSupport::TestContext;

void RootOperation(TestContext& context, Test::Fixture& fixture, const UInt8 number, const In::UsbSpeed speed,
                   UInt64& now) noexcept
{
    In::UsbRequest request{.Operation = In::UsbOperation::ResetPort,
                           .Token = now + 1U,
                           .Speed = speed,
                           .Address = 1U,
                           .PacketSize0 = speed == In::UsbSpeed::Super ? Asteria::Foundation::UInt16{512U}
                                                                       : Asteria::Foundation::UInt16{8U},
                           .Setup = {},
                           .Endpoint = {},
                           .Data = nullptr,
                           .Length = 0U};
    ASTERIA_EXPECT(context, "attached high root submits reset", fixture.Controller.Submit(number, request));
    X::Completion completion{};
    bool completed{false};
    for (Size poll{0U}; (poll < 40U) && !completed; ++poll)
    {
        fixture.Controller.Poll(++now);
        completed = fixture.Controller.TakeCompletion(number, completion);
    }
    ASTERIA_EXPECT(context, "high root owns a slot/context independent of its number", completed,
                   completion.Status == In::TransferStatus::Success, fixture.Model.Good);
}
void ManyPorts(TestContext& context) noexcept
{
    Test::Fixture fixture{true, 0U, 0x1'0000'0000ULL};
    struct EventStorage
    {
        UInt8 Bytes[8192U]{};
    };
    auto events = std::make_unique<EventStorage>();
    fixture.Memory.EventRing = fixture.Map(0x3'0000'0000ULL, events->Bytes, sizeof(events->Bytes));
    fixture.Model.Registers[1U] = (255U << 24U) | (1U << 8U) | 2U;
    fixture.Model.SetPort(1U, false, 0U);
    fixture.Model.SetPort(2U, false, 0U);
    fixture.Model.SetPort(128U, true, 1U);
    fixture.Model.SetPort(255U, true, 4U);
    fixture.Model.Registers[0x40U / 4U] = (2U << 24U) | (4U << 8U) | 2U;
    fixture.Model.Registers[0x48U / 4U] = (128U << 8U) | 1U;
    fixture.Model.Registers[0x50U / 4U] = (3U << 24U) | 2U;
    fixture.Model.Registers[0x54U / 4U] = 0x20425355U;
    fixture.Model.Registers[0x58U / 4U] = (127U << 8U) | 129U;
    ASTERIA_EXPECT(context, "255 roots, two slots, CSZ64 and 64-bit DMA initialize",
                   fixture.Controller.Initialize(fixture.Memory, 0x2100U, 100U, X::EventDelivery::Poll),
                   fixture.Controller.PortCount() == 255U, fixture.Controller.DeviceIndex(128U) == 0U,
                   fixture.Controller.DeviceIndex(255U) == 1U, fixture.Controller.DeviceIndex(1U) == X::MaximumDevices,
                   fixture.Model.Read32(Test::FakeXhci::Operational + 0x38U) == 2U,
                   fixture.Model.Read64(Test::FakeXhci::Operational + 0x30U) == fixture.Memory.Dcbaa.Address);
    UInt64 now{0U};
    for (UInt32 number{1U}; number <= 255U; ++number)
    {
        fixture.Model.Emit({.Parameter = UInt64{number} << 24U, .Status = 1U << 24U, .Control = 34U << 10U});
    }
    for (Size poll{0U}; poll < 4U; ++poll)
    {
        fixture.Controller.Poll(++now);
    }
    ASTERIA_EXPECT(context, "a change on every root fits and drains without losing high-port events",
                   fixture.Controller.GetState() == X::ControllerState::Running, fixture.Model.Good,
                   fixture.Model.EventRingFullCount == 0U);
    RootOperation(context, fixture, 128U, In::UsbSpeed::Full, now);
    ASTERIA_EXPECT(context, "USB2 root reset requested without PED or W1C pollution", fixture.Model.PortResets == 1U,
                   (fixture.Model.LastReset & 0xFE0002U) == 0U);
    RootOperation(context, fixture, 255U, In::UsbSpeed::Super, now);
    ASTERIA_EXPECT(context, "USB3 automatic training does not request a warm reset", fixture.Model.PortResets == 1U);
    const Size oldIndex = fixture.Controller.DeviceIndex(128U);
    ASTERIA_EXPECT(context, "live slot cannot be recycled", !fixture.Controller.ReleasePort(128U));
    fixture.Model.Disconnect(128U);
    fixture.Controller.Poll(++now);
    fixture.Controller.Poll(++now);
    X::Completion completion{};
    while (fixture.Controller.TakeCompletion(128U, completion))
    {
    }
    ASTERIA_EXPECT(context, "disable slot and completion drain release the device pool",
                   fixture.Controller.ReleasePort(128U));
    fixture.Model.SetPort(127U, true, 1U);
    fixture.Controller.Poll(++now);
    ASTERIA_EXPECT(context, "another high root reuses the released pool entry",
                   fixture.Controller.DeviceIndex(127U) == oldIndex, fixture.Controller.GetPortStatus(127U).Connected,
                   fixture.Controller.Halt(), fixture.Model.Good);
}
struct ScratchStorage
{
    UInt8 Array[8192U]{};
    UInt8 Pages[X::MaximumScratchpads][4096U]{};
    X::DmaMemory Descriptors[X::MaximumScratchpads]{};
};
void ManyScratchpads(TestContext& context) noexcept
{
    constexpr UInt32 counts[]{17U, 256U, 1023U};
    for (const UInt32 count : counts)
    {
        Test::Fixture fixture{true, count};
        auto storage = std::make_unique<ScratchStorage>();
        fixture.Memory.ScratchpadArray = fixture.Map(0x2'0000'0000ULL, storage->Array, sizeof(storage->Array));
        fixture.Memory.Scratchpads = storage->Descriptors;
        for (Size page{0U}; page < count; ++page)
        {
            storage->Pages[page][0U] = 0xA5U;
            storage->Descriptors[page] = fixture.Map(0x2'0001'0000ULL + page * 4096U, storage->Pages[page], 4096U);
        }
        ASTERIA_EXPECT(context, "full 10-bit scratchpad count accepted",
                       fixture.Controller.Initialize(fixture.Memory, 0x2100U),
                       X::Load64(fixture.Dcbaa) == fixture.Memory.ScratchpadArray.Address);
        for (Size page{0U}; page < count; ++page)
        {
            ASTERIA_EXPECT(context, "every requested scratchpad is cleared and linked by its full address",
                           X::Load64(storage->Array + page * 8U) == storage->Descriptors[page].Address,
                           storage->Pages[page][0U] == 0U);
        }
        ASTERIA_EXPECT(context, "scratchpad DMA quiet", fixture.Controller.Halt(), fixture.Model.Good);
    }
}
void FirmwareState(TestContext& context) noexcept
{
    for (UInt32 state{0U}; state < 4U; ++state)
    {
        Test::Fixture fixture;
        const bool cnr = state >= 2U;
        fixture.Model.Registers[Test::FakeXhci::Operational / 4U] = (state & 1U) != 0U ? 1U : 0U;
        fixture.Model.Registers[Test::FakeXhci::Operational / 4U + 1U] =
            ((state & 1U) != 0U ? 0U : 1U) | (cnr ? 0x800U : 0U);
        ASTERIA_EXPECT(context, "firmware schedule stops before replacing its IOMMU mappings",
                       fixture.Controller.Handoff(0x2100U, 4U), fixture.Controller.QuiesceFirmware(0x2100U, 4U) == !cnr,
                       fixture.Model.Resets == (cnr ? 0U : 1U));
        ASTERIA_EXPECT(context, "firmware running/halted state reset only after CNR clears",
                       fixture.Controller.Initialize(fixture.Memory, 0x2100U, 4U) == !cnr,
                       fixture.Model.Resets == (cnr ? 0U : 2U));
        if (!cnr)
        {
            ASTERIA_EXPECT(context, "firmware rings replaced", fixture.Controller.Halt(), fixture.Model.Good);
        }
    }
    constexpr UInt32 pageSizes[]{0U, 2U, 3U, 0xFFFF'FFFFU};
    for (const UInt32 pages : pageSizes)
    {
        Test::Fixture fixture;
        fixture.Model.Registers[Test::FakeXhci::Operational / 4U + 2U] = pages;
        ASTERIA_EXPECT(context, "unsupported page size rejected before reset",
                       !fixture.Controller.Initialize(fixture.Memory, 0x2100U), fixture.Model.Resets == 0U);
    }
}
void SpeedIds(TestContext& context) noexcept
{
    Test::Fixture fixture;
    fixture.Model.Registers[0x48U / 4U] |= 1U << 28U;
    // USB2 full-speed is explicitly mapped to non-default PSIV 7.
    fixture.Model.Registers[0x50U / 4U] = (12U << 16U) | (2U << 4U) | 7U;
    fixture.Model.SetPort(1U, true, 7U);
    fixture.Model.SetPort(2U, false, 0U);
    ASTERIA_EXPECT(context, "protocol PSI replaces implied speed IDs",
                   fixture.Controller.Initialize(fixture.Memory, 0x2100U),
                   fixture.Controller.GetPortStatus(1U).Speed == In::UsbSpeed::Full);
    UInt64 now{0U};
    RootOperation(context, fixture, 1U, In::UsbSpeed::Full, now);
    ASTERIA_EXPECT(context, "negotiated PSI speed ID enters Slot Context",
                   ((X::Load32(fixture.Inputs[0U] + 32U) >> 20U) & 15U) == 7U, fixture.Controller.Halt(),
                   fixture.Model.Good);
}
void DevicePoolModel(TestContext& context) noexcept
{
    const auto seeds = Asteria::TestSupport::GetSeedRange(1U, 16U);
    constexpr UInt8 numbers[]{1U, 2U, 17U, 33U, 128U, 254U, 255U};
    for (UInt64 seed{seeds.First}; seed - seeds.First < seeds.Count; ++seed)
    {
        auto fixture = std::make_unique<Test::Fixture>();
        fixture->Model.Registers[1U] = (255U << 24U) | (1U << 8U) | 2U;
        fixture->Model.Registers[0x48U / 4U] = (255U << 8U) | 1U;
        fixture->Model.SetPort(1U, false, 0U);
        fixture->Model.SetPort(2U, false, 0U);
        if (!ASTERIA_EXPECT(context, "pool model initializes",
                            fixture->Controller.Initialize(fixture->Memory, 0x2100U)))
        {
            return;
        }
        bool connected[255U]{};
        UInt8 owners[2U]{};
        Asteria::TestSupport::Random random{seed};
        for (UInt64 step{0U}; step < 128U; ++step)
        {
            const UInt8 number = numbers[random.Below(sizeof(numbers))];
            const bool release = random.Chance(1U, 3U);
            bool result{true};
            if (release)
            {
                bool expected{false};
                for (UInt8& owner : owners)
                {
                    if ((owner == number) && !connected[number - 1U])
                    {
                        owner = 0U;
                        expected = true;
                    }
                }
                result = fixture->Controller.ReleasePort(number) == expected;
            }
            else
            {
                connected[number - 1U] = !connected[number - 1U];
                fixture->Model.SetPort(number, connected[number - 1U], 1U);
            }
            fixture->Controller.Poll(step + 1U);
            for (UInt32 root{1U}; root <= 255U; ++root)
            {
                const bool assigned = (owners[0U] == root) || (owners[1U] == root);
                if (connected[root - 1U] && !assigned)
                {
                    for (UInt8& owner : owners)
                    {
                        if (owner == 0U)
                        {
                            owner = static_cast<UInt8>(root);
                            break;
                        }
                    }
                }
                Size index{X::MaximumDevices};
                for (Size entry{0U}; entry < 2U; ++entry)
                {
                    if (owners[entry] == root)
                    {
                        index = entry;
                    }
                }
                result = result && (fixture->Controller.DeviceIndex(static_cast<UInt8>(root)) == index) &&
                         (fixture->Controller.GetPortStatus(static_cast<UInt8>(root)).Connected ==
                          ((index != X::MaximumDevices) && connected[root - 1U]));
            }
            if (!ASTERIA_EXPECT(context, "attached-device pool matches reference ownership", result,
                                fixture->Model.Good))
            {
                static_cast<void>(std::fprintf(
                    stderr, "seed=%llu step=%llu root=%u operation=%s\n", static_cast<unsigned long long>(seed),
                    static_cast<unsigned long long>(step), number, release ? "release" : "toggle"));
                return;
            }
        }
        ASTERIA_EXPECT(context, "pool model halts", fixture->Controller.Halt());
    }
}
} // namespace
int main()
{
    TestContext context{"XhciIntelTests"};
    ManyPorts(context);
    ManyScratchpads(context);
    FirmwareState(context);
    SpeedIds(context);
    DevicePoolModel(context);
    return context.Finish();
}
