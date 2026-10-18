# SPDX-License-Identifier: Apache-2.0
"""IA-64 physical-access leaves: the diagnostic UART adapter and the MMIO leaves.

Checked code (helpers are located by symbol):

- `Firmware/Diagnostic.S`: the asm leaf
  `AsteriaIa64TryWriteDiagnosticCharacterPhysical`; `Firmware/Diagnostic.cpp`:
  the compiler-generated C++ diagnostic writer (`WriteDiagnosticAscii`,
  `TryWriteCharacter` at O0, `HaltAfterFirmwareExit`).
- `Device/Ia64PortIo.S`: exact-width sparse read/write leaves with `mf.a`.
- `Device/Ia64PhysicalMemoryAccess.S`: the asm leaves
  `AsteriaIa64ReadUncachedPhysicalDoubleWord` and
  `AsteriaIa64WriteUncachedPhysicalDoubleWord`, their quad-word twins (the
  ordered 8-byte VT-d register accessors) and their byte/word variants.

All eleven leaves follow one shape, the physical corridor: capture the stacked
inputs and the PSR, mask interrupts, force the RSE lazy (mode bits only),
turn data translation off, touch the device only through an uncacheable
physical alias (bit 63) with the fences a device needs, and restore PSR.dt,
the exact ar.rsc and PSR.i, each predicated on the entry PSR.

The golden Tests/Golden/Ia64/Firmware/AsteriaIa64TryWriteDiagnosticCharacterPhysical.dis
pins the adapter, including its constants (the uncacheable alias it ORs into
both register addresses, THRE bit 5); the transmit and line status register
addresses are its inputs r34/r35 (Ia64DebugUart, from the firmware's
debug-port tables). Double- and quad-word MMIO properties accept register
renaming, independent capture reordering and harmless scratch arithmetic,
and check every constant. Port leaves have goldens under Tests/Golden/Ia64/Device;
their width predicates, exact transactions and input captures also have
mutation-tested properties.
"""
from __future__ import annotations

import dataclasses
import functools
import re
from collections import Counter

from AsteriaVerify import Ia64, Relocations, Subjects
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.ControlFlow import Code, written
from AsteriaVerify.Elf import SHF_EXECINSTR, STT_FUNC, load
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import HelperExtents, HelperGroup, HelperProperty, HelperSymbols

UART_ADAPTER = 'AsteriaIa64TryWriteDiagnosticCharacterPhysical'
MMIO_READ = 'AsteriaIa64ReadUncachedPhysicalDoubleWord'
MMIO_WRITE = 'AsteriaIa64WriteUncachedPhysicalDoubleWord'
MMIO_READ_QUAD = 'AsteriaIa64ReadUncachedPhysicalQuadWord'
MMIO_WRITE_QUAD = 'AsteriaIa64WriteUncachedPhysicalQuadWord'
PORT_READ = 'AsteriaIa64ReadPhysicalPort'
PORT_WRITE = 'AsteriaIa64WritePhysicalPort'
DIAGNOSTIC_WRITER = '_ZN7Asteria8Platform8Firmware20WriteDiagnosticAsciiEPKc'

DIAGNOSTIC = HelperGroup('Diagnostic', (UART_ADAPTER,))
MMIO = HelperGroup('PhysicalMemoryMappedIo', (MMIO_READ, MMIO_WRITE, MMIO_READ_QUAD, MMIO_WRITE_QUAD))
PORTS = HelperGroup('PortIo', (PORT_READ, PORT_WRITE))
NARROW_MMIO = HelperGroup('NarrowPhysicalMemoryMappedIo', tuple(
    'AsteriaIa64%sUncachedPhysical%s' % (operation, width)
    for width in ('Byte', 'Word') for operation in ('Read', 'Write')))
PHYSICAL_GROUPS = (DIAGNOSTIC, MMIO, PORTS, NARROW_MMIO)

PSR_I_BIT = 14
PSR_DT_BIT = 17
UNCACHED_ALIAS = 1 << 63
MASK64 = (1 << 64) - 1


@register
class Ia64PhysicalAccessSymbols(HelperSymbols):
    GROUPS = PHYSICAL_GROUPS


@register
class Ia64PhysicalAccessExtents(HelperExtents):
    GROUPS = PHYSICAL_GROUPS


# The physical corridor ------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class CorridorSpec:
    """What one leaf may do inside the corridor."""
    accesses: tuple[tuple[str, str],...]    # (memory kind, mnemonic) allowed in the physical interval
    internal_branches: bool                  # Intra-leaf branches (the UART poll loop)


CORRIDOR_SPECS = {
    UART_ADAPTER: CorridorSpec((('load', 'ld1.acq'), ('store', 'st1')), True),
    MMIO_READ: CorridorSpec((('load', 'ld4.acq'),), False),
    MMIO_WRITE: CorridorSpec((('store', 'st4'),), False),
    MMIO_READ_QUAD: CorridorSpec((('load', 'ld8.acq'),), False),
    MMIO_WRITE_QUAD: CorridorSpec((('store', 'st8'),), False),
    PORT_READ: CorridorSpec((('load', 'ld1.acq'), ('load', 'ld2.acq'), ('load', 'ld4.acq')), False),
    PORT_WRITE: CorridorSpec((('store', 'st1'), ('store', 'st2'), ('store', 'st4')), False),
}
CORRIDOR_SPECS.update({
    'AsteriaIa64%sUncachedPhysical%s' % (operation, width):
        CorridorSpec(((kind, mnemonic),), False)
    for width, size in (('Byte', 1), ('Word', 2))
    for operation, kind, mnemonic in (('Read', 'load', 'ld%d.acq' % size), ('Write', 'store', 'st%d' % size))
})
CORRIDOR_FORBIDDEN = frozenset({'alloc', 'cover', 'flushrs', 'loadrs', 'break', 'rfi', 'bsw'})
RESERVED_REGISTERS = frozenset({Ia64.GLOBAL_POINTER, Ia64.STACK_POINTER, Ia64.THREAD_POINTER})
_DEP = re.compile(r'^dep (?P<target>r\d+)=0,(?P<source>r\d+),0,2$')


def _predicates(registers) -> set[str]:
    return {register for register in registers if re.fullmatch(r'p\d+', register)}


