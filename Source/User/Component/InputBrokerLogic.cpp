// SPDX-License-Identifier: Apache-2.0

#include "User/Component/InputBrokerLogic.hpp"

namespace Asteria::User::Component
{
namespace
{
[[nodiscard]] constexpr bool IsAttentionModifier(Foundation::UInt16 usage) noexcept
{
    return (usage == 0xE0U) || (usage == 0xE2U) || (usage == 0xE4U) || (usage == 0xE6U);
}
} // Namespace

InputBrokerLogic::Entry InputBrokerLogic::Pack(const Input::InputEvent& event,
                                               const Foundation::UInt64 horizon) noexcept
{
    // InputFrame validates the canonical event: at most one of dx, wheel and buttons is nonzero.
    return {horizon,
            event.Source,
            event.Usage,
            event.Modifiers,
            event.Kind,
            static_cast<Foundation::UInt32>(event.Dx) | static_cast<Foundation::UInt32>(event.Wheel) | event.Buttons,
            static_cast<Foundation::UInt32>(event.Dy)};
}
Input::InputEvent InputBrokerLogic::Unpack(const Entry& entry) noexcept
{
    return {.Kind = entry.Kind,
            .Modifiers = entry.Modifiers,
            .Usage = entry.Usage,
            .Source = entry.Source,
            .Dx = entry.Kind == Input::EventKind::Motion ? Detail::InputSigned(entry.A) : 0,
            .Dy = entry.Kind == Input::EventKind::Motion ? Detail::InputSigned(entry.B) : 0,
            .Buttons = entry.Kind == Input::EventKind::Buttons ? static_cast<Foundation::UInt8>(entry.A)
                                                               : Foundation::UInt8{0U},
            .Reserved = {},
            .Wheel = entry.Kind == Input::EventKind::Wheel ? Detail::InputSigned(entry.A) : 0};
}

Foundation::UInt32 InputBrokerLogic::RegisterSource() noexcept
{
    return m_state.SourceCount < MaximumSources ? ++m_state.SourceCount : 0U;
}
Foundation::UInt32 InputBrokerLogic::RegisterClient(const bool brokerPointer) noexcept
{
    if (m_state.ClientCount == MaximumClients || (brokerPointer && m_state.BrokerClient != 0U))
    {
        return 0U;
    }
    // One seat/session face carries one key sequence.
    const auto client = ++m_state.ClientCount;
    if (brokerPointer)
    {
        m_state.BrokerClient = client;
    }
    return client;
}
void InputBrokerLogic::DetachClient(const Foundation::UInt32 client) noexcept
{
    if (client == 0U || client > m_state.ClientCount)
    {
        return;
    }
    auto& lost = m_state.Clients[client - 1U];
    m_state.Dropped += lost.Count;
    for (Foundation::Size i{0U}; i < lost.HeldCount; ++i)
    {
        const auto& held = lost.HeldKeys[i];
        m_state.Sources[held.Source - 1U].Suppressed.Set(held.Usage, true);
    }
    lost.Count = 0U;
    lost.HeldCount = 0U;
    lost.Detached = true;
    if (m_state.BrokerClient == client)
    {
        m_state.BrokerClient = 0U;
    }
    if (m_state.Focus != client)
    {
        return;
    }
    // No synthetic releases are sent to a revoked receiver. Discard deferred
    // modifiers as on focus loss, so they cannot leak into a later consumer.
    for (auto& source : m_state.Sources)
    {
        for (Foundation::Size i{0U}; i < source.PendingCount; ++i)
        {
            source.Suppressed.Set(source.Pending[i].Usage, true);
        }
        m_state.Dropped += source.PendingCount;
        source.PendingCount = 0U;
    }
    m_state.Focus = 0U;
    m_state.FocusLocked = false;
}
bool InputBrokerLogic::BrokerPointer(const Foundation::UInt32 client) const noexcept
{
    return client != 0U && client == m_state.BrokerClient;
}
Foundation::UInt64 InputBrokerLogic::Horizon(const Foundation::UInt32 client) const noexcept
{
    return Queued(client) == 0U ? 0U : m_state.Clients[client - 1U].Queue[0U].Horizon;
}
Foundation::UInt32 InputBrokerLogic::Focus() const noexcept
{
    return m_state.Focus;
}
Foundation::Size InputBrokerLogic::Queued(Foundation::UInt32 client) const noexcept
{
    return ((client >= 1U) && (client <= m_state.ClientCount)) ? m_state.Clients[client - 1U].Count : 0U;
}
Foundation::Size InputBrokerLogic::Peek(Foundation::UInt32 client, Input::InputEvent* events,
                                        Foundation::Size count) const noexcept
{
    const Foundation::Size queued = Queued(client);
    count = (count < queued) ? count : queued;
    if (count == 0U)
    {
        return 0U;
    }
    const auto& destination = m_state.Clients[client - 1U];
    const auto horizon = destination.Queue[0U].Horizon;
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (destination.Queue[index].Horizon != horizon)
        {
            return index;
        }
        events[index] = Unpack(destination.Queue[index]);
    }
    return count;
}
void InputBrokerLogic::Take(Foundation::UInt32 client, Foundation::Size count) noexcept
{
    if ((client == 0U) || (client > m_state.ClientCount) || (count > Queued(client)))
    {
        return;
    }
    Client& destination = m_state.Clients[client - 1U];
    for (Foundation::Size index{count}; index < destination.Count; ++index)
    {
        destination.Queue[index - count] = destination.Queue[index];
    }
    destination.Count -= count;
}
Foundation::UInt32 InputBrokerLogic::AttentionSource() const noexcept
{
    return m_state.AttentionCount == 0U ? 0U : static_cast<Foundation::UInt32>(m_state.Attention[0U]);
}
Foundation::UInt16 InputBrokerLogic::AttentionUsage() const noexcept
{
    return m_state.AttentionCount == 0U ? 0U : static_cast<Foundation::UInt16>(m_state.Attention[0U] >> 32U);
}
void InputBrokerLogic::TakeAttention() noexcept
{
    if (m_state.AttentionCount != 0U)
    {
        --m_state.AttentionCount;
        for (Foundation::Size index{0U}; index < m_state.AttentionCount; ++index)
        {
            m_state.Attention[index] = m_state.Attention[index + 1U];
        }
    }
}

