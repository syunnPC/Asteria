# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the call-graph parser and the corridor walker.

Corridor fixtures supply nm rows, objdump text and `.ci` graphs. Every
fixture symbol is 0x100 bytes; cases check verdicts and accepted path totals.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import pathlib
import re
import tempfile
import unittest

from AsteriaVerify import CallGraph
from AsteriaVerify.CallGraph import Binding, Policy, Root

ROOT = 'AsteriaIa64BspNativePreemptionCoordinator'
IDLE = 'AsteriaIa64BspNativePreemptionIdleLanding'

POLICY = Policy(
    roots=(Root(ROOT, 16, 1), Root(IDLE, 0, 3)),
    leaves=(('AsteriaIa64ReadIntervalTimeCounter', 0), ('AsteriaIa64TryWriteDiagnosticCharacterPhysical', 2)),
    banned=('DispatchFromIdle', 'Ia64KernelContext8SwitchTo', 'SwitchIa64SynchronousContinuation'),
    bound_bytes=16384)

BASE_SYMBOLS = """\
0000000000001000 T AsteriaIa64BspNativePreemptionCoordinator
0000000000001100 T AsteriaCorridorLeaf
0000000000001200 T AsteriaCorridorAliasC1
0000000000001200 T AsteriaCorridorAliasC2
0000000000009000 T AsteriaIa64BspNativePreemptionIdleLanding
"""
BASE_DISASSEMBLY = """\
0000000000001000 <AsteriaIa64BspNativePreemptionCoordinator>:
 1000: [MII] alloc r34=ar.pfs,6,5,0
 1010: [MIB] br.call.sptk.many b0=1100 <AsteriaCorridorLeaf>
 1020: [MIB] br.call.sptk.many b0=1200 <AsteriaCorridorAliasC1>
0000000000001100 <AsteriaCorridorLeaf>:
 1100: [MII] alloc r34=ar.pfs,4,3,0
0000000000001200 <AsteriaCorridorAliasC1>:
 1200: [MII] alloc r127=ar.pfs,90,0,0
 1210: [MII] alloc r34=ar.pfs,2,1,0
0000000000009000 <AsteriaIa64BspNativePreemptionIdleLanding>:
 9000: [MII] alloc r34=ar.pfs,0,0,0
"""
BASE_GRAPH = r"""graph: { title: "CorridorFixture.cpp"
node: { title: "AsteriaIa64BspNativePreemptionCoordinator" label: "root\n64 bytes (static)" }
node: { title: "AsteriaCorridorLeaf" label: "leaf\n32 bytes (static)" }
node: { title: "AsteriaCorridorAliasC1" label: "alias" shape : triangle }
node: { title: "AsteriaCorridorAliasC2" label: "body\n32 bytes (static)" }
node: { title: "AsteriaIa64BspNativePreemptionIdleLanding" label: "idle\n0 bytes (static)" }
edge: { sourcename: "AsteriaIa64BspNativePreemptionCoordinator" targetname: "AsteriaCorridorLeaf" }
edge: { sourcename: "AsteriaIa64BspNativePreemptionCoordinator" targetname: "AsteriaCorridorAliasC1" }
edge: { sourcename: "AsteriaCorridorAliasC1" targetname: "AsteriaCorridorAliasC2" }
"""

LEAF_SYMBOLS = """\
0000000000001000 T AsteriaIa64BspNativePreemptionCoordinator
0000000000001100 T AsteriaIa64ReadIntervalTimeCounter
0000000000009000 T AsteriaIa64BspNativePreemptionIdleLanding
"""
LEAF_DISASSEMBLY = """\
0000000000001000 <AsteriaIa64BspNativePreemptionCoordinator>:
 1000: [MII] alloc r34=ar.pfs,0,0,0
 1010: [MIB] br.call.sptk.many b0=1100 <AsteriaIa64ReadIntervalTimeCounter>
0000000000001100 <AsteriaIa64ReadIntervalTimeCounter>:
 1100: [MII] nop.i 0x0
0000000000009000 <AsteriaIa64BspNativePreemptionIdleLanding>:
 9000: [MII] alloc r34=ar.pfs,0,0,0
"""
LEAF_GRAPH = r"""graph: { title: "AssemblyLeafFixture.cpp"
node: { title: "AsteriaIa64BspNativePreemptionCoordinator" label: "root\n0 bytes (static)" }
node: { title: "AsteriaIa64BspNativePreemptionIdleLanding" label: "idle\n0 bytes (static)" }
edge: { sourcename: "AsteriaIa64BspNativePreemptionCoordinator" targetname: "AsteriaIa64ReadIntervalTimeCounter" }
"""


