# SPDX-License-Identifier: Apache-2.0
"""The runner's model of the System Display Broker's console (AsteriaVm.Display)."""
from __future__ import annotations

import pathlib
import re
import tempfile
import unittest

from AsteriaVm import Display
from AsteriaVm.Display import DisplayError, TextConsole
from Tests.test_amd64_fixtures import REPOSITORY

FOREGROUND = (0xFF, 0xFF, 0xFF)
BACKGROUND = (0x10, 0x20, 0x40)
START = '# display: 1280x800 rgbx, console 160x50'


def synthetic_glyph(code: int) -> bytes:
    """Distinct, non-blank glyph rows for every code but space."""
    if code == 0x20:
        return bytes(16)
    return bytes((code * 31 + row * 17 + 1) & 0xFF | (0x80 if row == 0 else 0) for row in range(16))


def write_repository(root: pathlib.Path) -> None:
    """A repository with a synthetic font and the console's colors, in the real headers' format."""
    font = root / Display.FONT_HEADER
    font.parent.mkdir(parents=True)
    body = []
    for code in range(1, 255):
        rows = synthetic_glyph(code)
        body.append('    // 0x%02X' % code)
        body.append('    ' + ', '.join('0x%02XU' % value for value in rows[:8]) + ',')
        body.append('    ' + ', '.join('0x%02XU' % value for value in rows[8:]) + ',')
    font.write_text('''namespace X {
inline constexpr Foundation::UInt32 Bold8x16FirstCode{0x01U};
inline constexpr Foundation::UInt32 Bold8x16Count{254U};
inline constexpr Foundation::UInt32 Bold8x16Width{8U};
inline constexpr Foundation::UInt32 Bold8x16Height{16U};
inline constexpr Foundation::UInt8 Bold8x16Glyphs[4064U]{
%s
};
}
''' % '\n'.join(body))
    cursor = root / Display.CURSOR_HEADER
    cursor.parent.mkdir(parents=True, exist_ok=True)
    cursor.write_text((REPOSITORY / Display.CURSOR_HEADER).read_text())
    console = root / Display.CONSOLE_HEADER
    console.parent.mkdir(parents=True)
    console.write_text('''inline constexpr Graphics::Color ConsoleForeground{.Red = 0xFFU, .Green = 0xFFU, .Blue = 0xFFU};
inline constexpr Graphics::Color ConsoleBackground{.Red = 0x10U, .Green = 0x20U, .Blue = 0x40U};
''')


class RepositoryCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.repository = pathlib.Path(cls.directory.name)
        write_repository(cls.repository)
        cls.font = Display.load_font(cls.repository)
        cls.colors = Display.console_colors(cls.repository)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def console(self, text: bytes, width: int = 1280, height: int = 800) -> TextConsole:
        console = TextConsole(width, height)
        console.write(text)
        return console


class TestFontAndColors(RepositoryCase):
    def test_font_parse(self):
        self.assertEqual(len(self.font), 256)
        self.assertEqual(self.font[0x41], synthetic_glyph(0x41))
        self.assertEqual(self.font[0x00], bytes(16))
        self.assertEqual(self.font[0xFF], bytes(16))
        self.assertEqual(sum(1 for code in range(256) if any(self.font[code])), 253)  # all but space

    def test_colors(self):
        self.assertEqual(self.colors, (FOREGROUND, BACKGROUND))

    def test_missing_headers(self):
        with tempfile.TemporaryDirectory() as empty:
            with self.assertRaisesRegex(DisplayError, 'cannot read'):
                Display.load_font(pathlib.Path(empty))
            with self.assertRaisesRegex(DisplayError, 'cannot read'):
                Display.console_colors(pathlib.Path(empty))
            problems = Display.display_problems(b'', [START], pathlib.Path(empty))
            self.assertEqual(len(problems), 1)
            self.assertIn('cannot read', problems[0])

    @unittest.skipUnless((REPOSITORY / Display.FONT_HEADER).exists() and (REPOSITORY / Display.CONSOLE_HEADER).exists(),
                         'the repository has no font or console header yet')
    def test_real_headers(self):
        font = Display.load_font(REPOSITORY)
        self.assertEqual(Display.console_colors(REPOSITORY), (FOREGROUND, BACKGROUND))
        text = (REPOSITORY / Display.FONT_HEADER).read_text()
        block = re.search(r'// 0x41\n(.*?)// 0x42', text, re.S).group(1)
        self.assertEqual(font[0x41], bytes(int(value, 16) for value in re.findall(r'0x[0-9A-F]{2}', block)))
        self.assertEqual(sum(1 for code in range(256) if font[code] != bytes(16)) > 200, True)


