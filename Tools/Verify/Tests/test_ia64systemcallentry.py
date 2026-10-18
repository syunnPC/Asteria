# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the IA-64 system-call entry and user-probe payload checks
(TargetChecks/Ia64SystemCallEntry.py, Ia64UserProbePayload.py) on synthetic
decoded instruction lists; they need no build tree. The shared machinery they
use (Code queries and edits, bundle decoding, header constants) is tested in
test_controlflow.py and test_library.py.
"""
from __future__ import annotations

import dataclasses
import unittest

from AsteriaVerify import Layout
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import SHN_ABS, STB_GLOBAL, STB_LOCAL, STT_FUNC, STV_DEFAULT, STV_HIDDEN, Symbol
from AsteriaVerify.TargetChecks import Ia64SystemCallEntry as Entry
from AsteriaVerify.TargetChecks import Ia64UserProbePayload as Payload


def code(*texts: str, start: int = 0) -> Code:
    """Synthetic facts: three slots per bundle; `text;;` carries a stop and
    `(p06) text` a predicate."""
    instructions = []
    for index, text in enumerate(texts):
        stop = text.endswith(';;')
        body = text[:-2] if stop else text
        predicate = None
        if body.startswith('('):
            predicate, body = body[1:].split(') ', 1)
        address = start + (index // 3) * 16 + 6 * (index % 3)
        instructions.append(Instruction(address, body, 'MII', index % 3, stop, predicate))
    return Code(start, tuple(instructions))


class FailStopTests(unittest.TestCase):
    PROPERTY = Entry.Ia64SystemCallEntryFailStop()

    def facts(self, loop_target='<reloc:PCREL21B AsteriaIa64StopBreak>'):
        return code('cmp.eq p6,p7=r16,r0;;', '(p06) br.cond.dpnt.few <+0x20>;;', 'nop.i 0x0',
                    'br.cond.dpnt.few <+0x20>;;', 'rfi;;', 'nop.i 0x0',
                    'hint.m 0;;', 'br.many %s;;' % loop_target, 'nop.i 0x0')

    def test_accepts_a_single_fail_stop_after_rfi(self):
        self.assertEqual(self.PROPERTY.verify(self.facts()), [])

    def test_rejects_an_escaping_loop_and_a_misdirected_rejection(self):
        self.assertTrue(self.PROPERTY.verify(self.facts('<+0x0>')))
        facts = self.facts()
        self.assertTrue(self.PROPERTY.verify(facts.replace(facts.steps[1], text='br.cond.dpnt.few <+0x0>')))


class GlobalPointerTests(unittest.TestCase):
    PROPERTY = Entry.Ia64SystemCallEntryGlobalPointer()
    GP = 0x4000000000180000
    BRIDGE = 0x4000000000190980

    def facts(self):
        return Entry.GlobalPointerFacts(
            code('srlz.d;;', 'movl r1=<__gp>;;', 'br.call.sptk.many b0=<AsteriaIa64SystemCallBridge>;;'),
            {(6, 1): self.GP, (12, 2): self.BRIDGE}, ((self.GP, SHN_ABS),), self.BRIDGE)

    def test_accepts_the_final_gp_before_the_call(self):
        self.assertEqual(self.PROPERTY.verify(self.facts()), [])

    def test_every_mutant_is_rejected(self):
        facts = self.facts()
        mutants = list(self.PROPERTY.mutants(facts))
        self.assertGreaterEqual(len(mutants), 9)
        for description, mutant in mutants:
            self.assertNotEqual(mutant, facts, description)
            self.assertTrue(self.PROPERTY.verify(mutant), description)


class PayloadSourceParsingTests(unittest.TestCase):
    def test_header_constants(self):
        text = '''
constexpr Foundation::Size Ia64UserExecutionProbePayloadSizeBytes{0x450U};
constexpr Foundation::Size
    Ia64UserExecutionProbeTerminalLoopOffsetBytes{0x100U};
constexpr Foundation::UInt64 Ia64UserExecutionProbeSentinel{
    0x4153'5445'5249'4133ULL};
'''
        self.assertEqual(Layout.constants(text), {
            'Ia64UserExecutionProbePayloadSizeBytes': 0x450,
            'Ia64UserExecutionProbeTerminalLoopOffsetBytes': 0x100,
            'Ia64UserExecutionProbeSentinel': 0x4153544552494133})

    def test_hosted_bytes(self):
        text = 'alignas(16U) constexpr UInt8 HostedPayloadBytes[\n    Size]{\n    0x0AU, 0x88U,\n    0x10U,\n};\n' \
               'return {.Data = HostedPayloadBytes};'
        self.assertEqual(Payload.hosted_bytes(text), bytes([0x0A, 0x88, 0x10]))
        self.assertIsNone(Payload.hosted_bytes('no array here'))


class ConstantPropagationTests(unittest.TestCase):
    def test_tracks_movl_adds_and_post_increments(self):
        facts = code('movl r16=0x2000000002004200;;', 'adds r17=48,r16;;', 'st8 [r17]=r0;;',
                     'st8 [r16]=r8,8;;', 'mov r20=b0;;', '(p06) mov r21=r0;;', 'st8 [r16]=r9;;')
        values = (0x2000000002004200,) + (None,) * (len(facts.instructions) - 1)
        states = Payload._constants(facts, values)
        self.assertEqual(states[2]['r17'], 0x2000000002004230)          # before st8 [r17]=r0
        self.assertEqual(states[6]['r16'], 0x2000000002004208)          # after one post-increment
        self.assertNotIn('r20', states[6])                              # read of b0 is not a constant
        self.assertNotIn('r21', states[6])                              # predicated write


class PayloadSymbolTests(unittest.TestCase):
    PROPERTY = Payload.Ia64UserProbePayloadSymbols()

    def facts(self):
        def symbol(name, value, size, binding, visibility):
            return (Symbol(name, value, size, STT_FUNC, binding, visibility, 1),)
        header = (('PayloadSizeBytes', 0x450), ('PayloadBundleCount', 69), ('TerminalLoopOffsetBytes', 0x100),
                  ('ChildOffsetBytes', 0x110), ('ChildSizeBytes', 0x340))
        return Payload.SymbolFacts(symbol(Payload.PAYLOAD, 0, 0x450, STB_GLOBAL, STV_HIDDEN),
                                   symbol(Payload.LOOP, 0x100, 0x10, STB_GLOBAL, STV_HIDDEN),
                                   symbol(Payload.CHILD, 0x110, 0x340, STB_LOCAL, STV_DEFAULT), header)

    def test_accepts_the_reviewed_geometry(self):
        self.assertEqual(self.PROPERTY.verify(self.facts()), [])

    def test_every_mutant_is_rejected(self):
        facts = self.facts()
        for description, mutant in self.PROPERTY.mutants(facts):
            self.assertNotEqual(mutant, facts, description)
            self.assertTrue(self.PROPERTY.verify(mutant), description)

    def test_child_may_be_global_hidden_after_moving_to_its_own_object(self):
        facts = self.facts()
        moved = dataclasses.replace(facts, child=(dataclasses.replace(facts.child[0], binding=STB_GLOBAL,
                                                                      visibility=STV_HIDDEN),))
        self.assertEqual(self.PROPERTY.verify(moved), [])

    def test_missing_header_constant(self):
        facts = self.facts()
        header = dict(facts.header)
        header['ChildSizeBytes'] = None
        problems = self.PROPERTY.verify(Payload.SymbolFacts(facts.payload, facts.loop, facts.child,
                                                            tuple(header.items())))
        self.assertEqual(len(problems), 1)
        self.assertIn('ChildSizeBytes', problems[0])


if __name__ == '__main__':
    unittest.main()
