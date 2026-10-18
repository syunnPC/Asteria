# SPDX-License-Identifier: Apache-2.0
"""IA-64 runtime region activator.

Checked code in `Source/Platform/Ia64/Memory/Ia64RuntimeRegionActivator.{S,cpp}`:

- the hand-written assembly leaf `AsteriaIa64ActivateRuntimeRegions`, which
  replaces RR1-RR7 with PSR.ic cleared and reads every region register back;
  its golden is Tests/Golden/Ia64/Memory/AsteriaIa64ActivateRuntimeRegions.dis;
- the compiler-generated `Ia64RuntimeRegionActivator::ActivatePhysicalBootstrap`,
  which may call the leaf only after the PSR test (register- and
  layout-agnostic, checked at O0, O2 and in the firmware build).
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Ia64, Properties, Subjects
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.ControlFlow import END, EXIT, MARKERS, RETURN, Code
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import HelperExtents, HelperGroup, HelperProperty, HelperSymbols

LEAF = 'AsteriaIa64ActivateRuntimeRegions'
PHYSICAL_BOOTSTRAP = ('_ZN7Asteria8Platform4Ia646Memory26Ia64RuntimeRegionActivator25ActivatePhysicalBootstrap'
                      'ERKNS2_20Ia64RuntimeRegionSetE')
REGION_ACTIVATOR = HelperGroup('RuntimeRegionActivator', (LEAF,))

# PSR.ic, PSR.i, PSR.pk, PSR.dt, PSR.rt and PSR.it must all be clear.
PHYSICAL_BOOTSTRAP_MASK = 0x100802e000
REJECTED_BEFORE_COMMIT = 2
_REGION_INDEX = re.compile(r'^rr\[(?P<index>r\d+)\]$')


@register
class Ia64RegionActivatorSymbols(HelperSymbols):
    GROUPS = (REGION_ACTIVATOR,)


@register
class Ia64RegionActivatorExtents(HelperExtents):
    GROUPS = (REGION_ACTIVATOR,)


# The RR-write leaf ----------------------------------------------------------------------------

def _region_write(step) -> str | None:
    return next((w for w in step.operation.writes if w.startswith('rr[')), None)


@register
class Ia64RegionActivatorLeaf(HelperProperty):
    """The runtime RR-write leaf (G(i) is the instruction group of instruction i):

    - (a) every `mov rr[rX]=` is unpredicated and followed by `srlz.d` and
      `srlz.i` in later groups before any dependent instruction (an RR read,
      memory access, branch, PSR write or translation-cache change);
    - (b) exactly one `rsm 0x2000` (PSR.ic) precedes the first RR write,
      serialized by `srlz.d` and `srlz.i` in groups between them; no other
      PSR bit is written;
    - (c) nothing accesses memory or calls while PSR.ic is clear;
    - (d) every RR index is a `movl` constant of a nonzero region (RR0, the
      shared bootstrap region, is never written), no region twice;
    - (e) no alloc, call, memory access, gp/sp use or relocation;
    - (f) every path from the first RR write to a return passes the
      `ssm 0x2000` restore, which is serialized by `srlz.d` and `srlz.i`;
    - (g) that restore is predicated on the entry-PSR.ic-set predicate of a
      `tbit.z` of the single PSR capture taken before PSR.ic is cleared."""
    GROUPS = (REGION_ACTIVATOR,)

    def item_problems(self, code):
        problems = []

        def text(step):
            return '%s (#%d)' % (step.rendered, step.position)

        def serializes(step, after):
            return (step.mnemonic in ('srlz.d', 'srlz.i') and step.group > after.group
                    and not step.operation.is_predicated)

        problems += ['(e) %s allocates, calls, accesses memory or uses gp/sp' % text(step) for step in code.steps
                     if step.operation.base == 'alloc' or step.operation.is_call or step.operation.memory is not None
                     or {Ia64.GLOBAL_POINTER, Ia64.STACK_POINTER} & (step.operation.reads | step.operation.writes)]
        problems += ['(e) relocation %s inside the leaf' % (fact,) for fact in code.relocations]
        writes = code.all(_region_write)
        if not writes:
            return problems + ['no region-register write']

        for write in writes:
            if write.operation.is_predicated:
                problems.append('(a) %s is predicated' % text(write))
            serialized = set()
            for later in code.window(after=write):
                operation = later.operation
                if serializes(later, write):
                    serialized.add(later.mnemonic)
                    if len(serialized) == 2:
                        break
                    continue
                if (any(r.startswith('rr[') for r in operation.reads) or operation.memory is not None
                        or operation.is_branch or 'psr' in operation.writes or 'tlb' in operation.writes):
                    problems.append('(a) %s depends on %s before srlz.d and srlz.i' % (text(later), text(write)))
                    break
            else:
                if len(serialized) != 2:
                    problems.append('(a) %s is never serialized by srlz.d and srlz.i' % text(write))

        clears = code.all(lambda step: step.operation.base == 'rsm' and step.operation.immediates == (Ia64.PSR_IC,))
        problems += ['(b) %s writes the PSR other than PSR.ic' % text(step) for step in code.steps
                     if 'psr' in step.operation.writes and step.operation.immediates != (Ia64.PSR_IC,)]
        if len(clears) != 1:
            problems.append('(b) %d rsm 0x2000, expected exactly one' % len(clears))
        else:
            clear = clears[0]
            if clear.operation.is_predicated or not clear.position < writes[0].position:
                problems.append('(b) %s is predicated or does not precede the first RR write' % text(clear))
            if {step.mnemonic for step in code.window(after=clear, before=writes[0])
                    if serializes(step, clear) and step.group < writes[0].group} != {'srlz.d', 'srlz.i'}:
                problems.append('(b) PSR.ic is not serialized by srlz.d and srlz.i before %s' % text(writes[0]))

        regions = []
        for write in writes:
            register_name = _REGION_INDEX.match(_region_write(write)).group('index')
            definitions = [step for step in code.window(before=write) if register_name in step.operation.general_writes]
            definition = definitions[-1].operation if definitions else None
            if definition is None or definition.base != 'movl' or definition.is_predicated \
                    or len(definition.immediates) != 1:
                problems.append('(d) the index of %s is not a region constant' % text(write))
                continue
            if definition.immediates[0] >> 61 == 0:
                problems.append('(d) %s writes RR0' % text(write))
            regions.append(definition.immediates[0] >> 61)
        if len(set(regions)) != len(regions):
            problems.append('(d) a region register is written twice: regions %s' % regions)

        restores = code.all(lambda step: step.operation.base == 'ssm' and step.operation.immediates == (Ia64.PSR_IC,))
        if len(restores) != 1:
            return problems + ['(f) %d ssm 0x2000, expected exactly one' % len(restores)]
        restore = restores[0]
        if len(clears) == 1:
            problems += ['(c) %s runs while PSR.ic is clear' % text(step) for step in code.window(after=clears[0],
                                                                                                  before=restore)
                         if step.operation.memory is not None or step.operation.is_call]
        escapes = code.graph.reachable([writes[0].position], avoiding={restore.position}) & MARKERS
        if escapes:
            problems.append('(f) a path from the first RR write reaches %s without the PSR.ic restore'
                            % ', '.join(sorted(escapes)))
        serialized = set()
        for step in code.window(after=restore):
            if step.operation.is_branch:
                break
            if step.mnemonic in ('srlz.d', 'srlz.i') and step.group > restore.group:
                serialized.add(step.mnemonic)
        if serialized != {'srlz.d', 'srlz.i'}:
            problems.append('(f) the PSR.ic restore is not serialized by srlz.d and srlz.i before the return')
        captures = code.all(lambda step: 'psr' in step.operation.reads)
        saved = sorted(captures[0].operation.general_writes) if len(captures) == 1 else []
        if len(saved) != 1 or captures[0].operation.is_predicated or code.all(lambda step: saved[0] in step.operation.general_writes) != captures \
                or not clears or not captures[0].position < clears[0].position:
            return problems + ['(g) expected one PSR capture, defined once, before PSR.ic is cleared']
        tests = [step for step in code.steps if (match := Ia64.TBIT.match(step.text)) is not None
                 and match.group('source') == saved[0] and int(match.group('bit')) == Ia64.PSR_IC.bit_length() - 1
                 and not step.instruction.predicate]
        if len(tests) != 1:
            return problems + ['(g) %d tbit.z tests of entry PSR.ic, expected exactly one' % len(tests)]
        enabled = Ia64.canonical_predicate(Ia64.TBIT.match(tests[0].text).group('set'))
        if restore.predicate != enabled:
            problems.append('(g) %s is not predicated on entry PSR.ic being set (%s)' % (text(restore), enabled))
        if not tests[0].position < restore.position or any(step.operation.writes_predicate(enabled)
                                                           for step in code.window(after=tests[0], before=restore)):
            problems.append('(g) the PSR.ic predicate does not reach %s' % text(restore))
        return problems

    def item_mutants(self, code):
        writes = code.all(_region_write)
        first_index = _REGION_INDEX.match(_region_write(writes[0])).group('index')
        region_constant = [step for step in code.window(before=writes[0])
                           if first_index in step.operation.general_writes][-1]
        clear = code.first(lambda step: step.operation.base == 'rsm')
        restore = code.first(lambda step: step.operation.base == 'ssm')
        returns = code.all(lambda step: step.operation.is_return)
        after_writes = code.window(after=writes[-1])
        serialize_d = next(step for step in after_writes if step.mnemonic == 'srlz.d')
        readback = next(step for step in after_writes if any(r.startswith('rr[') for r in step.operation.reads))
        success = code.first(lambda step: step.operation.is_branch and not step.operation.is_predicated
                             and step.target is not None)
        test = next(match for step in code.steps if (match := Ia64.TBIT.match(step.text)))
        yield 'RR1 index materialized as region 0', code.rewrite(region_constant,
                                                                 '%s=0x0' % region_constant.text.split('=')[0])
        capture = code.first(lambda step: 'psr' in step.operation.reads)
        yield 'predicated entry PSR capture', code.rewrite(capture, '(p06) ' + capture.text)
        yield 'every stop removed', code.without_stops(0, len(code.instructions))
        yield 'first RR write predicated', code.rewrite(writes[0], '(p06) %s' % writes[0].text)
        yield 'success path branches past the PSR.ic restore', code.retarget(success, returns[-1].bundle)
        yield 'srlz.i after the RR writes removed', code.rewrite(
            next(step for step in after_writes if step.mnemonic == 'srlz.i'), 'nop.m 0x0')
        yield 'RR read back before srlz.d', code.swap(serialize_d, readback)
        yield 'srlz.d after rsm 0x2000 removed', code.rewrite(
            code.first(lambda step: step is clear, lambda step: step.mnemonic == 'srlz.d'), 'nop.m 0x0')
        yield 'PSR.ic never cleared', code.rewrite(clear, 'nop.m 0x0')
        yield 'PSR.ic restore unpredicated', code.rewrite(restore, restore.text)
        yield 'PSR.ic restore on the entry-clear predicate', code.rewrite(
            restore, '(%s) %s' % (test.group('clear'), restore.text))
        yield 'region written twice', code.rewrite(writes[1], writes[1].text.replace(
            _region_write(writes[1]), 'rr[%s]' % first_index))
        yield 'load while PSR.ic is clear', code.insert(code.next(clear), 'ld8 r21=[r14]', stop=True)
        yield 'call in the leaf', code.insert(returns[0], 'br.call.sptk.many b0=<AsteriaIa64UnexpectedCallee>',
                                              stop=True)
        yield 'relocation inside the leaf', dataclasses.replace(code, relocations=code.relocations + (
            RelocationFact(0x10, 'LTOFF22X', '.rodata', 0),))


# The physical-bootstrap wrapper (compiler-generated C++) ------------------------------------------

_CMP_ZERO = re.compile(r'^cmp\.eq (?P<equal>p\d+),(?P<different>p\d+)=(?P<left>\w+),(?P<right>\w+)$')


def evaluate(operation: Ia64.Operation, state: dict, index: int) -> dict:
    """Constant propagation over general registers: register -> (value, origin index)."""
    targets = operation.general_writes
    if not targets:
        return state
    state = dict(state)

    def value(register):
        return (0, None) if register == 'r0' else state.get(register)

    sources = sorted(operation.general_reads)
    result = None
    if not operation.is_predicated and operation.memory is None and len(targets) == 1:
        if operation.base == 'mov' and not sources and len(operation.immediates) == 1:
            result = (operation.immediates[0], index)
        elif operation.base == 'mov' and len(sources) == 1 and not operation.immediates:
            result = value(sources[0])
        elif operation.base in ('adds', 'addl') and len(sources) == 1 and len(operation.immediates) == 1:
            base = value(sources[0])
            result = None if base is None else ((base[0] + operation.immediates[0]) & ((1 << 64) - 1), index)
        elif operation.base in ('zxt1', 'zxt2', 'zxt4') and len(sources) == 1:
            base = value(sources[0])
            width = {'zxt1': 8, 'zxt2': 16, 'zxt4': 32}[operation.base]
            result = None if base is None else (base[0] & ((1 << width) - 1), base[1])
    for target in targets:
        state[target] = result if len(targets) == 1 else None
    return state


def returned_values(code: Code, source) -> tuple[list, list[str]]:
    """(r8 value and origin at every return reachable from `source`, problems)."""
    results, problems = [], []
    pending, visited = [(source, {})], set()
    while pending:
        node, state = pending.pop()
        if node in (EXIT, END):
            problems.append('the mask-set path leaves the function without returning')
            continue
        key = (node, tuple(sorted(state.items())))
        if key in visited:
            continue
        visited.add(key)
        if len(visited) > 4096:
            problems.append('the mask-set path is too long to evaluate')
            break
        operation = code.operations[node]
        if operation.is_call:
            problems.append('the mask-set path calls %s' % code.instructions[node].rendered())
            continue
        if operation.is_return:
            results.append(state.get('r8'))
            if not operation.is_predicated:
                continue
        state = evaluate(operation, state, node)
        pending.extend((successor, state) for successor in code.graph.successors[node] if successor != RETURN)
    return results, problems


def psr_mask_values(code: Code, capture, mask, test) -> list:
    """Values at the AND: follow copies and exact local-stack spills at O0.

    Unknown writes, partial/conditional stores, calls and non-dominating
    definitions invalidate the value. Only the captured PSR and the one mask
    definition carry those identities; a same-named overwritten GR does not.
    """
    cache = {}

    def across_call(register, value, definition, use):
        calls = [step for step in code.window(after=definition, before=use) if step.operation.is_call]
        if not calls:
            return value
        # The entry SP and alloc's stacked locals preserve stack addresses.
        # Other values and escaped spill slots are conservatively unknown.
        allocs = [step for step in code.window(before=use) if step.operation.base == 'alloc'
                  and code.graph.dominates(step.position, use)]
        sol = Ia64.alloc_frame(allocs[-1].operation)[1] if allocs else 0
        preserved = register == 'r12' or 32 <= int(register[1:]) < 32 + sol
        return value if value is not None and value[0] == 'stack' and preserved else None

    def value_at(register, use, depth=0):
        if register == 'r0':
            return ('constant', 0)
        if depth > 64:
            return None
        key = register, use
        if key in cache:
            return cache[key]
        definitions = [step for step in code.window(before=use) if register in step.operation.general_writes]
        if not definitions:
            return ('stack', 0) if register == 'r12' else None
        definition = definitions[-1]
        operation = definition.operation
        value = None
        if operation.is_predicated or not code.graph.dominates(definition.position, use):
            return None
        if definition is capture:
            value = ('psr', capture.position)
        elif definition is mask:
            value = ('mask', mask.position)
        elif operation.base == 'mov' and len(operation.general_reads) == 1 and not operation.immediates:
            value = value_at(next(iter(operation.general_reads)), definition.position, depth + 1)
        elif operation.base in ('adds', 'addl') and len(operation.general_reads) == 1 and len(operation.immediates) == 1:
            source = value_at(next(iter(operation.general_reads)), definition.position, depth + 1)
            if source is not None and source[0] == 'stack':
                value = ('stack', source[1] + operation.immediates[0])
        elif operation.base == 'ld8' and operation.memory == 'load':
            address = value_at(operation.memory_base, definition.position, depth + 1)
            if address is not None and address[0] == 'stack':
                for store in reversed(code.window(before=definition)):
                    if store.operation.is_call:
                        break
                    if store.operation.memory not in ('store', 'atomic'):
                        continue
                    stored = value_at(store.operation.memory_base, store.position, depth + 1)
                    width = re.fullmatch(r'st(1|2|4|8)', store.operation.base)
                    if stored is None or stored[0] != 'stack' or width is None:
                        break  # may alias the spill
                    size = int(width[1])
                    if stored[1] < address[1] + 8 and address[1] < stored[1] + size:
                        if size == 8 and stored == address and not store.operation.is_predicated \
                                and code.graph.dominates(store.position, definition.position):
                            sources = Ia64.split(store.text)[2]
                            value = value_at(sources[0], store.position, depth + 1)
                        break
        value = across_call(register, value, definition, use)
        cache[key] = value
        return value

    return [value_at(register, test.position) for register in sorted(test.operation.general_reads)]


@dataclasses.dataclass(frozen=True)
class WrapperRoles:
    mask: object
    capture: object
    test: object
    compare: object
    branch: object
    call: object
    clear_edge: tuple
    set_edge: tuple


def wrapper_roles(code: Code) -> tuple[WrapperRoles | None, list[str]]:
    problems = []
    masks = code.all(lambda step: step.operation.base == 'movl'
                     and step.operation.immediates == (PHYSICAL_BOOTSTRAP_MASK,))
    if len(masks) != 1:
        return None, ['%d movl of the PSR mask %#x, expected exactly one' % (len(masks), PHYSICAL_BOOTSTRAP_MASK)]
    mask = masks[0]
    mask_register = sorted(mask.operation.general_writes)[0]
    tests = code.all(lambda step: step.operation.base == 'and' and mask_register in step.operation.reads
                     and not step.operation.immediates, after=mask)
    if len(tests) != 1:
        return None, ['%d and of the mask register %s, expected exactly one' % (len(tests), mask_register)]
    test = tests[0]
    if mask.operation.is_predicated or not code.graph.dominates(mask.position, test.position):
        problems.append('the mask definition must unconditionally dominate the and')
    if any(mask_register in step.operation.general_writes or step.operation.is_call
           for step in code.window(after=mask, before=test)):
        problems.append('the mask register %s is redefined before the and' % mask_register)
    captures = code.all(lambda step: 'psr' in step.operation.reads)
    if len(captures) != 1 or not captures[0].position < test.position:
        return None, problems + ['%d PSR reads, expected exactly one before the mask test' % len(captures)]
    capture = captures[0]
    captured = sorted(capture.operation.general_writes)
    if len(captured) != 1 or capture.operation.is_predicated or test.operation.is_predicated:
        problems.append('the PSR capture and mask test must be unconditional')
    values = psr_mask_values(code, capture, mask, test)
    if None in values or set(values) != {('psr', capture.position), ('mask', mask.position)}:
        problems.append('the mask test does not AND the captured PSR with the mask')
    if not code.graph.dominates(capture.position, test.position):
        problems.append('the PSR capture does not dominate the mask test')
    tested = sorted(test.operation.general_writes)[0]
    compares = [(step, match) for step in code.window(after=test) if (match := _CMP_ZERO.match(step.text))
                and not step.instruction.predicate
                and {match.group('left'), match.group('right')} in ({tested, '0'}, {tested, 'r0'})]
    if len(compares) != 1:
        return None, problems + ['%d 64-bit cmp.eq of %s with zero, expected exactly one' % (len(compares), tested)]
    compare, match = compares[0]
    if not code.graph.dominates(test.position, compare.position):
        problems.append('the mask test does not dominate the compare')
    if any(tested in step.operation.general_writes for step in code.window(after=test, before=compare)):
        problems.append('%s is redefined between the and and the compare' % tested)
    clear = Ia64.canonical_predicate(match.group('equal'))
    branch = code.first(lambda step: step is compare, lambda step: step.operation.is_branch)
    operation = branch.operation if branch is not None else None
    if operation is None or operation.is_call or operation.predicate not in (clear, Ia64.canonical_predicate(
            match.group('different'))) or branch.target is None:
        return None, problems + ['the first branch after the compare is not a conditional branch on its result']
    if any(step.operation.writes_predicate(operation.predicate) for step in code.window(after=compare, before=branch)):
        problems.append('the compare result is redefined before the branch')
    references = Properties.branch_references(code.instructions, LEAF)
    if len(references) != 1:
        return None, problems + Properties.call_problems(code.instructions, LEAF)
    problems += Properties.call_problems(code.instructions, LEAF)
    taken, fall = code.graph.successors[branch.position]
    clear_edge, set_edge = ((branch.position, taken), (branch.position, fall)) if operation.predicate == clear \
        else ((branch.position, fall), (branch.position, taken))
    return WrapperRoles(mask, captures[0], test, compare, branch, code.step(references[0]), clear_edge,
                        set_edge), problems


@register
class Ia64RegionActivatorPhysicalWrapper(Property):
    """`ActivatePhysicalBootstrap` (compiler-generated, register- and
    layout-agnostic) calls the RR leaf only after verifying that PSR.ic, i,
    pk, dt, rt and it are all clear: exactly one `movl` of the mask
    0x100802e000, one `and` of it with the only PSR read, one 64-bit
    `cmp.eq` of the result with zero, and a conditional branch on that
    compare; exactly one unpredicated direct call to the leaf, reachable only
    through the mask-clear edge of that branch; and every path from the
    mask-set edge returns RejectedBeforeCommit (2) in r8 without a call."""
    isas = ('Ia64',)

    def extract(self, context, variant):
        return Subjects.code(context, context.defining_object(variant, PHYSICAL_BOOTSTRAP), PHYSICAL_BOOTSTRAP)

    def verify(self, code):
        roles, problems = wrapper_roles(code)
        if roles is None:
            return problems
        graph = code.graph
        if roles.call.position not in graph.reachable([graph.entry]):
            problems.append('the call to %s is unreachable' % LEAF)
        if roles.call.position in graph.reachable([graph.entry], removed={roles.clear_edge}):
            problems.append('%s is reachable without passing the mask-clear edge of the PSR test' % LEAF)
        values, found = returned_values(code, roles.set_edge[1])
        problems += found
        if not values:
            problems.append('the mask-set path never returns')
        problems += ['the mask-set path returns %s, not RejectedBeforeCommit (%d)'
                     % ('an unknown value' if value is None else value[0], REJECTED_BEFORE_COMMIT)
                     for value in values if value is None or value[0] != REJECTED_BEFORE_COMMIT]
        return problems

    def mutants(self, code):
        roles, _ = wrapper_roles(code)
        call, branch = roles.call, roles.branch
        match = _CMP_ZERO.match(roles.compare.text)
        other = match.group('different') if branch.predicate == Ia64.canonical_predicate(match.group('equal')) \
            else match.group('equal')
        tested = sorted(roles.test.operation.general_writes)[0]
        yield 'mask without PSR.ic', code.rewrite(roles.mask, roles.mask.text.replace(
            hex(PHYSICAL_BOOTSTRAP_MASK), hex(PHYSICAL_BOOTSTRAP_MASK & ~Ia64.PSR_IC)))
        yield 'predicated call', code.rewrite(call, '(p06) %s' % call.text)
        yield 'second call site', code.insert(code.next(call), call.text, stop=True)
        yield 'call hoisted before the PSR test', code.rewrite(call, 'nop.b 0x0').insert(roles.capture, call.rendered,
                                                                                          stop=True)
        yield 'mask test inverted', code.rewrite(branch, '(%s) %s' % (other, branch.text))
        captured = sorted(roles.capture.operation.general_writes)[0]
        mask_register = sorted(roles.mask.operation.general_writes)[0]
        psr_operand = next(r for r in roles.test.operation.general_reads if r != mask_register)
        yield 'and uses zero instead of PSR', code.rewrite(roles.test, re.sub(
            r'(?<=[=,])%s(?=,|$)' % psr_operand, 'r0', roles.test.text))
        yield 'captured PSR clobbered', code.insert(code.next(roles.capture), 'mov %s=r0' % captured, stop=True)
        yield 'predicated PSR capture', code.rewrite(roles.capture, '(p06) ' + roles.capture.text)
        yield 'predicated mask definition', code.rewrite(roles.mask, '(p06) ' + roles.mask.text)
        yield 'PSR not read', code.rewrite(roles.capture, re.sub(r'=psr$', '=r0', roles.capture.text))
        yield '32-bit compare', code.rewrite(roles.compare, roles.compare.text.replace('cmp.eq', 'cmp4.eq'))
        yield 'compare of another register', code.rewrite(roles.compare, re.sub(
            r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % tested, 'r0', roles.compare.text))
        values, _ = returned_values(code, roles.set_edge[1])
        for origin in sorted({value[1] for value in values if value is not None and value[1] is not None})[:1]:
            yield 'mask-set path returns another status', code.rewrite(origin, re.sub(
                r'=\d+$', '=%d' % (REJECTED_BEFORE_COMMIT + 1), code.instructions[origin].text))
