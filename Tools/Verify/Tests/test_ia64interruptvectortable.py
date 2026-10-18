# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the IA-64 interrupt-vector-table, AP-pivot and page-fault
check helpers; they build facts directly and need no build tree.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import dataclasses
import unittest

from AsteriaVerify import Mutate
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.TargetChecks import Ia64ApplicationProcessorPivot as Pivot
from AsteriaVerify.TargetChecks import Ia64InterruptVectorTable as Ivt
from AsteriaVerify.TargetChecks import Ia64PageFaultEntry as PageFault
from Tests.test_library import instructions


def code(*texts: str) -> Code:
    return Code(0, tuple(instructions(*texts)))


class ShapeTests(unittest.TestCase):
    def test_general_registers_are_renamed(self):
        for text, expected in (('mov r16=pr', 'mov r=pr'), ('mov.m r17=ar.k3', 'mov.m r=ar.k3'),
                               ('mov pr=r16,0xfffffffffffffffe', 'mov pr=r,0xfffffffffffffffe'),
                               ('mov cr.ifa=r24', 'mov cr.ifa=r'), ('itc.i r27', 'itc.i r'), ('rfi', 'rfi')):
            self.assertEqual(Ivt.shape(Instruction(0, text)), expected)


class ControlRegisterSerializationTests(unittest.TestCase):
    def test_write_serialize_and_read_in_three_groups(self):
        self.assertEqual(Ivt.control_register_serialization_problems(instructions(
            'mov cr.iva=r32;;', 'srlz.i;;', 'mov r14=cr.iva;;', 'br.ret.sptk.many b0;;')), [])

    def test_srlz_in_the_write_group_does_not_count(self):
        problems = Ivt.control_register_serialization_problems(instructions(
            'mov cr.iva=r32', 'srlz.i;;', 'mov r14=cr.iva;;'))
        self.assertIn('before a srlz in a later group', problems[0])

    def test_readback_in_the_srlz_group(self):
        problems = Ivt.control_register_serialization_problems(instructions(
            'mov cr.iva=r32;;', 'srlz.i', 'mov r14=cr.iva;;'))
        self.assertIn('in the group of its srlz', problems[0])

    def test_branch_before_the_srlz(self):
        problems = Ivt.control_register_serialization_problems(instructions(
            'mov cr.iva=r32;;', 'br.ret.sptk.many b0;;', 'srlz.i;;'))
        self.assertIn('br.ret', problems[0])


def object_slot(**changes) -> Ivt.SlotFacts:
    slot = Ivt.SlotFacts(
        'page not present', 0x5000, 0, 'Entry', 0x5000, None,
        tuple(instructions('nop.m 0x0', 'mov r16=0', 'br.many <reloc:PCREL21B Entry>;;', template='MIB',
                           start=0x5000)),
        (RelocationFact(2, 'PCREL21B', 'Entry', 0),), True, True, None, None)
    return dataclasses.replace(slot, **changes)


def image_slot(entry_address: int, **changes) -> Ivt.SlotFacts:
    slot = Ivt.SlotFacts(
        'break', 0x2c00, None, 'Entry', 0x4000, None,
        tuple(instructions('nop.m 0x0', 'nop.i 0x0', 'br.many <Entry>;;', template='MIB', start=0x4000)),
        (), False, False, entry_address, entry_address)
    return dataclasses.replace(slot, **changes)


