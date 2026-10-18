// SPDX-License-Identifier: Apache-2.0
#include "User/Component/FilesystemService.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/BlockFrame.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Component/FsFrame.hpp"
#include "User/Filesystem/FsFormat.hpp"

#include "Support/TestContext.hpp"
#include "User/Filesystem/Fat/FatTestSupport.hpp"

#include <cstring>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt16;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
namespace Component = User::Component;
using Abi::SystemCallError;
using Abi::SystemCallNumber;
using User::System::CapabilityHandle;

constexpr Size Header{Component::EchoDeliveryHeaderBytes};
constexpr Size DeliveryCapacity{Header + Component::EchoMaximumFrameBytes};
constexpr Size PageBytes{4096U};
constexpr Size SectorBytes{512U};
constexpr UInt64 DiskSectors{4096U};
// The start block's handles, the driver instance's tag, the client's sender
// identity and the crash kind.
constexpr UInt64 Account{0x10U};
constexpr UInt64 Space{0x20U};
constexpr UInt64 BlockService{0x34U};
constexpr UInt64 BlockReply{0x38U};
constexpr UInt64 WaitSet{0x60U};
constexpr UInt64 Endpoint{0x80U};
constexpr UInt64 DriverTag{7U};
constexpr UInt64 Sender{0x5100U};
constexpr UInt64 CrashKind{1U};
// A request's reply capability is FirstReply + its transaction; a Read's or
// Write's data Frame FirstPage + the client page it names.
constexpr UInt64 FirstReply{0x7000U};
constexpr UInt64 FirstPage{0x8000U};
constexpr Size ClientPages{8U};
constexpr UInt64 FailedDetail{0x51U};
constexpr UInt16 InvokeRight{0x0002U};

// The service's window (its client page, then its bounce page), the disk
// its block service serves and the client's data Frames, all host buffers.
alignas(4096) UInt8 g_window[2U][PageBytes]{};
UInt8 g_disk[DiskSectors][SectorBytes]{};
UInt8 g_pages[ClientPages][PageBytes]{};
Size g_crashes{0U};
UInt64 g_crashKind{0U};
UInt64 g_incarnation{0U};

void Crash(const UInt64 kind) noexcept
{
    ++g_crashes;
    g_crashKind = kind;
}

