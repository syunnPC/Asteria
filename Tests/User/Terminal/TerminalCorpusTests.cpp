// SPDX-License-Identifier: Apache-2.0
#include "User/Terminal/Terminal.hpp"
#include "User/Terminal/TerminalRenderer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Support/RenderCorpus.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

// Renders the terminal corpus (TerminalCorpus.txt) with User/Terminal and
// checks each image's SHA-256 against TerminalDigests.txt, which the runner's
// model (Tools/Vm/AsteriaVm/Terminal.py, Tools/Vm/Tests/test_terminal.py)
// must reproduce byte for byte. ASTERIA_RENDER_OUTPUT_DIR receives the PPMs
// for the runner's direct comparison. To regenerate the digests after a
// deliberate rendering change, run this test with
// ASTERIA_RENDER_DIGESTS_OUT=Tests/User/Terminal/TerminalDigests.txt, then
// run the runner's unit tests, which must agree with the new file.
namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using TestSupport::CorpusReader;
using User::Graphics::MemorySurface;
using User::Graphics::PixelFormat;

constexpr UInt32 MaximumWidth{1056U};
constexpr UInt32 MaximumHeight{800U};
constexpr Size MaximumWrite{4096U};

char g_corpus[256U * 1024U];
UInt32 g_pixels[MaximumWidth * MaximumHeight];
std::uint8_t g_ppm[64U + (MaximumWidth * MaximumHeight * 3U)];
UInt8 g_bytes[MaximumWrite];
User::Terminal::Terminal g_terminal;
TestSupport::RenderDigests g_digests;

constexpr const char DigestHeader[]{
    "# SHA-256 of each TerminalCorpus.txt case's PPM (EncodeRenderPpm). Written by\n"
    "# TerminalCorpusTests with ASTERIA_RENDER_DIGESTS_OUT; Tools/Vm/Tests/test_terminal.py\n"
    "# checks that the runner's model renders the same bytes.\n"};

/// Renders the case starting at the reader's `case` line; false on a
/// malformed case.
[[nodiscard]] bool RenderCase(CorpusReader& reader, MemorySurface& surface) noexcept
{
    std::uint64_t columns{0U};
    std::uint64_t rows{0U};
    if ((reader.Count() != 6U) || !reader.Number(3U, columns) || !reader.Number(4U, rows) ||
        (!reader.Is(5U, "rgbx") && !reader.Is(5U, "bgrx")) || (columns == 0U) || (rows == 0U) ||
        ((columns * 8U) > MaximumWidth) || ((rows * 16U) > MaximumHeight))
    {
        return false;
    }
    const PixelFormat format{reader.Is(5U, "rgbx") ? PixelFormat::Rgbx : PixelFormat::Bgrx};
    g_terminal.Configure(static_cast<UInt32>(columns), static_cast<UInt32>(rows));
    while (reader.NextLine())
    {
        if (reader.Is(0U, "end") && (reader.Count() == 1U))
        {
            const auto& screen = g_terminal.Screen();
            surface = MemorySurface{.Pixels = g_pixels,
                                    .Width = screen.Columns() * 8U,
                                    .Height = screen.Rows() * 16U,
                                    .Stride = screen.Columns() * 8U,
                                    .Format = format};
            for (UInt32& pixel : g_pixels)
            {
                pixel = 0xDEAD'BEEFU;
            }
            User::Terminal::RenderTerminal(screen, surface, User::Graphics::Point{.X = 0, .Y = 0});
            return true;
        }
        if (reader.Is(0U, "write") && (reader.Count() == 2U))
        {
            Size length{0U};
            if (!reader.Text(1U, g_bytes, sizeof(g_bytes), length))
            {
                return false;
            }
            g_terminal.Write(g_bytes, length);
            continue;
        }
        std::uint64_t newColumns{0U};
        std::uint64_t newRows{0U};
        if (reader.Is(0U, "resize") && (reader.Count() == 3U) && reader.Number(1U, newColumns) &&
            reader.Number(2U, newRows) && (newColumns != 0U) && (newRows != 0U) &&
            ((newColumns * 8U) <= MaximumWidth) && ((newRows * 16U) <= MaximumHeight))
        {
            g_terminal.Resize(static_cast<UInt32>(newColumns), static_cast<UInt32>(newRows));
            continue;
        }
        return false;
    }
    return false;
}

void TestCorpus(TestContext& runner) noexcept
{
    Size size{0U};
    const bool read{TestSupport::ReadRenderFile(ASTERIA_RENDER_CORPUS_DIR "/Terminal/TerminalCorpus.txt", g_corpus,
                                                sizeof(g_corpus), size)};
    const bool digests{g_digests.Load(ASTERIA_RENDER_CORPUS_DIR "/Terminal/TerminalDigests.txt")};
    if (!ASTERIA_EXPECT(runner, "the corpus and its digests are readable", read, digests))
    {
        return;
    }
    CorpusReader reader{g_corpus, size};
    Size cases{0U};
    while (reader.NextLine())
    {
        char name[TestSupport::RenderNameBytes]{};
        if (!reader.Is(0U, "case") || !reader.Is(1U, "terminal") || !reader.Word(2U, name, sizeof(name)))
        {
            static_cast<void>(std::fprintf(stderr, "line %d: not a terminal case\n", reader.Line()));
            ASTERIA_FAIL(runner, "the corpus holds only terminal cases");
            return;
        }
        MemorySurface surface{};
        if (!RenderCase(reader, surface))
        {
            static_cast<void>(std::fprintf(stderr, "case %s, line %d: malformed\n", name, reader.Line()));
            ASTERIA_FAIL(runner, "a corpus case is well formed");
            return;
        }
        ++cases;
        const Size bytes{TestSupport::EncodeRenderPpm(surface, g_ppm, sizeof(g_ppm))};
        char hex[65U]{};
        TestSupport::RenderSha256(g_ppm, bytes, hex);
        g_digests.Record(name, hex);
        const char* const expected{g_digests.Find(name)};
        const bool written{TestSupport::WriteRenderOutput(name, g_ppm, bytes)};
        if (!ASTERIA_EXPECT(runner, name, bytes != 0U, expected != nullptr,
                            (expected != nullptr) && (std::strcmp(expected, hex) == 0), written))
        {
            static_cast<void>(std::fprintf(stderr, "  %s renders %s\n", name, hex));
        }
    }
    ASTERIA_EXPECT(runner, "the corpus parsed to its end", !reader.Malformed(), cases >= 10U,
                   cases == g_digests.Count());
    ASTERIA_EXPECT(runner, "the digests were written where asked", g_digests.WriteRecorded(DigestHeader));
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"TerminalCorpusTests"};
    Asteria::Test::TestCorpus(runner);
    return runner.Finish();
}