def register_value(code: Code, register: str, use: int, depth: int = 0):
    """Abstract value of `register` at position `use`: ('const', value),
    ('bit63') when bit 63 is known to be set, or None when unknown. The
    register must have exactly one, unpredicated, definition in the leaf and
    it must precede the use."""
    if register == 'r0':
        return ('const', 0)
    definitions = [step for step in code.steps if register in step.operation.general_writes]
    if depth > 8 or len(definitions) != 1 or definitions[0].position >= use:
        return None
    operation = definitions[0].operation
    if operation.is_predicated or operation.memory is not None:
        return None
    sources = sorted(operation.general_reads)
    if operation.base in ('movl', 'mov') and not sources and len(operation.immediates) == 1:
        return ('const', operation.immediates[0] & MASK64)
    values = [register_value(code, source, definitions[0].position, depth + 1) for source in sources]
    if operation.base == 'mov' and len(sources) == 1 and not operation.immediates:
        return values[0]
    if operation.base in ('adds', 'addl', 'add') and len(sources) == 1 and len(operation.immediates) == 1:
        if values[0] is not None and values[0][0] == 'const':
            return ('const', (values[0][1] + operation.immediates[0]) & MASK64)
        return None
    if operation.base == 'or' and len(sources) == 2 and not operation.immediates:
        if all(value is not None and value[0] == 'const' for value in values):
            return ('const', values[0][1] | values[1][1])
        if any(is_uncached(value) for value in values):
            return ('bit63',)
    return None


def is_uncached(value) -> bool:
    return value == ('bit63',) or (value is not None and value[0] == 'const' and bool(value[1] & UNCACHED_ALIAS))


def corridor_problems(code: Code, spec: CorridorSpec) -> list[str]:
    """Leaf ABI, RSE, PSR, access and ordering checks for the physical corridor."""
    steps = code.steps
    problems = []

    def text(step):
        return '%s (#%d)' % (step.rendered, step.position)

    # A leaf that needs no stack, RSE, gp or relocation.
    for step in steps:
        operation = step.operation
        if operation.base in CORRIDOR_FORBIDDEN or operation.is_call:
            problems.append('leaf ABI: %s is not allowed in a physical-access leaf' % text(step))
        reserved = sorted((operation.reads | operation.writes) & RESERVED_REGISTERS)
        if reserved:
            problems.append('leaf ABI: %s uses %s' % (text(step), ', '.join(reserved)))
    problems += ['leaf ABI: relocation %s inside the leaf' % (fact,) for fact in code.relocations]

    # One return, in the last bundle; other branches stay inside the leaf.
    returns = [step for step in steps if step.operation.is_return]
    if len(returns) != 1:
        problems.append('return: %d returns, expected exactly one' % len(returns))
    else:
        ret = returns[0]
        if ret.operation.is_predicated or 'b0' not in ret.operation.reads:
            problems.append('return: %s is not an unpredicated return through b0' % text(ret))
        if ret.instruction.address & ~0xF != code.instructions[-1].address & ~0xF or steps[-1] is not ret:
            problems.append('return: the return %s is not the last instruction of the last bundle' % text(ret))
    for step in steps:
        operation = step.operation
        if operation.is_branch and not operation.is_return and not operation.is_call:
            if not spec.internal_branches:
                problems.append('return: %s branches' % text(step))
            elif step.target is None:
                problems.append('return: %s leaves the leaf' % text(step))

    # PSR roles used by the RSE, serialization and memory-access checks.
    def psr_form(step, base, mask):
        return step.operation.base == base and step.operation.immediates == (mask,)

    psr_writes = [step for step in steps if 'psr' in step.operation.writes]
    roles = {
        'disable_i': [step for step in psr_writes if psr_form(step, 'rsm', Ia64.PSR_I)],
        'disable_dt': [step for step in psr_writes if psr_form(step, 'rsm', Ia64.PSR_DT)],
        'restore_dt': [step for step in psr_writes if psr_form(step, 'ssm', Ia64.PSR_DT)],
        'restore_i': [step for step in psr_writes if psr_form(step, 'ssm', Ia64.PSR_I)],
        'capture': [step for step in steps if 'psr' in step.operation.reads],
    }
    problems += ['PSR: %s writes the PSR outside the corridor forms' % text(step) for step in psr_writes
                 if not any(step in found for found in roles.values())]
    problems += ['PSR: %d %s instructions, expected exactly one' % (len(found), key.replace('_', ' '))
                 for key, found in roles.items() if len(found) != 1]
    problems += ['PSR: %s accesses a control register or the predicate file' % text(step) for step in steps
                 if any(register.startswith('cr.') or register in ('pr', 'pr.rot')
                        for register in step.operation.reads | step.operation.writes)]
    single = {key: found[0] for key, found in roles.items() if len(found) == 1}
    complete = len(single) == len(roles)
    if complete:
        capture = single['capture']
        saved = sorted(capture.operation.general_writes)
        problems += ['PSR: %s is predicated' % text(single[key]) for key in ('capture', 'disable_i', 'disable_dt')
                     if single[key].operation.is_predicated]
        if not single['capture'].position < single['disable_i'].position < single['disable_dt'].position:
            problems.append('PSR: the PSR capture, rsm 0x4000 and rsm 0x20000 are out of order')
        if len(saved) != 1 or [step for step in steps if saved[0] in step.operation.general_writes] != [capture]:
            problems.append('PSR: the PSR capture register is not defined exactly once')
        else:
            for key, bit in (('restore_dt', PSR_DT_BIT), ('restore_i', PSR_I_BIT)):
                tests = [step for step in steps if (match := Ia64.TBIT.match(step.text)) and not
                         step.instruction.predicate and match.group('source') == saved[0]
                         and int(match.group('bit')) == bit]
                if len(tests) != 1:
                    problems.append('PSR: %d tbit.z tests of entry PSR bit %d, expected exactly one' % (len(tests), bit))
                    continue
                test = Ia64.TBIT.match(tests[0].text)
                restore = single[key]
                if restore.predicate != Ia64.canonical_predicate(test.group('set')):
                    problems.append('PSR: %s is not predicated on entry PSR bit %d being set (%s)'
                                    % (text(restore), bit, test.group('set')))
                if not tests[0].position < restore.position or any(
                        step.operation.writes_predicate(Ia64.canonical_predicate(test.group('set')))
                        for step in code.window(after=tests[0], before=restore)):
                    problems.append('PSR: the PSR bit %d predicate does not reach %s' % (bit, text(restore)))
        if not single['restore_dt'].position < single['restore_i'].position:
            problems.append('PSR: PSR.i is restored before PSR.dt')

    # Every PSR write is serialized by srlz.d in a later group, with no
    # memory access, ar.rsc write or other PSR write before it.
    problems += ['serialization: %s is not an unpredicated srlz.d' % text(step) for step in steps if step.operation.base == 'srlz'
                 and (step.mnemonic != 'srlz.d' or step.operation.is_predicated)]
    serializations = {}
    for write in psr_writes:
        serialize = code.first(lambda step: step is write, lambda step: step.operation.base == 'srlz')
        if serialize is None:
            problems.append('serialization: %s is never serialized' % text(write))
            continue
        serializations[write.position] = serialize
        if serialize.group <= write.group:
            problems.append('serialization: %s and its serialization share an instruction group' % text(write))
        problems += ['serialization: %s runs between %s and its srlz' % (text(between), text(write))
                     for between in code.window(after=write, before=serialize)
                     if between.operation.memory is not None or 'ar.rsc' in between.operation.writes
                     or 'psr' in between.operation.writes]

    # The RSE is forced lazy with the mode bits only and restored exactly.
    for step in steps:
        foreign = sorted(register for register in step.operation.reads | step.operation.writes
                         if register.startswith('ar.') and register != 'ar.rsc')
        if foreign:
            problems.append('RSE: %s uses %s' % (text(step), ', '.join(foreign)))
    saves = [step for step in steps if 'ar.rsc' in step.operation.reads]
    deps = [step for step in steps if step.operation.base == 'dep']
    rsc_writes = [step for step in steps if 'ar.rsc' in step.operation.writes]
    if len(saves) != 1 or len(deps) != 1 or len(rsc_writes) != 2:
        problems.append('RSE: %d ar.rsc reads, %d dep, %d ar.rsc writes; expected 1, 1 and 2'
                        % (len(saves), len(deps), len(rsc_writes)))
    else:
        saved = sorted(saves[0].operation.general_writes)
        lazy = _DEP.match(deps[0].text)
        first, second = rsc_writes
        if any(step.operation.is_predicated for step in (saves[0], deps[0], first, second)):
            problems.append('RSE: an ar.rsc save, dep or write is predicated')
        if len(saved) != 1 or lazy is None or lazy.group('source') != saved[0]:
            problems.append('RSE: %s does not clear only the mode bits of the saved ar.rsc' % text(deps[0]))
        elif sorted(first.operation.general_reads) != [lazy.group('target')] \
                or sorted(second.operation.general_reads) != saved:
            problems.append('RSE: the first ar.rsc write must install the lazy value and the second restore '
                            'the saved value')
        if not saves[0].position < deps[0].position < first.position < second.position:
            problems.append('RSE: the ar.rsc save, dep and writes are out of order')
        if complete:
            if not first.position < single['disable_dt'].position:
                problems.append('RSE: the RSE is not lazy before rsm 0x20000')
            restore_serialized = serializations.get(single['restore_dt'].position)
            if restore_serialized is None or not (restore_serialized.position < second.position
                                                  < single['restore_i'].position):
                problems.append('RSE: ar.rsc is not restored between the PSR.dt restore and the PSR.i restore')

    # Memory is touched only inside the physical interval, only by the
    # allowlisted accesses through an uncacheable alias, stores fenced.
    if complete and single['disable_dt'].position in serializations:
        start, end = serializations[single['disable_dt'].position].position, single['restore_dt'].position
        for step in steps:
            operation = step.operation
            if operation.memory is None:
                continue
            if not start < step.position < end:
                problems.append('access: %s accesses memory outside the physical interval' % text(step))
                continue
            if (operation.memory, step.mnemonic) not in spec.accesses:
                problems.append('access: %s is not an allowed device access' % text(step))
                continue
            if not is_uncached(register_value(code, operation.memory_base, step.position)):
                problems.append('access: %s does not address an uncacheable physical alias (bit 63)' % text(step))
            if operation.memory != 'store':
                continue
            ordering = [other for other in steps if other is not step and (other.mnemonic in ('mf', 'mf.a')
                                                                          or other.operation.memory is not None)]
            before = [other for other in ordering if other.position < step.position]
            after = [other for other in ordering if other.position > step.position]
            if not before or before[-1].mnemonic != 'mf' or before[-1].group >= step.group:
                problems.append('access: %s is not preceded by mf in an earlier group' % text(step))
            if not after or after[0].mnemonic != 'mf.a' or after[0].group <= step.group or after[0].position >= end:
                problems.append('access: %s is not followed by mf.a in a later group inside the interval' % text(step))

    # No general- or predicate-register read after write within a group.
    for index, first in enumerate(steps):
        for second in steps[index + 1:]:
            if second.group != first.group:
                break
            general = (first.operation.general_writes & second.operation.general_reads) - {'r0'}
            predicates = (_predicates(first.operation.writes) & _predicates(second.operation.reads)) - {'p0'}
            if second.operation.is_branch:
                predicates.discard(second.predicate)
            if general or predicates:
                problems.append('instruction group: %s reads %s written by %s in the same instruction group'
                                % (text(second), ', '.join(sorted(general | predicates)), text(first)))
    return problems


