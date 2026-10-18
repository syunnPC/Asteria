// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TEXT_UTF8_HPP_INCLUDED
#define ASTERIA_USER_TEXT_UTF8_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Text
{
/// Unicode Standard 16.0, section 3.9, table 3-7: shortest UTF-8 encodings of scalar values only.
/// No allocation; at most four steps for one code point.
inline constexpr Foundation::UInt32 ReplacementCodePoint{0xFFFDU};
[[nodiscard]] constexpr bool IsScalar(const Foundation::UInt32 point) noexcept
{
    return (point <= 0x10FFFFU) && ((point < 0xD800U) || (point > 0xDFFFU));
}
[[nodiscard]] constexpr bool IsPrintable(const Foundation::UInt32 point) noexcept
{
    return (point >= 0x20U) && ((point < 0x7FU) || (point > 0x9FU));
}

class Utf8Decoder final
{
public:
    /// False rejects this byte. Complete distinguishes a scalar from a
    /// partial sequence; Point is usable only after a complete sequence.
    [[nodiscard]] bool Feed(const Foundation::UInt8 byte) noexcept
    {
        if (m_remaining != 0U)
        {
            if ((byte < m_lower) || (byte > m_upper))
            {
                return false;
            }
            m_point = (m_point << 6U) | (byte & 0x3FU);
            --m_remaining;
            m_lower = 0x80U;
            m_upper = 0xBFU;
            return true;
        }
        m_lower = 0x80U;
        m_upper = 0xBFU;
        if (byte < 0x80U)
        {
            m_point = byte;
        }
        else if ((byte >= 0xC2U) && (byte <= 0xDFU))
        {
            m_point = byte & 0x1FU;
            m_remaining = 1U;
        }
        else if ((byte >= 0xE0U) && (byte <= 0xEFU))
        {
            m_point = byte & 0x0FU;
            m_remaining = 2U;
            if (byte == 0xE0U)
            {
                m_lower = 0xA0U;
            }
            if (byte == 0xEDU)
            {
                m_upper = 0x9FU;
            }
        }
        else if ((byte >= 0xF0U) && (byte <= 0xF4U))
        {
            m_point = byte & 0x07U;
            m_remaining = 3U;
            if (byte == 0xF0U)
            {
                m_lower = 0x90U;
            }
            if (byte == 0xF4U)
            {
                m_upper = 0x8FU;
            }
        }
        else
        {
            return false;
        }
        return true;
    }
    [[nodiscard]] bool Complete() const noexcept
    {
        return m_remaining == 0U;
    }
    [[nodiscard]] Foundation::UInt32 Point() const noexcept
    {
        return m_point;
    }

private:
    Foundation::UInt32 m_point{0U};
    Foundation::UInt8 m_remaining{0U};
    Foundation::UInt8 m_lower{0x80U};
    Foundation::UInt8 m_upper{0xBFU};
};

[[nodiscard]] inline bool ValidUtf8(const Foundation::UInt8* text, const Foundation::Size length,
                                    const bool printable = false) noexcept
{
    if ((text == nullptr) && (length != 0U))
    {
        return false;
    }
    Utf8Decoder decoder{};
    for (Foundation::Size at{0U}; at < length; ++at)
    {
        if (!decoder.Feed(text[at]) || (printable && decoder.Complete() && !IsPrintable(decoder.Point())))
        {
            return false;
        }
    }
    return decoder.Complete();
}

/// Consume one scalar, or one malformed byte, from a bounded string.
[[nodiscard]] inline Foundation::UInt32 NextCodePoint(const Foundation::UInt8* text, const Foundation::Size length,
                                                      Foundation::Size& at) noexcept
{
    const Foundation::Size start{at};
    Utf8Decoder decoder{};
    for (Foundation::Size step{0U}; (step < 4U) && (at < length); ++step)
    {
        if (!decoder.Feed(text[at++]))
        {
            at = start + 1U;
            return ReplacementCodePoint;
        }
        if (decoder.Complete())
        {
            return decoder.Point();
        }
    }
    at = start + 1U;
    return ReplacementCodePoint;
}

[[nodiscard]] inline Foundation::Size EncodeUtf8(const Foundation::UInt32 point, Foundation::UInt8* out) noexcept
{
    if ((out == nullptr) || !IsScalar(point))
    {
        return 0U;
    }
    if (point < 0x80U)
    {
        out[0U] = static_cast<Foundation::UInt8>(point);
        return 1U;
    }
    const Foundation::Size size{point < 0x800U ? 2U : (point < 0x10000U ? 3U : 4U)};
    constexpr Foundation::UInt8 prefixes[3U]{0xC0U, 0xE0U, 0xF0U};
    out[0U] = static_cast<Foundation::UInt8>(prefixes[size - 2U] | (point >> ((size - 1U) * 6U)));
    for (Foundation::Size at{1U}; at < size; ++at)
    {
        out[at] = static_cast<Foundation::UInt8>(0x80U | ((point >> ((size - 1U - at) * 6U)) & 0x3FU));
    }
    return size;
}
} // Namespace Asteria::User::Text
#endif
