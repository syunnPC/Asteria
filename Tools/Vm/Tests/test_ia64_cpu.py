# SPDX-License-Identifier: Apache-2.0
"""Processor-state observers (Observers.Ia64Cpu): initial-image and probe-loop stopped-state checks."""
from __future__ import annotations

import unittest

from AsteriaVm import Ia64Layout
from AsteriaVm.Observers import Ia64Cpu, Ia64State
from Tests.test_ia64_fixtures import expectation, registers

USER_PSR = expectation('SyntheticProbe').constants.probe_user_psr  # PSR.i clear, PSR.di set
BOOT_PSR = expectation('Smp4').constants.user_psr  # PSR.i and PSR.di set; dfl, dfh clear
# The test firmware's idle loop in the Smp4 recording, from its ELF facts.
IDLE = expectation('Smp4').facts.user(Ia64Layout.USER_IDLE_SYMBOL).value
PROBE_LOOP = 0x2000000001001100


def mutate(text: str, old: str, new: str) -> str:
    assert old in text, old
    return text.replace(old, new)


class LoopPredicateTests(unittest.TestCase):
    """Stable processor state in the initial-image idle and probe loops."""

    def test_architected_slots_are_accepted(self):
        for loop in (IDLE, PROBE_LOOP):
            for slot in (0, 1, 2):
                self.assertEqual(Ia64Cpu.loop_state_problems(loop, USER_PSR | slot << 41, loop, USER_PSR), [])

    def test_invalid_states_are_rejected(self):
        for loop in (IDLE, PROBE_LOOP):
            self.assertTrue(Ia64Cpu.loop_state_problems(loop + 0x10, USER_PSR, loop, USER_PSR))
            self.assertTrue(Ia64Cpu.loop_state_problems(loop, USER_PSR | 3 << 41, loop, USER_PSR))
            self.assertTrue(Ia64Cpu.loop_state_problems(loop, USER_PSR & ~(1 << 17), loop, USER_PSR))
            self.assertTrue(Ia64Cpu.loop_state_problems(loop, 0x0000101008022018, loop, USER_PSR))  # kernel PSR
            # A thread that wrote f32-f127 (or f2-f31) shows PSR.mfh (mfl).
            self.assertEqual(Ia64Cpu.loop_state_problems(loop, USER_PSR | 0x30, loop, USER_PSR), [])
            self.assertTrue(Ia64Cpu.loop_state_problems(loop, USER_PSR | 0x40, loop, USER_PSR))


class WaitTests(unittest.TestCase):
    def wait(self, samples, **arguments):
        feed = iter(samples)
        return Ia64Cpu.wait_for_loop(lambda: next(feed), IDLE, USER_PSR, samples=3, timeout=5, interval=0,
                                     **arguments)

    def test_three_consecutive_samples_are_required(self):
        kernel = (0x41A1490, 0x0000101008022018, 0)
        idle = (IDLE, USER_PSR, 0)
        result = self.wait([kernel, idle, idle, kernel, idle, idle, idle, kernel])
        self.assertTrue(result.reached)
        self.assertEqual((result.polls, result.matches), (7, 3))
        self.assertEqual(Ia64Cpu.sample_problems(result, IDLE, USER_PSR), [])
        # The recorded outcome is judged against the loop it must be at.
        self.assertTrue(Ia64Cpu.sample_problems(result, PROBE_LOOP, USER_PSR))

    def test_a_halted_cpu_is_not_in_the_loop(self):
        halted = (IDLE, USER_PSR, 1)
        result = Ia64Cpu.wait_for_loop(lambda: halted, IDLE, USER_PSR, samples=3, timeout=0.05, interval=0.01)
        self.assertFalse(result.reached)
        self.assertIn('deadline', result.reason)
        self.assertTrue(Ia64Cpu.sample_problems(result, IDLE, USER_PSR))

    def test_abort_ends_the_wait(self):
        result = Ia64Cpu.wait_for_loop(lambda: (IDLE, USER_PSR, 0), IDLE, USER_PSR, samples=3, timeout=5,
                                       interval=0, abort=lambda: 'QEMU exited with status 1')
        self.assertFalse(result.reached)
        self.assertEqual((result.polls, result.reason), (0, 'QEMU exited with status 1'))
        self.assertIn('QEMU exited', Ia64Cpu.sample_problems(result, IDLE, USER_PSR)[0])


