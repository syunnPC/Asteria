// SPDX-License-Identifier: Apache-2.0

#include "Inventory/ReportText.hpp"

namespace Asteria::Inventory
{
ReportText::ReportText(char* const buffer, const Foundation::Size capacity) noexcept
    : m_buffer{buffer}, m_capacity{buffer == nullptr ? 0U : capacity}
{
}

void ReportText::Character(const char value) noexcept
{
    if (m_bytes == m_capacity)
    {
        m_truncated = true;
        return;
    }
    m_buffer[m_bytes++] = (value >= 32 && value <= 126) || value == '\r' || value == '\n' ? value : '?';
}

void ReportText::Text(const char* text) noexcept
{
    if (text != nullptr)
    {
        while (*text != '\0')
        {
            Character(*text++);
        }
    }
}

void ReportText::Hex(const Foundation::UInt64 value, const Foundation::Size digits) noexcept
{
    constexpr char Alphabet[]{"0123456789abcdef"};
    if (digits > 16U)
    {
        m_truncated = true;
        return;
    }
    for (Foundation::Size index{digits}; index != 0U; --index)
    {
        Character(Alphabet[(value >> ((index - 1U) * 4U)) & 15U]);
    }
}

void ReportText::Decimal(Foundation::UInt64 value) noexcept
{
    char digits[20U]{};
    Foundation::Size count{0U};
    do
    {
        digits[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U)
    {
        Character(digits[--count]);
    }
}

Foundation::Size ReportText::Bytes() const noexcept
{
    return m_bytes;
}

bool ReportText::Truncated() const noexcept
{
    return m_truncated;
}

const char* Support(const PciFunction& function, const CapturedDriver driver) noexcept
{
    switch (driver)
    {
    case CapturedDriver::Ahci:
        return "ahci driver";
    case CapturedDriver::E1000e:
        return "e1000e NIC driver";
    case CapturedDriver::Edu:
        return "edu demo driver";
    case CapturedDriver::Display:
        return "display broker";
    case CapturedDriver::EduPending:
        return "edu demo driver (ECAM capture pending)";
    case CapturedDriver::Xhci:
        return "xhci driver";
    case CapturedDriver::UsbHost:
        return "usb host driver";
    case CapturedDriver::E1000ePending:
        return "e1000e NIC driver (ECAM capture pending)";
    case CapturedDriver::None:
        break;
    }
    if (function.Class == 6U)
    {
        return "bridge";
    }
    if (function.Class == 12U && function.Subclass == 3U &&
        (function.Interface == 0U || function.Interface == 0x10U || function.Interface == 0x20U ||
         function.Interface == 0x30U))
    {
        return "USB host controller, no driver published";
    }
    return "unsupported";
}

const char* ClassName(const PciFunction& function) noexcept
{
    switch (function.Class)
    {
    case 1U:
        return function.Subclass == 6U && function.Interface == 1U ? "SATA AHCI" : "storage";
    case 2U:
        return "network";
    case 3U:
        return "display";
    case 4U:
        return "multimedia";
    case 6U:
        return "bridge";
    case 8U:
        return "system";
    case 9U:
        return "input";
    case 12U:
        return function.Subclass == 3U ? "USB" : "serial bus";
    default:
        return "other";
    }
}

void FormatInventory(ReportText& text, const Abi::DeviceInventory& inventory) noexcept
{
    if (inventory.Version != Abi::DeviceInventoryVersion || inventory.PciCount > Abi::MaximumInventoryPciFunctions ||
        inventory.DiskCount > Abi::MaximumInventoryDisks)
    {
        text.Text("# devices: invalid inventory record\n");
        return;
    }
    for (Foundation::Size index{0U}; index < inventory.PciCount; ++index)
    {
        const auto& entry = inventory.Pci[index];
        const auto& function = entry.Function;
        text.Text("# devices: ");
        text.Hex(function.Segment, 4U);
        text.Character(':');
        text.Hex(function.Bus, 2U);
        text.Character(':');
        text.Hex(function.Device, 2U);
        text.Character('.');
        text.Hex(function.Function, 1U);
        text.Character(' ');
        text.Hex(function.Vendor, 4U);
        text.Character(':');
        text.Hex(function.Product, 4U);
        text.Text(" class=");
        text.Hex(function.Class, 2U);
        text.Character('/');
        text.Hex(function.Subclass, 2U);
        text.Character('/');
        text.Hex(function.Interface, 2U);
        text.Character(' ');
        text.Text(ClassName(function));
        text.Text(" -> ");
        text.Text(Support(function, entry.Driver));
        // The kernel lets this function DMA without isolation under a BOOT.CFG trust entry.
        if (entry.Dma == Abi::InventoryDma::Trusted)
        {
            text.Text(" (TRUSTED: no DMA isolation)");
        }
        if ((function.Class == 12U) && (function.Subclass == 3U) &&
            ((function.Interface == 0U) || (function.Interface == 0x10U) || (function.Interface == 0x20U) ||
             (function.Interface == 0x30U)))
        {
            text.Text(entry.XhciIndex != 0U ? " publication=published" : " publication=skipped");
            text.Text(" step=");
            text.Decimal(entry.PublicationStep);
            text.Text(" err=0x");
            text.Hex(entry.PublicationError, 2U);
        }
        text.Character('\n');
    }
    for (Foundation::Size index{0U}; index < inventory.DiskCount; ++index)
    {
        const auto& disk = inventory.Disks[index];
        text.Text("# devices: disk AHCI port=");
        text.Decimal(disk.Port);
        text.Text(" sectors=");
        text.Decimal(disk.Sectors);
        text.Text(" model=\"");
        for (char character : disk.Model)
        {
            text.Character(character);
        }
        text.Text("\"\n");
    }
    text.Text("# devices: input 8042=");
    const char* input{"unknown"};
    switch (inventory.I8042)
    {
    case 1U:
        input = "absent";
        break;
    case 2U:
        input = "present FADT-8042";
        break;
    case 3U:
        input = "present AML";
        break;
    default:
        break;
    }
    text.Text(input);
    text.Character('\n');
    if (inventory.Truncated != 0U)
    {
        text.Text("# devices: inventory truncated\n");
    }
}

void FormatPci(ReportText& text, const PciFunction& function, const CapturedDriver driver) noexcept
{
    text.Text("PCI ");
    text.Hex(function.Segment, 4U);
    text.Character(':');
    text.Hex(function.Bus, 2U);
    text.Character(':');
    text.Hex(function.Device, 2U);
    text.Character('.');
    text.Hex(function.Function, 1U);
    text.Character(' ');
    text.Hex(function.Vendor, 4U);
    text.Character(':');
    text.Hex(function.Product, 4U);
    text.Text(" class=");
    text.Hex(function.Class, 2U);
    text.Character('/');
    text.Hex(function.Subclass, 2U);
    text.Character('/');
    text.Hex(function.Interface, 2U);
    text.Character(' ');
    text.Text(ClassName(function));
    text.Text(" header=");
    text.Hex(function.HeaderType, 2U);
    text.Text(" irq-pin/line=");
    text.Hex(function.InterruptPin, 2U);
    text.Character('/');
    text.Hex(function.InterruptLine, 2U);
    for (Foundation::Size index{0U}; index < 6U; ++index)
    {
        if (function.Bars[index] != 0U)
        {
            text.Text(" BAR");
            text.Decimal(index);
            text.Character('=');
            text.Hex(function.Bars[index], 8U);
        }
    }
    text.Text(" support=");
    text.Text(Support(function, driver));
    text.Text("\r\n");
}
} // Namespace Asteria::Inventory
