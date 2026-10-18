// SPDX-License-Identifier: Apache-2.0
#include "User/Component/DisplayBroker.hpp"

#include "User/Graphics/Composition/CursorImages.hpp"

#include "Abi/ComponentBootstrap.hpp"
#include "User/Component/DisplayFrame.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Console/TextConsole.hpp"
#include "User/Graphics/Bold8x16Font.hpp"
#include "User/Graphics/PixelFormat.hpp"

#include "Support/TestContext.hpp"

#include <cstring>
#include <initializer_list>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Foundation::UInt8;
namespace Component = User::Component;
using Abi::SystemCallError;
using Abi::SystemCallNumber;

constexpr UInt32 Width{1280U};
constexpr UInt32 Height{800U};
constexpr UInt64 Pages{(Width * Height * 4U) / 4096U};
constexpr UInt64 Incarnation{0x100U};
constexpr UInt64 Region{0x30U};
constexpr UInt64 WaitSet{0x60U};
constexpr UInt64 Endpoint{0x80U};
constexpr User::System::CapabilityHandle ReplyHandle{0x0123U, 0xAAAA'BBBB'CCCC'DDDDULL};
constexpr Size Header{Component::EchoDeliveryHeaderBytes};

// The scanout the broker draws into: a host buffer in place of the
// framebuffer its kernel model maps nothing over.
alignas(4096) UInt32 g_scanout[Width * Height]{};

[[nodiscard]] UInt64 Load(const UInt8* const bytes) noexcept
{
    UInt64 value{0U};
    for (Size index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(bytes[index]) << (index * 8U);
    }
    return value;
}

struct MapRecord
{
    UInt64 Start;
    UInt64 Pages;
    UInt64 MemoryOffset;
    UInt64 FrameOffset;
};

/// The kernel the broker runs on: its creates and maps succeed and are
/// logged; its first wait is ready and its receives deliver the scripted
/// frames, then nothing; its second wait fails, which ends the broker.
class DisplayKernel final
{
public:
    static void Transport(void* const context, const Abi::SystemCallRequest* const request,
                          Abi::SystemCallResponse* const response) noexcept
    {
        static_cast<DisplayKernel*>(context)->Answer(*request, *response);
    }

    void Answer(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        response = Abi::SystemCallResponse{.Error = SystemCallError::None, .Value0 = 0U, .Value1 = 0U};
        const UInt8* const bytes =
            reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[3U]));
        switch (static_cast<SystemCallNumber>(request.Number))
        {
        case SystemCallNumber::ObjectCreate:
            CreatedTypes[Creates % 4U] = request.Arguments[2U];
            CreatedPages[Creates % 4U] = Load(bytes + Abi::ObjectCreatePagesOffset);
            CreatedCache[Creates % 4U] = bytes[Abi::ObjectCreateCachePolicyOffset];
            // A request's size is argument 4: only a device Frame's names a region.
            CreatedRegion[Creates % 4U] = (request.Arguments[4U] == Abi::ObjectCreateDeviceFrameRequestSizeBytes)
                                              ? Load(bytes + Abi::ObjectCreateDeviceFrameRegionOffset)
                                              : 0U;
            ++Creates;
            if ((CreatedTypes[(Creates - 1U) % 4U] == Abi::ObjectTypeMemoryObject) &&
                (CreatedCache[(Creates - 1U) % 4U] == Abi::ObjectCreateCacheWriteCombining) && RefuseCreate)
            {
                response.Error = SystemCallError::RightsDenied;
                return;
            }
            response.Value0 = 0x1000U + Creates;
            response.Value1 = 0x2000U + Creates;
            return;
        case SystemCallNumber::MemoryMap:
            Maps[MapCount % 4U] = MapRecord{.Start = Load(bytes + Abi::FaultResolveVirtualStartOffset),
                                            .Pages = Load(bytes + Abi::FaultResolvePageCountOffset),
                                            .MemoryOffset = Load(bytes + Abi::FaultResolveMemoryObjectPageOffset),
                                            .FrameOffset = Load(bytes + Abi::FaultResolveFramePageOffset)};
            ++MapCount;
            if ((RefuseMap != 0U) && (MapCount == RefuseMap))
            {
                response.Error = SystemCallError::RightsDenied;
            }
            return;
        case SystemCallNumber::MemoryUnmap:
            ++Unmaps;
            UnmapStart = request.Arguments[2U];
            UnmapPages = request.Arguments[3U];
            return;
        case SystemCallNumber::ObjectDestroy:
            ++Destroys;
            return;
        case SystemCallNumber::WaitSetBind:
            return;
        case SystemCallNumber::WaitSetWait:
            ++Waits;
            WaitedOn = request.Arguments[0U];
            response.Value0 = Component::DisplayServiceWaitTag;
            response.Error = (Waits <= Batches) ? SystemCallError::None : SystemCallError::InvalidHandle;
            return;
        case SystemCallNumber::IpcReceive:
            Receive(request, response);
            return;
        case SystemCallNumber::IpcSend:
            // What the first cell showed when the answer left: the fence.
            for (Size row{0U}; row < 16U; ++row)
            {
                for (Size column{0U}; column < 8U; ++column)
                {
                    CellAtReply[(row * 8U) + column] = g_scanout[(row * Width) + column];
                }
            }
            ++SentCount;
            SentTo = request.Arguments[0U];
            SentBytes = static_cast<Size>(request.Arguments[3U]);
            std::memcpy(Sent + Header,
                        reinterpret_cast<const UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U])),
                        SentBytes);
            if ((Load(Sent + Header + 40U) >> 63U) != 0U)
            {
                ++LivenessReplies;
                std::memcpy(CellAtLiveness, CellAtReply, sizeof(CellAtLiveness));
            }
            if (SentTo == BlockedSelector)
            {
                if (FirstBlockedBytes == 0U)
                {
                    FirstBlockedBytes = SentBytes;
                    std::memcpy(FirstBlocked, Sent + Header, SentBytes);
                }
                else if (CompareRetries)
                {
                    RetryUnchanged = RetryUnchanged && FirstBlockedBytes == SentBytes &&
                                     std::memcmp(FirstBlocked, Sent + Header, SentBytes) == 0;
                }
                if (AlwaysBlock || RefusedReplies != 0U)
                {
                    if (RefusedReplies != 0U)
                    {
                        --RefusedReplies;
                    }
                    response.Error = SystemCallError::WouldBlock;
                }
            }
            return;
        case SystemCallNumber::CapabilityClose:
            Closed = request.Arguments[0U];
            return;
        default:
            response.Error = SystemCallError::UnknownSystemCall;
            return;
        }
    }

    void Receive(const Abi::SystemCallRequest& request, Abi::SystemCallResponse& response) noexcept
    {
        ReceivedOn = request.Arguments[0U];
        if (request.Arguments[0U] != Endpoint)
        {
            response.Error = SystemCallError::WouldBlock;
            return;
        }
        UInt8* const delivery = reinterpret_cast<UInt8*>(static_cast<Foundation::UIntPointer>(request.Arguments[2U]));
        const Size delivered = Received++;
        if (delivered >= Deliveries)
        {
            response.Error = SystemCallError::WouldBlock;
            return;
        }
        if (ScriptBytes[delivered] == 0U)
        {
            response.Error = SystemCallError::WouldBlock;
            return;
        }
        std::memcpy(delivery, Script[delivered], ScriptBytes[delivered]);
        if ((delivery[14U] == 0U) && (delivery[15U] == 0U))
        {
            delivery[14U] = Component::EchoInvokeRight;
            delivery[15U] = 8U;
        }
        response.Value0 = 1U;
        response.Value1 = ScriptBytes[delivered];
    }

    UInt8 Script[100U][Header + Component::DisplayMaximumFrameBytes]{};
    Size ScriptBytes[100U]{};
    Size Deliveries{0U};
    Size Received{0U};
    UInt64 CreatedTypes[4U]{};
    UInt64 CreatedPages[4U]{};
    UInt8 CreatedCache[4U]{};
    UInt64 CreatedRegion[4U]{};
    Size Creates{0U};
    bool RefuseCreate{false};
    Size RefuseMap{0U};
    Size Unmaps{0U};
    Size Destroys{0U};
    UInt64 UnmapStart{0U};
    UInt64 UnmapPages{0U};
    MapRecord Maps[4U]{};
    Size MapCount{0U};
    Size Waits{0U};
    Size Batches{1U};
    UInt64 WaitedOn{0U};
    UInt64 ReceivedOn{0U};
    UInt64 SentTo{0U};
    Size SentCount{0U};
    UInt8 Sent[Header + Component::DisplayMaximumFrameBytes]{};
    Size SentBytes{0U};
    UInt64 Closed{0U};
    UInt32 CellAtReply[16U * 8U]{};
    UInt32 CellAtLiveness[16U * 8U]{};
    Size LivenessReplies{0U};
    UInt64 BlockedSelector{0U};
    Size RefusedReplies{0U};
    bool AlwaysBlock{false};
    bool CompareRetries{false};
    bool RetryUnchanged{true};
    UInt8 FirstBlocked[Component::DisplayMaximumFrameBytes]{};
    Size FirstBlockedBytes{0U};
};

