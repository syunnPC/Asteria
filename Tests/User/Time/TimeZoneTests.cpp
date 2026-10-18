// SPDX-License-Identifier: Apache-2.0
#include "User/Time/TzifFixture.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"
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
using Asteria::Foundation::UInt8;
[[nodiscard]] bool HasType(const T::TimeZone& zone, const Int64 utc, const Int32 offset, const bool dst)
{
    T::LocalType type{};
    return zone.TypeAt(utc, type) == T::Status::Ok && type.OffsetSeconds == offset && type.Daylight == dst;
}
void Table(TestSupport::TestContext& test)
{
    const Int64 spring = F::ReferenceSeconds({2024, 3U, 10U, 7U});
    const Int64 fall = F::ReferenceSeconds({2024, 11U, 3U, 6U});
    F::Block block{};
    block.Names = std::string{"EST\0EDT\0", 8U};
    block.Types = {{-18000, 0U, 0U}, {-14400, 1U, 4U}};
    block.Transitions = {{spring, 1U}, {fall, 0U}};
    for (const UInt8 version : {UInt8{0U}, UInt8{'2'}, UInt8{'3'}})
    {
        const auto bytes = F::File(block, version, version == 0U ? "" : "EST5EDT,M3.2.0,M11.1.0");
        T::TimeZone zone{};
        T::LocalTime local{};
        T::LocalMatches matches{};
        ASTERIA_EXPECT(test, "generated TZif table loads", zone.Load(bytes.data(), bytes.size()) == T::Status::Ok);
        ASTERIA_EXPECT(test, "spring exact boundary changes offset", HasType(zone, spring - 1, -18000, false),
                       HasType(zone, spring, -14400, true), HasType(zone, spring + 1, -14400, true),
                       zone.ToLocal(spring - 1, local) == T::Status::Ok,
                       (local.Date == T::Civil{2024, 3U, 10U, 1U, 59U, 59U}),
                       zone.ToLocal(spring, local) == T::Status::Ok, (local.Date == T::Civil{2024, 3U, 10U, 3U}));
        ASTERIA_EXPECT(test, "type zero before table", HasType(zone, spring - 10000000, -18000, false));
        ASTERIA_EXPECT(test, "gap never normalized",
                       zone.FromLocal({2024, 3U, 10U, 2U, 30U}, matches) == T::Status::Gap, matches.Count == 0U);
        if (version != 0U)
        {
            ASTERIA_EXPECT(test, "fall exact boundary and fold", HasType(zone, fall - 1, -14400, true),
                           HasType(zone, fall, -18000, false),
                           zone.FromLocal({2024, 11U, 3U, 1U, 30U}, matches) == T::Status::Ambiguous,
                           matches.Count == 2U, matches.Earliest == fall - 1800, matches.Latest == fall + 1800);
            const Int64 future = F::ReferenceSeconds({2099, 7U, 1U});
            ASTERIA_EXPECT(test, "footer beyond the table", HasType(zone, future, -14400, true),
                           HasType(zone, F::ReferenceSeconds({2099, 1U, 1U}), -18000, false));
        }
        else
        {
            ASTERIA_EXPECT(
                test, "v1 unspecified from final boundary", zone.ToLocal(fall, local) == T::Status::Unspecified,
                zone.FromLocal({2024, 11U, 3U, 1U, 30U}, matches) == T::Status::Unspecified, matches.Count == 0U);
        }
    }
    F::Block secondsBlock{};
    secondsBlock.Types = {{3661, 0U, 0U}};
    secondsBlock.Leaps = {{78796800, 1}, {94694400, 2}, {100000000, 1}};
    const auto secondsBytes = F::File(secondsBlock, 0U);
    T::TimeZone zone{};
    T::LocalTime local{};
    T::LocalMatches matches{};
    ASTERIA_EXPECT(test, "leap records accepted, ignored, and second offsets retained",
                   zone.Load(secondsBytes.data(), secondsBytes.size()) == T::Status::Ok,
                   zone.ToLocal(-1, local) == T::Status::Ok, (local.Date == T::Civil{1970, 1U, 1U, 1U, 1U}),
                   local.Type.OffsetSeconds == 3661, zone.FromLocal(local.Date, matches) == T::Status::Ok,
                   matches.Earliest == -1, zone.ToLocal(94694400, local) == T::Status::Ok,
                   local.Date == F::ReferenceCivil(94698061));
    ASTERIA_EXPECT(test, "civil overflow leaves result unchanged",
                   zone.ToLocal(std::numeric_limits<Int64>::min(), local) == T::Status::OutOfRange,
                   zone.ToLocal(std::numeric_limits<Int64>::max(), local) == T::Status::OutOfRange);
    // A deliberately unusual sequence creates three matches for one wall time.
    F::Block triple{};
    triple.Types = {{7200, 0U, 0U}, {3600, 0U, 0U}, {0, 0U, 0U}};
    triple.Transitions = {{0, 1U}, {1800, 2U}, {100000, 2U}};
    const auto tripleBytes = F::File(triple, 0U);
    ASTERIA_EXPECT(test, "inverse conversion retains more than two candidates",
                   zone.Load(tripleBytes.data(), tripleBytes.size()) == T::Status::Ok,
                   zone.FromLocal({1970, 1U, 1U, 1U, 15U}, matches) == T::Status::Ambiguous, matches.Count == 3U,
                   matches.Earliest == -2700, matches.Latest == 4500);
}
void Footers(TestSupport::TestContext& test)
{
    struct Sample
    {
        const char* Footer;
        UInt8 Version;
        T::Civil Date;
        Int32 Offset;
        bool Dst;
    };
    const Sample samples[]{
        {"<+054530>-5:45:30", '2', {2024, 1U, 1U}, 20730, false},
        {"STD0DST,M3.2.0,M11.1.0", '2', {2024, 7U, 1U}, 3600, true},
        {"STD-10DST-11,M10.1.0,M4.1.0", '2', {2024, 1U, 1U}, 39600, true},
        {"STD-10DST-11,M10.1.0,M4.1.0", '2', {2024, 7U, 1U}, 36000, false},
        {"STD0DST1,M3.2.0,M11.1.0", '2', {2024, 7U, 1U}, -3600, true},
        {"STD0DST,J60/0,J300/0", '2', {2024, 2U, 29U, 12U}, 0, false},
        {"STD0DST,59/0,300/0", '2', {2024, 2U, 29U, 12U}, 3600, true},
        {"STD0DST,J60/0,J300/0", '2', {2023, 3U, 1U, 12U}, 3600, true},
        {"<-03>3<-02>,M3.5.0/-2,M10.5.0/-1", '3', {2024, 3U, 31U}, -10800, false},
        {"<-03>3<-02>,M3.5.0/-2,M10.5.0/-1", '3', {2024, 3U, 31U, 1U}, -7200, true},
        {"EST5EDT,0/0,J365/25", '3', {2024, 1U, 1U}, -14400, true},
        {"EST5EDT,0/0,J365/25", '3', {2024, 12U, 31U, 23U}, -14400, true},
        {"STD0DST,0/-167,365/167", '3', {2024, 1U, 1U}, 3600, true},
        {"STD0DST,365/167,365/166", '3', {2024, 1U, 1U}, 3600, true},
        {"STD0DST,365/167,365/166", '3', {2024, 1U, 7U, 22U}, 0, false},
        {"STD0DST,365/167,365/166", '3', {2024, 1U, 7U, 23U}, 3600, true},
        {"STD0DST,M3.5.0/24,M10.5.0/24", '2', {2024, 4U, 1U}, 3600, true},
    };
    T::TimeZone zone{};
    for (const auto& sample : samples)
    {
        const auto bytes = F::File({}, sample.Version, sample.Footer);
        ASTERIA_EXPECT(test, "POSIX footer grammar and recurring boundary rules",
                       zone.Load(bytes.data(), bytes.size()) == T::Status::Ok,
                       HasType(zone, F::ReferenceSeconds(sample.Date), sample.Offset, sample.Dst));
    }
    const auto bytes = F::File({}, '3', "STD0DST,M3.2.0,M11.1.0");
    ASTERIA_EXPECT(test, "recurring rules support full timestamp range",
                   zone.Load(bytes.data(), bytes.size()) == T::Status::Ok);
    T::LocalMatches matches{};
    ASTERIA_EXPECT(test, "recurring footer gap and fold",
                   zone.FromLocal({2024, 3U, 10U, 2U, 30U}, matches) == T::Status::Gap,
                   zone.FromLocal({2024, 11U, 3U, 1U, 30U}, matches) == T::Status::Ambiguous, matches.Count == 2U,
                   matches.Earliest == F::ReferenceSeconds({2024, 11U, 3U, 0U, 30U}),
                   matches.Latest == F::ReferenceSeconds({2024, 11U, 3U, 1U, 30U}));
    T::LocalType type{};
    ASTERIA_EXPECT(test, "recurrence at integer extrema has no overflow",
                   zone.TypeAt(std::numeric_limits<Int64>::min(), type) == T::Status::Ok,
                   zone.TypeAt(std::numeric_limits<Int64>::max(), type) == T::Status::Ok);
}
// Separate reference: enumerate every day's weekday and put transitions on
// the second Sunday of March and first Sunday of November. No footer parser
// or calendar cycle formulas are shared with the production implementation.
[[nodiscard]] Int64 ReferenceSunday(const Int32 year, const UInt32 month, const UInt32 occurrence)
{
    UInt32 seen{0U};
    for (UInt32 day{1U}; day <= 31U; ++day)
    {
        const Int64 time = F::ReferenceSeconds({year, month, day});
        if (F::ReferenceWeekday(time) == 7U && ++seen == occurrence)
        {
            return time;
        }
    }
    return 0;
}
void Seeded(TestSupport::TestContext& test, const UInt64 seed)
{
    TestSupport::Random random{seed};
    F::Block block{};
    block.Names = std::string{"STD\0DST\0", 8U};
    // Offsets include seconds and a non-hour DST change.
    const Int32 standard = static_cast<Int32>(random.Below(72001U)) - 36000;
    const Int32 daylight = standard + static_cast<Int32>(random.Below(3600U) + 1U);
    block.Types = {{standard, 0U, 0U}, {daylight, 1U, 4U}, {standard, 0U, 0U}};
    for (Int32 year{1990}; year <= 2030; ++year)
    {
        block.Transitions.push_back({ReferenceSunday(year, 3U, 2U) + 7200 - standard, 1U});
        block.Transitions.push_back({ReferenceSunday(year, 11U, 1U) + 7200 - daylight, 2U});
    }
    const auto bytes = F::File(block);
    T::TimeZone zone{};
    ASTERIA_EXPECT(test, "seeded generated multi-year table", zone.Load(bytes.data(), bytes.size()) == T::Status::Ok);
    for (Size step{0U}; step < 2000U; ++step)
    {
        const Int64 time = 630720000 + static_cast<Int64>(random.Below(1200000000U));
        UInt8 type{0U};
        for (const auto& transition : block.Transitions)
        {
            if (transition.Time > time)
            {
                break;
            }
            type = transition.Type;
        }
        const Int32 offset = block.Types[type].Offset;
        T::LocalTime local{};
        bool equal = zone.ToLocal(time, local) == T::Status::Ok && local.Date == F::ReferenceCivil(time + offset) &&
                     local.Type.OffsetSeconds == offset && local.Type.Daylight == (type == 1U);
        T::LocalMatches matches{};
        if (equal)
        {
            const Int64 wall = time + offset;
            UInt32 count{0U};
            Int64 earliest{0};
            Int64 latest{0};
            for (const Int32 candidateOffset : {standard, daylight})
            {
                const Int64 candidate = wall - candidateOffset;
                UInt8 candidateType{0U};
                for (const auto& transition : block.Transitions)
                {
                    if (transition.Time > candidate)
                    {
                        break;
                    }
                    candidateType = transition.Type;
                }
                if (block.Types[candidateType].Offset == candidateOffset)
                {
                    if (count == 0U || candidate < earliest)
                    {
                        earliest = candidate;
                    }
                    if (count == 0U || candidate > latest)
                    {
                        latest = candidate;
                    }
                    ++count;
                }
            }
            const auto status = zone.FromLocal(local.Date, matches);
            equal = status == (count == 1U ? T::Status::Ok : T::Status::Ambiguous) && matches.Count == count &&
                    matches.Earliest == earliest && matches.Latest == latest;
        }
        if (!equal)
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu utc=%lld\n", seed, step, time));
            ASTERIA_FAIL(test, "seeded transition scan and independent civil reference");
            return;
        }
    }
    const auto footerBytes = F::File({}, '3', "STD5DST,M3.2.0,M11.1.0");
    ASTERIA_EXPECT(test, "reference footer zone", zone.Load(footerBytes.data(), footerBytes.size()) == T::Status::Ok);
    for (Size step{0U}; step < 1000U; ++step)
    {
        const Int32 year = static_cast<Int32>(random.Below(1001U)) + 1800;
        const Int64 start = ReferenceSunday(year, 3U, 2U) + 7 * 3600;
        const Int64 end = ReferenceSunday(year, 11U, 1U) + 6 * 3600;
        const Int64 utc = F::ReferenceSeconds({year, 1U, 1U}) + static_cast<Int64>(random.Below(365U * 86400U));
        const bool dst = utc >= start && utc < end;
        ASTERIA_EXPECT(test, "seeded future footer against enumerated weekdays",
                       HasType(zone, utc, dst ? -14400 : -18000, dst));
        for (const Int64 boundary : {start, end})
        {
            for (const Int64 change : {Int64{-1}, Int64{0}, Int64{1}})
            {
                const bool boundaryDst = boundary + change >= start && boundary + change < end;
                ASTERIA_EXPECT(test, "seeded footer exact transitions",
                               HasType(zone, boundary + change, boundaryDst ? -14400 : -18000, boundaryDst));
            }
        }
    }
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"TimeZoneTests"};
    Asteria::Test::Table(test);
    Asteria::Test::Footers(test);
    const auto seeds = Asteria::TestSupport::GetSeedRange(28536U, 8U);
    for (Asteria::Foundation::UInt64 index{0U}; index < seeds.Count; ++index)
    {
        Asteria::Test::Seeded(test, seeds.First + index);
    }
    return test.Finish();
}
