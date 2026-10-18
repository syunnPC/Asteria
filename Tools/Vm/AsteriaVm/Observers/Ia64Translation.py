# SPDX-License-Identifier: Apache-2.0
"""IA-64 translation state: pinned kernel TRs, user TCs and region registers.

The key kernel-memory-protection property: the translation
registers map exactly the bootstrap reservation, the run-time memory
reservation and the kernel's own loaded image, identity-mapped at PL0 with
W^X permissions and within the CPU's TR capacity; nothing maps the image's
header gap. Expected geometry comes from
the ELF note and PT_LOAD groups, relocated by the placed base and from the kernel's
bootstrap layout; expected encodings (access rights, QEMU permission masks,
PTE flag words) are parameters.

Bootstrap data lives in the runtime page and the fail-stop IVT in image
text. Exactly four DTRs and one live image ITR are required, plus reserved
firmware ITR0. The runtime DTR supplies the bootstrap base, independently
of the loaded image. Its page is naturally aligned and outside the image.

Architectural facts used here: TR/TC entries map `ps`-sized naturally aligned
pages; region registers are `rid << 8 | ps << 2 | ve` (IA-64 SDM Vol. 2).
"""
from __future__ import annotations

import dataclasses

from AsteriaVm.Observers.Ia64Image import KERNEL_SECTIONS, ImageFacts, KernelLayout, round_up
from AsteriaVm.Observers.Ia64State import CpuState, StateError, TlbEntry

BASE_PAGE = 0x1000


@dataclasses.dataclass(frozen=True)
class Access:
    """How a page may be used, as the kernel encodes it: the PTE access-rights
    field, QEMU's derived r/w/x mask (1/2/4), and the PTE flag word below the
    physical page number."""
    name: str
    ar: int
    perm: int
    flags: int


# PTE.ar (bits 9-11) at the page's own privilege level grants read (0),
# read/execute (1), read/write (2) or read/write/execute (3); QEMU prints the
# grant as perm (r=1, w=2, x=4). IA-64 SDM Vol. 2, page access rights.
_ACCESS_RIGHTS_PERMISSIONS = {0: 0x1, 1: 0x5, 2: 0x3, 3: 0x7}


def access(name: str, flags: int) -> Access:
    """The Access of a kernel-encoded PTE flag word (ar and perm follow from it)."""
    rights = (flags >> 9) & 0x7
    if rights not in _ACCESS_RIGHTS_PERMISSIONS:
        raise ValueError('%s: access rights %d are not a plain R/RX/RW/RWX grant' % (name, rights))
    return Access(name, rights, _ACCESS_RIGHTS_PERMISSIONS[rights], flags)


@dataclasses.dataclass(frozen=True)
class KernelTranslations:
    """What the kernel's pinned translations must look like."""
    rid: int                               # Region identifier of region 0 kernel translations
    page_sizes: tuple[int,...]            # PAL-insertable page sizes
    instruction_capacity: int
    data_capacity: int
    firmware_instruction_registers: int    # ITR slots before the kernel's (ITR0 belongs to the firmware)
    exception_deferral: int                # PTE bit set on every kernel translation
    text: Access                           # Text
    read_only: Access                      # Note/read-only body/tail
    read_write: Access                     # Data...bss and the run-time memory reservation
    runtime_memory_page: int               # The run-time memory reservation's page (one DTR)


@dataclasses.dataclass(frozen=True)
class UserPage:
    name: str
    kind: str        # 'ITLB' or 'DTLB'
    va: int
    access: Access


def region_register(rid: int, page_shift: int, enabled: bool = False) -> int:
    return rid << 8 | page_shift << 2 | int(enabled)


def region_register_problems(state: CpuState, expected: dict[int, int]) -> list[str]:
    """Region registers 0, 5, 6 and 7 (the ones QEMU prints) hold the
    expected region identifiers and preferred page size."""
    try:
        record = state.get('IHA')
    except StateError as error:
        return [str(error)]
    return ['RR%d %#018x, expected %#018x' % (region, record['rr%d' % region], value)
            for region, value in sorted(expected.items()) if record['rr%d' % region] != value]


