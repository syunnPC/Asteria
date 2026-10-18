# SPDX-License-Identifier: Apache-2.0
"""The IA-64 evidence record and the host's judgment of it.

The kernel runs its scripted diagnostic scenarios, stores what it observed
in `Ia64EvidenceRecord` (Source/Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp)
and judges nothing but its own contracts; this module decodes the record
(`AsteriaVm.Evidence`, layout from the headers' assertions) and compares
each sub-record with the expectations of the case:

- processors: the rendezvous count and mask against the case's CPU count
  and QEMU's `query-cpus-fast`;
- PCI: the first ECAM function and the AHCI function against `query-pci`
  (`Observers.Ia64Devices`);
- AHCI: no disk, or the IDENTIFY data decoded against the runner's own
  `-device ide-hd,serial=,model=` arguments and fixture size, and the READ
  DMA EXT of the fixture's marked sector, whose buffer the driver reads at
  the recorded address and compares with the fixture;
- application processors: every AP's local-timer proof, cooperative
  scheduler cycle, persistent-scheduler service request and deferred timer
  delivery against `ApplicationProcessorScenario`, the AP runtime's own
  parameters (task priority, polling grace) and the relocated polling leaf;
- shared-root shootdown: the page the kernel mapped and the processors it
  targeted, queued and heard acknowledge, against the case's CPUs;
- synthetic probe: the BSP's first local-timer interrupt,
  its TimerQueue transaction and the interrupt that resumed the probe's wait;
- native preemption: one capture and one restore of the preempted
  worker, which exited with its own exit code.

Expected values come from the case (CPU count, disk), QEMU (`query-*`), the
runner's disk fixture (`Machine`), the build's ELF files and C++ constants
(`Ia64Layout`, AsteriaVerify.Layout) and, for what the kernel's scripted
scenarios do, the scenario definitions below. Interrupt records are judged
against the kernel's interrupted-PSR contract (value under mask, slot 0-2),
not against emulator-pinned PSR bits.
"""
from __future__ import annotations

import dataclasses
import functools
import pathlib
import struct
from AsteriaVerify import Layout

from AsteriaVm import Evidence, Machine
from AsteriaVm.Observers import SharedImages
from AsteriaVm.Evidence import Record
from AsteriaVm.Ia64Layout import HeaderConstants
from AsteriaVm.Observers import Devices, Ia64Devices, DmaLifetime
from AsteriaVm.Observers.Ia64Cpu import MAXIMUM_SLOT, PSR_RI_MASK, PSR_RI_SHIFT
from AsteriaVm.Observers.Ia64State import CpuState, StateError

HEADERS = ('Source/Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp',
           'Source/Kernel/Diagnostics/ApplicationLaunchEvidence.hpp',
           'Source/Abi/ObjectCreationLimits.hpp',
           'Source/Kernel/Memory/PhysicalPageImportEvidence.hpp',
           'Source/Abi/ComponentImageTable.hpp',
           'Source/Boot/BootFiles.hpp',
           'Source/Platform/Ia64/Diagnostics/Ia64Stop.hpp',
           'Source/Kernel/Diagnostics/StopRecord.hpp',
           'Source/Kernel/Diagnostics/Watchdog.hpp',
           'Source/Kernel/Time/ProcessorClock.hpp',
           'Source/Kernel/Diagnostics/UserProcessorEvidence.hpp',
           'Source/Kernel/Scheduler/ReadyQueue.hpp',
           'Source/Kernel/Scheduler/SchedulerTypes.hpp',
           'Source/Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp',
           'Source/Platform/Ia64/Scheduler/Ia64BspKernelRuntime.hpp')
RECORD = 'Ia64EvidenceRecord'
SCHEMA_CONSTANT = 'Ia64EvidenceRecordSchema'
OUTCOME_ENUMERATION = 'Ia64AhciEvidenceOutcome'
DUMP = Evidence.ARTIFACT.removesuffix('.bin')   # The record's dump name (Evidence.bin)
BSP_PROCESSOR = 0            # The kernel's CpuId of the bootstrap processor
# ITM after the BSP run loop parks the local timer: its saturated stop-at
# deadline MaximumUInt64 - 1 (ProcessorScheduler.cpp dispatch, no named constant).
PARKED_TIMER_MATCH = (1 << 64) - 2


@functools.lru_cache(maxsize=None)
def schema(root: pathlib.Path) -> Evidence.Schema:
    """The record layout of the tree at `root`."""
    return Evidence.Schema.load(pathlib.Path(root), HEADERS, RECORD, SCHEMA_CONSTANT)


@functools.lru_cache(maxsize=None)
def outcomes(root: pathlib.Path) -> dict[str, int]:
    """Ia64AhciEvidenceOutcome's enumerators."""
    from AsteriaVerify import Layout
    return Layout.enumerators(Layout.read(pathlib.Path(root) / HEADERS[0]), OUTCOME_ENUMERATION)


def _differs(problems: list[str], what: str, observed, expected) -> None:
    if observed != expected:
        problems.append('%s is %s, expected %s' % (what, _show(observed), _show(expected)))


def _show(value) -> str:
    return '%#x' % value if isinstance(value, int) else repr(value)


def _published(part: Record, word: str = 'Published') -> list[str]:
    return [] if part[word] == 1 else ['not published (%s = %s)' % (word, _show(part[word]))]


def _mask(cpus: int) -> int:
    return (1 << cpus) - 1


# Interrupt records ---------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class TimerExpectation:
    """What every local-timer interrupt record must satisfy on this build."""
    poll_leaf: tuple[int, int]     # [start, end) of the relocated polling leaf
    vector: int                    # Ia64ExternalInterruptTimerVector
    psr: int                       # Ia64ExternalInterruptInterruptedProcessorStatusValue
    psr_mask: int                  # Under Ia64ExternalInterruptInterruptedProcessorStatusMask

    @classmethod
    def of(cls, constants: HeaderConstants, poll_leaf: tuple[int, int]) -> 'TimerExpectation':
        return cls(poll_leaf, constants.timer_vector, constants.interrupted_psr, constants.interrupted_psr_mask)


def poll_leaf_problems(poll_leaf: tuple[int, int]) -> list[str]:
    start, end = poll_leaf
    if end <= start or start % 16 or end % 16:
        return ['the polling leaf %#x..%#x is empty or not bundle-aligned' % (start, end)]
    return []


def interrupt_problems(record: Record, *, sequence: int, acknowledged: int, processor: int,
                       expected: TimerExpectation, deadline: int | None = None) -> list[str]:
    """One `Ia64LocalTimerInterruptEvidence`: the interrupt of this sequence,
    taken at the timer vector on `processor` after its programmed deadline
    while the processor polled in the kernel's polling leaf, with an
    interrupted PSR the kernel's contract accepts."""
    problems: list[str] = []
    _differs(problems, 'sequence', record.Sequence, sequence)
    _differs(problems, 'acknowledged sequence', record.AcknowledgedSequence, acknowledged)
    if record.ProgrammedDeadlineTicks == 0:
        problems.append('no programmed deadline')
    elif deadline is not None:
        _differs(problems, 'programmed deadline', record.ProgrammedDeadlineTicks, deadline)
    if record.ObservedIntervalTimeCounter < record.ProgrammedDeadlineTicks:
        problems.append('observed ITC %#x precedes the deadline %#x' % (record.ObservedIntervalTimeCounter,
                                                                         record.ProgrammedDeadlineTicks))
    start, end = expected.poll_leaf
    address = record.InterruptedInstructionPointer
    if address % 16 or not start <= address < end:
        problems.append('interrupted IIP %#018x is not a bundle of the polling leaf %#x..%#x' % (address, start, end))
    status = record.InterruptedProcessorStatus
    slot = (status & PSR_RI_MASK) >> PSR_RI_SHIFT
    if slot > MAXIMUM_SLOT or status & expected.psr_mask != expected.psr:
        problems.append('interrupted IPSR %#018x: under the mask %#018x it must be %#018x, with a slot of 0-%d' % (
            status, expected.psr_mask, expected.psr, MAXIMUM_SLOT))
    _differs(problems, 'interrupted IFS.v', record.InterruptedIfsValid, 0)
    _differs(problems, 'processor', record.ProcessorIndex, processor)
    _differs(problems, 'vector', record.Vector, expected.vector)
    if any(record.Reserved):
        problems.append('reserved bytes %s are not zero' % record.Reserved.hex())
    return problems


# Processors and devices ----------------------------------------------------------

def processor_problems(processors: Record, query_cpus: list[dict], cpus: int) -> list[str]:
    """The rendezvous admitted every processor the case asked QEMU for."""
    problems = _published(processors)
    problems += Ia64Devices.processor_problems(processors.ProcessorCount, query_cpus, cpus)
    _differs(problems, 'the enabled processor mask', processors.EnabledProcessorMask, _mask(cpus))
    return problems


def pci_facts(pci: Record) -> tuple[Ia64Devices.PciEcamFact, Ia64Devices.PciDiscoveryFact]:
    first = Ia64Devices.PciAddress(pci.FirstSegment, pci.FirstBus, pci.FirstDevice, pci.FirstFunction)
    ahci = Ia64Devices.PciAddress(pci.AhciSegment, pci.AhciBus, pci.AhciDevice, pci.AhciFunction)
    return (Ia64Devices.PciEcamFact(first, pci.FirstVendorDevice),
            Ia64Devices.PciDiscoveryFact(pci.PublishedFunctionCount, ahci, pci.AhciVendorDevice,
                                         (pci.AhciClassCode, pci.AhciSubclass, pci.AhciProgrammingInterface)))


def pci_ecam_problems(pci: Record, query_pci: list[dict]) -> list[str]:
    """The ECAM scan ran and its first function is QEMU's first function."""
    problems = _published(pci)
    _differs(problems, 'EcamReady', pci.EcamReady, 1)
    _differs(problems, 'FirstFunctionPresent', pci.FirstFunctionPresent, 1)
    return problems + Ia64Devices.ecam_problems(pci_facts(pci)[0], query_pci)


def pci_discovery_problems(pci: Record, query_pci: list[dict]) -> list[str]:
    """The kernel published every QEMU function and bound QEMU's AHCI controller."""
    problems = _published(pci)
    _differs(problems, 'AhciPresent', pci.AhciPresent, 1)
    return problems + Ia64Devices.discovery_problems(pci_facts(pci)[1], query_pci)


@dataclasses.dataclass(frozen=True)
class DiskExpectation:
    """The disk the runner attached (Machine): its model, serial, size and
    AHCI port, and the fixture sector the kernel's scripted read targets and
    its contents."""
    model: str
    serial: str
    sectors: int
    lba: int
    sector: bytes
    port: int


def ahci_identify_problems(ahci: Record, disk: DiskExpectation | None, outcomes: dict[str, int]) -> list[str]:
    """Without a disk the kernel found none; with one it identified the
    runner's disk on port 0 (the IDENTIFY words decoded here)."""
    problems = _published(ahci)
    if disk is None:
        _differs(problems, 'the AHCI outcome', ahci.Outcome, outcomes['NoDisk'])
        if any(ahci.IdentifyData):
            problems.append('IDENTIFY data recorded without a disk')
        return problems
    if ahci.Outcome not in (outcomes['Identified'], outcomes['Read']):
        problems.append('the AHCI outcome is %#x, the disk was not identified' % ahci.Outcome)
    _differs(problems, 'the AHCI port', ahci.PortNumber, disk.port)
    return problems + Devices.identify_problems(Devices.decode_identify(ahci.IdentifyData), serial=disk.serial,
                                                model=disk.model, sectors=disk.sectors)


