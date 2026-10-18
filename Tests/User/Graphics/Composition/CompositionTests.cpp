// SPDX-License-Identifier: Apache-2.0
#include "User/Graphics/Composition/Composition.hpp"
#include "User/Graphics/Composition/SessionPlane.hpp"
#include "User/Session/Palette.hpp"

#include <cstdio>
#include <initializer_list>
#include "Support/Random.hpp"
#include "Support/TestContext.hpp"
#include "User/Component/DisplayPointer.hpp"
#include "User/Graphics/Blit.hpp"

namespace
{
namespace F = Asteria::Foundation;
namespace G = Asteria::User::Graphics;
namespace C = G::Composition;
namespace Console = Asteria::User::Console;
namespace Protocol = Asteria::User::Component;
using Asteria::TestSupport::TestContext;
constexpr F::UInt32 Width{130U};
constexpr F::UInt32 Height{81U};
constexpr F::UInt32 Stride{139U};
constexpr F::UInt32 Sentinel{0xABCD1234U};
F::UInt32 g_actual[Stride * Height]{};
F::UInt32 g_expected[Stride * Height]{};
F::UInt32 g_sessionPixels[Stride * Height]{};
C::SessionPlane g_sessionPlane{};
Console::TextConsole g_console{};
C::Composer g_composer{};
C::DamageList g_damage{};
C::CursorPlane g_cursor{};
C::BackgroundPlane g_background{Console::ConsoleBackground};
C::ConsolePlane g_consolePlane{g_console};
F::Size g_orders{0U};
void Order() noexcept
{
    ++g_orders;
}
G::Surface Surface(F::UInt32* pixels, G::PixelFormat format) noexcept
{
    return {.Pixels = pixels, .Width = Width, .Height = Height, .Stride = Stride, .Format = format};
}
void Configure(TestContext& test, G::PixelFormat format)
{
    for (F::Size index{0U}; index < Stride * Height; ++index)
    {
        g_actual[index] = Sentinel;
        g_expected[index] = Sentinel;
    }
    g_console.Configure(Width, Height, G::Bold8x16);
    g_composer.Reset();
    g_damage.Configure(G::Bounds(Surface(g_actual, format)));
    g_cursor.Move({.X = 65, .Y = 40});
    g_cursor.Show(true);
    ASTERIA_EXPECT(
        test, "static plane/output registration", g_cursor.SetImage(C::Arrow),
        g_composer.AddOutput({.Scanout = Surface(g_actual, format), .Origin = {.X = 0, .Y = 0}, .Scale = 1U}),
        g_composer.AddPlane(g_background), g_composer.AddPlane(g_consolePlane), g_composer.AddPlane(g_cursor));
    g_damage.Add(G::Bounds(Surface(g_actual, format)));
    g_orders = 0U;
}

// Full reference renderer uses the original glyph drawing path and derives the
// arrow's outline by expanding each foreground bit, independently of MakeArrow.
void Reference(G::PixelFormat format, G::Point position, bool dot = false, bool session = false)
{
    const G::Surface target = Surface(g_expected, format);
    G::FillRect(target, G::Bounds(target), Console::ConsoleBackground);
    if (session)
    {
        for (F::UInt32 y{0U}; y < Height; ++y)
        {
            for (F::UInt32 x{0U}; x < Width; ++x)
            {
                g_expected[y * Stride + x] = g_sessionPixels[y * Stride + x];
            }
        }
    }
    for (F::UInt32 row{0U}; !session && row < g_console.Rows(); ++row)
    {
        for (F::UInt32 column{0U}; column < g_console.Columns(); ++column)
        {
            G::DrawGlyph(target, static_cast<F::Int32>(column * 8U), static_cast<F::Int32>(row * 16U), G::Bold8x16,
                         g_console.CellAt(row, column).Code, Console::ConsoleForeground, Console::ConsoleBackground);
        }
    }
    const G::Color black{};
    if (dot)
    {
        G::FillRect(target, {.X = position.X - 2, .Y = position.Y - 2, .Width = 5U, .Height = 5U}, black);
        G::FillRect(target, {.X = position.X - 1, .Y = position.Y - 1, .Width = 3U, .Height = 3U},
                    Console::ConsoleForeground);
        return;
    }
    for (F::Int32 y{0}; y < 16; ++y)
    {
        for (F::Int32 x{0}; x < 8; ++x)
        {
            if ((C::ArrowMask[y] & (0x80U >> static_cast<F::UInt32>(x))) != 0U)
            {
                G::FillRect(target, {.X = position.X + x - 1, .Y = position.Y + y - 1, .Width = 3U, .Height = 3U},
                            black);
            }
        }
    }
    for (F::Int32 y{0}; y < 16; ++y)
    {
        for (F::Int32 x{0}; x < 8; ++x)
        {
            if ((C::ArrowMask[y] & (0x80U >> static_cast<F::UInt32>(x))) != 0U)
            {
                G::FillRect(target, {.X = position.X + x, .Y = position.Y + y, .Width = 1U, .Height = 1U},
                            Console::ConsoleForeground);
            }
        }
    }
}
[[nodiscard]] bool Equal() noexcept
{
    for (F::Size index{0U}; index < Stride * Height; ++index)
    {
        if (g_actual[index] != g_expected[index])
        {
            return false;
        }
    }
    return true;
}
void DrawConsoleDamage()
{
    const Console::ConsoleDamage rows = g_console.Damage();
    g_damage.Add({.X = 0,
                  .Y = static_cast<F::Int32>(rows.First * 16U),
                  .Width = Width,
                  .Height = (rows.End - rows.First) * 16U});
    g_console.MarkDrawn();
}
void Move(G::Point position)
{
    g_damage.Add(g_cursor.Bounds());
    g_cursor.Move(position);
    g_damage.Add(g_cursor.Bounds());
}
void Pixels(TestContext& test)
{
    for (G::PixelFormat format : {G::PixelFormat::Bgrx, G::PixelFormat::Rgbx})
    {
        Configure(test, format);
        const F::UInt8 text[]{'H', 'e', 'l', 'l', 'o', '\n', 'A'};
        g_console.Write(text, sizeof(text));
        DrawConsoleDamage();
        // Includes all corners, each edge's centre, text and both tile seams.
        for (G::Point point : {G::Point{0, 0},
                               {129, 0},
                               {0, 80},
                               {129, 80},
                               {0, 40},
                               {129, 40},
                               {65, 0},
                               {65, 80},
                               {4, 4},
                               {63, 63},
                               {64, 64}})
        {
            Move(point);
            const F::Size before = g_orders;
            g_composer.Render(g_damage, Order);
            Reference(format, point);
            ASTERIA_EXPECT(test, "arrow, derived outline, clipped edges, glyphs, seams and untouched pitch", Equal(),
                           g_orders == before + 1U, g_damage.Count() == 0U);
        }
        const F::UInt8 scroll[]{'Z', '\n'};
        for (F::Size line{0U}; line < 7U; ++line)
        {
            g_console.Write(scroll, sizeof(scroll));
            DrawConsoleDamage();
            g_composer.Render(g_damage, Order);
            Reference(format, g_cursor.Position());
            ASTERIA_EXPECT(test, "scroll recomposes cursor with shifted glyph cells", Equal());
        }
        g_damage.Add(g_cursor.Bounds());
        ASTERIA_EXPECT(test, "alternate hot spot accepted", g_cursor.SetImage(C::Dot));
        Move({.X = 64, .Y = 64});
        g_composer.Render(g_damage, Order);
        Reference(format, g_cursor.Position(), true);
        ASTERIA_EXPECT(test, "dot centre is pointer position", Equal());
        const F::Size before = g_orders;
        g_composer.Render(g_damage, Order);
        ASTERIA_EXPECT(test, "empty pass has no store-ordering call", before == g_orders);
    }
}
void Damage(TestContext& test)
{
    C::DamageList damage{};
    damage.Configure({.X = 0, .Y = 0, .Width = 100U, .Height = 100U});
    damage.Add({.X = -3, .Y = -3, .Width = 5U, .Height = 5U});
    damage.Add({.X = 2, .Y = 2, .Width = 2U, .Height = 2U});
    ASTERIA_EXPECT(test, "damage clips and merges adjacent rectangles", damage.Count() == 1U, damage.At(0U).X == 0,
                   damage.At(0U).Width == 4U, damage.At(0U).Height == 4U);
    damage.Clear();
    for (F::UInt32 index{0U}; index <= C::DamageList::Capacity; ++index)
    {
        damage.Add({.X = static_cast<F::Int32>(index * 3U), .Y = 20, .Width = 1U, .Height = 1U});
    }
    ASTERIA_EXPECT(test, "full damage list merges without losing the seventeenth rectangle", damage.Count() == 1U,
                   damage.At(0U).Width == 49U, damage.At(0U).Height == 1U);
}
// Independent one-pixel reference, including console margins and cursor opacity.
F::UInt32 LogicalPixel(F::Int32 x, F::Int32 y, G::Point pointer, G::PixelFormat format)
{
    F::UInt32 pixel = G::PackColor(format, Console::ConsoleBackground);
    if ((x >= 0) && (y >= 0) && (x < 128) && (y < 80))
    {
        const F::UInt32 code = g_console.CellAt(static_cast<F::UInt32>(y / 16), static_cast<F::UInt32>(x / 8)).Code;
        if ((code >= G::Bold8x16.FirstCode) && (code - G::Bold8x16.FirstCode < G::Bold8x16.Count) &&
            (G::Bold8x16.Data[(code - G::Bold8x16.FirstCode) * 16U + static_cast<F::UInt32>(y % 16)] &
             (0x80U >> static_cast<F::UInt32>(x % 8))) != 0U)
        {
            pixel = G::PackColor(format, Console::ConsoleForeground);
        }
    }
    const F::Int32 imageX = x - pointer.X + 1;
    const F::Int32 imageY = y - pointer.Y + 1;
    if ((imageX >= 0) && (imageX < 10) && (imageY >= 0) && (imageY < 18))
    {
        const F::UInt32 source = C::ArrowPixels.Pixels[imageY * 10 + imageX];
        if (source != 0U)
        {
            pixel = source == C::CursorWhite ? G::PackColor(format, Console::ConsoleForeground) : 0U;
        }
    }
    return pixel;
}
void Outputs(TestContext& test)
{
    Configure(test, G::PixelFormat::Bgrx);
    g_composer.Reset();
    g_damage.Configure({.X = -65, .Y = 0, .Width = 195U, .Height = 80U});
    const G::Surface left{
        .Pixels = g_actual, .Width = Width, .Height = 80U, .Stride = Stride, .Format = G::PixelFormat::Bgrx};
    const G::Surface right{
        .Pixels = g_expected, .Width = Width, .Height = 80U, .Stride = Stride, .Format = G::PixelFormat::Rgbx};
    ASTERIA_EXPECT(test, "signed desktop origins and separate output scales/formats",
                   g_composer.AddOutput({.Scanout = left, .Origin = {-65, 0}, .Scale = 2U}),
                   g_composer.AddOutput({.Scanout = right, .Origin = {0, 0}, .Scale = 1U}),
                   g_composer.OutputCount() == 2U, g_composer.OutputAt(0U).Origin.X == -65,
                   g_composer.AddPlane(g_background), g_composer.AddPlane(g_consolePlane),
                   g_composer.AddPlane(g_cursor), !g_composer.AddPlane(g_cursor));
    g_cursor.Move({.X = 0, .Y = 3});
    const F::UInt8 text[]{'A'};
    g_console.Write(text, sizeof(text));
    g_damage.Add({.X = -65, .Y = 0, .Width = 195U, .Height = 80U});
    g_composer.Render(g_damage, Order);
    for (F::UInt32 y{0U}; y < 80U; ++y)
    {
        for (F::UInt32 x{0U}; x < Width; ++x)
        {
            ASTERIA_EXPECT(test, "scaled cursor straddles outputs without seams or format leakage",
                           g_actual[y * Stride + x] == LogicalPixel(static_cast<F::Int32>(x / 2U) - 65,
                                                                    static_cast<F::Int32>(y / 2U), g_cursor.Position(),
                                                                    left.Format),
                           g_expected[y * Stride + x] == LogicalPixel(static_cast<F::Int32>(x),
                                                                      static_cast<F::Int32>(y), g_cursor.Position(),
                                                                      right.Format));
        }
        for (F::UInt32 x{Width}; x < Stride; ++x)
        {
            ASTERIA_EXPECT(test, "multiple outputs leave pitch untouched", g_actual[y * Stride + x] == Sentinel,
                           g_expected[y * Stride + x] == Sentinel);
        }
    }
    ASTERIA_EXPECT(test, "one ordering call covers both outputs", g_orders == 1U);
    ASTERIA_EXPECT(test, "nonintegral dimensions and unrepresentable desktop extents are refused",
                   !g_composer.AddOutput({.Scanout = left, .Origin = {0, 0}, .Scale = 3U}),
                   !g_composer.AddOutput({.Scanout = left, .Origin = {0x7FFFFFFF, 0}, .Scale = 1U}));
}
void ScaledGlyphRuns(TestContext& test)
{
    for (const F::UInt32 scale : {3U, 5U})
    {
        Configure(test, G::PixelFormat::Bgrx);
        g_composer.Reset();
        const F::UInt32 width = scale == 3U ? 129U : 130U;
        const F::UInt32 height = scale == 3U ? 81U : 80U;
        const G::Point origin{-7, -3};
        const G::Surface surface{
            .Pixels = g_actual, .Width = width, .Height = height, .Stride = Stride, .Format = G::PixelFormat::Bgrx};
        ASTERIA_EXPECT(test, "non-power-of-two scale uses bounded glyph runs across tile seams",
                       g_composer.AddOutput({.Scanout = surface, .Origin = origin, .Scale = scale}),
                       g_composer.AddPlane(g_background), g_composer.AddPlane(g_consolePlane),
                       g_composer.AddPlane(g_cursor));
        const F::UInt8 text[]{'A', 0U, 255U, 'B', '\n', 'C'};
        g_console.Write(text, sizeof(text));
        g_cursor.Move({3, 4});
        g_damage.Configure({.X = origin.X, .Y = origin.Y, .Width = width / scale, .Height = height / scale});
        g_damage.Add({.X = origin.X, .Y = origin.Y, .Width = width / scale, .Height = height / scale});
        g_composer.Render(g_damage, Order);
        for (F::UInt32 y{0U}; y < height; ++y)
        {
            for (F::UInt32 x{0U}; x < width; ++x)
            {
                ASTERIA_EXPECT(test, "scaled margin, missing glyphs and cursor remain pixel exact",
                               g_actual[y * Stride + x] == LogicalPixel(static_cast<F::Int32>(x / scale) + origin.X,
                                                                        static_cast<F::Int32>(y / scale) + origin.Y,
                                                                        g_cursor.Position(), surface.Format));
            }
        }
    }
}

void CounterScaling(TestContext& test)
{
    Asteria::TestSupport::Random random{2080U};
    for (F::Size index{0U}; index < 1000U; ++index)
    {
        const F::UInt64 numerator = random.Next();
        const F::UInt64 denominator = random.Next() | 1U;
        ASTERIA_EXPECT(test, "64-bit freestanding counter scaling agrees with host arithmetic including carry",
                       C::DivideCounter(numerator, denominator) == numerator / denominator);
    }
    ASTERIA_EXPECT(test, "missing scale and full-width boundaries are defined",
                   C::DivideCounter(~F::UInt64{0U}, 0U) == 0U, C::DivideCounter(~F::UInt64{0U}, 1U) == ~F::UInt64{0U},
                   C::DivideCounter(~F::UInt64{0U}, ~F::UInt64{0U}) == 1U);
}

void AlphaAndVisibility(TestContext& test)
{
    static constexpr F::UInt32 pixel{0x80FF0000U};
    for (G::PixelFormat format : {G::PixelFormat::Bgrx, G::PixelFormat::Rgbx})
    {
        Configure(test, format);
        ASTERIA_EXPECT(test, "bounded custom alpha image accepted",
                       g_cursor.SetImage({.Pixels = &pixel, .Width = 1U, .Height = 1U, .HotSpot = {0, 0}}),
                       !g_cursor.SetImage({.Pixels = nullptr, .Width = 1U, .Height = 1U, .HotSpot = {0, 0}}),
                       !g_cursor.SetImage({.Pixels = &pixel, .Width = 65U, .Height = 1U, .HotSpot = {0, 0}}),
                       !g_cursor.SetImage({.Pixels = &pixel, .Width = 1U, .Height = 1U, .HotSpot = {1, 0}}));
        g_cursor.Move({20, 20});
        g_composer.Render(g_damage, Order);
        const G::Color expected{
            .Red = static_cast<F::UInt8>((255U * 128U + Console::ConsoleBackground.Red * 127U + 127U) / 255U),
            .Green = static_cast<F::UInt8>((Console::ConsoleBackground.Green * 127U + 127U) / 255U),
            .Blue = static_cast<F::UInt8>((Console::ConsoleBackground.Blue * 127U + 127U) / 255U)};
        ASTERIA_EXPECT(test, "straight alpha blends in scanout format with channel rounding",
                       g_actual[20U * Stride + 20U] == G::PackColor(format, expected));
        g_damage.Add(g_cursor.Bounds());
        g_cursor.Show(false);
        g_composer.Render(g_damage, Order);
        ASTERIA_EXPECT(test, "hiding restores underlying console plane", G::IsEmpty(g_cursor.Bounds()),
                       g_actual[20U * Stride + 20U] == G::PackColor(format, Console::ConsoleBackground));
        g_cursor.Show(true);
        g_composer.Reset();
        ASTERIA_EXPECT(test, "ordered planes may place opaque fill above the cursor",
                       g_composer.AddOutput({.Scanout = Surface(g_actual, format), .Origin = {0, 0}, .Scale = 1U}),
                       g_composer.AddPlane(g_cursor), g_composer.AddPlane(g_background));
        g_damage.Add(g_cursor.Bounds());
        g_composer.Render(g_damage, Order);
        ASTERIA_EXPECT(test, "plane insertion order determines final pixel",
                       g_actual[20U * Stride + 20U] == G::PackColor(format, Console::ConsoleBackground));
    }
    ASTERIA_EXPECT(test, "restore arrow image", g_cursor.SetImage(C::Arrow));
    g_cursor.Move({-0x7FFFFFFF - 1, -0x7FFFFFFF - 1});
    ASTERIA_EXPECT(test, "extreme cursor coordinates clip representable bounds without signed overflow",
                   g_cursor.Bounds().X == -0x7FFFFFFF - 1, g_cursor.Bounds().Width == 9U,
                   g_cursor.Bounds().Y == -0x7FFFFFFF - 1, g_cursor.Bounds().Height == 17U);
    g_damage.Configure({.X = 0x7FFFFFFF, .Y = 0, .Width = 1U, .Height = 1U});
    g_damage.Add({.X = 0x7FFFFFFF, .Y = 0, .Width = 1U, .Height = 1U});
    ASTERIA_EXPECT(test, "invalid desktop bounds disable damage", g_damage.Count() == 0U);
}
void Model(TestContext& test, F::UInt64 seed)
{
    Configure(test, G::PixelFormat::Bgrx);
    Asteria::TestSupport::Random random{seed};
    Protocol::DisplayPointer pointer{};
    pointer.Configure(Width, Height, 7U);
    F::Int64 modelX{Width / 2U};
    F::Int64 modelY{Height / 2U};
    for (F::UInt64 step{1U}; step <= 400U; ++step)
    {
        const F::UInt64 operation = random.Below(5U);
        if (operation < 3U)
        {
            const F::Int32 dx = static_cast<F::Int32>(random.Below(401U)) - 200;
            const F::Int32 dy = static_cast<F::Int32>(random.Below(301U)) - 150;
            const F::UInt64 words[8U]{7U,
                                      step,
                                      Protocol::DisplayRelativeMotion,
                                      static_cast<F::UInt32>(dx) |
                                          (static_cast<F::UInt64>(static_cast<F::UInt32>(dy)) << 32U),
                                      0U,
                                      1U,
                                      0U,
                                      0U};
            ASTERIA_EXPECT(test, "fresh pointer sequence applied", pointer.Apply(words));
            modelX += dx;
            modelY += dy;
            if (modelX < 0)
            {
                modelX = 0;
            }
            if (modelX >= Width)
            {
                modelX = Width - 1U;
            }
            if (modelY < 0)
            {
                modelY = 0;
            }
            if (modelY >= Height)
            {
                modelY = Height - 1U;
            }
            Move(pointer.Position());
            ASTERIA_EXPECT(test, "replayed sequence refused atomically", !pointer.Apply(words),
                           pointer.Sequence() == step);
        }
        else
        {
            const F::UInt8 bytes[4U]{static_cast<F::UInt8>('A' + random.Below(26U)), 'x',
                                     static_cast<F::UInt8>(random.Below(2U) == 0U ? '\n' : 'y'), '\n'};
            g_console.Write(bytes, sizeof(bytes));
            DrawConsoleDamage();
        }
        g_composer.Render(g_damage, Order);
        Reference(G::PixelFormat::Bgrx, {.X = static_cast<F::Int32>(modelX), .Y = static_cast<F::Int32>(modelY)});
        if (!Equal() || (pointer.Position().X != modelX) || (pointer.Position().Y != modelY))
        {
            static_cast<void>(std::fprintf(stderr, "seed=%llu step=%llu operation=%llu\n",
                                           static_cast<unsigned long long>(seed), static_cast<unsigned long long>(step),
                                           static_cast<unsigned long long>(operation)));
        }
        ASTERIA_EXPECT(test, "incremental composition matches full reference after pointer and text streams", Equal(),
                       pointer.Position().X == modelX, pointer.Position().Y == modelY);
    }
}
void SessionOutput(TestContext& test)
{
    for (const auto format : {G::PixelFormat::Bgrx, G::PixelFormat::Rgbx})
    {
        Configure(test, format);
        g_composer.Reset();
        g_consolePlane.Show(false);
        g_sessionPlane.Configure({.X = 0, .Y = 0, .Width = Width, .Height = Height});
        g_sessionPlane.SetBuffer(
            {.Pixels = g_sessionPixels, .Width = Width, .Height = Height, .Stride = Stride, .Format = format});
        g_sessionPlane.Show(true);
        for (F::UInt32 y{0U}; y < Height; ++y)
        {
            for (F::UInt32 x{0U}; x < Width; ++x)
            {
                g_sessionPixels[y * Stride + x] = G::PackColor(format, {.Red = static_cast<F::UInt8>(x),
                                                                        .Green = static_cast<F::UInt8>(y),
                                                                        .Blue = static_cast<F::UInt8>(x ^ y)});
            }
        }
        ASTERIA_EXPECT(
            test, "session between background and trusted planes",
            g_composer.AddOutput({.Scanout = Surface(g_actual, format), .Origin = {.X = 0, .Y = 0}, .Scale = 1U}),
            g_composer.AddPlane(g_background), g_composer.AddPlane(g_sessionPlane), g_composer.AddPlane(g_consolePlane),
            g_composer.AddPlane(g_cursor));
        const auto stats = g_composer.Render(g_damage, Order);
        Reference(format, {.X = 65, .Y = 40}, false, true);
        ASTERIA_EXPECT(test, "opaque direct tiles and cursor overlap match the independent pixel model", Equal(),
                       stats.DirectPixels > 0U, stats.DirectPixels < stats.Pixels, g_orders == 1U);
        static constexpr F::UInt8 text[]{'h', 'i', 'd', 'd', 'e', 'n', '\n'};
        g_console.Write(text, sizeof(text));
        g_consolePlane.Show(true);
        g_damage.Add(G::Bounds(Surface(g_actual, format)));
        const auto consoleStats = g_composer.Render(g_damage, Order);
        Reference(format, {.X = 65, .Y = 40});
        ASTERIA_EXPECT(test, "trusted console restores hidden text exactly", Equal(), consoleStats.DirectPixels == 0U);
        g_consolePlane.Show(false);
        g_sessionPlane.SetBuffer({});
        g_cursor.Show(false);
        g_damage.Add(G::Bounds(Surface(g_actual, format)));
        g_composer.Render(g_damage, Order);
        G::FillRect(Surface(g_expected, format), G::Bounds(Surface(g_expected, format)),
                    Asteria::User::Session::Desktop);
        ASTERIA_EXPECT(test, "detached shown session paints Desktop", Equal());
    }
    const auto extreme = C::ProjectBounds({.X = -0x7FFFFFFF - 1, .Y = -0x7FFFFFFF - 1, .Width = 1U, .Height = 1U},
                                          {.Scanout = Surface(g_actual, G::PixelFormat::Bgrx),
                                           .Origin = {.X = 0x7FFFFFFF, .Y = 0x7FFFFFFF},
                                           .Scale = 32768U});
    ASTERIA_EXPECT(test, "projection clips extreme coordinates without signed overflow", G::IsEmpty(extreme));
    g_consolePlane.Show(true);
}
} // namespace
int main()
{
    TestContext test{"CompositionTests"};
    Damage(test);
    Pixels(test);
    Outputs(test);
    AlphaAndVisibility(test);
    ScaledGlyphRuns(test);
    CounterScaling(test);
    SessionOutput(test);
    const auto seeds = Asteria::TestSupport::GetSeedRange(280U, 8U);
    for (F::UInt64 seed{seeds.First}; seed < seeds.First + seeds.Count; ++seed)
    {
        Model(test, seed);
    }
    return test.Finish();
}
