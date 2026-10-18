# SPDX-License-Identifier: Apache-2.0
"""Whole-case evaluation of the recorded IA-64 runs (every driver's check list).

A recorded passing run must pass every check; the same evidence judged
against the wrong case, or with one piece of evidence missing, must fail
exactly the checks that depend on it.
"""
from __future__ import annotations

import dataclasses
import pathlib
import tempfile
import unittest
from unittest import mock

from AsteriaVm import Evidence, Guest, Lan
from AsteriaVm.Drivers import Ia64Default, Ia64NativePreemption, Ia64SyntheticProbe
from AsteriaVm.Executor import CaseFailure, CaseResult, CheckResult
from AsteriaVm.Observers import Ia64Capture, Ia64Evidence, Ia64Faults, Ia64State, MemoryImport
from AsteriaVm.Qmp import Qmp, QmpError
from Tests.test_ia64_fixtures import expectation, failing, observation, fixture_schema

EVIDENCE_CHECKS = {
    'host.ia64.evidence.record', 'host.ia64.evidence.clocks',
    'host.ia64.evidence.iommu-boot', 'host.ia64.evidence.processors', 'host.ia64.evidence.pci-ecam-first-function',
    'host.ia64.evidence.pci-ahci-function', 'host.ia64.evidence.ahci-identify', 'host.ia64.evidence.ahci-read',
    'host.ia64.evidence.ap-runtime', 'host.ia64.evidence.shared-root-shootdown',
    'host.ia64.evidence.unreached-scenarios'}
if any(field.name == 'UserProcessors' for field in fixture_schema().record.fields):
    EVIDENCE_CHECKS.add('host.ia64.evidence.user-processors')

DEFAULT_CHECKS = EVIDENCE_CHECKS | {
    'host.ia64.evidence.memory-import',
    'host.ia64.evidence.iommu', 'host.ia64.evidence.user-preemption', 'host.ia64.evidence.object-management', 'host.ia64.evidence.threads',
    'host.ia64.evidence.domains', 'host.ia64.evidence.components', 'host.ia64.evidence.collection',
    'host.ia64.evidence.application-launch',
    'host.ia64.evidence.device-control', 'host.ia64.evidence.services', 'host.ia64.evidence.interrupts',
    'host.ia64.evidence.block', 'host.ia64.evidence.shared-images', 'host.ia64.cpu.user-idle-reached',
    'lifecycle.stages', 'host.ia64.cpu.stopped-state',
    'host.ia64.cpu.no-pending-exception', 'host.ia64.ivt.installed-address', 'host.ia64.ivt.live-image',
    'host.ia64.ivt.vector-routing', Ia64Default.NIC_CHECK}
# A case that attaches the e1000e replays the LAN peer's capture too
NETWORK_CHECKS = set(Lan.checks('ia64'))
PROBE_CHECKS = EVIDENCE_CHECKS | {
    'host.ia64.cpu.probe-loop-reached', 'lifecycle.stages', 'host.ia64.evidence.probe-local-timer',
    'host.ia64.evidence.probe-timer-queue', 'host.ia64.evidence.probe-wait-timer', 'host.ia64.cpu.stopped-state',
    'host.ia64.cpu.register-nat', 'host.ia64.probe.user-registers', 'host.ia64.translation.region-registers',
    'host.ia64.cpu.rse-state', 'host.ia64.probe.frame', 'host.ia64.translation.user-tcs',
    'host.ia64.translation.kernel-trs', 'host.ia64.cpu.no-pending-exception', 'host.ia64.ivt.installed-address',
    'host.ia64.ivt.live-image', 'host.ia64.ivt.vector-routing', 'host.ia64.probe.stack-page',
    'host.ia64.probe.wait-response', 'host.ia64.probe.rse-sentinel', 'host.ia64.probe.rse-parent-spill'}
PREEMPTION_CHECKS = EVIDENCE_CHECKS | {
    'lifecycle.stages', 'host.ia64.evidence.native-preemption', 'host.ia64.translation.cpu0-kernel-trs'}


def names(checks: list) -> set[str]:
    return {check[0] for check in checks}


