# SPDX-License-Identifier: Apache-2.0
"""GCC call graphs (`-fcallgraph-info=su`) and IA-64 stack/RSE bounds.

GCC writes one VCG file (`.ci`) per translation unit next to its object:

    graph: { title: "/abs/Source/X.cpp"
    node: { title: "T" label: "signature\\nlocation\\nN bytes (static)" }
    edge: { sourcename: "S" targetname: "T" label: "location" }

A node is *defined* only when its label reports a static frame; external
declarations (`shape: ellipse`) and dynamic frames are not. An indirect call
is an edge to the pseudo-node `__indirect_call`. Functions with internal
linkage are titled `/abs/path/File.cpp:_ZMangled`; such a title becomes
`_ZMangled` when that name is a plain edge target or a final symbol, so
absolute build paths never matter.

`decode_image` makes one pass over the disassembly of a linked IA-64 image
and summarizes each function body (the code between one text symbol and the
next, as objdump attributes it): its `alloc` frames, indirect branches,
memory-stack (r12) references, RSE-control writes and direct transfers.

`Program` joins both views: the normalized whole-program graph, the text
symbols and the bodies. `analyze` walks it from the roots of a `Policy` and
proves the native-preemption corridor (see Ia64PreemptionCorridor): every
reachable frame is static or an allowed assembly leaf, every indirect call is
bound, the reachable subgraph is acyclic and avoids banned callees, the graph
covers every direct transfer of the linked code, and the longest
memory-stack and RSE paths stay below the bound.
"""
from __future__ import annotations

import bisect
import dataclasses
import pathlib
import re
import subprocess

from AsteriaVerify.Elf import SHF_EXECINSTR, ElfFile

INDIRECT_CALL = '__indirect_call'
# RSE: a NaT collection (RNaT) word follows every 63 stacked registers.
RNAT_SLOT_INTERVAL = 63
MAX_FRAME_REGISTERS = 96


class CallGraphError(Exception):
    """Raised when a call-graph file or a disassembly line is malformed."""


# Graph files -------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Node:
    file: str               # Title of the graph (the translation unit)
    title: str
    frame: int | None       # Static frame bytes; None for declarations and dynamic frames


@dataclasses.dataclass(frozen=True)
class Edge:
    file: str
    source: str
    target: str


@dataclasses.dataclass(frozen=True)
class Graph:
    nodes: tuple[Node,...]
    edges: tuple[Edge,...]


_GRAPH = re.compile(r'^graph: \{ title: "([^"]*)"')
_NODE = re.compile(r'^node: \{ title: "([^"]*)"')
_EDGE_SOURCE = re.compile(r'sourcename: *"([^"]*)"')
_EDGE_TARGET = re.compile(r'targetname: *"([^"]*)"')
_STATIC_FRAME = re.compile(r'^(\d+) bytes \(static\)')


def parse(text: str, name: str = '') -> Graph:
    """Parses one `.ci` file; `name` labels records before its graph line."""
    file = name
    nodes, edges = [], []
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith('graph:'):
            match = _GRAPH.match(line)
            if not match:
                raise CallGraphError('%s:%d: malformed graph line' % (name, number))
            file = match.group(1)
        elif line.startswith('node:'):
            match = _NODE.match(line)
            if not match:
                raise CallGraphError('%s:%d: malformed node line' % (name, number))
            frame = None
            if 'bytes (static)' in line:
                # The frame is the last label line: "\n<N> bytes (static)".
                static = _STATIC_FRAME.match(line.rsplit('\\n', 1)[-1])
                if not static:
                    raise CallGraphError('%s:%d: unreadable static frame' % (name, number))
                frame = int(static.group(1))
            nodes.append(Node(file, match.group(1), frame))
        elif line.startswith('edge:'):
            source, target = _EDGE_SOURCE.search(line), _EDGE_TARGET.search(line)
            if not source or not target:
                raise CallGraphError('%s:%d: malformed edge line' % (name, number))
            edges.append(Edge(file, source.group(1), target.group(1)))
        elif line.strip() not in ('', '}'):
            raise CallGraphError('%s:%d: unexpected line %r' % (name, number, line[:60]))
    return Graph(tuple(nodes), tuple(edges))


