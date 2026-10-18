# SPDX-License-Identifier: Apache-2.0
"""The IA-64 firmware's pre-global placement entry and cache publication."""
from __future__ import annotations

import dataclasses
import struct

from AsteriaVerify import Elf, Pe
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Elf import STT_FUNC
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.TargetChecks.Ia64PhysicalAccess import FunctionFacts, function_facts

ROOT = 'EfiMain'
BOOT_STORAGE = frozenset({
    '_ZN7Asteria4Boot4Uefi11g_bootFilesE',
    # Boot-local display policy is parsed with the receipt before placement;
    # the image copy retains it alongside the existing manifest scratch.
    '_ZN7Asteria4Boot4Uefi15g_displayPolicyE',
    # Pointer-free loader failure text survives placement without output.
    '_ZN7Asteria4Boot4Uefi17g_bootFileFailureE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L18g_candidateDisplayE',
    '_ZN7Asteria4Boot4Uefi16g_bootKernelFileE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L17g_kernelCandidateE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L10g_manifestE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L11g_candidateE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L6g_infoE',
    # The BOOT.CFG trust allowlist and each trusted module's SHA-256, parsed and hashed with the
    # receipt; pointer-free, retained by the copy.
    '_ZN7Asteria4Boot4Uefi11g_bootTrustE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L16g_candidateTrustE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L15g_moduleDigestsE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L14g_moduleHashedE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L14g_consoleChunkE',
    # BOOT.CFG's `lowmemory` watermark, parsed with
    # the receipt; an integer, retained by the copy.
    '_ZN7Asteria4Boot4Uefi21g_bootLowMemoryBlocksE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L20g_candidateLowMemoryE',
    # Watchdog.md: scalar boot configuration retained with the receipt.
    '_ZN7Asteria4Boot4Uefi21g_bootWatchdogSecondsE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L19g_candidateWatchdogE',
    # Scalar session selection retained by the image copy.
    '_ZN7Asteria4Boot4Uefi17g_bootSessionModeE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L18g_candidateSessionE',
    # Scalar login selection, retained by the image copy.
    '_ZN7Asteria4Boot4Uefi18g_bootLoginEnabledE',
    '_ZN7Asteria4Boot4Uefi12_GLOBAL__N_1L16g_candidateLoginE',
    'AsteriaIa64DiagnosticWriter',
})
BOOT_CALLBACKS = ('ReadAndRetainBootFiles', 'LoadBootFiles', 'SizeOf', 'Whole',
                  'ReadManifest', 'OpenImage', 'SizeImages', 'CloseImage', 'ReadImages', 'ReadKernel')
SYMBOL_OPERANDS = frozenset({'AsteriaImageBase', 'AsteriaIa64LoadedImageEnd',
                            'AsteriaIa64RelocatedEntryDescriptor'})


@dataclasses.dataclass(frozen=True)
class EntryFacts:
    functions: dict[str, FunctionFacts]
    publication: tuple[str,...]
    boot_functions: frozenset[str]
    read_only_targets: frozenset[str]


