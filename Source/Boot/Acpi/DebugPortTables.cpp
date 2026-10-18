// SPDX-License-Identifier: Apache-2.0

#include "Boot/Acpi/DebugPortTables.hpp"

namespace Asteria::Boot::Acpi
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using Foundation::UIntPointer;

// ACPI 6.5 5.2.3.2 (Generic Address Structure) and 5.2.6 (System
// Description Table Header).
constexpr Size SdtHeaderBytes{36U};
constexpr Size GenericAddressBytes{12U};
constexpr Size RsdpVersion1Bytes{20U};
constexpr Size RsdpVersion2Bytes{36U};
constexpr Size MaximumRootTableBytes{SdtHeaderBytes + (256U * 8U)};
// No debug-port table here is longer than a page; a corrupt length cannot
// steer reads further.
constexpr Size MaximumTableBytes{4096U};
constexpr UInt64 AddressLimit{UInt64{1U} << 48U};
constexpr UInt8 AccessSizeUndefined{0U};
constexpr UInt8 AccessSizeByte{1U};

// DBG2 ("Debug Port Table 2", Table 1/2): the device array, then per device
// its length, generic-address count, port type/subtype and the offset of its
// first generic address.
constexpr Size Dbg2FixedBytes{44U};
constexpr Size Dbg2DeviceArrayOffset{36U};
constexpr Size Dbg2DeviceCountOffset{40U};
constexpr Size Dbg2DeviceFixedBytes{22U};
constexpr Size Dbg2DeviceLengthOffset{1U};
constexpr Size Dbg2DeviceAddressCountOffset{3U};
constexpr Size Dbg2DevicePortTypeOffset{12U};
constexpr Size Dbg2DevicePortSubtypeOffset{14U};
constexpr Size Dbg2DeviceAddressOffset{18U};
constexpr UInt16 Dbg2SerialPortType{0x8000U};
constexpr UInt16 SerialSubtype16550{0x0000U};
constexpr UInt16 SerialSubtype16550Subset{0x0001U};
constexpr UInt16 SerialSubtype16550Generic{0x0012U};
// Bounds the device walk: a table of at most a page holds fewer entries.
constexpr UInt32 MaximumDbg2Devices{MaximumTableBytes / Dbg2DeviceFixedBytes};

// DBGP (Debug Port Specification 1.0c, "Debug Port Table Format").
constexpr Size DbgpFixedBytes{52U};
constexpr Size InterfaceTypeOffset{36U};
constexpr Size BaseAddressOffset{40U};

// SPCR: interface type and base address as in DBGP, then the configured
// baud rate enumeration at 58 and, from revision 4, the precise baud rate.
constexpr Size SpcrFixedBytes{80U};
constexpr Size SpcrRevisionOffset{8U};
constexpr Size SpcrBaudOffset{58U};
constexpr Size SpcrPreciseBaudOffset{80U};
constexpr UInt8 SpcrPreciseBaudRevision{4U};

// HCDP 1.0a Table 2-1/2-2 and PCDP 2.0 Table 2-1/2-3: the count of the
// leading type 0/1 UART descriptors, 48 bytes each, each with its baud rate
// at 8 and its generic address at 16.
constexpr Size HcdpFixedBytes{40U};
constexpr Size HcdpUartCountOffset{36U};
constexpr Size HcdpUartBytes{48U};
constexpr Size HcdpUartBaudOffset{8U};
constexpr Size HcdpUartAddressOffset{16U};

enum class RegisterLayout : UInt8
{
    // Byte registers one byte apart (a PC 16550 or the DBGP subset).
    Legacy,
    // The register stride is the generic address's bit width and the access
    // size its access size field (DBG2 subtype 0x12).
    GenericAddress,
};

[[nodiscard]] UInt64 Read(const UInt8* const bytes, const Size count) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < count; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
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

[[nodiscard]] bool ChecksumIsZero(const UInt8* const bytes, const Size count) noexcept
{
    UInt8 sum{0U};
    for (Size index{0U}; index < count; ++index)
    {
        sum = static_cast<UInt8>(sum + bytes[index]);
    }
    return sum == 0U;
}

