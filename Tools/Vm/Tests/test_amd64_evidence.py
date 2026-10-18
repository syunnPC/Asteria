# SPDX-License-Identifier: Apache-2.0
"""The AMD64 evidence judgments (Observers.Amd64Evidence) on recorded records.

Every recorded record passes; changing one recorded value that a scenario
fixes (a count, a sequence, a mask, a state, a register, an address) fails
exactly the judgment of that sub-record. The runner's scenario definitions
and the build's constants are what the record is judged against.
"""
from __future__ import annotations

import dataclasses
import pathlib
import tempfile
import unittest

from AsteriaVm import Evidence, Guest, Lan, Machine
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Observers import Amd64Evidence
from .test_amd64_fixtures import (REPOSITORY, FakeContext, constants, expectations, observation, record, snapshot,
                                  with_evidence, edited_observation, field, record_address)


def failing(name: str, recorded) -> set[str]:
    with tempfile.TemporaryDirectory() as directory:
        Machine.write_disk_fixture(pathlib.Path(directory) / 'Disk.raw')
        context = FakeContext(name, pathlib.Path(directory))
        Amd64Default.judge(context, expectations(name), snapshot(name), recorded)
        failed = context.failed()
        # The NIC recordings predate LAN artifacts. Keep evidence mutation
        # assertions focused, but require the explicit missing-artifact failure.
        for check in Lan.CHECKS if context.case.guest else ():
            assert 'missing LAN artifact: Lan.pcap, Lan.json' in failed.pop(check)
        return set(failed)


class SchemaTests(unittest.TestCase):
    def test_the_amd64_record_is_described_by_its_headers(self):
        schema = Amd64Evidence.schema(REPOSITORY)
        self.assertEqual([field.name for field in schema.record.fields],
                         ['Header', 'Runtime', 'Pci', 'Ahci', 'ApplicationProcessors', 'SharedRootShootdown',
                          'NativePreemption', 'Wait', 'UserPreemption', 'Supervision', 'ObjectManagement',
                          'Threads', 'Components', 'Services', 'Iommu', 'Interrupts', 'DeviceControl',
                          'Collection', 'Block', 'Stops', 'IommuTopology', 'TscFrequencyHz', 'TscSource', 'X2Apic',
                      'IommuBackend', 'BootFiles', 'Metadata', 'Allocator', 'MemoryImport', 'Smp', 'ApplicationLaunch', 'Watchdog', 'Clocks', 'UserProcessors'])
        self.assertEqual((schema.version, schema.record.size), (39, 19160))
        self.assertEqual((schema.record.field('Stops').offset, schema.record.field('IommuTopology').offset),
                         (4008, 4600))
        self.assertEqual(schema.record.field('ApplicationProcessors').element.field('Processors').count, 4)
        self.assertEqual(schema.record.field('NativePreemption').element.field('Frame').element.name,
                         'Amd64TrapFrame')
        self.assertEqual(Amd64Evidence.outcomes(REPOSITORY),
                         {'None': 0, 'NoDisk': 1, 'Identified': 2, 'Read': 3, 'Unavailable': 4, 'UnsupportedPorts': 5, 'DiagnosticFailed': 6})

    def test_per_unit_state_capabilities_and_qmp_counts_are_checked(self):
        unit = Evidence.Record(RegisterBase=0xfed90000, State=2, ConfigureError=0, EnableError=0,
                               ReservedFunctions=0, ReservedPages=0, ServedFunctions=7, SweptFunctions=6,
                               DisabledReason=0, DroppedRequesters=0, Faults=1, Unattributed=0, Overflows=0,
                               Status=1 << 31, Levels=3, Capability=(6 << 8) | (47 << 16), Version=0x10)
        topology = Evidence.Record(Published=1, Count=1, IgnoredScopes=0, MalformedRmrr=0, Units=[unit])
        scenario = Amd64Evidence.IommuScenario(present=True, functions=7, swept=6, driver=0x20)
        self.assertEqual(Amd64Evidence.iommu_topology_problems(topology, scenario), [])
        for field in ('State', 'ReservedFunctions', 'Faults', 'ServedFunctions', 'SweptFunctions',
                      'DisabledReason', 'DroppedRequesters', 'RegisterBase', 'Levels', 'Version'):
            before = unit[field]
            unit[field] = before + (16 if field == 'Version' else 2 if field == 'Levels' else 1)
            self.assertTrue(Amd64Evidence.iommu_topology_problems(topology, scenario), field)
            unit[field] = before
        for status in (0, (1 << 31) | (1 << 26), (1 << 31) | (1 << 25)):
            unit['Status'] = status
            self.assertTrue(Amd64Evidence.iommu_topology_problems(topology, scenario))
        unit['Status'] = 1 << 31
        for capability in (0, 1 << 7, (2 << 8) | (1 << 7) | (47 << 16)):
            unit['Capability'] = capability
            self.assertTrue(Amd64Evidence.iommu_topology_problems(topology, scenario))
        unit['Capability'] = (6 << 8) | (47 << 16)
        for field in ('Count', 'IgnoredScopes', 'MalformedRmrr'):
            before = topology[field]
            topology[field] += 1
            self.assertTrue(Amd64Evidence.iommu_topology_problems(topology, scenario))
            topology[field] = before