@register
class Ia64PhysicalAccessCorridor(HelperProperty):
    """The physical corridor shared by the diagnostic UART adapter, the port
    leaves and the MMIO leaves (G(i) is the instruction group of instruction i):

    - No `alloc`, call, `cover`, `flushrs`, `loadrs`, `break`, `rfi` or
      `bsw`, no gp (r1), sp (r12) or tp (r13), and no relocation (read from
      the ELF RELA sections) inside the leaf.
    - Exactly one unpredicated `br.ret b0`, the last instruction of the
      last bundle; other branches (the UART poll loop only) stay inside.
    - Exactly one ar.rsc save, one `dep rL=0,rS,0,2` of it and two ar.rsc
      writes: the lazy value before `rsm 0x20000`, the saved value after the
      PSR.dt restore is serialized and before the PSR.i restore; no other AR.
    - Every PSR write is serialized by the next srlz, an unpredicated
      `srlz.d` with a larger G, with no memory access, ar.rsc write or PSR
      write in between; no other kind of srlz.
    - One `mov rP=psr` before `rsm 0x4000` before `rsm 0x20000`; the
      restores `ssm 0x20000` and then `ssm 0x4000` are predicated on the
      entry-PSR bit-17 and bit-14 set predicates of `tbit.z` on rP; no other
      PSR, control-register or predicate-file access.
    - Between the srlz after `rsm 0x20000` and the PSR.dt restore, memory
      is accessed only by the leaf's allowed device accesses, through a base
      whose value has bit 63 set (an uncacheable physical alias); each store
      is preceded by `mf` and followed by `mf.a`, in other groups, inside that
      interval; no memory access at all outside it.
    - No general or predicate register is read in the instruction group
      that writes it (a branch may read its qualifying predicate)."""
    GROUPS = PHYSICAL_GROUPS

    def verify(self, facts):
        return ['%s: %s' % (name, problem) for name, code in facts.items()
                for problem in corridor_problems(code, CORRIDOR_SPECS[name])]

    def item_mutants(self, code):
        def find(predicate):
            return code.first(lambda step: predicate(step.operation, step))

        def psr(base, mask):
            return find(lambda op, _: op.base == base and op.immediates == (mask,))

        disable_i, disable_dt = psr('rsm', Ia64.PSR_I), psr('rsm', Ia64.PSR_DT)
        restore_dt, restore_i = psr('ssm', Ia64.PSR_DT), psr('ssm', Ia64.PSR_I)
        dep = find(lambda op, _: op.base == 'dep')
        rsc_writes = code.all(lambda step: 'ar.rsc' in step.operation.writes)
        returning = find(lambda op, _: op.is_return)
        memory = code.all(lambda step: step.operation.memory is not None)
        alias = find(lambda op, _: op.base == 'movl' and op.immediates and op.immediates[0] & UNCACHED_ALIAS)
        serialize_dt = code.first(lambda step: step is disable_dt, lambda step: step.operation.base == 'srlz')
        lazy = _DEP.match(dep.text)
        test_dt = find(lambda op, step: (match := Ia64.TBIT.match(step.text)) is not None
                       and int(match.group('bit')) == PSR_DT_BIT)

        yield 'alias movl without bit 63 (cacheable)', code.rewrite(alias, '%s=%#x' % (
            alias.text.split('=')[0], alias.operation.immediates[0] & ~UNCACHED_ALIAS))
        yield 'dep replaced by clearing the whole ar.rsc', code.rewrite(dep, 'mov %s=r0' % lazy.group('target'))
        yield 'dep clears three ar.rsc bits', code.rewrite(dep, dep.text.replace(',0,2', ',0,3'))
        yield 'eager ar.rsc installed during the device access', code.rewrite(
            rsc_writes[0], 'mov.m ar.rsc=%s' % lazy.group('source'))
        yield 'ar.rsc not restored', code.rewrite(rsc_writes[1], 'nop.m 0x0')
        yield 'extra physical load', code.insert(code.next(serialize_dt), 'ld8 r21=[r22]', stop=True)
        yield 'load before PSR.dt is cleared', code.insert(disable_dt, 'ld8 r21=[r22]', stop=True)
        for store in (step for step in memory if step.operation.memory == 'store'):
            label = '%s (#%d)' % (store.text, store.position)
            fence = code.first(lambda step: step is store, lambda step: step.mnemonic == 'mf.a')
            yield 'mf.a after %s weakened to mf' % label, code.rewrite(fence, 'mf')
            fence = code.all(lambda step: step.mnemonic == 'mf', before=store)[-1]
            yield 'mf before %s removed' % label, code.rewrite(fence, 'nop.m 0x0')
        other = Ia64.TBIT.match(test_dt.text).group('clear')
        yield 'PSR.dt restored on the wrong predicate', code.rewrite(restore_dt, '(%s) %s' % (other, restore_dt.text))
        yield 'PSR.dt restore sets PSR.i', code.rewrite(restore_dt, restore_dt.rendered.replace('0x20000', '0x4000'))
        yield 'PSR.i restore sets PSR.dt', code.rewrite(restore_i, restore_i.rendered.replace('0x4000', '0x20000'))
        yield 'PSR.dt restore unpredicated', code.rewrite(restore_dt, restore_dt.text)
        yield 'PSR.i and PSR.dt restores exchanged', code.swap(restore_dt, restore_i)
        yield 'PSR.dt cleared before PSR.i', code.swap(disable_i, disable_dt)
        yield 'srlz.d after rsm 0x20000 removed', code.rewrite(serialize_dt, 'nop.m 0x0')
        yield 'srlz.i instead of srlz.d after rsm 0x4000', code.rewrite(
            code.first(lambda step: step is disable_i, lambda step: step.operation.base == 'srlz'), 'srlz.i')
        yield 'PSR capture replaced by nop.m', code.rewrite(find(lambda op, _: 'psr' in op.reads), 'nop.m 0x0')
        yield 'ar.rsc save replaced by nop.m', code.rewrite(find(lambda op, _: 'ar.rsc' in op.reads), 'nop.m 0x0')
        yield 'return replaced by nop.b', code.rewrite(returning, 'nop.b 0x0')
        yield 'register frame allocated', code.insert(0, 'alloc r2=ar.pfs,2,2,0', stop=True)
        yield 'stack pointer used', code.insert(returning, 'adds r12=-16,r12', stop=True)
        yield 'relocation inside the leaf', dataclasses.replace(code, relocations=code.relocations + (
            RelocationFact(0x10, 'LTOFF22X', '.rodata', 0),))
        branches = code.all(lambda step: step.target is not None)
        if branches:
            yield 'branch out of the leaf', code.retarget(branches[0], 'AsteriaIa64UnexpectedTarget')
        else:
            yield 'branch inserted', code.insert(returning, 'br.few <AsteriaIa64UnexpectedTarget>', stop=True)
        # Merge a producer's group with its consumer's.
        yield 'no stop after the alias movl', code.without_boundary(alias)
        for load in (step for step in memory if step.operation.memory == 'load'):
            consumer = code.next(load)
            if consumer is not None and load.operation.general_writes & consumer.operation.reads:
                yield 'no stop after %s' % load.text, code.without_boundary(load)


