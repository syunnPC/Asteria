// SPDX-License-Identifier: Apache-2.0
#include "User/Filesystem/Fat/FatVolume.hpp"

#include <cstdio>
#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Filesystem/Fat/FatTestSupport.hpp"
#include "User/Filesystem/Fat/FatWorkload.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Fat::Bytes;
using Fat::CheckReport;
using Fat::CheckVolume;
using Fat::FormatOptions;
using Fat::Layout;
using Fat::NoBudget;
using Fat::SparseDisk;
using Fat::SparseStorage;
using Fat::TestDevice;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::FatCrashPoint;
using User::Filesystem::FatFileSize;
using User::Filesystem::FatGeometry;
using User::Filesystem::FatObject;
using User::Filesystem::FatPageBytes;
using User::Filesystem::FatStatus;
using User::Filesystem::FatType;
using User::Filesystem::FatVolume;

// The service keeps its volume static: one sector buffer and small state.
static_assert(sizeof(FatVolume) <= 36U * 1024U, "bounded v2 chain and name scratch stay within 36 KiB");

SparseStorage<16U> g_baseStorage;
SparseStorage<12U> g_overlayStorage;
SparseDisk g_base{g_baseStorage.Keys, g_baseStorage.Data, 16U};
SparseDisk g_overlay{g_overlayStorage.Keys, g_overlayStorage.Data, 12U};
TestDevice g_device{g_base};
TestDevice g_crashDevice{g_base, &g_overlay};
FatVolume g_volume;
UInt8 g_page[FatPageBytes];
UInt8 g_expected[FatPageBytes];

/// FAT12, 512-byte clusters, 4057 clusters, a label: FAT12 entries 341 and
/// 682 straddle FAT sectors.
constexpr FormatOptions Fat12Options{.Sectors = 4096U,
                                     .SectorsPerCluster = 1U,
                                     .Reserved = 1U,
                                     .Fats = 2U,
                                     .RootEntries = 224U,
                                     .RootClusters = 0U,
                                     .HighNibble = 0U,
                                     .Label = true};
/// FAT12 with 8 KiB clusters (larger than a page).
constexpr FormatOptions Fat12LargeOptions{.Sectors = 16384U,
                                          .SectorsPerCluster = 16U,
                                          .Reserved = 4U,
                                          .Fats = 2U,
                                          .RootEntries = 512U,
                                          .RootClusters = 0U,
                                          .HighNibble = 0U,
                                          .Label = false};
/// FAT12 of 124 clusters: fills up.
constexpr FormatOptions Fat12TinyOptions{.Sectors = 128U,
                                         .SectorsPerCluster = 1U,
                                         .Reserved = 1U,
                                         .Fats = 2U,
                                         .RootEntries = 16U,
                                         .RootClusters = 0U,
                                         .HighNibble = 0U,
                                         .Label = false};
/// FAT16 with 8 KiB clusters, as disk.
constexpr FormatOptions Fat16Options{.Sectors = 70000U,
                                     .SectorsPerCluster = 16U,
                                     .Reserved = 4U,
                                     .Fats = 2U,
                                     .RootEntries = 512U,
                                     .RootClusters = 0U,
                                     .HighNibble = 0U,
                                     .Label = true};
/// FAT16 with one FAT and a root of 16 entries.
constexpr FormatOptions Fat16SmallOptions{.Sectors = 9000U,
                                          .SectorsPerCluster = 2U,
                                          .Reserved = 1U,
                                          .Fats = 1U,
                                          .RootEntries = 16U,
                                          .RootClusters = 0U,
                                          .HighNibble = 0U,
                                          .Label = true};
/// FAT32, 512-byte clusters, a reserved nibble 0xA in every entry.
constexpr FormatOptions Fat32Options{.Sectors = 66700U,
                                     .SectorsPerCluster = 1U,
                                     .Reserved = 32U,
                                     .Fats = 2U,
                                     .RootEntries = 0U,
                                     .RootClusters = 2U,
                                     .HighNibble = 0xAU,
                                     .Label = true};
/// FAT32 with 4 KiB clusters.
constexpr FormatOptions Fat32LargeOptions{.Sectors = 528000U,
                                          .SectorsPerCluster = 8U,
                                          .Reserved = 32U,
                                          .Fats = 2U,
                                          .RootEntries = 0U,
                                          .RootClusters = 1U,
                                          .HighNibble = 0U,
                                          .Label = false};

[[nodiscard]] bool Prepare(const FormatOptions& options, Layout& layout) noexcept
{
    g_base.Reset(options.Sectors);
    g_device.Arm(NoBudget);
    return Format(g_device, options, layout);
}

[[nodiscard]] FatStatus MountBase(FatVolume& volume, const UInt64 serial = 7U) noexcept
{
    g_device.Arm(NoBudget);
    return volume.Mount(g_device.Get(), serial);
}

[[nodiscard]] FatStatus OpenName(FatVolume& volume, const char* const name, FatObject& object) noexcept
{
    return volume.Open(Bytes(name), std::strlen(name), object);
}

[[nodiscard]] FatStatus CreateName(FatVolume& volume, const char* const name, FatObject& object) noexcept
{
    return volume.Create(Bytes(name), std::strlen(name), object);
}

[[nodiscard]] bool WritePages(FatVolume& volume, const FatObject& object, const UInt32 seed,
                              const UInt64 count) noexcept
{
    bool done{true};
    for (UInt64 index{0U}; done && (index < count); ++index)
    {
        done = Fat::WriteSeeded(volume, object, index, seed + static_cast<UInt32>(index));
    }
    return done;
}

/// Whether every page of object below count reads as seed + index, and the
/// next one as zeros.
[[nodiscard]] bool PagesRead(FatVolume& volume, const FatObject& object, const UInt32 seed, const UInt64 count) noexcept
{
    bool same{true};
    for (UInt64 index{0U}; same && (index <= count) && (index < 6U); ++index)
    {
        Fat::FillPage(g_expected, (index < count) ? (seed + static_cast<UInt32>(index)) : 0U);
        same = (volume.ReadPage(object, index, g_page) == FatStatus::Done) &&
               (std::memcmp(g_page, g_expected, FatPageBytes) == 0);
    }
    return same;
}

[[nodiscard]] UInt8 DirectoryByte(const Layout& layout, const UInt32 index, const Size offset) noexcept
{
    UInt8 sector[Fat::SectorBytes]{};
    static_cast<void>(g_device.Read(Fat::DirectoryLba(g_device, layout, index), sector));
    return sector[((index % 16U) * 32U) + offset];
}

void SetDirectoryByte(const Layout& layout, const UInt32 index, const Size offset, const UInt8 value) noexcept
{
    UInt8 sector[Fat::SectorBytes]{};
    const UInt64 lba = Fat::DirectoryLba(g_device, layout, index);
    static_cast<void>(g_device.Read(lba, sector));
    sector[((index % 16U) * 32U) + offset] = value;
    static_cast<void>(g_device.Write(lba, sector));
}

void SetBootByte(const Size offset, const UInt8 value) noexcept
{
    UInt8 sector[Fat::SectorBytes]{};
    static_cast<void>(g_device.Read(0U, sector));
    sector[offset] = value;
    static_cast<void>(g_device.Write(0U, sector));
}

[[nodiscard]] bool ShortNameIs(const Layout& layout, const UInt32 index, const char* const name,
                               const UInt8 caseFlags) noexcept
{
    bool same{DirectoryByte(layout, index, 12U) == caseFlags};
    for (Size at{0U}; at < 11U; ++at)
    {
        same = same && (DirectoryByte(layout, index, at) == static_cast<UInt8>(name[at]));
    }
    return same;
}

