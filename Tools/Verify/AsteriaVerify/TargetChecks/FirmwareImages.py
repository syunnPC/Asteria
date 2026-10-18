# SPDX-License-Identifier: Apache-2.0
"""Firmware image structure and relocation checks.

They run in every firmware tree (preferred and relocated image base, every
diagnostic-probe combination) and on both firmwares of a tree, the
production firmware and the test firmware whose initial user image
is the guest test program, because each is linked and converted to PE on its
own.

IA-64: the `--emit-relocs` kernel ELF (`AsteriaBoot.elf`,
`AsteriaBootTest.elf`) may carry only the relocation types NormalizeIa64Pe
represents and exactly one function-pointer relocation (the entry descriptor
anchor); both loader decoders are linked; the entry is EfiMain's descriptor at the start of
`.opd`; the PE has the six-section layout.

AMD64: every absolute reference of the closed core `Amd64Kernel.o` has
exactly one DIR64 base relocation and the right preferred-base value in the
PE, and every PC-relative reference holds the right displacement.

Both: the boot file is a PE32+ EFI application for the target machine.
"""
from __future__ import annotations

import dataclasses
import struct

from AsteriaVerify import Layout, Pe, Properties, Relocations
from AsteriaVerify.TargetChecks.KernelBoundary import loader_unit
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Elf import ET_REL, SHT_SYMTAB, STB_WEAK, STT_OBJECT, ElfFile
from AsteriaVerify.Mutate import replace_at

# Relocation types NormalizeIa64Pe turns into PE base relocations or
# link-time constants (reviewed; GCC under -mno-sdata -fno-pic
# -fno-jump-tables). FPTR64LSB is allowed once, for the entry descriptor.
IA64_ALLOWED_RELOCATIONS = frozenset({'GPREL22', 'GPREL64I', 'PCREL21B', 'DIR64LSB', 'IPLTLSB', 'IMM64', 'FPTR64LSB'})
IA64_RELOCATION_CODES = {name: code for code, name in Relocations.IA64.items()}
IA64_ENTRY_ANCHOR = 'EfiEntryDescriptorReference'
IA64_ENTRY_FUNCTION = 'EfiMain'
IA64_ENTRY_DESCRIPTOR = 'AsteriaEfiEntryDescriptor'
NORMALIZER_SOURCE = 'Tools/Firmware/NormalizeIa64Pe.cpp'

# The two loaders the boot path uses to decode the boot-file user image.
IA64_DECODERS = (
    '_ZN7Asteria10Executable3Elf10Elf64Image14DecodeLoadPlanENS1_13Elf64ByteViewERKNS1_19Elf64LoadPlanLimitsE'
    'RNS1_13Elf64LoadPlanE',
    '_ZN7Asteria8Platform4Ia6410Executable12Ia64P64Image14DecodeLoadPlanENS_10Executable3Elf13Elf64ByteViewE'
    'RKNS5_19Elf64LoadPlanLimitsEhhRNS2_15Ia64P64LoadPlanE',
)

PE_MACHINES = {'Ia64': Pe.MACHINE_IA64, 'Amd64': Pe.MACHINE_AMD64}
IA64_PE_SECTIONS = ('.text', '.rodata', '.opd', '.reloc', '.data', '.bss')
AMD64_PE_SECTIONS = ('.text', '.rdata', '.rodata', '.bss', '.idata', '.reloc', '.data')
AMD64_OPTIONAL_HEADER_SIZE = 240
R_X86_64_64, R_X86_64_PC32, R_X86_64_PLT32 = 1, 2, 4


# The firmwares of a tree: production, and the test firmware (same kernel objects, guest user image).
FIRMWARES = ('firmware', 'firmware-test')


class _FirmwareProperty(Property):
    isas = ('Ia64',)
    variants = FIRMWARES


# IA-64 relocation types -------------------------------------------------------

@dataclasses.dataclass(frozen=True, order=True)
class Ia64Relocation:
    section: str        # The SHT_RELA section holding the record (`source:.rela.x` in objects)
    offset: int         # R_offset: an image address, or an offset in an object's section
    type: str
    symbol: str
    addend: int


