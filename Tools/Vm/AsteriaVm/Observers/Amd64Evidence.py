# SPDX-License-Identifier: Apache-2.0
"""The AMD64 evidence record and the host's judgment of it.

The kernel runs its scripted diagnostic scenarios, stores what it observed
in `Amd64EvidenceRecord` (Source/Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp)
and judges nothing but its own contracts; this module decodes the record
(`AsteriaVm.Evidence`, layout from the headers' assertions) and compares
each sub-record with the expectations of the case:

- runtime: the pool, the two roots and the local APIC the boot runtime
  used, against the pool layout of its headers and the CPU state;
- processors: the topology's processor count against the case's CPU count
  and QEMU's `query-cpus-fast`;
- PCI: the ECAM bus range against the q35 fact, the first function, the
  published function count and the AHCI function against `query-pci`;
- AHCI: no disk, or the IDENTIFY data decoded against the runner's own
  `-device ide-hd,serial=,model=` arguments and fixture size, and the READ
  DMA EXT of the fixture's marked sector, whose buffer the driver reads at
  the recorded address and compares with the fixture;
- application processors: every AP's timer probe, deferred delivery
  and first service against `ApplicationProcessorScenario`;
- shared-root shootdown: the probe page, value and processor masks,
  and the word every processor read, against the case's CPUs;
- WaitSet fixture: every resumed wait of the initial image against
  `WaitScenario`;
- user preemption: the user ticks, the runs they ended per
  boot domain and the test firmware's spinner against
  `UserPreemptionScenario`;
- created threads: the test firmware pager's self-test thread,
  created, preempted, exited and reclaimed, against `ThreadScenario`;
- native preemption (probe builds): one capture, resume and exit of
  the loop's frame, whose registers survived (the loop's sentinels).

Expected values come from the case (CPU count, disk), QEMU (`query-*`), the
runner's disk fixture (`Machine`), the build's C++ constants (`Amd64Layout`)
and, for what the kernel's scripted scenarios do, the scenario definitions
below.
"""
from __future__ import annotations

import dataclasses
import functools
import pathlib
import struct as binary

from AsteriaVm import Amd64Layout, Evidence, Machine
from AsteriaVm.Amd64Layout import HeaderConstants
from AsteriaVm.Evidence import Record
from AsteriaVm.Observers import Devices, SharedImages, DmaLifetime
from AsteriaVerify import Layout

HEADERS = ('Source/Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp',
           'Source/Kernel/Diagnostics/ApplicationLaunchEvidence.hpp',
           'Source/Abi/ObjectCreationLimits.hpp',
           'Source/Kernel/Memory/PhysicalPageImportEvidence.hpp',
           'Source/Abi/ComponentImageTable.hpp',
           'Source/Boot/BootFiles.hpp',
           'Source/Platform/Amd64/Diagnostics/Amd64Stop.hpp',
           'Source/Kernel/Diagnostics/StopRecord.hpp',
           'Source/Kernel/Diagnostics/Watchdog.hpp',
           'Source/Kernel/Time/ProcessorClock.hpp',
           'Source/Kernel/Diagnostics/UserProcessorEvidence.hpp',
           'Source/Kernel/Scheduler/ReadyQueue.hpp',
           'Source/Kernel/Scheduler/SchedulerTypes.hpp',
           'Source/Platform/Amd64/Scheduler/Amd64ApplicationProcessorRuntime.hpp',
           'Source/Platform/Amd64/Interrupt/Amd64UserEntry.hpp')
RECORD = 'Amd64EvidenceRecord'
SCHEMA_CONSTANT = 'Amd64EvidenceRecordSchema'
OUTCOME_ENUMERATION = 'Amd64AhciEvidenceOutcome'
RFLAGS_IF = 1 << 9                  # Architectural (Intel SDM vol. 1, 3.4.3)
PAGE = 4096                         # The architectural 4 KiB page the frame pool hands out
AHCI_CLASS = (0x01, 0x06, 0x01)     # PCI class: mass storage / SATA / AHCI 1.0
# The general registers the preemption loop stores after it resumed, in order,
# as Amd64TrapFrame field names (Amd64NativePreemptionProbe.S).
PREEMPTION_REGISTERS = ('Rax', 'Rbx', 'Rcx', 'Rdx', 'Rbp', 'R8', 'R9', 'R10', 'R11', 'R12', 'R13', 'R14', 'R15',
                        'Rdi', 'Rsi', 'Rsp')


@functools.lru_cache(maxsize=None)
def schema(root: pathlib.Path) -> Evidence.Schema:
    """The record layout of the tree at `root`."""
    return Evidence.Schema.load(pathlib.Path(root), HEADERS, RECORD, SCHEMA_CONSTANT)


@functools.lru_cache(maxsize=None)
def outcomes(root: pathlib.Path) -> dict[str, int]:
    """Amd64AhciEvidenceOutcome's enumerators."""
    return Layout.enumerators(Layout.read(pathlib.Path(root) / HEADERS[0]), OUTCOME_ENUMERATION)


def _show(value) -> str:
    return '%#x' % value if isinstance(value, int) else repr(value)


def _differs(problems: list[str], what: str, observed, expected) -> None:
    if observed != expected:
        problems.append('%s is %s, expected %s' % (what, _show(observed), _show(expected)))


def _compare(problems: list[str], table) -> None:
    for what, observed, expected in table:
        _differs(problems, what, observed, expected)


def _published(part: Record) -> list[str]:
    return [] if part.Published == 1 else ['not published (Published = %s)' % _show(part.Published)]


def _mask(cpus: int) -> int:
    return (1 << cpus) - 1


# Runtime placement and processors -------------------------------------------------

def runtime_problems(runtime: Record, constants: HeaderConstants, user_root: int) -> list[str]:
    """The pool is a page-aligned range, the two roots sit in their arenas of
    the pool layout, the BSP runs the image on the user root, and the local
    APIC is a page frame."""
    problems = _published(runtime)
    pool = runtime.PoolPhysicalStart
    if pool == 0 or pool % constants.page_size:
        problems.append('the runtime pool %#x is not a page frame' % pool)
    _compare(problems, (
        ('the shared root', runtime.SharedRootPhysicalAddress, constants.root(pool, constants.shared_root_index)),
        ('the user root', runtime.UserRootPhysicalAddress, constants.root(pool, constants.user_root_index)),
        ('the BSP CR3 (the user root it idles on)', user_root, runtime.UserRootPhysicalAddress)))
    apic = runtime.LocalApicPhysicalAddress
    if apic == 0 or apic % constants.page_size:
        problems.append('the local APIC %#x is not a page frame' % apic)
    return problems


def processor_problems(evidence: Record, query_cpus: list[dict], cpus: int) -> list[str]:
    """The ACPI topology names every processor the case asked QEMU for (as
    the runtime and the application-processor start counted them)."""
    problems = []
    if len(query_cpus) != cpus:
        problems.append('QEMU reports %d CPUs, the case requests %d' % (len(query_cpus), cpus))
    _compare(problems, (('the runtime processor count', evidence.Runtime.ProcessorCount, cpus),
                        ('the application-processor count', evidence.ApplicationProcessors.ProcessorCount, cpus)))
    return problems


# PCI and AHCI ---------------------------------------------------------------------

def qemu_functions(query_pci: list[dict]) -> list[Devices.PciFunction]:
    return sorted(Devices.pci_functions(query_pci), key=lambda function: (function.bus, function.slot,
                                                                        function.function))


def _identifier(function: Devices.PciFunction) -> int:
    return function.device << 16 | function.vendor


def pci_ecam_problems(pci: Record, query_pci: list[dict]) -> list[str]:
    """The scan covered q35's ECAM bus range, and its first function is QEMU's first function."""
    problems = _published(pci)
    _compare(problems, (('the ECAM bus count', pci.BusCount, Amd64Layout.Q35_ECAM_BUSES),
                        ('FirstFunctionPresent', pci.FirstFunctionPresent, 1)))
    functions = qemu_functions(query_pci)
    if not functions:
        return problems + ['QEMU reports no PCI function']
    first = functions[0]
    _compare(problems, (('the first function', (pci.FirstSegment, pci.FirstBus, pci.FirstDevice, pci.FirstFunction),
                         (0, first.bus, first.slot, first.function)),
                        ('the first vendor/device', pci.FirstVendorDevice, _identifier(first))))
    return problems


def pci_ahci_problems(pci: Record, query_pci: list[dict], constants: HeaderConstants) -> list[str]:
    """The kernel published every QEMU function and bound QEMU's one SATA function as AHCI."""
    functions = qemu_functions(query_pci)
    problems = _published(pci) + Devices.function_count_problems(pci.PublishedFunctionCount, functions)
    _compare(problems, (('AhciPresent', pci.AhciPresent, 1),
                        ('the AHCI availability', pci.AhciAvailability, constants.ahci_ready),
                        ('the AHCI class', (pci.AhciClassCode, pci.AhciSubclass, pci.AhciProgrammingInterface),
                         AHCI_CLASS)))
    storage = [function for function in functions if function.class_code == AHCI_CLASS[0] << 8 | AHCI_CLASS[1]]
    if len(storage) != 1:
        return problems + ['QEMU has %d SATA functions, expected exactly one' % len(storage)]
    ahci = storage[0]
    _compare(problems, (('the AHCI function', (pci.AhciSegment, pci.AhciBus, pci.AhciDevice, pci.AhciFunction),
                         (0, ahci.bus, ahci.slot, ahci.function)),
                        ('the AHCI vendor/device', pci.AhciVendorDevice, _identifier(ahci))))
    return problems


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


def disk_expectation(fixture: pathlib.Path) -> DiskExpectation:
    """The runner's disk: Machine's model and serial, the fixture's size and marked sector."""
    lba = Machine.DISK_TOKEN_SECTOR
    return DiskExpectation(Machine.DISK_MODEL, Machine.DISK_SERIAL, Devices.fixture_sector_count(fixture), lba,
                           Devices.fixture_sector(fixture, lba), Machine.DISK_AHCI_PORT)


def ahci_identify_problems(ahci: Record, disk: DiskExpectation | None, outcomes_: dict[str, int]) -> list[str]:
    """Without a disk the kernel found none; with one it identified the
    runner's disk on its port (the IDENTIFY words decoded here)."""
    problems = _published(ahci)
    if disk is None:
        _differs(problems, 'the AHCI outcome', ahci.Outcome, outcomes_['NoDisk'])
        if any(ahci.IdentifyData):
            problems.append('IDENTIFY data recorded without a disk')
        return problems
    if ahci.Outcome not in (outcomes_['Identified'], outcomes_['Read']):
        problems.append('the AHCI outcome is %#x, the disk was not identified' % ahci.Outcome)
    _differs(problems, 'the AHCI port', ahci.PortNumber, disk.port)
    return problems + Devices.identify_problems(Devices.decode_identify(ahci.IdentifyData), serial=disk.serial,
                                                model=disk.model, sectors=disk.sectors)


def ahci_read_problems(ahci: Record, disk: DiskExpectation | None, buffer: bytes | None,
                       outcomes_: dict[str, int], read_buffer: int) -> list[str]:
    """With a disk, the kernel read one sector at the fixture's marked LBA
    into its permanent DMA page (`read_buffer`, from the pool layout), which
    holds exactly that sector; without one, nothing was read."""
    problems = _published(ahci)
    if disk is None:
        if ahci.Outcome == outcomes_['Read'] or ahci.ReadBufferPhysicalAddress:
            problems.append('a READ DMA EXT was recorded without a disk')
        return problems
    _compare(problems, (('the AHCI outcome', ahci.Outcome, outcomes_['Read']),
                        ('the starting LBA', ahci.ReadStartingLba, disk.lba),
                        ('the sector count', ahci.ReadSectorCount, 1),
                        ('the transfer size', ahci.ReadTransferSizeBytes, len(disk.sector)),
                        ('the buffer size', ahci.ReadBufferSizeBytes, len(disk.sector)),
                        ('the buffer', ahci.ReadBufferPhysicalAddress, read_buffer)))
    if buffer is None:
        return problems + ['no AHCI read-buffer dump']
    return problems + Devices.buffer_problems(buffer, disk.sector, 'the AHCI READ DMA EXT buffer')


