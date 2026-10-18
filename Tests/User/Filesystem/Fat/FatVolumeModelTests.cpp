// SPDX-License-Identifier: Apache-2.0
// The FAT volume library against a reference model:
// seeded random Create, WritePage, Commit, ReadPage, Stat, Open and
// remounts on FAT12, FAT16 and FAT32 volumes of several cluster sizes, small
// enough to fill their clusters or root directory. Every result is compared
// with the model's; every commit is followed by the invariant checker, which
// must find the volume clean. ASTERIA_TEST_SEED replays one seed.

#include "User/Filesystem/Fat/FatVolume.hpp"

#include <cstdio>
#include <cstring>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Filesystem/Fat/FatTestSupport.hpp"
#include "User/Filesystem/Fat/FatWorkload.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
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
using User::Filesystem::FatObject;
using User::Filesystem::FatPageBytes;
using User::Filesystem::FatStatus;
using User::Filesystem::FatVolume;

SparseStorage<16U> g_storage;
SparseDisk g_disk{g_storage.Keys, g_storage.Data, 16U};
TestDevice g_device{g_disk};
FatVolume g_volume;
UInt8 g_page[FatPageBytes];
UInt8 g_expected[FatPageBytes];

constexpr FormatOptions Volumes[]{
    {.Sectors = 4096U,
     .SectorsPerCluster = 1U,
     .Reserved = 1U,
     .Fats = 2U,
     .RootEntries = 224U,
     .RootClusters = 0U,
     .HighNibble = 0U,
     .Label = true},
    {.Sectors = 80U,
     .SectorsPerCluster = 1U,
     .Reserved = 1U,
     .Fats = 2U,
     .RootEntries = 16U,
     .RootClusters = 0U,
     .HighNibble = 0U,
     .Label = false},
    {.Sectors = 16384U,
     .SectorsPerCluster = 16U,
     .Reserved = 4U,
     .Fats = 2U,
     .RootEntries = 512U,
     .RootClusters = 0U,
     .HighNibble = 0U,
     .Label = false},
    {.Sectors = 70000U,
     .SectorsPerCluster = 16U,
     .Reserved = 4U,
     .Fats = 2U,
     .RootEntries = 512U,
     .RootClusters = 0U,
     .HighNibble = 0U,
     .Label = true},
    {.Sectors = 9000U,
     .SectorsPerCluster = 2U,
     .Reserved = 1U,
     .Fats = 1U,
     .RootEntries = 16U,
     .RootClusters = 0U,
     .HighNibble = 0U,
     .Label = true},
    {.Sectors = 66700U,
     .SectorsPerCluster = 1U,
     .Reserved = 32U,
     .Fats = 2U,
     .RootEntries = 0U,
     .RootClusters = 2U,
     .HighNibble = 0x5U,
     .Label = true},
    {.Sectors = 528000U,
     .SectorsPerCluster = 8U,
     .Reserved = 32U,
     .Fats = 2U,
     .RootEntries = 0U,
     .RootClusters = 1U,
     .HighNibble = 0U,
     .Label = false},
};

constexpr Size MaximumFiles{128U};
constexpr Size MaximumEntries{512U};

struct ModelFile
{
    char Name[65U];
    Size Length;
    UInt8 Short[11U];
    UInt32 Index;
    UInt32 Bytes;
    UInt32 Seeds[6U];
};

/// The model's short name for a created name: as FatVolume.hpp's Create
/// states it (8.3 when it fits, else up to six base characters, '~', a digit
/// and up to three extension characters), with the long-name entry count.
struct ShortName
{
    UInt8 Bytes[11U];
    bool Fits;
    Size Tilde;
    Size LongEntries;
};

