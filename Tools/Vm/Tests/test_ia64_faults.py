# SPDX-License-Identifier: Apache-2.0
import unittest

from AsteriaVm.Observers import Ia64Faults


class NativeFaultReports(unittest.TestCase):
    def test_all_supervisor_probes_and_each_missing_or_duplicate_result(self):
        lines = ['USER:' + line for line in Ia64Faults.LINES]
        self.assertEqual(Ia64Faults.problems(lines), [])
        for index in range(len(lines)):
            with self.subTest(index=index):
                self.assertTrue(Ia64Faults.problems(lines[:index] + lines[index + 1:]))
                self.assertTrue(Ia64Faults.problems(lines[:index] + [lines[index]] + lines[index:]))

    def test_wrong_reason_and_order(self):
        lines = ['USER:' + line for line in Ia64Faults.LINES]
        self.assertTrue(Ia64Faults.problems(list(reversed(lines))))
        lines[0] = lines[0].replace('slot', 'address')
        self.assertTrue(Ia64Faults.problems(lines))
