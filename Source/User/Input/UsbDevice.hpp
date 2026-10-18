// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_USBDEVICE_HPP_INCLUDED
#define ASTERIA_USER_INPUT_USBDEVICE_HPP_INCLUDED

#include "User/Input/HidBoot.hpp"
#include "User/Input/HidRelativeMouse.hpp"
#include "User/Input/UsbDescriptors.hpp"
#include "User/Input/UsbDiagnostic.hpp"

namespace Asteria::User::Input
{
inline constexpr Size MaximumBootInterfaces{8U};
// A core instance and its buffers belong to one device pipe (root or one hub child).
// Keep the instance and EventBatch in static/component storage, not a stack frame.
enum class UsbOperation : UInt8
{
    ResetPort,
    UpdateEndpointZero,
    AssignAddress,
    Control,
    ConfigureEndpoint,
    InterruptIn,
    BulkIn,
    BulkOut
};
enum class TransferStatus : UInt8
{
    Success,
    Retryable,
    Stall,
    Error,
    Disconnected,
    Cancelled,
    TimedOut
};
enum class UsbState : UInt8
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
    SetProtocol,
    SetIdle,
    ConfigureEndpoint,
    Polling,
    Failed,
    UnsupportedHubDepth,
    ReportDescriptor,
};
struct PortStatus
{
    bool Connected;
    UsbSpeed Speed;
};
struct UsbRoute
{
    UInt8 HubAddress{0U};
    UInt8 HubPort{0U};
    UInt8 Depth{0U};
    UsbSpeed Speed{UsbSpeed::Full};
    bool MultiTt{false};
};
struct UsbRequest
{
    UsbOperation Operation;
    Foundation::UInt64 Token;
    UsbSpeed Speed;
    UInt8 Address;
    UInt16 PacketSize0;
    SetupPacket Setup;
    EndpointDescriptor Endpoint;
    UInt8* Data;
    Size Length;
    UsbRoute Route{};
    // Relative to acceptance, in caller-clock milliseconds; zero means no deadline.
    UInt32 TimeoutMilliseconds{0U};
};

/// Nonblocking, allocation-free controller seam. Submit copies metadata and
/// returns false only for a retryable resource refusal, retaining nothing.
/// Accepted requests complete exactly once, including reset/address/context
/// commands. Transfers use the caller-selected timeout; expiration reports TimedOut
/// after pipe DMA is quiescent. Controller commands/reset retain bounded fault timers.
/// Data is core-owned and borrowed until Complete; actualLength never exceeds
/// Length. The adapter supplies validated enums, serialized with Step/Complete.
///
/// AssignAddress: OHCI/UHCI/EHCI send Setup (SET_ADDRESS), wait the USB settling
/// interval, then change their pipe address. xHCI instead runs Address Device
/// (BSR=0; controller chooses the address; Request.Address is a logical tag). ResetPort
/// creates the default xHCI slot/EP0 context (Address Device with BSR=1); UpdateEndpointZero runs Evaluate
/// Context. ConfigureEndpoint receives the complete selected endpoint fields
/// and speed (including SS companion), sufficient for Configure Endpoint.
/// InterruptIn is ED/TD on OHCI, QH/TD on UHCI, QH/qTD on EHCI, transfer TRBs
/// on xHCI; the adapters own DMA, toggles, scheduling and short-packet handling.
/// EHCI hands low/full speed root devices to companions. One-level children
/// carry hub address/port and negotiated speed for transaction translators.
///
/// Before Disconnect or a Disconnected completion, the adapter must revoke all
/// retained buffer borrows and quiesce DMA. This is a precondition, not a wait
/// in the pure core. Delayed completions are rejected by monotonic tokens.
class IUsbHostController
{
protected:
    ~IUsbHostController() = default;

public:
    [[nodiscard]] virtual PortStatus GetPortStatus() noexcept = 0;
    [[nodiscard]] virtual bool Submit(const UsbRequest& request) noexcept = 0;
    /// Cancel a matching pending transfer. False means stale, completed, or a
    /// noncancellable controller operation. True retains the borrow until its
    /// single Cancelled completion, after the hardware retirement handshake.
    /// Completed SET_ADDRESS settling and an existing retirement are not pending transfers.
    [[nodiscard]] virtual bool Abort(Foundation::UInt64 token) noexcept = 0;
};

