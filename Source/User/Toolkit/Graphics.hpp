// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_GRAPHICS_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_GRAPHICS_HPP_INCLUDED

#include "User/Toolkit/Theme.hpp"

namespace Asteria::User::Toolkit
{
enum class ImageFormat : UInt8
{
    Rgba,
    Bgra
};
struct Image
{
    const UInt8* Pixels{};
    Size Bytes{};
    UInt32 Width{};
    UInt32 Height{};
    UInt32 Stride{}; // stride in bytes
    ImageFormat Format{};
    [[nodiscard]] bool Valid() const noexcept;
};
class Graphics final
{
public:
    Graphics(User::Graphics::MemorySurface surface, Rect clip, const Theme& theme) noexcept;
    [[nodiscard]] Rect Clip() const noexcept
    {
        return m_clip;
    }
    void SetClip(Rect clip) noexcept
    {
        m_clip = User::Graphics::Intersect(m_limit, clip);
    }
    [[nodiscard]] const Theme& Palette() const noexcept
    {
        return m_theme;
    }
    void FillRect(Rect rect, Color color) noexcept;
    void Line(Point a, Point b, Color color) noexcept;
    void Bevel(Rect rect, bool sunken) noexcept;
    void FocusRect(Rect rect) noexcept;
    [[nodiscard]] UInt32 Measure(TextView text) noexcept;
    void Text(TextView text, Rect rect, Color color, Align align = Align::Start, bool ellipsis = true) noexcept;
    void DrawImage(const Image& image, Rect destination, bool scale) noexcept;

private:
    [[nodiscard]] bool Shape(TextView text, Font::Run& run) noexcept;
    void Scalar(UInt32 point, Point position, Color color) noexcept;
    void RunText(TextView text, Point position, Color color) noexcept;
    User::Graphics::MemorySurface m_surface{};
    Rect m_clip{};
    Rect m_limit{};
    const Theme& m_theme;
};
} // namespace Asteria::User::Toolkit
#endif
