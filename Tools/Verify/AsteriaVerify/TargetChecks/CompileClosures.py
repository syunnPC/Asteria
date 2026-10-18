# SPDX-License-Identifier: Apache-2.0
"""Compile-check objects and their relocatable closures (both ISAs).

The O0 and O2 compile-check builds compile every production translation
unit, the Executive services, the target fixtures and the ABI contract with
the freestanding target flags, and link them into relocatable closures:
`NucleusCore.o`, one closure per timer fixture, `ExecutiveProduction.o` and
`ExecutiveCore.o`. These checks keep those closures closed and free of C++
runtime machinery the kernel does not provide.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Properties
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Elf import ET_REL, ElfFile

COMPILE_VARIANTS = ('O0', 'O2')
IA64_LINK_SCRIPT_SYMBOLS = frozenset({
    '__gp',
    'AsteriaImageBase',
    'AsteriaIa64LoadedImageEnd',
    'AsteriaIa64LoadedReadOnlyDataStart', 'AsteriaIa64LoadedReadOnlyDataEnd',
})
CLOSURES = ('nucleus_o', 'timer_notification_closure', 'deferred_timer_closure',
            'executive_production', 'executive_core')


def compile_objects(context, variant: str) -> dict[str, object]:
    """Label → path of every compile-check object and closure of a variant."""
    artifacts = {'%s %s' % (group, source): path for group in sorted(context.groups)
                 if group == variant or group.startswith(variant + '-')
                 for source, path in sorted(context.objects(group).items())}
    artifacts.update({'%s:%s' % (variant, closure): context.artifact('%s:%s' % (variant, closure))
                      for closure in CLOSURES})
    return artifacts


class _CompileProperty(Property):
    variants = COMPILE_VARIANTS


@register
class CompileCheckHeaders(_CompileProperty):
    """Every compile-check object and closure is an ELF64 LSB relocatable
    object for the target machine with the ABI flags of the ISA (e_flags
    0x10, EF_IA_64_ABI64, on IA-64; 0 on AMD64)."""

    def extract(self, context, variant):
        return (context.isa, {label: Properties.header_of(ElfFile(path))
                              for label, path in compile_objects(context, variant).items()})

    def verify(self, facts):
        isa, headers = facts
        return ['%s: %s' % (label, problem) for label, header in headers.items()
                for problem in Properties.header_problems(header, isa, ET_REL)]

    def mutants(self, facts):
        isa, headers = facts
        label = next(iter(headers))
        for description, changes in (('executable type', dict(type=2)),
                                     ('other machine', dict(machine=headers[label].machine ^ 0x7C)),
                                     ('other ABI flags', dict(flags=headers[label].flags ^ 0x10)),
                                     ('big-endian', dict(data=2))):
            yield description, (isa, {**headers, label: dataclasses.replace(headers[label], **changes)})


@register
class CompileCheckForbiddenSections(_CompileProperty):
    """No compile-check object or closure carries exception tables, static
    constructors or destructors, TLS or dynamic-linking sections, nor (on
    IA-64) unwind tables or function-descriptor sections of its own."""

    def extract(self, context, variant):
        return (context.isa, {label: tuple(section.name for section in ElfFile(path).sections)
                              for label, path in compile_objects(context, variant).items()})

    def verify(self, facts):
        isa, sections = facts
        return ['%s: %s' % (label, problem) for label, names in sections.items()
                for problem in Properties.forbidden_section_problems(names, isa, linked=False)]

    def mutants(self, facts):
        isa, sections = facts
        label = next(label for label in sections if label.endswith(':nucleus_o'))
        names = list(Properties.FORBIDDEN_SECTIONS)
        if isa == 'Ia64':
            names += ['.IA_64.unwind', '.IA_64.unwind_info', '.opd']
        for name in names:
            yield 'adds %s' % name, (isa, {**sections, label: sections[label] + (name,)})


@register
class ClosureUndefinedSymbols(_CompileProperty):
    """The closures resolve every reference inside themselves.

    - `NucleusCore.o` and both timer-fixture closures reference nothing
      outside the nucleus; on IA-64 only the linker-script symbols (`__gp`
      and the loaded-read-only-data and initial-user-image bounds) remain.
    - `ExecutiveProduction.o` needs at most the compiler-generated `memcpy`
      and `memset`, and `ExecutiveCore.o`, which adds `Runtime/Memory.cpp`,
      needs nothing: the Executive never links against the nucleus.
    - The ABI contract object needs no runtime symbol at all.
    """

    def extract(self, context, variant):
        nucleus = (IA64_LINK_SCRIPT_SYMBOLS if context.isa == 'Ia64' else frozenset()) | frozenset({
            'AsteriaKernelTextStart', 'AsteriaKernelTextEnd'})
        allowed = {'nucleus_o': nucleus, 'timer_notification_closure': nucleus, 'deferred_timer_closure': nucleus,
                   'executive_production': frozenset({'memcpy', 'memset'}), 'executive_core': frozenset()}
        facts = {'%s:%s' % (variant, closure): (tuple(Properties.undefined_names(ElfFile(context.artifact(
            '%s:%s' % (variant, closure))))), allowed[closure]) for closure in CLOSURES}
        abi = context.object('%s-fixtures' % variant, 'Tests/Target/AbiContractTests.cpp')
        facts['%s ABI contract' % variant] = (tuple(Properties.undefined_names(ElfFile(abi))), frozenset())
        return facts

    def verify(self, facts):
        return ['%s: %s' % (label, problem) for label, (undefined, allowed) in facts.items()
                for problem in Properties.undefined_problems(undefined, allowed)]

    def mutants(self, facts):
        for label, (undefined, allowed) in facts.items():
            yield 'unresolved reference in %s' % label, {
                **facts, label: (undefined + ('AsteriaUnresolvedMutantSymbol',), allowed)}
        label = next(label for label in facts if label.endswith(':executive_core'))
        yield 'executive core needs memcpy', {**facts, label: (('memcpy',), facts[label][1])}


@register
class TargetFixtureEntryPoints(_CompileProperty):
    """The target fixtures export their entry points as the function-symbol
    contract requires: one GLOBAL DEFAULT FUNC with non-zero size inside a
    section whose flags are exactly ALLOC|EXECINSTR."""
    FIXTURES = {
        'Tests/Kernel/Time/TimerNotificationDeliveryTests.cpp': 'AsteriaTimerNotificationDeliveryTargetFixture',
        'Tests/Kernel/Time/DeferredTimerWorkerTests.cpp': 'AsteriaDeferredTimerWorkerTargetFixture',
    }

    def extract(self, context, variant):
        contract = Properties.FunctionContract(Properties.EXPORTED, alignment=16 if context.isa == 'Ia64' else 1)
        return contract, {symbol: tuple(Properties.symbol_facts(ElfFile(context.object(
            '%s-fixtures' % variant, source)), symbol)) for source, symbol in self.FIXTURES.items()}

    def verify(self, facts):
        contract, symbols = facts
        return [problem for symbol, entries in symbols.items() for problem in contract.problems(entries, symbol)]

    def mutants(self, facts):
        contract, symbols = facts
        for symbol, entries in symbols.items():
            for description, changed in contract.mutants(entries):
                yield '%s: %s' % (symbol, description), (contract, {**symbols, symbol: changed})


@register
class NativePreemptionProbeSelection(Property):
    """Only the opt-in build dispatches the one-shot native-preemption
    diagnostic.

    The default `KernelEntry` object references
    `...DispatchNativeExternalPreemptionProbeEv` zero times; the compile-check
    probe variant (both probe macros set) exactly once; the firmware object as
    many times as its ASTERIA_IA64_NATIVE_PREEMPTION_PROBE setting says.
    """
    isas = ('Ia64',)
    SOURCE = 'Source/Kernel/Boot/KernelEntry.cpp'
    PATTERN = re.compile(r'DispatchNativeExternalPreemptionProbeEv$')

    def extract(self, context, variant):
        def references(group):
            return sum(1 for symbol in ElfFile(context.object(group, self.SOURCE)).undefined_symbols()
                       if self.PATTERN.search(symbol.name))
        if variant == 'firmware':
            expected = 1 if context.option('ia64_native_preemption_probe') == '1' else 0
            return {'firmware KernelEntry': (references('firmware'), expected)}
        return {'%s default KernelEntry' % variant: (references(variant), 0),
                '%s probe KernelEntry' % variant: (references('%s-probe' % variant), 1)}

    def verify(self, facts):
        return ['%s references the probe dispatcher %d times, expected %d' % (label, count, expected)
                for label, (count, expected) in facts.items() if count != expected]

    def mutants(self, facts):
        for label, (count, expected) in facts.items():
            yield 'extra reference in %s' % label, {**facts, label: (count + 1, expected)}
            if count:
                yield 'no reference in %s' % label, {**facts, label: (0, expected)}


@register
class CallGraphFiles(Property):
    """Every IA-64 kernel C++ translation unit of the compile-check and
    firmware builds has a non-empty `-fcallgraph-info=su` call graph next to
    its object; the native-preemption corridor reads them. Assembly (.S)
    units have no call graph."""
    isas = ('Ia64',)

    def extract(self, context, variant):
        groups = [variant] if variant == 'firmware' else [variant, '%s-executive' % variant, '%s-runtime' % variant]
        facts = {}
        for group in groups:
            for source, path in context.objects(group).items():
                if source.endswith('.cpp'):
                    graph = context.side_file(path, '.ci')
                    facts['%s %s' % (group, source)] = graph.stat().st_size if graph.exists() else -1
        return facts

    def verify(self, facts):
        return ['%s has %s call graph' % (label, 'no' if size < 0 else 'an empty')
                for label, size in facts.items() if size <= 0]

    def mutants(self, facts):
        label = next(iter(facts))
        yield 'missing call graph', {**facts, label: -1}
        yield 'empty call graph', {**facts, label: 0}
