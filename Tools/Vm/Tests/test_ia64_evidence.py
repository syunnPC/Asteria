# SPDX-License-Identifier: Apache-2.0
"""The IA-64 evidence judgments (Observers.Ia64Evidence) on recorded records.

Every recorded record passes; changing one recorded value that a scenario
fixes (a sequence, a count, a cookie echo, a mask, a state) fails exactly
the judgment of that sub-record.
"""
from __future__ import annotations
from Tests.test_ia64_fixtures import fixture_schema

import dataclasses
import unittest

from AsteriaVm import Evidence, Ia64Layout, Machine
from AsteriaVm.Drivers import Ia64Default, Ia64NativePreemption, Ia64SyntheticProbe
from AsteriaVm.Observers import Ia64Evidence
from Tests.test_ia64_fixtures import ROOT, evidence_bytes, expectation, failing, observation


def offset_of(path: str) -> tuple[int, int]:
    """(offset, size) of a field of the IA-64 record, named like
    `ApplicationProcessors.Processors[1].ServiceCount`."""
    structure, offset = fixture_schema().record, 0
    parts = path.split('.')
    for position, part in enumerate(parts):
        name, _, index = part.partition('[')
        field = structure.field(name)
        offset += field.offset
        if index:
            offset += int(index.rstrip(']')) * field.element_size
        if position + 1 < len(parts):
            structure = field.element
        else:
            return offset, field.element_size if index or field.count is None else field.size
    raise AssertionError(path)


def with_value(data: bytes, path: str, value: int) -> bytes:
    offset, size = offset_of(path)
    return data[:offset] + value.to_bytes(size, 'little') + data[offset + size:]


def value_of(data: bytes, path: str) -> int:
    offset, size = offset_of(path)
    return int.from_bytes(data[offset:offset + size], 'little')


COLLECTION_CHECK = 'host.ia64.evidence.collection'
BLOCK_CHECK = 'host.ia64.evidence.block'


