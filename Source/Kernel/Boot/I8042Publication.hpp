// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_BOOT_I8042_PUBLICATION_HPP_INCLUDED
#define ASTERIA_KERNEL_BOOT_I8042_PUBLICATION_HPP_INCLUDED

#include "Abi/I8042.hpp"
#include "Boot/Acpi/AcpiTopology.hpp"
#include "Hal/Device/IPortIo.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Platform/Firmware/Diagnostic.hpp"

/// The kernel's ISA-neutral i8042 publication rule: the declaration gate, the PC/AT command
/// allowlist, the evidence-driven routing and signaling of ISA IRQ 1/12 and the publication lines.
/// AMD64's boot runtime and IA-64's pager runtime own the objects.
namespace Asteria::Boot::Bootstrap
{
enum class I8042PublicationReason : Foundation::UInt8
{
    Published,
    NoEvidence,
    FloatingPort,
    Resources,
    DsdtMismatch,
};

struct I8042Publication
{
    Foundation::UInt64 Declared{0U};
    I8042PublicationReason Reason{I8042PublicationReason::NoEvidence};
    /// The DSDT-bound IRQ 1/12 signaling (Abi::I8042InterruptMask) when the publication rests on
    /// namespace evidence alone; zero for a FADT one.
    Foundation::UInt32 AmlInterrupts{0U};
};

/// One ordered status read, no controller commands. Call on the BSP before
/// publishing authority. A FADT declaration also has to pass this gate.
[[nodiscard]] inline I8042Publication GateI8042(const Foundation::UInt16 bootArchitecture,
                                                const Foundation::UInt32 amlFlags, const bool dsdtMatches,
                                                Hal::Device::IPortIo& ports) noexcept
{
    I8042Publication result{};
    result.Declared = (bootArchitecture & 2U) != 0U ? Abi::I8042FadtFlag : 0U;
    if (dsdtMatches)
    {
        result.Declared |= amlFlags & (Abi::I8042KeyboardAml | Abi::I8042MouseAml | Abi::I8042StaUnknown);
    }
    if ((result.Declared & (Abi::I8042FadtFlag | Abi::I8042KeyboardAml)) == 0U)
    {
        result.Reason =
            amlFlags != 0U && !dsdtMatches ? I8042PublicationReason::DsdtMismatch : I8042PublicationReason::NoEvidence;
        result.Declared = 0U;
        return result;
    }
    if (ports.Read(0x64U, 1U) == 0xFFU)
    {
        result.Reason = I8042PublicationReason::FloatingPort;
        result.Declared = 0U;
        return result;
    }
    result.Reason = I8042PublicationReason::Published;
    if ((result.Declared & Abi::I8042FadtFlag) == 0U)
    {
        result.AmlInterrupts = amlFlags & static_cast<Foundation::UInt32>(Abi::I8042InterruptMask);
    }
    return result;
}

[[nodiscard]] inline const char* I8042ReasonText(const I8042PublicationReason reason) noexcept
{
    switch (reason)
    {
    case I8042PublicationReason::Published:
        return "published";
    case I8042PublicationReason::NoEvidence:
        return "no-evidence";
    case I8042PublicationReason::FloatingPort:
        return "port-0x64-floating";
    case I8042PublicationReason::Resources:
        return "resources";
    case I8042PublicationReason::DsdtMismatch:
        return "dsdt-mismatch";
    }
    return "no-evidence";
}

/// The exact publication lines on the diagnostic channel; identifier is the
/// keyboard's EISA PNP number from the bound namespace evidence.
inline void ReportI8042(const I8042Publication& publication, const Foundation::UInt32 identifier) noexcept
{
    if (publication.Reason != I8042PublicationReason::Published)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:I8042:UNPUBLISHED\r\n");
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:I8042:REASON:");
        Platform::Firmware::WriteDiagnosticAscii(I8042ReasonText(publication.Reason));
    }
    else if ((publication.Declared & Abi::I8042FadtFlag) != 0U)
    {
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:I8042:PUBLISHED:FADT-8042");
    }
    else
    {
        char id[]{"PNP0000"};
        constexpr char digits[]{"0123456789ABCDEF"};
        for (Foundation::Size digit{0U}; digit < 4U; ++digit)
        {
            id[6U - digit] = digits[(identifier >> (digit * 4U)) & 15U];
        }
        Platform::Firmware::WriteDiagnosticAscii("ASTERIA:I8042:PUBLISHED:AML-");
        Platform::Firmware::WriteDiagnosticAscii(id);
        if ((publication.Declared & Abi::I8042StaUnknown) != 0U)
        {
            Platform::Firmware::WriteDiagnosticAscii("-STA-UNKNOWN");
        }
    }
    Platform::Firmware::WriteDiagnosticAscii("\r\n");
}

