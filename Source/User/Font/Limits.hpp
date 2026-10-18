// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_LIMITS_HPP_INCLUDED
#define ASTERIA_USER_FONT_LIMITS_HPP_INCLUDED
#include "Foundation/Types.hpp"
namespace Asteria::User::Font
{
using Foundation::Int8;
using Foundation::UInt8;
using Foundation::Int16;
using Foundation::UInt16;
using Foundation::Int32;
using Foundation::UInt32;
using Foundation::Int64;
using Foundation::UInt64;
using Foundation::Size;
// Fonts are data, with bounded processing.
inline constexpr Size MaximumFontBytes = 64U * 1024U * 1024U;
inline constexpr UInt32 MaximumTables = 128U;
inline constexpr UInt32 MaximumGlyphs = 65535U;
inline constexpr UInt32 MaximumCommands = 4096U;
inline constexpr UInt32 MaximumPoints = 4096U;
inline constexpr UInt32 MaximumCompositeDepth = 16U;
inline constexpr UInt32 MaximumComponents = 128U;
inline constexpr UInt32 MaximumSubroutineDepth = 10U;
inline constexpr UInt32 MaximumOperands = 48U;
inline constexpr UInt32 MaximumTransient = 32U;
inline constexpr UInt32 MaximumStems = 96U;
inline constexpr UInt32 MaximumTokens = 65536U;
inline constexpr UInt32 MaximumProgramBytes = 65536U;
inline constexpr UInt32 MaximumCurveDepth = 16U;
inline constexpr UInt32 MaximumEdges = 16384U;
inline constexpr UInt32 MaximumMaskAxis = 512U;
inline constexpr UInt32 MaximumRunGlyphs = 4096U;
inline constexpr UInt32 MaximumTextBytes = 65536U;
inline constexpr UInt32 MaximumLayoutItems = 4096U;
inline constexpr UInt32 MaximumLigatureComponents = 64U;
inline constexpr UInt32 MaximumValidationWork = 4U * 1024U * 1024U;
inline constexpr UInt32 MaximumRasterWork = 16U * 1024U * 1024U;
inline constexpr Size MaximumCacheBytes = 1024U * 1024U;
inline constexpr UInt32 MaximumCacheEntries = 256U;
inline constexpr Int64 One = 65536;
inline constexpr Int64 MaximumCoordinate = 1048576LL * One;
inline constexpr Int64 MaximumPixelCoordinate = 32768LL * One;
inline constexpr UInt32 CoverageSamples = 8U;
inline constexpr Int64 MaximumGlyphAdvance =
    (65535LL + 2LL * MaximumLayoutItems * 32768LL) * MaximumMaskAxis * One / 16;
static_assert(MaximumMaskAxis * MaximumMaskAxis <= MaximumCacheBytes);
static_assert(MaximumSubroutineDepth + 1U == 11U);
static_assert(MaximumCoordinate * MaximumMaskAxis * One <= 0x7FFFFFFFFFFFFFFFLL);
static_assert(CoverageSamples * CoverageSamples <= 65535U);
static_assert(MaximumGlyphAdvance * MaximumRunGlyphs <= 0x7FFFFFFFFFFFFFFFLL);
enum class Error : UInt8
{
    None,
    InvalidArgument,
    Truncated,
    Malformed,
    Unsupported,
    Limit,
    BufferTooSmall,
    InvalidGlyph,
    StackUnderflow,
    StackOverflow,
    RecursionLimit,
    OperationLimit,
    CoordinateLimit,
    CacheTooSmall
};
struct Bytes
{
    const UInt8* Data{};
    Size Length{};
    [[nodiscard]] constexpr bool Contains(const Size at, const Size count) const noexcept
    {
        return (Data != nullptr) && (at <= Length) && (count <= Length - at);
    }
    [[nodiscard]] constexpr Bytes Slice(const Size at, const Size count) const noexcept
    {
        return Contains(at, count) ? Bytes{Data + at, count} : Bytes{};
    }
};
[[nodiscard]] constexpr UInt32 Tag(const char a, const char b, const char c, const char d) noexcept
{
    return (static_cast<UInt32>(a) << 24U) | (static_cast<UInt32>(b) << 16U) | (static_cast<UInt32>(c) << 8U) |
           static_cast<UInt32>(d);
}
} // Namespace Asteria::User::Font
#endif
