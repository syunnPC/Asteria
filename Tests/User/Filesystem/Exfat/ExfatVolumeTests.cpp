// SPDX-License-Identifier: Apache-2.0
// Example tests of the exFAT volume library (ExfatVolume.hpp,
// Source/User/Filesystem/Exfat/ExfatVolume.hpp) on the images ExfatToolImages.py makes with
// mkfs.exfat (64 MiB, 4 KiB and 512-byte clusters), with files the test writer places as another
// implementation would: what the mount accepts and refuses, how the directory decoder treats
// foreign entry sets (FAT-chained, across sectors, benign entries, broken sums, a valid data length
// short of the data length, directories), and the write path's refusals, placement, dirty flag,
// crash points and device failures. The crash sweep and the reference model have tests of their
// own. Without the images (exfatprogs absent) the test prints SKIP.

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "Support/TestContext.hpp"
#include "User/Filesystem/Exfat/ExfatTestSupport.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Exfat::Bytes;
using Exfat::CheckExfatVolume;
using Exfat::CheckReport;
using Exfat::DeviceOf;
using Exfat::FileSpec;
using Exfat::FillPage;
using Exfat::Get32;
using Exfat::Get16;
using Exfat::Get64;
using Exfat::Layout;
using Exfat::NoBudget;
using Exfat::Put32;
using Exfat::Put64;
using Exfat::SparseDisk;
using Exfat::SparseStorage;
using Exfat::TestDevice;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::ExfatCrashPoint;
using User::Filesystem::ExfatFileSize;
using User::Filesystem::ExfatObject;
using User::Filesystem::ExfatPageBytes;
using User::Filesystem::ExfatStatus;
using User::Filesystem::ExfatVolume;

SparseStorage<12U> g_storage4k;
SparseStorage<12U> g_storage512;
SparseStorage<14U> g_overlayStorage;
SparseDisk g_base4k{g_storage4k.Keys, g_storage4k.Data, 12U};
SparseDisk g_base512{g_storage512.Keys, g_storage512.Data, 12U};
SparseDisk g_overlay{g_overlayStorage.Keys, g_overlayStorage.Data, 14U};
TestDevice g_device4k{g_base4k, &g_overlay};
TestDevice g_device512{g_base512, &g_overlay};
Layout g_layout4k{};
Layout g_layout512{};
ExfatVolume g_volume;
UInt8 g_page[ExfatPageBytes];
UInt8 g_expected[ExfatPageBytes];
ExfatFileSize g_size{};
UInt64 g_serial{1U};

