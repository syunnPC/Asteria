// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_INVENTORY_EHCI_RMRR_REPORT_HPP_INCLUDED
#define ASTERIA_INVENTORY_EHCI_RMRR_REPORT_HPP_INCLUDED

#include "Inventory/ReportText.hpp"

namespace Asteria::Inventory
{
/// Read-only access to one EHCI function reserved by a VT-d RMRR, for the boot report.
class IEhciEvidenceAccess
{
public:
    /// The function's configuration dword at a naturally aligned offset below
    /// 0x100; all-ones when it cannot be read.
    [[nodiscard]] virtual Foundation::UInt32 ReadConfiguration(Foundation::UInt16 offset) noexcept = 0;
    /// The dword at offset of BAR 0's register block. The formatter calls it
    /// only while the function decodes memory and only below the firmware's
    /// extent of the BAR.
    [[nodiscard]] virtual Foundation::UInt32 ReadRegister(Foundation::UInt32 offset) noexcept = 0;

protected:
    ~IEhciEvidenceAccess() = default;
};

/// One reserved EHCI function as the inventory describes it. BarBytes is the
/// firmware's decoded extent of BAR 0 (zero when the firmware described
/// none), which bounds every register read.
struct EhciEvidenceFunction
{
    Foundation::UInt16 Segment;
    Foundation::UInt8 Bus;
    Foundation::UInt8 Device;
    Foundation::UInt8 Function;
    Foundation::UInt8 Reserved[3];
    Foundation::UInt16 Vendor;
    Foundation::UInt16 Product;
    Foundation::UInt32 Reserved2;
    Foundation::UInt64 BarBytes;
};

/// One UEFI memory-map descriptor: its raw type and its span in pages.
struct EhciEvidenceMemory
{
    Foundation::UInt64 Start;
    Foundation::UInt64 Pages;
    Foundation::UInt32 Type;
    Foundation::UInt32 Reserved;
};

/// The read-only accesses of the PCH's SMI_EN register: the LPC bridge's
/// (00:1f.0) configuration dwords and a 32-bit port read.
class IPchSmiAccess
{
public:
    /// The LPC bridge's configuration dword, all-ones when absent.
    [[nodiscard]] virtual Foundation::UInt32 ReadLpcConfiguration(Foundation::UInt16 offset) noexcept = 0;
    [[nodiscard]] virtual Foundation::UInt32 ReadPort32(Foundation::UInt16 port) noexcept = 0;

protected:
    ~IPchSmiAccess() = default;
};

/// The EHCI's lines: power state, Memory Space and Bus Master Enable, BAR 0,
/// then (while memory decodes) CAPLENGTH, HCSPARAMS (N_PORTS, N_PCC, N_CC),
/// HCCPARAMS and the EECP list's USBLEGSUP and USBLEGCTLSTS (EHCI 1.0
/// sections 2.1.7, 2.1.8, 2.2 and 2.3), USBCMD, USBSTS and CONFIGFLAG, and
/// the Intel 6-series SPECIAL_SMI register (Intel 6 Series Chipset and C200
/// Series Chipset datasheet 324645-006, section 16.1.29) for the device IDs
/// the specification update lists (1C26h, 1C2Dh); any other device's second
/// SMI register is reported as an unknown layout. All-ones configuration
/// reads end the block (an absent function). At most 48 capabilities of
/// each list are walked.
void FormatEhciEvidence(ReportText& text, IEhciEvidenceAccess& access, const EhciEvidenceFunction& function) noexcept;

/// The RMRR range [base, limit] and the UEFI memory-map descriptors that
/// cover it (at most four, with any uncovered span named a gap).
void FormatRmrrMemory(ReportText& text, Foundation::Size region, Foundation::UInt64 base, Foundation::UInt64 limit,
                      const EhciEvidenceMemory* memory, Foundation::Size count) noexcept;

/// The PCH's SMI_EN (PMBASE + 30h) USB-related bits, read only: PMBASE from
/// the LPC bridge's offset 40h with ACPI_EN (offset 44h, bit 7) set, for the
/// 6-series LPC device IDs of the specification update (datasheet sections
/// 13.1.12, 13.1.13 and 13.8.3.7), checked against the FADT's PM timer block
/// (PMBASE + 08h, section 13.8.3.4) when the FADT names one in I/O space.
/// Any other bridge is reported as an unknown layout and no port is read.
void FormatPchSmi(ReportText& text, IPchSmiAccess& access, Foundation::UInt64 pmTimerPort) noexcept;
} // Namespace Asteria::Inventory

#endif // ASTERIA_INVENTORY_EHCI_RMRR_REPORT_HPP_INCLUDED
