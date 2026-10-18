// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_COMPONENT_POINTER_ROUTE_HPP_INCLUDED
#define ASTERIA_USER_COMPONENT_POINTER_ROUTE_HPP_INCLUDED

#include "User/Component/DisplayFrame.hpp"
#include "User/Component/InputFrame.hpp"
#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Component
{
/// Bounded, allocation-free route queue. Caller checks CanQueue before accepting
/// a batch and queues only once, after Publish commits. Source ids are broker
/// assigned 1..InputMaximumSources (three since the fifth component slot: a
/// mouse on the third source moves the cursor too). Button snapshots combine
/// sources; sequence never wraps.
/// Keep in static storage and serialize all calls with the input broker drain.
class PointerRoute final
{
public:
    static constexpr Foundation::Size Capacity{64U};
    void Configure(Foundation::UInt64 epoch, Foundation::UInt64 sequence) noexcept
    {
        m_epoch = epoch;
        m_sequence = sequence;
        m_first = 0U;
        m_count = 0U;
    }
    [[nodiscard]] bool CanQueue(Foundation::Size count) const noexcept
    {
        return (m_epoch == 0U) || ((count <= Capacity - m_count) && (count <= ~Foundation::UInt64{0U} - m_sequence));
    }
    [[nodiscard]] Foundation::UInt64 Sequence() const noexcept
    {
        return m_sequence;
    }
    // Retire queued pointers at the trusted boundary, keeping the global route serial monotonic.
    void Discard() noexcept
    {
        m_first = 0U;
        m_count = 0U;
        for (auto& buttons : m_buttons)
        {
            buttons = 0U;
        }
    }
    [[nodiscard]] bool Enabled() const noexcept
    {
        return m_epoch != 0U;
    }
    void Queue(const Input::InputEvent& event, Foundation::UInt32 source, Foundation::UInt64 stamp = 0U) noexcept
    {
        if ((source < 1U) || (source > InputMaximumSources))
        {
            return;
        }
        if (event.Kind == Input::EventKind::Buttons)
        {
            m_buttons[source - 1U] = event.Buttons;
        }
        if ((m_epoch == 0U) || (event.Kind == Input::EventKind::KeyDown) || (event.Kind == Input::EventKind::KeyUp) ||
            !CanQueue(1U))
        {
            return;
        }
        Foundation::UInt64* const words = m_queue[(m_first + m_count) % Capacity];
        words[0U] = m_epoch;
        words[1U] = ++m_sequence;
        words[2U] = event.Kind == Input::EventKind::Motion ? DisplayRelativeMotion : DisplayPointerSnapshot;
        words[3U] = static_cast<Foundation::UInt32>(event.Dx) |
                    (static_cast<Foundation::UInt64>(static_cast<Foundation::UInt32>(event.Dy)) << 32U);
        Foundation::UInt8 buttons{0U};
        for (const Foundation::UInt8 held : m_buttons)
        {
            buttons = static_cast<Foundation::UInt8>(buttons | held);
        }
        words[4U] = buttons;
        words[5U] = source;
        words[6U] = static_cast<Foundation::UInt32>(event.Wheel);
        words[7U] = stamp; // Never rebuilt at send time.
        ++m_count;
    }
    [[nodiscard]] const Foundation::UInt64* Peek() const noexcept
    {
        return m_count == 0U ? nullptr : m_queue[m_first];
    }
    void Take() noexcept
    {
        if (m_count != 0U)
        {
            m_first = (m_first + 1U) % Capacity;
            --m_count;
        }
    }

private:
    Foundation::UInt64 m_queue[Capacity][8U]{};
    Foundation::Size m_first{0U};
    Foundation::Size m_count{0U};
    Foundation::UInt64 m_epoch{0U};
    Foundation::UInt64 m_sequence{0U};
    Foundation::UInt8 m_buttons[InputMaximumSources]{};
};
} // Namespace Asteria::User::Component
#endif // ASTERIA_USER_COMPONENT_POINTER_ROUTE_HPP_INCLUDED
