// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_SESSION_UI_TEXT_DRAWING_HPP_INCLUDED
#define ASTERIA_USER_SESSION_UI_TEXT_DRAWING_HPP_INCLUDED
#include "User/Font/Draw.hpp"
namespace Asteria::User::Session
{
// Ordinary drawing interface. The product link paints pixels; the host mask tool substitutes its
// own implementation. origin is relative to the clipped surface, clip is in the parent surface.
void DrawUiMask(const Graphics::MemorySurface& surface, const Font::Mask& mask, Graphics::Point origin,
                Graphics::Rect clip, Graphics::Color foreground) noexcept;
void DrawUiBitmap(const Graphics::MemorySurface& surface, Graphics::Point origin, Graphics::Rect clip,
                  Foundation::UInt32 code, Graphics::Color foreground, Graphics::Color background) noexcept;
}
#endif
