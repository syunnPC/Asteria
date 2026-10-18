// SPDX-License-Identifier: Apache-2.0
#include "User/Font/Cache.hpp"
#include "User/Font/Draw.hpp"
#include "User/Font/Layout.hpp"
#include "Support/TestContext.hpp"
#include "User/Font/Fixture.hpp"
#include <cstring>
using Asteria::User::Font::Bytes;
using Asteria::User::Font::CacheEntry;
using Asteria::User::Font::Command;
using Asteria::User::Font::CommandKind;
using Asteria::User::Font::DrawMask;
using Asteria::User::Font::Error;
using Asteria::User::Font::Font;
using Asteria::User::Font::GetLineMetrics;
using Asteria::User::Font::GlyphCache;
using Asteria::User::Font::Int64;
using Asteria::User::Font::Kerning;
using Asteria::User::Font::LayoutOptions;
using Asteria::User::Font::Line;
using Asteria::User::Font::LoadOutline;
using Asteria::User::Font::Mask;
using Asteria::User::Font::MaximumCommands;
using Asteria::User::Font::MaximumMaskAxis;
using Asteria::User::Font::MaximumRunGlyphs;
using Asteria::User::Font::Measure;
using Asteria::User::Font::One;
using Asteria::User::Font::Outline;
using Asteria::User::Font::OutlineWorkspace;
using Asteria::User::Font::PairAdjustment;
using Asteria::User::Font::PositionedGlyph;
using Asteria::User::Font::RasterWorkspace;
using Asteria::User::Font::Rasterize;
using Asteria::User::Font::Run;
using Asteria::User::Font::Shape;
using Asteria::User::Font::Size;
using Asteria::User::Font::Tag;
using Asteria::User::Font::UInt32;
using Asteria::User::Font::UInt8;
using Asteria::User::Font::Wrap;
using Asteria::TestSupport::FontFixture::Buffer;
using Asteria::TestSupport::FontFixture::Make;
using Asteria::TestSupport::FontFixture::Set16;
using Asteria::TestSupport::FontFixture::Set32;
using Asteria::TestSupport::FontFixture::TableOffset;
namespace
{
static OutlineWorkspace ow{};
static RasterWorkspace rw{};
static Command commands[MaximumCommands]{};
static UInt8 pixels[MaximumMaskAxis * MaximumMaskAxis]{};
static PositionedGlyph positioned[MaximumRunGlyphs]{};
static CacheEntry entries[2U]{};
static UInt8 cacheBytes[300U]{};
[[nodiscard]] Bytes View(const Buffer& b)
{
    return {b.data(), b.size()};
}
void Synthetic(Asteria::TestSupport::TestContext& t, const bool cff, const bool extension, const bool longLoca)
{
    auto data = Make(cff, extension, longLoca);
    Font font{};
    if (!ASTERIA_EXPECT(t, "synthetic CFF/TrueType font opens", font.Open(View(data)) == Error::None))
    {
        return;
    }
    const auto& m = font.GetMetrics();
    ASTERIA_EXPECT(t, "checked metrics and Unicode cmap12", m.UnitsPerEm == 1000U, m.GlyphCount == 12U,
                   m.Ascender == 800, m.Descender == -200, font.Glyph('A') == 1U, font.Glyph('V') == 2U,
                   font.Glyph(0x1F600U) == 11U, font.Glyph(0xD800U) == 0U, font.Glyph(0x10FFFFU) == 0U);
    UInt8 name[40U]{};
    Size length{};
    ASTERIA_EXPECT(t, "UTF-16 names become UTF-8", font.Name(1U, name, sizeof(name), length) == Error::None,
                   length == 9U, std::memcmp(name, "Synthetic", 9U) == 0);
    ASTERIA_EXPECT(t, "name capacity is checked", font.Name(2U, name, 2U, length) == Error::BufferTooSmall);
    Outline outline{commands, MaximumCommands, 0U, 0};
    ASTERIA_EXPECT(t, "rectangle outline is exact", LoadOutline(font, 1U, ow, outline) == Error::None,
                   outline.Count == (cff ? 6U : 5U), commands[0U].Kind == CommandKind::Move, commands[0U].A.X == 0,
                   commands[2U].A.X == 500 * One, commands[2U].A.Y == 700 * One,
                   commands[outline.Count - 1U].Kind == CommandKind::Close);
    ASTERIA_EXPECT(t, "CFF width argument/defaultWidth", !cff || outline.CharstringWidth == 600 * One);
    Mask mask{};
    ASTERIA_EXPECT(t, "integral rectangle produces exact opaque coverage",
                   Rasterize(outline, 1000U, {20U * static_cast<UInt32>(One), 0, 0}, rw, pixels, sizeof(pixels),
                             mask) == Error::None,
                   mask.Width == 10U, mask.Height == 14U, mask.Left == 0, mask.Top == -14);
    bool opaque{true};
    for (Size i{}; i < 140U; ++i)
    {
        opaque = opaque && pixels[i] == 255U;
    }
    ASTERIA_EXPECT(t, "every integral rectangle pixel is 255", opaque);
    ASTERIA_EXPECT(t, "fractional rectangle coverage is exact",
                   Rasterize(outline, 1000U, {25U * static_cast<UInt32>(One), 0, 0}, rw, pixels, sizeof(pixels),
                             mask) == Error::None,
                   mask.Width == 13U, mask.Height == 18U, pixels[0U] == 128U, pixels[12U] == 64U, pixels[13U] == 255U,
                   pixels[25U] == 128U);
    ASTERIA_EXPECT(t, "non-zero winding cuts a reversed inner contour",
                   LoadOutline(font, 2U, ow, outline) == Error::None,
                   Rasterize(outline, 1000U, {20U * static_cast<UInt32>(One), 0, 0}, rw, pixels, sizeof(pixels),
                             mask) == Error::None,
                   pixels[0U] == 255U, pixels[2U * mask.Stride + 2U] == 0U, pixels[13U * mask.Stride + 9U] == 255U);
    ASTERIA_EXPECT(t, "curves retain their original outline kind", LoadOutline(font, 3U, ow, outline) == Error::None,
                   outline.Count == 3U, commands[1U].Kind == (cff ? CommandKind::Cubic : CommandKind::Quadratic),
                   Rasterize(outline, 1000U, {20U * static_cast<UInt32>(One), 0, 0}, rw, pixels, sizeof(pixels),
                             mask) == Error::None,
                   mask.Width == 10U, mask.Height == (cff ? 6U : 5U));
    if (!cff)
    {
        ASTERIA_EXPECT(t, "composite scale/translation is exact", LoadOutline(font, 4U, ow, outline) == Error::None,
                       outline.Count == 5U, commands[0U].A.X == 100 * One, commands[0U].A.Y == 200 * One,
                       commands[2U].A.X == 350 * One, commands[2U].A.Y == 550 * One);
        ASTERIA_EXPECT(t, "composite point attachment is exact", LoadOutline(font, 5U, ow, outline) == Error::None,
                       outline.Count == 10U, commands[5U].A.X == 500 * One, commands[5U].A.Y == 0);
    }
    PairAdjustment pair{};
    ASTERIA_EXPECT(t, "format1 pair values including second placement", Kerning(font, 1U, 2U, pair) == Error::None,
                   pair.First.XAdvance == -100, pair.Second.XPlacement == 15);
    ASTERIA_EXPECT(t, "format2 class pair", Kerning(font, 2U, 1U, pair) == Error::None, pair.First.XAdvance == -40,
                   pair.Second.XPlacement == 0);
    Run run{positioned, MaximumRunGlyphs, 0U, 0, 0};
    LayoutOptions options{20U * static_cast<UInt32>(One), true, true};
    const UInt8 text[]{'A', 'V', ' ', 'f', 'i', ' ', 'f', 'l', 0xFFU, 0xF0U, 0x9FU, 0x98U, 0x80U};
    ASTERIA_EXPECT(t, "liga preserves UTF-8 clusters and invalid bytes become replacement",
                   Shape(font, {text, sizeof(text)}, options, run) == Error::None, run.Count == 8U,
                   positioned[3U].Glyph == 9U, positioned[3U].Cluster == 3U, positioned[3U].ClusterEnd == 5U,
                   positioned[5U].Glyph == 10U, positioned[6U].Scalar == 0xFFFDU, positioned[6U].Glyph == 0U,
                   positioned[7U].Glyph == 11U, positioned[0U].AdvanceX == 10 * One,
                   positioned[1U].X == 10 * One + 3 * One / 10);
    options.Ligatures = false;
    ASTERIA_EXPECT(t, "feature options disable substitution",
                   Shape(font, {text, sizeof(text)}, options, run) == Error::None, run.Count == 10U);
    options.Ligatures = true;
    Int64 width{};
    Int64 height{};
    const UInt8 multiline[]{'A', 'V', '\n', 'f', 'i'};
    ASTERIA_EXPECT(t, "measurement uses advances and line metrics",
                   Measure(font, {multiline, sizeof(multiline)}, options, run, width, height) == Error::None,
                   width == 22 * One, height == 48 * One, GetLineMetrics(font, options.PixelSize).Ascender == 16 * One);
    Line lines[10U]{};
    UInt32 lineCount{};
    ASTERIA_EXPECT(t, "wrapping respects LF and cluster boundaries",
                   Wrap(run, 15 * One, lines, 10U, lineCount) == Error::None, lineCount == 3U, lines[2U].Count == 1U);
    GlyphCache cache{};
    ASTERIA_EXPECT(t, "bounded caller-owned cache",
                   cache.Configure(cacheBytes, sizeof(cacheBytes), entries, 2U) == Error::None,
                   cache.Get(font, 1U, {20U * static_cast<UInt32>(One), 0, 0}, ow, outline, rw, mask) == Error::None,
                   cache.Count() == 1U, cache.UsedBytes() == 140U);
    const auto* const firstMask{mask.Pixels};
    commands[0U].A.X = 123;
    ASTERIA_EXPECT(t, "cache hit reuses mask without outline interpretation",
                   cache.Get(font, 1U, {20U * static_cast<UInt32>(One), 0, 0}, ow, outline, rw, mask) == Error::None,
                   mask.Pixels == firstMask, commands[0U].A.X == 123);
    ASTERIA_EXPECT(t, "cache eviction respects byte limit",
                   cache.Get(font, 2U, {20U * static_cast<UInt32>(One), 0, 0}, ow, outline, rw, mask) == Error::None,
                   cache.Get(font, 4U, {20U * static_cast<UInt32>(One), 0, 0}, ow, outline, rw, mask) == Error::None,
                   cache.Count() == 1U, cache.UsedBytes() <= sizeof(cacheBytes));
    // Choose the format4 record alone and exercise direct idDelta mappings.
    Set16(data, TableOffset(data, Tag('c', 'm', 'a', 'p')) + 2U, 1U);
    ASTERIA_EXPECT(t, "cmap4 is independently usable", font.Open(View(data)) == Error::None, font.Glyph('A') == 1U,
                   font.Glyph('f') == 6U, font.Glyph(0x1F600U) == 0U);
}
void Malformed(Asteria::TestSupport::TestContext& t)
{
    for (const bool cff : {false, true})
    {
        const Buffer data{Make(cff)};
        for (Size size{}; size < data.size(); ++size)
        {
            Font f{};
            ASTERIA_EXPECT(t, "every truncated sfnt fails safely", f.Open({data.data(), size}) != Error::None,
                           !f.IsOpen());
        }
        Buffer bad{data};
        Set32(bad, 20U, 0xFFFFFFF0U);
        Font f{};
        ASTERIA_EXPECT(t, "directory overflow is rejected", f.Open(View(bad)) == Error::Truncated);
        bad = data;
        Set32(bad, 28U, static_cast<UInt32>(Tag('C', 'F', 'F', ' ')));
        ASTERIA_EXPECT(t, "duplicate/unsorted tags are rejected", f.Open(View(bad)) != Error::None);
        bad = data;
        Set16(bad, TableOffset(bad, Tag('h', 'e', 'a', 'd')) + 18U, 0U);
        ASTERIA_EXPECT(t, "invalid em size is rejected", f.Open(View(bad)) == Error::Malformed);
        bad = data;
        Set16(bad, TableOffset(bad, Tag('h', 'h', 'e', 'a')) + 34U, 13U);
        ASTERIA_EXPECT(t, "hmtx metric count is checked", f.Open(View(bad)) == Error::Malformed);
        bad = data;
        Set16(bad, TableOffset(bad, Tag('G', 'P', 'O', 'S')) + 4U, 0U);
        ASTERIA_EXPECT(t, "layout offsets must be non-null", f.Open(View(bad)) == Error::Malformed);
        if (!cff)
        {
            bad = data;
            const Size glyf{TableOffset(bad, Tag('g', 'l', 'y', 'f'))};
            const Size loca{TableOffset(bad, Tag('l', 'o', 'c', 'a'))};
            const Size component{glyf + (static_cast<Size>(bad[loca + 16U]) << 24U) +
                                 (static_cast<Size>(bad[loca + 17U]) << 16U) +
                                 (static_cast<Size>(bad[loca + 18U]) << 8U) + bad[loca + 19U]};
            Set16(bad, component + 12U, 4U);
            Outline out{commands, MaximumCommands, 0U, 0};
            ASTERIA_EXPECT(t, "composite cycles are bounded", f.Open(View(bad)) == Error::None,
                           LoadOutline(f, 4U, ow, out) == Error::RecursionLimit);
        }
    }
}
void Blending(Asteria::TestSupport::TestContext& t)
{
    using Asteria::User::Graphics::MemorySurface;
    using Asteria::User::Graphics::PackColor;
    using Asteria::User::Graphics::PixelFormat;
    using Asteria::User::Graphics::Surface;
    UInt32 surfacePixels[12U]{};
    const UInt8 coverage[]{0U, 128U, 255U, 64U};
    const Mask mask{coverage, sizeof(coverage), 2U, 2U, 2U, -1, -1};
    for (const auto format : {PixelFormat::Rgbx, PixelFormat::Bgrx})
    {
        const MemorySurface surface{surfacePixels, 3U, 3U, 4U, format};
        for (auto& p : surfacePixels)
        {
            p = PackColor(format, {0U, 0U, 255U});
        }
        ASTERIA_EXPECT(t, "surface coverage blending and stride clipping",
                       DrawMask(surface, mask, {1, 1}, {255U, 0U, 0U}) == Error::None,
                       surfacePixels[0U] == PackColor(format, {0U, 0U, 255U}),
                       surfacePixels[1U] == PackColor(format, {128U, 0U, 127U}),
                       surfacePixels[4U] == PackColor(format, {255U, 0U, 0U}),
                       surfacePixels[5U] == PackColor(format, {64U, 0U, 191U}),
                       surfacePixels[3U] == PackColor(format, {0U, 0U, 255U}));
        const Surface volatileSurface{surfacePixels, 3U, 3U, 4U, format};
        ASTERIA_EXPECT(t, "MMIO surface with colour opacity",
                       DrawMask(volatileSurface, mask, {0, 0}, {0U, 255U, 0U}, 128U) == Error::None);
    }
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext t{"Font"};
    for (const bool cff : {false, true})
    {
        for (const bool extension : {false, true})
        {
            Synthetic(t, cff, extension, true);
        }
    }
    Synthetic(t, false, true, false);
    Malformed(t);
    Blending(t);
    return t.Finish();
}
