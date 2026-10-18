// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_FOUNDATION_TYPES_HPP_INCLUDED
#define ASTERIA_FOUNDATION_TYPES_HPP_INCLUDED

namespace Asteria::Foundation
{
using Int8 = signed char;
using UInt8 = unsigned char;
using Int16 = signed short;
using UInt16 = unsigned short;
using Int32 = signed int;
using UInt32 = unsigned int;
using Int64 = signed long long;
using UInt64 = unsigned long long;
// Processor sets never narrow to a byte.
using ProcessorMask = UInt64;
using UIntPointer = unsigned long long;
using Size = decltype(sizeof(0));

static_assert(static_cast<UInt8>(~UInt8{0U}) == UInt8{0xFFU});
static_assert(sizeof(Int8) == 1U);
static_assert(sizeof(UInt8) == 1U);
static_assert(sizeof(Int16) == 2U);
static_assert(sizeof(UInt16) == 2U);
static_assert(sizeof(Int32) == 4U);
static_assert(sizeof(UInt32) == 4U);
static_assert(sizeof(Int64) == 8U);
static_assert(sizeof(UInt64) == 8U);
static_assert(sizeof(ProcessorMask) == 8U);
static_assert(sizeof(UIntPointer) == sizeof(void*));
}

#endif // ASTERIA_FOUNDATION_TYPES_HPP_INCLUDED
