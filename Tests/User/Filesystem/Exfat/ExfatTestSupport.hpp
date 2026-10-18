// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_FILESYSTEM_EXFAT_EXFAT_TEST_SUPPORT_HPP_INCLUDED
#define ASTERIA_TESTS_USER_FILESYSTEM_EXFAT_EXFAT_TEST_SUPPORT_HPP_INCLUDED

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Foundation/Types.hpp"
#include "User/Filesystem/Exfat/ExfatVolume.hpp"
#include "User/Filesystem/Fat/FatTestSupport.hpp"

/// Test code for the exFAT volume library: the FAT tests' sparse disks and logging, failing device
/// behind an ExfatBlockDevice; the images ExfatToolImages.py makes with mkfs.exfat at test time
/// (loaded from ASTERIA_EXFAT_IMAGE_DIR); a writer that places files as another implementation
/// would (contiguous or FAT-chained, with benign entries, a set across sectors, or broken sums);
/// and an invariant checker. None of it is linked into the service; the writer and the checker
/// decode the format on their own, from the exFAT specification, not through the library.
namespace Asteria::Test::Exfat
{
using Fat::FillPage;
using Fat::Get16;
using Fat::Get32;
using Fat::NoBudget;
using Fat::Put16;
using Fat::Put32;
using Fat::SectorBytes;
using Fat::SparseDisk;
using Fat::SparseStorage;
using Fat::TestDevice;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::ExfatBlockDevice;
using User::Filesystem::ExfatPageBytes;

/// The images ExfatToolImages.py makes: 64 MiB, mkfs.exfat -c 4K and -c 512,
/// the boot diagnostic's token in sectors 8 and 20 with both boot checksums
/// recomputed.
inline constexpr char ImageVariable[]{"ASTERIA_EXFAT_IMAGE_DIR"};
inline constexpr const char* ImageNames[2U]{"exfat-4k.img", "exfat-512.img"};
inline constexpr UInt64 TokenSector{8U};
inline constexpr UInt32 EndOfChain{0xFFFF'FFFFU};

[[nodiscard]] inline UInt64 Get64(const UInt8* const bytes) noexcept
{
    return static_cast<UInt64>(Get32(bytes)) | (static_cast<UInt64>(Get32(bytes + 4U)) << 32U);
}

inline void Put64(UInt8* const bytes, const UInt64 value) noexcept
{
    Put32(bytes, static_cast<UInt32>(value));
    Put32(bytes + 4U, static_cast<UInt32>(value >> 32U));
}

[[nodiscard]] inline const UInt8* Bytes(const char* const text) noexcept
{
    return reinterpret_cast<const UInt8*>(text);
}

/// The test device as the library's sector device.
[[nodiscard]] inline ExfatBlockDevice DeviceOf(TestDevice& device) noexcept
{
    const User::Filesystem::FatBlockDevice fat = device.Get();
    return ExfatBlockDevice{.Context = fat.Context,
                            .ReadSectors = fat.ReadSectors,
                            .WriteSectors = fat.WriteSectors,
                            .Sectors = fat.Sectors};
}

/// Directory/name in a static buffer ("" when it does not fit).
[[nodiscard]] inline const char* PathOf(const char* const directory, const char* const name) noexcept
{
    static char path[1024U];
    const int written = std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    return ((written > 0) && (static_cast<Size>(written) < sizeof(path))) ? path : "";
}

/// Loads an image's nonzero sectors into disk, which takes the image's size.
[[nodiscard]] inline bool LoadImage(SparseDisk& disk, const char* const path) noexcept
{
    static UInt8 chunk[1U << 16U];
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    disk.Reset(~UInt64{0U});
    UInt64 sectors{0U};
    bool done{true};
    for (Size read = std::fread(chunk, 1U, sizeof(chunk), file); done && (read != 0U);
         read = std::fread(chunk, 1U, sizeof(chunk), file))
    {
        for (Size at{0U}; done && ((at + SectorBytes) <= read); at += SectorBytes, ++sectors)
        {
            bool zero{true};
            for (Size index{0U}; zero && (index < SectorBytes); ++index)
            {
                zero = chunk[at + index] == 0U;
            }
            done = zero || disk.Put(sectors, chunk + at);
        }
        done = done && ((read % SectorBytes) == 0U);
    }
    static_cast<void>(std::fclose(file));
    disk.SetSectors(sectors);
    return done && (sectors != 0U);
}

/// Writes base under overlay as a sparse image of base's size.
[[nodiscard]] inline bool SaveImage(const SparseDisk& base, const SparseDisk& overlay, const char* const path) noexcept
{
    std::FILE* const file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return false;
    }
    const UInt8 zero{0U};
    bool done = (std::fseek(file, static_cast<long>((base.GetSectors() * SectorBytes) - 1U), SEEK_SET) == 0) &&
                (std::fwrite(&zero, 1U, 1U, file) == 1U);
    for (Size pass{0U}; pass < 2U; ++pass)
    {
        const SparseDisk& disk = (pass == 0U) ? base : overlay;
        for (Size slot{0U}; done && (slot < disk.GetCapacity()); ++slot)
        {
            const UInt64 lba = disk.KeyAt(slot);
            if ((lba == SparseDisk::Empty) || ((pass == 0U) && (overlay.Find(lba) != nullptr)))
            {
                continue;
            }
            done = (std::fseek(file, static_cast<long>(lba * SectorBytes), SEEK_SET) == 0) &&
                   (std::fwrite(disk.DataAt(slot), 1U, SectorBytes, file) == SectorBytes);
        }
    }
    return (std::fclose(file) == 0) && done;
}

[[nodiscard]] inline bool SaveBytes(const char* const path, const UInt8* const bytes, const Size size) noexcept
{
    std::FILE* const file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return false;
    }
    const bool written = std::fwrite(bytes, 1U, size, file) == size;
    return (std::fclose(file) == 0) && written;
}