@dataclasses.dataclass(frozen=True)
class RelocationUnit:
    """The relocations of the linked image, or of all its input objects."""
    label: str
    relocations: tuple          # Ia64Relocation, sorted
    anchors: tuple              # (value, section) of each EfiEntryDescriptorReference definition


@dataclasses.dataclass(frozen=True)
class InputRelocationFacts:
    allowed: frozenset
    supported: frozenset        # Type codes NormalizeIa64Pe handles
    units: tuple                # RelocationUnit: the image, then the input objects


def _relocation_unit(label, elves) -> RelocationUnit:
    relocations, anchors = [], []
    for prefix, elf in elves:
        for rela in elf.relocation_sections():
            for relocation in elf.relocations(rela):
                symbol = relocation.symbol
                relocations.append(Ia64Relocation(prefix + rela.name, relocation.offset,
                                                  Relocations.name(elf.machine, relocation.type),
                                                  '' if symbol is None else symbol.name, relocation.addend))
        anchors += [(symbol.value, prefix + getattr(elf.section_of(symbol), 'name', '?'))
                    for symbol in elf.definitions(IA64_ENTRY_ANCHOR)]
    return RelocationUnit(label, tuple(sorted(relocations)), tuple(anchors))


def _unit_problems(unit: RelocationUnit, allowed) -> list[str]:
    problems = [] if unit.relocations else ['%s: no relocations (is --emit-relocs missing?)' % unit.label]
    unexpected: dict[str, list] = {}
    for relocation in unit.relocations:
        if relocation.type not in allowed:
            unexpected.setdefault(relocation.type, []).append(relocation)
    for kind, sites in sorted(unexpected.items()):
        problems.append('%s: %d unsupported %s relocation(s), first at %#x in %s against %s'
                        % (unit.label, len(sites), kind, sites[0].offset, sites[0].section, sites[0].symbol or '-'))
    if len(unit.anchors) != 1:
        return problems + ['%s: %s has %d definitions, expected one' % (unit.label, IA64_ENTRY_ANCHOR,
                                                                        len(unit.anchors))]
    anchor, anchor_section = unit.anchors[0]
    prefix, separator, section_name = anchor_section.rpartition(':')
    pointers = [relocation for relocation in unit.relocations if relocation.type == 'FPTR64LSB']
    if len(pointers) != 1:
        return problems + ['%s: %d FPTR64LSB relocations, expected exactly one (the entry descriptor anchor)'
                           % (unit.label, len(pointers))]
    pointer = pointers[0]
    expected = (prefix + separator + '.rela.data', anchor, IA64_ENTRY_FUNCTION, 0)
    if section_name != '.data' or (pointer.section, pointer.offset, pointer.symbol, pointer.addend) != expected:
        problems.append('%s: the FPTR64LSB is in %s at %#x against %s%+d, expected %s at %s (%#x in %s) '
                        'against %s+0' % (unit.label, pointer.section, pointer.offset, pointer.symbol, pointer.addend,
                                          expected[0], IA64_ENTRY_ANCHOR, anchor, anchor_section, IA64_ENTRY_FUNCTION))
    return problems


