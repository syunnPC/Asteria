// SPDX-License-Identifier: Apache-2.0

#include "User/Time/Posix.hpp"
#include "User/Time/Calendar.hpp"

namespace Asteria::User::Time::Detail
{
namespace
{
// POSIX Issue 7 (2018), Base Definitions §8.3 TZ; RFC 8536 §3.3.1.
class Parser final
{
public:
    Parser(const char* const data, const Size length, const bool version3) noexcept
        : m_data{data}, m_length{length}, m_version3{version3}
    {
    }
    [[nodiscard]] bool Done() const noexcept
    {
        return m_position == m_length;
    }
    [[nodiscard]] bool Take(const char value) noexcept
    {
        if (m_position < m_length && m_data[m_position] == value)
        {
            ++m_position;
            return true;
        }
        return false;
    }
    [[nodiscard]] bool Number(UInt32& value, const UInt32 maximum) noexcept
    {
        const Size start = m_position;
        UInt32 number{0U};
        for (; m_position < m_length; ++m_position)
        {
            const char digit = m_data[m_position];
            if (digit < '0' || digit > '9')
            {
                break;
            }
            const UInt32 unit = static_cast<UInt32>(digit - '0');
            if (unit > maximum || number > (maximum - unit) / 10U)
            {
                return false;
            }
            number = number * 10U + unit;
        }
        value = number;
        return m_position != start;
    }
    [[nodiscard]] bool Name(TextView& name) noexcept
    {
        const bool quoted = Take('<');
        const Size start = m_position;
        for (; m_position < m_length; ++m_position)
        {
            const char value = m_data[m_position];
            const bool letter = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
            if (!letter && !(quoted && ((value >= '0' && value <= '9') || value == '+' || value == '-')))
            {
                break;
            }
        }
        name = {m_data + start, m_position - start};
        return name.Length >= 3U && (!quoted || Take('>'));
    }
    [[nodiscard]] bool Clock(Int32& seconds, const bool transition) noexcept
    {
        bool negative{false};
        if (!transition || m_version3)
        {
            negative = Take('-');
            if (!negative)
            {
                static_cast<void>(Take('+'));
            }
        }
        UInt32 hours{0U};
        UInt32 minutes{0U};
        UInt32 units{0U};
        if (!Number(hours, transition && m_version3 ? 167U : 24U))
        {
            return false;
        }
        if (Take(':'))
        {
            if (!Number(minutes, 59U))
            {
                return false;
            }
            if (Take(':') && !Number(units, 59U))
            {
                return false;
            }
        }
        const Int32 magnitude = static_cast<Int32>(hours * 3600U + minutes * 60U + units);
        seconds = negative ? -magnitude : magnitude;
        return true;
    }
    [[nodiscard]] bool Transition(Rule& rule) noexcept
    {
        if (Take('M'))
        {
            rule.Kind = 'M';
            if (!Number(rule.Month, 12U) || rule.Month == 0U || !Take('.') || !Number(rule.Week, 5U) ||
                rule.Week == 0U || !Take('.') || !Number(rule.Day, 6U))
            {
                return false;
            }
        }
        else
        {
            rule.Kind = Take('J') ? 'J' : 'N';
            if (!Number(rule.Day, 365U) || (rule.Kind == 'J' && rule.Day == 0U))
            {
                return false;
            }
        }
        return !Take('/') || Clock(rule.Seconds, true);
    }

private:
    const char* m_data;
    Size m_length;
    Size m_position{0U};
    bool m_version3;
};
[[nodiscard]] Int64 RuleDay(const Rule& rule, const Int64 year) noexcept
{
    const Int64 january = Days(year, 1U, 1U);
    if (rule.Kind == 'N')
    {
        return january + rule.Day;
    }
    if (rule.Kind == 'J')
    {
        return january + rule.Day - 1 + (Leap(year) && rule.Day >= 60U ? 1 : 0);
    }
    const Int64 first = Days(year, rule.Month, 1U);
    const UInt32 firstSunday = Weekday(first) % 7U;
    UInt32 day = 1U + (rule.Day + 7U - firstSunday) % 7U + (rule.Week - 1U) * 7U;
    // Year narrowed only modulo the calendar's 400-year cycle.
    const UInt32 length = DaysInMonth(static_cast<Int32>(year % 400), rule.Month);
    if (day > length)
    {
        day -= 7U;
    }
    return first + day - 1;
}
} // namespace
bool ParseFooter(const char* const data, const Size length, const bool version3, Footer& footer) noexcept
{
    footer = {};
    if (length == 0U)
    {
        return true;
    }
    Parser parser{data, length, version3};
    Int32 offset{0};
    if (!parser.Name(footer.Standard.Designation) || !parser.Clock(offset, false))
    {
        return false;
    }
    footer.Standard.OffsetSeconds = -offset;
    footer.Present = true;
    if (parser.Done())
    {
        return true;
    }
    if (!parser.Name(footer.Daylight.Designation))
    {
        return false;
    }
    footer.HasDaylight = true;
    footer.Daylight.Daylight = true;
    footer.Daylight.OffsetSeconds = footer.Standard.OffsetSeconds + 3600;
    if (!parser.Take(','))
    {
        if (!parser.Clock(offset, false) || !parser.Take(','))
        {
            return false;
        }
        footer.Daylight.OffsetSeconds = -offset;
    }
    return parser.Transition(footer.Start) && parser.Take(',') && parser.Transition(footer.End) && parser.Done();
}
LocalType FooterType(const Footer& footer, const Int64 utc) noexcept
{
    if (!footer.HasDaylight)
    {
        return footer.Standard;
    }
    const Int64 utcDay = FloorDivide(utc, SecondsPerDay);
    // Avoid utcDay*86400 at INT64_MIN: use a nonnegative remainder directly.
    const Int64 remainder = utc % SecondsPerDay;
    const Int64 within = remainder < 0 ? remainder + SecondsPerDay : remainder;
    const Int64 year = Year(utcDay);
    Int64 latest{-9223372036854775807LL};
    bool daylight{false};
    // Two prior rule years are necessary when both late-December events
    // spill into January and neither event from the previous rule year has
    // happened yet. The next year covers negative January transition times.
    for (Int64 adjacent{-2}; adjacent <= 1; ++adjacent)
    {
        const Int64 start = (RuleDay(footer.Start, year + adjacent) - utcDay) * SecondsPerDay + footer.Start.Seconds -
                            footer.Standard.OffsetSeconds;
        const Int64 end = (RuleDay(footer.End, year + adjacent) - utcDay) * SecondsPerDay + footer.End.Seconds -
                          footer.Daylight.OffsetSeconds;
        if (end <= within && end > latest)
        {
            latest = end;
            daylight = false;
        }
        // Start wins simultaneous boundaries, including v3 perpetual DST.
        if (start <= within && start >= latest)
        {
            latest = start;
            daylight = true;
        }
    }
    return daylight ? footer.Daylight : footer.Standard;
}
} // namespace Asteria::User::Time::Detail