class DefaultImageTests(unittest.TestCase):
    def test_recorded_runs_pass(self):
        for name, extra in (('Smp4', set()), ('Ahci', set()), ('Relocated', {'host.ia64.image.relocated'})):
            with self.subTest(name):
                checks = Ia64Default.evaluate(observation(name), expectation(name))
                translations = {'host.ia64.translation.cpu%d-kernel-trs' % cpu
                                for cpu in range(expectation(name).cpus)}
                network = NETWORK_CHECKS if expectation(name).network else set()
                self.assertEqual(names(checks), DEFAULT_CHECKS | extra | translations | network)
                self.assertEqual(failing(checks), {})

    def test_the_network_demonstration_is_judged_from_its_capture(self):
        # The recorded run attaches the e1000e. Missing capture always fails
        # LAN checks; resolution retries may also put NIC-copy mappings above
        # the minimum bound available without a capture. A wrong echo fails
        # exactly its own check because the traffic count is unchanged.
        expected = expectation('Smp4')
        self.assertTrue(expected.network)
        recorded = observation('Smp4')
        recorded.lan = None
        missing = set(failing(Ia64Default.evaluate(recorded, expected)))
        self.assertEqual(missing & NETWORK_CHECKS, NETWORK_CHECKS)
        self.assertLessEqual(missing, NETWORK_CHECKS | {'host.ia64.evidence.object-management'})
        recorded = observation('Smp4')
        frames, errors = recorded.lan
        echo = Lan.ethernet(Lan.PEER_MAC, Lan.GUEST_MAC, 0x800, Lan.ipv4(Lan.GUEST_IP, Lan.PEER_IP, 17, Lan.udp(
            Lan.GUEST_IP, Lan.PEER_IP, 7, 40000, Lan.ECHO_PAYLOAD)))
        index = max(i for i, frame in enumerate(frames) if frame.direction == 'guest' and frame.data == echo)
        wrong = Lan.ethernet(Lan.PEER_MAC, Lan.GUEST_MAC, 0x800, Lan.ipv4(Lan.GUEST_IP, Lan.PEER_IP, 17, Lan.udp(
            Lan.GUEST_IP, Lan.PEER_IP, 7, 40001, Lan.ECHO_PAYLOAD)))
        recorded.lan = (frames[:index] + [dataclasses.replace(frames[index], data=wrong)] + frames[index + 1:], errors)
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expected))), {'host.ia64.net.udp-echo'})

    def test_the_nic_publication_is_required_exactly_where_attached(self):
        expected = expectation('Smp4')
        recorded = observation('Smp4')
        recorded.lines = [line for line in recorded.lines if line != Ia64Default.NIC_PUBLISHED_LINE]
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expected))), {Ia64Default.NIC_CHECK})
        detached = dataclasses.replace(expected, network=False)
        self.assertEqual(failing(Ia64Default.network_checks(observation('Smp4'), detached)).keys(),
                         {Ia64Default.NIC_CHECK})
        self.assertEqual(failing(Ia64Default.network_checks(recorded, detached)), {})

    def test_a_run_judged_as_another_topology_fails_the_topology_checks(self):
        wrong = dataclasses.replace(expectation('Smp4'), cpus=2)
        self.assertEqual(set(failing(Ia64Default.evaluate(observation('Smp4'), wrong))),
                         {'host.ia64.evidence.clocks', 'host.ia64.evidence.processors', 'host.ia64.evidence.ap-runtime',
                          'host.ia64.evidence.shared-root-shootdown'})

    def test_an_unrelocated_run_fails_the_relocation_check(self):
        wrong = dataclasses.replace(expectation('Smp4'), relocated=True)
        self.assertEqual(set(failing(Ia64Default.evaluate(observation('Smp4'), wrong))),
                         {'host.ia64.image.relocated'})

    def test_a_disk_run_judged_without_a_disk_fails_the_ahci_checks(self):
        wrong = dataclasses.replace(expectation('Ahci'), disk=None)
        self.assertEqual(set(failing(Ia64Default.evaluate(observation('Ahci'), wrong))),
                         {'lifecycle.stages', 'host.ia64.evidence.ahci-identify', 'host.ia64.evidence.ahci-read'})

    def test_a_diskless_run_judged_with_a_disk_fails_the_ahci_checks(self):
        wrong = dataclasses.replace(expectation('Smp4'), disk=expectation('Ahci').disk)
        self.assertEqual(set(failing(Ia64Default.evaluate(observation('Smp4'), wrong))),
                         {'lifecycle.stages', 'host.ia64.evidence.ahci-identify', 'host.ia64.evidence.ahci-read'})

    def test_a_wrong_fixture_fails_the_buffer_and_identify_checks(self):
        expected = expectation('Ahci')
        disk = dataclasses.replace(expected.disk, sector=b'\0' + expected.disk.sector[1:],
                                   sectors=expected.disk.sectors * 2)
        self.assertEqual(set(failing(Ia64Default.evaluate(observation('Ahci'), dataclasses.replace(expected,
                                                                                                    disk=disk)))),
                         {'host.ia64.evidence.ahci-read', 'host.ia64.evidence.ahci-identify'})

    def test_missing_evidence_fails_its_checks(self):
        recorded = observation('Smp4')
        recorded.memory.pop(Ia64Default.IVT_DUMP)
        recorded.memory.pop(Ia64Evidence.DUMP)
        recorded.loop = None
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expectation('Smp4')))),
                         {'host.ia64.cpu.user-idle-reached', 'host.ia64.ivt.live-image',
                          'host.ia64.ivt.vector-routing', 'host.ia64.evidence.iommu', 'host.ia64.evidence.user-preemption',
                          'host.ia64.evidence.object-management', 'host.ia64.evidence.threads',
                          'host.ia64.evidence.domains', 'host.ia64.evidence.components',
                          'host.ia64.evidence.collection', 'host.ia64.evidence.device-control',
                          'host.ia64.evidence.application-launch',
                          'host.ia64.evidence.services', 'host.ia64.evidence.interrupts',
                          'host.ia64.evidence.block'} | EVIDENCE_CHECKS)

    def test_a_run_without_its_fact_line_fails_the_record_check(self):
        recorded = observation('Smp4')
        recorded.lines = [line for line in recorded.lines if not line.startswith(Evidence.FACT_PREFIX)]
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expectation('Smp4')))),
                         {'host.ia64.evidence.record'})

    def test_a_missing_stage_fails_the_stage_check(self):
        recorded = observation('Smp4')
        recorded.lines = [line for line in recorded.lines if line != 'ASTERIA:STAGE:AP-RUNTIME']
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expectation('Smp4')))), {'lifecycle.stages'})

    def test_a_missing_register_dump_fails_every_register_check(self):
        recorded = observation('Smp4')
        recorded.registers = None
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expectation('Smp4')))),
                         {'host.ia64.cpu.stopped-state', 'host.ia64.cpu.no-pending-exception',
                          'host.ia64.ivt.installed-address', 'host.ia64.translation.cpu0-kernel-trs'})

    def test_each_processors_kernel_plan_is_checked(self):
        expected = expectation('Smp4')
        for cpu in range(expected.cpus):
            with self.subTest(cpu=cpu):
                recorded = observation('Smp4')
                text = recorded.registers if cpu == 0 else recorded.processor_registers[cpu]
                target = next(line for line in text.splitlines() if line.startswith('DTLB[1] TR '))
                changed = text.replace(target, target.replace('perm=0x5', 'perm=0x7'))
                self.assertNotEqual(changed, text)
                if cpu == 0:
                    recorded.registers = changed
                else:
                    recorded.processor_registers[cpu] = changed
                self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expected))),
                                 {'host.ia64.translation.cpu%d-kernel-trs' % cpu})
                if cpu != 0:
                    recorded.processor_registers.pop(cpu)
                    self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expected))),
                                     {'host.ia64.translation.cpu%d-kernel-trs' % cpu})

    def test_the_relocated_table_is_judged_at_the_loaded_base(self):
        # A wrong loaded base moves the expected table, its entries and the
        # polling leaf alike; the IP-relative routing is invariant, so IVA and
        # the APs' recorded polling leaf are what pin the base.
        expected = expectation('Relocated')
        recorded = observation('Relocated')
        loaded = next(line for line in recorded.lines if line.startswith('  image-base='))
        base = int(loaded.split('=')[1].split()[0], 16)
        moved = loaded.replace('image-base=0x%016X' % base, 'image-base=0x%016X' % (base + 0x1000000))
        self.assertNotEqual(moved, loaded)
        kernel = '  kernel-base=0x%016X' % base
        moved_kernel = '  kernel-base=0x%016X' % (base + 0x1000000)
        recorded.lines = [moved if line == loaded else moved_kernel if line == kernel else line
                          for line in recorded.lines]
        self.assertEqual(set(failing(Ia64Default.evaluate(recorded, expected))),
                         {'host.ia64.ivt.installed-address', 'host.ia64.evidence.ap-runtime', 'host.ia64.evidence.iommu-boot'}
                         | {'host.ia64.translation.cpu%d-kernel-trs' % cpu for cpu in range(expected.cpus)})


