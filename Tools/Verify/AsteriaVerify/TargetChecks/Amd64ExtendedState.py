# SPDX-License-Identifier: Apache-2.0
"""AMD64 extended state.

The kernel leaves the x87, MMX and SSE registers to user threads: it saves and
restores them only in the native context switch, and it may not otherwise
read or write them, since a user thread's state stays live in them while the
kernel runs. The kernel is compiled with `-mgeneral-regs-only`; this checks
the result, in the firmware kernel and in the compile-check closure images.
"""
from __future__ import annotations

import dataclasses
import re
import subprocess

from AsteriaVerify import Subjects
from AsteriaVerify.Checks import Property, register

# The save and restore the native context switch may execute, and the symbols
# (substrings of their mangled names) that may contain them.
SAVE_RESTORE = frozenset({'fxsave64', 'fxrstor64'})
SAVE_RESTORE_SYMBOLS: tuple[str,...] = ('Amd64SaveExtendedState', 'Amd64RestoreExtendedState')

_LABEL = re.compile(r'^[0-9a-f]+ <(?P<symbol>[^>]+)>:$')
_INSTRUCTION = re.compile(r'^\s*(?P<address>[0-9a-f]+):\s+(?P<text>\S.*)$')
_REGISTER = re.compile(r'%(?:[xyz]mm\d+|mm\d|st(?:\(\d\))?)\b')
# Instructions that touch extended state without naming one of its registers.
_STATE_MNEMONICS = frozenset({'ldmxcsr', 'stmxcsr', 'vldmxcsr', 'vstmxcsr', 'emms', 'femms'})
_STATE_PREFIXES = ('fx', 'xsave', 'xrstor')


@dataclasses.dataclass(frozen=True)
class KernelInstruction:
    symbol: str
    address: int
    text: str


@dataclasses.dataclass(frozen=True)
class ExtendedStateFacts:
    instructions: tuple[KernelInstruction,...]


def decode(objdump: str, path) -> tuple[KernelInstruction,...]:
    """Every instruction of every executable section, with its symbol."""
    result = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError('%s failed for %s: %s' % (objdump, path, result.stderr.strip()))
    symbol = '?'
    instructions = []
    for line in result.stdout.splitlines():
        label = _LABEL.match(line)
        if label:
            symbol = label.group('symbol')
            continue
        match = _INSTRUCTION.match(line)
        if match:
            instructions.append(KernelInstruction(symbol, int(match.group('address'), 16),
                                                  ' '.join(match.group('text').split())))
    return tuple(instructions)


def touches_extended_state(text: str) -> bool:
    mnemonic = text.split()[0]
    return bool(_REGISTER.search(text)) or (mnemonic in _STATE_MNEMONICS) \
        or mnemonic.startswith(_STATE_PREFIXES) \
        or (mnemonic.startswith('f') and mnemonic not in ('fs', 'fwait'))


def allowed(instruction: KernelInstruction) -> bool:
    return (instruction.text.split()[0] in SAVE_RESTORE) \
        and any(name in instruction.symbol for name in SAVE_RESTORE_SYMBOLS)


@register
class Amd64KernelGeneralRegistersOnly(Property):
    """No AMD64 kernel instruction reads or writes x87, MMX or SSE state
    (registers %st, %mm, %xmm, %ymm, %zmm, MXCSR, or an x87 operation), except
    `fxsave64` and `fxrstor64` in the extended-state save and restore the
    native context switch calls."""

    isas = ('Amd64',)

    def extract(self, context, variant):
        return ExtendedStateFacts(decode(context.tool('objdump'), context.artifact(Subjects.image_key(variant))))

    def verify(self, facts):
        if not facts.instructions:
            return ['the image decodes to no instruction']
        return ['"%s" at %#x in %s touches extended state' % (instruction.text, instruction.address,
                                                               instruction.symbol)
                for instruction in facts.instructions
                if touches_extended_state(instruction.text) and not allowed(instruction)]

    def mutants(self, facts):
        first = facts.instructions[0]
        for text in ('movaps %xmm0,(%rsp)', 'pxor %xmm1,%xmm1', 'movq %mm0,%rax', 'fldz', 'fstp %st(0)',
                     'ldmxcsr (%rax)', 'fxsave64 (%rdi)', 'xsave (%rdi)', 'vmovdqu %ymm0,(%rdi)'):
            yield 'uses extended state: %s' % text, dataclasses.replace(
                facts, instructions=(dataclasses.replace(first, text=text),) + facts.instructions[1:])
        yield 'nothing decoded', dataclasses.replace(facts, instructions=())
