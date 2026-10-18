# SPDX-License-Identifier: Apache-2.0
"""IA-64 user-mode launch leaf `AsteriaIa64LaunchUserMode`.

The hand-written `.explicit` leaf of Platform/Ia64/Scheduler/Ia64UserModeLaunch.S
validates the native launch plan, captures the complete P64 supervisor
continuation, publishes it as Retained, crosses a no-memory cut with
interruption delivery and collection disabled, installs the user state and
leaves through `rfi`. It is assembled identically at O0, O2 and in the
firmware, and the golden Tests/Golden/Ia64/Scheduler/AsteriaIa64LaunchUserMode.dis
pins its exact instruction stream and size; the properties below state the
rules that must survive a golden update.

Structure offsets, the initial FPSR and the Retained state come from the
`static_assert`s of Ia64UserModeLaunch.hpp and Ia64KernelContext.hpp, and the
two rejection codes from the adapter constants of Ia64UserModeLaunch.cpp, so
the rules follow the structures rather than literals: a layout change that
the leaf does not follow is reported here.
"""
from __future__ import annotations

import dataclasses
import functools
import re

from AsteriaVerify import Ia64, Layout, Properties, Subjects
from AsteriaVerify.Checks import AcceptanceError, Property, register
from AsteriaVerify.ControlFlow import Code

SOURCE = 'Source/Platform/Ia64/Scheduler/Ia64UserModeLaunch.cpp'
LAUNCH_HEADER = 'Source/Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp'
CONTEXT_HEADER = 'Source/Platform/Ia64/Scheduler/Ia64KernelContext.hpp'
SYMBOL = 'AsteriaIa64LaunchUserMode'
# The validation prefix: one processor-state gate and eleven supervisor
# continuation gates (null, alignment, freshness, memory stack x3, RSE x5).
PREFIX_GATES = 12

# Plan field loaded into each commit carrier. Commit installs
# r23/r24 into r4/r5, r21/r22 into r6/r7, r25 into ar.bspstore, r26
# into cr.ipsr, r27 into ar.rsc, r28 into cr.iip, r29 into r12 and r31 into r1
# (r30 carries the initial FPSR into ar.fpsr).
CARRIER_FIELDS = {23: 'InitialArguments.Register4', 24: 'InitialArguments.Register5',
                  21: 'InitialArguments.Register6', 22: 'InitialArguments.Register7', 25: 'UserRseBackingStorePointer',
                  26: 'UserProcessorStatus', 27: 'UserRseConfiguration', 28: 'EntryBundleAddress',
                  29: 'UserStackPointer', 31: 'InitialGlobalPointer'}
# Every commit operand register: the plan carriers and the FPSR carrier r30.
COMMIT_OPERANDS = tuple(sorted(set(CARRIER_FIELDS) | {30}))
# Each initial-argument register and the carrier it receives. The
# argument carriers may be cleared only once all four are installed.
ARGUMENT_CARRIERS = {4: 23, 5: 24, 6: 21, 7: 22}
# Registers that hold a plan value at rfi instead of zero: r1 (GP), r4-r7 and r12.
INSTALLED_GENERAL = (1, *ARGUMENT_CARRIERS, 12)
# Continuation register-state field → the register that holds the captured
# value when stored, and its architectural source.
# ar.unat is captured twice: before the preserved-register
# spills (the caller's UNAT) and after them (the spills' NaT collection,
# `ar.unat+spills`).
PRE_COVER_CAPTURE = (
    ('GlobalPointer', 'r1', 'r1'), ('StackPointer', 'r12', 'r12'), ('ThreadPointer', 'r13', 'r13'),
    ('InstructionPointer', 'r21', 'b0'), ('PreviousFunctionState', 'r18', 'ar.pfs'),
    ('RseConfiguration', 'r16', 'ar.rsc'), ('RseBackingStorePointer', 'r17', 'ar.bsp'),
    ('UserNatCollection', 'r19', 'ar.unat'), ('LoopCounter', 'r16', 'ar.lc'), ('FloatingPointStatus', 'r17', 'ar.fpsr'),
    ('Predicates', 'r18', 'pr'), ('B1', 'r19', 'b1'), ('B2', 'r21', 'b2'), ('B3', 'r22', 'b3'), ('B4', 'r23', 'b4'),
    ('B5', 'r24', 'b5'))
POST_COVER_CAPTURE = (('RseNatCollection', 'r16', 'ar.rnat'),
                      ('PreservedGeneralNatCollection', 'r16', 'ar.unat+spills'))
FLOAT_SPILL_ORDER = tuple([2, 3, 4, 5] + list(range(16, 32)))


# Structure layout from the headers ---------------------------------------------

@dataclasses.dataclass(frozen=True)
class LaunchLayout:
    """Offsets of the launch plan and the supervisor continuation, flattened
    to `Field` or `Field.SubField` names, and the fixed values the leaf
    installs or returns."""
    plan: tuple[tuple[str, int],...]
    continuation: tuple[tuple[str, int],...]
    register_state_offset: int
    register_state_size: int
    state_size: int
    float_register_size: int
    initial_fpsr: int
    retained: int
    processor_reject: int
    supervisor_reject: int

    def plan_offset(self, name: str) -> int:
        return dict(self.plan)[name]

    def field(self, name: str) -> int:
        return dict(self.continuation)[name]

    def register(self, name: str) -> int:
        return self.field('Registers.' + name)

    def moved(self, table: str, name: str, offset: int) -> 'LaunchLayout':
        """The layout with `name` of the `plan` or `continuation` at `offset`."""
        return dataclasses.replace(self, **{table: tuple((field, offset if field == name else old)
                                                         for field, old in getattr(self, table))})


def parse_layout(launch: str, context: str, source: str) -> LaunchLayout:
    """Reads the launch layout from the header and source texts (see the
    module docstring); raises Layout.LayoutError when one no longer states a
    value the rules rely on."""
    def flattened(fields: dict, nested: dict) -> tuple[tuple[str, int],...]:
        result = []
        for name, offset in sorted(fields.items(), key=lambda item: item[1]):
            members = nested.get(name)
            result += [(name, offset)] if members is None else [
                ('%s.%s' % (name, member), offset + delta) for member, delta in sorted(members.items(),
                                                                                      key=lambda item: item[1])]
        return tuple(result)

    ranges, fields = Layout.offsets(launch, 'Ia64UserModeVirtualRange'), Layout.offsets(
        launch, 'Ia64UserModeSupervisorContinuation')
    plan = flattened(Layout.offsets(launch, 'Ia64UserModeLaunchPlan'),
                     {'InitialArguments': Layout.offsets(launch, 'Ia64UserModeInitialArguments')})
    continuation = flattened(fields, {'Registers': Layout.offsets(context, 'Ia64KernelRegisterState'),
                                      'MemoryStack': ranges, 'RseBackingStore': ranges})
    asserted, codes = Layout.assertions(launch), Layout.constants(source)
    values = {'initial FPSR': asserted.get('Ia64UserModeInitialFloatingPointStatus'),
              'Retained state': next((value for expression, value in asserted.items()
                                      if expression.endswith('(Ia64UserModeSupervisorContinuationState::Retained)')),
                                     None),
              **{name: codes.get(name) for name in ('AdapterUnsupportedProcessorState',
                                                    'AdapterInvalidActiveSupervisorState')}}
    missing = [name for name, value in values.items() if value is None]
    if missing:
        raise Layout.LayoutError('the sources no longer state: %s' % ', '.join(missing))
    if 'Registers' not in fields:
        raise Layout.LayoutError('Ia64UserModeSupervisorContinuation no longer asserts the offset of Registers')
    return LaunchLayout(plan, continuation, fields['Registers'], Layout.size(context, 'Ia64KernelRegisterState'),
                        Layout.size(launch, 'Ia64UserModeSupervisorContinuationState'),
                        Layout.size(context, 'Ia64FloatingRegisterState'), *values.values())


@functools.lru_cache(maxsize=None)
def _layout(root: str) -> LaunchLayout:
    return parse_layout(*(Layout.read('%s/%s' % (root, path)) for path in (LAUNCH_HEADER, CONTEXT_HEADER, SOURCE)))


# Leaf facts ------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class LeafFacts:
    """The decoded leaf (nop slots included) and the layout it must follow."""
    code: Code
    layout: LaunchLayout

    def edited(self, code: Code) -> 'LeafFacts':
        return dataclasses.replace(self, code=code)


class _LeafProperty(Property):
    isas = ('Ia64',)

    def extract(self, context, variant):
        return LeafFacts(Subjects.code(context, context.defining_object(variant, SYMBOL), SYMBOL),
                         _layout(str(context.root)))


