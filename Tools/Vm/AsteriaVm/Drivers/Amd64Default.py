# SPDX-License-Identifier: Apache-2.0
"""AMD64 image: boot to the user image, wait until it idles, observe it.

Amd64.Smp1-4, Amd64.Ahci and Amd64.Relocated boot the test firmware; the
Amd64Preemption.* cases run the same flow on the production firmware of the
preemption-probe presets (Drivers.Amd64NativePreemption):

1. Wait for the kernel's stage event `ASTERIA:STAGE:AMD64-USER-LAUNCH`. In
   the test firmware the initial image is the guest test program: wait for
   its complete TAP stream (`AsteriaVm.Guest`), then for the line of the
   spinner, which runs CPU-bound beside it until then; the production image
   reports nothing. Then poll CPU 0 through QMP until it executes the
   image's idle loop, which the image enters only after `main` returned
   (both of its WaitSet waits included).
2. Stop the guest, capture every CPU's registers (`dump-guest-memory` ELF
   notes, native QMP; Observers.Amd64Cpu), and read the kernel's evidence
   record (`AsteriaVm.Evidence`, located by its fact line) through the same
   physical-page cache as every other read.
3. Judge:
   - guest: the TAP stream and each of its test points (everything the
     system-call ABI shows: BSS, Query, IPC, DebugWrite, WaitSet waits, the
     user selectors, RFLAGS and stack), and the spinner's line after the
     complete stream;
   - lifecycle: every stage of this configuration reported once and in
     order, and no other stage (`AsteriaVm.Stages`);
   - host: BSP and AP register state (the control-register policy on
     every CPU), page walks of the user root and of
     the shared kernel root (W^X, guards, root separation, shootdown probe
     page, MMIO window, kernel stacks), the relocated image base;
   - evidence (`Observers.Amd64Evidence`): the runtime's placement, the
     processor count, PCI and AHCI facts against QEMU and the runner's disk
     fixture, every AP's timer, delivery and service scenario, the
     shared-root shootdown, the WaitSet fixture's resumed waits, the user
     preemptions and the spinner, and the native-preemption proof or its
     absence.

The pool, the roots and the local APIC come from the evidence record, the
pool layout, selectors and CPU policies from the C++ sources
(`AsteriaVm.Amd64Layout`); nothing is derived arithmetically from a register.
The executor keeps a PNG screendump as an artifact. Every page the checks read is kept in
`Observation.json.gz`, the record in `Evidence.bin`.
"""
from __future__ import annotations

import dataclasses
import gzip
import json
import pathlib
import time

from AsteriaVm import Amd64Layout, DiskRecipes, Display, Evidence, Guest, Lan, Machine, Ps2, Stages
from AsteriaVm.Observers import UserProcessors, ProcessorClocks, MemoryImport, KernelBoundary, SharedImages, Amd64Init, Amd64Cpu, Amd64Evidence, Amd64Image, Amd64Paging, DiskImages
from AsteriaVm.Qmp import QmpError, wait_until
from AsteriaVm.Observers import DmaLifetime

PAGE = Amd64Paging.PAGE_SIZE
BOOT_SECONDS = 60      # Launch stage (firmware, 3 s UEFI stall, SMP bring-up, probes)
WAIT_SECONDS = 40      # Both WaitSet timer waits of the production image
IDLE_SECONDS = 10      # User code between its last report (TAP plan) and its idle loop
COMPONENT_SECONDS = 180  # Root scenarios, including the LAN script's bounded 30 + 30 + 60 s retries
MAXIMUM_BUFFER_BYTES = 4096   # The AHCI read buffer is one DMA page
NIC_CHECK = 'host.amd64.nic.published'
NIC_PUBLISHED_LINE = 'ASTERIA:AMD64-NIC:PUBLISHED'
STOP_ATTEMPTS = 20            # Stops until CPU 0 is caught in the idle loop rather than in a scheduling tick
# Between stops the guest runs a little longer each time: under a loaded host
# a slice's replenishment wait in the kernel can outlast twenty stops a
# millisecond apart.
STOP_BACKOFF_SECONDS = 0.005

# Stage events: the boot path every AMD64 image takes (BootApplication.cpp,
# InitialAddressSpacePreparation.S, Amd64BootRuntime.cpp, Amd64BootstrapDevices.cpp),
# the AHCI transfer of a disk case, the runtime scenarios, the optional
# preemption proof, and the user launch.
BOOT_STAGES = (
    'SYSTEM-TABLE', 'CONSOLE', 'BOOT-SERVICES', 'RUNTIME-SERVICES', 'SYSTEM-TABLE-CRC', 'LOADED-IMAGE',
    'AMD64-IMAGE-LAYOUT', 'KERNEL-ELF', 'MEMORY-MAP', 'AMD64-AHCI-RESOURCE', 'ACPI', 'GRAPHICS', 'AMD64-BOOTSTRAP-RESERVATION',
    'BOOT', 'EXIT-BOOT-SERVICES', 'BOOT-INFO', 'PAGE-ALLOCATOR', 'AMD64-INITIAL-MAPPING-PLAN',
    'AMD64-INITIAL-PAGE-TABLE', 'AMD64-INITIAL-ADDRESS-SPACE', 'AMD64-RUNTIME-ROOT', 'AMD64-PCI')
DISK_STAGES = ('AMD64-AHCI-IDENTIFY', 'AMD64-AHCI-READ')
RUNTIME_STAGES = ('AMD64-AP-RUNTIME', 'AMD64-SHOOTDOWN')
PREEMPTION_STAGE = 'AMD64-NATIVE-PREEMPTION'
LAUNCH_STAGE = 'AMD64-USER-LAUNCH'
# The disk's read rate the root service asks for before the read its AHCI
# driver crashes on: a sector a second, so the read
# is still running when the Device Manager resets the controller.
DISK_THROTTLE = {'device': Machine.DISK_DRIVE, 'bps': 0, 'bps_rd': 512, 'bps_wr': 0, 'iops': 0, 'iops_rd': 0,
                 'iops_wr': 0}
# The kernel's line for a Terminate the pager made (Amd64BootRuntime.cpp RecordTermination).
TERMINATED_EVENT = 'ASTERIA:AMD64-PAGER:TERMINATED:'


