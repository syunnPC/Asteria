# SPDX-License-Identifier: Apache-2.0
import unittest

from AsteriaVm import Inventory


class InventoryTests(unittest.TestCase):
    def test_report_and_console_match_qemu_and_reject_wrong_ids_classes_or_omissions(self):
        query = [{'devices': [{'bus': 0, 'slot': 31, 'function': 2,
                 'id': {'vendor': 0x8086, 'device': 0x2922}, 'class_info': {'class': 0x0106}}]}]
        report = 'PCI 0000:00:1f.2 8086:2922 class=01/06/01 SATA AHCI support=ahci driver\r\n'
        console = report.replace('PCI ', '# devices: ')
        self.assertEqual(Inventory.pci_problems(report, query), [])
        self.assertEqual(Inventory.pci_problems(console, query), [])
        for bad in ('', report.replace('8086', '8087'), report.replace('01/06', '01/05'), report + report,
                    report.replace('0000:', '0001:')):
            self.assertTrue(Inventory.pci_problems(bad, query))

    def test_large_xhci_snapshot_requires_every_register_and_high_hid_roots(self):
        registers = ('CAPLENGTH', 'HCIVERSION', 'HCSPARAMS1', 'HCSPARAMS2', 'HCSPARAMS3',
                     'HCCPARAMS1', 'HCCPARAMS2', 'DBOFF', 'RTSOFF', 'PAGESIZE', 'USBSTS')
        lines = ['  XHCI ' + key + '=0x' + ('1e000010' if key == 'HCSPARAMS1' else '00000000')
                 for key in registers]
        lines += ['  XHCI legacy absent',
                  '  XHCI protocol revision=3.00 port-offset=1 port-count=15',
                  '  XHCI protocol revision=2.00 port-offset=16 port-count=15',
                  '  XHCI port=29 PORTSC=0x00000e03', '  XHCI port=30 PORTSC=0x00000e03']
        text = '\n'.join(lines)
        self.assertEqual(Inventory.xhci_report_problems(text, True), [])
        for omitted in lines:
            self.assertTrue(Inventory.xhci_report_problems(text.replace(omitted, ''), True))
        self.assertTrue(Inventory.xhci_report_problems(text.replace('1e000010', '08000010'), True))
