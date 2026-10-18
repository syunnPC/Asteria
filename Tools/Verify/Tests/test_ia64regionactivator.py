# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the runtime region activator judgments: the
RR-write leaf rules (a)-(g) and the register- and layout-agnostic
physical-bootstrap wrapper property. Fixtures are the meaningful instructions
of the real code (the wrapper as GCC emits it at O2) plus a small O0-shaped
function whose taken branch is the mask-clear edge; no build tree is needed.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import unittest

from AsteriaVerify.TargetChecks import Ia64RegionActivator as activator
from Tests.test_ia64runtimehelpers import fixture

LEAF, WRAPPER = activator.Ia64RegionActivatorLeaf(), activator.Ia64RegionActivatorPhysicalWrapper()

REGION_LEAF = fixture(0x0, """
0 mov r9=psr ;;
16 movl r14=0xc000 ;;
20 and r14=r9,r14 ;;
26 cmp.eq p7,p6=r14,r0 ;;
3c (p06) br.cond.dpnt.few <+0x270> ;;
40 mov r14=rr[r0] ;;
56 movl r15=0x200030 ;;
60 cmp.eq p7,p6=r14,r15 ;;
7c (p06) br.cond.dpnt.few <+0x270> ;;
86 movl r14=0x2000000000000000
96 movl r15=0x4000000000000000 ;;
a6 movl r16=0x6000000000000000
b6 movl r17=0x8000000000000000 ;;
c6 movl r18=0xa000000000000000
d6 movl r19=0xc000000000000000 ;;
e6 movl r20=0xe000000000000000 ;;
f0 rsm 0x2000 ;;
f6 srlz.d ;;
100 srlz.i ;;
106 mov rr[r14]=r32 ;;
110 mov rr[r15]=r33 ;;
116 mov rr[r16]=r34 ;;
120 mov rr[r17]=r35 ;;
126 mov rr[r18]=r36 ;;
130 mov rr[r19]=r37 ;;
136 mov rr[r20]=r38 ;;
140 srlz.d ;;
146 srlz.i ;;
150 mov r21=rr[r14] ;;
156 mov r22=rr[r15] ;;
160 mov r23=rr[r16] ;;
166 mov r24=rr[r17] ;;
170 mov r25=rr[r18] ;;
176 mov r26=rr[r19] ;;
180 mov r27=rr[r20] ;;
186 cmp.eq p7,p6=r21,r32 ;;
19c (p06) br.cond.dpnt.few <+0x280> ;;
1a0 cmp.eq p7,p6=r22,r33 ;;
1bc (p06) br.cond.dpnt.few <+0x280> ;;
1c0 cmp.eq p7,p6=r23,r34 ;;
1dc (p06) br.cond.dpnt.few <+0x280> ;;
1e0 cmp.eq p7,p6=r24,r35 ;;
1fc (p06) br.cond.dpnt.few <+0x280> ;;
200 cmp.eq p7,p6=r25,r36 ;;
21c (p06) br.cond.dpnt.few <+0x280> ;;
220 cmp.eq p7,p6=r26,r37 ;;
23c (p06) br.cond.dpnt.few <+0x280> ;;
240 cmp.eq p7,p6=r27,r38 ;;
25c (p06) br.cond.dpnt.few <+0x280> ;;
260 mov r8=r0
26c br.few <+0x290> ;;
270 mov r8=1
27c br.ret.sptk.many b0 ;;
280 mov r8=2 ;;
296 tbit.z p9,p8=r9,13 ;;
2a0 (p08) ssm 0x2000 ;;
2a6 srlz.d ;;
2b0 srlz.i ;;
2cc br.ret.sptk.many b0 ;;
""")