DisplayKernel g_kernel{};

void StartBlock(UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    block[Abi::ComponentStartMagicWord] = Abi::ComponentStartMagic;
    block[Abi::ComponentIncarnationWord] = Incarnation;
    block[Abi::ComponentBehaviorWord] = Abi::ComponentBehaviorDisplay;
    block[Abi::ComponentWaitSetWord] = WaitSet;
    block[Abi::ComponentReceiveWord] = Endpoint;
    block[Abi::ComponentAccountWord] = 0x10U;
    block[Abi::ComponentAccountSpaceWord] = 0x20U;
    block[Abi::ComponentDeviceRegionWord] = Region;
    block[Abi::ComponentScanoutModeWord] = Width | (UInt64{Height} << 32U);
    block[Abi::ComponentScanoutModeWord + 1U] =
        Width | (UInt64{static_cast<UInt32>(User::Graphics::PixelFormat::Bgrx)} << 32U);
}

[[nodiscard]] bool CellShows(const Size row, const Size column, const UInt8 code) noexcept
{
    const UInt32 foreground =
        User::Graphics::PackColor(User::Graphics::PixelFormat::Bgrx, User::Console::ConsoleForeground);
    const UInt32 background =
        User::Graphics::PackColor(User::Graphics::PixelFormat::Bgrx, User::Console::ConsoleBackground);
    const UInt8* const glyph = User::Graphics::Bold8x16Glyphs + ((code - User::Graphics::Bold8x16FirstCode) * 16U);
    for (Size y{0U}; y < 16U; ++y)
    {
        for (Size x{0U}; x < 8U; ++x)
        {
            const bool set = ((glyph[y] >> (7U - x)) & 1U) != 0U;
            if (g_scanout[(((row * 16U) + y) * Width) + (column * 8U) + x] != (set ? foreground : background))
            {
                return false;
            }
        }
    }
    return true;
}

