# SPDX-License-Identifier: Apache-2.0
"""Tests of Observers.Devices: query-pci functions, the disk fixture, IDENTIFY data and DMA buffers."""
from __future__ import annotations

import pathlib
import struct
import tempfile
import unittest

from AsteriaVm import Machine
from AsteriaVm.Observers import Devices as D
from .test_amd64_fixtures import observation, record


def identify_block(serial: str, model: str, sectors: int, *, lba48: bool = True) -> bytes:
    words = [0] * 256

    def put(first: int, count: int, text: str) -> None:
        raw = text.ljust(count * 2).encode()
        for index in range(count):
            words[first + index] = raw[index * 2] << 8 | raw[index * 2 + 1]

    put(10, 10, serial)
    put(27, 20, model)
    words[49] = 1 << 9
    words[60], words[61] = sectors & 0xFFFF, sectors >> 16 & 0xFFFF
    if lba48:
        words[83] = 0x4000 | 1 << 10
        for index in range(4):
            words[100 + index] = sectors >> (16 * index) & 0xFFFF
    return struct.pack('<256H', *words)


class PciTests(unittest.TestCase):
    def test_recorded_query_pci(self):
        functions = D.pci_functions(observation('Smp4').query('query-pci'))
        self.assertEqual([(f.bus, f.slot, f.function) for f in functions],
                         [(0, 0, 0), (0, 1, 0), (0, 2, 0), (0, 3, 0), (0, 4, 0), (0, 16, 0), (0, 31, 0),
                          (0, 31, 2), (0, 31, 3)])
        self.assertEqual([f.class_code for f in functions if f.class_code >> 8 == 0x01], [0x0100, 0x0106])
        self.assertEqual(D.function_count_problems(9, functions), [])
        self.assertIn('QEMU has 9', D.function_count_problems(8, functions)[0])
        self.assertTrue(D.function_count_problems(10, functions))

    def test_functions_behind_bridges_count(self):
        leaf = {'bus': 1, 'slot': 0, 'function': 0, 'id': {'vendor': 0x1AF4, 'device': 0x1001},
                'class_info': {'class': 0x0100}}
        bridge = {'bus': 0, 'slot': 3, 'function': 0, 'id': {'vendor': 0x1B36, 'device': 0x0001},
                  'class_info': {'class': 0x0604}, 'pci_bridge': {'bus': {}, 'devices': [leaf]}}
        self.assertEqual(len(D.pci_functions([{'bus': 0, 'devices': [bridge]}])), 2)


class FixtureTests(unittest.TestCase):
    def test_disk_fixture_sectors(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'Disk.raw'
            Machine.write_disk_fixture(path)
            self.assertEqual(D.fixture_sector(path, 8), Machine.DISK_TOKEN * 16)
            self.assertEqual(D.fixture_sector(path, 7), bytes(512))
            self.assertEqual(D.fixture_sector_count(path), 4096)
            with self.assertRaises(ValueError):
                D.fixture_sector(path, 4096)

    def test_buffer_comparison(self):
        expected = Machine.DISK_TOKEN * 16
        self.assertEqual(D.buffer_problems(expected, expected, 'buffer'), [])
        corrupted = bytearray(expected)
        corrupted[0x1FF] ^= 1
        self.assertIn('offset 0x1ff', D.buffer_problems(bytes(corrupted), expected, 'buffer')[0])
        self.assertIn('511 bytes', D.buffer_problems(expected[:-1], expected, 'buffer')[0])


class IdentifyTests(unittest.TestCase):
    def test_synthetic_identify_data(self):
        for lba48 in (True, False):
            data = D.decode_identify(identify_block('SERIAL01', 'MODEL X', 4096, lba48=lba48))
            self.assertEqual((data.serial, data.model, data.lba48, data.sectors), ('SERIAL01', 'MODEL X', lba48, 4096))
        with self.assertRaises(ValueError):
            D.decode_identify(bytes(510))

    def test_recorded_identify_data_matches_the_runner_disk(self):
        raw = record('Ahci').Ahci.IdentifyData
        identify = D.decode_identify(raw)
        expected = dict(serial=Machine.DISK_SERIAL, model=Machine.DISK_MODEL,
                        sectors=Machine.DISK_SIZE_BYTES // 512)
        self.assertEqual(D.identify_problems(identify, **expected), [])
        for field, value in (('serial', 'ASTERIAAHCI0002'), ('model', 'QEMU HARDDISK'), ('sectors', 8192)):
            with self.subTest(field=field):
                self.assertTrue(D.identify_problems(identify, **dict(expected, **{field: value})))
        corrupted = bytearray(raw)
        corrupted[27 * 2] ^= 0x20   # one model character changes case
        self.assertTrue(D.identify_problems(D.decode_identify(bytes(corrupted)), **expected))

    def test_recorded_read_buffer_matches_the_fixture(self):
        ahci = record('Ahci').Ahci
        buffer = observation('Ahci').physical.read(ahci.ReadBufferPhysicalAddress, ahci.ReadBufferSizeBytes)
        self.assertEqual(D.buffer_problems(buffer, Machine.DISK_TOKEN * 16, 'buffer'), [])


if __name__ == '__main__':
    unittest.main()
