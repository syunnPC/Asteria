// SPDX-License-Identifier: Apache-2.0

#include "User/Input/KeyRepeat.hpp"

namespace Asteria::User::Input
{
void KeyRepeat::Feed(const InputEvent& event, const Foundation::UInt64 nowMicroseconds) noexcept
{
    if ((event.Kind != EventKind::KeyDown) && (event.Kind != EventKind::KeyUp))
    {
        return;
    }
    SourceModifiers* source{nullptr};
    for (auto& candidate : m_sources)
    {
        if (candidate.Present && (candidate.Source == event.Source))
        {
            source = &candidate;
            break;
        }
    }
    if (source == nullptr)
    {
        for (auto& candidate : m_sources)
        {
            if (!candidate.Present)
            {
                source = &candidate;
                break;
            }
        }
    }
    if (source == nullptr)
    {
        return;
    }
    *source = {.Source = event.Source, .Modifiers = event.Modifiers, .Present = true};
    if (event.Kind == EventKind::KeyUp)
    {
        if ((event.Source == m_source) && (event.Usage == m_usage))
        {
            m_usage = 0U;
        }
        return;
    }
    if (!CanRepeat(event.Usage) || ((event.Source == m_source) && (event.Usage == m_usage)))
    {
        return;
    }
    m_source = event.Source;
    m_usage = event.Usage;
    m_next = nowMicroseconds + m_delay;
}

void KeyRepeat::SourceGone(const UInt32 source) noexcept
{
    if (source == m_source)
    {
        m_usage = 0U;
    }
    for (auto& candidate : m_sources)
    {
        if (candidate.Present && (candidate.Source == source))
        {
            candidate = {};
        }
    }
}

void KeyRepeat::Reset() noexcept
{
    m_usage = 0U;
    for (auto& source : m_sources)
    {
        source = {};
    }
}

void KeyRepeat::Settle(const Foundation::UInt64 nowMicroseconds, const Foundation::UInt64 settleMicroseconds) noexcept
{
    if ((m_usage != 0U) && (nowMicroseconds - m_next < HalfRange))
    {
        m_next = nowMicroseconds + (settleMicroseconds < HalfRange ? settleMicroseconds : HalfRange - 1U);
    }
}

Foundation::UInt64 KeyRepeat::WaitMicroseconds(const Foundation::UInt64 nowMicroseconds,
                                               const Foundation::UInt64 maximumMicroseconds) const noexcept
{
    if (m_usage == 0U)
    {
        return maximumMicroseconds;
    }
    if (nowMicroseconds - m_next < HalfRange)
    {
        return 0U;
    }
    const Foundation::UInt64 remaining = m_next - nowMicroseconds;
    return remaining < maximumMicroseconds ? remaining : maximumMicroseconds;
}

Foundation::UInt64 KeyRepeat::TakeDue(const Foundation::UInt64 nowMicroseconds, InputEvent& event) noexcept
{
    if ((m_usage == 0U) || (nowMicroseconds - m_next >= HalfRange))
    {
        return 0U;
    }
    const Foundation::UInt64 late = nowMicroseconds - m_next;
    const Foundation::UInt64 due = 1U + late / m_period;
    m_next = nowMicroseconds + m_period - late % m_period;
    UInt8 modifiers{0U};
    for (const auto& source : m_sources)
    {
        modifiers = static_cast<UInt8>(modifiers | source.Modifiers);
    }
    event = InputEvent{.Kind = EventKind::KeyDown,
                       .Modifiers = modifiers,
                       .Usage = m_usage,
                       .Source = m_source,
                       .Dx = 0,
                       .Dy = 0,
                       .Buttons = 0U,
                       .Reserved = {},
                       .Wheel = 0};
    return due < MaximumDelivery ? due : MaximumDelivery;
}
} // namespace Asteria::User::Input
