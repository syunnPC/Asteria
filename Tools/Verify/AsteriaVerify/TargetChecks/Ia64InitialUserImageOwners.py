# SPDX-License-Identifier: Apache-2.0
"""IA-64 initial-user image owners and the default boot object.

The initial user image is staged by `Ia64InitialUserImageOwner` (decode,
arena allocation, I-cache publication), materialized into Frame and
MemoryObject kernel objects by `Ia64InitialUserImageMemoryOwner`, and mapped
by `Ia64InitialUserImageMappingOwner`; `KernelEntry.cpp` drives the three
owners from `RunPostExitBootstrap` and `AsteriaIa64RunOwnedExecutionContext`.
All of it is compiler-generated C++, so every check here is a register- and
layout-agnostic property evaluated on each variant that compiles it (O0, O2
and the firmware), never a golden.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Mutate, Properties, Subjects
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.ControlFlow import MARKERS, Code
from AsteriaVerify.Elf import load

OWNER = 'Source/Platform/Ia64/Executable/Ia64InitialUserImageOwner.cpp'
MEMORY_OWNER = 'Source/Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.cpp'
MAPPING_OWNER = 'Source/Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.cpp'
BOOT = 'Source/Kernel/Boot/KernelEntry.cpp'

_EXECUTABLE = '_ZN7Asteria8Platform4Ia6410Executable'
# Stage validates a borrowed view against the recorded boot-file region.
SOURCE_GET = (_EXECUTABLE + '26Ia64InitialUserImageSource10CreateViewERKNS_4Boot14BootFileRegionE'
              'NS_10Executable3Elf13Elf64ByteViewE')
P64_DECODE = (_EXECUTABLE + '12Ia64P64Image14DecodeLoadPlanENS_10Executable3Elf13Elf64ByteViewERKNS5_19Elf64Load'
              'PlanLimitsEhhRNS2_15Ia64P64LoadPlanE')
ALLOCATE = '_ZN7Asteria6Kernel6Memory22BootstrapPageAllocator8AllocateEyy'
OWNER_STAGE = (_EXECUTABLE + '25Ia64InitialUserImageOwner5StageERNS_6Kernel6Memory22BootstrapPageAllocatorEhh'
               'NS_10Executable3Elf13Elf64ByteViewERKNS_4Boot14BootFileRegionE')
PUBLISH = (_EXECUTABLE + '12_GLOBAL__N_1L23PublishExecutableSlicesEPKhRKNS_6Kernel6Memory17PhysicalPageRangeEPKNS2_31'
           'Ia64InitialUserImageSegmentPlanEm')
CLAIM = _EXECUTABLE + '25Ia64InitialUserImageOwner19ClaimForMemoryOwnerEv'
MATERIALIZE_PREFIX = _EXECUTABLE + '31Ia64InitialUserImageMemoryOwner11Materialize'
INSTALL = (_EXECUTABLE + '32Ia64InitialUserImageMappingOwner7InstallERNS2_31Ia64InitialUserImageMemoryOwnerERNS1_6'
           'Memory20Ia64BspRuntimeMemoryERNS_6Kernel6Object14ObjectRegistryENS6_24Ia64BspRuntimeUserDomainE')
ACTIVATE = '_ZN7Asteria8Platform4Ia646Memory27InitialTranslationActivator8ActivateEv'
_BOOT = '_ZN7Asteria4Boot9Bootstrap12_GLOBAL__N_1L'
BOOT_STAGE = _BOOT + '25StageIa64InitialUserImageEv'
SHARED_ROOT = _BOOT + '35AllocateIa64SharedRootShootdownPageEv'
RUN_POST_EXIT = _BOOT + '20RunPostExitBootstrapEv'
PROBE_STAGE = _BOOT + '34StageIa64UserExecutionProbeStorageEv'
PREPARE_OBJECTS = _BOOT + '34PrepareIa64InitialUserImageObjectsEv'
PREPARE_MAPPINGS = _BOOT + '35PrepareIa64InitialUserImageMappingsEv'
PREPARE_RUNTIME_MEMORY = _BOOT + '27PrepareIa64BspRuntimeMemoryEv'
RUN_OWNED = 'AsteriaIa64RunOwnedExecutionContext'

# Dependency allowlists pin the permitted undefined symbols
# (update them when a reviewed change adds a dependency); the denylists state
# the layering rule and must hold whatever the allowlists say.
OWNER_UNDEFINED = frozenset({
    SOURCE_GET, P64_DECODE, ALLOCATE,
    _EXECUTABLE.replace('_ZN', '_ZNK', 1) + '15Ia64P64LoadPlan10GetSegmentEm',
    'memcmp', 'memcpy', 'memset',   # Explicit calls in the owner's source
})
# The staging owner must not reach object, mapping, capability, scheduler or
# launch code.
OWNER_DENIED = r'(Kernel6Object|ObjectRegistry|MemoryObject|AddressSpace|Mapping|Capability|Scheduler|UserModeLaunch)'
_OBJECT, _MEMORY = '_ZN7Asteria6Kernel6Object', '_ZN7Asteria6Kernel6Memory'
MEMORY_OWNER_UNDEFINED = frozenset({
    _MEMORY + '12MemoryObject10InitializeERNS0_6Object14ObjectRegistryENS0_8Resource22ResourceDomainObjectIdERNS6_14'
              'ResourceDomainERKNS1_22MemoryObjectDescriptorENS1_18MemoryObjectChargeE',
    _MEMORY + '5Frame10InitializeENS0_6Object16ResourceDomainIdERKNS1_15FrameDescriptorE',
    _OBJECT + '14ObjectRegistry9PinObjectENS1_8ObjectIdENS1_10ObjectTypeERNS1_9ObjectPinE',
    _OBJECT + '20RootCapabilityIssuer12CreateObjectERNS1_13IKernelObjectE',
    _OBJECT + '9ObjectPin5ResetEv',
    _OBJECT + '9ObjectPinD1Ev',
    CLAIM,
    '_ZNK7Asteria6Kernel6Memory12MemoryObject11GetSnapshotEv',
    '_ZNK7Asteria6Kernel6Memory5Frame11GetSnapshotEv',
    '_ZNK7Asteria6Kernel6Memory5Frame14GetChargeOwnerEv',
    '_ZNK7Asteria6Kernel6Object14ObjectRegistry14GetActiveCountEv',
    # The registry's attached capacity bounds the preflight.
    '_ZNK7Asteria6Kernel6Object14ObjectRegistry11GetCapacityEv',
    '_ZNK7Asteria6Kernel6Object9ObjectPin11GetObjectIdEv',
    '_ZNK7Asteria6Kernel6Object9ObjectPin13GetObjectTypeEv',
    '_ZNK7Asteria6Kernel6Object9ObjectPin7IsValidEv',
    '_ZNK7Asteria6Kernel6Object9ObjectPin9GetObjectEv',
    '_ZNK7Asteria6Kernel8Resource14ResourceDomain11GetSnapshotEv',
    '_ZNK7Asteria6Kernel8Resource14ResourceDomain13GetIdentifierEv',
    _EXECUTABLE.replace('_ZN', '_ZNK', 1) + '25Ia64InitialUserImageOwner14GetSegmentPlanEm',
    _EXECUTABLE.replace('_ZN', '_ZNK', 1) + '25Ia64InitialUserImageOwner15GetStageSummaryEv',
    'memcpy', 'memset',   # Compiler-generated structure copies and zeroing (codegen-dependent)
})
MEMORY_OWNER_DENIED = r'(AddressSpace|FaultFrameMapping|UserModeLaunch|CapabilitySpace.*Mint)'
# The mapping owner goes through the pinned-frame-mapping preparation and the
# AddressSpace begin/complete protocol on the BSP's ready user root
# (demangled substrings).
MAPPING_OWNER_REQUIRED = (
    'Ia64InitialUserImageMemoryOwner::GetSummary() const',
    'Ia64InitialUserImageMemoryOwner::GetRangePlan(',
    'Ia64InitialUserImageMemoryOwner::BorrowRange(',
    'Ia64BspRuntimeMemory::GetSnapshot() const',
    'Ia64BspRuntimeMemory::BorrowReadyUserRoot(',
    'Ia64BspRuntimeMemory::GetUserAddressSpaceSnapshot(',
    'ObjectRegistry::PinObject(',
    'PinnedFrameMappingPreparation::Prepare(',
    'PinnedFrameMappingPreparation::GetMapping() const',
    'AddressSpace::BeginPinnedFrameMapping(',
    'AddressSpace::CompleteMapping(',
)
MAPPING_OWNER_DENIED = r'(FaultToken|FaultFrameMapping|Dispatcher|Capability|Scheduler)'

_LABELS = {
    SOURCE_GET: 'Ia64InitialUserImageSource::CreateView', P64_DECODE: 'Ia64P64Image::DecodeLoadPlan',
    ALLOCATE: 'BootstrapPageAllocator::Allocate', OWNER_STAGE: 'Ia64InitialUserImageOwner::Stage',
    PUBLISH: 'PublishExecutableSlices', CLAIM: 'Ia64InitialUserImageOwner::ClaimForMemoryOwner',
    INSTALL: 'Ia64InitialUserImageMappingOwner::Install', ACTIVATE: 'InitialTranslationActivator::Activate',
    BOOT_STAGE: 'StageIa64InitialUserImage', SHARED_ROOT: 'AllocateIa64SharedRootShootdownPage',
    RUN_POST_EXIT: 'RunPostExitBootstrap', PROBE_STAGE: 'StageIa64UserExecutionProbeStorage',
    PREPARE_OBJECTS: 'PrepareIa64InitialUserImageObjects', PREPARE_MAPPINGS: 'PrepareIa64InitialUserImageMappings',
    PREPARE_RUNTIME_MEMORY: 'PrepareIa64BspRuntimeMemory',
}


def short(symbol: str) -> str:
    """A readable label for a mangled name."""
    return 'Ia64InitialUserImageMemoryOwner::Materialize' if symbol.startswith(MATERIALIZE_PREFIX) \
        else _LABELS.get(symbol, symbol)


def decode(context, variant: str, source: str, symbol: str) -> Code:
    return Subjects.code(context, context.object(variant, source), symbol)


class _Ia64Property(Property):
    isas = ('Ia64',)


# Helper symbols ----------------------------------------------------------------------------

@register
class Ia64InitialUserImageHelperSymbols(_Ia64Property):
    """The functions whose call structure the call-site checks inspect stay
    distinct out-of-line functions, each with exactly one definition: the
    owner's local `PublishExecutableSlices`; the boot object's local
    `StageIa64InitialUserImage`, `AllocateIa64SharedRootShootdownPage`,
    `RunPostExitBootstrap`, `PrepareIa64InitialUserImageObjects` and
    `PrepareIa64InitialUserImageMappings` (`gnu::noinline, gnu::noipa`); the
    memory owner's global `Materialize`; and the mapping owner's global
    `Install(Ia64InitialUserImageMemoryOwner&, Ia64BspRuntimeMemory&,
    ObjectRegistry&, Ia64BspRuntimeUserDomain)` (the exact mangled name pins
    what installation may borrow). Each is a FUNC of non-zero size in a section whose flags are
    exactly ALLOC|EXECINSTR."""
    LOCAL = Properties.FunctionContract(Properties.LOCAL, alignment=16, size_multiple=16)
    GLOBAL = Properties.FunctionContract(Properties.EXPORTED, alignment=16, size_multiple=16)

    def extract(self, context, variant):
        def matching(source, predicate):
            elf = load(context.object(variant, source))
            return tuple(Properties.SymbolFacts(symbol, elf.section_of(symbol), True)
                         for symbol in elf.defined_symbols() if predicate(symbol.name))
        return tuple((short(name), matching(source, lambda symbol, name=name: symbol == name), contract)
                     for source, name, contract in (
                         (OWNER, PUBLISH, self.LOCAL), (BOOT, BOOT_STAGE, self.LOCAL), (BOOT, SHARED_ROOT, self.LOCAL),
                         (BOOT, RUN_POST_EXIT, self.LOCAL), (BOOT, PREPARE_OBJECTS, self.LOCAL),
                         (BOOT, PREPARE_MAPPINGS, self.LOCAL), (MAPPING_OWNER, INSTALL, self.GLOBAL))) + (
            (short(MATERIALIZE_PREFIX), matching(MEMORY_OWNER, lambda symbol: symbol.startswith(MATERIALIZE_PREFIX)),
             self.GLOBAL),)

    def verify(self, facts):
        return [problem for label, entries, contract in facts for problem in contract.problems(entries, label)]

    def mutants(self, facts):
        for index, (label, entries, contract) in enumerate(facts):
            for description, changed in contract.mutants(entries):
                yield '%s: %s' % (label, description), facts[:index] + ((label, changed, contract),) + facts[index + 1:]


@register
class Ia64DefaultBootProbeStaging(_Ia64Property):
    """The default IA-64 boot never allocates or stages the synthetic
    user-execution probe: the default `KernelEntry` object has no
    `StageIa64UserExecutionProbeStorage` symbol at all, while the probe
    configuration (both probe macros set; the compile-check `-probe` object)
    defines it once, which proves the symbol name is still the one the rule
    looks for. The firmware object has it exactly when the tree is configured
    with ASTERIA_IA64_SYNTHETIC_USER_PROBE=1."""

    def extract(self, context, variant):
        def entries(group):
            symbols = load(context.object(group, BOOT)).symbols_named(PROBE_STAGE)
            return len(symbols), len([symbol for symbol in symbols if symbol.section_index])
        if variant == 'firmware':
            return (('firmware KernelEntry', *entries('firmware'),
                     1 if context.option('ia64_synthetic_user_probe') == '1' else 0),)
        return (('%s default KernelEntry' % variant, *entries(variant), 0),
                ('%s probe KernelEntry' % variant, *entries('%s-probe' % variant), 1))

    def verify(self, facts):
        problems = []
        for label, total, defined, expected in facts:
            if expected == 0 and total:
                problems.append('%s has %d %s symbols, expected none' % (label, total, short(PROBE_STAGE)))
            elif expected and (total, defined) != (1, 1):
                problems.append('%s has %d %s symbols (%d defined), expected one definition'
                                % (label, total, short(PROBE_STAGE), defined))
        return problems

    def mutants(self, facts):
        for position, (label, total, defined, expected) in enumerate(facts):
            def changed(*counts):
                return facts[:position] + ((label, *counts, expected),) + facts[position + 1:]
            yield 'probe staging symbol added to %s' % label, changed(total + 1, defined + 1)
            if expected == 0:
                yield 'probe staging referenced by %s' % label, changed(total + 1, defined)
            else:
                yield 'probe staging missing from %s' % label, changed(0, 0)
                yield 'probe staging only referenced by %s' % label, changed(total, 0)


# Dependencies -------------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class Dependencies:
    """Undefined symbols of one object and the reviewed rules they obey."""
    label: str
    undefined: tuple[str,...]
    demangled: tuple[str,...]
    allowed: frozenset[str] | None
    denied: str
    denied_demangled: bool
    required: tuple[str,...]


@register
class Ia64InitialUserImageDependencies(_Ia64Property):
    """Layering of the three owners, from their undefined symbols.

    - Staging owner: exactly {Ia64InitialUserImageSource::CreateView,
      Ia64P64Image::DecodeLoadPlan, BootstrapPageAllocator::Allocate,
      Ia64P64LoadPlan::GetSegment, memcmp, memcpy, memset}; it never reaches
      object, mapping, capability, scheduler or launch code.
    - Memory owner: exactly the reviewed 21 names (registering Frame and
      MemoryObject objects and claiming the staged owner; its memcpy/memset
      are compiler-generated); it never maps, launches or mints capabilities
      (denylist AddressSpace, FaultFrameMapping, UserModeLaunch,
      CapabilitySpace...Mint).
    - Mapping owner: it goes through the pinned-frame-mapping preparation and
      the AddressSpace begin/complete protocol on the BSP's ready initial
      root (required dependencies) and stays out of fault handling,
      dispatchers, capabilities and scheduling (denylist). kernel image contract permits
      exactly RootCapabilityIssuer::CreateObject for the retained module Frame
      and read-only MemoryObject; capability minting remains forbidden.

    The denylists hold whatever the allowlists say, so an allowlist update
    cannot silently admit a layering violation."""

    def extract(self, context, variant):
        facts = []
        for label, source, allowed, denied, denied_demangled, required in (
                ('staging owner', OWNER, OWNER_UNDEFINED, OWNER_DENIED, False, ()),
                ('memory owner', MEMORY_OWNER, MEMORY_OWNER_UNDEFINED, MEMORY_OWNER_DENIED, False, ()),
                ('mapping owner', MAPPING_OWNER, None, MAPPING_OWNER_DENIED, True, MAPPING_OWNER_REQUIRED)):
            names = tuple(Properties.undefined_names(load(context.object(variant, source))))
            facts.append(Dependencies(label, names, context.demangle(names), allowed, denied, denied_demangled,
                                      required))
        return tuple(facts)

    def verify(self, facts):
        problems = []
        for owner in facts:
            if owner.allowed is not None:
                problems += ['%s: %s' % (owner.label, problem)
                             for problem in Properties.undefined_problems(owner.undefined, owner.allowed, exact=True)]
            problems += ['%s: dependency %s crosses a forbidden layer boundary (%s)' % (owner.label, demangled,
                                                                                       owner.denied)
                         for mangled, demangled in zip(owner.undefined, owner.demangled)
                         if re.search(owner.denied, demangled if owner.denied_demangled else mangled)
                         and not (owner.label == 'mapping owner' and demangled ==
                                  'Asteria::Kernel::Object::RootCapabilityIssuer::CreateObject(Asteria::Kernel::Object::IKernelObject&)')]
            problems += ['%s: no dependency on %s; the owner bypasses it' % (owner.label, required)
                         for required in owner.required if not any(required in name for name in owner.demangled)]
        return problems

    def mutants(self, facts):
        def added(position, mangled, demangled, *, allow=False):
            owner = facts[position]
            return Mutate.replace_at(facts, position, undefined=owner.undefined + (mangled,),
                                     demangled=owner.demangled + (demangled,),
                                     allowed=owner.allowed | {mangled} if allow else owner.allowed)

        def dropped(position, fragment):
            kept = [pair for pair in zip(facts[position].undefined, facts[position].demangled)
                    if fragment not in pair[1]]
            return Mutate.replace_at(facts, position, undefined=tuple(name for name, _ in kept),
                                     demangled=tuple(name for _, name in kept))

        scheduler = ('_ZN7Asteria6Kernel9Scheduler16ProcessorRunQueue7EnqueueEv',
                     'Asteria::Kernel::Scheduler::ProcessorRunQueue::Enqueue()')
        registry = ('_ZN7Asteria6Kernel6Object14ObjectRegistry9PinObjectEv',
                    'Asteria::Kernel::Object::ObjectRegistry::PinObject()')
        address_space = ('_ZN7Asteria6Kernel6Memory12AddressSpace15CompleteMappingEv',
                         'Asteria::Kernel::Memory::AddressSpace::CompleteMapping()')
        launch = ('_ZN7Asteria8Platform4Ia649Scheduler18Ia64UserModeLaunch6LaunchEv',
                  'Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunch::Launch()')
        mint = ('_ZN7Asteria6Kernel6Object15CapabilitySpace4MintEv', 'Asteria::Kernel::Object::CapabilitySpace::Mint()')
        fault_token = ('_ZN7Asteria6Kernel6Memory10FaultToken7ReleaseEv',
                       'Asteria::Kernel::Memory::FaultToken::Release()')
        yield 'staging owner gains an object-registry dependency', added(0, *registry)
        yield 'reviewed allowlist admits a scheduler dependency of the staging owner', added(0, *scheduler, allow=True)
        yield 'staging owner loses memcmp', dropped(0, 'memcmp')
        yield 'memory owner gains an AddressSpace dependency', added(1, *address_space)
        yield 'reviewed allowlist admits an AddressSpace dependency of the memory owner', \
            added(1, *address_space, allow=True)
        yield 'reviewed allowlist admits a launch dependency of the memory owner', added(1, *launch, allow=True)
        yield 'reviewed allowlist admits a capability mint by the memory owner', added(1, *mint, allow=True)
        yield 'memory owner no longer copies through memcpy', dropped(1, 'memcpy')
        yield 'mapping owner bypasses AddressSpace::CompleteMapping', dropped(2, 'AddressSpace::CompleteMapping(')
        yield 'mapping owner gains capability minting', added(2, *mint)
        yield 'mapping owner gains a scheduler dependency', added(2, *scheduler)
        yield 'mapping owner gains a fault-token dependency', added(2, *fault_token)


# Call structure ---------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class CallFacts:
    """Decoded callers by symbol, the caller → callee edges that must be
    crossed by exactly one direct b0 call, and the call orders required per
    caller."""
    functions: dict[str, Code]
    calls: tuple[tuple[str, str],...]
    orders: tuple[tuple[str, tuple[str,...]],...]

    def site(self, caller: str, callee: str) -> int:
        return Properties.branch_references(self.functions[caller].instructions, callee)[0]

    def edited(self, caller: str, code: Code) -> 'CallFacts':
        return dataclasses.replace(self, functions={**self.functions, caller: code})


class _CallProperty(_Ia64Property):
    """Mutants of the call-site rules, derived from the decoded callers: for
    the first call through a relocation, a second relocation to the callee,
    the relocation in slot 1, a tail jump instead of a call, a call through
    b6 and a predicated call; for the first assembler-resolved call, a
    missing and a duplicated call; for every order, the first and the last
    two calls exchanged."""
    SOURCES: tuple[tuple[str, str],...] = ()     # (source, caller symbol)

    def functions(self, context, variant) -> dict[str, Code]:
        return {symbol: decode(context, variant, source, symbol) for source, symbol in self.SOURCES}

    def verify(self, facts):
        problems = ['%s: %s' % (short(caller), problem) for caller, callee in facts.calls
                    for problem in Properties.call_problems(facts.functions[caller].instructions, callee,
                                                            short(callee))]
        return problems + ['%s: %s' % (short(caller), problem) for caller, callees in facts.orders
                           for problem in Properties.order_problems(facts.functions[caller].instructions, callees,
                                                                    [short(callee) for callee in callees])]

    def mutants(self, facts):
        def site(edge):
            return facts.functions[edge[0]], facts.site(*edge), '%s -> %s' % (short(edge[0]), short(edge[1]))

        relocated = [edge for edge in facts.calls
                     if facts.functions[edge[0]].operations[facts.site(*edge)].target.kind == 'reloc']
        if relocated:
            code, index, label = site(relocated[0])
            call = code.instructions[index]
            token, bundle = call.relocation, call.address & ~0xF
            slot_one = next(position for position, instruction in enumerate(code.instructions)
                            if instruction.address == bundle + 6)
            for description, changed in (
                    ('second relocation to %s', code.duplicate(index)),
                    ('relocation of %s in slot 1', code.replace(
                        slot_one, text=code.instructions[slot_one].text + ' ' + token, relocation=token).replace(
                        index, text=call.text.replace(token, '<+%#x>' % (bundle - code.start)), relocation=None)),
                    ('tail jump instead of call for %s', code.replace(index, text='br.cond.sptk.many ' + token)),
                    ('call through b6 for %s', code.replace(index, text=call.text.replace('b0=', 'b6='))),
                    ('predicated call for %s', code.replace(index, predicate='p06'))):
                yield description % label, facts.edited(relocated[0][0], changed)
        resolved = [edge for edge in facts.calls if edge not in relocated]
        if resolved:
            code, index, label = site(resolved[0])
            yield 'missing call %s' % label, facts.edited(resolved[0][0], code.nop(index))
            yield 'duplicate call %s' % label, facts.edited(resolved[0][0], code.duplicate(index))
        for caller, callees in facts.orders:
            code = facts.functions[caller]
            for first, second in {(callees[0], callees[1]), (callees[-2], callees[-1])}:
                a, b = facts.site(caller, first), facts.site(caller, second)
                x, y = code.instructions[a], code.instructions[b]
                yield '%s calls %s before %s' % (short(caller), short(second), short(first)), facts.edited(
                    caller, code.replace(a, text=y.text, relocation=y.relocation).replace(
                        b, text=x.text, relocation=x.relocation))


@register
class Ia64InitialUserImageStagingCalls(_CallProperty):
    """Staging reads and decodes the embedded image before it allocates the
    arena, and publishes the arena's executable slices last:
    `Ia64InitialUserImageOwner::Stage` calls `Ia64InitialUserImageSource::CreateView`,
    `Ia64P64Image::DecodeLoadPlan`, `BootstrapPageAllocator::Allocate` and the
    local `PublishExecutableSlices` through exactly one direct `br.call b0=`
    each (a PCREL21B relocation in slot 2 of the call bundle where the object
    has one), in that address order. The boot object's
    `StageIa64InitialUserImage` calls `Stage` exactly once, and
    `RunPostExitBootstrap` stages the image before it allocates the
    shared-root shootdown page. (Address order is a proxy for program order;
    the calls are compiler-generated.)"""
    SOURCES = ((OWNER, OWNER_STAGE), (BOOT, BOOT_STAGE), (BOOT, RUN_POST_EXIT))

    def extract(self, context, variant):
        return CallFacts(self.functions(context, variant),
                         ((OWNER_STAGE, SOURCE_GET), (OWNER_STAGE, P64_DECODE), (OWNER_STAGE, ALLOCATE),
                          (OWNER_STAGE, PUBLISH), (BOOT_STAGE, OWNER_STAGE), (RUN_POST_EXIT, BOOT_STAGE),
                          (RUN_POST_EXIT, SHARED_ROOT)),
                         ((OWNER_STAGE, (SOURCE_GET, P64_DECODE, ALLOCATE, PUBLISH)),
                          (RUN_POST_EXIT, (BOOT_STAGE, SHARED_ROOT))))


@register
class Ia64InitialUserImageMaterializationCalls(_CallProperty):
    """Materialization claims the staged owner once
    (`Materialize` → `ClaimForMemoryOwner`), the boot helpers call each owner
    once (`PrepareIa64InitialUserImageObjects` → `Materialize`,
    `PrepareIa64InitialUserImageMappings` → `Install`), and
    `AsteriaIa64RunOwnedExecutionContext` prepares the BSP runtime memory,
    then the objects, then the mappings, and only then activates the private
    root (`InitialTranslationActivator::Activate`), each through exactly one
    direct `br.call b0=`, in that address order."""
    SOURCES = ((BOOT, PREPARE_OBJECTS), (BOOT, PREPARE_MAPPINGS), (BOOT, RUN_OWNED))

    def extract(self, context, variant):
        # The one global Materialize definition (its mangled parameter list is
        # not pinned); Ia64InitialUserImageHelperSymbols checks that there is one.
        names = sorted({symbol.name for symbol in load(context.object(variant, MEMORY_OWNER)).defined_symbols()
                        if symbol.name.startswith(MATERIALIZE_PREFIX)})
        materialize = names[0] if len(names) == 1 else MATERIALIZE_PREFIX
        return CallFacts({materialize: decode(context, variant, MEMORY_OWNER, materialize),
                          **self.functions(context, variant)},
                         ((materialize, CLAIM), (PREPARE_OBJECTS, materialize), (PREPARE_MAPPINGS, INSTALL),
                          (RUN_OWNED, PREPARE_RUNTIME_MEMORY), (RUN_OWNED, PREPARE_OBJECTS),
                          (RUN_OWNED, PREPARE_MAPPINGS), (RUN_OWNED, ACTIVATE)),
                         ((RUN_OWNED, (PREPARE_RUNTIME_MEMORY, PREPARE_OBJECTS, PREPARE_MAPPINGS, ACTIVATE)),))


# I-cache publication -------------------------------------------------------------------------

_BUNDLE_STRIDE = re.compile(r'adds r\d+=16,r\d+')


@register
class Ia64InitialUserImagePublication(_Ia64Property):
    """Every 16-byte bundle of each executable slice is flushed from the
    I-cache before user code can fetch it. In `PublishExecutableSlices`:

    - exactly one unpredicated `fc.i`, inside a natural loop that exits and
      advances exactly once by one bundle (`adds rX=16,rY`), and that loop
      is nested in another loop (the one over the executable segments);
    - exactly one each of `sync.i`, `srlz.i` and `mf`, unpredicated,
      consecutive in that order and each starting a new instruction group,
      outside every loop around the flush, and on every path from the flush
      to a return.

    Loops come from the control-flow graph (dominators), so the rule is
    register- and layout-agnostic: GCC closes the bundle loop with
    `br.cloop` at O2 and with a compare-and-branch through stack slots at
    O0. Segment-loop nesting distinguishes the bundle loop from the segment loop."""

    def extract(self, context, variant):
        return decode(context, variant, OWNER, PUBLISH)

    def verify(self, code):
        graph, problems = code.graph, []
        flushes = code.all(lambda step: step.mnemonic == 'fc.i')
        if len(flushes) != 1:
            return ['%d fc.i instructions, expected exactly one' % len(flushes)]
        flush = flushes[0].position
        if flushes[0].instruction.predicate is not None:
            problems.append('fc.i is predicated')
        if graph.indirect:
            return problems + ['indirect branch at #%d; the control flow cannot be established' % graph.indirect[0]]
        around = sorted((body for body in graph.loops.values() if flush in body), key=len)
        if not around:
            return problems + ['fc.i is not inside a loop']
        inner = around[0]
        if not graph.exits(inner):
            problems.append('the loop around fc.i never exits')
        strides = [index for index in sorted(inner) if _BUNDLE_STRIDE.fullmatch(code.instructions[index].text)
                   and code.instructions[index].predicate is None]
        if len(strides) != 1:
            problems.append('the loop around fc.i advances by one 16-byte bundle (adds rX=16,rY) %d times, expected '
                            'exactly once' % len(strides))
        if not any(inner < body for body in around[1:]):
            problems.append('the bundle loop around fc.i is not nested in a loop over the executable segments')
        barrier = []
        for mnemonic in ('sync.i', 'srlz.i', 'mf'):
            sites = code.all(lambda step: step.mnemonic == mnemonic)
            if len(sites) != 1:
                return problems + ['%d %s instructions, expected exactly one' % (len(sites), mnemonic)]
            if sites[0].instruction.predicate is not None:
                problems.append('%s is predicated' % mnemonic)
            barrier.append(sites[0])
        positions = [step.position for step in barrier]
        if [step.position for step in code.steps if positions[0] <= step.position <= positions[-1]] != positions:
            problems.append('sync.i, srlz.i and mf are not consecutive in that order')
        if any(positions[0] in body for body in around):
            problems.append('the sync.i;srlz.i;mf barrier lies inside a loop around fc.i')
        if positions[0] not in graph.reachable([flush]) or MARKERS & graph.reachable([flush], avoiding={positions[0]}):
            problems.append('a path from fc.i leaves the function without the sync.i;srlz.i;mf barrier')
        return problems + ['%s does not start a new instruction group' % mnemonic
                           for site, mnemonic in zip(barrier, ('sync.i', 'srlz.i', 'mf'))
                           if code.previous(site) is not None and code.previous(site).group == site.group]

    def mutants(self, facts):
        code, graph, instructions = facts, facts.graph, facts.instructions
        flush, sync, serialize, fence = (code.site(lambda step, mnemonic=mnemonic: step.mnemonic == mnemonic).position
                                         for mnemonic in ('fc.i', 'sync.i', 'srlz.i', 'mf'))
        stride = code.site(lambda step: step.position > flush and _BUNDLE_STRIDE.fullmatch(step.text))
        around = sorted((body for body in graph.loops.values() if flush in body), key=len)
        inner, outer = around[0], around[-1]
        back_edge = next(index for index in sorted(inner) if code.operations[index].is_branch
                         and any(successor in inner and successor <= index for successor in graph.successors[index]))
        outer_header = next(header for header, body in graph.loops.items() if body == outer)
        outer_exit_source, outer_exit_target = graph.exits(outer)[0]
        # The barrier moved in front of the back edge, readdressed there so that addresses stay sorted.
        kept = [instruction for index, instruction in enumerate(instructions) if index not in (sync, serialize, fence)]
        position = kept.index(instructions[back_edge])
        barrier = [dataclasses.replace(instructions[index], address=instructions[back_edge].address)
                   for index in (sync, serialize, fence)]
        yield 'predicated fc.i', code.replace(flush, predicate='p06')
        yield 'srlz.i deleted', code.replace(serialize, text='nop.m 0x0')
        yield 'srlz.i and mf swapped', code.replace(serialize, text='mf').replace(fence, text='srlz.i')
        yield 'eight-byte stride', code.replace(stride, text=stride.text.replace('=16,', '=8,'))
        yield 'bundle-loop back edge removed', code.replace(back_edge, text='nop.b 0x0', predicate=None)
        yield 'bundle loop closed unconditionally', code.replace(
            back_edge, text=re.sub(r'^br\.\S+', 'br.few', instructions[back_edge].text), predicate=None)
        yield 'second fc.i', code.duplicate(flush)
        yield 'predicated mf', code.replace(fence, predicate='p06')
        yield 'barrier inside the bundle loop', code.with_instructions(kept[:position] + barrier + kept[position:])
        yield 'sync.i and srlz.i in one instruction group', code.without_stops(sync, serialize)
        yield 'early return skips the barrier', code.insert(outer_exit_source, '(p07) br.ret.sptk.many b0')
        # The segment loop runs once: each of its back edges leaves the loop.
        exit_offset = code.step(outer_exit_target).bundle
        once = code
        for node in sorted((node for node in outer if outer_header in graph.successors[node]
                            and graph.dominates(outer_header, node)), reverse=True):
            target = code.target(node)
            if code.operations[node].is_branch and target is not None and target.position == outer_header:
                once = once.retarget(node, exit_offset)
            else:
                once = once.insert(node, 'br.few <+%#x>' % exit_offset, after=True, stop=True)
        yield 'bundle loop not nested in a segment loop', once