class Fixture:
    """nm rows, objdump text and graph files, edited like the shell fixtures."""

    def __init__(self, symbols: str, disassembly: str, graph: str):
        self.symbols = symbols
        self.disassembly = disassembly
        self.graphs = [graph]

    def image(self) -> CallGraph.Image:
        symbols, listed = [], set()
        for line in self.symbols.splitlines():
            address, kind, name = line.split()
            listed.add(name)
            if kind in 'TtWw':      # the mock nm gave every symbol 0x100 bytes
                symbols.append(CallGraph.TextSymbol(name, int(address, 16), 0x100))
        for match in re.finditer(r'^([0-9a-f]+) <([^>]+)>:$', self.disassembly, re.M):
            if match.group(2) not in listed:    # a header label nm did not list
                symbols.append(CallGraph.TextSymbol(match.group(2), int(match.group(1), 16), 0))
        text = re.sub(r'^(\s*[0-9a-f]+): ', r'\1:\t', self.disassembly, flags=re.M)
        return CallGraph.decode_text(text, symbols)

    def graph(self) -> CallGraph.Graph:
        nodes, edges = [], []
        for index, text in enumerate(self.graphs):
            parsed = CallGraph.parse(text, 'fixture%d.ci' % index)
            nodes.extend(parsed.nodes)
            edges.extend(parsed.edges)
        return CallGraph.Graph(tuple(nodes), tuple(edges))

    def analyze(self, policy: Policy = POLICY) -> CallGraph.Analysis:
        return CallGraph.analyze(self.graph(), self.image(), policy)

    def insert_after(self, pattern: str, line: str) -> None:
        lines = self.disassembly.splitlines()
        index = next(i for i, text in enumerate(lines) if re.search(pattern, text))
        lines.insert(index + 1, line)
        self.disassembly = '\n'.join(lines) + '\n'

    def append_graph(self, text: str) -> None:
        self.graphs[0] += text

    def delete_graph_lines(self, pattern: str) -> None:
        self.graphs[0] = ''.join(line + '\n' for line in self.graphs[0].splitlines() if not re.search(pattern, line))


def base() -> Fixture:
    return Fixture(BASE_SYMBOLS, BASE_DISASSEMBLY, BASE_GRAPH)


def leaf() -> Fixture:
    return Fixture(LEAF_SYMBOLS, LEAF_DISASSEMBLY, LEAF_GRAPH)


