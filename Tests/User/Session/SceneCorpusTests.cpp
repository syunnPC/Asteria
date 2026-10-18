// SPDX-License-Identifier: Apache-2.0
#include "Abi/ComponentBootstrap.hpp"
#include "User/Session/Scene.hpp"
#include "User/Session/UiText.hpp"
#include "User/Terminal/Terminal.hpp"
#include "User/Terminal/TerminalRenderer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Support/RenderCorpus.hpp"
#include "Support/TestContext.hpp"
#include "User/Graphics/PixelFormat.hpp"
#include "User/Graphics/Surface.hpp"

// The scene corpus (SceneCorpus.txt): renders every case with Scene, checks
// that the damage-driven Compose and the naive RenderAll agree, and compares
// the PPM's SHA-256 with SceneDigests.txt, which the runner's model
// (Tools/Vm/AsteriaVm/Session.py, Tools/Vm/Tests/test_session_render.py)
// checks too. To regenerate the digests after an intended change of the
// look, run this binary with ASTERIA_RENDER_DIGESTS_OUT=<repo>/Tests/User/
// Session/SceneDigests.txt, then run the runner's unit tests: they must pass
// unchanged. ASTERIA_RENDER_OUTPUT_DIR receives the PPMs.

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Int32;
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt8;
using User::Graphics::Color;
using User::Graphics::MemorySurface;
using User::Graphics::PixelFormat;
using User::Graphics::Rect;
using User::Session::ButtonState;
using User::Session::ClientClass;
using User::Session::ImageView;
using User::Session::Scene;
using User::Session::TitleState;
using User::Session::WindowLook;

constexpr Size MaximumPixels{1024U * 768U};
constexpr Size ImagePixels{2U * 1024U * 1024U};

struct CaseWindow
{
    Rect Frame;
    WindowLook Look;
    ClientClass Class;
    UInt8 Name[User::Session::TitleNameBytes];
    Size NameLength;
    UInt8 Subtitle[User::Session::SubtitleBytes];
    Size SubtitleLength;
    ImageView Image;
    bool HasImage;
};

UInt32 g_output[MaximumPixels]{};
UInt32 g_reference[MaximumPixels]{};
UInt32 g_images[ImagePixels]{};
Size g_imageUsed{0U};
std::uint8_t g_ppm[64U + (3U * MaximumPixels)]{};
char g_corpus[1U << 16U]{};
Scene g_scene{};
User::Session::UiText g_ui{};
User::Session::UiText g_fallback{};
UInt8 g_fontBytes[2U][Abi::ComponentUiFontFaceBytes]{};
bool g_useFallback{false};
[[nodiscard]] bool OpenUi() noexcept
{
    Size lengths[2U]{};
    const char* const paths[]{ASTERIA_FONT_DATA_DIR "/Vegur-Bold.otf", ASTERIA_FONT_DATA_DIR "/Vegur-Regular.otf"};
    for (Size i{}; i < 2U; ++i)
    {
        auto* const file = std::fopen(paths[i], "rb");
        if (file == nullptr)
        {
            return false;
        }
        lengths[i] = std::fread(g_fontBytes[i], 1U, sizeof(g_fontBytes[i]), file);
        const bool valid{std::fgetc(file) == EOF && !std::ferror(file)};
        std::fclose(file);
        if (!valid)
        {
            return false;
        }
    }
    static_cast<void>(g_fallback.Open({}, {}));
    return g_ui.Open({g_fontBytes[0U], lengths[0U]}, {g_fontBytes[1U], lengths[1U]});
}
CaseWindow g_windows[Scene::MaximumWindows]{};
Size g_windowCount{0U};
UInt8 g_switcher[256U]{};
Size g_switcherLength{0U};
bool g_hasSwitcher{false};
Rect g_outline{};
bool g_hasOutline{false};
TestSupport::RenderDigests g_digests{};