[[nodiscard]] UInt64 Load(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

void Store(UInt8* const bytes, const UInt64 value) noexcept
{
    for (Size index{0U}; index < 8U; ++index)
    {
        bytes[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] UInt64 Address(const void* const pointer) noexcept
{
    return reinterpret_cast<Foundation::UIntPointer>(pointer);
}

// One block request the service sent: its payload, its two attachments'
// handles and rights.
struct BlockRecord
{
    UInt64 Method;
    UInt64 Transaction;
    UInt64 Tag;
    UInt64 Sector;
    UInt64 Count;
    UInt64 Reply;
    UInt64 Frame;
    UInt16 ReplyRights;
    UInt16 FrameRights;
};

// One Send to a client's reply capability, decoded.
struct ReplyRecord
{
    UInt64 Endpoint;
    UInt64 Transaction;
    UInt64 Kind;
    UInt64 Detail;
    UInt64 Extra;
};

// The kernel the service runs on, with the AHCI driver played over g_disk. Its creates, maps and
// unmaps succeed and are logged; a client's Frame mapped at the window's first page is copied in at
// the map and out at the unmap, the bounce Frame is the window's second page. A block request sent
// to the block service is executed against the disk with the attached Frame's host buffer and its
// reply queued for the block-reply Endpoint (with Delay, released only after one wake for the
// service Endpoint while requests remain; with InjectStale, after a reply to another transaction).
// The script's requests are delivered on the service Endpoint, whose readiness is the kernel's:
// signalled once as requests are queued and again by a receive that leaves some queued, and
// consumed by the wait that reports it. A wait with no signal left fails, which ends the run, as
// does a run that waits without end.
struct FsKernel
{
    UInt8 Script[24U][DeliveryCapacity];
    Size ScriptBytes[24U];
    Size Scripted;
    Size Received;
    bool ServiceSignal;
    UInt64 ScriptSender;
    UInt8 Pending[2U][DeliveryCapacity];
    Size PendingBytes[2U];
    Size PendingCount;
    bool PendingReady;
    bool Delay;
    bool Teased;
    Size TeasedWakes;
    bool InjectStale;
    bool FailClientWrites;
    bool FailReads;
    UInt64 DescribeSize{512U};
    UInt64 DescribeSectors{DiskSectors};
    UInt64 DescribeFlags{Component::BlockMediumPresent};
    UInt64 MediumGeneration{19U};
    bool ChangeOnClientWrite;
    Size Describes;
    Size Flushes;
    bool FailFlush;
    bool UnsupportedMedium;
    Size FailWriteNumber;
    Size Writes;
    UInt64 RefuseReplyTo;
    BlockRecord Blocks[256U];
    Size BlockCount;
    ReplyRecord Replies[32U];
    Size ReplyCount;
    UInt64 Closed[64U];
    Size Closes;
    UInt64 Types[4U];
    Size Creates;
    UInt64 BounceFrame;
    bool BounceMapped;
    UInt64 MappedClient;
    UInt64 MappedOffset;
    bool WideFrame;
    Size ClientMaps;
    Size ClientUnmaps;
    Size Waits;

    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<FsKernel*>(context)->Answer(*request, *response);
    }

    void Answer(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        switch (static_cast<SystemCallNumber>(request.Number))
        {
        case SystemCallNumber::ObjectCreate:
            Types[Creates % 4U] = request.Arguments[2U];
            ++Creates;
            response.Value0 = 0x1000U + Creates;
            response.Value1 = 0x2000U + Creates;
            BounceFrame = (request.Arguments[2U] == Abi::ObjectTypeFrame) ? response.Value0 : BounceFrame;
            return;
        case SystemCallNumber::MemoryMap:
            Map(reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U])), response);
            return;
        case SystemCallNumber::MemoryUnmap:
            Unmap(request, response);
            return;
        case SystemCallNumber::WaitSetWait:
            Wait(request, response);
            return;
        case SystemCallNumber::IpcReceive:
            Receive(request, response);
            return;
        case SystemCallNumber::IpcSend:
            Send(request, response);
            return;
        case SystemCallNumber::CapabilityClose:
            Closed[Closes % 64U] = request.Arguments[0U];
            ++Closes;
            return;
        default:
            response.Error = SystemCallError::UnknownSystemCall;
            return;
        }
    }

    void Map(const UInt8* const bytes, Abi::SystemCallResponse& response) noexcept
    {
        const UInt64 frame = Load(bytes + Abi::FaultResolveFrameOffset);
        const UInt64 start = Load(bytes + Abi::FaultResolveVirtualStartOffset);
        const auto offset = Load(bytes + Abi::FaultResolveFramePageOffset);
        const bool one = Load(bytes + Abi::FaultResolvePageCountOffset) == 1U;
        if (one && (start == Address(g_window[1U])) && (frame == BounceFrame) && !BounceMapped)
        {
            BounceMapped = true;
            return;
        }
        if (one && (start == Address(g_window[0U])) && (MappedClient == 0U) && (frame >= FirstPage) &&
            (frame < (FirstPage + ClientPages)) && offset < ClientPages - (frame - FirstPage) &&
            (offset == 0U || (WideFrame && frame == FirstPage)))
        {
            std::memcpy(g_window[0U], g_pages[frame - FirstPage + offset], PageBytes);
            MappedOffset = offset;
            MappedClient = frame;
            ++ClientMaps;
            return;
        }
        response.Error = SystemCallError::InvalidArguments;
    }

    void Unmap(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        if ((request.Arguments[0U] != Space) || (request.Arguments[2U] != Address(g_window[0U])) ||
            (request.Arguments[3U] != 1U) || (MappedClient == 0U))
        {
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
        std::memcpy(g_pages[MappedClient - FirstPage + MappedOffset], g_window[0U], PageBytes);
        // What the page shows once unmapped is no client's.
        std::memset(g_window[0U], 0xEE, PageBytes);
        MappedClient = 0U;
        ++ClientUnmaps;
    }

    void Wait(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        ++Waits;
        const bool requests = Received < Scripted;
        if ((request.Arguments[0U] != WaitSet) || (Waits > 4096U))
        {
            response.Error = SystemCallError::InvalidHandle;
        }
        else if ((PendingCount != 0U) && !PendingReady && Delay && !Teased && requests && ServiceSignal)
        {
            Teased = true;
            ++TeasedWakes;
            ServiceSignal = false;
            response.Value0 = Component::FsServiceWaitTag;
        }
        else if (PendingCount != 0U)
        {
            PendingReady = true;
            response.Value0 = Component::FsBlockReplyWaitTag;
        }
        else if (requests && ServiceSignal)
        {
            ServiceSignal = false;
            response.Value0 = Component::FsServiceWaitTag;
        }
        else
        {
            response.Error = SystemCallError::InvalidHandle;
        }
    }

    void Receive(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        UInt8* const target = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        const UInt8* source{nullptr};
        Size bytes{0U};
        if ((request.Arguments[0U] == Endpoint) && (Received < Scripted))
        {
            source = Script[Received];
            bytes = ScriptBytes[Received];
            ++Received;
            ServiceSignal = ServiceSignal || (Received < Scripted);
        }
        else if ((request.Arguments[0U] == BlockReply) && (PendingCount != 0U) && PendingReady)
        {
            source = Pending[0U];
            bytes = PendingBytes[0U];
        }
        if ((source == nullptr) || (bytes > request.Arguments[3U]))
        {
            response.Error = ((request.Arguments[0U] == Endpoint) || (request.Arguments[0U] == BlockReply))
                                 ? SystemCallError::WouldBlock
                                 : SystemCallError::InvalidHandle;
            return;
        }
        std::memcpy(target, source, bytes);
        response.Value0 = 1U;
        response.Value1 = bytes;
        if (source == Pending[0U])
        {
            std::memcpy(Pending[0U], Pending[1U], DeliveryCapacity);
            PendingBytes[0U] = PendingBytes[1U];
            --PendingCount;
            PendingReady = PendingCount != 0U;
        }
    }

    void Send(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        const UInt8* const frame =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        const Size bytes = static_cast<Size>(request.Arguments[3U]);
        UInt8 delivery[DeliveryCapacity]{};
        std::memcpy(delivery + Header, frame,
                    (bytes < Component::EchoMaximumFrameBytes) ? bytes : Component::EchoMaximumFrameBytes);
        if (request.Arguments[0U] == BlockService)
        {
            Block(delivery, Header + bytes, response);
            return;
        }
        Component::FsMessage reply{};
        const bool decoded =
            Component::ReadFsFrame(delivery, Header + bytes, reply) && (reply.Method == Component::FsReplyMethod);
        Replies[ReplyCount % 32U] = ReplyRecord{.Endpoint = request.Arguments[0U],
                                                .Transaction = decoded ? reply.Words[0U] : ~UInt64{0U},
                                                .Kind = reply.Words[1U],
                                                .Detail = reply.Words[2U],
                                                .Extra = reply.Words[3U]};
        ++ReplyCount;
        response.Error =
            (request.Arguments[0U] == RefuseReplyTo) ? SystemCallError::ResourceExhausted : SystemCallError::None;
    }

    [[nodiscard]] UInt8* FrameBuffer(const UInt64 frame) const noexcept
    {
        if ((frame == BounceFrame) && BounceMapped)
        {
            return g_window[1U];
        }
        return ((frame >= FirstPage) && (frame < (FirstPage + ClientPages))) ? g_pages[frame - FirstPage] : nullptr;
    }

    // The driver: one request outstanding, executed at once.
    void Block(const UInt8* const delivery, const Size bytes, Abi::SystemCallResponse& response) noexcept
    {
        Component::BlockMessage message{};
        if (!Component::ReadBlockFrame(delivery, bytes, message) || (PendingCount != 0U))
        {
            response.Error = SystemCallError::InvalidArguments;
            return;
        }
        if (message.Method == Component::BlockDescribeMethod)
        {
            ++Describes;
            if (message.Words[0U] != DriverTag)
            {
                Queue(message.Transaction, Component::BlockDriverRestarted, 0U);
            }
            else if (UnsupportedMedium)
            {
                Queue(message.Transaction, Component::BlockFailed, Component::BlockUnsupportedSectorSize);
            }
            else
            {
                const UInt64 words[4U]{DescribeSize, DescribeSectors, DescribeFlags, MediumGeneration};
                PendingBytes[PendingCount] =
                    Header + Component::WriteBlockFrame(Pending[PendingCount] + Header,
                                                        Component::BlockDescribeReplyMethod, message.Transaction, words,
                                                        nullptr, nullptr);
                ++PendingCount;
            }
            PendingReady = !Delay;
            return;
        }
        if (message.Method == Component::BlockFlushMethod)
        {
            ++Flushes;
            Queue(message.Transaction, FailFlush ? Component::BlockFailed : Component::BlockDone,
                  FailFlush ? FailedDetail : 0U);
            PendingReady = !Delay;
            return;
        }
        const UInt8* const attachments = delivery + Header + Component::EchoHeaderBytes;
        const UInt64 sector = message.Words[1U];
        const UInt64 count = message.Words[2U];
        Blocks[BlockCount % 256U] =
            BlockRecord{.Method = message.Method,
                        .Transaction = message.Transaction,
                        .Tag = message.Words[0U],
                        .Sector = sector,
                        .Count = count,
                        .Reply = message.Reply.Selector,
                        .Frame = message.Data.Selector,
                        .ReplyRights = static_cast<UInt16>(attachments[24U] | (attachments[25U] << 8U)),
                        .FrameRights = static_cast<UInt16>(attachments[56U] | (attachments[57U] << 8U))};
        ++BlockCount;
        UInt8* const buffer = FrameBuffer(message.Data.Selector);
        const bool write = message.Method == Component::BlockWriteMethod;
        UInt64 kind{Component::BlockDone};
        UInt64 detail{count};
        if (message.Words[0U] != DriverTag)
        {
            kind = Component::BlockDriverRestarted;
            detail = 0U;
        }
        else if ((message.Words[4U] != MediumGeneration) ||
                 (ChangeOnClientWrite && write && (message.Data.Selector != BounceFrame)))
        {
            ++MediumGeneration;
            kind = Component::BlockMediumChanged;
            detail = 0U;
        }
        else if ((buffer == nullptr) || (count > 8U) || (sector >= DiskSectors) || (count > (DiskSectors - sector)))
        {
            kind = Component::BlockFailed;
            detail = Component::BlockOutOfRange;
        }
        else if ((write && FailClientWrites && (message.Data.Selector != BounceFrame)) || (!write && FailReads) ||
                 (write && (++Writes == FailWriteNumber)))
        {
            kind = Component::BlockFailed;
            detail = FailedDetail;
        }
        else
        {
            for (UInt64 index{0U}; index < count; ++index)
            {
                UInt8* const page = buffer + (index * SectorBytes);
                std::memcpy(write ? g_disk[sector + index] : page, write ? page : g_disk[sector + index], SectorBytes);
            }
        }
        if (InjectStale)
        {
            InjectStale = false;
            Queue(message.Transaction + 100U, Component::BlockFailed, FailedDetail);
        }
        Queue(message.Transaction, kind, detail);
        PendingReady = !Delay;
        Teased = false;
    }

    void Queue(const UInt64 transaction, const UInt64 kind, const UInt64 detail) noexcept
    {
        const UInt64 words[Component::BlockPayloadWords]{transaction, kind, detail, 0U};
        PendingBytes[PendingCount] =
            Header + Component::WriteBlockFrame(Pending[PendingCount] + Header, Component::BlockReplyMethod,
                                                transaction, words, nullptr, nullptr);
        ++PendingCount;
    }

    // Adds a delivery of method with words {tag, first, second, third} (a
    // name for Open and Create; a Read or Write carries client page page's
    // Frame), from Sender through the sealed Endpoint.
    void Request(const UInt32 method, const UInt64 transaction, const UInt64 tag, const UInt64 first,
                 const UInt64 second, const UInt64 third, const UInt64 page = 0U,
                 const char* const name = nullptr) noexcept
    {
        UInt8* const delivery = Script[Scripted % 24U];
        const CapabilityHandle reply{FirstReply + transaction, 1U};
        const CapabilityHandle data{FirstPage + page, 2U};
        UInt64 words[Component::FsPayloadWords]{tag, first, second, third,
                                                1U,  0U,    1U,     method == Component::FsReadMethod ? 1U : 0U};
        const bool rootSlot = Component::Detail::HasFsName(method) || method == Component::FsCommitMethod ||
                              method == Component::FsQueryCommitMethod;
        if (!rootSlot)
        {
            words[6U] = 0U;
        }
        const bool manager = method == Component::FsInstallViewMethod || method == Component::FsRemoveViewMethod ||
                             method == Component::FsResetViewsMethod || method == Component::FsQuiesceMethod;
        if (method == Component::FsInstallViewMethod)
        {
            words[4U] = 0U;
            words[5U] = page;
            words[6U] = 0U;
        }
        if (method == Component::FsQuiesceMethod || method == Component::FsResetViewsMethod)
        {
            for (Size i{1U}; i < Component::FsPayloadWords; ++i)
            {
                words[i] = 0U;
            }
        }
        if (method == Component::FsRemoveViewMethod)
        {
            words[1U] = 0U;
            words[4U] = first;
        }
        Store(delivery + 14U, manager ? 0x802U : 2U);
        const Size length = (name != nullptr) ? std::strlen(name) : 0U;
        const Size bytes = Component::WriteFsFrame(delivery + Header, method, transaction, words,
                                                   reinterpret_cast<const UInt8*>(name), length, &reply, &data);
        Store(delivery + 24U, ScriptSender);
        Store(delivery + Component::EchoDeliverySealedSelectorOffset,
              manager ? Component::FsManagerSelector : Component::FsSealedSelector);
        ScriptBytes[Scripted % 24U] = Header + bytes;
        ++Scripted;
        ServiceSignal = true;
    }

    void Open(const UInt64 transaction, const char* const name) noexcept
    {
        Request(Component::FsOpenMethod, transaction, g_incarnation, 0U, 0U, 0U, 0U, name);
    }

    void Create(const UInt64 transaction, const char* const name) noexcept
    {
        Request(Component::FsCreateMethod, transaction, g_incarnation, 0U, 0U, 0U, 0U, name);
    }

    // A Read or Write of block index of (object, birth) with client page
    // page's Frame.
    void Read(const UInt64 transaction, const UInt64 object, const UInt64 birth, const UInt64 index,
              const UInt64 page) noexcept
    {
        Request(Component::FsReadMethod, transaction, g_incarnation, object, birth, index, page);
    }

    void Write(const UInt64 transaction, const UInt64 object, const UInt64 birth, const UInt64 index,
               const UInt64 page) noexcept
    {
        Request(Component::FsWriteMethod, transaction, g_incarnation, object, birth, index, page);
    }

    void Commit(const UInt64 transaction, const UInt64 committed, const UInt64 flags) noexcept
    {
        Request(Component::FsCommitMethod, transaction, g_incarnation, committed, flags, 0U);
    }

    void Query(const UInt64 transaction, const UInt64 committed) noexcept
    {
        Request(Component::FsQueryCommitMethod, transaction, g_incarnation, committed, 0U, 0U);
    }

    void Stat(const UInt64 transaction, const UInt64 object, const UInt64 birth,
              const UInt64 tag = g_incarnation) noexcept
    {
        Request(Component::FsStatMethod, transaction, tag, object, birth, 0U);
    }

    // The reply to transaction, on its reply capability.
    [[nodiscard]] bool Replied(const UInt64 transaction, const UInt64 kind, const UInt64 detail = 0U,
                               const UInt64 extra = 0U) const noexcept
    {
        for (Size index{0U}; (index < ReplyCount) && (index < 32U); ++index)
        {
            const ReplyRecord& record = Replies[index];
            if (record.Transaction == transaction)
            {
                return (record.Endpoint == (FirstReply + transaction)) && (record.Kind == kind) &&
                       (record.Detail == detail) && (record.Extra == extra);
            }
        }
        return false;
    }

    [[nodiscard]] bool ClosedHandle(const UInt64 selector) const noexcept
    {
        bool found{false};
        for (Size index{0U}; (index < Closes) && (index < 64U); ++index)
        {
            found = found || (Closed[index] == selector);
        }
        return found;
    }

    // Every block request named the driver's tag, a copy of the block-reply
    // Endpoint with Invoke and a Map-only Frame, with transactions that
    // count from 1.
    [[nodiscard]] bool BlockRequestsWellFormed() const noexcept
    {
        bool formed{BlockCount != 0U};
        for (Size index{0U}; (index < BlockCount) && (index < 256U); ++index)
        {
            const BlockRecord& record = Blocks[index];
            formed = formed && (record.Tag == DriverTag) && (record.Reply == BlockReply) &&
                     (record.ReplyRights == InvokeRight) && (record.FrameRights == Component::BlockMapRight) &&
                     (record.Transaction == (index + 2U));
        }
        return formed;
    }

    // How many block requests wrote sectors, how many went through the
    // client's Frames rather than the bounce page.
    [[nodiscard]] Size CountWrites(const UInt64 sector) const noexcept
    {
        Size count{0U};
        for (Size index{0U}; (index < BlockCount) && (index < 256U); ++index)
        {
            count +=
                ((Blocks[index].Method == Component::BlockWriteMethod) && (Blocks[index].Sector == sector)) ? 1U : 0U;
        }
        return count;
    }

    [[nodiscard]] Size CountClientTransfers() const noexcept
    {
        Size count{0U};
        for (Size index{0U}; (index < BlockCount) && (index < 256U); ++index)
        {
            count += (Blocks[index].Frame != BounceFrame) ? 1U : 0U;
        }
        return count;
    }
};

