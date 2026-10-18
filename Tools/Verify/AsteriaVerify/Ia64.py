# SPDX-License-Identifier: Apache-2.0
"""IA-64 instruction operand model for verification properties.

`parse` turns a decoded `Disassembly.Instruction` into an `Operation` that
names the registers the instruction writes and reads, its memory access, and
its branch or relocation target, using objdump's canonical spelling.
Properties reason about these sets instead of matching instruction text, so
they do not depend on register allocation or operand order unless the
property itself is about them.

Register names are canonical strings: `r0`..`r127`, `f0`..`f127`,
`b0`..`b7`, `p0`..`p63` (objdump's two-digit predicate prefix `(p06)` is
canonicalized to `p6`), `ar.<name>`, `cr.<name>`, `psr` (a partial write
such as `psr.l` also counts as a write of `psr`), `pr` for the whole
predicate file, and indexed resources such as `rr[r14]` or `dtr[r15]`
(writing an indexed resource reads its index register). Pseudo-resources:
`tlb` for translation inserts and purges, `rse` for register-stack control.

The bundle decoders at the end read values that the rendered text does not
carry: `movl` immediates and IP-relative branch targets in linked images.
"""
from __future__ import annotations

import dataclasses
import functools
import re

from AsteriaVerify.Disassembly import Instruction

# Register classes (software conventions and bank-0 interruption state).
SCRATCH_GENERAL = frozenset({'r2', 'r3', 'r8', 'r9', 'r10', 'r11'} | {'r%d' % n for n in range(14, 32)})
BANKED_GENERAL = frozenset('r%d' % n for n in range(16, 32))
STACKED_GENERAL = frozenset('r%d' % n for n in range(32, 128))
GLOBAL_POINTER = 'r1'
STACK_POINTER = 'r12'
THREAD_POINTER = 'r13'

# PSR bits (IA-64 SDM): interruption collection, external-interrupt enable,
# data address translation.
PSR_IC, PSR_I, PSR_DT = 1 << 13, 1 << 14, 1 << 17
SERIALIZE_MNEMONICS = frozenset({'srlz.d', 'srlz.i'})
LOOP_COMPLETERS = frozenset({'cloop', 'ctop', 'cexit', 'wtop', 'wexit'})
RSE_APPLICATION_REGISTERS = frozenset({'ar.rsc', 'ar.bsp', 'ar.bspstore', 'ar.rnat'})
ATOMIC_BASES = frozenset({'cmpxchg1', 'cmpxchg2', 'cmpxchg4', 'cmpxchg8', 'cmp8xchg16',
                          'xchg1', 'xchg2', 'xchg4', 'xchg8', 'fetchadd4', 'fetchadd8'})

_REGISTER = re.compile(
    r'^(?:r(?:\d|[1-9]\d|1[01]\d|12[0-7])|f\d+|b[0-7]|p\d+|ar\.[a-z0-9]+|cr\.[a-z0-9]+|'
    r'psr(?:\.[a-z]+)?|pr(?:\.rot)?|ip)$')
_INDEXED = re.compile(r'^(?P<file>rr|pkr|dbr|ibr|pmc|pmd|cpuid|msr|dtr|itr)\[(?P<index>r\d+)\]$')
_TARGET = re.compile(r'<(?P<body>[^>]*)>')
_GENERAL = re.compile(r'r\d+')
# `tbit.z clear,set=source,bit`: `set` holds when the bit is set.
TBIT = re.compile(r'^tbit\.z (?P<clear>p\d+),(?P<set>p\d+)=(?P<source>r\d+),(?P<bit>\d+)$')


@dataclasses.dataclass(frozen=True)
class Target:
    """A direct branch target or relocated operand as rendered by Disassembly.

    kind is `internal` (offset from the start of the decoded range),
    `symbol` (a symbol plus offset outside the range) or `reloc` (a
    relocation of type `relocation` against `symbol` plus `offset`).
    """
    kind: str
    symbol: str | None = None
    offset: int = 0
    relocation: str | None = None


