# SPDX-License-Identifier: Apache-2.0
"""Unit tests of AsteriaVerify.ControlFlow: Code navigation, editing and the
control-flow graph. No build tree is needed."""
from __future__ import annotations

import unittest

from AsteriaVerify.ControlFlow import END, RETURN, Code, SiteError
from AsteriaVerify.Disassembly import Instruction


def code(*bundles, start: int = 0) -> Code:
    """Code from bundles of three slot texts; `text;;` carries a stop and
    `(pN) text` a predicate."""
    instructions = []
    for number, slots in enumerate(bundles):
        for slot, text in enumerate(slots):
            stop = text.endswith(';;')
            text = text[:-2] if stop else text
            predicate = None
            if text.startswith('('):
                predicate, text = text[1:].split(') ', 1)
            instructions.append(Instruction(start + 16 * number + 6 * slot, text, 'MII', slot, stop, predicate))
    return Code(start, tuple(instructions))


STREAM = code(('mov r16=1;;', 'nop.i 0x0', 'adds r17=8,r16'), ('st8 [r17]=r16;;', 'mov r16=1', 'nop.m 0x0;;'),
              ('hint.m 0;;', 'br.few <+0x20>;;', 'nop.i 0x0'))

# Two nested loops around a flush, as PublishExecutableSlices compiles at O2:
# the bundle loop closes with br.cloop, the segment loop with a predicate.
PUBLISH = code(
    ('mov r19=r0', 'adds r34=72,r34', 'mov.i r2=ar.lc'),                          # 0x0
    ('cmp.eq p6,p7=0,r35', 'nop.m 0x0', '(p06) br.cond.dpnt.few <+0x60>;;'),      # 0x10 -> barrier
    ('ld8 r14=[r34]', 'mov r16=r0', 'mov.i ar.lc=r14;;'),                         # 0x20 segment loop head
    ('fc.i r16', 'adds r16=16,r16', 'nop.i 0x0'),                                 # 0x30 bundle loop head
    ('nop.m 0x0', 'nop.i 0x0', 'br.cloop.sptk.few <+0x30>;;'),                    # 0x40
    ('adds r19=1,r19', 'cmp.eq p7,p6=r19,r35', '(p06) br.cond.dptk.few <+0x20>;;'),  # 0x50
    ('sync.i;;', 'srlz.i', 'nop.i 0x0;;'),                                        # 0x60 barrier
    ('mf', 'mov.i ar.lc=r2', 'br.ret.sptk.many b0;;'))                            # 0x70


class NavigationTests(unittest.TestCase):
    def test_steps_skip_nops_and_keep_groups(self):
        self.assertEqual([step.rendered for step in STREAM.steps],
                         ['mov r16=1', 'adds r17=8,r16', 'st8 [r17]=r16', 'mov r16=1', 'hint.m 0', 'br.few <+0x20>'])
        self.assertEqual([step.group for step in STREAM.steps], [0, 1, 1, 2, 3, 4])
        self.assertEqual(STREAM.steps[-1].target, 0x20)

    def test_queries(self):
        first, _, store, second = STREAM.steps[:4]
        self.assertEqual(STREAM.all('mov r16=1'), [first, second])
        self.assertEqual(STREAM.all('mov r16=1', after=first), [second])
        self.assertEqual(STREAM.all(lambda s: s.writes('r17')), [STREAM.steps[1]])
        self.assertIs(STREAM.first('adds r17=8,r16', 'mov r16=1'), second)
        self.assertIsNone(STREAM.first('st8 [r17]=r16', 'adds r17=8,r16'))
        self.assertEqual(STREAM.window(after=first, before=second), list(STREAM.steps[1:3]))
        self.assertIs(STREAM.at_offset(0x10), store)       # first non-nop at or after +0x10
        self.assertIs(STREAM.target(STREAM.steps[-1]), STREAM.steps[-2])
        self.assertIs(STREAM.next(store), second)
        self.assertIs(STREAM.previous(store), STREAM.steps[1])
        problems = []
        self.assertIsNone(STREAM.unique('mov r16=1', 'loads', problems))
        self.assertEqual(problems, ['2 loads, expected exactly one'])
        with self.assertRaises(SiteError):
            STREAM.site('rfi')

    def test_group_placement(self):
        stream = code(('mov r16=1;;', 'flushrs', 'nop.i 0x0;;'),
                      ('alloc r17=ar.pfs,0,0,0', 'mov r18=r0;;', 'nop.i 0x0'))
        flush, alloc = stream.steps[1], stream.steps[2]
        self.assertTrue(stream.starts_group(flush) and stream.ends_group(flush))
        self.assertTrue(stream.starts_group(alloc))
        self.assertFalse(stream.ends_group(alloc))

    def test_editing(self):
        store = STREAM.steps[2]
        inserted = STREAM.insert(store, 'ld8 r11=[r12]', after=True)
        self.assertEqual(len(inserted.instructions), len(STREAM.instructions) + 1)
        self.assertEqual(inserted.instructions[store.position + 1].address, store.instruction.address)
        self.assertFalse(inserted.instructions[store.position + 1].stop_after)
        before = STREAM.insert(store, '(p06) mov r1=r0', stop=True)
        self.assertEqual(before.instructions[store.position].rendered(), '(p06) mov r1=r0')
        self.assertTrue(before.instructions[store.position].stop_after)
        nopped = STREAM.nop(store)
        self.assertEqual(nopped.instructions[store.position].text, 'nop.m 0x0')
        self.assertTrue(nopped.instructions[store.position].stop_after)
        self.assertEqual(STREAM.rewrite(store, '(p7) st8 [r17]=r0').instructions[store.position].rendered(),
                         '(p7) st8 [r17]=r0')
        self.assertFalse(STREAM.without_stops(0, 1).instructions[0].stop_after)
        merged = STREAM.without_boundary(STREAM.steps[3])
        self.assertEqual(merged.steps[3].group, merged.steps[4].group)
        self.assertEqual(STREAM.retarget(STREAM.steps[-1], 0x10).steps[-1].text, 'br.few <+0x10>')
        self.assertEqual(STREAM.retarget(STREAM.steps[-1], 'Other').steps[-1].text, 'br.few <Other>')
        self.assertEqual(len(STREAM.duplicate(0).steps), len(STREAM.steps) + 1)
        self.assertEqual(len(STREAM.delete(0).steps), len(STREAM.steps) - 1)


