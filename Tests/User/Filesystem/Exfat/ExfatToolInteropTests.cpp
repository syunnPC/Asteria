// SPDX-License-Identifier: Apache-2.0
// The exFAT volume library against images made by mkfs.exfat. CTest runs ExfatToolImages.py make
// before this test and ExfatToolImages.py check after it, with ASTERIA_EXFAT_IMAGE_DIR naming their
// directory: - make leaves exfat-4k.img and exfat-512.img (64 MiB, -c 4K and -c 512, the token in
// sectors 8 and 20, both boot checksums recomputed); - this test places files with the test writer
// (README.TXT FAT-chained; on the 4 KiB image also a long name chained over clusters three apart),
// reads them through the library, then for each image runs the crash sweep's workload once to
// completion, keeping clean-<variant>.img and every file's bytes (clean-<variant>.<name>.expected),
// and once per crash point, keeping crash-<variant>-<budget>.img; - check runs fsck.exfat -n on
// everything kept and reads the clean images' files with its own decoder. Without the variable or
// the images (the tools are absent) the test prints SKIP and CTest reports it skipped.

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Filesystem/Exfat/ExfatTestSupport.hpp"
#include "User/Filesystem/Exfat/ExfatWorkload.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Exfat::Bytes;
using Exfat::CheckReport;
using Exfat::CheckExfatVolume;
using Exfat::FileSpec;
using Exfat::Layout;
using Exfat::NoBudget;
using Exfat::SparseDisk;
using Exfat::SparseStorage;
using Exfat::TestDevice;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
using User::Filesystem::ExfatFileSize;
using User::Filesystem::ExfatObject;
using User::Filesystem::ExfatPageBytes;
using User::Filesystem::ExfatStatus;
using User::Filesystem::ExfatVolume;

SparseStorage<13U> g_baseStorage;
SparseStorage<12U> g_overlayStorage;
SparseDisk g_base{g_baseStorage.Keys, g_baseStorage.Data, 13U};
SparseDisk g_overlay{g_overlayStorage.Keys, g_overlayStorage.Data, 12U};
TestDevice g_writer{g_base};
TestDevice g_device{g_base, &g_overlay};
ExfatVolume g_volume;
UInt8 g_page[ExfatPageBytes];
UInt8 g_readme[2U * ExfatPageBytes];
UInt8 g_long[2U * ExfatPageBytes];
const char* g_directory{nullptr};

constexpr char LongLowerName[]{"a-long-lower-name.txt"};
constexpr Size LongBytes{6000U};

/// An image and the files the writer places on it.
struct Variant
{
    const char* Image;
    const char* Label;
    UInt32 ReadmeBytes;
    UInt32 ReadmeFirst;
    UInt32 ReadmeStride;
    bool Long;
};

constexpr Variant Variants[2U]{
    {.Image = "exfat-4k",
     .Label = "exfat-4k",
     .ReadmeBytes = 13U,
     .ReadmeFirst = 10U,
     .ReadmeStride = 1U,
     .Long = true},
    {.Image = "exfat-512",
     .Label = "exfat-512",
     .ReadmeBytes = 1300U,
     .ReadmeFirst = 100U,
     .ReadmeStride = 2U,
     .Long = false},
};

/// Reads a file through the library: its size and bytes, zeros past them,
/// and the same file under its name in uppercase.
[[nodiscard]] bool ReadsFile(const char* const name, const char* const upper, const UInt8* const bytes,
                             const Size size) noexcept
{
    ExfatObject object{};
    ExfatObject same{};
    ExfatFileSize stat{};
    bool equal = (g_volume.Open(Bytes(name), std::strlen(name), object) == ExfatStatus::Done) &&
                 (g_volume.Open(Bytes(upper), std::strlen(upper), same) == ExfatStatus::Done) &&
                 (same.Identifier == object.Identifier) && (g_volume.Stat(object, stat) == ExfatStatus::Done) &&
                 (stat.Bytes == size) && (stat.Pages == ((size + ExfatPageBytes - 1U) / ExfatPageBytes));
    for (UInt64 page{0U}; equal && (page <= stat.Pages); ++page)
    {
        const Size start = page * ExfatPageBytes;
        const Size valid = (start < size) ? (((size - start) < ExfatPageBytes) ? (size - start) : ExfatPageBytes) : 0U;
        equal = (g_volume.ReadPage(object, page, g_page) == ExfatStatus::Done) &&
                (std::memcmp(g_page, bytes + start, valid) == 0);
        for (Size index{valid}; equal && (index < ExfatPageBytes); ++index)
        {
            equal = g_page[index] == 0U;
        }
    }
    return equal;
}

