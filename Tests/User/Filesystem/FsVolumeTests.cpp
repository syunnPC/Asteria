// SPDX-License-Identifier: Apache-2.0
#include "User/Filesystem/FsVolume.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Support/TestContext.hpp"
#include "User/Filesystem/FsTestDevice.hpp"

namespace Asteria::Test::Filesystem
{
namespace
{
using TestSupport::TestContext;
RamDevice g_disk{};
UInt8 g_baseline[sizeof(g_disk.Image)]{};
UInt8 g_data[Fs::FsBlockBytes]{};
Fs::FsVolume g_volume{};
Fs::FsVolume g_remount{};
Fs::FsObject g_walkObjects[Fs::FsObjectCapacity]{};
Fs::FsDirectoryEntry g_walkDirectory[Fs::FsDirectoryCapacity]{};
constexpr UInt8 Base[4U]{'b', 'a', 's', 'e'};
constexpr UInt8 Alpha[5U]{'a', 'l', 'p', 'h', 'a'};
constexpr UInt8 Zulu[4U]{'z', 'u', 'l', 'u'};

[[nodiscard]] bool WriteData(const UInt32 block, const UInt8 byte) noexcept
{
    std::memset(g_data, byte, sizeof(g_data));
    const Fs::FsBlockDevice device = g_disk.Device();
    return device.WriteSectors(device.Context, static_cast<UInt64>(block) * Fs::FsSectorsPerBlock,
                               Fs::FsSectorsPerBlock, g_data);
}

/// Independent walk: decodes from the image, enumerates pointers and checks
/// ownership rather than consulting the allocator. Data blocks are never read
/// by the volume; the test checks their whole contents separately.
[[nodiscard]] bool CheckImage(const Fs::FsVolume& volume) noexcept
{
    const Fs::FsSuperblock& selected = volume.GetCommittedSuperblock();
    Fs::FsSuperblock decoded{};
    bool found{false};
    for (Size slot{0U}; slot < 2U; ++slot)
    {
        const UInt64 sector = (slot == 0U) ? Fs::FsSuperblockA : Fs::FsSuperblockB;
        if (Fs::FsFormat::DecodeSuperblock(g_disk.Image + sector * Fs::FsSectorBytes, Fs::FsSectorBytes, sector,
                                           decoded) &&
            (decoded.VolumeGeneration == selected.VolumeGeneration) && (decoded.TableBlock == selected.TableBlock))
        {
            found = true;
        }
    }
    const UInt8* table = g_disk.Image + selected.TableBlock * Fs::FsBlockBytes;
    if (!found ||
        !Fs::FsFormat::DecodeObjectTable(table, Fs::FsBlockBytes, selected.TableBlock, selected.VolumeGeneration,
                                         g_walkObjects) ||
        (Fs::FsFormat::ReadLittle(table + Fs::FsMetadataLayout::Crc, 4U) != selected.TableCrc))
    {
        return false;
    }
    const UInt32 directoryBlock = g_walkObjects[0U].Direct[0U];
    if (!Fs::FsFormat::IsCowBlock(directoryBlock) ||
        !Fs::FsFormat::DecodeDirectory(g_disk.Image + directoryBlock * Fs::FsBlockBytes, Fs::FsBlockBytes,
                                       directoryBlock, selected.VolumeGeneration, g_walkDirectory))
    {
        return false;
    }
    bool reachable[Fs::FsTotalBlocks]{};
    for (Size block{0U}; block < Fs::FsFirstCowBlock; ++block)
    {
        reachable[block] = true;
    }
    reachable[selected.TableBlock] = true;
    for (Size object{0U}; object < selected.NextObjectId; ++object)
    {
        for (const UInt16 block : g_walkObjects[object].Direct)
        {
            if (block != 0U)
            {
                if (!Fs::FsFormat::IsCowBlock(block) || reachable[block])
                {
                    return false;
                }
                reachable[block] = true;
            }
        }
    }
    for (UInt32 block{0U}; block < Fs::FsTotalBlocks; ++block)
    {
        if (Fs::FsFormat::BitmapHas(selected.Bitmap, block) != reachable[block])
        {
            return false;
        }
    }
    return g_disk.SentinelsIntact();
}

void TestBlankAndIo(TestContext& context) noexcept
{
    g_disk.Reset();
    ASTERIA_EXPECT(context, "blank mount is read-only; explicit format writes table, directory, one sector",
                   g_volume.Mount(g_disk.Device()) == Fs::FsStatus::Blank, g_disk.Writes == 0U,
                   g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success, g_volume.GetGeneration() == 1U,
                   g_disk.Writes == 3U, g_disk.WriteCounts[0U] == 8U, g_disk.WriteCounts[1U] == 8U,
                   g_disk.WriteCounts[2U] == 1U, g_disk.WriteSectors[2U] == Fs::FsSuperblockA, CheckImage(g_volume),
                   g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success);
    g_disk.Arm();
    ASTERIA_EXPECT(context, "format refuses an already formatted volume without writes",
                   g_volume.Format(g_disk.Device()) == Fs::FsStatus::Corrupt, g_disk.Writes == 0U);
    g_disk.Reset();
    g_disk.Image[Fs::FsSuperblockB * Fs::FsSectorBytes + 17U] = 1U;
    ASTERIA_EXPECT(context, "one nonzero superblock prevents destructive format or automatic format",
                   g_volume.Mount(g_disk.Device()) == Fs::FsStatus::Corrupt,
                   g_volume.Format(g_disk.Device()) == Fs::FsStatus::Corrupt, g_disk.Writes == 0U);
    g_disk.Reset();
    g_disk.ReadLimit = 1U;
    ASTERIA_EXPECT(context, "a failed read cannot authorize format",
                   g_volume.Format(g_disk.Device()) == Fs::FsStatus::IoError, g_disk.Writes == 0U);
    const Fs::FsBlockDevice missing{.Context = nullptr, .ReadSectors = nullptr, .WriteSectors = nullptr};
    ASTERIA_EXPECT(context, "missing callbacks and unmounted operations fail safely",
                   g_volume.Mount(missing) == Fs::FsStatus::IoError,
                   g_volume.Create(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::NotMounted,
                   g_volume.Stat(Fs::FsHandle{}).Status == Fs::FsStatus::NotMounted,
                   g_volume.AllocateData(Fs::FsHandle{}, 0U).Status == Fs::FsStatus::NotMounted,
                   g_volume.Commit(1U, 1U).Status == Fs::FsStatus::NotMounted,
                   g_volume.QueryCommit(1U, 1U).Status == Fs::FsStatus::NotMounted);
    for (UInt32 limit{0U}; limit <= 3U; ++limit)
    {
        g_disk.Reset();
        g_disk.Arm(limit);
        const Fs::FsStatus formatted = g_volume.Format(g_disk.Device());
        g_disk.Arm();
        const Fs::FsStatus mounted = g_remount.Mount(g_disk.Device());
        ASTERIA_EXPECT(context, "format can crash at any write and recover blank or complete",
                       formatted == ((limit < 3U) ? Fs::FsStatus::IoError : Fs::FsStatus::Success),
                       mounted == ((limit < 3U) ? Fs::FsStatus::Blank : Fs::FsStatus::Success),
                       g_disk.SentinelsIntact());
    }
}

void TestOperations(TestContext& context) noexcept
{
    g_disk.Reset();
    ASTERIA_EXPECT(context, "format operation fixture", g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success);
    const Fs::FsHandleResult zulu = g_volume.Create(Zulu, sizeof(Zulu));
    const Fs::FsHandleResult alpha = g_volume.Create(Alpha, sizeof(Alpha));
    const Fs::FsHandle stale{.ObjectId = alpha.Handle.ObjectId, .BirthGeneration = alpha.Handle.BirthGeneration + 1U};
    const UInt8 bad[3U]{'a', '/', 'z'};
    ASTERIA_EXPECT(context, "creates assign generation-bound IDs and support sorted names",
                   zulu.Status == Fs::FsStatus::Success, alpha.Status == Fs::FsStatus::Success,
                   zulu.Handle.ObjectId == 1U, alpha.Handle.ObjectId == 2U, alpha.Handle.BirthGeneration == 2U,
                   g_volume.Lookup(Alpha, sizeof(Alpha)).Handle.ObjectId == 2U,
                   g_volume.Create(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::AlreadyExists,
                   g_volume.Lookup(Base, sizeof(Base)).Status == Fs::FsStatus::NotFound,
                   g_volume.Create(bad, sizeof(bad)).Status == Fs::FsStatus::InvalidName,
                   g_volume.Stat(stale).Status == Fs::FsStatus::StaleObject,
                   g_volume.AllocateData(stale, 0U).Status == Fs::FsStatus::StaleObject,
                   g_volume.AllocateData(alpha.Handle, 6U).Status == Fs::FsStatus::InvalidIndex,
                   (g_volume.AllocateData(Fs::FsHandle{.ObjectId = 0U, .BirthGeneration = 1U}, 0U).Status ==
                    Fs::FsStatus::InvalidObject));
    const Fs::FsAllocationResult allocation = g_volume.AllocateData(alpha.Handle, 5U);
    ASTERIA_EXPECT(context, "allocation of sixth block preserves sparse holes without touching data",
                   allocation.Status == Fs::FsStatus::Success, g_disk.Writes == 3U, WriteData(allocation.Block, 0x31U),
                   g_volume.Stat(alpha.Handle).Object.SizeBytes == 24576U,
                   g_volume.Stat(alpha.Handle).Object.Direct[0U] == 0U, g_volume.Commit(11U, 101U).Generation == 2U,
                   CheckImage(g_volume));
    const UInt32 old = allocation.Block;
    const Fs::FsAllocationResult replacement = g_volume.AllocateData(alpha.Handle, 5U);
    ASTERIA_EXPECT(context, "old committed blocks cannot be reused before publication",
                   replacement.Status == Fs::FsStatus::Success, replacement.Block != old,
                   Fs::FsFormat::BitmapHas(g_volume.GetCommittedSuperblock().Bitmap, old),
                   WriteData(replacement.Block, 0x32U), g_volume.Commit(22U, 102U).Generation == 3U,
                   !Fs::FsFormat::BitmapHas(g_volume.GetCommittedSuperblock().Bitmap, old), CheckImage(g_volume));
    const Fs::FsAllocationResult reclaimed = g_volume.AllocateData(alpha.Handle, 0U);
    ASTERIA_EXPECT(context, "freed blocks become allocatable only after commit",
                   reclaimed.Status == Fs::FsStatus::Success, reclaimed.Block == old);
    g_disk.Arm();
    ASTERIA_EXPECT(context, "commit replay performs no writes and records original generations across senders",
                   g_volume.Commit(11U, 101U).Generation == 2U, g_disk.Writes == 0U,
                   g_volume.QueryCommit(11U, 101U).Status == Fs::FsStatus::Committed,
                   g_volume.QueryCommit(11U, 101U).Generation == 2U,
                   g_volume.QueryCommit(22U, 101U).Status == Fs::FsStatus::NotCommitted,
                   g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success,
                   g_remount.Stat(alpha.Handle).Object.Direct[0U] == 0U, CheckImage(g_remount));
    for (UInt64 transaction{103U}; transaction < 107U; ++transaction)
    {
        ASTERIA_EXPECT(context, "retain four commit generations after remount",
                       g_remount.Commit(11U, transaction).Status == Fs::FsStatus::Success);
    }
    ASTERIA_EXPECT(context, "older pairs are evicted and unrelated senders cannot match",
                   g_remount.QueryCommit(11U, 101U).Status == Fs::FsStatus::NotCommitted,
                   g_remount.QueryCommit(11U, 103U).Generation == 4U,
                   g_remount.QueryCommit(99U, 106U).Status == Fs::FsStatus::NotCommitted, CheckImage(g_remount));
}

void TestInternalPublication(TestContext& context) noexcept
{
    g_disk.Reset();
    ASTERIA_EXPECT(context, "internal publication starts from a clean volume",
                   g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success,
                   g_volume.Create(Base, sizeof(Base)).Status == Fs::FsStatus::Success,
                   g_volume.Commit(7U, 77U).Generation == 2U);
    std::memcpy(g_baseline, g_disk.Image, sizeof(g_baseline));
    for (UInt32 budget{0U}; budget <= 3U; ++budget)
    {
        std::memcpy(g_disk.Image, g_baseline, sizeof(g_baseline));
        g_disk.Arm();
        const bool staged = g_volume.Mount(g_disk.Device()) == Fs::FsStatus::Success &&
                            g_volume.Create(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::Success;
        g_disk.Arm(budget);
        const auto result = g_volume.CommitStaged();
        g_disk.Arm();
        const bool mounted = g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success;
        ASTERIA_EXPECT(context,
                       "InternalPublicationCrashSweep: each write leaves old or complete new metadata and history",
                       staged, result.Status == (budget == 3U ? Fs::FsStatus::Success : Fs::FsStatus::IoError), mounted,
                       g_remount.Lookup(Alpha, sizeof(Alpha)).Status ==
                           (budget == 3U ? Fs::FsStatus::Success : Fs::FsStatus::NotFound),
                       g_remount.QueryCommit(7U, 77U).Generation == 2U, CheckImage(g_remount));
        g_disk.Arm(0U);
        ASTERIA_EXPECT(context,
                       "InternalPublicationClean: a clean generation requires no write even on a refusing device",
                       g_remount.CommitStaged().Generation == g_remount.GetGeneration(), g_disk.Writes == 0U);
    }
}

void MakeTwoGenerations(TestContext& context) noexcept
{
    g_disk.Reset();
    ASTERIA_EXPECT(context, "fallback fixture initial generation",
                   g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success);
    ASTERIA_EXPECT(context, "fallback fixture newer generation",
                   g_volume.Create(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::Success,
                   g_volume.Commit(1U, 1U).Status == Fs::FsStatus::Success);
    std::memcpy(g_baseline, g_disk.Image, sizeof(g_baseline));
}

void TestFallback(TestContext& context) noexcept
{
    MakeTwoGenerations(context);
    ASTERIA_EXPECT(context, "mount chooses higher valid generation",
                   g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success, g_remount.GetGeneration() == 2U,
                   g_remount.Lookup(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::Success, CheckImage(g_remount));
    const UInt32 tableBlock = g_remount.GetCommittedSuperblock().TableBlock;
    const UInt32 directoryBlock = g_remount.Stat(Fs::FsHandle{.ObjectId = 0U, .BirthGeneration = 1U}).Object.Direct[0U];
    const Size offsets[4U]{Fs::FsSuperblockB * Fs::FsSectorBytes, Fs::FsSuperblockB * Fs::FsSectorBytes + 508U,
                           tableBlock * Fs::FsBlockBytes + 50U, directoryBlock * Fs::FsBlockBytes + 50U};
    for (const Size offset : offsets)
    {
        std::memcpy(g_disk.Image, g_baseline, sizeof(g_baseline));
        g_disk.Image[offset] ^= 0x5AU;
        ASTERIA_EXPECT(context, "corrupt newer superblock, table or directory falls back to an intact generation",
                       g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success, g_remount.GetGeneration() == 1U,
                       g_remount.Lookup(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::NotFound, CheckImage(g_remount));
    }
    std::memcpy(g_disk.Image, g_baseline, sizeof(g_baseline));
    // A coherent checksum over an incorrect bitmap must still be refused.
    UInt8* superblock = g_disk.Image + Fs::FsSuperblockB * Fs::FsSectorBytes;
    superblock[Fs::FsSuperblockLayout::Bitmap + 63U] ^= 0x80U;
    Fs::FsFormat::WriteLittle(superblock + 508U, 4U, Fs::FsFormat::Checksum(superblock, 512U, 508U, 16U, 2U));
    ASTERIA_EXPECT(context, "mount rejects a valid CRC with bitmap unequal to reachability",
                   g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success, g_remount.GetGeneration() == 1U);
    g_disk.Image[0U] = 0U;
    ASTERIA_EXPECT(context, "no valid reachable generation is Corrupt and never formatted",
                   g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Corrupt);
    std::memcpy(g_disk.Image, g_baseline, sizeof(g_baseline));
    g_disk.Arm();
    g_disk.ReadLimit = 2U;
    ASTERIA_EXPECT(context, "metadata read failure is IoError, not evidence for fallback or formatting",
                   g_remount.Mount(g_disk.Device()) == Fs::FsStatus::IoError, g_disk.Writes == 0U);
}

[[nodiscard]] bool Script() noexcept
{
    const Fs::FsHandle base = g_volume.Lookup(Base, sizeof(Base)).Handle;
    for (UInt8 step{1U}; step <= 3U; ++step)
    {
        const UInt8 name[2U]{'f', static_cast<UInt8>('0' + step)};
        const Fs::FsHandleResult created = g_volume.Create(name, sizeof(name));
        if (created.Status != Fs::FsStatus::Success)
        {
            return false;
        }
        const Fs::FsAllocationResult data = g_volume.AllocateData(created.Handle, 0U);
        const Fs::FsAllocationResult replacement = g_volume.AllocateData(base, 0U);
        if ((data.Status != Fs::FsStatus::Success) || (replacement.Status != Fs::FsStatus::Success) ||
            !WriteData(data.Block, step) || !WriteData(replacement.Block, static_cast<UInt8>(0x10U + step)) ||
            (g_volume.Commit(9U, step).Status != Fs::FsStatus::Success))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool BytesEqual(const UInt32 block, const UInt8 byte) noexcept
{
    if (!Fs::FsFormat::IsCowBlock(block))
    {
        return false;
    }
    for (Size index{0U}; index < Fs::FsBlockBytes; ++index)
    {
        if (g_disk.Image[block * Fs::FsBlockBytes + index] != byte)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool ScriptState(const UInt32 completed) noexcept
{
    if ((g_remount.GetGeneration() != (2U + completed)) ||
        (g_remount.GetCommittedSuperblock().NextObjectId != (2U + completed)) || !CheckImage(g_remount))
    {
        return false;
    }
    const Fs::FsHandleResult base = g_remount.Lookup(Base, sizeof(Base));
    if ((base.Status != Fs::FsStatus::Success) ||
        !BytesEqual(g_remount.Stat(base.Handle).Object.Direct[0U], static_cast<UInt8>(0x10U + completed)))
    {
        return false;
    }
    for (UInt8 step{1U}; step <= 3U; ++step)
    {
        const UInt8 name[2U]{'f', static_cast<UInt8>('0' + step)};
        const Fs::FsHandleResult file = g_remount.Lookup(name, sizeof(name));
        const Fs::FsCommitResult query = g_remount.QueryCommit(9U, step);
        if (step <= completed)
        {
            if ((file.Status != Fs::FsStatus::Success) || (file.Handle.ObjectId != (1U + step)) ||
                (file.Handle.BirthGeneration != (2U + step)) || (query.Status != Fs::FsStatus::Committed) ||
                (query.Generation != (2U + step)) || !BytesEqual(g_remount.Stat(file.Handle).Object.Direct[0U], step))
            {
                return false;
            }
        }
        else if ((file.Status != Fs::FsStatus::NotFound) || (query.Status != Fs::FsStatus::NotCommitted))
        {
            return false;
        }
    }
    return true;
}

void TestCrashEveryWrite(TestContext& context) noexcept
{
    g_disk.Reset();
    ASTERIA_EXPECT(context, "crash fixture formats", g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success);
    const Fs::FsHandleResult base = g_volume.Create(Base, sizeof(Base));
    const Fs::FsAllocationResult data = g_volume.AllocateData(base.Handle, 0U);
    ASTERIA_EXPECT(context, "crash fixture commits a baseline", base.Status == Fs::FsStatus::Success,
                   data.Status == Fs::FsStatus::Success, WriteData(data.Block, 0x10U),
                   g_volume.Commit(9U, 0U).Status == Fs::FsStatus::Success);
    std::memcpy(g_baseline, g_disk.Image, sizeof(g_baseline));
    // Three creates + overwrites, five ordered writes per commit. Test every
    // boundary, every landed-but-failed write, and every half-torn write.
    for (UInt32 mode{0U}; mode < 3U; ++mode)
    {
        for (UInt32 limit{0U}; limit <= 15U; ++limit)
        {
            std::memcpy(g_disk.Image, g_baseline, sizeof(g_baseline));
            g_disk.Arm();
            ASTERIA_EXPECT(context, "baseline remount", g_volume.Mount(g_disk.Device()) == Fs::FsStatus::Success);
            g_disk.Arm(limit, mode == 1U, mode == 2U);
            const bool finished = Script();
            const bool protectedIntact = g_disk.SentinelsIntact();
            const UInt32 completed = ((mode == 2U) && (limit != 0U)) ? ((limit - 1U) / 5U) : (limit / 5U);
            g_disk.Arm();
            const bool good = (g_remount.Mount(g_disk.Device()) == Fs::FsStatus::Success) && ScriptState(completed) &&
                              protectedIntact;
            if (!good)
            {
                static_cast<void>(std::fprintf(stderr, "crash mode=%u k=%u completed=%u\n", mode, limit, completed));
            }
            ASTERIA_EXPECT(context, "every write crash yields only the complete preceding or next transaction", good,
                           finished == ((mode == 0U) && (limit == 15U)));
        }
    }
}

struct ModelFile
{
    Fs::FsHandle Handle{};
    UInt64 SizeBytes{0U};
    UInt16 Direct[Fs::FsDirectCapacity]{};
    UInt8 Data[Fs::FsDirectCapacity]{};
};
ModelFile g_model[Fs::FsDirectoryCapacity]{};
ModelFile g_committed[Fs::FsDirectoryCapacity]{};

[[nodiscard]] UInt32 Random(UInt32& state) noexcept
{
    // Intentional wrapping LCG, reproducible on both architectures.
    state = state * 1664525U + 1013904223U;
    return state;
}

[[nodiscard]] bool ModelMatches(const UInt32 count, const ModelFile* model) noexcept
{
    for (UInt32 index{0U}; index < count; ++index)
    {
        const UInt8 name[3U]{'m', static_cast<UInt8>('0' + index / 10U), static_cast<UInt8>('0' + index % 10U)};
        const Fs::FsHandleResult found = g_volume.Lookup(name, sizeof(name));
        const ModelFile& expected = model[index];
        const Fs::FsStatResult stat = g_volume.Stat(expected.Handle);
        if ((found.Status != Fs::FsStatus::Success) || (found.Handle.ObjectId != expected.Handle.ObjectId) ||
            (found.Handle.BirthGeneration != expected.Handle.BirthGeneration) ||
            (stat.Status != Fs::FsStatus::Success) || (stat.Object.SizeBytes != expected.SizeBytes))
        {
            return false;
        }
        for (Size direct{0U}; direct < Fs::FsDirectCapacity; ++direct)
        {
            if ((stat.Object.Direct[direct] != expected.Direct[direct]) ||
                ((expected.Direct[direct] != 0U) && !BytesEqual(expected.Direct[direct], expected.Data[direct])))
            {
                return false;
            }
        }
    }
    return true;
}

void TestRandomModel(TestContext& context) noexcept
{
    const char* seedText = std::getenv("ASTERIA_TEST_SEED");
    const UInt32 seed = (seedText == nullptr) ? 0xA57E1234U : static_cast<UInt32>(std::strtoul(seedText, nullptr, 0));
    UInt32 state{seed};
    UInt32 count{0U};
    UInt32 committedCount{0U};
    UInt64 generation{1U};
    UInt64 transaction{1U};
    g_disk.Reset();
    ASTERIA_EXPECT(context, "model fixture", g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success);
    for (UInt32 step{0U}; step < 800U; ++step)
    {
        const UInt32 op = Random(state) % 7U;
        bool good{true};
        if ((op == 0U) && (count < Fs::FsDirectoryCapacity))
        {
            const UInt8 name[3U]{'m', static_cast<UInt8>('0' + count / 10U), static_cast<UInt8>('0' + count % 10U)};
            const Fs::FsHandleResult created = g_volume.Create(name, sizeof(name));
            good = (created.Status == Fs::FsStatus::Success) && (created.Handle.ObjectId == (count + 1U)) &&
                   (created.Handle.BirthGeneration == (generation + 1U));
            g_model[count] = ModelFile{};
            g_model[count].Handle = created.Handle;
            ++count;
        }
        else if ((op <= 2U) && (count != 0U))
        {
            const UInt32 file = Random(state) % count;
            const Size direct = Random(state) % Fs::FsDirectCapacity;
            const UInt8 byte = static_cast<UInt8>(Random(state) & 0xFFU);
            const Fs::FsAllocationResult allocation = g_volume.AllocateData(g_model[file].Handle, direct);
            good = (allocation.Status == Fs::FsStatus::Success) &&
                   !Fs::FsFormat::BitmapHas(g_volume.GetCommittedSuperblock().Bitmap, allocation.Block) &&
                   (allocation.Block != g_model[file].Direct[direct]) && WriteData(allocation.Block, byte);
            if (good)
            {
                g_model[file].Direct[direct] = static_cast<UInt16>(allocation.Block);
                g_model[file].Data[direct] = byte;
                const UInt64 end = (direct + 1U) * Fs::FsBlockBytes;
                if (end > g_model[file].SizeBytes)
                {
                    g_model[file].SizeBytes = end;
                }
            }
        }
        else if (op == 3U)
        {
            const Fs::FsCommitResult result = g_volume.Commit(77U, transaction);
            ++generation;
            good = (result.Status == Fs::FsStatus::Success) && (result.Generation == generation) &&
                   CheckImage(g_volume) && (g_volume.QueryCommit(77U, transaction).Generation == generation);
            ++transaction;
            std::memcpy(g_committed, g_model, sizeof(g_model));
            committedCount = count;
        }
        else if (op == 4U)
        {
            good = g_volume.Mount(g_disk.Device()) == Fs::FsStatus::Success;
            std::memcpy(g_model, g_committed, sizeof(g_model));
            count = committedCount;
            good = good && CheckImage(g_volume);
        }
        else if ((op == 5U) && (count != 0U))
        {
            Fs::FsHandle stale = g_model[Random(state) % count].Handle;
            ++stale.BirthGeneration;
            good = g_volume.Stat(stale).Status == Fs::FsStatus::StaleObject;
        }
        else if ((op == 6U) && (count != 0U))
        {
            const UInt32 file = Random(state) % count;
            const UInt8 name[3U]{'m', static_cast<UInt8>('0' + file / 10U), static_cast<UInt8>('0' + file % 10U)};
            good = g_volume.Create(name, sizeof(name)).Status == Fs::FsStatus::AlreadyExists;
        }
        good = good && (g_volume.GetGeneration() == generation) && ModelMatches(count, g_model) &&
               g_disk.SentinelsIntact();
        if (!good)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%u step=%u op=%u\n", seed, step, op));
        }
        ASTERIA_EXPECT(context, "LCG workload matches the reference after every operation", good);
        if (!good)
        {
            return;
        }
    }
}

void TestCapacity(TestContext& context) noexcept
{
    g_disk.Reset();
    ASTERIA_EXPECT(context, "capacity fixture", g_volume.Format(g_disk.Device()) == Fs::FsStatus::Success);
    Fs::FsHandle first{};
    for (UInt32 index{0U}; index < Fs::FsDirectoryCapacity; ++index)
    {
        const UInt8 name[3U]{'n', static_cast<UInt8>('0' + index / 10U), static_cast<UInt8>('0' + index % 10U)};
        const Fs::FsHandleResult created = g_volume.Create(name, sizeof(name));
        ASTERIA_EXPECT(context, "directory reaches bounded capacity", created.Status == Fs::FsStatus::Success);
        if (index == 0U)
        {
            first = created.Handle;
        }
    }
    ASTERIA_EXPECT(context, "full directory fails without mutation",
                   g_volume.Create(Alpha, sizeof(Alpha)).Status == Fs::FsStatus::Full);
    UInt32 allocated{0U};
    for (UInt32 attempt{0U}; attempt < Fs::FsTotalBlocks; ++attempt)
    {
        const Fs::FsAllocationResult data = g_volume.AllocateData(first, 0U);
        if (data.Status == Fs::FsStatus::Full)
        {
            break;
        }
        ASTERIA_EXPECT(context, "staged replacements reserve fresh blocks", data.Status == Fs::FsStatus::Success);
        ++allocated;
    }
    ASTERIA_EXPECT(context, "allocation exhaustion leaves two metadata blocks and commit reclaims abandoned staging",
                   allocated == 492U, g_volume.Commit(1U, 1U).Status == Fs::FsStatus::Success, CheckImage(g_volume),
                   g_volume.AllocateData(first, 1U).Status == Fs::FsStatus::Success);
}
} // namespace
} // namespace Asteria::Test::Filesystem

int main() noexcept
{
    Asteria::TestSupport::TestContext context{"FsVolume"};
    Asteria::Test::Filesystem::TestBlankAndIo(context);
    Asteria::Test::Filesystem::TestOperations(context);
    Asteria::Test::Filesystem::TestInternalPublication(context);
    Asteria::Test::Filesystem::TestFallback(context);
    Asteria::Test::Filesystem::TestCrashEveryWrite(context);
    Asteria::Test::Filesystem::TestRandomModel(context);
    Asteria::Test::Filesystem::TestCapacity(context);
    return context.Finish();
}