PHYSICAL_BOOTSTRAP_O2 = fixture(0x700, """
0 alloc r42=ar.pfs,19,12,0
6 adds r12=-64,r12
c mov r41=b0
10 mov r43=r1
1c mov r44=r32 ;;
20 adds r14=16,r12
26 adds r39=24,r12
2c adds r38=32,r12
30 adds r37=40,r12
36 adds r36=48,r12
3c adds r35=56,r12 ;;
40 adds r34=64,r12
46 adds r33=72,r12
4c mov r45=r14
50 st8 [r14]=r0
56 st8 [r39]=r0 ;;
60 st8 [r38]=r0
66 st8 [r37]=r0 ;;
70 st8 [r36]=r0
76 st8 [r35]=r0 ;;
86 st8 [r34]=r0
90 st8 [r33]=r0
9c br.call.sptk.many b0=<CollectValidatedRegionRegisters> ;;
a6 zxt1 r8=r8
ac mov r1=r43 ;;
b0 mov r40=r8
b6 cmp4.eq p7,p6=0,r8
bc (p06) br.cond.dpnt.few <+0x170> ;;
c0 ld8 r44=[r39]
c6 ld8 r45=[r38] ;;
d0 ld8 r46=[r37]
d6 ld8 r47=[r36] ;;
e0 ld8 r48=[r35]
e6 ld8 r49=[r34] ;;
f6 ld8 r50=[r33] ;;
100 mov r14=psr ;;
116 movl r15=0x100802e000 ;;
126 and r14=r15,r14 ;;
130 cmp.eq p6,p7=0,r14
13c (p07) br.cond.dpnt.few <+0x190> ;;
14c br.call.sptk.many b0=<AsteriaIa64ActivateRuntimeRegions> ;;
150 mov r1=r43
156 cmp.eq p6,p7=0,r8
15c (p06) br.cond.dpnt.few <+0x170> ;;
160 mov r40=3
166 cmp.eq p6,p7=1,r8
16c (p06) br.cond.dpnt.few <+0x190> ;;
170 mov r8=r40
176 mov b0=r41 ;;
17c mov.i ar.pfs=r42
186 adds r12=64,r12
18c br.ret.sptk.many b0
196 mov r40=2 ;;
1a0 mov r8=r40
1a6 mov b0=r41 ;;
1ac mov.i ar.pfs=r42
1b6 adds r12=64,r12
1bc br.ret.sptk.many b0 ;;
""")

# The O0 shape: the taken branch is the mask-clear edge, the result travels
# through another register to r8.
PHYSICAL_BOOTSTRAP_O0_SHAPE = fixture(0x0, """
0 mov r15=psr ;;
16 movl r14=0x100802e000 ;;
20 and r14=r14,r15 ;;
26 cmp.eq p6,p7=0,r14 ;;
2c (p06) br.cond.dptk.few <+0x40> ;;
30 mov r14=2
3c br.few <+0x60> ;;
40 mov r38=r9 ;;
4c br.call.sptk.many b0=<AsteriaIa64ActivateRuntimeRegions> ;;
50 mov r14=r8 ;;
60 mov r8=r14
6c br.ret.sptk.many b0 ;;
""")


PHYSICAL_BOOTSTRAP_SPILLED = fixture(0x0, """
0 mov r36=r12
6 adds r12=-112,r12 ;;
10 mov r15=psr ;;
16 adds r14=-88,r36 ;;
20 st8 [r14]=r15 ;;
26 adds r14=-88,r36 ;;
30 ld8 r14=[r14]
36 movl r15=0x100802e000 ;;
40 and r14=r15,r14 ;;
46 cmp.eq p6,p7=0,r14 ;;
5c (p06) br.cond.dptk.few <+0x80> ;;
60 mov r14=2
6c br.few <+0xa0> ;;
80 br.call.sptk.many b0=<AsteriaIa64ActivateRuntimeRegions> ;;
90 mov r14=r8 ;;
a0 mov r8=r14
ac br.ret.sptk.many b0 ;;
""")


