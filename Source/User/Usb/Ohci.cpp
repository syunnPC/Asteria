// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Ohci.hpp"

namespace Asteria::User::Usb
{
namespace
{
constexpr UInt32 Address(const UInt64 value) noexcept
{
    return static_cast<UInt32>(value & 0xFFFFFFFFU);
}
constexpr UInt32 PortOffset(const UInt8 port) noexcept
{
    return 0x50U + UInt32{port} * 4U;
}
}
bool OhciController::Wait(const UInt32 offset, const UInt32 mask, const UInt32 expected) noexcept
{
    for (UInt32 i{0U}; i < m_pollLimit; ++i)
    {
        if ((m_mmio.Read32(offset) & mask) == expected)
        {
            return true;
        }
    }
    return false;
}
Usb1Handoff OhciController::Handoff(const UInt32 barBytes, const UInt32 pollLimit) noexcept
{
    if ((GetState() != Usb1State::Uninitialized) || (barBytes < 0x54U) || !Usb1Limit(pollLimit) ||
        ((m_mmio.Read32(0U) & 0xFFU) != 0x10U))
    {
        return Usb1Handoff::InvalidAccess;
    }
    m_handoff = false;
    m_pollLimit = pollLimit;
    if ((m_mmio.Read32(4U) & (1U << 8U)) != 0U)
    {
        m_mmio.Write32(8U, 1U << 3U); // OwnershipChangeRequest, never clear BIOS IR ourselves.
        if (!Wait(4U, 1U << 8U, 0U))
        {
            return Usb1Handoff::BiosTimeout;
        }
    }
    m_mmio.Write32(0x14U, 0xFFFFFFFFU); // Disable all interrupt sources after ownership.
    m_mmio.Write32(0xCU, 1U << 30U);    // Acknowledge OwnershipChange.
    m_handoff = true;
    return Usb1Handoff::Success;
}
bool OhciController::Initialize(const OhciMemory& memory, const UInt32 barBytes, const UInt32 pollLimit) noexcept
{
    if ((GetState() != Usb1State::Uninitialized) || !m_handoff || !Usb1Limit(pollLimit) || (barBytes < 0x54U) ||
        !ValidMemory(memory.Hcca, 256U, 256U, false) || !ValidMemory(memory.Endpoints, 32U, 16U, false) ||
        !ValidMemory(memory.Descriptors, 64U, 16U, false) || !ValidMemory(memory.Buffer, 1088U, 64U, false))
    {
        return false;
    }
    const DmaMemory regions[]{memory.Hcca, memory.Endpoints, memory.Descriptors, memory.Buffer};
    const UInt32 descriptor = m_mmio.Read32(0x48U);
    const UInt32 ports = descriptor & 0xFFU;
    if (!Usb1MemoryValid(regions, 4U) || (ports == 0U) || (ports > 15U) || (barBytes < 0x54U + ports * 4U))
    {
        return false;
    }
    m_pollLimit = pollLimit;
    // Preserve firmware's calibrated FI. Recompute FSMPS and toggle FIT after reset.
    const UInt32 interval = m_mmio.Read32(0x34U) & 0x3FFFU;
    if (interval < 210U)
    {
        return false;
    }
    if (!StopHardware())
    {
        Stopped(false);
        return false;
    }
    m_memory = memory;
    Clear(memory.Hcca);
    Clear(memory.Endpoints);
    Clear(memory.Descriptors);
    Clear(memory.Buffer);
    m_mmio.PublishDma();
    const UInt32 fit = (m_mmio.Read32(0x38U) ^ 0x80000000U) & 0x80000000U;
    m_mmio.Write32(0x34U, fit | (((interval - 210U) * 6U / 7U) << 16U) | interval);
    m_mmio.Write32(0x40U, interval * 9U / 10U);
    m_mmio.Write32(0x44U, 0x628U);
    m_mmio.Write32(0x18U, Address(memory.Hcca.Address));
    m_mmio.Write32(0x20U, Address(memory.Endpoints.Address));
    m_mmio.Write32(0xCU, 0xFFFFFFFFU);
    m_mmio.Write32(0x10U, 0x80000052U); // MIE, RHSC, UE, WDH (INTx).
    // Global and individual power commands are write-only aliases, not RMW.
    m_mmio.Write32(0x50U, 1U << 16U);
    for (UInt32 port{1U}; port <= ports; ++port)
    {
        m_mmio.Write32(0x50U + port * 4U, 1U << 8U);
    }
    m_mmio.Write32(4U, m_control);
    Ready(static_cast<UInt8>(ports), memory.Buffer, ((descriptor >> 24U) * 2U) + 100U);
    return true;
}
bool OhciController::StopHardware() noexcept
{
    m_mmio.Write32(0x14U, 0xFFFFFFFFU);
    m_mmio.Write32(8U, 1U);
    const bool stopped = Wait(8U, 1U, 0U);
    if (stopped)
    {
        m_mmio.ObserveDma();
    }
    return stopped;
}
bool OhciController::Halt(const UInt32 pollLimit) noexcept
{
    if (!m_handoff || !Usb1Limit(pollLimit))
    {
        return false;
    }
    m_pollLimit = pollLimit;
    const bool stopped = StopHardware();
    Stopped(stopped);
    return stopped;
}
Input::PortStatus OhciController::ReadPort(const UInt8 port) noexcept
{
    const UInt32 value = m_mmio.Read32(PortOffset(port));
    if ((value & 0x1F0000U) != 0U)
    {
        m_mmio.Write32(PortOffset(port), value & 0x1F0000U);
    }
    return {.Connected = (value & 1U) != 0U,
            .Speed = (value & (1U << 9U)) != 0U ? Input::UsbSpeed::Low : Input::UsbSpeed::Full};
}
void OhciController::BeginReset(const UInt8 port) noexcept
{
    m_mmio.Write32(PortOffset(port), 0x1F0010U);
}
void OhciController::EndReset(const UInt8 port) noexcept
{
    static_cast<void>(port); // OHCI autonomously releases PRS.
}
bool OhciController::ResetEnabled(const UInt8 port) noexcept
{
    const UInt32 value = m_mmio.Read32(PortOffset(port));
    if ((value & 0x13U) == 3U)
    {
        m_mmio.Write32(PortOffset(port), 0x1F0000U);
        return true;
    }
    return false;
}
void OhciController::Td(const Size index, const UInt32 direction, const UInt32 toggle, const UInt32 buffer,
                        const Size length) noexcept
{
    UInt8* const td = m_memory.Descriptors.Cpu + index * 16U;
    // CC=NotAccessed, DI=0 ensures every retired TD reaches HCCA even on errors.
    Store32(td, 0xF0000000U | (toggle << 24U) | (direction << 19U) | (1U << 18U));
    Store32(td + 4U, length == 0U ? 0U : buffer);
    Store32(td + 8U, Address(m_memory.Descriptors.Address + (index + 1U) * 16U));
    Store32(td + 12U, length == 0U ? 0U : buffer + static_cast<UInt32>(length - 1U));
    m_lengths[index] = length;
    m_buffers[index] = buffer;
}
void OhciController::ArmPeriodic(const UInt32 frame) noexcept
{
    m_serviceFrame = (frame + 1U) & 0xFFFFU; // intentional 16-bit frame wrap
    Store32(m_memory.Hcca.Cpu + (m_serviceFrame & 31U) * 4U, Address(m_memory.Endpoints.Address + 16U));
    m_armed = true;
    m_mmio.PublishDma();
}
void OhciController::Build(const Input::UsbRequest& request, const bool toggle) noexcept
{
    m_stopping = false;
    m_won = false;
    m_seen = 0U;
    m_actual = 0U;
    m_interrupt = request.Operation == Input::UsbOperation::InterruptIn;
    m_interval = m_interrupt ? request.Endpoint.Interval : 1U;
    Clear(m_memory.Endpoints);
    Clear(m_memory.Descriptors);
    const bool input = m_interrupt || ((request.Setup.Bytes[0U] & 0x80U) != 0U);
    const UInt32 buffer = Address(m_memory.Buffer.Address);
    if (m_interrupt)
    {
        m_count = 1U;
        Td(0U, 2U, 0U, buffer + 64U, request.Length); // toggle from ED carry
    }
    else
    {
        m_count = request.Length == 0U ? 2U : 3U;
        Td(0U, 0U, 2U, buffer, 8U); // SETUP DATA0
        if (request.Length != 0U)
        {
            Td(1U, input ? 2U : 1U, 3U, buffer + 64U, request.Length); // DATA1
        }
        Td(m_count - 1U, input ? 1U : 2U, 3U, 0U, 0U); // opposite STATUS DATA1
    }
    UInt8* const ed = m_memory.Endpoints.Cpu + (m_interrupt ? 16U : 0U);
    const UInt32 address = request.Operation == Input::UsbOperation::AssignAddress ? 0U : request.Address;
    const UInt32 packet = m_interrupt ? request.Endpoint.MaxPacketSize : request.PacketSize0;
    Store32(ed, address | (m_interrupt ? UInt32{request.Endpoint.Address & 15U} << 7U : 0U) |
                    (request.Speed == Input::UsbSpeed::Low ? 1U << 13U : 0U) | (packet << 16U));
    Store32(ed + 4U, Address(m_memory.Descriptors.Address + m_count * 16U));
    Store32(ed + 8U, Address(m_memory.Descriptors.Address) | ((m_interrupt && toggle) ? 2U : 0U));
    m_mmio.PublishDma();
    if (m_interrupt)
    {
        if (m_interval == 1U)
        {
            for (Size i{0U}; i < 32U; ++i)
            {
                Store32(m_memory.Hcca.Cpu + i * 4U, Address(m_memory.Endpoints.Address + 16U));
            }
            m_mmio.PublishDma();
        }
        else
        {
            ArmPeriodic(m_mmio.Read32(0x3CU));
        }
        m_mmio.Write32(4U, m_control | 4U);
    }
    else
    {
        m_mmio.Write32(0x20U, Address(m_memory.Endpoints.Address));
        m_mmio.Write32(0x24U, 0U);
        m_mmio.Write32(4U, m_control | 16U);
        m_mmio.Write32(8U, 2U);
    }
}
Usb1Progress OhciController::Progress() noexcept
{
    m_mmio.ObserveDma();
    UInt32 done = Load32(m_memory.Hcca.Cpu + 0x84U) & ~UInt32{1U};
    if (done != 0U)
    {
        Store32(m_memory.Hcca.Cpu + 0x84U, 0U);
        m_mmio.PublishDma();
        // Hardware rewrites NextTD to the reverse done list. Validate each link
        // and reject cycles, duplicate/stale TDs and alien physical addresses.
        for (Size visited{0U}; (done != 0U) && (visited < m_count); ++visited)
        {
            if ((done < m_memory.Descriptors.Address) || ((done & 15U) != 0U) ||
                (UInt64{done} >= m_memory.Descriptors.Address + m_count * 16U))
            {
                return {.Complete = true, .Status = Input::TransferStatus::Error, .Length = 0U, .Toggle = false};
            }
            const Size index = (done - m_memory.Descriptors.Address) / 16U;
            const UInt32 bit = 1U << index;
            OhciTdResult decoded{};
            const UInt8* const td = m_memory.Descriptors.Cpu + index * 16U;
            if ((m_seen & bit) != 0U ||
                !DecodeOhciTd(Load32(td), Load32(td + 4U), m_buffers[index], m_lengths[index], decoded) ||
                decoded.Active)
            {
                return {.Complete = true, .Status = Input::TransferStatus::Error, .Length = 0U, .Toggle = false};
            }
            m_seen |= bit;
            if (decoded.Status != Input::TransferStatus::Success)
            {
                return {.Complete = true, .Status = decoded.Status, .Length = 0U, .Toggle = false};
            }
            if (m_interrupt || ((m_count == 3U) && (index == 1U)))
            {
                m_actual = decoded.ActualLength;
            }
            done = Load32(td + 8U);
        }
        if (done != 0U)
        {
            return {.Complete = true, .Status = Input::TransferStatus::Error, .Length = 0U, .Toggle = false};
        }
        m_mmio.Write32(0xCU, 2U);
    }
    if (m_seen == ((1U << m_count) - 1U))
    {
        const UInt8* const ed = m_memory.Endpoints.Cpu + (m_interrupt ? 16U : 0U);
        return {.Complete = true,
                .Status = Input::TransferStatus::Success,
                .Length = m_actual,
                .Toggle = (Load32(ed + 8U) & 2U) != 0U};
    }
    if (m_interrupt && (m_interval > 1U))
    {
        const UInt32 frame = m_mmio.Read32(0x3CU) & 0xFFFFU;
        const UInt32 elapsed = (frame - m_serviceFrame) & 0xFFFFU;
        if (m_armed && (elapsed >= 1U) && (elapsed < 0x8000U))
        {
            Store32(m_memory.Hcca.Cpu + (m_serviceFrame & 31U) * 4U, 0U);
            m_armed = false;
            m_mmio.PublishDma();
        }
        if (!m_armed && (elapsed + 1U >= m_interval) && (elapsed < 0x8000U))
        {
            ArmPeriodic(frame);
        }
    }
    return {};
}
bool OhciController::Quiesce() noexcept
{
    if (!m_stopping)
    {
        // Skip protects an ED already fetched; keep it reachable for two SOFs
        // before reusing TDs. No polling of software-writable HCFS as a halt proof.
        UInt8* const ed = m_memory.Endpoints.Cpu + (m_interrupt ? 16U : 0U);
        Store32(ed, Load32(ed) | (1U << 14U));
        for (Size i{0U}; i < 32U; ++i)
        {
            Store32(m_memory.Hcca.Cpu + i * 4U, m_interrupt ? Address(m_memory.Endpoints.Address + 16U) : 0U);
        }
        m_mmio.PublishDma();
        // Keep the skipped ED reachable for two frames: asynchronous packets
        // are cancelled when hardware fetches Skip/H, before the list is unlinked.
        m_mmio.Write32(4U, m_control | (m_interrupt ? 4U : 16U));
        m_offFrame = m_mmio.Read32(0x3CU) & 0xFFFFU;
        m_stopping = true;
        return false;
    }
    // WDH must be acknowledged so a second pending hardware done list can
    // reach HCCA on SOF. Cancelled TDs are discarded, never delivered to core.
    m_mmio.ObserveDma();
    Store32(m_memory.Hcca.Cpu + 0x84U, 0U);
    m_mmio.PublishDma();
    m_mmio.Write32(0xCU, 2U);
    if ((((m_mmio.Read32(0x3CU) - m_offFrame) & 0xFFFFU) < 2U) || (m_mmio.Read32(0x30U) != 0U))
    {
        return false;
    }
    m_mmio.Write32(4U, m_control);
    m_mmio.Write32(0x20U, 0U);
    m_mmio.Write32(0x24U, 0U);
    m_mmio.ObserveDma();
    if (m_interrupt && (m_seen == 0U))
    {
        // A report that retired before Skip took effect is final in its TD
        // (and its toggle in the ED carry); it is delivered, not lost.
        OhciTdResult decoded{};
        const UInt8* const td = m_memory.Descriptors.Cpu;
        if (DecodeOhciTd(Load32(td), Load32(td + 4U), m_buffers[0U], m_lengths[0U], decoded) && !decoded.Active &&
            (decoded.Status == Input::TransferStatus::Success))
        {
            m_won = true;
            m_actual = decoded.ActualLength;
        }
    }
    for (Size i{0U}; i < 32U; ++i)
    {
        Store32(m_memory.Hcca.Cpu + i * 4U, 0U);
    }
    Store32(m_memory.Hcca.Cpu + 0x84U, 0U);
    m_mmio.PublishDma();
    m_mmio.Write32(0xCU, 2U);
    return true;
}
bool OhciController::RetiredToggle() noexcept
{
    return (Load32(m_memory.Endpoints.Cpu + 24U) & 2U) != 0U;
}
bool OhciController::WonReport(Size& length) noexcept
{
    length = m_actual;
    return m_interrupt && m_won;
}
bool OhciController::ControllerError() noexcept
{
    const UInt32 status = m_mmio.Read32(0xCU);
    m_mmio.Write32(0xCU, status & ~UInt32{2U}); // WDH belongs to done processing.
    return (status & 16U) != 0U;
}
} // namespace Asteria::User::Usb
