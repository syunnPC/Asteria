// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_INPUT_BROKER_LOGIC_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_INPUT_BROKER_LOGIC_HPP_INCLUDED

#include "User/Component/InputFrame.hpp"

namespace Asteria::User::Component
{
/// Single-threaded bounded state machine. Caller serializes operations.
/// Publish and SetFocus are atomic on refusal; Take acknowledges a successful
/// event send. Keep this object in component/caller-owned static storage.
/// Queued events count as seen: focus-loss releases follow them in order.
class InputBrokerLogic final
{
public:
    static constexpr Foundation::Size MaximumSources{InputMaximumSources};
    static constexpr Foundation::Size MaximumClients{InputMaximumClients};
    static constexpr Foundation::Size QueueCapacity{64U};
    static constexpr Foundation::Size MaximumHeld{32U};

    [[nodiscard]] Foundation::UInt32 RegisterSource() noexcept;
    /// Registers source again for a restarted driver : every key it holds at a client is released
    /// there (a zero-modifier KeyUp, as a focus change releases), followed by a held-button
    /// release. The caller reserves pointer-route space first. Its pressed, suppressed and deferred
    /// state is forgotten, so the new instance's first make is not a repeat. Atomic: Exhausted,
    /// changing nothing, when a client's queue cannot take the releases; Refused for an
    /// unregistered source.
    [[nodiscard]] InputAnswer ResetSource(Foundation::UInt32 source, Foundation::UInt64 routeSequence = 0U,
                                          bool routed = false) noexcept;
    [[nodiscard]] Foundation::UInt8 SourceButtons(Foundation::UInt32 source) const noexcept
    {
        return source >= 1U && source <= m_state.SourceCount ? m_state.Sources[source - 1U].Buttons : 0U;
    }
    [[nodiscard]] Foundation::UInt8 Buttons() const noexcept
    {
        Foundation::UInt8 buttons{0U};
        for (const auto& source : m_state.Sources)
        {
            buttons |= source.Buttons;
        }
        return buttons;
    }
    [[nodiscard]] Foundation::UInt64 Delivered(Foundation::UInt32 client) const noexcept
    {
        return m_state.Clients[client - 1U].Sequence - m_state.Clients[client - 1U].Count;
    }
    [[nodiscard]] InputAnswer RebindClient(Foundation::UInt32 client) noexcept;
    [[nodiscard]] Foundation::UInt32 RegisterClient(bool brokerPointer = false) noexcept;
    /// A revoked consumer cannot retain focus or fill the key queue and obstruct the source's
    /// physical mouse batches. Its registration stays retired until the seat incarnation restarts.
    void DetachClient(Foundation::UInt32 client) noexcept;
    [[nodiscard]] Foundation::UInt64 Dropped() const noexcept
    {
        return m_state.Dropped;
    }
    [[nodiscard]] InputAnswer Publish(Foundation::UInt32 source, const Input::InputEvent* events,
                                      Foundation::Size count, Foundation::UInt64 routeSequence = 0U,
                                      bool routed = false) noexcept;
    /// Stamps are captured at each pointer in the accepted Publish, including deferred makes
    /// flushed earlier in it.
    [[nodiscard]] Foundation::UInt64 PointerStamp(Foundation::Size index) const noexcept
    {
        return m_stamps[index];
    }
    [[nodiscard]] Foundation::UInt64 Horizon(Foundation::UInt32 client) const noexcept;
    [[nodiscard]] bool BrokerPointer(Foundation::UInt32 client) const noexcept;
    void SetRouteSequence(Foundation::UInt64 sequence) noexcept
    {
        m_state.RouteSequence = sequence;
    }
    [[nodiscard]] InputAnswer SetFocus(Foundation::UInt32 client, bool lock = false) noexcept;
    [[nodiscard]] InputAnswer SetManagerFocus(Foundation::UInt32 client) noexcept
    {
        return m_state.FocusLocked ? InputAnswer::Refused : SetFocus(client);
    }
    [[nodiscard]] Foundation::UInt32 Focus() const noexcept;
    [[nodiscard]] bool FocusLocked() const noexcept
    {
        return m_state.FocusLocked;
    }
    [[nodiscard]] Foundation::Size Queued(Foundation::UInt32 client) const noexcept;
    /// Events is writable through count; returns at most the queued count.
    [[nodiscard]] Foundation::Size Peek(Foundation::UInt32 client, Input::InputEvent* events,
                                        Foundation::Size count) const noexcept;
    void Take(Foundation::UInt32 client, Foundation::Size count) noexcept;
    [[nodiscard]] Foundation::UInt32 AttentionSource() const noexcept;
    void TakeAttention() noexcept;
    [[nodiscard]] Foundation::UInt16 AttentionUsage() const noexcept;

private:
    static constexpr Foundation::Size UsageCount{256U};
    struct KeyBits
    {
        Foundation::UInt64 Words[UsageCount / 64U]{};
        [[nodiscard]] bool Has(Foundation::UInt16 usage) const noexcept
        {
            return (Words[usage / 64U] & (1ULL << (usage % 64U))) != 0U;
        }
        void Set(Foundation::UInt16 usage, bool down) noexcept
        {
            const auto bit = 1ULL << (usage % 64U);
            if (down)
            {
                Words[usage / 64U] |= bit;
            }
            else
            {
                Words[usage / 64U] &= ~bit;
            }
        }
    };
    static_assert(UsageCount % 64U == 0U);
    struct Source
    {
        KeyBits Pressed{};
        KeyBits Suppressed{};
        Input::InputEvent Pending[4U]{};
        Foundation::Size PendingCount{0U};
        Foundation::UInt8 Modifiers{0U};
        Foundation::UInt8 Buttons{0U};
    };
    // Omit wire-reserved/irrelevant fields in retained entries, leaving room for the 64-bit horizon
    // without growing the old queue.
    struct Entry
    {
        Foundation::UInt64 Horizon;
        Foundation::UInt32 Source;
        Foundation::UInt16 Usage;
        Foundation::UInt8 Modifiers;
        Input::EventKind Kind;
        Foundation::UInt32 A;
        Foundation::UInt32 B;
    };
    static_assert(sizeof(Entry) == sizeof(Input::InputEvent));
    [[nodiscard]] static Entry Pack(const Input::InputEvent& event, Foundation::UInt64 horizon) noexcept;
    [[nodiscard]] static Input::InputEvent Unpack(const Entry& entry) noexcept;
    struct Held
    {
        Foundation::UInt32 Source;
        Foundation::UInt16 Usage;
    };
    static_assert(sizeof(Held) == 8U);
    struct Client
    {
        Entry Queue[QueueCapacity]{};
        Held HeldKeys[MaximumHeld]{};
        Foundation::UInt64 Sequence{0U};
        Foundation::Size Count{0U};
        Foundation::Size HeldCount{0U};
        bool Detached{false};
    };
    struct State
    {
        Source Sources[MaximumSources]{};
        Client Clients[MaximumClients]{};
        Foundation::UInt32 SourceCount{0U};
        Foundation::UInt32 ClientCount{0U};
        Foundation::UInt32 Focus{0U};
        Foundation::UInt32 BrokerClient{0U};
        Foundation::UInt64 Attention[8U]{};
        Foundation::Size AttentionCount{0U};
        Foundation::UInt64 RouteSequence{0U};
        Foundation::UInt64 Dropped{0U};
        bool FocusLocked{false};
    };
    [[nodiscard]] bool Deliver(Input::InputEvent event) noexcept;
    [[nodiscard]] bool FlushModifiers() noexcept;
    [[nodiscard]] bool Event(Foundation::UInt32 source, Input::InputEvent event) noexcept;
    State m_state{};
    State m_next{};
    Foundation::UInt64 m_stamps[InputMaximumEvents]{};
};
} // Namespace Asteria::User::Component
#endif // ASTERIA_USER_COMPONENT_INPUT_BROKER_LOGIC_HPP_INCLUDED
