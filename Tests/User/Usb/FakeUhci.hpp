// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_FAKEUHCI_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_FAKEUHCI_HPP_INCLUDED

#include "User/Usb/Uhci.hpp"

#include "User/Usb/FakeUsb1.hpp"

namespace Asteria::Test::Usb
{
// UHCI 1.1 schedule traversal; QEMU hcd-uhci.c process_frame, complete_td,
// handle_td_error. Inactive/NAK/SPD do not advance QH, depth links do.
class FakeUhci final : public U::IUhciPortIo, public U::IUhciConfig, public Usb1Mappings
{
public:
    FakeUhci() noexcept
    {
        Devices[1U].Mouse = true;
        Registers[1U] = 0x20U;
    }
    [[nodiscard]] UInt8 Read8(const UInt32 offset) noexcept override
    {
        Good = Good && offset == 0xCU;
        return Sof;
    }
    [[nodiscard]] UInt16 Read16(const UInt32 offset) noexcept override
    {
        if ((offset == 0x10U) || (offset == 0x12U))
        {
            const Size port = (offset - 0x10U) / 2U;
            const auto& device = Devices[port];
            return static_cast<UInt16>((Registers[offset / 2U] & ~UInt32{0x105U}) | (device.Connected ? 1U : 0U) |
                                       (device.Enabled ? 4U : 0U) | (device.Low ? 0x100U : 0U));
        }
        if ((offset >= sizeof(Registers)) || ((offset & 1U) != 0U))
        {
            Good = false;
            return 0U;
        }
        return Registers[offset / 2U];
    }
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        Good = Good && offset == 8U;
        return Base;
    }
    void Write8(const UInt32 offset, const UInt8 value) noexcept override
    {
        Good = Good && offset == 0xCU;
        Sof = value;
    }
    void Write16(const UInt32 offset, const UInt16 value) noexcept override
    {
        ++Writes;
        if ((offset >= sizeof(Registers)) || ((offset & 1U) != 0U))
        {
            Good = false;
            return;
        }
        if (offset == 0U)
        {
            if ((value & 2U) != 0U)
            {
                ++Resets;
                if (HoldReset)
                {
                    Registers[0U] = 2U;
                    return;
                }
                Registers[0U] = 0U;
                Registers[1U] = 0x20U;
                Registers[3U] = 0U;
                Base = 0U;
                PendingTd = 0U;
                return;
            }
            Registers[0U] = value;
            if (!HoldHalt)
            {
                Registers[1U] =
                    static_cast<UInt16>((Registers[1U] & ~UInt32{0x20U}) | ((value & 1U) == 0U ? 0x20U : 0U));
            }
            return;
        }
        if (offset == 2U)
        {
            Registers[1U] = static_cast<UInt16>(Registers[1U] & ~UInt32{value & 0x1FU});
            return;
        }
        if ((offset == 0x10U) || (offset == 0x12U))
        {
            const Size port = (offset - 0x10U) / 2U;
            if ((value & 0x200U) != 0U)
            {
                if ((Registers[offset / 2U] & 0x200U) == 0U)
                {
                    ++PortResets;
                    m_resetAt[port] = Frames;
                    Devices[port].Reset();
                    Devices[port].Enabled = false;
                }
            }
            else if ((Registers[offset / 2U] & 0x200U) != 0U)
            {
                Good = Good && ((Frames - m_resetAt[port]) >= 50U);
                m_releasedAt[port] = Frames;
            }
            if ((value & 4U) != 0U)
            {
                Good = Good && ((Frames - m_releasedAt[port]) >= 10U) && ((value & 0x200U) == 0U);
                if (!HoldPortReset)
                {
                    Devices[port].Enabled = Devices[port].Connected;
                }
            }
            if ((value & 4U) == 0U)
            {
                Devices[port].Enabled = false;
            }
            const UInt32 retainedChanges = Registers[offset / 2U] & 0xAU & ~UInt32{value};
            Registers[offset / 2U] = static_cast<UInt16>((value & ~UInt32{0xAU}) | retainedChanges);
            return;
        }
        Registers[offset / 2U] = value;
    }
    void Write32(const UInt32 offset, const UInt32 value) noexcept override
    {
        Good = Good && offset == 8U && (value & 0xFFFU) == 0U;
        Base = value;
    }
    [[nodiscard]] bool ReadConfig16(const UInt32 offset, UInt16& value) noexcept override
    {
        ++OwnershipReads;
        Good = Good && offset == 0xC0U;
        if (DenyConfig)
        {
            return false;
        }
        value = HoldBios ? 0x2001U : Legacy;
        return true;
    }
    [[nodiscard]] bool WriteConfig16(const UInt32 offset, const UInt16 value) noexcept override
    {
        Good = Good && offset == 0xC0U;
        if (DenyConfig)
        {
            return false;
        }
        LegacyWrite = value;
        Legacy = static_cast<UInt16>((value & 0x20BFU) | (Legacy & 0x1040U) | ((Legacy & 0x8F00U) & ~UInt32{value}));
        return true;
    }
    void PublishDma() noexcept override
    {
        ++Publishes;
    }
    void ObserveDma() noexcept override
    {
        ++Observes;
    }
    void CompletePendingZlp() noexcept
    {
        UInt8* const td = Translate(PendingTd, 16U);
        if (td == nullptr)
        {
            return;
        }
        const UInt32 token = U::Load32(td + 8U);
        Usb1Wire* const device = Device((token >> 8U) & 0x7FU);
        if (device == nullptr)
        {
            Good = false;
            return;
        }
        device->Fault = WireFault::None;
        const WireResult packet = device->Packet(token & 0xFFU, (token >> 8U) & 0x7FU, (token >> 15U) & 15U,
                                                 (token & (1U << 19U)) != 0U, device->Low, nullptr, 0U, Frames, 8U);
        Good = Good && (packet.Fault == WireFault::None) && (packet.Length == 0U);
        U::Store32(td + 4U, (U::Load32(td + 4U) & ~UInt32{(1U << 23U) | (1U << 19U) | 0x7FFU}) | 0x7FFU);
        PendingTd = 0U;
    }
    // The held packet's device sends its report into the TD's buffer, as if
    // just before the controller saw the unlink.
    void CompletePendingReport() noexcept
    {
        UInt8* const td = Translate(PendingTd, 16U);
        if (td == nullptr)
        {
            return;
        }
        const UInt32 token = U::Load32(td + 8U);
        const Size length = ((token >> 21U) + 1U) & 0x7FFU;
        Usb1Wire* const device = Device((token >> 8U) & 0x7FU);
        UInt8* const data = Translate(U::Load32(td + 12U), length);
        if ((device == nullptr) || (data == nullptr))
        {
            Good = false;
            return;
        }
        device->Fault = WireFault::None;
        const WireResult packet =
            device->Packet(token & 0xFFU, (token >> 8U) & 0x7FU, (token >> 15U) & 15U, (token & (1U << 19U)) != 0U,
                           device->Low, data, length, Frames, static_cast<UInt32>(length));
        Good = Good && (packet.Fault == WireFault::None);
        U::Store32(td + 4U, (U::Load32(td + 4U) & ~UInt32{(1U << 23U) | (1U << 19U) | 0x7FFU}) |
                                static_cast<UInt32>((packet.Length + 0x7FFU) & 0x7FFU));
        PendingTd = 0U;
    }
    void HostError() noexcept
    {
        Registers[1U] |= 8U;
    }
    void Tick() noexcept
    {
        ++Frames;
        if (Freeze || ((Registers[0U] & 1U) == 0U))
        {
            return;
        }
        if (PendingTd != 0U)
        {
            if (++m_pendingAge >= 32U)
            {
                PendingTd = 0U;
                ++Canceled;
            }
        }
        UInt8* const entry = Translate(Base + (UInt32{Registers[3U]} & 0x3FFU) * 4U, 4U);
        if (entry == nullptr)
        {
            return;
        }
        UInt32 link = U::Load32(entry);
        UInt8* qh{nullptr};
        UInt32 horizontal{1U};
        for (Size visited{0U}; ((link & 1U) == 0U) && (visited < 140U); ++visited)
        {
            if ((link & 2U) != 0U)
            {
                qh = Translate(link & ~UInt32{15U}, 8U);
                if (qh == nullptr)
                {
                    break;
                }
                horizontal = U::Load32(qh);
                link = U::Load32(qh + 4U);
                if ((link & 1U) != 0U)
                {
                    link = horizontal;
                    qh = nullptr;
                }
                continue;
            }
            UInt8* const td = Translate(link & ~UInt32{15U}, 16U);
            if (td == nullptr)
            {
                break;
            }
            UInt32 status = U::Load32(td + 4U);
            if ((link & ~UInt32{15U}) == PendingTd)
            {
                m_pendingAge = 0U;
            }
            if ((status & (1U << 23U)) == 0U)
            {
                if ((PendingTd != 0U) && ((link & ~UInt32{15U}) == PendingTd))
                {
                    PendingTd = 0U;
                    ++Canceled;
                }
                link = qh == nullptr ? U::Load32(td) : horizontal;
                qh = nullptr;
                continue;
            }
            const UInt32 token = U::Load32(td + 8U);
            const Size length = ((token >> 21U) + 1U) & 0x7FFU;
            Usb1Wire* const device = Device((token >> 8U) & 0x7FU);
            UInt8* const data = length == 0U ? nullptr : Translate(U::Load32(td + 12U), length);
            WireResult result{.Fault = WireFault::Crc, .Length = 0U};
            if (device != nullptr)
            {
                result = device->Packet(token & 0xFFU, (token >> 8U) & 0x7FU, (token >> 15U) & 15U,
                                        (token & (1U << 19U)) != 0U, (status & (1U << 26U)) != 0U, data, length, Frames,
                                        length == 0U ? 8U : static_cast<UInt32>(length));
            }
            if (result.Fault == WireFault::Nak)
            {
                if (AsyncHold)
                {
                    PendingTd = link & ~UInt32{15U};
                    m_pendingAge = 0U;
                }
                U::Store32(td + 4U, status | (1U << 19U));
                link = horizontal;
                qh = nullptr;
                continue;
            }
            if ((link & ~UInt32{15U}) == PendingTd)
            {
                PendingTd = 0U;
            }
            UInt32 error{0U};
            switch (result.Fault)
            {
            case WireFault::None:
                break;
            case WireFault::Stall:
                error = 1U << 22U;
                break;
            case WireFault::Babble:
                error = (1U << 20U) | (1U << 22U);
                break;
            case WireFault::Buffer:
                error = 1U << 21U;
                break;
            case WireFault::Crc:
            case WireFault::Toggle:
                error = 1U << 18U;
                break;
            case WireFault::Stuff:
                error = 1U << 17U;
                break;
            case WireFault::Nak:
                break;
            }
            status &= ~UInt32{(1U << 23U) | (1U << 19U) | 0x7FFU};
            status |= error | static_cast<UInt32>((result.Length + 0x7FFU) & 0x7FFU);
            if (error != 0U)
            {
                status &= ~UInt32{3U << 27U};
                Registers[1U] |= 2U;
            }
            U::Store32(td + 4U, status);
            const bool shortPacket = (status & (1U << 29U)) != 0U && (result.Length < length);
            if ((error != 0U) || shortPacket)
            {
                link = horizontal;
                qh = nullptr;
                continue;
            }
            link = U::Load32(td);
            if (qh != nullptr)
            {
                U::Store32(qh + 4U, link);
                if ((link & 4U) == 0U)
                {
                    link = horizontal;
                    qh = nullptr;
                }
            }
        }
        Registers[3U] = static_cast<UInt16>((UInt32{Registers[3U]} + 1U) & 0x7FFU);
    }
    UInt16 Registers[10U]{};
    UInt16 Legacy{0x8F01U};
    UInt16 LegacyWrite{0U};
    UInt8 Sof{0U};
    UInt32 Base{0U};
    UInt32 OwnershipReads{0U};
    UInt32 Writes{0U};
    UInt32 Resets{0U};
    UInt32 PortResets{0U};
    bool HoldBios{false};
    bool DenyConfig{false};
    bool HoldReset{false};
    bool HoldHalt{false};
    bool HoldPortReset{false};
    bool Freeze{false};
    bool AsyncHold{false};
    UInt32 PendingTd{0U};
    UInt32 Canceled{0U};

private:
    UInt32 m_pendingAge{0U};
    UInt64 m_resetAt[2U]{};
    UInt64 m_releasedAt[2U]{};
};
class UhciFixture final
{
public:
    static constexpr UInt32 RetirementFrames{33U};
    UhciFixture() noexcept : Controller{Model, Model}
    {
        Memory = {.Frames = {0x10000U, Frames, sizeof(Frames)},
                  .Queues = {0x20000U, Qhs, sizeof(Qhs)},
                  .Descriptors = {0x30000U, Tds, sizeof(Tds)},
                  .Buffer = {0x40000U, Buffer, sizeof(Buffer)}};
        Model.Map(Memory.Frames);
        Model.Map(Memory.Queues);
        Model.Map(Memory.Descriptors);
        Model.Map(Memory.Buffer);
    }
    [[nodiscard]] bool Start() noexcept
    {
        return Controller.Handoff() == U::Usb1Handoff::Success && Controller.Initialize(Memory, 0x20U);
    }
    FakeUhci Model;
    U::UhciController Controller;
    U::UhciMemory Memory{};
    UInt8 Frames[4096U]{};
    UInt8 Qhs[32U]{};
    UInt8 Tds[2080U]{};
    UInt8 Buffer[1088U]{};
};
} // namespace Asteria::Test::Usb
#endif
