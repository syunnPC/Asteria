# SPDX-License-Identifier: Apache-2.0
"""IA-64 PL3 interrupt claim `AsteriaIa64UserInterruptClaim`
(Source/Platform/Ia64/Interrupt/Ia64UserFaultEntry.S), the continuation of
the external-interrupt dispatcher for an interrupt taken at PL3.

The claim runs on bank zero with PSR.ic and PSR.i clear and the interrupted
predicates in PR. It saves PR, reads cr.ivr once and has four outcomes
besides a fail-stop: the spurious vector returns with rfi and no EOI; a timer
interrupt whose deadline is not due yet (one that pended while delivery was
masked and whose deadline the kernel has since moved) is acknowledged with
one EOI and returns with rfi, the timer left armed; a due timer interrupt
masks the timer with readback, writes one EOI, leaves the External Interrupt
vector offset in ar.k5 and enters AsteriaIa64UserInterruptEntry with the
interrupt selector in r16; a device vector
writes one EOI, leaves that offset with the vector in its low byte in ar.k5
and enters the same way, the timer left as it is. Any other vector, or a
timer vector without a deadline, masks the timer with readback, writes one
EOI and stops. The golden
Tests/Golden/Ia64/Interrupt/AsteriaIa64UserInterruptClaim.dis pins the
instructions; these properties state the rules a golden update must
keep. `Ia64UserInterruptClaimOutcomes` judges every control-flow path of the
claim symbolically (Ia64ExternalInterrupt.Trace); the timer vector, its
masked ITV value, the selector, the vector offset and the device vectors
come from the C++ headers.
"""
from __future__ import annotations

import dataclasses
import functools

from AsteriaVerify import Ia64StopViews, Ia64, Layout, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import END, EXIT, RETURN, Code
from AsteriaVerify.TargetChecks.Ia64ExternalInterrupt import (ALL_PREDICATES, SPURIOUS_VECTOR, ZERO, eoi_problems,
                                                              fail_stop_branch, leaves, means, plain_trace, show)

CLAIM = 'AsteriaIa64UserInterruptClaim'
ENTRY = 'AsteriaIa64UserInterruptEntry'
EXTERNAL_HEADER = 'Source/Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp'
USER_FAULT_HEADER = 'Source/Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp'
SAVED_PREDICATES = 'r31'
# Bank-zero r16-r31 (the claim never switches banks) and the hard-wired r0.
REGISTERS = frozenset({'r0'} | {'r%d' % number for number in range(16, 32)})
CONTROL_WRITES = frozenset({'cr.itv', 'cr.eoi'})
# Where the claim leaves the vector offset for the capture; PL3 may read it
# but not write it.
VECTOR_REGISTER = 'ar.k5'
FRAME_BASES = frozenset({'alloc', 'bsw', 'cover', 'flushrs', 'loadrs', 'invala', 'ssm', 'rsm', 'sum', 'rum'})
IVR, ITM = ('in', 'cr.ivr'), ('in', 'cr.itm')
NO_DEADLINE = ('k', (1 << 64) - 1)


@dataclasses.dataclass(frozen=True)
class Constants:
    timer_vector: int           # Ia64ExternalInterruptTimerVector
    masked_timer_vector: int    # Ia64ExternalInterruptMaskedTimerVector, the ITV value that masks it
    selector: int               # Ia64UserInterruptSelector
    vector_offset: int          # Ia64UserInterruptVectorOffset
    first_device_vector: int    # Ia64UserInterruptFirstDeviceVector
    device_vector_count: int    # Ia64UserInterruptDeviceVectorCount


@functools.lru_cache(maxsize=None)
def constants(root) -> Constants:
    external = Layout.constants(Layout.read(root / EXTERNAL_HEADER))
    user = Layout.constants(Layout.read(root / USER_FAULT_HEADER))
    names = (('Ia64ExternalInterruptTimerVector', external), ('Ia64ExternalInterruptMaskedTimerVector', external),
             ('Ia64UserInterruptSelector', user), ('Ia64UserInterruptVectorOffset', user),
             ('Ia64UserInterruptFirstDeviceVector', user), ('Ia64UserInterruptDeviceVectorCount', user))
    missing = [name for name, values in names if name not in values]
    if missing:
        raise Layout.LayoutError('the headers no longer state %s' % ', '.join(missing))
    return Constants(*(values[name] for name, values in names))