class SyntheticProbeTests(unittest.TestCase):
    def test_recorded_run_passes(self):
        checks = Ia64SyntheticProbe.evaluate(observation('SyntheticProbe'), expectation('SyntheticProbe'))
        self.assertEqual(names(checks), PROBE_CHECKS)
        self.assertEqual(failing(checks), {})
        note = next(check[2] for check in checks if check[0] == 'host.ia64.translation.kernel-trs')
        self.assertRegex(note, r'^runtime page at 0x[0-9a-f]+ \(bootstrap consolidated\)$')

    def test_the_default_image_is_not_a_probe_run(self):
        failed = failing(Ia64SyntheticProbe.evaluate(observation('Smp4'), expectation('Smp4')))
        for name in ('host.ia64.cpu.probe-loop-reached', 'lifecycle.stages', 'host.ia64.translation.user-tcs',
                     'host.ia64.probe.user-registers', 'host.ia64.evidence.probe-local-timer',
                     'host.ia64.evidence.probe-wait-timer'):
            self.assertIn(name, failed)

    def test_a_probe_run_is_not_a_default_image_run(self):
        failed = failing(Ia64Default.evaluate(observation('SyntheticProbe'), expectation('SyntheticProbe')))
        for name in ('host.ia64.cpu.user-idle-reached', 'host.ia64.cpu.stopped-state', 'lifecycle.stages',
                     'host.ia64.evidence.unreached-scenarios'):
            self.assertIn(name, failed)