# MMIO leaf checks.

SCRATCH = Ia64.SCRATCH_GENERAL
_REGISTER_TOKEN = re.compile(r'(?<![A-Za-z0-9_])r(\d+)(?![A-Za-z0-9_])')
# (operation, transaction width in bytes) of each MMIO leaf.
MMIO_OPERATIONS = {MMIO_READ: ('Read', 4), MMIO_WRITE: ('Write', 4), MMIO_READ_QUAD: ('Read', 8),
                   MMIO_WRITE_QUAD: ('Write', 8)}
_CHAIN_ROLES = (('psr', r'mov r\d+=psr'), ('address', r'mov r\d+=r32'), ('value', r'mov r\d+=r33'),
                ('disable_i', r'rsm 0x4000'), ('disable_dt', r'rsm 0x20000'), ('srlz', r'srlz\.d'),
                ('rsc_save', r'mov\.m r\d+=ar\.rsc'), ('lazy', r'dep r\d+=0,r\d+,0,2'),
                ('rsc_write', r'mov\.m ar\.rsc=r\d+'), ('alias_bit', r'movl r\d+=0x8000000000000000'),
                ('or', r'or r\d+=r\d+,r\d+'), ('mf', r'mf'), ('mfa', r'mf\.a'), ('dt_test', r'tbit\.z p7,p6=r\d+,17'),
                ('i_test', r'tbit\.z p9,p8=r\d+,14'), ('restore_dt', r'\(p06\) ssm 0x20000'),
                ('restore_i', r'\(p08\) ssm 0x4000'), ('return', r'br\.ret\.sptk\.many b0'))
_TRANSACTION = {'Read': r'ld%d\.acq r8=\[(r\d+)\]', 'Write': r'st%d \[(r\d+)\]=(r\d+)'}
OTHER_WIDTH = {4: 8, 8: 4}            # The width mutant of each MMIO width
# Chain roles counted where they occur (the others need operand checks).
NOTED_ROLES = frozenset({'psr', 'address', 'value', 'disable_i', 'disable_dt', 'rsc_save', 'lazy', 'alias_bit', 'mf',
                         'mfa', 'dt_test', 'i_test', 'restore_dt', 'restore_i', 'return'})


def references(text: str, register: str) -> bool:
    return re.search(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % re.escape(register), text) is not None


