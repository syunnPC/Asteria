// SPDX-License-Identifier: Apache-2.0
#include "Platform/Ia64/Runtime/CompilerArithmetic.hpp"

#include <cstdlib>

#include "Support/Random.hpp"
#include "Support/TestContext.hpp"

namespace Asteria::Platform
{
[[noreturn]] void FatalTrap() noexcept
{
    std::abort();
}
} // namespace Asteria::Platform

int main()
{
    Asteria::TestSupport::TestContext context{"IA-64 compiler unsigned arithmetic"};
    const auto seeds{Asteria::TestSupport::GetSeedRange(1U, 8U)};
    for (Asteria::Foundation::UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        Asteria::TestSupport::Random random{seed};
        for (Asteria::Foundation::Size step{0U}; step < 10000U; ++step)
        {
            const Asteria::Foundation::UInt64 numerator{random.Next()};
            const Asteria::Foundation::UInt64 denominator{
                step % 64U == 0U ? Asteria::Foundation::UInt64{1U} << (step / 64U % 64U) : random.Next() | 1U};
            ASTERIA_EXPECT(context, "compiler divide agrees with native model",
                           __udivdi3(numerator, denominator) == numerator / denominator,
                           __umoddi3(numerator, denominator) == numerator % denominator);
        }
    }
    ASTERIA_EXPECT(context, "carry and full-width boundaries",
                   __udivdi3(~Asteria::Foundation::UInt64{0U}, 1U) == ~Asteria::Foundation::UInt64{0U},
                   __umoddi3(~Asteria::Foundation::UInt64{0U}, ~Asteria::Foundation::UInt64{0U}) == 0U,
                   __udivdi3(1U, ~Asteria::Foundation::UInt64{0U}) == 0U);
    return context.Finish();
}
