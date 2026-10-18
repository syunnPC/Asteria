// SPDX-License-Identifier: Apache-2.0

#include "User/Filesystem/Fat/FatVolume.hpp"

namespace Asteria::User::Filesystem
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;

constexpr UInt64 NoSector{~UInt64{0U}};
constexpr UInt32 NoEntry{~UInt32{0U}};
constexpr UInt32 UnknownCount{~UInt32{0U}};
/// A directory holds at most 65536 entries (4096 sectors), the FAT
/// specification's bound; a FAT32 root chain is not followed further.
constexpr UInt32 MaximumDirectorySectors{4096U};
constexpr UInt32 Deleted{0xE5U};
constexpr UInt32 LongNameAttributes{0x0FU};
constexpr UInt32 VolumeLabel{0x08U};
constexpr UInt32 Directory{0x10U};
constexpr UInt8 Archive{0x20U};
constexpr UInt32 LastLongEntry{0x40U};
constexpr UInt32 MaximumLongEntries{20U};
constexpr UInt32 LongNameCharacters{13U};
constexpr UInt8 LowerBase{0x08U};
constexpr UInt8 LowerExtension{0x10U};
constexpr UInt32 FsInfoLead{0x4161'5252U};
constexpr UInt32 FsInfoStructure{0x6141'7272U};
constexpr UInt32 FsInfoTrail{0xAA55'0000U};
/// The timestamp of created and written entries until user space has a clock: 00:00:00.
constexpr UInt32 FixedDate{((2026U - 1980U) << 9U) | (1U << 5U) | 1U};
constexpr UInt32 FixedTime{0U};
/// The byte offsets of a long-name entry's 13 UCS-2 characters.
constexpr UInt8 LongNameOffsets[LongNameCharacters]{1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
/// DOS device names: CON, PRN, AUX, NUL, then COM1-9 and LPT1-9.
constexpr UInt8 DeviceNames[6U][3U]{{'c', 'o', 'n'}, {'p', 'r', 'n'}, {'a', 'u', 'x'},
                                    {'n', 'u', 'l'}, {'c', 'o', 'm'}, {'l', 'p', 't'}};

[[nodiscard]] UInt32 Load16(const UInt8* const bytes) noexcept
{
    return static_cast<UInt32>(bytes[0U]) | (static_cast<UInt32>(bytes[1U]) << 8U);
}

[[nodiscard]] UInt32 Load32(const UInt8* const bytes) noexcept
{
    return Load16(bytes) | (Load16(bytes + 2U) << 16U);
}

void Store16(UInt8* const bytes, const UInt32 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value);
    bytes[1U] = static_cast<UInt8>(value >> 8U);
}

void Store32(UInt8* const bytes, const UInt32 value) noexcept
{
    Store16(bytes, value);
    Store16(bytes + 2U, value >> 16U);
}

[[nodiscard]] UInt32 Fold(const UInt32 value) noexcept
{
    return ((value >= 'A') && (value <= 'Z')) ? (value + 32U) : value;
}

[[nodiscard]] bool IsLower(const UInt32 value) noexcept
{
    return (value >= 'a') && (value <= 'z');
}

/// The file protocol's name rule (FsFrame.hpp IsFsName).
[[nodiscard]] bool IsValidName(const UInt8* const name, const Size length) noexcept
{
    return ValidName(name, length) && !((length == 1U) && (name[0U] == '.')) &&
           !((length == 2U) && (name[0U] == '.') && (name[1U] == '.'));
}

/// A name whose base (before its first '.') is a DOS device.
[[nodiscard]] bool IsDeviceName(const UInt8* const name, const Size length) noexcept
{
    Size base{0U};
    while ((base < length) && (name[base] != '.'))
    {
        ++base;
    }
    const bool numbered = (base == 4U) && (name[3U] >= '1') && (name[3U] <= '9');
    const Size first = numbered ? 4U : 0U;
    const Size last = numbered ? 6U : ((base == 3U) ? 4U : 0U);
    for (Size device{first}; device < last; ++device)
    {
        if ((name[0U] == DeviceNames[device][0U]) && (name[1U] == DeviceNames[device][1U]) &&
            (name[2U] == DeviceNames[device][2U]))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool MatchShortEntry(const UInt8* const entry, const UInt8* const name, const Size length) noexcept
{
    UInt8 rendered[12U]{};
    if (RenderFatShortName(entry, rendered) != length)
    {
        return false;
    }
    for (Size index{0U}; index < length; ++index)
    {
        if (Fold(rendered[index]) != Fold(name[index]))
        {
            return false;
        }
    }
    return true;
}
} // Namespace

FatStatus ReadFatGeometry(const UInt8* const boot, const UInt64 deviceSectors, FatGeometry& geometry) noexcept
{
    const UInt32 perCluster{boot[13U]};
    const UInt32 reserved = Load16(boot + 14U);
    const UInt32 fats{boot[16U]};
    const UInt32 rootEntries = Load16(boot + 17U);
    const UInt32 total16 = Load16(boot + 19U);
    const UInt64 total = (total16 != 0U) ? total16 : Load32(boot + 32U);
    const UInt32 fat16Size = Load16(boot + 22U);
    const UInt64 fatSize = (fat16Size != 0U) ? fat16Size : Load32(boot + 36U);
    const bool jump = ((boot[0U] == 0xEBU) && (boot[2U] == 0x90U)) || (boot[0U] == 0xE9U);
    const UInt64 rootStart = reserved + ((fats == 2U) ? (fatSize << 1U) : fatSize);
    const UInt64 dataStart = rootStart + ((rootEntries + 15U) >> 4U);
    if (!jump || (Load16(boot + 11U) != FatSectorBytes) || (perCluster == 0U) ||
        ((perCluster & (perCluster - 1U)) != 0U) || (reserved == 0U) || (fats == 0U) || (fats > 2U) || (total == 0U) ||
        (total > deviceSectors) || (fatSize == 0U) || (dataStart >= total))
    {
        return FatStatus::NotFat;
    }
    UInt8 shift{0U};
    while ((1U << shift) < perCluster)
    {
        ++shift;
    }
    const UInt64 clusters = (total - dataStart) >> shift;
    const FatType type = (clusters < 4085U) ? FatType::Fat12 : ((clusters < 65525U) ? FatType::Fat16 : FatType::Fat32);
    const UInt64 entries = clusters + 2U;
    UInt64 fatBytes = entries << 2U;
    if (type == FatType::Fat12)
    {
        fatBytes = entries + ((entries + 1U) >> 1U);
    }
    else if (type == FatType::Fat16)
    {
        fatBytes = entries << 1U;
    }
    const UInt32 extended = Load16(boot + 40U);
    const UInt32 active = extended & 0x0FU;
    const bool single = (type == FatType::Fat32) && ((extended & 0x80U) != 0U);
    const UInt32 rootCluster = Load32(boot + 44U);
    const UInt32 fsInfo = Load16(boot + 48U);
    if (((type == FatType::Fat32) != (fat16Size == 0U)) || ((fatSize << 9U) < fatBytes) ||
        ((type == FatType::Fat32) ? ((rootEntries != 0U) || (clusters > 0x0FFF'FFF5U) || (rootCluster < 2U) ||
                                     (rootCluster > (clusters + 1U)) || (single && (active >= fats)))
                                  : (rootEntries == 0U)))
    {
        return FatStatus::Corrupt;
    }
    geometry =
        FatGeometry{.FatStart = reserved + ((single && (active == 1U)) ? fatSize : 0U),
                    .FatSectors = fatSize,
                    .RootStart = (type == FatType::Fat32) ? 0U : rootStart,
                    .DataStart = dataStart,
                    .Clusters = static_cast<UInt32>(clusters),
                    .RootEntries = rootEntries,
                    .RootCluster = (type == FatType::Fat32) ? rootCluster : 0U,
                    .FsInfoSector = ((type == FatType::Fat32) && (fsInfo != 0U) && (fsInfo < reserved)) ? fsInfo : 0U,
                    .ClusterShift = shift,
                    .FatCount = static_cast<UInt8>(fats),
                    .Type = type,
                    .SingleActiveFat = single};
    return FatStatus::Done;
}

UInt8 ComputeFatNameChecksum(const UInt8* const shortName) noexcept
{
    UInt32 sum{0U};
    for (Size index{0U}; index < 11U; ++index)
    {
        sum = (((sum & 1U) << 7U) + (sum >> 1U) + shortName[index]) & 0xFFU;
    }
    return static_cast<UInt8>(sum);
}

Size RenderFatShortName(const UInt8* const entry, UInt8 (&name)[12]) noexcept
{
    Size length{0U};
    for (Size part{0U}; part < 2U; ++part)
    {
        const Size first = (part == 0U) ? 0U : 8U;
        const Size end = (part == 0U) ? 8U : 11U;
        Size last{first};
        for (Size index{first}; index < end; ++index)
        {
            last = (entry[index] != ' ') ? (index + 1U) : last;
        }
        if ((part == 1U) && (last > first))
        {
            name[length++] = '.';
        }
        const bool lower = (entry[12U] & ((part == 0U) ? LowerBase : LowerExtension)) != 0U;
        for (Size index{first}; index < last; ++index)
        {
            const UInt32 value = ((index == 0U) && (entry[0U] == 0x05U)) ? Deleted : entry[index];
            name[length++] = static_cast<UInt8>(lower ? Fold(value) : value);
        }
    }
    return length;
}

/// One pass over the root directory (ScanDirectory). In: Name (nullptr to
/// match nothing) and Length; Short, an 11-byte short name whose collisions to
/// find (nullptr for none), with Tilde the index of its '~' (8 when it has
/// none, then only an equal name collides); Run, the free entries wanted in
/// one sector (0 for none). Out: Found, the matching file's short entry index;
/// RunStart and RunLba, the first free run's first index and sector; End and
/// EndLba, the index and sector of the end mark (End the directory's capacity
/// when it has none); Tildes, bit n set when Short with '~n' names a file;
/// Collision, Short equals a file's short name.
struct FatVolume::Scan
{
    const UInt8* Name;
    Size Length;
    const UInt8* Short;
    UInt32 Tilde;
    UInt32 Run;
    UInt32 Found;
    UInt32 RunStart;
    UInt64 RunLba;
    UInt32 End;
    UInt64 EndLba;
    UInt32 Tildes;
    bool Collision;
    bool Enumerate{false};
    UInt32 Cursor{0U};
    UInt8* Output{nullptr};
    Size LengthOut{0U};
};

FatStatus FatVolume::Fail() noexcept
{
    m_state = State::Broken;
    return FatStatus::Failed;
}

FatStatus FatVolume::Usable() const noexcept
{
    if (m_state == State::Mounted)
    {
        return FatStatus::Done;
    }
    return (m_state == State::Broken) ? FatStatus::Failed : FatStatus::Refused;
}

bool FatVolume::IsMounted() const noexcept
{
    return m_state == State::Mounted;
}

bool FatVolume::IsReadOnly() const noexcept
{
    return m_readOnly;
}

const FatGeometry& FatVolume::GetGeometry() const noexcept
{
    return m_geometry;
}

bool FatVolume::IsCluster(const UInt32 value) const noexcept
{
    return (value >= 2U) && (value <= (m_geometry.Clusters + 1U));
}

bool FatVolume::InSession(const UInt32 cluster) const noexcept
{
    for (Size index{0U}; index < m_chainLength; ++index)
    {
        if (m_chain[index] == cluster)
        {
            return true;
        }
    }
    return false;
}

UInt64 FatVolume::ClusterSector(const UInt32 cluster) const noexcept
{
    return m_geometry.DataStart + (static_cast<UInt64>(cluster - 2U) << m_geometry.ClusterShift);
}

FatStatus FatVolume::Load(const UInt64 lba) noexcept
{
    if (lba == m_cached)
    {
        return FatStatus::Done;
    }
    const FatStatus status = Flush();
    if (status != FatStatus::Done)
    {
        return status;
    }
    m_cached = NoSector;
    if (!m_device.ReadSectors(m_device.Context, lba, 1U, m_sector))
    {
        return Fail();
    }
    m_cached = lba;
    return FatStatus::Done;
}

FatStatus FatVolume::Store(const UInt64 lba) noexcept
{
    return m_device.WriteSectors(m_device.Context, lba, 1U, m_sector) ? FatStatus::Done : Fail();
}

FatStatus FatVolume::Flush() noexcept
{
    if (!m_dirtyFat)
    {
        return FatStatus::Done;
    }
    m_dirtyFat = false;
    UInt64 lba{m_cached};
    for (UInt32 copy{0U}; copy < m_geometry.FatCount; ++copy)
    {
        const FatStatus status = Store(lba);
        if (status != FatStatus::Done)
        {
            return status;
        }
        lba += m_geometry.FatSectors;
    }
    return FatStatus::Done;
}

FatStatus FatVolume::GetEntry(const UInt32 cluster, UInt32& value) noexcept
{
    return Entry(cluster, value, false);
}

FatStatus FatVolume::SetEntry(const UInt32 cluster, const UInt32 value) noexcept
{
    UInt32 written{value};
    return Entry(cluster, written, true);
}

FatStatus FatVolume::Entry(const UInt32 cluster, UInt32& value, const bool write) noexcept
{
    const FatType type{m_geometry.Type};
    const UInt64 offset = (type == FatType::Fat12)
                              ? (cluster + (cluster >> 1U))
                              : (static_cast<UInt64>(cluster) << ((type == FatType::Fat16) ? 1U : 2U));
    const UInt64 lba = m_geometry.FatStart + (offset >> 9U);
    const UInt32 at = static_cast<UInt32>(offset & 511U);
    FatStatus status = Load(lba);
    if (status != FatStatus::Done)
    {
        return status;
    }
    if (type != FatType::Fat12)
    {
        const bool wide = type == FatType::Fat32;
        const UInt32 old = wide ? Load32(m_sector + at) : Load16(m_sector + at);
        if (write && wide)
        {
            // The high nibble of a FAT32 entry is reserved and kept.
            Store32(m_sector + at, (old & 0xF000'0000U) | (value & 0x0FFF'FFFFU));
        }
        else if (write)
        {
            Store16(m_sector + at, value);
        }
        m_dirtyFat = m_dirtyFat || write;
        value = wide ? (old & 0x0FFF'FFFFU) : old;
        return FatStatus::Done;
    }
    // FAT12: the 16-bit word that holds the entry, whose second byte starts
    // the next sector for an entry at byte 511.
    const UInt32 shift = ((cluster & 1U) != 0U) ? 4U : 0U;
    const UInt32 mask = 0xFFFU << shift;
    UInt32 word{m_sector[at]};
    status = (at == 511U) ? Load(lba + 1U) : FatStatus::Done;
    word |= static_cast<UInt32>(m_sector[(at + 1U) & 511U]) << 8U;
    const UInt32 updated = (word & ~mask) | ((value << shift) & mask);
    if ((status != FatStatus::Done) || !write)
    {
        value = (word & mask) >> shift;
        return status;
    }
    if (at != 511U)
    {
        Store16(m_sector + at, updated);
        m_dirtyFat = true;
        return FatStatus::Done;
    }
    // The entry straddles two FAT sectors, each written on its own. Write
    // first the sector that leaves the entry's half-written value free, an
    // End mark or a cluster: for allocating (0 to a value) or freeing (a value to 0) one of the two
    // orders always does.
    const UInt32 half = ((((updated & 0xFFU) | (word & 0xFF00U)) & mask) >> shift);
    const bool lowFirst = (half == 0U) || IsCluster(half) || (half >= 0xFF8U);
    for (UInt32 pass{0U}; (status == FatStatus::Done) && (pass < 2U); ++pass)
    {
        const bool lowSector = (pass == 0U) == lowFirst;
        status = Load(lowSector ? lba : (lba + 1U));
        m_sector[lowSector ? 511U : 0U] = static_cast<UInt8>(lowSector ? updated : (updated >> 8U));
        m_dirtyFat = true;
        status = ((status == FatStatus::Done) && (pass == 0U)) ? Flush() : status;
    }
    return status;
}

FatStatus FatVolume::Allocate(UInt32& cluster) noexcept
{
    UInt32 candidate{m_hint};
    for (UInt32 step{0U}; step < m_geometry.Clusters; ++step, ++candidate)
    {
        if (!IsCluster(candidate))
        {
            candidate = 2U;
        }
        UInt32 value{0U};
        const FatStatus status = GetEntry(candidate, value);
        if (status != FatStatus::Done)
        {
            return status;
        }
        if ((value == 0U) && !InSession(candidate))
        {
            m_hint = candidate + 1U;
            cluster = candidate;
            return FatStatus::Done;
        }
    }
    return FatStatus::Refused;
}

FatStatus FatVolume::SetDirty(const bool dirty) noexcept
{
    // PowerOff.md: durable data before the clean marker, durable marker
    // before acknowledgement. Never report a failed device flush clean.
    if (!dirty && m_device.Synchronize != nullptr && !m_device.Synchronize(m_device.Context))
    {
        return Fail();
    }
    if (dirty == m_dirtySet)
    {
        return FatStatus::Done;
    }
    FatStatus status{FatStatus::Done};
    if (m_geometry.Type == FatType::Fat12)
    {
        // FAT12 has no clean bit: bit 0 of the boot sector's flags byte.
        status = Load(0U);
        if (status != FatStatus::Done)
        {
            return status;
        }
        m_sector[0x25U] = static_cast<UInt8>(dirty ? (m_sector[0x25U] | 1U) : (m_sector[0x25U] & 0xFEU));
        status = Store(0U);
    }
    else
    {
        const UInt32 clean = (m_geometry.Type == FatType::Fat32) ? 0x0800'0000U : 0x8000U;
        UInt32 value{0U};
        status = GetEntry(1U, value);
        if (status == FatStatus::Done)
        {
            status = SetEntry(1U, dirty ? (value & ~clean) : (value | clean));
        }
        if (status == FatStatus::Done)
        {
            status = Flush();
        }
    }
    if (status == FatStatus::Done && m_device.Synchronize != nullptr && !m_device.Synchronize(m_device.Context))
    {
        return Fail();
    }
    m_dirtySet = (status == FatStatus::Done) ? dirty : m_dirtySet;
    return status;
}

FatStatus FatVolume::WriteFsInfo() noexcept
{
    const UInt64 lba{m_geometry.FsInfoSector};
    if (lba == 0U)
    {
        return FatStatus::Done;
    }
    const FatStatus status = Load(lba);
    if (status != FatStatus::Done)
    {
        return status;
    }
    Store32(m_sector + 488U, m_freeCount);
    // The next-free hint retains the last allocated cluster as the next search origin.
    Store32(m_sector + 492U, m_chain[m_chainLength - 1U]);
    return Store(lba);
}

FatStatus FatVolume::WriteDirectory() noexcept
{
    const FatStatus status = Load(m_sessionLba);
    if (status != FatStatus::Done)
    {
        return status;
    }
    UInt8* entry = m_sector + (static_cast<Size>(m_slot) << 5U);
    if (m_session == Session::Create)
    {
        const UInt8 checksum = ComputeFatNameChecksum(m_shortName);
        for (UInt32 written{0U}; written < m_longEntries; ++written, entry += 32U)
        {
            const UInt32 order = m_longEntries - written;
            entry[0U] = static_cast<UInt8>(order | ((written == 0U) ? LastLongEntry : 0U));
            UInt32 position = (order - 1U) * LongNameCharacters;
            for (UInt32 character{0U}; character < LongNameCharacters; ++character, ++position)
            {
                UInt32 value{0xFFFFU};
                if (position < m_nameLength)
                {
                    value = m_name[position];
                }
                else if (position == m_nameLength)
                {
                    value = 0U;
                }
                Store16(entry + LongNameOffsets[character], value);
            }
            entry[11U] = static_cast<UInt8>(LongNameAttributes);
            entry[12U] = 0U;
            entry[13U] = checksum;
            Store16(entry + 26U, 0U);
        }
        for (Size index{0U}; index < 32U; ++index)
        {
            entry[index] = (index < 11U) ? m_shortName[index] : UInt8{0U};
        }
        entry[11U] = m_directoryKind ? Directory : Archive;
        entry[12U] = m_caseFlags;
        Store16(entry + 14U, FixedTime);
        Store16(entry + 16U, FixedDate);
        Store16(entry + 18U, FixedDate);
        // The set took the end mark's place: what followed it is unused.
        for (UInt8* tail = entry + 32U; m_zeroTail && (tail < (m_sector + FatSectorBytes)); ++tail)
        {
            *tail = 0U;
        }
    }
    const UInt32 first = (m_chainLength != 0U) ? m_chain[0U] : 0U;
    Store16(entry + 20U, (m_geometry.Type == FatType::Fat32) ? (first >> 16U) : 0U);
    Store16(entry + 22U, FixedTime);
    Store16(entry + 24U, FixedDate);
    Store16(entry + 26U, first);
    Store32(entry + 28U, m_sessionSize);
    return Store(m_sessionLba);
}

FatStatus FatVolume::FreeOldChain() noexcept
{
    UInt32 cluster{m_oldFirst};
    for (UInt32 index{0U}; (index < m_oldClusters) && IsCluster(cluster); ++index)
    {
        UInt32 next{0U};
        FatStatus status = GetEntry(cluster, next);
        if (status == FatStatus::Done)
        {
            status = SetEntry(cluster, 0U);
        }
        if (status != FatStatus::Done)
        {
            return status;
        }
        cluster = next;
    }
    return Flush();
}

FatStatus FatVolume::NextSector(const UInt32 ordinal, UInt32& cluster, UInt64& lba) noexcept
{
    if (ordinal >= MaximumDirectorySectors)
    {
        return FatStatus::NotFound;
    }
    if ((m_directory == 0U) && (m_geometry.Type != FatType::Fat32))
    {
        if (ordinal >= ((m_geometry.RootEntries + 15U) >> 4U))
        {
            return FatStatus::NotFound;
        }
        lba = m_geometry.RootStart + ordinal;
        return FatStatus::Done;
    }
    const UInt32 mask = (1U << m_geometry.ClusterShift) - 1U;
    if ((ordinal != 0U) && ((ordinal & mask) == 0U))
    {
        UInt32 next{0U};
        const FatStatus status = GetEntry(cluster, next);
        if (status != FatStatus::Done)
        {
            return status;
        }
        if (!IsCluster(next))
        {
            return (next >= ((m_geometry.Type == FatType::Fat12)
                                 ? 0xFF8U
                                 : ((m_geometry.Type == FatType::Fat16) ? 0xFFF8U : 0x0FFF'FFF8U)))
                       ? FatStatus::NotFound
                       : FatStatus::Corrupt;
        }
        cluster = next;
    }
    lba = ClusterSector(cluster) + (ordinal & mask);
    return FatStatus::Done;
}

FatStatus FatVolume::ScanDirectory(Scan& scan) noexcept
{
    scan.Found = NoEntry;
    scan.RunStart = NoEntry;
    scan.End = NoEntry;
    scan.Tildes = 0U;
    scan.Collision = false;
    UInt32 cluster{m_directory != 0U ? m_directory : m_geometry.RootCluster};
    UInt64 lba{0U};
    UInt32 index{0U};
    // The open long-name set: the ordinal of its last entry (0 for none), its
    // checksum and whether it agrees with Name so far.
    UInt32 sequence{0U};
    UInt32 checksum{0U};
    bool longMatch{false};
    UInt32 nameUnits{0U};
    for (UInt32 ordinal{0U};; ++ordinal)
    {
        FatStatus status = NextSector(ordinal, cluster, lba);
        if (status == FatStatus::NotFound)
        {
            break;
        }
        if (status == FatStatus::Done)
        {
            status = Load(lba);
        }
        if (status != FatStatus::Done)
        {
            return status;
        }
        UInt32 run{0U};
        for (UInt32 slot{0U}; slot < 16U; ++slot, ++index)
        {
            const UInt8* const entry = m_sector + (slot << 5U);
            const UInt32 first{entry[0U]};
            if ((scan.End == NoEntry) && (first == 0U))
            {
                scan.End = index;
                scan.EndLba = lba;
            }
            if ((scan.End != NoEntry) || (first == Deleted))
            {
                sequence = 0U;
                ++run;
                if ((run == scan.Run) && (scan.RunStart == NoEntry))
                {
                    scan.RunStart = index + 1U - run;
                    scan.RunLba = lba;
                }
                continue;
            }
            run = 0U;
            const UInt32 attributes{entry[11U]};
            if ((attributes & 0x3FU) == LongNameAttributes)
            {
                const UInt32 order = first & ~LastLongEntry;
                if ((order == 0U) || (order > MaximumLongEntries))
                {
                    sequence = 0U;
                }
                else if ((first & LastLongEntry) != 0U)
                {
                    sequence = order;
                    nameUnits = order * LongNameCharacters;
                    checksum = entry[13U];
                    longMatch = (scan.Name != nullptr) && ((order * LongNameCharacters) >= scan.Length);
                }
                else
                {
                    sequence = ((order + 1U) == sequence) && (entry[13U] == checksum) ? order : 0U;
                }
                if (sequence != 0U)
                {
                    for (UInt32 at{0U}; at < LongNameCharacters; ++at)
                    {
                        const auto pos = (sequence - 1U) * LongNameCharacters + at;
                        m_scanName[pos] = static_cast<Foundation::UInt16>(Load16(entry + LongNameOffsets[at]));
                    }
                }
                continue;
            }
            const bool hasLong = (sequence == 1U) && (checksum == ComputeFatNameChecksum(entry));
            sequence = 0U;
            Size rendered{0U};
            if (hasLong)
            {
                Size count{0U};
                while ((count < nameUnits) && (m_scanName[count] != 0U) && (m_scanName[count] != 0xFFFFU))
                {
                    ++count;
                }
                rendered = FromUtf16(m_scanName, count, m_rendered);
                longMatch = (scan.Name != nullptr) && SameName(m_rendered, rendered, scan.Name, scan.Length);
            }
            else
            {
                UInt8 shortText[12U]{};
                rendered = RenderFatShortName(entry, shortText);
                for (Size at{0U}; at < rendered; ++at)
                {
                    m_rendered[at] = shortText[at];
                }
            }
            if ((attributes & VolumeLabel) != 0U)
            {
                continue;
            }
            if (scan.Short != nullptr)
            {
                // A '~' name compares all but its digit.
                const UInt32 skip = (scan.Tilde < 8U) ? (scan.Tilde + 1U) : 11U;
                bool same{true};
                for (UInt32 position{0U}; position < 11U; ++position)
                {
                    same = same && ((position == skip) || (entry[position] == scan.Short[position]));
                }
                const UInt32 digit = (scan.Tilde < 8U) ? entry[scan.Tilde + 1U] : 0U;
                if (same && (digit >= '1') && (digit <= '9'))
                {
                    scan.Tildes |= 1U << (digit - '0');
                }
                scan.Collision = scan.Collision || (same && (scan.Tilde >= 8U));
            }
            if ((scan.Name != nullptr) && (scan.Found == NoEntry) &&
                (hasLong ? longMatch : MatchShortEntry(entry, scan.Name, scan.Length)))
            {
                scan.Found = index;
            }
            if (scan.Enumerate && Text::ValidUtf8(m_rendered, rendered, true) && (index >= scan.Cursor) &&
                (scan.Found == NoEntry) && (rendered != 0U) && (entry[0U] != '.'))
            {
                scan.Found = index;
                scan.LengthOut = rendered;
                for (Size at{0U}; at < rendered; ++at)
                {
                    scan.Output[at] = m_rendered[at];
                }
            }
        }
        if ((scan.End != NoEntry) && ((scan.Run == 0U) || (scan.RunStart != NoEntry)))
        {
            break;
        }
    }
    scan.End = (scan.End == NoEntry) ? index : scan.End;
    return FatStatus::Done;
}

FatStatus FatVolume::Mount(const FatBlockDevice& device, const UInt64 serial) noexcept
{
    m_state = State::Unmounted;
    m_session = Session::None;
    m_chainLength = 0U;
    m_oldClusters = 0U;
    m_dirtyFat = false;
    m_dirtySet = false;
    m_readOnly = true;
    m_cached = NoSector;
    m_device = device;
    m_serial = serial;
    FatStatus status = Load(0U);
    FatGeometry geometry{};
    if (status == FatStatus::Done)
    {
        status = ReadFatGeometry(m_sector, device.Sectors, geometry);
    }
    if (status != FatStatus::Done)
    {
        return status;
    }
    m_geometry = geometry;
    m_directory = 0U;
    m_directoryKind = false;
    const bool fat32 = geometry.Type == FatType::Fat32;
    bool dirty = (m_sector[fat32 ? 0x41U : 0x25U] & 1U) != 0U;
    if (geometry.Type != FatType::Fat12)
    {
        // FAT[1]'s clean and no-hard-error bits.
        const UInt32 bits = fat32 ? 0x0C00'0000U : 0xC000U;
        UInt32 value{0U};
        status = GetEntry(1U, value);
        if (status != FatStatus::Done)
        {
            return status;
        }
        dirty = dirty || ((value & bits) != bits);
    }
    m_readOnly = dirty || geometry.SingleActiveFat;
    m_freeCount = UnknownCount;
    m_hint = 2U;
    if (m_geometry.FsInfoSector != 0U)
    {
        status = Load(m_geometry.FsInfoSector);
        if (status != FatStatus::Done)
        {
            return status;
        }
        const UInt32 free = Load32(m_sector + 488U);
        const UInt32 next = Load32(m_sector + 492U);
        if ((Load32(m_sector) != FsInfoLead) || (Load32(m_sector + 484U) != FsInfoStructure) ||
            (Load32(m_sector + 508U) != FsInfoTrail))
        {
            // Not an FSInfo sector: never written.
            m_geometry.FsInfoSector = 0U;
        }
        m_freeCount = (free <= geometry.Clusters) ? free : UnknownCount;
        m_hint = IsCluster(next) ? next : 2U;
    }
    Scan scan{.Name = nullptr,
              .Length = 0U,
              .Short = nullptr,
              .Tilde = 8U,
              .Run = 0U,
              .Found = 0U,
              .RunStart = 0U,
              .RunLba = 0U,
              .End = 0U,
              .EndLba = 0U,
              .Tildes = 0U,
              .Collision = false};
    status = ScanDirectory(scan);
    if (status != FatStatus::Done)
    {
        return status;
    }
    m_end = scan.End;
    m_state = State::Mounted;
    return FatStatus::Done;
}

FatStatus FatVolume::Open(const UInt8* const name, const Size length, FatObject& object) noexcept
{
    return Lookup(Root(), name, length, object);
}

FatStatus FatVolume::Lookup(const FatObject& directory, const UInt8* const name, const Size length,
                            FatObject& object) noexcept
{
    const FatStatus selected = SelectDirectory(directory);
    if (selected != FatStatus::Done)
    {
        return selected;
    }
    FatStatus status = Usable();
    if ((status == FatStatus::Done) && ((name == nullptr) || (length == 0U) || (length > FatMaximumNameBytes)))
    {
        status = FatStatus::Refused;
    }
    Scan scan{.Name = name,
              .Length = length,
              .Short = nullptr,
              .Tilde = 8U,
              .Run = 0U,
              .Found = 0U,
              .RunStart = 0U,
              .RunLba = 0U,
              .End = 0U,
              .EndLba = 0U,
              .Tildes = 0U,
              .Collision = false};
    if (status == FatStatus::Done)
    {
        status = ScanDirectory(scan);
    }
    if (status != FatStatus::Done)
    {
        return status;
    }
    if (scan.Found == NoEntry)
    {
        return FatStatus::NotFound;
    }
    object = FatObject{.Identifier = (UInt64{m_directory} << 32U) | scan.Found, .Generation = m_serial};
    return FatStatus::Done;
}

FatStatus FatVolume::Create(const UInt8* const name, const Size length, FatObject& object) noexcept
{
    return CreateIn(Root(), name, length, object);
}

FatStatus FatVolume::CreateIn(const FatObject& directory, const UInt8* const name, const Size length, FatObject& object,
                              const bool directoryKind) noexcept
{
    const FatStatus selected = SelectDirectory(directory);
    if (selected != FatStatus::Done)
    {
        return selected;
    }
    FatStatus status = Usable();
    if (status != FatStatus::Done)
    {
        return status;
    }
    if (m_readOnly || (m_session != Session::None) || !IsValidName(name, length) || IsDeviceName(name, length))
    {
        return FatStatus::Refused;
    }
    Size dot{length};
    Size dots{0U};
    for (Size index{0U}; index < length; ++index)
    {
        if (name[index] == '.')
        {
            dot = index;
            ++dots;
        }
    }
    bool ascii{true};
    for (Size at{0U}; at < length; ++at)
    {
        ascii = ascii && (name[at] < 0x80U);
    }
    const Size units = ToUtf16(name, length, m_name);
    const bool fits = ascii && (dots <= 1U) && (dot <= 8U) && ((length - dot) <= 4U);
    // An 8.3 name: the base and extension in uppercase with the NT case flags.
    // A longer one: up to six characters of its base (dots dropped), '~', a
    // digit chosen below, and up to three of its extension. Built in place:
    // with no session, a refusal leaves nothing behind.
    UInt32 put{0U};
    UInt32 flags{0U};
    for (Size index{0U}; index < 11U; ++index)
    {
        m_shortName[index] = ' ';
    }
    for (Size index{0U}; index < length; ++index)
    {
        const UInt32 value{name[index]};

        const bool base = index < dot;
        const Size at = base ? put : (index + 7U - dot);
        if ((value != '.') && (at < (base ? (fits ? 8U : 6U) : 11U)))
        {
            flags |= IsLower(value) ? (base ? LowerBase : LowerExtension) : 0U;
            m_shortName[at] = static_cast<UInt8>(value >= 0x80U ? '_' : (IsLower(value) ? (value - 32U) : value));
            put += base ? 1U : 0U;
        }
    }
    const UInt32 tilde = fits ? 8U : put;
    if (!fits)
    {
        m_shortName[put] = '~';
    }
    const UInt32 longEntries = fits ? 0U : static_cast<UInt32>((units + 12U) / LongNameCharacters);
    Scan scan{.Name = name,
              .Length = length,
              .Short = m_shortName,
              .Tilde = tilde,
              .Run = longEntries + 1U,
              .Found = 0U,
              .RunStart = 0U,
              .RunLba = 0U,
              .End = 0U,
              .EndLba = 0U,
              .Tildes = 0U,
              .Collision = false};
    status = ScanDirectory(scan);
    UInt32 digit{1U};
    while ((digit <= 9U) && ((scan.Tildes & (1U << digit)) != 0U))
    {
        ++digit;
    }
    // FAT volume contract: exhausted aliases are a preflight refusal, before directory growth can
    // dirty or link any clusters.
    if (status == FatStatus::Done && (scan.Found != NoEntry || scan.Collision || (!fits && digit > 9U)))
    {
        return FatStatus::Refused;
    }
    // Component launch: reserve the child's cluster before parent growth. A full volume must not
    // Grow the parent and then refuse the child.
    UInt32 directoryCluster{0U};
    if (status == FatStatus::Done && directoryKind)
    {
        status = Allocate(directoryCluster);
        if (status == FatStatus::Done)
        {
            m_chain[0U] = directoryCluster;
            m_chainLength = 1U;
        }
    }
    if (status == FatStatus::Done && scan.RunStart == NoEntry && scan.Found == NoEntry && !scan.Collision)
    {
        status = GrowDirectory();
        if (status == FatStatus::Done)
        {
            status = ScanDirectory(scan);
        }
    }
    m_chainLength = 0U;
    if (status != FatStatus::Done)
    {
        return status;
    }
    if (scan.RunStart == NoEntry)
    {
        return FatStatus::Refused;
    }
    if (!fits)
    {
        m_shortName[tilde + 1U] = static_cast<UInt8>('0' + digit);
    }
    m_session = Session::Create;
    m_sessionIndex = (UInt64{m_directory} << 32U) | (scan.RunStart + longEntries);
    m_directoryKind = directoryKind;
    m_sessionLba = scan.RunLba;
    m_sessionSize = 0U;
    m_slot = static_cast<UInt8>(scan.RunStart & 15U);
    m_zeroTail = static_cast<UInt32>(m_sessionIndex) >= scan.End;
    // A set in the sector after the end mark's: the end mark's sector's free
    // entries become deleted ones, or the directory would end before it.
    const bool gap = (scan.RunLba != scan.EndLba) && (scan.RunStart > scan.End);
    m_gapLba = gap ? scan.EndLba : 0U;
    m_gapSlot = static_cast<UInt8>(scan.End & 15U);
    m_longEntries = static_cast<UInt8>(longEntries);
    m_caseFlags = fits ? static_cast<UInt8>(flags) : UInt8{0U};
    m_nameLength = static_cast<UInt8>(units);
    m_chainLength = 0U;
    m_oldClusters = 0U;
    if (directoryKind)
    {
        const FatStatus initialized = InitializeDirectory(m_directory, directoryCluster);
        if (initialized != FatStatus::Done)
        {
            m_session = Session::None;
            m_chainLength = 0U;
            return initialized;
        }
    }
    object = FatObject{.Identifier = m_sessionIndex, .Generation = m_serial};
    return FatStatus::Done;
}

FatStatus FatVolume::Resolve(const FatObject& object, File& file) noexcept
{
    if (object.Generation != m_serial)
    {
        return FatStatus::StaleObject;
    }
    if (object.Identifier == RootIdentifier)
    {
        file = File{.Lba = 0U, .First = m_geometry.RootCluster, .Size = 0U, .Session = false, .Directory = true};
        return FatStatus::Done;
    }
    m_directory = static_cast<UInt32>(object.Identifier >> 32U);
    if ((m_directory != 0U) && !IsCluster(m_directory))
    {
        return FatStatus::StaleObject;
    }
    if ((m_session != Session::None) && (object.Identifier == m_sessionIndex))
    {
        file = File{
            .Lba = m_sessionLba, .First = 0U, .Size = m_sessionSize, .Session = true, .Directory = m_directoryKind};
        return FatStatus::Done;
    }
    if (static_cast<UInt32>(object.Identifier) >= MaximumDirectorySectors * 16U)
    {
        return FatStatus::StaleObject;
    }
    const UInt32 index = static_cast<UInt32>(object.Identifier);
    UInt32 cluster{m_directory != 0U ? m_directory : m_geometry.RootCluster};
    UInt64 lba{0U};
    FatStatus status{FatStatus::Done};
    for (UInt32 ordinal{0U}; (status == FatStatus::Done) && (ordinal <= (index >> 4U)); ++ordinal)
    {
        status = NextSector(ordinal, cluster, lba);
    }
    if (status == FatStatus::Done)
    {
        status = Load(lba);
    }
    if (status != FatStatus::Done)
    {
        return (status == FatStatus::NotFound) ? FatStatus::StaleObject : status;
    }
    const UInt8* const entry = m_sector + ((index & 15U) << 5U);
    const UInt32 attributes{entry[11U]};
    if ((entry[0U] == 0U) || (entry[0U] == Deleted) || ((attributes & 0x3FU) == LongNameAttributes) ||
        ((attributes & VolumeLabel) != 0U))
    {
        return FatStatus::StaleObject;
    }
    const UInt32 high = (m_geometry.Type == FatType::Fat32) ? (Load16(entry + 20U) << 16U) : 0U;
    file = File{.Lba = lba,
                .First = high | Load16(entry + 26U),
                .Size = Load32(entry + 28U),
                .Session = false,
                .Directory = (attributes & Directory) != 0U};
    return FatStatus::Done;
}

FatStatus FatVolume::Stat(const FatObject& object, FatFileSize& size) noexcept
{
    File file{};
    FatStatus status = Usable();
    if (status == FatStatus::Done)
    {
        status = Resolve(object, file);
    }
    if (status != FatStatus::Done)
    {
        return status;
    }
    size = FatFileSize{.Pages = (static_cast<UInt64>(file.Size) + (FatPageBytes - 1U)) >> 12U,
                       .Bytes = file.Size,
                       .Kind = file.Directory ? NodeKind::Directory : NodeKind::File};
    return FatStatus::Done;
}

FatStatus FatVolume::MapSector(const File& file, const UInt32 fileSector, UInt64& lba) noexcept
{
    const UInt32 index = fileSector >> m_geometry.ClusterShift;
    if (index >= m_geometry.Clusters)
    {
        return FatStatus::Corrupt;
    }
    UInt32 cluster{file.First};
    if (file.Session)
    {
        if (index >= m_chainLength)
        {
            return FatStatus::Corrupt;
        }
        cluster = m_chain[index];
    }
    for (UInt32 step{0U}; !file.Session && (step <= index); ++step)
    {
        if (!IsCluster(cluster))
        {
            return FatStatus::Corrupt;
        }
        if (step < index)
        {
            const FatStatus status = GetEntry(cluster, cluster);
            if (status != FatStatus::Done)
            {
                return status;
            }
        }
    }
    lba = ClusterSector(cluster) + (fileSector & ((1U << m_geometry.ClusterShift) - 1U));
    return FatStatus::Done;
}

FatStatus FatVolume::Transfer(const File& file, const UInt64 page, const UInt32 sectors, UInt8* const readBuffer,
                              const UInt8* const writeBuffer) noexcept
{
    UInt64 runLba{0U};
    UInt32 runFirst{0U};
    UInt32 runCount{0U};
    for (UInt32 sector{0U}; sector <= sectors; ++sector)
    {
        UInt64 lba{0U};
        if (sector < sectors)
        {
            const FatStatus status = MapSector(file, (static_cast<UInt32>(page) << 3U) + sector, lba);
            if (status != FatStatus::Done)
            {
                return status;
            }
        }
        if ((runCount != 0U) && ((sector == sectors) || (lba != (runLba + runCount))))
        {
            const Size offset = static_cast<Size>(runFirst) << 9U;
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
    return FatStatus::Done;
}

FatStatus FatVolume::ReadPage(const FatObject& object, const UInt64 page, UInt8* const buffer) noexcept
{
    FatStatus status = Usable();
    if ((status == FatStatus::Done) && ((page > 0xFFFFFU) || (buffer == nullptr)))
    {
        status = FatStatus::Refused;
    }
    File file{};
    if (status == FatStatus::Done)
    {
        status = Resolve(object, file);
    }
    if ((status == FatStatus::Done) && file.Directory)
    {
        return FatStatus::Refused;
    }
    const UInt64 start = page << 12U;
    const UInt64 valid = ((status == FatStatus::Done) && (file.Size > start))
                             ? (((file.Size - start) < FatPageBytes) ? (file.Size - start) : FatPageBytes)
                             : 0U;
    if (status == FatStatus::Done)
    {
        status = Transfer(file, page, static_cast<UInt32>((valid + 511U) >> 9U), buffer, nullptr);
    }
    if (status != FatStatus::Done)
    {
        return status;
    }
    for (Size index{static_cast<Size>(valid)}; index < FatPageBytes; ++index)
    {
        buffer[index] = 0U;
    }
    return FatStatus::Done;
}

FatStatus FatVolume::StartReplace(const File& file, const UInt64 index, const UInt64 page) noexcept
{
    FatStatus status{FatStatus::Done};
    const UInt32 shift = m_geometry.ClusterShift + 9U;
    const UInt32 count = static_cast<UInt32>((static_cast<UInt64>(file.Size) + (1U << shift) - 1U) >> shift);
    const UInt32 needed = static_cast<UInt32>((((page + 1U) << 12U) + (1U << shift) - 1U) >> shift);
    UInt32 cluster{file.First};
    m_chainLength = 0U;
    // Reserve the entire replacement and appended page before copying data or dirtying the volume.
    // Refused has no writes.
    while (status == FatStatus::Done && m_chainLength < (count > needed ? count : needed))
    {
        UInt32 fresh{0U};
        status = Allocate(fresh);
        if (status == FatStatus::Done)
        {
            m_chain[m_chainLength++] = fresh;
        }
    }
    if (status == FatStatus::Done)
    {
        status = SetDirty(true);
    }
    for (UInt32 position{0U}; (status == FatStatus::Done) && (position < count); ++position)
    {
        if (!IsCluster(cluster))
        {
            status = FatStatus::Corrupt;
            break;
        }
        const UInt32 fresh = m_chain[position];
        // Copy the sectors the size covers, but those of the page the caller
        // writes next.
        for (UInt32 sector{0U}; sector < (1U << m_geometry.ClusterShift); ++sector)
        {
            const UInt32 fileSector = (position << m_geometry.ClusterShift) + sector;
            if ((static_cast<UInt64>(fileSector) << 9U) >= file.Size)
            {
                break;
            }
            if ((fileSector >> 3U) == page)
            {
                continue;
            }
            status = Load(ClusterSector(cluster) + sector);
            if (status == FatStatus::Done)
            {
                status = Store(ClusterSector(fresh) + sector);
            }
            if (status != FatStatus::Done)
            {
                break;
            }
        }
        if ((status == FatStatus::Done) && ((position + 1U) < count))
        {
            status = GetEntry(cluster, cluster);
        }
    }
    if (status != FatStatus::Done)
    {
        m_chainLength = 0U;
        return status;
    }
    m_session = Session::Replace;
    m_sessionIndex = index;
    m_directoryKind = false;
    m_sessionLba = file.Lba;
    m_sessionSize = file.Size;
    m_slot = static_cast<UInt8>(index & 15U);
    m_oldFirst = file.First;
    m_oldClusters = count;
    return FatStatus::Done;
}

FatStatus FatVolume::ZeroTail() noexcept
{
    const File session{.Lba = m_sessionLba, .First = 0U, .Size = 0U, .Session = true};
    const UInt32 first = m_sessionSize >> 9U;
    const UInt32 end = ((m_sessionSize + 4095U) >> 12U) << 3U;
    FatStatus status{FatStatus::Done};
    for (UInt32 sector{first}; (status == FatStatus::Done) && (sector < end); ++sector)
    {
        UInt64 lba{0U};
        status = MapSector(session, sector, lba);
        if (status == FatStatus::Done)
        {
            status = Load(lba);
        }
        for (UInt32 at{(sector == first) ? (m_sessionSize & 511U) : 0U}; (status == FatStatus::Done) && (at < 512U);
             ++at)
        {
            m_sector[at] = 0U;
        }
        if (status == FatStatus::Done)
        {
            status = Store(lba);
        }
    }
    return status;
}

FatStatus FatVolume::WritePage(const FatObject& object, const UInt64 page, const UInt8* const buffer) noexcept
{
    FatStatus status = Usable();
    if ((status == FatStatus::Done) && (m_readOnly || (page >= FatMaximumFilePages) || (buffer == nullptr)))
    {
        status = FatStatus::Refused;
    }
    File file{};
    if (status == FatStatus::Done)
    {
        status = Resolve(object, file);
    }
    if ((status == FatStatus::Done) &&
        (file.Directory || (page > ((static_cast<UInt64>(file.Size) + (FatPageBytes - 1U)) >> 12U)) ||
         (!file.Session && ((m_session != Session::None) || (file.Size > (FatMaximumFilePages * FatPageBytes))))))
    {
        status = FatStatus::Refused;
    }
    const bool started = (status == FatStatus::Done) && !file.Session;
    if (started)
    {
        status = StartReplace(file, object.Identifier, page);
    }
    const UInt32 shift = m_geometry.ClusterShift + 9U;
    const UInt32 needed = static_cast<UInt32>((((page + 1U) << 12U) + (1U << shift) - 1U) >> shift);
    const UInt32 previous{m_chainLength};
    while ((status == FatStatus::Done) && (m_chainLength < needed))
    {
        UInt32 cluster{0U};
        status = Allocate(cluster);
        if (status == FatStatus::Done)
        {
            m_chain[m_chainLength] = cluster;
            ++m_chainLength;
        }
    }
    if (status == FatStatus::Done)
    {
        status = SetDirty(true);
    }
    if ((status == FatStatus::Done) && ((m_sessionSize & 4095U) != 0U) &&
        (page == ((static_cast<UInt64>(m_sessionSize) + 4095U) >> 12U)))
    {
        status = ZeroTail();
    }
    if (status == FatStatus::Done)
    {
        status =
            Transfer(File{.Lba = m_sessionLba, .First = 0U, .Size = 0U, .Session = true}, page, 8U, nullptr, buffer);
    }
    if (status != FatStatus::Done)
    {
        // A refused write leaves the session as it was: a session it started
        // ends, the chain of an earlier one keeps its length.
        m_session = started ? Session::None : m_session;
        m_chainLength = started ? UInt8{0U} : previous;
        return status;
    }
    const UInt32 end = static_cast<UInt32>((page + 1U) << 12U);
    m_sessionSize = (m_sessionSize > end) ? m_sessionSize : end;
    return FatStatus::Done;
}

FatStatus FatVolume::Abort() noexcept
{
    const auto status = Usable();
    if (status != FatStatus::Done)
    {
        return status;
    }
    m_session = Session::None;
    m_chainLength = 0U;
    m_oldClusters = 0U;
    return SetDirty(false);
}

FatStatus FatVolume::Commit(const FatCrashPoint crash) noexcept
{
    FatStatus status = Usable();
    if ((status != FatStatus::Done) || (m_session == Session::None))
    {
        return (status == FatStatus::Done) ? SetDirty(false) : status;
    }
    status = SetDirty(true);
    for (UInt32 index{0U}; (status == FatStatus::Done) && (index < m_chainLength); ++index)
    {
        const UInt32 endMark = (m_geometry.Type == FatType::Fat12)
                                   ? 0xFFFU
                                   : ((m_geometry.Type == FatType::Fat16) ? 0xFFFFU : 0x0FFF'FFFFU);
        status = SetEntry(m_chain[index], ((index + 1U) < m_chainLength) ? m_chain[index + 1U] : endMark);
    }
    if (status == FatStatus::Done)
    {
        status = Flush();
    }
    const bool counted = m_freeCount != UnknownCount;
    if ((status == FatStatus::Done) && (m_chainLength != 0U))
    {
        m_freeCount = (counted && (m_freeCount >= m_chainLength)) ? (m_freeCount - m_chainLength) : UnknownCount;
        status = WriteFsInfo();
    }
    if ((status == FatStatus::Done) && (crash == FatCrashPoint::BeforeDirectoryEntry))
    {
        m_state = State::Broken;
        return FatStatus::Crashed;
    }
    if ((status == FatStatus::Done) && (m_session == Session::Create) && (m_gapLba != 0U))
    {
        status = Load(m_gapLba);
        for (UInt32 slot{m_gapSlot}; (status == FatStatus::Done) && (slot < 16U); ++slot)
        {
            m_sector[slot << 5U] = static_cast<UInt8>(Deleted);
        }
        if (status == FatStatus::Done)
        {
            status = Store(m_gapLba);
        }
    }
    if (status == FatStatus::Done)
    {
        status = WriteDirectory();
    }
    if ((status == FatStatus::Done) && (crash == FatCrashPoint::AfterDirectoryEntry))
    {
        m_state = State::Broken;
        return FatStatus::Crashed;
    }
    if ((status == FatStatus::Done) && (m_oldClusters != 0U))
    {
        status = FreeOldChain();
        m_freeCount = (m_freeCount != UnknownCount) ? (m_freeCount + m_oldClusters) : UnknownCount;
        if (status == FatStatus::Done)
        {
            status = WriteFsInfo();
        }
    }
    if (status == FatStatus::Done)
    {
        status = SetDirty(false);
    }
    if (status != FatStatus::Done)
    {
        return status;
    }
    m_end = ((m_session == Session::Create) && (m_sessionIndex >= m_end)) ? static_cast<UInt32>(m_sessionIndex + 1U)
                                                                          : m_end;
    m_session = Session::None;
    m_chainLength = 0U;
    m_oldClusters = 0U;
    return FatStatus::Done;
}
// Microsoft Extensible Firmware Initiative FAT32 File System Specification
// version 1.03, "FAT Directory Structure" and "FAT Long Directory Entries". The child is
// initialized before any FAT allocation or parent entry makes it reachable.
FatStatus FatVolume::SelectDirectory(const FatObject& directory) noexcept
{
    File file{};
    auto status = Usable();
    if (status == FatStatus::Done)
    {
        status = Resolve(directory, file);
    }
    if (status != FatStatus::Done)
    {
        return status;
    }
    if (!file.Directory || file.Session)
    {
        return FatStatus::Refused;
    }
    m_directory = directory.Identifier == RootIdentifier ? 0U : file.First;
    return (m_directory == 0U || IsCluster(m_directory)) ? FatStatus::Done : FatStatus::Corrupt;
}
FatStatus FatVolume::GrowDirectory() noexcept
{
    if (m_directory == 0U && m_geometry.Type != FatType::Fat32)
    {
        return FatStatus::Refused;
    }
    UInt32 last = m_directory != 0U ? m_directory : m_geometry.RootCluster;
    const UInt32 endMark =
        m_geometry.Type == FatType::Fat12 ? 0xFFFU : (m_geometry.Type == FatType::Fat16 ? 0xFFFFU : 0x0FFF'FFFFU);
    bool ended{false};
    for (UInt32 step{0U}; step < (MaximumDirectorySectors >> m_geometry.ClusterShift); ++step)
    {
        UInt32 next{0U};
        const auto status = GetEntry(last, next);
        if (status != FatStatus::Done)
        {
            return status;
        }
        if (next >= endMark - 7U)
        {
            // A tail beyond the scan bound cannot provide a usable slot; refuse before growing the
            // directory.
            if (step + 1U == (MaximumDirectorySectors >> m_geometry.ClusterShift))
            {
                return FatStatus::Refused;
            }
            ended = true;
            break;
        }
        if (!IsCluster(next))
        {
            return FatStatus::Corrupt;
        }
        last = next;
    }
    if (!ended)
    {
        return FatStatus::Refused;
    }
    UInt32 fresh{0U};
    auto status = Allocate(fresh);
    if (status == FatStatus::Done)
    {
        status = SetDirty(true);
    }
    // Zero first, allocate the new tail, then link it from the old tail.
    // A crash can leak it or expose an empty initialized tail, never garbage.
    for (UInt32 sector{0U}; status == FatStatus::Done && sector < (1U << m_geometry.ClusterShift); ++sector)
    {
        m_cached = NoSector;
        for (Size at{0U}; at < FatSectorBytes; ++at)
        {
            m_sector[at] = 0U;
        }
        status = Store(ClusterSector(fresh) + sector);
    }
    if (status == FatStatus::Done)
    {
        status = SetEntry(fresh, endMark);
    }
    if (status == FatStatus::Done)
    {
        status = Flush();
    }
    if (status == FatStatus::Done)
    {
        status = SetEntry(last, fresh);
    }
    if (status == FatStatus::Done)
    {
        status = Flush();
    }
    if (status == FatStatus::Done && m_freeCount != UnknownCount)
    {
        m_freeCount = m_freeCount == 0U ? UnknownCount : m_freeCount - 1U;
        m_chain[0U] = fresh;
        m_chainLength = 1U;
        status = WriteFsInfo();
        m_chainLength = 0U;
    }
    return status;
}
FatStatus FatVolume::InitializeDirectory(const UInt32 parent, const UInt32 fresh) noexcept
{
    m_chain[0U] = fresh;
    m_chainLength = 1U;
    auto status = SetDirty(true);
    for (UInt32 sector{0U}; (status == FatStatus::Done) && (sector < (1U << m_geometry.ClusterShift)); ++sector)
    {
        m_cached = NoSector;
        for (Size at{0U}; at < FatSectorBytes; ++at)
        {
            m_sector[at] = 0U;
        }
        if (sector == 0U)
        {
            for (UInt32 n{0U}; n < 2U; ++n)
            {
                auto* entry = m_sector + n * 32U;
                for (Size at{0U}; at < 11U; ++at)
                {
                    entry[at] = ' ';
                }
                entry[0U] = '.';
                if (n != 0U)
                {
                    entry[1U] = '.';
                }
                entry[11U] = Directory;
                const UInt32 cluster = n == 0U ? fresh : parent;
                Store16(entry + 20U, cluster >> 16U);
                Store16(entry + 26U, cluster);
                Store16(entry + 16U, FixedDate);
                Store16(entry + 24U, FixedDate);
            }
        }
        status = Store(ClusterSector(fresh) + sector);
    }
    return status;
}
FatStatus FatVolume::ReadDirectory(const FatObject& directory, UInt64& cursor, UInt8* name, Size& length,
                                   FatObject& object) noexcept
{
    auto status = SelectDirectory(directory);
    if (status != FatStatus::Done)
    {
        return status;
    }
    if ((name == nullptr) || (cursor >= MaximumDirectorySectors * 16U))
    {
        return FatStatus::NotFound;
    }
    Scan scan{.Name = nullptr,
              .Length = 0U,
              .Short = nullptr,
              .Tilde = 8U,
              .Run = 0U,
              .Found = 0U,
              .RunStart = 0U,
              .RunLba = 0U,
              .End = 0U,
              .EndLba = 0U,
              .Tildes = 0U,
              .Collision = false,
              .Enumerate = true,
              .Cursor = static_cast<UInt32>(cursor),
              .Output = name};
    status = ScanDirectory(scan);
    if (status != FatStatus::Done)
    {
        return status;
    }
    if (scan.Found == NoEntry)
    {
        return FatStatus::NotFound;
    }
    object = {(UInt64{m_directory} << 32U) | scan.Found, m_serial};
    length = scan.LengthOut;
    cursor = scan.Found + 1U;
    return FatStatus::Done;
}
} // Namespace Asteria::User::Filesystem