def defined_general(plain: str) -> str:
    match = re.match(r'^\S+\s+(r\d+)=', plain)
    return match.group(1) if match else ''


def harmless(plain: str) -> bool:
    """`mov rD=rS` or `(add|sub|and|andcm|or|xor) rD=rS1,rS2` over caller-scratch registers (and r0)."""
    if defined_general(plain) not in SCRATCH or any('r%s' % number not in SCRATCH | {'r0'}
                                                     for number in _REGISTER_TOKEN.findall(plain)):
        return False
    return bool(re.fullmatch(r'mov r\d+=r\d+', plain) or
                re.fullmatch(r'(add|sub|and|andcm|or|xor) r\d+=r\d+,r\d+', plain))


def transaction_pattern(operation: str, width: int) -> str:
    """The pattern of the one device access of an MMIO leaf: `ld<width>.acq
    r8=[rC]` (Read) or `st<width> [rC]=rV` (Write), capturing rC and rV."""
    return _TRANSACTION[operation] % width


def mmio_chain(code: Code, operation: str, width: int = 4) -> dict:
    """Role indices (among meaningful instructions) of an MMIO leaf; see mmio_problems."""
    roles: dict[str, list[int]] = {}
    for index, step in enumerate(code.steps):
        for role, pattern in _CHAIN_ROLES + (('transaction', transaction_pattern(operation, width)),):
            if re.fullmatch(pattern, step.rendered):
                roles.setdefault(role, []).append(index)
    return roles


@functools.lru_cache(maxsize=None)
def mmio_problems(code: Code, operation: str, width: int = 4) -> tuple[str,...]:
    """M1- of an MMIO leaf (operation Read or Write, transaction width in
    bytes), over the meaningful instructions with stops carried backward
    from nop slots. Memoized: every mutant re-judges the unchanged
    acceptance variants."""
    pattern = transaction_pattern(operation, width)
    texts = [step.rendered for step in code.steps]
    plains = [step.text for step in code.steps]
    boundary = [code.ends_group(step) for step in code.steps]
    defined = [defined_general(plain) for plain in plains]
    problems = ['M5: relocation %s inside the leaf' % (fact,) for fact in code.relocations]
    count: Counter = Counter()
    first: dict[str, int] = {}
    last: dict[str, int] = {}
    register: dict[str, str] = {}
    serialization: list[int] = []
    lazy_source = None
    install = restore = None

    def note(role: str, position: int) -> None:
        count[role] += 1
        last[role] = position
        if count[role] == 1:
            first[role] = position
            register[role] = defined[position]

    for position, (text, plain) in enumerate(zip(texts, plains)):
        matched = {role for role, pattern in _CHAIN_ROLES if re.fullmatch(pattern, text)}
        for role in sorted(matched & NOTED_ROLES):
            note(role, position)
        if re.match(r'(rsm|ssm)(\s|$)', plain) and not matched & {'disable_i', 'disable_dt', 'restore_dt', 'restore_i'}:
            problems.append('M4: %s writes the PSR outside the four corridor forms' % text)
        if text == 'srlz.d':
            serialization.append(position)
        elif text.startswith('srlz.'):
            problems.append('M4: %s is not srlz.d' % text)
        if 'lazy' in matched and count['lazy'] == 1:
            lazy_source = re.fullmatch(r'dep (r\d+)=0,(r\d+),0,2', text).group(2)
        match = re.fullmatch(r'mov\.m ar\.rsc=(r\d+)', text)
        if match:
            count['rsc_write'] += 1
            if match.group(1) == register.get('lazy') and install is None:
                install = position
            elif match.group(1) == register.get('rsc_save') and restore is None:
                restore = position
            else:
                problems.append('M1: %s writes ar.rsc from neither the lazy nor the saved value' % text)
        if 'ar.' in text and not re.fullmatch(r'mov\.m r\d+=ar\.rsc', text) \
                and not re.fullmatch(r'mov\.m ar\.rsc=r\d+', text):
            problems.append('M4: %s accesses an application register other than ar.rsc' % text)
        if 'cr.' in text or ('psr' in text and not re.fullmatch(r'mov r\d+=psr', text)) \
                or references(plain, 'pr'):
            problems.append('M4: %s accesses a control register, the PSR or the predicate file' % text)
        match = re.fullmatch(r'or (r\d+)=(r\d+),(r\d+)', text)
        if match and 'address' in register and 'alias_bit' in register \
                and {match.group(2), match.group(3)} == {register['address'], register['alias_bit']}:
            note('alias_address', position)
        transaction = False
        match = re.fullmatch(pattern, text)
        if operation == 'Read' and match:
            note('transaction', position)
            transaction = True
            if match.group(1) != register.get('alias_address'):
                problems.append('M1: %s does not load through the uncached alias' % text)
        if operation == 'Write' and match:
            note('transaction', position)
            transaction = True
            if match.group(1) != register.get('alias_address') or match.group(2) != register.get('value'):
                problems.append('M1: %s does not store the r33 capture through the uncached alias' % text)
        if (re.match(r'(ld|st)\d(\.|\s)', plain) and not transaction) \
                or re.match(r'(cmpxchg|xchg|fetchadd|lfetch|probe|fc)\.', plain):
            problems.append('M3: %s accesses memory besides the transaction' % text)
        if matched & {'dt_test', 'i_test'} and Ia64.TBIT.match(text).group('source') != register.get('psr'):
            problems.append('M1: %s does not test the saved PSR' % text)
        if 'return' not in matched and re.match(r'(br|brl)\.', plain):
            problems.append('M5: %s branches' % text)
        if re.match(r'(alloc|cover|flushrs|loadrs|invala)(\s|$)', plain) \
                or re.match(r'(break|bsw\.|rfi)(\s|$)', plain) or re.match(r'(br|brl)\.call', plain) \
                or references(plain, 'r12') or 'ar.pfs' in plain:
            problems.append('M5: %s is forbidden in the leaf' % text)
        permitted = matched or match or harmless(plain)
        if not permitted:
            problems.append('M6: %s is neither a corridor instruction nor harmless scratch arithmetic' % text)

    roles = ['psr', 'address', 'rsc_save', 'lazy', 'alias_bit', 'alias_address'] + (
        ['value'] if operation == 'Write' else [])
    role_registers = [register.get(role, '') for role in roles]
    problems += ['M2: the %s role register %r is not caller-scratch' % (role, name)
                 for role, name in zip(roles, role_registers) if name not in SCRATCH]
    if len(set(role_registers)) != len(role_registers):
        problems.append('M2: role registers overlap: %s' % dict(zip(roles, role_registers)))
    for position, definition in enumerate(defined):
        text, plain = texts[position], plains[position]
        if definition and definition not in SCRATCH:
            problems.append('M2: %s defines %s, which is not caller-scratch' % (text, definition))
        problems += ['M2: %s redefines the %s role register' % (text, role) for role in roles
                     if definition and definition == register.get(role) and position != first.get(role)]
        if operation == 'Read' and definition == 'r8' and position != last.get('transaction'):
            problems.append('M2: %s redefines the r8 result' % text)
        stacked = any(32 <= int(number) <= 127 for number in _REGISTER_TOKEN.findall(text))
        if stacked and not (position == first.get('address') and re.fullmatch(r'mov r\d+=r32', text)) \
                and not (operation == 'Write' and position == first.get('value')
                         and re.fullmatch(r'mov r\d+=r33', text)):
            problems.append('M2: %s references a stacked register' % text)
        if re.match(r'^\S+\s+p\d+(,p\d+)?=', text) and position not in (last.get('dt_test'), last.get('i_test')):
            problems.append('M2: %s defines a predicate' % text)
        if re.match(r'^\S+\s+f\d+=', plain) or re.match(r'^\S+\s+b\d+=', plain):
            problems.append('M2: %s defines a floating-point or branch register' % text)
        if re.search(r'(^|[^A-Za-z0-9_])(rr|pkr|dbr|ibr|pmc|pmd)\[', plain):
            problems.append('M2: %s accesses a region, key, debug or monitor register' % text)
        if re.match(r'(itr|itc|ptr|ptc|fc|probe|tak|thash|ttag)(\.|\s)', plain) \
                or re.match(r'(invala|sync)(\.|\s)', plain):
            problems.append('M2: %s controls translation or caches' % text)

    expected = {'psr': 1, 'address': 1, 'disable_i': 1, 'rsc_save': 1, 'lazy': 1, 'rsc_write': 2,
                'disable_dt': 1, 'alias_bit': 1, 'alias_address': 1, 'mf': 1, 'transaction': 1, 'mfa': 1,
                'dt_test': 1, 'i_test': 1, 'restore_dt': 1, 'restore_i': 1, 'return': 1,
                'value': 1 if operation == 'Write' else 0}
    problems += ['M1: %d %s instructions, expected %d' % (count[role], role.replace('_', ' '), number)
                 for role, number in expected.items() if count[role] != number]
    if len(serialization) != 4:
        problems.append('M1: %d srlz.d, expected 4' % len(serialization))
    if lazy_source != register.get('rsc_save') or install is None or restore is None:
        problems.append('M1: the lazy ar.rsc is not derived from the saved one, or is not installed and restored')
    if problems:
        return tuple(problems)

    capture_end = max(first['psr'], first['address'], *([first['value']] if operation == 'Write' else []))
    final_test = max(last['dt_test'], last['i_test'])
    chain = [capture_end, last['disable_i'], serialization[0], first['rsc_save'], first['lazy'], install,
             last['disable_dt'], serialization[1], first['alias_bit'], first['alias_address'], last['mf'],
             last['transaction'], last['mfa'], final_test, last['restore_dt'], serialization[2], restore,
             last['restore_i'], serialization[3], last['return']]
    if any(a >= b for a, b in zip(chain, chain[1:])) or last['mfa'] >= min(last['dt_test'], last['i_test']):
        return tuple(problems + ['M7: the corridor steps are out of order: %s' % chain])
    return tuple(problems + ['M8: no instruction-group boundary between %s and %s' % (texts[a], texts[b])
                             for a, b in zip(chain, chain[1:]) if not any(boundary[k] for k in range(a, b))])