def full_restore(step) -> bool:
    """`mov pr=r31,-1`: every predicate from the entry save."""
    operation = step.operation if step is not None else None
    return (operation is not None and operation.writes == {'pr'} and operation.reads == {SAVED_PREDICATES}
            and bool(operation.immediates) and operation.immediates[-1] | 1 == ALL_PREDICATES)


def selector_load(step, selector: int) -> bool:
    return step is not None and step.rendered == 'mov r16=%d' % selector


class _ClaimProperty(Ia64StopViews.CoreCodeProperty):
    SYMBOL, IMAGES = CLAIM, True


class _ConstantsProperty(_ClaimProperty):
    """Facts: (code, the Constants of the headers)."""

    def extract(self, context, subject):
        return super().extract(context, subject), constants(context.root)

    def verify(self, facts):
        return self.judge(*facts)

    def mutants(self, facts):
        for description, changed in self.edits(*facts):
            yield description, (changed, facts[1])


@register
class Ia64UserInterruptClaimSymbol(Subjects.SymbolContract):
    """The dispatcher branches directly to the claim, so it is
    exactly one GLOBAL HIDDEN FUNC definition (never exported), bundle-aligned
    with a non-zero whole number of bundles, in an ALLOC|EXECINSTR section,
    in the objects and in the linked images."""
    SYMBOLS = (CLAIM,)
    IMAGES = True


@register
class Ia64UserInterruptClaimRelocations(_ClaimProperty):
    """The claim extent carries exactly two relocations, one for the
    due tick's entry and one for a device vector's: each PCREL21B →
    AsteriaIa64UserInterruptEntry, addend 0, in slot 2 of a bundle whose
    instruction is an unpredicated branch that is not a call. It accesses no
    data, so no data, GP-relative or LTOFF relocation exists. Checked in the
    objects and in the --emit-relocs firmware image."""

    def subjects(self, context):
        return Subjects.artifacts(context, CLAIM, images=True, image_filter=lambda elf: bool(elf.relocation_sections()))

    def extract(self, context, subject):
        code = super().extract(context, subject)
        by_address = {instruction.address: instruction for instruction in code.instructions}
        return tuple((fact, by_address.get(((code.start + fact.offset) & ~0xF) + 6 * (fact.offset & 0xF)))
                     for fact in code.relocations)

    def verify(self, facts):
        if [(fact.type, fact.target, fact.addend) for fact, _ in facts] != [('PCREL21B', ENTRY, 0)] * 2:
            return ['relocations %s, expected exactly two PCREL21B to %s'
                    % ([(fact.type, fact.target, fact.addend) for fact, _ in facts], ENTRY)]
        problems = []
        for fact, instruction in facts:
            if fact.offset & 0xF != 2:
                problems.append('a PCREL21B is not in bundle slot 2')
            operation = Ia64.parse(instruction) if instruction is not None else None
            if (operation is None or instruction.predicate or not operation.is_branch or operation.is_call
                    or operation.target is None or operation.target.symbol != ENTRY):
                problems.append('a PCREL21B does not feed an unpredicated branch to %s' % ENTRY)
        return problems

    def mutants(self, facts):
        (fact, instruction), other = facts[0], facts[1]
        yield 'PCREL21B against the user-fault entry', ((dataclasses.replace(
            fact, target='AsteriaIa64UserFaultEntry'), instruction), other)
        yield 'PCREL21B with an addend', ((dataclasses.replace(fact, addend=16), instruction), other)
        yield 'PCREL21B in slot 1', ((dataclasses.replace(fact, offset=(fact.offset & ~0xF) + 1), instruction),
                                     other)
        yield 'predicated branch', ((fact, dataclasses.replace(instruction, predicate='p06')), other)
        yield 'call instead of a branch', ((fact, dataclasses.replace(
            instruction, text='br.call.sptk.many b0=%s' % instruction.text.split(None, 1)[1])), other)
        yield 'device entry removed', facts[:1]
        yield 'extra data relocation', facts + ((Properties.RelocationFact(0x20, 'DIR64LSB', 'AsteriaWrong', 0),
                                                 None),)