class ParserTests(unittest.TestCase):
    def test_gcc_lines(self):
        graph = CallGraph.parse(
            'graph: { title: "/b/Source/X.cpp"\n'
            'node: { title: "/b/Source/X.cpp:_ZL1fv" label: "void f()\\n/b/Source/X.cpp:3:6\\n48 bytes (static)" }\n'
            'node: { title: "memcpy" label: "memcpy\\n<built-in>" shape : ellipse }\n'
            'node: { title: "g" label: "g\\nX.cpp:9:1\\n16 bytes (dynamic,bounded)" }\n'
            'edge: { sourcename: "/b/Source/X.cpp:_ZL1fv" targetname: "memcpy" label: "/b/Source/X.cpp:4:3" }\n'
            'edge: { sourcename: "g" targetname: "__indirect_call" label: "X.cpp:9:9" }\n'
            '}\n', 'X.ci')
        self.assertEqual([(n.file, n.title, n.frame) for n in graph.nodes],
                         [('/b/Source/X.cpp', '/b/Source/X.cpp:_ZL1fv', 48), ('/b/Source/X.cpp', 'memcpy', None),
                          ('/b/Source/X.cpp', 'g', None)])
        self.assertEqual([(e.source, e.target) for e in graph.edges],
                         [('/b/Source/X.cpp:_ZL1fv', 'memcpy'), ('g', '__indirect_call')])

    def test_malformed_lines(self):
        for text in ('node: title "x"', 'edge: { sourcename: "a" }', 'bogus',
                     'node: { title: "a" label: "x bytes (static)" }'):
            with self.assertRaises(CallGraph.CallGraphError, msg=text):
                CallGraph.parse(text, 'bad.ci')

    def test_local_titles_normalize_to_their_symbol(self):
        graph = CallGraph.parse(
            'graph: { title: "/b/A.cpp"\n'
            'node: { title: "/b/A.cpp:_ZL4Leafv" label: "l\\n8 bytes (static)" }\n'
            'node: { title: "/b/A.cpp:_ZL6Hiddenv" label: "h\\n8 bytes (static)" }\n'
            'node: { title: "Root" label: "r\\n8 bytes (static)" }\n'
            'edge: { sourcename: "Root" targetname: "/b/A.cpp:_ZL4Leafv" }\n'
            'edge: { sourcename: "Root" targetname: "/b/A.cpp:_ZL6Hiddenv" }\n')
        image = CallGraph.Image((CallGraph.TextSymbol('_ZL4Leafv', 0x100, 0x10),), {})
        program = CallGraph.Program(graph, image)
        # A final symbol names the local title; an unlinked one keeps its path.
        self.assertEqual(program.edges['Root'], {'_ZL4Leafv', '/b/A.cpp:_ZL6Hiddenv'})
        self.assertEqual(CallGraph.title_symbol('/b/A.cpp:_ZL6Hiddenv'), '_ZL6Hiddenv')

    def test_load_reads_only_the_listed_graphs(self):
        with tempfile.TemporaryDirectory() as directory:
            listed = pathlib.Path(directory) / 'Listed.ci'
            stale = pathlib.Path(directory) / 'UnlistedStale.ci'
            listed.write_text(BASE_GRAPH)
            stale.write_text('edge: { sourcename: "%s" targetname: "%s" }\n' % (ROOT, ROOT))
            graph = CallGraph.load([listed])
            with self.assertRaises(CallGraph.CallGraphError):
                CallGraph.load([pathlib.Path(directory) / 'Missing.ci'])
        # The unlisted stale self-cycle does not exist for the analysis.
        self.assertEqual(CallGraph.analyze(graph, base().image(), POLICY).problems, [])


class ClassifierTests(unittest.TestCase):
    def test_branches(self):
        self.assertEqual(CallGraph.classify('br.call.sptk.many b0=4000000000012340 <F+0x10>'),
                         CallGraph.Classified('direct', call=True, target=0x4000000000012340))
        self.assertEqual(CallGraph.classify('brl.call.sptk.many b0=1100 <Leaf>').target, 0x1100)
        self.assertEqual(CallGraph.classify('br.few 1040 <F+0x40>'), CallGraph.Classified('direct', target=0x1040))
        self.assertEqual(CallGraph.classify('br.cond.dpnt.few 1040 <F+0x40>').kind, 'direct')
        self.assertEqual(CallGraph.classify('br.call.sptk.many b0=b6'), CallGraph.Classified('indirect', call=True))
        self.assertEqual(CallGraph.classify('br.sptk.many b6').kind, 'indirect')
        self.assertEqual(CallGraph.classify('br.ret.sptk.many b0').kind, 'return')
        self.assertEqual(CallGraph.classify('break.i 0x100000').kind, 'other')
        self.assertEqual(CallGraph.classify('brp.sptk 1040,1080').kind, 'other')

    def test_frames_stack_and_rse(self):
        self.assertEqual(CallGraph.classify('alloc r34=ar.pfs,6,5,0').frame, (6, 5, 0))
        for text in ('adds r12=-16,r12', 'st8 [r12]=r14,8', 'mov r12=r14', 'ld8 r14=[r12]'):
            self.assertTrue(CallGraph.classify(text).stack, text)
        for text in ('mov r120=r1', 'adds r14=16,r125', 'br.call.sptk.many b0=10 <r12Name>'):
            self.assertFalse(CallGraph.classify(text).stack, text)
        for text in ('cover', 'flushrs', 'loadrs', 'mov.m ar.rsc=r0', 'mov.m ar.bspstore=r15', 'mov.m ar.rnat=r0'):
            self.assertTrue(CallGraph.classify(text).rse, text)
        for text in ('mov.m r15=ar.rsc', 'mov.m r16=ar.bsp', 'mov.i ar.pfs=r0'):
            self.assertFalse(CallGraph.classify(text).rse, text)

    def test_rse_bytes_with_rnat_words(self):
        def reference(registers, phase):
            total, slot = 0, phase
            for _ in range(registers):
                total, slot = total + 8, slot + 1
                if slot == 63:
                    total, slot = total + 8, 0
            return total
        for registers in range(0, 300, 7):
            for phase in range(63):
                self.assertEqual(CallGraph.rse_bytes(registers, phase), reference(registers, phase))
        self.assertEqual(CallGraph.worst_rse_bytes(99), 808)
        self.assertEqual(CallGraph.worst_rse_bytes(2016), 16384)
        self.assertLess(8 * 2016, 16384)