[[nodiscard]] ShortName MakeShort(const char* const name, const Size length) noexcept
{
    ShortName result{};
    std::memset(result.Bytes, ' ', sizeof(result.Bytes));
    Size dot{length};
    Size dots{0U};
    for (Size index{0U}; index < length; ++index)
    {
        if (name[index] == '.')
        {
            dot = index;
            ++dots;
        }
    }
    const Size extension = (dot < length) ? (length - dot - 1U) : 0U;
    result.Fits = (dots <= 1U) && (dot <= 8U) && (extension <= 3U);
    Size put{0U};
    for (Size index{0U}; (index < dot) && (put < (result.Fits ? 8U : 6U)); ++index)
    {
        if (name[index] != '.')
        {
            result.Bytes[put++] =
                static_cast<UInt8>(((name[index] >= 'a') && (name[index] <= 'z')) ? (name[index] - 32) : name[index]);
        }
    }
    result.Tilde = result.Fits ? 11U : put;
    if (!result.Fits)
    {
        result.Bytes[put] = '~';
    }
    for (Size index{0U}; (index < extension) && (index < 3U); ++index)
    {
        const char value = name[dot + 1U + index];
        result.Bytes[8U + index] = static_cast<UInt8>(((value >= 'a') && (value <= 'z')) ? (value - 32) : value);
    }
    result.LongEntries = result.Fits ? 0U : ((length + 12U) / 13U);
    return result;
}

