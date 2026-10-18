// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Firmware/DiagnosticSinkSelection.hpp"

#include "Platform/Amd64/Firmware/UartProbe.hpp"

namespace Asteria::Platform::Amd64::Firmware
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Foundation::UIntPointer;

constexpr Size SdtHeaderBytes{36U};
constexpr Size RsdpVersion1Bytes{20U};
constexpr Size RsdpVersion2Bytes{36U};
// Bounds that keep a corrupt table from steering reads far: a root table of
// at most 256 entries, an SPCR of at most a page.
constexpr Size MaximumRootTableBytes{SdtHeaderBytes + (256U * 8U)};
constexpr Size MinimumSpcrBytes{80U};
constexpr Size MaximumSpcrBytes{4096U};
constexpr UInt64 AddressLimit{UInt64{1U} << 46U};
constexpr Size SpcrInterfaceTypeOffset{36U};
constexpr Size SpcrAddressSpaceOffset{40U};
constexpr Size SpcrAddressOffset{44U};
constexpr UInt8 SystemMemorySpace{0U};
constexpr UInt8 SystemIoSpace{1U};
constexpr UInt64 MaximumUartPort{0xFFF8U};

[[nodiscard]] UInt64 ReadLittleEndian(const UInt8* const bytes, const Size count) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < count; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] bool ChecksumIsZero(const UInt8* const bytes, const Size count) noexcept
{
    UInt8 sum{0U};
    for (Size index{0U}; index < count; ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    return sum == 0U;
}

[[nodiscard]] bool SignatureIs(const UInt8* const bytes, const char* const signature, const Size count) noexcept
{
    for (Size index{0U}; index < count; ++index)
    {
        if (bytes[index] != static_cast<UInt8>(signature[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const UInt8* Pointer(const UInt64 address) noexcept
{
    return reinterpret_cast<const UInt8*>(static_cast<UIntPointer>(address));
}

// The table at address with signature, when its header is sane, its length
// within [minimum, maximum] and its checksum zero; null otherwise.
[[nodiscard]] const UInt8* ResolveTable(const UInt64 address, const char* const signature, const Size minimum,
                                        const Size maximum, Size& length) noexcept
{
    if ((address == 0U) || (address >= AddressLimit))
    {
        return nullptr;
    }
    const UInt8* const table = Pointer(address);
    if (!SignatureIs(table, signature, 4U))
    {
        return nullptr;
    }
    const UInt64 declared = ReadLittleEndian(table + 4U, 4U);
    if ((declared < minimum) || (declared > maximum) || !ChecksumIsZero(table, static_cast<Size>(declared)))
    {
        return nullptr;
    }
    length = static_cast<Size>(declared);
    return table;
}

[[nodiscard]] SpcrSerialPort Result(const SpcrSerialStatus status, const UInt16 port = 0U) noexcept
{
    return SpcrSerialPort{.Port = port, .Status = status, .Reserved = 0U};
}

[[nodiscard]] SpcrSerialPort DecodeSpcr(const UInt8* const spcr) noexcept
{
    // DBG2 serial subtypes: 0x00 full 16550, 0x01 16550 subset, 0x12
    // 16550-compatible with parameters in the generic address.
    const UInt8 interfaceType = spcr[SpcrInterfaceTypeOffset];
    if ((interfaceType != 0x00U) && (interfaceType != 0x01U) && (interfaceType != 0x12U))
    {
        return Result(SpcrSerialStatus::Unsupported);
    }
    const UInt8 space = spcr[SpcrAddressSpaceOffset];
    const UInt64 address = ReadLittleEndian(spcr + SpcrAddressOffset, 8U);
    if (space == SystemMemorySpace)
    {
        return Result(SpcrSerialStatus::MemoryMapped);
    }
    if ((space != SystemIoSpace) || (address == 0U) || (address > MaximumUartPort))
    {
        return Result(SpcrSerialStatus::Unsupported);
    }
    return Result(SpcrSerialStatus::Found, static_cast<UInt16>(address));
}
} // namespace

bool IsQemuDebugPortSignature(const UInt32 ebx, const UInt32 ecx, const UInt32 edx) noexcept
{
    constexpr char Signatures[2U][13U]{"TCGTCGTCGTCG", {'K', 'V', 'M', 'K', 'V', 'M', 'K', 'V', 'M', '\0', '\0', '\0'}};
    const UInt32 registers[3U]{ebx, ecx, edx};
    for (const char (&signature)[13U] : Signatures)
    {
        bool equal{true};
        for (Size index{0U}; equal && (index < 12U); ++index)
        {
            // The signature's bytes in register order, each little-endian.
            const UInt8 byte = static_cast<UInt8>(registers[index / 4U] >> ((index % 4U) * 8U));
            equal = byte == static_cast<UInt8>(signature[index]);
        }
        if (equal)
        {
            return true;
        }
    }
    return false;
}

SpcrSerialPort FindSpcrSerialPort(const void* const rsdpPointer) noexcept
{
    const UInt64 rsdpAddress = static_cast<UInt64>(reinterpret_cast<UIntPointer>(rsdpPointer));
    if ((rsdpAddress == 0U) || (rsdpAddress >= AddressLimit))
    {
        return Result(SpcrSerialStatus::Absent);
    }
    const UInt8* const rsdp = Pointer(rsdpAddress);
    if (!SignatureIs(rsdp, "RSD PTR ", 8U) || !ChecksumIsZero(rsdp, RsdpVersion1Bytes))
    {
        return Result(SpcrSerialStatus::Absent);
    }
    bool extended = rsdp[15U] >= 2U;
    if (extended)
    {
        const UInt64 rsdpLength = ReadLittleEndian(rsdp + 20U, 4U);
        extended = (rsdpLength >= RsdpVersion2Bytes) && (rsdpLength <= 4096U) &&
                   ChecksumIsZero(rsdp, static_cast<Size>(rsdpLength)) && (ReadLittleEndian(rsdp + 24U, 8U) != 0U);
    }
    const Size entryBytes = extended ? 8U : 4U;
    Size rootLength{0U};
    const UInt8* const root =
        extended
            ? ResolveTable(ReadLittleEndian(rsdp + 24U, 8U), "XSDT", SdtHeaderBytes, MaximumRootTableBytes, rootLength)
            : ResolveTable(ReadLittleEndian(rsdp + 16U, 4U), "RSDT", SdtHeaderBytes, MaximumRootTableBytes, rootLength);
    if (root == nullptr)
    {
        return Result(SpcrSerialStatus::Absent);
    }
    for (Size offset{SdtHeaderBytes}; (offset + entryBytes) <= rootLength; offset += entryBytes)
    {
        Size spcrLength{0U};
        const UInt8* const spcr = ResolveTable(ReadLittleEndian(root + offset, entryBytes), "SPCR", MinimumSpcrBytes,
                                               MaximumSpcrBytes, spcrLength);
        if (spcr != nullptr)
        {
            return DecodeSpcr(spcr);
        }
    }
    return Result(SpcrSerialStatus::Absent);
}
} // namespace Asteria::Platform::Amd64::Firmware

namespace Asteria::Platform::Amd64::Firmware
{
bool ProbeUart(IUartRegisters& registers) noexcept
{
    using Foundation::UInt8;
    // National PC16550D (June 1995), section 8.10: scratch is ordinary
    // eight-bit storage. Exercise every data bit in both states and restore.
    const UInt8 scratch = registers.Read(7U);
    bool present{true};
    for (unsigned int bit{0U}; bit < 8U; ++bit)
    {
        const UInt8 one = static_cast<UInt8>(1U << bit);
        registers.Write(7U, one);
        present = present && registers.Read(7U) == one;
        const UInt8 complement = static_cast<UInt8>(one ^ 0xFFU);
        registers.Write(7U, complement);
        present = present && registers.Read(7U) == complement;
    }
    registers.Write(7U, scratch);
    if (!present)
    {
        return false;
    }
    const UInt8 line = registers.Read(3U);
    const UInt8 modem = registers.Read(4U);
    // Sections 8.1/8.7: DLAB must be clear to access IER, which disables
    // all interrupt sources with zero. Preserve the pre-probe state on failure.
    registers.Write(3U, static_cast<UInt8>(line & 0x7FU));
    const UInt8 interrupts = registers.Read(1U);
    registers.Write(1U, 0U);
    // Sections 8.8/8.9: LOOP isolates external modem inputs. Toggle each
    // output independently: RTS->CTS, DTR->DSR, OUT1->RI, OUT2->DCD.
    for (unsigned int outputs{0U}; outputs < 16U; ++outputs)
    {
        registers.Write(4U, static_cast<UInt8>(0x10U | outputs));
        const UInt8 expected =
            static_cast<UInt8>(((outputs & 2U) << 3U) | ((outputs & 1U) << 5U) | ((outputs & 12U) << 4U));
        present = present && ((registers.Read(6U) & 0xF0U) == expected);
    }
    // Section 8.6: IIR bit 0 is one with all interrupt sources disabled;
    // bits 5:4 are always zero. Section 8.4 LSR is read-only; do not invent
    // a writable-LSR detection test or require an idle transmitter.
    present = present && ((registers.Read(2U) & 0x31U) == 1U);
    if (!present)
    {
        registers.Write(4U, modem);
        registers.Write(1U, interrupts);
        registers.Write(3U, line);
        return false;
    }
    // Sections 8.1/8.3/8.5/8.7/8.8: divisor 1, 8N1, clear FIFOs,
    // no UART interrupt sources or OUT2 board interrupt routing.
    registers.Write(4U, 3U);
    registers.Write(3U, 0x80U);
    registers.Write(0U, 1U);
    registers.Write(1U, 0U);
    registers.Write(3U, 3U);
    registers.Write(2U, 7U);
    return true;
}
} // namespace Asteria::Platform::Amd64::Firmware
