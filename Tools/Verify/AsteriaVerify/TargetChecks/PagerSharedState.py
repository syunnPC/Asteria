# SPDX-License-Identifier: Apache-2.0
"""The pager's assembly counter publishes an atomic increment on both ISAs.

C++ observes the naturally aligned UInt64 with an acquire load. Every loop
iteration must perform one unpredicated, 64-bit release read-modify-write at
the incoming argument, preserve that argument, and branch back to the update.
No separate load/store may replace the atomic update. The goldens
pin the instruction encoding; these properties reject broken ordering and
atomicity even after a golden update, on the O0/O2, production and guest
linked pager images.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Disassembly
from AsteriaVerify.Checks import Property, register


@dataclasses.dataclass(frozen=True)
class CounterFacts:
    isa: str
    instructions: tuple[str, ...]


@register
class PagerAtomicSpinningCounter(Property):
    variants = ('firmware', 'firmware-test', 'O0', 'O2')

    def subjects(self, context):
        return [variant + ':pager_elf' for variant in self.variants
                if context.has_variant(variant) and context.has_artifact(variant + ':pager_elf')]

    def extract(self, context, subject):
        symbol = 'Asteria%sPagerSpinningThreadEntry' % context.isa
        decoded = Disassembly.disassemble(context.tool('objdump'), str(context.artifact(subject)), symbol=symbol)
        if context.isa == 'Amd64':
            instructions = tuple(Disassembly.normalize_x86(decoded.instructions))
        else:
            instructions = tuple(step.rendered() for step in decoded.instructions if not step.is_nop)
        return CounterFacts(context.isa, instructions)

    def verify(self, facts):
        code = facts.instructions
        if facts.isa == 'Amd64':
            updates = [index for index, text in enumerate(code)
                       if text in ('lock incq (%rdi)', 'lock addq $0x1,(%rdi)')]
            branch, pause = 'jmp <+0x0>', 'pause'
        else:
            updates = [index for index, text in enumerate(code)
                       if re.fullmatch(r'fetchadd8\.rel r\d+=\[r4\],1', text)
                       and not text.startswith('fetchadd8.rel r4=')]
            branch, pause = 'br.few <+0x0>', 'hint.m 0'
        problems = []
        if len(updates) != 1:
            problems.append('the argument word must receive exactly one unconditional 64-bit release atomic increment')
        elif updates[0] != 0:
            problems.append('the counter update must execute on entry before any loop branch')
        if code.count(branch) != 1 or not code or code[-1] != branch:
            problems.append('the loop must end with one unconditional branch back to the atomic increment')
        problems.extend('unexpected counter-loop instruction: ' + text
                        for index, text in enumerate(code) if index not in updates and text not in (branch, pause))
        return problems

    def mutants(self, facts):
        code = facts.instructions

        def changed(*instructions):
            return dataclasses.replace(facts, instructions=tuple(instructions) + code[1:])

        if facts.isa == 'Amd64':
            yield 'lost LOCK prefix', changed('incq (%rdi)')
            yield 'plain load/add/store', changed('mov (%rdi),%rax', 'add $0x1,%rax', 'mov %rax,(%rdi)')
            yield '32-bit update', changed('lock incl (%rdi)')
            yield 'different argument address', changed('lock incq (%rsi)')
            skip, conditional = 'jmp <+0x4>', 'je <+0x0>'
        else:
            yield 'lost release completer', changed('fetchadd8 r14=[r4],1')
            yield 'acquire without release', changed('fetchadd8.acq r14=[r4],1')
            yield 'plain load/add/store', changed('ld8 r14=[r4]', 'adds r14=1,r14', 'st8 [r4]=r14')
            yield 'predicated increment', changed('(p06) fetchadd8.rel r14=[r4],1')
            yield '32-bit update', changed('fetchadd4.rel r14=[r4],1')
            yield 'different argument address', changed('fetchadd8.rel r14=[r5],1')
            yield 'clobbered argument', changed('fetchadd8.rel r4=[r4],1')
            skip, conditional = 'br.few <+0x10>', '(p06) br.few <+0x0>'
        yield 'missing update', dataclasses.replace(facts, instructions=code[1:])
        yield 'duplicate update', changed(code[0], code[0])
        yield 'loop skips the update', dataclasses.replace(facts, instructions=code[:-1] + (skip,))
        yield 'conditional loop branch', dataclasses.replace(facts, instructions=code[:-1] + (conditional,))
