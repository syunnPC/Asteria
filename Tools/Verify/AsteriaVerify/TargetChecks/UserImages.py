# SPDX-License-Identifier: Apache-2.0
"""Initial user ELF images, both ISAs, at O0, O2 and in both firmwares.

The initial user image is the PL3 / ring-3 program the kernel loads at its
link address without a relocator or dynamic linker: `MinimalUserImage.elf`
in the production firmware and the compile-check variants (artifacts
`firmware:user_elf`, `O0:user_elf`, `O2:user_elf`), and the guest test
program `GuestTests.elf` in the test firmware (`firmware-test:user_elf`),
which links the same runtime objects around another C++ entry. The loader
contract (header, entry, sections, load and BSS geometry, link closure) and,
on IA-64, the system-call `break`, the hand-written entry and integer-only
code (no floating-point register, which PL3 cannot use) hold for every
image; the call structure of the production program (Main calls the runtime,
which queries, sends and receives in order) only for the production images.
The goldens Tests/Golden/Ia64/User/AsteriaIa64UserSystemCallTransport.dis and
AsteriaIa64MinimalUserImageEntry.dis pin the two asm routines of the runtime,
and Tests/Golden/Ia64/Guest/AsteriaGuestIa64CheckedSystemCallTransport.dis the
guest program's checked transport.
"""
from __future__ import annotations

import dataclasses
import struct

from AsteriaVerify import Layout, Mutate, Properties, Subjects
from AsteriaVerify.Checks import AcceptanceError, Property, register
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Elf import (ET_EXEC, ET_REL, PF_R, PF_W, PF_X, PT_DYNAMIC, PT_LOAD, SHF_ALLOC, SHF_EXECINSTR,
                               SHF_WRITE, SHT_NOBITS, SHT_PROGBITS, SHT_REL, SHT_RELA, STB_GLOBAL, STB_LOCAL, STT_FUNC,
                               STT_OBJECT, STV_DEFAULT, ElfFile, Section, Segment, Symbol, load)

SHT_DYNAMIC = 6
LIMIT = 1 << 64

ENTRY = {'Ia64': 'AsteriaIa64MinimalUserImageEntry', 'Amd64': 'AsteriaAmd64MinimalUserImageEntry'}
DESCRIPTOR = 'AsteriaIa64MinimalUserImageEntryDescriptor'
DESCRIPTOR_SECTION = '.AsteriaUserEntryDescriptor'
MAIN = 'AsteriaIa64MinimalUserImageMain'
TRANSPORT = 'AsteriaIa64UserSystemCallTransport'
# Every image, and the production images only (the guest program has another Main).
IMAGES = ('firmware', 'firmware-test', 'O0', 'O2')
PRODUCTION_IMAGES = ('firmware', 'O0', 'O2')
# The BSS zero-initialization probe of the production images, per ISA, and
# the one the guest program's bss.zero point reads (Tests/Guest/GuestTests.hpp).
PROBE = {'Ia64': '_ZN7Asteria4User8Platform4Ia647Runtime28g_bssZeroInitializationProbeE',
         'Amd64': '_ZN7Asteria4User8Platform5Amd647Runtime28g_bssZeroInitializationProbeE'}
GUEST_PROBE = '_ZN7Asteria5Guest10g_bssProbeE'
RUNTIME_FRAGMENT = 'RunIa64MinimalUserImage'
OBSERVE = '_ZN7Asteria4User8Platform4Ia647Runtime28ObserveBssZeroInitializationEv'
QUERY = '_ZNK7Asteria4User6System16SystemCallClient15QueryCapabilityENS1_16CapabilityHandleE'
SEND = '_ZNK7Asteria4User6System16SystemCallClient4SendENS1_16CapabilityHandleEyy'
RECEIVE = '_ZNK7Asteria4User6System16SystemCallClient7ReceiveENS1_16CapabilityHandleEyy'
RUNTIME_CALLS = ((OBSERVE, 'ObserveBssZeroInitialization'), (QUERY, 'SystemCallClient::QueryCapability'),
                 (SEND, 'SystemCallClient::Send'), (RECEIVE, 'SystemCallClient::Receive'))
USER_TRANSPORT_HEADER = 'Source/User/Platform/Ia64/System/Ia64SystemCallTransport.hpp'
LAUNCH_HEADER = 'Source/Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp'
LAUNCH_PSR = 'Ia64UserModeInitialProcessorStatus'
BOOT_PSR = 'Ia64UserModePreemptibleProcessorStatus'
PSR_DFL, PSR_DFH = 1 << 18, 1 << 19     # Disabled floating-point register sets f2-f31 and f32-f127
KERNEL_ENTRY_HEADER = 'Source/Platform/Ia64/System/Ia64SystemCallEntry.hpp'
BREAK_IMMEDIATE = 'Ia64SystemCallBreakImmediate'
# Sections that would need constructors, unwinding, EH or dynamic linking:
# Forbidden section-name substrings supplement the shared list.
FORBIDDEN_FRAGMENTS = ('.init_array', '.fini_array', '.ctors', '.dtors', '.IA_64.unwind', '.eh_frame',
                       '.gcc_except_table', '.dynamic', '.dynsym', '.dynstr', '.interp')


