# SPDX-License-Identifier: Apache-2.0
"""The boot AML oracle checks device-local resources and firmware table order."""
import pathlib
import tempfile
import unittest

from AsteriaVm import AmlFacts


def facts(root):
    return ('''Asteria AML facts v1
_PIC(1)=known
device \\_SB_.PCI0
  _HID=[%s] _CID=[]
  _STA=present keyboard=no mouse=no pci-root=yes
  _PRT=128 entries
device \\_SB_.PCI0.KBD_
  _HID=[PNP0303]
  _STA=present keyboard=yes mouse=no pci-root=no
  resource IO min=0x60 max=0x60 length=0x1
  resource IO min=0x64 max=0x64 length=0x1
  resource IRQ mask=0x2 flags=0x0
device \\_SB_.PCI0.MOU_
  _HID=[PNP0F13]
  _STA=present keyboard=no mouse=yes pci-root=no
  resource IRQ mask=0x1000 flags=0x0
_S5=0,0
''' % root).encode('ascii')


class AmlFactsTests(unittest.TestCase):
    def test_device_local_identity_and_resource_mutants(self):
        for isa, root in (('Amd64', 'PNP0A08'), ('Ia64', 'PNP0A03')):
            data = facts(root)
            if isa == 'Ia64':
                data = data.replace(b'_STA=present keyboard=yes', b'_STA=absent keyboard=yes').replace(
                    b'_STA=present keyboard=no mouse=yes', b'_STA=absent keyboard=no mouse=yes')
            self.assertEqual(AmlFacts.identity_problems(data, isa), [])
            for before, after in ((b'PNP0303', b'PNP030B'), (b'PNP0F13', b'PNP0F00'),
                                  (b'min=0x64', b'min=0x65'), (b'mask=0x2 ', b'mask=0x4 '),
                                  (b'mask=0x1000', b'mask=0x800'), (b'pci-root=yes', b'pci-root=no'),
                                  (b'_STA=present', b'_STA=absent'), (b'_PRT=128', b'_PRT=127')):
                with self.subTest(isa=isa, fault=before):
                    self.assertTrue(AmlFacts.identity_problems(data.replace(before, after), isa))
            with self.subTest(isa=isa, fault='PS/2 presence'):
                before = b'_STA=present keyboard=yes' if isa == 'Amd64' else b'_STA=absent keyboard=yes'
                after = b'_STA=absent keyboard=yes' if isa == 'Amd64' else b'_STA=present keyboard=yes'
                self.assertTrue(AmlFacts.identity_problems(data.replace(before, after), isa))
            # Correct strings/resources in different devices cannot substitute.
            self.assertTrue(AmlFacts.identity_problems(data.replace(b'PNP0303', b'PNP0F13').replace(
                b'PNP0F13]\n  _STA=present keyboard=no', b'PNP0303]\n  _STA=present keyboard=no'), isa))

    def test_ssdts_follow_report_order_with_numeric_dump_names(self):
        with tempfile.TemporaryDirectory() as directory:
            esp = pathlib.Path(directory)
            dump = esp / 'EFI/ASTERIA/ACPI'
            dump.mkdir(parents=True)
            signatures = ['DSDT'] + ['SSDT'] * 12
            report = []
            for index, signature in enumerate(signatures):
                name = signature + (str(index) if index else '') + '.AML'
                (dump / name).touch()
                report.append('ACPI %s OEM-table=ASTERIA  address=0x%016x length=36 revision=2 checksum=valid'
                              % (signature, index + 4096))
            paths = AmlFacts.definition_blocks(esp, '\r\n'.join(report))
            self.assertEqual([p.name for p in paths], ['DSDT.AML'] + ['SSDT%d.AML' % i for i in range(1, 13)])
