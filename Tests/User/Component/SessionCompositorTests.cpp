// SPDX-License-Identifier: Apache-2.0
#include "User/Component/SessionCompositor.hpp"
#include "User/System/DeviceAccess.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
#include <sys/mman.h>
namespace
{
namespace F = Asteria::Foundation;
namespace A = Asteria::Abi;
namespace S = Asteria::User::System;
namespace P = Asteria::User::Session;
namespace C = Asteria::User::Component;
using Asteria::TestSupport::TestContext;
struct Kernel
{
    F::UInt64 Next{100U};
    F::UInt64 Endpoint{};
    F::UInt64 RejectAt{};
    F::UInt64 Now{100000U};
    F::UInt64 Base{};
    F::Size Maps{};
    F::Size Unmaps{};
    F::Size Closes{};
    F::Size Destroys{};
    F::Size Bytes{};
    F::Size EventBytes{};
    F::Size ReplyBytes{};
    F::Size PresentBytes{};
    F::UInt64 Permissions{};
    F::UInt64 FrameOffset{};
    F::UInt64 ObjectOffset{};
    F::UInt64 FramePages{};
    F::UInt64 LargestFrame{};
    F::UInt64 SurfacePages{};
    F::UInt64 SurfaceFrames{};
    F::Size FrameCreates{};
    F::Size SurfaceAttachments{};
    bool NativeOutput{};
    bool FaceValid{true};
    bool OffsetsValid{true};
    bool FrameFormsValid{true};
    bool BlockEvents{};
    bool BlockPresent{};
    bool BlockReplies{};
    bool BlockReports{};
    F::Size Presents{};
    F::Size ReportAttempts{};
    alignas(8) F::UInt8 Incoming[P::SessionDeliveryCapacity]{}, Events[P::SessionDeliveryCapacity]{},
        Reply[P::SessionDeliveryCapacity]{}, Present[P::SessionKernelFrameLimit]{};
    static void Transport(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        auto& k = *static_cast<Kernel*>(context);
        *response = {};
        const auto* bytes = reinterpret_cast<const F::UInt8*>(request->Arguments[3U]);
        switch (static_cast<A::SystemCallNumber>(request->Number))
        {
        case A::SystemCallNumber::ObjectCreate:
            response->Value0 = k.Next++;
            response->Value1 = 1U;
            if (request->Arguments[2U] == A::ObjectTypeFrame)
            {
                const auto pages = S::LoadWord(bytes + A::ObjectCreatePagesOffset);
                const bool large = pages > A::ObjectCreateFrameLegacyMaximumPages;
                k.FrameFormsValid =
                    k.FrameFormsValid && request->Arguments[4U] == (large ? A::ObjectCreateRamFrameRequestSizeBytes
                                                                          : A::ObjectCreateFrameRequestSizeBytes);
                if (large)
                {
                    k.FrameFormsValid = k.FrameFormsValid && bytes[A::ObjectCreateRamFrameLimitOffset] == 32U;
                }
                if (!k.FrameFormsValid)
                {
                    response->Error = A::SystemCallError::InvalidArguments;
                }
                ++k.FrameCreates;
                k.FramePages += pages;
                if (pages > k.LargestFrame)
                {
                    k.LargestFrame = pages;
                }
            }
            break;
        case A::SystemCallNumber::MemoryMap:
        {
            ++k.Maps;
            k.Permissions = bytes[A::FaultResolvePermissionsOffset];
            k.FrameOffset = S::LoadWord(bytes + A::FaultResolveFramePageOffset);
            k.ObjectOffset = S::LoadWord(bytes + A::FaultResolveMemoryObjectPageOffset);
            if (k.NativeOutput)
            {
                k.OffsetsValid = k.OffsetsValid && k.FrameOffset == 0U &&
                                 k.ObjectOffset == (k.Maps - 1U) * P::SessionMaximumFramePages;
            }
            if (k.Maps == k.RejectAt)
            {
                response->Error = A::SystemCallError::InvalidArguments;
                break;
            }
            const auto address = S::LoadWord(bytes + A::FaultResolveVirtualStartOffset);
            const auto pages = S::LoadWord(bytes + A::FaultResolvePageCountOffset);
            if (address < k.Base || address + pages * P::SessionPageBytes > k.Base + P::AddressRegionBytes ||
                ::mmap(reinterpret_cast<void*>(address), pages * P::SessionPageBytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
            {
                response->Error = A::SystemCallError::InvalidArguments;
            }
            break;
        }
        case A::SystemCallNumber::MemoryUnmap:
            ++k.Unmaps;
            if (::mmap(reinterpret_cast<void*>(request->Arguments[2U]), request->Arguments[3U] * P::SessionPageBytes,
                       PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
            {
                response->Error = A::SystemCallError::InvalidArguments;
            }
            break;
        case A::SystemCallNumber::ObjectDestroy:
            ++k.Destroys;
            break;
        case A::SystemCallNumber::CapabilityClose:
            ++k.Closes;
            break;
        case A::SystemCallNumber::IpcReceive:
            if (request->Arguments[0U] != k.Endpoint || k.Bytes == 0U)
            {
                response->Error = A::SystemCallError::WouldBlock;
                break;
            }
            std::memcpy(reinterpret_cast<void*>(request->Arguments[2U]), k.Incoming, k.Bytes);
            response->Value1 = k.Bytes;
            k.Bytes = 0U;
            break;
        case A::SystemCallNumber::IpcSend:
        {
            const auto* frame = reinterpret_cast<const F::UInt8*>(request->Arguments[2U]);
            const auto count = static_cast<F::Size>(request->Arguments[3U]);
            const auto method = S::LoadWord(frame + 32U) & 0xFFFF'FFFFU;
            const auto interface = S::LoadWord(frame + 16U);
            if (k.NativeOutput && interface == P::SessionFaceCodec::Low &&
                (method == P::FaceCreateSurface || method == P::FaceAddSurfaceFrames))
            {
                P::SessionFaceMessage surface{};
                std::memcpy(k.Incoming + C::EchoDeliveryHeaderBytes, frame, count);
                k.FaceValid =
                    k.FaceValid && P::ReadSessionFaceFrame(k.Incoming, count + C::EchoDeliveryHeaderBytes, surface);
                k.SurfaceAttachments += surface.AttachmentCount;
                if (method == P::FaceCreateSurface)
                {
                    k.SurfacePages = surface.Words[1U];
                    k.SurfaceFrames = surface.Words[2U];
                }
            }
            if (interface == P::SessionInterfaceLow && method == P::SessionReplyMethod)
            {
                k.ReplyBytes = count + C::EchoDeliveryHeaderBytes;
                std::memcpy(k.Reply + C::EchoDeliveryHeaderBytes, frame, count);
                if (k.BlockReplies)
                {
                    response->Error = A::SystemCallError::WouldBlock;
                }
            }
            if (interface == P::SessionInterfaceLow && method == P::SessionEventsMethod)
            {
                k.EventBytes = count + C::EchoDeliveryHeaderBytes;
                std::memcpy(k.Events + C::EchoDeliveryHeaderBytes, frame, count);
                if (k.BlockEvents)
                {
                    response->Error = A::SystemCallError::WouldBlock;
                }
            }
            if (request->Arguments[0U] == 8U && method == P::FacePresent)
            {
                ++k.Presents;
                k.PresentBytes = count;
                std::memcpy(k.Present, frame, count);
                if (k.BlockPresent)
                {
                    response->Error = A::SystemCallError::WouldBlock;
                }
            }
            if (request->Arguments[0U] == 9U)
            {
                ++k.ReportAttempts;
                if (k.BlockReports)
                {
                    response->Error = A::SystemCallError::WouldBlock;
                }
            }
            break;
        }
        default:
            break;
        }
    }
};
constexpr F::UInt64 HostAlignment{1ULL << 30U};
constexpr F::UInt64 HostReservation{2U * HostAlignment};
Kernel g_kernel{};
C::SessionCompositor g_owner{};
alignas(8) F::UInt64 g_words[32U]{}, g_block[A::ComponentBlockWords]{};
P::SessionMessage g_reply{};
P::SessionMessage g_events{};
P::SessionEvent g_event{};
F::UInt64 Clock() noexcept
{
    return g_kernel.Now;
}
void Words(F::UInt64 first = 0U)
{
    for (auto& word : g_words)
    {
        word = 0U;
    }
    g_words[0U] = first;
}
bool Queue(const F::UInt64 endpoint, const F::UInt32 method, const S::CapabilityHandle* handles = nullptr,
           const F::Size count = 0U, const bool manage = false)
{
    g_kernel.Endpoint = endpoint;
    std::memset(g_kernel.Incoming, 0, sizeof(g_kernel.Incoming));
    g_kernel.Incoming[14U] = manage ? 0U : 2U;
    g_kernel.Incoming[15U] = manage ? 8U : 0U;
    const auto bytes =
        P::WriteSessionFrame(g_kernel.Incoming + C::EchoDeliveryHeaderBytes, method, 50U, g_words, handles, count);
    g_kernel.Bytes = bytes + C::EchoDeliveryHeaderBytes;
    return bytes != 0U;
}
bool Has(const P::SessionEventKind kind, const F::UInt64 value)
{
    if (!P::ReadSessionFrame(g_kernel.Events, g_kernel.EventBytes, g_events))
    {
        return false;
    }
    for (F::Size i{0U}; i < P::SessionEventCount(g_events); ++i)
    {
        if (P::GetSessionEvent(g_events, i, g_event) && g_event.Kind == kind && g_event.Words[0U] == value)
        {
            return true;
        }
    }
    return false;
}
void Run(TestContext& test)
{
    // Reserve our own aligned region in the sanitizer's application range;
    // MAP_FIXED below replaces only pages of this reservation.
    void* const reserved = ::mmap(nullptr, HostReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASTERIA_EXPECT(test, "host reserves its own fake address space", reserved != MAP_FAILED);
    if (reserved == MAP_FAILED)
    {
        return;
    }
    g_kernel.Base = (reinterpret_cast<F::UIntPointer>(reserved) + HostAlignment - 1U) & ~(HostAlignment - 1U);
    const S::SystemCallClient client{{Kernel::Transport, &g_kernel}};
    g_block[A::ComponentAccountWord] = 2U;
    g_block[A::ComponentAccountSpaceWord] = 3U;
    g_block[A::ComponentWaitSetWord] = 4U;
    g_block[A::ComponentReceiveWord] = 1U;
    g_block[A::ComponentReplyWord] = 1U;
    g_block[A::ComponentInputServiceWord] = 8U;
    g_block[A::ComponentInputEpochWord] = 1U;
    g_block[A::ComponentMountFlagsWord] = 2U;
    g_block[A::ComponentClockTicksPerMicrosecondWord] = 1U;
    g_block[A::ComponentSessionReportWord] = 9U;
    g_block[A::ComponentSessionOutputModeWord] =
        P::PackSessionPair(A::ComponentSessionTestWidth, A::ComponentSessionTestHeight);
    ASTERIA_EXPECT(test, "one output and an outstanding broker fence",
                   g_owner.Start(client, g_block, Clock, g_kernel.Base), g_owner.Step(client),
                   g_kernel.PresentBytes != 0U);
    const S::CapabilityHandle reply{40U, 1U};
    Words();
    g_words[1U] = 1U;
    g_words[2U] = 4U;
    g_words[4U] = 0x74736574U;
    ASTERIA_EXPECT(test, "Invoke cannot register a trusted name", Queue(1U, P::SessionRegisterClientMethod, &reply, 1U),
                   g_owner.Step(client), g_kernel.ReplyBytes == 0U);
    ASTERIA_EXPECT(test, "manager registration mints the bounded client",
                   Queue(1U, P::SessionRegisterClientMethod, &reply, 1U, true), g_owner.Step(client),
                   P::ReadSessionFrame(g_kernel.Reply, g_kernel.ReplyBytes, g_reply), g_reply.Words[2U] == 257U,
                   g_reply.AttachmentCount == 1U);
    const auto endpoint = g_reply.Attachments[0U].Selector;
    S::CapabilityHandle frames[2U * P::SessionMaximumPoolFrames]{};
    for (F::Size i{0U}; i < 2U * P::SessionMaximumPoolFrames; ++i)
    {
        frames[i] = {700U + i, 1U};
    }
    Words(1U);
    g_words[1U] = 4U;
    g_words[2U] = 4U;
    g_words[4U] = 0x0001'0001'0001'0001ULL;
    g_kernel.RejectAt = g_kernel.Maps + 3U;
    const auto beforeUnmap = g_kernel.Unmaps;
    const auto beforeClose = g_kernel.Closes;
    ASTERIA_EXPECT(test, "a lying Frame rolls back its consumed prefix and closes the remainder",
                   Queue(endpoint, P::SessionCreatePoolMethod, frames, 4U), g_owner.Step(client),
                   g_kernel.Unmaps == beforeUnmap + 2U, g_kernel.Closes == beforeClose + 5U, g_kernel.Permissions == 9U,
                   g_kernel.FrameOffset == 0U, g_kernel.ObjectOffset == 2U);
    g_kernel.RejectAt = 0U;
    Words(1U);
    g_words[1U] = 7U;
    ASTERIA_EXPECT(test, "mapping failure is a retained state outcome",
                   Queue(endpoint, P::SessionCreditMethod, &reply, 1U), g_owner.Step(client),
                   Has(P::SessionEventKind::PoolRefused, 1U));
    Words(1U);
    g_words[1U] = 1U;
    g_words[2U] = 1U;
    g_words[4U] = 1U;
    ASTERIA_EXPECT(test, "failed pool slot can be reused", Queue(endpoint, P::SessionCreatePoolMethod, frames, 1U),
                   g_owner.Step(client));
    Words(1U);
    g_words[1U] = 8U;
    g_words[2U] = 4096U;
    g_words[3U] = P::PackSessionPair(1U, 1U);
    g_words[4U] = 1U;
    g_words[5U] = 2U;
    ASTERIA_EXPECT(test, "image end is checked against actual pool size", Queue(endpoint, P::SessionDefineImageMethod),
                   g_owner.Step(client));
    Words(1U);
    g_words[1U] = 7U;
    g_kernel.BlockEvents = true;
    ASTERIA_EXPECT(test, "bad range produces an ImageRefused state",
                   Queue(endpoint, P::SessionCreditMethod, &reply, 1U), g_owner.Step(client),
                   Has(P::SessionEventKind::ImageRefused, 8U));
    const auto frozenBytes = g_kernel.EventBytes;
    static F::UInt8 frozen[P::SessionDeliveryCapacity]{};
    std::memcpy(frozen, g_kernel.Events, frozenBytes);
    g_kernel.BlockEvents = false;
    g_kernel.Now += P::ClientRetryUs;
    ASTERIA_EXPECT(test, "WouldBlock retries the exact frozen answer", g_owner.Step(client),
                   g_kernel.EventBytes == frozenBytes, std::memcmp(frozen, g_kernel.Events, frozenBytes) == 0);
    // Remove the small pool, then supply all 32 records allowed per client.
    Words(1U);
    ASTERIA_EXPECT(test, "pool unmap releases the reusable slot", Queue(endpoint, P::SessionDestroyPoolMethod),
                   g_owner.Step(client));
    for (F::UInt64 pool{1U}; pool <= 2U; ++pool)
    {
        Words(pool);
        g_words[1U] = 16U;
        g_words[2U] = 16U;
        for (F::Size i{4U}; i < 8U; ++i)
        {
            g_words[i] = 0x0001'0001'0001'0001ULL;
        }
        ASTERIA_EXPECT(
            test, "maximum pool first message maps four Frames",
            Queue(endpoint, P::SessionCreatePoolMethod, frames + (pool - 1U) * P::SessionMaximumPoolFrames, 4U),
            g_owner.Step(client));
        for (F::UInt64 first{4U}; first < 16U; first += 4U)
        {
            Words(pool);
            g_words[1U] = first;
            ASTERIA_EXPECT(test, "maximum pool continuation",
                           Queue(endpoint, P::SessionAddPoolFramesMethod,
                                 frames + (pool - 1U) * P::SessionMaximumPoolFrames + first, 4U),
                           g_owner.Step(client));
        }
    }
    const auto unmaps = g_kernel.Unmaps;
    Words(257U);
    ASTERIA_EXPECT(test, "DropClient unmaps all 32 records before acknowledging",
                   Queue(1U, P::SessionDropClientMethod, &reply, 1U, true), g_owner.Step(client),
                   g_kernel.Unmaps == unmaps + 32U, P::ReadSessionFrame(g_kernel.Reply, g_kernel.ReplyBytes, g_reply),
                   g_reply.Words[2U] == 0U);
    Words(257U);
    const auto destroys = g_kernel.Destroys;
    ASTERIA_EXPECT(test, "stale client generation changes nothing",
                   Queue(1U, P::SessionDropClientMethod, &reply, 1U, true), g_owner.Step(client),
                   g_kernel.Destroys == destroys);
    F::UInt64 activeEndpoint{0U};
    for (F::Size slot{0U}; slot < P::SessionMaximumClients; ++slot)
    {
        Words();
        g_words[1U] = 1U;
        g_words[2U] = 4U;
        g_words[4U] = 0x74736574U;
        ASTERIA_EXPECT(test, "fill each bounded manager slot",
                       Queue(1U, P::SessionRegisterClientMethod, &reply, 1U, true), g_owner.Step(client),
                       P::ReadSessionFrame(g_kernel.Reply, g_kernel.ReplyBytes, g_reply),
                       g_reply.Words[1U] == static_cast<F::UInt64>(P::SessionAnswer::Done));
        if (slot == 0U)
        {
            activeEndpoint = g_reply.Attachments[0U].Selector;
        }
    }
    g_kernel.BlockReplies = true;
    ASTERIA_EXPECT(
        test, "full table refuses without minting", Queue(1U, P::SessionRegisterClientMethod, &reply, 1U, true),
        g_owner.Step(client), P::ReadSessionFrame(g_kernel.Reply, g_kernel.ReplyBytes, g_reply),
        g_reply.Words[1U] == static_cast<F::UInt64>(P::SessionAnswer::Exhausted), g_reply.AttachmentCount == 0U);
    const auto fullBytes = g_kernel.ReplyBytes;
    std::memcpy(frozen, g_kernel.Reply, fullBytes);
    g_kernel.Now += P::ClientRetryUs;
    ASTERIA_EXPECT(test, "full-table WouldBlock retains the exact reply", g_owner.Step(client),
                   g_kernel.ReplyBytes == fullBytes, std::memcmp(frozen, g_kernel.Reply, fullBytes) == 0);
    // A full manager reply queue shares this receive Endpoint with Presented.
    // The broker fence must still be consumed while the refusal stays blocked.
    Words(2U);
    g_words[1U] = P::SessionPresented;
    g_words[2U] = C::DisplayProtocolVersion;
    g_words[4U] = P::PackSessionPair(1024U, 768U);
    g_words[5U] = P::PackSessionPair(128U, 48U);
    g_words[6U] = 1U;
    g_kernel.Endpoint = 1U;
    std::memset(g_kernel.Incoming, 0, sizeof(g_kernel.Incoming));
    g_kernel.Bytes =
        C::EchoDeliveryHeaderBytes + P::WriteSessionFaceFrame(g_kernel.Incoming + C::EchoDeliveryHeaderBytes,
                                                              C::DisplayReplyMethod, 2U, g_words, nullptr, 0U);
    ASTERIA_EXPECT(test, "blocked registration refusal cannot stall Presented", g_owner.Step(client),
                   g_kernel.Bytes == 0U);
    const auto fullCloses = g_kernel.Closes;
    g_kernel.BlockReplies = false;
    ASTERIA_EXPECT(test, "full-table reply closes its retained credit on success", g_owner.Step(client),
                   g_kernel.Closes == fullCloses + 1U);
    // A diagnostic consumer that never reads must not pace FrameShown or
    // the next composition. The active transcript remains immutable while
    // complete later fences coalesce independently.
    g_kernel.BlockReports = true;
    Words(1U);
    g_words[1U] = 1U;
    g_words[2U] = 1U;
    g_words[4U] = 1U;
    ASTERIA_EXPECT(test, "diagnostic-stall pool", Queue(activeEndpoint, P::SessionCreatePoolMethod, frames, 1U),
                   g_owner.Step(client));
    Words(1U);
    g_words[1U] = 1U;
    g_words[3U] = P::PackSessionPair(1U, 1U);
    g_words[4U] = 1U;
    g_words[5U] = 2U;
    ASTERIA_EXPECT(test, "diagnostic-stall image", Queue(activeEndpoint, P::SessionDefineImageMethod),
                   g_owner.Step(client));
    Words(1U);
    g_words[1U] = P::SessionToplevelRole;
    ASTERIA_EXPECT(test, "diagnostic-stall surface", Queue(activeEndpoint, P::SessionCreateSurfaceMethod),
                   g_owner.Step(client));
    Words(1U);
    g_words[1U] = 1U;
    g_words[2U] = 1U;
    g_words[3U] = P::SessionWantFrameShown;
    g_words[5U] = 1U;
    g_words[6U] = P::PackSessionDamage(0U, 0U, 1U, 1U);
    g_kernel.Now += C::SessionComposeIntervalUs;
    const auto presents = g_kernel.Presents;
    ASTERIA_EXPECT(test, "blocked report permits a new output Present", Queue(activeEndpoint, P::SessionPresentMethod),
                   g_owner.Step(client), g_kernel.Presents == presents + 1U);
    Words(3U);
    g_words[1U] = P::SessionPresented;
    g_words[2U] = C::DisplayProtocolVersion;
    g_words[4U] = P::PackSessionPair(1024U, 768U);
    g_words[5U] = P::PackSessionPair(128U, 48U);
    g_words[6U] = 1U;
    g_kernel.Endpoint = 1U;
    std::memset(g_kernel.Incoming, 0, sizeof(g_kernel.Incoming));
    g_kernel.Bytes =
        C::EchoDeliveryHeaderBytes + P::WriteSessionFaceFrame(g_kernel.Incoming + C::EchoDeliveryHeaderBytes,
                                                              C::DisplayReplyMethod, 3U, g_words, nullptr, 0U);
    ASTERIA_EXPECT(test, "blocked report permits the broker fence", g_owner.Step(client), g_kernel.Bytes == 0U);
    Words(1U);
    g_words[1U] = P::SessionMaximumEvents;
    ASTERIA_EXPECT(test, "FrameShown does not wait for diagnostics",
                   Queue(activeEndpoint, P::SessionCreditMethod, &reply, 1U), g_owner.Step(client),
                   Has(P::SessionEventKind::FrameShown, 1U));
    g_kernel.BlockReports = false;
    ASTERIA_EXPECT(test, "diagnostics can resume after independent fences", g_owner.Step(client));
    static_cast<void>(::munmap(reserved, HostReservation));
}
void RunNativeOutput(TestContext& test)
{
    static Kernel kernel;
    static C::SessionCompositor owner;
    static F::UInt64 block[A::ComponentBlockWords]{};
    void* const reserved = ::mmap(nullptr, HostReservation, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASTERIA_EXPECT(test, "host reserves the native output's fixed region", reserved != MAP_FAILED);
    if (reserved == MAP_FAILED)
    {
        return;
    }
    kernel.Base = (reinterpret_cast<F::UIntPointer>(reserved) + HostAlignment - 1U) & ~(HostAlignment - 1U);
    kernel.NativeOutput = true;
    for (F::Size i{0U}; i < A::ComponentBlockWords; ++i)
    {
        block[i] = g_block[i];
    }
    const S::SystemCallClient client{{Kernel::Transport, &kernel}};
    block[A::ComponentSessionOutputModeWord] = 0U;
    const bool empty = !owner.Start(client, block, Clock, kernel.Base);
    block[A::ComponentSessionOutputModeWord] = P::PackSessionPair(4096U, 4096U);
    const bool excessive = !owner.Start(client, block, Clock, kernel.Base);
    ASTERIA_EXPECT(test, "missing and oversized modes refuse before allocating an output", empty, excessive,
                   kernel.Next == 100U);
    block[A::ComponentSessionOutputModeWord] = P::PackSessionPair(2560U, 1440U);
    ASTERIA_EXPECT(test, "native output maps disjoint owned chunks and transfers a valid complete broker surface",
                   owner.Start(client, block, Clock, kernel.Base), kernel.Maps == 8U, kernel.FrameCreates == 8U,
                   kernel.FramePages == 3600U, kernel.LargestFrame <= P::SessionMaximumFramePages,
                   kernel.FrameFormsValid, kernel.OffsetsValid, kernel.FrameOffset == 0U, kernel.ObjectOffset == 3584U,
                   kernel.Permissions == S::ReadWriteUser, kernel.FaceValid, kernel.SurfaceAttachments == 8U,
                   kernel.SurfacePages == 3600U, kernel.SurfaceFrames == 8U);
    static_cast<void>(::munmap(reserved, HostReservation));
}
} // Namespace
int main()
{
    TestContext test{"SessionCompositorTests"};
    Run(test);
    RunNativeOutput(test);
    return test.Finish();
}
