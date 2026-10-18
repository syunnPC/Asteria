// SPDX-License-Identifier: Apache-2.0
#include "User/Font/Outline.hpp"
#include "User/Font/Layout.hpp"
#include "User/Font/Fixture.hpp"
#include <cstddef>
#include <cstdint>
#include <cstdlib>
using Asteria::Foundation::UInt16;
namespace
{
using Asteria::User::Font::Bytes;
using Asteria::User::Font::Command;
using Asteria::User::Font::Error;
using Asteria::User::Font::Font;
using Asteria::User::Font::LoadOutline;
using Asteria::User::Font::MaximumCommands;
using Asteria::User::Font::MaximumCoordinate;
using Asteria::User::Font::Outline;
using Asteria::User::Font::OutlineWorkspace;
using Asteria::User::Font::PositionedGlyph;
using Asteria::User::Font::Run;
using Asteria::User::Font::Shape;
using Asteria::User::Font::Size;
using Asteria::User::Font::Tag;
using Asteria::User::Font::UInt32;
using Asteria::User::Font::UInt8;
using Asteria::TestSupport::FontFixture::Make;
using Asteria::TestSupport::FontFixture::TableOffset;
static OutlineWorkspace workspace{};
static Command commands[MaximumCommands]{};
static PositionedGlyph glyphs[32U]{};
void Check(const Bytes bytes) noexcept
{
    Font font{};
    const Error error{font.Open(bytes)};
    if (error != Error::None)
    {
        if (font.IsOpen())
        {
            std::abort();
        }
        return;
    }
    if (!font.IsOpen() || font.GetMetrics().UnitsPerEm < 16U || font.GetMetrics().GlyphCount == 0U)
    {
        std::abort();
    }
    for (UInt32 i{}; i < 16U && i < font.GetMetrics().GlyphCount; ++i)
    {
        Outline outline{commands, MaximumCommands, 0U, 0};
        const auto e = LoadOutline(font, static_cast<UInt16>(i), workspace, outline);
        if (outline.Count > MaximumCommands)
        {
            std::abort();
        }
        if (e == Error::None)
        {
            for (UInt32 j{}; j < outline.Count; ++j)
            {
                const auto& c = commands[j];
                if (c.A.X < -MaximumCoordinate || c.A.X > MaximumCoordinate || c.A.Y < -MaximumCoordinate ||
                    c.A.Y > MaximumCoordinate)
                {
                    std::abort();
                }
            }
        }
    }
    const UInt8 text[]{'A', 'V', ' ', 'f', 'i', ' ', 'f', 'l', 0xFFU};
    Run run{glyphs, 32U, 0U, 0, 0};
    static_cast<void>(Shape(font, {text, sizeof(text)}, {}, run));
    if (run.Count > 32U)
    {
        std::abort();
    }
}
} // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) noexcept
{
    Check({data, size});
    if (size == 0U)
    {
        return 0;
    }
    // Keep a valid directory around payload mutations to reach nested parsers.
    auto synthetic = Make((data[0U] & 1U) != 0U, (data[0U] & 2U) != 0U);
    const UInt32 tags[]{Tag('C', 'F', 'F', ' '), Tag('g', 'l', 'y', 'f'), Tag('c', 'm', 'a', 'p'),
                        Tag('G', 'P', 'O', 'S'), Tag('G', 'S', 'U', 'B'), Tag('n', 'a', 'm', 'e')};
    const Size offset{TableOffset(synthetic, tags[data[0U] % 6U])};
    if (offset != 0U)
    {
        for (Size i{1U}; i < size && offset + i - 1U < synthetic.size() && i < 257U; ++i)
        {
            synthetic[offset + i - 1U] ^= data[i];
        }
        Check({synthetic.data(), synthetic.size()});
    }
    return 0;
}
extern "C" void AsteriaFuzzSeeds(void (*const emit)(const std::uint8_t*, std::size_t)) noexcept
{
    for (const bool cff : {false, true})
    {
        for (const bool extension : {false, true})
        {
            const auto b = Make(cff, extension);
            emit(b.data(), b.size());
            emit(b.data(), 12U);
        }
    }
    const UInt8 invalid[]{0U};
    emit(invalid, 0U);
    emit(invalid, sizeof(invalid));
}
