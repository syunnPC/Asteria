# SPDX-License-Identifier: Apache-2.0
"""Recorded AMD64 runs and helpers shared by the AMD64 observer tests (no tests here).

Fixtures/Amd64 holds the artifacts of three passing runner cases, copied
from their run directories: `<Case>.CpuState.elf` (the `dump-guest-memory`
core of the stopped guest), `<Case>.Observation.json.gz` (every physical
page the checks read, the kernel's evidence record and the AHCI read buffer
included, and the QMP query results) and `<Case>.Debug.log`, for Amd64.Smp4
and Amd64.Ahci (the test firmware, whose initial image is the guest test
program `GuestTests.elf`) and Amd64Preemption.Relocated (the production
firmware of the preemption-probe preset, image `MinimalUserImage.elf`). The
user images are the amd64 preset's (identical in all AMD64 presets); the
checks use their idle loop and segments. Regenerate every fixture by copying
the same files from one passing `RunVmTests.py --suite Amd64 --suite
Amd64NativePreemption` run directory and the preset tree, and
`ComponentImages.json` (the staged and zero-filled segments of each image the
pager embeds) and `ComponentPages.json` (shared image pages and private
component pages retained by product init) by
`python3 -m Tests.test_amd64_fixtures --record <amd64 preset tree>`.
"""
from __future__ import annotations

import dataclasses
import functools
import json
import pathlib
import struct
import sys

from AsteriaVm import Amd64Layout, Evidence, Scenarios
from Tests import recorded_schema
from AsteriaVm.Drivers import Amd64Default
from AsteriaVm.Executor import CaseFailure, CheckResult
from AsteriaVm.Observers import Amd64Cpu, Amd64Evidence, Amd64Paging, SharedImages

FIXTURES = pathlib.Path(__file__).resolve().parent / 'Fixtures' / 'Amd64'
REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
PRODUCTION_IMAGE = FIXTURES / 'MinimalUserImage.elf'
GUEST_IMAGE = FIXTURES / 'GuestTests.elf'
CASES = {'Smp4': 'Amd64.Smp4', 'Ahci': 'Amd64.Ahci', 'PreemptionRelocated': 'Amd64Preemption.Relocated'}
USER_IMAGES = {'Smp4': GUEST_IMAGE, 'Ahci': GUEST_IMAGE, 'PreemptionRelocated': PRODUCTION_IMAGE}
CONFIGURATION = {
    'Smp4': {'image_base': '0x04000000', 'amd64_native_preemption_probe': '0'},
    'Ahci': {'image_base': '0x04000000', 'amd64_native_preemption_probe': '0'},
    'PreemptionRelocated': {'image_base': '0x0000000100000000', 'amd64_native_preemption_probe': '1'},
}


def core(name: str) -> bytes:
    return (FIXTURES / ('%s.CpuState.elf' % name)).read_bytes()


def snapshot(name: str) -> Amd64Cpu.CpuSnapshot:
    return Amd64Cpu.parse_core(core(name))


def debug_lines(name: str) -> list[str]:
    text = (FIXTURES / ('%s.Debug.log' % name)).read_bytes().decode('ascii')
    return [line[:-1] if line.endswith('\r') else line for line in text.split('\n')][:-1]


def observation(name: str) -> Amd64Default.Observation:
    return Amd64Default.Observation.recorded(FIXTURES / ('%s.Observation.json.gz' % name))


@functools.lru_cache(maxsize=None)
def constants() -> Amd64Layout.HeaderConstants:
    current = Amd64Layout.headers(REPOSITORY)
    captured = recorded_schema.schema(FIXTURES, Amd64Evidence)
    if any(field.name == 'UserProcessors' for field in captured.record.fields):
        return current
    from AsteriaVerify import Layout
    sources = json.loads((FIXTURES / 'BuildHeaders.json').read_text())['sources']
    pages = Layout.constants(sources['Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp'])['RuntimePoolPages']
    class CpuZeroConstants(Amd64Layout.HeaderConstants):
        def idle_root(self, pool, cpu):
            return self.root(pool, self.shared_root_index)
    return CpuZeroConstants(**{**dataclasses.asdict(current), 'pool_pages': pages})