def stage_expectation(disk: bool, preemption: bool) -> tuple[str,...]:
    """Every stage one configuration reports, in order."""
    return (BOOT_STAGES + (DISK_STAGES if disk else ()) + RUNTIME_STAGES
            + ((PREEMPTION_STAGE,) if preemption else ()) + (LAUNCH_STAGE,))


def stage_problems(lines: list[str], disk: bool, preemption: bool) -> list[str]:
    """Every stage of the configuration once and in order, and no other stage."""
    expected = stage_expectation(disk, preemption)
    problems = Stages.problems(lines, expected)
    return problems + ['stage %s is reported, but this configuration has no such stage' % name
                       for name in dict.fromkeys(Stages.stages(lines)) if name not in expected]


def run(context) -> None:
    run_image(context)


@dataclasses.dataclass(frozen=True)
class Expectations:
    """What the checks compare against, derived from the preset tree and Source/."""
    constants: Amd64Layout.HeaderConstants
    loop: Amd64Cpu.IdleLoop
    segments: list[Amd64Paging.ImageSegment]
    schema: Evidence.Schema
    outcomes: dict[str, int]
    # The module images' staged and zero-filled segments, by name.
    images: tuple[tuple[str, tuple[int, int]],...] = ()
    image_pages: tuple[tuple[str, int],...] = ()

    private_pages: int = 0
    display_private_pages: int = 0

    @classmethod
    def load(cls, target, firmware: str) -> 'Expectations':
        """From the user image of one firmware of the tree (`firmware` or `firmware-test`)."""
        root = pathlib.Path(target.root)
        user_image = target.artifact('%s:user_elf' % firmware)
        return cls(Amd64Layout.headers(root),
                   Amd64Cpu.idle_loop(target.tool('objdump'), user_image, Amd64Layout.USER_IDLE_SYMBOL),
                   Amd64Paging.user_image_segments(user_image), Amd64Evidence.schema(root),
                   Amd64Evidence.outcomes(root), component_images(target),
                   tuple((name, SharedImages.layout(pathlib.Path(target.artifact('firmware:%s_elf' % name)))[1])
                         for name in COMPONENT_IMAGES) + (('smp', SharedImages.layout(pathlib.Path(target.artifact('firmware-test:smp_elf')))[1]),),
                   SharedImages.private_pages(pathlib.Path(target.artifact('firmware:component_elf'))),
                   SharedImages.private_pages(pathlib.Path(target.artifact('firmware:display_elf'))))


COMPONENT_IMAGES = ('component', 'directory', 'client', 'interpreter', 'display')


def component_images(target) -> tuple[tuple[str, tuple[int, int]],...]:
    """The staged and zero-filled segments of each native module image."""
    return tuple((name, Amd64Paging.staged_segments(target.artifact('firmware:%s_elf' % name)))
                 for name in COMPONENT_IMAGES) + (('smp', Amd64Paging.staged_segments(target.artifact('firmware-test:smp_elf'))),)


class Observation:
    """Physical memory as the checks read it (each page once) plus QMP query results.

    Live, the pages come from `pmemsave`; the record (`Observation.json.gz`
    in the run directory) replays a run offline."""

    def __init__(self, physical, query):
        self.physical = Amd64Paging.PageCache(physical)
        self.walker = Amd64Paging.PageWalker(self.physical)
        self._query = query
        self.queries: dict[str, object] = {}

    @classmethod
    def live(cls, qmp) -> 'Observation':
        return cls(lambda page: qmp.pmemsave(page, PAGE), qmp.execute)

    @classmethod
    def recorded(cls, path) -> 'Observation':
        with gzip.open(path, 'rt') as handle:
            record = json.load(handle)

        def pages(space: str):
            recorded = {int(page, 16): bytes.fromhex(data) for page, data in record[space].items()}

            def fetch(page: int) -> bytes:
                if page not in recorded:
                    raise Amd64Paging.PagingError('page %#x of %s was not recorded' % (page, space))
                return recorded[page]
            return fetch
        return cls(pages('physical'), lambda command: record['queries'][command])

    def query(self, command: str):
        self.queries[command] = self._query(command)
        return self.queries[command]

    def pmemsave(self, address: int, size: int) -> bytes:
        """Physical bytes through the page cache (the reader `Evidence.Schema.read` takes)."""
        return self.physical.read(address, size)

    def save(self, path) -> None:
        record = {
            'physical': {'%#x' % page: data.hex() for page, data in sorted(self.physical.pages.items())},
            'queries': self.queries,
        }
        with gzip.open(path, 'wt') as handle:
            json.dump(record, handle)


def evidence_record(observation: Observation, lines: list[str],
                    schema: Evidence.Schema) -> tuple[Evidence.Record | None, bytes | None, list[str]]:
    """(decoded record, its bytes, problems): the record the fact line names,
    if its header is the one the build describes."""
    address, problems = Evidence.locate(lines)
    if address is None:
        return None, None, problems
    try:
        data = schema.read(observation, address)
        return schema.decode(data), data, []
    except (QmpError, Amd64Paging.PagingError, Evidence.EvidenceError) as error:
        return None, None, ['the record at %#x: %s' % (address, error)]


def run_image(context) -> None:
    # ARP resolution precedes the listening line, and may precede the TAP
    # stream. Keep the peer alive from driver entry, even during boot waits.
    peer = (Lan.LanPeer(context.lan, context.artifact(Lan.PCAP_ARTIFACT).parent).start()
            if getattr(context, 'lan', None) is not None else None)
    try:
        _run_image(context, peer)
    finally:
        if peer is not None:
            peer.stop()  # Also preserves the capture on failed boot/observation


