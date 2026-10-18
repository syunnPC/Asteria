# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the VT-d table publication judgments (Ia64VtdFlush). The
fixtures are the meaningful instructions of the real leaves, so no build
tree is needed.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import unittest

from AsteriaVerify.TargetChecks import Ia64VtdFlush as vtd
from Tests.test_ia64runtimehelpers import fixture

FLUSH_TABLES = fixture(0x0, """
0 add r16=r32,r33
6 dep r17=0,r32,0,5 ;;
10 cmp.ltu p6,p7=r17,r16 ;;
16 (p06) fc r17
1c (p06) adds r17=32,r17
2c (p06) br.cond.dptk.few <+0x10> ;;
30 sync.i ;;
36 srlz.i ;;
40 mf ;;
5c br.ret.sptk.many b0 ;;
""")

FENCE = fixture(0x60, """
0 mf ;;
1c br.ret.sptk.many b0 ;;
""")

FACTS = {vtd.FLUSH_TABLES: FLUSH_TABLES, vtd.FENCE: FENCE}
PROPERTY = vtd.Ia64VtdFlush()


def renamed(code, *pairs):
    """`code` with every instruction's rendered text rewritten by the (old, new) pairs."""
    for step in code.steps:
        text = step.rendered
        for old, new in pairs:
            text = text.replace(old, new)
        code = code.rewrite(step, text)
    return code


class VtdFlushTests(unittest.TestCase):
    def test_the_leaves_pass(self):
        self.assertEqual(PROPERTY.verify(FACTS), [])

    def test_the_required_mutants_are_declared_and_rejected(self):
        required = {
            'sync.i dropped', 'srlz.i dropped', 'mf dropped', 'sync.i and mf exchanged',
            'no stop between the loop and sync.i', 'stride 64', 'alignment to 64 bytes', 'fc predicated by p7',
            'extra instruction in the tail'}
        mutants = dict(PROPERTY.mutants(FACTS))
        self.assertLessEqual({'%s: %s' % (vtd.FLUSH_TABLES, name) for name in required}, set(mutants))
        self.assertIn('%s: mf dropped' % vtd.FENCE, mutants)
        for description, mutant in mutants.items():
            with self.subTest(mutant=description):
                self.assertNotEqual(mutant, FACTS)
                self.assertTrue(PROPERTY.verify(mutant))

    def test_equivalent_forms_are_accepted(self):
        align = FLUSH_TABLES.first('dep r17=0,r32,0,5')
        variants = {
            'and with -32 aligns like dep': FLUSH_TABLES.rewrite(align, 'and r17=-32,r32'),
            'bound and alignment exchanged': FLUSH_TABLES.swap(FLUSH_TABLES.first('add r16=r32,r33'), align),
            'registers renamed': renamed(FLUSH_TABLES, ('r16', 'r20'), ('r17', 'r21'), ('p6', 'p8'),
                                         ('p7', 'p9'), ('p06', 'p08')),
            'nop in the tail': FLUSH_TABLES.insert(FLUSH_TABLES.first('mf'), 'nop.m 0x0', stop=True),
        }
        for description, variant in variants.items():
            with self.subTest(variant=description):
                self.assertNotEqual(variant, FLUSH_TABLES)
                self.assertEqual(vtd.flush_problems(variant), [])

    def test_a_wider_and_mask_is_rejected(self):
        align = FLUSH_TABLES.first('dep r17=0,r32,0,5')
        self.assertTrue(vtd.flush_problems(FLUSH_TABLES.rewrite(align, 'and r17=-64,r32')))

    def test_the_summary_names_the_roles(self):
        self.assertIn('`(p6) fc r17` on every 32-byte line of [r32 & ~31, r32 + r33)', PROPERTY.summary(FACTS))


if __name__ == '__main__':
    unittest.main()