class TestConsoleSemantics(RepositoryCase):
    def row(self, console: TextConsole, row: int, length: int | None = None) -> bytes:
        cells = console.cells[row] if length is None else console.cells[row][:length]
        return bytes(cells)

    def test_geometry_is_clamped(self):
        self.assertEqual((self.console(b'').columns, self.console(b'').rows), (160, 50))
        big = TextConsole(2560, 1600)
        self.assertEqual((big.columns, big.rows), (320, 100))
        ia64 = TextConsole(2560, 1600, 'Ia64')
        self.assertEqual((ia64.columns, ia64.rows), (320, 38))
        small = TextConsole(100, 40)
        self.assertEqual((small.columns, small.rows), (12, 2))
        self.assertEqual(TextConsole(7, 100).rows, 0)

    def test_cells_start_blank(self):
        console = self.console(b'')
        self.assertTrue(all(code == 0x20 for line in console.cells for code in line))

    def test_deferred_wrap(self):
        console = self.console(b'a' * 160)
        self.assertEqual((console.row, console.column), (0, 160))
        console.write(b'b')
        self.assertEqual((console.row, console.column), (1, 1))
        self.assertEqual(console.cells[1][0], ord('b'))
        # a newline at the deferred position does not add a blank line
        console = self.console(b'a' * 160 + b'\nc')
        self.assertEqual((console.row, console.cells[1][0]), (1, ord('c')))

    def test_scroll_at_last_row(self):
        text = b''.join(b'%02d\n' % number for number in range(50))
        console = self.console(text)
        self.assertEqual(console.row, 49)
        self.assertEqual(self.row(console, 0, 2), b'01')
        self.assertEqual(self.row(console, 48, 2), b'49')
        self.assertEqual(self.row(console, 49, 2), b'  ')
        console.write(b'x')
        self.assertEqual(self.row(console, 49, 1), b'x')

    def test_wrap_scrolls_at_last_row(self):
        console = self.console(b'\n' * 49 + b'a' * 161)
        self.assertEqual((console.row, console.column), (49, 1))
        self.assertEqual(self.row(console, 48, 1), b'a')

    def test_tab(self):
        console = self.console(b'ab\tc')
        self.assertEqual(console.cells[0][8], ord('c'))
        self.assertEqual(console.cells[0][2], 0x20)
        console = self.console(b'\t\t')
        self.assertEqual(console.column, 16)
        console = self.console(b'a' * 157 + b'\t')
        self.assertEqual(console.column, 160)
        console = TextConsole(100, 40)
        console.write(b'abcdefghi\t')
        self.assertEqual(console.column, 12)

    def test_backspace_and_carriage_return(self):
        console = self.console(b'abc\b\bX')
        self.assertEqual(self.row(console, 0, 3), b'aXc')
        self.assertEqual(self.console(b'\b\bZ').cells[0][0], ord('Z'))
        console = self.console(b'abc\rZ')
        self.assertEqual(self.row(console, 0, 3), b'Zbc')
        console = self.console(b'a' * 160 + b'\bq')
        self.assertEqual((console.row, console.cells[0][159]), (0, ord('q')))

    def test_high_codes_are_stored(self):
        console = self.console(b'\x00\xff\x80')
        self.assertEqual(self.row(console, 0, 3), b'\x00\xff\x80')


