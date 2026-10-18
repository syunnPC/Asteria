# SPDX-License-Identifier: Apache-2.0
"""Power evidence reads the FAT marker and injects only product key requests."""
import pathlib
import struct
import tempfile
import types
import unittest

from AsteriaVm.Drivers import Power
from AsteriaVm import Machine, Scenarios
from AsteriaVm.Qmp import QmpError


class PowerTests(unittest.TestCase):
    def test_power_off_can_close_qmp_before_key_release(self):
        for status in (0, 1):
            requests = []
            def send(command, args):
                requests.append(args['events'][0])
                if len(requests) == 4:
                    raise QmpError('closed during release')
            context = types.SimpleNamespace(qmp=types.SimpleNamespace(execute=send),
                case=Scenarios.select([], ['Amd64.PowerOff'])[0], seconds=lambda seconds: seconds,
                process=types.SimpleNamespace(wait=lambda **kwargs: status))
            if status == 0:
                Power.chord(context, 'f10', allow_exit=True)
            else:
                with self.assertRaises(QmpError):
                    Power.chord(context, 'f10', allow_exit=True)

    def test_only_reboot_cases_allow_a_second_boot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            environment = Machine.Environment('qemu-ia64', 'firmware', 'bios', '', '', '')
            for reboot in (False, True):
                launch = Machine.prepare('Ia64', esp, root / str(reboot), 4, '', environment, reboot=reboot)
                try:
                    self.assertEqual(launch.argv[launch.argv.index('-action') + 1],
                                     'reboot=reset' if reboot else 'reboot=shutdown')
                finally:
                    for peer in launch.inherited:
                        peer.close()
                    launch.qmp.close()
                    launch.debug.close()

    def test_fat_clean_marker_uses_reserved_sector_count(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'disk.raw'
            disk = bytearray(4096)
            struct.pack_into('<H', disk, 14, 4)
            struct.pack_into('<H', disk, 4 * 512 + 2, 0xffff)
            path.write_bytes(disk)
            self.assertTrue(Power.fat_clean(path))
            struct.pack_into('<H', disk, 4 * 512 + 2, 0x7fff)
            path.write_bytes(disk)
            self.assertFalse(Power.fat_clean(path))

    def test_keys_never_send_host_reset_or_powerdown(self):
        requests = []
        qmp = types.SimpleNamespace(execute=lambda command, args: requests.append((command, args)))
        context = types.SimpleNamespace(qmp=qmp, case=Scenarios.select([], ['Amd64.PowerOff'])[0])
        Power.chord(context, 'f10')
        self.assertEqual(requests[0][0], 'input-send-event')
        keys = [request[1]['events'][0] for request in requests]
        self.assertEqual([(e['data']['key']['data'], e['data']['down']) for e in keys],
                         [('ctrl', True), ('alt', True), ('f10', True),
                          ('f10', False), ('alt', False), ('ctrl', False)])

    def test_both_isas_cover_off_and_next_boot(self):
        cases = Scenarios.select(['Power'], [])
        self.assertEqual({(case.isa, case.power) for case in cases},
                         {('Amd64', 'off'), ('Amd64', 'reboot'), ('Ia64', 'off'), ('Ia64', 'reboot')})
        self.assertTrue(all(case.cpus == 4 and case.disk == 'fat16' for case in cases))
        self.assertTrue(all(case.i8042 or case.usb_host for case in cases))