// The broker maps the scanout a page table's worth at a time, fills it, and
// draws what it is sent; it answers a Query on the reply capability the
// Query carried with what it draws and its incarnation as its epoch, and
// closes that capability.
void TestServesTheConsole(TestContext& runner) noexcept
{
    const UInt8 text[]{'H', 'i', '\n', 'O', 'K'};
    g_kernel.ScriptBytes[0U] =
        Component::WriteDisplayConsoleWrite(g_kernel.Script[0U] + Header, 5U, text, sizeof(text)) + Header;
    g_kernel.ScriptBytes[1U] = Component::WriteDisplayQuery(g_kernel.Script[1U] + Header, 6U, ReplyHandle) + Header;
    g_kernel.Deliveries = 2U;
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    const User::System::SystemCallClient client{
        User::System::SystemCallTransport{&DisplayKernel::Transport, &g_kernel}};
    const UInt64 address = reinterpret_cast<Foundation::UIntPointer>(g_scanout);
    const UInt64 exit = Component::ServeDisplay(client, block, address);

    ASTERIA_EXPECT(runner, "the broker serves until its wait fails, its only exit",
                   exit == (Abi::ComponentDisplayFailedExitCode | (Abi::ComponentDisplayWaitStep << 32U) |
                            (static_cast<UInt64>(SystemCallError::InvalidHandle) << 40U)),
                   g_kernel.Waits == 2U, g_kernel.WaitedOn == WaitSet, g_kernel.ReceivedOn == Endpoint,
                   g_kernel.Received == 3U);
    ASTERIA_EXPECT(
        runner, "it maps the scanout through a device Frame of the region and a write-combining MemoryObject",
        g_kernel.Creates == 2U, g_kernel.CreatedTypes[0U] == Abi::ObjectTypeFrame, g_kernel.CreatedPages[0U] == Pages,
        g_kernel.CreatedRegion[0U] == Region, g_kernel.CreatedTypes[1U] == Abi::ObjectTypeMemoryObject,
        g_kernel.CreatedPages[1U] == Pages, g_kernel.CreatedCache[1U] == Abi::ObjectCreateCacheWriteCombining);
    ASTERIA_EXPECT(runner, "in mappings of a page table's 512 pages at most, both objects at the same offset",
                   g_kernel.MapCount == 2U, g_kernel.Maps[0U].Start == address, g_kernel.Maps[0U].Pages == 512U,
                   g_kernel.Maps[0U].MemoryOffset == 0U, g_kernel.Maps[0U].FrameOffset == 0U,
                   g_kernel.Maps[1U].Start == (address + (512U * 4096U)), g_kernel.Maps[1U].Pages == (Pages - 512U),
                   g_kernel.Maps[1U].MemoryOffset == 512U, g_kernel.Maps[1U].FrameOffset == 512U);
    ASTERIA_EXPECT(runner, "the lines are drawn cell by cell in the font, the rest is background",
                   CellShows(0U, 0U, 'H'), CellShows(0U, 1U, 'i'), CellShows(1U, 0U, 'O'), CellShows(1U, 1U, 'K'),
                   CellShows(0U, 2U, ' '), CellShows(49U, 159U, ' '));

    Component::DisplayMessage reply{};
    const bool read = Component::ReadDisplayFrame(g_kernel.Sent, g_kernel.SentBytes + Header, reply);
    const Component::DisplayInfo info = Component::UnpackDisplayInfo(reply);
    bool fenced{true};
    for (Size row{0U}; row < 16U; ++row)
    {
        for (Size column{0U}; column < 8U; ++column)
        {
            fenced = fenced && (g_kernel.CellAtReply[(row * 8U) + column] == g_scanout[(row * Width) + column]);
        }
    }
    ASTERIA_EXPECT(runner, "the Query is answered only once the lines sent before it are drawn", fenced,
                   CellShows(0U, 0U, 'H'));
    ASTERIA_EXPECT(runner, "the Query is answered on its reply capability with what the broker draws",
                   g_kernel.SentTo == ReplyHandle.Selector, read, reply.Method == Component::DisplayReplyMethod,
                   reply.Transaction == 6U, reply.Words[1U] == Component::DisplayDone,
                   info.Version == Component::DisplayProtocolVersion,
                   info.Features == (Component::DisplayFeatureConsole | Component::DisplayFeaturePointer |
                                     Component::DisplayFeatureMetrics | Component::DisplayFeatureSession),
                   info.Width == Width, info.Height == Height, info.Columns == 160U, info.Rows == 50U,
                   info.Epoch == Incarnation, g_kernel.Closed == ReplyHandle.Selector);
}

