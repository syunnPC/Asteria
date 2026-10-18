// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the FAT volume library's decoding (FatVolume.hpp): the
// boot sector's parameter block, FAT entries and chains, and the root
// directory's short and long-name entries, on arbitrary sectors.
//
// Input layout: up to eight 512-byte blocks. The device has 2^17 sectors and
// sector n reads block n mod blocks (a short last block zero-padded), so
// every sector the library reads - boot sector, FSInfo, FAT, directory,
// data - is input. Writes go to a small overlay that later reads see; a write
// past its capacity fails, as a device would.
//
// Properties checked for every input:
// - Decoding never reads or writes outside its buffers and has no undefined
//   behaviour (sanitizers report violations); every loop is bounded.
// - ReadFatGeometry leaves the geometry unchanged unless it answers Done; a
//   decoded geometry is self-consistent (type by cluster count, regions
//   inside the volume, a FAT that holds every cluster's entry).
// - Open answers Done only with an identifier that Stat accepts; ReadPage of
//   a file never answers Done with nonzero bytes past the file's size.
// - Once an operation answers Failed, every later one does until Mount.
// - On a writable FAT12 or FAT16 volume, a created file written and
//   committed reads back by its identifier. (A FAT32 volume needs 65525
//   clusters, too many to search for free ones per input, so FAT32 is only
//   read.)

#include "User/Filesystem/Fat/FatVolume.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
namespace Fs = Asteria::User::Filesystem;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;

[[noreturn]] void Violation(const char* const property)
{
    static_cast<void>(std::fprintf(stderr, "FUZZ PROPERTY VIOLATED: %s\n", property));
    std::abort();
}

void Require(const bool condition, const char* const property)
{
    if (!condition)
    {
        Violation(property);
    }
}

constexpr Size Block{512U};
constexpr Size MaximumBlocks{8U};
constexpr UInt64 DeviceSectors{UInt64{1U} << 17U};
constexpr Size OverlayCapacity{96U};

UInt8 g_blocks[MaximumBlocks][Block];
Size g_blockCount{1U};
UInt64 g_overlayLba[OverlayCapacity];
UInt8 g_overlay[OverlayCapacity][Block];
Size g_overlayUsed{0U};
UInt8 g_page[Fs::FatPageBytes];
UInt8 g_written[Fs::FatPageBytes];
Fs::FatVolume g_volume;

[[nodiscard]] const UInt8* Sector(const UInt64 lba) noexcept
{
    for (Size slot{0U}; slot < g_overlayUsed; ++slot)
    {
        if (g_overlayLba[slot] == lba)
        {
            return g_overlay[slot];
        }
    }
    return g_blocks[lba % g_blockCount];
}

bool ReadSectors(void*, const UInt64 lba, const UInt32 count, UInt8* const buffer) noexcept
{
    Require((count >= 1U) && (count <= 8U) && ((lba + count) <= DeviceSectors), "a read stays inside the device");
    for (UInt32 index{0U}; index < count; ++index)
    {
        std::memcpy(buffer + (index * Block), Sector(lba + index), Block);
    }
    return true;
}

bool WriteSectors(void*, const UInt64 lba, const UInt32 count, const UInt8* const buffer) noexcept
{
    Require((count >= 1U) && (count <= 8U) && ((lba + count) <= DeviceSectors), "a write stays inside the device");
    for (UInt32 index{0U}; index < count; ++index)
    {
        Size slot{0U};
        while ((slot < g_overlayUsed) && (g_overlayLba[slot] != (lba + index)))
        {
            ++slot;
        }
        if (slot == OverlayCapacity)
        {
            return false;
        }
        g_overlayUsed += (slot == g_overlayUsed) ? 1U : 0U;
        g_overlayLba[slot] = lba + index;
        std::memcpy(g_overlay[slot], buffer + (index * Block), Block);
    }
    return true;
}

