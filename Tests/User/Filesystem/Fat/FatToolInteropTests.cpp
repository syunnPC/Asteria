// SPDX-License-Identifier: Apache-2.0
// The FAT volume library against images made by dosfstools and mtools. CTest runs FatToolImages.py
// make before this test and FatToolImages.py check after it, with ASTERIA_FAT_IMAGE_DIR naming
// their directory: - make leaves fat12.img, fat16.img and fat32.img ( disks: the token at sector 8,
// README.TXT and a-long-lower-name.txt copied in with mcopy) and the two copied files; - this test
// reads the files through the library, then for each image runs the crash sweep's workload once to
// completion, keeping clean-<type>.img and the bytes mtype must return
// (clean-<type>.<file>.expected), and once per crash point, keeping crash-<type>-<budget>.img and a
// line of crash-<type>.txt ("<budget> <commits> <FATs may differ>"); - check runs fsck.fat -n and
// mtype on what this test kept. Without the variable or the images (the tools are absent) the test
// prints SKIP and CTest reports it skipped.

#include "User/Filesystem/Fat/FatVolume.hpp"

#include <cstdio>
#include <cstdlib>
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
using Fat::NoBudget;
using Fat::SectorBytes;
using Fat::SparseDisk;
using Fat::SparseStorage;
using Fat::TestDevice;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::FatFileSize;
using User::Filesystem::FatObject;
using User::Filesystem::FatPageBytes;
using User::Filesystem::FatStatus;
using User::Filesystem::FatType;
using User::Filesystem::FatVolume;

SparseStorage<16U> g_baseStorage;
SparseStorage<12U> g_overlayStorage;
SparseDisk g_base{g_baseStorage.Keys, g_baseStorage.Data, 16U};
SparseDisk g_overlay{g_overlayStorage.Keys, g_overlayStorage.Data, 12U};
TestDevice g_device{g_base, &g_overlay};
FatVolume g_volume;
UInt8 g_chunk[1U << 16U];
UInt8 g_page[FatPageBytes];
UInt8 g_readme[FatPageBytes];
UInt8 g_long[2U * FatPageBytes];
char g_path[1024U];
const char* g_directory{nullptr};

constexpr Size ReadmeBytes{13U};
constexpr Size LongBytes{6000U};

[[nodiscard]] const char* PathOf(const char* const name) noexcept
{
    const int written = std::snprintf(g_path, sizeof(g_path), "%s/%s", g_directory, name);
    return ((written > 0) && (static_cast<Size>(written) < sizeof(g_path))) ? g_path : "";
}

/// Reads a whole file of at most capacity bytes; its size, or ~0 if absent.
[[nodiscard]] Size ReadFile(const char* const name, UInt8* const buffer, const Size capacity) noexcept
{
    std::FILE* const file = std::fopen(PathOf(name), "rb");
    if (file == nullptr)
    {
        return ~Size{0U};
    }
    const Size size = std::fread(buffer, 1U, capacity, file);
    static_cast<void>(std::fclose(file));
    return size;
}

/// Loads an image's nonzero sectors into the base disk.
[[nodiscard]] bool LoadImage(const char* const name) noexcept
{
    std::FILE* const file = std::fopen(PathOf(name), "rb");
    if (file == nullptr)
    {
        return false;
    }
    g_base.Reset(~UInt64{0U});
    UInt64 sectors{0U};
    bool done{true};
    for (Size read = std::fread(g_chunk, 1U, sizeof(g_chunk), file); done && (read != 0U);
         read = std::fread(g_chunk, 1U, sizeof(g_chunk), file))
    {
        for (Size at{0U}; done && ((at + SectorBytes) <= read); at += SectorBytes, ++sectors)
        {
            bool zero{true};
            for (Size index{0U}; zero && (index < SectorBytes); ++index)
            {
                zero = g_chunk[at + index] == 0U;
            }
            done = zero || g_base.Put(sectors, g_chunk + at);
        }
        done = done && ((read % SectorBytes) == 0U);
    }
    static_cast<void>(std::fclose(file));
    g_base.SetSectors(sectors);
    return done && (sectors != 0U);
}