bool InputBrokerLogic::Deliver(Input::InputEvent event) noexcept
{
    if (m_next.Focus == 0U)
    {
        return true;
    }
    Client& client = m_next.Clients[m_next.Focus - 1U];
    const bool key = event.Kind == Input::EventKind::KeyDown || event.Kind == Input::EventKind::KeyUp;
    if (!key && m_next.Focus == m_next.BrokerClient)
    {
        return true;
    }
    if (client.Count == QueueCapacity || client.Sequence == ~Foundation::UInt64{0U})
    {
        return false;
    }
    if (event.Kind == Input::EventKind::KeyDown)
    {
        if (client.HeldCount == MaximumHeld)
        {
            return false;
        }
        client.HeldKeys[client.HeldCount++] = {event.Source, event.Usage};
    }
    else if (event.Kind == Input::EventKind::KeyUp)
    {
        Foundation::Size held{0U};
        while ((held < client.HeldCount) &&
               ((client.HeldKeys[held].Usage != event.Usage) || (client.HeldKeys[held].Source != event.Source)))
        {
            ++held;
        }
        if (held == client.HeldCount)
        {
            return true;
        }
        client.HeldKeys[held] = client.HeldKeys[--client.HeldCount];
    }
    // Horizon applies to the session's key-only stream. The console still receives its original
    // mixed batches.
    client.Queue[client.Count++] = Pack(event, m_next.Focus == m_next.BrokerClient ? m_next.RouteSequence : 0U);
    ++client.Sequence;
    return true;
}

bool InputBrokerLogic::FlushModifiers() noexcept
{
    for (Foundation::UInt32 index{0U}; index < m_next.SourceCount; ++index)
    {
        Source& source = m_next.Sources[index];
        for (Foundation::Size pending{0U}; pending < source.PendingCount; ++pending)
        {
            if (!Deliver(source.Pending[pending]))
            {
                return false;
            }
        }
        source.PendingCount = 0U;
    }
    return true;
}