// Permitted PC/AT commands: configuration, interface control and auxiliary
// forwarding. Output-port writes, reset and self-test are absent.
inline constexpr Foundation::UInt32 I8042Commands[]{0x20U, 0x60U, 0xA7U, 0xA8U, 0xA9U, 0xABU, 0xADU, 0xAEU, 0xD4U};
inline constexpr Kernel::Device::DevicePortWriteList I8042CommandWrites[]{
    {.Offset = 0U, .Width = Kernel::Device::DeviceAccessWidth::Byte, .Values = I8042Commands, .Count = 9U}};

enum class I8042RouteSource : Foundation::UInt8
{
    IsaDefault,
    SourceOverride,
    Aml,
};

struct I8042Route
{
    Foundation::UInt32 Gsi;
    bool Level;
    bool ActiveLow;
    I8042RouteSource Source;
};

/// The GSI, trigger and polarity of ISA IRQ 1 or 12, from the strongest evidence: 1. a MADT type-2
/// ISA source override (ACPI 6.5, 5.2.12.5; both ISAs): its GSI, and its MPS INTI flags, where
/// "conforms to the bus" is the ISA default; an override with a reserved field or flag bit is
/// ignored; 2. for a publication from DSDT-bound namespace evidence alone, the IRQ or Extended
/// Interrupt descriptor of the declaring device's _CRS (ACPI 6.5, 6.4.2.1 _HE/_LL, 6.4.3.6),
/// amlInterrupts from GateI8042; 3. the ISA default: edge-triggered, active high.
[[nodiscard]] inline I8042Route ResolveI8042Route(const Acpi::AcpiTopology& topology, const Foundation::UInt8 irq,
                                                  const Foundation::UInt32 amlInterrupts) noexcept
{
    I8042Route route{.Gsi = irq, .Level = false, .ActiveLow = false, .Source = I8042RouteSource::IsaDefault};
    for (Foundation::Size index{0U};
         (index < topology.InterruptOverrideCount) && (index < Acpi::MaximumAcpiInterruptOverrides); ++index)
    {
        const Acpi::AcpiInterruptOverride& entry = topology.InterruptOverrides[index];
        if ((entry.Bus != 0U) || (entry.Source != irq) || ((entry.Flags & ~0xFU) != 0U) || ((entry.Flags & 3U) == 2U) ||
            (((entry.Flags >> 2U) & 3U) == 2U))
        {
            continue;
        }
        route.Gsi = entry.GlobalSystemInterrupt;
        route.ActiveLow = (entry.Flags & 3U) == 3U;
        route.Level = ((entry.Flags >> 2U) & 3U) == 3U;
        route.Source = I8042RouteSource::SourceOverride;
        return route;
    }
    const bool mouse = irq == 12U;
    const Foundation::UInt32 declared =
        static_cast<Foundation::UInt32>(mouse ? Abi::I8042MouseIrqDeclared : Abi::I8042KeyboardIrqDeclared);
    if (((irq == 1U) || mouse) && ((amlInterrupts & declared) != 0U))
    {
        route.Level = (amlInterrupts & static_cast<Foundation::UInt32>(mouse ? Abi::I8042MouseIrqLevel
                                                                             : Abi::I8042KeyboardIrqLevel)) != 0U;
        route.ActiveLow =
            (amlInterrupts & static_cast<Foundation::UInt32>(mouse ? Abi::I8042MouseIrqActiveLow
                                                                   : Abi::I8042KeyboardIrqActiveLow)) != 0U;
        route.Source = I8042RouteSource::Aml;
    }
    return route;
}
} // Namespace Asteria::Boot::Bootstrap

#endif // ASTERIA_KERNEL_BOOT_I8042_PUBLICATION_HPP_INCLUDED