def unused_scratch(code: Code, count: int) -> list[str]:
    used = {'r%s' % number for instruction in code.instructions
            for number in _REGISTER_TOKEN.findall(instruction.rendered())}
    return [name for name in sorted(SCRATCH, key=lambda r: int(r[1:])) if name not in used and name != 'r8'][:count]


def renamed(code: Code, old: str, new: str) -> Code:
    pattern = re.compile(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % re.escape(old))
    return code.with_instructions(written(instruction, pattern.sub(new, instruction.rendered()))
                                  if pattern.search(instruction.rendered()) else instruction
                                  for instruction in code.instructions)


def mmio_variants(code: Code, operation: str):
    """Legitimate variants the MMIO judgment must accept (the old oracle's positive self-tests)."""
    capture = code.first(lambda step: re.fullmatch(r'mov r\d+=psr', step.rendered))
    address = code.first(lambda step: re.fullmatch(r'mov r\d+=r32', step.rendered))
    role = r'movl (r\d+)=0x8000000000000000' if operation == 'Read' else r'mov (r\d+)=r33'
    target = next(match.group(1) for step in code.steps if (match := re.fullmatch(role, step.rendered)))
    spare, a, b = unused_scratch(code, 3)
    yield 'independent captures exchanged', code.swap(capture, address)
    yield 'role register %s renamed to %s' % (target, spare), renamed(code, target, spare)
    yield 'harmless scratch addition before the return', code.insert(code.steps[-1], 'add %s=%s,%s' % (spare, a, b),
                                                                      stop=True)


def mmio_mutants(code: Code, operation: str, width: int = 4):
    """The old oracle's rejected mutants, derived from the facts."""
    steps = code.steps
    roles = mmio_chain(code, operation, width)

    def at(role, which=0):
        return steps[roles[role][which]]

    transaction, returning, alias = at('transaction'), at('return'), at('alias_bit')
    lazy = re.fullmatch(r'dep (r\d+)=0,(r\d+),0,2', at('lazy').rendered).group(1)
    yield '%s replaced by nop.m' % ('load' if operation == 'Read' else 'store'), code.rewrite(transaction, 'nop.m 0x0')
    other = OTHER_WIDTH[width]
    yield 'transaction width %d instead of %d' % (other, width), code.rewrite(
        transaction, re.sub(r'^(ld|st)%d' % width, r'\g<1>%d' % other, transaction.rendered))
    yield 'alias bit 62 instead of 63', code.rewrite(alias, alias.text.replace('0x8000000000000000',
                                                                                '0x4000000000000000'))
    yield 'PSR capture replaced by nop.m', code.rewrite(at('psr'), 'nop.m 0x0')
    yield 'PSR.i disabled after the restore', code.insert(returning, 'rsm 0x4000', stop=True)
    yield 'ar.rsc written after the restore', code.insert(returning, 'mov.m ar.rsc=%s' % lazy, stop=True)
    if operation == 'Read':
        yield 'result clobbered after the load', code.insert(code.next(transaction), 'mov r8=r0', stop=True)
    else:
        value = re.fullmatch(transaction_pattern(operation, width), transaction.text).group(2)
        yield 'value clobbered before the store', code.insert(transaction, 'mov %s=r0' % value, stop=True)
    # Every required group boundary: clear all stops between consecutive chain steps.
    capture_end = max(roles['psr'][0], roles['address'][0], *(roles['value'][:1] if operation == 'Write' else []))
    final_test = max(roles['dt_test'][0], roles['i_test'][0])
    chain = [capture_end, roles['disable_i'][0], roles['srlz'][0], roles['rsc_save'][0], roles['lazy'][0],
             roles['rsc_write'][0], roles['disable_dt'][0], roles['srlz'][1], roles['alias_bit'][0], roles['or'][0],
             roles['mf'][0], roles['transaction'][0], roles['mfa'][0], final_test, roles['restore_dt'][0],
             roles['srlz'][2], roles['rsc_write'][1], roles['restore_i'][0], roles['srlz'][3], roles['return'][0]]
    for number, (a, b) in enumerate(zip(chain, chain[1:]), 1):
        yield ('group boundary %d (%s | %s) removed' % (number, steps[a].rendered, steps[b].rendered),
               code.without_stops(steps[a], steps[b]))
    for text in ('mov r22=r34', 'mov r4=r22', 'mov b1=r22', 'mov ar.unat=r22', 'mov rr[r22]=r23',
                 'mov dbr[r22]=r23', 'mov ibr[r22]=r23', 'mov pkr[r22]=r23', 'mov pmc[r22]=r23',
                 'mov pmd[r22]=r23', 'ptc.e r22', 'fmerge.s f2=f6,f7', 'cmp.eq p1,p2=r22,r23'):
        yield '%s inserted before the return' % text, code.insert(returning, text, stop=True)


