# SPDX-License-Identifier: Apache-2.0
"""IA-64 processor state: where the BSP runs, and the privileged state around it.

PSR, IVA, the RSE application registers and the pending-exception state are
privileged, so user code cannot report them; these checks read them from the
stopped CPU (`Ia64State`). Expected values are parameters: the caller
derives them from the build tree (ELF symbols) and the IA-64 constants.
"""
from __future__ import annotations

import dataclasses
from typing import Callable

from AsteriaVm.Observers.Ia64State import CpuState, StateError, schema_problems
from AsteriaVm.Qmp import wait_until

# PSR.ri (bits 41-42): the slot of the next instruction in the bundle
# (IA-64 SDM Vol. 2, PSR). An interrupted or stopped CPU may sit at any
# architected slot 0-2 of the loop bundle; 3 is reserved.
PSR_RI_SHIFT = 41
PSR_RI_MASK = 3 << PSR_RI_SHIFT
MAXIMUM_SLOT = 2
# PSR.mfl and PSR.mfh (bits 4 and 5): set by the processor once the thread
# wrote f2-f31 or f32-f127, which the boot domains may do with f32-f127
# they record the thread's own use.
PSR_MODIFIED_MASK = (1 << 4) | (1 << 5)


def loop_state_problems(ip: int, psr: int, expected_ip: int, expected_psr: int) -> list[str]:
    """Validate the user-loop processor state:
    IP is the loop bundle, PSR.ri names an architected slot, and every other PSR
    bit but the floating-point modified bits equals the launch PSR (PL3,
    translated, interrupts enabled,...)."""
    problems = []
    if ip != expected_ip:
        problems.append('IP %#018x, expected the loop bundle %#018x' % (ip, expected_ip))
    slot = (psr & PSR_RI_MASK) >> PSR_RI_SHIFT
    if slot > MAXIMUM_SLOT:
        problems.append('PSR.ri %d names no instruction slot' % slot)
    ignored = PSR_RI_MASK | PSR_MODIFIED_MASK
    if psr & ~ignored != expected_psr & ~PSR_MODIFIED_MASK:
        problems.append('PSR %#018x (ri, mfl and mfh masked %#018x), expected %#018x' % (
            psr, psr & ~ignored, expected_psr))
    return problems


@dataclasses.dataclass
class LoopWait:
    reached: bool
    polls: int
    matches: int
    last: tuple[int, int, int] | None      # (IP, PSR, HALTED) of the last sample
    reason: str = ''

    def describe(self, expected_ip: int, expected_psr: int) -> str:
        if self.last is None:
            return 'no processor sample (%s)' % (self.reason or 'none taken')
        ip, psr, halted = self.last
        state = 'reached' if self.reached else 'not reached'
        text = '%s after %d polls: last IP %#018x PSR %#018x HALTED %d; expected IP %#018x with PSR %#018x' % (
            state, self.polls, ip, psr, halted, expected_ip, expected_psr)
        return text + ('' if not self.reason else ' (%s)' % self.reason)


def wait_for_loop(sample: Callable[[], tuple[int, int, int]], expected_ip: int, expected_psr: int, *,
                  samples: int, timeout: float, interval: float = 0.05,
                  abort: Callable[[], str | None] | None = None) -> LoopWait:
    """Polls the BSP until `samples` consecutive samples sit in the loop.

    `sample` returns (IP, PSR, HALTED) of the running CPU. Bounded polling
    through `Qmp.wait_until` requires the requested loop and HALTED 0;
    `abort` ends the wait early with a reason (a kernel failure line, QEMU
    exiting).
    """
    wait = LoopWait(False, 0, 0, None)

    def settled() -> bool:
        if abort is not None:
            reason = abort()
            if reason:
                wait.reason = reason
                return True
        ip, psr, halted = sample()
        wait.polls += 1
        wait.last = (ip, psr, halted)
        if halted == 0 and not loop_state_problems(ip, psr, expected_ip, expected_psr):
            wait.matches += 1
        else:
            wait.matches = 0
        wait.reached = wait.matches >= samples
        return wait.reached

    if not wait_until(settled, timeout, interval) and not wait.reason:
        wait.reason = 'deadline of %.1f s passed' % timeout
    return wait