void CheckGeometry(const Fs::FatGeometry& geometry) noexcept
{
    const UInt64 clusters{geometry.Clusters};
    const bool typed = (geometry.Type == Fs::FatType::Fat12)
                           ? (clusters < 4085U)
                           : ((geometry.Type == Fs::FatType::Fat16) ? ((clusters >= 4085U) && (clusters < 65525U))
                                                                    : (clusters >= 65525U));
    Require(typed, "a decoded volume's type follows its cluster count");
    const UInt64 entries = clusters + 2U;
    const UInt64 bytes = (geometry.Type == Fs::FatType::Fat12)
                             ? (entries + ((entries + 1U) / 2U))
                             : (entries * ((geometry.Type == Fs::FatType::Fat16) ? 2U : 4U));
    Require((geometry.FatSectors * Block) >= bytes, "a decoded FAT holds every cluster's entry");
    Require((geometry.FatStart >= 1U) && (geometry.FatCount >= 1U) && (geometry.FatCount <= 2U) &&
                (geometry.ClusterShift <= 7U) && (geometry.DataStart < DeviceSectors) &&
                ((geometry.DataStart + (clusters << geometry.ClusterShift)) <= DeviceSectors),
            "a decoded volume lies inside the device");
    Require(
        (geometry.Type == Fs::FatType::Fat32)
            ? ((geometry.RootStart == 0U) && (geometry.RootCluster >= 2U) && (geometry.RootCluster <= (clusters + 1U)))
            : ((geometry.RootEntries != 0U) && (geometry.RootStart > geometry.FatStart) &&
               (geometry.DataStart >= (geometry.RootStart + ((geometry.RootEntries + 15U) / 16U)))),
        "a decoded root directory lies where its type puts it");
}

/// Stat and ReadPage of object; false once the volume answers Failed.
[[nodiscard]] bool ReadFile(const Fs::FatObject& object) noexcept
{
    Fs::FatFileSize size{};
    const Fs::FatStatus stat = g_volume.Stat(object, size);
    if (stat != Fs::FatStatus::Done)
    {
        Require((stat == Fs::FatStatus::StaleObject) || (stat == Fs::FatStatus::Failed) ||
                    (stat == Fs::FatStatus::Corrupt),
                "Stat answers Done, StaleObject, Corrupt or Failed");
        return stat != Fs::FatStatus::Failed;
    }
    if (size.Kind == Fs::NodeKind::Directory)
    {
        Require(g_volume.ReadPage(object, 0U, g_page) == Fs::FatStatus::Refused, "directories refuse file reads");
        return true;
    }
    Require(size.Pages == ((size.Bytes + 4095U) / 4096U), "Stat's pages are its bytes in pages");
    for (UInt64 page{0U}; page < 8U; ++page)
    {
        const Fs::FatStatus read = g_volume.ReadPage(object, page, g_page);
        if (read == Fs::FatStatus::Failed)
        {
            return false;
        }
        Require((read == Fs::FatStatus::Done) || (read == Fs::FatStatus::Corrupt), "ReadPage answers Done or Corrupt");
        const UInt64 start = page * Fs::FatPageBytes;
        for (Size index{0U}; (read == Fs::FatStatus::Done) && (index < Fs::FatPageBytes); ++index)
        {
            Require(((start + index) < size.Bytes) || (g_page[index] == 0U), "ReadPage zeroes past the size");
        }
    }
    return true;
}

