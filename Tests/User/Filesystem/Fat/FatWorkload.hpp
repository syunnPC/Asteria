// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_FILESYSTEM_FAT_FAT_WORKLOAD_HPP_INCLUDED
#define ASTERIA_TESTS_USER_FILESYSTEM_FAT_FAT_WORKLOAD_HPP_INCLUDED

#include <cstdio>
#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Filesystem/Fat/FatTestSupport.hpp"

/// The scripted workload of the crash sweep and the sweep itself: the workload runs once to count
/// its sector writes, then once per budget of 0 to that count with the device failing after the
/// budget, and every crashed disk is mounted again and checked.
namespace Asteria::Test::Fat
{
using User::Filesystem::FatCrashPoint;
using User::Filesystem::FatFileSize;
using User::Filesystem::FatObject;
using User::Filesystem::FatPageBytes;
using User::Filesystem::FatStatus;
using User::Filesystem::FatVolume;

/// The workload's files: a short name, a long name, and a tool-made
/// README.TXT where the disk has one.
inline constexpr char HelloName[]{"hello"};
inline constexpr char LongName[]{"a-much-longer-file-name.data"};
inline constexpr char ReadmeName[]{"readme.txt"};

[[nodiscard]] inline const UInt8* Bytes(const char* const text) noexcept
{
    return reinterpret_cast<const UInt8*>(text);
}

/// Writes page index of object from seed.
[[nodiscard]] inline bool WriteSeeded(FatVolume& volume, const FatObject& object, const UInt64 index,
                                      const UInt32 seed) noexcept
{
    static UInt8 page[FatPageBytes];
    FillPage(page, seed);
    return volume.WritePage(object, index, page) == FatStatus::Done;
}

/// Four commits, each a session: create hello (pages 0, 1); create the long
/// name (page 0); append page 2 to hello and overwrite its page 0 (a
/// replaced committed file); overwrite README.TXT's page 0 when withReadme.
/// False at the first operation that does not succeed.
[[nodiscard]] inline bool RunWorkload(FatVolume& volume, const bool withReadme) noexcept
{
    FatObject hello{};
    FatObject other{};
    if ((volume.Create(Bytes(HelloName), sizeof(HelloName) - 1U, hello) != FatStatus::Done) ||
        !WriteSeeded(volume, hello, 0U, 101U) || !WriteSeeded(volume, hello, 1U, 102U) ||
        (volume.Commit(FatCrashPoint::None) != FatStatus::Done))
    {
        return false;
    }
    if ((volume.Create(Bytes(LongName), sizeof(LongName) - 1U, other) != FatStatus::Done) ||
        !WriteSeeded(volume, other, 0U, 201U) || (volume.Commit(FatCrashPoint::None) != FatStatus::Done))
    {
        return false;
    }
    if ((volume.Open(Bytes(HelloName), sizeof(HelloName) - 1U, hello) != FatStatus::Done) ||
        !WriteSeeded(volume, hello, 2U, 103U) || !WriteSeeded(volume, hello, 0U, 104U) ||
        (volume.Commit(FatCrashPoint::None) != FatStatus::Done))
    {
        return false;
    }
    if (withReadme &&
        ((volume.Open(Bytes(ReadmeName), sizeof(ReadmeName) - 1U, other) != FatStatus::Done) ||
         !WriteSeeded(volume, other, 0U, 301U) || (volume.Commit(FatCrashPoint::None) != FatStatus::Done)))
    {
        return false;
    }
    return true;
}

/// A file as the workload leaves it after commits points of no return.
struct ExpectedFile
{
    const char* Name;
    bool Present;
    UInt32 Size;
    UInt32 Seeds[3];
};

[[nodiscard]] inline ExpectedFile ExpectFile(const Size file, const Size commits) noexcept
{
    if (file == 0U)
    {
        if (commits >= 3U)
        {
            return ExpectedFile{HelloName, true, 12288U, {104U, 102U, 103U}};
        }
        return ExpectedFile{HelloName, commits >= 1U, 8192U, {101U, 102U, 0U}};
    }
    if (file == 1U)
    {
        return ExpectedFile{LongName, commits >= 2U, 4096U, {201U, 0U, 0U}};
    }
    return ExpectedFile{ReadmeName, true, 4096U, {301U, 0U, 0U}};
}

/// Whether the mounted volume holds the workload's files after commits
/// points of no return; readme is README.TXT's original content (one page at
/// most) when the disk has one. Prints the first difference.
[[nodiscard]] inline bool VerifyFiles(FatVolume& volume, const Size commits, const UInt8* const readme,
                                      const UInt32 readmeSize, const char* const label) noexcept
{
    static UInt8 page[FatPageBytes];
    static UInt8 expected[FatPageBytes];
    for (Size file{0U}; file < ((readme != nullptr) ? 3U : 2U); ++file)
    {
        const ExpectedFile want = ExpectFile(file, commits);
        const bool original = (file == 2U) && (commits < 4U);
        const UInt32 size = original ? readmeSize : want.Size;
        FatObject object{};
        const FatStatus opened = volume.Open(Bytes(want.Name), std::strlen(want.Name), object);
        if (opened != (want.Present ? FatStatus::Done : FatStatus::NotFound))
        {
            static_cast<void>(std::fprintf(stderr, "%s: %s opened %u after %zu commits\n", label, want.Name,
                                           static_cast<unsigned>(opened), commits));
            return false;
        }
        FatFileSize stat{};
        if (want.Present && ((volume.Stat(object, stat) != FatStatus::Done) || (stat.Bytes != size) ||
                             (stat.Pages != ((size + 4095U) / 4096U))))
        {
            static_cast<void>(std::fprintf(stderr, "%s: %s has %llu bytes after %zu commits\n", label, want.Name,
                                           static_cast<unsigned long long>(stat.Bytes), commits));
            return false;
        }
        for (UInt64 index{0U}; want.Present && (index <= stat.Pages) && (index < 6U); ++index)
        {
            if (original)
            {
                std::memset(expected, 0, sizeof(expected));
                std::memcpy(expected, readme, (index == 0U) ? readmeSize : 0U);
            }
            else
            {
                FillPage(expected, (index < 3U) ? want.Seeds[index] : 0U);
            }
            if ((volume.ReadPage(object, index, page) != FatStatus::Done) ||
                (std::memcmp(page, expected, sizeof(page)) != 0))
            {
                static_cast<void>(std::fprintf(stderr, "%s: %s page %llu differs after %zu commits\n", label, want.Name,
                                               static_cast<unsigned long long>(index), commits));
                return false;
            }
        }
    }
    return true;
}

/// Whether lba is a sector of the root directory.
[[nodiscard]] inline bool IsDirectorySector(const TestDevice& device, const Layout& layout, const UInt64 lba) noexcept
{
    if (layout.Type != FatType::Fat32)
    {
        return (lba >= layout.RootStart) && (lba < (layout.RootStart + layout.RootSectors));
    }
    UInt32 cluster{layout.RootCluster};
    for (Size step{0U}; (step < 4096U) && (cluster >= 2U) && (cluster <= (layout.Clusters + 1U)); ++step)
    {
        const UInt64 first = ClusterLba(layout, cluster);
        if ((lba >= first) && (lba < (first + (UInt64{1U} << layout.ClusterShift))))
        {
            return true;
        }
        cluster = ReadEntry(device, layout, 0U, cluster) & 0x0FFF'FFFFU;
    }
    return false;
}

/// One crash point's outcome, for a caller that keeps crashed disks.
struct CrashPoint
{
    UInt64 Budget;
    Size Commits;
    bool FatsMayDiffer;
    CheckReport Report;
};

/// Runs the sweep on device (Base the formatted disk, writes to overlay) and
/// calls keep(point) for every crash point. readme as VerifyFiles.
template <typename TKeep>
inline void RunCrashSweep(TestSupport::TestContext& context, TestDevice& device, SparseDisk& overlay,
                          const char* const label, const UInt8* const readme, const UInt32 readmeSize,
                          TKeep&& keep) noexcept
{
    static FatVolume volume;
    UInt8 boot[SectorBytes]{};
    Layout layout{};
    overlay.Reset(device.Get().Sectors);
    device.Arm(NoBudget);
    const bool parsed = device.Read(0U, boot) && ParseLayout(boot, layout);
    const bool full = parsed && (volume.Mount(device.Get(), 1U) == FatStatus::Done) && !volume.IsReadOnly() &&
                      RunWorkload(volume, readme != nullptr);
    const UInt64 total = device.GetWrites();
    const CheckReport clean = CheckVolume(device);
    if (!clean.IsClean())
    {
        static_cast<void>(std::fprintf(stderr,
                                       "%s: after the workload: %s (%llu), %u lost, %zu FAT sectors differ, "
                                       "FSInfo wrong %d, dirty %d\n",
                                       label, (clean.FirstError != nullptr) ? clean.FirstError : "no invariant broken",
                                       static_cast<unsigned long long>(clean.ErrorDetail), clean.LostClusters,
                                       clean.DifferingSectors, clean.FsInfoWrong ? 1 : 0, clean.Dirty ? 1 : 0));
    }
    ASTERIA_EXPECT(context, label, parsed, full, total < TestDevice::LogCapacity, clean.IsClean(),
                   clean.Files >= ((readme != nullptr) ? 3U : 2U));
    if (!full)
    {
        return;
    }
    Size failures{0U};
    for (UInt64 budget{0U}; budget <= total; ++budget)
    {
        overlay.Reset(device.Get().Sectors);
        device.Arm(budget);
        const bool mounted = volume.Mount(device.Get(), 2U) == FatStatus::Done;
        const bool completed = mounted && RunWorkload(volume, readme != nullptr);
        Size commits{0U};
        for (Size index{0U}; index < budget; ++index)
        {
            commits += IsDirectorySector(device, layout, device.LogAt(index)) ? 1U : 0U;
        }
        const UInt64 last = (budget != 0U) ? device.LogAt(budget - 1U) : 0U;
        const CheckReport report = CheckVolume(device);
        const bool differ = (report.DifferingSectors == 0U) ||
                            ((report.DifferingSectors == 1U) && (last == (layout.FatStart + report.DifferingSector)));
        const bool remounted = volume.Mount(device.Get(), 3U) == FatStatus::Done;
        const bool files = remounted && VerifyFiles(volume, commits, readme, readmeSize, label);
        const bool good = mounted && (completed == (budget == total)) && report.Parsed && (report.Errors == 0U) &&
                          differ && remounted && files && (volume.IsReadOnly() == report.Dirty);
        if (!good && (failures < 4U))
        {
            static_cast<void>(std::fprintf(
                stderr,
                "%s: crash after %llu of %llu writes: %s (%llu), %zu FAT sectors differ, dirty %d, read-only %d\n",
                label, static_cast<unsigned long long>(budget), static_cast<unsigned long long>(total),
                (report.FirstError != nullptr) ? report.FirstError : "no invariant broken",
                static_cast<unsigned long long>(report.ErrorDetail), report.DifferingSectors, report.Dirty ? 1 : 0,
                volume.IsReadOnly() ? 1 : 0));
        }
        failures += good ? 0U : 1U;
        keep(CrashPoint{
            .Budget = budget, .Commits = commits, .FatsMayDiffer = report.DifferingSectors != 0U, .Report = report});
    }
    static_cast<void>(
        std::printf("%s: %llu crash points checked\n", label, static_cast<unsigned long long>(total + 1U)));
    ASTERIA_EXPECT(context, label, failures == 0U);
}
} // Namespace Asteria::Test::Fat

#endif // ASTERIA_TESTS_USER_FILESYSTEM_FAT_FAT_WORKLOAD_HPP_INCLUDED