class Model final
{
public:
    void Start(const Layout& layout, const FormatOptions& options, const UInt32 freeClusters) noexcept
    {
        *this = Model{};
        m_clusterBytes = UInt32{512U} << layout.ClusterShift;
        m_free = freeClusters;
        m_entries =
            (options.RootEntries != 0U) ? options.RootEntries : (options.RootClusters * (16U << layout.ClusterShift));
        m_used[0U] = options.Label;
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

    /// The file named by index (a short entry index), the session's view for
    /// the session's file; nullptr for none.
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
    [[nodiscard]] FatStatus Create(const char* const name, const Size length, const bool valid) noexcept
    {
        if (m_readOnly || m_session || !valid || IsDevice(name, length) || (FindName(name, length) != nullptr))
        {
            return FatStatus::Refused;
        }
        ShortName shortName = MakeShort(name, length);
        UInt32 used{0U};
        bool collision{false};
        for (Size file{0U}; file < m_count; ++file)
        {
            const UInt8* const other = m_files[file].Short;
            bool same{true};
            for (Size at{0U}; at < 11U; ++at)
            {
                same = same && ((at == (shortName.Tilde + 1U)) || (other[at] == shortName.Bytes[at]));
            }
            if (same && !shortName.Fits && (other[shortName.Tilde + 1U] >= '1') && (other[shortName.Tilde + 1U] <= '9'))
            {
                used |= 1U << (other[shortName.Tilde + 1U] - '0');
            }
            collision = collision || (same && shortName.Fits);
        }
        UInt32 digit{1U};
        while ((digit <= 9U) && ((used & (1U << digit)) != 0U))
        {
            ++digit;
        }
        const Size needed = shortName.LongEntries + 1U;
        Size start{MaximumEntries};
        Size run{0U};
        for (Size entry{0U}; (start == MaximumEntries) && (entry < m_entries); ++entry)
        {
            run = ((entry % 16U) == 0U) ? 0U : run;
            run = m_used[entry] ? 0U : (run + 1U);
            start = (run == needed) ? (entry + 1U - needed) : start;
        }
        if (collision || (digit > 9U) || (start == MaximumEntries))
        {
            return FatStatus::Refused;
        }
        if (!shortName.Fits)
        {
            shortName.Bytes[shortName.Tilde + 1U] = static_cast<UInt8>('0' + digit);
        }
        m_pending = ModelFile{};
        std::memcpy(m_pending.Name, name, length);
        m_pending.Length = length;
        std::memcpy(m_pending.Short, shortName.Bytes, 11U);
        m_pending.Index = static_cast<UInt32>(start + shortName.LongEntries);
        m_session = true;
        m_create = true;
        m_runStart = start;
        m_chain = 0U;
        return FatStatus::Done;
    }

    [[nodiscard]] FatStatus Write(const UInt32 index, const UInt64 page, const UInt32 seed) noexcept
    {
        const ModelFile* const file = Find(index);
        if (m_readOnly || (page >= 6U))
        {
            return FatStatus::Refused;
        }
        if (file == nullptr)
        {
            return FatStatus::StaleObject;
        }
        const bool sessionFile = m_session && (file == &m_pending);
        if ((page > Pages(file->Bytes)) || (!sessionFile && m_session))
        {
            return FatStatus::Refused;
        }
        const UInt32 needed = Clusters((static_cast<UInt32>(page) + 1U) * 4096U);
        if (!sessionFile)
        {
            const UInt32 copied = Clusters(file->Bytes);
            if ((m_free < copied) || (m_free < ((needed > copied) ? needed : copied)))
            {
                return FatStatus::Refused;
            }
            m_pending = *file;
            m_session = true;
            m_create = false;
            m_chain = copied;
            m_oldClusters = copied;
        }
        const UInt32 extra = (needed > m_chain) ? (needed - m_chain) : 0U;
        if ((m_free - m_chain) < extra)
        {
            return FatStatus::Refused;
        }
        m_dirty = true;
        m_dirtyByMount = true;
        m_chain += extra;
        m_pending.Seeds[page] = seed;
        const UInt32 end = (static_cast<UInt32>(page) + 1U) * 4096U;
        m_pending.Bytes = (m_pending.Bytes > end) ? m_pending.Bytes : end;
        return FatStatus::Done;
    }

    void Commit() noexcept
    {
        // Commit clears only a dirty mark this mount set.
        m_dirty = m_dirty && !m_dirtyByMount && !m_session;
        m_dirtyByMount = false;
        if (!m_session)
        {
            return;
        }
        m_free -= m_chain;
        if (m_create)
        {
            for (Size entry{m_runStart}; entry <= m_pending.Index; ++entry)
            {
                m_used[entry] = true;
            }
            m_files[m_count++] = m_pending;
        }
        else
        {
            m_free += m_oldClusters;
            *FindCommitted(m_pending.Index) = m_pending;
        }
        m_session = false;
    }

    /// A restart: the session is gone; a dirty volume mounts read-only until
    /// the test repairs it.
    void Remount(const bool repaired) noexcept
    {
        m_session = false;
        m_dirtyByMount = false;
        m_readOnly = m_dirty && !repaired;
        m_dirty = m_dirty && !repaired;
    }

    [[nodiscard]] bool IsReadOnly() const noexcept
    {
        return m_readOnly;
    }

    [[nodiscard]] static UInt64 Pages(const UInt32 size) noexcept
    {
        return (static_cast<UInt64>(size) + 4095U) / 4096U;
    }

private:
    [[nodiscard]] UInt32 Clusters(const UInt32 bytes) const noexcept
    {
        return (bytes + m_clusterBytes - 1U) / m_clusterBytes;
    }

    [[nodiscard]] ModelFile* FindCommitted(const UInt32 index) noexcept
    {
        for (Size file{0U}; file < m_count; ++file)
        {
            if (m_files[file].Index == index)
            {
                return &m_files[file];
            }
        }
        return &m_files[0U];
    }

    [[nodiscard]] static char Fold(const char value) noexcept
    {
        return ((value >= 'A') && (value <= 'Z')) ? static_cast<char>(value + 32) : value;
    }

    [[nodiscard]] static bool IsDevice(const char* const name, const Size length) noexcept
    {
        Size base{0U};
        while ((base < length) && (name[base] != '.'))
        {
            ++base;
        }
        static constexpr const char* Plain[]{"con", "prn", "aux", "nul"};
        for (const char* const device : Plain)
        {
            if ((base == 3U) && (std::strncmp(name, device, 3U) == 0))
            {
                return true;
            }
        }
        return (base == 4U) && ((std::strncmp(name, "com", 3U) == 0) || (std::strncmp(name, "lpt", 3U) == 0)) &&
               (name[3U] >= '1') && (name[3U] <= '9');
    }

    ModelFile m_files[MaximumFiles]{};
    Size m_count{0U};
    ModelFile m_pending{};
    bool m_session{false};
    bool m_create{false};
    bool m_dirty{false};
    bool m_dirtyByMount{false};
    bool m_readOnly{false};
    Size m_runStart{0U};
    UInt32 m_chain{0U};
    UInt32 m_oldClusters{0U};
    UInt32 m_free{0U};
    UInt32 m_clusterBytes{512U};
    Size m_entries{0U};
    bool m_used[MaximumEntries]{};
};

Model g_model;

/// Outcomes seen, by operation (Create, WritePage, ReadPage, Open) and
/// status, so a run shows what it exercised.
Size g_outcomes[4U][16U];

/// A random name: mostly valid ones from a small alphabet (so 8.3 and long
/// names, ~N and existing names recur), some device names, uppercase or
/// malformed ones.
[[nodiscard]] Size RandomName(TestSupport::Random& random, char (&name)[65U], bool& valid) noexcept
{
    static constexpr const char* Fixed[]{"con",   "aux.txt", "com3", "lpt1.x", "com0",
                                         "Upper", ".dot",    "dot.", "a~b",    ""};
    valid = true;
    if (random.Chance(1U, 12U))
    {
        const char* const fixed = Fixed[random.Below(sizeof(Fixed) / sizeof(Fixed[0U]))];
        const Size length = std::strlen(fixed);
        std::memcpy(name, fixed, length);
        valid = (length != 0U) && (fixed[0U] != '.') && (fixed[0U] != 'U') && (fixed[length - 1U] != '.') &&
                (fixed[1U] != '~');
        return length;
    }
    // Mostly 'a' and 'b', so names recur; a third share one basis and
    // extension, so ~N climbs to 9 and past it.
    static constexpr char Alphabet[]{"abababab1-_."};
    const auto pick = [&random]() noexcept
    {
        return Alphabet[random.Below(sizeof(Alphabet) - 1U)];
    };
    if (random.Chance(1U, 3U))
    {
        static constexpr char Prefix[]{"fabbab-"};
        Size length{sizeof(Prefix) - 1U};
        std::memcpy(name, Prefix, length);
        for (Size at{random.Below(4U)}; at < 4U; ++at)
        {
            name[length++] = "ab1_"[random.Below(4U)];
        }
        std::memcpy(name + length, ".txtz", 5U);
        return length + 5U;
    }
    const Size base = 1U + random.Below(random.Chance(1U, 2U) ? 8U : 20U);
    Size length{0U};
    for (Size at{0U}; at < base; ++at)
    {
        name[length++] = (at == 0U) ? 'f' : pick();
    }
    if (random.Chance(1U, 2U))
    {
        name[length++] = '.';
        const Size extension = 1U + random.Below(random.Chance(3U, 4U) ? 3U : 6U);
        for (Size at{0U}; at < extension; ++at)
        {
            name[length++] = (at == (extension - 1U)) ? 'z' : pick();
        }
    }
    for (Size at{1U}; at < length; ++at)
    {
        name[at] = ((name[at] == '.') && (name[at - 1U] == '.')) ? 'a' : name[at];
    }
    const char last = name[length - 1U];
    name[length - 1U] = ((last == '-') || (last == '_') || (last == '.')) ? 'q' : last;
    return length;
}

[[nodiscard]] bool Fail(const UInt64 seed, const Size step, const char* const operation, const FatStatus actual,
                        const FatStatus expected) noexcept
{
    static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: %s answered %u, the model %u\n",
                                   static_cast<unsigned long long>(seed), step, operation,
                                   static_cast<unsigned>(actual), static_cast<unsigned>(expected)));
    return false;
}

