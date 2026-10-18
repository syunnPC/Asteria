// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_BOOT_BOOT_DEBUG_PORT_HPP_INCLUDED
#define ASTERIA_BOOT_BOOT_DEBUG_PORT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Boot
{
/// The firmware table that named the diagnostic UART
/// (Documentation/Architecture/BootProtocol.md, "Debug port").
enum class BootDebugPortSource : Foundation::UInt8
{
    /// No usable UART: diagnostics are dropped.
    None = 0U,
    /// Microsoft Debug Port Table 2, serial port type 0x8000.
    Dbg2 = 1U,
    /// Microsoft Debug Port Table (DBGP).
    Dbgp = 2U,
    /// DIG64 HCDP/PCDP device descriptor type 1 (debug port UART).
    HcdpDebugPort = 3U,
    /// Microsoft Serial Port Console Redirection table: the firmware console.
    Spcr = 4U,
    /// DIG64 HCDP/PCDP device descriptor type 0 (console UART): the firmware console.
    HcdpConsole = 5U,
    /// No table named a usable UART: the platform's fixed debug UART.
    PlatformDefault = 6U,
};

/// ACPI Generic Address Structure address space IDs a debug port may use.
enum class BootDebugPortSpace : Foundation::UInt8
{
    SystemMemory = 0U,
    SystemIo = 1U,
};

/// A 16550-compatible UART: register n lives at Address + n *
/// RegisterStrideBytes in AddressSpace (a physical address, or an I/O port
/// the platform translates) and is accessed AccessBytes wide. BaudRate is the
/// rate the firmware configured in bits per second, or zero when the table
/// does not say; the kernel never reprograms the UART. Fields are raw table
/// facts: consumers validate them with IsValidBootDebugPort before use.
struct BootDebugPort
{
    Foundation::UInt64 Address;
    Foundation::UInt32 BaudRate;
    Foundation::UInt8 AddressSpace;
    Foundation::UInt8 AccessBytes;
    Foundation::UInt8 RegisterStrideBytes;
    Foundation::UInt8 Source;
};

static_assert(sizeof(BootDebugPortSource) == 1U);
static_assert(sizeof(BootDebugPortSpace) == 1U);
static_assert(sizeof(BootDebugPort) == 16U);
static_assert(alignof(BootDebugPort) == 8U);
static_assert(__builtin_offsetof(BootDebugPort, BaudRate) == 8U);
static_assert(__builtin_offsetof(BootDebugPort, AddressSpace) == 12U);
static_assert(__builtin_offsetof(BootDebugPort, AccessBytes) == 13U);
static_assert(__builtin_offsetof(BootDebugPort, RegisterStrideBytes) == 14U);
static_assert(__builtin_offsetof(BootDebugPort, Source) == 15U);

/// The register file of a 16550: eight registers.
inline constexpr Foundation::UInt64 BootDebugPortRegisterCount{8U};
/// The highest I/O port address.
inline constexpr Foundation::UInt64 BootDebugPortMaximumIoPort{0xFFFFU};
/// Physical addresses at or above this never name a debug port: on IA-64 bit
/// 63 selects the uncacheable alias, and no supported processor implements
/// more than 50 physical address bits.
inline constexpr Foundation::UInt64 BootDebugPortPhysicalAddressLimit{Foundation::UInt64{1U} << 50U};

/// Whether port is a record the loader produces: a zero record for None;
/// otherwise a known source, a nonzero address whose eight registers lie
/// below BootDebugPortPhysicalAddressLimit (memory) or within the 16-bit I/O
/// space, byte-wide registers and a stride of 1, 2, 4 or 8 bytes.
[[nodiscard]] constexpr bool IsValidBootDebugPort(const BootDebugPort& port) noexcept
{
    if (port.Source == static_cast<Foundation::UInt8>(BootDebugPortSource::None))
    {
        return (port.Address == 0U) && (port.BaudRate == 0U) && (port.AddressSpace == 0U) && (port.AccessBytes == 0U) &&
               (port.RegisterStrideBytes == 0U);
    }
    const Foundation::UInt8 stride = port.RegisterStrideBytes;
    if ((port.Source > static_cast<Foundation::UInt8>(BootDebugPortSource::PlatformDefault)) || (port.Address == 0U) ||
        (port.AccessBytes != 1U) || ((stride != 1U) && (stride != 2U) && (stride != 4U) && (stride != 8U)))
    {
        return false;
    }
    const Foundation::UInt64 span = BootDebugPortRegisterCount * stride;
    if (port.AddressSpace == static_cast<Foundation::UInt8>(BootDebugPortSpace::SystemIo))
    {
        return port.Address <= (BootDebugPortMaximumIoPort + 1U - span);
    }
    return (port.AddressSpace == static_cast<Foundation::UInt8>(BootDebugPortSpace::SystemMemory)) &&
           (port.Address <= (BootDebugPortPhysicalAddressLimit - span));
}

/// Whether the UART is also the firmware's console (SPCR, HCDP type 0):
/// while boot services run, the firmware writes it too.
[[nodiscard]] constexpr bool IsFirmwareConsole(const BootDebugPort& port) noexcept
{
    return (port.Source == static_cast<Foundation::UInt8>(BootDebugPortSource::Spcr)) ||
           (port.Source == static_cast<Foundation::UInt8>(BootDebugPortSource::HcdpConsole));
}
} // namespace Asteria::Boot

#endif // ASTERIA_BOOT_BOOT_DEBUG_PORT_HPP_INCLUDED
