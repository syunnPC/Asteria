// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/Usb1.hpp"

namespace Asteria::User::Usb
{
bool Usb1Limit(const UInt32 limit) noexcept
{
    return (limit != 0U) && (limit <= 100000U);
}
bool Usb1MemoryValid(const DmaMemory* const regions, const Size count) noexcept
{
    // Memory owner boundary: integer CPU ranges allow overlap checks without
    // relational comparisons between pointers to unrelated C++ objects.
    if ((regions == nullptr) || (count == 0U) || (count > 4U))
    {
        return false;
    }
    for (Size i{0U}; i < count; ++i)
    {
        if (!ValidMemory(regions[i], 1U, 1U, false))
        {
            return false;
        }
        const UInt64 cpu = reinterpret_cast<UInt64>(regions[i].Cpu);
        if (cpu > ~UInt64{0U} - regions[i].Bytes)
        {
            return false;
        }
        for (Size j{0U}; j < i; ++j)
        {
            const UInt64 other = reinterpret_cast<UInt64>(regions[j].Cpu);
            if (((cpu < other + regions[j].Bytes) && (other < cpu + regions[i].Bytes)) ||
                ((regions[i].Address < regions[j].Address + regions[j].Bytes) &&
                 (regions[j].Address < regions[i].Address + regions[i].Bytes)))
            {
                return false;
            }
        }
    }
    return true;
}
void Usb1Controller::Ready(const UInt8 ports, const DmaMemory buffer, const UInt32 powerDelay) noexcept
{
    m_ports = ports;
    m_buffer = buffer;
    m_powerDelay = powerDelay;
    m_powerAt = m_now;
    m_state = Usb1State::Running;
}
void Usb1Controller::Stopped(const bool success) noexcept
{
    m_state = success ? Usb1State::Halted : Usb1State::Faulted;
    if (m_phase != Phase::Idle)
    {
        Finish(success ? Input::TransferStatus::Retryable : Input::TransferStatus::Error);
    }
}
Input::PortStatus Usb1Controller::GetPortStatus(const UInt8 port) noexcept
{
    if ((m_state != Usb1State::Running) || (port == 0U) || (port > m_ports))
    {
        return {.Connected = false, .Speed = Input::UsbSpeed::Full};
    }
    // Revoke the core's borrow before GetPortStatus exposes a disconnect.
    const Input::PortStatus status = ReadPort(port);
    if (!status.Connected)
    {
        for (Pipe& pipe : m_pipes)
        {
            pipe.Configured = (pipe.Port == port) ? false : pipe.Configured;
        }
        if ((m_phase != Phase::Idle) && (port == m_port))
        {
            Retire(Input::TransferStatus::Disconnected);
            if (m_phase == Phase::Quiescing)
            {
                // Expose disconnect only after DMA quiescence. The component
                // delivers completions before stepping any USB core.
                return {.Connected = true, .Speed = status.Speed};
            }
        }
    }
    return status;
}
bool Usb1Controller::Submit(const UInt8 pipeNumber, const UInt8 port, const Input::UsbRequest& request) noexcept
{
    if ((m_state != Usb1State::Running) || (m_phase != Phase::Idle) || (port == 0U) || (port > m_ports) ||
        (pipeNumber == 0U) || (pipeNumber > Usb1MaximumPipes) || m_pipes[pipeNumber - 1U].Ready)
    {
        return false;
    }
    m_pipe = pipeNumber;
    m_port = port;
    m_pipes[pipeNumber - 1U].Port = port;
    m_request = request;
    m_result = {};
    m_started = m_now;
    m_phaseAt = m_now;
    if (!ReadPort(port).Connected)
    {
        Finish(Input::TransferStatus::Disconnected);
        return true;
    }
    if ((request.Route.Depth != 0U) || (request.Route.HubAddress != 0U) || (request.Route.HubPort != 0U) ||
        ((request.Speed != Input::UsbSpeed::Full) && (request.Speed != Input::UsbSpeed::Low)) ||
        (request.Speed != ReadPort(port).Speed) || (request.Address > 127U) || (request.Length > 1024U) ||
        ((request.Length != 0U) && (request.Data == nullptr)) ||
        ((request.PacketSize0 != 8U) && (request.PacketSize0 != 16U) && (request.PacketSize0 != 32U) &&
         (request.PacketSize0 != 64U)) ||
        ((request.Speed == Input::UsbSpeed::Low) && (request.PacketSize0 != 8U)))
    {
        Finish(Input::TransferStatus::Error);
        return true;
    }
    Pipe& pipe = m_pipes[pipeNumber - 1U];
    switch (request.Operation)
    {
    case Input::UsbOperation::ResetPort:
        pipe.Configured = false;
        pipe.Toggle = false;
        pipe.Serviced = false;
        m_phase = Phase::PowerWait;
        return true;
    case Input::UsbOperation::UpdateEndpointZero:
        Finish(Input::TransferStatus::Success);
        return true;
    case Input::UsbOperation::ConfigureEndpoint:
        if (((request.Endpoint.Address & 0xF0U) != 0x80U) || ((request.Endpoint.Address & 15U) == 0U) ||
            (request.Endpoint.Attributes != 3U) || (request.Endpoint.Interval == 0U) ||
            (request.Endpoint.MaxPacketSize == 0U) || (request.Endpoint.MaxPacketSize > 64U) ||
            ((request.Speed == Input::UsbSpeed::Low) && (request.Endpoint.MaxPacketSize > 8U)))
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        pipe.Endpoint = request.Endpoint;
        pipe.Configured = true;
        pipe.Toggle = false;
        pipe.Serviced = false;
        Finish(Input::TransferStatus::Success);
        return true;
    case Input::UsbOperation::InterruptIn:
        if (!pipe.Configured || (request.Endpoint.Address != pipe.Endpoint.Address) ||
            (request.Endpoint.MaxPacketSize != pipe.Endpoint.MaxPacketSize) ||
            (request.Endpoint.Interval != pipe.Endpoint.Interval) || (request.Length == 0U) ||
            (request.Length > pipe.Endpoint.MaxPacketSize))
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        m_phase = Phase::InterruptWait;
        return true;
    case Input::UsbOperation::AssignAddress:
        if ((request.Address == 0U) || (request.Length != 0U))
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        m_request.Setup = Input::UsbDescriptors::SetAddress(request.Address);
        break;
    case Input::UsbOperation::Control:
        if ((Size{request.Setup.Bytes[6U]} | (Size{request.Setup.Bytes[7U]} << 8U)) != request.Length)
        {
            Finish(Input::TransferStatus::Error);
            return true;
        }
        break;
    default:
        Finish(Input::TransferStatus::Error);
        return true;
    }
    StartTransfer();
    return true;
}
bool Usb1Controller::Abort(const UInt8 pipe, const UInt64 token) noexcept
{
    if ((m_state != Usb1State::Running) || (m_phase == Phase::Idle) || (m_phase == Phase::Quiescing) ||
        (m_phase == Phase::AddressRecovery) || (pipe != m_pipe) || (token != m_request.Token) ||
        ((m_request.Operation != Input::UsbOperation::Control) &&
         (m_request.Operation != Input::UsbOperation::InterruptIn) &&
         (m_request.Operation != Input::UsbOperation::AssignAddress)))
    {
        return false;
    }
    Retire(Input::TransferStatus::Cancelled);
    return true;
}
void Usb1Controller::StartTransfer() noexcept
{
    if (m_request.Operation != Input::UsbOperation::InterruptIn)
    {
        for (Size i{0U}; i < 8U; ++i)
        {
            m_buffer.Cpu[i] = m_request.Setup.Bytes[i];
        }
    }
    if ((m_request.Operation == Input::UsbOperation::Control) && ((m_request.Setup.Bytes[0U] & 0x80U) == 0U))
    {
        for (Size i{0U}; i < m_request.Length; ++i)
        {
            m_buffer.Cpu[64U + i] = m_request.Data[i];
        }
    }
    Pipe& pipe = m_pipes[m_pipe - 1U];
    if (m_request.Operation == Input::UsbOperation::InterruptIn)
    {
        pipe.ServiceAt = m_now;
        pipe.Serviced = true;
    }
    Build(m_request, pipe.Toggle);
    m_phase = Phase::Transfer;
}
void Usb1Controller::Finish(const Input::TransferStatus status, const Size length) noexcept
{
    Pipe& pipe = m_pipes[m_pipe - 1U];
    pipe.Completion = {.Token = m_request.Token, .Status = status, .ActualLength = length};
    pipe.Ready = true;
    m_request.Data = nullptr;
    m_phase = Phase::Idle;
}
bool Usb1Controller::TakeCompletion(const UInt8 pipe, Usb1Completion& output) noexcept
{
    if ((pipe == 0U) || (pipe > Usb1MaximumPipes) || !m_pipes[pipe - 1U].Ready)
    {
        return false;
    }
    output = m_pipes[pipe - 1U].Completion;
    m_pipes[pipe - 1U].Ready = false;
    return true;
}
void Usb1Controller::Retire(const Input::TransferStatus status, const Size length, const bool toggle) noexcept
{
    m_result = {.Complete = true, .Status = status, .Length = length, .Toggle = toggle};
    if (status == Input::TransferStatus::Disconnected)
    {
        m_request.Data = nullptr;
    }
    if ((m_phase == Phase::Transfer) || (m_phase == Phase::Quiescing))
    {
        if (m_phase != Phase::Quiescing)
        {
            m_phaseAt = m_now;
        }
        m_phase = Phase::Quiescing;
        static_cast<void>(Quiesce());
        return;
    }
    Finish(status, length);
}
void Usb1Controller::Poll(const UInt64 milliseconds) noexcept
{
    if ((m_state != Usb1State::Running) || (milliseconds < m_now))
    {
        return;
    }
    m_now = milliseconds;
    if (ControllerError())
    {
        const bool stopped = StopHardware();
        static_cast<void>(stopped);
        m_state = Usb1State::Faulted;
        if (m_phase != Phase::Idle)
        {
            Finish(Input::TransferStatus::Error);
        }
        return;
    }
    // Drain every root port's RW1C changes, including while idle, before the
    // component rearms INTx. GetPortStatus also initiates disconnect quiescence.
    for (UInt8 port{1U}; port <= m_ports; ++port)
    {
        static_cast<void>(GetPortStatus(port));
    }
    if (m_phase == Phase::Idle)
    {
        return;
    }
    if (!ReadPort(m_port).Connected && (m_result.Status != Input::TransferStatus::Disconnected))
    {
        Retire(Input::TransferStatus::Disconnected);
    }
    const bool reset = m_request.Operation == Input::UsbOperation::ResetPort;
    const UInt64 timeout = reset ? 1000U : m_request.TimeoutMilliseconds;
    if ((m_phase != Phase::Idle) && (timeout != 0U) && ((m_now - m_started) >= timeout) &&
        (m_phase != Phase::Quiescing) && (m_phase != Phase::AddressRecovery))
    {
        Retire(reset ? Input::TransferStatus::Retryable : Input::TransferStatus::TimedOut);
    }
    switch (m_phase)
    {
    case Phase::Idle:
        return;
    case Phase::PowerWait:
        if ((m_now - m_powerAt) >= m_powerDelay)
        {
            BeginReset(m_port);
            m_phase = Phase::ResetAssert;
            m_phaseAt = m_now;
        }
        return;
    case Phase::ResetAssert:
        if ((m_now - m_phaseAt) >= 50U)
        {
            EndReset(m_port);
            m_phase = Phase::ResetEnable;
            m_phaseAt = m_now;
        }
        return;
    case Phase::ResetEnable:
        if (((m_now - m_phaseAt) >= 10U) && ResetEnabled(m_port))
        {
            m_phase = Phase::ResetRecovery;
            m_phaseAt = m_now;
        }
        return;
    case Phase::ResetRecovery:
        if ((m_now - m_phaseAt) >= 10U)
        {
            Finish(Input::TransferStatus::Success);
        }
        return;
    case Phase::InterruptWait:
    {
        const Pipe& pipe = m_pipes[m_pipe - 1U];
        if (!pipe.Serviced || ((m_now - pipe.ServiceAt) >= pipe.Endpoint.Interval))
        {
            StartTransfer();
        }
        return;
    }
    case Phase::Transfer:
    {
        Usb1Progress result = Progress();
        if (result.Length > m_request.Length)
        {
            result = {.Complete = true, .Status = Input::TransferStatus::Error, .Length = 0U, .Toggle = false};
        }
        if (result.Complete)
        {
            Retire(result.Status, result.Length, result.Toggle);
        }
        return;
    }
    case Phase::Quiescing:
        if (!Quiesce())
        {
            if ((m_now - m_phaseAt) >= 100U)
            {
                const bool stopped = StopHardware();
                m_state = stopped ? Usb1State::Halted : Usb1State::Faulted;
                Finish(Input::TransferStatus::Error);
            }
            return;
        }
        if (m_request.Operation == Input::UsbOperation::InterruptIn)
        {
            m_pipes[m_pipe - 1U].Toggle = RetiredToggle();
            Size won{0U};
            if (((m_result.Status == Input::TransferStatus::Cancelled) ||
                 (m_result.Status == Input::TransferStatus::TimedOut)) &&
                WonReport(won) && (won <= m_request.Length))
            {
                // The device sent this report before the unlink took effect
                // and has moved on to its next toggle: deliver it.
                m_result.Status = Input::TransferStatus::Success;
                m_result.Length = won;
            }
        }
        if (m_result.Status == Input::TransferStatus::Success)
        {
            if ((m_request.Operation == Input::UsbOperation::InterruptIn) ||
                ((m_request.Setup.Bytes[0U] & 0x80U) != 0U))
            {
                for (Size i{0U}; i < m_result.Length; ++i)
                {
                    m_request.Data[i] = m_buffer.Cpu[64U + i];
                }
            }
            if ((m_request.Operation == Input::UsbOperation::Control) && (m_request.Setup.Bytes[0U] == 2U) &&
                (m_request.Setup.Bytes[1U] == 1U) && (m_request.Setup.Bytes[2U] == 0U) &&
                (m_request.Setup.Bytes[3U] == 0U) && (m_request.Setup.Bytes[5U] == 0U) && (m_request.Length == 0U))
            {
                // CLEAR_FEATURE(ENDPOINT_HALT) resets DATA0 on the pipe of
                // that endpoint of this device, whichever pipe sent it.
                for (Pipe& pipe : m_pipes)
                {
                    if (pipe.Configured && (pipe.Port == m_port) &&
                        (pipe.Endpoint.Address == m_request.Setup.Bytes[4U]))
                    {
                        pipe.Toggle = false;
                    }
                }
            }
            if (m_request.Operation == Input::UsbOperation::AssignAddress)
            {
                m_phase = Phase::AddressRecovery;
                m_phaseAt = m_now;
                return;
            }
        }
        Finish(m_result.Status, m_result.Length);
        return;
    case Phase::AddressRecovery:
        if ((m_now - m_phaseAt) >= 2U)
        {
            Finish(Input::TransferStatus::Success);
        }
        return;
    }
}
} // namespace Asteria::User::Usb