class RegionLeafTests(unittest.TestCase):
    def test_the_leaf_holds(self):
        self.assertEqual(LEAF.item_problems(REGION_LEAF), [])

    def test_every_mutant_is_rejected(self):
        count = 0
        for description, mutant in activator.Ia64RegionActivatorLeaf().item_mutants(REGION_LEAF):
            with self.subTest(mutant=description):
                self.assertNotEqual(mutant, REGION_LEAF)
                self.assertTrue(LEAF.item_problems(mutant))
            count += 1
        self.assertEqual(count, 15)

    def test_predicated_psr_capture_is_rejected(self):
        self.assertTrue(LEAF.item_problems(REGION_LEAF.rewrite(0, '(p06) mov r9=psr')))

    def test_the_inverted_preflight_is_golden_only(self):
        # Inverting both preflight branches keeps every rule (a)-(g); only the golden pins it.
        inverted = REGION_LEAF
        for index in (4, 8):
            inverted = inverted.rewrite(index, '(p07) ' + inverted.instructions[index].text)
        self.assertEqual(LEAF.item_problems(inverted), [])


class PhysicalWrapperTests(unittest.TestCase):
    def test_both_codegen_shapes_hold(self):
        self.assertEqual(WRAPPER.verify(PHYSICAL_BOOTSTRAP_O2), [])
        self.assertEqual(WRAPPER.verify(PHYSICAL_BOOTSTRAP_O0_SHAPE), [])

    def test_copies_and_stack_spills_preserve_the_psr_value(self):
        self.assertEqual(WRAPPER.verify(PHYSICAL_BOOTSTRAP_SPILLED), [])
        roles, _ = activator.wrapper_roles(PHYSICAL_BOOTSTRAP_O0_SHAPE)
        copied = PHYSICAL_BOOTSTRAP_O0_SHAPE.rewrite(roles.test, 'and r14=r14,r16').insert(
            roles.test, 'mov r16=r15', stop=True)
        self.assertEqual(WRAPPER.verify(copied), [])

    def test_spill_overwrites_and_wrong_reload_are_rejected(self):
        code = PHYSICAL_BOOTSTRAP_SPILLED
        store = code.first(lambda step: step.operation.memory == 'store')
        load = code.first(lambda step: step.operation.memory == 'load')
        mutants = [code.rewrite(store, 'st8 [r14]=r0'), code.rewrite(store, 'st4 [r14]=r15'),
                   code.rewrite(5, 'adds r14=-80,r36'),
                   code.insert(load, '(p06) st8 [r14]=r0', stop=True),
                   code.insert(load, 'st1 [r14]=r0', stop=True)]
        for index, mutant in enumerate(mutants):
            with self.subTest(mutant=index):
                self.assertTrue(WRAPPER.verify(mutant))

    def test_every_mutant_is_rejected(self):
        for code in (PHYSICAL_BOOTSTRAP_O2, PHYSICAL_BOOTSTRAP_O0_SHAPE):
            count = 0
            for description, mutant in activator.Ia64RegionActivatorPhysicalWrapper().mutants(code):
                with self.subTest(start=code.start, mutant=description):
                    self.assertNotEqual(mutant, code)
                    self.assertTrue(WRAPPER.verify(mutant))
                count += 1
            self.assertEqual(count, 13)

    def test_psr_operand_and_clobbers_are_rejected(self):
        for code in (PHYSICAL_BOOTSTRAP_O2, PHYSICAL_BOOTSTRAP_O0_SHAPE):
            roles, _ = activator.wrapper_roles(code)
            register = sorted(roles.capture.operation.general_writes)[0]
            mutants = [code.rewrite(roles.test, roles.test.text.replace(register, 'r0')),
                       code.insert(roles.test, 'mov %s=r0' % register, stop=True),
                       code.rewrite(roles.capture, '(p06) ' + roles.capture.text),
                       code.rewrite(roles.mask, '(p06) ' + roles.mask.text)]
            for mutant in mutants:
                with self.subTest(start=code.start, mutant=mutant):
                    self.assertTrue(WRAPPER.verify(mutant))

    def test_the_rejection_value_is_tracked_through_registers(self):
        roles, problems = activator.wrapper_roles(PHYSICAL_BOOTSTRAP_O2)
        self.assertEqual(problems, [])
        values, found = activator.returned_values(PHYSICAL_BOOTSTRAP_O2, roles.set_edge[1])
        self.assertEqual(found, [])
        self.assertEqual({value[0] for value in values}, {2})


if __name__ == '__main__':
    unittest.main()