class NativePreemptionTests(unittest.TestCase):
    def test_recorded_run_passes(self):
        checks = Ia64NativePreemption.evaluate(observation('NativePreemption'), expectation('NativePreemption'))
        self.assertEqual(names(checks), PREEMPTION_CHECKS)
        self.assertEqual(failing(checks), {})

    def test_other_runs_fail(self):
        for name in ('Smp4', 'SyntheticProbe'):
            failed = failing(Ia64NativePreemption.evaluate(observation(name), expectation(name)))
            self.assertIn('host.ia64.evidence.native-preemption', failed, name)
            self.assertIn('lifecycle.stages', failed, name)


class PlumbingTests(unittest.TestCase):
    """The driver plumbing a passing run does not exercise."""

    class Context:
        def __init__(self, directory, text, lines=('ASTERIA:X',), memory=None):
            self.run_directory = pathlib.Path(directory)
            self.result = CaseResult(None, self.run_directory)
            self.text = text
            test = self

            class FakeQmp:
                hmp = Qmp.hmp   # The real wrapper, over the fake execute

                def execute(self, command, arguments=None, timeout=30.0):
                    return {'human-monitor-command': test.text, 'query-pci': [], 'query-status': {}}[command]

                def cpus(self):
                    return [{'cpu-index': 0}]

                def memsave(self, address, size, cpu=0):
                    if address == 0xBAD:
                        raise QmpError('memsave failed: no translation')
                    return bytes(size)

                def pmemsave(self, address, size):
                    if memory is not None:
                        return memory(address, size)
                    return b'\x01' * size

            class FakeConsole:
                pass

            FakeConsole.lines = list(lines)
            self.qmp, self.console = FakeQmp(), FakeConsole()

        def wait_for_line(self, predicate, timeout, since=0):
            raise CaseFailure('waiting for %s: no matching line' % predicate.__name__)

        def check(self, name, passed, detail=''):
            self.result.checks.append(CheckResult(name, bool(passed), detail))
            return passed

    def test_a_failed_gate_keeps_the_registers(self):
        with tempfile.TemporaryDirectory() as directory:
            context = self.Context(directory, 'IP: 0x0\n')
            with self.assertRaises(CaseFailure) as raised:
                Ia64Default.gate(context, 'NEVER', 1)
            self.assertIn('ASTERIA:STAGE:NEVER', str(raised.exception))
            self.assertEqual((context.run_directory / 'Registers.txt').read_text(), 'IP: 0x0\n')

    class GateContext:
        """A context whose waits play a script: each entry is the lines that
        arrive during one budget and whether the stage came (True), the
        budget ran out (False) or the kernel failed ('fail')."""

        def __init__(self, directory, script):
            self.run_directory = pathlib.Path(directory)
            self.script = list(script)
            self.waits = 0
            self.qmp = None

            class FakeConsole:
                lines: list[str] = []
            self.console = FakeConsole()
            self.console.lines = ['ASTERIA:STAGE:BOOT']

        def wait_for_line(self, predicate, timeout, since=0):
            self.waits += 1
            arrived, outcome = self.script.pop(0)
            self.console.lines = self.console.lines + arrived
            if outcome is True:
                return len(self.console.lines) - 1, predicate.__name__
            if outcome == 'fail':
                raise CaseFailure('waiting for %s: kernel failure: ASTERIA:X:FAIL' % predicate.__name__)
            raise CaseFailure('waiting for %s: no matching line within %d s' % (predicate.__name__, timeout))

    def test_a_gate_renews_its_budget_while_the_kernel_reports(self):
        with tempfile.TemporaryDirectory() as directory:
            context = self.GateContext(directory, [(['ASTERIA:STAGE:A'], False), (['ASTERIA:STAGE:B'], False),
                                                   (['ASTERIA:STAGE:NEVER'], True)])
            self.assertEqual(Ia64Default.gate(context, 'NEVER', 1), 3)
            self.assertEqual(context.waits, 3)

    def test_a_silent_or_failing_kernel_ends_the_gate_at_once(self):
        for script in ([([], False)], [(['ASTERIA:STAGE:A'], 'fail')]):
            with tempfile.TemporaryDirectory() as directory, mock.patch.object(
                    Ia64Default.Ia64State, 'read_text', return_value='IP: 0x0\n'):
                context = self.GateContext(directory, script)
                with self.assertRaises(CaseFailure):
                    Ia64Default.gate(context, 'NEVER', 1)
                self.assertEqual(context.waits, 1, script)

    def test_gate_renewals_are_bounded(self):
        script = [(['ASTERIA:STAGE:A%d' % index], False) for index in range(Ia64Default.GATE_RENEWALS + 2)]
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
                Ia64Default.Ia64State, 'read_text', return_value='IP: 0x0\n'):
            context = self.GateContext(directory, script)
            with self.assertRaises(CaseFailure) as raised:
                Ia64Default.gate(context, 'NEVER', 1)
            self.assertIn('no matching line', str(raised.exception))
            self.assertEqual(context.waits, Ia64Default.GATE_RENEWALS + 1)

    def test_capture_each_processor_and_keep_its_dump(self):
        with tempfile.TemporaryDirectory() as directory:
            context = self.Context(directory, 'unused')
            with mock.patch.object(Ia64Default.Ia64State, 'read_text',
                                   side_effect=lambda qmp, cpu: 'CPU%d\n' % cpu) as read:
                observation_, errors = Ia64Default.capture(context, {}, processors=4)
            self.assertEqual(errors, {})
            self.assertEqual(observation_.processor_registers, {1: 'CPU1\n', 2: 'CPU2\n', 3: 'CPU3\n'})
            self.assertEqual([call.args[1] for call in read.call_args_list], [0, 1, 2, 3])
            Ia64Capture.save(observation_, pathlib.Path(directory))
            (pathlib.Path(directory) / 'Debug.log').write_text('')
            self.assertEqual(Ia64Capture.load(pathlib.Path(directory)).processor_registers,
                             observation_.processor_registers)

    def test_capture_and_record(self):
        with tempfile.TemporaryDirectory() as directory:
            context = self.Context(directory, 'IP: 0x0\n')
            observation_, errors = Ia64Default.capture(context, {'A': ('virtual', 0x1000, 8),
                                                                 'B': ('physical', 0x2000, 4),
                                                                 'C': ('virtual', 0xBAD, 8)})
            self.assertEqual(observation_.memory, {'A': bytes(8), 'B': b'\x01' * 4})
            self.assertEqual(list(errors), ['C'])
            self.assertEqual((observation_.registers, observation_.pci, observation_.lines),
                             ('IP: 0x0\n', [], ['ASTERIA:X']))
            Ia64Default.record(context, [('x.pass', [], 'derived 0x270000'), ('x.fail', ['bad', 'worse']),
                                         ('x.plain', [])], errors)
            self.assertEqual([(check.name, check.passed, check.detail) for check in context.result.checks],
                             [('x.pass', True, 'derived 0x270000'), ('x.fail', False, 'bad\nworse'),
                              ('x.plain', True, ''), ('lifecycle.capture', False, 'C: memsave failed: no translation')])

    def test_the_evidence_capture_follows_the_record(self):
        """The record, memory map and AHCI buffer are read at their named
        addresses; a record of another schema is refused."""
        recorded = observation('Ahci')
        expected = expectation('Ahci')
        data = recorded.memory[Ia64Evidence.DUMP]
        address = Evidence.locate(recorded.lines)[0]
        decoded = expected.schema.decode(data)
        buffer = decoded.Ahci.ReadBufferPhysicalAddress
        memory_map = decoded.MemoryImport

        def memory(start, size):
            if start == address:
                return data[:size]
            if start == memory_map.MapPhysical:
                self.assertEqual(size, memory_map.MapRegions * MemoryImport.MAP_RECORD.size)
                return recorded.memory[MemoryImport.DUMP]
            self.assertEqual((start, size), (buffer, len(recorded.memory['AhciReadBuffer'])))
            return recorded.memory['AhciReadBuffer']

        with tempfile.TemporaryDirectory() as directory:
            context = self.Context(directory, '', recorded.lines, memory)
            captured, _ = Ia64Default.capture(context, {})
            self.assertEqual(Ia64Default.capture_evidence(context, captured, expected), {})
            self.assertEqual(captured.memory[Ia64Evidence.DUMP], data)
            self.assertEqual(captured.memory[MemoryImport.DUMP], recorded.memory[MemoryImport.DUMP])
            self.assertEqual(captured.memory['AhciReadBuffer'], recorded.memory['AhciReadBuffer'])

            other = bytearray(data)
            other[8] += 1   # The schema number
            context = self.Context(directory, '', recorded.lines, lambda start, size: bytes(other[:size]))
            captured, _ = Ia64Default.capture(context, {})
            errors = Ia64Default.capture_evidence(context, captured, expected)
            self.assertIn('schema', errors[Ia64Evidence.DUMP])
            self.assertNotIn(Ia64Evidence.DUMP, captured.memory)


