#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write a C++ glyph table from a wsfont-style bitmap font header.

Reads the `static u_char <name>_data[] = {... };` array of a NetBSD wsfont
header (one byte a row of an 8-pixel-wide glyph, most significant bit
leftmost) and writes `Source/User/Graphics/Bold8x16Font.hpp`'s table. Only
the glyph bytes are taken: the bold8x16 font is in the public domain ("This
font lives in the public domain"), while the header's code is not copied.

Usage: WriteBitmapFont.py <bold8x16.h> <output.hpp>
"""
import re
import sys

FIRST, COUNT, WIDTH, HEIGHT = 0x01, 254, 8, 16


def glyph_bytes(text: str) -> bytes:
    start = text.index('_data[] = {')
    body = text[start + len('_data[] = {'):]
    body = body[:body.index('};')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    data = bytes(int(value, 16) for value in re.findall(r'0x[0-9a-fA-F]+', body))
    if len(data) != COUNT * HEIGHT:
        raise SystemExit('%d glyph bytes, expected %d' % (len(data), COUNT * HEIGHT))
    return data


def main() -> int:
    source, output = sys.argv[1], sys.argv[2]
    data = glyph_bytes(open(source).read())
    lines = []
    for glyph in range(COUNT):
        row = data[glyph * HEIGHT:(glyph + 1) * HEIGHT]
        lines.append('    // 0x%02X' % (FIRST + glyph))
        lines.append('    ' + ', '.join('0x%02XU' % value for value in row[:8]) + ',')
        lines.append('    ' + ', '.join('0x%02XU' % value for value in row[8:]) + ',')
    with open(output, 'w') as handle:
        handle.write('''// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Public-Domain
#ifndef ASTERIA_USER_GRAPHICS_BOLD8X16_FONT_HPP_INCLUDED
#define ASTERIA_USER_GRAPHICS_BOLD8X16_FONT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::User::Graphics
{
/// The glyphs of the "Boldface" 8x16 PC font, IBM encoding (code page 437)
/// 0x01-0xFE, sixteen rows of one byte each, the most significant bit
/// leftmost. The bitmap is NetBSD's sys/dev/wsfont/bold8x16.h (rev. 1.13)
/// glyph data, which that file states is in the public domain ("This font
/// lives in the public domain"); only the bitmap is copied, by
/// Tools/Build/WriteBitmapFont.py, which wrote this file.
inline constexpr Foundation::UInt32 Bold8x16FirstCode{0x%02XU};
inline constexpr Foundation::UInt32 Bold8x16Count{%dU};
inline constexpr Foundation::UInt32 Bold8x16Width{%dU};
inline constexpr Foundation::UInt32 Bold8x16Height{%dU};
inline constexpr Foundation::UInt8 Bold8x16Glyphs[%dU]{
%s
};
} // namespace Asteria::User::Graphics

#endif // ASTERIA_USER_GRAPHICS_BOLD8X16_FONT_HPP_INCLUDED
''' % (FIRST, COUNT, WIDTH, HEIGHT, COUNT * HEIGHT, '\n'.join(lines)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
