// SPDX-License-Identifier: Apache-2.0
#include "User/Component/DisplaySession.hpp"
#include "User/Identity/LoginRenderer.hpp"
#include "User/Graphics/Blit.hpp"
#include "User/System/DeviceAccess.hpp"
#include "Support/TestContext.hpp"
#include <cstring>
namespace I = Asteria::User::Identity;
namespace C = Asteria::User::Component;
namespace G = Asteria::User::Graphics;
namespace A = Asteria::Abi;
namespace S = Asteria::User::System;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::Size;
struct Kernel
{
    Size Creates{};
    Size Maps{};
    Size Closes{};
    Size Sends{};
    bool Block{};
    UInt8 Permissions{};
    UInt8 Delivery[I::SeatDeliveryBytes]{};
    Size Bytes{};
    static void Call(void* p, const A::SystemCallRequest* r, A::SystemCallResponse* out) noexcept
    {
        auto& k = *static_cast<Kernel*>(p);
        *out = {};
        switch (static_cast<A::SystemCallNumber>(r->Number))
        {
        case A::SystemCallNumber::ObjectCreate:
            out->Value0 = 100U + ++k.Creates;
            out->Value1 = 1U;
            break;
        case A::SystemCallNumber::MemoryMap:
            ++k.Maps;
            k.Permissions = reinterpret_cast<const UInt8*>(r->Arguments[3U])[A::FaultResolvePermissionsOffset];
            break;
        case A::SystemCallNumber::IpcSend:
            ++k.Sends;
            k.Bytes = static_cast<Size>(r->Arguments[3U]) + C::EchoDeliveryHeaderBytes;
            std::memcpy(k.Delivery + C::EchoDeliveryHeaderBytes, reinterpret_cast<const void*>(r->Arguments[2U]),
                        static_cast<Size>(r->Arguments[3U]));
            if (k.Block)
            {
                out->Error = A::SystemCallError::WouldBlock;
            }
            break;
        case A::SystemCallNumber::CapabilityClose:
            ++k.Closes;
            break;
        default:
            break;
        }
    }
};
static Kernel K;
static C::DisplaySession Session;
static C::DisplayInfo Info;
static G::Composition::PlaneStorage Planes;
static G::Composition::DamageList Damage;
static Asteria::User::Console::TextConsole Console;
static I::SeatMessage Message;
static I::SeatMessage Reply;
static Asteria::Foundation::UInt32 Pixels[I::LoginWidth * I::LoginHeight];
static Asteria::Foundation::UInt32 Output[800U * 600U];
static Asteria::Foundation::UInt32 Before[800U * 600U];
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"TrustedDisplay"};
    const S::SystemCallClient client{{Kernel::Call, &K}};
    Info = {.Version = C::DisplayProtocolVersion,
            .Features = C::DisplayFeatureSession,
            .Width = 800U,
            .Height = 600U,
            .Columns = 100U,
            .Rows = 37U,
            .Epoch = 7U};
    Console.Configure(800U, 600U, G::Bold8x16);
    Planes.Initialize(Console);
    Planes.Trusted().Configure({0, 0, 800U, 600U});
    Damage.Configure({0, 0, 800U, 600U});
    Session.Configure({1U, 1U}, {2U, 1U}, {3U, 1U}, 0x80000000U, G::PixelFormat::Bgrx, Info, Planes, Damage);
    Message.Face = I::SeatFace::TrustedDraw;
    Message.Method = I::SeatMethod::OpenDraw;
    Message.Transaction = 4U;
    Message.Words[0U] = 7U;
    Message.Attachment = {4U, 1U};
    ASTERIA_EXPECT(t, "opening trusted face requires root console Manage",
                   !Session.DispatchTrusted(client, Message, 0U), K.Creates == 0U);
    Message.Rights = C::EchoManageRight;
    ASTERIA_EXPECT(t, "separate trusted drawing Endpoint", Session.DispatchTrusted(client, Message, 0U),
                   Session.TrustedFace().Selector != 0U, Session.Face().Selector == 0U);
    K.Block = true;
    Session.Presented(client);
    const auto closes = K.Closes;
    UInt8 frozen[I::SeatDeliveryBytes]{};
    std::memcpy(frozen, K.Delivery, K.Bytes);
    Session.Presented(client);
    ASTERIA_EXPECT(t, "WouldBlock handover stays frozen and retains reply", Session.Retrying(), K.Closes == closes,
                   std::memcmp(frozen, K.Delivery, K.Bytes) == 0);
    K.Block = false;
    Session.Presented(client);
    ASTERIA_EXPECT(t, "handover completes once", I::ReadSeatFrame(K.Delivery, K.Bytes, Reply),
                   Reply.Method == I::SeatMethod::DrawOpened, K.Closes == closes + 1U);
    Asteria::User::Session::SessionFaceMessage ordinary{};
    ordinary.Method = Asteria::User::Session::FaceOpen;
    ordinary.Transaction = 6U;
    ordinary.Words[0U] = 7U;
    ordinary.Attachments[0U] = {7U, 1U};
    ordinary.AttachmentCount = 1U;
    ASTERIA_EXPECT(t, "ordinary pointer routing is live before trusted interval",
                   Session.Dispatch(client, ordinary, C::EchoManageRight, 0U), Session.Face().Selector != 0U);
    ordinary.Method = Asteria::User::Session::FaceCredit;
    ordinary.Words[1U] = 1U;
    ASTERIA_EXPECT(t, "ordinary capture credit is installed",
                   Session.Dispatch(client, ordinary, C::EchoInvokeRight, 1U));
    const UInt64 pointer[8U]{7U, 1U, C::DisplayRelativeMotion, 0U, 0U, 0U, 0U, 1U};
    Session.Pointer(pointer, {20, 20});
    Message.Method = I::SeatMethod::Enroll;
    Message.Words[0U] = 1U;
    Message.Words[1U] = 40U;
    Message.Attachment = {5U, 1U};
    ASTERIA_EXPECT(t, "only root enrolls a drawing incarnation", !Session.DispatchTrusted(client, Message, 1U),
                   Session.DispatchTrusted(client, Message, 3U));
    Message = {};
    Message.Face = I::SeatFace::TrustedDraw;
    Message.Method = I::SeatMethod::Show;
    Message.Words[0U] = 1U;
    ASTERIA_EXPECT(t, "drawing Invoke cannot show", !Session.DispatchTrusted(client, Message, 3U), !Session.Trusted());
    Message.Rights = C::EchoManageRight;
    ASTERIA_EXPECT(t, "root shows opaque trusted plane", Session.DispatchTrusted(client, Message, 3U),
                   Session.Trusted());
    Message.Method = I::SeatMethod::Draw;
    Message.Sender = 39U;
    Message.Words[1U] = 1U;
    Message.Words[2U] = static_cast<UInt64>(G::PixelFormat::Bgrx);
    Message.Attachment = {6U, 1U};
    Message.Transaction = 5U;
    ASTERIA_EXPECT(t, "spoofed incarnation never maps pixels", !Session.DispatchTrusted(client, Message, 3U),
                   K.Maps == 0U);
    Message.Sender = 40U;
    Message.Words[0U] = 2U;
    ASTERIA_EXPECT(t, "old or unissued epoch never maps pixels", !Session.DispatchTrusted(client, Message, 3U),
                   K.Maps == 0U);
    Message.Words[0U] = 1U;
    ASTERIA_EXPECT(t, "only enrolled public pixel Frame maps read-only", Session.DispatchTrusted(client, Message, 3U),
                   K.Maps == 1U, K.Permissions == 0x09U);
    K.Block = true;
    Session.Presented(client);
    const auto sends = K.Sends;
    Session.Presented(client);
    ASTERIA_EXPECT(t, "draw fence retries", Session.Retrying(), K.Sends == sends + 1U);
    K.Block = false;
    Session.Presented(client);
    ASTERIA_EXPECT(t, "a presented serial cannot be replayed", !Session.DispatchTrusted(client, Message, 3U));
    const G::MemorySurface pixels{Pixels, I::LoginWidth, I::LoginHeight, I::LoginWidth, G::PixelFormat::Bgrx};
    I::RenderLogin(pixels, {.State = I::ScreenState::Login,
                            .Edit = I::EditState::Editing,
                            .Account = {reinterpret_cast<const UInt8*>("administrator"), 13U},
                            .MaskCharacters = 6U,
                            .PasswordFocus = true});
    Planes.Trusted().SetBuffer({Pixels, I::LoginWidth, I::LoginHeight, I::LoginWidth, G::PixelFormat::Bgrx});
    const G::MemorySurface output{Output, 800U, 600U, 800U, G::PixelFormat::Bgrx};
    const G::Composition::Output geometry{
        .Scanout = {Output, 800U, 600U, 800U, G::PixelFormat::Bgrx}, .Origin = {0, 0}, .Scale = 1U};
    Planes.Trusted().Render(output, {0, 0, 800U, 600U}, geometry);
    std::memcpy(Before, Output, sizeof(Output));
    Console.Write(reinterpret_cast<const UInt8*>("spoofed console"), 15U);
    Planes.Cursor().Move({20, 20});
    Planes.Trusted().Render(output, {0, 0, 800U, 600U}, geometry);
    ASTERIA_EXPECT(t, "console and pointer changes cannot affect trusted pixels",
                   std::memcmp(Before, Output, sizeof(Output)) == 0,
                   Planes.Trusted().Covers({0, 0, 800U, 600U}, geometry));
    Session.Pointer(pointer, {20, 20});
    const auto pointerSends = K.Sends;
    Session.SendCredit(client);
    ASTERIA_EXPECT(t, "trusted interval excludes pointer snapshots and capture credits", K.Sends == pointerSends);
    Message = {};
    Message.Face = I::SeatFace::TrustedDraw;
    Message.Method = I::SeatMethod::Hide;
    Message.Words[0U] = 1U;
    ASTERIA_EXPECT(t, "no Invoke-only dismissal", !Session.DispatchTrusted(client, Message, 3U), Session.Trusted());
    return t.Finish();
}