def _run_image(context, peer: Lan.LanPeer | None) -> None:
    try:
        expectations = Expectations.load(context.target, context.case.firmware)
    except Exception as error:  # The build tree or its sources do not have the expected shape
        context.require('host.amd64.expectations', False, '%s: %s' % (type(error).__name__, error))

    input_demo = Ps2.Injection(context)
    launch, _ = context.wait_for_line(Stages.predicate(LAUNCH_STAGE), BOOT_SECONDS)
    if context.case.guest:
        _check_scanout_mapping(context, expectations)
        Guest.wait(context, since=launch + 1)
        # The spinner then faults and the pager terminates it, so only the
        # guest's idle loop runs. The pager's own faulting thread was
        # terminated earlier, so only a termination after the spinner's line
        # is the spinner's.
        spinner = Guest.wait_for_spinner(context, since=launch + 1)
        context.wait_for_line(TERMINATED_EVENT, Guest.SPINNER_SECONDS, since=spinner + 1)
        # The root service's scenarios run beside the guest, the last one the
        # loader's; the state judged after the stop is theirs once they ended.
        throttled = []

        def throttle_once() -> None:
            input_demo.poll()
            channel = Guest.channel_lines(context.console.lines)
            if peer is not None and Lan.LISTENING_LINE in channel:
                peer.start_script()  # Idempotent, as is the throttle below
            if not throttled and Guest.BLOCK_THROTTLE_LINE in channel:
                context.qmp.execute('block_set_io_throttle', DISK_THROTTLE)
                throttled.append(True)

        wait_until(lambda: throttle_once() or context.exited() or bool(context.console.failures())
                   or not Guest.component_problems(context.console.lines, echo=True, disk=context.case.disk, ports=context.case.i8042),
                   context.seconds(COMPONENT_SECONDS), 0.1)
        idle_seconds = IDLE_SECONDS
    else:
        context.wait_for_line('USER:# init: ready: display, input, ps2', COMPONENT_SECONDS)
        idle_seconds = WAIT_SECONDS + IDLE_SECONDS
    # The image enters its idle loop only after `main` returned.
    reached, detail = Amd64Cpu.wait_for_idle(
        context.qmp, context.artifact('CpuPoll.elf'), expectations.loop, context.seconds(idle_seconds),
        abort=lambda: context.exited() or next(iter(context.console.failures()), None))
    context.check('lifecycle.guest-complete', reached, detail)

    # User threads are preemptible: a scheduling tick may enter
    # the kernel between the sample above and the stop, and the spinner may
    # have been preempted between its line and its fault. Resume and stop
    # again until CPU 0 is caught in the loop it reached and the pager has
    # terminated the spinner (bounded).
    for attempt in range(STOP_ATTEMPTS):
        context.stop()
        try:
            snapshot = Amd64Cpu.capture(context.qmp, context.artifact('CpuState.elf'))
        except Amd64Cpu.CpuStateError as error:
            context.require('host.amd64.cpu.state', False, str(error))
        settled = snapshot.cpus[0].rip in expectations.loop.addresses and (
            not context.case.guest or _spinner_terminated(context, expectations))
        if not reached or settled or attempt + 1 == STOP_ATTEMPTS:
            break
        context.qmp.cont()
        time.sleep(STOP_BACKOFF_SECONDS * (attempt + 1))
    if peer is not None:
        peer.stop()  # Drain the stopped guest's final frames; artifacts precede judge
    if context.case.guest:
        context.check('pager.frame-rights', Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(context.console.lines))
        Guest.record(context, context.console.lines)
        context.check_problems(Guest.SPINNER_CHECK, Guest.spinner_problems(context.console.lines))
        context.check_problems(Guest.THREAD_CHECK, Guest.thread_problems(context.console.lines))
        context.check_problems(Guest.COMPONENT_CHECK, Guest.component_problems(
            context.console.lines, echo=True, disk=context.case.disk, ports=context.case.i8042))
        if not context.case.i8042:
            context.check_problems('i8042.unpublished', [] if 'ASTERIA:I8042:UNPUBLISHED' in context.console.lines
                                   else ['boot did not report that the optional i8042 was unpublished'])
        _check_display(context)
    observation = Observation.live(context.qmp)
    try:
        judge(context, expectations, snapshot, observation)
        if context.case.guest and context.case.i8042:
            record = None
            if input_demo.sent:
                record, _, _ = evidence_record(observation, context.console.lines, expectations.schema)
            input_demo.check(record)
    finally:
        observation.save(context.artifact('Observation.json.gz'))
    check_disk_images(context, [DiskRecipes.hello_page(3), DiskRecipes.hello_page(1)])


def check_disk_images(context, asteriafs_pages: list[bytes]) -> None:
    """The disk after the filesystem demonstration (DiskImages.check): on
    AsteriaFS hello holds the pages its last commit, the one crashed after
    its superblock, left: page 0 rewritten as pattern 3, page 1 as first
    written."""
    DiskImages.check(context, 'amd64', asteriafs_pages)


def _check_scanout_mapping(context, expected: Expectations) -> None:
    """Inspect the broker while resident, before the root service's scenarios finish."""
    _, line = context.wait_for_line(lambda line: line.startswith('USER:# display: ') and 'stopped' not in line,
                                    COMPONENT_SECONDS)
    mode = Display.parse_start(line.removeprefix('USER:').strip())
    context.stop()
    observation = Observation.live(context.qmp)
    try:
        evidence, _, errors = evidence_record(observation, context.console.lines, expected.schema)
        if mode is None or evidence is None:
            context.check_problems('host.amd64.display.mapping', errors or ['no display mode/evidence'])
            return
        c = expected.constants
        pool = evidence.Runtime.PoolPhysicalStart
        roots = [c.root(pool, index) for index in range(4)]
        roots += Amd64Paging.candidate_roots(observation.walker,
            [c.pool_page(pool, c.reserved_arena_first_page + slot * c.root_arena_pages)
             for slot in range(c.reserved_arenas)]
            + (c.arena_roots(evidence.Metadata.ArenaPhysicalStart) if evidence.Metadata.Published else []))
        pages = (mode[0] * mode[1] * 4 + 4095) // 4096  # QEMU GOP pitch equals its mode width
        context.check_problems('host.amd64.display.mapping', Amd64Paging.scanout_problems(
            observation.walker, roots, c.scanout_address, pages, c.expected_pat))
    finally:
        observation.save(context.artifact('ScanoutObservation.json.gz'))
        context.qmp.cont()


DISPLAY_CHECK = 'host.amd64.display'


