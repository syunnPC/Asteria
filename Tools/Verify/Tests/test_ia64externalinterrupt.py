# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the pure logic of TargetChecks/Ia64ExternalInterrupt.py and
Ia64NativePreemption.py: the symbolic trace, branch conditions, the
EOI-accounting and PSR.i-window path analyses, the operand rewrites, the
landing contract and the coordinator call-site judgment. No build tree is
needed.

Run with: python3 -m unittest discover -s Tools/Verify/Tests -t Tools/Verify
"""
from __future__ import annotations

import unittest

from AsteriaVerify import Properties
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import STB_GLOBAL, STB_LOCAL, STT_NOTYPE, STV_HIDDEN, Symbol
from AsteriaVerify.TargetChecks import Ia64ExternalInterrupt as K
from AsteriaVerify.TargetChecks import Ia64NativePreemption as L


def code(*texts: str) -> Code:
    """One instruction per bundle at 16 * index; `text;;` ends a group and
    `(pN) text` is predicated. Branch targets are written `<+0xOFF>`."""
    instructions = []
    for index, text in enumerate(texts):
        stop = text.endswith(';;')
        body = text[:-2] if stop else text
        predicate = None
        if body.startswith('('):
            predicate, body = body[1:].split(') ', 1)
        instructions.append(Instruction(16 * index, body, 'MII', 0, stop, predicate))
    return Code(0, tuple(instructions))


class ValueTests(unittest.TestCase):
    def test_sums_are_normalized(self):
        base = ('in', 'r4')
        self.assertEqual(K.plus(K.plus(base, 8), 8), ('add', base, 16))
        self.assertEqual(K.plus(K.plus(base, 8), -8), base)
        self.assertEqual(K.plus(('k', 2), 3), ('k', 5))


class TraceTests(unittest.TestCase):
    def test_banked_spills_collect_nat_bits_from_a_cleared_unat(self):
        trace = K.Trace(code('mov.m ar.unat=r0', 'adds r14=488,r19;;', 'bsw.1;;', 'st8.spill [r14]=r16,8;;',
                             'st8.spill [r14]=r17;;', 'mov.m r16=ar.unat;;', 'bsw.0;;', 'st8 [r15]=r16'),
                        registers={'r19#0': ('sym', 'frame')})
        spills = [access for access in trace.stores() if 'spill' in access.completers]
        self.assertEqual([access.value for access in spills], [('in', 'r16#1'), ('in', 'r17#1')])
        frame = ('sym', 'frame')
        self.assertEqual([access.address for access in spills], [K.plus(frame, 488), K.plus(frame, 496)])
        self.assertEqual(trace.gr['r16#1'], ('unat', K.ZERO, ('r16#1', 'r17#1')))
        # after bsw.0, r16 names the bank-0 register again
        self.assertEqual(trace.stores()[-1].value, ('in', 'r16#0'))

    def test_cover_changes_the_covered_reads(self):
        trace = K.Trace(code('mov r16=cr.ifs', 'cover;;', 'mov r17=cr.ifs', 'mov.m r18=ar.bsp'))
        self.assertEqual(trace.gr['r16#0'], ('in', 'cr.ifs'))
        self.assertEqual(trace.gr['r17#0'], ('covered', 'cr.ifs'))
        self.assertEqual(trace.gr['r18#0'], ('covered', 'ar.bsp'))

    def test_resource_writes_record_their_source_instruction(self):
        trace = K.Trace(code('adds r14=256,r4;;', 'ld8 r15=[r14],8;;', 'mov.i ar.lc=r15'),
                        registers={'r4': ('sym', 'frame')})
        write = trace.writes_of('ar.lc')[0]
        self.assertEqual(write.value, K.load(K.plus(('sym', 'frame'), 256)))
        self.assertEqual(write.source_writer, 1)
        self.assertEqual(trace.at(1).base_writer, 0)

    def test_a_call_clobbers_scratch_and_invalidates_sp_and_bsp(self):
        trace = K.Trace(code('mov r7=r12', 'mov.m r6=ar.bsp;;', 'br.call.sptk.many b0=b6;;', 'mov.m r14=ar.bsp'))
        call = [branch for branch in trace.branches if branch.call][0]
        self.assertEqual(dict(call.state)['r12'], ('in', 'r12'))
        self.assertEqual(trace.gr['r7'], ('in', 'r12'))
        self.assertEqual(trace.gr['r12'], ('op', 'after-call', 'r12', 2))
        self.assertEqual(trace.gr['r14'], ('op', 'after-call', 'ar.bsp', 2))

    def test_compare_conditions_follow_the_branch_predicate(self):
        trace = K.Trace(code('mov r18=15', 'cmp.eq p7,p6=r20,r18;;', '(p06) br.cond.dpnt.few <+0x0>;;'))
        condition = trace.conditions[2]
        self.assertTrue(K.means(condition, 'ne', ('k', 15), ('in', 'r20#0')))
        self.assertFalse(K.means(condition, 'eq', ('in', 'r20#0'), ('k', 15)))
        self.assertTrue(K.means(('not', condition), 'eq', ('in', 'r20#0'), ('k', 15)))


# A miniature legacy entry: claim, spurious exit, EOI then rfi, and a
# claimed-vector failure that EOIs before its fail-stop.
ENTRY = (
    'mov r31=pr;;',                                   # 0
    'mov r20=cr.ivr;;',                               # 1
    'srlz.d;;',                                       # 2
    'mov r18=15',                                     # 3
    'cmp.eq p6,p7=r20,r18;;',                         # 4
    '(p06) br.cond.dpnt.few <+0x90>;;',               # 5 -> 9 (spurious)
    'cmp.eq p6,p7=r20,r0;;',                          # 6
    '(p06) br.cond.dpnt.few <+0xb0>;;',               # 7 -> 11 (claimed failure)
    'br.few <+0xf0>;;',                               # 8 -> 15 (success)
    'mov pr=r31,0xfffffffffffffffe;;',                # 9
    'rfi;;',                                          # 10
    'mov cr.eoi=r0;;',                                # 11
    'srlz.d;;',                                       # 12
    'hint.m 0;;',                                     # 13
    'br.few <+0xd0>;;',                               # 14 fail-stop
    'mov cr.eoi=r0;;',                                # 15
    'srlz.d;;',                                       # 16
    'mov pr=r31,0xfffffffffffffffe;;',                # 17
    'rfi;;',                                          # 18
)


class EoiTests(unittest.TestCase):
    def test_accounting_holds(self):
        entry = code(*ENTRY)
        self.assertEqual(K.eoi_problems(entry, K.Trace(entry)), [])

    def test_missing_cleanup_eoi_is_found(self):
        entry = code(*ENTRY).nop(11)
        problems = K.eoi_problems(entry, K.Trace(entry))
        self.assertTrue(any('0 EOIs' in problem for problem in problems), problems)

    def test_eoi_on_the_spurious_path_is_found(self):
        texts = list(ENTRY)
        texts[9] = 'mov cr.eoi=r0;;'
        entry = code(*texts)
        problems = K.eoi_problems(entry, K.Trace(entry))
        self.assertTrue(any('spurious' in problem for problem in problems), problems)

    def test_fail_stop_looping_over_an_eoi_is_found(self):
        entry = code(*ENTRY).retarget(14, 0xb0)
        self.assertTrue(K.eoi_problems(entry, K.Trace(entry)))

    def test_pr_protocol(self):
        predicates = K.Ia64ExternalInterruptEntryPredicates()
        self.assertEqual(predicates.verify(code(*ENTRY)), [])
        self.assertTrue(predicates.verify(code(*ENTRY).nop(17)))


POLL = (
    'ld8.acq r14=[r32];;',                            # 0
    'ssm 0x4000;;',                                   # 1
    'srlz.d;;',                                       # 2
    'ld8.acq r14=[r32];;',                            # 3 loop head
    'cmp.eq p6,p7=r14,r33;;',                         # 4
    '(p06) br.cond.dpnt.few <+0x80>;;',               # 5 -> rsm
    'hint.m 0;;',                                     # 6
    'br.few <+0x30>;;',                               # 7 -> 3
    'rsm 0x4000;;',                                   # 8
    'srlz.d;;',                                       # 9
    'mov r8=r0',                                      # 10
    'br.ret.sptk.many b0;;',                          # 11
)


def window(texts, **options) -> list[str]:
    settings = dict(strict_exit=False, window_allows=K.poll_window_allows, back_edges=1, disables=1)
    settings.update(options)
    return Properties.psr_i_window_problems(code(*texts) if isinstance(texts, tuple) else texts, **settings)


class WindowTests(unittest.TestCase):
    def test_poll_window_holds(self):
        self.assertEqual(window(POLL), [])

    def test_return_inside_the_window_is_found(self):
        texts = list(POLL)
        texts[6] = 'br.ret.sptk.many b0;;'
        self.assertTrue(window(tuple(texts)))

    def test_unserialized_enable_is_found(self):
        self.assertTrue(window(code(*POLL).nop(2)))

    def test_missing_loop_branch_is_found(self):
        self.assertTrue(window(code(*POLL).nop(7)))

    def test_exit_without_masking_is_found(self):
        texts = list(POLL)
        texts[5] = '(p06) br.cond.dpnt.few <+0xa0>;;'    # skips the rsm
        self.assertTrue(any('enabled' in problem for problem in window(tuple(texts))))

    def test_strict_exit_requires_a_mask_on_every_return(self):
        sentinel = code(
            'cmp.eq p6,p7=r32,r0;;',                  # 0
            '(p06) br.cond.dpnt.few <+0x70>;;',       # 1 -> 7 (reject)
            'ssm 0x4000;;',                           # 2
            'srlz.d;;',                               # 3 landing
            'rsm 0x4000;;',                           # 4
            'srlz.d;;',                               # 5
            'br.ret.sptk.many b0;;',                  # 6
            'rsm 0x4000;;',                           # 7 reject re-mask
            'srlz.d;;',                               # 8
            'br.ret.sptk.many b0;;')                  # 9
        options = dict(strict_exit=True, window_allows=lambda op: op.mnemonic == 'srlz.d', back_edges=0,
                       disables=None)
        self.assertEqual(window(sentinel, **options), [])
        problems = window(sentinel.nop(7), **options)
        self.assertEqual(problems, ['br.ret.sptk.many b0 (#9) is reached without re-masking interrupts'])
        self.assertEqual(window(sentinel, **dict(options, strict_exit=False)), [])
        self.assertEqual(window(sentinel.nop(7), **dict(options, strict_exit=False)), [])


class MutationHelperTests(unittest.TestCase):
    def test_shift_immediate_and_increment(self):
        base = code('adds r18=144,r19;;', 'st8.spill [r16]=r1,8;;')
        self.assertEqual(K.shift_immediate(base, 0, 8).instructions[0].text, 'adds r18=152,r19')
        self.assertEqual(K.set_increment(base, 1, 16).instructions[1].text, 'st8.spill [r16]=r1,16')


def marker(name: str, value: int, binding: int = STB_GLOBAL) -> Symbol:
    return Symbol(name, value, 0, STT_NOTYPE, binding, STV_HIDDEN, 1)


def landing(*slots: tuple[str, bool], start: int = 0x500, end: int = 0x510) -> L.LandingFacts:
    """Six slots (two bundles) at `start`; each slot is (text, stop)."""
    bundles = tuple(Instruction(start + 16 * (index // 3) + 6 * (index % 3), text, 'MII', index % 3, stop)
                    for index, (text, stop) in enumerate(slots))
    return L.LandingFacts((marker(L.LANDING_START, start),), (marker(L.LANDING_END, end),), bundles)


LANDING = (('srlz.d', False), ('nop.i 0x0', False), ('nop.i 0x0', True),
           ('rsm 0x4000', True), ('srlz.d', False), ('nop.i 0x0', True))


class LandingTests(unittest.TestCase):
    def test_exact_landing_holds(self):
        self.assertEqual(L.Ia64BspNativePreemptionLanding().verify(landing(*LANDING)), [])

    def test_second_instruction_and_missing_stop_are_found(self):
        prop = L.Ia64BspNativePreemptionLanding()
        slots = list(LANDING)
        slots[1] = ('mov r14=r0', False)
        self.assertTrue(prop.verify(landing(*slots)))
        slots = list(LANDING)
        slots[2] = ('nop.i 0x0', False)
        self.assertTrue(prop.verify(landing(*slots)))

    def test_two_bundle_range_and_local_marker_are_found(self):
        prop = L.Ia64BspNativePreemptionLanding()
        self.assertTrue(prop.verify(landing(*LANDING, end=0x520)))
        facts = landing(*LANDING)
        local = L.LandingFacts((marker(L.LANDING_START, 0x500, STB_LOCAL),), facts.ends, facts.bundles)
        self.assertTrue(prop.verify(local))

    def test_self_test_mutants_are_all_rejected(self):
        prop = L.Ia64BspNativePreemptionLanding()
        facts = landing(*LANDING)
        for description, mutant in prop.mutants(facts):
            self.assertNotEqual(mutant, facts, description)
            self.assertTrue(prop.verify(mutant), description)


DISPATCH = '_ZN7Asteria6Kernel9Scheduler22ProcessorKernelRunLoopI1KE44DispatchRetainedExternalFrameFromCoordinatorEv'


class CoordinatorTests(unittest.TestCase):
    def coordinator(self, *texts):
        return code(*texts)

    def test_one_retained_dispatch_holds(self):
        prop = L.Ia64BspNativePreemptionCoordinator()
        facts = self.coordinator('br.call.sptk.many b0=<reloc:PCREL21B Other>;;',
                                 'br.call.sptk.many b0=<reloc:PCREL21B %sTransaction>;;' % DISPATCH[:-2],
                                 'br.ret.sptk.many b0;;')
        self.assertEqual(prop.verify(facts), [])
        for description, mutant in prop.mutants(facts):
            self.assertTrue(prop.verify(mutant), description)

    def test_context_switch_reference_is_found(self):
        prop = L.Ia64BspNativePreemptionCoordinator()
        facts = self.coordinator('br.call.sptk.many b0=<%s>;;' % DISPATCH,
                                 'movl r14=<_ZN7Asteria6Kernel9Scheduler5Run15SwitchToIdleEv>', 'br.ret.sptk.many b0;;')
        self.assertEqual(prop.verify(facts), ['#1 references _ZN7Asteria6Kernel9Scheduler5Run15SwitchToIdleEv'])


if __name__ == '__main__':
    unittest.main()
