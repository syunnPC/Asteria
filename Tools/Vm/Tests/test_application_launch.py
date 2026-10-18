# SPDX-License-Identifier: Apache-2.0
"""Evidence guards for component launch authority and quota refusals."""
import copy
from pathlib import Path
import tempfile
import unittest
from AsteriaVm import Evidence, Machine, ObjectLimits, Scenarios
from AsteriaVm.Observers import ApplicationLaunch

ROOT = Path(__file__).resolve().parents[3]


class ApplicationLaunchTests(unittest.TestCase):
    def test_account_refusals_are_required_and_unrelated_refusals_are_rejected(self):
        record = Evidence.Record(Published=1, Created=0, TagRefusals=0, CodeFlagRefusals=1,
                                 TypeLimitRefusals=[0, 0, 0, 0, 1, 0, 0, 0])
        self.assertEqual(ApplicationLaunch.problems(record, ROOT, 1), [])
        for name in ('Published', 'Created', 'TagRefusals', 'CodeFlagRefusals'):
            changed = copy.deepcopy(record)
            changed[name] += 1
            self.assertTrue(ApplicationLaunch.problems(changed, ROOT, 1), name)
        for index in range(8):
            changed = copy.deepcopy(record)
            changed.TypeLimitRefusals[index] += 1
            self.assertTrue(ApplicationLaunch.problems(changed, ROOT, 1), index)

    def test_the_native_layout_reads_the_shared_capacity_table(self):
        for kind, count in (('AddressSpace', 16), ('Frame', 4096), ('MemoryObject', 4096), ('FaultChannel', 4)):
            declaration = 'Capacity{Abi::ObjectPoolCapacities[static_cast<Foundation::Size>(Abi::LimitedObject::%s)]}' % kind
            self.assertEqual(ObjectLimits.capacity(ROOT, declaration, 'Capacity'), count)
        self.assertIsNone(ObjectLimits.capacity(ROOT, 'Capacity{7U}', 'Capacity'))

    def test_fault_supervisor_activation_keeps_the_module_count_and_fits_the_boot_name(self):
        for isa in ('Amd64', 'Ia64'):
            case, = Scenarios.select([], [isa + '.FaultChannel'])
            self.assertTrue(case.fault_channel)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / 'Esp/EFI/ASTERIA/BOOT.CFG'
            manifest.parent.mkdir(parents=True)
            original = 'version = 1\nmodule = face-probe PROBE.ELF 1 0\n'
            manifest.write_text(original)
            variables = root / 'Variables.fd'
            variables.write_bytes(b'variables')
            environment = Machine.Environment('', '', '', 'qemu', 'code', str(variables))
            launch = Machine.prepare('Amd64', root / 'Esp', root / 'Run', 1, '', environment, fault_channel=True)
            try:
                activated = (root / 'Run/Esp/EFI/ASTERIA/BOOT.CFG').read_text()
                self.assertIn('module = fault-channel ', activated)
                self.assertEqual(activated.count('module = '), original.count('module = '))
                self.assertEqual(manifest.read_text(), original)
                self.assertLess(len('fault-channel'), 16)
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug]:
                    peer.close()