@register
class Ia64UserInterruptClaimRegisters(_ClaimProperty):
    """Everything but bank-zero r16-r31 is still interrupted PL3
    state, so the claim's first instruction saves pr in r31, which is never
    written again; general register operands are only r0 and r16-r31; no
    branch or floating register is used; the only predicate registers
    written are p6 and p7, and pr is written only by the full restore
    `mov pr=r31,-1`; the only other resources written are cr.itv, cr.eoi and
    ar.k5, which carries the vector offset to the capture; no
    instruction accesses memory, switches banks, changes the register frame
    or the PSR."""

    def verify(self, code):
        if not code.steps:
            return ['empty extent']
        head = code.steps[0]
        problems = Properties.register_domain_problems(list(code.instructions), REGISTERS)
        if head.operation.reads != {'pr'} or head.operation.writes != {SAVED_PREDICATES} or head.predicate:
            problems.append('the first instruction %s does not save pr in %s' % (head.rendered, SAVED_PREDICATES))
        for step in code.steps:
            operation = step.operation
            reasons = []
            if step is not head and SAVED_PREDICATES in operation.writes:
                reasons.append('overwrites the saved pr')
            others = {name for name in operation.writes
                      if name not in REGISTERS | CONTROL_WRITES | {VECTOR_REGISTER, 'p6', 'p7'}}
            if operation.base == 'rfi':
                others -= {'ip', 'psr'}
            if others and not (others == {'pr'} and full_restore(step)):
                reasons.append('writes %s' % ', '.join(sorted(others)))
            if operation.memory is not None or operation.base == 'probe':
                reasons.append('accesses memory')
            if operation.base in FRAME_BASES:
                reasons.append('changes the bank, the register frame or the PSR')
            if reasons:
                problems.append('%s %s' % (step.describe(), ' and '.join(reasons)))
        return problems

    def mutants(self, code):
        head = code.steps[0]
        restore = code.site(full_restore)
        yield 'static register written', code.insert(head, 'mov r15=r0', after=True)
        yield 'memory load', code.insert(head, 'ld8 r16=[r17]', after=True)
        yield 'memory store', code.insert(head, 'st8 [r16]=r0', after=True)
        yield 'rotating predicate written', code.insert(head, 'cmp.eq p16,p17=r20,r0', after=True)
        yield 'bank switch', code.insert(head, 'bsw.1', after=True)
        yield 'IPSR written', code.insert(head, 'mov cr.ipsr=r16', after=True)
        yield 'exception vector register written', code.insert(head, 'mov.m ar.k4=r16', after=True)
        yield 'saved pr overwritten', code.insert(head, 'mov r31=r0', after=True)
        yield 'pr restored from another register', code.replace(restore, text='mov pr=r30,0xfffffffffffffffe')
        yield 'partial pr restore', code.replace(restore, text='mov pr=r31,0x3e')
        yield 'pr saved in static r15', code.replace(head, text='mov r15=pr')


def _serialized(code: Code, step) -> bool:
    """`step` ends its group and the next group is exactly `srlz.d`."""
    following = code.next(step)
    return (code.ends_group(step) and following is not None and following.mnemonic == 'srlz.d'
            and code.ends_group(following))


