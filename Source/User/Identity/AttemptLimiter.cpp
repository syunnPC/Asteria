// SPDX-License-Identifier: Apache-2.0

#include "User/Identity/Identity.hpp"
namespace Asteria::User::Identity
{
bool AttemptLimiter::Now(UInt64& time) noexcept
{
    UInt64 now{0U};
    if (m_clock.Read == nullptr || !m_clock.Read(m_clock.Context, now) || (m_state.ClockSeen && now < m_state.LastTime))
    {
        return false;
    }
    m_state.LastTime = now;
    m_state.ClockSeen = true;
    time = now;
    return true;
}
Bucket* AttemptLimiter::Find(PrincipalId account, bool create) noexcept
{
    if (account == 0U)
    {
        return &m_state.Unknown;
    }
    Bucket* free{nullptr};
    for (auto& bucket : m_state.Accounts)
    {
        if (bucket.Id == account)
        {
            return &bucket;
        }
        if (bucket.Id == 0U)
        {
            free = &bucket;
        }
    }
    if (!create || free == nullptr)
    {
        return nullptr;
    }
    free->Id = account;
    return free;
}
Status AttemptLimiter::Begin(PrincipalId account, UInt64& serial, UInt64& retryAt) noexcept
{
    if (account != 0U && account != Administrator && account < FirstCustomId)
    {
        return Status::Invalid;
    }
    UInt64 now{0U};
    if (!Now(now))
    {
        return Status::Unavailable;
    }
    if (m_state.Exhausted || m_state.NextSerial == MaximumCounter || m_state.NextSerial == 0U)
    {
        return Status::Exhausted;
    }
    if (m_state.InFlight)
    {
        return Status::Busy;
    }
    auto* bucket = Find(account, false);
    if (bucket != nullptr && bucket->Failures != 0U && now >= bucket->LastFailure &&
        now - bucket->LastFailure >= QuietSeconds * TimeTicksPerSecond)
    {
        bucket->Failures = 0U;
        bucket->Deadline = 0U;
    }
    const UInt64 deadline =
        (bucket != nullptr && bucket->Deadline > m_state.GlobalDeadline) ? bucket->Deadline : m_state.GlobalDeadline;
    if (now < deadline)
    {
        retryAt = deadline;
        return Status::RetryAt;
    }
    bucket = Find(account, true);
    if (bucket == nullptr)
    {
        return Status::Full;
    }
    m_state.InFlight = true;
    m_state.InFlightAccount = account;
    m_state.Serial = m_state.NextSerial++;
    serial = m_state.Serial;
    return Status::Ok;
}
Status AttemptLimiter::Complete(UInt64 serial, bool success, bool cancelled) noexcept
{
    if (!m_state.InFlight || serial == 0U || serial != m_state.Serial)
    {
        return Status::Invalid;
    }
    UInt64 now{0U};
    if (!Now(now))
    {
        return Status::Unavailable;
    }
    auto* bucket = Find(m_state.InFlightAccount, false);
    if (bucket == nullptr)
    {
        return Status::Corrupt;
    }
    UInt64 delay{0U};
    if (success && !cancelled)
    {
        bucket->Failures = 0U;
        bucket->Deadline = 0U;
    }
    else
    {
        if (bucket->Failures != MaximumCounter)
        {
            ++bucket->Failures;
        }
        bucket->LastFailure = now;
        delay =
            bucket->Failures <= 4U ? (UInt64{1U} << (bucket->Failures - 1U)) : (bucket->Failures < 10U ? 30U : 300U);
        delay *= TimeTicksPerSecond;
        if (now > MaximumCounter - delay)
        {
            m_state.Exhausted = true;
            bucket->Deadline = MaximumCounter;
        }
        else
        {
            bucket->Deadline = now + delay;
        }
    }
    if (now > MaximumCounter - GlobalDelaySeconds * TimeTicksPerSecond)
    {
        m_state.Exhausted = true;
        m_state.GlobalDeadline = MaximumCounter;
    }
    else
    {
        m_state.GlobalDeadline = now + GlobalDelaySeconds * TimeTicksPerSecond;
    }
    m_state.InFlight = false;
    m_state.Serial = 0U;
    m_state.InFlightAccount = 0U;
    return m_state.Exhausted ? Status::Exhausted : Status::Ok;
}
Status AttemptLimiter::Recover() noexcept
{
    if (!m_state.InFlight)
    {
        UInt64 now{0U};
        return Now(now) ? Status::Ok : Status::Unavailable;
    }
    return Complete(m_state.Serial, false, true);
}
} // namespace Asteria::User::Identity
