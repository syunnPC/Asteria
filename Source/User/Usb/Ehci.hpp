// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_EHCI_HPP_INCLUDED
#define ASTERIA_USER_USB_EHCI_HPP_INCLUDED

#include "User/Usb/EhciStatus.hpp"

namespace Asteria::User::Usb
{
/// BAR-relative 32-bit LE MMIO and PCI-configuration-space legacy registers.
/// Delay/clock scheduling belongs to the component. Cache hooks cover every
/// mapping with direction-aware maintenance, like IXhciMmio. Serialized calls.
class IEhciMmio
{
public:
    [[nodiscard]] virtual UInt32 Read32(UInt32 offset) noexcept = 0;
    virtual void Write32(UInt32 offset, UInt32 value) noexcept = 0;
    [[nodiscard]] virtual UInt32 ReadConfig32(UInt32 offset) noexcept = 0;
    virtual void WriteConfig32(UInt32 offset, UInt32 value) noexcept = 0;
    virtual void PublishDma() noexcept = 0;
    virtual void ObserveDma() noexcept = 0;

protected:
    ~IEhciMmio() = default;
};
enum class EhciState : UInt8
{
    Uninitialized,
    Running,
    Halted,
    Faulted,
};
enum class HandoffStatus : UInt8
{
    Success,
    BiosTimeout,
    InvalidCapability,
};
struct EhciMemory
{
    DmaMemory Frames;      // 1024 entries, 4096-byte device alignment
    DmaMemory AsyncHead;   // >=96 bytes, 32-byte device alignment
    DmaMemory Queue;       // >=96 bytes, 32-byte device alignment
    DmaMemory Descriptors; // >=3 * 64 bytes, 32-byte device alignment
    DmaMemory Buffer;      // >=1088 bytes, 64-byte device alignment
};
struct EhciCompletion
{
    UInt64 Token;
    Input::TransferStatus Status;
    Size ActualLength;
};
/// Allocation-free, one active request globally, <=16 logical device pipes,
/// <=15 hardware ports, one interrupt endpoint per pipe. Caller pins exclusive
/// nonoverlapping DMA until Halt succeeds; owner lives outside the stack.
/// Handoff is an explicit prerequisite, before PCI bus-master enable/reset.
/// Poll uses monotonic caller milliseconds; register waits have finite bounds.
/// HS bInterval 15/16 require Poll at least every millisecond to gate a single
/// periodic frame and prevent a second service on the 1024-frame list wrap.
/// Routed ResetPort acknowledges a hub port already reset by UsbHub.
class EhciController final
{
public:
    explicit EhciController(IEhciMmio& mmio) noexcept : m_mmio{mmio}
    {
    }
    EhciController(const EhciController&) = delete;
    EhciController& operator=(const EhciController&) = delete;
    [[nodiscard]] HandoffStatus Handoff(UInt32 barBytes, UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] bool Initialize(const EhciMemory& memory, UInt32 barBytes, UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] bool Halt(UInt32 pollLimit = 1000U) noexcept;
    void Poll(UInt64 milliseconds) noexcept;
    [[nodiscard]] Input::PortStatus GetPortStatus(UInt8 rootPort) noexcept;
    [[nodiscard]] bool Submit(UInt8 pipe, UInt8 rootPort, const Input::UsbRequest& request) noexcept;
    [[nodiscard]] bool Abort(UInt8 pipe, UInt64 token) noexcept;
    [[nodiscard]] bool TakeCompletion(UInt8 pipe, EhciCompletion& output) noexcept;
    [[nodiscard]] EhciState GetState() const noexcept
    {
        return m_state;
    }
    /// Whether an accepted request has not completed yet: the next Submit
    /// would be refused.
    [[nodiscard]] bool IsBusy() const noexcept
    {
        return m_phase != Phase::Idle;
    }

private:
    enum class Phase : UInt8
    {
        Idle,
        ResetAssert,
        ResetWait,
        ResetRecovery,
        Transfer,
        AddressRecovery,
        InterruptWait,
        Quiescing,
    };
    struct Pipe
    {
        EhciCompletion Result{};
        Input::EndpointDescriptor Endpoint{};
        Input::UsbSpeed Speed{Input::UsbSpeed::Full};
        bool Ready{false};
        bool Toggle{false};
        bool Configured{false};
        bool PhaseSet{false};
        Size FramePhase{0U};
        UInt64 ServiceAt{0U};
        UInt8 Root{0U};
    };
    [[nodiscard]] bool Wait(UInt32 offset, UInt32 mask, UInt32 expected, UInt32 limit) noexcept;
    [[nodiscard]] bool ScheduleOff() noexcept;
    [[nodiscard]] bool MemoryValid(const EhciMemory& memory) const noexcept;
    [[nodiscard]] UInt32 PortOffset(UInt8 port) const noexcept;
    void PortWrite(UInt8 port, UInt32 clear, UInt32 set) noexcept;
    void Finish(Input::TransferStatus status, Size length = 0U) noexcept;
    void Fail(Input::TransferStatus status) noexcept;
    void BuildTransfer() noexcept;
    void Descriptor(Size index, UInt32 next, UInt32 alternate, UInt32 token, UInt64 buffer) noexcept;
    void ProgressTransfer() noexcept;
    void Retire(Input::TransferStatus status) noexcept;
    void ClearTranslator(Input::TransferStatus status) noexcept;
    IEhciMmio& m_mmio;
    EhciMemory m_memory{};
    Pipe m_pipes[16U]{};
    Input::PortStatus m_ports[15U]{};
    Input::UsbRequest m_request{};
    Input::UsbRequest m_original{};
    Input::TransferStatus m_recoveryStatus{Input::TransferStatus::Error};
    bool m_recovering{false};
    EhciState m_state{EhciState::Uninitialized};
    Phase m_phase{Phase::Idle};
    UInt64 m_now{0U};
    UInt64 m_started{0U};
    UInt64 m_phaseStarted{0U};
    UInt32 m_operational{0U};
    UInt32 m_pollLimit{1000U};
    UInt8 m_portCount{0U};
    UInt8 m_pipe{0U};
    UInt8 m_root{0U};
    bool m_address64{false};
    bool m_powerControl{false};
    bool m_handoff{false};
    Size m_interruptFrames{1U};
    UInt64 m_nextService{0U};
    UInt32 m_unlinkFrame{0U};
    bool m_asyncUnlink{false};
    Input::TransferStatus m_abortStatus{Input::TransferStatus::Cancelled};
};
class EhciPort final : public Input::IUsbHostController
{
public:
    EhciPort(EhciController& controller, UInt8 pipe, UInt8 rootPort = 1U) noexcept
        : m_controller{controller}, m_pipe{pipe}, m_root{rootPort}
    {
    }
    [[nodiscard]] Input::PortStatus GetPortStatus() noexcept override
    {
        return m_controller.GetPortStatus(m_root);
    }
    [[nodiscard]] bool Submit(const Input::UsbRequest& request) noexcept override
    {
        return m_controller.Submit(m_pipe, m_root, request);
    }
    [[nodiscard]] bool Abort(UInt64 token) noexcept override
    {
        return m_controller.Abort(m_pipe, token);
    }

private:
    EhciController& m_controller;
    UInt8 m_pipe;
    UInt8 m_root;
};
} // namespace Asteria::User::Usb
#endif
