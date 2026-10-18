# SPDX-License-Identifier: Apache-2.0
"""The runner's terminal model (AsteriaVm.Terminal) against the C++ terminal.

The corpus check renders Tests/User/Terminal/TerminalCorpus.txt and compares
each image's SHA-256 with TerminalDigests.txt, which TerminalCorpusTests
checks for the C++ renderer; when ASTERIA_RENDER_OUTPUT_DIR names the
directory that test wrote, the PPMs are also compared byte for byte.
"""
from __future__ import annotations

import unittest

from AsteriaVm import RenderCorpus, Terminal
from Tests.test_amd64_fixtures import REPOSITORY

CORPUS = REPOSITORY / 'Tests/User/Terminal/TerminalCorpus.txt'
DIGESTS = REPOSITORY / 'Tests/User/Terminal/TerminalDigests.txt'


class TerminalCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = Terminal.TerminalModel(REPOSITORY)

    def terminal(self, data: bytes, columns: int = 10, rows: int = 4) -> Terminal.Terminal:
        terminal = self.model.terminal(columns, rows)
        terminal.write(data)
        return terminal

    def text(self, terminal: Terminal.Terminal) -> str:
        return '|'.join(''.join(chr(cell[0]) for cell in row).rstrip(' ')
                        for row in terminal.cells).rstrip('|')


class TestConstants(TerminalCase):
    def test_headers(self):
        model = self.model
        self.assertEqual((model.default_columns, model.default_rows), (80, 25))
        self.assertEqual((model.maximum_columns, model.maximum_rows), (320, 90))
        self.assertEqual(model.underline_row, 13)
        self.assertEqual(len(model.cp437), 254)
        self.assertEqual(model.map_code_point(0x2500), 0xC4)
        self.assertEqual(model.map_code_point(0x263A), 0x01)
        self.assertEqual(model.map_code_point(0x20AC), model.replacement)
        self.assertEqual(model.default_foreground, (0xFF, 0xFF, 0xFF))
        self.assertEqual(model.default_background, (0x10, 0x20, 0x40))

    def test_grid_for_output(self):
        self.assertEqual(self.model.grid_for_output(640, 400), (79, 23))
        self.assertEqual(self.model.grid_for_output(1920, 1080), (80, 25))
        self.assertEqual(self.model.grid_for_output(0, 0), (1, 1))

    def test_colors(self):
        model, default = self.model, self.model.default_color
        self.assertEqual(model.cell_colors((0x41, 0, default, default), False),
                         (model.default_foreground, model.default_background))
        self.assertEqual(model.cell_colors((0x41, model.bold, default, default), False)[0], model.default_foreground)
        self.assertEqual(model.cell_colors((0x41, model.bold, 1, 4), False), (model.colors[9], model.colors[4]))
        self.assertEqual(model.cell_colors((0x41, model.reverse, 1, 4), True), (model.colors[1], model.colors[4]))
        self.assertEqual(model.cell_colors((0x41, 0, 1, 4), True), (model.colors[4], model.colors[1]))


class TestBehaviour(TerminalCase):
    """A few rows of TerminalConformanceTests' tables, mirrored."""

    def test_deferred_wrap(self):
        terminal = self.terminal(b'0123456789')
        self.assertEqual((terminal.row, terminal.column, terminal.wrap_pending), (0, 9, True))
        self.assertEqual(self.text(self.terminal(b'0123456789A')), '0123456789|A')
        self.assertEqual(self.text(self.terminal(b'\x1b[?7l0123456789AB')), '012345678B')

    def test_cursor_and_erase(self):
        filled = b'0123456789\r\nabcdefghij\r\nABCDEFGHIJ'
        self.assertEqual(self.text(self.terminal(filled + b'\x1b[2;5H\x1b[J')), '0123456789|abcd')
        self.assertEqual(self.text(self.terminal(filled + b'\x1b[2;3H\x1b[2@')), '0123456789|ab  cdefgh|ABCDEFGHIJ')
        self.assertEqual(self.text(self.terminal(filled + b'\x1b[1;3r\x1b[2;1H\x1b[L')), '0123456789||abcdefghij')
        self.assertEqual(self.text(self.terminal(b'\x1b[2;3r\x1b[3;1H\x1b[5Ax')), '|x')

    def test_strings_and_utf8(self):
        self.assertEqual(self.text(self.terminal(b'a\x1bPq#0;1\x07still\x1b\\b')), 'ab')
        self.assertEqual(self.text(self.terminal(b'a\x1b]0;t\x1b[2Cx')), 'a  x')
        self.assertEqual(self.text(self.terminal(b'\xe0\x80\xafx')), '\ufffd\ufffd\ufffdx')
        self.assertEqual(self.text(self.terminal(b'a\xc2\x85b\xe2\x94\x80')), 'ab\u2500')

    def test_rendition_and_reports(self):
        default = self.model.default_color
        terminal = self.terminal(b'\x1b[38;5;196;4mA\x1b[44m\x1b[2K')
        self.assertEqual(terminal.cells[0][0], (0x20, 0, default, 4))
        terminal = self.terminal(b'\x1b[12;34H\x1b[6n\x1b[c\x1b[1c', 80, 25)
        self.assertEqual(terminal.take_responses(), b'\x1b[12;34R\x1b[?1;2c')
        terminal = self.terminal(b'\x1b[c' * 20, 80, 25)
        self.assertEqual((len(terminal.take_responses()), terminal.dropped), (126, 2))

    def test_unicode_storage(self):
        terminal = self.terminal('─é中😀'.encode('utf-8') + b'\xff')
        self.assertEqual([cell[0] for cell in terminal.cells[0][:5]], [0x2500, 0xE9, 0x4E2D, 0x1F600, 0xFFFD])
        self.assertEqual(self.model.map_code_point(0x4E2D), 0xFE)
        self.assertEqual(self.model.map_code_point(0xFFFD), 0xFE)

    def test_font_map_complete(self):
        self.assertEqual(set(self.model.cp437.values()), set(range(1, 0xFF)))
        for point, glyph in self.model.cp437.items():
            self.assertEqual(self.model.map_code_point(point), glyph)

    def test_resize(self):
        terminal = self.terminal(b'0123456789\r\nabcdefghij\r\n\x1b[44mX')
        terminal.resize(5, 2)
        terminal.resize(10, 4)
        self.assertEqual(self.text(terminal), '01234|abcde')
        self.assertEqual(terminal.cells[2][0][3], self.model.default_color)


class TestCorpus(TerminalCase):
    def test_corpus_matches_the_renderer(self):
        digests = RenderCorpus.read_digests(DIGESTS)
        cases = [case for case in RenderCorpus.read_cases(CORPUS)]
        self.assertEqual(sorted(case.name for case in cases), sorted(digests))
        for case in cases:
            with self.subTest(case=case.name):
                width, height, rgb = Terminal.render_case(self.model, case)
                image = RenderCorpus.ppm(width, height, rgb)
                renderer = RenderCorpus.renderer_output(case.name)
                if renderer is not None:
                    self.assertEqual(image, renderer, 'differs from the C++ renderer at %s'
                                     % RenderCorpus.first_difference(image, renderer, width))
                self.assertEqual(RenderCorpus.sha256(image), digests[case.name])


if __name__ == '__main__':
    unittest.main()
