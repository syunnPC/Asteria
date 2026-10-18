// SPDX-License-Identifier: Apache-2.0

#include "User/Input/Ps2Controller.hpp"

namespace Asteria::User::Input
{
Ps2Diagnostic Ps2Controller::Describe(const Ps2Step step, const Ps2Reason reason) const noexcept
{
    return Ps2Diagnostic{.Step = step,
                         .Reason = reason,
                         .DataValid = m_dataValid,
                         .Command = m_command,
                         .Status = m_status,
                         .Data = m_data};
}

bool Ps2Controller::Fail(const Ps2Reason reason) noexcept
{
    m_failure = Describe(m_step, reason);
    return false;
}

UInt64 Ps2Controller::Now() const noexcept
{
    return ((m_transport.Clock != nullptr) && (m_waitTicks != 0U)) ? m_transport.Clock(m_transport.Context) : 0U;
}

// The poll floor keeps the previous bound when no clock exists; the clock
// stretches it for a slow controller, and the poll ceiling bounds a clock
// that stops. Unsigned subtraction tolerates one counter wrap per wait.
bool Ps2Controller::Expired(const Size poll, const UInt64 start) const noexcept
{
    if (poll < m_polls)
    {
        return false;
    }
    if ((m_transport.Clock == nullptr) || (m_waitTicks == 0U) || ((poll / MaximumPollFactor) >= m_polls))
    {
        return true;
    }
    return (Now() - start) >= m_waitTicks;
}

bool Ps2Controller::ReadStatus(UInt8& status) noexcept
{
    if (!m_transport.Read(m_transport.Context, true, status))
    {
        return false;
    }
    m_status = status;
    return true;
}

bool Ps2Controller::ReadData(UInt8& value) noexcept
{
    if (!m_transport.Read(m_transport.Context, false, value))
    {
        return false;
    }
    m_data = value;
    m_dataValid = true;
    return true;
}

// Expiry is decided before the status read, so the last read of a timed-out
// wait happened after its bound even if this thread was descheduled.
bool Ps2Controller::Write(const bool command, const UInt8 value) noexcept
{
    const UInt64 start = Now();
    for (Size poll{0U};; ++poll)
    {
        const bool expired = Expired(poll, start);
        UInt8 status{0U};
        if (!ReadStatus(status))
        {
            return Fail(Ps2Reason::Transport);
        }
        if (status == 0xFFU)
        {
            return Fail(Ps2Reason::Unexpected);
        }
        if ((status & 2U) == 0U)
        {
            m_command = value;
            return m_transport.Write(m_transport.Context, command, value) || Fail(Ps2Reason::Transport);
        }
        if (expired)
        {
            return Fail(Ps2Reason::Timeout);
        }
    }
}

bool Ps2Controller::Command(const Ps2Step step, const UInt8 value) noexcept
{
    m_step = step;
    return Write(true, value);
}

bool Ps2Controller::Drain() noexcept
{
    for (Size poll{0U}; poll < m_polls; ++poll)
    {
        UInt8 status{0U};
        UInt8 value{0U};
        if (!ReadStatus(status))
        {
            return Fail(Ps2Reason::Transport);
        }
        if (status == 0xFFU)
        {
            return Fail(Ps2Reason::Unexpected);
        }
        if ((status & 1U) == 0U)
        {
            return true;
        }
        if (!ReadData(value) || (((status & 0xC0U) == 0U) && !m_transport.Observe(m_transport.Context, status, value)))
        {
            return Fail(Ps2Reason::Transport);
        }
    }
    return Fail(Ps2Reason::Timeout);
}

bool Ps2Controller::Quiesce() noexcept
{
    if (!Command(Ps2Step::DisableKeyboard, 0xADU) || !Command(Ps2Step::DisableAuxiliary, 0xA7U))
    {
        return false;
    }
    m_step = Ps2Step::Flush;
    return Drain();
}
}
