// SPDX-License-Identifier: Apache-2.0

#include "Platform/Firmware/Diagnostic.hpp"

#include "Foundation/Types.hpp"
#include "Platform/Firmware/DiagnosticUserRecord.hpp"
#include "Platform/Synchronization/AtomicUInt64.hpp"
#include "Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp"
#include "Platform/Amd64/Firmware/UartProbe.hpp"

// Kernel-owned diagnostic sinks, installed from BootInfo by the kernel entry
// and shared with the activation's assembly stage and fail-stop writers.
// The loader captures the same decision into its own storage. These words
// stay zero until kernel preparation installs it.
extern "C"
{
constinit Asteria::Foundation::UInt8 AsteriaAmd64DebugPortPresent{0U};
constinit Asteria::Foundation::UInt16 AsteriaAmd64DiagnosticSerialPort{0U};
}

namespace Asteria::Platform::Amd64::Firmware
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt8;

constexpr UInt16 DebugPort{0x0402U};
constexpr UInt16 Com1Port{0x03F8U};
// 16550 registers, offsets from the base.
constexpr UInt16 TransmitHolding{0U};
constexpr UInt16 LineStatus{5U};
constexpr UInt8 TransmitHoldingEmpty{0x20U};
// Each wait polls the line status at most this often: a few milliseconds of
// port reads on hardware, then the byte is dropped or the UART abandoned.
constexpr Size MaximumStatusPolls{20'000U};

void Out(const UInt16 port, const UInt8 value) noexcept
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port) : "memory");
}

[[nodiscard]] UInt8 In(const UInt16 port) noexcept
{
    UInt8 value{0U};
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

// QEMU's isa-debugcon at 0x402 exists only under QEMU. On real chipsets
// 0x402 may fall inside the ACPI PM1 block, where writes can toggle wake
// and event enables. Other hypervisors (VMware, Hyper-V) set the hypervisor bit
// too, so the vendor signature decides.
[[nodiscard]] bool HasQemuDebugPort() noexcept
{
    constexpr UInt32 HypervisorPresentBit{1U << 31U};
    UInt32 eax{1U};
    UInt32 ebx{0U};
    UInt32 ecx{0U};
    UInt32 edx{0U};
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if ((ecx & HypervisorPresentBit) == 0U)
    {
        return false;
    }
    eax = 0x4000'0000U;
    ecx = 0U;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    return IsQemuDebugPortSignature(ebx, ecx, edx);
}

// Port I/O is the platform adapter for the specification-authored probe.
class UartRegisters final : public Asteria::Platform::Amd64::Firmware::IUartRegisters
{
public:
    explicit UartRegisters(const UInt16 base) noexcept : m_base{base}
    {
    }
    [[nodiscard]] UInt8 Read(const UInt8 offset) noexcept override
    {
        return In(static_cast<UInt16>(m_base + offset));
    }
    void Write(const UInt8 offset, const UInt8 value) noexcept override
    {
        Out(static_cast<UInt16>(m_base + offset), value);
    }

private:
    UInt16 m_base;
};

// Any processor may write a diagnostic, so the UART's base is read and
// dropped atomically. Relaxed order suffices: the base was published before
// any other processor started, and it only ever changes to zero, after
// which a writer that still uses the old base merely loses its bytes.
void WriteSerialByte(UInt16& port, const UInt8 value) noexcept
{
    const UInt16 base = __atomic_load_n(&port, __ATOMIC_RELAXED);
    if (base == 0U)
    {
        return;
    }
    for (Size poll{0U}; poll < MaximumStatusPolls; ++poll)
    {
        if ((In(static_cast<UInt16>(base + LineStatus)) & TransmitHoldingEmpty) != 0U)
        {
            Out(static_cast<UInt16>(base + TransmitHolding), value);
            return;
        }
    }
    // A UART that stops draining is abandoned rather than slowing every
    // later diagnostic.
    __atomic_store_n(&port, UInt16{0U}, __ATOMIC_RELAXED);
}

} // namespace

void CaptureDiagnosticSinks(const void* const rsdp, Boot::BootDiagnosticSinks& output) noexcept
{
    output.DebugPortPresent = HasQemuDebugPort() ? 1U : 0U;
    const SpcrSerialPort spcr = FindSpcrSerialPort(rsdp);
    const UInt16 base = (spcr.Status == SpcrSerialStatus::Found) ? spcr.Port : Com1Port;
    UartRegisters uart{base};
    output.SerialPort = ProbeUart(uart) ? base : 0U;
    output.Reserved = 0U;
}

void InstallDiagnosticSinks(const Boot::BootDiagnosticSinks& sinks) noexcept
{
    AsteriaAmd64DebugPortPresent = static_cast<UInt8>(sinks.DebugPortPresent);
    AsteriaAmd64DiagnosticSerialPort = sinks.SerialPort;
}

void WriteDiagnosticByteToPorts(const UInt8 debugPortPresent, UInt16& serialPort, const UInt8 value) noexcept
{
    if (debugPortPresent != 0U)
    {
        Out(DebugPort, value);
    }
    WriteSerialByte(serialPort, value);
}

void WriteDiagnosticByte(const UInt8 value) noexcept
{
    WriteDiagnosticByteToPorts(AsteriaAmd64DebugPortPresent, AsteriaAmd64DiagnosticSerialPort, value);
}
} // namespace Asteria::Platform::Amd64::Firmware

namespace Asteria::Platform::Firmware
{
namespace
{
constinit Foundation::UInt64 g_exclusion{0U};
[[nodiscard]] bool TryBeginDiagnostic() noexcept
{
    Foundation::UInt64 expected{0U};
    return Synchronization::AtomicUInt64Reference{g_exclusion}.CompareExchangeStrongAcquireRelease(expected, 1U);
}
void EndDiagnostic() noexcept
{
    Synchronization::AtomicUInt64Reference{g_exclusion}.StoreRelease(0U);
}
} // namespace

void WriteDiagnosticAscii(const char* const text) noexcept
{
    if (!TryBeginDiagnostic())
    {
        return;
    }
    for (const char* cursor = text; *cursor != '\0'; ++cursor)
    {
        Amd64::Firmware::WriteDiagnosticByte(static_cast<Foundation::UInt8>(*cursor));
    }
    EndDiagnostic();
}

void WriteDiagnosticBytes(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    if (!TryBeginDiagnostic())
    {
        return;
    }
    for (Foundation::Size index{0U}; index < sizeBytes; ++index)
    {
        Amd64::Firmware::WriteDiagnosticByte(bytes[index]);
    }
    EndDiagnostic();
}

void WriteDiagnosticUserLine(const Foundation::UInt8* const bytes, const Foundation::Size sizeBytes) noexcept
{
    if (!TryBeginDiagnostic())
    {
        return;
    }
    WriteDiagnosticUserRecord(
        [](const Foundation::UInt8 value) noexcept
        {
            Amd64::Firmware::WriteDiagnosticByte(value);
            return true;
        },
        bytes, sizeBytes);
    EndDiagnostic();
}

[[noreturn]] void HaltAfterFirmwareExit() noexcept
{
#if defined(__x86_64__)
    __asm__ volatile("cli\n\t"
                     "1:\n\t"
                     "hlt\n\t"
                     "jmp 1b"
                     :
                     :
                     : "memory");
#else
    __builtin_trap();
#endif
    __builtin_unreachable();
}
} // namespace Asteria::Platform::Firmware
