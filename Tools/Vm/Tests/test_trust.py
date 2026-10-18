# SPDX-License-Identifier: Apache-2.0
"""The trusted-driver profile's runner parts: the cases' BOOT.CFG entries and
the judgement of what the boot reported (Drivers.Trusted)."""
import hashlib
import pathlib
import tempfile
import unittest
from types import SimpleNamespace

from AsteriaVm import Trust
from AsteriaVm.Drivers import Trusted


class TrustLinesTests(unittest.TestCase):
    def esp(self, directory: pathlib.Path, module: bytes, digest: bytes) -> tuple[pathlib.Path, pathlib.Path]:
        esp = directory / 'FirmwareTest' / 'Esp'
        (esp / 'EFI' / 'ASTERIA' / 'MODULES').mkdir(parents=True)
        (esp / 'EFI' / 'ASTERIA' / 'MODULES' / 'COMP.ELF').write_bytes(module)
        (esp.parent / Trust.DIGESTS).write_text('component COMP.ELF sha256=%s\ndirectory DIR.ELF sha256=%s\n' % (
            digest.hex(), bytes(32).hex()))
        return esp, esp

    def test_entries_name_the_functions_and_the_built_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            module = b'\x7fELF component'
            digest = hashlib.sha256(module).digest()
            esp, case = self.esp(pathlib.Path(directory), module, digest)
            self.assertEqual(Trust.lines('Amd64', ('ahci', 'xhci', 'e1000e'), esp, case), [
                'trust = pci=0000:00:1f.2 id=8086:2922 driver=ahci module=component sha256=' + digest.hex(),
                'trust = pci=0000:00:08.0 id=1b36:000d driver=xhci module=component sha256=' + digest.hex(),
                'trust = pci=0000:00:10.0 driver=e1000e module=component sha256=' + digest.hex()])
            self.assertEqual(Trust.lines('Ia64', ('ahci', 'e1000e'), esp, case),
                             ['trust = id=8086:2922 driver=ahci module=component sha256=' + digest.hex(),
                              'trust = pci=0000:00:10.0 id=8086:10d3 driver=e1000e module=component sha256='
                              + digest.hex()])
            self.assertEqual(Trust.lines('Amd64', (), esp, case), [])

    def test_the_usb_driver_names_its_own_module_once_per_function(self):
        with tempfile.TemporaryDirectory() as directory:
            component = b'\x7fELF component'
            usb = b'\x7fELF usb'
            esp, case = self.esp(pathlib.Path(directory), component, hashlib.sha256(component).digest())
            (esp / 'EFI' / 'ASTERIA' / 'MODULES' / 'USB.ELF').write_bytes(usb)
            digests = esp.parent / Trust.DIGESTS
            digests.write_text(digests.read_text() + 'usb USB.ELF sha256=%s\n' % hashlib.sha256(usb).hexdigest())
            digest = hashlib.sha256(usb).hexdigest()
            self.assertEqual(Trust.lines('Amd64', ('usb',), esp, case), [
                'trust = pci=0000:00:1d.7 driver=usb module=usb sha256=' + digest,
                'trust = pci=0000:00:1d.0 driver=usb module=usb sha256=' + digest,
                'trust = pci=0000:00:1d.1 driver=usb module=usb sha256=' + digest])
            self.assertEqual([driver for driver, _ in Trust.entries('Amd64', ('ahci', 'usb'))],
                             ['ahci', 'usb', 'usb', 'usb'])

    def test_a_digest_that_is_not_the_module_s_is_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            esp, case = self.esp(pathlib.Path(directory), b'module', bytes(32))
            with self.assertRaises(ValueError):
                Trust.lines('Amd64', ('ahci',), esp, case)


def boot(working: bool, trusted: bool = True) -> list[str]:
    """The lines of a q35 boot that trusts the AHCI controller and the xHCI."""
    decision = 'UNUSED-IOMMU-CONFINES' if working else 'TRUSTED-NO-DMA-ISOLATION'
    suffix = '' if working or not trusted else ' (TRUSTED: no DMA isolation)'
    lines = ['USER:# devices: 0000:00:1f.2 8086:2922 class=01/06/01 SATA AHCI -> ahci driver' + suffix,
             'USER:# devices: 0000:00:08.0 1b36:000d class=0c/03/30 USB -> xhci driver' + suffix
             + ' publication=published step=0 err=0x00']
    if not trusted:
        return lines
    return lines + [
        'ASTERIA:BOOT-TRUST:1 pci=0000:00:1f.2 id=8086:2922 driver=ahci module=component sha256=00 VERIFIED',
        'ASTERIA:BOOT-TRUST:2 pci=0000:00:08.0 id=1b36:000d driver=xhci module=component sha256=00 VERIFIED',
        'ASTERIA:DMA-TRUST:IOMMU:' + ('WORKING' if working else 'ABSENT'),
        'ASTERIA:DMA-TRUST:0000:00:1f.2:ahci:ENTRY-1:' + decision,
        'ASTERIA:DMA-TRUST:0000:00:08.0:xhci:ENTRY-2:' + decision]


REPORT = ('DMA trust: 2 BOOT.CFG entries; ...\r\n'
          'ASTERIA:BOOT-TRUST:1 pci=0000:00:1f.2 id=8086:2922 driver=ahci module=component sha256=00 VERIFIED'
          ' matches 0000:00:1f.2\r\n'
          'ASTERIA:BOOT-TRUST:2 pci=0000:00:08.0 id=1b36:000d driver=xhci module=component sha256=00 VERIFIED'
          ' matches 0000:00:08.0\r\n')


class TrustedJudgementTests(unittest.TestCase):
    def case(self, iommu: str, trust=('ahci', 'xhci')):
        return SimpleNamespace(isa='Amd64', iommu=iommu, trust=trust)

    def test_applied_unused_and_absent_reports_pass(self):
        self.assertEqual(Trusted.problems(self.case('None'), boot(False), REPORT), [])
        self.assertEqual(Trusted.problems(self.case('Default'), boot(True), REPORT), [])
        self.assertEqual(Trusted.problems(self.case('None', ()), boot(False, trusted=False), 'PCI ...\r\n'), [])

    def test_missing_or_wrong_reports_are_problems(self):
        lines = boot(False)
        for label, mutant, report in (
                ('no loader line', [line for line in lines if 'BOOT-TRUST:1' not in line], REPORT),
                ('digest mismatch', [line.replace('VERIFIED', 'DIGEST-MISMATCH') for line in lines], REPORT),
                ('no kernel decision', [line for line in lines if 'ENTRY-2' not in line], REPORT),
                ('no IOMMU line', [line for line in lines if 'IOMMU:ABSENT' not in line], REPORT),
                ('no suffix', [line.replace(' (TRUSTED: no DMA isolation)', '') for line in lines], REPORT),
                ('report', lines, REPORT.replace('matches 0000:00:08.0', 'matches no PCI function'))):
            with self.subTest(label=label):
                self.assertTrue(Trusted.problems(self.case('None'), mutant, report))
        self.assertTrue(Trusted.problems(self.case('Default'), boot(False), REPORT))
        self.assertTrue(Trusted.problems(self.case('None', ()), boot(False), None))


if __name__ == '__main__':
    unittest.main()
