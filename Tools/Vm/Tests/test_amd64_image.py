# SPDX-License-Identifier: Apache-2.0
"""Tests of Observers.Amd64Image: the loaded-image fact line and the relocation check."""
from __future__ import annotations

import unittest

from AsteriaVm.Observers import Amd64Image as I
from .test_amd64_fixtures import debug_lines

RELOCATED_PREFERRED_BASE = 0x1_0000_0000   # the amd64-*-relocated presets' image_base
DEFAULT_BASE = 0x0400_0000


def geometry(lines: list[str]) -> str:
    return next(line for line in lines if line.startswith('  image-base='))


class LoadedImageTests(unittest.TestCase):
    def test_recorded_geometry(self):
        image, problems = I.loaded_image(debug_lines('Smp4'))
        self.assertEqual(problems, [])
        self.assertEqual(image.base % 0x200000, 0)   # the loader allocates an independent aligned kernel
        self.assertNotEqual(image.size, 0)

    def test_relocated_run_passes_and_an_unrelocated_one_fails(self):
        self.assertEqual(I.relocation_problems(debug_lines('PreemptionRelocated'), RELOCATED_PREFERRED_BASE), [])
        self.assertTrue(I.relocation_problems(debug_lines('Smp4'), DEFAULT_BASE))

    def test_kernel_base_is_required_and_matches(self):
        lines = debug_lines('Smp4')
        kernel = next(line for line in lines if line.startswith('  kernel-base='))
        for replacement in ([], [kernel, kernel], [kernel + ' extra'], ['  kernel-base=0x0000000000000000']):
            self.assertIsNone(I.loaded_image([line for line in lines if line != kernel] + replacement)[0])

    def test_malformed_geometry_is_rejected(self):
        lines = debug_lines('PreemptionRelocated')
        line = geometry(lines)
        base = line.split()[0][len('image-base=0x'):]
        for mutant in (line + ' extra=1', line.replace('image-size=0x', 'image-size=0X'),
                       line.replace('image-base=0x' + base, 'image-base=0x' + base[1:]),
                       line.replace('image-size=0x', 'image-length=0x')):
            with self.subTest(mutant=mutant):
                self.assertTrue(I.relocation_problems([mutant if other == line else other for other in lines],
                                                      RELOCATED_PREFERRED_BASE))
        self.assertTrue(I.relocation_problems(lines + [line], RELOCATED_PREFERRED_BASE))       # two lines
        self.assertTrue(I.relocation_problems([other for other in lines if other != line], RELOCATED_PREFERRED_BASE))
        zero = '  image-base=0x%s image-size=0x%016X' % (base, 0)
        self.assertIn('size is zero', I.relocation_problems(
            [zero if other == line else other for other in lines], RELOCATED_PREFERRED_BASE)[0])


if __name__ == '__main__':
    unittest.main()
