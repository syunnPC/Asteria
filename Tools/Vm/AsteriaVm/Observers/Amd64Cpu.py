# SPDX-License-Identifier: Apache-2.0
"""AMD64 CPU state through native QMP (`dump-guest-memory`), and its checks.

QEMU has no QMP command that returns x86 registers, but `dump-guest-memory`
writes an ELF core whose notes carry, for every CPU, the Linux NT_PRSTATUS
register image and QEMU's own `QEMUCPUState` (general registers, RIP,
RFLAGS, all segment descriptors including the GDT/IDT bases, and CR0-CR4;
target/i386/arch_dump.c). The dump is restricted to one page of guest memory
(`begin`/`length`), so a capture costs about a millisecond. QEMU pauses a
running guest for the dump and resumes it afterwards; a stopped guest stays
stopped.

EFER is in neither note. Long mode is still attested: QEMU writes an
ELFCLASS64/EM_X86_64 core only when CPU 0 has EFER.LMA set
(`cpu_get_dump_info`), and every CPU's code segment must be a 64-bit one
(CS.L = 1, CS.D = 0) with CR0.PG and CR4.PAE set. EFER.NXE and EFER.SCE are
implied by other checks: NX leaves that are in use (the user data and the
kernel stacks) would fault on a reserved bit without NXE, and the user image
only completes its WaitSet waits through SYSCALL.
"""
from __future__ import annotations

import dataclasses
import pathlib
import struct
import time
from typing import Callable

from AsteriaVm import Build  # Noqa: F401 (puts Tools/Verify on sys.path)
from AsteriaVm.Qmp import wait_until
from AsteriaVerify import Disassembly

ELFCLASS64 = 2
ELFDATA2LSB = 1
EM_X86_64 = 62
PT_NOTE = 4
NT_PRSTATUS = 1
QEMU_NOTE_TYPE = 0
QEMUCPUSTATE_VERSION = 1

# Architectural bits (Intel SDM vol. 3, 2.5 and 3.4.5).
CR0_PE = 1 << 0
CR0_EM = 1 << 2
CR0_TS = 1 << 3
CR0_WP = 1 << 16
CR0_PG = 1 << 31
CR4_PAE = 1 << 5
CR4_LA57 = 1 << 12
CR4_FSGSBASE = 1 << 16
CR4_PCIDE = 1 << 17
RFLAGS_IF = 1 << 9
DESCRIPTOR_DPL_SHIFT = 13
DESCRIPTOR_L = 1 << 21
DESCRIPTOR_D = 1 << 22

_PRSTATUS_SIZE = 336
_PRSTATUS_PID_OFFSET = 32
_PRSTATUS_REGISTERS_OFFSET = 112
_PRSTATUS_REGISTERS = ('r15', 'r14', 'r13', 'r12', 'rbp', 'rbx', 'r11', 'r10', 'r9', 'r8', 'rax', 'rcx',
                       'rdx', 'rsi', 'rdi', 'orig_rax', 'rip', 'cs', 'rflags', 'rsp', 'ss', 'fs_base',
                       'gs_base', 'ds', 'es', 'fs', 'gs')
_QEMU_REGISTERS = ('rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rsp', 'rbp', 'r8', 'r9', 'r10', 'r11', 'r12',
                   'r13', 'r14', 'r15', 'rip', 'rflags')
_QEMU_SEGMENTS = ('cs', 'ds', 'es', 'fs', 'gs', 'ss', 'ldt', 'tr', 'gdt', 'idt')
_QEMU_HEADER = struct.Struct('<II')
_QEMU_REGISTER_BLOCK = struct.Struct('<18Q')
_QEMU_SEGMENT = struct.Struct('<IIIIQ')
_QEMU_CONTROL = struct.Struct('<5Q')
_QEMU_MINIMUM_SIZE = (_QEMU_HEADER.size + _QEMU_REGISTER_BLOCK.size + len(_QEMU_SEGMENTS) * _QEMU_SEGMENT.size
                      + _QEMU_CONTROL.size)
# Every register of the QEMU note is also in NT_PRSTATUS; the two must agree.
_CROSS_CHECKED = _QEMU_REGISTERS


class CpuStateError(Exception):
    """The core file is malformed or its notes are inconsistent."""


