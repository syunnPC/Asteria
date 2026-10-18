// SPDX-License-Identifier: Apache-2.0
// Fuzz harness for the exFAT volume library's decoding (ExfatVolume.hpp): the
// boot sector recogniser, the boot checksum, FAT chains, the allocation
// bitmap and the root directory's entry sets, on arbitrary sectors.
//
// Input layout: up to eight 512-byte blocks. Block 0 is the boot sector;
// sectors 1 to 10 read as zeros and sector 11 as the boot checksum of
// sectors 0 to 10, computed when read (one more than it when bit 7 of the
// boot sector's PercentInUse byte is set), so mutations reach past the
// checksum; sector n >= 12 reads block 1 + (n - 12) mod (blocks - 1) (zeros
// with one block). Writes go to a small overlay that later reads see; a
// write past its capacity fails, as a device would.
//
// Properties checked for every input:
// - Decoding never reads or writes outside its buffers and has no undefined
//   behaviour (sanitizers report violations); every loop is bounded.
// - ReadExfatBootSector leaves the geometry unchanged unless it answers
//   Done; a decoded geometry is self-consistent (a FAT entry per cluster, a
//   heap after the FAT and within the volume and the device, a root cluster
//   in the heap).
// - Mount answers Done exactly when the volume is mounted, and only for a
//   boot sector the recogniser accepts.
// - Open answers Done only with an identifier that Stat accepts; ReadPage of
//   a file never answers Done with nonzero bytes past the file's size.
// - Once an operation answers Failed, every later one does until Mount.
// - On a writable volume, a created file written and committed reads back
//   by its identifier, and after a new mount by its name: the entry set the
//   write path encodes is one the read path decodes.

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

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
UInt8 g_zero[Block];
UInt8 g_checksum[Block];
UInt8 g_page[Fs::ExfatPageBytes];
UInt8 g_written[Fs::ExfatPageBytes];
Fs::ExfatVolume g_volume;

[[nodiscard]] const UInt8* Overlay(const UInt64 lba) noexcept
{
    for (Size slot{0U}; slot < g_overlayUsed; ++slot)
    {
        if (g_overlayLba[slot] == lba)
        {
            return g_overlay[slot];
        }
    }
    return nullptr;
}

