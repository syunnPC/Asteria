# SPDX-License-Identifier: Apache-2.0
"""The freestanding memory routines of every AMD64 image (Source/Runtime/Memory.cpp).

`memcpy`, `memmove`, `memset` and `memcmp` are the C ABI routines that close
compiler-generated copies and fills in the kernel and in the initial user
image. GCC may turn a byte loop into a call of the library routine it
recognizes: Memory.cpp compiled as a hosted translation unit at -O2 makes
`memset`'s loop a `jmp memset`, an endless self-call. The kernel and the user
image are therefore compiled with -fno-builtin (and -ffreestanding), and this
property proves on every build that the four routines of every image stay
leaves: no call, no jump to their own entry or out of their extent, and no
indirect branch.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Disassembly
from AsteriaVerify.Checks import Property, register

ROUTINES = ('memcpy', 'memmove', 'memset', 'memcmp')
# The images that link Memory.cpp: both firmwares' kernel and user image and
# the O0/O2 compile-check nucleus closure and user image.
IMAGES = ('firmware:kernel_elf', 'firmware:user_elf', 'firmware-test:kernel_elf', 'firmware-test:user_elf',
          'O0:nucleus_elf', 'O0:user_elf', 'O2:nucleus_elf', 'O2:user_elf')
_INTERNAL = re.compile(r'<\+(?P<offset>0x[0-9a-f]+)>$')


@dataclasses.dataclass(frozen=True)
class Branch:
    """One control transfer of a routine: its mnemonic and its target, an
    offset inside the routine or None for any other target (text keeps it)."""
    offset: int
    mnemonic: str
    target: int | None
    text: str


def classify(instruction: Disassembly.Instruction, start: int) -> Branch | None:
    """The control transfer an x86-64 instruction performs, or None."""
    mnemonic = instruction.mnemonic
    if not (mnemonic.startswith(('j', 'call', 'loop')) or mnemonic in ('ret', 'retq')):
        return None
    match = _INTERNAL.search(instruction.text)
    target = int(match.group('offset'), 16) if match and '*' not in instruction.text else None
    return Branch(instruction.address - start, mnemonic, target, instruction.text)


def branch_problems(branch: Branch) -> list[str]:
    where = '%s at +%#x' % (branch.text, branch.offset)
    if branch.mnemonic in ('ret', 'retq'):
        return []
    if branch.mnemonic.startswith('call'):
        return ['calls a function: %s' % where]
    if branch.target is None:
        return ['leaves its extent or branches indirectly: %s' % where]
    if branch.target == 0:
        return ['jumps back to its own entry, a call of itself: %s' % where]
    return []


@register
class RuntimeMemoryLeaves(Property):
    """Every image's memcpy, memmove, memset and memcmp are leaf routines:
    each is defined, returns with `ret`, and transfers control only inside
    its own extent and never back to its entry. A call or tail jump into a
    memory routine from its own implementation recurses without end."""

    isas = ('Amd64',)
    variants = ('firmware', 'firmware-test', 'O0', 'O2')

    def subjects(self, context):
        return [key for key in IMAGES if context.has_artifact(key)]

    def extract(self, context, key):
        path = context.artifact(key)
        routines = {}
        for routine in ROUTINES:
            try:
                decoded = Disassembly.disassemble(context.tool('objdump'), path, symbol=routine)
            except Disassembly.DisassemblyError:
                continue
            routines[routine] = tuple(branch for branch in (
                classify(instruction, decoded.start) for instruction in decoded.instructions) if branch is not None)
        return routines

    def verify(self, facts):
        problems = ['%s is not defined' % routine for routine in ROUTINES if routine not in facts]
        for routine, branches in facts.items():
            problems += ['%s %s' % (routine, problem) for branch in branches for problem in branch_problems(branch)]
            if not any(branch.mnemonic in ('ret', 'retq') for branch in branches):
                problems.append('%s never returns' % routine)
        return problems

    def mutants(self, facts):
        yield 'memset jumps to its own entry', _with(facts, 'memset', Branch(0x9, 'jmp', 0, 'jmp <+0x0>'))
        yield 'memmove calls memcpy', _with(facts, 'memmove', Branch(0x10, 'call', None, 'call <memcpy>'))
        yield 'memcpy tail-jumps to memset', _with(facts, 'memcpy', Branch(0x10, 'jmp', None, 'jmp <memset>'))
        yield 'memcmp branches indirectly', _with(facts, 'memcmp', Branch(0x10, 'jmp', None, 'jmp *%rax'))
        yield 'memcpy is missing', {name: branches for name, branches in facts.items() if name != 'memcpy'}
        yield 'memset has no return', {**facts, 'memset': tuple(
            branch for branch in facts['memset'] if branch.mnemonic not in ('ret', 'retq'))}


def _with(facts: dict, routine: str, branch: Branch) -> dict:
    return {**facts, routine: facts[routine] + (branch,)}
