# SPDX-License-Identifier: Apache-2.0
"""exfatprogs-backed runner fixtures and independent clean-volume checks; no VM."""
from __future__ import annotations

import pathlib
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

from AsteriaVm import DiskRecipes as R, ExfatImages as E, Guest, Machine, Scenarios
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import DiskImages as D


class ExfatContractTests(unittest.TestCase):
    def test_scenario_and_guest_contract(self):
        self.assertEqual(R.FAT_SCENARIO_LINES['exfat'], 'asteria exfat clean\n')
        self.assertEqual(R.fat_expected('exfat')['README.TXT'], b'asteria exfat clean\n')
        self.assertEqual(Guest.FS_LINES['exfat'], Guest.FS_EXFAT_LINE)
        self.assertEqual(Amd64Default.FS_DEMOS['exfat'],
                         Amd64Default.FsDemo(services=1, crashes=0, root_maps=5, service_maps=5))

    def test_case_and_suites(self):
        case, = Scenarios.select([], ['Amd64.Exfat'])
        self.assertEqual((case.disk, case.cpus, case.preset, case.driver),
                         ('exfat', 1, 'amd64', 'Amd64Default'))
        self.assertTrue(case.guest and case.has_disk)
        for suite in ('All', 'Amd64'):
            self.assertIn(case, Scenarios.select([suite], []))
        self.assertIn('disk=exfat', Scenarios.describe(case))

    def test_driver_checks_exfat(self):
        context = mock.Mock()
        context.case, = Scenarios.select([], ['Amd64.Exfat'])
        context.artifact.return_value = pathlib.Path('/tmp/not-read-exfat.img')
        with mock.patch.object(D, 'exfat_problems', return_value=['broken']) as check:
            Amd64Default.check_disk_images(context, [R.hello_page(3)])
        check.assert_called_once_with(context.artifact.return_value, R.fat_expected('exfat'))
        context.check_problems.assert_called_once_with('host.amd64.disk.exfat', ['broken'])


class ExfatImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            for name in ('mkfs.exfat', 'fsck.exfat'):
                R.find_tool(name)
        except FileNotFoundError as error:
            raise unittest.SkipTest('EXFAT IMAGE TESTS SKIPPED: exfatprogs required: %s' % error)

    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix='ExfatTests-')
        self.addCleanup(scratch.cleanup)
        self.image = pathlib.Path(scratch.name) / 'Disk.raw'

    def make(self, hello=True):
        expected = R.write_disk_recipe(self.image, 'exfat')
        if hello:
            self.hello_offset = E.write_file(self.image, 'hello', R.hello_page(0) + R.hello_page(1))
        return expected

    def write(self, offset, data):
        with self.image.open('r+b') as handle:
            handle.seek(offset)
            handle.write(data)

    def read(self, offset, size):
        with self.image.open('rb') as handle:
            handle.seek(offset)
            return handle.read(size)

    def change_set(self, offset, data, reseal=True):
        entries = bytearray(self.read(self.hello_offset, 96))
        entries[offset:offset + len(data)] = data
        if reseal:
            struct.pack_into('<H', entries, 2, E.entry_set_checksum(entries))
        self.write(self.hello_offset, entries)

    def assert_fsck_clean(self):
        result = R.run_tool('fsck.exfat', ['-n', str(self.image)])
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode())

    def assert_problem(self, expected, message):
        self.assertIn(message, '\n'.join(D.exfat_problems(self.image, expected)))

    def test_recipe_sparse_token_checksums_and_decoder(self):
        expected = self.make(hello=False)
        self.assertEqual(self.image.stat().st_size, 64 << 20)
        self.assertLess(self.image.stat().st_blocks * 512, 64 << 20)
        self.assertEqual(expected['README.TXT'], R.FAT_SCENARIO_LINES['exfat'].encode('ascii'))
        for base in (0, 12):
            self.assertEqual(self.read((base + 8) * 512, 512), Machine.DISK_TOKEN * 16)
            # Separate reference recurrence, rather than the writer's checksum helper.
            checksum = 0
            for index, byte in enumerate(self.read(base * 512, 11 * 512)):
                if index not in (106, 107, 112):
                    checksum = ((checksum >> 1) + (0x80000000 if checksum & 1 else 0) + byte) & 0xFFFFFFFF
            self.assertEqual(self.read((base + 11) * 512, 512), struct.pack('<I', checksum) * 128)
        self.assert_fsck_clean()
        files, problems = D.decode_exfat(self.image)
        self.assertEqual(files, expected)
        self.assertEqual(problems, [])

    def test_simulated_guest_is_clean(self):
        expected = self.make()
        self.assert_fsck_clean()
        self.assertEqual(D.exfat_problems(self.image, expected), [])

    def test_leaked_bitmap_bit(self):
        expected = self.make()
        with E.Volume(self.image) as volume:
            entry = next(entry for _, entry in volume.root_entries() if entry[0] == 0x81)
            cluster = volume.geometry.cluster_count + 1
            bit = cluster - 2
            clusters = volume.allocation(E.uint(entry, 20), E.uint(entry, 24, 8))
            offset = volume.cluster_offset(clusters[(bit // 8) // volume.geometry.cluster_bytes])
            offset += (bit // 8) % volume.geometry.cluster_bytes
        self.write(offset, bytes((self.read(offset, 1)[0] | (1 << (bit % 8)),)))
        self.assert_problem(expected, 'bitmap has leaked clusters')

    def test_wrong_name_hash_with_valid_set_checksum(self):
        expected = self.make()
        value = int.from_bytes(self.read(self.hello_offset + 36, 2), 'little') ^ 1
        self.change_set(36, struct.pack('<H', value))
        self.assert_problem(expected, 'hello NameHash is wrong')
        self.assertNotIn('SetChecksum', '\n'.join(D.decode_exfat(self.image)[1]))

    def test_wrong_set_checksum(self):
        expected = self.make()
        value = int.from_bytes(self.read(self.hello_offset + 2, 2), 'little') ^ 1
        self.change_set(2, struct.pack('<H', value), reseal=False)
        self.assert_problem(expected, 'hello SetChecksum is wrong')

    def test_valid_data_length_differs(self):
        expected = self.make()
        self.change_set(40, struct.pack('<Q', 4096))
        self.assert_problem(expected, 'hello ValidDataLength differs from DataLength')

    def test_volume_dirty_is_outside_boot_checksum(self):
        expected = self.make()
        self.write(106, struct.pack('<H', 2))
        self.assert_problem(expected, 'VolumeDirty is set')
        self.assertFalse(any('boot checksum' in finding for finding in D.decode_exfat(self.image)[1]))

    def test_missing_hello(self):
        self.assert_problem(self.make(hello=False), 'hello is missing')

    def test_wrong_hello_byte(self):
        expected = self.make()
        with E.Volume(self.image) as volume:
            offset = volume.cluster_offset(E.uint(self.read(self.hello_offset + 52, 4), 0))
        self.write(offset + 10, b'X')
        self.assert_problem(expected, 'hello differs from expected bytes')

    def test_damaged_token(self):
        expected = self.make()
        self.write(8 * 512, b'X')
        self.assert_problem(expected, 'token at sector 8 changed')
        self.assert_problem(expected, 'boot checksum at sector 11 is wrong')

    def test_backup_checksum_damage(self):
        expected = self.make()
        self.write(23 * 512 + 500, b'X')
        self.assert_problem(expected, 'boot checksum at sector 23 is wrong')

    def test_damaged_boot_geometry_and_cyclic_root_are_bounded(self):
        expected = self.make()
        boot = self.read(0, 512)
        self.write(109, b'\xff')
        self.assert_problem(expected, 'boot sector or unsupported geometry')
        self.write(0, boot)
        with E.Volume(self.image) as volume:
            g = volume.geometry
        self.write(g.fat_sector * 512 + g.root_cluster * 4, struct.pack('<I', g.root_cluster))
        self.assert_problem(expected, 'cyclic exFAT FAT chain')

    def test_gap_rule_and_one_sector_sets(self):
        expected = self.make()
        # Nine-entry sets cannot fit in the tail of the first sector.
        first_gap = None
        for index in range(3):
            name = 'gap%d-' % index + 'a' * 95
            expected[name] = b'gap fixture\n'
            offset = E.write_file(self.image, name, expected[name])
            if first_gap is None:
                first_gap = offset + 9 * 32
            self.assertEqual(offset // 512, (offset + 9 * 32 - 1) // 512)
        gap_start = first_gap
        gap_end = (gap_start + 511) // 512 * 512
        self.assertGreater(gap_end, gap_start)
        self.assertEqual([self.read(offset, 1)[0] for offset in range(gap_start, gap_end, 32)],
                         [5] * ((gap_end - gap_start) // 32))
        self.assert_fsck_clean()
        self.assertEqual(D.exfat_problems(self.image, expected), [])

    def test_empty_file_and_duplicate_refusal(self):
        expected = self.make()
        offset = E.write_file(self.image, 'empty', b'')
        expected['empty'] = b''
        self.assertEqual(self.read(offset + 33, 1), b'\x01')
        self.assert_fsck_clean()
        self.assertEqual(D.exfat_problems(self.image, expected), [])
        with self.assertRaisesRegex(ValueError, 'already exists'):
            E.write_file(self.image, 'HELLO', b'replace refused')
        self.assertEqual(D.exfat_problems(self.image, expected), [])

    def test_512_byte_clusters_follow_metadata_fat_chains(self):
        with self.image.open('wb') as handle:
            handle.truncate(64 << 20)
        result = R.run_tool('mkfs.exfat', ['-c', '512', str(self.image)])
        self.assertEqual(result.returncode, 0, result.stderr)
        E.stamp_boot_token(self.image, Machine.DISK_TOKEN * 16)
        expected = R.fat_expected('exfat')
        # This metadata-chain fixture has one 512-byte root sector; it does
        # not exercise writable service mounts and leaves out their opt-in.
        expected.pop('ASTERIA.RW')
        for name, content in {**expected, 'hello': R.hello_page(0) + R.hello_page(1)}.items():
            E.write_file(self.image, name, content)
        with E.Volume(self.image) as volume:
            self.assertEqual(volume.geometry.cluster_bytes, 512)
            for _, entry in volume.root_entries():
                if entry[0] in (0x81, 0x82):
                    self.assertGreater(len(volume.allocation(E.uint(entry, 20), E.uint(entry, 24, 8))), 1)
        self.assert_fsck_clean()
        self.assertEqual(D.exfat_problems(self.image, expected), [])

    def test_fragmented_hello_follows_fat(self):
        expected = self.make()
        with E.Volume(self.image, 'r+b') as volume:
            first = E.uint(self.read(self.hello_offset + 52, 4), 0)
            # Move page 1 past a free cluster and encode a real two-cluster FAT chain.
            new = first + 2
            volume.write(volume.cluster_offset(new), R.hello_page(1))
            fat = volume.geometry.fat_sector * 512
            volume.write(fat + first * 4, struct.pack('<I', new))
            volume.write(fat + new * 4, struct.pack('<I', 0xFFFFFFFF))
            entry = next(entry for _, entry in volume.root_entries() if entry[0] == 0x81)
            bitmap_offset = volume.cluster_offset(E.uint(entry, 20))
            bitmap = bytearray(volume.read(bitmap_offset, E.uint(entry, 24, 8)))
            for cluster, allocate in ((first + 1, False), (new, True)):
                bit = cluster - 2
                if allocate:
                    bitmap[bit // 8] |= 1 << (bit % 8)
                else:
                    bitmap[bit // 8] &= ~(1 << (bit % 8))
            volume.write(bitmap_offset, bitmap)
        self.change_set(33, b'\x01')
        self.assert_fsck_clean()
        self.assertEqual(D.exfat_problems(self.image, expected), [])

    def test_referenced_cluster_marked_free(self):
        expected = self.make()
        with E.Volume(self.image) as volume:
            first = E.uint(self.read(self.hello_offset + 52, 4), 0)
            entry = next(entry for _, entry in volume.root_entries() if entry[0] == 0x81)
            offset = volume.cluster_offset(E.uint(entry, 20)) + (first - 2) // 8
        self.write(offset, bytes((self.read(offset, 1)[0] & ~(1 << ((first - 2) % 8)),)))
        self.assert_problem(expected, 'referenced clusters are marked free')

    def test_recipe_refuses_failed_fsck(self):
        run_tool = R.run_tool

        def fail_fsck(name, arguments):
            if name == 'fsck.exfat':
                return subprocess.CompletedProcess([], 4, b'', b'injected fsck failure')
            return run_tool(name, arguments)

        with mock.patch.object(R, 'run_tool', side_effect=fail_fsck):
            with self.assertRaisesRegex(ValueError, 'fsck.exfat exited 4'):
                R.write_disk_recipe(self.image, 'exfat')

    def test_missing_host_tools_still_runs_model(self):
        expected = self.make(hello=False)
        with mock.patch.object(R, 'run_tool', side_effect=FileNotFoundError('fsck.exfat missing')):
            findings = '\n'.join(D.exfat_problems(self.image, expected))
        self.assertIn('fsck.exfat missing', findings)
        self.assertIn('hello is missing', findings)


if __name__ == '__main__':
    unittest.main()