bool InputBrokerLogic::Event(Foundation::UInt32 id, Input::InputEvent event) noexcept
{
    event.Source = id;
    Source& source = m_next.Sources[id - 1U];
    const bool key = (event.Kind == Input::EventKind::KeyDown) || (event.Kind == Input::EventKind::KeyUp);
    if (!key)
    {
        if (event.Kind == Input::EventKind::Buttons)
        {
            source.Buttons = event.Buttons;
        }
        // Retain physical state while excluding session and login pointer delivery throughout the
        // trusted interval.
        return m_next.FocusLocked || Deliver(event);
    }
    source.Modifiers = event.Modifiers;
    const bool down = event.Kind == Input::EventKind::KeyDown;
    if (source.Pressed.Has(event.Usage) == down)
    {
        return true; // No broker repeat; unmatched releases never reach a client.
    }
    source.Pressed.Set(event.Usage, down);
    if (source.Suppressed.Has(event.Usage))
    {
        if (!down)
        {
            source.Suppressed.Set(event.Usage, false);
        }
        return true;
    }
    Foundation::UInt8 modifiers{0U};
    Foundation::UInt8 attentionModifiers{0U};
    for (Foundation::UInt32 index{0U}; index < m_next.SourceCount; ++index)
    {
        const Source& physical = m_next.Sources[index];
        attentionModifiers = static_cast<Foundation::UInt8>(attentionModifiers | physical.Modifiers);
        Foundation::UInt8 suppressed{0U};
        for (Foundation::UInt16 usage{0xE0U}; usage <= 0xE7U; ++usage)
        {
            if (physical.Suppressed.Has(usage))
            {
                suppressed = static_cast<Foundation::UInt8>(suppressed | (1U << (usage - 0xE0U)));
            }
        }
        modifiers = static_cast<Foundation::UInt8>(modifiers |
                                                   (physical.Modifiers & static_cast<Foundation::UInt8>(~suppressed)));
        for (Foundation::UInt16 usage{0xE0U}; usage <= 0xE7U; ++usage)
        {
            if (physical.Pressed.Has(usage))
            {
                attentionModifiers = static_cast<Foundation::UInt8>(attentionModifiers | (1U << (usage - 0xE0U)));
            }
            if (physical.Pressed.Has(usage) && !physical.Suppressed.Has(usage))
            {
                modifiers = static_cast<Foundation::UInt8>(modifiers | (1U << (usage - 0xE0U)));
            }
        }
    }
    // Shutdown stays root-owned in GUI mode. Console registrations retain their existing
    // InputTerminal chord path.
    const bool rootPower = m_next.BrokerClient != 0U && event.Usage >= 66U && event.Usage <= 68U;
    if (down && ((event.Usage == 0x4CU) || rootPower) && ((attentionModifiers & 0x11U) != 0U) &&
        ((attentionModifiers & 0x44U) != 0U))
    {
        if (m_next.AttentionCount == 8U)
        {
            return false;
        }
        m_next.Attention[m_next.AttentionCount++] =
            id | (rootPower ? static_cast<Foundation::UInt64>(event.Usage) << 32U : 0U);
        source.Suppressed.Set(event.Usage, true);
        for (Source& chordSource : m_next.Sources)
        {
            for (Foundation::Size index{0U}; index < chordSource.PendingCount; ++index)
            {
                chordSource.Suppressed.Set(chordSource.Pending[index].Usage, true);
            }
            chordSource.PendingCount = 0U;
        }
        return true;
    }
    if (down && IsAttentionModifier(event.Usage))
    {
        source.Pending[source.PendingCount++] = event;
        return true;
    }
    if (!FlushModifiers())
    {
        return false;
    }
    // A consumed chord cannot leak its modifier snapshot through later keys.
    event.Modifiers = modifiers;
    return Deliver(event);
}