class _ImageProperty(Property):
    variants = IMAGES

    def subjects(self, context):
        return [variant for variant in self.variants
                if context.has_variant(variant) and context.has_artifact('%s:user_elf' % variant)]


class _Ia64ImageProperty(_ImageProperty):
    isas = ('Ia64',)


def probe_name(isa: str, variant: str) -> str:
    """The BSS probe of the image a variant links."""
    return GUEST_PROBE if variant == 'firmware-test' else PROBE[isa]


def image(context, variant: str) -> ElfFile:
    return load(context.artifact('%s:user_elf' % variant))


def decode(context, variant: str, symbol: str) -> Code:
    return Subjects.code(context, context.artifact('%s:user_elf' % variant), symbol)


@register
class UserImageHeader(_ImageProperty):
    """The kernel's user-image loaders accept only an ELF64 little-endian
    executable of the ISA: EM_IA_64 with EF_IA_64_ABI64 (e_flags 0x10) on
    IA-64, EM_X86_64 with e_flags 0 on AMD64. Anything else is rejected at
    boot."""

    def extract(self, context, variant):
        return context.isa, Properties.header_of(image(context, variant))

    def verify(self, facts):
        return Properties.header_problems(facts[1], facts[0], ET_EXEC)

    def mutants(self, facts):
        isa, header = facts
        for description, changes in (('relocatable type', dict(type=ET_REL)),
                                     ('other machine', dict(machine=header.machine ^ 0x7C)),
                                     ('other ABI flags', dict(flags=header.flags ^ 0x10)),
                                     ('big-endian', dict(data=2)), ('ELF32', dict(elf_class=1))):
            yield description, (isa, dataclasses.replace(header, **changes))


# Entry --------------------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class EntryFacts:
    isa: str
    entry: int
    symbols: dict[str, tuple[Symbol,...]]      # Definitions of each named symbol
    descriptor_section: Section | None
    descriptor_bytes: bytes
    segments: tuple[Segment,...]

    def with_symbol(self, name: str, symbols: tuple[Symbol,...], **changes) -> 'EntryFacts':
        return dataclasses.replace(self, symbols={**self.symbols, name: symbols}, **changes)


@register
class UserImageEntry(_ImageProperty):
    """The loader enters the image through e_entry. On IA-64, e_entry is the
    P64 function descriptor `AsteriaIa64MinimalUserImageEntryDescriptor`,
    the 16-byte start of `.AsteriaUserEntryDescriptor`, whose words are the
    IP of `AsteriaIa64MinimalUserImageEntry` and the image's `__gp` (a wrong
    word starts user code at the wrong IP or with a wrong GP). On AMD64,
    e_entry is the function `AsteriaAmd64MinimalUserImageEntry`. Each named
    symbol is defined once, and the entry code lies in an executable PT_LOAD
    segment."""

    def extract(self, context, variant):
        elf = image(context, variant)
        names = (DESCRIPTOR, ENTRY['Ia64'], '__gp') if context.isa == 'Ia64' else (ENTRY['Amd64'],)
        section = elf.section(DESCRIPTOR_SECTION) if context.isa == 'Ia64' else None
        return EntryFacts(context.isa, elf.entry, {name: elf.definitions(name) for name in names}, section,
                          elf.section_bytes(section) if section is not None else b'', tuple(elf.segments))

    def verify(self, facts):
        problems = ['%s has %d definitions, expected exactly one' % (name, len(symbols))
                    for name, symbols in facts.symbols.items() if len(symbols) != 1]
        if problems:
            return problems
        if facts.isa == 'Ia64':
            descriptor, code, global_pointer = (facts.symbols[name][0] for name in (DESCRIPTOR, ENTRY['Ia64'], '__gp'))
            if facts.entry != descriptor.value:
                problems.append('e_entry %#x is not the entry descriptor %s at %#x'
                                % (facts.entry, DESCRIPTOR, descriptor.value))
            section = facts.descriptor_section
            if section is None:
                return problems + ['no %s section' % DESCRIPTOR_SECTION]
            if descriptor.value != section.address or descriptor.size != 16:
                problems.append('%s (%#x, %d bytes) is not the 16-byte start of %s at %#x'
                                % (DESCRIPTOR, descriptor.value, descriptor.size, DESCRIPTOR_SECTION, section.address))
            if facts.descriptor_bytes != struct.pack('<QQ', code.value, global_pointer.value):
                problems.append('%s holds %s, expected {IP %s at %#x, GP __gp at %#x}'
                                % (DESCRIPTOR_SECTION, facts.descriptor_bytes.hex(), ENTRY['Ia64'], code.value,
                                   global_pointer.value))
        else:
            code = facts.symbols[ENTRY['Amd64']][0]
            if facts.entry != code.value:
                problems.append('e_entry %#x is not %s at %#x' % (facts.entry, ENTRY['Amd64'], code.value))
            if code.type != STT_FUNC:
                problems.append('%s is not a function' % ENTRY['Amd64'])
        if not any(segment.type == PT_LOAD and segment.flags & PF_X
                   and segment.virtual_address <= code.value < segment.virtual_address + segment.memory_size
                   for segment in facts.segments):
            problems.append('the entry code at %#x lies outside every executable PT_LOAD segment' % code.value)
        return problems

    def mutants(self, facts):
        if facts.isa == 'Ia64':
            words, descriptor = facts.descriptor_bytes, facts.symbols[DESCRIPTOR][0]
            for description, changed in (
                    ('swapped descriptor words', words[8:16] + words[:8]), ('24-byte descriptor', words + bytes(8)),
                    ('descriptor GP off by one bundle',
                     words[:8] + struct.pack('<Q', struct.unpack('<Q', words[8:])[0] + 16))):
                yield description, dataclasses.replace(facts, descriptor_bytes=changed)
            yield 'e_entry at the entry code', dataclasses.replace(facts, entry=facts.symbols[ENTRY['Ia64']][0].value)
            yield 'descriptor symbol missing', facts.with_symbol(DESCRIPTOR, ())
            yield 'descriptor symbol inside its section', facts.with_symbol(
                DESCRIPTOR, (dataclasses.replace(descriptor, value=descriptor.value + 8),), entry=descriptor.value + 8)
        else:
            code = facts.symbols[ENTRY['Amd64']][0]
            yield 'e_entry past the entry', dataclasses.replace(facts, entry=code.value + 1)
            yield 'entry symbol missing', facts.with_symbol(ENTRY['Amd64'], ())
            yield 'entry symbol is data', facts.with_symbol(ENTRY['Amd64'],
                                                            (dataclasses.replace(code, type=STT_OBJECT),))
        yield 'entry code outside the executable segment', dataclasses.replace(facts, segments=tuple(
            dataclasses.replace(segment, flags=segment.flags & ~PF_X) for segment in facts.segments))


