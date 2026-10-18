// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_LAYOUT_HPP_INCLUDED
#define ASTERIA_USER_FONT_LAYOUT_HPP_INCLUDED
#include "User/Font/Font.hpp"
namespace Asteria::User::Font
{
struct Adjustment
{
    Int32 XPlacement{};
    Int32 YPlacement{};
    Int32 XAdvance{};
    Int32 YAdvance{};
};
struct PairAdjustment
{
    Adjustment First{};
    Adjustment Second{};
};
struct PositionedGlyph
{
    UInt16 Glyph{};
    UInt32 Cluster{};
    UInt32 ClusterEnd{};
    UInt32 Scalar{}; // Original UTF-8 byte range.
    Int64 X{};
    Int64 Y{};
    Int64 AdvanceX{};
    Int64 AdvanceY{}; // 16.16 pixels, y up.
};
struct Run
{
    PositionedGlyph* Glyphs{};
    UInt32 Capacity{};
    UInt32 Count{};
    Int64 AdvanceX{};
    Int64 AdvanceY{};
};
struct LayoutOptions
{
    UInt32 PixelSize{16U * static_cast<UInt32>(One)};
    bool Kerning{true};
    bool Ligatures{true};
};
struct LineMetrics
{
    Int64 Ascender{};
    Int64 Descender{};
    Int64 Gap{};
    Int64 Height{};
};
struct Line
{
    UInt32 First{};
    UInt32 Count{};
    Int64 Width{};
};
[[nodiscard]] Error Kerning(const Font& font, UInt16 left, UInt16 right, PairAdjustment& adjustment) noexcept;
[[nodiscard]] Error Shape(const Font& font, Bytes utf8, const LayoutOptions& options, Run& output) noexcept;
[[nodiscard]] LineMetrics GetLineMetrics(const Font& font, UInt32 pixelSize) noexcept;
[[nodiscard]] Error Measure(const Font& font, Bytes utf8, const LayoutOptions& options, Run& scratch, Int64& width,
                            Int64& height) noexcept;
/// Preserves clusters; LF is consumed as a line break. Positions in each line
/// remain run-relative: subtract the first glyph's pen position when drawing.
[[nodiscard]] Error Wrap(const Run& run, Int64 width, Line* lines, UInt32 capacity, UInt32& count) noexcept;
} // namespace Asteria::User::Font
#endif
