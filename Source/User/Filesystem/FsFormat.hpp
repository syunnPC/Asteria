// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_FS_FORMAT_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_FS_FORMAT_HPP_INCLUDED

#include "Foundation/Types.hpp"
#include "User/Filesystem/Crc32c.hpp"

namespace Asteria::User::Filesystem
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;

inline constexpr UInt32 FsVersion{0U};
inline constexpr UInt32 FsSectorBytes{512U};
inline constexpr UInt32 FsBlockBytes{4096U};
inline constexpr UInt32 FsSectorsPerBlock{8U};
inline constexpr UInt32 FsTotalBlocks{512U};
inline constexpr UInt32 FsTotalSectors{4096U};
inline constexpr UInt32 FsFirstCowBlock{16U};
inline constexpr UInt32 FsReservedSectors{128U};
inline constexpr UInt64 FsSuperblockA{0U};
inline constexpr UInt64 FsSuperblockB{16U};
inline constexpr UInt64 FsBootTokenSector{8U};
inline constexpr UInt64 FsScratchSector{64U};
inline constexpr Size FsBitmapBytes{64U};
inline constexpr Size FsCommitCapacity{4U};
inline constexpr Size FsDirectCapacity{6U};
inline constexpr Size FsNameCapacity{64U};
inline constexpr Size FsObjectCapacity{127U};
inline constexpr Size FsDirectoryCapacity{50U};

/// Byte offsets, independent of the native layout of the logical structures.
struct FsCommitLayout
{
    static constexpr Size Sender{0U};
    static constexpr Size Transaction{8U};
    static constexpr Size Bytes{16U};
};
struct FsSuperblockLayout
{
    static constexpr Size Magic{0U};
    static constexpr Size Version{8U};
    static constexpr Size BlockBytes{12U};
    static constexpr Size TotalBlocks{16U};
    static constexpr Size VolumeGeneration{20U};
    static constexpr Size TableBlock{28U};
    static constexpr Size TableCrc{32U};
    static constexpr Size MapBlock{36U}; // v0 requires zero
    static constexpr Size NextObjectId{40U};
    static constexpr Size Committed{48U}; // four {u64 Sender, u64 Transaction}
    static constexpr Size Bitmap{112U};
    static constexpr Size CommitGenerations{176U}; // generations of the four records
    static constexpr Size Reserved{208U};          // all the following fields are zero in v0
    static constexpr Size SnapshotRoot{208U};
    static constexpr Size Quota{216U};
    static constexpr Size Features{224U};
    static constexpr Size Uuid{232U};
    static constexpr Size ReservedWords{248U};
    static constexpr Size Crc{508U};
    static constexpr Size Bytes{512U};
};
struct FsMetadataLayout
{
    static constexpr Size Magic{0U};
    static constexpr Size Kind{8U};
    static constexpr Size VolumeGeneration{12U};
    static constexpr Size Crc{20U};
    static constexpr Size HeaderBytes{24U};
    static constexpr Size Entries{32U}; // eight reserved zero bytes after the header
    static constexpr Size Bytes{4096U};
};
struct FsObjectLayout
{
    static constexpr Size BirthGeneration{0U};
    static constexpr Size SizeBytes{8U};
    static constexpr Size Kind{16U};
    static constexpr Size Direct{20U}; // six u16 block numbers; zero = hole
    static constexpr Size Bytes{32U};
};
struct FsDirectoryLayout
{
    static constexpr Size NameLength{0U}; // u32
    static constexpr Size Name{4U};       // 64 bytes, unused tail zero
    static constexpr Size Reserved{68U};  // four zero bytes
    static constexpr Size ObjectId{72U};  // u64
    static constexpr Size Bytes{80U};
};

