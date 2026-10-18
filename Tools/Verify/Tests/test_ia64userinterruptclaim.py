# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the PL3 interrupt claim checks
(TargetChecks/Ia64UserInterruptClaim.py) on a synthetic claim; they need no
build tree.
"""
from __future__ import annotations

import re
import unittest

from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.TargetChecks import Ia64ExternalInterrupt as K
from AsteriaVerify.TargetChecks import Ia64UserInterruptClaim as Claim

VALUES = Claim.Constants(timer_vector=0xE8, masked_timer_vector=0x100E8, selector=5, vector_offset=0x3000,
                         first_device_vector=0x40, device_vector_count=16)


def assemble(*lines: str) -> Code:
    """One instruction per bundle at 16 * index; `text;;` ends a group,
    `(pN) text` is predicated, a `label:` line names the next instruction
    and `<@label>` is its offset."""
    labels, texts = {}, []
    for line in lines:
        if line.endswith(':'):
            labels[line[:-1]] = len(texts)
        else:
            texts.append(line)
    instructions = []
    for index, text in enumerate(texts):
        text = re.sub(r'<@(\w+)>', lambda match: '<+%#x>' % (16 * labels[match.group(1)]), text)
        stop = text.endswith(';;')
        body = text[:-2] if stop else text
        predicate = None
        if body.startswith('('):
            predicate, body = body[1:].split(') ', 1)
        instructions.append(Instruction(16 * index, body, 'MII', 0, stop, predicate))
    return Code(0, tuple(instructions))


# The claim as Ia64UserFaultEntry.S writes it.
CLAIM = (
    'mov r31=pr;;', 'mov r20=cr.ivr;;', 'srlz.d;;',
    'mov r18=15;;', 'cmp.eq p6,p7=r20,r18;;', '(p06) br.cond.dpnt.few <@spurious>;;',
    'mov r18=232;;', 'cmp.ne p6,p7=r20,r18;;', '(p06) br.cond.dpnt.few <@device>;;',
    'mov r19=cr.itm', 'mov.m r18=ar.itc;;',
    'cmp.eq p6,p7=r19,r0;;', '(p06) br.cond.dpnt.few <@quiesce>;;',
    'movl r17=0xffffffffffffffff;;', 'cmp.eq p6,p7=r19,r17;;', '(p06) br.cond.dpnt.few <@quiesce>;;',
    'cmp.ltu p6,p7=r18,r19;;', '(p06) br.cond.dpnt.few <@stale>;;',
    'movl r16=0x100e8;;', 'mov cr.itv=r16;;', 'srlz.d;;', 'mov r17=cr.itv;;',
    'cmp.eq p7,p6=r17,r16;;', '(p06) br.cond.dpnt.few <@claimed>;;',
    'mov cr.eoi=r0;;', 'srlz.d;;', 'mov r18=12288;;', 'mov.m ar.k5=r18;;',
    'mov pr=r31,0xfffffffffffffffe', 'mov r16=5;;', 'br.few <AsteriaIa64UserInterruptEntry>;;',
    'device:', 'mov r18=64;;', 'cmp.ltu p6,p7=r20,r18;;', '(p06) br.cond.dpnt.few <@quiesce>;;',
    'adds r18=16,r18;;', 'cmp.geu p6,p7=r20,r18;;', '(p06) br.cond.dpnt.few <@quiesce>;;',
    'mov cr.eoi=r0;;', 'srlz.d;;', 'mov r18=12288;;', 'or r18=r18,r20;;', 'mov.m ar.k5=r18;;',
    'mov pr=r31,0xfffffffffffffffe', 'mov r16=5;;', 'br.few <AsteriaIa64UserInterruptEntry>;;',
    'stale:', 'mov cr.eoi=r0;;', 'srlz.d;;', 'mov pr=r31,0xfffffffffffffffe;;', 'rfi;;',
    'spurious:', 'mov pr=r31,0xfffffffffffffffe;;', 'rfi;;',
    'quiesce:', 'movl r17=0x100e8;;', 'mov cr.itv=r17;;', 'srlz.d;;', 'mov r18=cr.itv;;',
    'cmp.eq p7,p6=r18,r17;;', '(p06) br.cond.dpnt.few <@unmasked>;;',
    'claimed:', 'mov cr.eoi=r0;;', 'srlz.d;;',
    'stop:', 'hint.m 0;;', 'br.few <@stop>;;',
    'unmasked:', 'mov cr.eoi=r0;;', 'srlz.d;;',
    'terminal:', 'hint.m 0;;', 'br.few <@terminal>;;',
)


class PathTests(unittest.TestCase):
    def test_paths_end_in_the_entries_two_rfis_or_fail_stops(self):
        facts = assemble(*CLAIM)
        ends = [path.end for path in Claim.paths(facts, K.plain_trace(facts))]
        self.assertEqual((ends.count('exit'), ends.count('rfi'), ends.count('fail-stop')), (2, 2, 9))
        self.assertEqual(len(ends), 13)

    def test_a_device_vector_carried_without_its_vector_is_found(self):
        facts = assemble(*CLAIM)
        dropped = facts.nop(facts.first('or r18=r18,r20'))
        problems = Claim.Ia64UserInterruptClaimOutcomes().verify((dropped, VALUES))
        self.assertIn('an entry path writes ar.k5 [0x3000], expected once with or(0x3000,cr.ivr)', problems)

    def test_a_signed_timer_vector_is_found(self):
        facts = assemble(*CLAIM)
        signed = facts.replace(facts.first('mov r18=232'), text='mov r18=-24')
        problems = Claim.Ia64UserInterruptClaimOutcomes().verify((signed, VALUES))
        self.assertIn('0 entry paths hold cr.ivr == 0xe8, expected one', problems)

    def test_an_unguarded_readback_is_found(self):
        facts = assemble(*CLAIM)
        guard = facts.first('mov r17=cr.itv', 'cmp.eq p7,p6=r17,r16')
        problems = Claim.Ia64UserInterruptClaimOutcomes().verify((facts.nop(facts.next(guard)), VALUES))
        self.assertIn('the cr.itv readback compare does not guard the entry path', problems)


class ClaimPropertyTests(unittest.TestCase):
    PROPERTIES = (Claim.Ia64UserInterruptClaimRegisters(), Claim.Ia64UserInterruptClaimSerialization(),
                  Claim.Ia64UserInterruptClaimEoi(), Claim.Ia64UserInterruptClaimExits(),
                  Claim.Ia64UserInterruptClaimOutcomes())

    def test_the_claim_holds_and_every_mutant_is_rejected(self):
        for prop in self.PROPERTIES:
            facts = assemble(*CLAIM)
            if isinstance(prop, Claim._ConstantsProperty):
                facts = (facts, VALUES)
            self.assertEqual(prop.verify(facts), [], prop.name)
            for description, mutant in prop.mutants(facts):
                self.assertNotEqual(mutant, facts, description)
                self.assertTrue(prop.verify(mutant), '%s: %s' % (prop.name, description))


# A miniature dispatcher: the current-PSR guard, the PL3 route, the
# interrupted-PSR guard, the legacy and native continuations and the
# fail-stop.
DISPATCHER = (
    'mov r31=pr', 'mov r30=psr', 'mov r28=cr.ipsr;;',
    'and r17=r30,r16;;', 'cmp.eq p7,p6=r17,r18;;', '(p06) br.cond.dpnt.few <@stop>;;',
    'extr.u r17=r28,32,2;;', 'cmp.eq p6,p7=3,r17;;', '(p06) br.cond.dpnt.few <@user>;;',
    'and r17=r28,r16;;', 'cmp.eq p7,p6=r17,r18;;', '(p06) br.cond.dpnt.few <@stop>;;',
    'cmp.eq p6,p7=r21,r0;;', '(p06) br.cond.dpnt.few <@legacy>;;',
    'br.few <AsteriaIa64ExternalPreemptionNativeEntry>;;',
    'legacy:', 'mov pr=r31,0xfffffffffffffffe;;', 'br.few <AsteriaIa64ExternalInterruptEntry>;;',
    'user:', 'mov pr=r31,0xfffffffffffffffe;;', 'br.few <AsteriaIa64UserInterruptClaim>;;',
    'stop:', 'hint.m 0;;', 'br.few <@stop>;;',
)


class DispatcherTests(unittest.TestCase):
    PROPERTY = K.Ia64ExternalPreemptionDispatchContinuations()

    def test_the_pl3_route_holds_and_every_mutant_is_rejected(self):
        facts = (assemble(*DISPATCHER), False)
        self.assertEqual(self.PROPERTY.verify(facts), [])
        for description, mutant in self.PROPERTY.mutants(facts):
            self.assertNotEqual(mutant, facts, description)
            self.assertTrue(self.PROPERTY.verify(mutant), description)

    def test_a_route_on_the_slot_index_is_found(self):
        facts = assemble(*DISPATCHER)
        slot = facts.replace(facts.first('extr.u r17=r28,32,2'), text='extr.u r17=r28,41,2')
        self.assertEqual(self.PROPERTY.verify((slot, False)),
                         ['the branch to the PL3 continuation (#8) is not taken exactly when IPSR.cpl is 3'])

    def test_a_fall_through_into_the_pl3_restore_is_found(self):
        facts = assemble(*DISPATCHER)
        fallen = facts.nop(facts.first('br.few <AsteriaIa64ExternalInterruptEntry>'))
        self.assertIn('the PL3 continuation is reached from #8, #15, expected from one branch only',
                      self.PROPERTY.verify((fallen, False)))


if __name__ == '__main__':
    unittest.main()
