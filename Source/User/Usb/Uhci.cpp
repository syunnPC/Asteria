// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Uhci.hpp"

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
    return 0xEU + UInt32{port} * 2U;
}
}
bool UhciController::Wait(const UInt32 offset, const UInt16 mask, const UInt16 expected) noexcept
{
    for (UInt32 i{0U}; i < m_pollLimit; ++i)
    {
        if ((m_io.Read16(offset) & mask) == expected)
        {
            return true;
        }
    }
    return false;
}
Usb1Handoff UhciController::Handoff(const UInt32 pollLimit) noexcept
{
    if ((GetState() != Usb1State::Uninitialized) || !Usb1Limit(pollLimit))
    {
        return Usb1Handoff::InvalidAccess;
    }
    m_handoff = false;
    // Intel UHCI design guide rev 1.1 section 5.2.1: LEGSUP bit 13
    // enables PIRQ; bits 7 and 5:0 enable legacy SMIs/traps/pass-through.
    // Clear those controls and acknowledge RW1C bits 15 and 11:8.
    // Intel PIIX4 290562 section 6.1.12 reserves bit 7 (write/read zero);
    // generic UHCI defines it as SMI at end of pass-through. In both cases
    // the ownership readback must find it clear. RO bits 12/6 are ignored.
    constexpr UInt16 pirq{UInt16{1U} << 13U};
    constexpr UInt16 controls{pirq | (UInt16{1U} << 7U) | 0x3FU};
    constexpr UInt16 acknowledgements{(UInt16{1U} << 15U) | (UInt16{0xFU} << 8U)};
    if (!m_config.WriteConfig16(0xC0U, pirq | acknowledgements))
    {
        return Usb1Handoff::InvalidAccess;
    }
    for (UInt32 i{0U}; i < pollLimit; ++i)
    {
        UInt16 value{0U};
        if (!m_config.ReadConfig16(0xC0U, value))
        {
            return Usb1Handoff::InvalidAccess;
        }
        if ((value & controls) == pirq)
        {
            m_handoff = true;
            return Usb1Handoff::Success;
        }
    }
    return Usb1Handoff::BiosTimeout;
}
bool UhciController::Pause() noexcept
{
    m_io.Write16(0U, 0xC0U); // CF, MaxP, RS clear
    const bool paused = Wait(2U, 0x20U, 0x20U);
    if (paused)
    {
        m_io.ObserveDma();
    }
    return paused;
}
void UhciController::Resume() noexcept
{
    m_io.Write16(0U, 0xC1U);
}
bool UhciController::StopHardware() noexcept
{
    m_io.Write16(4U, 0U);
    static_cast<void>(Pause());
    // HCHalted may precede QEMU's timer/BH cancellation. HCRESET is the
    // synchronous cancel-all barrier for public Halt and fault teardown.
    m_io.Write16(0U, 2U);
    const bool stopped = Wait(0U, 2U, 0U) && Wait(2U, 0x20U, 0x20U);
    if (stopped)
    {
        m_io.ObserveDma();
    }
    return stopped;
}
bool UhciController::Halt(const UInt32 pollLimit) noexcept
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
bool UhciController::Initialize(const UhciMemory& memory, const UInt32 ioBytes, const UInt32 pollLimit) noexcept
{
    if ((GetState() != Usb1State::Uninitialized) || !m_handoff || (ioBytes < 0x14U) || !Usb1Limit(pollLimit) ||
        !ValidMemory(memory.Frames, 4096U, 4096U, false) || !ValidMemory(memory.Queues, 32U, 16U, false) ||
        !ValidMemory(memory.Descriptors, 2080U, 16U, false) || !ValidMemory(memory.Buffer, 1088U, 64U, false))
    {
        return false;
    }
    const DmaMemory regions[]{memory.Frames, memory.Queues, memory.Descriptors, memory.Buffer};
    if (!Usb1MemoryValid(regions, 4U))
    {
        return false;
    }
    m_pollLimit = pollLimit;
    if (!StopHardware())
    {
        Stopped(false);
        return false;
    }
    m_memory = memory;
    Clear(memory.Frames);
    Clear(memory.Queues);
    Clear(memory.Descriptors);
    Clear(memory.Buffer);
    for (Size i{0U}; i < 1024U; ++i)
    {
        Store32(memory.Frames.Cpu + i * 4U, Address(memory.Queues.Address + 16U) | 2U);
    }
    Store32(memory.Queues.Cpu, Address(memory.Queues.Address + 16U) | 2U);
    Store32(memory.Queues.Cpu + 4U, 1U);
    Store32(memory.Queues.Cpu + 16U, 1U);
    Store32(memory.Queues.Cpu + 20U, 1U);
    m_io.PublishDma();
    m_io.Write32(8U, Address(memory.Frames.Address));
    m_io.Write16(6U, 0U);
    m_io.Write8(0xCU, 64U); // 1-ms SOF timing
    m_io.Write16(2U, 0x1FU);
    m_io.Write16(4U, 0xFU); // timeout/CRC, resume, IOC, short-packet INTx
    Resume();
    if (!Wait(2U, 0x20U, 0U))
    {
        Stopped(false);
        return false;
    }
    Ready(2U, memory.Buffer, 100U);
    return true;
}
Input::PortStatus UhciController::ReadPort(const UInt8 port) noexcept
{
    const UInt16 status = m_io.Read16(PortOffset(port));
    if ((status & 0xAU) != 0U)
    {
        // Acknowledge CSC/PEC while preserving enable/reset/resume/suspend.
        m_io.Write16(PortOffset(port), status & 0x124EU);
    }
    return {.Connected = (status & 1U) != 0U,
            .Speed = (status & 0x100U) != 0U ? Input::UsbSpeed::Low : Input::UsbSpeed::Full};
}
void UhciController::PortWrite(const UInt8 port, const UInt16 clear, const UInt16 set) noexcept
{
    const UInt16 current = m_io.Read16(PortOffset(port));
    // Mask CSC/PEC before RMW; acknowledge explicitly and preserve read-only
    // connect/line/LS bits. Resume/suspend are deliberately cleared on reset.
    m_io.Write16(PortOffset(port), static_cast<UInt16>((current & ~UInt32{0xAU} & ~UInt32{clear}) | set));
}
void UhciController::BeginReset(const UInt8 port) noexcept
{
    PortWrite(port, 0x1044U, 0x20AU);
}
void UhciController::EndReset(const UInt8 port) noexcept
{
    PortWrite(port, 0x200U, 0xAU);
}
bool UhciController::ResetEnabled(const UInt8 port) noexcept
{
    PortWrite(port, 0U, 0xCU); // Enable after the 10-ms reset recovery, clear PEC.
    return (m_io.Read16(PortOffset(port)) & 0x205U) == 5U;
}
void UhciController::Td(const Size index, const UInt32 pid, const bool toggle, const UInt32 buffer, const Size length,
                        const UInt32 address) noexcept
{
    UInt8* const td = m_memory.Descriptors.Cpu + index * 16U;
    const UInt32 maximum = length == 0U ? 0x7FFU : static_cast<UInt32>(length - 1U);
    Store32(td, index + 1U == m_count ? 1U : Address(m_memory.Descriptors.Address + (index + 1U) * 16U) | 4U);
    Store32(td + 4U, 0x7FFU | (1U << 23U) | (3U << 27U) | (m_lowSpeed ? 1U << 26U : 0U) |
                         (pid == 0x69U ? 1U << 29U : 0U) | (index + 1U == m_count ? 1U << 24U : 0U));
    Store32(td + 8U, pid | (address << 8U) | (m_interrupt ? m_endpoint << 15U : 0U) | (toggle ? 1U << 19U : 0U) |
                         (maximum << 21U));
    Store32(td + 12U, length == 0U ? 0U : buffer);
}
void UhciController::ArmPeriodic(const UInt32 frame) noexcept
{
    m_serviceFrame = (frame + 1U) & 0x7FFU; // intentional USB frame-number wrap
    Store32(m_memory.Frames.Cpu + (m_serviceFrame & 0x3FFU) * 4U, Address(m_memory.Queues.Address) | 2U);
    m_armed = true;
    m_io.PublishDma();
}
void UhciController::Build(const Input::UsbRequest& request, const bool toggle) noexcept
{
    m_interrupt = request.Operation == Input::UsbOperation::InterruptIn;
    m_input = m_interrupt || ((request.Setup.Bytes[0U] & 0x80U) != 0U);
    m_lowSpeed = request.Speed == Input::UsbSpeed::Low;
    m_endpoint = request.Endpoint.Address & 15U;
    m_interval = m_interrupt ? request.Endpoint.Interval : 1U;
    m_scan = 0U;
    m_actual = 0U;
    m_length = request.Length;
    m_packet = m_interrupt ? request.Endpoint.MaxPacketSize : request.PacketSize0;
    m_count = m_interrupt ? 1U : 2U + (m_length + m_packet - 1U) / m_packet;
    m_toggle = toggle;
    m_stopping = false;
    m_won = false;
    m_wonLength = 0U;
    Clear(m_memory.Descriptors);
    const UInt32 address = request.Operation == Input::UsbOperation::AssignAddress ? 0U : request.Address;
    const UInt32 buffer = Address(m_memory.Buffer.Address);
    if (m_interrupt)
    {
        Td(0U, 0x69U, toggle, buffer + 64U, m_length, address);
    }
    else
    {
        Td(0U, 0x2DU, false, buffer, 8U, address);
        for (Size i{1U}; i + 1U < m_count; ++i)
        {
            const Size offset = (i - 1U) * m_packet;
            const Size length = m_length - offset < m_packet ? m_length - offset : m_packet;
            Td(i, m_input ? 0x69U : 0xE1U, (i & 1U) != 0U, buffer + 64U + static_cast<UInt32>(offset), length, address);
        }
        Td(m_count - 1U, m_input ? 0xE1U : 0x69U, true, 0U, 0U, address);
    }
    for (Size i{0U}; i < 1024U; ++i)
    {
        Store32(m_memory.Frames.Cpu + i * 4U, Address(m_memory.Queues.Address + 16U) | 2U);
    }
    // Publish TDs before making a QH element accessible to the live frame list.
    m_io.PublishDma();
    Store32(m_memory.Queues.Cpu + (m_interrupt ? 4U : 20U), Address(m_memory.Descriptors.Address));
    m_io.PublishDma();
    if (m_interrupt)
    {
        if (m_interval == 1U)
        {
            for (Size i{0U}; i < 1024U; ++i)
            {
                Store32(m_memory.Frames.Cpu + i * 4U, Address(m_memory.Queues.Address) | 2U);
            }
            m_io.PublishDma();
        }
        else
        {
            ArmPeriodic(m_io.Read16(6U));
        }
    }
}
Usb1Progress UhciController::Progress() noexcept
{
    m_io.ObserveDma();
    for (Size count{0U}; (m_scan < m_count) && (count < 130U); ++count)
    {
        const UInt8* const td = m_memory.Descriptors.Cpu + m_scan * 16U;
        const UInt32 token = Load32(td + 8U);
        const Size submitted = ((token >> 21U) + 1U) & 0x7FFU;
        UhciTdResult decoded{};
        if (!DecodeUhciTd(Load32(td + 4U), submitted, decoded))
        {
            return {.Complete = true, .Status = Input::TransferStatus::Error, .Length = 0U, .Toggle = false};
        }
        if (decoded.Active)
        {
            break;
        }
        if (decoded.Status != Input::TransferStatus::Success)
        {
            return {.Complete = true, .Status = decoded.Status, .Length = 0U, .Toggle = false};
        }
        const bool data = m_interrupt || ((m_scan > 0U) && (m_scan + 1U < m_count));
        if (data)
        {
            m_actual += decoded.ActualLength;
            if (m_interrupt)
            {
                m_toggle = !m_toggle;
            }
        }
        if (data && m_input && (decoded.ActualLength < submitted) && !m_interrupt)
        {
            // SPD leaves QH at the short TD (including QEMU). Skip remaining
            // DATA while halted, then run the opposite-direction STATUS TD.
            if (!Pause())
            {
                return {.Complete = true, .Status = Input::TransferStatus::Error, .Length = 0U, .Toggle = false};
            }
            m_scan = m_count - 1U;
            Store32(m_memory.Queues.Cpu + 20U, Address(m_memory.Descriptors.Address + m_scan * 16U));
            m_io.PublishDma();
            Resume();
            return {};
        }
        ++m_scan;
    }
    if (m_scan == m_count)
    {
        return {.Complete = true, .Status = Input::TransferStatus::Success, .Length = m_actual, .Toggle = m_toggle};
    }
    if (m_interrupt)
    {
        const UInt32 frame = m_io.Read16(6U) & 0x7FFU;
        const UInt32 elapsed = (frame - m_serviceFrame) & 0x7FFU;
        if (m_armed && (elapsed >= 1U) && (elapsed < 0x400U))
        {
            Store32(m_memory.Frames.Cpu + (m_serviceFrame & 0x3FFU) * 4U, Address(m_memory.Queues.Address + 16U) | 2U);
            m_armed = false;
            m_io.PublishDma();
        }
        if (!m_armed && (elapsed + 1U >= m_interval) && (elapsed < 0x400U))
        {
            ArmPeriodic(frame);
        }
    }
    return {};
}
bool UhciController::Quiesce() noexcept
{
    if (!m_stopping)
    {
        // Unlink the QHs without racing hardware writes to TD status. UHCI
        // requires a subsequent frame before reuse; QEMU's asynchronous queue
        // validation retains removed queues for 32 frames (QH_VALID). Waiting
        // 33 frames covers both that cancellation and a fetched hardware TD.
        for (Size i{0U}; i < 1024U; ++i)
        {
            Store32(m_memory.Frames.Cpu + i * 4U, 1U);
        }
        m_io.PublishDma();
        m_offFrame = m_io.Read16(6U);
        m_stopping = true;
        return false;
    }
    // A report already retired leaves nothing in flight for an emulator to
    // complete late: the hardware minimum of a frame boundary (two, so one
    // whole frame passed) protects the TD's reuse. Otherwise 33, as above.
    const UInt32 frames = (m_interrupt && (m_scan == m_count)) ? 2U : 33U;
    if (((m_io.Read16(6U) - m_offFrame) & 0x7FFU) < frames)
    {
        return false;
    }
    m_io.ObserveDma();
    if (m_interrupt && (m_scan == 0U))
    {
        UhciTdResult result{};
        if (DecodeUhciTd(Load32(m_memory.Descriptors.Cpu + 4U), m_length, result) && !result.Active &&
            (result.Status == Input::TransferStatus::Success))
        {
            m_toggle = !m_toggle; // A report, including ZLP, won cancellation.
            m_won = true;
            m_wonLength = result.ActualLength;
        }
    }
    Store32(m_memory.Queues.Cpu + 4U, 1U);
    Store32(m_memory.Queues.Cpu + 20U, 1U);
    m_io.PublishDma();
    return true;
}
bool UhciController::RetiredToggle() noexcept
{
    return m_toggle;
}
bool UhciController::WonReport(Size& length) noexcept
{
    length = m_wonLength;
    return m_interrupt && m_won;
}
bool UhciController::ControllerError() noexcept
{
    const UInt16 status = m_io.Read16(2U);
    m_io.Write16(2U, status & 0x1FU);
    return (status & 0x38U) != 0U; // host system/process error or unexpectedly halted
}
} // namespace Asteria::User::Usb