def _check_display(context) -> None:
    """The System Display Broker's console: the
    screen, dumped as PPM (`Screen.ppm`), shows every pixel of the root
    service's lines between its two display lines as a model of the console
    draws them."""
    path = context.artifact('Screen.ppm')
    try:
        context.qmp.screendump(path, format='ppm')
        ppm = path.read_bytes()
    except (QmpError, OSError) as error:
        context.check(DISPLAY_CHECK, False, 'no screendump: %s' % error)
        return
    lines = Guest.root_service_lines(context.console.lines)
    context.check_problems(DISPLAY_CHECK, Display.display_problems(ppm, lines, pathlib.Path(context.target.root))
                           + Display.resolution_problems(lines, context.case.expected_display))


def _spinner_terminated(context, expectations: Expectations) -> bool:
    """Whether the stopped guest's evidence record shows the pager terminated the spinner."""
    record, _, _ = evidence_record(Observation.live(context.qmp), context.console.lines, expectations.schema)
    return record is not None and record.Supervision.SpinnerExited == 1


def judge(context, expectations: Expectations, snapshot: Amd64Cpu.CpuSnapshot, observation: Observation) -> None:
    """Every lifecycle, host and evidence check of the stopped guest;
    replayable from a run's recorded artifacts. (The guest's TAP checks come
    from the channel transcript alone, `Guest.checks`.)"""
    case, lines = context.case, context.console.lines
    preemption = context.target.option('amd64_native_preemption_probe') == '1'
    problems = stage_problems(lines, case.disk, preemption)
    context.check('lifecycle.stages', not problems, '\n'.join(problems) or ' '.join(Stages.stages(lines)))
    if case.guest:
        # The e1000e the runner attaches is an assignable device in the
        # pager's device table.
        context.check(NIC_CHECK, NIC_PUBLISHED_LINE in lines, 'no %r line' % NIC_PUBLISHED_LINE)
        for name, problems in Lan.judge_artifacts(context.artifact(Lan.PCAP_ARTIFACT).parent, lines).items():
            context.check_problems(name, problems)
    record, data, problems = evidence_record(observation, lines, expectations.schema)
    if data is not None:
        context.artifact(Evidence.ARTIFACT).write_bytes(data)
    context.check('host.amd64.evidence.record', record is not None,
                  '\n'.join(problems) if problems else 'the record is at %#x' % Evidence.locate(lines)[0])
    _check_bsp(context, snapshot, expectations)
    _check_paging(context, observation, snapshot.cpus[0], record, expectations)
    _check_processors(context, observation, snapshot, record, expectations.constants)
    _check_evidence(context, observation, snapshot.cpus[0], record, expectations, preemption)
    if case.boot_services_absent:
        address, problems = KernelBoundary.system_table(lines)
        if address is not None:
            header, problems = _read(observation.physical, address, 24)
            if header is not None:
                size = int.from_bytes(header[12:16], 'little')
                if 120 <= size <= 512:
                    data, problems = _read(observation.physical, address, size)
                    if data is not None:
                        problems = KernelBoundary.problems(data)
                else:
                    problems = ['system table extent is invalid']
        context.check_problems('host.amd64.kernel.boot-services-absent', problems)
    if case.x2apic:
        context.check('host.amd64.apic.x2apic', record is not None and record.X2Apic == 1,
                      'the test entry must leave the BSP in x2APIC mode')
    if not case.pit:
        context.check('host.amd64.tsc.no-pit', record is not None and record.TscSource in (1, 2, 3, 4, 6, 7)
                      and record.TscFrequencyHz >= 1_000_000,
                      'calibration must use CPUID, PM timer or HPET with PIT removed')
    if case.relocated:
        context.check_problems('host.amd64.image.relocated',
                               Amd64Image.relocation_problems(lines, int(context.target.option('image_base'), 16)))


def _read(memory, address: int, size: int) -> tuple[bytes | None, list[str]]:
    try:
        return memory.read(address, size), []
    except (QmpError, Amd64Paging.PagingError) as error:
        return None, ['cannot read %#x..%#x: %s' % (address, address + size, error)]


def control_register_policy(constants: Amd64Layout.HeaderConstants) -> Amd64Cpu.ControlRegisterPolicy:
    lazy = (constants.lazy_start, constants.lazy_start + constants.lazy_pages * constants.page_size)
    spinner = (constants.spinner_fault_address, constants.spinner_fault_address + 8)
    component = (constants.component_window_start, constants.component_window_start + (UserProcessors.worker_count() + 1) * PAGE)
    return Amd64Cpu.ControlRegisterPolicy(constants.cr0_required, constants.cr0_forbidden, constants.cr4_installed,
                                          (lazy, spinner, component))


def _check_bsp(context, snapshot, expectations: Expectations) -> None:
    bsp, constants = snapshot.cpus[0], expectations.constants
    context.check_problems('host.amd64.cpu.bsp.idle-loop', Amd64Cpu.idle_problems(bsp, expectations.loop))
    context.check_problems('host.amd64.cpu.bsp.user-mode', Amd64Cpu.user_mode_problems(
        bsp, Amd64Cpu.UserModeExpectation(
            constants.user_code_selector, constants.user_data_selector,
            constants.user_stack_start + constants.user_stack_pages * PAGE,
            constants.user_rflags_required, constants.user_rflags_permitted)))
    context.check_problems('host.amd64.cpu.bsp.control-registers', Amd64Cpu.control_register_problems(
        bsp, control_register_policy(constants)))
    context.check_problems('host.amd64.cpu.bsp.long-mode', Amd64Cpu.long_mode_problems(snapshot, bsp))
    problems = Amd64Cpu.root_table_problems(bsp)
    context.require('host.amd64.cpu.bsp.root-table', not problems, '\n'.join(problems))


SHARED_ROOT_CHECKS = ('host.amd64.paging.root-separation', 'host.amd64.paging.shootdown-probe',
                      'host.amd64.paging.mmio-window', 'host.amd64.paging.kernel-stacks',
                      'host.amd64.paging.pager-window', 'host.amd64.paging.scrub-slots',
                      'host.amd64.paging.thread-stacks', 'host.amd64.evidence.allocator')


def frame_pool(constants: Amd64Layout.HeaderConstants, pool: int) -> tuple[int, int]:
    """[start, end) of the frame pool created Frames draw from."""
    if constants.frame_pool_pages == 0:
        return 0, 1 << 32
    first = constants.frame_pool_first_page
    return constants.pool_page(pool, first), constants.pool_page(pool, first + constants.frame_pool_pages)


