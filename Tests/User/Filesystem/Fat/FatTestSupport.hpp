// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_FILESYSTEM_FAT_FAT_TEST_SUPPORT_HPP_INCLUDED
#define ASTERIA_TESTS_USER_FILESYSTEM_FAT_FAT_TEST_SUPPORT_HPP_INCLUDED

#include <cstring>

#include "Foundation/Types.hpp"
#include "User/Filesystem/Fat/FatVolume.hpp"

/// Test code for the FAT volume library: sparse in-memory disks, a device that logs and fails
/// writes, a formatter for geometries mkfs.fat will not make, and an invariant checker. None of it
/// is linked into the service; the formatter and checker decode the format on their own, not
/// through the library.
namespace Asteria::Test::Fat
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::FatBlockDevice;
using User::Filesystem::FatType;

inline constexpr Size SectorBytes{512U};
inline constexpr UInt64 NoBudget{~UInt64{0U}};

[[nodiscard]] inline UInt32 Get16(const UInt8* const bytes) noexcept
{
    return static_cast<UInt32>(bytes[0U]) | (static_cast<UInt32>(bytes[1U]) << 8U);
}

[[nodiscard]] inline UInt32 Get32(const UInt8* const bytes) noexcept
{
    return Get16(bytes) | (Get16(bytes + 2U) << 16U);
}

inline void Put16(UInt8* const bytes, const UInt32 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value);
    bytes[1U] = static_cast<UInt8>(value >> 8U);
}

inline void Put32(UInt8* const bytes, const UInt32 value) noexcept
{
    Put16(bytes, value);
    Put16(bytes + 2U, value >> 16U);
}

/// The sectors of a disk that were ever written, in an open-addressed table
/// over caller storage; any other sector reads as zeros. Holds a 64 MiB or
/// larger volume while only its touched sectors take memory.
class SparseDisk final
{
public:
    constexpr SparseDisk(UInt64* const keys, UInt8 (*const data)[SectorBytes], const Size capacityBits) noexcept
        : m_keys{keys}, m_data{data}, m_bits{capacityBits}
    {
    }

    SparseDisk(const SparseDisk&) = delete;
    SparseDisk& operator=(const SparseDisk&) = delete;

    void Reset(const UInt64 sectors) noexcept
    {
        m_sectors = sectors;
        m_used = 0U;
        std::memset(m_keys, 0xFF, GetCapacity() * sizeof(UInt64));
    }

    [[nodiscard]] UInt64 GetSectors() const noexcept
    {
        return m_sectors;
    }

    /// Sets the disk's size and keeps its sectors (an image loaded before its
    /// size was known).
    void SetSectors(const UInt64 sectors) noexcept
    {
        m_sectors = sectors;
    }

    [[nodiscard]] Size GetCapacity() const noexcept
    {
        return Size{1U} << m_bits;
    }

    [[nodiscard]] Size GetUsed() const noexcept
    {
        return m_used;
    }

    /// The stored sector, or nullptr when it was never written.
    [[nodiscard]] const UInt8* Find(const UInt64 lba) const noexcept
    {
        for (Size slot{Hash(lba)};; slot = (slot + 1U) & (GetCapacity() - 1U))
        {
            if (m_keys[slot] == lba)
            {
                return m_data[slot];
            }
            if (m_keys[slot] == Empty)
            {
                return nullptr;
            }
        }
    }

    /// Stores data as sector lba; false when the table is full.
    [[nodiscard]] bool Put(const UInt64 lba, const UInt8* const data) noexcept
    {
        Size slot{Hash(lba)};
        while ((m_keys[slot] != lba) && (m_keys[slot] != Empty))
        {
            slot = (slot + 1U) & (GetCapacity() - 1U);
        }
        if (m_keys[slot] == Empty)
        {
            if ((m_used + 1U) >= (GetCapacity() - (GetCapacity() >> 3U)))
            {
                return false;
            }
            m_keys[slot] = lba;
            ++m_used;
        }
        std::memcpy(m_data[slot], data, SectorBytes);
        return true;
    }

    [[nodiscard]] UInt64 KeyAt(const Size slot) const noexcept
    {
        return m_keys[slot];
    }

    [[nodiscard]] const UInt8* DataAt(const Size slot) const noexcept
    {
        return m_data[slot];
    }

