# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the memory-routine leaf check (TargetChecks/RuntimeMemory.py).

The property's mutants judge facts that are already classified; these tests
cover the classification of x86-64 instructions as objdump renders them in
objects (relocations folded in) and in linked images (normalized targets).
"""
from __future__ import annotations

import unittest

from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.TargetChecks import RuntimeMemory

START = 0x4000_1000


def branches(*texts: str) -> tuple[RuntimeMemory.Branch, ...]:
    """The classified control transfers of one routine made of `texts`, 4 bytes apart."""
    classified = (RuntimeMemory.classify(Instruction(START + 4 * index, text), START)
                  for index, text in enumerate(texts))
    return tuple(branch for branch in classified if branch is not None)


def problems(*texts: str) -> list[str]:
    facts = {routine: branches('ret') for routine in RuntimeMemory.ROUTINES}
    facts['memset'] = branches(*texts)
    return RuntimeMemory.RuntimeMemoryLeaves().verify(facts)


class ClassificationTests(unittest.TestCase):
    def test_a_leaf_loop_passes(self):
        self.assertEqual(problems('test %rdx,%rdx', 'je <+0x20>', 'movb %sil,(%rdi,%rax,1)', 'jne <+0x10>', 'ret'),
                         [])

    def test_the_hosted_self_call_is_rejected(self):
        """GCC's hosted -O2 output of Memory.cpp: memset's loop became `jmp memset`."""
        self.assertIn('jumps back to its own entry', problems('test %rdx,%rdx', 'je <+0x10>', 'jmp <+0x0>', 'ret')[0])

    def test_calls_tail_jumps_and_indirect_branches_are_rejected(self):
        for text, reason in (('call <reloc:R_X86_64_PLT32 memcpy-0x4>', 'calls a function'),
                             ('call <memcpy>', 'calls a function'),
                             ('call *%rax', 'calls a function'),
                             ('jmp <memcpy>', 'leaves its extent'),
                             ('jmp <reloc:R_X86_64_PLT32 memset-0x4>', 'leaves its extent'),
                             ('jmp *%rax', 'branches indirectly'),
                             ('jmp *0x10(%rip)', 'branches indirectly')):
            with self.subTest(text=text):
                self.assertIn(reason, problems(text, 'ret')[0])

    def test_non_branches_are_not_control_transfers(self):
        for text in ('mov %rdi,%rax', 'lea 0x1(%rax),%rax', 'cmp %rax,%rdx', 'nopw 0x0(%rax,%rax,1)'):
            with self.subTest(text=text):
                self.assertIsNone(RuntimeMemory.classify(Instruction(START, text), START))

    def test_a_routine_without_a_return_is_rejected(self):
        self.assertIn('memset never returns', problems('jne <+0x10>'))


if __name__ == '__main__':
    unittest.main()