InputAnswer InputBrokerLogic::Publish(Foundation::UInt32 source, const Input::InputEvent* events,
                                      Foundation::Size count, const Foundation::UInt64 routeSequence,
                                      const bool routed) noexcept
{
    if ((source == 0U) || (source > m_state.SourceCount) || (events == nullptr) || (count == 0U) ||
        (count > InputMaximumEvents))
    {
        return InputAnswer::Refused;
    }
    m_next = m_state;
    m_next.RouteSequence = routeSequence;
    for (Foundation::Size index{0U}; index < count; ++index)
    {
        if (!IsInputEvent(events[index]))
        {
            return InputAnswer::Refused;
        }
        m_stamps[index] = 0U;
        const bool pointer =
            events[index].Kind != Input::EventKind::KeyDown && events[index].Kind != Input::EventKind::KeyUp;
        if (pointer && routed)
        {
            if (m_next.RouteSequence == ~Foundation::UInt64{0U})
            {
                return InputAnswer::Exhausted;
            }
            if (m_next.BrokerClient != 0U)
            {
                m_stamps[index] = m_next.Clients[m_next.BrokerClient - 1U].Sequence;
            }
            ++m_next.RouteSequence;
        }
        if (!Event(source, events[index]))
        {
            return InputAnswer::Exhausted;
        }
    }
    m_state = m_next;
    return InputAnswer::Done;
}
InputAnswer InputBrokerLogic::ResetSource(const Foundation::UInt32 source, const Foundation::UInt64 routeSequence,
                                          const bool routed) noexcept
{
    if ((source == 0U) || (source > m_state.SourceCount))
    {
        return InputAnswer::Refused;
    }
    m_next = m_state;
    m_next.RouteSequence = routeSequence;
    // Its deferred modifier makes never reached a client; each key it still
    // holds is released as the device would have released it (which also
    // ends a suppression), so a client that saw the make gets a
    // zero-modifier break and one that did not gets nothing.
    Source& reset = m_next.Sources[source - 1U];
    reset.PendingCount = 0U;
    Input::InputEvent release{};
    release.Kind = Input::EventKind::KeyUp;
    for (Foundation::UInt16 usage{0U}; usage < UsageCount; ++usage)
    {
        release.Usage = usage;
        if (reset.Pressed.Has(usage) && !Event(source, release))
        {
            return InputAnswer::Exhausted;
        }
    }
    reset.Modifiers = 0U;
    // Source retirement balances pointer state after its keys, in
    // the same accepted order.
    if (reset.Buttons != 0U)
    {
        release = {};
        release.Kind = Input::EventKind::Buttons;
        if (!Event(source, release))
        {
            return InputAnswer::Exhausted;
        }
        m_stamps[0U] = m_next.BrokerClient == 0U ? 0U : m_next.Clients[m_next.BrokerClient - 1U].Sequence;
        if (routed)
        {
            if (m_next.RouteSequence == ~Foundation::UInt64{0U})
            {
                return InputAnswer::Exhausted;
            }
            ++m_next.RouteSequence;
        }
    }
    m_state = m_next;
    return InputAnswer::Done;
}
InputAnswer InputBrokerLogic::RebindClient(const Foundation::UInt32 client) noexcept
{
    if (!BrokerPointer(client))
    {
        return InputAnswer::Refused;
    }
    auto& destination = m_state.Clients[client - 1U];
    // Keeps pending keys. A make delivered to the old consumer grants no held-key state to its
    // replacement. Only retained makes do.
    for (Foundation::Size held{0U}; held < destination.HeldCount;)
    {
        const auto key = destination.HeldKeys[held];
        bool queued{false};
        for (Foundation::Size i{0U}; i < destination.Count; ++i)
        {
            const auto& event = destination.Queue[i];
            queued = queued || (event.Source == key.Source && event.Usage == key.Usage &&
                                event.Kind == Input::EventKind::KeyDown);
        }
        if (queued)
        {
            ++held;
            continue;
        }
        m_state.Sources[key.Source - 1U].Suppressed.Set(key.Usage, true);
        destination.HeldKeys[held] = destination.HeldKeys[--destination.HeldCount];
    }
    return InputAnswer::Done;
}
InputAnswer InputBrokerLogic::SetFocus(Foundation::UInt32 client, const bool lock) noexcept
{
    if (client > m_state.ClientCount || (client != 0U && m_state.Clients[client - 1U].Detached))
    {
        return InputAnswer::Refused;
    }
    if (client == m_state.Focus && (!lock || client == 1U))
    {
        m_state.FocusLocked = lock;
        return InputAnswer::Done;
    }
    m_next = m_state;
    if (m_next.Focus != 0U)
    {
        Client& previous = m_next.Clients[m_next.Focus - 1U];
        if (previous.Count + previous.HeldCount > QueueCapacity ||
            previous.HeldCount > ~Foundation::UInt64{0U} - previous.Sequence)
        {
            return InputAnswer::Exhausted;
        }
        for (Foundation::Size index{0U}; index < previous.HeldCount; ++index)
        {
            const auto& held = previous.HeldKeys[index];
            previous.Queue[previous.Count++] = {m_next.Focus == m_next.BrokerClient ? m_next.RouteSequence : 0U,
                                                held.Source,
                                                held.Usage,
                                                0U,
                                                Input::EventKind::KeyUp,
                                                0U,
                                                0U};
            ++previous.Sequence;
        }
        previous.HeldCount = 0U;
    }
    for (Source& source : m_next.Sources)
    {
        // Every physical make held across focus is suppressed until its
        // release, including modifiers already delivered to the old owner.
        source.Suppressed = source.Pressed;
        for (Foundation::Size index{0U}; index < source.PendingCount; ++index)
        {
            source.Suppressed.Set(source.Pending[index].Usage, true);
        }
        source.PendingCount = 0U;
    }
    m_next.Focus = client;
    m_next.FocusLocked = lock;
    if (lock && client > 1U)
    {
        // A reused login client cannot consume queued keys from an old screen
        // epoch. Sequence remains monotonic; no old credit becomes a new epoch.
        m_next.Clients[client - 1U].Count = 0U;
        m_next.Clients[client - 1U].HeldCount = 0U;
    }
    m_state = m_next;
    return InputAnswer::Done;
}
} // Namespace Asteria::User::Component