void TestGeometry(TestContext& context) noexcept
{
    Layout layout{};
    UInt8 boot[Fat::SectorBytes]{};
    FatGeometry geometry{};
    const bool formatted = Prepare(Fat12Options, layout) && g_device.Read(0U, boot);
    ASTERIA_EXPECT(context, "a FAT12 boot sector decodes to the formatter's layout", formatted,
                   User::Filesystem::ReadFatGeometry(boot, Fat12Options.Sectors, geometry) == FatStatus::Done,
                   geometry.Type == FatType::Fat12, geometry.FatStart == 1U, geometry.FatSectors == layout.FatSectors,
                   geometry.RootStart == layout.RootStart, geometry.DataStart == layout.DataStart,
                   geometry.Clusters == layout.Clusters, geometry.RootEntries == 224U, geometry.ClusterShift == 0U,
                   geometry.FatCount == 2U, !geometry.SingleActiveFat, geometry.FsInfoSector == 0U,
                   geometry.RootCluster == 0U);

    struct Mutation
    {
        Size Offset;
        UInt8 Value;
        Size SecondOffset;
        UInt8 SecondValue;
        FatStatus Expected;
    };
    static constexpr Mutation Fat12Mutations[]{
        {0U, 0x00U, 0U, 0x00U, FatStatus::NotFat},   // No jump
        {2U, 0x00U, 2U, 0x00U, FatStatus::NotFat},   // A short jump without its NOP
        {0U, 0xE9U, 0U, 0xE9U, FatStatus::Done},     // A near jump
        {510U, 0U, 511U, 0U, FatStatus::Done},       // No 0x55AA signature
        {12U, 0x04U, 12U, 0x04U, FatStatus::NotFat}, // 1024-byte sectors
        {13U, 3U, 13U, 3U, FatStatus::NotFat},       // A cluster of three sectors
        {13U, 0U, 13U, 0U, FatStatus::NotFat},       {14U, 0U, 14U, 0U, FatStatus::NotFat}, // No reserved sector
        {16U, 0U, 16U, 0U, FatStatus::NotFat},                                              // No FAT
        {16U, 3U, 16U, 3U, FatStatus::NotFat},                                              // Three FATs
        {19U, 0U, 20U, 0U, FatStatus::NotFat},                                              // No sector count
        {22U, 0U, 23U, 0U, FatStatus::NotFat},                                              // No FAT size
        {14U, 0xFFU, 15U, 0x0FU, FatStatus::NotFat},                                        // No data area
        {22U, 1U, 22U, 1U, FatStatus::Corrupt}, // A FAT too small for its clusters
        {17U, 0U, 18U, 0U, FatStatus::Corrupt}, // A FAT12 volume without root entries
    };
    Size wrong{0U};
    for (const Mutation& mutation : Fat12Mutations)
    {
        UInt8 copy[Fat::SectorBytes]{};
        std::memcpy(copy, boot, sizeof(copy));
        copy[mutation.Offset] = mutation.Value;
        copy[mutation.SecondOffset] = mutation.SecondValue;
        FatGeometry decoded{};
        decoded.Clusters = 0x1234U;
        const FatStatus status = User::Filesystem::ReadFatGeometry(copy, Fat12Options.Sectors, decoded);
        const bool kept = (status == FatStatus::Done) || (decoded.Clusters == 0x1234U);
        if ((status != mutation.Expected) || !kept)
        {
            static_cast<void>(
                std::fprintf(stderr, "mutation at %zu gave %u\n", mutation.Offset, static_cast<unsigned>(status)));
            ++wrong;
        }
    }
    FatGeometry untouched{};
    ASTERIA_EXPECT(context, "the parameter block's sanity and type checks refuse what they must, leaving the geometry",
                   wrong == 0U,
                   User::Filesystem::ReadFatGeometry(boot, Fat12Options.Sectors - 1U, untouched) == FatStatus::NotFat,
                   untouched.Clusters == 0U);

    const bool fat16 = Prepare(Fat16Options, layout) && g_device.Read(0U, boot);
    UInt8 copy[Fat::SectorBytes]{};
    std::memcpy(copy, boot, sizeof(copy));
    copy[22U] = 0U;
    copy[23U] = 0U;
    Fat::Put32(copy + 36U, static_cast<UInt32>(layout.FatSectors));
    ASTERIA_EXPECT(context, "a FAT16 volume decodes; with FATSz16 zero it is corrupt", fat16,
                   User::Filesystem::ReadFatGeometry(boot, Fat16Options.Sectors, geometry) == FatStatus::Done,
                   geometry.Type == FatType::Fat16, geometry.ClusterShift == 4U, geometry.RootEntries == 512U,
                   User::Filesystem::ReadFatGeometry(copy, Fat16Options.Sectors, geometry) == FatStatus::Corrupt);

    const bool fat32 = Prepare(Fat32Options, layout) && g_device.Read(0U, boot);
    const bool decoded = User::Filesystem::ReadFatGeometry(boot, Fat32Options.Sectors, geometry) == FatStatus::Done;
    ASTERIA_EXPECT(context, "a FAT32 volume decodes with its root cluster and FSInfo", fat32, decoded,
                   geometry.Type == FatType::Fat32, geometry.RootCluster == 2U, geometry.FsInfoSector == 1U,
                   geometry.RootStart == 0U, geometry.FatStart == 32U, geometry.Clusters == layout.Clusters);
    static constexpr Mutation Fat32Mutations[]{
        {22U, 1U, 22U, 1U, FatStatus::Corrupt},       // FATSz16 nonzero on a FAT32 cluster count
        {17U, 16U, 17U, 16U, FatStatus::Corrupt},     // Root entries on FAT32
        {44U, 0U, 44U, 0U, FatStatus::Corrupt},       // Root cluster 0
        {46U, 0x10U, 46U, 0x10U, FatStatus::Corrupt}, // Root cluster beyond the volume
        {40U, 0x82U, 40U, 0x82U, FatStatus::Corrupt}, // The active FAT is not one of the two
    };
    wrong = 0U;
    for (const Mutation& mutation : Fat32Mutations)
    {
        std::memcpy(copy, boot, sizeof(copy));
        copy[mutation.Offset] = mutation.Value;
        copy[mutation.SecondOffset] = mutation.SecondValue;
        FatGeometry refused{};
        wrong +=
            (User::Filesystem::ReadFatGeometry(copy, Fat32Options.Sectors, refused) != mutation.Expected) ? 1U : 0U;
    }
    std::memcpy(copy, boot, sizeof(copy));
    copy[40U] = 0x81U;
    copy[48U] = 40U;
    FatGeometry single{};
    ASTERIA_EXPECT(context,
                   "FAT32 refusals; a single active FAT is read from its copy; an FSInfo past the reserved area "
                   "is none",
                   wrong == 0U,
                   User::Filesystem::ReadFatGeometry(copy, Fat32Options.Sectors, single) == FatStatus::Done,
                   single.SingleActiveFat, single.FatStart == (32U + layout.FatSectors), single.FsInfoSector == 0U);
}

