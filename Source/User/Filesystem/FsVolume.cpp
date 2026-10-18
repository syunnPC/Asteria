// SPDX-License-Identifier: Apache-2.0

#include "User/Filesystem/FsVolume.hpp"

namespace Asteria::User::Filesystem
{
namespace
{
[[nodiscard]] constexpr UInt64 SuperblockSector(const Size slot) noexcept
{
    return (slot == 0U) ? FsSuperblockA : FsSuperblockB;
}
constexpr UInt64 MaximumGeneration{~UInt64{0U}};
} // namespace

void FsVolume::ReserveBitmap(UInt8* bitmap) noexcept
{
    for (Size index{0U}; index < FsBitmapBytes; ++index)
    {
        bitmap[index] = 0U;
    }
    for (UInt32 block{0U}; block < FsFirstCowBlock; ++block)
    {
        FsFormat::BitmapSet(bitmap, block);
    }
}

FsStatus FsVolume::ReadSuperblocks(const FsBlockDevice device) noexcept
{
    m_mounted = false;
    m_staged = false;
    m_device = device;
    for (Size slot{0U}; slot < 2U; ++slot)
    {
        m_valid[slot] = false;
        m_blank[slot] = false;
    }
    if ((device.ReadSectors == nullptr) || (device.WriteSectors == nullptr))
    {
        return FsStatus::IoError;
    }
    for (Size slot{0U}; slot < 2U; ++slot)
    {
        if (!m_device.ReadSectors(m_device.Context, SuperblockSector(slot), 1U, m_buffer))
        {
            return FsStatus::IoError;
        }
        m_blank[slot] = FsFormat::IsZero(m_buffer, FsSectorBytes);
        m_valid[slot] =
            FsFormat::DecodeSuperblock(m_buffer, FsSectorBytes, SuperblockSector(slot), m_superblocks[slot]);
    }
    return (m_blank[0U] && m_blank[1U]) ? FsStatus::Blank : FsStatus::Success;
}

FsStatus FsVolume::Mount(const FsBlockDevice device) noexcept
{
    const FsStatus read = ReadSuperblocks(device);
    if (read != FsStatus::Success)
    {
        return read;
    }
    Size first{0U};
    if (m_valid[1U] && (!m_valid[0U] || (m_superblocks[1U].VolumeGeneration > m_superblocks[0U].VolumeGeneration)))
    {
        first = 1U;
    }
    for (Size attempt{0U}; attempt < 2U; ++attempt)
    {
        const Size slot = (attempt == 0U) ? first : (1U - first);
        if (!m_valid[slot])
        {
            continue;
        }
        const FsStatus loaded = LoadCandidate(slot);
        if (loaded == FsStatus::IoError)
        {
            return loaded;
        }
        if (loaded == FsStatus::Success)
        {
            m_selected = slot;
            m_nextObjectId = m_superblocks[slot].NextObjectId;
            for (Size index{0U}; index < FsBitmapBytes; ++index)
            {
                m_allocated[index] = m_superblocks[slot].Bitmap[index];
            }
            m_mounted = true;
            return FsStatus::Success;
        }
    }
    return FsStatus::Corrupt;
}

FsStatus FsVolume::LoadCandidate(const Size slot) noexcept
{
    const FsSuperblock& superblock = m_superblocks[slot];
    if (!m_device.ReadSectors(m_device.Context, static_cast<UInt64>(superblock.TableBlock) * FsSectorsPerBlock,
                              FsSectorsPerBlock, m_buffer))
    {
        return FsStatus::IoError;
    }
    if ((FsFormat::ReadLittle(m_buffer + FsMetadataLayout::Crc, 4U) != superblock.TableCrc) ||
        !FsFormat::DecodeObjectTable(m_buffer, FsBlockBytes, superblock.TableBlock, superblock.VolumeGeneration,
                                     m_objects) ||
        (m_objects[0U].Kind != FsObjectKind::Directory) || !FsFormat::IsCowBlock(m_objects[0U].Direct[0U]))
    {
        return FsStatus::Corrupt;
    }
    const UInt32 directoryBlock = m_objects[0U].Direct[0U];
    if (!m_device.ReadSectors(m_device.Context, static_cast<UInt64>(directoryBlock) * FsSectorsPerBlock,
                              FsSectorsPerBlock, m_buffer))
    {
        return FsStatus::IoError;
    }
    if (!FsFormat::DecodeDirectory(m_buffer, FsBlockBytes, directoryBlock, superblock.VolumeGeneration, m_directory) ||
        !ValidateReachable(superblock))
    {
        return FsStatus::Corrupt;
    }
    return FsStatus::Success;
}

bool FsVolume::AddReachable(const UInt32 block) noexcept
{
    if (!FsFormat::IsCowBlock(block) || FsFormat::BitmapHas(m_reachable, block))
    {
        return false;
    }
    FsFormat::BitmapSet(m_reachable, block);
    return true;
}

int FsVolume::CompareName(const UInt8* left, const Size leftSize, const UInt8* right, const Size rightSize) noexcept
{
    const Size common = (leftSize < rightSize) ? leftSize : rightSize;
    for (Size index{0U}; index < common; ++index)
    {
        if (left[index] != right[index])
        {
            return (left[index] < right[index]) ? -1 : 1;
        }
    }
    if (leftSize == rightSize)
    {
        return 0;
    }
    return (leftSize < rightSize) ? -1 : 1;
}

bool FsVolume::ValidateReachable(const FsSuperblock& superblock) noexcept
{
    ReserveBitmap(m_reachable);
    if (!AddReachable(superblock.TableBlock) || !AddReachable(m_objects[0U].Direct[0U]) ||
        (m_objects[0U].SizeBytes != FsBlockBytes))
    {
        return false;
    }
    for (Size direct{1U}; direct < FsDirectCapacity; ++direct)
    {
        if (m_objects[0U].Direct[direct] != 0U)
        {
            return false;
        }
    }
    m_directoryCount = 0U;
    bool ended{false};
    for (Size index{0U}; index < FsDirectoryCapacity; ++index)
    {
        const FsDirectoryEntry& entry = m_directory[index];
        if (entry.NameLength == 0U)
        {
            ended = true;
            continue;
        }
        if (ended || (entry.ObjectId >= superblock.NextObjectId) ||
            (m_objects[entry.ObjectId].Kind != FsObjectKind::File))
        {
            return false;
        }
        if ((index != 0U) && (CompareName(m_directory[index - 1U].Name, m_directory[index - 1U].NameLength, entry.Name,
                                          entry.NameLength) >= 0))
        {
            return false;
        }
        for (Size prior{0U}; prior < index; ++prior)
        {
            if (m_directory[prior].ObjectId == entry.ObjectId)
            {
                return false;
            }
        }
        ++m_directoryCount;
    }
    // No deletion in v0: IDs are dense and each file has exactly one name.
    if ((m_directoryCount + 1U) != superblock.NextObjectId)
    {
        return false;
    }
    for (Size index{0U}; index < FsObjectCapacity; ++index)
    {
        const FsObject& object = m_objects[index];
        if (index >= superblock.NextObjectId)
        {
            if (object.Kind != FsObjectKind::Empty)
            {
                return false;
            }
            continue;
        }
        if ((object.BirthGeneration == 0U) || (object.BirthGeneration > superblock.VolumeGeneration))
        {
            return false;
        }
        if (index == 0U)
        {
            continue;
        }
        if ((object.Kind != FsObjectKind::File) || ((object.SizeBytes % FsBlockBytes) != 0U))
        {
            return false;
        }
        for (Size direct{0U}; direct < FsDirectCapacity; ++direct)
        {
            const UInt32 block = object.Direct[direct];
            if (block != 0U)
            {
                if ((direct * FsBlockBytes >= object.SizeBytes) || !AddReachable(block))
                {
                    return false;
                }
            }
        }
    }
    for (Size index{0U}; index < FsBitmapBytes; ++index)
    {
        if (m_reachable[index] != superblock.Bitmap[index])
        {
            return false;
        }
    }
    return true;
}

FsStatus FsVolume::Format(const FsBlockDevice device) noexcept
{
    const FsStatus read = ReadSuperblocks(device);
    if (read != FsStatus::Blank)
    {
        return (read == FsStatus::IoError) ? read : FsStatus::Corrupt;
    }
    for (FsObject& object : m_objects)
    {
        object = FsObject{};
    }
    for (FsDirectoryEntry& entry : m_directory)
    {
        entry = FsDirectoryEntry{};
    }
    m_superblocks[0U] = FsSuperblock{};
    m_superblocks[1U] = FsSuperblock{};
    m_objects[0U].BirthGeneration = 1U;
    m_objects[0U].Kind = FsObjectKind::Directory;
    m_objects[0U].SizeBytes = FsBlockBytes;
    m_nextObjectId = 1U;
    m_directoryCount = 0U;
    m_selected = 1U; // publish the first generation in A
    ReserveBitmap(m_allocated);
    m_mounted = true;
    return Publish(0U, 0U, false).Status;
}

FsStatus FsVolume::ReadDirectory(UInt64& cursor, UInt8* name, Size& length, FsHandle& handle) const noexcept
{
    if (!m_mounted)
    {
        return FsStatus::NotMounted;
    }
    if (cursor >= m_directoryCount || name == nullptr)
    {
        return FsStatus::NotFound;
    }
    const auto& entry = m_directory[cursor++];
    length = entry.NameLength;
    for (Size at{0U}; at < length; ++at)
    {
        name[at] = entry.Name[at];
    }
    handle = {entry.ObjectId, m_objects[entry.ObjectId].BirthGeneration};
    return FsStatus::Success;
}

FsHandleResult FsVolume::Lookup(const UInt8* name, const Size size) const noexcept
{
    if (!m_mounted)
    {
        return FsHandleResult{.Status = FsStatus::NotMounted, .Handle = {}};
    }
    if (!FsFormat::ValidName(name, size))
    {
        return FsHandleResult{.Status = FsStatus::InvalidName, .Handle = {}};
    }
    for (Size index{0U}; index < m_directoryCount; ++index)
    {
        const FsDirectoryEntry& entry = m_directory[index];
        if (CompareName(name, size, entry.Name, entry.NameLength) == 0)
        {
            return FsHandleResult{
                .Status = FsStatus::Success,
                .Handle = {.ObjectId = entry.ObjectId, .BirthGeneration = m_objects[entry.ObjectId].BirthGeneration}};
        }
    }
    return FsHandleResult{.Status = FsStatus::NotFound, .Handle = {}};
}

FsHandleResult FsVolume::Create(const UInt8* name, const Size size) noexcept
{
    const FsHandleResult found = Lookup(name, size);
    if (found.Status != FsStatus::NotFound)
    {
        return FsHandleResult{.Status = (found.Status == FsStatus::Success) ? FsStatus::AlreadyExists : found.Status,
                              .Handle = found.Handle};
    }
    if ((m_directoryCount == FsDirectoryCapacity) || (m_nextObjectId == FsObjectCapacity))
    {
        return FsHandleResult{.Status = FsStatus::Full, .Handle = {}};
    }
    if (GetGeneration() == MaximumGeneration)
    {
        return FsHandleResult{.Status = FsStatus::GenerationExhausted, .Handle = {}};
    }
    Size position{0U};
    while ((position < m_directoryCount) &&
           (CompareName(m_directory[position].Name, m_directory[position].NameLength, name, size) < 0))
    {
        ++position;
    }
    for (Size index{m_directoryCount}; index > position; --index)
    {
        m_directory[index] = m_directory[index - 1U];
    }
    FsDirectoryEntry& entry = m_directory[position];
    entry = FsDirectoryEntry{};
    entry.NameLength = static_cast<UInt32>(size);
    entry.ObjectId = m_nextObjectId;
    for (Size index{0U}; index < size; ++index)
    {
        entry.Name[index] = name[index];
    }
    FsObject& object = m_objects[m_nextObjectId];
    object = FsObject{};
    object.BirthGeneration = GetGeneration() + 1U;
    object.Kind = FsObjectKind::File;
    ++m_directoryCount;
    ++m_nextObjectId;
    m_staged = true;
    return FsHandleResult{.Status = FsStatus::Success,
                          .Handle = {.ObjectId = entry.ObjectId, .BirthGeneration = object.BirthGeneration}};
}

FsStatus FsVolume::ValidateHandle(const FsHandle handle) const noexcept
{
    if (!m_mounted)
    {
        return FsStatus::NotMounted;
    }
    if ((handle.ObjectId >= m_nextObjectId) || (handle.BirthGeneration == 0U) ||
        (m_objects[handle.ObjectId].BirthGeneration != handle.BirthGeneration))
    {
        return FsStatus::StaleObject;
    }
    return FsStatus::Success;
}

FsStatResult FsVolume::Stat(const FsHandle handle) const noexcept
{
    const FsStatus status = ValidateHandle(handle);
    if (status != FsStatus::Success)
    {
        return FsStatResult{.Status = status, .Object = {}};
    }
    return FsStatResult{.Status = FsStatus::Success, .Object = m_objects[handle.ObjectId]};
}

UInt32 FsVolume::FindFreshBlock() noexcept
{
    for (UInt32 block{FsFirstCowBlock}; block < FsTotalBlocks; ++block)
    {
        if (!FsFormat::BitmapHas(m_allocated, block))
        {
            FsFormat::BitmapSet(m_allocated, block);
            return block;
        }
    }
    return 0U;
}

FsAllocationResult FsVolume::AllocateData(const FsHandle handle, const Size index) noexcept
{
    const FsStatus status = ValidateHandle(handle);
    if (status != FsStatus::Success)
    {
        return FsAllocationResult{.Status = status, .Block = 0U};
    }
    if (m_objects[handle.ObjectId].Kind != FsObjectKind::File)
    {
        return FsAllocationResult{.Status = FsStatus::InvalidObject, .Block = 0U};
    }
    if (index >= FsDirectCapacity)
    {
        return FsAllocationResult{.Status = FsStatus::InvalidIndex, .Block = 0U};
    }
    UInt32 free{0U};
    for (UInt32 block{FsFirstCowBlock}; block < FsTotalBlocks; ++block)
    {
        free += FsFormat::BitmapHas(m_allocated, block) ? 0U : 1U;
    }
    // Always leave two fresh metadata blocks for Commit.
    if (free <= 2U)
    {
        return FsAllocationResult{.Status = FsStatus::Full, .Block = 0U};
    }
    const UInt32 block = FindFreshBlock();
    FsObject& object = m_objects[handle.ObjectId];
    object.Direct[index] = static_cast<UInt16>(block);
    const UInt64 end = (index + 1U) * FsBlockBytes;
    if (object.SizeBytes < end)
    {
        object.SizeBytes = end;
    }
    m_staged = true;
    return FsAllocationResult{.Status = FsStatus::Success, .Block = block};
}

bool FsVolume::WriteBlock(const UInt32 block) noexcept
{
    return FsFormat::IsCowBlock(block) &&
           m_device.WriteSectors(m_device.Context, static_cast<UInt64>(block) * FsSectorsPerBlock, FsSectorsPerBlock,
                                 m_buffer);
}

FsCommitResult FsVolume::Publish(const UInt64 sender, const UInt64 transaction, const bool recordCommit) noexcept
{
    if (GetGeneration() == MaximumGeneration)
    {
        return FsCommitResult{.Status = FsStatus::GenerationExhausted, .Generation = 0U};
    }
    m_candidate = m_superblocks[m_selected];
    ++m_candidate.VolumeGeneration;
    m_candidate.NextObjectId = m_nextObjectId;
    const UInt32 tableBlock = FindFreshBlock();
    const UInt32 directoryBlock = FindFreshBlock();
    if ((tableBlock == 0U) || (directoryBlock == 0U))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::Full, .Generation = 0U};
    }
    m_candidate.TableBlock = tableBlock;
    m_objects[0U].Direct[0U] = static_cast<UInt16>(directoryBlock);
    ReserveBitmap(m_candidate.Bitmap);
    FsFormat::BitmapSet(m_candidate.Bitmap, tableBlock);
    for (Size object{0U}; object < m_nextObjectId; ++object)
    {
        for (const UInt16 block : m_objects[object].Direct)
        {
            if (block != 0U)
            {
                FsFormat::BitmapSet(m_candidate.Bitmap, block);
            }
        }
    }
    if (recordCommit)
    {
        for (Size index{FsCommitCapacity - 1U}; index > 0U; --index)
        {
            m_candidate.Committed[index] = m_candidate.Committed[index - 1U];
            m_candidate.CommitGenerations[index] = m_candidate.CommitGenerations[index - 1U];
        }
        m_candidate.Committed[0U] = FsCommitRecord{.Sender = sender, .Transaction = transaction};
        m_candidate.CommitGenerations[0U] = m_candidate.VolumeGeneration;
    }
    if (!FsFormat::EncodeObjectTable(m_objects, tableBlock, m_candidate.VolumeGeneration, m_buffer, FsBlockBytes))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::Corrupt, .Generation = 0U};
    }
    m_candidate.TableCrc = static_cast<UInt32>(FsFormat::ReadLittle(m_buffer + FsMetadataLayout::Crc, 4U));
    if (!WriteBlock(tableBlock))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::IoError, .Generation = 0U};
    }
    if (!FsFormat::EncodeDirectory(m_directory, directoryBlock, m_candidate.VolumeGeneration, m_buffer, FsBlockBytes))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::Corrupt, .Generation = 0U};
    }
    if (!WriteBlock(directoryBlock))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::IoError, .Generation = 0U};
    }
    const Size other = 1U - m_selected;
    const UInt64 sector = SuperblockSector(other);
    if (!FsFormat::EncodeSuperblock(m_candidate, sector, m_buffer, FsSectorBytes))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::Corrupt, .Generation = 0U};
    }
    if (!m_device.WriteSectors(m_device.Context, sector, 1U, m_buffer))
    {
        m_mounted = false;
        return FsCommitResult{.Status = FsStatus::IoError, .Generation = 0U};
    }
    m_superblocks[other] = m_candidate;
    m_selected = other;
    for (Size index{0U}; index < FsBitmapBytes; ++index)
    {
        m_allocated[index] = m_candidate.Bitmap[index];
    }
    m_staged = false;
    return FsCommitResult{.Status = FsStatus::Success, .Generation = m_candidate.VolumeGeneration};
}