def stack_layout(constants: Amd64Layout.HeaderConstants, pool: int) -> Amd64Paging.StackSlotLayout:
    return Amd64Paging.StackSlotLayout(constants.stack_extent(pool, 0), constants.stack_extent_pages,
                                       constants.stacks)


def _check_paging(context, observation, bsp, record, expectations: Expectations) -> None:
    walker, constants, segments = observation.walker, expectations.constants, expectations.segments
    user_root = bsp.cr3
    stack_start, stack_pages = constants.user_stack_start, constants.user_stack_pages
    context.check_problems('host.amd64.paging.user-image',
                           Amd64Paging.user_image_problems(walker, user_root, segments))
    context.check_problems('host.amd64.paging.user-stack',
                           Amd64Paging.user_stack_problems(walker, user_root, stack_start, stack_pages))
    context.check_problems('host.amd64.paging.null-page',
                           Amd64Paging.expect_unmapped(walker, user_root, 0, 'page zero'))
    context.check_problems('host.amd64.paging.descriptor-tables', Amd64Paging.descriptor_table_problems(
        walker, user_root, {'GDT': bsp.segments['gdt'].base, 'IDT': bsp.segments['idt'].base}))
    if record is None:
        for name in SHARED_ROOT_CHECKS:
            context.check(name, False, 'no evidence record names the shared root')
        return
    runtime = record.Runtime
    shared_root, pool = runtime.SharedRootPhysicalAddress, runtime.PoolPhysicalStart
    user_pages = {'user image page %#x' % page: page
                  for segment in segments for page in range(segment.start, segment.end, PAGE)}
    user_pages.update({'user stack page %#x' % page: page
                       for page in range(stack_start, stack_start + stack_pages * PAGE, PAGE)})
    context.check_problems(SHARED_ROOT_CHECKS[0],
                           Amd64Paging.root_separation_problems(walker, shared_root, user_pages))
    context.check_problems(SHARED_ROOT_CHECKS[1], Amd64Paging.shootdown_probe_problems(
        walker, user_root, shared_root, constants.shared_probe_address, constants.shared_probe_value))
    context.check_problems(SHARED_ROOT_CHECKS[2], Amd64Paging.mmio_window_problems(
        walker, {'private': user_root, 'shared': shared_root}, constants.mmio_window_address,
        runtime.LocalApicPhysicalAddress))
    context.check_problems(SHARED_ROOT_CHECKS[3], Amd64Paging.kernel_stack_problems(
        walker, user_root, stack_layout(constants, pool), constants.stack_slots))
    management = record.ObjectManagement
    context.check_problems(SHARED_ROOT_CHECKS[4], Amd64Paging.mapped_window_problems(
        walker, constants.root(pool, constants.pager_root_index), constants.mapping_window_start,
        constants.mapping_window_pages, management.MappedPages if context.case.guest else 0,
        management.MappedPhysicalStart if context.case.guest else 0,
        constants.mapped_page_pattern,
        unmapped=_all_unmapped(context, management, constants, expectations),
        scrubbed=context.case.guest))
    roots = {'shared': constants.shared_root_index, 'client': constants.user_root_index,
             'pager': constants.pager_root_index}
    if context.case.guest:
        roots['spinner'] = constants.spinner_root_index
    roots = {name: constants.root(pool, index) for name, index in roots.items()}
    context.check_problems(SHARED_ROOT_CHECKS[7], Amd64Evidence.allocator_problems(
        record, record.MemoryImport.ManagedPages, roots['pager'], creation_probe=context.case.guest))
    memory_map = observation.physical.read(record.MemoryImport.MapPhysical, record.MemoryImport.MapRegions * MemoryImport.MAP_RECORD.size)
    context.check_problems('host.amd64.evidence.memory-import', MemoryImport.problems(record,
        memory_map, require_all=context.case.name == 'Amd64.Memory6G'))
    if context.case.memory_rmrr:
        from AsteriaVm import Dmar
        context.check_problems('host.amd64.evidence.rmrr-bootservices', MemoryImport.rmrr_problems(
            record, memory_map, Dmar.BOOTSERVICES_RMRR_BASE, Dmar.BOOTSERVICES_RMRR_LIMIT))
    context.check_problems(SHARED_ROOT_CHECKS[5], Amd64Paging.scrub_slot_problems(
        walker, roots, constants.scrub_slot_virtual_address,
        [constants.pool_page(pool, constants.scrub_slot_first_page + cpu) for cpu in range(constants.stack_slots)]))
    # The runtime pool's created-thread stacks, then the metadata arena's
    arena = record.Metadata.ArenaPhysicalStart if record.Metadata.Published else 0
    context.check_problems(SHARED_ROOT_CHECKS[6], Amd64Paging.thread_stack_problems(
        walker, roots, constants.thread_stacks(pool) + (constants.arena_thread_stacks(arena) if arena else []),
        constants.kernel_stack_pages) + ([] if arena else ['the metadata arena is not published']))


