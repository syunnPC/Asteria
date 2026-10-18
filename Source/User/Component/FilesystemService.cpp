// SPDX-License-Identifier: Apache-2.0

#include "User/Component/FilesystemService.hpp"

#include "User/Component/ServiceScratch.hpp"

#include "Abi/SystemCallAbi.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Component/FsViews.hpp"
#include "User/Component/ServiceCapabilities.hpp"
#include "User/Filesystem/Exfat/ExfatVolume.hpp"
#include "User/Filesystem/Fat/FatVolume.hpp"
#include "User/Filesystem/FsVolume.hpp"
#include "User/Filesystem/Partitions.hpp"
#include "User/System/DeviceAccess.hpp"
#include "User/System/ObjectRequests.hpp"

namespace Asteria::User::Component
{
namespace
{
using Filesystem::ExfatStatus;
using Filesystem::FatStatus;
using Filesystem::FsStatus;

constexpr Foundation::UInt64 PageBytes{4096U};
constexpr Foundation::UInt32 SectorBytes{512U};
// The most sectors one transfer of the sector device moves: a page, the
// bounce Frame's size and the libraries' largest request.
constexpr Foundation::UInt32 MaximumSectors{8U};
// A delivery's sender identity (IpcDeliveryCodec): the ResourceDomainId of
// the sender, which the kernel writes and the sender cannot choose.
constexpr Foundation::Size DeliverySenderOffset{24U};

static_assert(FsMaximumFileBlocks == Filesystem::FsDirectCapacity);
static_assert(Filesystem::FsSectorsPerBlock == MaximumSectors);
static_assert((Filesystem::FatPageBytes == PageBytes) && (Filesystem::FsBlockBytes == PageBytes) &&
              (Filesystem::ExfatPageBytes == PageBytes));

enum class Backend : Foundation::UInt8
{
    None,
    Fat,
    Exfat,
    Asteria,
};

// The instance: what its start block and image gave it, the capabilities
// it created, its sector device's state and its mounted backend.
struct ServiceState
{
    const System::SystemCallClient* Client;
    FilesystemPlatform Platform;
    System::CapabilityHandle Endpoint;
    System::CapabilityHandle WaitSet;
    System::CapabilityHandle Manager;
    System::CapabilityHandle Space;
    System::CapabilityHandle BlockService;
    System::CapabilityHandle BlockReplies;
    // The bounce Frame at the window's second page, and the MemoryObject
    // through which the first page maps a client's Frame.
    System::CapabilityHandle Bounce;
    System::CapabilityHandle ClientMemory;
    Foundation::UInt64 Incarnation;
    Foundation::UInt64 CrashKind;
    Foundation::UInt64 BlockGeneration;
    Foundation::UInt64 MediumGeneration;
    Foundation::UInt64 Sectors;
    Foundation::UInt64 VolumeStart{0U};
    Foundation::UInt64 DeviceSectors{0U};
    Foundation::UInt64 ClientPageOffset{0U};
    Foundation::UInt64 MediumFlags;
    Foundation::UInt64 MountFlags;
    // The last block transaction sent, zero before the first.
    Foundation::UInt64 Transaction;
    // The last device failure's reason, an FsFailed answer's detail:
    // BlockFailed's detail, or the reply's kind (BlockRefused for a request
    // the kernel did not send).
    Foundation::UInt64 DeviceFailure;
    // Commit's crash flags while an AsteriaFS Commit runs, else zero.
    Foundation::UInt64 CrashFlags;
    // What every request is answered while no backend is mounted.
    Foundation::UInt64 Unmounted;
    // Replies refused by the kernel are dropped and counted.
    Foundation::UInt64 DroppedReplies;
    // Nonzero once the instance must end: a failure of its own, or a crash
    // that returned.
    Foundation::UInt64 Exit;
    Backend Mounted;
    bool Writable{false};
    Foundation::UInt64 Writer{0U};
    bool Quiesced{false};
};

// A request's answer: its reply's kind, detail and extra word.
struct FsAnswer
{
    Foundation::UInt64 Kind;
    Foundation::UInt64 Detail{0U};
    Foundation::UInt64 Extra{0U};
    Foundation::UInt64 NodeKind{0U};
    Foundation::UInt64 Size{0U};
    Foundation::UInt64 Object{0U};
    Foundation::UInt64 Generation{0U};
};

// The instance's state, both backends (only the mounted one is used) and the
// buffers it receives requests and block replies into: static, not on its
// stack, and inside its image's own mappings, as the kernel's record-backed
// access requires.
constinit ServiceState g_state{};
Filesystem::FatVolume* g_fat{nullptr};
Filesystem::ExfatVolume* g_exfat{nullptr};
FsViews* g_views{nullptr};
Filesystem::Partitions* g_partitions{nullptr};
constinit FsMessage g_backendRequest{};
constinit FsMessage g_pagesRequest{};
constinit Foundation::UInt8 g_name[FsMaximumNameBytes]{};
alignas(8) constinit Foundation::UInt8 g_request[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};
alignas(8) constinit Foundation::UInt8 g_blockReply[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};

[[nodiscard]] constexpr Foundation::UInt64 FilesystemFailure(const Foundation::UInt64 step,
                                                             const Abi::SystemCallError error) noexcept
{
    return Abi::ComponentFilesystemFailedExitCode | (step << 32U) | (static_cast<Foundation::UInt64>(error) << 40U);
}

// Records the instance's first failure of its own; later ones keep it.
void End(const Foundation::UInt64 exit) noexcept
{
    g_state.Exit = (g_state.Exit == 0U) ? exit : g_state.Exit;
}

[[nodiscard]] Foundation::UInt8* WindowPage(const Foundation::UInt64 page) noexcept
{
    return reinterpret_cast<Foundation::UInt8*>(
        static_cast<Foundation::UIntPointer>(g_state.Platform.Window + (page * PageBytes)));
}

void CopyBytes(Foundation::UInt8* const target, const Foundation::UInt8* const source,
               const Foundation::Size bytes) noexcept
{
    for (Foundation::Size index{0U}; index < bytes; ++index)
    {
        target[index] = source[index];
    }
}

// Receives block replies until the one answering the transaction in flight: first what the
// block-reply Endpoint holds, then a wait whichever member wakes it. A wake for the service
// Endpoint consumes that Endpoint's readiness, which is signalled once per message published: its
// request stays queued, and the loop receives until the Endpoint is empty before it waits again.
// Another frame is dropped, its capabilities closed. True for BlockDone with sectors; otherwise the
// failure is recorded, or the instance's end when a wait or receive failed.
[[nodiscard, gnu::noinline]] bool Await(const Foundation::UInt64 sectors,
                                        const Foundation::UInt32 expectedMethod = BlockReplyMethod) noexcept
{
    const System::SystemCallClient& client = *g_state.Client;
    for (;;)
    {
        const Abi::SystemCallResponse received =
            client.Receive(g_state.BlockReplies, System::AddressOf(g_blockReply), sizeof(g_blockReply));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            const Abi::SystemCallError waited = client.Wait(g_state.WaitSet).Error;
            if (waited != Abi::SystemCallError::None)
            {
                End(FilesystemFailure(Abi::ComponentFilesystemWaitStep, waited));
                return false;
            }
            continue;
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            End(FilesystemFailure(Abi::ComponentFilesystemReceiveStep, received.Error));
            return false;
        }
        BlockMessage reply{};
        if (!ReadBlockFrame(g_blockReply, static_cast<Foundation::Size>(received.Value1), reply) ||
            (reply.Transaction != g_state.Transaction) ||
            ((reply.Method != BlockReplyMethod) &&
             ((expectedMethod != BlockDescribeReplyMethod) || (reply.Method != BlockDescribeReplyMethod))) ||
            ((reply.Method == BlockReplyMethod) && (reply.Words[0U] != g_state.Transaction)))
        {
            if (!CloseAttachments(client, g_blockReply, 0U))
            {
                End(FilesystemFailure(Abi::ComponentFilesystemCloseStep, Abi::SystemCallError::None));
                return false;
            }
            continue;
        }
        if (reply.Method == BlockDescribeReplyMethod)
        {
            g_state.Sectors = reply.Words[1U];
            g_state.MediumFlags = reply.Words[2U];
            g_state.MediumGeneration = reply.Words[3U];
            if (reply.Words[0U] != SectorBytes)
            {
                g_state.Unmounted = FsUnsupported;
                return false;
            }
            if ((g_state.MediumFlags & BlockMediumPresent) == 0U)
            {
                g_state.DeviceFailure = BlockNoDevice;
                return false;
            }
            return true;
        }
        if ((expectedMethod == BlockReplyMethod) && (reply.Words[1U] == BlockDone) && (reply.Words[2U] == sectors))
        {
            return true;
        }
        if ((reply.Words[1U] == BlockFailed) && (reply.Words[2U] == BlockUnsupportedSectorSize))
        {
            g_state.Mounted = Backend::None;
            g_state.Unmounted = FsUnsupported;
        }
        if (reply.Words[1U] == BlockMediumChanged)
        {
            g_state.Mounted = Backend::None;
            g_state.Unmounted = FsFailed;
        }
        g_state.DeviceFailure = (reply.Words[1U] == BlockFailed) ? reply.Words[2U] : reply.Words[1U];
        return false;
    }
}

// One block request (BlockFrame.hpp) of method for count sectors from
// sector, with a copy of the block-reply Endpoint (Invoke) and a Map-only
// copy of frame, awaited. Transactions count from 1, since the kernel's
// codec refuses 0; an instance does not send 2^64 requests.
[[nodiscard, gnu::noinline]] bool Transfer(const Foundation::UInt32 method, const Foundation::UInt64 sector,
                                           const Foundation::UInt32 count,
                                           const System::CapabilityHandle frame) noexcept
{
    if ((g_state.Exit != 0U) || (g_state.DeviceFailure == BlockMediumChanged))
    {
        return false;
    }
    const auto capacity = g_state.DeviceSectors != 0U ? g_state.DeviceSectors : g_state.Sectors;
    if ((sector > capacity) || (count > capacity - sector))
    {
        g_state.DeviceFailure = BlockOutOfRange;
        return false;
    }
    if ((method == BlockWriteMethod) && ((g_state.MediumFlags & BlockReadOnly) != 0U))
    {
        g_state.DeviceFailure = BlockReadOnly;
        return false;
    }
    ++g_state.Transaction;
    alignas(8) Foundation::UInt8 request[BlockIoRequestBytes]{};
    const Foundation::UInt64 words[BlockIoPayloadWords]{g_state.BlockGeneration, sector, count, 0U,
                                                        g_state.MediumGeneration};
    const Foundation::Size bytes =
        WriteBlockFrame(request, method, g_state.Transaction, words, &g_state.BlockReplies, &frame);
    if (g_state.Client->Send(g_state.BlockService, System::AddressOf(request), bytes).Error !=
        Abi::SystemCallError::None)
    {
        g_state.DeviceFailure = BlockRefused;
        return false;
    }
    return Await(count);
}

[[nodiscard]] bool IsTransferable(const Foundation::UInt32 count) noexcept
{
    if ((count == 0U) || (count > MaximumSectors))
    {
        g_state.DeviceFailure = BlockOutOfRange;
        return false;
    }
    return true;
}

// The crash a Commit injects. It returns only where the crash does not trap (a hosted test's): the
// instance then ends without answering, and the write is reported failed.
[[nodiscard]] bool Crash() noexcept
{
    g_state.Platform.Crash(g_state.CrashKind);
    End(Abi::ComponentCrashReturnedExitCode);
    return false;
}

// The sector device both backends run over (FsBlockDevice, FatBlockDevice):
// every transfer passes through the bounce page. A write of an AsteriaFS
// superblock sector during a Commit with a crash flag crashes the instance
// just before its request is sent, or just after its BlockDone.
bool ReadSectors(void*, const Foundation::UInt64 sector, const Foundation::UInt32 count,
                 Foundation::UInt8* const buffer) noexcept
{
    if (!IsTransferable(count) || sector > g_state.Sectors || count > g_state.Sectors - sector ||
        !Transfer(BlockReadMethod, sector + g_state.VolumeStart, count, g_state.Bounce))
    {
        return false;
    }
    CopyBytes(buffer, WindowPage(1U), static_cast<Foundation::Size>(count) * SectorBytes);
    return true;
}

bool WriteSectors(void*, const Foundation::UInt64 sector, const Foundation::UInt32 count,
                  const Foundation::UInt8* const buffer) noexcept
{
    if (!IsTransferable(count) || sector > g_state.Sectors || count > g_state.Sectors - sector)
    {
        return false;
    }
    CopyBytes(WindowPage(1U), buffer, static_cast<Foundation::Size>(count) * SectorBytes);
    const bool superblock = (sector == Filesystem::FsSuperblockA) || (sector == Filesystem::FsSuperblockB);
    if (superblock && ((g_state.CrashFlags & FsCrashBeforeSuperblock) != 0U))
    {
        return Crash();
    }
    if (!Transfer(BlockWriteMethod, sector + g_state.VolumeStart, count, g_state.Bounce))
    {
        return false;
    }
    return (superblock && ((g_state.CrashFlags & FsCrashAfterSuperblock) != 0U)) ? Crash() : true;
}

// PowerOff.md: Describe and Flush share their attachment-free data payload,
// transport failure and reply matching; keep one bounded control exchange.
[[nodiscard, gnu::noinline]] bool ControlBlock(const Foundation::UInt32 method,
                                               const Foundation::UInt32 expectedReply) noexcept
{
    ++g_state.Transaction;
    alignas(8) Foundation::UInt8 request[BlockCancelBytes]{};
    const Foundation::UInt64 words[BlockPayloadWords]{g_state.BlockGeneration, 0U, 0U, 0U};
    const auto size = WriteBlockFrame(request, method, g_state.Transaction, words, &g_state.BlockReplies, nullptr);
    if (g_state.Client->Send(g_state.BlockService, System::AddressOf(request), size).Error !=
        Abi::SystemCallError::None)
    {
        g_state.DeviceFailure = BlockRefused;
        return false;
    }
    return Await(0U, expectedReply);
}
[[nodiscard]] bool Synchronize(void*) noexcept
{
    return !g_state.Quiesced || ControlBlock(BlockFlushMethod, BlockReplyMethod);
}

[[nodiscard]] Filesystem::FsBlockDevice AsteriaDevice() noexcept
{
    return Filesystem::FsBlockDevice{.Context = &g_state, .ReadSectors = &ReadSectors, .WriteSectors = &WriteSectors};
}

[[nodiscard]] Filesystem::FatBlockDevice FatDevice() noexcept
{
    return Filesystem::FatBlockDevice{.Context = &g_state,
                                      .ReadSectors = &ReadSectors,
                                      .WriteSectors = &WriteSectors,
                                      .Sectors = g_state.Sectors,
                                      .Synchronize = &Synchronize};
}

[[nodiscard]] Filesystem::ExfatBlockDevice ExfatDevice() noexcept
{
    return Filesystem::ExfatBlockDevice{.Context = &g_state,
                                        .ReadSectors = &ReadSectors,
                                        .WriteSectors = &WriteSectors,
                                        .Sectors = g_state.Sectors,
                                        .Synchronize = &Synchronize};
}

// Mounts FAT with the incarnation as its serial.
void MountFat() noexcept
{
    if (g_state.DeviceFailure == BlockMediumChanged)
    {
        return;
    }
    g_views->InvalidateOwnerSlots(0U);
    g_state.Writer = 0U;
    Scratch().InitializeFat();
    g_fat = &Scratch().FilesystemState().Volume.Fat;
    const FatStatus status = g_fat->Mount(FatDevice(), g_state.Incarnation);
    g_state.Mounted = (status == FatStatus::Done) ? Backend::Fat : Backend::None;
    if (g_fat->IsReadOnly())
    {
        g_state.Writable = false;
    }
    g_state.Unmounted = (status == FatStatus::Failed) ? FsFailed : FsCorrupt;
}

// Mounts exFAT, as FAT, with the incarnation as its serial.
void MountExfat() noexcept
{
    if (g_state.DeviceFailure == BlockMediumChanged)
    {
        return;
    }
    g_views->InvalidateOwnerSlots(0U);
    g_state.Writer = 0U;
    Scratch().InitializeExfat();
    g_exfat = &Scratch().FilesystemState().Volume.Exfat;
    const ExfatStatus status = g_exfat->Mount(ExfatDevice(), g_state.Incarnation);
    g_state.Mounted = (status == ExfatStatus::Done) ? Backend::Exfat : Backend::None;
    if (g_exfat->IsReadOnly())
    {
        g_state.Writable = false;
    }
    g_state.Unmounted = (status == ExfatStatus::Failed) ? FsFailed : FsCorrupt;
}

// Mounts AsteriaFS; with format, a blank volume is formatted, then mounted.
void MountAsteria(const bool format) noexcept
{
    if (g_state.DeviceFailure == BlockMediumChanged)
    {
        return;
    }
    if (g_state.Sectors < Filesystem::FsTotalSectors)
    {
        g_state.Mounted = Backend::None;
        g_state.Unmounted = FsCorrupt;
        return;
    }
    g_views->InvalidateOwnerSlots(0U);
    g_state.Writer = 0U;
    Scratch().InitializeNative();
    FsStatus status = Scratch().Filesystem().Mount(AsteriaDevice());
    if ((status == FsStatus::Blank) && format)
    {
        status = Scratch().Filesystem().Format(AsteriaDevice());
        if (status == FsStatus::Success)
        {
            // The explicit format opt-in is supplied only by the test pager; a newly formatted test
            // volume records it as ASTERIA.RW before a writable v2 view can be installed.
            constexpr Foundation::UInt8 consent[]{'a', 's', 't', 'e', 'r', 'i', 'a', '.', 'r', 'w'};
            status = Scratch().Filesystem().Create(consent, sizeof(consent)).Status;
            if (status == FsStatus::Success)
            {
                status = Scratch().Filesystem().Commit(0U, 1U).Status;
            }
        }
        status = (status == FsStatus::Success) ? Scratch().Filesystem().Mount(AsteriaDevice()) : status;
    }
    g_state.Mounted = (status == FsStatus::Success) ? Backend::Asteria : Backend::None;
    g_state.Unmounted = (status == FsStatus::IoError) ? FsFailed : FsCorrupt;
}

// Sector 0 decides the backend: an exFAT boot sector mounts exFAT, else a FAT parameter block
// mounts FAT, neither AsteriaFS; a corrupt one mounts nothing.
[[gnu::noinline]] void MountVolume() noexcept
{
    g_state.Mounted = Backend::None;
    g_state.Unmounted = FsFailed;
    if (!ControlBlock(BlockDescribeMethod, BlockDescribeReplyMethod) ||
        !Transfer(BlockReadMethod, 0U, 1U, g_state.Bounce))
    {
        return;
    }
    g_state.DeviceSectors = g_state.Sectors;
    const bool mbr = WindowPage(1U)[510U] == 0x55U && WindowPage(1U)[511U] == 0xAAU;
    const bool protective = Filesystem::Partitions::IsProtectiveMbr(WindowPage(1U), g_state.Sectors);
    Filesystem::FatGeometry whole{};
    Filesystem::ExfatGeometry wholeExfat{};
    if (mbr &&
        (protective ||
         (Filesystem::ReadFatGeometry(WindowPage(1U), g_state.Sectors, whole) == FatStatus::NotFat &&
          Filesystem::ReadExfatBootSector(WindowPage(1U), g_state.Sectors, wholeExfat) == ExfatStatus::NotExfat)))
    {
        Scratch().InitializePartitions();
        g_partitions = &Scratch().FilesystemState().Volume.Partitions;
        const auto enumerated = g_partitions->Read(FatDevice());
        if (enumerated != FatStatus::Done)
        {
            g_state.Unmounted = enumerated == FatStatus::Failed ? FsFailed : FsCorrupt;
            return;
        }
        if (g_partitions->Count() == 0U)
        {
            g_state.Unmounted = FsNotFound;
            return;
        }
        Foundation::Size selected{g_partitions->Count()};
        for (Foundation::Size n{0U}; n < g_partitions->Count(); ++n)
        {
            if (g_partitions->Get(n).Esp)
            {
                selected = n;
                break;
            }
        }
        if (selected == g_partitions->Count())
        {
            selected = 0U;
        }
        const auto& partition = g_partitions->Get(selected);
        g_state.VolumeStart = partition.First;
        g_state.Sectors = partition.Sectors;
        if (!Transfer(BlockReadMethod, g_state.VolumeStart, 1U, g_state.Bounce))
        {
            return;
        }
    }
    Filesystem::ExfatGeometry exfat{};
    const ExfatStatus named = Filesystem::ReadExfatBootSector(WindowPage(1U), g_state.Sectors, exfat);
    if (named != ExfatStatus::NotExfat)
    {
        if (named == ExfatStatus::Done)
        {
            MountExfat();
            return;
        }
        g_state.Unmounted = FsCorrupt;
        return;
    }
    Filesystem::FatGeometry geometry{};
    const FatStatus recognized = Filesystem::ReadFatGeometry(WindowPage(1U), g_state.Sectors, geometry);
    if (recognized == FatStatus::Done)
    {
        MountFat();
        return;
    }
    if (recognized == FatStatus::NotFat)
    {
        MountAsteria((g_state.MountFlags & Abi::ComponentMountFormatBlank) != 0U);
        return;
    }
    g_state.Unmounted = FsCorrupt;
}

[[nodiscard, gnu::noinline]] FsAnswer Answer(const Foundation::UInt64 kind, const Foundation::UInt64 detail = 0U,
                                             const Foundation::UInt64 extra = 0U) noexcept
{
    return FsAnswer{.Kind = kind, .Detail = detail, .Extra = extra};
}

[[nodiscard, gnu::noinline]] FsAnswer Refused() noexcept
{
    return Answer(FsRefused, FsMaximumOutstanding);
}

[[nodiscard]] FsAnswer Failed() noexcept
{
    return ((g_state.DeviceFailure == BlockReadOnly) || (g_state.DeviceFailure == BlockUnsupportedSectorSize))
               ? Answer(FsUnsupported)
               : Answer(FsFailed, g_state.DeviceFailure);
}

// What a request is answered while no backend is mounted.
[[nodiscard]] FsAnswer Unmounted() noexcept
{
    return (g_state.Unmounted == FsFailed) ? Failed() : Answer(g_state.Unmounted);
}

// An AsteriaFS status other than success as the file protocol's answer: a name or argument the
// volume cannot take, a full volume and an exhausted generation are refusals; a handle of an object
// that is not a file is stale (the directory, object 0, is not the file protocol's). component
// share status translation instead of duplicating the eight-word reply at every backend
// call within the bounded component image.
[[nodiscard, gnu::noinline]] FsAnswer AnswerOf(const FsStatus status) noexcept
{
    switch (status)
    {
    case FsStatus::NotFound:
        return Answer(FsNotFound);
    case FsStatus::StaleObject:
    case FsStatus::InvalidObject:
        return Answer(FsStaleObject);
    case FsStatus::IoError:
    case FsStatus::NotMounted:
        return Failed();
    case FsStatus::Blank:
    case FsStatus::Corrupt:
        return Answer(FsCorrupt);
    case FsStatus::Success:
    case FsStatus::InvalidName:
    case FsStatus::AlreadyExists:
    case FsStatus::Full:
    case FsStatus::InvalidIndex:
    case FsStatus::GenerationExhausted:
    case FsStatus::Committed:
    case FsStatus::NotCommitted:
        break;
    }
    return Refused();
}

// A FAT status other than Done as the file protocol's answer.
[[nodiscard, gnu::noinline]] FsAnswer AnswerOf(const FatStatus status) noexcept
{
    switch (status)
    {
    case FatStatus::Corrupt:
    case FatStatus::NotFat:
        return Answer(FsCorrupt);
    case FatStatus::StaleObject:
        return Answer(FsStaleObject);
    case FatStatus::NotFound:
        return Answer(FsNotFound);
    case FatStatus::Failed:
    case FatStatus::Crashed:
        return Failed();
    case FatStatus::Unsupported:
        return Answer(FsUnsupported);
    case FatStatus::Done:
    case FatStatus::Refused:
        break;
    }
    return Refused();
}

// An exFAT status other than Done as the file protocol's answer.
[[nodiscard, gnu::noinline]] FsAnswer AnswerOf(const ExfatStatus status) noexcept
{
    switch (status)
    {
    case ExfatStatus::Corrupt:
    case ExfatStatus::NotExfat:
        return Answer(FsCorrupt);
    case ExfatStatus::StaleObject:
        return Answer(FsStaleObject);
    case ExfatStatus::NotFound:
        return Answer(FsNotFound);
    case ExfatStatus::Failed:
    case ExfatStatus::Crashed:
        return Failed();
    case ExfatStatus::Unsupported:
        return Answer(FsUnsupported);
    case ExfatStatus::Done:
    case ExfatStatus::Refused:
        break;
    }
    return Refused();
}

// Maps a client's data Frame at the window's first page for one request;
// false when the kernel refused (not a Frame of a page the service may map).
[[nodiscard, gnu::noinline]] bool MapClient(const System::CapabilityHandle data) noexcept
{
    return System::MapWindow(*g_state.Client, g_state.Space, g_state.ClientMemory, data, g_state.Platform.Window, 1U,
                             g_state.ClientPageOffset) == Abi::SystemCallError::None;
}

// Unmaps it again; a refusal ends the instance, its window page taken.
void UnmapClient() noexcept
{
    const Abi::SystemCallError error = g_state.Client->UnmapMemory(g_state.Space, g_state.Platform.Window, 1U).Error;
    if (error != Abi::SystemCallError::None)
    {
        End(FilesystemFailure(Abi::ComponentFilesystemUnmapStep, error));
    }
}

// A block a file does not hold reads as zeros: written into the client's
// Frame through the window, the only AsteriaFS data the service touches.
[[nodiscard, gnu::noinline]] FsAnswer ReadAbsent(const System::CapabilityHandle data,
                                                 const Foundation::UInt64 index) noexcept
{
    if (!MapClient(data))
    {
        return Refused();
    }
    Foundation::UInt8* const page = WindowPage(0U);
    for (Foundation::Size byte{0U}; byte < PageBytes; ++byte)
    {
        page[byte] = 0U;
    }
    UnmapClient();
    return Answer(FsDone, index);
}

[[nodiscard, gnu::noinline]] FsAnswer StatAsteria(const Filesystem::FsHandle handle) noexcept
{
    const Filesystem::FsStatResult stat = Scratch().Filesystem().Stat(handle);
    if (stat.Status != FsStatus::Success)
    {
        return AnswerOf(stat.Status);
    }
    if (stat.Object.Kind != Filesystem::FsObjectKind::File)
    {
        return Answer(FsStaleObject);
    }
    return Answer(FsDone, stat.Object.SizeBytes / PageBytes, stat.Object.SizeBytes);
}

// The block the object table names is read straight into the client's Frame.
[[nodiscard, gnu::noinline]] FsAnswer ReadAsteria(const Filesystem::FsHandle handle, const Foundation::UInt64 index,
                                                  const System::CapabilityHandle data) noexcept
{
    const Filesystem::FsStatResult stat = Scratch().Filesystem().Stat(handle);
    if (stat.Status != FsStatus::Success)
    {
        return AnswerOf(stat.Status);
    }
    if (stat.Object.Kind != Filesystem::FsObjectKind::File)
    {
        return Answer(FsStaleObject);
    }
    const Foundation::UInt64 block = stat.Object.Direct[index];
    if (block == 0U)
    {
        return ReadAbsent(data, index);
    }
    const auto target = g_state.ClientPageOffset == 0U ? data : g_state.Bounce;
    if (!Transfer(BlockReadMethod, g_state.VolumeStart + block * Filesystem::FsSectorsPerBlock, MaximumSectors, target))
    {
        return Failed();
    }
    if (g_state.ClientPageOffset != 0U)
    {
        if (!MapClient(data))
        {
            return Refused();
        }
        CopyBytes(WindowPage(0U), WindowPage(1U), PageBytes);
        UnmapClient();
    }
    return Answer(FsDone, index);
}

// A fresh block is allocated and the client's Frame written to it. A failed write leaves the block
// staged without its data, so the volume is mounted again, which drops what was staged.
[[nodiscard, gnu::noinline]] FsAnswer WriteAsteria(const Filesystem::FsHandle handle, const Foundation::UInt64 index,
                                                   const System::CapabilityHandle data) noexcept
{
    const Filesystem::FsAllocationResult allocated = Scratch().Filesystem().AllocateData(handle, index);
    if (allocated.Status != FsStatus::Success)
    {
        return AnswerOf(allocated.Status);
    }
    const Foundation::UInt64 sector = static_cast<Foundation::UInt64>(allocated.Block) * Filesystem::FsSectorsPerBlock;
    if (Transfer(BlockWriteMethod, g_state.VolumeStart + sector, MaximumSectors, data))
    {
        return Answer(FsDone, index);
    }
    const FsAnswer failed = Failed();
    if (g_state.Exit == 0U)
    {
        MountAsteria(false);
    }
    return failed;
}

// The transaction is committed for the sender; the crash flags are armed for
// this Commit only. A Commit that failed fenced the volume, which is mounted
// again so that QueryCommit stays definite.
[[nodiscard, gnu::noinline]] FsAnswer CommitAsteria(const Foundation::UInt64 sender,
                                                    const Foundation::UInt64 transaction,
                                                    const Foundation::UInt64 flags) noexcept
{
    g_state.CrashFlags = flags;
    const Filesystem::FsCommitResult committed = Scratch().Filesystem().Commit(sender, transaction);
    g_state.CrashFlags = 0U;
    if (committed.Status == FsStatus::Success)
    {
        return Answer(FsCommitted, committed.Generation);
    }
    const FsAnswer failed = AnswerOf(committed.Status);
    if (g_state.Exit == 0U)
    {
        MountAsteria(false);
    }
    return failed;
}

[[nodiscard]] FsAnswer Queried(const Filesystem::FsCommitResult& result) noexcept
{
    if (result.Status == FsStatus::Committed)
    {
        return Answer(FsCommitted, result.Generation);
    }
    return (result.Status == FsStatus::NotCommitted) ? Answer(FsNotCommitted) : AnswerOf(result.Status);
}

[[nodiscard, gnu::noinline]] FsAnswer ServeAsteria(const FsMessage& message, const Foundation::UInt64 sender) noexcept
{
    const Filesystem::FsHandle handle{.ObjectId = message.Words[1U], .BirthGeneration = message.Words[2U]};
    switch (message.Method)
    {
    case FsReadMethod:
        return ReadAsteria(handle, message.Words[3U], message.Data);
    case FsWriteMethod:
        return WriteAsteria(handle, message.Words[3U], message.Data);
    case FsCommitMethod:
        return CommitAsteria(sender, message.Words[1U], message.Words[2U]);
    case FsQueryCommitMethod:
        return Queried(Scratch().Filesystem().QueryCommit(sender, message.Words[1U]));
    case FsStatMethod:
        return StatAsteria(handle);
    default:
        break;
    }
    return Refused();
}

// A FAT page passes through the window's first page, mapped for this request only.
[[nodiscard, gnu::noinline]] FatStatus TransferFat(const Filesystem::FatObject& object, const Foundation::UInt64 index,
                                                   const System::CapabilityHandle data, const bool write) noexcept
{
    if (!MapClient(data))
    {
        return FatStatus::Refused;
    }
    Foundation::UInt8* const page = WindowPage(0U);
    const FatStatus status = write ? g_fat->WritePage(object, index, page) : g_fat->ReadPage(object, index, page);
    UnmapClient();
    return status;
}

// The crash flags stop the commit before or after its directory-entry write.
[[nodiscard, gnu::noinline]] FatStatus CommitFat(const Foundation::UInt64 flags) noexcept
{
    const Filesystem::FatCrashPoint point =
        ((flags & FsCrashBeforeSuperblock) != 0U)
            ? Filesystem::FatCrashPoint::BeforeDirectoryEntry
            : (((flags & FsCrashAfterSuperblock) != 0U) ? Filesystem::FatCrashPoint::AfterDirectoryEntry
                                                        : Filesystem::FatCrashPoint::None);
    const FatStatus status = g_fat->Commit(point);
    if (status == FatStatus::Crashed)
    {
        static_cast<void>(Crash());
    }
    return status;
}

// A FAT device failure breaks the volume until it mounts again, which the
// service does at once; a dirty volume then mounts read-only.
[[nodiscard, gnu::noinline]] FsAnswer ServeFat(const FsMessage& message) noexcept
{
    const Filesystem::FatObject object{.Identifier = message.Words[1U], .Generation = message.Words[2U]};
    Filesystem::FatFileSize size{};
    FatStatus status{FatStatus::Refused};
    FsAnswer done = Answer(FsDone, message.Words[3U]);
    switch (message.Method)
    {
    case FsReadMethod:
    case FsWriteMethod:
        status = TransferFat(object, message.Words[3U], message.Data, message.Method == FsWriteMethod);
        break;
    case FsCommitMethod:
        status = CommitFat(message.Words[2U]);
        done = Answer(FsDone);
        break;
    case FsQueryCommitMethod:
        // No transaction record: the answer names the format.
        status = Filesystem::FatVolume::QueryCommit();
        if (status == FatStatus::Unsupported)
        {
            return Answer(FsUnsupported, FsFormatFat);
        }
        break;
    case FsStatMethod:
        status = g_fat->Stat(object, size);
        done = Answer(FsDone, size.Pages, size.Bytes);
        break;
    default:
        break;
    }
    if (status == FatStatus::Done)
    {
        return done;
    }
    const FsAnswer failed = AnswerOf(status);
    if ((status == FatStatus::Failed) && (g_state.Exit == 0U))
    {
        MountFat();
    }
    return failed;
}

// An exFAT page passes through the window's first page, as a FAT page does.
[[nodiscard, gnu::noinline]] ExfatStatus TransferExfat(const Filesystem::ExfatObject& object,
                                                       const Foundation::UInt64 index,
                                                       const System::CapabilityHandle data, const bool write) noexcept
{
    if (!MapClient(data))
    {
        return ExfatStatus::Refused;
    }
    Foundation::UInt8* const page = WindowPage(0U);
    const ExfatStatus status = write ? g_exfat->WritePage(object, index, page) : g_exfat->ReadPage(object, index, page);
    UnmapClient();
    return status;
}

// The crash flags stop the commit before or after its entry set's write.
[[nodiscard, gnu::noinline]] ExfatStatus CommitExfat(const Foundation::UInt64 flags) noexcept
{
    const Filesystem::ExfatCrashPoint point =
        ((flags & FsCrashBeforeSuperblock) != 0U)
            ? Filesystem::ExfatCrashPoint::BeforeDirectoryEntry
            : (((flags & FsCrashAfterSuperblock) != 0U) ? Filesystem::ExfatCrashPoint::AfterDirectoryEntry
                                                        : Filesystem::ExfatCrashPoint::None);
    const ExfatStatus status = g_exfat->Commit(point);
    if (status == ExfatStatus::Crashed)
    {
        static_cast<void>(Crash());
    }
    return status;
}

// As ServeFat: a device failure breaks the volume until it mounts again,
// which the service does at once; a dirty volume then mounts read-only.
[[nodiscard, gnu::noinline]] FsAnswer ServeExfat(const FsMessage& message, const FsNode& node) noexcept
{
    const Filesystem::ExfatObject object{.Identifier = node.Object,
                                         .Generation = node.Birth,
                                         .ParentBytes = node.ParentBytes,
                                         .ParentContiguous = node.ParentContiguous};
    Filesystem::ExfatFileSize size{};
    ExfatStatus status{ExfatStatus::Refused};
    FsAnswer done = Answer(FsDone, message.Words[3U]);
    switch (message.Method)
    {
    case FsReadMethod:
    case FsWriteMethod:
        status = TransferExfat(object, message.Words[3U], message.Data, message.Method == FsWriteMethod);
        break;
    case FsCommitMethod:
        status = CommitExfat(message.Words[2U]);
        done = Answer(FsDone);
        break;
    case FsQueryCommitMethod:
        // No transaction record: the answer names the format.
        status = Filesystem::ExfatVolume::QueryCommit();
        if (status == ExfatStatus::Unsupported)
        {
            return Answer(FsUnsupported, FsFormatExfat);
        }
        break;
    case FsStatMethod:
        status = g_exfat->Stat(object, size);
        done = Answer(FsDone, size.Pages, size.Bytes);
        break;
    default:
        break;
    }
    if (status == ExfatStatus::Done)
    {
        return done;
    }
    const FsAnswer failed = AnswerOf(status);
    if ((status == ExfatStatus::Failed) && (g_state.Exit == 0U))
    {
        MountExfat();
    }
    return failed;
}

[[nodiscard]] FsNode VolumeRoot() noexcept
{
    return FsNode{.Object = g_state.Mounted == Backend::Asteria ? 0U : Filesystem::RootIdentifier,
                  .Birth = g_state.Mounted == Backend::Asteria ? 1U : g_state.Incarnation,
                  .Kind = Filesystem::NodeKind::Directory};
}
// ASTERIA.OK grants no write authority; writable views require ASTERIA.RW.
void CheckWriteConsent() noexcept
{
    constexpr Foundation::UInt8 name[]{'A', 'S', 'T', 'E', 'R', 'I', 'A', '.', 'R', 'W'};
    bool consent{false};
    if (g_state.Mounted == Backend::Fat)
    {
        Filesystem::FatObject object{};
        Filesystem::FatFileSize size{};
        consent = g_fat->Open(name, sizeof(name), object) == FatStatus::Done &&
                  g_fat->Stat(object, size) == FatStatus::Done && size.Kind == Filesystem::NodeKind::File &&
                  !g_fat->IsReadOnly();
    }
    else if (g_state.Mounted == Backend::Exfat)
    {
        Filesystem::ExfatObject object{};
        Filesystem::ExfatFileSize size{};
        consent = g_exfat->Open(name, sizeof(name), object) == ExfatStatus::Done &&
                  g_exfat->Stat(object, size) == ExfatStatus::Done && size.Kind == Filesystem::NodeKind::File &&
                  !g_exfat->IsReadOnly();
    }
    else if (g_state.Mounted == Backend::Asteria)
    {
        constexpr Foundation::UInt8 native[]{'a', 's', 't', 'e', 'r', 'i', 'a', '.', 'r', 'w'};
        consent = Scratch().Filesystem().Lookup(native, sizeof(native)).Status == FsStatus::Success;
    }
    g_state.Writable = consent && (g_state.MediumFlags & BlockReadOnly) == 0U;
}
void AbortWriter() noexcept
{
    if (g_state.Writer == 0U)
    {
        return;
    }
    if (g_state.Mounted == Backend::Fat)
    {
        static_cast<void>(g_fat->Abort());
    }
    else if (g_state.Mounted == Backend::Exfat)
    {
        static_cast<void>(g_exfat->Abort());
    }
    else if (g_state.Mounted == Backend::Asteria)
    {
        MountAsteria(false);
    }
    g_views->InvalidateOwnerSlots(g_state.Writer);
    g_state.Writer = 0U;
}
// PowerOff.md: the root manager commits the stopped writer's staging,
// then acknowledges only durable storage. Admission remains closed on failure.
[[nodiscard, gnu::noinline]] FsAnswer Quiesce() noexcept
{
    g_state.Quiesced = true;
    if (g_state.Mounted == Backend::Fat)
    {
        return g_fat->Commit(Filesystem::FatCrashPoint::None) == Filesystem::FatStatus::Done ? Answer(FsDone)
                                                                                             : Failed();
    }
    if (g_state.Mounted == Backend::Exfat)
    {
        return g_exfat->Commit(Filesystem::ExfatCrashPoint::None) == Filesystem::ExfatStatus::Done ? Answer(FsDone)
                                                                                                   : Failed();
    }
    const auto committed = Scratch().Filesystem().CommitStaged();
    if (committed.Status != FsStatus::Success)
    {
        return AnswerOf(committed.Status);
    }
    g_state.Writer = 0U;
    return Synchronize(nullptr) && committed.Generation == Scratch().Filesystem().GetGeneration() ? Answer(FsDone)
                                                                                                  : Failed();
}
[[nodiscard, gnu::noinline]] FsAnswer ManageViews(const FsMessage& message, const Foundation::UInt64 sender) noexcept
{
    if (message.Words[0U] != g_state.Incarnation)
    {
        return Answer(FsRestarted);
    }
    if (g_state.Mounted == Backend::None)
    {
        return Unmounted();
    }
    if (g_state.Quiesced)
    {
        return Refused();
    }
    if (message.Method == FsQuiesceMethod)
    {
        return Quiesce();
    }
    if (message.Method == FsResetViewsMethod)
    {
        AbortWriter();
        g_views->Reset();
        return Answer(FsDone);
    }
    if (message.Method == FsRemoveViewMethod)
    {
        auto* view = g_views->ManagerFind(message.Words[4U]);
        if (view == nullptr)
        {
            return Answer(FsStaleObject);
        }
        if (view->Owner == g_state.Writer)
        {
            AbortWriter();
        }
        view->Id = 0U;
        return Answer(FsDone);
    }
    if (message.Method != FsInstallViewMethod || g_state.Writer != 0U)
    {
        return Refused();
    }
    FsNode root = VolumeRoot();
    const auto rights = message.Words[5U];
    if (message.Words[4U] != 0U)
    {
        auto* source = g_views->ManagerFind(message.Words[4U]);
        auto* slot = source == nullptr ? nullptr : g_views->Resolve(*source, message.Words[2U], message.Words[3U]);
        if (slot == nullptr)
        {
            return Answer(FsStaleObject);
        }
        if ((rights & ~source->Rights) != 0U)
        {
            return Answer(FsRightsDenied);
        }
        root = slot->Node;
    }
    else if (message.Words[2U] != 0U || message.Words[3U] != 0U)
    {
        return Refused();
    }
    if ((rights & FsWriteRight) != 0U && !g_state.Writable)
    {
        return Answer(FsRightsDenied);
    }
    auto* view = g_views->Install(message.Words[1U] == 0U ? sender : message.Words[1U], rights, root);
    return view == nullptr ? Refused() : Answer(FsDone, view->Id, view->Slots[0U].Generation);
}
[[nodiscard]] FsAnswer BackendRequest(const FsMessage& message, const FsNode& node,
                                      const Foundation::UInt64 sender) noexcept
{
    g_backendRequest = message;
    if (message.Method != FsCommitMethod && message.Method != FsQueryCommitMethod)
    {
        g_backendRequest.Words[1U] = node.Object;
        g_backendRequest.Words[2U] = node.Birth;
    }
    switch (g_state.Mounted)
    {
    case Backend::Asteria:
        if ((message.Method == FsReadMethod || message.Method == FsWriteMethod) &&
            message.Words[3U] >= Filesystem::FsDirectCapacity)
        {
            return Refused();
        }
        return ServeAsteria(g_backendRequest, sender);
    case Backend::Fat:
        return ServeFat(g_backendRequest);
    case Backend::Exfat:
        return ServeExfat(g_backendRequest, node);
    case Backend::None:
        break;
    }
    return Unmounted();
}
// one Frame may carry several pages, without growing the two-page virtual window
// or taking a large target stack. Mapping offsets are checked by the kernel against both the Frame
// and the MemoryObject.
[[nodiscard, gnu::noinline]] FsAnswer ReadPages(const FsMessage& message, const FsNode& node,
                                                const Foundation::UInt64 sender) noexcept
{
    const auto count = message.Words[7U];
    if (g_state.Mounted == Backend::Asteria &&
        (message.Words[3U] >= Filesystem::FsDirectCapacity || count > Filesystem::FsDirectCapacity - message.Words[3U]))
    {
        return Refused();
    }
    g_pagesRequest = message;
    FsAnswer answer{.Kind = FsRefused};
    for (Foundation::UInt64 page{0U}; page < count; ++page)
    {
        g_state.ClientPageOffset = page;
        g_pagesRequest.Words[3U] = message.Words[3U] + page;
        answer = BackendRequest(g_pagesRequest, node, sender);
        if (answer.Kind != FsDone)
        {
            break;
        }
    }
    g_state.ClientPageOffset = 0U;
    return answer.Kind == FsDone && count > 1U ? Answer(FsDone, count) : answer;
}
[[nodiscard, gnu::noinline]] FsAnswer OpenNode(const FsMessage& message, const FsNode& parent, FsNode& node) noexcept
{
    const bool create = message.Method == FsCreateMethod || message.Method == FsMakeDirectoryMethod;
    const bool directory = message.Method == FsMakeDirectoryMethod;
    if (g_state.Mounted == Backend::Fat)
    {
        Filesystem::FatObject object{};
        const Filesystem::FatObject dir{parent.Object, parent.Birth};
        const auto status = create ? g_fat->CreateIn(dir, message.Name, message.NameLength, object, directory)
                                   : g_fat->Lookup(dir, message.Name, message.NameLength, object);
        if (status != FatStatus::Done)
        {
            return AnswerOf(status);
        }
        Filesystem::FatFileSize size{};
        const auto stated = g_fat->Stat(object, size);
        if (stated != FatStatus::Done)
        {
            return AnswerOf(stated);
        }
        node = {.Object = object.Identifier, .Birth = object.Generation, .Kind = size.Kind};
    }
    else if (g_state.Mounted == Backend::Exfat)
    {
        Filesystem::ExfatObject object{};
        const Filesystem::ExfatObject dir{parent.Object, parent.Birth, parent.ParentBytes, parent.ParentContiguous};
        const auto status = create ? g_exfat->CreateIn(dir, message.Name, message.NameLength, object, directory)
                                   : g_exfat->Lookup(dir, message.Name, message.NameLength, object);
        if (status != ExfatStatus::Done)
        {
            return AnswerOf(status);
        }
        Filesystem::ExfatFileSize size{};
        const auto stated = g_exfat->Stat(object, size);
        if (stated != ExfatStatus::Done)
        {
            return AnswerOf(stated);
        }
        node = {.Object = object.Identifier,
                .Birth = object.Generation,
                .ParentBytes = object.ParentBytes,
                .Kind = size.Kind,
                .ParentContiguous = object.ParentContiguous};
    }
    else
    {
        if (directory)
        {
            return Answer(FsUnsupported);
        } // AsteriaFS directory operations are unsupported.
        const auto result = create ? Scratch().Filesystem().Create(message.Name, message.NameLength)
                                   : Scratch().Filesystem().Lookup(message.Name, message.NameLength);
        if (result.Status != FsStatus::Success)
        {
            return AnswerOf(result.Status);
        }
        node = {.Object = result.Handle.ObjectId,
                .Birth = result.Handle.BirthGeneration,
                .Kind = Filesystem::NodeKind::File};
    }
    return Answer(FsDone);
}
[[nodiscard, gnu::noinline]] FsAnswer Enumerate(const FsMessage& message, const FsNode& directory) noexcept
{
    Foundation::Size length{0U};
    Foundation::UInt64 cursor = message.Words[3U];
    FsNode node{};
    if (g_state.Mounted == Backend::Fat)
    {
        Filesystem::FatObject object{};
        Filesystem::FatFileSize size{};
        const auto status = g_fat->ReadDirectory({directory.Object, directory.Birth}, cursor, g_name, length, object);
        if (status != FatStatus::Done)
        {
            return AnswerOf(status);
        }
        const auto stated = g_fat->Stat(object, size);
        if (stated != FatStatus::Done)
        {
            return AnswerOf(stated);
        }
        node = {.Object = object.Identifier, .Birth = object.Generation, .Kind = size.Kind};
    }
    else if (g_state.Mounted == Backend::Exfat)
    {
        Filesystem::ExfatObject object{};
        Filesystem::ExfatFileSize size{};
        const auto status = g_exfat->ReadDirectory(
            {directory.Object, directory.Birth, directory.ParentBytes, directory.ParentContiguous}, cursor, g_name,
            length, object);
        if (status != ExfatStatus::Done)
        {
            return AnswerOf(status);
        }
        const auto stated = g_exfat->Stat(object, size);
        if (stated != ExfatStatus::Done)
        {
            return AnswerOf(stated);
        }
        node = {.Object = object.Identifier, .Birth = object.Generation, .Kind = size.Kind};
    }
    else
    {
        Filesystem::FsHandle handle{};
        const auto status = Scratch().Filesystem().ReadDirectory(cursor, g_name, length, handle);
        if (status != FsStatus::Success)
        {
            return AnswerOf(status);
        }
        node = {.Object = handle.ObjectId, .Birth = handle.BirthGeneration, .Kind = Filesystem::NodeKind::File};
    }
    if (!MapClient(message.Data))
    {
        return Refused();
    }
    for (Foundation::Size at{0U}; at < PageBytes; ++at)
    {
        WindowPage(0U)[at] = at < length ? g_name[at] : Foundation::UInt8{0U};
    }
    UnmapClient();
    return FsAnswer{.Kind = FsDone,
                    .Detail = cursor,
                    .Extra = length,
                    .NodeKind = static_cast<Foundation::UInt64>(node.Kind),
                    .Object = node.Object,
                    .Generation = node.Birth};
}
[[nodiscard, gnu::noinline]] FsAnswer Execute(const FsMessage& message, const Foundation::UInt64 sender) noexcept
{
    if (message.Method == FsQuiesceMethod)
    {
        return Answer(FsRightsDenied);
    }
    if (g_state.Quiesced)
    {
        return Refused();
    }
    if (message.Words[0U] != g_state.Incarnation)
    {
        return Answer(FsRestarted);
    }
    if (g_state.Mounted == Backend::None)
    {
        return Unmounted();
    }
    auto* view = g_views->Find(message.Words[4U], sender);
    if (view == nullptr)
    {
        return Answer(FsRightsDenied);
    }
    const bool named = Detail::HasFsName(message.Method);
    const bool transaction = message.Method == FsCommitMethod || message.Method == FsQueryCommitMethod;
    const Foundation::UInt64 index = named || transaction ? message.Words[5U] : message.Words[1U];
    const Foundation::UInt64 generation = named || transaction ? message.Words[6U] : message.Words[2U];
    auto* slot = g_views->Resolve(*view, index, generation);
    if (slot == nullptr)
    {
        return Answer(FsStaleObject);
    }
    const bool mutation = message.Method == FsCreateMethod || message.Method == FsMakeDirectoryMethod ||
                          message.Method == FsWriteMethod || message.Method == FsCommitMethod;
    if ((mutation && ((view->Rights & FsWriteRight) == 0U || !g_state.Writable)) ||
        (!mutation && message.Method != FsCloseMethod && (view->Rights & FsReadRight) == 0U))
    {
        return Answer(FsRightsDenied);
    }
    // Cross-process visibility is reserved. A staged transaction is private
    // to its writer; no second identity can observe or commit its staging.
    if (g_state.Writer != 0U && g_state.Writer != sender)
    {
        return Refused();
    }
    if (message.Method == FsCloseMethod)
    {
        g_views->Close(*view, index);
        return Answer(FsDone);
    }
    if (named || message.Method == FsReadDirectoryMethod)
    {
        if (slot->Node.Kind != Filesystem::NodeKind::Directory)
        {
            return Refused();
        }
        if (message.Method == FsReadDirectoryMethod)
        {
            return Enumerate(message, slot->Node);
        }
        if (message.NameLength == 2U && message.Name[0U] == '.' && message.Name[1U] == '.')
        {
            if (mutation)
            {
                return Refused();
            }
            const auto parent = g_views->Parent(*view, index);
            return parent == FsOpenSlots
                       ? Refused()
                       : FsAnswer{.Kind = FsDone,
                                  .Detail = parent,
                                  .Extra = view->Slots[parent].Generation,
                                  .NodeKind = static_cast<Foundation::UInt64>(Filesystem::NodeKind::Directory)};
        }
        if (!g_views->HasFree(*view))
        {
            return Refused();
        }
        FsNode node{};
        const auto answer = OpenNode(message, slot->Node, node);
        if (answer.Kind != FsDone)
        {
            return answer;
        }
        const auto opened = g_views->Open(*view, node, index);
        if (opened == FsOpenSlots)
        {
            return Refused();
        }
        if (mutation)
        {
            g_state.Writer = sender;
        }
        return FsAnswer{.Kind = FsDone,
                        .Detail = opened,
                        .Extra = view->Slots[opened].Generation,
                        .NodeKind = static_cast<Foundation::UInt64>(node.Kind),
                        .Object = node.Object,
                        .Generation = node.Birth};
    }
    if (message.Method == FsStatMethod && slot->Node.Kind == Filesystem::NodeKind::Directory &&
        (slot->Node.Object == Filesystem::RootIdentifier || g_state.Mounted == Backend::Asteria))
    {
        return FsAnswer{.Kind = FsDone,
                        .NodeKind = static_cast<Foundation::UInt64>(slot->Node.Kind),
                        .Object = slot->Node.Object,
                        .Generation = slot->Node.Birth};
    }
    if ((message.Method == FsReadMethod || message.Method == FsWriteMethod) &&
        slot->Node.Kind != Filesystem::NodeKind::File)
    {
        return Refused();
    }
    if (message.Method == FsReadMethod)
    {
        return ReadPages(message, slot->Node, sender);
    }
    const auto before = g_state.Mounted == Backend::Asteria ? Scratch().Filesystem().GetGeneration() : 0U;
    auto answer = BackendRequest(message, slot->Node, sender);
    if (message.Method == FsStatMethod && answer.Kind == FsDone)
    {
        answer.NodeKind = static_cast<Foundation::UInt64>(slot->Node.Kind);
        answer.Size = answer.Extra;
        answer.Object = slot->Node.Object;
        answer.Generation = slot->Node.Birth;
    }
    if (answer.Kind == FsDone || answer.Kind == FsCommitted)
    {
        if (message.Method == FsWriteMethod)
        {
            g_state.Writer = sender;
        }
        // A retained reply does not publish staging.
        if (message.Method == FsCommitMethod &&
            (g_state.Mounted != Backend::Asteria || Scratch().Filesystem().GetGeneration() > before))
        {
            g_state.Writer = 0U;
        }
    }
    return answer;
}

// Sends the answer on the request's reply capability (a reply the kernel
// refuses is dropped and counted), then closes that and a Read's or Write's
// data Frame. False only when a Close failed other than on a stale handle.
[[nodiscard, gnu::noinline]] bool Respond(const FsMessage& message, const FsAnswer& answer) noexcept
{
    const System::SystemCallClient& client = *g_state.Client;
    alignas(8) Foundation::UInt8 frame[FsReplyBytes]{};
    const Foundation::UInt64 words[FsPayloadWords]{message.Transaction, answer.Kind, answer.Detail, answer.Extra,
                                                   answer.NodeKind,     answer.Size, answer.Object, answer.Generation};
    const Foundation::Size bytes =
        WriteFsFrame(frame, FsReplyMethod, message.Transaction, words, nullptr, 0U, nullptr, nullptr);
    if (client.Send(message.Reply, System::AddressOf(frame), bytes).Error != Abi::SystemCallError::None)
    {
        ++g_state.DroppedReplies;
    }
    const bool replyClosed = Closed(client, message.Reply);
    const bool data = (message.Method == FsReadMethod) || (message.Method == FsWriteMethod) ||
                      (message.Method == FsReadDirectoryMethod);
    return (!data || Closed(client, message.Data)) && replyClosed;
}

// One delivery of deliveryBytes. A frame that is no request of the file
// protocol through the sealed Endpoint is dropped, every capability it
// carried closed: its reply capability cannot be trusted. Zero, or the
// instance's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 ServeDelivery(const Foundation::Size deliveryBytes) noexcept
{
    FsMessage message{};
    if (!ReadFsFrame(g_request, deliveryBytes, message) || (message.Method == FsReplyMethod) ||
        ((ReadEchoSealedSelector(g_request) != FsSealedSelector) &&
         (ReadEchoSealedSelector(g_request) != FsManagerSelector)))
    {
        return CloseAttachments(*g_state.Client, g_request, 0U)
                   ? 0U
                   : FilesystemFailure(Abi::ComponentFilesystemCloseStep, Abi::SystemCallError::None);
    }
    const auto sender = System::LoadWord(g_request + DeliverySenderOffset);
    const bool manager = ReadEchoSealedSelector(g_request) == FsManagerSelector;
    const auto rights = static_cast<Foundation::UInt16>(g_request[14U] | (g_request[15U] << 8U));
    const FsAnswer answer = manager ? (((message.Method == FsResetViewsMethod || message.Method == FsQuiesceMethod) &&
                                        (rights & EchoManageRight) == 0U)
                                           ? Answer(FsRightsDenied)
                                           : ManageViews(message, sender))
                                    : Execute(message, sender);
    if (g_state.Exit != 0U)
    {
        return g_state.Exit;
    }
    return Respond(message, answer) ? 0U
                                    : FilesystemFailure(Abi::ComponentFilesystemCloseStep, Abi::SystemCallError::None);
}

// Receives and serves every request the Endpoint holds, one at a time, in
// order. Zero, or the instance's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64 DrainRequests(const System::CapabilityHandle endpoint) noexcept
{
    for (;;)
    {
        const Abi::SystemCallResponse received =
            g_state.Client->Receive(endpoint, System::AddressOf(g_request), sizeof(g_request));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            return 0U;
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            return FilesystemFailure(Abi::ComponentFilesystemReceiveStep, received.Error);
        }
        const Foundation::UInt64 exit = ServeDelivery(static_cast<Foundation::Size>(received.Value1));
        if (exit != 0U)
        {
            return exit;
        }
    }
}

