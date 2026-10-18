# SPDX-License-Identifier: Apache-2.0
"""Filesystem disk observers; host tools only, no VM processes."""
from __future__ import annotations

import pathlib
import struct
import tempfile
import unittest
from unittest import mock

from AsteriaVm import DiskRecipes as R, Machine, Scenarios
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import DiskImages as D


class ContractTests(unittest.TestCase):
    def test_scenario_lines_are_the_guest_contract(self):
        self.assertEqual(R.FAT_SCENARIO_LINES, {
            'fat12': 'asteria fat12 clean\n',
            'fat16': 'asteria fat16 crash-before\n',
            'fat32': 'asteria fat32 clean\n',
            'exfat': 'asteria exfat clean\n',
            'fat32-v2': 'asteria fat32 v2\n',
            'exfat-v2': 'asteria exfat v2\n',
        })
        for recipe, line in R.FAT_SCENARIO_LINES.items():
            self.assertEqual(R.fat_expected(recipe)['README.TXT'], line.encode('ascii'))
        self.assertTrue(200 <= len(R.LONG_CONTENT) <= 1000)
        self.assertGreater(R.LONG_CONTENT.count(b'\n'), 2)
        R.LONG_CONTENT.decode('ascii')

    def test_hello_word_and_page_size(self):
        for k in (0, 1, 2, 5):
            page = R.hello_page(k)
            self.assertEqual(len(page), 4096)
            self.assertEqual(page, struct.pack('<Q', 0x48454C4C4F000000 | k) * 512)
            self.assertEqual(page[:8], bytes((k, 0, 0, 0x4F, 0x4C, 0x4C, 0x45, 0x48)))

    def test_recipe_cases_and_suite_selection(self):
        fats = Scenarios.select([], ['Amd64.Fat*'])
        self.assertEqual([case.name for case in fats], ['Amd64.Fat12', 'Amd64.Fat16', 'Amd64.Fat32'])
        for case, recipe in zip(fats, ('fat12', 'fat16', 'fat32')):
            self.assertEqual((case.disk, case.cpus, case.preset, case.driver), (recipe, 1, 'amd64', 'Amd64Default'))
            self.assertTrue(case.guest and case.has_disk)
            self.assertIn('disk=' + recipe, Scenarios.describe(case))
            self.assertIn('firmware=firmware-test', Scenarios.describe(case))
            for suite in ('All', 'Amd64'):
                self.assertIn(case, Scenarios.select([suite], []))
        for name in ('Amd64.Ahci', 'Ia64.Ahci', 'Amd64Preemption.Ahci'):
            self.assertEqual(Scenarios.select([], [name])[0].disk, 'raw')
        for case in Scenarios.CASES:
            self.assertIsInstance(case.disk, str)
            self.assertEqual(case.has_disk, bool(case.disk))
        self.assertFalse(Scenarios.select([], ['Amd64.Production'])[0].disk)

    def test_driver_records_family_checks_and_skips_production(self):
        context = mock.Mock()
        context.case = Scenarios.select([], ['Amd64.Fat16'])[0]
        context.artifact.return_value = pathlib.Path('/tmp/not-read.img')
        pages = [R.hello_page(4)]
        with mock.patch.object(D, 'fat_problems', return_value=['broken']) as fat:
            Amd64Default.check_disk_images(context, pages)
            fat.assert_called_once_with(context.artifact.return_value, 'fat16', R.fat_expected('fat16'))
            context.check_problems.assert_called_once_with('host.amd64.disk.fat', ['broken'])
        context.reset_mock()
        context.case = Scenarios.select([], ['Amd64.Ahci'])[0]
        with mock.patch.object(D, 'asteriafs_problems', return_value=[]) as native:
            Amd64Default.check_disk_images(context, pages)
            native.assert_called_once_with(context.artifact.return_value, pages)
            context.check_problems.assert_called_once_with('host.amd64.disk.asteriafs', [])
        for name in ('Amd64.Smp1', 'Amd64Preemption.Ahci', 'Amd64.Production'):
            context.reset_mock()
            context.case = Scenarios.select([], [name])[0]
            Amd64Default.check_disk_images(context, pages)
            context.check_problems.assert_not_called()
        # IA-64's disk cases take the same checks under their ISA's names.
        context.reset_mock()
        context.case = Scenarios.select([], ['Ia64.Exfat'])[0]
        with mock.patch.object(D, 'exfat_problems', return_value=[]) as exfat:
            D.check(context, 'ia64', pages)
            exfat.assert_called_once_with(context.artifact.return_value, R.fat_expected('exfat'))
            context.check_problems.assert_called_once_with('host.ia64.disk.exfat', [])
        context.reset_mock()
        context.case = Scenarios.select([], ['Ia64.Smp1'])[0]
        D.check(context, 'ia64', pages)
        context.check_problems.assert_not_called()


class FatImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            for name in ('mkfs.fat', 'fsck.fat', 'mcopy', 'mdir', 'mtype'):
                R.find_tool(name)
        except FileNotFoundError as error:
            raise unittest.SkipTest('FAT image tests need dosfstools and mtools: %s' % error)

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.directory = pathlib.Path(self.scratch.name)
        self.image = self.directory / 'Disk.raw'

    def copy(self, name, data):
        source = self.directory / 'source'
        source.write_bytes(data)
        result = R.run_tool('mcopy', ['-o', '-i', str(self.image), str(source), '::' + name])
        self.assertEqual(result.returncode, 0, result.stderr)

    def make(self, recipe='fat16', hello=True):
        expected = R.write_disk_recipe(self.image, recipe)
        if hello:
            # Lowercase 8.3 name, as the guest creates it: NT lowercase flag.
            self.copy('hello', R.hello_page(0) + R.hello_page(1))
        return expected

    def test_all_recipes_and_intact_files(self):
        for recipe, (size, _) in R.FAT_FORMATS.items():
            with self.subTest(recipe=recipe):
                expected = self.make(recipe)
                self.assertEqual(self.image.stat().st_size, size)
                self.assertLess(self.image.stat().st_blocks * 512, size)
                with self.image.open('rb') as handle:
                    boot = handle.read(512)
                self.assertEqual(int.from_bytes(boot[14:16], 'little'), 128)
                self.assertEqual(boot[13], 16 if recipe == 'fat16' else 1)
                self.assertEqual(D.fat_problems(self.image, recipe, expected), [])

    def test_missing_hello(self):
        expected = self.make(hello=False)
        self.assertIn('HELLO', '\n'.join(D.fat_problems(self.image, 'fat16', expected)))

    def test_wrong_hello_byte(self):
        expected = self.make()
        content = bytearray(R.hello_page(0) + R.hello_page(1))
        content[5000] ^= 1
        self.copy('hello', content)
        self.assertIn('mtype: HELLO differs', '\n'.join(D.fat_problems(self.image, 'fat16', expected)))

    def test_damaged_token(self):
        expected = self.make()
        with self.image.open('r+b') as handle:
            handle.seek(8 * 512)
            handle.write(b'!')
        self.assertIn('token at sector 8', '\n'.join(D.fat_problems(self.image, 'fat16', expected)))

    def geometry(self):
        with self.image.open('rb') as handle:
            boot = handle.read(512)
        reserved, fat_size = struct.unpack_from('<H', boot, 14)[0], struct.unpack_from('<H', boot, 22)[0]
        return reserved, fat_size, (reserved + 2 * fat_size) * 512

    def test_cross_linked_fat_chain_is_rejected(self):
        expected = self.make()
        reserved, fat_size, root = self.geometry()
        with self.image.open('r+b') as handle:
            handle.seek(root)
            entries = handle.read(32 * 512)
            starts = {}
            for offset in range(0, len(entries), 32):
                name = entries[offset:offset + 11]
                if name in (b'README  TXT', b'HELLO      '):
                    starts[name] = struct.unpack_from('<H', entries, offset + 26)[0]
            self.assertEqual(len(starts), 2)
            for copy in range(2):
                handle.seek((reserved + copy * fat_size) * 512 + starts[b'HELLO      '] * 2)
                handle.write(struct.pack('<H', starts[b'README  TXT']))
        problems = D.fat_problems(self.image, 'fat16', expected)
        self.assertTrue(any('fsck.fat' in problem for problem in problems), problems)

    def crash_state(self, copies=(0, 1), sectors=1):
        reserved, fat_size, _ = self.geometry()
        with self.image.open('r+b') as handle:
            for copy in range(2):
                handle.seek((reserved + copy * fat_size) * 512 + 2)
                handle.write(struct.pack('<H', 0x7FFF))  # dirty, both copies
            for copy in copies:
                for sector in range(sectors):
                    handle.seek((reserved + copy * fat_size + 1 + sector) * 512)
                    handle.write(struct.pack('<H', 0xFFFF))  # one lost cluster per sector

    def test_allowed_dirty_and_lost_clusters(self):
        expected = self.make()
        self.crash_state()
        self.assertEqual(D.fat_problems(self.image, 'fat16', expected), [])

    def test_one_sector_newer_first_fat_is_allowed(self):
        expected = self.make()
        self.crash_state(copies=(0,))
        self.assertEqual(D.fat_problems(self.image, 'fat16', expected), [])

    def test_second_fat_newer_is_rejected(self):
        expected = self.make()
        self.crash_state(copies=(1,))
        self.assertIn('first copy', '\n'.join(D.fat_problems(self.image, 'fat16', expected)))

    def test_two_differing_sectors_are_rejected(self):
        expected = self.make()
        self.crash_state(copies=(0,), sectors=2)
        self.assertIn('differ in 2 sectors', '\n'.join(D.fat_problems(self.image, 'fat16', expected)))

    def test_crash_file_must_be_absent(self):
        expected = self.make()
        self.copy('crash.txt', b'failed commit\n')
        self.assertIn('crash.txt exists', '\n'.join(D.fat_problems(self.image, 'fat16', expected)))

    def test_clean_scenario_rejects_dirty_volume(self):
        expected = self.make('fat12')
        with self.image.open('r+b') as handle:
            handle.seek(37)  # FAT12 boot flags
            handle.write(b'\x01')
        self.assertIn('fsck.fat', '\n'.join(D.fat_problems(self.image, 'fat12', expected)))


