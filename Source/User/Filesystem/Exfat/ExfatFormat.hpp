// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_EXFAT_EXFAT_FORMAT_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_EXFAT_EXFAT_FORMAT_HPP_INCLUDED

#include "Foundation/Types.hpp"

/// The on-disk encoding the two halves of the exFAT library share (the exFAT
/// specification's boot sector, FAT and directory entries): offsets, entry
/// types, the little-endian field accessors and the rotate-add sums. Only
/// User/Filesystem/Exfat includes it.
namespace Asteria::User::Filesystem::ExfatFormat
{
inline constexpr Foundation::UInt32 NoEntry{~Foundation::UInt32{0U}};
inline constexpr Foundation::UInt64 NoSector{~Foundation::UInt64{0U}};

/// The main boot sector's VolumeFlags byte (outside the boot checksum).
inline constexpr Foundation::Size VolumeFlagsOffset{106U};
inline constexpr Foundation::UInt32 VolumeDirty{0x02U};
inline constexpr Foundation::UInt32 MediaFailure{0x04U};

/// The FAT entry that ends a chain.
inline constexpr Foundation::UInt32 EndOfChain{0xFFFF'FFFFU};

/// A directory entry's type byte: 0x00 ends the directory, a clear InUse bit
/// Marks a deleted entry, TypeCategory a secondary one, TypeImportance a benign one.
inline constexpr Foundation::UInt32 EndOfDirectory{0x00U};
inline constexpr Foundation::UInt32 InUse{0x80U};
inline constexpr Foundation::UInt32 SecondaryCategory{0x40U};
inline constexpr Foundation::UInt32 BenignImportance{0x20U};
inline constexpr Foundation::UInt32 AllocationBitmapEntry{0x81U};
inline constexpr Foundation::UInt32 FileEntry{0x85U};
inline constexpr Foundation::UInt32 StreamExtensionEntry{0xC0U};
inline constexpr Foundation::UInt32 FileNameEntry{0xC1U};
/// A File entry with its InUse bit clear: what a free entry before a created set becomes.
inline constexpr Foundation::UInt32 DeletedFileEntry{0x05U};
inline constexpr Foundation::UInt32 EntryBytes{32U};
inline constexpr Foundation::UInt32 EntriesPerSector{16U};
inline constexpr Foundation::UInt32 NameCharactersPerEntry{15U};
/// A File entry's SecondaryCount bounds (a Stream Extension and 1 to 17 File
/// Name entries).
inline constexpr Foundation::UInt32 MinimumSecondaries{2U};
inline constexpr Foundation::UInt32 MaximumSecondaries{18U};
inline constexpr Foundation::UInt32 DirectoryAttribute{0x10U};
inline constexpr Foundation::UInt32 ArchiveAttribute{0x20U};
/// The Stream Extension's GeneralSecondaryFlags.
inline constexpr Foundation::UInt32 AllocationPossible{0x01U};
inline constexpr Foundation::UInt32 NoFatChain{0x02U};

[[nodiscard]] inline Foundation::UInt32 Load16(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt32>(bytes[0U]) | (static_cast<Foundation::UInt32>(bytes[1U]) << 8U);
}

[[nodiscard]] inline Foundation::UInt32 Load32(const Foundation::UInt8* const bytes) noexcept
{
    return Load16(bytes) | (Load16(bytes + 2U) << 16U);
}

[[nodiscard]] inline Foundation::UInt64 Load64(const Foundation::UInt8* const bytes) noexcept
{
    return static_cast<Foundation::UInt64>(Load32(bytes)) |
           (static_cast<Foundation::UInt64>(Load32(bytes + 4U)) << 32U);
}

inline void Store16(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    bytes[0U] = static_cast<Foundation::UInt8>(value);
    bytes[1U] = static_cast<Foundation::UInt8>(value >> 8U);
}

inline void Store32(Foundation::UInt8* const bytes, const Foundation::UInt32 value) noexcept
{
    Store16(bytes, value);
    Store16(bytes + 2U, value >> 16U);
}

inline void Store64(Foundation::UInt8* const bytes, const Foundation::UInt64 value) noexcept
{
    Store32(bytes, static_cast<Foundation::UInt32>(value));
    Store32(bytes + 4U, static_cast<Foundation::UInt32>(value >> 32U));
}

/// One step of the 16-bit rotate-right-and-add sum of NameHash and
/// SetChecksum.
[[nodiscard]] inline Foundation::UInt32 Rotate16(const Foundation::UInt32 sum, const Foundation::UInt32 byte) noexcept
{
    return ((((sum & 1U) << 15U) | (sum >> 1U)) + byte) & 0xFFFFU;
}

/// ASCII upper case: what every volume's up-case table maps 'a' to 'z' to
///; other values are returned unchanged.
[[nodiscard]] inline Foundation::UInt32 ToUpper(const Foundation::UInt32 value) noexcept
{
    return ((value >= 'a') && (value <= 'z')) ? (value - 32U) : value;
}

/// The NameHash step for one UTF-16 code unit of a name, upper-cased.
[[nodiscard]] inline Foundation::UInt32 HashCharacter(const Foundation::UInt32 hash,
                                                      const Foundation::UInt32 value) noexcept
{
    const Foundation::UInt32 upper = ToUpper(value);
    return Rotate16(Rotate16(hash, upper & 0xFFU), (upper >> 8U) & 0xFFU);
}
} // Namespace Asteria::User::Filesystem::ExfatFormat

#endif // ASTERIA_USER_FILESYSTEM_EXFAT_EXFAT_FORMAT_HPP_INCLUDED
