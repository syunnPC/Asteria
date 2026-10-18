# SPDX-License-Identifier: Apache-2.0
"""AMD-Vi profile and raw event expectations, independent of old recordings."""
import pathlib
import tempfile
import unittest
from types import SimpleNamespace

from AsteriaVm import Machine, Scenarios
from AsteriaVm.Observers import Amd64Evidence


class AmdViTests(unittest.TestCase):
    def test_case_keeps_the_intel_matrix_unchanged(self):
        cases = Scenarios.select(['Amd64AmdVi'], [])
        self.assertEqual(len(cases), 1)
        self.assertEqual((cases[0].name, cases[0].preset, cases[0].disk, cases[0].iommu),
                         ('Amd64.AmdVi', 'amd64', 'raw', 'AmdVi'))
        # The authored RMRR probe admits BootServices without a translating
        # unit; every existing Intel case retains its default backend.
        self.assertEqual([(case.name, case.iommu) for case in Scenarios.select(['Amd64'], [])
                          if case.iommu != 'Default'], [('Amd64.MemoryRmrrBootServices', 'None')])

    def test_profile_pins_the_pci_function_and_enables_dma(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'Variables.fd'
            variables.write_bytes(b'variables')
            environment = Machine.Environment('unused', 'unused', 'unused', '/usr/bin/qemu-system-x86_64',
                                              'unused', str(variables))
            launch = Machine.prepare('Amd64', esp, root / 'Run', 1, '', environment,
                                     device=True, iommu='AmdVi')
            try:
                self.assertIn('AMDVI-PCI,id=amdvi,addr=06.0', launch.argv)
                self.assertIn('amd-iommu,pci-id=amdvi,dma-remap=on,intremap=off', launch.argv)
                self.assertNotIn('intel-iommu,intremap=off', launch.argv)
            finally:
                for connection in [launch.qmp, launch.debug, launch.lan, *launch.inherited]:
                    connection.close()

    def test_backend_kind_and_ivrs_mutants_are_rejected(self):
        scenario = Amd64Evidence.IommuScenario(present=True, amd_vi=True)
        backend = SimpleNamespace(Published=1, Kind=2, MalformedIvrs=0, SkippedHid=0, IvhdType=0x10)
        self.assertEqual(Amd64Evidence.iommu_backend_problems(backend, scenario), [])
        for field, value in [('Kind', 1), ('MalformedIvrs', 1), ('IvhdType', 0x20), ('Published', 0)]:
            changed = SimpleNamespace(**vars(backend))
            setattr(changed, field, value)
            self.assertTrue(Amd64Evidence.iommu_backend_problems(changed, scenario), field)

    def test_fault_observer_accepts_emulator_variations_and_rejects_missing_or_wrong_source(self):
        scenario = Amd64Evidence.IommuScenario(present=True, amd_vi=True, functions=8, swept=8, driver=8)
        facts = SimpleNamespace(Published=1, SelectionError=0, ConfigureError=0, EnableError=0,
                                Enabled=1, ServedFunctions=8, SweptFunctions=8, Faults=1,
                                Unattributed=0, Overflows=0, DomainsCreated=1, DomainsReclaimed=1,
                                LastFaultSource=8, LastFaultAddress=0x200000, LastFaultRead=1,
                                LastFaultReason=2, Levels=4)
        for count in (1, 7, 64, 256):
            facts.Faults = count
            self.assertEqual(Amd64Evidence.iommu_problems(facts, scenario), [])
        facts.Faults = 0
        self.assertTrue(Amd64Evidence.iommu_problems(facts, scenario))
        facts.Faults = 1
        facts.LastFaultSource = 9
        self.assertTrue(Amd64Evidence.iommu_problems(facts, scenario))