@dataclasses.dataclass(frozen=True)
class Segment:
    selector: int
    limit: int
    flags: int
    base: int

    @property
    def dpl(self) -> int:
        return (self.flags >> DESCRIPTOR_DPL_SHIFT) & 3


@dataclasses.dataclass(frozen=True)
class CpuState:
    index: int
    registers: dict[str, int]
    segments: dict[str, Segment]
    control: tuple[int, int, int, int, int]
    kernel_gs_base: int | None = None

    @property
    def rip(self) -> int:
        return self.registers['rip']

    @property
    def rsp(self) -> int:
        return self.registers['rsp']

    @property
    def rflags(self) -> int:
        return self.registers['rflags']

    @property
    def rax(self) -> int:
        return self.registers['rax']

    @property
    def cr0(self) -> int:
        return self.control[0]

    @property
    def cr2(self) -> int:
        return self.control[2]

    @property
    def cr3(self) -> int:
        return self.control[3]

    @property
    def cr4(self) -> int:
        return self.control[4]

    @property
    def cs(self) -> Segment:
        return self.segments['cs']

    @property
    def ss(self) -> Segment:
        return self.segments['ss']

    @property
    def cpl(self) -> int:
        """QEMU's CPL (hflags) follows the DPL of the loaded stack segment."""
        return self.ss.dpl

    def describe(self) -> str:
        return ('CPU %d: RIP=%#x RSP=%#x RFLAGS=%#x RAX=%#x CS=%#x SS=%#x CPL=%d CR0=%#x CR2=%#x CR3=%#x CR4=%#x '
                'GDT=%#x IDT=%#x GS.base=%#x' % (
                    self.index, self.rip, self.rsp, self.rflags, self.rax, self.cs.selector, self.ss.selector,
                    self.cpl, self.cr0, self.cr2, self.cr3, self.cr4, self.segments['gdt'].base,
                    self.segments['idt'].base, self.segments['gs'].base))


@dataclasses.dataclass(frozen=True)
class CpuSnapshot:
    elf_class: int
    machine: int
    cpus: tuple[CpuState,...]

    @property
    def long_mode_attested(self) -> bool:
        """QEMU writes a 64-bit x86-64 core only if CPU 0 has EFER.LMA set."""
        return self.elf_class == ELFCLASS64 and self.machine == EM_X86_64


# Parsing -------------------------------------------------------------------

def _unpack(form: struct.Struct | str, data: bytes, offset: int):
    layout = form if isinstance(form, struct.Struct) else struct.Struct(form)
    if offset < 0 or offset + layout.size > len(data):
        raise CpuStateError('core file truncated at offset %#x' % offset)
    return layout.unpack_from(data, offset)


def _align4(value: int) -> int:
    return (value + 3) & ~3


def _notes(data: bytes):
    """Yields (name, type, descriptor bytes) of every note of every PT_NOTE segment."""
    (program_offset,) = _unpack('<Q', data, 32)
    entry_size, count = _unpack('<HH', data, 54)
    if count and entry_size != 56:
        raise CpuStateError('unexpected program header size %d' % entry_size)
    for index in range(count):
        kind, _, offset, _, _, size, _, _ = _unpack('<IIQQQQQQ', data, program_offset + index * 56)
        if kind != PT_NOTE:
            continue
        if offset + size > len(data):
            raise CpuStateError('note segment exceeds the core file')
        cursor, end = offset, offset + size
        while cursor + 12 <= end:
            name_size, descriptor_size, note_type = _unpack('<III', data, cursor)
            name_start = cursor + 12
            descriptor_start = name_start + _align4(name_size)
            following = descriptor_start + _align4(descriptor_size)
            if following > end:
                raise CpuStateError('note at %#x exceeds its segment' % cursor)
            name = data[name_start:name_start + name_size].rstrip(b'\0').decode('ascii', errors='replace')
            yield name, note_type, data[descriptor_start:descriptor_start + descriptor_size]
            cursor = following