class TestRender(RepositoryCase):
    def render(self, console: TextConsole, width: int = 1280, height: int = 800) -> bytes:
        return console.render(width, height, self.font, self.colors)

    def pixel(self, image: bytes, x: int, y: int, width: int = 1280) -> tuple[int, int, int]:
        offset = (y * width + x) * 3
        return tuple(image[offset:offset + 3])

    def test_cell_is_pixel_exact(self):
        image = self.render(self.console(b'\n  A'))
        self.assertEqual(len(image), 1280 * 800 * 3)
        glyph = synthetic_glyph(0x41)
        for line in range(16):
            for bit in range(8):
                expected = FOREGROUND if glyph[line] & (0x80 >> bit) else BACKGROUND
                self.assertEqual(self.pixel(image, 16 + bit, 16 + line), expected)

    def test_background_and_margin(self):
        image = self.render(self.console(b'x'), 1290, 810)
        self.assertEqual(len(image), 1290 * 810 * 3)
        self.assertEqual(self.pixel(image, 1285, 5, 1290), BACKGROUND)
        self.assertEqual(self.pixel(image, 5, 805, 1290), BACKGROUND)
        self.assertEqual(self.pixel(image, 1289, 809, 1290), BACKGROUND)

    def test_codes_outside_the_font_draw_blank(self):
        image = self.render(self.console(b'\x00\xff'))
        for y in range(16):
            for x in range(16):
                self.assertEqual(self.pixel(image, x, y), BACKGROUND)


class TestPpm(unittest.TestCase):
    def test_parse_with_comments(self):
        data = b'P6\n# made by qemu\n2 1 # size\n#more\n255\n' + bytes(range(6))
        self.assertEqual(Display.parse_ppm(data), (2, 1, bytes(range(6))))

    def test_pixels_may_begin_with_whitespace_bytes(self):
        data = b'P6 1 1 255\n' + b'\n\n\n'
        self.assertEqual(Display.parse_ppm(data), (1, 1, b'\n\n\n'))

    def test_rejects(self):
        for data in (b'P5\n1 1\n255\n\0', b'P6\n1 1\n65535\n\0\0\0\0\0\0', b'P6\n1 1\n255\n\0\0', b'P6\n1'):
            with self.assertRaises(DisplayError):
                Display.parse_ppm(data)


class TestLines(unittest.TestCase):
    def test_console_lines(self):
        lines = ['boot', START, 'a', 'b', '# display: stopped', 'after']
        self.assertEqual(Display.console_lines(lines), [START, 'a', 'b'])
        self.assertEqual(Display.console_lines(lines[:4]), [START, 'a', 'b'])
        self.assertIsNone(Display.console_lines(['boot', '# display: stopped']))

    def test_parse_start(self):
        self.assertEqual(Display.parse_start(START), (1280, 800, 'rgbx', 160, 50))
        self.assertEqual(Display.parse_start('# display: 640x480 bgrx, console 80x30'), (640, 480, 'bgrx', 80, 30))
        self.assertIsNone(Display.parse_start('# display: 640x480 argb, console 80x30'))
        self.assertIsNone(Display.parse_start('# display: stopped'))


class TestDisplayProblems(RepositoryCase):
    LINES = [START, 'Hello, Asteria', 'second line \x01\xfe', '# display: stopped']

    def screendump(self) -> bytearray:
        shown = Display.console_lines(self.LINES)
        console = self.console(''.join(line + '\n' for line in shown).encode('latin-1'))
        return bytearray(Display.draw_cursor(console.render(1280, 800, self.font, self.colors),
                                             1280, 800, (640, 400), self.repository))

    def problems(self, image) -> list[str]:
        return Display.display_problems(Display.write_ppm(1280, 800, bytes(image)), self.LINES, self.repository)

    def test_round_trip(self):
        self.assertEqual(self.problems(self.screendump()), [])

    def test_one_flipped_pixel_names_its_cell(self):
        image = self.screendump()
        x, y = 3 * 8 + 2, 2 * 16 + 5  # row 2 (the second text line, after the start line), column 3
        offset = (y * 1280 + x) * 3
        image[offset] ^= 0xFF
        problems = self.problems(image)
        self.assertIn('1 of 1024000 pixels differ', problems[0])
        self.assertIn('x 26..26, y 37..37', problems[0])
        self.assertIn("cell (row 2, column 3) expected 0x6F 'o'", problems[1])
        self.assertIn('1 pixels differ', problems[1])
        self.assertIn('neither foreground nor background', problems[1])

    def test_wrong_glyph_is_described(self):
        image = self.screendump()
        glyph = self.font[ord('Q')]
        for line in range(16):
            for bit in range(8):
                offset = ((16 + line) * 1280 + bit) * 3
                image[offset:offset + 3] = bytes(FOREGROUND if glyph[line] & (0x80 >> bit) else BACKGROUND)
        problems = self.problems(image)
        self.assertIn('cell (row 1, column 0)', problems[1])
        self.assertIn("expected 0x48 'H'", problems[1])
        self.assertIn('the glyph of 0x51', problems[1])

    def test_margin_difference(self):
        lines = ['# display: 1288x808 rgbx, console 161x50', '# display: stopped']
        console = self.console(lines[0].encode() + b'\n', 1288, 808)
        image = bytearray(Display.draw_cursor(console.render(1288, 808, self.font, self.colors),
                                             1288, 808, (644, 404), self.repository))
        ppm = lambda: Display.write_ppm(1288, 808, bytes(image))
        self.assertEqual(Display.display_problems(ppm(), lines, self.repository), [])
        image[(807 * 1288 + 1287) * 3] ^= 1
        problems = Display.display_problems(ppm(), lines, self.repository)
        self.assertEqual(len(problems), 2)
        self.assertIn('1 pixels differ outside the grid', problems[-1])

    def test_missing_or_malformed_start(self):
        ppm = Display.write_ppm(1280, 800, bytes(1280 * 800 * 3))
        self.assertIn('start line', Display.display_problems(ppm, ['x'], self.repository)[0])
        self.assertIn('malformed', Display.display_problems(ppm, ['# display: 1280x800'], self.repository)[0])

    def test_size_and_geometry_mismatch(self):
        ppm = Display.write_ppm(640, 480, bytes(640 * 480 * 3))
        self.assertIn('640x480, the mode is 1280x800', Display.display_problems(ppm, [START], self.repository)[0])
        ppm = Display.write_ppm(1280, 800, bytes(1280 * 800 * 3))
        bad = '# display: 1280x800 rgbx, console 80x25'
        self.assertIn('console 80x25', Display.display_problems(ppm, [bad], self.repository)[0])