constexpr UInt8 Readme[]{"hello readme\n"};
/// The library's fixed timestamp 00:00:00 in exFAT's encoding.
constexpr UInt32 Fixed2026{0x5C21'0000U};

/// Starts a case on device: an empty overlay, no write budget, reads that work.
TestDevice& Fresh(TestDevice& device) noexcept
{
    g_overlay.Reset(device.Get().Sectors);
    device.Arm(NoBudget);
    device.SetFailReads(false);
    return device;
}

[[nodiscard]] ExfatStatus MountOn(TestDevice& device) noexcept
{
    return g_volume.Mount(DeviceOf(device), ++g_serial);
}

[[nodiscard]] FileSpec Spec(const char* const name, const UInt32 index, const UInt32 first, const UInt32 stride,
                            const UInt64 size, const UInt32 seed) noexcept
{
    return FileSpec{.Name = name,
                    .Index = index,
                    .First = first,
                    .Stride = stride,
                    .Size = size,
                    .Valid = size,
                    .Data = nullptr,
                    .Seed = seed,
                    .Attributes = 0x20U,
                    .Benign = 0U,
                    .ChecksumDelta = 0U,
                    .HashDelta = 0U};
}

void XorByte(TestDevice& device, const UInt64 lba, const Size offset, const UInt8 value) noexcept
{
    UInt8 sector[512U]{};
    static_cast<void>(device.Read(lba, sector));
    sector[offset] = static_cast<UInt8>(sector[offset] ^ value);
    static_cast<void>(device.Write(lba, sector));
}

[[nodiscard]] UInt8 ByteAt(const TestDevice& device, const UInt64 lba, const Size offset) noexcept
{
    UInt8 sector[512U]{};
    static_cast<void>(device.Read(lba, sector));
    return sector[offset];
}

/// Entries first to last become deleted (type 0x05), as gaps another
/// implementation left.
void MarkDeleted(TestDevice& device, const Layout& layout, const UInt32 first, const UInt32 last) noexcept
{
    UInt8 entry[32U]{};
    entry[0U] = 0x05U;
    for (UInt32 index{first}; index <= last; ++index)
    {
        static_cast<void>(Exfat::WriteEntry(device, layout, index, entry));
    }
}

void PatchEntry(TestDevice& device, const Layout& layout, const UInt32 index, const Size offset,
                const UInt8 value) noexcept
{
    UInt8 entry[32U]{};
    static_cast<void>(Exfat::ReadEntry(device, layout, index, entry));
    entry[offset] = value;
    static_cast<void>(Exfat::WriteEntry(device, layout, index, entry));
}

/// Recomputes the SetChecksum of the set whose File entry is start, so that
/// a fault a test patched into the set is its only one.
void Reseal(TestDevice& device, const Layout& layout, const UInt32 start) noexcept
{
    static UInt8 set[19U * 32U];
    static_cast<void>(Exfat::ReadEntry(device, layout, start, set));
    const Size count = 1U + set[1U];
    for (Size at{1U}; (at < count) && (at < 19U); ++at)
    {
        static_cast<void>(Exfat::ReadEntry(device, layout, start + static_cast<UInt32>(at), set + (at * 32U)));
    }
    Exfat::Put16(set + 2U, Exfat::SetChecksum(set, count));
    static_cast<void>(Exfat::WriteEntry(device, layout, start, set));
}

[[nodiscard]] ExfatStatus OpenName(const char* const name, ExfatObject& object) noexcept
{
    return g_volume.Open(Bytes(name), std::strlen(name), object);
}

[[nodiscard]] ExfatStatus CreateName(const char* const name, ExfatObject& object) noexcept
{
    return g_volume.Create(Bytes(name), std::strlen(name), object);
}

[[nodiscard]] ExfatStatus WriteSeed(const ExfatObject& object, const UInt64 page, const UInt32 seed) noexcept
{
    FillPage(g_page, seed);
    return g_volume.WritePage(object, page, g_page);
}

/// Whether page reads as FillPage(seed) for valid bytes, zeros after (seed 0:
/// zeros throughout).
[[nodiscard]] bool PageHolds(const ExfatObject& object, const UInt64 page, const UInt32 seed, const Size valid) noexcept
{
    FillPage(g_expected, seed);
    std::memset(g_expected + valid, 0, ExfatPageBytes - valid);
    return (g_volume.ReadPage(object, page, g_page) == ExfatStatus::Done) &&
           (std::memcmp(g_page, g_expected, ExfatPageBytes) == 0);
}

[[nodiscard]] bool StatIs(const ExfatObject& object, const UInt64 bytes) noexcept
{
    ExfatFileSize size{};
    return (g_volume.Stat(object, size) == ExfatStatus::Done) && (size.Bytes == bytes) &&
           (size.Pages == ((bytes + 4095U) / 4096U));
}

/// The directory entry index's 32 bytes.
[[nodiscard]] bool EntryOf(const TestDevice& device, const Layout& layout, const UInt64 index,
                           UInt8 (&entry)[32U]) noexcept
{
    return Exfat::ReadEntry(device, layout, static_cast<UInt32>(index), entry);
}

void TestMount(TestContext& context) noexcept
{
    TestDevice& device = Fresh(g_device4k);
    ASTERIA_EXPECT(
        context, "a tool-made volume with the token mounts writable", MountOn(device) == ExfatStatus::Done,
        g_volume.IsMounted(), !g_volume.IsReadOnly(), g_volume.GetGeometry().HeapStart == g_layout4k.HeapStart,
        g_volume.GetGeometry().FatStart == g_layout4k.FatStart, g_volume.GetGeometry().Clusters == g_layout4k.Clusters,
        g_volume.GetGeometry().RootCluster == g_layout4k.RootCluster, g_volume.GetGeometry().ClusterShift == 3U,
        g_volume.GetGeometry().VolumeSectors == 131072U);

    struct Damage
    {
        const char* Description;
        UInt64 Lba;
        Size Offset;
        UInt8 Xor;
        ExfatStatus Expected;
        bool ReadOnly;
    };
    static constexpr Damage Damages[]{
        {"a byte of an extended boot sector, which the boot checksum covers", 5U, 100U, 0x01U, ExfatStatus::Corrupt,
         false},
        {"a byte of the token sector", 8U, 0U, 0x01U, ExfatStatus::Corrupt, false},
        {"a byte of the OEM parameters", 9U, 7U, 0x80U, ExfatStatus::Corrupt, false},
        {"a byte of the reserved sector", 10U, 511U, 0x40U, ExfatStatus::Corrupt, false},
        {"the boot sector's serial number", 0U, 100U, 0x01U, ExfatStatus::Corrupt, false},
        {"one of the 128 checksum copies", 11U, 400U, 0x01U, ExfatStatus::Corrupt, false},
        {"VolumeDirty, outside the checksum", 0U, 106U, 0x02U, ExfatStatus::Done, true},
        {"MediaFailure, outside the checksum", 0U, 106U, 0x04U, ExfatStatus::Done, true},
        {"ClearToZero, outside the checksum", 0U, 106U, 0x08U, ExfatStatus::Done, false},
        {"VolumeFlags' high byte", 0U, 107U, 0x01U, ExfatStatus::Done, false},
        {"PercentInUse", 0U, 112U, 0x33U, ExfatStatus::Done, false},
        {"the backup boot region, which Mount does not read", 12U, 100U, 0x01U, ExfatStatus::Done, false},
        {"the file system name", 0U, 3U, 0x01U, ExfatStatus::NotExfat, false},
    };
    for (const Damage& damage : Damages)
    {
        Fresh(device);
        XorByte(device, damage.Lba, damage.Offset, damage.Xor);
        const ExfatStatus status = MountOn(device);
        ASTERIA_EXPECT(context, damage.Description, status == damage.Expected,
                       g_volume.IsMounted() == (status == ExfatStatus::Done),
                       (status != ExfatStatus::Done) || (g_volume.IsReadOnly() == damage.ReadOnly));
    }

    // A dirty volume is read-only, and its flag is never cleared.
    Fresh(device);
    XorByte(device, 0U, 106U, 0x02U);
    ExfatObject object{};
    ExfatObject created{};
    ASTERIA_EXPECT(context, "a dirty volume mounts read-only and keeps its flag", MountOn(device) == ExfatStatus::Done,
                   g_volume.IsReadOnly(), CreateName("new", created) == ExfatStatus::Refused,
                   g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done,
                   (ByteAt(device, 0U, 106U) & 0x02U) != 0U, device.GetWrites() == 0U);

    // The root directory's allocation bitmap entry.
    UInt8 entry[32U]{};
    Fresh(device);
    static_cast<void>(EntryOf(device, g_layout4k, 2U, entry));
    entry[0U] = 0x01U;
    static_cast<void>(Exfat::WriteEntry(device, g_layout4k, 2U, entry));
    ASTERIA_EXPECT(context, "a volume without an allocation bitmap entry is corrupt",
                   MountOn(device) == ExfatStatus::Corrupt, !g_volume.IsMounted(),
                   OpenName("x", object) == ExfatStatus::Refused);
    Fresh(device);
    static_cast<void>(EntryOf(device, g_layout4k, 2U, entry));
    Put64(entry + 24U, 1983U);
    static_cast<void>(Exfat::WriteEntry(device, g_layout4k, 2U, entry));
    ASTERIA_EXPECT(context, "a bitmap a byte short of the heap is corrupt", MountOn(device) == ExfatStatus::Corrupt);
    Fresh(device);
    static_cast<void>(EntryOf(device, g_layout4k, 2U, entry));
    entry[1U] = 0x01U;
    static_cast<void>(Exfat::WriteEntry(device, g_layout4k, 2U, entry));
    ASTERIA_EXPECT(context, "TexFAT's second bitmap is not the volume's", MountOn(device) == ExfatStatus::Corrupt);

    // A root directory that continues through a broken FAT entry: on the
    // 512-byte image the root is one sector, here all in use or deleted.
    TestDevice& small = Fresh(g_device512);
    MarkDeleted(small, g_layout512, 4U, 15U);
    Exfat::WriteFat(small, g_layout512, g_layout512.RootCluster, 1U);
    ASTERIA_EXPECT(context, "a root chain that leaves the heap is corrupt", MountOn(small) == ExfatStatus::Corrupt);

    Fresh(device);
    device.SetFailReads(true);
    ASTERIA_EXPECT(context, "a read failure fails the mount", MountOn(device) == ExfatStatus::Failed,
                   !g_volume.IsMounted());
    device.SetFailReads(false);
    UInt8 zero[512U]{};
    static_cast<void>(device.Write(0U, zero));
    ASTERIA_EXPECT(context, "an unmounted volume refuses every operation", MountOn(device) == ExfatStatus::NotExfat,
                   OpenName("readme.txt", object) == ExfatStatus::Refused,
                   CreateName("x", object) == ExfatStatus::Refused,
                   g_volume.Stat(object, g_size) == ExfatStatus::Refused,
                   g_volume.ReadPage(object, 0U, g_page) == ExfatStatus::Refused,
                   g_volume.WritePage(object, 0U, g_page) == ExfatStatus::Refused,
                   g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Refused,
                   ExfatVolume::QueryCommit() == ExfatStatus::Unsupported);
}

/// Foreign entry sets on the 4 KiB image (indexes in the root directory;
/// sectors hold 16 entries).
void PlaceForeignFiles(TestDevice& device, const Layout& layout) noexcept
{
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("contig.bin", 4U, 40U, 0U, 12388U, 50U)));
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("chain.bin", 7U, 60U, 3U, 5U * 4096U, 60U)));
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("short-chain.bin", 10U, 80U, 1U, 3U * 4096U, 70U)));
    Exfat::WriteFat(device, layout, 81U, Exfat::EndOfChain);
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("big.bin", 13U, 100U, 0U, 8U * 4096U, 80U)));
    FileSpec valid = Spec("short-valid.bin", 16U, 120U, 0U, 8192U, 90U);
    valid.Valid = 5000U;
    static_cast<void>(Exfat::PlaceFile(device, layout, valid));
    // A volume GUID entry (a benign primary), then a set with two vendor
    // extension entries after its name.
    UInt8 guid[32U]{};
    guid[0U] = 0xA0U;
    for (Size at{6U}; at < 22U; ++at)
    {
        guid[at] = static_cast<UInt8>(at * 13U);
    }
    static_cast<void>(Exfat::WriteEntry(device, layout, 19U, guid));
    FileSpec vendor = Spec("vendor.bin", 20U, 130U, 0U, 4096U, 100U);
    vendor.Benign = 2U;
    static_cast<void>(Exfat::PlaceFile(device, layout, vendor));
    // A deleted set (its types' InUse bits clear) and an orphaned File Name.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("gone.bin", 25U, 135U, 0U, 0U, 0U)));
    UInt8 entry[32U]{};
    for (UInt32 index{25U}; index < 28U; ++index)
    {
        static_cast<void>(EntryOf(device, layout, index, entry));
        entry[0U] = static_cast<UInt8>(entry[0U] & 0x7FU);
        static_cast<void>(Exfat::WriteEntry(device, layout, index, entry));
    }
    static_cast<void>(EntryOf(device, layout, 6U, entry));
    static_cast<void>(Exfat::WriteEntry(device, layout, 28U, entry));
    MarkDeleted(device, layout, 29U, 30U);
    // File entry in slot 15 of sector 1, its Stream Extension in sector 2.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("straddle.bin", 31U, 140U, 0U, 4096U, 110U)));
    MarkDeleted(device, layout, 34U, 45U);
    // File and Stream Extension in slots 14 and 15 of sector 2, the names
    // in sector 3.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("names-across.bin", 46U, 150U, 0U, 4096U, 120U)));
    FileSpec badSum = Spec("bad-sum.bin", 50U, 160U, 0U, 4096U, 130U);
    badSum.ChecksumDelta = 1U;
    static_cast<void>(Exfat::PlaceFile(device, layout, badSum));
    FileSpec badHash = Spec("bad-hash.bin", 53U, 170U, 0U, 4096U, 140U);
    badHash.HashDelta = 1U;
    static_cast<void>(Exfat::PlaceFile(device, layout, badHash));
    FileSpec directory = Spec("dir", 56U, 180U, 0U, 4096U, 0U);
    directory.Attributes = 0x10U;
    static_cast<void>(Exfat::PlaceFile(device, layout, directory));
    // A name outside ASCII whose NameHash, which needs the up-case table,
    // is not the ASCII one: not verified, so the set is a file.
    FileSpec foreign = Spec("caf\xE9", 59U, 190U, 0U, 4096U, 150U);
    foreign.HashDelta = 1U;
    static_cast<void>(Exfat::PlaceFile(device, layout, foreign));
    // A set cut short: a File and a Stream Extension entry, then a primary.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("cut.bin", 62U, 200U, 0U, 0U, 0U)));
    static_cast<void>(Exfat::WriteEntry(device, layout, 64U, guid));
    // A chain of two consecutive clusters whose FAT entries lie in the FAT's
    // second sector; a name with an uppercase Z.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("pair.bin", 65U, 220U, 1U, 8192U, 160U)));
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("ZIP.Z", 68U, 230U, 0U, 4096U, 170U)));
    // Malformed sets, resealed so each has one fault: a File Name entry
    // where the Stream Extension belongs; no name (its hash 0 and a vendor
    // entry where the name was); a SecondaryCount short of the name; a
    // valid data length past the data length; a vendor entry where a name
    // entry belongs; a critical secondary entry after the name.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("m-stream.bin", 71U, 0U, 0U, 0U, 0U)));
    PatchEntry(device, layout, 72U, 0U, 0xC1U);
    Reseal(device, layout, 71U);
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("m-noname.bin", 74U, 0U, 0U, 0U, 0U)));
    PatchEntry(device, layout, 75U, 3U, 0U);
    PatchEntry(device, layout, 75U, 4U, 0U);
    PatchEntry(device, layout, 75U, 5U, 0U);
    PatchEntry(device, layout, 76U, 0U, 0xE0U);
    Reseal(device, layout, 74U);
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("m-short-name.bin", 77U, 0U, 0U, 0U, 0U)));
    PatchEntry(device, layout, 77U, 1U, 2U);
    const UInt32 shortHash = Exfat::NameHash("m-short-name.bi", 15U);
    PatchEntry(device, layout, 78U, 4U, static_cast<UInt8>(shortHash));
    PatchEntry(device, layout, 78U, 5U, static_cast<UInt8>(shortHash >> 8U));
    Reseal(device, layout, 77U);
    FileSpec beyond = Spec("m-valid.bin", 81U, 240U, 0U, 4096U, 0U);
    beyond.Valid = 4097U;
    static_cast<void>(Exfat::PlaceFile(device, layout, beyond));
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("m-type.bin", 84U, 0U, 0U, 0U, 0U)));
    PatchEntry(device, layout, 86U, 0U, 0xE0U);
    Reseal(device, layout, 84U);
    FileSpec critical = Spec("m-crit.bin", 87U, 0U, 0U, 0U, 0U);
    critical.Benign = 1U;
    static_cast<void>(Exfat::PlaceFile(device, layout, critical));
    PatchEntry(device, layout, 90U, 0U, 0xC2U);
    Reseal(device, layout, 87U);
    // DEL is ASCII, 0x80 is not: the first two hashes are verified (one is
    // wrong), the third is not.
    static_cast<void>(Exfat::PlaceFile(device, layout,
                                       Spec("a\x7F"
                                            "b",
                                            91U, 0U, 0U, 0U, 0U)));
    FileSpec del = Spec("c\x7F"
                        "d",
                        94U, 0U, 0U, 0U, 0U);
    del.HashDelta = 1U;
    static_cast<void>(Exfat::PlaceFile(device, layout, del));
    FileSpec high = Spec("e\x80"
                         "f",
                         97U, 0U, 0U, 0U, 0U);
    high.HashDelta = 1U;
    static_cast<void>(Exfat::PlaceFile(device, layout, high));
    // The longest name exFAT stores (SecondaryCount 18), across sectors.
    static char longest[256U];
    std::memset(longest, 'n', 255U);
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec(longest, 100U, 0U, 0U, 0U, 0U)));
    // A file in the heap's last cluster, and one that runs past it.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("last.bin", 119U, layout.Clusters + 1U, 0U, 4096U, 180U)));
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("past.bin", 122U, layout.Clusters + 1U, 0U, 8192U, 0U)));
    // A chain whose second link names the cluster after the heap's last.
    static_cast<void>(Exfat::PlaceFile(device, layout, Spec("beyond.bin", 125U, 250U, 1U, 8192U, 190U)));
    Exfat::WriteFat(device, layout, 250U, layout.Clusters + 2U);
}