/// A volume's layout as the specification decodes it (the writer's and the
/// checker's view, independent of ReadExfatBootSector), with the allocation
/// bitmap's and the up-case table's entries.
struct Layout
{
    UInt64 VolumeSectors;
    UInt64 FatStart;
    UInt64 HeapStart;
    UInt32 Clusters;
    UInt32 RootCluster;
    UInt32 ClusterShift;
    UInt32 Fats;
    UInt32 BitmapCluster;
    UInt64 BitmapBytes;
    UInt32 UpcaseCluster;
    UInt64 UpcaseBytes;
};

[[nodiscard]] inline UInt64 ClusterLba(const Layout& layout, const UInt32 cluster) noexcept
{
    return layout.HeapStart + (static_cast<UInt64>(cluster - 2U) << layout.ClusterShift);
}

[[nodiscard]] inline UInt32 ClusterBytes(const Layout& layout) noexcept
{
    return UInt32{512U} << layout.ClusterShift;
}

[[nodiscard]] inline bool InHeap(const Layout& layout, const UInt32 cluster) noexcept
{
    return (cluster >= 2U) && (cluster <= (layout.Clusters + 1U));
}

[[nodiscard]] inline UInt32 ReadFat(const TestDevice& device, const Layout& layout, const UInt32 cluster) noexcept
{
    UInt8 sector[SectorBytes]{};
    static_cast<void>(device.Read(layout.FatStart + ((cluster * 4ULL) / SectorBytes), sector));
    return Get32(sector + ((cluster * 4U) % SectorBytes));
}

inline void WriteFat(TestDevice& device, const Layout& layout, const UInt32 cluster, const UInt32 value) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt64 lba = layout.FatStart + ((cluster * 4ULL) / SectorBytes);
    static_cast<void>(device.Read(lba, sector));
    Put32(sector + ((cluster * 4U) % SectorBytes), value);
    static_cast<void>(device.Write(lba, sector));
}

