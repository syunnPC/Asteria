# SPDX-License-Identifier: Apache-2.0
"""IA-64 native-preemption stack/RSE corridor checks.

The BSP native-preemption coordinator and the idle landing run on a borrowed
per-CPU corridor: a 16 KiB memory stack and a 16 KiB RSE transition area,
while an interrupted frame is live. The property checks, using GCC's
`-fcallgraph-info=su` graphs of every kernel translation unit and the linked
image, that everything they can reach fits (see AsteriaVerify.CallGraph), and
prints the totals without pinning them.

The data below binds the indirect calls the compiler reports, lists
the assembly leaves that may appear without a frame, bans the cooperative
switches, and records the entry cost of each root, which the property
re-derives from the assembly bridge that enters it. The bound is
cross-checked against the corridor sizes in Ia64ExternalPreemptionEntry.hpp.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import CallGraph, Ia64, Layout, Mutate, Subjects
from AsteriaVerify.CallGraph import Binding, Body, Edge, Node, Policy, Root, TextSymbol, Transfer
from AsteriaVerify.Checks import Property, register

ASSEMBLY_SUFFIXES = ('.S', '.s')
BOUND_HEADER = 'Source/Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp'
BOUND_CONSTANTS = ('Ia64ExternalPreemptionCoordinatorStackSizeBytes',
                   'Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes')

_GET_TICKS = '_ZNK7Asteria8Platform4Ia644Time14Ia64LocalTimer8GetTicksEv'
_DEADLINE = '_ZN7Asteria6Kernel4Time27ProcessorDeadlineController'
_DISPATCH = '_ZN7Asteria6Kernel9Scheduler19DispatchCoordinator'
_LOCAL_TIMER = '_ZN7Asteria8Platform4Ia644Time14Ia64LocalTimer'

POLICY = Policy(
    roots=(
        # The external-preemption bridge reserves the 16-byte scratch area
        # and allocates one output register before `br.call b0=b6`.
        Root('AsteriaIa64BspNativePreemptionCoordinator', 16, 1, 'AsteriaIa64ExternalPreemptionNativeEntry'),
        # A fresh start context allocates three stacked registers and does
        # not move r12 before it calls the landing.
        Root('AsteriaIa64BspNativePreemptionIdleLanding', 0, 3, 'AsteriaIa64StartKernelContext'),
    ),
    # Every virtual call reachable from the roots lands in bounded,
    # non-switching IA-64 timer or page-table code (exact counts per caller).
    bindings=(
        # Detach's ISA policy and the common interrupts-off mailbox progress now belong to this
        # corridor.
        Binding('_ZN7Asteria6Kernel6Memory12AddressSpace21DetachProcessorLockedENS0_9Scheduler5CpuIdE', 1,
                ('_ZNK7Asteria8Platform4Ia646Memory32Ia64RuntimeAddressSpacePageTable27DetachAcknowledgesShootdownEb',)),
        Binding('_ZN7Asteria6Kernel6Memory12AddressSpace26ReserveProcessorTransitionENS0_9Scheduler5CpuIdE', 1,
                ('_ZNK7Asteria8Platform4Ia646Memory32Ia64RuntimeAddressSpacePageTable27DetachAcknowledgesShootdownEb',)),
        Binding('_ZN7Asteria6Kernel6Memory24DrainProcessorShootdownsEPv', 1,
                ('_ZN7Asteria6Kernel6Memory17TlbShootdownDrain5DrainEv',)),
        Binding('_ZN7Asteria6Kernel6Memory22TlbShootdownDispatcher12HandleRemoteERKNS1_16TlbShootdownWorkE', 1,
                ('_ZN7Asteria8Platform4Ia646Memory23Ia64LocalTlbInvalidator10InvalidateENS_6Kernel6Memory14VirtualAddressENS5_9PageCountE',)),
        # The BSP table's Clock collaborator is its Ia64LocalTimer; pending binds sample that home
        # clock once.
        Binding('_ZN7Asteria8Platform4Ia646System19Ia64UserThreadTable5AdmitERNS3_4SlotERNS_6Kernel9Scheduler'
                '6ThreadENS7_8ThreadIdERNS7_17SchedulingContextENS7_19SchedulingContextIdE', 1, (_GET_TICKS,)),
        Binding(_DEADLINE + '15ValidateCurrentERKNS1_24ProcessorDeadlineReceiptEy', 1, (_GET_TICKS,)),
        Binding(_DEADLINE + '7ProgramERKNS1_24ProcessorDeadlineSourcesEy', 1, (_GET_TICKS,)),
        Binding(_DISPATCH + '5StartERNS1_17SchedulingContextERNS1_6ThreadENS1_8ThreadIdENS1_5CpuIdE', 1,
                (_GET_TICKS,)),
        Binding(_DISPATCH + '6FinishENS1_20ThreadRunDispositionEy', 1, (_GET_TICKS,)),
        # The scheduler's selection sample, which ranks deadline candidates.
        Binding('_ZN7Asteria6Kernel9Scheduler18ProcessorScheduler18DispatchNextLockedERNS1_27ProcessorScheduler'
                'AdmissionERNS1_25ProcessorSchedulerFailureENS2_23DeferredCandidatePolicyE', 1, (_GET_TICKS,)),
        Binding('_ZN7Asteria6Kernel9Scheduler22ProcessorRunController36ValidateCooperativeAdmissionDeadlineERKNS1_'
                '21ProcessorRunAdmissionERNS1_24ProcessorRunEntryFailureE', 1, (_GET_TICKS,)),
        Binding(_DEADLINE + '33ConsumeExternallyMaskedExpirationERKNS1_24ProcessorDeadlineReceiptEy', 1,
                (_LOCAL_TIMER + '35AcknowledgeExternallyMaskedDeadlineEy',)),
        Binding(_DEADLINE + '23FinishNoDeadlineProgramEy', 2, (_LOCAL_TIMER + '14CancelDeadlineEv', _GET_TICKS)),
        Binding(_DEADLINE + '29FinishSelectedDeadlineProgramEyhy', 2,
                (_LOCAL_TIMER + '15ProgramDeadlineEy', _GET_TICKS)),
        Binding('_ZN7Asteria6Kernel6Memory12AddressSpace23ActivateProcessorLockedENS0_9Scheduler5CpuIdE', 1,
                ('_ZN7Asteria8Platform4Ia646Memory32Ia64RuntimeAddressSpacePageTable8ActivateERKNS_6Kernel6Memory'
                 '12AddressSpaceENS4_9Scheduler5CpuIdE',)),
    ),
    # Frameless assembly leaves: no alloc, no r12, no indirect branch; the
    # physical UART adapter saves and restores ar.rsc (two writes).
    leaves=(
        ('AsteriaIa64CaptureStopSnapshot', 2),
        ('AsteriaIa64FillStopPhysical', 2),
        ('AsteriaIa64DrawStopGlyphPhysical', 2),
        ('AsteriaIa64StopPark', 0),
        ('AsteriaIa64ReadIntervalTimeCounter', 0),
        ('AsteriaIa64GetStartKernelContextAddress', 0),
        ('AsteriaIa64ActivateRuntimeRegions', 0),
        # The translation-cache purge an activation runs after an unmap.
        ('AsteriaIa64PurgeTranslationCacheEntry', 0),
        ('AsteriaIa64PurgeLocalTranslation', 0),
        ('AsteriaIa64SerializeLocalTranslationPurge', 0),
        ('AsteriaIa64SerializeTranslationCachePurge', 0),
        ('AsteriaIa64CancelLocalTimer', 0),
        ('AsteriaIa64ProgramLocalTimer', 0),
        ('AsteriaIa64TryWriteDiagnosticCharacterPhysical', 2),
    ),
    # Allocator work belongs to ordinary management, never to this borrowed interruption corridor.
    banned=('DispatchFromIdle', 'Ia64KernelContext8SwitchTo', 'SwitchIa64SynchronousContinuation',
            'PhysicalPageAllocator'),
    bound_bytes=16 * 1024,
)


@dataclasses.dataclass(frozen=True)
class BridgeCost:
    root: str
    bridge: str
    indirect_calls: int     # Indirect calls in the bridge (the root is entered by the only one)
    memory_bytes: int       # R12 decrements since r12 was last set, at that call
    registers: int | None   # Sof of the last alloc before that call


@dataclasses.dataclass(frozen=True)
class CorridorFacts:
    manifest: tuple                 # (source, call-graph path, size or -1) per kernel object
    graph: CallGraph.Graph
    image: CallGraph.Image
    header_bounds: tuple            # (constant, value or None)
    bridges: tuple                  # BridgeCost per root


def header_bounds(text: str) -> tuple:
    constants = Layout.constants(text)
    return tuple((name, constants.get(name)) for name in BOUND_CONSTANTS)


def bridge_cost(context, image, root: Root) -> BridgeCost:
    """Derives what `root.bridge` spends before its indirect call."""
    operations = Subjects.code(context, image, root.bridge).operations
    calls = [index for index, operation in enumerate(operations) if operation.is_call and operation.target is None]
    memory, registers = 0, None
    for operation in operations[:calls[0]] if calls else []:
        if operation.base == 'alloc':
            registers = Ia64.alloc_frame(operation)[0]
        if Ia64.STACK_POINTER in operation.writes:
            if operation.base == 'adds' and Ia64.STACK_POINTER in operation.reads and len(operation.immediates) == 1:
                memory -= operation.immediates[0]
            else:
                memory = 0      # R12 set anew (the corridor stack base)
    return BridgeCost(root.title, root.bridge, len(calls), memory, registers)


def manifest_problems(manifest) -> list[str]:
    problems, seen = [], set()
    for source, path, size in manifest:
        if path in seen:
            problems.append('call graph %s is listed twice' % path)
        seen.add(path)
        if size < 0:
            problems.append('%s has no call graph %s' % (source, path))
        elif size == 0:
            problems.append('%s has an empty call graph %s' % (source, path))
    if not manifest:
        problems.append('no call graphs')
    return problems


def cross_check_problems(facts: CorridorFacts, policy: Policy) -> list[str]:
    problems = []
    for name, value in facts.header_bounds:
        if value != policy.bound_bytes:
            problems.append('%s in %s is %s, but the corridor bound is %d' % (name, BOUND_HEADER, value,
                                                                               policy.bound_bytes))
    costs = {cost.root: cost for cost in facts.bridges}
    for root in policy.roots:
        cost = costs.get(root.title)
        if cost is None or cost.indirect_calls != 1:
            problems.append('%s enters %s through %s indirect calls, expected one'
                            % (root.bridge, root.title, None if cost is None else cost.indirect_calls))
        elif (cost.memory_bytes, cost.registers) != (root.memory_bytes, root.registers):
            problems.append('%s enters %s with %d memory-stack bytes and %s stacked registers; the corridor data '
                            'says %d and %d' % (root.bridge, root.title, cost.memory_bytes, cost.registers,
                                                root.memory_bytes, root.registers))
    return problems


@register
class Ia64NativePreemptionCorridor(Property):
    """The coordinator and idle-landing call trees fit the borrowed corridor.

    Every compiled C++ kernel translation unit contributes its call graph
    (none missing, empty or listed twice; assembled `.S` objects have none).
    From the roots `AsteriaIa64BspNativePreemptionCoordinator` and
    `...IdleLanding`: every reachable function has a static frame, or is a
    resolved C1/ alias of its C2/ body, or is one of the frameless
    assembly leaves; every indirect call is bound by the reviewed table (a
    listed caller with another count fails even when unreachable) and every
    bound target is linked; nothing reachable cooperatively switches
    context; the reachable graph is acyclic; each reachable body has a valid
    extent that no other symbol splits, well-formed allocs (sof <= 96,
    sol <= sof, sor <= sol, sor % 8 == 0), as many indirect branches as its
    call graph reports, and every direct call or cross-function branch it
    makes enters a text symbol along a graph edge; a reachable function with
    a frame is linked. Then the longest memory-stack path plus the bridge
    reservation, and the longest stacked-register path plus the bridge frame
    in RSE bytes with RNaT words (worst phase), stay below 16 KiB, the size
    Ia64ExternalPreemptionEntry.hpp gives both corridor areas; the root
    entry costs match the bridges that enter the roots."""
    isas = ('Ia64',)
    _analyzed: tuple = (None, None)

    def subjects(self, context):
        # A production firmware does not contain the probe's roots.
        return [variant for variant in super().subjects(context)
                if variant != 'firmware' or context.configuration.get('ia64_native_preemption_probe') == '1']

    def analysis(self, facts) -> CallGraph.Analysis:
        """The corridor analysis of `facts`, shared by verify, summary and
        mutants of the same facts."""
        if self._analyzed[0] is not facts:
            self._analyzed = facts, CallGraph.analyze(facts.graph, facts.image, POLICY)
        return self._analyzed[1]

    def extract(self, context, variant):
        manifest, paths = [], []
        for source, path in sorted(context.objects(variant).items()):
            if source.endswith(ASSEMBLY_SUFFIXES):
                continue    # Assembled objects have no call graph; their callers' graphs name them
            graph = context.side_file(path, '.ci')
            size = graph.stat().st_size if graph.exists() else -1
            manifest.append((source, str(graph), size))
            if size > 0:
                paths.append(graph)
        image = context.artifact(Subjects.image_key(variant))
        return CorridorFacts(tuple(manifest), CallGraph.load(paths),
                             CallGraph.decode_image(context.tool('objdump'), image),
                             header_bounds(Layout.read(context.root / BOUND_HEADER)),
                             tuple(bridge_cost(context, image, root) for root in POLICY.roots))

    def verify(self, facts):
        return (manifest_problems(facts.manifest) + cross_check_problems(facts, POLICY)
                + self.analysis(facts).problems)

    def summary(self, facts):
        return '%s (bound %d)' % (self.analysis(facts).summary(), POLICY.bound_bytes)

    def mutants(self, facts):
        # Mutate the real graph and image to check corridor bounds and exclusions.
        analysis, policy, symbols = self.analysis(facts), POLICY, facts.image.symbols
        program = CallGraph.Program(facts.graph, facts.image)
        reachable, labels = analysis.reachable, program.labels
        root, idle = policy.roots
        root_address = min(program.addresses(root.title))
        root_size = program.extents[(root.title, root_address)]
        root_body = _body(facts, root_address)
        beyond = max(s.address + s.size for s in symbols) + 0x1000
        mutant = 'AsteriaCorridorMutant'

        def with_body(address, body):
            return _image(facts, bodies={address: body})

        def root_with(**changes):
            return with_body(root_address, dataclasses.replace(root_body, **changes))

        # A call (or tail branch) of the root that the graph records, and its callee.
        call = next(t for t in root_body.transfers if (t.call or t.target_owner != root_address) and t.target in labels)
        callee = sorted(t for t in program.edges.get(root.title, ()) if call.target in program.addresses(t))[0]

        def drops_call(edge):
            return edge.source == root.title and call.target in program.addresses(CallGraph.title_symbol(edge.target))

        # Interior labels (objdump headers inside a reachable body) and extents of reachable bodies.
        interior = root_address + 16
        yield 'interior label with an unowned alloc', _image(facts, symbols + (
            TextSymbol(mutant + 'Interior', interior, 0),), {interior: Body(interior, allocs=((96, 0, 0),))})
        yield 'out-of-order interior label', _image(
            facts, (TextSymbol(mutant + 'Interior', root_address + root_size - 16, 0),) + symbols)
        yield 'reachable function without a size', _image(facts, tuple(dataclasses.replace(s, size=0) if (
            s.name, s.address) == (root.title, root_address) else s for s in symbols))
        yield 'conflicting records of one symbol', _image(
            facts, symbols + (TextSymbol(root.title, root_address, root_size + 16),))

        # Bounds.
        yield 'oversized idle-landing frame', _graph(facts, nodes=(Node(mutant, idle.title, policy.bound_bytes),))
        yield 'memory-stack path at 16 KiB', _graph(facts, nodes=(Node(
            mutant, root.title, program.defined[root.title] + policy.bound_bytes - analysis.roots[root.title][0]),))
        yield 'larger duplicate callee frame is charged', _graph(facts, nodes=(
            Node(mutant, callee, policy.bound_bytes),))
        needed = next(n for n in range(1, policy.bound_bytes) if CallGraph.worst_rse_bytes(n) >= policy.bound_bytes)
        assert 8 * needed < policy.bound_bytes      # Only the RNaT words cross the bound
        heaviest = max(policy.roots, key=lambda r: analysis.roots[r.title][1])
        extra = needed - analysis.roots[heaviest.title][1]
        frames = tuple((min(96, extra - done), min(96, extra - done), 0) for done in range(0, extra, 96))
        heavy_address = min(program.addresses(heaviest.title))
        heavy_body = _body(facts, heavy_address)
        yield 'RSE path crossing 16 KiB only through RNaT words', with_body(
            heavy_address, dataclasses.replace(heavy_body, allocs=heavy_body.allocs + frames))
        yield 'every same-name body is charged', _image(
            facts, symbols + (TextSymbol(heaviest.title, beyond, 0x100),), {beyond: Body(beyond, allocs=frames)})

        # Direct transfers the graph must cover.
        yield 'direct call without a final text target', _graph(
            root_with(transfers=root_body.transfers + (
                Transfer(root_address, True, beyond, CallGraph.owner_of(labels, beyond)),)),
            nodes=(Node(mutant, mutant + 'MissingFinal', 0),),
            edges=(Edge(mutant, root.title, mutant + 'MissingFinal'),))
        yield 'direct call to a non-text address', root_with(
            transfers=root_body.transfers + (Transfer(root_address, True, labels[0] - 0x100, None),))
        secondary = next(call.target + offset for offset in range(16, 0x1000, 16) if call.target + offset not in labels)
        yield 'call to a secondary function entry', root_with(transfers=tuple(
            dataclasses.replace(t, target=secondary) if t == call else t for t in root_body.transfers))
        yield 'direct call omitted from the call graph', _graph(facts, drop=drops_call)
        yield 'source-alias call omitted from the call graph', _image(
            _graph(facts, drop=drops_call), symbols + (TextSymbol(mutant + 'RootAlias', root_address, root_size),))
        yield 'self-recursive call omitted from the call graph', root_with(
            transfers=root_body.transfers + (Transfer(root_address, True, root_address, root_address),))

        # Aliases.
        for marker in ('C1', 'D1'):
            alias = sorted(title for title in program.resolve_aliases() & reachable if marker in title)
            if not alias:
                continue
            symbol = CallGraph.title_symbol(alias[0])
            yield 'non-coincident %s alias' % marker, _image(facts, tuple(
                dataclasses.replace(s, address=beyond) if s.name == symbol else s for s in symbols))
            yield '%s alias with an extra final body' % marker, _image(
                facts, symbols + (TextSymbol(symbol, beyond, 0x100),), {beyond: Body(beyond, allocs=((96, 0, 0),))})

        # Call-graph files and roots.
        source, path, _ = facts.manifest[0]
        yield 'duplicate call-graph file', dataclasses.replace(facts, manifest=facts.manifest + facts.manifest[:1])
        yield 'missing call-graph file', dataclasses.replace(facts, manifest=((source, path, -1),) + facts.manifest[1:])
        yield 'empty call-graph file', dataclasses.replace(facts, manifest=((source, path, 0),) + facts.manifest[1:])
        yield 'root is not a final text symbol', _image(facts, tuple(s for s in symbols if s.name != root.title))
        yield 'root has no call-graph frame', dataclasses.replace(facts, graph=CallGraph.Graph(
            tuple(node for node in facts.graph.nodes if node.title != root.title), facts.graph.edges))

        # Graph shape and indirect calls.
        root_indirect = root_body.indirect + ((root_address, 'br.call.sptk.many b0=b6'),)
        yield 'indirect call omitted from the call graph', root_with(indirect=root_indirect)
        yield 'reachable call cycle', _graph(facts, edges=(Edge(mutant, callee, root.title),))
        yield 'unbound indirect call', _graph(root_with(indirect=root_indirect),
                                              edges=(Edge(mutant, root.title, CallGraph.INDIRECT_CALL),))
        yield 'missing reachable node', _graph(facts, edges=(Edge(mutant, root.title, mutant + 'MissingNode'),))
        switch = sorted(title for title in program.titles if any(p in title for p in policy.banned))
        yield 'cooperative switch reachable', _graph(facts, edges=(
            Edge(mutant, callee, switch[0] if switch else mutant + 'Ia64KernelContext8SwitchTo'),))
        yield 'function with a frame absent from the image', _graph(
            facts, nodes=(Node(mutant, mutant + 'Absent', 32),), edges=(Edge(mutant, root.title, mutant + 'Absent'),))
        binding = next(b for b in policy.bindings if b.caller in program.indirect)
        caller_address = min(program.addresses(binding.caller))
        caller_body = _body(facts, caller_address)
        yield 'bound caller with another indirect-call count', _graph(
            with_body(caller_address, dataclasses.replace(caller_body, indirect=caller_body.indirect + (
                (caller_address, 'br.call.sptk.many b0=b7'),))),
            edges=(Edge(mutant, binding.caller, CallGraph.INDIRECT_CALL),))
        yield 'missing bound indirect target', _image(facts, tuple(s for s in symbols if s.name != binding.targets[0]))

        # Alloc frames.
        first_alloc = next(address for address in sorted(program.addresses(root.title)) if _body(facts, address).allocs)
        alloc_body = _body(facts, first_alloc)
        for description, frame in (('alloc with more than 96 registers', (97, 97, 0)),
                                   ('rotating registers not a multiple of 8', (16, 16, 4))):
            yield description, with_body(first_alloc, dataclasses.replace(
                alloc_body, allocs=(frame,) + alloc_body.allocs[1:]))

        # Assembly leaves.
        leaf = next(symbol for symbol, writes in policy.leaves if writes == 0 and symbol in reachable)
        leaf_address = min(program.addresses(leaf))
        leaf_body = _body(facts, leaf_address)
        for description, text in (('assembly leaf with an alloc frame', 'alloc r34=ar.pfs,1,0,0'),
                                  ('assembly leaf that adjusts the memory stack', 'adds r12=-16,r12'),
                                  ('assembly leaf that post-increments the memory stack', 'st8 [r12]=r14,8'),
                                  ('assembly leaf that covers the RSE frame', 'cover'),
                                  ('assembly leaf with an indirect call', 'br.call.sptk.many b0=b6')):
            yield description, with_body(leaf_address, _with_instruction(leaf_body, leaf_address, text))
        uart = next((symbol for symbol, writes in policy.leaves if writes and symbol in reachable), None)
        if uart is not None:
            uart_address = min(program.addresses(uart))
            uart_body = _body(facts, uart_address)
            yield 'UART leaf with one ar.rsc write', with_body(
                uart_address, dataclasses.replace(uart_body, rse=uart_body.rse[1:]))
            yield 'UART leaf with three ar.rsc writes', with_body(
                uart_address, _with_instruction(uart_body, uart_address, 'mov.m ar.rsc=r0'))

        # Cross-checks of the data.
        name, value = facts.header_bounds[0]
        yield 'header declares another corridor size', dataclasses.replace(
            facts, header_bounds=((name, value // 2),) + facts.header_bounds[1:])
        cost = facts.bridges[0]
        for description, changes in (('bridge reserves more memory stack', dict(memory_bytes=cost.memory_bytes + 16)),
                                     ('bridge allocates another frame', dict(registers=cost.registers + 1)),
                                     ('bridge enters the root twice', dict(indirect_calls=2))):
            yield description, dataclasses.replace(facts, bridges=Mutate.replace_at(facts.bridges, 0, **changes))


def _graph(facts, nodes=(), edges=(), drop=lambda edge: False):
    graph = facts.graph
    return dataclasses.replace(facts, graph=CallGraph.Graph(
        graph.nodes + tuple(nodes), tuple(edge for edge in graph.edges if not drop(edge)) + tuple(edges)))


def _image(facts, symbols=None, bodies=None):
    image = facts.image
    return dataclasses.replace(facts, image=CallGraph.Image(image.symbols if symbols is None else symbols,
                                                            {**image.bodies, **(bodies or {})}))


def _body(facts, address):
    return facts.image.bodies.get(address, Body(address))


def _with_instruction(body: Body, address: int, text: str) -> Body:
    """The body with one more instruction, recorded as decode_text would."""
    item = CallGraph.classify(text)
    changes = {}
    if item.kind == 'alloc':
        changes['allocs'] = body.allocs + (item.frame,)
    if item.kind == 'indirect':
        changes['indirect'] = body.indirect + ((address, text),)
    if item.stack:
        changes['stack'] = body.stack + ((address, text),)
    if item.rse:
        changes['rse'] = body.rse + ((address, text),)
    return dataclasses.replace(body, **changes)