void TestForeignSets(TestContext& context) noexcept
{
    TestDevice& device = Fresh(g_device4k);
    PlaceForeignFiles(device, g_layout4k);
    ExfatObject contig{};
    ExfatObject same{};
    ASTERIA_EXPECT(context, "a contiguous file reads in any case and zeros past its size",
                   MountOn(device) == ExfatStatus::Done, OpenName("contig.bin", contig) == ExfatStatus::Done,
                   contig.Identifier == 4U, contig.Generation == g_serial,
                   OpenName("CONTIG.BIN", same) == ExfatStatus::Done, same.Identifier == 4U, StatIs(contig, 12388U),
                   PageHolds(contig, 0U, 50U, 4096U), PageHolds(contig, 1U, 51U, 4096U),
                   PageHolds(contig, 2U, 52U, 4096U), PageHolds(contig, 3U, 53U, 100U), PageHolds(contig, 4U, 0U, 0U),
                   PageHolds(contig, 5U, 0U, 0U), g_volume.ReadPage(contig, 6U, g_page) == ExfatStatus::Done,
                   g_volume.ReadPage(contig, 0U, nullptr) == ExfatStatus::Refused);
    ExfatObject chain{};
    ASTERIA_EXPECT(context, "a FAT-chained file reads through its chain",
                   OpenName("chain.bin", chain) == ExfatStatus::Done, StatIs(chain, 5U * 4096U),
                   PageHolds(chain, 4U, 64U, 4096U), PageHolds(chain, 0U, 60U, 4096U), PageHolds(chain, 3U, 63U, 4096U),
                   PageHolds(chain, 1U, 61U, 4096U), PageHolds(chain, 2U, 62U, 4096U), PageHolds(chain, 5U, 0U, 0U));
    ExfatObject broken{};
    ASTERIA_EXPECT(context, "a chain that ends before its size is corrupt where it ends",
                   OpenName("short-chain.bin", broken) == ExfatStatus::Done, PageHolds(broken, 1U, 71U, 4096U),
                   g_volume.ReadPage(broken, 2U, g_page) == ExfatStatus::Corrupt);
    ExfatObject big{};
    ASTERIA_EXPECT(context, "a file over six pages reads its first six and is not written",
                   OpenName("big.bin", big) == ExfatStatus::Done, StatIs(big, 8U * 4096U),
                   PageHolds(big, 5U, 85U, 4096U), g_volume.ReadPage(big, 6U, g_page) == ExfatStatus::Done,
                   WriteSeed(big, 0U, 1U) == ExfatStatus::Done, g_volume.Abort() == ExfatStatus::Done);
    ExfatObject valid{};
    ASTERIA_EXPECT(context, "bytes past the valid data length read as zeros; such a file is not written",
                   OpenName("short-valid.bin", valid) == ExfatStatus::Done, StatIs(valid, 8192U),
                   PageHolds(valid, 0U, 90U, 4096U), PageHolds(valid, 1U, 91U, 904U),
                   WriteSeed(valid, 0U, 1U) == ExfatStatus::Refused);
    ExfatObject vendor{};
    ExfatObject straddle{};
    ExfatObject object{};
    ASTERIA_EXPECT(context, "benign entries, deleted sets and orphaned entries are skipped",
                   OpenName("vendor.bin", vendor) == ExfatStatus::Done, vendor.Identifier == 20U,
                   PageHolds(vendor, 0U, 100U, 4096U), OpenName("gone.bin", object) == ExfatStatus::NotFound,
                   g_volume.Stat(ExfatObject{.Identifier = 25U, .Generation = g_serial}, g_size) ==
                       ExfatStatus::StaleObject,
                   g_volume.ReadPage(ExfatObject{.Identifier = 28U, .Generation = g_serial}, 0U, g_page) ==
                       ExfatStatus::StaleObject,
                   g_volume.ReadPage(ExfatObject{.Identifier = 19U, .Generation = g_serial}, 0U, g_page) ==
                       ExfatStatus::StaleObject);
    ASTERIA_EXPECT(context, "a set across sectors reads; its File and Stream Extension apart, it is not written",
                   OpenName("straddle.bin", straddle) == ExfatStatus::Done, straddle.Identifier == 31U,
                   PageHolds(straddle, 0U, 110U, 4096U), WriteSeed(straddle, 0U, 1U) == ExfatStatus::Refused);
    ASTERIA_EXPECT(
        context, "broken sums make the named set corrupt", OpenName("bad-sum.bin", object) == ExfatStatus::Corrupt,
        OpenName("bad-hash.bin", object) == ExfatStatus::Corrupt,
        g_volume.ReadPage(ExfatObject{.Identifier = 50U, .Generation = g_serial}, 0U, g_page) == ExfatStatus::Corrupt,
        g_volume.ReadPage(ExfatObject{.Identifier = 62U, .Generation = g_serial}, 0U, g_page) == ExfatStatus::Corrupt,
        CreateName("bad-sum.bin", object) == ExfatStatus::Refused, OpenName("contig.bin", object) == ExfatStatus::Done);
    ASTERIA_EXPECT(
        context, "a directory is not a file, but its name is taken", OpenName("dir", object) == ExfatStatus::Done,
        g_volume.ReadPage(ExfatObject{.Identifier = 56U, .Generation = g_serial}, 0U, g_page) == ExfatStatus::Refused,
        CreateName("dir", object) == ExfatStatus::Refused);
    ASTERIA_EXPECT(context, "a name outside ASCII is never matched and its hash not verified",
                   OpenName("caf", object) == ExfatStatus::NotFound, OpenName("cafe", object) == ExfatStatus::NotFound,
                   g_volume.ReadPage(ExfatObject{.Identifier = 59U, .Generation = g_serial}, 0U, g_page) ==
                       ExfatStatus::Corrupt);
    ASTERIA_EXPECT(context, "names of no file and other generations are refused or stale",
                   g_volume.Open(Bytes("x"), 0U, object) == ExfatStatus::Refused,
                   g_volume.Open(nullptr, 1U, object) == ExfatStatus::Refused,
                   g_volume.Open(Bytes("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 65U,
                                 object) == ExfatStatus::Refused,
                   g_volume.ReadPage(ExfatObject{.Identifier = 2U, .Generation = g_serial}, 0U, g_page) ==
                       ExfatStatus::StaleObject,
                   g_volume.ReadPage(ExfatObject{.Identifier = 70000U, .Generation = g_serial}, 0U, g_page) ==
                       ExfatStatus::StaleObject,
                   g_volume.ReadPage(ExfatObject{.Identifier = 4U, .Generation = g_serial - 1U}, 0U, g_page) ==
                       ExfatStatus::StaleObject);

    // Replacing a set whose names continue in the next sector, and one with
    // vendor extensions: the SetChecksum spans every entry, so the files
    // still open (the sums are verified) after a remount.
    ExfatObject across{};
    const bool written = (OpenName("names-across.bin", across) == ExfatStatus::Done) && (across.Identifier == 46U) &&
                         (WriteSeed(across, 1U, 121U) == ExfatStatus::Done) &&
                         (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done) &&
                         (OpenName("vendor.bin", vendor) == ExfatStatus::Done) &&
                         (WriteSeed(vendor, 0U, 101U) == ExfatStatus::Done) &&
                         (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    UInt8 entry[32U]{};
    ASTERIA_EXPECT(context, "a replaced set's checksum spans its sectors and its benign entries", written,
                   MountOn(device) == ExfatStatus::Done, OpenName("names-across.bin", across) == ExfatStatus::Done,
                   StatIs(across, 8192U), PageHolds(across, 0U, 120U, 4096U), PageHolds(across, 1U, 121U, 4096U),
                   OpenName("vendor.bin", vendor) == ExfatStatus::Done, PageHolds(vendor, 0U, 101U, 4096U),
                   EntryOf(device, g_layout4k, 20U, entry), entry[1U] == 4U,
                   Get32(entry + 8U) == Exfat::WriterTimestamp, Get32(entry + 12U) == Fixed2026,
                   Get32(entry + 16U) == Fixed2026);

    // More foreign sets: a FAT chain read across clusters whose entries lie
    // past the FAT's first sector, case folding of 'Z', the boundaries of
    // ASCII and of the heap, the longest name, and one fault per set.
    const auto at = [](const UInt64 index) noexcept
    {
        return ExfatObject{.Identifier = index, .Generation = g_serial};
    };
    ExfatObject pair{};
    ASTERIA_EXPECT(
        context, "the further foreign sets read or are corrupt as they should",
        OpenName("pair.bin", pair) == ExfatStatus::Done, PageHolds(pair, 0U, 160U, 4096U),
        PageHolds(pair, 1U, 161U, 4096U), OpenName("zip.z", object) == ExfatStatus::Done, object.Identifier == 68U,
        g_volume.Stat(at(71U), g_size) == ExfatStatus::Corrupt, g_volume.Stat(at(74U), g_size) == ExfatStatus::Corrupt,
        g_volume.Stat(at(77U), g_size) == ExfatStatus::Corrupt,
        g_volume.Stat(at(80U), g_size) == ExfatStatus::StaleObject,
        g_volume.Stat(at(81U), g_size) == ExfatStatus::Corrupt, g_volume.Stat(at(84U), g_size) == ExfatStatus::Corrupt,
        g_volume.Stat(at(87U), g_size) == ExfatStatus::Corrupt,
        OpenName("a\x7F"
                 "b",
                 object) == ExfatStatus::Done,
        object.Identifier == 91U,
        OpenName("c\x7F"
                 "d",
                 object) == ExfatStatus::Corrupt,
        OpenName("e\x80"
                 "f",
                 object) == ExfatStatus::NotFound,
        g_volume.Stat(at(97U), g_size) == ExfatStatus::Corrupt, g_volume.Stat(at(100U), g_size) == ExfatStatus::Done,
        PageHolds(at(119U), 0U, 180U, 4096U), g_volume.ReadPage(at(122U), 1U, g_page) == ExfatStatus::Corrupt,
        PageHolds(at(125U), 0U, 190U, 4096U), g_volume.ReadPage(at(125U), 1U, g_page) == ExfatStatus::Corrupt);

    // Replacing FAT-chained files frees their clusters run by run: chain.bin's
    // five apart, pair.bin's two together.
    const bool chains = (OpenName("chain.bin", chain) == ExfatStatus::Done) &&
                        (WriteSeed(chain, 0U, 65U) == ExfatStatus::Done) &&
                        (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done) &&
                        (WriteSeed(pair, 0U, 162U) == ExfatStatus::Done) &&
                        (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    bool freed{true};
    for (const UInt32 cluster : {60U, 63U, 66U, 69U, 72U, 220U, 221U})
    {
        freed = freed && !Exfat::IsAllocated(device, g_layout4k, cluster);
    }
    ASTERIA_EXPECT(context, "a replaced chain's clusters are freed run by run", chains, freed,
                   PageHolds(chain, 0U, 65U, 4096U), PageHolds(chain, 4U, 64U, 4096U), PageHolds(pair, 0U, 162U, 4096U),
                   PageHolds(pair, 1U, 161U, 4096U));

    // A set created over a deleted one leaves none of its stale bytes.
    UInt8 name[32U]{};
    bool zeros = (CreateName("x", object) == ExfatStatus::Done) && (object.Identifier == 25U) &&
                 (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done) &&
                 EntryOf(device, g_layout4k, 27U, name) && (name[0U] == 0xC1U) && (name[2U] == 'x');
    for (Size offset{3U}; offset < 32U; ++offset)
    {
        zeros = zeros && (name[offset] == 0U);
    }
    ASTERIA_EXPECT(context, "a created set clears the deleted entries it takes, and no entry after them", zeros,
                   OpenName("x", object) == ExfatStatus::Done, EntryOf(device, g_layout4k, 28U, name),
                   name[0U] == 0xC1U, OpenName("straddle.bin", object) == ExfatStatus::Done);
}

void TestCreate(TestContext& context) noexcept
{
    TestDevice& device = Fresh(g_device4k);
    ExfatObject object{};
    static constexpr const char* Invalid[]{"Upper", ".dot", "dot.", "a b", "-a", "a_", "a/b", "caf\xE9"};
    bool refused = MountOn(device) == ExfatStatus::Done;
    for (const char* const name : Invalid)
    {
        refused = refused && (CreateName(name, object) == ExfatStatus::Refused);
    }
    static constexpr char Long[]{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    ASTERIA_EXPECT(context, "names outside the file protocol's rule are refused", refused,
                   g_volume.Create(Bytes(Long), 65U, object) == ExfatStatus::Refused,
                   g_volume.Create(Bytes(Long), 0U, object) == ExfatStatus::Refused,
                   g_volume.Create(nullptr, 1U, object) == ExfatStatus::Refused, device.GetWrites() == 0U);

    // An empty file: no cluster, AllocationPossible only, no NoFatChain.
    ExfatObject empty{};
    ExfatObject other{};
    UInt8 stream[32U]{};
    const bool created = (CreateName("e.m-p_t.y", empty) == ExfatStatus::Done) && (empty.Identifier == 4U) &&
                         (CreateName("other", other) == ExfatStatus::Refused) && StatIs(empty, 0U) &&
                         PageHolds(empty, 0U, 0U, 0U) && (OpenName("e.m-p_t.y", object) == ExfatStatus::NotFound) &&
                         (device.GetWrites() == 0U) && (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    const CheckReport report = CheckExfatVolume(device);
    UInt8 file[32U]{};
    ASTERIA_EXPECT(context, "a created file is an archive with the fixed timestamps", created,
                   EntryOf(device, g_layout4k, 4U, file), file[0U] == 0x85U, file[1U] == 2U, Get16(file + 4U) == 0x20U,
                   Get32(file + 8U) == Fixed2026, Get32(file + 12U) == Fixed2026, Get32(file + 16U) == Fixed2026);
    ASTERIA_EXPECT(context, "an empty file commits without a cluster", created, report.IsClean(), report.Files == 1U,
                   EntryOf(device, g_layout4k, 5U, stream), stream[1U] == 0x01U, Get32(stream + 20U) == 0U,
                   Get64(stream + 24U) == 0U, Get64(stream + 8U) == 0U,
                   OpenName("e.m-p_t.y", object) == ExfatStatus::Done, object.Identifier == 4U, StatIs(object, 0U),
                   CreateName("e.m-p_t.y", other) == ExfatStatus::Refused,
                   WriteSeed(object, 1U, 1U) == ExfatStatus::Refused);

    // Its first write replaces it; the dirty flag lives from the session's
    // first write to the end of its commit.
    const bool clean = (ByteAt(device, 0U, 106U) & 0x02U) == 0U;
    const bool replaced =
        (WriteSeed(object, 0U, 7U) == ExfatStatus::Done) && ((ByteAt(device, 0U, 106U) & 0x02U) != 0U) &&
        (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done) && ((ByteAt(device, 0U, 106U) & 0x02U) == 0U);
    const CheckReport after = CheckExfatVolume(device);
    ASTERIA_EXPECT(context, "an empty file's first write gives it a contiguous cluster", clean, replaced,
                   after.IsClean(), EntryOf(device, g_layout4k, 5U, stream), stream[1U] == 0x03U,
                   Get32(stream + 20U) == 6U, Get64(stream + 24U) == 4096U, PageHolds(object, 0U, 7U, 4096U),
                   g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);

    // The gap rule: three sets of three fill entries 4 to 12; a set of four
    // does not fit 13 to 15, so it goes to 16 and 13 to 15 become deleted.
    Fresh(device);
    bool placed = MountOn(device) == ExfatStatus::Done;
    for (const char* const name : {"f1", "f2", "f3"})
    {
        placed = placed && (CreateName(name, object) == ExfatStatus::Done) &&
                 (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    }
    UInt8 entry[32U]{};
    const auto typesAre = [&device, &entry](const UInt8 type) noexcept
    {
        bool same{true};
        for (UInt32 index{13U}; index < 16U; ++index)
        {
            same = same && EntryOf(device, g_layout4k, index, entry) && (entry[0U] == type);
        }
        return same;
    };
    const bool unused = typesAre(0x00U);
    placed = placed && (CreateName("a-name-of-twenty-chars", object) == ExfatStatus::Done) &&
             (object.Identifier == 16U) && (WriteSeed(object, 0U, 3U) == ExfatStatus::Done) &&
             (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    // A stale File entry past the end of the directory, in the sector the
    // next set of four takes (entries 13 to 15 hold three): the set's commit
    // leaves the entries after it unused.
    UInt8 stale[32U]{};
    static_cast<void>(EntryOf(device, g_layout4k, 16U, stale));
    static_cast<void>(Exfat::WriteEntry(device, g_layout4k, 25U, stale));
    placed = placed && (CreateName("g-name-of-twenty-chars", object) == ExfatStatus::Done) &&
             (object.Identifier == 20U) && (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    const CheckReport gap = CheckExfatVolume(device);
    ASTERIA_EXPECT(context, "a set in the sector after the end makes the free entries before it deleted", placed,
                   EntryOf(device, g_layout4k, 25U, entry), entry[0U] == 0x00U, unused, typesAre(0x05U),
                   EntryOf(device, g_layout4k, 20U, entry), entry[0U] == 0x85U, gap.IsClean(), gap.Files == 5U,
                   MountOn(device) == ExfatStatus::Done,
                   OpenName("a-name-of-twenty-chars", object) == ExfatStatus::Done, PageHolds(object, 0U, 3U, 4096U));

    // The root never grows: the 512-byte image's root sector has 12 free
    // entries, four sets of three.
    TestDevice& small = Fresh(g_device512);
    bool filled = MountOn(small) == ExfatStatus::Done;
    for (const char* const name : {"s1", "s2", "s3", "s4"})
    {
        filled = filled && (CreateName(name, object) == ExfatStatus::Done) &&
                 (WriteSeed(object, 0U, 9U) == ExfatStatus::Done) &&
                 (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    }
    const CheckReport full = CheckExfatVolume(small);
    // The longest protocol name, with the alphabet's last letter and digits.
    static constexpr char Longest[]{"z0aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa9"};
    static_assert(sizeof(Longest) == 65U);
    ASTERIA_EXPECT(context, "a full root directory refuses a create", filled,
                   CreateName("s5", object) == ExfatStatus::Refused, full.IsClean(), full.Files == 4U,
                   OpenName("s4", object) == ExfatStatus::Done, PageHolds(object, 0U, 9U, 4096U));
    Fresh(device);
    ExfatObject longest{};
    ASTERIA_EXPECT(context, "a name of 64 bytes is created and opened", MountOn(device) == ExfatStatus::Done,
                   CreateName(Longest, longest) == ExfatStatus::Done, WriteSeed(longest, 0U, 13U) == ExfatStatus::Done,
                   g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done,
                   OpenName(Longest, object) == ExfatStatus::Done, object.Identifier == longest.Identifier,
                   PageHolds(object, 0U, 13U, 4096U), CheckExfatVolume(device).IsClean());
}

void TestWrite(TestContext& context) noexcept
{
    // README.TXT as the interop image has it: 13 bytes, FAT-chained; its
    // cluster past the 13 bytes and the free clusters hold stale data, as
    // another implementation leaves them.
    TestDevice& device = Fresh(g_device4k);
    FillPage(g_page, 77U);
    for (UInt64 lba{Exfat::ClusterLba(g_layout4k, 6U)}; lba < Exfat::ClusterLba(g_layout4k, 40U); ++lba)
    {
        static_cast<void>(device.Write(lba, g_page + ((lba % 8U) * 512U)));
    }
    FileSpec readme = Spec("README.TXT", 4U, 10U, 1U, 13U, 0U);
    readme.Data = Readme;
    static_cast<void>(Exfat::PlaceFile(device, g_layout4k, readme));
    FillPage(g_page, 78U);
    std::memcpy(g_page, Readme, 13U);
    static_cast<void>(device.Write(Exfat::ClusterLba(g_layout4k, 10U), g_page));
    // A file of 4097 bytes, the last 511 bytes of its second page's first
    // sector stale.
    static_cast<void>(Exfat::PlaceFile(device, g_layout4k, Spec("odd.bin", 7U, 50U, 0U, 4097U, 60U)));
    ExfatObject file{};
    ExfatObject one{};
    ExfatObject object{};
    const bool mounted = (MountOn(device) == ExfatStatus::Done) && (OpenName("readme.txt", file) == ExfatStatus::Done);
    std::memcpy(g_expected, Readme, 13U);
    const bool partial = mounted && (g_volume.ReadPage(file, 0U, g_page) == ExfatStatus::Done) &&
                         (std::memcmp(g_page, Readme, 13U) == 0);
    ASTERIA_EXPECT(context, "a page write refuses pages past the file's and a session's other files", partial,
                   WriteSeed(file, 2U, 1U) == ExfatStatus::Refused, WriteSeed(file, 6U, 1U) == ExfatStatus::Refused,
                   g_volume.WritePage(file, 0U, nullptr) == ExfatStatus::Refused,
                   CreateName("one", one) == ExfatStatus::Done, WriteSeed(one, 1U, 1U) == ExfatStatus::Refused,
                   WriteSeed(one, 0U, 11U) == ExfatStatus::Done, WriteSeed(file, 0U, 1U) == ExfatStatus::Refused,
                   CreateName("two", object) == ExfatStatus::Refused,
                   g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);

    // Appending a page to the 13-byte file zeros the rest of its first page
    // in the copy; the chained cluster is freed at the commit.
    device.Arm(NoBudget);
    const bool appended = (WriteSeed(file, 1U, 12U) == ExfatStatus::Done) && (device.LogAt(0U) == 0U) &&
                          StatIs(file, 8192U) && PageHolds(file, 1U, 12U, 4096U) &&
                          (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    std::memset(g_expected, 0, sizeof(g_expected));
    std::memcpy(g_expected, Readme, 13U);
    const CheckReport report = CheckExfatVolume(device);
    ASTERIA_EXPECT(context, "an append zeros the old last page past its size; the old chain is freed", appended,
                   MountOn(device) == ExfatStatus::Done, OpenName("README.TXT", file) == ExfatStatus::Done,
                   StatIs(file, 8192U), g_volume.ReadPage(file, 0U, g_page) == ExfatStatus::Done,
                   std::memcmp(g_page, g_expected, ExfatPageBytes) == 0, PageHolds(file, 1U, 12U, 4096U),
                   report.IsClean(), report.Files == 3U, !Exfat::IsAllocated(device, g_layout4k, 10U));

    // A size one byte into a page: two pages, the second read to its byte;
    // an appended third page zeros the second past it.
    ExfatObject odd{};
    const bool grown = (OpenName("odd.bin", odd) == ExfatStatus::Done) && StatIs(odd, 4097U) &&
                       PageHolds(odd, 1U, 61U, 1U) && (WriteSeed(odd, 2U, 62U) == ExfatStatus::Done) &&
                       (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    ASTERIA_EXPECT(context, "a file's partial last page is read to its size and zeroed when it grows", grown,
                   MountOn(device) == ExfatStatus::Done, OpenName("odd.bin", odd) == ExfatStatus::Done,
                   StatIs(odd, 12288U), PageHolds(odd, 0U, 60U, 4096U), PageHolds(odd, 1U, 61U, 1U),
                   PageHolds(odd, 2U, 62U, 4096U), CheckExfatVolume(device).IsClean());

    // No run of six free clusters: the bitmap full but for clusters 6 to 10.
    Fresh(device);
    UInt8 full[512U]{};
    std::memset(full, 0xFF, sizeof(full));
    for (UInt64 sector{0U}; sector < 4U; ++sector)
    {
        static_cast<void>(device.Write(Exfat::ChainLba(device, g_layout4k, g_layout4k.BitmapCluster, sector), full));
    }
    for (UInt32 cluster{6U}; cluster <= 10U; ++cluster)
    {
        Exfat::SetAllocated(device, g_layout4k, cluster, false);
    }
    ExfatObject spare{};
    const bool none = (MountOn(device) == ExfatStatus::Done) && (CreateName("spare", spare) == ExfatStatus::Done) &&
                      (WriteSeed(spare, 0U, 5U) == ExfatStatus::Refused) &&
                      ((ByteAt(device, 0U, 106U) & 0x02U) == 0U) && StatIs(spare, 0U) &&
                      (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    Exfat::SetAllocated(device, g_layout4k, 11U, false);
    ASTERIA_EXPECT(
        context, "a write needs a free run of six pages' clusters", none, OpenName("spare", spare) == ExfatStatus::Done,
        StatIs(spare, 0U), WriteSeed(spare, 0U, 5U) == ExfatStatus::Done,
        g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done, Exfat::IsAllocated(device, g_layout4k, 6U),
        !Exfat::IsAllocated(device, g_layout4k, 7U), PageHolds(spare, 0U, 5U, 4096U));

    // A six-page replacement fits the run, but appending its seventh page
    // does not. Reject before dirtying or copying any of the old pages.
    Fresh(device);
    const bool placed = Exfat::PlaceFile(device, g_layout4k, Spec("append.bin", 4U, 100U, 1U, 6U * 4096U, 51U));
    std::memset(full, 0xFF, sizeof(full));
    for (UInt64 sector{0U}; sector < 4U; ++sector)
    {
        static_cast<void>(device.Write(Exfat::ChainLba(device, g_layout4k, g_layout4k.BitmapCluster, sector), full));
    }
    for (UInt32 cluster{6U}; cluster <= 11U; ++cluster)
    {
        Exfat::SetAllocated(device, g_layout4k, cluster, false);
    }
    const bool opened = MountOn(device) == ExfatStatus::Done && OpenName("append.bin", spare) == ExfatStatus::Done;
    device.Arm(Exfat::NoBudget);
    const auto append = WriteSeed(spare, 6U, 52U);
    ASTERIA_EXPECT(context, "AppendRunRefusal: an undersized replacement run is refused before disk writes", placed,
                   opened, append == ExfatStatus::Refused, device.GetWrites() == 0U,
                   (ByteAt(device, 0U, 106U) & 0x02U) == 0U, StatIs(spare, 6U * 4096U));

    // The 512-byte image's bitmap spans 31 clusters through the FAT: a run
    // from the end of its first sector into its second.
    TestDevice& small = Fresh(g_device512);
    for (Size at{0U}; at < 512U; ++at)
    {
        full[at] = static_cast<UInt8>((at == 511U) ? 0x1FU : 0xFFU);
    }
    static_cast<void>(small.Write(Exfat::ChainLba(small, g_layout512, g_layout512.BitmapCluster, 0U), full));
    ExfatObject span{};
    UInt8 stream[32U]{};
    const bool spanned = (MountOn(small) == ExfatStatus::Done) && (CreateName("span", span) == ExfatStatus::Done) &&
                         (WriteSeed(span, 0U, 21U) == ExfatStatus::Done) &&
                         (WriteSeed(span, 1U, 22U) == ExfatStatus::Done) &&
                         (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    bool bits{true};
    for (UInt32 cluster{4095U}; cluster < 4111U; ++cluster)
    {
        bits = bits && Exfat::IsAllocated(small, g_layout512, cluster);
    }
    ASTERIA_EXPECT(context, "the bitmap is followed through its chain on 512-byte clusters", spanned, bits,
                   !Exfat::IsAllocated(small, g_layout512, 4111U), EntryOf(small, g_layout512, 5U, stream),
                   Get32(stream + 20U) == 4095U, MountOn(small) == ExfatStatus::Done,
                   OpenName("span", span) == ExfatStatus::Done, PageHolds(span, 0U, 21U, 4096U),
                   PageHolds(span, 1U, 22U, 4096U));

    // A bitmap chain cut after its first cluster, which holds no free bit.
    Fresh(small);
    std::memset(full, 0xFF, sizeof(full));
    static_cast<void>(small.Write(Exfat::ChainLba(small, g_layout512, g_layout512.BitmapCluster, 0U), full));
    Exfat::WriteFat(small, g_layout512, g_layout512.BitmapCluster, Exfat::EndOfChain);
    ASTERIA_EXPECT(context, "a bitmap chain shorter than the heap is corrupt", MountOn(small) == ExfatStatus::Done,
                   CreateName("cut", span) == ExfatStatus::Done, WriteSeed(span, 0U, 1U) == ExfatStatus::Corrupt);

    // A page over eight clusters chained two apart, their FAT entries in the
    // FAT's third sector: read in eight runs, replaced, freed run by run.
    Fresh(small);
    static_cast<void>(Exfat::PlaceFile(small, g_layout512, Spec("spread.bin", 4U, 300U, 2U, 4096U, 23U)));
    ExfatObject spread{};
    const bool replaced = (MountOn(small) == ExfatStatus::Done) &&
                          (OpenName("spread.bin", spread) == ExfatStatus::Done) && PageHolds(spread, 0U, 23U, 4096U) &&
                          (WriteSeed(spread, 1U, 24U) == ExfatStatus::Done) &&
                          (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done);
    bool released{true};
    for (UInt32 cluster{300U}; cluster < 316U; cluster += 2U)
    {
        released = released && !Exfat::IsAllocated(small, g_layout512, cluster);
    }
    const CheckReport spreadReport = CheckExfatVolume(small);
    ASTERIA_EXPECT(context, "a chain of scattered clusters reads, and frees its clusters when replaced", replaced,
                   released, spreadReport.IsClean(), MountOn(small) == ExfatStatus::Done,
                   OpenName("spread.bin", spread) == ExfatStatus::Done, PageHolds(spread, 0U, 23U, 4096U),
                   PageHolds(spread, 1U, 24U, 4096U));
}

void TestCrashes(TestContext& context) noexcept
{
    // Before the entry set: the new run's bits are set, nothing names them.
    TestDevice& device = Fresh(g_device4k);
    ExfatObject object{};
    const bool before = (MountOn(device) == ExfatStatus::Done) && (CreateName("crash", object) == ExfatStatus::Done) &&
                        (WriteSeed(object, 0U, 31U) == ExfatStatus::Done) &&
                        (WriteSeed(object, 1U, 32U) == ExfatStatus::Done) &&
                        (g_volume.Commit(ExfatCrashPoint::BeforeDirectoryEntry) == ExfatStatus::Crashed);
    const bool broken = (OpenName("crash", object) == ExfatStatus::Failed) &&
                        (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Failed);
    CheckReport report = CheckExfatVolume(device);
    ASTERIA_EXPECT(context, "a crash before the entry set leaks the run and leaves the volume dirty", before, broken,
                   report.Errors == 0U, report.LostClusters == 2U, report.Dirty, MountOn(device) == ExfatStatus::Done,
                   g_volume.IsReadOnly(), OpenName("crash", object) == ExfatStatus::NotFound);

    // After it: the file is there, the volume still dirty.
    Fresh(device);
    const bool after = (MountOn(device) == ExfatStatus::Done) && (CreateName("crash", object) == ExfatStatus::Done) &&
                       (WriteSeed(object, 0U, 31U) == ExfatStatus::Done) &&
                       (g_volume.Commit(ExfatCrashPoint::AfterDirectoryEntry) == ExfatStatus::Crashed);
    report = CheckExfatVolume(device);
    ASTERIA_EXPECT(context, "a crash after the entry set keeps the file and the dirty flag", after, report.Errors == 0U,
                   report.LostClusters == 0U, report.Dirty, report.Files == 1U, MountOn(device) == ExfatStatus::Done,
                   g_volume.IsReadOnly(), OpenName("crash", object) == ExfatStatus::Done,
                   PageHolds(object, 0U, 31U, 4096U));

    // After a replaced file's entry set: its old cluster is still allocated.
    Fresh(device);
    const bool replaced = (MountOn(device) == ExfatStatus::Done) &&
                          (CreateName("crash", object) == ExfatStatus::Done) &&
                          (WriteSeed(object, 0U, 31U) == ExfatStatus::Done) &&
                          (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done) &&
                          (WriteSeed(object, 0U, 33U) == ExfatStatus::Done) &&
                          (g_volume.Commit(ExfatCrashPoint::AfterDirectoryEntry) == ExfatStatus::Crashed);
    report = CheckExfatVolume(device);
    ASTERIA_EXPECT(context, "a crash after a replaced file's entry set leaks its old clusters", replaced,
                   report.Errors == 0U, report.LostClusters == 1U, report.Dirty, MountOn(device) == ExfatStatus::Done,
                   OpenName("crash", object) == ExfatStatus::Done, PageHolds(object, 0U, 33U, 4096U));

    // A device failure breaks the volume until the next mount.
    Fresh(device);
    ExfatObject kept{};
    const bool mounted = (MountOn(device) == ExfatStatus::Done) && (CreateName("kept", kept) == ExfatStatus::Done) &&
                         (WriteSeed(kept, 0U, 41U) == ExfatStatus::Done) &&
                         (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done) &&
                         (CreateName("lost", object) == ExfatStatus::Done);
    device.Arm(0U);
    const bool failed = (WriteSeed(object, 0U, 1U) == ExfatStatus::Failed) &&
                        (g_volume.Stat(object, g_size) == ExfatStatus::Failed) &&
                        (g_volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Failed) && !g_volume.IsMounted();
    device.Arm(NoBudget);
    const bool remounted = (MountOn(device) == ExfatStatus::Done) && !g_volume.IsReadOnly() &&
                           (OpenName("lost", object) == ExfatStatus::NotFound) &&
                           (OpenName("kept", kept) == ExfatStatus::Done);
    device.SetFailReads(true);
    const bool readFailed = g_volume.ReadPage(kept, 0U, g_page) == ExfatStatus::Failed;
    device.SetFailReads(false);
    ASTERIA_EXPECT(context, "a device failure answers Failed until the next mount", mounted, failed, remounted,
                   readFailed, OpenName("kept", kept) == ExfatStatus::Failed, MountOn(device) == ExfatStatus::Done,
                   OpenName("kept", kept) == ExfatStatus::Done, PageHolds(kept, 0U, 41U, 4096U));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    namespace Test = Asteria::Test;
    namespace Exfat = Asteria::Test::Exfat;
    const char* const directory = std::getenv(Exfat::ImageVariable);
    if (directory == nullptr)
    {
        static_cast<void>(std::printf("SKIP: ASTERIA_EXFAT_IMAGE_DIR is not set; CTest sets it\n"));
        return 0;
    }
    if (!Exfat::LoadImage(Test::g_base4k, Exfat::PathOf(directory, "exfat-4k.img")) ||
        !Exfat::LoadImage(Test::g_base512, Exfat::PathOf(directory, "exfat-512.img")))
    {
        static_cast<void>(std::printf("SKIP: no tool-made exFAT images in %s (exfatprogs absent)\n", directory));
        return 0;
    }
    TestContext context{"ExfatVolumeTests"};
    Test::Fresh(Test::g_device4k);
    Test::Fresh(Test::g_device512);
    const bool parsed = Exfat::ParseLayout(Test::g_device4k, Test::g_layout4k) &&
                        Exfat::ParseLayout(Test::g_device512, Test::g_layout512);
    ASTERIA_EXPECT(context, "the tool-made images decode", parsed, Test::g_layout4k.ClusterShift == 3U,
                   Test::g_layout512.ClusterShift == 0U, Test::g_layout512.RootCluster == 45U);
    Test::TestMount(context);
    Test::TestForeignSets(context);
    Test::TestCreate(context);
    Test::TestWrite(context);
    Test::TestCrashes(context);
    return context.Finish();
}
