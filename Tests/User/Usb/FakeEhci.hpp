// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_USB_FAKEEHCI_HPP_INCLUDED
#define ASTERIA_TESTS_USER_USB_FAKEEHCI_HPP_INCLUDED

#include "User/Usb/Ehci.hpp"
#include "User/Input/ProtocolFixtures.hpp"

namespace Asteria::Test::Usb
{
namespace E = User::Usb;
namespace In = User::Input;
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
// Intel 8087:0024 USB2 rate-matching hub: six ports, individual power,
// per-port overcurrent, 100ms power good; serialized USB2 descriptor, not native ABI.
inline constexpr UInt8 RateMatchingDevice[]{18U,   1U,    0U, 2U, 9U, 0U, 1U, 64U, 0x87U,
                                            0x80U, 0x24U, 0U, 0U, 0U, 0U, 0U, 0U,  1U};
inline constexpr UInt8 RateMatchingConfiguration[]{9U, 2U, 25U, 0U, 1U, 1U, 0U, 0xE0U, 0U, 9U, 4U, 0U, 0U,
                                                   1U, 9U, 0U,  0U, 0U, 7U, 5U, 0x81U, 3U, 1U, 0U, 12U};
inline constexpr UInt8 RateMatchingHub[]{9U, 0x29U, 6U, 9U, 0U, 50U, 0U, 0U, 0xFFU};

/// Device-address-only EHCI schedule interpreter. Never sees UsbRequest or core
/// state. Traverses ring/frame links and qTD next/alternate pointers, enforces
/// PID/toggle/status sequence and TT address/port/S/C timing, writes DMA tokens.
class FakeEhci final : public E::IEhciMmio
{
public:
    static constexpr UInt32 Operational{0x20U};
    E::EhciMemory Memory{};
    UInt32 Registers[64U]{};
    UInt32 Legacy{1U | (1U << 16U)};
    UInt32 LegacyControl{0xE03FE03FU};
    UInt32 LegacyReads{0U};
    UInt32 ReleaseAfter{2U};
    UInt32 Publishes{0U};
    UInt32 Observes{0U};
    UInt32 AsyncVisits{0U};
    UInt32 PeriodicVisits{0U};
    UInt32 Splits{0U};
    UInt32 PowerWrites{0U};
    UInt32 PortResets{0U};
    UInt32 ClearChanges{0U};
    UInt32 TranslatorClears{0U};
    bool ScheduleStuck{false};
    UInt32 FaultNext{0U};
    UInt32 LastMasks{0U};
    UInt32 LastFrame{0U};
    UInt32 LastToken{0U};
    UInt8 HubAddress{0U};
    UInt8 ChildAddresses[6U]{};
    UInt16 HubStatus[6U]{1U, 1U, 0U, 0U, 0U, 0U};
    UInt16 HubChanges[6U]{1U, 1U, 0U, 0U, 0U, 0U};
    UInt8 KeyboardReport[8U]{2U, 0U, 4U, 0U, 0U, 0U, 0U, 0U};
    UInt8 MouseReport[4U]{1U, 5U, 0xFDU, 0U};
    bool Good{true};
    bool HoldTransfers{false};
    bool HoldAdvance{false};
    bool FreezeFrames{false};
    UInt32 Doorbells{0U};
    UInt32 Advances{0U};
    bool BiosStuck{false};
    bool HaltStuck{false};
    bool ResetStuck{false};
    bool HubResetStuck{false};
    bool RootHighSpeed{true};
    bool ShortNext{false};
    bool NestedHub{false};
    bool HighChild{false};
    bool MultiTt{false};
    bool RejectBadMasks{true};
    explicit FakeEhci(const bool address64 = false) noexcept
    {
        Registers[0U] = 0x01000020U;
        Registers[1U] = 0x11U;
        Registers[2U] = 0x4000U | (address64 ? 1U : 0U);
        Registers[(Operational + 4U) / 4U] = 1U << 12U;
        Registers[(Operational + 0x44U) / 4U] = 1U;
    }
    [[nodiscard]] UInt32 Read32(const UInt32 offset) noexcept override
    {
        if ((offset & 3U) != 0U || offset >= sizeof(Registers))
        {
            Good = false;
            return 0U;
        }
        return Registers[offset / 4U];
    }
    void Write32(const UInt32 offset, const UInt32 value) noexcept override
    {
        if ((offset & 3U) != 0U || offset >= sizeof(Registers))
        {
            Good = false;
            return;
        }
        UInt32& reg = Registers[offset / 4U];
        if (offset == Operational)
        {
            const UInt32 old = reg;
            reg = value;
            if ((value & (1U << 6U)) != 0U)
            {
                ++Doorbells;
            }
            UInt32& status = Registers[(Operational + 4U) / 4U];
            if ((value & 2U) != 0U)
            {
                if (!ResetStuck)
                {
                    reg = 0U;
                }
                status = 1U << 12U;
                return;
            }
            if (((old ^ value) & 0x30U) != 0U)
            {
                Good = Good && ((old & 0x30U) == ((status >> 10U) & 0x30U));
            }
            if ((value & 1U) != 0U)
            {
                status &= ~(1U << 12U);
            }
            else if (!HaltStuck)
            {
                status |= 1U << 12U;
            }
            if (!ScheduleStuck || ((value & 0x30U) != 0U) || ((value & 1U) == 0U))
            {
                status = (status & ~0xC000U) | ((value & 0x30U) << 10U);
            }
        }
        else if (offset == Operational + 4U)
        {
            reg &= ~(value & 0x3FU);
        }
        else if (offset == Operational + 0x44U)
        {
            const UInt32 old = reg;
            reg = (value & ~0x2BU) | (old & 1U) | ((old & 0x2AU) & ~value);
            if (((old & 0x100U) != 0U) && ((value & 0x100U) == 0U) && RootHighSpeed)
            {
                reg |= 4U;
                HubAddress = 0U;
            }
        }
        else
        {
            reg = value;
        }
    }
    [[nodiscard]] UInt32 ReadConfig32(const UInt32 offset) noexcept override
    {
        if (offset == 0x40U)
        {
            ++LegacyReads;
            if (!BiosStuck && ((Legacy & (1U << 24U)) != 0U) && (LegacyReads >= ReleaseAfter))
            {
                Legacy &= ~(1U << 16U);
            }
            return Legacy;
        }
        if (offset == 0x44U)
        {
            return LegacyControl;
        }
        Good = false;
        return 0U;
    }
    void WriteConfig32(const UInt32 offset, const UInt32 value) noexcept override
    {
        if (offset == 0x40U)
        {
            Legacy = (Legacy & ~(1U << 24U)) | (value & (1U << 24U));
        }
        else if (offset == 0x44U)
        {
            Good = Good && ((Legacy & (1U << 16U)) == 0U);
            LegacyControl = (value & 0xE03FU) | ((LegacyControl & 0xE03F0000U) & ~value);
        }
        else
        {
            Good = false;
        }
    }
    void PublishDma() noexcept override
    {
        ++Publishes;
    }
    void ObserveDma() noexcept override
    {
        ++Observes;
        if ((Read32(Operational) & 1U) == 0U)
        {
            return;
        }
        for (UInt32 i{0U}; i < 8U; ++i)
        {
            UInt32& frame = Registers[(Operational + 0xCU) / 4U];
            if (!FreezeFrames)
            {
                frame = (frame + 1U) & 0x3FFFU;
            }
            UInt32& commandRegister = Registers[Operational / 4U];
            if (!HoldAdvance && ((commandRegister & (1U << 6U)) != 0U))
            {
                Good = Good && E::Load32(Memory.AsyncHead.Cpu) ==
                                   (static_cast<UInt32>(Memory.AsyncHead.Address & 0xFFFFFFFFU) | 2U);
                commandRegister &= ~(1U << 6U);
                Registers[(Operational + 4U) / 4U] |= 1U << 5U;
                ++Advances;
            }
            if (HoldTransfers)
            {
                continue;
            }
            const UInt32 command = Read32(Operational);
            if ((command & 0x20U) != 0U)
            {
                AsyncSchedule();
            }
            if ((command & 0x10U) != 0U)
            {
                PeriodicSchedule(frame);
            }
        }
    }

private:
    In::SetupPacket m_setup{};
    UInt8 m_pendingAddress{0U};
    UInt8 m_pendingPort{0U};
    UInt8 m_splitPort{0U};
    UInt32 m_splitFrame{0U};
    bool m_setupSeen{false};
    bool m_asyncSplit{false};
    UInt32 m_asyncStarted{0U};
    UInt64 m_dataQtd{0U};
    Size m_dataActual{0U};
    Size m_dataTransferred{0U};
    Size m_dataPackets{0U};
    UInt8 m_payload[1024U]{};
    bool m_dataSeen{false};
    bool m_expectedToggle[6U]{};
    bool m_ttDirty{false};
    UInt8 m_faultAddress{0U};
    UInt8 m_faultPort{0U};
    [[nodiscard]] UInt8* Map(const UInt64 address, const Size bytes) noexcept
    {
        const E::DmaMemory regions[]{Memory.Frames, Memory.AsyncHead, Memory.Queue, Memory.Descriptors, Memory.Buffer};
        for (const E::DmaMemory region : regions)
        {
            if ((address >= region.Address) && (address - region.Address <= region.Bytes) &&
                (bytes <= region.Bytes - (address - region.Address)))
            {
                return region.Cpu + (address - region.Address);
            }
        }
        Good = false;
        return nullptr;
    }
    [[nodiscard]] UInt64 ScheduleAddress(const UInt32 link) noexcept
    {
        return (UInt64{Read32(Operational + 0x10U)} << 32U) | (link & 0xFFFFFFE0U);
    }
    void AsyncSchedule() noexcept
    {
        const UInt32 start = Read32(Operational + 0x18U);
        UInt32 link = start | 2U;
        bool head{false};
        for (Size visited{0U}; visited < 4U; ++visited)
        {
            if ((link & 7U) != 2U)
            {
                Good = false;
                return;
            }
            UInt8* const qh = Map(ScheduleAddress(link), 96U);
            if (qh == nullptr)
            {
                return;
            }
            head = head || ((E::Load32(qh + 4U) & (1U << 15U)) != 0U);
            ++AsyncVisits;
            Execute(qh, false, Read32(Operational + 0xCU));
            link = E::Load32(qh);
            if ((link & 0xFFFFFFE0U) == start)
            {
                Good = Good && head;
                return;
            }
        }
        Good = false;
    }
    void PeriodicSchedule(const UInt32 frame) noexcept
    {
        const Size index = (frame >> 3U) & 1023U;
        UInt8* const entry = Map(ScheduleAddress(Read32(Operational + 0x14U)) + index * 4U, 4U);
        if (entry == nullptr)
        {
            return;
        }
        UInt32 link = E::Load32(entry);
        if ((link & 1U) != 0U)
        {
            return;
        }
        if ((link & 7U) != 2U)
        {
            Good = false;
            return;
        }
        UInt8* const qh = Map(ScheduleAddress(link), 96U);
        if (qh == nullptr)
        {
            return;
        }
        ++PeriodicVisits;
        LastFrame = frame;
        Execute(qh, true, frame);
        Good = Good && ((E::Load32(qh) & 1U) != 0U);
    }
    void Copy(UInt8* const target, const Size requested, const UInt8* const source, const Size size,
              Size& actual) noexcept
    {
        actual = requested < size ? requested : size;
        if (ShortNext)
        {
            ShortNext = false;
            actual = actual == 0U ? 0U : actual - 1U;
        }
        for (Size i{0U}; i < actual; ++i)
        {
            target[i] = source[i];
        }
    }
    void Data(const UInt8 childPort, UInt8* const data, const Size requested, Size& actual) noexcept
    {
        const UInt8 type = m_setup.Bytes[3U];
        const UInt8 request = m_setup.Bytes[1U];
        if (request == 6U)
        {
            if (childPort == 0U || NestedHub)
            {
                if (type == 1U)
                {
                    Copy(data, requested, RateMatchingDevice, sizeof(RateMatchingDevice), actual);
                }
                else if (type == 2U)
                {
                    Copy(data, requested, RateMatchingConfiguration, sizeof(RateMatchingConfiguration), actual);
                    if (MultiTt && actual > 16U)
                    {
                        data[16U] = 2U;
                    }
                }
                else if (type == 0x29U)
                {
                    Copy(data, requested, RateMatchingHub, sizeof(RateMatchingHub), actual);
                }
                else
                {
                    Good = false;
                }
            }
            else if (childPort == 1U)
            {
                Copy(data, requested, type == 1U ? Input::KeyboardDevice : Input::KeyboardConfiguration,
                     type == 1U ? sizeof(Input::KeyboardDevice) : sizeof(Input::KeyboardConfiguration), actual);
                if (type == 1U && HighChild && actual >= 8U)
                {
                    data[7U] = 64U;
                }
            }
            else
            {
                Copy(data, requested, type == 1U ? Input::MouseDevice : Input::MouseConfiguration,
                     type == 1U ? sizeof(Input::MouseDevice) : sizeof(Input::MouseConfiguration), actual);
            }
        }
        else if ((request == 0U) && (m_setup.Bytes[0U] == 0xA3U))
        {
            const UInt8 port = m_setup.Bytes[4U];
            Good = Good && (port != 0U) && (port <= 6U) && (requested == 4U);
            if ((port == 0U) || (port > 6U) || (requested < 4U))
            {
                return;
            }
            data[0U] = static_cast<UInt8>(HubStatus[port - 1U] & 0xFFU);
            data[1U] = static_cast<UInt8>(HubStatus[port - 1U] >> 8U);
            data[2U] = static_cast<UInt8>(HubChanges[port - 1U] & 0xFFU);
            data[3U] = static_cast<UInt8>(HubChanges[port - 1U] >> 8U);
            actual = 4U;
        }
        else
        {
            Good = false;
        }
    }
    void SetupAction(const UInt8 childPort) noexcept
    {
        if ((m_setup.Bytes[0U] == 0x23U) && (m_setup.Bytes[1U] == 8U))
        {
            Good =
                Good &&
                ((((UInt32{m_setup.Bytes[2U]} | (UInt32{m_setup.Bytes[3U]} << 8U)) >> 4U) & 0x7FU) == m_faultAddress) &&
                ((m_setup.Bytes[3U] & 0x18U) == 0x10U) && (m_setup.Bytes[4U] == (MultiTt ? m_faultPort : UInt8{1U}));
            m_ttDirty = false;
            ++TranslatorClears;
        }
        if (m_setup.Bytes[1U] == 5U)
        {
            m_pendingAddress = m_setup.Bytes[2U];
            m_pendingPort = childPort;
        }
        if ((m_setup.Bytes[0U] == 0x23U) && (m_setup.Bytes[4U] != 0U) && (m_setup.Bytes[4U] <= 6U))
        {
            const Size port = m_setup.Bytes[4U] - 1U;
            if (m_setup.Bytes[1U] == 3U)
            {
                if (m_setup.Bytes[2U] == 8U)
                {
                    HubStatus[port] |= 0x100U;
                    ++PowerWrites;
                }
                else if (m_setup.Bytes[2U] == 4U)
                {
                    ++PortResets;
                    ChildAddresses[port] = 0U;
                    m_expectedToggle[port] = false;
                    if (!HubResetStuck)
                    {
                        HubStatus[port] |= 2U;
                        HubChanges[port] |= 16U;
                    }
                    else
                    {
                        HubStatus[port] |= 16U;
                    }
                }
            }
            else if ((m_setup.Bytes[1U] == 1U) && (m_setup.Bytes[2U] >= 16U) && (m_setup.Bytes[2U] <= 20U))
            {
                ++ClearChanges;
                HubChanges[port] &= static_cast<UInt16>(~(UInt16{1U} << (m_setup.Bytes[2U] - 16U)));
            }
        }
    }
    void Execute(UInt8* const qh, const bool periodic, const UInt32 frame) noexcept
    {
        if ((E::Load32(qh + 24U) & 0x40U) != 0U || ((E::Load32(qh + 16U) & 1U) != 0U))
        {
            return;
        }
        const UInt32 endpoint = E::Load32(qh + 4U);
        const UInt32 caps = E::Load32(qh + 8U);
        const UInt32 speed = (endpoint >> 12U) & 3U;
        const UInt8 childPort = static_cast<UInt8>((caps >> 23U) & 0x7FU);
        if (childPort > 6U)
        {
            Good = false;
            return;
        }
        const UInt8 address = static_cast<UInt8>(endpoint & 0x7FU);
        const UInt32 packet = (endpoint >> 16U) & 0x7FFU;
        Good = Good && (packet != 0U) && ((endpoint & (1U << 14U)) != 0U);
        if (speed != 2U)
        {
            Good = Good && (childPort >= 1U) && (childPort <= 6U) && (((caps >> 16U) & 0x7FU) == HubAddress) &&
                   (HubAddress != 0U);
            if (childPort == 0U || childPort > 6U)
            {
                return;
            }
            Good = Good && ((HubStatus[childPort - 1U] & 3U) == 3U) && (address == ChildAddresses[childPort - 1U]);
            if (!periodic)
            {
                Good = Good && !m_ttDirty;
                Good = Good && ((endpoint & (1U << 27U)) != 0U) && ((caps & 0xFFFFU) == 0U);
                if (!m_asyncSplit)
                {
                    m_asyncSplit = true;
                    m_asyncStarted = frame;
                    return;
                }
                if (((frame - m_asyncStarted) & 0x3FFFU) < 2U)
                {
                    return;
                }
                m_asyncSplit = false;
                ++Splits;
            }
        }
        else
        {
            Good = Good && (address == (childPort == 0U ? HubAddress : ChildAddresses[childPort - 1U]));
        }
        if (periodic)
        {
            Good = Good && ((endpoint >> 28U) == 0U);
            LastMasks = caps & 0xFFFFU;
            const UInt32 micro = frame & 7U;
            if (speed != 2U)
            {
                if (RejectBadMasks)
                {
                    Good = Good && ((caps & 0xFFFFU) == 0x1C01U);
                }
                if ((caps & (1U << micro)) != 0U)
                {
                    m_splitPort = childPort;
                    m_splitFrame = frame >> 3U;
                    return;
                }
                if ((caps & (1U << (micro + 8U))) == 0U || m_splitPort != childPort || m_splitFrame != (frame >> 3U))
                {
                    return;
                }
                m_splitPort = 0U;
                ++Splits;
            }
            else if ((caps & (1U << micro)) == 0U)
            {
                return;
            }
        }
        const Size transactionBudget = speed != 2U && !periodic ? 1U : 3U;
        for (Size count{0U}; count < transactionBudget; ++count)
        {
            const UInt32 link = E::Load32(qh + 16U);
            if ((link & 1U) != 0U)
            {
                return;
            }
            UInt8* const qtd = Map(ScheduleAddress(link), 64U);
            if (qtd == nullptr)
            {
                return;
            }
            UInt32 token = E::Load32(qtd + 8U);
            if ((token & 0x80U) == 0U)
            {
                return;
            }
            const UInt32 pid = (token >> 8U) & 3U;
            const Size length = (token >> 16U) & 0x7FFFU;
            LastToken = token;
            if (FaultNext != 0U)
            {
                token = (token & ~0x80U) | FaultNext;
                FaultNext = 0U;
                if (!periodic && speed != 2U)
                {
                    m_ttDirty = true;
                    m_faultAddress = address;
                    m_faultPort = childPort;
                }
                E::Store32(qtd + 8U, token);
                E::Store32(qh + 24U, token);
                return;
            }
            const UInt64 bufferAddress = UInt64{E::Load32(qtd + 12U)} | (UInt64{E::Load32(qtd + 32U)} << 32U);
            UInt8* const buffer = length == 0U ? nullptr : Map(bufferAddress, length);
            if (length != 0U && buffer == nullptr)
            {
                return;
            }
            if (length != 0U)
            {
                const Size span = length + (bufferAddress & 0xFFFU);
                for (Size page{1U}; page * 4096U < span; ++page)
                {
                    const UInt64 pointer =
                        UInt64{E::Load32(qtd + 12U + page * 4U)} | (UInt64{E::Load32(qtd + 32U + page * 4U)} << 32U);
                    Good = Good && pointer == (bufferAddress & ~UInt64{0xFFFU}) + page * 4096U;
                }
            }
            Size actual{length};
            if (periodic)
            {
                Good = Good && (pid == 1U) && (childPort != 0U);
                if (childPort == 0U || childPort > 6U)
                {
                    return;
                }
                const bool toggle = (token & (1U << 31U)) != 0U;
                Good = Good && (toggle == m_expectedToggle[childPort - 1U]);
                m_expectedToggle[childPort - 1U] = !toggle;
                Copy(buffer, length, childPort == 1U ? KeyboardReport : MouseReport,
                     childPort == 1U ? sizeof(KeyboardReport) : sizeof(MouseReport), actual);
                token ^= 1U << 31U;
            }
            else if (pid == 2U)
            {
                Good = Good && (length == 8U) && ((token & (1U << 31U)) == 0U);
                if (length != 8U)
                {
                    return;
                }
                for (Size i{0U}; i < 8U; ++i)
                {
                    m_setup.Bytes[i] = buffer[i];
                }
                m_setupSeen = true;
                m_dataSeen = false;
                m_dataQtd = 0U;
                SetupAction(childPort);
            }
            else if (length != 0U)
            {
                Good = Good && m_setupSeen && ((token & (1U << 31U)) != 0U) &&
                       (pid == ((m_setup.Bytes[0U] & 0x80U) != 0U ? 1U : 0U));
                const UInt64 descriptor = ScheduleAddress(link);
                if (m_dataQtd != descriptor)
                {
                    m_dataQtd = descriptor;
                    m_dataActual = 0U;
                    m_dataTransferred = 0U;
                    m_dataPackets = 0U;
                    Data(childPort, m_payload, length, m_dataActual);
                }
                const Size remaining = m_dataActual - m_dataTransferred;
                const Size packetBytes = remaining < packet ? remaining : packet;
                for (Size i{0U}; i < packetBytes; ++i)
                {
                    buffer[m_dataTransferred + i] = m_payload[m_dataTransferred + i];
                }
                m_dataTransferred += packetBytes;
                ++m_dataPackets;
                actual = m_dataTransferred;
                const bool initialToggle = (token & (1U << 31U)) != 0U;
                const bool nextToggle = initialToggle != ((m_dataPackets & 1U) != 0U);
                const UInt32 advanced = (token & ~(0x7FFF0000U | (1U << 31U))) |
                                        (static_cast<UInt32>(length - actual) << 16U) | (nextToggle ? 1U << 31U : 0U);
                if ((packetBytes == packet) && (actual < length))
                {
                    E::Store32(qh + 24U, advanced);
                    return;
                }
                token = advanced;
                m_dataQtd = 0U;
                m_dataSeen = true;
            }
            else
            {
                Good = Good && m_setupSeen && ((token & (1U << 31U)) != 0U) &&
                       (pid == ((m_setup.Bytes[0U] & 0x80U) != 0U ? 0U : 1U));
                const Size setupLength = Size{m_setup.Bytes[6U]} | (Size{m_setup.Bytes[7U]} << 8U);
                Good = Good && ((setupLength == 0U) || m_dataSeen);
                m_setupSeen = false;
                if (m_pendingAddress != 0U)
                {
                    if (m_pendingPort == 0U)
                    {
                        HubAddress = m_pendingAddress;
                    }
                    else
                    {
                        ChildAddresses[m_pendingPort - 1U] = m_pendingAddress;
                    }
                    m_pendingAddress = 0U;
                }
            }
            token = (token & ~(0x7FFF0000U | 0x80U)) | (static_cast<UInt32>(length - actual) << 16U);
            E::Store32(qtd + 8U, token);
            E::Store32(qh + 12U, link);
            E::Store32(qh + 24U, token);
            const UInt32 next = actual < length && pid == 1U ? E::Load32(qtd + 4U) : E::Load32(qtd);
            E::Store32(qh + 16U, next);
            if ((token & (1U << 15U)) != 0U)
            {
                Registers[(Operational + 4U) / 4U] |= 1U;
            }
        }
    }
};
struct EhciFixture
{
    UInt8 Frames[4096U]{};
    UInt8 Head[96U]{};
    UInt8 Queue[96U]{};
    UInt8 Descriptors[192U]{};
    UInt8 Buffer[2048U]{};
    E::EhciMemory Memory;
    FakeEhci Model;
    E::EhciController Controller;
    explicit EhciFixture(const UInt64 base = 0x100000U, const bool address64 = false) noexcept
        : Memory{{base, Frames, sizeof(Frames)},
                 {base + 4096U, Head, sizeof(Head)},
                 {base + 4224U, Queue, sizeof(Queue)},
                 {base + 4352U, Descriptors, sizeof(Descriptors)},
                 {base + 4608U, Buffer, sizeof(Buffer)}},
          Model{address64}, Controller{Model}
    {
        Model.Memory = Memory;
    }
};
} // namespace Asteria::Test::Usb
#endif