static_assert(FsSectorBytes * FsSectorsPerBlock == FsBlockBytes);
static_assert(FsTotalBlocks * FsSectorsPerBlock == FsTotalSectors);
static_assert(FsBitmapBytes * 8U == FsTotalBlocks);
static_assert(FsReservedSectors == FsFirstCowBlock * FsSectorsPerBlock);
static_assert(FsSuperblockA < FsReservedSectors);
static_assert(FsSuperblockB < FsReservedSectors);
static_assert(FsBootTokenSector < FsReservedSectors);
static_assert(FsScratchSector < FsReservedSectors);
static_assert(FsCommitLayout::Sender == 0U);
static_assert(FsCommitLayout::Transaction == FsCommitLayout::Sender + 8U);
static_assert(FsCommitLayout::Bytes == FsCommitLayout::Transaction + 8U);
static_assert(FsSuperblockLayout::Magic == 0U);
static_assert(FsSuperblockLayout::Version == FsSuperblockLayout::Magic + 8U);
static_assert(FsSuperblockLayout::BlockBytes == FsSuperblockLayout::Version + 4U);
static_assert(FsSuperblockLayout::TotalBlocks == FsSuperblockLayout::BlockBytes + 4U);
static_assert(FsSuperblockLayout::VolumeGeneration == FsSuperblockLayout::TotalBlocks + 4U);
static_assert(FsSuperblockLayout::TableBlock == FsSuperblockLayout::VolumeGeneration + 8U);
static_assert(FsSuperblockLayout::TableCrc == FsSuperblockLayout::TableBlock + 4U);
static_assert(FsSuperblockLayout::MapBlock == FsSuperblockLayout::TableCrc + 4U);
static_assert(FsSuperblockLayout::NextObjectId == FsSuperblockLayout::MapBlock + 4U);
static_assert(FsSuperblockLayout::Committed == FsSuperblockLayout::NextObjectId + 8U);
static_assert(FsSuperblockLayout::Bitmap == FsSuperblockLayout::Committed + FsCommitCapacity * FsCommitLayout::Bytes);
static_assert(FsSuperblockLayout::CommitGenerations == FsSuperblockLayout::Bitmap + FsBitmapBytes);
static_assert(FsSuperblockLayout::Reserved == FsSuperblockLayout::CommitGenerations + FsCommitCapacity * 8U);
static_assert(FsSuperblockLayout::SnapshotRoot == FsSuperblockLayout::Reserved);
static_assert(FsSuperblockLayout::Quota == FsSuperblockLayout::SnapshotRoot + 8U);
static_assert(FsSuperblockLayout::Features == FsSuperblockLayout::Quota + 8U);
static_assert(FsSuperblockLayout::Uuid == FsSuperblockLayout::Features + 8U);
static_assert(FsSuperblockLayout::ReservedWords == FsSuperblockLayout::Uuid + 16U);
static_assert(FsSuperblockLayout::Crc == FsSuperblockLayout::ReservedWords + 260U);
static_assert(FsSuperblockLayout::Crc == FsSuperblockLayout::Reserved + 300U);
static_assert(FsSuperblockLayout::Bytes == FsSuperblockLayout::Crc + 4U);
static_assert(FsSuperblockLayout::Bytes == FsSectorBytes);
static_assert(FsMetadataLayout::Magic == 0U);
static_assert(FsMetadataLayout::Kind == FsMetadataLayout::Magic + 8U);
static_assert(FsMetadataLayout::VolumeGeneration == FsMetadataLayout::Kind + 4U);
static_assert(FsMetadataLayout::Crc == FsMetadataLayout::VolumeGeneration + 8U);
static_assert(FsMetadataLayout::HeaderBytes == FsMetadataLayout::Crc + 4U);
static_assert(FsMetadataLayout::Entries == FsMetadataLayout::HeaderBytes + 8U);
static_assert(FsMetadataLayout::Bytes == FsBlockBytes);
static_assert(FsObjectLayout::BirthGeneration == 0U);
static_assert(FsObjectLayout::SizeBytes == FsObjectLayout::BirthGeneration + 8U);
static_assert(FsObjectLayout::Kind == FsObjectLayout::SizeBytes + 8U);
static_assert(FsObjectLayout::Direct == FsObjectLayout::Kind + 4U);
static_assert(FsObjectLayout::Bytes == FsObjectLayout::Direct + FsDirectCapacity * 2U);
static_assert(FsMetadataLayout::Entries + FsObjectCapacity * FsObjectLayout::Bytes == FsBlockBytes);
static_assert(FsDirectoryLayout::NameLength == 0U);
static_assert(FsDirectoryLayout::Name == FsDirectoryLayout::NameLength + 4U);
static_assert(FsDirectoryLayout::Reserved == FsDirectoryLayout::Name + FsNameCapacity);
static_assert(FsDirectoryLayout::ObjectId == FsDirectoryLayout::Reserved + 4U);
static_assert(FsDirectoryLayout::Bytes == FsDirectoryLayout::ObjectId + 8U);
static_assert(FsMetadataLayout::Entries + FsDirectoryCapacity * FsDirectoryLayout::Bytes + 64U == FsBlockBytes);