/// Runs one seed on its volume; false at the first mismatch (printed).
[[nodiscard]] bool RunSeed(const UInt64 seed, Size& commits) noexcept
{
    TestSupport::Random random{seed};
    const FormatOptions& options = Volumes[seed % (sizeof(Volumes) / sizeof(Volumes[0U]))];
    Layout layout{};
    g_disk.Reset(options.Sectors);
    g_device.Arm(NoBudget);
    UInt64 serial{seed << 8U};
    if (!Fat::Format(g_device, options, layout) || (g_volume.Mount(g_device.Get(), serial) != FatStatus::Done))
    {
        static_cast<void>(
            std::fprintf(stderr, "seed %llu: the volume does not format\n", static_cast<unsigned long long>(seed)));
        return false;
    }
    g_model.Start(layout, options, CheckVolume(g_device).FreeClusters);
    for (Size step{0U}; step < 300U; ++step)
    {
        const UInt64 kind = random.Below(100U);
        // A file to act on: a committed one, the session's, or no file.
        UInt32 index{static_cast<UInt32>(random.Below(40U))};
        if ((g_model.GetCount() != 0U) && random.Chance(3U, 4U))
        {
            index = g_model.FileAt(random.Below(g_model.GetCount())).Index;
        }
        if (g_model.HasSession() && random.Chance(2U, 3U))
        {
            index = g_model.Session().Index;
        }
        const FatObject object{.Identifier = index, .Generation = serial};
        const ModelFile* const file = g_model.Find(index);
        if (kind < 15U)
        {
            char name[65U]{};
            bool valid{false};
            const Size length = RandomName(random, name, valid);
            FatObject created{};
            const FatStatus expected = g_model.Create(name, length, valid);
            const FatStatus actual = g_volume.Create(reinterpret_cast<const UInt8*>(name), length, created);
            ++g_outcomes[0U][static_cast<Size>(actual)];
            for (Size at{1U}; (actual == FatStatus::Done) && (at < 7U); ++at)
            {
                const UInt8* const shortName = g_model.Session().Short;
                g_outcomes[0U][10U] += ((shortName[at] == '~') && (shortName[at + 1U] >= '2')) ? 1U : 0U;
            }
            if ((actual != expected) ||
                ((actual == FatStatus::Done) &&
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
            const UInt64 page = ((file != nullptr) && random.Chance(4U, 5U))
                                    ? random.Below(Model::Pages(file->Bytes) + 1U)
                                    : random.Below(8U);
            const UInt32 pageSeed = static_cast<UInt32>(random.Next()) | 1U;
            const FatStatus expected = g_model.Write(index, page, pageSeed);
            Fat::FillPage(g_page, pageSeed);
            const FatStatus actual =
                g_volume.WritePage(object, page < 6U ? page : User::Filesystem::FatMaximumFilePages, g_page);
            ++g_outcomes[1U][static_cast<Size>(actual)];
            if (actual != expected)
            {
                return Fail(seed, step, "WritePage", actual, expected);
            }
        }
        else if (kind < 65U)
        {
            g_model.Commit();
            const FatStatus actual = g_volume.Commit(FatCrashPoint::None);
            const CheckReport report = CheckVolume(g_device);
            ++commits;
            const bool clean = report.Parsed && (report.Errors == 0U) && (report.LostClusters == 0U) &&
                               (report.DifferingSectors == 0U) && !report.FsInfoWrong &&
                               (report.Dirty == g_model.IsDirty());
            if ((actual != FatStatus::Done) || !clean || (report.Files != g_model.GetCount()))
            {
                static_cast<void>(std::fprintf(
                    stderr, "seed %llu step %zu: commit %u, %s (%llu), %u lost, files %zu\n",
                    static_cast<unsigned long long>(seed), step, static_cast<unsigned>(actual),
                    (report.FirstError != nullptr) ? report.FirstError : "-",
                    static_cast<unsigned long long>(report.ErrorDetail), report.LostClusters, report.Files));
                return false;
            }
        }
        else if (kind < 85U)
        {
            const UInt64 page = random.Below(7U);
            const FatStatus expected =
                (page >= 6U) ? FatStatus::Refused : ((file == nullptr) ? FatStatus::StaleObject : FatStatus::Done);
            const FatStatus actual = g_volume.ReadPage(object, page < 6U ? page : 0x20000000U, g_page);
            ++g_outcomes[2U][static_cast<Size>(actual)];
            if (actual != expected)
            {
                return Fail(seed, step, "ReadPage", actual, expected);
            }
            Fat::FillPage(g_expected,
                          ((file != nullptr) && (page < Model::Pages(file->Bytes))) ? file->Seeds[page] : 0U);
            if ((actual == FatStatus::Done) && (std::memcmp(g_page, g_expected, FatPageBytes) != 0))
            {
                static_cast<void>(std::fprintf(stderr, "seed %llu step %zu: page %llu of %u differs\n",
                                               static_cast<unsigned long long>(seed), step,
                                               static_cast<unsigned long long>(page), index));
                return false;
            }
        }
        else if (kind < 90U)
        {
            FatFileSize size{};
            const FatStatus actual = g_volume.Stat(object, size);
            if ((actual != ((file == nullptr) ? FatStatus::StaleObject : FatStatus::Done)) ||
                ((file != nullptr) && ((size.Bytes != file->Bytes) || (size.Pages != Model::Pages(file->Bytes)))))
            {
                return Fail(seed, step, "Stat", actual, (file == nullptr) ? FatStatus::StaleObject : FatStatus::Done);
            }
        }
        else if (kind < 97U)
        {
            char name[65U]{};
            bool valid{false};
            Size length = RandomName(random, name, valid);
            if ((g_model.GetCount() != 0U) && random.Chance(1U, 2U))
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
            const ModelFile* const known = (length != 0U) ? g_model.FindName(name, length) : nullptr;
            FatObject found{};
            const FatStatus expected =
                (length == 0U) ? FatStatus::Refused : ((known != nullptr) ? FatStatus::Done : FatStatus::NotFound);
            const FatStatus actual = g_volume.Open(reinterpret_cast<const UInt8*>(name), length, found);
            ++g_outcomes[3U][static_cast<Size>(actual)];
            if ((actual != expected) || ((known != nullptr) && (found.Identifier != known->Index)))
            {
                return Fail(seed, step, "Open", actual, expected);
            }
        }
        else
        {
            // A restart. A dirty volume mounts read-only; half the time the
            // test then clears the mark, as a repair tool would.
            const bool dirty = g_model.IsDirty();
            g_model.Remount(false);
            ++serial;
            const bool mounted =
                (g_volume.Mount(g_device.Get(), serial) == FatStatus::Done) && (g_volume.IsReadOnly() == dirty);
            const bool repair = dirty && random.Chance(1U, 2U);
            if (repair)
            {
                Fat::ClearDirty(g_device, layout);
                g_model.Remount(true);
                ++serial;
            }
            if (!mounted || (repair && (g_volume.Mount(g_device.Get(), serial) != FatStatus::Done)) ||
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

void TestModel(TestContext& context) noexcept
{
    const TestSupport::SeedRange range = TestSupport::GetSeedRange(1U, 28U);
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
        static_cast<void>(std::printf("%s: %zu done, %zu refused, %zu stale, %zu not found, %zu with ~2 to ~9\n",
                                      Operations[operation], g_outcomes[operation][0U], g_outcomes[operation][1U],
                                      g_outcomes[operation][3U], g_outcomes[operation][4U],
                                      g_outcomes[operation][10U]));
    }
    ASTERIA_EXPECT(context, "the volume follows the reference model on every seed", failures == 0U, commits > 100U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext context{"FatVolumeModelTests"};
    Asteria::Test::TestModel(context);
    return context.Finish();
}