class RecordMutationTests(unittest.TestCase):
    """One edited value of a recorded passing record fails its sub-record's check."""

    def evaluate(self, name: str, data: bytes, evaluate=Ia64Default.evaluate) -> set[str]:
        recorded = observation(name)
        recorded.memory[Ia64Evidence.DUMP] = data
        return set(failing(evaluate(recorded, expectation(name))))

    def test_clock_placement_exclusion_corruption(self):
        data = evidence_bytes('Smp4')
        self.assertEqual(self.evaluate('Smp4', data), set())
        mutant = with_value(data, 'Clocks.ExcludedMask', value_of(data, 'Clocks.ExcludedMask') ^ 1)
        self.assertEqual(self.evaluate('Smp4', mutant), {'host.ia64.evidence.clocks'})

    def assertDetected(self, name: str, mutants: dict[str, tuple[str, int]], check: str, evaluate=Ia64Default.evaluate, additional: tuple[str,...] = ()):
        data = evidence_bytes(name)
        self.assertEqual(self.evaluate(name, data, evaluate), set(), name)
        for description, (path, delta) in mutants.items():
            with self.subTest(description):
                mutant = with_value(data, path, value_of(data, path) + delta)
                self.assertNotEqual(mutant, data, path)
                # The collection check sums every other sub-record's reclaimed count.
                summed = {COLLECTION_CHECK} if path.endswith('Reclaimed') and check != COLLECTION_CHECK else set()
                self.assertEqual(self.evaluate(name, mutant, evaluate), {check} | summed | set(additional))

    def test_iommu(self):
        data = evidence_bytes('Smp4')
        self.assertEqual(self.evaluate('Smp4', data), set())
        boot, driver = 'host.ia64.evidence.iommu-boot', 'host.ia64.evidence.iommu'
        for path, delta, checks in (
            ('Iommu.Bypass', 1, {boot, driver}),
            ('Iommu.QueuePhysical', 0x10000000, {boot}),
            ('Iommu.LastFaultReason', 1, {driver}),
            ('Iommu.LastFaultAddress', 4096, {driver}),
            ('Iommu.DomainsReclaimed', -1, {driver, COLLECTION_CHECK}),
        ):
            with self.subTest(path=path):
                mutant = with_value(data, path, value_of(data, path) + delta)
                self.assertEqual(self.evaluate('Smp4', mutant), checks)

    def test_processors(self):
        self.assertDetected('Smp4', {'a lost processor': ('Processors.ProcessorCount', -1),
                                     'a mask with an extra CPU': ('Processors.EnabledProcessorMask', 0x10),
                                     'unpublished': ('Processors.Published', -1)},
                            'host.ia64.evidence.processors')

    def test_pci(self):
        self.assertDetected('Smp4', {'another first function': ('Pci.FirstDevice', 1),
                                     'another first vendor/device': ('Pci.FirstVendorDevice', 1 << 16)},
                            'host.ia64.evidence.pci-ecam-first-function')
        self.assertDetected('Smp4', {'a function lost': ('Pci.PublishedFunctionCount', -1)},
                            'host.ia64.evidence.pci-ahci-function',
                            additional=('host.ia64.evidence.iommu-boot', 'host.ia64.evidence.iommu'))
        self.assertDetected('Smp4', {'another AHCI function': ('Pci.AhciDevice', 1),
                                     'another class': ('Pci.AhciProgrammingInterface', -1)},
                            'host.ia64.evidence.pci-ahci-function')

    def test_application_processors(self):
        cpu = 'ApplicationProcessors.Processors[2].'
        self.assertDetected('Smp4', {
            'proof sequence': (cpu + 'LocalTimerInterrupt.Sequence', 1),
            'proof not acknowledged': (cpu + 'LocalTimerAcknowledgedSequence', -1),
            'task priority': (cpu + 'LocalTimerTaskPriority', 0x10),
            'polling grace': (cpu + 'LocalTimerPollDeadlineTicks', 1),
            'polling leaf': (cpu + 'LocalTimerPollCodeEnd', 16),
            'interrupt outside the leaf': (cpu + 'LocalTimerInterrupt.InterruptedInstructionPointer', 0x10000),
            'interrupt on another CPU': (cpu + 'LocalTimerInterrupt.ProcessorIndex', 1),
            'run sequence': (cpu + 'FirstResumedRunSequence', 1),
            'queued timer': (cpu + 'TimerQueuePendingEntries', 1),
            'run-loop bindings': (cpu + 'RunLoopBindingCount', 1),
            'cookie not echoed': (cpu + 'ServiceCompletionCookie', 1),
            'service count': (cpu + 'ServiceCount', 1),
            'dispatch count': (cpu + 'ServiceDispatchCount', 1),
            'completion value': (cpu + 'ServiceCompletionValue', 16),
            'delivery claim': (cpu + 'DeliveryClaimSequence', 1),
            'drained twice': (cpu + 'DeliveryDrainedCount', 1),
            'queue position': (cpu + 'DeliveryFinalSchedulerNextSequence', 1),
            'delivery interrupt sequence': (cpu + 'DeliveryInterrupt.Sequence', 1),
            'delivery interrupt before its deadline': (cpu + 'DeliveryInterrupt.ProgrammedDeadlineTicks', 1 << 40),
            'delivery not acknowledged': (cpu + 'DeliveryAcknowledgedSequence', -1),
            'AP unpublished': (cpu + 'Published', -1),
            'BSP slot published': ('ApplicationProcessors.Processors[0].Published', 1),
            'serving mask': ('ApplicationProcessors.ServingProcessorMask', -2),
        }, 'host.ia64.evidence.ap-runtime')

    def test_shootdown(self):
        self.assertDetected('Smp4', {'a CPU did not acknowledge': ('SharedRootShootdown.AcknowledgedProcessorMask', -8),
                                     'another page': ('SharedRootShootdown.VirtualStart', 0x1000),
                                     'two pages': ('SharedRootShootdown.Pages', 1),
                                     'queued BSP': ('SharedRootShootdown.QueuedProcessorMask', 1)},
                            'host.ia64.evidence.shared-root-shootdown')

    def test_ahci(self):
        self.assertDetected('Ahci', {'another LBA': ('Ahci.ReadStartingLba', 1),
                                     'two sectors': ('Ahci.ReadSectorCount', 1),
                                     'only identified': ('Ahci.Outcome', -1)},
                            'host.ia64.evidence.ahci-read')
        self.assertDetected('Ahci', {'another port': ('Ahci.PortNumber', 1),
                                     'another serial (IDENTIFY word 10)': ('Ahci.IdentifyData[20]', 1)},
                            'host.ia64.evidence.ahci-identify')

    def test_unreached_scenarios(self):
        self.assertDetected('Smp4', {'probe timer published': ('UserProbe.LocalTimerPublished', 1),
                                     'preemption published': ('NativePreemption.Published', 1)},
                            'host.ia64.evidence.unreached-scenarios')
        self.assertDetected('SyntheticProbe', {'user preemption published': ('UserPreemption.Published', 1),
                                               'object management published': ('ObjectManagement.Published', 1)},
                            'host.ia64.evidence.unreached-scenarios', Ia64SyntheticProbe.evaluate)

    def test_user_preemption(self):
        self.assertDetected('Smp4', {
            'unpublished': ('UserPreemption.Published', -1),
            'more runs ended than ticks': ('UserPreemption.RunStops[0]', 1 << 20),
            'a spinner in a firmware without one': ('UserPreemption.SpinnerPresent', 1),
            'spinner runs ended': ('UserPreemption.RunStops[2]', 1)},
            'host.ia64.evidence.user-preemption')

    def test_object_management(self):
        self.assertDetected('Smp4', {
            'unpublished': ('ObjectManagement.Published', -1),
            'an extra Endpoint': ('ObjectManagement.Created', 1),
            'one Endpoint not reclaimed': ('ObjectManagement.Reclaimed', -1),
            'an Endpoint left in the pool': ('ObjectManagement.OccupiedSlots', 1),
            'a charge left behind': ('ObjectManagement.MetadataBytes', 64),
            'no Endpoint charge recorded': ('ObjectManagement.EndpointMetadataBytes',
                                            -value_of(evidence_bytes('Smp4'), 'ObjectManagement.EndpointMetadataBytes')),
            'one Frame too many': ('ObjectManagement.FramesCreated', 1),
            'a MemoryObject not reclaimed': ('ObjectManagement.MemoryObjectsReclaimed', -1),
            'committed pages left behind': ('ObjectManagement.CommittedPages', 2),
            'resident pages left behind': ('ObjectManagement.ResidentPages', 2),
            'a dirty page left behind': ('ObjectManagement.AllocatorDirtyPages', 2),
            'a frame pool page lost': ('ObjectManagement.AllocatorCleanPages', -1),
            'no Frame charge recorded': ('ObjectManagement.FrameMetadataBytes',
                                         -value_of(evidence_bytes('Smp4'), 'ObjectManagement.FrameMetadataBytes')),
            'no MemoryMap': ('ObjectManagement.Mappings', -1),
            'a mapping left behind': ('ObjectManagement.Unmappings', -1),
            'mapped outside the window': ('ObjectManagement.MappedVirtualStart', 0x1000),
            'a third page mapped': ('ObjectManagement.MappedPages', 1),
            'a misaligned physical run': ('ObjectManagement.MappedPhysicalStart', 8)},
            'host.ia64.evidence.object-management')

    def test_threads(self):
        self.assertDetected('Smp4', {
            'unpublished': ('Threads.Published', -1),
            'an extra thread': ('Threads.Created', 1),
            'a thread not reclaimed': ('Threads.Destroyed', -1),
            'a pool slot not freed': ('Threads.Reclaimed', -1),
            'no ThreadExit': ('Threads.Exits', -1),
            'an entry slot not reaped': ('Threads.Reaped', -1),
            'another last exit code': ('Threads.LastExitCode', 1),
            'a pool slot still held': ('Threads.FreeSlots', -1),
            'an entry slot still held': ('Threads.FreeNativeSlots', -1),
            'a Threads charge left behind': ('Threads.ThreadsUsage', 1),
            'never preempted': ('Threads.RunStops', -value_of(evidence_bytes('Smp4'), 'Threads.RunStops')),
            'no fault': ('Threads.Faults', -value_of(evidence_bytes('Smp4'), 'Threads.Faults')),
            'every fault deferred': ('Threads.Deferrals', 1 << 20),
            'a longer deferral than all deferrals': ('Threads.LongestDeferral', 1 << 20),
            'no kill': ('Threads.Kills', -1),
            'a second kill': ('Threads.Kills', 1)},
            'host.ia64.evidence.threads')

    def test_domains(self):
        self.assertDetected('Smp4', {
            'unpublished': ('Domains.Published', -1),
            'an extra AddressSpace': ('Domains.AddressSpacesCreated', 1),
            'an AddressSpace not reclaimed': ('Domains.AddressSpacesReclaimed', -1),
            'a CapabilitySpace not created': ('Domains.CapabilitySpacesCreated', -1),
            'a CapabilitySpace not reclaimed': ('Domains.CapabilitySpacesReclaimed', -1),
            'an extra domain': ('Domains.DomainsCreated', 1),
            'a domain not reclaimed': ('Domains.DomainsReclaimed', -1),
            'an AddressSpace slot held': ('Domains.FreeAddressSpaceSlots', -1),
            'a CapabilitySpace slot held': ('Domains.FreeCapabilitySpaceSlots', -1),
            'a domain slot held': ('Domains.FreeDomainSlots', -1),
            'a preparation missing': ('Domains.SlotsPrepared', -1),
            'a slot not released': ('Domains.SlotsReleased', -1),
            'a slot spent': ('Domains.SlotsSpent', 1),
            'a RID block reused': ('Domains.AllocatedAddressSpaces', -1),
            'a domain not recorded': ('Domains.DomainsRecorded', -1),
            'a record not retired': ('Domains.DomainsRetired', -1)},
            'host.ia64.evidence.domains')

    def test_components(self):
        rate = value_of(evidence_bytes('Smp4'), 'Components.ItcTicksPerMicrosecond')
        self.assertDetected('Smp4', {
            'unpublished': ('Components.Published', -1),
            'no measured rate': ('Components.ItcTicksPerMicrosecond', -rate),
            'twice the rate': ('Components.ItcTicksPerMicrosecond', rate),
            'no wait timed out': ('Components.WaitExpiries',
                                  -value_of(evidence_bytes('Smp4'), 'Components.WaitExpiries')),
            'no tagged fault': ('Components.TaggedFaults',
                                -value_of(evidence_bytes('Smp4'), 'Components.TaggedFaults')),
            'the last fault tagged by the hang': ('Components.LastTag', 1),
            # A Ready kill has no aborted exit or domain notification; an
            # unmatched reduction in notifications must still be rejected.
            'domain notifications missing for aborted exits': ('Components.DomainExits', -2),
            'an extra domain exit': ('Components.DomainExits', 1),
            'a component without a ResourceDomain': ('Components.ResourceDomainsCreated', -1),
            'a ResourceDomain not drained': ('Components.ResourceDomainsDestroyed', -1),
            'a ResourceDomain not reclaimed': ('Components.ResourceDomainsReclaimed', -1),
            'a ResourceDomain left in its slot': ('Components.ResourceDomainsLive', 1)},
            'host.ia64.evidence.components')

    def test_collection(self):
        self.assertDetected('Smp4', {
            'unpublished': ('Collection.Published', -1),
            'a reclamation no collection made': ('Collection.Reclaimed', -1),
            'a run longer than the pool hands out': ('Collection.ScrubbedPages', 10 ** 6)},
            'host.ia64.evidence.collection')

    def test_filesystem_manager_binds(self):
        # the native crash demo starts three filesystem incarnations. Each binds
        # its separate manager Endpoint; losing those three binds must fail even when every WaitSet
        # is reclaimed.
        self.assertDetected('Ahci', {'missing managers': ('Services.Binds', -3)},
                            'host.ia64.evidence.services')

    def test_services(self):
        self.assertDetected('Smp4', {
            'unpublished': ('Services.Published', -1),
            'a WaitSet left standing': ('Services.WaitSetsReclaimed', -1),
            # The pager's timed wait and the NIC driver's backstop expire at
            # least once each.
            'only the pager\'s wait expired': ('Services.WaitExpiries',
                                              1 - value_of(evidence_bytes('Smp4'), 'Services.WaitExpiries')),
            'no wait expired': ('Services.WaitExpiries', -value_of(evidence_bytes('Smp4'), 'Services.WaitExpiries')),
            'a kill that did not abort the wait': ('Services.WaitAborts', -1),
            'a bind': ('Services.Binds', 1)},
            'host.ia64.evidence.services')

    def test_ready_service_kills_preserve_all_teardown_checks(self):
        data = evidence_bytes('Smp4')
        aborts = value_of(data, 'Services.WaitAborts')
        exits = value_of(data, 'Components.DomainExits')
        # A kill of a Ready service preserves Kills, KilledExitCode and every
        # reclaimed object. Only the aborted-wait and notification counts vary.
        for remaining in range(1, aborts + 1):
            with self.subTest(aborted_waits=remaining):
                record = with_value(data, 'Services.WaitAborts', remaining)
                record = with_value(record, 'Services.AbortedExits', remaining)
                record = with_value(record, 'Components.DomainExits', exits - aborts + remaining)
                self.assertEqual(self.evaluate('Smp4', record), set())
                held = with_value(record, 'Threads.Reclaimed', value_of(record, 'Threads.Reclaimed') - 1)
                self.assertEqual(self.evaluate('Smp4', held),
                                 {'host.ia64.evidence.threads', COLLECTION_CHECK})

    def test_aborted_waits_require_their_exit_and_domain_notification(self):
        data = evidence_bytes('Smp4')
        exits = value_of(data, 'Services.AbortedExits')
        record = with_value(data, 'Services.AbortedExits', exits - 1)
        self.assertEqual(self.evaluate('Smp4', record),
                         {'host.ia64.evidence.services', 'host.ia64.evidence.components'})
        # Even a consistent zero cannot satisfy the native waiting-thread probe.
        record = with_value(data, 'Services.WaitAborts', 0)
        record = with_value(record, 'Services.AbortedExits', 0)
        record = with_value(record, 'Components.DomainExits', value_of(data, 'Components.DomainExits') - exits)
        self.assertEqual(self.evaluate('Smp4', record), {'host.ia64.evidence.services'})

    def test_interrupts(self):
        self.assertDetected('Smp4', {
            'unpublished': ('Interrupts.Published', -1),
            'an Interrupt left standing': ('Interrupts.Live', 1),
            'an Interrupt not drained': ('Interrupts.Destroyed', -1),
            # The NIC needs none of its deliveries, so the recorded count
            # exceeds the least by however many it took.
            'no INTx delivered': ('Interrupts.Delivered', -value_of(evidence_bytes('Smp4'), 'Interrupts.Delivered')),
            # One each the crashed service and the drained NIC driver may leave.
            'more spurious deliveries than the drains may leave': ('Interrupts.Spurious', 3)},
            'host.ia64.evidence.interrupts')

    def test_ps2_interrupt_bounds(self):
        # Edu's driver (6 with the IOMMU: its raise and five transfers) and
        # two services (2 each) less one recovery, whose crashed factorial may
        # never arrive, need 9 deliveries, plus one for the recovery at most;
        # the PS/2 driver's two lines add 13..225.
        def problems(delivered, spurious=0):
            record = Ia64Evidence.Record(Published=1, Created=5, Destroyed=5, Reclaimed=5, Live=0,
                                         Delivered=delivered, Spurious=spurious)
            return Ia64Evidence.device_interrupt_problems(record, drivers=1, driver_services=2, recoveries=1,
                                                          iommu=True, ps2=1)
        self.assertEqual(problems(22), [])
        self.assertEqual(problems(235), [])
        self.assertEqual(problems(234, spurious=1), [])
        self.assertTrue(problems(21))
        self.assertTrue(problems(236))
        self.assertTrue(problems(235, spurious=1))

    def test_device_control(self):
        self.assertDetected('Smp4', {
            'unpublished': ('DeviceControl.Published', -1),
            'a quarantine left standing': ('DeviceControl.Confirmations', -1),
            'a drain that did not quarantine': ('DeviceControl.Quarantines', -1),
            'one more assignment': ('DeviceControl.Assignments', 1)},
            'host.ia64.evidence.device-control')

    def test_retired_dma_pins_are_balanced_and_bounded(self):
        name = 'Ahci'
        data = evidence_bytes(name)
        baseline = value_of(data, 'Block.PinsParked')

        def changed(total, released=None, still=0):
            edited = data
            for path, value in (
                ('Block.PinsParked', total), ('Block.PinsReleased', total if released is None else released),
                ('Block.ParkedPins', still), ('Block.Releases', total if released is None else released)):
                edited = with_value(edited, path, value)
            return edited

        self.assertEqual(self.evaluate(name, changed(baseline + 12)), set())
        for total in (0, 1_000_000):
            self.assertEqual(self.evaluate(name, changed(total)), {BLOCK_CHECK})
        for edited in (changed(baseline + 12, released=baseline + 11), changed(baseline + 12, still=1)):
            self.assertEqual(self.evaluate(name, edited), {BLOCK_CHECK})

    def test_block(self):
        # Every case parks the edu driver services' pins; the disk case also
        # the AHCI drivers', one of them the Frame the crash's late read ran
        # into, whose page held the disk's token when it was scrubbed.
        self.assertDetected('Smp4', {
            'unpublished': ('Block.Published', -1),
            'a pin still parked': ('Block.ParkedPins', 1),
            'a parked pin never released': ('Block.PinsReleased', -1),
            'one more parked': ('Block.PinsParked', 1),
            'a release not recorded': ('Block.Releases', -1)},
            BLOCK_CHECK)
        data = evidence_bytes('Ahci')
        token = int.from_bytes(Machine.DISK_TOKEN[:8], 'little')
        slot = next(index for index in range(Ia64Evidence.BLOCK_RELEASES_KEPT)
                    if value_of(data, 'Block.Released[%d].Sample[0]' % index) == token)
        self.assertDetected('Ahci', {
            'the token not found': ('Block.Released[%d].Sample[0]' % slot, 1),
            'not sampled': ('Block.Released[%d].Sampled' % slot, -1),
            'a release of no Frame': ('Block.Released[%d].Pages' % slot, -1)},
            BLOCK_CHECK)

    def test_probe_timer_evidence(self):
        evaluate = Ia64SyntheticProbe.evaluate
        self.assertDetected('SyntheticProbe', {
            'first interrupt on another CPU': ('UserProbe.LocalTimerInterrupt.ProcessorIndex', 1),
            'first interrupt vector': ('UserProbe.LocalTimerInterrupt.Vector', 1),
            'first interrupt with IFS.v': ('UserProbe.LocalTimerInterrupt.InterruptedIfsValid', 1)},
            'host.ia64.evidence.probe-local-timer', evaluate)
        # The TimerQueue commit mirrors the first interrupt's sequence.
        mutant = with_value(evidence_bytes('SyntheticProbe'), 'UserProbe.LocalTimerInterrupt.Sequence', 2)
        self.assertEqual(self.evaluate('SyntheticProbe', mutant, evaluate),
                         {'host.ia64.evidence.probe-local-timer', 'host.ia64.evidence.probe-timer-queue'})
        self.assertDetected('SyntheticProbe', {
            'queue claim': ('UserProbe.TimerQueue.QueueClaimSequence', 1),
            'claimed timer state': ('UserProbe.TimerQueue.ClaimedTimerState', -1),
            'final notification state': ('UserProbe.TimerQueue.FinalNotificationState', 1),
            'final queue entry': ('UserProbe.TimerQueue.FinalScheduledEntries', 1)},
            'host.ia64.evidence.probe-timer-queue', evaluate)
        self.assertDetected('SyntheticProbe', {
            'wait interrupt sequence': ('UserProbe.WaitTimerInterrupt.Sequence', 1),
            'wait interrupt acknowledgement': ('UserProbe.WaitTimerInterrupt.AcknowledgedSequence', 1),
            'wait interrupt PSR': ('UserProbe.WaitTimerInterrupt.InterruptedProcessorStatus', 1 << 13)},
            'host.ia64.evidence.probe-wait-timer', evaluate)

    def test_native_preemption(self):
        self.assertDetected('NativePreemption', {
            'two captures': ('NativePreemption.ControlCaptureCount', 1),
            'no restore': ('NativePreemption.Runtime.RestoreCount', -1),
            'another exit code': ('NativePreemption.WorkerExitCode', 1),
            'readmitted twice': ('NativePreemption.Runtime.AdmissionRunSequence', 1),
            'unpublished': ('NativePreemption.Published', -1)},
            'host.ia64.evidence.native-preemption', Ia64NativePreemption.evaluate)


