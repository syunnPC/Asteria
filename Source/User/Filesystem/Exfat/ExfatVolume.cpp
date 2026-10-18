// SPDX-License-Identifier: Apache-2.0

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

#include "User/Filesystem/Exfat/ExfatFormat.hpp"

// The read path of the exFAT library: recognition, mount, directory decoding,
// open, stat and page reads. Nothing here refers to ExfatVolumeWrite.cpp, so a
// read-only service links this unit alone (ExfatVolume.hpp).

namespace Asteria::User::Filesystem
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using ExfatFormat::AllocationBitmapEntry;
using ExfatFormat::BenignImportance;
using ExfatFormat::DirectoryAttribute;
using ExfatFormat::EndOfChain;
using ExfatFormat::EndOfDirectory;
using ExfatFormat::EntriesPerSector;
using ExfatFormat::EntryBytes;
using ExfatFormat::FileEntry;
using ExfatFormat::FileNameEntry;
using ExfatFormat::HashCharacter;
using ExfatFormat::InUse;
using ExfatFormat::Load16;
using ExfatFormat::Load32;
using ExfatFormat::Load64;
using ExfatFormat::MaximumSecondaries;
using ExfatFormat::MediaFailure;
using ExfatFormat::MinimumSecondaries;
using ExfatFormat::NameCharactersPerEntry;
using ExfatFormat::NoEntry;
using ExfatFormat::NoFatChain;
using ExfatFormat::NoSector;
using ExfatFormat::Rotate16;
using ExfatFormat::SecondaryCategory;
using ExfatFormat::StreamExtensionEntry;
using ExfatFormat::VolumeDirty;
using ExfatFormat::VolumeFlagsOffset;

/// The root directory is followed for at most 4096 sectors (65536 entries),
/// FatVolume's bound; entries beyond are not the directory's here.
constexpr UInt32 MaximumDirectorySectors{4096U};
/// The main boot region: sectors 0 to 10 summed into the 128 copies of
/// sector 11, skipping the boot sector's VolumeFlags and PercentInUse.
constexpr UInt64 BootChecksumSector{11U};
constexpr Size PercentInUseOffset{112U};
constexpr UInt8 FileSystemName[8U]{'E', 'X', 'F', 'A', 'T', ' ', ' ', ' '};

/// An entry set being decoded as the scan streams past it: from its File
/// entry (Start, at sector Lba) through Count entries, Position of them seen.
/// Sum, Hash: the running SetChecksum and NameHash; Checksum, NameHash: the
/// values the set records. Ascii: every name character seen is ASCII (else
/// the hash cannot be checked without the up-case table). Match: the name
/// agrees with the wanted one so far. WellFormed: every entry seen is where
/// the specification puts it. Paired: the Stream Extension shares the File
/// entry's sector.
struct Set
{
    UInt64 Lba;
    UInt64 Size;
    UInt64 Valid;
    UInt32 Start;
    UInt32 First;
    UInt32 Count;
    UInt32 Position;
    UInt32 Sum;
    UInt32 Checksum;
    UInt32 Hash;
    UInt32 NameHash;
    UInt32 NameLength;
    UInt32 Attributes;
    UInt32 Flags;
    bool Ascii;
    bool Match;
    bool WellFormed;
    bool Paired;
};

[[nodiscard]] UInt32 NameEntries(const UInt32 nameLength) noexcept
{
    return (nameLength + (NameCharactersPerEntry - 1U)) / NameCharactersPerEntry;
}

