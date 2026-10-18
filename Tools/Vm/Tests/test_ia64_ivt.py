# SPDX-License-Identifier: Apache-2.0
"""Live IVT observers (Observers.Ia64Ivt) on the recorded tables and routing and table-content mutants."""
from __future__ import annotations

import unittest

from AsteriaVm import Ia64Layout
from AsteriaVm.Drivers import Ia64Default
from AsteriaVm.Observers import Ia64Image, Ia64Ivt, Ia64State
from Tests.test_ia64_fixtures import directory, expectation, lines, registers

# Bundles the IA-64 assembler produced (checked against objdump -m ia64): nop.m 0 / adds r16=1,r0 /
# br.sptk.many +0x50 (adds prints as `adds`, not `mov`) nop.m 0 / addl r16=2,r0 (`mov r16=2`) /
# br.spnt.many +0x40 (p1) nop.m 0 / nop.i 1 / br.few +0x20 nop.m 0 / nop.i 0 / br.many.clr +0x10;
# nop.m 0 / nop.i 0 / (p2) br.sptk.many -0x0;
SAMPLES = [bytes.fromhex(text) for text in (
    '10000000010000090000420058000040', '10000000010000110000480048000041',
    '30000000010010000002000020000040', '11000000010000000002000018000044',
    '11000000010000000002000108000040')]


class DecoderTests(unittest.TestCase):
    def test_fields(self):
        self.assertEqual(Ia64Ivt.decode(Ia64Layout.IVT_FAIL_STOP_BUNDLE), (0x11, (1 << 27, 1 << 27, 0x8000000000)))
        results = [(Ia64Ivt.decode(bundle)[0], Ia64Ivt.addl_immediate(Ia64Ivt.decode(bundle)[1][1], 16),
                    Ia64Ivt.branch_target(Ia64Ivt.decode(bundle)[1][2], 0x1000)) for bundle in SAMPLES]
        self.assertEqual(results, [(0x10, None, 0x1050), (0x10, 2, None), (0x10, None, None), (0x11, None, None),
                                   (0x11, None, None)])
        # The fail-stop bundle branches to itself with the few hint: not a trampoline branch.
        self.assertIsNone(Ia64Ivt.branch_target(Ia64Ivt.decode(Ia64Layout.IVT_FAIL_STOP_BUNDLE)[1][2], 0x1000))

    def test_move_to_kernel_register(self):
        # `mov.m ar.k4=84`, `=92` and `=93` as GAS assembles the exception
        # trampolines; nop.m 0 is no such move.
        self.assertEqual(Ia64Ivt.move_to_application_register(0x1404a8000, 4), 84)
        self.assertEqual(Ia64Ivt.move_to_application_register(0x1404b8000, 4), 92)
        self.assertEqual(Ia64Ivt.move_to_application_register(0x1404ba000, 4), 93)
        self.assertIsNone(Ia64Ivt.move_to_application_register(0x1404a8000, 5))
        self.assertIsNone(Ia64Ivt.move_to_application_register(0x8000000, 4))

    def test_terminal_vector_captures_ip_and_has_exact_target(self):
        memory = 1 << 27
        integer = 0x180000400  # Mov r16=ip, as assembled by GAS
        branch = 4 << 37 | 6 << 13 | 1 << 12
        value = 0x11 | memory << 5 | integer << 46 | branch << 87
        bundle = value.to_bytes(16, 'little')
        route = Ia64Ivt.Route('terminal', 0x400, 0x1460, capture_ip=True)
        self.assertEqual(Ia64Ivt.route_problems(bundle, 0x1400, route), [])
        wrong_register = (value ^ (1 << (46 + 6))).to_bytes(16, 'little')
        self.assertTrue(Ia64Ivt.route_problems(wrong_register, 0x1400, route))
        wrong_target = (value ^ (1 << (87 + 13))).to_bytes(16, 'little')
        self.assertTrue(Ia64Ivt.route_problems(wrong_target, 0x1400, route))

    def test_negative_displacement(self):
        slot = 4 << 37 | 1 << 36 | 0xFFFFF << 13 | 1 << 12   # Br.sptk.many -0x10
        self.assertEqual(Ia64Ivt.branch_target(slot, 0x4000), 0x3FF0)


