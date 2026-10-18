// SPDX-License-Identifier: Apache-2.0
#include "Guest/PatternClient.hpp"
#include "User/Session/SessionFrame.hpp"
#include "User/Session/ClientEvents.hpp"
#include "User/Session/SessionFaceFrame.hpp"
#include "User/Graphics/Blit.hpp"
#include "Inventory/ReportText.hpp"
#include "User/System/DeviceAccess.hpp"
#if defined(ASTERIA_TARGET_IA64)
#include "User/Platform/Ia64/System/Ia64SystemCallTransport.hpp"
#else
#include "User/Platform/Amd64/System/Amd64SystemCallTransport.hpp"
#endif
namespace Asteria::Test
{
namespace
{
namespace Component = Asteria::User::Component;
namespace Graphics = Asteria::User::Graphics;
namespace Session = Asteria::User::Session;
namespace System = Asteria::User::System;
using Foundation::UInt64;
using Foundation::Size;
using K = Session::SessionEventKind;
constexpr auto Width = PatternWidth;
constexpr auto Height = PatternHeight;
constexpr auto PatchWidth = PatternPatchWidth;
constexpr auto PatchHeight = PatternPatchHeight;
constinit Session::SessionMessage g_message{};
constinit Session::SessionFaceMessage g_command{};
constinit Session::SessionEvent g_event{};
constinit System::CapabilityHandle g_frames[2U]{};
constinit System::CapabilityHandle g_memories[4U]{};
constinit System::CapabilityHandle g_four[4U]{};
constinit System::CapabilityHandle g_nativeFrames[4U][Session::SessionMaximumPoolFrames]{};
UInt64 g_nativePages[Session::SessionMaximumPoolFrames]{};
Size g_nativeCount{0U};
constinit Graphics::MemorySurface g_images[4U]{};
constinit UInt64 g_words[32U]{};
alignas(8) constinit Foundation::UInt8 g_delivery[Session::SessionDeliveryCapacity]{},
    g_frame[Session::SessionKernelFrameLimit]{};
UInt64 g_imagePages{0U};
UInt64 g_poolPages{0U};
UInt64 g_transaction{0U};
UInt64 g_epoch{0U};
UInt64 g_returned{0U};
UInt64 g_shown{0U};
UInt64 g_arrange{0U};
bool g_commandPending{false};
bool g_largeMode{false};
bool g_nativeStorage{false};
bool g_singleImagePools{false};
System::CapabilityHandle g_server{};
System::CapabilityHandle g_reply{};
System::CapabilityHandle g_receive{};
System::CapabilityHandle g_wait{};
System::CapabilityHandle g_report{};
System::CapabilityHandle g_servers[2U]{};
System::CapabilityHandle g_receivers[2U]{};
System::CapabilityHandle g_replies[2U]{};
bool g_independent{false};
Size g_active{0U};
void Select(const Size slot) noexcept
{
    g_active = slot;
    g_server = g_servers[slot];
    g_reply = g_replies[slot];
    g_receive = g_receivers[slot];
}
UInt64 g_seed{0U};
char g_line[Component::DisplayMaximumWriteBytes]{};
[[nodiscard, gnu::noinline]] bool RetryPause(const System::SystemCallClient& client) noexcept
{
    // The test client's timer-only WaitSet yields a full turn even when replies are unread. Retain
    // the exact outbound frame on retry.
    const auto waited = client.Wait(g_wait, Session::ClientRetryUs).Error;
    return waited == Abi::SystemCallError::None || waited == Abi::SystemCallError::TimedOut;
}
[[nodiscard]] bool Transmit(const System::SystemCallClient& client, const System::CapabilityHandle target,
                            const Size bytes) noexcept
{
    for (Size attempt{0U}; attempt < 200U; ++attempt)
    {
        const auto error = client.Send(target, System::AddressOf(g_frame), bytes).Error;
        if (error != Abi::SystemCallError::WouldBlock)
        {
            return error == Abi::SystemCallError::None;
        }
        if (!RetryPause(client))
        {
            return false;
        }
    }
    return false;
}
[[nodiscard]] bool LogEvent(const System::SystemCallClient& client) noexcept
{
    Inventory::ReportText text{g_line, sizeof(g_line)};
    text.Text("# window-event: ");
    text.Decimal(g_independent ? g_active + 1U : g_seed);
    text.Character(' ');
    text.Decimal(static_cast<UInt64>(g_event.Kind));
    for (const auto word : g_event.Words)
    {
        text.Character(' ');
        text.Decimal(word);
    }
    text.Character(' ');
    text.Decimal(g_message.Words[3U]);
    text.Character('\n');
    const auto bytes = Component::WriteDisplayConsoleWrite(
        g_frame, g_transaction, reinterpret_cast<const Foundation::UInt8*>(g_line), text.Bytes());
    return Transmit(client, g_report, bytes);
}

void Words(const UInt64 first = 0U) noexcept
{
    for (auto& w : g_words)
    {
        w = 0U;
    }
    g_words[0U] = first;
}
[[nodiscard]] bool Send(const System::SystemCallClient& client, const Foundation::UInt32 method,
                        const System::CapabilityHandle* handles = nullptr, const Size count = 0U) noexcept
{
    const auto bytes = Session::WriteSessionFrame(g_frame, method, ++g_transaction, g_words, handles, count);
    if (bytes == 0U)
    {
        return false;
    }
    return Transmit(client, g_server, bytes);
}
[[nodiscard]] bool Credit(const System::SystemCallClient& client) noexcept
{
    Words(g_epoch);
    g_words[1U] = Session::SessionMaximumEvents;
    return Send(client, Session::SessionCreditMethod, &g_reply, 1U);
}
[[nodiscard]] bool Events(const System::SystemCallClient& client, const K wanted, const UInt64 id) noexcept
{
    for (Size attempt{0U}; attempt < 6000U; ++attempt)
    {
        const auto received = client.Receive(g_receive, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            const auto waited = client.Wait(g_wait, Session::ClientRetryUs).Error;
            if (waited != Abi::SystemCallError::None && waited != Abi::SystemCallError::TimedOut)
            {
                return false;
            }
            continue;
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            return false;
        }
        if (Session::ReadSessionFaceFrame(g_delivery, static_cast<Size>(received.Value1), g_command))
        {
            g_commandPending = true;
            continue;
        }
        if (!Session::ReadSessionFrame(g_delivery, static_cast<Size>(received.Value1), g_message) ||
            g_message.Method != Session::SessionEventsMethod)
        {
            return false;
        }
        bool found{false};
        for (Size i{0U}; i < Session::SessionEventCount(g_message); ++i)
        {
            if (!Session::GetSessionEvent(g_message, i, g_event) || !LogEvent(client))
            {
                return false;
            }
            if (g_event.Kind == K::ImageReturned)
            {
                g_returned |= g_event.Words[0U];
            }
            if (g_event.Kind == K::FrameShown)
            {
                g_shown = g_event.Words[1U];
            }
            if (g_event.Kind == K::Arrange)
            {
                g_arrange = g_event.Words[1U];
            }
            if (g_event.Kind == wanted && (id == 0U || g_event.Words[0U] == id))
            {
                found = true;
            }
            if (g_event.Kind == K::PoolRefused || (g_event.Kind == K::RequestRefused && wanted != K::ImageRefused))
            {
                return false;
            }
            if (g_event.Kind == K::ImageRefused &&
                (g_event.Words[0U] != 8U || g_event.Words[1U] != static_cast<UInt64>(Session::SessionRefusal::Range)))
            {
                return false;
            }
        }
        if (!Credit(client))
        {
            return false;
        }
        if (found)
        {
            return true;
        }
    }
    return false;
}
[[nodiscard]] bool Pool(const System::SystemCallClient& client, const Size index) noexcept
{
    // The codec attenuates every attachment to exactly Map, while the producer keeps CreatedRights
    // for its own writable view.
    if (g_nativeStorage)
    {
        const UInt64 id = g_singleImagePools ? index % 2U + 1U : g_independent ? 1U : index + 1U;
        Words(id);
        g_words[1U] = g_poolPages;
        g_words[2U] = g_nativeCount;
        for (Size i{0U}; i < g_nativeCount; ++i)
        {
            g_words[4U + (i >> 2U)] |= g_nativePages[i] << ((i & 3U) * 16U);
        }
        const Size firstCount =
            g_nativeCount < Session::SessionMaximumAttachments ? g_nativeCount : Session::SessionMaximumAttachments;
        if (!Send(client, Session::SessionCreatePoolMethod, g_nativeFrames[index], firstCount))
        {
            return false;
        }
        for (Size first{firstCount}; first < g_nativeCount; first += Session::SessionMaximumAttachments)
        {
            const Size remaining = g_nativeCount - first;
            const Size attached =
                remaining < Session::SessionMaximumAttachments ? remaining : Session::SessionMaximumAttachments;
            Words(id);
            g_words[1U] = first;
            if (!Send(client, Session::SessionAddPoolFramesMethod, g_nativeFrames[index] + first, attached))
            {
                return false;
            }
        }
        return Events(client, K::PoolReady, id);
    }
    Words(index + 1U);
    g_words[1U] = g_poolPages;
    g_words[2U] = 1U;
    g_words[4U] = g_poolPages;
    return Send(client, Session::SessionCreatePoolMethod, &g_frames[index], 1U) &&
           Events(client, K::PoolReady, index + 1U);
}
[[nodiscard, gnu::noinline]] bool MapProducer(const System::SystemCallClient& client,
                                              const System::CapabilityHandle space,
                                              const System::CapabilityHandle memory,
                                              const System::CapabilityHandle frame, const UInt64 address,
                                              const UInt64 pages, const UInt64 offset) noexcept
{
    // Distinct owned Frames start at zero; the pool offset advances.
    alignas(8) Foundation::UInt8 request[Abi::FaultResolveRequestSizeBytes]{};
    System::StoreHandle(request + Abi::FaultResolveAddressSpaceOffset, space);
    System::StoreHandle(request + Abi::FaultResolveMemoryObjectOffset, memory);
    System::StoreHandle(request + Abi::FaultResolveFrameOffset, frame);
    System::StoreWord(request + Abi::FaultResolveVirtualStartOffset, address);
    System::StoreWord(request + Abi::FaultResolvePageCountOffset, pages);
    System::StoreWord(request + Abi::FaultResolveMemoryObjectPageOffset, offset);
    request[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
    return client.MapMemory(System::AddressOf(request)).Error == Abi::SystemCallError::None;
}
[[nodiscard]] bool Define(const System::SystemCallClient& client, const UInt64 pool, const UInt64 image,
                          const Foundation::UInt32 width = Width, const Foundation::UInt32 height = Height,
                          const UInt64 offset = 0U) noexcept
{
    Words(pool);
    g_words[1U] = image;
    g_words[2U] = offset;
    g_words[3U] = Session::PackSessionPair(width, height);
    g_words[4U] = width;
    g_words[5U] = static_cast<UInt64>(g_images[0U].Format);
    return Send(client, Session::SessionDefineImageMethod);
}
// Keep native allocation separate from the small test start.
[[nodiscard, gnu::noinline]] UInt64 InitializeNative(const System::SystemCallClient& client,
                                                     const UInt64 (&block)[Abi::ComponentBlockWords],
                                                     const System::CapabilityHandle domain,
                                                     const System::CapabilityHandle space) noexcept
{
    const auto mode = block[Abi::ComponentSessionOutputModeWord];
    g_imagePages = (static_cast<UInt64>(static_cast<Foundation::UInt32>(mode)) * (mode >> 32U) * 4U +
                    Session::SessionPageBytes - 1U) /
                   Session::SessionPageBytes;
    g_singleImagePools = 2U * g_imagePages > Session::SessionMaximumPoolPages;
    g_poolPages = g_singleImagePools ? g_imagePages : 2U * g_imagePages;
    if (g_poolPages == 0U || g_poolPages > Session::SessionMaximumPoolPages)
    {
        return 0xC0DE00EFU;
    }
    g_nativeCount =
        static_cast<Size>((g_poolPages + Session::SessionMaximumFramePages - 1U) / Session::SessionMaximumFramePages);
    const Size pools = g_singleImagePools && (block[Abi::ComponentMountFlagsWord] >> 32U) >= 3U ? 4U : 2U;
    // Native capacity preserves each initial image's geometry.
    const auto initialHeight = (block[Abi::ComponentMountFlagsWord] >> 32U) == 2U ? PatternSecondaryHeight : Height;
    for (Size pool{0U}; pool < pools; ++pool)
    {
        const auto memory = System::CreateMemory(client, domain, Abi::ObjectTypeMemoryObject, g_poolPages);
        if (memory.Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00E0U;
        }
        g_memories[pool] = {memory.Value0, memory.Value1};
        const UInt64 address = block[Abi::ComponentWindowStartWord] + pool * Session::SessionMaximumPoolBytes;
        UInt64 offset{0U};
        for (Size i{0U}; i < g_nativeCount; ++i)
        {
            const UInt64 remaining = g_poolPages - offset;
            g_nativePages[i] =
                remaining < Session::SessionMaximumFramePages ? remaining : Session::SessionMaximumFramePages;
            const auto owned = System::CreateRamFrame(client, domain, g_nativePages[i]);
            if (owned.Error != Abi::SystemCallError::None)
            {
                return 0xC0DE00E0U;
            }
            g_nativeFrames[pool][i] = {owned.Value0, owned.Value1};
            if (!MapProducer(client, space, g_memories[pool], g_nativeFrames[pool][i],
                             address + offset * Session::SessionPageBytes, g_nativePages[i], offset))
            {
                return 0xC0DE00E1U;
            }
            offset += g_nativePages[i];
        }
        for (Size back{0U}; back < (g_singleImagePools ? 1U : 2U); ++back)
        {
            g_images[g_singleImagePools ? pool : pool * 2U + back] = {
                .Pixels = reinterpret_cast<Foundation::UInt32*>(
                    static_cast<Foundation::UIntPointer>(address + back * g_imagePages * Session::SessionPageBytes)),
                .Width = Width,
                .Height = initialHeight,
                .Stride = Width,
                .Format = static_cast<Graphics::PixelFormat>(
                    static_cast<Foundation::UInt32>(block[Abi::ComponentMountFlagsWord]))};
        }
    }
    g_frames[0U] = g_nativeFrames[0U][0U];
    return 0U;
}
[[nodiscard, gnu::noinline]] UInt64 Initialize(const System::SystemCallClient& client,
                                               const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const System::CapabilityHandle domain{block[Abi::ComponentAccountWord], block[Abi::ComponentAccountWord + 1U]};
    const System::CapabilityHandle space{block[Abi::ComponentAccountSpaceWord],
                                         block[Abi::ComponentAccountSpaceWord + 1U]};
    g_server = {block[Abi::ComponentInputServiceWord], block[Abi::ComponentInputServiceWord + 1U]};
    g_reply = {block[Abi::ComponentReplyWord], block[Abi::ComponentReplyWord + 1U]};
    g_receive = {block[Abi::ComponentReceiveWord], block[Abi::ComponentReceiveWord + 1U]};
    g_wait = {block[Abi::ComponentWaitSetWord], block[Abi::ComponentWaitSetWord + 1U]};
    g_epoch = block[Abi::ComponentInputEpochWord];
    const auto seed = block[Abi::ComponentMountFlagsWord] >> 32U;
    g_largeMode = seed == 4U;
    g_independent = seed == 5U;
    g_seed = seed >= 3U ? 1U : seed;
    g_report = {block[Abi::ComponentSessionReportWord], block[Abi::ComponentSessionReportWord + 1U]};
    const auto initialHeight = seed == 2U ? PatternSecondaryHeight : Height;
    const auto mode = block[Abi::ComponentSessionOutputModeWord];
    g_imagePages = (static_cast<UInt64>(static_cast<Foundation::UInt32>(mode)) * (mode >> 32U) * 4U +
                    Session::SessionPageBytes - 1U) /
                   Session::SessionPageBytes;
    g_nativeStorage =
        mode != Session::PackSessionPair(Abi::ComponentSessionTestWidth, Abi::ComponentSessionTestHeight) ||
        g_independent;
    g_poolPages = g_imagePages * 2U;
    if (seed == 3U)
    {
        g_imagePages = Abi::ComponentPatternInputPages / 4U;
        g_poolPages = Abi::ComponentPatternInputPages;
    }
    if (g_nativeStorage)
    {
        const auto initialized = InitializeNative(client, block, domain, space);
        if (initialized != 0U)
        {
            return initialized;
        }
        g_independent = g_independent || (g_singleImagePools && seed >= 3U);
    }
    else
    {
        const auto frame = System::CreateMemory(client, domain, Abi::ObjectTypeFrame, g_poolPages);
        if (frame.Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00E0U;
        }
        g_frames[0U] = {frame.Value0, frame.Value1};
        g_memories[0U] = {block[Abi::ComponentPatternMemoryWord], block[Abi::ComponentPatternMemoryWord + 1U]};
        const UInt64 address = block[Abi::ComponentWindowStartWord];
        alignas(8) Foundation::UInt8 map[Abi::FaultResolveRequestSizeBytes]{};
        System::StoreHandle(map + Abi::FaultResolveAddressSpaceOffset, space);
        System::StoreHandle(map + Abi::FaultResolveMemoryObjectOffset, g_memories[0U]);
        System::StoreHandle(map + Abi::FaultResolveFrameOffset, g_frames[0U]);
        System::StoreWord(map + Abi::FaultResolveVirtualStartOffset, address);
        System::StoreWord(map + Abi::FaultResolvePageCountOffset, g_poolPages);
        System::StoreWord(map + Abi::FaultResolveMemoryObjectPageOffset, block[Abi::ComponentPatternMemoryOffsetWord]);
        map[Abi::FaultResolvePermissionsOffset] = System::ReadWriteUser;
        if (client.MapMemory(System::AddressOf(map)).Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00E1U;
        }
        for (Size i{0U}; i < (seed >= 3U ? 4U : 2U); ++i)
        {
            g_images[i] = {.Pixels = reinterpret_cast<Foundation::UInt32*>(static_cast<Foundation::UIntPointer>(
                               address + i * g_imagePages * Session::SessionPageBytes)),
                           .Width = Width,
                           .Height = initialHeight,
                           .Stride = Width,
                           .Format = static_cast<Graphics::PixelFormat>(
                               static_cast<Foundation::UInt32>(block[Abi::ComponentMountFlagsWord]))};
        }
    }
    if (!Credit(client) || !Events(client, K::Welcome, 0U))
    {
        return 0xC0DE00E2U;
    }
    if (seed >= 3U)
    {
        // The bootstrap separates Read from Invoke|Duplicate. Switching clients must retain the
        // send copy used by credit attachments.
        g_servers[0U] = g_server;
        g_receivers[0U] = g_receive;
        g_replies[0U] = g_reply;
        if (!Pool(client, 0U) || (g_singleImagePools && !Pool(client, 1U)))
        {
            return 0xC0DE00E3U;
        }
        if (!g_independent)
        {
            return g_nativeStorage && !Pool(client, 1U) ? 0xC0DE00E3U : 0U;
        }
        alignas(8) Foundation::UInt8 request[Abi::ObjectCreateEndpointRequestSizeBytes]{};
        System::BuildDefaultEndpointRequest(request);
        const auto second = client.CreateEndpoint(domain, System::AddressOf(request));
        if (second.Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00D3U;
        }
        g_receivers[1U] = {second.Value0, second.Value1};
        g_replies[1U] = g_receivers[1U];
        // Both routes use the timer-only 10-ms turn; persistent readiness must not turn
        // Wait/WouldBlock retries into a spin.
        g_servers[1U] = {block[Abi::ComponentPatternSecondClientWord],
                         block[Abi::ComponentPatternSecondClientWord + 1U]};
        Select(1U);
        if (!Credit(client) || !Events(client, K::Welcome, 0U) ||
            !Pool(client, g_singleImagePools ? 2U
                          : g_nativeStorage  ? 1U
                                             : 0U) ||
            (g_singleImagePools && !Pool(client, 3U)))
        {
            return 0xC0DE00D5U;
        }
        Select(0U);
        return 0U;
    }
    // The current grant cannot hold sixteen distinct new Frames beside three
    // resident images. Exercise all sixteen transfers and records using aliases
    // of one owned Frame; the double buffers are disjoint images in that Frame.
    Words(1U);
    g_words[1U] = 16U;
    g_words[2U] = 16U;
    for (Size i{4U}; i < 8U; ++i)
    {
        g_words[i] = 0x0001'0001'0001'0001ULL;
    }
    for (auto& handle : g_four)
    {
        const auto copy = client.DuplicateCapability(
            g_frames[0U], Session::SessionMapRight | Session::SessionDuplicateRight, ~UInt64{0U});
        if (copy.Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00FCU;
        }
        handle = {copy.Value0, copy.Value1};
    }
    if (!Send(client, Session::SessionCreatePoolMethod, g_four, 4U))
    {
        return 0xC0DE00E3U;
    }
    for (Size first{4U}; first < 16U; first += 4U)
    {
        Words(1U);
        g_words[1U] = first;
        if (!Send(client, Session::SessionAddPoolFramesMethod, g_four, 4U))
        {
            return 0xC0DE00E4U;
        }
    }
    if (!Events(client, K::PoolReady, 1U))
    {
        return 0xC0DE00E5U;
    }
    for (const auto handle : g_four)
    {
        static_cast<void>(client.CloseCapability(handle));
    }
    Words(1U);
    if (!Send(client, Session::SessionDestroyPoolMethod) || !Events(client, K::PoolDestroyed, 1U) ||
        !Pool(client, 0U) || (g_singleImagePools && !Pool(client, 1U)) ||
        !Define(client, 1U, 1U, Width, initialHeight) ||
        !Define(client, g_singleImagePools ? 2U : 1U, 2U, Width, initialHeight,
                g_singleImagePools ? 0U : g_imagePages * Session::SessionPageBytes) ||
        !Define(client, 1U, 8U, 1U, 1U, g_poolPages * Session::SessionPageBytes) ||
        !Events(client, K::ImageRefused, 8U))
    {
        return 0xC0DE00E6U;
    }
    Words(1U);
    g_words[1U] = Session::SessionToplevelRole;
    if (!Send(client, Session::SessionCreateSurfaceMethod))
    {
        return 0xC0DE00E7U;
    }
    Words(1U);
    g_words[1U] = 5U;
    constexpr char subtitle[]{"tiles"};
    for (Size i{0U}; i < 5U; ++i)
    {
        g_words[2U] |= static_cast<UInt64>(subtitle[i]) << (i * 8U);
    }
    if (!Send(client, Session::SessionSetSubtitleMethod))
    {
        return 0xC0DE00E8U;
    }
    return 0U;
}
// Two windows in one process leave a component slot for the PS/2 driver. Four disjoint images in
// the small Frame or native pools; no alias is redrawn while lent. Every Arrange is acknowledged by
// a complete re-render.
struct InputWindow
{
    UInt64 Arrange{0U};
    UInt64 Serial{0U};
    Foundation::UInt32 Width{0U};
    Foundation::UInt32 Height{0U};
    Size Back{0U};
    bool Pending{false};
    bool Closed{false};
};
constinit InputWindow g_windows[2U]{};
bool g_hung{false};
[[nodiscard]] bool Repaint(const System::SystemCallClient& client, const Size surface) noexcept
{
    auto& w = g_windows[surface];
    const Size image = surface * 2U + w.Back;
    if ((g_returned & (1ULL << image)) == 0U)
    {
        return true;
    }
    if (!Session::SessionImageFits(0U, w.Width, w.Height, w.Width, g_imagePages * Session::SessionPageBytes))
    {
        return false;
    }
    if (++w.Serial == 2U)
    {
        ++w.Serial;
    } // serial 2 is patch fixture.
    auto& view = g_images[image];
    view.Width = w.Width;
    view.Height = w.Height;
    view.Stride = w.Width;
    if (!Define(client,
                g_singleImagePools                  ? w.Back + 1U
                : g_nativeStorage && !g_independent ? surface + 1U
                                                    : 1U,
                image + 1U, w.Width, w.Height,
                (g_singleImagePools ? 0U
                 : g_nativeStorage  ? w.Back
                                    : image) *
                    g_imagePages * Session::SessionPageBytes))
    {
        return false;
    }
    for (Foundation::UInt32 y{0U}; y < w.Height; ++y)
    {
        for (Foundation::UInt32 x{0U}; x < w.Width; ++x)
        {
            view.Pixels[static_cast<Size>(y) * view.Stride + x] = Graphics::PackColor(
                view.Format, {.Red = static_cast<Foundation::UInt8>(x + w.Serial * 17U +
                                                                    (g_independent ? surface + 1U : g_seed) * 31U),
                              .Green = static_cast<Foundation::UInt8>(y * 2U + w.Serial * 11U +
                                                                      (g_independent ? surface + 1U : g_seed) * 7U),
                              .Blue = static_cast<Foundation::UInt8>((x ^ y) + w.Serial * 23U +
                                                                     (g_independent ? surface + 1U : g_seed) * 13U)});
        }
    }
    System::OrderDmaStorage();
    g_returned &= ~(1ULL << image);
    Words(g_independent ? 1U : surface + 1U);
    g_words[1U] = image + 1U;
    g_words[2U] = w.Serial;
    g_words[3U] = Session::SessionWantFrameShown;
    g_words[4U] = w.Arrange;
    g_words[5U] = 1U;
    g_words[6U] = Session::PackSessionDamage(0U, 0U, w.Width, w.Height);
    if (!Send(client, Session::SessionPresentMethod))
    {
        return false;
    }
    w.Back ^= 1U;
    w.Pending = false;
    return true;
}
[[nodiscard]] UInt64 RunInput(const System::SystemCallClient& client) noexcept
{
    g_returned = 15U;
    for (auto& window : g_windows)
    {
        window.Width = g_largeMode ? Test::PatternNativeInitialWidth : Abi::ComponentPatternInputInitialWidth;
        window.Height = g_largeMode ? Test::PatternNativeInitialHeight : Abi::ComponentPatternInputHeight;
    }

    for (Size s{0U}; s < 2U; ++s)
    {
        if (g_independent)
        {
            Select(s);
        }
        Words(g_independent ? 1U : s + 1U);
        g_words[1U] = Session::SessionToplevelRole;
        if (!Send(client, Session::SessionCreateSurfaceMethod))
        {
            return 0xC0DE00A0U;
        }
        if (g_largeMode)
        {
            // The native arena case also proves kerning/ligatures in a wide title.
            constexpr char subtitle[]{"office ffi AV"};
            Words(g_independent ? 1U : s + 1U);
            g_words[1U] = sizeof(subtitle) - 1U;
            for (Size i{0U}; i < sizeof(subtitle) - 1U; ++i)
            {
                g_words[2U + i / 8U] |= static_cast<UInt64>(subtitle[i]) << ((i % 8U) * 8U);
            }
            if (!Send(client, Session::SessionSetSubtitleMethod))
            {
                return 0xC0DE00A0U;
            }
        }
        Words(g_independent ? 1U : s + 1U);
        g_words[1U] = Session::PackSessionPair(48U, 16U);
        g_words[2U] = 0U;
        g_words[3U] = g_largeMode ? Session::PackSessionPair(1U, 1U) : Session::PackSessionPair(4U, 4U);
        if (!Send(client, Session::SessionSetSizeHintsMethod) || !Repaint(client, s))
        {
            return 0xC0DE00A1U;
        }
    }
    Size route{0U};
    for (;;)
    {
        // A stalled second client really leaves its Endpoint unread. The
        // first client and root commands keep their independent credit path.
        if (g_independent)
        {
            Select(route);
            route ^= 1U;
        }
        if (g_independent && g_hung && g_active == 1U)
        {
            continue;
        }
        const auto received = client.Receive(g_receive, System::AddressOf(g_delivery), sizeof(g_delivery));
        if (received.Error == Abi::SystemCallError::WouldBlock)
        {
            if (!g_independent || route == 0U || g_hung)
            {
                static_cast<void>(client.Wait(g_wait, Session::ClientRetryUs));
            }
            continue;
        }
        if (received.Error != Abi::SystemCallError::None)
        {
            return 0xC0DE00A2U;
        }
        if (Session::ReadSessionFaceFrame(g_delivery, static_cast<Size>(received.Value1), g_command))
        {
            // Root command: suspend credits (hung client) or recover them.
            g_hung = g_command.Words[1U] == 90U;
            Words(g_command.Transaction);
            g_words[1U] = static_cast<UInt64>(Session::SessionAnswer::Done);
            const auto bytes = Session::WriteSessionFrame(g_frame, Session::SessionReplyMethod, g_command.Transaction,
                                                          g_words, nullptr, 0U);
            if (!Transmit(client, g_command.Attachments[0U], bytes))
            {
                return 0xC0DE00A9U;
            }
            static_cast<void>(client.CloseCapability(g_command.Attachments[0U]));
            // Native stress exercises the project's ordinary fault-channel
            // Termination after the root has received the command fence.
            if (g_command.Words[1U] == 92U)
            {
                __builtin_trap();
            }
            if (!g_hung && !g_independent && !Credit(client))
            {
                return 0xC0DE00A8U;
            }
            continue;
        }
        if (!Session::ReadSessionFrame(g_delivery, static_cast<Size>(received.Value1), g_message))
        {
            return 0xC0DE00A3U;
        }
        if (g_hung && !g_independent)
        {
            continue;
        }
        for (Size i{0U}; i < Session::SessionEventCount(g_message); ++i)
        {
            if (!Session::GetSessionEvent(g_message, i, g_event) || !LogEvent(client))
            {
                return 0xC0DE00A4U;
            }
            if (g_event.Kind == K::ImageReturned)
            {
                g_returned |= g_event.Words[0U];
            }
            if (g_event.Words[0U] < 1U || g_event.Words[0U] > 2U)
            {
                continue;
            }
            auto& w = g_windows[g_independent ? g_active : g_event.Words[0U] - 1U];
            if (g_event.Kind == K::Arrange)
            {
                w.Arrange = g_event.Words[1U];
                w.Width = static_cast<Foundation::UInt32>(g_event.Words[2U]);
                w.Height = static_cast<Foundation::UInt32>(g_event.Words[2U] >> 32U);
                w.Pending = true;
            }
            // Retained/coalesced state can deliver another close after DestroySurface. Drain it
            // without destroying the id again.
            if (g_event.Kind == K::CloseAsked && !w.Closed)
            {
                w.Closed = true;
                Words(g_event.Words[0U]);
                if (!Send(client, Session::SessionDestroySurfaceMethod))
                {
                    return 0xC0DE00A5U;
                }
            }
        }
        for (Size s{0U}; s < 2U; ++s)
        {
            if ((!g_independent || s == g_active) && g_windows[s].Pending && !g_windows[s].Closed &&
                !Repaint(client, s))
            {
                return 0xC0DE00A6U;
            }
        }
        if (!Credit(client))
        {
            return 0xC0DE00A7U;
        }
    }
}
[[nodiscard]] UInt64 Run(const System::SystemCallClient& client,
                         const UInt64 (&block)[Abi::ComponentBlockWords]) noexcept
{
    const auto initialized = Initialize(client, block);
    if (initialized != 0U)
    {
        return initialized;
    }
    if ((block[Abi::ComponentMountFlagsWord] >> 32U) >= 3U)
    {
        return RunInput(client);
    }
    const auto seed = block[Abi::ComponentMountFlagsWord] >> 32U;
    const auto initialHeight = seed == 2U ? PatternSecondaryHeight : Height;
    g_returned = 3U;
    g_windows[0U].Width = Width;
    g_windows[0U].Height = initialHeight;
    for (;;)
    {
        if (!g_commandPending)
        {
            const auto received = client.Receive(g_receive, System::AddressOf(g_delivery), sizeof(g_delivery));
            if (received.Error == Abi::SystemCallError::WouldBlock)
            {
                static_cast<void>(client.Wait(g_wait, Session::ClientRetryUs));
                continue;
            }
            if (received.Error != Abi::SystemCallError::None)
            {
                return 0xC0DE00E9U;
            }
            if (Session::ReadSessionFaceFrame(g_delivery, static_cast<Size>(received.Value1), g_command))
            {
                g_commandPending = true;
            }
            else
            {
                // Arrange/focus may finish a credit before the root's command.
                if (!Session::ReadSessionFrame(g_delivery, static_cast<Size>(received.Value1), g_message))
                {
                    return 0xC0DE00EAU;
                }
                for (Size i{0U}; i < Session::SessionEventCount(g_message); ++i)
                {
                    if (!Session::GetSessionEvent(g_message, i, g_event) || !LogEvent(client))
                    {
                        return 0xC0DE00EAU;
                    }
                    if (g_event.Kind == K::ImageReturned)
                    {
                        g_returned |= g_event.Words[0U];
                    }
                    if (g_event.Kind == K::Arrange)
                    {
                        g_arrange = g_event.Words[1U];
                        auto& w = g_windows[0U];
                        w.Arrange = g_arrange;
                        const auto width = static_cast<Foundation::UInt32>(g_event.Words[2U]);
                        const auto height = static_cast<Foundation::UInt32>(g_event.Words[2U] >> 32U);
                        w.Pending = w.Pending || w.Width != width || w.Height != height;
                        w.Width = width;
                        w.Height = height;
                    }
                    if (g_event.Kind == K::CloseAsked)
                    {
                        if (!g_windows[0U].Closed)
                        {
                            g_windows[0U].Closed = true;
                            Words(1U);
                            if (!Send(client, Session::SessionDestroySurfaceMethod))
                            {
                                return 0xC0DE00ECU;
                            }
                        }
                    }
                }
                if (g_windows[0U].Pending && !g_windows[0U].Closed && !Repaint(client, 0U))
                {
                    return 0xC0DE00EEU;
                }
                if (!Credit(client))
                {
                    return 0xC0DE00EBU;
                }
                continue;
            }
        }
        const auto serial = g_command.Words[1U];
        const auto transaction = g_command.Transaction;
        const auto root = g_command.Attachments[0U];
        g_commandPending = false;
        const Size back = serial == 2U ? 1U : 0U;
        if (serial == 4U)
        {
            Words(1U);
            if (!Send(client, Session::SessionDestroySurfaceMethod) || !Events(client, K::ImageReturned, 0U))
            {
                return 0xC0DE00ECU;
            }
        }
        else
        {
            if (serial > 1U && (g_returned & (1ULL << back)) == 0U && serial != 2U)
            {
                return 0xC0DE00EDU;
            }
            if (serial == 3U)
            {
                g_images[back].Width = 176U;
                g_images[back].Height = initialHeight - 10U;
                g_images[back].Stride = 176U;
                if (!Define(client, 1U, 1U, 176U, initialHeight - 10U))
                {
                    return 0xC0DE00EEU;
                }
            }
            if (serial == 2U)
            {
                static_cast<void>(
                    Graphics::CopyRect(g_images[back],
                                       Graphics::PixelSurface<const Foundation::UInt32>{
                                           g_images[0U].Pixels, Width, initialHeight, Width, g_images[0U].Format},
                                       {0, 0, Width, initialHeight}, {0, 0}));
            }
            const auto drawWidth = serial == 2U ? PatchWidth : g_images[back].Width;
            const auto drawHeight = serial == 2U ? PatchHeight : g_images[back].Height;
            for (Foundation::UInt32 y{0U}; y < drawHeight; ++y)
            {
                for (Foundation::UInt32 x{0U}; x < drawWidth; ++x)
                {
                    g_images[back].Pixels[static_cast<Size>(y) * g_images[back].Stride + x] = Graphics::PackColor(
                        g_images[back].Format,
                        {.Red = static_cast<Foundation::UInt8>(x + serial * 17U + seed * 31U),
                         .Green = static_cast<Foundation::UInt8>(y * 2U + serial * 11U + seed * 7U),
                         .Blue = static_cast<Foundation::UInt8>((x ^ y) + serial * 23U + seed * 13U)});
                }
            }
            System::OrderDmaStorage();
            g_returned &= ~(1ULL << back);
            Words(1U);
            g_words[1U] = back + 1U;
            g_words[2U] = serial;
            g_words[3U] = Session::SessionWantFrameShown;
            g_words[4U] = g_arrange;
            g_words[5U] = 1U;
            g_words[6U] = Session::PackSessionDamage(0U, 0U, drawWidth, drawHeight);
            if (!Send(client, Session::SessionPresentMethod) || !Events(client, K::FrameShown, 1U) || g_shown != serial)
            {
                return 0xC0DE00EFU;
            }
        }
        g_windows[0U].Serial = serial;
        g_windows[0U].Back = back ^ 1U;
        Words(transaction);
        g_words[1U] = static_cast<UInt64>(Session::SessionAnswer::Done);
        g_words[2U] = serial;
        const auto bytes =
            Session::WriteSessionFrame(g_frame, Session::SessionReplyMethod, transaction, g_words, nullptr, 0U);
        if (!Transmit(client, root, bytes))
        {
            return 0xC0DE00FBU;
        }
        static_cast<void>(client.CloseCapability(root));
    }
}
} // Namespace
} // Namespace Asteria::Test
#if defined(ASTERIA_TARGET_IA64)
extern "C" void AsteriaIa64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
#else
extern "C" void AsteriaAmd64ComponentImageMain(const Asteria::Foundation::UInt64 address) noexcept
#endif
{
    namespace Abi = Asteria::Abi;
    namespace Foundation = Asteria::Foundation;
    namespace Inventory = Asteria::Inventory;
    namespace Test = Asteria::Test;
    namespace User = Asteria::User;
#if defined(ASTERIA_TARGET_IA64)
    const User::System::SystemCallClient client{User::Platform::Ia64::System::GetIa64SystemCallTransport()};
#else
    const User::System::SystemCallClient client{User::Platform::Amd64::System::GetAmd64SystemCallTransport()};
#endif
    const auto& block = *reinterpret_cast<const Foundation::UInt64(*)[Abi::ComponentBlockWords]>(
        static_cast<Foundation::UIntPointer>(address));
    const auto code = Test::Run(client, block);
    for (;;)
    {
        static_cast<void>(client.ExitThread(code));
    }
}