/// The sector ordinal of the chain from first, followed through the FAT; 0
/// past its end.
[[nodiscard]] inline UInt64 ChainLba(const TestDevice& device, const Layout& layout, const UInt32 first,
                                     const UInt64 ordinal) noexcept
{
    UInt32 cluster{first};
    for (UInt64 step{0U}; step < (ordinal >> layout.ClusterShift); ++step)
    {
        if (!InHeap(layout, cluster))
        {
            return 0U;
        }
        cluster = ReadFat(device, layout, cluster);
    }
    return InHeap(layout, cluster) ? (ClusterLba(layout, cluster) + (ordinal & ((1U << layout.ClusterShift) - 1U)))
                                   : 0U;
}

/// The sector of root directory entry index; 0 past the directory.
[[nodiscard]] inline UInt64 EntryLba(const TestDevice& device, const Layout& layout, const UInt32 index) noexcept
{
    return ChainLba(device, layout, layout.RootCluster, index / 16U);
}

[[nodiscard]] inline bool ReadEntry(const TestDevice& device, const Layout& layout, const UInt32 index,
                                    UInt8* const entry) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt64 lba = EntryLba(device, layout, index);
    const bool done = (lba != 0U) && device.Read(lba, sector);
    std::memcpy(entry, sector + ((index % 16U) * 32U), 32U);
    return done;
}

[[nodiscard]] inline bool WriteEntry(TestDevice& device, const Layout& layout, const UInt32 index,
                                     const UInt8* const entry) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt64 lba = EntryLba(device, layout, index);
    if ((lba == 0U) || !device.Read(lba, sector))
    {
        return false;
    }
    std::memcpy(sector + ((index % 16U) * 32U), entry, 32U);
    return device.Write(lba, sector);
}

/// The allocation bitmap's sector for cluster and the bit's byte offset in it.
[[nodiscard]] inline UInt64 BitmapLba(const TestDevice& device, const Layout& layout, const UInt32 cluster,
                                      Size& offset) noexcept
{
    const UInt64 byte = (cluster - 2U) / 8U;
    offset = static_cast<Size>(byte % SectorBytes);
    return ChainLba(device, layout, layout.BitmapCluster, byte / SectorBytes);
}

[[nodiscard]] inline bool IsAllocated(const TestDevice& device, const Layout& layout, const UInt32 cluster) noexcept
{
    UInt8 sector[SectorBytes]{};
    Size offset{0U};
    static_cast<void>(device.Read(BitmapLba(device, layout, cluster, offset), sector));
    return (sector[offset] & (1U << ((cluster - 2U) % 8U))) != 0U;
}

inline void SetAllocated(TestDevice& device, const Layout& layout, const UInt32 cluster, const bool allocated) noexcept
{
    UInt8 sector[SectorBytes]{};
    Size offset{0U};
    const UInt64 lba = BitmapLba(device, layout, cluster, offset);
    static_cast<void>(device.Read(lba, sector));
    const UInt32 bit = 1U << ((cluster - 2U) % 8U);
    sector[offset] = static_cast<UInt8>(allocated ? (sector[offset] | bit) : (sector[offset] & ~bit));
    static_cast<void>(device.Write(lba, sector));
}

/// The boot checksum of the boot region at base (sectors base to base + 10,
/// skipping VolumeFlags and PercentInUse of its boot sector).
[[nodiscard]] inline UInt32 BootChecksum(const TestDevice& device, const UInt64 base) noexcept
{
    UInt32 checksum{0U};
    UInt8 sector[SectorBytes]{};
    for (UInt64 index{0U}; index < 11U; ++index)
    {
        static_cast<void>(device.Read(base + index, sector));
        for (Size at{0U}; at < SectorBytes; ++at)
        {
            if ((index != 0U) || ((at != 106U) && (at != 107U) && (at != 112U)))
            {
                checksum = ((checksum >> 1U) | (checksum << 31U)) + sector[at];
            }
        }
    }
    return checksum;
}

/// Whether sector base + 11 holds 128 copies of the region's checksum.
[[nodiscard]] inline bool BootChecksumHolds(const TestDevice& device, const UInt64 base) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt32 checksum = BootChecksum(device, base);
    bool holds = device.Read(base + 11U, sector);
    for (Size at{0U}; holds && (at < SectorBytes); at += 4U)
    {
        holds = Get32(sector + at) == checksum;
    }
    return holds;
}