/// Room for an image of width x height pixels, or null.
[[nodiscard]] UInt32* TakeImage(const UInt32 width, const UInt32 height) noexcept
{
    const Size pixels = static_cast<Size>(width) * height;
    if ((width == 0U) || (height == 0U) || (pixels > (ImagePixels - g_imageUsed)))
    {
        return nullptr;
    }
    UInt32* const image = g_images + g_imageUsed;
    g_imageUsed += pixels;
    return image;
}

[[nodiscard]] bool PatternImage(CaseWindow& window, const PixelFormat format, const std::uint64_t seed,
                                const UInt32 width, const UInt32 height) noexcept
{
    UInt32* const pixels = TakeImage(width, height);
    if (pixels == nullptr)
    {
        return false;
    }
    for (UInt32 y{0U}; y < height; ++y)
    {
        for (UInt32 x{0U}; x < width; ++x)
        {
            const Color color{.Red = static_cast<UInt8>((x * 3U) + (seed * 7U)),
                              .Green = static_cast<UInt8>((y * 5U) + (seed * 11U)),
                              .Blue = static_cast<UInt8>((x ^ y) + (seed * 13U))};
            pixels[(static_cast<Size>(y) * width) + x] = User::Graphics::PackColor(format, color);
        }
    }
    window.Image = ImageView{.Pixels = pixels, .Width = width, .Height = height, .Stride = width, .Format = format};
    window.HasImage = true;
    return true;
}

[[nodiscard]] bool SolidImage(CaseWindow& window, const PixelFormat format, const Color color, const UInt32 width,
                              const UInt32 height) noexcept
{
    UInt32* const pixels = TakeImage(width, height);
    if (pixels == nullptr)
    {
        return false;
    }
    const UInt32 word = User::Graphics::PackColor(format, color);
    for (Size index{0U}; index < (static_cast<Size>(width) * height); ++index)
    {
        pixels[index] = word;
    }
    window.Image = ImageView{.Pixels = pixels, .Width = width, .Height = height, .Stride = width, .Format = format};
    window.HasImage = true;
    return true;
}

