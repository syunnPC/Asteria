# SPDX-License-Identifier: Apache-2.0
"""IA-64 external interrupt entry, sequence poll and external preemption.

Checked code (hand-written assembly, identical at O0, O2 and in the
firmware; symbols below): in Source/Platform/Ia64/Interrupt,
Ia64ExternalInterruptEntry.S holds the bank-0 legacy timer front end, the
P64 sequence poll, its two-argument wrapper and the poll range leaf, and
Ia64ExternalPreemptionEntry.S the sole IVT external-interrupt dispatcher,
the native capture, the restore tail and the preemption range leaf. The
dispatcher's PL3 continuation, AsteriaIa64UserInterruptClaim, is checked in
Ia64UserInterruptClaim.py.

The goldens under Tests/Golden/Ia64/Interrupt pin every instruction,
immediate and branch target. The properties here state the rules a reviewed
golden update must not break. They evaluate the decoded instructions
symbolically (`Trace`): register and memory values are expressions over the
interrupted state, so the capture and restore maps are checked as dataflow
("the frame field at offset N receives cr.iip"), with the field offsets
taken from the `static_assert(__builtin_offsetof(...) == N)` lists of the
headers instead of literals.

The dispatcher, native-entry and restore properties also run on the linked
images (the compile-check `NucleusCore.elf` and the firmware
`AsteriaBoot.elf`).
"""
from __future__ import annotations

from AsteriaVerify import Ia64StopViews

import collections
import dataclasses
import functools
import re
from typing import Callable, Iterable

from AsteriaVerify import Elf, Ia64, Layout, Mutate, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.Context import ContextError
from AsteriaVerify.ControlFlow import MARKERS, Code
from AsteriaVerify.Subjects import CodeProperty, SymbolContract

ENTRY_HEADER = 'Source/Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp'
PREEMPTION_HEADER = 'Source/Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp'
FRAME_HEADER = 'Source/Platform/Ia64/Interrupt/Ia64ExternalPreemptionFrame.hpp'

ENTRY = 'AsteriaIa64ExternalInterruptEntry'
POLL = 'AsteriaIa64PollForExternalInterrupt'
SEQUENCE_POLL = 'AsteriaIa64PollForExternalInterruptSequence'
POLL_RANGE = 'AsteriaIa64GetExternalInterruptPollCodeRangeWords'
DISPATCH = 'AsteriaIa64ExternalInterruptDispatchEntry'
NATIVE = 'AsteriaIa64ExternalPreemptionNativeEntry'
RESTORE = 'AsteriaIa64ExternalPreemptionRestoreTail'
PREEMPTION_RANGE = 'AsteriaIa64GetExternalPreemptionCodeRangeWords'
USER_CLAIM = 'AsteriaIa64UserInterruptClaim'
# The dispatcher's continuations: native capture, legacy evidence entry and
# the claim of an interrupt taken at PL3.
CONTINUATIONS = (NATIVE, ENTRY, USER_CLAIM)

# Architectural constants (IA-64 SDM), not layout.
SPURIOUS_VECTOR = 15            # The architected spurious external-interrupt vector
IFS_VALID_BIT = 63              # IFS.v
IPSR_CPL = ('op', 'extr.u', ('in', 'cr.ipsr'), ('k', 32), ('k', 2))     # IPSR.cpl, bits 32-33
USER_PRIVILEGE = ('k', 3)
PSR_I_BIT = Ia64.PSR_I.bit_length() - 1
ALL_PREDICATES = (1 << 64) - 1  # `mov pr=rX,-1`; p0 is hard-wired, so bit 0 may be clear


# Navigation -------------------------------------------------------------------

def find(code: Code, predicate: Callable[[Ia64.Operation], bool]) -> list[int]:
    return [step.position for step in code.steps if predicate(step.operation)]


def first(items: Iterable, what: str):
    for item in items:
        return item
    raise LookupError('cannot locate %s for a self-test mutant' % what)


def leaves(step) -> bool:
    return step.operation.is_branch and not step.operation.is_call and step.target is None and not Ia64StopViews.terminal(step)


def fail_stop_branch(code: Code, at) -> bool:
    previous = code.previous(at)
    return previous is not None and Properties.fail_stop(code, previous) and code.next(previous) is code.step(at)


# Operand rewrites (mutants) ------------------------------------------------------

def _operands(code: Code, position: int, change) -> Code:
    mnemonic, destinations, sources = change(*Ia64.split(code.instructions[position].text))
    return code.replace(position, text=Ia64.render(mnemonic, destinations, sources), relocation=None)


def rewrite_operand(code: Code, position: int, old: str, new: str) -> Code:
    return _operands(code, position, lambda mnemonic, destinations, sources: (
        mnemonic, [new if token == old else token for token in destinations],
        [new if token == old else token for token in sources]))


def rewrite_mnemonic(code: Code, position: int, mnemonic: str) -> Code:
    return _operands(code, position, lambda _, destinations, sources: (mnemonic, destinations, sources))


def shift_immediate(code: Code, position: int, delta: int) -> Code:
    """Adds `delta` to the first numeric source operand (an `adds` offset)."""
    def change(mnemonic, destinations, sources):
        index = first((index for index, token in enumerate(sources) if _IMMEDIATE.match(token)), 'an immediate')
        return mnemonic, destinations, sources[:index] + [str(int(sources[index], 0) + delta)] + sources[index + 1:]
    return _operands(code, position, change)


def set_increment(code: Code, position: int, increment: int) -> Code:
    """Rewrites the post-increment of a load or store."""
    return _operands(code, position, lambda mnemonic, destinations, sources: (
        mnemonic, destinations, sources[:-1] + [str(increment)]))


def _source(code: Code, index: int) -> str:
    return Ia64.split(code.instructions[index].text)[2][0]


def source_of(code: Code, position: int, old: int, new: int) -> Code:
    """Instruction `position` with the first source of instruction `old`
    replaced by the first source of instruction `new`."""
    return rewrite_operand(code, position, _source(code, old), _source(code, new))


# Symbolic evaluation ------------------------------------------------------------

ZERO = ('k', 0)


def plus(value: tuple, offset: int) -> tuple:
    """value + offset, normalized to ('add', base, offset)."""
    if offset == 0:
        return value
    if value[0] == 'k':
        return ('k', value[1] + offset)
    if value[0] == 'add':
        total = value[2] + offset
        return value[1] if total == 0 else ('add', value[1], total)
    return ('add', value, offset)


def load(address: tuple) -> tuple:
    return ('mem', address)


_GR = re.compile(r'^r(\d+)$')
_IMMEDIATE = re.compile(r'^-?(?:0x[0-9a-f]+|\d+)$')
_RESOURCE = re.compile(r'^(?:ar\.\w+|cr\.\w+|psr(?:\.\w+)?|pr|b[0-7]|f\d+|p\d+)$')
NEGATED = {'eq': 'ne', 'ne': 'eq', 'ltu': 'geu', 'geu': 'ltu', 'lt': 'ge', 'ge': 'lt',
           'gtu': 'leu', 'leu': 'gtu', 'gt': 'le', 'le': 'gt'}
SCRATCH_STATIC = ('r2', 'r3', 'r8', 'r9', 'r10', 'r11', 'r14', 'r15')


@functools.lru_cache(maxsize=None)
def _split(text: str) -> tuple[str, tuple[str,...], tuple[str,...]]:
    mnemonic, destinations, sources = Ia64.split(text)
    return mnemonic, tuple(destinations), tuple(sources)


@dataclasses.dataclass(frozen=True)
class Access:
    """One memory access of a trace."""
    index: int
    mnemonic: str
    kind: str                   # 'load', 'store' or 'atomic'
    address: tuple
    value: tuple                # The stored value, or the loaded value
    register: str               # Physical data register ('' when none)
    predicated: bool
    base_writer: int | None     # Instruction that last wrote the address register
    data_writer: int | None     # Instruction that last wrote the stored register
    unat: tuple | None          # Ar.unat when the access is a GR spill or fill

    @property
    def completers(self) -> tuple[str,...]:
        return tuple(self.mnemonic.split('.')[1:])


@dataclasses.dataclass(frozen=True)
class Write:
    """One write of an application, control, branch or predicate resource."""
    index: int
    resource: str
    value: tuple
    source_writer: int | None


@dataclasses.dataclass(frozen=True)
class Branch:
    index: int
    condition: tuple | None     # Taken when this holds; None if unpredicated
    call: bool
    via_value: tuple | None     # Value of the branch register of an indirect branch
    state: tuple                # (register, value) pairs at a call


def physical(register: str, bank: int) -> str:
    number = int(register[1:])
    return '%s#%d' % (register, bank) if 16 <= number <= 31 else register


