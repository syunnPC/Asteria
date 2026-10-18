// SPDX-License-Identifier: Apache-2.0

#include "User/Filesystem/FsFormat.hpp"
#include "User/Filesystem/Names.hpp"

namespace Asteria::User::Filesystem
{
namespace
{
constexpr UInt8 SuperblockMagic[8U]{'A', 'S', 'T', 'R', 'F', 'S', '0', '0'};
constexpr UInt8 MetadataMagic[8U]{'A', 'S', 'T', 'R', 'M', 'E', 'T', '0'};

[[nodiscard]] bool MagicMatches(const UInt8* bytes, const UInt8* magic) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        if (bytes[index] != magic[index])
        {
            return false;
        }
    }
    return true;
}
} // namespace

UInt64 FsFormat::ReadLittle(const UInt8* bytes, const Size width) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < width; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void FsFormat::WriteLittle(UInt8* bytes, const Size width, const UInt64 value) noexcept
{
    for (Size index{0U}; index < width; ++index)
    {
        bytes[index] = static_cast<UInt8>((value >> (index * 8U)) & 0xFFU);
    }
}

bool FsFormat::IsZero(const UInt8* bytes, const Size size) noexcept
{
    for (Size index{0U}; index < size; ++index)
    {
        if (bytes[index] != 0U)
        {
            return false;
        }
    }
    return true;
}

bool FsFormat::IsCowBlock(const UInt64 block) noexcept
{
    return (block >= FsFirstCowBlock) && (block < FsTotalBlocks);
}

bool FsFormat::ValidName(const UInt8* bytes, const Size size) noexcept
{
    return size <= FsNameCapacity && Filesystem::ValidName(bytes, size) && !(size == 1U && bytes[0U] == '.') &&
           !(size == 2U && bytes[0U] == '.' && bytes[1U] == '.');
}

bool FsFormat::BitmapHas(const UInt8* bitmap, const UInt32 block) noexcept
{
    return (block < FsTotalBlocks) && ((bitmap[block / 8U] & (1U << (block % 8U))) != 0U);
}

void FsFormat::BitmapSet(UInt8* bitmap, const UInt32 block) noexcept
{
    if (block < FsTotalBlocks)
    {
        bitmap[block / 8U] |= static_cast<UInt8>(1U << (block % 8U));
    }
}

UInt32 FsFormat::Checksum(const UInt8* bytes, const Size size, const Size crcOffset, const UInt64 location,
                          const UInt64 generation) noexcept
{
    UInt32 state = Crc32c::Start();
    for (Size index{0U}; index < 8U; ++index)
    {
        state = Crc32c::Update(state, static_cast<UInt8>((location >> (index * 8U)) & 0xFFU));
    }
    for (Size index{0U}; index < 8U; ++index)
    {
        state = Crc32c::Update(state, static_cast<UInt8>((generation >> (index * 8U)) & 0xFFU));
    }
    for (Size index{0U}; index < size; ++index)
    {
        state = Crc32c::Update(state, ((index >= crcOffset) && ((index - crcOffset) < 4U)) ? 0U : bytes[index]);
    }
    return Crc32c::Finish(state);
}

bool FsFormat::ValidSuperblock(const FsSuperblock& value) noexcept
{
    if ((value.VolumeGeneration == 0U) || !IsCowBlock(value.TableBlock) || (value.NextObjectId == 0U) ||
        (value.NextObjectId > FsObjectCapacity))
    {
        return false;
    }
    for (Size index{0U}; index < FsCommitCapacity; ++index)
    {
        const FsCommitRecord& record = value.Committed[index];
        const UInt64 generation = value.CommitGenerations[index];
        if ((generation > value.VolumeGeneration) ||
            ((generation == 0U) && ((record.Sender != 0U) || (record.Transaction != 0U))))
        {
            return false;
        }
        if ((index != 0U) && (generation != 0U) && (generation >= value.CommitGenerations[index - 1U]))
        {
            return false;
        }
    }
    return true;
}

