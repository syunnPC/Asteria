# SPDX-License-Identifier: Apache-2.0
import unittest
from types import SimpleNamespace as N
from AsteriaVm.Observers import UserProcessors as P


def record():
    processors = [N(Dispatches=1, Ticks=1, RunStops=1, CreatedDispatches=1, CreatedTicks=1,
                    Resolved=1, IdleRoot=4096*(cpu+1), IdleScrubVisits=1) for cpu in range(4)]
    threads = [N(ThreadIdentity=cpu+1, Home=cpu, SupervisionTag=P.SMP_TAG,
                 RanMask=1<<cpu, Ticks=1, Faults=1) for cpu in range(4)]
    return N(Published=15, BootRanMask=1, SelfTestRanMask=1, CreatedRanMask=15,
             Processors=processors, Threads=threads)


class UserProcessorTests(unittest.TestCase):
    def check_record(self, value):
        layout = N(idle_root=lambda pool, cpu: pool + 4096*(cpu+1))
        return P.problems(value, 4, amd64=True, constants=layout, spinners=True)

    def test_complete_four_processor_proof(self):
        self.assertEqual(self.check_record(record()), [])

    def test_reject_missing_tick_and_fault_resolution(self):
        for member in ('CreatedTicks', 'RunStops', 'Resolved'):
            value = record()
            setattr(value.Processors[2], member, 0)
            self.assertTrue(self.check_record(value))

    def test_reject_foreign_home_and_shared_idle(self):
        value = record()
        value.Threads[1].RanMask = 4
        self.assertTrue(self.check_record(value))
        value = record()
        value.Processors[3].IdleRoot = value.Processors[1].IdleRoot
        self.assertTrue(self.check_record(value))

    def test_reject_pager_self_test_on_ap(self):
        value = record()
        value.SelfTestRanMask = 3
        self.assertTrue(self.check_record(value))
        value = record()
        value.Threads[1].SupervisionTag = 0
        self.assertTrue(self.check_record(value))

    def test_ia64_stays_on_bsp(self):
        value = record()
        self.assertTrue(P.problems(value, 4))

    def test_desktop_must_itself_use_ap(self):
        value = record()
        self.assertTrue(P.session_problems(value))
        value.Threads[1].SupervisionTag = 256
        self.assertEqual(P.session_problems(value), [])

    def test_two_processor_desktop_requires_ticks_without_the_four_cpu_fault_probe(self):
        value = record()
        value.Published = value.CreatedRanMask = 3
        value.Threads = value.Threads[:2]
        value.Threads[1].SupervisionTag = 256
        for thread in value.Threads:
            thread.Faults = 0
        for processor in value.Processors:
            processor.Resolved = 0
        for cpu in (2, 3):
            value.Processors[cpu] = N(Dispatches=0, Ticks=0, RunStops=0,
                CreatedDispatches=0, CreatedTicks=0, Resolved=0, IdleRoot=0, IdleScrubVisits=0)
        self.assertEqual(P.problems(value, 2, amd64=True), [])
        self.assertEqual(P.session_problems(value, 2), [])
        value.Processors[1].CreatedTicks = 0
        self.assertTrue(P.session_problems(value, 2))