class Trace:
    """Straight-line symbolic evaluation of an extent in instruction order.

    Values are hashable tuples: ('k', n) constants, ('in', name) the state
    at entry (interrupted registers and resources), ('sym', text) relocated
    or symbolized immediates, ('mem', address) loads, ('add', v, n) sums,
    ('covered', name) cr.ifs/ar.bsp after `cover`, ('unat', base, regs)
    ar.unat collections and ('op',...) any other result. General registers
    r16-r31 are tracked per bank (`r16#0`, `r16#1`); `bsw` switches banks.
    A predicated instruction's result is opaque. Branches do not redirect
    the evaluation; code after an unconditional branch sees the state of the
    preceding instructions, which suits the forward-exit style of these
    handlers. A call clobbers the scratch registers and makes sp and ar.bsp
    unknown, so a post-call validation compares them with their baselines.
    """

    def __init__(self, code: Code, *, registers: dict[str, tuple] | None = None, bank: int = 0):
        self.bank = bank
        self.gr: dict[str, tuple] = dict(registers or {})
        self.res: dict[str, tuple] = {}
        self.writer: dict[str, int] = {}
        self.accesses: list[Access] = []
        self.writes: list[Write] = []
        self.branches: list[Branch] = []
        self.conditions: dict[int, tuple] = {}
        self.compares: list[tuple[int, tuple]] = []
        self.inputs: dict[int, dict[str, int | None]] = {}
        for index, operation in enumerate(code.operations):
            self.index = index
            if not operation.is_nop:
                self._step(index, operation, code.instructions[index].text)

    def gr_name(self, register: str) -> str:
        return physical(register, self.bank)

    def value(self, token: str) -> tuple:
        if token == 'r0':
            return ZERO
        if _GR.match(token):
            name = self.gr_name(token)
            return self.gr.get(name, ('in', name))
        if _IMMEDIATE.match(token):
            return ('k', int(token, 0))
        if token.startswith('<'):
            return ('sym', token[1:-1])
        if token == 'ar.itc':
            return ('op', 'ar.itc', self.index)
        if _RESOURCE.match(token):
            return self.res.get(token, ('in', token))
        return ('?', token)

    def source_writer(self, token: str) -> int | None:
        return self.writer.get(self.gr_name(token)) if _GR.match(token) else self.writer.get(token)

    def set(self, token: str, value: tuple) -> None:
        if token in ('r0', 'p0'):
            return
        name = self.gr_name(token) if _GR.match(token) else token
        (self.gr if _GR.match(token) else self.res)[name] = value
        self.writer[name] = self.index

    def _step(self, index: int, operation: Ia64.Operation, text: str) -> None:
        base = operation.base
        mnemonic, destinations, sources = _split(text)
        predicated = operation.is_predicated
        tokens = [token.strip('[]') for token in sources] + [token[1:-1] for token in destinations
                                                               if token.startswith('[')]
        self.inputs[index] = {token: self.source_writer(token) for token in tokens if _GR.match(token)}
        if base in ('br', 'brl'):
            self._branch(index, operation, sources)
        elif base == 'bsw':
            self.bank = int(operation.completers[0])
        elif base == 'cover':
            self.res['cr.ifs'] = ('covered', 'cr.ifs')
            self.res['ar.bsp'] = ('covered', 'ar.bsp')
        elif base in ('ssm', 'rsm', 'sum', 'rum'):
            self.writes.append(Write(index, 'psr', ('op', base, self.value(sources[0])), None))
            self.res['psr'] = ('op', 'psr', index)
        elif base == 'loadrs':
            for name in ('ar.bsp', 'ar.bspstore', 'ar.rnat'):
                self.res[name] = ('op', 'after-loadrs', name, index)
        elif base.startswith(('ld', 'st')):
            self._memory(index, operation, mnemonic, destinations, sources, predicated)
        elif base in Ia64.ATOMIC_BASES:
            address = self.value(sources[0][1:-1])
            self.accesses.append(Access(index, mnemonic, 'atomic', address,
                                        tuple(self.value(token) for token in sources[1:]), '', predicated,
                                        self.source_writer(sources[0][1:-1]), None, None))
            self.set(destinations[0], ('op', 'atomic', index))
        elif base == 'alloc':
            self.set(destinations[0], self.value('ar.pfs'))
            for name in [name for name in self.gr if _GR.match(name) and int(name[1:]) >= 32]:
                del self.gr[name]
        elif base in ('cmp', 'cmp4', 'tbit', 'tnat'):
            self._compare(index, operation, destinations, sources)
        elif destinations and base in ('mov', 'movl'):
            self._move(index, destinations[0], sources, predicated)
        elif destinations:
            self.set(destinations[0], self._result(index, mnemonic, base, sources, predicated))

    def _move(self, index, destination, sources, predicated) -> None:
        value = ('op', 'predicated', index) if predicated else self.value(sources[0])
        if destination == 'pr':
            mask = self.value(sources[1]) if len(sources) > 1 else ('k', ALL_PREDICATES)
            self.writes.append(Write(index, 'pr', ('pr', value, mask), self.source_writer(sources[0])))
            self.res['pr'] = ('op', 'pr', index)
        elif _GR.match(destination):
            self.set(destination, value)
        else:
            self.writes.append(Write(index, destination, value, self.source_writer(sources[0])))
            if destination == 'ar.bspstore':
                self.res['ar.bsp'] = ('op', 'bsp-after-bspstore', index)
            self.set(destination, value)

    def _result(self, index, mnemonic, base, sources, predicated) -> tuple:
        values = [self.value(token) for token in sources]
        if predicated:
            return ('op', 'predicated', index)
        if base in ('add', 'adds', 'addl') and len(values) == 2:
            first_value, second = values
            if first_value[0] == 'k':
                return plus(second, first_value[1])
            return plus(first_value, second[1]) if second[0] == 'k' else ('op', 'add', first_value, second)
        if base == 'sub' and len(values) == 2 and values[1][0] == 'k':
            return plus(values[0], -values[1][1])
        return ('op', mnemonic) + tuple(values)

    def _memory(self, index, operation, mnemonic, destinations, sources, predicated) -> None:
        store = operation.base.startswith('st')
        base_register = (destinations[0] if store else sources[0])[1:-1]
        address = self.value(base_register)
        data = sources[0] if store else destinations[0]
        register = self.gr_name(data) if _GR.match(data) else data
        unat = None
        if store:
            value, writer = self.value(data), self.source_writer(data)
            if 'spill' in operation.completers and _GR.match(data):
                # Ar.unat: its value when last written plus the registers whose NaT bits it
                # collected since
                unat = self.res.get('ar.unat', ('in', 'ar.unat'))
                base, collected = (unat[1], unat[2]) if unat[0] == 'unat' else (unat, ())
                self.res['ar.unat'] = ('unat', base, collected + (register,))
        else:
            value, writer = load(address), None
            if 'fill' in operation.completers:
                unat = self.res.get('ar.unat', ('in', 'ar.unat'))
        self.accesses.append(Access(index, mnemonic, 'store' if store else 'load', address, value, register,
                                    predicated, self.source_writer(base_register), writer, unat))
        if len(sources) > 1:
            increment = self.value(sources[1])
            self.set(base_register, plus(address, increment[1]) if increment[0] == 'k'
                     else ('op', 'add', address, increment))
        if not store:
            self.set(data, ('op', 'predicated', index) if predicated else value)

    def _compare(self, index, operation, destinations, sources) -> None:
        relation = operation.completers[0] if operation.completers else 'eq'
        values = tuple(self.value(token) for token in sources)
        condition, negated = ('cmp', relation) + values, ('not', ('cmp', relation) + values)
        if operation.is_predicated:
            guard = self.res.get(operation.predicate, ('in', operation.predicate))
            unconditional = 'unc' in operation.completers
            condition = ('and', guard, condition) if unconditional else ('op', 'predicated', index)
            negated = ('and', guard, negated) if unconditional else ('op', 'predicated', index)
        if destinations:
            self.set(destinations[0], condition)
        if len(destinations) > 1:
            self.set(destinations[1], negated)
        self.compares.append((index, condition))
        self.res['pr'] = ('op', 'pr', index)

    def _branch(self, index, operation, sources) -> None:
        condition = None
        if operation.is_predicated:
            condition = self.res.get(operation.predicate, ('in', operation.predicate))
            self.conditions[index] = condition
        via_value = self.value(sources[0] if sources else '') if operation.target is None and not operation.is_return \
            else None
        state: tuple = ()
        if operation.is_call:
            state = tuple((name, self.value(name)) for name in ('r1', 'r12', 'r32', 'r33', 'ar.bsp'))
            for name in SCRATCH_STATIC + tuple('r%d' % n for n in range(16, 32)):
                self.set(name, ('op', 'clobbered', name, index))
            self.set('r8', ('op', 'call-result', index))
            self.gr['r12'] = ('op', 'after-call', 'r12', index)
            self.res['ar.bsp'] = ('op', 'after-call', 'ar.bsp', index)
            for name in ('b0', 'b6', 'b7'):
                self.res[name] = ('op', 'clobbered', name, index)
        self.branches.append(Branch(index, condition, operation.is_call, via_value, state))

    def _accesses(self, kind: str, completer: str | None, address: tuple | None) -> list[Access]:
        return [access for access in self.accesses if access.kind == kind and (
            completer is None or completer in access.completers) and (address is None or access.address == address)]

    def stores(self, completer: str | None = None, address: tuple | None = None) -> list[Access]:
        return self._accesses('store', completer, address)

    def loads(self, completer: str | None = None, address: tuple | None = None) -> list[Access]:
        return self._accesses('load', completer, address)

    def writes_of(self, resource: str) -> list[Write]:
        return [write for write in self.writes if write.resource == resource]

    def at(self, index: int | None) -> Access:
        """The memory access of instruction `index`."""
        return next(access for access in self.accesses if access.index == index)

    def store_of(self, value: tuple) -> Access:
        """The first store of `value`."""
        return next(access for access in self.stores() if access.value == value)


def means(condition: tuple | None, relation: str, first_value: tuple, second: tuple) -> bool:
    """Whether the condition is `first relation second` (eq/ne symmetric)."""
    negate = False
    while condition is not None and condition[0] == 'not':
        negate, condition = not negate, condition[1]
    if condition is None or condition[0] != 'cmp':
        return False
    if (NEGATED.get(condition[1], 'not-' + condition[1]) if negate else condition[1]) != relation:
        return False
    if relation in ('eq', 'ne'):
        return {condition[2], condition[3]} == {first_value, second}
    return (condition[2], condition[3]) == (first_value, second)


def failure_conditions(trace: Trace, code: Code) -> list[tuple[int, tuple]]:
    """(branch index, condition) of every predicated branch into a fail-stop."""
    return [(branch.index, branch.condition) for branch in trace.branches if branch.condition is not None
            and (target := code.target(branch.index)) is not None and Properties.fail_stop(code, target)]


def fails_when(guards: list[tuple[int, tuple]], relation: str, first_value: tuple, second: tuple) -> bool:
    return any(means(condition, relation, first_value, second) for _, condition in guards)


def involves(value, leaf: tuple) -> bool:
    """Whether the symbolic value is, or is computed from, `leaf`."""
    return value == leaf or (isinstance(value, tuple)
                             and any(isinstance(item, tuple) and involves(item, leaf) for item in value))


# Shared control-flow judgments ---------------------------------------------------

def eoi_problems(code: Code, trace: Trace) -> list[str]:
    """EOI accounting on every control-flow path.

    A path that ends before reading cr.ivr executes no `mov cr.eoi=`; a path
    that claimed a vector executes exactly one EOI before it ends (rfi,
    return, leaving the extent, or a fail-stop), except after the spurious
    vector (15), which executes none. cr.ivr is read at most once per path,
    and the only loops are fail-stop self-loops: every backward branch is
    `hint @pause` plus an unconditional branch to it."""
    problems = ['%s (#%d) branches back to %s (#%d) outside a fail-stop self-loop'
                % (step.operation.text, step.position, target.operation.text, target.position)
                for step in code.steps if step.operation.is_branch and not step.operation.is_call
                and (target := code.target(step)) is not None and target.position <= step.position
                and not fail_stop_branch(code, step)]
    ivr_reads = {step.position for step in code.steps if 'cr.ivr' in step.operation.reads}
    eois = {step.position for step in code.steps if 'cr.eoi' in step.operation.writes}
    if not ivr_reads:
        problems.append('no cr.ivr read')
    if not code.steps:
        return problems + ['empty extent']
    spurious, vector = ('k', SPURIOUS_VECTOR), ('in', 'cr.ivr')
    verdicts: dict[str, None] = {}
    stack, seen = [(code.steps[0].position, 0, 0, False)], set()
    while stack:
        position, reads, count, is_spurious = stack.pop()
        reads, count = reads + (position in ivr_reads), count + (position in eois)
        key = (position, min(reads, 2), min(count, 2), is_spurious)
        if key in seen:
            continue
        seen.add(key)
        # (successor, the condition under which it is taken): a branch predicated on c is taken when c holds
        stopped, successors = Properties.fail_stop(code, position), code.graph.successors[position]
        condition = trace.conditions.get(position)
        successors = ([(successors[0], condition), (successors[1], ('not', condition))] if len(successors) == 2
                      and condition is not None else [(successor, None) for successor in successors])
        if stopped or any(successor in MARKERS for successor, _ in successors):
            where = '%s (#%d)' % (code.operations[position].text, position)
            if reads > 1:
                verdicts['%s is reached after reading cr.ivr %d times' % (where, reads)] = None
            elif reads == 0 and count:
                verdicts['%s is reached after %d EOIs without a cr.ivr read' % (where, count)] = None
            elif reads and is_spurious and count:
                verdicts['%s is reached after the spurious vector with %d EOIs' % (where, count)] = None
            elif reads and not is_spurious and count != 1:
                verdicts['%s is reached after claiming a vector with %d EOIs, expected exactly one'
                         % (where, count)] = None
        if not stopped:
            stack.extend((successor, min(reads, 2), min(count, 2),
                          is_spurious or means(condition, 'eq', vector, spurious))
                         for successor, condition in successors if successor not in MARKERS)
    return problems + list(verdicts)


# Layouts and entry states --------------------------------------------------------

CONTROL = ('sym', 'control')
RESTORE_FRAME = ('sym', 'frame')
# The raw IVT-tail contract between the dispatcher and the native entry
# (register -> architectural value captured before any other instruction).
DISPATCH_CARRIERS = (('r31', 'pr'), ('r30', 'psr'), ('r29', 'cr.iip'), ('r28', 'cr.ipsr'), ('r27', 'cr.ifs'),
                     ('r26', 'cr.isr'), ('r20', 'ar.ccv'))
CONTROL_CARRIER = 'r21'
# The restore tail's raw contract: r4/r5 frame/control, r6/r7 committed
# generations, entered from the bridge in bank 1.
RESTORE_REGISTERS = {'r4': RESTORE_FRAME, 'r5': CONTROL, 'r6': ('sym', 'frame-token'),
                     'r7': ('sym', 'idle-generation')}

EVIDENCE_FIELDS = ('Sequence', 'AcknowledgedSequence', 'InterruptedInstructionPointer', 'InterruptedProcessorStatus')
CONTROL_FIELDS = ('State', 'FrameAddress', 'CoordinatorMemoryStackEnd', 'CoordinatorEntryPoint',
                  'CoordinatorGlobalPointer', 'CoordinatorArgument')
