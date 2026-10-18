# SPDX-License-Identifier: Apache-2.0
"""IA-64 synthetic user probe: Ia64.SyntheticProbe.

The probe build replaces the initial user image by a kernel-embedded PL3
payload that runs Query, an IPC loopback and a timer-backed WaitSetWait in a
child frame, then checks its parent frame and ends in a loop. The driver
waits for the probe's last stage event (`IA64-PRIVATE-USER-ROOT-RESUME`)
and the stable loop, stops the guest, captures its state and the kernel's
evidence record, and judges them:

- lifecycle: the probe's boot stages once and in order, none of the default
  image's or the preemption proof's;
- host: loop reached, stopped IP/PSR, no NaT, region registers, RSE,
  user TCs, kernel TRs (coverage, W^X, capacity, header gap, bootstrap
  and run-time memory reservations), IVA and the live IVT, no pending
  exception;
- evidence (`Observers.Ia64Evidence`): processors, PCI, no disk, the AP
  runtime and shared-root shootdown as for the default image, and the
  probe's kernel-captured timer evidence (first interrupt, TimerQueue,
  wait-resume interrupt);
- probe (`Observers.Ia64ProbeUser`): what the payload itself produced and
  keeps in its registers and user memory (registers, frame, stack page,
  wait response, RSE spill). The payload reports nothing itself, so the
  host reads it until it reports TAP.

The bootstrap reservation's base is derived from the run
(see `Observers.Ia64Translation`).
"""
from __future__ import annotations

from AsteriaVm import Ia64Layout
from AsteriaVm.Drivers import Ia64Default
from AsteriaVm.Ia64Layout import HeaderConstants
from AsteriaVm.Observers import Ia64Capture, Ia64Cpu, Ia64Evidence, Ia64Image, Ia64ProbeUser, Ia64Translation
from AsteriaVm.Observers.Ia64Translation import access

STACK_PAGE_DUMP = 'UserStackPage'
RSE_SENTINEL_DUMP = 'UserRseSentinel'
RSE_PARENT_SPILL_DUMP = 'UserRseParentSpill'
USER_REGION = 1                   # every probe page lies in region 1 (Ia64InitialUserVirtualRegion)
RELEVANT_GENERAL_REGISTERS = 37   # r1-r37: statics, the scratch set and the parent's frame
# The probe's final predicate register file: p0 plus p6, the result of the
# payload's last compare (Ia64UserExecutionProbePayload.S).
PROBE_PREDICATES = 0x41
FINAL_STAGE = 'IA64-PRIVATE-USER-ROOT-RESUME'


def loop_address(expected: Ia64Default.Expectation) -> int:
    """The probe's post-syscall loop, at its payload offset in the user code page."""
    facts = expected.facts
    offset = (facts.kernel(Ia64Layout.PROBE_LOOP_SYMBOL).value
              - facts.kernel(Ia64Layout.PROBE_PAYLOAD_SYMBOL).value)
    return expected.constants.probe_code + offset


def parent_pfs(frame: int) -> int:
    """AR.PFS after returning into a PL3 frame of `frame` locals (no outputs,
    rotation or epilogue): ppl (bits 62-63) = 3, pfm.sof = pfm.sol = frame."""
    return 3 << 62 | frame << 7 | frame


def kernel_translations(constants: HeaderConstants, runtime_memory_page: int) -> Ia64Translation.KernelTranslations:
    read_write = access('kernel RW', constants.pte_kernel_rw)
    read_execute = access('kernel RX', constants.pte_kernel_rx)
    return Ia64Translation.KernelTranslations(
        rid=constants.kernel_rid,
        page_sizes=Ia64Layout.INSERTABLE_PAGE_SIZES, instruction_capacity=Ia64Layout.INSTRUCTION_TR_CAPACITY,
        data_capacity=Ia64Layout.DATA_TR_CAPACITY, firmware_instruction_registers=Ia64Layout.FIRMWARE_INSTRUCTION_TRS,
        exception_deferral=constants.pte_exception_deferral, text=read_execute,
        read_only=access('kernel R', constants.pte_kernel_r), read_write=read_write,
        runtime_memory_page=runtime_memory_page)


def user_pages(constants: HeaderConstants) -> tuple[Ia64Translation.UserPage, ...]:
    page_mask = ~(constants.page_size - 1)
    read_write = access('user RW', constants.pte_user_rw)
    return (Ia64Translation.UserPage('code', 'ITLB', constants.probe_code, access('user RX', constants.pte_user_rx)),
            Ia64Translation.UserPage('memory stack', 'DTLB', constants.probe_initial_stack_pointer & page_mask,
                                     read_write),
            Ia64Translation.UserPage('RSE parent spill', 'DTLB', constants.probe_rse, read_write),
            Ia64Translation.UserPage('RSE result', 'DTLB', constants.probe_rse_sentinel & page_mask, read_write))


