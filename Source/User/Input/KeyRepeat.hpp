// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_INPUT_KEY_REPEAT_HPP_INCLUDED
#define ASTERIA_USER_INPUT_KEY_REPEAT_HPP_INCLUDED

#include "User/Input/InputEvent.hpp"

namespace Asteria::User::Input
{
/// Client-side repeat, serialized by its owner. Time is unsigned microseconds;
/// successive observations must be less than 2^63 microseconds apart. Arithmetic
/// intentionally wraps. No allocation, blocking, hardware access or callbacks.
/// A delivery emits at most eight repeats; older missed repeats are discarded.
class KeyRepeat final
{
public:
    static constexpr Foundation::UInt64 DefaultDelayMicroseconds{500'000U};
    static constexpr Foundation::UInt32 DefaultRatePerSecond{30U};
    static constexpr Foundation::UInt64 MaximumDelivery{8U};
    static constexpr Size MaximumSources{4U};

    /// Delay is clamped to 1..2^63-1; rate to 1..1000. The period rounds up
    /// to whole microseconds so delivery never exceeds the requested rate.
    explicit constexpr KeyRepeat(Foundation::UInt64 delayMicroseconds = DefaultDelayMicroseconds,
                                 Foundation::UInt32 ratePerSecond = DefaultRatePerSecond) noexcept
        : m_delay{ClampDelay(delayMicroseconds)},
          m_period{(1'000'000U + ClampRate(ratePerSecond) - 1U) / ClampRate(ratePerSecond)}
    {
    }

    /// Up to MaximumSources keyboards; excess source tags are ignored until
    /// SourceGone/Reset frees a slot. Key events carry each source's modifier
    /// snapshot; repeat combines held modifiers across those sources. Another
    /// repeatable make takes over; duplicate makes do not restart the delay. Pointer events do
    /// not change modifiers. Modifier and lock usages never take over.
    void Feed(const InputEvent& event, Foundation::UInt64 nowMicroseconds) noexcept;
    /// Stop after source disappearance, even when no KeyUp was delivered.
    void SourceGone(UInt32 source) noexcept;
    /// Stop and discard modifiers after focus loss or a source restart.
    void Reset() noexcept;
    /// Returns 0..MaximumDelivery identical KeyDown events, with current
    /// modifiers, through one caller-owned event. Zero leaves event untouched.
    [[nodiscard]] Foundation::UInt64 TakeDue(Foundation::UInt64 nowMicroseconds, InputEvent& event) noexcept;
    /// The owner listens for input again at now after being busy (drawing, for
    /// example). A repeat that fell due meanwhile waits until now + settle, so a
    /// release queued while the owner was busy arrives before any repeat.
    void Settle(Foundation::UInt64 nowMicroseconds, Foundation::UInt64 settleMicroseconds) noexcept;
    /// Time to next delivery, capped by maximumMicroseconds; zero means due.
    [[nodiscard]] Foundation::UInt64 WaitMicroseconds(Foundation::UInt64 nowMicroseconds,
                                                      Foundation::UInt64 maximumMicroseconds) const noexcept;
    [[nodiscard]] static constexpr bool CanRepeat(const UInt16 usage) noexcept
    {
        return (usage >= 4U) && (usage < 0xE0U) && (usage != 0x39U) && (usage != 0x47U) && (usage != 0x53U) &&
               (usage != 0x82U) && (usage != 0x83U) && (usage != 0x84U);
    }

private:
    static constexpr Foundation::UInt64 HalfRange{Foundation::UInt64{1U} << 63U};
    [[nodiscard]] static constexpr Foundation::UInt32 ClampRate(const Foundation::UInt32 rate) noexcept
    {
        if (rate == 0U)
        {
            return 1U;
        }
        return rate > 1000U ? 1000U : rate;
    }
    [[nodiscard]] static constexpr Foundation::UInt64 ClampDelay(const Foundation::UInt64 delay) noexcept
    {
        if (delay == 0U)
        {
            return 1U;
        }
        return delay > HalfRange - 1U ? HalfRange - 1U : delay;
    }
    Foundation::UInt64 m_delay;
    Foundation::UInt64 m_period;
    Foundation::UInt64 m_next{0U};
    UInt32 m_source{0U};
    UInt16 m_usage{0U};
    struct SourceModifiers
    {
        UInt32 Source{0U};
        UInt8 Modifiers{0U};
        bool Present{false};
    };
    SourceModifiers m_sources[MaximumSources]{};
};
} // namespace Asteria::User::Input

#endif // ASTERIA_USER_INPUT_KEY_REPEAT_HPP_INCLUDED