class UserLoopTimingTests(unittest.TestCase):
    def test_three_loop_samples_fit_before_the_next_preemption(self):
        context = mock.Mock()
        context.seconds.side_effect = lambda seconds: seconds
        context.console.failures.return_value = []
        context.exited.return_value = None
        idle, psr = 0x2000000004003020, 0x0000101308026000
        elapsed = 0.0

        def read(*_):
            # A 50 ms user run alternates with a kernel/other-thread interval.
            # Polling every 50 ms can never see three consecutive user samples.
            user = elapsed % 0.1 < 0.05
            return mock.Mock(get=mock.Mock(return_value={
                'ip': idle if user else 0x41A1490, 'psr': psr if user else 0x0000101008022018, 'halted': 0}))

        def poll(predicate, timeout, interval):
            nonlocal elapsed
            for _ in range(1000):
                if predicate():
                    return True
                elapsed += interval
                if elapsed >= timeout:
                    return False
            self.fail('polling did not reach its bound')

        with mock.patch.object(Ia64State, 'read', side_effect=read), \
                mock.patch.object(Ia64Default.Ia64Cpu, 'wait_until', side_effect=poll):
            result = Ia64Default.wait_for_user_loop(context, idle, psr)
        self.assertTrue(result.reached)
        self.assertEqual((result.polls, result.matches), (3, 3))
        self.assertLess(elapsed, 0.05)

    def test_spinner_is_observed_before_waiting_for_the_idle_loop(self):
        context = mock.Mock()
        facts, expected = mock.Mock(), mock.Mock()
        calls = mock.Mock()
        calls.attach_mock(mock.Mock(), 'guest')
        calls.attach_mock(mock.Mock(), 'self_tests')
        calls.attach_mock(mock.Mock(side_effect=CaseFailure('end observation')), 'loop')
        with mock.patch.object(Ia64Default.Ia64Image.ImageFacts, 'from_target', return_value=facts), \
                mock.patch.object(Ia64Default, 'expectation', return_value=expected), \
                mock.patch.object(Ia64Default, 'gate', return_value=0), \
                mock.patch.object(Ia64Default, 'wait_for_guest', calls.guest), \
                mock.patch.object(Ia64Default, 'wait_for_self_tests', calls.self_tests), \
                mock.patch.object(Ia64Default, 'wait_for_user_loop', calls.loop):
            with self.assertRaisesRegex(CaseFailure, 'end observation'):
                Ia64Default._run(context, None)
        self.assertEqual([call[0] for call in calls.mock_calls], ['guest', 'self_tests', 'loop'])

    def test_stop_follows_a_live_loop_sample_after_kernel_samples(self):
        context = mock.Mock()
        context.seconds.side_effect = lambda seconds: seconds
        idle, psr = 0x2000000004003020, 0x0000101308026000
        kernel = {'ip': 0x41A1490, 'psr': 0x0000101008022018, 'halted': 0}
        user = {'ip': idle, 'psr': psr, 'halted': 0}
        records = [kernel, kernel, user, user]
        read = mock.Mock(side_effect=[mock.Mock(get=mock.Mock(return_value=record)) for record in records])

        def poll(predicate, timeout, interval):
            self.assertFalse(predicate())
            self.assertTrue(predicate())
            return True

        with mock.patch.object(Ia64State, 'read', read), \
                mock.patch.object(Ia64Default, 'wait_until', side_effect=poll) as wait:
            Ia64Default.stop_in_user_loop(context, idle, psr)
        self.assertEqual(context.stop.call_count, 2)
        context.qmp.cont.assert_called_once_with()
        self.assertEqual(wait.call_args.args[1:], (0.2, 0.01))

    def test_stops_remain_bounded_when_the_guest_never_reaches_the_loop(self):
        context = mock.Mock()
        context.seconds.side_effect = lambda seconds: seconds
        kernel = mock.Mock(get=mock.Mock(return_value={'ip': 0x41A1490, 'psr': 0x0000101008022018,
                                                      'halted': 0}))
        with mock.patch.object(Ia64State, 'read', return_value=kernel), \
                mock.patch.object(Ia64Default, 'wait_until', side_effect=lambda predicate, *_: predicate()) as wait:
            Ia64Default.stop_in_user_loop(context, 0x2000000004003020, 0x0000101308026000)
        self.assertEqual(context.stop.call_count, 20)
        self.assertEqual(context.qmp.cont.call_count, 19)
        self.assertEqual(wait.call_count, 19)