FsKernel g_kernel{};

// A fresh kernel for an incarnation's run; the disk stays.
void Begin(const UInt64 incarnation) noexcept
{
    g_kernel = FsKernel{};
    g_kernel.ScriptSender = Sender;
    g_incarnation = incarnation;
    g_crashes = 0U;
    g_crashKind = 0U;
}

[[nodiscard]] UInt64 Run(const UInt64 flags = Abi::ComponentMountFormatBlank, const UInt64 tag = DriverTag,
                         const UInt64 windowPages = 2U) noexcept
{
    UInt64 block[Abi::ComponentBlockWords]{};
    block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
    block[Abi::ComponentIncarnationWord] = g_incarnation;
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorFilesystem;
    block[Abi::ComponentWaitSetWord] = WaitSet;
    block[Abi::ComponentReceiveWord] = Endpoint;
    block[Abi::ComponentCrashKindWord] = CrashKind;
    block[Abi::ComponentAccountWord] = Account;
    block[Abi::ComponentAccountSpaceWord] = Space;
    block[Abi::ComponentWindowStartWord] = Address(g_window);
    block[Abi::ComponentWindowPagesWord] = windowPages;
    block[Abi::ComponentBlockServiceWord] = BlockService;
    block[Abi::ComponentBlockGenerationWord] = tag;
    block[Abi::ComponentMountFlagsWord] = flags;
    block[Abi::ComponentBlockReplyWord] = BlockReply;
    block[Abi::ComponentFsManagerWord] = Endpoint;
    const User::System::SystemCallClient client{User::System::SystemCallTransport{&FsKernel::Transport, &g_kernel}};
    const Component::FilesystemPlatform platform{.Crash = &Crash, .Window = Address(g_window)};
    return Component::ServeFilesystem(client, block, platform);
}