_GR_DESTINATION = re.compile(r'^\S+\s+r(\d+)=')
_PREDICATE_PAIR_DESTINATION = re.compile(r'^\S+\s+p\d+,p\d+=')
_BRANCH_REGISTER_DESTINATION = re.compile(r'^mov(\.\S+)?\s+b([0-7])=')


def gr_destination(text: str) -> int | None:
    match = _GR_DESTINATION.match(text)
    return int(match.group(1)) if match else None


def updating_memory_base(text: str) -> int | None:
    """The base register written by post-increment memory forms: `ld*`/`lfetch
    [rB],imm|rI` and `st* [rB]=rX,imm|rI`; other memory forms (cmpxchg,
    `st8.rel [rB]=rX`,...) do not write their base."""
    mnemonic = text.split(None, 1)[0] if text else ''
    if not re.match(r'(ld|st|lfetch)', mnemonic) or not re.search(r'\[r\d+\]', text):
        return None
    increment = r'(r\d+|-?(0x[0-9a-fA-F]+|\d+))'
    tail = text[text.rindex(']') + 1:]
    if not re.fullmatch((r',\s*' if mnemonic.startswith(('ld', 'lfetch')) else r'=[^,]+,\s*') + increment, tail):
        return None
    return int(re.findall(r'\[r(\d+)\]', text)[-1])


def address_text(register: int, offset: int, base: str = 'r14') -> str:
    """objdump's spelling of `add rN = offset, base` (`mov` for offset 0)."""
    return 'mov r%d=%s' % (register, base) if offset == 0 else 'adds r%d=%d,%s' % (register, offset, base)


def _capture(code: Code) -> int:
    """The position of the second `mov.m r16=ar.rsc`, which starts the capture."""
    return code.all('mov.m r16=ar.rsc')[1].position


def _spill_base_moved(facts: LeafFacts, field: str, delta: int) -> LeafFacts:
    """The spill base `adds r20=<field>,r15` moved by `delta` bytes."""
    offset = facts.layout.register(field)
    return facts.edited(facts.code.replace(facts.code.site(address_text(20, offset, 'r15')),
                                           text=address_text(20, offset + delta, 'r15')))


# Symbol contract and relocations ------------------------------------------------------

@register
class Ia64LaunchUserModeSymbol(Subjects.SymbolContract):
    """The leaf is one GLOBAL FUNC, so that the C++ direct call binds to it,
    with DEFAULT or HIDDEN visibility (both bind inside the statically linked
    image), and
    it starts on a bundle boundary and spans whole bundles inside a section
    whose flags are exactly ALLOC|EXECINSTR, so that the checked extent is the
    executed code. (The exact size is pinned by the golden.)"""
    SYMBOLS = (SYMBOL,)
    CONTRACT = Properties.FunctionContract(Properties.CALLABLE, alignment=16, size_multiple=16)


@register
class Ia64LaunchUserModeRelocations(Subjects.CodeProperty):
    """The leaf is self-contained: no relocation entry has its r_offset in
    [value, value + size) of the leaf's section. Its only inputs are the plan
    pointer and live processor state, so the verified bytes are exactly what
    runs after linking at any address or GP. Relocations are read from the
    ELF RELA sections."""
    SYMBOL = SYMBOL

    def verify(self, code):
        return ['relocation %s against %s at +%#x inside the launch leaf' % (fact.type, fact.target, fact.offset)
                for fact in code.relocations] + (['the launch leaf is missing or empty'] if code.end == code.start
                                                 else [])

    def mutants(self, code):
        for description, fact in (('call relocation in the first bundle',
                                   Properties.RelocationFact(2, 'PCREL21B', 'memset', 0)),
                                  ('data relocation in the last bundle',
                                   Properties.RelocationFact(code.end - code.start - 16, 'LTOFF22X',
                                                             'g_ia64BspObjectRegistry', 0))):
            yield description, dataclasses.replace(code, relocations=code.relocations + (fact,))


# Validation prefix ----------------------------------------------------------

_PREFIX_BRANCH = re.compile(r'br\.cond\.(sptk|spnt|dptk|dpnt)\.(few|many) <\+0x[0-9a-f]+>')
_ADDS = re.compile(r'adds r(\d+)=(-?\d+),r(\d+)')
_MOV_GR = re.compile(r'mov r(\d+)=r(\d+)')


def _propagate(pointers: dict, text: str, operation: Ia64.Operation, predicated: bool) -> None:
    """Symbolic plan/continuation pointers through `adds rD=imm,rS` and
    `mov rD=rS`; any other general-register write forgets its pointer."""
    adds, move = _ADDS.fullmatch(text), _MOV_GR.fullmatch(text)
    if adds and not predicated:
        source = pointers.get('r' + adds.group(3))
        pointers['r' + adds.group(1)] = (source[0], source[1] + int(adds.group(2))) if source else None
    elif move and not predicated:
        pointers['r' + move.group(1)] = pointers.get('r' + move.group(2))
    else:
        pointers.update(dict.fromkeys(operation.general_writes))


def _prefix_operation(text: str) -> bool:
    """Instructions the validation prefix may use besides its entry copy."""
    return (text == 'mov r15=psr' or re.fullmatch(r'mov\.m r\d+=ar\.(rsc|bsp|bspstore)', text) is not None
            or text.split(None, 1)[0] in ('adds', 'add', 'and', 'or') or text.startswith('cmp.')
            or re.fullmatch(r'ld8 r\d+=\[r\d+\]', text) is not None)


def _tails(facts: LeafFacts) -> tuple[list[str], int | None, int | None]:
    """Problems of the rejection tails, and the positions of tails A and B."""
    code, layout = facts.code, facts.layout
    rfis = code.all(lambda step: step.mnemonic == 'rfi')
    if len(rfis) != 1:
        return ['rejection tail: %d rfi instructions, expected exactly one' % len(rfis)], None, None
    if rfis[0].instruction.predicate is not None:
        return ['rejection tail: the rfi is predicated'], None, None
    after = code.window(after=rfis[0])
    expected = ['mov r8=%d' % layout.processor_reject, 'br.ret.sptk.many b0',
                'mov r8=%d' % layout.supervisor_reject, 'br.ret.sptk.many b0']
    actual = [step.rendered for step in after]
    if actual != expected:
        return ['rejection tail: after the rfi the leaf holds %s, expected only the rejection tails %s'
                % (actual, expected)], None, None
    return [], after[0].position, after[2].position