def probe_page(expected: Ia64Default.Expectation) -> Ia64ProbeUser.ProbePage:
    constants, facts = expected.constants, expected.facts
    base = constants.probe_stack_page
    frame = bytes.fromhex(facts.kernel_objects[Ia64Layout.PROBE_CANONICAL_FRAME_OBJECT])
    # The probe Endpoint's delivery: the initial image's canonical AIPD header
    # (both images receive the same wire format) with the probe Endpoint's rights.
    user_delivery = bytes.fromhex(facts.user_objects[Ia64Layout.USER_CANONICAL_DELIVERY_OBJECT])
    header = bytearray(user_delivery[:len(user_delivery) - len(bytes.fromhex(
        facts.user_objects[Ia64Layout.USER_CANONICAL_FRAME_OBJECT]))])
    offset = constants.delivery_rights_offset
    header[offset:offset + 2] = constants.probe_endpoint_rights.to_bytes(2, 'little')
    sentinel = constants.probe_sentinel
    evidence = (Ia64ProbeUser.response(constants.probe_ipc_information, constants.probe_send_bytes,
                                       constants.probe_ipc_error)
                + Ia64ProbeUser.response(constants.probe_ipc_information, constants.probe_receive_bytes,
                                         constants.probe_ipc_error)
                + Ia64ProbeUser.word(sentinel))
    return Ia64ProbeUser.ProbePage(
        size=constants.page_size, bootstrap=constants.probe_bootstrap - base,
        bootstrap_authenticator=constants.probe_bootstrap_authenticator,
        bootstrap_frame=constants.probe_bootstrap_frame,
        bootstrap_waitset_selector=constants.probe_bootstrap_waitset_selector,
        bootstrap_waitset_authenticator=constants.probe_bootstrap_waitset_authenticator,
        endpoint_selector=constants.probe_endpoint_selector, waitset_selector=constants.probe_waitset_selector,
        canonical_frame=frame, ipc_evidence=constants.probe_ipc_evidence - base, ipc_evidence_bytes=evidence,
        delivery=constants.probe_delivery - base, delivery_bytes=bytes(header) + frame,
        wait_response=constants.probe_wait_response - base,
        wait_response_bytes=Ia64ProbeUser.response(constants.probe_wait_tag, constants.probe_wait_sequence),
        query_response=constants.probe_query_response - base,
        query_response_bytes=Ia64ProbeUser.response(constants.probe_query_information, constants.probe_lease,
                                                    constants.probe_query_error),
        stack_sentinel=constants.probe_initial_stack_pointer - base,
        stack_sentinel_bytes=Ia64ProbeUser.word(sentinel))


def probe_registers(constants: HeaderConstants) -> tuple[dict[int, int | None], dict[int, int]]:
    """The payload's final r1-r37 (everything not listed is cleared) and b0-b7."""
    general: dict[int, int | None] = {index: 0 for index in range(1, RELEVANT_GENERAL_REGISTERS + 1)}
    general.update({4: constants.probe_domain_selector, 5: None, 8: constants.probe_query_error,
                    9: constants.probe_query_information, 10: constants.probe_lease,
                    12: constants.probe_initial_stack_pointer})
    for index in range(constants.probe_sentinel_count):
        general[32 + index] = constants.probe_sentinel + index
    branch = {index: 0 for index in range(8)}
    branch[0] = constants.probe_code + constants.probe_parent_resume
    return general, branch


def probe_evidence_checks(observation: Ia64Capture.Observation, expected: Ia64Default.Expectation,
                          state) -> list[Ia64Default.Check]:
    """Local-timer, TimerQueue and WaitSet evidence from the kernel's record."""
    names = ('host.ia64.evidence.probe-local-timer', 'host.ia64.evidence.probe-timer-queue',
             'host.ia64.evidence.probe-wait-timer')
    evidence, problems = Ia64Default.evidence_record(observation, expected)
    leaf = Ia64Default.poll_leaf(observation, expected)
    if leaf is None:
        problems = problems + Ia64Image.loaded_image(observation.lines)[1]
    if evidence is None or leaf is None:
        return [(name, problems) for name in names]
    timer = Ia64Evidence.TimerExpectation.of(expected.constants, leaf)
    probe = evidence.UserProbe
    return [(names[0], Ia64Evidence.probe_local_timer_problems(probe, state, timer)),
            (names[1], Ia64Evidence.probe_timer_queue_problems(probe, expected.constants)),
            (names[2], Ia64Evidence.probe_wait_timer_problems(probe, timer))]