inline void FixBootChecksum(TestDevice& device, const UInt64 base) noexcept
{
    UInt8 sector[SectorBytes]{};
    const UInt32 checksum = BootChecksum(device, base);
    for (Size at{0U}; at < SectorBytes; at += 4U)
    {
        Put32(sector + at, checksum);
    }
    static_cast<void>(device.Write(base + 11U, sector));
}

/// Decodes the boot sector and finds the bitmap and up-case entries in the
/// root directory; false for no exFAT volume.
[[nodiscard]] inline bool ParseLayout(const TestDevice& device, Layout& layout) noexcept
{
    UInt8 boot[SectorBytes]{};
    if (!device.Read(0U, boot) || (std::memcmp(boot + 3U, "EXFAT   ", 8U) != 0) || (boot[108U] != 9U))
    {
        return false;
    }
    layout = Layout{.VolumeSectors = Get64(boot + 72U),
                    .FatStart = Get32(boot + 80U),
                    .HeapStart = Get32(boot + 88U),
                    .Clusters = Get32(boot + 92U),
                    .RootCluster = Get32(boot + 96U),
                    .ClusterShift = boot[109U],
                    .Fats = boot[110U],
                    .BitmapCluster = 0U,
                    .BitmapBytes = 0U,
                    .UpcaseCluster = 0U,
                    .UpcaseBytes = 0U};
    if ((layout.ClusterShift > 16U) || !InHeap(layout, layout.RootCluster))
    {
        return false;
    }
    UInt8 entry[32U]{};
    for (UInt32 index{0U}; (index < 65536U) && ReadEntry(device, layout, index, entry); ++index)
    {
        if (entry[0U] == 0U)
        {
            break;
        }
        if ((entry[0U] == 0x81U) && (layout.BitmapCluster == 0U))
        {
            layout.BitmapCluster = Get32(entry + 20U);
            layout.BitmapBytes = Get64(entry + 24U);
        }
        if ((entry[0U] == 0x82U) && (layout.UpcaseCluster == 0U))
        {
            layout.UpcaseCluster = Get32(entry + 20U);
            layout.UpcaseBytes = Get64(entry + 24U);
        }
    }
    return InHeap(layout, layout.BitmapCluster);
}

/// NameHash of an ASCII name, upper-cased by the writer's own table.
[[nodiscard]] inline UInt32 NameHash(const char* const name, const Size length) noexcept
{
    UInt32 hash{0U};
    for (Size at{0U}; at < length; ++at)
    {
        const UInt32 value = ((name[at] >= 'a') && (name[at] <= 'z')) ? static_cast<UInt32>(name[at] - 32)
                                                                      : static_cast<UInt8>(name[at]);
        hash = (((hash >> 1U) | (hash << 15U)) + (value & 0xFFU)) & 0xFFFFU;
        hash = (((hash >> 1U) | (hash << 15U)) + (value >> 8U)) & 0xFFFFU;
    }
    return hash;
}

/// SetChecksum of count entries, skipping the File entry's bytes 2 and 3.
[[nodiscard]] inline UInt32 SetChecksum(const UInt8* const entries, const Size count) noexcept
{
    UInt32 sum{0U};
    for (Size at{0U}; at < (count * 32U); ++at)
    {
        if ((at != 2U) && (at != 3U))
        {
            sum = (((sum >> 1U) | (sum << 15U)) + entries[at]) & 0xFFFFU;
        }
    }
    return sum;
}

/// A file the writer places: Name as stored, its File entry at directory
/// Index; its clusters First, First + Stride,... (Stride 0: contiguous and
/// NoFatChain; otherwise a FAT chain); Size and Valid bytes of Data (or of
/// pages FillPage(Seed + page) when Data is nullptr), of which the first
/// ExfatMaximumFilePages pages are written (the library reads no further);
/// Attributes; Benign vendor extension entries after the names;
/// ChecksumDelta and HashDelta added to the sums to break them.
struct FileSpec
{
    const char* Name;
    UInt32 Index;
    UInt32 First;
    UInt32 Stride;
    UInt64 Size;
    UInt64 Valid;
    const UInt8* Data;
    UInt32 Seed;
    UInt32 Attributes;
    UInt32 Benign;
    UInt32 ChecksumDelta;
    UInt32 HashDelta;
};