FsCommitResult FsVolume::Commit(const UInt64 sender, const UInt64 transaction) noexcept
{
    if (!m_mounted)
    {
        return FsCommitResult{.Status = FsStatus::NotMounted, .Generation = 0U};
    }
    const FsCommitResult previous = QueryCommit(sender, transaction);
    if (previous.Status == FsStatus::Committed)
    {
        return FsCommitResult{.Status = FsStatus::Success, .Generation = previous.Generation};
    }
    return Publish(sender, transaction, true);
}

FsCommitResult FsVolume::CommitStaged() noexcept
{
    if (!m_mounted)
    {
        return FsCommitResult{.Status = FsStatus::NotMounted, .Generation = 0U};
    }
    return m_staged ? Publish(0U, 0U, false)
                    : FsCommitResult{.Status = FsStatus::Success, .Generation = GetGeneration()};
}

FsCommitResult FsVolume::QueryCommit(const UInt64 sender, const UInt64 transaction) const noexcept
{
    if (!m_mounted)
    {
        return FsCommitResult{.Status = FsStatus::NotMounted, .Generation = 0U};
    }
    const FsSuperblock& superblock = m_superblocks[m_selected];
    for (Size index{0U}; index < FsCommitCapacity; ++index)
    {
        if ((superblock.CommitGenerations[index] != 0U) && (superblock.Committed[index].Sender == sender) &&
            (superblock.Committed[index].Transaction == transaction))
        {
            return FsCommitResult{.Status = FsStatus::Committed, .Generation = superblock.CommitGenerations[index]};
        }
    }
    return FsCommitResult{.Status = FsStatus::NotCommitted, .Generation = 0U};
}

UInt64 FsVolume::GetGeneration() const noexcept
{
    return m_mounted ? m_superblocks[m_selected].VolumeGeneration : 0U;
}

const FsSuperblock& FsVolume::GetCommittedSuperblock() const noexcept
{
    return m_superblocks[m_selected];
}
} // namespace Asteria::User::Filesystem