# Sections -----------------------------------------------------------------------------------------

@register
class UserImageSections(_ImageProperty):
    """The user runtime needs no constructors, unwind or EH data, TLS, or
    dynamic linking: no section of the shared forbidden list, and none whose
    name contains.init_array,.fini_array,.ctors,.dtors,.IA_64.unwind,
.eh_frame,.gcc_except_table,.dynamic,.dynsym,.dynstr or.interp.
    `.text` is PROGBITS with flags exactly ALLOC|EXECINSTR. On IA-64 both
    descriptor tables, `.AsteriaUserEntryDescriptor` and `.opd`, exist as
    PROGBITS, allocated and neither writable nor executable, so user code
    cannot overwrite a function descriptor."""

    def extract(self, context, variant):
        return context.isa, tuple(image(context, variant).sections)

    def verify(self, facts):
        isa, sections = facts
        names = [section.name for section in sections]
        problems = Properties.forbidden_section_problems(names, isa, linked=True)
        problems += ['forbidden section %s' % name for name in names
                     if any(fragment in name for fragment in FORBIDDEN_FRAGMENTS)
                     and name not in Properties.FORBIDDEN_SECTIONS]
        required = [('.text', SHF_ALLOC | SHF_EXECINSTR, 'PROGBITS section with flags exactly ALLOC|EXECINSTR')]
        if isa == 'Ia64':
            required += [(name, SHF_ALLOC, 'allocated, read-only, non-executable PROGBITS section')
                         for name in (DESCRIPTOR_SECTION, '.opd')]
        for name, flags, description in required:
            found = [section for section in sections if section.name == name]
            if len(found) != 1 or found[0].type != SHT_PROGBITS \
                    or found[0].flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR) != flags:
                problems.append('%s is not one %s' % (name, description))
        return problems

    def mutants(self, facts):
        isa, sections = facts
        last = sections[-1]

        def added(name):
            return isa, sections + (dataclasses.replace(last, index=last.index + 1, name=name),)

        def changed(name, **changes):
            return isa, tuple(dataclasses.replace(section, **changes) if section.name == name else section
                              for section in sections)
        for name in ('.init_array', '.eh_frame', '.dynamic'):
            yield 'adds %s' % name, added(name)
        yield 'adds a prioritized .ctors.65535', added('.ctors.65535')
        yield 'writable .text', changed('.text', flags=SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR)
        if isa == 'Ia64':
            yield 'adds .IA_64.unwind', added('.IA_64.unwind')
            yield 'drops .opd', (isa, tuple(section for section in sections if section.name != '.opd'))
            yield 'writable .opd', changed('.opd', flags=SHF_ALLOC | SHF_WRITE)
            yield 'executable entry descriptor', changed(DESCRIPTOR_SECTION, flags=SHF_ALLOC | SHF_EXECINSTR)


# Load and BSS geometry ------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class GeometryFacts:
    loads: tuple[Segment,...]
    bss: tuple[Section,...]
    probe: tuple[Symbol,...]
    probe_name: str


def _inside(start: int, end: int, low: int, high: int) -> bool:
    return low <= start and end <= high


