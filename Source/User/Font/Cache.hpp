// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_CACHE_HPP_INCLUDED
#define ASTERIA_USER_FONT_CACHE_HPP_INCLUDED
#include "User/Font/Raster.hpp"
namespace Asteria::User::Font
{
struct CacheEntry
{
    Bytes Face{};
    UInt16 Glyph{};
    RasterOptions Options{};
    Mask Coverage{};
};
class GlyphCache final
{
public:
    [[nodiscard]] Error Configure(UInt8* bytes, Size size, CacheEntry* entries, UInt32 count) noexcept;
    void Clear() noexcept
    {
        m_used = 0U;
        m_count = 0U;
    }
    /// Returned masks survive until Clear/Configure or an arena eviction.
    [[nodiscard]] Error Get(const Font& font, UInt16 glyph, const RasterOptions& options,
                            OutlineWorkspace& outlineWorkspace, Outline& outline, RasterScratch& rasterWorkspace,
                            Mask& mask) noexcept;
    /// CFF-only owner with a small Type 2 workspace; shares cache/raster path.
    [[nodiscard]] Error GetCff(const Font& font, UInt16 glyph, const RasterOptions& options, Type2Workspace& workspace,
                               Outline& outline, RasterScratch& raster, Mask& mask) noexcept;
    [[nodiscard]] Size UsedBytes() const noexcept
    {
        return m_used;
    }
    [[nodiscard]] UInt32 Count() const noexcept
    {
        return m_count;
    }

private:
    [[nodiscard]] bool Find(const Font& font, UInt16 glyph, const RasterOptions& options, Mask& mask) noexcept;
    [[nodiscard]] Error Store(const Font& font, UInt16 glyph, const RasterOptions& options, Outline& outline,
                              RasterScratch& raster, Mask& mask) noexcept;
    UInt8* m_bytes{};
    Size m_size{};
    Size m_used{};
    CacheEntry* m_entries{};
    UInt32 m_capacity{};
    UInt32 m_count{};
};
} // namespace Asteria::User::Font
#endif
