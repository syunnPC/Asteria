# SPDX-License-Identifier: Apache-2.0
"""Replays recorded AMD64 runs through `Amd64Default.judge` (every check the driver records, no VM).

The NIC fixtures carry the LAN peer's capture (`<Case>.Lan.pcap` and
`<Case>.Lan.json`), copied into the replay's run directory. Every check must
pass with exactly the expected check names, and a
corruption of the recorded registers, memory, evidence or log must fail the
check that owns the property (and no other).
"""
from __future__ import annotations

import pathlib
import socket
import struct
import tempfile
import unittest
from contextlib import ExitStack
from types import SimpleNamespace
from unittest import mock

from AsteriaVm import Guest, Lan, Machine, Ps2, Stages
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Executor import CaseFailure
from.test_amd64_fixtures import (FIXTURES, FakeContext, constants, debug_lines, edited_observation, expectations, field,
                                  observation, record, record_address, replace_cpu, snapshot, with_evidence, schema)

COMMON = {
    'lifecycle.stages', 'host.amd64.evidence.record', 'host.amd64.evidence.allocator', 'host.amd64.evidence.memory-import',
    'host.amd64.evidence.clocks',
    'host.amd64.cpu.bsp.idle-loop', 'host.amd64.cpu.bsp.user-mode', 'host.amd64.cpu.bsp.control-registers',
    'host.amd64.cpu.bsp.long-mode', 'host.amd64.cpu.bsp.root-table',
    *('host.amd64.paging.' + name for name in (
        'user-image', 'user-stack', 'null-page', 'descriptor-tables', 'root-separation', 'shootdown-probe',
        'mmio-window', 'kernel-stacks', 'pager-window', 'scrub-slots', 'thread-stacks')),
    'host.amd64.cpu.count', 'host.amd64.cpu.ap-control-registers', 'host.amd64.cpu.ap-registers',
    'host.amd64.cpu.ap-descriptor-tables',
    'host.amd64.cpu.ap-entry-context', *Amd64Default.EVIDENCE_CHECKS,
}
if any(field.name == 'UserProcessors' for field in schema().record.fields):
    COMMON.add('host.amd64.evidence.user-processors')

EXTRA = {
    'Smp4': {Amd64Default.UNREACHED_CHECK, Amd64Default.NIC_CHECK, 'host.amd64.evidence.shared-images',
             'host.amd64.evidence.application-launch', *Lan.CHECKS},
    'Ahci': {Amd64Default.UNREACHED_CHECK, Amd64Default.NIC_CHECK, 'host.amd64.evidence.shared-images',
             'host.amd64.evidence.application-launch', *Lan.CHECKS},
    'PreemptionRelocated': {Amd64Default.PREEMPTION_CHECK, 'host.amd64.image.relocated',
                            'host.amd64.evidence.shared-images'},
}


class ReplayTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        Machine.write_disk_fixture(pathlib.Path(self.directory.name) / 'Disk.raw')

    def tearDown(self):
        self.directory.cleanup()

    def judge(self, name, *, state=None, recorded=None, lines=None, lan=True) -> FakeContext:
        directory = pathlib.Path(self.directory.name)
        for artifact in (Lan.PCAP_ARTIFACT, Lan.JSON_ARTIFACT):
            recorded_artifact = FIXTURES / ('%s.%s' % (name, artifact))
            if lan and recorded_artifact.exists():
                (directory / artifact).write_bytes(recorded_artifact.read_bytes())
            else:
                (directory / artifact).unlink(missing_ok=True)
        context = FakeContext(name, directory, lines)
        Amd64Default.judge(context, expectations(name), state or snapshot(name), recorded or observation(name))
        return context

    def other_failures(self, context):
        return context.failed()

    def test_recorded_runs_pass_every_check(self):
        for name in ('Smp4', 'Ahci', 'PreemptionRelocated'):
            with self.subTest(name):
                context = self.judge(name)
                self.assertEqual(context.failed(), {})
                self.assertEqual({check.name for check in context.checks}, COMMON | EXTRA[name])
                self.assertEqual(len(context.checks), len(COMMON | EXTRA[name]))
                saved = pathlib.Path(self.directory.name) / 'Evidence.bin'
                self.assertEqual(len(saved.read_bytes()), expectations(name).schema.record.size)

    def assertOnlyFailing(self, context: FakeContext, check: str) -> str:
        self.assertEqual(set(self.other_failures(context)), {check}, context.failed())
        return context.failed()[check]

    def test_register_corruption(self):
        state = replace_cpu(snapshot('Smp4'), 0, control={2: 0x5000_0000})
        self.assertIn('CR2', self.assertOnlyFailing(self.judge('Smp4', state=state),
                                                    'host.amd64.cpu.bsp.control-registers'))
        # Processor scheduling permits IF inside the proven AP window. Corrupt both facts so the
        # mutation always sets IF outside it, even when a fresh fixture captured CPU 2 in
        # STI/PAUSE/CLI.
        baseline = snapshot('Smp4')
        state = replace_cpu(baseline, 2, registers={
            'rip': record('Smp4').Smp.InterruptWindowEnd,
            'rflags': baseline.cpus[2].rflags | 0x200})
        self.assertIn('CPU 2', self.assertOnlyFailing(self.judge('Smp4', state=state), 'host.amd64.cpu.ap-registers'))
        state = replace_cpu(snapshot('Smp4'), 3, control={4: 0x20})
        self.assertIn('CPU 3 CR4', self.assertOnlyFailing(self.judge('Smp4', state=state),
                                                          'host.amd64.cpu.ap-control-registers'))

    def test_clock_placement_exclusion_corruption(self):
        recorded = with_evidence('Smp4', 'Clocks.ExcludedMask', lambda value: value ^ 1)
        self.assertIn('CPU 0 placement exclusion', self.assertOnlyFailing(
            self.judge('Smp4', recorded=recorded), 'host.amd64.evidence.clocks'))

    def test_unaligned_root_ends_the_case(self):
        state = replace_cpu(snapshot('Smp4'), 0, control={3: snapshot('Smp4').cpus[0].cr3 | 0x800})
        with self.assertRaises(CaseFailure):
            self.judge('Smp4', state=state)

    def test_dma_buffer_and_identify_corruption(self):
        buffer = record('Ahci').Ahci.ReadBufferPhysicalAddress
        identify = record_address('Ahci') + field('Ahci.IdentifyData[20]')[0]   # IDENTIFY word 10: the serial
        for address, check in ((buffer + 0x100, 'host.amd64.evidence.ahci-read'),
                               (identify, 'host.amd64.evidence.ahci-identify')):
            def flip(memory, address=address):
                memory.write(address, bytes([memory.read(address, 1)[0] ^ 0x01]))
            with self.subTest(check):
                self.assertOnlyFailing(self.judge('Ahci', recorded=edited_observation('Ahci', flip)), check)

    def test_pager_window_corruption(self):
        """The physical run the record names must have been scrubbed to zeros after the pager's writes, and the
        window must be unmapped after its MemoryUnmap."""
        physical = record('Smp4').ObjectManagement.MappedPhysicalStart

        def flip(memory, address=physical + 0x1000 + 8):
            memory.write(address, bytes([memory.read(address, 1)[0] ^ 0x01]))
        self.assertIn('window page 1', self.assertOnlyFailing(self.judge('Smp4', recorded=edited_observation(
            'Smp4', flip)), 'host.amd64.paging.pager-window'))
        pool, c = record('Smp4').Runtime.PoolPhysicalStart, constants()

        def leave(memory):
            memory.set_entry(c.root(pool, c.pager_root_index), c.mapping_window_start,
                             physical | 0x8000_0000_0000_0007)
        self.assertIn('window page 0', self.assertOnlyFailing(self.judge('Smp4', recorded=edited_observation(
            'Smp4', leave)), 'host.amd64.paging.pager-window'))

    def test_scrub_slot_corruption(self):
        """Every root must map each processor's scrub slot to its slot page as supervisor data."""
        pool, c = record('Smp4').Runtime.PoolPhysicalStart, constants()

        def repoint(memory):
            root = c.root(pool, c.spinner_root_index)
            leaf = memory.entry(root, c.scrub_slot_virtual_address + 0x1000)
            memory.set_entry(root, c.scrub_slot_virtual_address + 0x1000, leaf + 0x1000)
        self.assertIn('spinner root', self.assertOnlyFailing(self.judge('Smp4', recorded=edited_observation(
            'Smp4', repoint)), 'host.amd64.paging.scrub-slots'))

        def expose(memory):
            root = c.root(pool, c.pager_root_index)
            leaf = memory.entry(root, c.scrub_slot_virtual_address)
            memory.set_entry(root, c.scrub_slot_virtual_address, leaf & ~(1 << 63))
        self.assertIn('pager root', self.assertOnlyFailing(self.judge('Smp4', recorded=edited_observation(
            'Smp4', expose)), 'host.amd64.paging.scrub-slots'))

    def test_thread_stack_corruption(self):
        """Every root must map each created-thread stack slot as supervisor data between unmapped guards."""
        pool, c = record('Smp4').Runtime.PoolPhysicalStart, constants()
        first, last = c.thread_stacks(pool)[0], c.thread_stacks(pool)[-1]

        def execute(memory):
            root = c.root(pool, c.user_root_index)
            memory.set_entry(root, first, memory.entry(root, first) & ~(1 << 63))
        self.assertIn('client root', self.assertOnlyFailing(self.judge('Smp4', recorded=edited_observation(
            'Smp4', execute)), 'host.amd64.paging.thread-stacks'))

        def unguard(memory):
            root = c.root(pool, c.shared_root_index)
            above = last + c.kernel_stack_pages * c.page_size
            memory.set_entry(root, above, memory.entry(root, last))
        self.assertIn('guard above thread stack %d in the shared root' % (c.thread_stack_slots - 1),
                      self.assertOnlyFailing(self.judge(
            'Smp4', recorded=edited_observation('Smp4', unguard)), 'host.amd64.paging.thread-stacks'))

    def test_missing_query_pci_function(self):
        recorded = observation('Smp4')
        pci = recorded.query('query-pci')
        pci[0]['devices'].pop()
        recorded._query = lambda command: pci if command == 'query-pci' else observation('Smp4').query(command)
        # The VT-d unit then serves a function more than QEMU has, too.
        context = self.judge('Smp4', recorded=recorded)
        self.assertEqual(set(self.other_failures(context)), {'host.amd64.evidence.pci-ahci-function',
                                                 'host.amd64.evidence.iommu'}, context.failed())
        self.assertIn('QEMU has 8', context.failed()['host.amd64.evidence.pci-ahci-function'])
        self.assertIn('expected (8, 7)', context.failed()['host.amd64.evidence.iommu'])

    def test_log_corruption(self):
        lines = [line for line in debug_lines('PreemptionRelocated') if not line.startswith('  image-base=')]
        self.assertOnlyFailing(self.judge('PreemptionRelocated', lines=lines), 'host.amd64.image.relocated')

    def test_stage_corruption(self):
        """A stage lost, repeated, out of order or of another configuration fails only the stage check."""
        lines = debug_lines('Smp4')
        stage = Stages.line
        launch = lines.index(stage('AMD64-USER-LAUNCH'))
        mutants = {
            'lost': [line for line in lines if line != stage('AMD64-SHOOTDOWN')],
            'repeated': lines[:launch] + [stage('AMD64-PCI')] + lines[launch:],
            'out of order': [stage('AMD64-SHOOTDOWN') if line == stage('AMD64-AP-RUNTIME')
                             else stage('AMD64-AP-RUNTIME') if line == stage('AMD64-SHOOTDOWN') else line
                             for line in lines],
            'a disk stage without a disk': lines[:launch] + [stage('AMD64-AHCI-READ')] + lines[launch:],
            'the preemption proof in a default build': lines[:launch] + [stage('AMD64-NATIVE-PREEMPTION')]
            + lines[launch:],
            'another ISA': lines[:launch] + [stage('AP-RENDEZVOUS')] + lines[launch:],
        }
        for label, mutant in mutants.items():
            with self.subTest(label):
                self.assertOnlyFailing(self.judge('Smp4', lines=mutant), 'lifecycle.stages')
        lines = [line for line in debug_lines('PreemptionRelocated') if line != stage('AMD64-NATIVE-PREEMPTION')]
        self.assertIn('AMD64-NATIVE-PREEMPTION reported 0 times',
                      self.assertOnlyFailing(self.judge('PreemptionRelocated', lines=lines), 'lifecycle.stages'))

    def test_a_missing_record_fails_every_check_that_needs_it(self):
        lines = [line for line in debug_lines('Smp4') if not line.startswith('ASTERIA:EVIDENCE:')]
        context = self.judge('Smp4', lines=lines)
        needs = {'host.amd64.evidence.record', 'host.amd64.paging.root-separation', 'host.amd64.paging.shootdown-probe',
                 'host.amd64.paging.mmio-window', 'host.amd64.paging.kernel-stacks', 'host.amd64.paging.pager-window',
                 'host.amd64.paging.scrub-slots', 'host.amd64.paging.thread-stacks', 'host.amd64.evidence.allocator',
                 'host.amd64.cpu.ap-registers',
                 'host.amd64.cpu.ap-descriptor-tables', 'host.amd64.cpu.ap-entry-context',
                 *Amd64Default.EVIDENCE_CHECKS, Amd64Default.UNREACHED_CHECK}
        self.assertEqual(set(self.other_failures(context)), needs)
        self.assertIn('0 evidence fact lines', context.failed()['host.amd64.evidence.record'])

    def test_a_record_of_another_schema_is_not_decoded(self):
        address = record_address('Smp4')
        recorded = edited_observation('Smp4', lambda memory: memory.write(address + 8, struct.pack('<I', 99)))
        context = self.judge('Smp4', recorded=recorded)
        self.assertIn('schema 99', context.failed()['host.amd64.evidence.record'])

    def test_the_roots_come_from_the_record(self):
        """A record naming another shared root fails its own check and every walk of that root."""
        shared = constants().root_arena_pages * constants().page_size
        recorded = with_evidence('Smp4', 'Runtime.SharedRootPhysicalAddress', lambda value: value + shared)
        failed = set(self.judge('Smp4', recorded=recorded).failed())
        self.assertIn('host.amd64.evidence.runtime', failed)
        self.assertIn('host.amd64.paging.root-separation', failed)
        # APs hold private idle roots; changing the shared BSP root
        # no longer changes their CR3 expectation.


class LanIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = pathlib.Path(self.directory.name)
        self.context = FakeContext('Smp4', self.path)

    def socketpair(self):
        ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.addCleanup(ours.close)
        self.addCleanup(theirs.close)
        theirs.settimeout(1)
        self.context.lan = ours
        return ours, theirs

    def test_failed_expectations_still_save_lan_artifacts(self):
        ours, _ = self.socketpair()
        with mock.patch.object(Amd64Default.Expectations, 'load', side_effect=ValueError('no build')):
            with self.assertRaises(CaseFailure):
                Amd64Default.run_image(self.context)
        self.assertEqual(Lan.load_capture(self.path), ([], []))
        self.assertIsNone(ours.gettimeout())

    def test_peer_answers_resolve_while_waiting_for_boot(self):
        _, theirs = self.socketpair()
        theirs.send(Lan.arp_request(Lan.GUEST_MAC, Lan.GUEST_IP, Lan.PEER_IP))

        def failed_boot(*args, **kwargs):
            reply = Lan.decode_frame(theirs.recv(65536))
            self.assertEqual(reply['arp']['sender_ip'], Lan.PEER_IP)
            self.assertEqual(reply['arp']['sender_mac'], Lan.PEER_MAC)
            raise CaseFailure('boot did not finish')

        self.context.wait_for_line = failed_boot
        with mock.patch.object(Amd64Default.Expectations, 'load', return_value=expectations('Smp4')):
            with self.assertRaisesRegex(CaseFailure, 'boot did not finish'):
                Amd64Default.run_image(self.context)
        frames, errors = Lan.load_capture(self.path)
        self.assertEqual([f.direction for f in frames], ['guest', 'peer'])
        self.assertEqual(errors, [])

    def test_console_callback_starts_script_and_peer_stops_before_judge(self):
        context = self.context
        context.lan = object()
        context.console.lines += ['USER:' + Lan.LISTENING_LINE, 'USER:' + Guest.BLOCK_THROTTLE_LINE]
        context.console.failures = lambda: []
        context.wait_for_line = mock.Mock(return_value=(0, 'line'))
        context.exited = lambda: None
        context.seconds = lambda value: value
        context.stop = mock.Mock()
        context.qmp = mock.Mock()
        peer = mock.Mock()
        peer.start.return_value = peer
        expected = SimpleNamespace(loop=SimpleNamespace(addresses={0}))
        state = SimpleNamespace(cpus=[SimpleNamespace(rip=0)])
        observation = mock.Mock()

        def poll(predicate, *args):
            for _ in range(3):
                self.assertTrue(predicate())

        def judge(*args):
            peer.stop.assert_called_once()

        with ExitStack() as stack:
            for target, changes in (
                    (Lan, {'LanPeer': mock.Mock(return_value=peer)}),
                    (Amd64Default.Expectations, {'load': mock.Mock(return_value=expected)}),
                    (Guest, {'wait': mock.Mock(), 'wait_for_spinner': mock.Mock(return_value=0),
                             'record': mock.Mock(), 'component_problems': mock.Mock(return_value=[])}),
                    (Amd64Default.Amd64Cpu, {'wait_for_idle': mock.Mock(return_value=(True, 'idle')),
                                            'capture': mock.Mock(return_value=state)}),
                    (Amd64Default.Observation, {'live': mock.Mock(return_value=observation)}),
                    (Ps2, {'Injection': mock.Mock(return_value=SimpleNamespace(sent=False, poll=mock.Mock(), check=mock.Mock()))}),
                    (Amd64Default, {'wait_until': mock.Mock(side_effect=poll),
                                    '_spinner_terminated': mock.Mock(return_value=True),
                                    '_check_display': mock.Mock(), '_check_scanout_mapping': mock.Mock(), 'judge': mock.Mock(side_effect=judge),
                                    'check_disk_images': mock.Mock()})):
                stack.enter_context(mock.patch.multiple(target, **changes))
            Amd64Default.run_image(context)
        self.assertEqual(peer.start_script.call_count, 3)
        self.assertEqual(peer.stop.call_count, 2)  # Pre-judge + idempotent finally cleanup
        context.qmp.execute.assert_called_once_with('block_set_io_throttle', Amd64Default.DISK_THROTTLE)
        observation.save.assert_called_once_with(context.artifact('Observation.json.gz'))


if __name__ == '__main__':
    unittest.main()