// The declared length of a table at bytes with this signature, when it is at
// least minimum, at most available and the table's checksum is zero; zero
// otherwise.
[[nodiscard]] Size ValidatedLength(const UInt8* const bytes, const Size available, const char* const signature,
                                   const Size minimum) noexcept
{
    if ((bytes == nullptr) || (available < SdtHeaderBytes) || !SignatureIs(bytes, signature, 4U))
    {
        return 0U;
    }
    const UInt64 declared = Read(bytes + 4U, 4U);
    if ((declared < minimum) || (declared > available) || !ChecksumIsZero(bytes, static_cast<Size>(declared)))
    {
        return 0U;
    }
    return static_cast<Size>(declared);
}

// Decodes the generic address of a UART: system memory or I/O space, no bit
// offset, byte access, and a stride the layout allows.
[[nodiscard]] bool DecodeAddress(const UInt8* const address, const RegisterLayout layout,
                                 const BootDebugPortSource source, const UInt32 baudRate, BootDebugPort& port) noexcept
{
    const UInt8 space = address[0U];
    const UInt8 bitWidth = address[1U];
    const UInt8 accessSize = address[3U];
    UInt8 stride{1U};
    switch (layout)
    {
    case RegisterLayout::Legacy:
        // DBGP and SPCR give COM1 an 8-bit width; older tables leave it zero.
        if ((bitWidth != 0U) && (bitWidth != 8U))
        {
            return false;
        }
        break;
    case RegisterLayout::GenericAddress:
        if ((bitWidth != 8U) && (bitWidth != 16U) && (bitWidth != 32U) && (bitWidth != 64U))
        {
            return false;
        }
        stride = static_cast<UInt8>(bitWidth / 8U);
        break;
    }
    if ((address[2U] != 0U) || ((accessSize != AccessSizeUndefined) && (accessSize != AccessSizeByte)))
    {
        return false;
    }
    const BootDebugPort candidate{
        .Address = Read(address + 4U, 8U),
        .BaudRate = baudRate,
        .AddressSpace = space,
        .AccessBytes = 1U,
        .RegisterStrideBytes = stride,
        .Source = static_cast<UInt8>(source),
    };
    if (!IsValidBootDebugPort(candidate))
    {
        return false;
    }
    port = candidate;
    return true;
}

[[nodiscard]] bool SubtypeLayout(const UInt16 subtype, RegisterLayout& layout) noexcept
{
    if ((subtype == SerialSubtype16550) || (subtype == SerialSubtype16550Subset))
    {
        layout = RegisterLayout::Legacy;
        return true;
    }
    if (subtype == SerialSubtype16550Generic)
    {
        layout = RegisterLayout::GenericAddress;
        return true;
    }
    return false;
}

// SPCR's configured baud rate enumeration; zero keeps the configuration.
[[nodiscard]] UInt32 SpcrBaudRate(const UInt8 code) noexcept
{
    switch (code)
    {
    case 3U:
        return 9'600U;
    case 4U:
        return 19'200U;
    case 6U:
        return 57'600U;
    case 7U:
        return 115'200U;
    default:
        return 0U;
    }
}

[[nodiscard]] UInt64 TableAddress(const void* const pointer) noexcept
{
    return static_cast<UInt64>(reinterpret_cast<UIntPointer>(pointer));
}

[[nodiscard]] const UInt8* Pointer(const UInt64 address) noexcept
{
    return reinterpret_cast<const UInt8*>(static_cast<UIntPointer>(address));
}

// A table at address with signature: validated through its header before
// any byte past the header is read.
[[nodiscard]] DebugPortTableView ResolveTable(const UInt64 address, const char* const signature, const Size minimum,
                                              const Size maximum) noexcept
{
    if ((address == 0U) || (address >= AddressLimit))
    {
        return {};
    }
    const UInt8* const table = Pointer(address);
    if (!SignatureIs(table, signature, 4U))
    {
        return {};
    }
    const UInt64 declared = Read(table + 4U, 4U);
    if ((declared < minimum) || (declared > maximum) || !ChecksumIsZero(table, static_cast<Size>(declared)))
    {
        return {};
    }
    return DebugPortTableView{table, static_cast<Size>(declared)};
}

// The first table of a kind the root table lists is the one used.
void Adopt(DebugPortTableView& view, const UInt64 address, const char* const signature, const Size minimum) noexcept
{
    if (view.Bytes == nullptr)
    {
        view = ResolveTable(address, signature, minimum, MaximumTableBytes);
    }
}
} // namespace

