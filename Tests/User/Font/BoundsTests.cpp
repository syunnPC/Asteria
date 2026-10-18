// SPDX-License-Identifier: Apache-2.0
#include "User/Font/Draw.hpp"
#include "User/Font/Layout.hpp"
#include "User/Font/Fixture.hpp"
#include "Support/TestContext.hpp"
using Asteria::Foundation::UInt16;
using Asteria::User::Font::Command;
using Asteria::User::Font::CommandKind;
using Asteria::User::Font::Error;
using Asteria::User::Font::Font;
using Asteria::User::Font::Line;
using Asteria::User::Font::LoadOutline;
using Asteria::User::Font::Mask;
using Asteria::User::Font::MaximumCommands;
using Asteria::User::Font::MaximumMaskAxis;
using Asteria::User::Font::MaximumValidationWork;
using Asteria::User::Font::One;
using Asteria::User::Font::Outline;
using Asteria::User::Font::OutlineWorkspace;
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
using Asteria::TestSupport::FontFixture::Append;
using Asteria::TestSupport::FontFixture::Buffer;
using Asteria::TestSupport::FontFixture::Make;
using Asteria::TestSupport::FontFixture::Set16;
using Asteria::TestSupport::FontFixture::Set32;
using Asteria::TestSupport::FontFixture::TableOffset;
namespace
{
static RasterWorkspace raster{};
static OutlineWorkspace outlineWorkspace{};
static Command commands[MaximumCommands]{};
static UInt8 pixels[MaximumMaskAxis * MaximumMaskAxis]{};
static PositionedGlyph positioned[32U]{};
void CmapRange(Asteria::TestSupport::TestContext& t)
{
    auto data = Make(false);
    const Size at{TableOffset(data, Tag('c', 'm', 'a', 'p'))};
    // Replace the last table's format4 with a tiny indexed segment plus sentinel.
    Set16(data, at + 2U, 1U);
    Set16(data, at + 22U, 34U);
    Set16(data, at + 26U, 4U);
    Set16(data, at + 34U, 65U);
    Set16(data, at + 36U, 0xFFFFU);
    Set16(data, at + 38U, 0U);
    Set16(data, at + 40U, 65U);
    Set16(data, at + 42U, 0xFFFFU);
    Set16(data, at + 44U, 1U);
    Set16(data, at + 46U, 1U);
    Set16(data, at + 48U, 4U);
    Set16(data, at + 50U, 0U);
    Set16(data, at + 52U, 1U);
    Font font{};
    ASTERIA_EXPECT(t, "cmap4 idRangeOffset and delta both apply", font.Open({data.data(), data.size()}) == Error::None,
                   font.Glyph('A') == 2U, font.Glyph('B') == 0U);
    Set16(data, at + 52U, 0U);
    ASTERIA_EXPECT(t, "cmap4 zero glyph stays notdef despite delta",
                   font.Open({data.data(), data.size()}) == Error::None, font.Glyph('A') == 0U);
    Set16(data, at + 48U, 2U);
    ASTERIA_EXPECT(t, "cmap4 indexed data cannot alias offset records",
                   font.Open({data.data(), data.size()}) != Error::None);
}
void RasterLimits(Asteria::TestSupport::TestContext& t)
{
    Outline out{commands, MaximumCommands, 0U, 0};
    Mask mask{};
    commands[0U] = {CommandKind::Move, {0, 0}, {}, {}};
    commands[1U] = {CommandKind::Line, {500 * One, 0}, {}, {}};
    commands[2U] = {CommandKind::Line, {500 * One, 700 * One}, {}, {}};
    commands[3U] = {CommandKind::Line, {0, 700 * One}, {}, {}};
    commands[4U] = {CommandKind::Close, {}, {}, {}};
    // A second same-direction contour fills the centre, unlike even-odd fill.
    commands[5U] = {CommandKind::Move, {100 * One, 100 * One}, {}, {}};
    commands[6U] = {CommandKind::Line, {400 * One, 100 * One}, {}, {}};
    commands[7U] = {CommandKind::Line, {400 * One, 600 * One}, {}, {}};
    commands[8U] = {CommandKind::Line, {100 * One, 600 * One}, {}, {}};
    commands[9U] = {CommandKind::Close, {}, {}, {}};
    out.Count = 10U;
    ASTERIA_EXPECT(t, "non-zero winding fills nested same-direction contours",
                   Rasterize(out, 1000U, {20U * static_cast<UInt32>(One), 0, 0}, raster, pixels, sizeof(pixels),
                             mask) == Error::None,
                   pixels[2U * mask.Stride + 2U] == 255U);
    ASTERIA_EXPECT(t, "coverage capacity is checked before writes",
                   Rasterize(out, 1000U, {}, raster, pixels, 1U, mask) == Error::BufferTooSmall);
    ASTERIA_EXPECT(t, "pixel size and coordinate limits",
                   Rasterize(out, 1000U, {0U, 0, 0}, raster, pixels, sizeof(pixels), mask) == Error::InvalidArgument,
                   Rasterize(out, 1000U, {513U * static_cast<UInt32>(One), 0, 0}, raster, pixels, sizeof(pixels),
                             mask) == Error::InvalidArgument);
    commands[0U].A.X = (-0x7FFFFFFFFFFFFFFFLL - 1);
    ASTERIA_EXPECT(t, "most negative coordinate rejects without signed overflow",
                   Rasterize(out, 1000U, {}, raster, pixels, sizeof(pixels), mask) == Error::CoordinateLimit);
    commands[0U].A.X = 0;
    out.Count = 2U;
    commands[1U].Kind = static_cast<CommandKind>(255U);
    ASTERIA_EXPECT(t, "unknown command rejects",
                   Rasterize(out, 1000U, {}, raster, pixels, sizeof(pixels), mask) == Error::Malformed);
}
void NameWork(Asteria::TestSupport::TestContext& t)
{
    auto data = Make(true);
    constexpr UInt16 length{65534U};
    constexpr UInt16 within{static_cast<UInt16>(MaximumValidationWork / length)};
    constexpr UInt16 base{8U + 4U * (within + 1U)};
    Buffer names(static_cast<Size>(base) + length);
    Set16(names, 0U, 1U);
    Set16(names, 4U, base);
    Set16(names, 6U, within);
    // Language records deliberately alias one UTF-16 string. Validation work
    // counts each reference, including records outside the main name array.
    for (UInt32 i{}; i <= within; ++i)
    {
        Set16(names, 8U + 4U * i, length);
    }
    while ((data.size() & 3U) != 0U)
    {
        data.push_back(0U);
    }
    const UInt32 at{static_cast<UInt32>(data.size())};
    Append(data, names);
    const UInt32 tables{(static_cast<UInt32>(data[4U]) << 8U) | data[5U]};
    for (UInt32 i{}; i < tables; ++i)
    {
        const Size record{12U + 16U * i};
        if (data[record] == 'n' && data[record + 1U] == 'a' && data[record + 2U] == 'm' && data[record + 3U] == 'e')
        {
            Set32(data, record + 8U, at);
            Set32(data, record + 12U, static_cast<UInt32>(names.size()));
        }
    }
    Font font{};
    ASTERIA_EXPECT(t, "language-name work within budget is accepted",
                   font.Open({data.data(), data.size()}) == Error::None);
    Set16(data, at + 6U, within + 1U);
    ASTERIA_EXPECT(t, "aliased language names share the validation work budget",
                   font.Open({data.data(), data.size()}) == Error::OperationLimit, !font.IsOpen());
}
void LayoutLimits(Asteria::TestSupport::TestContext& t)
{
    auto data = Make(true);
    Font font{};
    if (!ASTERIA_EXPECT(t, "layout fixture opens", font.Open({data.data(), data.size()}) == Error::None))
    {
        return;
    }
    Run run{positioned, 32U, 0U, 0, 0};
    const UInt8 bad[]{0xC0U, 0xAFU, 0xEDU, 0xA0U, 0x80U, 0xF4U, 0x90U, 0x80U, 0x80U, 0xE2U, 0x82U};
    ASTERIA_EXPECT(t, "overlong/surrogate/out-of-range/truncated UTF-8 becomes replacement",
                   Shape(font, {bad, sizeof(bad)}, {}, run) == Error::None, run.Count == sizeof(bad));
    bool replacements{true};
    for (UInt32 i{}; i < run.Count; ++i)
    {
        replacements = replacements && run.Glyphs[i].Scalar == 0xFFFDU && run.Glyphs[i].Cluster == i;
    }
    ASTERIA_EXPECT(t, "every malformed byte has a replacement cluster", replacements);
    run.Capacity = 1U;
    const UInt8 text[]{'A', 'V'};
    ASTERIA_EXPECT(t, "layout output bound", Shape(font, {text, sizeof(text)}, {}, run) == Error::BufferTooSmall);
    run.Capacity = 32U;
    const UInt8 spaces[]{'A', ' ', ' ', 'V', ' ', '\n'};
    ASTERIA_EXPECT(t, "space fixture shapes", Shape(font, {spaces, sizeof(spaces)}, {}, run) == Error::None);
    Line lines[8U]{};
    UInt32 count{};
    ASTERIA_EXPECT(t, "wrapping trims trailing spaces and retains final empty LF line",
                   Wrap(run, 30 * One, lines, 8U, count) == Error::None, count == 3U, lines[0U].Count == 1U,
                   lines[0U].Width == font.Horizontal(font.Glyph('A')).Advance * 16 * One / 1000,
                   lines[2U].Count == 0U);
    positioned[0U].AdvanceX = (-0x7FFFFFFFFFFFFFFFLL - 1);
    ASTERIA_EXPECT(t, "wrapping rejects unbounded external advances",
                   Wrap(run, 30 * One, lines, 8U, count) == Error::CoordinateLimit);
    Outline outline{commands, MaximumCommands, 0U, 0};
    ASTERIA_EXPECT(t, "invalid glyph has a clear error",
                   LoadOutline(font, 65535U, outlineWorkspace, outline) == Error::InvalidGlyph);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext t{"Font bounds"};
    CmapRange(t);
    RasterLimits(t);
    NameWork(t);
    LayoutLimits(t);
    return t.Finish();
}
