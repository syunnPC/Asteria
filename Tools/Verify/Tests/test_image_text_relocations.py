# SPDX-License-Identifier: Apache-2.0
"""The IA-64 second-link gate rejects text fixups but permits descriptor fixups."""
import importlib.util
import pathlib
import struct
import unittest


path = pathlib.Path(__file__).resolve().parents[2] / 'Build' / 'CheckImageTextRelocations.py'
spec = importlib.util.spec_from_file_location('CheckImageTextRelocations', path)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


class TextGateTests(unittest.TestCase):
    def test_relocation_targets_and_boundaries(self):
        data = bytearray(512)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<QQ', data, 32, 64, 128)
        struct.pack_into('<HHHH', data, 54, 56, 1, 64, 2)
        struct.pack_into('<IIQQQQQQ', data, 64, 1, 5, 0, 0x1000, 0, 0x100, 0x100, 4096)
        struct.pack_into('<IIQQQQIIQQ', data, 128, 0, 4, 2, 0, 256, 24, 0, 0, 8, 24)
        struct.pack_into('<IIQQQQIIQQ', data, 192, 0, 6, 2, 0, 300, 16, 0, 0, 8, 16)
        for target in (0xFFF, 0x1100, 0x2000):
            struct.pack_into('<QQq', data, 256, target, 0x6F, 0)
            checker.check(data)
        for target in (0x1000, 0x10FF):
            with self.subTest(target=target):
                struct.pack_into('<QQq', data, 256, target, 0x6F, 0)
                with self.assertRaisesRegex(ValueError, 'executable text'):
                    checker.check(data)
        writable = bytearray(data)
        struct.pack_into('<I', writable, 68, 7)
        struct.pack_into('<QQq', writable, 256, 0x2000, 0x6F, 0)
        with self.assertRaisesRegex(ValueError, 'writable executable'):
            checker.check(writable)
        # A lost -pie must fail even with no allocated relocation table.
        static = bytearray(data)
        struct.pack_into('<H', static, 60, 0)
        with self.assertRaisesRegex(ValueError, 'SHT_DYNAMIC'):
            checker.check(static)
        # Allocated REL tables must be checked too, even though the current
        # IA-64 ABI emits RELA: dropping either table kind weakens the gate.
        struct.pack_into('<IIQQQQIIQQ', data, 128, 0, 9, 2, 0, 256, 16, 0, 0, 8, 16)
        with self.assertRaisesRegex(ValueError, 'executable text'):
            checker.check(data)


if __name__ == '__main__':
    unittest.main()