void TestMount(TestContext& context) noexcept
{
    Layout layout{};
    FatVolume volume;
    FatObject object{};
    ASTERIA_EXPECT(
        context, "an unmounted volume refuses every operation", !volume.IsMounted(),
        OpenName(volume, "a", object) == FatStatus::Refused, CreateName(volume, "a", object) == FatStatus::Refused,
        volume.Commit(FatCrashPoint::None) == FatStatus::Refused, FatVolume::QueryCommit() == FatStatus::Unsupported);

    g_base.Reset(4096U);
    ASTERIA_EXPECT(context, "a blank disk holds no FAT volume", MountBase(volume) == FatStatus::NotFat,
                   !volume.IsMounted());

    struct DirtyCase
    {
        const FormatOptions* Options;
        Size BootOffset;
        UInt32 ClearedBits;
    };
    static constexpr DirtyCase Cases[]{
        {&Fat12Options, 0x25U, 0U},        {&Fat16Options, 0x25U, 0U}, {&Fat16Options, 0U, 0x8000U},
        {&Fat16Options, 0U, 0x4000U},      {&Fat32Options, 0x41U, 0U}, {&Fat32Options, 0U, 0x0800'0000U},
        {&Fat32Options, 0U, 0x0400'0000U},
    };
    Size wrong{0U};
    for (const DirtyCase& entry : Cases)
    {
        const bool formatted = Prepare(*entry.Options, layout);
        const bool clean = formatted && (MountBase(volume) == FatStatus::Done) && !volume.IsReadOnly();
        if (entry.BootOffset != 0U)
        {
            SetBootByte(entry.BootOffset, 1U);
        }
        else
        {
            Fat::WriteEntry(g_device, layout, 0U, 1U, Fat::ReadEntry(g_device, layout, 0U, 1U) & ~entry.ClearedBits);
        }
        g_device.Arm(NoBudget);
        const bool readOnly = (MountBase(volume) == FatStatus::Done) && volume.IsReadOnly() &&
                              (CreateName(volume, "x", object) == FatStatus::Refused) &&
                              (volume.Commit(FatCrashPoint::None) == FatStatus::Done) && (g_device.GetWrites() == 0U);
        wrong += (clean && readOnly) ? 0U : 1U;
    }
    ASTERIA_EXPECT(context, "a dirty volume (either mark, or a hard-error bit) mounts read-only and is never written",
                   wrong == 0U);

    // FAT32 with mirroring off and FAT 1 active: read-only, read through FAT 1.
    const bool formatted = Prepare(Fat32Options, layout);
    static constexpr UInt32 Chain[]{40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 60U, 61U, 62U, 63U, 64U, 65U, 66U, 67U};
    const bool placed =
        Fat::PlaceFile(g_device, layout, 1U, "TWOPAGE BIN", 0U, nullptr, Chain, 16U, 8192U, 0x0FFF'FFFFU, 500U);
    Fat::WriteEntry(g_device, layout, 0U, 47U, 0xA000'0000U);
    SetBootByte(40U, 0x81U);
    ASTERIA_EXPECT(context, "a FAT32 volume with one active FAT mounts read-only and follows that FAT", formatted,
                   placed, MountBase(volume) == FatStatus::Done, volume.IsReadOnly(),
                   OpenName(volume, "twopage.bin", object) == FatStatus::Done, PagesRead(volume, object, 500U, 2U),
                   Fat::WriteSeeded(volume, object, 0U, 1U) == false);

    const bool again = Prepare(Fat16Options, layout);
    g_device.SetFailReads(true);
    const FatStatus failed = MountBase(volume);
    g_device.SetFailReads(false);
    ASTERIA_EXPECT(context, "a device failure fails the mount and every later operation until the next mount", again,
                   failed == FatStatus::Failed, OpenName(volume, "a", object) == FatStatus::Failed,
                   MountBase(volume) == FatStatus::Done, OpenName(volume, "a", object) == FatStatus::NotFound);

    // A FAT32 root whose first cluster holds no end mark and whose chain
    // reaches a free entry.
    const bool fat32 = Prepare(Fat32LargeOptions, layout);
    UInt8 deleted[Fat::SectorBytes]{};
    for (Size at{0U}; at < Fat::SectorBytes; at += 32U)
    {
        deleted[at] = 0xE5U;
        deleted[at + 11U] = 0x20U;
    }
    for (UInt64 sector{0U}; sector < 8U; ++sector)
    {
        static_cast<void>(g_device.Write(Fat::ClusterLba(layout, 2U) + sector, deleted));
    }
    const bool full = MountBase(volume) == FatStatus::Done;
    Fat::WriteEntry(g_device, layout, 0U, 2U, 0U);
    ASTERIA_EXPECT(context, "a FAT32 root chain that leaves the volume is corrupt", fat32, full,
                   MountBase(volume) == FatStatus::Corrupt);
}

void TestFatEntries(TestContext& context) noexcept
{
    Layout layout{};
    FatVolume& volume = g_volume;
    FatObject object{};
    // Reading through FAT12 entries 341 and 682, which straddle sectors, and
    // chains ending in 0xFF8.
    static constexpr UInt32 Straddling[]{336U, 337U, 338U, 339U, 340U, 341U, 342U, 343U,
                                         678U, 679U, 680U, 681U, 682U, 683U, 684U, 685U};
    const bool formatted = Prepare(Fat12Options, layout);
    const bool placed =
        Fat::PlaceFile(g_device, layout, 1U, "STRADDLEBIN", 0U, nullptr, Straddling, 16U, 8192U, 0xFF8U, 900U);
    ASTERIA_EXPECT(context, "a FAT12 chain through entries that straddle FAT sectors reads", formatted, placed,
                   MountBase(volume) == FatStatus::Done, OpenName(volume, "straddle.bin", object) == FatStatus::Done,
                   PagesRead(volume, object, 900U, 2U), CheckVolume(g_device).IsClean());

    // Allocation through the straddling entries: clusters 2-325 are taken, so
    // a two-page file gets 326-341 and ends on entry 341; replacing it frees
    // that end mark again.
    UInt32 fill[324U]{};
    for (UInt32 index{0U}; index < 324U; ++index)
    {
        fill[index] = index + 2U;
    }
    const bool filled = Prepare(Fat12Options, layout) && Fat::PlaceFile(g_device, layout, 1U, "FILL    BIN", 0U,
                                                                        nullptr, fill, 324U, 324U * 512U, 0xFFFU, 0U);
    FatObject twin{};
    const bool written =
        filled && (MountBase(volume) == FatStatus::Done) && (CreateName(volume, "edge", object) == FatStatus::Done) &&
        WritePages(volume, object, 910U, 2U) && (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    const UInt32 tail = Fat::ReadEntry(g_device, layout, 0U, 341U);
    const UInt32 inner = Fat::ReadEntry(g_device, layout, 0U, 340U);
    const CheckReport first = CheckVolume(g_device);
    const bool replaced =
        (Fat::WriteSeeded(volume, object, 0U, 920U)) && (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    const UInt32 freed = Fat::ReadEntry(g_device, layout, 0U, 341U);
    const CheckReport second = CheckVolume(g_device);
    ASTERIA_EXPECT(context, "FAT12 entries straddling a sector boundary are written in both sectors", written,
                   tail == 0xFFFU, inner == 341U, first.IsClean(), replaced, freed == 0U,
                   Fat::ReadEntry(g_device, layout, 1U, 341U) == 0U, second.IsClean(),
                   MountBase(volume) == FatStatus::Done, OpenName(volume, "edge", twin) == FatStatus::Done,
                   Fat::WriteSeeded(volume, twin, 1U, 921U), volume.Commit(FatCrashPoint::None) == FatStatus::Done,
                   PagesRead(volume, twin, 920U, 2U), CheckVolume(g_device).IsClean());

    // FAT32 keeps the reserved high nibble of every entry it writes.
    const bool fat32 = Prepare(Fat32Options, layout) && (MountBase(volume) == FatStatus::Done);
    const bool created = (CreateName(volume, "nibble", object) == FatStatus::Done) &&
                         WritePages(volume, object, 930U, 1U) &&
                         (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    bool nibbles{true};
    UInt32 cluster{4U};
    for (Size index{0U}; index < 8U; ++index, ++cluster)
    {
        const UInt32 raw = Fat::ReadEntry(g_device, layout, 0U, cluster);
        nibbles = nibbles && ((raw >> 28U) == 0xAU) &&
                  ((raw & 0x0FFF'FFFFU) == ((index < 7U) ? (cluster + 1U) : 0x0FFF'FFFFU));
    }
    const UInt32 header = Fat::ReadEntry(g_device, layout, 1U, 1U);
    const bool rewritten =
        Fat::WriteSeeded(volume, object, 0U, 931U) && (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    ASTERIA_EXPECT(context, "FAT32 entries keep their high nibble; chains end in 0x0FFFFFFF", fat32, created, nibbles,
                   header == 0xAFFF'FFFFU, rewritten, Fat::ReadEntry(g_device, layout, 0U, 4U) == 0xA000'0000U,
                   Fat::ReadEntry(g_device, layout, 0U, 12U) == 0xA000'000DU, CheckVolume(g_device).IsClean());

    const bool fat16 = Prepare(Fat16Options, layout) && (MountBase(volume) == FatStatus::Done) &&
                       (CreateName(volume, "mark", object) == FatStatus::Done) &&
                       WritePages(volume, object, 940U, 3U) && (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    ASTERIA_EXPECT(context, "a FAT16 chain of 8 KiB clusters holds three pages in two clusters and ends in 0xFFFF",
                   fat16, Fat::ReadEntry(g_device, layout, 0U, 2U) == 3U,
                   Fat::ReadEntry(g_device, layout, 0U, 3U) == 0xFFFFU, Fat::ReadEntry(g_device, layout, 0U, 4U) == 0U,
                   PagesRead(volume, object, 940U, 3U), CheckVolume(g_device).IsClean());
}

void TestNames(TestContext& context) noexcept
{
    Layout layout{};
    FatVolume& volume = g_volume;
    FatObject object{};
    const bool formatted = Prepare(Fat16Options, layout);
    static constexpr UInt32 One[]{100U};
    static constexpr UInt32 Two[]{101U};
    static constexpr UInt32 Three[]{102U};
    static constexpr UInt32 Four[]{103U};
    // Entries: 0 the label, 1-3 a long name, 4 an 8.3 name, 5-7 a long name
    // whose checksum is wrong, 8-11 one with an ordinal gap, 12 a deleted
    // file, 13 a directory.
    const bool placed =
        Fat::PlaceFile(g_device, layout, 3U, "LONGFI~1TXT", 0U, "Long-File.Name.txt", One, 1U, 100U, 0xFFFFU, 1U) &&
        Fat::PlaceFile(g_device, layout, 4U, "README  TXT", 0U, nullptr, Two, 1U, 13U, 0xFFFFU, 2U) &&
        Fat::PlaceFile(g_device, layout, 7U, "BROKEN~1BIN", 0U, "broken-name.bin", Three, 1U, 10U, 0xFFFFU, 3U) &&
        Fat::PlaceFile(g_device, layout, 11U, "GAPPED~1BIN", 0U, "gapped-name-of-thirty-chars.bin", Four, 1U, 10U,
                       0xFFFFU, 4U) &&
        Fat::PlaceFile(g_device, layout, 12U, "GONE    TXT", 0U, nullptr, nullptr, 0U, 0U, 0xFFFFU, 0U) &&
        Fat::PlaceFile(g_device, layout, 13U, "SUBDIR     ", 0U, nullptr, nullptr, 0U, 0U, 0xFFFFU, 0U);
    SetDirectoryByte(layout, 6U, 13U, static_cast<UInt8>(DirectoryByte(layout, 6U, 13U) + 1U));
    SetDirectoryByte(layout, 9U, 0U, 3U);
    SetDirectoryByte(layout, 12U, 0U, 0xE5U);
    SetDirectoryByte(layout, 13U, 11U, 0x10U);
    FatFileSize scratch{};
    FatObject found{};
    FatObject upper{};
    FatObject plain{};
    ASTERIA_EXPECT(context, "long names match case-insensitively; an entry with one is not matched by its short name",
                   formatted, placed, MountBase(volume) == FatStatus::Done,
                   OpenName(volume, "long-file.name.txt", found) == FatStatus::Done, found.Identifier == 3U,
                   found.Generation == 7U, OpenName(volume, "LONG-FILE.NAME.TXT", upper) == FatStatus::Done,
                   upper.Identifier == 3U, OpenName(volume, "longfi~1.txt", object) == FatStatus::NotFound,
                   OpenName(volume, "long-file.name.tx", object) == FatStatus::NotFound,
                   OpenName(volume, "long-file.name.txtx", object) == FatStatus::NotFound,
                   OpenName(volume, "readme.txt", plain) == FatStatus::Done, plain.Identifier == 4U);
    ASTERIA_EXPECT(context, "a long name whose checksum or ordinals are wrong falls back to the short name",
                   OpenName(volume, "broken-name.bin", object) == FatStatus::NotFound,
                   OpenName(volume, "broken~1.bin", object) == FatStatus::Done, object.Identifier == 7U,
                   OpenName(volume, "gapped-name-of-thirty-chars.bin", object) == FatStatus::NotFound,
                   OpenName(volume, "gapped~1.bin", object) == FatStatus::Done, object.Identifier == 11U);
    ASTERIA_EXPECT(context, "deleted entries and the label are absent; directories have their own kind",
                   OpenName(volume, "gone.txt", object) == FatStatus::NotFound,
                   OpenName(volume, "subdir", object) == FatStatus::Done,
                   OpenName(volume, "asteria", object) == FatStatus::NotFound,
                   volume.Stat(FatObject{.Identifier = 0U, .Generation = 7U}, scratch) == FatStatus::StaleObject,
                   volume.Stat(FatObject{.Identifier = 12U, .Generation = 7U}, scratch) == FatStatus::StaleObject,
                   volume.Stat(FatObject{.Identifier = 13U, .Generation = 7U}, scratch) == FatStatus::Done,
                   scratch.Kind == User::Filesystem::NodeKind::Directory,
                   volume.Stat(FatObject{.Identifier = 4U, .Generation = 8U}, scratch) == FatStatus::StaleObject,
                   volume.Stat(FatObject{.Identifier = 500U, .Generation = 7U}, scratch) == FatStatus::StaleObject,
                   volume.Stat(FatObject{.Identifier = 4U, .Generation = 7U}, scratch) == FatStatus::Done,
                   scratch.Bytes == 13U);

    UInt8 rendered[12U]{};
    static constexpr UInt8 ReadmeEntry[32U]{'R', 'E', 'A', 'D', 'M', 'E', ' ', ' ', 'T', 'X', 'T', 0x20U, 0x18U};
    static constexpr UInt8 BaseLower[32U]{'R', 'E', 'A', 'D', 'M', 'E', ' ', ' ', 'T', 'X', 'T', 0x20U, 0x08U};
    static constexpr UInt8 NoExtension[32U]{'H', 'E', 'L', 'L', 'O', ' ', ' ', ' ', ' ', ' ', ' ', 0x20U, 0x10U};
    static constexpr UInt8 Kanji[32U]{0x05U, 'B', ' ', 'C', ' ', ' ', ' ', ' ', 'D', ' ', ' ', 0x20U, 0U};
    static constexpr UInt8 MtoolsShort[11U]{'A', '-', 'L', 'O', 'N', 'G', '~', '1', 'T', 'X', 'T'};
    const Size readme = User::Filesystem::RenderFatShortName(ReadmeEntry, rendered);
    const bool lower = (readme == 10U) && (std::memcmp(rendered, "readme.txt", 10U) == 0);
    const Size base = User::Filesystem::RenderFatShortName(BaseLower, rendered);
    const bool baseOnly = (base == 10U) && (std::memcmp(rendered, "readme.TXT", 10U) == 0);
    const Size hello = User::Filesystem::RenderFatShortName(NoExtension, rendered);
    const bool bare = (hello == 5U) && (std::memcmp(rendered, "HELLO", 5U) == 0);
    const Size kanji = User::Filesystem::RenderFatShortName(Kanji, rendered);
    ASTERIA_EXPECT(context, "short names render with the NT case flags, without padding, 0x05 as 0xE5", lower, baseOnly,
                   bare, kanji == 6U, rendered[0U] == 0xE5U, rendered[2U] == ' ', rendered[4U] == '.',
                   User::Filesystem::ComputeFatNameChecksum(MtoolsShort) == 0x67U);

    // Creation on a fresh volume: 8.3 names as mtools writes them, long names
    // with ~N.
    FatObject created{};
    const bool fresh83 = Prepare(Fat16Options, layout) && (MountBase(volume) == FatStatus::Done);
    const bool short83 = fresh83 && (CreateName(volume, "hello", created) == FatStatus::Done) &&
                         (created.Identifier == 1U) && (volume.Commit(FatCrashPoint::None) == FatStatus::Done) &&
                         ShortNameIs(layout, 1U, "HELLO      ", 0x08U);
    const bool both = (CreateName(volume, "notes.txt", created) == FatStatus::Done) && (created.Identifier == 2U) &&
                      (volume.Commit(FatCrashPoint::None) == FatStatus::Done) &&
                      ShortNameIs(layout, 2U, "NOTES   TXT", 0x18U);
    const bool digits = (CreateName(volume, "1234-678.9_1", created) == FatStatus::Done) &&
                        (volume.Commit(FatCrashPoint::None) == FatStatus::Done) &&
                        ShortNameIs(layout, static_cast<UInt32>(created.Identifier), "1234-6789_1", 0U);
    ASTERIA_EXPECT(context, "a name that fits 8.3 is an uppercase short entry with the NT case flags", short83, both,
                   digits, OpenName(volume, "HELLO", object) == FatStatus::Done, object.Identifier == 1U);

    FatObject longFile{};
    const bool longCreated = (CreateName(volume, "a-much-longer-file-name.data", longFile) == FatStatus::Done) &&
                             (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    const UInt32 at = static_cast<UInt32>(longFile.Identifier);
    ASTERIA_EXPECT(context, "a long name gets three long-name entries and BASIS~1, in one sector", longCreated,
                   at == 7U, ShortNameIs(layout, at, "A-MUCH~1DAT", 0U), DirectoryByte(layout, at - 3U, 0U) == 0x43U,
                   DirectoryByte(layout, at - 1U, 0U) == 0x01U, DirectoryByte(layout, at - 1U, 11U) == 0x0FU,
                   DirectoryByte(layout, at - 1U, 1U) == 'a', DirectoryByte(layout, at - 3U, 1U) == 't',
                   DirectoryByte(layout, at - 3U, 3U) == 'a', DirectoryByte(layout, at - 3U, 5U) == 0U,
                   DirectoryByte(layout, at - 3U, 6U) == 0U, DirectoryByte(layout, at - 3U, 7U) == 0xFFU,
                   OpenName(volume, "A-Much-Longer-File-Name.Data", object) == FatStatus::Done, object.Identifier == at,
                   CheckVolume(g_device).IsClean());

    // Eight more names of the same basis take ~2 to ~9; a tenth is refused.
    static constexpr char Similar[9U][32U]{
        "a-much-longer-file-name.dat2", "a-much-longer-file-name.dat3", "a-much-longer-file-name.dat4",
        "a-much-longer-file-name.dat5", "a-much-longer-file-name.dat6", "a-much-longer-file-name.dat7",
        "a-much-longer-file-name.dat8", "a-much-longer-file-name.dat9", "a-much-longer-file-name.datz"};
    Size made{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        made += ((CreateName(volume, Similar[index], created) == FatStatus::Done) &&
                 (volume.Commit(FatCrashPoint::None) == FatStatus::Done) &&
                 (DirectoryByte(layout, static_cast<UInt32>(created.Identifier), 7U) == ('2' + index)))
                    ? 1U
                    : 0U;
    }
    ASTERIA_EXPECT(context, "~N runs from 1 to 9, then the name is refused", made == 8U,
                   CreateName(volume, Similar[8U], created) == FatStatus::Refused, CheckVolume(g_device).IsClean());

    static constexpr const char* Refused[]{
        "con",  "con.txt", "aux.c", "nul", "prn.x", "com1",      "lpt9.log",  "Hello",
        ".a",   "a.",      "a b",   "-x",  "x~1",   "notes.txt", "NOTES.TXT", "a-much-longer-file-name.data",
        "hello"};
    Size refused{0U};
    for (const char* const name : Refused)
    {
        refused += (CreateName(volume, name, created) == FatStatus::Refused) ? 1U : 0U;
    }
    UInt8 tooLong[65U]{};
    std::memset(tooLong, 'a', sizeof(tooLong));
    FatObject empty{};
    ASTERIA_EXPECT(
        context, "device, invalid and existing names are refused; com0 and console are names",
        refused == (sizeof(Refused) / sizeof(Refused[0U])), volume.Create(Bytes("x"), 0U, empty) == FatStatus::Refused,
        volume.Create(tooLong, sizeof(tooLong), empty) == FatStatus::Refused,
        volume.Create(tooLong, sizeof(tooLong) - 1U, empty) == FatStatus::Done,
        volume.Commit(FatCrashPoint::None) == FatStatus::Done, CreateName(volume, "com0", created) == FatStatus::Done,
        volume.Commit(FatCrashPoint::None) == FatStatus::Done,
        CreateName(volume, "console", created) == FatStatus::Done,
        volume.Commit(FatCrashPoint::None) == FatStatus::Done);

    // A short name that collides with another file's (whose long name differs).
    static constexpr UInt32 Five[]{104U};
    const bool other = Prepare(Fat16Options, layout) && Fat::PlaceFile(g_device, layout, 2U, "HELLO2     ", 0U,
                                                                       "zz-other-name", Five, 1U, 1U, 0xFFFFU, 5U);
    ASTERIA_EXPECT(context, "an 8.3 name whose short entry exists is refused", other,
                   MountBase(volume) == FatStatus::Done, CreateName(volume, "hello2", created) == FatStatus::Refused,
                   OpenName(volume, "zz-other-name", object) == FatStatus::Done);

    // The whole set within one sector: sector 0 keeps two free entries (14,
    // 15); a three-entry set goes to sector 1, a short name to entry 14.
    const bool fresh = Prepare(Fat16Options, layout);
    static constexpr char Fillers[13U][9U]{"f1", "f2", "f3",  "f4",  "f5",  "f6", "f7",
                                           "f8", "f9", "f10", "f11", "f12", "f13"};
    bool filled = fresh && (MountBase(volume) == FatStatus::Done);
    for (const char* const name : Fillers)
    {
        filled = filled && (CreateName(volume, name, created) == FatStatus::Done) &&
                 (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    }
    FatObject spanning{};
    FatObject gap{};
    ASTERIA_EXPECT(context, "a long-name set never spans two directory sectors; a later short name fills the gap",
                   filled, created.Identifier == 13U,
                   CreateName(volume, "twenty-six-characters-long", spanning) == FatStatus::Done,
                   spanning.Identifier == 18U, volume.Commit(FatCrashPoint::None) == FatStatus::Done,
                   CreateName(volume, "g", gap) == FatStatus::Done, gap.Identifier == 14U,
                   volume.Commit(FatCrashPoint::None) == FatStatus::Done, CheckVolume(g_device).IsClean(),
                   OpenName(volume, "twenty-six-characters-long", object) == FatStatus::Done, object.Identifier == 18U);

    // A full fixed root refuses a create.
    const bool small = Prepare(Fat16SmallOptions, layout) && (MountBase(volume) == FatStatus::Done);
    static constexpr char Many[15U][4U]{"m1", "m2",  "m3",  "m4",  "m5",  "m6",  "m7", "m8",
                                        "m9", "m10", "m11", "m12", "m13", "m14", "m15"};
    bool all{small};
    for (const char* const name : Many)
    {
        all = all && (CreateName(volume, name, created) == FatStatus::Done) &&
              (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    }
    ASTERIA_EXPECT(context, "a full root directory refuses a create; one FAT is written alone", all,
                   CreateName(volume, "m16", created) == FatStatus::Refused, CheckVolume(g_device).IsClean());
}

void TestAliasRefusalBeforeGrowth(TestContext& context) noexcept
{
    auto options = Fat32Options;
    options.Label = false;
    Layout layout{};
    bool made = Prepare(options, layout);
    for (UInt32 index{0U}; made && index < 32U; ++index)
    {
        char shortName[12U]{};
        if (index < 9U)
        {
            std::snprintf(shortName, sizeof(shortName), "A-LONG~%uTXT", index + 1U);
        }
        else
        {
            std::snprintf(shortName, sizeof(shortName), "F%07uTXT", index);
        }
        made = Fat::PlaceFile(g_device, layout, index, shortName, 0U, nullptr, nullptr, 0U, 0U, 0x0FFF'FFFFU, 0U);
    }
    FatObject object{};
    const bool mounted = MountBase(g_volume) == FatStatus::Done;
    const auto refused = CreateName(g_volume, "a-long-new-file.txt", object);
    const auto writes = g_device.GetWrites();
    const bool clean = CheckVolume(g_device).IsClean();
    ASTERIA_EXPECT(context, "AliasRefusalBeforeGrowth: alias exhaustion on a full FAT32 root has no disk effects", made,
                   mounted, refused == FatStatus::Refused, writes == 0U, clean,
                   MountBase(g_volume, 8U) == FatStatus::Done, !g_volume.IsReadOnly());
    g_device.Arm(0U);
    ASTERIA_EXPECT(context, "DirectoryGrowthIoFailure: a write failure is distinct from preflight Refused",
                   CreateName(g_volume, "new.txt", object) == FatStatus::Failed);
}

void TestDirectorySpaceRefusal(TestContext& context) noexcept
{
    Layout layout{};
    constexpr UInt32 chain[]{2U};
    bool made = Prepare(Fat12TinyOptions, layout) &&
                Fat::PlaceFile(g_device, layout, 0U, "PARENT     ", 0U, nullptr, chain, 1U, 512U, 0xFFFU, 0U);
    static UInt8 sector[512U]{};
    made = made && g_device.Read(layout.RootStart, sector);
    sector[11U] = 0x10U;
    Fat::Put32(sector + 28U, 0U);
    made = made && g_device.Write(layout.RootStart, sector);
    std::memset(sector, 0, sizeof(sector));
    for (Size at{0U}; at < 16U; ++at)
    {
        std::memcpy(sector + at * 32U, "F0000000TXT", 11U);
        sector[at * 32U + 7U] = static_cast<UInt8>('A' + at);
    }
    for (Size at{0U}; at < 2U; ++at)
    {
        auto* entry = sector + at * 32U;
        std::memset(entry, ' ', 11U);
        entry[0U] = '.';
        if (at != 0U)
        {
            entry[1U] = '.';
        }
        entry[11U] = 0x10U;
        Fat::Put16(entry + 26U, at == 0U ? 2U : 0U);
    }
    made = made && g_device.Write(layout.DataStart, sector);
    for (UInt32 copy{0U}; copy < layout.Fats; ++copy)
    {
        for (UInt32 cluster{4U}; cluster < layout.Clusters + 2U; ++cluster)
        {
            Fat::WriteEntry(g_device, layout, copy, cluster, 0xFF7U);
        }
    }
    FatObject parent{};
    FatObject child{};
    const bool opened =
        MountBase(g_volume) == FatStatus::Done && OpenName(g_volume, "parent", parent) == FatStatus::Done;
    const auto refused = g_volume.CreateIn(parent, Bytes("child"), 5U, child, true);
    const auto writes = g_device.GetWrites();
    const bool mounted = MountBase(g_volume, 8U) == FatStatus::Done;
    ASTERIA_EXPECT(context, "DirectorySpaceRefusal: parent growth and child both need a cluster before writes", made,
                   opened, refused == FatStatus::Refused, writes == 0U, mounted, !g_volume.IsReadOnly());
}

void TestDirectoryLimitRefusal(TestContext& context) noexcept
{
    auto options = Fat32Options;
    options.Label = false;
    options.RootClusters = 4096U;
    Layout layout{};
    bool made = Prepare(options, layout);
    static UInt8 sector[Fat::SectorBytes]{};
    for (UInt32 ordinal{0U}; made && ordinal < options.RootClusters; ++ordinal)
    {
        std::memset(sector, 0, sizeof(sector));
        for (UInt32 slot{0U}; slot < 16U; ++slot)
        {
            char name[12U]{};
            std::snprintf(name, sizeof(name), "F%07uTXT", ordinal * 16U + slot);
            std::memcpy(sector + slot * 32U, name, 11U);
            sector[slot * 32U + 11U] = 0x20U;
        }
        made = g_device.Write(layout.DataStart + ordinal, sector);
    }
    FatObject object{};
    const bool mounted = MountBase(g_volume) == FatStatus::Done;
    const auto refused = CreateName(g_volume, "new.txt", object);
    const auto writes = g_device.GetWrites();
    ASTERIA_EXPECT(context, "DirectoryLimitRefusal: a full directory at the scan limit cannot grow before refusal",
                   made, mounted, refused == FatStatus::Refused, writes == 0U,
                   MountBase(g_volume, 8U) == FatStatus::Done, !g_volume.IsReadOnly());
}

void TestPages(TestContext& context) noexcept
{
    Layout layout{};
    FatVolume& volume = g_volume;
    FatObject readme{};
    FatObject large{};
    FatObject object{};
    FatFileSize size{};
    static constexpr UInt32 Readme[]{10U};
    static constexpr UInt32 Small[]{300U};
    UInt32 seven[56U]{};
    for (UInt32 index{0U}; index < 56U; ++index)
    {
        seven[index] = 200U + index;
    }
    const bool formatted =
        Prepare(Fat12Options, layout) &&
        Fat::PlaceFile(g_device, layout, 1U, "README  TXT", 0U, nullptr, Readme, 1U, 13U, 0xFFFU, 50U) &&
        Fat::PlaceFile(g_device, layout, 2U, "SEVEN   BIN", 0U, nullptr, seven, 56U, 7U * 4096U, 0xFFFU, 60U) &&
        Fat::PlaceFile(g_device, layout, 3U, "SMALL   BIN", 0U, nullptr, Small, 1U, 512U, 0xFFFU, 70U);
    FatObject small{};
    Fat::FillPage(g_expected, 50U);
    std::memset(g_expected + 13U, 0, FatPageBytes - 13U);
    const bool opened = formatted && (MountBase(volume) == FatStatus::Done) &&
                        (OpenName(volume, "readme.txt", readme) == FatStatus::Done) &&
                        (OpenName(volume, "seven.bin", large) == FatStatus::Done) &&
                        (OpenName(volume, "small.bin", small) == FatStatus::Done);
    ASTERIA_EXPECT(
        context, "a 13-byte file is one page; reads zero past its size; pages from six on are refused", opened,
        volume.Stat(readme, size) == FatStatus::Done, size.Pages == 1U, size.Bytes == 13U,
        volume.ReadPage(readme, 0U, g_page) == FatStatus::Done, std::memcmp(g_page, g_expected, FatPageBytes) == 0,
        volume.ReadPage(readme, 6U, g_page) == FatStatus::Done, volume.ReadPage(readme, 1U, g_page) == FatStatus::Done,
        g_page[0U] == 0U, g_page[FatPageBytes - 1U] == 0U);
    ASTERIA_EXPECT(context, "a file of more than six pages reads but is not written",
                   volume.Stat(large, size) == FatStatus::Done, size.Pages == 7U,
                   volume.ReadPage(large, 5U, g_page) == FatStatus::Done,
                   Fat::WriteSeeded(volume, large, 0U, 1U) && volume.Abort() == FatStatus::Done,
                   volume.Stat(large, size) == FatStatus::Done);

    // Appending to the 13-byte file: page 2 is past its page count; page 1
    // brings page 0's tail inside the file, zeroed.
    const bool appended = !Fat::WriteSeeded(volume, readme, 2U, 1U) && Fat::WriteSeeded(volume, readme, 1U, 51U);
    const bool tailZero = (volume.ReadPage(readme, 0U, g_page) == FatStatus::Done) &&
                          (std::memcmp(g_page, g_expected, FatPageBytes) == 0);
    FatObject another{};
    const bool busy =
        (CreateName(volume, "other", another) == FatStatus::Refused) && !Fat::WriteSeeded(volume, small, 0U, 1U);
    const bool committed = volume.Commit(FatCrashPoint::None) == FatStatus::Done;
    ASTERIA_EXPECT(context, "an append zeroes the partial page's tail; one file has a session at a time", appended,
                   tailZero, busy, committed, volume.Stat(readme, size) == FatStatus::Done, size.Bytes == 8192U,
                   CheckVolume(g_device).IsClean(), MountBase(volume) == FatStatus::Done,
                   volume.ReadPage(readme, 0U, g_page) == FatStatus::Done,
                   std::memcmp(g_page, g_expected, FatPageBytes) == 0);

    // A volume of 124 clusters: two six-page files leave 28 clusters.
    const bool tiny = Prepare(Fat12TinyOptions, layout) && (MountBase(volume) == FatStatus::Done);
    FatObject first{};
    FatObject second{};
    const bool two = (CreateName(volume, "one", first) == FatStatus::Done) && WritePages(volume, first, 300U, 6U) &&
                     (volume.Commit(FatCrashPoint::None) == FatStatus::Done) &&
                     (CreateName(volume, "two", second) == FatStatus::Done) && WritePages(volume, second, 310U, 6U) &&
                     (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    const bool third = (CreateName(volume, "three", object) == FatStatus::Done) &&
                       WritePages(volume, object, 320U, 3U) && !Fat::WriteSeeded(volume, object, 3U, 323U) &&
                       (volume.Stat(object, size) == FatStatus::Done) && (size.Pages == 3U) &&
                       (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    const bool copyRefused = !Fat::WriteSeeded(volume, first, 0U, 330U) &&
                             (volume.Stat(first, size) == FatStatus::Done) && (size.Pages == 6U) &&
                             (CreateName(volume, "four", object) == FatStatus::Done) &&
                             (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    ASTERIA_EXPECT(context, "a full volume refuses a write and keeps the session as it was", tiny, two, third,
                   copyRefused, PagesRead(volume, first, 300U, 6U), CheckVolume(g_device).IsClean());
}

/// The log of sector writes classified: f/F a FAT sector in copy 0/1, i the
/// FSInfo sector, d the root directory, b the boot sector, D data.
void Classify(const Layout& layout, char* const text, const Size capacity) noexcept
{
    Size length{0U};
    for (Size index{0U}; (index < g_device.GetWrites()) && ((length + 1U) < capacity); ++index)
    {
        const UInt64 lba = g_device.LogAt(index);
        char kind{'D'};
        if (lba == 0U)
        {
            kind = 'b';
        }
        else if ((layout.Type == FatType::Fat32) && (lba == layout.FsInfo))
        {
            kind = 'i';
        }
        else if ((lba >= layout.FatStart) && (lba < (layout.FatStart + layout.FatSectors)))
        {
            kind = 'f';
        }
        else if ((lba >= (layout.FatStart + layout.FatSectors)) && (lba < (layout.FatStart + (2U * layout.FatSectors))))
        {
            kind = (g_device.LogAt(index - 1U) == (lba - layout.FatSectors)) ? 'F' : '?';
        }
        else if (Fat::IsDirectorySector(g_device, layout, lba))
        {
            kind = 'd';
        }
        text[length++] = kind;
    }
    text[length] = '\0';
}

void TestCommitOrder(TestContext& context) noexcept
{
    Layout layout{};
    FatVolume& volume = g_volume;
    FatObject object{};
    char text[128U]{};
    // FAT16: dirty in FAT[1] of both FATs, the data, the chain, the entry,
    // clean again.
    const bool fat16 = Prepare(Fat16Options, layout) && (MountBase(volume) == FatStatus::Done) &&
                       (CreateName(volume, "order", object) == FatStatus::Done) && WritePages(volume, object, 1U, 1U);
    Classify(layout, text, sizeof(text));
    const bool session = (std::strcmp(text, "fFDDDDDDDD") == 0) && (g_device.GetLargestCall() == 8U);
    g_device.Arm(NoBudget);
    const bool committed = volume.Commit(FatCrashPoint::None) == FatStatus::Done;
    Classify(layout, text, sizeof(text));
    ASTERIA_EXPECT(context, "FAT16: copy 0 then copy 1 per FAT sector, the directory entry, then the clean bit", fat16,
                   session, committed, std::strcmp(text, "fFdfF") == 0, g_device.GetLargestCall() == 1U);

    // FAT12: the dirty mark is the boot sector's.
    const bool fat12 = Prepare(Fat12Options, layout) && (MountBase(volume) == FatStatus::Done) &&
                       (CreateName(volume, "order", object) == FatStatus::Done) && WritePages(volume, object, 1U, 1U) &&
                       (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    Classify(layout, text, sizeof(text));
    ASTERIA_EXPECT(context, "FAT12: the boot sector's flags byte marks the session", fat12,
                   std::strcmp(text, "bDDDDDDDDfFdb") == 0);

    // FAT32: replacing a committed file writes FSInfo before the entry and
    // again after the old chain is freed.
    const bool fat32 = Prepare(Fat32Options, layout) && (MountBase(volume) == FatStatus::Done) &&
                       (CreateName(volume, "order", object) == FatStatus::Done) && WritePages(volume, object, 1U, 2U) &&
                       (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    Classify(layout, text, sizeof(text));
    const bool created = std::strcmp(text, "fFDDDDDDDDDDDDDDDDfFidfF") == 0;
    g_device.Arm(NoBudget);
    const bool replaced =
        Fat::WriteSeeded(volume, object, 1U, 9U) && (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    Classify(layout, text, sizeof(text));
    UInt8 info[Fat::SectorBytes]{};
    static_cast<void>(g_device.Read(1U, info));
    const CheckReport report = CheckVolume(g_device);
    ASTERIA_EXPECT(context, "FAT32: FSInfo before the entry, the old chain freed and FSInfo exact after it", fat32,
                   created, replaced, std::strcmp(text, "fFDDDDDDDDDDDDDDDDfFidfFifF") == 0, report.IsClean(),
                   Fat::Get32(info + 488U) == report.FreeClusters, Fat::Get32(info + 492U) == 35U,
                   FatVolume::QueryCommit() == FatStatus::Unsupported);
}

UInt32 g_synchronizations{0U};
UInt32 g_failSynchronization{0U};
bool g_probeSynchronization{false};
bool g_syncDirty[2U]{};
[[nodiscard]] bool SynchronizePower(void*) noexcept
{
    if (!g_probeSynchronization)
    {
        return true;
    }
    ++g_synchronizations;
    if (g_synchronizations <= 2U)
    {
        g_syncDirty[g_synchronizations - 1U] = CheckVolume(g_device).Dirty;
    }
    return g_synchronizations != g_failSynchronization;
}
void TestPowerDurability(TestContext& context) noexcept
{
    for (UInt32 fail{0U}; fail < 3U; ++fail)
    {
        Layout layout{};
        FatObject object{};
        g_probeSynchronization = false;
        const bool formatted = Prepare(Fat16Options, layout);
        auto device = g_device.Get();
        device.Synchronize = &SynchronizePower;
        const bool staged = formatted && g_volume.Mount(device, 7U) == FatStatus::Done &&
                            CreateName(g_volume, "power", object) == FatStatus::Done &&
                            WritePages(g_volume, object, 5U, 1U);
        g_synchronizations = 0U;
        g_failSynchronization = fail;
        g_probeSynchronization = true;
        const auto result = g_volume.Commit(FatCrashPoint::None);
        g_probeSynchronization = false;
        ASTERIA_EXPECT(context, "power flush brackets clean marker and propagates device failures", staged,
                       result == (fail == 0U ? FatStatus::Done : FatStatus::Failed),
                       g_synchronizations == (fail == 1U ? 1U : 2U), g_syncDirty[0U],
                       fail == 1U ? CheckVolume(g_device).Dirty : !g_syncDirty[1U]);
    }
}

void TestCrashPoints(TestContext& context) noexcept
{
    static constexpr const FormatOptions* Volumes[]{&Fat12Options, &Fat16Options, &Fat32Options};
    FatVolume& volume = g_volume;
    for (const FormatOptions* const options : Volumes)
    {
        Layout layout{};
        FatObject object{};
        FatObject reopened{};
        FatFileSize size{};
        const bool before = Prepare(*options, layout) && (MountBase(volume) == FatStatus::Done) &&
                            (CreateName(volume, "crash", object) == FatStatus::Done) &&
                            WritePages(volume, object, 10U, 1U) &&
                            (volume.Commit(FatCrashPoint::BeforeDirectoryEntry) == FatStatus::Crashed) &&
                            (volume.Stat(object, size) == FatStatus::Failed) &&
                            (volume.Commit(FatCrashPoint::None) == FatStatus::Failed);
        const CheckReport lost = CheckVolume(g_device);
        ASTERIA_EXPECT(context, "a crash before the directory entry: the file is absent, its clusters lost", before,
                       MountBase(volume) == FatStatus::Done, volume.IsReadOnly(),
                       OpenName(volume, "crash", reopened) == FatStatus::NotFound, lost.Errors == 0U,
                       lost.LostClusters != 0U, lost.Dirty, lost.DifferingSectors == 0U, !lost.FsInfoWrong);

        const bool after =
            Prepare(*options, layout) && (MountBase(volume) == FatStatus::Done) &&
            (CreateName(volume, "crash", object) == FatStatus::Done) && WritePages(volume, object, 20U, 1U) &&
            (volume.Commit(FatCrashPoint::None) == FatStatus::Done) && Fat::WriteSeeded(volume, object, 1U, 21U) &&
            (volume.Commit(FatCrashPoint::AfterDirectoryEntry) == FatStatus::Crashed);
        const CheckReport present = CheckVolume(g_device);
        ASTERIA_EXPECT(context, "a crash after the directory entry: the file has its new size, the old chain is lost",
                       after, MountBase(volume) == FatStatus::Done, volume.IsReadOnly(),
                       OpenName(volume, "crash", reopened) == FatStatus::Done,
                       volume.Stat(reopened, size) == FatStatus::Done, size.Bytes == 8192U,
                       PagesRead(volume, reopened, 20U, 2U), present.Errors == 0U, present.LostClusters != 0U,
                       present.Dirty, !present.FsInfoWrong);
    }
}

void TestCrashSweep(TestContext& context) noexcept
{
    struct SweepVolume
    {
        const char* Label;
        const FormatOptions* Options;
        UInt32 Fill;
    };
    // The first FAT12 sweep allocates through entry 341, which straddles two
    // FAT sectors: hello's first chain ends on it.
    static constexpr SweepVolume Volumes[]{
        {"FAT12 sweep, entries straddling sectors", &Fat12Options, 323U},
        {"FAT12 sweep, 8 KiB clusters", &Fat12LargeOptions, 0U},
        {"FAT16 sweep, 8 KiB clusters", &Fat16Options, 0U},
        {"FAT32 sweep, 512-byte clusters, reserved nibble", &Fat32Options, 0U},
        {"FAT32 sweep, 4 KiB clusters", &Fat32LargeOptions, 0U},
    };
    static UInt32 fill[400U];
    for (const SweepVolume& entry : Volumes)
    {
        Layout layout{};
        const bool formatted = Prepare(*entry.Options, layout);
        const UInt32 first = (layout.Type == FatType::Fat32) ? (2U + entry.Options->RootClusters) : 2U;
        for (UInt32 index{0U}; index < entry.Fill; ++index)
        {
            fill[index] = first + index;
        }
        const UInt32 readme = first + entry.Fill;
        const UInt32 index = entry.Options->Label ? 1U : 0U;
        const bool placed = (entry.Fill == 0U) ||
                            Fat::PlaceFile(g_device, layout, index + 1U, "FILL    BIN", 0U, nullptr, fill, entry.Fill,
                                           entry.Fill << (9U + layout.ClusterShift), Fat::EndMarkOf(layout.Type), 0U);
        const bool withReadme = Fat::PlaceFile(g_device, layout, index, "README  TXT", 0U, nullptr, &readme, 1U, 13U,
                                               Fat::EndMarkOf(layout.Type), 70U);
        Fat::FillPage(g_expected, 70U);
        ASTERIA_EXPECT(context, entry.Label, formatted, placed, withReadme);
        Fat::RunCrashSweep(context, g_crashDevice, g_overlay, entry.Label, g_expected, 13U,
                           [](const Fat::CrashPoint&) noexcept
                           {
                           });
    }
}

void TestSessionRules(TestContext& context) noexcept
{
    Layout layout{};
    FatVolume& volume = g_volume;
    FatObject object{};
    FatObject other{};
    FatFileSize size{};
    const bool formatted = Prepare(Fat16Options, layout) && (MountBase(volume, 9U) == FatStatus::Done);
    const bool created = CreateName(volume, "pending", object) == FatStatus::Done;
    ASTERIA_EXPECT(
        context, "a created file is the session's: Stat sees it, Open does not, until the commit", formatted, created,
        object.Generation == 9U, volume.Stat(object, size) == FatStatus::Done, size.Bytes == 0U,
        volume.ReadPage(object, 0U, g_page) == FatStatus::Done,
        OpenName(volume, "pending", other) == FatStatus::NotFound,
        volume.Stat(FatObject{.Identifier = object.Identifier, .Generation = 8U}, size) == FatStatus::StaleObject,
        !Fat::WriteSeeded(volume, object, 1U, 1U), Fat::WriteSeeded(volume, object, 0U, 1U),
        volume.Commit(FatCrashPoint::None) == FatStatus::Done, OpenName(volume, "pending", other) == FatStatus::Done,
        other.Identifier == object.Identifier, PagesRead(volume, other, 1U, 1U));

    // A remount drops the session: the uncommitted file never existed, and
    // the dirty mark the session set keeps the volume read-only.
    const bool written =
        (CreateName(volume, "dropped", object) == FatStatus::Done) && Fat::WriteSeeded(volume, object, 0U, 2U);
    const CheckReport dirty = CheckVolume(g_device);
    ASTERIA_EXPECT(context, "an uncommitted session leaves only the dirty mark", written, dirty.Errors == 0U,
                   dirty.LostClusters == 0U, dirty.Dirty, MountBase(volume, 10U) == FatStatus::Done,
                   volume.IsReadOnly(), OpenName(volume, "dropped", other) == FatStatus::NotFound);

    // A commit with no session clears only a mark this mount set.
    Fat::ClearDirty(g_device, layout);
    FatObject twin{};
    const bool cleared = (MountBase(volume, 11U) == FatStatus::Done) && !volume.IsReadOnly() &&
                         (OpenName(volume, "pending", object) == FatStatus::Done) &&
                         (OpenName(volume, "pending", twin) == FatStatus::Done) &&
                         (CreateName(volume, "after", other) == FatStatus::Done);
    g_device.Arm(0U);
    const bool failed = !Fat::WriteSeeded(volume, other, 0U, 3U) && (volume.Stat(object, size) == FatStatus::Failed) &&
                        (OpenName(volume, "pending", twin) == FatStatus::Failed);
    ASTERIA_EXPECT(context, "a failed write breaks the volume until the next mount", cleared, failed,
                   MountBase(volume, 12U) == FatStatus::Done, !volume.IsReadOnly(),
                   OpenName(volume, "after", twin) == FatStatus::NotFound,
                   volume.Commit(FatCrashPoint::None) == FatStatus::Done, g_device.GetWrites() == 0U,
                   CheckVolume(g_device).IsClean());

    // A refused first write of a committed file starts no session and does
    // not dirty the volume; another create can still commit normally.
    const bool tiny = Prepare(Fat12TinyOptions, layout) && (MountBase(volume) == FatStatus::Done) &&
                      (CreateName(volume, "big", object) == FatStatus::Done) && WritePages(volume, object, 1U, 6U) &&
                      (volume.Commit(FatCrashPoint::None) == FatStatus::Done) &&
                      (CreateName(volume, "big2", other) == FatStatus::Done) && WritePages(volume, other, 1U, 6U) &&
                      (volume.Commit(FatCrashPoint::None) == FatStatus::Done);
    g_device.Arm(NoBudget);
    const bool refused = !Fat::WriteSeeded(volume, object, 0U, 5U);
    const bool marked = CheckVolume(g_device).Dirty;
    ASTERIA_EXPECT(context, "FullWriteRefusal: a refused copy starts no session and makes no disk writes", tiny,
                   refused, !marked, g_device.GetWrites() == 0U, CreateName(volume, "free", twin) == FatStatus::Done,
                   volume.Commit(FatCrashPoint::None) == FatStatus::Done, CheckVolume(g_device).IsClean());
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext context{"FatVolumeTests"};
    Asteria::Test::TestGeometry(context);
    Asteria::Test::TestMount(context);
    Asteria::Test::TestFatEntries(context);
    Asteria::Test::TestNames(context);
    Asteria::Test::TestAliasRefusalBeforeGrowth(context);
    Asteria::Test::TestDirectorySpaceRefusal(context);
    Asteria::Test::TestDirectoryLimitRefusal(context);
    Asteria::Test::TestPages(context);
    Asteria::Test::TestCommitOrder(context);
    Asteria::Test::TestPowerDurability(context);
    Asteria::Test::TestCrashPoints(context);
    Asteria::Test::TestCrashSweep(context);
    Asteria::Test::TestSessionRules(context);
    return context.Finish();
}
