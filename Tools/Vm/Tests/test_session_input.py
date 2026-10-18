# SPDX-License-Identifier: Apache-2.0
"""activation and replay of window health/decorations."""
import pathlib
import unittest
from AsteriaVm import Display, Scenarios, Session
from AsteriaVm.Drivers import SessionInput, SessionStress, SessionIdle

ROOT = pathlib.Path(__file__).resolve().parents[3]


class SessionInputTests(unittest.TestCase):
    def test_native_hardware_path_on_both_isas(self):
        for isa in ('Amd64', 'Ia64'):
            case, = Scenarios.select([], [isa + '.SessionInput'])
            self.assertTrue(case.session_input)
            self.assertTrue(case.i8042)
            self.assertTrue(case.guest)
            self.assertFalse(case.assignable)
            self.assertEqual(case.driver, 'SessionInput')
            self.assertIn(isa + 'Session', case.suites)

    def test_stress_uses_real_seat_smp_and_slow_present_media(self):
        for isa in ('Amd64', 'Ia64'):
            for cpus in (2, 4):
                case, = Scenarios.select([], ['%s.SessionStressSmp%d' % (isa, cpus)])
                self.assertTrue(case.session_stress and case.session_input and case.i8042)
                self.assertEqual(case.cpus, cpus)
                self.assertEqual(case.display_policy, '1024x768')
                self.assertIn(isa + 'Session', case.suites)
        self.assertGreaterEqual(SessionStress.CYCLES, 20)
        xs, ys = zip(*SessionStress.BURST_POINTS)
        self.assertLess(max(xs) - min(xs), 128)
        self.assertLess(max(ys) - min(ys), 128)
        self.assertLess(min(xs), 256)
        self.assertGreater(max(xs), 256)

    def test_stress_observer_requires_a_complete_scene(self):
        lines = ['# session: begin 1 256 112', '# session: window 0 200 35 72 53 1 1 64 24 3 0',
                 '# session: presented 1 pixels 10 us 100', '# session: begin 2 256 112']
        self.assertEqual(SessionStress.complete(lines), lines[:3])
        self.assertEqual(SessionStress.complete(lines[-1:]), [])

    def test_stress_complete_scene_retains_its_font_selection(self):
        font = '# session: font Vegur-Bold Vegur-Regular'
        lines = [font, '# session: font-map read-only Map',
                 '# window-event: 1 13 1 0 0 0 0', '# session: begin 1 136 112',
                 '# session: presented 1 pixels 10 us 100',
                 '# session: font Bold8x16-fallback Bold8x16-fallback',
                 '# session: begin 2 136 112']
        self.assertEqual(SessionStress.complete(lines), [font, *lines[3:5]])

    def test_stress_pointer_arrival_requires_one_complete_arrow(self):
        width, height = 540, 260
        cursor = Display.load_cursor(ROOT)
        background = bytes((17, 34, 51)) * width * height
        rgb = Display.draw_cursor(background, width, height, (430, 230), ROOT)
        self.assertEqual(SessionStress.cursor_position(rgb, width, height, cursor), (430, 230))
        ambiguous = Display.draw_cursor(rgb, width, height, (380, 190), ROOT)
        self.assertIsNone(SessionStress.cursor_position(ambiguous, width, height, cursor))
        damaged = bytearray(rgb)
        at = ((230 - cursor[2][1]) * width + 430 - cursor[2][0]) * 3
        damaged[at:at + cursor[0] * 3] = background[at:at + cursor[0] * 3]
        self.assertIsNone(SessionStress.cursor_position(bytes(damaged), width, height, cursor))

    def test_manual_demo_wait_is_checked_beyond_the_old_timeout(self):
        self.assertGreater(SessionIdle.IDLE_SECONDS, 60)
        for isa in ('Amd64', 'Ia64'):
            case, = Scenarios.select([], [isa + '.SessionIdleSmp4'])
            self.assertTrue(case.session_input and not case.session_stress)
            self.assertEqual((case.cpus, case.driver), (4, 'SessionIdle'))

    def test_health_flag_changes_decoration_pixels_only(self):
        lines = ['# session: begin 1 256 112',
                 '# session: window 0 20 4 208 53 1 1 200 24 3 0',
                 '# session: title 0 7061747465726e 74696c6573',
                 '# session: input dropped 0',
                 '# session: presented 1 pixels 28672 us 100']
        healthy = Session.transcript_output(ROOT, 640, 400, lines, (400, 300))
        lines[1] = lines[1][:-1] + '1'
        hung = Session.transcript_output(ROOT, 640, 400, lines, (400, 300))
        self.assertTrue(healthy != hung)
        for y in range(29, 53):
            start = (y * 640 + 24) * 3
            self.assertEqual(healthy[start:start + 200 * 3], hung[start:start + 200 * 3])

    def test_diagnostic_events_keep_surface_and_drop_count(self):
        lines = ['# window-event: 1 17 2 1 4 0 65',
                 '# session: begin 1 256 112',
                 '# session: window 0 32 32 72 53 0 1 64 24 1 1',
                 '# session: begin 2 256 112',
                 '# session: window 1 56 56 72 45 1 1 64 16 3 0']
        self.assertEqual(SessionInput.events(lines), [(1, 17, 2, 1, 4, 0, 65)])
        self.assertEqual(set(SessionInput.windows(lines)), {1})
        self.assertEqual(SessionInput.windows(lines)[1][-1], 0)