_LOADED: dict[tuple, Graph] = {}


def load(paths) -> Graph:
    """Parses and concatenates `.ci` files (cached by path, mtime and size)."""
    nodes, edges = [], []
    for path in paths:
        path = pathlib.Path(path)
        if not path.is_file():
            raise CallGraphError('missing call graph %s' % path)
        stat = path.stat()
        key = (str(path), stat.st_mtime_ns, stat.st_size)
        if key not in _LOADED:
            _LOADED[key] = parse(path.read_text(), str(path))
        nodes.extend(_LOADED[key].nodes)
        edges.extend(_LOADED[key].edges)
    return Graph(tuple(nodes), tuple(edges))


def title_symbol(title: str) -> str:
    """The linker symbol of a title: `/path/File.cpp:_ZX` names `_ZX`."""
    if '.cpp:_Z' in title:
        return title.rpartition('.cpp:')[2]
    return title


def resolve_alias(title: str) -> str | None:
    """The base-object ctor/dtor (C2) that a complete-object title
    (C1) may alias (first occurrence, as GCC names it)."""
    if 'C1' in title:
        return title.replace('C1', 'C2', 1)
    if 'D1' in title:
        return title.replace('D1', 'D2', 1)
    return None


def rse_bytes(registers: int, phase: int) -> int:
    """Backing-store bytes of `registers` stacked registers spilled from
    RNaT slot phase `phase` (0..62): an RNaT word follows every 63 slots."""
    if not 0 <= phase < RNAT_SLOT_INTERVAL:
        raise ValueError('RNaT phase %d outside 0..62' % phase)
    return 8 * registers + 8 * ((phase + registers) // RNAT_SLOT_INTERVAL)


def worst_rse_bytes(registers: int) -> int:
    """RSE bytes of `registers` for the worst starting phase (62)."""
    return max(rse_bytes(registers, phase) for phase in range(RNAT_SLOT_INTERVAL))


def malformed_frame(frame: tuple[int, int, int]) -> bool:
    """An `alloc sof,sol,sor` outside the architecture's frame rules."""
    sof, sol, sor = frame
    return sof < 0 or sof > MAX_FRAME_REGISTERS or sol > sof or sor > sol or sor % 8 != 0


# Linked-image code ---------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class TextSymbol:
    name: str
    address: int
    size: int


@dataclasses.dataclass(frozen=True)
class Transfer:
    """A direct branch or call; `target_owner` is the text-symbol address
    whose body contains the target (the closest at or below it)."""
    address: int
    call: bool
    target: int
    target_owner: int | None


@dataclasses.dataclass(frozen=True)
class Body:
    """The code between one text-symbol address and the next."""
    address: int
    allocs: tuple[tuple[int, int, int],...] = ()   # (sof, sol, sor) as objdump prints them
    indirect: tuple[tuple[int, str],...] = ()      # Calls or non-return branches through bN
    stack: tuple[tuple[int, str],...] = ()         # Instructions naming r12 in any operand
    rse: tuple[tuple[int, str],...] = ()           # Cover, loadrs, flushrs, mov ar.rsc|bspstore|rnat=
    transfers: tuple[Transfer,...] = ()


@dataclasses.dataclass(frozen=True)
class Image:
    symbols: tuple[TextSymbol,...]     # Every named symbol of an executable section
    bodies: dict                        # Address -> Body, for bodies with records


_INSTRUCTION = re.compile(r'^\s*([0-9a-f]+):\t(.*)$')
_TEMPLATE = re.compile(r'^\[[A-Z]+\]\s*')
_PREDICATE = re.compile(r'^\(p\d+\)\s*')
_ALLOC = re.compile(r'ar\.pfs,(\d+),(\d+),(\d+)$')
# A branch operand after an optional `bN=`: a branch register (indirect) or
# an address, which objdump follows with `<symbol+off>`.
_BRANCH_OPERAND = re.compile(r'^(?:b[0-7]=)?(?:(?P<register>b[0-7])|(?P<address>[0-9a-f]+)(?: <[^>]*>)?)$')
_ANNOTATION = re.compile(r'<[^>]*>')
_STACK_POINTER = re.compile(r'(?<![A-Za-z0-9_])r12(?![0-9])')
_RELEVANT = re.compile(r'alloc|br|r12|cover|loadrs|flushrs|ar\.r|ar\.bsp')
_RSE_DESTINATIONS = ('ar.rsc=', 'ar.bspstore=', 'ar.rnat=')


def owner_of(labels: list[int], address: int) -> int | None:
    """The closest label at or below `address` (objdump's attribution)."""
    index = bisect.bisect_right(labels, address) - 1
    return labels[index] if index >= 0 else None


@dataclasses.dataclass(frozen=True)
class Classified:
    kind: str                   # Alloc, direct, indirect, return or other
    frame: tuple | None = None  # Alloc (sof, sol, sor)
    call: bool = False
    target: int | None = None   # Direct target address
    stack: bool = False         # Names r12
    rse: bool = False           # Writes RSE control state


def classify(text: str) -> Classified:
    """Classifies one instruction, without template, predicate or stop."""
    mnemonic, _, operands = text.partition(' ')
    operands = operands.strip()
    stack = bool(_STACK_POINTER.search(_ANNOTATION.sub('', text)))
    rse = mnemonic in ('cover', 'loadrs', 'flushrs') or (
        mnemonic.startswith('mov') and operands.startswith(_RSE_DESTINATIONS))
    if mnemonic == 'alloc':
        match = _ALLOC.search(operands)
        frame = tuple(int(field) for field in match.groups()) if match else (-1, -1, -1)
        return Classified('alloc', frame, stack=stack, rse=rse)
    if mnemonic.startswith(('br.', 'brl.')):
        call = mnemonic.startswith(('br.call', 'brl.call'))
        if mnemonic.startswith('br.ret'):
            return Classified('return', stack=stack, rse=rse)
        match = _BRANCH_OPERAND.match(operands)
        if match is None:
            raise CallGraphError('unrecognized branch operand in %r' % text)
        if match.group('address') is not None:
            return Classified('direct', call=call, target=int(match.group('address'), 16), stack=stack, rse=rse)
        return Classified('indirect', call=call, stack=stack, rse=rse)
    return Classified('other', stack=stack, rse=rse)


def decode_text(text: str, symbols) -> Image:
    """Summarizes `objdump -d` output of a linked image per function body."""
    symbols = tuple(symbols)
    labels = sorted({symbol.address for symbol in symbols})
    records: dict[int, dict] = {}
    for line in text.splitlines():
        if ':\t' not in line or not _RELEVANT.search(line):
            continue
        match = _INSTRUCTION.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        body = _TEMPLATE.sub('', match.group(2).strip())
        if body.endswith(';;'):
            body = body[:-2].rstrip()
        body = ' '.join(_PREDICATE.sub('', body).split())
        item = classify(body)
        if item.kind in ('other', 'return') and not item.stack and not item.rse:
            continue
        owner = owner_of(labels, address)
        if owner is None:
            continue
        record = records.setdefault(owner, {'allocs': [], 'indirect': [], 'stack': [], 'rse': [], 'transfers': []})
        if item.kind == 'alloc':
            record['allocs'].append(item.frame)
        elif item.kind == 'indirect':
            record['indirect'].append((address, body))
        elif item.kind == 'direct':
            record['transfers'].append(Transfer(address, item.call, item.target, owner_of(labels, item.target)))
        if item.stack:
            record['stack'].append((address, body))
        if item.rse:
            record['rse'].append((address, body))
    bodies = {owner: Body(owner, tuple(r['allocs']), tuple(r['indirect']), tuple(r['stack']), tuple(r['rse']),
                          tuple(r['transfers']))
              for owner, r in records.items()}
    return Image(symbols, bodies)


def text_symbols(elf: ElfFile) -> tuple[TextSymbol,...]:
    """Named symbols defined in executable sections (nm types T, t and W)."""
    result = []
    for symbol in elf.defined_symbols():
        section = elf.section_of(symbol)
        if section is not None and section.flags & SHF_EXECINSTR:
            result.append(TextSymbol(symbol.name, symbol.value, symbol.size))
    return tuple(sorted(result, key=lambda s: (s.address, s.name)))


_DECODED: dict[tuple, Image] = {}


def decode_image(objdump: str, path) -> Image:
    """Decodes a linked IA-64 image once per process."""
    path = pathlib.Path(path)
    stat = path.stat()
    key = (objdump, str(path), stat.st_mtime_ns, stat.st_size)
    if key not in _DECODED:
        result = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(path)], capture_output=True, text=True)
        if result.returncode != 0:
            raise CallGraphError('%s failed for %s: %s' % (objdump, path, result.stderr.strip()))
        _DECODED[key] = decode_text(result.stdout, text_symbols(ElfFile(path)))
    return _DECODED[key]