COLLECTION_CHECK = 'host.amd64.evidence.collection'
BLOCK_CHECK = 'host.amd64.evidence.block'


class ReturnBoundaryScenarioTests(unittest.TestCase):
    def test_rejected_xcr0_thread_uses_existing_counts_without_new_layout(self):
        scenario = Amd64Evidence.ThreadScenario(self_test=True, killed_exit_code=123,
                                                return_boundary=True, entry_state=True)
        evidence = Evidence.Record(Published=1, Created=8, Destroyed=8, Reclaimed=8,
                                   Exits=2, Reaped=8, Lines=1, LastExitCode=123,
                                   FreeSlots=2, FreeNativeSlots=2, ThreadsUsage=0, Kills=2,
                                   RunStops=1, DestroyPolls=1, SlotMetadataBytes=1,
                                   Faults=4, Deferrals=0, Exceptions=3)
        self.assertEqual(Amd64Evidence.thread_problems(evidence, scenario), [])
        evidence.Reclaimed -= 1
        self.assertTrue(Amd64Evidence.thread_problems(evidence, scenario))
        supervision = Evidence.Record(Published=1, Terminations=2, SpinnerExited=0, SpinnerExitCode=0, Deferrals=0)
        self.assertEqual(Amd64Evidence.supervision_problems(supervision,
            Amd64Evidence.SupervisionScenario(spinner_faults=False, return_boundary=True, entry_state=True)), [])

    def test_boundary_unmaps_leave_the_pager_window_empty(self):
        with tempfile.TemporaryDirectory() as directory:
            context = FakeContext('Smp4', pathlib.Path(directory))
            self.assertIn(Guest.AMD64_RETURN_BOUNDARY_LINE, Guest.channel_lines(context.console.lines))
            management = Evidence.Record(record('Smp4').ObjectManagement)
            self.assertTrue(Amd64Default._all_unmapped(context, management, constants(), expectations('Smp4')))
            management.Unmappings -= 1
            self.assertFalse(Amd64Default._all_unmapped(context, management, constants(), expectations('Smp4')))

    def test_boundary_thread_is_counted_and_reclaimed(self):
        scenario = Amd64Evidence.ThreadScenario(self_test=True, killed_exit_code=123, return_boundary=True)
        evidence = Evidence.Record(Published=1, Created=7, Destroyed=7, Reclaimed=7,
                                   Exits=2, Reaped=7, Lines=1, LastExitCode=123,
                                   FreeSlots=2, FreeNativeSlots=2, ThreadsUsage=0, Kills=2,
                                   RunStops=1, DestroyPolls=1, SlotMetadataBytes=1,
                                   Faults=3, Deferrals=0, Exceptions=2)
        self.assertEqual(Amd64Evidence.thread_problems(evidence, scenario), [])
        self.assertTrue(Amd64Evidence.thread_problems(evidence, dataclasses.replace(scenario, return_boundary=False)))
        evidence.Reclaimed = 6
        self.assertTrue(Amd64Evidence.thread_problems(evidence, scenario))

    def test_boundary_termination_is_counted(self):
        scenario = Amd64Evidence.SupervisionScenario(spinner_faults=True, thread_faults=True,
                                                    exit_code=123, return_boundary=True)
        evidence = Evidence.Record(Published=1, Terminations=4, SpinnerExited=1, SpinnerExitCode=123, Deferrals=0)
        self.assertEqual(Amd64Evidence.supervision_problems(evidence, scenario), [])
        self.assertTrue(Amd64Evidence.supervision_problems(evidence, dataclasses.replace(scenario, return_boundary=False)))


