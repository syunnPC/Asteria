// SPDX-License-Identifier: Apache-2.0

#include "User/Time/Civil.hpp"
#include "User/Time/Calendar.hpp"

namespace Asteria::User::Time
{
// Calendar conversions use POSIX seconds and UTF-8 strings.
bool IsLeapYear(const Int32 year) noexcept
{
    return Detail::Leap(year);
}
UInt32 DaysInMonth(const Int32 year, const UInt32 month) noexcept
{
    constexpr UInt32 lengths[]{31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    if (month == 0U || month > 12U)
    {
        return 0U;
    }
    return lengths[month - 1U] + (month == 2U && IsLeapYear(year) ? 1U : 0U);
}
bool IsValid(const Civil& civil) noexcept
{
    return civil.Year >= MinimumYear && civil.Year <= MaximumYear && civil.Day >= 1U &&
           civil.Day <= DaysInMonth(civil.Year, civil.Month) && civil.Hour < 24U && civil.Minute < 60U &&
           civil.Second < 60U;
}
Status ToSeconds(const Civil& civil, Int64& seconds) noexcept
{
    if (!IsValid(civil))
    {
        return Status::Invalid;
    }
    seconds = Detail::Days(civil.Year, civil.Month, civil.Day) * SecondsPerDay + static_cast<Int64>(civil.Hour) * 3600 +
              static_cast<Int64>(civil.Minute) * 60 + civil.Second;
    return Status::Ok;
}
Status FromSeconds(const Int64 seconds, Civil& civil) noexcept
{
    if (seconds < Detail::MinimumSeconds || seconds > Detail::MaximumSeconds)
    {
        return Status::OutOfRange;
    }
    const Int64 days = Detail::FloorDivide(seconds, SecondsPerDay);
    const Int64 year = Detail::Year(days);
    const Int64 ordinal = days - Detail::Days(year, 1U, 1U);
    UInt32 month{1U};
    UInt32 day = static_cast<UInt32>(ordinal);
    for (; month < 12U; ++month)
    {
        const UInt32 count = DaysInMonth(static_cast<Int32>(year), month);
        if (day < count)
        {
            break;
        }
        day -= count;
    }
    const Int64 within = seconds - days * SecondsPerDay;
    civil = {static_cast<Int32>(year),
             month,
             day + 1U,
             static_cast<UInt32>(within / 3600),
             static_cast<UInt32>(within % 3600 / 60),
             static_cast<UInt32>(within % 60)};
    return Status::Ok;
}
Status AddDays(const Civil& civil, const Int64 days, Civil& result) noexcept
{
    Int64 seconds{0};
    if (ToSeconds(civil, seconds) != Status::Ok)
    {
        return Status::Invalid;
    }
    if (days < (Detail::MinimumSeconds - seconds) / SecondsPerDay ||
        days > (Detail::MaximumSeconds - seconds) / SecondsPerDay)
    {
        return Status::OutOfRange;
    }
    return FromSeconds(seconds + days * SecondsPerDay, result);
}
UInt32 DayOfWeek(const Civil& civil) noexcept
{
    return IsValid(civil) ? Detail::Weekday(Detail::Days(civil.Year, civil.Month, civil.Day)) : 0U;
}
Status GetIsoWeek(const Civil& civil, IsoWeek& week) noexcept
{
    if (!IsValid(civil))
    {
        return Status::Invalid;
    }
    const Int64 days = Detail::Days(civil.Year, civil.Month, civil.Day);
    const UInt32 weekday = Detail::Weekday(days);
    const Int64 thursday = days + 4 - weekday;
    const Int64 year = Detail::Year(thursday);
    const Int64 january4 = Detail::Days(year, 1U, 4U);
    const Int64 firstMonday = january4 + 1 - Detail::Weekday(january4);
    week = {static_cast<Int32>(year), static_cast<UInt32>((days - firstMonday) / 7 + 1), weekday};
    return Status::Ok;
}
namespace
{
void Pair(char* const output, const UInt32 value) noexcept
{
    output[0U] = static_cast<char>('0' + value / 10U);
    output[1U] = static_cast<char>('0' + value % 10U);
}
void WriteDate(char* const output, const Civil& civil) noexcept
{
    Pair(output, static_cast<UInt32>(civil.Year) / 100U);
    Pair(output + 2U, static_cast<UInt32>(civil.Year) % 100U);
    output[4U] = '-';
    Pair(output + 5U, civil.Month);
    output[7U] = '-';
    Pair(output + 8U, civil.Day);
}
}
Status FormatIsoDate(const Civil& civil, char* const buffer, const Size capacity, Size& length) noexcept
{
    if (!IsValid(civil))
    {
        return Status::Invalid;
    }
    if (civil.Year < 0 || civil.Year > 9999)
    {
        return Status::OutOfRange;
    }
    if (buffer == nullptr || capacity <= IsoDateLength)
    {
        return Status::BufferTooSmall;
    }
    WriteDate(buffer, civil);
    buffer[IsoDateLength] = '\0';
    length = IsoDateLength;
    return Status::Ok;
}
Status FormatIsoWeekDate(const Civil& civil, char* const buffer, const Size capacity, Size& length) noexcept
{
    IsoWeek week{};
    const Status status = GetIsoWeek(civil, week);
    if (status != Status::Ok)
    {
        return status;
    }
    if (week.Year < 0 || week.Year > 9999)
    {
        return Status::OutOfRange;
    }
    if (buffer == nullptr || capacity <= IsoWeekDateLength)
    {
        return Status::BufferTooSmall;
    }
    Pair(buffer, static_cast<UInt32>(week.Year) / 100U);
    Pair(buffer + 2U, static_cast<UInt32>(week.Year) % 100U);
    buffer[4U] = '-';
    buffer[5U] = 'W';
    Pair(buffer + 6U, week.Week);
    buffer[8U] = '-';
    buffer[9U] = static_cast<char>('0' + week.Weekday);
    buffer[IsoWeekDateLength] = '\0';
    length = IsoWeekDateLength;
    return Status::Ok;
}
Status FormatIso8601(const Civil& civil, const Int32 offsetSeconds, char* const buffer, const Size capacity,
                     Size& length) noexcept
{
    if (!IsValid(civil))
    {
        return Status::Invalid;
    }
    if (civil.Year < 0 || civil.Year > 9999 || offsetSeconds <= -SecondsPerDay || offsetSeconds >= SecondsPerDay)
    {
        return Status::OutOfRange;
    }
    const UInt32 magnitude = static_cast<UInt32>(offsetSeconds < 0 ? -offsetSeconds : offsetSeconds);
    const Size needed = offsetSeconds == 0 ? 20U : (magnitude % 60U == 0U ? 25U : MaximumIso8601Length);
    if (buffer == nullptr || capacity <= needed)
    {
        return Status::BufferTooSmall;
    }
    WriteDate(buffer, civil);
    buffer[10U] = 'T';
    Pair(buffer + 11U, civil.Hour);
    buffer[13U] = ':';
    Pair(buffer + 14U, civil.Minute);
    buffer[16U] = ':';
    Pair(buffer + 17U, civil.Second);
    if (offsetSeconds == 0)
    {
        buffer[19U] = 'Z';
    }
    else
    {
        buffer[19U] = offsetSeconds < 0 ? '-' : '+';
        Pair(buffer + 20U, magnitude / 3600U);
        buffer[22U] = ':';
        Pair(buffer + 23U, magnitude % 3600U / 60U);
        if (needed == MaximumIso8601Length)
        {
            buffer[25U] = ':';
            Pair(buffer + 26U, magnitude % 60U);
        }
    }
    buffer[needed] = '\0';
    length = needed;
    return Status::Ok;
}
} // namespace Asteria::User::Time
