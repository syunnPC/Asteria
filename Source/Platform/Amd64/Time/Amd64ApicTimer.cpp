// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::Time
{
Foundation::UInt64 Amd64ApicTimer::GetTicks() const noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    Foundation::UInt32 low{0U};
    Foundation::UInt32 high{0U};
    __asm__ __volatile__("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return (static_cast<Foundation::UInt64>(high) << 32U) | low;
#else
    return 1U;
#endif
}

Foundation::UInt32 Amd64ApicTimer::Read(const Foundation::UInt32 offset) const noexcept
{
    return m_apic.Read(offset);
}

void Amd64ApicTimer::Write(const Foundation::UInt32 offset, const Foundation::UInt32 value) noexcept
{
    m_apic.Write(offset, value);
}

bool Amd64ApicTimer::Initialize(const Foundation::UInt64 mappedApicAddress) noexcept
{
    if (!m_apic.Initialize(mappedApicAddress))
    {
        return false;
    }
    Write(0x80U, 0U);
    Write(0xF0U, 0x100U | ApicSpuriousVector);
    Write(ApicLvtTimerRegister, ApicTimerMaskedLvt);
    MaskUnusedInterrupts(m_apic);
    Write(0x3E0U, 3U); // Divide by sixteen.
    const Foundation::UInt64 start = GetTicks();
    Write(ApicInitialCountRegister, 0xFFFF'FFFFU);
    Foundation::UInt64 elapsed{0U};
    for (Foundation::Size attempt{0U}; attempt < 10'000'000U; ++attempt)
    {
        const Foundation::UInt64 now = GetTicks();
        if (now < start)
        {
            break;
        }
        elapsed = now - start;
        if (elapsed >= 10'000'000U)
        {
            break;
        }
    }
    const Foundation::UInt64 counts = 0xFFFF'FFFFU - Read(0x390U);
    Write(ApicInitialCountRegister, 0U);
    if ((elapsed < 10'000'000U) || (counts == 0U))
    {
        return false;
    }
    m_ticksPerCount = elapsed / counts + (elapsed % counts != 0U ? 1U : 0U);
    return m_ticksPerCount != 0U;
}

Hal::Time::LocalTimerError Amd64ApicTimer::ProgramDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (m_ticksPerCount == 0U)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if (deadlineTicks == 0U)
    {
        return Hal::Time::LocalTimerError::InvalidDeadline;
    }
    const Foundation::UInt64 now = GetTicks();
    if (deadlineTicks <= now)
    {
        return Hal::Time::LocalTimerError::DeadlineReached;
    }

    m_deadline = deadlineTicks;
    Rearm(now);
    return Hal::Time::LocalTimerError::None;
}

Hal::Time::LocalTimerError Amd64ApicTimer::CancelDeadline() noexcept
{
    if (m_ticksPerCount == 0U)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    Write(ApicLvtTimerRegister, ApicTimerMaskedLvt);
    Write(ApicInitialCountRegister, 0U);
    m_deadline = 0U;
    Rearm(GetTicks());
    return Hal::Time::LocalTimerError::None;
}

Hal::Time::LocalTimerError
Amd64ApicTimer::AcknowledgeExternallyMaskedDeadline(const Foundation::UInt64 deadlineTicks) noexcept
{
    if (m_ticksPerCount == 0U)
    {
        return Hal::Time::LocalTimerError::NotReady;
    }
    if ((deadlineTicks == 0U) || (deadlineTicks != m_deadline))
    {
        return Hal::Time::LocalTimerError::InvalidDeadline;
    }
    m_deadline = 0U;
    Rearm(GetTicks());
    return Hal::Time::LocalTimerError::None;
}

void Amd64ApicTimer::EndOfInterrupt() noexcept
{
    Write(0xB0U, 0U);
}

bool Amd64ApicTimer::AcknowledgeInterrupt() noexcept
{
    Write(ApicLvtTimerRegister, ApicTimerMaskedLvt);
    Write(ApicInitialCountRegister, 0U);
    Write(0xB0U, 0U);
    const auto now = GetTicks();
    if (m_watchdogInterval != 0U && now >= m_watchdogWake)
    {
        m_watchdogWake = now + m_watchdogInterval;
    }
    const bool due = m_deadline != 0U && now >= m_deadline;
    if (!due)
    {
        Rearm(now);
    }
    return due;
}
void Amd64ApicTimer::EnableWatchdog(const Foundation::UInt64 interval) noexcept
{
    m_watchdogInterval = interval;
    m_watchdogWake = interval == 0U ? 0U : GetTicks() + interval;
    Rearm(GetTicks());
}
void Amd64ApicTimer::Rearm(const Foundation::UInt64 now) noexcept
{
    auto deadline = m_deadline;
    if (m_watchdogInterval != 0U)
    {
        if (now >= m_watchdogWake)
        {
            m_watchdogWake = now + m_watchdogInterval;
        }
        if (deadline == 0U || m_watchdogWake < deadline)
        {
            deadline = m_watchdogWake;
        }
    }
    if (deadline != 0U)
    {
        Write(ApicLvtTimerRegister, ApicTimerVector);
        Write(ApicInitialCountRegister, CountdownFor(now, deadline, m_ticksPerCount));
    }
}
} // namespace Asteria::Platform::Amd64::Time
