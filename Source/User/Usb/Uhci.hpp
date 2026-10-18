// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_UHCI_HPP_INCLUDED
#define ASTERIA_USER_USB_UHCI_HPP_INCLUDED

#include "User/Usb/UhciStatus.hpp"

namespace Asteria::User::Usb
{
/// I/O-BAR-relative port operations, implemented by PortIo syscalls on either
/// ISA, never an MMIO mapping. Hooks include DMA ordering/cache maintenance.
class IUhciPortIo
{
public:
    [[nodiscard]] virtual UInt8 Read8(UInt32 offset) noexcept = 0;
    [[nodiscard]] virtual UInt16 Read16(UInt32 offset) noexcept = 0;
    [[nodiscard]] virtual UInt32 Read32(UInt32 offset) noexcept = 0;
    virtual void Write8(UInt32 offset, UInt8 value) noexcept = 0;
    virtual void Write16(UInt32 offset, UInt16 value) noexcept = 0;
    virtual void Write32(UInt32 offset, UInt32 value) noexcept = 0;
    virtual void PublishDma() noexcept = 0;
    virtual void ObserveDma() noexcept = 0;

protected:
    ~IUhciPortIo() = default;
};
/// The component asks the trusted platform to perform PCI config reads/writes;
/// this is separate from the I/O BAR. False means denied/unavailable access.
class IUhciConfig
{
public:
    [[nodiscard]] virtual bool ReadConfig16(UInt32 offset, UInt16& value) noexcept = 0;
    [[nodiscard]] virtual bool WriteConfig16(UInt32 offset, UInt16 value) noexcept = 0;

protected:
    ~IUhciConfig() = default;
};
struct UhciMemory
{
    DmaMemory Frames;      // >=4096 bytes, 4096-byte device alignment
    DmaMemory Queues;      // >=32 bytes, 16-byte device alignment (interrupt -> control)
    DmaMemory Descriptors; // >=2080 bytes, 16-byte device alignment (SETUP +128 DATA +STATUS)
    DmaMemory Buffer;      // >=1088 bytes, 64-byte device alignment
};
/// UHCI 1.1, two root ports. Pin exclusive disjoint DMA below 4 GiB until
/// successful Halt. Handoff disables LEGSUP SMI/trap enables and acknowledges
/// RW1C statuses while enabling PIRQ; UHCI has no BIOS-owned semaphore. Bounded
/// readback refuses firmware reassertion. Initialize cannot bypass handoff.
class UhciController final : public Usb1Controller
{
public:
    UhciController(IUhciPortIo& ports, IUhciConfig& config) noexcept : m_io{ports}, m_config{config}
    {
    }
    [[nodiscard]] Usb1Handoff Handoff(UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] bool Initialize(const UhciMemory& memory, UInt32 ioBytes, UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] bool Halt(UInt32 pollLimit = 1000U) noexcept;

private:
    [[nodiscard]] bool Wait(UInt32 offset, UInt16 mask, UInt16 expected) noexcept;
    [[nodiscard]] Input::PortStatus ReadPort(UInt8 port) noexcept override;
    void BeginReset(UInt8 port) noexcept override;
    void EndReset(UInt8 port) noexcept override;
    [[nodiscard]] bool ResetEnabled(UInt8 port) noexcept override;
    void Build(const Input::UsbRequest& request, bool toggle) noexcept override;
    [[nodiscard]] Usb1Progress Progress() noexcept override;
    [[nodiscard]] bool Quiesce() noexcept override;
    [[nodiscard]] bool RetiredToggle() noexcept override;
    [[nodiscard]] bool WonReport(Size& length) noexcept override;
    [[nodiscard]] bool ControllerError() noexcept override;
    [[nodiscard]] bool StopHardware() noexcept override;
    void PortWrite(UInt8 port, UInt16 clear, UInt16 set) noexcept;
    void Td(Size index, UInt32 pid, bool toggle, UInt32 buffer, Size length, UInt32 address) noexcept;
    void ArmPeriodic(UInt32 frame) noexcept;
    [[nodiscard]] bool Pause() noexcept;
    void Resume() noexcept;
    IUhciPortIo& m_io;
    IUhciConfig& m_config;
    UhciMemory m_memory{};
    UInt32 m_pollLimit{1000U};
    UInt32 m_serviceFrame{0U};
    UInt32 m_interval{1U};
    Size m_count{0U};
    Size m_scan{0U};
    Size m_actual{0U};
    Size m_length{0U};
    UInt32 m_packet{8U};
    bool m_handoff{false};
    bool m_interrupt{false};
    bool m_input{false};
    bool m_toggle{false};
    bool m_lowSpeed{false};
    UInt32 m_endpoint{0U};
    bool m_armed{false};
    bool m_stopping{false};
    bool m_won{false};
    Size m_wonLength{0U};
    UInt32 m_offFrame{0U};
};
using UhciPort = Usb1Port;
using UhciCompletion = Usb1Completion;
} // namespace Asteria::User::Usb
#endif
