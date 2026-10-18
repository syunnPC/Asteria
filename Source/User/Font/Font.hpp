// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_FONT_HPP_INCLUDED
#define ASTERIA_USER_FONT_FONT_HPP_INCLUDED
#include "User/Font/Limits.hpp"
namespace Asteria::User::Font
{
struct Metrics
{
    UInt16 UnitsPerEm{};
    UInt16 GlyphCount{};
    UInt16 HorizontalMetrics{};
    UInt16 Weight{};
    UInt16 Width{};
    Int16 Ascender{};
    Int16 Descender{};
    Int16 LineGap{};
    Int16 TypoAscender{};
    Int16 TypoDescender{};
    Int16 TypoLineGap{};
    UInt16 WinAscender{};
    UInt16 WinDescender{};
    Int16 XHeight{};
    Int16 CapHeight{};
    Int16 UnderlinePosition{};
    Int16 UnderlineThickness{};
    Int32 ItalicAngle{}; // 16.16 degrees, as stored in post.
    bool FixedPitch{};
    bool UseTypoMetrics{};
};
struct GlyphMetrics
{
    UInt16 Advance{};
    Int16 Bearing{};
};
// Adobe #5176 section 5: validated one-based INDEX offsets, borrowed data.
struct Index
{
    Bytes Offsets{};
    Bytes Objects{};
    UInt32 Count{};
    UInt32 OffsetSize{};
    [[nodiscard]] Bytes Object(UInt32 index) const noexcept;
};
class Font final
{
public:
    /// A failed open clears the font. Storage is borrowed and must stay immutable.
    [[nodiscard]] Error Open(Bytes data) noexcept;
    [[nodiscard]] bool IsOpen() const noexcept
    {
        return m_open;
    }
    [[nodiscard]] Bytes Identity() const noexcept
    {
        return m_open ? m_data : Bytes{};
    }
    [[nodiscard]] bool IsCff() const noexcept
    {
        return m_cff;
    }
    [[nodiscard]] const Metrics& GetMetrics() const noexcept
    {
        return m_metrics;
    }
    [[nodiscard]] UInt16 Glyph(UInt32 scalar) const noexcept;
    [[nodiscard]] GlyphMetrics Horizontal(UInt16 glyph) const noexcept;
    /// Unicode name IDs 1 (family) and 2 (style), converted at the UTF-16 edge.
    /// Returns byte count excluding NUL; no NUL is written. Missing name => zero.
    [[nodiscard]] Error Name(UInt16 id, UInt8* output, Size capacity, Size& length) const noexcept;
    [[nodiscard]] Error FamilyName(UInt8* output, Size capacity, Size& length) const noexcept;
    [[nodiscard]] Error StyleName(UInt8* output, Size capacity, Size& length) const noexcept;
    [[nodiscard]] Bytes Table(UInt32 tag) const noexcept;
    [[nodiscard]] const Index& Charstrings() const noexcept
    {
        return m_charstrings;
    }
    [[nodiscard]] const Index& GlobalSubroutines() const noexcept
    {
        return m_globals;
    }
    [[nodiscard]] const Index& LocalSubroutines() const noexcept
    {
        return m_locals;
    }
    [[nodiscard]] Int64 DefaultWidth() const noexcept
    {
        return m_defaultWidth;
    }
    [[nodiscard]] Int64 NominalWidth() const noexcept
    {
        return m_nominalWidth;
    }
    [[nodiscard]] UInt32 GlyphOffset(UInt32 glyph) const noexcept;

private:
    [[nodiscard]] Error OpenMetrics() noexcept;
    [[nodiscard]] Error OpenCmap() noexcept;
    [[nodiscard]] Error OpenNames() noexcept;
    [[nodiscard]] Error OpenCff() noexcept;
    Bytes m_data{};
    Bytes m_cmap{};
    Metrics m_metrics{};
    Index m_charstrings{};
    Index m_globals{};
    Index m_locals{};
    Int64 m_defaultWidth{};
    Int64 m_nominalWidth{};
    UInt16 m_tableCount{};
    UInt16 m_locaFormat{};
    bool m_open{};
    bool m_cff{};
};
} // namespace Asteria::User::Font
#endif
