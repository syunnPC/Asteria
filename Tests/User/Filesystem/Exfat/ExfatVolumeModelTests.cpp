// SPDX-License-Identifier: Apache-2.0
// The exFAT volume library against a reference model:
// seeded random Create, WritePage, Commit, ReadPage, Stat, Open and
// remounts on the mkfs.exfat images (4 KiB and 512-byte clusters), each with
// a contiguous filler file the test writer places over all but the heap's
// last clusters, so runs run out and the root directory fills. The model
// keeps its own allocation bitmap and directory: a session's first write
// reserves the first run of free clusters that holds six pages, a commit
// allocates the run's used prefix and frees the replaced file's clusters,
// and a create takes the first run of free entries within one sector. Every
// result is compared with the model's; every commit is followed by the
// invariant checker, which must find the volume clean but for the dirty flag
// the model expects. ASTERIA_TEST_SEED replays one seed. Without the images
// (exfatprogs absent) the test prints SKIP.

#include "User/Filesystem/Exfat/ExfatVolume.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Support/Random.hpp"
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
using Exfat::Layout;
using Exfat::NoBudget;
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

constexpr Size MaximumFiles{64U};
constexpr Size MaximumEntries{128U};
constexpr Size MaximumClusters{Size{1U} << 17U};
constexpr UInt64 MaximumBytes{6U * ExfatPageBytes};

SparseStorage<12U> g_storage[2U];
SparseStorage<13U> g_overlayStorage;
SparseDisk g_bases[2U]{{g_storage[0U].Keys, g_storage[0U].Data, 12U}, {g_storage[1U].Keys, g_storage[1U].Data, 12U}};
SparseDisk g_overlay{g_overlayStorage.Keys, g_overlayStorage.Data, 13U};
TestDevice g_writers[2U]{TestDevice{g_bases[0U]}, TestDevice{g_bases[1U]}};
TestDevice g_devices[2U]{TestDevice{g_bases[0U], &g_overlay}, TestDevice{g_bases[1U], &g_overlay}};
Layout g_layouts[2U]{};
/// The heap clusters each image's filler leaves free, at its end.
constexpr UInt32 FreeTail[2U]{40U, 200U};
ExfatVolume g_volume;
UInt8 g_page[ExfatPageBytes];
UInt8 g_expected[ExfatPageBytes];

struct ModelFile
{
    char Name[65U];
    Size Length;
    UInt32 Index;
    UInt32 First;
    UInt64 Bytes;
    UInt32 Seeds[6U];
};

[[nodiscard]] UInt64 Pages(const UInt64 bytes) noexcept
{
    return (bytes + 4095U) / 4096U;
}

[[nodiscard]] char Fold(const char value) noexcept
{
    return ((value >= 'A') && (value <= 'Z')) ? static_cast<char>(value + 32) : value;
}

class Model final
{
public:
    /// The image's state: its directory entries in use and its bitmap.
    void Start(const TestDevice& device, const Layout& layout, const ModelFile& filler) noexcept
    {
        // Field by field: a Model temporary would not fit the stack.
        m_count = 0U;
        m_session = false;
        m_create = false;
        m_reserved = false;
        m_dirty = false;
        m_readOnly = false;
        m_layout = layout;
        m_clusterBytes = UInt32{512U} << layout.ClusterShift;
        m_wanted = static_cast<UInt32>((MaximumBytes + m_clusterBytes - 1U) / m_clusterBytes);
        UInt32 rootClusters{0U};
        for (UInt32 cluster{layout.RootCluster}; Exfat::InHeap(layout, cluster) && (rootClusters < 16U); ++rootClusters)
        {
            cluster = Exfat::ReadFat(device, layout, cluster);
        }
        m_entries = rootClusters * (16U << layout.ClusterShift);
        m_entries = (m_entries > MaximumEntries) ? MaximumEntries : m_entries;
        UInt8 entry[32U]{};
        for (UInt32 index{0U}; index < m_entries; ++index)
        {
            static_cast<void>(Exfat::ReadEntry(device, layout, index, entry));
            m_used[index] = (entry[0U] & 0x80U) != 0U;
        }
        UInt8 sector[512U]{};
        for (UInt32 bit{0U}; bit < layout.Clusters; ++bit)
        {
            if ((bit % 4096U) == 0U)
            {
                static_cast<void>(
                    device.Read(Exfat::ChainLba(device, layout, layout.BitmapCluster, bit / 4096U), sector));
            }
            m_allocated[bit + 2U] = (sector[(bit / 8U) % 512U] & (1U << (bit % 8U))) != 0U;
        }
        m_files[m_count++] = filler;
    }

