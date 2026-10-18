// SPDX-License-Identifier: Apache-2.0
#include "User/Session/Scene.hpp"
#include "User/Graphics/Composition/SessionPlane.hpp"
#include "User/Graphics/Blit.hpp"
#include "Support/TestContext.hpp"
#include <vector>
#include <initializer_list>

namespace
{
namespace F = Asteria::Foundation;
namespace G = Asteria::User::Graphics;
namespace P = Asteria::User::Session;
namespace C = G::Composition;
constexpr F::UInt32 Width{256U};
constexpr F::UInt32 Height{112U};
constexpr F::UInt32 Stride{263U};
constexpr F::UInt32 Guard{0xCAFE1234U};
P::Scene scene;
C::Composer composer;
C::SessionPlane session;
C::BackgroundPlane background{{0x10U, 0x20U, 0x40U}};
C::CursorPlane cursor;
C::DamageList damage;
bool Guards(const std::vector<F::UInt32>& pixels, F::UInt32 width, F::UInt32 height, F::UInt32 stride)
{
    if (pixels.front() != Guard || pixels.back() != Guard)
    {
        return false;
    }
    for (F::UInt32 y{0U}; y < height; ++y)
    {
        for (F::UInt32 x{width}; x < stride; ++x)
        {
            if (pixels[1U + y * stride + x] != Guard)
            {
                return false;
            }
        }
    }
    return true;
}
void Run(Asteria::TestSupport::TestContext& test)
{
    // Guard both ends and every row's padding. ASan also checks the exactly
    // sized image reads when frames cross all four session edges.
    std::vector<F::UInt32> pixels(Stride * Height + 2U, Guard), reference(Stride * Height + 2U, Guard);
    std::vector<F::UInt32> image(248U * 83U, 0x00345678U);
    const G::MemorySurface output{pixels.data() + 1U, Width, Height, Stride, G::PixelFormat::Bgrx};
    const G::MemorySurface expected{reference.data() + 1U, Width, Height, Stride, G::PixelFormat::Bgrx};
    scene.Configure(Width, Height, G::PixelFormat::Bgrx);
    ASTERIA_EXPECT(test, "clipped scene starts with a small complete image", scene.Show(0U, {200, 35, 72U, 53U}));
    const G::Rect full{0, 0, 248U, 83U};
    for (F::Size round{0U}; round < 24U; ++round)
    {
        for (const G::Rect frame : {G::Rect{200, 35, 72U, 53U}, G::Rect{0, 0, Width, Height},
                                    G::Rect{-40, -15, 72U, 53U}, G::Rect{224, 100, 72U, 53U}})
        {
            scene.SetFrame(0U, frame);
            scene.Present(0U, {image.data(), 248U, 83U, 248U, G::PixelFormat::Bgrx}, &full, 1U);
            static_cast<void>(scene.Compose(output));
            scene.RenderAll(expected);
            ASTERIA_EXPECT(test,
                           "incremental clipped/maximized composition matches full render without escaping output",
                           pixels == reference, Guards(pixels, Width, Height, Stride));
            for (F::Size i{0U}; i < scene.Composed().Count(); ++i)
            {
                ASTERIA_EXPECT(test, "broker damage stays inside the session",
                               P::ContainsRect(scene.Output(), scene.Composed().At(i)));
            }
        }
    }
    constexpr F::UInt32 ScanWidth{1024U};
    constexpr F::UInt32 ScanHeight{768U};
    constexpr F::UInt32 ScanStride{1031U};
    std::vector<F::UInt32> scanout(ScanStride * ScanHeight + 2U, Guard);
    session.Configure({0, 0, Width, Height});
    session.SetBuffer({output.Pixels, Width, Height, Stride, output.Format});
    session.Show(true);
    cursor.Move({248, 104});
    cursor.Show(true);
    ASTERIA_EXPECT(
        test, "broker registers clipped session and cursor over a larger scanout", cursor.SetImage(C::Arrow),
        composer.AddOutput({.Scanout = {scanout.data() + 1U, ScanWidth, ScanHeight, ScanStride, output.Format},
                            .Origin = {},
                            .Scale = 1U}),
        composer.AddPlane(background), composer.AddPlane(session), composer.AddPlane(cursor));
    damage.Configure({0, 0, ScanWidth, ScanHeight});
    damage.Add({-30, -20, 1080U, 820U});
    const auto rendered = composer.Render(damage, nullptr);
    ASTERIA_EXPECT(test, "direct copy and cursor edge tiles preserve scanout guards", rendered.DirectPixels > 0U,
                   Guards(scanout, ScanWidth, ScanHeight, ScanStride));
    bool outside{true};
    for (F::UInt32 y{Height + 16U}; y < ScanHeight; ++y)
    {
        for (F::UInt32 x{Width + 16U}; x < ScanWidth; ++x)
        {
            outside = outside && scanout[1U + y * ScanStride + x] == 0x00102040U;
        }
    }
    ASTERIA_EXPECT(test, "session reads never extend into the rest of the GOP", outside);
}
} // namespace
int main()
{
    Asteria::TestSupport::TestContext test{"SessionFreezeTests"};
    Run(test);
    return test.Finish();
}
