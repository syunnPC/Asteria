// SPDX-License-Identifier: Apache-2.0
#include "User/Identity/TrustedScreen.hpp"
#include "User/Identity/Support.hpp"
using Asteria::User::Identity::MaximumPasswordBytes;
using Asteria::User::Identity::Purpose;
using Asteria::User::Identity::ScreenState;
using Asteria::User::Identity::SecretEntry;
using Asteria::User::Identity::Status;
using Asteria::User::Identity::TimeTicksPerSecond;
using Asteria::User::Identity::TrustedScreen;
using Asteria::User::Identity::UInt64;
using Asteria::User::Identity::UInt8;
using IdentityTest::View;
using IdentityTest::Zero;
int main() noexcept
{
    Asteria::TestSupport::TestContext t{"TrustedScreen"};
    TrustedScreen screen;
    ASTERIA_EXPECT(t, "boot requires SAS", screen.Trusted(), !screen.AcceptsInput(), !screen.Begin(1U, 1U));
    ASTERIA_EXPECT(t, "SAS enters login", screen.Attention(), screen.Snapshot().State == ScreenState::Login,
                   screen.Begin(2U, 1U), screen.AcceptsInput(), screen.Submitted(2U, 1U, 1U), !screen.AcceptsInput());
    ASTERIA_EXPECT(t, "stale epoch serial and purpose cannot publish",
                   !screen.Result(1U, 1U, 1U, Purpose::Login, Status::Ok, 2U, 3U, 0U),
                   !screen.Result(2U, 1U, 2U, Purpose::Login, Status::Ok, 2U, 3U, 0U),
                   !screen.Result(2U, 1U, 1U, Purpose::Unlock, Status::Ok, 2U, 3U, 0U));
    const UInt64 deadline = TimeTicksPerSecond + 1U;
    ASTERIA_EXPECT(t, "failure waits until deadline",
                   screen.Result(2U, 1U, 1U, Purpose::Login, Status::Failed, 0U, 0U, deadline),
                   screen.Snapshot().Epoch == 3U, screen.RetrySeconds(0U) == 2U, screen.RetrySeconds(1U) == 1U,
                   screen.RetrySeconds(deadline - 1U) == 1U, !screen.Retry(deadline - 1U), !screen.AcceptsInput(),
                   screen.RetrySeconds(deadline) == 0U, screen.Retry(deadline), screen.RetrySeconds(0U) == 0U,
                   screen.Begin(4U, 2U));
    ASTERIA_EXPECT(t, "one success creates a pending session", screen.Submitted(4U, 2U, 1U),
                   screen.Result(4U, 2U, 1U, Purpose::Login, Status::Ok, 2U, 3U, 0U), screen.Trusted(),
                   !screen.Result(4U, 2U, 1U, Purpose::Login, Status::Ok, 2U, 3U, 0U), !screen.SessionReady(4U, 9U),
                   screen.SessionReady(5U, 9U), !screen.Trusted());
    ASTERIA_EXPECT(t, "second SAS never unlocks", screen.Attention(), screen.Snapshot().State == ScreenState::Locked,
                   screen.Attention(), screen.Snapshot().State == ScreenState::Locked, screen.Trusted(),
                   screen.Snapshot().Account == 2U);
    auto epoch = screen.Snapshot().Epoch;
    ASTERIA_EXPECT(t, "unlock fixes account", screen.Begin(epoch, 3U), screen.Submitted(epoch, 3U, 1U),
                   !screen.Result(epoch, 3U, 1U, Purpose::Unlock, Status::Ok, 256U, 1U, 0U),
                   screen.Result(epoch, 3U, 1U, Purpose::Unlock, Status::Ok, 2U, 3U, 0U), !screen.Trusted());
    ASTERIA_EXPECT(t, "logout awaits actual drain", screen.Attention(), screen.Logout(), screen.Trusted(),
                   !screen.Attention(), !screen.SessionDrained(1U), screen.SessionDrained(screen.Snapshot().Epoch),
                   screen.Snapshot().State == ScreenState::AwaitingAttention, screen.Snapshot().Account == 0U);
    ASTERIA_EXPECT(t, "cancel retires transaction", screen.Attention(), screen.Begin(screen.Snapshot().Epoch, 4U),
                   screen.Cancel(), screen.Snapshot().State == ScreenState::AwaitingAttention, screen.Attention(),
                   !screen.Begin(screen.Snapshot().Epoch, 4U));
    screen.Fail();
    ASTERIA_EXPECT(t, "failure grants nothing", screen.Trusted(), !screen.AcceptsInput(), !screen.Attention(),
                   screen.Snapshot().State == ScreenState::Recovery, screen.Snapshot().Transaction == 0U);
    TrustedScreen rollback;
    ASTERIA_EXPECT(t, "only current pending startup can roll back", !rollback.SessionFailed(1U), rollback.Attention(),
                   rollback.Begin(2U, 1U), rollback.Submitted(2U, 1U, 1U),
                   rollback.Result(2U, 1U, 1U, Purpose::Login, Status::Ok, 2U, 3U, 0U), !rollback.SessionFailed(2U),
                   rollback.SessionFailed(3U), rollback.Trusted(), rollback.Snapshot().State == ScreenState::Login,
                   rollback.Snapshot().Epoch == 4U, rollback.Snapshot().Account == 0U,
                   rollback.Snapshot().Revision == 0U, rollback.Snapshot().Session == 0U,
                   !rollback.SessionReady(3U, 9U), !rollback.Result(2U, 1U, 1U, Purpose::Login, Status::Ok, 2U, 3U, 0U),
                   !rollback.Begin(4U, 1U), rollback.Begin(4U, 2U), rollback.Submitted(4U, 2U, 1U),
                   rollback.Result(4U, 2U, 1U, Purpose::Login, Status::Ok, 256U, 1U, 0U),
                   rollback.SessionReady(5U, 10U));
    SecretEntry secret;
    ASTERIA_EXPECT(t, "counted UTF8 editing masks characters", secret.Append(View("a 密码")), secret.Characters() == 4U,
                   secret.View().Length == 8U);
    secret.Backspace();
    ASTERIA_EXPECT(t, "backspace wipes complete last codepoint", secret.View().Length == 5U, secret.Characters() == 3U);
    const auto* bytes = secret.View().Bytes;
    ASTERIA_EXPECT(t, "deleted secret tail is zero", Zero(bytes + 5U, MaximumPasswordBytes - 5U));
    secret.Clear();
    ASTERIA_EXPECT(t, "terminal cancellation failure wipe", secret.View().Length == 0U,
                   Zero(bytes, MaximumPasswordBytes));
    UInt8 maximum[MaximumPasswordBytes]{};
    std::memset(maximum, 'x', sizeof(maximum));
    ASTERIA_EXPECT(t, "no truncation or malformed append", secret.Append({maximum, sizeof(maximum)}),
                   !secret.Append(View("x")), secret.View().Length == MaximumPasswordBytes);
    secret.Clear();
    maximum[0U] = 0U;
    ASTERIA_EXPECT(t, "NUL refused before modification", !secret.Append({maximum, 1U}), secret.View().Length == 0U,
                   Zero(bytes, MaximumPasswordBytes));
    return t.Finish();
}