void TestPointerAndRights(TestContext& runner) noexcept
{
    g_kernel = DisplayKernel{};
    const UInt8 forbidden[]{'X', 'X', 'X'};
    const UInt8 allowed[]{'Y'};
    g_kernel.ScriptBytes[0U] =
        Header + Component::WriteDisplayConsoleWrite(g_kernel.Script[0U] + Header, 1U, forbidden, sizeof(forbidden));
    g_kernel.Script[0U][14U] = Component::EchoInvokeRight;
    g_kernel.ScriptBytes[1U] =
        Header + Component::WriteDisplayConsoleWrite(g_kernel.Script[1U] + Header, 2U, allowed, sizeof(allowed));
    UInt64 words[8U]{Incarnation, 7U, Component::DisplayRelativeMotion, 0x80000000ULL | (0x80000000ULL << 32U), 1U, 2U,
                     0U,          0U};
    for (Size index{2U}; index <= 4U; ++index)
    {
        words[0U] = index == 3U ? Incarnation + 1U : Incarnation;
        words[1U] = index == 4U ? 6U : 7U;
        g_kernel.ScriptBytes[index] =
            Header + Component::WriteDisplayPointer(g_kernel.Script[index] + Header, 10U + index, words);
        g_kernel.Script[index][14U] = Component::EchoInvokeRight;
    }
    g_kernel.ScriptBytes[5U] = Header + Component::WriteDisplayQuery(g_kernel.Script[5U] + Header, 20U, ReplyHandle);
    g_kernel.Script[5U][14U] = Component::EchoInvokeRight;
    g_kernel.ScriptBytes[6U] = Header + Component::WriteDisplayQuery(g_kernel.Script[6U] + Header, 21U, ReplyHandle);
    g_kernel.Script[6U][14U] = Component::EchoInspectRight;
    g_kernel.Deliveries = 7U;
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    const User::System::SystemCallClient client{{.Function = DisplayKernel::Transport, .Context = &g_kernel}};
    static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout)));
    Component::DisplayMessage reply{};
    ASTERIA_EXPECT(runner, "Invoke queries fence the composed cursor; unauthorized Query sends no reply",
                   Component::ReadDisplayFrame(g_kernel.Sent, Header + g_kernel.SentBytes, reply),
                   reply.Transaction == 20U, reply.Words[7U] == 0U, reply.Words[8U] == 7U, g_kernel.SentCount == 1U);
    ASTERIA_EXPECT(runner, "Invoke moves cursor, stale epoch and sequence do not; Invoke cannot write console",
                   g_scanout[0U] == 0xFFFFFFU, CellShows(0U, 2U, ' '));
}

