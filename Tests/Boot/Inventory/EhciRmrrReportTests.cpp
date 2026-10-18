// SPDX-License-Identifier: Apache-2.0
#include "Inventory/EhciRmrrReport.hpp"

#include <cstring>

#include "Support/TestContext.hpp"

namespace
{
using Asteria::Foundation::UInt16;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Inventory::EhciEvidenceFunction;
using Asteria::Inventory::EhciEvidenceMemory;
using Asteria::Inventory::ReportText;

// Synthetic PCI PM and EHCI capability chains use distinct offsets. The
// Intel 6-series identity also exercises its documented SPECIAL_SMI register.
// Every register access is checked against a 1 KiB BAR extent.
class Ehci final : public Asteria::Inventory::IEhciEvidenceAccess
{
public:
    Ehci() noexcept
    {
        Configuration[0x00U / 4U] = 0x1C2D'8086U;
        Configuration[0x04U / 4U] = 0x0290'0006U;
        Configuration[0x10U / 4U] = 0xE000'0000U;
        Configuration[0x34U / 4U] = 0x40U;
        Configuration[0x40U / 4U] = 0x0000'4801U;
        Configuration[0x44U / 4U] = 0x0000'0000U;
        Configuration[0x48U / 4U] = 0x0000'000AU;
        Configuration[0x80U / 4U] = 0x0101'0001U;
        Configuration[0x84U / 4U] = 0x2000'E03FU;
        Configuration[0x70U / 4U] = 0x0102'0FFFU;
        Registers[0x00U / 4U] = 0x0100'0020U;
        Registers[0x04U / 4U] = 0x0020'0006U;
        Registers[0x08U / 4U] = 0x0000'8001U;
        Registers[0x20U / 4U] = 0x0001'0031U;
        Registers[0x24U / 4U] = 0x0000'C008U;
        Registers[0x60U / 4U] = 1U;
    }
    [[nodiscard]] UInt32 ReadConfiguration(const UInt16 offset) noexcept override
    {
        Good = Good && (offset < 0x100U) && ((offset & 3U) == 0U);
        return (offset < 0x100U) ? Configuration[offset / 4U] : 0xFFFF'FFFFU;
    }
    [[nodiscard]] UInt32 ReadRegister(const UInt32 offset) noexcept override
    {
        ++RegisterReads;
        Good = Good && (offset + 4U <= Extent) && ((offset & 3U) == 0U);
        return (offset < sizeof(Registers)) ? Registers[offset / 4U] : 0xFFFF'FFFFU;
    }
    UInt32 Configuration[64U]{};
    UInt32 Registers[0x400U / 4U]{};
    UInt64 Extent{0x400U};
    UInt32 RegisterReads{0U};
    bool Good{true};
};

class Lpc final : public Asteria::Inventory::IPchSmiAccess
{
public:
    [[nodiscard]] UInt32 ReadLpcConfiguration(const UInt16 offset) noexcept override
    {
        switch (offset)
        {
        case 0x00U:
            return Identity;
        case 0x40U:
            return 0x0000'1001U;
        case 0x44U:
            return Control;
        default:
            return 0xFFFF'FFFFU;
        }
    }
    [[nodiscard]] UInt32 ReadPort32(const UInt16 port) noexcept override
    {
        LastPort = port;
        ++Ports;
        return 0x0006'200BU;
    }
    UInt32 Identity{0x1C4F'8086U};
    UInt32 Control{0x80U};
    UInt16 LastPort{0U};
    UInt32 Ports{0U};
};

[[nodiscard]] bool Contains(const char* const text, const char* const expected) noexcept
{
    return std::strstr(text, expected) != nullptr;
}

constexpr EhciEvidenceFunction SixSeriesEhci{0U, 2U, 3U, 1U, {}, 0x8086U, 0x1C2DU, 0U, 0x400U};
} // namespace

int main()
{
    Asteria::TestSupport::TestContext context{"EhciRmrrReportTests"};
    Ehci ehci{};
    char text[2048U]{};
    ReportText report{text, sizeof(text) - 1U};
    Asteria::Inventory::FormatEhciEvidence(report, ehci, SixSeriesEhci);
    ASTERIA_EXPECT(context, "PCI PM, EHCI legacy ownership and operational registers are reported", ehci.Good,
                   !report.Truncated(),
                   Contains(text, "EHCI-RMRR 0000:02:03.1 8086:1c2d PMCSR=0x0000 D0 command=0x0006 MSE=1 BME=1 "
                                  "BAR0=0xe0000000 bytes=0x0400\r\n"),
                   Contains(text, "  CAPLENGTH=0x20 HCIVERSION=0x0100 HCSPARAMS=0x00200006 N_PORTS=6 N_PCC=0 N_CC=0 "
                                  "HCCPARAMS=0x00008001 EECP=0x80\r\n"),
                   Contains(text, "  USBLEGSUP@0x80=0x01010001 BIOS-owned=1 OS-owned=1 USBLEGCTLSTS=0x2000e03f "
                                  "SMI-enables=0xe03f SMI-status=0x20000000\r\n"),
                   Contains(text, "  USBCMD=0x00010031 RS=1 PSE=1 ASE=1 USBSTS=0x0000c008 HCHalted=0 PSS=1 ASS=1 "
                                  "CONFIGFLAG=0x00000001\r\n"),
                   Contains(text, "  SPECIAL_SMI@0x70=0x01020fff enables=0x0fff status=0x102\r\n"));

    ehci.Configuration[0x40U / 4U] = 0x0000'4801U;
    ehci.Configuration[0x44U / 4U] = 0x0000'0103U;
    char d3[2048U]{};
    ReportText sleeping{d3, sizeof(d3) - 1U};
    Asteria::Inventory::FormatEhciEvidence(sleeping, ehci, SixSeriesEhci);
    ASTERIA_EXPECT(context, "the power state comes from PMCSR's low bits", Contains(d3, "PMCSR=0x0103 D3 "));

    ehci.Configuration[0x04U / 4U] = 0x0290'0004U;
    ehci.RegisterReads = 0U;
    char off[2048U]{};
    ReportText undecoded{off, sizeof(off) - 1U};
    Asteria::Inventory::FormatEhciEvidence(undecoded, ehci, SixSeriesEhci);
    ASTERIA_EXPECT(context, "a function without memory decode is never read through its BAR", ehci.RegisterReads == 0U,
                   Contains(off, " MSE=0 BME=1 "), Contains(off, "  registers not read: memory decode off\r\n"),
                   Contains(off, "  SPECIAL_SMI@0x70="));

    ehci.Configuration[0x04U / 4U] = 0x0280'0006U;
    EhciEvidenceFunction foreign{SixSeriesEhci};
    foreign.Product = 0x293AU;
    foreign.BarBytes = 0U;
    char qemu[2048U]{};
    ReportText unknown{qemu, sizeof(qemu) - 1U};
    Asteria::Inventory::FormatEhciEvidence(unknown, ehci, foreign);
    ASTERIA_EXPECT(context, "another EHCI's second SMI register is an unknown layout, and no extent means no MMIO",
                   ehci.RegisterReads == 0U, Contains(qemu, " PMCSR=none "),
                   Contains(qemu, "  registers not read: no firmware extent of BAR 0\r\n"),
                   Contains(qemu, "  SPECIAL_SMI unknown layout for 8086:293a\r\n"), !Contains(qemu, "SPECIAL_SMI@"));

    ehci.Extent = 0x40U;
    EhciEvidenceFunction small{SixSeriesEhci};
    small.BarBytes = 0x40U;
    char clipped[2048U]{};
    ReportText outside{clipped, sizeof(clipped) - 1U};
    Asteria::Inventory::FormatEhciEvidence(outside, ehci, small);
    ASTERIA_EXPECT(context, "operational registers beyond the extent are not read", ehci.Good,
                   Contains(clipped, "  operational registers outside the BAR extent\r\n"),
                   !Contains(clipped, "USBCMD"));

    ehci.Extent = 0x400U;
    ehci.Registers[0x08U / 4U] = 0x0000'3071U;
    char malformed[2048U]{};
    ReportText broken{malformed, sizeof(malformed) - 1U};
    Asteria::Inventory::FormatEhciEvidence(broken, ehci, SixSeriesEhci);
    ehci.Registers[0x08U / 4U] = 0x0000'7071U;
    ehci.Configuration[0x70U / 4U] = 0x0000'0002U;
    char missing[2048U]{};
    ReportText absentCapability{missing, sizeof(missing) - 1U};
    Asteria::Inventory::FormatEhciEvidence(absentCapability, ehci, SixSeriesEhci);
    ASTERIA_EXPECT(context, "an EECP below 40h is malformed, and a list without ID 1 has no USBLEGSUP",
                   Contains(malformed, "  EECP malformed at 0x30\r\n"),
                   Contains(missing, "  USBLEGSUP not found in the EECP list\r\n"));

    ehci.Configuration[0x04U / 4U] = 0xFFFF'FFFFU;
    char gone[256U]{};
    ReportText absent{gone, sizeof(gone) - 1U};
    Asteria::Inventory::FormatEhciEvidence(absent, ehci, SixSeriesEhci);
    ASTERIA_EXPECT(context, "an all-ones function ends its block",
                   Contains(gone, "8086:1c2d absent (configuration reads all-ones)\r\n"), !Contains(gone, "PMCSR"));

    const EhciEvidenceMemory map[]{
        {0x2000'0000U, 5U, 4U, 0U},
        {0x2000'5000U, 4U, 0U, 0U},
        {0x2000'9000U, 1U, 10U, 0U},
    };
    char range[512U]{};
    ReportText memory{range, sizeof(range) - 1U};
    Asteria::Inventory::FormatRmrrMemory(memory, 0U, 0x2000'5000U, 0x2000'8FFFU, map, 3U);
    char spanning[512U]{};
    ReportText gap{spanning, sizeof(spanning) - 1U};
    Asteria::Inventory::FormatRmrrMemory(gap, 1U, 0x2000'4000U, 0x2000'AFFFU, map + 1U, 2U);
    ASTERIA_EXPECT(
        context, "an RMRR names the UEFI types that cover it, and its gaps",
        Contains(range, "  RMRR#0 0x0000000020005000-0x0000000020008fff UEFI map: type=0(Reserved) "
                        "0x0000000020005000-0x0000000020008fff\r\n"),
        Contains(
            spanning,
            "  RMRR#1 0x0000000020004000-0x000000002000afff UEFI map: gap "
            "0x0000000020004000-0x0000000020004fff type=0(Reserved) 0x0000000020005000-0x0000000020008fff "
            "type=10(AcpiNvs) 0x0000000020009000-0x0000000020009fff gap 0x000000002000a000-0x000000002000afff\r\n"));

    Lpc lpc{};
    char smi[512U]{};
    ReportText pch{smi, sizeof(smi) - 1U};
    Asteria::Inventory::FormatPchSmi(pch, lpc, 0x1008U);
    ASTERIA_EXPECT(context, "Intel 6-series SMI_EN is read at PMBASE + 30h", lpc.LastPort == 0x1030U,
                   Contains(smi,
                            "PCH SMI_EN LPC 8086:1c4f PMBASE=0x1000 ACPI_EN=1 FADT-PM-timer=0x1008 (PMBASE+08h) "
                            "SMI_EN=0x0006200b GBL_SMI_EN=1 LEGACY_USB_EN=1 LEGACY_USB2_EN=1 INTEL_USB2_EN=1\r\n"));

    lpc.Ports = 0U;
    lpc.Control = 0U;
    char disabled[512U]{};
    ReportText off2{disabled, sizeof(disabled) - 1U};
    Asteria::Inventory::FormatPchSmi(off2, lpc, 0x2008U);
    lpc.Control = 0x80U;
    lpc.Identity = 0x2918'8086U;
    char ich9[512U]{};
    ReportText other{ich9, sizeof(ich9) - 1U};
    Asteria::Inventory::FormatPchSmi(other, lpc, 0U);
    lpc.Identity = 0xFFFF'FFFFU;
    char none[512U]{};
    ReportText absentBridge{none, sizeof(none) - 1U};
    Asteria::Inventory::FormatPchSmi(absentBridge, lpc, 0U);
    ASTERIA_EXPECT(context, "no port is read without ACPI decode or a known 6-series bridge", lpc.Ports == 0U,
                   Contains(disabled, "ACPI_EN=0 FADT-PM-timer=0x2008 (differs from PMBASE+08h) SMI_EN not read: "
                                      "ACPI I/O decode off\r\n"),
                   Contains(ich9, "PCH SMI_EN LPC 8086:2918 unknown layout\r\n"),
                   Contains(none, "PCH SMI_EN unknown layout: no LPC bridge at 00:1f.0\r\n"));
    return context.Finish();
}
