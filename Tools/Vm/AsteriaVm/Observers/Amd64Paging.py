# SPDX-License-Identifier: Apache-2.0
"""AMD64 page walks over guest-physical memory read through QMP, and their checks.

The walker follows the four-level tables from a root (CR3) the way the MMU
does, accumulating R/W, U/S and NX over all levels, and reads each table page
once with native `pmemsave`. The bootstrap profile maps only 4 KiB leaves
with a PAT-selected memory type. Bit 7 is PAT at a leaf; at an ancestor
it denotes an unsupported large page and is rejected.

The checks protect the address-space properties of the kernel's two roots:
the private user root (the BSP's CR3 while the user image idles) and the
shared kernel root the application processors run on.
"""
from __future__ import annotations

import dataclasses
import pathlib
import struct

from AsteriaVm import Build  # Noqa: F401 (puts Tools/Verify on sys.path)
from AsteriaVerify.Elf import PF_W, PF_X, PT_LOAD, ElfFile

PAGE_SIZE = 4096
PAGE_MASK = ~(PAGE_SIZE - 1)
# Architectural PTE bits (Intel SDM vol. 3, 4.5).
PTE_PRESENT = 1 << 0
PTE_WRITABLE = 1 << 1
PTE_USER = 1 << 2
PTE_LARGE_OR_PAT = 1 << 7
PTE_NO_EXECUTE = 1 << 63
PTE_ADDRESS = 0x000F_FFFF_FFFF_F000
LEVEL_SHIFTS = (39, 30, 21, 12)
LEVEL_NAMES = ('PML4E', 'PDPTE', 'PDE', 'PTE')


class PagingError(Exception):
    """A walk met an entry the bootstrap profile forbids (large page)."""


@dataclasses.dataclass(frozen=True)
class Leaf:
    physical: int      # Physical address of the walked virtual address (not only its page)
    writable: bool
    user: bool
    executable: bool
    pat_index: int = 0

    @property
    def permissions(self) -> str:
        return 'R%s%s %s' % ('W' if self.writable else '-', 'X' if self.executable else '-',
                              'user' if self.user else 'supervisor')


class PageCache:
    """Whole 4 KiB pages fetched once each; valid while the guest is stopped."""

    def __init__(self, fetch):
        self._fetch = fetch
        self.pages: dict[int, bytes] = {}

    def read(self, address: int, size: int) -> bytes:
        data = bytearray()
        cursor = address
        while cursor < address + size:
            page = cursor & PAGE_MASK
            if page not in self.pages:
                content = self._fetch(page)
                if len(content) != PAGE_SIZE:
                    raise PagingError('read of page %#x returned %d bytes' % (page, len(content)))
                self.pages[page] = content
            start = cursor - page
            chunk = min(PAGE_SIZE - start, address + size - cursor)
            data += self.pages[page][start:start + chunk]
            cursor += chunk
        return bytes(data)


class PageWalker:
    def __init__(self, memory: PageCache):
        self.memory = memory

    def entries(self, table: int) -> tuple[int,...]:
        return struct.unpack('<512Q', self.memory.read(table, PAGE_SIZE))

    def walk(self, root: int, address: int) -> Leaf | None:
        """The leaf mapping `address` under `root`, or None if it is not present."""
        table = root & PTE_ADDRESS
        writable = user = executable = True
        for level, shift in enumerate(LEVEL_SHIFTS):
            entry = self.entries(table)[(address >> shift) & 511]
            if not entry & PTE_PRESENT:
                return None
            if level < 3 and entry & PTE_LARGE_OR_PAT:
                raise PagingError('%s %#x for %#x under root %#x has bit 7 (PS/PAT) set' % (
                    LEVEL_NAMES[level], entry, address, root))
            writable &= bool(entry & PTE_WRITABLE)
            user &= bool(entry & PTE_USER)
            executable &= not entry & PTE_NO_EXECUTE
            table = entry & PTE_ADDRESS
        return Leaf(table | (address & ~PAGE_MASK), writable, user, executable,
                    ((entry >> 3) & 3) | ((entry >> 5) & 4))