class SelfTestWaitTests(unittest.TestCase):
    def test_completion_timeout_is_reported_before_partial_evidence(self):
        for completed in (False, True):
            with self.subTest(completed=completed):
                context = mock.Mock()
                context.console.lines = []
                context.seconds.side_effect = lambda seconds: seconds
                with mock.patch.object(Ia64Default, 'wait_until', return_value=completed) as wait:
                    Ia64Default.wait_for_self_tests(context, mock.Mock())
                wait.assert_called_once()
                name, passed, detail = context.check.call_args.args
                self.assertEqual((name, passed), ('host.ia64.self-tests-complete', completed))
                self.assertEqual(bool(detail), not completed)

    def test_spinner_preemption_counts_only_the_run_stops_while_it_counts(self):
        """Created-thread run stops before the spinner's first line and after
        its last never count; one between them does."""
        for stops, preempted in ((0, False), (1, True)):
            with self.subTest(stops=stops):
                context = mock.Mock()
                context.console.lines = []
                context.console.failures.return_value = []
                context.exited.return_value = False
                context.seconds.side_effect = lambda seconds: seconds
                threads = mock.Mock(RunStops=5, Reclaimed=0, Kills=0)
                # A diskless case's lines decide when the components are done.
                expected = mock.Mock(recipe='', iommu=True, ps2=False)
                expected.schema.decode.return_value.Threads = threads

                def poll(finished, timeout, interval):
                    finished()
                    threads.RunStops += 7
                    context.console.lines.append(Guest.USER_PREFIX + Ia64Faults.COUNT_START)
                    finished()
                    threads.RunStops += stops
                    finished()
                    context.console.lines.append(Guest.USER_PREFIX + Ia64Faults.COUNT_DONE)
                    threads.RunStops += 3
                    finished()
                    return True

                with mock.patch.object(Evidence, 'locate', return_value=(0x1000, None)), \
                        mock.patch.object(Ia64Default, 'wait_until', side_effect=poll):
                    Ia64Default.wait_for_self_tests(context, expected)
                checks = {call.args[0]: call.args[1] for call in context.check.call_args_list}
                self.assertEqual(checks['host.ia64.user-spinner-preempted'], preempted)


if __name__ == '__main__':
    unittest.main()
