# SPDX-License-Identifier: Apache-2.0
"""Tests of Observers.Amd64Paging: the page walker and every page-table check.

`test_h20_walk` checks the page walker with synthetic tables. The other tests walk
the page tables recorded from a real Amd64.Smp4 run, with the roots and the
pool its evidence record names, and then corrupt single entries: each
corruption must fail exactly the property it breaks.
"""
from __future__ import annotations

import struct
import unittest

from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import Amd64Paging as P
from .test_amd64_fixtures import PRODUCTION_IMAGE, USER_IMAGES, EditedMemory, constants, observation, record, snapshot

NX = P.PTE_NO_EXECUTE


def synthetic(tables: dict[int, list[int]]) -> P.PageWalker:
    pages = {address: struct.pack('<512Q', *entries) for address, entries in tables.items()}
    return P.PageWalker(P.PageCache(lambda page: pages.get(page, bytes(4096))))


class WalkTests(unittest.TestCase):
    def test_h20_walk(self):
        tables = {address: [0] * 512 for address in (0x1000, 0x2000, 0x3000, 0x4000)}
        tables[0x1000][0] = 0x2007
        tables[0x2000][0] = 0x3007
        tables[0x3000][0] = 0x4007
        tables[0x4000][1] = 0x5007 | NX
        self.assertEqual(synthetic(tables).walk(0x1000, 0x1001), P.Leaf(0x5001, True, True, False))
        self.assertIsNone(synthetic(tables).walk(0x1000, 0))
        tables[0x2000][0] &= ~4
        self.assertFalse(synthetic(tables).walk(0x1000, 0x1001).user, 'ancestor U/S bit ignored')
        tables[0x2000][0] &= ~2
        self.assertFalse(synthetic(tables).walk(0x1000, 0x1001).writable, 'ancestor R/W bit ignored')
        tables[0x3000][0] |= NX
        tables[0x4000][1] &= ~NX
        self.assertFalse(synthetic(tables).walk(0x1000, 0x1001).executable, 'ancestor NX bit ignored')

    def test_wc_scanout_pat_and_permission_mutants(self):
        tables = {address: [0] * 512 for address in (0x1000, 0x2000, 0x3000, 0x4000)}
        tables[0x1000][0], tables[0x2000][0], tables[0x3000][0] = 0x2007, 0x3007, 0x4007
        tables[0x4000][1] = 0x5087 | NX
        c = constants()
        self.assertEqual(P.scanout_problems(synthetic(tables), [0x1000], 0x1000, 1, c.expected_pat), [])
        self.assertEqual(P.memory_type(synthetic(tables).walk(0x1000, 0x1000), c.expected_pat), 1)
        for flags in (0x07, 0x1F, 0x87, 0x83, 0x85):
            tables[0x4000][1] = 0x5000 | flags | (0 if flags == 0x87 else NX)
            self.assertTrue(P.scanout_problems(synthetic(tables), [0x1000], 0x1000, 1, c.expected_pat))
        tables[0x4000][1] = 0x5087 | NX
        self.assertTrue(P.scanout_problems(synthetic(tables), [0x1000, 0x1000], 0x1000, 1, c.expected_pat))

    def test_unconstructed_reserved_root_is_not_walked(self):
        tables = {address: [0] * 512 for address in (0x1000, 0x2000, 0x3000, 0x4000)}
        tables[0x1000][0], tables[0x2000][0], tables[0x3000][0] = 0x2007, 0x3007, 0x4007
        tables[0x4000][1] = 0x5087 | NX
        tables[0x6000] = [0xAFAFAFAFAFAFAFAF] * 512
        walker = synthetic(tables)
        roots = P.candidate_roots(walker, [0x1000, 0x6000])
        self.assertEqual(roots, [0x1000])
        self.assertEqual(P.scanout_problems(walker, roots, 0x1000, 1, constants().expected_pat), [])
        tables[0x1000][0] |= P.PTE_LARGE_OR_PAT
        roots = P.candidate_roots(synthetic(tables), [0x1000, 0x6000])
        self.assertTrue(P.scanout_problems(synthetic(tables), roots, 0x1000, 1, constants().expected_pat))

    def test_large_pages_are_rejected(self):
        for level, table in ((1, 0x2000), (2, 0x3000)):
            tables = {address: [0] * 512 for address in (0x1000, 0x2000, 0x3000, 0x4000)}
            tables[0x1000][0], tables[0x2000][0], tables[0x3000][0], tables[0x4000][1] = 0x2007, 0x3007, 0x4007, 0x5007
            tables[table][1 if level == 3 else 0] |= P.PTE_LARGE_OR_PAT
            with self.subTest(level=level):
                with self.assertRaises(P.PagingError):
                    synthetic(tables).walk(0x1000, 0x1001)
                problems = P.expect_mapped(synthetic(tables), 0x1000, 0x1001, P.USER_DATA, 'page')
                self.assertIn('bit 7', problems[0])

    def test_stack_slot_guards_match_the_former_list(self):
        layout = Amd64Default.stack_layout(constants(), 0)
        self.assertEqual(layout.guards(), [0, 5, 6, 11, 12, 17, 18, 21])

    def test_user_image_segments(self):
        segments = P.user_image_segments(PRODUCTION_IMAGE)
        self.assertEqual([(s.start, s.end, s.writable, s.executable) for s in segments],
                         [(0x4000_0000, 0x4000_2000, False, True), (0x4000_2000, 0x4000_3000, False, False),
                          (0x4000_3000, 0x4000_4000, True, False)])


