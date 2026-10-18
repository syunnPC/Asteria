// SPDX-License-Identifier: Apache-2.0

#include "User/Session/UiTextDrawing.hpp"
#include "User/Graphics/BitmapFont.hpp"
namespace Asteria::User::Session
{
void DrawUiMask(const Graphics::MemorySurface& surface, const Font::Mask& mask, const Graphics::Point origin,
                const Graphics::Rect, const Graphics::Color foreground) noexcept
{
    static_cast<void>(Font::DrawMask(surface, mask, origin, foreground));
}
void DrawUiBitmap(const Graphics::MemorySurface& surface, const Graphics::Point origin, const Graphics::Rect,
                  const Foundation::UInt32 code, const Graphics::Color foreground,
                  const Graphics::Color background) noexcept
{
    Graphics::DrawGlyph(surface, origin.X, origin.Y, Graphics::Bold8x16, code, foreground, background);
}
}
