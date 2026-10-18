// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_UI_TEXT_HPP_INCLUDED
#define ASTERIA_USER_SESSION_UI_TEXT_HPP_INCLUDED
#include "User/Font/Cache.hpp"
#include "User/Graphics/BitmapFont.hpp"
#include "User/Font/Layout.hpp"
#include "User/Session/Decorations.hpp"
namespace Asteria::User::Session
{
// UI default; terminal stays Bold8x16.
inline constexpr Foundation::UInt32 UiPixelSize{13U};
inline constexpr Foundation::UInt32 UiTextHeight{16U};
inline constexpr Foundation::UInt32 UiBaseline{12U};
inline constexpr Foundation::UInt32 UiOutlineCommands{40U};
inline constexpr Foundation::UInt32 UiRasterEdges{128U};
inline constexpr Foundation::UInt32 UiRasterCrossings{32U};
inline constexpr Foundation::UInt32 UiRasterColumns{32U};
inline constexpr Foundation::Size UiCacheBytes{1024U};
inline constexpr Foundation::UInt32 UiCacheEntries{8U};
static_assert(UiTextHeight + 2U * TitleTextTop == TitleBarHeight);
static_assert(UiOutlineCommands <= Font::MaximumCommands && UiRasterEdges <= Font::MaximumEdges);
/// Single owner, static storage on targets. CFF UI files are borrowed immutable
/// data; rejection of either face selects the bitmap fallback for that face.
class UiText final
{
public:
    UiText() noexcept = default;
    UiText(const UiText&) = delete;
    UiText& operator=(const UiText&) = delete;
    [[nodiscard]] bool Open(Font::Bytes bold, Font::Bytes regular) noexcept;
    [[nodiscard]] bool Scalable(bool bold) const noexcept;
    [[nodiscard]] Foundation::Size FittedPrefixBytes() const noexcept
    {
        return m_prefix;
    }
    [[nodiscard]] Foundation::Size FittedSuffixBytes() const noexcept
    {
        return m_suffix;
    }
    [[nodiscard]] Foundation::UInt32 CachedGlyphs() const noexcept
    {
        return m_cache.Count();
    }
    [[nodiscard]] Foundation::UInt32 Width(const Foundation::UInt8* text, Foundation::Size length, bool bold) noexcept;
    /// Draw inside box, including glyph overhang clipping. trustedEnd is the
    /// first subtitle byte; suffix bytes use trusted colour. VolumeCodePoint
    /// at the start is always the trusted bitmap marker. Overflow keeps the
    /// longest whole-cluster prefix + "..." + suffix when the suffix fits.
    void PaintTitle(const Graphics::MemorySurface& surface, Graphics::Rect clip, Graphics::Rect box,
                    const TitleView& title, Graphics::Color trusted, Graphics::Color subtitle,
                    Graphics::Color background) noexcept;
    void Paint(const Graphics::MemorySurface& surface, Graphics::Rect clip, Graphics::Rect box,
               const Foundation::UInt8* text, Foundation::Size length, bool bold, Foundation::Size trustedEnd,
               Foundation::Size suffixBytes, Graphics::Color trusted, Graphics::Color subtitle,
               Graphics::Color background) noexcept;

private:
    [[nodiscard]] bool OpenFace(Font::Font& face, Font::Bytes data) noexcept;
    [[nodiscard]] bool Shape(const Foundation::UInt8* text, Foundation::Size length, bool bold) noexcept;
    [[nodiscard]] Foundation::UInt32 MarkerWidth(bool bold) noexcept;
    [[nodiscard]] Foundation::Int64 Advance() const noexcept;
    [[nodiscard]] Foundation::Int64 Fit(const Foundation::UInt8* text, Foundation::Size length, bool bold,
                                        Foundation::UInt32 width, Foundation::Size suffixBytes) noexcept;
    void Draw(const Graphics::MemorySurface& surface, Graphics::Point origin, bool bold, Foundation::Size trustedEnd,
              Foundation::Size suffixStart, Graphics::Color trusted, Graphics::Color subtitle,
              Graphics::Color background) noexcept;
    void Bitmap(const Graphics::MemorySurface& surface, Graphics::Point point, Foundation::UInt32 scalar,
                Graphics::Color foreground, Graphics::Color background) noexcept;
    Graphics::Rect m_drawClip{};
    Font::Font m_bold{};
    Font::Font m_regular{};
    Font::GlyphCache m_cache{};
    Font::Type2Workspace m_type2{};
    Font::Command m_commands[UiOutlineCommands]{};
    Font::Edge m_edges[UiRasterEdges]{};
    Font::Crossing m_crossings[UiRasterCrossings]{};
    Foundation::UInt16 m_samples[UiRasterColumns]{};
    Font::RasterScratch m_raster{};
    Font::CacheEntry m_entries[UiCacheEntries]{};
    Foundation::UInt8 m_bytes[UiCacheBytes]{};
    Font::PositionedGlyph m_glyphs[TitleTextBytes]{};
    Font::Run m_run{};
    Foundation::UInt32 m_ends[TitleTextBytes]{};
    Foundation::UInt8 m_title[TitleTextBytes]{};
    Foundation::UInt8 m_fitted[TitleTextBytes]{};
    Foundation::Size m_length{};
    Foundation::Size m_prefix{};
    Foundation::Size m_suffix{};
};
} // Namespace Asteria::User::Session
#endif
