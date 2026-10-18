// SPDX-License-Identifier: Apache-2.0

#include "Memory.hpp"

extern "C"
{
void* memcpy(void* const destination, const void* const source, const Asteria::Foundation::Size countBytes) noexcept
{
    auto* const destinationBytes = static_cast<Asteria::Foundation::UInt8*>(destination);
    const auto* const sourceBytes = static_cast<const Asteria::Foundation::UInt8*>(source);

    for (Asteria::Foundation::Size index{0U}; index < countBytes; ++index)
    {
        destinationBytes[index] = sourceBytes[index];
    }

    return destination;
}

void* memmove(void* const destination, const void* const source, const Asteria::Foundation::Size countBytes) noexcept
{
    auto* const destinationBytes = static_cast<Asteria::Foundation::UInt8*>(destination);
    const auto* const sourceBytes = static_cast<const Asteria::Foundation::UInt8*>(source);

    if ((destinationBytes == sourceBytes) || (countBytes == 0U))
    {
        return destination;
    }

    // Supported targets use a flat address space whose pointer-to-integer mapping preserves
    // byte-address order for pointers into the same allocated object.
    const Asteria::Foundation::UIntPointer destinationAddress =
        reinterpret_cast<Asteria::Foundation::UIntPointer>(destinationBytes);
    const Asteria::Foundation::UIntPointer sourceAddress =
        reinterpret_cast<Asteria::Foundation::UIntPointer>(sourceBytes);

    if (destinationAddress < sourceAddress)
    {
        for (Asteria::Foundation::Size index{0U}; index < countBytes; ++index)
        {
            destinationBytes[index] = sourceBytes[index];
        }
    }
    else
    {
        for (Asteria::Foundation::Size remaining{countBytes}; remaining > 0U; --remaining)
        {
            const Asteria::Foundation::Size index = remaining - 1U;
            destinationBytes[index] = sourceBytes[index];
        }
    }

    return destination;
}

void* memset(void* const destination, const int value, const Asteria::Foundation::Size countBytes) noexcept
{
    auto* const destinationBytes = static_cast<Asteria::Foundation::UInt8*>(destination);
    const auto byteValue = static_cast<Asteria::Foundation::UInt8>(value);

    for (Asteria::Foundation::Size index{0U}; index < countBytes; ++index)
    {
        destinationBytes[index] = byteValue;
    }

    return destination;
}

int memcmp(const void* const first, const void* const second, const Asteria::Foundation::Size countBytes) noexcept
{
    const auto* const firstBytes = static_cast<const Asteria::Foundation::UInt8*>(first);
    const auto* const secondBytes = static_cast<const Asteria::Foundation::UInt8*>(second);

    for (Asteria::Foundation::Size index{0U}; index < countBytes; ++index)
    {
        if (firstBytes[index] < secondBytes[index])
        {
            return -1;
        }

        if (firstBytes[index] > secondBytes[index])
        {
            return 1;
        }
    }

    return 0;
}
}