@register
class Ia64ImagePlacementBeforeGlobals(Property):
    """Before entry into the copy, every reachable direct callee uses only
    PC-relative code references or the three relocated IMM64 symbol operands.
    Before placement, boot file contract's bounded boot-file subtree may use its
    explicit scratch/receipt, console scratch, diagnostic writer and loaded
    read-only constants. Other image globals remain forbidden. Only the entry
    and named file/console adapters call firmware indirectly. File loading
    precedes copy/cache publication and the copied descriptor call.
    """
    isas = ('Ia64',)
    variants = ('firmware', 'firmware-test')

    def extract(self, context, variant):
        path = context.artifact('%s:loader_elf' % variant)
        elf = Elf.load(path)
        functions, pending = {}, [ROOT]
        while pending:
            name = pending.pop()
            if name in functions or not any(s.type == STT_FUNC and s.size for s in elf.definitions(name)):
                continue
            facts = function_facts(context, path, name)
            functions[name] = facts
            pending.extend(target for kind, target, _ in facts.calls if kind == 'direct')
        boot_functions, pending = set(), [name for name in functions if 'ReadAndRetainBootFiles' in name]
        while pending:
            name = pending.pop()
            if name in boot_functions or name not in functions:
                continue
            boot_functions.add(name)
            pending.extend(target for kind, target, _ in functions[name].calls if kind == 'direct')
        read_only_targets = set()
        for name in boot_functions:
            for relocation in functions[name].relocations:
                target = relocation.target
                if target.startswith('.rodata+') or target == '.rodata':
                    read_only_targets.add(target)
                elif any(getattr(elf.section_of(symbol), 'name', '') == '.rodata'
                         for symbol in elf.definitions(target)):
                    read_only_targets.add(target)
        calls = functions[ROOT].calls
        publication = tuple('files' if target and 'ReadAndRetainBootFiles' in target
                            else 'copy' if target and 'RelocateToAlignment' in target
                            else 'coherent' if target and 'ImageInstructionCoherent' in target
                            else 'indirect' for kind, target, _ in calls
                            if kind == 'indirect' or target and ('RelocateToAlignment' in target
                                                               or 'ImageInstructionCoherent' in target
                                                               or 'ReadAndRetainBootFiles' in target))
        return EntryFacts(functions, publication, frozenset(boot_functions), frozenset(read_only_targets))

    def verify(self, facts):
        problems = []
        for name, function in facts.functions.items():
            for relocation in function.relocations:
                if relocation.type == 'PCREL21B' and relocation.target in facts.functions:
                    continue
                if (relocation.type == 'IMM64' and relocation.target in SYMBOL_OPERANDS
                        and relocation.addend == 0):
                    continue
                if (name in facts.boot_functions and relocation.type in ('GPREL64I', 'GPREL22', 'IMM64')
                        and relocation.addend == 0
                        and relocation.target in BOOT_STORAGE | facts.read_only_targets):
                    continue
                problems.append('%s uses pre-copy global/foreign relocation %s' % (name, relocation))
            for kind, target, offset in function.calls:
                if kind == 'indirect' and (name == ROOT or name in facts.boot_functions
                                           and any(adapter in name for adapter in BOOT_CALLBACKS)):
                    continue
                if kind != 'direct' or offset or target not in facts.functions:
                    problems.append('%s makes an unbounded pre-copy call %s' % (name, (kind, target, offset)))
        sequence = facts.publication
        if (sequence.count('files') != 1 or sequence.count('copy') != 1 or sequence.count('coherent') != 1
                or sequence.index('files') > sequence.index('copy')
                or sequence.index('copy') > sequence.index('coherent')
                or 'indirect' not in sequence[sequence.index('coherent') + 1:]):
            problems.append('entry publication sequence %s does not end copy/coherent/descriptor' % (sequence,))
        return problems

    def mutants(self, facts):
        for target in ('.data', '.rodata'):
            function = facts.functions[ROOT]
            yield 'entry references global ' + target, dataclasses.replace(facts, functions={
                **facts.functions, ROOT: dataclasses.replace(function, relocations=function.relocations +
                                                            (RelocationFact(0, 'GPREL22', target, 0),))})
        callee = next(name for name in facts.functions if 'RelocateToAlignment' in name)
        function = facts.functions[callee]
        yield 'copy helper references a global', dataclasses.replace(facts, functions={
            **facts.functions, callee: dataclasses.replace(function, relocations=function.relocations +
                                                          (RelocationFact(0, 'IMM64', '.bss', 0),))})
        yield 'missing copy helper', dataclasses.replace(facts, functions={
            name: value for name, value in facts.functions.items() if name != callee})
        loader = next(name for name in facts.boot_functions if 'LoadBootFiles' in name)
        function = facts.functions[loader]
        yield 'boot loader touches unrelated writable data', dataclasses.replace(facts, functions={
            **facts.functions, loader: dataclasses.replace(function, relocations=function.relocations +
                                                           (RelocationFact(0, 'GPREL64I', '.data', 0),))})
        yield 'files after copy', dataclasses.replace(facts, publication=('copy', 'files', 'coherent', 'indirect'))
        yield 'missing cache publication', dataclasses.replace(facts, publication=('copy', 'indirect'))
        yield 'entry before cache publication', dataclasses.replace(facts, publication=('copy', 'indirect', 'coherent'))


