// SPDX-License-Identifier: Apache-2.0

#include "User/Input/UsbDevice.hpp"

#include "User/Input/UsbConfiguration.hpp"

namespace Asteria::User::Input
{
static_assert(HidRelativeMouse::MaximumDescriptorBytes <= MaximumConfigurationBytes);
bool UsbDevice::Attach(const UInt8 address, const UsbRoute route) noexcept
{
    if ((m_state != UsbState::Detached) || IsOutstanding() || (address == 0U) || (address > 127U))
    {
        return false;
    }
    if ((route.Depth > 1U) ||
        ((route.Depth == 1U) && ((route.HubAddress == 0U) || (route.HubAddress > 127U) || (route.HubPort == 0U) ||
                                 (route.HubPort > 15U) || (route.Speed == UsbSpeed::Super))))
    {
        return false;
    }
    Disconnect();
    m_route = route;
    m_address = address;
    m_state = UsbState::Reset;
    return true;
}

void UsbDevice::ReleaseKeys(const Size interface, EventBatch& output) noexcept
{
    output.Count = 0U;
    if (interface < m_interfaceCount)
    {
        auto& pipe = m_bootPipes[interface];
        if (pipe.ReportProtocol)
        {
            pipe.Relative.Release(m_source, output);
        }
        else
        {
            pipe.Hid.Release(output);
        }
    }
}

void UsbDevice::Disconnect() noexcept
{
    m_state = UsbState::Detached;
    m_route = {};
    m_outstanding = false;
    m_attempts = 0U;
    m_configurationIndex = 0U;
    m_configurationCount = 0U;
    m_configurationLength = 0U;
    m_interface = {};
    for (Size i{0U}; i < m_interfaceCount; ++i)
    {
        m_bootPipes[i].Hid.Reset();
        m_bootPipes[i].Outstanding = false;
        m_bootPipes[i].Attempts = 0U;
    }
    m_interfaceCount = 0U;
    m_activeInterface = 0U;
    m_pollInterface = 0U;
}

void UsbDevice::Retry() noexcept
{
    if (m_attempts >= 3U)
    {
        m_state = UsbState::Failed;
    }
}

void UsbDevice::Trace(const UsbDiagnosticKind kind, const UInt8 detail, const UInt8 code, const UInt32 value0,
                      const UInt32 value1, const Foundation::UInt64 data) const noexcept
{
    if (m_diagnostics != nullptr)
    {
        m_diagnostics->Record({.Kind = kind,
                               .Port = m_address,
                               .Detail = detail,
                               .Code = code,
                               .Value0 = value0,
                               .Value1 = value1,
                               .Data = data});
    }
}

void UsbDevice::StartInterface() noexcept
{
    m_state = m_bootPipes[m_activeInterface].ReportProtocol ? UsbState::ReportDescriptor : UsbState::SetProtocol;
    if (m_state == UsbState::ReportDescriptor &&
        (m_interface.ReportDescriptorLength == 0U ||
         m_interface.ReportDescriptorLength > HidRelativeMouse::MaximumDescriptorBytes))
    {
        DropInterface();
    }
}

void UsbDevice::DropInterface() noexcept
{
    for (Size i{m_activeInterface}; i + 1U < m_interfaceCount; ++i)
    {
        m_bootPipes[i] = m_bootPipes[i + 1U];
    }
    --m_interfaceCount;
    if (m_activeInterface < m_interfaceCount)
    {
        m_interface = m_bootPipes[m_activeInterface].Interface;
        StartInterface();
        return;
    }
    if (m_interfaceCount == 0U)
    {
        m_state = UsbState::Failed;
        return;
    }
    m_interface = m_bootPipes[0U].Interface;
    m_state = UsbState::Polling;
    Trace(UsbDiagnosticKind::Polling, static_cast<UInt8>(m_interfaceCount), 0U, 0U);
}

void UsbDevice::Step(IUsbHostController& host) noexcept
{
    const PortStatus port = host.GetPortStatus();
    if (!port.Connected)
    {
        Disconnect();
        return;
    }
    if (m_outstanding || (m_state == UsbState::Detached) || (m_state == UsbState::Failed) ||
        (m_state == UsbState::UnsupportedHubDepth))
    {
        return;
    }
    if (m_state == UsbState::Polling)
    {
        PollInterface(host);
        return;
    }
    UsbRequest request{.Operation = UsbOperation::Control,
                       .Token = 0U,
                       .Speed = m_speed,
                       .Address = m_address,
                       .PacketSize0 = m_packetSize0,
                       .Setup = {},
                       .Endpoint = {},
                       .Data = m_buffer,
                       .Length = 0U,
                       .Route = m_route,
                       .TimeoutMilliseconds = 1000U};
    switch (m_state)
    {
    case UsbState::Detached:
    case UsbState::Failed:
    case UsbState::UnsupportedHubDepth:
        return;
    case UsbState::Reset:
        m_speed = m_route.Depth == 0U ? port.Speed : m_route.Speed;
        switch (m_speed)
        {
        case UsbSpeed::Low:
        case UsbSpeed::Full:
            m_packetSize0 = 8U;
            break;
        case UsbSpeed::High:
            m_packetSize0 = 64U;
            break;
        case UsbSpeed::Super:
            m_packetSize0 = 512U;
            break;
        }
        request.Speed = m_speed;
        request.PacketSize0 = m_packetSize0;
        request.Operation = UsbOperation::ResetPort;
        break;
    case UsbState::FirstDescriptor:
        // Reset can reveal the negotiated speed; the pre-reset value is provisional.
        m_speed = m_route.Depth == 0U ? port.Speed : m_route.Speed;
        switch (m_speed)
        {
        case UsbSpeed::Low:
        case UsbSpeed::Full:
            m_packetSize0 = 8U;
            break;
        case UsbSpeed::High:
            m_packetSize0 = 64U;
            break;
        case UsbSpeed::Super:
            m_packetSize0 = 512U;
            break;
        }
        request.Speed = m_speed;
        request.PacketSize0 = m_packetSize0;
        request.Address = 0U;
        request.Length = 8U;
        request.Setup = UsbDescriptors::GetDescriptor(DescriptorKind::Device, 0U, 8U);
        break;
    case UsbState::EndpointZero:
        request.Address = 0U;
        request.Operation = UsbOperation::UpdateEndpointZero;
        break;
    case UsbState::Address:
        request.Operation = UsbOperation::AssignAddress;
        request.Setup = UsbDescriptors::SetAddress(m_address);
        break;
    case UsbState::DeviceDescriptor:
        request.Length = 18U;
        request.Setup = UsbDescriptors::GetDescriptor(DescriptorKind::Device, 0U, 18U);
        break;
    case UsbState::ConfigurationHeader:
        request.Length = 9U;
        request.Setup = UsbDescriptors::GetDescriptor(DescriptorKind::Configuration, m_configurationIndex, 9U);
        break;
    case UsbState::Configuration:
        request.Length = m_configurationLength;
        request.Setup =
            UsbDescriptors::GetDescriptor(DescriptorKind::Configuration, m_configurationIndex, m_configurationLength);
        break;
    case UsbState::SetConfiguration:
        request.Setup = UsbDescriptors::SetConfiguration(m_interface.Configuration);
        break;
    case UsbState::ReportDescriptor:
        request.Length = m_interface.ReportDescriptorLength;
        request.Setup = {.Bytes = {0x81U, 6U, 0U, 0x22U, m_interface.Number, 0U, static_cast<UInt8>(request.Length),
                                   static_cast<UInt8>(request.Length >> 8U)}};
        break;
    case UsbState::SetProtocol:
        request.Setup = UsbDescriptors::SetProtocol(m_interface.Number);
        break;
    case UsbState::SetIdle:
        request.Setup = UsbDescriptors::SetIdle(m_interface.Number);
        break;
    case UsbState::ConfigureEndpoint:
        request.Operation = UsbOperation::ConfigureEndpoint;
        request.Endpoint = m_interface.Endpoint;
        break;
    case UsbState::Polling:
        return;
    }
    // Never wrap tokens: an old completion must not alias a new request.
    if (m_token == ~Foundation::UInt64{0U})
    {
        m_state = UsbState::Failed;
        return;
    }
    request.Token = ++m_token;
    ++m_attempts;
    m_requestedLength = request.Length;
    m_outstanding = host.Submit(request);
    if (!m_outstanding)
    {
        const UsbState before = m_state;
        Retry();
        if (m_state == UsbState::Failed)
        {
            Trace(UsbDiagnosticKind::Failed, static_cast<UInt8>(before), UsbDiagnosticRefused, 0U);
        }
    }
}

void UsbDevice::Advance(const Size actualLength) noexcept
{
    switch (m_state)
    {
    case UsbState::Detached:
    case UsbState::Failed:
    case UsbState::UnsupportedHubDepth:
    case UsbState::Polling:
        return;
    case UsbState::Reset:
        m_state = UsbState::FirstDescriptor;
        return;
    case UsbState::FirstDescriptor:
        if ((actualLength != 8U) || (m_buffer[0U] != 18U) || (m_buffer[1U] != 1U) ||
            !UsbDescriptors::PacketSize0(m_buffer[7U], m_speed, m_packetSize0))
        {
            m_state = UsbState::Failed;
            return;
        }
        Trace(UsbDiagnosticKind::EndpointZero, 0U, 0U, m_buffer[7U]);
        m_state = UsbState::EndpointZero;
        return;
    case UsbState::EndpointZero:
        m_state = UsbState::Address;
        return;
    case UsbState::Address:
        m_state = UsbState::DeviceDescriptor;
        return;
    case UsbState::DeviceDescriptor:
    {
        DeviceDescriptor device{};
        UInt16 packet = 0U;
        if (!UsbDescriptors::Device(m_buffer, actualLength, device))
        {
            m_state = UsbState::Failed;
            return;
        }
        Trace(UsbDiagnosticKind::Device, device.PacketSize0, device.DeviceClass,
              UInt32{device.Vendor} | (UInt32{device.Product} << 16U),
              UInt32{device.UsbVersion} | (UInt32{device.ConfigurationCount} << 16U));
        if (!UsbDescriptors::PacketSize0(device.PacketSize0, m_speed, packet) || (packet != m_packetSize0))
        {
            m_state = UsbState::Failed;
            return;
        }
        if (device.DeviceClass == 9U)
        {
            m_state = m_route.Depth == 0U ? UsbState::Failed : UsbState::UnsupportedHubDepth;
            return;
        }
        m_configurationCount = device.ConfigurationCount;
        m_state = UsbState::ConfigurationHeader;
        return;
    }
    case UsbState::ConfigurationHeader:
    {
        ConfigurationHeader header{};
        if (!UsbDescriptors::Configuration(m_buffer, actualLength, header))
        {
            m_state = UsbState::Failed;
            return;
        }
        m_configurationLength = header.TotalLength;
        m_state = UsbState::Configuration;
        return;
    }
    case UsbState::Configuration:
        if ((actualLength != m_configurationLength) || !BindConfiguration(actualLength))
        {
            // Try the next bounded configuration; unsupported devices fail.
            if (m_configurationIndex >= m_configurationCount)
            {
                m_state = UsbState::Failed;
                return;
            }
            ++m_configurationIndex;
            m_state = m_configurationIndex < m_configurationCount ? UsbState::ConfigurationHeader : UsbState::Failed;
            return;
        }
        for (Size i{0U}; i < m_interfaceCount; ++i)
        {
            const EndpointDescriptor& endpoint = m_bootPipes[i].Interface.Endpoint;
            const UInt16 packet = endpoint.MaxPacketSize & 0x7FFU;
            const UInt8 interval = endpoint.Interval;
            bool valid = false;
            switch (m_speed)
            {
            case UsbSpeed::Low:
                valid = packet <= 8U;
                break;
            case UsbSpeed::Full:
                valid = packet <= 64U;
                break;
            case UsbSpeed::High:
                valid = interval <= 16U;
                break;
            case UsbSpeed::Super:
                valid = (interval <= 16U) && endpoint.HasCompanion && (endpoint.MaxBurst <= 15U) &&
                        (endpoint.CompanionAttributes == 0U) && (endpoint.BytesPerInterval != 0U) &&
                        (endpoint.BytesPerInterval <= packet * (endpoint.MaxBurst + 1U));
                break;
            }
            if (((m_speed != UsbSpeed::High) && ((endpoint.MaxPacketSize & 0x1800U) != 0U)) || !valid)
            {
                m_state = UsbState::Failed;
                return;
            }
        }
        m_state = UsbState::SetConfiguration;
        return;
    case UsbState::SetConfiguration:
        Trace(UsbDiagnosticKind::Request, 9U, static_cast<UInt8>(TransferStatus::Success), m_interface.Configuration);
        StartInterface();
        return;
    case UsbState::ReportDescriptor:
        if (actualLength != m_interface.ReportDescriptorLength ||
            !m_bootPipes[m_activeInterface].Relative.Open(m_buffer, actualLength))
        {
            DropInterface();
            return;
        }
        m_state = UsbState::SetIdle;
        return;
    case UsbState::SetProtocol:
        Trace(UsbDiagnosticKind::Request, 11U, static_cast<UInt8>(TransferStatus::Success), m_interface.Number);
        m_state = UsbState::SetIdle;
        return;
    case UsbState::SetIdle:
        Trace(UsbDiagnosticKind::Request, 10U, static_cast<UInt8>(TransferStatus::Success), m_interface.Number);
        m_state = UsbState::ConfigureEndpoint;
        return;
    case UsbState::ConfigureEndpoint:
        ++m_activeInterface;
        if (m_activeInterface < m_interfaceCount)
        {
            m_interface = m_bootPipes[m_activeInterface].Interface;
            StartInterface();
        }
        else
        {
            m_interface = m_bootPipes[0U].Interface;
            m_state = UsbState::Polling;
            Trace(UsbDiagnosticKind::Polling, static_cast<UInt8>(m_interfaceCount), 0U, 0U);
        }
        return;
    }
}

bool UsbDevice::BindConfiguration(const Size length) noexcept
{
    UsbConfiguration configuration{};
    const DescriptorError parsed = UsbConfiguration::Parse(m_buffer, length, configuration);
    if (parsed != DescriptorError::None)
    {
        Trace(UsbDiagnosticKind::Configuration, length >= 6U ? m_buffer[5U] : UInt8{0U}, static_cast<UInt8>(parsed),
              static_cast<UInt32>(length), length >= 5U ? m_buffer[4U] : 0U);
        // Malformed input is a device failure, not an invitation to partially bind.
        m_configurationIndex = m_configurationCount;
        return false;
    }
    m_interfaceCount = 0U;
    for (Size i{0U}; i < configuration.InterfaceCount(); ++i)
    {
        InterfaceBinding binding{};
        const BindingError bound = configuration.Bind(i, binding);
        InterfaceDescriptor shown{};
        if (configuration.Interface(i, shown) && (shown.Alternate == 0U))
        {
            EndpointDescriptor endpoint{};
            const bool hid = (bound == BindingError::None) && (binding.Class != InterfaceClass::MassStorage);
            if (hid)
            {
                endpoint = binding.In;
            }
            else if (!configuration.Endpoint(shown, 0U, endpoint))
            {
                endpoint = {};
            }
            Trace(UsbDiagnosticKind::Interface, shown.Number, hid ? 1U : 0U,
                  UInt32{shown.Class} | (UInt32{shown.Subclass} << 8U) | (UInt32{shown.Protocol} << 16U),
                  UInt32{endpoint.Address} | (UInt32{endpoint.MaxPacketSize} << 8U) |
                      (UInt32{endpoint.Interval} << 24U));
        }
        if ((bound != BindingError::None) || (binding.Class == InterfaceClass::MassStorage))
        {
            continue;
        }
        if (m_interfaceCount == MaximumBootInterfaces)
        {
            m_interfaceCount = 0U;
            m_configurationIndex = m_configurationCount;
            return false;
        }
        BootPipe& pipe = m_bootPipes[m_interfaceCount++];
        pipe.Interface = {.Configuration = configuration.Header().Value,
                          .Number = binding.Number,
                          .Protocol = binding.Class == InterfaceClass::BootKeyboard ? BootProtocol::Keyboard
                                                                                    : BootProtocol::Mouse,
                          .ReportDescriptorLength = binding.ReportLength,
                          .Endpoint = binding.In};
        pipe.Hid = HidBoot{m_source};
        pipe.ReportProtocol = binding.Class == InterfaceClass::ReportMouse;
        pipe.Diagnostics = 0U;
        pipe.Reported = false;
    }
    Trace(UsbDiagnosticKind::Configuration, configuration.Header().Value, static_cast<UInt8>(DescriptorError::None),
          configuration.Header().TotalLength,
          static_cast<UInt32>(configuration.InterfaceCount() & 0xFFU) | (static_cast<UInt32>(m_interfaceCount) << 8U));
    if (m_interfaceCount == 0U)
    {
        return false;
    }
    m_interface = m_bootPipes[0U].Interface;
    m_activeInterface = 0U;
    return true;
}
void UsbDevice::PollInterface(IUsbHostController& host) noexcept
{
    for (Size i{0U}; i < m_interfaceCount; ++i)
    {
        const Size selected = m_pollInterface;
        m_pollInterface = (m_pollInterface + 1U) % m_interfaceCount;
        BootPipe& pipe = m_bootPipes[selected];
        if (pipe.Outstanding)
        {
            continue;
        }
        if (m_token == ~Foundation::UInt64{0U})
        {
            m_state = UsbState::Failed;
            return;
        }
        pipe.Token = ++m_token;
        pipe.Requested = pipe.Interface.Endpoint.MaxPacketSize & 0x7FFU;
        UsbRequest request{.Operation = UsbOperation::InterruptIn,
                           .Token = pipe.Token,
                           .Speed = m_speed,
                           .Address = m_address,
                           .PacketSize0 = m_packetSize0,
                           .Setup = {},
                           .Endpoint = pipe.Interface.Endpoint,
                           .Data = pipe.Buffer,
                           .Length = pipe.Requested,
                           .Route = m_route,
                           .TimeoutMilliseconds = 0U};
        ++pipe.Attempts;
        pipe.Outstanding = host.Submit(request);
        if (!pipe.Outstanding && (pipe.Attempts >= 3U))
        {
            m_state = UsbState::Failed;
            Trace(UsbDiagnosticKind::Failed, static_cast<UInt8>(UsbState::Polling), UsbDiagnosticRefused, 0U);
        }
        return;
    }
}
bool UsbDevice::CompleteReport(const Foundation::UInt64 token, const TransferStatus status, const Size actual,
                               EventBatch& output) noexcept
{
    for (Size i{0U}; i < m_interfaceCount; ++i)
    {
        BootPipe& pipe = m_bootPipes[i];
        if (!pipe.Outstanding || (pipe.Token != token))
        {
            continue;
        }
        pipe.Outstanding = false;
        if (m_state != UsbState::Polling)
        {
            return true; // Parked completions after failure/detach never emit input.
        }
        if ((status != TransferStatus::Success) && (status != TransferStatus::Disconnected) &&
            (status != TransferStatus::Cancelled) && (pipe.Diagnostics < 4U))
        {
            ++pipe.Diagnostics;
            Trace(UsbDiagnosticKind::ReportError, pipe.Interface.Number, static_cast<UInt8>(status), 0U);
        }
        switch (status)
        {
        case TransferStatus::Disconnected:
            // Other borrows are returned separately by the controller's slot
            // retirement. Complete must accept them even after logical detach.
            m_state = UsbState::Detached;
            return true;
        case TransferStatus::Error:
            m_state = UsbState::Failed;
            return true;
        case TransferStatus::TimedOut:
        case TransferStatus::Retryable:
            if (pipe.Attempts >= 3U)
            {
                m_state = UsbState::Failed;
            }
            return true;
        case TransferStatus::Cancelled:
        case TransferStatus::Stall:
            pipe.Attempts = 0U;
            return true;
        case TransferStatus::Success:
            break;
        }
        if (actual > pipe.Requested)
        {
            m_state = UsbState::Failed;
            return true;
        }
        pipe.Attempts = 0U;
        ReportResult result{ReportResult::Accepted};
        switch (pipe.Interface.Protocol)
        {
        case BootProtocol::Keyboard:
            result = pipe.Hid.Keyboard(pipe.Buffer, actual, output);
            break;
        case BootProtocol::Mouse:
            result = pipe.ReportProtocol ? pipe.Relative.Mouse(pipe.Buffer, actual, m_source, output)
                                         : pipe.Hid.Mouse(pipe.Buffer, actual, output);
            break;
        }
        // The first report of each interface, and a few rejected ones, show
        // whether the device really sends boot-format reports.
        const bool rejected = (result != ReportResult::Accepted) && (pipe.Diagnostics < 4U);
        if (!pipe.Reported || rejected)
        {
            pipe.Diagnostics = static_cast<UInt8>(pipe.Diagnostics + (rejected ? 1U : 0U));
            pipe.Reported = true;
            Foundation::UInt64 bytes{0U};
            for (Size byte{0U}; (byte < 8U) && (byte < actual); ++byte)
            {
                bytes |= Foundation::UInt64{pipe.Buffer[byte]} << (byte * 8U);
            }
            Trace(UsbDiagnosticKind::Report, pipe.Interface.Number, static_cast<UInt8>(result),
                  static_cast<UInt32>(actual), 0U, bytes);
        }
        return true;
    }
    return false;
}
bool UsbDevice::Abort(IUsbHostController& host) noexcept
{
    if (m_outstanding)
    {
        return host.Abort(m_token);
    }
    for (Size i{0U}; i < m_interfaceCount; ++i)
    {
        if (m_bootPipes[i].Outstanding && host.Abort(m_bootPipes[i].Token))
        {
            return true;
        }
    }
    return false;
}
bool UsbDevice::Complete(const Foundation::UInt64 token, const TransferStatus status, const Size actualLength,
                         EventBatch& output) noexcept
{
    const UsbState before = m_state;
    const bool accepted = Accept(token, status, actualLength, output);
    if (accepted && (m_state == UsbState::Failed) && (before != UsbState::Failed))
    {
        Trace(UsbDiagnosticKind::Failed, static_cast<UInt8>(before), static_cast<UInt8>(status),
              static_cast<UInt32>(actualLength));
    }
    return accepted;
}
bool UsbDevice::Accept(const Foundation::UInt64 token, const TransferStatus status, const Size actualLength,
                       EventBatch& output) noexcept
{
    output.Count = 0U;
    if ((m_state == UsbState::Polling) || !m_outstanding)
    {
        return CompleteReport(token, status, actualLength, output);
    }
    if (!m_outstanding || (token != m_token))
    {
        return false;
    }
    m_outstanding = false;
    switch (status)
    {
    case TransferStatus::Disconnected:
        Disconnect();
        return true;
    case TransferStatus::Cancelled:
        m_attempts = 0U;
        return true;
    case TransferStatus::Stall:
        if (m_state == UsbState::SetIdle)
        {
            // SET_IDLE is optional for boot mice (HID 1.11 section 7.2.4 and
            // Appendix G); a device without it still reports on change.
            Trace(UsbDiagnosticKind::Request, 10U, static_cast<UInt8>(status), m_interface.Number);
            m_attempts = 0U;
            m_state = UsbState::ConfigureEndpoint;
            return true;
        }
        if (m_state == UsbState::SetProtocol)
        {
            // Without boot protocol this interface's reports are not in the
            // boot format; keep the device's other boot interfaces.
            Trace(UsbDiagnosticKind::Request, 11U, static_cast<UInt8>(status), m_interface.Number);
            m_attempts = 0U;
            if (m_interface.Protocol == BootProtocol::Mouse)
            {
                m_bootPipes[m_activeInterface].ReportProtocol = true;
                StartInterface();
            }
            else
            {
                DropInterface();
            }
            return true;
        }
        if (m_state == UsbState::ReportDescriptor)
        {
            DropInterface();
            return true;
        }
        m_state = UsbState::Failed;
        return true;
    case TransferStatus::Error:
        m_state = UsbState::Failed;
        return true;
    case TransferStatus::TimedOut:
    case TransferStatus::Retryable:
        Retry();
        return true;
    case TransferStatus::Success:
        break;
    }
    if (actualLength > m_requestedLength)
    {
        m_state = UsbState::Failed;
        return true;
    }
    m_attempts = 0U;
    Advance(actualLength);
    return true;
}
} // namespace Asteria::User::Input
