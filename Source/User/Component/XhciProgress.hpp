// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_XHCI_PROGRESS_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_XHCI_PROGRESS_HPP_INCLUDED

#include "User/Input/UsbDiagnostic.hpp"
#include "User/System/SystemCallClient.hpp"

namespace Asteria::User::Component
{
/// The xHCI driver's bounded queue of enumeration diagnostics for the root
/// service's screen (User/Component/UsbProgressFrame.hpp). Recording copies
/// into a fixed ring; Flush sends them on the report Endpoint without
/// blocking and keeps what the Endpoint refuses for the next call. Budgets
/// bound the output: PortRecords per root port between connects and
/// TotalRecords per incarnation, each announced once by a Suppressed record,
/// as are records lost to a full ring. Disabled until Configure enables it.
/// Constructed in place after entry (it has a vtable); single-threaded.
class XhciProgress final : public Input::IUsbDiagnosticSink
{
public:
    static constexpr Foundation::Size Capacity{64U};
    static constexpr Foundation::UInt16 PortRecords{48U};
    static constexpr Foundation::UInt32 TotalRecords{400U};
    /// Frames sent per Flush; the report Endpoint queues four per sender.
    static constexpr Foundation::Size FlushLimit{4U};

    constexpr XhciProgress() noexcept = default;
    XhciProgress(const XhciProgress&) = delete;
    XhciProgress& operator=(const XhciProgress&) = delete;
    void Configure(Foundation::UInt8 controller, bool enabled) noexcept;
    void Record(const Input::UsbDiagnostic& record) noexcept override;
    /// frame holds at least XhciProgressFrameBytes. A send error other than
    /// a full queue disables the queue: diagnostics never stop the driver.
    void Flush(const System::SystemCallClient& client, System::CapabilityHandle root,
               Foundation::UInt8* frame) noexcept;
    [[nodiscard]] Foundation::Size Pending() const noexcept
    {
        return m_count;
    }
    [[nodiscard]] bool Enabled() const noexcept
    {
        return m_enabled;
    }

private:
    void Push(const Input::UsbDiagnostic& record) noexcept;
    Input::UsbDiagnostic m_ring[Capacity]{};
    Foundation::UInt16 m_portRecords[256U]{};
    Foundation::Size m_head{0U};
    Foundation::Size m_count{0U};
    Foundation::UInt64 m_transaction{0U};
    Foundation::UInt32 m_total{0U};
    Foundation::UInt32 m_lost{0U};
    Foundation::UInt8 m_controller{0U};
    bool m_enabled{false};
};
} // namespace Asteria::User::Component

#endif // ASTERIA_USER_COMPONENT_XHCI_PROGRESS_HPP_INCLUDED
