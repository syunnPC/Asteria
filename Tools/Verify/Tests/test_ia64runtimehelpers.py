# SPDX-License-Identifier: Apache-2.0
"""Unit tests of runtime asm helper verification.

The fixtures are the meaningful instructions of the real helpers (address
offset, text, and `;;` when an instruction-group stop follows before the
next meaningful instruction), so no build tree is needed.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import unittest

from AsteriaVerify import Disassembly, Golden
from AsteriaVerify.ControlFlow import Code, written
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.TargetChecks import Ia64RuntimeHelpers as helpers


def fixture(start: int, text: str) -> Code:
    """Code from `offset text [;;]` lines (meaningful instructions only)."""
    instructions = []
    for line in text.strip().splitlines():
        offset, body = line.split(' ', 1)
        stop = body.endswith(' ;;')
        address = start + int(offset, 16)
        instructions.append(written(Instruction(address, '', 'MII', (address & 0xF) // 6, stop),
                                    body[:-3] if stop else body))
    return Code(start, tuple(instructions))


SERIALIZATION, LEAF = helpers.Ia64RuntimeHelperCrSerialization(), helpers.Ia64RuntimeHelperLeafDiscipline()
PUBLICATION, DESCRIPTOR = helpers.Ia64RuntimeHelperEvidencePublication(), helpers.Ia64RuntimeHelperDescriptorWords()
SWITCH = helpers.Ia64KernelContextSwitchPhases()


AP_EVIDENCE = fixture(0x0, """
0 mov.m r14=ar.bsp
6 mov.m r15=ar.bspstore
10 mov.m r16=ar.rnat
16 mov.m r17=ar.rsc
20 mov r18=psr ;;
26 st8 [r32]=r12,8 ;;
30 st8 [r32]=r14,8 ;;
36 st8 [r32]=r15,8 ;;
40 st8 [r32]=r16,8 ;;
46 st8 [r32]=r17,8 ;;
50 st8 [r32]=r18,8 ;;
56 mov r14=1
60 mf ;;
66 st1 [r32]=r14 ;;
7c br.ret.sptk.many b0 ;;
""")

BSP_PREEMPTION_DESCRIPTOR = fixture(0xc0, """
6 movl r14=<reloc:IMM64 AsteriaIa64BspNativePreemptionCoordinator>
10 mov r15=r1 ;;
26 movl r16=<reloc:IMM64 AsteriaIa64BspNativePreemptionSentinelLandingStart>
36 movl r17=<reloc:IMM64 AsteriaIa64BspNativePreemptionSentinelLandingEnd> ;;
46 movl r18=<reloc:IMM64 AsteriaIa64BspNativePreemptionIdleLanding> ;;
50 st8 [r32]=r14,8 ;;
56 st8 [r32]=r15,8 ;;
60 st8 [r32]=r16,8 ;;
66 st8 [r32]=r17,8 ;;
70 st8 [r32]=r18,8 ;;
76 st8 [r32]=r15 ;;
8c br.ret.sptk.many b0 ;;
""")

PROGRAM_TIMER = fixture(0xa0, """
0 mov cr.itm=r32 ;;
6 srlz.d ;;
10 mov cr.itv=r33 ;;
16 srlz.d ;;
20 mov r14=cr.itm
26 mov r15=cr.itv ;;
30 cmp.eq p7,p6=r14,r32 ;;
4c (p06) br.cond.dpnt.few <+0x130> ;;
50 cmp.eq p7,p6=r15,r33 ;;
6c (p06) br.cond.dpnt.few <+0x130> ;;
70 mov.m r16=ar.itc ;;
76 cmp.ltu p7,p6=r16,r32 ;;
8c (p06) br.cond.dpnt.few <+0xa0> ;;
90 mov r8=r0
9c br.ret.sptk.many b0 ;;
a0 mov cr.itv=r36 ;;
a6 srlz.d ;;
b0 mov cr.itm=r34 ;;
b6 srlz.d ;;
c0 mov cr.itv=r35 ;;
c6 srlz.d ;;
d0 mov r14=cr.itm
d6 mov r15=cr.itv ;;
e0 cmp.eq p7,p6=r14,r34 ;;
fc (p06) br.cond.dpnt.few <+0x130> ;;
100 cmp.eq p7,p6=r15,r35 ;;
11c (p06) br.cond.dpnt.few <+0x130> ;;
120 mov r8=2
12c br.ret.sptk.many b0 ;;
130 mov cr.itv=r36 ;;
136 srlz.d ;;
140 mov r14=cr.itv ;;
146 cmp.eq p7,p6=r14,r36 ;;
15c (p06) br.cond.dpnt.few <+0x170> ;;
160 mov r8=1
16c br.ret.sptk.many b0 ;;
170 mov r8=3
17c br.ret.sptk.many b0 ;;
""")

def all_rejected(test: unittest.TestCase, judge, facts, mutants) -> int:
    count = 0
    for description, mutant in mutants:
        with test.subTest(mutant=description):
            test.assertNotEqual(mutant, facts)
            test.assertTrue(judge(mutant), description)
        count += 1
    return count


class SerializationTests(unittest.TestCase):
    def test_program_timer_is_serialized(self):
        self.assertEqual(SERIALIZATION.item_problems(PROGRAM_TIMER), [])

    def test_mutants_are_rejected(self):
        mutants = helpers.Ia64RuntimeHelperCrSerialization().item_mutants(PROGRAM_TIMER)
        self.assertEqual(all_rejected(self, SERIALIZATION.item_problems, PROGRAM_TIMER, mutants), 4)

    def test_an_unserialized_write_is_reported(self):
        broken = PROGRAM_TIMER.insert(14, 'mov cr.itv=r36', stop=True)
        self.assertTrue(SERIALIZATION.item_problems(broken))


class LeafDisciplineTests(unittest.TestCase):
    ALLOWED = frozenset({'cr.itm', 'cr.itv', 'ar.itc'})

    def test_program_timer_is_a_leaf(self):
        self.assertEqual(LEAF.item_problems((self.ALLOWED, PROGRAM_TIMER)), [])

    def test_every_forbidden_operation_is_reported(self):
        mutants = list(helpers.Ia64RuntimeHelperLeafDiscipline().item_mutants((self.ALLOWED, PROGRAM_TIMER)))
        self.assertEqual(len(mutants), len(helpers.FORBIDDEN_LEAF_OPERATIONS) + 1)
        for description, (allowed, mutant) in mutants:
            with self.subTest(mutant=description):
                self.assertTrue(LEAF.item_problems((allowed, mutant)))

    def test_interrupt_request_registers_are_matched(self):
        # Interrupt-request registers include cr.irr0-3.
        mutant = PROGRAM_TIMER.insert(0, 'mov r14=cr.irr3', stop=True)
        self.assertEqual(len(LEAF.item_problems((self.ALLOWED, mutant))), 1)

    def test_relocations_are_reported(self):
        relocated = Code(PROGRAM_TIMER.start, PROGRAM_TIMER.instructions,
                         (RelocationFact(0, 'PCREL21B', 'AsteriaIa64CancelLocalTimer', 0),))
        self.assertTrue(LEAF.item_problems((self.ALLOWED, relocated)))


class PublicationTests(unittest.TestCase):
    def test_evidence_capture_publishes_in_order(self):
        self.assertEqual(PUBLICATION.item_problems(AP_EVIDENCE), [])

    def test_mutants_are_rejected(self):
        mutants = helpers.Ia64RuntimeHelperEvidencePublication().item_mutants(AP_EVIDENCE)
        self.assertEqual(all_rejected(self, PUBLICATION.item_problems, AP_EVIDENCE, mutants), 7)


class DescriptorTests(unittest.TestCase):
    EXPECTED = (helpers.address_of(helpers.COORDINATOR), helpers.GP, helpers.address_of(helpers.LANDING_START),
                helpers.address_of(helpers.LANDING_END), helpers.address_of(helpers.IDLE_LANDING), helpers.GP)

    def test_native_preemption_descriptor_words(self):
        self.assertEqual(DESCRIPTOR.item_problems((self.EXPECTED, BSP_PREEMPTION_DESCRIPTOR)), [])

    def test_the_order_of_the_words_matters(self):
        swapped = self.EXPECTED[:2] + (self.EXPECTED[3], self.EXPECTED[2]) + self.EXPECTED[4:]
        self.assertTrue(DESCRIPTOR.item_problems((swapped, BSP_PREEMPTION_DESCRIPTOR)))

    def test_mutants_are_rejected(self):
        item = (self.EXPECTED, BSP_PREEMPTION_DESCRIPTOR)
        self.assertEqual(all_rejected(self, DESCRIPTOR.item_problems,
                                      item, helpers.Ia64RuntimeHelperDescriptorWords().item_mutants(item)), 7)


class SwitchPhaseTests(unittest.TestCase):
    CODE = fixture(0, """
