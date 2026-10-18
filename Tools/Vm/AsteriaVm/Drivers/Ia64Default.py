# SPDX-License-Identifier: Apache-2.0
"""IA-64 test firmware: Ia64.Smp1-4, Ia64.Ahci, Ia64.Relocated.

The kernel boots and launches the initial user image, the guest test program
(GuestTests.elf), which reports TAP through its DiagnosticChannel and then
enters the idle loop at PL3. The driver waits for the kernel's last stage
event (`ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH`), for the complete TAP
stream (`AsteriaVm.Guest`), observes the pager's self-tests and spinner,
then waits for the stable idle loop (three consecutive samples), stops the
guest, captures its state and the kernel's evidence record (`AsteriaVm.Evidence`), and judges them:

- guest: the TAP stream and each of its test points (everything the
  system-call ABI shows: BSS, Query, IPC, DebugWrite, the global pointer and
  the stacks);
- lifecycle: every stage of the default boot reported once and in order,
  none of the diagnostic probes' (`AsteriaVm.Stages`);
- host: idle loop reached, stopped IP/PSR, IVA and the live IVT, no pending
  exception, relocation (relocated case);
- evidence (`Observers.Ia64Evidence`): the processor count and PCI facts
  against QEMU, the AHCI outcome (disk case: the IDENTIFY data and the READ
  DMA EXT buffer against the runner's fixture), every AP's runtime scenario,
  the shared-root shootdown, and no probe scenario.

It also holds the IA-64 case plumbing the production, synthetic-probe and
preemption drivers reuse.
"""
from __future__ import annotations

import dataclasses
import pathlib

from AsteriaVm import DiskRecipes, Display, Evidence, Guest, Ia64Layout, Lan, Machine, Stages
from AsteriaVm.Executor import CaseFailure
from AsteriaVm import Ps2
from AsteriaVm.Ia64Layout import HeaderConstants
from AsteriaVm.Observers import (UserProcessors, ProcessorClocks, ApplicationLaunch, MemoryImport, DiskImages, SharedImages, Ia64Capture, Ia64Cpu, Ia64Evidence, Ia64Faults, Ia64Image,
                                 Ia64Ivt, Ia64State)
from AsteriaVm.Qmp import QmpError, wait_until
from AsteriaVm.Observers import DmaLifetime

BOOT_TIMEOUT = 60.0         # IA-64 boot phase
# A stage wait renews its budget while the kernel keeps reporting: a slow or
# loaded host stretches the boot, not a correct kernel's silences (the longest,
# the firmware before the kernel's first line, takes seconds), and a kernel that
# stops reporting still fails one budget after its last line. The renewals are
# bounded, so a kernel that keeps reporting without reaching the stage fails too.
GATE_RENEWALS = 4
STOP_ATTEMPTS = 20          # Stops until CPU 0 is caught in the user loop rather than in a timer tick
RESUME_SECONDS = 0.2        # Maximum wait for a running loop sample between stops
LOOP_POLL_SECONDS = 0.01    # Three samples fit inside a 50 ms user slice
USER_TICKS = 2              # Time slices the initial image's idle loop runs out before the stop
# The root service's incarnations on IA-64: the three of domain teardown, then one that creates in
# its own ResourceDomain, then a driver of edu, which every test-firmware case attaches, then the
# echo service and the driver's recovery, a service that crashes with a factorial running and its
# restart then the filesystem demonstration's AHCI driver and services, the network demonstration's
# NIC driver and stack where the case attaches the e1000e and the block demonstration's AHCI driver
# (DMA isolation, as AMD64's). They are listed in the order of their tags: the echo's is taken
# before the recovery runs, though the echo runs after it. The broker starts first and is listed
# last because it is torn down last. Directory, Echo, Client and restarted Echo then run, followed
# by the loader interpreter.
def component_behaviors(recipe: str = '', ps2: bool = False, network: bool = False) -> tuple[str,...]:
    """The incarnations of a case with the disk `recipe` ('' for none), with
    the network demonstration's NIC driver and stack where it has the NIC,
    and with the PS/2 demonstration's second broker, client and driver where
    the i8042 is published."""
    fs = DiskRecipes.FS_DEMOS[recipe]
    filesystems = ('filesystem-crash',) * fs.crashes + ('filesystem',) * (fs.services - fs.crashes)
    return (('crash', 'hang', 'report', 'account', 'driver', 'echo', 'driver-crash', 'driver-service', 'fs-driver')
            + filesystems + (('nic-driver', 'network') if network else ())
            + ('block-driver', 'directory', 'echo', 'client', 'echo', 'interpreter', 'input',
               'input-client') + (('input', 'input-client', 'ps2') if ps2 else ()) + ('display',))


# The disk's read rate the root service asks for before the read its AHCI
# driver crashes on: a sector in four seconds, a
# quarter of AMD64's rate, so the read is still running when the Device
# Manager resets the controller after the slower IA-64 emulation tore the
# driver down, and the probe's read the crash's waits behind still ends
# within the root service's ten-second watchdog.
DISK_THROTTLE = {'device': Machine.DISK_DRIVE, 'bps': 0, 'bps_rd': 128, 'bps_wr': 0, 'iops': 0, 'iops_rd': 0,
                 'iops_wr': 0}
USER_TICK_SECONDS = 30.0    # For them (a slice is 50 ms of ITC time; the bound covers a loaded host)
# Includes the storage demonstrations (a disk case's formats, commits and
# crashes, and the block driver's throttled read) and the final input
# broker/probe incarnations and their scripted IPC exchanges, after all
# previous scenarios have torn down.
SELF_TEST_SECONDS = 420.0
# The network demonstration's more, where the case has the NIC: the LAN
# peer's script, whose retries are bounded at 30 + 30 + 60 s (Lan.py), and
# the slower IA-64 bring-up and teardown of the driver and the stack.
NETWORK_SECONDS = 180.0
SELF_TEST_THREADS = 18      # Six lifecycle probes, eleven fault probes and the spinner
IVT_DUMP = 'InterruptVectorTable'
AHCI_READ_BUFFER_DUMP = 'AhciReadBuffer'
# A check result: (name, problems) or (name, problems, note), where the note is
# the detail recorded when the check passes (for example a value derived from the run).
Check = tuple[str, list[str]] | tuple[str, list[str], str]

