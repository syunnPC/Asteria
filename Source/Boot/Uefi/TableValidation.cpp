// SPDX-License-Identifier: Apache-2.0

#include "Boot/Uefi/TableValidation.hpp"

namespace Asteria::Boot::Uefi
{
namespace
{
constexpr Foundation::UInt32 ReflectedCrc32Polynomial{0xEDB8'8320U};
constexpr Foundation::Size Crc32FieldOffset{16U};
constexpr Foundation::Size Crc32FieldSize{4U};

[[nodiscard]] Foundation::UInt32 UpdateCrc32(Foundation::UInt32 crc, const Foundation::UInt8 byte) noexcept
{
    crc ^= byte;
    for (Foundation::UInt32 bit{0U}; bit < 8U; ++bit)
    {
        const Foundation::UInt32 lowBitMask = static_cast<Foundation::UInt32>(0U - (crc & 1U));
        crc = (crc >> 1U) ^ (ReflectedCrc32Polynomial & lowBitMask);
    }
    return crc;
}
}

Foundation::UInt32 ComputeCrc32(const void* const data, const Foundation::Size byteCount) noexcept
{
    const auto* const bytes = static_cast<const Foundation::UInt8*>(data);
    Foundation::UInt32 crc{~Foundation::UInt32{0U}};
    for (Foundation::Size index{0U}; index < byteCount; ++index)
    {
        crc = UpdateCrc32(crc, bytes[index]);
    }
    return ~crc;
}

bool IsTableHeaderValid(const TableHeader& header, const Foundation::UInt64 expectedSignature,
                        const Foundation::Size minimumSize) noexcept
{
    return (header.Signature == expectedSignature) && (header.Revision >= MinimumSupportedTableRevision) &&
           (header.HeaderSize >= sizeof(TableHeader)) && (header.HeaderSize >= minimumSize) &&
           (header.HeaderSize <= MaximumTableHeaderBytes) && (header.Reserved == 0U);
}

bool IsTableCrcValid(const void* const table, const TableHeader& header) noexcept
{
    if ((table == nullptr) || (header.HeaderSize < sizeof(TableHeader)) ||
        (header.HeaderSize > MaximumTableHeaderBytes))
    {
        return false;
    }

    const auto* const bytes = static_cast<const Foundation::UInt8*>(table);
    Foundation::UInt32 crc{~Foundation::UInt32{0U}};
    for (Foundation::Size index{0U}; index < header.HeaderSize; ++index)
    {
        const bool isCrcByte = (index >= Crc32FieldOffset) && (index < (Crc32FieldOffset + Crc32FieldSize));
        crc = UpdateCrc32(crc, isCrcByte ? Foundation::UInt8{0U} : bytes[index]);
    }
    return (~crc) == header.Crc32;
}

bool IsZeroInitializedProbe(const volatile Foundation::UInt64& probe) noexcept
{
    return probe == 0U;
}

void ClearExpiredBootServices(SystemTable& table) noexcept
{
    table.BootServicesTable = nullptr;
    table.ConsoleInHandle = nullptr;
    table.ConsoleIn = nullptr;
    table.ConsoleOutHandle = nullptr;
    table.ConsoleOut = nullptr;
    table.StandardErrorHandle = nullptr;
    table.StandardError = nullptr;
    table.Header.Crc32 = 0U;
    table.Header.Crc32 = ComputeCrc32(&table, table.Header.HeaderSize);
}
} // namespace Asteria::Boot::Uefi
