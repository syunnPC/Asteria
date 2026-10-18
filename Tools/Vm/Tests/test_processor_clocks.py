# SPDX-License-Identifier: Apache-2.0
"""Clock evidence must exclude certain skew, including negative residuals."""
from types import SimpleNamespace as Record
import unittest

from AsteriaVm.Observers import ProcessorClocks


class ClockTests(unittest.TestCase):
    def evidence(self, lower=-20, upper=20, excluded=0, flags=7):
        return Record(Published=3, Frequency=1_000_000, EpsilonTicks=50, ExcludedMask=excluded,
                      Processors=[Record(Lower=0, Upper=0, RoundTrip=0, Flags=7),
                                  Record(Lower=lower, Upper=upper, RoundTrip=upper-lower, Flags=flags),
                                  Record(Lower=0, Upper=0, RoundTrip=0, Flags=0),
                                  Record(Lower=0, Upper=0, RoundTrip=0, Flags=0)])

    def test_wide_intervals_do_not_exclude_a_processor(self):
        self.assertEqual(ProcessorClocks.problems(self.evidence(-100_000, 100_000), 2), [])

    def test_certain_skew_requires_placement_exclusion(self):
        for lower, upper in ((100, 110), (-110, -100)):
            self.assertEqual(ProcessorClocks.problems(self.evidence(lower, upper, 2), 2), [])
            self.assertTrue(ProcessorClocks.problems(self.evidence(lower, upper), 2))

    def test_all_online_processors_must_publish(self):
        evidence = self.evidence()
        evidence.Published = 1
        self.assertTrue(ProcessorClocks.problems(evidence, 2))

    def test_a_non_invariant_counter_is_excluded(self):
        self.assertEqual(ProcessorClocks.problems(self.evidence(excluded=2, flags=1), 2), [])
        self.assertTrue(ProcessorClocks.problems(self.evidence(flags=1), 2))

    def test_hypervisor_gate_uses_residual_without_invariant_tsc(self):
        self.assertEqual(ProcessorClocks.problems(self.evidence(flags=1 | 8), 2), [])
        self.assertTrue(ProcessorClocks.problems(self.evidence(excluded=2, flags=1 | 8), 2))
        for lower, upper in ((100, 110), (-110, -100)):
            self.assertEqual(ProcessorClocks.problems(self.evidence(lower, upper, 2, flags=1 | 8), 2), [])
            self.assertTrue(ProcessorClocks.problems(self.evidence(lower, upper, flags=1 | 8), 2))
