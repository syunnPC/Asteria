// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_INVENTORY_XHCI_REPORT_HPP_INCLUDED
#define ASTERIA_INVENTORY_XHCI_REPORT_HPP_INCLUDED

#include "Inventory/ReportText.hpp"

namespace Asteria::Inventory
{
/// Read-only BAR-relative adapter. The caller proves the physical BAR is
/// decoded and identity mapped; the formatter bounds every access by barBytes.
class IXhciReportRegisters
{
public:
    [[nodiscard]] virtual Foundation::UInt32 Read32(Foundation::UInt32 offset) noexcept = 0;

protected:
    ~IXhciReportRegisters() = default;
};

/// Intel xHCI 1.2 sections 5 and 7. Boot-only snapshot, with no register writes.
/// At most 64 extended capabilities, 255 port reads and 32 connected-port lines.
/// All-ones reads end a capability walk without following its next pointer.
void FormatXhci(ReportText& text, IXhciReportRegisters& registers, Foundation::UInt64 barBytes) noexcept;
} // namespace Asteria::Inventory

#endif // ASTERIA_INVENTORY_XHCI_REPORT_HPP_INCLUDED
