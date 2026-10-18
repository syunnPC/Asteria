// SPDX-License-Identifier: Apache-2.0
#include "User/Time/TzifFixture.hpp"
#include "Support/TestContext.hpp"
#include "Support/Random.hpp"
#include <limits>
#include <array>
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
// Keep fixture reset/copy explicit: GCC 15's analyzer diagnoses the empty
// standard allocator returned by vector::get_allocator during vector copy/move
// paths. These range/clear operations keep all analyzer gates enabled.
[[nodiscard]] std::vector<UInt8> CopyBytes(const std::vector<UInt8>& original)
{
    return std::vector<UInt8>(original.begin(), original.end());
}
void ResetBlock(F::Block& block)
{
    block.Transitions.clear();
    block.Types.clear();
    block.Types.push_back({0, 0U, 0U});
    block.Names.assign("UTC\0", 4U);
    block.Leaps.clear();
    block.Standard.clear();
    block.Ut.clear();
}
void Reject(TestSupport::TestContext& test, const std::vector<UInt8>& bytes)
{
    T::TimeZone zone{};
    T::LocalType type{};
    ASTERIA_EXPECT(test, "malformed RFC 8536 structure rejected",
                   zone.Load(bytes.data(), bytes.size()) == T::Status::Invalid, !zone.IsLoaded(),
                   zone.TypeAt(0, type) == T::Status::Invalid);
}
void Structures(TestSupport::TestContext& test)
{
    // RFC 8536 §§3.1–3.3 mandatory fields, counts and value relationships.
    const auto valid = F::File({}, 0U);
    T::TimeZone zone{};
    ASTERIA_EXPECT(test, "minimal v1", zone.Load(valid.data(), valid.size()) == T::Status::Ok);
    for (const Size field : {Size{20U}, Size{24U}, Size{28U}, Size{32U}, Size{36U}, Size{40U}})
    {
        auto bytes = CopyBytes(valid);
        F::Put(bytes, field, 0xFFFFFFFFU);
        Reject(test, bytes);
    }
    for (const Size field : {Size{36U}, Size{40U}})
    {
        auto bytes = CopyBytes(valid);
        F::Put(bytes, field, 0U);
        Reject(test, bytes);
    }
    for (const Size byte : {Size{0U}, Size{4U}, Size{48U}, Size{49U}, Size{53U}})
    {
        auto bytes = CopyBytes(valid);
        bytes[byte] = 0xFFU;
        Reject(test, bytes);
    }
    auto trailing = CopyBytes(valid);
    trailing.push_back(0U);
    Reject(test, trailing);
    auto offsetMin = CopyBytes(valid);
    F::Put(offsetMin, 44U, 0x80000000U);
    Reject(test, offsetMin);
    auto reserved = CopyBytes(valid);
    reserved[5U] = 99U;
    ASTERIA_EXPECT(test, "reserved bytes ignored", zone.Load(reserved.data(), reserved.size()) == T::Status::Ok);
    F::Block block{};
    block.Transitions = {{5, 0U}, {5, 0U}};
    Reject(test, F::File(block));
    block.Transitions = {{6, 0U}, {5, 0U}};
    Reject(test, F::File(block));
    block.Transitions = {{5, 1U}};
    Reject(test, F::File(block));
    block.Transitions = {};
    block.Standard = {2U};
    Reject(test, F::File(block));
    block.Standard = {0U};
    block.Ut = {1U};
    Reject(test, F::File(block));
    block.Standard = {};
    Reject(test, F::File(block));
    block.Standard = {1U};
    block.Ut = {2U};
    Reject(test, F::File(block));
    block.Ut = {1U};
    const auto indicatorBytes = F::File(block);
    ASTERIA_EXPECT(test, "valid standard and UT indicators",
                   zone.Load(indicatorBytes.data(), indicatorBytes.size()) == T::Status::Ok);
    ResetBlock(block);
    for (const std::vector<F::Leap>& leaps : {std::vector<F::Leap>{{-1, 1}},
                                              {{0, 0}},
                                              {{0, 2}},
                                              {{0, 1}, {2419198, 2}},
                                              {{0, 1}, {2419199, 3}},
                                              {{0, 1}, {0, 2}},
                                              {{3, 1}, {2, 2}}})
    {
        block.Leaps = leaps;
        Reject(test, F::File(block));
    }
    block.Leaps = {{0, -1}, {2419199, -2}, {4838398, -1}};
    const auto leapBytes = F::File(block);
    ASTERIA_EXPECT(test, "negative leaps and minimum spacing accepted",
                   zone.Load(leapBytes.data(), leapBytes.size()) == T::Status::Ok);
    ResetBlock(block);
    block.Names = std::string{"\xC0\x80\0", 3U};
    Reject(test, F::File(block));
    block.Names = std::string{"\xE6\x99\x82\0", 4U};
    const auto utf8Bytes = F::File(block);
    ASTERIA_EXPECT(test, "UTF-8 designations", zone.Load(utf8Bytes.data(), utf8Bytes.size()) == T::Status::Ok);
    block.Types[0U].Name = 1U;
    Reject(test, F::File(block));
    block.Names = std::string{"EST\0", 4U};
    block.Types[0U].Name = 1U;
    const auto suffixBytes = F::File(block);
    ASTERIA_EXPECT(test, "overlapping suffix designations",
                   zone.Load(suffixBytes.data(), suffixBytes.size()) == T::Status::Ok);
    ResetBlock(block);
    block.Transitions = {{0, 0U}};
    Reject(test, F::File(block, '3', "UTC1"));
    Reject(test, F::File(block, '3', "ABC0"));
    block.Types[0U].Dst = 1U;
    Reject(test, F::File(block, '3', "UTC0"));
    ResetBlock(block);
    auto mismatchVersion = F::File(block);
    mismatchVersion[54U + 4U] = '2';
    Reject(test, mismatchVersion);
    auto invalidFirst = F::File(block);
    invalidFirst[48U] = 2U;
    Reject(test, invalidFirst);
    auto missingNewline = F::File(block);
    missingNewline.back() = 0U;
    Reject(test, missingNewline);
    auto largeFooter = F::File(block, '3', std::string(T::TimeZone::MaximumFooterSize + 1U, 'A'));
    Reject(test, largeFooter);
    for (const char* footer : {"UT0",
                               "UTC",
                               "UTC25",
                               "UTC0DST",
                               "UTC0DST,",
                               "UTC0DST,M0.1.0,M11.1.0",
                               "UTC0DST,M3.0.0,M11.1.0",
                               "UTC0DST,M3.6.0,M11.1.0",
                               "UTC0DST,M3.1.7,M11.1.0",
                               "UTC0DST,J0,J365",
                               "UTC0DST,366,1",
                               "UTC0DST,0/168,1",
                               "UTC0DST,0/-168,1",
                               "UTC0DST,0/1:60,1",
                               "UTC0DST,0/1:0:60,1",
                               "UTC0\n",
                               ":UTC0",
                               "<UT>0",
                               "<U!C>0",
                               "UTC999999999999999999",
                               "UTC0DST,0,1extra"})
    {
        Reject(test, F::File({}, '3', footer));
    }
    for (const char* footer : {"UTC0DST,0/-1,1", "UTC0DST,0/+1,1", "UTC0DST,0/25,1"})
    {
        Reject(test, F::File({}, '2', footer));
    }
    Reject(test, F::File({}, '3', std::string{"UTC0\0", 5U}));
    ASTERIA_EXPECT(test, "null and oversized input", zone.Load(nullptr, 100U) == T::Status::Invalid,
                   zone.Load(valid.data(), T::TimeZone::MaximumFileSize + 1U) == T::Status::Invalid);
    for (const UInt8 version : {UInt8{0U}, UInt8{'2'}, UInt8{'3'}})
    {
        const auto full = F::File({}, version, version == 0U ? "" : "UTC0");
        for (Size size{0U}; size < full.size(); ++size)
        {
            ASTERIA_EXPECT(test, "every truncation rejected", zone.Load(full.data(), size) == T::Status::Invalid);
        }
    }
    F::Block extremes{};
    extremes.Types = {{2147483647, 0U, 0U}, {-2147483647, 0U, 0U}};
    extremes.Transitions = {{std::numeric_limits<Int64>::min(), 1U}, {std::numeric_limits<Int64>::max(), 0U}};
    const auto extremeBytes = F::File(extremes);
    T::LocalType type{};
    ASTERIA_EXPECT(test, "full signed transition and legal offset ranges",
                   zone.Load(extremeBytes.data(), extremeBytes.size()) == T::Status::Ok,
                   zone.TypeAt(std::numeric_limits<Int64>::min(), type) == T::Status::Ok,
                   type.OffsetSeconds == -2147483647,
                   zone.TypeAt(std::numeric_limits<Int64>::max(), type) == T::Status::Unspecified);
    F::Block limits{};
    limits.Types.assign(T::TimeZone::MaximumTypes, {0, 0U, 0U});
    for (UInt32 i{0U}; i < T::TimeZone::MaximumTransitions; ++i)
    {
        limits.Transitions.push_back({static_cast<Int64>(i), 255U});
    }
    limits.Names.assign(T::TimeZone::MaximumDesignations, '\0');
    limits.Leaps.reserve(T::TimeZone::MaximumLeaps);
    for (UInt32 i{0U}; i < T::TimeZone::MaximumLeaps; ++i)
    {
        limits.Leaps.push_back({static_cast<Int64>(i) * 2419199, static_cast<Int32>(i + 1U)});
    }
    const auto limitBytes = F::File(limits);
    ASTERIA_EXPECT(test, "all count limits including index 255",
                   zone.Load(limitBytes.data(), limitBytes.size()) == T::Status::Ok,
                   zone.TypeAt(0, type) == T::Status::Ok);
}
void Exercise(TestSupport::TestContext& test, T::TimeZone& zone, const UInt8* data, const Size length)
{
    const auto loaded = zone.Load(data, length);
    if (loaded != T::Status::Ok)
    {
        ASTERIA_EXPECT(test, "failed fuzz parse invalidates view", !zone.IsLoaded());
        return;
    }
    for (const Int64 time : {std::numeric_limits<Int64>::min(), Int64{-2208988800LL}, Int64{-1}, Int64{0},
                             Int64{2147483647}, std::numeric_limits<Int64>::max()})
    {
        T::LocalTime local{};
        T::LocalType type{};
        static_cast<void>(zone.TypeAt(time, type));
        if (zone.ToLocal(time, local) == T::Status::Ok)
        {
            T::LocalMatches matches{};
            const auto inverse = zone.FromLocal(local.Date, matches);
            ASTERIA_EXPECT(test, "accepted fuzz files round trip unless coverage unknown",
                           inverse == T::Status::Unspecified ||
                               ((inverse == T::Status::Ok || inverse == T::Status::Ambiguous) && matches.Count != 0U &&
                                time >= matches.Earliest && time <= matches.Latest));
        }
    }
}
void Fuzz(TestSupport::TestContext& test, const UInt64 seed)
{
    TestSupport::Random random{seed};
    static std::array<UInt8, 4096U> buffer{};
    T::TimeZone zone{};
    F::Block block{};
    block.Types = {{0, 0U, 0U}, {3600, 1U, 4U}};
    block.Names = std::string{"STD\0DST\0", 8U};
    block.Transitions = {{1000, 1U}, {10000, 0U}};
    block.Leaps = {{0, 1}, {2419199, 2}};
    block.Standard = {1U, 0U};
    block.Ut = {1U, 0U};
    const auto corpus = F::File(block, '3', "STD0DST,M3.2.0,M11.1.0");
    for (Size iteration{0U}; iteration < 4000U; ++iteration)
    {
        Size length{0U};
        if (iteration % 2U == 0U)
        {
            length = static_cast<Size>(random.Below(buffer.size() + 1U));
            for (Size i{0U}; i < length; ++i)
            {
                buffer[i] = static_cast<UInt8>(random.Next());
            }
        }
        else
        {
            length = corpus.size();
            for (Size i{0U}; i < length; ++i)
            {
                buffer[i] = corpus[i];
            }
            const Size mutations = static_cast<Size>(random.Below(8U) + 1U);
            for (Size i{0U}; i < mutations; ++i)
            {
                buffer[static_cast<Size>(random.Below(length))] = static_cast<UInt8>(random.Next());
            }
            if (random.Chance(1U, 4U))
            {
                length = static_cast<Size>(random.Below(length + 1U));
            }
        }
        Exercise(test, zone, buffer.data(), length);
    }
}
} // namespace
} // namespace Asteria::Test
int main()
{
    Asteria::TestSupport::TestContext test{"TzifValidationTests"};
    Asteria::Test::Structures(test);
    const auto seeds = Asteria::TestSupport::GetSeedRange(38536U, 8U);
    for (Asteria::Foundation::UInt64 index{0U}; index < seeds.Count; ++index)
    {
        Asteria::Test::Fuzz(test, seeds.First + index);
    }
    return test.Finish();
}