def kernel_section_problems(facts: ImageFacts, ivt_size: int) -> list[str]:
    """The linked sections are page-aligned and contiguous after page
    rounding (so the kernel can tile them with TRs), and the landing routine
    and the runtime IVT lie inside.text."""
    problems = []
    names = KERNEL_SECTIONS
    base = facts.linked_base
    for name in names:
        start, size = facts.kernel_sections[name]
        if size == 0 or start % BASE_PAGE:
            problems.append('%s at %#x size %#x is empty or not page-aligned' % (name, start, size))
    text_start, text_size = facts.kernel_sections['.text']
    if text_start <= base:
        problems.append('.text %#x does not follow the image base %#x' % (text_start, base))
    for previous, following in zip(names, names[1:]):
        start, size = facts.kernel_sections[previous]
        if round_up(start + size, BASE_PAGE) != facts.kernel_sections[following][0]:
            problems.append('%s ends at %#x (page-rounded) but %s starts at %#x' % (
                previous, round_up(start + size, BASE_PAGE), following, facts.kernel_sections[following][0]))
    landing = facts.kernel('AsteriaIa64KernelIdle').value
    ivt = facts.kernel('AsteriaIa64RuntimeInterruptVectorTableImage').value
    if not text_start <= landing < text_start + text_size:
        problems.append('AsteriaIa64KernelIdle %#x is outside .text' % landing)
    if not (text_start <= ivt and ivt + ivt_size <= text_start + text_size):
        problems.append('the runtime IVT %#x is outside .text' % ivt)
    return problems


def _entry_problems(entry: TlbEntry, access: Access, exception_deferral: int) -> list[str]:
    expected_pte = entry.pa | exception_deferral | access.flags
    if (entry.ar, entry.perm, entry.pte) != (access.ar, access.perm, expected_pte):
        return ['%s is not a %s mapping (expected ar=%d perm=%#x pte=%#018x)' % (
            entry.describe(), access.name, access.ar, access.perm, expected_pte)]
    return []


def kernel_translation_problems(state: CpuState, layout: KernelLayout,
                                expected: KernelTranslations) -> tuple[list[str], int | None]:
    """Returns the problems and the bootstrap reservation base derived from the run."""
    try:
        instruction = state.translation_registers('ITLB')
        data = state.translation_registers('DTLB')
    except StateError as error:
        return [str(error)], None
    problems = []

    # Every TR: identity, naturally aligned, PL0, insertable size, kernel RID, no key.
    for entry in instruction + data:
        if (entry.va != entry.pa or entry.va % entry.ps or entry.pl != 0 or entry.ps not in expected.page_sizes
                or entry.rid != expected.rid or entry.key != 0):
            problems.append('%s is not an identity PL0 kernel translation (rid %#x, insertable page size)'
                            % (entry.describe(), expected.rid))
    for position, entry in enumerate(instruction):
        if entry.index != position + expected.firmware_instruction_registers:
            problems.append('ITR slots are not sequential from %d: %s' % (
                expected.firmware_instruction_registers, entry.describe()))
            break
    for position, entry in enumerate(data):
        if entry.index != position:
            problems.append('DTR slots are not sequential from 0: %s' % entry.describe())
            break
    if len(instruction) + expected.firmware_instruction_registers > expected.instruction_capacity:
        problems.append('%d ITRs plus %d firmware ITR exceed the %d instruction TRs' % (
            len(instruction), expected.firmware_instruction_registers, expected.instruction_capacity))
    if len(data) > expected.data_capacity:
        problems.append('%d DTRs exceed the %d data TRs' % (len(data), expected.data_capacity))
    for entry in instruction:
        problems += _entry_problems(entry, expected.text, expected.exception_deferral)

    # The run-time memory reservation: exactly one read/write DTR of its page
    # size, identity and naturally aligned (checked above); image tiling
    # below runs over the other DTRs.
    image_start, bss_end = layout.loaded.base, layout.section('.bss')[1]
    runtime = [entry for entry in data if entry.ps == expected.runtime_memory_page
               and entry.ar == expected.read_write.ar and not (entry.va < bss_end and image_start < entry.va + entry.ps)]
    if len(runtime) != 1:
        problems.append('%d read/write DTRs of %#x bytes outside the image, expected the one run-time memory '
                        'reservation' % (len(runtime), expected.runtime_memory_page))
    for entry in runtime:
        problems += _entry_problems(entry, expected.read_write, expected.exception_deferral)
    data = [entry for entry in data if entry not in runtime]

    if (len(data) != 3 or len(instruction) != 1 or len(runtime) != 1
            or len(data) + len(runtime) > 8
            or len(instruction) + expected.firmware_instruction_registers > 8):
        problems.append('the consolidated mandatory set must be exactly 4 DTRs and 2 ITRs within 8+8')
    if len(runtime) != 1:
        return problems, None
    base = runtime[0].va

    # The loaded image: geometry, no overlap with the reservation, TR tiling.
    image_start = layout.loaded.base
    text_start, text_end = layout.section('.text')
    rodata_start, _ = layout.section('.note.asteria')
    _, reloc_end = layout.section('ReadOnlyTail')
    data_start, _ = layout.section('.data')
    _, bss_end = layout.section('.bss')
    if bss_end - image_start != layout.loaded.size:
        problems.append('the relocated image spans %#x..%#x (%#x bytes) but the firmware loaded %#x bytes' % (
            image_start, bss_end, bss_end - image_start, layout.loaded.size))
    broken = []

    def tile(entries: list[TlbEntry], start: int, end: int, what: str) -> tuple[list[TlbEntry], list[TlbEntry]]:
        """Splits off the leading entries that tile [start, end) exactly. The
        first gap or overhang is recorded and ends the tiling (later ranges
        would only repeat it)."""
        if broken:
            return [], []
        cursor, used = start, 0
        while cursor < end:
            if used >= len(entries) or entries[used].va != cursor or entries[used].ps > end - cursor:
                problems.append('%s coverage breaks at %#x (%s)' % (
                    what, cursor, entries[used].describe() if used < len(entries) else 'no further TR'))
                broken.append(what)
                return entries[:used], []
            cursor += entries[used].ps
            used += 1
        return entries[:used], entries[used:]

    text_itrs, extra = tile(instruction, text_start, text_end, '.text ITR')
    if extra:
        problems.append('extra ITRs after .text: %s' % '; '.join(entry.describe() for entry in extra))
    text_dtrs, remaining = tile(data, text_start, text_end, '.text DTR')
    if not broken and [(dtr.va, dtr.ps) for dtr in text_dtrs] != [(itr.va, itr.ps) for itr in text_itrs]:
        problems.append('the .text DTRs do not mirror the .text ITRs')
    read_only, remaining = tile(remaining, rodata_start, reloc_end, '.rodata...reloc DTR')
    read_write, remaining = tile(remaining, data_start, bss_end, '.data...bss DTR')
    if remaining:
        problems.append('extra DTRs after the image: %s' % '; '.join(entry.describe() for entry in remaining))
    for entries, access in ((text_dtrs, expected.text), (read_only, expected.read_only),
                            (read_write, expected.read_write)):
        for entry in entries:
            problems += _entry_problems(entry, access, expected.exception_deferral)

    for entry in instruction + data:
        if entry.va < text_start and entry.va + entry.ps > image_start:
            problems.append('%s maps the image header gap %#x..%#x' % (entry.describe(), image_start, text_start))
    return problems, base