@register
class Ia64LaunchPrefixSafety(_LeafProperty):
    """Every invalid processor, plan or continuation state leaves through a
    side-effect-free rejection tail before capture, cover or rfi.

    - The leaf starts with an unpredicated `mov r14=r32` at its symbol.
    - Before the second `mov.m r16=ar.rsc` (the capture) every meaningful
      instruction is a `(p6) br.cond` to a label of the leaf, or an
      unpredicated PSR/RSE read, ALU operation, compare or plain `ld8`
      writing only r14-r22 (r14 only by the entry copy): no store, alloc,
      call, long branch or other branch.
    - Exactly 12 validation branches, each ending its instruction group;
      the first targets tail A, the others tail B.
    - After the unique unpredicated rfi only the tails remain: tail A
      `mov r8=AdapterUnsupportedProcessorState; br.ret.sptk.many b0` and tail
      B `mov r8=AdapterInvalidActiveSupervisorState; br.ret.sptk.many b0`.
    - The leaf reads `ar.rsc` into r16 exactly twice.
    - At the capture r14 still holds the plan and r15 was loaded from the
      plan's SupervisorContinuationAddress field.
    - Every prefix load reads a field of the plan or of the
      continuation record (offsets from the headers).
    - Every gate is decided by a compare of a non-constant value.
    """

    def verify(self, facts):
        code, layout = facts.code, facts.layout
        instructions = code.instructions
        if not instructions:
            return ['entry: the leaf decodes to no instruction']
        problems = []
        if instructions[0].address != code.start or instructions[0].rendered() != 'mov r14=r32':
            problems.append('entry: the instruction at %s+0 is "%s", not an unpredicated mov r14=r32'
                            % (SYMBOL, instructions[0].rendered()))
        rsc_reads = [index for index, instruction in enumerate(instructions) if instruction.text == 'mov.m r16=ar.rsc']
        if len(rsc_reads) != 2:
            problems.append('RSC capture: %d reads mov.m r16=ar.rsc, expected two (validation, then capture)' % len(rsc_reads))
        if len(rsc_reads) < 2:
            return problems
        tail_problems, tail_a, tail_b = _tails(facts)
        problems += tail_problems
        plan_fields = {offset for _, offset in layout.plan}
        continuation_fields = {offset for _, offset in layout.continuation}
        continuation_pointer = layout.plan_offset('SupervisorContinuationAddress')
        pointers: dict[str, tuple[str, int] | None] = {}
        predicate_writers: dict[str, int] = {}
        branches, r14_writes, r15_writes = [], [], []
        for step in code.window(before=rsc_reads[1]):
            text, operation, index = step.text, step.operation, step.position
            if operation.is_branch:
                branches.append(index)
                if not _PREFIX_BRANCH.fullmatch(text) or operation.predicate != 'p6':
                    problems.append('prefix instruction: prefix branch "%s" (#%d) is not a (p6) br.cond to a label of the leaf'
                                    % (step.rendered, index))
                    continue
                if not code.ends_group(step):
                    problems.append('validation branch: prefix branch "%s" (#%d) does not end its instruction group'
                                    % (step.rendered, index))
                writer = predicate_writers.get('p6')
                if writer is None:
                    problems.append('validation gate: nothing in the prefix computes the p6 gate of branch #%d' % index)
                elif (code.operations[writer].base not in ('cmp', 'cmp4', 'tbit', 'tnat')
                      or not code.operations[writer].general_reads - {'r0'}):
                    problems.append('validation gate: the p6 gate of branch #%d is set by "%s" (#%d), which is not a compare of '
                                    'a validated value' % (index, instructions[writer].rendered(), writer))
                continue
            if step.instruction.predicate is not None:
                problems.append('prefix instruction: prefix instruction "%s" (#%d) is predicated' % (step.rendered, index))
                continue
            entry_copy = index == 0 and text == 'mov r14=r32'
            if not entry_copy and not _prefix_operation(text):
                problems.append('prefix instruction: prefix instruction "%s" (#%d) is not a PSR/RSE read, ALU operation, compare or '
                                'plain ld8' % (text, index))
            destination = gr_destination(text)
            if destination is not None and not 14 <= destination <= 22:
                problems.append('prefix instruction: prefix instruction "%s" (#%d) writes r%d, outside the scratch window r14-r22'
                                % (text, index, destination))
            {14: r14_writes, 15: r15_writes}.get(destination, []).append(index)
            predicate_writers.update(dict.fromkeys((name for name in operation.writes if re.fullmatch(r'p\d+', name)),
                                                   index))
            # Symbolic plan/continuation pointers.
            if entry_copy:
                pointers['r14'] = ('plan', 0)
            elif operation.memory == 'load':
                base = pointers.get(operation.memory_base)
                fields = plan_fields if base is not None and base[0] == 'plan' else continuation_fields
                if base is None or base[1] not in fields:
                    problems.append('plan load: prefix load "%s" (#%d) does not read a field of the plan or of the '
                                    'continuation record' % (text, index))
                if destination is not None:
                    pointers['r%d' % destination] = ('continuation', 0) \
                        if base == ('plan', continuation_pointer) else None
            else:
                _propagate(pointers, text, operation, False)
        if r14_writes != [0]:
            problems.append('plan preservation: r14 (the plan pointer) is written at %s, expected only by the entry copy' % r14_writes)
        expected_pointer = [address_text(15, continuation_pointer), 'ld8 r15=[r15]']
        last_pointer_writes = [instructions[index].rendered() for index in r15_writes[-2:]]
        if last_pointer_writes != expected_pointer:
            problems.append('plan preservation: r15 does not hold the continuation address at the capture: its last writes are %s, '
                            'expected %s' % (last_pointer_writes, expected_pointer))
        if len(branches) != PREFIX_GATES:
            problems.append('validation branch: %d validation branches, expected %d' % (len(branches), PREFIX_GATES))
        if tail_a is not None and tail_b is not None:
            for number, index in enumerate(branches, start=1):
                resolved = code.target(index)
                expected, tail = (tail_a, 'A (processor state)') if number == 1 else (tail_b, 'B (supervisor state)')
                if resolved is None or resolved.position != expected:
                    problems.append('validation branch: validation branch %d (#%d) does not target rejection tail %s'
                                    % (number, index, tail))
        return problems

    def mutants(self, facts):
        code, layout, edit = facts.code, facts.layout, facts.edited
        capture = _capture(code)
        rfi = code.site(lambda step: step.mnemonic == 'rfi')
        branches = [step.position for step in code.window(before=capture) if step.operation.is_branch]
        _, tail_a, tail_b = _tails(facts)
        # Transition sentinels.
        yield 'reject-edge-to-rfi', edit(code.retarget(branches[0], rfi.bundle))
        yield 'entry-long-call', edit(code.insert(0, 'brl.call.sptk.few b1=<+%#x>' % rfi.bundle, stop=True))
        yield 'pre-capture-store', edit(code.insert(capture, 'st8 [r14]=r0', stop=True))
        yield 'reject-tail-state-write', edit(code.insert(rfi, 'ssm 0x6000', after=True, stop=True))
        null_compare = code.site('ld8 r15=[r15]', lambda step: step.operation.base == 'cmp'
                                 and {'r15', 'r0'} <= step.operation.reads)
        yield 'prefix-gate-bypass', edit(code.replace(code.free_slot(null_compare, 'nop.i 0x0'),
                                                      text='cmp.ne p6,p7=r0,r0', stop_after=True))
        gate = next(index for index in branches if index > null_compare.position)
        yield 'prefix-unmodelled-load', edit(code.insert(gate, 'ld8 r16=[r0]', after=True, stop=True))
        # New self-tests.
        yield 'predicated prefix ALU operation', edit(code.replace(code.site(lambda step: step.mnemonic == 'and'),
                                                                   predicate='p7'))
        for description, text in (('prefix write to r23', 'adds r23=8,r14'), ('third ar.rsc read', 'mov.m r16=ar.rsc'),
                                  ('prefix write to r12', 'adds r12=-16,r12'),
                                  ('alloc in the prefix', 'alloc r2=ar.pfs,0,0,0')):
            yield description, edit(code.insert(0, text, after=True, stop=True))
        yield 'prefix branch without a stop', edit(code.without_boundary(branches[0]))
        yield 'wrong processor rejection code', edit(code.replace(tail_a,
                                                                  text='mov r8=%d' % (layout.processor_reject + 2)))
        yield 'supervisor gate to the processor tail', edit(code.retarget(branches[-1], code.step(tail_a).bundle))
        yield 'predicated rejection return', edit(code.replace(code.next(tail_b), predicate='p6'))
        yield 'thirteenth gate', edit(code.insert(branches[-1], code.instructions[branches[-1]].rendered(), after=True,
                                                  stop=True))
        continuation, global_pointer = (layout.plan_offset(name)
                                        for name in ('SupervisorContinuationAddress', 'InitialGlobalPointer'))
        yield 'continuation pointer from another plan field', edit(code.replace(
            code.site(address_text(15, continuation)), text=address_text(15, global_pointer)))
        yield 'header moves SupervisorContinuationAddress', dataclasses.replace(
            facts, layout=layout.moved('plan', 'SupervisorContinuationAddress', global_pointer))


# Capture, publication, cut and install --------------------------------------

EARLY_CAPTURE_READS = ('mov.m r16=ar.rsc', 'mov.m r17=ar.bsp', 'mov.i r18=ar.pfs', 'mov.m r19=ar.unat')
LATE_CAPTURE_READS = ('mov r21=b0', 'mov.i r16=ar.lc', 'mov.m r17=ar.fpsr', 'mov r18=pr', 'mov r19=b1',
                      'mov r21=b2', 'mov r22=b3', 'mov r23=b4', 'mov r24=b5')
CLEARED_APPLICATION_REGISTERS = (('ar.rnat', 'mov.m'), ('ar.unat', 'mov.m'), ('ar.pfs', 'mov.i'),
                                 ('ar.lc', 'mov.i'), ('ar.ccv', 'mov.m'), ('ar.ec', 'mov.i'))
# The write that must be the last one of each resource at rfi, and the
# carrier whose still-valid value it installs (none for a clear).
FINAL_WRITES = (('ar.bspstore', 'mov.m ar.bspstore=r25', 25), ('ar.fpsr', 'mov.m ar.fpsr=r30', 30),
                ('ar.rsc', 'mov.m ar.rsc=r27', 27), ('cr.iip', 'mov cr.iip=r28', 28),
                ('cr.ipsr', 'mov cr.ipsr=r26', 26)) + tuple(
    (resource, '%s %s=r0' % (mnemonic, resource), None) for resource, mnemonic in CLEARED_APPLICATION_REGISTERS) + (
    ('cr.ifs', 'mov cr.ifs=r0', None),)
# The texts that clear or install a resource at commit.
COMMIT_CLEARS = {**{'%s %s=r0' % (mnemonic, resource): resource
                    for resource, mnemonic in CLEARED_APPLICATION_REGISTERS},
                 'mov.m ar.fpsr=r30': 'ar.fpsr', 'mov pr=r0,0xfffffffffffffffe': 'pr'}
