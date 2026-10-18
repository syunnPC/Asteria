// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_TOOLKIT_THEME_HPP_INCLUDED
#define ASTERIA_USER_TOOLKIT_THEME_HPP_INCLUDED

#include "User/Toolkit/Types.hpp"
#include "User/Graphics/BitmapFont.hpp"
#include "User/Font/Cache.hpp"
#include "User/Font/Layout.hpp"
#include "User/Session/Palette.hpp"

namespace Asteria::User::Toolkit
{
struct FontContext
{
    const Font::Font* Face{};
    Font::OutlineWorkspace OutlineScratch{};
    Font::RasterWorkspace RasterScratch{};
    Font::Command Commands[Font::MaximumCommands]{};
    Font::PositionedGlyph Glyphs[MaximumText]{};
    Font::GlyphCache Cache{};
};
struct Theme
{
    // Every colour/metric/font is data.
    Color Face{Session::Face};
    Color Highlight{Session::Highlight};
    Color Light{Session::Light};
    Color Shadow{Session::Shadow};
    Color DarkShadow{Session::DarkShadow};
    Color Text{Session::Glyph};
    Color Window{Session::Highlight};
    Color Selection{Session::ActiveTitle};
    Color SelectedText{Session::ActiveTitleText};
    Color DisabledText{Session::Shadow};
    Color Hot{Session::Light};
    Color Link{Session::ActiveTitle};
    FontContext* Font{};
    User::Graphics::BitmapFont Fallback{User::Graphics::Bold8x16};
    UInt32 FontSize{16U};
    UInt32 LineHeight{20U};
    UInt32 Border{2U};
    UInt32 Padding{4U};
    UInt32 Gap{4U};
    UInt32 Indicator{13U};
    UInt32 Arrow{16U};
    UInt32 ItemHeight{24U};
    UInt32 Separator{6U};
    UInt32 CaretWidth{1U};
    UInt32 FocusInset{4U};
    UInt32 PressOffset{1U};
    UInt32 TabWidth{80U};
    UInt32 Splitter{5U};
    UInt32 DefaultWidth{100U};
    UInt32 DefaultHeight{26U};
    UInt32 MinimumThumb{8U};
    UInt32 ScrollStep{20U};
    UInt32 PasswordScalar{0x2022U};
    UInt32 PopupRows{6U};
    UInt64 CaretPeriod{500U};
    UInt64 ToolTipDelay{700U};
    UInt64 DoubleClickPeriod{500U};
    UInt32 DoubleClickDistance{4U};
    [[nodiscard]] static Theme Classic(FontContext* font = nullptr) noexcept;
    [[nodiscard]] bool Valid() const noexcept;
};
} // Namespace Asteria::User::Toolkit
#endif