// How a run ends once its script is served: the kernel fails its wait.
[[nodiscard]] constexpr UInt64 Served() noexcept
{
    return Abi::ComponentFilesystemFailedExitCode | (Abi::ComponentFilesystemWaitStep << 32U) |
           (static_cast<UInt64>(SystemCallError::InvalidHandle) << 40U);
}

// A blank 2 MiB disk with the boot token at sector 8 and the block
// demonstration's scratch at sector 64.
void Blank() noexcept
{
    std::memset(g_disk, 0, sizeof(g_disk));
    std::memset(g_disk[User::Filesystem::FsBootTokenSector], 0xA5, SectorBytes);
    std::memset(g_disk[User::Filesystem::FsScratchSector], 0x5A, SectorBytes);
}

[[nodiscard]] bool SentinelsIntact() noexcept
{
    bool intact{true};
    for (Size index{0U}; index < SectorBytes; ++index)
    {
        intact = intact && (g_disk[User::Filesystem::FsBootTokenSector][index] == 0xA5U) &&
                 (g_disk[User::Filesystem::FsScratchSector][index] == 0x5AU);
    }
    return intact;
}

void Fill(const Size page, const UInt32 seed) noexcept
{
    Fat::FillPage(g_pages[page], seed);
}

[[nodiscard]] bool Shows(const Size page, const UInt32 seed) noexcept
{
    UInt8 expected[PageBytes]{};
    Fat::FillPage(expected, seed);
    return std::memcmp(g_pages[page], expected, PageBytes) == 0;
}