@register
class FirmwareIa64InputRelocations(_FirmwareProperty):
    """The IA-64 firmware carries only relocation types NormalizeIa64Pe
    represents, so every site stays correct when UEFI relocates the image.

    In the `--emit-relocs` kernel ELF of the firmware, and in its input objects
    (the kernel objects), every relocation type
    is one of GPREL22, GPREL64I, PCREL21B, DIR64LSB, IPLTLSB, IMM64 and
    FPTR64LSB, each a type the normalizer declares (parsed from
    NormalizeIa64Pe.cpp). Exactly one FPTR64LSB exists: in `.rela.data`, at
    `EfiEntryDescriptorReference`, against EfiMain with addend 0; it creates
    the single `.opd` descriptor used as the image entry. Any other function
    pointer would need a descriptor the image does not have."""

    def extract(self, context, variant):
        kernel = context.artifact('%s:loader_elf' % variant)
        image = _relocation_unit(kernel.name, [('', ElfFile(kernel))])
        inputs = [(source + ':', ElfFile(path)) for source, path in sorted((source, path) for source, path in context.objects(variant).items() if loader_unit(source) or source.startswith("Runtime/"))]
        supported = frozenset(value for name, value in Layout.constants(Layout.read(
            context.root / NORMALIZER_SOURCE)).items() if name.startswith('Ia64Relocation'))
        return InputRelocationFacts(IA64_ALLOWED_RELOCATIONS, supported,
                                    (image, _relocation_unit('input objects', inputs)))

    def verify(self, facts):
        problems = [] if facts.supported else ['cannot read the relocation types %s supports' % NORMALIZER_SOURCE]
        problems += ['allowed type %s is not handled by %s' % (name, NORMALIZER_SOURCE)
                     for name in sorted(facts.allowed) if IA64_RELOCATION_CODES.get(name) not in facts.supported]
        return problems + [problem for unit in facts.units for problem in _unit_problems(unit, facts.allowed)]

    def mutants(self, facts):
        image, inputs = facts.units

        def with_unit(index, relocations=None, anchors=None):
            unit = facts.units[index]
            return dataclasses.replace(facts, units=replace_at(
                facts.units, index, relocations=unit.relocations if relocations is None else tuple(sorted(relocations)),
                anchors=unit.anchors if anchors is None else anchors))

        relocations = list(image.relocations)
        pointer_index = next(index for index, r in enumerate(relocations) if r.type == 'FPTR64LSB')
        pointer = relocations[pointer_index]
        first_text = next(r for r in relocations if r.section == '.rela.text')

        def with_pointer(**changes):
            return with_unit(0, replace_at(relocations, pointer_index, **changes))

        yield 'LTOFF22X relocation', with_unit(0, relocations + [dataclasses.replace(first_text, type='LTOFF22X')])
        yield 'second FPTR64LSB', with_unit(0, relocations + [dataclasses.replace(pointer, offset=pointer.offset + 8)])
        yield 'FPTR64LSB at another address', with_pointer(offset=pointer.offset + 8)
        yield 'FPTR64LSB against another function', with_pointer(symbol=first_text.symbol)
        yield 'FPTR64LSB with addend 8', with_pointer(addend=8)
        yield 'FPTR64LSB in another section', with_pointer(section='.rela.rodata')
        yield 'no FPTR64LSB', with_unit(0, [r for index, r in enumerate(relocations) if index != pointer_index])
        yield 'duplicate anchor symbol', with_unit(0, anchors=image.anchors * 2)
        yield 'normalizer drops IPLTLSB', dataclasses.replace(
            facts, supported=facts.supported - {IA64_RELOCATION_CODES['IPLTLSB']})
        objects = list(inputs.relocations)
        object_pointer = next(r for r in objects if r.type == 'FPTR64LSB')
        other = next(r for r in objects if r.section.rpartition(':')[0] != object_pointer.section.rpartition(':')[0])
        yield 'SEGREL64LSB in an input object', with_unit(1, objects + [dataclasses.replace(
            other, section=other.section.rpartition(':')[0] + ':.rela.IA_64.unwind', type='SEGREL64LSB')])
        yield 'FPTR64LSB in another input object', with_unit(1, objects + [dataclasses.replace(
            other, type='FPTR64LSB', symbol=IA64_ENTRY_FUNCTION, addend=0)])


# IA-64 loader decoders ----------------------------------------------------------------