# Application processors ------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class ApplicationProcessorScenario:
    """What the boot runtime's AP scenario does on every AP, as the runner
    defines it (the kernel no longer compares these numbers):

    - the worker's first dispatch runs it once and blocks it;
    - the AP arms one TimerQueue timer: one local-timer interrupt arrives
      after its deadline, the DeferredTimerWorker delivers that one timer and
      leaves the queue empty, the AP claims the Notification once (claim
      sequence 1), and the controller disarms the local timer (LVT masked
      at the timer vector, initial count 0): one completed delivery;
    - the BSP's first service request wakes the worker once (run 2, one
      completed service), and the shootdown probe's request once more (run 3).
    """
    initial_runs: int = 1
    timer_interrupts: int = 1
    deliveries: int = 1
    notification_claims: int = 1
    services: int = 1
    service_runs: int = 2
    probe_runs: int = 3


def application_processor_problems(slot: Record, processor: int, constants: HeaderConstants,
                                   scenario: ApplicationProcessorScenario = ApplicationProcessorScenario()
                                   ) -> list[str]:
    """One AP's recorded scenario."""
    problems = _published(slot)
    s, timer = scenario, slot.Timer
    _compare(problems, (
        ('Processor', slot.Processor, processor),
        ('State (serving)', slot.State, 1),
        ('TimerInterrupts', slot.TimerInterrupts, s.timer_interrupts),
        ('TimerDeliveries', slot.TimerDeliveries, s.deliveries),
        ('Timer.InitialRuns', timer.InitialRuns, s.initial_runs),
        ('Timer.ObservedInterrupts', timer.ObservedInterrupts, s.timer_interrupts),
        ('Timer.DeliveredCount', timer.DeliveredCount, s.deliveries),
        ('Timer.DrainStop', timer.DrainStop, constants.drain_stop_empty),
        ('Timer.DeliveredOwnTimer', timer.DeliveredOwnTimer, 1),
        ('the queue after the drain (next deadline, scheduled, pending, claimed)',
         (timer.QueueNextDeadlineTicks, timer.QueueScheduledEntries, timer.QueuePendingEntries,
          timer.QueueClaimedEntries), (0, 0, 0, 0)),
        ('Timer.NotificationClaimSequence', timer.NotificationClaimSequence, s.notification_claims),
        ('Timer.NotificationState', timer.NotificationState, constants.notification_ready),
        ('Timer.NotificationPending', timer.NotificationPending, 0),
        ('Timer.NotificationLastClaimSequence', timer.NotificationLastClaimSequence, s.notification_claims),
        ('Timer.DisarmedDeadlineState', timer.DisarmedDeadlineState, constants.deadline_disarmed),
        ('Timer.DisarmedDeadlineTicks', timer.DisarmedDeadlineTicks, 0),
        ('Timer.DisarmedTimerLvt', timer.DisarmedTimerLvt, constants.timer_masked_lvt),
        ('Timer.DisarmedInitialCount', timer.DisarmedInitialCount, 0),
        ('ServiceCompletions', slot.ServiceCompletions, s.services),
        ('ServiceRuns', slot.ServiceRuns, s.service_runs)))
    if timer.RequestedDeadlineTicks == 0 or timer.ObservedTicks < timer.RequestedDeadlineTicks:
        problems.append('the timer interrupt was seen at %#x, before its deadline %#x' % (
            timer.ObservedTicks, timer.RequestedDeadlineTicks))
    return problems


def application_processors_problems(evidence: Record, cpus: int, constants: HeaderConstants,
                                    scenario: ApplicationProcessorScenario = ApplicationProcessorScenario()
                                    ) -> list[str]:
    """Every AP of the case served its recorded scenario; the BSP's slot
    and the slots of absent processors stay empty."""
    problems = _published(evidence)
    _differs(problems, 'the serving processor mask', evidence.ServingProcessorMask, _mask(cpus) & ~1)
    for index, slot in enumerate(evidence.Processors):
        if 0 < index < cpus:
            problems += ['CPU %d: %s' % (index, problem)
                         for problem in application_processor_problems(slot, index, constants, scenario)]
        elif slot.Published:
            problems.append('slot %d is published, but CPU %d is %s' % (
                index, index, 'the BSP' if index == 0 else 'not part of the case'))
    return problems


# Shared-root shootdown --------------------------------------------------------------

def shootdown_problems(evidence: Record, cpus: int, constants: HeaderConstants, pool: int,
                       scenario: ApplicationProcessorScenario = ApplicationProcessorScenario()) -> list[str]:
    """The probe page of the pool layout mapped at the kernel's probe
    address with the kernel's probe value, a shootdown that every resident
    processor acknowledged (queued to every AP, the BSP invalidating
    locally), and every processor reading the value through the new
    mapping (each AP on its third worker run)."""
    problems = _published(evidence)
    everyone = _mask(cpus)
    _compare(problems, (
        ('the page', evidence.VirtualAddress, constants.shared_probe_address),
        ('the physical page', evidence.PhysicalAddress, constants.pool_page(pool, constants.shared_probe_page)),
        ('the stored value', evidence.Value, constants.shared_probe_value),
        ('the resident mask', evidence.ResidentProcessorMask, everyone),
        ('the target mask', evidence.TargetProcessorMask, everyone),
        ('the queued mask', evidence.QueuedProcessorMask, everyone & ~1),
        ('the mask acknowledged at dispatch', evidence.DispatchAcknowledgedMask, 1),
        ('the acknowledged mask', evidence.AcknowledgedProcessorMask, everyone),
        ('the words read (by CPU)', list(evidence.ObservedValues),
         [evidence.Value if cpu < cpus else 0 for cpu in range(len(evidence.ObservedValues))]),
        ('the worker runs after the read (by CPU)', list(evidence.ServiceRuns),
         [scenario.probe_runs if 0 < cpu < cpus else 0 for cpu in range(len(evidence.ServiceRuns))])))
    if evidence.ShootdownSequence == 0:
        problems.append('no shootdown sequence')
    return problems


def smp_ipi_problems(evidence: Record, cpus: int) -> list[str]:
    """processor scheduling: messages reached every processor's installed IDT."""
    problems = _published(evidence)
    for cpu in range(cpus):
        sent, taken = evidence.ShootdownSent[cpu], evidence.ShootdownTaken[cpu]
        if sent == 0 or taken == 0:
            problems.append('CPU %d has no delivered shootdown IPI (sent %d, taken %d)' % (cpu, sent, taken))
        if taken > sent:
            problems.append('CPU %d took more shootdown IPIs than were sent' % cpu)
    return problems


# The WaitSet fixture ------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class WaitScenario:
    """What the initial image and the kernel's wait fixture do, as the runner
    defines it: both user images (the guest test program and the production
    image) wait on their WaitSet twice; each wait suspends, one timer
    interrupt after the fixture's deadline delivers the fixture's one timer,
    and the dispatcher completes the wait with the registered tag and the
    registration's sequence 1, which the response returns."""
    waits: int = 2
    interrupts: int = 1
    deliveries: int = 1
    registration_sequence: int = 1


def wait_problems(evidence: Record, constants: HeaderConstants, scenario: WaitScenario = WaitScenario()) -> list[str]:
    """The fixture's registration and every resumed wait."""
    problems = _published(evidence)
    s = scenario
    _compare(problems, (('the registered tag', evidence.RegisteredTag, constants.wait_tag),
                        ('WaitCalls', evidence.WaitCalls, s.waits),
                        ('SuspendedWaits', evidence.SuspendedWaits, s.waits),
                        ('ResumedWaits', evidence.ResumedWaits, s.waits)))
    for index, wait in enumerate(evidence.Resumptions):
        if index >= min(evidence.ResumedWaits, len(evidence.Resumptions)):
            if any(value for value in wait.values()):
                problems.append('resumption %d is recorded, but only %d waits resumed' % (index, evidence.ResumedWaits))
            continue
        found: list[str] = []
        _compare(found, (
            ('ObservedInterrupts', wait.ObservedInterrupts, s.interrupts),
            ('DeliveredCount', wait.DeliveredCount, s.deliveries),
            ('DrainStop', wait.DrainStop, constants.drain_stop_empty),
            ('DeliveredOwnTimer', wait.DeliveredOwnTimer, 1),
            ('the queue after the drain (next deadline, scheduled, pending, claimed)',
             (wait.QueueNextDeadlineTicks, wait.QueueScheduledEntries, wait.QueuePendingEntries,
              wait.QueueClaimedEntries), (0, 0, 0, 0)),
            ('DeadlineState', wait.DeadlineState, constants.deadline_disarmed),
            ('CompletedTag', wait.CompletedTag, evidence.RegisteredTag),
            ('CompletedRegistrationSequence', wait.CompletedRegistrationSequence, s.registration_sequence),
            ('the response (error, value 0, value 1)', (wait.ResponseError, wait.ResponseValue0, wait.ResponseValue1),
             (constants.system_call_error_none, evidence.RegisteredTag, s.registration_sequence))))
        if wait.DeadlineTicks == 0 or wait.ObservedTicks < wait.DeadlineTicks:
            found.append('the interrupt was seen at %#x, before the deadline %#x' % (wait.ObservedTicks,
                                                                                     wait.DeadlineTicks))
        problems += ['wait %d: %s' % (index + 1, problem) for problem in found]
    return problems


# User preemption -----------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class UserPreemptionScenario:
    """What the boot domains do under user preemption, as the runner defines
    it: the test firmware's
    spinner counts across many time slices, so user ticks end its run at
    least once, then writes one line and faults, so it never reaches its
    final wait; the production firmware has no
    spinner. A tick ends at most one run, and a tick whose fired deadline was
    the TimerQueue's ends one too."""
    spinner: bool


SPINNER_DOMAIN = 2   # RunStops slot (Amd64EvidenceSpinnerDomain)


def user_preemption_problems(evidence: Record, scenario: UserPreemptionScenario) -> list[str]:
    problems = _published(evidence)
    runs = sum(evidence.RunStops)
    if scenario.spinner:
        _compare(problems, (('SpinnerPresent', evidence.SpinnerPresent, 1),
                            ('SpinnerLines', evidence.SpinnerLines, 1),
                            ('SpinnerWaitSuspended', evidence.SpinnerWaitSuspended, 0)))
        if evidence.RunStops[SPINNER_DOMAIN] == 0:
            problems.append('no user tick ended a run of the spinner')
    else:
        _compare(problems, (('the spinner (present, runs ended, lines, wait suspended)',
                             (evidence.SpinnerPresent, evidence.RunStops[SPINNER_DOMAIN], evidence.SpinnerLines,
                              evidence.SpinnerWaitSuspended), (0, 0, 0, 0)),))
    if not evidence.TimerQueueTicks <= runs <= evidence.UserTicks:
        problems.append('%d TimerQueue ticks, %d runs ended and %d user ticks are not in that order' % (
            evidence.TimerQueueTicks, runs, evidence.UserTicks))
    return problems


# Supervised fault termination ----------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class SupervisionScenario:
    """What the boot pager terminates, as the runner defines it
    : with
    `spinner_faults`, the test firmware's spinner faults after its line and
    the pager ends it with FaultResolve Terminate, exiting it with
    `exit_code` (Abi::FaultTerminatedExitCode); with `thread_faults` the
    pager also terminates the thread it created to fault
     and the one whose invalid instruction reaches
    it as an exception; otherwise nothing
    is terminated. A boot
    thread's fault may be deferred behind another any number of times, but
    only where something faults."""
    spinner_faults: bool
    exit_code: int = 0
    thread_faults: bool = False
    component_crashes: int = 0
    return_boundary: bool = False
    entry_state: int = 0