void RequireFailed(const Fs::FatObject& object) noexcept
{
    Fs::FatFileSize size{};
    Fs::FatObject found{};
    const UInt8 name[1U]{'a'};
    Require((g_volume.Stat(object, size) == Fs::FatStatus::Failed) &&
                (g_volume.Open(name, 1U, found) == Fs::FatStatus::Failed) &&
                (g_volume.Commit(Fs::FatCrashPoint::None) == Fs::FatStatus::Failed),
            "a failed volume answers Failed until it is mounted again");
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size)
{
    if ((size == 0U) || (size > (MaximumBlocks * Block)))
    {
        return 0;
    }
    std::memset(g_blocks, 0, sizeof(g_blocks));
    std::memcpy(g_blocks, data, size);
    g_blockCount = (size + Block - 1U) / Block;
    g_overlayUsed = 0U;

    Fs::FatGeometry geometry{};
    geometry.Clusters = 0xC0FFEEU;
    const Fs::FatStatus decoded = Fs::ReadFatGeometry(g_blocks[0U], DeviceSectors, geometry);
    if (decoded != Fs::FatStatus::Done)
    {
        Require(geometry.Clusters == 0xC0FFEEU, "a refused boot sector leaves the geometry unchanged");
        Require((decoded == Fs::FatStatus::NotFat) || (decoded == Fs::FatStatus::Corrupt),
                "a boot sector is a FAT volume, not one, or corrupt");
    }
    else
    {
        CheckGeometry(geometry);
    }
    const Fs::FatBlockDevice device{
        .Context = nullptr, .ReadSectors = &ReadSectors, .WriteSectors = &WriteSectors, .Sectors = DeviceSectors};
    const Fs::FatStatus mounted = g_volume.Mount(device, 77U);
    Require((mounted == Fs::FatStatus::Done) == g_volume.IsMounted(), "Mount's answer and the volume agree");
    if (mounted != Fs::FatStatus::Done)
    {
        Require((mounted == Fs::FatStatus::NotFat) || (mounted == Fs::FatStatus::Corrupt),
                "Mount fails only to decode");
        return 0;
    }
    Require(decoded == Fs::FatStatus::Done, "a mounted volume's boot sector decodes");

    // Names: two fixed ones and the input's last bytes.
    static constexpr const char* Names[]{"readme.txt", "a-long-lower-name.txt"};
    for (const char* const name : Names)
    {
        Fs::FatObject object{};
        const Fs::FatStatus opened = g_volume.Open(reinterpret_cast<const UInt8*>(name), std::strlen(name), object);
        Require((opened == Fs::FatStatus::Done) || (opened == Fs::FatStatus::NotFound) ||
                    (opened == Fs::FatStatus::Corrupt),
                "Open answers Done, NotFound or Corrupt");
        if (opened == Fs::FatStatus::Done)
        {
            Fs::FatFileSize sizes{};
            Require((object.Generation == 77U) && (g_volume.Stat(object, sizes) == Fs::FatStatus::Done),
                    "an opened file's name is valid");
            static_cast<void>(ReadFile(object));
        }
    }
    const Size tail = (size < 24U) ? size : 24U;
    Fs::FatObject any{};
    static_cast<void>(g_volume.Open(data + size - tail, tail, any));
    for (UInt64 identifier{0U}; identifier < 40U; ++identifier)
    {
        static_cast<void>(ReadFile(Fs::FatObject{.Identifier = identifier, .Generation = 77U}));
    }

    // The write path, on a writable FAT12 or FAT16 volume.
    if (g_volume.IsReadOnly() || (g_volume.GetGeometry().Type == Fs::FatType::Fat32))
    {
        return 0;
    }
    static constexpr UInt8 Created[]{'f', 'u', 'z', 'z', '-', 'c', 'r', 'e', 'a', 't', 'e', 'd', '.', 'b', 'i', 'n'};
    Fs::FatObject object{};
    const Fs::FatStatus created = g_volume.Create(Created, sizeof(Created), object);
    if (created != Fs::FatStatus::Done)
    {
        Require((created == Fs::FatStatus::Refused) || (created == Fs::FatStatus::Corrupt) ||
                    (created == Fs::FatStatus::Failed),
                "Create answers Done, Refused, Corrupt or Failed");
        return 0;
    }
    for (Size index{0U}; index < sizeof(g_written); ++index)
    {
        g_written[index] = data[index % size];
    }
    const Fs::FatStatus written = g_volume.WritePage(object, 0U, g_written);
    const Fs::FatStatus committed =
        (written == Fs::FatStatus::Done) ? g_volume.Commit(Fs::FatCrashPoint::None) : written;
    if ((written == Fs::FatStatus::Failed) || (committed == Fs::FatStatus::Failed))
    {
        RequireFailed(object);
        return 0;
    }
    if (committed == Fs::FatStatus::Done)
    {
        Fs::FatFileSize sizes{};
        Require((g_volume.Stat(object, sizes) == Fs::FatStatus::Done) && (sizes.Bytes == Fs::FatPageBytes) &&
                    (g_volume.ReadPage(object, 0U, g_page) == Fs::FatStatus::Done) &&
                    (std::memcmp(g_page, g_written, sizeof(g_page)) == 0),
                "a committed page reads back");
    }
    return 0;
}

