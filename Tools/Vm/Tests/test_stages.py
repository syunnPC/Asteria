# SPDX-License-Identifier: Apache-2.0
"""Stage events (AsteriaVm.Stages): parsing, waiting and the order/absence rules."""
from __future__ import annotations

import unittest

from AsteriaVm import Console, Stages
from Tests.test_ia64_fixtures import lines


class ParseTests(unittest.TestCase):
    def test_names_and_lines(self):
        self.assertEqual(Stages.line('AP-RUNTIME'), 'ASTERIA:STAGE:AP-RUNTIME')
        for bad in ('ap-runtime', 'AP_RUNTIME', 'AP-', '-AP', 'AP--RUNTIME', '', 'AP:CPU'):
            with self.assertRaises(ValueError, msg=bad):
                Stages.line(bad)

    def test_stages_and_malformed_lines(self):
        text = ['ASTERIA:STAGE:BOOT', 'USER:ASTERIA:STAGE:FAKE', 'ASTERIA:STAGE:AP-RUNTIME', 'ASTERIA:STAGE:',
                'ASTERIA:STAGE:Lower', 'ASTERIA:STAGE:A:B', 'firmware ASTERIA:STAGE:X', 'ASTERIA:BOOT-INFO:PASS']
        self.assertEqual(Stages.stages(text), ['BOOT', 'AP-RUNTIME'])
        self.assertEqual(Stages.malformed(text), ['ASTERIA:STAGE:', 'ASTERIA:STAGE:Lower', 'ASTERIA:STAGE:A:B'])

    def test_a_stage_event_is_not_a_failure(self):
        for text in ('ASTERIA:STAGE:BOOT', 'ASTERIA:STAGE:AP-RUNTIME', 'ASTERIA:STAGE:NATIVE-PREEMPTION-RECORDED'):
            self.assertIsNone(Console.FAILURE.match(text), text)

    def test_predicate_matches_whole_lines(self):
        matches = Stages.predicate('BOOT')
        self.assertEqual(matches.__name__, 'ASTERIA:STAGE:BOOT')
        self.assertTrue(matches('ASTERIA:STAGE:BOOT'))
        for text in ('ASTERIA:STAGE:BOOT-INFO', ' ASTERIA:STAGE:BOOT', 'USER:ASTERIA:STAGE:BOOT'):
            self.assertFalse(matches(text), text)


class ProblemTests(unittest.TestCase):
    EXPECTED = ('A', 'B', 'C')

    def events(self, *names: str) -> list[str]:
        return ['noise'] + [Stages.line(name) for name in names]

    def test_order_presence_and_absence(self):
        self.assertEqual(Stages.problems(self.events('A', 'X', 'B', 'C'), self.EXPECTED, absent=('Y',)), [])
        cases = {
            'missing': self.events('A', 'C'),
            'twice': self.events('A', 'B', 'B', 'C'),
            'out of order': self.events('B', 'A', 'C'),
            'absent reported': self.events('A', 'B', 'C', 'Y'),
            'malformed': self.events('A', 'B', 'C') + ['ASTERIA:STAGE:bad'],
        }
        for name, events in cases.items():
            self.assertTrue(Stages.problems(events, self.EXPECTED, absent=('Y',)), name)

    def test_recorded_boot(self):
        from AsteriaVm.Drivers import Ia64Default
        expected, absent = Ia64Default.stage_expectation(False, Ia64Default.DEFAULT_STAGES)
        self.assertEqual(Stages.problems(lines('Smp2'), expected, absent=absent), [])
        self.assertEqual(Stages.stages(lines('Smp2'))[-1], Ia64Default.LAUNCH_STAGE)
        probe, absent = Ia64Default.stage_expectation(False, Ia64Default.PROBE_STAGES)
        self.assertEqual(Stages.problems(lines('SyntheticProbe'), probe, absent=absent), [])
        self.assertTrue(Stages.problems(lines('Smp2'), probe, absent=absent))


if __name__ == '__main__':
    unittest.main()