# Stage events (BootApplication.cpp): the boot path every IA-64 image takes,
# the AHCI transfer of a disk case, and each image's tail. A stage of another
# configuration must not appear.
BOOT_STAGES = (
    'SYSTEM-TABLE', 'CONSOLE', 'BOOT-SERVICES', 'RUNTIME-SERVICES', 'SYSTEM-TABLE-CRC', 'LOADED-IMAGE',
    'IA64-IMAGE-LAYOUT', 'KERNEL-ELF', 'MEMORY-MAP', 'SAL-SYSTEM-TABLE', 'ACPI', 'GRAPHICS', 'IA64-BOOTSTRAP-RESERVATION', 'BOOT', 'EXIT-BOOT-SERVICES',
    'BOOT-INFO', 'SAL-ENTRYPOINT-BINDING', 'PAL-CAPABILITIES', 'PAGE-ALLOCATOR', 'INITIAL-TRANSLATION-PLAN',
    'IA64-TRANSLATION-REGISTER-BUDGET',
    'INITIAL-EXECUTION-CONTEXT', 'INITIAL-IVT-INSTALL', 'AP-RENDEZVOUS', 'INITIAL-TRANSLATION-INSTALL',
    'BSP-RUNTIME-MEMORY', 'INITIAL-USER-IMAGE-OBJECTS', 'INITIAL-USER-IMAGE-MAPPINGS',
    'INITIAL-TRANSLATION-ACTIVATION', 'PCI-ECAM', 'PCI-DISCOVERY')
DISK_STAGES = ('AHCI-IDENTIFY', 'AHCI-READ-DMA-EXT')
LAUNCH_STAGE = 'INITIAL-USER-IMAGE-LAUNCH'
DEFAULT_STAGES = ('INITIAL-USER-IMAGE-LAUNCH-PREPARED', 'AP-RUNTIME', 'IA64-SHARED-ROOT-SHOOTDOWN', LAUNCH_STAGE)
PROBE_STAGES = ('IA64-PRIVATE-USER-ROOT-PREPARED', 'AP-RUNTIME', 'IA64-SHARED-ROOT-SHOOTDOWN',
                'IA64-PRIVATE-USER-ROOT-WAIT-IDLE', 'IA64-PRIVATE-USER-ROOT-RESUME')
PREEMPTION_STAGE = 'NATIVE-PREEMPTION-RECORDED'
PREEMPTION_STAGES = ('IA64-PRIVATE-USER-ROOT-PREPARED', 'AP-RUNTIME', 'IA64-SHARED-ROOT-SHOOTDOWN',
                     PREEMPTION_STAGE)
_ALL_TAIL_STAGES = frozenset(DEFAULT_STAGES + PROBE_STAGES + PREEMPTION_STAGES)


def stage_expectation(disk: bool, tail: tuple[str,...]) -> tuple[tuple[str,...], tuple[str,...]]:
    """(stages expected in order, stages that must not appear) of one image."""
    expected = BOOT_STAGES + (DISK_STAGES if disk else ()) + tail
    absent = tuple(sorted((_ALL_TAIL_STAGES - set(tail)) | (set() if disk else set(DISK_STAGES))))
    return expected, absent


@dataclasses.dataclass(frozen=True)
class Expectation:
    """Everything the checks compare against, from the case, the preset tree,
    the build's C++ constants and the runner's own disk fixture."""
    cpus: int
    disk: Ia64Evidence.DiskExpectation | None
    relocated: bool
    preferred_base: int
    facts: Ia64Image.ImageFacts
    constants: HeaderConstants
    schema: Evidence.Schema
    outcomes: dict[str, int]
    iommu: bool
    runtime_memory_page: int        # The run-time memory reservation's page (Ia64Layout.runtime_memory_page)
    images: tuple[tuple[str, tuple[int, int]],...]
    image_pages: tuple[tuple[str, int],...]
    ps2: bool = False
    # The case's disk recipe ('' for none), which decides the storage
    # demonstrations the root service runs (DiskRecipes.FS_DEMOS).
    recipe: str = ''
    vtd: Ia64Evidence.VtdExpectation | None = None   # VT-d instead of virtio-iommu (implies `iommu`)
    # No IOMMU, and the AHCI controller (and the e1000e, where attached)
    # trusted by BOOT.CFG:
    # their drivers' IommuDomains are the identity backend's.
    trusted: bool = False
    # The e1000e attached, and with it the network demonstration and the
    # runner as its LAN peer.
    network: bool = False

    def to_json(self) -> dict:
        disk = None if self.disk is None else {**dataclasses.asdict(self.disk), 'sector': self.disk.sector.hex()}
        result = {'iommu': self.iommu, 'cpus': self.cpus, 'disk': disk, 'relocated': self.relocated,
                'preferred_base': self.preferred_base, 'facts': self.facts.to_json(),
                'constants': dataclasses.asdict(self.constants)}
        result['images'] = self.images
        result['image_pages'] = self.image_pages
        if self.ps2:
            result['ps2'] = True
        result['recipe'] = self.recipe
        if self.vtd is not None:
            result['vtd'] = self.vtd.to_json()
        if self.trusted:
            result['trusted'] = True
        if self.network:
            result['network'] = True
        return result

    @classmethod
    def from_json(cls, data: dict, ivt_image: bytes, root: pathlib.Path) -> 'Expectation':
        """The inverse of `to_json` (a recorded run's expectation, independent of
        the current tree except for the evidence layout, which comes from `root`)."""
        constants = {name: tuple(value) if isinstance(value, list) else value
                     for name, value in data['constants'].items()}
        disk = None
        if data['disk'] is not None:
            disk = Ia64Evidence.DiskExpectation(**{**data['disk'], 'sector': bytes.fromhex(data['disk']['sector'])})
        return cls(data['cpus'], disk, data['relocated'], data['preferred_base'],
                   Ia64Image.ImageFacts.from_json(data['facts'], ivt_image), HeaderConstants(**constants),
                   Ia64Evidence.schema(root), Ia64Evidence.outcomes(root), data['iommu'],
                   Ia64Layout.runtime_memory_page(root),
                   tuple((name, tuple(counts)) for name, counts in data['images']),
                   tuple((name, pages) for name, pages in data['image_pages']), ps2=data.get('ps2', False),
                   recipe=data['recipe'], vtd=Ia64Evidence.VtdExpectation.from_json(data.get('vtd')),
                   trusted=data.get('trusted', False), network=data.get('network', False))