    [[nodiscard]] Size GetCount() const noexcept
    {
        return m_count;
    }

    [[nodiscard]] const ModelFile& FileAt(const Size index) const noexcept
    {
        return m_files[index];
    }

    [[nodiscard]] bool HasSession() const noexcept
    {
        return m_session;
    }

    [[nodiscard]] const ModelFile& Session() const noexcept
    {
        return m_pending;
    }

    [[nodiscard]] bool IsDirty() const noexcept
    {
        return m_dirty;
    }

    [[nodiscard]] bool IsReadOnly() const noexcept
    {
        return m_readOnly;
    }

    /// The file named by index (a File entry's index), the session's view
    /// for the session's file; nullptr for none.
    [[nodiscard]] const ModelFile* Find(const UInt32 index) const noexcept
    {
        if (m_session && (m_pending.Index == index))
        {
            return &m_pending;
        }
        for (Size file{0U}; file < m_count; ++file)
        {
            if (m_files[file].Index == index)
            {
                return &m_files[file];
            }
        }
        return nullptr;
    }

    [[nodiscard]] const ModelFile* FindName(const char* const name, const Size length) const noexcept
    {
        for (Size file{0U}; file < m_count; ++file)
        {
            bool same = m_files[file].Length == length;
            for (Size at{0U}; same && (at < length); ++at)
            {
                same = Fold(m_files[file].Name[at]) == Fold(name[at]);
            }
            if (same)
            {
                return &m_files[file];
            }
        }
        return nullptr;
    }

    /// Create's outcome; on Done the session's file, whose Index is the id.
    [[nodiscard]] ExfatStatus Create(const char* const name, const Size length, const bool valid) noexcept
    {
        if (m_readOnly || m_session || !valid || (FindName(name, length) != nullptr))
        {
            return ExfatStatus::Refused;
        }
        const Size needed = 2U + ((length + 14U) / 15U);
        Size start{MaximumEntries};
        Size run{0U};
        for (Size entry{0U}; (start == MaximumEntries) && (entry < m_entries); ++entry)
        {
            run = ((entry % 16U) == 0U) ? 0U : run;
            run = m_used[entry] ? 0U : (run + 1U);
            start = (run == needed) ? (entry + 1U - needed) : start;
        }
        if (start == MaximumEntries)
        {
            return ExfatStatus::Refused;
        }
        m_pending = ModelFile{};
        std::memcpy(m_pending.Name, name, length);
        m_pending.Length = length;
        m_pending.Index = static_cast<UInt32>(start);
        m_session = true;
        m_create = true;
        m_reserved = false;
        m_runStart = start;
        m_runEntries = needed;
        return ExfatStatus::Done;
    }

