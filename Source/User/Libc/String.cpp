// SPDX-License-Identifier: Apache-2.0

#include "Include/string.h"

#include "Include/strings.h"
#include "Include/ctype.h"
#include "Include/stdlib.h"
#include "Include/errno.h"
#include "LibcHooks.hpp"

namespace Asteria::User::Libc
{
namespace
{
char* g_token{nullptr};
// External C API returns non-const borrows; callers may write only if the original is mutable.
[[nodiscard]] char* Mutable(const char* const text) noexcept
{
    return const_cast<char*>(text);
}
}
}

extern "C" void* ASTERIA_LIBC_NAME(memcpy)(void* const destination, const void* const source,
                                           const size_t count) noexcept
{
    auto* const output = static_cast<unsigned char*>(destination);
    const auto* const input = static_cast<const unsigned char*>(source);
    for (size_t index{0U}; index < count; ++index)
    {
        output[index] = input[index];
    }
    return destination;
}

extern "C" void* ASTERIA_LIBC_NAME(memmove)(void* const destination, const void* const source,
                                            const size_t count) noexcept
{
    auto* const output = static_cast<unsigned char*>(destination);
    const auto* const input = static_cast<const unsigned char*>(source);
    // ABI pointer ordering: integer addresses on the flat target address spaces.
    if (reinterpret_cast<uintptr_t>(output) < reinterpret_cast<uintptr_t>(input))
    {
        return ASTERIA_LIBC_NAME(memcpy)(destination, source, count);
    }
    for (size_t index{count}; index != 0U; --index)
    {
        output[index - 1U] = input[index - 1U];
    }
    return destination;
}

extern "C" void* ASTERIA_LIBC_NAME(memset)(void* const destination, const int value, const size_t count) noexcept
{
    auto* const output = static_cast<unsigned char*>(destination);
    for (size_t index{0U}; index < count; ++index)
    {
        output[index] = static_cast<unsigned char>(static_cast<unsigned int>(value) & 255U);
    }
    return destination;
}

extern "C" int ASTERIA_LIBC_NAME(memcmp)(const void* const left, const void* const right, const size_t count) noexcept
{
    const auto* const a = static_cast<const unsigned char*>(left);
    const auto* const b = static_cast<const unsigned char*>(right);
    for (size_t index{0U}; index < count; ++index)
    {
        if (a[index] != b[index])
        {
            return static_cast<int>(a[index]) - static_cast<int>(b[index]);
        }
    }
    return 0;
}

extern "C" void* ASTERIA_LIBC_NAME(memchr)(const void* const data, const int value, const size_t count) noexcept
{
    const auto* const bytes = static_cast<const unsigned char*>(data);
    const auto wanted = static_cast<unsigned char>(static_cast<unsigned int>(value) & 255U);
    for (size_t index{0U}; index < count; ++index)
    {
        if (bytes[index] == wanted)
        {
            return const_cast<unsigned char*>(bytes + index);
        }
    }
    return nullptr;
}

extern "C" size_t ASTERIA_LIBC_NAME(strlen)(const char* const text) noexcept
{
    size_t length{0U};
    while (text[length] != '\0')
    {
        ++length;
    }
    return length;
}

extern "C" size_t ASTERIA_LIBC_NAME(strnlen)(const char* const text, const size_t count) noexcept
{
    size_t length{0U};
    while (length < count && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

extern "C" char* ASTERIA_LIBC_NAME(strcpy)(char* const destination, const char* const source) noexcept
{
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(destination, source, ASTERIA_LIBC_NAME(strlen)(source) + 1U));
    return destination;
}

extern "C" char* ASTERIA_LIBC_NAME(strncpy)(char* const destination, const char* const source,
                                            const size_t count) noexcept
{
    const size_t length = ASTERIA_LIBC_NAME(strnlen)(source, count);
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(destination, source, length));
    static_cast<void>(ASTERIA_LIBC_NAME(memset)(destination + length, 0, count - length));
    return destination;
}

extern "C" char* ASTERIA_LIBC_NAME(strcat)(char* const destination, const char* const source) noexcept
{
    static_cast<void>(ASTERIA_LIBC_NAME(strcpy)(destination + ASTERIA_LIBC_NAME(strlen)(destination), source));
    return destination;
}

extern "C" char* ASTERIA_LIBC_NAME(strncat)(char* const destination, const char* const source,
                                            const size_t count) noexcept
{
    const size_t offset = ASTERIA_LIBC_NAME(strlen)(destination);
    const size_t length = ASTERIA_LIBC_NAME(strnlen)(source, count);
    static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(destination + offset, source, length));
    destination[offset + length] = '\0';
    return destination;
}

extern "C" int ASTERIA_LIBC_NAME(strncmp)(const char* const left, const char* const right, const size_t count) noexcept
{
    for (size_t index{0U}; index < count; ++index)
    {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (a != b || a == 0U)
        {
            return static_cast<int>(a) - static_cast<int>(b);
        }
    }
    return 0;
}

extern "C" int ASTERIA_LIBC_NAME(strcmp)(const char* const left, const char* const right) noexcept
{
    return ASTERIA_LIBC_NAME(strncmp)(left, right, SIZE_MAX);
}

extern "C" int ASTERIA_LIBC_NAME(strcoll)(const char* const left, const char* const right) noexcept
{
    return ASTERIA_LIBC_NAME(strcmp)(left, right);
}