_CAPTURE_SOURCE = re.compile(r'^mov(?:\.[mi])? r(\d+)=(ar\.\w+|b[0-7]|pr)$')


def preparation_text(layout: LaunchLayout, kind: str, register: int) -> str:
    """objdump text of a commit-operand preparation step."""
    if kind == 'address':
        return address_text(register, layout.plan_offset(CARRIER_FIELDS[register]))
    if kind == 'load':
        return 'ld8 r%d=[r%d]' % (register, register)
    return 'movl r%d=%#x' % (register, layout.initial_fpsr)


def semantic_problems(facts: LeafFacts) -> list[str]:
    """Validate layout-derived offsets, captured architectural values and
    register ownership. Spill bases start at R4 and FloatingRegisters."""
    code, layout = facts.code, facts.layout
    instructions = code.instructions
    problems: list[str] = []

    def fail(clause, index, message):
        problems.append('%s: %s (#%d "%s")' % (clause, message, index, instructions[index].rendered()))

    # Preparation steps of each commit operand r21-r31, in order.
    preparation = {register: [preparation_text(layout, kind, register) for kind in ('address', 'load')]
                   for register in CARRIER_FIELDS}
    preparation[30] = [preparation_text(layout, 'initializer', 30)]
    pre_cover = {layout.register(field): register for field, register, _ in PRE_COVER_CAPTURE}
    post_cover = {layout.register(field): register for field, register, _ in POST_COVER_CAPTURE}
    expected_source = {layout.register(field): source for field, _, source in PRE_COVER_CAPTURE + POST_COVER_CAPTURE}
    integer_spill_base = address_text(20, layout.register('R4'), 'r15')
    float_spill_base = address_text(20, layout.register('FloatingRegisters'), 'r15')
    publication = ('mov r16=%d' % layout.retained, address_text(20, layout.field('State'), 'r15'), 'st8 [r20]=r16')

    capture_stage = rsc_reads = integer_spills = float_spills = cover_stage = publication_stage = rfi_count = 0
    integer_base_ready = float_base_ready = rnat_read = unat_read = preparation_started = after_cut = False
    scrubbing = False
    rsc_disable_awaiting_stop = fence_awaiting_stop = False
    continuation_offset = None
    late_reads = dict.fromkeys(LATE_CAPTURE_READS, False)
    holds = {'r1': 'r1', 'r12': 'r12', 'r13': 'r13'}
    store_counts: dict[int, int] = {}
    operand_stage = dict.fromkeys(COMMIT_OPERANDS, 0)
    address_ready, value_ready = dict.fromkeys(COMMIT_OPERANDS, False), dict.fromkeys(COMMIT_OPERANDS, False)
    final: dict[str, bool] = {}
    general_final: dict[int, bool] = {}
    branch_final: dict[int, bool] = {}
    cleared = {'r%d' % carrier: False for carrier in (*ARGUMENT_CARRIERS.values(), 31)}

    def pre_cover_missing():
        missing = [] if capture_stage == 4 else ['ar.rsc/ar.bsp/ar.pfs/ar.unat capture (stage %d)' % capture_stage]
        missing += ['read "%s"' % text for text, seen in late_reads.items() if not seen]
        return missing + ['store at +%d' % offset for offset in pre_cover if store_counts.get(offset, 0) != 1]

    def operands_terminal():
        return [r for r in COMMIT_OPERANDS if operand_stage[r] != len(preparation[r]) or not value_ready[r]]

    def pre_commit_missing():
        missing = pre_cover_missing() + [text for failed, text in (
            (cover_stage != 3, 'cover/flushrs/rsc=0 sequence (stage %d)' % cover_stage),
            (not rnat_read, 'RNAT read after the RSE is drained'), (not unat_read, 'UNAT read after the four spills'),
            (integer_spills != 4, 'st8.spill of r4-r7 (%d of 4)' % integer_spills),
            (float_spills != len(FLOAT_SPILL_ORDER),
             'stf.spill of f2-f5,f16-f31 (%d of %d)' % (float_spills, len(FLOAT_SPILL_ORDER)))) if failed]
        missing += ['terminal operand r%d' % r for r in operands_terminal()]
        missing += [] if publication_stage == 5 else ['Retained publication (stage %d)' % publication_stage]
        return missing + ['store at +%d' % offset for offset in post_cover if store_counts.get(offset, 0) != 1]

    def record_operand_write(register, text, predicated, index):
        nonlocal preparation_started
        was_address_ready, current, texts = address_ready[register], operand_stage[register], preparation[register]
        address_ready[register] = value_ready[register] = False
        stage = current + 1 if (current < len(texts) and text == texts[current]
                                and (current == 0 or was_address_ready)) else 0
        if stage:
            preparation_started = preparation_started or register == 23
            if predicated:
                fail('commit operands', index, 'predicated preparation of commit operand r%d' % register)
            else:
                operand_stage[register] = stage
                (value_ready if stage == len(texts) else address_ready)[register] = True
        if preparation_started and (not stage or predicated):
            fail('commit operands', index, 'unexpected or duplicate write of commit operand r%d once preparation started'
                 % register)

    def final_missing():
        missing = [resource for resource in ('ar.bspstore', 'ar.rnat', 'ar.unat', 'ar.pfs', 'ar.lc', 'ar.ccv',
                                             'ar.ec', 'ar.fpsr', 'ar.rsc', 'cr.iip', 'cr.ipsr', 'cr.ifs', 'r12',
                                             'r1', 'r4', 'r5', 'r6', 'r7', 'pr') if not final.get(resource)]
        missing += ['carrier %s cleared after its consumers' % name for name, done in cleared.items() if not done]
        missing += ['r%d=r0' % r for r in range(1, 32) if r not in INSTALLED_GENERAL and not general_final.get(r)]
        return missing + ['b%d=r0' % b for b in range(8) if not branch_final.get(b)]

    for index, instruction in enumerate(instructions):
        if rfi_count:
            break
        text, predicated, stop, nop = instruction.text, instruction.predicate is not None, instruction.stop_after, \
            instruction.is_nop
        operation, destination, updated_base = code.operations[index], gr_destination(text), updating_memory_base(text)
        if re.match(r'bsw\.[01](\.\S+)*(\s|$)', text):
            fail('register bank', index, 'register-bank switch before rfi')
        stacked = sorted((w for w in operation.general_writes if int(w[1:]) >= 32), key=lambda w: int(w[1:]))
        if operation.base == 'alloc' and after_cut and not predicated:
            scrubbing = Ia64.alloc_frame(operation)[0] != 0
        elif stacked and not (scrubbing and text == 'mov %s=r0' % stacked[0] and len(stacked) == 1):
            fail('stacked registers', index, 'writes the stacked register(s) %s, which the leaf does not own after cover'
                 % ', '.join(stacked))
        if fence_awaiting_stop:
            if not nop:
                fail('publication', index, 'the second publication fence does not end its group')
            elif stop:
                publication_stage, fence_awaiting_stop = 5, False
            continue
        if rsc_disable_awaiting_stop:
            if nop and stop:
                cover_stage, rsc_disable_awaiting_stop = 3, False
            elif not nop:
                fail('RSE detach', index, 'mov.m ar.rsc=r0 is not followed by a group end')
                rsc_disable_awaiting_stop = False
        if text == 'rsm 0x6000':
            missing = pre_commit_missing()
            if not stop or predicated or missing:
                fail('memory cutoff', index, 'the cut is predicated, does not end its group, or comes before %s'
                     % (', '.join(missing) or 'nothing missing'))
            after_cut = True
            continue

        if not after_cut:
            # Capture reads.
            if text == 'mov.m r16=ar.rsc':
                rsc_reads += 1
                if rsc_reads == 2:
                    if capture_stage != 0 or predicated:
                        fail('capture reads', index, 'capture does not start with an unpredicated ar.rsc read')
                    capture_stage = 1
                elif rsc_reads > 2:
                    fail('capture reads', index, 'third ar.rsc read')
            elif text in EARLY_CAPTURE_READS[1:]:
                stage = EARLY_CAPTURE_READS.index(text)
                if capture_stage != stage or predicated or (stage == 3 and not stop):
                    fail('capture reads', index, 'capture read out of order, predicated, or (ar.unat) not ending its group')
                if capture_stage < 4:
                    capture_stage = stage + 1
            if capture_stage == 4 and text in late_reads:
                late_reads[text] = not predicated
            # Architectural value held in each general register.
            source = _CAPTURE_SOURCE.match(text)
            if source and not predicated:
                value = source.group(2)
                spilled = value == 'ar.unat' and integer_spills == 4
                holds['r' + source.group(1)] = value + '+spills' if spilled else value
            else:
                holds.update(dict.fromkeys(operation.general_writes))
            # Continuation stores through r20.
            adds = re.fullmatch(r'adds r20=(\d+),r15', text)
            if text == integer_spill_base:
                continuation_offset = None
            elif adds:
                continuation_offset = int(adds.group(1)) if int(adds.group(1)) in expected_source else None
            store = re.fullmatch(r'st8 \[r20\]=(r\d+)', text)
            if store and continuation_offset is not None:
                register = pre_cover.get(continuation_offset, post_cover.get(continuation_offset))
                if store.group(1) != register or predicated:
                    fail('continuation state', index, 'continuation +%d must receive an unpredicated %s'
                         % (continuation_offset, register))
                elif holds.get(register) != expected_source[continuation_offset]:
                    fail('continuation state', index, 'continuation +%d receives %s holding %s, expected the captured %s'
                         % (continuation_offset, register, holds.get(register),
                            expected_source[continuation_offset]))
                store_counts[continuation_offset] = store_counts.get(continuation_offset, 0) + 1
                continuation_offset = None
            # Cover, drain and RSE detach.
            if text == 'cover':
                if not stop or predicated or pre_cover_missing() or cover_stage != 0:
                    fail('RSE detach', index, 'cover is predicated, does not end its group, or comes before %s'
                         % (', '.join(pre_cover_missing()) or 'nothing missing'))
                cover_stage = 1
            elif text == 'flushrs':
                if cover_stage != 1 or not stop or predicated:
                    fail('RSE detach', index, 'flushrs does not follow cover or does not end its group')
                cover_stage = 2
            elif text == 'mov.m ar.rsc=r0':
                if cover_stage != 2 or predicated:
                    fail('RSE detach', index, 'the RSE is not put in enforced-lazy mode right after flushrs')
                if stop:
                    cover_stage = 3
                else:
                    rsc_disable_awaiting_stop = True
            elif text == 'mov.m r16=ar.rnat':
                rnat_read = cover_stage == 3 and not predicated
            elif text == 'mov.m r16=ar.unat':
                unat_read = integer_spills == 4 and not predicated
            # Spill bases must start at R4 or
            # FloatingRegisters, advanced only by the spills themselves.
            if re.fullmatch(r'st8\.spill \[r20\]=r[4-7],8', text):
                if not integer_base_ready or text != 'st8.spill [r20]=r%d,8' % (integer_spills + 4) or predicated:
                    fail('preserved-register spills', index, 'preserved-register spill out of order, predicated, or not based at R4')
                else:
                    integer_spills += 1
                float_base_ready = False
            elif re.fullmatch(r'stf\.spill \[r20\]=f\d+,16', text):
                expected = 'stf.spill [r20]=f%d,16' % FLOAT_SPILL_ORDER[float_spills] \
                    if float_spills < len(FLOAT_SPILL_ORDER) else None
                if store_counts.get(layout.register('PreservedGeneralNatCollection'), 0) != 1 or text != expected \
                        or predicated or not float_base_ready:
                    fail('floating-point spills', index, 'floating-point spill out of order, predicated, before the NaT collection is '
                         'stored, or not based at FloatingRegisters')
                else:
                    float_spills += 1
                integer_base_ready = False
            elif text == integer_spill_base:
                integer_base_ready, float_base_ready = not predicated, False
            elif text == float_spill_base:
                integer_base_ready, float_base_ready = False, not predicated
            elif 'r20' in operation.writes:
                integer_base_ready = float_base_ready = False
            # Commit operands, written explicitly or as a post-increment base.
            for register in dict.fromkeys(r for r in (destination, updated_base) if r in COMMIT_OPERANDS):
                record_operand_write(register, text, predicated, index)
            # Publication.
            if publication_stage == 0:
                if text == 'mf':
                    if float_spills != len(FLOAT_SPILL_ORDER) or operands_terminal() or not stop or predicated:
                        fail('publication', index, 'the publication fence is predicated, does not end its group, or comes '
                             'before the spills and terminal operands')
                    publication_stage = 1
            elif not nop:
                if 1 <= publication_stage <= 3 and text == publication[publication_stage - 1] and not predicated \
                        and (publication_stage != 3 or stop):
                    publication_stage += 1
                elif publication_stage == 4 and text == 'mf' and not predicated:
                    publication_stage, fence_awaiting_stop = (5, False) if stop else (4, True)
                else:
                    fail('publication', index, 'unexpected instruction in the Retained publication')
            continue

        # After the cut: no memory access; sanitize return state.
        if text == 'rfi':
            rfi_count += 1
            missing = final_missing()
            if predicated or missing:
                fail('return state', index, 'rfi is predicated or the state is not sanitized: %s'
                     % (', '.join(missing) or 'nothing missing'))
            continue
        if '[' in text:
            fail('post-cut accesses', index, 'memory access after the no-memory cut')
        if updated_base in COMMIT_OPERANDS:
            address_ready[updated_base] = value_ready[updated_base] = False
        if destination is not None:
            if destination in COMMIT_OPERANDS:
                value_ready[destination] = False
            carrier = {1: 31, **ARGUMENT_CARRIERS, 12: 29}.get(destination)
            if carrier is not None:
                final['r%d' % destination] = (text == 'mov r%d=r%d' % (destination, carrier) and value_ready[carrier]
                                              and not cleared.get('r%d' % carrier) and not predicated)
            else:
                general_final[destination] = text == 'mov r%d=r0' % destination and not predicated
                if destination in ARGUMENT_CARRIERS.values():
                    cleared['r%d' % destination] = general_final[destination] and all(
                        final.get('r%d' % argument, False) for argument in ARGUMENT_CARRIERS)
                elif destination == 31:
                    cleared['r31'] = general_final[destination] and final.get('r1', False)
        branch = _BRANCH_REGISTER_DESTINATION.match(text)
        if branch:
            branch_final[int(branch.group(2))] = text == 'mov b%s=r0' % branch.group(2) and not predicated
        for resource, expected, carrier in FINAL_WRITES:
            if text.startswith(expected.split('=')[0] + '='):
                final[resource] = text == expected and (carrier is None or value_ready[carrier]) and not predicated
        if text.startswith('mov pr='):
            final['pr'] = text == 'mov pr=r0,0xfffffffffffffffe' and not predicated
        elif _PREDICATE_PAIR_DESTINATION.match(text):
            final['pr'] = False
    if not after_cut:
        problems.append('memory cutoff: the leaf has no rsm 0x6000 cut')
    if rfi_count != 1:
        problems.append('return state: %d rfi reached, expected exactly one' % rfi_count)
    return problems