class RecordMutationTests(unittest.TestCase):
    """One edited value of a recorded passing record fails its sub-record's check."""

    def assertDetected(self, name: str, mutants: dict[str, tuple[str, int]], check: str):
        self.assertEqual(failing(name, observation(name)), set(), name)
        for description, (path, delta) in mutants.items():
            with self.subTest(description):
                # The collection check sums every other sub-record's reclaimed
                # count; the block check counts the pins released.
                summed = {COLLECTION_CHECK} if path.endswith('Reclaimed') and check != COLLECTION_CHECK else set()
                summed |= {BLOCK_CHECK} if path == 'DeviceControl.PinsReleased' else set()
                if path in ('Allocator.ManagedPages', 'Allocator.AllocatablePages', 'Allocator.CleanPages'):
                    summed.add('host.amd64.evidence.memory-import')
                self.assertEqual(failing(name, with_evidence(name, path, lambda value: value + delta)),
                                 {check} | summed)

    def test_runtime(self):
        self.assertDetected('Smp4', {'a user root outside its arena': ('Runtime.UserRootPhysicalAddress', 0x1000),
                                     'unpublished': ('Runtime.Published', -1)},
                            'host.amd64.evidence.runtime')

    def test_the_pool_and_the_local_apic_place_what_the_host_walks(self):
        """The recorded pool and local APIC are where the host looks for the
        stacks, the probe page, the MMIO window's target, the scrub slots and
        the frame pool (the pager's first Frame starts it)."""
        self.assertEqual(failing('Smp4', with_evidence('Smp4', 'Runtime.LocalApicPhysicalAddress',
                                                       lambda value: value + 0x1000)),
                         {'host.amd64.paging.mmio-window'})
        self.assertEqual(failing('Smp4', with_evidence('Smp4', 'Runtime.PoolPhysicalStart', lambda value: value + 8)),
                         {'host.amd64.evidence.runtime', 'host.amd64.evidence.shared-root-shootdown',
                         'host.amd64.cpu.ap-entry-context', 'host.amd64.paging.scrub-slots',
                         'host.amd64.evidence.allocator', 'host.amd64.cpu.ap-registers'}
                         | ({'host.amd64.evidence.user-processors'} if 'UserProcessors' in record('Smp4') else set()))

    def test_processors(self):
        self.assertDetected('Smp4', {'a lost processor': ('Runtime.ProcessorCount', -1),
                                     'a lost application processor': ('ApplicationProcessors.ProcessorCount', -1)},
                            'host.amd64.evidence.processors')

    def test_pci(self):
        self.assertDetected('Smp4', {'a bus less': ('Pci.BusCount', -1),
                                     'another first function': ('Pci.FirstDevice', 1),
                                     'another first vendor/device': ('Pci.FirstVendorDevice', 1 << 16)},
                            'host.amd64.evidence.pci-ecam')
        self.assertDetected('Smp4', {'a function lost': ('Pci.PublishedFunctionCount', -1),
                                     'another AHCI function': ('Pci.AhciDevice', 1),
                                     'another AHCI vendor/device': ('Pci.AhciVendorDevice', 1),
                                     'another class': ('Pci.AhciProgrammingInterface', -1),
                                     'AHCI not ready': ('Pci.AhciAvailability', -1)},
                            'host.amd64.evidence.pci-ahci-function')

    def test_iommu(self):
        self.assertDetected('Smp4', {'a function unserved': ('Iommu.ServedFunctions', -1),
                                     'a function left mastering': ('Iommu.SweptFunctions', -1),
                                     'not translating': ('Iommu.Enabled', -1),
                                     'a fault': ('Iommu.Faults', 1),
                                     'an enabling error': ('Iommu.EnableError', 1),
                                     'two levels': ('Iommu.Levels', -1),
                                     'the stray write elsewhere': ('Iommu.LastFaultAddress', 0x1000),
                                     'another requester faulted': ('Iommu.LastFaultSource', 8),
                                     'a read faulted': ('Iommu.LastFaultRead', 1),
                                     'a fault of no domain': ('Iommu.Unattributed', 1),
                                     'another reason': ('Iommu.LastFaultReason', 1),
                                     'the domain left standing': ('Iommu.DomainsReclaimed', -1),
                                     'unpublished': ('Iommu.Published', -1)},
                            'host.amd64.evidence.iommu')

    def test_interrupts(self):
        self.assertDetected('Smp4', {'an Interrupt left standing': ('Interrupts.Reclaimed', -1),
                                     'one more created': ('Interrupts.Created', 1),
                                     'one live': ('Interrupts.Live', 1),
                                     'more than the crash, the NIC and the PS/2 lines may leave': ('Interrupts.Delivered', 1000),
                                     'two spurious': ('Interrupts.Spurious', 2),
                                     'unpublished': ('Interrupts.Published', -1)},
                            'host.amd64.evidence.interrupts')
        # The NIC's messages lie in a range, so the least the drivers need is
        # what a lost message must fall below.
        self.assertEqual(failing('Smp4', with_evidence('Smp4', 'Interrupts.Delivered', lambda _: 0)),
                         {'host.amd64.evidence.interrupts'})

    def test_device_control(self):
        self.assertDetected('Smp4', {'a quarantine left standing': ('DeviceControl.Confirmations', -1),
                                     'a drain that did not quarantine': ('DeviceControl.Quarantines', -1),
                                     'one more assignment': ('DeviceControl.Assignments', 1),
                                     'a pin still parked': ('DeviceControl.ParkedPins', 1),
                                     'a parked pin never released': ('DeviceControl.PinsReleased', -1),
                                     'unpublished': ('DeviceControl.Published', -1)},
                            'host.amd64.evidence.device-control')

    def test_retired_dma_pins_are_balanced_and_bounded(self):
        name = 'Ahci'
        baseline = record(name).DeviceControl.PinsParked

        def changed(total, released=None, still=0):
            def edit(memory):
                for path, value in (
                    ('DeviceControl.PinsParked', total),
                    ('DeviceControl.PinsReleased', total if released is None else released),
                    ('DeviceControl.ParkedPins', still),
                    ('Block.Releases', total if released is None else released)):
                    offset, size = field(path)
                    memory.write(record_address(name) + offset, value.to_bytes(size, 'little'))
            return edited_observation(name, edit)

        # Normal Unmap can retire further extents. The total changes, but
        # every pin must still be released and the mapping bound still holds.
        self.assertEqual(failing(name, changed(baseline + 10)), set())
        # Keep both scrub samples visible while dropping mandatory driver pins.
        for total in (Amd64Evidence.BLOCK_RELEASES_KEPT, 1_000_000):
            self.assertEqual(failing(name, changed(total)), {'host.amd64.evidence.device-control'})
        for recorded in (changed(baseline + 10, released=baseline + 9), changed(baseline + 10, still=1)):
            self.assertEqual(failing(name, recorded), {'host.amd64.evidence.device-control'})

    def test_block(self):
        self.assertDetected('Smp4', {'a release not recorded': ('Block.Releases', -1),
                                     'unpublished': ('Block.Published', -1)},
                            BLOCK_CHECK)
        # With the disk, the crash's Frame held the token when it was scrubbed;
        # the record keeps the last releases in a ring, so its slot follows
        # how many pins were released before.
        token = int.from_bytes(Machine.DISK_TOKEN[:8], 'little')
        slot = next(index for index, released in enumerate(record('Ahci').Block.Released)
                    if released.Sample[0] == token)
        self.assertDetected('Ahci', {'the token not found': ('Block.Released[%d].Sample[0]' % slot, 1),
                                     'not sampled': ('Block.Released[%d].Sampled' % slot, -1)},
                            BLOCK_CHECK)

    def test_collection(self):
        self.assertDetected('Smp4', {'a reclamation no collection made': ('Collection.Reclaimed', -1),
                                     'a run longer than the pool hands out': ('Collection.ScrubbedPages', 10 ** 6),
                                     'unpublished': ('Collection.Published', -1)},
                            'host.amd64.evidence.collection')

    def test_ahci(self):
        self.assertDetected('Ahci', {'another LBA': ('Ahci.ReadStartingLba', 1),
                                     'two sectors': ('Ahci.ReadSectorCount', 1),
                                     'another buffer': ('Ahci.ReadBufferPhysicalAddress', 0x1000),
                                     'only identified': ('Ahci.Outcome', -1)},
                            'host.amd64.evidence.ahci-read')
        self.assertDetected('Ahci', {'another port': ('Ahci.PortNumber', 1),
                                     'another serial (IDENTIFY word 10)': ('Ahci.IdentifyData[20]', 1)},
                            'host.amd64.evidence.ahci-identify')
        self.assertDetected('Smp4', {'a disk identified without one': ('Ahci.Outcome', 1)},
                            'host.amd64.evidence.ahci-identify')

    def test_application_processors(self):
        cpu = 'ApplicationProcessors.Processors[2].'
        self.assertDetected('Smp4', {
            'not serving': (cpu + 'State', 1),
            'a second timer interrupt': (cpu + 'TimerInterrupts', 1),
            'no delivery': (cpu + 'TimerDeliveries', -1),
            'the worker ran twice before blocking': (cpu + 'Timer.InitialRuns', 1),
            'the interrupt before its deadline': (cpu + 'Timer.RequestedDeadlineTicks', 1 << 40),
            'two expirations delivered': (cpu + 'Timer.DeliveredCount', 1),
            'the drain stopped early': (cpu + 'Timer.DrainStop', 1),
            'another timer delivered': (cpu + 'Timer.DeliveredOwnTimer', -1),
            'a timer left queued': (cpu + 'Timer.QueuePendingEntries', 1),
            'the second claim': (cpu + 'Timer.NotificationClaimSequence', 1),
            'a signal left pending': (cpu + 'Timer.NotificationPending', 1),
            'the deadline still armed': (cpu + 'Timer.DisarmedDeadlineState', 1),
            'the timer unmasked': (cpu + 'Timer.DisarmedTimerLvt', -0x10000),
            'a countdown left': (cpu + 'Timer.DisarmedInitialCount', 1),
            'a third run': (cpu + 'ServiceRuns', 1),
            'AP unpublished': (cpu + 'Published', -1),
            'BSP slot published': ('ApplicationProcessors.Processors[0].Published', 1),
            'serving mask': ('ApplicationProcessors.ServingProcessorMask', -2),
        }, 'host.amd64.evidence.ap-runtime')

    def test_shootdown(self):
        path = 'SharedRootShootdown.'
        self.assertDetected('Smp4', {
            'a CPU did not acknowledge': (path + 'AcknowledgedProcessorMask', -8),
            'another page': (path + 'VirtualAddress', 0x1000),
            'another value': (path + 'Value', 1),
            'queued to the BSP': (path + 'QueuedProcessorMask', 1),
            'an AP read another word': (path + 'ObservedValues[3]', 1),
            'the BSP read another word': (path + 'ObservedValues[0]', 1),
            'a fourth run': (path + 'ServiceRuns[1]', 1),
        }, 'host.amd64.evidence.shared-root-shootdown')

    def test_wait(self):
        self.assertDetected('Ahci', {
            'another tag': ('Wait.RegisteredTag', 1),
            'a third wait': ('Wait.WaitCalls', 1),
            'a wait that did not suspend': ('Wait.SuspendedWaits', -1),
            'two timer interrupts': ('Wait.Resumptions[1].ObservedInterrupts', 1),
            'the interrupt before the deadline': ('Wait.Resumptions[0].DeadlineTicks', 1 << 40),
            'no expiration delivered': ('Wait.Resumptions[0].DeliveredCount', -1),
            'another completion tag': ('Wait.Resumptions[1].CompletedTag', 1),
            'another registration': ('Wait.Resumptions[0].CompletedRegistrationSequence', 1),
            'an error response': ('Wait.Resumptions[1].ResponseError', 2),
            'another response value': ('Wait.Resumptions[0].ResponseValue1', 1),
            'a resumption beyond the count': ('Wait.Resumptions[2].DeliveredCount', 1),
        }, 'host.amd64.evidence.wait')

    def test_user_preemption(self):
        path = 'UserPreemption.'
        self.assertDetected('Smp4', {
            'unpublished': (path + 'Published', -1),
            'no spinner': (path + 'SpinnerPresent', -1),
            'a second spinner line': (path + 'SpinnerLines', 1),
            'a spinner that blocked instead of faulting': (path + 'SpinnerWaitSuspended', 1),
            'more runs ended than user ticks': (path + 'RunStops[0]', 1 << 20),
            'more TimerQueue ticks than runs ended': (path + 'TimerQueueTicks', 1 << 20),
        }, 'host.amd64.evidence.user-preemption')
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'RunStops[2]', lambda value: 0)),
                         {'host.amd64.evidence.user-preemption'}, 'a spinner never preempted')
        self.assertDetected('PreemptionRelocated', {
            'an extra self-test termination': ('Supervision.Terminations', 1),
            'a deferred fault in the production firmware': ('Supervision.Deferrals', 1)},
            'host.amd64.evidence.supervision')
        self.assertDetected('PreemptionRelocated', {
            'a spinner in the production firmware': (path + 'SpinnerPresent', 1),
            'spinner lines in the production firmware': (path + 'SpinnerLines', 1),
        }, 'host.amd64.evidence.user-preemption')

    def test_supervision(self):
        path = 'Supervision.'
        self.assertDetected('Smp4', {
            'unpublished': (path + 'Published', -1),
            'a third termination': (path + 'Terminations', 1),
            'a termination missing': (path + 'Terminations', -1),
            'a spinner that did not exit': (path + 'SpinnerExited', -1),
            'another exit code': (path + 'SpinnerExitCode', 1),
        }, 'host.amd64.evidence.supervision')

    def test_object_management(self):
        path = 'ObjectManagement.'
        self.assertDetected('Smp4', {
            'unpublished': (path + 'Published', -1),
            'a third Endpoint': (path + 'Created', 1),
            'one Endpoint not destroyed': (path + 'Destroyed', -1),
            'one Endpoint not reclaimed': (path + 'Reclaimed', -1),
            'an Endpoint left in the pool': (path + 'OccupiedSlots', 1),
            'a charge left behind': (path + 'MetadataBytes', 64),
            'a queue charge left behind': (path + 'IpcQueueBytes', 64),
            'a mapping outside the window': (path + 'MappedVirtualStart', 0x1000),
            'a page charge left behind': (path + 'CommittedPages', 2),
            'the mapped pair still occupying its slots': (path + 'OccupiedSlots', 2),
            'the mapped Frame not reclaimed': (path + 'FramesReclaimed', -1),
            'pages left dirty': (path + 'AllocatorDirtyPages', 2),
            'pages missing from the free set': (path + 'AllocatorCleanPages', -2),
        }, 'host.amd64.evidence.object-management')

        # The host walks the window, and reads the run the record names, so a
        # mapping left in place or a run outside the frame pool fails both; a
        # mapping counted twice leaves the window as the walk finds it.
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'Unmappings', lambda value: value - 1)),
                         {'host.amd64.evidence.object-management', 'host.amd64.paging.pager-window'}, 'no unmapping')
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'Mappings', lambda value: value + 1)),
                         {'host.amd64.evidence.object-management'}, 'a second mapping')
        pool_end = Amd64Default.frame_pool(constants(), record('Smp4').Runtime.PoolPhysicalStart)[1]
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'MappedPhysicalStart',
                                                       lambda _: pool_end)),
                         {'host.amd64.evidence.object-management', 'host.amd64.paging.pager-window'},
                         'a mapped run outside the frame pool')
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'RegisteredObjects', lambda value: 0)),
                         {'host.amd64.evidence.object-management'}, 'no registered objects')
        # With a component, the peak must cover one live domain's spaces.
        spaces = Amd64Evidence.domain_bytes(record('Smp4').Components)
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'PeakMetadataBytes', lambda value: spaces - 64)),
                         {'host.amd64.evidence.object-management'}, 'a peak below a domain\'s spaces')
        self.assertDetected('PreemptionRelocated', {
            'an extra resident endpoint': (path + 'Created', 1),
            'a charge in the production firmware': (path + 'PeakMetadataBytes', 64),
            'an extra resident mapping': (path + 'Mappings', 1),
        }, 'host.amd64.evidence.object-management')

    def test_allocator_creation_evidence(self):
        self.assertDetected('Smp4', {
            'unpublished allocator': ('Allocator.Published', -1),
            'managed range changed': ('Allocator.ManagedPages', 1),
            'allocatable partition changed': ('Allocator.AllocatablePages', 1),
            'clean page lost': ('Allocator.CleanPages', -1),
            'ownerless slot retained': ('Allocator.ZeroingSlots', 1),
            'no peak Zeroing slot': ('Allocator.PeakZeroingSlots', -1),
            'no creation zero': ('Allocator.CreationZeroedPages', -record('Smp4').Allocator.CreationZeroedPages),
            'creation zero used another root': ('Allocator.CreationZeroRoot', 4096),
        }, 'host.amd64.evidence.allocator')

    def test_threads(self):
        path = 'Threads.'
        self.assertDetected('Smp4', {
            'unpublished': (path + 'Published', -1),
            'an eighth thread': (path + 'Created', 1),
            'a thread not reclaimed': (path + 'Destroyed', -1),
            'a pool slot not freed': (path + 'Reclaimed', -1),
            'no exit': (path + 'Exits', -1),
            'a second exit': (path + 'Exits', 1),
            'a native slot not reaped': (path + 'Reaped', -1),
            'a second line': (path + 'Lines', 1),
            'another exit code': (path + 'LastExitCode', 1),
            'a pool slot still held': (path + 'FreeSlots', -1),
            'a native slot still held': (path + 'FreeNativeSlots', -1),
            'a Threads charge left behind': (path + 'ThreadsUsage', 1),
            'no kill': (path + 'Kills', -1),
            'a third kill': (path + 'Kills', 1),
        }, 'host.amd64.evidence.threads')
        for label, field_ in (('never preempted', 'RunStops'), ('never refused', 'DestroyPolls'),
                              ('no fault', 'Faults'), ('no exception', 'Exceptions')):
            self.assertEqual(failing('Smp4', with_evidence('Smp4', path + field_, lambda value: 0)),
                             {'host.amd64.evidence.threads'}, label)
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'Deferrals', lambda value: 1 << 20)),
                         {'host.amd64.evidence.threads'}, 'every fault deferred')
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'Exceptions', lambda value: 1 << 20)),
                         {'host.amd64.evidence.threads'}, 'every fault an exception')
        # With a component the peak covers the domains, not one thread, so a
        # different thread charge changes no judgment.
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'SlotMetadataBytes', lambda value: value + 64)),
                         set())
        self.assertDetected('PreemptionRelocated', {
            'an extra resident thread': (path + 'Created', 1),
            'an extra free native slot': (path + 'FreeNativeSlots', 1),
            'a fault in the production firmware': (path + 'Faults', 1),
            'a deferral in the production firmware': (path + 'Deferrals', 1),
            'an exception in the production firmware': (path + 'Exceptions', 1),
        }, 'host.amd64.evidence.threads')
        # Init keeps native threads; scheduling counts vary after readiness.
        for field_ in ('SlotMetadataBytes', 'RunStops', 'DestroyPolls'):
            self.assertEqual(failing('PreemptionRelocated', with_evidence(
                'PreemptionRelocated', path + field_, lambda _: 0)),
                {'host.amd64.evidence.threads'}, field_)

    def test_components(self):
        path = 'Components.'
        self.assertDetected('Smp4', {
            'unpublished': (path + 'Published', -1),
            'a fourth domain': (path + 'DomainsCreated', 1),
            'a domain not destroyed': (path + 'DomainsDestroyed', -1),
            'a domain not reclaimed': (path + 'DomainsReclaimed', -1),
            'the reporting incarnation crashed': (path + 'LastTag', 1),
            'an AddressSpace not reclaimed': (path + 'AddressSpacesReclaimed', -1),
            'a CapabilitySpace not reclaimed': (path + 'CapabilitySpacesReclaimed', -1),
            'an arena not prepared again': (path + 'ArenasPrepared', -1),
            'an AddressSpace slot spent': (path + 'AddressSpacesFree', -1),
            'a CapabilitySpace slot spent': (path + 'CapabilitySpacesFree', -1),
            'a component without a ResourceDomain': (path + 'ResourceDomainsCreated', -1),
            'a ResourceDomain not drained': (path + 'ResourceDomainsDestroyed', -1),
            'a ResourceDomain not reclaimed': (path + 'ResourceDomainsReclaimed', -1),
            'a ResourceDomain left in its slot': (path + 'ResourceDomainsLive', 1),
        }, 'host.amd64.evidence.components')
        # The NIC driver's exit can race its drain, so one less than the
        # captured count may still lie in the accepted range. Mutate outside
        # both bounds rather than assuming a fixture always sits at the minimum.
        for exits in (0, 1 << 20):
            self.assertEqual(failing('Smp4', with_evidence(
                'Smp4', path + 'DomainExits', lambda _, exits=exits: exits)),
                {'host.amd64.evidence.components'}, 'domain exits outside the scenario range')
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'TaggedFaults', lambda value: 0)),
                         {'host.amd64.evidence.components'}, 'no crash delivered')
        # The pager's self-test waits expire too, so only none at all is short.
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'WaitExpiries', lambda value: 0)),
                         {'host.amd64.evidence.components'}, 'no watchdog expiry')
        # A live space keeps its charge in the grant too.
        self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'CapabilitySpacesLive', lambda value: value + 1)),
                         {'host.amd64.evidence.components', 'host.amd64.evidence.object-management'})
        self.assertDetected('PreemptionRelocated', {
            'an extra resident domain': (path + 'DomainsCreated', 1),
            'an extra resident ResourceDomain': (path + 'ResourceDomainsCreated', 1),
            'duplicate publication': (path + 'Published', 1),
        }, 'host.amd64.evidence.components')

    def test_services(self):
        path = 'Services.'
        self.assertDetected('Smp4', {
            'unpublished': (path + 'Published', -1),
            'a WaitSet not created': (path + 'WaitSetsCreated', -1),
            'a WaitSet not reclaimed': (path + 'WaitSetsReclaimed', -1),
            'a bind': (path + 'Binds', 1),
            'a wait not aborted': (path + 'WaitAborts', -1),
            'an aborted thread that returned to user mode': (path + 'AbortedExits', -1),
        }, 'host.amd64.evidence.services')
        # How often the services and the client block depends on the order
        # they ran in, so the recorded count lies anywhere in its range: a
        # count below every thread's last wait, or above one block per
        # request and reply, fails.
        for description, count in (('no wait blocked', 0), ('a block for each step and more', 100000)):
            with self.subTest(description):
                self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'WaitsBlocked', lambda _: count)),
                                 {'host.amd64.evidence.services'})
        # The NIC driver's backstop ends its waits every 10 ms, so the count
        # lies in a range too: none expired (not even the pager's first
        # thread's), or more than its whole demonstration allows, fails.
        for description, count in (('no wait expired', 0), ('more expiries than the backstop allows', 100000)):
            with self.subTest(description):
                self.assertEqual(failing('Smp4', with_evidence('Smp4', path + 'WaitExpiries', lambda _: count)),
                                 {'host.amd64.evidence.services'})
        self.assertDetected('PreemptionRelocated', {
            'an extra resident WaitSet': (path + 'WaitSetsCreated', 1),
        }, 'host.amd64.evidence.services')
        self.assertEqual(failing('PreemptionRelocated', with_evidence(
            'PreemptionRelocated', path + 'WaitsBlocked', lambda _: 0)),
            {'host.amd64.evidence.services'}, 'resident services never blocked')

    def test_product_residents(self):
        self.assertDetected('PreemptionRelocated', {
            'a retained frame missing': ('ObjectManagement.FramesCreated', -1),
            'a private page uncharged': ('ObjectManagement.CommittedPages', -1),
            'a queue uncharged': ('ObjectManagement.IpcQueueBytes', -1),
            'a staging unmap missing': ('ObjectManagement.Unmappings', -1),
            'a resident slot missing': ('ObjectManagement.OccupiedSlots', -1),
            'a retained charge lost': ('ObjectManagement.MetadataBytes', -1),
        }, 'host.amd64.evidence.object-management')
        self.assertDetected('PreemptionRelocated', {
            'a service drained': ('Components.ResourceDomainsDestroyed', 1),
            'a service missing': ('Components.AddressSpacesLive', -1),
        }, 'host.amd64.evidence.components')
        self.assertDetected('PreemptionRelocated', {
            'PS/2 binding missing': ('Interrupts.Live', -1),
            'PS/2 unexpectedly reclaimed': ('Interrupts.Reclaimed', 1),
        }, 'host.amd64.evidence.interrupts')

    def test_native_preemption(self):
        path = 'NativePreemption.'
        self.assertDetected('PreemptionRelocated', {
            'two captures': (path + 'CaptureCount', 1),
            'no resume': (path + 'ResumeCount', -1),
            'a lost sentinel': (path + 'Registers[4]', 1),
            'a restored register that differs from the capture': (path + 'Frame.R12', 1),
            'another stack pointer after resuming': (path + 'Registers[15]', 8),
            'readmitted as the stopped run': (path + 'AdmissionRunSequence', -1),
            'budget left': (path + 'RemainingBudgetTicks', 1),
            'a second token': (path + 'ExternalFrameToken', 1),
            'another vector': (path + 'Frame.Vector', 1),
            'captured outside the loop': (path + 'LoopStart', 0x100),
            'unpublished': (path + 'Published', -1),
        }, Amd64Default.PREEMPTION_CHECK)
        self.assertDetected('Smp4', {'a proof in a default build': (path + 'Published', 1)},
                            Amd64Default.UNREACHED_CHECK)


