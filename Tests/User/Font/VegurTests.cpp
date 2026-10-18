// SPDX-License-Identifier: Apache-2.0
#include "User/Font/Outline.hpp"
#include "User/Font/Raster.hpp"
#include "User/Font/Layout.hpp"
// Host verification only: reuse the project's specification-written SHA-256.
#include "Kernel/Security/Sha256.hpp"
#include "Support/TestContext.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using Asteria::Foundation::UInt16;
using Asteria::Foundation::Int32;
using Asteria::User::Font::Command;
using Asteria::User::Font::CommandKind;
using Asteria::User::Font::Error;
using Asteria::User::Font::Font;
using Asteria::User::Font::Kerning;
using Asteria::User::Font::LoadOutline;
using Asteria::User::Font::Mask;
using Asteria::User::Font::MaximumCommands;
using Asteria::User::Font::MaximumMaskAxis;
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
using Asteria::User::Font::UInt32;
using Asteria::User::Font::UInt8;
namespace
{
static OutlineWorkspace ow{};
static RasterWorkspace rw{};
static Command commands[MaximumCommands]{};
static UInt8 pixels[MaximumMaskAxis * MaximumMaskAxis]{};
static PositionedGlyph glyphs[32U]{};
struct Face
{
    const char* Style;
    const char* Hash;
    UInt16 Weight;
};
constexpr Face Faces[]{{"Light", "b0d25ce0ea5fb6c6fe8da1c04680e2319a877ffe76f47dd57e19e81e48f3752d", 300U},
                       {"Regular", "5c494cd1381fe91cfb511fd6efdb8ef8ec9555e48f78faaf7340e209e4a96536", 400U},
                       {"Bold", "0643810681e276b8d2b37d09e089e76cffc882f6f002c1f68125eea6a6f87c76", 700U}};
[[nodiscard]] std::vector<UInt8> Read(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
[[nodiscard]] std::string Pgm(const Mask& mask)
{
    std::string pgm{"P5\n# baseline "};
    pgm += std::to_string(mask.Left) + " " + std::to_string(mask.Top) + "\n";
    pgm += std::to_string(mask.Width) + " " + std::to_string(mask.Height) + "\n255\n";
    pgm.append(reinterpret_cast<const char*>(mask.Pixels), static_cast<Size>(mask.Width) * mask.Height);
    return pgm;
}
[[nodiscard]] bool Hash(const std::vector<UInt8>& bytes, const char* const expected)
{
    Asteria::Kernel::Security::Sha256 hash{};
    hash.Update(bytes.data(), bytes.size());
    UInt8 digest[32U]{};
    hash.Finish(digest);
    constexpr char hex[]{"0123456789abcdef"};
    for (Size i{}; i < 32U; ++i)
    {
        if (hex[digest[i] >> 4U] != expected[2U * i] || hex[digest[i] & 15U] != expected[2U * i + 1U])
        {
            return false;
        }
    }
    return true;
}
void TestFace(Asteria::TestSupport::TestContext& t, const Face& face, const bool record)
{
    const auto bytes = Read(std::string{ASTERIA_FONT_DATA_DIR} + "/Vegur-" + face.Style + ".otf");
    if (!ASTERIA_EXPECT(t, "Vegur file exactly matches NOTICE SHA-256", Hash(bytes, face.Hash)))
    {
        return;
    }
    Font font{};
    const Error error{font.Open({bytes.data(), bytes.size()})};
    if (!ASTERIA_EXPECT(t, "unmodified Vegur opens", error == Error::None))
    {
        std::printf("Open error: %u\n", static_cast<unsigned>(error));
        return;
    }
    const auto& m = font.GetMetrics();
    ASTERIA_EXPECT(t, "recorded Vegur line metrics", m.UnitsPerEm == 1000U, m.GlyphCount == 205U, m.Ascender == 965,
                   m.Descender == -235, m.LineGap == 0, m.TypoAscender == 765, m.TypoDescender == -235,
                   m.TypoLineGap == 200, m.Weight == face.Weight);
    UInt8 name[64U]{};
    Size length{};
    ASTERIA_EXPECT(t, "Vegur Unicode family name", font.FamilyName(name, sizeof(name), length) == Error::None,
                   length == 5U, std::memcmp(name, "Vegur", 5U) == 0);
    ASTERIA_EXPECT(t, "Vegur Unicode style name", font.StyleName(name, sizeof(name), length) == Error::None,
                   length == std::strlen(face.Style), std::memcmp(name, face.Style, length) == 0);
    bool ascii{true};
    for (UInt32 p{32U}; p < 127U; ++p)
    {
        ascii = ascii && font.Glyph(p) != 0U;
    }
    ASTERIA_EXPECT(t, "Vegur cmap covers ASCII and Latin/punctuation", ascii, font.Glyph(0xE9U) != 0U,
                   font.Glyph(0x2014U) != 0U, font.Glyph(0x20ACU) != 0U, font.Glyph(0x3042U) == 0U,
                   font.Glyph(0x1F600U) == 0U);
    Outline outline{commands, MaximumCommands, 0U, 0};
    // All glyphs, including accented glyphs, must interpret within the bounds.
    for (UInt32 g{}; g < m.GlyphCount; ++g)
    {
        ASTERIA_EXPECT(t, "all Vegur glyph programs interpret",
                       LoadOutline(font, static_cast<UInt16>(g), ow, outline) == Error::None,
                       outline.Count <= MaximumCommands);
    }
    constexpr Int32 expectedKern[3U][6U]{
        {-69, -69, -55, -86, -60, -96}, {-55, -38, -84, -80, -42, -72}, {-69, -69, -108, -102, -60, -96}};
    const UInt32 faceIndex{face.Weight == 300U ? 0U : (face.Weight == 400U ? 1U : 2U)};
    PairAdjustment pair{};
    UInt32 pairIndex{};
    for (const char* text : {"AV", "VA", "To", "Ta", "WA", "Yo"})
    {
        ASTERIA_EXPECT(t, "Vegur kerning pair matches recorded design-unit adjustment",
                       Kerning(font, font.Glyph(static_cast<UInt8>(text[0U])), font.Glyph(static_cast<UInt8>(text[1U])),
                               pair) == Error::None,
                       pair.First.XAdvance == expectedKern[faceIndex][pairIndex++]);
        if (record)
        {
            std::printf("%s kern %s = %d\n", face.Style, text, pair.First.XAdvance);
        }
    }
    const UInt8 text[]{'f', 'i', ' ', 'f', 'l'};
    Run run{glyphs, 32U, 0U, 0, 0};
    ASTERIA_EXPECT(t, "Vegur fi and fl ligatures", Shape(font, {text, sizeof(text)}, {}, run) == Error::None,
                   run.Count == 3U, glyphs[0U].Glyph == font.Glyph(0xFB01U), glyphs[2U].Glyph == font.Glyph(0xFB02U),
                   glyphs[0U].ClusterEnd == 2U, glyphs[2U].ClusterEnd == 5U);
    constexpr UInt32 expectedCommands[6U]{13U, 8U, 12U, 20U, 19U, 19U};
    constexpr Int32 firstX[3U][6U]{
        {413, 301, 356, 438, 386, 352}, {422, 349, 354, 473, 435, 376}, {421, 380, 376, 514, 523, 401}};
    constexpr Int32 firstY[3U][6U]{{247, 0, 698, 474, 470, 0}, {227, 0, 698, 478, 474, 0}, {206, 0, 702, 482, 478, 0}};
    constexpr UInt16 advances[3U][6U]{{554U, 549U, 712U, 523U, 470U, 470U},
                                      {596U, 584U, 708U, 543U, 505U, 528U},
                                      {633U, 610U, 746U, 570U, 579U, 589U}};
    struct Sample
    {
        UInt32 Code;
        UInt32 Size;
        const char* Label;
    };
    for (const Sample sample : {Sample{'A', 24U, "A24"},
                                {'V', 24U, "V24"},
                                {'O', 24U, "O24"},
                                {'g', 24U, "g24"},
                                {0xFB01U, 24U, "fi24"},
                                {0xFB02U, 24U, "fl24"},
                                {'A', 12U, "A12"},
                                {'g', 36U, "g36"}})
    {
        if (!ASTERIA_EXPECT(t, "Vegur sample outline is closed",
                            LoadOutline(font, font.Glyph(sample.Code), ow, outline) == Error::None, outline.Count > 2U,
                            commands[0U].Kind == CommandKind::Move,
                            commands[outline.Count - 1U].Kind == CommandKind::Close))
        {
            continue;
        }
        const UInt32 sampleIndex{
            sample.Code == 'A'
                ? 0U
                : (sample.Code == 'V'
                       ? 1U
                       : (sample.Code == 'O' ? 2U : (sample.Code == 'g' ? 3U : (sample.Code == 0xFB01U ? 4U : 5U))))};
        ASTERIA_EXPECT(t, "Vegur outlines and widths match recorded coordinates",
                       outline.Count == expectedCommands[sampleIndex],
                       commands[0U].A.X == firstX[faceIndex][sampleIndex] * One,
                       commands[0U].A.Y == firstY[faceIndex][sampleIndex] * One,
                       font.Horizontal(font.Glyph(sample.Code)).Advance == advances[faceIndex][sampleIndex],
                       outline.CharstringWidth == advances[faceIndex][sampleIndex] * One);
        if (record)
        {
            std::printf("%s %s glyph=%u commands=%u first=(%lld,%lld) advance=%u width=%lld\n", face.Style,
                        sample.Label, font.Glyph(sample.Code), outline.Count, commands[0U].A.X / One,
                        commands[0U].A.Y / One, font.Horizontal(font.Glyph(sample.Code)).Advance,
                        outline.CharstringWidth / One);
        }
        Mask mask{};
        if (!ASTERIA_EXPECT(t, "Vegur sample rasterizes",
                            Rasterize(outline, m.UnitsPerEm, {sample.Size * static_cast<UInt32>(One), 0, 0}, rw, pixels,
                                      sizeof(pixels), mask) == Error::None,
                            mask.Width != 0U, mask.Height != 0U))
        {
            continue;
        }
        const auto pgm = Pgm(mask);
        const std::string path{std::string{ASTERIA_FONT_GOLDEN_DIR} + "/" + face.Style + "-" + sample.Label + ".pgm"};
        if (record)
        {
            std::ofstream file{path, std::ios::binary};
            file.write(pgm.data(), static_cast<std::streamsize>(pgm.size()));
            ASTERIA_EXPECT(t, "golden written", file.good());
        }
        else
        {
            const auto golden = Read(path);
            ASTERIA_EXPECT(t, "reviewed coverage and bearings match byte-for-byte", golden.size() == pgm.size(),
                           std::memcmp(golden.data(), pgm.data(), pgm.size()) == 0);
        }
    }
}
} // namespace
int main(const int argc, const char* const* argv)
{
    const bool record{argc == 2 && std::strcmp(argv[1U], "--record-goldens") == 0};
    Asteria::TestSupport::TestContext t{"Vegur"};
    for (const auto& face : Faces)
    {
        TestFace(t, face, record);
    }
    return t.Finish();
}
