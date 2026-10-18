# SPDX-License-Identifier: Apache-2.0
"""Opaque UI-font receipts keep the ordinary boot-manifest checksum contract."""
import importlib.util
import pathlib
import tempfile
import unittest
from AsteriaVm import Build  # installs the project Verify package path
from AsteriaVerify import BootFiles

ROOT = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('UiFontBootMedia', ROOT / 'Tools/Build/MakeBootMedia.py')
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)


class UiFontMediaTests(unittest.TestCase):
    def test_font_data_and_absence_use_the_existing_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            esp = pathlib.Path(temporary)
            directory = esp / 'EFI/ASTERIA'
            (directory / 'MODULES').mkdir(parents=True)
            lines = ['version = 1']
            for logical, name in (('kernel', 'KERNEL.ELF'), ('init', 'INIT.ELF'), ('user', 'USER.ELF')):
                payload = logical.encode()
                (directory / name).write_bytes(payload)
                lines.append('%s = %s crc32c=0x%08x' % (logical, name, BootFiles.crc32c(payload)))
            base = '\n'.join(lines) + '\n'
            manifest = directory / 'BOOT.CFG'
            manifest.write_text(base)
            media.validate_esp(esp)
            for logical, name in (('ui-bold', 'VEGURB.OTF'), ('ui-regular', 'VEGURR.OTF')):
                payload = b'opaque font bytes'
                (directory / 'MODULES' / name).write_bytes(payload)
                lines.append('module = %s %s crc32c=0x%08x' % (logical, name, BootFiles.crc32c(payload)))
            manifest.write_text('\n'.join(lines) + '\n')
            media.validate_esp(esp)
            (directory / 'MODULES/VEGURB.OTF').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError, 'ui-bold.*CRC-32C mismatch'):
                media.validate_esp(esp)