/// Every file of the clean image as the check's decoder must read it.
[[nodiscard]] bool SaveExpected(const Variant& variant) noexcept
{
    static UInt8 bytes[3U * ExfatPageBytes];
    char name[128U]{};
    bool done{true};
    for (Size file{0U}; done && (file < 3U); ++file)
    {
        const Exfat::ExpectedFile want = Exfat::ExpectFile(file, 4U);
        for (Size page{0U}; page < (want.Size / ExfatPageBytes); ++page)
        {
            Exfat::FillPage(bytes + (page * ExfatPageBytes), want.Seeds[page]);
        }
        const int written = std::snprintf(name, sizeof(name), "clean-%s.%s.expected", variant.Image, want.Name);
        done = (written > 0) && Exfat::SaveBytes(Exfat::PathOf(g_directory, name), bytes, want.Size);
    }
    const int written = std::snprintf(name, sizeof(name), "clean-%s.%s.expected", variant.Image, LongLowerName);
    return done &&
           (!variant.Long || ((written > 0) && Exfat::SaveBytes(Exfat::PathOf(g_directory, name), g_long, LongBytes)));
}

const Variant* g_variant{nullptr};
Size g_kept{0U};

void Keep(const Exfat::CrashPoint& point) noexcept
{
    char name[96U]{};
    const int written = std::snprintf(name, sizeof(name), "crash-%s-%03llu.img", g_variant->Image,
                                      static_cast<unsigned long long>(point.Budget));
    if ((written > 0) && Exfat::SaveImage(g_base, g_overlay, Exfat::PathOf(g_directory, name)))
    {
        ++g_kept;
    }
}

void TestImage(TestContext& context, const Variant& variant) noexcept
{
    char name[96U]{};
    static_cast<void>(std::snprintf(name, sizeof(name), "%s.img", variant.Image));
    const bool loaded = Exfat::LoadImage(g_base, Exfat::PathOf(g_directory, name));
    g_overlay.Reset(g_base.GetSectors());
    g_writer.Arm(NoBudget);
    g_device.Arm(NoBudget);
    Layout layout{};
    const bool parsed = loaded && Exfat::ParseLayout(g_writer, layout);
    const CheckReport made = CheckExfatVolume(g_writer);
    for (Size at{0U}; at < variant.ReadmeBytes; ++at)
    {
        g_readme[at] = static_cast<UInt8>("hello readme\n"[at % 13U]);
    }
    for (Size at{0U}; at < LongBytes; ++at)
    {
        g_long[at] = static_cast<UInt8>((at * 7U) + 3U);
    }
    // README.TXT (FAT-chained) at entries 4 to 6, after mkfs's label, its
    // cleared entry, the bitmap and the up-case table; the long name at 7.
    const bool placed = parsed &&
                        Exfat::PlaceFile(g_writer, layout,
                                         FileSpec{.Name = "README.TXT",
                                                  .Index = 4U,
                                                  .First = variant.ReadmeFirst,
                                                  .Stride = variant.ReadmeStride,
                                                  .Size = variant.ReadmeBytes,
                                                  .Valid = variant.ReadmeBytes,
                                                  .Data = g_readme,
                                                  .Seed = 0U,
                                                  .Attributes = 0x20U,
                                                  .Benign = 0U,
                                                  .ChecksumDelta = 0U,
                                                  .HashDelta = 0U}) &&
                        (!variant.Long || Exfat::PlaceFile(g_writer, layout,
                                                           FileSpec{.Name = LongLowerName,
                                                                    .Index = 7U,
                                                                    .First = 20U,
                                                                    .Stride = 3U,
                                                                    .Size = LongBytes,
                                                                    .Valid = LongBytes,
                                                                    .Data = g_long,
                                                                    .Seed = 0U,
                                                                    .Attributes = 0x20U,
                                                                    .Benign = 0U,
                                                                    .ChecksumDelta = 0U,
                                                                    .HashDelta = 0U}));
    const CheckReport golden = CheckExfatVolume(g_writer);
    ExfatObject object{};
    ASTERIA_EXPECT(
        context, "a tool-made image passes the checker and mounts; the writer's files read back", loaded, parsed,
        made.IsClean(), made.Files == 0U, placed, golden.IsClean(), golden.Files == (variant.Long ? 2U : 1U),
        g_volume.Mount(Exfat::DeviceOf(g_device), 1U) == ExfatStatus::Done, !g_volume.IsReadOnly(),
        g_volume.GetGeometry().ClusterShift == layout.ClusterShift, g_volume.GetGeometry().Clusters == layout.Clusters,
        ReadsFile("readme.txt", "README.TXT", g_readme, variant.ReadmeBytes),
        !variant.Long || ReadsFile(LongLowerName, "A-Long-Lower-Name.TXT", g_long, LongBytes),
        g_volume.Open(Bytes("readme.tx"), 9U, object) == ExfatStatus::NotFound);

    // The workload to completion, kept for fsck.exfat and the decoder.
    const bool mounted = g_volume.Mount(Exfat::DeviceOf(g_device), 2U) == ExfatStatus::Done;
    const bool ran = mounted && Exfat::RunWorkload(g_volume, true);
    const CheckReport report = CheckExfatVolume(g_device);
    static_cast<void>(std::snprintf(name, sizeof(name), "clean-%s.img", variant.Image));
    if (!report.IsClean())
    {
        Exfat::PrintReport(variant.Label, report);
    }
    ASTERIA_EXPECT(context, "the workload on a tool-made image leaves a clean volume", mounted, ran, report.IsClean(),
                   report.Files == (variant.Long ? 4U : 3U),
                   Exfat::SaveImage(g_base, g_overlay, Exfat::PathOf(g_directory, name)), SaveExpected(variant),
                   Exfat::VerifyFiles(g_volume, 4U, g_readme, variant.ReadmeBytes, variant.Label),
                   !variant.Long || ReadsFile(LongLowerName, LongLowerName, g_long, LongBytes));

    // The crash sweep on the same image, every crashed disk kept.
    g_variant = &variant;
    g_kept = 0U;
    char label[64U]{};
    static_cast<void>(std::snprintf(label, sizeof(label), "%s crash sweep", variant.Label));
    Exfat::RunCrashSweep(context, g_device, g_overlay, label, g_readme, variant.ReadmeBytes, Keep);
    ASTERIA_EXPECT(context, "every crashed image is kept for fsck.exfat", g_kept > 50U);
}