def expectation(case, target, facts: Ia64Image.ImageFacts, fixture: pathlib.Path | None = None) -> Expectation:
    root = pathlib.Path(target.root)
    disk = Ia64Evidence.disk_expectation(fixture) if case.disk else None
    return Expectation(case.cpus, disk, case.relocated, int(target.option('image_base'), 16), facts,
                       Ia64Layout.headers(root), Ia64Evidence.schema(root), Ia64Evidence.outcomes(root),
                       case.iommu != 'None', Ia64Layout.runtime_memory_page(root),
                       tuple((name, SharedImages.layout(pathlib.Path(target.artifact('firmware:%s_elf' % name)))[0])
                             for name in ('component', 'directory', 'client', 'interpreter', 'display')),
                       tuple((name, SharedImages.layout(pathlib.Path(target.artifact('firmware:%s_elf' % name)))[1])
                             for name in ('component', 'directory', 'client', 'interpreter', 'display')),
                       ps2=case.i8042, recipe=case.disk, vtd=vtd_expectation(case),
                       trusted=bool(getattr(case, 'trust', ())) and case.iommu == 'None',
                       network=case.guest and 'e1000e' in getattr(case, 'assignable', ()))


def component_lines(expected: Expectation) -> list[str]:
    """The root service's lines this case expects (Guest.ia64_component_lines),
    with the PS/2 demonstration's where the i8042 is published."""
    lines = Guest.ia64_component_lines(expected.recipe, expected.iommu, expected.network)
    if expected.ps2:
        lines[-1:] = [Ps2.READY, 'asteria', Ps2.DONE]
    return lines


def vtd_expectation(case) -> Ia64Evidence.VtdExpectation | None:
    """The VT-d hub a case boots behind, from its machine properties."""
    if case.iommu != 'VtD':
        return None
    options = case.machine_options.split(',')
    high = 'vtd-high-mmio=on' in options
    return Ia64Evidence.VtdExpectation(coherent='vtd-coherent=on' in options,
                                       bases=(0x10000FED91000, 0x10000FED90000) if high else (0xFED91000, 0xFED90000))


# Waiting --------------------------------------------------------------------

def wait_for_self_tests(context, expected: 'Expectation', peer: Lan.LanPeer | None = None) -> None:
    """Waits until the pager's thread self-tests finished, every thread it
    creates reclaimed and the spinning one killed (`Threads` evidence, read
    from guest memory), and its root service wrote the line of each
    component incarnation (a crash, a hang its watchdog ends, a report). They
    take a varying number of time slices (a fault deferred behind the
    client's costs a yield and a second delivery; the component spins and
    hangs for seconds of ITC time), so the initial image's slices are
    counted only afterwards. The LAN `peer` starts its script once the
    root service listens."""
    address, _ = Evidence.locate(context.console.lines)
    spinner_baseline = None
    spinner_stops = 0
    injection = Ps2.Injection(context)
    throttled: list[bool] = []

    def finished() -> bool:
        nonlocal spinner_baseline, spinner_stops
        injection.poll()
        if context.exited() or context.console.failures():
            return True
        if address is None:
            return False
        try:
            threads = expected.schema.decode(expected.schema.read(context.qmp, address)).Threads
        except (QmpError, Evidence.EvidenceError):
            return False
        lines = Guest.channel_lines(context.console.lines)
        if peer is not None and Lan.LISTENING_LINE in lines:
            peer.start_script()  # Idempotent
        if Ia64Faults.COUNT_START in lines and Ia64Faults.COUNT_DONE not in lines:
            if spinner_baseline is None:
                spinner_baseline = threads.RunStops
            spinner_stops = max(spinner_stops, threads.RunStops - spinner_baseline)
        # The block demonstration asks for the disk's throttle before its
        # driver's crash.
        if not throttled and Guest.BLOCK_THROTTLE_LINE in lines:
            context.qmp.execute('block_set_io_throttle', DISK_THROTTLE)
            throttled.append(True)
        if Guest.component_problems(context.console.lines, extra=component_lines(expected), ia64=True):
            return False
        return (threads.Reclaimed >= SELF_TEST_THREADS) and (threads.Kills >= 2)

    budget = SELF_TEST_SECONDS + (90.0 if expected.ps2 else 0.0) + (NETWORK_SECONDS if expected.network else 0.0)
    completed = wait_until(finished, context.seconds(budget), 0.25)
    context.check('host.ia64.user-spinner-preempted', spinner_stops > 0,
                  '%d created-thread timer stops while only the spinner was counting' % spinner_stops)
    context.check('host.ia64.self-tests-complete', completed,
                  '' if completed else 'pager/component completion exceeded the %.0fs host budget' %
                  context.seconds(budget))


def wait_for_user_ticks(context, ticks: int = USER_TICKS) -> None:
    """Lets the initial image's CPU-bound idle loop run out `ticks` time
    slices: each slice that ends at a PL3 tick readmits the
    thread under a new run stop, which the kernel programs into cr.itm, so
    the wait ends once cr.itm took `ticks` new values. The evidence record
    then shows the runs those ticks ended (`host.ia64.evidence.user-preemption`)."""
    seen: list[int] = []

    def ticked() -> bool:
        deadline = Ia64State.read(context.qmp, 0).get('CR.DCR')['itm']
        if not seen or seen[-1] != deadline:
            seen.append(deadline)
        return len(seen) > ticks or bool(context.exited() or context.console.failures())

    wait_until(ticked, context.seconds(USER_TICK_SECONDS), 0.05)