@register
class FirmwareIa64DecoderLinkContract(_FirmwareProperty):
    """The firmware links exactly one code definition of each loader decoder
    the boot path uses on the boot-file user image
    (`Elf64Image::DecodeLoadPlan` and `Ia64P64Image::DecodeLoadPlan`): one
    STT_FUNC in an executable section per mangled name. A missing decoder
    means the user-image load path was compiled out or not linked."""

    def extract(self, context, variant):
        elf = ElfFile(context.artifact('%s:kernel_elf' % variant))
        return tuple((name, tuple(Properties.symbol_facts(elf, name))) for name in IA64_DECODERS)

    def verify(self, facts):
        return [problem for name, entries in facts
                for problem in Properties.unique_code_definition_problems(list(entries), name)]

    def mutants(self, facts):
        (elf64, entries), p64 = facts
        yield 'omits the Elf64 decoder', ((elf64, ()), p64)
        yield 'omits the P64 decoder', (facts[0], (p64[0], ()))
        yield 'duplicate decoder definition', ((elf64, entries + entries), p64)
        yield 'data-typed decoder definition', ((elf64, (dataclasses.replace(entries[0], symbol=dataclasses.replace(
            entries[0].symbol, type=STT_OBJECT)),) + entries[1:]), p64)


# IA-64 entry descriptor -----------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class EntryFacts:
    entry: int
    descriptor: tuple           # (value, section name) of each AsteriaEfiEntryDescriptor definition
    words: tuple | None         # The two 64-bit words at the descriptor
    function: tuple             # Values of the EfiMain definitions
    global_pointer: tuple       # Values of the __gp definitions
    pe_entry: int | None        # The PE AddressOfEntryPoint + ImageBase


@register
class FirmwareIa64EntryDescriptor(_FirmwareProperty):
    """IA-64 UEFI enters the image through EfiMain's function descriptor.

    e_entry is the value of the unique `AsteriaEfiEntryDescriptor`, which
    lies in `.opd` (Application.lds ENTRY); the descriptor's first word is
    the address of EfiMain and its second the final `__gp`; and the PE entry
    point (ImageBase + AddressOfEntryPoint) is the same descriptor."""

    def extract(self, context, variant):
        elf = ElfFile(context.artifact('%s:loader_elf' % variant))
        descriptors = tuple((s.value, getattr(elf.section_of(s), 'name', None))
                            for s in elf.definitions(IA64_ENTRY_DESCRIPTOR))
        words = None
        if len(descriptors) == 1:
            try:
                words = struct.unpack('<QQ', elf.read_address(descriptors[0][0], 16))
            except Exception:
                words = None
        pe = Pe.PeFile(context.artifact('%s:efi' % variant))
        return EntryFacts(elf.entry, descriptors, words, tuple(s.value for s in elf.definitions(IA64_ENTRY_FUNCTION)),
                          tuple(s.value for s in elf.definitions('__gp')), pe.header.image_base + pe.header.entry_rva)

    def verify(self, facts):
        if len(facts.descriptor) != 1:
            return ['%s has %d definitions, expected one' % (IA64_ENTRY_DESCRIPTOR, len(facts.descriptor))]
        address, section = facts.descriptor[0]
        problems = []
        if facts.entry != address:
            problems.append('e_entry %#x is not %s (%#x)' % (facts.entry, IA64_ENTRY_DESCRIPTOR, address))
        if section != '.opd':
            problems.append('%s lies in %s, not .opd' % (IA64_ENTRY_DESCRIPTOR, section))
        if len(facts.function) != 1 or len(facts.global_pointer) != 1:
            problems.append('EfiMain or __gp is not defined exactly once')
        elif facts.words != (facts.function[0], facts.global_pointer[0]):
            problems.append('the entry descriptor holds %s, expected (EfiMain %#x, __gp %#x)'
                            % (facts.words and tuple(hex(word) for word in facts.words),
                               facts.function[0], facts.global_pointer[0]))
        if facts.pe_entry != address:
            problems.append('the PE entry point %s is not the entry descriptor %#x'
                            % (facts.pe_entry and hex(facts.pe_entry), address))
        return problems

    def mutants(self, facts):
        address, section = facts.descriptor[0]
        yield 'entry moved by 16', dataclasses.replace(facts, entry=facts.entry + 16)
        yield 'missing descriptor symbol', dataclasses.replace(facts, descriptor=())
        yield 'duplicate descriptor symbol', dataclasses.replace(facts, descriptor=facts.descriptor * 2)
        yield 'descriptor outside .opd', dataclasses.replace(facts, descriptor=((address, '.rodata'),))
        yield 'descriptor names another function', dataclasses.replace(
            facts, words=(facts.words[0] + 16, facts.words[1]))
        yield 'descriptor carries another gp', dataclasses.replace(facts, words=(facts.words[0], 0))
        yield 'PE enters elsewhere', dataclasses.replace(facts, pe_entry=facts.pe_entry + 16)