# Expectations --------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Access:
    writable: bool
    user: bool
    executable: bool

    def describe(self) -> str:
        return Leaf(0, self.writable, self.user, self.executable).permissions


USER_DATA = Access(writable=True, user=True, executable=False)
SUPERVISOR_DATA = Access(writable=True, user=False, executable=False)


def expect_mapped(walker: PageWalker, root: int, address: int, access: Access, what: str,
                  physical: int | None = None) -> list[str]:
    try:
        leaf = walker.walk(root, address)
    except PagingError as error:
        return ['%s: %s' % (what, error)]
    if leaf is None:
        return ['%s (%#x) is not mapped under root %#x' % (what, address, root)]
    problems = []
    if (leaf.writable, leaf.user, leaf.executable) != (access.writable, access.user, access.executable):
        problems.append('%s (%#x) is mapped %s, expected %s' % (what, address, leaf.permissions, access.describe()))
    if physical is not None and leaf.physical != physical:
        problems.append('%s (%#x) maps physical %#x, expected %#x' % (what, address, leaf.physical, physical))
    return problems


def expect_unmapped(walker: PageWalker, root: int, address: int, what: str) -> list[str]:
    try:
        leaf = walker.walk(root, address)
    except PagingError as error:
        return ['%s: %s' % (what, error)]
    if leaf is not None:
        return ['%s (%#x) is mapped %s at physical %#x under root %#x' % (
            what, address, leaf.permissions, leaf.physical, root)]
    return []


def scrub_slot_problems(walker: PageWalker, roots: dict[str, int], start: int, pages: list[int]) -> list[str]:
    """Every root maps each processor's scrub slot address to its slot page as supervisor data, restored after
    every scrub."""
    problems = []
    for name, root in roots.items():
        for cpu, page in enumerate(pages):
            problems += expect_mapped(walker, root, start + cpu * PAGE_SIZE, SUPERVISOR_DATA,
                                      'the %s root\'s scrub slot of CPU %d' % (name, cpu), page)
    return problems


def thread_stack_problems(walker: PageWalker, roots: dict[str, int], starts: list[int], pages: int) -> list[str]:
    """Every root identity-maps each created-thread kernel stack slot as supervisor data,
    with no translation for the guard pages before and after each slot."""
    problems = []
    for name, root in roots.items():
        for slot, start in enumerate(starts):
            problems += expect_unmapped(walker, root, start - PAGE_SIZE,
                                        'the guard below thread stack %d in the %s root' % (slot, name))
            problems += expect_unmapped(walker, root, start + pages * PAGE_SIZE,
                                        'the guard above thread stack %d in the %s root' % (slot, name))
            for index in range(pages):
                address = start + index * PAGE_SIZE
                problems += expect_mapped(walker, root, address, SUPERVISOR_DATA,
                                          'page %d of thread stack %d in the %s root' % (index, slot, name), address)
    return problems


def mapped_window_problems(walker: PageWalker, root: int, start: int, window_pages: int, pages: int,
                           physical: int, pattern: int, *, unmapped: bool, scrubbed: bool = False) -> list[str]:
    """The pager's MemoryMap window: the run of `pages`
    pages it mapped from `physical` holds `pattern` and each page's index in
    memory, or, once the kernel `scrubbed` the destroyed run, nothing but
    zeros; while it is mapped, the window's first `pages` pages map that run
    as user data. Every other page of the window, and every page once the run
    was `unmapped`, has no translation."""
    problems = []
    for index in range(window_pages):
        address, what = start + index * PAGE_SIZE, 'window page %d' % index
        if unmapped or index >= pages:
            problems += expect_unmapped(walker, root, address, what)
        else:
            problems += expect_mapped(walker, root, address, USER_DATA, what, physical + index * PAGE_SIZE)
        if index < pages:
            try:
                content = walker.memory.read(physical + index * PAGE_SIZE, PAGE_SIZE if scrubbed else 16)
            except PagingError as error:
                problems.append('%s (physical %#x): %s' % (what, physical + index * PAGE_SIZE, error))
                continue
            if scrubbed:
                if any(content):
                    offset = next(position for position, byte in enumerate(content) if byte)
                    problems.append('%s (physical %#x) was scrubbed but byte %#x is %#x' % (
                        what, physical + index * PAGE_SIZE, offset, content[offset]))
                continue
            words = struct.unpack('<2Q', content)
            if words != (pattern, index):
                problems.append('%s (physical %#x) holds %#x %#x, expected the pattern %#x and %d' % (
                    what, physical + index * PAGE_SIZE, words[0], words[1], pattern, index))
    return problems