@register
class Ia64UserInterruptClaimSerialization(_ClaimProperty):
    """The destructive `mov rX=cr.ivr` is the claim's only cr.ivr
    read, unpredicated and directly after the pr save; it, every
    `mov cr.itv=` mask and every `mov cr.eoi=` end their instruction group,
    and the next group is exactly `srlz.d;;`, so the claim, the mask and the
    EOI take effect before any dependent instruction."""

    def verify(self, code):
        reads = code.all(lambda step: 'cr.ivr' in step.operation.reads)
        problems = [] if len(reads) == 1 else ['%d cr.ivr reads, expected exactly one' % len(reads)]
        if len(reads) == 1 and (reads[0].predicate or code.previous(reads[0]) is not code.steps[0]):
            problems.append('%s is predicated or does not directly follow the pr save' % reads[0].describe())
        return problems + ['%s is not followed by an instruction group of exactly srlz.d' % step.describe()
                           for step in code.steps
                           if ('cr.ivr' in step.operation.reads or CONTROL_WRITES & step.operation.writes)
                           and not _serialized(code, step)]

    def mutants(self, code):
        claim = code.site(lambda step: 'cr.ivr' in step.operation.reads)
        yield 'no stop after the cr.ivr read', code.without_boundary(claim)
        yield 'cr.ivr serialization removed', code.nop(code.next(claim))
        yield 'second cr.ivr read', code.duplicate(claim)
        yield 'cr.ivr read predicated', code.replace(claim, predicate='p06')
        eoi = code.site(lambda step: 'cr.eoi' in step.operation.writes)
        yield 'EOI serialization removed', code.nop(code.next(eoi))
        yield 'no stop after a timer mask', code.without_boundary(code.site(
            lambda step: 'cr.itv' in step.operation.writes))


@register
class Ia64UserInterruptClaimEoi(_ClaimProperty):
    """EOI accounting on every control-flow path of the claim
    (Ia64ExternalInterrupt.eoi_problems): cr.ivr is read at most once per
    path; after it, the path of the spurious vector (15) executes no
    `mov cr.eoi=`, and every other path to rfi, to the user interrupt entry
    or to a fail-stop executes exactly one. The only loops are fail-stop
    self-loops (`hint @pause` plus a branch to itself)."""

    def verify(self, code):
        return eoi_problems(code, plain_trace(code))

    def mutants(self, code):
        eois = code.all(lambda step: 'cr.eoi' in step.operation.writes)
        restores = code.all(full_restore)
        loops = [step for step in code.steps if fail_stop_branch(code, step)]
        yield 'due tick without EOI', code.nop(eois[0])
        yield 'device vector without EOI', code.nop(eois[1])
        yield 'stale tick without EOI', code.nop(eois[2])
        yield 'second EOI before the user interrupt entry', code.duplicate(eois[0])
        yield 'EOI on the spurious path', code.insert(restores[-1], 'mov cr.eoi=r0')
        yield 'fail-stop looping over its EOI', code.retarget(loops[0], code.previous(code.previous(loops[0])).bundle)


