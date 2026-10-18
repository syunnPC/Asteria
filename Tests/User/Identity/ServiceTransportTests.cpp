// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/Support.hpp"
#include "User/Identity/AuthService.hpp"
#include "User/Identity/LoginService.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/System/DeviceAccess.hpp"
#include <array>
#include <vector>
#include <cstdlib>
using Asteria::User::Identity::AccountCapacity;
using Asteria::User::Identity::AccountStore;
using Asteria::User::Identity::Administrator;
using Asteria::User::Identity::AdvanceAuth;
using Asteria::User::Identity::AdvanceLogin;
using Asteria::User::Identity::ArenaBlocks;
using Asteria::User::Identity::AuthArenaAddress;
using Asteria::User::Identity::AuthArenaPages;
using Asteria::User::Identity::AuthFixtureAddress;
using Asteria::User::Identity::AuthPasswordAddress;
using Asteria::User::Identity::AuthRuns;
using Asteria::User::Identity::Authentication;
using Asteria::User::Identity::AuthenticationWorkspace;
namespace C = IdentityTest::C;
using Asteria::User::Identity::Cancellation;
using Asteria::User::Identity::ClearAuth;
using Asteria::User::Identity::ClearLogin;
using Asteria::User::Identity::InitializeAuth;
using Asteria::User::Identity::InitializeLogin;
using Asteria::User::Identity::LimiterState;
using Asteria::User::Identity::LoginPasswordAddress;
using Asteria::User::Identity::LoginPixelsAddress;
using Asteria::User::Identity::LoginRuntimePages;
using IdentityTest::Memory;
using IdentityTest::Put;
using Asteria::User::Identity::ReadSeatFrame;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::ScreenState;
using Asteria::User::Identity::SeatDeliveryBytes;
using Asteria::User::Identity::SeatFace;
using Asteria::User::Identity::SeatInterfaceLow;
using Asteria::User::Identity::SeatMaximumWords;
using Asteria::User::Identity::SeatMessage;
using Asteria::User::Identity::SeatMethod;
using Asteria::User::Identity::SeatPageBytes;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::Status;
using Asteria::User::Identity::StorageBytes;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
using IdentityTest::View;
using Asteria::User::Identity::WriteSeatFrame;
using IdentityTest::Zero;
namespace A = Asteria::Abi;
namespace S = Asteria::User::System;
namespace P = Asteria::User::Component;
struct Packet
{
    std::array<UInt8, SeatDeliveryBytes> Bytes{};
    Size Length{};
};
struct Queue
{
    std::array<Packet, 64U> Packets{};
    Size First{};
    Size Count{};
    void push_back(const Packet& packet) noexcept
    {
        if (Count == Packets.size())
        {
            std::abort();
        }
        Packets[(First + Count) % Packets.size()] = packet;
        ++Count;
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return Count == 0U;
    }
    [[nodiscard]] const Packet& front() const noexcept
    {
        return Packets[First];
    }
    void pop_front() noexcept
    {
        First = (First + 1U) % Packets.size();
        --Count;
    }
};
static UInt64 ClockTicks{5'000'000U};
static UInt64 ReadClock() noexcept
{
    return ClockTicks;
}
struct Kernel
{
    UInt8* Memory{};
    UInt64 Region{};
    UInt64 Next{100U};
    UInt64 SubmitEndpoint{};
    Size Frames{};
    Size Pages{};
    Size Unmaps{};
    Size Closes{};
    Size Copied{};
    Size Results{};
    Size CancelPoll{};
    bool Block{};
    bool Fail{};
    bool SnapshotMapped{};
    bool SnapshotClosed{};
    bool CheckOrder{true};
    Queue Control;
    Queue Submit;
    std::vector<Packet> Sent, Retried;
    SeatMessage Terminal{};
    Packet Cancellation{};
    static void Transport(void* p, const A::SystemCallRequest* r, A::SystemCallResponse* out) noexcept
    {
        auto& k = *static_cast<Kernel*>(p);
        *out = {};
        const auto* bytes = reinterpret_cast<const UInt8*>(r->Arguments[3U]);
        switch (static_cast<A::SystemCallNumber>(r->Number))
        {
        case A::SystemCallNumber::ObjectCreate:
            out->Value0 = ++k.Next;
            out->Value1 = 1U;
            if (r->Arguments[2U] == A::ObjectTypeFrame)
            {
                ++k.Frames;
                k.Pages += S::LoadWord(bytes);
            }
            if (r->Arguments[2U] == A::ObjectTypeEndpoint)
            {
                k.SubmitEndpoint = out->Value0;
            }
            break;
        case A::SystemCallNumber::MemoryMap:
            if (S::LoadWord(bytes + A::FaultResolveVirtualStartOffset) == k.Region + AuthPasswordAddress)
            {
                k.SnapshotMapped = true;
                k.CheckOrder = k.CheckOrder && bytes[A::FaultResolvePermissionsOffset] == 0x09U;
            }
            break;
        case A::SystemCallNumber::MemoryUnmap:
            ++k.Unmaps;
            k.SnapshotMapped = false;
            break;
        case A::SystemCallNumber::CapabilityClose:
            ++k.Closes;
            if (r->Arguments[0U] == 90U)
            {
                k.SnapshotClosed = true;
            }
            break;
        case A::SystemCallNumber::IpcReceive:
        {
            if (k.Fail)
            {
                out->Error = A::SystemCallError::RightsDenied;
                break;
            }
            auto& queue = r->Arguments[0U] == k.SubmitEndpoint ? k.Submit : k.Control;
            if (r->Arguments[0U] == 8U && k.CancelPoll != 0U && --k.CancelPoll == 0U)
            {
                queue.push_back(k.Cancellation);
            }
            if (queue.empty())
            {
                out->Error = A::SystemCallError::WouldBlock;
                break;
            }
            const auto packet = queue.front();
            queue.pop_front();
            std::memcpy(reinterpret_cast<void*>(r->Arguments[2U]), packet.Bytes.data(), packet.Length);
            out->Value1 = packet.Length;
            ClockTicks += 1000U;
            break;
        }
        case A::SystemCallNumber::IpcSend:
        {
            Packet packet{};
            packet.Length = P::EchoDeliveryHeaderBytes + static_cast<Size>(r->Arguments[3U]);
            std::memcpy(packet.Bytes.data() + P::EchoDeliveryHeaderBytes,
                        reinterpret_cast<const void*>(r->Arguments[2U]), static_cast<Size>(r->Arguments[3U]));
            SeatMessage message{};
            const bool valid = ReadSeatFrame(packet.Bytes.data(), packet.Length, message);
            if (k.Block)
            {
                k.Retried.push_back(packet);
                out->Error = A::SystemCallError::WouldBlock;
                break;
            }
            k.Sent.push_back(packet);
            if (valid && message.Method == SeatMethod::Copied)
            {
                ++k.Copied;
                k.CheckOrder = k.CheckOrder && !k.SnapshotMapped && k.SnapshotClosed;
            }
            if (valid && message.Method == SeatMethod::Result)
            {
                ++k.Results;
                k.Terminal = message;
            }
            break;
        }
        default:
            break;
        }
    }
    Packet Message(SeatFace face, SeatMethod method, const UInt64* words, S::CapabilityHandle attachment = {},
                   UInt64 sender = 7U)
    {
        Packet p{};
        p.Length = P::EchoDeliveryHeaderBytes +
                   WriteSeatFrame(p.Bytes.data() + P::EchoDeliveryHeaderBytes, face, method, 1U, words, attachment);
        S::StoreWord(p.Bytes.data() + 24U, sender);
        S::StoreWord(p.Bytes.data() + 32U, SeatInterfaceLow + static_cast<UInt64>(face));
        Put(p.Bytes.data() + 14U, 2U, P::EchoManageRight | P::EchoInvokeRight);
        return p;
    }
};
static Memory Disk;
static AccountStore Store;
static AuthenticationWorkspace Work;
static C::Argon2Block Arena[ArenaBlocks];
static Kernel AuthKernel;
static Kernel LoginKernel;
static UInt64 Words[SeatMaximumWords]{};
static UInt64 Bootstrap[A::ComponentBlockWords]{};
static bool FillEntropy(void*, UInt8* p, Size n) noexcept
{
    for (Size i = 0U; i < n; ++i)
    {
        p[i] = static_cast<UInt8>(i + 1U);
    }
    return true;
}
static void ClearWords()
{
    for (auto& w : Words)
    {
        w = 0U;
    }
}
static void AuthControl(SeatMethod method, S::CapabilityHandle attachment = {})
{
    AuthKernel.Control.push_back(AuthKernel.Message(SeatFace::AuthControl, method, Words, attachment));
}
static void Begin(UInt64 epoch)
{
    ClearWords();
    Words[0U] = epoch;
    Words[1U] = 9U;
    Words[4U] = 1U;
    AuthControl(SeatMethod::Begin);
    static_cast<void>(AdvanceAuth());
}
static UInt64 Transaction()
{
    SeatMessage m{};
    for (auto i = AuthKernel.Sent.rbegin(); i != AuthKernel.Sent.rend(); ++i)
    {
        if (ReadSeatFrame(i->Bytes.data(), i->Length, m) && m.Method == SeatMethod::Reply)
        {
            return m.Words[2U];
        }
    }
    return 0U;
}
static void Attempt(UInt64 epoch, UInt64 transaction, const char* password)
{
    ClearWords();
    Words[0U] = epoch;
    Words[1U] = transaction;
    Words[2U] = 1U;
    Words[3U] = 13U;
    Words[4U] = std::strlen(password);
    for (Size i = 0U; i < 13U; ++i)
    {
        Words[5U + i / 8U] |= static_cast<UInt64>("administrator"[i]) << (8U * (i % 8U));
    }
    std::memcpy(reinterpret_cast<void*>(AuthKernel.Region + AuthPasswordAddress), password, std::strlen(password));
    AuthKernel.SnapshotClosed = false;
    AuthKernel.Submit.push_back(AuthKernel.Message(SeatFace::AuthSubmit, SeatMethod::Submit, Words, {90U, 1U}, 9U));
    static_cast<void>(AdvanceAuth());
}
static void Auth(Asteria::TestSupport::TestContext& t)
{
    Authentication enroll{Work, Arena, ArenaBlocks};
    ASTERIA_EXPECT(t, "synthetic fixture full profile", Store.Format(Disk.Interface()) == Status::Ok,
                   enroll.Enroll(Store, Administrator, View("synthetic"), {nullptr, FillEntropy}) == Status::Ok,
                   Store.UpdateAccount(Administrator, View("administrator"), Role::Admin, false) == Status::Ok);
    constexpr Size extent = AuthFixtureAddress + StorageBytes + SeatPageBytes - AuthArenaAddress;
    AuthKernel.Memory = static_cast<UInt8*>(std::calloc(1U, extent));
    ASTERIA_EXPECT(t, "host admission storage", AuthKernel.Memory != nullptr);
    if (!AuthKernel.Memory)
    {
        return;
    }
    AuthKernel.Region = reinterpret_cast<UInt64>(AuthKernel.Memory) - AuthArenaAddress;
    std::memcpy(reinterpret_cast<void*>(AuthKernel.Region + AuthFixtureAddress), Disk.Bytes, StorageBytes);
    *reinterpret_cast<LimiterState*>(AuthKernel.Region + AuthFixtureAddress + StorageBytes) = LimiterState{};
    Bootstrap[A::ComponentIncarnationWord] = 3U;
    Bootstrap[A::ComponentClockTicksPerMicrosecondWord] = 1U;
    Bootstrap[A::ComponentReceiveWord] = 8U;
    Bootstrap[A::ComponentReportWord] = 2U;
    Bootstrap[45U] = 91U;
    Bootstrap[50U] = 92U;
    const S::SystemCallClient client{{Kernel::Transport, &AuthKernel}};
    ASTERIA_EXPECT(t, "funding before readiness uses full 64 MiB",
                   InitializeAuth(client, Bootstrap, ReadClock, AuthKernel.Region), AuthKernel.Frames == AuthRuns,
                   AuthKernel.Pages == AuthArenaPages);
    ClearWords();
    Words[0U] = 3U;
    Words[1U] = 9U;
    AuthControl(SeatMethod::Configure, {40U, 1U});
    static_cast<void>(AdvanceAuth());
    Begin(2U);
    const auto transaction = Transaction();
    Attempt(2U, transaction, "synthetic");
    ASTERIA_EXPECT(t, "snapshot copied, unmapped and closed before acknowledgement", AuthKernel.Copied == 1U,
                   AuthKernel.CheckOrder);
    ASTERIA_EXPECT(t, "real service full-profile success and wipe", AdvanceAuth(), AuthKernel.Results == 1U,
                   AuthKernel.Terminal.Words[4U] == static_cast<UInt64>(Status::Ok),
                   AuthKernel.Terminal.Words[9U] == 1U);
    const UInt64 revision = AuthKernel.Terminal.Words[6U];
    ClearWords();
    Words[0U] = Administrator;
    Words[1U] = revision;
    AuthControl(SeatMethod::CheckRevision);
    static_cast<void>(AdvanceAuth());
    SeatMessage checked{};
    ASTERIA_EXPECT(t, "authenticated revision check accepts current account",
                   ReadSeatFrame(AuthKernel.Sent.back().Bytes.data(), AuthKernel.Sent.back().Length, checked),
                   checked.Words[1U] == static_cast<UInt64>(Status::Ok));
    ClearWords();
    Words[0U] = Administrator;
    Words[1U] = revision + 1U;
    AuthControl(SeatMethod::CheckRevision);
    static_cast<void>(AdvanceAuth());
    ASTERIA_EXPECT(t, "authenticated revision check rejects stale revision",
                   ReadSeatFrame(AuthKernel.Sent.back().Bytes.data(), AuthKernel.Sent.back().Length, checked),
                   checked.Words[1U] == static_cast<UInt64>(Status::Failed));
    ClockTicks += 2'000'000U;
    Begin(3U);
    Attempt(3U, Transaction(), "wrong");
    ASTERIA_EXPECT(t, "real service full-profile failure and wipe", AdvanceAuth(), AuthKernel.Results == 2U,
                   AuthKernel.Terminal.Words[4U] == static_cast<UInt64>(Status::Failed),
                   AuthKernel.Terminal.Words[9U] == 1U);
    ClockTicks += 2'000'000U;
    Begin(4U);
    const auto cancelled = Transaction();
    Attempt(4U, cancelled, "synthetic");
    ClearWords();
    Words[0U] = 4U;
    Words[1U] = cancelled;
    AuthKernel.Cancellation = AuthKernel.Message(SeatFace::AuthControl, SeatMethod::Cancel, Words);
    AuthKernel.CancelPoll = 2U;
    ASTERIA_EXPECT(t, "cancellation after segment starts still wipes full arena", AdvanceAuth(),
                   AuthKernel.Results == 3U, AuthKernel.Terminal.Words[4U] == static_cast<UInt64>(Status::Cancelled),
                   AuthKernel.Terminal.Words[9U] == 1U);
    bool secretFree = true;
    for (const auto& packet : AuthKernel.Sent)
    {
        const auto secret = View("synthetic");
        secretFree = secretFree && std::search(packet.Bytes.begin(),
                                               packet.Bytes.begin() + static_cast<std::ptrdiff_t>(packet.Length),
                                               secret.Bytes, secret.Bytes + secret.Length) ==
                                       packet.Bytes.begin() + static_cast<std::ptrdiff_t>(packet.Length);
    }
    ASTERIA_EXPECT(t, "credential bytes never enter an IPC payload", secretFree);
    const auto before = AuthKernel.Results;
    Attempt(3U, transaction, "synthetic");
    ASTERIA_EXPECT(t, "old epoch and retired transaction cannot verify", AdvanceAuth(), AuthKernel.Results == before);
    ClearWords();
    Words[0U] = 3U;
    AuthKernel.Block = true;
    AuthControl(SeatMethod::Query, {43U, 1U});
    static_cast<void>(AdvanceAuth());
    static_cast<void>(AdvanceAuth());
    ASTERIA_EXPECT(t, "WouldBlock preserves exact query bytes and attachment", AuthKernel.Retried.size() >= 2U,
                   AuthKernel.Retried.size() < 2U || AuthKernel.Retried[0U].Bytes == AuthKernel.Retried[1U].Bytes);
    const auto closes = AuthKernel.Closes;
    AuthKernel.Block = false;
    ASTERIA_EXPECT(t, "retained borrowed Endpoint closes after delivery", AdvanceAuth(),
                   AuthKernel.Closes == closes + 1U);
    auto* limiter = reinterpret_cast<LimiterState*>(AuthKernel.Region + AuthFixtureAddress + StorageBytes);
    const auto failures = limiter->Accounts[AccountCapacity - 1U].Failures;
    ClearAuth();
    Bootstrap[A::ComponentIncarnationWord] = 4U;
    ASTERIA_EXPECT(t, "nonsecret limiter survives a new auth incarnation",
                   InitializeAuth(client, Bootstrap, ReadClock, AuthKernel.Region),
                   limiter->Accounts[AccountCapacity - 1U].Failures == failures, failures >= 2U);
    ClearWords();
    Words[0U] = 4U;
    Words[1U] = 9U;
    AuthControl(SeatMethod::Configure, {40U, 1U});
    static_cast<void>(AdvanceAuth());
    ClockTicks += 6'000'000U;
    Begin(6U);
    const auto retained = Transaction();
    Attempt(6U, retained, "synthetic");
    const auto published = AuthKernel.Results;
    AuthKernel.Block = true;
    ASTERIA_EXPECT(t, "completed success can await its bounded reply", AdvanceAuth(), AuthKernel.Results == published,
                   ReadSeatFrame(AuthKernel.Retried.back().Bytes.data(), AuthKernel.Retried.back().Length, checked),
                   checked.Method == SeatMethod::Result, checked.Words[4U] == static_cast<UInt64>(Status::Ok),
                   checked.Words[9U] == 1U);
    ClearWords();
    Words[0U] = 6U;
    Words[1U] = retained;
    AuthControl(SeatMethod::Cancel);
    ASTERIA_EXPECT(t, "root cancellation is consumed while reply stays blocked", AdvanceAuth());
    AuthKernel.Block = false;
    ASTERIA_EXPECT(t, "cancel discards a WouldBlock success before publication", AdvanceAuth(),
                   AuthKernel.Results == published);
    Begin(7U);
    ASTERIA_EXPECT(t, "retired outcome does not block a fresh transaction", Transaction() != retained);
    ClearAuth();
    std::free(AuthKernel.Memory);
}
static void LoginControl(SeatMethod method, S::CapabilityHandle attachment = {})
{
    LoginKernel.Control.push_back(LoginKernel.Message(SeatFace::LoginControl, method, Words, attachment));
    static_cast<void>(AdvanceLogin());
}
static void Key(Asteria::Foundation::UInt16 usage, Asteria::Foundation::UInt32 source = 1U,
                Asteria::Foundation::UInt8 modifiers = 0U)
{
    Asteria::User::Input::InputEvent event{.Kind = Asteria::User::Input::EventKind::KeyDown,
                                           .Modifiers = modifiers,
                                           .Usage = usage,
                                           .Source = source,
                                           .Dx = 0,
                                           .Dy = 0,
                                           .Buttons = 0U,
                                           .Reserved = {},
                                           .Wheel = 0};
    Packet packet{};
    const UInt64 w[4U]{42U, 1U, 1U, 0U};
    packet.Length = P::EchoDeliveryHeaderBytes + P::WriteInputFrame(packet.Bytes.data() + P::EchoDeliveryHeaderBytes,
                                                                    P::InputEventsMethod, 1U, w, &event, 1U);
    S::StoreWord(packet.Bytes.data() + 24U, 10U);
    LoginKernel.Control.push_back(packet);
    static_cast<void>(AdvanceLogin());
    static_cast<void>(AdvanceLogin());
}
static void Keys(const char* value)
{
    for (const char* p = value; *p; ++p)
    {
        Asteria::User::Input::InputEvent event{.Kind = Asteria::User::Input::EventKind::KeyDown,
                                               .Modifiers = 0U,
                                               .Usage = static_cast<Asteria::Foundation::UInt16>(*p == '\t' ? 43
                                                                                                 : *p == '\n'
                                                                                                     ? 40
                                                                                                     : *p - 'a' + 4),
                                               .Source = 1U,
                                               .Dx = 0,
                                               .Dy = 0,
                                               .Buttons = 0U,
                                               .Reserved = {},
                                               .Wheel = 0};
        Packet packet{};
        const UInt64 w[4U]{42U, 1U, 1U, 0U};
        packet.Length =
            P::EchoDeliveryHeaderBytes + P::WriteInputFrame(packet.Bytes.data() + P::EchoDeliveryHeaderBytes,
                                                            P::InputEventsMethod, 1U, w, &event, 1U);
        S::StoreWord(packet.Bytes.data() + 24U, 10U);
        LoginKernel.Control.push_back(packet);
        static_cast<void>(AdvanceLogin());
        static_cast<void>(AdvanceLogin());
    }
}
static void Login(Asteria::TestSupport::TestContext& t)
{
    LoginKernel.Memory =
        static_cast<UInt8*>(std::calloc(1U, LoginPasswordAddress + SeatPageBytes - LoginPixelsAddress));
    ASTERIA_EXPECT(t, "login host storage", LoginKernel.Memory != nullptr);
    if (!LoginKernel.Memory)
    {
        return;
    }
    LoginKernel.Region = reinterpret_cast<UInt64>(LoginKernel.Memory) - LoginPixelsAddress;
    Bootstrap[A::ComponentIncarnationWord] = 5U;
    Bootstrap[A::ComponentReceiveWord] = 8U;
    Bootstrap[A::ComponentMountFlagsWord] = 1U;
    Bootstrap[A::ComponentReplyWord] = 17U;
    const S::SystemCallClient client{{Kernel::Transport, &LoginKernel}};
    ASTERIA_EXPECT(t, "login has bounded pixels and submission page",
                   InitializeLogin(client, Bootstrap, LoginKernel.Region), LoginKernel.Pages == LoginRuntimePages);
    static_cast<void>(AdvanceLogin());
    ClearWords();
    Words[0U] = 5U;
    Words[1U] = 9U;
    Words[2U] = 1U;
    LoginControl(SeatMethod::Configure, {41U, 1U});
    ClearWords();
    Words[0U] = 5U;
    Words[1U] = 10U;
    Words[2U] = 3U;
    Words[3U] = 42U;
    LoginControl(SeatMethod::Configure, {42U, 1U});
    ClearWords();
    Words[0U] = 5U;
    Words[1U] = 11U;
    Words[2U] = 2U;
    LoginControl(SeatMethod::Configure, {43U, 1U});
    ClearWords();
    Words[0U] = 2U;
    Words[1U] = static_cast<UInt64>(ScreenState::Login);
    Words[3U] = 3U;
    LoginControl(SeatMethod::Screen);
    static_cast<void>(AdvanceLogin());
    ClearWords();
    Words[0U] = 2U;
    Words[2U] = 1U;
    LoginKernel.Control.push_back(LoginKernel.Message(SeatFace::LoginControl, SeatMethod::Reply, Words, {}, 11U));
    static_cast<void>(AdvanceLogin());
    static_cast<void>(AdvanceLogin());
    Keys("administrator\tsecret\n");
    const auto* password = reinterpret_cast<const UInt8*>(LoginKernel.Region + LoginPasswordAddress);
    ASTERIA_EXPECT(t, "only the submission page holds counted secret", std::memcmp(password, "secret", 6U) == 0);
    ClearWords();
    Words[0U] = 3U;
    Words[1U] = static_cast<UInt64>(ScreenState::AwaitingAttention);
    LoginControl(SeatMethod::Screen);
    ASTERIA_EXPECT(t, "cancellation wipes entire submission page", Zero(password, SeatPageBytes));
    ClearWords();
    Words[0U] = 2U;
    Words[1U] = static_cast<UInt64>(ScreenState::Login);
    Words[3U] = 3U;
    LoginControl(SeatMethod::Screen);
    Keys("administrator\tsecret\n");
    ASTERIA_EXPECT(t, "old screen cannot reenter editing", Zero(password, SeatPageBytes));
    ClearWords();
    Words[0U] = 4U;
    Words[1U] = static_cast<UInt64>(ScreenState::Login);
    Words[3U] = 4U;
    LoginControl(SeatMethod::Screen);
    static_cast<void>(AdvanceLogin());
    ClearWords();
    Words[0U] = 4U;
    Words[2U] = 1U;
    LoginKernel.Control.push_back(LoginKernel.Message(SeatFace::LoginControl, SeatMethod::Reply, Words, {}, 11U));
    static_cast<void>(AdvanceLogin());
    static_cast<void>(AdvanceLogin());
    Keys("administrator\t");
    Key(59U);
    Key(0x89U);
    Key(31U, 1U, 2U);
    Key(31U, 2U, 2U);
    Key(40U, 2U);
    const UInt8 utf8[4U]{0xC2U, 0xA5U, '"', '@'};
    ASTERIA_EXPECT(t, "JIS yen is UTF-8 and each connected keyboard keeps its layout",
                   std::memcmp(password, utf8, sizeof(utf8)) == 0);
    ClearWords();
    Words[0U] = 5U;
    Words[1U] = static_cast<UInt64>(ScreenState::AwaitingAttention);
    LoginControl(SeatMethod::Screen);
    ASTERIA_EXPECT(t, "UTF-8 submission wipes on exit", Zero(password, SeatPageBytes));
    ClearWords();
    Words[0U] = 6U;
    Words[1U] = static_cast<UInt64>(ScreenState::Login);
    Words[3U] = 6U;
    LoginControl(SeatMethod::Screen);
    static_cast<void>(AdvanceLogin());
    ClearWords();
    Words[0U] = 6U;
    Words[2U] = 1U;
    LoginKernel.Control.push_back(LoginKernel.Message(SeatFace::LoginControl, SeatMethod::Reply, Words, {}, 11U));
    static_cast<void>(AdvanceLogin());
    static_cast<void>(AdvanceLogin());
    Keys("administrator\tsecret\n");
    ASTERIA_EXPECT(t, "failure test starts with a live submitted secret", std::memcmp(password, "secret", 6U) == 0);
    LoginKernel.Fail = true;
    ASTERIA_EXPECT(t, "failed receive clears secrets", !AdvanceLogin(), Zero(password, SeatPageBytes));
    ClearLogin();
    std::free(LoginKernel.Memory);
}
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"IdentityServiceTransport"};
    Auth(t);
    Login(t);
    return t.Finish();
}