# The joined program ----------------------------------------------------------------


class Program:
    """The normalized whole-program call graph joined with a linked image."""

    def __init__(self, graph: Graph, image: Image):
        self.image = image
        self.symbols: dict[str, set] = {}
        self.extents: dict[tuple, int | None] = {}
        for symbol in image.symbols:
            self.symbols.setdefault(symbol.name, set()).add(symbol.address)
            key = (symbol.name, symbol.address)
            if key in self.extents and self.extents[key] != symbol.size:
                self.extents[key] = None
            else:
                self.extents.setdefault(key, symbol.size)
        self.labels = sorted({symbol.address for symbol in image.symbols})
        plain_targets = {edge.target for edge in graph.edges if '.cpp:' not in edge.target}

        def normalize(title: str) -> str:
            suffix = title_symbol(title)
            if suffix != title and (suffix in plain_targets or suffix in self.symbols):
                return suffix
            return title

        self.titles: set[str] = set()
        self.defined: dict[str, int] = {}
        for node in graph.nodes:
            title = normalize(node.title)
            self.titles.add(title)
            if node.frame is not None:
                self.defined[title] = max(self.defined.get(title, 0), node.frame)
        self.edges: dict[str, set] = {}
        self.indirect: dict[str, int] = {}
        for edge in graph.edges:
            source = normalize(edge.source)
            if edge.target == INDIRECT_CALL:
                self.indirect[source] = self.indirect.get(source, 0) + 1
                continue
            self.add_edge(source, normalize(edge.target))
        self.aliases: set[str] = set()

    def add_edge(self, source: str, target: str) -> None:
        self.edges.setdefault(source, set()).add(target)
        self.titles.update((source, target))

    def is_final(self, title: str) -> bool:
        return title_symbol(title) in self.symbols

    def addresses(self, title: str) -> set:
        return self.symbols.get(title_symbol(title), set())

    def bodies(self, title: str) -> list[Body]:
        return [self.image.bodies.get(address, Body(address)) for address in sorted(self.addresses(title))]

    def frames(self, title: str) -> list[tuple[int, int, int]]:
        """Every alloc of every final body of the title's symbol."""
        return [frame for body in self.bodies(title) for frame in body.allocs]

    def resolve_aliases(self) -> set[str]:
        """Links undefined C1/ titles to the defined C2/ title whose
        final symbol names exactly the same bodies."""
        for title in sorted(self.titles):
            if title in self.defined:
                continue
            alias = resolve_alias(title)
            if alias is None or alias not in self.defined:
                continue
            left, right = self.addresses(title), self.addresses(alias)
            if left and left == right:
                self.aliases.add(title)
                self.add_edge(title, alias)
        return self.aliases

    def extent_problems(self, titles) -> list[str]:
        """Every final body of `titles` has a valid extent that no other text
        symbol splits (a split would re-attribute the rest of the body)."""
        problems = []
        for title in sorted(titles):
            symbol = title_symbol(title)
            for address in sorted(self.addresses(title)):
                size = self.extents.get((symbol, address))
                if not size or address + size > 1 << 64:
                    problems.append('%s@%#x has no valid extent' % (symbol, address))
                    continue
                first = bisect.bisect_right(self.labels, address)
                if first < len(self.labels) and self.labels[first] < address + size:
                    problems.append('a symbol at %#x splits the reachable function %s@%#x'
                                    % (self.labels[first], symbol, address))
        return problems

    def transfer_problems(self, titles) -> list[str]:
        """Every direct call, and every branch that leaves its function, in a
        body of `titles` enters a text symbol and is a call-graph edge of
        some title bound to that body."""
        bound: dict[int, list] = {}
        for title in sorted(titles):
            for address in self.addresses(title):
                bound.setdefault(address, []).append(title)
        labels = set(self.labels)
        problems = []
        for address in sorted(bound):
            body = self.image.bodies.get(address)
            if body is None:
                continue
            owners = bound[address]
            for transfer in body.transfers:
                if not transfer.call and transfer.target_owner == address:
                    continue    # Control flow inside the function
                source = '%s@%#x' % (title_symbol(owners[0]), transfer.address)
                if transfer.target not in labels:
                    problems.append('%s transfers to %#x, which is not the entry of a text symbol'
                                    % (source, transfer.target))
                    continue
                if not any(transfer.target in self.addresses(target)
                           for title in owners for target in self.edges.get(title, ())):
                    problems.append('%s reaches %#x, which the call graph lacks' % (source, transfer.target))
        return problems


