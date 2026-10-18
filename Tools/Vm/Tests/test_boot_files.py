# SPDX-License-Identifier: Apache-2.0
"""Provenance failures that existing image/page observers cannot see."""
import copy
from types import SimpleNamespace
import unittest

from AsteriaVm import BootFiles
from AsteriaVerify import BootFiles as Manifest


class BootFilesTests(unittest.TestCase):
    def test_castagnoli_vector(self):
        self.assertEqual(Manifest.crc32c(b'123456789'), 0xe3069283)

    def test_kernel_is_required_in_manifest(self):
        old = b'version = 1\ninit = INIT.ELF crc32c=0x00000001\nuser = USER.ELF crc32c=0x00000002\n'
        with self.assertRaises(ValueError):
            Manifest.manifest(old)
        self.assertEqual(Manifest.manifest(old + b'kernel = KERNEL.ELF crc32c=0x00000003\n')['kernel'],
                         ('KERNEL.ELF', 3))

    def test_region_and_stage_provenance(self):
        expected = {'init': ('INIT.ELF', 2048, 1), 'user': ('USER.ELF', 2048, 2)}
        files = [SimpleNamespace(Name=tuple(name.encode().ljust(13, b'\0')), SizeBytes=size,
                                 Crc32c=crc, Offset=4096 + index * 2048, Reserved=(0,) * 7)
                 for index, (name, size, crc) in enumerate(expected.values())]
        files.append(SimpleNamespace(Name=(0,) * 13, SizeBytes=0, Crc32c=0, Offset=0, Reserved=(0,) * 7))
        module = SimpleNamespace(Name=tuple(b'component'.ljust(16, b'\0')),
                                 File=SimpleNamespace(Name=tuple(b'COMP.ELF'.ljust(13, b'\0')),
                                                      SizeBytes=64, Crc32c=3, Offset=8240, Reserved=(0,) * 7))
        region = SimpleNamespace(PhysicalStart=0x10000, PageCount=3, ManifestBytes=160, Files=files,
                                 ModuleOffset=8192, ModuleBytes=4096, ModuleCount=1, Modules=[module])
        lines = ['%s%s size=%d crc32c=0x%08x' % ((BootFiles.PREFIX,) + entry) for entry in expected.values()]
        expected['module:component'] = ('MODULES/COMP.ELF', 64, 3)
        lines.append('ASTERIA:STAGE:BOOT-MODULE:component COMP.ELF size=64 crc32c=0x00000003')
        expected['kernel'] = ('KERNEL.ELF', 4096, 4)
        lines.append('ASTERIA:STAGE:BOOT-FILE:KERNEL.ELF size=4096 crc32c=0x00000004')
        self.assertEqual(BootFiles.problems(region, expected, lines), [])
        self.assertTrue(BootFiles.problems(region, expected, lines[:-1]))
        self.assertTrue(BootFiles.problems(region, {key: value for key, value in expected.items()
                                                        if key != 'kernel'}, lines))
        for label, change in (
                ('missing receipt', lambda r: setattr(r, 'PageCount', 0)),
                ('bad CRC', lambda r: setattr(r.Files[0], 'Crc32c', 3)),
                ('bad name', lambda r: setattr(r.Files[0], 'Name', (0,) * 13)),
                ('bad size', lambda r: setattr(r.Files[0], 'SizeBytes', 2049)),
                ('overlap', lambda r: setattr(r.Files[1], 'Offset', 4096)),
                ('outside region', lambda r: setattr(r.Files[1], 'Offset', 8192)),
                ('unexpected spinner', lambda r: setattr(r.Files[2], 'SizeBytes', 1)),
                ('reserved bits', lambda r: setattr(r.Files[0], 'Reserved', (1,))),
                ('bad manifest extent', lambda r: setattr(r, 'ManifestBytes', 4097)),
                ('missing module', lambda r: setattr(r, 'ModuleCount', 0)),
                ('unaligned module range', lambda r: setattr(r, 'ModuleOffset', 8193)),
                ('module range overlaps image', lambda r: setattr(r, 'ModuleOffset', 4096)),
                ('module outside range', lambda r: setattr(r.Modules[0].File, 'Offset', 12288)),
                ('module overlaps header', lambda r: setattr(r.Modules[0].File, 'Offset', 8192)),
                ('module CRC', lambda r: setattr(r.Modules[0].File, 'Crc32c', 4)),
                ('module name', lambda r: setattr(r.Modules[0], 'Name', (0,) * 16))):
            with self.subTest(label=label):
                mutant = copy.deepcopy(region)
                change(mutant)
                self.assertTrue(BootFiles.problems(mutant, expected, lines))
        self.assertTrue(BootFiles.problems(region, expected, lines * 2))
        self.assertTrue(BootFiles.problems(region, expected, [lines[1], lines[0], lines[2]]))

    def test_module_receipts_use_the_abi_capacity(self):
        for count in (10, Manifest.MAXIMUM_MODULES):
            with self.subTest(count=count):
                modules, expected, lines = [], {}, []
                for index in range(count):
                    logical, name = 'module%d' % index, 'M%d.ELF' % index
                    modules.append(SimpleNamespace(Name=tuple(logical.encode().ljust(16, b'\0')),
                        File=SimpleNamespace(Name=tuple(name.encode().ljust(13, b'\0')), SizeBytes=64,
                            Crc32c=index, Offset=4112 + count * 32 + index * 64, Reserved=(0,) * 7)))
                    expected['module:' + logical] = ('MODULES/' + name, 64, index)
                    lines.append('ASTERIA:STAGE:BOOT-MODULE:%s %s size=64 crc32c=0x%08x' % (logical, name, index))
                expected['kernel'] = ('KERNEL.ELF', 64, 0)
                lines.append('ASTERIA:STAGE:BOOT-FILE:KERNEL.ELF size=64 crc32c=0x00000000')
                empty = SimpleNamespace(Name=(0,) * 13, SizeBytes=0, Crc32c=0, Offset=0, Reserved=(0,) * 7)
                region = SimpleNamespace(PhysicalStart=0x10000, PageCount=2, ManifestBytes=4096,
                    Files=[empty] * 3, ModuleOffset=4096, ModuleBytes=4096, ModuleCount=count, Modules=modules)
                self.assertEqual(BootFiles.problems(region, expected, lines), [])



