# SPDX-License-Identifier: Apache-2.0
"""The synthetic probe's user-visible results (Observers.Ia64ProbeUser): parent spill, sentinel, stack-page and WaitSet results.

"""
from __future__ import annotations

import re
import struct
import unittest

from AsteriaVm.Drivers import Ia64SyntheticProbe
from AsteriaVm.Observers import Ia64ProbeUser, Ia64State
from Tests.test_ia64_fixtures import expectation, observation, registers


class ProbeMemoryTests(unittest.TestCase):
    def setUp(self):
        self.memory = observation('SyntheticProbe').memory
        self.page = self.memory['UserStackPage']
        self.layout = Ia64SyntheticProbe.probe_page(expectation('SyntheticProbe'))

    def test_recorded(self):
        self.assertEqual(Ia64ProbeUser.probe_page_problems(self.page, self.layout), [])
        self.assertEqual(Ia64ProbeUser.wait_response_problems(self.page, self.layout), [])
        # The derived probe delivery has rights 6.
        self.assertEqual(self.layout.delivery_bytes[:16].hex(), '41495044010030008800000058000600')
        self.assertEqual(self.layout.ipc_evidence_bytes.hex(),
                         '00000000000000000100000000000000' '00000000000000000000000000000000'
                         '01000000000000008800000000000000' '3341495245545341')

    def test_stack_page_mutants(self):
        def word(offset, value):
            return self.page[:offset] + struct.pack('<Q', value) + self.page[offset + 8:]
        mutants = {'endpoint selector': word(0x100, 0x100), 'endpoint authenticator': word(0x108, 0),
                   'frame': word(0x110, 0), 'WaitSet selector': word(0x168, 0x101),
                   'WaitSet authenticator': word(0x170, 0), 'send error': word(0x200, 1),
                   'receive bytes': word(0x228, 0x58), 'completion': word(0x230, 0),
                   'delivery rights': self.page[:0x30E] + b'\x07' + self.page[0x30F:],
                   'delivery payload': word(0x330, 0), 'query information': word(0xFE0, 0x70009),
                   'query lease': word(0xFE8, 0), 'stack sentinel': word(0xFF0, 0x4153544552494134),
                   'truncated': self.page[:-1]}
        for name, page in mutants.items():
            self.assertTrue(Ia64ProbeUser.probe_page_problems(page, self.layout), name)

    def test_wait_response_mutants(self):
        for offset, value in ((0x4D0, 1), (0x4D8, 0x5741495453455402), (0x4E0, 2)):
            page = self.page[:offset] + struct.pack('<Q', value) + self.page[offset + 8:]
            self.assertTrue(Ia64ProbeUser.wait_response_problems(page, self.layout), hex(offset))

    def test_rse_dumps(self):
        """Parent spill and sentinel: byte mutants and truncations fail."""
        sentinel = Ia64ProbeUser.word(0x4153544552494133)
        spill = b''.join(Ia64ProbeUser.word(0x4153544552494133 + index) for index in range(4))
        self.assertEqual(Ia64ProbeUser.memory_problems(self.memory, {'UserRseSentinel': sentinel,
                                                                     'UserRseParentSpill': spill}), [])
        for name, value, offset in (('UserRseSentinel', sentinel, 7), ('UserRseParentSpill', spill, 16)):
            data = self.memory[name]
            for mutant in (data[:offset] + b'\x00' + data[offset + 1:], data[:-1]):
                self.assertTrue(Ia64ProbeUser.memory_problems({name: mutant}, {name: value}), name)


class RegisterTests(unittest.TestCase):
    def test_probe_registers(self):
        # General, branch and predicate register mutants.
        constants = expectation('SyntheticProbe').constants
        general, branch = Ia64SyntheticProbe.probe_registers(constants)
        text = registers('SyntheticProbe')

        def problems(source):
            return Ia64ProbeUser.register_problems(Ia64State.parse(source), general, branch, 0x41)

        self.assertEqual(problems(text), [])
        # r5 holds the bootstrap authenticator, which differs on every boot.
        [authenticator] = re.findall(r'GR index=005 value=([0-9a-f]{16})', text)
        mutants = [('index=004 value=0000000000000100', 'index=004 value=0000000000000000'),
                   ('index=005 value=' + authenticator, 'index=005 value=0000000000000000'),
                   ('index=008 value=0000000000000000', 'index=008 value=0000000000000001'),
                   ('index=009 value=0000000000010001', 'index=009 value=0000000000000001'),
                   ('index=00a value=ffffffffffffffff', 'index=00a value=7fffffffffffffff'),
                   ('index=00c value=2000000002004ff0', 'index=00c value=2000000002004fe0'),
                   ('index=020 value=4153544552494133 nat=0', 'index=020 value=4153544552494133 nat=1'),
                   ('BR index=000 value=2000000001001060', 'BR index=000 value=2000000001001070'),
                   ('BR index=007 value=0000000000000000', 'BR index=007 value=0000000000000001'),
                   ('PR value=0000000000000041', 'PR value=0000000000000043')]
        mutants += [('index=%03x value=%016x' % (index, 0x4153544552494133 + index - 32),
                     'index=%03x value=%016x' % (index, 0x4153544552494132 + index - 32)) for index in range(32, 36)]
        mutants += [('index=%03x value=0000000000000000' % index, 'index=%03x value=0000000000000001' % index)
                    for index in range(14, 32)]
        for old, new in mutants:
            self.assertIn(old, text)
            self.assertTrue(problems(text.replace(old, new)), old)

    def test_probe_frame(self):
        constants = expectation('SyntheticProbe').constants
        frame = Ia64ProbeUser.ProbeFrame(4, Ia64SyntheticProbe.parent_pfs(4), constants.user_fpsr, constants.user_rsc)
        self.assertEqual(frame.pfs, 0xC000000000000204)
        text = registers('SyntheticProbe')
        self.assertEqual(Ia64ProbeUser.probe_frame_problems(Ia64State.parse(text), frame), [])
        for old, new in (('CFM: sof=4', 'CFM: sof=5'), ('sol=4 sor=0', 'sol=3 sor=0'), ('sor=0 rrb.gr', 'sor=8 rrb.gr'),
                         ('rrb.gr=0', 'rrb.gr=1'), ('rrb.fr=0', 'rrb.fr=1'), ('rrb.pr=0', 'rrb.pr=1'),
                         ('AR.PFS=0xc000000000000204', 'AR.PFS=0xc000000000000205'),
                         ('ccv=0000000000000000', 'ccv=0000000000000001'),
                         ('unat=0000000000000000', 'unat=0000000000000001'),
                         ('fpsr=0009804c0270033f', 'fpsr=0009804c0270033e'),
                         ('rnat=0000000000000000 pfs', 'rnat=0000000000000001 pfs'),
                         ('pfs=c000000000000204', 'pfs=c000000000000205'),
                         ('rsc=000000000000000c', 'rsc=0000000000000000'),
                         ('IA64STATE CFM sof=0000000000000004', 'IA64STATE CFM sof=0000000000000000')):
            self.assertIn(old, text)
            self.assertTrue(Ia64ProbeUser.probe_frame_problems(Ia64State.parse(text.replace(old, new, 1)), frame), old)


if __name__ == '__main__':
    unittest.main()