    [[nodiscard]] ExfatStatus Write(const UInt32 index, const UInt64 page, const UInt32 seed) noexcept
    {
        if (m_readOnly || (page >= 6U))
        {
            return ExfatStatus::Refused;
        }
        const ModelFile* const file = Find(index);
        if (file == nullptr)
        {
            return ExfatStatus::StaleObject;
        }
        const bool sessionFile = m_session && (file == &m_pending);
        if ((page > Pages(file->Bytes)) || (!sessionFile && (m_session || (file->Bytes > MaximumBytes))))
        {
            return ExfatStatus::Refused;
        }
        if (!m_reserved || !sessionFile)
        {
            // The first run of free clusters that holds six pages.
            UInt32 run{0U};
            UInt32 first{0U};
            for (UInt32 cluster{2U}; (first == 0U) && (cluster <= (m_layout.Clusters + 1U)); ++cluster)
            {
                run = m_allocated[cluster] ? 0U : (run + 1U);
                first = (run == m_wanted) ? (cluster + 1U - m_wanted) : 0U;
            }
            if (first == 0U)
            {
                return ExfatStatus::Refused;
            }
            if (!sessionFile)
            {
                m_old = *file;
                m_pending = *file;
                m_session = true;
                m_create = false;
            }
            m_reserved = true;
            m_pending.First = first;
        }
        m_dirty = true;
        m_pending.Seeds[page] = seed;
        const UInt64 end = (page + 1U) * 4096U;
        m_pending.Bytes = (m_pending.Bytes > end) ? m_pending.Bytes : end;
        return ExfatStatus::Done;
    }

    void Commit() noexcept
    {
        m_dirty = m_dirty && m_readOnly;
        if (!m_session)
        {
            return;
        }
        const UInt64 used = (m_pending.Bytes + m_clusterBytes - 1U) / m_clusterBytes;
        for (UInt64 cluster{0U}; cluster < used; ++cluster)
        {
            m_allocated[m_pending.First + cluster] = true;
        }
        if (m_pending.Bytes == 0U)
        {
            m_pending.First = 0U;
        }
        if (m_create)
        {
            for (Size entry{m_runStart}; entry < (m_runStart + m_runEntries); ++entry)
            {
                m_used[entry] = true;
            }
            m_files[m_count++] = m_pending;
        }
        else
        {
            const UInt64 old = (m_old.Bytes + m_clusterBytes - 1U) / m_clusterBytes;
            for (UInt64 cluster{0U}; cluster < old; ++cluster)
            {
                m_allocated[m_old.First + cluster] = false;
            }
            for (Size file{0U}; file < m_count; ++file)
            {
                m_files[file] = (m_files[file].Index == m_pending.Index) ? m_pending : m_files[file];
            }
        }
        m_session = false;
        m_reserved = false;
    }

    /// A restart: the session is gone; a dirty volume mounts read-only until
    /// the test repairs it.
    void Remount(const bool repaired) noexcept
    {
        m_session = false;
        m_reserved = false;
        m_dirty = m_dirty && !repaired;
        m_readOnly = m_dirty;
    }

private:
    ModelFile m_files[MaximumFiles]{};
    Size m_count{0U};
    ModelFile m_pending{};
    ModelFile m_old{};
    Layout m_layout{};
    bool m_session{false};
    bool m_create{false};
    bool m_reserved{false};
    bool m_dirty{false};
    bool m_readOnly{false};
    Size m_runStart{0U};
    Size m_runEntries{0U};
    UInt32 m_clusterBytes{512U};
    UInt32 m_wanted{0U};
    UInt32 m_entries{0U};
    bool m_used[MaximumEntries]{};
    bool m_allocated[MaximumClusters + 2U]{};
};

Model g_model;
ModelFile g_fillers[2U];

/// Outcomes seen, by operation (Create, WritePage, ReadPage, Open) and
/// status, so a run shows what it exercised.
Size g_outcomes[4U][16U];

/// A random name: mostly valid ones from a small alphabet (so names recur
/// and long ones need two or three File Name entries), some invalid ones.
[[nodiscard]] Size RandomName(TestSupport::Random& random, char (&name)[65U], bool& valid) noexcept
{
    static constexpr const char* Fixed[]{"Upper", ".dot", "dot.", "a b", "-x", "filler"};
    valid = true;
    if (random.Chance(1U, 12U))
    {
        const char* const fixed = Fixed[random.Below(sizeof(Fixed) / sizeof(Fixed[0U]))];
        const Size length = std::strlen(fixed);
        std::memcpy(name, fixed, length);
        valid = fixed[0U] == 'f';
        return length;
    }
    static constexpr char Alphabet[]{"abab1-_."};
    const Size length = 1U + random.Below(random.Chance(1U, 3U) ? 40U : 4U);
    for (Size at{0U}; at < length; ++at)
    {
        name[at] = ((at == 0U) || ((at + 1U) == length)) ? "ab"[random.Below(2U)] : Alphabet[random.Below(8U)];
    }
    return length;
}