    static constexpr UInt64 Empty{~UInt64{0U}};

private:
    [[nodiscard]] Size Hash(const UInt64 lba) const noexcept
    {
        return static_cast<Size>((lba * 0x9E37'79B9'7F4A'7C15ULL) >> (64U - m_bits));
    }

    UInt64* m_keys;
    UInt8 (*m_data)[SectorBytes];
    Size m_bits;
    UInt64 m_sectors{0U};
    Size m_used{0U};
};

template <Size Bits> struct SparseStorage
{
    UInt64 Keys[Size{1U} << Bits];
    UInt8 Data[Size{1U} << Bits][SectorBytes];
};

/// The device a test mounts: reads from Overlay (when given) then Base,
/// writes to Overlay or Base. After Budget sector writes every write fails
/// (a multi-sector write is written up to the budget first), which models a
/// crash at that point. Every sector written is logged in order.
class TestDevice final
{
public:
    static constexpr Size LogCapacity{16384U};

    constexpr explicit TestDevice(SparseDisk& base, SparseDisk* const overlay = nullptr) noexcept
        : m_base{&base}, m_overlay{overlay}
    {
    }

    TestDevice(const TestDevice&) = delete;
    TestDevice& operator=(const TestDevice&) = delete;

    [[nodiscard]] FatBlockDevice Get() noexcept
    {
        return FatBlockDevice{.Context = this,
                              .ReadSectors = &ReadSectors,
                              .WriteSectors = &WriteSectors,
                              .Sectors = m_base->GetSectors()};
    }

    /// Forgets the log and allows budget more sector writes.
    void Arm(const UInt64 budget) noexcept
    {
        m_budget = budget;
        m_writes = 0U;
        m_largestCall = 0U;
        m_calls = 0U;
    }

    [[nodiscard]] UInt64 GetWrites() const noexcept
    {
        return m_writes;
    }

    [[nodiscard]] UInt64 LogAt(const Size index) const noexcept
    {
        return (index < LogCapacity) ? m_log[index] : ~UInt64{0U};
    }

    [[nodiscard]] UInt32 GetLargestCall() const noexcept
    {
        return m_largestCall;
    }

    [[nodiscard]] UInt64 GetCalls() const noexcept
    {
        return m_calls;
    }

    void SetFailReads(const bool fail) noexcept
    {
        m_failReads = fail;
    }

    /// Reads a sector as the volume sees it; false past the disk.
    bool Read(const UInt64 lba, UInt8* const buffer) const noexcept
    {
        if (lba >= m_base->GetSectors())
        {
            return false;
        }
        const UInt8* stored = (m_overlay != nullptr) ? m_overlay->Find(lba) : nullptr;
        stored = (stored == nullptr) ? m_base->Find(lba) : stored;
        if (stored == nullptr)
        {
            std::memset(buffer, 0, SectorBytes);
        }
        else
        {
            std::memcpy(buffer, stored, SectorBytes);
        }
        return true;
    }

    /// Writes a sector outside the budget and the log (test setup).
    bool Write(const UInt64 lba, const UInt8* const buffer) noexcept
    {
        return (lba < m_base->GetSectors()) && ((m_overlay != nullptr) ? m_overlay : m_base)->Put(lba, buffer);
    }

    static bool ReadSectors(void* const context, const UInt64 lba, const UInt32 count, UInt8* const buffer) noexcept
    {
        TestDevice& device = *static_cast<TestDevice*>(context);
        ++device.m_calls;
        device.m_largestCall = (count > device.m_largestCall) ? count : device.m_largestCall;
        bool done = !device.m_failReads && (count != 0U);
        for (UInt32 index{0U}; done && (index < count); ++index)
        {
            done = device.Read(lba + index, buffer + (static_cast<Size>(index) * SectorBytes));
        }
        return done;
    }

    static bool WriteSectors(void* const context, const UInt64 lba, const UInt32 count,
                             const UInt8* const buffer) noexcept
    {
        TestDevice& device = *static_cast<TestDevice*>(context);
        ++device.m_calls;
        device.m_largestCall = (count > device.m_largestCall) ? count : device.m_largestCall;
        bool done{count != 0U};
        for (UInt32 index{0U}; done && (index < count); ++index)
        {
            done = (device.m_writes < device.m_budget) &&
                   device.Write(lba + index, buffer + (static_cast<Size>(index) * SectorBytes));
            if (done)
            {
                if (device.m_writes < LogCapacity)
                {
                    device.m_log[device.m_writes] = lba + index;
                }
                ++device.m_writes;
            }
        }
        return done;
    }

private:
    SparseDisk* m_base;
    SparseDisk* m_overlay;
    UInt64 m_budget{NoBudget};
    UInt64 m_writes{0U};
    UInt64 m_calls{0U};
    UInt32 m_largestCall{0U};
    bool m_failReads{false};
    UInt64 m_log[LogCapacity]{};
};

/// A volume's layout decoded by test code (the formatter's and the checker's
/// View, independent of ReadFatGeometry).
struct Layout
{
    FatType Type;
    UInt64 FatStart;
    UInt64 FatSectors;
    UInt64 RootStart;
    UInt64 RootSectors;
    UInt64 DataStart;
    UInt32 Clusters;
    UInt32 RootCluster;
    UInt32 FsInfo;
    UInt32 Fats;
    UInt32 ClusterShift;
};

/// Decodes the boot sector the way the FAT specification computes a layout.
[[nodiscard]] inline bool ParseLayout(const UInt8* const boot, Layout& layout) noexcept
{
    const UInt32 perCluster{boot[13U]};
    const UInt32 reserved = Get16(boot + 14U);
    const UInt32 fats{boot[16U]};
    const UInt32 rootEntries = Get16(boot + 17U);
    const UInt64 total = (Get16(boot + 19U) != 0U) ? Get16(boot + 19U) : Get32(boot + 32U);
    const UInt64 fatSize = (Get16(boot + 22U) != 0U) ? Get16(boot + 22U) : Get32(boot + 36U);
    if ((Get16(boot + 11U) != 512U) || (perCluster == 0U) || ((perCluster & (perCluster - 1U)) != 0U) || (fats == 0U) ||
        (fatSize == 0U))
    {
        return false;
    }
    UInt32 shift{0U};
    while ((1U << shift) < perCluster)
    {
        ++shift;
    }
    layout.FatStart = reserved;
    layout.FatSectors = fatSize;
    layout.RootStart = reserved + (fats * fatSize);
    layout.RootSectors = ((rootEntries * 32U) + 511U) / 512U;
    layout.DataStart = layout.RootStart + layout.RootSectors;
    if (layout.DataStart >= total)
    {
        return false;
    }
    layout.Clusters = static_cast<UInt32>((total - layout.DataStart) / perCluster);
    layout.Type =
        (layout.Clusters < 4085U) ? FatType::Fat12 : ((layout.Clusters < 65525U) ? FatType::Fat16 : FatType::Fat32);
    layout.RootCluster = (layout.Type == FatType::Fat32) ? Get32(boot + 44U) : 0U;
    layout.FsInfo = (layout.Type == FatType::Fat32) ? Get16(boot + 48U) : 0U;
    layout.Fats = fats;
    layout.ClusterShift = shift;
    return true;
}

[[nodiscard]] inline UInt64 ClusterLba(const Layout& layout, const UInt32 cluster) noexcept
{
    return layout.DataStart + (static_cast<UInt64>(cluster - 2U) << layout.ClusterShift);
}

/// FAT copy's entry for cluster: 12 or 16 bits, or all 32 bits on FAT32 (the
/// caller masks the reserved nibble).
[[nodiscard]] inline UInt32 ReadEntry(const TestDevice& device, const Layout& layout, const UInt32 copy,
                                      const UInt32 cluster) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt64 base = layout.FatStart + (copy * layout.FatSectors);
    if (layout.Type == FatType::Fat12)
    {
        const UInt64 offset = cluster + (cluster / 2U);
        static_cast<void>(device.Read(base + (offset / 512U), sector));
        UInt32 word = sector[offset % 512U];
        if ((offset % 512U) == 511U)
        {
            static_cast<void>(device.Read(base + (offset / 512U) + 1U, sector));
            word |= static_cast<UInt32>(sector[0U]) << 8U;
        }
        else
        {
            word |= static_cast<UInt32>(sector[(offset % 512U) + 1U]) << 8U;
        }
        return ((cluster % 2U) != 0U) ? (word >> 4U) : (word & 0xFFFU);
    }
    const UInt64 width = (layout.Type == FatType::Fat16) ? 2U : 4U;
    const UInt64 offset = cluster * width;
    static_cast<void>(device.Read(base + (offset / 512U), sector));
    return (width == 2U) ? Get16(sector + (offset % 512U)) : Get32(sector + (offset % 512U));
}

inline void WriteEntry(TestDevice& device, const Layout& layout, const UInt32 copy, const UInt32 cluster,
                       const UInt32 value) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt64 base = layout.FatStart + (copy * layout.FatSectors);
    if (layout.Type == FatType::Fat12)
    {
        const UInt64 offset = cluster + (cluster / 2U);
        const UInt32 shift = ((cluster % 2U) != 0U) ? 4U : 0U;
        const UInt32 mask = 0xFFFU << shift;
        for (UInt64 byte{offset}; byte < (offset + 2U); ++byte)
        {
            static_cast<void>(device.Read(base + (byte / 512U), sector));
            const UInt32 part = ((value << shift) & mask) >> ((byte - offset) * 8U);
            const UInt32 keep = (~mask >> ((byte - offset) * 8U)) & 0xFFU;
            sector[byte % 512U] = static_cast<UInt8>((sector[byte % 512U] & keep) | (part & 0xFFU));
            static_cast<void>(device.Write(base + (byte / 512U), sector));
        }
        return;
    }
    const UInt64 width = (layout.Type == FatType::Fat16) ? 2U : 4U;
    const UInt64 offset = cluster * width;
    static_cast<void>(device.Read(base + (offset / 512U), sector));
    if (width == 2U)
    {
        Put16(sector + (offset % 512U), value);
    }
    else
    {
        Put32(sector + (offset % 512U), value);
    }
    static_cast<void>(device.Write(base + (offset / 512U), sector));
}