enum class FsObjectKind : UInt32
{
    Empty = 0U,
    File = 1U,
    Directory = 2U,
};
enum class FsMetadataKind : UInt32
{
    ObjectTable = 1U,
    Directory = 2U,
};
struct FsCommitRecord
{
    UInt64 Sender{0U};
    UInt64 Transaction{0U};
};
struct FsSuperblock
{
    UInt64 VolumeGeneration{0U};
    UInt32 TableBlock{0U};
    UInt32 TableCrc{0U};
    UInt64 NextObjectId{0U};
    FsCommitRecord Committed[FsCommitCapacity]{};
    UInt8 Bitmap[FsBitmapBytes]{};
    UInt64 CommitGenerations[FsCommitCapacity]{};
};
struct FsObject
{
    UInt64 BirthGeneration{0U};
    UInt64 SizeBytes{0U};
    FsObjectKind Kind{FsObjectKind::Empty};
    UInt16 Direct[FsDirectCapacity]{};
};
struct FsDirectoryEntry
{
    UInt32 NameLength{0U};
    UInt8 Name[FsNameCapacity]{};
    UInt64 ObjectId{0U};
};

/// All functions are allocation-free. Exact buffer sizes are required; failed
/// decoders leave outputs unchanged. Logical structures are never overlaid on
/// bytes. Reserved bytes must be zero (no unknown v0 features are accepted).
class FsFormat final
{
public:
    [[nodiscard]] static UInt64 ReadLittle(const UInt8* bytes, Size width) noexcept;
    static void WriteLittle(UInt8* bytes, Size width, UInt64 value) noexcept;
    [[nodiscard]] static bool IsZero(const UInt8* bytes, Size size) noexcept;
    [[nodiscard]] static bool IsCowBlock(UInt64 block) noexcept;
    [[nodiscard]] static bool ValidName(const UInt8* bytes, Size size) noexcept;
    [[nodiscard]] static bool BitmapHas(const UInt8* bitmap, UInt32 block) noexcept;
    static void BitmapSet(UInt8* bitmap, UInt32 block) noexcept;

    /// CRC over LE u64 location, LE u64 generation, then every encoded byte,
    /// with the four checksum bytes logically zero. Superblocks use a sector
    /// location; metadata uses a block location. No temporary buffer is needed.
    [[nodiscard]] static UInt32 Checksum(const UInt8* bytes, Size size, Size crcOffset, UInt64 location,
                                         UInt64 generation) noexcept;
    [[nodiscard]] static bool EncodeSuperblock(const FsSuperblock& value, UInt64 sector, UInt8* bytes,
                                               Size size) noexcept;
    [[nodiscard]] static bool DecodeSuperblock(const UInt8* bytes, Size size, UInt64 sector,
                                               FsSuperblock& output) noexcept;
    [[nodiscard]] static bool EncodeObjectTable(const FsObject* objects, UInt32 block, UInt64 generation, UInt8* bytes,
                                                Size size) noexcept;
    [[nodiscard]] static bool DecodeObjectTable(const UInt8* bytes, Size size, UInt32 block, UInt64 generation,
                                                FsObject* output) noexcept;
    [[nodiscard]] static bool EncodeDirectory(const FsDirectoryEntry* entries, UInt32 block, UInt64 generation,
                                              UInt8* bytes, Size size) noexcept;
    [[nodiscard]] static bool DecodeDirectory(const UInt8* bytes, Size size, UInt32 block, UInt64 generation,
                                              FsDirectoryEntry* output) noexcept;
    [[nodiscard]] static bool DecodeMetadataHeader(const UInt8* bytes, Size size, UInt32 block, UInt64 generation,
                                                   FsMetadataKind kind) noexcept;

private:
    [[nodiscard]] static bool ValidSuperblock(const FsSuperblock& value) noexcept;
    [[nodiscard]] static bool ValidObject(const FsObject& value) noexcept;
    [[nodiscard]] static bool ValidDirectoryEntry(const FsDirectoryEntry& value) noexcept;
    static void ReadObject(const UInt8* bytes, FsObject& output) noexcept;
    static void ReadDirectoryEntry(const UInt8* bytes, FsDirectoryEntry& output) noexcept;
    static void WriteMetadataHeader(UInt8* bytes, FsMetadataKind kind, UInt64 generation) noexcept;
};
} // namespace Asteria::User::Filesystem

#endif // ASTERIA_USER_FILESYSTEM_FS_FORMAT_HPP_INCLUDED