@dataclasses.dataclass(frozen=True)
class ImageSegment:
    start: int
    end: int           # Exclusive, page-rounded
    writable: bool
    executable: bool


def user_image_segments(image: pathlib.Path) -> list[ImageSegment]:
    """The loadable segments of the user ELF, page-rounded, with their permissions."""
    segments = []
    for segment in ElfFile(image).segments:
        if segment.type != PT_LOAD or segment.memory_size == 0:
            continue
        start = segment.virtual_address & PAGE_MASK
        end = (segment.virtual_address + segment.memory_size + PAGE_SIZE - 1) & PAGE_MASK
        segments.append(ImageSegment(start, end, bool(segment.flags & PF_W), bool(segment.flags & PF_X)))
    return segments


def staged_segments(image: pathlib.Path) -> tuple[int, int]:
    """(segments with file bytes, zero-filled segments) of a component image:
    how many the root service stages and how many it maps without staging
    (an empty PT_LOAD, as `-Os` leaves for absent read-only data, is none)."""
    filled = zeroed = 0
    for segment in ElfFile(image).segments:
        if segment.type != PT_LOAD or segment.memory_size == 0:
            continue
        if segment.file_size:
            filled += 1
        else:
            zeroed += 1
    return filled, zeroed


# Checks (each returns a list of problems) ----------------------------------

def user_image_problems(walker: PageWalker, root: int, segments: list[ImageSegment]) -> list[str]:
    """Every page of every loadable segment is a user page with exactly its
    segment's permissions (W^X: text RX, read-only data R, data RW, never WX)."""
    problems = []
    if not segments:
        return ['the user image has no loadable segments']
    for segment in segments:
        access = Access(segment.writable, True, segment.executable)
        for page in range(segment.start, segment.end, PAGE_SIZE):
            problems += expect_mapped(walker, root, page, access, 'user image page')
    return problems


def user_stack_problems(walker: PageWalker, root: int, start: int, pages: int) -> list[str]:
    problems = []
    for page in range(start, start + pages * PAGE_SIZE, PAGE_SIZE):
        problems += expect_mapped(walker, root, page, USER_DATA, 'user stack page')
    problems += expect_unmapped(walker, root, start - PAGE_SIZE, 'guard page below the user stack')
    problems += expect_unmapped(walker, root, start + pages * PAGE_SIZE, 'guard page above the user stack')
    return problems


def descriptor_table_problems(walker: PageWalker, root: int, tables: dict[str, int]) -> list[str]:
    problems = []
    for name, address in tables.items():
        problems += expect_mapped(walker, root, address, SUPERVISOR_DATA, name)
    return problems


def root_separation_problems(walker: PageWalker, shared_root: int, user_addresses: dict[str, int]) -> list[str]:
    """User mappings exist only in the private root: none of them is present in the shared root."""
    problems = []
    for name, address in user_addresses.items():
        problems += expect_unmapped(walker, shared_root, address, '%s in the shared root' % name)
    return problems