# Frame field -> the interrupted architectural value the native entry stores
# there before `cover` (BranchRegisters[i] <- bi is added separately).
CAPTURE_MAP = (
    ('CurrentProcessorStatus', 'psr'), ('InterruptedInstructionPointer', 'cr.iip'),
    ('InterruptedProcessorStatus', 'cr.ipsr'), ('HardwareInterruptionFunctionState', 'cr.ifs'),
    ('InterruptionStatus', 'cr.isr'), ('InterruptionFaultingAddress', 'cr.ifa'),
    ('InterruptionTranslationInformation', 'cr.itir'), ('InterruptionPreviousInstructionPointer', 'cr.iipa'),
    ('InterruptionImmediate', 'cr.iim'), ('InterruptionHashAddress', 'cr.iha'),
    ('PreviousFunctionState', 'ar.pfs'), ('RseConfiguration', 'ar.rsc'), ('RseBackingStorePointer', 'ar.bsp'),
    ('RseBackingStoreStorePointer', 'ar.bspstore'), ('RseNatCollection', 'ar.rnat'),
    ('OriginalNatCollection', 'ar.unat'), ('LoopCounter', 'ar.lc'), ('EpilogCounter', 'ar.ec'),
    ('CompareAndStoreData', 'ar.csd'), ('CompareExchangeValue', 'ar.ccv'), ('FloatingPointStatus', 'ar.fpsr'),
    ('Predicates', 'pr'),
)
# Resource -> frame field its final write before rfi must come from.
RESTORE_MAP = (
    ('ar.lc', 'LoopCounter'), ('ar.ec', 'EpilogCounter'), ('ar.csd', 'CompareAndStoreData'),
    ('ar.ccv', 'CompareExchangeValue'), ('ar.fpsr', 'FloatingPointStatus'), ('ar.pfs', 'PreviousFunctionState'),
    ('ar.bspstore', 'RseBackingStoreStorePointer'), ('ar.rnat', 'RseNatCollection'),
    ('ar.rsc', 'RseConfiguration'), ('ar.unat', 'OriginalNatCollection'),
    ('cr.ifs', 'CoveredInterruptionFunctionState'), ('cr.iip', 'InterruptedInstructionPointer'),
)
BRANCH_REGISTERS = 8
FRAME_FIELDS = tuple(sorted({name for name, _ in CAPTURE_MAP} | {name for _, name in RESTORE_MAP} | {
    'PublishedToken', 'BranchRegisters', 'GeneralRegisters', 'FloatingRegisters',
    'StaticGeneralRegisterSpillNatCollection', 'BankedGeneralRegisterSpillNatCollection',
    'CoveredRseBackingStorePointer', 'RseDirtyPartitionLoadConfiguration'}))

# Asserted struct offsets (field -> offset) of the frame, control and evidence.
Offsets = collections.namedtuple('Offsets', 'frame control evidence', defaults=({}, {}, {}))


@functools.lru_cache(maxsize=None)
def _offsets(root, header: str, struct: str, fields: tuple[str,...]) -> dict[str, int]:
    return Layout.offsets(Layout.read(root / header), struct, fields)


def preemption_layout(context) -> Offsets:
    return Offsets(_offsets(context.root, FRAME_HEADER, 'Ia64ExternalPreemptionFrame', FRAME_FIELDS),
                   _offsets(context.root, PREEMPTION_HEADER, 'Ia64ExternalPreemptionControl', CONTROL_FIELDS))


def evidence_layout(context) -> Offsets:
    return Offsets(evidence=_offsets(context.root, ENTRY_HEADER, 'Ia64LocalTimerInterruptEvidence',
                                     EVIDENCE_FIELDS))


def control_field(layout: Offsets, name: str) -> tuple:
    return plus(CONTROL, layout.control[name])


def native_frame(layout: Offsets) -> tuple:
    return load(control_field(layout, 'FrameAddress'))


def restore_field(layout: Offsets, name: str, offset: int = 0) -> tuple:
    """The address of a frame field in the restore tail (frame in r4)."""
    return plus(RESTORE_FRAME, layout.frame[name] + offset)


@functools.lru_cache(maxsize=16)
def native_trace(code: Code) -> Trace:
    registers = {physical(register, 0): ('in', source) for register, source in DISPATCH_CARRIERS}
    registers[physical(CONTROL_CARRIER, 0)] = CONTROL
    return Trace(code, registers=registers, bank=0)


@functools.lru_cache(maxsize=16)
def restore_trace(code: Code) -> Trace:
    return Trace(code, registers=RESTORE_REGISTERS, bank=1)


@functools.lru_cache(maxsize=16)
def plain_trace(code: Code) -> Trace:
    return Trace(code)


def events(code: Code, base: str) -> list[int]:
    return find(code, lambda operation: operation.base == base)


def show(value: tuple) -> str:
    """Compact rendering of a symbolic value for messages."""
    kind = value[0]
    if kind == 'k':
        return '%#x' % value[1]
    if kind in ('in', 'sym'):
        return value[1]
    if kind == 'covered':
        return 'covered %s' % value[1]
    if kind == 'add':
        return '%s%+#x' % (show(value[1]), value[2])
    if kind == 'mem':
        return '[%s]' % show(value[1])
    if kind == 'unat':
        return 'unat(%s; %s)' % (show(value[1]), ','.join(value[2]) if len(value[2]) < 4 else '%s..%s' % (
            value[2][0], value[2][-1]))
    return '%s(%s)' % (value[1], ','.join(show(item) if isinstance(item, tuple) else str(item)
                                         for item in value[2:]))


# Base classes ---------------------------------------------------------------------

class EntryProperty(Ia64StopViews.CoreCodeProperty):
    SYMBOL = ENTRY


class PollProperty(Ia64StopViews.CoreCodeProperty):
    SYMBOL = SEQUENCE_POLL


class DispatchProperty(Ia64StopViews.CoreCodeProperty):
    SYMBOL, IMAGES = DISPATCH, True


class TraceProperty(Ia64StopViews.CoreCodeProperty):
    """Facts: (code, the asserted offsets LAYOUT reads). `judge` and `edits`
    also get the symbolic TRACE of the code; every edited code keeps the
    layout."""

    def extract(self, context, subject):
        return super().extract(context, subject), self.LAYOUT(context)

    def verify(self, facts):
        return self.judge(facts[0], facts[1], self.TRACE(facts[0]))

    def mutants(self, facts):
        for description, changed in self.edits(facts[0], facts[1], self.TRACE(facts[0])):
            yield description, (changed, facts[1])


class EvidenceProperty(TraceProperty):
    SYMBOL = ENTRY
    LAYOUT, TRACE = staticmethod(evidence_layout), staticmethod(plain_trace)


class NativeProperty(TraceProperty):
    SYMBOL, IMAGES = NATIVE, True
    LAYOUT, TRACE = staticmethod(preemption_layout), staticmethod(native_trace)


class RestoreProperty(TraceProperty):
    SYMBOL, IMAGES = RESTORE, True
    LAYOUT, TRACE = staticmethod(preemption_layout), staticmethod(restore_trace)


# ELF symbol contracts ------------------------------------------------------------

class _Symbols(SymbolContract):
    """Each symbol is defined once as a GLOBAL HIDDEN FUNC with a non-zero,
    bundle-granular extent in an ALLOC+EXECINSTR section, and all of them lie
    in one section, so IVT and bridge branches (PCREL21B) and C++ calls bind
    inside the image without interposition or GP/PLT indirection."""
    SAME_SECTION = True


@register
class Ia64ExternalInterruptSymbols(_Symbols):
    __doc__ = _Symbols.__doc__ + """

    Symbols: the legacy entry, the wrapper and sequence polls, and the poll
    range leaf."""
    SYMBOLS = (ENTRY, POLL, SEQUENCE_POLL, POLL_RANGE)


@register
class Ia64ExternalPreemptionSymbols(_Symbols):
    __doc__ = _Symbols.__doc__ + """

    Symbols: the dispatcher, the native entry, the restore tail and the
    preemption range leaf."""
    SYMBOLS = (DISPATCH, NATIVE, RESTORE, PREEMPTION_RANGE)


# Code ranges ---------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class RangeWord:
    """A relocation inside a range leaf, resolved to (section, value)."""
    offset: int
    type: str
    section: int
    value: int
    label: str


def definition(elf: Elf.ElfFile, name: str) -> Elf.Symbol:
    definitions = elf.definitions(name)
    if len(definitions) != 1:
        raise ContextError('%s defines %s %d times' % (elf.path, name, len(definitions)))
    return definitions[0]


class _CodeRange(Ia64StopViews.CoreCodeProperty):
    """Facts: the definitions of FUNCTIONS (the published code, in address
    order) and of OTHER, and the relocations of the range leaf SYMBOL. The
    functions are bundle-aligned and contiguous in one section, and the leaf
    materializes exactly two IMM64 words: the start of the first function and
    the end of the last."""
    FUNCTIONS: tuple[str,...] = ()
    OTHER = ''

    def extract(self, context, subject):
        elf = Elf.load(subject.path)
        leaf = definition(elf, self.SYMBOL)
        words = tuple(RangeWord(fact.offset, fact.type, *fact.resolved, fact.target or 'absolute') for fact in
                      Properties.relocations_in_extent(elf, leaf.section_index, leaf.value, leaf.value + leaf.size))
        return tuple(definition(elf, name) for name in self.FUNCTIONS + (self.OTHER,)), words

    def verify(self, facts):
        definitions, words = facts
        functions, start, end = definitions[:-1], definitions[0], definitions[-2]
        problems = ['%s [%#x, +%#x) is not bundle-aligned' % (function.name, function.value, function.size)
                    for function in functions if function.value % 16 or function.size % 16 or not function.size]
        if len({function.section_index for function in functions}) != 1:
            problems.append('%s lie in different sections' % ', '.join(self.FUNCTIONS))
        problems += ['%s ends at %#x but %s starts at %#x' % (before.name, before.value + before.size, after.name,
                                                              after.value)
                     for before, after in zip(functions, functions[1:]) if before.value + before.size != after.value]
        if [word.type for word in words] != ['IMM64', 'IMM64']:
            return problems + ['%s holds relocations %s, expected exactly two IMM64 (start, end)'
                               % (self.SYMBOL, [(word.type, word.label) for word in words])]
        if (words[0].section, words[0].value) != (start.section_index, start.value):
            problems.append('%s publishes start %s, expected %s' % (self.SYMBOL, words[0].label, start.name))
        if (words[1].section, words[1].value) != (end.section_index, end.value + end.size):
            problems.append('%s publishes end %s, expected the end of %s' % (self.SYMBOL, words[1].label, end.name))
        return problems

    def mutants(self, facts):
        definitions, words = facts
        start, end, other = definitions[0], definitions[-2], definitions[-1]
        yield 'end shifted by a bundle', (definitions, words[:1] + (dataclasses.replace(
            words[1], value=words[1].value + 16, label=words[1].label + '+0x10'),))
        yield 'start at %s' % other.name, (definitions, (dataclasses.replace(
            words[0], section=other.section_index, value=other.value, label=other.name),) + words[1:])
        yield 'first function of a partial bundle', (Mutate.replace_at(definitions, 0, size=start.size + 8), words)
        yield 'last function one bundle late', (definitions[:-2] + (dataclasses.replace(end, value=end.value + 16),
                                                                    other), words)
        yield 'missing start word', (definitions, words[1:])
        yield 'missing end word', (definitions, words[:1])


@register
class Ia64ExternalInterruptPollRange(_CodeRange):
    """`GetIa64ExternalInterruptPollCodeRange` publishes exactly the
    sequence poll: the range leaf's two IMM64 relocations resolve to
    AsteriaIa64PollForExternalInterruptSequence+0 and to its end
    (`.Lasteria_ia64_poll_code_end`, compared as a section offset), and the
    sequence poll starts and ends on bundle boundaries. The BSP/AP runtimes
    accept an interrupted IIP in the timer evidence only inside this range."""
    SYMBOL, FUNCTIONS, OTHER = POLL_RANGE, (SEQUENCE_POLL,), POLL


