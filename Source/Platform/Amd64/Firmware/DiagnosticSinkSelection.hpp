// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_FIRMWARE_DIAGNOSTIC_SINK_SELECTION_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_FIRMWARE_DIAGNOSTIC_SINK_SELECTION_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "Boot/BootKernelInput.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
/// Whether the hypervisor vendor signature of CPUID leaf 0x40000000 (EBX,
/// ECX, EDX) is QEMU's, the only host with the isa-debugcon port at 0x402:
/// "TCGTCGTCGTCG" (TCG) or "KVMKVMKVM\0\0\0" (KVM, as QEMU configures it).
/// Other hypervisors ("VMwareVMware", "Microsoft Hv") also set the
/// hypervisor bit of leaf 1, but 0x402 means nothing there. The caller reads
/// the leaf only when that bit is set: physical processors answer it with
/// unrelated data.
[[nodiscard]] bool IsQemuDebugPortSignature(Foundation::UInt32 ebx, Foundation::UInt32 ecx,
                                            Foundation::UInt32 edx) noexcept;

enum class SpcrSerialStatus : Foundation::UInt8
{
    /// The SPCR names a 16550-compatible UART in I/O port space.
    Found,
    /// No valid RSDP, root table or SPCR.
    Absent,
    /// The SPCR's UART is memory-mapped, which the diagnostic sink does not
    /// drive (only port I/O is supported).
    MemoryMapped,
    /// The SPCR names another interface type or an unusable address.
    Unsupported,
};

struct SpcrSerialPort
{
    Foundation::UInt16 Port;
    SpcrSerialStatus Status;
    Foundation::UInt8 Reserved;
};

/// Finds the console UART of the ACPI SPCR table through the RSDP at rsdp
/// (null for none): the RSDP's checksums, the XSDT (or, before ACPI 2.0, the
/// RSDT) and the SPCR are validated, signatures, bounded lengths and
/// checksums, before anything they point to is read. Accepts the 16550
/// interface types (full 16550, the 16550 subset, and 16550-compatible with
/// a generic address) at an I/O port of at most 0xFFF8. Every table address
/// must be readable as identity-mapped physical memory before ExitBootServices; addresses at or above 2^46 are refused unread.
[[nodiscard]] SpcrSerialPort FindSpcrSerialPort(const void* rsdp) noexcept;

/// Captures the debug-port and UART decision into caller-owned storage. BSP
/// only, bounded device polling, no allocation or UEFI calls.
void CaptureDiagnosticSinks(const void* rsdp, Boot::BootDiagnosticSinks& output) noexcept;

/// Installs the validated BootInfo decision in kernel-owned native/assembly
/// diagnostic state. Called only by the kernel entry, before AP admission.
void InstallDiagnosticSinks(const Boot::BootDiagnosticSinks& sinks) noexcept;

/// Writes one byte using caller-owned UART state; a stalled UART is cleared.
/// Port inputs are validated capture results. No allocation or firmware calls.
void WriteDiagnosticByteToPorts(Foundation::UInt8 debugPortPresent, Foundation::UInt16& serialPort,
                                Foundation::UInt8 value) noexcept;

static_assert(sizeof(SpcrSerialStatus) == 1U);
static_assert(sizeof(SpcrSerialPort) == 4U);
} // namespace Asteria::Platform::Amd64::Firmware

#endif // ASTERIA_PLATFORM_AMD64_FIRMWARE_DIAGNOSTIC_SINK_SELECTION_HPP_INCLUDED
