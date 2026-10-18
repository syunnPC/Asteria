// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TIME_CALENDAR_HPP_INCLUDED
#define ASTERIA_USER_TIME_CALENDAR_HPP_INCLUDED
#include "User/Time/Civil.hpp"

namespace Asteria::User::Time::Detail
{
// Derived from the Gregorian cycle: 400*365 + 97 leap days. March-based
// months put the leap day at the end of each year; epoch adjustment counts
// the days from the start of that cycle to 1970-01-01.
inline constexpr Int64 DaysPerCycle{146097};
inline constexpr Int64 EpochAdjustment{719468};
static_assert(DaysPerCycle == 400 * 365 + 97);
[[nodiscard]] constexpr Int64 FloorDivide(const Int64 value, const Int64 divisor) noexcept
{
    return value / divisor - (value % divisor < 0 ? 1 : 0);
}
[[nodiscard]] constexpr bool Leap(const Int64 year) noexcept
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
[[nodiscard]] constexpr Int64 Days(const Int64 year, const UInt32 month, const UInt32 day) noexcept
{
    const Int64 adjusted = year - (month <= 2U ? 1 : 0);
    const Int64 era = FloorDivide(adjusted, 400);
    const Int64 inEra = adjusted - era * 400;
    const Int64 marchMonth = static_cast<Int64>(month) + (month > 2U ? -3 : 9);
    return era * DaysPerCycle + inEra * 365 + inEra / 4 - inEra / 100 + (153 * marchMonth + 2) / 5 +
           static_cast<Int64>(day) - 1 - EpochAdjustment;
}
[[nodiscard]] constexpr Int64 Year(const Int64 days) noexcept
{
    const Int64 shifted = days + EpochAdjustment;
    const Int64 era = FloorDivide(shifted, DaysPerCycle);
    const Int64 day = shifted - era * DaysPerCycle;
    const Int64 year = (day - day / 1460 + day / 36524 - day / 146096) / 365;
    const Int64 dayInYear = day - (365 * year + year / 4 - year / 100);
    const Int64 month = (5 * dayInYear + 2) / 153;
    return era * 400 + year + (month >= 10 ? 1 : 0);
}
[[nodiscard]] constexpr UInt32 Weekday(const Int64 days) noexcept
{
    const Int64 remainder = (days + 3) % 7;
    return static_cast<UInt32>(remainder < 0 ? remainder + 8 : remainder + 1);
}
inline constexpr Int64 MinimumSeconds{Days(MinimumYear, 1U, 1U) * SecondsPerDay};
inline constexpr Int64 MaximumSeconds{Days(MaximumYear + 1, 1U, 1U) * SecondsPerDay - 1};
} // namespace Asteria::User::Time::Detail
#endif