def stop_in_user_loop(context, loop_ip: int, psr: int) -> None:
    """Stops the guest in the user loop it settled in. Boot threads are
    preemptible: a timer tick may enter the kernel between the
    last sample and the stop, so the guest is resumed and stopped again until
    CPU 0 is caught in the loop (bounded; the checks judge the last stop).
    Each resumption observes the running loop within RESUME_SECONDS: under
    single-threaded TCG a round-robin gap longer than a time slice costs the CPU-bound loop
    its window (SchedulingContext.md's overrun rule), so CPU 0 may wait out a
    replenishment of up to a slice in the kernel. A fixed delay can repeatedly
    stop the same phase of that cycle; a matching live sample directs the next
    stop while the stopped-state checks still judge the captured CPU."""
    def in_loop() -> bool:
        record = Ia64State.read(context.qmp, 0).get('IP')
        return not record['halted'] and not Ia64Cpu.loop_state_problems(
            record['ip'], record['psr'], loop_ip, psr)

    for attempt in range(STOP_ATTEMPTS):
        context.stop()
        record = Ia64State.read(context.qmp, 0).get('IP')
        if attempt + 1 == STOP_ATTEMPTS or not Ia64Cpu.loop_state_problems(record['ip'], record['psr'], loop_ip, psr):
            return
        context.qmp.cont()
        wait_until(in_loop, context.seconds(RESUME_SECONDS), LOOP_POLL_SECONDS)


def wait_for_user_loop(context, loop_ip: int, psr: int) -> Ia64Cpu.LoopWait:
    """Polls the BSP until it settles in the user loop."""
    def sample() -> tuple[int, int, int]:
        record = Ia64State.read(context.qmp, 0).get('IP')
        return record['ip'], record['psr'], record['halted']

    def abort() -> str | None:
        failures = context.console.failures()
        return 'kernel failure %s' % failures[0] if failures else context.exited()

    return Ia64Cpu.wait_for_loop(sample, loop_ip, psr, samples=Ia64Layout.LOOP_SAMPLES,
                                 timeout=context.seconds(Ia64Layout.LOOP_BUDGET_SECONDS),
                                 interval=LOOP_POLL_SECONDS, abort=abort)


def gate(context, stage: str, timeout: float) -> int:
    """Waits for a stage event and returns its line index. A budget that ends
    without the event but with new debug-channel lines is renewed, at most
    GATE_RENEWALS times. If the event never comes, CPU 0's registers are kept
    as Registers.txt before the case ends."""
    def wait() -> int:
        seen = len(context.console.lines)
        renewal = 0
        while True:
            try:
                return context.wait_for_line(Stages.predicate(stage), timeout)[0]
            except CaseFailure as error:
                reported = len(context.console.lines)
                if 'no matching line' not in str(error) or reported == seen or renewal == GATE_RENEWALS:
                    raise
                seen, renewal = reported, renewal + 1

    return _keeping_registers(context, wait)


def wait_for_guest(context, since: int) -> None:
    """Waits for the guest's complete TAP stream, keeping the registers like `gate`."""
    _keeping_registers(context, lambda: Guest.wait(context, since=since))


def _keeping_registers(context, wait):
    try:
        return wait()
    except Exception:
        try:
            (context.run_directory / Ia64Capture.REGISTERS).write_text(Ia64State.read_text(context.qmp, 0))
        except (QmpError, OSError):
            pass
        raise


# Capture --------------------------------------------------------------------

def capture(context, requests: dict[str, tuple[str, int, int]], *, processors: int = 1) -> tuple[Ia64Capture.Observation, dict[str, str]]:
    """Reads CPU 0's registers, QEMU's CPU and PCI view, and each requested
    dump (`name: ('virtual' | 'physical', address, size)`) from the stopped guest.
    A dump QEMU refuses is left out and its error returned."""
    qmp = context.qmp
    observation = Ia64Capture.Observation(context.console.lines, Ia64State.read_text(qmp, 0),
                                          pci=qmp.execute('query-pci'), cpus=qmp.cpus())
    errors = {}
    for processor in range(1, processors):
        try:
            observation.processor_registers[processor] = Ia64State.read_text(qmp, processor)
        except QmpError as error:
            errors['Registers.Cpu%d' % processor] = str(error)
    for name, (space, address, size) in requests.items():
        try:
            observation.memory[name] = (qmp.memsave(address, size) if space == 'virtual'
                                        else qmp.pmemsave(address, size))
        except QmpError as error:
            errors[name] = str(error)
    return observation, errors


def capture_evidence(context, observation: Ia64Capture.Observation, expected: Expectation) -> dict[str, str]:
    """Adds the kernel's evidence record to the observation (the `Evidence`
    dump, located by its fact line) and, when the record names one, the AHCI
    READ DMA EXT buffer; returns the errors of dumps that failed."""
    address, problems = Evidence.locate(observation.lines)
    if address is None:
        return {Ia64Evidence.DUMP: '; '.join(problems)}
    try:
        data = expected.schema.read(context.qmp, address)
    except (QmpError, Evidence.EvidenceError) as error:
        return {Ia64Evidence.DUMP: str(error)}
    observation.memory[Ia64Evidence.DUMP] = data
    decoded = expected.schema.decode(data)
    if decoded.MemoryImport.Published:
        observation.memory[MemoryImport.DUMP] = context.qmp.pmemsave(decoded.MemoryImport.MapPhysical,
            decoded.MemoryImport.MapRegions * MemoryImport.MAP_RECORD.size)
    ahci = decoded.Ahci
    if ahci.ReadBufferPhysicalAddress and ahci.ReadBufferSizeBytes:
        try:
            observation.memory[AHCI_READ_BUFFER_DUMP] = context.qmp.pmemsave(ahci.ReadBufferPhysicalAddress,
                                                                             ahci.ReadBufferSizeBytes)
        except QmpError as error:
            return {AHCI_READ_BUFFER_DUMP: str(error)}
    return {}