bool FsFormat::EncodeSuperblock(const FsSuperblock& value, const UInt64 sector, UInt8* bytes, const Size size) noexcept
{
    if ((bytes == nullptr) || (size != FsSectorBytes) || ((sector != FsSuperblockA) && (sector != FsSuperblockB)) ||
        !ValidSuperblock(value))
    {
        return false;
    }
    for (Size index{0U}; index < size; ++index)
    {
        bytes[index] = 0U;
    }
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = SuperblockMagic[index];
    }
    WriteLittle(bytes + FsSuperblockLayout::Version, 4U, FsVersion);
    WriteLittle(bytes + FsSuperblockLayout::BlockBytes, 4U, FsBlockBytes);
    WriteLittle(bytes + FsSuperblockLayout::TotalBlocks, 4U, FsTotalBlocks);
    WriteLittle(bytes + FsSuperblockLayout::TableBlock, 4U, value.TableBlock);
    WriteLittle(bytes + FsSuperblockLayout::VolumeGeneration, 8U, value.VolumeGeneration);
    WriteLittle(bytes + FsSuperblockLayout::TableCrc, 4U, value.TableCrc);
    WriteLittle(bytes + FsSuperblockLayout::NextObjectId, 8U, value.NextObjectId);
    for (Size index{0U}; index < FsCommitCapacity; ++index)
    {
        WriteLittle(bytes + FsSuperblockLayout::Committed + index * FsCommitLayout::Bytes + FsCommitLayout::Sender, 8U,
                    value.Committed[index].Sender);
        WriteLittle(bytes + FsSuperblockLayout::Committed + index * FsCommitLayout::Bytes + FsCommitLayout::Transaction,
                    8U, value.Committed[index].Transaction);
        WriteLittle(bytes + FsSuperblockLayout::CommitGenerations + index * 8U, 8U, value.CommitGenerations[index]);
    }
    for (Size index{0U}; index < FsBitmapBytes; ++index)
    {
        bytes[FsSuperblockLayout::Bitmap + index] = value.Bitmap[index];
    }
    WriteLittle(bytes + FsSuperblockLayout::Crc, 4U,
                Checksum(bytes, size, FsSuperblockLayout::Crc, sector, value.VolumeGeneration));
    return true;
}

bool FsFormat::DecodeSuperblock(const UInt8* bytes, const Size size, const UInt64 sector, FsSuperblock& output) noexcept
{
    if ((bytes == nullptr) || (size != FsSectorBytes) || ((sector != FsSuperblockA) && (sector != FsSuperblockB)) ||
        !MagicMatches(bytes, SuperblockMagic) || (ReadLittle(bytes + FsSuperblockLayout::Version, 4U) != FsVersion) ||
        (ReadLittle(bytes + FsSuperblockLayout::BlockBytes, 4U) != FsBlockBytes) ||
        (ReadLittle(bytes + FsSuperblockLayout::TotalBlocks, 4U) != FsTotalBlocks) ||
        !IsZero(bytes + FsSuperblockLayout::MapBlock, 4U) || !IsZero(bytes + FsSuperblockLayout::Reserved, 300U))
    {
        return false;
    }
    const UInt64 generation = ReadLittle(bytes + FsSuperblockLayout::VolumeGeneration, 8U);
    if (ReadLittle(bytes + FsSuperblockLayout::Crc, 4U) !=
        Checksum(bytes, size, FsSuperblockLayout::Crc, sector, generation))
    {
        return false;
    }
    FsSuperblock candidate{};
    candidate.VolumeGeneration = generation;
    candidate.TableBlock = static_cast<UInt32>(ReadLittle(bytes + FsSuperblockLayout::TableBlock, 4U));
    candidate.TableCrc = static_cast<UInt32>(ReadLittle(bytes + FsSuperblockLayout::TableCrc, 4U));
    candidate.NextObjectId = ReadLittle(bytes + FsSuperblockLayout::NextObjectId, 8U);
    for (Size index{0U}; index < FsCommitCapacity; ++index)
    {
        candidate.Committed[index].Sender = ReadLittle(
            bytes + FsSuperblockLayout::Committed + index * FsCommitLayout::Bytes + FsCommitLayout::Sender, 8U);
        candidate.Committed[index].Transaction = ReadLittle(
            bytes + FsSuperblockLayout::Committed + index * FsCommitLayout::Bytes + FsCommitLayout::Transaction, 8U);
        candidate.CommitGenerations[index] = ReadLittle(bytes + FsSuperblockLayout::CommitGenerations + index * 8U, 8U);
    }
    for (Size index{0U}; index < FsBitmapBytes; ++index)
    {
        candidate.Bitmap[index] = bytes[FsSuperblockLayout::Bitmap + index];
    }
    if (!ValidSuperblock(candidate))
    {
        return false;
    }
    output = candidate;
    return true;
}

