# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the IA-64 user-fault entry checks
(TargetChecks/Ia64UserFaultEntry.py) on synthetic decoded instruction lists;
they need no build tree.
"""
from __future__ import annotations

import unittest

from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.TargetChecks import Ia64UserFaultEntry as Entry


def code(*texts: str) -> Code:
    """Synthetic facts: three slots per bundle; `text;;` carries a stop and
    `(p06) text` a predicate."""
    instructions = []
    for index, text in enumerate(texts):
        stop = text.endswith(';;')
        body = text[:-2] if stop else text
        predicate = None
        if body.startswith('('):
            predicate, body = body[1:].split(') ', 1)
        address = (index // 3) * 16 + 6 * (index % 3)
        instructions.append(Instruction(address, body, 'MII', index % 3, stop, predicate))
    return Code(0, tuple(instructions))


# The final window of the entry: bank-zero carriers loaded from the frame,
# bank-one fills of the same register names, and the control writes.
TAIL = ('rsm 0x2000;;', 'srlz.d;;', 'bsw.0;;',
        'adds r16=24,r4', 'adds r24=64,r4;;', 'nop.i 0x0',
        'ld8 r17=[r24],8;;', 'ld8 r18=[r24];;', 'adds r24=88,r4;;',
        'ld8 r19=[r24];;', 'bsw.1;;', 'ld8.fill r17=[r14],8;;',
        'ld8.fill r18=[r14],8;;', 'ld8.fill r19=[r14];;', 'bsw.0;;',
        'mov cr.ifs=r19;;', 'mov cr.iip=r17', 'mov cr.ipsr=r18;;',
        'rfi;;', 'nop.i 0x0', 'nop.i 0x0')


class BankTests(unittest.TestCase):
    def test_bank_switches_take_effect_after_the_bsw(self):
        facts = code(*TAIL)
        banks = Entry.banks(facts)
        by_text = {step.rendered: banks[step.position] for step in facts.steps}
        self.assertEqual(by_text['adds r24=64,r4'], 0)
        self.assertEqual(by_text['ld8.fill r17=[r14],8'], 1)
        self.assertEqual(by_text['mov cr.iip=r17'], 0)


class NoSlotAdvanceTests(unittest.TestCase):
    PROPERTY = Entry.Ia64UserFaultEntryNoSlotAdvance()

    def test_accepts_the_frame_words_across_the_bank_one_fills(self):
        self.assertEqual(self.PROPERTY.verify(code(*TAIL)), [])

    def test_rejects_an_advanced_slot_and_the_hardware_ifs(self):
        facts = code(*TAIL)
        ipsr = facts.first('mov cr.ipsr=r18')
        self.assertTrue(self.PROPERTY.verify(facts.insert(ipsr, 'dep r18=r0,r18,41,2')))
        hardware = facts.replace(facts.first('adds r24=88,r4'), text='adds r24=80,r4')
        self.assertTrue(self.PROPERTY.verify(hardware))

    def test_every_mutant_is_rejected(self):
        facts = code(*TAIL)
        for description, mutant in self.PROPERTY.mutants(facts):
            self.assertNotEqual(mutant, facts, description)
            self.assertTrue(self.PROPERTY.verify(mutant), description)


class FailStopTests(unittest.TestCase):
    PROPERTY = Entry.Ia64UserFaultEntryFailStop()

    def facts(self, loop_target='<reloc:PCREL21B AsteriaIa64StopUserFault>'):
        return code('cmp.eq p6,p7=r16,r0;;', '(p06) br.cond.dpnt.few <+0x20>;;', 'nop.i 0x0',
                    'rfi;;', 'nop.i 0x0', 'nop.i 0x0',
                    'hint.m 0;;', 'br.many %s;;' % loop_target, 'nop.i 0x0')

    def test_accepts_one_fail_stop_after_rfi(self):
        self.assertEqual(self.PROPERTY.verify(self.facts()), [])

    def test_rejects_an_escaping_loop_and_a_misdirected_rejection(self):
        self.assertTrue(self.PROPERTY.verify(self.facts('<+0x0>')))
        facts = self.facts()
        self.assertTrue(self.PROPERTY.verify(facts.replace(facts.steps[1], text='br.cond.dpnt.few <+0x10>')))


# The Capturing claim and the VectorOffset record of either entry, then the
# bridge call.
def vector_offset(offset: int) -> Code:
    # The exception entry records the offset its trampoline left in ar.k4,
    # the interrupt entry the one its claim left in ar.k5.
    value = {0x5400: ('mov.m r23=ar.k4;;', 'shl r23=r23,8;;'), 0x3000: ('mov.m r23=ar.k5;;',)}.get(
        offset, ('mov r23=%d;;' % offset,))
    return code('adds r23=24,r22', 'mov r18=1;;', 'st8.rel [r23]=r18;;',
                *value, 'adds r18=48,r22;;', 'st8 [r18]=r23;;',
                'br.call.sptk.many b0=<AsteriaIa64UserFaultBridge>;;')


class VectorOffsetTests(unittest.TestCase):
    PROPERTIES = (Entry.Ia64UserFaultEntryVectorOffset(), Entry.Ia64UserInterruptEntryVectorOffset(),
                  Entry.Ia64UserExceptionEntryVectorOffset())

    def test_each_entry_records_only_its_own_vector(self):
        fault, interrupt, exception = self.PROPERTIES
        self.assertEqual(fault.verify(vector_offset(0x1000)), [])
        self.assertEqual(interrupt.verify(vector_offset(0x3000)), [])
        self.assertEqual(exception.verify(vector_offset(0x5400)), [])
        self.assertTrue(fault.verify(vector_offset(0x3000)))
        self.assertTrue(interrupt.verify(vector_offset(0x1000)))
        self.assertTrue(exception.verify(vector_offset(0x1000)))

    def test_every_mutant_is_rejected(self):
        for prop in self.PROPERTIES:
            facts = vector_offset(prop.VARIANT.vector_offset)
            for description, mutant in prop.mutants(facts):
                self.assertNotEqual(mutant, facts, description)
                self.assertTrue(prop.verify(mutant), description)


class VariantTests(unittest.TestCase):
    def test_the_interrupt_twins_judge_the_interrupt_entry(self):
        self.assertEqual(Entry.Ia64UserInterruptEntryAdmission().SYMBOL, 'AsteriaIa64UserInterruptEntry')
        self.assertEqual(Entry.Ia64UserFaultEntryAdmission().SYMBOL, 'AsteriaIa64UserFaultEntry')
        self.assertIn('cmp.eq p7,p6=5,r16', Entry.admission_compares(Entry.INTERRUPT))
        self.assertNotIn('cmp.eq p7,p6=4,r16', Entry.admission_compares(Entry.INTERRUPT))
        self.assertIn('movl r17=0x101308026000', Entry.admission_constants(Entry.INTERRUPT))
        self.assertIn('movl r17=0x101308022000', Entry.admission_constants(Entry.FAULT))

    def test_the_exception_twins_judge_the_exception_entry(self):
        self.assertEqual(Entry.Ia64UserExceptionEntryAdmission().SYMBOL, 'AsteriaIa64UserExceptionEntry')
        self.assertIn('cmp.eq p7,p6=6,r16', Entry.admission_compares(Entry.EXCEPTION))
        self.assertNotIn('cmp.eq p7,p6=4,r16', Entry.admission_compares(Entry.EXCEPTION))
        self.assertIn('movl r17=0x101308022000', Entry.admission_constants(Entry.EXCEPTION))
        self.assertIn('AsteriaIa64UserExceptionEntry', Entry.Ia64UserFaultEntrySymbol.SYMBOLS)


if __name__ == '__main__':
    unittest.main()
