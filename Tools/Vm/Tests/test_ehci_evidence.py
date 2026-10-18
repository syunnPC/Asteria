# SPDX-License-Identifier: Apache-2.0
"""The synthetic-RMRR machine and the evidence boot's checks
(Documentation/Architecture/IommuDomain.md)."""
import pathlib
import struct
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'Tools' / 'Verify'))

from AsteriaVm import Dmar, Machine, Scenarios
from AsteriaVm.Drivers import EhciEvidence


class DmarTests(unittest.TestCase):
    def test_table_reserves_both_q35_ehci_functions(self):
        table = Dmar.table()
        self.assertEqual(table[:4], b'DMAR')
        self.assertEqual(struct.unpack_from('<I', table, 4)[0], len(table))
        self.assertEqual(sum(table) & 0xFF, 0)
        self.assertEqual(table[36], 38)
        kind, length, flags, _, segment, base = struct.unpack_from('<HHBBHQ', table, 48)
        self.assertEqual((kind, length, flags, segment, base), (0, 16, 1, 0, Dmar.REGISTER_BASE))
        kind, length, _, segment, rmrr_base, rmrr_limit = struct.unpack_from('<HHHHQQ', table, 64)
        self.assertEqual((kind, length, segment, rmrr_base, rmrr_limit),
                         (1, 40, 0, Dmar.RMRR_BASE, Dmar.RMRR_LIMIT))
        self.assertEqual(table[88:], bytes([1, 8, 0, 0, 0, 0, 0x1A, 0, 1, 8, 0, 0, 0, 0, 0x1D, 0]))

    def test_bootservices_reservation_has_no_live_firmware_unit(self):
        table = Dmar.boot_services_table()
        self.assertEqual(sum(table) & 0xFF, 0)
        kind, length, _, segment, base, limit = struct.unpack_from('<HHHHQQ', table, 48)
        self.assertEqual((kind, length, segment, base, limit),
                         (1, 32, 0, Dmar.BOOTSERVICES_RMRR_BASE, Dmar.BOOTSERVICES_RMRR_LIMIT))
        case, = Scenarios.select([], ['Amd64.MemoryRmrrBootServices'])
        self.assertEqual((case.graphics_device, case.iommu), ('', 'None'))
        self.assertIn('Amd64', case.suites)

    def test_unaligned_ranges_are_refused(self):
        with self.assertRaises(ValueError):
            Dmar.table(rmrr_base=0xFEFF_C800)
        with self.assertRaises(ValueError):
            Dmar.table(rmrr_limit=0xFEFF_EFFE)
        with self.assertRaises(ValueError):
            Dmar.table(register_base=0xFED9_0010)

    def test_machine_attaches_both_ehcis_and_the_table_without_an_iommu(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'vars.fd'
            variables.write_bytes(b'')
            environment = Machine.Environment('ia64', 'firmware', 'bios', 'amd64', 'code', str(variables))
            launch = Machine.prepare('Amd64', esp, root / 'Run', 1, '', environment, synthetic_rmrr=True)
            for peer in (launch.qmp, launch.debug, *launch.inherited):
                peer.close()
            argv = ' '.join(launch.argv)
            self.assertIn('ich9-usb-ehci2,id=ehci2,addr=1a.0', argv)
            self.assertIn('ich9-usb-ehci1,id=ehci1,addr=1d.0', argv)
            self.assertNotIn('intel-iommu', argv)
            self.assertNotIn('amd-iommu', argv)
            self.assertEqual((root / 'Run/Dmar.aml').read_bytes(), Dmar.table())
            self.assertIn('file=%s' % (root / 'Run/Dmar.aml'), launch.argv)
            with self.assertRaises(ValueError):
                Machine.prepare('Amd64', esp, root / 'Other', 1, '', environment, device=True, synthetic_rmrr=True)

    def test_the_case_is_in_the_usb_suite(self):
        case = next(case for case in Scenarios.CASES if case.name == 'Amd64.EhciRmrrEvidence')
        self.assertTrue(case.synthetic_rmrr)
        self.assertEqual((case.driver, case.firmware), ('EhciEvidence', Scenarios.PRODUCTION_FIRMWARE))
        self.assertIn('Amd64UsbHid', case.suites)


class QuarantineTests(unittest.TestCase):
    def test_kernel_lines(self):
        lines = ['ASTERIA:USB:SKIPPED:001A0:5:00', 'ASTERIA:USB:SKIPPED:001D0:5:00']
        self.assertEqual(EhciEvidence.kernel_problems(lines), [])
        self.assertEqual(EhciEvidence.kernel_problems(lines[1:]), ["no 'ASTERIA:USB:SKIPPED:001A0:5:00'"])
        self.assertEqual(EhciEvidence.kernel_problems(lines + ['ASTERIA:USB:PUBLISHED:001A0']),
                         ['reserved EHCI 00:1a.0 was published'])

    def test_report_section(self):
        report = '\n'.join([
            'RMRR on 00:1a.0 mask=0x01', 'RMRR on 00:1d.0 mask=0x01',
            'EHCI-RMRR 0000:00:1a.0 8086:293c PMCSR=none command=0x0007 MSE=1 BME=1 BAR0=0x81012000 bytes=0x1000',
            '  CAPLENGTH=0x20 HCIVERSION=0x0100 HCSPARAMS=0x00002206 N_PORTS=6 N_PCC=2 N_CC=2 HCCPARAMS=0x00006880 '
            'EECP=0x68',
            '  USBLEGSUP@0x68=0x00000001 BIOS-owned=0 OS-owned=0 USBLEGCTLSTS=0xc0000000 SMI-enables=0x0000 '
            'SMI-status=0xc0000000',
            '  USBCMD=0x00080031 RS=1 PSE=1 ASE=1 USBSTS=0x0000c000 HCHalted=0 PSS=1 ASS=1 CONFIGFLAG=0x00000001',
            '  SPECIAL_SMI unknown layout for 8086:293c',
            '  RMRR#0 0x00000000feffc000-0x00000000feffffff UEFI map: gap 0x00000000feffc000-0x00000000feffffff',
            'EHCI-RMRR 0000:00:1d.0 8086:293a PMCSR=none command=0x0007 MSE=1 BME=1 BAR0=0x81011000 bytes=0x1000',
            '  SPECIAL_SMI unknown layout for 8086:293a',
            'PCH SMI_EN LPC 8086:2918 unknown layout'])
        self.assertEqual(EhciEvidence.report_problems(report), [])
        self.assertEqual(len(EhciEvidence.report_problems(report.replace('8086:293c', 'ffff:ffff'))), 2)
        self.assertEqual(EhciEvidence.report_problems(None),
                         ['EFI/ASTERIA/REPORT.TXT was not exported to the case ESP'])


if __name__ == '__main__':
    unittest.main()