void FsFormat::WriteMetadataHeader(UInt8* bytes, const FsMetadataKind kind, const UInt64 generation) noexcept
{
    for (Size index{0U}; index < FsBlockBytes; ++index)
    {
        bytes[index] = 0U;
    }
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = MetadataMagic[index];
    }
    WriteLittle(bytes + FsMetadataLayout::Kind, 4U, static_cast<UInt32>(kind));
    WriteLittle(bytes + FsMetadataLayout::VolumeGeneration, 8U, generation);
}

bool FsFormat::DecodeMetadataHeader(const UInt8* bytes, const Size size, const UInt32 block, const UInt64 generation,
                                    const FsMetadataKind kind) noexcept
{
    return (bytes != nullptr) && (size == FsBlockBytes) && IsCowBlock(block) && (generation != 0U) &&
           ((kind == FsMetadataKind::ObjectTable) || (kind == FsMetadataKind::Directory)) &&
           MagicMatches(bytes, MetadataMagic) &&
           (ReadLittle(bytes + FsMetadataLayout::Kind, 4U) == static_cast<UInt32>(kind)) &&
           (ReadLittle(bytes + FsMetadataLayout::VolumeGeneration, 8U) == generation) &&
           IsZero(bytes + FsMetadataLayout::HeaderBytes, 8U) &&
           (ReadLittle(bytes + FsMetadataLayout::Crc, 4U) ==
            Checksum(bytes, size, FsMetadataLayout::Crc, block, generation));
}

bool FsFormat::ValidObject(const FsObject& value) noexcept
{
    if (value.Kind == FsObjectKind::Empty)
    {
        if ((value.BirthGeneration != 0U) || (value.SizeBytes != 0U))
        {
            return false;
        }
        for (const UInt16 block : value.Direct)
        {
            if (block != 0U)
            {
                return false;
            }
        }
        return true;
    }
    if (((value.Kind != FsObjectKind::File) && (value.Kind != FsObjectKind::Directory)) ||
        (value.BirthGeneration == 0U) || (value.SizeBytes > FsDirectCapacity * FsBlockBytes))
    {
        return false;
    }
    for (const UInt16 block : value.Direct)
    {
        if ((block != 0U) && !IsCowBlock(block))
        {
            return false;
        }
    }
    return true;
}

void FsFormat::ReadObject(const UInt8* bytes, FsObject& output) noexcept
{
    output.BirthGeneration = ReadLittle(bytes + FsObjectLayout::BirthGeneration, 8U);
    output.SizeBytes = ReadLittle(bytes + FsObjectLayout::SizeBytes, 8U);
    output.Kind = static_cast<FsObjectKind>(ReadLittle(bytes + FsObjectLayout::Kind, 4U));
    for (Size index{0U}; index < FsDirectCapacity; ++index)
    {
        output.Direct[index] = static_cast<UInt16>(ReadLittle(bytes + FsObjectLayout::Direct + index * 2U, 2U));
    }
}

bool FsFormat::EncodeObjectTable(const FsObject* objects, const UInt32 block, const UInt64 generation, UInt8* bytes,
                                 const Size size) noexcept
{
    if ((objects == nullptr) || (bytes == nullptr) || (size != FsBlockBytes) || !IsCowBlock(block) ||
        (generation == 0U))
    {
        return false;
    }
    for (Size index{0U}; index < FsObjectCapacity; ++index)
    {
        if (!ValidObject(objects[index]))
        {
            return false;
        }
    }
    WriteMetadataHeader(bytes, FsMetadataKind::ObjectTable, generation);
    for (Size index{0U}; index < FsObjectCapacity; ++index)
    {
        const FsObject& object = objects[index];
        UInt8* entry = bytes + FsMetadataLayout::Entries + index * FsObjectLayout::Bytes;
        WriteLittle(entry + FsObjectLayout::BirthGeneration, 8U, object.BirthGeneration);
        WriteLittle(entry + FsObjectLayout::SizeBytes, 8U, object.SizeBytes);
        WriteLittle(entry + FsObjectLayout::Kind, 4U, static_cast<UInt32>(object.Kind));
        for (Size direct{0U}; direct < FsDirectCapacity; ++direct)
        {
            WriteLittle(entry + FsObjectLayout::Direct + direct * 2U, 2U, object.Direct[direct]);
        }
    }
    WriteLittle(bytes + FsMetadataLayout::Crc, 4U, Checksum(bytes, size, FsMetadataLayout::Crc, block, generation));
    return true;
}