// Retry both a creation refusal and a later mapping refusal; the latter
// must remove the first WC chunk before any UC chunk is mapped.
void TestUncachedFallback(TestContext& runner) noexcept
{
    for (Size refused{0U}; refused < 3U; ++refused)
    {
        g_kernel = DisplayKernel{};
        g_kernel.RefuseCreate = refused == 0U;
        g_kernel.RefuseMap = refused;
        UInt64 block[Abi::ComponentBlockWords]{};
        StartBlock(block);
        const User::System::SystemCallClient client{
            User::System::SystemCallTransport{&DisplayKernel::Transport, &g_kernel}};
        const UInt64 address = reinterpret_cast<Foundation::UIntPointer>(g_scanout);
        const UInt64 exit = Component::ServeDisplay(client, block, address);
        ASTERIA_EXPECT(runner, "WC refusal retries UC and reaches the serving loop",
                       exit == (Abi::ComponentDisplayFailedExitCode | (Abi::ComponentDisplayWaitStep << 32U) |
                                (static_cast<UInt64>(SystemCallError::InvalidHandle) << 40U)),
                       g_kernel.Creates == 3U, g_kernel.CreatedCache[1U] == Abi::ObjectCreateCacheWriteCombining,
                       g_kernel.CreatedCache[2U] == Abi::ObjectCreateCacheUncached,
                       g_kernel.Destroys == ((refused == 0U) ? 0U : 1U), g_kernel.MapCount == 2U + refused,
                       g_kernel.Unmaps == ((refused == 2U) ? 1U : 0U));
        if (refused == 2U)
        {
            ASTERIA_EXPECT(runner, "partial WC mapping is removed at its exact extent", g_kernel.UnmapStart == address,
                           g_kernel.UnmapPages == 512U);
        }
    }
}