/// Feeds the set its next entry, which lies in sector lba; name and length
/// are the wanted name (name nullptr for none).
void Feed(Set& set, const UInt8* const entry, const UInt64 lba, const UInt8* const name, const Size /*length*/,
          Foundation::UInt16* const units) noexcept
{
    const UInt32 position{set.Position};
    for (UInt32 at{0U}; at < EntryBytes; ++at)
    {
        if ((position != 0U) || ((at != 2U) && (at != 3U)))
        {
            set.Sum = Rotate16(set.Sum, entry[at]);
        }
    }
    const UInt32 type{entry[0U]};
    ++set.Position;
    if (position == 1U)
    {
        set.Flags = entry[1U];
        set.NameLength = entry[3U];
        set.NameHash = Load16(entry + 4U);
        set.Valid = Load64(entry + 8U);
        set.First = Load32(entry + 20U);
        set.Size = Load64(entry + 24U);
        set.Paired = lba == set.Lba;
        set.Match = name != nullptr;
        set.WellFormed = set.WellFormed && (type == StreamExtensionEntry) && (set.NameLength != 0U) &&
                         (set.Count >= (2U + NameEntries(set.NameLength))) && (set.Valid <= set.Size);
        return;
    }
    if ((position < 2U) || (position >= (2U + NameEntries(set.NameLength))))
    {
        // The File entry itself, or past the name: benign secondaries only.
        set.WellFormed = set.WellFormed && ((position == 0U) || ((type & BenignImportance) != 0U));
        return;
    }
    set.WellFormed = set.WellFormed && (type == FileNameEntry);
    const UInt32 base = (position - 2U) * NameCharactersPerEntry;
    for (UInt32 character{0U}; (character < NameCharactersPerEntry) && ((base + character) < set.NameLength);
         ++character)
    {
        const UInt32 value = Load16(entry + 2U + (character << 1U));
        units[base + character] = static_cast<Foundation::UInt16>(value);
        set.Ascii = set.Ascii && (value < 0x80U);
        set.Hash = HashCharacter(set.Hash, value);
    }
}

/// Starts decoding the set whose File entry is entry (directory index index).
void Begin(Set& set, const UInt8* const entry, const UInt32 index, const UInt64 lba, const UInt8* const name,
           const Size length, Foundation::UInt16* const units) noexcept
{
    const UInt32 secondaries{entry[1U]};
    set = Set{.Lba = lba,
              .Size = 0U,
              .Valid = 0U,
              .Start = index,
              .First = 0U,
              .Count = secondaries + 1U,
              .Position = 0U,
              .Sum = 0U,
              .Checksum = Load16(entry + 2U),
              .Hash = 0U,
              .NameHash = 0U,
              .NameLength = 0U,
              .Attributes = Load16(entry + 4U),
              .Flags = 0U,
              .Ascii = true,
              .Match = false,
              .WellFormed = (secondaries >= MinimumSecondaries) && (secondaries <= MaximumSecondaries),
              .Paired = false};
    Feed(set, entry, lba, name, length, units);
}

/// Whether the set is complete, well formed and its sums agree (the name's
/// hash only for an ASCII name).
[[nodiscard]] bool IsValid(const Set& set) noexcept
{
    return set.WellFormed && (set.Position == set.Count) && (set.Sum == set.Checksum) && (set.Hash == set.NameHash);
}
} // Namespace