[[nodiscard]] bool Fail(const UInt64 seed, const Size step, const char* const operation, const ExfatStatus actual,
                        const ExfatStatus expected) noexcept
{
    static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: %s answered %u, the model %u\n",
                                   static_cast<unsigned long long>(seed), step, operation,
                                   static_cast<unsigned>(actual), static_cast<unsigned>(expected)));
    return false;
}

/// Runs one seed on its image; false at the first mismatch (printed).
[[nodiscard]] bool RunSeed(const UInt64 seed, Size& commits) noexcept
{
    TestSupport::Random random{seed};
    const Size image = seed % 2U;
    TestDevice& device = g_devices[image];
    g_overlay.Reset(device.Get().Sectors);
    device.Arm(NoBudget);
    UInt64 serial{seed << 8U};
    if (g_volume.Mount(DeviceOf(device), serial) != ExfatStatus::Done)
    {
        static_cast<void>(
            std::fprintf(stderr, "seed %llu: the volume does not mount\n", static_cast<unsigned long long>(seed)));
        return false;
    }
    g_model.Start(device, g_layouts[image], g_fillers[image]);
    for (Size step{0U}; step < 300U; ++step)
    {
        const UInt64 kind = random.Below(100U);
        // A file to act on: a committed one, the session's, or an index.
        UInt32 index{static_cast<UInt32>(random.Below(24U))};
        if (random.Chance(3U, 4U))
        {
            index = g_model.FileAt(random.Below(g_model.GetCount())).Index;
        }
        if (g_model.HasSession() && random.Chance(2U, 3U))
        {
            index = g_model.Session().Index;
        }
        const ExfatObject object{.Identifier = index, .Generation = serial};
        const ModelFile* const file = g_model.Find(index);
        if (kind < 15U)
        {
            char name[65U]{};
            bool valid{false};
            const Size length = RandomName(random, name, valid);
            ExfatObject created{};
            const ExfatStatus expected = g_model.Create(name, length, valid);
            const ExfatStatus actual = g_volume.Create(reinterpret_cast<const UInt8*>(name), length, created);
            ++g_outcomes[0U][static_cast<Size>(actual)];
            if ((actual != expected) ||
                ((actual == ExfatStatus::Done) &&
                 ((created.Identifier != g_model.Session().Index) || (created.Generation != serial))))
            {
                static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: Create of \"%.*s\"\n",
                                               static_cast<unsigned long long>(seed), step, static_cast<int>(length),
                                               name));
                return Fail(seed, step, "Create", actual, expected);
            }
        }
        else if (kind < 45U)
        {
            // Mostly a page the file accepts: one it has or the next.
            const UInt64 page =
                ((file != nullptr) && random.Chance(4U, 5U)) ? random.Below(Pages(file->Bytes) + 1U) : random.Below(8U);
            const UInt32 pageSeed = static_cast<UInt32>(random.Next()) | 1U;
            const ExfatStatus expected = g_model.Write(index, page, pageSeed);
            FillPage(g_page, pageSeed);
            const ExfatStatus actual =
                g_volume.WritePage(object, page < 6U ? page : User::Filesystem::ExfatMaximumFilePages, g_page);
            ++g_outcomes[1U][static_cast<Size>(actual)];
            if (actual != expected)
            {
                return Fail(seed, step, "WritePage", actual, expected);
            }
        }
        else if (kind < 63U)
        {
            g_model.Commit();
            const ExfatStatus actual = g_volume.Commit(ExfatCrashPoint::None);
            const CheckReport report = CheckExfatVolume(device);
            ++commits;
            const bool clean = report.Parsed && (report.Errors == 0U) && (report.LostClusters == 0U) &&
                               (report.Dirty == g_model.IsDirty());
            if ((actual != ExfatStatus::Done) || !clean || (report.Files != g_model.GetCount()))
            {
                static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: commit %u, model files %zu dirty %d\n",
                                               static_cast<unsigned long long>(seed), step,
                                               static_cast<unsigned>(actual), g_model.GetCount(),
                                               g_model.IsDirty() ? 1 : 0));
                Exfat::PrintReport("the checker", report);
                return false;
            }
        }
        else if (kind < 83U)
        {
            const UInt64 page = random.Below(7U);
            const ExfatStatus expected = (page >= 6U)
                                             ? ExfatStatus::Refused
                                             : ((file == nullptr) ? ExfatStatus::StaleObject : ExfatStatus::Done);
            const ExfatStatus actual = g_volume.ReadPage(object, page < 6U ? page : 0x20000000U, g_page);
            ++g_outcomes[2U][static_cast<Size>(actual)];
            if (actual != expected)
            {
                return Fail(seed, step, "ReadPage", actual, expected);
            }
            FillPage(g_expected, ((file != nullptr) && (page < Pages(file->Bytes))) ? file->Seeds[page] : 0U);
            if ((actual == ExfatStatus::Done) && (std::memcmp(g_page, g_expected, ExfatPageBytes) != 0))
            {
                static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: page %llu of %u differs\n",
                                               static_cast<unsigned long long>(seed), step,
                                               static_cast<unsigned long long>(page), index));
                return false;
            }
        }
        else if (kind < 88U)
        {
            ExfatFileSize size{};
            const ExfatStatus actual = g_volume.Stat(object, size);
            const ExfatStatus expected = (file == nullptr) ? ExfatStatus::StaleObject : ExfatStatus::Done;
            if ((actual != expected) ||
                ((file != nullptr) && ((size.Bytes != file->Bytes) || (size.Pages != Pages(file->Bytes)))))
            {
                return Fail(seed, step, "Stat", actual, expected);
            }
        }
        else if (kind < 96U)
        {
            char name[65U]{};
            bool valid{false};
            Size length = RandomName(random, name, valid);
            if (random.Chance(1U, 2U))
            {
                const ModelFile& known = g_model.FileAt(random.Below(g_model.GetCount()));
                length = known.Length;
                for (Size at{0U}; at < length; ++at)
                {
                    const char value = known.Name[at];
                    name[at] = (random.Chance(1U, 2U) && (value >= 'a') && (value <= 'z'))
                                   ? static_cast<char>(value - 32)
                                   : value;
                }
            }
            const ModelFile* const known = g_model.FindName(name, length);
            ExfatObject found{};
            const ExfatStatus expected = (known != nullptr) ? ExfatStatus::Done : ExfatStatus::NotFound;
            const ExfatStatus actual = g_volume.Open(reinterpret_cast<const UInt8*>(name), length, found);
            ++g_outcomes[3U][static_cast<Size>(actual)];
            if ((actual != expected) || ((known != nullptr) && (found.Identifier != known->Index)))
            {
                return Fail(seed, step, "Open", actual, expected);
            }
        }
        else
        {
            // A restart. A dirty volume mounts read-only; half the time the
            // test then clears the flag, as a repair tool would.
            const bool dirty = g_model.IsDirty();
            g_model.Remount(false);
            ++serial;
            const bool mounted =
                (g_volume.Mount(DeviceOf(device), serial) == ExfatStatus::Done) && (g_volume.IsReadOnly() == dirty);
            const bool repair = dirty && random.Chance(1U, 2U);
            if (repair)
            {
                UInt8 boot[512U]{};
                static_cast<void>(device.Read(0U, boot));
                boot[106U] = static_cast<UInt8>(boot[106U] & ~0x02U);
                static_cast<void>(device.Write(0U, boot));
                g_model.Remount(true);
                ++serial;
            }
            if (!mounted || (repair && (g_volume.Mount(DeviceOf(device), serial) != ExfatStatus::Done)) ||
                (g_volume.IsReadOnly() != g_model.IsReadOnly()))
            {
                static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: the remount differs\n",
                                               static_cast<unsigned long long>(seed), step));
                return false;
            }
        }
    }
    return true;
}