/// Writes base under overlay as a sparse image of the disk's size.
[[nodiscard]] bool SaveImage(const char* const name) noexcept
{
    std::FILE* const file = std::fopen(PathOf(name), "wb");
    if (file == nullptr)
    {
        return false;
    }
    const UInt8 zero{0U};
    bool done = (std::fseek(file, static_cast<long>((g_base.GetSectors() * SectorBytes) - 1U), SEEK_SET) == 0) &&
                (std::fwrite(&zero, 1U, 1U, file) == 1U);
    for (Size pass{0U}; pass < 2U; ++pass)
    {
        const SparseDisk& disk = (pass == 0U) ? g_base : g_overlay;
        for (Size slot{0U}; done && (slot < disk.GetCapacity()); ++slot)
        {
            const UInt64 lba = disk.KeyAt(slot);
            if ((lba == SparseDisk::Empty) || ((pass == 0U) && (g_overlay.Find(lba) != nullptr)))
            {
                continue;
            }
            done = (std::fseek(file, static_cast<long>(lba * SectorBytes), SEEK_SET) == 0) &&
                   (std::fwrite(disk.DataAt(slot), 1U, SectorBytes, file) == SectorBytes);
        }
    }
    return (std::fclose(file) == 0) && done;
}

[[nodiscard]] bool SaveBytes(const char* const name, const UInt8* const bytes, const Size size) noexcept
{
    std::FILE* const file = std::fopen(PathOf(name), "wb");
    if (file == nullptr)
    {
        return false;
    }
    const bool written = std::fwrite(bytes, 1U, size, file) == size;
    return (std::fclose(file) == 0) && written;
}

/// The workload's files as mtype must return them after its last commit.
[[nodiscard]] bool SaveExpected(const char* const type) noexcept
{
    static UInt8 bytes[3U * FatPageBytes];
    char name[96U]{};
    bool done{true};
    for (Size file{0U}; done && (file < 3U); ++file)
    {
        const Fat::ExpectedFile want = Fat::ExpectFile(file, 4U);
        for (Size page{0U}; page < (want.Size / FatPageBytes); ++page)
        {
            Fat::FillPage(bytes + (page * FatPageBytes), want.Seeds[page]);
        }
        const int written = std::snprintf(name, sizeof(name), "clean-%s.%s.expected", type, want.Name);
        done = (written > 0) && SaveBytes(name, bytes, want.Size);
    }
    return done;
}

/// Reads a tool-made file through the library: its size and bytes, zeros
/// past them, and the same file under its name in uppercase.
[[nodiscard]] bool ReadsFile(const char* const name, const char* const upper, const UInt8* const bytes,
                             const Size size) noexcept
{
    FatObject object{};
    FatObject same{};
    FatFileSize stat{};
    bool equal = (g_volume.Open(Bytes(name), std::strlen(name), object) == FatStatus::Done) &&
                 (g_volume.Open(Bytes(upper), std::strlen(upper), same) == FatStatus::Done) &&
                 (same.Identifier == object.Identifier) && (g_volume.Stat(object, stat) == FatStatus::Done) &&
                 (stat.Bytes == size) && (stat.Pages == ((size + FatPageBytes - 1U) / FatPageBytes));
    for (UInt64 page{0U}; equal && (page <= stat.Pages); ++page)
    {
        const Size start = page * FatPageBytes;
        const Size valid = (start < size) ? (((size - start) < FatPageBytes) ? (size - start) : FatPageBytes) : 0U;
        equal = (g_volume.ReadPage(object, page, g_page) == FatStatus::Done) &&
                (std::memcmp(g_page, bytes + start, valid) == 0);
        for (Size index{valid}; equal && (index < FatPageBytes); ++index)
        {
            equal = g_page[index] == 0U;
        }
    }
    return equal;
}

std::FILE* g_manifest{nullptr};
const char* g_type{""};
Size g_kept{0U};

void Keep(const Fat::CrashPoint& point) noexcept
{
    char name[96U]{};
    const int written =
        std::snprintf(name, sizeof(name), "crash-%s-%03llu.img", g_type, static_cast<unsigned long long>(point.Budget));
    if ((written > 0) && SaveImage(name) && (g_manifest != nullptr))
    {
        static_cast<void>(std::fprintf(g_manifest, "%llu %zu %d\n", static_cast<unsigned long long>(point.Budget),
                                       point.Commits, point.FatsMayDiffer ? 1 : 0));
        ++g_kept;
    }
}

