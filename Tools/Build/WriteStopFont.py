# SPDX-License-Identifier: Apache-2.0
"""Slice printable ASCII from the existing public-domain NetBSD bitmap only."""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[2]
source = (root / 'Source/User/Graphics/Bold8x16Font.hpp').read_text()
first = int(re.search(r'Bold8x16FirstCode\{(0x[0-9A-Fa-f]+|\d+)U\}', source)[1], 0)
body = source.split('Bold8x16Glyphs', 1)[1]
body = body[body.index('{') + 1:body.index('};')]
body = re.sub(r'//[^\n]*|/\*.*?\*/', '', body, flags=re.S)
values = re.findall(r'0x[0-9A-Fa-f]+', body)[(32 - first) * 16:(127 - first) * 16]
if len(values) != 95 * 16:
    raise ValueError('source font does not contain printable ASCII')
text = '''// SPDX-License-Identifier: Apache-2.0 AND LicenseRef-Public-Domain
#ifndef ASTERIA_KERNEL_DIAGNOSTICS_STOP_FONT_HPP_INCLUDED
#define ASTERIA_KERNEL_DIAGNOSTICS_STOP_FONT_HPP_INCLUDED

#include "Foundation/Types.hpp"

namespace Asteria::Kernel::Diagnostics
{
// Public-domain NetBSD bold8x16 bitmap, revision 1.13. Printable ASCII slice
// (0x20..0x7E) of User/Graphics/Bold8x16Font.hpp; no font-file code copied.
inline constexpr Foundation::UInt8 StopGlyphs[1520U]{
'''
text += ''.join('    ' + ', '.join(values[index:index + 16]) + ',\n' for index in range(0, len(values), 16))
text += '''};
} // namespace Asteria::Kernel::Diagnostics

#endif // ASTERIA_KERNEL_DIAGNOSTICS_STOP_FONT_HPP_INCLUDED
'''
output = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'Source/Kernel/Diagnostics/StopFont.hpp'
if not output.exists() or output.read_text() != text:
    output.write_text(text)
