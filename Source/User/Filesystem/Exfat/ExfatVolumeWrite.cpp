// SPDX-License-Identifier: Apache-2.0

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

#include "User/Filesystem/Exfat/ExfatFormat.hpp"

// The write path of the exFAT library (exFAT volume contract to 7): create, page writes into a
// reserved contiguous run, and the commit order. ExfatVolume.cpp never refers to this unit; a
// service that links it may call Create, WritePage and Commit.

namespace Asteria::User::Filesystem
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using ExfatFormat::AllocationPossible;
using ExfatFormat::ArchiveAttribute;
using ExfatFormat::DeletedFileEntry;
using ExfatFormat::EntriesPerSector;
using ExfatFormat::EntryBytes;
using ExfatFormat::FileEntry;
using ExfatFormat::FileNameEntry;
using ExfatFormat::HashCharacter;
using ExfatFormat::NameCharactersPerEntry;
using ExfatFormat::NoEntry;
using ExfatFormat::NoFatChain;
using ExfatFormat::NoSector;
using ExfatFormat::Rotate16;
using ExfatFormat::Store16;
using ExfatFormat::Store32;
using ExfatFormat::Store64;
using ExfatFormat::StreamExtensionEntry;
using ExfatFormat::VolumeDirty;
using ExfatFormat::VolumeFlagsOffset;

/// The timestamp of created and written files until user space has a clock: 00:00:00, local time
/// (no UTC offset recorded).
constexpr UInt32 FixedTimestamp{((2026U - 1980U) << 25U) | (1U << 21U) | (1U << 16U)};
/// The largest file this interface writes, and so the run a session reserves.
constexpr UInt64 MaximumFileBytes{ExfatMaximumFilePages * ExfatPageBytes};
// Extend the selected free run without
// reserving additional clusters on disk.
constexpr UInt64 MinimumReserveBytes{6U * ExfatPageBytes};
/// The bits of the allocation bitmap one sector holds.
constexpr UInt32 BitsPerSector{ExfatSectorBytes * 8U};

/// The file protocol's name rule (FsFrame.hpp IsFsName).
[[nodiscard]] bool IsValidName(const UInt8* const name, const Size length) noexcept
{
    return ValidName(name, length) && !((length == 1U) && (name[0U] == '.')) &&
           !((length == 2U) && (name[0U] == '.') && (name[1U] == '.'));
}

[[nodiscard]] UInt64 PagesOf(const UInt64 bytes) noexcept
{
    return (bytes >> 12U) + (((bytes & (ExfatPageBytes - 1U)) != 0U) ? 1U : 0U);
}
} // Namespace

ExfatStatus ExfatVolume::Store(const UInt64 lba) noexcept
{
    m_cached = lba;
    return m_device.WriteSectors(m_device.Context, lba, 1U, m_sector) ? ExfatStatus::Done : Fail();
}

