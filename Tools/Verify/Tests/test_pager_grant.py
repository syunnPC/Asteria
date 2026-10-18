# SPDX-License-Identifier: Apache-2.0
"""The grant gate detects shared/private image growth on both ISAs."""
import importlib.util
import pathlib
import struct
import unittest

path = pathlib.Path(__file__).resolve().parents[2] / 'Build' / 'CheckPagerGrant.py'
spec = importlib.util.spec_from_file_location('CheckPagerGrant', path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class PagerGrantTests(unittest.TestCase):
    def test_pattern_sizes_follow_the_single_output_page_constant(self):
        header = (path.parents[2] / 'Source/Abi/ComponentBootstrap.hpp').read_text()
        output = checker.constant(header, 'ComponentCompositorPages')
        for pages, factor in (('ComponentPatternPages', 2), ('ComponentPatternSecondaryPages', 2),
                              ('ComponentPatternInputPages', 4)):
            self.assertEqual(checker.constant(header, pages), output * factor)
            changed = header.replace('ComponentCompositorPages{%dU}' % output,
                                     'ComponentCompositorPages{%dU}' % (output + 1))
            self.assertEqual(checker.constant(changed, pages), (output + 1) * factor)
        with self.assertRaisesRegex(ValueError, 'recursive constant'):
            checker.constant('constexpr int A{B}; constexpr int B{A};', 'A')

    def test_peaks_and_growth(self):
        for isa, images, stack, pages, pairs in (
            ('Amd64', [(19, 11, 2, 1), (6, 3, 2, 1), (4, 1, 1, 1), (3, 1, 2, 1)], 3, 456, 27),
            ('Ia64', [(12, 5, 2, 1), (19, 3, 2, 1), (13, 1, 2, 1), (7, 1, 2, 1)], 9, 107, 18),
        ):
            with self.subTest(isa=isa):
                def peak(records):
                    return checker.peaks(records, stack, 2, 2, 13, 4, isa)
                checker.require(peak(images), pages, pairs)
                self.assertEqual(max(value[0] for value in peak(images).values()), pages)
                self.assertEqual(max(value[1] for value in peak(images).values()), pairs)
                for field in range(4):
                    growth = list(images)
                    component = list(growth[0])
                    component[field] += 1
                    growth[0] = tuple(component)
                    with self.assertRaisesRegex(ValueError, 'grant allows'):
                        checker.require(peak(growth), pages, pairs)
                checker.require(peak(images), pages + 4, pairs + 1)

    def test_usb_host_scenario_charges_the_usb_image_only_there(self):
        images = [(19, 11, 2, 1), (6, 3, 2, 1), (4, 1, 1, 1), (3, 1, 2, 1)]
        usb = (11, 22, 2, 1)
        without = checker.peaks(images, 9, 2, 2, 13, 4, 'Ia64')
        scenarios = checker.peaks(images + [usb], 9, 2, 2, 13, 4, 'Ia64', usb=(8, 4))
        self.assertNotIn('USB host HID', without)
        # Component and usb images retained (19 + 11 pages, 2 + 2 pairs);
        # display, broker and PS/2 private (11 pages and a 9-page stack, 1 + 1
        # pairs each) and the driver's (22 + 9, 2); its eight DMA pages, one
        # DMA pair, four register pairs and the scanout pair.
        self.assertEqual(scenarios['USB host HID'],
                         (19 + 11 + 3 * (11 + 9) + (22 + 9) + 8, 2 + 2 + 3 * 2 + 2 + 1 + 4 + 1))
        self.assertEqual({name: value for name, value in scenarios.items()
                          if name not in ('USB host HID', 'power USB flush')}, without)
        self.assertEqual(scenarios['power USB flush'],
                         (19 + 11 + 4 * (11 + 9) + (22 + 9) + 10,
                          2 + 2 + 4 * 2 + 2 + 1 + 4 + 4 + 1))
        with self.assertRaisesRegex(ValueError, 'power USB flush'):
            checker.require({'power USB flush': scenarios['power USB flush']}, 256, 22)
        # The same flush overlap retains the low bounce run and the
        # driver's client-copy MemoryObject until DMA is quiet.
        bounce = checker.peaks(images + [usb], 9, 2, 2, 13, 4, 'Ia64', usb=(8, 4), ahci_bounce=16)
        for name in ('power PS/2 flush', 'power USB flush'):
            self.assertEqual(bounce[name], (scenarios[name][0] + 16, scenarios[name][1] + 2))

    def test_five_components_beside_one_xhci(self):
        images = [(19, 11, 2, 1), (6, 3, 2, 1), (4, 1, 1, 1), (3, 1, 2, 1)]
        usb = (11, 22, 2, 1)
        without = checker.peaks(images, 3, 2, 2, 13, 4, 'Amd64')
        scenarios = checker.peaks(images + [usb], 3, 2, 2, 13, 4, 'Amd64', xhci=(68, 57, 256, 128), usb=(8, 4))
        self.assertNotIn('five components (128 scratchpads)', without)
        self.assertNotIn('five components (128 scratchpads)', checker.peaks(images + [usb], 9, 2, 2, 13, 4, 'Ia64'))
        # Component and usb images retained (19 + 11 pages, 2 + 2 pairs);
        # display, broker, PS/2 and xHCI private (11 pages and a 3-page stack,
        # 1 + 1 pairs each), the group's (22 + 3, 2); xHCI's 68 DMA, 57 state
        # and 128 scratchpad pages in two state pairs, seven DMA pairs and a
        # register pair; the group's eight DMA pages, one DMA and four
        # register pairs; the scanout pair.
        self.assertEqual(scenarios['five components (128 scratchpads)'],
                         (19 + 11 + 4 * (11 + 3) + (22 + 3) + 68 + 57 + 128 + 8,
                          2 + 2 + 4 * 2 + 2 + (2 + 7 + 1) + (1 + 4) + 1))
        with self.assertRaisesRegex(ValueError, 'five components'):
            checker.require({'five components': scenarios['five components (128 scratchpads)']}, 512, 29)

    def test_display_is_retained_once_and_has_only_one_console_grid(self):
        images = [(31, 14, 2, 1), (6, 3, 2, 1), (4, 1, 1, 1), (5, 1, 2, 1)]
        display = (13, 22, 2, 1)
        scenarios = checker.peaks(images, 3, 2, 2, 13, 4, 'Amd64', display=display, probe=(4, 1, 2, 1))
        # Display + input broker + probe + PS/2; the grid rides in display only.
        self.assertEqual(scenarios['input'], (sum(r[0] for r in images) + 13 + 25 + 3 * 17, 9 + 8 + 1))
        self.assertEqual(scenarios['session probe'], (31 + 4 + 13 + 25 + 4 + 63, 6 + 4 + 1 + 2))
        checker.require(scenarios, 512, 32)
        with self.assertRaisesRegex(ValueError, 'grant allows'):
            checker.require(checker.peaks(images, 3, 2, 2, 13, 4, 'Amd64', display=(13, 40, 2, 1)), 512, 32)

    def test_ports_phase_and_objects(self):
        images = [(19, 11, 2, 1), (6, 3, 2, 1), (4, 1, 1, 1), (3, 1, 2, 1)]
        scenarios = checker.peaks(images, 3, 2, 2, 13, 4, 'Amd64')
        self.assertEqual(scenarios['ports'], (33, 5))
        self.assertNotIn('ports', checker.peaks(images, 9, 2, 2, 13, 4, 'Ia64'))
        checker.require_ports(2, 1)
        for waitsets, interrupts in ((1, 1), (2, 0)):
            with self.assertRaisesRegex(ValueError, 'ports needs'):
                checker.require_ports(waitsets, interrupts)

    def test_page_rounding_and_private_segments(self):
        data = bytearray(256)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<Q', data, 32, 64)
        struct.pack_into('<HH', data, 54, 56, 2)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0x1001, 0, 4096, 4096, 4096)
        struct.pack_into('<IIQQQQQQ', data, 120, 1, 6, 0, 0x3000, 0, 0, 4096, 4096)
        self.assertEqual(checker.layout(data), (2, 1, 1, 1))

    def test_edu_dma_and_quarantine_storage(self):
        images = [(24, 9, 2, 1), (19, 3, 2, 1), (13, 1, 2, 1), (12, 1, 2, 1)]
        scenarios = checker.peaks(images, 9, 2, 2, 13, 4, 'Ia64')
        # DMA confinement owns two additional pages and pairs besides the
        # register Frame. Drain parks those pins until the device is quiet.
        self.assertEqual(scenarios['edu DMA'], (62, 10))
        self.assertEqual(scenarios['edu parked pins'], (44, 8))
        self.assertEqual(scenarios['input'], (140, 17))
        # The block and filesystem services run on IA-64 too : broker, driver and service with the
        # bounce page.
        self.assertEqual(scenarios['filesystem'], (79, 12))
        # So does the network demonstration (Source/User/Network/NetworkStack.hpp, IA-64
        # ): broker, NIC driver and stack, the driver's 13 DMA
        # pages and the stack's 4 window pages, with five pairs and four.
        self.assertEqual(scenarios['network'], (95, 18))
        checker.require(scenarios, 144, 18)
        with self.assertRaisesRegex(ValueError, 'input'):
            checker.require(scenarios, 139, 17)
        with self.assertRaisesRegex(ValueError, 'edu DMA'):
            checker.require({'edu DMA': scenarios['edu DMA']}, 61, 10)
        with self.assertRaisesRegex(ValueError, 'edu parked pins'):
            checker.require({'edu parked pins': scenarios['edu parked pins']}, 44, 7)


if __name__ == '__main__':
    unittest.main()
