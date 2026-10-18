// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_IMAGING_INTERNAL_HPP_INCLUDED
#define ASTERIA_USER_IMAGING_INTERNAL_HPP_INCLUDED
#include "User/Imaging/Imaging.hpp"
namespace Asteria::User::Imaging::Detail
{
[[nodiscard]] inline bool Range(Size offset, Size length, Size total) noexcept
{
    return offset <= total && length <= total - offset;
}
[[nodiscard]] inline UInt32 Le16(const UInt8* p) noexcept
{
    return static_cast<UInt32>(p[0]) | (static_cast<UInt32>(p[1]) << 8U);
}
[[nodiscard]] inline UInt32 Le32(const UInt8* p) noexcept
{
    return Le16(p) | (Le16(p + 2U) << 16U);
}
[[nodiscard]] inline UInt32 Be16(const UInt8* p) noexcept
{
    return (static_cast<UInt32>(p[0]) << 8U) | p[1];
}
[[nodiscard]] inline UInt32 Be32(const UInt8* p) noexcept
{
    return (Be16(p) << 16U) | Be16(p + 2U);
}
[[nodiscard]] inline bool Input(Bytes b) noexcept
{
    return b.Data != nullptr && b.Count <= MaximumInput;
}
[[nodiscard]] inline Status Dimensions(UInt32 w, UInt32 h, Buffer out) noexcept
{
    if (w == 0U || h == 0U)
    {
        return Status::Header;
    }
    if (w > MaximumDimension || h > MaximumDimension || w > MaximumPixels / h)
    {
        return Status::Limit;
    }
    if (out.Data == nullptr)
    {
        return Status::InvalidBuffer;
    }
    if (static_cast<Size>(w) * h * 4U > out.Count)
    {
        return Status::Limit;
    }
    return Status::Ok;
}
[[nodiscard]] Status Dib(Bytes input, Size pixelOffset, bool icon, Buffer output, Image& image) noexcept;
} // namespace Asteria::User::Imaging::Detail
#endif