@dataclasses.dataclass(frozen=True)
class Operation:
    mnemonic: str
    completers: tuple[str, ...]
    predicate: str | None
    writes: frozenset[str]
    reads: frozenset[str]
    memory: str | None          # 'load', 'store', 'atomic', 'prefetch' or None
    memory_base: str | None     # base register of the memory operand
    immediates: tuple[int, ...]
    target: Target | None
    text: str

    @property
    def base(self) -> str:
        """The mnemonic without completers, e.g. `ld8` for `ld8.acq`."""
        return self.mnemonic.split('.', 1)[0]

    @property
    def is_nop(self) -> bool:
        return self.base == 'nop'

    @property
    def is_branch(self) -> bool:
        return self.base in ('br', 'brl')

    @property
    def is_call(self) -> bool:
        return self.is_branch and 'call' in self.completers

    @property
    def is_return(self) -> bool:
        return self.is_branch and 'ret' in self.completers

    @property
    def is_direct_branch(self) -> bool:
        return self.is_branch and self.target is not None

    @property
    def is_indirect_branch(self) -> bool:
        return self.is_branch and self.target is None

    @property
    def is_predicated(self) -> bool:
        return self.predicate not in (None, 'p0')

    @property
    def accesses_memory(self) -> bool:
        return self.memory in ('load', 'store', 'atomic')

    @property
    def general_registers(self) -> frozenset[str]:
        return self.general_writes | self.general_reads

    @property
    def general_writes(self) -> frozenset[str]:
        return frozenset(r for r in self.writes if _GENERAL.fullmatch(r))

    @property
    def general_reads(self) -> frozenset[str]:
        return frozenset(r for r in self.reads if _GENERAL.fullmatch(r))

    @property
    def is_conditional_branch(self) -> bool:
        """A branch that may fall through: predicated or a loop branch."""
        return self.is_branch and (self.is_predicated or bool(LOOP_COMPLETERS & set(self.completers)))

    def writes_predicate(self, predicate: str) -> bool:
        return predicate in self.writes or 'pr' in self.writes or 'pr.rot' in self.writes


def canonical_predicate(predicate: str | None) -> str | None:
    if predicate is None:
        return None
    return 'p%d' % int(predicate[1:])


def split_operands(operands: str) -> list[str]:
    """Splits on commas outside brackets and angle brackets."""
    parts, depth, current = [], 0, []
    for char in operands:
        if char in '[<':
            depth += 1
        elif char in ']>':
            depth -= 1
        if char == ',' and depth == 0:
            parts.append(''.join(current).strip())
            current = []
        else:
            current.append(char)
    parts.append(''.join(current).strip())
    return [part for part in parts if part]


def parse_target(text: str) -> Target | None:
    match = _TARGET.search(text)
    if not match:
        return None
    body = match.group('body')
    if body.startswith('reloc:'):
        kind, _, rest = body[len('reloc:'):].partition(' ')
        symbol, offset = _symbol_offset(rest)
        return Target('reloc', symbol, offset, kind)
    if body.startswith('+'):
        return Target('internal', None, int(body[1:], 0))
    symbol, offset = _symbol_offset(body)
    return Target('symbol', symbol, offset)


def _symbol_offset(text: str) -> tuple[str, int]:
    match = re.fullmatch(r'(?P<symbol>.+?)(?P<offset>[+-]0x[0-9a-f]+)?', text.strip())
    if not match:
        return text.strip(), 0
    offset = int(match.group('offset'), 16) if match.group('offset') else 0
    return match.group('symbol'), offset


def _immediate(token: str) -> int | None:
    try:
        return int(token, 0)
    except ValueError:
        return None


def split(text: str) -> tuple[str, list[str], list[str]]:
    """(mnemonic, destination tokens, source tokens) of an instruction text."""
    mnemonic, _, rest = text.partition(' ')
    depth = 0
    for position, char in enumerate(rest):
        if char in '<[':
            depth += 1
        elif char in '>]':
            depth -= 1
        elif char == '=' and depth == 0:
            return mnemonic, split_operands(rest[:position]), split_operands(rest[position + 1:])
    return mnemonic, [], split_operands(rest)


def render(mnemonic: str, destinations: list[str], sources: list[str]) -> str:
    """The inverse of `split`."""
    if destinations:
        return '%s %s=%s' % (mnemonic, ','.join(destinations), ','.join(sources))
    return mnemonic + (' ' + ','.join(sources) if sources else '')