/// The timestamp the writer records: 2025-06-15 12:30:00.
inline constexpr UInt32 WriterTimestamp{((2025U - 1980U) << 25U) | (6U << 21U) | (15U << 16U) | (12U << 11U) |
                                        (30U << 5U)};

[[nodiscard]] inline bool PlaceFile(TestDevice& device, const Layout& layout, const FileSpec& spec) noexcept
{
    const Size length = std::strlen(spec.Name);
    const Size names = (length + 14U) / 15U;
    const Size entries = 2U + names + spec.Benign;
    const UInt32 count = static_cast<UInt32>((spec.Size + ClusterBytes(layout) - 1U) / ClusterBytes(layout));
    if ((length == 0U) || (length > 255U) || (entries > 19U))
    {
        return false;
    }
    UInt8 set[19U * 32U]{};
    set[0U] = 0x85U;
    set[1U] = static_cast<UInt8>(entries - 1U);
    Put16(set + 4U, spec.Attributes);
    Put32(set + 8U, WriterTimestamp);
    Put32(set + 12U, WriterTimestamp);
    Put32(set + 16U, WriterTimestamp);
    UInt8* const stream = set + 32U;
    stream[0U] = 0xC0U;
    stream[1U] = static_cast<UInt8>((count == 0U) ? 0x01U : ((spec.Stride == 0U) ? 0x03U : 0x01U));
    stream[3U] = static_cast<UInt8>(length);
    Put16(stream + 4U, (NameHash(spec.Name, length) + spec.HashDelta) & 0xFFFFU);
    Put64(stream + 8U, spec.Valid);
    Put32(stream + 20U, (count == 0U) ? 0U : spec.First);
    Put64(stream + 24U, spec.Size);
    for (Size at{0U}; at < length; ++at)
    {
        UInt8* const name = set + ((2U + (at / 15U)) * 32U);
        name[0U] = 0xC1U;
        Put16(name + 2U + ((at % 15U) * 2U), static_cast<UInt8>(spec.Name[at]));
    }
    for (Size extra{0U}; extra < spec.Benign; ++extra)
    {
        // A vendor extension entry: a GUID and vendor data.
        UInt8* const vendor = set + ((2U + names + extra) * 32U);
        vendor[0U] = 0xE0U;
        for (Size at{2U}; at < 32U; ++at)
        {
            vendor[at] = static_cast<UInt8>((at * 7U) + extra);
        }
    }
    Put16(set + 2U, (SetChecksum(set, entries) + spec.ChecksumDelta) & 0xFFFFU);
    bool done{true};
    for (UInt32 at{0U}; done && (at < entries); ++at)
    {
        done = WriteEntry(device, layout, spec.Index + at, set + (at * 32U));
    }
    const UInt32 step = (spec.Stride == 0U) ? 1U : spec.Stride;
    for (UInt32 index{0U}; done && (index < count); ++index)
    {
        const UInt32 cluster = spec.First + (index * step);
        done = InHeap(layout, cluster) && !IsAllocated(device, layout, cluster);
        SetAllocated(device, layout, cluster, true);
        if (spec.Stride != 0U)
        {
            WriteFat(device, layout, cluster, ((index + 1U) < count) ? (cluster + step) : EndOfChain);
        }
    }
    static UInt8 page[ExfatPageBytes];
    const UInt64 limit = 6U * ExfatPageBytes; // Fixtures write six pages; large-file tests write through v2.
    for (UInt64 written{0U}; done && (written < spec.Size) && (written < limit); written += SectorBytes)
    {
        if ((written % ExfatPageBytes) == 0U)
        {
            const UInt32 index = static_cast<UInt32>(written / ExfatPageBytes);
            FillPage(page, spec.Seed + index);
            if (spec.Data != nullptr)
            {
                std::memset(page, 0, sizeof(page));
                const UInt64 left = spec.Size - written;
                std::memcpy(page, spec.Data + written, (left < ExfatPageBytes) ? left : ExfatPageBytes);
            }
        }
        const UInt32 cluster = spec.First + (static_cast<UInt32>(written / ClusterBytes(layout)) * step);
        done = device.Write(ClusterLba(layout, cluster) + ((written % ClusterBytes(layout)) / SectorBytes),
                            page + (written % ExfatPageBytes));
    }
    return done;
}

