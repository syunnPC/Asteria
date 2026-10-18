// SPDX-License-Identifier: Apache-2.0

#include "Inventory/XhciReport.hpp"

namespace Asteria::Inventory
{
namespace
{
using Foundation::UInt32;
using Foundation::UInt64;
[[nodiscard]] bool Range(const UInt32 offset, const UInt32 bytes, const UInt64 barBytes) noexcept
{
    return (offset <= barBytes) && (bytes <= barBytes - offset);
}
void Word(ReportText& text, const char* name, const UInt32 value, const Foundation::Size digits = 8U) noexcept
{
    text.Character(' ');
    text.Text(name);
    text.Text("=0x");
    text.Hex(value, digits);
}
}
void FormatXhci(ReportText& text, IXhciReportRegisters& registers, const UInt64 barBytes) noexcept
{
    if (!Range(0U, 32U, barBytes))
    {
        text.Text("  XHCI skipped: BAR shorter than capability registers\r\n");
        return;
    }
    const UInt32 cap = registers.Read32(0U);
    const UInt32 hcs1 = registers.Read32(4U);
    const UInt32 hcc1 = registers.Read32(16U);
    text.Text("  XHCI");
    Word(text, "CAPLENGTH", cap & 255U, 2U);
    Word(text, "HCIVERSION", cap >> 16U, 4U);
    Word(text, "HCSPARAMS1", hcs1);
    Word(text, "HCSPARAMS2", registers.Read32(8U));
    Word(text, "HCSPARAMS3", registers.Read32(12U));
    text.Text("\r\n  XHCI");
    Word(text, "HCCPARAMS1", hcc1);
    // HCCPARAMS2 is reserved before xHCI 1.1, retained as a raw read.
    Word(text, "HCCPARAMS2", registers.Read32(28U));
    Word(text, "DBOFF", registers.Read32(20U));
    Word(text, "RTSOFF", registers.Read32(24U));
    text.Text("\r\n");
    if ((cap == 0xFFFF'FFFFU) || (hcs1 == 0xFFFF'FFFFU) || (hcc1 == 0xFFFF'FFFFU))
    {
        text.Text("  XHCI unavailable: all-ones capability read\r\n");
        return;
    }
    const UInt32 operational = cap & 255U;
    if ((operational < 32U) || ((operational & 3U) != 0U) || !Range(operational, 12U, barBytes))
    {
        text.Text("  XHCI unavailable: invalid CAPLENGTH\r\n");
        return;
    }
    text.Text("  XHCI");
    Word(text, "PAGESIZE", registers.Read32(operational + 8U));
    Word(text, "USBSTS", registers.Read32(operational + 4U));
    text.Text("\r\n");
    bool legacy{false};
    UInt32 offset = (hcc1 >> 16U) * 4U;
    for (Foundation::Size count{0U}; (offset != 0U) && (count < 64U); ++count)
    {
        if ((offset < 32U) || !Range(offset, 4U, barBytes))
        {
            text.Text("  XHCI capabilities truncated: outside BAR\r\n");
            break;
        }
        const UInt32 header = registers.Read32(offset);
        if (header == 0xFFFF'FFFFU)
        {
            text.Text("  XHCI capabilities unavailable: all-ones read\r\n");
            break;
        }
        const UInt32 id = header & 255U;
        if ((id == 1U) && Range(offset, 8U, barBytes))
        {
            legacy = true;
            text.Text("  XHCI legacy");
            Word(text, "offset", offset);
            Word(text, "USBLEGSUP", header);
            Word(text, "USBLEGCTLSTS", registers.Read32(offset + 4U));
            text.Text(" BIOS=");
            text.Decimal((header >> 16U) & 1U);
            text.Text(" OS=");
            text.Decimal((header >> 24U) & 1U);
            text.Text("\r\n");
        }
        else if ((id == 2U) && Range(offset, 16U, barBytes))
        {
            const UInt32 ports = registers.Read32(offset + 8U);
            text.Text("  XHCI protocol");
            Word(text, "offset", offset);
            text.Text(" revision=");
            text.Decimal(header >> 24U);
            text.Character('.');
            text.Hex((header >> 16U) & 255U, 2U);
            text.Text(" port-offset=");
            text.Decimal(ports & 255U);
            text.Text(" port-count=");
            text.Decimal((ports >> 8U) & 255U);
            Word(text, "name", registers.Read32(offset + 4U));
            Word(text, "ports", ports);
            Word(text, "slot-type", registers.Read32(offset + 12U));
            text.Text("\r\n");
        }
        const UInt32 next = ((header >> 8U) & 255U) * 4U;
        if (next == 0U)
        {
            offset = 0U;
            break;
        }
        if (!Range(offset, next + 4U, barBytes))
        {
            text.Text("  XHCI capabilities truncated: invalid next\r\n");
            break;
        }
        offset += next;
        if (count == 63U)
        {
            text.Text("  XHCI capabilities truncated: 64 entries\r\n");
        }
    }
    if (!legacy)
    {
        text.Text(offset == 0U ? "  XHCI legacy absent\r\n" : "  XHCI legacy unavailable\r\n");
    }
    const UInt32 ports = hcs1 >> 24U;
    UInt32 connected{0U};
    for (UInt32 number{1U}; number <= ports; ++number)
    {
        const UInt32 portOffset = operational + 0x400U + (number - 1U) * 16U;
        if (!Range(portOffset, 4U, barBytes))
        {
            text.Text("  XHCI ports truncated: outside BAR\r\n");
            break;
        }
        const UInt32 status = registers.Read32(portOffset);
        if ((status == 0xFFFF'FFFFU) || ((status & 1U) == 0U))
        {
            continue;
        }
        ++connected;
        if (connected <= 32U)
        {
            text.Text("  XHCI port=");
            text.Decimal(number);
            Word(text, "PORTSC", status);
            text.Text("\r\n");
        }
    }
    if (connected > 32U)
    {
        text.Text("  XHCI connected ports truncated: ");
        text.Decimal(connected);
        text.Text(" connected, first 32 shown\r\n");
    }
}
} // namespace Asteria::Inventory