class ScenarioExpectationTests(unittest.TestCase):
    """The runner's scenario definitions are what the record is judged against."""

    def test_a_wrong_wait_scenario_fails_the_wait_check(self):
        evidence = record('Smp4').Wait
        self.assertEqual(Amd64Evidence.wait_problems(evidence, constants()), [])
        problems = Amd64Evidence.wait_problems(evidence, constants(), Amd64Evidence.WaitScenario(waits=3))
        self.assertEqual(problems, ['WaitCalls is 0x2, expected 0x3', 'SuspendedWaits is 0x2, expected 0x3',
                                    'ResumedWaits is 0x2, expected 0x3'])

    def test_a_wrong_ap_scenario_fails_every_ap(self):
        evidence = record('Smp4').ApplicationProcessors
        wrong = Amd64Evidence.ApplicationProcessorScenario(service_runs=3)
        problems = Amd64Evidence.application_processors_problems(evidence, 4, constants(), wrong)
        self.assertEqual(problems, ['CPU %d: ServiceRuns is 0x2, expected 0x3' % cpu for cpu in (1, 2, 3)])

    def test_the_masked_lvt_comes_from_the_timer_header(self):
        evidence = record('Smp4').ApplicationProcessors
        changed = dataclasses.replace(constants(), timer_masked_lvt=constants().timer_masked_lvt ^ 0x10000)
        problems = Amd64Evidence.application_processors_problems(evidence, 4, changed)
        self.assertEqual(len(problems), 3)
        self.assertTrue(all('DisarmedTimerLvt' in problem for problem in problems))

    def test_a_readmission_after_a_rolled_back_one_is_a_later_run(self):
        later = with_evidence('PreemptionRelocated', 'NativePreemption.AdmissionRunSequence', lambda value: value + 1)
        self.assertEqual(failing('PreemptionRelocated', later), set())

    def test_the_sentinels_come_from_the_probe_header(self):
        evidence = record('PreemptionRelocated').NativePreemption
        self.assertEqual(Amd64Evidence.native_preemption_problems(evidence, constants()), [])
        sentinels = list(constants().preemption_sentinels)
        sentinels[0] += 1
        changed = dataclasses.replace(constants(), preemption_sentinels=tuple(sentinels))
        [problem] = Amd64Evidence.native_preemption_problems(evidence, changed)
        self.assertIn('the sentinel registers', problem)


if __name__ == '__main__':
    unittest.main()