@dataclasses.dataclass(frozen=True)
class PlacementFacts:
    geometry: tuple[int,...]
    entry_rva: int
    continuation_rva: int
    words: tuple[int, int]
    expected_words: tuple[int, int]
    fixups: tuple[tuple[int, int],...]


@register
class Ia64ImagePlacementContract(Property):
    """The copy has three 4 MiB permission groups at fixed aligned RVAs.
    The second.opd descriptor names the continuation and its GP, and both
    words have exactly one DIR64 fixup in the actual normalized PE stream.
    """
    isas = ('Ia64',)
    variants = ('firmware', 'firmware-test')

    def extract(self, context, variant):
        elf = Elf.load(context.artifact('%s:loader_elf' % variant))
        pe = Pe.PeFile(context.artifact('%s:efi' % variant))
        continuation = elf.symbol('AsteriaIa64RelocatedEntryDescriptor').value - pe.header.image_base
        text, rodata, data, bss = (pe.section(name) for name in ('.text', '.rodata', '.data', '.bss'))
        fixups = tuple((block.page_rva + (entry & 0xFFF), entry >> 12)
                       for block in Pe.relocation_blocks(pe.base_relocation_data()) for entry in block.entries
                       if continuation <= block.page_rva + (entry & 0xFFF) < continuation + 16)
        return PlacementFacts((text.rva, text.virtual_size, rodata.rva, data.rva,
                               bss.rva + bss.virtual_size, pe.header.image_size),
                              pe.header.entry_rva, continuation, struct.unpack('<QQ', pe.read_rva(continuation, 16)),
                              (elf.symbol('EfiMainAligned').value, elf.symbol('__gp').value), fixups)

    def verify(self, facts):
        problems = []
        if facts.geometry != (0x400000, 0x400000, 0x800000, 0xC00000, 0x1000000, 0x1000000):
            problems.append('permission group RVAs/extents are %s' % (facts.geometry,))
        if facts.continuation_rva != facts.entry_rva + 16 or facts.continuation_rva & 15:
            problems.append('continuation is not the second aligned .opd entry')
        if facts.words != facts.expected_words:
            problems.append('continuation descriptor does not name EfiMainAligned and __gp')
        if facts.fixups != ((facts.continuation_rva, 10), (facts.continuation_rva + 8, 10)):
            problems.append('continuation words lack exactly one DIR64 fixup each')
        return problems

    def mutants(self, facts):
        for index in range(len(facts.geometry)):
            geometry = list(facts.geometry)
            geometry[index] += 0x10000
            yield 'group geometry %d loses alignment/extent' % index, dataclasses.replace(facts, geometry=tuple(geometry))
        yield 'continuation selects original entry', dataclasses.replace(facts, continuation_rva=facts.entry_rva)
        yield 'continuation code is wrong', dataclasses.replace(facts, words=(facts.words[0] + 16, facts.words[1]))
        yield 'continuation GP is wrong', dataclasses.replace(facts, words=(facts.words[0], facts.words[1] + 8))
        yield 'missing descriptor fixup', dataclasses.replace(facts, fixups=facts.fixups[:1])
        yield 'duplicate descriptor fixup', dataclasses.replace(facts, fixups=facts.fixups + facts.fixups[:1])
        yield 'wrong descriptor fixup type', dataclasses.replace(facts, fixups=((facts.continuation_rva, 9),
                                                                             facts.fixups[1]))
