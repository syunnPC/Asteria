// SPDX-License-Identifier: Apache-2.0

#include "User/Session/ClientEvents.hpp"
namespace Asteria::User::Session
{
using Foundation::Size;
using Foundation::UInt64;
using K = SessionEventKind;
namespace
{
[[nodiscard]] bool InputEvent(const K kind) noexcept
{
    return (kind == K::Key) || (kind == K::PointerButtons) || (kind == K::PointerMotion) || (kind == K::Wheel);
}
} // Namespace
void ClientEvents::Reset() noexcept
{
    // Avoid a large aggregate temporary on the target's 512-byte stack.
    m_first = 0U;
    m_count = 0U;
    m_maximum = 0U;
    m_heldCount = 0U;
    m_builtCount = 0U;
    m_builtKeys = 0U;
    m_dropped = 0U;
    m_builtDropped = 0U;
    m_noCreditSince = 0U;
    m_retry = 0U;
    m_failedSends = 0U;
    m_builtMore = false;
    m_credit = false;
    m_notResponding = false;
    m_builtValid = false;
    m_blockedSince = 0U;
    for (auto& state : m_state)
    {
        state.Pending = false;
        state.Version = 0U;
    }
    for (auto& close : m_close)
    {
        close = 0U;
    }
    for (auto& buttons : m_buttons)
    {
        buttons = 0U;
    }
}
Size ClientEvents::StateIndex(const SessionEvent& event) const noexcept
{
    const Size id = static_cast<Size>(event.Words[0U] - 1U);
    switch (event.Kind)
    {
    case K::Welcome:
        return 0U;
    case K::ImageReturned:
        return 1U;
    case K::RequestRefused:
        return 2U;
    case K::PoolReady:
    case K::PoolRefused:
        return 3U + id;
    case K::PoolReleased:
        return 5U + id;
    case K::PoolDestroyed:
        return 7U + id;
    case K::ImageRefused:
        return 9U + id;
    case K::Arrange:
        return 17U + id;
    case K::FrameShown:
        return 21U + id;
    case K::KeyFocus:
        return 25U + id;
    case K::PointerEnter:
    case K::PointerLeave:
        return 29U + id;
    case K::CloseAsked:
        return 33U + id;
    default:
        return ClientStateCapacity;
    }
}
bool ClientEvents::Queue(const SessionEvent& event, const UInt64 now) noexcept
{
    if (!IsSessionEvent(event))
    {
        return false;
    }
    if (!InputEvent(event.Kind))
    {
        State& state = m_state[StateIndex(event)];
        SessionEvent latest = event;
        if (state.Pending && (event.Kind == K::ImageReturned))
        {
            latest.Words[0U] |= state.Event.Words[0U];
        }
        if (state.Pending && (event.Kind == K::RequestRefused))
        {
            latest.Words[2U] = latest.Words[2U] > ~UInt64{0U} - state.Event.Words[2U]
                                   ? ~UInt64{0U}
                                   : latest.Words[2U] + state.Event.Words[2U];
        }
        state.Event = latest;
        ++state.Version;
        state.Pending = true;
        if (event.Kind == K::CloseAsked && m_close[event.Words[0U] - 1U] == 0U)
        {
            m_close[event.Words[0U] - 1U] = now == 0U ? 1U : now;
        }
        return true;
    }
    if ((event.Kind == K::PointerMotion) && (m_count > m_builtKeys))
    {
        SessionEvent& last = m_queue[(m_first + m_count - 1U) & (ClientInputCapacity - 1U)];
        if ((last.Kind == K::PointerMotion) && (last.Words[0U] == event.Words[0U]))
        {
            last = event;
            return true;
        }
    }
    if (m_count == ClientInputCapacity)
    {
        // The frozen answer must settle before overflow releases can be built.
        // Conservatively include its makes: it may have reached the client.
        for (Size i{0U}; i < m_builtCount; ++i)
        {
            Delivered(m_built[i]);
        }
        Overflow(now);
        ++m_dropped;
        return true;
    }
    m_queue[(m_first + m_count) & (ClientInputCapacity - 1U)] = event;
    ++m_count;
    if (!m_credit && (m_noCreditSince == 0U))
    {
        m_noCreditSince = now == 0U ? 1U : now;
    }
    return true;
}
void ClientEvents::Overflow(const UInt64 now) noexcept
{
    m_dropped += m_count - m_builtKeys;
    m_count = m_builtKeys;
    m_notResponding = true;
    for (Size i{0U}; i < m_heldCount; ++i)
    {
        SessionEvent release = m_held[i];
        release.Words[1U] = SessionKeyUp;
        release.Words[3U] = 0U;
        m_queue[(m_first + m_count++) & (ClientInputCapacity - 1U)] = release;
    }
    for (Size i{0U}; i < SessionMaximumSurfaces; ++i)
    {
        if (m_buttons[i] != 0U)
        {
            m_queue[(m_first + m_count++) & (ClientInputCapacity - 1U)] =
                SessionEvent{K::PointerButtons, {i + 1U, 0U, 0U, 0U}};
        }
    }
    m_noCreditSince = now == 0U ? 1U : now;
}
bool ClientEvents::Credit(const Size maximum, const UInt64 now) noexcept
{
    if (m_credit || (maximum == 0U) || (maximum > SessionMaximumEvents))
    {
        return false;
    }
    m_credit = true;
    m_maximum = maximum;
    m_retry = now;
    m_noCreditSince = 0U;
    m_blockedSince = 0U;
    if (m_count == 0U)
    {
        m_notResponding = false;
    }
    return true;
}
Size ClientEvents::Build(SessionEvent* const out, const UInt64 now) noexcept
{
    if (!m_credit || (out == nullptr) || (now < m_retry))
    {
        return 0U;
    }
    if (!m_builtValid)
    {
        m_builtCount = 0U;
        m_builtKeys = 0U;
        m_builtDropped = m_dropped;
        for (Size i{0U}; (i < ClientStateCapacity) && (m_builtCount < m_maximum); ++i)
        {
            if (m_state[i].Pending)
            {
                m_builtIndices[m_builtCount] = i;
                m_builtVersions[m_builtCount] = m_state[i].Version;
                m_built[m_builtCount++] = m_state[i].Event;
            }
        }
        while ((m_builtCount < m_maximum) && (m_builtKeys < m_count))
        {
            m_builtIndices[m_builtCount] = ClientStateCapacity;
            m_built[m_builtCount++] = m_queue[(m_first + m_builtKeys++) & (ClientInputCapacity - 1U)];
        }
        m_builtValid = m_builtCount != 0U;
        m_builtMore = MorePending();
    }
    for (Size i{0U}; i < m_builtCount; ++i)
    {
        out[i] = m_built[i];
    }
    return m_builtCount;
}
void ClientEvents::Delivered(const SessionEvent& event) noexcept
{
    if (event.Kind == K::PointerButtons)
    {
        m_buttons[event.Words[0U] - 1U] = static_cast<Foundation::UInt8>(event.Words[2U]);
    }
    if (event.Kind != K::Key)
    {
        return;
    }
    Size found = m_heldCount;
    for (Size i{0U}; i < m_heldCount; ++i)
    {
        if ((m_held[i].Words[0U] == event.Words[0U]) && (m_held[i].Words[2U] == event.Words[2U]))
        {
            found = i;
            break;
        }
    }
    if ((event.Words[1U] == SessionKeyDown) && (found == m_heldCount) && (m_heldCount < ClientHeldCapacity))
    {
        m_held[m_heldCount++] = event;
    }
    if ((event.Words[1U] == SessionKeyUp) && (found < m_heldCount))
    {
        m_held[found] = m_held[--m_heldCount];
    }
}
void ClientEvents::Commit(const UInt64 now) noexcept
{
    if (!m_builtValid)
    {
        return;
    }
    for (Size i{0U}; i < m_builtCount; ++i)
    {
        const Size index = m_builtIndices[i];
        if (index < ClientStateCapacity)
        {
            State& state = m_state[index];
            if (state.Version == m_builtVersions[i])
            {
                state.Pending = false;
            }
            else if (state.Event.Kind == K::RequestRefused)
            {
                state.Event.Words[2U] -= m_built[i].Words[2U];
                state.Pending = state.Event.Words[2U] != 0U;
            }
            else if (state.Event.Kind == K::ImageReturned)
            {
                state.Event.Words[0U] &= ~m_built[i].Words[0U];
                state.Pending = state.Event.Words[0U] != 0U;
            }
        }
        Delivered(m_built[i]);
    }
    m_first = (m_first + m_builtKeys) & (ClientInputCapacity - 1U);
    m_count -= m_builtKeys;
    m_dropped -= m_builtDropped;
    m_credit = false;
    m_builtValid = false;
    m_builtCount = 0U;
    m_builtKeys = 0U;
    m_noCreditSince = m_count == 0U ? 0U : (now == 0U ? 1U : now);
    m_blockedSince = 0U;
    if (m_count == 0U)
    {
        m_notResponding = false;
    }
}
void ClientEvents::SendFailed(const bool wouldBlock, const UInt64 now) noexcept
{
    if (wouldBlock)
    {
        m_retry = now + ClientRetryUs;
        if (m_blockedSince == 0U)
        {
            m_blockedSince = now == 0U ? 1U : now;
        }
    }
    else
    {
        ++m_failedSends;
        m_credit = false;
        m_builtValid = false;
        m_builtCount = 0U;
        m_builtKeys = 0U;
        m_noCreditSince = now == 0U ? 1U : now;
    }
}
void ClientEvents::Tick(const UInt64 now) noexcept
{
    if (!m_credit && (m_count != 0U) && (now >= m_noCreditSince) && (now - m_noCreditSince >= ClientTimeoutUs))
    {
        m_notResponding = true;
    }
    if (m_credit && m_blockedSince != 0U && now >= m_blockedSince && now - m_blockedSince >= ClientTimeoutUs)
    {
        m_notResponding = true;
    }
    for (const UInt64 close : m_close)
    {
        if ((close != 0U) && (now >= close) && (now - close >= ClientTimeoutUs))
        {
            m_notResponding = true;
        }
    }
}
void ClientEvents::DestroySurface(const UInt64 surface) noexcept
{
    if ((surface > 0U) && (surface <= SessionMaximumSurfaces))
    {
        m_close[surface - 1U] = 0U;
    }
}
void ClientEvents::ReleaseKeys(const UInt64 surface, const UInt64 now) noexcept
{
    // An earlier key keeps its recipient even when a click changes focus in the same drain.
    // Accepted makes precede their releases, just as Input Broker SetFocus does.
    for (Size i{0U}; i < m_builtCount; ++i)
    {
        if (m_built[i].Kind == K::Key)
        {
            Delivered(m_built[i]);
        }
    }
    for (Size i{m_builtKeys}; i < m_count; ++i)
    {
        const SessionEvent event = m_queue[(m_first + i) & (ClientInputCapacity - 1U)];
        if (event.Kind == K::Key && event.Words[0U] == surface)
        {
            Delivered(event);
        }
    }
    if (m_count + m_heldCount > ClientInputCapacity)
    {
        Overflow(now);
        return;
    }
    for (Size i{0U}; i < m_heldCount; ++i)
    {
        if (m_held[i].Words[0U] == surface)
        {
            SessionEvent event = m_held[i];
            event.Words[1U] = SessionKeyUp;
            event.Words[3U] = 0U;
            static_cast<void>(Queue(event, now));
        }
    }
}
bool ClientEvents::MorePending() const noexcept
{
    Size count = m_count;
    for (const auto& state : m_state)
    {
        if (state.Pending)
        {
            ++count;
        }
    }
    return count > m_builtCount;
}
} // Namespace Asteria::User::Session
