// SPDX-License-Identifier: Apache-2.0

#include "User/Toolkit/Theme.hpp"
#include "Foundation/Compiler.hpp"

namespace Asteria::User::Toolkit
{
namespace
{
[[gnu::section(".data.rel.ro.ClassicTheme")]] constexpr Theme Defaults{};
ASTERIA_NOINLINE_NOIPA const Theme& ClassicDefaults() noexcept
{
    return Defaults;
}
} // Namespace
Theme Theme::Classic(FontContext* font) noexcept
{
    // A const template keeps the compiler's aggregate initializer in RX data in a component image
    // (writable segments are zero-filled).
    Theme result{ClassicDefaults()};
    result.Font = font;
    return result;
}
bool Theme::Valid() const noexcept
{
    return FontSize > 0U && FontSize <= 128U && LineHeight >= FontSize && LineHeight <= 256U && Border > 0U &&
           Border <= 8U && Padding <= 64U && Gap <= 64U && Indicator > 4U && Indicator <= 64U && Arrow > 0U &&
           Arrow <= 64U && ItemHeight >= LineHeight && ItemHeight <= 256U && Separator > 0U && Separator <= 64U &&
           CaretWidth > 0U && CaretWidth <= 8U && FocusInset <= 64U && PressOffset <= 8U && TabWidth > 0U &&
           TabWidth <= MaximumAxis && Splitter > 0U && Splitter <= 64U && DefaultWidth <= MaximumAxis &&
           DefaultHeight <= MaximumAxis && MinimumThumb > 0U && MinimumThumb <= 64U && ScrollStep <= MaximumAxis &&
           Fallback.Data != nullptr && Fallback.GlyphWidth > 0U && Fallback.GlyphWidth <= 8U &&
           Fallback.GlyphHeight > 0U && Fallback.GlyphHeight <= 32U && CaretPeriod > 0U && ToolTipDelay > 0U &&
           User::Text::IsScalar(PasswordScalar) && PopupRows > 0U && PopupRows <= MaximumItems &&
           DoubleClickPeriod > 0U && DoubleClickPeriod <= 10'000U && DoubleClickDistance <= 64U;
}
} // Namespace Asteria::User::Toolkit
