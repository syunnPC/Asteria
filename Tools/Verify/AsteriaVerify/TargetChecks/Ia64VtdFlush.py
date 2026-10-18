# SPDX-License-Identifier: Apache-2.0
"""IA-64 VT-d table publication leaves.

Checked code (helpers are located by symbol): `Device/Ia64VtdCacheMaintenance.S`,
the leaves `AsteriaIa64VtdFlushTables(start, bytes)` and `AsteriaIa64VtdFence`.

A remapping unit that reports ECAP.C = 0 does not snoop the processor caches,
so every table line the kernel changed is written back before the register
write that makes it reachable. The decision fixes the sequence: `fc` on a
32-byte stride, then `;; sync.i;; srlz.i;; mf`, and forbids shortening it.
The Itanium SDM, Vol. 3 rev 1.1 (245319-002), states why: `fc` affects a line
of at least 32 bytes aligned on a 32-byte boundary; `fc` is unordered and
not affected by `mf`, but ordered with respect to `sync.i`, which must be in
a later instruction group than the `fc`; an instruction serialize (`srlz.i`)
ensures the `sync.i` has been observed; `mf` then orders the flushes before
the unit's next uncached register access. The line size here is that
architectural minimum, not the layout header's constant, so a header change
to a longer stride is rejected.

Goldens under Tests/Golden/Ia64/Device pin both leaves; the properties here
state what a golden update must keep.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Ia64, Properties
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import HelperExtents, HelperGroup, HelperProperty, HelperSymbols

FLUSH_TABLES = 'AsteriaIa64VtdFlushTables'
FENCE = 'AsteriaIa64VtdFence'
VTD = HelperGroup('VtdCacheMaintenance', (FLUSH_TABLES, FENCE))

START, BYTES = 'r32', 'r33'          # In0, in1 of the leaf
LINE_SHIFT = 5                        # SDM: fc affects at least 32 aligned bytes
LINE_BYTES = 1 << LINE_SHIFT
TAIL = ('sync.i', 'srlz.i', 'mf')
# Software conventions: p6-p15 are the scratch predicates (p1-p5, p16-p63 preserved).
SCRATCH_PREDICATES = frozenset('p%d' % number for number in range(6, 16))
_PREDICATE = re.compile(r'p\d+')


@register
class Ia64VtdFlushSymbols(HelperSymbols):
    GROUPS = (VTD,)


@register
class Ia64VtdFlushExtents(HelperExtents):
    GROUPS = (VTD,)


def _label(step) -> str:
    return '%s (#%d)' % (step.rendered, step.position)


def leaf_rule_problems(code: Code) -> list[str]:
    """P-LEAF and the leaf's return: no alloc, call, gp/sp use, RSE control,
    memory access or relocation (Properties.leaf_problems); no tp, AR, CR,
    PSR, predicate-file, indexed-resource or floating-point access, and no
    branch register but the return's read of b0; writes only to
    caller-scratch general registers and p6-p15; exactly one unpredicated
    `br.ret` through b0, the last instruction of the last bundle; any other
    branch stays inside the leaf; and no general or predicate register is
    read in the instruction group that writes it (a branch may read its
    qualifying predicate)."""
    steps = code.steps
    problems = Properties.leaf_problems(list(code.instructions), relocations=code.relocations)
    for step in steps:
        operation = step.operation
        used = operation.reads | operation.writes
        foreign = sorted(register for register in used if register.startswith(('ar.', 'cr.', 'psr', 'pr', 'f'))
                         or '[' in register or register in (Ia64.THREAD_POINTER, 'tlb')
                         or (register.startswith('b') and register != 'b0'))
        foreign += ['b0'] if 'b0' in operation.writes else []
        if foreign:
            problems.append('%s uses %s' % (_label(step), ', '.join(foreign)))
        preserved = sorted(register for register in operation.general_writes if register not in Ia64.SCRATCH_GENERAL)
        preserved += sorted(register for register in operation.writes if _PREDICATE.fullmatch(register)
                            and register != 'p0' and register not in SCRATCH_PREDICATES)
        if preserved:
            problems.append('%s writes %s, not a caller-scratch register' % (_label(step), ', '.join(preserved)))
        if operation.is_branch and not operation.is_return and step.target is None:
            problems.append('%s leaves the leaf' % _label(step))
    returns = [step for step in steps if step.operation.is_return]
    if len(returns) != 1:
        problems.append('%d returns, expected exactly one' % len(returns))
    else:
        ret = returns[0]
        if ret.operation.is_predicated or ret.operation.reads != {'b0'}:
            problems.append('%s is not an unpredicated return through b0' % _label(ret))
        if steps[-1] is not ret or ret.bundle != (code.instructions[-1].address - code.start) & ~0xF:
            problems.append('the return %s is not the last instruction of the last bundle' % _label(ret))
    for index, first in enumerate(steps):
        for second in steps[index + 1:]:
            if second.group != first.group:
                break
            general = (first.operation.general_writes & second.operation.general_reads) - {'r0'}
            predicates = {register for register in first.operation.writes & second.operation.reads
                          if _PREDICATE.fullmatch(register) and register != 'p0'}
            if second.operation.is_branch:
                predicates.discard(second.predicate)
            if general or predicates:
                problems.append('%s reads %s written by %s in the same instruction group'
                                % (_label(second), ', '.join(sorted(general | predicates)), _label(first)))
    return problems


@dataclasses.dataclass(frozen=True)
class FlushRoles:
    """The steps of a well-formed flush loop (see flush_roles)."""
    bound: object
    align: object
    head: object
    flush: object
    advance: object
    branch: object
    tail: tuple
    line: str
    limit: str
    taken: str                 # The loop predicate: the line is below the limit
    other: str                 # The cmp's complementary predicate


def _is_alignment(step, line: str) -> bool:
    """`dep L=0,r32,0,5` or `and L=-32,r32`: the start with bits 0..4 cleared."""
    mnemonic, destinations, sources = Ia64.split(step.text)
    if step.operation.is_predicated or destinations != [line]:
        return False
    return ((mnemonic == 'dep' and sources == ['0', START, '0', str(LINE_SHIFT)])
            or (mnemonic == 'and' and sources == [str(-LINE_BYTES), START]))


def flush_roles(code: Code, problems: list[str]) -> FlushRoles | None:
    """Binds the roles of AsteriaIa64VtdFlushTables, recording a problem and
    returning None where the shape breaks:

    - the only non-return branch is `(P) br.cond` back to the loop head, the
      unique natural loop, whose body is exactly the steps from the head to
      that branch and which leaves only by falling through that branch;
    - the head is `cmp.ltu P,Q=L,B` (P: L < B unsigned), unpredicated;
    - before it only `add B=r32,r33` and the alignment of r32 to L
      (`dep L=0,r32,0,5`, or the equal `and L=-32,r32`), both unpredicated;
    - in the body, after the head, only `(P) fc L` and then `(P) adds L=32,L`.

    With the exact tail of flush_problems, nothing else writes L, B or P."""
    steps = code.steps
    branches = [step for step in steps if step.operation.is_branch and not step.operation.is_return]
    if len(branches) != 1:
        problems.append('%d branches besides the return, expected exactly the loop branch' % len(branches))
        return None
    branch = branches[0]
    head = code.target(branch)
    if head is None or head.position >= branch.position:
        problems.append('%s is not a backward branch inside the leaf' % _label(branch))
        return None
    loops = code.graph.loops
    body = frozenset(step.position for step in steps if head.position <= step.position <= branch.position)
    if loops != {head.position: body}:
        problems.append('the natural loops are %s, expected one loop from %s through %s'
                        % ({key: sorted(value) for key, value in loops.items()}, _label(head), _label(branch)))
        return None
    tail = tuple(step for step in steps if step.position > branch.position)
    if not tail or code.graph.exits(body) != [(branch.position, tail[0].position)]:
        problems.append('the loop does not leave only by falling through %s' % _label(branch))
        return None
    mnemonic, destinations, sources = Ia64.split(head.text)
    if mnemonic != 'cmp.ltu' or len(destinations) != 2 or len(sources) != 2 or head.operation.is_predicated \
            or len(set(sources)) != 2:
        problems.append('the loop head %s is not an unpredicated cmp.ltu of two registers' % _label(head))
        return None
    taken, other = (Ia64.canonical_predicate(predicate) for predicate in destinations)
    line, limit = sources
    if taken == 'p0':
        problems.append('the loop head %s sets p0' % _label(head))
        return None
    if branch.operation.completers[:1] != ('cond',) or branch.predicate != taken:
        problems.append('%s is not a br.cond on the loop predicate %s' % (_label(branch), taken))
    inside = [step for step in steps if head.position < step.position < branch.position]
    if [step.mnemonic for step in inside] != ['fc', 'adds']:
        problems.append('the loop body between %s and %s is %s, expected exactly fc then adds'
                        % (_label(head), _label(branch), [step.rendered for step in inside]))
        return None
    flush, advance = inside
    if flush.predicate != taken or Ia64.split(flush.text)[2] != [line]:
        problems.append('%s does not flush the line register %s under the loop predicate %s'
                        % (_label(flush), line, taken))
    if advance.predicate != taken or advance.operation.general_writes != {line} \
            or advance.operation.general_reads != {line} or advance.operation.immediates != (LINE_BYTES,):
        problems.append('%s does not advance %s by exactly %d under %s' % (_label(advance), line, LINE_BYTES, taken))
    prologue = [step for step in steps if step.position < head.position]
    aligns = [step for step in prologue if _is_alignment(step, line)]
    bounds = [step for step in prologue if step.mnemonic == 'add' and not step.operation.is_predicated
              and Ia64.split(step.text)[1] == [limit] and sorted(Ia64.split(step.text)[2]) == [START, BYTES]]
    if len(aligns) != 1:
        problems.append('%d alignments of %s to the %d-byte line into %s, expected exactly one'
                        % (len(aligns), START, LINE_BYTES, line))
    if len(bounds) != 1:
        problems.append('%d loop bounds %s=%s+%s, expected exactly one' % (len(bounds), limit, START, BYTES))
    extra = [step for step in prologue if step not in aligns and step not in bounds]
    problems += ['%s precedes the loop but is neither its bound nor its alignment' % _label(step) for step in extra]
    if len(aligns) != 1 or len(bounds) != 1:
        return None
    return FlushRoles(bounds[0], aligns[0], head, flush, advance, branch, tail, line, limit, taken, other)


def flush_problems(code: Code) -> list[str]:
    """AsteriaIa64VtdFlushTables: the leaf rules, the flush loop of
    flush_roles, and after the loop exactly `;; sync.i;; srlz.i;; mf;;
    br.ret` (nops aside), none of them predicated, each in a later
    instruction group than the one before (so `fc` and `sync.i` are in
    different groups)."""
    problems = leaf_rule_problems(code)
    roles = flush_roles(code, problems)
    if roles is None:
        return problems
    tail = roles.tail
    if [step.mnemonic for step in tail[:-1]] != list(TAIL) or not tail[-1].operation.is_return:
        problems.append('the code after the loop is %s, expected exactly sync.i ;; srlz.i ;; mf ;; br.ret'
                        % [step.rendered for step in tail])
        return problems
    problems += ['%s after the loop is predicated' % _label(step) for step in tail if step.operation.is_predicated]
    chain = (roles.branch,) + tail
    problems += ['no instruction-group stop between %s and %s' % (_label(first), _label(second))
                 for first, second in zip(chain, chain[1:]) if not first.group < second.group]
    if not roles.flush.group < tail[0].group:
        problems.append('%s and %s share an instruction group' % (_label(roles.flush), _label(tail[0])))
    return problems


def fence_problems(code: Code) -> list[str]:
    """AsteriaIa64VtdFence: the leaf rules, and exactly an unpredicated `mf`,
    a stop, then the return."""
    problems = leaf_rule_problems(code)
    steps = code.steps
    if [step.mnemonic for step in steps[:-1]] != ['mf'] or not steps or not steps[-1].operation.is_return:
        return problems + ['the leaf is %s, expected exactly mf ;; br.ret' % [step.rendered for step in steps]]
    fence, ret = steps
    if fence.operation.is_predicated:
        problems.append('%s is predicated' % _label(fence))
    if not fence.group < ret.group:
        problems.append('no instruction-group stop between %s and %s' % (_label(fence), _label(ret)))
    return problems


PROBLEMS = {FLUSH_TABLES: flush_problems, FENCE: fence_problems}


def leaf_rule_mutants(code: Code):
    ret = code.first(lambda step: step.operation.is_return)
    yield 'register frame allocated', code.insert(0, 'alloc r2=ar.pfs,2,2,0', stop=True)
    yield 'stack pointer used', code.insert(ret, 'adds r12=-16,r12', stop=True)
    yield 'gp read', code.insert(ret, 'mov r8=r1', stop=True)
    yield 'preserved r4 written', code.insert(ret, 'mov r4=r0', stop=True)
    yield 'memory load inserted', code.insert(ret, 'ld8 r8=[r32]', stop=True)
    yield 'tp read', code.insert(ret, 'mov r8=r13', stop=True)
    yield 'PSR.i masked', code.insert(ret, 'rsm 0x4000', stop=True)
    yield 'b0 overwritten', code.insert(ret, 'mov b0=r8', stop=True)
    yield 'branch out of the leaf', code.insert(ret, 'br.few <AsteriaIa64UnexpectedTarget>', stop=True)
    yield 'relocation inside the leaf', dataclasses.replace(code, relocations=code.relocations + (
        RelocationFact(0x10, 'LTOFF22X', '.rodata', 0),))
    yield 'return predicated', code.rewrite(ret, '(p6) %s' % ret.text)
    yield 'return replaced by nop.b', code.rewrite(ret, 'nop.b 0x0')


def flush_mutants(code: Code):
    roles = flush_roles(code, [])
    sync, serialize, fence, ret = roles.tail
    yield 'sync.i dropped', code.rewrite(sync, 'nop.m 0x0')
    yield 'srlz.i dropped', code.rewrite(serialize, 'nop.m 0x0')
    yield 'mf dropped', code.rewrite(fence, 'nop.m 0x0')
    yield 'sync.i and mf exchanged', code.swap(sync, fence)
    yield 'no stop between the loop and sync.i', code.without_boundary(roles.branch)
    for first, second in zip(roles.tail, roles.tail[1:]):
        yield 'no stop between %s and %s' % (first.mnemonic, second.mnemonic), code.without_boundary(first)
    yield 'stride %d' % (2 * LINE_BYTES), code.rewrite(roles.advance, roles.advance.rendered.replace(
        '=%d,' % LINE_BYTES, '=%d,' % (2 * LINE_BYTES)))
    yield 'alignment to %d bytes' % (2 * LINE_BYTES), code.rewrite(roles.align, roles.align.rendered.replace(
        ',0,%d' % LINE_SHIFT, ',0,%d' % (LINE_SHIFT + 1)).replace('=%d,' % -LINE_BYTES, '=%d,' % (-2 * LINE_BYTES)))
    yield 'fc predicated by %s' % roles.other, code.rewrite(roles.flush, '(%s) fc %s' % (roles.other, roles.line))
    yield 'extra instruction in the tail', code.insert(ret, 'mov r8=r0', stop=True)
    yield 'sync.i predicated', code.rewrite(sync, '(%s) sync.i' % roles.taken)
    yield 'fc.i instead of fc', code.rewrite(roles.flush, roles.flush.rendered.replace('fc ', 'fc.i '))
    yield 'srlz.d instead of srlz.i', code.rewrite(serialize, 'srlz.d')
    yield 'mf.a instead of mf', code.rewrite(fence, 'mf.a')
    yield 'fc dropped', code.rewrite(roles.flush, 'nop.m 0x0')
    yield 'fc after the advance', code.swap(roles.flush, roles.advance)
    yield 'fc of the bound register', code.rewrite(roles.flush, '(%s) fc %s' % (roles.taken, roles.limit))
    yield 'signed loop test', code.rewrite(roles.head, roles.head.rendered.replace('cmp.ltu', 'cmp.lt'))
    yield 'loop test operands exchanged', code.rewrite(roles.head, 'cmp.ltu %s,%s=%s,%s' % (
        roles.taken, roles.other, roles.limit, roles.line))
    yield 'loop bound is the start alone', code.rewrite(roles.bound, 'mov %s=%s' % (roles.limit, START))
    yield 'alignment from the length', code.rewrite(roles.align, roles.align.rendered.replace(START, BYTES))
    yield 'loop branch unpredicated', code.rewrite(roles.branch, roles.branch.text)
    yield 'loop branch to the entry', code.retarget(roles.branch, 0)
    yield 'loop predicate rewritten in the body', code.insert(roles.flush, 'cmp.eq %s,%s=r0,r0' % (
        roles.taken, roles.other), stop=True)
    yield 'preserved loop predicate', code.rewrite(roles.head, 'cmp.ltu p16,%s=%s,%s' % (
        roles.other, roles.line, roles.limit)).rewrite(roles.flush, '(p16) fc %s' % roles.line).rewrite(
        roles.advance, roles.advance.rendered.replace('(p%02d)' % int(roles.taken[1:]), '(p16)')).rewrite(
        roles.branch, roles.branch.rendered.replace('(p%02d)' % int(roles.taken[1:]), '(p16)'))
    yield from leaf_rule_mutants(code)


def fence_mutants(code: Code):
    fence = code.first(lambda step: step.mnemonic == 'mf')
    yield 'mf dropped', code.rewrite(fence, 'nop.m 0x0')
    yield 'mf.a instead of mf', code.rewrite(fence, 'mf.a')
    yield 'mf predicated', code.rewrite(fence, '(p6) mf')
    yield 'no stop between mf and the return', code.without_boundary(fence)
    yield 'extra instruction', code.insert(fence, 'mov r8=r0', after=True, stop=True)
    yield from leaf_rule_mutants(code)


MUTANTS = {FLUSH_TABLES: flush_mutants, FENCE: fence_mutants}


@register
class Ia64VtdFlush(HelperProperty):
    """The VT-d table publication leaves:

    - AsteriaIa64VtdFlushTables(start=r32, bytes=r33): `add B=r32,r33` and
      `dep L=0,r32,0,5` (or the equal `and L=-32,r32`), then one natural loop
      headed by `cmp.ltu P,Q=L,B` whose body is exactly `(P) fc L`,
      `(P) adds L=32,L` and `(P) br.cond` back to the head; nothing else writes
      L, B or P; the loop leaves only by falling through its branch into
      exactly `;; sync.i;; srlz.i;; mf;; br.ret b0` (nops aside), none
      predicated, each in a later instruction group than the one before.
      So every 32-byte line of [start & ~31, start + bytes) is flushed, and
      `fc` and `sync.i` are in different groups.
    - AsteriaIa64VtdFence: exactly `mf;; br.ret b0`, unpredicated.
    - Both are leaves: no alloc, call, gp/sp/tp, RSE, memory access,
      relocation, AR/CR/PSR/predicate-file access; writes only to scratch
      registers; one unpredicated return through b0, last; no register read
      in the group that writes it."""
    GROUPS = (VTD,)

    def verify(self, facts):
        return ['%s: %s' % (name, problem) for name, code in facts.items() for problem in PROBLEMS[name](code)]

    def mutants(self, facts):
        for name, code in facts.items():
            for description, mutant in MUTANTS[name](code):
                yield '%s: %s' % (name, description), {**facts, name: mutant}

    def summary(self, facts):
        roles = flush_roles(facts[FLUSH_TABLES], [])
        return ('%s: `(%s) fc %s` on every %d-byte line of [%s & ~%d, %s + %s), then sync.i ;; srlz.i ;; mf ;; '
                'br.ret; %s: mf ;; br.ret' % (FLUSH_TABLES, roles.taken, roles.line, LINE_BYTES, START,
                                              LINE_BYTES - 1, START, BYTES, FENCE))