def kernel_layout(lines: list[str], facts: Ia64Image.ImageFacts) -> Ia64Image.KernelLayout | None:
    """The kernel as the firmware loaded it, if the channel carried the geometry line."""
    loaded, _ = Ia64Image.loaded_image(lines)
    return Ia64Image.KernelLayout(facts, loaded) if loaded is not None else None


def kernel_requests(expected: Expectation, kernel: Ia64Image.KernelLayout | None) -> dict[str, tuple[str, int, int]]:
    """The live IVT (it needs the loaded base)."""
    if kernel is None:
        return {}
    ivt = expected.facts.kernel('AsteriaIa64RuntimeInterruptVectorTableImage')
    return {IVT_DUMP: ('virtual', kernel.address('AsteriaIa64RuntimeInterruptVectorTableImage'), ivt.size)}


def record(context, checks: list[Check], capture_errors: dict[str, str]) -> None:
    for name, problems, *note in checks:
        context.check(name, not problems, '\n'.join(problems) if problems else ''.join(note))
    if capture_errors:
        context.check('lifecycle.capture', False, '\n'.join('%s: %s' % item for item in capture_errors.items()))


# Checks shared by the IA-64 drivers -------------------------------------------

def cpu_state(observation: Ia64Capture.Observation) -> Ia64State.CpuState:
    """CPU 0 after the stop (an absent dump parses as unusable, failing every register check)."""
    return Ia64State.parse(observation.registers if observation.registers is not None else '', 0)


def stage_checks(observation: Ia64Capture.Observation, expected: Expectation, tail: tuple[str,...]) -> list[Check]:
    stages, absent = stage_expectation(expected.disk is not None, tail)
    if expected.iommu:
        stages = stages[:-len(tail)] + ('IOMMU-TRANSLATION',) + tail
    else:
        absent += ('IOMMU-TRANSLATION',)
    return [('lifecycle.stages', Stages.problems(observation.lines, stages, absent=absent),
             ' '.join(Stages.stages(observation.lines)))]


def evidence_record(observation: Ia64Capture.Observation, expected: Expectation) -> tuple[Evidence.Record | None,
                                                                                            list[str]]:
    """The decoded record, or why there is none."""
    data = observation.memory.get(Ia64Evidence.DUMP)
    if data is None:
        return None, ['no evidence record was read']
    problems = expected.schema.header_problems(data)
    if problems:
        return None, problems
    try:
        return expected.schema.decode(data), []
    except Evidence.EvidenceError as error:
        return None, [str(error)]


def poll_leaf(observation: Ia64Capture.Observation, expected: Expectation) -> tuple[int, int] | None:
    kernel = kernel_layout(observation.lines, expected.facts)
    return None if kernel is None else kernel.extent(Ia64Layout.POLL_LEAF_SYMBOL)


def evidence_checks(observation: Ia64Capture.Observation, expected: Expectation,
                    unreached: tuple[str,...]) -> list[Check]:
    """The evidence checks every IA-64 image shares: the record itself, the
    processors, PCI and AHCI facts, the AP runtime, the shared-root
    shootdown, and the sub-records of scenarios this image does not run."""
    names = ('host.ia64.evidence.processors', 'host.ia64.evidence.pci-ecam-first-function',
             'host.ia64.evidence.pci-ahci-function', 'host.ia64.evidence.ahci-identify',
             'host.ia64.evidence.ahci-read', 'host.ia64.evidence.ap-runtime',
             'host.ia64.evidence.shared-root-shootdown', 'host.ia64.evidence.unreached-scenarios',
             'host.ia64.evidence.iommu-boot', 'host.ia64.evidence.clocks')
    if any(field.name == 'UserProcessors' for field in expected.schema.record.fields):
        names += ('host.ia64.evidence.user-processors',)
    address, located = Evidence.locate(observation.lines)
    evidence, problems = evidence_record(observation, expected)
    checks: list[Check] = [('host.ia64.evidence.record', located + problems,
                            '' if address is None else 'the record is at %#x' % address)]
    if evidence is None:
        return checks + [(name, ['no evidence record']) for name in names]
    checks.append(('host.ia64.evidence.clocks', ProcessorClocks.problems(evidence.Clocks, expected.cpus),
                   ProcessorClocks.describe(evidence.Clocks)))
    if 'UserProcessors' in evidence:
        checks.append(('host.ia64.evidence.user-processors', UserProcessors.problems(evidence.UserProcessors, expected.cpus),
                       'every IA-64 user home remains CPU 0'))
    leaf = poll_leaf(observation, expected)
    constants, cpus = expected.constants, expected.cpus
    ap_problems = (Ia64Image.loaded_image(observation.lines)[1] if leaf is None
                   else Ia64Evidence.application_processors_problems(
                       evidence.ApplicationProcessors, cpus, constants,
                       Ia64Evidence.TimerExpectation.of(constants, leaf)))
    ahci = Ia64Evidence.describe_ahci(evidence.Ahci, expected.outcomes)
    kernel = kernel_layout(observation.lines, expected.facts)
    queue_range = kernel.section('.bss') if kernel is not None else None
    if evidence.MemoryImport.Published:
        checks.append(('host.ia64.evidence.memory-import', MemoryImport.problems(
            evidence, observation.memory.get(MemoryImport.DUMP))))
    return checks + [
        ('host.ia64.evidence.iommu-boot',
         Ia64Evidence.iommu_problems(evidence, observation.pci or [], present=expected.iommu,
                                     queue_range=queue_range, vtd=expected.vtd)),
        (names[0], Ia64Evidence.processor_problems(evidence.Processors, observation.cpus or [], cpus),
         Ia64Evidence.describe_processors(evidence.Processors)),
        (names[1], Ia64Evidence.pci_ecam_problems(evidence.Pci, observation.pci or []),
         Ia64Evidence.describe_pci(evidence.Pci)),
        (names[2], Ia64Evidence.pci_discovery_problems(evidence.Pci, observation.pci or []),
         Ia64Evidence.describe_pci(evidence.Pci)),
        (names[3], Ia64Evidence.ahci_identify_problems(evidence.Ahci, expected.disk, expected.outcomes), ahci),
        (names[4], Ia64Evidence.ahci_read_problems(evidence.Ahci, expected.disk,
                                                   observation.memory.get(AHCI_READ_BUFFER_DUMP),
                                                   expected.outcomes), ahci),
        (names[5], ap_problems, Ia64Evidence.describe_application_processors(evidence.ApplicationProcessors)),
        (names[6], Ia64Evidence.shootdown_problems(evidence.SharedRootShootdown, cpus, constants),
         Ia64Evidence.describe_shootdown(evidence.SharedRootShootdown)),
        (names[7], Ia64Evidence.unreached_problems(evidence, unreached)),
    ]


