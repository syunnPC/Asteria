// SPDX-License-Identifier: Apache-2.0

#include "User/Font/Cache.hpp"
namespace Asteria::User::Font
{
Error GlyphCache::Configure(UInt8* const bytes, const Size size, CacheEntry* const entries, const UInt32 count) noexcept
{
    if (bytes == nullptr || entries == nullptr || size == 0U || size > MaximumCacheBytes || count == 0U ||
        count > MaximumCacheEntries)
    {
        return Error::InvalidArgument;
    }
    m_bytes = bytes;
    m_size = size;
    m_entries = entries;
    m_capacity = count;
    Clear();
    return Error::None;
}
Error GlyphCache::Get(const Font& font, const UInt16 glyph, const RasterOptions& options, OutlineWorkspace& ow,
                      Outline& outline, RasterScratch& rw, Mask& mask) noexcept
{
    mask = {};
    if (m_bytes == nullptr || !font.IsOpen())
    {
        return Error::InvalidArgument;
    }
    if (Find(font, glyph, options, mask))
    {
        return Error::None;
    }
    const Error error{LoadOutline(font, glyph, ow, outline)};
    return error == Error::None ? Store(font, glyph, options, outline, rw, mask) : error;
}
Error GlyphCache::GetCff(const Font& font, const UInt16 glyph, const RasterOptions& options, Type2Workspace& w,
                         Outline& outline, RasterScratch& rw, Mask& mask) noexcept
{
    mask = {};
    if (m_bytes == nullptr || !font.IsOpen() || !font.IsCff())
    {
        return Error::Unsupported;
    }
    if (glyph >= font.GetMetrics().GlyphCount)
    {
        return Error::InvalidGlyph;
    }
    if (Find(font, glyph, options, mask))
    {
        return Error::None;
    }
    outline.Count = 0U;
    const Error error{InterpretType2(font.Charstrings().Object(glyph), font.LocalSubroutines(),
                                     font.GlobalSubroutines(), font.DefaultWidth(), font.NominalWidth(), w, outline)};
    return error == Error::None ? Store(font, glyph, options, outline, rw, mask) : error;
}
bool GlyphCache::Find(const Font& font, const UInt16 glyph, const RasterOptions& options, Mask& mask) noexcept
{
    const Bytes face{font.Identity()};
    for (UInt32 i{}; i < m_count; ++i)
    {
        const auto& e = m_entries[i];
        if (e.Face.Data == face.Data && e.Face.Length == face.Length && e.Glyph == glyph &&
            e.Options.PixelSize == options.PixelSize && e.Options.OffsetX == options.OffsetX &&
            e.Options.OffsetY == options.OffsetY)
        {
            mask = e.Coverage;
            return true;
        }
    }
    return false;
}
Error GlyphCache::Store(const Font& font, const UInt16 glyph, const RasterOptions& options, Outline& outline,
                        RasterScratch& rw, Mask& mask) noexcept
{
    const Bytes face{font.Identity()};
    if (m_count == m_capacity)
    {
        Clear();
    }
    Error error =
        Rasterize(outline, font.GetMetrics().UnitsPerEm, options, rw, m_bytes + m_used, m_size - m_used, mask);
    if (error == Error::BufferTooSmall && m_used != 0U)
    {
        Clear();
        error = Rasterize(outline, font.GetMetrics().UnitsPerEm, options, rw, m_bytes, m_size, mask);
    }
    if (error == Error::BufferTooSmall)
    {
        return Error::CacheTooSmall;
    }
    if (error != Error::None)
    {
        return error;
    }
    mask.Length = static_cast<Size>(mask.Width) * mask.Height;
    m_entries[m_count++] = {face, glyph, options, mask};
    m_used += mask.Length;
    return Error::None;
}
} // namespace Asteria::User::Font