class TrampolineTests(unittest.TestCase):
    def test_object_and_image_trampolines(self):
        self.assertEqual(Ivt.trampoline_problems(object_slot()), [])
        self.assertEqual(Ivt.trampoline_problems(image_slot(0x4000 - 0x1000000)), [])

    def test_exception_trampolines_leave_their_vector_in_ar_k4(self):
        floating = object_slot(name='floating-point fault', offset=0x5c00, selector=6, vector=0x5c, instructions=tuple(
            instructions('mov.m ar.k4=92', 'mov r16=6', 'br.many <reloc:PCREL21B Entry>;;', template='MIB',
                         start=0x5c00)))
        self.assertEqual(Ivt.trampoline_problems(floating), [])
        self.assertTrue(Ivt.trampoline_problems(dataclasses.replace(floating, vector=0x5d)))
        self.assertTrue(Ivt.trampoline_problems(dataclasses.replace(floating, vector=None)))

    def test_call_wrong_selector_and_missing_stop(self):
        slot = object_slot().instructions
        for position, changes in ((2, dict(text='br.call.sptk.many b0=<reloc:PCREL21B Entry>')),
                                  (1, dict(text='mov r16=1')), (2, dict(stop_after=False))):
            self.assertTrue(Ivt.trampoline_problems(object_slot(
                instructions=Mutate.replace_at(slot, position, **changes))), changes)

    def test_object_needs_the_pcrel21b_relocation(self):
        self.assertTrue(Ivt.trampoline_problems(object_slot(relocations=())))
        self.assertTrue(Ivt.trampoline_problems(object_slot(relocations=(RelocationFact(2, 'PCREL21B', 'Other', 0),))))

    def test_image_target_and_range(self):
        self.assertTrue(Ivt.trampoline_problems(image_slot(0x8000, resolved_target=0x8010)))
        self.assertTrue(Ivt.trampoline_problems(image_slot(0x4000 + 0x1000000)))
        self.assertTrue(Ivt.trampoline_problems(image_slot(0x4000 - 0x1000010)))


class UserFaultEscapeTests(unittest.TestCase):
    RELOCATIONS = Ivt.Ia64RuntimeIvtRelocations()

    def facts(self, offset: int = 0x13a2) -> tuple[RelocationFact, ...]:
        return tuple(sorted(Ivt.EXPECTED_TABLE_RELOCATIONS + Ivt.STOP_TABLE_RELOCATIONS + (
            RelocationFact(0xf92, "PCREL21B", "AsteriaIa64UserExceptionEntry", 0),
            RelocationFact(0xfa2, "PCREL21B", "AsteriaIa64StopEntry", 0),
            RelocationFact(0x13f2, "PCREL21B", "AsteriaIa64StopEntry", 0),
            RelocationFact(offset, 'PCREL21B', Ivt.USER_FAULT_ENTRY, 0),)))

    def test_one_escape_relocation_inside_the_data_slot(self):
        self.assertEqual(self.RELOCATIONS.verify(self.facts()), [])
        self.assertTrue(self.RELOCATIONS.verify(self.facts(0x0fa2)))
        self.assertTrue(self.RELOCATIONS.verify(self.facts(0x13a1)))
        self.assertTrue(self.RELOCATIONS.verify(Ivt.EXPECTED_TABLE_RELOCATIONS))

    def test_every_relocation_mutant_is_rejected(self):
        facts = self.facts()
        for description, mutant in self.RELOCATIONS.mutants(facts):
            self.assertNotEqual(mutant, facts, description)
            self.assertTrue(self.RELOCATIONS.verify(mutant), description)

    def test_escape_branches(self):
        facts = code('br.many <reloc:PCREL21B AsteriaIa64UserFaultEntry>;;', 'br.few <AsteriaIa64UserFaultEntry>;;',
                     'br.call.sptk.many b0=<reloc:PCREL21B AsteriaIa64UserFaultEntry>;;',
                     'br.many <AsteriaIa64UserFaultEntry+0x10>;;', 'br.many <reloc:PCREL21B AsteriaIa64SystemCallEntry>;;')
        self.assertEqual([Ivt.is_escape(step) for step in facts.steps], [True, True, False, False, False])