class EntryStateFaultCountsTests(unittest.TestCase):
    def test_each_completed_admission_fault_adds_one_reclaimed_thread_and_exception(self):
        evidence = Evidence.Record(Published=1, Created=13, Destroyed=13, Reclaimed=13,
            Exits=2, Reaped=13, Kills=2, Exceptions=8, Faults=9, Deferrals=0,
            LongestDeferral=0, InitialDeferrals=0, LastExitCode=123,
            FreeSlots=2, FreeNativeSlots=2, ThreadsUsage=0, RunStops=1,
            SlotMetadataBytes=1)
        self.assertEqual(Ia64Evidence.thread_problems(evidence, killed_exit_code=123,
            slots=2, entry_state_faults=7), [])
        evidence.Reclaimed -= 1
        self.assertTrue(Ia64Evidence.thread_problems(evidence, killed_exit_code=123,
            slots=2, entry_state_faults=7))


class ScenarioExpectationTests(unittest.TestCase):
    def test_a_wrong_scenario_expectation_is_detected(self):
        """The runner's scenario definition is what the record is judged
        against: expecting two service requests per AP fails every AP."""
        record = fixture_schema().decode(evidence_bytes('Smp4'))
        expected = expectation('Smp4')
        leaf = Ia64Default.poll_leaf(observation('Smp4'), expected)
        timer = Ia64Evidence.TimerExpectation.of(expected.constants, leaf)
        wrong = Ia64Evidence.ApplicationProcessorScenario(service_requests=2)
        slot = record.ApplicationProcessors.Processors[1]
        self.assertEqual(Ia64Evidence.application_processor_problems(slot, 1, expected.constants, timer), [])
        problems = Ia64Evidence.application_processor_problems(slot, 1, expected.constants, timer, wrong)
        self.assertEqual(problems, ['ServiceRequestSequence is 0x1, expected 0x2',
                                    'ServiceCompletionSequence is 0x1, expected 0x2',
                                    'ServiceCount is 0x1, expected 0x2'])

    def test_the_task_priority_comes_from_the_ap_runtime(self):
        expected = expectation('Smp4')
        constants = dataclasses.replace(expected.constants, ap_task_priority=expected.constants.ap_task_priority + 1)
        record = fixture_schema().decode(evidence_bytes('Smp4'))
        leaf = Ia64Default.poll_leaf(observation('Smp4'), expected)
        problems = Ia64Evidence.application_processors_problems(
            record.ApplicationProcessors, 4, constants, Ia64Evidence.TimerExpectation.of(constants, leaf))
        self.assertEqual(len(problems), 3)
        self.assertTrue(all('LocalTimerTaskPriority' in problem for problem in problems))
        self.assertEqual(Ia64Layout.headers(ROOT).ap_task_priority, expected.constants.ap_task_priority)


if __name__ == '__main__':
    unittest.main()
