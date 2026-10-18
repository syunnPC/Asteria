// SPDX-License-Identifier: Apache-2.0

#include "Inventory/EhciRmrrReport.hpp"

namespace Asteria::Inventory
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

constexpr UInt32 AllOnes{0xFFFF'FFFFU};
constexpr Size MaximumCapabilities{48U};
constexpr UInt64 PageBytes{4096U};
// Intel 6 Series Chipset and C200 Series Chipset specification update (July
// 2011), PCH device and revision ID table: USB EHCI #1 (D29:F0) and #2
// (D26:F0), and the LPC bridge (D31:F0) of each 6-series and C200 part.
constexpr UInt16 IntelVendor{0x8086U};
constexpr UInt16 SixSeriesEhci[]{0x1C26U, 0x1C2DU};
constexpr UInt16 SixSeriesLpc[]{0x1C44U, 0x1C46U, 0x1C47U, 0x1C49U, 0x1C4AU, 0x1C4BU, 0x1C4CU, 0x1C4DU,
                                0x1C4EU, 0x1C4FU, 0x1C50U, 0x1C52U, 0x1C54U, 0x1C56U, 0x1C5CU};
// Datasheet 324645-006, section 16.1.29: SPECIAL_SMI's enables (bits 13:0)
// and its write-one-to-clear status (bits 24:16).
constexpr UInt16 SpecialSmiOffset{0x70U};
constexpr UInt32 SpecialSmiEnables{0x0000'3FFFU};
// EHCI 1.0 section 2.1.8 (and datasheet section 16.1.28): USBLEGCTLSTS's SMI
// enables and status bits.
constexpr UInt32 LegacySmiEnables{0x0000'E03FU};
constexpr UInt32 LegacySmiStatus{0xE03F'0000U};
// Datasheet sections 13.1.12, 13.1.13, 13.8.3.4 and 13.8.3.7.
constexpr UInt16 PmBaseOffset{0x40U};
constexpr UInt16 AcpiControlOffset{0x44U};
constexpr UInt32 PmBaseMask{0xFF80U};
constexpr UInt32 AcpiEnable{1U << 7U};
constexpr UInt16 SmiEnableOffset{0x30U};
constexpr UInt16 PmTimerOffset{0x08U};

template <Size Count> [[nodiscard]] bool Listed(const UInt16 (&values)[Count], const UInt16 value) noexcept
{
    for (const UInt16 candidate : values)
    {
        if (candidate == value)
        {
            return true;
        }
    }
    return false;
}

void Bit(ReportText& text, const char* const name, const UInt32 value, const UInt32 bit) noexcept
{
    text.Character(' ');
    text.Text(name);
    text.Character('=');
    text.Character(((value >> bit) & 1U) != 0U ? '1' : '0');
}

void HexField(ReportText& text, const char* const name, const UInt64 value, const Size digits) noexcept
{
    text.Character(' ');
    text.Text(name);
    text.Text("=0x");
    text.Hex(value, digits);
}

// The PCI power management capability's PMCSR (PCI PM 1.2 section 3.2.4),
// found in the standard list; false without one.
[[nodiscard]] bool FindPowerState(IEhciEvidenceAccess& access, const UInt32 command, UInt32& control) noexcept
{
    if ((command & (UInt32{1U} << 20U)) == 0U)
    {
        return false;
    }
    UInt32 next = access.ReadConfiguration(0x34U) & 0xFCU;
    for (Size count{0U}; (next >= 0x40U) && (count < MaximumCapabilities); ++count)
    {
        const UInt32 header = access.ReadConfiguration(static_cast<UInt16>(next));
        if (header == AllOnes)
        {
            return false;
        }
        if ((header & 0xFFU) == 1U)
        {
            control = access.ReadConfiguration(static_cast<UInt16>(next + 4U)) & 0xFFFFU;
            return true;
        }
        next = (header >> 8U) & 0xFCU;
    }
    return false;
}

// The EECP list from HCCPARAMS (EHCI 1.0 section 2.2.4): the USB Legacy
// Support capability (ID 1) and its control/status dword.
void FormatLegacySupport(ReportText& text, IEhciEvidenceAccess& access, const UInt32 parameters) noexcept
{
    UInt32 next = (parameters >> 8U) & 0xFFU;
    for (Size count{0U}; (next != 0U) && (count < MaximumCapabilities); ++count)
    {
        if ((next < 0x40U) || ((next & 3U) != 0U) || (next > 0xF8U))
        {
            text.Text("  EECP malformed at 0x");
            text.Hex(next, 2U);
            text.Text("\r\n");
            return;
        }
        const UInt32 header = access.ReadConfiguration(static_cast<UInt16>(next));
        if (header == AllOnes)
        {
            break;
        }
        if ((header & 0xFFU) == 1U)
        {
            const UInt32 control = access.ReadConfiguration(static_cast<UInt16>(next + 4U));
            text.Text("  USBLEGSUP@0x");
            text.Hex(next, 2U);
            text.Text("=0x");
            text.Hex(header, 8U);
            Bit(text, "BIOS-owned", header, 16U);
            Bit(text, "OS-owned", header, 24U);
            HexField(text, "USBLEGCTLSTS", control, 8U);
            HexField(text, "SMI-enables", control & LegacySmiEnables, 4U);
            HexField(text, "SMI-status", control & LegacySmiStatus, 8U);
            text.Text("\r\n");
            return;
        }
        next = (header >> 8U) & 0xFFU;
    }
    text.Text("  USBLEGSUP not found in the EECP list\r\n");
}

void FormatRegisters(ReportText& text, IEhciEvidenceAccess& access, const UInt64 barBytes) noexcept
{
    const UInt32 capability = access.ReadRegister(0U);
    const UInt32 structural = access.ReadRegister(4U);
    const UInt32 parameters = access.ReadRegister(8U);
    const UInt32 length = capability & 0xFFU;
    text.Text("  CAPLENGTH=0x");
    text.Hex(length, 2U);
    HexField(text, "HCIVERSION", capability >> 16U, 4U);
    HexField(text, "HCSPARAMS", structural, 8U);
    text.Text(" N_PORTS=");
    text.Decimal(structural & 15U);
    text.Text(" N_PCC=");
    text.Decimal((structural >> 8U) & 15U);
    text.Text(" N_CC=");
    text.Decimal((structural >> 12U) & 15U);
    HexField(text, "HCCPARAMS", parameters, 8U);
    HexField(text, "EECP", (parameters >> 8U) & 0xFFU, 2U);
    text.Text("\r\n");
    FormatLegacySupport(text, access, parameters);
    // USBCMD, USBSTS and CONFIGFLAG (offsets 00h, 04h and 40h of the
    // operational block, EHCI 1.0 section 2.3) must lie inside the extent.
    if ((length < 0x10U) || ((length & 3U) != 0U) || (barBytes < UInt64{length} + 0x44U))
    {
        text.Text("  operational registers outside the BAR extent\r\n");
        return;
    }
    const UInt32 command = access.ReadRegister(length);
    const UInt32 status = access.ReadRegister(length + 4U);
    const UInt32 configured = access.ReadRegister(length + 0x40U);
    text.Text("  USBCMD=0x");
    text.Hex(command, 8U);
    Bit(text, "RS", command, 0U);
    Bit(text, "PSE", command, 4U);
    Bit(text, "ASE", command, 5U);
    HexField(text, "USBSTS", status, 8U);
    Bit(text, "HCHalted", status, 12U);
    Bit(text, "PSS", status, 14U);
    Bit(text, "ASS", status, 15U);
    HexField(text, "CONFIGFLAG", configured, 8U);
    text.Text("\r\n");
}

[[nodiscard]] const char* MemoryTypeName(const UInt32 type) noexcept
{
    constexpr const char* Names[]{"Reserved",
                                  "LoaderCode",
                                  "LoaderData",
                                  "BootServicesCode",
                                  "BootServicesData",
                                  "RuntimeServicesCode",
                                  "RuntimeServicesData",
                                  "Conventional",
                                  "Unusable",
                                  "AcpiReclaim",
                                  "AcpiNvs",
                                  "Mmio",
                                  "MmioPortSpace",
                                  "PalCode",
                                  "Persistent",
                                  "Unaccepted"};
    return (type < (sizeof(Names) / sizeof(Names[0U]))) ? Names[type] : "Other";
}

// The descriptor's last byte, or false when its span is empty or wraps.
[[nodiscard]] bool LastByte(const EhciEvidenceMemory& memory, UInt64& last) noexcept
{
    if ((memory.Pages == 0U) || (memory.Pages > ((~UInt64{0U} - memory.Start) / PageBytes) + 1U) ||
        ((memory.Start & (PageBytes - 1U)) != 0U))
    {
        return false;
    }
    last = memory.Start + ((memory.Pages * PageBytes) - 1U);
    return last >= memory.Start;
}

void Span(ReportText& text, const UInt64 first, const UInt64 last) noexcept
{
    text.Text(" 0x");
    text.Hex(first, 16U);
    text.Text("-0x");
    text.Hex(last, 16U);
}
} // namespace

void FormatEhciEvidence(ReportText& text, IEhciEvidenceAccess& access, const EhciEvidenceFunction& function) noexcept
{
    text.Text("EHCI-RMRR ");
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
    const UInt32 command = access.ReadConfiguration(0x04U);
    if (command == AllOnes)
    {
        text.Text(" absent (configuration reads all-ones)\r\n");
        return;
    }
    UInt32 control{0U};
    if (FindPowerState(access, command, control))
    {
        HexField(text, "PMCSR", control, 4U);
        text.Text(" D");
        text.Decimal(control & 3U);
    }
    else
    {
        text.Text(" PMCSR=none");
    }
    HexField(text, "command", command & 0xFFFFU, 4U);
    Bit(text, "MSE", command, 1U);
    Bit(text, "BME", command, 2U);
    const UInt32 bar = access.ReadConfiguration(0x10U);
    HexField(text, "BAR0", bar, 8U);
    HexField(text, "bytes", function.BarBytes, 4U);
    text.Text("\r\n");
    if ((command & 2U) == 0U)
    {
        text.Text("  registers not read: memory decode off\r\n");
    }
    else if (function.BarBytes < 0x0CU)
    {
        text.Text("  registers not read: no firmware extent of BAR 0\r\n");
    }
    else
    {
        FormatRegisters(text, access, function.BarBytes);
    }
    if ((function.Vendor == IntelVendor) && Listed(SixSeriesEhci, function.Product))
    {
        const UInt32 smi = access.ReadConfiguration(SpecialSmiOffset);
        text.Text("  SPECIAL_SMI@0x70=0x");
        text.Hex(smi, 8U);
        HexField(text, "enables", smi & SpecialSmiEnables, 4U);
        HexField(text, "status", (smi >> 16U) & 0x1FFU, 3U);
        text.Text("\r\n");
    }
    else
    {
        text.Text("  SPECIAL_SMI unknown layout for ");
        text.Hex(function.Vendor, 4U);
        text.Character(':');
        text.Hex(function.Product, 4U);
        text.Text("\r\n");
    }
}

void FormatRmrrMemory(ReportText& text, const Size region, const UInt64 base, const UInt64 limit,
                      const EhciEvidenceMemory* const memory, const Size count) noexcept
{
    text.Text("  RMRR#");
    text.Decimal(region);
    Span(text, base, limit);
    text.Text(" UEFI map:");
    UInt64 cursor{base};
    for (Size piece{0U}; piece < 8U; ++piece)
    {
        UInt64 nextStart{~UInt64{0U}};
        bool covered{false};
        for (Size index{0U}; (memory != nullptr) && (index < count); ++index)
        {
            UInt64 last{0U};
            if (!LastByte(memory[index], last))
            {
                continue;
            }
            if ((memory[index].Start <= cursor) && (cursor <= last))
            {
                const UInt64 end = (last < limit) ? last : limit;
                text.Text(" type=");
                text.Decimal(memory[index].Type);
                text.Character('(');
                text.Text(MemoryTypeName(memory[index].Type));
                text.Character(')');
                Span(text, cursor, end);
                if (end == limit)
                {
                    text.Text("\r\n");
                    return;
                }
                cursor = end + 1U;
                covered = true;
                break;
            }
            if ((memory[index].Start > cursor) && (memory[index].Start < nextStart))
            {
                nextStart = memory[index].Start;
            }
        }
        if (covered)
        {
            continue;
        }
        const UInt64 end = (nextStart <= limit) ? (nextStart - 1U) : limit;
        text.Text(" gap");
        Span(text, cursor, end);
        if (end == limit)
        {
            text.Text("\r\n");
            return;
        }
        cursor = end + 1U;
    }
    text.Text(" ...\r\n");
}

void FormatPchSmi(ReportText& text, IPchSmiAccess& access, const UInt64 pmTimerPort) noexcept
{
    const UInt32 identity = access.ReadLpcConfiguration(0x00U);
    const UInt16 vendor = static_cast<UInt16>(identity & 0xFFFFU);
    const UInt16 product = static_cast<UInt16>(identity >> 16U);
    text.Text("PCH SMI_EN");
    if (identity == AllOnes)
    {
        text.Text(" unknown layout: no LPC bridge at 00:1f.0\r\n");
        return;
    }
    text.Text(" LPC ");
    text.Hex(vendor, 4U);
    text.Character(':');
    text.Hex(product, 4U);
    if ((vendor != IntelVendor) || !Listed(SixSeriesLpc, product))
    {
        text.Text(" unknown layout\r\n");
        return;
    }
    const UInt32 base = access.ReadLpcConfiguration(PmBaseOffset) & PmBaseMask;
    const bool decoded = (access.ReadLpcConfiguration(AcpiControlOffset) & AcpiEnable) != 0U;
    HexField(text, "PMBASE", base, 4U);
    text.Text(decoded ? " ACPI_EN=1" : " ACPI_EN=0");
    if (pmTimerPort == 0U)
    {
        text.Text(" FADT-PM-timer=none");
    }
    else
    {
        HexField(text, "FADT-PM-timer", pmTimerPort, 4U);
        text.Text((pmTimerPort == (UInt64{base} + PmTimerOffset)) ? " (PMBASE+08h)" : " (differs from PMBASE+08h)");
    }
    if (!decoded || (base == 0U))
    {
        text.Text(" SMI_EN not read: ACPI I/O decode off\r\n");
        return;
    }
    const UInt32 enables = access.ReadPort32(static_cast<UInt16>(base + SmiEnableOffset));
    HexField(text, "SMI_EN", enables, 8U);
    Bit(text, "GBL_SMI_EN", enables, 0U);
    Bit(text, "LEGACY_USB_EN", enables, 3U);
    Bit(text, "LEGACY_USB2_EN", enables, 17U);
    Bit(text, "INTEL_USB2_EN", enables, 18U);
    text.Text("\r\n");
}
} // namespace Asteria::Inventory
