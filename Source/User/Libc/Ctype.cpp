// SPDX-License-Identifier: Apache-2.0

#include "Include/ctype.h"

namespace Asteria::User::Libc
{
namespace
{
[[nodiscard]] constexpr bool Between(const int value, const int low, const int high) noexcept
{
    return value >= low && value <= high;
}
}
}

extern "C" int ASTERIA_LIBC_NAME(isdigit)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, '0', '9');
}

extern "C" int ASTERIA_LIBC_NAME(islower)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, 'a', 'z');
}

extern "C" int ASTERIA_LIBC_NAME(isupper)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, 'A', 'Z');
}

extern "C" int ASTERIA_LIBC_NAME(isalpha)(const int character) noexcept
{
    return ASTERIA_LIBC_NAME(islower)(character) || ASTERIA_LIBC_NAME(isupper)(character);
}

extern "C" int ASTERIA_LIBC_NAME(isalnum)(const int character) noexcept
{
    return ASTERIA_LIBC_NAME(isalpha)(character) || ASTERIA_LIBC_NAME(isdigit)(character);
}

extern "C" int ASTERIA_LIBC_NAME(isxdigit)(const int character) noexcept
{
    return ASTERIA_LIBC_NAME(isdigit)(character) || Asteria::User::Libc::Between(character, 'a', 'f') ||
           Asteria::User::Libc::Between(character, 'A', 'F');
}

extern "C" int ASTERIA_LIBC_NAME(isspace)(const int character) noexcept
{
    return character == ' ' || Asteria::User::Libc::Between(character, '\t', '\r');
}

extern "C" int ASTERIA_LIBC_NAME(isblank)(const int character) noexcept
{
    return character == ' ' || character == '\t';
}

extern "C" int ASTERIA_LIBC_NAME(iscntrl)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, 0, 31) || character == 127;
}

extern "C" int ASTERIA_LIBC_NAME(isprint)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, 32, 126);
}

extern "C" int ASTERIA_LIBC_NAME(isgraph)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, 33, 126);
}

extern "C" int ASTERIA_LIBC_NAME(ispunct)(const int character) noexcept
{
    return ASTERIA_LIBC_NAME(isgraph)(character) && !ASTERIA_LIBC_NAME(isalnum)(character);
}

extern "C" int ASTERIA_LIBC_NAME(isascii)(const int character) noexcept
{
    return Asteria::User::Libc::Between(character, 0, 127);
}

extern "C" int ASTERIA_LIBC_NAME(toascii)(const int character) noexcept
{
    return character & 127;
}

extern "C" int ASTERIA_LIBC_NAME(tolower)(const int character) noexcept
{
    return ASTERIA_LIBC_NAME(isupper)(character) ? character + ('a' - 'A') : character;
}

extern "C" int ASTERIA_LIBC_NAME(toupper)(const int character) noexcept
{
    return ASTERIA_LIBC_NAME(islower)(character) ? character - ('a' - 'A') : character;
}