def walk(edges: dict, roots, banned=()) -> tuple[set, list[str], bool]:
    """Depth-first reachability over `edges` (title -> callees) from `roots`:
    (reachable, problems, cyclic); reaching a title that contains a `banned`
    substring is a problem."""
    reachable, state, problems, cyclic = set(), {}, [], False
    for root in roots:
        if state.get(root) == 2:
            continue
        state[root] = 1
        reachable.add(root)
        stack = [(root, iter(sorted(edges.get(root, ()))))]
        while stack:
            title, successors = stack[-1]
            target = next(successors, None)
            if target is None:
                state[title] = 2
                stack.pop()
                continue
            for pattern in banned:
                if pattern in target:
                    problems.append('%s reaches the cooperative switch %s' % (title, target))
            if state.get(target) == 1:
                problems.append('call cycle through %s' % target)
                cyclic = True
            elif state.get(target) != 2:
                state[target] = 1
                reachable.add(target)
                stack.append((target, iter(sorted(edges.get(target, ())))))
    return reachable, problems, cyclic


def longest(edges: dict, root: str, weight, cache: dict) -> int:
    """Largest sum of `weight` over a path from `root` (acyclic graphs)."""
    stack = [root]
    while stack:
        current = stack[-1]
        if current in cache:
            stack.pop()
            continue
        pending = [target for target in edges.get(current, ()) if target not in cache]
        if pending:
            stack.extend(sorted(pending))
            continue
        cache[current] = weight(current) + max((cache[t] for t in edges.get(current, ())), default=0)
        stack.pop()
    return cache[root]