@functools.lru_cache(maxsize=None)
def parse(instruction: Instruction) -> Operation:
    """Parses one decoded IA-64 instruction (cached: mutants share almost
    every instruction with the real facts)."""
    mnemonic, _, operands = instruction.text.partition(' ')
    completers = tuple(mnemonic.split('.')[1:])
    base = mnemonic.split('.', 1)[0]
    target = parse_target(operands)
    if target is not None:
        operands = _TARGET.sub('<target>', operands)

    writes: set[str] = set()
    reads: set[str] = set()
    immediates: list[int] = []
    memory_base: str | None = None

    if '=' in operands:
        left, right = operands.split('=', 1)
        destinations, sources = split_operands(left), split_operands(right)
    else:
        destinations, sources = [], split_operands(operands)

    def operand(token: str, written: bool) -> None:
        nonlocal memory_base
        if token.startswith('[') and token.endswith(']'):
            memory_base = token[1:-1]
            reads.add(memory_base)
            return
        indexed = _INDEXED.match(token)
        if indexed:
            reads.add(indexed.group('index'))
            (writes if written else reads).add(token)
            return
        if _REGISTER.match(token):
            (writes if written else reads).add(token)
            if written and token.startswith('psr.'):
                writes.add('psr')
            return
        if not written:
            value = _immediate(token)
            if value is not None:
                immediates.append(value)

    for token in destinations:
        operand(token, True)
    for token in sources:
        operand(token, False)

    memory = None
    if base.startswith('ld'):
        memory = 'load'
    elif base.startswith('st'):
        memory = 'store'
    elif base == 'lfetch':
        memory = 'prefetch'
    elif base in ATOMIC_BASES:
        memory = 'atomic'
    if memory in ('load', 'store', 'prefetch') and len(sources) > 1 and memory_base is not None:
        writes.add(memory_base)              # post-increment base update

    if base in ('br', 'brl'):
        if 'call' in completers:
            writes.add('ar.pfs')
        if 'cloop' in completers or 'ctop' in completers or 'cexit' in completers:
            reads.add('ar.lc')
            writes.add('ar.lc')
    elif base in ('rsm', 'ssm'):
        writes.add('psr')
    elif base in ('itc', 'itr', 'ptc', 'ptr'):
        writes.add('tlb')
    elif base in ('alloc', 'flushrs', 'loadrs', 'cover'):
        writes.add('rse')
    elif base == 'rfi':
        writes.update(('psr', 'ip'))

    predicate = canonical_predicate(instruction.predicate)
    if predicate is not None:
        reads.add(predicate)
    return Operation(mnemonic, completers, predicate, frozenset(writes), frozenset(reads), memory,
                     memory_base, tuple(immediates), target, instruction.rendered())


def alloc_frame(operation: Operation) -> tuple[int, int, int]:
    """(size of frame, size of locals, size of rotating) of an `alloc`.

    objdump prints `alloc rX=ar.pfs,sof,sol,sor`; the assembler source form is
    `alloc rX=ar.pfs,i,l,o,r` with sof = i + l + o and sol = i + l.
    """
    if operation.base != 'alloc' or len(operation.immediates) != 3:
        raise ValueError('not an alloc: %s' % operation.text)
    sof, sol, sor = operation.immediates
    return sof, sol, sor


def single_write(operation: Operation) -> str | None:
    """The one general register an operation writes, or None."""
    return next(iter(operation.general_writes)) if len(operation.general_writes) == 1 else None


def single_read(operation: Operation) -> str | None:
    """The one general register other than r0 an operation reads, or None."""
    reads = operation.general_reads - {'r0'}
    return next(iter(reads)) if len(reads) == 1 else None


def is_serialization_write(operation: Operation) -> bool:
    """A write whose effect is guaranteed only after an explicit srlz.

    Covers PSR writes (`rsm`, `ssm`, `mov psr.l=`), control-register writes,
    region/protection-key/debug register writes, and translation inserts and
    purges.
    """
    if 'psr' in operation.writes or 'tlb' in operation.writes:
        return True
    return any(w.startswith(('cr.', 'rr[', 'pkr[', 'dbr[', 'ibr[')) for w in operation.writes)


def is_serialize(operation: Operation) -> bool:
    return operation.mnemonic in SERIALIZE_MNEMONICS


def touches_rse(operation: Operation) -> bool:
    return 'rse' in operation.writes or bool((operation.writes | operation.reads) & RSE_APPLICATION_REGISTERS)


# Bundle decoding --------------------------------------------------------------

_SLOT_MASK = (1 << 41) - 1


def bundle_slots(raw: bytes) -> tuple[int, int, int, int]:
    """(template, slot 0, slot 1, slot 2) of a 16-byte little-endian bundle."""
    if len(raw) != 16:
        raise ValueError('a bundle has 16 bytes, not %d' % len(raw))
    value = int.from_bytes(raw, 'little')
    return value & 0x1F, (value >> 5) & _SLOT_MASK, (value >> 46) & _SLOT_MASK, (value >> 87) & _SLOT_MASK


def movl_immediate(raw: bytes) -> int:
    """The 64-bit immediate of the `movl` (X2) in an MLX bundle."""
    _, _, imm41, x = bundle_slots(raw)
    return (((x >> 36) & 1) << 63 | imm41 << 22 | ((x >> 21) & 1) << 21 | ((x >> 22) & 0x1F) << 16
            | ((x >> 27) & 0x1FF) << 7 | ((x >> 13) & 0x7F))


def branch_target(raw: bytes, slot: int, bundle_address: int) -> int | None:
    """The target of the IP-relative branch or call in `slot` (B-unit
    opcodes 4 and 5, B1/B3: imm21 = s:imm20b counted in bundles), or None
    when the slot holds another instruction."""
    instruction = bundle_slots(raw)[1 + slot]
    if (instruction >> 37) & 0xF not in (4, 5):
        return None
    displacement = ((instruction >> 13) & 0xFFFFF) | (((instruction >> 36) & 1) << 20)
    if displacement & (1 << 20):
        displacement -= 1 << 21
    return (bundle_address + (displacement << 4)) & ((1 << 64) - 1)