def user_preemption_checks(observation: Ia64Capture.Observation, expected: Expectation) -> list[Check]:
    """The boot threads' time slices: the initial image's idle
    loop ran out USER_TICKS slices before the stop. The test pager's spinner
    runs as a created thread and is counted there, alongside its fault,
    object-management and thread
    self-tests, which both IA-64 firmwares run."""
    evidence, problems = evidence_record(observation, expected)
    if evidence is None:
        return [(name, problems or ['no evidence record'])
                for name in ('host.ia64.evidence.iommu', 'host.ia64.evidence.user-preemption', 'host.ia64.evidence.object-management',
                             'host.ia64.evidence.threads', 'host.ia64.evidence.domains',
                             'host.ia64.evidence.components', 'host.ia64.evidence.collection',
                             'host.ia64.evidence.application-launch',
                             'host.ia64.evidence.device-control', 'host.ia64.evidence.services',
                             'host.ia64.evidence.interrupts', 'host.ia64.evidence.block')]
    constants = expected.constants
    fs = DiskRecipes.FS_DEMOS[expected.recipe]
    lan_frames = len(observation.lan[0]) if observation.lan is not None else 0
    component = Ia64Evidence.ComponentScenario(behaviors=component_behaviors(expected.recipe, expected.ps2,
                                                                             expected.network),
                                               drained=True, disk=expected.recipe, fs_root_maps=fs.root_maps,
                                               fs_service_maps=fs.service_maps,
                                               nic_copies_least=Lan.LEAST_COPIES if expected.network else 0,
                                               nic_copies_most=lan_frames if expected.network else 0,
                                               inventory=any(line.startswith("# devices:") for line in Guest.channel_lines(observation.lines)),
                                               exit_code=constants.component_exit_code,
                                               data_start=constants.component_stack_start,
                                               data_pages=constants.component_stack_pages + 1,
                                               account_exit_code=constants.account_exit_code,
                                               window_start=constants.component_window_start,
                                               window_pages=constants.component_window_pages,
                                               driver_exit_code=0xC0DE0009 if expected.iommu else constants.driver_exit_code,
                                               iommu=expected.iommu, trusted=expected.trusted,
                                               mapping_window=constants.mapping_window,
                                               images=expected.images, image_pages=expected.image_pages)
    return [('host.ia64.evidence.iommu',
             Ia64Evidence.iommu_problems(evidence, observation.pci or [], present=expected.iommu,
                                        drivers=component.drivers, driver_services=component.driver_services,
                                        storage_served=component.block_served + component.fs_served,
                                        vtd=expected.vtd, trusted=expected.trusted,
                                        nic_served=component.nic_served)),
            ('host.ia64.evidence.shared-images', SharedImages.problems(
                observation.lines, component.behaviors, component.images)), ('host.ia64.evidence.user-preemption',
             Ia64Evidence.user_preemption_problems(evidence.UserPreemption, spinner=False,
                                                   initial_runs=USER_TICKS),
             Ia64Evidence.describe_user_preemption(evidence.UserPreemption)),
            ('host.ia64.evidence.application-launch',
             ApplicationLaunch.problems(
                 evidence.ApplicationLaunch, None, component.accounts), 'Code flag and per-type refusals'),
            ('host.ia64.evidence.object-management',
             Ia64Evidence.object_management_problems(evidence.ObjectManagement, self_test=True,
                                                      frame_pool_pages=evidence.MemoryImport.ManagedPages, imported=True,
                                                      mapping_window=constants.mapping_window,
                                                      component=component, fault_scenarios=True,
                                                      frame_rights=Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(observation.lines)),
             Ia64Evidence.describe_object_management(evidence.ObjectManagement)),
            ('host.ia64.evidence.threads',
             Ia64Evidence.thread_problems(evidence.Threads, killed_exit_code=constants.killed_exit_code,
                                          slots=constants.thread_slots, pool_slots=constants.thread_pool_slots,
                                          component=component, fault_scenarios=True,
                                          entry_state_faults=sum(line.startswith(Guest.IA64_ENTRY_STATE_PREFIX)
                                              for line in Guest.channel_lines(observation.lines))),
             Ia64Evidence.describe_threads(evidence.Threads)),
            ('host.ia64.evidence.domains',
             Ia64Evidence.domain_problems(evidence.Domains, component=component,
                                          address_space_slots=constants.address_space_slots,
                                          pool_slots=constants.domain_slots),
             Ia64Evidence.describe_domains(evidence.Domains)),
            ('host.ia64.evidence.components',
             Ia64Evidence.component_problems(evidence.Components, slice_ticks=constants.slice_ticks,
                                             component=component, aborted_exits=evidence.Services.AbortedExits),
             Ia64Evidence.describe_components(evidence.Components)),
            ('host.ia64.evidence.collection', Ia64Evidence.collection_problems(evidence),
             Ia64Evidence.describe_collection(evidence.Collection)),
            ('host.ia64.evidence.device-control',
             Ia64Evidence.device_control_problems(evidence.DeviceControl, drivers=component.drivers,
                                                  driver_services=component.driver_services,
                                                  storage_quiesces=component.storage_quiesces,
                                                  nic_drivers=component.nic_drivers),
             Ia64Evidence.describe_device_control(evidence.DeviceControl)),
            ('host.ia64.evidence.services', Ia64Evidence.service_problems(
                evidence.Services, echoes=component.echoes, drivers=component.drivers,
                driver_services=component.driver_services, recoveries=component.recoveries, displays=component.displays,
                directories=component.directories, clients=component.clients, inputs=component.inputs,
                ps2=component.ps2, component=component),
             Ia64Evidence.describe_services(evidence.Services)),
            ('host.ia64.evidence.interrupts',
             Ia64Evidence.device_interrupt_problems(evidence.Interrupts, drivers=component.drivers,
                                                    driver_services=component.driver_services,
                                                    recoveries=component.recoveries, iommu=expected.iommu,
                                                    ps2=component.ps2, component=component),
             Ia64Evidence.describe_device_interrupts(evidence.Interrupts)),
            ('host.ia64.evidence.block',
             Ia64Evidence.block_problems(evidence, parked_pins=component.parked_pins,
                                         token=Machine.DISK_TOKEN if component.block_served else None,
                                         parked_pins_most=DmaLifetime.maximum_pins(
                                             component.block_served + component.fs_served,
                                             (2 * component.driver_services if expected.iommu else 0)
                                             + 3 * component.nic_served,
                                             component.drivers if expected.iommu and expected.vtd is None else 0)),
             Ia64Evidence.describe_block(evidence.Block))]