void TestImage(TestContext& context, const char* const type, const FatType expected) noexcept
{
    char name[64U]{};
    static_cast<void>(std::snprintf(name, sizeof(name), "%s.img", type));
    const bool loaded = LoadImage(name);
    g_overlay.Reset(g_base.GetSectors());
    g_device.Arm(NoBudget);
    const CheckReport golden = CheckVolume(g_device);
    FatObject object{};
    ASTERIA_EXPECT(context, "a tool-made image mounts with its type and passes the invariant checker", loaded,
                   g_volume.Mount(g_device.Get(), 1U) == FatStatus::Done, !g_volume.IsReadOnly(),
                   g_volume.GetGeometry().Type == expected, golden.IsClean(), golden.Files == 2U,
                   ReadsFile("readme.txt", "README.TXT", g_readme, ReadmeBytes),
                   ReadsFile("a-long-lower-name.txt", "A-Long-Lower-Name.TXT", g_long, LongBytes),
                   g_volume.Open(Bytes("a-long~1.txt"), 12U, object) == FatStatus::NotFound);

    // The workload to completion, kept for fsck.fat and mtype.
    const bool clean = g_volume.Mount(g_device.Get(), 2U) == FatStatus::Done;
    const bool ran = clean && Fat::RunWorkload(g_volume, true);
    const CheckReport report = CheckVolume(g_device);
    static_cast<void>(std::snprintf(name, sizeof(name), "clean-%s.img", type));
    ASTERIA_EXPECT(context, "the workload on a tool-made image leaves a clean volume", clean, ran, report.IsClean(),
                   report.Files == 4U, SaveImage(name), SaveExpected(type),
                   Fat::VerifyFiles(g_volume, 4U, g_readme, ReadmeBytes, type),
                   ReadsFile("a-long-lower-name.txt", "a-long-lower-name.txt", g_long, LongBytes));

    // The crash sweep on the same image, every crashed disk kept.
    static_cast<void>(std::snprintf(name, sizeof(name), "crash-%s.txt", type));
    g_manifest = std::fopen(PathOf(name), "w");
    g_type = type;
    g_kept = 0U;
    char label[64U]{};
    static_cast<void>(std::snprintf(label, sizeof(label), "%s crash sweep on a tool-made image", type));
    Fat::RunCrashSweep(context, g_device, g_overlay, label, g_readme, ReadmeBytes, Keep);
    const bool closed = (g_manifest != nullptr) && (std::fclose(g_manifest) == 0);
    g_manifest = nullptr;
    ASTERIA_EXPECT(context, "every crashed image is kept for fsck.fat", closed, g_kept > 50U);
}