class CorridorAcceptanceTests(unittest.TestCase):
    """The accepted fixtures of the shell self-test, with their totals."""

    def test_base_charges_alias_and_alternate_allocs(self):
        analysis = base().analyze()
        self.assertEqual(analysis.problems, [])
        self.assertEqual((analysis.memory_bytes, analysis.registers, analysis.rse_bytes), (112, 99, 808))

    def test_largest_duplicate_frame_is_charged(self):
        fixture = base()
        fixture.graphs += [r'graph: { title: "DuplicateLarge.cpp"' '\n'
                           r'node: { title: "%s" label: "large\n128 bytes (static)" }' % ROOT + '\n',
                           r'graph: { title: "DuplicateSmall.cpp"' '\n'
                           r'node: { title: "%s" label: "small\n16 bytes (static)" }' % ROOT + '\n']
        analysis = fixture.analyze()
        self.assertEqual((analysis.problems, analysis.memory_bytes), ([], 176))

    def test_direct_call_to_the_c2_body(self):
        fixture = base()
        fixture.disassembly = fixture.disassembly.replace('<AsteriaCorridorAliasC1>', '<AsteriaCorridorAliasC2>')
        self.assertEqual(fixture.analyze().problems, [])

    def test_brl_call(self):
        fixture = base()
        fixture.disassembly = fixture.disassembly.replace('br.call.sptk.many b0=1100', 'brl.call.sptk.many b0=1100')
        self.assertEqual(fixture.analyze().problems, [])

    def test_every_duplicate_name_body_is_charged(self):
        fixture = base()
        fixture.symbols += '0000000000001300 T AsteriaCorridorLeaf\n'
        fixture.disassembly += '0000000000001300 <AsteriaCorridorLeaf>:\n 1300: [MII] alloc r127=ar.pfs,96,0,0\n'
        analysis = fixture.analyze()
        self.assertEqual((analysis.problems, analysis.registers, analysis.rse_bytes), ([], 107, 872))

    def test_assembly_leaf(self):
        self.assertEqual(leaf().analyze().problems, [])