def supervision_problems(evidence: Record, scenario: SupervisionScenario) -> list[str]:
    problems = _published(evidence)
    terminations = int(scenario.spinner_faults) + 2 * int(scenario.thread_faults) + scenario.component_crashes + (int(scenario.return_boundary) + int(scenario.entry_state))
    expected = (terminations, 1, scenario.exit_code) if scenario.spinner_faults else (terminations, 0, 0)
    _compare(problems, (('the terminations, spinner exit and exit code',
                         (evidence.Terminations, evidence.SpinnerExited, evidence.SpinnerExitCode), expected),))
    if not scenario.spinner_faults and evidence.Deferrals:
        problems.append('%d deferred faults where nothing faults' % evidence.Deferrals)
    return problems


def describe_supervision(evidence: Record) -> str:
    return '%d termination(s); spinner exited %d with exit code %#x; %d deferred fault(s)' % (
        evidence.Terminations, evidence.SpinnerExited, evidence.SpinnerExitCode, evidence.Deferrals)


# Object management ---------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class ComponentScenario:
    """What the boot pager builds as root service, as the runner defines it:
    with `built` (the test firmware), one incarnation per entry of
    `behaviors` ('crash', 'hang', 'report', 'account' or 'echo') of the embedded image of its behaviour, whose
    loadable segments `images` gives, each in a fresh domain (an AddressSpace, a CapabilitySpace and
    a ProtectionDomain tagged with its number), read-only segments staged once
    per image and mapped through the same sealed MemoryObjects,
    beside one Frame and MemoryObject holding its stack and start block of
    `data_pages` pages at `data_start`, and one report Endpoint. A crashing
    incarnation faults (a tagged exception the pager terminates); a hanging
    one spins until the root service's timed wait expires and it kills the
    thread; a reporting one exits with `exit_code`;
    an echoing one gets a service Endpoint and a WaitSet it
    is bound to, receives a request with a reply capability and a moved
    Frame, which the root service creates, moves the Frame back, and is
    killed while it waits again, its wait aborted. Each dead domain is torn down before the next
    is built: its ProtectionDomain, CapabilitySpace and AddressSpace are
    reclaimed and the next creation prepares the arena again, so `slots`
    slots of each space serve every incarnation and none stays live. Where
    the components are `drained`, each has a ResourceDomain of its
    own, a child of the grant charged with private objects, and its
    teardown is that domain's drain, which takes its final mappings with its
    AddressSpace; every such domain is reclaimed."""
    built: bool
    behaviors: tuple[str,...] = ('crash', 'hang', 'report')
    drained: bool = False      # Each component has its own ResourceDomain, which its teardown drains
    exit_code: int = 0
    interpreted_exit_code: int = 0
    data_start: int = 0
    data_pages: int = 3
    slots: int = 2
    disk: bool = False         # The AHCI driver serves a disk
    distributed: bool = False  # Remote kills may catch a service Ready rather than waiting
    smp_workers: int = 0       # Processor scheduling shared-root spinners, from their protocol header
    scanout_mappings: int = 2  # A display broker's mappings of its scanout, a page table's worth each
    # The filesystem demonstration's own mappings of the root service's data
    # Frame (to fill or check a page) and the service's of a client's Frame in
    # its window (the FAT path's Reads and Writes), as its script makes them.
    fs_root_maps: int = 0
    inventory: bool = False
    fs_service_maps: int = 0
    # The NIC driver's copies through its client page, each a MemoryMap and a
    # MemoryUnmap of a stack's Frame: one a frame it received into a credit or
    # sent, at least the demonstration's least traffic and at most the frames
    # the runner captured (Tools/Vm/AsteriaVm/Lan.py).
    nic_copies_least: int = 0
    nic_copies_most: int = 0
    # (segments with file bytes, zero-filled segments) of each embedded image,
    # by name (component, directory, client, interpreter), as the build made
    # them; the defaults are the production PIC layouts.
    images: tuple[tuple[str, tuple[int, int]],...] = (
        ('display', (2, 1)), ('component', (2, 1)), ('directory', (2, 1)), ('client', (1, 1)), ('interpreter', (2, 1)))

    image_pages: tuple[tuple[str, int],...] = (('display', 12), ('component', 31), ('directory', 6), ('client', 4), ('interpreter', 5))

    @property
    def shared_segments(self) -> int:
        names = {SharedImages.image_name(behavior) for behavior in self.behaviors}
        return (sum(dict(self.images)[name][0] for name in names)
                + (dict(self.images)['smp'][0] if self.smp_workers else 0)) if self.built else 0

    @property
    def shared_pages(self) -> int:
        return sum(dict(self.image_pages)[name] for name in
                   {SharedImages.image_name(b) for b in self.behaviors}) + (dict(self.image_pages)['smp'] if self.smp_workers else 0) if self.built else 0

    @property
    def repeated_staging(self) -> int:
        return (sum(dict(self.images)[SharedImages.image_name(behavior)][0] for behavior in self.behaviors)
                - self.shared_segments + (dict(self.images)['smp'][0] if self.smp_workers else 0)) if self.built else 0

    @property
    def incarnations(self) -> int:
        return len(self.behaviors)

    @property
    def domains(self) -> int:
        return self.incarnations + bool(self.smp_workers) if self.built else 0

    @property
    def crashes(self) -> int:
        return (sum(self.behaviors.count(name) for name in ('crash', 'driver-crash', 'filesystem-crash'))
                + self.block_served
                if self.built else 0)

    @property
    def reports(self) -> int:
        return self.behaviors.count('report') if self.built else 0

    @property
    def hangs(self) -> int:
        return self.behaviors.count('hang') if self.built else 0

    @property
    def echoes(self) -> int:
        return self.behaviors.count('echo') if self.built else 0

    @property
    def services(self) -> int:
        """The services the root service runs side by side,
        Echo's restart included: each gets an Endpoint and a WaitSet
        bound to it and is killed while it waits."""
        return sum(self.behaviors.count(name) for name in ('directory', 'service')) if self.built else 0

    @property
    def clients(self) -> int:
        """The clients of those services: each gets an Endpoint and a
        WaitSet bound to it, as a service does, and exits after its report."""
        return self.behaviors.count('client') if self.built else 0

    @property
    def accounts(self) -> int:
        """The incarnations that create in their own ResourceDomain
        :
        each creates an Endpoint and a Frame with a MemoryObject, maps them
        into its window, is refused a page beyond its domain's and executable
        memory without Code, reports and
        exits; its drain takes what it created."""
        return self.behaviors.count('account') if self.built else 0

    @property
    def drivers(self) -> int:
        """The incarnations that drive the assignable device: each maps its
        registers through a device Frame and an uncached MemoryObject of its
        own domain, drives it, maps two pages of its own (a Frame and a
        MemoryObject each) for the device's DMA, reports and exits; its drain
        takes the mappings."""
        return self.behaviors.count('driver') if self.built else 0

    @property
    def driver_services(self) -> int:
        """The driver's service incarnations: each maps, confines and connects
        the device as a driver does, gets a service Endpoint and WaitSet
        bound to it and creates a WaitSet for its Interrupt. A 'driver-crash'
        crashes as its copy's write-back transfer starts and is terminated
        by its supervisor; a 'driver-service' is restarted after it, serves
        and is killed while it waits."""
        return sum(self.behaviors.count(name) for name in ('driver-crash', 'driver-service')) if self.built else 0

    @property
    def recoveries(self) -> int:
        """The crashed driver services, each followed by a restart."""
        return self.behaviors.count('driver-crash') if self.built else 0

    @property
    def block_drivers(self) -> int:
        """The AHCI driver's incarnations: each maps the controller's registers as a driver does
        and gets a service Endpoint and WaitSet bound to it, beside the root
        service's reply Endpoint and three one-page Frames. With a `disk`
        (block_served) it confines its function in an IommuDomain with a DMA
        page of its own (a Frame and a MemoryObject), binds its Interrupt to
        that WaitSet, serves the root service, which maps its Frames three
        times to fill and check them, and crashes with a read running;
        without one it is killed while it waits."""
        return self.behaviors.count('block-driver') if self.built else 0

    @property
    def block_served(self) -> int:
        return self.block_drivers if self.disk else 0

    @property
    def fs_drivers(self) -> int:
        """The AHCI driver incarnations of the filesystem demonstration
, before the block
        driver's: each maps the controller's registers and gets a service
        Endpoint and WaitSet as the block driver does, beside the root
        service's reply Endpoint and one data Frame; without a disk it answers
        the root service's probe and is killed while it waits."""
        return self.behaviors.count('fs-driver') if self.built else 0

    @property
    def fs_served(self) -> int:
        """With a disk, the filesystem demonstration's driver serves it: it
        confines its function with a DMA page of its own (a Frame and a
        MemoryObject, mapped), binds its Interrupt and is killed at the end,
        its drain parking that page's pin."""
        return self.fs_drivers if self.disk else 0

    @property
    def filesystems(self) -> int:
        """The filesystem service's incarnations: each gets service,
        block-reply and manager Endpoints, all bound to its WaitSet, and its window's
        bounce page (a Frame and a MemoryObject, mapped for good) and the
        MemoryObject that maps a client's Frame; a 'filesystem-crash'
        crashes on a commit and is terminated, the last 'filesystem' is
        killed while it waits."""
        return sum(self.behaviors.count(name) for name in ('filesystem', 'filesystem-crash')) if self.built else 0

    @property
    def fs_crashes(self) -> int:
        return self.behaviors.count('filesystem-crash') if self.built else 0

    @property
    def nic_drivers(self) -> int:
        """The NIC driver's incarnations: each gets a service Endpoint and a WaitSet with
        Manage, beside the root service's reply Endpoint; maps the device's
        first eight register pages through a device Frame and an uncached
        MemoryObject, a descriptor page, a receive run and a transmit run (a
        Frame and a MemoryObject each, mapped and in an IommuDomain) and a
        MemoryObject for a client's page, binds its Interrupt to its WaitSet,
        waits with a 10 ms backstop, and is killed while it waits; its drain
        parks the three DMA pins."""
        return self.behaviors.count('nic-driver') if self.built else 0

    @property
    def networks(self) -> int:
        """The network stack's incarnations: each gets a service Endpoint and a
        driver-reply Endpoint, both bound to its WaitSet, beside the root
        service's socket-reply Endpoint, maps four one-page Frames of its own
        (a Frame and a MemoryObject each) in its window, and is killed while it
        waits. The NIC driver beside it copies each frame through its client
        page."""
        return self.behaviors.count('network') if self.built else 0

    @property
    def device_quiesces(self) -> int:
        """The Device Manager's confirmations that a drained driver's device is
        quiet, one after every
        driver incarnation's drain: each maps the device's registers in the
        root service's own space through a device Frame and a MemoryObject,
        unmaps them and destroys both."""
        return self.drivers + self.driver_services + self.block_drivers + self.fs_drivers + self.nic_drivers

    @property
    def displays(self) -> int:
        """The display brokers:
        each, listed last as it is torn down last, gets a service Endpoint and
        WaitSet bound to it, maps the scanout through a device Frame and an
        write-combining MemoryObject of its own domain in `scanout_mappings`
        mappings, serves the root service's lines and is killed while it
        waits."""
        return self.behaviors.count('display') if self.built else 0

    @property
    def inputs(self) -> int:
        """Broker and probe services: two threads, five total Endpoints,
        two WaitSets and five bindings in the scripted demonstration."""
        return sum(self.behaviors.count(name) for name in ('input', 'input-client')) if self.built else 0

    @property
    def watchdog_services(self) -> int:
        """Watchdog.md: each monitored incarnation has a private reply Endpoint."""
        return sum(self.behaviors.count(name) for name in ('display', 'input', 'compositor')) if self.built else 0

    @property
    def ps2(self) -> int:
        return self.behaviors.count('ps2') if self.built else 0

    @property
    def interpreters(self) -> int:
        """The interpreters the loader's demonstration runs: each
        is given a foreign executable, reports and exits."""
        return self.behaviors.count('interpret') if self.built else 0

    def _segments(self, behavior: str) -> tuple[int, int]:
        """The segments with file bytes and the zero-filled ones of the image
        an incarnation of behavior runs."""
        # Each behaviour runs its image's segments as the build made them; an
        # interpreter's also has its guest, staged like file bytes.
        name = SharedImages.image_name(behavior)
        filled, zeroed = dict(self.images)[name]
        return (filled + 1, zeroed) if behavior == 'interpret' else (filled, zeroed)

    @property
    def last_exit_code(self) -> int:
        """The exit code of the last incarnation: a reporting one's, an
        interpreter's, or the kernel's for a killed one."""
        last = self.behaviors[-1] if self.behaviors else ''
        return {'report': self.exit_code, 'interpret': self.interpreted_exit_code}.get(last, 0)

    @property
    def last_crash_tag(self) -> int:
        """The tag of the last crashing incarnation: its number."""
        crashing = ('crash', 'driver-crash', 'filesystem-crash') + (('block-driver',) if self.disk else ())
        tags = [index + 1 for index, behavior in enumerate(self.behaviors) if behavior in crashing]
        return tags[-1] if (tags and self.built) else 0

    @property
    def smp_segments(self) -> tuple[int, int]:
        return dict(self.images)['smp'] if self.smp_workers else (0, 0)

    @property
    def frames(self) -> int:
        """Each domain's segments and data, each echo's moved Frame, each
        account incarnation's own and each driver's device Frame and two DMA
        pages."""
        if not self.built:
            return 0
        return (sum(sum(self._segments(behavior)) + 1 for behavior in self.behaviors) + self.echoes + self.accounts
                + 3 * (self.drivers + self.driver_services) + self.device_quiesces + 4 * self.block_drivers
                + self.block_served + self.displays + 2 * self.fs_drivers + self.filesystems + self.fs_served
                + 4 * self.nic_drivers + 4 * self.networks - self.repeated_staging
                + (sum(self.smp_segments) + 2 if self.smp_workers else 0))

    @property
    def inventory_root_maps(self) -> int:
        return int(self.inventory) * (1 + int(bool(self.disk) and self.disk != 'fat16'))

    @property
    def inventory_service_maps(self) -> int:
        return int(self.inventory and self.disk in ('fat12', 'fat32', 'exfat', 'fat32-v2', 'exfat-v2'))

    @property
    def memory_objects(self) -> int:
        """A staging and a final MemoryObject per segment with file bytes,
        one per zero-filled segment and one for the data, each account
        incarnation's own, and each driver's three."""
        if not self.built:
            return 0
        return (sum(2 * filled + zeroed + 1 for filled, zeroed in map(self._segments, self.behaviors)) + self.accounts
                + 3 * (self.drivers + self.driver_services) + self.device_quiesces + self.block_drivers
                + 4 * self.block_served + self.displays + self.fs_drivers + 2 * self.filesystems + self.fs_served
                + self.fs_root_maps + self.inventory_root_maps + 5 * self.nic_drivers + 4 * self.networks - 2 * self.repeated_staging
                + (2 * self.smp_segments[0] + self.smp_segments[1] + 2 if self.smp_workers else 0))

    @property
    def mappings(self) -> int:
        """MemoryMap calls: a staging and a final mapping of every segment
        with file bytes and of the data pages, one mapping of a zero-filled
        segment, each account incarnation's own of its window, and each
        driver's three of its."""
        if not self.built:
            return 0
        return (sum(2 * (filled + 1) + zeroed for filled, zeroed in map(self._segments, self.behaviors)) + self.accounts
                + 3 * (self.drivers + self.driver_services) + self.device_quiesces + self.block_drivers
                + 4 * self.block_served + self.scanout_mappings * self.displays + self.fs_drivers
                + self.filesystems + self.fs_served + self.fs_root_maps + self.fs_service_maps
                + self.inventory_root_maps + self.inventory_service_maps
                + 4 * self.nic_drivers + 4 * self.networks + self.nic_copies_least - self.repeated_staging
                + (2 * (self.smp_segments[0] + 1) + self.smp_segments[1] + 2 if self.smp_workers else 0))

    @property
    def unmappings(self) -> int:
        """MemoryUnmap calls: every mapping, or, where the teardown drains the
        component's ResourceDomain, only the staging ones; the
        drain takes the rest with the AddressSpace."""
        if not self.drained:
            return self.mappings
        return (sum(filled + 1 for filled, _ in map(self._segments, self.behaviors)) + self.device_quiesces
                + 3 * self.block_served + self.fs_root_maps + self.fs_service_maps
                + self.inventory_root_maps + self.inventory_service_maps + self.nic_copies_least
                - self.repeated_staging + (self.smp_segments[0] + 3 if self.smp_workers else 0)
                if self.built else 0)

    @property
    def copies_beyond(self) -> int:
        """How many more of the NIC driver's copies, each a mapping and an
        unmapping, than mappings and unmappings count: at most this many."""
        return max(self.nic_copies_most - self.nic_copies_least, 0) if self.built else 0

    @property
    def resource_domains(self) -> int:
        """The components' own ResourceDomains: one per incarnation where they
        are drained, each destroyed and reclaimed by its teardown."""
        return self.domains if self.drained else 0


