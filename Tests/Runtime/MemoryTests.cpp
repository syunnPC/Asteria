// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>

#include "Foundation/Types.hpp"
#include "Runtime/Memory.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{

void TestMemoryPrimitives(TestContext& runner) noexcept
{
    Foundation::UInt8 source[8U]{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    Foundation::UInt8 destination[8U]{};

    ASTERIA_EXPECT(runner, "memcpy returns its destination", memcpy(destination, source, 8U) == destination);
    ASTERIA_EXPECT(runner, "memcpy copies every byte", memcmp(destination, source, 8U) == 0);

    ASTERIA_EXPECT(runner, "memset returns its destination", memset(destination, 0x1AB, 8U) == destination);
    for (Foundation::Size index{0U}; index < 8U; ++index)
    {
        ASTERIA_EXPECT(runner, "memset uses the low eight value bits", destination[index] == 0xABU);
    }

    Foundation::UInt8 overlapForward[8U]{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    static_cast<void>(memmove(overlapForward, overlapForward + 2U, 6U));
    const Foundation::UInt8 expectedForward[8U]{2U, 3U, 4U, 5U, 6U, 7U, 6U, 7U};
    ASTERIA_EXPECT(runner, "memmove handles overlap toward lower addresses",
                   memcmp(overlapForward, expectedForward, 8U) == 0);

    Foundation::UInt8 overlapBackward[8U]{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    static_cast<void>(memmove(overlapBackward + 2U, overlapBackward, 6U));
    const Foundation::UInt8 expectedBackward[8U]{0U, 1U, 0U, 1U, 2U, 3U, 4U, 5U};
    ASTERIA_EXPECT(runner, "memmove handles overlap toward higher addresses",
                   memcmp(overlapBackward, expectedBackward, 8U) == 0);

    ASTERIA_EXPECT(runner, "memcmp reports a lower byte", memcmp(source, destination, 1U) < 0);
    ASTERIA_EXPECT(runner, "memcmp reports a higher byte", memcmp(destination, source, 1U) > 0);
    ASTERIA_EXPECT(runner, "zero-length memcmp accepts null ranges", memcmp(nullptr, nullptr, 0U) == 0);
    ASTERIA_EXPECT(runner, "zero-length memset accepts a null range", memset(nullptr, 0, 0U) == nullptr);
    ASTERIA_EXPECT(runner, "zero-length memcpy accepts null ranges", memcpy(nullptr, nullptr, 0U) == nullptr);
    ASTERIA_EXPECT(runner, "zero-length memmove accepts null ranges", memmove(nullptr, nullptr, 0U) == nullptr);
}

void TestMemoryBoundaryMatrix(TestContext& runner) noexcept
{
    Foundation::UInt8 source[96U]{};
    for (Foundation::Size index{0U}; index < 96U; ++index)
    {
        source[index] = static_cast<Foundation::UInt8>((index * 37U + 11U) & 0xFFU);
    }

    for (Foundation::Size offset{0U}; offset < 8U; ++offset)
    {
        for (Foundation::Size length{0U}; length <= 64U; ++length)
        {
            Foundation::UInt8 destination[96U]{};
            static_cast<void>(memset(destination, 0xA5, 96U));
            static_cast<void>(memcpy(destination + offset, source + (7U - offset), length));

            bool matches{true};
            for (Foundation::Size index{0U}; index < 96U; ++index)
            {
                Foundation::UInt8 expected{0xA5U};
                if ((index >= offset) && (index < (offset + length)))
                {
                    expected = source[(7U - offset) + (index - offset)];
                }
                if (destination[index] != expected)
                {
                    matches = false;
                }
            }
            ASTERIA_EXPECT(runner, "memcpy handles alignment and length matrix", matches);
        }
    }

    for (Foundation::Size sourceOffset{0U}; sourceOffset < 8U; ++sourceOffset)
    {
        for (Foundation::Size destinationOffset{0U}; destinationOffset < 8U; ++destinationOffset)
        {
            Foundation::UInt8 candidate[48U]{};
            Foundation::UInt8 expected[48U]{};
            Foundation::UInt8 temporary[32U]{};
            for (Foundation::Size index{0U}; index < 48U; ++index)
            {
                const Foundation::UInt8 value = static_cast<Foundation::UInt8>(index + 3U);
                candidate[index] = value;
                expected[index] = value;
            }
            for (Foundation::Size index{0U}; index < 32U; ++index)
            {
                temporary[index] = expected[sourceOffset + index];
            }
            for (Foundation::Size index{0U}; index < 32U; ++index)
            {
                expected[destinationOffset + index] = temporary[index];
            }

            static_cast<void>(memmove(candidate + destinationOffset, candidate + sourceOffset, 32U));
            ASTERIA_EXPECT(runner, "memmove matches a temporary-buffer reference model",
                           memcmp(candidate, expected, 48U) == 0);
        }
    }

    Foundation::UInt8 first[32U]{};
    Foundation::UInt8 second[32U]{};
    static_cast<void>(memset(first, -1, 32U));
    static_cast<void>(memset(second, -1, 32U));
    ASTERIA_EXPECT(runner, "negative memset value uses its low byte", memcmp(first, second, 32U) == 0);
    first[31U] = 0x7FU;
    ASTERIA_EXPECT(runner, "memcmp detects a difference in the final byte", memcmp(first, second, 32U) < 0);
    ASTERIA_EXPECT(runner, "nonempty self-move preserves the range and returns destination",
                   memmove(first, first, 32U) == first);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"MemoryTests"};
    Asteria::Test::TestMemoryPrimitives(runner);
    Asteria::Test::TestMemoryBoundaryMatrix(runner);
    return runner.Finish();
}
