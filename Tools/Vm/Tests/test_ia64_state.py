# SPDX-License-Identifier: Apache-2.0
"""The `info registers` parser (Observers.Ia64State) on recorded and mutated dumps.

The parser rejects duplicate or incomplete records, NUL bytes and
malformed TLB records.
"""
from __future__ import annotations

import unittest

from AsteriaVm.Observers import Ia64State
from AsteriaVm.Observers.Ia64State import StateError
from AsteriaVm import Ia64Layout
from AsteriaVm.Qmp import Qmp
from Tests.test_ia64_fixtures import expectation, registers

# The test firmware's idle loop in the Smp4 recording, from its ELF facts.
IDLE = expectation('Smp4').facts.user(Ia64Layout.USER_IDLE_SYMBOL).value


def mutate(text: str, old: str, new: str) -> str:
    assert old in text, old
    return text.replace(old, new, 1)


class ParseTests(unittest.TestCase):
    def setUp(self):
        self.text = registers('SyntheticProbe')
        self.state = Ia64State.parse(self.text)

    def test_recorded_dump(self):
        state = self.state
        expected_psr = expectation('SyntheticProbe').constants.probe_user_psr
        self.assertEqual((state.ip, state.psr), (0x2000000001001100, expected_psr))
        self.assertEqual(Ia64State.schema_problems(state), [])
        self.assertEqual(state.get('META'), {'ip': 0x2000000001001100, 'psr': expected_psr, 'halted': 0})
        self.assertEqual(state.value('EXCEPTION', 'code'), 0)
        self.assertEqual(state.general(12), (0x2000000002004FF0, 0))
        self.assertEqual(state.general(0), (0, 0))
        self.assertEqual(state.branch(0), 0x2000000001001060)
        self.assertEqual(state.value('PR', 'value'), 0x41)
        self.assertEqual(state.get('CFM:')['sof'], 4)
        self.assertEqual(state.get('RSE')['invalid'], 92)
        self.assertEqual(state.value('IHA', 'rr0'), 0x200030)
        self.assertEqual(state.value('CR.DCR', 'itm'), 0xFFFFFFFFFFFFFFFE)
        entries = state.tlb()
        self.assertEqual(len(state.translation_registers('ITLB')), 1)
        self.assertEqual(len(state.translation_registers('DTLB')), 4)
        self.assertEqual(len(state.translation_cache()), 4)
        self.assertEqual(entries[0].describe(),
                         'ITLB[0] TC va=0x2000000001001000 pa=0x1000 ps=0x1000 rid=0x002011 key=0x0 ar=1 pl=3 '
                         'perm=0x5 pte=0x00100000000013a1')

    def test_cfm_bases_belong_to_records(self):
        names = ('sof', 'sol', 'sor', 'rrb_gr', 'rrb_fr', 'rrb_pr')
        for value in (10, 16, 96):
            machine = 'IA64STATE CFM ' + ' '.join('%s=%016x' % (name, value) for name in names) + '\n'
            human = ('CFM: sof={0} sol={0} sor={0} rrb.gr={0} rrb.fr={0} rrb.pr={0} '
                     'AR.PFS=0x0000000000000000 BSP=0x0000000000000000 BSPSTORE=0x0000000000000000\n').format(value)
            state = Ia64State.parse(machine + human)
            for name in names:
                with self.subTest(value=value, name=name):
                    self.assertEqual(state.value('CFM', name), value)
                    self.assertEqual(state.value('CFM:', name), value)
            with self.assertRaises(StateError):
                Ia64State.parse(machine.replace('sof=', 'sof=g')).get('CFM')

    def test_default_image_dump(self):
        state = Ia64State.parse(registers('Smp4'))
        self.assertEqual(state.ip, IDLE)
        self.assertEqual(state.general(1), (0x2000000004020800, 0))

    def test_incomplete_or_binary_text_is_unusable(self):
        for text in (self.text.rstrip('\n'), self.text + 'DTLB[999] malformed', self.text + 'IT\0LB[999] x\n'):
            state = Ia64State.parse(text)
            for read in (lambda: state.ip, lambda: state.general(4), lambda: state.tlb(), lambda: state.branch(0)):
                with self.assertRaises(StateError):
                    read()

    def test_conflicting_records_are_refused(self):
        # A second record of any kind the checks read.
        for prefix, line in (('IP: ', 'IP: conflicting-state'), ('IVA: ', 'IVA: conflicting-state'),
                             ('IHA: ', 'IHA: conflicting-state'), ('CFM: ', 'CFM: conflicting-state'),
                             ('RSE: ', 'RSE: conflicting-state'),
                             ('IA64STATE META ', 'IA64STATE META conflicting-state'),
                             ('IA64STATE AR ', 'IA64STATE AR conflicting-state'),
                             ('IA64STATE CFM ', 'IA64STATE CFM conflicting-state'),
                             ('IA64STATE EXCEPTION ', 'IA64STATE EXCEPTION conflicting-state')):
            key = next(key for key, (record_prefix, _) in Ia64State._RECORDS.items() if record_prefix == prefix)
            state = Ia64State.parse(self.text + line + '\r\n')
            with self.assertRaises(StateError, msg=prefix):
                state.get(key)

    def test_missing_and_malformed_records_are_refused(self):
        with self.assertRaises(StateError):
            Ia64State.parse(mutate(self.text, 'IA64STATE META ', 'IA64STATE-META ')).get('META')
        with self.assertRaises(StateError):
            Ia64State.parse(mutate(self.text, 'HALTED: 0', 'HALTED: x')).get('IP')
        with self.assertRaises(StateError):
            Ia64State.parse(mutate(self.text, 'IVA: 0x', 'IVA: 0y')).get('IVA')
        with self.assertRaises(StateError):
            Ia64State.parse(mutate(self.text, ' RR7: 0x0000000000201730', '')).get('IHA')

    def test_general_and_branch_records(self):
        with self.assertRaises(StateError):   # duplicated index
            Ia64State.parse(self.text + 'IA64STATE GR index=00c value=0000000000000000 nat=0\r\n').general(12)
        with self.assertRaises(StateError):   # Any malformed GR record poisons every GR read
            Ia64State.parse(self.text + 'IA64STATE GR index=00c conflicting-state\r\n').general(4)
        with self.assertRaises(StateError):
            Ia64State.parse(mutate(self.text, 'IA64STATE GR index=025 ', 'IA64STATE GX index=025 ')).general(37)
        with self.assertRaises(StateError):
            Ia64State.parse(self.text + 'IA64STATE BR index=007 value=00\r\n').branch(0)
        self.assertEqual(Ia64State.parse(mutate(self.text, 'index=020 value=4153544552494133 nat=0',
                                                'index=020 value=4153544552494133 nat=1')).general(32),
                         (0x4153544552494133, 1))

    def test_malformed_translation_entries_are_refused(self):
        for line in ('ITLB[999] TC va=0x0000000006000000 pa=0x0000000006000000',
                     'DTLB[1] TX va=0x0000000000270000 pa=0x0000000000270000 ps=0x0000000000004000 rid=0x002000'
                     ' key=0x000000 ar=2 pl=0 perm=0x3 pte=0x0010000000270461'):
            with self.assertRaises(StateError, msg=line):
                Ia64State.parse(self.text + line + '\r\n').tlb()

    def test_schema_version(self):
        state = Ia64State.parse(mutate(self.text, 'version=0000000000000001', 'version=0000000000000002'))
        self.assertTrue(Ia64State.schema_problems(state))


class QmpAccessTests(unittest.TestCase):
    def test_read_asks_for_one_cpu(self):
        text = registers('Smp4')
        calls = []

        class FakeQmp:
            hmp = Qmp.hmp   # the real wrapper, over the fake execute

            def execute(self, command, arguments, timeout=30.0):
                calls.append((command, arguments))
                return text

        state = Ia64State.read(FakeQmp(), 2)
        self.assertEqual(calls, [('human-monitor-command', {'command-line': 'info registers', 'cpu-index': 2})])
        self.assertEqual(state.cpu, 2)
        self.assertEqual(state.ip, IDLE)


if __name__ == '__main__':
    unittest.main()
