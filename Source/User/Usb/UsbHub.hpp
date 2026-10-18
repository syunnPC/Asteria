// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_USBHUB_HPP_INCLUDED
#define ASTERIA_USER_USB_USBHUB_HPP_INCLUDED

#include "User/Input/UsbDevice.hpp"

namespace Asteria::User::Usb
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
enum class HubState : UInt8
{
    Detached,
    Reset,
    FirstDescriptor,
    EndpointZero,
    Address,
    DeviceDescriptor,
    ConfigurationHeader,
    Configuration,
    SetConfiguration,
    Descriptor,
    Power,
    PowerWait,
    Ready,
    ResetChild,
    ResetWait,
    ResetRecovery,
    Failed,
    UnsupportedDepth,
};
struct HubPortStatus
{
    bool Connected;
    bool Enabled;
    bool ResetComplete;
    Input::UsbSpeed Speed;
    UInt16 Changes;
};
/// One USB2 high/full-speed hub, <=15 ports, polling instead of interrupt IN.
/// One request at a time; three attempts, bounded 1000ms port-reset deadline.
/// Step/Complete are serialized with the host and use monotonic milliseconds.
/// Child default-address enumeration is serialized by the component: ResetChild,
/// wait for ResetComplete, then Attach a UsbDevice through UsbHubPort. Do not
/// reset another child until SET_ADDRESS and its settling interval complete.
/// On child disconnect, quiesce its controller pipe before core Disconnect.
class UsbHub final
{
public:
    [[nodiscard]] bool Attach(UInt8 address, UInt8 depth = 0U) noexcept;
    void Disconnect() noexcept;
    void Step(Input::IUsbHostController& host, UInt64 milliseconds) noexcept;
    [[nodiscard]] bool Complete(UInt64 token, Input::TransferStatus status, Size actualLength) noexcept;
    [[nodiscard]] bool ResetChild(UInt8 port) noexcept;
    [[nodiscard]] HubPortStatus GetPortStatus(UInt8 port) const noexcept;
    [[nodiscard]] Input::UsbRoute Route(UInt8 port) const noexcept;
    [[nodiscard]] HubState GetState() const noexcept
    {
        return m_state;
    }
    [[nodiscard]] UInt8 PortCount() const noexcept
    {
        return m_portCount;
    }
    [[nodiscard]] bool IsOutstanding() const noexcept
    {
        return m_outstanding;
    }

private:
    [[nodiscard]] bool Configuration(Size length) noexcept;
    [[nodiscard]] bool Descriptor(Size length) noexcept;
    void Advance(Size length) noexcept;
    Input::UsbRequest m_request{};
    HubPortStatus m_ports[15U]{};
    UInt8 m_buffer[Input::MaximumConfigurationBytes]{};
    HubState m_state{HubState::Detached};
    Input::UsbSpeed m_speed{Input::UsbSpeed::High};
    UInt64 m_token{0U};
    UInt64 m_now{0U};
    UInt64 m_waitStarted{0U};
    UInt64 m_resetStarted{0U};
    UInt64 m_nextPoll{0U};
    UInt16 m_packetSize0{64U};
    UInt16 m_configurationLength{0U};
    UInt16 m_powerMilliseconds{0U};
    UInt8 m_address{0U};
    UInt8 m_configuration{0U};
    UInt8 m_portCount{0U};
    UInt8 m_port{1U};
    UInt8 m_clearFeature{0U};
    UInt8 m_attempts{0U};
    bool m_outstanding{false};
    bool m_multiTt{false};
};
/// Adds cached hub child status to a controller's dedicated logical pipe view.
/// The underlying controller must understand routed requests (EHCI does).
class UsbHubPort final : public Input::IUsbHostController
{
public:
    UsbHubPort(UsbHub& hub, UInt8 port, Input::IUsbHostController& pipe) noexcept
        : m_hub{hub}, m_port{port}, m_pipe{pipe}
    {
    }
    [[nodiscard]] Input::PortStatus GetPortStatus() noexcept override
    {
        const HubPortStatus status = m_hub.GetPortStatus(m_port);
        return {.Connected = status.Connected && status.ResetComplete, .Speed = status.Speed};
    }
    [[nodiscard]] bool Submit(const Input::UsbRequest& request) noexcept override
    {
        m_request = request;
        m_request.Route = m_hub.Route(m_port);
        return m_pipe.Submit(m_request);
    }
    [[nodiscard]] bool Abort(UInt64 token) noexcept override
    {
        return m_pipe.Abort(token);
    }

private:
    UsbHub& m_hub;
    UInt8 m_port;
    Input::IUsbHostController& m_pipe;
    // Metadata scratch lives with the serialized view, keeping target frames bounded.
    Input::UsbRequest m_request{};
};
} // namespace Asteria::User::Usb
#endif