ExfatStatus ExfatVolume::ChainSector(const UInt32 first, const UInt32 ordinal, UInt64& lba) noexcept
{
    UInt32 cluster{first};
    for (UInt32 step{0U}; step < (ordinal >> m_geometry.ClusterShift); ++step)
    {
        const ExfatStatus status = IsCluster(cluster) ? GetEntry(cluster, cluster) : ExfatStatus::Corrupt;
        if (status != ExfatStatus::Done)
        {
            return status;
        }
    }
    if (!IsCluster(cluster))
    {
        return ExfatStatus::Corrupt;
    }
    lba = ClusterSector(cluster) + (ordinal & ((1U << m_geometry.ClusterShift) - 1U));
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::SetDirty(const bool dirty) noexcept
{
    // PowerOff.md: durable data before the clean marker, durable marker
    // before acknowledgement. Never report a failed device flush clean.
    if (!dirty && m_device.Synchronize != nullptr && !m_device.Synchronize(m_device.Context))
    {
        return Fail();
    }
    if (dirty == m_dirtySet)
    {
        return ExfatStatus::Done;
    }
    // The main boot sector only: VolumeFlags lies outside the boot checksum,
    // and the backup boot region is never written.
    ExfatStatus status = Load(0U);
    if (status == ExfatStatus::Done)
    {
        const UInt32 flags{m_sector[VolumeFlagsOffset]};
        m_sector[VolumeFlagsOffset] = static_cast<UInt8>(dirty ? (flags | VolumeDirty) : (flags & ~VolumeDirty));
        status = Store(0U);
    }
    if (status == ExfatStatus::Done && m_device.Synchronize != nullptr && !m_device.Synchronize(m_device.Context))
    {
        return Fail();
    }
    m_dirtySet = (status == ExfatStatus::Done) ? dirty : m_dirtySet;
    return status;
}

ExfatStatus ExfatVolume::Reserve(const UInt32 minimum) noexcept
{
    // The first run of free clusters that holds MaximumFileBytes, found by
    // walking the bitmap's sectors in order through its FAT chain.
    const UInt32 shift = m_geometry.ClusterShift + 9U;
    const UInt32 required =
        minimum == 0U ? static_cast<UInt32>((MinimumReserveBytes + (UInt64{1U} << shift) - 1U) >> shift) : minimum;
    const UInt32 wanted = static_cast<UInt32>((MaximumFileBytes + (UInt64{1U} << shift) - 1U) >> shift);
    UInt32 cluster{m_bitmap};
    UInt64 lba{0U};
    UInt32 run{0U};
    for (UInt32 bit{0U}; bit < m_geometry.Clusters; ++bit)
    {
        if ((bit % BitsPerSector) == 0U)
        {
            ExfatStatus status = NextSector(bit / BitsPerSector, cluster, lba);
            if (status == ExfatStatus::Done)
            {
                status = Load(lba);
            }
            if (status != ExfatStatus::Done)
            {
                // A bitmap chain that ends before the heap does is corrupt.
                return (status == ExfatStatus::NotFound) ? ExfatStatus::Corrupt : status;
            }
        }
        const bool allocated = (m_sector[(bit >> 3U) % ExfatSectorBytes] & (1U << (bit & 7U))) != 0U;
        if (allocated && run >= required)
        {
            m_runFirst = bit + 2U - run;
            m_runLength = run;
            return ExfatStatus::Done;
        }
        run = allocated ? 0U : (run + 1U);
        if (run == wanted)
        {
            m_runFirst = bit + 3U - wanted;
            m_runLength = wanted;
            return ExfatStatus::Done;
        }
    }
    if (run >= required)
    {
        m_runFirst = m_geometry.Clusters + 2U - run;
        m_runLength = run;
        return ExfatStatus::Done;
    }
    return ExfatStatus::Refused;
}

ExfatStatus ExfatVolume::SetBits(const UInt32 first, const UInt32 count, const bool allocated) noexcept
{
    // Each bitmap sector the clusters touch is read, changed and written once.
    ExfatStatus status{ExfatStatus::Done};
    UInt64 lba{0U};
    for (UInt32 index{0U}; (status == ExfatStatus::Done) && (index < count); ++index)
    {
        const UInt32 bit = first + index - 2U;
        if ((index == 0U) || ((bit % BitsPerSector) == 0U))
        {
            status = (index == 0U) ? ExfatStatus::Done : Store(lba);
            if (status == ExfatStatus::Done)
            {
                status = ChainSector(m_bitmap, bit / BitsPerSector, lba);
            }
            if (status == ExfatStatus::Done)
            {
                status = Load(lba);
            }
        }
        UInt8& byte = m_sector[(bit >> 3U) % ExfatSectorBytes];
        const UInt32 mask = 1U << (bit & 7U);
        if (status == ExfatStatus::Done)
        {
            byte = static_cast<UInt8>(allocated ? (byte | mask) : (byte & ~mask));
        }
    }
    return ((status == ExfatStatus::Done) && (count != 0U)) ? Store(lba) : status;
}

ExfatStatus ExfatVolume::FreeOld() noexcept
{
    // The replaced file's clusters as runs of consecutive ones: a contiguous
    // file is one; a chain (already walked by StartReplace) gives a run
    // wherever it is consecutive.
    UInt32 cluster{m_oldFirst};
    UInt32 start{cluster};
    UInt32 length{0U};
    for (UInt32 index{0U}; index < m_oldClusters; ++index)
    {
        if (!IsCluster(cluster))
        {
            return ExfatStatus::Corrupt;
        }
        if ((length != 0U) && (cluster != (start + length)))
        {
            const ExfatStatus status = SetBits(start, length, false);
            if (status != ExfatStatus::Done)
            {
                return status;
            }
            start = cluster;
            length = 0U;
        }
        ++length;
        if ((index + 1U) < m_oldClusters)
        {
            const ExfatStatus status = m_oldContiguous ? ExfatStatus::Done : GetEntry(cluster, cluster);
            if (status != ExfatStatus::Done)
            {
                return status;
            }
            cluster = m_oldContiguous ? (cluster + 1U) : cluster;
        }
    }
    return SetBits(start, length, false);
}

ExfatStatus ExfatVolume::StartReplace(File& file, const UInt64 index, const UInt64 page) noexcept
{
    // A refusal for want of a run comes before any write. exFAT volume contract: the replacement
    // run must also hold the page being appended; otherwise refuse before dirtying or copying old
    // data.
    const UInt64 end = (page + 1U) * ExfatPageBytes;
    const UInt64 bytes = file.Size > end ? file.Size : end;
    const UInt64 reserveBytes = bytes < MinimumReserveBytes ? MinimumReserveBytes : bytes;
    ExfatStatus status = Reserve(static_cast<UInt32>(
        (reserveBytes + (UInt64{1U} << (m_geometry.ClusterShift + 9U)) - 1U) >> (m_geometry.ClusterShift + 9U)));
    if (status == ExfatStatus::Done)
    {
        status = SetDirty(true);
    }
    // Copy the sectors the size covers, but those of the page the caller
    // writes next.
    const UInt32 sectors = static_cast<UInt32>((file.Size + (ExfatSectorBytes - 1U)) >> 9U);
    for (UInt32 sector{0U}; (status == ExfatStatus::Done) && (sector < sectors); ++sector)
    {
        if ((sector >> 3U) == page)
        {
            continue;
        }
        UInt64 source{0U};
        status = MapSector(file, sector, source);
        if (status == ExfatStatus::Done)
        {
            status = Load(source);
        }
        if (status == ExfatStatus::Done)
        {
            status = Store(ClusterSector(m_runFirst) + sector);
        }
    }
    if (status != ExfatStatus::Done)
    {
        m_runLength = 0U;
        return status;
    }
    const UInt32 shift = m_geometry.ClusterShift + 9U;
    m_session = Session::Replace;
    m_sessionIndex = index;
    m_sessionParent = m_directory;
    m_directoryKind = false;
    m_sessionLba = file.Lba;
    m_sessionSize = static_cast<UInt32>(file.Size);
    m_slot = static_cast<UInt8>(index % EntriesPerSector);
    m_setEntries = static_cast<UInt8>(file.Secondaries + 1U);
    m_gapLba = 0U;
    m_oldFirst = file.First;
    m_oldContiguous = file.Contiguous;
    m_oldClusters = static_cast<UInt32>((file.Size + (UInt64{1U} << shift) - 1U) >> shift);
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::ZeroTail() noexcept
{
    // The session's partial last page, from its size to the page's end.
    const UInt32 first = m_sessionSize >> 9U;
    const UInt32 end = static_cast<UInt32>(PagesOf(m_sessionSize) << 3U);
    ExfatStatus status{ExfatStatus::Done};
    for (UInt32 sector{first}; (status == ExfatStatus::Done) && (sector < end); ++sector)
    {
        const UInt64 lba = ClusterSector(m_runFirst) + sector;
        status = Load(lba);
        for (UInt32 at{(sector == first) ? (m_sessionSize & 511U) : 0U};
             (status == ExfatStatus::Done) && (at < ExfatSectorBytes); ++at)
        {
            m_sector[at] = 0U;
        }
        if (status == ExfatStatus::Done)
        {
            status = Store(lba);
        }
    }
    return status;
}

void ExfatVolume::PrepareSet() noexcept
{
    // Rewrites, in the sector buffer holding m_sessionLba, what the commit
    // changes in the session's set; idempotent, so WriteEntrySet may read the
    // sector again and repeat it.
    UInt8* const set = m_sector + (static_cast<Size>(m_slot) * EntryBytes);
    UInt8* const stream = set + EntryBytes;
    if (m_session == Session::Create)
    {
        for (Size at{0U}; at < (static_cast<Size>(m_setEntries) * EntryBytes); ++at)
        {
            set[at] = 0U;
        }
        set[0U] = static_cast<UInt8>(FileEntry);
        set[1U] = static_cast<UInt8>(m_setEntries - 1U);
        Store16(set + 4U, m_directoryKind ? ExfatFormat::DirectoryAttribute : ArchiveAttribute);
        Store32(set + 8U, FixedTimestamp);
        stream[0U] = static_cast<UInt8>(StreamExtensionEntry);
        stream[3U] = m_nameLength;

        for (UInt32 position{0U}; position < m_nameLength; ++position)
        {
            UInt8* const name = set + ((2U + (position / NameCharactersPerEntry)) * EntryBytes);
            name[0U] = static_cast<UInt8>(FileNameEntry);
            Store16(name + 2U + ((position % NameCharactersPerEntry) << 1U), m_name[position]);
        }
        Store16(stream + 4U, m_nameHash);
        // A set that reaches the end of the directory leaves only unused
        // entries after it in its sector.
        for (UInt8* tail = set + (static_cast<Size>(m_setEntries) * EntryBytes);
             m_zeroTail && (tail < (m_sector + ExfatSectorBytes)); ++tail)
        {
            *tail = 0U;
        }
    }
    Store32(set + 12U, FixedTimestamp);
    Store32(set + 16U, FixedTimestamp);
    // An empty file has no cluster and no NoFatChain flag.
    const bool data = m_sessionSize != 0U;
    stream[1U] = static_cast<UInt8>(data ? (AllocationPossible | NoFatChain) : AllocationPossible);
    Store64(stream + 8U, m_sessionSize);
    Store32(stream + 20U, data ? m_runFirst : 0U);
    Store64(stream + 24U, m_sessionSize);
}

ExfatStatus ExfatVolume::WriteEntrySet() noexcept
{
    // The SetChecksum runs over the whole set. A replaced set's File Name
    // entries may continue in the following directory sectors: they are
    // read in turn, then the set's sector is read and prepared again.
    ExfatStatus status = Load(m_sessionLba);
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    PrepareSet();
    UInt32 sum{0U};
    for (UInt32 position{0U}; (status == ExfatStatus::Done) && (position < m_setEntries); ++position)
    {
        const UInt32 index = static_cast<UInt32>(m_sessionIndex) + position;
        if ((position != 0U) && ((index % EntriesPerSector) == 0U))
        {
            UInt64 lba{0U};
            m_directory = m_sessionParent;
            UInt32 cluster{m_directory.First};
            for (UInt32 ordinal{0U}; status == ExfatStatus::Done && ordinal <= index / EntriesPerSector; ++ordinal)
            {
                status = DirectorySector(ordinal, cluster, lba);
            }
            if (status == ExfatStatus::Done)
            {
                status = Load(lba);
            }
        }
        const UInt8* const entry = m_sector + ((index % EntriesPerSector) * EntryBytes);
        for (UInt32 at{0U}; (status == ExfatStatus::Done) && (at < EntryBytes); ++at)
        {
            sum = ((position != 0U) || ((at != 2U) && (at != 3U))) ? Rotate16(sum, entry[at]) : sum;
        }
    }
    if ((status == ExfatStatus::Done) && ((m_slot + static_cast<UInt32>(m_setEntries)) > EntriesPerSector))
    {
        status = Load(m_sessionLba);
        if (status == ExfatStatus::Done)
        {
            PrepareSet();
        }
    }
    if (status == ExfatStatus::Done)
    {
        Store16(m_sector + (static_cast<Size>(m_slot) * EntryBytes) + 2U, sum);
        status = Store(m_sessionLba);
    }
    return status;
}

ExfatStatus ExfatVolume::Create(const UInt8* const name, const Size length, ExfatObject& object) noexcept
{
    return CreateIn(Root(), name, length, object);
}

ExfatStatus ExfatVolume::CreateIn(const ExfatObject& directory, const UInt8* const name, const Size length,
                                  ExfatObject& object, const bool directoryKind) noexcept
{
    const auto selected = SelectDirectory(directory);
    if (selected != ExfatStatus::Done)
    {
        return selected;
    }
    ExfatStatus status = Usable();
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if (m_readOnly || (m_session != Session::None) || !IsValidName(name, length))
    {
        return ExfatStatus::Refused;
    }
    const Size units = ToUtf16(name, length, m_name);
    const UInt32 entries = 2U + static_cast<UInt32>((units + (NameCharactersPerEntry - 1U)) / NameCharactersPerEntry);
    Scan scan{.Name = name,
              .Length = length,
              .Target = NoEntry,
              .Run = entries,
              .Found = 0U,
              .RunStart = 0U,
              .End = 0U,
              .Bitmap = 0U,
              .RunLba = 0U,
              .EndLba = 0U,
              .BitmapBytes = 0U,
              .Match = File{},
              .Directory = false,
              .Corrupt = false};
    status = ScanDirectory(scan);
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if ((scan.Found != NoEntry) || scan.Corrupt || (scan.RunStart == NoEntry))
    {
        return ExfatStatus::Refused;
    }
    m_nameHash = 0U;
    for (Size at{0U}; at < units; ++at)
    {
        Foundation::UInt16 upper{0U};
        status = Upper(m_name[at], upper);
        if (status != ExfatStatus::Done)
        {
            return status;
        }
        m_nameHash = Rotate16(Rotate16(m_nameHash, upper & 0xFFU), upper >> 8U);
    }
    m_session = Session::Create;
    m_sessionIndex = (UInt64{directory.Identifier == RootIdentifier ? 0U : m_directory.First} << 32U) | scan.RunStart;
    m_sessionParent = m_directory;
    m_directoryKind = directoryKind;
    m_sessionLba = scan.RunLba;
    m_sessionSize = 0U;
    m_slot = static_cast<UInt8>(scan.RunStart % EntriesPerSector);
    m_setEntries = static_cast<UInt8>(entries);
    m_nameLength = static_cast<UInt8>(units);
    m_zeroTail = (scan.RunStart + entries) > scan.End;
    // A set in the sector after the end mark's: that sector's free entries
    // become deleted ones, or the directory would end before the set.
    const bool gap = (scan.RunLba != scan.EndLba) && (scan.RunStart > scan.End);
    m_gapLba = gap ? scan.EndLba : 0U;
    m_gapSlot = static_cast<UInt8>(scan.End % EntriesPerSector);
    m_runLength = 0U;
    m_oldClusters = 0U;
    if (directoryKind)
    {
        status = InitializeDirectory();
        if (status != ExfatStatus::Done)
        {
            m_session = Session::None;
            m_runLength = 0U;
            return status;
        }
    }
    object = ExfatObject{.Identifier = m_sessionIndex,
                         .Generation = m_serial,
                         .ParentBytes = m_sessionParent.Size,
                         .ParentContiguous = m_sessionParent.Contiguous};
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::WritePage(const ExfatObject& object, const UInt64 page, const UInt8* const buffer) noexcept
{
    ExfatStatus status = Usable();
    if ((status == ExfatStatus::Done) && (m_readOnly || (page >= ExfatMaximumFilePages) || (buffer == nullptr)))
    {
        status = ExfatStatus::Refused;
    }
    File file{};
    if (status == ExfatStatus::Done)
    {
        status = Resolve(object, file);
    }
    // A committed file is written only when its session can be the only one,
    // its set's commit is one sector's write and a copy of it is exact.
    if ((status == ExfatStatus::Done) &&
        (file.Directory || (page > PagesOf(file.Size)) ||
         (!file.Session && ((m_session != Session::None) || (file.Size > MaximumFileBytes) ||
                            (file.Valid != file.Size) || !file.Paired))))
    {
        status = ExfatStatus::Refused;
    }
    const bool started = (status == ExfatStatus::Done) && !file.Session;
    const bool reserved = m_runLength != 0U;
    if (started)
    {
        status = StartReplace(file, object.Identifier, page);
    }
    if ((status == ExfatStatus::Done) && (m_runLength == 0U))
    {
        // A created file's first write.
        status = Reserve();
    }
    if (status == ExfatStatus::Done)
    {
        const UInt64 needed = ((page + 1U) * ExfatPageBytes + (UInt64{1U} << (m_geometry.ClusterShift + 9U)) - 1U) >>
                              (m_geometry.ClusterShift + 9U);
        if (needed > m_runLength)
        {
            status = ExfatStatus::Refused;
        }
        else
        {
            status = SetDirty(true);
        }
    }
    if ((status == ExfatStatus::Done) && ((m_sessionSize % ExfatPageBytes) != 0U) && (page == PagesOf(m_sessionSize)))
    {
        status = ZeroTail();
    }
    if (status == ExfatStatus::Done)
    {
        File session = SessionFile();
        status = Transfer(session, page, 8U, nullptr, buffer);
    }
    if (status != ExfatStatus::Done)
    {
        // A refused write leaves the session as it was: a session it started
        // ends, a run it reserved is released.
        m_session = started ? Session::None : m_session;
        m_runLength = (started || !reserved) ? 0U : m_runLength;
        return status;
    }
    const UInt32 end = static_cast<UInt32>((page + 1U) << 12U);
    m_sessionSize = (m_sessionSize > end) ? m_sessionSize : end;
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Abort() noexcept
{
    const auto status = Usable();
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    m_session = Session::None;
    m_runLength = 0U;
    m_oldClusters = 0U;
    return SetDirty(false);
}

ExfatStatus ExfatVolume::Commit(const ExfatCrashPoint crash) noexcept
{
    ExfatStatus status = Usable();
    if ((status != ExfatStatus::Done) || (m_session == Session::None))
    {
        return (status == ExfatStatus::Done) ? SetDirty(false) : status;
    }
    status = SetDirty(true);
    const UInt32 shift = m_geometry.ClusterShift + 9U;
    const UInt32 used = static_cast<UInt32>((UInt64{m_sessionSize} + (UInt64{1U} << shift) - 1U) >> shift);
    if ((status == ExfatStatus::Done) && (used != 0U))
    {
        status = SetBits(m_runFirst, used, true);
    }
    if ((status == ExfatStatus::Done) && (crash == ExfatCrashPoint::BeforeDirectoryEntry))
    {
        m_state = State::Broken;
        return ExfatStatus::Crashed;
    }
    if ((status == ExfatStatus::Done) && (m_session == Session::Create) && (m_gapLba != 0U))
    {
        status = Load(m_gapLba);
        for (UInt32 slot{m_gapSlot}; (status == ExfatStatus::Done) && (slot < EntriesPerSector); ++slot)
        {
            m_sector[slot * EntryBytes] = static_cast<UInt8>(DeletedFileEntry);
        }
        if (status == ExfatStatus::Done)
        {
            status = Store(m_gapLba);
        }
    }
    if (status == ExfatStatus::Done)
    {
        status = WriteEntrySet();
    }
    if ((status == ExfatStatus::Done) && (crash == ExfatCrashPoint::AfterDirectoryEntry))
    {
        m_state = State::Broken;
        return ExfatStatus::Crashed;
    }
    if ((status == ExfatStatus::Done) && (m_oldClusters != 0U))
    {
        status = FreeOld();
    }
    if (status == ExfatStatus::Done)
    {
        status = SetDirty(false);
    }
    if (status != ExfatStatus::Done)
    {
        // The sector buffer may hold a prepared set that never reached the
        // disk.
        m_cached = NoSector;
        return status;
    }
    m_session = Session::None;
    m_runLength = 0U;
    m_oldClusters = 0U;
    return ExfatStatus::Done;
}
ExfatStatus ExfatVolume::InitializeDirectory() noexcept
{
    auto status = Reserve(1U);
    if (status == ExfatStatus::Done)
    {
        status = SetDirty(true);
    }
    for (UInt32 sector{0U}; status == ExfatStatus::Done && sector < (1U << m_geometry.ClusterShift); ++sector)
    {
        for (Size at{0U}; at < ExfatSectorBytes; ++at)
        {
            m_sector[at] = 0U;
        }
        status = Store(ClusterSector(m_runFirst) + sector);
    }
    // ExFAT directories have no dot entries; v2 keeps the lexical parent in its open table. Growth
    // beyond this first cluster is refused.
    if (status == ExfatStatus::Done)
    {
        m_sessionSize = 1U << (m_geometry.ClusterShift + 9U);
    }
    return status;
}
} // Namespace Asteria::User::Filesystem
