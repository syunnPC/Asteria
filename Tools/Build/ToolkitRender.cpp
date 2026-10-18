// SPDX-License-Identifier: Apache-2.0
// VM content oracle: toolkit render model; scene/input remain independent.
#include "User/Toolkit/Demo.hpp"
#include "User/Toolkit/SessionComponent.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace
{
namespace Ui = Asteria::User::Toolkit;
namespace Abi = Asteria::Abi;
namespace Foundation = Asteria::Foundation;
namespace User = Asteria::User;
class Host final : public Ui::IThreadHost, public Ui::IEventSource, public Ui::IPresenter
{
public:
    [[nodiscard]] bool IsUiThread() noexcept override
    {
        return true;
    }
    void LockQueue() noexcept override
    {
    }
    void UnlockQueue() noexcept override
    {
    }
    void Wait(Ui::Completion&) noexcept override
    {
    }
    void Finish(Ui::Completion& c) noexcept override
    {
        c.Done = 1U;
    }
    void WakeUi() noexcept override
    {
    }
    [[nodiscard]] bool Poll(Ui::SourceEvent&) noexcept override
    {
        return false;
    }
    void WaitUntil(Foundation::UInt64) noexcept override
    {
    }
    [[nodiscard]] Foundation::UInt64 Now() noexcept override
    {
        return 0U;
    }
    [[nodiscard]] bool Present(Foundation::UInt32, const Ui::Rect*, Foundation::UInt32) noexcept override
    {
        return true;
    }
};
Foundation::UInt32 g_pixels[2U][Ui::MaximumAxis * Ui::MaximumAxis]{};
Ui::SessionFontWorkspace g_fonts{};
Foundation::UInt8 g_face[Abi::ComponentUiFontFaceBytes]{};
} // Namespace
int main(const int argc, const char* const* const argv)
{
    if (argc != 5)
    {
        return 2;
    }
    const auto w = std::strtoul(argv[1U], nullptr, 10);
    const auto h = std::strtoul(argv[2U], nullptr, 10);
    if (w == 0U || h == 0U || w > Ui::MaximumAxis || h > Ui::MaximumAxis)
    {
        return 2;
    }
    if (std::strcmp(argv[3U], "-") != 0)
    {
        auto* const file = std::fopen(argv[3U], "rb");
        if (file == nullptr)
        {
            return 3;
        }
        const auto bytes = std::fread(g_face, 1U, sizeof(g_face), file);
        const bool valid = std::fgetc(file) == EOF && !std::ferror(file);
        std::fclose(file);
        if (!valid || g_fonts.Face.Open({g_face, bytes}) != User::Font::Error::None)
        {
            return 3;
        }
        g_fonts.Context.Face = &g_fonts.Face;
    }
    if (g_fonts.Context.Cache.Configure(g_fonts.Cache, sizeof(g_fonts.Cache), g_fonts.Entries, 64U) !=
        User::Font::Error::None)
    {
        return 3;
    }
    Host host{};
    Ui::Application application{host, host, Ui::Theme::Classic(&g_fonts.Context)};
    Ui::DemoForm demo{application};
    const auto width = static_cast<Foundation::UInt32>(w);
    const auto height = static_cast<Foundation::UInt32>(h);
    const User::Graphics::MemorySurface first{g_pixels[0U], width, height, width, User::Graphics::PixelFormat::Rgbx};
    const User::Graphics::MemorySurface second{g_pixels[1U], width, height, width, User::Graphics::PixelFormat::Rgbx};
    if (demo.Window().Bind(first, second, host) != Ui::Status::Ok ||
        demo.Build(Ui::DemoColumns, Ui::DemoCellWidth, Ui::DemoRowHeight) != Ui::Status::Ok)
    {
        return 4;
    }
    if (std::strcmp(argv[4U], "interacted") == 0)
    {
        static_cast<void>(demo.ActionButton().SetText(Ui::Utf8("Clicked")));
        static_cast<void>(demo.Entry().SetText(Ui::Utf8("asteria")));
        static_cast<void>(demo.Check().SetChecked(true));
        static_cast<void>(demo.Choices().SetSelectedIndex(1));
        static_cast<void>(demo.Choices().Focus());
    }
    else if (std::strcmp(argv[4U], "initial") != 0)
    {
        return 2;
    }
    if (demo.Window().Render() != Ui::Status::Ok)
    {
        return 4;
    }
    std::printf("P6\n%u %u\n255\n", width, height);
    for (Foundation::UInt32 y{}; y < height; ++y)
    {
        for (Foundation::UInt32 x{}; x < width; ++x)
        {
            const auto pixel = g_pixels[0U][static_cast<Foundation::Size>(y) * width + x];
            std::fputc(static_cast<int>(pixel & 255U), stdout);
            std::fputc(static_cast<int>((pixel >> 8U) & 255U), stdout);
            std::fputc(static_cast<int>((pixel >> 16U) & 255U), stdout);
        }
    }
    return std::ferror(stdout) ? 5 : 0;
}