def parse_core(data: bytes) -> CpuSnapshot:
    """Decodes the per-CPU notes of a QEMU `dump-guest-memory` ELF core."""
    if len(data) < 64 or data[:4] != b'\x7fELF':
        raise CpuStateError('not an ELF core file')
    elf_class, encoding = data[4], data[5]
    if encoding != ELFDATA2LSB:
        raise CpuStateError('core file is not little-endian')
    (machine,) = _unpack('<H', data, 18)
    if elf_class != ELFCLASS64:
        raise CpuStateError('QEMU wrote an ELF class %d core: CPU 0 is not in long mode' % elf_class)
    prstatus, qemu = [], []
    for name, note_type, descriptor in _notes(data):
        if name == 'CORE' and note_type == NT_PRSTATUS:
            if len(descriptor) != _PRSTATUS_SIZE:
                raise CpuStateError('NT_PRSTATUS note of %d bytes, expected %d' % (len(descriptor), _PRSTATUS_SIZE))
            (pid,) = struct.unpack_from('<I', descriptor, _PRSTATUS_PID_OFFSET)
            values = struct.unpack_from('<%dQ' % len(_PRSTATUS_REGISTERS), descriptor, _PRSTATUS_REGISTERS_OFFSET)
            prstatus.append((pid, dict(zip(_PRSTATUS_REGISTERS, values))))
        elif name == 'QEMU' and note_type == QEMU_NOTE_TYPE:
            qemu.append(_parse_qemu_note(descriptor))
    if not prstatus:
        raise CpuStateError('core file has no CPU notes')
    if len(prstatus) != len(qemu):
        raise CpuStateError('%d NT_PRSTATUS notes but %d QEMU notes' % (len(prstatus), len(qemu)))
    cpus = []
    # Dump.c writes all NT_PRSTATUS notes (pid = cpu_index + 1), then all QEMU notes, both in
    # CPU_FOREACH order.
    for position, ((pid, linux), (registers, segments, control, kernel_gs_base)) in enumerate(zip(prstatus, qemu)):
        if pid != position + 1:
            raise CpuStateError('NT_PRSTATUS note %d names CPU %d' % (position, pid - 1))
        for name in _CROSS_CHECKED:
            if linux[name] != registers[name]:
                raise CpuStateError('CPU %d: notes disagree on %s (%#x, %#x)' % (position, name, linux[name],
                                                                                 registers[name]))
        for name in ('cs', 'ss'):
            if linux[name] != segments[name].selector:
                raise CpuStateError('CPU %d: notes disagree on the %s selector' % (position, name.upper()))
        cpus.append(CpuState(position, registers, segments, control, kernel_gs_base))
    return CpuSnapshot(elf_class, machine, tuple(cpus))


def _parse_qemu_note(descriptor: bytes):
    if len(descriptor) < _QEMU_MINIMUM_SIZE:
        raise CpuStateError('QEMU CPU note of %d bytes is too short' % len(descriptor))
    version, size = _QEMU_HEADER.unpack_from(descriptor, 0)
    if version != QEMUCPUSTATE_VERSION or not _QEMU_MINIMUM_SIZE <= size <= len(descriptor):
        raise CpuStateError('unsupported QEMUCPUState version %d size %d' % (version, size))
    offset = _QEMU_HEADER.size
    registers = dict(zip(_QEMU_REGISTERS, _QEMU_REGISTER_BLOCK.unpack_from(descriptor, offset)))
    offset += _QEMU_REGISTER_BLOCK.size
    segments = {}
    for name in _QEMU_SEGMENTS:
        selector, limit, flags, _, base = _QEMU_SEGMENT.unpack_from(descriptor, offset)
        segments[name] = Segment(selector, limit, flags, base)
        offset += _QEMU_SEGMENT.size
    control = _QEMU_CONTROL.unpack_from(descriptor, offset)
    offset += _QEMU_CONTROL.size
    kernel_gs_base = struct.unpack_from('<Q', descriptor, offset)[0] if size >= offset + 8 else None
    return registers, segments, control, kernel_gs_base


# Capture -------------------------------------------------------------------

def capture(qmp, path: pathlib.Path) -> CpuSnapshot:
    """Writes the CPU notes (and physical page 0) to `path` and decodes them."""
    path.unlink(missing_ok=True)  # QEMU creates the file read-only and cannot reuse it
    qmp.execute('dump-guest-memory', {'paging': False, 'protocol': 'file:%s' % path, 'format': 'elf',
                                      'begin': 0, 'length': 4096})
    return parse_core(path.read_bytes())


