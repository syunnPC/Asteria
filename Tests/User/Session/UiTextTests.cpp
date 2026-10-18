// SPDX-License-Identifier: Apache-2.0
#include "Abi/ComponentBootstrap.hpp"
#include "User/Session/UiText.hpp"
#include "Support/TestContext.hpp"
#include <cstdio>
#include <cstring>
namespace Abi = Asteria::Abi;
namespace Foundation = Asteria::Foundation;
namespace TestSupport = Asteria::TestSupport;
namespace User = Asteria::User;
namespace
{
namespace U = User::Session;
namespace G = User::Graphics;
namespace F = Foundation;
U::UiText g_ui{};
User::Font::OutlineWorkspace g_outlineWorkspace{};
constinit User::Font::RasterWorkspace g_rasterWorkspace{};
User::Font::Command g_commands[User::Font::MaximumCommands]{};
F::UInt8 g_mask[User::Font::MaximumMaskAxis * User::Font::MaximumMaskAxis]{};
F::UInt8 g_data[2U][Abi::ComponentUiFontFaceBytes]{};
F::Size g_lengths[2U]{};
F::UInt32 g_pixels[300U * 40U]{};
[[nodiscard]] bool Read() noexcept
{
    const char* const paths[]{ASTERIA_FONT_DATA_DIR "/Vegur-Bold.otf", ASTERIA_FONT_DATA_DIR "/Vegur-Regular.otf"};
    for (unsigned i{}; i < 2U; ++i)
    {
        auto* const file = std::fopen(paths[i], "rb");
        if (file == nullptr)
        {
            return false;
        }
        g_lengths[i] = std::fread(g_data[i], 1U, sizeof(g_data[i]), file);
        const bool valid{std::fgetc(file) == EOF && !std::ferror(file)};
        std::fclose(file);
        if (!valid)
        {
            return false;
        }
    }
    return true;
}
void Paint(const char* const text, const F::UInt32 width, const F::Size suffix = 0U) noexcept
{
    for (auto& pixel : g_pixels)
    {
        pixel = 0x00402010U;
    }
    const G::MemorySurface surface{g_pixels, 300U, 40U, 300U, G::PixelFormat::Rgbx};
    const G::Rect box{12, 10, width, U::UiTextHeight};
    g_ui.Paint(surface, {0, 0, 300U, 40U}, box, reinterpret_cast<const F::UInt8*>(text), std::strlen(text), true, 7U,
               suffix, {255U, 255U, 255U}, {184U, 200U, 232U}, {16U, 32U, 64U});
}
} // namespace
int main()
{
    TestSupport::TestContext t{"UiText"};
    ASTERIA_EXPECT(t, "UI default is 13 px in the unchanged 20 px title bar", U::UiPixelSize == 13U,
                   U::UiTextHeight + 2U * U::TitleTextTop == U::TitleBarHeight, Read(),
                   g_ui.Open({g_data[0U], g_lengths[0U]}, {g_data[1U], g_lengths[1U]}), g_ui.Scalable(true),
                   g_ui.Scalable(false));
    bool inkFits{true};
    for (unsigned face{}; face < 2U; ++face)
    {
        User::Font::Font font{};
        if (font.Open({g_data[face], g_lengths[face]}) != User::Font::Error::None)
        {
            inkFits = false;
            continue;
        }
        User::Font::Outline outline{g_commands, User::Font::MaximumCommands};
        for (F::UInt32 glyph{}; glyph < font.GetMetrics().GlyphCount; ++glyph)
        {
            User::Font::Mask mask{};
            if (User::Font::LoadOutline(font, static_cast<F::UInt16>(glyph), g_outlineWorkspace, outline) !=
                    User::Font::Error::None ||
                User::Font::Rasterize(outline, font.GetMetrics().UnitsPerEm,
                                      {U::UiPixelSize * static_cast<F::UInt32>(User::Font::One)}, g_rasterWorkspace,
                                      g_mask, sizeof(g_mask), mask) != User::Font::Error::None ||
                mask.Top + static_cast<F::Int32>(U::UiBaseline) < 0 ||
                mask.Top + static_cast<F::Int32>(U::UiBaseline + mask.Height) > static_cast<F::Int32>(U::UiTextHeight))
            {
                inkFits = false;
            }
        }
    }
    ASTERIA_EXPECT(t, "every pinned Vegur glyph fits the title area at this baseline", inkFits);
    const F::UInt8 av[]{'A', 'V'};
    const F::UInt8 a[]{'A'};
    const F::UInt8 v[]{'V'};
    ASTERIA_EXPECT(t, "title measurement applies Vegur kerning",
                   g_ui.Width(av, 2U, true) < g_ui.Width(a, 1U, true) + g_ui.Width(v, 1U, true));
    Paint("office AV - efficient", 250U);
    ASTERIA_EXPECT(t, "a fitting title retains all bytes", g_ui.FittedPrefixBytes() == 21U);
    Paint("AV", 250U);
    const auto cached = g_ui.CachedGlyphs();
    Paint("AV", 250U);
    ASTERIA_EXPECT(t, "repainting retains cached coverage", cached != 0U, g_ui.CachedGlyphs() == cached);
    Paint("office AV - efficient", 250U);
    bool clipped{true};
    bool ink{false};
    for (F::UInt32 y{}; y < 40U; ++y)
    {
        for (F::UInt32 x{}; x < 300U; ++x)
        {
            const bool outside{x < 12U || x >= 262U || y < 10U || y >= 10U + U::UiTextHeight};
            if (outside && g_pixels[y * 300U + x] != 0x00402010U)
            {
                clipped = false;
            }
            if (!outside && g_pixels[y * 300U + x] != 0x00402010U)
            {
                ink = true;
            }
        }
    }
    ASTERIA_EXPECT(t, "coverage and overhang stay inside the title text box", clipped, ink);
    Paint("office AV - a very long title (not responding)", 160U, sizeof(U::NotRespondingMark) - 1U);
    ASTERIA_EXPECT(t, "ellipsis preserves the liveness suffix when it fits", g_ui.FittedPrefixBytes() < 28U,
                   g_ui.FittedSuffixBytes() == sizeof(U::NotRespondingMark) - 1U);
    Paint("fi efficient", 18U);
    ASTERIA_EXPECT(t, "ellipsis cannot split the fi shaped cluster", g_ui.FittedPrefixBytes() != 1U);
    ASTERIA_EXPECT(t, "absent font selects bitmap text", !g_ui.Open({}, {}), !g_ui.Scalable(true),
                   !g_ui.Scalable(false), g_ui.Width(av, 2U, true) == 16U);
    Paint("abcdef", 40U);
    ASTERIA_EXPECT(t, "bitmap fallback uses the same ellipsis policy", g_ui.FittedPrefixBytes() == 2U);
    const F::UInt8 invalid[]{0U, 1U, 2U, 3U};
    ASTERIA_EXPECT(t, "a rejected face falls back independently",
                   !g_ui.Open({invalid, sizeof(invalid)}, {g_data[1U], g_lengths[1U]}), !g_ui.Scalable(true),
                   g_ui.Scalable(false));
    return t.Finish();
}
