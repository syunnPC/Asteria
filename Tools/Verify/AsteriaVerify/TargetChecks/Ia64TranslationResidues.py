# SPDX-License-Identifier: Apache-2.0
"""IA-64 kernel ELF translation residues.

The independent kernel note defines six ownership ranges covering three
4 MiB PT_LOAD permission groups. The production planners enumerate all 256
64 KiB source residues and align each placed base to 16 MiB. With consolidated
bootstrap data in the runtime page and an 8+8 PAL report they must use exactly
4 DTRs and 2 ITRs at every residue. A report below the architected minimum
must fail. Both product and test Kernel.elf geometries are proved.
"""
from __future__ import annotations

import dataclasses
import functools
import subprocess

from AsteriaVerify.Checks import Property, register
from AsteriaVerify.KernelElfNote import ROLES, ownership
from AsteriaVerify.Elf import SHN_UNDEF, ElfFile

IMAGE_BASE = 'AsteriaConfiguredImageBase'
# The run-time memory object no longer lives in the image: its storage is the
# run-time memory reservation the boot allocates, so no image symbol may name it.
FORMER_ARENA_SUFFIX = 'g_ia64BspRuntimeMemoryStorageE'
# The loaded-image sections the boot mapping plan covers, in the order the
# proof takes them (code, three read-only, two writable).
SECTIONS = ROLES
PAGE = 4096


@dataclasses.dataclass(frozen=True)
class ResidueFacts:
    proof: str
    image_base: tuple       # values of every AsteriaConfiguredImageBase definition
    arena: tuple            # names of every *g_ia64BspRuntimeMemoryStorageE definition (none expected)
    sections: tuple         # (name, address, size) of every section named in SECTIONS


def geometry(facts: ResidueFacts) -> tuple[list[str], list[str]]:
    """Problems with the image geometry, or the proof's 14 arguments."""
    problems = []
    if len(facts.image_base) != 1:
        problems.append('%s has %d definitions, expected one' % (IMAGE_BASE, len(facts.image_base)))
    if facts.arena:
        problems.append('the image defines %s: the run-time memory object belongs in its reservation, '
                        'not in the image' % ', '.join(facts.arena))
    if problems:
        return problems, []
    base = facts.image_base[0]
    arguments, end = [], base
    for name in SECTIONS:
        found = [(address, size) for section, address, size in facts.sections if section == name]
        if len(found) != 1:
            problems.append('%d sections named %s, expected one' % (len(found), name))
            continue
        address, size = found[0]
        if address < base or size == 0:
            problems.append('%s (%#x, %#x bytes) is empty or below the image base %#x' % (name, address, size, base))
            continue
        arguments += ['%x' % (address - base), '%x' % size]
        end = max(end, (address + size + PAGE - 1) & ~(PAGE - 1))
    if problems:
        return problems, []
    return [], ['--final-image-residues', '%x' % (end - base)] + arguments


@functools.lru_cache(maxsize=None)
def run_proof(facts: ResidueFacts) -> tuple[tuple[str, ...], str]:
    problems, arguments = geometry(facts)
    if problems:
        return tuple(problems), ''
    result = subprocess.run([facts.proof] + arguments, capture_output=True, text=True)
    output = (result.stdout + result.stderr).strip()
    if result.returncode != 0:
        return ('the residue proof rejects the image (exit %d): %s' % (result.returncode, output),), output
    return (), output


@register
class Ia64FinalImageTranslationResidues(Property):
    """The final IA-64 image maps at every loader-aligned base.

    `AsteriaConfiguredImageBase` is defined once, and no symbol ending in
    `g_ia64BspRuntimeMemoryStorageE` is (the run-time memory object lives in
    its own reservation). The six note ownership ranges
    exist once, at or above the base, and are non-empty. From them (range
    RVAs and sizes, image size = the highest page-rounded section end minus
    the base) the production planners prove that every 64 KiB-aligned source
    residue maps after aligned placement with exactly 4 DTRs and 2 ITRs under
    an 8+8 PAL report, with exactly one runtime-page DTR. A PAL report below
    the architectural minimum fails at the worst residue."""
    isas = ('Ia64',)
    variants = ('firmware', 'firmware-test')

    def extract(self, context, variant):
        elf = ElfFile(context.artifact('%s:kernel_elf' % variant))
        defined = [symbol for symbol in elf.symbols[1:] if symbol.section_index != SHN_UNDEF]
        return ResidueFacts(
            str(context.artifact('firmware:residue_proof')),
            tuple(symbol.value for symbol in defined if symbol.name == IMAGE_BASE),
            tuple(symbol.name for symbol in defined if symbol.name.endswith(FORMER_ARENA_SUFFIX)),
            ownership(elf))

    def verify(self, facts):
        return list(run_proof(facts)[0])

    def summary(self, facts):
        return run_proof(facts)[1].removeprefix('PASS: ')

    def mutants(self, facts):
        sections = {name: (address, length) for name, address, length in facts.sections}
        yield 'the runtime arena back in the image', dataclasses.replace(
            facts, arena=('_ZN7Asteria4Boot4Uefi29' + FORMER_ARENA_SUFFIX,))
        yield 'missing image base', dataclasses.replace(facts, image_base=())
        yield 'duplicate image base', dataclasses.replace(facts, image_base=facts.image_base * 2)
        yield 'missing read-only tail', dataclasses.replace(
            facts, sections=tuple(section for section in facts.sections if section[0] != 'ReadOnlyTail'))
        yield 'duplicate .data', dataclasses.replace(facts, sections=facts.sections + (
            ('.data',) + sections['.data'],))
        yield 'empty .opd', dataclasses.replace(facts, sections=tuple(
            (name, address, 0 if name == 'ReadOnlyBody' else length) for name, address, length in facts.sections))
        # Rejected by the C++ proof itself (its geometry validation and the
        # production mapping planner's contiguous-section contract).
        yield 'section RVA not page-aligned', dataclasses.replace(facts, sections=tuple(
            (name, address + 8 if name == '.bss' else address, length) for name, address, length in facts.sections))
        yield 'a gap before .rodata', dataclasses.replace(facts, sections=tuple(
            (name, address + PAGE if name == '.note.asteria' else address, length)
            for name, address, length in facts.sections))
