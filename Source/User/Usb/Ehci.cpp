// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Ehci.hpp"

namespace Asteria::User::Usb
{
namespace
{
constexpr UInt32 Run{1U};
constexpr UInt32 Periodic{1U << 4U};
constexpr UInt32 Async{1U << 5U};
constexpr UInt32 Halted{1U << 12U};
constexpr UInt32 ScheduleStatus{(1U << 14U) | (1U << 15U)};
constexpr UInt32 PortChanges{0x2AU};
constexpr UInt32 PortOwner{1U << 13U};
constexpr UInt32 Link(const UInt64 address) noexcept
{
    return static_cast<UInt32>(address & 0xFFFFFFE0U);
}
constexpr bool Limit(const UInt32 limit) noexcept
{
    return (limit != 0U) && (limit <= 100000U);
}
}

bool EhciController::Wait(const UInt32 offset, const UInt32 mask, const UInt32 expected, const UInt32 limit) noexcept
{
    for (UInt32 i{0U}; i < limit; ++i)
    {
        if ((m_mmio.Read32(offset) & mask) == expected)
        {
            return true;
        }
    }
    return false;
}
HandoffStatus EhciController::Handoff(const UInt32 barBytes, const UInt32 pollLimit) noexcept
{
    if ((m_state != EhciState::Uninitialized) || (barBytes < 12U) || !Limit(pollLimit))
    {
        return HandoffStatus::InvalidCapability;
    }
    UInt32 next = (m_mmio.Read32(8U) >> 8U) & 0xFFU;
    UInt64 visited{0U};
    // PCI configuration EECP list (not MMIO), including non-forward links.
    for (Size count{0U}; (next != 0U) && (count < 48U); ++count)
    {
        if ((next < 0x40U) || (next > 0xFCU) || ((next & 3U) != 0U))
        {
            return HandoffStatus::InvalidCapability;
        }
        const UInt64 bit = UInt64{1U} << ((next - 0x40U) / 4U);
        if ((visited & bit) != 0U)
        {
            return HandoffStatus::InvalidCapability;
        }
        visited |= bit;
        const UInt32 legacy = m_mmio.ReadConfig32(next);
        if ((legacy & 0xFFU) == 1U)
        {
            if (next > 0xF8U)
            {
                return HandoffStatus::InvalidCapability;
            }
            m_mmio.WriteConfig32(next, legacy | (1U << 24U));
            bool released{false};
            for (UInt32 i{0U}; i < pollLimit; ++i)
            {
                if ((m_mmio.ReadConfig32(next) & (1U << 16U)) == 0U)
                {
                    released = true;
                    break;
                }
            }
            if (!released)
            {
                return HandoffStatus::BiosTimeout;
            }
            // Disable USB/ownership/PCI-command/BAR SMIs; acknowledge RW1C statuses.
            m_mmio.WriteConfig32(next + 4U, 0xE03F0000U);
        }
        const UInt32 following = (legacy >> 8U) & 0xFFU;
        next = following;
    }
    if (next != 0U)
    {
        return HandoffStatus::InvalidCapability;
    }
    m_handoff = true;
    return HandoffStatus::Success;
}
bool EhciController::MemoryValid(const EhciMemory& memory) const noexcept
{
    if (!ValidMemory(memory.Frames, 4096U, 4096U, m_address64) ||
        !ValidMemory(memory.AsyncHead, 96U, 32U, m_address64) || !ValidMemory(memory.Queue, 96U, 32U, m_address64) ||
        !ValidMemory(memory.Descriptors, 192U, 32U, m_address64) ||
        !ValidMemory(memory.Buffer, 1088U, 64U, m_address64))
    {
        return false;
    }
    const DmaMemory regions[]{memory.Frames, memory.AsyncHead, memory.Queue, memory.Descriptors, memory.Buffer};
    for (Size i{0U}; i < 5U; ++i)
    {
        for (Size j{i + 1U}; j < 5U; ++j)
        {
            if (((regions[i].Address < regions[j].Address + regions[j].Bytes) &&
                 (regions[j].Address < regions[i].Address + regions[i].Bytes)) ||
                (regions[i].Cpu == regions[j].Cpu))
            {
                return false;
            }
        }
    }
    const UInt64 segment = memory.Frames.Address >> 32U;
    // All schedule links use CTRLDSSEGMENT; buffers have independent high dwords.
    return (memory.Frames.Address >> 32U) == ((memory.Frames.Address + memory.Frames.Bytes - 1U) >> 32U) &&
           (memory.AsyncHead.Address >> 32U) == segment &&
           ((memory.AsyncHead.Address + memory.AsyncHead.Bytes - 1U) >> 32U) == segment &&
           (memory.Queue.Address >> 32U) == segment &&
           ((memory.Queue.Address + memory.Queue.Bytes - 1U) >> 32U) == segment &&
           (memory.Descriptors.Address >> 32U) == segment &&
           ((memory.Descriptors.Address + memory.Descriptors.Bytes - 1U) >> 32U) == segment;
}
bool EhciController::Initialize(const EhciMemory& memory, const UInt32 barBytes, const UInt32 pollLimit) noexcept
{
    if ((m_state != EhciState::Uninitialized) || !m_handoff || (barBytes < 12U) || !Limit(pollLimit))
    {
        return false;
    }
    m_operational = m_mmio.Read32(0U) & 0xFFU;
    const UInt32 structural = m_mmio.Read32(4U);
    m_portCount = static_cast<UInt8>(structural & 15U);
    m_powerControl = (structural & 16U) != 0U;
    m_address64 = (m_mmio.Read32(8U) & 1U) != 0U;
    if ((m_operational < 0x10U) || ((m_operational & 3U) != 0U) || (m_portCount == 0U) ||
        (barBytes < m_operational + 0x44U + UInt32{m_portCount} * 4U) || !MemoryValid(memory))
    {
        return false;
    }
    m_pollLimit = pollLimit;
    if (!Halt(pollLimit))
    {
        return false;
    }
    m_mmio.Write32(m_operational, 2U);
    if (!Wait(m_operational, 2U, 0U, pollLimit))
    {
        m_state = EhciState::Faulted;
        return false;
    }
    m_memory = memory;
    Clear(memory.Frames);
    Clear(memory.AsyncHead);
    Clear(memory.Queue);
    Clear(memory.Descriptors);
    Clear(memory.Buffer);
    for (Size i{0U}; i < 1024U; ++i)
    {
        Store32(memory.Frames.Cpu + i * 4U, 1U);
    }
    Store32(memory.AsyncHead.Cpu, Link(memory.AsyncHead.Address) | 2U);
    Store32(memory.AsyncHead.Cpu + 4U, (1U << 15U) | (2U << 12U) | (64U << 16U));
    Store32(memory.AsyncHead.Cpu + 16U, 1U);
    Store32(memory.AsyncHead.Cpu + 20U, 1U);
    Store32(memory.AsyncHead.Cpu + 24U, 0x40U);
    m_mmio.PublishDma();
    m_mmio.Write32(m_operational + 0x10U, static_cast<UInt32>(memory.Frames.Address >> 32U));
    m_mmio.Write32(m_operational + 0x14U, Link(memory.Frames.Address));
    m_mmio.Write32(m_operational + 0x18U, Link(memory.AsyncHead.Address));
    m_mmio.Write32(m_operational + 4U, 0x3FU);
    m_mmio.Write32(m_operational + 8U, 0x17U);
    m_mmio.Write32(m_operational + 0x40U, 1U); // Route all ports to EHCI.
    for (UInt8 i{1U}; i <= m_portCount; ++i)
    {
        if (m_powerControl)
        {
            PortWrite(i, 0U, 1U << 12U);
        }
    }
    m_mmio.Write32(m_operational, Run | (8U << 16U));
    if (!Wait(m_operational + 4U, Halted, 0U, pollLimit))
    {
        m_state = EhciState::Faulted;
        return false;
    }
    m_state = EhciState::Running;
    Poll(m_now);
    return true;
}
bool EhciController::Halt(const UInt32 pollLimit) noexcept
{
    if (!Limit(pollLimit) || (m_operational == 0U))
    {
        return false;
    }
    m_mmio.Write32(m_operational, m_mmio.Read32(m_operational) & ~(Run | Async | Periodic));
    if (!Wait(m_operational + 4U, Halted, Halted, pollLimit))
    {
        m_state = EhciState::Faulted;
        return false;
    }
    m_mmio.ObserveDma();
    m_state = EhciState::Halted;
    if (m_phase != Phase::Idle)
    {
        Finish(Input::TransferStatus::Retryable);
    }
    return true;
}
bool EhciController::ScheduleOff() noexcept
{
    const UInt32 enabled = m_mmio.Read32(m_operational) & (Async | Periodic);
    if (!Wait(m_operational + 4U, ScheduleStatus, enabled << 10U, m_pollLimit))
    {
        Fail(Input::TransferStatus::Error);
        return false;
    }
    m_mmio.Write32(m_operational, m_mmio.Read32(m_operational) & ~(Async | Periodic));
    if (!Wait(m_operational + 4U, ScheduleStatus, 0U, m_pollLimit))
    {
        Fail(Input::TransferStatus::Error);
        return false;
    }
    m_mmio.ObserveDma();
    return true;
}
UInt32 EhciController::PortOffset(const UInt8 port) const noexcept
{
    return m_operational + 0x44U + UInt32{port - 1U} * 4U;
}
void EhciController::PortWrite(const UInt8 port, const UInt32 clear, const UInt32 set) noexcept
{
    m_mmio.Write32(PortOffset(port), (m_mmio.Read32(PortOffset(port)) & ~(PortChanges | clear)) | set);
}
Input::PortStatus EhciController::GetPortStatus(const UInt8 rootPort) noexcept
{
    if ((rootPort == 0U) || (rootPort > m_portCount))
    {
        return {.Connected = false, .Speed = Input::UsbSpeed::High};
    }
    return m_ports[rootPort - 1U];
}
void EhciController::Finish(const Input::TransferStatus status, const Size length) noexcept
{
    if (m_recovering)
    {
        m_request = m_original;
        m_original.Data = nullptr;
        m_recovering = false;
    }
    Pipe& pipe = m_pipes[m_pipe - 1U];
    pipe.Result = {.Token = m_request.Token, .Status = status, .ActualLength = length};
    pipe.Ready = true;
    m_request.Data = nullptr;
    m_phase = Phase::Idle;
}
void EhciController::Fail(const Input::TransferStatus status) noexcept
{
    const bool busy = m_phase != Phase::Idle;
    if (Halt(m_pollLimit) && busy)
    {
        m_pipes[m_pipe - 1U].Result.Status = status;
    }
}
bool EhciController::TakeCompletion(const UInt8 pipe, EhciCompletion& output) noexcept
{
    if ((pipe == 0U) || (pipe > 16U) || !m_pipes[pipe - 1U].Ready)
    {
        return false;
    }
    output = m_pipes[pipe - 1U].Result;
    m_pipes[pipe - 1U].Ready = false;
    return true;
}
bool EhciController::Submit(const UInt8 pipe, const UInt8 rootPort, const Input::UsbRequest& request) noexcept
{
    if ((m_state != EhciState::Running) || (m_phase != Phase::Idle) || (pipe == 0U) || (pipe > 16U) ||
        (rootPort == 0U) || (rootPort > m_portCount) || m_pipes[pipe - 1U].Ready || !m_ports[rootPort - 1U].Connected)
    {
        return false;
    }
    m_pipe = pipe;
    m_root = rootPort;
    m_pipes[pipe - 1U].Root = rootPort;
    m_request = request;
    m_started = m_now;
    m_phaseStarted = m_now;
    m_phase = Phase::Transfer;
    const Input::UsbRoute route = request.Route;
    if ((request.Speed == Input::UsbSpeed::Super) || (request.Address > 127U) ||
        ((request.PacketSize0 != 8U) && (request.PacketSize0 != 16U) && (request.PacketSize0 != 32U) &&
         (request.PacketSize0 != 64U)) ||
        ((request.Speed == Input::UsbSpeed::Low) && (request.PacketSize0 != 8U)) ||
        ((request.Speed == Input::UsbSpeed::High) && (request.PacketSize0 != 64U)) ||
        (request.Length > Input::MaximumInterruptBytes) || ((request.Length != 0U) && (request.Data == nullptr)) ||
        (route.Depth > 1U) ||
        ((route.Depth == 1U) &&
         ((route.HubAddress == 0U) || (route.HubAddress > 127U) || (route.HubPort == 0U) || (route.HubPort > 15U))) ||
        ((route.Depth == 0U) && (request.Speed != Input::UsbSpeed::High) &&
         (request.Operation != Input::UsbOperation::ResetPort)))
    {
        Finish(Input::TransferStatus::Error);
        return true;
    }
    switch (request.Operation)
    {
    case Input::UsbOperation::ResetPort:
        m_pipes[pipe - 1U].Toggle = false;
        m_pipes[pipe - 1U].Configured = false;
        if (route.Depth != 0U)
        {
            Finish(Input::TransferStatus::Success);
        }
        else
        {
            // A root port reset returns every endpoint below it to DATA0
            // (USB 2.0 section 7.1.7.5), whichever logical pipe served it.
            for (Pipe& other : m_pipes)
            {
                other.Configured = (other.Root == rootPort) ? false : other.Configured;
                other.Toggle = (other.Root == rootPort) ? false : other.Toggle;
            }
            PortWrite(rootPort, 4U, 1U << 8U);
            m_phase = Phase::ResetAssert;
        }
        return true;
    case Input::UsbOperation::UpdateEndpointZero:
        Finish(Input::TransferStatus::Success);
        return true;
    case Input::UsbOperation::ConfigureEndpoint:
    {
        const Input::EndpointDescriptor endpoint = request.Endpoint;
        const UInt16 packet = endpoint.MaxPacketSize & 0x7FFU;
        if (((endpoint.Address & 0xF0U) != 0x80U) || ((endpoint.Address & 15U) == 0U) || (endpoint.Attributes != 3U) ||
            (packet == 0U) || (endpoint.Interval == 0U) || ((endpoint.MaxPacketSize & 0xF800U) != 0U) ||
            ((request.Speed == Input::UsbSpeed::Low) && (packet > 8U)) ||
            ((request.Speed == Input::UsbSpeed::Full) && (packet > 64U)) ||
            ((request.Speed == Input::UsbSpeed::High) && ((packet > 1024U) || (endpoint.Interval > 16U))))
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        if (!m_pipes[pipe - 1U].Configured || (m_pipes[pipe - 1U].Endpoint.Address != endpoint.Address))
        {
            m_pipes[pipe - 1U].Toggle = false;
        }
        m_pipes[pipe - 1U].Endpoint = endpoint;
        m_pipes[pipe - 1U].Speed = request.Speed;
        m_pipes[pipe - 1U].Configured = true;
        m_pipes[pipe - 1U].PhaseSet = false;
        m_pipes[pipe - 1U].ServiceAt = 0U;
        Finish(Input::TransferStatus::Success);
        return true;
    }
    case Input::UsbOperation::InterruptIn:
        if (!m_pipes[pipe - 1U].Configured || (request.Speed != m_pipes[pipe - 1U].Speed) || (request.Length == 0U) ||
            (request.Endpoint.Address != m_pipes[pipe - 1U].Endpoint.Address) ||
            (request.Length > (m_pipes[pipe - 1U].Endpoint.MaxPacketSize & 0x7FFU)))
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        break;
    case Input::UsbOperation::BulkIn:
    case Input::UsbOperation::BulkOut:
        Finish(Input::TransferStatus::Error);
        return true;
    case Input::UsbOperation::AssignAddress:
    case Input::UsbOperation::Control:
        if ((Size{request.Setup.Bytes[6U]} | (Size{request.Setup.Bytes[7U]} << 8U)) != request.Length)
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        break;
    }
    if (!ScheduleOff())
    {
        return true;
    }
    if ((request.Operation == Input::UsbOperation::InterruptIn) && (request.Speed == Input::UsbSpeed::High) &&
        (m_pipes[pipe - 1U].Endpoint.Interval >= 15U) && (m_now < m_pipes[pipe - 1U].ServiceAt))
    {
        m_nextService = m_pipes[pipe - 1U].ServiceAt;
        m_phase = Phase::InterruptWait;
        return true;
    }
    BuildTransfer();
    return true;
}
void EhciController::Descriptor(const Size index, const UInt32 next, const UInt32 alternate, const UInt32 token,
                                const UInt64 buffer) noexcept
{
    UInt8* const bytes = m_memory.Descriptors.Cpu + index * 64U;
    Store32(bytes, next);
    Store32(bytes + 4U, alternate);
    Store32(bytes + 8U, token);
    for (Size i{0U}; i < 5U; ++i)
    {
        const Size span = Size{(token >> 16U) & 0x7FFFU} + (buffer & 0xFFFU);
        const UInt64 page = i == 0U ? buffer : (i * 4096U < span ? (buffer & ~UInt64{0xFFFU}) + i * 4096U : 0U);
        Store32(bytes + 12U + i * 4U, static_cast<UInt32>(page & 0xFFFFFFFFU));
        Store32(bytes + 32U + i * 4U, static_cast<UInt32>(page >> 32U));
    }
}
bool EhciController::Abort(const UInt8 pipe, const UInt64 token) noexcept
{
    if ((m_state != EhciState::Running) || (m_phase == Phase::Idle) || (m_phase == Phase::Quiescing) ||
        (m_phase == Phase::AddressRecovery) || m_recovering || (pipe != m_pipe) || (token != m_request.Token) ||
        ((m_request.Operation != Input::UsbOperation::Control) &&
         (m_request.Operation != Input::UsbOperation::InterruptIn) &&
         (m_request.Operation != Input::UsbOperation::AssignAddress)))
    {
        return false;
    }
    Retire(Input::TransferStatus::Cancelled);
    return true;
}
void EhciController::Retire(const Input::TransferStatus status) noexcept
{
    if (m_phase != Phase::Transfer)
    {
        Finish(status);
        return;
    }
    m_abortStatus = status;
    m_asyncUnlink = m_request.Operation != Input::UsbOperation::InterruptIn;
    // Preserve the removed QH and all qTDs until the HC drops cached links.
    if (m_asyncUnlink)
    {
        Store32(m_memory.AsyncHead.Cpu, Link(m_memory.AsyncHead.Address) | 2U);
    }
    else
    {
        for (Size i{0U}; i < 1024U; ++i)
        {
            Store32(m_memory.Frames.Cpu + i * 4U, 1U);
        }
    }
    m_mmio.PublishDma();
    m_unlinkFrame = (m_mmio.Read32(m_operational + 0xCU) >> 3U) & 0x7FFU;
    m_phaseStarted = m_now;
    m_phase = Phase::Quiescing;
    if (m_asyncUnlink)
    {
        m_mmio.Write32(m_operational + 4U, 1U << 5U); // discard stale IAA
        m_mmio.Write32(m_operational, m_mmio.Read32(m_operational) | (1U << 6U));
    }
}
void EhciController::BuildTransfer() noexcept
{
    const bool interrupt = m_request.Operation == Input::UsbOperation::InterruptIn;
    const bool input = interrupt || ((m_request.Setup.Bytes[0U] & 0x80U) != 0U);
    Pipe& pipe = m_pipes[m_pipe - 1U];
    const UInt32 speed =
        m_request.Speed == Input::UsbSpeed::High ? 2U : (m_request.Speed == Input::UsbSpeed::Low ? 1U : 0U);
    const UInt32 packet = interrupt ? (pipe.Endpoint.MaxPacketSize & 0x7FFU) : m_request.PacketSize0;
    const UInt32 address = m_request.Operation == Input::UsbOperation::AssignAddress ? 0U : m_request.Address;
    Clear(m_memory.Queue);
    Clear(m_memory.Descriptors);
    for (Size i{0U}; i < 8U; ++i)
    {
        m_memory.Buffer.Cpu[i] = m_request.Setup.Bytes[i];
    }
    if (!input)
    {
        for (Size i{0U}; i < m_request.Length; ++i)
        {
            m_memory.Buffer.Cpu[64U + i] = m_request.Data[i];
        }
    }
    Store32(m_memory.Queue.Cpu, interrupt ? 1U : Link(m_memory.AsyncHead.Address) | 2U);
    Store32(m_memory.Queue.Cpu + 4U, address | (interrupt ? UInt32{pipe.Endpoint.Address & 15U} << 8U : 0U) |
                                         (speed << 12U) | (1U << 14U) | (packet << 16U) | (interrupt ? 0U : 4U << 28U) |
                                         ((!interrupt && (speed != 2U)) ? 1U << 27U : 0U));
    UInt32 masks{0U};
    Size frames{1U};
    if (interrupt)
    {
        if (speed == 2U)
        {
            const UInt32 microframes = 1U << (pipe.Endpoint.Interval - 1U);
            if (microframes < 8U)
            {
                for (UInt32 micro{0U}; micro < 8U; micro += microframes)
                {
                    masks |= 1U << micro;
                }
            }
            else
            {
                masks = 1U;
                frames = microframes / 8U;
            }
        }
        else
        {
            // One periodic transaction globally: SS at uframe 0, CS at 2/3/4.
            masks = 0x1C01U;
            while ((frames * 2U) <= pipe.Endpoint.Interval)
            {
                frames *= 2U;
            }
        }
    }
    Store32(m_memory.Queue.Cpu + 8U, masks | (UInt32{m_request.Route.HubAddress} << 16U) |
                                         (UInt32{m_request.Route.HubPort} << 23U) | (1U << 30U));
    const UInt32 status = Link(m_memory.Descriptors.Address + 128U);
    const UInt32 active = 0x80U | (3U << 10U);
    const UInt32 length = static_cast<UInt32>(m_request.Length);
    if (interrupt)
    {
        Descriptor(0U, 1U, 1U, active | (1U << 8U) | (1U << 15U) | (length << 16U) | (pipe.Toggle ? 1U << 31U : 0U),
                   m_memory.Buffer.Address + 64U);
    }
    else
    {
        Descriptor(0U, m_request.Length == 0U ? status : Link(m_memory.Descriptors.Address + 64U), 1U,
                   active | (2U << 8U) | (8U << 16U), m_memory.Buffer.Address);
        Descriptor(1U, status, input ? status : 1U, active | (input ? 1U << 8U : 0U) | (length << 16U) | (1U << 31U),
                   m_memory.Buffer.Address + 64U);
        Descriptor(2U, 1U, 1U, active | (input ? 0U : 1U << 8U) | (1U << 15U) | (1U << 31U), 0U);
    }
    Store32(m_memory.Queue.Cpu + 16U, Link(m_memory.Descriptors.Address));
    Store32(m_memory.Queue.Cpu + 20U, 1U);
    Store32(m_memory.Queue.Cpu + 24U, 0U);
    m_interruptFrames = frames;
    if (interrupt)
    {
        if (!pipe.PhaseSet || (frames > 1024U))
        {
            pipe.FramePhase = ((m_mmio.Read32(m_operational + 0xCU) >> 3U) + 1U) % frames;
            pipe.PhaseSet = true;
        }
        const Size phase = pipe.FramePhase;
        for (Size i{0U}; i < 1024U; ++i)
        {
            Store32(m_memory.Frames.Cpu + i * 4U, (frames > 1024U ? i == (phase & 1023U) : (i % frames) == phase)
                                                      ? Link(m_memory.Queue.Address) | 2U
                                                      : 1U);
        }
    }
    else
    {
        Store32(m_memory.AsyncHead.Cpu, Link(m_memory.Queue.Address) | 2U);
    }
    if (interrupt && (frames > 1024U))
    {
        m_nextService = m_now <= ~UInt64{0U} - frames ? m_now + frames : ~UInt64{0U};
        pipe.ServiceAt = m_nextService;
        m_phaseStarted = m_now;
    }
    m_mmio.PublishDma();
    m_mmio.Write32(m_operational, m_mmio.Read32(m_operational) | (interrupt ? Periodic : Async));
}
void EhciController::ClearTranslator(const Input::TransferStatus status) noexcept
{
    m_original = m_request;
    m_recoveryStatus = status;
    m_recovering = true;
    const UInt32 address = m_request.Operation == Input::UsbOperation::AssignAddress ? 0U : m_request.Address;
    const UInt32 value = (address << 4U) | (2U << 11U) | ((m_request.Setup.Bytes[0U] & 0x80U) != 0U ? 1U << 15U : 0U);
    m_request = {.Operation = Input::UsbOperation::Control,
                 .Token = m_original.Token,
                 .Speed = Input::UsbSpeed::High,
                 .Address = m_original.Route.HubAddress,
                 .PacketSize0 = 64U,
                 .Setup = {{0x23U, 8U, static_cast<UInt8>(value & 0xFFU), static_cast<UInt8>(value >> 8U),
                            m_original.Route.MultiTt ? m_original.Route.HubPort : UInt8{1U}, 0U, 0U, 0U}},
                 .Endpoint = {},
                 .Data = nullptr,
                 .Length = 0U};
    m_started = m_now; // recovery control has its own bounded deadline
    BuildTransfer();
}
void EhciController::ProgressTransfer() noexcept
{
    const bool interrupt = m_request.Operation == Input::UsbOperation::InterruptIn;
    const Size count = interrupt ? 1U : 3U;
    Size actual{0U};
    bool done{true};
    Input::TransferStatus status{Input::TransferStatus::Success};
    QtdResult result{};
    // Controllers may halt the overlay without retiring the backing qTD.
    if (!DecodeQtd(Load32(m_memory.Queue.Cpu + 24U), 0x5000U, result))
    {
        Fail(Input::TransferStatus::Error);
        return;
    }
    if ((result.State != QtdState::Success) && (result.State != QtdState::Active))
    {
        status = QtdStatus(result.State);
    }
    else
    {
        for (Size i{0U}; i < count; ++i)
        {
            if (!interrupt && (i == 1U) && (m_request.Length == 0U))
            {
                continue;
            }
            const Size length = interrupt ? m_request.Length : (i == 0U ? 8U : (i == 1U ? m_request.Length : 0U));
            if (!DecodeQtd(Load32(m_memory.Descriptors.Cpu + i * 64U + 8U), length, result))
            {
                Fail(Input::TransferStatus::Error);
                return;
            }
            if (result.State == QtdState::Active)
            {
                done = false;
                break;
            }
            if (result.State != QtdState::Success)
            {
                status = QtdStatus(result.State);
                break;
            }
            if (interrupt || (i == 1U))
            {
                actual = result.ActualLength;
            }
        }
    }
    if (!done)
    {
        if ((m_interruptFrames > 1024U) && ((m_now - m_phaseStarted) >= 2U) && ScheduleOff())
        {
            m_phase = Phase::InterruptWait;
        }
        return;
    }
    if (!ScheduleOff())
    {
        return;
    }
    if (m_recovering)
    {
        const bool originalRetired = (m_recoveryStatus == Input::TransferStatus::Cancelled) ||
                                     (m_recoveryStatus == Input::TransferStatus::TimedOut);
        const Input::TransferStatus recovered = ((status == Input::TransferStatus::Success) || originalRetired)
                                                    ? m_recoveryStatus
                                                    : Input::TransferStatus::Error;
        Finish(recovered);
        return;
    }
    if (status != Input::TransferStatus::Success)
    {
        if (!interrupt && (m_request.Route.Depth == 1U) && (m_request.Speed != Input::UsbSpeed::High))
        {
            ClearTranslator(status);
            return;
        }
        Finish(status);
        return;
    }
    if (interrupt)
    {
        // The qTD toggle is hardware's next packet toggle, including short/ZLP.
        m_pipes[m_pipe - 1U].Toggle = (Load32(m_memory.Descriptors.Cpu + 8U) & (1U << 31U)) != 0U;
    }
    if (interrupt || ((m_request.Setup.Bytes[0U] & 0x80U) != 0U))
    {
        for (Size i{0U}; i < actual; ++i)
        {
            m_request.Data[i] = m_memory.Buffer.Cpu[64U + i];
        }
    }
    if (m_request.Operation == Input::UsbOperation::AssignAddress)
    {
        m_phase = Phase::AddressRecovery;
        m_phaseStarted = m_now;
    }
    else
    {
        Finish(status, actual);
    }
}
void EhciController::Poll(const UInt64 milliseconds) noexcept
{
    if ((m_state != EhciState::Running) || (milliseconds < m_now))
    {
        return;
    }
    m_now = milliseconds;
    m_mmio.ObserveDma();
    const UInt32 controllerStatus = m_mmio.Read32(m_operational + 4U);
    m_mmio.Write32(m_operational + 4U, controllerStatus & 0x3FU);
    if ((controllerStatus & (1U << 4U)) != 0U)
    {
        Fail(Input::TransferStatus::Error);
        return;
    }
    for (UInt8 i{1U}; i <= m_portCount; ++i)
    {
        const UInt32 port = m_mmio.Read32(PortOffset(i));
        const bool connected = ((port & 1U) != 0U) && ((port & PortOwner) == 0U);
        if (!connected && (m_phase != Phase::Idle) && (m_root == i))
        {
            if (!ScheduleOff())
            {
                return;
            }
            Finish(Input::TransferStatus::Disconnected);
        }
        m_ports[i - 1U] = {.Connected = connected,
                           .Speed = (port & 4U) != 0U
                                        ? Input::UsbSpeed::High
                                        : (((port >> 10U) & 3U) == 1U ? Input::UsbSpeed::Low : Input::UsbSpeed::Full)};
    }
    if (m_phase == Phase::Idle)
    {
        return;
    }
    const bool reset = m_request.Operation == Input::UsbOperation::ResetPort;
    const UInt64 timeout = (reset || m_recovering) ? 1000U : m_request.TimeoutMilliseconds;
    if ((m_phase != Phase::Quiescing) && (m_phase != Phase::AddressRecovery) && (timeout != 0U) &&
        ((m_now - m_started) >= timeout))
    {
        if (reset)
        {
            PortWrite(m_root, 1U << 8U, 0U);
        }
        Retire(m_recovering && (m_recoveryStatus == Input::TransferStatus::Cancelled)
                   ? Input::TransferStatus::Cancelled
                   : Input::TransferStatus::TimedOut);
        return; // IAA sampled before this unlink cannot acknowledge its doorbell.
    }
    switch (m_phase)
    {
    case Phase::Idle:
        return;
    case Phase::ResetAssert:
        if ((m_now - m_phaseStarted) >= 50U)
        {
            PortWrite(m_root, 1U << 8U, 0U);
            m_phase = Phase::ResetWait;
        }
        return;
    case Phase::ResetWait:
    {
        const UInt32 port = m_mmio.Read32(PortOffset(m_root));
        if ((port & (1U << 8U)) != 0U)
        {
            return;
        }
        if ((port & 4U) == 0U)
        {
            PortWrite(m_root, 0U, PortOwner);
            m_ports[m_root - 1U].Connected = false;
            Finish(Input::TransferStatus::Disconnected);
            return;
        }
        m_phase = Phase::ResetRecovery;
        m_phaseStarted = m_now;
        return;
    }
    case Phase::ResetRecovery:
        if ((m_now - m_phaseStarted) >= 10U)
        {
            Finish(Input::TransferStatus::Success);
        }
        return;
    case Phase::AddressRecovery:
        if ((m_now - m_phaseStarted) >= 2U)
        {
            Finish(Input::TransferStatus::Success);
        }
        return;
    case Phase::InterruptWait:
        if (m_now >= m_nextService)
        {
            BuildTransfer();
            m_phase = Phase::Transfer;
        }
        return;
    case Phase::Quiescing:
    {
        const UInt32 frame = (m_mmio.Read32(m_operational + 0xCU) >> 3U) & 0x7FFU;
        const bool retired =
            m_asyncUnlink ? (controllerStatus & (1U << 5U)) != 0U : ((frame - m_unlinkFrame) & 0x7FFU) >= 2U;
        if (!retired)
        {
            if ((m_now - m_phaseStarted) >= 100U)
            {
                Fail(Input::TransferStatus::Error); // broken HC retirement handshake
            }
            return;
        }
        if (!ScheduleOff())
        {
            return;
        }
        if (!m_asyncUnlink)
        {
            // Retain the next DATA toggle if a packet won the unlink race,
            // and deliver that report rather than lose it.
            m_pipes[m_pipe - 1U].Toggle = (Load32(m_memory.Descriptors.Cpu + 8U) & (1U << 31U)) != 0U;
            QtdResult won{};
            if ((m_request.Operation == Input::UsbOperation::InterruptIn) && !m_recovering &&
                ((m_abortStatus == Input::TransferStatus::Cancelled) ||
                 (m_abortStatus == Input::TransferStatus::TimedOut)) &&
                DecodeQtd(Load32(m_memory.Descriptors.Cpu + 8U), m_request.Length, won) &&
                (won.State == QtdState::Success) && (won.ActualLength <= m_request.Length) &&
                (m_request.Data != nullptr))
            {
                for (Size i{0U}; i < won.ActualLength; ++i)
                {
                    m_request.Data[i] = m_memory.Buffer.Cpu[64U + i];
                }
                Finish(Input::TransferStatus::Success, won.ActualLength);
                return;
            }
        }
        if (m_asyncUnlink && (m_request.Route.Depth == 1U) && (m_request.Speed != Input::UsbSpeed::High))
        {
            ClearTranslator(m_abortStatus);
            m_phase = Phase::Transfer;
            return;
        }
        Finish(m_abortStatus);
        return;
    }
    case Phase::Transfer:
        ProgressTransfer();
        return;
    }
}
} // namespace Asteria::User::Usb