@register
class UserImageLoadGeometry(_ImageProperty):
    """The user loaders map exactly an R+X text, an R read-only-data and an
    R+W data segment (W^X), zero-fill each segment's p_memsz beyond p_filesz,
    and reject overlapping or wrapping segments: exactly three PT_LOAD
    segments, one each R+X, R and R+W; each non-empty with p_filesz <=
    p_memsz and no extent past 2^64; pairwise disjoint; the R+W segment has a
    zero-filled tail. Exactly one `.bss`, a non-empty NOBITS WRITE|ALLOC
    section, lies inside that tail, and the image's BSS zero-initialization
    probe (GLOBAL DEFAULT OBJECT, unique, non-empty) lies inside `.bss`, so
    the program sees loader-zeroed memory, never file bytes: in the
    production images the probe `ObserveBssZeroInitialization` reads, in the
    guest program `Asteria::Guest::g_bssProbe`, which its bss.zero point
    reads. Mutations apply eight invalid layouts to the real image."""

    def extract(self, context, variant):
        elf = image(context, variant)
        probe = probe_name(context.isa, variant)
        return GeometryFacts(tuple(segment for segment in elf.segments if segment.type == PT_LOAD),
                             tuple(section for section in elf.sections if section.name == '.bss'),
                             elf.definitions(probe), probe)

    def verify(self, facts):
        problems = []
        kinds = {PF_R | PF_X: 'R+X', PF_R: 'R', PF_R | PF_W: 'R+W'}
        flags = sorted(segment.flags & (PF_R | PF_W | PF_X) for segment in facts.loads)
        if flags != sorted(kinds):
            problems.append('PT_LOAD flags are %s, expected exactly one each of R+X, R and R+W'
                            % [kinds.get(flag, '%#x' % flag) for flag in flags])
        for segment in facts.loads:
            label = 'PT_LOAD at %#x' % segment.virtual_address
            if segment.memory_size == 0 or segment.file_size > segment.memory_size:
                problems.append('%s: p_memsz %#x is zero or below p_filesz %#x'
                                % (label, segment.memory_size, segment.file_size))
            if segment.offset + segment.file_size > LIMIT or segment.virtual_address + segment.memory_size > LIMIT \
                    or segment.physical_address + segment.memory_size > LIMIT:
                problems.append('%s: extent wraps the address space' % label)
        ordered = sorted(facts.loads, key=lambda segment: segment.virtual_address)
        problems += ['PT_LOAD at %#x overlaps PT_LOAD at %#x' % (first.virtual_address, second.virtual_address)
                     for first, second in zip(ordered, ordered[1:])
                     if first.virtual_address + first.memory_size > second.virtual_address]
        writable = [segment for segment in facts.loads if segment.flags & (PF_R | PF_W | PF_X) == PF_R | PF_W]
        if len(writable) != 1:
            return problems
        data = writable[0]
        tail = (data.virtual_address + data.file_size, data.virtual_address + data.memory_size)
        if data.memory_size <= data.file_size:
            problems.append('the R+W segment has no zero-filled tail')
        if len(facts.bss) != 1:
            return problems + ['%d .bss sections, expected exactly one' % len(facts.bss)]
        bss = facts.bss[0]
        if bss.type != SHT_NOBITS or bss.flags & (SHF_ALLOC | SHF_WRITE | SHF_EXECINSTR) != SHF_ALLOC | SHF_WRITE \
                or bss.size == 0:
            problems.append('.bss is not a non-empty NOBITS section with flags exactly WRITE|ALLOC')
        if not _inside(bss.address, bss.address + bss.size, *tail):
            problems.append('.bss [%#x, %#x) is not inside the zero-filled tail [%#x, %#x) of the R+W segment'
                            % (bss.address, bss.address + bss.size, *tail))
        if len(facts.probe) != 1:
            return problems + ['%s has %d definitions, expected exactly one' % (facts.probe_name, len(facts.probe))]
        probe = facts.probe[0]
        if (probe.type, probe.binding, probe.visibility) != (STT_OBJECT, STB_GLOBAL, STV_DEFAULT) \
                or probe.section_index != bss.index or probe.size == 0:
            problems.append('the BSS probe is not a non-empty GLOBAL DEFAULT OBJECT in .bss')
        if not _inside(probe.value, probe.value + probe.size, bss.address, bss.address + bss.size) \
                or not _inside(probe.value, probe.value + probe.size, *tail):
            problems.append('the BSS probe [%#x, %#x) is not inside .bss and the zero-filled tail'
                            % (probe.value, probe.value + probe.size))
        return problems

    def mutants(self, facts):
        def kind(flags):
            return next(index for index, segment in enumerate(facts.loads)
                        if segment.flags & (PF_R | PF_W | PF_X) == flags)

        def load_changed(index, **changes):
            return dataclasses.replace(facts, loads=Mutate.replace_at(facts.loads, index, **changes))

        text, rodata, data = kind(PF_R | PF_X), kind(PF_R), kind(PF_R | PF_W)
        bss, probe = facts.bss[0], facts.probe[0]
        low, high = facts.loads[rodata], facts.loads[data]
        yield 'missing read permission on text', load_changed(text, flags=PF_X)
        yield 'read-only segment with p_memsz below p_filesz', load_changed(rodata, file_size=low.memory_size + 0x20)
        yield 'wrapping R+W extent', load_changed(data, virtual_address=LIMIT - 0x10, physical_address=LIMIT - 0x10)
        yield '.bss inside the read-only segment', dataclasses.replace(facts, bss=(dataclasses.replace(
            bss, address=low.virtual_address),), probe=(dataclasses.replace(probe, value=low.virtual_address),))
        yield 'writable and executable text', load_changed(text, flags=PF_R | PF_W | PF_X)
        yield 'read-only segment overlapping the data segment', load_changed(
            rodata, virtual_address=high.virtual_address, physical_address=high.physical_address)
        yield 'file-backed .bss', dataclasses.replace(facts, bss=(dataclasses.replace(
            bss, address=high.virtual_address + high.file_size - 0x10),))
        yield 'probe outside .bss', dataclasses.replace(facts, probe=(dataclasses.replace(
            probe, value=bss.address + bss.size),))
        yield 'R+W segment without zero-filled tail', load_changed(data, file_size=high.memory_size)
        yield 'fourth PT_LOAD segment', dataclasses.replace(facts, loads=facts.loads + (dataclasses.replace(
            high, virtual_address=LIMIT >> 1, physical_address=LIMIT >> 1),))
        yield 'local probe', dataclasses.replace(facts, probe=(dataclasses.replace(probe, binding=STB_LOCAL),))


