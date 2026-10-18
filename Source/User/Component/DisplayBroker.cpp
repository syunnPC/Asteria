// SPDX-License-Identifier: Apache-2.0

#include "User/Component/DisplayBroker.hpp"

#include "Abi/Clock.hpp"

#include "User/Component/DisplaySession.hpp"

#include "User/Component/DisplayFrame.hpp"
#include "User/Component/DisplayPointer.hpp"
#include "User/Graphics/Composition/PlaneStorage.hpp"
#include "User/Component/EchoFrame.hpp"
#include "User/Console/TextConsole.hpp"
#include "User/Graphics/BitmapFont.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/System/DeviceAccess.hpp"

namespace Asteria::User::Component
{
namespace
{
constexpr Foundation::UInt64 PageBytes{4096U};
// The largest scanout one MemoryObject maps, and the most pages one MemoryMap maps on either ISA
// (512).
constexpr Foundation::UInt64 MaximumScanoutPages{8192U};
constexpr Foundation::UInt64 MappingPages{512U};

// The packed console grid and broker workspace have static storage in this module; target call
// frames stay within 512 bytes.
constinit Console::TextConsole g_console{};
namespace Composition = Graphics::Composition;
constinit Composition::DamageList g_damage{};
constinit Composition::PlaneStorage g_planes{};
constinit DisplayPointer g_pointer{};
constinit Composition::Composer g_composer{};
constinit DisplaySession g_session{};
constinit Session::SessionFaceMessage g_faceMessage{};
constinit Identity::SeatMessage g_seatMessage{};
struct PendingQuery
{
    System::CapabilityHandle Reply;
    Foundation::UInt64 Transaction;
    Foundation::Size Bytes;
    alignas(8) Foundation::UInt8 Frame[DisplayReplyBytes];
};
constexpr Foundation::Size DrainLimit{16U};
// And Watchdog.md: unread drawing fences cannot occupy the root's liveness reply. Both kinds retain
// immutable WouldBlock answers.
constexpr Foundation::Size MaximumPendingQueries{8U};
constinit PendingQuery g_queries[MaximumPendingQueries]{};
constinit PendingQuery g_liveness{};
constinit DisplayMessage g_message{};
alignas(8) constinit Foundation::UInt8 g_delivery[EchoDeliveryHeaderBytes + EchoMaximumFrameBytes]{};

[[nodiscard]] constexpr Foundation::UInt64 DisplayFailure(const Foundation::UInt64 step,
                                                          const Abi::SystemCallError error) noexcept
{
    return Abi::ComponentDisplayFailedExitCode | (step << 32U) | (static_cast<Foundation::UInt64>(error) << 40U);
}

// The scanout the start block's mode describes, at address, and its pages;
// false for a mode the broker cannot draw.
[[nodiscard]] bool DecodeScanout(const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                 const Foundation::UInt64 address, Graphics::Surface& surface,
                                 Foundation::UInt64& pages) noexcept
{
    const Foundation::UInt64 size = startBlock[Abi::ComponentScanoutModeWord];
    const Foundation::UInt64 pitch = startBlock[Abi::ComponentScanoutModeWord + 1U];
    surface = Graphics::Surface{
        .Pixels = reinterpret_cast<volatile Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(address)),
        .Width = static_cast<Foundation::UInt32>(size),
        .Height = static_cast<Foundation::UInt32>(size >> 32U),
        .Stride = static_cast<Foundation::UInt32>(pitch),
        .Format = static_cast<Graphics::PixelFormat>(static_cast<Foundation::UInt32>(pitch >> 32U)),
    };
    pages = ((static_cast<Foundation::UInt64>(surface.Stride) * surface.Height * 4U) + PageBytes - 1U) / PageBytes;
    return Graphics::IsUsable(surface) && (pages <= MaximumScanoutPages);
}

// Maps a MemoryObject in bounded chunks. A refusal removes every preceding
// chunk before the caller can retry with a different memory type.
[[nodiscard, gnu::noinline]] Abi::SystemCallError
MapScanoutPages(const System::SystemCallClient& client, const System::CapabilityHandle space,
                const System::CapabilityHandle memory, const System::CapabilityHandle frame,
                const Foundation::UInt64 address, const Foundation::UInt64 pages) noexcept
{
    for (Foundation::UInt64 offset{0U}; offset < pages; offset += MappingPages)
    {
        const Foundation::UInt64 left = pages - offset;
        const Abi::SystemCallError mapped =
            System::MapWindow(client, space, memory, frame, address + (offset * PageBytes),
                              (left < MappingPages) ? left : MappingPages, offset);
        if (mapped == Abi::SystemCallError::None)
        {
            continue;
        }
        for (Foundation::UInt64 undo{0U}; undo < offset; undo += MappingPages)
        {
            const Abi::SystemCallError unmapped =
                client.UnmapMemory(space, address + (undo * PageBytes), MappingPages).Error;
            if (unmapped != Abi::SystemCallError::None)
            {
                return unmapped;
            }
        }
        return mapped;
    }
    return Abi::SystemCallError::None;
}

// One device Frame, with WC preferred and UC on creation/mapping refusal.
// Destroy the refused MemoryObject after removing its mappings: no mixed
// memory-type aliases or leaked pool slot may survive a retry.
[[nodiscard, gnu::noinline]] Foundation::UInt64
MapScanout(const System::SystemCallClient& client, const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
           const Foundation::UInt64 address, const Foundation::UInt64 pages, Foundation::UInt64& policy) noexcept
{
    const System::CapabilityHandle domain{startBlock[Abi::ComponentAccountWord],
                                          startBlock[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{startBlock[Abi::ComponentAccountSpaceWord],
                                         startBlock[Abi::ComponentAccountSpaceWord + 1U]};
    const System::CapabilityHandle region{startBlock[Abi::ComponentDeviceRegionWord],
                                          startBlock[Abi::ComponentDeviceRegionWord + 1U]};
    const Abi::SystemCallResponse frame = System::CreateDeviceFrame(client, domain, region, pages);
    if (frame.Error != Abi::SystemCallError::None)
    {
        return DisplayFailure(Abi::ComponentDisplayFrameStep, frame.Error);
    }
    constexpr Foundation::UInt8 policies[]{Abi::ObjectCreateCacheWriteCombining, Abi::ObjectCreateCacheUncached};
    for (const Foundation::UInt8 cache : policies)
    {
        const Abi::SystemCallResponse memory =
            System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, pages, cache);
        if (memory.Error != Abi::SystemCallError::None)
        {
            if (cache == Abi::ObjectCreateCacheWriteCombining)
            {
                continue;
            }
            return DisplayFailure(Abi::ComponentDisplayMemoryObjectStep, memory.Error);
        }
        const System::CapabilityHandle handle{memory.Value0, memory.Value1};
        const Abi::SystemCallError mapped = MapScanoutPages(
            client, space, handle, System::CapabilityHandle{frame.Value0, frame.Value1}, address, pages);
        if (mapped == Abi::SystemCallError::None)
        {
            policy = cache == Abi::ObjectCreateCacheWriteCombining ? DisplayScanoutWc : DisplayScanoutUc;
            return 0U;
        }
        const Abi::SystemCallError destroyed = client.DestroyObject(handle, Abi::ObjectTypeMemoryObject).Error;
        if (destroyed != Abi::SystemCallError::None)
        {
            return DisplayFailure(Abi::ComponentDisplayMemoryObjectStep, destroyed);
        }
        static_cast<void>(client.CloseCapability(handle));
        if (cache == Abi::ObjectCreateCacheUncached)
        {
            return DisplayFailure(Abi::ComponentDisplayMapStep, mapped);
        }
    }
    return DisplayFailure(Abi::ComponentDisplayMapStep, Abi::SystemCallError::InvalidArguments);
}

void Answer(const System::SystemCallClient& client, PendingQuery& query, const DisplayInfo& info) noexcept
{
    if (query.Reply.Selector == 0U)
    {
        return;
    }
    if (query.Bytes == 0U)
    {
        query.Bytes = WriteDisplayReply(query.Frame, query.Transaction, DisplayDone, info);
    }
    const auto error = client.Send(query.Reply, System::AddressOf(query.Frame), query.Bytes).Error;
    if (error == Abi::SystemCallError::WouldBlock)
    {
        return;
    }
    static_cast<void>(client.CloseCapability(query.Reply));
    query.Reply = {};
    query.Bytes = 0U;
}
void RetryAnswers(const System::SystemCallClient& client, const DisplayInfo& info) noexcept
{
    Answer(client, g_liveness, info);
    for (auto& query : g_queries)
    {
        Answer(client, query, info);
    }
}
[[nodiscard]] bool QueryRetrying() noexcept
{
    if (g_liveness.Reply.Selector != 0U)
    {
        return true;
    }
    for (const auto& query : g_queries)
    {
        if (query.Reply.Selector != 0U)
        {
            return true;
        }
    }
    return false;
}

// Foreign and unauthorized frames release all kernel-installed attachments.
void CloseAttachments(const System::SystemCallClient& client, const Foundation::Size bytes) noexcept
{
    if (bytes < EchoDeliveryHeaderBytes + EchoHeaderBytes)
    {
        return;
    }
    const Foundation::UInt8* const frame = g_delivery + EchoDeliveryHeaderBytes;
    const Foundation::Size start = EchoDeliveryHeaderBytes + EchoHeaderBytes + frame[80U] * 8U;
    for (Foundation::Size index{0U}; index < frame[81U]; ++index)
    {
        const Foundation::Size at = start + index * EchoAttachmentBytes;
        if (at + EchoAttachmentBytes > bytes)
        {
            return;
        }
        static_cast<void>(client.CloseCapability(
            System::CapabilityHandle{System::LoadWord(g_delivery + at), System::LoadWord(g_delivery + at + 8U)}));
    }
}

void Render(const Graphics::Surface& surface, DisplayInfo& info, const ComponentClock clock,
            const Foundation::UInt64 scale, const bool startup = false) noexcept
{
    const Console::ConsoleDamage rows = g_console.Damage();
    const bool fullConsole =
        g_planes.Console().Visible() && (rows.First == 0U) && (rows.End == g_console.Rows()) && (rows.End != 0U);
    if (g_planes.Console().Visible() && g_console.HasDamage())
    {
        g_damage.Add(Graphics::Rect{.X = 0,
                                    .Y = static_cast<Foundation::Int32>(rows.First * 16U),
                                    .Width = g_console.Columns() * 8U,
                                    .Height = (rows.End - rows.First) * 16U});
    }
    const Foundation::UInt64 before = clock == nullptr ? 0U : clock();
    const Composition::RenderStatistics statistics = g_composer.Render(g_damage, &System::OrderScanoutStorage);
    const Foundation::UInt64 ticks = clock == nullptr ? 0U : Abi::ElapsedClockTicks(clock(), before);
    const Foundation::UInt64 microseconds = Composition::DivideCounter(ticks, scale);
    g_console.MarkDrawn();
    if (g_planes.Session().Visible() && statistics.Pixels != 0U)
    {
        info.SessionPixels = statistics.Pixels;
        info.SessionMicroseconds = microseconds;
    }
    if (startup)
    {
        info.FillBytes = statistics.Pixels * 4U;
        info.FillMicroseconds = microseconds;
    }
    else if (fullConsole)
    {
        info.LatestPixels = statistics.Pixels;
        info.LatestMicroseconds = microseconds;
        if ((info.SlowestPixels == 0U) || (microseconds > info.SlowestMicroseconds))
        {
            info.SlowestPixels = statistics.Pixels;
            info.SlowestMicroseconds = microseconds;
        }
    }
    static_cast<void>(surface);
}

[[gnu::noinline]] void FinishPass(const System::SystemCallClient& client, const Graphics::Surface& surface,
                                  DisplayInfo& info, const ComponentClock clock,
                                  const Foundation::UInt64 scale) noexcept
{
    g_session.SendCredit(client);
    Render(surface, info, clock, scale);
    info.PointerX = static_cast<Foundation::UInt32>(g_pointer.Position().X);
    info.PointerY = static_cast<Foundation::UInt32>(g_pointer.Position().Y);
    info.PointerSequence = g_pointer.Sequence();
    g_session.Presented(client);
    RetryAnswers(client, info);
}

// Bound a drain even under a continuous producer. Deferred replies describe
// the pass that follows this batch; later queued messages remain signalled.
[[nodiscard, gnu::noinline]] Foundation::UInt64
Drain(const System::SystemCallClient& client, const System::CapabilityHandle endpoint, const Graphics::Surface& surface,
      DisplayInfo& info, const ComponentClock clock, const Foundation::UInt64 scale) noexcept
{
    Foundation::Size empty{0U};
    Foundation::Size route{0U};
    Foundation::Size count{0U};
    while ((count < DrainLimit) && (empty < 4U))
    {
        const Foundation::Size selected = route;
        route = (route + 1U) % 4U;
        const System::CapabilityHandle receive = selected == 0U   ? endpoint
                                                 : selected == 1U ? g_session.Face()
                                                 : selected == 2U ? g_session.Control()
                                                                  : g_session.TrustedFace();
        if (receive.Selector == 0U)
        {
            ++empty;
            continue;
        }
        Abi::SystemCallResponse received{};
        received = client.Receive(receive, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            ++empty;
            continue;
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            return DisplayFailure(Abi::ComponentDisplayReceiveStep, received.Error);
        }
        empty = 0U;
        ++count;
        const Foundation::Size bytes = static_cast<Foundation::Size>(received.Value1);
        const Foundation::UInt64 rights = g_delivery[14U] | (static_cast<Foundation::UInt64>(g_delivery[15U]) << 8U);
        if (Identity::ReadSeatFrame(g_delivery, bytes, g_seatMessage))
        {
            if (!g_session.DispatchTrusted(client, g_seatMessage, selected))
            {
                CloseAttachments(client, bytes);
            }
            if (g_session.Failed())
            {
                return DisplayFailure(Abi::ComponentDisplayMapStep, Abi::SystemCallError::InvalidConfiguration);
            }
            continue;
        }
        if (Session::ReadSessionFaceFrame(g_delivery, bytes, g_faceMessage))
        {
            if (!g_session.Dispatch(client, g_faceMessage, rights, selected))
            {
                CloseAttachments(client, bytes);
            }
            else if (g_faceMessage.Method == Session::FaceShow && (g_faceMessage.Words[1U] & 1U) != 0U)
            {
                g_console.Clear();
            }
            if (g_session.Failed())
            {
                return DisplayFailure(Abi::ComponentDisplayMapStep, Abi::SystemCallError::InvalidConfiguration);
            }
            // In-drain answers precede the potentially long render.
            g_session.SendCredit(client);
            continue;
        }
        DisplayMessage& message = g_message;
        if ((selected != 0U) || !ReadDisplayFrame(g_delivery, bytes, message))
        {
            CloseAttachments(client, bytes);
            continue;
        }
        if (message.Method == DisplayConsoleWriteMethod)
        {
            if ((rights & EchoManageRight) != 0U)
            {
                g_console.Write(message.Text, message.Count);
            }
        }
        else if (message.Method == DisplayPointerMethod)
        {
            if (g_session.Trusted())
            {
                CloseAttachments(client, bytes);
                continue;
            }
            // Drawing the cursor never depends on the compositor taking a SessionCredit or
            // Presented answer.
            if (((rights & EchoInvokeRight) != 0U) && g_pointer.Apply(message.Words))
            {
                g_session.Pointer(message.Words, g_pointer.Position());
                g_session.SendCredit(client);
                g_damage.Add(g_planes.Cursor().Bounds());
                g_planes.Cursor().Move(g_pointer.Position());
                g_damage.Add(g_planes.Cursor().Bounds());
            }
        }
        else if ((message.Method == DisplayQueryMethod) && ((rights & EchoInvokeRight) != 0U))
        {
#if defined(ASTERIA_COMPONENT_WATCHDOG_PROBE) && ASTERIA_COMPONENT_WATCHDOG_PROBE
            // Test firmware's first tagged incarnation withholds only liveness
            // Queries; normal drawing fences remain observable during recovery.
            if (info.Epoch == 0x8000'0000'0000'0100ULL && (message.Transaction >> 63U) != 0U)
            {
                CloseAttachments(client, bytes);
                continue;
            }
#endif
            PendingQuery* query{nullptr};
            if ((rights & EchoManageRight) != 0U && (message.Transaction >> 63U) != 0U)
            {
                if (g_liveness.Reply.Selector == 0U)
                {
                    query = &g_liveness;
                }
            }
            else
            {
                for (auto& pending : g_queries)
                {
                    if (pending.Reply.Selector == 0U)
                    {
                        query = &pending;
                        break;
                    }
                }
                if (query == nullptr)
                {
                    // Complete this bounded fence batch before admitting
                    // another. Unread replies keep their frozen snapshots.
                    FinishPass(client, surface, info, clock, scale);
                    for (auto& pending : g_queries)
                    {
                        if (pending.Reply.Selector == 0U)
                        {
                            query = &pending;
                            break;
                        }
                    }
                }
            }
            if (query == nullptr)
            {
                CloseAttachments(client, bytes);
                continue;
            }
            query->Reply = message.Reply;
            query->Transaction = message.Transaction;
            query->Bytes = 0U;
            // Watchdog Queries test service progress, independently of the
            // drawing fence. Reply before a potentially slow render.
            if (query == &g_liveness)
            {
                Answer(client, *query, info);
            }
        }
        else
        {
            CloseAttachments(client, bytes);
        }
    }
    FinishPass(client, surface, info, clock, scale);
    return 0U;
}
} // namespace

Foundation::UInt64 ServeDisplay(const System::SystemCallClient& client,
                                const Foundation::UInt64 (&startBlock)[Abi::ComponentBlockWords],
                                const Foundation::UInt64 address, const ComponentClock clock) noexcept
{
    Graphics::Surface surface{};
    Foundation::UInt64 pages{0U};
    if (!DecodeScanout(startBlock, address, surface, pages))
    {
        return DisplayFailure(Abi::ComponentDisplayModeStep, Abi::SystemCallError::None);
    }
    DisplayInfo info{};
    info.Version = DisplayProtocolVersion;
    info.Features = DisplayFeatureConsole | DisplayFeaturePointer | DisplayFeatureMetrics | DisplayFeatureSession;
    const Foundation::UInt64 scale = startBlock[Abi::ComponentClockTicksPerMicrosecondWord];
    if ((clock != nullptr) && (scale != 0U))
    {
        info.Features |= DisplayFeatureTiming;
    }
    const Foundation::UInt64 mapped = MapScanout(client, startBlock, address, pages, info.ScanoutPolicy);
    if (mapped != 0U)
    {
        return mapped;
    }
    g_console.Configure(surface.Width, surface.Height, Graphics::Bold8x16);
    g_composer.Reset();
    g_planes.Initialize(g_console);
    g_pointer.Configure(surface.Width, surface.Height, startBlock[Abi::ComponentIncarnationWord]);
    g_planes.Cursor().Move(g_pointer.Position());
    g_planes.Cursor().Show(true);
    g_damage.Configure(Graphics::Bounds(surface));
    if (!g_composer.AddOutput(Composition::Output{.Scanout = surface, .Origin = {.X = 0, .Y = 0}, .Scale = 1U}) ||
        !g_composer.AddPlane(g_planes.Background()) || !g_composer.AddPlane(g_planes.Session()) ||
        !g_composer.AddPlane(g_planes.Console()) || !g_composer.AddPlane(g_planes.Cursor()) ||
        !g_composer.AddPlane(g_planes.Trusted()))
    {
        return DisplayFailure(Abi::ComponentDisplayModeStep, Abi::SystemCallError::None);
    }
    g_damage.Add(Graphics::Bounds(surface));
    info.Width = surface.Width;
    info.Height = surface.Height;
    info.Columns = g_console.Columns();
    info.Rows = g_console.Rows();
    info.Epoch = startBlock[Abi::ComponentIncarnationWord];
    g_session.Configure({startBlock[Abi::ComponentAccountWord], startBlock[Abi::ComponentAccountWord + 1U]},
                        {startBlock[Abi::ComponentAccountSpaceWord], startBlock[Abi::ComponentAccountSpaceWord + 1U]},
                        {startBlock[Abi::ComponentWaitSetWord], startBlock[Abi::ComponentWaitSetWord + 1U]},
                        address + Abi::ComponentSessionOffsetBytes, surface.Format, info, g_planes, g_damage);
    g_planes.Trusted().Configure(Graphics::Bounds(surface));
    Render(surface, info, clock, scale, true);
    const System::CapabilityHandle endpoint{startBlock[Abi::ComponentReceiveWord],
                                            startBlock[Abi::ComponentReceiveWord + 1U]};
    const System::CapabilityHandle waitSet{startBlock[Abi::ComponentWaitSetWord],
                                           startBlock[Abi::ComponentWaitSetWord + 1U]};
    g_liveness.Reply = {};
    g_liveness.Bytes = 0U;
    for (auto& query : g_queries)
    {
        query.Reply = {};
        query.Bytes = 0U;
    }
    for (;;)
    {
        RetryAnswers(client, info);
        const Abi::SystemCallResponse waited =
            client.Wait(waitSet, g_session.Retrying() || QueryRetrying() ? 10'000U : 0U);
        if (waited.Error != Abi::SystemCallError::None && waited.Error != Abi::SystemCallError::TimedOut)
        {
            return DisplayFailure(Abi::ComponentDisplayWaitStep, waited.Error);
        }
        const Foundation::UInt64 failure = Drain(client, endpoint, surface, info, clock, scale);
        if (failure != 0U)
        {
            return failure;
        }
    }
}
} // Namespace Asteria::User::Component