@register
class Ia64ExternalPreemptionCodeRange(_CodeRange):
    """`GetIa64ExternalPreemptionCodeRange` publishes exactly the
    PSR.ic-off corridor that needs locked instruction-translation coverage:
    the dispatcher, the native entry and the restore tail are contiguous in
    one section in that order, and the range leaf's two IMM64 relocations
    resolve to the dispatcher's start and to the restore tail's end
    (`.Lasteria_ia64_external_preemption_code_end`, a section offset)."""
    SYMBOL, FUNCTIONS, OTHER = PREEMPTION_RANGE, (DISPATCH, NATIVE, RESTORE), NATIVE


# Relocation-free leaves ----------------------------------------------------------

@register
class Ia64ExternalInterruptRelocations(Subjects.HelperRelocations):
    def extract(self, context, subject):
        facts = super().extract(context, subject)
        return {name: (tuple(fact for fact in actual if fact.target not in Ia64StopViews.TERMINAL_STUBS
                             and fact.target != 'AsteriaIa64StopEntry'), expected)
                for name, (actual, expected) in facts.items()}

    """The legacy entry and the sequence poll carry no relocation (they are
    position-independent bank-0/P64 leaves), and the two-argument wrapper
    carries exactly one: the PCREL21B of its tail branch to
    AsteriaIa64PollForExternalInterruptSequence+0. Relocations come from the
    ELF RELA sections."""
    GROUPS = (Subjects.HelperGroup('external interrupt', (ENTRY, SEQUENCE_POLL, POLL)),)
    EXPECTED = {POLL: ((0x12, 'PCREL21B', SEQUENCE_POLL, 0),)}     # Slot 2 of the wrapper's second bundle


# Legacy external-interrupt entry ---------------------------------------------------

ENTRY_REGISTERS = frozenset({'r0'} | {'r%d' % number for number in range(16, 32)})
RSE_ARS = frozenset({'ar.pfs', 'ar.rsc', 'ar.bsp', 'ar.bspstore', 'ar.rnat'})
FRAME_BASES = frozenset({'alloc', 'cover', 'flushrs', 'loadrs', 'invala', 'bsw'})


@register
class Ia64ExternalInterruptEntryRegisters(EntryProperty):
    """The bank-0 legacy timer front end touches only r0 and bank-0 r16-r31
    (the interrupted bank-1 leaf survives untouched until rfi), no branch
    register, no RSE/frame application register, no sp, and never allocates,
    covers, flushes, switches banks, calls or leaves through anything but
    rfi and internal branches."""

    def verify(self, code):
        problems = Properties.register_domain_problems(list(code.instructions), ENTRY_REGISTERS)
        for step in code.steps:
            operation = step.operation
            used = (operation.reads | operation.writes) & RSE_ARS
            reasons = ['uses %s' % ', '.join(sorted(used))] if used else []
            if operation.base in FRAME_BASES:
                reasons.append('changes the register frame or bank')
            if operation.is_branch and (operation.is_call or operation.base == 'brl' or operation.is_return
                                        or code.target(step) is None) and not Ia64StopViews.terminal(step):
                reasons.append('is a call, return, long or non-local branch')
            if reasons:
                problems.append('%s (#%d) %s' % (operation.text, step.position, ' and '.join(reasons)))
        return problems

    def mutants(self, code):
        save = code.site(lambda step: step.operation.base == 'mov' and step.operation.reads == {'pr'})
        yield 'pr saved in static r15', rewrite_operand(code, save.position, next(iter(save.operation.writes)), 'r15')
        yield 'alloc in an M-slot nop', code.rewrite(code.free_slot(kind='nop.m'), 'alloc r16=ar.pfs,0,0,0')


@register
class Ia64ExternalInterruptEntryProcessorState(EntryProperty):
    """The entry never writes the processor status: no `mov cr.ipsr=`, no
    `mov psr.l=`/`psr.um=`, no rsm/ssm/rum/sum. rfi resumes the interrupted
    PSR exactly as the hardware saved it."""

    def verify(self, code):
        return ['%s (#%d) writes the processor status' % (step.operation.text, step.position) for step in code.steps
                if step.operation.base in ('ssm', 'rsm', 'sum', 'rum') or 'cr.ipsr' in step.operation.writes
                or any(name.startswith('psr.') for name in step.operation.writes)]

    def mutants(self, code):
        last = len(code.instructions) - 1
        yield 'appended mov cr.ipsr=r16', code.insert(last, 'mov cr.ipsr=r16', after=True)
        yield 'appended rum 0x8', code.insert(last, 'rum 0x8', after=True)


@register
class Ia64ExternalInterruptEntryPredicates(EntryProperty):
    """PR protocol: instruction #1 saves pr before any compare, the saved
    register is never redefined, and both rfi exits (Sequence publication and
    the spurious vector) are immediately preceded by the full restore
    `mov pr=saved,-1`; restore and rfi each end their instruction group, and
    no other pr write exists."""

    def verify(self, code):
        if not code.steps:
            return ['empty extent']
        head = code.steps[0].operation
        if not (head.base == 'mov' and head.reads == {'pr'} and len(head.writes) == 1):
            return ['the first instruction %s does not save pr' % head.text]
        saved = next(iter(head.writes))
        problems = ['%s (#%d) overwrites the saved pr in %s' % (step.operation.text, step.position, saved)
                    for step in code.steps[1:] if saved in step.operation.writes]
        restores = [step for step in code.steps if 'pr' in step.operation.writes]
        rfis = [step for step in code.steps if step.operation.base == 'rfi']
        if len(rfis) != 2:
            problems.append('%d rfi, expected 2' % len(rfis))
        for rfi in rfis:
            previous = code.previous(rfi)
            if previous is None or previous not in restores:
                problems.append('rfi (#%d) is not immediately preceded by the pr restore' % rfi.position)
                continue
            restore = previous.operation
            if saved not in restore.reads or not restore.immediates or restore.immediates[-1] | 1 != ALL_PREDICATES:
                problems.append('%s (#%d) does not restore every predicate from %s'
                                % (restore.text, previous.position, saved))
            if not code.ends_group(previous) or not code.ends_group(rfi):
                problems.append('the pr restore and rfi (#%d, #%d) do not each end an instruction group'
                                % (previous.position, rfi.position))
        return problems + ['%s (#%d) is not immediately followed by rfi' % (restore.operation.text, restore.position)
                           for restore in restores if (following := code.next(restore)) is None
                           or following.operation.base != 'rfi']

    def mutants(self, code):
        yield 'missing pr restore', code.nop(code.site(lambda step: 'pr' in step.operation.writes))


@register
class Ia64ExternalInterruptEntrySerialization(EntryProperty):
    """The destructive `mov rX=cr.ivr` claim, every `mov cr.itv=` mask and
    every `mov cr.eoi=` end their instruction group, and the next group is
    exactly `srlz.d;;`, so the claim, mask and EOI take effect before any
    dependent instruction."""

    def verify(self, code):
        return ['%s (#%d) is not followed by an instruction group of exactly srlz.d'
                % (step.operation.text, step.position) for step in code.steps
                if ('cr.ivr' in step.operation.reads or {'cr.itv', 'cr.eoi'} & step.operation.writes)
                and (not code.ends_group(step) or (following := code.next(step)) is None
                     or following.mnemonic != 'srlz.d' or not code.ends_group(following))]

    def mutants(self, code):
        claim = code.site(lambda step: 'cr.ivr' in step.operation.reads)
        yield 'no stop after the cr.ivr read', code.without_boundary(claim)
        yield 'cr.ivr serialization removed', code.nop(code.next(claim))


def evidence_base(trace: Trace, layout: Offsets) -> tuple | None:
    """The evidence mailbox, located through the single release store of
    Evidence.Sequence."""
    releases = trace.stores('rel')
    return plus(releases[0].address, -layout.evidence['Sequence']) if len(releases) == 1 else None


@register
class Ia64ExternalInterruptEntryOrdering(EvidenceProperty):
    """Memory ordering of the timer mailbox: the only release store in the
    entry publishes Evidence.Sequence, directly after the EOI and its
    `srlz.d` and directly before the pr restore; every load of
    Evidence.Sequence and Evidence.AcknowledgedSequence is an acquire; no
    load or store is predicated. Offsets come from the
    Ia64LocalTimerInterruptEvidence static_asserts."""

    def judge(self, code, layout, trace):
        problems = ['%s (#%d) is predicated' % (code.operations[access.index].text, access.index)
                    for access in trace.accesses if access.predicated]
        releases = trace.stores('rel')
        if len(releases) != 1:
            return problems + ['%d release stores, expected exactly one (Evidence.Sequence)' % len(releases)]
        release, base = releases[0], evidence_base(trace, layout)
        for field in ('Sequence', 'AcknowledgedSequence'):
            accesses = trace.loads(address=plus(base, layout.evidence[field]))
            if not accesses:
                problems.append('no load of Evidence.%s' % field)
            problems += ['%s (#%d) loads Evidence.%s without acquire' % (code.operations[access.index].text,
                                                                         access.index, field)
                         for access in accesses if 'acq' not in access.completers]
        previous = code.previous(release.index)
        earlier = code.previous(previous) if previous is not None else None
        following = code.next(release.index)
        if earlier is None or 'cr.eoi' not in earlier.operation.writes or previous.mnemonic != 'srlz.d':
            problems.append('the Sequence release (#%d) does not directly follow the EOI and its srlz.d'
                            % release.index)
        if following is None or 'pr' not in following.operation.writes:
            problems.append('the Sequence release (#%d) is not directly followed by the pr restore' % release.index)
        return problems

    def edits(self, code, layout, trace):
        yield 'Sequence store without release', rewrite_mnemonic(code, trace.stores('rel')[0].index, 'st8')
        acknowledged = plus(evidence_base(trace, layout), layout.evidence['AcknowledgedSequence'])
        yield 'AcknowledgedSequence load without acquire', rewrite_mnemonic(
            code, trace.loads(address=acknowledged)[0].index, 'ld8')


@register
class Ia64ExternalInterruptEntryEoi(EntryProperty):
    """EOI accounting on every control-flow path of the entry: a path that
    ends before the destructive cr.ivr read executes no EOI; after the read,
    every path to rfi or to a fail-stop executes exactly one
    `mov cr.eoi=r0`, except the spurious-vector (15) exit, which executes
    none. The only loops are fail-stop self-loops (`hint @pause` plus a
    branch to itself)."""

    def verify(self, code):
        return eoi_problems(code, plain_trace(code))

    def mutants(self, code):
        eois = find(code, lambda op: 'cr.eoi' in op.writes)
        fail_stops = [step for step in code.steps if fail_stop_branch(code, step)]
        if len(eois) < 3 or len(fail_stops) < 2:
            raise LookupError('the entry lacks the three EOIs and two fail-stops the mutants edit')
        yield 'claimed-vector cleanup without EOI', code.nop(eois[1])
        yield 'fail-stop looping over an EOI', code.retarget(fail_stops[1], code.step(eois[2]).bundle)


@register
class Ia64ExternalInterruptEntryEvidence(EvidenceProperty):
    """Carrier def-use: the evidence records the interrupted IIP and IPSR
    exactly as captured (Evidence.InterruptedInstructionPointer receives
    cr.iip and Evidence.InterruptedProcessorStatus receives cr.ipsr, one
    store each), so the saved carriers are not clobbered between capture and
    publication."""

    def judge(self, code, layout, trace):
        base = evidence_base(trace, layout)
        if base is None:
            return ['cannot locate the evidence mailbox (no single Sequence release)']
        problems = []
        for field, source in (('InterruptedInstructionPointer', 'cr.iip'), ('InterruptedProcessorStatus', 'cr.ipsr')):
            accesses = trace.stores(address=plus(base, layout.evidence[field]))
            if len(accesses) != 1:
                problems.append('%d stores to Evidence.%s, expected one' % (len(accesses), field))
            elif accesses[0].value != ('in', source):
                problems.append('Evidence.%s receives %s, not the interrupted %s' % (
                    field, show(accesses[0].value), source))
        return problems

    def edits(self, code, layout, trace):
        capture = code.site(lambda step: step.operation.base == 'mov' and step.operation.reads == {'cr.ipsr'})
        carrier = next(iter(capture.operation.writes))
        extract = code.site(lambda step: step is capture,
                            lambda step: step.operation.base == 'extr' and carrier in step.operation.reads)
        yield 'saved IPSR clobbered at the RI check', code.rewrite(extract, 'mov %s=r0' % carrier)


