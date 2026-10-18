// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_FAKEOHCI_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_FAKEOHCI_HPP_INCLUDED

#include "User/Usb/Ohci.hpp"

#include "User/Usb/FakeUsb1.hpp"

namespace Asteria::Test::Usb
{
// OpenHCI 1.0a sections 4/6; QEMU hcd-ohci.c service_ed_list/service_td and
// frame_boundary. Fetches exclusively device addresses and MMIO registers.
class FakeOhci final : public U::IOhciMmio, public Usb1Mappings
{
public:
    FakeOhci() noexcept
    {
        Registers[0U] = 0x10U;
        Registers[1U] = 0x100U;
        Registers[0x34U / 4U] = 11999U;
        Registers[0x48U / 4U] = 2U | (2U << 24U);
        Devices[1U].Mouse = true;
    }
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        if ((offset >= sizeof(Registers)) || ((offset & 3U) != 0U))
        {
            Good = false;
            return 0U;
        }
        if (offset == 0x30U)
        {
            return m_done;
        }
        if (offset == 4U)
        {
            ++OwnershipReads;
            if (Requested && !HoldBios)
            {
                Registers[1U] &= ~0x100U;
            }
        }
        if ((offset == 0x54U) || (offset == 0x58U))
        {
            const Size port = (offset - 0x54U) / 4U;
            UInt32 value = Registers[offset / 4U] & ~UInt32{0x203U};
            value |= Devices[port].Connected ? 1U : 0U;
            value |= Devices[port].Enabled ? 2U : 0U;
            value |= Devices[port].Low ? 0x200U : 0U;
            return value;
        }
        return Registers[offset / 4U];
    }
    void Write32(const UInt32 offset, const UInt32 value) noexcept override
    {
        if ((offset >= sizeof(Registers)) || ((offset & 3U) != 0U))
        {
            Good = false;
            return;
        }
        ++Writes;
        if (offset == 8U)
        {
            if ((value & 8U) != 0U)
            {
                Requested = true;
            }
            if ((value & 1U) != 0U)
            {
                ++Resets;
                Good = Good && ((Registers[1U] & 0x100U) == 0U);
                if (HoldReset)
                {
                    Registers[2U] |= 1U;
                    return;
                }
                Registers[1U] = 0U;
                Registers[2U] = 0U;
                Registers[0x18U / 4U] = 0U;
                Registers[0x3CU / 4U] = 0U;
                m_done = 0U;
                PendingTd = 0U;
                return;
            }
            Registers[2U] |= value & 6U;
            return;
        }
        if ((offset == 0x54U) || (offset == 0x58U))
        {
            const Size port = (offset - 0x54U) / 4U;
            UInt32& status = Registers[offset / 4U];
            status &= ~(value & 0x1F0000U);
            if ((value & 0x100U) != 0U)
            {
                ++PowerWrites;
                status |= 0x100U;
            }
            if ((value & 16U) != 0U)
            {
                ++PortResets;
                if (HoldPortReset)
                {
                    status |= 16U;
                }
                else
                {
                    Devices[port].Reset();
                    status = (status & ~UInt32{16U}) | (1U << 20U);
                }
            }
            return;
        }
        if (offset == 0xCU)
        {
            Registers[offset / 4U] &= ~value;
            return;
        }
        if (offset == 0x10U)
        {
            Registers[offset / 4U] |= value;
            return;
        }
        if (offset == 0x14U)
        {
            Registers[0x10U / 4U] &= ~value;
            return;
        }
        Registers[offset / 4U] = value;
    }
    void PublishDma() noexcept override
    {
        ++Publishes;
    }
    void ObserveDma() noexcept override
    {
        ++Observes;
    }
    void LateDone(const UInt32 td) noexcept
    {
        m_done = td;
        Registers[3U] |= 2U; // previous WDH still owned by software
    }
    void HostError() noexcept
    {
        Registers[0xCU / 4U] |= 16U;
    }
    void Tick() noexcept
    {
        ++Frames;
        if (Freeze || ((Registers[1U] & 0xC0U) != 0x80U))
        {
            return;
        }
        UInt8* const hcca = Translate(Registers[0x18U / 4U], 256U);
        if (hcca == nullptr)
        {
            return;
        }
        if ((Registers[1U] & 4U) != 0U)
        {
            Service(U::Load32(hcca + (Registers[0x3CU / 4U] & 31U) * 4U));
        }
        if (((Registers[1U] & 16U) != 0U) && ((Registers[2U] & 2U) != 0U))
        {
            Service(Registers[0x20U / 4U]);
        }
        Registers[0x3CU / 4U] = (Registers[0x3CU / 4U] + 1U) & 0xFFFFU;
        U::Store32(hcca + 0x80U, Registers[0x3CU / 4U]);
        if (!HoldDoneFlush && (m_done != 0U) && ((Registers[3U] & 2U) == 0U))
        {
            U::Store32(hcca + 0x84U, m_done | 1U);
            m_done = 0U;
            Registers[3U] |= 2U;
        }
    }
    void Service(UInt32 head) noexcept
    {
        for (Size edCount{0U}; (head != 0U) && (edCount < 2U); ++edCount)
        {
            UInt8* const ed = Translate(head & ~UInt32{15U}, 16U);
            if (ed == nullptr)
            {
                return;
            }
            const UInt32 flags = U::Load32(ed);
            UInt32 cursor = U::Load32(ed + 8U);
            const UInt32 tail = U::Load32(ed + 4U);
            if (((flags & (1U << 14U)) != 0U) && ((cursor & ~UInt32{15U}) == PendingTd) && (PendingTd != 0U))
            {
                PendingTd = 0U;
                ++Canceled;
            }
            if (((flags & (1U << 14U)) == 0U) && ((cursor & 1U) == 0U))
            {
                for (Size tdCount{0U}; ((cursor & ~UInt32{15U}) != tail) && (tdCount < 130U); ++tdCount)
                {
                    const UInt32 tdAddress = cursor & ~UInt32{15U};
                    UInt8* const td = Translate(tdAddress, 16U);
                    if (td == nullptr)
                    {
                        return;
                    }
                    UInt32 control = U::Load32(td);
                    const UInt32 buffer = U::Load32(td + 4U);
                    const UInt32 end = U::Load32(td + 12U);
                    const Size length = buffer == 0U ? 0U : Size{end - buffer + 1U};
                    const UInt32 next = U::Load32(td + 8U);
                    const UInt32 direction = (control >> 19U) & 3U;
                    const UInt32 pid = direction == 0U ? 0x2DU : (direction == 1U ? 0xE1U : 0x69U);
                    const UInt32 packet = (flags >> 16U) & 0x7FFU;
                    const Size maximum = pid == 0x69U || length < packet ? length : packet;
                    Usb1Wire* const device = Device(flags & 0x7FU);
                    if (device == nullptr)
                    {
                        return; // QEMU leaves an OHCI TD pending for an absent device.
                    }
                    const bool toggle =
                        (control & (1U << 25U)) != 0U ? (control & (1U << 24U)) != 0U : (cursor & 2U) != 0U;
                    UInt8* const data = maximum == 0U ? nullptr : Translate(buffer, maximum);
                    const WireResult result =
                        device->Packet(pid, flags & 0x7FU, (flags >> 7U) & 15U, toggle, (flags & (1U << 13U)) != 0U,
                                       data, maximum, Frames, packet);
                    if (result.Fault == WireFault::Nak)
                    {
                        if (AsyncHold)
                        {
                            PendingTd = tdAddress;
                        }
                        break;
                    }
                    UInt32 code{0U};
                    switch (result.Fault)
                    {
                    case WireFault::None:
                        break;
                    case WireFault::Stall:
                        code = 4U;
                        break;
                    case WireFault::Crc:
                        code = 5U;
                        break;
                    case WireFault::Babble:
                        code = 8U;
                        break;
                    case WireFault::Buffer:
                        code = 12U;
                        break;
                    case WireFault::Stuff:
                        code = 2U;
                        break;
                    case WireFault::Toggle:
                        code = 3U;
                        break;
                    case WireFault::Nak:
                        break;
                    }
                    UInt32 current = buffer;
                    if (code == 0U)
                    {
                        current = result.Length == length ? 0U : buffer + static_cast<UInt32>(result.Length);
                        const Size packets = result.Length == 0U ? 1U : (result.Length + packet - 1U) / packet;
                        const bool after = toggle != ((packets & 1U) != 0U);
                        control = (control & ~UInt32{3U << 24U}) | (1U << 25U) | (after ? 1U << 24U : 0U);
                        cursor = (cursor & ~UInt32{2U}) | (after ? 2U : 0U);
                        if ((pid != 0x69U) && (result.Length < length))
                        {
                            U::Store32(td + 4U, current);
                            U::Store32(td, control);
                            if (++PartialPackets > 20000U)
                            {
                                Good = false;
                                return;
                            }
                            continue;
                        }
                    }
                    else
                    {
                        cursor |= 1U;
                    }
                    cursor = (cursor & 3U) | (next & ~UInt32{15U});
                    U::Store32(td, (control & 0x0FFFFFFFU) | (code << 28U));
                    U::Store32(td + 4U, current);
                    U::Store32(td + 8U, m_done);
                    m_done = tdAddress;
                    if (code != 0U)
                    {
                        break;
                    }
                }
                U::Store32(ed + 8U, cursor);
            }
            head = U::Load32(ed + 12U);
        }
    }
    UInt32 Registers[32U]{};
    UInt32 OwnershipReads{0U};
    UInt32 Writes{0U};
    UInt32 Resets{0U};
    UInt32 PortResets{0U};
    UInt32 PowerWrites{0U};
    UInt32 PartialPackets{0U};
    bool Requested{false};
    bool HoldBios{false};
    bool HoldReset{false};
    bool HoldPortReset{false};
    bool Freeze{false};
    bool HoldDoneFlush{false};
    bool AsyncHold{false};
    UInt32 PendingTd{0U};
    UInt32 Canceled{0U};

private:
    UInt32 m_done{0U};
};
class OhciFixture final
{
public:
    static constexpr UInt32 RetirementFrames{2U};
    OhciFixture() noexcept : Controller{Model}
    {
        Memory = {.Hcca = {0x10000U, Hcca, sizeof(Hcca)},
                  .Endpoints = {0x20000U, Eds, sizeof(Eds)},
                  .Descriptors = {0x30000U, Tds, sizeof(Tds)},
                  .Buffer = {0x40000U, Buffer, sizeof(Buffer)}};
        Model.Map(Memory.Hcca);
        Model.Map(Memory.Endpoints);
        Model.Map(Memory.Descriptors);
        Model.Map(Memory.Buffer);
    }
    [[nodiscard]] bool Start() noexcept
    {
        return Controller.Handoff(0x100U) == U::Usb1Handoff::Success && Controller.Initialize(Memory, 0x100U);
    }
    FakeOhci Model;
    U::OhciController Controller;
    U::OhciMemory Memory{};
    UInt8 Hcca[256U]{};
    UInt8 Eds[32U]{};
    UInt8 Tds[64U]{};
    UInt8 Buffer[1088U]{};
};
} // namespace Asteria::Test::Usb
#endif