[[nodiscard]] const UInt8* Sector(const UInt64 lba) noexcept
{
    const UInt8* const written = Overlay(lba);
    if (written != nullptr)
    {
        return written;
    }
    if (lba == 0U)
    {
        return g_blocks[0U];
    }
    if (lba < 11U)
    {
        return g_zero;
    }
    if (lba == 11U)
    {
        // The boot checksum of sectors 0 to 10 as they read now.
        UInt32 checksum{0U};
        for (UInt64 sector{0U}; sector < 11U; ++sector)
        {
            const UInt8* const bytes = Sector(sector);
            for (Size at{0U}; at < Block; ++at)
            {
                if ((sector != 0U) || ((at != 106U) && (at != 107U) && (at != 112U)))
                {
                    checksum = ((checksum >> 1U) | (checksum << 31U)) + bytes[at];
                }
            }
        }
        checksum += ((Sector(0U)[112U] & 0x80U) != 0U) ? 1U : 0U;
        for (Size at{0U}; at < Block; at += 4U)
        {
            g_checksum[at] = static_cast<UInt8>(checksum);
            g_checksum[at + 1U] = static_cast<UInt8>(checksum >> 8U);
            g_checksum[at + 2U] = static_cast<UInt8>(checksum >> 16U);
            g_checksum[at + 3U] = static_cast<UInt8>(checksum >> 24U);
        }
        return g_checksum;
    }
    return (g_blockCount > 1U) ? g_blocks[1U + ((lba - 12U) % (g_blockCount - 1U))] : g_zero;
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

void CheckGeometry(const Fs::ExfatGeometry& geometry) noexcept
{
    const UInt64 clusters{geometry.Clusters};
    Require((geometry.ClusterShift <= 16U) && (clusters >= 1U) && (clusters <= 0xFFFF'FFF5U) &&
                ((geometry.FatSectors * 128U) >= (clusters + 2U)) && (geometry.FatStart >= 24U) &&
                (geometry.HeapStart >= (geometry.FatStart + geometry.FatSectors)) &&
                ((geometry.HeapStart + (clusters << geometry.ClusterShift)) <= geometry.VolumeSectors) &&
                (geometry.VolumeSectors <= DeviceSectors) && (geometry.RootCluster >= 2U) &&
                (geometry.RootCluster <= (clusters + 1U)),
            "a decoded volume is self-consistent and inside the device");
}

/// Stat and ReadPage of object; false once the volume answers Failed.
[[nodiscard]] bool ReadFile(const Fs::ExfatObject& object) noexcept
{
    Fs::ExfatFileSize size{};
    const Fs::ExfatStatus stat = g_volume.Stat(object, size);
    if (stat != Fs::ExfatStatus::Done)
    {
        Require((stat == Fs::ExfatStatus::StaleObject) || (stat == Fs::ExfatStatus::Failed) ||
                    (stat == Fs::ExfatStatus::Corrupt),
                "Stat answers Done, StaleObject, Corrupt or Failed");
        return stat != Fs::ExfatStatus::Failed;
    }
    if (size.Kind == Fs::NodeKind::Directory)
    {
        Require(g_volume.ReadPage(object, 0U, g_page) == Fs::ExfatStatus::Refused, "directories refuse file reads");
        return true;
    }
    Require(size.Pages == ((size.Bytes >> 12U) + (((size.Bytes & 4095U) != 0U) ? 1U : 0U)),
            "Stat's pages are its bytes in pages");
    for (UInt64 page{0U}; page < 8U; ++page)
    {
        const Fs::ExfatStatus read = g_volume.ReadPage(object, page, g_page);
        if (read == Fs::ExfatStatus::Failed)
        {
            return false;
        }
        Require((read == Fs::ExfatStatus::Done) || (read == Fs::ExfatStatus::Corrupt),
                "ReadPage answers Done or Corrupt");
        const UInt64 start = page * Fs::ExfatPageBytes;
        for (Size index{0U}; (read == Fs::ExfatStatus::Done) && (index < Fs::ExfatPageBytes); ++index)
        {
            Require(((start + index) < size.Bytes) || (g_page[index] == 0U), "ReadPage zeroes past the size");
        }
    }
    return true;
}

void RequireFailed(const Fs::ExfatObject& object) noexcept
{
    Fs::ExfatFileSize size{};
    Fs::ExfatObject found{};
    const UInt8 name[1U]{'a'};
    Require((g_volume.Stat(object, size) == Fs::ExfatStatus::Failed) &&
                (g_volume.Open(name, 1U, found) == Fs::ExfatStatus::Failed) &&
                (g_volume.Commit(Fs::ExfatCrashPoint::None) == Fs::ExfatStatus::Failed),
            "a failed volume answers Failed until it is mounted again");
}

[[nodiscard]] bool ReadsBack(const Fs::ExfatObject& object) noexcept
{
    Fs::ExfatFileSize sizes{};
    return (g_volume.Stat(object, sizes) == Fs::ExfatStatus::Done) && (sizes.Bytes == Fs::ExfatPageBytes) &&
           (g_volume.ReadPage(object, 0U, g_page) == Fs::ExfatStatus::Done) &&
           (std::memcmp(g_page, g_written, sizeof(g_page)) == 0);
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

    Fs::ExfatGeometry geometry{};
    geometry.Clusters = 0xC0FFEEU;
    const Fs::ExfatStatus decoded = Fs::ReadExfatBootSector(g_blocks[0U], DeviceSectors, geometry);
    if (decoded != Fs::ExfatStatus::Done)
    {
        Require(geometry.Clusters == 0xC0FFEEU, "a refused boot sector leaves the geometry unchanged");
        Require((decoded == Fs::ExfatStatus::NotExfat) || (decoded == Fs::ExfatStatus::Corrupt),
                "a boot sector is an exFAT volume, not one, or corrupt");
    }
    else
    {
        CheckGeometry(geometry);
    }
    const Fs::ExfatBlockDevice device{
        .Context = nullptr, .ReadSectors = &ReadSectors, .WriteSectors = &WriteSectors, .Sectors = DeviceSectors};
    const Fs::ExfatStatus mounted = g_volume.Mount(device, 77U);
    Require((mounted == Fs::ExfatStatus::Done) == g_volume.IsMounted(), "Mount's answer and the volume agree");
    if (mounted != Fs::ExfatStatus::Done)
    {
        Require((mounted == Fs::ExfatStatus::NotExfat) || (mounted == Fs::ExfatStatus::Corrupt),
                "Mount fails only to decode");
        return 0;
    }
    Require(decoded == Fs::ExfatStatus::Done, "a mounted volume's boot sector decodes");

    // Names: two fixed ones and the input's last bytes.
    static constexpr const char* Names[]{"readme.txt", "a-long-lower-name.txt"};
    for (const char* const name : Names)
    {
        Fs::ExfatObject object{};
        const Fs::ExfatStatus opened = g_volume.Open(reinterpret_cast<const UInt8*>(name), std::strlen(name), object);
        Require((opened == Fs::ExfatStatus::Done) || (opened == Fs::ExfatStatus::NotFound) ||
                    (opened == Fs::ExfatStatus::Corrupt),
                "Open answers Done, NotFound or Corrupt");
        if (opened == Fs::ExfatStatus::Done)
        {
            Fs::ExfatFileSize sizes{};
            Require((object.Generation == 77U) && (g_volume.Stat(object, sizes) == Fs::ExfatStatus::Done),
                    "an opened file's name is valid");
            static_cast<void>(ReadFile(object));
        }
    }
    const Size tail = (size < 24U) ? size : 24U;
    Fs::ExfatObject any{};
    static_cast<void>(g_volume.Open(data + size - tail, tail, any));
    for (UInt64 identifier{0U}; identifier < 40U; ++identifier)
    {
        static_cast<void>(ReadFile(Fs::ExfatObject{.Identifier = identifier, .Generation = 77U}));
    }

    // The write path, on a writable volume: a created file and a replaced one.
    if (g_volume.IsReadOnly())
    {
        return 0;
    }
    static constexpr UInt8 Created[]{'f', 'u', 'z', 'z', '-', 'c', 'r', 'e', 'a', 't', 'e', 'd', '.', 'b', 'i', 'n'};
    Fs::ExfatObject object{};
    const Fs::ExfatStatus created = g_volume.Create(Created, sizeof(Created), object);
    if (created != Fs::ExfatStatus::Done)
    {
        Require((created == Fs::ExfatStatus::Refused) || (created == Fs::ExfatStatus::Corrupt) ||
                    (created == Fs::ExfatStatus::Failed),
                "Create answers Done, Refused, Corrupt or Failed");
        return 0;
    }
    for (Size index{0U}; index < sizeof(g_written); ++index)
    {
        g_written[index] = data[index % size];
    }
    const Fs::ExfatStatus written = g_volume.WritePage(object, 0U, g_written);
    const Fs::ExfatStatus committed =
        (written == Fs::ExfatStatus::Done) ? g_volume.Commit(Fs::ExfatCrashPoint::None) : written;
    if ((written == Fs::ExfatStatus::Failed) || (committed == Fs::ExfatStatus::Failed))
    {
        RequireFailed(object);
        return 0;
    }
    if (committed != Fs::ExfatStatus::Done)
    {
        return 0;
    }
    Require(ReadsBack(object), "a committed page reads back");
    Fs::ExfatObject found{};
    const Fs::ExfatStatus remounted = g_volume.Mount(device, 78U);
    Require((remounted == Fs::ExfatStatus::Done) && !g_volume.IsReadOnly() &&
                (g_volume.Open(Created, sizeof(Created), found) == Fs::ExfatStatus::Done) &&
                (found.Identifier == object.Identifier) && ReadsBack(found),
            "a committed file opens and reads back after a new mount");
    Fs::ExfatObject readme{};
    if (g_volume.Open(reinterpret_cast<const UInt8*>("readme.txt"), 10U, readme) == Fs::ExfatStatus::Done)
    {
        const Fs::ExfatStatus replaced = g_volume.WritePage(readme, 0U, g_written);
        const Fs::ExfatStatus done =
            (replaced == Fs::ExfatStatus::Done) ? g_volume.Commit(Fs::ExfatCrashPoint::None) : replaced;
        if (done == Fs::ExfatStatus::Failed)
        {
            RequireFailed(readme);
            return 0;
        }
        Require((done != Fs::ExfatStatus::Done) || ((g_volume.Mount(device, 79U) == Fs::ExfatStatus::Done) &&
                                                    (g_volume.Open(reinterpret_cast<const UInt8*>("README.TXT"), 10U,
                                                                   found) == Fs::ExfatStatus::Done) &&
                                                    ReadsBack(found)),
                "a replaced file opens and reads back after a new mount");
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

/// Block b of the seed (block 0 the boot sector).
[[nodiscard]] UInt8* SeedBlock(const Size block) noexcept
{
    return g_seed + (block * Block);
}

/// An entry set for name, written at set (contiguous: flags 0x03 with a
/// cluster, 0x01 without), with its SetChecksum and NameHash.
void PutSet(UInt8* const set, const char* const name, const UInt32 first, const UInt32 bytes,
            const UInt32 flags) noexcept
{
    const Size length = std::strlen(name);
    const Size names = (length + 14U) / 15U;
    std::memset(set, 0, (2U + names) * 32U);
    set[0U] = 0x85U;
    set[1U] = static_cast<UInt8>(1U + names);
    Put16(set + 4U, 0x20U);
    set[32U] = 0xC0U;
    set[33U] = static_cast<UInt8>(flags);
    set[35U] = static_cast<UInt8>(length);
    UInt32 hash{0U};
    for (Size at{0U}; at < length; ++at)
    {
        const UInt32 upper = ((name[at] >= 'a') && (name[at] <= 'z')) ? static_cast<UInt32>(name[at] - 32)
                                                                      : static_cast<UInt32>(name[at]);
        hash = (((hash >> 1U) | (hash << 15U)) + upper) & 0xFFFFU;
        hash = ((hash >> 1U) | (hash << 15U)) & 0xFFFFU;
        set[64U + ((at / 15U) * 32U)] = 0xC1U;
        Put16(set + 64U + ((at / 15U) * 32U) + 2U + ((at % 15U) * 2U), static_cast<UInt8>(name[at]));
    }
    Put16(set + 36U, hash);
    Put32(set + 40U, bytes);
    Put32(set + 52U, first);
    Put32(set + 56U, bytes);
    UInt32 sum{0U};
    for (Size at{0U}; at < ((2U + names) * 32U); ++at)
    {
        sum = ((at == 2U) || (at == 3U)) ? sum : ((((sum >> 1U) | (sum << 15U)) + set[at]) & 0xFFFFU);
    }
    Put16(set + 2U, sum);
}

/// A volume of 120 clusters of 512 bytes: the FAT at sector 24 (block 6), the
/// heap at 26; the bitmap in cluster 2 (block 1), the up-case table in 3
/// (block 2), the root in 4 (block 3) and 8 (block 7); README.TXT in cluster
/// 5 (block 4); a long name chained over clusters 6 and 13 (block 5).
void BuildVolume() noexcept
{
    std::memset(g_seed, 0, sizeof(g_seed));
    UInt8* const boot = SeedBlock(0U);
    boot[0U] = 0xEBU;
    boot[1U] = 0x76U;
    boot[2U] = 0x90U;
    std::memcpy(boot + 3U, "EXFAT   ", 8U);
    Put32(boot + 72U, 146U);
    Put32(boot + 80U, 24U);
    Put32(boot + 84U, 1U);
    Put32(boot + 88U, 26U);
    Put32(boot + 92U, 120U);
    Put32(boot + 96U, 4U);
    boot[105U] = 1U;
    boot[108U] = 9U;
    boot[110U] = 1U;
    boot[510U] = 0x55U;
    boot[511U] = 0xAAU;
    UInt8* const bitmap = SeedBlock(1U);
    bitmap[0U] = 0x5FU;
    bitmap[1U] = 0x08U;
    UInt8* const fat = SeedBlock(6U);
    static constexpr UInt32 Chain[14U]{0xFFFF'FFF8U, 0xFFFF'FFFFU, 0xFFFF'FFFFU, 0xFFFF'FFFFU, 8U,
                                       0xFFFF'FFFFU, 13U,          0U,           0xFFFF'FFFFU, 0U,
                                       0U,           0U,           0U,           0xFFFF'FFFFU};
    for (Size cluster{0U}; cluster < 14U; ++cluster)
    {
        Put32(fat + (cluster * 4U), Chain[cluster]);
    }
    UInt8* const root = SeedBlock(3U);
    root[0U] = 0x83U;
    root[32U] = 0x81U;
    Put32(root + 32U + 20U, 2U);
    Put32(root + 32U + 24U, 15U);
    root[64U] = 0x82U;
    Put32(root + 64U + 20U, 3U);
    Put32(root + 64U + 24U, 512U);
    PutSet(root + 96U, "README.TXT", 5U, 13U, 0x03U);
    PutSet(root + 192U, "a-long-lower-name.txt", 6U, 600U, 0x01U);
    for (Size slot{10U}; slot < 15U; ++slot)
    {
        root[slot * 32U] = 0x05U;
    }
    // A set across the root's two clusters: its File entry in slot 15.
    static UInt8 straddle[3U * 32U];
    PutSet(straddle, "straddle.bin", 0U, 0U, 0x01U);
    std::memcpy(root + (15U * 32U), straddle, 32U);
    std::memcpy(SeedBlock(7U), straddle + 32U, 64U);
    std::memcpy(SeedBlock(4U), "hello readme\n", 13U);
    for (Size at{0U}; at < Block; ++at)
    {
        SeedBlock(5U)[at] = static_cast<UInt8>((at * 7U) + 3U);
    }
}
} // namespace

extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t* data, std::size_t size))
{
    BuildVolume();
    emit(g_seed, sizeof(g_seed));
    // Read-only: VolumeDirty set.
    SeedBlock(0U)[106U] = 0x02U;
    emit(g_seed, sizeof(g_seed));
    // A wrong boot checksum, and TexFAT's two FATs.
    SeedBlock(0U)[106U] = 0U;
    SeedBlock(0U)[112U] = 0x80U;
    emit(g_seed, sizeof(g_seed));
    SeedBlock(0U)[112U] = 0U;
    SeedBlock(0U)[110U] = 2U;
    emit(g_seed, sizeof(g_seed));
    // The boot sector alone, and a sector that decodes to nothing.
    SeedBlock(0U)[110U] = 1U;
    emit(g_seed, Block);
    std::memset(g_seed, 0, Block);
    emit(g_seed, Block);
}