// Tool-made trees, UTF-16 edge conversion, and an independent fsck judgment after every sector
// write of directory/nested create.
void TestTree(TestContext& context, const char* type) noexcept
{
    char image[128U]{};
    static_cast<void>(std::snprintf(image, sizeof(image), "%s.img", type));
    const bool loaded = Exfat::LoadImage(g_base, Exfat::PathOf(g_directory, image));
    constexpr char child[]{"日本-😀"};
    constexpr char file[]{"résumé-😀.txt"};
    for (Size stage{0U}; stage < 3U; ++stage)
    {
        UInt64 writes{0U};
        for (UInt64 budget{0U}; budget < 512U; ++budget)
        {
            g_overlay.Reset(g_base.GetSectors());
            g_device.Arm(NoBudget);
            const bool mounted = g_volume.Mount(Exfat::DeviceOf(g_device), 40U) == ExfatStatus::Done;
            ExfatObject apps{};
            ExfatObject nested{};
            ExfatObject object{};
            bool prepared = mounted;
            if (stage != 0U)
            {
                prepared = prepared &&
                           g_volume.CreateIn(g_volume.Root(), Bytes("apps"), 4U, apps, true) == ExfatStatus::Done &&
                           g_volume.Commit(User::Filesystem::ExfatCrashPoint::None) == ExfatStatus::Done;
            }
            g_device.Arm(stage == 2U ? NoBudget : budget);
            bool ran{prepared};
            if (stage == 0U)
            {
                ran = ran && g_volume.CreateIn(g_volume.Root(), Bytes("apps"), 4U, apps, true) == ExfatStatus::Done &&
                      g_volume.Commit(User::Filesystem::ExfatCrashPoint::None) == ExfatStatus::Done;
            }
            else
            {
                if (stage == 2U)
                {
                    ran =
                        ran &&
                        g_volume.CreateIn(apps, Bytes(child), sizeof(child) - 1U, nested, true) == ExfatStatus::Done &&
                        g_volume.Commit(User::Filesystem::ExfatCrashPoint::None) == ExfatStatus::Done;
                    apps = nested;
                }
                ran = ran && g_volume.CreateIn(apps, Bytes(file), sizeof(file) - 1U, object) == ExfatStatus::Done;
                for (UInt64 page{0U}; ran && page < 8U; ++page)
                {
                    Exfat::FillPage(g_page, static_cast<UInt32>(100U + page));
                    ran = g_volume.WritePage(object, page, g_page) == ExfatStatus::Done;
                }
                ran = ran && g_volume.Commit(User::Filesystem::ExfatCrashPoint::None) == ExfatStatus::Done;
            }
            if (stage == 2U)
            {
                ExfatFileSize size{};
                ExfatObject opened{};
                bool read = ran && g_volume.Lookup(apps, Bytes(file), sizeof(file) - 1U, opened) == ExfatStatus::Done &&
                            g_volume.Stat(opened, size) == ExfatStatus::Done && size.Bytes == 8U * ExfatPageBytes;
                for (UInt64 page{0U}; read && page < 8U; ++page)
                {
                    static UInt8 expected[ExfatPageBytes];
                    Exfat::FillPage(expected, static_cast<UInt32>(100U + page));
                    read = g_volume.ReadPage(opened, page, g_page) == ExfatStatus::Done &&
                           std::memcmp(g_page, expected, sizeof(expected)) == 0;
                }
                constexpr char folded[]{"rÉsumÉ-😀.txt"};
                ExfatObject foldedObject{};
                read = read &&
                       g_volume.Lookup(apps, Bytes(folded), sizeof(folded) - 1U, foldedObject) == ExfatStatus::Done &&
                       foldedObject.Identifier == opened.Identifier;
                static_cast<void>(std::snprintf(image, sizeof(image), "tree-%s.img", type));
                ASTERIA_EXPECT(
                    context,
                    "nested UTF-8 names, authenticated Unicode upcase and eight-page files survive publication", loaded,
                    read, Exfat::SaveImage(g_base, g_overlay, Exfat::PathOf(g_directory, image)));
                break;
            }
            static_cast<void>(std::snprintf(image, sizeof(image), "tree-crash-%s-%zu-%03llu.img", type, stage,
                                            static_cast<unsigned long long>(budget)));
            const bool saved = Exfat::SaveImage(g_base, g_overlay, Exfat::PathOf(g_directory, image));
            g_device.Arm(NoBudget);
            const bool remounted = g_volume.Mount(Exfat::DeviceOf(g_device), 41U) == ExfatStatus::Done;
            ExfatFileSize size{};
            ExfatObject found{};
            if (stage != 0U)
            {
                static_cast<void>(g_volume.Lookup(g_volume.Root(), Bytes("apps"), 4U, apps));
            }
            const auto discovered = stage == 0U ? g_volume.Lookup(g_volume.Root(), Bytes("apps"), 4U, found)
                                                : g_volume.Lookup(apps, Bytes(file), sizeof(file) - 1U, found);
            const bool valid = discovered == ExfatStatus::NotFound ||
                               (discovered == ExfatStatus::Done && g_volume.Stat(found, size) == ExfatStatus::Done &&
                                (stage == 0U ? size.Kind == User::Filesystem::NodeKind::Directory
                                             : size.Bytes == 8U * ExfatPageBytes));
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
            ASTERIA_EXPECT(context, "directory and nested create sweep reaches a complete commit", writes > 2U,
                           writes < 512U);
        }
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    namespace Test = Asteria::Test;
    Test::g_directory = std::getenv(Asteria::Test::Exfat::ImageVariable);
    if (Test::g_directory == nullptr)
    {
        static_cast<void>(std::printf("SKIP: ASTERIA_EXFAT_IMAGE_DIR is not set; CTest sets it\n"));
        return 0;
    }
    std::FILE* const probe = std::fopen(Test::Exfat::PathOf(Test::g_directory, "exfat-4k.img"), "rb");
    if (probe == nullptr)
    {
        static_cast<void>(
            std::printf("SKIP: no tool-made exFAT images in %s (exfatprogs absent)\n", Test::g_directory));
        return 0;
    }
    static_cast<void>(std::fclose(probe));
    TestContext context{"ExfatToolInteropTests"};
    for (const Test::Variant& variant : Test::Variants)
    {
        Test::TestImage(context, variant);
        Test::TestTree(context, variant.Image);
    }
    return context.Finish();
}
