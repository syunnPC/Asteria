# SPDX-License-Identifier: Apache-2.0
"""Object-level loader/kernel symbol boundary for kernel image contract."""
from __future__ import annotations

import dataclasses

from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Elf import SHN_UNDEF, STB_LOCAL, load


def loader_unit(source: str) -> bool:
    return (source.startswith(('Source/Boot/Uefi/', 'Source/Acpi/Aml/'))
            or source in {
                'Source/Boot/Edid.cpp',
                'Source/Boot/BootManifest.cpp',
                'Source/Boot/KernelElf.cpp',
                'Source/Boot/Acpi/AcpiTableParser.cpp',
                'Source/Boot/Acpi/AcpiDefinitionBlocks.cpp',
                'Source/Inventory/ReportText.cpp',
                'Source/Inventory/XhciReport.cpp',
                'Source/Inventory/EhciRmrrReport.cpp',
                'Source/Boot/Entropy/BootEntropyCollector.cpp',
                'Source/Platform/Firmware/AhciPciResourceCapture.cpp',
                'Source/Platform/Amd64/Firmware/ProcessorInventory.cpp',
                'Source/Platform/Amd64/Firmware/ProcessorInventory.S',
                'Source/Platform/Amd64/Firmware/BootEntropySources.cpp',
                'Source/Platform/Amd64/Firmware/FirmwarePortIo.cpp',
                'Source/Platform/Ia64/Firmware/BootEntropySources.cpp',
                'Source/Platform/Ia64/Firmware/SalSystemTableCapture.cpp',
                'Source/Platform/Ia64/Firmware/SalSystemTableLocator.cpp',
                'Source/Platform/Ia64/Firmware/Ia64ImageRelocation.cpp',
                'Source/Platform/Ia64/Firmware/Ia64ImageRelocation.S',
            })


@dataclasses.dataclass(frozen=True)
class BoundaryFacts:
    loader_symbols: frozenset[str]
    kernel_references: tuple[tuple[str, str],...]
    entries: frozenset[str]


@register
class KernelLoaderSymbolBoundary(Property):
    """Every kernel link unit is free of loader and UEFI application imports.

    Pure memory/diagnostic adapters may be composed into both links; their
    storage is private to each link. SAL/PAL are processor firmware and
    remain kernel imports. Both entries must be present, so an empty kernel
    cannot satisfy the check.
    """
    variants = ('firmware', 'O0', 'O2')

    def extract(self, context, variant):
        objects = context.objects(variant)
        loader = set()
        references = []
        entries = set()
        for source, path in objects.items():
            symbols = load(path).symbols
            if loader_unit(source):
                loader.update(symbol.name for symbol in symbols
                              if symbol.section_index != SHN_UNDEF and symbol.binding != STB_LOCAL)
            else:
                references.extend((source, symbol.name) for symbol in symbols
                                  if symbol.section_index == SHN_UNDEF)
                if source == 'Source/Kernel/Boot/KernelEntry.cpp':
                    entries.update(symbol.name for symbol in symbols if symbol.section_index != SHN_UNDEF
                                   and ('13PrepareKernel' in symbol.name or '11EnterKernel' in symbol.name))
        return BoundaryFacts(frozenset(loader), tuple(sorted(references)), frozenset(entries))

    def verify(self, facts):
        problems = []
        if len(facts.entries) != 2:
            problems.append('kernel must define its preparation and entry functions')
        for source, symbol in facts.kernel_references:
            if symbol in facts.loader_symbols or '_ZN7Asteria4Boot4Uefi' in symbol:
                problems.append('%s imports loader symbol %s' % (source, symbol))
        return problems

    def mutants(self, facts):
        for name in ('_ZN7Asteria4Boot4UefiFakeBootServicesEv', 'LoaderPrivateStorage'):
            yield 'kernel imports ' + name, dataclasses.replace(
                facts, loader_symbols=facts.loader_symbols | {name},
                kernel_references=facts.kernel_references + (('Source/Kernel/Boot/KernelEntry.cpp', name),))
        yield 'missing kernel entries', dataclasses.replace(facts, entries=frozenset())


# The closed query list of KernelElf.md, in selector order.
QUERY_HEADER = 'Source/Boot/KernelEntryAbi.hpp'
QUERY_SELECTORS = (
    'EvidenceAddress', 'FirmwareApicId', 'PagingCapabilityError', 'ProcessorStateCaptureError',
    'PreparationFailed', 'PreparationError', 'CaptureError', 'PageTableError', 'PageTableBuildError',
    'PageTableVerificationError', 'MappingError', 'MappingIndex', 'MtrrCapability', 'MtrrDefaultType',
    'VariableMtrrCount', 'VariableMtrrBase', 'VariableMtrrMask', 'RuntimeRejectionAddress',
    'FinalMapRejectionAddress', 'TableRejected', 'TscSourcesPresent', 'TscSourcesTried', 'MetadataArenaPages',
)


@register
class KernelQueryClosedList(Property):
    """The kernel's query interface is the closed list KernelElf.md names.

    `KernelQuery` (Boot/KernelEntryAbi.hpp) enumerates exactly those
    selectors, numbered from zero in that order: a selector added, removed or
    renumbered without the document and this list changes the loader/kernel
    boundary silently (kernel image contract; physical memory management appended
    MetadataArenaPages).
    """
    variants = ('firmware',)

    def extract(self, context, variant):
        from AsteriaVerify import Layout
        selectors = Layout.enumerators(Layout.read(context.root / QUERY_HEADER), 'KernelQuery')
        return tuple(sorted(selectors.items(), key=lambda item: item[1]))

    def verify(self, facts):
        expected = tuple((name, index) for index, name in enumerate(QUERY_SELECTORS))
        return [] if facts == expected else ['KernelQuery is %s, the closed list is %s' % (facts, expected)]

    def mutants(self, facts):
        yield 'MetadataArenaPages dropped', tuple(item for item in facts if item[0] != 'MetadataArenaPages')
        yield 'an unlisted selector', facts + (('KernelAllocate', len(facts)),)
        if len(facts) > 1:
            yield 'two selectors swapped', ((facts[1][0], 0), (facts[0][0], 1)) + facts[2:]