/// Places each image's filler: a contiguous file over every free cluster
/// but the last FreeTail ones, at entry 4.
[[nodiscard]] bool PlaceFillers() noexcept
{
    bool done{true};
    for (Size image{0U}; done && (image < 2U); ++image)
    {
        TestDevice& writer = g_writers[image];
        writer.Arm(NoBudget);
        Layout& layout = g_layouts[image];
        done = Exfat::ParseLayout(writer, layout);
        UInt32 first{2U};
        while (done && Exfat::IsAllocated(writer, layout, first))
        {
            ++first;
        }
        const UInt32 clusters = (layout.Clusters + 2U) - FreeTail[image] - first;
        const UInt64 bytes = static_cast<UInt64>(clusters) << (layout.ClusterShift + 9U);
        done = done &&
               Exfat::PlaceFile(writer, layout,
                                FileSpec{.Name = "filler",
                                         .Index = 4U,
                                         .First = first,
                                         .Stride = 0U,
                                         .Size = bytes,
                                         .Valid = bytes,
                                         .Data = nullptr,
                                         .Seed = 500U,
                                         .Attributes = 0x20U,
                                         .Benign = 0U,
                                         .ChecksumDelta = 0U,
                                         .HashDelta = 0U}) &&
               CheckExfatVolume(writer).IsClean();
        ModelFile& filler = g_fillers[image];
        filler = ModelFile{};
        std::memcpy(filler.Name, "filler", 6U);
        filler.Length = 6U;
        filler.Index = 4U;
        filler.First = first;
        filler.Bytes = bytes;
        for (UInt32 page{0U}; page < 6U; ++page)
        {
            filler.Seeds[page] = 500U + page;
        }
    }
    return done;
}