[[nodiscard]] bool IsZero(const Size page) noexcept
{
    bool zero{true};
    for (Size index{0U}; index < PageBytes; ++index)
    {
        zero = zero && (g_pages[page][index] == 0U);
    }
    return zero;
}

// A FAT12 volume of the disk's 4096 sectors with a label and README.TXT
// (13 bytes from ReadmeSeed in cluster 2), made by the FAT tests' formatter.
constexpr UInt32 ReadmeSeed{77U};
constexpr UInt32 ReadmeBytes{13U};
Fat::SparseStorage<12U> g_fatStorage;
Fat::SparseDisk g_fatDisk{g_fatStorage.Keys, g_fatStorage.Data, 12U};
Fat::TestDevice g_fatDevice{g_fatDisk};

[[nodiscard]] bool LoadFat() noexcept
{
    constexpr Fat::FormatOptions options{.Sectors = DiskSectors,
                                         .SectorsPerCluster = 1U,
                                         .Reserved = 1U,
                                         .Fats = 2U,
                                         .RootEntries = 224U,
                                         .RootClusters = 0U,
                                         .HighNibble = 0U,
                                         .Label = true};
    constexpr UInt32 chain[1U]{2U};
    g_fatDisk.Reset(DiskSectors);
    g_fatDevice.Arm(Fat::NoBudget);
    Fat::Layout layout{};
    bool made =
        Fat::Format(g_fatDevice, options, layout) &&
        Fat::PlaceFile(g_fatDevice, layout, 2U, "ASTERIA RW ", 0U, nullptr, nullptr, 0U, 0U, 0xFFFU, 0U) &&
        Fat::PlaceFile(g_fatDevice, layout, 1U, "README  TXT", 0U, nullptr, chain, 1U, ReadmeBytes, 0xFFFU, ReadmeSeed);
    for (UInt64 lba{0U}; made && (lba < DiskSectors); ++lba)
    {
        made = g_fatDevice.Read(lba, g_disk[lba]);
    }
    return made;
}

[[nodiscard]] bool ShowsReadme(const Size page) noexcept
{
    UInt8 expected[PageBytes]{};
    Fat::FillPage(expected, ReadmeSeed);
    std::memset(expected + ReadmeBytes, 0, PageBytes - ReadmeBytes);
    return std::memcmp(g_pages[page], expected, PageBytes) == 0;
}

void Install(const UInt64 rights = Component::FsReadRight | Component::FsWriteRight | Component::FsExecRight) noexcept
{
    g_kernel.Request(Component::FsInstallViewMethod, 900U, g_incarnation, 0U, 0U, 0U, rights);
}