def wait_for_idle(qmp, path: pathlib.Path, loop: 'IdleLoop', timeout: float, *,
                  abort: Callable[[], str | None] = lambda: None, interval: float = 0.02) -> tuple[bool, str]:
    """Polls CPU 0 until it executes `loop`, `abort` returns a reason, or `timeout` elapses.

    The loop must be one the guest enters only after its work is complete (the
    user image's idle loop follows `main`'s return), so one sample inside it
    settles the question; the caller then stops the guest and captures the
    state it judges. Returns (reached, detail)."""
    started = time.monotonic()
    samples: list[int] = []
    reasons: list[str] = []

    def settled() -> bool:
        reason = abort()
        if reason:
            reasons.append(reason)
            return True
        try:
            samples.append(capture(qmp, path).cpus[0].rip)
        except CpuStateError as error:
            reasons.append(str(error))
            return True
        return samples[-1] in loop.addresses

    wait_until(settled, timeout, interval)
    path.unlink(missing_ok=True)
    elapsed = time.monotonic() - started
    if not reasons and samples and samples[-1] in loop.addresses:
        return True, 'CPU 0 reached %s after %d sample(s), %.3f s' % (loop.describe(), len(samples), elapsed)
    return False, 'CPU 0 did not reach %s (%s after %.1f s); last RIPs: %s' % (
        loop.describe(), reasons[0] if reasons else 'timeout', elapsed,
        ', '.join('%#x' % rip for rip in samples[-5:]) or 'none')


# Build-tree inputs ---------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class IdleLoop:
    symbol: str
    addresses: frozenset[int]  # Every instruction start of the loop

    def describe(self) -> str:
        return '%s at {%s}' % (self.symbol, ', '.join('%#x' % address for address in sorted(self.addresses)))


def idle_loop(objdump: str, image: pathlib.Path, symbol: str) -> IdleLoop:
    """The instruction addresses of a closed `symbol:...; jmp symbol` loop."""
    decoded = Disassembly.disassemble(objdump, str(image), symbol=symbol)
    instructions = decoded.instructions
    if not instructions or instructions[-1].text != 'jmp <+0x0>':
        raise CpuStateError('%s in %s is not a closed loop: %s' % (
            symbol, image, '; '.join(instruction.text for instruction in instructions)))
    return IdleLoop(symbol, frozenset(instruction.address for instruction in instructions))


# Checks (each returns a list of problems; empty means the check passed) -----

def idle_problems(cpu: CpuState, loop: IdleLoop) -> list[str]:
    if cpu.rip in loop.addresses:
        return []
    return ['CPU %d RIP %#x is not in the idle loop %s (%s)' % (cpu.index, cpu.rip, loop.describe(), cpu.describe())]


@dataclasses.dataclass(frozen=True)
class UserModeExpectation:
    code_selector: int
    data_selector: int
    stack_pointer: int
    rflags_required: int
    rflags_allowed: int


def user_mode_problems(cpu: CpuState, expected: UserModeExpectation) -> list[str]:
    problems = []
    if cpu.cpl != 3:
        problems.append('CPL %d, expected 3' % cpu.cpl)
    if cpu.cs.selector != expected.code_selector:
        problems.append('CS %#x, expected %#x' % (cpu.cs.selector, expected.code_selector))
    if cpu.ss.selector != expected.data_selector:
        problems.append('SS %#x, expected %#x' % (cpu.ss.selector, expected.data_selector))
    if cpu.rsp != expected.stack_pointer:
        problems.append('RSP %#x, expected the initial user stack top %#x' % (cpu.rsp, expected.stack_pointer))
    if cpu.rflags & expected.rflags_required != expected.rflags_required:
        problems.append('RFLAGS %#x lacks %#x' % (cpu.rflags, expected.rflags_required))
    if cpu.rflags & ~expected.rflags_allowed:
        problems.append('RFLAGS %#x has bits outside %#x' % (cpu.rflags, expected.rflags_allowed))
    return problems


@dataclasses.dataclass(frozen=True)
class ControlRegisterPolicy:
    """What the entry install leaves on every CPU (Amd64UserEntry.hpp): CR0
    bits it requires and clears, and the exact CR4. CR2 is zero or the last
    fault address in one of `fault_ranges` [start, end): the initial image's
    lazy region, whose first touches the boot pager resolves, and the address
    the test firmware's spinner reads so that the pager terminates it."""
    cr0_required: int
    cr0_forbidden: int
    cr4: int
    fault_ranges: tuple[tuple[int, int],...] = ()