// Tool-made trees, UTF-16 edge conversion, and an independent fsck judgment after every sector
// write of directory/nested create.
void TestTree(TestContext& context, const char* type) noexcept
{
    char image[128U]{};
    static_cast<void>(std::snprintf(image, sizeof(image), "%s.img", type));
    const bool loaded = LoadImage(image);
    constexpr char child[]{"日本-😀"};
    constexpr char file[]{"résumé-😀.txt"};
    for (Size stage{0U}; stage < 4U; ++stage)
    {
        UInt64 writes{0U};
        for (UInt64 budget{0U}; budget < 512U; ++budget)
        {
            g_overlay.Reset(g_base.GetSectors());
            g_device.Arm(NoBudget);
            const bool mounted = g_volume.Mount(g_device.Get(), 40U) == FatStatus::Done;
            FatObject apps{};
            FatObject nested{};
            FatObject object{};
            bool prepared = mounted;
            if (stage != 0U)
            {
                prepared = prepared &&
                           g_volume.CreateIn(g_volume.Root(), Bytes("apps"), 4U, apps, true) == FatStatus::Done &&
                           g_volume.Commit(User::Filesystem::FatCrashPoint::None) == FatStatus::Done;
            }
            if (stage == 3U)
            {
                // Fill the first cluster so nested Create has to append a
                // freshly zeroed cluster before publishing the entry set.
                const UInt32 entries = (16U << g_volume.GetGeometry().ClusterShift) - 2U;
                for (UInt32 index{0U}; prepared && index < entries; ++index)
                {
                    char name[16U]{};
                    static_cast<void>(std::snprintf(name, sizeof(name), "f%u", index));
                    FatObject filler{};
                    prepared = g_volume.CreateIn(apps, Bytes(name), std::strlen(name), filler) == FatStatus::Done &&
                               g_volume.Commit(User::Filesystem::FatCrashPoint::None) == FatStatus::Done;
                }
            }
            g_device.Arm(stage == 2U ? NoBudget : budget);
            bool ran{prepared};
            if (stage == 0U)
            {
                ran = ran && g_volume.CreateIn(g_volume.Root(), Bytes("apps"), 4U, apps, true) == FatStatus::Done &&
                      g_volume.Commit(User::Filesystem::FatCrashPoint::None) == FatStatus::Done;
            }
            else
            {
                if (stage == 2U)
                {
                    ran = ran &&
                          g_volume.CreateIn(apps, Bytes(child), sizeof(child) - 1U, nested, true) == FatStatus::Done &&
                          g_volume.Commit(User::Filesystem::FatCrashPoint::None) == FatStatus::Done;
                    apps = nested;
                }
                ran = ran && g_volume.CreateIn(apps, Bytes(file), sizeof(file) - 1U, object) == FatStatus::Done;
                for (UInt64 page{0U}; ran && page < 8U; ++page)
                {
                    Fat::FillPage(g_page, static_cast<UInt32>(100U + page));
                    ran = g_volume.WritePage(object, page, g_page) == FatStatus::Done;
                }
                ran = ran && g_volume.Commit(User::Filesystem::FatCrashPoint::None) == FatStatus::Done;
            }
            if (stage == 2U)
            {
                FatFileSize size{};
                FatObject opened{};
                bool read = ran && g_volume.Lookup(apps, Bytes(file), sizeof(file) - 1U, opened) == FatStatus::Done &&
                            g_volume.Stat(opened, size) == FatStatus::Done && size.Bytes == 8U * FatPageBytes;
                for (UInt64 page{0U}; read && page < 8U; ++page)
                {
                    static UInt8 expected[FatPageBytes];
                    Fat::FillPage(expected, static_cast<UInt32>(100U + page));
                    read = g_volume.ReadPage(opened, page, g_page) == FatStatus::Done &&
                           std::memcmp(g_page, expected, sizeof(expected)) == 0;
                }
                static_cast<void>(std::snprintf(image, sizeof(image), "tree-%s.img", type));
                ASTERIA_EXPECT(context, "nested UTF-8 names and eight-page files survive clean publication", loaded,
                               read, SaveImage(image));
                break;
            }
            static_cast<void>(std::snprintf(image, sizeof(image), "tree-crash-%s-%zu-%03llu.img", type, stage,
                                            static_cast<unsigned long long>(budget)));
            const bool saved = SaveImage(image);
            g_device.Arm(NoBudget);
            const bool remounted = g_volume.Mount(g_device.Get(), 41U) == FatStatus::Done;
            FatFileSize size{};
            FatObject found{};
            if (stage != 0U)
            {
                static_cast<void>(g_volume.Lookup(g_volume.Root(), Bytes("apps"), 4U, apps));
            }
            const auto discovered = stage == 0U ? g_volume.Lookup(g_volume.Root(), Bytes("apps"), 4U, found)
                                                : g_volume.Lookup(apps, Bytes(file), sizeof(file) - 1U, found);
            const bool valid =
                discovered == FatStatus::NotFound ||
                (discovered == FatStatus::Done && g_volume.Stat(found, size) == FatStatus::Done &&
                 (stage == 0U ? size.Kind == User::Filesystem::NodeKind::Directory : size.Bytes == 8U * FatPageBytes));
            if (!prepared || !saved || !remounted || !valid)
            {
                ASTERIA_EXPECT(context, "every sector crash leaves absent or fully initialized entries", false);
                break;
            }
            ++writes;
            if (ran)
            {
                break;
            }
        }
        if (stage != 2U)
        {
            static_cast<void>(std::printf("%s tree stage %zu: %llu sector crash points checked\n", type, stage,
                                          static_cast<unsigned long long>(writes)));
            ASTERIA_EXPECT(context, "directory, nested create and directory-growth sweeps reach complete commits",
                           writes > 2U, writes < 512U);
        }
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    namespace Test = Asteria::Test;
    using Asteria::User::Filesystem::FatType;
    Test::g_directory = std::getenv("ASTERIA_FAT_IMAGE_DIR");
    if (Test::g_directory == nullptr)
    {
        static_cast<void>(std::printf("SKIP: ASTERIA_FAT_IMAGE_DIR is not set; CTest sets it\n"));
        return 0;
    }
    if ((Test::ReadFile("README.TXT", Test::g_readme, sizeof(Test::g_readme)) != Test::ReadmeBytes) ||
        (Test::ReadFile("a-long-lower-name.txt", Test::g_long, sizeof(Test::g_long)) != Test::LongBytes))
    {
        static_cast<void>(
            std::printf("SKIP: no tool-made FAT images in %s (dosfstools or mtools absent)\n", Test::g_directory));
        return 0;
    }
    TestContext context{"FatToolInteropTests"};
    Test::TestImage(context, "fat12", FatType::Fat12);
    Test::TestImage(context, "fat16", FatType::Fat16);
    Test::TestImage(context, "fat32", FatType::Fat32);
    Test::TestTree(context, "fat12");
    Test::TestTree(context, "fat16");
    Test::TestTree(context, "fat32");
    return context.Finish();
}