# P64 sequence poll ------------------------------------------------------------------

POLL_REGISTERS = frozenset({'r0', 'r8', 'r14', 'r15', 'r16', 'r17', 'r32', 'r33', 'r34'})
POLL_DEADLINE = ('in', 'r34')


@register
class Ia64ExternalInterruptPollLeaf(PollProperty):
    """P64 leaf ABI of the sequence poll: general registers only in {r0, r8,
    r14-r17, r32-r34} (scratch and the three inputs); no alloc, cover,
    flushrs, loadrs, invala or bank switch; no RSE/frame application
    register; no store; no call; every exit is an unpredicated,
    group-ending `br.ret.sptk.many b0`, and no other branch leaves the leaf."""

    def verify(self, code):
        problems = Properties.register_domain_problems(list(code.instructions), POLL_REGISTERS,
                                                       allow_other_classes=True)
        returns = 0
        for step in code.steps:
            operation = step.operation
            reasons = []
            if operation.base in FRAME_BASES:
                reasons.append('changes the register frame or bank')
            if (operation.reads | operation.writes) & RSE_ARS:
                reasons.append('uses an RSE/frame application register')
            if operation.memory in ('store', 'atomic'):
                reasons.append('stores')
            if operation.is_call or operation.base == 'brl':
                reasons.append('calls')
            if operation.is_return:
                returns += 1
                if operation.is_predicated or operation.reads != {'b0'} or not code.ends_group(step):
                    reasons.append('is not an unpredicated, group-ending return through b0')
            elif operation.is_branch and code.target(step) is None:
                reasons.append('leaves the leaf other than by returning')
            if reasons:
                problems.append('%s (#%d) %s' % (operation.text, step.position, ' and '.join(reasons)))
        return problems + ([] if returns else ['no return'])

    def mutants(self, code):
        head = code.steps[0]
        yield 'inserted store', code.insert(head, 'st8 [r32]=r0', after=True)
        yield 'uses r18', rewrite_operand(code, head.position, next(iter(head.operation.writes)), 'r18')
        yield 'inserted alloc', code.insert(head, 'alloc r16=ar.pfs,0,0,0', after=True)
        yield 'predicated return', code.replace(code.site(lambda step: step.operation.is_return), predicate='p06')


def poll_window_allows(operation: Ia64.Operation) -> bool:
    return not (operation.is_return or operation.is_call or operation.memory in ('store', 'atomic'))


@register
class Ia64ExternalInterruptPollWindow(PollProperty):
    """PSR.i window (shared P-PSR-I-WINDOW): exactly one `ssm 0x4000` and one
    `rsm 0x4000` and no other PSR mask change, each followed by `srlz.d`
    alone in the next group; no return or store inside the window; every
    in-window branch targets the rsm, except exactly one unconditional
    backward branch to the window's first acquire (the bounded poll loop);
    nothing enters the window from outside; no path returns with interrupts
    enabled."""

    def verify(self, code):
        return Properties.psr_i_window_problems(code, strict_exit=False, window_allows=poll_window_allows,
                                                back_edges=1, disables=1)

    def mutants(self, code):
        enable = code.site(lambda step: step.operation.base == 'ssm')
        disable = code.site(lambda step: step.operation.base == 'rsm')
        yield 'ssm psr.ic instead of psr.i', rewrite_operand(code, enable.position, '0x4000', '0x2000')
        yield 'no stop after the rsm', code.without_boundary(disable)
        yield 'loop branch removed', code.nop(first((step for step in code.window(after=enable, before=disable)
                                                     if step.operation.is_branch and not step.operation.is_predicated),
                                                    'the poll loop branch'))


@register
class Ia64ExternalInterruptPollAcquire(PollProperty):
    """Acquire discipline of the poll: every load is `ld8.acq`, and after the
    window closes (rsm + srlz.d) the very next instruction group rechecks the
    same Sequence address the loop polled, before any return, so a completion
    that races the timeout is reported as Completed."""

    def verify(self, code):
        trace = plain_trace(code)
        problems = ['%s (#%d) is not an acquire load' % (code.operations[access.index].text, access.index)
                    for access in trace.loads() if 'acq' not in access.completers]
        enables = find(code, lambda op: op.base == 'ssm' and op.immediates == (Ia64.PSR_I,))
        masks = find(code, lambda op: op.base == 'rsm' and op.immediates == (Ia64.PSR_I,))
        if len(enables) != 1 or len(masks) != 1:
            return problems + ['cannot locate the psr.i window']
        window = [access for access in trace.loads() if enables[0] < access.index < masks[0]]
        after = [access for access in trace.loads() if access.index > masks[0]]
        serialize = code.next(masks[0])
        if not window:
            problems.append('no load inside the psr.i window')
        if not after:
            return problems + ['no recheck after the psr.i window closes']
        recheck = after[0]
        if window and recheck.address != window[0].address:
            problems.append('the recheck (#%d) reads %s, not the polled %s' % (
                recheck.index, show(recheck.address), show(window[0].address)))
        if serialize is None or code.groups[recheck.index] != serialize.group + 1:
            problems.append('the recheck (#%d) is not in the instruction group after the rsm serialization'
                            % recheck.index)
        if any(step.operation.is_return for step in code.window(after=masks[0], before=recheck.index)):
            problems.append('a return precedes the recheck')
        return problems

    def mutants(self, code):
        loads = plain_trace(code).loads()
        enable = code.site(lambda step: step.operation.base == 'ssm').position
        disable = code.site(lambda step: step.operation.base == 'rsm').position
        polled = next(access for access in loads if enable < access.index < disable)
        acknowledged = first((access for access in loads if access.index < enable and access.address != polled.address),
                             'the AcknowledgedSequence load')
        yield 'AcknowledgedSequence load without acquire', rewrite_mnemonic(code, acknowledged.index, 'ld8')
        recheck = next(access for access in loads if access.index > disable)
        yield 'recheck without acquire', rewrite_mnemonic(code, recheck.index, 'ld8')


@register
class Ia64ExternalInterruptPollDeadline(PollProperty):
    """The wait is bounded by the absolute deadline: every ar.itc sample is
    compared with the deadline input r34."""

    def verify(self, code):
        trace = plain_trace(code)
        samples = find(code, lambda op: 'ar.itc' in op.reads)
        if not samples:
            return ['no ar.itc sample']
        return ['the ar.itc sample of %s (#%d) is never compared with the deadline r34'
                % (code.operations[index].text, index) for index in samples
                if not any(condition[0] == 'cmp' and {condition[2], condition[3]} == {('op', 'ar.itc', index),
                                                                                      POLL_DEADLINE}
                           for _, condition in trace.compares)]

    def mutants(self, code):
        sample = ('op', 'ar.itc', find(code, lambda op: 'ar.itc' in op.reads)[-1])
        compare = next(index for index, condition in plain_trace(code).compares
                       if condition[0] == 'cmp' and sample in condition[2:])
        yield 'deadline compared with r32', rewrite_operand(code, compare, 'r34', 'r32')


# Dispatcher ------------------------------------------------------------------------

@register
class Ia64ExternalPreemptionDispatchCarriers(DispatchProperty):
    """The sole IVT external-interrupt dispatcher captures the interruption
    tuple into its carriers before its first branch (the raw IVT-tail
    contract of its continuations): r31<-pr, r30<-psr, r29<-cr.iip,
    r28<-cr.ipsr, r27<-cr.ifs, r26<-cr.isr, r20<-ar.ccv, each written exactly
    once in the dispatcher, so no carrier is redefined before the
    continuation branches."""

    def verify(self, code):
        branches = find(code, lambda op: op.is_branch)
        first_branch = branches[0] if branches else len(code.instructions)
        problems = []
        for register_name, source in DISPATCH_CARRIERS:
            writes = find(code, lambda op: register_name in op.writes)
            if len(writes) != 1:
                problems.append('%s (the %s carrier) is written %d times, expected once'
                                % (register_name, source, len(writes)))
                continue
            operation = code.operations[writes[0]]
            if operation.base != 'mov' or operation.reads != {source}:
                problems.append('%s is written by %s (#%d), not captured from %s'
                                % (register_name, operation.text, writes[0], source))
            elif writes[0] > first_branch:
                problems.append('%s is captured after the first branch' % register_name)
        return problems

    def mutants(self, code):
        capture = code.site(lambda step: step.operation.base == 'mov' and step.operation.reads == {'ar.ccv'})
        carrier = next(iter(capture.operation.writes))
        other = 'r%d' % (int(carrier[1:]) + 1)
        yield 'interrupted CCV captured into %s' % other, rewrite_operand(code, capture.position, carrier, other)


@register
class Ia64ExternalPreemptionDispatchSideEffects(DispatchProperty):
    """The dispatcher only reads and validates: no store (st*, stf*,
    atomics), no cover, invala, alloc, flushrs or loadrs, and no call."""

    def verify(self, code):
        return ['%s (#%d) has a side effect' % (step.operation.text, step.position) for step in code.steps
                if step.operation.memory in ('store', 'atomic') or step.operation.is_call
                or step.operation.base in ('cover', 'invala', 'alloc', 'flushrs', 'loadrs')]

    def mutants(self, code):
        yield 'inserted store', code.insert(code.steps[0], 'st8 [r17]=r16', after=True)
        yield 'inserted cover', code.insert(code.steps[0], 'cover', after=True)


