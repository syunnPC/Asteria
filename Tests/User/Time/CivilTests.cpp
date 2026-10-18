// SPDX-License-Identifier: Apache-2.0
#include "User/Time/Civil.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"
#include "User/Time/TzifFixture.hpp"
#include <cstring>
#include <limits>
namespace Asteria::Test
{
namespace
{
namespace T = User::Time;
namespace F = TimeFixture;
using Asteria::Foundation::Int32;
using Asteria::Foundation::Int64;
using Asteria::Foundation::Size;
using Asteria::Foundation::UInt32;
using Asteria::Foundation::UInt64;
void Boundaries(TestSupport::TestContext& test)
{
    ASTERIA_EXPECT(test, "Gregorian century rule and astronomical year zero", T::IsLeapYear(0), T::IsLeapYear(2000),
                   !T::IsLeapYear(1900), T::IsLeapYear(-400), !T::IsLeapYear(-100));
    Int64 seconds{123};
    T::Civil civil{};
    ASTERIA_EXPECT(test, "invalid dates and leap seconds fail without writing output",
                   T::ToSeconds({2023, 2U, 29U}, seconds) == T::Status::Invalid,
                   T::ToSeconds({2024, 0U, 1U}, seconds) == T::Status::Invalid,
                   T::ToSeconds({2024, 13U, 1U}, seconds) == T::Status::Invalid,
                   T::ToSeconds({2024, 1U, 1U, 24U}, seconds) == T::Status::Invalid,
                   T::ToSeconds({2024, 1U, 1U, 0U, 60U}, seconds) == T::Status::Invalid,
                   T::ToSeconds({2024, 1U, 1U, 0U, 0U, 60U}, seconds) == T::Status::Invalid, seconds == 123);
    ASTERIA_EXPECT(test, "negative epoch uses floor division", T::FromSeconds(-1, civil) == T::Status::Ok,
                   (civil == T::Civil{1969, 12U, 31U, 23U, 59U, 59U}), T::DayOfWeek(civil) == 3U);
    for (const T::Civil boundary :
         {T::Civil{T::MinimumYear, 1U, 1U}, T::Civil{T::MaximumYear, 12U, 31U, 23U, 59U, 59U}})
    {
        ASTERIA_EXPECT(test, "civil bounds round trip", T::ToSeconds(boundary, seconds) == T::Status::Ok,
                       T::FromSeconds(seconds, civil) == T::Status::Ok, civil == boundary);
    }
    ASTERIA_EXPECT(test, "full integer endpoints rejected",
                   T::FromSeconds(std::numeric_limits<Int64>::min(), civil) == T::Status::OutOfRange,
                   T::FromSeconds(std::numeric_limits<Int64>::max(), civil) == T::Status::OutOfRange);
    ASTERIA_EXPECT(test, "day arithmetic crosses leap and century boundaries",
                   T::AddDays({2000, 2U, 28U, 12U}, 2, civil) == T::Status::Ok, (civil == T::Civil{2000, 3U, 1U, 12U}),
                   T::AddDays({1900, 3U, 1U}, -1, civil) == T::Status::Ok, (civil == T::Civil{1900, 2U, 28U}),
                   T::AddDays({0, 1U, 1U}, -1, civil) == T::Status::Ok, (civil == T::Civil{-1, 12U, 31U}));
    ASTERIA_EXPECT(test, "extreme day addition is checked before multiplication",
                   T::AddDays({}, std::numeric_limits<Int64>::min(), civil) == T::Status::OutOfRange,
                   T::AddDays({}, std::numeric_limits<Int64>::max(), civil) == T::Status::OutOfRange,
                   T::AddDays({T::MinimumYear, 1U, 1U}, -1, civil) == T::Status::OutOfRange,
                   T::AddDays({T::MaximumYear, 12U, 31U}, 1, civil) == T::Status::OutOfRange);
    T::IsoWeek week{};
    ASTERIA_EXPECT(test, "ISO week crossing year boundary", T::GetIsoWeek({2021, 1U, 1U}, week) == T::Status::Ok,
                   week.Year == 2020, week.Week == 53U, week.Weekday == 5U,
                   T::GetIsoWeek({2018, 12U, 31U}, week) == T::Status::Ok, week.Year == 2019, week.Week == 1U);
    char text[T::MaximumIso8601Length + 1U]{};
    Size length{999U};
    ASTERIA_EXPECT(test, "calendar date format",
                   T::FormatIsoDate({2024, 2U, 29U}, text, sizeof(text), length) == T::Status::Ok,
                   std::strcmp(text, "2024-02-29") == 0, length == T::IsoDateLength);
    ASTERIA_EXPECT(test, "ISO week-date format uses the week year",
                   T::FormatIsoWeekDate({2018, 12U, 31U}, text, sizeof(text), length) == T::Status::Ok,
                   std::strcmp(text, "2019-W01-1") == 0, length == T::IsoWeekDateLength);
    ASTERIA_EXPECT(test, "date buffer rejection is atomic",
                   T::FormatIsoDate({}, text, T::IsoDateLength, length) == T::Status::BufferTooSmall,
                   T::FormatIsoWeekDate({}, text, T::IsoWeekDateLength, length) == T::Status::BufferTooSmall,
                   std::strcmp(text, "2019-W01-1") == 0, length == 10U,
                   T::FormatIsoDate({-1, 1U, 1U}, text, sizeof(text), length) == T::Status::OutOfRange,
                   T::FormatIsoWeekDate({-1, 1U, 1U}, text, sizeof(text), length) == T::Status::OutOfRange);
    ASTERIA_EXPECT(test, "UTC extended date/time",
                   T::FormatIso8601({2024, 2U, 29U, 3U, 4U, 5U}, 0, text, sizeof(text), length) == T::Status::Ok,
                   std::strcmp(text, "2024-02-29T03:04:05Z") == 0, length == 20U);
    ASTERIA_EXPECT(test, "second offset extension",
                   T::FormatIso8601({2024, 2U, 29U, 3U, 4U, 5U}, -3661, text, sizeof(text), length) == T::Status::Ok,
                   std::strcmp(text, "2024-02-29T03:04:05-01:01:01") == 0, length == T::MaximumIso8601Length);
    ASTERIA_EXPECT(test, "too small buffer preserves result",
                   T::FormatIso8601({}, 0, text, 20U, length) == T::Status::BufferTooSmall,
                   std::strcmp(text, "2024-02-29T03:04:05-01:01:01") == 0, length == 28U);
    ASTERIA_EXPECT(test, "format year/offset limits",
                   T::FormatIso8601({-1, 1U, 1U}, 0, text, sizeof(text), length) == T::Status::OutOfRange,
                   T::FormatIso8601({}, 86400, text, sizeof(text), length) == T::Status::OutOfRange,
                   T::FormatIso8601({}, std::numeric_limits<Int32>::min(), text, sizeof(text), length) ==
                       T::Status::OutOfRange,
                   T::FormatIso8601({}, 0, nullptr, 999U, length) == T::Status::BufferTooSmall);
}
void Seeded(TestSupport::TestContext& test, const UInt64 seed)
{
    TestSupport::Random random{seed};
    for (Size step{0U}; step < 1000U; ++step)
    {
        T::Civil input{static_cast<Int32>(random.Below(3601U)) - 800,
                       static_cast<UInt32>(random.Below(12U) + 1U),
                       1U,
                       static_cast<UInt32>(random.Below(24U)),
                       static_cast<UInt32>(random.Below(60U)),
                       static_cast<UInt32>(random.Below(60U))};
        input.Day = static_cast<UInt32>(random.Below(F::ReferenceMonth(input.Year, input.Month)) + 1U);
        const Int64 reference = F::ReferenceSeconds(input);
        const T::IsoWeek referenceWeek = F::ReferenceWeek(input);
        Int64 actual{0};
        T::Civil back{};
        T::IsoWeek week{};
        if (T::ToSeconds(input, actual) != T::Status::Ok || actual != reference ||
            T::FromSeconds(reference, back) != T::Status::Ok || back != input ||
            T::DayOfWeek(input) != F::ReferenceWeekday(reference) || T::GetIsoWeek(input, week) != T::Status::Ok ||
            week.Year != referenceWeek.Year || week.Week != referenceWeek.Week || week.Weekday != referenceWeek.Weekday)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu\n", seed, step));
            ASTERIA_FAIL(test, "independent year/month/day counting reference");
            return;
        }
        const Int64 change = static_cast<Int64>(random.Below(1001U)) - 500;
        ASTERIA_EXPECT(test, "seeded day arithmetic", T::AddDays(input, change, back) == T::Status::Ok,
                       back == F::ReferenceCivil(reference + change * 86400));
    }
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"CivilTests"};
    Asteria::Test::Boundaries(test);
    const auto seeds = Asteria::TestSupport::GetSeedRange(8536U, 8U);
    for (Asteria::Foundation::UInt64 index{0U}; index < seeds.Count; ++index)
    {
        Asteria::Test::Seeded(test, seeds.First + index);
    }
    return test.Finish();
}
