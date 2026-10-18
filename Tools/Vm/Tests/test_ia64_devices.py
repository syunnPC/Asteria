# SPDX-License-Identifier: Apache-2.0
"""Processor, PCI and AHCI host checks (Observers.Ia64Devices)."""
from __future__ import annotations
from Tests.test_ia64_fixtures import fixture_schema

import copy
import pathlib
import tempfile
import unittest

from AsteriaVm import Machine
from AsteriaVm.Observers import Ia64Devices, Ia64Evidence
from Tests.test_ia64_fixtures import ROOT, evidence_bytes, observation


class ProcessorTests(unittest.TestCase):
    def test_counts(self):
        cpus = observation('Smp4').cpus
        self.assertEqual(Ia64Devices.processor_problems(4, cpus, 4), [])
        self.assertTrue(Ia64Devices.processor_problems(3, cpus, 4))       # the kernel lost a CPU
        self.assertTrue(Ia64Devices.processor_problems(4, cpus[:3], 4))   # QEMU has fewer than the case
        self.assertTrue(Ia64Devices.processor_problems(None, cpus, 4))


class PciTests(unittest.TestCase):
    def setUp(self):
        self.pci = observation('Smp4').pci
        record = fixture_schema().decode(evidence_bytes('Smp4'))
        self.ecam, self.discovery = Ia64Evidence.pci_facts(record.Pci)

    def test_recorded(self):
        functions = Ia64Devices.pci_functions(self.pci)
        # Fixed slots preserve distinct IOSAPIC INTx lines for edu, the IOMMU
        # and the e1000e.
        self.assertEqual([function.describe() for function in functions], [
            '0000:00:01.0 id 0x29228086 class 0106',
            '0000:00:02.0 id 0x003f106b class 0c03',
            '0000:00:03.0 id 0x70208086 class 0c03',
            '0000:00:04.0 id 0x00121000 class 0100',
            '0000:00:05.0 id 0x50461002 class 0300',
            '0000:00:06.0 id 0x11e81234 class 00ff',
            '0000:00:07.0 id 0x10571af4 class 00ff',
            '0000:00:10.0 id 0x10d38086 class 0200',
        ])
        self.assertEqual(Ia64Devices.ecam_problems(self.ecam, self.pci), [])
        self.assertEqual(Ia64Devices.discovery_problems(self.discovery, self.pci), [])

    def test_kernel_facts_that_disagree_with_qemu(self):
        address = Ia64Devices.PciAddress(0, 0, 2, 0)
        for fact in (Ia64Devices.PciEcamFact(address, self.ecam.identifier),
                     Ia64Devices.PciEcamFact(self.ecam.address, 0x29238086)):
            self.assertTrue(Ia64Devices.ecam_problems(fact, self.pci), fact)
        base = self.discovery
        for fact in (Ia64Devices.PciDiscoveryFact(4, base.ahci, base.identifier, base.class_code),
                     Ia64Devices.PciDiscoveryFact(base.functions, address, base.identifier, base.class_code),
                     Ia64Devices.PciDiscoveryFact(base.functions, base.ahci, 0x29238086, base.class_code),
                     Ia64Devices.PciDiscoveryFact(base.functions, base.ahci, base.identifier, (1, 6, 0))):
            self.assertTrue(Ia64Devices.discovery_problems(fact, self.pci), fact)

    def test_qemu_views_the_kernel_must_not_accept(self):
        without_ahci = copy.deepcopy(self.pci)
        without_ahci[0]['devices'] = [device for device in without_ahci[0]['devices']
                                      if device['class_info']['class'] != 0x0106]
        self.assertTrue(Ia64Devices.discovery_problems(self.discovery, without_ahci))
        self.assertTrue(Ia64Devices.ecam_problems(self.ecam, []))


class ReadBufferTests(unittest.TestCase):
    def test_buffer_and_fixture(self):
        buffer = observation('Ahci').memory['AhciReadBuffer']
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / 'Disk.raw'
            written = Machine.write_disk_fixture(path)
            sector, sectors = Ia64Devices.fixture_geometry(path, 8)
            self.assertEqual((sector, sectors), (written, 4096))
            with self.assertRaises(ValueError):
                Ia64Devices.fixture_geometry(path, 4096)
        self.assertEqual(Ia64Devices.read_buffer_problems(buffer, sector), [])
        for mutant in (b'\x00' + buffer[1:], buffer[:-1], bytes(512), b'\xa5' * 512):
            self.assertTrue(Ia64Devices.read_buffer_problems(mutant, sector))


if __name__ == '__main__':
    unittest.main()
