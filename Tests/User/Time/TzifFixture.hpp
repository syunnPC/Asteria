// SPDX-License-Identifier: Apache-2.0
#ifndef ASTERIA_TESTS_USER_TIME_TZIF_FIXTURE_HPP_INCLUDED
#define ASTERIA_TESTS_USER_TIME_TZIF_FIXTURE_HPP_INCLUDED
#include <vector>
#include <string>
#include "User/Time/TimeZone.hpp"
namespace Asteria::Test::TimeFixture
{
using Asteria::Foundation::Int32;
using Asteria::Foundation::Int64;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
using Asteria::Foundation::UInt8;
namespace T = User::Time;
struct Transition
{
    Int64 Time;
    UInt8 Type;
};
struct Type
{
    Int32 Offset;
    UInt8 Dst;
    UInt8 Name;
};
struct Leap
{
    Int64 Time;
    Int32 Correction;
};
struct Block
{
    std::vector<Transition> Transitions{};
    std::vector<Type> Types{{0, 0U, 0U}};
    std::string Names{"UTC\0", 4U};
    std::vector<Leap> Leaps{};
    std::vector<UInt8> Standard{};
    std::vector<UInt8> Ut{};
};
inline void Append(std::vector<UInt8>& bytes, const UInt64 value, const Size width)
{
    for (Size i{width}; i != 0U; --i)
    {
        bytes.push_back(static_cast<UInt8>(value >> ((i - 1U) * 8U)));
    }
}
inline void Put(std::vector<UInt8>& bytes, const Size position, const UInt64 value, const Size width = 4U)
{
    for (Size i{0U}; i < width; ++i)
    {
        bytes[position + i] = static_cast<UInt8>(value >> ((width - i - 1U) * 8U));
    }
}
inline void AppendBlock(std::vector<UInt8>& bytes, const Block& block, const UInt8 version, const Size width)
{
    bytes.insert(bytes.end(), {'T', 'Z', 'i', 'f', version});
    bytes.insert(bytes.end(), 15U, 0U);
    for (const Size count : {block.Ut.size(), block.Standard.size(), block.Leaps.size(), block.Transitions.size(),
                             block.Types.size(), block.Names.size()})
    {
        Append(bytes, count, 4U);
    }
    for (const auto& transition : block.Transitions)
    {
        Append(bytes, static_cast<UInt64>(transition.Time), width);
    }
    for (const auto& transition : block.Transitions)
    {
        bytes.push_back(transition.Type);
    }
    for (const auto& type : block.Types)
    {
        Append(bytes, static_cast<UInt64>(type.Offset), 4U);
        bytes.push_back(type.Dst);
        bytes.push_back(type.Name);
    }
    for (const char byte : block.Names)
    {
        bytes.push_back(static_cast<UInt8>(byte));
    }
    for (const auto& leap : block.Leaps)
    {
        Append(bytes, static_cast<UInt64>(leap.Time), width);
        Append(bytes, static_cast<UInt64>(leap.Correction), 4U);
    }
    bytes.insert(bytes.end(), block.Standard.begin(), block.Standard.end());
    bytes.insert(bytes.end(), block.Ut.begin(), block.Ut.end());
}
[[nodiscard]] inline std::vector<UInt8> File(const Block& block, const UInt8 version = '3',
                                             const std::string& footer = "", const Block& compatibility = {})
{
    std::vector<UInt8> bytes{};
    AppendBlock(bytes, version == 0U ? block : compatibility, version, 4U);
    if (version != 0U)
    {
        AppendBlock(bytes, block, version, 8U);
        bytes.push_back('\n');
        for (const char byte : footer)
        {
            bytes.push_back(static_cast<UInt8>(byte));
        }
        bytes.push_back('\n');
    }
    return bytes;
}
// Deliberately simple reference: count whole years and months from the epoch.
[[nodiscard]] inline bool ReferenceLeap(const Int32 year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
[[nodiscard]] inline UInt32 ReferenceMonth(const Int32 year, const UInt32 month)
{
    constexpr UInt32 lengths[]{31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
    return lengths[month - 1U] + (month == 2U && ReferenceLeap(year) ? 1U : 0U);
}
[[nodiscard]] inline Int64 ReferenceSeconds(const T::Civil& civil)
{
    Int64 days{0};
    for (Int32 year{1970}; year < civil.Year; ++year)
    {
        days += ReferenceLeap(year) ? 366 : 365;
    }
    for (Int32 year{civil.Year}; year < 1970; ++year)
    {
        days -= ReferenceLeap(year) ? 366 : 365;
    }
    for (UInt32 month{1U}; month < civil.Month; ++month)
    {
        days += ReferenceMonth(civil.Year, month);
    }
    days += civil.Day - 1U;
    return days * 86400 + civil.Hour * 3600U + civil.Minute * 60U + civil.Second;
}
[[nodiscard]] inline T::Civil ReferenceCivil(const Int64 seconds)
{
    Int64 days = seconds / 86400;
    Int64 time = seconds % 86400;
    if (time < 0)
    {
        time += 86400;
        --days;
    }
    Int32 year{1970};
    for (; days < 0; --year)
    {
        days += ReferenceLeap(year - 1) ? 366 : 365;
    }
    for (; days >= (ReferenceLeap(year) ? 366 : 365); ++year)
    {
        days -= ReferenceLeap(year) ? 366 : 365;
    }
    UInt32 month{1U};
    for (; days >= ReferenceMonth(year, month); ++month)
    {
        days -= ReferenceMonth(year, month);
    }
    return {year,
            month,
            static_cast<UInt32>(days + 1),
            static_cast<UInt32>(time / 3600),
            static_cast<UInt32>(time % 3600 / 60),
            static_cast<UInt32>(time % 60)};
}
[[nodiscard]] inline UInt32 ReferenceWeekday(const Int64 seconds)
{
    Int64 days = seconds / 86400;
    if (seconds % 86400 < 0)
    {
        --days;
    }
    Int64 day = (days + 4) % 7;
    if (day < 0)
    {
        day += 7;
    }
    return static_cast<UInt32>(day == 0 ? 7 : day);
}
[[nodiscard]] inline T::IsoWeek ReferenceWeek(const T::Civil& civil)
{
    const Int64 seconds = ReferenceSeconds(civil);
    const UInt32 weekday = ReferenceWeekday(seconds);
    const auto thursday = ReferenceCivil(seconds + (4 - static_cast<Int64>(weekday)) * 86400);
    // Count Thursdays beginning with the first Thursday in January.
    const Int64 january = ReferenceSeconds({thursday.Year, 1U, 1U});
    const Int64 firstThursday = january + ((4 + 7 - ReferenceWeekday(january)) % 7U) * 86400;
    const Int64 currentThursday = ReferenceSeconds({thursday.Year, thursday.Month, thursday.Day});
    return {thursday.Year, static_cast<UInt32>((currentThursday - firstThursday) / (7 * 86400) + 1), weekday};
}
} // namespace Asteria::Test::TimeFixture
#endif
