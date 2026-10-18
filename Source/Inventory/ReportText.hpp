// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_INVENTORY_REPORT_TEXT_HPP_INCLUDED
#define ASTERIA_INVENTORY_REPORT_TEXT_HPP_INCLUDED

#include "Abi/DeviceInventory.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Inventory
{
/// Borrowed ASCII buffer. No allocation; excess bytes are discarded and recorded.
/// Bytes excludes a terminator: the output is suitable for counted file writes.
class ReportText final
{
public:
    ReportText(char* buffer, Foundation::Size capacity) noexcept;
    void Text(const char* text) noexcept;
    void Character(char value) noexcept;
    void Hex(Foundation::UInt64 value, Foundation::Size digits) noexcept;
    void Decimal(Foundation::UInt64 value) noexcept;
    [[nodiscard]] Foundation::Size Bytes() const noexcept;
    [[nodiscard]] bool Truncated() const noexcept;

private:
    char* m_buffer;
    Foundation::Size m_capacity;
    Foundation::Size m_bytes{0U};
    bool m_truncated{false};
};

using PciFunction = Abi::InventoryPciFunction;
using CapturedDriver = Abi::InventoryDriver;

/// A driver is reported only when boot resource capture selected this function.
[[nodiscard]] const char* Support(const PciFunction& function, CapturedDriver driver) noexcept;
[[nodiscard]] const char* ClassName(const PciFunction& function) noexcept;
void FormatInventory(ReportText& text, const Abi::DeviceInventory& inventory) noexcept;
void FormatPci(ReportText& text, const PciFunction& function, CapturedDriver driver) noexcept;
} // Namespace Asteria::Inventory

#endif // ASTERIA_INVENTORY_REPORT_TEXT_HPP_INCLUDED