// The window : the bounce Frame, created in the instance's own domain and mapped for good at the
// second page, and the MemoryObject the first page maps clients' Frames through. Zero, or the
// failure's exit code.
[[nodiscard, gnu::noinline]] Foundation::UInt64
PrepareWindow(const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords]) noexcept
{
    if (startBlock[Abi::ComponentWindowPagesWord] < 2U)
    {
        return FilesystemFailure(Abi::ComponentFilesystemWindowStep, Abi::SystemCallError::None);
    }
    const System::SystemCallClient& client = *g_state.Client;
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const Abi::SystemCallResponse frame = System::CreateMemory(client, domain, Abi::ObjectTypeFrame, 1U);
    if (frame.Error != Abi::SystemCallError::None)
    {
        return FilesystemFailure(Abi::ComponentFilesystemFrameStep, frame.Error);
    }
    g_state.Bounce = System::CapabilityHandle{frame.Value0, frame.Value1};
    const Abi::SystemCallResponse bounce = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, 1U);
    const Abi::SystemCallResponse window =
        (bounce.Error == Abi::SystemCallError::None)
            ? System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, FsReadPagesCapacity)
            : bounce;
    if (window.Error != Abi::SystemCallError::None)
    {
        return FilesystemFailure(Abi::ComponentFilesystemMemoryObjectStep, window.Error);
    }
    g_state.ClientMemory = System::CapabilityHandle{window.Value0, window.Value1};
    const Abi::SystemCallError mapped =
        System::MapWindow(client, g_state.Space, System::CapabilityHandle{bounce.Value0, bounce.Value1}, g_state.Bounce,
                          g_state.Platform.Window + PageBytes, 1U);
    return (mapped == Abi::SystemCallError::None) ? 0U : FilesystemFailure(Abi::ComponentFilesystemMapStep, mapped);
}
} // Namespace