@register
class Ia64UserInterruptClaimExits(_ConstantsProperty):
    """The claim leaves only by rfi, each directly preceded by the
    full pr restore `mov pr=r31,-1`, or by its two unpredicated direct
    branches (not calls) to AsteriaIa64UserInterruptEntry, the due tick's
    and a device vector's, whose two preceding instructions are that restore
    and `mov r16=S` with S the Ia64UserInterruptSelector. No path runs past
    the end of the extent, and no call, return or indirect branch exists, so
    every other path ends in a fail-stop self-loop."""

    def judge(self, code, values):
        problems = []
        exits = []
        for position, successors in code.graph.successors.items():
            step = code.step(position)
            if RETURN in successors:
                if step.operation.base != 'rfi':
                    problems.append('%s returns' % step.describe())
                elif not full_restore(code.previous(step)):
                    problems.append('%s is not directly preceded by the full pr restore' % step.describe())
            if EXIT in successors and not Ia64StopViews.terminal(step):
                exits.append(step)
            if END in successors:
                problems.append('execution runs past the end of the claim after %s' % step.describe())
        problems += ['%s calls' % step.describe() for step in code.steps if step.operation.is_call]
        if len(exits) != 2:
            return problems + ['%d branches leave the claim, expected two to %s' % (len(exits), ENTRY)]
        for branch in exits:
            target = branch.operation.target
            if (branch.predicate or branch.operation.is_call or not leaves(branch) or target is None
                    or (target.symbol, target.offset) != (ENTRY, 0)):
                problems.append('%s is not an unpredicated branch to %s' % (branch.describe(), ENTRY))
            previous = code.previous(branch)
            earlier = code.previous(previous) if previous is not None else None
            pair = (earlier, previous)
            if not (any(map(full_restore, pair)) and any(selector_load(step, values.selector) for step in pair)):
                problems.append('%s is not directly preceded by the full pr restore and mov r16=%d'
                                % (branch.describe(), values.selector))
        return problems

    def edits(self, code, values):
        branch = code.site(lambda step: leaves(step) and step.operation.target is not None)
        rfi = code.site('rfi')
        yield 'rfi without the pr restore', code.nop(code.previous(rfi))
        yield 'user interrupt entry without its selector', code.nop(code.site('mov r16=%d' % values.selector))
        yield 'user interrupt entry with the fault selector', code.replace(
            code.site('mov r16=%d' % values.selector), text='mov r16=%d' % (values.selector - 1))
        yield 'predicated branch to the entry', code.replace(branch, predicate='p06')
        yield 'branch to the user-fault entry', code.replace(
            branch, text=branch.text.replace(ENTRY, 'AsteriaIa64UserFaultEntry'), relocation=None)
        yield 'third branch to the entry', code.duplicate(branch)
        yield 'return to the dispatcher', code.insert(rfi, 'br.ret.sptk.many b0')
        loops = [step for step in code.steps if fail_stop_branch(code, step)]
        yield 'last fail-stop falls through', code.nop(loops[-1])
        yield 'entry branch replaced by a call', code.replace(
            branch, text='br.call.sptk.many b0=%s' % branch.text.split(None, 1)[1], relocation=None)


@dataclasses.dataclass(frozen=True)
class Path:
    """One control-flow path from the claim's first instruction."""
    positions: tuple[int,...]      # The instructions executed, in order
    conditions: tuple[tuple,...]   # The branch conditions that hold on it
    end: str                        # 'rfi', 'exit' (the entry branch), 'fail-stop', 'end', 'return' or 'loop'


def paths(code: Code, trace) -> list[Path]:
    """Every path from the first instruction to rfi, a leaving branch, a
    fail-stop (its `hint @pause`) or another end; a path that would revisit
    an instruction ends as 'loop'."""
    if not code.steps:
        return []
    result, stack = [], [((code.steps[0].position,), ())]
    while stack:
        positions, conditions = stack.pop()
        position = positions[-1]
        if Properties.fail_stop(code, position):
            result.append(Path(positions, conditions, 'fail-stop'))
            continue
        successors = code.graph.successors[position]
        condition = trace.conditions.get(position)
        taken = ([(successors[0], condition), (successors[1], ('not', condition))]
                 if len(successors) == 2 and condition is not None else [(node, None) for node in successors])
        for successor, holds in taken:
            following = conditions + ((holds,) if holds is not None else ())
            if successor == RETURN:
                kind = 'rfi' if code.operations[position].base == 'rfi' else 'return'
                result.append(Path(positions, following, kind))
            elif successor in (EXIT, END):
                result.append(Path(positions, following, successor))
            elif successor in positions:
                result.append(Path(positions, following, 'loop'))
            else:
                stack.append((positions + (successor,), following))
    return sorted(result, key=lambda path: path.positions)


def _on(code: Code, path: Path, matcher) -> list[int]:
    return [position for position in path.positions if matcher(code.operations[position])]


def _holds(path: Path, relation: str, first_value: tuple, second: tuple) -> bool:
    return any(means(condition, relation, first_value, second) for condition in path.conditions)