def control_register_problems(cpu: CpuState, policy: ControlRegisterPolicy) -> list[str]:
    """The control-register policy, on the BSP and on every AP alike."""
    problems = []
    if cpu.cr0 & policy.cr0_required != policy.cr0_required:
        problems.append('CPU %d CR0 %#x lacks %#x' % (cpu.index, cpu.cr0, policy.cr0_required & ~cpu.cr0))
    if cpu.cr0 & policy.cr0_forbidden:
        problems.append('CPU %d CR0 %#x has forbidden bits %#x' % (cpu.index, cpu.cr0, cpu.cr0 & policy.cr0_forbidden))
    if cpu.cr4 != policy.cr4:
        problems.append('CPU %d CR4 %#x, expected %#x (extra bits %#x, missing bits %#x)' % (
            cpu.index, cpu.cr4, policy.cr4, cpu.cr4 & ~policy.cr4, policy.cr4 & ~cpu.cr4))
    if cpu.cr2 != 0 and not any(start <= cpu.cr2 < end for start, end in policy.fault_ranges):
        problems.append('CPU %d CR2 %#x: a page fault outside the expected fault addresses was recorded'
                        % (cpu.index, cpu.cr2))
    return problems


def root_table_problems(cpu: CpuState) -> list[str]:
    if cpu.cr3 == 0 or cpu.cr3 & 0xFFF:
        return ['CPU %d CR3 %#x is not a 4 KiB-aligned root table' % (cpu.index, cpu.cr3)]
    return []


def long_mode_problems(snapshot: CpuSnapshot, cpu: CpuState) -> list[str]:
    """IA-32e mode evidence without EFER (see the module docstring)."""
    problems = []
    if cpu.index == 0 and not snapshot.long_mode_attested:
        problems.append('QEMU wrote an ELF class %d machine %d core: CPU 0 has no EFER.LMA'
                        % (snapshot.elf_class, snapshot.machine))
    if not cpu.cs.flags & DESCRIPTOR_L or cpu.cs.flags & DESCRIPTOR_D:
        problems.append('CPU %d CS flags %#x are not a 64-bit code segment' % (cpu.index, cpu.cs.flags))
    if not cpu.cr0 & CR0_PG or not cpu.cr4 & CR4_PAE:
        problems.append('CPU %d paging is not 4-level PAE paging (CR0 %#x CR4 %#x)' % (cpu.index, cpu.cr0, cpu.cr4))
    return problems


@dataclasses.dataclass(frozen=True)
class ParkedProcessorExpectation:
    """Where an application processor must be parked on the shared root (its
    control registers are judged by `control_register_problems`)."""
    root: int
    code_selector: int
    data_selector: int
    stack_low: int   # RSP must be in (stack_low, stack_high]
    stack_high: int
    interrupt_window: tuple[int, int] = (0, 0)


def parked_processor_problems(cpu: CpuState, expected: ParkedProcessorExpectation) -> list[str]:
    problems = []
    if cpu.cr3 != expected.root:
        problems.append('CPU %d CR3 %#x, expected the shared root %#x' % (cpu.index, cpu.cr3, expected.root))
    if cpu.cpl != 0:
        problems.append('CPU %d CPL %d, expected 0' % (cpu.index, cpu.cpl))
    if cpu.cs.selector != expected.code_selector or cpu.ss.selector != expected.data_selector:
        problems.append('CPU %d CS/SS %#x/%#x, expected %#x/%#x' % (
            cpu.index, cpu.cs.selector, cpu.ss.selector, expected.code_selector, expected.data_selector))
    if cpu.rflags & RFLAGS_IF and not expected.interrupt_window[0] <= cpu.rip < expected.interrupt_window[1]:
        problems.append("CPU %d RFLAGS %#x: IF is set outside the AP's proven interrupt window"
                        % (cpu.index, cpu.rflags))
    if not expected.stack_low < cpu.rsp <= expected.stack_high:
        problems.append('CPU %d RSP %#x is outside its idle stack (%#x, %#x]' % (
            cpu.index, cpu.rsp, expected.stack_low, expected.stack_high))
    return problems


def descriptor_table_problems(cpu: CpuState, gdt: int, idt: int) -> list[str]:
    observed = (cpu.segments['gdt'].base, cpu.segments['idt'].base)
    if observed == (gdt, idt):
        return []
    return ['CPU %d GDT/IDT %#x/%#x, expected its own tables %#x/%#x' % ((cpu.index,) + observed + (gdt, idt))]