@register
class Ia64ExternalPreemptionDispatchContinuations(DispatchProperty):
    """Exactly three unpredicated direct branches leave the dispatcher: one to
    AsteriaIa64ExternalPreemptionNativeEntry, one to
    AsteriaIa64ExternalInterruptEntry (the legacy continuation) and one to
    AsteriaIa64UserInterruptClaim (the PL3 continuation); the last two are
    each directly preceded by the full pr restore from the r31 carrier. Every
    other branch targets the single fail-stop self-loop or one of those two
    restores. The PL3 restore is reached only by one branch, taken exactly
    when IPSR.cpl (`extr.u` of cr.ipsr bits 32-33) is 3, and no fail-stop
    guard before that branch reads cr.ipsr: the interrupted-PSR checks, which
    reject every CPL but 0, are the PL0 paths'. In an object (and the
    --emit-relocs firmware image) the three continuations are the only
    relocations inside the dispatcher: one PCREL21B each, addend 0; in the
    compile-check image they are resolved branch targets."""

    def extract(self, context, subject):
        elf = Elf.load(subject.path)
        section = elf.symbol(DISPATCH).section_index
        return super().extract(context, subject), any(rela.info == section for rela in elf.relocation_sections())

    def verify(self, facts):
        code, relocated = facts
        problems = []
        continuations: dict[str, list] = {name: [] for name in CONTINUATIONS}
        for step in filter(leaves, code.steps):
            target = step.operation.target
            name = target.symbol if target is not None else None
            if step.operation.is_predicated or target is None or target.offset != 0 or name not in continuations:
                problems.append('%s (#%d) leaves the dispatcher other than by an unpredicated branch to %s'
                                % (step.operation.text, step.position, ', '.join(CONTINUATIONS)))
            else:
                continuations[name].append(step)
        problems += ['%d branches to %s, expected exactly one' % (len(steps), name)
                     for name, steps in continuations.items() if len(steps) != 1]
        fail_stops = [step for step in code.steps if Properties.fail_stop(code, step)]
        if len(fail_stops) != 1:
            problems.append('%d fail-stop loops, expected exactly one' % len(fail_stops))
        carrier = dict((source, register_name) for register_name, source in DISPATCH_CARRIERS)['pr']
        restores = {}
        for name, label in ((ENTRY, 'legacy'), (USER_CLAIM, 'PL3')):
            if not continuations[name]:
                continue
            restore = code.previous(continuations[name][0])
            operation = restore.operation if restore is not None else None
            if (operation is None or 'pr' not in operation.writes or carrier not in operation.reads
                    or not operation.immediates or operation.immediates[-1] | 1 != ALL_PREDICATES):
                problems.append('the %s continuation is not directly preceded by the full pr restore from %s'
                                % (label, carrier))
            else:
                restores[name] = restore
        problems += ["%s (#%d) targets neither the fail-stop nor a continuation's pr restore"
                     % (step.operation.text, step.position) for step in code.steps
                     if step.operation.is_branch and not leaves(step) and not fail_stop_branch(code, step)
                     and (target := code.target(step)) not in fail_stops and target not in restores.values()]
        if USER_CLAIM in restores:
            problems += self._user_route(code, restores[USER_CLAIM])
        if relocated:
            actual = sorted((fact.type, fact.target, fact.addend) for fact in code.relocations)
            if actual != sorted(('PCREL21B', name, 0) for name in CONTINUATIONS):
                problems.append('relocations %s, expected exactly one PCREL21B to each continuation' % actual)
        return problems

    @staticmethod
    def _user_route(code, restore) -> list[str]:
        predecessors = [node for node, successors in code.graph.successors.items() if restore.position in successors]
        routes = [node for node in predecessors if code.step(node).operation.is_branch
                  and code.target(node) is restore]
        if len(predecessors) != 1 or len(routes) != 1:
            return ['the PL3 continuation is reached from %s, expected from one branch only'
                    % ', '.join('#%d' % node for node in predecessors)]
        trace = plain_trace(code)
        if not means(trace.conditions.get(routes[0]), 'eq', IPSR_CPL, USER_PRIVILEGE):
            return ['the branch to the PL3 continuation (#%d) is not taken exactly when IPSR.cpl is 3' % routes[0]]
        return ['the interrupted-PSR guard (#%d) precedes the PL3 route' % index
                for index, condition in failure_conditions(trace, code)
                if index < routes[0] and involves(condition, ('in', 'cr.ipsr'))]

    def mutants(self, facts):
        code, relocated = facts
        native, legacy, user = (code.site(lambda step, name=name: leaves(step) and step.operation.target.symbol == name)
                                for name in CONTINUATIONS)

        def renamed(old, new):
            return tuple(dataclasses.replace(fact, target=new) if fact.target == old else fact
                         for fact in code.relocations)

        retargeted = code.replace(native, text=native.text.replace(NATIVE, ENTRY), relocation=None)
        yield 'native continuation retargeted to the legacy entry', (
            dataclasses.replace(retargeted, relocations=renamed(NATIVE, ENTRY)), relocated)
        yield 'duplicated native continuation', (dataclasses.replace(code.duplicate(native), relocations=(
            code.relocations + tuple(fact for fact in code.relocations if fact.target == NATIVE))), relocated)
        yield 'legacy continuation without the pr restore', (code.nop(code.previous(legacy)), relocated)
        yield 'PL3 continuation without the pr restore', (code.nop(code.previous(user)), relocated)
        bypass = 'AsteriaIa64UserInterruptEntry'
        yield 'PL3 continuation bypasses the claim', (dataclasses.replace(
            code.replace(user, text=user.text.replace(USER_CLAIM, bypass), relocation=None),
            relocations=renamed(USER_CLAIM, bypass)), relocated)
        route = code.site(lambda step: step.operation.is_branch and code.target(step) is code.previous(user))
        test = code.previous(route)
        extract = code.previous(test)
        yield 'PL3 route on IPSR.ri', (rewrite_operand(code, extract.position, '32', '41'), relocated)
        yield 'PL3 route at CPL 0', (rewrite_operand(code, test.position, '3', '0'), relocated)
        yield 'PL3 route when CPL is not 3', (code.replace(route, predicate='p07'), relocated)
        guard = code.site(lambda step: step.operation.base == 'and' and 'r28' in step.operation.reads,
                          lambda step: step.operation.is_branch)
        moved = code.nop(extract).nop(test).nop(route).insert(guard, extract.rendered, test.rendered, route.rendered,
                                                               after=True)
        yield 'PL3 route after the interrupted-PSR guard', (moved, relocated)


# Native capture ------------------------------------------------------------------------

def sequence_problems(label: str, actual: list, expected: list) -> list[str]:
    """First difference between two ordered lists of (value, address) or
    (register, address) pairs."""
    def pair(items) -> str:
        return ' at '.join(show(item) if isinstance(item, tuple) else str(item) for item in items)

    for position, (have, want) in enumerate(zip(actual, expected)):
        if have != want:
            return ['%s #%d is %s, expected %s' % (label, position, pair(have), pair(want))]
    return ['%d %ss, expected %d' % (len(actual), label, len(expected))] if len(actual) != len(expected) else []


# After cover and the RSE stop (ar.rsc=0), the native entry stores the
# BSPSTORE/RNAT pair again; the post-cover pair is authoritative.
POST_COVER_FIELDS = ('RseBackingStoreStorePointer', 'RseNatCollection')


@register
class Ia64ExternalPreemptionCaptureMap(NativeProperty):
    """Capture map: before `cover`, the native entry stores every
    interruption, application, predicate and branch register into its
    Ia64ExternalPreemptionFrame field, exactly one store per field, with the
    value as interrupted (dataflow from the dispatcher carriers r20/r26-r31
    or from a `mov rX=cr.*|ar.*|bN` producer): psr, cr.iip, cr.ipsr, cr.ifs,
    cr.isr, cr.ifa, cr.itir, cr.iipa, cr.iim, cr.iha, ar.pfs, ar.rsc,
    ar.bsp, ar.bspstore, ar.rnat, ar.unat (OriginalNatCollection), ar.lc,
    ar.ec, ar.csd, ar.ccv, ar.fpsr, pr and b0-b7. The frame is
    [Control.FrameAddress]; all offsets come from the frame and control
    static_asserts. Only the BSPSTORE/RNAT pair may be stored again after
    cover (the authoritative post-cover pair), with the same values."""

    def judge(self, code, layout, trace):
        frame, covers = native_frame(layout), events(code, 'cover')
        expected = [(name, layout.frame[name], ('in', source)) for name, source in CAPTURE_MAP] + [
            ('BranchRegisters[%d]' % number, layout.frame['BranchRegisters'] + 8 * number, ('in', 'b%d' % number))
            for number in range(BRANCH_REGISTERS)]
        problems = []
        for label, offset, value in expected:
            accesses = trace.stores(address=plus(frame, offset))
            before = [access for access in accesses if covers and access.index < covers[0]]
            if len(before) != 1:
                problems.append('%d stores to Frame.%s before cover, expected one' % (len(before), label))
            if len(accesses) > len(before) and label not in POST_COVER_FIELDS:
                problems.append('Frame.%s is rewritten after cover' % label)
            problems += ['Frame.%s receives %s (#%d), expected the interrupted %s'
                         % (label, show(access.value), access.index, value[1])
                         for access in accesses if access.value != value]
        return problems

    def edits(self, code, layout, trace):
        for description, source, shift in (('IFA stored at the ITIR field', 'cr.ifa', 8),
                                           ('LC stored at the EC field', 'ar.lc', 8),
                                           ('b3 stored at the b4 field', 'b3', 8),
                                           ('original UNAT stored below its field', 'ar.unat', -8)):
            yield description, shift_immediate(code, trace.store_of(('in', source)).base_writer, shift)
        for description, source, other in (('PFS capture reads ar.lc', 'ar.pfs', 'ar.lc'),
                                           ('b4 capture reads b5', 'b4', 'b5'),
                                           ('CSD capture reads ar.fpsr', 'ar.csd', 'ar.fpsr')):
            yield description, rewrite_operand(code, trace.store_of(('in', source)).data_writer, source, other)


@register
class Ia64ExternalPreemptionSpills(NativeProperty):
    """Spill completeness and UNAT discipline: `st8.spill` of static r1-r15
    and then of the interrupted bank-1 r16-r31 (their values as interrupted,
    so the bank-1 run executes between `bsw.1` and `bsw.0`) into
    Frame.GeneralRegisters with an 8-byte stride, then `stf.spill` of
    f2-f127 into Frame.FloatingRegisters with a 16-byte stride; each register
    once, ascending, all before `cover`. Each GR run starts from a cleared
    ar.unat, and Frame.StaticGeneralRegisterSpillNatCollection /
    Frame.BankedGeneralRegisterSpillNatCollection receive exactly the NaT
    collection of their run. ar.ssd is never touched."""
    STATIC = tuple('r%d' % number for number in range(1, 16))
    BANKED = tuple('r%d#1' % number for number in range(16, 32))

    def judge(self, code, layout, trace):
        frame = native_frame(layout)
        general, floating = layout.frame['GeneralRegisters'], layout.frame['FloatingRegisters']
        spills = trace.stores('spill')
        integer = [access for access in spills if access.mnemonic.startswith('st8')]
        floats = [access for access in spills if access.mnemonic.startswith('stf')]
        problems = sequence_problems(
            'general-register spill', [(access.value, access.address) for access in integer],
            [(('in', name), plus(frame, general + 8 * position))
             for position, name in enumerate(self.STATIC + self.BANKED)])
        problems += sequence_problems(
            'floating-register spill', [(access.value, access.address) for access in floats],
            [(('in', 'f%d' % number), plus(frame, floating + 16 * (number - 2))) for number in range(2, 128)])
        if integer and floats and integer[-1].index > floats[0].index:
            problems.append('floating registers are spilled before the general registers are complete')
        covers = events(code, 'cover')
        if spills and (not covers or spills[-1].index > covers[0]):
            problems.append('registers are spilled after cover')
        for field, registers in (('StaticGeneralRegisterSpillNatCollection', self.STATIC),
                                 ('BankedGeneralRegisterSpillNatCollection', self.BANKED)):
            accesses = trace.stores(address=plus(frame, layout.frame[field]))
            if [access.value for access in accesses] != [('unat', ZERO, registers)]:
                problems.append('Frame.%s receives %s, expected the NaT collection of exactly %s..%s from a cleared '
                                'ar.unat' % (field, [show(access.value) for access in accesses], registers[0],
                                             registers[-1]))
        return problems + ['%s (#%d) uses ar.ssd' % (step.operation.text, step.position) for step in code.steps
                           if step.uses('ar.ssd')]

    def edits(self, code, layout, trace):
        static, banked = trace.store_of(('unat', ZERO, self.STATIC)), trace.store_of(('unat', ZERO, self.BANKED))
        first_static, first_banked = trace.store_of(('in', 'r1')), trace.store_of(('in', 'r16#1'))
        first_float = trace.store_of(('in', 'f2'))
        yield 'static NaT collection stored at OriginalNatCollection', shift_immediate(code, static.base_writer, -8)
        yield 'banked NaT collection stored at the static field', shift_immediate(code, banked.base_writer, -8)
        yield 'no switch to the interrupted bank', code.nop(events(code, 'bsw')[0])
        yield 'static spill base one register late', shift_immediate(code, first_static.base_writer, 8)
        yield 'static spill stride 16', set_increment(code, first_static.index, 16)
        yield 'banked spill base one register late', shift_immediate(code, first_banked.base_writer, 8)
        yield 'floating spill base one register late', shift_immediate(code, first_float.base_writer, 16)
        yield 'floating spill stride 8', set_increment(code, first_float.index, 8)


@register
class Ia64ExternalPreemptionClaimCcv(NativeProperty):
    """The native entry claims Control.State with exactly one compare-exchange
    whose comparand is the value it wrote to ar.ccv, and preserves the
    interrupted CCV around it: after the claim, ar.ccv is written exactly
    once, with the interrupted value carried from the dispatcher."""

    def judge(self, code, layout, trace):
        atomics = [access for access in trace.accesses if access.kind == 'atomic']
        if len(atomics) != 1:
            return ['%d atomic accesses, expected exactly one claim' % len(atomics)]
        claim = atomics[0]
        problems = []
        if claim.address != control_field(layout, 'State'):
            problems.append('the claim targets %s, not Control.State' % show(claim.address))
        writes = trace.writes_of('ar.ccv')
        before = [write for write in writes if write.index < claim.index]
        if not before or claim.value[-1] != before[-1].value:
            problems.append('the claim does not compare against the value written to ar.ccv before it')
        after = [write.value for write in writes if write.index > claim.index]
        if after != [('in', 'ar.ccv')]:
            problems.append('after the claim ar.ccv is written with %s, expected once with the interrupted ar.ccv'
                            % [show(value) for value in after])
        return problems

    def edits(self, code, layout, trace):
        claim = next(access for access in trace.accesses if access.kind == 'atomic')
        writes = trace.writes_of('ar.ccv')
        before = [write for write in writes if write.index < claim.index][-1]
        restore = next(write for write in writes if write.index > claim.index)
        yield 'CCV restored from the claim comparand', source_of(code, restore.index, restore.index, before.index)