def image_checks(observation: Ia64Capture.Observation, expected: Expectation) -> list[Check]:
    if not expected.relocated:
        return []
    loaded, problems = Ia64Image.loaded_image(observation.lines)
    return [('host.ia64.image.relocated', problems if loaded is None
             else Ia64Image.relocation_problems(loaded, expected.preferred_base))]


def ivt_routes(expected: Expectation, kernel: Ia64Image.KernelLayout) -> tuple[Ia64Ivt.Route,...]:
    constants = expected.constants
    vectors = (('instruction TLB', 1024), ('data TLB', 2048), ('instruction key miss', 6144), ('data key miss', 7168), ('dirty bit', 8192), ('instruction access bit', 9216), ('data access bit', 10240), ('page not present', 20480), ('key permission', 20736), ('instruction access rights', 20992), ('data access rights', 21248), ('general exception', 21504), ('disabled FP register', 21760), ('NaT consumption', 22016), ('speculation', 22272), ('debug', 22784), ('unaligned reference', 23040), ('unsupported data reference', 23296), ('floating-point fault', 23552), ('floating-point trap', 23808), ('lower privilege transfer', 24064), ('taken branch', 24320), ('single step', 24576))
    return (Ia64Ivt.Route('break', constants.ivt_break, kernel.address('AsteriaIa64UserBreakEntry')),
            Ia64Ivt.Route('external interrupt', constants.ivt_external,
                          kernel.address(Ia64Layout.IVT_EXTERNAL_INTERRUPT_ENTRY)),
            *(Ia64Ivt.Route(name, offset, kernel.address(Ia64Layout.IVT_USER_EXCEPTION_ENTRY),
                            constants.user_exception_selector, offset >> 8) for name, offset in vectors),
            *(Ia64Ivt.Route('terminal', offset, kernel.address('AsteriaIa64StopFromIvt'), capture_ip=True)
              for offset in (0x0000, 0x1400, *range(0x3400, 0x5000, 0x400), 0x5800,
                             *range(0x6100, 0x8000, 0x100))))



def ivt_checks(observation: Ia64Capture.Observation, expected: Expectation,
               state: Ia64State.CpuState) -> list[Check]:
    """IVA, equality of live and linked IVTs, and vector routing."""
    kernel = kernel_layout(observation.lines, expected.facts)
    if kernel is None:
        missing = Ia64Image.loaded_image(observation.lines)[1]
        return [(name, missing) for name in ('host.ia64.ivt.installed-address', 'host.ia64.ivt.live-image',
                                             'host.ia64.ivt.vector-routing')]
    table = kernel.address('AsteriaIa64RuntimeInterruptVectorTableImage')
    live = observation.memory.get(IVT_DUMP)
    checks = [('host.ia64.ivt.installed-address', Ia64Ivt.installed_address_problems(state, table))]
    if live is None:
        return checks + [(name, ['no live IVT dump']) for name in ('host.ia64.ivt.live-image',
                                                                    'host.ia64.ivt.vector-routing')]
    constants = expected.constants
    checks.append(('host.ia64.ivt.live-image', Ia64Ivt.live_image_problems(live, expected.facts.ivt_image)))
    checks.append(('host.ia64.ivt.vector-routing', Ia64Ivt.vector_routing_problems(
        live, table, fail_stop=Ia64Layout.IVT_FAIL_STOP_BUNDLE,
        handlers=(constants.ivt_alternate_itlb, constants.ivt_alternate_dtlb), routes=ivt_routes(expected, kernel))))
    return checks


def loop_checks(name: str, observation: Ia64Capture.Observation, loop_ip: int, psr: int) -> list[Check]:
    if observation.loop is None:
        return [(name, ['the loop wait did not run'])]
    return [(name, Ia64Cpu.sample_problems(observation.loop, loop_ip, psr))]


# The test firmware ------------------------------------------------------------

def native_kernel_translation_checks(observation: Ia64Capture.Observation, expected: Expectation,
                                     *, include_bsp: bool = True) -> list[Check]:
    """Every native CPU must install the same four-DTR/one-image-ITR plan."""
    from AsteriaVm.Drivers import Ia64SyntheticProbe
    from AsteriaVm.Observers import Ia64Translation
    kernel = kernel_layout(observation.lines, expected.facts)
    translations = Ia64SyntheticProbe.kernel_translations(expected.constants, expected.runtime_memory_page)
    checks = []
    for processor in range(0 if include_bsp else 1, expected.cpus):
        text = observation.registers if processor == 0 else observation.processor_registers.get(processor)
        name = 'host.ia64.translation.cpu%d-kernel-trs' % processor
        if kernel is None or text is None:
            checks.append((name, ['no kernel geometry or processor register dump']))
            continue
        problems, base = Ia64Translation.kernel_translation_problems(
            Ia64State.parse(text, processor), kernel, translations)
        checks.append((name, problems, '4 DTRs + 2 ITRs (including reserved ITR0); runtime page %#x' % base
                       if base is not None else ''))
    return checks