def schema() -> Evidence.Schema:
    return recorded_schema.schema(FIXTURES, Amd64Evidence)


def idle_loop(name: str = 'Smp4') -> Amd64Cpu.IdleLoop:
    """The idle loop as objdump decodes it (`pause` at +0, `jmp` at +2); no toolchain needed."""
    from AsteriaVerify.Elf import ElfFile
    symbol = ElfFile(USER_IMAGES[name]).symbol(Amd64Layout.USER_IDLE_SYMBOL)
    return Amd64Cpu.IdleLoop(symbol.name, frozenset({symbol.value, symbol.value + 2}))


def component_images() -> tuple[tuple[str, tuple[int, int]],...]:
    recorded = json.loads((FIXTURES / 'ComponentImages.json').read_text())
    return tuple((name, tuple(recorded[name])) for name in Amd64Default.COMPONENT_IMAGES + (('smp',) if 'smp' in recorded else ()))


def expectations(name: str) -> Amd64Default.Expectations:
    return Amd64Default.Expectations(constants(), idle_loop(name), Amd64Paging.user_image_segments(USER_IMAGES[name]),
                                     schema(), Amd64Evidence.outcomes(REPOSITORY), component_images(),
                                     tuple(json.loads((FIXTURES / 'ComponentPages.json').read_text())['shared'].items()),
                                     json.loads((FIXTURES / 'ComponentPages.json').read_text())['private'],
                                     json.loads((FIXTURES / 'ComponentPages.json').read_text())['display_private'])


def record(name: str, recorded: Amd64Default.Observation | None = None) -> Evidence.Record:
    """The kernel's evidence record of a recorded run."""
    found, _, problems = Amd64Default.evidence_record(recorded or observation(name), debug_lines(name), schema())
    assert found is not None, problems
    return found


def record_address(name: str) -> int:
    address, problems = Evidence.locate(debug_lines(name))
    assert address is not None, problems
    return address


def field(path: str) -> tuple[int, int]:
    """(offset, size) in the AMD64 record of a field named like
    `ApplicationProcessors.Processors[1].Timer.DeliveredCount` or `Ahci.IdentifyData[20]`."""
    structure, offset = schema().record, 0
    parts = path.split('.')
    for position, part in enumerate(parts):
        name, _, index = part.partition('[')
        member = structure.field(name)
        offset += member.offset
        if index:
            offset += int(index.rstrip(']')) * member.element_size
        if position + 1 < len(parts):
            structure = member.element
        else:
            return offset, member.element_size if index or member.count is None else member.size
    raise AssertionError(path)


# Core-file mutation ----------------------------------------------------------

def note_descriptors(data: bytes) -> list[tuple[str, int, int]]:
    """(name, type, descriptor offset) of every note, in file order."""
    (program_offset,) = struct.unpack_from('<Q', data, 32)
    (count,) = struct.unpack_from('<H', data, 56)
    found = []
    for index in range(count):
        kind, _, offset, _, _, size, _, _ = struct.unpack_from('<IIQQQQQQ', data, program_offset + index * 56)
        if kind != 4:
            continue
        cursor = offset
        while cursor + 12 <= offset + size:
            name_size, descriptor_size, note_type = struct.unpack_from('<III', data, cursor)
            name = data[cursor + 12:cursor + 12 + name_size].rstrip(b'\0').decode()
            descriptor = cursor + 12 + ((name_size + 3) & ~3)
            found.append((name, note_type, descriptor))
            cursor = descriptor + ((descriptor_size + 3) & ~3)
    return found


