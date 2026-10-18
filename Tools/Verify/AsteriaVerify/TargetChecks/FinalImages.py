# SPDX-License-Identifier: Apache-2.0
"""Closed final images for compile-check closures and independently linked kernels.

NucleusCore.elf is a generic, statically linked compile-check closure. Each
firmware's Kernel.elf is the closed ELF64 ET_EXEC with retained relocations
that the loader places. Loader ELF/PE contracts are checked separately.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import Mutate, Properties
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Elf import ET_EXEC, ET_REL, PF_R, PF_W, PF_X, PT_LOAD, SHT_RELA, STB_LOCAL, ElfFile, Section, \
    Segment

# Handlers the nucleus must define for C++ semantics it relies on.
REQUIRED_HANDLERS = ('__cxa_pure_virtual', '_ZN7Asteria8Platform9FatalTrapEv')


@dataclasses.dataclass(frozen=True)
class ImageFacts:
    isa: str
    header: Properties.Header
    expected_type: int
    undefined: tuple[str, ...]
    relocation_sections: tuple[Section, ...]
    segments: tuple[Segment, ...]
    handlers: tuple[tuple[str, tuple[Properties.SymbolFacts, ...]], ...]
    contract: Properties.FunctionContract


class _ImageProperty(Property):
    def facts(self, context, path, *, expected_type: int, handlers=(), relocations=False) -> ImageFacts:
        elf = ElfFile(path)
        return ImageFacts(context.isa, Properties.header_of(elf), expected_type, tuple(Properties.undefined_names(elf)),
                          tuple(elf.relocation_sections()) if not relocations else (), tuple(elf.segments),
                          tuple((name, tuple(Properties.symbol_facts(elf, name))) for name in handlers),
                          Properties.FunctionContract(Properties.EXPORTED,
                                                      alignment=16 if context.isa == 'Ia64' else 1))

    def verify(self, facts):
        problems = Properties.header_problems(facts.header, facts.isa, facts.expected_type)
        problems += Properties.undefined_problems(facts.undefined, ())
        problems += Properties.relocation_section_problems(facts.relocation_sections)
        if facts.expected_type == ET_EXEC:
            problems += Properties.load_segment_problems(facts.segments)
        for name, entries in facts.handlers:
            problems += facts.contract.problems(entries, name)
        return problems

    def mutants(self, facts):
        yield 'undefined weak reference', dataclasses.replace(facts, undefined=facts.undefined + ('weak_mutant',))
        yield 'relocatable type', dataclasses.replace(facts, header=dataclasses.replace(
            facts.header, type=ET_REL if facts.expected_type == ET_EXEC else ET_EXEC))
        if facts.expected_type == ET_EXEC:
            index, load = next((index, s) for index, s in enumerate(facts.segments) if s.type == PT_LOAD)
            yield 'writable and executable segment', dataclasses.replace(
                facts, segments=Mutate.replace_at(facts.segments, index, flags=PF_R | PF_W | PF_X))
            yield 'segment file size exceeds memory size', dataclasses.replace(
                facts, segments=Mutate.replace_at(facts.segments, index, file_size=load.memory_size + 1))
            yield 'no load segment', dataclasses.replace(
                facts, segments=tuple(s for s in facts.segments if s.type != PT_LOAD))
        if facts.handlers:
            name, entries = facts.handlers[0]
            yield 'missing %s' % name, dataclasses.replace(facts, handlers=((name, ()),) + facts.handlers[1:])
            local = (dataclasses.replace(entries[0], symbol=dataclasses.replace(entries[0].symbol, binding=STB_LOCAL)),)
            yield 'local %s' % name, dataclasses.replace(facts, handlers=((name, local),) + facts.handlers[1:])


@register
class ClosureImage(_ImageProperty):
    """The compile-check nucleus links statically: an ELF64 executable of the
    target machine with no undefined symbol (weak ones included), no
    remaining relocation section, sane PT_LOAD segments of which none is
    writable and executable, and exactly one GLOBAL DEFAULT definition of
    the pure-virtual and fatal-trap handlers."""
    variants = ('O0', 'O2')

    def extract(self, context, variant):
        return self.facts(context, context.artifact('%s:nucleus_elf' % variant), expected_type=ET_EXEC,
                          handlers=REQUIRED_HANDLERS)

    def mutants(self, facts):
        yield from super().mutants(facts)
        yield 'leftover relocation section', dataclasses.replace(facts, relocation_sections=(
            Section(99, '.rela.text', SHT_RELA, 0, 0, 0, 24, 0, 1, 8, 24),))


@register
class FirmwareKernelImage(_ImageProperty):
    """The firmware kernel is closed: the IA-64 `AsteriaBoot.elf` is an
    executable with no undefined symbol and sane, W^X PT_LOAD segments (its
    relocations are kept on purpose for the PE normalizer); the AMD64
    `Amd64Kernel.o` core is a relocatable object with no undefined symbol.
    Like the compile-check closure, it defines the pure-virtual and
    fatal-trap handlers exactly once as GLOBAL DEFAULT functions, so a
    pure-virtual slot of any vtable the firmware may carry resolves to the
    project's fail-stop rather than to nothing. Checked for both firmwares."""
    variants = ('firmware', 'firmware-test')

    def extract(self, context, variant):
        return self.facts(context, context.artifact('%s:kernel_elf' % variant), relocations=True,
                          expected_type=ET_EXEC, handlers=REQUIRED_HANDLERS)
