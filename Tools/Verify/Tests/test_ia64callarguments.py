# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the IA-64 call-argument property
(TargetChecks/Ia64CallArguments.py) on synthetic code and sources; no build
tree is needed."""
from __future__ import annotations

import pathlib
import tempfile
import unittest

from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.TargetChecks import Ia64CallArguments as Arguments


def code(*texts: str) -> Code:
    """Three slots per bundle; `text;;` carries a stop and `(p06) text` a predicate."""
    instructions = []
    for index, text in enumerate(texts):
        stop = text.endswith(';;')
        body = text[:-2] if stop else text
        predicate = None
        if body.startswith('('):
            predicate, body = body[1:].split(') ', 1)
        instructions.append(Instruction((index // 3) * 16 + 6 * (index % 3), body, 'MII', index % 3, stop, predicate))
    return Code(0, tuple(instructions))


CALL = 'br.call.sptk.many b0=<reloc:PCREL21B Dispatch>;;'
# The SAL rendezvous shape: alloc r32=ar.pfs,0,3,2,0 (objdump: sof 5, sol 3),
# out0 = r35 and out1 = r36 (written on both predicate paths).
RENDEZVOUS = code('alloc r32=ar.pfs,5,3,0', 'mov r33=b0', 'mov r34=r1;;',
                  'cmp.eq p6,p7=r14,r0;;', '(p06) mov r36=1', '(p07) mov r36=r0;;',
                  'mov r35=cr.lid;;', 'nop.i 0x0', CALL)


class DeclarationTests(unittest.TestCase):
    def test_counts_parameters_of_every_declaration_form(self):
        with tempfile.TemporaryDirectory() as root:
            source = pathlib.Path(root, 'Source')
            source.mkdir()
            (source / 'A.hpp').write_text(
                'extern "C" [[noreturn]] void Dispatch(\n    UInt64 lid,\n    UInt64 state) noexcept;\n'
                'extern "C" ASTERIA_NOINLINE_NOIPA\nUInt64 Bridge(Frame* frame, Context* context)\n    noexcept;\n'
                'extern "C" void Leaf() noexcept;\nextern "C" void Unit(void) noexcept;\n')
            (source / 'A.cpp').write_text(
                'extern "C" [[noreturn, gnu::used]] void\nDispatch(const UInt64 lid, const UInt64 state) noexcept\n{\n}\n')
            Arguments.declarations.cache_clear()
            declared = Arguments.declarations(root)
        self.assertEqual(declared['Dispatch'], (2, 2))
        self.assertEqual(declared['Bridge'], (2,))
        self.assertEqual(declared['Leaf'], (0,))
        self.assertEqual(declared['Unit'], (0,))


class CallArgumentTests(unittest.TestCase):
    PROPERTY = Arguments.Ia64CallArguments()

    def sites(self, facts: Code, parameters=(2,)):
        return Arguments.call_sites(facts, {'Dispatch': parameters})

    def test_accepts_arguments_in_the_output_registers(self):
        sites = self.sites(RENDEZVOUS)
        self.assertEqual(list(sites), ['Dispatch at +0x2c'])
        site = sites['Dispatch at +0x2c']
        self.assertEqual(site.frame, (5, 3))
        self.assertTrue({'r35', 'r36'} <= site.written)
        self.assertEqual(self.PROPERTY.verify(sites), [])

    def test_rejects_a_frame_that_shifts_the_outputs(self):
        shifted = RENDEZVOUS.rewrite(0, 'alloc r32=ar.pfs,6,4,0')     # alloc r32=ar.pfs,0,4,2,0
        self.assertIn('parameter 1 (r37) is not written before the call',
                      ' '.join(self.PROPERTY.verify(self.sites(shifted))))

    def test_rejects_missing_outputs_and_a_missing_alloc(self):
        self.assertTrue(self.PROPERTY.verify(self.sites(RENDEZVOUS.rewrite(0, 'alloc r32=ar.pfs,4,3,0'))))
        self.assertTrue(self.PROPERTY.verify(self.sites(RENDEZVOUS.rewrite(0, 'nop.m 0x0'))))

    def test_only_writes_between_the_governing_alloc_and_the_call_count(self):
        # r32 is written before the second alloc, which governs the call.
        late = code('alloc r32=ar.pfs,1,0,0', 'mov r32=r8;;', 'nop.i 0x0',
                    'alloc r14=ar.pfs,1,0,0;;', 'nop.i 0x0', 'nop.i 0x0', 'nop.m 0x0', 'nop.i 0x0', CALL)
        self.assertEqual(self.sites(late, (1,))['Dispatch at +0x2c'].frame, (1, 0))
        self.assertTrue(self.PROPERTY.verify(self.sites(late, (1,))))

    def test_declarations_must_agree_and_mutants_fail(self):
        self.assertTrue(self.PROPERTY.verify(self.sites(RENDEZVOUS, (2, 3))))
        facts = self.sites(RENDEZVOUS)
        mutants = list(self.PROPERTY.mutants(facts))
        self.assertGreaterEqual(len(mutants), 5)
        for description, mutant in mutants:
            self.assertTrue(self.PROPERTY.verify(mutant), description)

    def test_calls_to_undeclared_code_are_ignored(self):
        self.assertEqual(Arguments.call_sites(RENDEZVOUS, {}), {})


if __name__ == '__main__':
    unittest.main()
