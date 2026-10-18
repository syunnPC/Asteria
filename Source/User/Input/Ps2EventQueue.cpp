// SPDX-License-Identifier: Apache-2.0

#include "User/Input/Ps2EventQueue.hpp"

namespace Asteria::User::Input
{
bool Ps2EventQueue::Append(const InputEvent& event) noexcept
{
    if ((m_count == 64U) || !Component::IsInputEvent(event))
    {
        return false;
    }
    m_events[m_count++] = event;
    return true;
}
Size Ps2EventQueue::Prepare() noexcept
{
    if (m_pending == 0U)
    {
        m_pending = (m_count < Component::InputMaximumEvents) ? m_count : Component::InputMaximumEvents;
    }
    return m_pending;
}
bool Ps2EventQueue::Answer(const Component::InputAnswer answer) noexcept
{
    switch (answer)
    {
    case Component::InputAnswer::Busy:
    case Component::InputAnswer::Exhausted:
        return true;
    case Component::InputAnswer::Restarted:
        Restart();
        return true;
    case Component::InputAnswer::Refused:
        return false;
    case Component::InputAnswer::Done:
        if ((m_pending == 0U) || (m_transaction == (~Foundation::UInt64{0U} - 1U)))
        {
            return false;
        }
        m_count -= m_pending;
        for (Size index{0U}; index < m_count; ++index)
        {
            m_events[index] = m_events[index + m_pending];
        }
        m_pending = 0U;
        ++m_transaction;
        return true;
    }
    return false;
}
void Ps2EventQueue::Restart() noexcept
{
    m_count = 0U;
    m_pending = 0U;
    // Keep transactions increasing even across broker instances.
}
}
