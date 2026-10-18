// SPDX-License-Identifier: Apache-2.0
// Host glyph-mask oracle for the runner; scene composition stays in Python.
// personalization and DesktopSessionLibraries.md.
#include "Abi/ComponentBootstrap.hpp"
#include "User/Session/UiText.hpp"
#include "User/Session/UiTextDrawing.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace Abi = Asteria::Abi;
namespace Foundation = Asteria::Foundation;
namespace User = Asteria::User;
namespace
{
constexpr Foundation::UInt32 MaximumWidth{32768U};
Foundation::UInt8 g_fonts[2U][Abi::ComponentUiFontFaceBytes]{};
Foundation::UInt8 g_text[User::Session::TitleTextBytes]{};
Foundation::UInt32 g_pixels[MaximumWidth * User::Session::UiTextHeight]{};
User::Session::UiText g_ui{};
[[nodiscard]] User::Font::Bytes Read(const char* const path, const unsigned face) noexcept
{
    if (std::strcmp(path, "-") == 0)
    {
        return {};
    }
    auto* const file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return {};
    }
    const auto length = std::fread(g_fonts[face], 1U, sizeof(g_fonts[face]), file);
    const bool valid{std::fgetc(file) == EOF && !std::ferror(file)};
    std::fclose(file);
    return valid ? User::Font::Bytes{g_fonts[face], length} : User::Font::Bytes{};
}
void Emit(void*, const User::Font::Mask& mask, const User::Graphics::Point baseline, const User::Graphics::Rect clip,
          const User::Graphics::Color color, const bool opaque) noexcept
{
    std::printf("mask %d %d %d %d %u %u %d %d %u %u %u %u\n", baseline.X, baseline.Y, mask.Left, mask.Top, mask.Width,
                mask.Height, clip.X, clip.Y, clip.Width, clip.Height, color.Green == 0U ? 1U : 0U, opaque ? 1U : 0U);
    for (Foundation::UInt32 y{}; y < mask.Height; ++y)
    {
        static_cast<void>(
            std::fwrite(mask.Pixels + static_cast<Foundation::Size>(y) * mask.Stride, 1U, mask.Width, stdout));
    }
    std::fputc('\n', stdout);
}
[[nodiscard]] int Hex(const char c) noexcept
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
} // Namespace
namespace Asteria::User::Session
{
void DrawUiMask(const Graphics::MemorySurface&, const Font::Mask& mask, const Graphics::Point origin,
                const Graphics::Rect clip, const Graphics::Color foreground) noexcept
{
    Emit(nullptr, mask, {origin.X + clip.X, origin.Y + clip.Y}, clip, foreground, false);
}
void DrawUiBitmap(const Graphics::MemorySurface&, const Graphics::Point origin, const Graphics::Rect clip,
                  const Foundation::UInt32 code, const Graphics::Color foreground, const Graphics::Color) noexcept
{
    constexpr auto& bitmap = Graphics::Bold8x16;
    static Foundation::UInt8 pixels[bitmap.GlyphWidth * bitmap.GlyphHeight]{};
    for (Foundation::UInt32 y{}; y < bitmap.GlyphHeight; ++y)
    {
        for (Foundation::UInt32 x{}; x < bitmap.GlyphWidth; ++x)
        {
            pixels[y * bitmap.GlyphWidth + x] =
                (code >= bitmap.FirstCode && code - bitmap.FirstCode < bitmap.Count &&
                 ((bitmap.Data[static_cast<Foundation::Size>(code - bitmap.FirstCode) * bitmap.GlyphHeight + y] >>
                   (7U - x)) &
                  1U) != 0U)
                    ? 255U
                    : 0U;
        }
    }
    const Font::Mask mask{pixels, sizeof(pixels), bitmap.GlyphWidth, bitmap.GlyphHeight, bitmap.GlyphWidth, 0, 0};
    Emit(nullptr, mask, {origin.X + clip.X, origin.Y + clip.Y}, clip, foreground, true);
}
} // Namespace Asteria::User::Session
int main(const int argc, const char* const* const argv)
{
    if (argc != 8)
    {
        return 2;
    }
    const auto width = std::strtoul(argv[3], nullptr, 10);
    const bool bold = std::strcmp(argv[4], "bold") == 0;
    const auto trusted = std::strtoul(argv[5], nullptr, 10);
    const auto suffix = std::strtoul(argv[6], nullptr, 10);
    const auto hexBytes = std::strlen(argv[7]);
    if (width > MaximumWidth || hexBytes % 2U != 0U || hexBytes / 2U > sizeof(g_text))
    {
        return 2;
    }
    for (Foundation::Size at{}; at < hexBytes / 2U; ++at)
    {
        const int a{Hex(argv[7][at * 2U])};
        const int b{Hex(argv[7][at * 2U + 1U])};
        if (a < 0 || b < 0)
        {
            return 2;
        }
        g_text[at] = static_cast<Foundation::UInt8>(a * 16 + b);
    }
    static_cast<void>(g_ui.Open(Read(argv[1], 0U), Read(argv[2], 1U)));
    if ((std::strcmp(argv[1], "-") != 0 && !g_ui.Scalable(true)) ||
        (std::strcmp(argv[2], "-") != 0 && !g_ui.Scalable(false)))
    {
        std::fputs("UI font validation failed\n", stderr);
        return 3;
    }
    const auto w = static_cast<Foundation::UInt32>(width);
    const User::Graphics::MemorySurface surface{g_pixels, w, User::Session::UiTextHeight, w,
                                                User::Graphics::PixelFormat::Rgbx};
    const User::Graphics::Color white{255U, 255U, 255U};
    const User::Graphics::Color red{255U, 0U, 0U};
    const User::Graphics::Color black{};
    std::printf("UITM1\nwidth %u\n", g_ui.Width(g_text, hexBytes / 2U, bold));
    g_ui.Paint(surface, User::Graphics::Bounds(surface), User::Graphics::Bounds(surface), g_text, hexBytes / 2U, bold,
               trusted, suffix, white, red, black);
    std::fputs("end\n", stdout);
    return std::ferror(stdout) ? 4 : 0;
}