void TestNative(TestContext& runner) noexcept
{
    Blank();
    Begin(10U);
    Install();
    Fill(0U, 11U);
    Fill(1U, 12U);
    g_kernel.Create(1U, "hello");
    g_kernel.Write(2U, 1U, 2U, 0U, 0U);
    g_kernel.Write(3U, 1U, 2U, 2U, 1U);
    g_kernel.Commit(4U, 77U, 0U);
    g_kernel.Query(5U, 77U);
    g_kernel.Stat(6U, 1U, 2U);
    g_kernel.Read(7U, 1U, 2U, 0U, 2U);
    g_kernel.Read(8U, 1U, 2U, 1U, 3U);
    ASTERIA_EXPECT(runner, "native v2 preserves direct I/O, sparse zeros and definite commits", Run() == Served(),
                   g_kernel.Replied(900U, Component::FsDone, 1U, 1U), g_kernel.Replied(1U, Component::FsDone, 1U, 2U),
                   g_kernel.Replied(4U, Component::FsCommitted, 3U), g_kernel.Replied(5U, Component::FsCommitted, 3U),
                   g_kernel.Replied(6U, Component::FsDone, 3U, 3U * PageBytes), Shows(2U, 11U), IsZero(3U),
                   SentinelsIntact());
    Begin(11U);
    Install();
    g_kernel.Open(1U, "hello");
    g_kernel.Read(2U, 1U, 2U, 2U, 4U);
    g_kernel.Read(3U, 1U, 99U, 0U, 5U);
    g_kernel.Stat(4U, 0U, 1U);
    g_kernel.ScriptSender = Sender + 1U;
    g_kernel.Stat(5U, 1U, 2U);
    ASTERIA_EXPECT(runner, "remount requires lookup; root stat and sender/slot validation work", Run() == Served(),
                   Shows(4U, 12U), g_kernel.Replied(3U, Component::FsStaleObject),
                   g_kernel.Replied(4U, Component::FsDone), g_kernel.Replied(5U, Component::FsRightsDenied));
    Begin(12U);
    Install();
    g_kernel.WideFrame = true;
    g_kernel.Open(1U, "hello");
    g_kernel.Read(2U, 1U, 2U, 0U, 0U);
    auto* payload = g_kernel.Script[g_kernel.Scripted - 1U] + Header + Component::EchoHeaderBytes +
                    2U * Component::EchoAttachmentBytes;
    Store(payload + 7U * 8U, 3U);
    ASTERIA_EXPECT(runner, "ReadPages fills every page of a Frame using checked mapping offsets", Run() == Served(),
                   g_kernel.Replied(2U, Component::FsDone, 3U), Shows(0U, 11U), IsZero(1U), Shows(2U, 12U));
}
void TestFailedWrite(TestContext& runner) noexcept
{
    Blank();
    Begin(15U);
    Install();
    Fill(0U, 13U);
    g_kernel.FailClientWrites = true;
    g_kernel.Create(1U, "pending");
    g_kernel.Write(2U, 1U, 2U, 0U, 0U);
    g_kernel.Stat(3U, 1U, 2U);
    g_kernel.Create(4U, "replacement");
    g_kernel.Stat(5U, 1U, 2U);
    ASTERIA_EXPECT(runner, "a backend remount invalidates pending slots even when the object index is reused",
                   Run() == Served(), g_kernel.Replied(2U, Component::FsFailed, FailedDetail),
                   g_kernel.Replied(3U, Component::FsStaleObject), g_kernel.Replied(4U, Component::FsDone, 1U, 3U),
                   g_kernel.Replied(5U, Component::FsStaleObject), SentinelsIntact());
}
void TestCrash(TestContext& runner) noexcept
{
    for (UInt64 flags{1U}; flags <= 2U; ++flags)
    {
        Blank();
        Begin(20U);
        Install();
        Fill(0U, 21U);
        g_kernel.Create(1U, "hello");
        g_kernel.Write(2U, 1U, 2U, 0U, 0U);
        g_kernel.Commit(3U, 77U, 0U);
        const bool seeded = Run() == Served();
        Begin(21U);
        Install();
        Fill(1U, 22U);
        g_kernel.Open(1U, "hello");
        g_kernel.Write(2U, 1U, 2U, 0U, 1U);
        g_kernel.Commit(3U, 78U, flags);
        const auto crashed = Run();
        Begin(22U);
        Install();
        g_kernel.Query(1U, 78U);
        g_kernel.Open(2U, "hello");
        g_kernel.Read(3U, 1U, 2U, 0U, 2U);
        ASTERIA_EXPECT(runner, "v2 crash queries stay definite on either side of publication", seeded,
                       crashed == Abi::ComponentCrashReturnedExitCode, Run() == Served(),
                       g_kernel.Replied(1U, flags == 1U ? Component::FsNotCommitted : Component::FsCommitted,
                                        flags == 1U ? 0U : 4U),
                       Shows(2U, flags == 1U ? 21U : 22U));
    }
}
void TestFat(TestContext& runner) noexcept
{
    const bool made = LoadFat();
    Begin(30U);
    Install();
    Fill(0U, 33U);
    g_kernel.Open(1U, "readme.txt");
    g_kernel.Read(2U, 1U, 2U, 0U, 1U);
    g_kernel.Create(3U, "hello");
    g_kernel.Write(4U, 2U, 3U, 0U, 0U);
    g_kernel.Commit(5U, 77U, 0U);
    g_kernel.Read(6U, 2U, 3U, 0U, 2U);
    g_kernel.Query(7U, 77U);
    ASTERIA_EXPECT(runner, "FAT v2 converts slots to entry identities and keeps bounce I/O", made, Run(0U) == Served(),
                   ShowsReadme(1U), Shows(2U, 33U),
                   g_kernel.Replied(7U, Component::FsUnsupported, Component::FsFormatFat), g_kernel.ClientMaps == 3U,
                   g_kernel.BlockRequestsWellFormed());
    Begin(31U);
    Install();
    g_kernel.WideFrame = true;
    g_kernel.Open(1U, "hello");
    g_kernel.Read(2U, 1U, 2U, 0U, 0U);
    auto* payload = g_kernel.Script[g_kernel.Scripted - 1U] + Header + Component::EchoHeaderBytes +
                    2U * Component::EchoAttachmentBytes;
    Store(payload + 7U * 8U, 2U);
    ASTERIA_EXPECT(runner, "FAT ReadPages spans Frame pages and zeroes past EOF", Run(0U) == Served(),
                   g_kernel.Replied(2U, Component::FsDone, 2U), Shows(0U, 33U), IsZero(1U));
}
void TestPartition(TestContext& runner) noexcept
{
    const bool made = LoadFat();
    constexpr UInt32 first{128U};
    std::memmove(g_disk[first], g_disk, (DiskSectors - first) * SectorBytes);
    std::memset(g_disk, 0, first * SectorBytes);
    Fat::Put16(g_disk[first] + 19U, static_cast<UInt32>(DiskSectors - first));
    g_disk[0U][510U] = 0x55U;
    g_disk[0U][511U] = 0xAAU;
    g_disk[0U][450U] = 0x01U; // FAT12 legacy MBR data partition.
    Fat::Put32(g_disk[0U] + 454U, first);
    Fat::Put32(g_disk[0U] + 458U, static_cast<UInt32>(DiskSectors - first));
    Begin(35U);
    Install(Component::FsReadRight);
    g_kernel.DescribeFlags |= Component::BlockReadOnly;
    g_kernel.Open(1U, "readme.txt");
    g_kernel.Read(2U, 1U, 2U, 0U, 1U);
    ASTERIA_EXPECT(runner, "MBR mount translates bounded partition I/O and enumeration never writes", made,
                   Run(0U) == Served(), g_kernel.Replied(900U, Component::FsDone, 1U, 1U), ShowsReadme(1U),
                   g_kernel.Writes == 0U);
}
void TestPowerQuiesce(TestContext& runner) noexcept
{
    Blank();
    Begin(0x91U);
    Install();
    g_kernel.Request(Component::FsQuiesceMethod, 1U, g_incarnation, 0U, 0U, 0U);
    Store(g_kernel.Script[g_kernel.Scripted - 1U] + Component::EchoDeliverySealedSelectorOffset,
          Component::FsSealedSelector);
    g_kernel.Request(Component::FsQuiesceMethod, 2U, g_incarnation, 0U, 0U, 0U);
    Store(g_kernel.Script[g_kernel.Scripted - 1U] + 14U, 2U);
    g_kernel.Create(3U, "power");
    ASTERIA_EXPECT(runner, "quiesce needs the manager face and Manage, without closing admission on denial",
                   Run() == Served(), g_kernel.Flushes == 0U, g_kernel.Replied(1U, Component::FsRightsDenied),
                   g_kernel.Replied(2U, Component::FsRightsDenied), g_kernel.Replied(3U, Component::FsDone, 1U, 2U));
    for (Size failure{0U}; failure < 2U; ++failure)
    {
        Blank();
        Begin(0x92U);
        Install();
        g_kernel.FailFlush = failure != 0U;
        g_kernel.Create(1U, "power");
        g_kernel.Request(Component::FsQuiesceMethod, 2U, g_incarnation, 0U, 0U, 0U);
        g_kernel.Open(3U, "power");
        ASTERIA_EXPECT(runner, "native root quiesce commits writer staging, flushes and closes admission",
                       Run() == Served(), g_kernel.Flushes == 1U,
                       g_kernel.Replied(2U, failure == 0U ? Component::FsDone : Component::FsFailed,
                                        failure == 0U ? 0U : FailedDetail),
                       g_kernel.Replied(3U, Component::FsRefused, Component::FsMaximumOutstanding));
        const bool made = LoadFat();
        Begin(0xA1U);
        Install();
        Fill(1U, 37U);
        g_kernel.FailFlush = failure != 0U;
        g_kernel.Create(1U, "power.txt");
        g_kernel.Write(2U, 1U, 2U, 0U, 1U);
        g_kernel.Request(Component::FsQuiesceMethod, 3U, g_incarnation, 0U, 0U, 0U);
        g_kernel.Open(4U, "power.txt");
        ASTERIA_EXPECT(runner, "FAT durable data precedes the clean marker and acknowledgement", made,
                       Run(0U) == Served(), g_kernel.Flushes == (failure == 0U ? 2U : 1U),
                       g_kernel.Replied(3U, failure == 0U ? Component::FsDone : Component::FsFailed,
                                        failure == 0U ? 0U : FailedDetail),
                       g_kernel.Replied(4U, Component::FsRefused, Component::FsMaximumOutstanding),
                       (g_disk[0U][0x25U] & 1U) == failure);
        if (failure == 0U)
        {
            Begin(0xA2U);
            Install();
            g_kernel.Open(1U, "power.txt");
            g_kernel.Read(2U, 1U, 2U, 0U, 2U);
            ASTERIA_EXPECT(runner, "a fresh mount reads the quiesced staged page", Run(0U) == Served(), Shows(2U, 37U));
        }
    }
}
void TestRefusals(TestContext& runner) noexcept
{
    Blank();
    Begin(40U);
    Install(Component::FsReadRight);
    g_kernel.Create(1U, "hello");
    g_kernel.Read(2U, 1000U, 1U, 0U, 0U);
    g_kernel.Request(Component::FsResetViewsMethod, 3U, g_incarnation, 0U, 0U, 0U);
    const auto reset = g_kernel.Scripted - 1U;
    g_kernel.Script[reset][14U] = 2U;
    g_kernel.Script[reset][15U] = 0U;
    g_kernel.Stat(4U, 0U, 1U);
    g_kernel.Request(Component::FsRemoveViewMethod, 5U, g_incarnation, 1U, 0U, 0U);
    g_kernel.Stat(6U, 0U, 1U);
    g_kernel.Request(Component::FsStatMethod, 7U, g_incarnation - 1U, 0U, 1U, 0U);
    ASTERIA_EXPECT(runner, "read-only views, forged slots, manager rights and removal are enforced", Run() == Served(),
                   g_kernel.Replied(1U, Component::FsRightsDenied), g_kernel.Replied(2U, Component::FsStaleObject),
                   g_kernel.Replied(3U, Component::FsRightsDenied), g_kernel.Replied(4U, Component::FsDone),
                   g_kernel.Replied(5U, Component::FsDone), g_kernel.Replied(6U, Component::FsRightsDenied),
                   g_kernel.Replied(7U, Component::FsRestarted));
    Blank();
    Begin(41U);
    Install();
    g_kernel.DescribeFlags |= Component::BlockReadOnly;
    ASTERIA_EXPECT(runner, "read-only media cannot install writable views or format", Run(0U) == Served(),
                   g_kernel.Replied(900U, Component::FsCorrupt), g_kernel.Writes == 0U);
    const bool made = LoadFat();
    // Inventory consent alone is not ASTERIA.RW: remove the latter entry.
    for (Size lba{0U}; lba < DiskSectors; ++lba)
    {
        for (Size at{0U}; at < SectorBytes; at += 32U)
        {
            if (std::memcmp(g_disk[lba] + at, "ASTERIA RW ", 11U) == 0)
            {
                g_disk[lba][at] = 0xE5U;
            }
        }
    }
    Begin(42U);
    Install();
    ASTERIA_EXPECT(runner, "a writable FAT device without ASTERIA.RW refuses write authority", made,
                   Run(0U) == Served(), g_kernel.Replied(900U, Component::FsRightsDenied), g_kernel.Writes == 0U);
}