def component_problems(evidence: Record, scenario: ComponentScenario) -> list[str]:
    if not scenario.built:
        unbuilt: list[str] = []
        _differs(unbuilt, 'the record of created domains, which stays unpublished',
                 tuple(getattr(evidence, name) for name in COMPONENT_FIELDS), (0,) * len(COMPONENT_FIELDS))
        return unbuilt
    problems = _published(evidence)
    domains = scenario.domains
    _compare(problems, (
        ('the AddressSpaces created, live, reclaimed and free',
         (evidence.AddressSpacesCreated, evidence.AddressSpacesLive, evidence.AddressSpacesReclaimed,
          evidence.AddressSpacesFree), (domains, 0, domains, scenario.slots)),
        ('the CapabilitySpaces created, live, reclaimed and free',
         (evidence.CapabilitySpacesCreated, evidence.CapabilitySpacesLive, evidence.CapabilitySpacesReclaimed,
          evidence.CapabilitySpacesFree), (domains, 0, domains, scenario.slots)),
        ('the arenas prepared', evidence.ArenasPrepared, domains),
        ('the ProtectionDomains created, destroyed and reclaimed',
         (evidence.DomainsCreated, evidence.DomainsDestroyed, evidence.DomainsReclaimed), (domains, domains, domains)),
        ('the last tagged fault\'s tag', evidence.LastTag, scenario.last_crash_tag),
        ('the components\' ResourceDomains created, destroyed, reclaimed and live',
         (evidence.ResourceDomainsCreated, evidence.ResourceDomainsDestroyed, evidence.ResourceDomainsReclaimed,
          evidence.ResourceDomainsLive), (scenario.resource_domains,) * 3 + (0,))))
    # A NIC driver's backstop may have woken it as its kill came, so it may be killed ready to run
    # rather than in its wait: then no exit is reported for it (Amd64UserThreadTable::Terminate).
    exits = (scenario.reports + scenario.echoes + scenario.services + scenario.clients + scenario.interpreters
             + scenario.accounts + scenario.drivers + scenario.driver_services - scenario.recoveries
             + scenario.block_drivers - scenario.block_served + scenario.displays + scenario.fs_drivers
             + scenario.filesystems - scenario.fs_crashes + scenario.networks + scenario.inputs + scenario.ps2 + scenario.smp_workers)
    # A Ready kill reclaims directly; Running/waiting termination reports
    # from the native continuation. AP scheduling makes either outcome valid.
    least_exits = (scenario.reports + scenario.clients + scenario.interpreters + scenario.accounts
                   + scenario.drivers + scenario.smp_workers) if scenario.distributed else exits
    most_exits = exits + scenario.nic_drivers + (scenario.hangs if scenario.distributed else 0)
    if not least_exits <= evidence.DomainExits <= most_exits:
        problems.append('the created domains\' exits are %d, expected %d to %d' % (
            evidence.DomainExits, least_exits, most_exits))
    if evidence.WaitExpiries < scenario.hangs:
        problems.append('%d of the root service\'s waits timed out; each of %d hangs needs one'
                        % (evidence.WaitExpiries, scenario.hangs))
    if evidence.TaggedFaults < scenario.crashes:
        problems.append('%d faults of created domains reached their supervisor; each of %d crashes needs one'
                        % (evidence.TaggedFaults, scenario.crashes))
    if evidence.AddressSpaceBytes == 0 or evidence.CapabilitySpaceBytes == 0:
        problems.append('no AddressSpace (%d) or CapabilitySpace (%d) charge recorded'
                        % (evidence.AddressSpaceBytes, evidence.CapabilitySpaceBytes))
    return problems


def leaked_bytes(evidence: Record) -> int:
    """The charge the created domains' live spaces still hold."""
    return (evidence.AddressSpacesLive * evidence.AddressSpaceBytes
            + evidence.CapabilitySpacesLive * evidence.CapabilitySpaceBytes)


def domain_bytes(evidence: Record) -> int:
    """The charge one created domain's spaces hold while it lives."""
    return evidence.AddressSpaceBytes + evidence.CapabilitySpaceBytes


def describe_components(evidence: Record) -> str:
    return ('%d domains created, %d destroyed, %d reclaimed; AddressSpaces %d live, %d reclaimed, %d free; '
            'CapabilitySpaces %d live, %d reclaimed, %d free; %d arenas prepared; %d tagged faults, last tag %d; '
            '%d exits; %d timed-out waits; %d ResourceDomains created, %d destroyed, %d reclaimed, %d live') % (
        evidence.DomainsCreated, evidence.DomainsDestroyed, evidence.DomainsReclaimed, evidence.AddressSpacesLive,
        evidence.AddressSpacesReclaimed, evidence.AddressSpacesFree, evidence.CapabilitySpacesLive,
        evidence.CapabilitySpacesReclaimed, evidence.CapabilitySpacesFree, evidence.ArenasPrepared,
        evidence.TaggedFaults, evidence.LastTag, evidence.DomainExits, evidence.WaitExpiries,
        evidence.ResourceDomainsCreated, evidence.ResourceDomainsDestroyed, evidence.ResourceDomainsReclaimed,
        evidence.ResourceDomainsLive)


COMPONENT_FIELDS = ('Published', 'AddressSpacesCreated', 'AddressSpacesLive', 'CapabilitySpacesCreated',
                    'CapabilitySpacesLive', 'DomainsCreated', 'DomainsDestroyed', 'DomainsReclaimed', 'TaggedFaults',
                    'LastTag', 'DomainExits', 'AddressSpaceBytes', 'CapabilitySpaceBytes', 'AddressSpacesReclaimed',
                    'CapabilitySpacesReclaimed', 'ArenasPrepared', 'AddressSpacesFree', 'CapabilitySpacesFree',
                    'WaitExpiries', 'ResourceDomainsCreated', 'ResourceDomainsDestroyed', 'ResourceDomainsReclaimed',
                    'ResourceDomainsLive')