class LiveTableTests(unittest.TestCase):
    def setUp(self):
        self.live = (directory('Smp4') / 'InterruptVectorTable.bin').read_bytes()
        self.expected = expectation('Smp4')
        loaded, _ = Ia64Image.loaded_image(lines('Smp4'))
        self.kernel = Ia64Image.KernelLayout(self.expected.facts, loaded)
        self.table = self.kernel.address('AsteriaIa64RuntimeInterruptVectorTableImage')
        self.routes = Ia64Default.ivt_routes(self.expected, self.kernel)
        constants = self.expected.constants
        self.handlers = (constants.ivt_alternate_itlb, constants.ivt_alternate_dtlb)

    def routing(self, table: bytes, routes=None) -> list[str]:
        return Ia64Ivt.vector_routing_problems(table, self.table, fail_stop=Ia64Layout.IVT_FAIL_STOP_BUNDLE,
                                               handlers=self.handlers, routes=routes or self.routes)

    def patched(self, offset: int, data: bytes) -> bytes:
        return self.live[:offset] + data + self.live[offset + len(data):]

    def flipped(self, offset: int) -> bytes:
        return self.patched(offset, bytes([self.live[offset] ^ 1]))

    def test_recorded(self):
        self.assertEqual(Ia64Ivt.live_image_problems(self.live, self.expected.facts.ivt_image), [])
        self.assertEqual(self.routing(self.live), [])
        self.assertEqual(Ia64Ivt.installed_address_problems(Ia64State.parse(registers('Smp4')), self.table), [])
        self.assertTrue(Ia64Ivt.installed_address_problems(Ia64State.parse(registers('Smp4')), self.table + 0x8000))

    def test_s2_mutants(self):
        refill = self.live[192 * 16:193 * 16]
        mutants = {'non-branch break opcode': self.patched(0x2C00, refill),
                   'wrong break target': self.flipped(0x2C00 + 13),
                   'non-fail-stop break padding': self.patched(0x2C10, b'\x10'),
                   'non-branch external opcode': self.patched(0x3000, refill),
                   'wrong external target': self.flipped(0x3000 + 13),
                   'non-fail-stop external padding': self.patched(0x3010, b'\x10'),
                   'fail-stop refill handler': self.patched(0x0C00, Ia64Layout.IVT_FAIL_STOP_BUNDLE),
                   'unimplemented vector': self.patched(0x0400, refill),
                   'truncated': self.live[:-1]}
        for selector in range(4):
            offset = 0x5000 + selector * 0x100
            mutants['wrong page-fault %d target' % selector] = self.flipped(offset + 13)
            mutants['non-fail-stop page-fault %d padding' % selector] = self.patched(offset + 16, b'\x10')
            mutants['page-fault %d selector' % selector] = self.patched(offset, self.live[
                0x5000 + (selector + 1) % 4 * 0x100:0x5000 + (selector + 1) % 4 * 0x100 + 16])
        for name, table in mutants.items():
            self.assertTrue(self.routing(table), name)
            self.assertTrue(Ia64Ivt.live_image_problems(table, self.expected.facts.ivt_image), name)
        # The wrong-target mutants keep a direct branch: only its target is wrong.
        target = Ia64Ivt.branch_target(Ia64Ivt.decode(self.flipped(0x2C00 + 13)[0x2C00:0x2C10])[1][2],
                                       self.table + 0x2C00)
        self.assertNotIn(target, (None, self.kernel.address(Ia64Layout.IVT_SYSTEM_CALL_ENTRY)))

    def test_the_legacy_external_entry_is_rejected(self):
        entry = self.kernel.address(Ia64Layout.IVT_LEGACY_EXTERNAL_ENTRY)
        legacy = tuple(Ia64Ivt.Route(route.name, route.offset, entry)
                       if route.offset == self.expected.constants.ivt_external else route for route in self.routes)
        self.assertTrue(self.routing(self.live, legacy))

    def test_routing_is_relocation_invariant(self):
        # The same table at another base routes to equally shifted entries (IP-relative branches).
        relocated = expectation('Relocated')
        self.assertEqual(relocated.facts.ivt_image, self.live)
        loaded, _ = Ia64Image.loaded_image(lines('Relocated'))
        kernel = Ia64Image.KernelLayout(relocated.facts, loaded)
        self.assertEqual(Ia64Ivt.vector_routing_problems(
            self.live, kernel.address('AsteriaIa64RuntimeInterruptVectorTableImage'),
            fail_stop=Ia64Layout.IVT_FAIL_STOP_BUNDLE, handlers=self.handlers,
            routes=Ia64Default.ivt_routes(relocated, kernel)), [])


if __name__ == '__main__':
    unittest.main()