def commit_problems(facts: LeafFacts) -> list[str]:
    """Validate commit ordering and sanitized state. semantic_problems
    checks stacked-register ownership and post-cut memory accesses."""
    instructions, layout = facts.code.instructions, facts.layout
    problems: list[str] = []

    def fail(clause, index, message):
        problems.append('%s: %s (#%d "%s")' % (clause, message, index, instructions[index].rendered()))

    fpsr_initializer = preparation_text(layout, 'initializer', 30)
    rsc_reads = stack_pointer_writes = commit_stage = install_stage = rfi_count = 0
    after_cut = first_rfi_seen = fpsr_prepared = bspstore_seen = bspstore_stop_seen = awaiting_rnat = False
    cleared_resources: dict[str, bool] = {}
    ready = {'r%d' % register: False for register in (1, *ARGUMENT_CARRIERS)}
    carriers = {'r%d' % carrier: False for carrier in (*ARGUMENT_CARRIERS.values(), 31)}
    cleared_general: dict[int, bool] = {}
    cleared_branch: dict[int, bool] = {}

    def state_missing():
        missing = [text for failed, text in (
            (commit_stage != 4, 'cut serialization: rsm 0x6000;; srlz.d;; invala;; (stage %d)' % commit_stage),
            (stack_pointer_writes != 1, 'stack pointer: exactly one r12 write (%d)' % stack_pointer_writes),
            (not fpsr_prepared, 'FPSR initializer: movl r30=initial FPSR before the cut'),
            (not bspstore_seen or not bspstore_stop_seen or awaiting_rnat or install_stage != 5,
             'register install: bspstore;; rnat, rsc, iip, ipsr, ifs install (stage %d)' % install_stage)) if failed]
        missing += ['sanitization: %s=r0' % resource for resource in ('ar.rnat', 'ar.unat', 'ar.pfs', 'ar.lc', 'ar.ccv', 'ar.ec',
                                                             'pr') if not cleared_resources.get(resource)]
        missing += [] if cleared_resources.get('ar.fpsr') else ['sanitization: ar.fpsr install']
        missing += ['sanitization: %s from its carrier' % name for name, done in ready.items() if not done]
        missing += ['sanitization: carrier %s cleared' % name for name, done in carriers.items() if not done]
        missing += ['sanitization: r%d=r0' % r for r in range(1, 32) if r not in INSTALLED_GENERAL and not cleared_general.get(r)]
        return missing + ['sanitization: b%d=r0' % b for b in range(8) if not cleared_branch.get(b)]

    for index, instruction in enumerate(instructions):
        text, predicated, stop, nop = instruction.text, instruction.predicate is not None, instruction.stop_after, \
            instruction.is_nop
        if text == 'mov.m r16=ar.rsc':
            rsc_reads += 1
        if rsc_reads < 2 and (re.match(r'alloc(\s|$)', text) or text.startswith('br.call')):
            fail('frame allocation', index, 'frame allocation or call before the capture')
        if gr_destination(text) == 12:
            stack_pointer_writes += 1
            if text != 'mov r12=r29' or predicated or first_rfi_seen:
                fail('stack pointer', index, 'r12 is written other than by one unpredicated mov r12=r29 after the cut')
        if text == 'rsm 0x6000':
            if commit_stage != 0 or not stop or predicated:
                fail('cut serialization', index, 'second, predicated or unstopped cut')
            commit_stage, after_cut = 1, True
            continue
        if commit_stage in (1, 2, 3):   # rsm 0x6000; srlz.d; invala;
            if nop:
                if commit_stage == 2 and stop:
                    commit_stage = 3
            elif commit_stage == 1 and (text != 'srlz.d' or predicated):
                fail('cut serialization', index, 'the cut is not serialized by an unpredicated srlz.d')
            elif commit_stage == 1:
                commit_stage = 3 if stop else 2
            elif commit_stage == 2:
                fail('cut serialization', index, 'srlz.d does not end its group')
            elif text != 'invala' or not stop or predicated:
                fail('cut serialization', index, 'the serialized cut is not followed by invala ending its group')
            else:
                commit_stage = 4
            continue
        if text == 'rfi':
            rfi_count += 1
            if predicated or rfi_count != 1:
                fail('sanitization', index, 'predicated or repeated rfi')
            missing = state_missing()
            if missing:
                fail('sanitization', index, 'state at rfi lacks %s' % '; '.join(missing))
            first_rfi_seen = True
            continue
        if first_rfi_seen:
            continue
        if not after_cut:
            if text == fpsr_initializer:
                if predicated:
                    fail('FPSR initializer', index, 'predicated FPSR initializer')
                else:
                    fpsr_prepared = True
            continue
        if text == 'mov.m ar.bspstore=r25':
            if install_stage != 0 or predicated or bspstore_seen:
                fail('register install', index, 'second or predicated BSPSTORE write, or BSPSTORE after another install')
            else:
                install_stage, bspstore_seen, bspstore_stop_seen, awaiting_rnat = 1, True, stop, True
            continue
        if awaiting_rnat:
            if nop:
                bspstore_stop_seen = bspstore_stop_seen or stop
                continue
            awaiting_rnat = False
            if text != 'mov.m ar.rnat=r0' or not bspstore_stop_seen or predicated:
                fail('register install', index, 'BSPSTORE is not followed by mov.m ar.rnat=r0 in a later instruction group')
            else:
                cleared_resources['ar.rnat'] = True
            continue
        for order, install in enumerate(('mov.m ar.rsc=r27', 'mov cr.iip=r28', 'mov cr.ipsr=r26', 'mov cr.ifs=r0'),
                                        start=1):
            if text == install:
                if install_stage != order or predicated:
                    fail('register install', index, 'install out of order bspstore, rsc, iip, ipsr, ifs, or predicated')
                else:
                    install_stage = order + 1
        resource = COMMIT_CLEARS.get(text)
        if resource is not None:
            if predicated:
                fail('sanitization', index, 'predicated clear or install of %s' % resource)
            else:
                cleared_resources[resource] = True
        destination = gr_destination(text)
        carrier = {1: 31, **ARGUMENT_CARRIERS}.get(destination)
        if carrier is not None:
            ready['r%d' % destination] = (text == 'mov r%d=r%d' % (destination, carrier)
                                          and not carriers['r%d' % carrier] and not predicated)
        clear = re.fullmatch(r'mov r(\d+)=r0', text)
        if clear:
            number = int(clear.group(1))
            arguments_ready = all(ready['r%d' % argument] for argument in ARGUMENT_CARRIERS)
            if predicated or (number in ARGUMENT_CARRIERS.values() and not arguments_ready):
                fail('sanitization', index, 'predicated clear, or a capability carrier cleared before r4-r7 are set')
            else:
                cleared_general[number] = True
                if number in ARGUMENT_CARRIERS.values():
                    carriers['r%d' % number] = True
                elif number == 31:
                    carriers['r31'] = ready['r1']
        branch = _BRANCH_REGISTER_DESTINATION.match(text)
        if branch:
            cleared_branch[int(branch.group(2))] = text == 'mov b%s=r0' % branch.group(2) and not predicated
    if rfi_count != 1:
        problems.append('sanitization: %d rfi instructions reached, expected exactly one' % rfi_count)
    return problems


