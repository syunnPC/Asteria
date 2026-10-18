// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FILESYSTEM_NAMES_HPP_INCLUDED
#define ASTERIA_USER_FILESYSTEM_NAMES_HPP_INCLUDED
#include "Foundation/Types.hpp"
#include "User/Text/Utf8.hpp"
namespace Asteria::User::Filesystem
{
// Unicode Standard 16.0, section 3.9: strict scalar encodings. Format adapters
// share these conversions; UTF-16 is used only in on-disk formats.
inline constexpr Foundation::Size NameBytes{64U};
inline constexpr Foundation::Size MaximumWritePages{256U};
inline constexpr Foundation::UInt64 RootIdentifier{~Foundation::UInt64{0U}};
enum class NodeKind : Foundation::UInt8
{
    File = 1U,
    Directory = 2U
};
[[nodiscard, gnu::noinline]] inline bool ValidName(const Foundation::UInt8* name, Foundation::Size length) noexcept
{
    if ((length == 0U) || (length > NameBytes) || !Text::ValidUtf8(name, length, true))
    {
        return false;
    }
    if (length == 2U && name[0U] == '.' && name[1U] == '.')
    {
        return true;
    }
    for (Foundation::Size at{0U}; at < length; ++at)
    {
        const auto c = name[at];
        if (c >= 0x80U)
        {
            continue;
        } // UTF-8 was validated above.
        const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        const bool interior = at != 0U && at + 1U != length && (c == '.' || c == '-' || c == '_');
        if (!alphanumeric && !interior)
        {
            return false;
        }
    }

    return true;
}
[[nodiscard, gnu::noinline]] inline bool ValidLeafName(const Foundation::UInt8* name, Foundation::Size length) noexcept
{
    return ValidName(name, length) && !(length == 2U && name[0U] == '.' && name[1U] == '.');
}
[[nodiscard, gnu::noinline]] inline Foundation::Size ToUtf16(const Foundation::UInt8* name, Foundation::Size length,
                                                             Foundation::UInt16* units) noexcept
{
    Foundation::Size count{0U};
    for (Foundation::Size at{0U}; at < length;)
    {
        const auto point = Text::NextCodePoint(name, length, at);
        if (point < 0x10000U)
        {
            units[count++] = static_cast<Foundation::UInt16>(point);
        }
        else
        {
            units[count++] = static_cast<Foundation::UInt16>(0xD800U + ((point - 0x10000U) >> 10U));
            units[count++] = static_cast<Foundation::UInt16>(0xDC00U + ((point - 0x10000U) & 0x3FFU));
        }
    }
    return count;
}
[[nodiscard, gnu::noinline]] inline bool ValidUtf16(const Foundation::UInt16* units, Foundation::Size count) noexcept
{
    for (Foundation::Size at{0U}; at < count; ++at)
    {
        if (units[at] == 0U)
        {
            return false;
        }
        if (units[at] >= 0xD800U && units[at] <= 0xDBFFU)
        {
            if (++at >= count || units[at] < 0xDC00U || units[at] > 0xDFFFU)
            {
                return false;
            }
        }
        else if (units[at] >= 0xDC00U && units[at] <= 0xDFFFU)
        {
            return false;
        }
    }
    return true;
}
[[nodiscard, gnu::noinline]] inline Foundation::Size FromUtf16(const Foundation::UInt16* units, Foundation::Size count,
                                                               Foundation::UInt8* name) noexcept
{
    Foundation::Size bytes{0U};
    for (Foundation::Size at{0U}; at < count; ++at)
    {
        Foundation::UInt32 point{units[at]};
        if ((point >= 0xD800U) && (point <= 0xDBFFU))
        {
            if ((++at >= count) || (units[at] < 0xDC00U) || (units[at] > 0xDFFFU))
            {
                return 0U;
            }
            point = 0x10000U + ((point - 0xD800U) << 10U) + units[at] - 0xDC00U;
        }
        else if ((point >= 0xDC00U) && (point <= 0xDFFFU))
        {
            return 0U;
        }
        const Foundation::Size needed = point < 0x80U ? 1U : (point < 0x800U ? 2U : (point < 0x10000U ? 3U : 4U));
        if ((point == 0U) || (bytes + needed > NameBytes))
        {
            return 0U;
        }
        bytes += Text::EncodeUtf8(point, name + bytes);
    }
    return bytes;
}
[[nodiscard, gnu::noinline]] inline bool SameName(const Foundation::UInt8* a, Foundation::Size an,
                                                  const Foundation::UInt8* b, Foundation::Size bn) noexcept
{
    if (an != bn)
    {
        return false;
    }
    for (Foundation::Size at{0U}; at < an; ++at)
    {
        const auto fold = [](Foundation::UInt8 c) noexcept
        {
            return ((c >= 'A') && (c <= 'Z')) ? c + 32U : c;
        };
        if (fold(a[at]) != fold(b[at]))
        {
            return false;
        }
    }
    return true;
}
} // Namespace Asteria::User::Filesystem
#endif