def evaluate(observation: Ia64Capture.Observation, expected: Ia64Default.Expectation) -> list[Ia64Default.Check]:
    constants, facts = expected.constants, expected.facts
    loop = loop_address(expected)
    state = Ia64Default.cpu_state(observation)
    checks = Ia64Default.loop_checks('host.ia64.cpu.probe-loop-reached', observation, loop, constants.probe_user_psr)
    checks += Ia64Default.stage_checks(observation, expected, Ia64Default.PROBE_STAGES)
    checks += Ia64Default.evidence_checks(observation, expected, ('NativePreemption.Published',
                                                                 'UserPreemption.Published',
                                                                 'ObjectManagement.Published',
                                                                 'Threads.Published'))
    checks += probe_evidence_checks(observation, expected, state)
    checks.append(('host.ia64.cpu.stopped-state', Ia64Cpu.stopped_state_problems(state, loop, constants.probe_user_psr)))
    checks.append(('host.ia64.cpu.register-nat', Ia64Cpu.register_nat_problems(state)))
    general, branch = probe_registers(constants)
    checks.append(('host.ia64.probe.user-registers',
                   Ia64ProbeUser.register_problems(state, general, branch, PROBE_PREDICATES)))
    shift = constants.region_page_shift
    region_registers = {0: Ia64Translation.region_register(constants.kernel_rid, shift)}
    region_registers.update({region: Ia64Translation.region_register(constants.user_rid(region), shift)
                             for region in (5, 6, 7)})
    checks.append(('host.ia64.translation.region-registers',
                   Ia64Translation.region_register_problems(state, region_registers)))
    checks.append(('host.ia64.cpu.rse-state',
                   Ia64Cpu.rse_problems(state, backing_store=constants.probe_rse, rsc=constants.user_rsc)))
    checks.append(('host.ia64.probe.frame', Ia64ProbeUser.probe_frame_problems(
        state, Ia64ProbeUser.ProbeFrame(constants.probe_sentinel_count, parent_pfs(constants.probe_sentinel_count),
                                        constants.user_fpsr, constants.user_rsc))))
    checks.append(('host.ia64.translation.user-tcs', Ia64Translation.user_translation_cache_problems(
        state, user_pages(constants), constants.user_rid(USER_REGION), constants.pte_exception_deferral)))
    kernel = Ia64Default.kernel_layout(observation.lines, facts)
    ivt_size = facts.kernel('AsteriaIa64RuntimeInterruptVectorTableImage').size
    if kernel is None:
        checks.append(('host.ia64.translation.kernel-trs', Ia64Image.loaded_image(observation.lines)[1]))
    else:
        problems, base = Ia64Translation.kernel_translation_problems(
            state, kernel, kernel_translations(constants, expected.runtime_memory_page))
        problems += Ia64Translation.kernel_section_problems(facts, ivt_size)
        checks.append(('host.ia64.translation.kernel-trs', problems,
                       ('runtime page at %#x (bootstrap consolidated)' % base)
                       if base is not None else ''))
    checks.append(('host.ia64.cpu.no-pending-exception', Ia64Cpu.pending_exception_problems(state)))
    checks += Ia64Default.ivt_checks(observation, expected, state)
    page = observation.memory.get(STACK_PAGE_DUMP)
    layout = probe_page(expected)
    checks.append(('host.ia64.probe.stack-page',
                   ['no stack-page dump'] if page is None else Ia64ProbeUser.probe_page_problems(page, layout)))
    checks.append(('host.ia64.probe.wait-response',
                   ['no stack-page dump'] if page is None else Ia64ProbeUser.wait_response_problems(page, layout)))
    sentinel = constants.probe_sentinel
    checks.append(('host.ia64.probe.rse-sentinel', Ia64ProbeUser.memory_problems(
        observation.memory, {RSE_SENTINEL_DUMP: Ia64ProbeUser.word(sentinel)})))
    checks.append(('host.ia64.probe.rse-parent-spill', Ia64ProbeUser.memory_problems(
        observation.memory, {RSE_PARENT_SPILL_DUMP: b''.join(Ia64ProbeUser.word(sentinel + index)
                                                             for index in range(constants.probe_sentinel_count))})))
    checks += Ia64Default.native_kernel_translation_checks(observation, expected, include_bsp=False)
    return checks


def run(context) -> None:
    facts = Ia64Image.ImageFacts.from_target(context.target, context.case.firmware)
    expected = Ia64Default.expectation(context.case, context.target, facts, context.artifact('Disk.raw'))
    constants = expected.constants
    Ia64Default.gate(context, FINAL_STAGE, Ia64Default.BOOT_TIMEOUT)
    loop = Ia64Default.wait_for_user_loop(context, loop_address(expected), constants.probe_user_psr)
    context.stop()
    requests = Ia64Default.kernel_requests(expected, Ia64Default.kernel_layout(context.console.lines, facts))
    spill = constants.probe_sentinel_count * 8
    requests.update({STACK_PAGE_DUMP: ('virtual', constants.probe_stack_page, constants.page_size),
                     RSE_SENTINEL_DUMP: ('virtual', constants.probe_rse_sentinel, 8),
                     RSE_PARENT_SPILL_DUMP: ('virtual', constants.probe_rse, spill)})
    observation, errors = Ia64Default.capture(context, requests, processors=expected.cpus)
    errors.update(Ia64Default.capture_evidence(context, observation, expected))
    observation.loop = loop
    Ia64Capture.save(observation, context.run_directory)
    Ia64Default.record(context, evaluate(observation, expected), errors)