class RecordedTableTests(unittest.TestCase):
    """The recorded Amd64.Smp4 page tables, whole and with single corrupted entries."""

    def setUp(self):
        self.state = snapshot('Smp4')
        self.memory = EditedMemory(observation('Smp4'))
        self.walker = P.PageWalker(self.memory)
        self.private = self.state.cpus[0].cr3
        self.runtime = record('Smp4').Runtime
        self.pool = self.runtime.PoolPhysicalStart
        self.shared = self.runtime.SharedRootPhysicalAddress
        self.c = constants()
        self.segments = P.user_image_segments(USER_IMAGES['Smp4'])

    def segment(self, *, writable: bool, executable: bool) -> int:
        """The first page of the recorded image's segment with these permissions."""
        return next(segment.start for segment in self.segments
                    if (segment.writable, segment.executable) == (writable, executable))

    def problems(self) -> dict[str, list[str]]:
        walker, private, shared, c = self.walker, self.private, self.shared, self.c
        user_pages = {'page %#x' % page: page
                      for segment in self.segments for page in range(segment.start, segment.end, 4096)}
        bsp = self.state.cpus[0]
        return {
            'user-image': P.user_image_problems(walker, private, self.segments),
            'user-stack': P.user_stack_problems(walker, private, c.user_stack_start, c.user_stack_pages),
            'null-page': P.expect_unmapped(walker, private, 0, 'page zero'),
            'descriptor-tables': P.descriptor_table_problems(
                walker, private, {'GDT': bsp.segments['gdt'].base, 'IDT': bsp.segments['idt'].base}),
            'root-separation': P.root_separation_problems(walker, shared, user_pages),
            'shootdown-probe': P.shootdown_probe_problems(walker, private, shared, c.shared_probe_address,
                                                          c.shared_probe_value),
            'mmio-window': P.mmio_window_problems(walker, {'private': private, 'shared': shared},
                                                  c.mmio_window_address, self.runtime.LocalApicPhysicalAddress),
            'kernel-stacks': P.kernel_stack_problems(
                walker, private, Amd64Default.stack_layout(c, self.pool), c.stack_slots),
        }

    def assertFailing(self, *checks: str) -> str:
        """Exactly `checks` fail; returns their problems as text."""
        problems = self.problems()
        self.assertEqual({name for name, found in problems.items() if found}, set(checks), problems)
        return '\n'.join(problem for name in checks for problem in problems[name])

    def edit(self, address: int, change, level: int = 3, root: int | None = None):
        root = self.private if root is None else root
        self.memory.set_entry(root, address, change(self.memory.entry(root, address, level)), level)

    def test_recorded_tables_pass(self):
        self.assertEqual({name: problems for name, problems in self.problems().items() if problems}, {})

    def test_user_data_must_not_be_executable(self):
        self.edit(self.segment(writable=True, executable=False), lambda entry: entry & ~NX)
        self.assertIn('RWX user', self.assertFailing('user-image'))

    def test_user_text_must_not_be_writable(self):
        self.edit(self.segment(writable=False, executable=True), lambda entry: entry | P.PTE_WRITABLE)
        self.assertFailing('user-image')

    def test_user_read_only_data_must_not_be_executable(self):
        self.edit(self.segment(writable=False, executable=False), lambda entry: entry & ~NX)
        self.assertFailing('user-image')

    def test_stack_guard_must_stay_unmapped(self):
        stack = self.memory.entry(self.private, self.c.user_stack_start)
        self.edit(self.c.user_stack_start - 4096, lambda entry: stack)
        self.assertIn('guard page below', self.assertFailing('user-stack'))

    def test_stack_must_be_user_memory(self):
        self.edit(self.c.user_stack_start, lambda entry: entry & ~P.PTE_USER, level=2)  # an ancestor
        self.assertFailing('user-stack')

    def test_page_zero_must_stay_unmapped(self):
        self.edit(0, lambda entry: 0x1000 | P.PTE_PRESENT | P.PTE_WRITABLE)
        self.assertFailing('null-page')

    def test_descriptor_tables_must_not_be_executable(self):
        self.edit(self.state.cpus[0].segments['gdt'].base, lambda entry: entry & ~NX)
        self.assertFailing('descriptor-tables')

    def test_user_image_must_stay_out_of_the_shared_root(self):
        user_pde = self.memory.entry(self.private, 0x4000_0000, level=2)
        self.memory.set_entry(self.shared, 0x4000_0000, user_pde, level=2)  # the shared root's PD exists (probe page)
        self.assertIn('0x40000000) is mapped R-X', self.assertFailing('root-separation'))

    def test_shootdown_probe_must_stay_out_of_the_private_root(self):
        user_pde = self.memory.entry(self.private, 0x4000_0000, level=2)
        self.memory.set_entry(self.private, self.c.shared_probe_address, user_pde, level=2)
        self.assertIn('private root', self.assertFailing('shootdown-probe'))

    def test_shootdown_probe_must_hold_the_published_value(self):
        leaf = self.walker.walk(self.shared, self.c.shared_probe_address)
        self.memory.write(leaf.physical, struct.pack('<Q', self.c.shared_probe_value ^ 1))
        self.assertIn('holds', self.assertFailing('shootdown-probe'))

    def test_mmio_window_must_be_restored(self):
        self.edit(self.c.mmio_window_address, lambda entry: (entry & ~P.PTE_ADDRESS) | 0xFED0_0000, root=self.shared)
        self.assertIn('shared root', self.assertFailing('mmio-window'))

    def test_kernel_stack_guard_must_stay_unmapped(self):
        base = self.c.stack_extent(self.pool, 2)
        stack = self.memory.entry(self.private, base + 4096)
        self.edit(base + 6 * 4096, lambda entry: stack)
        self.assertIn('CPU 2 kernel stack guard page 6', self.assertFailing('kernel-stacks'))

    def test_kernel_stack_must_not_be_executable(self):
        self.edit(self.c.stack_extent(self.pool, 3) + 22 * 4096, lambda entry: entry & ~NX)
        self.assertIn('CPU 3 kernel stack page 22', self.assertFailing('kernel-stacks'))


if __name__ == '__main__':
    unittest.main()
