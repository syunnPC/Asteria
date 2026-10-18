// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/TrustedScreen.hpp"
#include "User/Text/Utf8.hpp"
namespace Asteria::User::Identity
{
bool TrustedScreen::AcceptsInput() const noexcept
{
    return (m_screen.State == ScreenState::Login || m_screen.State == ScreenState::Locked) &&
           m_screen.Edit == EditState::Editing && m_screen.Transaction != 0U;
}
bool TrustedScreen::Enter(const ScreenState state, const EditState edit) noexcept
{
    if (m_screen.Epoch == MaximumCounter)
    {
        m_screen.State = ScreenState::Recovery;
        m_screen.Transaction = 0U;
        return false;
    }
    ++m_screen.Epoch;
    m_screen.State = state;
    m_screen.Edit = edit;
    m_screen.Transaction = 0U;
    m_screen.Attempt = 0U;
    m_screen.RetryDeadline = 0U;
    return true;
}
bool TrustedScreen::Attention() noexcept
{
    switch (m_screen.State)
    {
    case ScreenState::AwaitingAttention:
        return Enter(ScreenState::Login);
    case ScreenState::Active:
        return Enter(ScreenState::Locked);
    case ScreenState::Locked:
        return Enter(ScreenState::Locked);
    case ScreenState::Login:
        return Enter(ScreenState::Login);
    default:
        return false;
    }
}
bool TrustedScreen::Begin(const UInt64 epoch, const UInt64 transaction) noexcept
{
    if (epoch != m_screen.Epoch || transaction == 0U || transaction <= m_lastTransaction ||
        m_screen.Transaction != 0U || m_screen.Edit != EditState::Editing ||
        (m_screen.State != ScreenState::Login && m_screen.State != ScreenState::Locked))
    {
        return false;
    }
    m_lastTransaction = transaction;
    m_screen.Transaction = transaction;
    return true;
}
bool TrustedScreen::Submitted(const UInt64 epoch, const UInt64 transaction, const UInt64 attempt) noexcept
{
    if (!AcceptsInput() || epoch != m_screen.Epoch || transaction != m_screen.Transaction || attempt == 0U ||
        attempt <= m_screen.Attempt)
    {
        return false;
    }
    m_screen.Attempt = attempt;
    m_screen.Edit = EditState::Verifying;
    return true;
}
bool TrustedScreen::Result(const UInt64 epoch, const UInt64 transaction, const UInt64 attempt, const Purpose purpose,
                           const Status status, const PrincipalId account, const UInt64 revision,
                           const UInt64 retryDeadline) noexcept
{
    if (epoch != m_screen.Epoch || transaction == 0U || transaction != m_screen.Transaction || attempt == 0U ||
        attempt != m_screen.Attempt || m_screen.Edit != EditState::Verifying ||
        (m_screen.State != ScreenState::Login && m_screen.State != ScreenState::Locked) ||
        purpose != (m_screen.State == ScreenState::Locked ? Purpose::Unlock : Purpose::Login))
    {
        return false;
    }
    if (status == Status::Ok)
    {
        if (account == 0U || revision == 0U || (account != Administrator && account < FirstCustomId) ||
            (m_screen.State == ScreenState::Locked && account != m_screen.Account))
        {
            return false;
        }
        const auto next = m_screen.State == ScreenState::Locked ? ScreenState::Active : ScreenState::StartingSession;
        if (!Enter(next))
        {
            return false;
        }
        m_screen.Account = account;
        m_screen.Revision = revision;
        return true;
    }
    if (status == Status::Failed || status == Status::RetryAt || status == Status::Cancelled)
    {
        const auto state = m_screen.State;
        if (!Enter(state, EditState::RetryAt))
        {
            return false;
        }
        m_screen.RetryDeadline = retryDeadline;
        return true;
    }
    Fail();
    return true;
}
bool TrustedScreen::Retry(const UInt64 now) noexcept
{
    if (m_screen.Edit != EditState::RetryAt || now < m_screen.RetryDeadline ||
        (m_screen.State != ScreenState::Login && m_screen.State != ScreenState::Locked))
    {
        return false;
    }
    return Enter(m_screen.State);
}
UInt64 TrustedScreen::RetrySeconds(const UInt64 now) const noexcept
{
    // Public presentation rounds up, never shortening admission.
    if (m_screen.Edit != EditState::RetryAt || now >= m_screen.RetryDeadline)
    {
        return 0U;
    }
    const UInt64 remaining = m_screen.RetryDeadline - now;
    return remaining / TimeTicksPerSecond + (remaining % TimeTicksPerSecond != 0U ? 1U : 0U);
}
bool TrustedScreen::SessionReady(const UInt64 epoch, const UInt64 session) noexcept
{
    if (epoch != m_screen.Epoch || m_screen.State != ScreenState::StartingSession || session == 0U)
    {
        return false;
    }
    if (!Enter(ScreenState::Active))
    {
        return false;
    }
    m_screen.Session = session;
    return true;
}
bool TrustedScreen::SessionFailed(const UInt64 epoch) noexcept
{
    if (epoch != m_screen.Epoch || m_screen.State != ScreenState::StartingSession)
    {
        return false;
    }
    if (!Enter(ScreenState::Login))
    {
        return false;
    }
    m_screen.Account = 0U;
    m_screen.Revision = 0U;
    m_screen.Session = 0U;
    return true;
}
bool TrustedScreen::Cancel() noexcept
{
    if (m_screen.State == ScreenState::Locked)
    {
        return Enter(ScreenState::Locked);
    }
    if (m_screen.State != ScreenState::Login)
    {
        return false;
    }
    return Enter(ScreenState::AwaitingAttention);
}
bool TrustedScreen::Logout() noexcept
{
    if (m_screen.State != ScreenState::Locked && m_screen.State != ScreenState::Active)
    {
        return false;
    }
    return Enter(ScreenState::LoggingOut);
}
bool TrustedScreen::SessionDrained(const UInt64 epoch) noexcept
{
    if (epoch != m_screen.Epoch || m_screen.State != ScreenState::LoggingOut)
    {
        return false;
    }
    if (!Enter(ScreenState::AwaitingAttention))
    {
        return false;
    }
    m_screen.Account = 0U;
    m_screen.Revision = 0U;
    m_screen.Session = 0U;
    return true;
}
void TrustedScreen::Fail() noexcept
{
    static_cast<void>(Enter(ScreenState::Recovery));
}
} // Namespace Asteria::User::Identity