void TestCommitReplayWriter(TestContext& runner) noexcept
{
    Blank();
    Begin(0xB1U);
    Install();
    Fill(0U, 41U);
    Fill(1U, 42U);
    g_kernel.Request(Component::FsInstallViewMethod, 901U, g_incarnation, Sender + 1U, 0U, 0U,
                     Component::FsReadRight | Component::FsWriteRight);
    g_kernel.Create(1U, "private");
    g_kernel.Write(2U, 1U, 3U, 0U, 0U);
    g_kernel.Commit(3U, 77U, 0U);
    g_kernel.Write(4U, 1U, 3U, 0U, 1U);
    g_kernel.Commit(5U, 77U, 0U);
    g_kernel.ScriptSender = Sender + 1U;
    g_kernel.Open(6U, "private");
    g_kernel.Stat(7U, 0U, 1U);
    g_kernel.Commit(8U, 78U, 0U);
    for (Size at{g_kernel.Scripted - 3U}; at < g_kernel.Scripted; ++at)
    {
        auto* payload = g_kernel.Script[at] + Header + Component::EchoHeaderBytes + Component::EchoAttachmentBytes;
        Store(payload + 4U * 8U, 2U);
        Store(payload + (at == g_kernel.Scripted - 2U ? 2U : 6U) * 8U, 2U);
    }
    g_kernel.ScriptSender = Sender;
    g_kernel.Commit(9U, 78U, 0U);
    ASTERIA_EXPECT(runner, "CommitReplayWriter: replay preserves writer exclusion until publication", Run() == Served(),
                   g_kernel.Replied(5U, Component::FsCommitted, 3U),
                   g_kernel.Replied(6U, Component::FsRefused, Component::FsMaximumOutstanding),
                   g_kernel.Replied(7U, Component::FsRefused, Component::FsMaximumOutstanding),
                   g_kernel.Replied(8U, Component::FsRefused, Component::FsMaximumOutstanding),
                   g_kernel.Replied(9U, Component::FsCommitted, 4U));
}

