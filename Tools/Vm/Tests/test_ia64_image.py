# SPDX-License-Identifier: Apache-2.0
"""Image facts, the loaded-image fact line and the observation files (Ia64Image, Ia64Capture)."""
from __future__ import annotations

import json
import os
import pathlib
import tempfile
import unittest

from AsteriaVm import Build, Scenarios
from AsteriaVm.Observers import Ia64Capture, Ia64Image
from Tests.test_ia64_fixtures import FULL_CASES, directory, expectation, expectation_json, lines, observation

GEOMETRY = '  image-base=0x000000001C000000 image-size=0x0000000001000000'
FIRMWARE = '  firmware-image-base=0x0000000004000000 image-size=0x0000000001000000'
ROOT = pathlib.Path(__file__).resolve().parents[3]
TEST_BUILD = os.environ.get('ASTERIA_IA64_TEST_BUILD')


class LoadedImageTests(unittest.TestCase):
    def test_recorded(self):
        image, problems = Ia64Image.loaded_image(lines('Smp4'))
        self.assertEqual(problems, [])
        self.assertEqual((image.size, image.firmware_base), (0x1000000, 0x4000000))
        self.assertEqual(image.base % 0x1000000, 0)
        self.assertNotEqual(image.base, image.firmware_base)
        self.assertEqual(Ia64Image.loaded_image(lines('Relocated'))[0].base % 0x1000000, 0)

    def test_aligned_copy_and_firmware_relocation_are_distinct(self):
        active = '  image-base=0x0000000020000000 image-size=0x0000000001000000'
        firmware = '  firmware-image-base=0x000000001DF00000 image-size=0x0000000001000000'
        image, problems = Ia64Image.loaded_image([firmware, active, '  kernel-base=' + active.split()[0].split('=')[1]])
        self.assertEqual(problems, [])
        self.assertEqual(image.base, 0x20000000)
        self.assertEqual(image.firmware_base, 0x1DF00000)
        self.assertEqual(Ia64Image.relocation_problems(image, 0x100000000), [])
        self.assertTrue(Ia64Image.relocation_problems(image, 0x1DF00000))
        for transcript in ([active], [firmware, firmware, active],
                           [firmware.replace('image-base=0x', 'image-base=x'), active],
                           [firmware.replace('1DF00000', '20000000'), active],
                           [firmware, active.replace('20000000', '20010000')],
                           [firmware.replace('01000000', '02000000'), active]):
            self.assertIsNone(Ia64Image.loaded_image([*transcript, '  kernel-base=0x0000000020000000'])[0])

    def test_malformed_lines(self):
        # Reject malformed or duplicate image-geometry reports.
        for transcript in ([], [GEOMETRY, GEOMETRY], [GEOMETRY.replace('image-base=', 'image-base=x')],
                           [GEOMETRY.replace('0x0000000001000000', '0x0000000000000000')],
                           [GEOMETRY + ' extra'], [GEOMETRY.replace('0x000000001C000000', '0x1C000000')]):
            image, problems = Ia64Image.loaded_image([FIRMWARE, *transcript, '  kernel-base=0x000000001C000000'])
            self.assertIsNone(image, transcript)
            self.assertTrue(problems, transcript)

    def test_relocation(self):
        """Preferred-base relocation checks."""
        preferred = expectation('Relocated').preferred_base
        self.assertEqual(preferred, 0x100000000)
        self.assertEqual(Ia64Image.relocation_problems(Ia64Image.loaded_image(lines('Relocated'))[0], preferred), [])
        self.assertTrue(Ia64Image.relocation_problems(Ia64Image.LoadedImage(0x1C000000, 0x1000000, preferred), preferred))

    def test_layout(self):
        # The recorded values change with every build; the relations do not.
        facts = expectation('Relocated').facts
        image = Ia64Image.loaded_image(lines('Relocated'))[0]
        layout = Ia64Image.KernelLayout(facts, image)
        self.assertEqual(layout.delta, image.base - facts.linked_base)
        self.assertNotEqual(layout.delta, 0)    # the Relocated case really moved the image
        ivt = 'AsteriaIa64RuntimeInterruptVectorTableImage'
        self.assertEqual(layout.address(ivt), facts.kernel(ivt).value + layout.delta)
        self.assertEqual(layout.address(ivt) % 0x8000, 0)    # cr.iva needs 32 KiB alignment after relocation
        start, size = facts.kernel_sections['ReadOnlyBody']    # the extent's end is rounded up to 4 KiB
        self.assertEqual(layout.section('ReadOnlyBody'),
                         (start + layout.delta, Ia64Image.round_up(start + size, 0x1000) + layout.delta))


class FactsTests(unittest.TestCase):
    def test_json_round_trip(self):
        for name in FULL_CASES:
            recorded = expectation(name)
            # NativePreemption keeps no IVT: its facts carry the linked table's hash only.
            again = Ia64Image.ImageFacts.from_json(recorded.facts.to_json(), recorded.facts.ivt_image or None)
            self.assertEqual(again, recorded.facts, name)
            self.assertEqual(json.loads(json.dumps(recorded.to_json())), expectation_json(name), name)

    @unittest.skipUnless(TEST_BUILD, 'set ASTERIA_IA64_TEST_BUILD to check a completed IA-64 build')
    def test_the_build_tree_provides_every_fact(self):
        target = Build.TargetContext.load(pathlib.Path(TEST_BUILD).resolve())
        for firmware in (Scenarios.PRODUCTION_FIRMWARE, Scenarios.TEST_FIRMWARE):
            facts = Ia64Image.ImageFacts.from_target(target, firmware)
            self.assertEqual(set(facts.kernel_symbols), set(Ia64Image.KERNEL_SYMBOLS), firmware)
            self.assertEqual(set(facts.user_symbols), set(Ia64Image.USER_SYMBOLS), firmware)
            self.assertEqual(len(facts.ivt_image), facts.kernel('AsteriaIa64RuntimeInterruptVectorTableImage').size)
            if firmware == Scenarios.PRODUCTION_FIRMWARE:     # what the synthetic probe's expectation needs
                self.assertEqual(len(bytes.fromhex(facts.user_objects['CanonicalIpcDelivery'])), 136)


class CaptureTests(unittest.TestCase):
    def test_console_framing(self):
        self.assertEqual(Ia64Capture.lines_from_log(b'A\r\nB\n\nC\r\r\nD'), ['A', 'B', '', 'C\r', 'D'])
        self.assertEqual(Ia64Capture.lines_from_log(b'X\0Y\n\xff\n'), ['X\0Y', '�'])
        self.assertEqual(Ia64Capture.lines_from_log(b''), [])

    def test_save_and_load(self):
        recorded = observation('SyntheticProbe')
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary)
            (path / 'Debug.log').write_bytes((directory('SyntheticProbe') / 'Debug.log').read_bytes())
            Ia64Capture.save(recorded, path)
            again = Ia64Capture.load(path)
        self.assertEqual(again, recorded)


if __name__ == '__main__':
    unittest.main()
