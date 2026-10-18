# SPDX-License-Identifier: Apache-2.0
"""The scenario's independent capture/input evidence helpers."""
import struct
import unittest
import zlib
from AsteriaVm import Scenarios
from AsteriaVm.Drivers import ToolkitSession


class ToolkitSessionTests(unittest.TestCase):
    def test_native_scenarios_both_isas_and_default_off(self):
        cases = Scenarios.select(['ToolkitSession'], [])
        self.assertEqual({case.isa for case in cases}, {'Amd64','Ia64'})
        for case in cases:
            self.assertTrue(case.toolkit_session and case.product_desktop)
            self.assertFalse(case.session_compositor or case.session_input or case.session_large)
            self.assertEqual(case.firmware,Scenarios.TEST_FIRMWARE)
        self.assertTrue(all(not case.toolkit_session for case in Scenarios.CASES
                            if case.driver != 'ToolkitSession'))

    def test_snapshot_selects_trusted_name_and_complete_image(self):
        lines = ['USER:# session: font Vegur-Bold Vegur-Regular',
                 'USER:# session: begin 5 640 400',
                 'USER:# session: window 0 32 32 648 429 1 1 640 400 8 0',
                 'USER:# session: title 0 '+b'boot-log'.hex()+' -',
                 'USER:# session: window 4 32 11 608 389 2 1 600 360 3 0',
                 'USER:# session: title 4 '+b'toolkit-demo'.hex()+' -',
                 'USER:# session: presented 5 pixels 6000 us 100',
                 'USER:# session: begin 6 640 400']
        self.assertEqual(ToolkitSession.demo_window(lines),[4,32,11,608,389,2,1,600,360,3,0])
        with self.assertRaisesRegex(ValueError,'complete toolkit'):
            ToolkitSession.demo_window(lines[:-2])

    def test_crop_and_png_are_lossless(self):
        rgb = bytes(range(36))
        actual = ToolkitSession.crop(rgb,4,(1,1,2,2))
        self.assertEqual(actual,rgb[15:21]+rgb[27:33])
        data = ToolkitSession.png(2,2,actual)
        self.assertEqual(data[:8],b'\x89PNG\r\n\x1a\n')
        chunks = {}; at = 8
        while at < len(data):
            length, = struct.unpack_from('>I',data,at); kind = data[at+4:at+8]
            payload = data[at+8:at+8+length]
            self.assertEqual(struct.unpack_from('>I',data,at+8+length)[0],zlib.crc32(kind+payload))
            chunks[kind] = payload; at += length+12
        self.assertEqual(struct.unpack('>IIBBBBB',chunks[b'IHDR']),(2,2,8,2,0,0,0))
        self.assertEqual(zlib.decompress(chunks[b'IDAT']),b'\0'+actual[:6]+b'\0'+actual[6:])
