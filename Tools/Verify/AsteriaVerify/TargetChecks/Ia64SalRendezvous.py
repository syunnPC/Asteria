# SPDX-License-Identifier: Apache-2.0
"""IA-64 SAL boot rendezvous: the raw handler entry, the SAL_SET_VECTORS
bridge, their C++ ABI fixtures and the registration stack/RSE budget.
Goldens pin the assembly (the entry and its
three getters under Tests/Golden/Ia64/Interrupt, the bridge under
Tests/Golden/Ia64/Firmware); these properties state what must survive a
golden update and what depends on compiler output. Objects are
located by the symbols they define.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import CallGraph, Disassembly, Ia64, Layout, Properties, Relocations, Subjects
from AsteriaVerify.Checks import ItemProperty, Property, register
from AsteriaVerify.Elf import STT_SECTION, load

ENTRY = 'AsteriaIa64SalBootRendezvousHandlerEntry'
ENTRY_GETTER = 'AsteriaIa64GetSalBootRendezvousEntryAddress'
GETTERS = ('AsteriaIa64ReadCurrentCrLid', ENTRY_GETTER, 'AsteriaIa64GetSalBootRendezvousGlobalPointerAddress')
DISPATCHER = 'AsteriaIa64DispatchSalBootRendezvous'
CONTINUATION = 'AsteriaIa64RunApplicationProcessorOwnedExecutionContext'
BRIDGE = 'AsteriaIa64InvokePhysicalSalSetVectors'
REGISTER = ('_ZN7Asteria8Platform4Ia648Firmware29SalBootRendezvousRegistration8RegisterERKNS2_20SalEntrypointBinding'
            'ERKNS2_25SalSystemTableInformationERKNS2_24SalBootRendezvousHandlerE')
REGISTER_PATTERN = 'SalBootRendezvousRegistration8Register'
RETURN_FIXTURE = 'AsteriaIa64SalBootRendezvousReturnAbiFixture'
BRIDGE_FIXTURE = 'AsteriaIa64SalSetVectorsBridgeAbiFixture'
_HANDLER = '_ZN7Asteria8Platform4Ia649Interrupt28Ia64SalBootRendezvousHandler'
RETURN_FIXTURE_DEPENDENCIES = frozenset({
    DISPATCHER, _HANDLER + '16ReadCurrentCrLidEv', _HANDLER + '23GetPhysicalEntryAddressEv',
    _HANDLER + '31GetPhysicalGlobalPointerAddressEv'})
RESULT_REGISTERS = ('r8', 'r9', 'r10', 'r11')
SAL_ARGUMENTS = 8
# SalRegistrationFreeStorageBytes = SAL's 16 KiB minimum + this reserve,
# for the memory stack and the RSE backing store alike.
ADAPTER_RESERVE_BYTES = 1024
RESERVE_HEADER = 'Source/Platform/Ia64/Firmware/SalBootRendezvousRegistration.hpp'
RESERVE_CONSTANT = 'SalRegistrationAdapterReserveBytes'


@register
class Ia64SalRendezvousUnwindFree(ItemProperty):
    """SAL enters the rendezvous handler at a raw bundle address with only the
    registered GP, and the firmware image has no unwinder and no descriptors
    beyond its entry. The objects of the handler entry, its dispatcher, the
    SAL_SET_VECTORS bridge and `SalBootRendezvousRegistration::Register`
    therefore carry no `.IA_64.unwind*` or `.opd` section and no SEGREL or
    FPTR relocation (GAS unwind directives emit SEGREL, function pointers
    FPTR; the PE closure represents neither)."""
    isas = ('Ia64',)

    def extract(self, context, variant):
        facts = {}
        for symbol in (ENTRY, DISPATCHER, BRIDGE, REGISTER):
            path = Subjects.defining(context, variant, symbol)
            if path is None:
                facts[symbol] = None
                continue
            elf = load(path)
            kinds = tuple(sorted({Relocations.name(elf.machine, relocation.type)
                                  for rela in elf.relocation_sections() for relocation in elf.relocations(rela)}))
            facts[str(path)] = (tuple(section.name for section in elf.sections if section.name), kinds)
        return facts

    def item_problems(self, record):
        if record is None:
            return ['no unique object defines it']
        sections, kinds = record
        return (['forbidden section %s' % name for name in sections
                 if name.startswith('.IA_64.unwind') or name == '.opd']
                + ['forbidden %s relocation' % kind for kind in kinds if kind.startswith('SEGREL') or 'FPTR' in kind])

    def item_mutants(self, record):
        sections, kinds = record
        for name in ('.IA_64.unwind', '.IA_64.unwind_info', '.opd'):
            yield 'adds %s' % name, (sections + (name,), kinds)
        for kind in ('SEGREL64LSB', 'FPTR64LSB', 'LTOFF_FPTR22'):
            yield 'adds a %s relocation' % kind, (sections, tuple(sorted(kinds + (kind,))))


@dataclasses.dataclass(frozen=True, order=True)
class SiteRelocation:
    object: str
    site: str       # The function containing r_offset
    type: str
    target: str


@register
class Ia64SalRendezvousHandlerRelocations(Property):
    """The handler publishes a raw entry address and reaches C++ only through
    direct IP-relative calls: exactly one IMM64 relocation names the raw
    entry, inside `AsteriaIa64GetSalBootRendezvousEntryAddress` (a `movl`,
    not a descriptor), and exactly one PCREL21B names the C++ dispatcher and
    one the AP continuation, both inside the raw entry."""
    isas = ('Ia64',)

    def extract(self, context, variant):
        relocations = []
        paths = {Subjects.defining(context, variant, symbol) for symbol in (ENTRY, ENTRY_GETTER, DISPATCHER)}
        for path in sorted(path for path in paths if path is not None):
            elf = load(path)
            for rela in elf.relocation_sections():
                for relocation in elf.relocations(rela):
                    symbol = relocation.symbol
                    if symbol is None:
                        continue
                    target = (Disassembly.describe_address(elf, symbol.section_index, relocation.addend)
                              if symbol.type == STT_SECTION else symbol.name)   # A section symbol names its function
                    site = Disassembly.describe_address(elf, rela.info, relocation.offset).split('+')[0]
                    relocations.append(SiteRelocation(path.name, site, Relocations.name(elf.machine, relocation.type),
                                                      target))
        return tuple(sorted(relocations))

    def verify(self, facts):
        problems = []
        for kind, target, site in (('IMM64', ENTRY, ENTRY_GETTER), ('PCREL21B', DISPATCHER, ENTRY),
                                   ('PCREL21B', CONTINUATION, ENTRY)):
            found = [r for r in facts if r.type == kind and r.target == target]
            if len(found) != 1:
                problems.append('%d %s relocations against %s, expected exactly one' % (len(found), kind, target))
            elif found[0].site != site:
                problems.append('the %s against %s lies in %s, not %s' % (kind, target, found[0].site, site))
        return problems + ['%s relocation against the raw entry in %s' % (r.type, r.site) for r in facts
                           if r.target == ENTRY and r.type != 'IMM64']

    def mutants(self, facts):
        dispatcher = next(r for r in facts if r.target == DISPATCHER and r.type == 'PCREL21B')
        entry = next(r for r in facts if r.target == ENTRY and r.type == 'IMM64')
        continuation = next(r for r in facts if r.target == CONTINUATION)

        def changed(old, **changes):
            return tuple(sorted(dataclasses.replace(r, **changes) if r == old else r for r in facts))

        yield 'second dispatcher call', tuple(sorted(facts + (dataclasses.replace(dispatcher, site=ENTRY_GETTER),)))
        yield 'no continuation call', tuple(r for r in facts if r != continuation)
        yield 'continuation called outside the entry', changed(continuation, site=GETTERS[0])
        yield 'entry address from another getter', changed(entry, site=GETTERS[0])
        yield 'IMM64 names another symbol', changed(entry, target=ENTRY_GETTER)
        yield 'entry address as data', tuple(sorted(facts + (dataclasses.replace(entry, type='DIR64LSB'),)))


@register
class Ia64SalRendezvousHandlerSymbols(Subjects.SymbolContract):
    """The raw entry and its three getters are global functions of whole
    bundles: each is defined once as a GLOBAL FUNC (default or hidden
    visibility) in a section whose flags are exactly ALLOC|EXECINSTR, with a
    16-byte-aligned address and a non-zero size that is a multiple of 16
    inside its section. C++ calls the getters, and the address handed to SAL
    must be bundle-aligned code. (The goldens pin the sizes.)"""
    SYMBOLS = (ENTRY,) + GETTERS
    CONTRACT = Properties.FunctionContract(Properties.CALLABLE, alignment=16, size_multiple=16)


def _targets(code) -> list[tuple[int, int | None]]:
    """(branch position, target position) of every internal direct branch."""
    return [(step.position, getattr(code.target(step), 'position', None))
            for step in code.steps if step.operation.is_branch and step.target is not None]


@register
class Ia64SalRendezvousEntryControlFlow(Subjects.CodeProperty):
    """Every exit of the raw SAL entry is deliberate. Its failure branches,
    and the branch after the AP continuation call, all enter the single park
    loop (`hint` plus an unconditional branch back to it); exactly one branch
    enters the SAL-return tail, which restores b0, ar.pfs and gp before the
    entry's only `br.ret`; and the continuation call never falls through into
    that tail. (The golden pins the instructions; this states which target is
    which, so a golden update cannot silently retarget a branch.)"""
    SYMBOL = ENTRY

    def verify(self, code):
        pauses = code.all(lambda s: s.operation.base == 'hint')
        returns = code.all(lambda s: s.operation.is_return)
        if len(pauses) != 1 or len(returns) != 1:
            return ['%d pause instructions and %d returns, expected one each' % (len(pauses), len(returns))]
        park, ret = pauses[0].position, returns[0].position
        branches = dict(_targets(code))

        def parks(step):
            return step is not None and branches.get(step.position) == park and not step.instruction.predicate

        if not parks(code.next(park)):
            return ['the pause is not followed by an unconditional branch back to it']
        heads = sorted({target for target in branches.values() if target is not None and target != park})
        if len(heads) != 1:
            return ['branches enter %d places other than the park loop, expected only the return tail' % len(heads)]
        tail = heads[0]         # The return tail runs from its head straight to the br.ret
        between = [step.operation for step in code.steps if tail <= step.position < ret]
        problems = []
        if tail > ret or any(operation.is_branch for operation in between):
            problems.append('the return tail at #%d does not run straight to the br.ret' % tail)
        missing = {'b0', 'ar.pfs', Ia64.GLOBAL_POINTER} - set().union(*(operation.writes for operation in between))
        if missing:
            problems.append('the return tail does not restore %s' % ', '.join(sorted(missing)))
        entering = [index for index, target in branches.items() if target == tail]
        if len(entering) != 1:
            problems.append('%d branches enter the return tail, expected one' % len(entering))
        calls = code.all(lambda s: s.operation.is_call)
        if calls and not parks(code.next(calls[-1])):
            problems.append('the continuation call is not followed by an unconditional branch to the park loop')
        return problems

    def mutants(self, code):
        branches = _targets(code)
        park = code.site(lambda s: s.operation.base == 'hint').position
        park_text = code.instructions[next(b for b, t in branches if t == park)].text
        tail_branch, tail = next((b, t) for b, t in branches if t != park)
        tail_text = code.instructions[tail_branch].text
        yield 'failure branch enters the return tail', code.replace(
            next(b for b, t in branches if t == park and code.instructions[b].predicate), text=tail_text)
        yield 'SAL return enters the park loop', code.replace(tail_branch, text=park_text)
        yield 'park loop exits to the return tail', code.replace(next(b for b, t in branches if b > park),
                                                                 text=tail_text)
        call = code.all(lambda s: s.operation.is_call)[-1]
        yield 'continuation call falls through', code.replace(next(b for b, _ in branches if b > call.position),
                                                              text='nop.b 0x0')
        yield 'return tail keeps the dispatcher b0', code.replace(
            code.site(lambda s: s.position >= tail and s.writes('b0')), text='nop.i 0x0')


@register
class Ia64SalSetVectorsBridgeContract(Subjects.CodeProperty):
    """The SAL_SET_VECTORS bridge consumes no memory stack and hands SAL's
    result back untouched: no instruction names r12 (the 1,024-byte adapter
    reserve accounting assumes the bridge uses none); its single `alloc` has
    no rotating registers and eight outputs, SAL's eight arguments; SAL is
    called through exactly one indirect `br.call`; and between that call and
    the next `br.ret` nothing writes r8-r11, the four result words."""
    SYMBOL = BRIDGE

    def verify(self, code):
        problems = ['%s (#%d) names r12' % (step.operation.text, step.position) for step in code.steps
                    if step.uses(Ia64.STACK_POINTER)]
        allocs = code.all(lambda s: s.operation.base == 'alloc')
        if len(allocs) != 1:
            problems.append('%d allocs, expected one' % len(allocs))
        else:
            sof, sol, sor = Ia64.alloc_frame(allocs[0].operation)
            if sor or sof - sol != SAL_ARGUMENTS:
                problems.append('frame %d,%d,%d does not have %d outputs and no rotating registers'
                                % (sof, sol, sor, SAL_ARGUMENTS))
        calls = code.all(lambda s: s.operation.is_call)
        if len(calls) != 1 or not calls[0].operation.is_indirect_branch:
            return problems + ['%d calls, expected one indirect call of SAL' % len(calls)]
        for step in code.window(after=calls[0]):
            if step.operation.is_return:
                return problems
            clobbered = sorted(set(RESULT_REGISTERS) & step.operation.writes)
            if clobbered:
                problems.append('%s (#%d) overwrites the SAL result in %s' % (step.operation.text, step.position,
                                                                              ', '.join(clobbered)))
        return problems + ['no br.ret follows the SAL call']

    def mutants(self, code):
        call = code.site(lambda s: s.operation.is_call)
        alloc = code.site(lambda s: s.operation.base == 'alloc')
        sof, sol, sor = Ia64.alloc_frame(alloc.operation)
        destination = alloc.text.split('=')[0]
        for description, at, text in (('reserves memory stack', alloc, 'adds r12=-16,r12'),
                                      ('reads the memory stack', alloc, 'ld8 r14=[r12]'),
                                      ('rewrites r9 after SAL returns', call, 'mov r9=r0'),
                                      ('rewrites r11 after SAL returns', call, 'mov r11=r14'),
                                      ('second SAL call', call, call.text)):
            yield description, code.insert(at.position + 1, text, stop=True)
        yield 'seven outputs', code.replace(alloc, text='%s=ar.pfs,%d,%d,%d' % (destination, sof, sol + 1, sor))
        yield 'rotating registers', code.replace(alloc, text='%s=ar.pfs,%d,%d,8' % (destination, sof, sol))
        yield 'no SAL call', code.delete(call)
        yield 'direct SAL call', code.replace(call, text='br.call.sptk.many b0=<reloc:PCREL21B SalProc>')


class _FixtureProperty(Property):
    """Facts: the decoded ABI fixture FIXTURE and the undefined symbols of its object."""
    isas = ('Ia64',)
    variants = ('O0', 'O2')
    FIXTURE = ''

    def extract(self, context, variant):
        path = context.defining_object('%s-fixtures' % variant, self.FIXTURE)
        return Subjects.code(context, path, self.FIXTURE), tuple(Properties.undefined_names(load(path)))


@register
class Ia64SalRendezvousReturnAbiFixture(_FixtureProperty):
    """The 32-byte `Ia64SalBootRendezvousAction` the C++ dispatcher returns
    comes back by value in r8-r11, which the raw entry consumes right after
    its `br.call`, not through a hidden result pointer. In the ABI fixture:
    exactly one direct call of `AsteriaIa64DispatchSalBootRendezvous`; after
    it each of r8, r9, r10 and r11 is read before it is written, and nothing
    accesses memory through r8; the fixture needs exactly the dispatcher and
    the three handler accessors, no compiler-inserted helper such as memcpy."""
    FIXTURE = RETURN_FIXTURE

    def verify(self, facts):
        code, undefined = facts
        problems = Properties.undefined_problems(undefined, RETURN_FIXTURE_DEPENDENCIES, exact=True)
        problems += Properties.call_problems(code.instructions, DISPATCHER)
        sites = Properties.branch_references(code.instructions, DISPATCHER)
        if len(sites) != 1:
            return problems
        after = [step.operation for step in code.window(after=sites[0])]
        for register_name in RESULT_REGISTERS:
            first = next((operation for operation in after if register_name in operation.reads | operation.writes),
                         None)
            if first is None or register_name not in first.reads:
                problems.append('%s is %s after the call' % (register_name, 'never read' if first is None
                                                              else 'written by %s before it is read' % first.text))
        return problems + ['%s reads the result through r8 (a hidden result pointer)' % operation.text
                           for operation in after if operation.memory is not None and operation.memory_base == 'r8']

    def mutants(self, facts):
        code, undefined = facts
        call = Properties.branch_references(code.instructions, DISPATCHER)[0]
        yield 'drops the r11 consumer', (code.delete(code.site(lambda s: s.position > call and s.reads('r11'))),
                                         undefined)
        for description, text in (('memory-return form', 'ld8 r14=[r8]'), ('overwrites r9 first', 'mov r9=r0'),
                                  ('two dispatcher calls', code.instructions[call].text)):
            yield description, (code.insert(call + 1, text, stop=True), undefined)
        yield 'memcpy dependency', (code, tuple(sorted(undefined + ('memcpy',))))
        yield 'no dispatcher dependency', (code, tuple(name for name in undefined if name != DISPATCHER))


@register
class Ia64SalSetVectorsBridgeAbiFixture(_FixtureProperty):
    """A C++ caller passes the bridge exactly its two arguments (request and
    invocation state), which the assembly frame takes as in0/in1: the
    fixture's first `alloc` has no rotating registers and exactly two
    outputs; the fixture holds exactly one PCREL21B relocation, against the
    bridge; and it needs nothing but the bridge (no memcpy for the request or
    the result)."""
    FIXTURE = BRIDGE_FIXTURE

    def verify(self, facts):
        code, undefined = facts
        problems = Properties.undefined_problems(undefined, {BRIDGE}, exact=True)
        allocs = code.all(lambda s: s.operation.base == 'alloc')
        if not allocs:
            problems.append('no alloc')
        else:
            sof, sol, sor = Ia64.alloc_frame(allocs[0].operation)
            if sor or sof - sol != 2:
                problems.append('first alloc %d,%d,%d does not pass exactly two outputs' % (sof, sol, sor))
        calls = [r for r in code.relocations if r.type == 'PCREL21B']
        if len(calls) != 1 or calls[0].target != BRIDGE:
            problems.append('PCREL21B relocations %s, expected exactly one against %s' % (calls, BRIDGE))
        return problems

    def mutants(self, facts):
        code, undefined = facts
        alloc = code.site(lambda s: s.operation.base == 'alloc')
        sof, sol, sor = Ia64.alloc_frame(alloc.operation)
        destination = alloc.text.split('=')[0]
        call = next(r for r in code.relocations if r.type == 'PCREL21B')
        yield 'three outputs', (code.replace(alloc, text='%s=ar.pfs,%d,%d,%d' % (destination, sof + 1, sol, sor)),
                                undefined)
        yield 'rotating registers', (code.replace(alloc, text='%s=ar.pfs,%d,%d,8' % (destination, sof + 8, sol + 8)),
                                     undefined)
        yield 'no alloc', (code.delete(alloc), undefined)
        yield 'no bridge call', (dataclasses.replace(code, relocations=tuple(
            r for r in code.relocations if r != call)), undefined)
        yield 'two bridge calls', (dataclasses.replace(code, relocations=code.relocations + (
            dataclasses.replace(call, offset=call.offset + 16),)), undefined)
        yield 'memcpy dependency', (code, tuple(sorted(undefined + ('memcpy',))))


@dataclasses.dataclass(frozen=True)
class BudgetNode:
    title: str
    kind: str           # Frame (static.ci frame), alias (C1/ of a C2/ body), bridge, or unresolved
    frame: int          # Static memory-stack bytes (0 unless kind == frame)
    registers: tuple    # Every (sof, sol, sor) of its final bodies


@dataclasses.dataclass(frozen=True)
class BudgetFacts:
    registers: tuple    # Defined call-graph titles matching the Register pattern
    nodes: tuple        # BudgetNode reachable from the Register title
    edges: tuple        # (source, target) among them
    reserve: int | None     # SalRegistrationAdapterReserveBytes of the header


def budget(facts: BudgetFacts) -> tuple[list[str], int, int]:
    """Problems, the longest memory-stack chain and the longest stacked-
    register chain from Register (the bridge contributes its registers and,
    as Ia64SalSetVectorsBridgeContract proves, no memory stack)."""
    if len(facts.registers) != 1:
        return ['%d call-graph definitions match %s, expected one' % (len(facts.registers), REGISTER_PATTERN)], 0, 0
    root = facts.registers[0]
    nodes = {node.title: node for node in facts.nodes}
    edges: dict[str, list] = {}
    for source, target in facts.edges:
        edges.setdefault(source, []).append(target)
    problems = ['%s calls %s, which has no static frame' % (root, node.title)
                for node in facts.nodes if node.kind == 'unresolved']
    register_frames = nodes[root].registers if root in nodes else ()
    if not register_frames:
        problems.append('Register has no alloc')
    problems += ['Register frame %s has rotating registers' % (frame,) for frame in register_frames if frame[2]]
    if not any(CallGraph.title_symbol(target) == BRIDGE for target in edges.get(root, ())):
        problems.append('Register does not call %s' % BRIDGE)
    problems += CallGraph.walk(edges, [root])[1]
    if problems:
        return problems, 0, 0
    memory = CallGraph.longest(edges, root, lambda title: nodes[title].frame, {})
    registers = CallGraph.longest(edges, root, lambda title: sum(frame[0] for frame in nodes[title].registers), {})
    return problems, memory, registers


@register
class Ia64SalRegistrationBudget(Property):
    """SAL_SET_VECTORS runs on the storage `SalBootRendezvousRegistration::
    Register` leaves: SalRegistrationFreeStorageBytes is SAL's 16 KiB minimum
    plus a 1,024-byte adapter reserve, for the memory stack and the RSE
    backing store alike. Register is one static-frame function of the call
    graph, and every function it can reach has a static frame (or is the
    assembly bridge, which uses no memory stack). The longest memory-stack
    chain from Register is at most 1,024 bytes, and the longest stacked-
    register chain N from Register (every alloc of every body, the bridge's
    15 registers included; Register's frames without rotating registers)
    satisfies 8N + 8*ceil(N/63) <= 1,024 with its RNaT words. Register calls
    the bridge directly, so the bridge frame is on that chain. The 1,024
    bytes are SalRegistrationAdapterReserveBytes of the header."""
    isas = ('Ia64',)

    def extract(self, context, variant):
        paths = [context.side_file(path, '.ci') for source, path in sorted(context.objects(variant).items())
                 if not source.endswith(('.S', '.s'))]
        program = CallGraph.Program(CallGraph.load(paths), CallGraph.decode_image(
            context.tool('objdump'), context.artifact(Subjects.image_key(variant))))
        program.resolve_aliases()
        reserve = Layout.constants(Layout.read(str(context.root / RESERVE_HEADER))).get(RESERVE_CONSTANT)
        registers = tuple(sorted(title for title in program.defined if REGISTER_PATTERN in title))
        if len(registers) != 1:
            return BudgetFacts(registers, (), (), reserve)
        reachable, _, _ = CallGraph.walk(program.edges, registers)
        nodes = []
        for title in sorted(reachable):
            if title in program.defined:
                kind, frame = 'frame', program.defined[title]
            elif title in program.aliases:
                kind, frame = 'alias', 0
            else:
                kind, frame = ('bridge' if CallGraph.title_symbol(title) == BRIDGE else 'unresolved'), 0
            nodes.append(BudgetNode(title, kind, frame, tuple(program.frames(title))))
        edges = tuple(sorted((source, target) for source in reachable for target in program.edges.get(source, ())))
        return BudgetFacts(registers, tuple(nodes), edges, reserve)

    def verify(self, facts):
        problems, memory, registers = budget(facts)
        if facts.reserve != ADAPTER_RESERVE_BYTES:
            problems.append('%s in %s is %s, but the check budgets %d bytes'
                            % (RESERVE_CONSTANT, RESERVE_HEADER, facts.reserve, ADAPTER_RESERVE_BYTES))
        if problems:
            return problems
        if memory > ADAPTER_RESERVE_BYTES:
            problems.append('Register needs %d memory-stack bytes, more than the %d-byte reserve'
                            % (memory, ADAPTER_RESERVE_BYTES))
        rse = 8 * registers + 8 * -(-registers // CallGraph.RNAT_SLOT_INTERVAL)
        if rse > ADAPTER_RESERVE_BYTES:
            problems.append('Register and the bridge hold %d stacked registers (%d RSE bytes), more than the '
                            '%d-byte reserve' % (registers, rse, ADAPTER_RESERVE_BYTES))
        return problems

    def summary(self, facts):
        _, memory, registers = budget(facts)
        return ('Register needs %d memory-stack bytes and %d stacked registers/%d RSE bytes (reserve %d)'
                % (memory, registers, CallGraph.worst_rse_bytes(registers), ADAPTER_RESERVE_BYTES))

    def mutants(self, facts):
        root = facts.registers[0]
        nodes = {node.title: node for node in facts.nodes}
        register_node = nodes[root]
        callee = next(nodes[t] for s, t in facts.edges if s == root and nodes[t].kind == 'frame')
        first = register_node.registers[0]

        def with_nodes(*changed):
            replaced = {node.title: node for node in changed}
            return dataclasses.replace(facts, nodes=tuple(replaced.get(n.title, n) for n in facts.nodes))

        yield 'Register frame 1000 and a 32-byte callee', with_nodes(
            dataclasses.replace(register_node, frame=1000), dataclasses.replace(callee, frame=32))
        yield 'no Register definition', dataclasses.replace(facts, registers=())
        yield 'two Register definitions', dataclasses.replace(facts, registers=(root, root + 'Clone'))
        yield 'Register alloc of 120 registers', with_nodes(dataclasses.replace(
            register_node, registers=((120, first[1], first[2]),) + register_node.registers[1:]))
        yield 'Register rotating registers', with_nodes(dataclasses.replace(
            register_node, registers=((max(first[0], 8), max(first[1], 8), 8),) + register_node.registers[1:]))
        yield 'Register without alloc', with_nodes(dataclasses.replace(register_node, registers=()))
        yield 'reachable callee without a static frame', dataclasses.replace(
            facts, nodes=facts.nodes + (BudgetNode('AsteriaSalMutantCallee', 'unresolved', 0, ()),),
            edges=facts.edges + ((root, 'AsteriaSalMutantCallee'),))
        yield 'Register no longer calls the bridge', dataclasses.replace(
            facts, edges=tuple(edge for edge in facts.edges
                               if not (edge[0] == root and CallGraph.title_symbol(edge[1]) == BRIDGE)))
        yield 'reachable cycle', dataclasses.replace(facts, edges=facts.edges + ((callee.title, root),))
        yield 'header declares another reserve', dataclasses.replace(facts, reserve=ADAPTER_RESERVE_BYTES // 2)
