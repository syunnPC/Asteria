// SPDX-License-Identifier: Apache-2.0
#include "User/Text/Utf8.hpp"
#include "User/Terminal/Cp437.hpp"
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include <cstdio>
using Asteria::TestSupport::TestContext;
namespace Asteria::Test
{
namespace
{
void TestScalars(TestContext& runner) noexcept
{
    const auto seeds{TestSupport::GetSeedRange(3500U, 16U)};
    for (Foundation::UInt64 offset{0U}; offset < seeds.Count; ++offset)
    {
        const auto seed{seeds.First + offset};
        TestSupport::Random random{seed};
        for (Foundation::Size step{0U}; step < 4096U; ++step)
        {
            const auto point{static_cast<Foundation::UInt32>(random.Below(0x110000U))};
            Foundation::UInt8 bytes[4U]{};
            const auto size{User::Text::EncodeUtf8(point, bytes)};
            User::Text::Utf8Decoder decoder{};
            bool valid{true};
            for (Foundation::Size at{0U}; at < size; ++at)
            {
                valid = valid && decoder.Feed(bytes[at]);
            }
            if (!ASTERIA_EXPECT(runner, "scalar UTF-8 round trip", (size != 0U) == User::Text::IsScalar(point),
                                (size == 0U) || (valid && decoder.Complete() && (decoder.Point() == point) &&
                                                 User::Text::ValidUtf8(bytes, size))))
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%zu point=%x\n",
                                               static_cast<unsigned long long>(seed), step, point));
                return;
            }
        }
    }
    ASTERIA_EXPECT(runner, "invalid scalar arguments rejected", !User::Text::IsScalar(0x110000U),
                   User::Text::EncodeUtf8(0x110000U, nullptr) == 0U, !User::Text::ValidUtf8(nullptr, 1U),
                   User::Text::ValidUtf8(nullptr, 0U));
}
void TestFontBoundary(TestContext& runner) noexcept
{
    using Asteria::User::Terminal::Cp437Graphics;
    using Asteria::User::Terminal::Cp437MappingCount;
    using Asteria::User::Terminal::MapToCp437;
    bool exact{true};
    for (Foundation::Size at{0U}; at < Cp437MappingCount; ++at)
    {
        exact = exact && (Cp437Graphics[at].Code == at + 1U) &&
                (MapToCp437(Cp437Graphics[at].CodePoint) == Cp437Graphics[at].Code);
    }
    ASTERIA_EXPECT(runner, "exactly glyphs 01-FE share one Unicode map", exact, Cp437MappingCount == 254U,
                   MapToCp437(0x4E2DU) == 0xFEU, MapToCp437(User::Text::ReplacementCodePoint) == 0xFEU,
                   MapToCp437(0x00A0U) == 0xFEU);
}
} // namespace
} // namespace Asteria::Test
int main()
{
    TestContext runner{"Utf8Tests"};
    Asteria::Test::TestScalars(runner);
    Asteria::Test::TestFontBoundary(runner);
    return runner.Finish();
}
