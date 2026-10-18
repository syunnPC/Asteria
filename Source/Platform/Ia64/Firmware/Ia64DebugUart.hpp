// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_DEBUG_UART_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_DEBUG_UART_HPP_INCLUDED

#include "Boot/BootDebugPort.hpp"
#include "Boot/BootInfo.hpp"
#include "Foundation/Types.hpp"

namespace Asteria::Platform::Ia64::Firmware
{
/// The physical addresses of the two 16550 registers the diagnostic writer
/// uses: the transmit holding register (0) and the line status register (5).
/// Both zero names no UART. The writer reaches them through the uncacheable
/// alias (bit 63) with data translation off; neither is mapped.
struct Ia64DebugUart
{
    Foundation::UInt64 TransmitAddress;
    Foundation::UInt64 LineStatusAddress;
};

/// The firmware's I/O port space window (UEFI EfiMemoryMappedIOPortSpace,
/// memory type 12): Bytes from Base, or zero for none.
struct Ia64PortSpaceWindow
{
    Foundation::UInt64 Base;
    Foundation::UInt64 Bytes;
};

/// The UEFI memory type of the I/O port space window.
inline constexpr Foundation::UInt32 Ia64PortSpaceFirmwareType{12U};

/// itanium2-vpc's debug UART, the platform default when no firmware table
/// names one: byte registers one byte apart in physical memory.
inline constexpr Boot::BootDebugPort Ia64PlatformDefaultDebugPort{
    .Address = 0x0000'0047'F000'1000ULL,
    .BaudRate = 0U,
    .AddressSpace = static_cast<Foundation::UInt8>(Boot::BootDebugPortSpace::SystemMemory),
    .AccessBytes = 1U,
    .RegisterStrideBytes = 1U,
    .Source = static_cast<Foundation::UInt8>(Boot::BootDebugPortSource::PlatformDefault),
};

/// The registers of Ia64PlatformDefaultDebugPort, which writers use until a
/// boot decision is installed.
inline constexpr Ia64DebugUart Ia64PlatformDefaultDebugUart{
    .TransmitAddress = 0x0000'0047'F000'1000ULL,
    .LineStatusAddress = 0x0000'0047'F000'1005ULL,
};

/// The one port space window among count regions, or a zero window when
/// there is none or more than one. Pure; no allocation.
[[nodiscard]] Ia64PortSpaceWindow FindIa64PortSpaceWindow(const Boot::PhysicalMemoryRegion* regions,
                                                          Foundation::Size count) noexcept;

/// Resolves a debug port to its register addresses. A memory port's
/// registers are Address + n * stride. An I/O port's are its sparse
/// translations in the port space window (SDM rev. 2.3 Vol. 2 Part I 10.7:
/// base + ((port >> 2) << 12) + (port & 0xFFF)), which must hold a whole
/// 64-MiB, 64-MiB aligned window. Returns a zero UART for None, for a record
/// IsValidBootDebugPort refuses, and for an I/O port without a usable window.
/// Pure; no allocation.
[[nodiscard]] Ia64DebugUart ResolveIa64DebugUart(const Boot::BootDebugPort& port,
                                                 const Ia64PortSpaceWindow& window) noexcept;
} // namespace Asteria::Platform::Ia64::Firmware

#endif // ASTERIA_PLATFORM_IA64_FIRMWARE_IA64_DEBUG_UART_HPP_INCLUDED