0 st8 [r20]=r24 ;;
10 flushrs
16 mov.m ar.rsc=r0 ;;
20 stf.spill [r20]=f31,16 ;;
30 mf ;;
36 invala ;;
40 sync.i ;;
46 adds r20=80,r15 ;;
50 ld8 r16=[r20] ;;
6c br.ret.sptk.many b6 ;;
""")

    def test_phases_hold(self):
        self.assertEqual(SWITCH.item_problems(self.CODE), [])

    def test_mutants_are_rejected(self):
        mutants = helpers.Ia64KernelContextSwitchPhases().item_mutants(self.CODE)
        self.assertEqual(all_rejected(self, SWITCH.item_problems, self.CODE, mutants), 6)


class DroppedOracleTests(unittest.TestCase):
    """Golden geometry and helper ownership checks."""

    def test_golden_comparison_reports_a_size_mismatch(self):
        record = Golden.GoldenRecord('binutils', 'AsteriaIa64ProgramLocalTimer', None, 0x180, ['O2'], ['0:1:x'])
        self.assertTrue(Golden.compare(record, ['0:1:x'], 0x170, 'O2', 'golden'))
        self.assertTrue(Golden.compare(record, ['0:1:x'], 0x190, 'O2', 'golden'))
        self.assertEqual(Golden.compare(record, ['0:1:x'], 0x180, 'O2', 'golden'), [])

    def test_normalization_is_stop_sensitive(self):
        code = fixture(0, """
0 mov cr.itv=r32 ;;
6 srlz.d ;;
1c br.ret.sptk.many b0 ;;
""")
        lines = Disassembly.normalize_ia64_groups(code.instructions)
        self.assertNotEqual(lines, Disassembly.normalize_ia64_groups(code.without_stops(0, 1).instructions))
        self.assertNotEqual(lines, Disassembly.normalize_ia64_groups(code.without_stops(1, 2).instructions))


if __name__ == '__main__':
    unittest.main()