@dataclasses.dataclass(frozen=True)
class MmioFacts:
    operation: str
    code: Code
    variants: tuple[tuple[str, Code],...]
    width: int = 4


@register
class Ia64PhysicalMemoryMappedIoLeaves(HelperProperty):
    """MMIO leaf checks bind registers to roles, allowing register renaming
    and independent instruction reordering:

    -: exactly one of each role instruction (PSR capture, r32 capture, r33
      capture for Write only, `rsm 0x4000`, ar.rsc save, `dep rL=0,rS,0,2`,
      `rsm 0x20000`, `movl rB=0x8000000000000000`, `or rC=rA,rB`, `mf`, the
      transaction `ldW.acq r8=[rC]` or `stW [rC]=rV` of the leaf's width W (4
      for the double-word, 8 for the quad-word leaves), `mf.a`,
      `tbit.z p7,p6=rP,17`, `tbit.z p9,p8=rP,14`, `(p06) ssm 0x20000`, `(p08) ssm 0x4000`,
      `br.ret.sptk.many b0`), four `srlz.d`, and two ar.rsc writes: rL, then rS.
    -: role registers are distinct caller-scratch registers defined only by
      their role (r8 only by the load); no stacked register besides the
      captures, no predicate definition besides the tbits, no FR/BR
      definition, no RR/PKR/DBR/IBR/PMC/PMD access, no translation or cache
      control.
    -: no memory access besides the transaction.
    -: no other rsm/ssm/srlz form, AR, CR, PSR or predicate-file access.
    -: the return is the only branch; no alloc, cover, flushrs, loadrs,
      invala, break, bsw, rfi, call, r12 or ar.pfs; no relocation in the leaf.
    -: every instruction is a role instruction or harmless scratch arithmetic.
    -: the role steps are in corridor order with an instruction-group
      boundary between each consecutive pair (19 boundaries).

    The judgment must also accept the old oracle's legitimate variants:
    exchanged independent captures, a renamed role register, and harmless
    scratch arithmetic before the return."""
    GROUPS = (MMIO,)

    def item(self, context, variant, name):
        code = Subjects.leaf(context, variant, name).code
        operation, width = MMIO_OPERATIONS[name]
        return MmioFacts(operation, code, tuple(mmio_variants(code, operation)), width)

    def item_problems(self, leaf):
        problems = list(mmio_problems(leaf.code, leaf.operation, leaf.width))
        for description, variant in leaf.variants:
            if variant == leaf.code:
                problems.append('the variant "%s" does not differ from the leaf' % description)
            elif mmio_problems(variant, leaf.operation, leaf.width):
                problems.append('the legitimate variant "%s" is rejected: %s'
                                % (description, mmio_problems(variant, leaf.operation, leaf.width)[0]))
        return problems

    def item_mutants(self, leaf):
        for description, code in mmio_mutants(leaf.code, leaf.operation, leaf.width):
            yield description, dataclasses.replace(leaf, code=code)
        yield 'relocation inside the leaf', dataclasses.replace(leaf, code=dataclasses.replace(
            leaf.code, relocations=leaf.code.relocations + (RelocationFact(0x20, 'PCREL21B', 'memcpy', 0),)))
        label, variant = leaf.variants[0]
        yield 'an acceptance variant broken', dataclasses.replace(
            leaf, variants=((label, variant.rewrite(variant.steps[-1], 'nop.b 0x0')),) + leaf.variants[1:])


# The diagnostic writer's relocations (compiler-generated C++) --------------------------------

@dataclasses.dataclass(frozen=True)
class FunctionFacts:
    relocations: tuple[RelocationFact,...]
    calls: tuple[tuple[str, str | None, int],...]     # ('direct', symbol, offset) or ('indirect', None, 0)


def writer_problems(roots: tuple[str,...], functions: dict[str, FunctionFacts]) -> list[str]:
    """Code reachable from the roots through direct calls has no relocation
    other than a PC-relative branch to a function of the image, and no
    indirect call or call outside the image."""
    problems, pending, seen = [], list(roots), set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        facts = functions.get(name)
        if facts is None:
            problems.append('%s is not a function defined in the image' % name)
            continue
        for fact in facts.relocations:
            if fact.type == 'IMM64' and fact.target == 'AsteriaIa64DiagnosticWriter' and fact.addend == 0:
                continue   # Dedicated identity-address exclusion word; never gp-relative.
            if fact.type not in Relocations.IA64_BRANCH:
                problems.append('%s: %s relocation against %s; the writer runs with PSR.dt clear or before gp is '
                                'established and must not use GP-relative or linked data addresses'
                                % (name, fact.type, fact.target))
            elif fact.target not in functions:
                problems.append('%s: branch relocation to %s, which is not a function of the image'
                                % (name, fact.target))
        for kind, target, offset in facts.calls:
            if kind != 'direct':
                problems.append('%s makes an indirect call' % name)
            elif target not in functions or offset:
                problems.append('%s calls %s+%#x, not the start of a function of the image' % (name, target, offset))
            else:
                pending.append(target)
    return problems


def function_facts(context, path, name: str) -> FunctionFacts:
    code = Subjects.code(context, path, name)
    return FunctionFacts(code.relocations, tuple(
        ('indirect', None, 0) if operation.target is None else ('direct', operation.target.symbol,
                                                                  operation.target.offset)
        for operation in code.operations if operation.is_call))