class GraphTests(unittest.TestCase):
    def test_targets_resolve_to_the_first_meaningful_slot(self):
        graph = code(('(p06) br.cond.dpnt.few <+0x10>', 'mov r8=r0', 'nop.i 0x0;;'),
                     ('nop.m 0x0', 'mov r9=r0', 'br.ret.sptk.many b0;;')).graph
        self.assertEqual(graph.successors[0], (4, 1))
        self.assertEqual(graph.successors[5], (RETURN,))

    def test_call_resumes_at_the_next_bundle(self):
        graph = code(('nop.m 0x0', 'mov r14=r1', 'br.call.sptk.many b0=b6'),
                     ('mov r8=r0', 'nop.i 0x0', 'br.ret.sptk.many b0;;')).graph
        self.assertEqual(graph.successors[2], (3,))
        self.assertEqual(graph.successors[1], (2,))
        self.assertEqual(graph.indirect, [])

    def test_falling_off_the_end_and_indirect_branches(self):
        graph = code(('mov r8=r0', 'nop.i 0x0', 'br.sptk.many b6;;'), ('mov r9=r0', 'nop.i 0x0', 'nop.i 0x0')).graph
        self.assertEqual(graph.indirect, [2])
        self.assertEqual(graph.successors[3], (END,))
        self.assertEqual(graph.leaving(), {2, 3})

    def test_loops_and_dominators(self):
        graph = PUBLISH.graph
        flush = 9                                       # fc.i at 0x30
        around = sorted((body for body in graph.loops.values() if flush in body), key=len)
        self.assertEqual(len(around), 2)
        self.assertLess(around[0], around[1])
        self.assertTrue(graph.dominates(6, flush))      # the segment-loop head dominates the flush
        self.assertFalse(graph.dominates(flush, 18))    # the barrier is reachable without flushing
        self.assertEqual(graph.exits(around[0]), [(14, 15)])

    def test_reachability_avoiding_nodes_and_edges(self):
        graph = PUBLISH.graph
        self.assertIn(18, graph.reachable([9]))
        self.assertNotIn(RETURN, graph.reachable([9], avoiding={18}))
        self.assertNotIn(18, graph.reachable([0], removed={(5, 18), (17, 18)}))

    def test_nodes_reaching_a_node(self):
        graph = PUBLISH.graph
        self.assertTrue({0, 6, 9, 17} <= graph.reaching([18]))
        self.assertNotIn(21, graph.reaching([18]))           # after the barrier
        self.assertNotIn(0, graph.reaching([9], avoiding={6}))   # the flush is entered only through the segment head


if __name__ == '__main__':
    unittest.main()