Size g_clockReads{0U};
UInt64 MeasurementClock() noexcept
{
    constexpr UInt64 Readings[]{100U, 32100U, 40000U, 450000U, 500000U, 600000U};
    return Readings[(g_clockReads++) % 6U];
}

void TestMeasurements(TestContext& runner) noexcept
{
    for (const bool fallback : {false, true})
    {
        g_kernel = DisplayKernel{};
        g_kernel.RefuseMap = fallback ? 2U : 0U;
        g_kernel.Batches = 2U;
        g_clockReads = 0U;
        UInt8 scroll[50U]{};
        for (UInt8& byte : scroll)
        {
            byte = '\n';
        }
        g_kernel.ScriptBytes[0U] =
            Header + Component::WriteDisplayConsoleWrite(g_kernel.Script[0U] + Header, 1U, scroll, sizeof(scroll));
        g_kernel.ScriptBytes[1U] = Header + Component::WriteDisplayQuery(g_kernel.Script[1U] + Header, 2U, ReplyHandle);
        // An empty script delivery ends the first drain, before a faster scroll.
        g_kernel.ScriptBytes[3U] =
            Header + Component::WriteDisplayConsoleWrite(g_kernel.Script[3U] + Header, 3U, scroll, 1U);
        g_kernel.ScriptBytes[4U] = Header + Component::WriteDisplayQuery(g_kernel.Script[4U] + Header, 4U, ReplyHandle);
        g_kernel.Deliveries = 5U;
        UInt64 block[Abi::ComponentBlockWords]{};
        StartBlock(block);
        block[Abi::ComponentClockTicksPerMicrosecondWord] = 10U;
        const User::System::SystemCallClient client{{.Function = DisplayKernel::Transport, .Context = &g_kernel}};
        static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout),
                                                  MeasurementClock));
        Component::DisplayMessage message{};
        ASTERIA_EXPECT(runner, "measurements accompany replies after each render",
                       Component::ReadDisplayFrame(g_kernel.Sent, Header + g_kernel.SentBytes, message),
                       g_kernel.SentCount == 2U, g_clockReads == 6U);
        const Component::DisplayInfo info = Component::UnpackDisplayInfo(message);
        ASTERIA_EXPECT(runner, "accepted mapping policy, startup fill, latest and maximum scaled passes are reported",
                       (info.Features & Component::DisplayFeatureTiming) != 0U,
                       info.ScanoutPolicy == (fallback ? Component::DisplayScanoutUc : Component::DisplayScanoutWc),
                       info.FillBytes == Width * Height * 4U, info.FillMicroseconds == 3200U,
                       info.LatestPixels == Width * Height, info.LatestMicroseconds == 10000U,
                       info.SlowestPixels == Width * Height, info.SlowestMicroseconds == 41000U);
    }
}

void TestQueryBatchBoundary(TestContext& runner) noexcept
{
    g_kernel = DisplayKernel{};
    g_kernel.Batches = 2U;
    g_kernel.Deliveries = 9U;
    for (Size query{0U}; query < 9U; ++query)
    {
        g_kernel.ScriptBytes[query] =
            Header + Component::WriteDisplayQuery(g_kernel.Script[query] + Header, query + 1U, ReplyHandle);
    }
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    const User::System::SystemCallClient client{{.Function = DisplayKernel::Transport, .Context = &g_kernel}};
    static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout)));
    Component::DisplayMessage message{};
    ASTERIA_EXPECT(runner, "a full fence batch is completed without losing the ninth Query", g_kernel.SentCount == 9U,
                   g_kernel.Waits == 3U,
                   Component::ReadDisplayFrame(g_kernel.Sent, Header + g_kernel.SentBytes, message),
                   message.Transaction == 9U, Component::UnpackDisplayInfo(message).LatestPixels == 0U);
}

