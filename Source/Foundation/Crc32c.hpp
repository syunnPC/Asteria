// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_FOUNDATION_CRC32C_HPP_INCLUDED
#define ASTERIA_FOUNDATION_CRC32C_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Foundation
{
/// Reflected Castagnoli CRC. Seed is a previous finalized CRC (zero for a
/// new stream); Compute can therefore be chained without buffering the input.
class Crc32c final
{
public:
    [[nodiscard]] static constexpr Foundation::UInt32 Start(const Foundation::UInt32 seed = 0U) noexcept
    {
        return ~seed;
    }

    [[nodiscard]] static constexpr Foundation::UInt32 Update(Foundation::UInt32 state,
                                                             const Foundation::UInt8 byte) noexcept
    {
        state ^= byte;
        for (Foundation::UInt32 bit{0U}; bit < 8U; ++bit)
        {
            state = (state >> 1U) ^ (((state & 1U) != 0U) ? 0x82F63B78U : 0U);
        }
        return state;
    }

    [[nodiscard]] static constexpr Foundation::UInt32 Finish(const Foundation::UInt32 state) noexcept
    {
        return ~state;
    }

    /// data may be null only when size is zero.
    [[nodiscard]] static constexpr Foundation::UInt32
    Compute(const Foundation::UInt8* data, const Foundation::Size size, const Foundation::UInt32 seed = 0U) noexcept
    {
        Foundation::UInt32 state = Start(seed);
        for (Foundation::Size index{0U}; index < size; ++index)
        {
            state = Update(state, data[index]);
        }
        return Finish(state);
    }
};
} // namespace Asteria::Foundation

#endif // ASTERIA_FOUNDATION_CRC32C_HPP_INCLUDED