@register
class Ia64ExternalPreemptionCover(NativeProperty):
    """Exactly one `cover`, after every capture and spill (see the capture
    and spill properties) and before the covered reads:
    Frame.CoveredInterruptionFunctionState receives cr.ifs as read after
    cover and Frame.CoveredRseBackingStorePointer receives ar.bsp as read
    after cover, one store each."""

    def judge(self, code, layout, trace):
        covers = events(code, 'cover')
        problems = [] if len(covers) == 1 else ['%d cover, expected exactly one' % len(covers)]
        for field, resource in (('CoveredInterruptionFunctionState', 'cr.ifs'),
                                ('CoveredRseBackingStorePointer', 'ar.bsp')):
            values = [access.value for access in trace.stores(address=plus(native_frame(layout), layout.frame[field]))]
            if values != [('covered', resource)]:
                problems.append('Frame.%s receives %s, expected %s read after cover once'
                                % (field, [show(value) for value in values], resource))
        return problems

    def edits(self, code, layout, trace):
        yield 'cover removed', code.nop(events(code, 'cover')[0])
        yield 'covered BSP stored past its field', shift_immediate(
            code, trace.store_of(('covered', 'ar.bsp')).base_writer, 8)


@register
class Ia64ExternalPreemptionPublication(NativeProperty):
    """Acknowledgement and publication order: exactly one `mov cr.itv=`
    (timer mask) and then exactly one `mov cr.eoi=` precede the frame
    release-publish (the st*.rel of Frame.PublishedToken), which precedes
    the control release-publish (st*.rel of Control.State); no other release
    store. EOI accounting holds on every control-flow path, as for the legacy
    entry (the spurious vector fail-stops without EOI)."""

    def judge(self, code, layout, trace):
        releases = trace.stores('rel')
        frames = [access for access in releases
                  if access.address == plus(native_frame(layout), layout.frame['PublishedToken'])]
        controls = [access for access in releases if access.address == control_field(layout, 'State')]
        problems = eoi_problems(code, trace)
        if len(frames) != 1 or len(controls) != 1 or len(releases) != 2:
            return problems + ['release stores %s, expected exactly Frame.PublishedToken then Control.State'
                               % [show(access.address) for access in releases]]
        published = frames[0]
        if published.index > controls[0].index:
            problems.append('the control is published before the frame')
        masks = [write.index for write in trace.writes_of('cr.itv') if write.index < published.index]
        eois = [write.index for write in trace.writes_of('cr.eoi') if write.index < published.index]
        if len(masks) != 1 or len(eois) != 1:
            problems.append('%d timer masks and %d EOIs precede the frame publication, expected one each'
                            % (len(masks), len(eois)))
        elif masks[0] > eois[0]:
            problems.append('the EOI precedes the timer mask')
        return problems

    def edits(self, code, layout, trace):
        yield 'accepted timer mask removed', code.nop(trace.writes_of('cr.itv')[0].index)
        yield 'accepted EOI removed', code.nop(trace.writes_of('cr.eoi')[0].index)


@register
class Ia64ExternalPreemptionBridge(NativeProperty):
    """The coordinator bridge: after the control publication and exactly one
    `alloc` with at least one output, exactly one unpredicated indirect
    `br.call` linking b0 transfers to Control.CoordinatorEntryPoint with
    gp = Control.CoordinatorGlobalPointer, out0 = Control.CoordinatorArgument
    and sp on the coordinator stack (Control.CoordinatorMemoryStackEnd).
    After it returns: an `mf` before any memory access, fail-stops unless sp
    and ar.bsp equal their pre-call baselines, an `alloc` of an empty frame,
    `rsm psr.ic` serialized by srlz.d in a later group, and an unconditional
    branch to AsteriaIa64ExternalPreemptionRestoreTail."""

    def judge(self, code, layout, trace):
        calls = [branch for branch in trace.branches if branch.call]
        if len(calls) != 1:
            return ['%d calls, expected exactly one returning bridge call' % len(calls)]
        call = calls[0]
        operation = code.operations[call.index]
        state = dict(call.state)
        problems = []
        if operation.target is not None or operation.is_predicated or 'b0' not in operation.writes:
            problems.append('%s is not an unpredicated indirect call linking b0' % operation.text)
        for label, have, field in (('the target', call.via_value, 'CoordinatorEntryPoint'),
                                   ('gp', state.get('r1'), 'CoordinatorGlobalPointer'),
                                   ('out0', state.get('r32'), 'CoordinatorArgument')):
            want = load(control_field(layout, field))
            if have != want:
                problems.append('%s of the bridge call is %s, expected %s' % (label, show(have), show(want)))
        stack = state.get('r12', ZERO)
        if (stack[1] if stack[0] == 'add' else stack) != load(control_field(layout, 'CoordinatorMemoryStackEnd')):
            problems.append('sp at the bridge call is %s, not on the coordinator stack' % show(stack))
        published = [access.index for access in trace.stores('rel') if access.address == control_field(layout, 'State')]
        start = published[0] if published else -1
        if not published or start > call.index:
            problems.append('the bridge call does not follow the control publication')
        allocs = events(code, 'alloc')
        before = [index for index in allocs if start < index < call.index]
        if len(before) != 1:
            problems.append('%d allocs between the control publication and the call, expected one' % len(before))
        elif Ia64.alloc_frame(code.operations[before[0]])[0] - Ia64.alloc_frame(code.operations[before[0]])[1] < 1:
            problems.append('the bridge frame has no output register for the argument')
        fences = [index for index in events(code, 'mf') if index > call.index]
        later = [access.index for access in trace.accesses if access.index > call.index]
        if not fences or later and later[0] < fences[0]:
            problems.append('no mf between the call and the next memory access')
        guards = failure_conditions(trace, code)
        problems += ['the returned %s is not checked against its pre-call baseline' % label
                     for label, resource in (('sp', 'r12'), ('ar.bsp', 'ar.bsp'))
                     if not fails_when(guards, 'ne', ('op', 'after-call', resource, call.index), state.get(resource))]
        discards = [index for index in allocs if index > call.index
                    and Ia64.alloc_frame(code.operations[index])[0] == 0]
        clears = [write.index for write in trace.writes_of('psr')
                  if write.index > call.index and write.value == ('op', 'rsm', ('k', Ia64.PSR_IC))]
        if not discards:
            problems.append('no alloc of an empty frame after the call')
        if len(clears) != 1:
            return problems + ['%d `rsm psr.ic` after the call, expected one' % len(clears)]
        if discards and discards[0] > clears[0]:
            problems.append('psr.ic is cleared before the coordinator frame is discarded')
        serialize = code.next(clears[0])
        if serialize is None or serialize.mnemonic != 'srlz.d' or serialize.group <= code.groups[clears[0]]:
            problems.append('`rsm psr.ic` is not followed by srlz.d in a later group')
        exit_step = code.next(serialize) if serialize is not None else None
        target = exit_step.operation.target if exit_step is not None else None
        if (exit_step is None or not exit_step.operation.is_branch or exit_step.operation.is_predicated
                or target is None or target.symbol != RESTORE or target.offset != 0):
            problems.append('the bridge does not continue with an unconditional branch to %s' % RESTORE)
        return problems

    def edits(self, code, layout, trace):
        call = next(branch for branch in trace.branches if branch.call)
        yield 'coordinator transfer that does not return', code.replace(
            call.index, text='br.sptk.many ' + _source(code, call.index), relocation=None)


# Restore tail ------------------------------------------------------------------------

def restore_releases(trace: Trace, layout: Offsets) -> tuple[list[Access], list[Access], list[Access]]:
    """(frame releases, control completions, all release stores)."""
    releases = trace.stores('rel')
    return ([access for access in releases if access.address == restore_field(layout, 'PublishedToken')],
            [access for access in releases if access.address == control_field(layout, 'State')], releases)


@register
class Ia64ExternalPreemptionRestoreMap(RestoreProperty):
    """The restore map mirrors the capture map: the last write of each
    resource before rfi comes from its frame field (ar.lc/ec/csd/ccv/fpsr,
    ar.pfs, ar.bspstore, ar.rnat, the final ar.rsc and ar.unat, cr.ifs from
    the covered IFS, cr.iip, b0-b7 from BranchRegisters[0..7]); resources
    other than ar.rsc and ar.unat are written exactly once; pr is restored
    once from Frame.Predicates with every predicate; cr.ipsr is written once
    with Frame.InterruptedProcessorStatus with only PSR.i cleared. The
    hardware IFS image is only validated: the restore fail-stops unless its
    IFS.v is clear and the covered image's IFS.v is set."""

    def judge(self, code, layout, trace):
        expected = [(resource, name, load(restore_field(layout, name))) for resource, name in RESTORE_MAP] + [
            ('b%d' % number, 'BranchRegisters[%d]' % number, load(restore_field(layout, 'BranchRegisters', 8 * number)))
            for number in range(BRANCH_REGISTERS)]
        problems = []
        for resource, name, value in expected:
            writes = trace.writes_of(resource)
            if not writes:
                problems.append('%s is never restored' % resource)
                continue
            if resource not in ('ar.rsc', 'ar.unat') and len(writes) != 1:
                problems.append('%s is written %d times, expected once' % (resource, len(writes)))
            if writes[-1].value != value:
                problems.append('%s is restored from %s, expected Frame.%s' % (resource, show(writes[-1].value), name))
        predicates = trace.writes_of('pr')
        if (len(predicates) != 1 or predicates[0].value[1] != load(restore_field(layout, 'Predicates'))
                or predicates[0].value[2][0] != 'k' or predicates[0].value[2][1] | 1 != ALL_PREDICATES):
            problems.append('pr is not restored once, completely, from Frame.Predicates')
        ipsr = [write.value for write in trace.writes_of('cr.ipsr')]
        if ipsr != [('op', 'dep', ZERO, load(restore_field(layout, 'InterruptedProcessorStatus')), ('k', PSR_I_BIT),
                     ('k', 1))]:
            problems.append('cr.ipsr is restored from %s, expected Frame.InterruptedProcessorStatus with PSR.i '
                            'cleared' % [show(value) for value in ipsr])
        guards = failure_conditions(trace, code)

        def valid_bit(name):
            return ('op', 'shr.u', load(restore_field(layout, name)), ('k', IFS_VALID_BIT))

        if not fails_when(guards, 'ne', valid_bit('HardwareInterruptionFunctionState'), ZERO):
            problems.append('the restore does not fail-stop on a valid hardware IFS image')
        if not fails_when(guards, 'eq', valid_bit('CoveredInterruptionFunctionState'), ZERO):
            problems.append('the restore does not fail-stop on an invalid covered IFS image')
        return problems

    def edits(self, code, layout, trace):
        rnat, rsc, csd, ccv, b0, b4, b5, ipsr, lc = (trace.writes_of(resource)[0] for resource in (
            'ar.rnat', 'ar.rsc', 'ar.csd', 'ar.ccv', 'b0', 'b4', 'b5', 'cr.ipsr', 'ar.lc'))
        hardware = trace.loads(address=restore_field(layout, 'HardwareInterruptionFunctionState'))[0]
        lc_load, csd_load, b0_load = (trace.at(write.source_writer) for write in (lc, csd, b0))
        yield 'RNAT restored from the loadrs configuration', source_of(code, rnat.index, rnat.index, rsc.index)
        yield 'application registers loaded one field late', shift_immediate(code, lc_load.base_writer, 8)
        yield 'CSD loaded into another register', source_of(code, csd_load.index, csd.index, ccv.index)
        yield 'branch registers loaded one field late', shift_immediate(code, b0_load.base_writer, 8)
        yield 'b4 restored from the b5 image', source_of(code, b4.index, b4.index, b5.index)
        yield 'IFS validation loads one field early', shift_immediate(code, hardware.base_writer, -8)
        yield 'IPSR normalization clears bit 13', rewrite_operand(code, ipsr.source_writer, str(PSR_I_BIT),
                                                                  str(PSR_I_BIT - 1))


