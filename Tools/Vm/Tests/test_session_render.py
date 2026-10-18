# SPDX-License-Identifier: Apache-2.0
"""The runner's desktop-session scene model (AsteriaVm.Session) against the C++ renderer.

Every case of Tests/User/Session/SceneCorpus.txt is rendered by the model and
its PPM's SHA-256 compared with Tests/User/Session/SceneDigests.txt, which the
C++ SceneCorpusTests checks too. When ASTERIA_RENDER_OUTPUT_DIR names the
directory the C++ corpus tests wrote (the CTest test User/Session/
RenderModelCompare sets it), the PPMs are also compared byte for byte.
"""
from __future__ import annotations

import pathlib
import tempfile
import unittest

from AsteriaVm import RenderCorpus, Session
from Tests.test_amd64_fixtures import REPOSITORY

CORPUS = REPOSITORY / 'Tests/User/Session/SceneCorpus.txt'
DIGESTS = REPOSITORY / 'Tests/User/Session/SceneDigests.txt'
TERMINAL_CORPUS = REPOSITORY / 'Tests/User/Terminal/TerminalCorpus.txt'


def terminal_images(cases: list[RenderCorpus.Case]) -> dict[str, Session.Image] | None:
    """The terminal corpus's images, when a scene case shows one."""
    wanted = {str(tokens[2]) for case in cases for _line, tokens in case.statements
              if tokens[0] == 'image' and len(tokens) > 2 and tokens[1] == 'terminal'}
    if not wanted:
        return None
    from AsteriaVm import Terminal
    model = Terminal.TerminalModel(REPOSITORY)
    images = {}
    for case in RenderCorpus.read_cases(TERMINAL_CORPUS):
        if case.name in wanted:
            width, height, rgb = Terminal.render_case(model, case)
            images[case.name] = Session.Image(width, height, rgb)
    return images


class TestSessionModel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = Session.SessionModel(REPOSITORY)

    def test_constants_come_from_the_headers(self):
        constants = self.model.constants
        self.assertEqual(self.model.policy['CascadeOrigin'], 32)
        self.assertEqual(self.model.policy['CascadeStep'], 24)
        self.assertEqual(self.model.policy['DoubleClickUs'], 500000)
        self.assertEqual(constants['FrameWidth'], 4)
        self.assertEqual(constants['TitleBarHeight'], 20)
        self.assertEqual(constants['ResizeZone'], constants['FrameWidth'])
        self.assertEqual(self.model.marks['CloseGlyph'][:2], (8, 7))
        self.assertEqual(self.model.marks['MaximizeGlyph'][3:], (3, 2))  # (3, 2)
        self.assertEqual(self.model.strings['NotRespondingMark'], b' (not responding)')
        self.assertEqual(self.model.colors['Desktop'], (0x30, 0x68, 0x68))
        self.assertNotEqual(self.model.colors['Desktop'], (0x10, 0x20, 0x40))  # The console's

    def test_geometry(self):
        # An 80x25 terminal is 640x400 content in a 648x429 frame.
        frame = self.model.frame_around_content(100, 50, 640, 400)
        self.assertEqual(frame, (96, 25, 648, 429))
        self.assertEqual(self.model.content_rect(frame), (100, 50, 640, 400))
        self.assertEqual(self.model.close_button_rect((0, 0, 648, 429)), (648 - 22, 7, 16, 14))
        self.assertEqual(self.model.maximize_button_rect((0, 0, 648, 429)), (648 - 40, 7, 16, 14))
        # IA-64's firmware mode: a maximized terminal has 632x371 content, 79x23 cells.
        self.assertEqual(self.model.content_rect(self.model.maximized_frame(640, 400)), (4, 25, 632, 371))
        self.assertEqual(self.model.switcher_rect(400, 300, 10), (144, 132, 112, 36))
        self.assertEqual(self.model.switcher_rect(400, 300, 100), (16, 132, 368, 36))
        self.assertEqual(self.model.switcher_rect(30, 20, 1), (0, 0, 30, 36))

    def test_title_text(self):
        window = Session.Window(frame=(0, 0, 10, 10), name=b'term', subtitle='x\x04\u2666'.encode('utf-8'), volume=True,
                                not_responding=True)
        self.assertEqual(self.model.title_text(window), '\u2666 term - x?? (not responding)'.encode('utf-8'))
        self.assertEqual(self.model.title_text(Session.Window(frame=(0, 0, 1, 1), name=b'shell')), b'shell')

    def test_unicode_title_and_class_marker(self):
        window = Session.Window(frame=(0, 0, 10, 10), name='café'.encode('utf-8'),
                                subtitle='─中♦'.encode('utf-8'), volume=True)
        self.assertEqual(self.model.title_text(window), '♦ café - ─中?'.encode('utf-8'))

    def test_missing_header_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(RenderCorpus.CorpusError):
                Session.SessionModel(pathlib.Path(directory))


class TestSceneCorpus(unittest.TestCase):
    def test_every_case_matches_the_renderer(self):
        model = Session.SessionModel(REPOSITORY)
        cases = RenderCorpus.read_cases(CORPUS)
        digests = RenderCorpus.read_digests(DIGESTS)
        self.assertEqual(sorted(case.name for case in cases), sorted(digests))
        images = terminal_images(cases)
        for case in cases:
            with self.subTest(case=case.name):
                width, height, rgb = Session.render_case(model, case, images)
                data = RenderCorpus.ppm(width, height, rgb)
                renderer = RenderCorpus.renderer_output(case.name)
                if renderer is not None:
                    self.assertTrue(data == renderer, RenderCorpus.first_difference(data, renderer, width))
                self.assertEqual(RenderCorpus.sha256(data), digests[case.name])

    def test_corpus_errors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'corpus.txt'
            path.write_text('case scene a 4 4 rgbx\nwindow 0 0 9 29 sideways\nend\n')
            case = RenderCorpus.read_cases(path)[0]
            with self.assertRaises(RenderCorpus.CorpusError):
                Session.render_case(Session.SessionModel(REPOSITORY), case)
            path.write_text('case scene a 4 4 rgbx\n')
            with self.assertRaises(RenderCorpus.CorpusError):
                RenderCorpus.read_cases(path)


if __name__ == '__main__':
    unittest.main()
