// SPDX-License-Identifier: Apache-2.0
#include "User/Component/DisplaySession.hpp"
#include "User/Graphics/Blit.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
#include <initializer_list>

namespace
{
namespace F = Asteria::Foundation;
namespace A = Asteria::Abi;
namespace S = Asteria::User::System;
namespace C = Asteria::User::Component;
namespace P = Asteria::User::Session;
namespace G = Asteria::User::Graphics;
using Asteria::TestSupport::TestContext;
struct Kernel
{
    F::Size Creates{};
    F::Size Maps{};
    F::Size Unmaps{};
    F::Size Closes{};
    F::Size Sends{};
    F::Size Destroys{};
    F::UInt8 Permissions{};
    F::UInt64 MapPermissions{};
    bool Block{};
    bool RefuseMap{};
    alignas(8) F::UInt8 Sent[C::EchoDeliveryHeaderBytes + C::EchoMaximumFrameBytes]{};
    F::Size Bytes{};
    static void Transport(void* context, const A::SystemCallRequest* request, A::SystemCallResponse* response) noexcept
    {
        auto& k = *static_cast<Kernel*>(context);
        *response = {};
        const auto* bytes = reinterpret_cast<const F::UInt8*>(request->Arguments[3U]);
        switch (static_cast<A::SystemCallNumber>(request->Number))
        {
        case A::SystemCallNumber::ObjectCreate:
            ++k.Creates;
            if (request->Arguments[2U] == A::ObjectTypeMemoryObject)
            {
                k.Permissions = bytes[A::ObjectCreatePermissionsOffset];
            }
            response->Value0 = 100U + k.Creates;
            response->Value1 = 200U + k.Creates;
            break;
        case A::SystemCallNumber::MemoryMap:
            ++k.Maps;
            k.MapPermissions = bytes[A::FaultResolvePermissionsOffset];
            if (k.RefuseMap)
            {
                response->Error = A::SystemCallError::RightsDenied;
            }
            break;
        case A::SystemCallNumber::MemoryUnmap:
            ++k.Unmaps;
            break;
        case A::SystemCallNumber::ObjectDestroy:
            ++k.Destroys;
            break;
        case A::SystemCallNumber::CapabilityClose:
            ++k.Closes;
            break;
        case A::SystemCallNumber::IpcSend:
            ++k.Sends;
            k.Bytes = static_cast<F::Size>(request->Arguments[3U]);
            std::memcpy(k.Sent + C::EchoDeliveryHeaderBytes, reinterpret_cast<const void*>(request->Arguments[2U]),
                        k.Bytes);
            if (k.Block)
            {
                response->Error = A::SystemCallError::WouldBlock;
            }
            break;
        default:
            break;
        }
    }
};
Kernel g_kernel{};
C::DisplaySession g_session{};
C::DisplayInfo g_info{};
G::Composition::PlaneStorage g_planes{};
G::Composition::DamageList g_damage{};
Asteria::User::Console::TextConsole g_console{};
P::SessionFaceMessage g_message{};
P::SessionFaceMessage g_reply{};
void Configure()
{
    g_kernel = {};
    g_info = {.Version = C::DisplayProtocolVersion,
              .Features = C::DisplayFeatureSession,
              .Width = 160U,
              .Height = 100U,
              .Columns = 20U,
              .Rows = 6U,
              .Epoch = 256U};
    g_console.Configure(160U, 100U, G::Bold8x16);
    g_planes.Initialize(g_console);
    g_damage.Configure({.X = 0, .Y = 0, .Width = 160U, .Height = 100U});
    g_session.Configure({1U, 2U}, {3U, 4U}, {5U, 6U}, 0x82000000U, G::PixelFormat::Bgrx, g_info, g_planes, g_damage);
}
void Message(F::UInt32 method)
{
    g_message = {};
    g_message.Method = method;
    g_message.Transaction = 20U;
    g_message.Words[0U] = 256U;
    g_message.AttachmentCount = 1U;
    g_message.Attachments[0U] = {30U, 31U};
}
bool ReadReply()
{
    return P::ReadSessionFaceFrame(g_kernel.Sent, g_kernel.Bytes + C::EchoDeliveryHeaderBytes, g_reply);
}
void Restart(TestContext& test)
{
    Configure();
    const S::SystemCallClient client{{Kernel::Transport, &g_kernel}};
    Message(P::FaceOpen);
    static_cast<void>(g_session.Dispatch(client, g_message, C::EchoManageRight, 0U));
    Message(P::FaceCredit);
    g_message.Words[1U] = 7U;
    ASTERIA_EXPECT(test, "dead compositor holds a pointer credit",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    F::UInt64 pointer[]{256U, 1U, C::DisplayPointerSnapshot, 0U, 1U, 1U, 0U, 9U};
    g_session.Pointer(pointer, {.X = 60, .Y = 80});
    g_kernel.Block = true;
    g_session.SendCredit(client);
    // FIFO FaceFence is sent only after the old thread stopped. A delayed
    // old Credit is processed before that fence, never after Detach.
    g_kernel.Block = false;
    Message(17U);
    ASTERIA_EXPECT(test, "same-route fence acknowledges the stopped producer",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    Message(P::FaceDetach);
    g_message.AttachmentCount = 0U;
    ASTERIA_EXPECT(test, "detach retires the old credit and queued snapshots",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 2U), !g_session.Retrying(),
                   g_session.DroppedSnapshots() == 1U);
    Message(P::FaceCredit);
    g_message.Words[1U] = 7U;
    ASTERIA_EXPECT(test, "replacement's first credit is accepted",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    pointer[1U] = 2U;
    pointer[4U] = 0U;
    g_session.Pointer(pointer, {.X = 61, .Y = 81});
    g_session.SendCredit(client);
    ASTERIA_EXPECT(test, "replacement receives only post-detach input", ReadReply(), g_reply.Method == P::FaceSnapshots,
                   g_reply.Words[4U] == 2U, g_reply.Words[6U] == 0U);
}
void Run(TestContext& test)
{
    Configure();
    const S::SystemCallClient client{{.Function = Kernel::Transport, .Context = &g_kernel}};
    Message(P::FaceOpen);
    ASTERIA_EXPECT(test, "only root Manage on console opens the session",
                   !g_session.Dispatch(client, g_message, C::EchoInvokeRight, 0U), g_kernel.Creates == 0U,
                   g_session.Dispatch(client, g_message, C::EchoManageRight, 0U), ReadReply(),
                   g_reply.AttachmentCount == 2U, g_kernel.Creates == 2U);
    Message(P::FaceShow);
    g_message.Words[0U] = 1U;
    g_message.AttachmentCount = 0U;
    ASTERIA_EXPECT(test, "Invoke never switches trusted visibility",
                   !g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U), g_planes.Console().Visible(),
                   g_session.Dispatch(client, g_message, C::EchoManageRight, 1U), !g_planes.Console().Visible(),
                   g_planes.Session().Visible());
    Message(P::FaceCreateSurface);
    g_message.Words[1U] = 1U;
    g_message.Words[2U] = 1U;
    g_message.Words[4U] = 16U | (16ULL << 32U);
    g_message.Words[5U] = 16U;
    g_message.Words[6U] = static_cast<F::UInt64>(G::PixelFormat::Bgrx);
    g_message.Words[8U] = 1U;
    ASTERIA_EXPECT(test, "output maps only Read/User in a Read/User MemoryObject",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U), g_kernel.Permissions == 0x09U,
                   g_kernel.MapPermissions == 0x09U, g_kernel.Maps == 1U);
    Message(P::FacePresent);
    g_message.Words[1U] = 1U;
    g_message.Words[2U] = 1U;
    g_message.Words[4U] = P::PackSessionDamage(0U, 0U, 16U, 16U);
    ASTERIA_EXPECT(test, "one pending Present and an explicit Busy reply",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U), g_session.Pending(),
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U), ReadReply(),
                   g_reply.Words[1U] == P::SessionBusy);
    g_kernel.Block = true;
    g_session.Presented(client);
    ASTERIA_EXPECT(test, "blocked Presented retains the buffer handshake", g_session.Pending());
    g_kernel.Block = false;
    g_session.Presented(client);
    ASTERIA_EXPECT(test, "Presented settles after the pass", !g_session.Pending(), ReadReply(),
                   g_reply.Words[1U] == P::SessionPresented);
    Message(P::FaceCredit);
    g_message.Words[1U] = 7U;
    ASTERIA_EXPECT(test, "credit retained", g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    F::UInt64 pointer[]{256U, 1U, C::DisplayRelativeMotion, 0U, 0U, 1U, 0U, 42U};
    g_session.Pointer(pointer, {.X = 96, .Y = 72});
    g_kernel.Block = true;
    g_session.SendCredit(client);
    ASTERIA_EXPECT(test, "key stamp and position survive the route", ReadReply(), g_reply.Method == P::FaceSnapshots,
                   g_reply.Words[4U] == 1U, g_reply.Words[5U] == (96U | (72ULL << 32U)), g_reply.Words[7U] == 42U);
    pointer[1U] = 2U;
    g_session.Pointer(pointer, {.X = 97, .Y = 73});
    g_kernel.Block = false;
    g_session.SendCredit(client);
    ASTERIA_EXPECT(test, "WouldBlock retry is a frozen batch", ReadReply(), g_reply.Words[4U] == 1U,
                   g_reply.Words[5U] == (96U | (72ULL << 32U)));
    Message(P::FaceCredit);
    g_message.Words[1U] = 7U;
    ASTERIA_EXPECT(test, "next credit accepted", g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    g_session.SendCredit(client);
    ASTERIA_EXPECT(test, "later snapshot remains queued", ReadReply(), g_reply.Words[4U] == 2U);
    // A stopped compositor saturates distinct button transitions. Cursor
    // drawing must not wait for a credit; exhaustion invalidates only the
    // ordered route, and an already frozen prefix must stay byte-identical.
    Message(P::FaceCredit);
    g_message.Words[1U] = 1U;
    ASTERIA_EXPECT(test, "hold a fresh snapshot credit", g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    pointer[1U] = 3U;
    pointer[2U] = C::DisplayPointerSnapshot;
    pointer[4U] = 1U;
    g_session.Pointer(pointer, {.X = 98, .Y = 74});
    g_kernel.Block = true;
    g_session.SendCredit(client);
    static F::UInt8 frozen[sizeof(g_kernel.Sent)]{};
    const auto frozenBytes = g_kernel.Bytes;
    std::memcpy(frozen, g_kernel.Sent, sizeof(frozen));
    ASTERIA_EXPECT(test, "duplicate credit cannot cancel its frozen predecessor",
                   !g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    for (F::UInt64 i{4U}; i < 100U; ++i)
    {
        pointer[1U] = i;
        pointer[4U] = i & 1U;
        g_session.Pointer(pointer, {.X = 99, .Y = 75});
    }
    g_kernel.Block = false;
    g_session.SendCredit(client);
    ASTERIA_EXPECT(test, "exhaustion preserves the blocked prefix and counts loss", g_session.DroppedSnapshots() > 0U,
                   g_kernel.Bytes == frozenBytes, std::memcmp(frozen, g_kernel.Sent, sizeof(frozen)) == 0);
    ASTERIA_EXPECT(test, "stale pointer route accepts the next credit",
                   g_session.Dispatch(client, g_message, C::EchoInvokeRight, 1U));
    g_kernel.Block = true;
    g_session.SendCredit(client);
    ASTERIA_EXPECT(test, "ordered route reports Stale without blocking broker work", ReadReply(),
                   g_reply.Words[1U] == P::SessionStale, g_session.Retrying());
    g_kernel.Block = false;
    g_session.SendCredit(client);
    Message(P::FaceDrop);
    g_message.Words[0U] = 0U;
    g_message.AttachmentCount = 0U;
    ASTERIA_EXPECT(test, "root Drop restores console and releases borrowed mappings",
                   g_session.Dispatch(client, g_message, C::EchoManageRight, 1U), g_planes.Console().Visible(),
                   !g_planes.Session().Visible(), g_kernel.Unmaps == 1U, g_kernel.Destroys == 1U);
}
} // namespace
int main()
{
    TestContext test{"DisplaySessionTests"};
    Run(test);
    Restart(test);
    return test.Finish();
}
