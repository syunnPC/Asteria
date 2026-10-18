// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_TEXT_WRITER_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_TEXT_WRITER_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Diagnostics
{
/// Fixed-width terminal text formatting; T::Put consumes one ASCII byte.
/// The sink performs clipping and physical access after formatting.
template <class T> class StopTextWriter
{
public:
    void Text(const char* text) noexcept
    {
        for (; *text != '\0'; ++text)
        {
            static_cast<T*>(this)->Put(static_cast<Foundation::UInt8>(*text));
        }
    }
    void Code(const Foundation::UInt32 value) noexcept
    {
        Text("0x");
        constexpr char digits[]{"0123456789ABCDEF"};
        for (Foundation::Size i{0U}; i < 8U; ++i)
        {
            static_cast<T*>(this)->Put(static_cast<Foundation::UInt8>(digits[(value >> ((7U - i) * 4U)) & 15U]));
        }
    }
    void Hex(const Foundation::UInt64 value) noexcept
    {
        Text("0x");
        constexpr char digits[]{"0123456789ABCDEF"};
        for (Foundation::Size i{0U}; i < 16U; ++i)
        {
            static_cast<T*>(this)->Put(static_cast<Foundation::UInt8>(digits[(value >> ((15U - i) * 4U)) & 15U]));
        }
    }
    void Decimal(const Foundation::UInt64 value) noexcept
    {
        char digits[21U]{};
        Foundation::Size length{0U};
        Foundation::UInt64 remaining{value};
        do
        {
            digits[length++] = static_cast<char>('0' + remaining % 10U);
            remaining /= 10U;
        } while (remaining != 0U);
        while (length != 0U)
        {
            static_cast<T*>(this)->Put(static_cast<Foundation::UInt8>(digits[--length]));
        }
    }
};
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_TEXT_WRITER_HPP_INCLUDED