@dataclasses.dataclass(frozen=True)
class ObjectManagementScenario:
    """What the boot pager creates through ObjectCreate, as the runner defines
    it: with `self_test` (the test firmware, which grants
    the pager object management), the pager creates an Endpoint, loops a
    message through it, destroys it, then creates and destroys a second one,
    and the kernel reclaims both; with `memory_test` it also creates and
    destroys one Frame of `frame_pages` pages and one MemoryObject, whose
    pages then stay dirty in the frame pool of `frame_pool_pages` pages; with
    `mapping_test` it creates another pair, maps it with MemoryMap at
    `window_start` of its own space, unmaps it with MemoryUnmap and destroys
    it, a run of the frame pool (physical [start, end) `frame_pool`). Its
    destroyed pages end dirty, unless the kernel `scrubbed` them, which
    leaves the whole pool free. Either way no object and no charge remain;
    the production firmware creates nothing."""
    self_test: bool
    memory_test: bool = False
    mapping_test: bool = False
    frame_rights: bool = False  # Desktop session test-only pair and two maps
    return_boundary: bool = False  # One Frame, RW/RX MemoryObjects, two maps
    frame_pages: int = 2
    imported: bool = False
    frame_pool_pages: int = 1024
    frame_pool: tuple[int, int] = (0, 0)
    window_start: int = 0
    scrubbed: bool = False
    thread_bytes: int = 0      # One created thread's charge, which the peak covers when nonzero
    component: ComponentScenario = ComponentScenario(built=False)
    leaked_bytes: int = 0      # The charge the component's live spaces still hold (leaked_bytes)
    domain_bytes: int = 0      # The charge of one created domain's spaces (domain_bytes)

    @property
    def unmappings(self) -> int:
        """MemoryUnmap calls: the self-test's mapped pair and the component's."""
        return (1 if (self.self_test and self.mapping_test) else 0) + self.component.unmappings + 2 * self.frame_rights + 2 * self.return_boundary


