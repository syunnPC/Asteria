// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_XHCI_HPP_INCLUDED
#define ASTERIA_USER_USB_XHCI_HPP_INCLUDED

#include "User/Input/UsbDiagnostic.hpp"
#include "User/Usb/XhciEvent.hpp"

namespace Asteria::User::Usb
{
inline constexpr Size MaximumPorts{255U};
inline constexpr Size MaximumDevices{16U};
inline constexpr Size MaximumScratchpads{1023U};
inline constexpr Size MaximumEndpointPipes{8U};

/// BAR-relative little-endian accesses. The platform implementation owns MMIO
/// ordering, split-64 register rules, and cache maintenance of *all* registered
/// DMA mappings. PublishDma finishes CPU stores before a doorbell; ObserveDma
/// finishes device writes before CPU loads and preserves unpublished CPU
/// stores in CPU-owned regions. Cache maintenance must respect DMA direction;
/// never write stale event/output-context cache lines back over device writes.
/// Calls are serialized with Poll.
class IXhciMmio
{
protected:
    ~IXhciMmio() = default;

public:
    [[nodiscard]] virtual UInt32 Read32(UInt32 offset) noexcept = 0;
    [[nodiscard]] virtual UInt64 Read64(UInt32 offset) noexcept = 0;
    virtual void Write32(UInt32 offset, UInt32 value) noexcept = 0;
    /// Byte write at USBLEGSUP + 3, value one. Mandatory independent semaphore
    /// access: a dword read/modify/write can race firmware's BIOS-owned update.
    virtual void WriteOsOwned(UInt32 legacyOffset) noexcept = 0;
    virtual void Write64(UInt32 offset, UInt64 value) noexcept = 0;
    virtual void PublishDma() noexcept = 0;
    virtual void ObserveDma() noexcept = 0;
    /// Between failed synchronous polls. Native accessor waits one millisecond;
    /// hosted models may advance logical hardware time without blocking.
    virtual void PausePoll() noexcept
    {
    }
};
struct EndpointMemory
{
    DmaMemory Ring;   // 16..4096 TRBs including Link, 64-byte alignment
    DmaMemory Buffer; // 1024..65536 bytes, one 64KiB window, 64-byte alignment
};
struct PortMemory
{
    DmaMemory InputContext;  // 33 * context size, 64-byte device alignment
    DmaMemory DeviceContext; // 32 * context size, 64-byte alignment
    DmaMemory ControlRing;
    DmaMemory Buffer;            // EP0 bounce buffer, 1024..65536 bytes, one 64KiB window
    const EndpointMemory* Pipes; // exclusively owned resources for non-control endpoints
    Size PipeCount;              // 1..MaximumEndpointPipes, bound on simultaneous configured endpoints
};
struct ControllerMemory
{
    DmaMemory Dcbaa; // (managed slot count + 1) * 8, 64-byte alignment
    DmaMemory CommandRing;
    DmaMemory EventRing;          // 16..4096 TRBs; no Link
    DmaMemory Erst;               // >=16 bytes, 64-byte alignment
    DmaMemory ScratchpadArray;    // scratchpad count * 8, 64-byte alignment
    const DmaMemory* Scratchpads; // count pages of 4096 bytes, page-aligned
    Size ScratchpadCount;
    const PortMemory* Ports; // bounded pool; assigned only to attached root devices
    Size PortCount;          // device pool capacity, independent of hardware root-port count
};
enum class ControllerState : UInt8
{
    Uninitialized,
    Running,
    Halted,
    Faulted,
};
enum class EventDelivery : UInt8
{
    Interrupt,
    Poll,
};
struct Completion
{
    UInt64 Token;
    Input::TransferStatus Status;
    Size ActualLength;
    UInt8 EndpointAddress{0U};
};
struct PortChange
{
    UInt8 Port;
    Input::PortStatus Status;
};

/// No allocation or core ownership; the only callback is the optional
/// diagnostic sink, which receives at most a few records per event or port
/// scan. Store this owner outside the stack; memory descriptors remain alive
/// through Halt. Poll consumes <=budget
/// events (hard cap 64), scans <=255 ports, and uses caller's monotonic milliseconds
/// for finite operation deadlines. Initialize/Halt use bounded register polls.
/// Call Handoff before PCI bus mastering/DMA setup and Initialize. Root devices
/// only; EP0 plus up to eight interrupt IN/bulk IN/OUT endpoints. One outstanding
/// request and retained completion per pipe; pipes progress independently.
class XhciController final
{
public:
    explicit constexpr XhciController(IXhciMmio& mmio) noexcept : m_mmio{mmio}
    {
    }
    XhciController(const XhciController&) = delete;
    XhciController& operator=(const XhciController&) = delete;
    /// Bounded BAR capability walk and BIOS semaphore wait. Poll counts are not
    /// milliseconds; the accessor/caller supplies a cadence of at most one second.
    /// Failure permits neither reset nor firmware DMA reservation release.
    [[nodiscard]] bool Handoff(UInt32 barBytes, UInt32 pollLimit = 1000U) noexcept;
    /// After successful Handoff, stop firmware's schedule before attaching a
    /// replacement IOMMU domain. No reset or DMA-memory access occurs here.
    [[nodiscard]] bool QuiesceFirmware(UInt32 barBytes, UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] UInt16 Version() const noexcept
    {
        return m_version;
    }
    /// Optional; null disables records. The sink outlives every later call.
    void SetDiagnostics(Input::IUsbDiagnosticSink* sink) noexcept
    {
        m_diagnostics = sink;
    }
    [[nodiscard]] bool Initialize(const ControllerMemory& memory, UInt32 barBytes, UInt32 pollLimit = 1000U,
                                  EventDelivery delivery = EventDelivery::Interrupt) noexcept;
    /// Powers non-faulted root ports when HCCPARAMS1 advertises power control.
    /// Caller waits for power stabilization before beginning enumeration.
    void PowerPorts() noexcept;
    [[nodiscard]] bool Halt(UInt32 pollLimit = 1000U) noexcept;
    /// Establish the clock with Poll before the first Submit.
    void Poll(UInt64 milliseconds, Size eventBudget = 32U) noexcept;
    [[nodiscard]] bool Abort(UInt8 pipe, UInt64 token) noexcept;
    [[nodiscard]] bool Abort(UInt8 port, UInt8 endpointAddress, UInt64 token) noexcept;
    /// Serialized on EP0; target pipe must be idle with its completion consumed.
    /// Completes after device CLEAR_FEATURE and host reset/stop + dequeue/drain.
    [[nodiscard]] bool ClearHalt(UInt8 port, UInt8 endpointAddress, UInt64 token) noexcept;
    [[nodiscard]] bool TakeCompletion(UInt8 port, Completion& output) noexcept;
    [[nodiscard]] bool TakeCompletion(UInt8 port, UInt8 endpointAddress, Completion& output) noexcept;
    [[nodiscard]] bool TakePortChange(PortChange& output) noexcept;
    [[nodiscard]] Input::PortStatus GetPortStatus(UInt8 port) noexcept;
    [[nodiscard]] bool IsPortPoweredOff(UInt8 port) const noexcept
    {
        return (port != 0U) && (port <= m_portCount) && m_roots[port - 1U].PoweredOff;
    }
    /// Pool index, or MaximumDevices when the root has no resources assigned.
    [[nodiscard]] Size DeviceIndex(UInt8 port) const noexcept;
    /// After disconnect completions and decoder state are drained. A live slot,
    /// pending request or retained completion prevents resource reuse.
    [[nodiscard]] bool ReleasePort(UInt8 port) noexcept;
    [[nodiscard]] bool Submit(UInt8 port, const Input::UsbRequest& request) noexcept;
    [[nodiscard]] ControllerState GetState() const noexcept
    {
        return m_state;
    }
    /// Diagnostic counter wraps modulo 2^32.
    [[nodiscard]] UInt32 RejectedEvents() const noexcept
    {
        return m_rejected;
    }
    [[nodiscard]] UInt8 PortCount() const noexcept
    {
        return m_portCount;
    }

private:
    enum class Phase : UInt8
    {
        Idle,
        Reset,
        ResetRecovery,
        Enable,
        DefaultAddress,
        Context,
        Operation,
        AddressRecovery,
        ResetEndpoint,
        Dequeue,
        EndpointRecovery,
        Disable,
        StopEndpoint,
        AbortReset,
        AbortDequeue,
        AbortDrain,
    };
    struct Port
    {
        UInt8 RootNumber{0U};
        UInt8 SpeedIdentifier{0U};
        PortMemory Memory{};
        Input::EndpointDescriptor Descriptor{};
        UInt8 Index{0U};
        UInt8 RecoveryEndpoint{0U};
        bool Configured{false};
        XhciRing Control{};
        XhciRing Interrupt{};
        Input::UsbRequest Request{};
        Completion Result{};
        Input::UsbSpeed Speed{Input::UsbSpeed::Full};
        Phase Stage{Phase::Idle};
        UInt64 Started{0U};
        UInt64 RecoveryStarted{0U};
        UInt64 TransferPointers[3U]{};
        Size TransferCount{0U};
        Size ShortLength{0U};
        UInt8 Slot{0U};
        UInt8 Endpoint{0U};
        UInt8 ActiveEndpoint{0U};
        UInt8 SlotType{0U};
        bool SuperPort{false};
        bool PoweredOff{false};
        bool Busy{false};
        bool Ready{false};
        bool Connected{false};
        bool Change{false};
        bool Detaching{false};
        bool ShortSeen{false};
        bool ResetSeen{false};
        bool ResetIssued{false};
        bool StallRecovery{false};
        Input::TransferStatus AbortStatus{Input::TransferStatus::Cancelled};
    };
    struct RootPort
    {
        UInt8 Device{0U}; // one-based pool index; zero means unassigned
        UInt8 SlotType{0U};
        bool SuperPort{false};
        bool Protocol{false};
        bool PoweredOff{false};
        bool Present{false}; // last sampled CCS, for connect/disconnect records
        UInt8 Speeds[16U]{}; // UsbSpeed + 1; zero means unsupported/reserved
    };
    [[nodiscard]] Port& Root(UInt8 number) noexcept;
    [[nodiscard]] bool Wait(UInt32 offset, UInt32 mask, UInt32 value, UInt32 limit) noexcept;
    [[nodiscard]] bool Capabilities(UInt32 barBytes) noexcept;
    [[nodiscard]] bool ValidateMemory(const ControllerMemory& memory) const noexcept;
    [[nodiscard]] bool Protocols(UInt32 first, UInt32 barBytes) noexcept;
    [[nodiscard]] bool Command(Port& pipe, UInt8 port, UInt32 type, UInt64 parameter, UInt32 flags = 0U) noexcept;
    void BuildAddress(Port& port, UInt8 number) noexcept;
    [[nodiscard]] bool BuildEndpoint(Port& port) noexcept;
    void Transfer(Port& port) noexcept;
    [[nodiscard]] bool Cancel(UInt8 number, UInt64 token, Input::TransferStatus status) noexcept;
    void Finish(Port& port, Input::TransferStatus status, Size actual = 0U) noexcept;
    void HandleEvent(const XhciEvent& event) noexcept;
    void HandleCommand(Port& port, const XhciEvent& event) noexcept;
    void HandleTransfer(Port& port, UInt8 number, const XhciEvent& event) noexcept;
    void ScanPort(UInt8 number) noexcept;
    void StartReset(Port& port, UInt8 number) noexcept;
    void Progress(Port& pipe, UInt8 number) noexcept;
    [[nodiscard]] Port* FindPipe(UInt8 port, UInt8 endpointAddress) noexcept;
    [[nodiscard]] Port& Pipe(UInt8 port, Size index) noexcept;
    [[nodiscard]] XhciRing& TransferRing(Port& pipe) noexcept;
    [[nodiscard]] bool CancelPipe(Port& pipe, UInt64 token, Input::TransferStatus status) noexcept;
    void RetirePipes(UInt8 port, Input::TransferStatus status) noexcept;
    void FailController(Input::TransferStatus status) noexcept;
    [[nodiscard]] UInt32 PortOffset(UInt8 number) const noexcept;
    void Record(Input::UsbDiagnosticKind kind, UInt8 port, UInt8 detail, UInt8 code, UInt32 value0,
                UInt32 value1 = 0U) noexcept;
    IXhciMmio& m_mmio;
    Input::IUsbDiagnosticSink* m_diagnostics{nullptr};
    ControllerState m_state{ControllerState::Uninitialized};
    ControllerMemory m_memory{};
    XhciRing m_commands{};
    RootPort m_roots[MaximumPorts]{};
    Port m_ports[MaximumDevices]{};
    Port m_endpointPipes[MaximumDevices][MaximumEndpointPipes]{};
    // Per pool entry: attached while running, so reset waits for the attach
    // debounce measured from m_attachedAt.
    UInt64 m_attachedAt[MaximumDevices]{};
    bool m_settling[MaximumDevices]{};
    UInt32 m_operational{0U};
    UInt32 m_runtime{0U};
    UInt32 m_doorbells{0U};
    UInt32 m_contextBytes{32U};
    UInt32 m_rejected{0U};
    UInt32 m_scratchpads{0U};
    UInt32 m_pollLimit{1000U};
    UInt16 m_version{0U};
    UInt32 m_handoffBar{0U};
    UInt64 m_commandPointer{0U};
    UInt64 m_commandStarted{0U};
    UInt64 m_now{0U};
    Size m_eventIndex{0U};
    UInt32 m_eventCycle{1U};
    UInt8 m_commandPort{0U};
    UInt8 m_commandType{0U};
    Port* m_commandPipe{nullptr};
    UInt8 m_portCount{0U};
    UInt8 m_maxSlots{0U};
    UInt8 m_slotCount{0U};
    bool m_address64{false};
    bool m_initialScan{false};
    EventDelivery m_delivery{EventDelivery::Interrupt};
};

/// A permanent view of one controller root port (numbered from one).
class XhciPort final : public Input::IUsbHostController
{
public:
    XhciPort(XhciController& controller, const UInt8 port) noexcept : m_controller{controller}, m_port{port}
    {
    }
    [[nodiscard]] Input::PortStatus GetPortStatus() noexcept override
    {
        return m_controller.GetPortStatus(m_port);
    }
    [[nodiscard]] bool Submit(const Input::UsbRequest& request) noexcept override
    {
        return m_controller.Submit(m_port, request);
    }
    [[nodiscard]] bool Abort(UInt64 token) noexcept override
    {
        return m_controller.Abort(m_port, token);
    }

private:
    XhciController& m_controller;
    UInt8 m_port;
};
} // namespace Asteria::User::Usb
#endif
