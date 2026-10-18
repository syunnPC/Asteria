// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_LIBC_FLOAT_HPP_INCLUDED
#define ASTERIA_USER_LIBC_FLOAT_HPP_INCLUDED

#include <stddef.h>
#include <stdint.h>

namespace Asteria::User::Libc
{
// ConversionLock protects the bounded workspace. Values are IEEE binary32/binary64.
struct DecimalView
{
    const char* Digits;
    size_t Length;
    int Point;
    int Precision;
    bool Scientific;
};
[[nodiscard]] DecimalView Decimal(uint64_t bits, char conversion, int precision, bool alternate) noexcept;
[[nodiscard]] uint64_t DoubleBits(double value) noexcept;
[[nodiscard]] uint64_t LongDoubleBits(const long double& value) noexcept;
[[nodiscard]] uint64_t ParseFloat(const char* text, char** end, bool single) noexcept;
}

#endif