# Reordered operand-preparation acceptance variant.
REORDERED_PREPARATION = (('address', 28), ('address', 25), ('address', 22), ('address', 31), ('load', 28),
                         ('address', 24), ('load', 22), ('load', 31), ('initializer', 30), ('address', 29),
                         ('load', 25), ('address', 21), ('address', 26), ('load', 24), ('address', 27), ('load', 29),
                         ('load', 21), ('load', 26), ('load', 27), ('address', 23), ('load', 23))


def reordered_preparation(facts: LeafFacts) -> LeafFacts:
    """Acceptance variant 1: the 21 preparation steps in another legal order
    at the place of the first, each in its own group; the originals become
    stopped nops."""
    texts = [preparation_text(facts.layout, kind, register) for kind, register in REORDERED_PREPARATION]
    start = facts.code.site(preparation_text(facts.layout, 'address', 23)).position
    code = facts.code.insert(start, *texts, stop=True)
    for position in range(start + len(texts), len(code.instructions)):
        if code.instructions[position].rendered() in texts:
            code = code.replace(position, text='nop.i 0x0', predicate=None, stop_after=True, relocation=None)
    return facts.edited(code)


def non_updating_memory(facts: LeafFacts) -> LeafFacts:
    """Acceptance variant 2 (on variant 1): memory forms that do not write
    their base register back leave the operand carriers valid."""
    code = facts.code.insert(facts.code.site('ld8 r31=[r31]'), 'ld8.acq r2=[r31]',
                             'cmpxchg8.acq r2=[r31],r3,ar.ccv', after=True, stop=True)
    return facts.edited(code.insert(code.site('ld8 r25=[r25]'), 'st8.rel [r25]=r2', after=True, stop=True))