void TestQueryRetryAndLiveness(TestContext& runner) noexcept
{
    g_kernel = DisplayKernel{};
    g_kernel.Batches = 2U;
    g_kernel.BlockedSelector = ReplyHandle.Selector;
    g_kernel.RefusedReplies = 3U;
    g_kernel.CompareRetries = true;
    g_kernel.ScriptBytes[0U] = Header + Component::WriteDisplayQuery(g_kernel.Script[0U] + Header, 1U, ReplyHandle);
    const UInt64 motion[8U]{Incarnation, 1U, Component::DisplayRelativeMotion, 20U, 0U, 1U, 0U, 0U};
    g_kernel.ScriptBytes[2U] = Header + Component::WriteDisplayPointer(g_kernel.Script[2U] + Header, 2U, motion);
    g_kernel.Deliveries = 3U;
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    const User::System::SystemCallClient client{{.Function = DisplayKernel::Transport, .Context = &g_kernel}};
    static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout)));
    Component::DisplayMessage reply{};
    ASTERIA_EXPECT(runner, "WouldBlock retains the exact fenced answer while later pointer input is drawn",
                   g_kernel.RetryUnchanged, g_kernel.SentCount == 4U, g_kernel.Closed == ReplyHandle.Selector,
                   Component::ReadDisplayFrame(g_kernel.Sent, Header + g_kernel.SentBytes, reply),
                   reply.Words[8U] == 0U, g_scanout[Height / 2U * Width + Width / 2U + 20U] == 0xFFFFFFU);

    g_kernel = DisplayKernel{};
    g_kernel.BlockedSelector = ReplyHandle.Selector;
    g_kernel.AlwaysBlock = true;
    for (Size i{0U}; i < 8U; ++i)
    {
        g_kernel.ScriptBytes[i] =
            Header + Component::WriteDisplayQuery(g_kernel.Script[i] + Header, i + 1U, ReplyHandle);
    }
    const UInt8 text[]{'Z'};
    g_kernel.ScriptBytes[8U] =
        Header + Component::WriteDisplayConsoleWrite(g_kernel.Script[8U] + Header, 9U, text, sizeof(text));
    const User::System::CapabilityHandle liveness{ReplyHandle.Selector + 1U, ReplyHandle.Authenticator};
    g_kernel.ScriptBytes[9U] =
        Header + Component::WriteDisplayQuery(g_kernel.Script[9U] + Header, UInt64{1U} << 63U, liveness);
    g_kernel.ScriptBytes[10U] = Header + Component::WriteDisplayPointer(g_kernel.Script[10U] + Header, 10U, motion);
    g_kernel.Deliveries = 11U;
    static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout)));
    bool drawnAfterLiveness{false};
    for (Size row{0U}; row < 16U; ++row)
    {
        for (Size x{0U}; x < 8U; ++x)
        {
            drawnAfterLiveness =
                drawnAfterLiveness || g_kernel.CellAtLiveness[row * 8U + x] != g_scanout[row * Width + x];
        }
    }
    ASTERIA_EXPECT(runner, "eight unread drawing fences cannot block the reserved liveness reply or cursor",
                   g_kernel.LivenessReplies == 1U, g_kernel.Closed == liveness.Selector, drawnAfterLiveness,
                   CellShows(0U, 0U, 'Z'), g_scanout[Height / 2U * Width + Width / 2U + 20U] == 0xFFFFFFU);
}

// A mode it cannot draw ends the broker before it maps anything.
void TestRefusesAnUndrawableMode(TestContext& runner) noexcept
{
    g_kernel = DisplayKernel{};
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    block[Abi::ComponentScanoutModeWord + 1U] = Width;
    const User::System::SystemCallClient client{
        User::System::SystemCallTransport{&DisplayKernel::Transport, &g_kernel}};
    const UInt64 exit = Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout));
    ASTERIA_EXPECT(runner, "a mode without a pixel format is refused unmapped",
                   exit == (Abi::ComponentDisplayFailedExitCode | (Abi::ComponentDisplayModeStep << 32U)),
                   g_kernel.Creates == 0U, g_kernel.MapCount == 0U);
}