def object_management_problems(evidence: Record, scenario: ObjectManagementScenario) -> list[str]:
    problems = _published(evidence)
    component = scenario.component
    # The self-test's two, the report Endpoint, each echo's service Endpoint,
    # each account incarnation's own and, with an echo, the root service's
    # reply Endpoint.
    count = (2 + (1 + component.echoes + component.services + component.clients + component.accounts
                  + component.driver_services + min(component.echoes, 1) + 2 * component.block_drivers
                  + component.displays + 2 * component.fs_drivers + 3 * component.filesystems + component.nic_drivers
                  + 3 * component.networks + (5 * component.inputs // 2) + component.ps2
                  + component.watchdog_services
                  if component.built else 0)) \
        if scenario.self_test else 0
    memory = 1 if (scenario.self_test and scenario.memory_test) else 0
    mapped = 1 if (scenario.self_test and scenario.mapping_test) else 0
    pairs = memory + mapped + int(scenario.frame_rights)
    dirty = 0 if scenario.scrubbed else scenario.frame_pages * (memory + mapped) + int(scenario.frame_rights)
    run = scenario.frame_pages * mapped
    pair = evidence.FrameMetadataBytes + evidence.MemoryObjectMetadataBytes
    peak = max(evidence.EndpointMetadataBytes, pair * min(pairs, 1),
               scenario.thread_bytes) if scenario.self_test else 0
    maps, unmaps = mapped + component.mappings + 2 * scenario.frame_rights + 2 * scenario.return_boundary, scenario.unmappings
    # The NIC driver's copies beyond the least traffic add a mapping and an
    # unmapping each.
    beyond = evidence.Mappings - maps
    if (0 <= beyond <= component.copies_beyond) and (evidence.Unmappings - unmaps == beyond):
        maps, unmaps = evidence.Mappings, evidence.Unmappings
    # The last MemoryMap: the component's stack and start block once it was
    # built, otherwise the self-test's pair.
    last = (component.data_start, component.data_pages) if component.built else (scenario.window_start * mapped, run)
    if scenario.imported:
        if evidence.AllocatorCleanPages + evidence.AllocatorDirtyPages != scenario.frame_pool_pages - component.shared_pages:
            problems.append('imported free-page partition does not match managed minus retained Frames')
    _compare(problems, (
        ('the Endpoints created, destroyed and reclaimed',
         (evidence.Created, evidence.Destroyed, evidence.Reclaimed), (count, count, count)),
        ('the Frames and MemoryObjects created and reclaimed',
         (evidence.FramesCreated, evidence.FramesReclaimed, evidence.MemoryObjectsCreated,
          evidence.MemoryObjectsReclaimed),
         (pairs + int(scenario.return_boundary) + component.frames, pairs + int(scenario.return_boundary) + component.frames - component.shared_segments,
          pairs + 2 * int(scenario.return_boundary) + component.memory_objects, pairs + 2 * int(scenario.return_boundary) + component.memory_objects - component.shared_segments)),
        ('the occupied pool slots and the remaining kernel-metadata (the live spaces of created domains), '
         'IPC-queue, committed and resident charge',
         (evidence.OccupiedSlots, evidence.MetadataBytes, evidence.IpcQueueBytes, evidence.CommittedPages,
          evidence.ResidentPages), (2 * component.shared_segments, scenario.leaked_bytes + pair * component.shared_segments,
                                   0, component.shared_pages, component.shared_pages)),
        ("the frame pool's free and dirty pages",
         (evidence.AllocatorCleanPages, evidence.AllocatorDirtyPages),
         (evidence.AllocatorCleanPages, evidence.AllocatorDirtyPages) if scenario.imported else
         (scenario.frame_pool_pages - dirty - component.shared_pages, dirty) if scenario.self_test else (0, 0)),
        ("MemoryMap's and MemoryUnmap's mappings and the last map's virtual start and pages",
         (evidence.Mappings, evidence.Unmappings, evidence.MappedVirtualStart, evidence.MappedPages),
         (maps, unmaps) + last)))
    if component.built:
        # One domain with its arena is live at a time, and what spaces stay
        # live keep their charge; the self-test's objects come first, one at a
        # time.
        floor = max(peak, scenario.domain_bytes, scenario.leaked_bytes)
        if evidence.PeakMetadataBytes < floor:
            problems.append('the peak kernel-metadata charge %#x is below the self-test\'s %#x, a domain\'s spaces\' '
                            '%#x or the live spaces\' %#x' % (evidence.PeakMetadataBytes, peak, scenario.domain_bytes,
                                                              scenario.leaked_bytes))
    else:
        _differs(problems, 'the peak kernel-metadata charge (one Endpoint, a Frame and a MemoryObject, or a thread, '
                 'at a time)', evidence.PeakMetadataBytes, peak)
    start, end = scenario.frame_pool
    physical, pages = evidence.MappedPhysicalStart, last[1]
    if maps and (physical % PAGE or physical < start or physical + pages * PAGE > end):
        problems.append('the mapped run at physical %#x..%#x is not page-aligned within the frame pool %#x..%#x'
                        % (physical, physical + pages * PAGE, start, end))
    if not maps and physical:
        problems.append('a mapping at physical %#x without a MemoryMap' % physical)
    if (evidence.EndpointMetadataBytes == 0 or evidence.FrameMetadataBytes == 0
            or evidence.MemoryObjectMetadataBytes == 0 or evidence.RegisteredObjects == 0):
        problems.append('no Endpoint (%s), Frame (%s) or MemoryObject (%s) charge or registered objects (%s) '
                        'recorded' % (_show(evidence.EndpointMetadataBytes), _show(evidence.FrameMetadataBytes),
                                      _show(evidence.MemoryObjectMetadataBytes), _show(evidence.RegisteredObjects)))
    return problems


def describe_object_management(evidence: Record) -> str:
    return ('%d Endpoints created, %d destroyed, %d reclaimed; %d Frames and %d MemoryObjects reclaimed; '
            'frame pool %d free, %d dirty; peak charge %d bytes; %d registered objects; %d mapping(s) and %d '
            'unmapping(s), the last map %d pages at %#x from physical %#x') % (
        evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.FramesReclaimed,
        evidence.MemoryObjectsReclaimed, evidence.AllocatorCleanPages, evidence.AllocatorDirtyPages,
        evidence.PeakMetadataBytes, evidence.RegisteredObjects, evidence.Mappings, evidence.Unmappings,
        evidence.MappedPages, evidence.MappedVirtualStart, evidence.MappedPhysicalStart)


# Created threads -----------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class ThreadScenario:
    """What the boot pager's created threads do, as the runner defines it
    : with
    `self_test` (the test firmware), the pager creates a thread, which is
    preempted at least once, writes one line and exits with ThreadExit; the
    pager's ObjectDestroy is refused at least once while it runs, then
    reclaims it. It then creates a second thread, which faults at least once
    (a fault behind another may be deferred and repeated) and which the pager
    terminates, then a third, whose invalid instruction reaches the pager as
    an exception and which the pager terminates too,
    so ObjectDestroy returns `terminated_exit_code`, then a fourth, which
    spins until the pager kills it with ThreadTerminate, so ObjectDestroy returns `killed_exit_code`, then a fifth, whose
    timed WaitSetWait expires and which exits with
    `waiting_exit_code` through ThreadExit, and a sixth, which the pager kills
    while it waits, so ObjectDestroy returns `killed_exit_code`
    again. With `component` built,
    each of its incarnations is one more thread: a crashing one ends with an
    exception the pager terminates, a reporting one with ThreadExit, and the
    last, reporting, makes the last exit code the component's. The kernel
    frees the threads' pool and native slots, leaving `slots` of each free and
    no Threads usage. The production firmware has no grant: nothing is
    created, the pool keeps its `slots` free slots and there is no native
    table or charge."""
    self_test: bool
    terminated_exit_code: int = 0
    killed_exit_code: int = 0
    waiting_exit_code: int = 0
    slots: int = 2
    component: ComponentScenario = ComponentScenario(built=False)
    return_boundary: bool = False
    entry_state: int = 0


def thread_problems(evidence: Record, scenario: ThreadScenario) -> list[str]:
    problems = _published(evidence)
    one = 1 if scenario.self_test else 0
    component = scenario.component
    created = (6 * one + component.domains + max(component.smp_workers - 1, 0)
               + int(scenario.return_boundary) + int(scenario.entry_state))
    exits = (2 * one + component.reports + component.clients + component.interpreters + component.accounts
             + component.drivers + component.smp_workers)
    last = (component.last_exit_code or scenario.killed_exit_code) if scenario.self_test else 0
    _compare(problems, (
        ('the threads created, reclaimed by ObjectDestroy and freed',
         (evidence.Created, evidence.Destroyed, evidence.Reclaimed), (created, created, created)),
        ('the ThreadExit calls, reaped native slots and lines',
         (evidence.Exits, evidence.Reaped, evidence.Lines), (exits, created, one)),
        ('the last exit code', evidence.LastExitCode, last),
        ('the free pool and native slots and the Threads usage',
         (evidence.FreeSlots, evidence.FreeNativeSlots, evidence.ThreadsUsage),
         (scenario.slots, scenario.slots if scenario.self_test else 0, 0))))
    _differs(problems, 'the threads ThreadTerminate killed', evidence.Kills,
             2 * one + component.hangs + component.echoes + component.services + component.driver_services
             - component.recoveries + component.block_drivers - component.block_served + component.displays
             + component.fs_drivers + component.filesystems - component.fs_crashes + component.nic_drivers
             + component.networks + component.inputs + component.ps2)
    if scenario.self_test:
        if evidence.RunStops == 0 or evidence.DestroyPolls == 0:
            problems.append('%d user ticks ended the thread\'s run and %d ObjectDestroy calls were refused; the '
                            'demonstration needs at least one of each' % (evidence.RunStops, evidence.DestroyPolls))
        if evidence.SlotMetadataBytes == 0:
            problems.append('no thread charge recorded')
        crashes = component.crashes
        if evidence.Faults < evidence.Deferrals + 2 + crashes + (int(scenario.return_boundary) + int(scenario.entry_state)):
            problems.append('%d faults of created threads with %d deferred; the faulting and the excepting thread '
                            '(and a crashing component) each need one delivered' % (evidence.Faults, evidence.Deferrals))
        if evidence.Exceptions < 1 + crashes + (int(scenario.return_boundary) + int(scenario.entry_state)) or evidence.Exceptions >= evidence.Faults:
            problems.append('%d of %d faults were exceptions; the excepting thread (and a crashing component) needs '
                            'one beside the page fault' % (evidence.Exceptions, evidence.Faults))
    else:
        _compare(problems, (('the run stops, refused polls, thread charge, faults, deferrals and exceptions',
                             (evidence.RunStops, evidence.DestroyPolls, evidence.SlotMetadataBytes, evidence.Faults,
                              evidence.Deferrals, evidence.Exceptions), (0, 0, 0, 0, 0, 0)),))
    return problems


def describe_threads(evidence: Record) -> str:
    return ('%d created, %d reclaimed, %d freed; last exit code %#x; %d run stops, %d refused polls; %d faults, '
            '%d deferred, %d exceptions; %d free slots, %d free native slots, %d Threads in use; %d bytes each') % (
        evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.LastExitCode, evidence.RunStops,
        evidence.DestroyPolls, evidence.Faults, evidence.Deferrals, evidence.Exceptions, evidence.FreeSlots,
        evidence.FreeNativeSlots, evidence.ThreadsUsage, evidence.SlotMetadataBytes)


# Metadata arena --------------------------------------------------------------------------

# The arena lies below every fixed user range (Abi::Amd64FixedUserRangesStart).
ARENA_LIMIT = 0x4000_0000
ARENA_ALIGNMENT = 0x20_0000


def metadata_problems(evidence: Record, *, slab_first_page: int, maximum_pages: int, root_arena_pages: int,
                      registry_capacity: int, frame_slots: int, memory_object_slots: int) -> list[str]:
    """The metadata arena the loader allocated and the kernel accepted
    : 2 MiB-aligned below 1 GiB, at most the cap,
    holding the slabs and some bitmap budget; the shared root's seed within
    its 64-page arena, also with the arena at the cap; the slabs at their
     capacities."""
    problems = _published(evidence)
    start, pages = evidence.ArenaPhysicalStart, evidence.ArenaPages
    if not start or start % ARENA_ALIGNMENT or pages % (ARENA_ALIGNMENT // 4096) or start + pages * 4096 > ARENA_LIMIT:
        problems.append('the arena [%s, +%s pages) is not 2 MiB blocks below 1 GiB' % (_show(start), _show(pages)))
    if pages > maximum_pages:
        problems.append('the arena of %d pages exceeds the cap of %d' % (pages, maximum_pages))
    _differs(problems, 'the slabs\' first arena page', evidence.SlabFirstPage, slab_first_page)
    if not slab_first_page < evidence.BitmapFirstPage <= pages:
        problems.append('the bitmaps begin at arena page %d, outside (%d, %d]'
                        % (evidence.BitmapFirstPage, slab_first_page, pages))
    # The arena's 4 KiB leaves take one table page per 2 MiB block beyond the
    # seed's others (Amd64MetadataArenaTests), so the same seed with the
    # arena at the cap must fit the root's arena too.
    at_cap = evidence.SeedTablePages - pages // (ARENA_ALIGNMENT // 4096) + maximum_pages // (ARENA_ALIGNMENT // 4096)
    if not 0 < evidence.SeedTablePages <= at_cap <= root_arena_pages:
        problems.append('the shared root\'s seed took %d of its %d table pages, %d with the arena at the cap'
                        % (evidence.SeedTablePages, root_arena_pages, at_cap))
    _compare(problems, (
        ('the registry entries', evidence.RegistryCapacity, registry_capacity),
        ('the Frame slots', evidence.FrameSlots, frame_slots),
        ('the MemoryObject slots', evidence.MemoryObjectSlots, memory_object_slots)))
    return problems


def describe_metadata(evidence: Record) -> str:
    return ('arena %#x, %d pages, slabs from page %d, bitmaps from page %d; the shared root\'s seed takes %d '
            'table pages; %d registry entries, %d Frame and %d MemoryObject slots') % (
        evidence.ArenaPhysicalStart, evidence.ArenaPages, evidence.SlabFirstPage, evidence.BitmapFirstPage,
        evidence.SeedTablePages, evidence.RegistryCapacity, evidence.FrameSlots, evidence.MemoryObjectSlots)


# Service IPC ---------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class ServiceScenario:
    """service IPC on AMD64: with `self_test` (the test firmware), the pager
    creates one WaitSet, on which its first waiting thread's timed WaitSetWait
    blocks and expires and its second's blocks until ThreadTerminate aborts
    it, so that thread ends as it resumes, and destroys it, so
    the kernel reclaims it. With `component`'s echo, the root service creates
    another, binds the echo's Endpoint to it, and the echo's waits block and
    the last is aborted too; how often the echo blocks depends on whether it
    first ran before or after the request. Each driver creates one for its
    Interrupt, binds
    it, and blocks at most once per interrupt it waits for; its drain takes
    the WaitSet; the AHCI driver's waits also expire while a command is in
    flight. The optional i8042 self-check creates one more WaitSet and binds
    one line Interrupt. The production firmware creates nothing."""
    self_test: bool
    component: ComponentScenario = ComponentScenario(built=False)
    legacy_lines: int = 1


def service_problems(evidence: Record, scenario: ServiceScenario) -> list[str]:
    problems = _published(evidence)
    one = 1 if scenario.self_test else 0
    component = scenario.component
    echoes = (component.echoes + component.services + component.driver_services - component.recoveries
              + component.block_drivers - component.block_served + component.displays + component.fs_drivers
              + component.filesystems - component.fs_crashes + component.networks + component.inputs + component.ps2)
    # The AHCI and NIC drivers bind their Interrupt to their service WaitSet,
    # the filesystem service its block-reply Endpoint to its own.
    waitsets = component.echoes + component.services + component.clients + component.drivers \
        + 2 * component.driver_services + component.block_drivers + component.displays + component.fs_drivers \
        + component.filesystems + component.nic_drivers + component.networks + component.inputs + component.ps2 \
        + bool(component.smp_workers)
    bound = waitsets + component.block_served + component.fs_served + 2 * component.filesystems + component.nic_drivers \
        + component.networks + (3 * component.inputs // 2) + 2 * component.ps2
    _compare(problems, (
        ('the WaitSets created and reclaimed', (evidence.WaitSetsCreated, evidence.WaitSetsReclaimed),
         (one + waitsets + one * scenario.legacy_lines, one + waitsets + one * scenario.legacy_lines)),
        ('the Endpoints and Interrupts WaitSetBind bound', evidence.Binds, bound + one * scenario.legacy_lines),
        ('the threads that ended as their aborted wait resumed', evidence.AbortedExits, evidence.WaitAborts)))
    # A NIC driver may be killed ready to run, its backstop having ended its
    # wait (component_problems).
    # On several processors a kill may arrive after a reply/backstop has
    # made a service Ready. Its guest kill and exit are checked separately.
    least = one if component.distributed else one + echoes
    if not least <= evidence.WaitAborts <= one + echoes + component.nic_drivers:
        problems.append('ThreadTerminate aborted %d waits, expected %d to %d' % (
            evidence.WaitAborts, least, one + echoes + component.nic_drivers))
    # The pager's first waiting thread's wait expires, besides the AHCI
    # driver's polls of a command in flight.
    polls = BLOCK_POLLS * (component.block_served + component.fs_served) + NIC_POLLS * component.nic_drivers
    # A NIC driver's backstop expires at least once while it waits for its
    # kill.
    expired = one + component.nic_drivers
    if not expired <= evidence.WaitExpiries <= one + polls + 1000 * (component.inputs + component.ps2):
        problems.append('%d created threads\' waits expired, expected %d to %d' % (evidence.WaitExpiries, expired,
                                                                                  one + polls + 1000 * (component.inputs + component.ps2)))
    # Each echo or service blocks at least once, in the wait its kill aborts,
    # and at most once more per request it got: the echo one; the
    # Directory the bind, Echo's rebind and the client's two resolves; each
    # Echo incarnation one round of the client's three echoes. The client
    # blocks at most once per message it awaits: two resolves, six echoes and
    # the go.
    clients = component.clients
    requests = component.echoes + component.behaviors.count('directory') * (1 + 3 * clients) \
        + component.behaviors.count('service') * 3 * clients if component.built else 0
    blocked = 2 * one + echoes
    most = blocked + requests + 9 * clients + DRIVER_INTERRUPTS * component.drivers \
        + RECOVERY_WAITS * component.recoveries + BLOCK_WAITS * component.block_served + polls \
        + DISPLAY_WAITS * component.displays + FS_DRIVER_WAITS * component.fs_drivers \
        + FS_WAITS * (component.filesystems + component.fs_served) + NIC_WAITS * component.nic_drivers \
        + NETWORK_WAITS * component.networks + 2000 * (component.inputs + component.ps2)
    if not blocked <= evidence.WaitsBlocked <= most:
        problems.append('%d created threads\' waits blocked, expected %d to %d' % (evidence.WaitsBlocked, blocked,
                                                                                 most))
    return problems


def describe_services(evidence: Record) -> str:
    return '%d WaitSets created, %d reclaimed, %d binds; %d waits blocked, %d expired, %d aborted (%d ended)' % (
        evidence.WaitSetsCreated, evidence.WaitSetsReclaimed, evidence.Binds, evidence.WaitsBlocked,
        evidence.WaitExpiries, evidence.WaitAborts, evidence.AbortedExits)


# DMA remapping ---------------------------------------------------------------------------

# The interrupts each driver waits for: the one it raises and one at the end
# of each of its five transfers (two copies, the stray write's two and the
# copy after mapping again).
DRIVER_INTERRUPTS = 6
# A recovery's: the crashed instance's raise and its copy's
# first transfer, and the restarted instance's raise and both transfers of
# the copy it serves, besides the write-back the crashed one started; and at
# most the waits its two instances block in.
RECOVERY_INTERRUPTS = 2 + 3
RECOVERY_WAITS = 8
# The AHCI driver's: a completion's for each of the
# token's read, the scratch write, its read back and the two reads of each
# throttle probe (IDENTIFY is polled; the crash's read lands after the
# drain), one probe if the runner throttled the disk at once and at most
# BLOCK_THROTTLE_PROBES; and at most the waits it blocks in.
BLOCK_INTERRUPTS = 5
BLOCK_THROTTLE_PROBES = 16
BLOCK_WAITS = 16 + 4 * BLOCK_THROTTLE_PROBES
# The filesystem demonstration's driver without a disk: its bring-up's waits
# and the probe's.
FS_DRIVER_WAITS = 16
# The filesystem service's and its driver's waits over the demonstration:
# about one a sector transfer (a FAT16 commit is dozens) and one a request.
FS_WAITS = 512
# The completions the filesystem demonstration's driver takes: the probe's
# read at least, at most one a sector transfer of the demonstration.
FS_INTERRUPTS = 1
FS_MOST_INTERRUPTS = 512
# The display broker blocks once per batch of the root service's lines it
# drains: at most once per line and Query.
DISPLAY_WAITS = 64
# With a command in flight the driver's wait ends every 20 ms
# (AhciDriver.cpp CommandPollMicroseconds); the throttled probe's read stays
# in flight about a second at the runner's 512 bytes a second, any other
# read far less: at most 10 s of polls.
BLOCK_POLLS = 500
# The NIC driver's wait ends every 10 ms while its device runs (its
# backstop), and it wakes besides for each interrupt and request: at most
# 80 s of either over the network demonstration.
NIC_POLLS = 8000
NIC_WAITS = 8000
# The messages the NIC delivers: none required (the link may be up before
# the driver unmasks its causes), at most one a frame or a send of the
# demonstration and the link's changes.
NIC_MOST_INTERRUPTS = 64
# The network stack wakes for each of its driver's replies and each request:
# a few per frame of the demonstration, which its bring-up's timed waits
# precede.
NETWORK_WAITS = 512

@dataclasses.dataclass(frozen=True)
class IommuScenario:
    """driver isolation on AMD64: with `present` (the test firmware's
    cases attach QEMU's intel-iommu), the kernel uses the DMAR's one unit,
    which lists each of QEMU's `functions` in a scope, stops every function's
    bus mastering, and enables translation with every requester blocked.
    The driver (`driver`, edu's requester identifier) confines edu in one
    IommuDomain, which its drain reclaims; its stray write to the device
    address it unmapped is the unit's one fault, a write the unit refused at
    `stray_address` (QEMU reports a zero leaf as reason 5, hardware may say 7).
    Without it there is no DMAR (VtdTopologyError NoDmar)."""
    present: bool
    functions: int = 0
    swept: int | None = None
    driver: int | None = None
    stray_address: int = 0x20_0000
    # The IommuDomains the drivers and driver services create, one each.
    domains: int = 1
    # QEMU 10.2 splits the 256-byte rejected edu write into 64 4-byte
    # accesses. Its event encoder leaves Address zero and DomainID absent.
    # Keep raw event counts/addresses; the guest separately proves no write lands.
    amd_vi: bool = False


NO_DMAR = 1


def iommu_backend_problems(backend: Record, scenario: IommuScenario) -> list[str]:
    problems = _published(backend)
    _compare(problems, (
        ('selected DMA backend', backend.Kind, (2 if scenario.amd_vi else 1) if scenario.present else 0),
        ('malformed IVRS records and skipped HID devices', (backend.MalformedIvrs, backend.SkippedHid), (0, 0))))
    if scenario.amd_vi and backend.IvhdType not in (0x10, 0x11, 0x40):
        problems.append('unsupported IVHD image type')
    return problems


def iommu_problems(evidence: Record, scenario: IommuScenario) -> list[str]:
    problems = _published(evidence)
    if not scenario.present:
        _compare(problems, (('why no DMA remapping unit is used', evidence.SelectionError, NO_DMAR),
                            ('whether a unit translates', evidence.Enabled, 0)))
        return problems
    driven = scenario.driver is not None
    _compare(problems, (
        ('why no DMA remapping unit is used', evidence.SelectionError, 0),
        ('the unit\'s configuration and enabling errors', (evidence.ConfigureError, evidence.EnableError), (0, 0)),
        ('whether the unit translates', evidence.Enabled, 1),
        ('the functions the unit serves and those whose bus mastering the boot stopped',
         (evidence.ServedFunctions, evidence.SweptFunctions), (scenario.functions, scenario.functions if scenario.swept is None else scenario.swept)),
        ('the faults recorded, those of no domain, and the overflows',
         (evidence.Faults, evidence.Unattributed, evidence.Overflows), (evidence.Faults if scenario.amd_vi else 1, 0, 0) if driven else (0, 0, 0)),
        ('the IommuDomains created and reclaimed', (evidence.DomainsCreated, evidence.DomainsReclaimed),
         (scenario.domains, scenario.domains) if driven else (0, 0))))
    if driven:
        if scenario.amd_vi:
            if evidence.Faults < 1:
                problems.append('the rejected DMA must produce at least one fault')
            _compare(problems, (('the rejected DMA requester', evidence.LastFaultSource, scenario.driver),))
            # The guest TAP independently verifies the target stayed unchanged.
            # Event count, raw address and direction differ across QEMU versions.
        else:
            _compare(problems, (
                ('the stray write\'s requester, page and direction',
                 (evidence.LastFaultSource, evidence.LastFaultAddress, evidence.LastFaultRead),
                 (scenario.driver, scenario.stray_address, 0)),))
        if evidence.LastFaultReason not in ((2,) if scenario.amd_vi else (5, 7)):
            problems.append('the stray write faulted with reason %d, expected 5 or 7' % evidence.LastFaultReason)
    if evidence.Levels not in (3, 4):
        problems.append('the unit walks %d levels, expected 3 or 4' % evidence.Levels)
    return problems


def iommu_topology_problems(topology: Record, scenario: IommuScenario) -> list[str]:
    """Validate unit facts against q35 and QMP, independently of the summary."""
    problems = _published(topology)
    _compare(problems, (('the DRHD count', topology.Count, 1 if scenario.present else 0),
                        ('skipped DMAR scopes and malformed RMRRs', (topology.IgnoredScopes, topology.MalformedRmrr),
                         (0, 0))))
    if not scenario.present or topology.Count != 1:
        return problems
    unit = topology.Units[0]
    swept = scenario.functions if scenario.swept is None else scenario.swept
    _compare(problems, (
        ('the q35 DRHD register base', unit.RegisterBase, 0xfed80000 if scenario.amd_vi else 0xfed90000),
        ('the unit state, configure and enable errors', (unit.State, unit.ConfigureError, unit.EnableError), (2, 0, 0)),
        ('the firmware domains and identity pages', (unit.ReservedFunctions, unit.ReservedPages), (0, 0)),
        ('the retained and swept QMP requesters', (unit.ServedFunctions, unit.SweptFunctions),
         (scenario.functions, swept)),
        ('disabled and dropped requesters', (unit.DisabledReason, unit.DroppedRequesters), (0, 0)),
        ('unit faults, unattributed faults and overflows', (unit.Faults, unit.Unattributed, unit.Overflows),
         (unit.Faults if scenario.amd_vi else 1, 0, 0) if scenario.driver is not None else (0, 0, 0)),
    ))
    if scenario.amd_vi:
        if scenario.driver is not None and unit.Faults < 1:
            problems.append('the AMD-Vi unit recorded no rejected DMA')
        if (unit.Status & 0x101d) != 0x1005:
            problems.append('AMD-Vi requires translation, command and event logging enabled, event interrupts off')
        if unit.Levels != 4 or unit.Version != 1:
            problems.append('AMD-Vi requires the v1 four-level walk')
        return problems
    if (unit.Status & ((1 << 31) | (1 << 26) | (1 << 25))) != (1 << 31):
        problems.append('the unit must have TES set, QIES and IRES clear')
    widths = (unit.Capability >> 8) & 31
    if unit.Levels not in (3, 4) or not widths & (1 << (unit.Levels - 2)):
        problems.append('the configured walk is not supported by SAGAW')
    if unit.Capability & (1 << 7):
        problems.append('the unit uses unsupported caching mode')
    if ((unit.Capability >> 16) & 63) + 1 < 12:
        problems.append('MGAW cannot address a page')
    if unit.Version >> 4 != 1:
        problems.append('the q35 unit has an unexpected major version')
    return problems


# Device interrupts --------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class InterruptScenario:
    """driver isolation on AMD64: each of `drivers` creates one
    Interrupt for edu's MSI, which its drain destroys and the pool reclaims;
    every interrupt it waits for arrives on the Interrupt's vector. A message
    can reach an unbound vector at most once, if one were still in flight
    when its Interrupt went. Without a driver nothing is created or
    delivered. Each of `driver_services` creates one too. In each of
    `recoveries` the crashed instance's write-back transfer ends either after
    its drain turned the messages off, delivering nothing, or, where the
    emulator's clock outran the drain, before it, on its armed Interrupt
."""
    drivers: int
    driver_services: int = 0
    recoveries: int = 0
    block_served: int = 0
    fs_served: int = 0
    nic_drivers: int = 0
    legacy_lines: int = 0
    ps2: int = 0


def interrupt_problems(evidence: Record, scenario: InterruptScenario) -> list[str]:
    problems = _published(evidence)
    created = scenario.drivers + scenario.driver_services + scenario.block_served + scenario.fs_served \
        + scenario.nic_drivers + scenario.legacy_lines + 2 * scenario.ps2
    _compare(problems, (
        ('the Interrupts created, destroyed, reclaimed and live',
         (evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.Live), (created, created, created, 0)),))
    delivered = (DRIVER_INTERRUPTS * scenario.drivers + RECOVERY_INTERRUPTS * scenario.recoveries
                 + BLOCK_INTERRUPTS * scenario.block_served + FS_INTERRUPTS * scenario.fs_served)
    delivered += 25 * scenario.ps2
    most = delivered + 200 * scenario.ps2 + scenario.recoveries + 2 * (BLOCK_THROTTLE_PROBES - 1) * scenario.block_served \
        + FS_MOST_INTERRUPTS * scenario.fs_served + NIC_MOST_INTERRUPTS * scenario.nic_drivers + scenario.legacy_lines
    if not delivered <= evidence.Delivered <= most:
        problems.append('%d messages delivered on bound vectors, expected %d to %d' % (
            evidence.Delivered, delivered, most))
    if evidence.Spurious > (1 if created else 0):
        problems.append('%d messages reached an unbound vector, expected at most %d' % (evidence.Spurious,
                                                                                       1 if created else 0))
    return problems


@dataclasses.dataclass(frozen=True)
class DeviceControlScenario:
    """driver isolation on AMD64: the Device Manager assigns the
    device once to each of `assignments` driver incarnations, whose drains
    each release it into quarantine, and it lifts each quarantine once it
    found the device quiet; the driver task's first bus-master request, with
    no IommuDomain attached, is refused. Without a driver nothing happens.
    The drain of a driver whose IommuDomain was still attached parks its
    `parked_pins` or more (up to `parked_pins_most`) Frame pins with the quarantined function, and the Device
    Manager's confirmation releases every one."""
    assignments: int
    parked_pins: int = 0
    parked_pins_most: int | None = None


def device_control_problems(evidence: Record, scenario: DeviceControlScenario) -> list[str]:
    problems = _published(evidence)
    count = scenario.assignments
    _compare(problems, (
        ('the assignments, releases, quarantines and confirmations',
         (evidence.Assignments, evidence.Releases, evidence.Quarantines, evidence.Confirmations),
         (count, count, count, count)),))
    problems += DmaLifetime.pin_problems(evidence, scenario.parked_pins,
        scenario.parked_pins if scenario.parked_pins_most is None else scenario.parked_pins_most)
    if (evidence.BusMasterRefusals == 0) != (count == 0):
        problems.append('%d bus-master requests refused, expected %s' % (
            evidence.BusMasterRefusals, 'some' if count else 'none'))
    return problems


# Block I/O -----------------------------------------------------------------------------------

# The released parked pins the block evidence keeps (Amd64BlockEvidence.Released).
BLOCK_RELEASES_KEPT = 2


@dataclasses.dataclass(frozen=True)
class BlockScenario:
    """AHCI block I/O on AMD64: the evidence counts every parked pin a
    confirmation released, as DeviceControl does, and keeps the last two, each
    a Frame of whole pages; where the AHCI driver crashed with a read in
    flight into a Frame its client destroyed, one of them is that Frame, and
    the scrub that zeroed its page after the release found `token` there: the
    late write landed in the pinned Frame, not in reused memory."""
    token: bytes | None = None


def block_problems(record: Record, scenario: BlockScenario) -> list[str]:
    evidence = record.Block
    problems = _published(evidence)
    _compare(problems, (('the released pins', evidence.Releases, record.DeviceControl.PinsReleased),))
    kept = [evidence.Released[index] for index in range(min(evidence.Releases, BLOCK_RELEASES_KEPT))]
    for released in kept:
        if not released.Frame or not released.Pages or not released.PhysicalStart or released.PhysicalStart % 4096:
            problems.append('a released pin of Frame %#x at %#x, %d pages' % (
                released.Frame, released.PhysicalStart, released.Pages))
    if scenario.token is not None:
        samples = [binary.pack('<4Q', *released.Sample) for released in kept if released.Sampled]
        if scenario.token not in samples:
            problems.append('no released Frame held the token when it was scrubbed: %r' % samples)
    return problems


def describe_block(evidence: Record) -> str:
    kept = [evidence.Released[index] for index in range(min(evidence.Releases, BLOCK_RELEASES_KEPT))]
    return '%d parked pins released; last %s' % (evidence.Releases, ', '.join(
        'Frame %#x at %#x%s' % (released.Frame, released.PhysicalStart,
                                ' (scrubbed holding %r)' % binary.pack('<4Q', *released.Sample)
                                if released.Sampled else '') for released in kept) or 'none')


# Collections ---------------------------------------------------------------------------------

def allocator_problems(record: Record, managed_pages: int, runtime_root: int, *, creation_probe: bool) -> list[str]:
    """physical memory management: admitted-memory accounting and creation in a runtime root."""
    evidence = record.Allocator
    problems = _published(evidence)
    _compare(problems, (
        ('managed pages', evidence.ManagedPages, managed_pages),
        ('allocatable pages', evidence.AllocatablePages, managed_pages - evidence.RetiredPages),
        ('allocator page partition', evidence.CleanPages + evidence.DirtyPages + evidence.ClaimedPages
         + evidence.RetiredPages + evidence.AllocatedPages, managed_pages),
        ('remaining Zeroing slots', evidence.ZeroingSlots, 0),))
    if creation_probe:
        if not evidence.CreationZeroedPages or not evidence.PeakZeroingSlots:
            problems.append('no creation-time zero or Zeroing slot observed')
        _compare(problems, (('creation zero runtime root', evidence.CreationZeroRoot, runtime_root),))
    return problems

# The longest run the frame pool hands a scrub (FramePool::MaximumRunPages).
MAXIMUM_RUN_PAGES = 128


def collection_problems(record: Record) -> list[str]:
    """What the object manager's collections did: every slot a pool reclaimed, as
    the other sub-records count them, was reclaimed by a collection, and a
    scrubbed run is one to MAXIMUM_RUN_PAGES pages."""
    evidence = record.Collection
    problems = _published(evidence)
    management, components = record.ObjectManagement, record.Components
    reclaimed = (management.Reclaimed + management.FramesReclaimed + management.MemoryObjectsReclaimed
                 + record.Threads.Reclaimed + components.DomainsReclaimed + components.AddressSpacesReclaimed
                 + components.CapabilitySpacesReclaimed + components.ResourceDomainsReclaimed
                 + record.Services.WaitSetsReclaimed + record.Interrupts.Reclaimed + record.Iommu.DomainsReclaimed
                 + record.ApplicationLaunch.Reclaimed)
    _compare(problems, (('the slots the collections reclaimed', evidence.Reclaimed, reclaimed),))
    if not evidence.ScrubRuns <= evidence.ScrubbedPages <= MAXIMUM_RUN_PAGES * evidence.ScrubRuns:
        problems.append('%d pages scrubbed in %d runs' % (evidence.ScrubbedPages, evidence.ScrubRuns))
    if evidence.Reclaimed and not evidence.Collections:
        problems.append('%d slots reclaimed without a collection' % evidence.Reclaimed)
    return problems


def describe_collection(evidence: Record) -> str:
    return '%d collections reclaimed %d slots; %d runs scrubbed, %d pages' % (
        evidence.Collections, evidence.Reclaimed, evidence.ScrubRuns, evidence.ScrubbedPages)


def describe_device_control(evidence: Record) -> str:
    return ('%d assignments, %d releases, %d bus-master refusals, %d quarantines, %d confirmed quiet; '
            '%d Frame pins parked, %d released') % (
        evidence.Assignments, evidence.Releases, evidence.BusMasterRefusals, evidence.Quarantines,
        evidence.Confirmations, evidence.PinsParked, evidence.PinsReleased)


def describe_interrupts(evidence: Record) -> str:
    return '%d Interrupts created, %d destroyed, %d reclaimed, %d live; %d messages delivered, %d spurious' % (
        evidence.Created, evidence.Destroyed, evidence.Reclaimed, evidence.Live, evidence.Delivered,
        evidence.Spurious)


def describe_iommu(evidence: Record) -> str:
    if not evidence.Enabled:
        return 'no DMA remapping (selection %d)' % evidence.SelectionError
    return ('unit at %#x: %d levels, serves %d functions, %d IommuDomains (%d reclaimed), %d faults (last %#x '
            'reason %d at %#x); VER %#x CAP %#x ECAP %#x GSTS %#x at handoff') % (
        evidence.RegisterBase, evidence.Levels, evidence.ServedFunctions, evidence.DomainsCreated,
        evidence.DomainsReclaimed, evidence.Faults, evidence.LastFaultSource, evidence.LastFaultReason,
        evidence.LastFaultAddress, evidence.Version, evidence.Capability, evidence.ExtendedCapability,
        evidence.HandoffStatus)


EDU = (0x1234, 0x11E8)     # QEMU's edu: vendor and device


def driver_requester(query_pci: list[dict]) -> int | None:
    """edu's requester identifier (bus << 8 | slot << 3 | function), if QEMU has it."""
    for function in qemu_functions(query_pci):
        if (function.vendor, function.device) == EDU:
            return function.bus << 8 | function.slot << 3 | function.function
    return None


# Native preemption ---------------------------------------------------------------------

def native_preemption_problems(evidence: Record, constants: HeaderConstants) -> list[str]:
    """One capture of the loop's frame at the timer vector after its
    deadline, the stopped run readmitted once as a later run (a readmission
    rolled back by a stalled processor consumes a run) with its budget
    exhausted, one resume and one exit; the sixteen registers the
    loop stored after resuming equal the captured frame, the thirteen
    sentinels are the loop's constants, and RDI, RSI and RSP are the flag,
    the buffer and the worker stack the loop runs with."""
    problems = _published(evidence)
    frame = evidence.Frame
    _compare(problems, (
        ('the capture, resume and exit counts', (evidence.CaptureCount, evidence.ResumeCount, evidence.ExitCount),
         (1, 1, 1)),
        ('the frame vector', frame.Vector, constants.timer_vector),
        ('the frame error code', frame.Error, 0),
        ('the frame selectors (CS, SS)', (frame.Cs, frame.Ss),
         (constants.kernel_code_selector, constants.kernel_data_selector)),
        ('the external frame token', evidence.ExternalFrameToken, 1),
        ('the external frame resume generation', evidence.ExternalFrameResumeGeneration, 1),
        ('the budget left when the run stopped', evidence.RemainingBudgetTicks, 0),
        ('the idle continuation generation', evidence.IdleContinuationGeneration, 1),
        ('the registers after resuming', list(evidence.Registers),
         [frame[name] for name in PREEMPTION_REGISTERS]),
        ('the sentinel registers', list(evidence.Registers[:len(constants.preemption_sentinels)]),
         list(constants.preemption_sentinels)),
        ('the loop arguments (RDI, RSI)', (frame.Rdi, frame.Rsi),
         (evidence.ResumeFlagAddress, evidence.RegisterBufferAddress))))
    if evidence.AdmissionRunSequence <= evidence.StoppedRunSequence:
        problems.append('the admission run %d does not follow the stopped run %d' % (
            evidence.AdmissionRunSequence, evidence.StoppedRunSequence))
    if not frame.Rflags & RFLAGS_IF:
        problems.append('the captured RFLAGS %#x have IF clear' % frame.Rflags)
    if not evidence.LoopStart <= frame.Rip < evidence.LoopEnd:
        problems.append('the captured RIP %#x is outside the loop %#x..%#x' % (frame.Rip, evidence.LoopStart,
                                                                               evidence.LoopEnd))
    if not evidence.WorkerStackStart < frame.Rsp < evidence.WorkerStackEnd:
        problems.append('the captured RSP %#x is outside the worker stack %#x..%#x' % (
            frame.Rsp, evidence.WorkerStackStart, evidence.WorkerStackEnd))
    if evidence.ProgrammedDeadlineTicks == 0 or evidence.ObservedTicks < evidence.ProgrammedDeadlineTicks:
        problems.append('the capture at %#x precedes the deadline %#x' % (evidence.ObservedTicks,
                                                                          evidence.ProgrammedDeadlineTicks))
    for what, value in (('the stopped run', evidence.StoppedRunSequence),
                        ('the replenishment', evidence.ReplenishmentTicks)):
        if value == 0:
            problems.append('%s is zero' % what)
    return problems


def unreached_problems(evidence: Record) -> list[str]:
    """A build without the preemption probe records no preemption proof."""
    if evidence.NativePreemption.Published:
        return ['NativePreemption is published, but this build does not run the proof']
    return []


# Summaries of passing records (the detail a passing check keeps) ------------------------

def describe_runtime(runtime: Record) -> str:
    return 'pool %#x, shared root %#x, user root %#x, local APIC %#x, %d processors' % (
        runtime.PoolPhysicalStart, runtime.SharedRootPhysicalAddress, runtime.UserRootPhysicalAddress,
        runtime.LocalApicPhysicalAddress, runtime.ProcessorCount)


def describe_pci(pci: Record) -> str:
    return ('%d region(s), %d buses, %d functions published; first %04x:%02x:%02x.%x id %#010x; '
            'AHCI %04x:%02x:%02x.%x id %#010x class %02x:%02x:%02x' % (
                pci.RegionCount, pci.BusCount, pci.PublishedFunctionCount, pci.FirstSegment, pci.FirstBus,
                pci.FirstDevice, pci.FirstFunction, pci.FirstVendorDevice, pci.AhciSegment, pci.AhciBus,
                pci.AhciDevice, pci.AhciFunction, pci.AhciVendorDevice, pci.AhciClassCode, pci.AhciSubclass,
                pci.AhciProgrammingInterface))


def describe_ahci(ahci: Record, outcomes_: dict[str, int]) -> str:
    names = {value: name for name, value in outcomes_.items()}
    text = 'outcome %s' % names.get(ahci.Outcome, '%#x' % ahci.Outcome)
    if ahci.Outcome == outcomes_['Read']:
        text += ', LBA %d x %d sector(s), %d bytes into %#x' % (
            ahci.ReadStartingLba, ahci.ReadSectorCount, ahci.ReadTransferSizeBytes, ahci.ReadBufferPhysicalAddress)
    return text


def describe_application_processors(evidence: Record) -> str:
    served = [slot for slot in evidence.Processors if slot.Published]
    if not served:
        return 'no application processor (serving mask %#x)' % evidence.ServingProcessorMask
    return '; '.join('CPU %d: %d interrupt(s), %d delivery, claim %d, runs %d/%d, LVT %#x' % (
        slot.Processor, slot.TimerInterrupts, slot.TimerDeliveries, slot.Timer.NotificationClaimSequence,
        slot.Timer.InitialRuns, slot.ServiceRuns, slot.Timer.DisarmedTimerLvt) for slot in served)


def describe_shootdown(evidence: Record) -> str:
    return 'page %#x -> %#x holds %#x, sequence %d; resident %#x, queued %#x, acknowledged %#x' % (
        evidence.VirtualAddress, evidence.PhysicalAddress, evidence.Value, evidence.ShootdownSequence,
        evidence.ResidentProcessorMask, evidence.QueuedProcessorMask, evidence.AcknowledgedProcessorMask)


def describe_wait(evidence: Record) -> str:
    return 'tag %#x: %d call(s), %d suspended, %d resumed' % (
        evidence.RegisteredTag, evidence.WaitCalls, evidence.SuspendedWaits, evidence.ResumedWaits)


def describe_user_preemption(evidence: Record) -> str:
    return '%d user ticks (%d TimerQueue); runs ended: client %d, pager %d, spinner %d' % (
        evidence.UserTicks, evidence.TimerQueueTicks, *evidence.RunStops)


def describe_native_preemption(evidence: Record) -> str:
    return 'captured %d at %#x, resumed %d, exited %d; stopped run %d, readmitted as run %d' % (
        evidence.CaptureCount, evidence.Frame.Rip, evidence.ResumeCount, evidence.ExitCount,
        evidence.StoppedRunSequence, evidence.AdmissionRunSequence)
