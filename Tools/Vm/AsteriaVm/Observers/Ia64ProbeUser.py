# SPDX-License-Identifier: Apache-2.0
"""The synthetic probe's user-visible results.

The kernel-embedded PL3 probe payload leaves its syscall responses, IPC
buffers, sentinels and final registers in its user memory and in the stopped
BSP, and reports nothing itself, so the host reads them: until the probe's
checks move into a guest test program that reports TAP,
the synthetic-probe case stays a host-observed case. Every value here is
visible to the program that produced it; the kernel's own observations of
the probe (its timer interrupts and TimerQueue transaction) are in the
evidence record instead (`Observers.Ia64Evidence`).

Expected values are built by the driver from the ELF (canonical wire
buffers) and the build's C++ constants (`AsteriaVm.Ia64Layout`).
"""
from __future__ import annotations

import dataclasses
import struct

from AsteriaVm.Observers.Ia64State import CpuState, StateError

SYSTEM_CALL_OK = 0          # SystemCallError::None (Source/Abi/SystemCallAbi.hpp)


def response(value0: int, value1: int, error: int = SYSTEM_CALL_OK) -> bytes:
    """A SystemCallResponse as user memory holds it: Error, Value0, Value1."""
    return struct.pack('<3Q', error, value0, value1)


def word(value: int) -> bytes:
    return struct.pack('<Q', value)


def _bytes_problems(observed: bytes, expected: bytes, what: str) -> list[str]:
    if observed == expected:
        return []
    if len(observed) != len(expected):
        return ['%s has %d bytes, expected %d' % (what, len(observed), len(expected))]
    offset = next(index for index, (left, right) in enumerate(zip(observed, expected)) if left != right)
    return ['%s differs from offset %#x: %s, expected %s' % (what, offset, observed.hex(), expected.hex())]


def memory_problems(observed: dict[str, bytes], expected: dict[str, bytes]) -> list[str]:
    """Each named dump equals its expected bytes."""
    problems = []
    for name, value in expected.items():
        if name not in observed:
            problems.append('no %s dump' % name)
        else:
            problems += _bytes_problems(observed[name], value, name)
    return problems


# The synthetic probe's evidence page -------------------------------------------

@dataclasses.dataclass(frozen=True)
class ProbePage:
    """Where the probe and its bootstrap record put things in the last stack
    page, as offsets into that page, and what they must hold."""
    size: int
    bootstrap: int
    bootstrap_authenticator: int     # offsets inside the bootstrap record
    bootstrap_frame: int
    bootstrap_waitset_selector: int
    bootstrap_waitset_authenticator: int
    endpoint_selector: int
    waitset_selector: int
    canonical_frame: bytes
    ipc_evidence: int
    ipc_evidence_bytes: bytes
    delivery: int
    delivery_bytes: bytes
    wait_response: int
    wait_response_bytes: bytes
    query_response: int
    query_response_bytes: bytes
    stack_sentinel: int
    stack_sentinel_bytes: bytes


def probe_page_problems(page: bytes, expected: ProbePage) -> list[str]:
    """The kernel staged the bootstrap
    record (Endpoint and WaitSet handles, canonical frame), and the probe's
    Query, IpcSend/IpcReceive evidence, delivery and stack sentinel are exact."""
    if len(page) != expected.size:
        return ['the stack page dump has %d bytes, expected %d' % (len(page), expected.size)]
    problems = []

    def u64(offset: int) -> int:
        return struct.unpack_from('<Q', page, offset)[0]

    bootstrap = expected.bootstrap
    if u64(bootstrap) != expected.endpoint_selector:
        problems.append('bootstrap Endpoint selector %#x, expected %#x' % (u64(bootstrap), expected.endpoint_selector))
    if u64(bootstrap + expected.bootstrap_authenticator) == 0:
        problems.append('the bootstrap Endpoint authenticator is zero')
    waitset = bootstrap + expected.bootstrap_waitset_selector
    if u64(waitset) != expected.waitset_selector:
        problems.append('bootstrap WaitSet selector %#x, expected %#x' % (u64(waitset), expected.waitset_selector))
    if u64(bootstrap + expected.bootstrap_waitset_authenticator) == 0:
        problems.append('the bootstrap WaitSet authenticator is zero')
    frame = bootstrap + expected.bootstrap_frame
    problems += _bytes_problems(page[frame:frame + len(expected.canonical_frame)], expected.canonical_frame,
                                'the bootstrap frame')
    for name, offset, value in (('the IPC evidence', expected.ipc_evidence, expected.ipc_evidence_bytes),
                                ('the IPC delivery', expected.delivery, expected.delivery_bytes),
                                ('the Query response', expected.query_response, expected.query_response_bytes),
                                ('the stack sentinel', expected.stack_sentinel, expected.stack_sentinel_bytes)):
        problems += _bytes_problems(page[offset:offset + len(value)], value, name)
    return problems