def _check_processors(context, observation, snapshot, record, constants: Amd64Layout.HeaderConstants) -> None:
    cpus = observation.query('query-cpus-fast')
    expected = context.case.cpus
    context.check_problems('host.amd64.cpu.count', [] if len(cpus) == len(snapshot.cpus) == expected else [
        'query-cpus-fast lists %d CPUs and the dump has %d; the case asks for %d' % (
            len(cpus), len(snapshot.cpus), expected)])
    # Every AP follows the BSP's control-register policy, whatever its startup
    # code left; this needs no evidence record.
    context.check_problems('host.amd64.cpu.ap-control-registers', [
        problem for cpu in snapshot.cpus[1:]
        for problem in Amd64Cpu.control_register_problems(cpu, control_register_policy(constants))])
    names = ('host.amd64.cpu.ap-registers', 'host.amd64.cpu.ap-descriptor-tables', 'host.amd64.cpu.ap-entry-context')
    if record is None:
        for name in names:
            context.check(name, False, 'no evidence record names the shared root and the pool')
        return
    shared_root, pool = record.Runtime.SharedRootPhysicalAddress, record.Runtime.PoolPhysicalStart
    gdt0 = snapshot.cpus[0].segments['gdt'].base
    registers, tables, contexts = [], [], []
    window = record.Smp.InterruptWindowStart, record.Smp.InterruptWindowEnd
    # Only the exact native STI/PAUSE/CLI sequence may explain a captured IF=1. Its placed address
    # is supplied by the kernel.
    if window[0] == 0 or window[1] - window[0] != 4:
        registers.append('AP interrupt window has invalid bounds')
    else:
        code, problems = _read(observation.physical, window[0], 4)
        registers += problems
        if code != b'\xfb\xf3\x90\xfa':
            registers.append('AP interrupt window is not STI/PAUSE/CLI')
    for cpu in snapshot.cpus[1:]:
        low, high = constants.idle_stack_range(pool, cpu.index)
        registers += Amd64Cpu.parked_processor_problems(cpu, Amd64Cpu.ParkedProcessorExpectation(
            constants.idle_root(pool, cpu.index), constants.kernel_code_selector, constants.kernel_data_selector, low, high,
            (window[0] + 1, window[1])))
        registers += Amd64Cpu.long_mode_problems(snapshot, cpu)
        gdt = gdt0 + cpu.index * constants.descriptor_tables_size
        tables += Amd64Cpu.descriptor_table_problems(cpu, gdt, gdt + constants.idt_offset)
        contexts += _entry_context_problems(observation, cpu, constants,
                                            (constants.entry_stack_top(pool, cpu.index), 0, cpu.index, 0))
    context.check_problems(names[0], registers)
    context.check_problems(names[1], tables)
    context.check_problems(names[2], contexts)


def _entry_context_problems(observation, cpu, constants: Amd64Layout.HeaderConstants,
                            wanted: tuple[int,...]) -> list[str]:
    """The AP's GS base names its own per-CPU entry context (read through its own root)."""
    base = cpu.segments['gs'].base
    try:
        leaf = observation.walker.walk(cpu.cr3, base)
    except Amd64Paging.PagingError as error:
        return ['CPU %d GS base: %s' % (cpu.index, error)]
    if leaf is None:
        return ['CPU %d GS base %#x is not mapped in its root %#x' % (cpu.index, base, cpu.cr3)]
    data, problems = _read(observation.physical, leaf.physical, constants.entry_context_size)
    if data is None:
        return problems
    observed = tuple(int.from_bytes(data[offset:offset + 8], 'little') for offset in range(0, len(data), 8))
    if observed != wanted:
        return ['CPU %d per-CPU entry context at %#x is (%s), expected (%s)' % (
            cpu.index, base, ', '.join('%#x' % value for value in observed),
            ', '.join('%#x' % value for value in wanted))]
    return []


EVIDENCE_CHECKS = ('host.amd64.evidence.runtime', 'host.amd64.evidence.processors',
                   'host.amd64.evidence.pci-ecam', 'host.amd64.evidence.pci-ahci-function',
                   'host.amd64.evidence.ahci-identify', 'host.amd64.evidence.ahci-read',
                   'host.amd64.evidence.ap-runtime', 'host.amd64.evidence.shared-root-shootdown',
                   'host.amd64.evidence.smp-ipis',
                   'host.amd64.evidence.wait', 'host.amd64.evidence.user-preemption',
                   'host.amd64.evidence.supervision', 'host.amd64.evidence.object-management',
                   'host.amd64.evidence.threads', 'host.amd64.evidence.components',
                   'host.amd64.evidence.metadata', 'host.amd64.evidence.services', 'host.amd64.evidence.iommu',
                   'host.amd64.evidence.interrupts', 'host.amd64.evidence.device-control',
                   'host.amd64.evidence.collection', 'host.amd64.evidence.block')
PREEMPTION_CHECK = 'host.amd64.evidence.native-preemption'
UNREACHED_CHECK = 'host.amd64.evidence.unreached-scenarios'


# What the root service's filesystem demonstration does on each disk recipe
# (DiskRecipes.FsDemo), as both ISAs run it.
FsDemo = DiskRecipes.FsDemo
FS_DEMOS = DiskRecipes.FS_DEMOS


# The least frames the network demonstration's NIC driver copies (Lan.py).
NIC_LEAST_COPIES = Lan.LEAST_COPIES


def captured_frames(context) -> int:
    """The frames the runner's LAN peer captured, both ways; zero without a
    capture."""
    try:
        frames, _ = Lan.load_capture(context.artifact(Lan.PCAP_ARTIFACT).parent)
    except (OSError, ValueError):
        return 0
    return len(frames)


def _all_unmapped(context, management, constants, expectations) -> bool:
    """Whether MemoryUnmap took every mapping that is not the drains', the
    self-test's run included, and so the pager's window is empty."""
    if not context.case.guest:
        return True
    scenario = Amd64Evidence.ObjectManagementScenario(
        self_test=context.case.guest, mapping_test=context.case.guest,
        return_boundary=Guest.AMD64_RETURN_BOUNDARY_LINE in Guest.channel_lines(context.console.lines),
        frame_rights=Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(context.console.lines),
        component=_component_scenario(context.case, constants, expectations.images, captured_frames(context),
                                      inventory=any(line.startswith("# devices:") for line in Guest.channel_lines(context.console.lines))))
    return 0 <= management.Unmappings - scenario.unmappings <= scenario.component.copies_beyond


