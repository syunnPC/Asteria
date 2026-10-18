// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_ACPI_DEBUG_PORT_TABLES_HPP_INCLUDED
#define ASTERIA_BOOT_ACPI_DEBUG_PORT_TABLES_HPP_INCLUDED

#include "Boot/BootDebugPort.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Boot::Acpi
{
/// One firmware table: Length readable bytes at Bytes, or none (null).
struct DebugPortTableView
{
    const Foundation::UInt8* Bytes;
    Foundation::Size Length;
};

/// The tables that can name a diagnostic UART. Any of them may be absent.
struct DebugPortTableSet
{
    DebugPortTableView Dbg2;
    DebugPortTableView Dbgp;
    DebugPortTableView Hcdp;
    DebugPortTableView Spcr;
};

/// The DIG64 HCDP/PCDP UART device descriptor types.
enum class HcdpUartType : Foundation::UInt8
{
    Console = 0U,
    DebugPort = 1U,
};

/// The decoders below read only bytes[0, length). Each accepts its table
/// only with the right signature, a declared length of at least its fixed
/// part and at most `length`, and a zero checksum over the declared length.
/// They return true and write port only for a 16550-compatible UART in
/// system memory or I/O space that IsValidBootDebugPort accepts; otherwise
/// they return false and leave port unchanged. No allocation, no firmware
/// call, bounded by the table length.
///
/// DBG2 (Microsoft "Debug Port Table 2"): the first Debug Device Information
/// structure of port type 0x8000 and subtype 0x0000 (16550), 0x0001 (16550
/// subset) or 0x0012 (16550 with the register stride and access size in its
/// generic address).
[[nodiscard]] bool DecodeDbg2DebugPort(const Foundation::UInt8* bytes, Foundation::Size length,
                                       BootDebugPort& port) noexcept;
/// DBGP (Microsoft "Debug Port Specification" 1.0): interface type 0 (16550)
/// or 1 (16550 subset).
[[nodiscard]] bool DecodeDbgpDebugPort(const Foundation::UInt8* bytes, Foundation::Size length,
                                       BootDebugPort& port) noexcept;
/// SPCR (Microsoft "Serial Port Console Redirection Table"): revision 1
/// interface types 0 (16550) and 1 (16450); from revision 2 the DBG2 serial
/// subtypes 0x0000, 0x0001 and 0x0012. A zero base address means console
/// redirection is disabled. The configured baud rate, or from revision 4 a
/// nonzero precise baud rate, is recorded.
[[nodiscard]] bool DecodeSpcrConsolePort(const Foundation::UInt8* bytes, Foundation::Size length,
                                         BootDebugPort& port) noexcept;
/// HCDP (DIG64 "Descriptions for Headless Console & Debug Port Devices" 1.0a,
/// and its PCDP 2.0 successor, revision 3): the first of the leading 48-byte
/// UART descriptors with this type.
[[nodiscard]] bool DecodeHcdpUart(const Foundation::UInt8* bytes, Foundation::Size length, HcdpUartType type,
                                  BootDebugPort& port) noexcept;

/// The first UART named by, in order: DBG2, DBGP, an HCDP debug port, SPCR,
/// an HCDP console. Dedicated debug ports come before the firmware console.
/// Returns fallback when none names a usable UART.
[[nodiscard]] BootDebugPort SelectDebugPort(const DebugPortTableSet& tables, const BootDebugPort& fallback) noexcept;

/// Finds DBG2, DBGP, HCDP and SPCR through the RSDP (the XSDT, or the RSDT
/// before ACPI 2.0) and, for HCDP, also the EFI configuration table the DIG64
/// specification defines (hcdpConfigurationTable, null for none; the root
/// table's HCDP comes first). The RSDP, root table and each table are
/// validated (signature, bounded length, checksum) before anything they point
/// to is read; addresses at or above 2^48 are refused unread. Every address
/// must be readable as identity-mapped physical memory before ExitBootServices. Missing tables are null views.
[[nodiscard]] DebugPortTableSet LocateDebugPortTables(const void* rsdp, const void* hcdpConfigurationTable) noexcept;
} // namespace Asteria::Boot::Acpi

#endif // ASTERIA_BOOT_ACPI_DEBUG_PORT_TABLES_HPP_INCLUDED