[[nodiscard]] inline UInt32 EndMarkOf(const FatType type) noexcept
{
    return (type == FatType::Fat12) ? 0xFFFU : ((type == FatType::Fat16) ? 0xFFFFU : 0x0FFF'FFFFU);
}

/// The sector holding root directory entry index, or 0 past the directory.
[[nodiscard]] inline UInt64 DirectoryLba(const TestDevice& device, const Layout& layout, const UInt32 index) noexcept
{
    const UInt64 ordinal = index / 16U;
    if (layout.Type != FatType::Fat32)
    {
        return (ordinal < layout.RootSectors) ? (layout.RootStart + ordinal) : 0U;
    }
    UInt32 cluster{layout.RootCluster};
    for (UInt64 step{0U}; step < (ordinal >> layout.ClusterShift); ++step)
    {
        cluster = ReadEntry(device, layout, 0U, cluster) & 0x0FFF'FFFFU;
        if ((cluster < 2U) || (cluster > (layout.Clusters + 1U)))
        {
            return 0U;
        }
    }
    return ClusterLba(layout, cluster) + (ordinal & ((1U << layout.ClusterShift) - 1U));
}

/// A formatter for the geometries the tests need: FAT12/16/32, any cluster
/// size, one or two FATs, a FAT32 root of several clusters, and a FAT32
/// reserved nibble written into every entry.
struct FormatOptions
{
    UInt64 Sectors;
    UInt32 SectorsPerCluster;
    UInt32 Reserved;
    UInt32 Fats;
    UInt32 RootEntries;
    UInt32 RootClusters;
    UInt32 HighNibble;
    bool Label;
};

[[nodiscard]] inline bool Format(TestDevice& device, const FormatOptions& options, Layout& layout) noexcept
{
    UInt64 fatSectors{1U};
    UInt32 clusters{0U};
    FatType type{FatType::Fat12};
    for (Size round{0U}; round < 16U; ++round)
    {
        const UInt64 dataStart =
            options.Reserved + (options.Fats * fatSectors) + (((options.RootEntries * 32U) + 511U) / 512U);
        if (dataStart >= options.Sectors)
        {
            return false;
        }
        clusters = static_cast<UInt32>((options.Sectors - dataStart) / options.SectorsPerCluster);
        type = (clusters < 4085U) ? FatType::Fat12 : ((clusters < 65525U) ? FatType::Fat16 : FatType::Fat32);
        const UInt64 entries = clusters + 2U;
        const UInt64 bytes = (type == FatType::Fat12) ? (entries + ((entries + 1U) / 2U))
                                                      : (entries * ((type == FatType::Fat16) ? 2U : 4U));
        const UInt64 needed = (bytes + 511U) / 512U;
        if (needed <= fatSectors)
        {
            break;
        }
        fatSectors = needed;
    }
    const bool fat32 = type == FatType::Fat32;
    if (fat32 != (options.RootEntries == 0U))
    {
        return false;
    }
    UInt8 sector[SectorBytes]{};
    sector[0U] = 0xEBU;
    sector[1U] = fat32 ? 0x58U : 0x3CU;
    sector[2U] = 0x90U;
    std::memcpy(sector + 3U, "ASTERIA ", 8U);
    Put16(sector + 11U, 512U);
    sector[13U] = static_cast<UInt8>(options.SectorsPerCluster);
    Put16(sector + 14U, options.Reserved);
    sector[16U] = static_cast<UInt8>(options.Fats);
    Put16(sector + 17U, options.RootEntries);
    Put16(sector + 19U, (options.Sectors < 65536U) ? static_cast<UInt32>(options.Sectors) : 0U);
    sector[21U] = 0xF8U;
    Put16(sector + 22U, fat32 ? 0U : static_cast<UInt32>(fatSectors));
    Put16(sector + 24U, 32U);
    Put16(sector + 26U, 8U);
    Put32(sector + 32U, (options.Sectors < 65536U) ? 0U : static_cast<UInt32>(options.Sectors));
    UInt8* const extended = sector + (fat32 ? 64U : 36U);
    if (fat32)
    {
        Put32(sector + 36U, static_cast<UInt32>(fatSectors));
        Put32(sector + 44U, 2U);
        Put16(sector + 48U, 1U);
        Put16(sector + 50U, (options.Reserved > 7U) ? 6U : 0U);
    }
    extended[0U] = 0x80U;
    extended[2U] = 0x29U;
    Put32(extended + 3U, 0x4153'5445U);
    std::memcpy(extended + 7U, "ASTERIA    ", 11U);
    std::memcpy(extended + 18U, fat32 ? "FAT32   " : ((type == FatType::Fat16) ? "FAT16   " : "FAT12   "), 8U);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    bool done = device.Write(0U, sector);
    if (fat32 && (options.Reserved > 7U))
    {
        done = done && device.Write(6U, sector);
    }
    if (!done || !ParseLayout(sector, layout) || (layout.Type != type))
    {
        return false;
    }
    if (fat32)
    {
        UInt8 info[SectorBytes]{};
        Put32(info, 0x4161'5252U);
        Put32(info + 484U, 0x6141'7272U);
        Put32(info + 488U, clusters - options.RootClusters);
        Put32(info + 492U, 2U);
        Put32(info + 508U, 0xAA55'0000U);
        done = device.Write(1U, info) && ((options.Reserved <= 7U) || device.Write(7U, info));
    }
    // Every FAT sector with a nonzero entry: the reserved nibble pattern on
    // FAT32 touches all of them.
    const UInt32 nibble = fat32 ? (options.HighNibble << 28U) : 0U;
    for (UInt32 copy{0U}; done && (copy < options.Fats); ++copy)
    {
        const UInt64 base = layout.FatStart + (copy * fatSectors);
        const UInt64 last = (nibble != 0U) ? fatSectors : 1U;
        for (UInt64 index{0U}; done && (index < last); ++index)
        {
            UInt8 fat[SectorBytes]{};
            for (Size at{0U}; fat32 && (nibble != 0U) && (at < SectorBytes); at += 4U)
            {
                Put32(fat + at, nibble);
            }
            done = device.Write(base + index, fat);
        }
        WriteEntry(device, layout, copy, 0U, nibble | (EndMarkOf(type) & ~0x7U) | 0x8U);
        WriteEntry(device, layout, copy, 1U, nibble | EndMarkOf(type));
        for (UInt32 cluster{0U}; fat32 && (cluster < options.RootClusters); ++cluster)
        {
            const UInt32 next = ((cluster + 1U) < options.RootClusters) ? (cluster + 3U) : EndMarkOf(type);
            WriteEntry(device, layout, copy, cluster + 2U, nibble | next);
        }
    }
    // A zeroed root: never written sectors read as zeros, but a reused disk
    // may hold old data.
    const UInt64 rootSectors =
        fat32 ? (static_cast<UInt64>(options.RootClusters) << layout.ClusterShift) : layout.RootSectors;
    const UInt64 rootFirst = fat32 ? ClusterLba(layout, 2U) : layout.RootStart;
    UInt8 zero[SectorBytes]{};
    for (UInt64 index{0U}; done && (index < rootSectors); ++index)
    {
        done = device.Write(rootFirst + index, zero);
    }
    if (done && options.Label)
    {
        UInt8 root[SectorBytes]{};
        std::memcpy(root, "ASTERIA    ", 11U);
        root[11U] = 0x08U;
        done = device.Write(rootFirst, root);
    }
    return done;
}

/// Fills a page from a seed (a seed of zero gives zeros).
inline void FillPage(UInt8* const page, const UInt32 seed) noexcept
{
    UInt32 state{seed};
    for (Size index{0U}; index < User::Filesystem::FatPageBytes; ++index)
    {
        state = (seed == 0U) ? 0U : ((state * 1'103'515'245U) + 12'345U);
        page[index] = static_cast<UInt8>(state >> 16U);
    }
}

/// Writes a directory entry set and a chain as another tool would: an
/// 11-byte short name with case flags, an optional long name (written in
/// reverse order before it), the chain's clusters in both FATs ending in
/// endMark, and size bytes of data from seed. Returns the short entry index.
[[nodiscard]] inline bool PlaceFile(TestDevice& device, const Layout& layout, const UInt32 index,
                                    const char* const shortName, const UInt8 caseFlags, const char* const longName,
                                    const UInt32* const clusters, const Size count, const UInt32 size,
                                    const UInt32 endMark, const UInt32 seed) noexcept
{
    UInt8 checksum{0U};
    for (Size at{0U}; at < 11U; ++at)
    {
        checksum = static_cast<UInt8>(((checksum & 1U) << 7U) + (checksum >> 1U) + static_cast<UInt8>(shortName[at]));
    }
    const Size longLength = (longName != nullptr) ? std::strlen(longName) : 0U;
    const UInt32 longEntries = static_cast<UInt32>((longLength + 12U) / 13U);
    static constexpr UInt8 Offsets[13U]{1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
    UInt8 sector[SectorBytes]{};
    bool done{index >= longEntries};
    for (UInt32 entry{0U}; done && (entry <= longEntries); ++entry)
    {
        const UInt32 at = index - longEntries + entry;
        const UInt64 lba = DirectoryLba(device, layout, at);
        done = (lba != 0U) && device.Read(lba, sector);
        UInt8* const record = sector + ((at % 16U) * 32U);
        std::memset(record, 0, 32U);
        if (entry < longEntries)
        {
            const UInt32 order = longEntries - entry;
            record[0U] = static_cast<UInt8>(order | ((entry == 0U) ? 0x40U : 0U));
            for (Size character{0U}; character < 13U; ++character)
            {
                const Size position = ((order - 1U) * 13U) + character;
                const UInt32 value = (position < longLength) ? static_cast<UInt8>(longName[position])
                                                             : ((position == longLength) ? 0U : 0xFFFFU);
                Put16(record + Offsets[character], value);
            }
            record[11U] = 0x0FU;
            record[13U] = checksum;
        }
        else
        {
            std::memcpy(record, shortName, 11U);
            record[11U] = 0x20U;
            record[12U] = caseFlags;
            const UInt32 first = (count != 0U) ? clusters[0U] : 0U;
            Put16(record + 20U, (layout.Type == FatType::Fat32) ? (first >> 16U) : 0U);
            Put16(record + 26U, first);
            Put32(record + 28U, size);
        }
        done = done && device.Write(lba, sector);
    }
    for (UInt32 copy{0U}; done && (copy < layout.Fats); ++copy)
    {
        for (Size at{0U}; at < count; ++at)
        {
            const UInt32 old =
                (layout.Type == FatType::Fat32) ? (ReadEntry(device, layout, copy, clusters[at]) & 0xF000'0000U) : 0U;
            WriteEntry(device, layout, copy, clusters[at], old | (((at + 1U) < count) ? clusters[at + 1U] : endMark));
        }
    }
    if (done && (layout.Type == FatType::Fat32) && (count != 0U))
    {
        // FSInfo's free count stays exact, as the tool's would.
        done = device.Read(layout.FsInfo, sector);
        Put32(sector + 488U, Get32(sector + 488U) - static_cast<UInt32>(count));
        done = done && device.Write(layout.FsInfo, sector);
    }
    UInt8 page[User::Filesystem::FatPageBytes]{};
    for (UInt32 written{0U}; done && (written < size); written += SectorBytes)
    {
        if ((written % User::Filesystem::FatPageBytes) == 0U)
        {
            FillPage(page,
                     (seed == 0U) ? 0U : (seed + (written / static_cast<UInt32>(User::Filesystem::FatPageBytes))));
        }
        const UInt32 fileSector = written / 512U;
        const UInt32 cluster = clusters[fileSector >> layout.ClusterShift];
        done = device.Write(ClusterLba(layout, cluster) + (fileSector & ((1U << layout.ClusterShift) - 1U)),
                            page + (written % User::Filesystem::FatPageBytes));
    }
    return done;
}

/// Clears both dirty marks (boot flags byte, FAT[1]'s clean and hard-error
/// Bits in every FAT) as a repair tool would.
inline void ClearDirty(TestDevice& device, const Layout& layout) noexcept
{
    UInt8 boot[SectorBytes]{};
    static_cast<void>(device.Read(0U, boot));
    boot[(layout.Type == FatType::Fat32) ? 0x41U : 0x25U] = 0U;
    static_cast<void>(device.Write(0U, boot));
    for (UInt32 copy{0U}; (layout.Type != FatType::Fat12) && (copy < layout.Fats); ++copy)
    {
        const UInt32 bits = (layout.Type == FatType::Fat32) ? 0x0C00'0000U : 0xC000U;
        WriteEntry(device, layout, copy, 1U, ReadEntry(device, layout, copy, 1U) | bits);
    }
}

/// What the invariant checker found. Errors counts the violations a crash
/// may never leave: a cluster in two chains, a chain that ends
/// in a free, bad or out-of-range entry, a length that is not
/// ceil(size / cluster), an entry value 1 or out of range, a malformed or
/// orphaned long-name set, two files of one name, a FAT32 root chain that is
/// broken. The other fields are what a crash may leave.
struct CheckReport
{
    bool Parsed;
    Size Errors;
    const char* FirstError;
    UInt64 ErrorDetail;
    UInt32 LostClusters;
    UInt32 FreeClusters;
    Size DifferingSectors;
    UInt64 DifferingSector;
    bool FsInfoWrong;
    bool FsInfoUnknown;
    bool Dirty;
    Size Files;

    [[nodiscard]] bool IsClean() const noexcept
    {
        return Parsed && (Errors == 0U) && (LostClusters == 0U) && (DifferingSectors == 0U) && !FsInfoWrong && !Dirty;
    }
};

namespace Detail
{
inline constexpr Size MaximumCheckedClusters{Size{1U} << 22U};
inline constexpr Size MaximumCheckedFiles{1024U};

struct CheckerState
{
    UInt64 Visited[MaximumCheckedClusters / 64U];
    UInt8 Names[MaximumCheckedFiles][64U];
    Size NameLengths[MaximumCheckedFiles];
    UInt8 ShortNames[MaximumCheckedFiles][11U];
    UInt8 SectorA[SectorBytes];
    UInt8 SectorB[SectorBytes];
    UInt64 CachedLba;
    UInt8 Cached[SectorBytes];
};

inline CheckerState g_checker{};

/// FAT copy 0's entry for cluster through a one-sector cache, which
/// CheckVolume empties first.
inline UInt32 Entry(const TestDevice& device, const Layout& layout, const UInt32 cluster) noexcept
{
    const UInt64 width = (layout.Type == FatType::Fat12) ? 3U : ((layout.Type == FatType::Fat16) ? 4U : 8U);
    const UInt64 offset = (static_cast<UInt64>(cluster) * width) / 2U;
    const auto byteAt = [&](const UInt64 at) noexcept
    {
        const UInt64 lba = layout.FatStart + (at / 512U);
        if (g_checker.CachedLba != lba)
        {
            static_cast<void>(device.Read(lba, g_checker.Cached));
            g_checker.CachedLba = lba;
        }
        return static_cast<UInt32>(g_checker.Cached[at % 512U]);
    };
    if (layout.Type == FatType::Fat12)
    {
        const UInt32 word = byteAt(offset) | (byteAt(offset + 1U) << 8U);
        return ((cluster % 2U) != 0U) ? (word >> 4U) : (word & 0xFFFU);
    }
    UInt32 value{0U};
    for (UInt64 at{0U}; at < (width / 2U); ++at)
    {
        value |= byteAt(offset + at) << (8U * at);
    }
    return value;
}

inline void Error(CheckReport& report, const char* const what, const UInt64 detail) noexcept
{
    if (report.Errors == 0U)
    {
        report.FirstError = what;
        report.ErrorDetail = detail;
    }
    ++report.Errors;
}

/// Marks a chain's clusters visited: its length, or ~0 for a broken chain
/// (reported).
inline UInt32 WalkChain(const TestDevice& device, const Layout& layout, UInt32 cluster, CheckReport& report) noexcept
{
    const UInt32 mask = (layout.Type == FatType::Fat32) ? 0x0FFF'FFFFU : 0xFFFF'FFFFU;
    const UInt32 endMark = EndMarkOf(layout.Type) & ~0x7U;
    UInt32 length{0U};
    for (;;)
    {
        if ((cluster < 2U) || (cluster > (layout.Clusters + 1U)))
        {
            Error(report, "a chain reaches an entry that is no cluster", cluster);
            return ~UInt32{0U};
        }
        UInt64& word = g_checker.Visited[cluster / 64U];
        const UInt64 bit = UInt64{1U} << (cluster % 64U);
        if ((word & bit) != 0U)
        {
            Error(report, "a cluster is in two chains", cluster);
            return ~UInt32{0U};
        }
        word |= bit;
        ++length;
        const UInt32 next = Entry(device, layout, cluster) & mask;
        if (next >= endMark)
        {
            return length;
        }
        if ((next == 0U) || (next == (endMark - 1U)))
        {
            Error(report, "a chain contains a free or bad cluster", cluster);
            return ~UInt32{0U};
        }
        cluster = next;
    }
}
} // Namespace Detail

/// Checks the volume on device against invariants.
[[nodiscard]] inline CheckReport CheckVolume(const TestDevice& device) noexcept
{
    using Detail::g_checker;
    CheckReport report{};
    UInt8* const boot = g_checker.SectorA;
    Layout layout{};
    if (!device.Read(0U, boot) || !ParseLayout(boot, layout) ||
        ((layout.Clusters + 2U) > Detail::MaximumCheckedClusters))
    {
        return report;
    }
    report.Parsed = true;
    g_checker.CachedLba = ~UInt64{0U};
    const UInt32 mask = (layout.Type == FatType::Fat32) ? 0x0FFF'FFFFU : 0xFFFF'FFFFU;
    const UInt32 endMark = EndMarkOf(layout.Type) & ~0x7U;
    const UInt32 bad = endMark - 1U;
    const UInt32 last = layout.Clusters + 1U;
    report.Dirty = (boot[(layout.Type == FatType::Fat32) ? 0x41U : 0x25U] & 1U) != 0U;
    if (layout.Type != FatType::Fat12)
    {
        const UInt32 bits = (layout.Type == FatType::Fat32) ? 0x0C00'0000U : 0xC000U;
        report.Dirty = report.Dirty || ((ReadEntry(device, layout, 0U, 1U) & bits) != bits);
    }
    for (UInt64 index{0U}; (layout.Fats == 2U) && (index < layout.FatSectors); ++index)
    {
        static_cast<void>(device.Read(layout.FatStart + index, g_checker.SectorA));
        static_cast<void>(device.Read(layout.FatStart + layout.FatSectors + index, g_checker.SectorB));
        if (std::memcmp(g_checker.SectorA, g_checker.SectorB, SectorBytes) != 0)
        {
            report.DifferingSector = (report.DifferingSectors == 0U) ? index : report.DifferingSector;
            ++report.DifferingSectors;
        }
    }
    std::memset(g_checker.Visited, 0, (((last + 1U) / 64U) + 1U) * sizeof(UInt64));
    for (UInt32 cluster{2U}; cluster <= last; ++cluster)
    {
        const UInt32 value = Detail::Entry(device, layout, cluster) & mask;
        report.FreeClusters += (value == 0U) ? 1U : 0U;
        if ((value == 1U) || ((value > last) && (value < bad)))
        {
            Detail::Error(report, "an entry holds a value out of range", cluster);
        }
    }
    if (layout.Type == FatType::Fat32)
    {
        UInt8* const info = g_checker.SectorB;
        static_cast<void>(device.Read(layout.FsInfo, info));
        if ((layout.FsInfo == 0U) || (Get32(info) != 0x4161'5252U) || (Get32(info + 484U) != 0x6141'7272U) ||
            (Get32(info + 508U) != 0xAA55'0000U))
        {
            Detail::Error(report, "the FSInfo sector is missing", layout.FsInfo);
        }
        report.FsInfoUnknown = Get32(info + 488U) == 0xFFFF'FFFFU;
        report.FsInfoWrong = !report.FsInfoUnknown && (Get32(info + 488U) != report.FreeClusters);
        if (Detail::WalkChain(device, layout, layout.RootCluster, report) == ~UInt32{0U})
        {
            Detail::Error(report, "the root chain is broken", layout.RootCluster);
        }
    }
    // The root directory: long-name sets, files, their chains and names.
    UInt32 sequence{0U};
    UInt32 checksum{0U};
    UInt8 longName[64U]{};
    Size longLength{0U};
    bool longValid{false};
    const UInt64 capacity = (layout.Type == FatType::Fat32) ? (UInt64{65536U}) : (layout.RootSectors * 16U);
    for (UInt32 index{0U}; index < capacity; ++index)
    {
        const UInt64 lba = DirectoryLba(device, layout, index);
        if (lba == 0U)
        {
            break;
        }
        UInt8* const sector = g_checker.SectorA;
        static_cast<void>(device.Read(lba, sector));
        const UInt8* const entry = sector + ((index % 16U) * 32U);
        if (entry[0U] == 0U)
        {
            if (sequence != 0U)
            {
                Detail::Error(report, "a long-name set ends the directory", index);
            }
            break;
        }
        if (entry[0U] == 0xE5U)
        {
            if (sequence != 0U)
            {
                Detail::Error(report, "a long-name set is followed by a deleted entry", index);
            }
            sequence = 0U;
            continue;
        }
        if ((entry[11U] & 0x3FU) == 0x0FU)
        {
            const UInt32 order = entry[0U] & 0x3FU;
            if ((entry[0U] & 0x40U) != 0U)
            {
                if (sequence != 0U)
                {
                    Detail::Error(report, "a long-name set is interrupted", index);
                }
                sequence = order;
                checksum = entry[13U];
                longValid = (order >= 1U) && (order <= 20U) && ((entry[0U] & 0x80U) == 0U);
                longLength = 0U;
                std::memset(longName, 0, sizeof(longName));
            }
            else if ((sequence == 0U) || ((order + 1U) != sequence) || (entry[13U] != checksum))
            {
                Detail::Error(report, "a long-name entry is orphaned", index);
                sequence = 0U;
                continue;
            }
            else
            {
                sequence = order;
            }
            static constexpr UInt8 Offsets[13U]{1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
            for (Size character{0U}; longValid && (character < 13U); ++character)
            {
                const UInt32 value = Get16(entry + Offsets[character]);
                const Size position = ((order - 1U) * 13U) + character;
                if ((value != 0U) && (value != 0xFFFFU) && (position < 64U))
                {
                    longName[position] = static_cast<UInt8>(((value >= 'A') && (value <= 'Z')) ? (value + 32U) : value);
                    longLength = (position + 1U > longLength) ? (position + 1U) : longLength;
                }
            }
            continue;
        }
        UInt8 sum{0U};
        for (Size at{0U}; at < 11U; ++at)
        {
            sum = static_cast<UInt8>(((sum & 1U) << 7U) + (sum >> 1U) + entry[at]);
        }
        const bool hasLong = (sequence != 0U);
        if (hasLong && ((sequence != 1U) || (sum != checksum) || !longValid))
        {
            Detail::Error(report, "a long-name set does not belong to its short entry", index);
        }
        sequence = 0U;
        if ((entry[11U] & 0x18U) != 0U)
        {
            if (hasLong)
            {
                Detail::Error(report, "a long name names a label or directory", index);
            }
            continue;
        }
        const Size file = report.Files;
        if (file >= Detail::MaximumCheckedFiles)
        {
            Detail::Error(report, "too many files to check", index);
            break;
        }
        ++report.Files;
        UInt8 rendered[12U]{};
        const Size length = User::Filesystem::RenderFatShortName(entry, rendered);
        std::memcpy(g_checker.ShortNames[file], entry, 11U);
        g_checker.NameLengths[file] = hasLong ? longLength : length;
        for (Size at{0U}; at < g_checker.NameLengths[file]; ++at)
        {
            const UInt32 value = hasLong ? longName[at] : rendered[at];
            g_checker.Names[file][at] = static_cast<UInt8>(((value >= 'A') && (value <= 'Z')) ? (value + 32U) : value);
        }
        for (Size other{0U}; other < file; ++other)
        {
            if (((g_checker.NameLengths[other] == g_checker.NameLengths[file]) &&
                 (std::memcmp(g_checker.Names[other], g_checker.Names[file], g_checker.NameLengths[file]) == 0)) ||
                (std::memcmp(g_checker.ShortNames[other], g_checker.ShortNames[file], 11U) == 0))
            {
                Detail::Error(report, "two files have one name", index);
            }
        }
        const UInt32 high = (layout.Type == FatType::Fat32) ? (Get16(entry + 20U) << 16U) : 0U;
        const UInt32 first = high | Get16(entry + 26U);
        const UInt64 size = Get32(entry + 28U);
        const UInt64 clusterBytes = UInt64{512U} << layout.ClusterShift;
        const UInt64 expected = (size + clusterBytes - 1U) / clusterBytes;
        if (size == 0U)
        {
            if (first != 0U)
            {
                Detail::Error(report, "an empty file has a chain", index);
            }
            continue;
        }
        const UInt32 length2 = Detail::WalkChain(device, layout, first, report);
        if ((length2 != ~UInt32{0U}) && (length2 != expected))
        {
            Detail::Error(report, "a chain's length is not its size's", index);
        }
    }
    for (UInt32 cluster{2U}; cluster <= last; ++cluster)
    {
        const UInt32 value = Detail::Entry(device, layout, cluster) & mask;
        const bool visited = (g_checker.Visited[cluster / 64U] & (UInt64{1U} << (cluster % 64U))) != 0U;
        report.LostClusters += ((value != 0U) && (value != bad) && !visited) ? 1U : 0U;
    }
    return report;
}
} // Namespace Asteria::Test::Fat

#endif // ASTERIA_TESTS_USER_FILESYSTEM_FAT_FAT_TEST_SUPPORT_HPP_INCLUDED