extern "C" size_t ASTERIA_LIBC_NAME(strxfrm)(char* const destination, const char* const source,
                                             const size_t count) noexcept
{
    const size_t length = ASTERIA_LIBC_NAME(strlen)(source);
    if (count != 0U)
    {
        static_cast<void>(ASTERIA_LIBC_NAME(strncpy)(destination, source, count));
    }
    return length;
}

extern "C" char* ASTERIA_LIBC_NAME(strchr)(const char* const text, const int value) noexcept
{
    const auto wanted = static_cast<unsigned char>(static_cast<unsigned int>(value) & 255U);
    for (size_t index{0U};; ++index)
    {
        if (static_cast<unsigned char>(text[index]) == wanted)
        {
            return Asteria::User::Libc::Mutable(text + index);
        }
        if (text[index] == '\0')
        {
            return nullptr;
        }
    }
}

extern "C" char* ASTERIA_LIBC_NAME(strrchr)(const char* const text, const int value) noexcept
{
    char* result{nullptr};
    const auto wanted = static_cast<unsigned char>(static_cast<unsigned int>(value) & 255U);
    for (size_t index{0U};; ++index)
    {
        if (static_cast<unsigned char>(text[index]) == wanted)
        {
            result = Asteria::User::Libc::Mutable(text + index);
        }
        if (text[index] == '\0')
        {
            return result;
        }
    }
}

extern "C" size_t ASTERIA_LIBC_NAME(strspn)(const char* const text, const char* const accept) noexcept
{
    size_t count{0U};
    while (text[count] != '\0' && ASTERIA_LIBC_NAME(strchr)(accept, text[count]) != nullptr)
    {
        ++count;
    }
    return count;
}

extern "C" size_t ASTERIA_LIBC_NAME(strcspn)(const char* const text, const char* const reject) noexcept
{
    size_t count{0U};
    while (text[count] != '\0' && ASTERIA_LIBC_NAME(strchr)(reject, text[count]) == nullptr)
    {
        ++count;
    }
    return count;
}

extern "C" char* ASTERIA_LIBC_NAME(strpbrk)(const char* const text, const char* const accept) noexcept
{
    const size_t offset = ASTERIA_LIBC_NAME(strcspn)(text, accept);
    return text[offset] != '\0' ? Asteria::User::Libc::Mutable(text + offset) : nullptr;
}

extern "C" char* ASTERIA_LIBC_NAME(strstr)(const char* const text, const char* const needle) noexcept
{
    if (*needle == '\0')
    {
        return Asteria::User::Libc::Mutable(text);
    }
    const size_t length = ASTERIA_LIBC_NAME(strlen)(needle);
    for (const char* cursor{text}; *cursor != '\0'; ++cursor)
    {
        if (ASTERIA_LIBC_NAME(strncmp)(cursor, needle, length) == 0)
        {
            return Asteria::User::Libc::Mutable(cursor);
        }
    }
    return nullptr;
}

extern "C" char* ASTERIA_LIBC_NAME(strtok_r)(char* text, const char* const delimiters, char** const state) noexcept
{
    if (text == nullptr)
    {
        text = *state;
    }
    if (text == nullptr)
    {
        return nullptr;
    }
    text += ASTERIA_LIBC_NAME(strspn)(text, delimiters);
    if (*text == '\0')
    {
        *state = text;
        return nullptr;
    }
    char* const end = text + ASTERIA_LIBC_NAME(strcspn)(text, delimiters);
    *state = end;
    if (*end != '\0')
    {
        *end = '\0';
        *state = end + 1U;
    }
    return text;
}

extern "C" char* ASTERIA_LIBC_NAME(strtok)(char* const text, const char* const delimiters) noexcept
{
    return ASTERIA_LIBC_NAME(strtok_r)(text, delimiters, &Asteria::User::Libc::g_token);
}

extern "C" char* ASTERIA_LIBC_NAME(strndup)(const char* const text, const size_t count) noexcept
{
    const size_t length = ASTERIA_LIBC_NAME(strnlen)(text, count);
    if (length == SIZE_MAX)
    {
        Asteria::User::Libc::SetError(ENOMEM);
        return nullptr;
    }
    auto* const result = static_cast<char*>(ASTERIA_LIBC_NAME(malloc)(length + 1U));
    if (result != nullptr)
    {
        static_cast<void>(ASTERIA_LIBC_NAME(memcpy)(result, text, length));
        result[length] = '\0';
    }
    return result;
}

extern "C" char* ASTERIA_LIBC_NAME(strdup)(const char* const text) noexcept
{
    return ASTERIA_LIBC_NAME(strndup)(text, SIZE_MAX);
}

extern "C" int ASTERIA_LIBC_NAME(strncasecmp)(const char* const left, const char* const right,
                                              const size_t count) noexcept
{
    for (size_t index{0U}; index < count; ++index)
    {
        const int a = ASTERIA_LIBC_NAME(tolower)(static_cast<unsigned char>(left[index]));
        const int b = ASTERIA_LIBC_NAME(tolower)(static_cast<unsigned char>(right[index]));
        if (a != b || a == 0)
        {
            return a - b;
        }
    }
    return 0;
}

extern "C" int ASTERIA_LIBC_NAME(strcasecmp)(const char* const left, const char* const right) noexcept
{
    return ASTERIA_LIBC_NAME(strncasecmp)(left, right, SIZE_MAX);
}