# Link closure -------------------------------------------------------------------------------------

@register
class UserImageLinkClosure(_ImageProperty):
    """The image is mapped at its link address without a relocator or
    dynamic linker: no undefined symbol (weak ones included), no relocation
    entries in any REL/RELA section, no .dynamic section and no PT_DYNAMIC segment."""

    def extract(self, context, variant):
        elf = image(context, variant)
        return (tuple(Properties.undefined_names(elf)),
                tuple(section for section in elf.sections if section.type in (SHT_REL, SHT_RELA, SHT_DYNAMIC)
                      or section.name == '.dynamic'),
                tuple(segment for segment in elf.segments if segment.type == PT_DYNAMIC))

    def verify(self, facts):
        undefined, sections, dynamic = facts
        problems = Properties.undefined_problems(undefined, ()) + Properties.relocation_section_problems(sections)
        problems += ['dynamic-linking section %s' % section.name for section in sections
                     if section.type == SHT_DYNAMIC or section.name == '.dynamic']
        return problems + ['PT_DYNAMIC segment at %#x' % segment.virtual_address for segment in dynamic]

    def mutants(self, facts):
        undefined, sections, dynamic = facts
        yield 'undefined weak reference', (undefined + ('weak_mutant',), sections, dynamic)
        yield 'runtime relocations', (undefined, sections + (
            Section(99, '.rela.dyn', SHT_RELA, SHF_ALLOC, 0, 0, 24, 0, 0, 8, 24),), dynamic)
        yield 'dynamic section', (undefined, sections + (
            Section(98, '.dynamic', SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, 0, 0, 16, 0, 0, 8, 16),), dynamic)
        yield 'PT_DYNAMIC segment', (undefined, sections, dynamic + (Segment(PT_DYNAMIC, PF_R, 0, 0, 0, 16, 16, 8),))


# IA-64: system-call transport --------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class TransportFacts:
    transport: Code
    user_immediate: int | None
    kernel_immediate: int | None


@register
class Ia64UserImageTransportBreak(_Ia64ImageProperty):
    """Every user system call enters the kernel through exactly one
    unpredicated `break` in `AsteriaIa64UserSystemCallTransport`, whose
    immediate is Ia64SystemCallBreakImmediate as the user transport header
    states it, and that value equals the one the kernel entry validates
    (Platform/Ia64/System/Ia64SystemCallEntry.hpp). A missing, duplicated or
    mismatched break loses or double-issues each call. (The golden pins the
    whole routine.)"""

    def extract(self, context, variant):
        return TransportFacts(decode(context, variant, TRANSPORT), *(
            Layout.constants(Layout.read(context.root / header)).get(BREAK_IMMEDIATE)
            for header in (USER_TRANSPORT_HEADER, KERNEL_ENTRY_HEADER)))

    def verify(self, facts):
        if facts.user_immediate is None or facts.kernel_immediate is None:
            return ['%s is no longer stated in %s and %s' % (BREAK_IMMEDIATE, USER_TRANSPORT_HEADER,
                                                            KERNEL_ENTRY_HEADER)]
        problems = []
        if facts.user_immediate != facts.kernel_immediate:
            problems.append('the user transport breaks with %#x but the kernel entry accepts %#x'
                            % (facts.user_immediate, facts.kernel_immediate))
        breaks = facts.transport.all(lambda step: step.mnemonic.startswith('break'))
        if len(breaks) != 1:
            return problems + ['%d break instructions in %s, expected exactly one' % (len(breaks), TRANSPORT)]
        if breaks[0].instruction.predicate is not None:
            problems.append('the system-call break is predicated')
        if breaks[0].operation.immediates != (facts.user_immediate,):
            problems.append('the system-call break is "%s", expected immediate %#x'
                            % (breaks[0].rendered, facts.user_immediate))
        return problems

    def mutants(self, facts):
        transport = facts.transport
        site = transport.site(lambda step: step.mnemonic.startswith('break'))
        for description, code in (
                ('second break', transport.duplicate(site)),
                ('break removed', transport.replace(site, text='nop.i 0x0')),
                ('wrong break immediate', transport.replace(site, text='break.i %#x' % (facts.user_immediate + 1))),
                ('predicated break', transport.replace(site, predicate='p06'))):
            yield description, dataclasses.replace(facts, transport=code)
        yield 'kernel entry validates another immediate', dataclasses.replace(
            facts, kernel_immediate=facts.user_immediate + 0x10000)


