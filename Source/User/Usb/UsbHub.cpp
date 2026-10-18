// SPDX-License-Identifier: Apache-2.0

#include "User/Usb/UsbHub.hpp"

namespace Asteria::User::Usb
{
namespace
{
constexpr Input::SetupPacket ClassRequest(const UInt8 type, const UInt8 request, const UInt16 value, const UInt8 port,
                                          const UInt8 length) noexcept
{
    return {{type, request, static_cast<UInt8>(value & 0xFFU), static_cast<UInt8>(value >> 8U), port, 0U, length, 0U}};
}
UInt16 Word(const UInt8* const bytes) noexcept
{
    return static_cast<UInt16>(UInt16{bytes[0U]} | (UInt16{bytes[1U]} << 8U));
}
}
bool UsbHub::Attach(const UInt8 address, const UInt8 depth) noexcept
{
    if (m_state != HubState::Detached)
    {
        return false;
    }
    if (depth != 0U)
    {
        m_state = HubState::UnsupportedDepth;
        return false;
    }
    if ((address == 0U) || (address > 127U))
    {
        return false;
    }
    m_address = address;
    m_state = HubState::Reset;
    return true;
}
void UsbHub::Disconnect() noexcept
{
    m_state = HubState::Detached;
    m_outstanding = false;
    m_attempts = 0U;
    m_portCount = 0U;
    m_port = 1U;
    m_clearFeature = 0U;
    m_nextPoll = 0U;
    for (HubPortStatus& port : m_ports)
    {
        port = {};
    }
}
HubPortStatus UsbHub::GetPortStatus(const UInt8 port) const noexcept
{
    if ((port == 0U) || (port > m_portCount))
    {
        return {};
    }
    return m_ports[port - 1U];
}
Input::UsbRoute UsbHub::Route(const UInt8 port) const noexcept
{
    if ((port == 0U) || (port > m_portCount))
    {
        return {};
    }
    return {
        .HubAddress = m_address, .HubPort = port, .Depth = 1U, .Speed = m_ports[port - 1U].Speed, .MultiTt = m_multiTt};
}
bool UsbHub::ResetChild(const UInt8 port) noexcept
{
    if ((m_state != HubState::Ready) || m_outstanding || (m_clearFeature != 0U) || (port == 0U) ||
        (port > m_portCount) || !m_ports[port - 1U].Connected)
    {
        return false;
    }
    m_port = port;
    m_ports[port - 1U].ResetComplete = false;
    m_attempts = 0U;
    m_resetStarted = m_now;
    m_state = HubState::ResetChild;
    return true;
}
void UsbHub::Step(Input::IUsbHostController& host, const UInt64 milliseconds) noexcept
{
    if (milliseconds < m_now)
    {
        return;
    }
    m_now = milliseconds;
    const Input::PortStatus root = host.GetPortStatus();
    if (!root.Connected)
    {
        Disconnect();
        return;
    }
    if (m_outstanding || (m_state == HubState::Detached) || (m_state == HubState::Failed) ||
        (m_state == HubState::UnsupportedDepth))
    {
        return;
    }
    if (((m_state == HubState::ResetWait) || (m_state == HubState::ResetRecovery)) &&
        ((m_now - m_resetStarted) >= 1000U))
    {
        m_state = HubState::Failed;
        return;
    }
    if (m_state == HubState::PowerWait)
    {
        if ((m_now - m_waitStarted) < m_powerMilliseconds)
        {
            return;
        }
        m_state = HubState::Ready;
        m_port = 1U;
    }
    if (m_state == HubState::ResetRecovery)
    {
        if ((m_now - m_waitStarted) < 10U)
        {
            return;
        }
        m_ports[m_port - 1U].ResetComplete = true;
        m_state = HubState::Ready;
    }
    if (((m_state == HubState::Ready) || (m_state == HubState::ResetWait)) && (m_now < m_nextPoll))
    {
        return;
    }
    m_request = {.Operation = Input::UsbOperation::Control,
                 .Token = 0U,
                 .Speed = m_speed,
                 .Address = m_address,
                 .PacketSize0 = m_packetSize0,
                 .Setup = {},
                 .Endpoint = {},
                 .Data = m_buffer,
                 .Length = 0U,
                 .TimeoutMilliseconds = 1000U};
    switch (m_state)
    {
    case HubState::Detached:
    case HubState::Failed:
    case HubState::UnsupportedDepth:
    case HubState::PowerWait:
    case HubState::ResetRecovery:
        return;
    case HubState::Reset:
        m_speed = root.Speed;
        m_packetSize0 = m_speed == Input::UsbSpeed::High ? 64U : 8U;
        m_request.Speed = m_speed;
        m_request.PacketSize0 = m_packetSize0;
        m_request.Operation = Input::UsbOperation::ResetPort;
        break;
    case HubState::FirstDescriptor:
        m_speed = root.Speed;
        m_packetSize0 = m_speed == Input::UsbSpeed::High ? 64U : 8U;
        m_request.Speed = m_speed;
        m_request.PacketSize0 = m_packetSize0;
        m_request.Address = 0U;
        m_request.Length = 8U;
        m_request.Setup = Input::UsbDescriptors::GetDescriptor(Input::DescriptorKind::Device, 0U, 8U);
        break;
    case HubState::EndpointZero:
        m_request.Operation = Input::UsbOperation::UpdateEndpointZero;
        m_request.Address = 0U;
        break;
    case HubState::Address:
        m_request.Operation = Input::UsbOperation::AssignAddress;
        m_request.Setup = Input::UsbDescriptors::SetAddress(m_address);
        break;
    case HubState::DeviceDescriptor:
        m_request.Length = 18U;
        m_request.Setup = Input::UsbDescriptors::GetDescriptor(Input::DescriptorKind::Device, 0U, 18U);
        break;
    case HubState::ConfigurationHeader:
        m_request.Length = 9U;
        m_request.Setup = Input::UsbDescriptors::GetDescriptor(Input::DescriptorKind::Configuration, 0U, 9U);
        break;
    case HubState::Configuration:
        m_request.Length = m_configurationLength;
        m_request.Setup =
            Input::UsbDescriptors::GetDescriptor(Input::DescriptorKind::Configuration, 0U, m_configurationLength);
        break;
    case HubState::SetConfiguration:
        m_request.Setup = Input::UsbDescriptors::SetConfiguration(m_configuration);
        break;
    case HubState::Descriptor:
        m_request.Length = 11U; // enough for <=15 ports: two two-byte bitmaps
        m_request.Setup = ClassRequest(0xA0U, 6U, 0x2900U, 0U, 11U);
        break;
    case HubState::Power:
        m_request.Setup = ClassRequest(0x23U, 3U, 8U, m_port, 0U);
        break;
    case HubState::ResetChild:
        m_request.Setup = ClassRequest(0x23U, 3U, 4U, m_port, 0U);
        break;
    case HubState::Ready:
    case HubState::ResetWait:
        if (m_clearFeature != 0U)
        {
            m_request.Setup = ClassRequest(0x23U, 1U, m_clearFeature, m_port, 0U);
        }
        else
        {
            m_request.Length = 4U;
            m_request.Setup = ClassRequest(0xA3U, 0U, 0U, m_port, 4U);
        }
        break;
    }
    if (m_token == ~UInt64{0U})
    {
        m_state = HubState::Failed;
        return;
    }
    m_request.Token = ++m_token;
    ++m_attempts;
    m_outstanding = host.Submit(m_request);
    if (!m_outstanding && (m_attempts >= 3U))
    {
        m_state = HubState::Failed;
    }
}
bool UsbHub::Configuration(const Size length) noexcept
{
    Input::ConfigurationHeader header{};
    if (!Input::UsbDescriptors::Configuration(m_buffer, length, header) || (length != header.TotalLength))
    {
        return false;
    }
    bool hubInterface{false};
    bool endpoint{false};
    for (Size offset{0U}; offset < length;)
    {
        const Size bytes = m_buffer[offset];
        if ((bytes < 2U) || (bytes > length - offset))
        {
            return false;
        }
        const UInt8 kind = m_buffer[offset + 1U];
        if (kind == 4U)
        {
            hubInterface = (bytes >= 9U) && (m_buffer[offset + 3U] == 0U) && (m_buffer[offset + 5U] == 9U) &&
                           (m_buffer[offset + 6U] == 0U) && (m_buffer[offset + 7U] <= 2U);
            if (hubInterface)
            {
                m_multiTt = m_buffer[offset + 7U] == 2U;
            }
        }
        if ((kind == 5U) && hubInterface)
        {
            endpoint = (bytes >= 7U) && ((m_buffer[offset + 2U] & 0xF0U) == 0x80U) &&
                       ((m_buffer[offset + 2U] & 15U) != 0U) && ((m_buffer[offset + 3U] & 3U) == 3U) &&
                       (Word(m_buffer + offset + 4U) != 0U) && (m_buffer[offset + 6U] != 0U);
        }
        offset += bytes;
    }
    m_configuration = header.Value;
    return endpoint;
}
bool UsbHub::Descriptor(const Size length) noexcept
{
    if ((length < 9U) || (m_buffer[1U] != 0x29U) || (m_buffer[2U] == 0U) || (m_buffer[2U] > 15U))
    {
        return false;
    }
    const Size expected = 7U + 2U * ((Size{m_buffer[2U]} + 8U) / 8U);
    if ((m_buffer[0U] != expected) || (length != expected) || ((Word(m_buffer + 3U) & 3U) == 3U))
    {
        return false;
    }
    m_portCount = m_buffer[2U];
    m_powerMilliseconds = static_cast<UInt16>(UInt16{m_buffer[5U]} * 2U + 100U); // power plus debounce
    return true;
}
void UsbHub::Advance(const Size length) noexcept
{
    switch (m_state)
    {
    case HubState::Detached:
    case HubState::Failed:
    case HubState::UnsupportedDepth:
    case HubState::PowerWait:
    case HubState::ResetRecovery:
        return;
    case HubState::Reset:
        m_state = HubState::FirstDescriptor;
        return;
    case HubState::FirstDescriptor:
        if ((length != 8U) || (m_buffer[0U] != 18U) || (m_buffer[1U] != 1U) ||
            !Input::UsbDescriptors::PacketSize0(m_buffer[7U], m_speed, m_packetSize0))
        {
            m_state = HubState::Failed;
            return;
        }
        m_state = HubState::EndpointZero;
        return;
    case HubState::EndpointZero:
        m_state = HubState::Address;
        return;
    case HubState::Address:
        m_state = HubState::DeviceDescriptor;
        return;
    case HubState::DeviceDescriptor:
    {
        Input::DeviceDescriptor descriptor{};
        UInt16 packet{0U};
        if (!Input::UsbDescriptors::Device(m_buffer, length, descriptor) || (descriptor.DeviceClass != 9U) ||
            !Input::UsbDescriptors::PacketSize0(descriptor.PacketSize0, m_speed, packet) || (packet != m_packetSize0) ||
            ((m_speed != Input::UsbSpeed::High) && (m_speed != Input::UsbSpeed::Full)))
        {
            m_state = HubState::Failed;
            return;
        }
        m_state = HubState::ConfigurationHeader;
        return;
    }
    case HubState::ConfigurationHeader:
    {
        Input::ConfigurationHeader header{};
        if (!Input::UsbDescriptors::Configuration(m_buffer, length, header))
        {
            m_state = HubState::Failed;
            return;
        }
        m_configurationLength = header.TotalLength;
        m_state = HubState::Configuration;
        return;
    }
    case HubState::Configuration:
        m_state = Configuration(length) ? HubState::SetConfiguration : HubState::Failed;
        return;
    case HubState::SetConfiguration:
        m_state = HubState::Descriptor;
        return;
    case HubState::Descriptor:
        m_state = Descriptor(length) ? HubState::Power : HubState::Failed;
        return;
    case HubState::Power:
        if (m_port < m_portCount)
        {
            ++m_port;
        }
        else
        {
            m_state = HubState::PowerWait;
            m_waitStarted = m_now;
        }
        return;
    case HubState::ResetChild:
        m_state = HubState::ResetWait;
        m_nextPoll = (m_now <= ~UInt64{0U} - 10U ? m_now + 10U : ~UInt64{0U});
        return;
    case HubState::Ready:
    case HubState::ResetWait:
    {
        HubPortStatus& port = m_ports[m_port - 1U];
        if (m_clearFeature != 0U)
        {
            port.Changes &= static_cast<UInt16>(~(UInt16{1U} << (m_clearFeature - 16U)));
            m_clearFeature = 0U;
        }
        else
        {
            if (length != 4U)
            {
                m_state = HubState::Failed;
                return;
            }
            const UInt16 status = Word(m_buffer);
            port.Connected = (status & 1U) != 0U;
            port.Enabled = (status & 2U) != 0U;
            port.Changes = Word(m_buffer + 2U) & 0x1FU;
            port.Speed = (status & (1U << 10U)) != 0U
                             ? Input::UsbSpeed::High
                             : ((status & (1U << 9U)) != 0U ? Input::UsbSpeed::Low : Input::UsbSpeed::Full);
            if (!port.Connected || !port.Enabled || ((port.Changes & 1U) != 0U))
            {
                port.ResetComplete = false;
            }
            if ((m_state == HubState::ResetWait) && port.Connected && port.Enabled && ((status & 16U) == 0U) &&
                ((port.Changes & 16U) != 0U))
            {
                m_state = HubState::ResetRecovery;
                m_waitStarted = m_now;
            }
        }
        for (UInt8 bit{0U}; bit < 5U; ++bit)
        {
            if ((port.Changes & (UInt16{1U} << bit)) != 0U)
            {
                m_clearFeature = static_cast<UInt8>(16U + bit);
                return;
            }
        }
        if (m_state == HubState::Ready)
        {
            m_port = m_port == m_portCount ? 1U : static_cast<UInt8>(m_port + 1U);
        }
        m_nextPoll = (m_now <= ~UInt64{0U} - 10U ? m_now + 10U : ~UInt64{0U});
        return;
    }
    }
}
bool UsbHub::Complete(const UInt64 token, const Input::TransferStatus status, const Size actualLength) noexcept
{
    if (!m_outstanding || (token != m_request.Token) || (actualLength > m_request.Length))
    {
        return false;
    }
    m_outstanding = false;
    switch (status)
    {
    case Input::TransferStatus::Disconnected:
        Disconnect();
        return true;
    case Input::TransferStatus::Cancelled:
        m_attempts = 0U;
        return true;
    case Input::TransferStatus::TimedOut:
    case Input::TransferStatus::Retryable:
        if (m_attempts >= 3U)
        {
            m_state = HubState::Failed;
        }
        return true;
    case Input::TransferStatus::Stall:
    case Input::TransferStatus::Error:
        m_state = HubState::Failed;
        return true;
    case Input::TransferStatus::Success:
        m_attempts = 0U;
        Advance(actualLength);
        return true;
    }
    return false;
}
} // namespace Asteria::User::Usb
