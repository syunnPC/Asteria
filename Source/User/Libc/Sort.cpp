// SPDX-License-Identifier: Apache-2.0

#include "Include/stdlib.h"

#include <stdint.h>

namespace Asteria::User::Libc
{
namespace
{
void Swap(unsigned char* const left, unsigned char* const right, const size_t size) noexcept
{
    for (size_t index{0U}; index < size; ++index)
    {
        const unsigned char saved = left[index];
        left[index] = right[index];
        right[index] = saved;
    }
}

void Sift(unsigned char* const base, size_t root, const size_t count, const size_t size,
          int (*const compare)(const void*, const void*)) noexcept
{
    while (root < count / 2U)
    {
        size_t child{root * 2U + 1U};
        if (child + 1U < count && compare(base + child * size, base + (child + 1U) * size) < 0)
        {
            ++child;
        }
        if (compare(base + root * size, base + child * size) >= 0)
        {
            return;
        }
        Swap(base + root * size, base + child * size, size);
        root = child;
    }
}
}
}

extern "C" void ASTERIA_LIBC_NAME(qsort)(void* const base, const size_t count, const size_t size,
                                         int (*const compare)(const void*, const void*)) noexcept
{
    if (size == 0U || count < 2U || count > SIZE_MAX / size)
    {
        return;
    }
    auto* const bytes = static_cast<unsigned char*>(base);
    // In-place heapsort has O(n log n) worst-case comparisons and O(1) storage.
    for (size_t root{count / 2U}; root != 0U; --root)
    {
        Asteria::User::Libc::Sift(bytes, root - 1U, count, size, compare);
    }
    for (size_t remaining{count - 1U}; remaining != 0U; --remaining)
    {
        Asteria::User::Libc::Swap(bytes, bytes + remaining * size, size);
        Asteria::User::Libc::Sift(bytes, 0U, remaining, size, compare);
    }
}

extern "C" void* ASTERIA_LIBC_NAME(bsearch)(const void* const key, const void* const base, size_t count,
                                            const size_t size, int (*const compare)(const void*, const void*)) noexcept
{
    if (size == 0U || count > SIZE_MAX / size)
    {
        return nullptr;
    }
    const auto* bytes = static_cast<const unsigned char*>(base);
    while (count != 0U)
    {
        const size_t middle{count / 2U};
        const unsigned char* const candidate = bytes + middle * size;
        const int order = compare(key, candidate);
        if (order == 0)
        {
            return const_cast<unsigned char*>(candidate); // C API borrow; mutability depends on the caller-owned object.
        }
        if (order > 0)
        {
            bytes = candidate + size;
            count -= middle + 1U;
        }
        else
        {
            count = middle;
        }
    }
    return nullptr;
}