# IA-64: user floating point -----------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class FloatingPointLaunchFacts:
    launch_psr: int | None                      # The synthetic probe's
    boot_psr: int | None                        # Every other user thread's


@register
class Ia64UserFloatingPointLaunch(Property):
    """User threads own the whole floating-point register file, so compiled user code may use it, integer multiplication and
    division included (GCC computes both in f6-f15): every thread but the
    synthetic probe's launches with `Ia64UserModePreemptibleProcessorStatus`,
    PSR.dfl and PSR.dfh clear. That rests on what the kernel does for them:
    it runs under its own fpsr at every PL3 entry
    (Ia64KernelFloatingPointStatus), keeps f2-f127 across interruptions and
    context switches, and hands a PL3 floating-point fault or trap to the
    thread's supervisor (Ia64IvtDirectTrampolines). The probe
    (`Ia64UserModeInitialProcessorStatus`) executes no floating point and
    keeps both halves disabled."""

    isas = ('Ia64',)
    variants = ('firmware',)

    def subjects(self, context):
        return ['firmware'] if context.has_variant('firmware') else []

    def extract(self, context, variant):
        constants = Layout.constants(Layout.read(context.root / LAUNCH_HEADER))
        return FloatingPointLaunchFacts(constants.get(LAUNCH_PSR), constants.get(BOOT_PSR))

    def verify(self, facts):
        problems = []
        for name, psr in ((LAUNCH_PSR, facts.launch_psr), (BOOT_PSR, facts.boot_psr)):
            if psr is None:
                problems.append('%s is no longer stated in %s' % (name, LAUNCH_HEADER))
        if facts.launch_psr is not None and (facts.launch_psr & (PSR_DFL | PSR_DFH)) != (PSR_DFL | PSR_DFH):
            problems.append('the probe launch PSR %#x enables floating-point registers' % facts.launch_psr)
        if facts.boot_psr is not None and facts.boot_psr & (PSR_DFL | PSR_DFH):
            problems.append('the user launch PSR %#x disables floating-point registers user code may use'
                            % facts.boot_psr)
        return problems

    def mutants(self, facts):
        yield 'user threads without f2-f31', dataclasses.replace(facts, boot_psr=(facts.boot_psr or 0) | PSR_DFL)
        yield 'user threads without f32-f127', dataclasses.replace(facts, boot_psr=(facts.boot_psr or 0) | PSR_DFH)
        yield 'the probe with f2-f31', dataclasses.replace(facts, launch_psr=(facts.launch_psr or 0) & ~PSR_DFL)
        yield 'the probe with f32-f127', dataclasses.replace(facts, launch_psr=(facts.launch_psr or 0) & ~PSR_DFH)
        yield 'no user launch PSR stated', dataclasses.replace(facts, boot_psr=None)


# IA-64: call structure ----------------------------------------------------------------------------

def runtime_symbols(elf: ElfFile) -> tuple[str,...]:
    """Defined symbols whose name contains RunIa64MinimalUserImage; a
.constprop/.isra/.part clone makes this ambiguous on purpose."""
    return tuple(sorted({symbol.name for symbol in elf.defined_symbols() if RUNTIME_FRAGMENT in symbol.name}))


@dataclasses.dataclass(frozen=True)
class MainCallFacts:
    main: Code
    runtimes: tuple[str,...]


