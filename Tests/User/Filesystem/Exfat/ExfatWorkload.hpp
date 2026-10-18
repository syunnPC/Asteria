// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_FILESYSTEM_EXFAT_EXFAT_WORKLOAD_HPP_INCLUDED
#define ASTERIA_TESTS_USER_FILESYSTEM_EXFAT_EXFAT_WORKLOAD_HPP_INCLUDED

#include <cstdio>
#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Filesystem/Exfat/ExfatTestSupport.hpp"

/// The scripted workload of the crash sweep and the sweep itself: the workload runs once to count
/// its sector writes, then once per budget of 0 to that count with the device failing after the
/// budget, and every crashed disk is checked and mounted again.
namespace Asteria::Test::Exfat
{
using User::Filesystem::ExfatCrashPoint;
using User::Filesystem::ExfatFileSize;
using User::Filesystem::ExfatMaximumFilePages;
using User::Filesystem::ExfatObject;
using User::Filesystem::ExfatStatus;
using User::Filesystem::ExfatVolume;

/// The workload's files: a short name, a long one (two File Name entries,
/// whose set needs the gap rule on a 4 KiB-cluster root), and README.TXT,
/// which the test writer places FAT-chained before the workload.
inline constexpr char HelloName[]{"hello"};
inline constexpr char LongName[]{"a-much-longer-file-name.data"};
inline constexpr char ReadmeName[]{"readme.txt"};

/// Writes page index of object from seed.
[[nodiscard]] inline bool WriteSeeded(ExfatVolume& volume, const ExfatObject& object, const UInt64 index,
                                      const UInt32 seed) noexcept
{
    static UInt8 page[ExfatPageBytes];
    FillPage(page, seed);
    return volume.WritePage(object, index, page) == ExfatStatus::Done;
}

/// Four commits, each a session: create hello (pages 0, 1); create the long
/// name (page 0); append page 2 to hello and overwrite its page 0 (a
/// contiguous file replaced); overwrite README.TXT's page 0 (a chained file
/// replaced) when withReadme. False at the first operation that does not
/// succeed.
[[nodiscard]] inline bool RunWorkload(ExfatVolume& volume, const bool withReadme) noexcept
{
    ExfatObject hello{};
    ExfatObject other{};
    if ((volume.Create(Bytes(HelloName), sizeof(HelloName) - 1U, hello) != ExfatStatus::Done) ||
        !WriteSeeded(volume, hello, 0U, 101U) || !WriteSeeded(volume, hello, 1U, 102U) ||
        (volume.Commit(ExfatCrashPoint::None) != ExfatStatus::Done))
    {
        return false;
    }
    if ((volume.Create(Bytes(LongName), sizeof(LongName) - 1U, other) != ExfatStatus::Done) ||
        !WriteSeeded(volume, other, 0U, 201U) || (volume.Commit(ExfatCrashPoint::None) != ExfatStatus::Done))
    {
        return false;
    }
    if ((volume.Open(Bytes(HelloName), sizeof(HelloName) - 1U, hello) != ExfatStatus::Done) ||
        !WriteSeeded(volume, hello, 2U, 103U) || !WriteSeeded(volume, hello, 0U, 104U) ||
        (volume.Commit(ExfatCrashPoint::None) != ExfatStatus::Done))
    {
        return false;
    }
    return !withReadme ||
           ((volume.Open(Bytes(ReadmeName), sizeof(ReadmeName) - 1U, other) == ExfatStatus::Done) &&
            WriteSeeded(volume, other, 0U, 301U) && (volume.Commit(ExfatCrashPoint::None) == ExfatStatus::Done));
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
[[nodiscard]] inline bool VerifyFiles(ExfatVolume& volume, const Size commits, const UInt8* const readme,
                                      const UInt32 readmeSize, const char* const label) noexcept
{
    static UInt8 page[ExfatPageBytes];
    static UInt8 expected[ExfatPageBytes];
    for (Size file{0U}; file < ((readme != nullptr) ? 3U : 2U); ++file)
    {
        const ExpectedFile want = ExpectFile(file, commits);
        const bool original = (file == 2U) && (commits < 4U);
        const UInt32 size = original ? readmeSize : want.Size;
        ExfatObject object{};
        const ExfatStatus opened = volume.Open(Bytes(want.Name), std::strlen(want.Name), object);
        if (opened != (want.Present ? ExfatStatus::Done : ExfatStatus::NotFound))
        {
            static_cast<void>(std::fprintf(stderr, "%s: %s opened %u after %zu commits\n", label, want.Name,
                                           static_cast<unsigned>(opened), commits));
            return false;
        }
        ExfatFileSize stat{};
        if (want.Present && ((volume.Stat(object, stat) != ExfatStatus::Done) || (stat.Bytes != size) ||
                             (stat.Pages != ((size + 4095U) / 4096U))))
        {
            static_cast<void>(std::fprintf(stderr, "%s: %s has %llu bytes after %zu commits\n", label, want.Name,
                                           static_cast<unsigned long long>(stat.Bytes), commits));
            return false;
        }
        for (UInt64 index{0U}; want.Present && (index <= stat.Pages) && (index < ExfatMaximumFilePages); ++index)
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
            if ((volume.ReadPage(object, index, page) != ExfatStatus::Done) ||
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
    UInt32 cluster{layout.RootCluster};
    for (Size step{0U}; (step < 4096U) && InHeap(layout, cluster); ++step)
    {
        const UInt64 first = ClusterLba(layout, cluster);
        if ((lba >= first) && (lba < (first + (UInt64{1U} << layout.ClusterShift))))
        {
            return true;
        }
        cluster = ReadFat(device, layout, cluster);
    }
    return false;
}

/// One crash point's outcome, for a caller that keeps crashed disks.
struct CrashPoint
{
    UInt64 Budget;
    Size Commits;
    CheckReport Report;
};

/// Runs the sweep on device (Base the prepared disk, writes to overlay) and
/// calls keep(point) for every crash point. readme as VerifyFiles. A crashed
/// disk must hold no error the checker knows, VolumeDirty exactly when the
/// crash fell inside a session (an odd number of boot sector writes), lost
/// clusters only then and at most one reserved run of them, mount read-only
/// exactly when dirty, and hold the files of the commits whose entry set
/// was written.
template <typename TKeep>
inline void RunCrashSweep(TestSupport::TestContext& context, TestDevice& device, SparseDisk& overlay,
                          const char* const label, const UInt8* const readme, const UInt32 readmeSize,
                          TKeep&& keep) noexcept
{
    static ExfatVolume volume;
    static bool entryWrite[TestDevice::LogCapacity];
    Layout layout{};
    overlay.Reset(device.Get().Sectors);
    device.Arm(NoBudget);
    const bool parsed = ParseLayout(device, layout);
    const bool full = parsed && (volume.Mount(DeviceOf(device), 1U) == ExfatStatus::Done) && !volume.IsReadOnly() &&
                      RunWorkload(volume, readme != nullptr);
    const UInt64 total = device.GetWrites();
    // An entry set's write is the directory write the next write of which is
    // not one: a created set's gap write precedes it.
    for (UInt64 index{0U}; full && (index < total) && (index < TestDevice::LogCapacity); ++index)
    {
        const bool next = ((index + 1U) < total) && IsDirectorySector(device, layout, device.LogAt(index + 1U));
        entryWrite[index] = IsDirectorySector(device, layout, device.LogAt(index)) && !next;
    }
    const CheckReport clean = CheckExfatVolume(device);
    if (!clean.IsClean())
    {
        PrintReport(label, clean);
    }
    ASTERIA_EXPECT(context, label, parsed, full, total < TestDevice::LogCapacity, clean.IsClean(),
                   clean.Files >= ((readme != nullptr) ? 3U : 2U));
    if (!full || (total >= TestDevice::LogCapacity))
    {
        return;
    }
    const UInt32 run = static_cast<UInt32>(((ExfatMaximumFilePages * ExfatPageBytes) + ClusterBytes(layout) - 1U) /
                                           ClusterBytes(layout));
    Size failures{0U};
    for (UInt64 budget{0U}; budget <= total; ++budget)
    {
        overlay.Reset(device.Get().Sectors);
        device.Arm(budget);
        const bool mounted = volume.Mount(DeviceOf(device), 2U) == ExfatStatus::Done;
        const bool completed = mounted && RunWorkload(volume, readme != nullptr);
        Size commits{0U};
        Size bootWrites{0U};
        for (UInt64 index{0U}; index < budget; ++index)
        {
            commits += entryWrite[index] ? 1U : 0U;
            bootWrites += (device.LogAt(index) == 0U) ? 1U : 0U;
        }
        const bool dirty = (bootWrites % 2U) != 0U;
        const CheckReport report = CheckExfatVolume(device);
        const bool lost = (report.LostClusters == 0U) || (dirty && (report.LostClusters <= run));
        const bool remounted = volume.Mount(DeviceOf(device), 3U) == ExfatStatus::Done;
        const bool files = remounted && VerifyFiles(volume, commits, readme, readmeSize, label);
        const bool good = mounted && (completed == (budget == total)) && report.Parsed && (report.Errors == 0U) &&
                          lost && (report.Dirty == dirty) && remounted && files &&
                          (volume.IsReadOnly() == report.Dirty);
        if (!good && (failures < 4U))
        {
            static_cast<void>(std::fprintf(stderr, "%s: crash after %llu of %llu writes (%zu commits, dirty %d):\n",
                                           label, static_cast<unsigned long long>(budget),
                                           static_cast<unsigned long long>(total), commits, dirty ? 1 : 0));
            PrintReport(label, report);
        }
        failures += good ? 0U : 1U;
        keep(CrashPoint{.Budget = budget, .Commits = commits, .Report = report});
    }
    static_cast<void>(
        std::printf("%s: %llu crash points checked\n", label, static_cast<unsigned long long>(total + 1U)));
    ASTERIA_EXPECT(context, label, failures == 0U);
}
} // Namespace Asteria::Test::Exfat

#endif // ASTERIA_TESTS_USER_FILESYSTEM_EXFAT_EXFAT_WORKLOAD_HPP_INCLUDED