[[nodiscard]] bool Unsigned(const TestSupport::CorpusReader& reader, const Size index, UInt32& value) noexcept
{
    std::uint64_t number{0U};
    if (!reader.Number(index, number) || (number > 0xFFFF'FFFFU))
    {
        return false;
    }
    value = static_cast<UInt32>(number);
    return true;
}

[[nodiscard]] bool Coordinate(const TestSupport::CorpusReader& reader, const Size index, Int32& value) noexcept
{
    std::int64_t number{0};
    if (!reader.Signed(index, number) || (number < -1'000'000) || (number > 1'000'000))
    {
        return false;
    }
    value = static_cast<Int32>(number);
    return true;
}

[[nodiscard]] bool ParseWindow(const TestSupport::CorpusReader& reader, const UInt32 width,
                               const UInt32 height) noexcept
{
    if (g_windowCount == Scene::MaximumWindows)
    {
        return false;
    }
    CaseWindow& window = g_windows[g_windowCount];
    window = CaseWindow{};
    window.Class = ClientClass::SystemModule;
    window.Look = WindowLook{.State = TitleState::Unfocused,
                             .NotResponding = false,
                             .Maximized = false,
                             .Close = ButtonState::Raised,
                             .Maximize = ButtonState::Raised};
    Size next{0U};
    if (reader.Is(1U, "maximized"))
    {
        window.Frame = User::Session::MaximizedFrame(Rect{.X = 0, .Y = 0, .Width = width, .Height = height});
        window.Look.Maximized = true;
        next = 2U;
    }
    else
    {
        if (!Coordinate(reader, 1U, window.Frame.X) || !Coordinate(reader, 2U, window.Frame.Y) ||
            !Unsigned(reader, 3U, window.Frame.Width) || !Unsigned(reader, 4U, window.Frame.Height))
        {
            return false;
        }
        next = 5U;
    }
    if (reader.Is(next, "focused"))
    {
        window.Look.State = TitleState::Focused;
    }
    else if (reader.Is(next, "attention"))
    {
        window.Look.State = TitleState::Attention;
    }
    else if (!reader.Is(next, "unfocused"))
    {
        return false;
    }
    for (Size index{next + 1U}; index < reader.Count(); ++index)
    {
        if (reader.Is(index, "notresponding"))
        {
            window.Look.NotResponding = true;
        }
        else if (reader.Is(index, "volume"))
        {
            window.Class = ClientClass::VolumeBundle;
        }
        else if (reader.Is(index, "close-pressed"))
        {
            window.Look.Close = ButtonState::Pressed;
        }
        else if (reader.Is(index, "maximize-pressed"))
        {
            window.Look.Maximize = ButtonState::Pressed;
        }
        else
        {
            return false;
        }
    }
    ++g_windowCount;
    return true;
}

char g_terminalCorpus[256U * 1024U];
UInt8 g_terminalBytes[4096U];
User::Terminal::Terminal g_terminal;
[[nodiscard]] bool TerminalImage(CaseWindow& window, const PixelFormat format, const char* name) noexcept
{
    Size bytes{0U};
    if (!TestSupport::ReadRenderFile(ASTERIA_RENDER_CORPUS_DIR "/Terminal/TerminalCorpus.txt", g_terminalCorpus,
                                     sizeof(g_terminalCorpus), bytes))
    {
        return false;
    }
    TestSupport::CorpusReader r{g_terminalCorpus, bytes};
    bool found{false};
    while (r.NextLine())
    {
        if (!found)
        {
            if (!r.Is(0U, "case") || !r.Is(2U, name))
            {
                continue;
            }
            UInt32 cols{0U};
            UInt32 rows{0U};
            if (!Unsigned(r, 3U, cols) || !Unsigned(r, 4U, rows))
            {
                return false;
            }
            g_terminal.Configure(cols, rows);
            found = true;
            continue;
        }
        if (r.Is(0U, "end"))
        {
            const auto& screen = g_terminal.Screen();
            const UInt32 width = screen.Columns() * User::Terminal::TerminalCellWidth;
            const UInt32 height = screen.Rows() * User::Terminal::TerminalCellHeight;
            UInt32* const pixels = TakeImage(width, height);
            if (pixels == nullptr)
            {
                return false;
            }
            const MemorySurface surface{pixels, width, height, width, format};
            User::Terminal::RenderTerminal(screen, surface, {0, 0});
            window.Image = {pixels, width, height, width, format};
            window.HasImage = true;
            return true;
        }
        if (r.Is(0U, "write"))
        {
            Size length{0U};
            if (!r.Text(1U, g_terminalBytes, sizeof(g_terminalBytes), length))
            {
                return false;
            }
            g_terminal.Write(g_terminalBytes, length);
        }
        else if (r.Is(0U, "resize"))
        {
            UInt32 cols{0U};
            UInt32 rows{0U};
            if (!Unsigned(r, 1U, cols) || !Unsigned(r, 2U, rows))
            {
                return false;
            }
            g_terminal.Resize(cols, rows);
        }
        else
        {
            return false;
        }
    }
    return false;
}

[[nodiscard]] bool ParseImage(const TestSupport::CorpusReader& reader, const PixelFormat format) noexcept
{
    if (g_windowCount == 0U)
    {
        return false;
    }
    CaseWindow& window = g_windows[g_windowCount - 1U];
    if (reader.Is(1U, "none") && (reader.Count() == 2U))
    {
        window.HasImage = false;
        return true;
    }
    if (reader.Is(1U, "terminal") && (reader.Count() == 3U))
    {
        char name[64U]{};
        return reader.Word(2U, name, sizeof(name)) && TerminalImage(window, format, name);
    }
    UInt32 values[5U]{};
    if (reader.Is(1U, "pattern") && (reader.Count() == 5U))
    {
        return Unsigned(reader, 2U, values[0U]) && Unsigned(reader, 3U, values[1U]) &&
               Unsigned(reader, 4U, values[2U]) && PatternImage(window, format, values[0U], values[1U], values[2U]);
    }
    if (reader.Is(1U, "solid") && (reader.Count() == 7U))
    {
        for (Size index{0U}; index < 5U; ++index)
        {
            if (!Unsigned(reader, 2U + index, values[index]) || ((index < 3U) && (values[index] > 255U)))
            {
                return false;
            }
        }
        return SolidImage(window, format,
                          Color{.Red = static_cast<UInt8>(values[0U]),
                                .Green = static_cast<UInt8>(values[1U]),
                                .Blue = static_cast<UInt8>(values[2U])},
                          values[3U], values[4U]);
    }
    return false;
}

/// Reads one case's statements up to `end`.
[[nodiscard]] bool ParseCase(TestSupport::CorpusReader& reader, const UInt32 width, const UInt32 height,
                             const PixelFormat format) noexcept
{
    g_useFallback = false;
    g_windowCount = 0U;
    g_imageUsed = 0U;
    g_hasSwitcher = false;
    g_hasOutline = false;
    while (reader.NextLine())
    {
        if (reader.Is(0U, "end"))
        {
            return reader.Count() == 1U;
        }
        bool parsed{false};
        if (reader.Is(0U, "font") && reader.Count() == 2U && (reader.Is(1U, "fallback") || reader.Is(1U, "vegur")))
        {
            g_useFallback = reader.Is(1U, "fallback");
            parsed = true;
        }
        else if (reader.Is(0U, "window"))
        {
            parsed = ParseWindow(reader, width, height);
        }
        else if (reader.Is(0U, "title") && (reader.Count() == 3U) && (g_windowCount != 0U))
        {
            CaseWindow& window = g_windows[g_windowCount - 1U];
            parsed = reader.Text(1U, window.Name, sizeof(window.Name), window.NameLength) &&
                     reader.Text(2U, window.Subtitle, sizeof(window.Subtitle), window.SubtitleLength);
        }
        else if (reader.Is(0U, "image"))
        {
            parsed = ParseImage(reader, format);
        }
        else if (reader.Is(0U, "switcher") && (reader.Count() == 2U))
        {
            parsed = reader.Text(1U, g_switcher, sizeof(g_switcher), g_switcherLength);
            g_hasSwitcher = true;
        }
        else if (reader.Is(0U, "outline") && (reader.Count() == 5U))
        {
            parsed = Coordinate(reader, 1U, g_outline.X) && Coordinate(reader, 2U, g_outline.Y) &&
                     Unsigned(reader, 3U, g_outline.Width) && Unsigned(reader, 4U, g_outline.Height);
            g_hasOutline = true;
        }
        if (!parsed)
        {
            return false;
        }
    }
    return false;
}

/// Builds the scene of the parsed case through the public interface.
[[nodiscard]] bool BuildScene(const UInt32 width, const UInt32 height, const PixelFormat format) noexcept
{
    g_scene.SetUiText(g_useFallback ? &g_fallback : &g_ui);
    g_scene.Configure(width, height, format);
    for (Size slot{0U}; slot < g_windowCount; ++slot)
    {
        const CaseWindow& window = g_windows[slot];
        if (!g_scene.Show(slot, window.Frame))
        {
            return false;
        }
        if ((window.NameLength != 0U) && !g_scene.SetTitle(slot, window.Name, window.NameLength, window.Class))
        {
            return false;
        }
        if (!g_scene.SetSubtitle(slot, window.Subtitle, window.SubtitleLength))
        {
            return false;
        }
        g_scene.SetLook(slot, window.Look);
        if (window.HasImage)
        {
            g_scene.Present(slot, window.Image, nullptr, 0U);
        }
    }
    if (g_hasSwitcher)
    {
        g_scene.ShowSwitcher(g_switcher, g_switcherLength);
    }
    if (g_hasOutline)
    {
        g_scene.ShowOutline(g_outline);
    }
    return true;
}

void RunCase(TestContext& runner, TestSupport::CorpusReader& reader) noexcept
{
    char name[TestSupport::RenderNameBytes]{};
    UInt32 width{0U};
    UInt32 height{0U};
    const bool header = (reader.Count() == 6U) && reader.Word(2U, name, sizeof(name)) && Unsigned(reader, 3U, width) &&
                        Unsigned(reader, 4U, height) && (width != 0U) && (height != 0U) &&
                        ((static_cast<Size>(width) * height) <= MaximumPixels) &&
                        (reader.Is(5U, "rgbx") || reader.Is(5U, "bgrx"));
    if (!ASTERIA_EXPECT(runner, "a scene case header is <name> <width> <height> <format>", header))
    {
        std::fprintf(stderr, "  at corpus line %d\n", reader.Line());
        return;
    }
    const PixelFormat format = reader.Is(5U, "rgbx") ? PixelFormat::Rgbx : PixelFormat::Bgrx;
    if (!ASTERIA_EXPECT(runner, "a scene case parses", ParseCase(reader, width, height, format),
                        BuildScene(width, height, format)))
    {
        std::fprintf(stderr, "  in case %s near corpus line %d\n", name, reader.Line());
        return;
    }
    const MemorySurface output{.Pixels = g_output, .Width = width, .Height = height, .Stride = width, .Format = format};
    const MemorySurface reference{
        .Pixels = g_reference, .Width = width, .Height = height, .Stride = width, .Format = format};
    for (Size index{0U}; index < (static_cast<Size>(width) * height); ++index)
    {
        g_output[index] = 0xDEAD'BEEFU;
        g_reference[index] = 0x0BAD'F00DU;
    }
    static_cast<void>(g_scene.Compose(output));
    g_scene.RenderAll(reference);
    const bool same = std::memcmp(g_output, g_reference, static_cast<Size>(width) * height * 4U) == 0;
    if (!ASTERIA_EXPECT(runner, "Compose of a fresh scene equals RenderAll", same))
    {
        std::fprintf(stderr, "  in case %s\n", name);
    }
    const Size size = TestSupport::EncodeRenderPpm(reference, g_ppm, sizeof(g_ppm));
    char hex[65U]{};
    TestSupport::RenderSha256(g_ppm, size, hex);
    g_digests.Record(name, hex);
    const char* const expected = g_digests.Find(name);
    if (!ASTERIA_EXPECT(runner, "the PPM matches its committed digest", size != 0U,
                        TestSupport::WriteRenderOutput(name, g_ppm, size), expected != nullptr,
                        (expected != nullptr) && (std::strcmp(expected, hex) == 0)))
    {
        std::fprintf(stderr, "  case %s renders %s\n", name, hex);
    }
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SceneCorpusTests"};
    ASTERIA_EXPECT(runner, "both UI fonts validate in the bounded owner", Asteria::Test::OpenUi());
    using Asteria::Test::g_corpus;
    std::size_t size{0U};
    const bool read = Asteria::TestSupport::ReadRenderFile(ASTERIA_RENDER_CORPUS_DIR "/Session/SceneCorpus.txt",
                                                           g_corpus, sizeof(g_corpus), size);
    const bool digests = Asteria::Test::g_digests.Load(ASTERIA_RENDER_CORPUS_DIR "/Session/SceneDigests.txt");
    ASTERIA_EXPECT(runner, "the corpus and its digests are read", read, digests);
    Asteria::TestSupport::CorpusReader reader{g_corpus, size};
    std::size_t cases{0U};
    while (reader.NextLine())
    {
        if (ASTERIA_EXPECT(runner, "every corpus statement outside a case starts one", reader.Is(0U, "case"),
                           reader.Is(1U, "scene")))
        {
            Asteria::Test::RunCase(runner, reader);
            ++cases;
        }
    }
    ASTERIA_EXPECT(runner, "the corpus is well formed and covers every digest", !reader.Malformed(), cases != 0U,
                   cases == Asteria::Test::g_digests.Count());
    ASTERIA_EXPECT(runner, "the computed digests are written when asked",
                   Asteria::Test::g_digests.WriteRecorded(
                       "# SHA-256 of each SceneCorpus.txt case's PPM (SceneCorpusTests.cpp).\n"));
    return runner.Finish();
}