class FailStopImageTests(unittest.TestCase):
    def table(self) -> Ivt.FailStopFacts:
        bundles = [Ivt.FAIL_STOP_BUNDLE] * (Ivt.TABLE_SIZE // Ivt.BUNDLE)
        for offset in [0x0c00, 0x0c10, 0x1000] + [t.offset for t in Ivt.TRAMPOLINES] + list(Ivt.STOP_VECTOR_OFFSETS):
            bundles[offset // Ivt.BUNDLE] = bytes(16)
        return Ivt.FailStopFacts(tuple(bundles), (('instruction', 0x0c00, 0x0c20), ('data', 0x1000, 0x1010)))

    def test_populated_table(self):
        self.assertEqual(Ivt.fail_stop_problems(self.table()), [])

    def test_slot_tail_and_missing_code(self):
        facts = self.table()
        for offset, bundle, expected in ((0x0c20, bytes(16), '+0xc20'),
                                         (0x3000, Ivt.FAIL_STOP_BUNDLE, 'external interrupt')):
            bundles = facts.bundles[:offset // Ivt.BUNDLE] + (bundle,) + facts.bundles[offset // Ivt.BUNDLE + 1:]
            self.assertIn(expected, Ivt.fail_stop_problems(dataclasses.replace(facts, bundles=bundles))[0])


LANDING_CHECK = Pivot.Ia64ApTranslatedLanding()


class PivotLandingTests(unittest.TestCase):
    LANDING = Code(0, tuple(instructions(
        'alloc r15=ar.pfs,1,0,0;;', 'mov r32=r14', 'nop.i 0x0;;',
        'nop.m 0x0', 'nop.i 0x0', 'br.call.sptk.many b0=<reloc:PCREL21B %s>;;' % Pivot.CONTINUATION,
        'hint.m 0;;', 'nop.m 0x0', 'nop.i 0x0',
        'nop.m 0x0', 'nop.i 0x0', 'br.few <+0x20>;;')), (RelocationFact(0x12, 'PCREL21B', Pivot.CONTINUATION, 0),))

    def test_landing(self):
        self.assertEqual(LANDING_CHECK.verify(Pivot.LandingFacts(self.LANDING, 'r14')), [])

    def test_wrong_carrier_and_escaping_park(self):
        self.assertTrue(LANDING_CHECK.verify(Pivot.LandingFacts(self.LANDING, 'r15')))
        escaping = self.LANDING.rewrite(11, 'br.few <+0x0>')
        self.assertTrue(LANDING_CHECK.verify(Pivot.LandingFacts(escaping, 'r14')))


CAPTURE, ALLOWLIST = PageFault.Ia64PageFaultEntryCapturePrefix(), PageFault.Ia64PageFaultEntryAllowlist()


class PageFaultEntryTests(unittest.TestCase):
    PREFIX = ('mov r24=r16', 'mov r31=pr', 'mov r30=psr', 'mov r29=cr.iip', 'mov r28=cr.ipsr', 'mov r27=cr.ifs',
              'mov r26=cr.isr', 'mov r25=cr.ifa;;', 'mov.m r23=ar.ccv')

    def test_capture_prefix(self):
        self.assertEqual(CAPTURE.verify(code(*self.PREFIX, 'mov r17=4;;')), [])
        self.assertTrue(CAPTURE.verify(code(*self.PREFIX[:7], 'mov r25=cr.iip;;', self.PREFIX[8])))

    def test_allowlist_forms(self):
        self.assertEqual(ALLOWLIST.verify(code(
            'mov r24=r16', 'mov.m r22=ar.k3', 'adds r16=32,r22;;', 'ld8.acq r20=[r16];;', 'shladd r17=r24,3,r20',
            'cmpxchg8.acq r16=[r21],r19,ar.ccv;;', 'st8.rel [r21]=r16;;', '(p06) br.cond.dpnt.few <+0x0>;;')), [])
        for text in ('rfi', 'alloc r16=ar.pfs,0,0,0', 'mov psr.l=r16', 'st4 [r17]=r16', 'br.ret.sptk.many b0',
                     'mov r15=r16'):
            with self.subTest(text=text):
                self.assertTrue(ALLOWLIST.verify(code(text)))

    def test_context_pointer_loads(self):
        loads = PageFault.context_pointer_loads(code(
            'mov.m r22=ar.k3;;', 'adds r16=8,r22;;', 'ld8 r21=[r16];;', 'adds r16=32,r22;;', 'ld8.acq r20=[r16];;',
            'adds r16=8,r20;;', 'ld8 r19=[r16];;', 'cmp.eq p7,p6=r19,r21;;').steps)
        self.assertEqual([step.text for step in loads], ['ld8.acq r20=[r16]'])


if __name__ == '__main__':
    unittest.main()