bool FsFormat::DecodeObjectTable(const UInt8* bytes, const Size size, const UInt32 block, const UInt64 generation,
                                 FsObject* output) noexcept
{
    if ((output == nullptr) || !DecodeMetadataHeader(bytes, size, block, generation, FsMetadataKind::ObjectTable))
    {
        return false;
    }
    FsObject candidate{};
    for (Size index{0U}; index < FsObjectCapacity; ++index)
    {
        const UInt8* entry = bytes + FsMetadataLayout::Entries + index * FsObjectLayout::Bytes;
        const UInt64 kind = ReadLittle(entry + FsObjectLayout::Kind, 4U);
        if (kind > static_cast<UInt32>(FsObjectKind::Directory))
        {
            return false;
        }
        ReadObject(entry, candidate);
        if (!ValidObject(candidate))
        {
            return false;
        }
    }
    for (Size index{0U}; index < FsObjectCapacity; ++index)
    {
        ReadObject(bytes + FsMetadataLayout::Entries + index * FsObjectLayout::Bytes, output[index]);
    }
    return true;
}

bool FsFormat::ValidDirectoryEntry(const FsDirectoryEntry& value) noexcept
{
    if (value.NameLength == 0U)
    {
        return (value.ObjectId == 0U) && IsZero(value.Name, FsNameCapacity);
    }
    return ValidName(value.Name, value.NameLength) && (value.ObjectId != 0U) && (value.ObjectId < FsObjectCapacity) &&
           IsZero(value.Name + value.NameLength, FsNameCapacity - value.NameLength);
}

void FsFormat::ReadDirectoryEntry(const UInt8* bytes, FsDirectoryEntry& output) noexcept
{
    output.NameLength = static_cast<UInt32>(ReadLittle(bytes + FsDirectoryLayout::NameLength, 4U));
    for (Size index{0U}; index < FsNameCapacity; ++index)
    {
        output.Name[index] = bytes[FsDirectoryLayout::Name + index];
    }
    output.ObjectId = ReadLittle(bytes + FsDirectoryLayout::ObjectId, 8U);
}

bool FsFormat::EncodeDirectory(const FsDirectoryEntry* entries, const UInt32 block, const UInt64 generation,
                               UInt8* bytes, const Size size) noexcept
{
    if ((entries == nullptr) || (bytes == nullptr) || (size != FsBlockBytes) || !IsCowBlock(block) ||
        (generation == 0U))
    {
        return false;
    }
    for (Size index{0U}; index < FsDirectoryCapacity; ++index)
    {
        if (!ValidDirectoryEntry(entries[index]))
        {
            return false;
        }
    }
    WriteMetadataHeader(bytes, FsMetadataKind::Directory, generation);
    for (Size index{0U}; index < FsDirectoryCapacity; ++index)
    {
        UInt8* entry = bytes + FsMetadataLayout::Entries + index * FsDirectoryLayout::Bytes;
        WriteLittle(entry + FsDirectoryLayout::NameLength, 4U, entries[index].NameLength);
        for (Size name{0U}; name < FsNameCapacity; ++name)
        {
            entry[FsDirectoryLayout::Name + name] = entries[index].Name[name];
        }
        WriteLittle(entry + FsDirectoryLayout::ObjectId, 8U, entries[index].ObjectId);
    }
    WriteLittle(bytes + FsMetadataLayout::Crc, 4U, Checksum(bytes, size, FsMetadataLayout::Crc, block, generation));
    return true;
}

bool FsFormat::DecodeDirectory(const UInt8* bytes, const Size size, const UInt32 block, const UInt64 generation,
                               FsDirectoryEntry* output) noexcept
{
    if ((output == nullptr) || !DecodeMetadataHeader(bytes, size, block, generation, FsMetadataKind::Directory) ||
        !IsZero(bytes + FsMetadataLayout::Entries + FsDirectoryCapacity * FsDirectoryLayout::Bytes, 64U))
    {
        return false;
    }
    FsDirectoryEntry candidate{};
    for (Size index{0U}; index < FsDirectoryCapacity; ++index)
    {
        const UInt8* entry = bytes + FsMetadataLayout::Entries + index * FsDirectoryLayout::Bytes;
        ReadDirectoryEntry(entry, candidate);
        if (!IsZero(entry + FsDirectoryLayout::Reserved, 4U) || !ValidDirectoryEntry(candidate))
        {
            return false;
        }
    }
    for (Size index{0U}; index < FsDirectoryCapacity; ++index)
    {
        ReadDirectoryEntry(bytes + FsMetadataLayout::Entries + index * FsDirectoryLayout::Bytes, output[index]);
    }
    return true;
}
} // namespace Asteria::User::Filesystem
