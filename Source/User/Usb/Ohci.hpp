// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_USB_OHCI_HPP_INCLUDED
#define ASTERIA_USER_USB_OHCI_HPP_INCLUDED

#include "User/Usb/OhciStatus.hpp"

namespace Asteria::User::Usb
{
/// BAR-relative, little-endian dword MMIO; hooks order/register DMA visibility
/// and perform direction-aware cache maintenance over all supplied mappings.
class IOhciMmio
{
public:
    [[nodiscard]] virtual UInt32 Read32(UInt32 offset) noexcept = 0;
    virtual void Write32(UInt32 offset, UInt32 value) noexcept = 0;
    virtual void PublishDma() noexcept = 0;
    virtual void ObserveDma() noexcept = 0;

protected:
    ~IOhciMmio() = default;
};
struct OhciMemory
{
    DmaMemory Hcca;        // >=256 bytes, 256-byte device alignment
    DmaMemory Endpoints;   // >=32 bytes, 16-byte device alignment (control/interrupt ED)
    DmaMemory Descriptors; // >=64 bytes, 16-byte device alignment (3 TDs + dummy)
    DmaMemory Buffer;      // >=1088 bytes, 64-byte device alignment (setup + data)
};
/// OpenHCI 1.0a, <=15 root ports, FS/LS control and interrupt IN. Handoff must
/// succeed before Initialize/bus mastering. Pin disjoint DMA below 4 GiB until
/// successful Halt; no allocation. Halt uses HCR as the synchronous DMA barrier.
/// Poll drains at most three done TDs and waits two frames before descriptor
/// reuse. A stalled ED is rebuilt with H clear; interrupt carry is preserved.
class OhciController final : public Usb1Controller
{
public:
    explicit OhciController(IOhciMmio& mmio) noexcept : m_mmio{mmio}
    {
    }
    [[nodiscard]] Usb1Handoff Handoff(UInt32 barBytes, UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] bool Initialize(const OhciMemory& memory, UInt32 barBytes, UInt32 pollLimit = 1000U) noexcept;
    [[nodiscard]] bool Halt(UInt32 pollLimit = 1000U) noexcept;

private:
    [[nodiscard]] bool Wait(UInt32 offset, UInt32 mask, UInt32 expected) noexcept;
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
    void Td(Size index, UInt32 direction, UInt32 toggle, UInt32 buffer, Size length) noexcept;
    void ArmPeriodic(UInt32 frame) noexcept;
    IOhciMmio& m_mmio;
    OhciMemory m_memory{};
    Size m_lengths[3U]{};
    UInt32 m_buffers[3U]{};
    UInt32 m_pollLimit{1000U};
    UInt32 m_control{0x80U};
    UInt32 m_offFrame{0U};
    UInt32 m_serviceFrame{0U};
    UInt32 m_interval{1U};
    UInt32 m_seen{0U};
    Size m_count{0U};
    Size m_actual{0U};
    bool m_handoff{false};
    bool m_interrupt{false};
    bool m_stopping{false};
    bool m_armed{false};
    bool m_won{false};
};
using OhciPort = Usb1Port;
using OhciCompletion = Usb1Completion;
} // namespace Asteria::User::Usb
#endif
