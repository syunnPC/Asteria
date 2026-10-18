// SPDX-License-Identifier: Apache-2.0
#include "User/Filesystem/Crc32c.hpp"

#include "Support/TestContext.hpp"

namespace Asteria::Test
{
namespace
{
using Foundation::UInt8;
using User::Filesystem::Crc32c;
constexpr UInt8 Check[9U]{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(Crc32c::Compute(Check, 9U) == 0xE3069283U);
static_assert(Crc32c::Compute(nullptr, 0U) == 0U);
} // namespace
} // namespace Asteria::Test

int main() noexcept
{
    Asteria::TestSupport::TestContext context{"Crc32c"};
    using Asteria::User::Filesystem::Crc32c;
    Asteria::Foundation::UInt8 zeros[32U]{};
    Asteria::Foundation::UInt8 ones[32U]{};
    Asteria::Foundation::UInt8 ascending[32U]{};
    for (Asteria::Foundation::Size index{0U}; index < 32U; ++index)
    {
        ones[index] = 0xFFU;
        ascending[index] = static_cast<Asteria::Foundation::UInt8>(index);
    }
    ASTERIA_EXPECT(context, "standard Castagnoli and SCTP vectors",
                   Crc32c::Compute(Asteria::Test::Check, 9U) == 0xE3069283U, Crc32c::Compute(nullptr, 0U) == 0U,
                   Crc32c::Compute(zeros, 32U) == 0x8A9136AAU, Crc32c::Compute(ones, 32U) == 0x62A8AB43U,
                   Crc32c::Compute(ascending, 32U) == 0x46DD794EU);
    for (Asteria::Foundation::Size split{0U}; split <= 9U; ++split)
    {
        const Asteria::Foundation::UInt32 seed = Crc32c::Compute(Asteria::Test::Check, split);
        ASTERIA_EXPECT(context, "seed resumes a finalized prefix CRC",
                       Crc32c::Compute(Asteria::Test::Check + split, 9U - split, seed) == 0xE3069283U,
                       Crc32c::Compute(nullptr, 0U, seed) == seed);
    }
    return context.Finish();
}
