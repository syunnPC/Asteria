# SPDX-License-Identifier: Apache-2.0
import signal
import unittest
from unittest import mock

from AsteriaVm import Scenarios
from AsteriaVm.Drivers import WholeMachineStall


class WholeMachineStallTests(unittest.TestCase):
    def test_pause_signals_only_its_child_and_resumes(self):
        process = mock.Mock()
        with mock.patch.object(WholeMachineStall.time, 'sleep') as sleep:
            WholeMachineStall.pause(process, 7)
        self.assertEqual(process.send_signal.call_args_list,
                         [mock.call(signal.SIGSTOP), mock.call(signal.SIGCONT)])
        sleep.assert_called_once_with(7)

    def test_interruption_still_resumes_the_child(self):
        process = mock.Mock()
        with mock.patch.object(WholeMachineStall.time, 'sleep', side_effect=RuntimeError('interrupted')):
            with self.assertRaises(RuntimeError):
                WholeMachineStall.pause(process, 7)
        self.assertEqual(process.send_signal.call_args_list,
                         [mock.call(signal.SIGSTOP), mock.call(signal.SIGCONT)])

    def test_both_isas_cover_boot_and_live_desktop_with_peers(self):
        cases = [case for case in Scenarios.CASES if case.whole_machine_pause]
        self.assertEqual({(case.isa, case.whole_machine_pause) for case in cases},
                         {(isa, phase) for isa in ('Amd64', 'Ia64') for phase in ('early', 'session')})
        self.assertTrue(all(case.cpus == 4 and case.product_desktop and not case.expected_stop
                            and case.driver == 'WholeMachineStall' for case in cases))

    def test_amd64_covers_both_tcg_modes_without_changing_other_cases(self):
        cases = [case for case in Scenarios.CASES if case.isa == 'Amd64']
        self.assertEqual({case.tcg_thread for case in cases if case.whole_machine_pause},
                         {'single', 'multi'})
        self.assertTrue(all(case.tcg_thread == 'multi' for case in cases if not case.whole_machine_pause))