/// What the invariant checker found. Errors counts what no crash may leave
///: a wrong boot checksum (main or backup region), more than
/// one FAT, a cluster reached twice or reached but free in the bitmap, a
/// broken or overlong chain, a malformed entry set (count, types, order,
/// SetChecksum, NameHash), a valid data length that is not the data length,
/// an empty file with a cluster or NoFatChain, an in-use entry after the end
/// of the directory, a set cut short, two files of one name. The other
/// fields are what a crash may leave: clusters allocated but reached from
/// nothing (fsck.exfat does not report them) and VolumeDirty.
struct CheckReport
{
    bool Parsed;
    Size Errors;
    const char* FirstError;
    UInt64 ErrorDetail;
    UInt32 LostClusters;
    UInt32 FreeClusters;
    bool Dirty;
    Size Files;

    [[nodiscard]] bool IsClean() const noexcept
    {
        return Parsed && (Errors == 0U) && (LostClusters == 0U) && !Dirty;
    }
};

namespace Detail
{
inline constexpr Size MaximumCheckedClusters{Size{1U} << 20U};
inline constexpr Size MaximumCheckedFiles{256U};

struct CheckerState
{
    UInt64 Reached[MaximumCheckedClusters / 64U];
    UInt8 Bitmap[MaximumCheckedClusters / 8U];
    UInt8 Names[MaximumCheckedFiles][255U];
    Size NameLengths[MaximumCheckedFiles];
    UInt8 Set[19U * 32U];
};

inline CheckerState g_checker{};

inline void Error(CheckReport& report, const char* const what, const UInt64 detail) noexcept
{
    if (report.Errors == 0U)
    {
        report.FirstError = what;
        report.ErrorDetail = detail;
    }
    ++report.Errors;
}

/// Marks a cluster reached: an error when it lies outside the heap, was
/// reached before, or is free in the bitmap.
inline bool Reach(const Layout& layout, const UInt32 cluster, CheckReport& report) noexcept
{
    if (!InHeap(layout, cluster))
    {
        Error(report, "a cluster lies outside the heap", cluster);
        return false;
    }
    UInt64& word = g_checker.Reached[cluster / 64U];
    const UInt64 bit = UInt64{1U} << (cluster % 64U);
    if ((word & bit) != 0U)
    {
        Error(report, "a cluster is reached twice", cluster);
        return false;
    }
    word |= bit;
    if ((g_checker.Bitmap[(cluster - 2U) / 8U] & (1U << ((cluster - 2U) % 8U))) == 0U)
    {
        Error(report, "a reached cluster is free in the bitmap", cluster);
    }
    return true;
}

/// Reaches the clusters of bytes of data from first: a run when contiguous,
/// else a FAT chain of exactly that many clusters.
inline void ReachData(const TestDevice& device, const Layout& layout, const UInt32 first, const UInt64 bytes,
                      const bool contiguous, CheckReport& report) noexcept
{
    const UInt64 count = (bytes + ClusterBytes(layout) - 1U) / ClusterBytes(layout);
    UInt32 cluster{first};
    for (UInt64 index{0U}; index < count; ++index)
    {
        if (!Reach(layout, cluster, report))
        {
            return;
        }
        const UInt32 next = contiguous ? (cluster + 1U) : ReadFat(device, layout, cluster);
        if (!contiguous && ((index + 1U) == count) && (next != EndOfChain))
        {
            Error(report, "a chain does not end with its data", cluster);
        }
        cluster = next;
    }
}

/// Checks one complete entry set of count entries (in g_checker.Set) whose
/// File entry is directory entry index.
inline void CheckSet(const TestDevice& device, const Layout& layout, const Size count, const UInt32 index,
                     CheckReport& report) noexcept
{
    const UInt8* const set = g_checker.Set;
    const UInt8* const stream = set + 32U;
    const Size length{stream[3U]};
    const Size names = (length + 14U) / 15U;
    if ((count < 3U) || (count > 19U) || (stream[0U] != 0xC0U) || (length == 0U) || (count < (2U + names)))
    {
        Error(report, "an entry set's count or Stream Extension is wrong", index);
        return;
    }
    for (Size at{2U}; at < count; ++at)
    {
        const UInt8 type = set[at * 32U];
        if ((at < (2U + names)) ? (type != 0xC1U) : ((type & 0x20U) == 0U))
        {
            Error(report, "an entry set's File Name or benign entries are wrong", index);
            return;
        }
    }
    if (SetChecksum(set, count) != Get16(set + 2U))
    {
        Error(report, "an entry set's SetChecksum is wrong", index);
    }
    UInt8* const name = g_checker.Names[report.Files % MaximumCheckedFiles];
    bool ascii{true};
    for (Size at{0U}; at < length; ++at)
    {
        const UInt32 value = Get16(set + ((2U + (at / 15U)) * 32U) + 2U + ((at % 15U) * 2U));
        ascii = ascii && (value < 0x80U);
        name[at] = static_cast<UInt8>(((value >= 'A') && (value <= 'Z')) ? (value + 32U) : value);
    }
    if (ascii && (NameHash(reinterpret_cast<const char*>(name), length) != Get16(stream + 4U)))
    {
        Error(report, "an entry set's NameHash is wrong", index);
    }
    const UInt32 flags{stream[1U]};
    const UInt64 valid = Get64(stream + 8U);
    const UInt32 first = Get32(stream + 20U);
    const UInt64 size = Get64(stream + 24U);
    if (valid != size)
    {
        Error(report, "a valid data length is not the data length", index);
    }
    if ((size == 0U) && ((first != 0U) || ((flags & 0x02U) != 0U)))
    {
        Error(report, "an empty file has a cluster or NoFatChain", index);
    }
    if (size != 0U)
    {
        ReachData(device, layout, first, size, (flags & 0x02U) != 0U, report);
    }
    if (report.Files >= MaximumCheckedFiles)
    {
        Error(report, "too many files to check", index);
        return;
    }
    g_checker.NameLengths[report.Files] = length;
    for (Size other{0U}; other < report.Files; ++other)
    {
        if ((g_checker.NameLengths[other] == length) && (std::memcmp(g_checker.Names[other], name, length) == 0))
        {
            Error(report, "two files have one name", index);
        }
    }
    ++report.Files;
}
} // Namespace Detail

