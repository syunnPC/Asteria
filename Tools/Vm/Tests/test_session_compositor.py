# SPDX-License-Identifier: Apache-2.0
"""complete-fence replay and test-only activation."""
import pathlib
import unittest
from AsteriaVm import Scenarios, Session

ROOT = pathlib.Path(__file__).resolve().parents[3]


class SessionCompositorTests(unittest.TestCase):
    def test_damage_copies_previous_image_and_changes_only_patch(self):
        first = Session.client_pattern_image(160, 100, 1, 2).rgb
        second = Session.client_pattern_image(160, 100, 2, 2).rgb
        different = sum(first[i:i + 3] != second[i:i + 3] for i in range(0, len(first), 3))
        self.assertEqual(different, 24 * 18)
        self.assertEqual(first[-3:], second[-3:])

    def test_only_complete_presented_snapshots_replay(self):
        lines = ['# session: begin 1 320 128', '# session: window 0 0 0 168 129 1 1 160 100 1',
                 '# session: title 0 7061747465726e 74696c6573', '# session: presented 1 pixels 40960 us 100']
        rgb = Session.transcript_output(ROOT, 640, 400, lines, (400, 300))
        self.assertEqual(len(rgb), 640 * 400 * 3)
        at = (25 * 640 + 4) * 3
        self.assertEqual(rgb[at:at + 3], Session.client_pattern_image(160, 100, 1, 1).rgb[:3])
        with self.assertRaises(ValueError):
            Session.transcript_output(ROOT, 640, 400, lines[:-1], (400, 300))
        with self.assertRaises(ValueError):
            Session.transcript_output(ROOT, 640, 400, [*lines[:-1], '# session: presented 2 pixels 40960 us 100'], (400, 300))

    def test_both_isas_activate_only_the_test_receipt(self):
        for isa in ('Amd64', 'Ia64'):
            cases = Scenarios.select([], [isa + '.SessionCompositor'])
            self.assertEqual(len(cases), 1)
            self.assertTrue(cases[0].session_compositor)
            self.assertTrue(cases[0].guest)
            self.assertFalse(cases[0].assignable)
            self.assertIn(isa + 'Session', cases[0].suites)