def _component_scenario(case, constants: Amd64Layout.HeaderConstants,
                        images: tuple[tuple[str, tuple[int, int]],...],
                        lan_frames: int = 0, *, inventory: bool = False) -> Amd64Evidence.ComponentScenario:
    """The test firmware's root service: every component in a ResourceDomain
    of its own, drained at its teardown; behaviours in their tags' order, the
    display broker last as it is torn down last."""
    fs = FS_DEMOS[case.disk]
    filesystems = ('filesystem-crash',) * fs.crashes + ('filesystem',) * (fs.services - fs.crashes)
    width, height = case.expected_display or (1280, 800)
    scanout_pages = (width * height * 4 + 4095) // 4096
    return Amd64Evidence.ComponentScenario(built=case.guest,
                                           distributed=case.cpus > 1 and any(name == 'smp' for name, _ in images),
                                           smp_workers=UserProcessors.worker_count() if case.guest and case.cpus == 4 and any(name == 'smp' for name, _ in images) else 0,
                                           scanout_mappings=(scanout_pages + 511) // 512,
                                           behaviors=('crash', 'hang', 'report', 'account', 'driver', 'echo',
                                                      'driver-crash', 'driver-service', 'fs-driver') + filesystems
                                           + ('nic-driver', 'network', 'block-driver', 'directory', 'service',
                                              'client', 'service', 'interpret', 'input', 'input-client')
                                           + (('input', 'input-client', 'ps2') if case.i8042 else ()) + ('display',),
                                           fs_root_maps=fs.root_maps, fs_service_maps=fs.service_maps, inventory=inventory,
                                           nic_copies_least=NIC_LEAST_COPIES if case.guest else 0,
                                           nic_copies_most=lan_frames if case.guest else 0,
                                           drained=case.guest,
                                           exit_code=constants.component_exit_code,
                                           interpreted_exit_code=constants.interpreted_exit_code,
                                           data_start=constants.component_stack_start,
                                           data_pages=constants.component_stack_pages + 1,
                                           slots=constants.domain_slots,
                                           disk=case.disk, images=images or Amd64Evidence.ComponentScenario(
                                               built=False).images)


