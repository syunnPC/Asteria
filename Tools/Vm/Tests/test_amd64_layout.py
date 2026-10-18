# SPDX-License-Identifier: Apache-2.0
"""The AMD64 layout facts (AsteriaVm.Amd64Layout).

Every value the kernel chooses is read from the tree's C++ sources; a
declaration that disappears or is no longer an integer expression is an
error, not a guess.
"""
from __future__ import annotations

import pathlib
import tempfile
import unittest

from AsteriaVm import Amd64Layout, ObjectLimits
from AsteriaVm.Amd64Layout import LayoutError

ROOT = pathlib.Path(__file__).resolve().parents[3]


class HeaderTests(unittest.TestCase):
    def test_object_capacity_lookup_is_independent_of_cpp_line_wrapping(self):
        declaration = '''static constexpr Foundation::Size Capacity{
    Abi::ObjectPoolCapacities[
        static_cast<Foundation::Size>(Abi::LimitedObject::AddressSpace)]};'''
        self.assertEqual(ObjectLimits.capacity(ROOT, declaration, 'Capacity'), 16)
        self.assertIsNone(ObjectLimits.capacity(ROOT, declaration, 'OtherCapacity'))

    def test_every_constant_is_read_from_the_tree(self):
        c = Amd64Layout.headers(ROOT)
        self.assertEqual((c.user_code_selector, c.user_data_selector, c.kernel_code_selector,
                          c.kernel_data_selector), (0x23, 0x1B, 0x08, 0x10))
        self.assertEqual((c.descriptor_tables_size, c.idt_offset, c.entry_context_size), (8192, 4096, 32))
        # IF and bit 1; CF, PF, AF, ZF, SF, DF, OF, NT, AC and ID pass through a system call.
        self.assertEqual((c.user_rflags_required, c.user_rflags_permitted),
                         (0x202, 0x202 | sum(1 << bit for bit in (0, 2, 4, 6, 7, 10, 11, 14, 18, 21))))
        # PE|WP|PG from the activation, MP|NE from the entry install, which clears EM, TS and AM.
        self.assertEqual((c.cr0_required, c.cr0_forbidden), (0x8001_0023, (1 << 18) | (1 << 2) | (1 << 3)))
        # The entry install gives every CPU exactly PAE, MCE, OSFXSR and OSXMMEXCPT.
        self.assertEqual(c.cr4_installed, (1 << 5) | (1 << 6) | (1 << 9) | (1 << 10))
        self.assertEqual((c.timer_vector, c.timer_masked_lvt), (0xE8, 0x1_00E8))
        self.assertEqual((c.shared_probe_address, c.shared_probe_value), (0x6000_0000, 0x4153_5445_544C_4234))
        self.assertEqual(c.wait_tag, 0x5741_4954)
        self.assertEqual(c.preemption_sentinels, (0x101, 0x102, 0x103, 0x104, 0x105) + tuple(range(0x108, 0x110)))
        self.assertEqual((c.drain_stop_empty, c.notification_ready, c.deadline_disarmed, c.ahci_ready,
                          c.system_call_error_none), (0, 1, 0, 4, 0))

    def test_the_pool_layout(self):
        c = Amd64Layout.headers(ROOT)
        pool = 0x1000_0000
        # The pool includes the root and per-CPU arenas.
        self.assertEqual((c.pool_pages, c.root_arena_pages, c.stack_slots), (3529, 64, 4))
        self.assertEqual([c.idle_root(pool, cpu) for cpu in range(4)],
                         [pool, pool + 3337 * 4096, pool + 3401 * 4096, pool + 3465 * 4096])
        self.assertEqual(c.root(pool, c.user_root_index) - c.root(pool, c.shared_root_index), 64 * 4096)
        self.assertEqual(c.stacks, ((1, 4), (7, 4), (13, 4), (19, 2), (22, 2)))
        # Four root arenas (shared, initial image, pager, spinner) precede the stacks.
        self.assertEqual(c.stack_extent(pool, 2), pool + (256 + 48) * 4096)
        self.assertEqual(c.idle_stack_range(pool, 1), (pool + (256 + 24 + 1) * 4096, pool + (256 + 24 + 5) * 4096))
        self.assertEqual(c.entry_stack_top(pool, 3), pool + (256 + 72 + 17) * 4096)
        self.assertEqual((c.pool_page(pool, c.shared_probe_page), c.pool_page(pool, c.ahci_read_data_page)),
                         (pool + 352 * 4096, pool + 355 * 4096))
        # The boot pager's lazy region lies between the initial image and its stack.
        self.assertEqual((c.lazy_start, c.lazy_pages), (0x4800_0000, 2))

    def test_values_fall_back_to_pins_and_refuse_what_the_sources_do_not_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'X.hpp').write_text('inline constexpr Foundation::UInt64 A{0x1\'0000U};\n'
                                        'inline constexpr Foundation::UInt32 B{A | 0xE8U};\n'
                                        'constexpr Foundation::UInt64 C{\n    (A\n        | 1U << 4U)};\n'
                                        'constexpr Foundation::UInt64 D{Unknown(A)};\n'
                                        'static_assert(sizeof(T) == 32U);\n'
                                        'enum class E : Foundation::UInt8\n{\n    First,\n    Third = 3,\n'
                                        '    Fourth,\n};\n')
            sources = Amd64Layout._Sources(root)
            self.assertEqual(sources.value('X.hpp', 'B'), 0x1_00E8)
            self.assertEqual(sources.value('X.hpp', 'C'), 0x1_0010)
            self.assertEqual(sources.value('X.hpp', 'sizeof(T)'), 32)
            self.assertEqual(sources.enumerator('X.hpp', 'E', 'Fourth'), 4)
            with self.assertRaises(LayoutError):
                sources.value('X.hpp', 'D')
            with self.assertRaises(LayoutError):
                sources.enumerator('X.hpp', 'E', 'Second')


if __name__ == '__main__':
    unittest.main()