/// Caller assigns 1..127 before Step. Enumeration is serialized; polling has
/// one outstanding report per boot interface, up to MaximumBootInterfaces.
/// Step submits at most one operation; Complete advances one state, returns
/// report events via output, and never submits. Three total attempts per
/// operation/report, reset after success. Polling stalls are reportable through
/// Complete without failing the device. A stalled SET_IDLE is tolerated (it is
/// optional for boot mice, HID 1.11 Appendix G); a stalled SET_PROTOCOL drops
/// only that interface, whose reports would not be in boot format. Other
/// enumeration stalls and Error are fatal.
/// Enumeration selects a 1000-ms timeout; interrupt IN waits without a deadline.
/// The optional diagnostic sink receives at most one record per Step or
/// Complete, plus one per interface when a configuration is read.
class UsbDevice final
{
public:
    constexpr UsbDevice() noexcept = default;
    explicit constexpr UsbDevice(UInt32 source) noexcept : m_source{source}
    {
    }
    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;
    /// Optional; null disables records. The sink outlives every later call.
    void SetDiagnostics(IUsbDiagnosticSink* sink) noexcept
    {
        m_diagnostics = sink;
    }
    [[nodiscard]] bool Attach(UInt8 address, UsbRoute route = {}) noexcept;
    void Disconnect() noexcept;
    /// Before Disconnect, release one interface's held keys. Replaces output;
    /// out-of-range interfaces produce none. Adapter publishes these releases.
    void ReleaseKeys(Size interface, EventBatch& output) noexcept;
    void Step(IUsbHostController& host) noexcept;
    [[nodiscard]] bool Abort(IUsbHostController& host) noexcept;
    [[nodiscard]] bool Complete(Foundation::UInt64 token, TransferStatus status, Size actualLength,
                                EventBatch& output) noexcept;
    [[nodiscard]] UsbState GetState() const noexcept
    {
        return m_state;
    }
    [[nodiscard]] bool IsOutstanding() const noexcept
    {
        if (m_outstanding)
        {
            return true;
        }
        for (Size i{0U}; i < m_interfaceCount; ++i)
        {
            if (m_bootPipes[i].Outstanding)
            {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] Size InterfaceCount() const noexcept
    {
        return m_interfaceCount;
    }
    [[nodiscard]] const BootInterface* GetInterface(Size index) const noexcept
    {
        return index < m_interfaceCount ? &m_bootPipes[index].Interface : nullptr;
    }
    [[nodiscard]] const BootInterface& GetInterface() const noexcept
    {
        return m_interface;
    }

private:
    void Retry() noexcept;
    void Advance(Size actualLength) noexcept;
    void DropInterface() noexcept;
    void StartInterface() noexcept;
    void Trace(UsbDiagnosticKind kind, UInt8 detail, UInt8 code, UInt32 value0, UInt32 value1 = 0U,
               Foundation::UInt64 data = 0U) const noexcept;
    [[nodiscard]] bool Accept(Foundation::UInt64 token, TransferStatus status, Size actualLength,
                              EventBatch& output) noexcept;
    struct BootPipe
    {
        BootInterface Interface{};
        HidBoot Hid{};
        HidRelativeMouse Relative{};
        bool ReportProtocol{false};
        Foundation::UInt64 Token{0U};
        Size Requested{0U};
        UInt8 Attempts{0U};
        UInt8 Diagnostics{0U}; // error and rejected-report records this attachment
        bool Reported{false};  // first report recorded this attachment
        bool Outstanding{false};
        UInt8 Buffer[MaximumInterruptBytes]{};
    };
    void PollInterface(IUsbHostController& host) noexcept;
    [[nodiscard]] bool CompleteReport(Foundation::UInt64 token, TransferStatus status, Size actual,
                                      EventBatch& output) noexcept;
    [[nodiscard]] bool BindConfiguration(Size length) noexcept;
    BootPipe m_bootPipes[MaximumBootInterfaces]{};
    Size m_interfaceCount{0U};
    Size m_activeInterface{0U};
    Size m_pollInterface{0U};
    UInt32 m_source{0U};
    UsbRoute m_route{};
    UsbState m_state{UsbState::Detached};
    UsbSpeed m_speed{UsbSpeed::Full};
    UInt8 m_address{0U};
    UInt8 m_attempts{0U};
    UInt8 m_configurationIndex{0U};
    UInt8 m_configurationCount{0U};
    UInt16 m_packetSize0{8U};
    UInt16 m_configurationLength{0U};
    Foundation::UInt64 m_token{0U};
    Size m_requestedLength{0U};
    bool m_outstanding{false};
    IUsbDiagnosticSink* m_diagnostics{nullptr};
    BootInterface m_interface{};
    UInt8 m_buffer[MaximumConfigurationBytes]{};
};
} // namespace Asteria::User::Input

#endif
