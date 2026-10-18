// SPDX-License-Identifier: Apache-2.0 AND BSD-3-Clause
// RFC 9106 test vectors only; project-authored test logic is Apache-2.0.
// Copyright (c) 2021 IETF Trust and the persons identified as authors of the code. All rights reserved.
// Redistribution and use in source and binary forms, with or without modification, is permitted pursuant to,
// and subject to the license terms contained in, the Revised BSD License set forth in Section 4.c of the
// IETF Trust's Legal Provisions Relating to IETF Documents (https://trustee.ietf.org/license-info).
// See LICENSES/BSD-3-Clause.txt and https://www.rfc-editor.org/rfc/rfc9106.txt.
#include "User/Identity/Support.hpp"
#include "User/Text/Utf8.hpp"
using Asteria::User::Identity::AccountCapacity;
using Asteria::User::Identity::AccountStore;
using Asteria::User::Identity::ArenaBlocks;
using Asteria::User::Identity::AttemptLimiter;
using Asteria::User::Identity::Authentication;
using Asteria::User::Identity::AuthenticationWorkspace;
namespace C = IdentityTest::C;
using Asteria::User::Identity::Cancellation;
using Asteria::User::Identity::Credential;
using Asteria::User::Identity::Entropy;
using Asteria::User::Identity::LimiterState;
using IdentityTest::Memory;
using Asteria::User::Identity::PrincipalContext;
using Asteria::User::Identity::PrincipalId;
using Asteria::User::Identity::Role;
using Asteria::User::Identity::Size;
using Asteria::User::Identity::Status;
using IdentityTest::Time;
using Asteria::User::Identity::TimeTicksPerSecond;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
using IdentityTest::View;
using IdentityTest::Zero;
static Memory Disk;
static AccountStore Store;
static AuthenticationWorkspace Work;
static C::Argon2Block Arena[ArenaBlocks];
static LimiterState State;
static PrincipalContext Output;
static PrincipalContext Before;
struct EntropySource
{
    UInt8 Sequence{1U};
    bool Fail{false};
    Size Calls{0U};
    Entropy Interface() noexcept
    {
        return {this, Fill};
    }
    static bool Fill(void* p, UInt8* bytes, Size n) noexcept
    {
        auto& e = *static_cast<EntropySource*>(p);
        ++e.Calls;
        for (Size i = 0; i < n; ++i)
        {
            bytes[i] = e.Sequence;
        }
        ++e.Sequence;
        return !e.Fail;
    }
};
struct CancelSource
{
    Size Calls{0U};
    Size At{2U};
    Cancellation Interface() noexcept
    {
        return {this, Requested};
    }
    static bool Requested(void* p) noexcept
    {
        auto& c = *static_cast<CancelSource*>(p);
        return ++c.Calls >= c.At;
    }
};
static bool Clean() noexcept
{
    return Zero(&Work, sizeof(Work)) && Zero(Arena, sizeof(Arena));
}
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"IdentityAuthentication"};
    Time time;
    EntropySource entropy;
    AttemptLimiter limiter(State, time.Interface());
    Authentication auth(Work, Arena, ArenaBlocks);
    C::Wipe({reinterpret_cast<UInt8*>(&Work), sizeof(Work)});
    UInt8 password[32U];
    UInt8 salt[16U];
    UInt8 secret[8U];
    UInt8 aad[12U];
    UInt8 tag[32U];
    std::memset(password, 1, 32U);
    std::memset(salt, 2, 16U);
    std::memset(secret, 3, 8U);
    std::memset(aad, 4, 12U);
    const UInt8 expected[]{0x0dU, 0x64U, 0x0dU, 0xf5U, 0x8dU, 0x78U, 0x76U, 0x6cU, 0x08U, 0xc0U, 0x37U,
                           0xa3U, 0x4aU, 0x8bU, 0x53U, 0xc9U, 0xd0U, 0x1eU, 0xf0U, 0x45U, 0x2dU, 0x75U,
                           0xb6U, 0x5eU, 0xb5U, 0x25U, 0x20U, 0xe9U, 0x6bU, 0x01U, 0xe6U, 0x59U};
    ASTERIA_EXPECT(t, "RFC 9106 September 2021 v1.3 section 5.3 KAT",
                   C::Argon2id(Work.CryptoWork, {32U, 3U, 4U}, {password, 32U}, {salt, 16U}, {secret, 8U}, {aad, 12U},
                               Arena, ArenaBlocks, {tag, 32U}) == C::Result::Success,
                   std::memcmp(tag, expected, 32U) == 0, Clean());
    bool utf8 = true;
    UInt64 random = 15U;
    UInt8 bytes[1024U]{};
    for (Size trial = 0U; trial < 5000U; ++trial)
    {
        random = random * 6364136223846793005ULL + 1U;
        Size length = trial < 256U ? 1U : random % 33U;
        bool nul = false;
        for (Size i = 0; i < length; ++i)
        {
            random = random * 6364136223846793005ULL + 1U;
            bytes[i] = trial < 256U ? static_cast<UInt8>(trial) : static_cast<UInt8>(random >> 32U);
            nul |= bytes[i] == 0U;
        }
        bool expectedValid = length != 0U && !nul && Asteria::User::Text::ValidUtf8(bytes, length);
        utf8 &= Authentication::ValidPassword({bytes, length}) == expectedValid;
    }
    std::memset(bytes, ' ', sizeof(bytes));
    ASTERIA_EXPECT(t, "password byte/UTF8 bounds and literal spaces", utf8,
                   Authentication::ValidPassword({bytes, 1024U}), !Authentication::ValidPassword({bytes, 1025U}),
                   !Authentication::ValidPassword({nullptr, 0U}), Authentication::ValidPassword(View(" 密码 ")));
    const auto loginPassword = View(" 密码 password ");
    const auto wrongPassword = View(" 密码 password");
    const auto otherPassword = View("password");
    Credential credential;
    Credential copy;
    std::memset(static_cast<void*>(&credential), 0xA5, sizeof(credential));
    copy = credential;
    entropy.Fail = true;
    ASTERIA_EXPECT(t, "entropy failure wipes preserves output",
                   auth.Enroll(otherPassword, entropy.Interface(), credential) == Status::Unavailable,
                   std::memcmp(&copy, &credential, sizeof(copy)) == 0, Clean());
    entropy.Fail = false;
    bytes[0U] = 0U;
    ASTERIA_EXPECT(t, "malformed password wipes preserves output",
                   auth.Enroll({bytes, 1U}, entropy.Interface(), credential) == Status::Invalid,
                   std::memcmp(&copy, &credential, sizeof(copy)) == 0, Clean());
    Authentication shortArena(Work, Arena, ArenaBlocks - 1U);
    Arena[0U].Word[0U] = 77U;
    ASTERIA_EXPECT(t, "full arena admission refusal before secret",
                   shortArena.Enroll(otherPassword, entropy.Interface(), credential) == Status::Unavailable,
                   Arena[0U].Word[0U] == 77U, std::memcmp(&copy, &credential, sizeof(copy)) == 0);
    Arena[0U].Word[0U] = 0U;
    ASTERIA_EXPECT(t, "overlap refusal preserves caller",
                   auth.Enroll(otherPassword, entropy.Interface(), Work.Stored) == Status::Unavailable, Clean());
    ASTERIA_EXPECT(t, "format unprovisioned", Store.Format(Disk.Interface()) == Status::Ok,
                   Store.Ready() == Status::NotProvisioned);
    UInt64 retry = 77U;
    std::memset(static_cast<void*>(&Output), 0xA5, sizeof(Output));
    Before = Output;
    ASTERIA_EXPECT(t, "unprovisioned store cannot issue principal",
                   auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry) ==
                       Status::NotProvisioned,
                   !State.InFlight, std::memcmp(&Before, &Output, sizeof(Output)) == 0, Clean());
    Output = {};
    Before = {};
    ASTERIA_EXPECT(t, "64 MiB three passes four lanes enrollment",
                   auth.Enroll(Store, 2U, loginPassword, entropy.Interface()) == Status::Ok, Clean(),
                   Store.UpdateAccount(2U, View("administrator"), Role::Admin, false) == Status::Ok,
                   Store.Ready() == Status::Ok);
    ASTERIA_EXPECT(t, "full-profile verify",
                   auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry) ==
                       Status::Ok,
                   Output.AccountId == 2U, Output.Revision == 3U, Output.AllowSubjects.Count == 3U,
                   Output.DenySubjects.Count == 4U, retry == 77U, Clean());
    Before = Output;
    time.Seconds = 1U;
    ASTERIA_EXPECT(t, "full-profile wrong password preserves principal",
                   auth.Verify(Store, limiter, View("administrator"), wrongPassword, 44U, 0U, Output, retry) ==
                       Status::Failed,
                   std::memcmp(&Before, &Output, sizeof(Output)) == 0, Clean());
    const auto failures = State.Accounts[AccountCapacity - 1U].Failures;
    ASTERIA_EXPECT(
        t, "throttle no hash no extension",
        auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry) == Status::RetryAt,
        retry == 2U * TimeTicksPerSecond, State.Accounts[AccountCapacity - 1U].Failures == failures, Clean());
    time.Seconds = 2U;
    CancelSource cancel;
    ASTERIA_EXPECT(t, "cancel after full hash suppresses success",
                   auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry,
                               cancel.Interface()) == Status::Cancelled,
                   cancel.Calls == 2U, std::memcmp(&Before, &Output, sizeof(Output)) == 0,
                   State.Accounts[AccountCapacity - 1U].Failures == 2U, Clean());
    time.Seconds = 4U;
    cancel.Calls = 0U;
    cancel.At = 3U;
    ASTERIA_EXPECT(t, "unknown full dummy hash",
                   auth.Verify(Store, limiter, View("unknown A"), otherPassword, 44U, 0U, Output, retry,
                               cancel.Interface()) == Status::Failed,
                   cancel.Calls == 2U, State.Unknown.Failures == 1U, Clean());
    ASTERIA_EXPECT(t, "different unknown shares bucket",
                   auth.Verify(Store, limiter, View("unknown B"), otherPassword, 44U, 0U, Output, retry) ==
                       Status::RetryAt,
                   retry == 5U * TimeTicksPerSecond, State.Unknown.Failures == 1U);
    PrincipalId backup = 0U;
    ASTERIA_EXPECT(t, "another fully enrolled administrator keeps provisioned state",
                   Store.CreateAccount(View("backup administrator"), Role::Admin, backup) == Status::Ok,
                   auth.Enroll(Store, backup, otherPassword, entropy.Interface()) == Status::Ok,
                   Store.UpdateAccount(backup, View("backup administrator"), Role::Admin, false) == Status::Ok,
                   Clean());
    time.Seconds = 5U;
    ASTERIA_EXPECT(t, "disable", Store.UpdateAccount(2U, View("administrator"), Role::Admin, true) == Status::Ok);
    cancel.Calls = 0U;
    ASTERIA_EXPECT(t, "disabled full dummy hash",
                   auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry,
                               cancel.Interface()) == Status::Failed,
                   cancel.Calls == 2U, std::memcmp(&Before, &Output, sizeof(Output)) == 0, Clean());
    cancel.Calls = 0U;
    cancel.At = 2U;
    ASTERIA_EXPECT(t, "cancel enrollment output preserved",
                   auth.Enroll(otherPassword, entropy.Interface(), credential, cancel.Interface()) == Status::Cancelled,
                   std::memcmp(&copy, &credential, sizeof(copy)) == 0, Clean());
    ASTERIA_EXPECT(t, "fresh salt enrollment one",
                   auth.Enroll(otherPassword, entropy.Interface(), credential) == Status::Ok, Clean());
    copy = credential;
    ASTERIA_EXPECT(t, "fresh independent salt enrollment two",
                   auth.Enroll(otherPassword, entropy.Interface(), credential) == Status::Ok,
                   !C::Equal({copy.Salt, 16U}, {credential.Salt, 16U}),
                   !C::Equal({copy.Tag, 32U}, {credential.Tag, 32U}), Clean());
    State = {};
    time.Seconds = 0U;
    ASTERIA_EXPECT(t, "re-enable", Store.UpdateAccount(2U, View("administrator"), Role::Admin, false) == Status::Ok);
    // Completion clock loss leaves only the root-owned accepted checkpoint.
    struct ClockCancel
    {
        Time* T;
        Size Calls{0U};
        static bool Check(void* p) noexcept
        {
            auto& c = *static_cast<ClockCancel*>(p);
            if (++c.Calls == 2U)
            {
                c.T->Available = false;
            }
            return false;
        }
    } clockCancel{&time};
    ASTERIA_EXPECT(t, "terminal clock loss cannot publish success",
                   auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry,
                               {&clockCancel, ClockCancel::Check}) == Status::Unavailable,
                   State.InFlight, std::memcmp(&Before, &Output, sizeof(Output)) == 0, Clean());
    time.Available = true;
    AttemptLimiter restart(State, time.Interface());
    ASTERIA_EXPECT(t, "restart accounts lost terminal result", restart.Recover() == Status::Ok, !State.InFlight,
                   State.Accounts[AccountCapacity - 1U].Failures == 1U);
    time.Seconds = 1U;
    cancel.Calls = 0U;
    cancel.At = 1U;
    ASTERIA_EXPECT(t, "cancel admitted before hash still accounts",
                   auth.Verify(Store, limiter, View("administrator"), loginPassword, 44U, 0U, Output, retry,
                               cancel.Interface()) == Status::Cancelled,
                   cancel.Calls == 1U, State.Accounts[AccountCapacity - 1U].Failures == 2U,
                   std::memcmp(&Before, &Output, sizeof(Output)) == 0, Clean());
    std::memset(Work.Password, 0xA5U, sizeof(Work.Password));
    const auto overlapPassword = auth.Enroll({Work.Password, 8U}, entropy.Interface(), credential);
    ASTERIA_EXPECT(t, "overlapping secret refusal changes nothing", overlapPassword == Status::Unavailable,
                   Work.Password[0U] == 0xA5U, Zero(Arena, sizeof(Arena)));
    C::Wipe({reinterpret_cast<UInt8*>(&Work), sizeof(Work)});
    Store.Close();
    ASTERIA_EXPECT(t, "store wipe observable", Zero(&Store, sizeof(Store)));
    C::Wipe({reinterpret_cast<UInt8*>(&credential), sizeof(credential)});
    C::Wipe({reinterpret_cast<UInt8*>(&copy), sizeof(copy)});
    return t.Finish();
}
