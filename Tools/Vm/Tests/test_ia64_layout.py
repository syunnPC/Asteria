# SPDX-License-Identifier: Apache-2.0
"""The IA-64 layout facts (AsteriaVm.Ia64Layout).

Every value the kernel chooses is read from the tree's C++ sources; a
declaration that disappears or is no longer an integer expression is an
error, not a guess.
"""
from __future__ import annotations

import pathlib
import tempfile
import unittest

from AsteriaVm import Ia64Layout
from AsteriaVm.Ia64Layout import LayoutError
from AsteriaVm.Observers import Ia64Evidence

ROOT = pathlib.Path(__file__).resolve().parents[3]


class HeaderTests(unittest.TestCase):
    def test_every_constant_is_read_from_the_tree(self):
        constants = Ia64Layout.headers(ROOT)
        self.assertEqual(constants.user_psr, 0x0000101308426000)
        self.assertEqual(constants.probe_user_psr, 0x00001013084E2000)
        self.assertEqual(constants.probe_stack_page, 0x2000000002004000)
        self.assertEqual(constants.probe_initial_stack_pointer, 0x2000000002004FF0)
        self.assertEqual(constants.bootstrap_ivt, (16, 8))
        self.assertEqual(constants.user_rid(1), 0x2011)
        self.assertEqual(constants.ivt_page_faults, (0x5000, 0x5100, 0x5200, 0x5300))
        self.assertEqual((constants.probe_endpoint_rights, constants.delivery_rights_offset), (6, 14))
        self.assertEqual((constants.timer_vector, constants.ap_task_priority), (0xE8, 0xD0))
        self.assertEqual((constants.deadline_disarmed, constants.deadline_armed, constants.timer_expired),
                         (0, 1, 3))

    def test_values_fall_back_to_pins_and_refuse_what_the_sources_do_not_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / 'X.hpp').write_text('constexpr Foundation::UInt64 A{0x0000\'1000ULL};\n'
                                        'constexpr Foundation::UInt64 B{Unknown(A)};\n'
                                        'inline constexpr Foundation::UInt64 C{~Foundation::UInt64{0U}};\n'
                                        'static_assert(B == 0x1001U);\n'
                                        'static_assert(Rights.GetRawValue() == 6U);\n'
                                        'enum class E : Foundation::UInt8\n{\n    First,\n    Third = 3,\n'
                                        '    Fourth,\n};\n')
            sources = Ia64Layout._Sources(root)
            self.assertEqual(sources.value('X.hpp', 'A'), 0x1000)
            self.assertEqual(sources.value('X.hpp', 'B'), 0x1001)
            self.assertEqual(sources.value('X.hpp', 'C'), (1 << 64) - 1)
            self.assertEqual(sources.value('X.hpp', 'Rights.GetRawValue()'), 6)
            self.assertEqual(sources.enumerator('X.hpp', 'E', 'Fourth'), 4)
            with self.assertRaises(LayoutError):
                sources.value('X.hpp', 'D')
            with self.assertRaises(LayoutError):
                sources.enumerator('X.hpp', 'E', 'Second')


class TranslationWordTests(unittest.TestCase):
    def test_the_encoders_flags_form_the_architected_words(self):
        constants = Ia64Layout.headers(ROOT)
        for flags, rights, level in ((constants.pte_kernel_rx, 1, 0), (constants.pte_kernel_r, 0, 0),
                                     (constants.pte_kernel_rw, 2, 0), (constants.pte_user_rx, 1, 3),
                                     (constants.pte_user_rw, 2, 3)):
            # AR (bits 9-11), PL (bits 7-8), P|A (bits 0 and 5), D (bit 6) exactly when writable.
            self.assertEqual(((flags >> 9) & 7, (flags >> 7) & 3, flags & 0x21), (rights, level, 0x21))
            self.assertEqual(bool(flags & 0x40), rights == 2)
        self.assertEqual(constants.pte_exception_deferral, 1 << 52)

    def test_bootstrap_data_fits_the_runtime_page(self):
        constants = Ia64Layout.headers(ROOT)
        runtime_page = Ia64Layout.runtime_memory_page(ROOT)
        for offset, count in (constants.bootstrap_stack, constants.bootstrap_storage, constants.bootstrap_rse):
            self.assertLessEqual((offset + count) * constants.page_size, runtime_page)
        self.assertIn(runtime_page, Ia64Layout.INSERTABLE_PAGE_SIZES)

    def test_the_parked_timer_value(self):
        self.assertEqual(Ia64Evidence.PARKED_TIMER_MATCH, 0xFFFFFFFFFFFFFFFE)


if __name__ == '__main__':
    unittest.main()