class DisplayManifestTests(unittest.TestCase):
    def test_policy_is_validated_without_becoming_a_file_entry(self):
        base = (b'version=1\nkernel=KERNEL.ELF crc32c=0x00000000\n'
                b'init=INIT.ELF crc32c=0x00000000\nuser=USER.ELF crc32c=0x00000000\n')
        expected = Manifest.manifest(base)
        for value in (b'native', b'keep', b'1366x768', b'4294967295x1'):
            self.assertEqual(Manifest.manifest(base + b'display = ' + value), expected)
        for value in (b'auto', b'0x768', b'1366X768', b'1366x0', b'4294967296x768', b'1366x'):
            with self.assertRaises(ValueError):
                Manifest.manifest(base + b'display = ' + value)
        with self.assertRaises(ValueError):
            Manifest.manifest(base + b'display=keep\ndisplay=native')


if __name__ == '__main__':
    unittest.main()


class TrustManifestTests(unittest.TestCase):
    """The trust key, checked as strictly as the loader checks it."""
    BASE = (b'version=1\nkernel=KERNEL.ELF crc32c=0x00000000\ninit=INIT.ELF crc32c=0x00000000\n'
            b'user=USER.ELF crc32c=0x00000000\nmodule=component COMP.ELF crc32c=0x00000000\n')
    DIGEST = bytes(range(32))

    def line(self, **fields) -> bytes:
        return (Manifest.trust_line('component', self.DIGEST, fields.get('driver', 'ahci'),
                                    fields.get('address'), fields.get('identifier')) + '\n').encode()

    def test_entries_are_validated_without_becoming_files(self):
        expected = Manifest.manifest(self.BASE)
        text = (self.BASE + self.line(address=(0, 0, 0x1f, 2), identifier=(0x8086, 0x2922))
                + self.line(driver='xhci', identifier=(0x1b36, 0x000d))
                + self.line(driver='e1000e', address=(1, 2, 3, 4)))
        self.assertEqual(Manifest.manifest(text), expected)
        entries = Manifest.trust_entries(text)
        self.assertEqual([entry['driver'] for entry in entries], ['ahci', 'xhci', 'e1000e'])
        self.assertEqual(entries[0]['address'], (0, 0, 0x1f, 2))
        self.assertEqual(entries[1]['address'], None)
        self.assertEqual(entries[1]['identifier'], (0x1b36, 0x000d))
        self.assertEqual(entries[2]['digest'], self.DIGEST)
        self.assertIn(b'trust = pci=0000:00:1f.2 id=8086:2922 driver=ahci module=component sha256=0001',
                      self.line(address=(0, 0, 0x1f, 2), identifier=(0x8086, 0x2922)))

    def test_invalid_entries_are_refused(self):
        digest = self.DIGEST.hex()
        for label, text in (
                ('no selector', 'trust = driver=ahci module=component sha256=%s' % digest),
                ('device beyond 1f', 'trust = pci=0000:00:20.0 driver=ahci module=component sha256=%s' % digest),
                ('function beyond 7', 'trust = pci=0000:00:1f.8 driver=ahci module=component sha256=%s' % digest),
                ('vendor ffff', 'trust = id=ffff:2922 driver=ahci module=component sha256=%s' % digest),
                ('edu', 'trust = id=1234:11e8 driver=edu module=component sha256=%s' % digest),
                ('order', 'trust = id=8086:2922 pci=0000:00:1f.2 driver=ahci module=component sha256=%s' % digest),
                ('short digest', 'trust = id=8086:2922 driver=ahci module=component sha256=%s' % digest[:62]),
                ('unknown module', 'trust = id=8086:2922 driver=ahci module=other sha256=%s' % digest),
                ('trailing word', 'trust = id=8086:2922 driver=ahci module=component sha256=%s x' % digest)):
            with self.subTest(label=label), self.assertRaises(ValueError):
                Manifest.manifest(self.BASE + text.encode())
        with self.assertRaises(ValueError):
            Manifest.manifest(self.BASE + self.line(address=(0, 0, 1, 0)) + self.line(address=(0, 0, 1, 0),
                                                                                       identifier=(1, 2)))
        with self.assertRaises(ValueError):
            Manifest.manifest(self.BASE + self.line(identifier=(1, 2)) * 2)
        with self.assertRaises(ValueError):
            Manifest.manifest(self.BASE + b''.join(self.line(identifier=(1, index)) for index in range(9)))
        with self.assertRaises(ValueError):
            Manifest.trust_line('component', self.DIGEST, 'ahci')