def ahci_read_problems(ahci: Record, disk: DiskExpectation | None, buffer: bytes | None,
                       outcomes: dict[str, int]) -> list[str]:
    """With a disk, the kernel read one sector at the fixture's marked LBA
    and its DMA buffer holds exactly that sector; without one, nothing."""
    problems = _published(ahci)
    if disk is None:
        if ahci.Outcome == outcomes['Read'] or ahci.ReadBufferPhysicalAddress:
            problems.append('a READ DMA EXT was recorded without a disk')
        return problems
    _differs(problems, 'the AHCI outcome', ahci.Outcome, outcomes['Read'])
    _differs(problems, 'the starting LBA', ahci.ReadStartingLba, disk.lba)
    _differs(problems, 'the sector count', ahci.ReadSectorCount, 1)
    _differs(problems, 'the transfer size', ahci.ReadTransferSizeBytes, len(disk.sector))
    _differs(problems, 'the buffer size', ahci.ReadBufferSizeBytes, len(disk.sector))
    if buffer is None:
        return problems + ['no AHCI read-buffer dump']
    return problems + Ia64Devices.read_buffer_problems(buffer, disk.sector)


def disk_expectation(fixture: pathlib.Path | None) -> DiskExpectation:
    """The runner's disk: Machine's model and serial, the fixture's size and marked sector."""
    lba = Machine.DISK_TOKEN_SECTOR
    if fixture is not None and fixture.exists():
        sector, sectors = Ia64Devices.fixture_geometry(fixture, lba)
    else:   # The fixture Machine.write_disk_fixture writes
        sector = Machine.DISK_TOKEN * (Ia64Devices.SECTOR_SIZE // len(Machine.DISK_TOKEN))
        sectors = Machine.DISK_SIZE_BYTES // Ia64Devices.SECTOR_SIZE
    return DiskExpectation(Machine.DISK_MODEL, Machine.DISK_SERIAL, sectors, lba, sector, Machine.DISK_AHCI_PORT)


# Application processors -------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class ApplicationProcessorScenario:
    """What BootApplication's AP runtime scenario does on every AP, as the
    runner defines it (the kernel no longer compares these numbers):

    - the AP proves its local timer before it serves: its first local
      interrupt (sequence 1), which the BSP acknowledges;
    - its portable-scheduler worker runs, blocks and resumes once (run
      sequences 1 and 2) and stays bound to the one run loop, with no timer
      queued;
    - the BSP sends one persistent-scheduler service request with its cookie:
      request and completion sequence 1, the cookie echoed on receipt and on
      completion, completed on the worker's second run after two dispatches,
      one wake and one service;
    - the BSP requests one deferred timer delivery (gate sequence 1): one
      TimerQueue registration and one arm (while armed: one scheduled entry,
      next claim 1), one drained expiration claimed once by the queue (claims
      1..1, then next claim 2) and once through its notification (claim 1),
      after which the worker is back at run 2, still bound, with the ready
      queue at position 3; the expiration arrives as the AP's second local
      interrupt (sequence 2), which the BSP acknowledges.
    """
    proof_interrupt: int = 1
    initial_run: int = 1
    resumed_run: int = 2
    service_requests: int = 1
    service_dispatches: int = 2
    service_wakes: int = 1
    run_loop_bindings: int = 1
    delivery_requests: int = 1
    delivery_registrations: int = 1
    delivery_arms: int = 1
    delivery_claims: int = 1
    delivery_ready_queue_position: int = 3
    delivery_interrupt: int = 2


def application_processor_problems(slot: Record, processor: int, constants: HeaderConstants,
                                   timer: TimerExpectation,
                                   scenario: ApplicationProcessorScenario = ApplicationProcessorScenario()
                                   ) -> list[str]:
    """One AP's recorded scenario."""
    problems = _published(slot)
    s = scenario
    for what, observed, expected in (
            ('Processor', slot.Processor, processor),
            # The local-timer proof.
            ('LocalTimerVector', slot.LocalTimerVector, constants.timer_vector),
            ('LocalTimerTaskPriority', slot.LocalTimerTaskPriority, constants.ap_task_priority),
            ('LocalTimerPollDeadlineTicks', slot.LocalTimerPollDeadlineTicks,
             slot.LocalTimerDeadlineTicks + constants.ap_poll_grace_ticks),
            ('LocalTimerPollCode', (slot.LocalTimerPollCodeStart, slot.LocalTimerPollCodeEnd), timer.poll_leaf),
            ('LocalTimerAcknowledgedSequence', slot.LocalTimerAcknowledgedSequence, s.proof_interrupt),
            # The cooperative scheduler cycle.
            ('InitialRunSequence', slot.InitialRunSequence, s.initial_run),
            ('FirstResumedRunSequence', slot.FirstResumedRunSequence, s.resumed_run),
            ('queued timers (scheduled, pending, claimed)', (slot.TimerQueueScheduledEntries,
             slot.TimerQueuePendingEntries, slot.TimerQueueClaimedEntries), (0, 0, 0)),
            ('RunLoopBindingCount', slot.RunLoopBindingCount, s.run_loop_bindings),
            # The persistent-scheduler service request.
            ('ServiceRequestSequence', slot.ServiceRequestSequence, s.service_requests),
            ('ServiceCompletionSequence', slot.ServiceCompletionSequence, s.service_requests),
            ('ServiceRequestCookie', slot.ServiceRequestCookie, slot.ServiceCookie),
            ('ServiceCompletionCookie', slot.ServiceCompletionCookie, slot.ServiceCookie),
            ('ServiceCompletionValue', slot.ServiceCompletionValue, slot.ResumedWorkerStackPointer),
            ('ServiceCompletionRunSequence', slot.ServiceCompletionRunSequence, s.resumed_run),
            ('ServiceDispatchCount', slot.ServiceDispatchCount, s.service_dispatches),
            ('ServiceWakeCount', slot.ServiceWakeCount, s.service_wakes),
            ('ServiceCount', slot.ServiceCount, s.service_requests),
            # The deferred timer delivery.
            ('DeliveryRequestSequence', slot.DeliveryRequestSequence, s.delivery_requests),
            ('DeliveryCompletionSequence', slot.DeliveryCompletionSequence, s.delivery_requests),
            ('DeliveryRegistrationSequence', slot.DeliveryRegistrationSequence, s.delivery_registrations),
            ('DeliveryArmSequence', slot.DeliveryArmSequence, s.delivery_arms),
            ('DeliveryArmedNextClaimSequence', slot.DeliveryArmedNextClaimSequence, s.delivery_claims),
            ('DeliveryArmedScheduledEntries', slot.DeliveryArmedScheduledEntries, s.delivery_arms),
            ('DeliveryPollDeadlineTicks', slot.DeliveryPollDeadlineTicks,
             slot.DeliveryDeadlineTicks + constants.ap_poll_grace_ticks),
            ('DeliveryDrainedCount', slot.DeliveryDrainedCount, s.delivery_claims),
            ('DeliveryFirstClaimSequence', slot.DeliveryFirstClaimSequence, s.delivery_claims),
            ('DeliveryLastClaimSequence', slot.DeliveryLastClaimSequence, s.delivery_claims),
            ('DeliveryClaimSequence', slot.DeliveryClaimSequence, s.delivery_claims),
            ('DeliveryNotificationClaimSequence', slot.DeliveryNotificationClaimSequence, s.delivery_claims),
            ('DeliveryFinalNextClaimSequence', slot.DeliveryFinalNextClaimSequence, s.delivery_claims + 1),
            ('DeliveryFinalRegistrationSequence', slot.DeliveryFinalRegistrationSequence,
             s.delivery_registrations),
            ('DeliveryFinalRegisteredBindings', slot.DeliveryFinalRegisteredBindings, s.delivery_registrations),
            ('DeliveryFinalSchedulerNextSequence', slot.DeliveryFinalSchedulerNextSequence,
             s.delivery_ready_queue_position),
            ('DeliveryFinalThreadRunSequence', slot.DeliveryFinalThreadRunSequence, s.resumed_run),
            ('DeliveryFinalRunLoopBindingCount', slot.DeliveryFinalRunLoopBindingCount, s.run_loop_bindings),
            ('DeliveryAcknowledgedSequence', slot.DeliveryAcknowledgedSequence, s.delivery_interrupt)):
        _differs(problems, what, observed, expected)
    for what, value in (('Generation', slot.Generation), ('ServiceCookie', slot.ServiceCookie),
                        ('ResumedWorkerStackPointer', slot.ResumedWorkerStackPointer),
                        ('DeliveryDeadlineTicks', slot.DeliveryDeadlineTicks)):
        if value == 0:
            problems.append('%s is zero' % what)
    problems += ['local-timer proof interrupt: %s' % problem for problem in interrupt_problems(
        slot.LocalTimerInterrupt, sequence=s.proof_interrupt, acknowledged=s.proof_interrupt - 1,
        processor=processor, expected=timer, deadline=slot.LocalTimerDeadlineTicks)]
    problems += ['timer-delivery interrupt: %s' % problem for problem in interrupt_problems(
        slot.DeliveryInterrupt, sequence=s.delivery_interrupt, acknowledged=s.delivery_interrupt - 1,
        processor=processor, expected=timer, deadline=slot.DeliveryDeadlineTicks)]
    return problems


def application_processors_problems(evidence: Record, cpus: int, constants: HeaderConstants,
                                    timer: TimerExpectation) -> list[str]:
    """Every AP of the case served its recorded scenario; the BSP's slot
    and the slots of absent processors stay empty."""
    problems = _published(evidence)
    _differs(problems, 'the runtime processor count', evidence.ProcessorCount, cpus)
    _differs(problems, 'the serving processor mask', evidence.ServingProcessorMask, _mask(cpus) & ~1)
    for index, slot in enumerate(evidence.Processors):
        if 0 < index < cpus:
            problems += ['CPU %d: %s' % (index, problem)
                         for problem in application_processor_problems(slot, index, constants, timer)]
        elif slot.Published:
            problems.append('slot %d is published, but CPU %d is %s' % (
                index, index, 'the BSP' if index == 0 else 'not part of the case'))
    return problems


# Shared-root shootdown --------------------------------------------------------

def shootdown_problems(evidence: Record, cpus: int, constants: HeaderConstants) -> list[str]:
    """The one supervisor page mapped into the shared root at the
    kernel's shootdown address, with a shootdown that targeted every
    processor, queued every AP and was acknowledged by every processor."""
    problems = _published(evidence)
    _differs(problems, 'the page', evidence.VirtualStart, constants.shootdown_virtual_start)
    _differs(problems, 'the page count', evidence.Pages, 1)
    if evidence.PhysicalStart == 0 or evidence.PhysicalStart % constants.page_size:
        problems.append('physical page %#x is not a page frame' % evidence.PhysicalStart)
    if evidence.ShootdownSequence == 0 or evidence.MappingSequence != evidence.ShootdownSequence:
        problems.append('shootdown sequence %#x and mapping sequence %#x must be the same non-zero sequence' % (
            evidence.ShootdownSequence, evidence.MappingSequence))
    _differs(problems, 'the target mask', evidence.TargetProcessorMask, _mask(cpus))
    _differs(problems, 'the queued mask', evidence.QueuedProcessorMask, _mask(cpus) & ~1)
    _differs(problems, 'the acknowledged mask', evidence.AcknowledgedProcessorMask, _mask(cpus))
    return problems


# Synthetic probe -----------------------------------------------

def probe_local_timer_problems(probe: Record, state: CpuState, timer: TimerExpectation) -> list[str]:
    """The BSP's first local-timer interrupt (acknowledged by the BSP
    before it was recorded), and the timer parked afterwards (CR.ITM at the
    run loop's stop-at value, CR.ITV unmasked at the timer vector)."""
    problems = _published(probe, 'LocalTimerPublished') + poll_leaf_problems(timer.poll_leaf)
    problems += ['first timer interrupt: %s' % problem for problem in interrupt_problems(
        probe.LocalTimerInterrupt, sequence=1, acknowledged=1, processor=BSP_PROCESSOR, expected=timer)]
    try:
        registers = state.get('CR.DCR')
    except StateError as error:
        return problems + [str(error)]
    if registers['itm'] != PARKED_TIMER_MATCH:
        problems.append('CR.ITM %#018x, expected the parked value %#018x' % (registers['itm'], PARKED_TIMER_MATCH))
    if registers['itv'] != timer.vector:
        problems.append('CR.ITV %#x, expected the unmasked timer vector %#x' % (registers['itv'], timer.vector))
    return problems


def probe_timer_queue_problems(probe: Record, constants: HeaderConstants) -> list[str]:
    """A TimerQueue timer armed for the probe's WaitSet expired once and
    was claimed once by the queue and once through its notification; the
    queue, timer, notification, deadline and local timer end idle."""
    queue, first = probe.TimerQueue, probe.LocalTimerInterrupt
    problems = _published(probe, 'LocalTimerPublished')
    _differs(problems, 'sequence', queue.Sequence, first.Sequence)
    if queue.TimerObjectIdentity == 0 or queue.NotificationObjectIdentity in (0, queue.TimerObjectIdentity):
        problems.append('timer object %#x and notification object %#x must be distinct non-zero identities' % (
            queue.TimerObjectIdentity, queue.NotificationObjectIdentity))
    c = constants
    for what, observed, expected in (
            ('the timer arm sequence', queue.TimerArmSequence, 1),
            ('the deadline', queue.DeadlineTicks, first.ProgrammedDeadlineTicks),
            ('the queue claim sequence', queue.QueueClaimSequence, 1),
            ('the notification claim sequence', queue.NotificationClaimSequence, 1),
            ('the initial deadline state', queue.InitialDeadlineState, c.deadline_armed),
            ('the selected deadline sources', queue.SelectedDeadlineSources, c.deadline_source_timer_queue),
            ('InitialDeadlineReprogrammed', queue.InitialDeadlineReprogrammed, 1),
            ('the claimed timer state', queue.ClaimedTimerState, c.timer_expired),
            ('NotificationPendingAfterSignal', queue.NotificationPendingAfterSignal, 1),
            ('the final timer state', queue.FinalTimerState, c.timer_disarmed),
            ('the final notification state', queue.FinalNotificationState, c.notification_ready),
            ('FinalNotificationPending', queue.FinalNotificationPending, 0),
            ('the final deadline state', queue.FinalDeadlineState, c.deadline_disarmed),
            ('the final local-timer state', queue.FinalLocalTimerState, c.local_timer_ready),
            ('FinalLocalTimerArmed', queue.FinalLocalTimerArmed, 0),
            ('the final queue entries', (queue.FinalScheduledEntries, queue.FinalPendingEntries,
                                         queue.FinalClaimedEntries), (0, 0, 0))):
        _differs(problems, what, observed, expected)
    if queue.ObservedTicks < queue.DeadlineTicks or queue.ObservedTicks < first.ObservedIntervalTimeCounter:
        problems.append('observed %#x precedes the deadline %#x or the interrupt\'s ITC %#x' % (
            queue.ObservedTicks, queue.DeadlineTicks, first.ObservedIntervalTimeCounter))
    if any(queue.Reserved):
        problems.append('reserved bytes %s are not zero' % queue.Reserved.hex())
    return problems


def probe_wait_timer_problems(probe: Record, timer: TimerExpectation) -> list[str]:
    """The second BSP timer interrupt, which resumed the
    thread blocked in WaitSetWait, came after the first was acknowledged
    and after its own deadline, which followed the first interrupt."""
    second, first = probe.WaitTimerInterrupt, probe.LocalTimerInterrupt
    problems = _published(probe, 'WaitTimerPublished')
    problems += ['wait timer interrupt: %s' % problem for problem in interrupt_problems(
        second, sequence=2, acknowledged=1, processor=BSP_PROCESSOR, expected=timer)]
    if second.ProgrammedDeadlineTicks < first.ObservedIntervalTimeCounter:
        problems.append('wait deadline %#x precedes the first interrupt\'s ITC %#x' % (
            second.ProgrammedDeadlineTicks, first.ObservedIntervalTimeCounter))
    return problems


# Native preemption ----------------------------------------------------------

def native_preemption_problems(evidence: Record, constants: HeaderConstants) -> list[str]:
    """The one-shot proof captured the preempted worker's frame once
    after its deadline, restored it once, readmitted the worker once, and the
    worker exited with its own exit code."""
    runtime = evidence.Runtime
    problems = _published(evidence)
    for what, observed, expected in (('the runtime evidence sequence', runtime.Sequence, 1),
                                     ('the runtime capture count', runtime.CaptureCount, 1),
                                     ('the runtime restore count', runtime.RestoreCount, 1),
                                     ('the control capture count', evidence.ControlCaptureCount, 1),
                                     ('the control restore count', evidence.ControlRestoreCount, 1),
                                     ('the admission run sequence', runtime.AdmissionRunSequence,
                                      runtime.StoppedRunSequence + 1),
                                     ('the worker exit code', evidence.WorkerExitCode,
                                      constants.native_preemption_exit_code)):
        _differs(problems, what, observed, expected)
    for what, value in (('the frame token', runtime.FrameToken), ('the stopped run', runtime.StoppedRunSequence),
                        ('the idle continuation generation', runtime.IdleContinuationGeneration),
                        ('the programmed deadline', runtime.ProgrammedDeadlineTicks)):
        if value == 0:
            problems.append('%s is zero' % what)
    if runtime.ObservedTicks < runtime.ProgrammedDeadlineTicks:
        problems.append('the preempting interrupt at %#x precedes its deadline %#x' % (
            runtime.ObservedTicks, runtime.ProgrammedDeadlineTicks))
    return problems


# Summaries of passing records (the detail a passing check keeps) ----------------

def describe_processors(processors: Record) -> str:
    return '%d processors, mask %#x' % (processors.ProcessorCount, processors.EnabledProcessorMask)


def describe_pci(pci: Record) -> str:
    ecam, discovery = pci_facts(pci)
    return 'first function %s id %#010x; %d functions published; AHCI %s id %#010x class %02x:%02x:%02x' % (
        ecam.address.describe(), ecam.identifier, discovery.functions, discovery.ahci.describe(),
        discovery.identifier, *discovery.class_code)


def describe_ahci(ahci: Record, outcomes: dict[str, int]) -> str:
    names = {value: name for name, value in outcomes.items()}
    text = 'outcome %s' % names.get(ahci.Outcome, '%#x' % ahci.Outcome)
    if ahci.Outcome == outcomes['Read']:
        text += ', LBA %d x %d sector(s), %d bytes into %#x' % (
            ahci.ReadStartingLba, ahci.ReadSectorCount, ahci.ReadTransferSizeBytes, ahci.ReadBufferPhysicalAddress)
    return text


def describe_application_processors(evidence: Record) -> str:
    served = [slot for slot in evidence.Processors if slot.Published]
    if not served:
        return 'no application processor (serving mask %#x)' % evidence.ServingProcessorMask
    return '; '.join(
        'CPU %d: proof interrupt %d (acknowledged %d), runs %d/%d, service %d/%d cookie %#x echoed, '
        'delivery %d (interrupt %d, acknowledged %d)' % (
            slot.Processor, slot.LocalTimerInterrupt.Sequence, slot.LocalTimerAcknowledgedSequence,
            slot.InitialRunSequence, slot.FirstResumedRunSequence, slot.ServiceRequestSequence,
            slot.ServiceCompletionSequence, slot.ServiceCookie, slot.DeliveryCompletionSequence,
            slot.DeliveryInterrupt.Sequence, slot.DeliveryAcknowledgedSequence) for slot in served)


def describe_shootdown(evidence: Record) -> str:
    return 'page %#x -> %#x (%d), sequence %d; target %#x, queued %#x, acknowledged %#x' % (
        evidence.VirtualStart, evidence.PhysicalStart, evidence.Pages, evidence.ShootdownSequence,
        evidence.TargetProcessorMask, evidence.QueuedProcessorMask, evidence.AcknowledgedProcessorMask)


def describe_native_preemption(evidence: Record) -> str:
    runtime = evidence.Runtime
    return 'captured %d, restored %d; stopped run %d, readmitted as run %d; worker exit code %#x' % (
        evidence.ControlCaptureCount, evidence.ControlRestoreCount, runtime.StoppedRunSequence,
        runtime.AdmissionRunSequence, evidence.WorkerExitCode)


# User preemption -------------------------------------------------------------------

SPINNER_DOMAIN = 2   # RunStops slot (Ia64EvidenceUserDomains)


def user_preemption_problems(evidence: Record, *, spinner: bool, initial_runs: int = 0) -> list[str]:
    """user preemption: the boot threads' PL3 ticks and the runs they ended. A
    tick ends at most one run, and ticks ended at least `initial_runs` runs
    of the initial image. Without a spinner its fields stay zero; with one,
    ticks ended at least one of its runs, it wrote one line and its final
    wait suspended."""
    problems = _published(evidence)
    if evidence.RunStops[0] < initial_runs:
        problems.append('user ticks ended %d runs of the initial image, expected at least %d'
                        % (evidence.RunStops[0], initial_runs))
    runs = sum(evidence.RunStops)
    if runs > evidence.UserTicks:
        problems.append('%d runs ended by %d user ticks' % (runs, evidence.UserTicks))
    spinner_fields = (evidence.SpinnerPresent, evidence.SpinnerLines, evidence.SpinnerWaitSuspended)
    if spinner:
        _differs(problems, 'the spinner (present, lines, wait suspended)', spinner_fields, (1, 1, 1))
        if evidence.RunStops[SPINNER_DOMAIN] == 0:
            problems.append('no user tick ended a run of the spinner')
    else:
        _differs(problems, 'the spinner (present, lines, wait suspended, runs ended)',
                 spinner_fields + (evidence.RunStops[SPINNER_DOMAIN],), (0, 0, 0, 0))
    return problems


def describe_user_preemption(evidence: Record) -> str:
    return '%d user ticks; runs ended: initial image %d, pager %d, spinner %d' % (
        evidence.UserTicks, *evidence.RunStops)


# Object management -----------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class ComponentScenario:
    """What the IA-64 boot pager builds as root service, as the runner defines it: one incarnation per
    entry of `behaviors` of a component of `segments` loadable segments, each
    in a fresh domain tagged with its number; read-only segments staged once per image and shared through sealed
    MemoryObjects, private segments mapped with their final permissions,
    beside one Frame and MemoryObject holding its stack and start block of
    `data_pages` pages at `data_start`, and one report Endpoint. A crashing
    incarnation's exception reaches the pager tagged, a hanging one is
    killed when the root service's timed wait expires, a reporting one exits
    with `exit_code`. Where the components are `drained`
, each
    has a ResourceDomain of its own, a child of the grant, and its teardown
    is that domain's drain, which takes its final mappings with its
    AddressSpace; every such domain is reclaimed. An 'account' incarnation
     creates an Endpoint and a Frame with a MemoryObject in its
    own domain, maps them over the `window_pages` of its window at
    `window_start`, is refused a page more and executable memory without Code, reports and exits with
    `account_exit_code`; its drain takes what it created. A 'driver'
    incarnation maps its device's registers
    through a device Frame and an uncached MemoryObject of its own domain,
    finds bus mastering refused and no IOMMU, reports and exits with
    `driver_exit_code`; once it is drained, the root service as Device
    Manager maps the registers' page at its own `mapping_window` with a Frame
    and MemoryObject of its own, finds the device quiet and unmaps them. An
    'echo' incarnation is a service: its Endpoint and
    WaitSet, bound, are in its own domain; the root service sends it one
    request with a Frame of its own, which comes back on the root service's
    reply Endpoint, then kills it while it waits, so its wait is aborted and
    it ends as it resumes, a created domain's exit. A 'driver-crash' and a
    'driver-service' are the driver's service instances of its recovery
    : each maps the registers as a driver does, has a service
    Endpoint and WaitSet, bound, and creates a WaitSet for its Interrupt,
    bound to it; the first crashes on an invalid instruction once the
    factorial it was asked for runs and is terminated, the second, started
    after it, serves and is killed while it waits. The Device Manager
    confirms the device quiet after each one's drain as after a driver's.
    The storage behaviours match AMD64's (Amd64Evidence.ComponentScenario):
    an 'fs-driver' and its 'filesystem' (or
    'filesystem-crash') services for the case's `disk` recipe, with the root
    service's `fs_root_maps` and the service's `fs_service_maps` of the data
    Frame, then a 'block-driver'; each AHCI driver serves the disk, if there
    is one, behind the IOMMU with its INTx line. The network behaviours are
    AMD64's too: a 'nic-driver' and the
    'network' stack on it, between the filesystem's and the block driver's,
    the driver copying `nic_copies_least` to `nic_copies_most` frames
    through its client page."""
    behaviors: tuple[str,...] = ('crash', 'hang', 'report')
    drained: bool = False
    inventory: bool = False
    exit_code: int = 0
    data_start: int = 0
    data_pages: int = 9
    account_exit_code: int = 0
    window_start: int = 0
    window_pages: int = 0
    driver_exit_code: int = 0
    mapping_window: int = 0
    iommu: bool = False
    # No IOMMU, and the AHCI controller and the e1000e trusted by BOOT.CFG
    # the AHCI and NIC
    # drivers master the bus without isolation, edu still drives its
    # registers only.
    trusted: bool = False
    disk: str = ''
    fs_root_maps: int = 0
    fs_service_maps: int = 0
    # The NIC driver's copies through its client page, each a MemoryMap and a
    # MemoryUnmap of a stack's Frame: one a frame it received into a credit or
    # sent, at least the demonstration's least traffic and at most the frames
    # the runner captured (AsteriaVm.Lan), as AMD64's.
    nic_copies_least: int = 0
    nic_copies_most: int = 0

    images: tuple[tuple[str, tuple[int, int]],...] = dataclasses.field(kw_only=True)
    image_pages: tuple[tuple[str, int],...] = dataclasses.field(kw_only=True)

    @property
    def segment_counts(self) -> tuple[tuple[int, int],...]:
        return tuple(dict(self.images)[SharedImages.image_name(b)] for b in self.behaviors)

    @property
    def shared_segments(self) -> int:
        return sum(dict(self.images)[name][0] for name in {SharedImages.image_name(b) for b in self.behaviors})

    @property
    def shared_pages(self) -> int:
        return sum(dict(self.image_pages)[name] for name in {SharedImages.image_name(b) for b in self.behaviors})

    @property
    def repeated_staging(self) -> int:
        return sum(dict(self.images)[SharedImages.image_name(b)][0] for b in self.behaviors) - self.shared_segments

    @property
    def incarnations(self) -> int:
        return len(self.behaviors)

    @property
    def crashes(self) -> int:
        return self.behaviors.count('crash')

    @property
    def hangs(self) -> int:
        return self.behaviors.count('hang')

    @property
    def reports(self) -> int:
        return self.behaviors.count('report')

    @property
    def accounts(self) -> int:
        return self.behaviors.count('account')

    @property
    def drivers(self) -> int:
        return self.behaviors.count('driver')

    @property
    def echoes(self) -> int:
        return self.behaviors.count('echo')

    @property
    def directories(self) -> int:
        return self.behaviors.count('directory')

    @property
    def clients(self) -> int:
        return self.behaviors.count('client')

    @property
    def interpreters(self) -> int:
        return self.behaviors.count('interpreter')

    @property
    def driver_services(self) -> int:
        return sum(self.behaviors.count(name) for name in ('driver-crash', 'driver-service'))

    @property
    def displays(self) -> int:
        """Resident console brokers: one device pair and scanout mapping."""
        return self.behaviors.count('display')

    @property
    def inputs(self) -> int:
        return sum(self.behaviors.count(name) for name in ('input', 'input-client'))

    @property
    def watchdog_services(self) -> int:
        """Watchdog.md: each monitored incarnation has a private reply Endpoint."""
        return sum(self.behaviors.count(name) for name in ('display', 'input', 'compositor'))

    @property
    def recoveries(self) -> int:
        """The crashed driver services, each followed by a restart."""
        return self.behaviors.count('driver-crash')

    @property
    def ps2(self) -> int:
        return self.behaviors.count('ps2')

    @property
    def block_drivers(self) -> int:
        """The AHCI driver's incarnations of the block demonstration
        : each maps the controller's registers as a
        driver does and gets a service Endpoint and WaitSet bound to it,
        beside the root service's reply Endpoint and three one-page Frames.
        With a disk (block_served) it confines its function in an IommuDomain
        with a DMA page of its own, binds its Interrupt to that WaitSet,
        serves the root service, which maps its Frames three times to fill and
        check them, and crashes with a read running; without one it is killed
        while it waits."""
        return self.behaviors.count('block-driver')

    @property
    def block_served(self) -> int:
        """Without an IOMMU no AHCI driver serves a disk: it answers every
        transfer BlockNoDevice, unless the
        controller is `trusted`. No runner case has a disk without either, so
        Guest.storage_lines needs only the recipe."""
        return self.block_drivers if self.disk and (self.iommu or self.trusted) else 0

    @property
    def fs_drivers(self) -> int:
        """The AHCI driver incarnations of the filesystem demonstration
        : each maps the registers and gets a
        service Endpoint and WaitSet, beside the root service's reply Endpoint
        and one data Frame; without a disk it answers the probe and is killed
        while it waits."""
        return self.behaviors.count('fs-driver')

    @property
    def fs_served(self) -> int:
        """With a disk, the filesystem demonstration's driver confines its
        function with a DMA page of its own, binds its Interrupt and is killed
        at the end, its drain parking that page's pin."""
        return self.fs_drivers if self.disk and (self.iommu or self.trusted) else 0

    @property
    def filesystems(self) -> int:
        """The filesystem service's incarnations:
        each gets a service Endpoint and a block-reply Endpoint, both bound to
        its WaitSet, and its window's bounce page (a Frame and a MemoryObject,
        mapped for good) and the MemoryObject that maps a client's Frame; a
        'filesystem-crash' crashes on a commit, the last is killed."""
        return sum(self.behaviors.count(name) for name in ('filesystem', 'filesystem-crash'))

    @property
    def fs_crashes(self) -> int:
        return self.behaviors.count('filesystem-crash')

    @property
    def storage_quiesces(self) -> int:
        """The Device Manager's AHCI confirmations, one after each AHCI
        driver's drain (its port reset first): each maps the controller's
        registers' page with a Frame and MemoryObject of its own and unmaps
        them."""
        return self.block_drivers + self.fs_drivers

    @property
    def storage_crashes(self) -> int:
        """The storage incarnations that end by an exception: each crashed
        commit's service and the block driver that crashes reading."""
        return self.fs_crashes + self.block_served

    @property
    def storage_killed(self) -> int:
        """The storage incarnations killed while they wait."""
        return self.fs_drivers + self.filesystems - self.fs_crashes + self.block_drivers - self.block_served

    @property
    def inventory_save_root_maps(self) -> int:
        """The root service's mapping of the data Frame that writes the device
        inventory to a writable volume (FsDemo.cpp SaveInventory): every disk
        but fat16's, whose restarted service mounts it read-only."""
        return int(self.inventory and bool(self.disk) and self.disk != 'fat16')

    @property
    def inventory_save_service_maps(self) -> int:
        """The FAT service's mapping of that Frame as it writes the page."""
        return int(self.inventory and self.disk in ('fat12', 'fat32', 'exfat', 'fat32-v2', 'exfat-v2'))

    @property
    def nic_drivers(self) -> int:
        """The NIC driver's incarnations: each
        gets a service Endpoint and a WaitSet, beside the root service's
        reply Endpoint, maps the device's first eight register pages through
        a device Frame and an uncached MemoryObject, and is killed while it
        waits or, its 10 ms backstop having ended its wait, ready to run; the
        Device Manager then resets the device and confirms it quiet."""
        return self.behaviors.count('nic-driver')

    @property
    def nic_served(self) -> int:
        """Behind the IOMMU, or trusted, each NIC driver also maps a
        descriptor page, a receive run and a transmit run (a Frame and a
        MemoryObject each, mapped and in an IommuDomain) and a MemoryObject
        for a client's page, binds its Interrupt to its WaitSet and lets the
        device master the bus; its drain parks the three DMA pins."""
        return self.nic_drivers if (self.iommu or self.trusted) else 0

    @property
    def networks(self) -> int:
        """The network stack's incarnations: each
        gets a service Endpoint and a driver-reply Endpoint, both bound to its
        WaitSet, beside the root service's socket-reply Endpoint, maps four
        one-page Frames of its own (a Frame and a MemoryObject each) in its
        window, and is killed while it waits."""
        return self.behaviors.count('network')

    @property
    def copies_beyond(self) -> int:
        """How many more of the NIC driver's copies, each a mapping and an
        unmapping, than mappings and unmappings count: at most this many."""
        return max(self.nic_copies_most - self.nic_copies_least, 0)

    @property
    def parked_pins(self) -> int:
        """The Frame pins drains park with the quarantined function
        : each driver service's two DMA pages, the
        block driver's DMA page and the client's Frame its crash's read runs
        into, the filesystem driver's DMA page and the NIC driver's three DMA
        runs."""
        return ((2 * self.driver_services if self.iommu else 0) + 2 * self.block_served + self.fs_served
                + 3 * self.nic_served)

    @property
    def killed_services(self) -> int:
        """The driver services, Directory, console brokers and network stacks
        killed while waiting."""
        return (self.driver_services - self.recoveries + self.displays + self.directories + self.inputs + self.ps2
                + self.networks)

    @property
    def device_quiesces(self) -> int:
        """The Device Manager's confirmations, one after every driver's and
        driver service's drain: each maps the registers' page at its own
        `mapping_window` with a Frame and MemoryObject of its own, finds the
        device quiet and unmaps them."""
        return self.drivers + self.driver_services

    @property
    def exits(self) -> int:
        """The incarnations that exit by ThreadExit after their report."""
        return self.reports + self.accounts + self.drivers + self.clients + self.interpreters

    @property
    def last_exit_code(self) -> int:
        """The exit code of the last incarnation that exits, zero for none or
        for an echo, which a kill ends."""
        codes = {'report': self.exit_code, 'account': self.account_exit_code, 'driver': self.driver_exit_code,
                 'echo': 0, 'client': 0xC11E0001, 'interpreter': 0xC0DE0007, 'display': 0, 'directory': 0, 'input': 0, 'input-client': 0,
                 'driver-service': 0, 'ps2': 0, 'nic-driver': 0, 'network': 0}
        exiting = [codes[behavior] for behavior in self.behaviors if behavior in codes]
        return exiting[-1] if exiting else 0

    @property
    def last_crash_tag(self) -> int:
        """The tag of the last crashing incarnation: its number."""
        crashing = ('crash', 'driver-crash', 'filesystem-crash') + (('block-driver',) if self.block_served else ())
        tags = [index + 1 for index, behavior in enumerate(self.behaviors) if behavior in crashing]
        return tags[-1] if tags else 0

    @property
    def frames(self) -> int:
        """A Frame per segment and the data pages'; an account incarnation's
        own; a driver's or a driver service's device Frame and the Device
        Manager's after it; the Frame the root service moves through its
        standalone echo; an interpreter's foreign image; a NIC driver's
        device Frame, its three DMA runs and the Device Manager's after it,
        and a network stack's four window pages."""
        return (sum(filled + private + 1 for filled, private in self.segment_counts) + self.accounts
                + 2 * self.device_quiesces + (2 * self.device_quiesces if self.iommu else 0) + min(self.echoes, 1) + self.displays + self.interpreters
                + self.storage_quiesces + 4 * self.block_drivers + self.block_served + 2 * self.fs_drivers
                + self.filesystems + self.fs_served + 2 * self.nic_drivers + 3 * self.nic_served + 4 * self.networks
                - self.repeated_staging)

    @property
    def memory_objects(self) -> int:
        """A staging and a final MemoryObject per segment with file bytes, one
        per zero-filled segment (nothing is staged in it, and it keeps its
        permissions) and the data pages'; a staged and final foreign-image object. The scanout
        tries WC before its UC fallback, creating one extra object."""
        return (sum(2 * filled + private + 1 for filled, private in self.segment_counts) + self.accounts
                + 2 * self.device_quiesces + (2 * self.device_quiesces if self.iommu else 0) + 2 * self.displays + 2 * self.interpreters - 2 * self.repeated_staging
                + int(self.inventory) + self.storage_quiesces + self.block_drivers + 4 * self.block_served
                + self.fs_drivers + 2 * self.filesystems + self.fs_served + self.fs_root_maps
                + self.inventory_save_root_maps + 2 * self.nic_drivers + 4 * self.nic_served + 4 * self.networks)

    @property
    def mappings(self) -> int:
        """MemoryMap calls: a staging and a final mapping of every segment with
        file bytes and of the data pages, the final one of the zero-filled
        segment, each account incarnation's own of its window, and each
        driver's and driver service's of its registers and the Device
        Manager's after it; staging and final mappings of the foreign bytes."""
        return (sum(2 * (filled + 1) + private for filled, private in self.segment_counts) + self.accounts
                + 2 * self.device_quiesces + (2 * self.device_quiesces if self.iommu else 0) + self.displays + 2 * self.interpreters - self.repeated_staging
                + int(self.inventory) + self.storage_quiesces + self.block_drivers + 4 * self.block_served
                + self.fs_drivers + self.filesystems + self.fs_served + self.fs_root_maps + self.fs_service_maps
                + self.inventory_save_root_maps + self.inventory_save_service_maps + 2 * self.nic_drivers
                + 3 * self.nic_served + 4 * self.networks + self.nic_copies_least)

    @property
    def last_map(self) -> tuple[int, int]:
        """The last MemoryMap's virtual start and pages: an account
        incarnation's window if one ran last, the Device Manager's register
        page after a driver, otherwise the data pages."""
        if self.behaviors and self.behaviors[-1] == 'account':
            return (self.window_start, self.window_pages)
        if self.behaviors and self.behaviors[-1] == 'driver':
            return (self.mapping_window, 1)
        return (self.data_start, self.data_pages)

    @property
    def unmappings(self) -> int:
        """MemoryUnmap calls: every mapping, or, where the components are
        drained, only the staging ones and the Device Manager's."""
        return (sum(filled + 1 for filled, _ in self.segment_counts) + self.device_quiesces + self.interpreters
                - self.repeated_staging + int(self.inventory) + self.storage_quiesces + 3 * self.block_served
                + self.fs_root_maps + self.fs_service_maps + self.inventory_save_root_maps
                + self.inventory_save_service_maps + self.nic_drivers + self.nic_copies_least
                if self.drained else self.mappings)

    @property
    def resource_domains(self) -> int:
        """The components' own ResourceDomains: one per incarnation where they
        are drained."""
        return self.incarnations if self.drained else 0


# The test pager's native fault probes (Tests/Guest/Pager/Ia64FaultScenarios.cpp,
# IA-64 fault delivery). Threads: seven single probes, the lazy-code and backing-store
# probes, two busy-pager probes and the spinner; the lazy-code, backing-store
# and chk.s probes exit by themselves; the break, privileged-operation,
# NaT-register and unaligned probes, both busy-pager breaks and the
# spinner's closing break are exceptions (the access-rights probes are
# protection faults).
FAULT_PROBE_THREADS = 12
FAULT_PROBE_EXITS = 3
FAULT_PROBE_EXCEPTIONS = 7
# Their memory: the lazy code page (a Frame, a staging and a final
# MemoryObject, the staging map, two unmaps), the backing store (a Frame and
# a MemoryObject; its two spilled pages unmapped at the leaf and again at the
# end) and the spinner (a Frame, two MemoryObjects, two maps, two unmaps).
FAULT_PROBE_FRAMES = 3
FAULT_PROBE_MEMORY_OBJECTS = 5
FAULT_PROBE_MAPS = 3
FAULT_PROBE_UNMAPS = 8


@functools.lru_cache(maxsize=None)
def ledger_reuse_cycles() -> int:
    # The guest and observer share the record's bounded loop constant.
    header = pathlib.Path(__file__).resolve().parents[4] / 'Source/User/Pager/PagerProgram.hpp'
    return Layout.constants(Layout.read(header))['LedgerReuseSelfTestCycles']


def object_management_problems(evidence: Record, *, self_test: bool, frame_pool_pages: int,
                               mapping_window: int, frame_pages: int = 2, imported: bool = False,
                               component: ComponentScenario | None = None,
                               fault_scenarios: bool = False, frame_rights: bool = False) -> list[str]:
    """object management ABI and memory management ABI: with `self_test`, the boot pager created,
    destroyed and the kernel reclaimed two Endpoints, then a Frame of
    `frame_pages` pages and a MemoryObject, then a second pair it mapped at
    `mapping_window` in its own space, used and unmapped, before it served;
    then, as root service, the `component`'s report Endpoint and its Frames
    and MemoryObjects, each mapping made and removed again, the last one its
    stack and start block; otherwise it created nothing. Either way nothing
    private remains; image records retain their shared objects and charge.
    The physical-mode scrubber returned the destroyed Frames' pages to the frame pool of
    `frame_pool_pages` pages. Every unmap advanced its space's unmap epoch,
    so there are translation-cache purges exactly when there were unmaps.
    With `fault_scenarios`, the test pager's native fault probes
     add their Frames, MemoryObjects, maps and unmaps
    (FAULT_PROBE_*)."""
    problems = _published(evidence)
    built = self_test and component is not None
    # The self-test's two, the report Endpoint, each account incarnation's
    # own and each echo's and driver service's service Endpoint, and, with a
    # service, the root service's reply Endpoint.
    count = (2 + ((1 + component.accounts + component.echoes + component.driver_services + component.displays
                   + component.directories + component.clients + (5 * component.inputs // 2) + component.ps2
                   + min(component.echoes + component.driver_services, 1) + 2 * component.block_drivers
                   + 2 * component.fs_drivers + 3 * component.filesystems + component.nic_drivers
                   + 3 * component.networks + component.watchdog_services) if built else 0)) \
        if self_test else 0
    pairs = 2 + int(frame_rights) if self_test else 0
    mapped = 1 + ledger_reuse_cycles() + 2 * frame_rights if self_test else 0
    frames = pairs + (component.frames if built else 0) + (FAULT_PROBE_FRAMES if fault_scenarios else 0)
    memory_objects = pairs + (component.memory_objects if built else 0) + (
        FAULT_PROBE_MEMORY_OBJECTS if fault_scenarios else 0)
    shared = component.shared_segments if built else 0
    shared_pages = component.shared_pages if built else 0
    maps = mapped + (component.mappings if built else 0) + (FAULT_PROBE_MAPS if fault_scenarios else 0)
    unmaps = mapped + (component.unmappings if built else 0) + (FAULT_PROBE_UNMAPS if fault_scenarios else 0)
    last = (component.last_map if built
            else ((mapping_window, frame_pages) if mapped else (0, 0)))
    # The NIC driver's copies beyond the least traffic add a mapping and an
    # unmapping each (as AMD64's).
    beyond = evidence.Mappings - maps
    if built and (0 <= beyond <= component.copies_beyond) and (evidence.Unmappings - unmaps == beyond):
        maps, unmaps = evidence.Mappings, evidence.Unmappings
    _differs(problems, 'the Endpoints created, destroyed and reclaimed',
             (evidence.Created, evidence.Destroyed, evidence.Reclaimed), (count, count, count))
    _differs(problems, 'the Frames and MemoryObjects created and reclaimed',
             (evidence.FramesCreated, evidence.FramesReclaimed, evidence.MemoryObjectsCreated,
              evidence.MemoryObjectsReclaimed), (frames, frames - shared, memory_objects, memory_objects - shared))
    _differs(problems, 'the occupied pool slots and the remaining kernel-metadata, IPC-queue, committed and '
             'resident charge', (evidence.OccupiedSlots, evidence.MetadataBytes, evidence.IpcQueueBytes,
                                 evidence.CommittedPages, evidence.ResidentPages),
             (2 * shared, shared * (evidence.FrameMetadataBytes + evidence.MemoryObjectMetadataBytes),
              0, shared_pages, shared_pages))
    if imported:
        _differs(problems, "imported free-page partition", evidence.AllocatorCleanPages + evidence.AllocatorDirtyPages,
                 frame_pool_pages - shared_pages)
    else:
        _differs(problems, "the frame pool's free and dirty pages",
                 (evidence.AllocatorCleanPages, evidence.AllocatorDirtyPages), (frame_pool_pages - shared_pages, 0))
    _differs(problems, "MemoryMap's and MemoryUnmap's mappings and the last map's virtual start and pages",
             (evidence.Mappings, evidence.Unmappings, evidence.MappedVirtualStart, evidence.MappedPages),
             (maps, unmaps) + last)
    if (evidence.TranslationCachePurges > 0) != (evidence.Unmappings > 0):
        problems.append('%d translation-cache purge(s) after %d unmapping(s)'
                        % (evidence.TranslationCachePurges, evidence.Unmappings))
    physical = evidence.MappedPhysicalStart
    if (physical % 4096 if maps else physical) or (maps and not physical):
        problems.append('the last mapped run starts at physical %#x' % physical)
    if (evidence.EndpointMetadataBytes == 0 or evidence.FrameMetadataBytes == 0
            or evidence.MemoryObjectMetadataBytes == 0 or evidence.RegisteredObjects == 0):
        problems.append('no Endpoint (%d), Frame (%d) or MemoryObject (%d) charge or registered objects (%d) '
                        'recorded' % (evidence.EndpointMetadataBytes, evidence.FrameMetadataBytes,
                                      evidence.MemoryObjectMetadataBytes, evidence.RegisteredObjects))
    return problems


def describe_object_management(evidence: Record) -> str:
    return ('%d created, %d destroyed, %d reclaimed; %d Frames and %d MemoryObjects reclaimed; frame pool %d '
            'free, %d dirty; %d bytes per Endpoint; %d registered objects; %d mapping(s) and %d unmapping(s), '
            'the last map %d pages at %#x from physical %#x; %d translation-cache purge(s)') % (
        evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.FramesReclaimed,
        evidence.MemoryObjectsReclaimed, evidence.AllocatorCleanPages, evidence.AllocatorDirtyPages,
        evidence.EndpointMetadataBytes, evidence.RegisteredObjects, evidence.Mappings, evidence.Unmappings,
        evidence.MappedPages, evidence.MappedVirtualStart, evidence.MappedPhysicalStart,
        evidence.TranslationCachePurges)


# Created domains -----------------------------------------------------------------

def domain_problems(evidence: Record, *, component: ComponentScenario, address_space_slots: int,
                    pool_slots: int, boot_roots: int = 3) -> list[str]:
    """domain creation ABI and domain teardown on IA-64: the
    root service built each of its component's incarnations in a domain of
    its own (an AddressSpace, a CapabilitySpace and a ProtectionDomain), one
    more than IA-64's `address_space_slots` run-time AddressSpace slots, and
    the kernel reclaimed all of them, so every slot is free again: the
    AddressSpace pool's as many as IA-64 provides, the other pools'
    `pool_slots`. The run-time AddressSpace slots prepared one AddressSpace
    per domain and released each, spending none, so a reused slot took a
    fresh RID block: the RID allocator issued one block per domain beyond the
    boot's `boot_roots` roots. The thread table recorded and retired each
    domain."""
    problems = _published(evidence)
    domains = component.incarnations
    if domains <= address_space_slots:
        problems.append('%d incarnations reuse none of the %d run-time AddressSpace slots'
                        % (domains, address_space_slots))
    _differs(problems, 'the AddressSpaces, CapabilitySpaces and ProtectionDomains created',
             (evidence.AddressSpacesCreated, evidence.CapabilitySpacesCreated, evidence.DomainsCreated),
             (domains, domains, domains))
    _differs(problems, 'the AddressSpaces, CapabilitySpaces and ProtectionDomains reclaimed',
             (evidence.AddressSpacesReclaimed, evidence.CapabilitySpacesReclaimed, evidence.DomainsReclaimed),
             (domains, domains, domains))
    _differs(problems, 'the free AddressSpace, CapabilitySpace and ProtectionDomain slots',
             (evidence.FreeAddressSpaceSlots, evidence.FreeCapabilitySpaceSlots, evidence.FreeDomainSlots),
             (address_space_slots, pool_slots, pool_slots))
    _differs(problems, 'the run-time AddressSpace slots prepared, released and spent',
             (evidence.SlotsPrepared, evidence.SlotsReleased, evidence.SlotsSpent), (domains, domains, 0))
    _differs(problems, "the RID allocator's AddressSpaces", evidence.AllocatedAddressSpaces, boot_roots + domains)
    _differs(problems, 'the domains the thread table recorded and retired',
             (evidence.DomainsRecorded, evidence.DomainsRetired), (domains, domains))
    return problems


def describe_domains(evidence: Record) -> str:
    return ('%d domains created and %d reclaimed; %d run-time AddressSpaces prepared, %d released; %d RID blocks '
            'issued in all') % (evidence.DomainsCreated, evidence.DomainsReclaimed, evidence.SlotsPrepared,
                                evidence.SlotsReleased, evidence.AllocatedAddressSpaces)


# Components ----------------------------------------------------------------------

# The user time slice is 50 ms of the emulator's 400-MHz ITC
# (Ia64UserPreemption.hpp): twenty slices a second.
SLICES_PER_SECOND = 20


def component_problems(evidence: Record, *, slice_ticks: int, component: ComponentScenario,
                       aborted_exits: int) -> list[str]:
    """domain creation ABI and domain teardown on IA-64. The boot measured the ITC across
    its firmware stall, and the rate is the emulator's, within a quarter of
    the one the time slice assumes (`slice_ticks` SLICES_PER_SECOND times a
    second). Each crashing
    incarnation's exception reached the root service tagged, the last with
    its number; each hanging one cost at least one timed-out wait, its
    watchdog's (any incarnation may cost more: a termination or a kill wakes
    no one, so a wait for a thread that ObjectDestroy found not yet exited
    lasts a whole watchdog period); each reporting incarnation's ThreadExit
    woke the root service; and, where the components are drained, each had a
    ResourceDomain of its own, drained and reclaimed."""
    problems = _published(evidence)
    expected = slice_ticks * SLICES_PER_SECOND // 1_000_000
    rate = evidence.ItcTicksPerMicrosecond
    if not (expected * 3 // 4 <= rate <= expected * 5 // 4):
        problems.append('the measured ITC rate is %d ticks per microsecond, expected about %d' % (rate, expected))
    if evidence.WaitExpiries < component.hangs:
        problems.append('%d of the root service\'s waits timed out; each of %d hangs needs one'
                        % (evidence.WaitExpiries, component.hangs))
    crashes = component.crashes + component.recoveries + component.storage_crashes
    if evidence.TaggedFaults < crashes:
        problems.append('%d faults of created domains reached their supervisor; each of %d crashes needs one'
                        % (evidence.TaggedFaults, crashes))
    _differs(problems, "the last tagged fault's tag", evidence.LastTag, component.last_crash_tag)
    # Domain teardown kills Ready without OnExit; aborting a waiter calls OnExit.
    # A fixed supervisor pause cannot guarantee which state a preemptible service reached. Every
    # aborted exit except the pager's untagged self-test belongs to a created domain.
    _differs(problems, 'exits of created domains (reported exits plus aborted waits)',
             evidence.DomainExits, component.exits + aborted_exits - 1)
    _differs(problems, "the components' ResourceDomains created, destroyed, reclaimed and live",
             (evidence.ResourceDomainsCreated, evidence.ResourceDomainsDestroyed, evidence.ResourceDomainsReclaimed,
              evidence.ResourceDomainsLive), (component.resource_domains,) * 3 + (0,))
    return problems


# The longest run the frame pool hands a scrub (FramePool::MaximumRunPages).
MAXIMUM_RUN_PAGES = 128


def collection_problems(record: Record) -> list[str]:
    """What the object manager's collections did: every slot a pool reclaimed, as
    the other sub-records count them, was reclaimed by a collection, and a
    scrubbed run is one to MAXIMUM_RUN_PAGES pages."""
    evidence = record.Collection
    problems = _published(evidence)
    management, domains = record.ObjectManagement, record.Domains
    reclaimed = (management.Reclaimed + management.FramesReclaimed + management.MemoryObjectsReclaimed
                 + record.Threads.Reclaimed + domains.AddressSpacesReclaimed + domains.CapabilitySpacesReclaimed
                 + domains.DomainsReclaimed + record.Components.ResourceDomainsReclaimed
                 + record.Services.WaitSetsReclaimed + record.Interrupts.Reclaimed + record.Iommu.DomainsReclaimed)
    _differs(problems, 'the slots the collections reclaimed', evidence.Reclaimed, reclaimed)
    if not evidence.ScrubRuns <= evidence.ScrubbedPages <= MAXIMUM_RUN_PAGES * evidence.ScrubRuns:
        problems.append('%d pages scrubbed in %d runs' % (evidence.ScrubbedPages, evidence.ScrubRuns))
    if evidence.Reclaimed and not evidence.Collections:
        problems.append('%d slots reclaimed without a collection' % evidence.Reclaimed)
    return problems


# The storage demonstrations' bounds, as AMD64's (Amd64Evidence): the AHCI
# driver's wait ends every 20 ms with a command in flight (AhciDriver.cpp
# CommandPollMicroseconds), at most 10 s of such polls a served driver; and
# its completions: the block demonstration's at least the token's read, the
# scratch write, its read back and the two reads of a throttle probe, at most
# BLOCK_THROTTLE_PROBES probes; the filesystem's at least the probe's read,
# at most one a sector transfer. INTx through the I/O SAPIC is masked from
# its delivery to the driver's rearm, so each completion is delivered at most
# once, plus one a crashed driver's read may raise as its drain disables the
# line.
BLOCK_POLLS = 500
BLOCK_INTERRUPTS = 5
BLOCK_THROTTLE_PROBES = 16
FS_INTERRUPTS = 1
FS_MOST_INTERRUPTS = 512
# The network demonstration's, as AMD64's: the NIC driver's wait ends every
# 10 ms while its device runs (its backstop), at most 80 s of such expiries;
# its INTx is required for none of the frames (each wake reads every cause,
# and the backstop serves the rings), at most one a frame or a send of the
# demonstration and the link's changes.
NIC_POLLS = 8000
NIC_MOST_INTERRUPTS = 64
# The network stack wakes for each of its driver's replies and each request:
# a few per frame of the demonstration.
NETWORK_WAITS = 512


def service_problems(evidence: Record, *, echoes: int, drivers: int = 0, driver_services: int = 0,
                     recoveries: int = 0, displays: int = 0, directories: int = 0, clients: int = 0, inputs: int = 0,
                     ps2: int = 0, component: 'ComponentScenario | None' = None) -> list[str]:
    """service IPC on IA-64: the pager's self-test creates a
    WaitSet, on which a created thread's timed wait blocks and expires and a
    second thread blocks until ThreadTerminate aborts its wait and it ends as
    it resumes; each of `echoes` waits on a WaitSet of its own, its Endpoint
    bound, blocks for its request or later idle wait, and can be killed Ready
    or in that wait; each of `drivers` binds its
    Interrupt to a WaitSet of its own and
    waits once, blocking unless its interrupt arrived first. Each of
    `driver_services` waits on its service WaitSet, its Endpoint
    bound, and binds its Interrupt to a second WaitSet: one of the
    `recoveries` blocks at most once, for its request, before it crashes;
    the others block at most twice for requests, once for the factorial's
    interrupt and once more in their idle wait. Every WaitSet is
    destroyed and reclaimed. Each AHCI driver and filesystem service of the
    `component`'s storage demonstrations waits on a WaitSet of its own, its
    Endpoint bound; a served driver binds its Interrupt there too, a service
    its block-reply and filesystem-manager Endpoints, and a served driver's
    wait expires as it polls a command in flight. Each NIC driver and network stack of its network
    demonstration waits on a WaitSet of its own, its Endpoint bound; the
    driver binds its Interrupt there too and its wait expires at least once
    on its backstop, the stack its driver-reply Endpoint. A service killed
    Ready has no aborted wait; a blocked wait ends as its abort resumes."""
    problems = _published(evidence)
    storage = component if component is not None else ComponentScenario(images=(), image_pages=())
    killed = (driver_services - recoveries + displays + directories + inputs + ps2 + storage.storage_killed
              + storage.networks)
    stored = storage.block_drivers + storage.fs_drivers + storage.filesystems
    networked = storage.nic_drivers + storage.networks
    created = (1 + echoes + drivers + 2 * driver_services + displays + directories + clients + inputs + ps2 + stored
               + networked)
    _differs(problems, 'the WaitSets created and reclaimed', (evidence.WaitSetsCreated, evidence.WaitSetsReclaimed),
             (created, created))
    _differs(problems, 'the Endpoints and Interrupts WaitSetBind bound', evidence.Binds,
             echoes + drivers + 2 * driver_services + displays + directories + clients + (5 * inputs // 2) + 3 * ps2
             + stored + storage.block_served + storage.fs_served + 2 * storage.filesystems + networked
             + storage.nic_served + storage.networks)
    least_expiries = 1 + storage.nic_served
    expiries = (1 + 1000 * (inputs + ps2) + BLOCK_POLLS * (storage.block_served + storage.fs_served)
                + NIC_POLLS * storage.nic_served)
    if not least_expiries <= evidence.WaitExpiries <= expiries:
        problems.append("%d created threads' waits expired, expected %d to %d" % (evidence.WaitExpiries,
                                                                                least_expiries, expiries))
    _differs(problems, 'the threads that ended as their aborted wait resumed', evidence.AbortedExits,
             evidence.WaitAborts)
    # The pager's waiting-thread probe must abort once. Services may instead
    # be Ready at termination; their kills, exit codes
    # and complete reclamation are checked by thread_problems. Keep the upper
    # bound and pair every observed aborted exit with its domain notification.
    if not 1 <= evidence.WaitAborts <= 1 + echoes + killed + storage.nic_drivers:
        problems.append('ThreadTerminate aborted %d waits, expected %d to %d' % (
            evidence.WaitAborts, 1, 1 + echoes + killed + storage.nic_drivers))
    least = 2 + echoes + killed
    most = (2 + 2 * echoes + drivers + recoveries + 4 * killed + NIC_POLLS * storage.nic_drivers
            + NETWORK_WAITS * storage.networks)
    # Console writes may each wake the broker; the root service retries queued
    # writes and fences with Query. The number of drains is schedule dependent.
    if evidence.WaitsBlocked < least or (not displays and evidence.WaitsBlocked > most):
        problems.append("%d created threads' waits blocked, expected %d to %d" % (evidence.WaitsBlocked, least, most))
    return problems


def describe_services(evidence: Record) -> str:
    return '%d WaitSets created, %d reclaimed, %d binds; %d waits blocked, %d expired, %d aborted (%d ended)' % (
        evidence.WaitSetsCreated, evidence.WaitSetsReclaimed, evidence.Binds, evidence.WaitsBlocked,
        evidence.WaitExpiries, evidence.WaitAborts, evidence.AbortedExits)


PS2_LEAST_DELIVERIES = 13    # Most of the 25 scripted events (Ps2.Injection.check)
PS2_EXTRA_DELIVERIES = 225   # Every event, and AMD64's 200 further controller bytes


def device_interrupt_problems(evidence: Record, *, drivers: int, driver_services: int = 0,
                              recoveries: int = 0, iommu: bool = False, ps2: int = 0,
                              component: 'ComponentScenario | None' = None) -> list[str]:
    """Documentation/Architecture/IommuDomain.md, on IA-64: each of `drivers`
    and `driver_services` creates an Interrupt for edu's INTx line, which its
    drain destroys and the kernel reclaims. The one interrupt a driver raises
    reaches its bound vector once, since the driver acknowledges edu before
    it rearms and the line is masked until then, as does each of its five
    transfers' (behind the IOMMU; without one it copies nothing), and the
    factorial's and, behind the IOMMU, the copy's of each service that
    serves. The factorial's of each of `recoveries`, whose
    service crashed once it ran, arrives before the drain disabled the line
    or never: one the processor latched as the drain ran may still be
    dispatched, bound or already released, so at most that many find their
    vector unbound. Each of `ps2` drivers' two i8042 lines take a delivery
    for most of the scripted input's events (the runner's own threshold:
    a late delivery may carry two events' bytes), and controller bytes
    (acknowledgements, multi-byte codes and packets) at most AMD64's 200
    more. Each AHCI driver that serves the `component`'s disk creates an
    Interrupt for the controller's line and takes its completions
    (BLOCK_INTERRUPTS, FS_INTERRUPTS); the block driver that crashes reading
    may leave one more unbound. Each NIC driver that runs its device creates
    an Interrupt for the e1000e's line, needs none of its deliveries and takes
    at most NIC_MOST_INTERRUPTS; one the line raised as its drain disabled it
    may find its vector unbound."""
    problems = _published(evidence)
    storage = component if component is not None else ComponentScenario(images=(), image_pages=())
    served = storage.block_served + storage.fs_served
    created = drivers + driver_services + 2 * ps2 + served + storage.nic_served
    _differs(problems, 'the Interrupts created, destroyed, reclaimed and live',
             (evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.Live),
             (created, created, created, 0))
    least = ((6 if iommu else 1) * drivers + (2 if iommu else 1) * driver_services - recoveries
             + BLOCK_INTERRUPTS * storage.block_served + FS_INTERRUPTS * storage.fs_served)
    unbound = recoveries + storage.block_served + storage.nic_served
    most = (least + unbound + PS2_EXTRA_DELIVERIES * ps2 + 2 * (BLOCK_THROTTLE_PROBES - 1) * storage.block_served
            + (FS_MOST_INTERRUPTS - FS_INTERRUPTS) * storage.fs_served + NIC_MOST_INTERRUPTS * storage.nic_served)
    least += PS2_LEAST_DELIVERIES * ps2
    if (evidence.Delivered < least or evidence.Spurious > unbound
            or evidence.Delivered + evidence.Spurious > most):
        problems.append('%d INTx delivered and %d spurious, expected at least %d delivered, at most %d spurious '
                        'and at most %d together' % (evidence.Delivered, evidence.Spurious, least, unbound, most))
    return problems


def describe_device_interrupts(evidence: Record) -> str:
    return '%d Interrupts created, %d destroyed, %d reclaimed, %d live; %d INTx delivered, %d spurious' % (
        evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.Live, evidence.Delivered,
        evidence.Spurious)


def device_control_problems(evidence: Record, *, drivers: int, driver_services: int = 0,
                            storage_quiesces: int = 0, nic_drivers: int = 0) -> list[str]:
    """Documentation/Architecture/IommuDomain.md on IA-64: the Device Manager
    assigns edu once to each of `drivers` and `driver_services`, the AHCI
    controller once to each of its `storage_quiesces` drivers and the e1000e once to each of `nic_drivers`, whose drains each release it into quarantine, and it lifts
    each quarantine once it found the device quiet; each driver's first
    bus-master request, before its IommuDomain is attached, is refused (a
    service asks for none)."""
    problems = _published(evidence)
    assigned = drivers + driver_services + storage_quiesces + nic_drivers
    _differs(problems, 'the assignments, releases, quarantines and confirmations',
             (evidence.Assignments, evidence.Releases, evidence.Quarantines, evidence.Confirmations),
             (assigned, assigned, assigned, assigned))
    if (evidence.BusMasterRefusals == 0) != (drivers == 0):
        problems.append('%d bus-master requests refused, expected %s' % (
            evidence.BusMasterRefusals, 'some' if drivers else 'none'))
    return problems


def describe_device_control(evidence: Record) -> str:
    return '%d assignments, %d releases, %d bus-master refusals, %d quarantines, %d confirmed quiet' % (
        evidence.Assignments, evidence.Releases, evidence.BusMasterRefusals, evidence.Quarantines,
        evidence.Confirmations)


def describe_collection(evidence: Record) -> str:
    return '%d collections reclaimed %d slots; %d runs scrubbed, %d pages' % (
        evidence.Collections, evidence.Reclaimed, evidence.ScrubRuns, evidence.ScrubbedPages)


def describe_components(evidence: Record) -> str:
    return ('%d ITC ticks per microsecond; %d wait(s) timed out; %d tagged fault(s), the last tagged %d; '
            '%d exit(s) of created domains; %d ResourceDomains created, %d destroyed, %d reclaimed, %d live') % (
        evidence.ItcTicksPerMicrosecond, evidence.WaitExpiries, evidence.TaggedFaults, evidence.LastTag,
        evidence.DomainExits, evidence.ResourceDomainsCreated, evidence.ResourceDomainsDestroyed,
        evidence.ResourceDomainsReclaimed, evidence.ResourceDomainsLive)


# Created threads -----------------------------------------------------------------

def thread_problems(evidence: Record, *, killed_exit_code: int, slots: int, pool_slots: int | None = None,
                    component: ComponentScenario | None = None, fault_scenarios: bool = False, entry_state_faults: int = 0) -> list[str]:
    """thread creation ABI and domain teardown: the boot pager, which both
    IA-64 firmwares run, created a thread that was preempted at least once
    and exited with ThreadExit, then a thread whose page fault it terminated,
    then a thread whose privileged operation reached it as an exception
     and which it terminated too, then a thread that
    spun until the pager killed it with ThreadTerminate, then a thread whose
    timed wait expired and which exited, and one killed while it waited
; then, as root
    service, one thread per `component` incarnation: a crash's exception
    terminated, a hang killed, a report exiting with the component's exit
    code, the last exit code ObjectDestroy returned (`killed_exit_code`
    without a component). The kernel reclaimed every thread and freed its
    pool and entry slots, leaving `pool_slots` (or `slots`) pool slots and
    `slots` entry slots free and no Threads usage.
    A created thread's fault may be deferred behind another, but the page
    fault and the exceptions were each delivered. With `fault_scenarios`,
    the test pager's native fault probes and spinner add their
    threads, exits and exceptions (FAULT_PROBE_*), and one of their two
    simultaneous breaks was deferred behind the other. No bound limits a
    deferral: the longest run of
    consecutive deferrals, which only timing decides, is reported and must
    lie within the created and initial threads' deferrals."""
    problems = _published(evidence)
    created = 6 + (component.incarnations if component else 0) + (FAULT_PROBE_THREADS if fault_scenarios else 0) + entry_state_faults
    exits = 2 + (component.exits if component else 0) + (FAULT_PROBE_EXITS if fault_scenarios else 0)
    kills = 2 + ((component.hangs + component.echoes + component.killed_services + component.storage_killed
                  + component.nic_drivers) if component else 0)
    _differs(problems, 'the threads created, reclaimed by ObjectDestroy and freed',
             (evidence.Created, evidence.Destroyed, evidence.Reclaimed), (created, created, created))
    _differs(problems, 'the ThreadExit calls, reaped slots and kills', (evidence.Exits, evidence.Reaped, evidence.Kills),
             (exits, created, kills))
    exceptions = 1 + ((component.crashes + component.recoveries + component.storage_crashes) if component else 0) + (
        FAULT_PROBE_EXCEPTIONS if fault_scenarios else 0) + entry_state_faults
    if evidence.Exceptions < exceptions or evidence.Exceptions >= evidence.Faults:
        problems.append('%d of %d faults were exceptions; the excepting threads need %d beside the page fault'
                        % (evidence.Exceptions, evidence.Faults, exceptions))
    _differs(problems, 'the last exit code', evidence.LastExitCode,
             (component.last_exit_code or killed_exit_code) if component else killed_exit_code)
    _differs(problems, 'the free pool and entry slots and the Threads usage',
             (evidence.FreeSlots, evidence.FreeNativeSlots, evidence.ThreadsUsage),
             (slots if pool_slots is None else pool_slots, slots, 0))
    if evidence.RunStops == 0:
        problems.append('no PL3 tick ended a created thread\'s run')
    if evidence.SlotMetadataBytes == 0:
        problems.append('no thread charge recorded')
    if evidence.Faults == 0 or evidence.Deferrals >= evidence.Faults:
        problems.append('%d faults of created threads with %d deferred; the faulting thread needs one delivered'
                        % (evidence.Faults, evidence.Deferrals))
    if fault_scenarios and evidence.Deferrals == 0:
        problems.append('the two pending fault probes did not defer behind the busy pager')
    # The pager writes all three at once (RecordManagement); a run stop's
    # publication touches only RunStops.
    if evidence.LongestDeferral > evidence.Deferrals + evidence.InitialDeferrals:
        problems.append('%d consecutive deferrals of one fault, more than the %d of created and %d of the initial '
                        'thread' % (evidence.LongestDeferral, evidence.Deferrals, evidence.InitialDeferrals))
    return problems


def describe_threads(evidence: Record) -> str:
    return ('%d created, %d reclaimed, %d freed; last exit code %#x; %d run stops; %d faults, %d deferred; '
            '%d free slots, %d free entry slots, %d Threads in use; %d bytes each; initial thread %d deferred; '
            'longest deferral %d') % (
        evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.LastExitCode, evidence.RunStops,
        evidence.Faults, evidence.Deferrals, evidence.FreeSlots, evidence.FreeNativeSlots, evidence.ThreadsUsage,
        evidence.SlotMetadataBytes, evidence.InitialDeferrals, evidence.LongestDeferral)


# Scenarios a configuration does not run ------------------------------------------

UNREACHED_WORDS = {
    'UserProbe.LocalTimerPublished': ('UserProbe', 'LocalTimerPublished'),
    'UserProbe.WaitTimerPublished': ('UserProbe', 'WaitTimerPublished'),
    'NativePreemption.Published': ('NativePreemption', 'Published'),
    'UserPreemption.Published': ('UserPreemption', 'Published'),
    'ObjectManagement.Published': ('ObjectManagement', 'Published'),
    'Threads.Published': ('Threads', 'Published'),
}


def unreached_problems(record: Record, words: tuple[str,...]) -> list[str]:
    """Sub-records of scenarios this build does not run stay unpublished."""
    problems = []
    for name in words:
        part, word = UNREACHED_WORDS[name]
        if record[part][word]:
            problems.append('%s is published, but this configuration does not run that scenario' % name)
    return problems


@dataclasses.dataclass(frozen=True)
class VtdExpectation:
    """The 7500-class hub of `itanium2-vpc-iommu`: two units in DMAR order (the bridge-scoped unit at VTBAR +
    0x1000 first, INCLUDE_PCI_ALL at VTBAR last), VT-d 1.0, four-level walks,
    a 51-bit host address width limited to the Tukwila's 50 physical bits,
    and ECAP.C set only with `vtd-coherent=on`."""
    coherent: bool = False
    bases: tuple[int, int] = (0xFED91000, 0xFED90000)
    address_bits: int = 50

    def to_json(self) -> dict:
        return dataclasses.asdict(self)

    @classmethod
    def from_json(cls, data: dict | None) -> 'VtdExpectation | None':
        return None if data is None else cls(data['coherent'], tuple(data['bases']), data['address_bits'])


# Evidence backend kinds (Ia64IommuBackendEvidence::Backend) and VT-d unit states.
BACKEND_NONE, BACKEND_VTD, BACKEND_VIRTIO = 0, 1, 2
VTD_TRANSLATING = 2
VTD_UNIT_PAGES = 37                # Root, four context tables, 4 IommuDomain slots x 8 table pages


def _vtd_problems(record: Record, functions: list, expected: VtdExpectation, drivers: int | None,
                  table_range: tuple[int, int] | None) -> list[str]:
    backend, evidence = record.IommuBackend, record.Iommu
    problems = _published(backend)
    _differs(problems, 'VT-d topology accepted (SelectionError)', backend.SelectionError, 0)
    _differs(problems, 'address limit: min(DMAR HAW + 1, PAL physical bits)', backend.HostAddressBits,
             expected.address_bits)
    _differs(problems, 'DRHD count', backend.UnitCount, len(expected.bases))
    # Ordinary endpoints only: bridges keep forwarding, displays scan out.
    ordinary = [function for function in functions
                if function.class_code != 0x0604 and (function.class_code >> 8) != 0x03]
    _differs(problems, 'ordinary endpoints swept master-off', (backend.SweptFunctions, evidence.SweptFunctions),
             (len(ordinary), len(ordinary)))
    _differs(problems, 'table pages (two units)', backend.TablePages, 2 * VTD_UNIT_PAGES)
    if backend.TablePhysical == 0 or backend.TablePhysical % 4096:
        problems.append('VT-d table pages are missing or unaligned')
    if table_range is not None and not (table_range[0] <= backend.TablePhysical
                                        and backend.TablePhysical + backend.TablePages * 4096 <= table_range[1]):
        problems.append('VT-d table pages are outside the executing image writable section')
    served = 0
    for index, base in enumerate(expected.bases):
        unit = backend.Units[index]
        served += unit.ServedFunctions
        name = 'unit %d (%#x)' % (index, base)
        _differs(problems, name + ' base, state and errors',
                 (unit.RegisterBase, unit.State, unit.ConfigureError, unit.EnableError),
                 (base, VTD_TRANSLATING, 0, 0))
        _differs(problems, name + ' version, levels and ECAP.C',
                 (unit.Version, unit.Levels, unit.ExtendedCapability & 1), (0x10, 4, int(expected.coherent)))
        if not unit.Status & (1 << 31):
            problems.append('%s does not report GSTS.TES' % name)
        # Fc... sync.i... srlz.i... mf only where the walks do not snoop.
        if expected.coherent and unit.TableFlushes:
            problems.append('%s flushed %d table ranges although ECAP.C is set' % (name, unit.TableFlushes))
        if not expected.coherent and not unit.TableFlushes:
            problems.append('%s flushed no table range although ECAP.C is clear' % name)
    _differs(problems, 'the bridge-scoped unit serves only its bridge', backend.Units[0].ServedFunctions, 1)
    _differs(problems, 'every published function is served by one unit', served, record.Pci.PublishedFunctionCount)
    for index in range(len(expected.bases), len(backend.Units)):
        if backend.Units[index].RegisterBase or backend.Units[index].State:
            problems.append('unit record %d is filled beyond the DMAR' % index)
    if drivers is not None:
        _differs(problems, 'faults recorded by the INCLUDE_PCI_ALL unit only',
                 (backend.Units[0].Faults, backend.Units[1].Faults), (0, drivers))
        if drivers:
            _differs(problems, 'the stray DMA was a write', backend.LastFaultRead, 0)
    return problems


def iommu_problems(record: Record, pci: list[dict], *, present: bool, drivers: int | None = None,
                   driver_services: int = 0, queue_range: tuple[int, int] | None = None,
                   storage_served: int = 0, vtd: VtdExpectation | None = None, trusted: bool = False,
                   nic_served: int = 0) -> list[str]:
    """The boot's IOMMU choice and state: virtio-iommu (`present`), VT-d
    (`vtd`, which implies `present`) or none, and with `drivers` the
    confined edu's IommuDomains and its one stray-write fault. Without an
    IOMMU but with the AHCI controller and the e1000e `trusted`, each AHCI driver that serves a disk
    and each of `nic_served` creates an IommuDomain of the identity backend,
    and nothing records a fault."""
    evidence = record.Iommu
    problems = _published(evidence)
    virtio = present and vtd is None
    _differs(problems, 'IOMMU backend (0 none, 1 VT-d, 2 virtio-iommu)', record.IommuBackend.Backend,
             BACKEND_VTD if vtd is not None else (BACKEND_VIRTIO if virtio else BACKEND_NONE))
    _differs(problems, 'virtio IOMMU presence', evidence.Present, int(virtio))
    _differs(problems, 'translation ready and bypass clear', (evidence.Ready, evidence.Bypass),
             (int(present), 0))
    if vtd is not None:
        problems += _vtd_problems(record, Ia64Devices.pci_functions(pci), vtd, drivers, queue_range)
    if virtio:
        _differs(problems, 'IOMMU requester at root slot 7', evidence.Requester, 7 << 3)
        _differs(problems, 'bus-master sweep covers every published function', evidence.SweptFunctions,
                 record.Pci.PublishedFunctionCount)
        _differs(problems, 'coherent permanently reserved queue pages', evidence.QueuePages, 8)
        if evidence.QueuePhysical == 0 or evidence.QueuePhysical % 4096:
            problems.append('queue reservation is missing or unaligned')
        if queue_range is not None and not (queue_range[0] <= evidence.QueuePhysical
                                           and evidence.QueuePhysical + evidence.QueuePages * 4096 <= queue_range[1]):
            problems.append('queue reservation is outside the executing image writable section')
    functions = [function for function in Ia64Devices.pci_functions(pci) if function.identifier == 0x10571AF4]
    _differs(problems, 'QEMU IOMMU functions', len(functions), int(virtio))
    if virtio and functions:
        _differs(problems, 'QEMU IOMMU root location', dataclasses.astuple(functions[0].address), (0, 0, 7, 0))
    if drivers is None:
        return problems
    # Each AHCI driver that serves a disk confines the controller in an
    # IommuDomain of its own, and each NIC driver
    # the e1000e.
    domains = (drivers + driver_services + storage_served + nic_served if present
               else (storage_served + nic_served if trusted else 0))
    _differs(problems, 'IommuDomains created and reclaimed', (evidence.DomainsCreated, evidence.DomainsReclaimed),
             (domains, domains))
    _differs(problems, 'confined stray DMA fault count', evidence.Faults, drivers if present else 0)
    if present and drivers and vtd is None:
        _differs(problems, 'stray fault endpoint, reason and address',
                 (evidence.LastFaultSource, evidence.LastFaultReason, evidence.LastFaultAddress),
                 (6 << 3, 2, 0x200000))
    if present and drivers and vtd is not None:
        # VT-d revision 1.3 appendix A: 5h, a write through an entry whose
        # Write field is clear (the unmapped 2 MiB window's directory entry or
        # leaf); 7h where a next-level fetch fails, as AMD64's observer allows.
        _differs(problems, 'stray fault endpoint and address', (evidence.LastFaultSource, evidence.LastFaultAddress),
                 (6 << 3, 0x200000))
        if evidence.LastFaultReason not in (5, 7):
            problems.append('the stray write faulted with VT-d reason %d, expected 5 or 7' % evidence.LastFaultReason)
    return problems


# Block I/O ----------------------------------------------------------------------

# The released parked pins the block evidence keeps (Ia64BlockEvidence.Released).
BLOCK_RELEASES_KEPT = 2


def block_problems(record: Record, *, parked_pins: int, token: bytes | None,
                   parked_pins_most: int | None = None) -> list[str]:
    """AHCI block I/O on IA-64: the
    drains parked at least `parked_pins` Frame pins, bounded by `parked_pins_most`
    when retired extents remain, with their quarantined functions,
    and the Device Manager's confirmations released every one; the record
    counts each release and keeps the last two, each a Frame of whole pages.
    Where the AHCI driver crashed with a read in flight into a Frame its
    client destroyed, one of them is that Frame, and the scrub that zeroed
    its page after the release found `token` there: the late write landed in
    the pinned Frame, not in reused memory."""
    evidence = record.Block
    problems = _published(evidence)
    problems += DmaLifetime.pin_problems(evidence, parked_pins,
        parked_pins if parked_pins_most is None else parked_pins_most)
    _differs(problems, 'the released pins the record counted', evidence.Releases, evidence.PinsReleased)
    kept = [evidence.Released[index] for index in range(min(evidence.Releases, BLOCK_RELEASES_KEPT))]
    for released in kept:
        if not released.Frame or not released.Pages or not released.PhysicalStart or released.PhysicalStart % 4096:
            problems.append('a released pin of Frame %#x at %#x, %d pages' % (
                released.Frame, released.PhysicalStart, released.Pages))
    if token is not None:
        samples = [struct.pack('<4Q', *released.Sample) for released in kept if released.Sampled]
        if token not in samples:
            problems.append('no released Frame held the token when it was scrubbed: %r' % samples)
    return problems


def describe_block(evidence: Record) -> str:
    kept = [evidence.Released[index] for index in range(min(evidence.Releases, BLOCK_RELEASES_KEPT))]
    return '%d pins parked, %d released, %d still parked; last %s' % (
        evidence.PinsParked, evidence.PinsReleased, evidence.ParkedPins, ', '.join(
            'Frame %#x at %#x%s' % (released.Frame, released.PhysicalStart,
                                    ' (scrubbed holding %r)' % struct.pack('<4Q', *released.Sample)
                                    if released.Sampled else '') for released in kept) or 'none')