# PE image type and IA-64 sections -----------------------------------------------------------

@register
class FirmwarePeImageType(Property):
    """The boot file is a PE32+ EFI application for the target machine:
    'MZ' and 'PE\\0\\0' signatures, Machine 0x0200 (IA-64) or 0x8664
    (AMD64), optional-header magic 0x20B, Subsystem 10. Otherwise firmware
    refuses to load BOOTIA64.EFI / BOOTX64.EFI."""
    variants = FIRMWARES

    def extract(self, context, variant):
        header = Pe.PeFile(context.artifact('%s:efi' % variant)).header
        return context.isa, (header.machine, header.magic, header.subsystem)

    def verify(self, facts):
        isa, (machine, magic, subsystem) = facts
        problems = []
        if machine != PE_MACHINES[isa]:
            problems.append('Machine is %#x, expected %#x' % (machine, PE_MACHINES[isa]))
        if magic != Pe.MAGIC_PE32_PLUS:
            problems.append('optional-header magic is %#x, not PE32+' % magic)
        if subsystem != Pe.SUBSYSTEM_EFI_APPLICATION:
            problems.append('Subsystem is %d, not an EFI application (10)' % subsystem)
        return problems

    def mutants(self, facts):
        isa, (machine, magic, subsystem) = facts
        yield 'EFI runtime driver subsystem', (isa, (machine, magic, 12))
        yield 'PE32 magic', (isa, (machine, Pe.MAGIC_PE32, subsystem))
        yield 'other machine', (isa, (PE_MACHINES['Amd64' if isa == 'Ia64' else 'Ia64'], magic, subsystem))


@register
class FirmwareIa64PeSections(_FirmwareProperty):
    """The IA-64 PE keeps exactly `.text.rodata.opd.reloc.data.bss` in
    that order. `.rodata` (constants and the embedded user image) is
    initialized, readable, not writable, not executable and not code; `.text`
    is readable executable code and not writable; `.data` and `.bss` are
    readable and writable and not executable."""
    RULES = {  # Section -> (required, forbidden) characteristics
        '.text': (Pe.SCN_CNT_CODE | Pe.SCN_MEM_READ | Pe.SCN_MEM_EXECUTE, Pe.SCN_MEM_WRITE),
        '.rodata': (Pe.SCN_CNT_INITIALIZED_DATA | Pe.SCN_MEM_READ,
                    Pe.SCN_MEM_WRITE | Pe.SCN_MEM_EXECUTE | Pe.SCN_CNT_CODE),
        '.data': (Pe.SCN_MEM_READ | Pe.SCN_MEM_WRITE, Pe.SCN_MEM_EXECUTE | Pe.SCN_CNT_CODE),
        '.bss': (Pe.SCN_MEM_READ | Pe.SCN_MEM_WRITE, Pe.SCN_MEM_EXECUTE | Pe.SCN_CNT_CODE),
    }

    def extract(self, context, variant):
        return tuple((s.name, s.characteristics, s.raw_size)
                     for s in Pe.PeFile(context.artifact('%s:efi' % variant)).sections)

    def verify(self, facts):
        names = tuple(name for name, _, _ in facts)
        if names != IA64_PE_SECTIONS:
            return ['PE sections are %s, expected %s' % (' '.join(names), ' '.join(IA64_PE_SECTIONS))]
        problems = []
        for name, characteristics, raw_size in facts:
            if name not in self.RULES:
                continue
            required, forbidden = self.RULES[name]
            if characteristics & required != required:
                problems.append('%s characteristics %#x lack %#x'
                                % (name, characteristics, required & ~characteristics))
            if characteristics & forbidden:
                problems.append('%s characteristics %#x carry %#x'
                                % (name, characteristics, characteristics & forbidden))
            if name == '.rodata' and not raw_size:
                problems.append('.rodata has no raw data')
        return problems

    def mutants(self, facts):
        index = {name: position for position, (name, _, _) in enumerate(facts)}

        def with_section(name, flags=0, raw=None):
            _, characteristics, raw_size = facts[index[name]]
            return (facts[:index[name]] + ((name, characteristics | flags, raw_size if raw is None else raw),)
                    + facts[index[name] + 1:])

        yield 'added .got', facts[:5] + (('.got', Pe.SCN_CNT_INITIALIZED_DATA | Pe.SCN_MEM_READ | Pe.SCN_MEM_WRITE,
                                          0x200),) + facts[5:]
        yield 'writable .rodata', with_section('.rodata', Pe.SCN_MEM_WRITE)
        yield 'executable .rodata', with_section('.rodata', Pe.SCN_MEM_EXECUTE)
        yield 'swapped .rodata and .opd', (facts[0], facts[2], facts[1]) + facts[3:]
        yield 'writable .text', with_section('.text', Pe.SCN_MEM_WRITE)
        yield 'executable .data', with_section('.data', Pe.SCN_MEM_EXECUTE)
        yield '.rodata without raw data', with_section('.rodata', raw=0)