class CorridorRejectionTests(unittest.TestCase):
    """Every rejected fixture of the shell self-test, plus the cases it lacked."""

    def rejects(self, fixture: Fixture, expected: str, policy: Policy = POLICY):
        problems = fixture.analyze(policy).problems
        self.assertTrue(problems, 'accepted: ' + expected)
        self.assertTrue(any(expected in problem for problem in problems), problems)

    # Interior labels.
    def test_interior_label_with_an_unowned_alloc(self):
        fixture = base()
        fixture.disassembly = fixture.disassembly.replace(
            ' 1010: [MIB] br.call.sptk.many b0=1100 <AsteriaCorridorLeaf>',
            '0000000000001010 <AsteriaCorridorInterior>:\n 1010: [MII] alloc r127=ar.pfs,96,0,0')
        self.rejects(fixture, 'splits the reachable function')

    def test_out_of_order_interior_label(self):
        fixture = base()
        fixture.disassembly += '0000000000001010 <AsteriaCorridorInterior>:\n'
        self.rejects(fixture, 'splits the reachable function')

    # Bounds.
    def test_oversized_idle_landing_phase(self):
        fixture = base()
        fixture.append_graph(r'node: { title: "%s" label: "idle-large\n16384 bytes (static)" }' % IDLE + '\n')
        self.rejects(fixture, 'both must stay below 16384')

    def test_memory_path_at_the_bound(self):
        fixture = base()
        fixture.graphs[0] = fixture.graphs[0].replace('64 bytes', '16336 bytes')
        self.rejects(fixture, '16384 memory-stack bytes')

    def test_rse_path_crossing_the_bound_through_rnat_words(self):
        # 21 chained 96-register frames. The shell fixture appended each
        # chain call after the previous function, so the first one was
        # attributed to the idle landing and the case failed as an unknown
        # edge before the bound was computed; here every call sits in its
        # caller and only the RNaT words carry the path over 16 KiB.
        symbols = ['0000000000001000 T %s' % ROOT, '0000000000009000 T %s' % IDLE]
        disassembly = ['0000000000001000 <%s>:' % ROOT, ' 1000: [MII] alloc r34=ar.pfs,0,0,0',
                       ' 1010: [MIB] br.call.sptk.many b0=1100 <AsteriaCorridorRnat1>']
        graph = ['graph: { title: "RnatFixture.cpp"',
                 r'node: { title: "%s" label: "root\n0 bytes (static)" }' % ROOT,
                 r'node: { title: "%s" label: "idle\n0 bytes (static)" }' % IDLE]
        previous = ROOT
        for index in range(1, 22):
            address = 0x1000 + index * 0x100
            name = 'AsteriaCorridorRnat%d' % index
            symbols.append('%016x T %s' % (address, name))
            disassembly += ['%016x <%s>:' % (address, name), ' %x: [MII] alloc r127=ar.pfs,96,0,0' % address]
            if index < 21:
                disassembly.append(' %x: [MIB] br.call.sptk.many b0=%x <AsteriaCorridorRnat%d>'
                                   % (address + 0x10, address + 0x100, index + 1))
            graph += [r'node: { title: "%s" label: "node\n0 bytes (static)" }' % name,
                      'edge: { sourcename: "%s" targetname: "%s" }' % (previous, name)]
            previous = name
        disassembly += ['0000000000009000 <%s>:' % IDLE, ' 9000: [MII] alloc r34=ar.pfs,0,0,0']
        fixture = Fixture('\n'.join(symbols) + '\n', '\n'.join(disassembly) + '\n', '\n'.join(graph) + '\n')
        self.assertLess(8 * (1 + 21 * 96), 16384)
        problems = fixture.analyze().problems
        self.assertEqual(len(problems), 1, problems)
        self.assertIn('2017 stacked registers/16400 RSE bytes', problems[0])

    # Direct transfers.
    def test_direct_call_without_a_final_text_target(self):
        fixture = base()
        fixture.insert_after(r'^ 1000: .*alloc ', ' 1006: [MIB] br.call.sptk.many b0=1400 <AsteriaMissingFinal>')
        fixture.append_graph(r'node: { title: "AsteriaMissingFinal" label: "missing\n0 bytes (static)" }' '\n'
                             'edge: { sourcename: "%s" targetname: "AsteriaMissingFinal" }\n' % ROOT)
        self.rejects(fixture, 'not the entry of a text symbol')

    def test_direct_call_to_non_text_data(self):
        fixture = base()
        fixture.symbols += '0000000000001400 D AsteriaCorridorLeaf\n'
        fixture.disassembly = fixture.disassembly.replace('b0=1100 <AsteriaCorridorLeaf>',
                                                          'b0=1400 <AsteriaCorridorLeaf>')
        self.rejects(fixture, 'not the entry of a text symbol')

    def test_call_to_a_secondary_function_entry(self):
        # The shell fixture rewrote only objdump's annotation; the port
        # decodes numeric targets, so the address moves with it.
        fixture = base()
        fixture.disassembly = fixture.disassembly.replace(
            'br.call.sptk.many b0=1100 <AsteriaCorridorLeaf>', 'brl.call.sptk.many b0=1110 <AsteriaCorridorLeaf+0x10>')
        self.rejects(fixture, 'not the entry of a text symbol')

    def test_brl_call_omitted_from_the_call_graph(self):
        fixture = base()
        fixture.disassembly = fixture.disassembly.replace('br.call.sptk.many b0=1100', 'brl.call.sptk.many b0=1100')
        fixture.delete_graph_lines('targetname: "AsteriaCorridorLeaf"')
        self.rejects(fixture, 'which the call graph lacks')

    def test_direct_call_omitted_from_the_call_graph(self):
        fixture = base()
        fixture.delete_graph_lines('targetname: "AsteriaCorridorLeaf"')
        self.rejects(fixture, 'which the call graph lacks')

    def test_source_alias_call_omitted_from_the_call_graph(self):
        fixture = base()
        fixture.symbols += '0000000000001000 T AsteriaCorridorRootAlias\n'
        fixture.disassembly = fixture.disassembly.replace('<%s>:' % ROOT, '<AsteriaCorridorRootAlias>:')
        fixture.delete_graph_lines('targetname: "AsteriaCorridorLeaf"')
        self.rejects(fixture, 'which the call graph lacks')

    def test_self_recursive_call_omitted_from_the_call_graph(self):
        fixture = base()
        fixture.insert_after(r'^ 1000: .*alloc ', ' 1006: [MIB] br.call.sptk.many b0=1000 <%s>' % ROOT)
        self.assertEqual(len(re.findall(r'br\.call.*<%s>' % ROOT, fixture.disassembly)), 1)
        self.rejects(fixture, 'which the call graph lacks')

    # Aliases.
    def moved_alias(self, marker: str) -> Fixture:
        fixture = base()
        if marker == 'D':
            fixture.symbols = fixture.symbols.replace('AsteriaCorridorAliasC', 'AsteriaCorridorAliasD')
            fixture.disassembly = fixture.disassembly.replace('AsteriaCorridorAliasC', 'AsteriaCorridorAliasD')
            fixture.graphs[0] = fixture.graphs[0].replace('AsteriaCorridorAliasC', 'AsteriaCorridorAliasD')
        return fixture

    def test_non_coincident_aliases(self):
        for marker in ('C', 'D'):
            fixture = self.moved_alias(marker)
            name = 'AsteriaCorridorAlias%s1' % marker
            fixture.symbols = fixture.symbols.replace('0000000000001200 T %s' % name, '0000000000001300 T %s' % name)
            fixture.disassembly = (fixture.disassembly.replace('b0=1200 <%s>' % name, 'b0=1300 <%s>' % name)
                                   .replace('0000000000001200 <%s>:' % name, '0000000000001300 <%s>:' % name)
                                   .replace(' 1200:', ' 1300:').replace(' 1210:', ' 1310:'))
            with self.subTest(marker=marker):
                self.rejects(fixture, 'unresolved call-graph node %s' % name)

    def test_alias_with_a_shared_and_an_extra_body(self):
        for marker in ('C', 'D'):
            fixture = self.moved_alias(marker)
            name = 'AsteriaCorridorAlias%s1' % marker
            fixture.symbols += '0000000000001300 T %s\n' % name
            fixture.disassembly = fixture.disassembly.replace('b0=1200 <%s>' % name, 'b0=1300 <%s>' % name)
            fixture.disassembly += '0000000000001300 <%s>:\n 1300: [MII] alloc r127=ar.pfs,96,0,0\n' % name
            with self.subTest(marker=marker):
                self.rejects(fixture, 'unresolved call-graph node %s' % name)

    # Roots.
    def test_non_text_root(self):
        fixture = base()
        fixture.symbols = fixture.symbols.replace(' T %s' % ROOT, ' D %s' % ROOT)
        self.rejects(fixture, 'corridor root %s' % ROOT)

    def test_missing_root(self):
        fixture = base()
        fixture.symbols = ''.join(line + '\n' for line in fixture.symbols.splitlines() if not line.endswith(' ' + ROOT))
        fixture.disassembly = fixture.disassembly.replace('<%s>:' % ROOT, '<Unlisted>:')
        self.rejects(fixture, 'corridor root %s' % ROOT)

    # Graph shape and indirect calls.
    def test_final_indirect_call_omitted_from_the_call_graph(self):
        fixture = base()
        fixture.insert_after(r'^ 1000: .*alloc ', ' 1006: [MIB] br.call.sptk.many b0=b6')
        self.rejects(fixture, '1 indirect branches in the image but 0 in the call graph')

    def test_reachable_cycle(self):
        fixture = base()
        fixture.append_graph('edge: { sourcename: "AsteriaCorridorAliasC2" targetname: "%s" }\n' % ROOT)
        self.rejects(fixture, 'call cycle through')

    def test_unbound_indirect_call(self):
        fixture = base()
        fixture.append_graph('edge: { sourcename: "%s" targetname: "__indirect_call" }\n' % ROOT)
        fixture.insert_after(r'^ 1000: .*alloc ', ' 1006: [MIB] br.call.sptk.many b0=b6')
        self.rejects(fixture, 'unbound indirect call in %s' % ROOT)

    def test_missing_reachable_node(self):
        fixture = base()
        fixture.append_graph('edge: { sourcename: "%s" targetname: "AsteriaMissingCorridorNode" }\n' % ROOT)
        self.rejects(fixture, 'unresolved call-graph node AsteriaMissingCorridorNode')

    def test_cooperative_switch_reachable(self):
        fixture = base()
        fixture.append_graph('edge: { sourcename: "AsteriaCorridorLeaf" '
                             'targetname: "_ZN7Asteria8Platform4Ia649Scheduler17Ia64KernelContext8SwitchToERS3_" }\n')
        self.rejects(fixture, 'reaches the cooperative switch')

    def test_bound_indirect_calls(self):
        fixture = base()
        fixture.append_graph('edge: { sourcename: "AsteriaCorridorLeaf" targetname: "__indirect_call" }\n')
        fixture.insert_after(r'^ 1100: ', ' 1106: [MIB] br.call.sptk.many b0=b6')
        fixture.symbols += '0000000000002000 T AsteriaCorridorBound\n'
        fixture.append_graph(r'node: { title: "AsteriaCorridorBound" label: "bound\n8 bytes (static)" }' '\n')
        bound = Policy(POLICY.roots, (Binding('AsteriaCorridorLeaf', 1, ('AsteriaCorridorBound',)),),
                       POLICY.leaves, POLICY.banned, POLICY.bound_bytes)
        analysis = fixture.analyze(bound)
        self.assertEqual((analysis.problems, analysis.memory_bytes), ([], 16 + 64 + 32 + 8))
        wrong = Policy(bound.roots, (Binding('AsteriaCorridorLeaf', 2, ('AsteriaCorridorBound',)),),
                       bound.leaves, bound.banned, bound.bound_bytes)
        self.rejects(fixture, 'the binding table expects 2', wrong)
        missing = Policy(bound.roots, (Binding('AsteriaCorridorLeaf', 1, ('AsteriaCorridorMissing',)),),
                         bound.leaves, bound.banned, bound.bound_bytes)
        self.rejects(fixture, 'bound indirect target AsteriaCorridorMissing is not a final text symbol', missing)

    def test_defined_function_absent_from_the_image(self):
        fixture = base()
        fixture.append_graph(r'node: { title: "AsteriaCorridorAbsent" label: "absent\n32 bytes (static)" }' '\n'
                             'edge: { sourcename: "AsteriaCorridorLeaf" targetname: "AsteriaCorridorAbsent" }\n')
        self.rejects(fixture, '32-byte frame but is absent from the image')

    def test_malformed_alloc_frames(self):
        for frame in ('97,97,0', '16,16,4', '4,6,0', '8,4,8'):
            fixture = base()
            fixture.disassembly = fixture.disassembly.replace('alloc r34=ar.pfs,4,3,0', 'alloc r34=ar.pfs,' + frame)
            with self.subTest(frame=frame):
                self.rejects(fixture, 'malformed alloc frame')

    # Assembly leaves.
    def test_assembly_leaf_changes(self):
        for text in ('alloc r34=ar.pfs,1,0,0', 'adds r12=-16,r12', 'st8 [r12]=r14,8', 'cover',
                     'br.call.sptk.many b0=b6'):
            fixture = leaf()
            fixture.insert_after(r'^ 1100:', ' 1106: [MII] ' + text)
            with self.subTest(text=text):
                self.rejects(fixture, 'assembly leaf AsteriaIa64ReadIntervalTimeCounter changes corridor state')

    def test_uart_leaf_needs_exactly_two_rse_writes(self):
        for writes, accepted in ((1, False), (2, True), (3, False)):
            fixture = leaf()
            fixture.symbols = fixture.symbols.replace('AsteriaIa64ReadIntervalTimeCounter',
                                                      'AsteriaIa64TryWriteDiagnosticCharacterPhysical')
            fixture.disassembly = fixture.disassembly.replace('AsteriaIa64ReadIntervalTimeCounter',
                                                              'AsteriaIa64TryWriteDiagnosticCharacterPhysical')
            fixture.graphs[0] = fixture.graphs[0].replace('AsteriaIa64ReadIntervalTimeCounter',
                                                          'AsteriaIa64TryWriteDiagnosticCharacterPhysical')
            for index in range(writes):
                fixture.insert_after(r'^ 1100:', ' %x: [MMI] mov.m ar.rsc=r%d' % (0x1106 + index, 14 + index))
            with self.subTest(writes=writes):
                if accepted:
                    self.assertEqual(fixture.analyze().problems, [])
                else:
                    self.rejects(fixture, '%d RSE-control writes (expected 2)' % writes)


if __name__ == '__main__':
    unittest.main()