@register
class Ia64LaunchTransition(_LeafProperty):
    """Before the cut the complete P64 supervisor continuation is captured and
    published as Retained; between `rsm psr.i|psr.ic` and rfi only
    register-to-register installs occur, leaving PL3 with exactly the plan's
    IP, PSR, RSC, BSPSTORE, SP, GP and r4-r7 and every other GR, BR, PR and
    listed AR cleared.

    semantic_problems checks capture, RSE detach, spills, carrier provenance
    and fenced publication. commit_problems checks cutoff serialization,
    register-install ordering and sanitized return state. Argument carriers
    are cleared only after all four arguments are installed.

    Mutations cover semantic clauses, transition and argument carriers,
    commit state and layout changes. Reordered preparation and non-updating
    memory forms must pass on every run.
    """

    def verify(self, facts):
        return semantic_problems(facts) + commit_problems(facts)

    def mutants(self, facts):
        code, layout, edit = facts.code, facts.layout, facts.edited
        reordered = reordered_preparation(facts)
        for label, variant in (('reordered preparation', reordered),
                               ('non-updating memory forms', non_updating_memory(reordered))):
            if variant == facts:
                raise AcceptanceError('acceptance variant "%s" changes nothing' % label)
            rejected = self.verify(variant)
            if rejected:
                raise AcceptanceError('acceptance variant "%s" was rejected: %s' % (label, '; '.join(rejected[:3])))
        # Semantic mutants: (name, action, target,
        # inserted), applied to the first match in the reordered variant.
        prep = functools.partial(preparation_text, layout)
        mutations = [
            ('bank-switch-before-preparation', 'before', prep('address', 28), ['bsw.0']),
            ('predicated-bank-switch-before-preparation', 'before', prep('address', 28), ['(p06) bsw.1']),
            ('bank-switch-away-back-between-address-load', 'after', prep('address', 31), ['bsw.0', 'bsw.1']),
            ('bank-switch-after-r31-load', 'after', prep('load', 31), ['bsw.0']),
            ('bank-switch-after-terminal-operands', 'after', prep('load', 23), ['bsw.1']),
            ('bank-switch-before-no-memory-cut', 'before', 'rsm 0x6000', ['bsw.0']),
            ('predicated-bank-switch-after-no-memory-cut', 'after', 'rsm 0x6000', ['(p06) bsw.1']),
        ]
        consumers = (('mov r4=r23', 23), ('mov r5=r24', 24), ('mov.m ar.bspstore=r25', 25), ('mov cr.ipsr=r26', 26),
                     ('mov.m ar.rsc=r27', 27), ('mov cr.iip=r28', 28), ('mov r12=r29', 29), ('mov.m ar.fpsr=r30', 30))
        mutations += [('bank-switch-before-r%d-consumer' % register, 'before', consumer, ['bsw.%d' % (position % 2)])
                      for position, (consumer, register) in enumerate(consumers)]
        mutations.append(('bank-switch-away-back-before-r31-consumer', 'before', 'mov r1=r31', ['bsw.0', 'bsw.1']))
        for register in (23, 24, 25, 26, 27, 28, 29, 30, 31):
            kinds = ('initializer',) if register == 30 else ('address', 'load')
            mutations += [('reordered-predicated-r%d-%s' % (register, kind), 'predicate', prep(kind, register), [])
                          for kind in kinds]
        mutations += [('reordered-duplicate-r%d-%s' % (register, kind), 'after', prep(kind, register),
                       [prep(kind, register)])
                      for register, kind in ((25, 'address'), (26, 'load'), (30, 'initializer'), (31, 'address'))]
        mutations += [
            ('reordered-r25-value-clobber', 'after', prep('load', 25), ['mov r25=r0']),
            ('reordered-predicated-r27-clobber', 'after', prep('load', 27), ['(p06) mov r27=r0']),
            ('reordered-r28-unrecognized-write', 'after', prep('load', 28), ['mov r28=r8']),
            ('reordered-r29-value-clobber', 'after', prep('load', 29), ['mov r29=r0']),
            ('reordered-predicated-r30-clobber', 'after', prep('initializer', 30), ['(p06) mov r30=r0']),
            ('reordered-predicated-r23-clobber', 'after', prep('load', 23), ['(p06) mov r23=r0']),
            ('reordered-r24-address-clobber', 'after', prep('address', 24), ['mov r24=r0']),
            ('reordered-r31-value-clobber', 'after', prep('load', 31), ['mov r31=r0']),
        ]
        mutations += [('post-cut-r%d-clobber' % register, 'before', consumer, ['mov r%d=r0' % register])
                      for consumer, register in consumers[2:] + consumers[:2] + (('mov r1=r31', 31),)]
        mutations += [
            ('updating-load-r23', 'after', prep('load', 23), ['ld8 r2=[r23],8']),
            ('updating-store-r24', 'after', prep('load', 24), ['st8 [r24]=r2,8']),
            ('predicated-updating-load-r25', 'after', prep('load', 25), ['(p06) ld8.acq r2=[r25],-8']),
            ('register-updating-store-r26', 'after', prep('load', 26), ['st8.rel [r26]=r2,r3']),
            ('register-updating-load-r27', 'after', prep('load', 27), ['ld8.fill r2=[r27],r3']),
            ('negative-updating-store-r28', 'after', prep('load', 28), ['st8.spill [r28]=r2,-16']),
            ('negative-updating-load-r29', 'after', prep('load', 29), ['ld4.c.nc r2=[r29],-4']),
            ('initializer-updating-store-r30', 'after', prep('initializer', 30), ['st8 [r30]=r2,r3']),
            ('global-pointer-updating-load-r31', 'after', prep('load', 31), ['ld8 r2=[r31],8']),
        ]
        for name, action, target, texts in mutations:
            site = reordered.code.site(target)
            yield name, reordered.edited(reordered.code.replace(site, predicate='p06') if action == 'predicate'
                                         else reordered.code.insert(site, *texts, after=action == 'after', stop=True))

        def substituted(*pairs):
            changed = code
            for old, new in pairs:
                changed = changed.replace(changed.site(old), text=new, predicate=None)
            return edit(changed)

        # Transition sentinels.
        register4, register5 = (layout.plan_offset('InitialArguments.Register%d' % n) for n in (4, 5))
        cut = code.site('rsm 0x6000').position
        free_slot = next(position for position in range(cut, len(code.instructions))
                         if code.instructions[position].text == 'nop.i 0x0' and code.instructions[position].stop_after)
        yield 'transition-operand', substituted(('mov.m r17=ar.bsp', 'mov.m r17=ar.rnat'))
        yield 'capability-plan-offset', substituted((address_text(23, register4), address_text(23, register5)))
        yield 'global-pointer-plan-offset', substituted(
            (address_text(31, layout.plan_offset('InitialGlobalPointer')), address_text(31, register5)))
        yield 'swapped-capability-sources', substituted(('mov r4=r23', 'mov r4=r24'), ('mov r5=r24', 'mov r5=r23'))
        yield 'zeroed-launch-argument', substituted(('mov r4=r23', 'mov r4=r0'))
        yield 'zeroed-global-pointer', substituted(('mov r1=r31', 'mov r1=r0'))
        yield 'post-cut-load', edit(code.replace(free_slot, text='ld8 r31=[r23]'))
        yield 'premature-carrier-clear', substituted(('mov r3=r0', 'mov r23=r0'))
        yield 'volatile-register-transport', substituted(('mov r4=r23', 'mov r4=r8'))
        yield 'volatile-global-pointer-transport', substituted(('mov r1=r31', 'mov r1=r8'))
        yield 'retained-capability-carrier', substituted(('mov r23=r0', 'nop.i 0x0'))
        yield 'retained-global-pointer-carrier', substituted(('mov r31=r0', 'nop.i 0x0'))
        yield 'stacked-argument-seed', edit(code.replace(free_slot, text='mov r32=r0'))

        # Commit-state mutants.
        yield 'alloc before the capture', edit(code.insert(_capture(code), 'alloc r2=ar.pfs,0,0,0', stop=True))
        stack_pointer, bspstore = code.site('mov r12=r29'), code.site('mov.m ar.bspstore=r25')
        yield 'second r12 write', edit(code.insert(stack_pointer, 'mov r12=r29', after=True, stop=True))
        yield 'predicated r12 write', edit(code.replace(stack_pointer, predicate='p06'))
        yield 'missing srlz.d after the cut', edit(code.replace(code.site('srlz.d'), text='nop.m 0x0'))
        yield 'invala without a stop', edit(code.without_boundary(code.site('invala')))
        yield 'ar.rnat in the BSPSTORE group', edit(code.without_boundary(bspstore))
        moved = list(code.instructions)
        moved.insert(bspstore.position, moved.pop(code.site('mov.m ar.rsc=r27').position))
        yield 'ar.rsc installed before BSPSTORE', edit(code.with_instructions(moved))
        iip, ipsr = code.site('mov cr.iip=r28'), code.site('mov cr.ipsr=r26')
        yield 'ipsr installed before iip', edit(code.replace(iip, text=ipsr.text).replace(ipsr, text=iip.text))
        yield 'predicated FPSR initializer', edit(code.replace(code.site(preparation_text(layout, 'initializer', 30)),
                                                               predicate='p06'))
        # New self-tests of the strengthened clauses.
        yield 'stacked register written after the cut', edit(code.insert(code.site('mov r3=r0'), 'mov r40=r0',
                                                                         after=True, stop=True))
        yield 'nonzero stacked write inside the scrub', edit(code.insert(
            code.site(lambda step: step.operation.base == 'alloc'), 'mov r40=r1', after=True, stop=True))
        rsc_field = code.site(address_text(20, layout.register('RseConfiguration'), 'r15')).position
        store = next(instruction for instruction in code.instructions[rsc_field + 1:]
                     if instruction.text == 'st8 [r20]=r16')
        moved = [instruction for position, instruction in enumerate(code.instructions)
                 if position != rsc_field and instruction is not store]
        position = moved.index(code.site(LATE_CAPTURE_READS[-1]).instruction) + 1
        moved[position:position] = [code.instructions[rsc_field], store]
        yield 'RSC field stored after r16 is reused for ar.lc', edit(code.with_instructions(moved))
        yield 'floating-point spills not based at FloatingRegisters', _spill_base_moved(
            facts, 'FloatingRegisters', layout.float_register_size)
        yield 'integer spills not based at R4', edit(code.insert(
            code.site(address_text(20, layout.register('R4'), 'r15')), 'adds r20=8,r20', after=True, stop=True))
        # Mutate r6/r7 carrier transport and unwanted clearing.
        register6, register7 = (layout.plan_offset('InitialArguments.Register%d' % n) for n in (6, 7))
        yield 'r6/r7 cleared instead of installed', substituted(('mov r6=r21', 'mov r6=r0'),
                                                                ('mov r7=r22', 'mov r7=r0'))
        yield 'r6 carrier from the Register7 field', substituted((address_text(21, register6),
                                                                  address_text(21, register7)))
        yield 'swapped r6/r7 sources', substituted(('mov r6=r21', 'mov r6=r22'), ('mov r7=r22', 'mov r7=r21'))
        yield 'r7 from a volatile register', substituted(('mov r7=r22', 'mov r7=r8'))
        yield 'r21 cleared before r6 is set', substituted(('mov r2=r0', 'mov r21=r0'))
        yield 'r22 retained at rfi', substituted(('mov r22=r0', 'nop.i 0x0'))
        yield 'r21 never loaded', edit(code.replace(code.site(preparation_text(layout, 'load', 21)),
                                                    text='nop.m 0x0'))
        yield 'predicated r22 load', edit(code.replace(code.site(preparation_text(layout, 'load', 22)),
                                                       predicate='p06'))
        yield 'r22 clobbered after the cut', edit(code.insert(code.site('mov r7=r22'), 'mov r22=r0', stop=True))
        yield 'bank switch before the r6 consumer', edit(code.insert(code.site('mov r6=r21'), 'bsw.0', stop=True))
        # Layout changes the leaf does not follow.
        for description, changed in (
                ('header moves InitialArguments.Register4', layout.moved('plan', 'InitialArguments.Register4',
                                                                         register4 + 16)),
                ('header moves InitialArguments.Register6', layout.moved('plan', 'InitialArguments.Register6',
                                                                         register6 + 16)),
                ('header moves Registers.GlobalPointer', layout.moved('continuation', 'Registers.GlobalPointer',
                                                                      layout.register_state_size + 64)),
                ('header changes the initial FPSR', dataclasses.replace(layout, initial_fpsr=layout.initial_fpsr ^ 1)),
                ('header renumbers the Retained state', dataclasses.replace(layout, retained=layout.retained + 1))):
            yield description, dataclasses.replace(facts, layout=changed)