ExfatStatus ReadExfatBootSector(const UInt8* const boot, const UInt64 deviceSectors, ExfatGeometry& geometry) noexcept
{
    for (Size index{0U}; index < sizeof(FileSystemName); ++index)
    {
        if (boot[3U + index] != FileSystemName[index])
        {
            return ExfatStatus::NotExfat;
        }
    }
    bool zero{true};
    for (Size index{11U}; index < 64U; ++index)
    {
        zero = zero && (boot[index] == 0U);
    }
    const UInt64 volume = Load64(boot + 72U);
    const UInt64 fatStart = Load32(boot + 80U);
    const UInt64 fatSectors = Load32(boot + 84U);
    const UInt64 heap = Load32(boot + 88U);
    const UInt32 clusters = Load32(boot + 92U);
    const UInt32 root = Load32(boot + 96U);
    const UInt32 shift{boot[109U]};
    // BytesPerSectorShift 9, SectorsPerClusterShift at most 25 - 9,
    // NumberOfFats 1, FileSystemRevision 1.x; a FAT of 4-byte entries for
    // clusters 0 to clusters + 1; ClusterCount at most 0xFFFFFFF5.
    if (!zero || (boot[510U] != 0x55U) || (boot[511U] != 0xAAU) || (boot[108U] != 9U) || (shift > 16U) ||
        (boot[110U] != 1U) || (boot[105U] != 1U) || (volume > deviceSectors) || (fatStart < 24U) ||
        ((fatSectors << 7U) < (static_cast<UInt64>(clusters) + 2U)) || (heap < (fatStart + fatSectors)) ||
        (clusters == 0U) || (clusters > 0xFFFF'FFF5U) || ((heap + (static_cast<UInt64>(clusters) << shift)) > volume) ||
        (root < 2U) || (root > (clusters + 1U)))
    {
        return ExfatStatus::Corrupt;
    }
    geometry = ExfatGeometry{.VolumeSectors = volume,
                             .FatStart = fatStart,
                             .FatSectors = fatSectors,
                             .HeapStart = heap,
                             .Clusters = clusters,
                             .RootCluster = root,
                             .ClusterShift = static_cast<UInt8>(shift)};
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Fail() noexcept
{
    m_state = State::Broken;
    return ExfatStatus::Failed;
}

ExfatStatus ExfatVolume::Usable() const noexcept
{
    if (m_state == State::Mounted)
    {
        return ExfatStatus::Done;
    }
    return (m_state == State::Broken) ? ExfatStatus::Failed : ExfatStatus::Refused;
}

bool ExfatVolume::IsMounted() const noexcept
{
    return m_state == State::Mounted;
}

bool ExfatVolume::IsReadOnly() const noexcept
{
    return m_readOnly;
}

const ExfatGeometry& ExfatVolume::GetGeometry() const noexcept
{
    return m_geometry;
}

bool ExfatVolume::IsCluster(const UInt32 value) const noexcept
{
    return (value >= 2U) && (value <= (m_geometry.Clusters + 1U));
}

UInt64 ExfatVolume::ClusterSector(const UInt32 cluster) const noexcept
{
    return m_geometry.HeapStart + (static_cast<UInt64>(cluster - 2U) << m_geometry.ClusterShift);
}

ExfatVolume::File ExfatVolume::SessionFile() const noexcept
{
    return File{.Lba = m_sessionLba,
                .Size = m_sessionSize,
                .Valid = m_sessionSize,
                .First = m_runFirst,
                .Cursor = 0U,
                .CursorCluster = m_runFirst,
                .Secondaries = 0U,
                .Contiguous = true,
                .Paired = true,
                .Session = true,
                .Directory = m_directoryKind};
}

ExfatStatus ExfatVolume::Load(const UInt64 lba) noexcept
{
    if (lba == m_cached)
    {
        return ExfatStatus::Done;
    }
    m_cached = NoSector;
    if (!m_device.ReadSectors(m_device.Context, lba, 1U, m_sector))
    {
        return Fail();
    }
    m_cached = lba;
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::GetEntry(const UInt32 cluster, UInt32& value) noexcept
{
    // The caller passes a cluster of the heap: its entry lies in the FAT.
    const ExfatStatus status = Load(m_geometry.FatStart + (cluster >> 7U));
    if (status == ExfatStatus::Done)
    {
        value = Load32(m_sector + ((cluster & 127U) << 2U));
    }
    return status;
}

ExfatStatus ExfatVolume::NextSector(const UInt32 ordinal, UInt32& cluster, UInt64& lba) noexcept
{
    const UInt32 mask = (1U << m_geometry.ClusterShift) - 1U;
    if ((ordinal != 0U) && ((ordinal & mask) == 0U))
    {
        UInt32 next{0U};
        const ExfatStatus status = GetEntry(cluster, next);
        if (status != ExfatStatus::Done)
        {
            return status;
        }
        if (!IsCluster(next))
        {
            return (next == EndOfChain) ? ExfatStatus::NotFound : ExfatStatus::Corrupt;
        }
        cluster = next;
    }
    lba = ClusterSector(cluster) + (ordinal & mask);
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::ScanDirectory(Scan& scan) noexcept
{
    scan.Found = NoEntry;
    scan.RunStart = NoEntry;
    scan.End = NoEntry;
    scan.Bitmap = 0U;
    scan.Directory = false;
    scan.Corrupt = false;
    Set set{};
    // Ends the open set, complete or cut short: the first set the scan wants
    // (by name, all of it seen, or by index) is its answer.
    const auto close = [this, &scan, &set]() noexcept
    {
        const Size rendered = FromUtf16(m_scanName, set.NameLength, m_rendered);
        set.Match = (scan.Name != nullptr) && SameName(m_rendered, rendered, scan.Name, scan.Length);
        if (set.Count != 0U && !ValidUtf16(m_scanName, set.NameLength))
        {
            set.WellFormed = false;
        }
        if (!set.Match && scan.Name != nullptr && Text::ValidUtf8(scan.Name, scan.Length, true) &&
            ToUtf16(scan.Name, scan.Length, m_matchName) == set.NameLength)
        {
            set.Match = true;
            for (Size at{0U}; set.Match && at < set.NameLength; ++at)
            {
                Foundation::UInt16 a{0U};
                Foundation::UInt16 b{0U};
                set.Match = Upper(m_scanName[at], a) == ExfatStatus::Done &&
                            Upper(m_matchName[at], b) == ExfatStatus::Done && a == b;
            }
        }
        if (!set.Ascii && set.Position >= (2U + NameEntries(set.NameLength)))
        {
            set.Hash = 0U;
            for (Size at{0U}; at < set.NameLength; ++at)
            {
                Foundation::UInt16 upper{0U};
                if (Upper(m_scanName[at], upper) != ExfatStatus::Done)
                {
                    set.WellFormed = false;
                    break;
                }
                set.Hash = ExfatFormat::Rotate16(ExfatFormat::Rotate16(set.Hash, upper & 0xFFU), upper >> 8U);
            }
        }
        const bool named = set.Match && (set.Position >= (2U + NameEntries(set.NameLength)));
        if ((set.Count != 0U) &&
            ((set.Start == scan.Target) || named || (scan.Enumerate && set.Start >= scan.Cursor && rendered != 0U)) &&
            (scan.Found == NoEntry) && !scan.Corrupt)
        {
            scan.Corrupt = !IsValid(set);
            scan.Found = scan.Corrupt ? NoEntry : set.Start;
            scan.Directory = (set.Attributes & DirectoryAttribute) != 0U;
            scan.Match = File{.Lba = set.Lba,
                              .Size = set.Size,
                              .Valid = set.Valid,
                              .First = set.First,
                              .Cursor = 0U,
                              .CursorCluster = set.First,
                              .Secondaries = static_cast<UInt8>(set.Count - 1U),
                              .Contiguous = (set.Flags & NoFatChain) != 0U,
                              .Paired = set.Paired,
                              .Session = false,
                              .Directory = scan.Directory};
            if (scan.Enumerate && !scan.Corrupt)
            {
                scan.LengthOut = rendered;
                for (Size at{0U}; at < rendered; ++at)
                {
                    scan.Output[at] = m_rendered[at];
                }
            }
        }
        set.Count = 0U;
    };
    UInt32 cluster{m_directory.First};
    UInt64 lba{0U};
    UInt32 index{0U};
    for (UInt32 ordinal{0U}; ordinal < MaximumDirectorySectors; ++ordinal)
    {
        ExfatStatus status = DirectorySector(ordinal, cluster, lba);
        if (status == ExfatStatus::NotFound)
        {
            break;
        }
        if (status == ExfatStatus::Done)
        {
            status = Load(lba);
        }
        if (status != ExfatStatus::Done)
        {
            return status;
        }
        UInt32 run{0U};
        for (UInt32 slot{0U}; slot < EntriesPerSector; ++slot, ++index)
        {
            const UInt8* const entry = m_sector + (slot * EntryBytes);
            const UInt32 type{entry[0U]};
            if ((scan.End == NoEntry) && (type == EndOfDirectory))
            {
                scan.End = index;
                scan.EndLba = lba;
            }
            if ((scan.End != NoEntry) || ((type & InUse) == 0U))
            {
                // A free entry: past the end, or deleted.
                close();
                ++run;
                if ((run == scan.Run) && (scan.RunStart == NoEntry))
                {
                    scan.RunStart = index + 1U - run;
                    scan.RunLba = lba;
                }
                continue;
            }
            run = 0U;
            if ((set.Count != 0U) && ((type & SecondaryCategory) != 0U))
            {
                Feed(set, entry, lba, scan.Name, scan.Length, m_scanName);
                if (set.Position == set.Count)
                {
                    close();
                }
                continue;
            }
            // A primary entry ends any open set; other primaries (the label,
            // the up-case table, benign ones) and orphaned secondaries are
            // skipped.
            close();
            if (type == FileEntry)
            {
                Begin(set, entry, index, lba, scan.Name, scan.Length, m_scanName);
            }
            else if (type == 0x82U)
            {
                scan.Upcase = Load32(entry + 20U);
                scan.UpcaseBytes = Load64(entry + 24U);
                scan.UpcaseChecksum = Load32(entry + 4U);
            }
            else if ((type == AllocationBitmapEntry) && (scan.Bitmap == 0U) && ((entry[1U] & 1U) == 0U))
            {
                scan.Bitmap = Load32(entry + 20U);
                scan.BitmapBytes = Load64(entry + 24U);
            }
        }
        if ((scan.Run == 0U) && ((scan.Found != NoEntry) || scan.Corrupt))
        {
            return m_state == State::Broken ? ExfatStatus::Failed : ExfatStatus::Done;
        }
        if ((scan.End != NoEntry) && ((scan.Run == 0U) || (scan.RunStart != NoEntry)))
        {
            break;
        }
    }
    close();
    scan.End = (scan.End == NoEntry) ? index : scan.End;
    return m_state == State::Broken ? ExfatStatus::Failed : ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Mount(const ExfatBlockDevice& device, const UInt64 serial) noexcept
{
    m_state = State::Unmounted;
    m_session = Session::None;
    m_runLength = 0U;
    m_oldClusters = 0U;
    m_dirtySet = false;
    m_readOnly = true;
    m_cached = NoSector;
    m_device = device;
    m_serial = serial;
    ExfatStatus status = Load(0U);
    ExfatGeometry geometry{};
    if (status == ExfatStatus::Done)
    {
        status = ReadExfatBootSector(m_sector, device.Sectors, geometry);
    }
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    m_geometry = geometry;
    m_directory = File{.First = geometry.RootCluster};
    m_directoryKind = false;
    const UInt32 flags{m_sector[VolumeFlagsOffset]};
    UInt32 checksum{0U};
    for (UInt64 lba{0U}; (status == ExfatStatus::Done) && (lba < BootChecksumSector); ++lba)
    {
        status = Load(lba);
        for (Size at{0U}; (status == ExfatStatus::Done) && (at < ExfatSectorBytes); ++at)
        {
            const bool skipped = (lba == 0U) && ((at == VolumeFlagsOffset) || (at == (VolumeFlagsOffset + 1U)) ||
                                                 (at == PercentInUseOffset));
            if (!skipped)
            {
                // A 32-bit rotate right and add; the sum wraps by design.
                checksum = ((checksum & 1U) << 31U) + (checksum >> 1U) + m_sector[at];
            }
        }
    }
    if (status == ExfatStatus::Done)
    {
        status = Load(BootChecksumSector);
    }
    for (Size at{0U}; (status == ExfatStatus::Done) && (at < ExfatSectorBytes); at += 4U)
    {
        status = (Load32(m_sector + at) == checksum) ? ExfatStatus::Done : ExfatStatus::Corrupt;
    }
    Scan scan{.Name = nullptr,
              .Length = 0U,
              .Target = NoEntry,
              .Run = 0U,
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
    if (status == ExfatStatus::Done)
    {
        status = ScanDirectory(scan);
    }
    if ((status == ExfatStatus::Done) &&
        (!IsCluster(scan.Bitmap) || (scan.BitmapBytes < ((static_cast<UInt64>(geometry.Clusters) + 7U) >> 3U))))
    {
        status = ExfatStatus::Corrupt;
    }
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    m_bitmap = scan.Bitmap;
    m_upcase = scan.Upcase;
    m_upcaseBytes = scan.UpcaseBytes;
    m_upcaseChecksum = scan.UpcaseChecksum;
    m_upcaseVerified = false;
    m_upperCount = 0U;
    m_readOnly = (flags & (VolumeDirty | MediaFailure)) != 0U;
    m_state = State::Mounted;
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Open(const UInt8* const name, const Size length, ExfatObject& object) noexcept
{
    return Lookup(Root(), name, length, object);
}

ExfatStatus ExfatVolume::Lookup(const ExfatObject& directory, const UInt8* const name, const Size length,
                                ExfatObject& object) noexcept
{
    const auto selected = SelectDirectory(directory);
    if (selected != ExfatStatus::Done)
    {
        return selected;
    }
    ExfatStatus status = Usable();
    if ((status == ExfatStatus::Done) && ((name == nullptr) || (length == 0U) || (length > ExfatMaximumNameBytes)))
    {
        status = ExfatStatus::Refused;
    }
    Scan scan{.Name = name,
              .Length = length,
              .Target = NoEntry,
              .Run = 0U,
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
    if (status == ExfatStatus::Done)
    {
        status = ScanDirectory(scan);
    }
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if (scan.Corrupt)
    {
        return ExfatStatus::Corrupt;
    }
    if (scan.Found == NoEntry)
    {
        return ExfatStatus::NotFound;
    }
    object = ExfatObject{
        .Identifier = (UInt64{directory.Identifier == RootIdentifier ? 0U : m_directory.First} << 32U) | scan.Found,
        .Generation = m_serial,
        .ParentBytes = m_directory.Size,
        .ParentContiguous = m_directory.Contiguous};
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Resolve(const ExfatObject& object, File& file) noexcept
{
    if (object.Generation != m_serial)
    {
        return ExfatStatus::StaleObject;
    }
    if (object.Identifier == RootIdentifier)
    {
        file = File{.First = m_geometry.RootCluster, .Directory = true};
        return ExfatStatus::Done;
    }
    const UInt32 parent = static_cast<UInt32>(object.Identifier >> 32U);
    m_directory = File{.Size = object.ParentBytes,
                       .First = parent == 0U ? m_geometry.RootCluster : parent,
                       .Contiguous = object.ParentContiguous};
    if (!IsCluster(m_directory.First))
    {
        return ExfatStatus::StaleObject;
    }
    if ((m_session != Session::None) && (object.Identifier == m_sessionIndex))
    {
        file = SessionFile();
        return ExfatStatus::Done;
    }
    if (static_cast<UInt32>(object.Identifier) >= (static_cast<UInt64>(MaximumDirectorySectors) * EntriesPerSector))
    {
        return ExfatStatus::StaleObject;
    }
    Scan scan{.Name = nullptr,
              .Length = 0U,
              .Target = static_cast<UInt32>(object.Identifier),
              .Run = 0U,
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
    const ExfatStatus status = ScanDirectory(scan);
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if (scan.Corrupt)
    {
        return ExfatStatus::Corrupt;
    }
    if (scan.Found == NoEntry)
    {
        return ExfatStatus::StaleObject;
    }
    file = scan.Match;
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Stat(const ExfatObject& object, ExfatFileSize& size) noexcept
{
    File file{};
    ExfatStatus status = Usable();
    if (status == ExfatStatus::Done)
    {
        status = Resolve(object, file);
    }
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    size = ExfatFileSize{.Pages = (file.Size >> 12U) + (((file.Size & (ExfatPageBytes - 1U)) != 0U) ? 1U : 0U),
                         .Bytes = file.Size,
                         .Kind = file.Directory ? NodeKind::Directory : NodeKind::File};
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::MapSector(File& file, const UInt32 fileSector, UInt64& lba) noexcept
{
    const UInt32 index = fileSector >> m_geometry.ClusterShift;
    if (index >= m_geometry.Clusters)
    {
        return ExfatStatus::Corrupt;
    }
    UInt32 cluster{file.First + index};
    if (file.Contiguous)
    {
        if (!IsCluster(file.First) || (index > ((m_geometry.Clusters + 1U) - file.First)))
        {
            return ExfatStatus::Corrupt;
        }
    }
    else
    {
        // A chain is walked on from the last sector mapped: every caller maps
        // a file's sectors in increasing order (one going back would get
        // Corrupt, not another cluster).
        while ((file.Cursor < index) && IsCluster(file.CursorCluster))
        {
            const ExfatStatus status = GetEntry(file.CursorCluster, file.CursorCluster);
            if (status != ExfatStatus::Done)
            {
                return status;
            }
            ++file.Cursor;
        }
        if ((file.Cursor != index) || !IsCluster(file.CursorCluster))
        {
            return ExfatStatus::Corrupt;
        }
        cluster = file.CursorCluster;
    }
    lba = ClusterSector(cluster) + (fileSector & ((1U << m_geometry.ClusterShift) - 1U));
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::Transfer(File& file, const UInt64 page, const UInt32 sectors, UInt8* const readBuffer,
                                  const UInt8* const writeBuffer) noexcept
{
    // Device writes of a page bypass the sector buffer, which then no
    // longer holds what the disk does.
    m_cached = (writeBuffer != nullptr) ? NoSector : m_cached;
    UInt64 runLba{0U};
    UInt32 runFirst{0U};
    UInt32 runCount{0U};
    for (UInt32 sector{0U}; sector <= sectors; ++sector)
    {
        UInt64 lba{0U};
        if (sector < sectors)
        {
            const ExfatStatus status = MapSector(file, (static_cast<UInt32>(page) << 3U) + sector, lba);
            if (status != ExfatStatus::Done)
            {
                return status;
            }
        }
        if ((runCount != 0U) && ((sector == sectors) || (lba != (runLba + runCount))))
        {
            const Size offset = static_cast<Size>(runFirst) * ExfatSectorBytes;
            const bool done = (writeBuffer != nullptr)
                                  ? m_device.WriteSectors(m_device.Context, runLba, runCount, writeBuffer + offset)
                                  : m_device.ReadSectors(m_device.Context, runLba, runCount, readBuffer + offset);
            if (!done)
            {
                return Fail();
            }
            runCount = 0U;
        }
        if (sector < sectors)
        {
            runLba = (runCount == 0U) ? lba : runLba;
            runFirst = (runCount == 0U) ? sector : runFirst;
            ++runCount;
        }
    }
    return ExfatStatus::Done;
}

ExfatStatus ExfatVolume::ReadPage(const ExfatObject& object, const UInt64 page, UInt8* const buffer) noexcept
{
    ExfatStatus status = Usable();
    if ((status == ExfatStatus::Done) && ((page > 0x1FFF'FFFFU) || (buffer == nullptr)))
    {
        status = ExfatStatus::Refused;
    }
    File file{};
    if (status == ExfatStatus::Done)
    {
        status = Resolve(object, file);
    }
    if ((status == ExfatStatus::Done) && file.Directory)
    {
        return ExfatStatus::Refused;
    }
    // Bytes past the valid data length read as zeros.
    const UInt64 start = page << 12U;
    const UInt64 valid = ((status == ExfatStatus::Done) && (file.Valid > start))
                             ? (((file.Valid - start) < ExfatPageBytes) ? (file.Valid - start) : ExfatPageBytes)
                             : 0U;
    if (status == ExfatStatus::Done)
    {
        status = Transfer(file, page, static_cast<UInt32>((valid + (ExfatSectorBytes - 1U)) >> 9U), buffer, nullptr);
    }
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    for (Size index{static_cast<Size>(valid)}; index < ExfatPageBytes; ++index)
    {
        buffer[index] = 0U;
    }
    return ExfatStatus::Done;
}
// exFAT File System Specification revision 1.00, sections 6.4 (secondary
// allocation), 7.4/7.6/7.7 (File, Stream Extension and UTF-16 File Name entries).
ExfatStatus ExfatVolume::SelectDirectory(const ExfatObject& directory) noexcept
{
    File file{};
    auto status = Usable();
    if (status == ExfatStatus::Done)
    {
        status = Resolve(directory, file);
    }
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if (!file.Directory || file.Session || !IsCluster(file.First))
    {
        return ExfatStatus::Refused;
    }
    m_directory = file;
    return ExfatStatus::Done;
}
ExfatStatus ExfatVolume::DirectorySector(const UInt32 ordinal, UInt32& cluster, UInt64& lba) noexcept
{
    if (m_directory.Size != 0U && (UInt64{ordinal} << 9U) >= m_directory.Size)
    {
        return ExfatStatus::NotFound;
    }
    if (m_directory.Contiguous)
    {
        cluster = m_directory.First + (ordinal >> m_geometry.ClusterShift);
        if (!IsCluster(cluster))
        {
            return ExfatStatus::Corrupt;
        }
        lba = ClusterSector(cluster) + (ordinal & ((1U << m_geometry.ClusterShift) - 1U));
        return ExfatStatus::Done;
    }
    return NextSector(ordinal, cluster, lba);
}
ExfatStatus ExfatVolume::ReadDirectory(const ExfatObject& directory, UInt64& cursor, UInt8* name, Size& length,
                                       ExfatObject& object) noexcept
{
    auto status = SelectDirectory(directory);
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if ((name == nullptr) || (cursor >= MaximumDirectorySectors * EntriesPerSector))
    {
        return ExfatStatus::NotFound;
    }
    Scan scan{.Target = NoEntry, .Enumerate = true, .Cursor = static_cast<UInt32>(cursor), .Output = name};
    status = ScanDirectory(scan);
    if (status != ExfatStatus::Done)
    {
        return status;
    }
    if (scan.Corrupt)
    {
        return ExfatStatus::Corrupt;
    }
    if (scan.Found == NoEntry)
    {
        return ExfatStatus::NotFound;
    }
    object = {(UInt64{directory.Identifier == RootIdentifier ? 0U : m_directory.First} << 32U) | scan.Found, m_serial,
              m_directory.Size, m_directory.Contiguous};
    length = scan.LengthOut;
    cursor = scan.Found + scan.Match.Secondaries + 1U;
    return ExfatStatus::Done;
}
// exFAT File System Specification revision 1.00 sections 7.2.2/7.2.5:
// checksum and compressed UTF-16 up-case table. Separate sector scratch
// preserves the directory scanner. All walks have the format's 65536 bound.
ExfatStatus ExfatVolume::ReadUpcaseSector(const UInt32 ordinal, UInt32& cluster) noexcept
{
    const UInt32 mask = (1U << m_geometry.ClusterShift) - 1U;
    if (ordinal != 0U && (ordinal & mask) == 0U)
    {
        if (!m_device.ReadSectors(m_device.Context, m_geometry.FatStart + (cluster >> 7U), 1U, m_upcaseSector))
        {
            return Fail();
        }
        cluster = Load32(m_upcaseSector + ((cluster & 127U) << 2U));
    }
    if (!IsCluster(cluster))
    {
        return ExfatStatus::Corrupt;
    }
    return m_device.ReadSectors(m_device.Context, ClusterSector(cluster) + (ordinal & mask), 1U, m_upcaseSector)
               ? ExfatStatus::Done
               : Fail();
}
ExfatStatus ExfatVolume::VerifyUpcase() noexcept
{
    if (m_upcaseVerified)
    {
        return ExfatStatus::Done;
    }
    if (!IsCluster(m_upcase) || m_upcaseBytes < 256U || m_upcaseBytes > 131072U || (m_upcaseBytes & 1U) != 0U)
    {
        return ExfatStatus::Corrupt;
    }
    UInt32 checksum{0U};
    UInt32 cluster{m_upcase};
    for (UInt32 at{0U}; at < m_upcaseBytes; ++at)
    {
        if ((at & 511U) == 0U)
        {
            const auto status = ReadUpcaseSector(at >> 9U, cluster);
            if (status != ExfatStatus::Done)
            {
                return status;
            }
        }
        checksum = ((checksum & 1U) << 31U) + (checksum >> 1U) + m_upcaseSector[at & 511U];
    }
    if (checksum != m_upcaseChecksum)
    {
        return ExfatStatus::Corrupt;
    }
    m_upcaseVerified = true;
    return ExfatStatus::Done;
}
ExfatStatus ExfatVolume::Upper(const Foundation::UInt16 unit, Foundation::UInt16& upper) noexcept
{
    upper = unit;
    if (unit < 0x80U)
    {
        upper = static_cast<Foundation::UInt16>(ExfatFormat::ToUpper(unit));
        return ExfatStatus::Done;
    }
    for (Size n{0U}; n < m_upperCount; ++n)
    {
        if (m_upperCache[n].Lower == unit)
        {
            upper = m_upperCache[n].Upper;
            return ExfatStatus::Done;
        }
    }
    const auto verified = VerifyUpcase();
    if (verified != ExfatStatus::Done)
    {
        return verified;
    }
    UInt32 point{0U};
    bool skip{false};
    bool found{false};
    UInt32 cluster{m_upcase};
    for (UInt32 at{0U}; UInt64{at} * 2U < m_upcaseBytes && point <= unit; ++at)
    {
        if ((at & 255U) == 0U)
        {
            const auto status = ReadUpcaseSector(at >> 8U, cluster);
            if (status != ExfatStatus::Done)
            {
                return status;
            }
        }
        const auto value = Load16(m_upcaseSector + ((at & 255U) << 1U));
        if (skip)
        {
            if (value == 0U || value > 65536U - point)
            {
                return ExfatStatus::Corrupt;
            }
            point += value;
            skip = false;
        }
        else if (value == 0xFFFFU && point != 0xFFFFU)
        {
            skip = true;
        }
        else
        {
            if (point == unit)
            {
                upper = static_cast<Foundation::UInt16>(value);
                found = true;
                break;
            }
            ++point;
        }
    }
    if ((!found && point <= unit) || skip)
    {
        return ExfatStatus::Corrupt;
    }
    if (m_upperCount < sizeof(m_upperCache) / sizeof(m_upperCache[0U]))
    {
        m_upperCache[m_upperCount++] = {unit, upper};
    }
    return ExfatStatus::Done;
}
} // Namespace Asteria::User::Filesystem
