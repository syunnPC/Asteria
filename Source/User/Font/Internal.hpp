// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_INTERNAL_HPP_INCLUDED
#define ASTERIA_USER_FONT_INTERNAL_HPP_INCLUDED
#include "User/Font/Font.hpp"
namespace Asteria::User::Font::Detail
{
// Reads are deliberately safe even on failed validation; callers still return errors.
[[nodiscard]] inline UInt8 U8(const Bytes b, const Size at) noexcept
{
    return b.Contains(at, 1U) ? b.Data[at] : 0U;
}
[[nodiscard]] inline UInt16 U16(const Bytes b, const Size at) noexcept
{
    return b.Contains(at, 2U) ? static_cast<UInt16>((static_cast<UInt16>(b.Data[at]) << 8U) | b.Data[at + 1U]) : 0U;
}
[[nodiscard]] inline UInt32 U32(const Bytes b, const Size at) noexcept
{
    return b.Contains(at, 4U) ? (static_cast<UInt32>(U16(b, at)) << 16U) | U16(b, at + 2U) : 0U;
}
[[nodiscard]] inline Int16 S16(const Bytes b, const Size at) noexcept
{
    return static_cast<Int16>(U16(b, at));
}
[[nodiscard]] inline Int32 S32(const Bytes b, const Size at) noexcept
{
    return static_cast<Int32>(U32(b, at));
}
[[nodiscard]] inline UInt32 Offset(const Bytes b, const Size at, const UInt32 size) noexcept
{
    if ((size < 1U) || (size > 4U) || !b.Contains(at, size))
    {
        return 0U;
    }
    UInt32 value{};
    for (UInt32 i{}; i < size; ++i)
    {
        value = (value << 8U) | b.Data[at + i];
    }
    return value;
}
[[nodiscard]] inline Bytes Tail(const Bytes b, const Size at) noexcept
{
    return at <= b.Length ? b.Slice(at, b.Length - at) : Bytes{};
}
[[nodiscard]] constexpr Int64 Abs(const Int64 v) noexcept
{
    return v < 0 ? -v : v;
}
[[nodiscard]] constexpr bool Coordinate(const Int64 v) noexcept
{
    return v >= -MaximumCoordinate && v <= MaximumCoordinate;
}
[[nodiscard]] constexpr Int64 Mid(const Int64 a, const Int64 b) noexcept
{
    return (a + b) / 2;
}
struct Budget
{
    UInt32 Left{MaximumValidationWork};
    [[nodiscard]] bool Spend(const Size count = 1U) noexcept
    {
        if (count > Left)
        {
            Left = 0U;
            return false;
        }
        Left -= static_cast<UInt32>(count);
        return true;
    }
};
[[nodiscard]] Error ReadIndex(Bytes data, Size& at, Index& index) noexcept;
[[nodiscard]] Error ValidateLayout(Bytes table, bool positioning, UInt16 glyphs, Budget& budget) noexcept;
} // namespace Asteria::User::Font::Detail
#endif