def user_translation_cache_problems(state: CpuState, pages: tuple[UserPage,...], rid: int,
                                    exception_deferral: int) -> list[str]:
    """Exactly the expected user pages have translation-cache entries,
    each a 4 KiB PL3 page of the user region with its own physical page and
    the expected (W^X) permissions."""
    try:
        entries = [entry for entry in state.translation_cache() if entry.kind in ('ITLB', 'DTLB')]
    except StateError as error:
        return [str(error)]
    problems = []
    seen: dict[str, TlbEntry] = {}
    for entry in entries:
        if (entry.ps != BASE_PAGE or entry.va % BASE_PAGE or entry.pa % BASE_PAGE or entry.pl != 3
                or entry.rid != rid or entry.key != 0):
            problems.append('%s is not a 4 KiB PL3 user translation of rid %#x' % (entry.describe(), rid))
        page = next((page for page in pages if page.va == entry.va), None)
        if page is None:
            problems.append('unexpected user translation %s' % entry.describe())
            continue
        if page.name in seen:
            problems.append('the %s page has two translations: %s' % (page.name, entry.describe()))
            continue
        seen[page.name] = entry
        if entry.kind != page.kind:
            problems.append('the %s page is in the %s, expected the %s' % (page.name, entry.kind, page.kind))
        problems += _entry_problems(entry, page.access, exception_deferral)
    for page in pages:
        if page.name not in seen:
            problems.append('no translation of the %s page %#x' % (page.name, page.va))
    physical: dict[int, str] = {}
    for name, entry in seen.items():
        if entry.pa in physical:
            problems.append('the %s and %s pages share physical page %#x' % (physical[entry.pa], name, entry.pa))
        physical[entry.pa] = name
    return problems
