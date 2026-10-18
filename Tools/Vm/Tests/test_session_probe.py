# SPDX-License-Identifier: Apache-2.0
"""Sub-screen session pixel oracle and root-only test-media activation."""
import pathlib
import unittest
from types import SimpleNamespace
from unittest import mock
from AsteriaVm import Display, Scenarios, Session
from AsteriaVm.Drivers import SessionCompositor, SessionProbe

ROOT = pathlib.Path(__file__).resolve().parents[3]


class SessionProbeTests(unittest.TestCase):
    def test_retained_image_counts_include_the_four_processor_worker_cache(self):
        counts = {'display': 2, 'compositor': 1, 'firmware-test:smp_elf': 2}
        target = SimpleNamespace(artifact=lambda key: key)
        with mock.patch.object(SessionProbe.SharedImages, 'layout', side_effect=lambda key: ((counts[key],),)):
            for isa, cpus, expected in (('Amd64', 4, 5), ('Amd64', 2, 3), ('Ia64', 4, 3)):
                context = SimpleNamespace(case=SimpleNamespace(isa=isa, cpus=cpus), target=target)
                self.assertEqual(SessionProbe._retained_pairs(context, ('display', 'compositor')), expected)

    def test_pattern_cursor_and_uncovered_background(self):
        width, height = 640, 400
        for serial in (1, 2):
            rgb = Session.probe_output(ROOT, width, height, serial)
            def pixel(x, y):
                at = (y * width + x) * 3
                return tuple(rgb[at:at + 3])
            self.assertEqual(pixel(0, 0), (0x30, 0x68, 0x68))
            self.assertEqual(pixel(320, 200), (0x10, 0x20, 0x40))
            self.assertEqual(pixel(48, 32), (serial * 17, serial * 11, serial * 23))
            cursor = Display.draw_cursor(bytes(len(rgb)), width, height, (96, 72), ROOT)
            self.assertTrue(any(cursor))
            self.assertEqual(len(rgb), width * height * 3)

    def test_both_session_suites_use_root_started_test_firmware(self):
        for isa in ('Amd64', 'Ia64'):
            cases = [case for case in Scenarios.select([isa + 'Session'], []) if case.guest]
            self.assertEqual(len(cases), 10)
            self.assertEqual(cases[0].driver, 'SessionProbe')
            self.assertTrue(cases[0].guest)
            self.assertTrue(cases[0].session_probe)
            self.assertFalse(cases[0].assignable)
            large = next(case for case in cases if case.session_large)
            self.assertEqual(large.driver, 'SessionInput')
            self.assertTrue(large.session_input)
            self.assertEqual(large.expected_display, (2560, 1440) if isa == 'Amd64' else (640, 400))


    def test_optional_font_cases_remove_only_the_data_receipts(self):
        for isa in ('Amd64', 'Ia64'):
            cases = Scenarios.select([isa + 'Session'], [])
            absent = [case for case in cases if case.ui_font_absent]
            self.assertEqual([case.name for case in absent], [isa + '.SessionFontFallback'])
            self.assertEqual(absent[0].driver, 'SessionCompositor')

    def test_product_session_uses_real_seats_and_has_two_fallback_probes(self):
        for isa in ('Amd64', 'Ia64'):
            cases = [case for case in Scenarios.CASES if case.product_desktop and case.isa == isa and case.driver == 'ProductDesktop']
            self.assertEqual(len(cases), 6)
            self.assertTrue(all(case.firmware == Scenarios.PRODUCTION_FIRMWARE and not case.guest for case in cases))
            usb = next(case for case in cases if case.name.endswith('DesktopUsb'))
            self.assertTrue(usb.usb_console or usb.usb_host)
            self.assertTrue(any(case.session_face_withheld for case in cases))
            self.assertTrue(any(case.session_compositor_fault for case in cases))
            self.assertEqual({case.power for case in cases if case.power}, {'off', 'reboot'})

    def test_pointer_fence_waits_for_complete_presented_metadata(self):
        lines = ['USER:# session: begin 8 136 112',
                 'USER:# session: input dropped 0', 'USER:# pointer: 320,200 sequence 0']
        context = SimpleNamespace(console=SimpleNamespace(lines=lines),
                                  wait_for_line=mock.Mock(return_value=(3, 'USER:# session: presented 8 pixels 0 us 0')))
        self.assertEqual(SessionCompositor.complete_snapshot(context, 2), 4)
        context.wait_for_line.assert_called_once_with('USER:# session: presented 8 ', 30, since=1)
        lines.append('USER:# session: presented 8 pixels 0 us 0')
        context.wait_for_line.reset_mock()
        self.assertEqual(SessionCompositor.complete_snapshot(context, 3), 4)
        context.wait_for_line.assert_not_called()
