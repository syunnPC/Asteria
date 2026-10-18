# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the initial-user image owner checks
(TargetChecks/Ia64InitialUserImageOwners.py); the call-site and control-flow
helpers they use are tested with the library."""
from __future__ import annotations

import dataclasses
import unittest

from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.TargetChecks import Ia64InitialUserImageOwners as Owners
from Tests.test_library import bundles


def function(*groups):
    return Code(0x100, tuple(bundles(*groups)))


# Two nested loops around a flush, as PublishExecutableSlices compiles at O2:
# the bundle loop closes with br.cloop, the segment loop with a predicate.
PUBLISH = (
    ('MMI', 'mov r19=r0', 'adds r34=72,r34', 'mov.i r2=ar.lc'),                    # 0x100
    ('MMB', 'cmp.eq p6,p7=0,r35', 'nop.m 0x0', '(p06) br.cond.dpnt.few <+0x60>;;'),  # 0x110 → barrier
    ('MMI', 'ld8 r14=[r34]', 'mov r16=r0', 'mov.i ar.lc=r14;;'),                   # 0x120 segment loop head
    ('MII', 'fc.i r16', 'adds r16=16,r16', 'nop.i 0x0'),                           # 0x130 bundle loop head
    ('MIB', 'nop.m 0x0', 'nop.i 0x0', 'br.cloop.sptk.few <+0x30>;;'),              # 0x140
    ('MIB', 'adds r19=1,r19', 'cmp.eq p7,p6=r19,r35', '(p06) br.cond.dptk.few <+0x20>;;'),  # 0x150
    ('MMI', 'sync.i;;', 'srlz.i', 'nop.i 0x0;;'),                                  # 0x160 barrier
    ('MIB', 'mf', 'mov.i ar.lc=r2', 'br.ret.sptk.many b0;;'),                      # 0x170
)


class PublicationTests(unittest.TestCase):
    def test_nested_flush_loop(self):
        self.assertEqual(Owners.Ia64InitialUserImagePublication().verify(function(*PUBLISH)), [])

    def test_rejected_publications(self):
        def edited(position, slot, text):
            groups = [list(group) for group in PUBLISH]
            groups[position][slot + 1] = text
            return function(*groups)
        cases = {
            'predicated fc.i': edited(3, 0, '(p06) fc.i r16'),
            'eight-byte stride': edited(3, 1, 'adds r16=8,r16'),
            'no bundle loop': edited(4, 2, 'nop.b 0x0;;'),
            'no segment loop': edited(5, 2, 'nop.b 0x0;;'),
            'srlz.i missing': edited(6, 1, 'nop.m 0x0'),
            'mf before srlz.i': edited(6, 1, 'mf'),
            'early return': edited(5, 0, '(p07) br.ret.sptk.many b0'),
        }
        for label, stream in cases.items():
            with self.subTest(label):
                self.assertNotEqual(Owners.Ia64InitialUserImagePublication().verify(stream), [])


class DependencyTests(unittest.TestCase):
    def problems(self, undefined, allowed=None, required=(), **changes):
        facts = Owners.Dependencies('owner', tuple(undefined), tuple(undefined), allowed, r'(AddressSpace)', False,
                                    tuple(required))
        return Owners.Ia64InitialUserImageDependencies().verify((dataclasses.replace(facts, **changes),))

    def test_rules(self):
        self.assertEqual(self.problems(['memcpy'], frozenset({'memcpy'})), [])
        self.assertNotEqual(self.problems(['memcpy', 'memset'], frozenset({'memcpy'})), [])
        self.assertNotEqual(self.problems([], frozenset({'memcpy'})), [])
        # A denied dependency is rejected even when the allowlist admits it.
        self.assertNotEqual(self.problems(['AddressSpace::Map'], frozenset({'AddressSpace::Map'})), [])
        self.assertNotEqual(self.problems(['memcpy'], required=['Complete']), [])
        self.assertEqual(self.problems(['CompleteMapping'], required=['Complete']), [])

    def test_mapping_owner_rules_are_on_demangled_names(self):
        self.assertNotEqual(self.problems(['_ZN7Asteria6Kernel9Scheduler3RunEv'],
                                          demangled=('Asteria::Kernel::Scheduler::Run()',),
                                          denied=Owners.MAPPING_OWNER_DENIED, denied_demangled=True), [])


if __name__ == '__main__':
    unittest.main()