def shootdown_probe_problems(walker: PageWalker, private_root: int, shared_root: int, address: int,
                             value: int) -> list[str]:
    """The shootdown probe page exists only in the shared root and holds the published value."""
    problems = expect_unmapped(walker, private_root, address, 'shootdown probe page in the private root')
    mapped = expect_mapped(walker, shared_root, address, SUPERVISOR_DATA, 'shootdown probe page in the shared root')
    problems += mapped
    if not mapped:
        leaf = walker.walk(shared_root, address)
        (observed,) = struct.unpack('<Q', walker.memory.read(leaf.physical, 8))
        if observed != value:
            problems.append('shootdown probe page holds %#x, expected %#x' % (observed, value))
    return problems


def mmio_window_problems(walker: PageWalker, roots: dict[str, int], address: int, target: int) -> list[str]:
    """The bootstrap MMIO window leaf was restored to its supervisor mapping of `target` in every root."""
    problems = []
    for name, root in roots.items():
        problems += expect_mapped(walker, root, address, SUPERVISOR_DATA, 'MMIO window in the %s root' % name,
                                  physical=target)
    return problems


@dataclasses.dataclass(frozen=True)
class StackSlotLayout:
    """Per-CPU kernel stack slots: `pages` pages each, mapped runs (offset, pages) with guards between."""
    first: int                               # Physical address of slot 0
    pages: int
    mapped: tuple[tuple[int, int],...]

    def base(self, cpu: int) -> int:
        return self.first + cpu * self.pages * PAGE_SIZE

    def guards(self) -> list[int]:
        covered = {offset + index for offset, count in self.mapped for index in range(count)}
        return [offset for offset in range(self.pages) if offset not in covered]


def kernel_stack_problems(walker: PageWalker, root: int, layout: StackSlotLayout, cpus: int) -> list[str]:
    """Every stack page of every CPU slot is supervisor RW/NX and every other page of the slot is a guard."""
    problems = []
    for cpu in range(cpus):
        base = layout.base(cpu)
        for offset in layout.guards():
            problems += expect_unmapped(walker, root, base + offset * PAGE_SIZE,
                                        'CPU %d kernel stack guard page %d' % (cpu, offset))
        for offset, count in layout.mapped:
            for index in range(count):
                problems += expect_mapped(walker, root, base + (offset + index) * PAGE_SIZE, SUPERVISOR_DATA,
                                          'CPU %d kernel stack page %d' % (cpu, offset + index))
    return problems


def memory_type(leaf: Leaf, pat_layout: int) -> int:
    """Architectural PAT/PCD/PWT index decoded against the kernel's PAT layout."""
    return (pat_layout >> (leaf.pat_index * 8)) & 255


def candidate_roots(walker: PageWalker, pages: list[int]) -> list[int]:
    """Reserved arenas need not contain a root until admitted. An initialized
    PML4 never has PS/PAT set on a present entry. Ignore untouched firmware
    storage; a damaged broker root still fails the required unique mapping.
    """
    return [page for page in pages if not any(entry & PTE_PRESENT and entry & PTE_LARGE_OR_PAT
                                            for entry in walker.entries(page))]


def scanout_problems(walker: PageWalker, roots: list[int], address: int, pages: int,
                     pat_layout: int) -> list[str]:
    """One broker root maps the entire scanout RW/user/NX with the WC PAT entry."""
    holders = [root for root in roots if walker.walk(root, address) is not None]
    if len(holders) != 1:
        return ['%d roots map the scanout, expected one broker' % len(holders)]
    root = holders[0]
    first = walker.walk(root, address)
    errors = []
    for page in range(pages):
        leaf = walker.walk(root, address + page * PAGE_SIZE)
        if leaf is None or (leaf.writable, leaf.user, leaf.executable) != (True, True, False):
            errors.append('scanout page %d is not user RW/NX' % page)
            break
        if leaf.physical != first.physical + page * PAGE_SIZE:
            errors.append('scanout page %d is not consecutive device memory' % page)
            break
        if memory_type(leaf, pat_layout) != 1:  # Intel/AMD architectural WC type code
            errors.append('scanout page %d selects PAT entry %d, not WC' % (page, leaf.pat_index))
            break
    return errors + expect_unmapped(walker, root, address + pages * PAGE_SIZE, 'page beyond the scanout')