def _check_evidence(context, observation, bsp, record, expectations: Expectations, preemption: bool) -> None:
    last = PREEMPTION_CHECK if preemption else UNREACHED_CHECK
    if record is None:
        for name in EVIDENCE_CHECKS + (last,):
            context.check(name, False, 'no evidence record')
        return
    clock_problems = ProcessorClocks.problems(record.Clocks, context.case.cpus)
    context.check('host.amd64.evidence.clocks', not clock_problems,
                  '\n'.join(clock_problems) if clock_problems else ProcessorClocks.describe(record.Clocks))
    case, constants, outcomes = context.case, expectations.constants, expectations.outcomes
    component = _component_scenario(case, constants, expectations.images, captured_frames(context),
                                    inventory=any(line.startswith("# devices:") for line in Guest.channel_lines(context.console.lines)))
    component = dataclasses.replace(component, image_pages=expectations.image_pages or component.image_pages)
    if 'UserProcessors' not in record:  # A recorded pre-step-(4) build's own schema.
        component = dataclasses.replace(component, distributed=False, smp_workers=0)
    if component.built:
        context.check_problems('host.amd64.evidence.shared-images', SharedImages.problems(
            context.console.lines, component.behaviors, component.images, extra={UserProcessors.SMP_TAG: 'smp'} if component.smp_workers else None))
    if 'UserProcessors' in record:
        context.check_problems('host.amd64.evidence.user-processors', UserProcessors.problems(
            record.UserProcessors, case.cpus, amd64=True, constants=constants,
            pool=record.Runtime.PoolPhysicalStart, spinners=bool(component.smp_workers)))
    query_pci = observation.query('query-pci')
    ahci, pool = record.Ahci, record.Runtime.PoolPhysicalStart
    disk = Amd64Evidence.disk_expectation(context.artifact('Disk.raw')) if case.disk else None
    buffer, buffer_problems = None, []
    if ahci.ReadBufferPhysicalAddress and 0 < ahci.ReadBufferSizeBytes <= MAXIMUM_BUFFER_BYTES:
        buffer, buffer_problems = _read(observation.physical, ahci.ReadBufferPhysicalAddress,
                                        ahci.ReadBufferSizeBytes)
    ahci_detail = Amd64Evidence.describe_ahci(ahci, outcomes)
    checks = (
        (Amd64Evidence.runtime_problems(record.Runtime, constants, bsp.cr3),
         Amd64Evidence.describe_runtime(record.Runtime)),
        (Amd64Evidence.processor_problems(record, observation.query('query-cpus-fast'), case.cpus),
         '%d processors' % record.Runtime.ProcessorCount),
        (Amd64Evidence.pci_ecam_problems(record.Pci, query_pci), Amd64Evidence.describe_pci(record.Pci)),
        (Amd64Evidence.pci_ahci_problems(record.Pci, query_pci, constants), Amd64Evidence.describe_pci(record.Pci)),
        (Amd64Evidence.ahci_identify_problems(ahci, disk, outcomes), ahci_detail),
        (buffer_problems + Amd64Evidence.ahci_read_problems(
            ahci, disk, buffer, outcomes, constants.pool_page(pool, constants.ahci_read_data_page)), ahci_detail),
        (Amd64Evidence.application_processors_problems(record.ApplicationProcessors, case.cpus, constants),
         Amd64Evidence.describe_application_processors(record.ApplicationProcessors)),
        (Amd64Evidence.shootdown_problems(record.SharedRootShootdown, case.cpus, constants, pool),
         Amd64Evidence.describe_shootdown(record.SharedRootShootdown)),
        (Amd64Evidence.smp_ipi_problems(record.Smp, case.cpus), 'shootdown IPIs sent and taken by every processor'),
        (Amd64Evidence.wait_problems(record.Wait, constants), Amd64Evidence.describe_wait(record.Wait)),
        (Amd64Evidence.user_preemption_problems(record.UserPreemption,
                                                Amd64Evidence.UserPreemptionScenario(spinner=case.guest)),
         Amd64Evidence.describe_user_preemption(record.UserPreemption)),
        (Amd64Evidence.supervision_problems(record.Supervision, Amd64Evidence.SupervisionScenario(
            spinner_faults=case.guest, exit_code=constants.fault_terminated_exit_code, thread_faults=case.guest,
            component_crashes=component.crashes,
            return_boundary=Guest.AMD64_RETURN_BOUNDARY_LINE in Guest.channel_lines(context.console.lines),
            entry_state=sum(line.startswith(Guest.AMD64_ENTRY_STATE_PREFIX)
                for line in Guest.channel_lines(context.console.lines)))),
         Amd64Evidence.describe_supervision(record.Supervision)),
        (Amd64Evidence.object_management_problems(record.ObjectManagement,
                                                  Amd64Evidence.ObjectManagementScenario(
                                                      self_test=case.guest, memory_test=case.guest,
                                                      mapping_test=case.guest,
                                                      return_boundary=Guest.AMD64_RETURN_BOUNDARY_LINE in Guest.channel_lines(context.console.lines),
                                                      frame_rights=Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(context.console.lines),
                                                      frame_pool_pages=record.MemoryImport.ManagedPages,
                                                      imported=True,
                                                      frame_pool=frame_pool(constants, pool),
                                                      window_start=constants.mapping_window_start,
                                                      scrubbed=case.guest,
                                                      thread_bytes=record.Threads.SlotMetadataBytes,
                                                      component=component,
                                                      leaked_bytes=Amd64Evidence.leaked_bytes(record.Components),
                                                      domain_bytes=Amd64Evidence.domain_bytes(record.Components))),
         Amd64Evidence.describe_object_management(record.ObjectManagement)),
        (Amd64Evidence.thread_problems(record.Threads, Amd64Evidence.ThreadScenario(
            self_test=case.guest, terminated_exit_code=constants.fault_terminated_exit_code,
            killed_exit_code=constants.killed_exit_code, waiting_exit_code=constants.waiting_exit_code,
            slots=constants.thread_slots, component=component,
            return_boundary=Guest.AMD64_RETURN_BOUNDARY_LINE in Guest.channel_lines(context.console.lines),
            entry_state=sum(line.startswith(Guest.AMD64_ENTRY_STATE_PREFIX)
                for line in Guest.channel_lines(context.console.lines)))),
         Amd64Evidence.describe_threads(record.Threads)),
        (Amd64Evidence.component_problems(record.Components, component),
         Amd64Evidence.describe_components(record.Components)),
        (Amd64Evidence.metadata_problems(record.Metadata, slab_first_page=constants.arena_slab_first_page,
            maximum_pages=constants.arena_maximum_pages, root_arena_pages=constants.root_arena_pages,
            registry_capacity=constants.registry_capacity, frame_slots=constants.frame_slots,
            memory_object_slots=constants.memory_object_slots),
         Amd64Evidence.describe_metadata(record.Metadata)),
        (Amd64Evidence.service_problems(record.Services, Amd64Evidence.ServiceScenario(self_test=case.guest,
                                                                                       component=component, legacy_lines=int(case.i8042))),
         Amd64Evidence.describe_services(record.Services)),
        (Amd64Evidence.iommu_backend_problems(record.IommuBackend, Amd64Evidence.IommuScenario(
            present=case.guest, amd_vi=case.iommu == 'AmdVi'))
         + Amd64Evidence.iommu_problems(record.Iommu, Amd64Evidence.IommuScenario(
            present=case.guest, amd_vi=case.iommu == 'AmdVi',
            functions=len(Amd64Evidence.qemu_functions(query_pci)) - (2 if case.iommu == 'AmdVi' else 0),
            swept=sum(f.class_code != 0x0604 and f.class_code >> 8 != 3
                      for f in Amd64Evidence.qemu_functions(query_pci)) - (1 if case.iommu == 'AmdVi' else 0),
            driver=Amd64Evidence.driver_requester(query_pci) if case.guest else None,
            domains=component.drivers + component.driver_services + component.block_served + component.fs_served
            + component.nic_drivers))
         + Amd64Evidence.iommu_topology_problems(record.IommuTopology, Amd64Evidence.IommuScenario(
             present=case.guest, amd_vi=case.iommu == 'AmdVi',
            functions=len(Amd64Evidence.qemu_functions(query_pci)) - (2 if case.iommu == 'AmdVi' else 0),
             swept=sum(f.class_code != 0x0604 and f.class_code >> 8 != 3
                       for f in Amd64Evidence.qemu_functions(query_pci)) - (1 if case.iommu == 'AmdVi' else 0),
             driver=Amd64Evidence.driver_requester(query_pci) if case.guest else None)),
         Amd64Evidence.describe_iommu(record.Iommu)),
        (Amd64Evidence.interrupt_problems(record.Interrupts, Amd64Evidence.InterruptScenario(
            drivers=component.drivers, driver_services=component.driver_services,
            recoveries=component.recoveries, block_served=component.block_served, fs_served=component.fs_served,
            nic_drivers=component.nic_drivers, legacy_lines=int(case.guest and case.i8042), ps2=component.ps2)),
         Amd64Evidence.describe_interrupts(record.Interrupts)),
        # Each storage domain parks active and retired mappings within the core's bound. Services
        # and NIC keep their fixed DMA arrangements. Every parked pin must still be released.
        (Amd64Evidence.device_control_problems(record.DeviceControl, Amd64Evidence.DeviceControlScenario(
            assignments=component.device_quiesces,
            parked_pins=2 * (component.driver_services + component.block_served) + component.fs_served
            + 3 * component.nic_drivers,
            parked_pins_most=DmaLifetime.maximum_pins(component.block_served + component.fs_served,
                2 * component.driver_services + 3 * component.nic_drivers))),
         Amd64Evidence.describe_device_control(record.DeviceControl)),
        (Amd64Evidence.collection_problems(record), Amd64Evidence.describe_collection(record.Collection)),
        (Amd64Evidence.block_problems(record, Amd64Evidence.BlockScenario(
            token=Machine.DISK_TOKEN if component.block_served else None)),
         Amd64Evidence.describe_block(record.Block)),
        (Amd64Evidence.native_preemption_problems(record.NativePreemption, constants),
         Amd64Evidence.describe_native_preemption(record.NativePreemption)) if preemption
        else (Amd64Evidence.unreached_problems(record), ''))
    resident = (Amd64Init.problems(record, constants, expectations, case.i8042,
                                  frame_pool(constants, pool), pathlib.Path(context.target.root))
                if not case.guest else {})
    if not case.guest:
        behaviors = ('input', 'ps2', 'display') if case.i8042 else ('display',)
        context.check_problems('host.amd64.evidence.shared-images', SharedImages.problems(
            context.console.lines, behaviors, expectations.images))
    if component.accounts:
        from AsteriaVm.Observers import ApplicationLaunch
        context.check_problems('host.amd64.evidence.application-launch', ApplicationLaunch.problems(
            record.ApplicationLaunch, context.target.root, component.accounts, channels=int(bool(component.smp_workers))))
    for name, (problems, note) in zip(EVIDENCE_CHECKS + (last,), checks, strict=True):
        problems = resident.get(name, problems)
        context.check(name, not problems, '\n'.join(problems) if problems else note)
