# SPDX-License-Identifier: Apache-2.0
"""Unit tests of physical-access verification: corridor safety,
the MMIO M1-M8 port (double- and quad-word leaves) with its acceptance
variants, and the diagnostic writer's relocation closure. Fixtures are the meaningful instructions of the
real leaves; no build tree is needed.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import dataclasses
import unittest

from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.TargetChecks import Ia64PhysicalAccess as physical
from Tests.test_ia64runtimehelpers import fixture

UART_ADAPTER = fixture(0x0, """
0 mov r14=psr
6 mov r16=r32
c mov r17=r33
10 mov r22=r34
16 mov r23=r35 ;;
26 movl r24=0x8000000000000000 ;;
30 or r15=r22,r24
36 or r18=r23,r24
40 rsm 0x4000 ;;
46 srlz.d ;;
50 mov.m r20=ar.rsc ;;
5c dep r21=0,r20,0,2 ;;
60 mov.m ar.rsc=r21 ;;
66 rsm 0x20000 ;;
70 srlz.d ;;
80 ld1.acq r19=[r18] ;;
8c tbit.z p7,p6=r19,5 ;;
96 (p06) br.cond.sptk.few <+0xf0>
a6 hint.i 0x0
ac adds r17=-1,r17 ;;
b0 cmp.eq p7,p6=r17,r0 ;;
cc (p06) br.cond.sptk.few <+0x80>
d0 mov r8=r0 ;;
ec br.few <+0x110> ;;
f0 mf ;;
f6 st1 [r15]=r16 ;;
100 mf.a
106 mov r8=1 ;;
116 tbit.z p7,p6=r14,17
11c tbit.z p9,p8=r14,14 ;;
120 (p06) ssm 0x20000 ;;
126 srlz.d ;;
130 mov.m ar.rsc=r20 ;;
136 (p08) ssm 0x4000 ;;
140 srlz.d ;;
15c br.ret.sptk.many b0 ;;
""")

MMIO_READ = fixture(0x0, """
0 mov r14=psr
6 mov r16=r32 ;;
10 rsm 0x4000 ;;
16 srlz.d ;;
20 mov.m r20=ar.rsc ;;
2c dep r21=0,r20,0,2 ;;
30 mov.m ar.rsc=r21 ;;
36 rsm 0x20000 ;;
40 srlz.d ;;
56 movl r17=0x8000000000000000 ;;
60 or r18=r16,r17 ;;
66 mf ;;
70 ld4.acq r8=[r18] ;;
76 mf.a ;;
86 tbit.z p7,p6=r14,17
8c tbit.z p9,p8=r14,14 ;;
90 (p06) ssm 0x20000 ;;
96 srlz.d ;;
a0 mov.m ar.rsc=r20 ;;
a6 (p08) ssm 0x4000 ;;
b0 srlz.d ;;
cc br.ret.sptk.many b0 ;;
""")

MMIO_WRITE = fixture(0xd0, """
0 mov r14=psr
6 mov r16=r32
c mov r19=r33 ;;
10 rsm 0x4000 ;;
16 srlz.d ;;
20 mov.m r20=ar.rsc ;;
2c dep r21=0,r20,0,2 ;;
30 mov.m ar.rsc=r21 ;;
36 rsm 0x20000 ;;
40 srlz.d ;;
56 movl r17=0x8000000000000000 ;;
60 or r18=r16,r17 ;;
66 mf ;;
70 st4 [r18]=r19 ;;
76 mf.a ;;
86 tbit.z p7,p6=r14,17
8c tbit.z p9,p8=r14,14 ;;
90 (p06) ssm 0x20000 ;;
96 srlz.d ;;
a0 mov.m ar.rsc=r20 ;;
a6 (p08) ssm 0x4000 ;;
b0 srlz.d ;;
cc br.ret.sptk.many b0 ;;
""")

# The quad-word leaves (the VT-d register accessors) differ only in the access width.
QUAD_READ = MMIO_READ.rewrite(MMIO_READ.first('ld4.acq r8=[r18]'), 'ld8.acq r8=[r18]')
QUAD_WRITE = MMIO_WRITE.rewrite(MMIO_WRITE.first('st4 [r18]=r19'), 'st8 [r18]=r19')

LEAVES = {
    physical.UART_ADAPTER: UART_ADAPTER,
    physical.MMIO_READ: MMIO_READ,
    physical.MMIO_WRITE: MMIO_WRITE,
    physical.MMIO_READ_QUAD: QUAD_READ,
    physical.MMIO_WRITE_QUAD: QUAD_WRITE,
}
MMIO_LEAVES = ((MMIO_READ, 'Read', 4), (MMIO_WRITE, 'Write', 4), (QUAD_READ, 'Read', 8), (QUAD_WRITE, 'Write', 8))


class CorridorTests(unittest.TestCase):
    def test_the_leaves_hold_the_corridor(self):
        for name, code in LEAVES.items():
            with self.subTest(leaf=name):
                self.assertEqual(physical.corridor_problems(code, physical.CORRIDOR_SPECS[name]), [])

    def test_every_mutant_is_rejected(self):
        for name, code in LEAVES.items():
            for description, mutant in physical.Ia64PhysicalAccessCorridor().item_mutants(code):
                with self.subTest(leaf=name, mutant=description):
                    self.assertNotEqual(mutant, code)
                    self.assertTrue(physical.corridor_problems(mutant, physical.CORRIDOR_SPECS[name]))

    def test_the_uart_poll_loop_may_branch_but_mmio_may_not(self):
        looped = MMIO_READ.insert(20, 'br.few <+0x70>', stop=True)
        problems = physical.corridor_problems(looped, physical.CORRIDOR_SPECS[physical.MMIO_READ])
        self.assertTrue(any(problem.startswith('return') for problem in problems))

    def test_the_quad_word_leaves_allow_only_their_width(self):
        for name, code, narrow in ((physical.MMIO_READ_QUAD, QUAD_READ, 'ld4.acq r8=[r18]'),
                                   (physical.MMIO_WRITE_QUAD, QUAD_WRITE, 'st4 [r18]=r19')):
            access = code.first(lambda step: step.operation.memory is not None)
            problems = physical.corridor_problems(code.rewrite(access, narrow), physical.CORRIDOR_SPECS[name])
            with self.subTest(leaf=name):
                self.assertTrue(any(problem.startswith('access') for problem in problems))

    def test_uncached_alias_evaluation(self):
        load = UART_ADAPTER.first(lambda step: step.operation.memory == 'load').position
        self.assertEqual(physical.register_value(UART_ADAPTER, 'r18', load), ('bit63',))
        self.assertIsNone(physical.register_value(UART_ADAPTER, 'r23', load))
        load = MMIO_READ.first(lambda step: step.operation.memory == 'load').position
        self.assertEqual(physical.register_value(MMIO_READ, 'r18', load), ('bit63',))
        self.assertIsNone(physical.register_value(MMIO_READ, 'r16', load))

    def test_a_merged_capture_group_is_not_a_hazard(self):
        # Input captures may share a group when no register is read after
        # being written within that group.
        merged = UART_ADAPTER.without_boundary(4)
        self.assertEqual(physical.corridor_problems(merged, physical.CORRIDOR_SPECS[physical.UART_ADAPTER]), [])


class MemoryMappedIoTests(unittest.TestCase):
    def test_the_leaves_pass(self):
        for code, operation, width in MMIO_LEAVES:
            with self.subTest(operation=operation, width=width):
                self.assertEqual(physical.mmio_problems(code, operation, width), ())

    def test_the_operation_matters(self):
        self.assertTrue(physical.mmio_problems(MMIO_READ, 'Write'))
        self.assertTrue(physical.mmio_problems(MMIO_WRITE, 'Read'))

    def test_the_width_matters(self):
        for code, operation, width in MMIO_LEAVES:
            with self.subTest(operation=operation, width=width):
                self.assertTrue(physical.mmio_problems(code, operation, physical.OTHER_WIDTH[width]))

    def test_the_old_oracle_mutants_are_rejected(self):
        for code, operation, width in MMIO_LEAVES:
            count = 0
            for description, mutant in physical.mmio_mutants(code, operation, width):
                with self.subTest(operation=operation, width=width, mutant=description):
                    self.assertNotEqual(mutant, code)
                    self.assertTrue(physical.mmio_problems(mutant, operation, width))
                count += 1
            self.assertEqual(count, 39)

    def test_the_legitimate_variants_are_accepted(self):
        for code, operation, width in MMIO_LEAVES:
            variants = list(physical.mmio_variants(code, operation))
            self.assertEqual(len(variants), 3)
            for description, variant in variants:
                with self.subTest(operation=operation, width=width, variant=description):
                    self.assertNotEqual(variant, code)
                    self.assertEqual(physical.mmio_problems(variant, operation, width), ())

    def test_relocations_are_rejected(self):
        relocated = dataclasses.replace(MMIO_READ, relocations=(RelocationFact(0x20, 'PCREL21B', 'memcpy', 0),))
        self.assertTrue(physical.mmio_problems(relocated, 'Read'))


class WriterTests(unittest.TestCase):
    WRITER = physical.DIAGNOSTIC_WRITER
    ADAPTER = physical.UART_ADAPTER
    HALT = '_ZN7Asteria8Platform8Firmware21HaltAfterFirmwareExitEv'

    def functions(self, writer_relocations=(), adapter_relocations=(), halt_relocations=()):
        return {
            self.WRITER: physical.FunctionFacts(tuple(writer_relocations), (('direct', self.ADAPTER, 0),)),
            self.ADAPTER: physical.FunctionFacts(tuple(adapter_relocations), ()),
            self.HALT: physical.FunctionFacts(tuple(halt_relocations), ()),
        }

    def test_a_call_to_the_adapter_in_another_object_is_allowed(self):
        # After the asm moves to Diagnostic.S, the call is a PCREL21B to a symbol of another object.
        call = RelocationFact(0x92, 'PCREL21B', self.ADAPTER, 0)
        self.assertEqual(physical.writer_problems((self.WRITER,), self.functions([call])), [])

    def test_data_and_gp_relative_relocations_are_rejected_transitively(self):
        for relocations in ([RelocationFact(0x10, 'LTOFF22X', '.rodata', 0)],
                            [RelocationFact(0x10, 'IMM64', '.rodata', 0)]):
            with self.subTest(relocations=relocations):
                self.assertTrue(physical.writer_problems((self.WRITER,), self.functions(relocations)))
                self.assertTrue(physical.writer_problems((self.WRITER,), self.functions((), relocations)))

    def test_unreachable_functions_are_not_judged(self):
        gp = [RelocationFact(0x10, 'GPREL22', '.data', 0)]
        self.assertEqual(physical.writer_problems((self.WRITER,), self.functions((), (), gp)), [])

    def test_calls_outside_the_image_and_indirect_calls_are_rejected(self):
        functions = self.functions([RelocationFact(0x22, 'PCREL21B', 'memcpy', 0)])
        self.assertTrue(physical.writer_problems((self.WRITER,), functions))
        functions = self.functions()
        functions[self.WRITER] = physical.FunctionFacts((), (('indirect', None, 0),))
        self.assertTrue(physical.writer_problems((self.WRITER,), functions))


if __name__ == '__main__':
    unittest.main()