NIC_CHECK = 'host.ia64.nic.published'
NIC_PUBLISHED_LINE = 'ASTERIA:IA64-NIC:PUBLISHED'


def network_checks(observation: Ia64Capture.Observation, expected: Expectation) -> list[Check]:
    """Where the case attaches the e1000e:
    the boot published it as the device table's NIC entry, and the LAN
    peer's capture replays AMD64's ISA-neutral judgment (AsteriaVm.Lan:
    resolution, ARP, ICMP echo, the UDP echo, the negative controls and the
    stack's drop counts); without it the boot publishes none."""
    published = NIC_PUBLISHED_LINE in observation.lines
    if not expected.network:
        return [(NIC_CHECK, ['the boot published a NIC no case attached'] if published else [])]
    frames, errors = observation.lan if observation.lan is not None else ([], ['no LAN capture'])
    return [(NIC_CHECK, [] if published else ['no %r line' % NIC_PUBLISHED_LINE])] + list(
        Lan.judge_frames(frames, observation.lines, errors, isa='ia64').items())


def evaluate(observation: Ia64Capture.Observation, expected: Expectation) -> list[Check]:
    """The lifecycle, host and evidence checks of the stopped test firmware;
    the guest's TAP checks come from the same transcript (`Guest.checks`)."""
    facts, constants = expected.facts, expected.constants
    idle = facts.user(Ia64Layout.USER_IDLE_SYMBOL).value
    state = cpu_state(observation)
    checks = loop_checks('host.ia64.cpu.user-idle-reached', observation, idle, constants.user_psr)
    checks += stage_checks(observation, expected, DEFAULT_STAGES)
    checks += evidence_checks(observation, expected, tuple(
        word for word in Ia64Evidence.UNREACHED_WORDS
        if word not in ('UserPreemption.Published', 'ObjectManagement.Published', 'Threads.Published')))
    checks += user_preemption_checks(observation, expected)
    checks += network_checks(observation, expected)
    checks += image_checks(observation, expected)
    checks.append(('host.ia64.cpu.stopped-state', Ia64Cpu.stopped_state_problems(state, idle, constants.user_psr)))
    checks.append(('host.ia64.cpu.no-pending-exception', Ia64Cpu.pending_exception_problems(state)))
    checks += ivt_checks(observation, expected, state)
    checks += native_kernel_translation_checks(observation, expected)
    return checks


def lan_peer(context) -> Lan.LanPeer | None:
    """The runner as the NIC's LAN (AsteriaVm.Lan), listening from the
    driver's start: the stack's ARP resolution may precede everything else;
    None where the case attaches no NIC."""
    if getattr(context, 'lan', None) is None:
        return None
    return Lan.LanPeer(context.lan, context.artifact(Lan.PCAP_ARTIFACT).parent).start()


def run(context) -> None:
    peer = lan_peer(context)
    try:
        _run(context, peer)
    finally:
        if peer is not None:
            peer.stop()  # Also keeps the capture of a failed boot


def _run(context, peer: Lan.LanPeer | None) -> None:
    facts = Ia64Image.ImageFacts.from_target(context.target, context.case.firmware)
    expected = expectation(context.case, context.target, facts, context.artifact('Disk.raw'))
    idle = facts.user(Ia64Layout.USER_IDLE_SYMBOL).value
    launch = gate(context, LAUNCH_STAGE, BOOT_TIMEOUT)
    wait_for_guest(context, launch + 1)
    # Observe the spinner before an idle wait can consume its entire counting
    # phase (IA-64 fault delivery's created-thread spinner).
    wait_for_self_tests(context, expected, peer)
    loop = wait_for_user_loop(context, idle, expected.constants.user_psr)
    wait_for_user_ticks(context)
    stop_in_user_loop(context, idle, expected.constants.user_psr)
    if peer is not None:
        peer.stop()  # Drain the stopped guest's last frames before the judgment
    observation, errors = capture(context, kernel_requests(expected, kernel_layout(context.console.lines, facts)),
                                  processors=expected.cpus)
    errors.update(capture_evidence(context, observation, expected))
    observation.loop = loop
    observation.lan = Ia64Capture.load_lan(context.run_directory)
    Ia64Capture.save(observation, context.run_directory)
    Guest.record(context, observation.lines)
    context.check('pager.frame-rights', Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(observation.lines))
    context.check_problems(Guest.THREAD_CHECK, Guest.thread_problems(observation.lines))
    context.check_problems('guest.ia64.supervisor-faults', Ia64Faults.problems(observation.lines))
    context.check_problems(Guest.COMPONENT_CHECK,
                           Guest.component_problems(observation.lines, extra=component_lines(expected), ia64=True))
    record(context, evaluate(observation, expected), errors)
    if expected.ps2:
        evidence, _ = evidence_record(observation, expected)
        injection = Ps2.Injection(context)
        injection.sent = context.artifact('Ps2Injection.json').exists()
        baseline = context.artifact('Ps2Before.bin')
        if baseline.exists():
            injection.before = expected.schema.decode(baseline.read_bytes()).Interrupts
        injection.check(evidence)
    _check_display(context)
    # On AsteriaFS hello holds what its last commit, the one crashed after its
    # superblock, left: page 0 rewritten as pattern 3, page 1 as first written.
    DiskImages.check(context, 'ia64', [DiskRecipes.hello_page(3), DiskRecipes.hello_page(1)])


def _check_display(context) -> None:
    """Compare every scanout pixel with the shared console model."""
    path = context.artifact('Screen.ppm')
    try:
        context.qmp.screendump(path, format='ppm')
        ppm = path.read_bytes()
    except (QmpError, OSError) as error:
        context.check('host.ia64.display', False, 'no screendump: %s' % error)
        return
    context.check_problems('host.ia64.display', Display.display_problems(
        ppm, Guest.root_service_lines(context.console.lines), pathlib.Path(context.target.root), isa='Ia64'))
