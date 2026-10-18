// SPDX-License-Identifier: Apache-2.0
#include "LibcTestSupport.hpp"

#include "User/Libc/CompilerArithmetic.hpp"

namespace Asteria::Test::Libc
{
namespace
{
void Pair(TestContext& context, const std::uint64_t numerator, const std::uint64_t denominator) noexcept
{
    if (denominator == 0U)
    {
        return;
    }
    ASTERIA_EXPECT(context, "unsigned compiler ABI quotient and remainder",
                   asteria_libc___udivdi3(numerator, denominator) == numerator / denominator,
                   asteria_libc___umoddi3(numerator, denominator) == numerator % denominator);
    const auto signedNumerator = std::bit_cast<std::int64_t>(numerator);
    const auto signedDenominator = std::bit_cast<std::int64_t>(denominator);
    if (signedNumerator == INT64_MIN && signedDenominator == -1)
    {
        return; // The C signed overflow case has no defined oracle.
    }
    ASTERIA_EXPECT(context, "signed compiler ABI quotient and remainder",
                   asteria_libc___divdi3(signedNumerator, signedDenominator) == signedNumerator / signedDenominator,
                   asteria_libc___moddi3(signedNumerator, signedDenominator) == signedNumerator % signedDenominator);
}
} // namespace
} // namespace Asteria::Test::Libc

int main()
{
    Asteria::Test::Libc::TestContext context{"LibcCompilerArithmetic"};
    Asteria::Test::Libc::Random random;
    constexpr std::uint64_t Edges[]{
        0U, 1U, 2U, 3U, 0xFFFFFFFFU, 0x100000000U, 0x7FFFFFFFFFFFFFFFU, 0x8000000000000000U, UINT64_MAX};
    for (const auto numerator : Edges)
    {
        for (const auto denominator : Edges)
        {
            Asteria::Test::Libc::Pair(context, numerator, denominator);
        }
    }
    for (size_t step{0U}; step < 10000U; ++step)
    {
        Asteria::Test::Libc::Pair(context, random.Next(), random.Next());
    }
    return context.Finish();
}
