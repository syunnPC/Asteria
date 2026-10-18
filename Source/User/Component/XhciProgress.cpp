// SPDX-License-Identifier: Apache-2.0

#include "User/Component/XhciProgress.hpp"

#include "User/Component/UsbProgressFrame.hpp"

namespace Asteria::User::Component
{
void XhciProgress::Configure(const Foundation::UInt8 controller, const bool enabled) noexcept
{
    m_controller = controller;
    m_enabled = enabled;
}

void XhciProgress::Push(const Input::UsbDiagnostic& record) noexcept
{
    if (m_count == Capacity)
    {
        ++m_lost; // diagnostic count; saturation is irrelevant at 2^32 records
        return;
    }
    m_ring[(m_head + m_count) % Capacity] = record;
    ++m_count;
}

void XhciProgress::Record(const Input::UsbDiagnostic& record) noexcept
{
    if (!m_enabled)
    {
        return;
    }
    if (record.Kind == Input::UsbDiagnosticKind::Connect)
    {
        m_portRecords[record.Port] = 0U;
    }
    if (m_total > TotalRecords)
    {
        return;
    }
    if (m_total == TotalRecords)
    {
        ++m_total;
        Push({.Kind = Input::UsbDiagnosticKind::Suppressed,
              .Port = 0U,
              .Detail = 1U,
              .Code = 0U,
              .Value0 = 0U,
              .Value1 = 0U,
              .Data = 0U});
        return;
    }
    Foundation::UInt16& port = m_portRecords[record.Port];
    if ((record.Port != 0U) && (port > PortRecords))
    {
        return;
    }
    ++m_total;
    if ((record.Port != 0U) && (port++ == PortRecords))
    {
        Push({.Kind = Input::UsbDiagnosticKind::Suppressed,
              .Port = record.Port,
              .Detail = 0U,
              .Code = 0U,
              .Value0 = 0U,
              .Value1 = 0U,
              .Data = 0U});
        return;
    }
    Push(record);
}

void XhciProgress::Flush(const System::SystemCallClient& client, const System::CapabilityHandle root,
                         Foundation::UInt8* const frame) noexcept
{
    for (Foundation::Size sent{0U}; m_enabled && (sent < FlushLimit); ++sent)
    {
        if ((m_count == 0U) && (m_lost != 0U))
        {
            Push({.Kind = Input::UsbDiagnosticKind::Suppressed,
                  .Port = 0U,
                  .Detail = 2U,
                  .Code = 0U,
                  .Value0 = m_lost,
                  .Value1 = 0U,
                  .Data = 0U});
            m_lost = 0U;
        }
        if (m_count == 0U)
        {
            return;
        }
        const Foundation::Size bytes = WriteXhciProgress(frame, m_transaction + 1U, m_controller, m_ring[m_head]);
        const Abi::SystemCallError error = client.Send(root, System::AddressOf(frame), bytes).Error;
        if ((error == Abi::SystemCallError::WouldBlock) || (error == Abi::SystemCallError::ResourceExhausted))
        {
            return;
        }
        if (error != Abi::SystemCallError::None)
        {
            m_enabled = false;
            m_count = 0U;
            return;
        }
        ++m_transaction;
        m_head = (m_head + 1U) % Capacity;
        --m_count;
    }
}
} // namespace Asteria::User::Component
