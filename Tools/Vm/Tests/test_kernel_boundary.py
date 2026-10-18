# SPDX-License-Identifier: Apache-2.0
import struct
import unittest
import zlib

from AsteriaVm.Observers import KernelBoundary


def table():
    data = bytearray(120)
    struct.pack_into('<QIIII', data, 0, 0x5453595320494249, 0x2000A, len(data), 0, 0)
    struct.pack_into('<I', data, 16, zlib.crc32(data))
    return data


class KernelBoundaryTests(unittest.TestCase):
    def test_fact_requires_one_full_width_aligned_address(self):
        valid = KernelBoundary.PREFIX + '0x0000000012345000'
        self.assertEqual(KernelBoundary.system_table([valid]), (0x12345000, []))
        for lines in ([], [valid, valid], [valid[:-1]], [valid[:-1] + '1'],
                      [KernelBoundary.PREFIX + '0x0000000000000000']):
            self.assertTrue(KernelBoundary.system_table(lines)[1])

    def test_expired_pointers_are_checked_independently_of_crc(self):
        self.assertEqual(KernelBoundary.problems(table()), [])
        for offset in (40, 48, 56, 64, 72, 80, 96):
            changed = table()
            struct.pack_into('<Q', changed, offset, 0x12345000)
            struct.pack_into('<I', changed, 16, 0)
            struct.pack_into('<I', changed, 16, zlib.crc32(changed))
            self.assertIn('boot services or console fields remain available', KernelBoundary.problems(changed))

    def test_header_crc_and_extent_mutants(self):
        for offset in (0, 8, 12, 16, 20):
            changed = table()
            changed[offset] ^= 1
            self.assertTrue(KernelBoundary.problems(changed))
        self.assertTrue(KernelBoundary.problems(table()[:-1]))
        self.assertTrue(KernelBoundary.problems(table() + bytes(1)))