bool DecodeDbg2DebugPort(const UInt8* const bytes, const Size length, BootDebugPort& port) noexcept
{
    const Size tableLength = ValidatedLength(bytes, length, "DBG2", Dbg2FixedBytes);
    if (tableLength == 0U)
    {
        return false;
    }
    const UInt64 first = Read(bytes + Dbg2DeviceArrayOffset, 4U);
    const UInt64 count = Read(bytes + Dbg2DeviceCountOffset, 4U);
    if ((first < Dbg2FixedBytes) || (first > tableLength) || (count > MaximumDbg2Devices))
    {
        return false;
    }
    Size offset = static_cast<Size>(first);
    for (UInt64 index{0U}; index < count; ++index)
    {
        if ((tableLength - offset) < Dbg2DeviceFixedBytes)
        {
            return false;
        }
        const UInt8* const device = bytes + offset;
        const Size deviceLength = static_cast<Size>(Read(device + Dbg2DeviceLengthOffset, 2U));
        if ((deviceLength < Dbg2DeviceFixedBytes) || (deviceLength > (tableLength - offset)))
        {
            return false;
        }
        const Size addressOffset = static_cast<Size>(Read(device + Dbg2DeviceAddressOffset, 2U));
        RegisterLayout layout{RegisterLayout::Legacy};
        if ((Read(device + Dbg2DevicePortTypeOffset, 2U) == Dbg2SerialPortType) &&
            SubtypeLayout(static_cast<UInt16>(Read(device + Dbg2DevicePortSubtypeOffset, 2U)), layout) &&
            (device[Dbg2DeviceAddressCountOffset] != 0U) && (addressOffset >= Dbg2DeviceFixedBytes) &&
            (addressOffset <= (deviceLength - GenericAddressBytes)) &&
            DecodeAddress(device + addressOffset, layout, BootDebugPortSource::Dbg2, 0U, port))
        {
            return true;
        }
        offset += deviceLength;
    }
    return false;
}

bool DecodeDbgpDebugPort(const UInt8* const bytes, const Size length, BootDebugPort& port) noexcept
{
    if (ValidatedLength(bytes, length, "DBGP", DbgpFixedBytes) == 0U)
    {
        return false;
    }
    const UInt8 interfaceType = bytes[InterfaceTypeOffset];
    return ((interfaceType == 0U) || (interfaceType == 1U)) &&
           DecodeAddress(bytes + BaseAddressOffset, RegisterLayout::Legacy, BootDebugPortSource::Dbgp, 0U, port);
}

bool DecodeSpcrConsolePort(const UInt8* const bytes, const Size length, BootDebugPort& port) noexcept
{
    const Size tableLength = ValidatedLength(bytes, length, "SPCR", SpcrFixedBytes);
    if (tableLength == 0U)
    {
        return false;
    }
    const UInt8 revision = bytes[SpcrRevisionOffset];
    const UInt8 interfaceType = bytes[InterfaceTypeOffset];
    RegisterLayout layout{RegisterLayout::Legacy};
    // Revision 1: 0 is a 16550, 1 a 16450 (which also accepts FCR writes).
    // From revision 2 the interface type is a DBG2 serial subtype.
    if ((revision < 2U) ? (interfaceType > 1U) : !SubtypeLayout(interfaceType, layout))
    {
        return false;
    }
    UInt32 baudRate = SpcrBaudRate(bytes[SpcrBaudOffset]);
    if ((revision >= SpcrPreciseBaudRevision) && (tableLength >= (SpcrPreciseBaudOffset + 4U)))
    {
        const UInt32 precise = static_cast<UInt32>(Read(bytes + SpcrPreciseBaudOffset, 4U));
        baudRate = precise != 0U ? precise : baudRate;
    }
    return DecodeAddress(bytes + BaseAddressOffset, layout, BootDebugPortSource::Spcr, baudRate, port);
}

