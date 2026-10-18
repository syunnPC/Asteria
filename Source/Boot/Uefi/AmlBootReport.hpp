// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_UEFI_AML_BOOT_REPORT_HPP_INCLUDED
#define ASTERIA_BOOT_UEFI_AML_BOOT_REPORT_HPP_INCLUDED

#include "Boot/Uefi/AcpiReportVolume.hpp"

namespace Asteria::Boot::Uefi
{
/// BSP, before the final memory map, after REPORT.TXT and the raw dump. Borrows
/// immutable firmware tables; owns temporary LoaderData pages and releases them
/// before returning. No region handlers. Failures only append a report outcome;
/// AML keyboard evidence is retained independently of report writes. NOAML skips
/// allocation/evaluation and prevents AML publication on flag-clear machines.
struct AmlBootEvidence
{
    Foundation::UInt64 Address{0U};
    Foundation::UInt32 Length{0U};
    Foundation::UInt32 Flags{0U};
    Foundation::UInt32 Identifier{0U};
    Foundation::UInt8 Checksum{0U};
    bool Attempted{false};
    bool MaskFadt{false};
    Acpi::AcpiTableRecord SleepBinding{};
    Foundation::UInt8 TypeA{0U};
    Foundation::UInt8 TypeB{0U};
    bool SleepKnown{false};
};
/// BSP boot-only state; no allocation, table reads or firmware calls.
[[nodiscard]] AmlBootEvidence& GetAmlBootEvidence() noexcept;
/// Begin a single BSP boot attempt before any report destination is opened.
void ResetAmlBootEvidence() noexcept;
/// Copies bound evidence into the authoritative final-map topology; no allocation.
void ApplyAmlBootEvidence(Acpi::AcpiTopology& topology, Acpi::PowerRegisters* power = nullptr) noexcept;
[[nodiscard]] bool AmlBootEvidenceAttempted() noexcept;
void WriteAmlBootReportWithoutVolume(BootServices& services, const Acpi::AcpiTopology& topology,
                                     Inventory::ReportText& outcome) noexcept;
void WriteAmlBootReport(IAcpiReportVolume& volume, BootServices& services, const Acpi::AcpiTopology& topology,
                        Inventory::ReportText& outcome) noexcept;
}

#endif