PRSTATUS_REGISTERS = ('r15', 'r14', 'r13', 'r12', 'rbp', 'rbx', 'r11', 'r10', 'r9', 'r8', 'rax', 'rcx', 'rdx',
                      'rsi', 'rdi', 'orig_rax', 'rip', 'cs', 'rflags', 'rsp', 'ss')
QEMU_REGISTERS = ('rax', 'rbx', 'rcx', 'rdx', 'rsi', 'rdi', 'rsp', 'rbp', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13',
                  'r14', 'r15', 'rip', 'rflags')


def set_register(data: bytes, cpu: int, name: str, value: int) -> bytes:
    """Rewrites one general register of one CPU consistently in both of its notes."""
    patched = bytearray(data)
    notes = note_descriptors(data)
    prstatus = [offset for note, kind, offset in notes if note == 'CORE' and kind == 1]
    qemu = [offset for note, kind, offset in notes if note == 'QEMU' and kind == 0]
    struct.pack_into('<Q', patched, prstatus[cpu] + 112 + 8 * PRSTATUS_REGISTERS.index(name), value)
    struct.pack_into('<Q', patched, qemu[cpu] + 8 + 8 * QEMU_REGISTERS.index(name), value)
    return bytes(patched)


def replace_cpu(snapshot_: Amd64Cpu.CpuSnapshot, cpu: int, **changes) -> Amd64Cpu.CpuSnapshot:
    """A snapshot whose CPU `cpu` has `registers`/`segments`/`control` entries replaced."""
    state = snapshot_.cpus[cpu]
    registers = dict(state.registers, **changes.get('registers', {}))
    segments = dict(state.segments, **changes.get('segments', {}))
    control = list(state.control)
    for index, value in changes.get('control', {}).items():
        control[index] = value
    replaced = dataclasses.replace(state, registers=registers, segments=segments, control=tuple(control))
    cpus = list(snapshot_.cpus)
    cpus[cpu] = replaced
    return dataclasses.replace(snapshot_, cpus=tuple(cpus))


# Physical-memory mutation ----------------------------------------------------

class EditedMemory(Amd64Paging.PageCache):
    """Recorded physical pages that a test may patch before walking them."""

    def __init__(self, observation_: Amd64Default.Observation):
        recorded = observation_.physical
        self.recorded = recorded

        def fetch(page: int) -> bytes:
            return recorded.read(page, Amd64Paging.PAGE_SIZE)
        super().__init__(fetch)

    def entry_address(self, root: int, address: int, level: int) -> int:
        """Physical address of the level-`level` entry (0 = PML4E.. 3 = PTE) that maps `address`."""
        table = root & Amd64Paging.PTE_ADDRESS
        for depth, shift in enumerate(Amd64Paging.LEVEL_SHIFTS):
            location = table + ((address >> shift) & 511) * 8
            if depth == level:
                return location
            (entry,) = struct.unpack('<Q', self.read(location, 8))
            if not entry & Amd64Paging.PTE_PRESENT:
                raise AssertionError('no level-%d table for %#x' % (depth + 1, address))
            table = entry & Amd64Paging.PTE_ADDRESS
        raise AssertionError('level %d out of range' % level)

    def entry(self, root: int, address: int, level: int = 3) -> int:
        return struct.unpack('<Q', self.read(self.entry_address(root, address, level), 8))[0]

    def set_entry(self, root: int, address: int, value: int, level: int = 3) -> None:
        self.write(self.entry_address(root, address, level), struct.pack('<Q', value))

    def write(self, address: int, data: bytes) -> None:
        cursor = 0
        while cursor < len(data):
            page = (address + cursor) & Amd64Paging.PAGE_MASK
            start = address + cursor - page
            chunk = min(Amd64Paging.PAGE_SIZE - start, len(data) - cursor)
            content = bytearray(self.read(page, Amd64Paging.PAGE_SIZE))
            content[start:start + chunk] = data[cursor:cursor + chunk]
            self.pages[page] = bytes(content)
            cursor += chunk