# The corridor analysis ---------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Root:
    title: str
    memory_bytes: int       # Memory stack the entering bridge consumes
    registers: int          # Stacked registers the entering bridge allocates
    bridge: str = ''        # The assembly that enters the root


@dataclasses.dataclass(frozen=True)
class Binding:
    """An indirect caller: its exact `__indirect_call` count and the
    functions its indirect calls reach."""
    caller: str
    count: int
    targets: tuple[str,...]


@dataclasses.dataclass(frozen=True)
class Policy:
    roots: tuple[Root,...]
    bindings: tuple[Binding,...] = ()
    leaves: tuple[tuple[str, int],...] = ()    # Assembly leaf -> exact number of RSE-control writes
    banned: tuple[str,...] = ()                # Title substrings that must not be reachable
    bound_bytes: int = 16384                    # Memory and RSE bytes stay below this


@dataclasses.dataclass
class Analysis:
    problems: list
    reachable: set = dataclasses.field(default_factory=set)
    memory_bytes: int = 0
    registers: int = 0
    rse_bytes: int = 0
    roots: dict = dataclasses.field(default_factory=dict)     # Title -> (memory bytes, registers)

    def summary(self) -> str:
        return ('at most %d memory-stack bytes and %d stacked registers/%d RSE bytes'
                % (self.memory_bytes, self.registers, self.rse_bytes))