void TestModel(TestContext& context) noexcept
{
    const TestSupport::SeedRange range = TestSupport::GetSeedRange(1U, 16U);
    Size failures{0U};
    Size commits{0U};
    for (UInt64 seed{range.First}; seed < (range.First + range.Count); ++seed)
    {
        failures += RunSeed(seed, commits) ? 0U : 1U;
    }
    static_cast<void>(
        std::printf("%llu seeds, %zu commits checked\n", static_cast<unsigned long long>(range.Count), commits));
    static constexpr const char* Operations[]{"Create", "WritePage", "ReadPage", "Open"};
    for (Size operation{0U}; operation < 4U; ++operation)
    {
        static_cast<void>(std::printf("%s: %zu done, %zu refused, %zu stale, %zu not found\n", Operations[operation],
                                      g_outcomes[operation][0U], g_outcomes[operation][1U], g_outcomes[operation][3U],
                                      g_outcomes[operation][4U]));
    }
    ASTERIA_EXPECT(context, "the volume follows the reference model on every seed", failures == 0U,
                   (range.Count == 1U) || (commits > 100U));
}
} // namespace
} // namespace Asteria::Test

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
    if (!Exfat::LoadImage(Test::g_bases[0U], Exfat::PathOf(directory, "exfat-4k.img")) ||
        !Exfat::LoadImage(Test::g_bases[1U], Exfat::PathOf(directory, "exfat-512.img")))
    {
        static_cast<void>(std::printf("SKIP: no tool-made exFAT images in %s (exfatprogs absent)\n", directory));
        return 0;
    }
    TestContext context{"ExfatVolumeModelTests"};
    ASTERIA_EXPECT(context, "the images take their fillers", Test::PlaceFillers());
    Test::TestModel(context);
    return context.Finish();
}
