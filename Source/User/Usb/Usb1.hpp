// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_USB1_HPP_INCLUDED
#define ASTERIA_USER_USB_USB1_HPP_INCLUDED

#include "User/Input/UsbDevice.hpp"
#include "User/Usb/XhciMemory.hpp"

namespace Asteria::User::Usb
{
enum class Usb1State : UInt8
{
    Uninitialized,
    Running,
    Halted,
    Faulted,
};
enum class Usb1Handoff : UInt8
{
    Success,
    BiosTimeout,
    InvalidAccess,
};
struct Usb1Completion
{
    UInt64 Token;
    Input::TransferStatus Status;
    Size ActualLength;
};
/// Logical pipes: each holds one DATA toggle, one interrupt IN endpoint and
/// one retained completion. A device uses one for its control requests and
/// one per interrupt IN endpoint it polls (a composite keyboard and mouse
/// needs three), all on its root port.
inline constexpr UInt8 Usb1MaximumPipes{16U};
struct Usb1Progress
{
    bool Complete;
    Input::TransferStatus Status;
    Size Length;
    bool Toggle;
};
/// Shared USB1 request policy. One request controller-wide and one retained
/// completion per logical pipe (1..Usb1MaximumPipes), each pipe bound to the
/// root port (1..15) of its last request. Serialized, no allocation/callbacks/syscalls.
/// Poll receives monotonic milliseconds, at least once per millisecond while
/// DMA is active. Keep owners in component storage. Only root FS/LS pipes.
/// Core buffers are borrowed until completion; hardware only sees bounce DMA.
class Usb1Controller
{
public:
    Usb1Controller(const Usb1Controller&) = delete;
    Usb1Controller& operator=(const Usb1Controller&) = delete;
    [[nodiscard]] Input::PortStatus GetPortStatus(UInt8 port) noexcept;
    /// request on logical pipe for the device on root port. False only for a
    /// retryable refusal (another request is accepted, or the pipe still
    /// holds a completion), retaining nothing.
    [[nodiscard]] bool Submit(UInt8 pipe, UInt8 port, const Input::UsbRequest& request) noexcept;
    /// The root port's own pipe: pipe number equals port number.
    [[nodiscard]] bool Submit(UInt8 port, const Input::UsbRequest& request) noexcept
    {
        return Submit(port, port, request);
    }
    /// Cancels pipe's accepted request. An interrupt IN report that reached
    /// the bounce buffer before the unlink took effect completes as Success
    /// with its data instead of Cancelled, so the device's next toggle and
    /// the delivered reports stay consistent and no report is lost.
    [[nodiscard]] bool Abort(UInt8 pipe, UInt64 token) noexcept;
    [[nodiscard]] bool TakeCompletion(UInt8 pipe, Usb1Completion& output) noexcept;
    void Poll(UInt64 milliseconds) noexcept;
    /// Whether an accepted request has not completed yet: the next Submit
    /// would be refused.
    [[nodiscard]] bool IsBusy() const noexcept
    {
        return m_phase != Phase::Idle;
    }
    [[nodiscard]] Usb1State GetState() const noexcept
    {
        return m_state;
    }

protected:
    Usb1Controller() noexcept = default;
    ~Usb1Controller() = default;
    void Ready(UInt8 ports, DmaMemory buffer, UInt32 powerDelay) noexcept;
    void Stopped(bool success) noexcept;
    [[nodiscard]] virtual Input::PortStatus ReadPort(UInt8 port) noexcept = 0;
    virtual void BeginReset(UInt8 port) noexcept = 0;
    virtual void EndReset(UInt8 port) noexcept = 0;
    [[nodiscard]] virtual bool ResetEnabled(UInt8 port) noexcept = 0;
    virtual void Build(const Input::UsbRequest& request, bool toggle) noexcept = 0;
    [[nodiscard]] virtual Usb1Progress Progress() noexcept = 0;
    /// Called until schedules are quiescent; never reuse descriptors sooner.
    [[nodiscard]] virtual bool Quiesce() noexcept = 0;
    [[nodiscard]] virtual bool RetiredToggle() noexcept = 0;
    /// After Quiesce succeeded for a cancelled interrupt IN: true, with the
    /// report's length, when its TD completed successfully before the unlink
    /// took effect. The report is then in the bounce buffer at offset 64.
    [[nodiscard]] virtual bool WonReport(Size& length) noexcept = 0;
    [[nodiscard]] virtual bool ControllerError() noexcept = 0;
    [[nodiscard]] virtual bool StopHardware() noexcept = 0;
    void Finish(Input::TransferStatus status, Size length = 0U) noexcept;

private:
    enum class Phase : UInt8
    {
        Idle,
        PowerWait,
        ResetAssert,
        ResetEnable,
        ResetRecovery,
        InterruptWait,
        Transfer,
        Quiescing,
        AddressRecovery,
    };
    struct Pipe
    {
        Usb1Completion Completion{};
        Input::EndpointDescriptor Endpoint{};
        UInt64 ServiceAt{0U};
        bool Ready{false};
        bool Configured{false};
        bool Toggle{false};
        bool Serviced{false};
        UInt8 Port{0U};
    };
    void StartTransfer() noexcept;
    void Retire(Input::TransferStatus status, Size length = 0U, bool toggle = false) noexcept;
    Pipe m_pipes[Usb1MaximumPipes]{};
    Input::UsbRequest m_request{};
    DmaMemory m_buffer{};
    Usb1Progress m_result{};
    Usb1State m_state{Usb1State::Uninitialized};
    Phase m_phase{Phase::Idle};
    UInt64 m_now{0U};
    UInt64 m_started{0U};
    UInt64 m_phaseAt{0U};
    UInt64 m_powerAt{0U};
    UInt32 m_powerDelay{0U};
    UInt8 m_ports{0U};
    UInt8 m_port{0U};
    UInt8 m_pipe{0U};
};
/// Core-facing borrowed view of one pipe on one root port. One UsbDevice per
/// root port, unique USB address; by default the pipe is the port's own.
class Usb1Port final : public Input::IUsbHostController
{
public:
    Usb1Port(Usb1Controller& controller, UInt8 port) noexcept : m_controller{controller}, m_pipe{port}, m_port{port}
    {
    }
    Usb1Port(Usb1Controller& controller, UInt8 pipe, UInt8 port) noexcept
        : m_controller{controller}, m_pipe{pipe}, m_port{port}
    {
    }
    [[nodiscard]] Input::PortStatus GetPortStatus() noexcept override
    {
        return m_controller.GetPortStatus(m_port);
    }
    [[nodiscard]] bool Submit(const Input::UsbRequest& request) noexcept override
    {
        return m_controller.Submit(m_pipe, m_port, request);
    }
    [[nodiscard]] bool Abort(UInt64 token) noexcept override
    {
        return m_controller.Abort(m_pipe, token);
    }

private:
    Usb1Controller& m_controller;
    UInt8 m_pipe;
    UInt8 m_port;
};
/// Checks whole CPU/device mappings, not only the schedule-used prefixes.
[[nodiscard]] bool Usb1MemoryValid(const DmaMemory* regions, Size count) noexcept;
[[nodiscard]] bool Usb1Limit(UInt32 limit) noexcept;
} // namespace Asteria::User::Usb
#endif