@register
class Ia64UserImageMainCall(_Ia64ImageProperty):
    """In the production images, the P64 C++ entry
    `AsteriaIa64MinimalUserImageMain` invokes the runtime exactly once and
    unconditionally: exactly one unpredicated direct branch to
    `RunIa64MinimalUserImage`, either a `br.call b0=` or, because
    `return Run(...)` is in tail position and the user flags do not disable
    sibling calls, a direct tail branch. The runtime symbol must be unique (a
    GCC clone fails the lookup on purpose). Compiler-dependent; every current
    variant uses `br.call`. The sibling-call form is an acceptance variant
    that must pass on every run."""
    variants = PRODUCTION_IMAGES

    def extract(self, context, variant):
        return MainCallFacts(decode(context, variant, MAIN), runtime_symbols(image(context, variant)))

    def verify(self, facts):
        if len(facts.runtimes) != 1:
            return ['%d defined symbols contain %s, expected exactly one: %s'
                    % (len(facts.runtimes), RUNTIME_FRAGMENT, ', '.join(facts.runtimes))]
        return ['%s: %s' % (MAIN, problem) for problem in Properties.call_problems(
            facts.main.instructions, facts.runtimes[0], RUNTIME_FRAGMENT, allow_tail_branch=True)]

    def mutants(self, facts):
        main, runtime = facts.main, facts.runtimes[0]
        site = Properties.branch_references(main.instructions, runtime)[0]
        call = main.instructions[site]

        def at_site(**changes):
            return dataclasses.replace(facts, main=main.replace(site, **changes))
        tail = call.text.replace('br.call.sptk.many b0=', 'br.sptk.many ')
        if Properties.call_form(main.instructions, runtime) == 'call':
            rejected = self.verify(at_site(text=tail))
            if rejected:
                raise AcceptanceError('the sibling-call form of Main was rejected: %s' % '; '.join(rejected))
        yield 'missing call (only a movl reference)', at_site(text='movl r14=<%s>' % runtime)
        yield 'duplicate call', dataclasses.replace(facts, main=main.duplicate(site))
        yield 'predicated call', at_site(predicate='p06')
        yield 'call through b6', at_site(text=call.text.replace('b0=', 'b6='))
        yield 'predicated tail branch', at_site(text=tail, predicate='p06')
        yield 'runtime cloned by GCC', dataclasses.replace(facts, runtimes=facts.runtimes + (runtime + '.constprop.0',))


@dataclasses.dataclass(frozen=True)
class RuntimeCallFacts:
    run: Code
    runtimes: tuple[str,...]
    definitions: tuple[tuple[str, int],...]


@register
class Ia64UserImageRuntimeCalls(_Ia64ImageProperty):
    """In the production images, `RunIa64MinimalUserImage` zero-checks the
    BSS, then queries, sends and receives exactly once and unconditionally,
    in protocol order: exactly one unpredicated direct `br.call b0=` each to
    ObserveBssZeroInitialization, SystemCallClient::QueryCapability,::Send
    and::Receive (each defined once, the runtime symbol unique), and each
    call dominates the next in the control-flow graph: no path reaches Query
    without the BSS check, Send without Query, or Receive without Send.
    Other calls (O0 helpers) are ignored. Dominance, not address order, so
    block layout does not matter. Mutations apply eleven invalid call
    structures to the real function."""
    variants = PRODUCTION_IMAGES

    def extract(self, context, variant):
        elf = image(context, variant)
        runtimes = runtime_symbols(elf)
        return RuntimeCallFacts(decode(context, variant, runtimes[0]) if runtimes else Code(0, ()), runtimes,
                                tuple((callee, len(elf.definitions(callee))) for callee, _ in RUNTIME_CALLS))

    def verify(self, facts):
        problems = [] if len(facts.runtimes) == 1 else [
            '%d defined symbols contain %s, expected exactly one' % (len(facts.runtimes), RUNTIME_FRAGMENT)]
        problems += ['%s has %d definitions, expected exactly one' % (label, count)
                     for (_, label), (_, count) in zip(RUNTIME_CALLS, facts.definitions) if count != 1]
        instructions = facts.run.instructions
        problems += ['%s: %s' % (RUNTIME_FRAGMENT, problem) for callee, label in RUNTIME_CALLS
                     for problem in Properties.call_problems(instructions, callee, label)]
        if problems:
            return problems
        graph = facts.run.graph
        if graph.indirect:
            return ['%s: indirect branch at #%d; the call order cannot be established'
                    % (RUNTIME_FRAGMENT, graph.indirect[0])]
        sites = [Properties.branch_references(instructions, callee)[0] for callee, _ in RUNTIME_CALLS]
        return ['%s: a path reaches the call to %s without the call to %s' % (RUNTIME_FRAGMENT, second_label,
                                                                             first_label)
                for (first, (_, first_label)), (second, (_, second_label))
                in zip(zip(sites, RUNTIME_CALLS), zip(sites[1:], RUNTIME_CALLS[1:]))
                if not graph.dominates(first, second)]

    def mutants(self, facts):
        run = facts.run
        sites = {callee: Properties.branch_references(run.instructions, callee)[0] for callee, _ in RUNTIME_CALLS}

        def changed(code):
            return dataclasses.replace(facts, run=code)
        query, send = run.instructions[sites[QUERY]], run.instructions[sites[SEND]]
        yield 'reordered query and send', changed(run.replace(sites[QUERY], text=send.text).replace(
            sites[SEND], text=query.text))
        for callee, label in RUNTIME_CALLS[1:]:
            yield 'missing %s' % label, changed(run.replace(sites[callee], text='nop.b 0x0'))
        for callee, label in (RUNTIME_CALLS[0], RUNTIME_CALLS[2], RUNTIME_CALLS[3]):
            yield 'duplicate %s' % label, changed(run.duplicate(sites[callee]))
        for callee, label in RUNTIME_CALLS:
            yield 'predicated %s' % label, changed(run.replace(sites[callee], predicate='p06'))


