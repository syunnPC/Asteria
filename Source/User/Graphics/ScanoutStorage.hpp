// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_GRAPHICS_SCANOUT_STORAGE_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_SCANOUT_STORAGE_HPP_INCLUDED

#include "User/Graphics/Surface.hpp"

namespace Asteria::User::Graphics
{
/// MMIO adapter for little-endian RGBX/BGRX scanout accepting aligned four- and eight-byte writes.
/// No loads, allocation or ordering; the caller fences once after the complete pass. A row's
/// leading/trailing pixel uses a 32-bit store. GNU may_alias is confined to this adapter: host
/// pixel arrays and device storage can both be accessed without violating the compiler's alias
/// rules.
class ScanoutStorage final
{
public:
    struct Stores
    {
        Foundation::UInt64 Narrow{0U};
        Foundation::UInt64 Wide{0U};
    };

    static Stores WriteRow(volatile Foundation::UInt32* destination, const Foundation::UInt32* source,
                           Foundation::UInt32 pixels) noexcept
    {
        Stores stores{};
        if ((pixels != 0U) && ((reinterpret_cast<Foundation::UIntPointer>(destination) & 7U) != 0U))
        {
            *destination++ = *source++;
            --pixels;
            ++stores.Narrow;
        }
        const Foundation::UInt32 pairs = pixels >> 1U;
        // Destination is now 8-byte aligned; source needs only 4-byte alignment.
        typedef Foundation::UInt64 WideWord [[gnu::may_alias]];
        volatile WideWord* const wide =
            reinterpret_cast<volatile WideWord*>(reinterpret_cast<Foundation::UIntPointer>(destination));
        for (Foundation::UInt32 index{0U}; index < pairs; ++index)
        {
            wide[index] = source[index * 2U] | (static_cast<Foundation::UInt64>(source[index * 2U + 1U]) << 32U);
        }
        stores.Wide = pairs;
        if ((pixels & 1U) != 0U)
        {
            destination[pairs * 2U] = source[pairs * 2U];
            ++stores.Narrow;
        }
        return stores;
    }
};
} // Namespace Asteria::User::Graphics
#endif
