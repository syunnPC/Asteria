# SPDX-License-Identifier: Apache-2.0
"""Publication diagnostics and the authored floating-port runner seam."""
import pathlib
import tempfile
import types
import unittest

from AsteriaVm import I8042Evidence, Machine, Scenarios


class EvidenceTests(unittest.TestCase):
    def outcome(self, name, lines, synthetic='0'):
        case = next(case for case in Scenarios.CASES if case.name == name)
        checks = []
        target = types.SimpleNamespace(option={'ia64_synthetic_user_probe': synthetic}.__getitem__)
        context = types.SimpleNamespace(case=case, console=types.SimpleNamespace(lines=lines), target=target,
                                        check=lambda *args: checks.append(args))
        I8042Evidence.record(context)
        return checks

    def test_aml_requires_the_aml_publication_line(self):
        self.assertTrue(self.outcome('Amd64.AmlI8042', ['ASTERIA:I8042:PUBLISHED:AML-PNP0303'])[0][1])
        self.assertFalse(self.outcome('Amd64.AmlI8042', ['ASTERIA:I8042:PUBLISHED:FADT-8042'])[0][1])

    def test_floating_evidence_needs_the_specific_reason(self):
        lines = ['ASTERIA:I8042:UNPUBLISHED', 'ASTERIA:I8042:REASON:port-0x64-floating']
        self.assertTrue(self.outcome('Amd64.FloatingI8042', lines)[0][1])
        self.assertFalse(self.outcome('Amd64.FloatingI8042', lines[:1])[0][1])
        self.assertFalse(self.outcome('Amd64.FloatingI8042',
                                     [lines[0], 'ASTERIA:I8042:REASON:no-evidence'])[0][1])

    def test_ia64_uses_the_same_evidence_and_gate(self):
        self.assertTrue(self.outcome('Ia64.Ps2', ['ASTERIA:I8042:PUBLISHED:AML-PNP0303'])[0][1])
        self.assertFalse(self.outcome('Ia64.Ps2', ['ASTERIA:I8042:PUBLISHED:FADT-8042'])[0][1])
        self.assertTrue(self.outcome('Ia64.Production', ['ASTERIA:I8042:PUBLISHED:FADT-8042'])[0][1])
        self.assertFalse(self.outcome('Ia64.Production', ['ASTERIA:I8042:UNPUBLISHED'])[0][1])
        for name in ('Ia64.Smp4', 'Ia64.Aml'):
            self.assertTrue(self.outcome(name,
                ['ASTERIA:I8042:UNPUBLISHED', 'ASTERIA:I8042:REASON:no-evidence'])[0][1])
            self.assertFalse(self.outcome(name, ['ASTERIA:I8042:UNPUBLISHED'])[0][1])
        # Synthetic probes start no pager, so they make no publication claim.
        self.assertEqual(self.outcome('Ia64.SyntheticProbe', [], synthetic='1'), [])
        self.assertEqual(self.outcome('Ia64.NativePreemption', [], synthetic='1'), [])

    def test_namespace_case_prepares_marker_and_authored_ssdt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'vars.fd'
            variables.write_bytes(b'')
            environment = Machine.Environment('ia64', 'firmware', 'bios', 'amd64', 'code', str(variables))
            launch = Machine.prepare('Amd64', esp, root / 'Run', 1, '', environment,
                                     i8042=False, fadt8042=False, floating_keyboard=True)
            try:
                self.assertTrue((root / 'Run/Esp/EFI/ASTERIA/NOFADT8042').is_file())
                self.assertIn('q35,i8042=off', launch.argv)
                self.assertIn('tcg,thread=multi', launch.argv)
                self.assertIn('-acpitable', launch.argv)
                self.assertTrue(pathlib.Path(launch.argv[launch.argv.index('-acpitable') + 1][5:]).is_file())
            finally:
                for sock in (launch.qmp, launch.debug, *launch.inherited):
                    sock.close()