def wait_response_problems(page: bytes, expected: ProbePage) -> list[str]:
    """WaitSetWait returned the registered tag and sequence."""
    offset = expected.wait_response
    return _bytes_problems(page[offset:offset + len(expected.wait_response_bytes)], expected.wait_response_bytes,
                           'the WaitSetWait response')


# User registers ---------------------------------------------------------------

def register_problems(state: CpuState, general: dict[int, int | None], branch: dict[int, int] | None = None,
                      predicates: int | None = None) -> list[str]:
    """General registers (None: any non-zero value, e.g. an authenticator),
    each without NaT; branch registers; the predicate file."""
    problems = []
    try:
        for index, value in sorted(general.items()):
            observed, nat = state.general(index)
            if nat:
                problems.append('r%d carries a NaT bit' % index)
            if value is None:
                if observed == 0:
                    problems.append('r%d is zero, expected a non-zero value' % index)
            elif observed != value:
                problems.append('r%d = %#018x, expected %#018x' % (index, observed, value))
        for index, value in sorted((branch or {}).items()):
            observed = state.branch(index)
            if observed != value:
                problems.append('b%d = %#018x, expected %#018x' % (index, observed, value))
        if predicates is not None:
            observed = state.value('PR', 'value')
            if observed != predicates:
                problems.append('predicates %#018x, expected %#018x' % (observed, predicates))
    except StateError as error:
        problems.append(str(error))
    return problems


@dataclasses.dataclass(frozen=True)
class ProbeFrame:
    locals: int          # the parent frame's size and locals (sof = sol)
    pfs: int             # AR.PFS after the child returned
    fpsr: int
    rsc: int


def probe_frame_problems(state: CpuState, expected: ProbeFrame) -> list[str]:
    """After the child syscall frame returned, the CPU is back
    in the parent's four-local frame with the PL3 parent PFS, and the user
    application registers are the launch values."""
    try:
        human = state.get('CFM:')
        machine = state.get('CFM')
        registers = state.get('AR')
    except StateError as error:
        return [str(error)]
    frame = {'sof': expected.locals, 'sol': expected.locals, 'sor': 0, 'rrb_gr': 0, 'rrb_fr': 0, 'rrb_pr': 0}
    problems = ['CFM %s=%d, expected %d' % (name, human[name], value)
                for name, value in frame.items() if human[name] != value]
    problems += ['IA64STATE CFM %s=%d, expected %d' % (name, machine[name], value)
                 for name, value in frame.items() if machine[name] != value]
    if human['pfs'] != expected.pfs:
        problems.append('AR.PFS %#018x, expected %#018x' % (human['pfs'], expected.pfs))
    application = {'ccv': 0, 'unat': 0, 'fpsr': expected.fpsr, 'csd': 0, 'ssd': 0, 'rsc': expected.rsc,
                   'rnat': 0, 'pfs': expected.pfs}
    problems += ['IA64STATE AR %s=%#x, expected %#x' % (name, registers[name], value)
                 for name, value in application.items() if registers[name] != value]
    return problems
