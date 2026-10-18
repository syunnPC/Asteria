# SPDX-License-Identifier: Apache-2.0
"""IA-64 BSP native-preemption sentinel, landing and coordinator
(Source/Platform/Ia64/Scheduler/Ia64BspKernelRuntime.{S,cpp}): the hand-written
sentinel `AsteriaIa64RunBspNativePreemptionSentinel`, whose landing bounds
are published to Ia64ExternalPreemptionControl as the admission range for
the interrupted IIP, and the compiled coordinator
`Ia64BspKernelRuntime::CoordinateNativeExternalPreemption`, whose
retained-frame dispatch is inlined differently at O0 and at O2/in the
firmware (the property matches the call target by substring).

Every property also runs on the linked images (the compile-check
`NucleusCore.elf` and, in a probe firmware, the firmware `AsteriaBoot.elf`),
alongside the external-preemption properties of Ia64ExternalInterrupt.py.
A production firmware (built with
ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=0) contains none of the probe;
Ia64NativePreemptionProbeLinkage states that.

The golden Tests/Golden/Ia64/Scheduler/AsteriaIa64RunBspNativePreemptionSentinel.dis
pins the sentinel as the range from the sentinel symbol to the next function
(`AsteriaIa64KernelIdle`): the sentinel's ELF size stops one bundle short of
its code, because its `.size` directive is evaluated before GAS emits the
pending success-return bundle (the descriptor next to it avoids this with
`.align 16` before `.size`). The properties below decode the same range.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import Elf, Ia64, Mutate, Properties, Subjects
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Context import ContextError
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Disassembly import Instruction
from AsteriaVerify.Elf import STB_GLOBAL, STB_LOCAL, STT_FUNC, STT_NOTYPE, STT_OBJECT, STV_DEFAULT, STV_HIDDEN, Symbol
from AsteriaVerify.Subjects import CodeProperty

SENTINEL = 'AsteriaIa64RunBspNativePreemptionSentinel'
LANDING_START = 'AsteriaIa64BspNativePreemptionSentinelLandingStart'
LANDING_END = 'AsteriaIa64BspNativePreemptionSentinelLandingEnd'
IDLE_LANDING = 'AsteriaIa64BspNativePreemptionIdleLanding'
COORDINATOR = '_ZN7Asteria8Platform4Ia649Scheduler20Ia64BspKernelRuntime34CoordinateNativeExternalPreemptionEv'
RETAINED_DISPATCH = 'DispatchRetainedExternalFrameFromCoordinator'
FORBIDDEN_TRANSFERS = ('DispatchFromIdle', 'SwitchIa64SynchronousContinuation', 'SwitchTo')


class DefiningProperty(CodeProperty):
    """Subjects: the artifacts that must contain SYMBOL. The compile-check
    variants and a probe firmware always do (a missing or duplicated symbol
    then fails the extraction); a default firmware is checked when its
    linked image defines the symbol."""
    OBJECTS = True

    def subjects(self, context):
        image = 'firmware:kernel_elf'
        variants = [variant for variant in Subjects.VARIANTS if variant != 'firmware'
                    or context.configuration.get('ia64_native_preemption_probe') == '1'
                    or not context.has_artifact(image) or Elf.load(context.artifact(image)).definitions(self.SYMBOL)]
        return Subjects.artifacts(context, self.SYMBOL, variants=variants, objects=self.OBJECTS, images=True)


def sentinel_code(context, subject) -> Code:
    """The sentinel from its symbol up to the next function (the first FUNC or
    OBJECT symbol after it in its section): the ELF size of the symbol omits
    the final (success-return) bundle."""
    elf = Elf.load(subject.path)
    sentinel = elf.symbol(SENTINEL)
    if sentinel is None:
        raise ContextError('%s does not define %s' % (subject.path, SENTINEL))
    following = [entry for entry in elf.defined_symbols() if entry.section_index == sentinel.section_index
                 and entry.value > sentinel.value and entry.type in (STT_FUNC, STT_OBJECT)]
    if not following:
        raise ContextError('%s: nothing follows %s in its section' % (elf.path, SENTINEL))
    end = min(following, key=lambda entry: (entry.value, entry.name))
    return Subjects.code(context, subject.path, start=SENTINEL, end=end.name)


# Landing ---------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class LandingFacts:
    starts: tuple[Symbol, ...]           # definitions of the start marker
    ends: tuple[Symbol, ...]             # definitions of the end marker
    bundles: tuple[Instruction, ...]     # every slot (nops included) of [start, end + 16)


@register
class Ia64BspNativePreemptionLanding(DefiningProperty):
    """The admission range [LandingStart, LandingEnd) published to
    Ia64ExternalPreemptionControl is exactly one aligned bundle whose only
    instruction is the `srlz.d` sampling the pending timer interrupt (slot 0,
    the bundle ends with a stop), and interrupts are masked again at once:
    the first instruction at LandingEnd is `rsm 0x4000` in slot 0, ending its
    group, followed by `srlz.d` before any other instruction. Both markers
    are defined once, NOTYPE GLOBAL HIDDEN, in one section, and both bundles
    lie inside the sentinel's code (they are taken from its decoded range);
    in the linked images this is the final landing extent (one
    16-byte-aligned bundle). Slots, templates and nops are kept in the facts,
    which the nop-dropping golden normalization cannot express."""
    SYMBOL = SENTINEL

    def extract(self, context, subject):
        elf = Elf.load(subject.path)
        starts, ends = elf.definitions(LANDING_START), elf.definitions(LANDING_END)
        bundles = ()
        if len(starts) == 1 and len(ends) == 1:
            bundles = tuple(instruction for instruction in sentinel_code(context, subject).instructions
                            if starts[0].value <= instruction.address < ends[0].value + 16)
        return LandingFacts(starts, ends, bundles)

    def verify(self, facts):
        problems = []
        for label, entries in (('LandingStart', facts.starts), ('LandingEnd', facts.ends)):
            if len(entries) != 1:
                problems.append('%d definitions of %s, expected one' % (len(entries), label))
            elif (entries[0].type, entries[0].binding, entries[0].visibility) != (STT_NOTYPE, STB_GLOBAL, STV_HIDDEN):
                problems.append('%s is not a NOTYPE GLOBAL HIDDEN marker' % label)
        if problems:
            return problems
        start, end = facts.starts[0], facts.ends[0]
        if start.value % 16 or end.value - start.value != 16 or start.section_index != end.section_index:
            return ['the landing [%#x, %#x) is not one aligned bundle' % (start.value, end.value)]
        landing = [instruction for instruction in facts.bundles if start.value <= instruction.address < end.value]
        closing = [instruction for instruction in facts.bundles if end.value <= instruction.address]
        if len(landing) != 3 or [instruction.slot for instruction in landing] != [0, 1, 2]:
            problems.append('the landing is not exactly one decoded bundle')
        else:
            if landing[0].text != 'srlz.d' or landing[0].predicate is not None:
                problems.append('landing slot 0 is %s, expected srlz.d' % landing[0].rendered())
            others = [instruction.rendered() for instruction in landing[1:] if not instruction.is_nop]
            if others:
                problems.append('the landing bundle also executes %s' % ', '.join(others))
            if not landing[-1].stop_after:
                problems.append('the landing bundle does not end with a stop')
        meaningful = [instruction for instruction in closing if not instruction.is_nop]
        if (not closing or closing[0].slot != 0 or closing[0].text != 'rsm 0x4000'
                or closing[0].predicate is not None or not closing[0].stop_after):
            problems.append('LandingEnd does not start with `rsm 0x4000;;`')
        elif len(meaningful) < 2 or meaningful[1].text != 'srlz.d':
            problems.append('the re-mask at LandingEnd is not followed by srlz.d')
        return problems

    def mutants(self, facts):
        for description, position, changes in (
                ('second instruction in the landing bundle', 1, dict(text='mov r14=r0')),
                ('no stop after the landing bundle', 2, dict(stop_after=False)),
                ('landing re-mask removed', 3, dict(text='nop.m 0x0'))):
            yield description, dataclasses.replace(facts, bundles=Mutate.replace_at(facts.bundles, position, **changes))
        start, end = facts.starts[0], facts.ends[0]
        for description, field, marker, changes in (
                ('landing end one bundle late', 'ends', end, dict(value=end.value + 16)),
                ('misaligned landing', 'starts', start, dict(value=start.value + 8)),
                ('local landing marker', 'starts', start, dict(binding=STB_LOCAL)),
                ('exported landing marker', 'ends', end, dict(visibility=STV_DEFAULT))):
            yield description, dataclasses.replace(facts, **{field: (dataclasses.replace(marker, **changes),)})


# PSR.i window around the landing ----------------------------------------------------

@dataclasses.dataclass(frozen=True)
class SentinelFacts:
    code: Code
    landing_start: int
    landing_end: int


def psr_i(base: str):
    """Matches `ssm`/`rsm` (`base`) of exactly PSR.i."""
    return lambda step: step.operation.base == base and step.operation.immediates == (Ia64.PSR_I,)


@register
class Ia64BspNativePreemptionSentinelWindow(DefiningProperty):
    """P-PSR-I-WINDOW for the sentinel: exactly one `ssm 0x4000`; the window
    it opens contains only the landing `srlz.d` at LandingStart and closes
    with the `rsm 0x4000` at LandingEnd; every `rsm 0x4000` is followed by
    `srlz.d` alone in the next group; no branch enters the window (the retry
    re-runs the whole admission instead); and every path to a return
    re-masks interrupts, whatever the entry state, so the BSP never
    continues in C++ with psr.i set."""
    SYMBOL = SENTINEL

    def extract(self, context, subject):
        elf = Elf.load(subject.path)
        start, end = elf.symbol(LANDING_START), elf.symbol(LANDING_END)
        if start is None or end is None:
            raise ContextError('%s lacks the landing markers' % elf.path)
        return SentinelFacts(sentinel_code(context, subject), start.value, end.value)

    def verify(self, facts):
        code = facts.code
        problems = Properties.psr_i_window_problems(code, strict_exit=True, back_edges=0,
                                                    window_allows=lambda operation: operation.mnemonic == 'srlz.d')
        enables = code.all(psr_i('ssm'))
        if len(enables) == 1:
            closing = code.all(psr_i('rsm'), after=enables[0])
            window = code.window(after=enables[0], before=closing[0] if closing else None)
            if [step.instruction.address for step in window] != [facts.landing_start]:
                problems.append('the psr.i window executes %s, not only the landing at %#x'
                                % ([step.operation.text for step in window], facts.landing_start))
            if not closing or closing[0].instruction.address != facts.landing_end:
                problems.append('the psr.i window does not close at LandingEnd %#x' % facts.landing_end)
        return problems

    def mutants(self, facts):
        code = facts.code
        landing = code.site(lambda step: step.instruction.address == facts.landing_start)
        reject = code.site(lambda step: psr_i('rsm')(step) and step.instruction.address != facts.landing_end)
        closing = code.site(lambda step: psr_i('rsm')(step) and step.instruction.address == facts.landing_end)
        retry = code.site(lambda step: step.operation.is_branch and (target := code.target(step)) is not None
                          and target.position < step.position)
        for description, changed in (('load inside the window', code.insert(landing, 'ld8 r14=[r32]', after=True)),
                                     ('reject path without the re-mask', code.nop(reject)),
                                     ('no serialization after the re-mask', code.nop(code.next(closing))),
                                     ('retry branch into the landing', code.retarget(retry, landing.bundle))):
            yield description, dataclasses.replace(facts, code=changed)


# Coordinator call graph ----------------------------------------------------------------

def coordinator_calls(code: Code) -> tuple[list[tuple[int, str]], list[tuple[int, str]]]:
    """(direct call sites, other symbol references) as (index, target name)."""
    calls, references = [], []
    for index, operation in enumerate(code.operations):
        target = operation.target
        if target is not None and target.kind != 'internal' and target.symbol is not None:
            (calls if operation.is_call else references).append((index, target.symbol))
    return calls, references


@register
class Ia64BspNativePreemptionCoordinator(DefiningProperty):
    """`Ia64BspKernelRuntime::CoordinateNativeExternalPreemption` resumes the
    preempted thread only through the retained external frame: exactly one
    direct call to a function whose name contains
    `DispatchRetainedExternalFrameFromCoordinator` (the wrapper at O0, the
    `…Transaction` function once the wrapper is inlined), no other reference
    to it, and no call or reference to a `DispatchFromIdle`,
    `SwitchIa64SynchronousContinuation` or `SwitchTo…` symbol, which would
    bypass the captured interruption frame. Call sites come from PCREL21B
    relocations in objects and resolved targets in images. Compiled C++:
    register- and layout-agnostic, run on O0, O2 and the firmware."""
    SYMBOL = COORDINATOR

    def verify(self, code):
        calls, references = coordinator_calls(code)
        dispatches = [(index, name) for index, name in calls if RETAINED_DISPATCH in name]
        problems = [] if len(dispatches) == 1 else ['%d calls to %s, expected exactly one'
                                                    % (len(dispatches), RETAINED_DISPATCH)]
        problems += ['#%d references %s other than by its call' % (index, name)
                     for index, name in references if RETAINED_DISPATCH in name]
        return problems + ['#%d %s %s' % (index, 'calls' if (index, name) in calls else 'references', name)
                           for index, name in calls + references
                           if any(forbidden in name for forbidden in FORBIDDEN_TRANSFERS)]

    def mutants(self, code):
        index, name = next((index, name) for index, name in coordinator_calls(code)[0] if RETAINED_DISPATCH in name)
        yield 'second retained-frame dispatch', code.duplicate(index)
        yield 'direct context switch', code.insert(index, code.instructions[index].text.replace(
            name, name.replace(RETAINED_DISPATCH, 'SwitchToThread')), after=True)
        yield 'idle dispatch reference', code.insert(index, 'movl r14=<reloc:IMM64 %s>' % name.replace(
            RETAINED_DISPATCH, 'DispatchFromIdle'), after=True)
        yield 'retained-frame dispatch removed', code.nop(index)


# Linkage ------------------------------------------------------------------------------

PROBE_OPTION = 'ia64_native_preemption_probe'
_RUNTIME = '_ZN7Asteria8Platform4Ia649Scheduler20Ia64BspKernelRuntime'
# The code of the one-shot BSP probe (Ia64BspKernelRuntime.{S,cpp}): its
# assembly, its C ABI adapters and the runtime members only it uses.
PROBE_SYMBOLS = (
    SENTINEL, LANDING_START, LANDING_END, IDLE_LANDING, 'AsteriaIa64BspNativePreemptionCoordinator',
    'AsteriaIa64InitializeBspNativePreemptionDescriptor', 'AsteriaIa64CaptureBspNativePreemptionObservation',
    COORDINATOR, _RUNTIME + '37DispatchNativeExternalPreemptionProbeEv',
    _RUNTIME + '32LandNativeExternalPreemptionIdleEv', _RUNTIME + '27ArmNativePreemptionSentinelEv',
    _RUNTIME + '31PrepareNativePreemptionCorridorEv')


@register
class Ia64NativePreemptionProbeLinkage(Property):
    """Only a probe firmware contains the one-shot native-preemption probe.
    The compile-check objects and images define every PROBE_SYMBOLS entry
    (the probe stays verified in every tree); the firmware objects and both
    linked firmware images (production and test) define all of them when
    the tree was built with ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=1 and none
    of them otherwise, so a production firmware carries no sentinel,
    coordinator, idle landing or their preparation."""
    isas = ('Ia64',)

    def subjects(self, context):
        subjects = []
        for variant in Subjects.VARIANTS:
            if not context.has_variant(variant):
                continue
            expected = variant != 'firmware' or context.configuration.get(PROBE_OPTION) == '1'
            subjects.append(('%s objects' % variant, ('group', variant), expected))
            images = ('firmware:kernel_elf', 'firmware-test:kernel_elf') if variant == 'firmware' \
                else (Subjects.image_key(variant),)
            subjects += [(key, ('image', key), expected) for key in images if context.has_artifact(key)]
        return subjects

    def describe(self, subject):
        return subject[0]

    def extract(self, context, subject):
        label, (kind, key), expected = subject
        if kind == 'group':
            defined = set()
            for path in context.objects(key).values():
                defined |= {symbol.name for symbol in Elf.load(path).defined_symbols()}
        else:
            defined = {symbol.name for symbol in Elf.load(context.artifact(key)).defined_symbols()}
        return tuple(sorted(defined & set(PROBE_SYMBOLS))), expected

    def verify(self, facts):
        defined, expected = facts
        if expected:
            return ['the probe build lacks %s' % name for name in PROBE_SYMBOLS if name not in defined]
        return ['a firmware built without the probe contains %s' % name for name in defined]

    def mutants(self, facts):
        defined, expected = facts
        if expected:
            yield 'the probe coordinator missing', (tuple(name for name in defined if name != COORDINATOR), expected)
        else:
            yield 'the sentinel linked into production', ((SENTINEL,), expected)
            yield 'the probe dispatcher linked into production', ((PROBE_SYMBOLS[8],), expected)


# Final images -------------------------------------------------------------------------

@register
class Ia64FinalNativePreemptionTransport(DefiningProperty):
    """In the linked images (compile-check NucleusCore.elf, firmware
    AsteriaBoot.elf) the native-preemption transport is complete: the idle
    landing `AsteriaIa64BspNativePreemptionIdleLanding` (a descriptor target
    of the coordinator) is one defined FUNC with a non-zero size in an
    executable section, and `__gp` is defined with a non-zero value. The
    landing extent, sentinel window, external-preemption code and coordinator
    call graph of the images are the image subjects of
    Ia64BspNativePreemptionLanding, Ia64BspNativePreemptionSentinelWindow,
    the Ia64ExternalPreemption* properties and
    Ia64BspNativePreemptionCoordinator."""
    SYMBOL, OBJECTS = IDLE_LANDING, False

    def extract(self, context, subject):
        elf = Elf.load(subject.path)
        return tuple(Properties.symbol_facts(elf, IDLE_LANDING)), elf.definitions('__gp')

    def verify(self, facts):
        idle, pointers = facts
        problems = Properties.unique_code_definition_problems(list(idle), IDLE_LANDING)
        if not problems and not idle[0].symbol.size:
            problems.append('%s has size zero' % IDLE_LANDING)
        if len(pointers) != 1 or pointers[0].value == 0:
            problems.append('%d definitions of __gp (values %s), expected one non-zero'
                            % (len(pointers), [hex(entry.value) for entry in pointers]))
        return problems

    def mutants(self, facts):
        idle, pointers = facts
        entry = idle[0]
        yield 'missing idle landing', ((), pointers)
        for description, changes in (('idle landing as data', dict(type=STT_OBJECT)),
                                     ('empty idle landing', dict(size=0))):
            yield description, ((dataclasses.replace(entry, symbol=dataclasses.replace(entry.symbol, **changes)),)
                                + idle[1:], pointers)
        yield 'zero __gp', (idle, (dataclasses.replace(pointers[0], value=0),) + pointers[1:])
        yield 'missing __gp', (idle, ())
