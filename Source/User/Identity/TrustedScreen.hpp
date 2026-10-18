// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IDENTITY_TRUSTED_SCREEN_HPP_INCLUDED
#define ASTERIA_USER_IDENTITY_TRUSTED_SCREEN_HPP_INCLUDED
#include "User/Identity/Identity.hpp"

namespace Asteria::User::Identity
{
// Root owns this nonsecret seat model. Every transition retires the transaction; epochs and
// transaction IDs never wrap.
enum class ScreenState : UInt8
{
    AwaitingAttention,
    Login,
    StartingSession,
    Active,
    Locked,
    LoggingOut,
    Recovery,
    ElevationPrompt // Reserved; no transition enters it.
};
enum class EditState : UInt8
{
    Editing,
    Verifying,
    RetryAt
};
enum class Purpose : UInt8
{
    Login,
    Unlock,
    Elevation
};
struct ScreenSnapshot
{
    ScreenState State{ScreenState::AwaitingAttention};
    EditState Edit{EditState::Editing};
    UInt64 Epoch{1U};
    UInt64 Transaction{0U};
    UInt64 Attempt{0U};
    UInt64 RetryDeadline{0U};
    PrincipalId Account{0U};
    UInt64 Revision{0U};
    UInt64 Session{0U};
};
class TrustedScreen final
{
public:
    [[nodiscard]] const ScreenSnapshot& Snapshot() const noexcept
    {
        return m_screen;
    }
    [[nodiscard]] bool Trusted() const noexcept
    {
        return m_screen.State != ScreenState::Active;
    }
    [[nodiscard]] bool AcceptsInput() const noexcept;
    [[nodiscard]] bool Attention() noexcept;
    [[nodiscard]] bool Begin(UInt64 epoch, UInt64 transaction) noexcept;
    [[nodiscard]] bool Submitted(UInt64 epoch, UInt64 transaction, UInt64 attempt) noexcept;
    [[nodiscard]] bool Result(UInt64 epoch, UInt64 transaction, UInt64 attempt, Purpose purpose, Status status,
                              PrincipalId account, UInt64 revision, UInt64 retryDeadline) noexcept;
    [[nodiscard]] bool Retry(UInt64 now) noexcept;
    [[nodiscard]] UInt64 RetrySeconds(UInt64 now) const noexcept;
    [[nodiscard]] bool SessionReady(UInt64 epoch, UInt64 session) noexcept;
    // Root calls this only after draining all partial session authority. A failed drain instead
    // enters Recovery through Fail.
    [[nodiscard]] bool SessionFailed(UInt64 epoch) noexcept;
    [[nodiscard]] bool Cancel() noexcept;
    [[nodiscard]] bool Logout() noexcept;
    [[nodiscard]] bool SessionDrained(UInt64 epoch) noexcept;
    void Fail() noexcept;

private:
    [[nodiscard]] bool Enter(ScreenState state, EditState edit = EditState::Editing) noexcept;
    ScreenSnapshot m_screen{};
    UInt64 m_lastTransaction{0U};
};

// Login owns the secret. It cannot reuse its submission page before auth's
// snapshot acknowledgement. Clear also covers cancellation/failure/destruction.
class SecretEntry final
{
public:
    ~SecretEntry() noexcept
    {
        Clear();
    }
    [[nodiscard]] bool Append(TextView bytes) noexcept;
    void Backspace() noexcept;
    void Clear() noexcept;
    [[nodiscard]] TextView View() const noexcept
    {
        return {m_bytes, m_length};
    }
    [[nodiscard]] Size Characters() const noexcept;

private:
    UInt8 m_bytes[MaximumPasswordBytes]{};
    Size m_length{0U};
};
} // Namespace Asteria::User::Identity
#endif