def edited_observation(name: str, edit) -> Amd64Default.Observation:
    """The recorded observation of `name` with its physical memory changed by `edit(EditedMemory)`."""
    recorded = observation(name)
    memory = EditedMemory(recorded)
    edit(memory)
    recorded.physical = Amd64Paging.PageCache(lambda page: memory.read(page, Amd64Paging.PAGE_SIZE))
    recorded.walker = Amd64Paging.PageWalker(recorded.physical)
    return recorded


def with_evidence(name: str, path: str, change) -> Amd64Default.Observation:
    """The recorded observation of `name` whose evidence field `path` is `change(value)`."""
    address = record_address(name)
    offset, size = field(path)

    def edit(memory: EditedMemory) -> None:
        value = int.from_bytes(memory.read(address + offset, size), 'little')
        memory.write(address + offset, (change(value) % (1 << (8 * size))).to_bytes(size, 'little'))
    return edited_observation(name, edit)


# A case context without a VM ------------------------------------------------

class FakeConsole:
    def __init__(self, lines: list[str]):
        self.lines = lines

    def failures(self) -> list[str]:
        from AsteriaVm.Console import FAILURE
        return [line for line in self.lines if FAILURE.match(line)]


class FakeTarget:
    def __init__(self, configuration: dict[str, str]):
        self.configuration = configuration
        self.root = REPOSITORY

    def option(self, name: str) -> str:
        return self.configuration[name]


class FakeContext:
    """The parts of Executor.CaseContext that `Amd64Default.judge` uses."""

    def __init__(self, name: str, directory: pathlib.Path, lines: list[str] | None = None):
        self.case = next(case for case in Scenarios.CASES if case.name == CASES[name])
        self.target = FakeTarget(CONFIGURATION[name])
        self.console = FakeConsole(debug_lines(name) if lines is None else lines)
        self.run_directory = directory
        self.checks: list[CheckResult] = []

    def check(self, name: str, passed: bool, detail: str = '', *, skipped: str = '') -> bool:
        self.checks.append(CheckResult(name, bool(passed), detail, skipped))
        return bool(passed)

    def check_problems(self, name: str, problems: list[str]) -> bool:
        return self.check(name, not problems, '\n'.join(problems))

    def require(self, name: str, passed: bool, detail: str = '') -> None:
        if not self.check(name, passed, detail):
            raise CaseFailure('%s: %s' % (name, detail))

    def artifact(self, name: str) -> pathlib.Path:
        return self.run_directory / name

    def failed(self) -> dict[str, str]:
        return {check.name: check.detail for check in self.checks if not check.passed}


if __name__ == '__main__' and sys.argv[1:2] == ['--record']:
    recorded_schema.record(FIXTURES, Amd64Evidence, REPOSITORY)
    tree = pathlib.Path(sys.argv[2])
    images = {name: Amd64Paging.staged_segments(tree / 'Component' / ('%sImage.elf' % stem))
              for name, stem in zip(Amd64Default.COMPONENT_IMAGES, ('Component', 'Directory', 'Client',
                                                                     'StubInterpreter', 'Display'))}
    images['smp'] = Amd64Paging.staged_segments(tree / 'Smp/SmpWorkers.elf')
    (FIXTURES / 'ComponentImages.json').write_text(json.dumps(images, indent=1) + '\n')

    pages = {'shared': {name: SharedImages.layout(tree / 'Component' / ('%sImage.elf' % stem))[1]
                        for name, stem in zip(Amd64Default.COMPONENT_IMAGES,
                                              ('Component', 'Directory', 'Client', 'StubInterpreter', 'Display'))},
             'private': SharedImages.private_pages(tree / 'Component' / 'ComponentImage.elf'),
             'display_private': SharedImages.private_pages(tree / 'Component' / 'DisplayImage.elf')}
    pages['shared']['smp'] = SharedImages.layout(tree / 'Smp/SmpWorkers.elf')[1]
    (FIXTURES / 'ComponentPages.json').write_text(json.dumps(pages, indent=1) + '\n')