class StoppedStateTests(unittest.TestCase):
    def setUp(self):
        self.default = registers('Smp4')
        self.probe = registers('SyntheticProbe')

    def test_recorded(self):
        self.assertEqual(Ia64Cpu.stopped_state_problems(Ia64State.parse(self.default), IDLE, BOOT_PSR), [])
        self.assertEqual(Ia64Cpu.stopped_state_problems(Ia64State.parse(self.probe), PROBE_LOOP, USER_PSR), [])
        for text in (self.default, self.probe):
            state = Ia64State.parse(text)
            self.assertEqual(Ia64Cpu.pending_exception_problems(state), [])
            self.assertEqual(Ia64Cpu.register_nat_problems(state), [])
        self.assertEqual(Ia64Cpu.rse_problems(Ia64State.parse(self.probe), backing_store=0x2000000003001000,
                                              rsc=0xC), [])

    def test_stopped_state_mutants(self):
        # Mutants of the IP/PSR/META records.
        for text in (mutate(self.probe, f'{USER_PSR:016x}', f'{USER_PSR & ~(1 << 17):016x}'),
                     mutate(self.probe, 'HALTED: 0', 'HALTED: 1'),
                     mutate(self.probe, 'IP: 0x2000000001001100', 'IP: 0x2000000001001110'),
                     mutate(self.probe, 'META ip=2000000001001100', 'META ip=2000000001001110'),
                     mutate(self.probe, 'IA64STATE META ', 'IA64STATE-META ')):
            self.assertTrue(Ia64Cpu.stopped_state_problems(Ia64State.parse(text), PROBE_LOOP, USER_PSR))
        self.assertTrue(Ia64Cpu.stopped_state_problems(Ia64State.parse(self.default), PROBE_LOOP, USER_PSR))
        self.assertTrue(Ia64Cpu.stopped_state_problems(Ia64State.parse(self.default), IDLE, USER_PSR))

    def test_exception_and_nat_mutants(self):
        self.assertTrue(Ia64Cpu.pending_exception_problems(Ia64State.parse(
            mutate(self.probe, 'EXCEPTION code=0000000000000000', 'EXCEPTION code=0000000000000001'))))
        # QEMU's retained fault history is not judged.
        self.assertEqual(Ia64Cpu.pending_exception_problems(Ia64State.parse(
            mutate(self.probe, 'fault_code=0000000000000008', 'fault_code=0000000000000001'))), [])
        self.assertTrue(Ia64Cpu.register_nat_problems(Ia64State.parse(
            mutate(self.probe, 'hi=0000000000000000', 'hi=0000000000000001'))))

    def test_rse_mutants(self):
        # RSE mutants that concern architectural state; the partition counters
        # (bol/dirty/clean/invalid) are QEMU model details and dropped.
        for old, new in (('BSP=0x2000000003001000', 'BSP=0x2000000003001008'),
                         ('BSPSTORE=0x2000000003001000', 'BSPSTORE=0x2000000003002000'),
                         ('RNAT=0x0000000000000000 RNAT_ADDR', 'RNAT=0x0000000000000001 RNAT_ADDR'),
                         ('RNAT_ADDR=0x20000000030011f8', 'RNAT_ADDR=0x20000000030013f8'),
                         ('RNAT_DEFINED=0x0000000000000001', 'RNAT_DEFINED=0x0000000000000000'),
                         ('RSC=0x000000000000000c', 'RSC=0x0000000000000000')):
            state = Ia64State.parse(mutate(self.probe, old, new))
            self.assertTrue(Ia64Cpu.rse_problems(state, backing_store=0x2000000003001000, rsc=0xC), old)
        self.assertEqual(Ia64Cpu.rse_problems(Ia64State.parse(mutate(self.probe, 'invalid=92', 'invalid=91')),
                                              backing_store=0x2000000003001000, rsc=0xC), [])


if __name__ == '__main__':
    unittest.main()