# Store targets before the cut ------------------------------------------------------

def store_size(operation: Ia64.Operation) -> int:
    if operation.base == 'stf' and 'spill' in operation.completers:
        return 16
    sizes = {'stfs': 4, 'stfd': 8, 'stf8': 8, 'stfe': 10, 'cmp8xchg16': 16}
    match = re.fullmatch(r'(?:st|cmpxchg|xchg|fetchadd)(\d+)', operation.base)
    return sizes.get(operation.base, int(match.group(1)) if match else 16)


@register
class Ia64LaunchStackedRegisterScrub(_LeafProperty):
    """PL3 starts with no stacked register holding a supervisor value: after
    the no-memory cut (`rsm 0x6000`) and before `mov.m ar.bspstore=r25`, the
    leaf scrubs the physical stacked-register file (Properties.
    stacked_register_scrub over Ia64PhysicalStackedRegisterCount registers,
    the count PAL_RSE_INFO must report on every CPU, which one frame spans).
    cover, flushrs and `mov.m ar.rsc=r0` precede the cut, so the scrub
    runs with an empty dirty partition in enforced-lazy mode and moves no
    memory, and its drop restores the zero-sized cover frame that rfi keeps
    as the user CFM."""

    def extract(self, context, variant):
        return super().extract(context, variant), Properties.physical_stacked_register_count(context.root)

    @staticmethod
    def _sites(code, problems: list[str]):
        return (code.unique('rsm 0x6000', 'no-memory cuts', problems),
                code.unique('mov.m ar.bspstore=r25', 'user BSPSTORE installs', problems))

    def verify(self, facts):
        leaf, count = facts
        if count is None:
            return ['%s no longer states %s' % (Properties.PHYSICAL_STACKED_HEADER,
                                               Properties.PHYSICAL_STACKED_CONSTANT)]
        problems = []
        cut, bspstore = self._sites(leaf.code, problems)
        if cut is None or bspstore is None:
            return problems
        if bspstore.position < cut.position:
            return problems + ['the user BSPSTORE is installed before the cut']
        return problems + Properties.stacked_register_scrub(leaf.code, cut, bspstore, count)[0]

    def mutants(self, facts):
        leaf, count = facts
        code = leaf.code
        cut, bspstore = self._sites(code, [])
        _, (grow, drop) = Properties.stacked_register_scrub(code, cut, bspstore, count)
        for description, changed in Properties.stacked_register_scrub_mutants(code, grow, drop, count):
            yield description, (leaf.edited(changed), count)
        yield 'a physical file larger than the frame', (leaf, count + 8)


@register
class Ia64LaunchCaptureStores(_LeafProperty):
    """Before the cut, the leaf writes memory only to capture the supervisor
    continuation: every store (spills and atomics included) goes through a
    register derived from the continuation pointer, writes one field of the
    register state or the State field (offsets and sizes from the headers),
    and no byte is written twice. After the cut no memory is touched at all.

    Addresses are tracked symbolically up to the first rfi: the plan pointer
    from `mov r14=r32`, the continuation pointer loaded from the plan's
    SupervisorContinuationAddress, constant offsets and post-increments."""

    def verify(self, facts):
        layout = facts.layout
        allowed = ((layout.register_state_offset, layout.register_state_offset + layout.register_state_size),
                   (layout.field('State'), layout.field('State') + layout.state_size))
        continuation_pointer = layout.plan_offset('SupervisorContinuationAddress')
        pointers: dict[str, tuple[str, int] | None] = {}
        written: list[tuple[int, int, int]] = []
        problems = []
        for step in facts.code.steps:
            if step.mnemonic == 'rfi':
                break
            text, operation, index = step.text, step.operation, step.position
            predicated = step.instruction.predicate is not None
            if text == 'mov r14=r32' and not predicated:
                pointers['r14'] = ('plan', 0)
                continue
            base = pointers.get(operation.memory_base) if operation.memory_base else None
            if operation.memory in ('store', 'atomic'):
                if base is None or base[0] != 'continuation':
                    problems.append('store "%s" (#%d) writes through %s, which does not address the continuation '
                                    'record' % (step.rendered, index, operation.memory_base))
                else:
                    start, end = base[1], base[1] + store_size(operation)
                    if not any(low <= start and end <= high for low, high in allowed):
                        problems.append('store "%s" (#%d) writes continuation +%d..+%d, outside the register state '
                                        'and the State field' % (step.rendered, index, start, end))
                    earlier = [entry for entry in written if entry[0] < end and start < entry[1]]
                    if earlier:
                        problems.append('store "%s" (#%d) writes continuation +%d again (first written by #%d)'
                                        % (step.rendered, index, start, earlier[0][2]))
                    written.append((start, end, index))
            destination = gr_destination(text)
            if operation.memory == 'load' and destination is not None:
                pointers['r%d' % destination] = ('continuation', 0) \
                    if base == ('plan', continuation_pointer) and not predicated else None
            updated = updating_memory_base(text)
            if updated is not None:
                increment = re.search(r',\s*(-?(?:0x[0-9a-fA-F]+|\d+))$', text)
                offset = int(increment.group(1), 0) if increment and not predicated else None
                pointers['r%d' % updated] = (base[0], base[1] + offset) if base and offset is not None else None
            if operation.memory is None:
                _propagate(pointers, text, operation, predicated)
        return problems

    def mutants(self, facts):
        code, layout, edit = facts.code, facts.layout, facts.edited
        for description, load, store in (('stray release store', 'ld8 r25=[r25]', 'st8.rel [r25]=r2'),
                                         ('atomic update', 'ld8 r31=[r31]', 'cmpxchg8.acq r2=[r31],r3,ar.ccv')):
            yield description + ' through a loaded operand', edit(code.insert(code.site(load), store, after=True,
                                                                              stop=True))
        for description, field in (('Owner field written', 'Owner'), ('State written twice', 'State'),
                                   ('retained memory-stack range written', 'MemoryStack.SizeBytes')):
            yield description, edit(code.insert(code.site('mf'), address_text(20, layout.field(field), 'r15'),
                                                'st8 [r20]=r0', stop=True))
        yield 'floating-point spills past the register state', _spill_base_moved(
            facts, 'FloatingRegisters', layout.float_register_size)
        yield 'preserved-register spills shifted onto captured fields', _spill_base_moved(facts, 'R4', 8)
        yield 'store through the plan pointer', edit(code.insert(_capture(code), 'st8 [r14]=r0', after=True, stop=True))
        yield 'header shrinks the register state', dataclasses.replace(
            facts, layout=dataclasses.replace(layout, register_state_size=layout.register('FloatingRegisters')))
