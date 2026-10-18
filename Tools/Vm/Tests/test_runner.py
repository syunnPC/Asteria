# SPDX-License-Identifier: Apache-2.0
"""Unit tests of the VM runner core (no QEMU needed).

Run with: python3 -m unittest discover -s Tools/Vm/Tests -t Tools/Vm
"""
from __future__ import annotations

import ctypes
import dataclasses
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock
import xml.etree.ElementTree as ElementTree

from AsteriaVm import Machine, Report, Scenarios, Tap
from AsteriaVm.Console import FAILURE, Console, ConsoleTimeout
from AsteriaVm.Executor import CaseResult, CheckResult, unexpected_watchdog_reports
from AsteriaVm.Qmp import Qmp, QmpError


class EnvironmentTests(unittest.TestCase):
    def test_unconfigured_host_uses_path_and_requires_explicit_ia64_firmware(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            environment = Machine.Environment.from_environment()
        self.assertEqual(environment.ia64_qemu, 'qemu-system-ia64')
        self.assertEqual(environment.amd64_qemu, 'qemu-system-x86_64')
        self.assertEqual(environment.ia64_bios_directory, '')
        with mock.patch.object(Machine.shutil, 'which', return_value='/tools/qemu'):
            with self.assertRaisesRegex(ValueError, 'set IA64_FIRMWARE'):
                environment.validate('Ia64')
            with self.assertRaisesRegex(ValueError, 'set IA64_VTD_FIRMWARE'):
                environment.validate('Ia64', 'itanium2-vpc-iommu')

    def test_selected_isa_validates_overrides_without_requiring_other_isa(self):
        with tempfile.TemporaryDirectory() as temporary:
            firmware = pathlib.Path(temporary) / 'ia64-firmware.bin'
            firmware.touch()
            with mock.patch.dict(os.environ, {'IA64_QEMU': sys.executable, 'IA64_FIRMWARE': str(firmware),
                                              'IA64_BIOS_DIRECTORY': temporary}, clear=True):
                environment = Machine.Environment.from_environment()
            environment.validate('Ia64')
            environment.validate('Ia64', 'hp-i2000')
            with self.assertRaisesRegex(ValueError, 'IA64_VTD_'):
                environment.validate('Ia64', 'itanium2-vpc-iommu')
            unavailable = dataclasses.replace(environment, ia64_bios_directory=str(firmware))
            with self.assertRaisesRegex(ValueError, 'set IA64_BIOS_DIRECTORY'):
                unavailable.validate('Ia64')

    def test_missing_ovmf_installation_requires_firmware_overrides(self):
        with mock.patch.dict(os.environ, {}, clear=True), mock.patch.object(pathlib.Path, 'is_file', return_value=False):
            environment = Machine.Environment.from_environment()
        with mock.patch.object(Machine.shutil, 'which', return_value='/tools/qemu'):
            with self.assertRaisesRegex(ValueError, 'set AMD64_FIRMWARE_CODE'):
                environment.validate('Amd64')

    def test_omitted_bios_directory_uses_qemu_search_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            environment = Machine.Environment('qemu', 'firmware', '', '', '', '')
            launch = Machine.prepare('Ia64', esp, root / 'Run', 1, '', environment)
            try:
                self.assertNotIn('-L', launch.argv)
                self.assertEqual(launch.argv[launch.argv.index('-bios') + 1], 'firmware')
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()


class WatchdogAuditTests(unittest.TestCase):
    def test_liveness_reports_fail_normal_cases_and_only_the_withholding_probe_is_exempt(self):
        idle = next(case for case in Scenarios.CASES if case.name == 'Ia64.SessionIdleSmp4')
        probe = next(case for case in Scenarios.CASES if case.driver == 'ComponentWatchdog')
        report = 'USER:# watchdog: session-compositor unresponsive for 5420 ms'
        self.assertEqual(unexpected_watchdog_reports(idle, ['USER:# session: presented 4']), [])
        self.assertEqual(unexpected_watchdog_reports(idle, [report]), [report])
        self.assertEqual(unexpected_watchdog_reports(probe, [report]), [])
        recovery = next(case for case in Scenarios.CASES if case.session_compositor_fault)
        self.assertEqual(unexpected_watchdog_reports(recovery, [report]), [])
        other = 'USER:# watchdog: display-broker unresponsive for 5000 ms'
        self.assertEqual(unexpected_watchdog_reports(recovery, [other]), [other])
        startup = 'USER:# startup: session-compositor not ready within 60000 ms'
        self.assertEqual(unexpected_watchdog_reports(idle, [startup]), [startup])

    def test_deliberate_machine_pause_keeps_startup_failures_visible(self):
        pause = next(case for case in Scenarios.CASES if case.name == 'Amd64Watchdog.PauseSession')
        report = 'USER:# watchdog: display-broker unresponsive for 7000 ms'
        startup = 'USER:# startup: session-compositor not ready within 60000 ms'
        # The pause driver separately bounds Query ages and requires resumed
        # input without a restart; its deliberate suspension is not a healthy boot.
        self.assertEqual(unexpected_watchdog_reports(pause, [report]), [])
        self.assertEqual(unexpected_watchdog_reports(pause, [startup]), [startup])


class TapTests(unittest.TestCase):
    def test_complete_passing_stream(self):
        stream = Tap.parse(['TAP version 14', 'ok 1 - bss.zero', 'ok 2 - query # SKIP no capability',
                            '# diagnostic', '1..2'])
        self.assertTrue(stream.complete)
        self.assertTrue(stream.passed, stream.problems())
        self.assertEqual(stream.points[1].directive, 'SKIP')
        self.assertEqual(stream.points[1].diagnostics, ['diagnostic'])

    def test_failures_are_reported_with_diagnostics(self):
        stream = Tap.parse(['TAP version 14', 'not ok 1 - ipc.send', '# expected 1 observed 0', '1..1'])
        self.assertFalse(stream.passed)
        self.assertIn('expected 1 observed 0', '\n'.join(stream.problems()))

    def test_malformed_streams_never_pass(self):
        for lines in (['ok 1', '1..1'],                                   # no version line
                      ['TAP version 14', 'ok 1', 'ok 3', '1..2'],          # numbering gap
                      ['TAP version 14', 'ok 1'],                          # no plan
                      ['TAP version 14', 'ok 1', '1..2'],                  # short
                      ['TAP version 14', '    ok 1 - subtest', '1..0'],   # subtest
                      ['TAP version 14', 'Bail out! no memory'],
                      ['TAP version 14', 'garbage', '1..0']):
            self.assertFalse(Tap.parse(lines).passed, lines)

    def test_bail_out_completes_the_stream(self):
        self.assertTrue(Tap.parse(['TAP version 14', 'Bail out!']).complete)

    def test_not_ok_fails_whatever_its_directive(self):
        for line in ('not ok 1 - a # TODO later', 'not ok 1 - a # SKIP not here'):
            stream = Tap.parse(['TAP version 14', line, '1..1'])
            self.assertFalse(stream.passed, line)
            self.assertFalse(stream.points[0].skipped, line)
            self.assertEqual(stream.structure_problems(), [], line)
            self.assertEqual(len(stream.problems()), 1, line)
        stream = Tap.parse(['TAP version 14', 'ok 1 - a # SKIP no WaitSet', 'ok 2 - b # TODO', '1..2'])
        self.assertEqual([point.skipped for point in stream.points], [True, False])
        self.assertTrue(stream.passed)


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.ours, self.theirs = socket.socketpair()
        self.directory = tempfile.TemporaryDirectory()
        self.console = Console(self.ours, pathlib.Path(self.directory.name) / 'Debug.log')

    def tearDown(self):
        self.theirs.close()
        self.console.close()
        self.directory.cleanup()

    def test_lines_are_framed_across_chunks_and_carriage_returns(self):
        # A firmware console serving as the channel ends lines with CR CR LF.
        self.theirs.sendall(b'ASTERIA:A:PA')
        self.theirs.sendall(b'SS\r\nUSER:ok 1\nfirmware text\nASTERIA:STAGE:BOOT\r\r\n')
        index, line = self.console.wait_for('USER:', 5)
        self.assertEqual((index, line), (1, 'USER:ok 1'))
        self.console.wait_for('ASTERIA:STAGE:BOOT', 5)
        self.assertEqual(self.console.kernel_lines(), ['ASTERIA:A:PASS', 'ASTERIA:STAGE:BOOT'])
        self.assertEqual(self.console.user_lines(), ['ok 1'])

    def test_kernel_failure_ends_a_wait(self):
        self.theirs.sendall(b'ASTERIA:PAGE-ALLOCATOR:FAIL\n')
        with self.assertRaises(ConsoleTimeout) as raised:
            self.console.wait_for('never', 5)
        self.assertIn('kernel failure', str(raised.exception))

    def test_nul_bytes_mark_the_line_corrupt(self):
        self.theirs.sendall(b'ASTERIA:X\0Y\n')
        self.console.wait_for('ASTERIA', 5)
        self.assertEqual(len(self.console.corrupt), 1)

    def test_timeout_and_abort(self):
        with self.assertRaises(ConsoleTimeout):
            self.console.wait_for('never', 0.2)
        with self.assertRaises(ConsoleTimeout) as raised:
            self.console.wait_for('never', 5, abort=lambda: 'QEMU exited with status 1')
        self.assertIn('QEMU exited', str(raised.exception))

    def test_failure_pattern(self):
        for line, expected in (('ASTERIA:PAGE-ALLOCATOR:FAIL', True),
                               ('ASTERIA:INITIAL-IVT-INSTALL-ERROR:TABLE-REJECTED', True),
                               ('ASTERIA:USER-EXECUTION-PROBE:FAIL:ERROR=0', True),
                               ('ASTERIA:AMD64-FAULT:RIP=0x1:ADDRESS=0x2:ERROR=0x3', True),
                               ('ASTERIA:AMD64-INITIAL-ADDRESS-SPACE:FAULT', True),
                               ('ASTERIA:AMD64-AHCI-IDENTIFY:NO-DISK', False),
                               ('ASTERIA:X:PASS:ERROR=0', False), ('USER:not ok 1 FAIL', False),
                               ('ASTERIA:STAGE:AP-RUNTIME', False),
                               ('ASTERIA:EVIDENCE:0x000000000432B110', False),
                               ('ASTERIA:IA64-SHARED-ROOT-SHOOTDOWN:FAIL', True)):
            self.assertEqual(bool(FAILURE.match(line)), expected, line)


class FakeQemu(threading.Thread):
    """Answers QMP commands on one end of a socket pair."""

    def __init__(self, peer: socket.socket, directory: pathlib.Path):
        super().__init__(daemon=True)
        self.peer = peer
        self.directory = directory
        self.running = True

    def run(self):
        stream = self.peer.makefile('rwb')

        def send(message):
            stream.write((json.dumps(message) + '\n').encode())
            stream.flush()

        send({'QMP': {'version': {}, 'capabilities': []}})
        for raw in stream:
            request = json.loads(raw)
            command, identifier = request['execute'], request.get('id')
            arguments = request.get('arguments', {})
            if command == 'query-status':
                send({'return': {'running': self.running, 'status': 'running' if self.running else 'paused'},
                      'id': identifier})
            elif command == 'stop':
                self.running = False
                send({'return': {}, 'id': identifier})
                send({'event': 'STOP', 'timestamp': {}})
            elif command in ('memsave', 'pmemsave'):
                pathlib.Path(arguments['filename']).write_bytes(b'\xab' * arguments['size'])
                send({'return': {}, 'id': identifier})
            elif command == 'bogus':
                send({'error': {'class': 'CommandNotFound', 'desc': 'no bogus'}, 'id': identifier})
            else:
                send({'return': {}, 'id': identifier})
                if command == 'quit':
                    break
        self.peer.close()


class QmpTests(unittest.TestCase):
    def test_commands_events_and_dumps(self):
        ours, theirs = socket.socketpair()
        with tempfile.TemporaryDirectory() as directory:
            fake = FakeQemu(theirs, pathlib.Path(directory))
            fake.start()
            qmp = Qmp(ours, scratch=pathlib.Path(directory))
            qmp.handshake(timeout=5)
            self.assertTrue(qmp.status()['running'])
            qmp.stop(timeout=5)
            self.assertFalse(qmp.status()['running'])
            self.assertEqual(qmp.memsave(0x1000, 16), b'\xab' * 16)
            self.assertEqual(qmp.pmemsave(0x2000, 8), b'\xab' * 8)
            with self.assertRaises(QmpError):
                qmp.execute('bogus')
            qmp.quit()
            fake.join(5)
            qmp.close()


class ReportTests(unittest.TestCase):
    def test_skipped_checks_pass_and_are_reported_as_skipped(self):
        case = next(case for case in Scenarios.CASES if case.name == 'Ia64.Smp1')
        result = CaseResult(case, pathlib.Path('/run/Ia64.Smp1'), [
            CheckResult('guest.1.bss.zero', True, 'bss.zero'),
            CheckResult('guest.2.waitset.wait', True, 'waitset.wait', 'this image receives no WaitSet')], None, 1.0)
        self.assertTrue(result.passed)
        self.assertEqual(Report.console_line(result), 'PASS Ia64.Smp1 (2 checks, 1 skipped, 1s)')
        with tempfile.TemporaryDirectory() as directory:
            junit, report = pathlib.Path(directory) / 'Junit.xml', pathlib.Path(directory) / 'Results.json'
            Report.write_junit([result], junit)
            Report.write_json([result], report)
            suite = ElementTree.parse(junit).getroot().find('testsuite')
            data = json.loads(report.read_text())
        self.assertEqual((suite.get('tests'), suite.get('failures'), suite.get('skipped')), ('2', '0', '1'))
        self.assertEqual([testcase.find('skipped') is not None for testcase in suite.findall('testcase')],
                         [False, True])
        self.assertEqual(data[0]['firmware'], Scenarios.TEST_FIRMWARE)
        self.assertEqual([check.get('skipped') for check in data[0]['checks']],
                         [None, 'this image receives no WaitSet'])


class ScenarioTests(unittest.TestCase):
    def test_suites_select_the_former_scenarios(self):
        # Product/display cases, the unavailable-services boundary case,
        # IA-64's PS/2 case and its four filesystem formats.
        self.assertEqual(len([case for case in Scenarios.select(['All'], []) if not case.product_desktop]), 56)
        self.assertEqual(len(Scenarios.select(['Amd64NativePreemption'], [])), 6)
        self.assertEqual([case.name for case in Scenarios.select(['Ia64Ahci'], [])],
                         ['Ia64.Smp1', 'Ia64.Ahci', 'Ia64.Fat12', 'Ia64.Fat16', 'Ia64.Fat32', 'Ia64.Exfat'])
        self.assertEqual([case.name for case in Scenarios.select([], ['Ia64.Smp*'])],
                         ['Ia64.Smp4', 'Ia64.Smp3', 'Ia64.Smp2', 'Ia64.Smp1'])
        with self.assertRaises(ValueError):
            Scenarios.select(['Nope'], [])

    def test_only_the_smoke_and_probe_cases_boot_the_production_firmware(self):
        production = {case.name for case in Scenarios.CASES if case.firmware==Scenarios.PRODUCTION_FIRMWARE and not case.product_desktop}
        self.assertEqual(production, {'Amd64.MemoryRmrrBootServices', 'Ia64.Production', 'Ia64I2000.Production', 'Ia64I2000.Smp2', 'Amd64.Production',
                                      'Ia64.SyntheticProbe',
                                      'Amd64.PowerOff', 'Amd64.PowerReboot', 'Ia64.PowerOff', 'Ia64.PowerReboot',
                                      'Ia64.NativePreemption', 'Amd64.XhciProduct', 'Amd64.XhciUsbProduct',
                                      'Amd64.XhciFailedProduct', 'Amd64.AmlProductMedia', 'Amd64.XhciUsbLargeProduct',
                                      'Amd64Trusted.Product', 'Amd64Trusted.Untrusted', 'Amd64Trusted.Vtd',
                                      'Amd64.UsbHostProduct', 'Amd64.UsbHostCompanion', 'Amd64.UsbHostEhci',
                                      'Amd64.UsbHostAmdVi', 'Amd64Trusted.UsbHost', 'Ia64.UsbHostNec',
                                      'Ia64.UsbHostIch9', 'Amd64.EhciRmrrEvidence', 'Amd64.UsbHostBesideXhci',
                                      *('Amd64Preemption.' + name for name in (
                                          'Smp1', 'Smp2', 'Smp3', 'Smp4', 'Ahci', 'Relocated'))})
        self.assertEqual({case.firmware for case in Scenarios.CASES if case.name in production},
                         {Scenarios.PRODUCTION_FIRMWARE})
        self.assertTrue(all(case.product_desktop for case in Scenarios.CASES if case.firmware=='firmware-login'))
        self.assertEqual({case.name for case in Scenarios.CASES if case.firmware=='firmware-login'}, {'Amd64.Login','Ia64.Login',*(isa+'.LoginFailure.'+service for isa in ('Amd64','Ia64') for service in ('auth','login','display'))})

    def test_disk_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'Disk.raw'
            sector = Machine.write_disk_fixture(path)
            data = path.read_bytes()
        self.assertEqual(len(data), Machine.DISK_SIZE_BYTES)
        self.assertEqual(data[8 * 512:9 * 512], sector)
        self.assertEqual(sector, Machine.DISK_TOKEN * 16)
        self.assertEqual(data[:8 * 512], bytes(8 * 512))


@unittest.skipUnless(sys.platform == 'linux' and shutil.which('setpriv'), 'needs Linux and util-linux setpriv')
class RunnerDeathTests(unittest.TestCase):
    # A helper starts `sleep` the way a runner case starts QEMU (from a worker
    # thread that then waits for it) and kills itself with SIGKILL, which runs
    # no handler. As child subreaper, the test adopts the orphan and sees how
    # it ended. The helper dies only once `setpriv` has executed `sleep`, so
    # after it set the parent-death signal: a parent that dies before that
    # call leaves the child unsignalled, and on a loaded host `setpriv` can
    # start that late.
    HELPER = """
import os, pathlib, signal, subprocess, threading, time
from AsteriaVm import Machine
started = threading.Event()
def case():
    process = subprocess.Popen(Machine.bound_to_runner(['sleep', '30']), stdout=subprocess.DEVNULL)
    comm = pathlib.Path('/proc/%d/comm' % process.pid)
    for _ in range(1000):
        if comm.read_text().strip() == 'sleep':
            break
        time.sleep(0.01)
    print(process.pid, flush=True)
    started.set()
    process.wait()
threading.Thread(target=case).start()
started.wait()
os.kill(os.getpid(), signal.SIGKILL)
"""
    PR_SET_CHILD_SUBREAPER = 36

    def test_a_killed_runner_takes_its_machines_with_it(self):
        libc = ctypes.CDLL(None, use_errno=True)
        self.assertEqual(libc.prctl(self.PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0), 0)
        try:
            helper = subprocess.run([sys.executable, '-c', self.HELPER], stdout=subprocess.PIPE, text=True,
                                    cwd=pathlib.Path(__file__).resolve().parents[1])
            self.assertEqual(helper.returncode, -signal.SIGKILL)
            _, status = os.waitpid(int(helper.stdout), 0)
        finally:
            libc.prctl(self.PR_SET_CHILD_SUBREAPER, 0, 0, 0, 0)
        self.assertTrue(os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGKILL, hex(status))


class Ia64IommuLaunchTests(unittest.TestCase):
    def test_ia64_uses_eight_lines_fixed_noncolliding_slots_and_an_explicit_fallback(self):
        from AsteriaVm import Machine
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            environment = Machine.Environment('qemu-ia64', 'firmware', 'bios', '', '', '')
            for enabled in (True, False):
                launch = Machine.prepare('Ia64', esp, root / str(enabled), 1, '', environment,
                                         device=True, iommu='Default' if enabled else 'None')
                try:
                    self.assertIn('itanium2-vpc,firmware-console=serial,nvram=none,pci-intx-lines=8,i8042=on', launch.argv)
                    self.assertIn('edu,addr=0x6', launch.argv)
                    self.assertEqual('virtio-iommu-pci,granule=4k,addr=0x7' in launch.argv, enabled)
                    self.assertNotEqual((6 + 0) % 8, (7 + 0) % 8)
                finally:
                    for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                        peer.close()
        fallback = Scenarios.select([], ['Ia64.NoIommu'])
        self.assertEqual(len(fallback), 1)
        self.assertEqual(fallback[0].iommu, 'None')
        self.assertTrue(fallback[0].guest)

    def test_vtd_cases_boot_the_branch_build_and_keep_the_default_machine(self):
        from AsteriaVm import Machine
        environment = Machine.Environment('qemu-ia64', 'firmware', 'bios', '', '', '',
                                          'qemu-vtd', 'firmware-vtd', 'bios-vtd')
        cases = {case.name: case for case in Scenarios.select(['Ia64Vtd'], [])}
        self.assertEqual(set(cases), {'Ia64Vtd.Edu', 'Ia64Vtd.Coherent', 'Ia64Vtd.HighMmio', 'Ia64Vtd.Ahci',
                                      'Ia64Vtd.Fat16', 'Ia64Vtd.Absent'})
        self.assertTrue(all(case.guest and case.isa == 'Ia64' for case in cases.values()))
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            for name, case in sorted(cases.items()):
                launch = Machine.prepare('Ia64', esp, root / name, 1, '', environment, device=True, iommu=case.iommu,
                                         i8042=case.i8042, machine=case.machine,
                                         machine_options=case.machine_options)
                try:
                    argv = launch.argv
                    self.assertEqual((argv[0], argv[argv.index('-bios') + 1], argv[argv.index('-L') + 1]),
                                     ('qemu-vtd', 'firmware-vtd', 'bios-vtd'))
                    self.assertTrue(argv[argv.index('-machine') + 1].startswith(case.machine + ','))
                    self.assertIn(case.machine_options, argv[argv.index('-machine') + 1])
                    self.assertIn('i8042=off', argv[argv.index('-machine') + 1])
                    self.assertEqual(argv[argv.index('-cpu') + 1],
                                     'tukwila-9350' if case.machine == 'itanium2-vpc-iommu' else 'montecito')
                    self.assertNotIn('virtio-iommu-pci,granule=4k,addr=0x7', argv)
                    self.assertIn('edu,addr=0x6', argv)
                finally:
                    for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                        peer.close()
            default = Machine.prepare('Ia64', esp, root / 'Default', 1, '', environment, device=True)
            try:
                self.assertEqual(default.argv[0], 'qemu-ia64')
                self.assertIn('itanium2-vpc,firmware-console=serial,nvram=none,pci-intx-lines=8,i8042=on', default.argv)
            finally:
                for peer in default.inherited + [default.qmp, default.debug]:
                    peer.close()


class I2000LaunchTests(unittest.TestCase):
    def test_hp_i2000_takes_its_channel_from_com1(self):
        from AsteriaVm import Machine
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            environment = Machine.Environment('qemu-ia64', 'firmware', 'bios', '', '', '')
            launch = Machine.prepare('Ia64', esp, root / 'Run', 1, '', environment, iommu='None', machine='hp-i2000')
            try:
                argv = launch.argv
                self.assertEqual(argv[argv.index('-machine') + 1], 'hp-i2000,nvram=none,usb=off')
                self.assertEqual(argv[argv.index('-cpu') + 1], 'merced')
                self.assertEqual(argv[argv.index('-serial') + 1], 'chardev:dbg')
                self.assertEqual(argv[argv.index('-m') + 1], '2048M')
                self.assertEqual(argv[argv.index('-debug-port') + 1], 'chardev:dbgport')
                self.assertIn('file,id=dbgport,path=%s' % (root / 'Run' / 'DebugPort.log'), argv)
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()
            for options in ({'disk': 'raw'}, {'device': True}, {'aml': 'facts'}):
                with self.subTest(options=options), self.assertRaises(ValueError):
                    Machine.prepare('Ia64', esp, root / 'Refused', 1, options.get('disk', ''), environment,
                                    device=options.get('device', False), aml=options.get('aml', ''),
                                    iommu='None', machine='hp-i2000')
        cases = Scenarios.select(['Ia64I2000'], [])
        self.assertEqual([(case.name, case.cpus, case.machine, case.firmware) for case in cases],
                         [('Ia64I2000.Production', 1, 'hp-i2000', Scenarios.PRODUCTION_FIRMWARE),
                          ('Ia64I2000.Smp2', 2, 'hp-i2000', Scenarios.PRODUCTION_FIRMWARE)])
        self.assertNotIn('Ia64I2000.Production', [case.name for case in Scenarios.select(['All', 'Ia64'], [])])


class XhciLaunchTests(unittest.TestCase):
    def test_usb_boot_replaces_virtio_and_keeps_the_default_controller(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'Variables.fd'
            variables.write_bytes(b'variables')
            environment = Machine.Environment('', '', '', 'qemu', 'code', str(variables))
            with mock.patch.object(Machine.subprocess, 'run') as media:
                launch = Machine.prepare('Amd64', esp, root / 'Run', 2, '', environment,
                                         xhci=True, usb_boot=True)
            try:
                self.assertIn('--mbr', media.call_args.args[0])
                self.assertFalse(any('if=virtio' in arg for arg in launch.argv))
                controller = 'qemu-xhci,id=xhci,addr=08.0'
                storage = 'usb-storage,bus=xhci.0,drive=usbboot'
                self.assertLess(launch.argv.index(controller), launch.argv.index(storage))
                self.assertLess(launch.argv.index(storage), launch.argv.index(
                    'usb-kbd,id=usbkbd,bus=xhci.0,display=usbconsole'))
                self.assertIn('intel-iommu,intremap=off', launch.argv)
                self.assertIn('2048M', launch.argv)
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()

    def test_failure_case_omits_dma_isolation(self):
        case, = Scenarios.select([], ['Amd64.XhciFailedProduct'])
        self.assertTrue(case.xhci_failure)
        self.assertFalse(case.guest)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'Variables.fd'
            variables.touch()
            environment = Machine.Environment('', '', '', 'qemu', 'code', str(variables))
            launch = Machine.prepare('Amd64', esp, root / 'Run', 2, '', environment,
                                     xhci=True, xhci_failure=case.xhci_failure)
            try:
                self.assertIn('qemu-xhci,id=xhci,addr=08.0,msi=on,msix=off,p2=9,p3=9', launch.argv)
                self.assertNotIn('intel-iommu,intremap=off', launch.argv)
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()

    def test_large_usb_product_has_high_hid_ports_and_vtd(self):
        case, = Scenarios.select([], ['Amd64.XhciUsbLargeProduct'])
        self.assertTrue(case.usb_boot and case.xhci_large)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'Variables.fd'
            variables.touch()
            environment = Machine.Environment('', '', '', 'qemu', 'code', str(variables))
            with mock.patch.object(Machine.subprocess, 'run'):
                launch = Machine.prepare('Amd64', esp, root / 'Run', 2, '', environment,
                                         xhci=True, usb_boot=True, xhci_large=True)
            try:
                self.assertIn('qemu-xhci,id=xhci,addr=08.0,p2=15,p3=15', launch.argv)
                self.assertIn('usb-storage,bus=xhci.0,drive=usbboot', launch.argv)
                self.assertIn('usb-kbd,id=usbkbd,bus=xhci.0,display=usbconsole,port=14', launch.argv)
                self.assertIn('usb-mouse,id=usbmouse,bus=xhci.0,port=15', launch.argv)
                self.assertIn('intel-iommu,intremap=off', launch.argv)
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()

    def test_usb_host_beside_xhci_attaches_its_controllers_without_devices(self):
        case, = Scenarios.select([], ['Amd64.UsbHostBesideXhci'])
        self.assertTrue(case.xhci and case.i8042 and case.usb_console and not case.guest)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            esp.mkdir()
            variables = root / 'Variables.fd'
            variables.touch()
            environment = Machine.Environment('', '', '', 'qemu', 'code', str(variables))
            launch = Machine.prepare('Amd64', esp, root / 'Run', 1, '', environment, device=case.guest,
                                     xhci=True, usb_host=case.usb_host, usb_layout=case.usb_layout)
            try:
                self.assertIn('ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on', launch.argv)
                # The keyboard and the mouse are the xHCI's alone, so QMP's
                # usbconsole reaches one keyboard.
                self.assertEqual([arg for arg in launch.argv if arg.startswith(('usb-kbd', 'usb-mouse'))],
                                 ['usb-kbd,id=usbkbd,bus=xhci.0,display=usbconsole', 'usb-mouse,id=usbmouse,bus=xhci.0'])
                self.assertEqual(launch.argv.count('intel-iommu,intremap=off'), 1)
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()


class DisplayModeLaunchTests(unittest.TestCase):
    def test_edid_policy_is_scoped_to_case_media(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            esp = root / 'Esp'
            manifest = esp / 'EFI/ASTERIA/BOOT.CFG'
            manifest.parent.mkdir(parents=True)
            original = 'version = 1\n'
            manifest.write_text(original)
            variables = root / 'Variables.fd'
            variables.write_bytes(b'variables')
            environment = Machine.Environment('', '', '', 'qemu', 'code', str(variables))
            launch = Machine.prepare('Amd64', esp, root / 'Run', 1, '', environment,
                                     edid=(1366, 768), display_policy='keep')
            try:
                self.assertIn('VGA,edid=on,xres=1366,yres=768', launch.argv)
                self.assertEqual(manifest.read_text(), original)
                self.assertIn('display = keep', (root / 'Run/Esp/EFI/ASTERIA/BOOT.CFG').read_text())
            finally:
                for peer in launch.inherited + [launch.qmp, launch.debug] + ([launch.lan] if launch.lan is not None else []):
                    peer.close()

    def test_native_and_keep_cases_have_independent_resolution_oracles(self):
        native, = Scenarios.select([], ['Amd64.DisplayNative'])
        keep, = Scenarios.select([], ['Amd64.DisplayKeep'])
        self.assertEqual(native.edid, (1440, 900))
        self.assertEqual(native.expected_display, (1440, 900))
        self.assertEqual(keep.edid, (1280, 800))
        self.assertEqual(keep.display_policy, 'keep')
        self.assertEqual(keep.expected_display, (1280, 800))


if __name__ == '__main__':
    unittest.main()