/// Checks the volume on device against invariants.
[[nodiscard]] inline CheckReport CheckExfatVolume(const TestDevice& device) noexcept
{
    using Detail::g_checker;
    CheckReport report{};
    Layout layout{};
    if (!ParseLayout(device, layout) || ((layout.Clusters + 2U) > Detail::MaximumCheckedClusters))
    {
        return report;
    }
    report.Parsed = true;
    UInt8 boot[SectorBytes]{};
    static_cast<void>(device.Read(0U, boot));
    report.Dirty = (boot[106U] & 0x02U) != 0U;
    if (!BootChecksumHolds(device, 0U))
    {
        Detail::Error(report, "the main boot checksum is wrong", 0U);
    }
    if (!BootChecksumHolds(device, 12U))
    {
        Detail::Error(report, "the backup boot checksum is wrong", 12U);
    }
    if (layout.Fats != 1U)
    {
        Detail::Error(report, "the volume has more than one FAT", layout.Fats);
    }
    std::memset(g_checker.Reached, 0, (((layout.Clusters + 2U) / 64U) + 1U) * sizeof(UInt64));
    // The allocation bitmap, read once in its chain's order.
    const UInt64 bitmapSectors = (static_cast<UInt64>(layout.Clusters) + 4095U) / 4096U;
    for (UInt64 sector{0U}; sector < bitmapSectors; ++sector)
    {
        static_cast<void>(device.Read(ChainLba(device, layout, layout.BitmapCluster, sector),
                                      g_checker.Bitmap + (sector * SectorBytes)));
    }
    // The root directory's chain, then its entries.
    UInt32 cluster{layout.RootCluster};
    UInt64 rootClusters{0U};
    while (InHeap(layout, cluster) && (rootClusters < 4096U))
    {
        if (!Detail::Reach(layout, cluster, report))
        {
            break;
        }
        ++rootClusters;
        cluster = ReadFat(device, layout, cluster);
    }
    if (cluster != EndOfChain)
    {
        Detail::Error(report, "the root directory's chain is broken", cluster);
    }
    const UInt64 capacity = (rootClusters << layout.ClusterShift) * 16U;
    Size open{0U};
    Size expected{0U};
    UInt32 start{0U};
    bool ended{false};
    UInt8 entry[32U]{};
    for (UInt32 index{0U}; index < capacity; ++index)
    {
        static_cast<void>(ReadEntry(device, layout, index, entry));
        const UInt32 type{entry[0U]};
        if (ended || (type == 0U) || ((type & 0x80U) == 0U))
        {
            if (ended && ((type & 0x80U) != 0U))
            {
                Detail::Error(report, "an entry follows the end of the directory", index);
            }
            if (open != 0U)
            {
                Detail::Error(report, "an entry set is cut short", start);
            }
            open = 0U;
            ended = ended || (type == 0U);
            continue;
        }
        if ((open != 0U) && ((type & 0x40U) != 0U))
        {
            std::memcpy(g_checker.Set + (open * 32U), entry, 32U);
            ++open;
            if (open == expected)
            {
                Detail::CheckSet(device, layout, expected, start, report);
                open = 0U;
            }
            continue;
        }
        if (open != 0U)
        {
            Detail::Error(report, "an entry set is cut short", start);
            open = 0U;
        }
        if (type == 0x85U)
        {
            std::memcpy(g_checker.Set, entry, 32U);
            open = 1U;
            expected = 1U + entry[1U];
            start = index;
            if (expected > 19U)
            {
                Detail::Error(report, "an entry set's count is wrong", index);
                open = 0U;
            }
        }
        else if ((type == 0x81U) || (type == 0x82U))
        {
            Detail::ReachData(device, layout, Get32(entry + 20U), Get64(entry + 24U), false, report);
        }
        else if ((type & 0x40U) != 0U)
        {
            Detail::Error(report, "a secondary entry belongs to no set", index);
        }
        else if ((type != 0x83U) && ((type & 0x20U) == 0U))
        {
            Detail::Error(report, "an unknown critical primary entry", index);
        }
    }
    if (open != 0U)
    {
        Detail::Error(report, "an entry set is cut short", start);
    }
    if (layout.BitmapBytes < ((layout.Clusters + 7U) / 8U))
    {
        Detail::Error(report, "the bitmap does not cover the heap", layout.BitmapBytes);
    }
    for (UInt32 candidate{2U}; candidate <= (layout.Clusters + 1U); ++candidate)
    {
        const UInt32 bit = candidate - 2U;
        const bool reached = (g_checker.Reached[candidate / 64U] & (UInt64{1U} << (candidate % 64U))) != 0U;
        const bool allocated = (g_checker.Bitmap[bit / 8U] & (1U << (bit % 8U))) != 0U;
        report.LostClusters += (allocated && !reached) ? 1U : 0U;
        report.FreeClusters += allocated ? 0U : 1U;
    }
    return report;
}

/// Prints a report's findings under label.
inline void PrintReport(const char* const label, const CheckReport& report) noexcept
{
    static_cast<void>(std::fprintf(stderr, "%s: %s (%llu), %u lost, dirty %d, %zu files\n", label,
                                   (report.FirstError != nullptr) ? report.FirstError : "no invariant broken",
                                   static_cast<unsigned long long>(report.ErrorDetail), report.LostClusters,
                                   report.Dirty ? 1 : 0, report.Files));
}
} // Namespace Asteria::Test::Exfat

#endif // ASTERIA_TESTS_USER_FILESYSTEM_EXFAT_EXFAT_TEST_SUPPORT_HPP_INCLUDED