# AMD64 PE relocation proof ---------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class CoreRelocation:
    """One RELA record of the closed AMD64 core with the value the PE stores
    at its site (None when the site lies outside the section's raw data)."""
    section: str            # Target section of the RELA section
    offset: int
    kind: int
    binding: int | None     # Binding of the target symbol (None: no symbol)
    symbol_section: str | None
    value: int
    addend: int
    stored: int | None


@dataclasses.dataclass(frozen=True)
class Amd64PeFacts:
    core_problems: tuple            # Structural problems of the ELF core
    header: tuple                   # (machine, section count, optional header size, magic)
    image_base: int
    sections: tuple                 # (name, rva, virtual size, raw size) in table order
    relocation_directory: tuple     # (rva, size)
    blocks: tuple | None            # Pe.RelocationBlock, or None when the framing is malformed
    framing_problem: str | None
    references: tuple               # CoreRelocation


@register
class FirmwareAmd64PeRelocations(Property):
    """Every reference in the closed AMD64 core survives the direct ELF->PE
    link at any load address.

    The core `Amd64Kernel.o` may hold only R_X86_64_64, PC32 and PLT32
    relocations against symbols defined in sections the PE keeps (no weak,
    undefined or absolute targets, core sections at address 0). The PE has
    exactly `.text.rdata.rodata.bss.idata.reloc.data` in that order, a
    240-byte PE32+ optional header, and a base-relocation directory that is
    the `.reloc` section: page-aligned blocks of DIR64 entries (zero padding
    only), no duplicate and no trailing partial block. Each R_X86_64_64 site
    stores the preferred-base value and has exactly one DIR64 entry; the
    DIR64 sites are exactly the absolute references. Each PC-relative site
    stores the right displacement."""
    isas = ('Amd64',)
    variants = FIRMWARES

    def extract(self, context, variant):
        core = ElfFile(context.artifact('%s:loader_elf' % variant))
        pe = Pe.PeFile(context.artifact('%s:efi' % variant))
        by_name = {s.name: s for s in pe.sections}
        core_problems = [] if (core.type, core.machine) == (ET_REL, 62) else [
            'the core is not an AMD64 relocatable object']
        references = []
        for rela in core.relocation_sections():
            if rela.entry_size != 24 or rela.size % 24:
                core_problems.append('%s has malformed entries' % rela.name)
                continue
            target = core.sections[rela.info]
            if core.sections[rela.link].type != SHT_SYMTAB:
                core_problems.append('%s does not link to the symbol table' % rela.name)
            if target.address != 0:
                core_problems.append('core section %s is not at address 0' % target.name)
            if target.name not in by_name:
                core_problems.append('core section %s has no PE section' % target.name)
                continue
            pe_section = by_name[target.name]
            if target.size > pe_section.virtual_size:
                core_problems.append('core section %s exceeds its PE section' % target.name)
            for relocation in core.relocations(rela):
                symbol = relocation.symbol
                symbol_section = None
                if symbol is not None and 0 < symbol.section_index < len(core.sections):
                    symbol_section = core.sections[symbol.section_index].name
                width = 8 if relocation.type == R_X86_64_64 else 4
                stored = None
                if relocation.offset + width <= min(target.size, pe_section.raw_size):
                    site = pe_section.raw_offset + relocation.offset
                    stored = struct.unpack('<Q' if width == 8 else '<i', pe.data[site:site + width])[0]
                references.append(CoreRelocation(
                    target.name, relocation.offset, relocation.type,
                    None if symbol is None else symbol.binding, symbol_section,
                    0 if symbol is None else symbol.value, relocation.addend, stored))
        blocks, framing = None, None
        try:
            blocks = tuple(Pe.relocation_blocks(pe.base_relocation_data()))
        except Pe.PeError as error:
            framing = str(error)
        header = pe.header
        return Amd64PeFacts(tuple(core_problems),
                            (header.machine, header.section_count, header.optional_header_size, header.magic),
                            header.image_base, tuple((s.name, s.rva, s.virtual_size, s.raw_size) for s in pe.sections),
                            header.directory(Pe.DIRECTORY_BASE_RELOCATION), blocks, framing, tuple(references))

    def summary(self, facts):
        absolute = sum(1 for reference in facts.references if reference.kind == R_X86_64_64)
        return 'the PE covers %d absolute and %d relative references of the core' % (
            absolute, len(facts.references) - absolute)

    def verify(self, facts):
        problems = list(facts.core_problems)
        machine, count, optional_size, magic = facts.header
        if (machine, optional_size, magic) != (Pe.MACHINE_AMD64, AMD64_OPTIONAL_HEADER_SIZE, Pe.MAGIC_PE32_PLUS):
            problems.append('PE header (machine %#x, optional header %d bytes, magic %#x) is not AMD64 PE32+'
                            % (machine, optional_size, magic))
        names = tuple(name for name, _, _, _ in facts.sections)
        if names != AMD64_PE_SECTIONS or count != len(AMD64_PE_SECTIONS):
            return problems + ['PE sections are %s, expected %s' % (' '.join(names), ' '.join(AMD64_PE_SECTIONS))]
        pe = {name: (rva, virtual_size, raw_size) for name, rva, virtual_size, raw_size in facts.sections}
        reloc_rva, reloc_size = facts.relocation_directory
        if reloc_rva != pe['.reloc'][0] or reloc_size > pe['.reloc'][1]:
            problems.append('the base-relocation directory (%#x, %#x) is not the .reloc section'
                            % (reloc_rva, reloc_size))
        if facts.framing_problem is not None or facts.blocks is None:
            return problems + ['malformed base relocations: %s' % facts.framing_problem]
        actual = set()
        for block in facts.blocks:
            if block.page_rva & 0xFFF or block.size & 3:
                problems.append('base-relocation block at %#x (size %#x) is not page-aligned and 32-bit framed'
                                % (block.page_rva, block.size))
            for entry in block.entries:
                if entry == 0:
                    continue
                if entry >> 12 != Pe.REL_BASED_DIR64:
                    problems.append('base relocation %#06x in block %#x is not DIR64' % (entry, block.page_rva))
                    continue
                site = block.page_rva + (entry & 0xFFF)
                if site in actual:
                    problems.append('duplicate DIR64 relocation at RVA %#x' % site)
                actual.add(site)
        expected = set()
        for reference in facts.references:
            label = '%s+%#x' % (reference.section, reference.offset)
            if reference.kind not in (R_X86_64_64, R_X86_64_PC32, R_X86_64_PLT32):
                problems.append('%s: unsupported relocation type %d' % (label, reference.kind))
            elif reference.binding == STB_WEAK:
                problems.append('%s: weak target in the closed core' % label)
            elif reference.symbol_section not in pe:
                problems.append('%s: target symbol is undefined, absolute or in a section the PE lacks' % label)
            elif reference.stored is None:
                problems.append('%s: site lies outside the raw data of %s' % (label, reference.section))
            else:
                section_rva = pe[reference.section][0]
                resolved = facts.image_base + pe[reference.symbol_section][0] + reference.value + reference.addend
                if reference.kind == R_X86_64_64:
                    site = section_rva + reference.offset
                    if site in expected:
                        problems.append('%s: duplicate absolute reference' % label)
                    expected.add(site)
                    if reference.stored != resolved:
                        problems.append('%s: PE stores %#x, expected %#x' % (label, reference.stored, resolved))
                else:
                    displacement = resolved - (facts.image_base + section_rva + reference.offset)
                    if not -(1 << 31) <= displacement < (1 << 31) or reference.stored != displacement:
                        problems.append('%s: PE stores displacement %#x, expected %#x'
                                        % (label, reference.stored, displacement))
        if not expected:
            problems.append('the core has no absolute reference (empty proof)')
        if actual != expected:
            problems.append('DIR64 coverage mismatch: missing %s, extra %s'
                            % ([hex(site) for site in sorted(expected - actual)[:8]],
                               [hex(site) for site in sorted(actual - expected)[:8]]))
        return problems

    def mutants(self, facts):
        blocks = list(facts.blocks)
        first = blocks[0]
        entry_index = next(index for index, entry in enumerate(first.entries) if entry)
        entry = first.entries[entry_index]

        def with_entries(entries, size=None):
            return dataclasses.replace(facts, blocks=(Pe.RelocationBlock(
                first.page_rva, first.size if size is None else size, tuple(entries)),) + tuple(blocks[1:]))

        def with_entry(value):
            return with_entries(first.entries[:entry_index] + (value,) + first.entries[entry_index + 1:])

        yield 'missing DIR64 fixup', with_entry(0)
        yield 'HIGHLOW fixup', with_entry((Pe.REL_BASED_HIGHLOW << 12) | (entry & 0xFFF))
        used = {e & 0xFFF for e in first.entries if e}
        spare = next(offset for offset in range(0, 0x1000, 8) if offset not in used)
        yield 'extra DIR64 at a non-reference site', with_entries(
            list(first.entries) + [(Pe.REL_BASED_DIR64 << 12) | spare, 0], first.size + 4)
        yield 'duplicate DIR64 entry', with_entries(list(first.entries) + [entry, 0], first.size + 4)
        yield 'unaligned block', dataclasses.replace(facts, blocks=(
            Pe.RelocationBlock(first.page_rva + 8, first.size, first.entries),) + tuple(blocks[1:]))
        yield 'malformed framing', dataclasses.replace(facts, blocks=None, framing_problem='mutant')
        references = facts.references
        absolute = next(i for i, r in enumerate(references) if r.kind == R_X86_64_64)
        for description, changes in (('wrong stored absolute value', dict(stored=references[absolute].stored + 8)),
                                     ('weak absolute target', dict(binding=STB_WEAK)),
                                     ('undefined absolute target', dict(symbol_section=None)),
                                     ('unsupported relocation type', dict(kind=9))):
            yield description, dataclasses.replace(facts, references=replace_at(references, absolute, **changes))
        relative = next((i for i, r in enumerate(references) if r.kind != R_X86_64_64), None)
        if relative is not None:
            yield 'wrong displacement', dataclasses.replace(facts, references=replace_at(
                references, relative, stored=references[relative].stored + 1))
        sections = list(facts.sections)
        sections[1], sections[2] = sections[2], sections[1]
        yield 'reordered PE sections', dataclasses.replace(facts, sections=tuple(sections))
        machine, count, optional_size, magic = facts.header
        yield 'PE32 optional header', dataclasses.replace(facts, header=(machine, count, 224, magic))
        yield 'relocation directory elsewhere', dataclasses.replace(
            facts, relocation_directory=(facts.relocation_directory[0] + 0x1000, facts.relocation_directory[1]))