@register
class Ia64UserInterruptClaimOutcomes(_ConstantsProperty):
    """The outcome of every control-flow path of the claim, judged on
    the symbolic conditions of its branches (V the cr.ivr value, T the
    Ia64ExternalInterruptTimerVector, M its masked ITV value
    Ia64ExternalInterruptMaskedTimerVector, D the cr.itm deadline, C the
    path's single ar.itc sample, O the Ia64UserInterruptVectorOffset, F and
    N the first device vector and their count):

    - exactly two paths enter AsteriaIa64UserInterruptEntry, each writing
      ar.k5 exactly once, after its EOI;
    - one holds V == T, D != 0, D != -1 and C >= D (due), writes O to ar.k5,
      and on it `mov cr.itv=` writes M, cr.itv is read back after it, the
      readback is compared with the written register by a branch whose
      mismatch path reaches only fail-stops, and the EOI follows that branch;
    - the other holds V >= F and V < F + N (a device vector), writes O | V to
      ar.k5, writes no cr.itv and samples no ar.itc;
    - exactly two paths end in rfi: one holds V == 15 (spurious) and writes
      neither cr.itv nor cr.eoi; the other holds V == T, D != 0, D != -1 and
      C < D (not due), writes no cr.itv and one EOI after the ar.itc sample;
    - every other path ends in a fail-stop, and each one that read cr.ivr
      without V == 15 writes cr.itv = M, reads it back and only then writes
      its EOI, so no timer source remains live."""

    def judge(self, code, values):
        trace = plain_trace(code)
        timer, masked = ('k', values.timer_vector), ('k', values.masked_timer_vector)
        found = paths(code, trace)
        problems = ['a path ends in %s after %s' % (path.end, code.step(path.positions[-1]).describe())
                    for path in found if path.end not in ('exit', 'rfi', 'fail-stop')]
        entries = [path for path in found if path.end == 'exit']
        returns = [path for path in found if path.end == 'rfi']
        if len(entries) != 2:
            problems.append('%d paths enter %s, expected two' % (len(entries), ENTRY))
        if len(returns) != 2:
            problems.append('%d paths end in rfi, expected the spurious and the not-due one' % len(returns))
        ticks = [path for path in entries if _holds(path, 'eq', IVR, timer)]
        if len(ticks) != 1:
            problems.append('%d entry paths hold cr.ivr == %#x, expected one' % (len(ticks), timer[1]))
        offset = ('k', values.vector_offset)
        for path in entries:
            problems += self._carried(code, trace, path, offset if path in ticks else ('op', 'or', offset, IVR))
            if path in ticks:
                problems += self._timer(code, path, timer, due=True)
                problems += self._entry(code, trace, path, masked)
            else:
                problems += self._device(code, path, values)
        spurious = [path for path in returns if _holds(path, 'eq', IVR, ('k', SPURIOUS_VECTOR))]
        stale = [path for path in returns if path not in spurious]
        if len(spurious) != 1:
            problems.append('%d rfi paths hold the spurious vector, expected one' % len(spurious))
        for path in spurious:
            if _on(code, path, lambda operation: operation.writes & {'cr.itv', 'cr.eoi'}):
                problems.append('the spurious path writes cr.itv or cr.eoi')
        for path in stale:
            problems += self._timer(code, path, timer, due=False)
            if _on(code, path, lambda operation: 'cr.itv' in operation.writes):
                problems.append('the not-due path writes cr.itv')
        for path in found:
            if path.end == 'fail-stop' and _on(code, path, lambda operation: 'cr.ivr' in operation.reads) \
                    and not _holds(path, 'eq', IVR, ('k', SPURIOUS_VECTOR)):
                problems += self._quiesced(code, trace, path, masked)
        return list(dict.fromkeys(problems))

    @staticmethod
    def _timer(code, path, timer, due: bool) -> list[str]:
        what = 'the entry path' if due else 'an rfi path'
        problems = [] if _holds(path, 'eq', IVR, timer) else ['%s does not hold cr.ivr == %#x' % (what, timer[1])]
        if not (_holds(path, 'ne', ITM, ZERO) and _holds(path, 'ne', ITM, NO_DEADLINE)):
            problems.append('%s does not exclude the deadlines 0 and -1' % what)
        samples = _on(code, path, lambda operation: 'ar.itc' in operation.reads)
        if len(samples) != 1:
            return problems + ['%s samples ar.itc %d times, expected once' % (what, len(samples))]
        sample = ('op', 'ar.itc', samples[0])
        if not _holds(path, 'geu' if due else 'ltu', sample, ITM):
            problems.append('%s does not hold ar.itc %s cr.itm' % (what, '>=' if due else '<'))
        eois = _on(code, path, lambda operation: 'cr.eoi' in operation.writes)
        if not due and (len(eois) != 1 or eois[0] < samples[0]):
            problems.append('the not-due path does not write one EOI after its ar.itc sample')
        return problems

    @staticmethod
    def _carried(code, trace, path, expected) -> list[str]:
        """The single ar.k5 write of an entry path: `expected`, after the EOI."""
        writes = [write for write in trace.writes_of(VECTOR_REGISTER) if write.index in path.positions]
        accepted = (expected, expected[:2] + expected[2:][::-1])
        if len(writes) != 1 or writes[0].value not in accepted:
            return ['an entry path writes ar.k5 [%s], expected once with %s'
                    % (', '.join(show(write.value) for write in writes), show(expected))]
        eois = _on(code, path, lambda operation: 'cr.eoi' in operation.writes)
        order = path.positions.index
        if len(eois) != 1 or order(writes[0].index) < order(eois[0]):
            return ['an entry path writes ar.k5 before its EOI']
        return []

    @staticmethod
    def _device(code, path, values) -> list[str]:
        first = ('k', values.first_device_vector)
        last = ('k', values.first_device_vector + values.device_vector_count)
        problems = []
        if not (_holds(path, 'geu', IVR, first) and _holds(path, 'ltu', IVR, last)):
            problems.append('the device entry path does not hold %#x <= cr.ivr < %#x' % (first[1], last[1]))
        if _on(code, path, lambda operation: 'cr.itv' in operation.writes or 'ar.itc' in operation.reads):
            problems.append('the device entry path touches the timer')
        return problems

    @staticmethod
    def _masks(code, trace, path, masked) -> tuple[list[str], int | None, int | None]:
        """(problems, the mask write, the readback) of a path that must mask the timer."""
        writes = [write for write in trace.writes_of('cr.itv') if write.index in path.positions]
        if len(writes) != 1 or writes[0].value != masked:
            return ['a path that claimed a vector writes cr.itv %s, expected once with %#x'
                    % ([write.value for write in writes], masked[1])], None, None
        order = path.positions.index
        readbacks = [position for position in _on(code, path, lambda operation: 'cr.itv' in operation.reads)
                     if order(position) > order(writes[0].index)]
        if not readbacks:
            return ['a path that claimed a vector does not read cr.itv back after its mask'], writes[0].index, None
        return [], writes[0].index, readbacks[0]

    def _quiesced(self, code, trace, path, masked) -> list[str]:
        problems, write, readback = self._masks(code, trace, path, masked)
        eois = _on(code, path, lambda operation: 'cr.eoi' in operation.writes)
        if readback is not None and eois and path.positions.index(eois[0]) < path.positions.index(readback):
            problems.append('a fail-stop path writes its EOI before the timer readback')
        return problems

    def _entry(self, code, trace, path, masked) -> list[str]:
        problems, write, readback = self._masks(code, trace, path, masked)
        if readback is None:
            return problems
        source = next(write_.source_writer for write_ in trace.writes_of('cr.itv') if write_.index == write)
        target = next(iter(code.operations[readback].writes))
        order = path.positions.index
        guard = None
        for position in path.positions[order(readback) + 1:]:
            step = code.step(position)
            inputs = trace.inputs.get(position, {})
            if step.operation.base == 'cmp' and inputs.get(target) == readback and source in [
                    writer for register, writer in inputs.items() if register != target]:
                guard = step
                break
        if guard is None:
            return problems + ['the entry path does not compare the cr.itv readback with the written mask']
        branch = code.next(guard)
        if (branch is None or branch.position not in path.positions or not branch.operation.is_branch
                or branch.predicate not in guard.operation.writes or code.target(branch) is None):
            return problems + ['the cr.itv readback compare does not guard the entry path']
        mismatch = code.graph.reachable([code.target(branch).position])
        if mismatch & {RETURN, END} or any(EXIT in code.graph.successors.get(position, ())
                and not Ia64StopViews.terminal(code.step(position)) for position in mismatch if isinstance(position, int)) \
                or order(branch.position) + 1 >= len(path.positions) \
                or path.positions[order(branch.position) + 1] == code.target(branch).position:
            problems.append('a cr.itv readback mismatch does not stop')
        eois = _on(code, path, lambda operation: 'cr.eoi' in operation.writes)
        if len(eois) != 1 or order(eois[0]) < order(branch.position):
            problems.append('the entry path does not write its EOI after the cr.itv readback check')
        return problems

    def edits(self, code, values):
        branch = code.site(lambda step: leaves(step) and step.operation.target is not None)
        due = code.site(lambda step: step.rendered.startswith('cmp.ltu') and 'r18' in step.operation.reads)
        stale = code.site(lambda step: step is due, lambda step: step.operation.is_branch)
        mask = code.site(lambda step: step is stale, lambda step: 'cr.itv' in step.operation.writes)
        readback = code.site(lambda step: step is mask, lambda step: 'cr.itv' in step.operation.reads)
        guard = code.next(readback)
        eoi = code.site(lambda step: step is mask, lambda step: 'cr.eoi' in step.operation.writes)
        target = code.target(stale)
        timer = code.site('mov r18=%d' % values.timer_vector)
        other = code.next(code.next(timer))
        quiesce_readback = code.site(lambda step: step is branch,
                                     lambda step: 'cr.itv' in step.operation.reads)
        yield 'due test inverted', code.replace(due, text='cmp.ltu p6,p7=r19,r18')
        yield 'not-due path masks the timer', code.insert(target, 'mov cr.itv=r16')
        yield 'due path without the timer mask', code.nop(mask)
        yield 'due path with an unmasked ITV', code.insert(mask, 'movl r16=%#x' % values.timer_vector)
        yield 'EOI before the timer mask', code.swap(mask, eoi)
        yield 'readback compared with itself', code.replace(guard, text='cmp.eq p7,p6=r17,r17')
        yield 'readback mismatch enters the user interrupt entry', code.retarget(
            code.next(guard), branch.bundle)
        yield 'other vectors enter the user interrupt entry', code.nop(other)
        yield 'zero deadline admitted', code.nop(code.site(lambda step: step is timer, 'cmp.eq p6,p7=r19,r0',
                                                           lambda step: step.operation.is_branch))
        yield 'timer vector as a signed byte', code.replace(timer, text='mov r18=-24')
        yield 'spurious test against vector 14', code.replace(code.site('mov r18=%d' % SPURIOUS_VECTOR),
                                                               text='mov r18=14')
        yield 'fail-stop without the timer readback', code.nop(quiesce_readback)
        low = code.site('mov r18=%d' % values.first_device_vector)
        lower = code.site(lambda step: step is low, lambda step: step.operation.is_branch)
        bound = code.site(lambda step: step is lower, lambda step: step.operation.base == 'adds')
        upper = code.site(lambda step: step is lower, lambda step: step.operation.is_branch)
        combined = code.site(lambda step: step.operation.base == 'or' and 'r20' in step.operation.reads)
        carried = code.site(lambda step: VECTOR_REGISTER in step.operation.writes)
        yield 'device vectors below the range admitted', code.nop(lower)
        yield 'device vectors above the range admitted', code.nop(upper)
        yield 'device range one vector wider', code.replace(bound, text='adds r18=%d,r18'
                                                            % (values.device_vector_count + 1))
        yield 'device vector dropped from ar.k5', code.nop(combined)
        yield 'due tick carries the vector', code.insert(carried, 'or r18=r18,r20')
        yield 'due tick without ar.k5', code.nop(carried)
        yield 'device path masks the timer', code.insert(combined, 'mov cr.itv=r16')
