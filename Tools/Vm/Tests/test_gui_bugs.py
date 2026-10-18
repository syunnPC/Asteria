# SPDX-License-Identifier: Apache-2.0
"""Regression oracle must reject both wrong geometry and gray content."""
import unittest
import pathlib
from types import SimpleNamespace
from unittest import mock
from AsteriaVm import Display, Scenarios, Session
from AsteriaVm.Drivers import GuiBugs


class GuiBugsTests(unittest.TestCase):
    def test_title_point_excludes_buttons_on_the_narrow_demo_frame(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        model = Session.SessionModel(root)
        frame = (32, 32, 64, 45)
        px, py = frame[0]+GuiBugs.Script.title_x, frame[1]+GuiBugs.Script.title_y
        for x, y, w, h in (model.close_button_rect(frame), model.maximize_button_rect(frame)):
            self.assertFalse(x <= px < x+w and y <= py < y+h)
        x, y, w, h = model.title_bar_rect(frame)
        self.assertTrue(x <= px < x+w and y <= py < y+h)

    def test_cursor_can_be_observed_at_the_clipped_physical_corner(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        cursor = Display.load_cursor(root)
        rgb = Display.draw_cursor(bytes((48, 104, 104))*100*80, 100, 80, (99, 79), root)
        self.assertEqual(GuiBugs.clipped_cursor_position(rgb, 100, 80, cursor), (99, 79))

    def test_frame_fence_rejects_a_torn_frame_and_excludes_client_pixels(self):
        root = pathlib.Path(__file__).resolve().parents[3]
        script = object.__new__(GuiBugs.Script)
        script.context = SimpleNamespace(target=SimpleNamespace(root=root))
        script.model = Session.SessionModel(root)
        script.model.ui_faces = (False, False)
        script.observe_pointer = lambda: (90, 70)
        window = Session.Window((0, 0, 80, 60), name=b'boot-log', state='focused')
        rgb = Display.draw_cursor(Session.render(script.model, 100, 80, [window]), 100, 80, (90, 70), root)
        logged = ['# session: font Bold8x16-fallback Bold8x16-fallback', '# session: begin 1 100 80',
                  '# session: title 0 '+b'boot-log'.hex()+' -']
        current = {0: [0, 0, 80, 60, 1, 1, 72, 31, 1, 0]}
        self.assertTrue(script.frame_matches(100, 80, rgb, logged, current))
        broken = bytearray(rgb)
        broken[:3] = bytes((1, 2, 3))
        self.assertFalse(script.frame_matches(100, 80, broken, logged, current))

    def judge(self, window, pixels):
        script = object.__new__(GuiBugs.Script)
        script.context = SimpleNamespace(check=mock.Mock())
        script.model = SimpleNamespace(constants=dict(FrameExtraWidth=2, FrameExtraHeight=2),
                                       colors=dict(Face=(192, 192, 192)),
                                       content_rect=lambda frame: (1, 1, 2, 2))
        script.wait = lambda predicate: predicate()
        script.scene = lambda: {0: window}
        script.picture = lambda name: (4, 4, pixels)
        script.geometry('test', (0, 0, 4, 4), True)
        return {call.args[0]: call.args[1] for call in script.context.check.call_args_list}

    def test_accepts_full_image_and_gray_decorations(self):
        pixels = bytearray(bytes((192, 192, 192))*16)
        for index in (5, 6, 9, 10):
            pixels[index*3:index*3+3] = bytes((16, 32, 64))
        self.assertTrue(all(self.judge([0, 0, 4, 4, 1, 1, 2, 2, 3, 2], pixels).values()))

    def test_rejects_gray_inside_client_content(self):
        verdict = self.judge([0, 0, 4, 4, 1, 1, 2, 2, 3, 2], bytes((192, 192, 192))*16)
        self.assertFalse(verdict['gui.test.content-covered'])

    def test_rejects_old_frame_with_maximized_look_and_old_image(self):
        verdict = self.judge([1, 1, 3, 3, 1, 1, 1, 1, 3, 2], bytes((16, 32, 64))*16)
        self.assertFalse(verdict['gui.test.geometry'])
        self.assertFalse(verdict['gui.test.arranged-image'])

    def test_both_regressions_cover_both_clients_and_both_isas(self):
        for isa in ('Amd64', 'Ia64'):
            cases = Scenarios.select([], [isa + '.Gui*'])
            self.assertEqual(len(cases), 6)
            for case in cases:
                self.assertEqual(case.driver, 'GuiBugs')
                self.assertIn(isa + 'Session', case.suites)
                self.assertTrue(case.session_input or case.product_desktop)
                self.assertFalse(case.assignable)