Foundation::UInt64 ServeFilesystem(const System::SystemCallClient& client,
                                   const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                   const FilesystemPlatform& platform) noexcept
{
    Scratch().InitializeFilesystem();
    g_views = &Scratch().FilesystemState().Views;
    g_views->Initialize();
    g_state = ServiceState{
        .Client = &client,
        .Platform = platform,
        .Endpoint = {startBlock[Abi::ComponentReceiveWord], startBlock[Abi::ComponentReceiveWord + 1U]},
        .WaitSet = {startBlock[Abi::ComponentWaitSetWord], startBlock[Abi::ComponentWaitSetWord + 1U]},
        .Manager = {startBlock[Abi::ComponentFsManagerWord], startBlock[Abi::ComponentFsManagerWord + 1U]},
        .Space = {startBlock[Abi::ComponentAccountSpaceWord], startBlock[Abi::ComponentAccountSpaceWord + 1U]},
        .BlockService = {startBlock[Abi::ComponentBlockServiceWord], startBlock[Abi::ComponentBlockServiceWord + 1U]},
        .BlockReplies = {startBlock[Abi::ComponentBlockReplyWord], startBlock[Abi::ComponentBlockReplyWord + 1U]},
        .Bounce = {},
        .ClientMemory = {},
        .Incarnation = startBlock[Abi::ComponentIncarnationWord],
        .CrashKind = startBlock[Abi::ComponentCrashKindWord],
        .BlockGeneration = startBlock[Abi::ComponentBlockGenerationWord],
        .MediumGeneration = 0U,
        .Sectors = 0U,
        .MediumFlags = 0U,
        .MountFlags = startBlock[Abi::ComponentMountFlagsWord],
        .Transaction = 0U,
        .DeviceFailure = 0U,
        .CrashFlags = 0U,
        .Unmounted = FsFailed,
        .DroppedReplies = 0U,
        .Exit = 0U,
        .Mounted = Backend::None};
    const Foundation::UInt64 prepared = PrepareWindow(startBlock);
    if (prepared != 0U)
    {
        return prepared;
    }
    MountVolume();
    CheckWriteConsent();
    // Every request queued is served before the instance waits: a request
    // whose readiness a block transfer's wait consumed (during the mount or
    // another request) would otherwise wait for a wake that never comes.
    for (;;)
    {
        End(DrainRequests(g_state.Manager));
        End(DrainRequests(g_state.Endpoint));
        if (g_state.Exit != 0U)
        {
            return g_state.Exit;
        }
        const Abi::SystemCallError waited = client.Wait(g_state.WaitSet).Error;
        if (waited != Abi::SystemCallError::None)
        {
            return FilesystemFailure(Abi::ComponentFilesystemWaitStep, waited);
        }
    }
}
} // Namespace Asteria::User::Component