# Small test-only encoder: raw offsets, deliberately no production encoder.
def put(data, offset, value, width=4):
    data[offset:offset + width] = value.to_bytes(width, 'little')


def seal(data, offset, location, generation):
    put(data, offset, D.checksum(bytes(data), offset, location, generation))


def native_image(path, generations=(2, 3)):
    image = bytearray(2 << 20)
    image[8 * 512:9 * 512] = Machine.DISK_TOKEN * 16
    pages_by_generation = {}
    for slot, generation in enumerate(generations):
        table_block, directory_block, data_block = 16 + slot * 4, 17 + slot * 4, 18 + slot * 4
        pages = [R.hello_page(generation * 2), R.hello_page(generation * 2 + 1)]
        pages_by_generation[generation] = pages
        for index, page in enumerate(pages):
            image[(data_block + index) * 4096:(data_block + index + 1) * 4096] = page
        table, directory, sb = bytearray(4096), bytearray(4096), bytearray(512)
        for metadata, kind, block in ((table, 1, table_block), (directory, 2, directory_block)):
            metadata[:8] = b'ASTRMET0'
            put(metadata, 8, kind)
            put(metadata, 12, generation, 8)
        for offset, kind, size, pointers in ((32, 2, 4096, [directory_block]),
                                           (64, 1, 8192, [data_block, data_block + 1])):
            put(table, offset, 1, 8)
            put(table, offset + 8, size, 8)
            put(table, offset + 16, kind)
            for index, pointer in enumerate(pointers):
                put(table, offset + 20 + index * 2, pointer, 2)
        put(directory, 32, 5)
        directory[36:41] = b'hello'
        put(directory, 104, 1, 8)
        seal(table, 20, table_block, generation)
        seal(directory, 20, directory_block, generation)
        image[table_block * 4096:(table_block + 1) * 4096] = table
        image[directory_block * 4096:(directory_block + 1) * 4096] = directory
        sb[:8] = b'ASTRFS00'
        for offset, value, width in ((12, 4096, 4), (16, 512, 4), (20, generation, 8),
                                     (28, table_block, 4), (32, int.from_bytes(table[20:24], 'little'), 4),
                                     (40, 2, 8), (48, 7, 8), (56, generation, 8), (176, generation, 8)):
            put(sb, offset, value, width)
        for block in (*range(16), table_block, directory_block, data_block, data_block + 1):
            sb[112 + block // 8] |= 1 << (block % 8)
        sector = slot * 16
        seal(sb, 508, sector, generation)
        image[sector * 512:(sector + 1) * 512] = sb
    path.write_bytes(image)
    return pages_by_generation


class AsteriaFsTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.image = pathlib.Path(self.scratch.name) / 'Disk.raw'
        self.pages = native_image(self.image)

    def corrupt(self, offset, reseal_sb=False):
        image = bytearray(self.image.read_bytes())
        image[offset] ^= 1
        if reseal_sb:
            sb = image[8192:8704]
            seal(sb, 508, 16, 3)
            image[8192:8704] = sb
        self.image.write_bytes(image)

    def test_crc32c_standard_vector_and_finalized_seeding(self):
        self.assertEqual(D.crc32c(b'123456789'), 0xE3069283)
        self.assertEqual(D.crc32c(b''), 0)
        for split in range(10):
            self.assertEqual(D.crc32c(b'123456789'[split:], D.crc32c(b'123456789'[:split])), 0xE3069283)
        data = b'123456789' + bytes(4)
        prefix = struct.pack('<QQ', 16, 3)
        self.assertEqual(D.checksum(data, 9, 16, 3), D.crc32c(data, D.crc32c(prefix)))
        self.assertNotEqual(D.checksum(data, 9, 16, 3), D.checksum(data, 9, 2, 3))
        self.assertNotEqual(D.checksum(data, 9, 16, 3), D.checksum(data, 9, 16, 4))

    def test_newer_valid_superblock_is_selected(self):
        self.assertEqual(D.asteriafs_problems(self.image, self.pages[3]), [])
        self.assertIn('generation 3 (sector 16)', '\n'.join(D.asteriafs_problems(self.image, self.pages[2])))

    def test_bad_newer_superblock_crc_falls_back(self):
        self.corrupt(8192 + 508)
        self.assertEqual(D.asteriafs_problems(self.image, self.pages[2]), [])
        self.assertIn('generation 2 (sector 0)', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_a_wins_generation_tie(self):
        pages = native_image(self.image, generations=(3, 3))[3]
        image = bytearray(self.image.read_bytes())
        image[22 * 4096] ^= 1  # only B's data differs
        self.image.write_bytes(image)
        self.assertEqual(D.asteriafs_problems(self.image, pages), [])

    def test_wrong_bitmap_bit_is_reported(self):
        self.corrupt(8192 + 112 + 10, reseal_sb=True)
        self.assertIn('bitmap differs', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_table_crc_is_reported_and_fallback_used(self):
        self.corrupt(20 * 4096 + 20)
        problems = D.asteriafs_problems(self.image, self.pages[2])
        self.assertIn('metadata CRC at block 20', '\n'.join(problems))
        self.assertFalse(any('hello differs' in problem for problem in problems))

    def test_directory_crc_is_reported(self):
        self.corrupt(21 * 4096 + 100)
        self.assertIn('metadata CRC at block 21', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_token_damage_is_reported(self):
        self.corrupt(8 * 512)
        self.assertIn('token at sector 8', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_sector_64_belongs_to_block_demo(self):
        self.corrupt(64 * 512)
        self.assertEqual(D.asteriafs_problems(self.image, self.pages[3]), [])

    def test_wrong_hello_data_is_reported(self):
        self.corrupt(22 * 4096 + 10)
        self.assertIn('hello differs', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_reserved_metadata_pointer_is_rejected(self):
        image = bytearray(self.image.read_bytes())
        sb = image[8192:8704]
        put(sb, 28, 8)
        seal(sb, 508, 16, 3)
        image[8192:8704] = sb
        self.image.write_bytes(image)
        self.assertIn('generation 2', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_misdirected_metadata_in_reserved_sectors_is_reported(self):
        image = bytearray(self.image.read_bytes())
        image[4 * 4096:5 * 4096] = image[21 * 4096:22 * 4096]
        self.image.write_bytes(image)
        self.assertIn('reserved sectors 0-127', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))

    def test_both_invalid_superblocks_are_reported(self):
        self.corrupt(508)
        self.corrupt(8192 + 508)
        self.assertIn('no mountable', '\n'.join(D.asteriafs_problems(self.image, self.pages[3])))


if __name__ == '__main__':
    unittest.main()
