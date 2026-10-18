// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_USER_FONT_LAYOUT_INTERNAL_HPP_INCLUDED
#define ASTERIA_USER_FONT_LAYOUT_INTERNAL_HPP_INCLUDED
#include "User/Font/Internal.hpp"
#include "User/Font/Layout.hpp"
namespace Asteria::User::Font::Detail
{
[[nodiscard]] Int32 Coverage(Bytes data, UInt16 glyph) noexcept;
[[nodiscard]] UInt16 Class(Bytes data, UInt16 glyph) noexcept;
[[nodiscard]] Bytes LookupSubtable(Bytes lookup, UInt32 index, bool positioning, UInt16& type) noexcept;
[[nodiscard]] bool Selected(Bytes table, UInt32 lookup, UInt32 feature, Budget& budget) noexcept;
[[nodiscard]] Error Pair(const Font& font, UInt16 left, UInt16 right, PairAdjustment& result, Budget& budget) noexcept;
[[nodiscard]] Error Ligatures(const Font& font, Run& run, Budget& budget) noexcept;
}
#endif