namespace
{
UInt8 g_seed[MaximumBlocks * Block];

void Put16(UInt8* const bytes, const UInt32 value) noexcept
{
    bytes[0U] = static_cast<UInt8>(value);
    bytes[1U] = static_cast<UInt8>(value >> 8U);
}

void Put32(UInt8* const bytes, const UInt32 value) noexcept
{
    Put16(bytes, value);
    Put16(bytes + 2U, value >> 16U);
}

/// A boot sector; the layout keeps the boot sector, FAT, root directory and
/// first data clusters in distinct blocks of the input.
void Boot(const UInt32 perCluster, const UInt32 reserved, const UInt32 fats, const UInt32 rootEntries,
          const UInt32 sectors, const UInt32 fatSectors, const bool fat32) noexcept
{
    std::memset(g_seed, 0, sizeof(g_seed));
    UInt8* const boot = g_seed;
    boot[0U] = 0xEBU;
    boot[1U] = 0x3CU;
    boot[2U] = 0x90U;
    Put16(boot + 11U, 512U);
    boot[13U] = static_cast<UInt8>(perCluster);
    Put16(boot + 14U, reserved);
    boot[16U] = static_cast<UInt8>(fats);
    Put16(boot + 17U, rootEntries);
    Put16(boot + 19U, (sectors < 65536U) ? sectors : 0U);
    boot[21U] = 0xF8U;
    Put16(boot + 22U, fat32 ? 0U : fatSectors);
    Put32(boot + 32U, (sectors < 65536U) ? 0U : sectors);
    if (fat32)
    {
        Put32(boot + 36U, fatSectors);
        Put32(boot + 44U, 2U);
        Put16(boot + 48U, 1U);
    }
    boot[510U] = 0x55U;
    boot[511U] = 0xAAU;
}

/// A directory with a label, README.TXT (13 bytes, cluster 2), and a
/// long-named file (cluster 3) whose long-name entries precede it.
void Directory(UInt8* const root, const UInt32 highWord) noexcept
{
    std::memcpy(root, "ASTERIA    ", 11U);
    root[11U] = 0x08U;
    std::memcpy(root + 32U, "README  TXT", 11U);
    root[32U + 11U] = 0x20U;
    Put16(root + 32U + 26U, 2U);
    Put32(root + 32U + 28U, 13U);
    static constexpr char Long[]{"a-long-lower-name.txt"};
    static constexpr UInt8 Offsets[13U]{1U, 3U, 5U, 7U, 9U, 14U, 16U, 18U, 20U, 22U, 24U, 28U, 30U};
    static constexpr UInt8 Short[11U]{'A', '-', 'L', 'O', 'N', 'G', '~', '1', 'T', 'X', 'T'};
    const UInt8 checksum = Fs::ComputeFatNameChecksum(Short);
    for (UInt32 entry{0U}; entry < 2U; ++entry)
    {
        UInt8* const record = root + 64U + (entry * 32U);
        const UInt32 order = 2U - entry;
        record[0U] = static_cast<UInt8>(order | ((entry == 0U) ? 0x40U : 0U));
        for (Size character{0U}; character < 13U; ++character)
        {
            const Size position = ((order - 1U) * 13U) + character;
            Put16(record + Offsets[character],
                  (position < 21U) ? static_cast<UInt8>(Long[position]) : ((position == 21U) ? 0U : 0xFFFFU));
        }
        record[11U] = 0x0FU;
        record[13U] = checksum;
    }
    UInt8* const record = root + 128U;
    std::memcpy(record, Short, 11U);
    record[11U] = 0x20U;
    Put16(record + 20U, highWord);
    Put16(record + 26U, 3U);
    Put32(record + 28U, 300U);
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    // FAT12: 300 sectors; FAT 1 and 2, root 3, clusters 2.. from block 4.
    Boot(1U, 1U, 2U, 16U, 300U, 1U, false);
    for (UInt32 copy{1U}; copy <= 2U; ++copy)
    {
        UInt8* const fat = g_seed + (copy * Block);
        fat[0U] = 0xF8U;
        fat[1U] = 0xFFU;
        fat[2U] = 0xFFU;
        fat[3U] = 0xFFU;
        fat[4U] = 0xFFU;
        fat[5U] = 0xFFU;
    }
    Directory(g_seed + (3U * Block), 0U);
    std::memcpy(g_seed + (4U * Block), "hello readme\n", 13U);
    emit(g_seed, sizeof(g_seed));
    emit(g_seed, 4U * Block);

    // FAT16: 4400 sectors (4380 clusters); FAT at 1-18, root at 19 (block
    // 3), cluster 2 at 20 (block 4).
    Boot(1U, 1U, 1U, 16U, 4400U, 18U, false);
    UInt8* const fat16 = g_seed + Block;
    Put16(fat16, 0xFFF8U);
    Put16(fat16 + 2U, 0xFFFFU);
    Put16(fat16 + 4U, 0xFFFFU);
    Put16(fat16 + 6U, 0xFFFFU);
    Directory(g_seed + (3U * Block), 0U);
    std::memcpy(g_seed + (4U * Block), "hello readme\n", 13U);
    emit(g_seed, sizeof(g_seed));

    // FAT32: 66600 sectors (66081 clusters), FSInfo at 1, FAT at 2-518, the
    // root cluster 2 at 519 (block 7).
    Boot(1U, 2U, 1U, 0U, 66600U, 517U, true);
    UInt8* const info = g_seed + Block;
    Put32(info, 0x4161'5252U);
    Put32(info + 484U, 0x6141'7272U);
    Put32(info + 488U, 1000U);
    Put32(info + 492U, 2U);
    Put32(info + 508U, 0xAA55'0000U);
    UInt8* const fat32 = g_seed + (2U * Block);
    Put32(fat32, 0x0FFF'FFF8U);
    Put32(fat32 + 4U, 0x0FFF'FFFFU);
    Put32(fat32 + 8U, 0x0FFF'FFFFU);
    Put32(fat32 + 12U, 0x0FFF'FFFFU);
    Directory(g_seed + (7U * Block), 0U);
    emit(g_seed, sizeof(g_seed));

    // A sector that decodes to nothing, to start mutations at a refusal.
    std::memset(g_seed, 0, Block);
    emit(g_seed, Block);
}