void TestOpensSessionFace(TestContext& runner) noexcept
{
    g_kernel = DisplayKernel{};
    UInt64 words[32U]{Incarnation};
    g_kernel.ScriptBytes[0U] =
        Header + User::Session::WriteSessionFaceFrame(g_kernel.Script[0U] + Header, User::Session::FaceOpen, 1U, words,
                                                      &ReplyHandle, 1U);
    g_kernel.Deliveries = 1U;
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    const User::System::SystemCallClient client{{.Function = DisplayKernel::Transport, .Context = &g_kernel}};
    static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout)));
    User::Session::SessionFaceMessage reply{};
    ASTERIA_EXPECT(runner, "the broker opens a Manage face and an Invoke control endpoint on its console route",
                   g_kernel.Creates == 4U, g_kernel.SentCount == 1U,
                   User::Session::ReadSessionFaceFrame(g_kernel.Sent, Header + g_kernel.SentBytes, reply),
                   reply.AttachmentCount == 2U, reply.Words[1U] == Component::DisplayDone);
}
void TestStoppedCompositorCannotBlockCursor(TestContext& runner) noexcept
{
    g_kernel = DisplayKernel{};
    UInt64 face[32U]{Incarnation};
    g_kernel.ScriptBytes[0U] =
        Header + User::Session::WriteSessionFaceFrame(g_kernel.Script[0U] + Header, User::Session::FaceOpen, 1U, face,
                                                      &ReplyHandle, 1U);
    constexpr Size Motions{96U};
    for (Size i{1U}; i <= Motions; ++i)
    {
        // Distinct key stamps prohibit coalescing. No session credit is
        // supplied: the stopped compositor fills then exhausts its queue.
        const UInt64 words[8U]{Incarnation, i, Component::DisplayRelativeMotion, 1U, 0U, 1U, 0U, i};
        g_kernel.ScriptBytes[i] = Header + Component::WriteDisplayPointer(g_kernel.Script[i] + Header, i + 1U, words);
        g_kernel.Script[i][14U] = Component::EchoInvokeRight;
    }
    const UInt8 text[]{'Z'};
    g_kernel.ScriptBytes[Motions + 1U] =
        Header +
        Component::WriteDisplayConsoleWrite(g_kernel.Script[Motions + 1U] + Header, Motions + 2U, text, sizeof(text));
    g_kernel.ScriptBytes[Motions + 2U] =
        Header + Component::WriteDisplayQuery(g_kernel.Script[Motions + 2U] + Header, Motions + 3U, ReplyHandle);
    g_kernel.Deliveries = Motions + 3U;
    g_kernel.Batches = 8U;
    UInt64 block[Abi::ComponentBlockWords]{};
    StartBlock(block);
    const User::System::SystemCallClient client{{.Function = DisplayKernel::Transport, .Context = &g_kernel}};
    static_cast<void>(Component::ServeDisplay(client, block, reinterpret_cast<Foundation::UIntPointer>(g_scanout)));
    Component::DisplayMessage reply{};
    const Size x = Width / 2U + Motions;
    const Size y = Height / 2U;
    ASTERIA_EXPECT(runner, "exhausted session snapshots cannot stop the broker cursor or console",
                   Component::ReadDisplayFrame(g_kernel.Sent, Header + g_kernel.SentBytes, reply),
                   reply.Words[8U] == Motions, reply.Words[7U] == (x | (UInt64{y} << 32U)),
                   g_scanout[y * Width + x] == 0xFFFFFFU, CellShows(0U, 0U, 'Z'));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DisplayBrokerTests"};
    Asteria::Test::TestServesTheConsole(runner);
    Asteria::Test::TestPointerAndRights(runner);
    Asteria::Test::TestUncachedFallback(runner);
    Asteria::Test::TestRefusesAnUndrawableMode(runner);
    Asteria::Test::TestMeasurements(runner);
    Asteria::Test::TestQueryBatchBoundary(runner);
    Asteria::Test::TestQueryRetryAndLiveness(runner);
    Asteria::Test::TestOpensSessionFace(runner);
    Asteria::Test::TestStoppedCompositorCannotBlockCursor(runner);
    return runner.Finish();
}