bool DecodeHcdpUart(const UInt8* const bytes, const Size length, const HcdpUartType type, BootDebugPort& port) noexcept
{
    const Size tableLength = ValidatedLength(bytes, length, "HCDP", HcdpFixedBytes);
    if (tableLength == 0U)
    {
        return false;
    }
    const UInt64 count = Read(bytes + HcdpUartCountOffset, 4U);
    if (count > ((tableLength - HcdpFixedBytes) / HcdpUartBytes))
    {
        return false;
    }
    const BootDebugPortSource source =
        type == HcdpUartType::Console ? BootDebugPortSource::HcdpConsole : BootDebugPortSource::HcdpDebugPort;
    for (UInt64 index{0U}; index < count; ++index)
    {
        const UInt8* const entry = bytes + HcdpFixedBytes + (static_cast<Size>(index) * HcdpUartBytes);
        if (entry[0U] != static_cast<UInt8>(type))
        {
            continue;
        }
        // HCDP 1.0a lets zero mean the device's default rate; a rate that
        // does not fit 32 bits is no rate a 16550 runs at.
        const UInt64 baud = Read(entry + HcdpUartBaudOffset, 8U);
        const UInt32 baudRate = baud <= 0xFFFF'FFFFU ? static_cast<UInt32>(baud) : 0U;
        // The first descriptor of a type is the primary device; later ones
        // are backups (HCDP 1.0a section 2 note).
        return DecodeAddress(entry + HcdpUartAddressOffset, RegisterLayout::Legacy, source, baudRate, port);
    }
    return false;
}

BootDebugPort SelectDebugPort(const DebugPortTableSet& tables, const BootDebugPort& fallback) noexcept
{
    BootDebugPort port{};
    if (DecodeDbg2DebugPort(tables.Dbg2.Bytes, tables.Dbg2.Length, port) ||
        DecodeDbgpDebugPort(tables.Dbgp.Bytes, tables.Dbgp.Length, port) ||
        DecodeHcdpUart(tables.Hcdp.Bytes, tables.Hcdp.Length, HcdpUartType::DebugPort, port) ||
        DecodeSpcrConsolePort(tables.Spcr.Bytes, tables.Spcr.Length, port) ||
        DecodeHcdpUart(tables.Hcdp.Bytes, tables.Hcdp.Length, HcdpUartType::Console, port))
    {
        return port;
    }
    return fallback;
}

DebugPortTableSet LocateDebugPortTables(const void* const rsdpPointer,
                                        const void* const hcdpConfigurationTable) noexcept
{
    DebugPortTableSet tables{};
    const UInt64 rsdpAddress = TableAddress(rsdpPointer);
    const UInt8* const rsdp = Pointer(rsdpAddress);
    if ((rsdpAddress != 0U) && (rsdpAddress < AddressLimit) && SignatureIs(rsdp, "RSD PTR ", 8U) &&
        ChecksumIsZero(rsdp, RsdpVersion1Bytes))
    {
        bool extended = rsdp[15U] >= 2U;
        if (extended)
        {
            const UInt64 rsdpLength = Read(rsdp + 20U, 4U);
            extended = (rsdpLength >= RsdpVersion2Bytes) && (rsdpLength <= MaximumTableBytes) &&
                       ChecksumIsZero(rsdp, static_cast<Size>(rsdpLength)) && (Read(rsdp + 24U, 8U) != 0U);
        }
        const Size entryBytes = extended ? 8U : 4U;
        const DebugPortTableView root =
            extended ? ResolveTable(Read(rsdp + 24U, 8U), "XSDT", SdtHeaderBytes, MaximumRootTableBytes)
                     : ResolveTable(Read(rsdp + 16U, 4U), "RSDT", SdtHeaderBytes, MaximumRootTableBytes);
        for (Size offset{SdtHeaderBytes}; (root.Bytes != nullptr) && ((offset + entryBytes) <= root.Length);
             offset += entryBytes)
        {
            const UInt64 address = Read(root.Bytes + offset, entryBytes);
            Adopt(tables.Dbg2, address, "DBG2", Dbg2FixedBytes);
            Adopt(tables.Dbgp, address, "DBGP", DbgpFixedBytes);
            Adopt(tables.Hcdp, address, "HCDP", HcdpFixedBytes);
            Adopt(tables.Spcr, address, "SPCR", SpcrFixedBytes);
        }
    }
    if (tables.Hcdp.Bytes == nullptr)
    {
        tables.Hcdp = ResolveTable(TableAddress(hcdpConfigurationTable), "HCDP", HcdpFixedBytes, MaximumTableBytes);
    }
    return tables;
}
} // namespace Asteria::Boot::Acpi