def sample_problems(wait: LoopWait, expected_ip: int, expected_psr: int) -> list[str]:
    """The wait ended with the required run of samples at this loop."""
    if not wait.reached or wait.last is None:
        return [wait.describe(expected_ip, expected_psr)]
    ip, psr, halted = wait.last
    problems = loop_state_problems(ip, psr, expected_ip, expected_psr)
    if halted:
        problems.append('the last sample is halted')
    return ['the last loop sample does not match: %s' % problem for problem in problems]


def stopped_state_problems(state: CpuState, expected_ip: int, expected_psr: int) -> list[str]:
    """The stopped BSP is at the loop, running, and the machine-readable
    record agrees with the human one."""
    problems = schema_problems(state)
    try:
        human = state.get('IP')
        machine = state.get('META')
    except StateError as error:
        return problems + [str(error)]
    problems += loop_state_problems(human['ip'], human['psr'], expected_ip, expected_psr)
    if human['halted'] != 0:
        problems.append('the BSP is halted (HALTED %d)' % human['halted'])
    if (machine['ip'], machine['psr'], machine['halted']) != (human['ip'], human['psr'], human['halted']):
        problems.append('IA64STATE META ip=%#x psr=%#x halted=%d disagrees with IP %#x PSR %#x HALTED %d' % (
            machine['ip'], machine['psr'], machine['halted'], human['ip'], human['psr'], human['halted']))
    return problems


def pending_exception_problems(state: CpuState) -> list[str]:
    """No exception is pending on the stopped BSP (`IA64STATE EXCEPTION code=0`).

    QEMU's retained fault history (fault_code/ip/imm) is emulator-internal and
    not judged."""
    try:
        code = state.value('EXCEPTION', 'code')
    except StateError as error:
        return [str(error)]
    return [] if code == 0 else ['exception code %#x is pending' % code]


def register_nat_problems(state: CpuState) -> list[str]:
    """No general register carries a NaT bit (`IA64STATE GRNAT lo=0 hi=0`)."""
    try:
        record = state.get('GRNAT')
    except StateError as error:
        return [str(error)]
    if record['lo'] or record['hi']:
        return ['general-register NaT bits lo=%#018x hi=%#018x, expected none' % (record['lo'], record['hi'])]
    return []


# RNAT collection: the RSE stores the NaT collection word at the backing-store
# address whose bits 8:3 are all ones (IA-64 SDM Vol. 2, RSE).
RNAT_SLOT_OFFSET = 0x1F8


def rse_problems(state: CpuState, *, backing_store: int, rsc: int) -> list[str]:
    """The RSE sits at the empty start of the user backing
    store with nothing dirty (BSP == BSPSTORE), in the launch RSC mode, with no
    NaT collection pending.

    QEMU's partition counters (bol/dirty/clean/invalid) are model details and
    not judged."""
    try:
        frame = state.get('CFM:')
        rse = state.get('RSE')
    except StateError as error:
        return [str(error)]
    problems = []
    if frame['bsp'] != backing_store or frame['bspstore'] != backing_store:
        problems.append('BSP %#018x BSPSTORE %#018x, expected both at the backing-store start %#018x'
                        % (frame['bsp'], frame['bspstore'], backing_store))
    if rse['rsc'] != rsc:
        problems.append('RSC %#x, expected %#x' % (rse['rsc'], rsc))
    if rse['rnat'] != 0:
        problems.append('RNAT %#018x, expected 0' % rse['rnat'])
    if rse['rnat_addr'] != backing_store + RNAT_SLOT_OFFSET:
        problems.append('RNAT_ADDR %#018x, expected %#018x' % (rse['rnat_addr'], backing_store + RNAT_SLOT_OFFSET))
    if rse['rnat_defined'] != 1:
        problems.append('RNAT_DEFINED %#x, expected 1' % rse['rnat_defined'])
    return problems