# IA-64: bootstrap forwarding ------------------------------------------------------------------------

# The output register that carries each launch argument into Main: the
# bootstrap Endpoint (r4/r5) and the DiagnosticChannel (r6/r7).
FORWARDED_ARGUMENTS = (('r32', 'r4'), ('r33', 'r5'), ('r34', 'r6'), ('r35', 'r7'))


@register
class Ia64UserImageBootstrapForwarding(_Ia64ImageProperty):
    """The hand-written entry keeps the loader-provided GP and forwards the
    two launch capabilities (static r4/r5 and r6/r7, set by the launch) as
    out0-out3 of a four-output frame into `AsteriaIa64MinimalUserImageMain`:
    exactly one call in the entry, an unpredicated `br.call b0=` to Main and
    no other reference to Main; before it, nothing writes r1 (no reload, no
    post-increment through it), one `alloc` of exactly four output registers,
    and r32-r35 last written by unpredicated `mov r32=r4`... `mov r35=r7` in
    instruction groups after the allocation's and before the call's, with
    nothing else. The golden pins the exact entry; the self-tests are the
    nine invalid entry layouts and r6/r7 forwarding defects."""

    def extract(self, context, variant):
        return decode(context, variant, ENTRY['Ia64'])

    def verify(self, entry):
        calls = entry.all(lambda step: step.operation.is_call)
        if len(calls) != 1:
            return ['%d calls in %s, expected exactly one' % (len(calls), ENTRY['Ia64'])]
        call = calls[0]
        problems = Properties.call_problems(entry.instructions, MAIN, MAIN)
        if [step.position for step in entry.steps if step.operation.target is not None
                and step.operation.target.symbol == MAIN] != [call.position]:
            problems.append('%s is referenced other than by the one call' % MAIN)
        allocs, last_write = [], {}
        for step in entry.window(before=call):
            if 'r1' in step.operation.writes:
                problems.append('"%s" (#%d) changes the loader-provided GP before the call'
                                % (step.rendered, step.position))
            if step.operation.base == 'alloc':
                allocs.append(step)
                continue
            written = {register for register in step.operation.writes if register.startswith('r')}
            if not written or not written <= {output for output, _ in FORWARDED_ARGUMENTS}:
                problems.append('"%s" (#%d) is neither the frame allocation nor an output move'
                                % (step.rendered, step.position))
            last_write.update((register, step) for register in written)
        if len(allocs) != 1:
            return problems + ['%d frame allocations before the call, expected one' % len(allocs)]
        alloc = allocs[0]
        if alloc.operation.immediates != (len(FORWARDED_ARGUMENTS), 0, 0):
            problems.append('the entry frame "%s" is not four output registers and nothing else' % alloc.rendered)
        for register, source in FORWARDED_ARGUMENTS:
            step = last_write.get(register)
            if step is None or step.rendered != 'mov %s=%s' % (register, source):
                problems.append('%s (out%d) is not last written by an unpredicated mov %s=%s'
                                % (register, int(register[1:]) - 32, register, source))
            elif not alloc.group < step.group < call.group:
                problems.append('mov %s=%s is not in an instruction group after the allocation and before the call'
                                % (register, source))
        return problems

    def mutants(self, code):
        call = code.site(lambda step: step.operation.is_call)
        alloc = code.site(lambda step: step.operation.base == 'alloc')
        first, third, last = (code.site('mov %s=%s' % FORWARDED_ARGUMENTS[index]) for index in (0, 2, 3))
        yield 'reordered call', code.delete(call).insert(alloc, call.rendered, after=True,
                                                         stop=call.instruction.stop_after)
        yield 'deleted call', code.replace(call, text='nop.b 0x0')
        yield 'predicated move', code.replace(first, predicate='p6')
        yield 'clobbered output', code.insert(first, 'add r32=1,r32', after=True)
        yield 'duplicate call', code.duplicate(call)
        yield 'missing allocation stop', code.without_boundary(alloc)
        yield 'missing argument stop', code.without_boundary(last)
        yield 'reloaded global pointer', code.insert(alloc, 'movl r1=0x1234', stop=True)
        yield 'post-incremented global pointer', code.insert(alloc, 'ld8 r14=[r1],8', stop=True)
        yield 'three-output frame', code.replace(alloc, text=alloc.text.replace(',4,0,0', ',3,0,0'))
        yield 'five-output frame', code.replace(alloc, text=alloc.text.replace(',4,0,0', ',5,0,0'))
        yield 'call through b6', code.replace(call, text=call.text.replace('b0=', 'b6='))
        yield 'r6 not forwarded', code.replace(third, text='mov r34=r0')
        yield 'swapped r6/r7 outputs', code.replace(third, text='mov r34=r7').replace(last, text='mov r35=r6')
        yield 'r7 forwarded outside the frame', code.replace(last, text='mov r36=r7')
