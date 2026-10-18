# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the user-mode launch checks (TargetChecks/Ia64UserModeLaunch.py).

The golden Tests/Golden/Ia64/Scheduler/AsteriaIa64LaunchUserMode.dis
serves as a build-independent fixture: its `stopBefore:stopAfter:text` lines
are turned back into an instruction stream (a stop carried by a dropped nop
becomes a stopped nop again), on which the transition and store rules, their
acceptance variants and their mutants run without a build tree.
"""
from __future__ import annotations

import pathlib
import unittest

from AsteriaVerify import Golden, Layout
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.TargetChecks import Ia64UserModeLaunch as Launch

ROOT = pathlib.Path(__file__).resolve().parents[3]
GOLDEN = ROOT / 'Tests' / 'Golden' / 'Ia64' / 'Scheduler' / 'AsteriaIa64LaunchUserMode.dis'


def golden_stream() -> tuple[Instruction, ...]:
    instructions: list[Instruction] = []
    address = 0
    for line in Golden.parse(GOLDEN).lines:
        before, after, text = line.split(':', 2)
        if before == '1' and instructions and not instructions[-1].stop_after:
            instructions.append(Instruction(address, 'nop.i 0x0', 'MII', 2, True))
            address += 6
        predicate = None
        if text.startswith('('):
            predicate, text = text[1:].split(') ', 1)
        instructions.append(Instruction(address, text, 'MII', 0, after == '1', predicate))
        address += 6
    return tuple(instructions)


class LayoutTests(unittest.TestCase):
    def test_repository_headers(self):
        layout = Launch._layout(str(ROOT))
        self.assertEqual(layout.plan_offset('EntryBundleAddress'), 0)
        self.assertEqual(layout.plan_offset('SupervisorContinuationAddress'), 56)
        self.assertEqual([layout.plan_offset('InitialArguments.Register%d' % n) for n in (4, 5, 6, 7)],
                         [64, 72, 80, 88])
        self.assertEqual(layout.plan_offset('InitialGlobalPointer'), 96)
        self.assertEqual(layout.field('State'), 528)
        self.assertEqual(layout.register('FloatingRegisters'), 176)
        self.assertEqual((layout.register_state_offset, layout.register_state_size, layout.state_size),
                         (0, 496, 8))
        self.assertEqual(layout.initial_fpsr, 0x0009804C0270033F)
        self.assertEqual((layout.retained, layout.processor_reject, layout.supervisor_reject), (1, 1, 2))

    def test_missing_assertion_is_an_error(self):
        launch = (ROOT / Launch.LAUNCH_HEADER).read_text().replace('Ia64UserModeInitialFloatingPointStatus ==',
                                                                   'SomethingElse ==')
        with self.assertRaises(Layout.LayoutError):
            Launch.parse_layout(launch, (ROOT / Launch.CONTEXT_HEADER).read_text(), (ROOT / Launch.SOURCE).read_text())


class OperandTests(unittest.TestCase):
    def test_updating_memory_base(self):
        updating = {'ld8 r2=[r23],8': 23, 'st8 [r24]=r2,8': 24, 'ld8.acq r2=[r25],-8': 25,
                    'st8.rel [r26]=r2,r3': 26, 'ld8.fill r2=[r27],r3': 27, 'st8.spill [r28]=r2,-16': 28,
                    'ld4.c.nc r2=[r29],-4': 29, 'st8 [r30]=r2,r3': 30, 'lfetch [r31],16': 31}
        for text, base in updating.items():
            with self.subTest(text):
                self.assertEqual(Launch.updating_memory_base(text), base)
        for text in ('ld8.acq r2=[r31]', 'cmpxchg8.acq r2=[r31],r3,ar.ccv', 'st8.rel [r25]=r2', 'ld8 r23=[r23]',
                     'adds r23=64,r14'):
            with self.subTest(text):
                self.assertIsNone(Launch.updating_memory_base(text))


class GoldenStreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.facts = Launch.LeafFacts(Code(0, golden_stream()), Launch._layout(str(ROOT)))

    def test_rules_accept_the_reviewed_stream(self):
        self.assertEqual(Launch.Ia64LaunchTransition().verify(self.facts), [])
        self.assertEqual(Launch.Ia64LaunchCaptureStores().verify(self.facts), [])

    def test_acceptance_variants(self):
        reordered = Launch.reordered_preparation(self.facts)
        self.assertNotEqual(reordered, self.facts)
        self.assertEqual(Launch.Ia64LaunchTransition().verify(reordered), [])
        non_updating = Launch.non_updating_memory(reordered)
        self.assertEqual(Launch.Ia64LaunchTransition().verify(non_updating), [])
        # The second variant has a stray release store, violating capture-store rules.
        self.assertNotEqual(Launch.Ia64LaunchCaptureStores().verify(non_updating), [])

    def test_mutants_are_rejected(self):
        for prop in (Launch.Ia64LaunchTransition(), Launch.Ia64LaunchCaptureStores()):
            names = set()
            for name, mutant in prop.mutants(self.facts):
                with self.subTest(prop.name, mutant=name):
                    self.assertNotIn(name, names)
                    names.add(name)
                    self.assertNotEqual(mutant, self.facts)
                    self.assertNotEqual(prop.verify(mutant), [])

    def test_semantic_rules_alone_reject_the_former_oracle_mutants(self):
        """Semantic clauses reject all 63 semantic mutants and 13 transition sentinels."""
        mutants = list(Launch.Ia64LaunchTransition().mutants(self.facts))
        former = mutants[:63 + 13]
        self.assertEqual(len(former), 76)
        for name, mutant in former:
            with self.subTest(name):
                self.assertNotEqual(Launch.semantic_problems(mutant), [])

    def test_commit_rules_alone_reject_their_new_mutants(self):
        """Commit ordering and register installs reject these mutants independently."""
        names = ('alloc before the capture', 'second r12 write', 'missing srlz.d after the cut',
                 'invala without a stop', 'ar.rnat in the BSPSTORE group', 'ar.rsc installed before BSPSTORE',
                 'ipsr installed before iip')
        mutants = dict(Launch.Ia64LaunchTransition().mutants(self.facts))
        for name in names:
            with self.subTest(name):
                self.assertEqual(Launch.semantic_problems(mutants[name]), [])
                self.assertNotEqual(Launch.commit_problems(mutants[name]), [])


if __name__ == '__main__':
    unittest.main()