class ResolutionOracleTests(unittest.TestCase):
    def test_wrong_but_self_consistent_screen_resolution_is_rejected(self):
        self.assertEqual(Display.resolution_problems(['# display: 1366x768 bgrx, console 160x48'], (1366, 768)), [])
        self.assertTrue(Display.resolution_problems([START], (1366, 768)))
        self.assertTrue(Display.resolution_problems([], (1366, 768)))
        self.assertEqual(Display.resolution_problems([START], None), [])

class QmpPpmStorageTests(unittest.TestCase):
    def test_aligned_rows_keep_every_pixel_and_drop_only_padding(self):
        pixels = bytes(range(18))
        packed = Display.write_ppm(3, 2, pixels)
        padded = Display.write_ppm(3, 2, pixels[:9] + b'pad' + pixels[9:] + b'PAD')
        self.assertEqual(Display.normalize_qmp_ppm(padded), packed)
        self.assertEqual(Display.normalize_qmp_ppm(packed), packed)
        with self.assertRaises(DisplayError):
            Display.parse_ppm(padded)
        for malformed in (padded[:-1], padded + b'x'):
            with self.assertRaises(DisplayError):
                Display.normalize_qmp_ppm(malformed)


class ConsoleLimitTests(unittest.TestCase):
    def test_limits_match_the_header(self):
        header = (REPOSITORY / 'Source/User/Console/TextConsole.hpp').read_text()
        ia64, other = header.split('#else', 1)
        def value(text, name):
            return int(re.search(r'%s\{([0-9\']+)U\}' % name, text)[1].replace("'", ''))
        self.assertEqual(Display.CONSOLE_LIMITS['Ia64'],
                         tuple(value(ia64, name) for name in ('ConsoleMaximumColumns', 'ConsoleMaximumRows', 'ConsoleMaximumCells')))
        self.assertEqual(Display.CONSOLE_LIMITS['Amd64'],
                         tuple(value(other, name) for name in ('ConsoleMaximumColumns', 'ConsoleMaximumRows',
                                                               'ConsoleMaximumCells')))

    def test_grid_fills_the_screen_within_the_limits(self):
        self.assertEqual((TextConsole(1280, 800).columns, TextConsole(1280, 800).rows), (160, 50))
        self.assertEqual((TextConsole(2560, 1440).columns, TextConsole(2560, 1440).rows), (320, 90))
        self.assertEqual((TextConsole(7680, 4320).columns, TextConsole(7680, 4320).rows), (640, 102))
        self.assertEqual((TextConsole(2560, 1440, 'Ia64').columns, TextConsole(2560, 1440, 'Ia64').rows), (320, 38))


if __name__ == '__main__':
    unittest.main()