def analyze(graph: Graph, image: Image, policy: Policy) -> Analysis:
    """Proves the corridor bound of `policy` for a graph and its image."""
    program = Program(graph, image)
    problems: list[str] = []

    # A listed indirect caller must make exactly its reviewed number of
    # indirect calls (reachable or not); they become edges to its targets.
    bindings = {binding.caller: binding for binding in policy.bindings}
    unbound, required = set(), set()
    for source, count in sorted(program.indirect.items()):
        binding = bindings.get(source)
        if binding is None:
            unbound.add(source)
        elif count != binding.count:
            problems.append('%s makes %d indirect calls; the binding table expects %d'
                            % (source, count, binding.count))
        else:
            for target in binding.targets:
                program.add_edge(source, target)
                required.add(target)
    aliases = program.resolve_aliases()

    reachable, walk_problems, cyclic = walk(program.edges, [root.title for root in policy.roots], policy.banned)
    problems += walk_problems
    for root in policy.roots:
        if root.title not in program.defined or not program.is_final(root.title):
            problems.append('corridor root %s is not a defined call-graph node and a final text symbol' % root.title)
    problems += program.extent_problems(reachable)

    leaves = dict(policy.leaves)
    registers: dict[str, int] = {}
    for title in sorted(reachable):
        symbol = title_symbol(title)
        bodies = program.bodies(title)
        if title in unbound:
            problems.append('unbound indirect call in %s' % title)
        if title not in program.defined and title not in aliases:
            if symbol not in leaves or not program.is_final(title):
                problems.append('unresolved call-graph node %s (no static frame, not an assembly leaf)' % title)
                continue
            allocs = [frame for body in bodies for frame in body.allocs]
            stack = [text for body in bodies for _, text in body.stack]
            branches = [text for body in bodies for _, text in body.indirect]
            rse = [text for body in bodies for _, text in body.rse]
            if allocs or stack or branches or len(rse) != leaves[symbol]:
                problems.append('assembly leaf %s changes corridor state: allocs %s, r12 uses %s, indirect '
                                'branches %s, %d RSE-control writes (expected %d)'
                                % (symbol, allocs, stack, branches, len(rse), leaves[symbol]))
            continue
        if title not in program.defined:
            continue    # A resolved alias; its C2/ body is charged
        final_indirect = sum(len(body.indirect) for body in bodies)
        if final_indirect != program.indirect.get(title, 0):
            problems.append('%s has %d indirect branches in the image but %d in the call graph'
                            % (symbol, final_indirect, program.indirect.get(title, 0)))
        if not program.is_final(title):
            if program.defined[title]:
                problems.append('%s has a %d-byte frame but is absent from the image' % (title, program.defined[title]))
            continue
        frames = program.frames(title)
        bad = [frame for frame in frames if malformed_frame(frame)]
        if bad:
            problems.append('malformed alloc frame %s in %s' % (bad[0], symbol))
        # Every alloc of every body counts: alternate entries, clones and
        # same-name bodies are charged without a control-flow proof.
        registers[title] = sum(frame[0] for frame in frames)

    problems += program.transfer_problems(reachable)
    for target in sorted(required):
        if not program.is_final(target):
            problems.append('bound indirect target %s is not a final text symbol' % target)

    analysis = Analysis(problems, reachable)
    if cyclic or problems:
        return analysis
    memory_cache: dict[str, int] = {}
    rse_cache: dict[str, int] = {}
    for root in policy.roots:
        memory = root.memory_bytes + longest(program.edges, root.title, lambda t: program.defined.get(t, 0),
                                             memory_cache)
        count = root.registers + longest(program.edges, root.title, lambda t: registers.get(t, 0), rse_cache)
        rse = worst_rse_bytes(count)
        analysis.roots[root.title] = (memory, count)
        analysis.memory_bytes = max(analysis.memory_bytes, memory)
        if rse > analysis.rse_bytes:
            analysis.rse_bytes, analysis.registers = rse, count
    if analysis.memory_bytes >= policy.bound_bytes or analysis.rse_bytes >= policy.bound_bytes:
        problems.append('the corridor needs %s; both must stay below %d bytes'
                        % (analysis.summary(), policy.bound_bytes))
    return analysis