@register
class Ia64DiagnosticWriterRelocations(Property):
    """The diagnostic writer uses no GP-relative reference and no indirect
    or foreign call. Its sole permitted linked data address is the dedicated
    identity-address exclusion word, materialized by IMM64 without an addend.
    Everything
    reachable from `WriteDiagnosticAscii` through direct calls (at O0
    `TryWriteCharacter`, and the physical UART adapter, wherever it is
    assembled) carries only that IMM64 or a PC-relative branch to a function
    of the image and makes no indirect call. The writer runs with
    PSR.dt clear or before gp is established, where such an address would be
    wrong. The facts cover every function of the writer's object and the
    functions reachable from them in other objects of the group. (The old
    check required zero relocations in the whole object, which depends on
    the codegen of unrelated functions and breaks once the adapter moves to
    its own object.)"""
    isas = ('Ia64',)

    def extract(self, context, variant):
        functions: dict[str, FunctionFacts] = {}
        home = context.defining_object(variant, DIAGNOSTIC_WRITER)
        elf = load(home)
        pending = []
        for symbol in elf.symbols:
            section = elf.section_of(symbol)
            if symbol.type == STT_FUNC and symbol.size and section is not None and section.flags & SHF_EXECINSTR:
                functions[symbol.name] = function_facts(context, home, symbol.name)
                pending.extend(target for kind, target, _ in functions[symbol.name].calls if kind == 'direct')
        while pending:
            name = pending.pop()
            path = Subjects.defining(context, variant, name)
            if name in functions or path is None:
                continue                     # Outside the image: reported by the judgment
            if any(s.type == STT_FUNC and s.size for s in load(path).symbols_named(name)):
                functions[name] = function_facts(context, path, name)
                pending.extend(target for kind, target, _ in functions[name].calls if kind == 'direct')
        return (DIAGNOSTIC_WRITER,), functions

    def verify(self, facts):
        return writer_problems(*facts)

    def mutants(self, facts):
        roots, functions = facts
        root = functions[roots[0]]

        def changed(name, **changes):
            return roots, {**functions, name: dataclasses.replace(functions[name], **changes)}

        def gains(name, fact):
            return changed(name, relocations=functions[name].relocations + (fact,))

        yield 'writer loads a constant GP-relative', gains(roots[0], RelocationFact(0x10, 'LTOFF22X', '.rodata', 0))
        yield 'writer calls a helper outside the image', gains(
            roots[0], RelocationFact(0x22, 'PCREL21B', 'AsteriaUnresolvedHelper', 0))
        yield 'writer calls through a function pointer', changed(roots[0], calls=root.calls + (('indirect', None, 0),))
        yield 'the UART adapter gains an absolute address', gains(UART_ADAPTER,
                                                                  RelocationFact(0x1, 'IMM64', '.rodata', 0))
        callees = [target for kind, target, _ in root.calls if kind == 'direct' and target != UART_ADAPTER]
        for callee in callees[:1]:
            yield 'a callee of the writer uses GPREL22', gains(callee, RelocationFact(0x10, 'GPREL22', '.data', 0))
        for other in sorted(set(functions) - set(roots) - {UART_ADAPTER} - set(callees))[:1]:
            _, broken = gains(other, RelocationFact(0x10, 'GPREL22', '.data', 0))
            broken[roots[0]] = dataclasses.replace(root, calls=root.calls + (('direct', other, 0),))
            yield 'writer calls %s, which uses GPREL22' % other, (roots, broken)


@register
class Ia64PortAccessWidths(HelperProperty):
    """Three mutually exclusive exact-width transactions, selected by the
    captured r33 width. Address and write value are captured before masking
    interrupts. The shared physical corridor proves UC and mf.a ordering."""
    GROUPS = (PORTS,)

    def verify(self, facts):
        problems = []
        for name, code in facts.items():
            accesses = [step for step in code.steps if step.operation.memory is not None]
            widths = (1, 2, 4)
            expected = ['ld%d.acq' % width if name == PORT_READ else 'st%d' % width for width in widths]
            if [step.mnemonic for step in accesses] != expected:
                problems.append('%s: exact byte/word/dword transactions missing or repeated' % name)
                continue
            fence = code.first(lambda step: step is accesses[-1], lambda step: step.mnemonic == 'mf.a')
            if fence is None or fence.group <= accesses[-1].group:
                problems.append('%s: final port transaction lacks a following mf.a' % name)
            for access in accesses:
                if name == PORT_READ and access.operation.general_writes != frozenset({'r8'}):
                    problems.append('%s: read does not return its value in r8' % name)
                if name == PORT_WRITE and 'r19' not in access.operation.general_reads:
                    problems.append('%s: write does not use its captured value' % name)
            captures = [step for step in code.steps if step.text == 'mov r22=r33']
            disabled = code.first(lambda step: step.operation.base == 'rsm')
            if len(captures) != 1 or disabled is None or captures[0].position >= disabled.position \
                    or len(code.all(lambda step: 'r22' in step.operation.general_writes)) != 1:
                problems.append('%s: width is not captured before interruption masking' % name)
            for access, width in zip(accesses, widths):
                predicate = access.predicate
                tests = [step for step in code.steps if step.operation.base == 'cmp'
                         and step.operation.writes_predicate(predicate)]
                if not predicate or len(tests) != 1 or tests[0].position >= access.position:
                    problems.append('%s: transaction has no unique width predicate' % name)
                    continue
                test = tests[0]
                # Binutils renders cmp.eq with the literal first.
                match = re.fullmatch(r'cmp.eq (p\d+),(p\d+)=(\d+),r22', test.text)
                if match is None or Ia64.canonical_predicate(match.group(1)) != predicate \
                        or test.operation.immediates != (width,) \
                        or test.operation.general_reads != frozenset({'r22'}) \
                        or any(step.operation.writes_predicate(predicate)
                               for step in code.window(after=test, before=access)):
                    problems.append('%s: %s is not selected by width %d' % (name, access.mnemonic, width))
            for target, source in [('r16', 'r32')] + ([('r19', 'r34')] if name == PORT_WRITE else []):
                captures = [step for step in code.steps if step.text == 'mov %s=%s' % (target, source)]
                if len(captures) != 1 or captures[0].position >= disabled.position \
                        or len(code.all(lambda step: target in step.operation.general_writes)) != 1:
                    problems.append('%s: input %s not captured before masking' % (name, source))
        return problems

    def item_mutants(self, code):
        for access in code.all(lambda step: step.operation.memory is not None):
            yield 'port transaction unpredicated #%d' % access.position, code.rewrite(access, access.text)
            yield 'port transaction removed #%d' % access.position, code.rewrite(access, 'nop.m 0x0')
        capture = code.first(lambda step: step.text == 'mov r22=r33')
        yield 'wrong width input', code.rewrite(capture, 'mov r22=r32')
        for test in code.all(lambda step: step.operation.base == 'cmp'):
            yield 'wrong selected port width #%d' % test.position, code.rewrite(test, test.rendered.replace('=1,', '=8,')
.replace('=2,', '=8,').replace('=4,', '=8,'))