@register
class Ia64ExternalPreemptionRestoreRse(RestoreProperty):
    """RSE restore order: ar.rsc is set to
    Frame.RseDirtyPartitionLoadConfiguration, then `loadrs`, then
    `mov ar.bspstore=`, then `mov ar.rnat=` (one each); afterwards the
    restore fail-stops unless the resulting ar.bsp equals
    Frame.CoveredRseBackingStorePointer."""

    def judge(self, code, layout, trace):
        loadrs = events(code, 'loadrs')
        bspstore, rnat = trace.writes_of('ar.bspstore'), trace.writes_of('ar.rnat')
        if len(loadrs) != 1 or len(bspstore) != 1 or len(rnat) != 1:
            return ['%d loadrs, %d ar.bspstore and %d ar.rnat writes, expected one each'
                    % (len(loadrs), len(bspstore), len(rnat))]
        problems = []
        configuration = [write for write in trace.writes_of('ar.rsc') if write.index < loadrs[0]]
        if (not configuration
                or configuration[-1].value != load(restore_field(layout, 'RseDirtyPartitionLoadConfiguration'))):
            problems.append('loadrs runs with ar.rsc = %s, expected Frame.RseDirtyPartitionLoadConfiguration'
                            % (show(configuration[-1].value) if configuration else 'unset'))
        if not loadrs[0] < bspstore[0].index < rnat[0].index:
            problems.append('the RSE is not restored in the order loadrs, ar.bspstore, ar.rnat')
        bsp = ('op', 'bsp-after-bspstore', bspstore[0].index)
        covered = load(restore_field(layout, 'CoveredRseBackingStorePointer'))
        if not any(index > rnat[0].index and means(condition, 'ne', bsp, covered)
                   for index, condition in failure_conditions(trace, code)):
            problems.append('the restored ar.bsp is not checked against Frame.CoveredRseBackingStorePointer')
        return problems

    def edits(self, code, layout, trace):
        rsc, rnat = trace.writes_of('ar.rsc')[0].index, trace.writes_of('ar.rnat')[0].index
        yield 'loadrs configured from the RNAT image', source_of(code, rsc, rsc, rnat)
        covered = trace.loads(address=restore_field(layout, 'CoveredRseBackingStorePointer'))[0]
        yield 'BSP compared with the preceding field', shift_immediate(code, covered.base_writer, -8)


@register
class Ia64ExternalPreemptionRestoreFills(RestoreProperty):
    """Fill completeness: `ldf.fill` of f2-f127 from Frame.FloatingRegisters
    (16-byte stride), then `ld8.fill` of the interrupted bank-1 r16-r31 and
    then of static r1-r15 from Frame.GeneralRegisters (8-byte stride), each
    register once and ascending; the banked fills run with ar.unat equal to
    Frame.BankedGeneralRegisterSpillNatCollection and the static fills with
    Frame.StaticGeneralRegisterSpillNatCollection."""

    def judge(self, code, layout, trace):
        fills = trace.loads('fill')
        floats = [access for access in fills if access.mnemonic.startswith('ldf')]
        integer = [access for access in fills if access.mnemonic.startswith('ld8')]
        problems = sequence_problems(
            'floating-register fill', [(access.register, access.address) for access in floats],
            [('f%d' % number, restore_field(layout, 'FloatingRegisters', 16 * (number - 2)))
             for number in range(2, 128)])
        order = ['r%d#1' % number for number in range(16, 32)] + ['r%d' % number for number in range(1, 16)]
        problems += sequence_problems(
            'general-register fill', [(access.register, access.address) for access in integer],
            [(name, restore_field(layout, 'GeneralRegisters', 8 * (int(name[1:].split('#')[0]) - 1)))
             for name in order])
        for access in integer:
            field = ('BankedGeneralRegisterSpillNatCollection' if access.register.endswith('#1')
                     else 'StaticGeneralRegisterSpillNatCollection')
            if access.unat != load(restore_field(layout, field)):
                problems.append('the fill of %s (#%d) runs with ar.unat = %s, expected Frame.%s'
                                % (access.register, access.index, show(access.unat), field))
                break
        if floats and integer and floats[-1].index > integer[0].index:
            problems.append('general registers are filled before the floating registers are complete')
        return problems

    def edits(self, code, layout, trace):
        def fill(register):
            return next(access for access in trace.loads('fill') if access.register == register)

        yield 'floating fill base one register late', shift_immediate(code, fill('f2').base_writer, 8)
        yield 'floating fill stride 8', set_increment(code, fill('f2').index, 8)
        yield 'banked fill base one register late', shift_immediate(code, fill('r16#1').base_writer, 8)
        yield 'banked fill stride 16', set_increment(code, fill('r16#1').index, 16)
        yield 'static fill base one register late', shift_immediate(code, fill('r1').base_writer, 8)


@register
class Ia64ExternalPreemptionRestoreImage(RestoreProperty):
    """The resumed code receives the complete general-register image: after
    its `ld8.fill`, no instruction before the single `rfi` writes a static
    register r1-r15, or a bank-1 register r16-r31 while bank 1 is selected
    (destination or post-increment base). Every register the capture spilled
    thus reaches rfi with its Frame.GeneralRegisters value and NaT bit; the
    tail's own address and value carriers live in bank-0 r16-r31, which are
    not part of the interrupted BN=1 image."""

    def judge(self, code, layout, trace):
        rfis = events(code, 'rfi')
        if len(rfis) != 1:
            return ['%d rfi, expected one' % len(rfis)]
        filled = {access.register: access.index for access in trace.loads('fill')
                  if access.mnemonic.startswith('ld8')}
        problems, bank = [], 1
        for step in code.window(before=rfis[0]):
            if step.operation.base == 'bsw':
                bank = int(step.operation.completers[0])
                continue
            for register in sorted(step.operation.general_writes):
                name = physical(register, bank)
                if name in filled and step.position > filled[name]:
                    problems.append('%s (#%d) overwrites the restored %s before rfi'
                                    % (step.operation.text, step.position, name))
        return problems

    def edits(self, code, layout, trace):
        rfi = events(code, 'rfi')[0]
        last_fill = trace.loads('fill')[-1].index
        yield 'static r14 reused as an address after the fills', code.insert(rfi, 'adds r14=8,r16')
        yield 'static r15 cleared after the fills', code.insert(last_fill, 'mov r15=r0', after=True)
        yield 'bank-1 r20 written after its fill', code.insert(last_fill, 'mov r20=r0', after=True)
        yield 'static r4 post-incremented after its fill', code.insert(rfi, 'ld8 r9=[r4],8')


@register
class Ia64ExternalPreemptionRestoreCarriers(RestoreProperty):
    """The frame and control address carriers (bank-0 copies of r4/r5, and
    the Control.State address derived from the latter) survive the bank-1
    and static fills: after the last fill, exactly one release store
    completes Control.State and exactly one release store clears
    Frame.PublishedToken (storing zero); no other release store exists."""

    def judge(self, code, layout, trace):
        frames, controls, releases = restore_releases(trace, layout)
        if len(frames) != 1 or len(controls) != 1 or len(releases) != 2:
            return ['release stores %s, expected one to Control.State and one to Frame.PublishedToken'
                    % [show(access.address) for access in releases]]
        problems = [] if frames[0].value == ZERO else ['Frame.PublishedToken is released with %s, not zero'
                                                       % show(frames[0].value)]
        fills = [access.index for access in trace.loads('fill')]
        if fills and min(frames[0].index, controls[0].index) < fills[-1]:
            problems.append('a release store precedes the last fill')
        return problems

    def edits(self, code, layout, trace):
        (release,), (completion,), _ = restore_releases(trace, layout)
        frame_copy, address = release.base_writer, completion.base_writer
        control_register, control_copy = next(iter(trace.inputs[address].items()))
        yield 'frame carrier copied from the control', source_of(code, frame_copy, frame_copy, control_copy)
        yield 'control carrier copied from the frame', source_of(code, control_copy, control_copy, frame_copy)
        yield 'control completion one field late', shift_immediate(code, address, 8)
        base = Ia64.split(code.instructions[release.index].text)[1][0]
        yield 'frame released through the control carrier', rewrite_operand(code, release.index, base,
                                                                             '[%s]' % control_register)


@register
class Ia64ExternalPreemptionRestoreRelease(RestoreProperty):
    """Tail order and post-release allowlist: after the last fill, the
    control completion precedes the cr.ifs write, which precedes the final
    ar.rsc write, which precedes the frame release; the frame release is the
    last memory access, and between it and the single rfi only the IPSR.i
    `dep`, `mov cr.iip=`, `mov cr.ipsr=` and `mov pr=` run, once each and in
    that order: no memory access, call, alloc, RSE or AR write, or branch
    can fault or refill after ownership is released."""

    def judge(self, code, layout, trace):
        frames, controls, _ = restore_releases(trace, layout)
        rfis = events(code, 'rfi')
        if len(frames) != 1 or len(controls) != 1 or len(rfis) != 1:
            return ['%d frame releases, %d control completions and %d rfi, expected one each'
                    % (len(frames), len(controls), len(rfis))]
        release, rfi = frames[0], rfis[0]
        fills = [access.index for access in trace.loads('fill')]
        ifs, rsc = trace.writes_of('cr.ifs'), trace.writes_of('ar.rsc')
        sequence = [('last fill', fills[-1] if fills else -1), ('control completion', controls[0].index),
                    ('cr.ifs write', ifs[-1].index if ifs else -1),
                    ('final ar.rsc write', rsc[-1].index if rsc else -1),
                    ('frame release', release.index), ('rfi', rfi)]
        problems = ['the %s (#%d) does not precede the %s (#%d)' % (before, first_index, after, second_index)
                    for (before, first_index), (after, second_index) in zip(sequence, sequence[1:])
                    if not first_index < second_index]
        kinds = []
        for step in code.window(after=release.index, before=rfi):
            operation = step.operation
            if operation.base == 'dep' and operation.memory is None:
                kinds.append('dep')
            elif operation.base == 'mov' and operation.writes in ({'cr.iip'}, {'cr.ipsr'}, {'pr'}):
                kinds.append(next(iter(operation.writes)))
            else:
                problems.append('%s (#%d) runs after the frame release' % (operation.text, step.position))
        if kinds != ['dep', 'cr.iip', 'cr.ipsr', 'pr']:
            problems.append('the post-release sequence is %s, expected dep, cr.iip, cr.ipsr, pr' % kinds)
        return problems

    def edits(self, code, layout, trace):
        (release,), (completion,), _ = restore_releases(trace, layout)
        rfi = events(code, 'rfi')[0]
        ifs, rsc = trace.writes_of('cr.ifs')[-1].index, trace.writes_of('ar.rsc')[-1].index
        dep = trace.writes_of('cr.ipsr')[0].source_writer
        yield 'rfi removed', code.nop(rfi)
        yield 'final ar.rsc written before cr.ifs', code.replace(
            ifs, text=code.instructions[rsc].text, relocation=None).replace(
            rsc, text=code.instructions[ifs].text, relocation=None)
        yield 'duplicated frame release', code.duplicate(release.index)
        yield 'duplicated control completion', code.duplicate(completion.index)
        for text in ('ld8 r18=[r17]', 'br.call.sptk.many b0=b6', 'mov.m ar.rsc=r23'):
            yield 'post-release %s' % text, code.insert(dep, text)
        for label, index in (('dep', dep), ('cr.iip write', trace.writes_of('cr.iip')[0].index),
                             ('cr.ipsr write', trace.writes_of('cr.ipsr')[0].index),
                             ('pr restore', trace.writes_of('pr')[0].index), ('rfi', rfi)):
            yield 'duplicated %s' % label, code.duplicate(index)