void TestQuiesceHistoryCollision(TestContext& runner) noexcept
{
    Blank();
    Begin(0xB2U);
    Install();
    Fill(0U, 43U);
    Fill(1U, 44U);
    g_kernel.Create(1U, "durable");
    g_kernel.Write(2U, 1U, 2U, 0U, 0U);
    g_kernel.Commit(3U, 77U, 0U);
    g_kernel.Write(4U, 1U, 2U, 0U, 1U);
    g_kernel.ScriptSender = Sender + 2U;
    g_kernel.Request(Component::FsQuiesceMethod, 77U, g_incarnation, 0U, 0U, 0U);
    const bool quiesced = Run() == Served() && g_kernel.Replied(77U, Component::FsDone) && g_kernel.Flushes == 1U;
    Begin(0xB3U);
    Install();
    g_kernel.Query(1U, 77U);
    g_kernel.Open(2U, "durable");
    g_kernel.Read(3U, 1U, 2U, 0U, 2U);
    ASTERIA_EXPECT(runner, "QuiesceHistoryCollision: shutdown publishes staging independently of client history",
                   quiesced, Run(0U) == Served(), g_kernel.Replied(1U, Component::FsCommitted, 3U), Shows(2U, 44U));
}

void TestReadOnlyQuiesce(TestContext& runner) noexcept
{
    Blank();
    Begin(0xB4U);
    const bool formatted = Run() == Served();
    for (Size failure{0U}; failure < 2U; ++failure)
    {
        Begin(0xB5U);
        Install(Component::FsReadRight);
        g_kernel.DescribeFlags |= Component::BlockReadOnly;
        g_kernel.FailFlush = failure != 0U;
        g_kernel.Request(Component::FsQuiesceMethod, 1U, g_incarnation, 0U, 0U, 0U);
        ASTERIA_EXPECT(runner, "ReadOnlyQuiesce: an unstaged AsteriaFS only flushes, including failure", formatted,
                       Run(0U) == Served(), g_kernel.Writes == 0U, g_kernel.Flushes == 1U,
                       g_kernel.Replied(1U, failure == 0U ? Component::FsDone : Component::FsFailed,
                                        failure == 0U ? 0U : FailedDetail));
    }
}

void TestFatBootCodeProtectiveByte(TestContext& runner) noexcept
{
    const bool made = LoadFat();
    g_disk[0U][450U] = 0xEEU;
    Begin(0xB6U);
    Install(Component::FsReadRight);
    g_kernel.Open(1U, "readme.txt");
    g_kernel.Read(2U, 1U, 2U, 0U, 1U);
    ASTERIA_EXPECT(runner, "FatBootCodeProtectiveByte: boot code is not a partition entry", made, Run(0U) == Served(),
                   ShowsReadme(1U), g_kernel.Writes == 0U);
    // A real protective entry must retain GPT failure even with a FAT-like BPB.
    std::memset(g_disk[0U] + 446U, 0, 64U);
    g_disk[0U][450U] = 0xEEU;
    Fat::Put32(g_disk[0U] + 454U, 1U);
    Fat::Put32(g_disk[0U] + 458U, DiskSectors - 1U);
    Begin(0xB7U);
    Install(Component::FsReadRight);
    ASTERIA_EXPECT(runner, "ProtectiveGptFailure: an identified GPT never falls back to FAT or formatting",
                   Run() == Served(), g_kernel.Replied(900U, Component::FsCorrupt), g_kernel.Writes == 0U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"FilesystemServiceTests"};
    Asteria::Test::TestNative(runner);
    Asteria::Test::TestFailedWrite(runner);
    Asteria::Test::TestCrash(runner);
    Asteria::Test::TestFat(runner);
    Asteria::Test::TestPartition(runner);
    Asteria::Test::TestRefusals(runner);
    Asteria::Test::TestPowerQuiesce(runner);
    Asteria::Test::TestCommitReplayWriter(runner);
    Asteria::Test::TestQuiesceHistoryCollision(runner);
    Asteria::Test::TestReadOnlyQuiesce(runner);
    Asteria::Test::TestFatBootCodeProtectiveByte(runner);
    return runner.Finish();
}
