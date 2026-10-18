# SPDX-License-Identifier: Apache-2.0
"""IA-64 application-processor runtime pivot (Source/Platform/Ia64/Memory/
Ia64ApplicationProcessorRuntimePivot.S).

`AsteriaIa64PivotApplicationProcessorRuntime(cpu, stack, bspstore)` is the
no-return physical-to-translated switch of an application processor. It
copies its three stacked inputs into unbanked static registers, rejects an
incompatible PSR (status 1), resolves the translated landing by relocatable IMM64,
switches to register bank 1, covers and flushes the inherited register
stack, installs a fresh translated backing store in architectural order,
makes no memory reference after the BSPSTORE cutoff, and rfi's into
`AsteriaIa64ApplicationProcessorTranslatedLanding`. The landing allocates a
one-output frame, calls `AsteriaIa64RunApplicationProcessorTranslatedKernel`
with the CPU value and parks if it ever returns.

Goldens (Tests/Golden/Ia64/Memory) check the exact instructions and stops.
Property checks verify register preservation, RSE ordering and the landing call.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import Ia64, Mutate, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code, writes_between
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import CodeProperty, SymbolContract

PIVOT = 'AsteriaIa64PivotApplicationProcessorRuntime'
LANDING = 'AsteriaIa64ApplicationProcessorTranslatedLanding'
CONTINUATION = 'AsteriaIa64RunApplicationProcessorTranslatedKernel'
# The P64 stacked inputs: CPU value, idle stack pointer, idle backing store.
CPU_INPUT, STACK_INPUT, BACKING_STORE_INPUT = INPUTS = ('r32', 'r33', 'r34')
# Unbanked static registers that may carry a value across bsw.1: not r0,
# not gp, sp or tp.
CARRIERS = frozenset('r%d' % n for n in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15))


@register
class Ia64ApPivotSymbols(SymbolContract):
    """The pivot and the translated landing are each defined once as
    functions internal to the image (binding LOCAL, or GLOBAL with HIDDEN
    visibility; never exported,
    and the IMM64 reference to the landing needs no dynamic symbol),
    bundle-aligned with a whole number of bundles, in a section whose flags
    are exactly ALLOC|EXECINSTR."""
    SYMBOLS = (PIVOT, LANDING)
    CONTRACT = Properties.FunctionContract(Properties.INTERNAL, alignment=16, size_multiple=16)


# The pivot -----------------------------------------------------------------------------

def _landing_address(steps) -> list[int]:
    return [index for index, step in enumerate(steps) if step.operation.target is not None
            and step.operation.target.kind == 'reloc' and step.operation.target.symbol == LANDING]


def pivot_carriers(steps, switch: int) -> dict[str, tuple[int, str]]:
    """Input register -> (step index, register) of its single copy before bsw.1."""
    carriers = {}
    for register in INPUTS:
        copies = [index for index in range(switch) if steps[index].operation.base == 'mov'
                  and steps[index].operation.reads == {register} and Ia64.single_write(steps[index].operation)]
        if len(copies) == 1:
            carriers[register] = (copies[0], Ia64.single_write(steps[copies[0]].operation))
    return carriers


def cpu_carrier(pivot: Code) -> str | None:
    """The register in which the pivot carries its CPU input across bsw.1."""
    switches = [index for index, step in enumerate(pivot.steps) if step.mnemonic == 'bsw.1']
    carriers = pivot_carriers(pivot.steps, switches[0]) if len(switches) == 1 else {}
    return carriers.get(CPU_INPUT, (0, None))[1]


@register
class Ia64ApPivotContract(CodeProperty):
    """Register and control-state contract of the application-processor pivot.
    Before `bsw.1` the inputs r32..r34 and the absolute landing
    address are held in unbanked static registers that nothing overwrites
    before bsw.1; after it no r16..r31 is read before being written, no
    stacked register is used after `cover`, sp comes from the carried r33,
    ar.bspstore from the carried r34, cr.iip from the landing address, and
    the carried CPU value (r32) survives to the landing. cover < flushrs
    < `mov ar.rsc=r0` < invala < `mov ar.bspstore=` < `mov ar.rnat=r0`, each
    in a later instruction group. No memory access between the BSPSTORE
    cutoff and rfi. cr.ifs, cr.ipsr and cr.iip are written once each,
    after the PSR gate, in groups before the one unpredicated rfi. The
    only relocation is IMM64 against the landing on the absolute
    `movl`."""
    SYMBOL = PIVOT

    def verify(self, code):
        steps = code.steps
        switches = [index for index, step in enumerate(steps) if step.mnemonic == 'bsw.1']
        if len(switches) != 1:
            return ['register preservation: %d bsw.1, expected one' % len(switches)]
        switch = switches[0]
        problems = []

        carriers = pivot_carriers(steps, switch)
        for register in INPUTS:
            if register not in carriers:
                problems.append('register preservation: %s is not copied exactly once before bsw.1' % register)
                continue
            index, carrier = carriers[register]
            if carrier not in CARRIERS:
                problems.append('register preservation: %s is carried in %s, which does not survive bsw.1' % (register, carrier))
            elif writes_between(steps, carrier, index, switch):
                problems.append('register preservation: %s, the carrier of %s, is overwritten before bsw.1' % (carrier, register))
        landings = _landing_address(steps)
        landing = None
        if len(landings) != 1 or landings[0] > switch:
            problems.append('register preservation: the landing address is not formed exactly once before bsw.1')
        else:
            landing = Ia64.single_write(steps[landings[0]].operation)
            if landing not in CARRIERS or writes_between(steps, landing, landings[0], switch):
                problems.append('register preservation: the landing address in %s does not survive bsw.1' % landing)
        written: set[str] = set()
        for step in steps[switch + 1:]:
            problems += ['register preservation: %s (#%d) reads %s after bsw.1 before writing it'
                         % (step.operation.text, step.position, name)
                         for name in sorted((step.operation.reads & Ia64.BANKED_GENERAL) - written)]
            written |= step.operation.writes & Ia64.BANKED_GENERAL
        covers = [index for index, step in enumerate(steps) if step.operation.base == 'cover']
        problems += ['register preservation: %s (#%d) uses a stacked register after cover' % (step.operation.text, step.position)
                     for step in (steps[covers[0] + 1:] if covers else ())
                     if (step.operation.reads | step.operation.writes) & Ia64.STACKED_GENERAL]

        def transported(destination: str, source: str | None, what: str) -> None:
            uses = [index for index, step in enumerate(steps)
                    if destination in step.operation.writes and index > switch]
            if len(uses) != 1:
                problems.append('register preservation: %s is written %d times after bsw.1, expected once' % (destination, len(uses)))
                return
            origin = next((index for index in range(uses[0] - 1, -1, -1)
                           if source is not None and source in steps[index].operation.writes), None)
            if (source is None or Ia64.single_read(steps[uses[0]].operation) != source or origin is None
                    or origin > switch):
                problems.append('register preservation: %s is not set from %s' % (destination, what))

        transported('r12', carriers.get(STACK_INPUT, (0, None))[1], 'the carried idle stack pointer (%s)' % STACK_INPUT)
        transported('ar.bspstore', carriers.get(BACKING_STORE_INPUT, (0, None))[1],
                    'the carried idle backing store (%s)' % BACKING_STORE_INPUT)
        transported('cr.iip', landing, 'the carried landing address')
        if CPU_INPUT in carriers:
            index, carrier = carriers[CPU_INPUT]
            if writes_between(steps, carrier, index, len(steps)):
                problems.append('register preservation: %s, the carried CPU value, is overwritten before the landing' % carrier)

        order = (('cover', lambda operation: operation.base == 'cover'),
                 ('flushrs', lambda operation: operation.base == 'flushrs'),
                 ('mov ar.rsc=r0', lambda operation: operation.writes == {'ar.rsc'} and operation.reads == {'r0'}),
                 ('invala', lambda operation: operation.base == 'invala'),
                 ('mov ar.bspstore=', lambda operation: operation.writes == {'ar.bspstore'}),
                 ('mov ar.rnat=r0', lambda operation: operation.writes == {'ar.rnat'} and operation.reads == {'r0'}))
        found = []
        for label, test in order:
            matches = [index for index, step in enumerate(steps) if test(step.operation)]
            if len(matches) != 1:
                problems.append('RSE ordering: %d %s, expected one' % (len(matches), label))
            else:
                found.append((label, matches[0]))
        problems += ['RSE ordering: %s is not in an instruction group after %s' % (second, first)
                     for (first, before), (second, after) in zip(found, found[1:])
                     if not (before < after and steps[before].group < steps[after].group)]

        returns = [index for index, step in enumerate(steps) if step.operation.base == 'rfi']
        cutoffs = [index for index, step in enumerate(steps) if 'ar.bspstore' in step.operation.writes]
        if len(returns) != 1:
            return problems + ['return state: %d rfi, expected one' % len(returns)]
        rfi = returns[0]
        problems += ['memory cutoff: %s (#%d) accesses memory after the BSPSTORE cutoff' % (step.operation.text, step.position)
                     for step in (steps[cutoffs[0] + 1:rfi] if cutoffs else ()) if step.operation.memory is not None]

        gates = [index for index, step in enumerate(steps) if step.operation.is_branch and step.operation.is_predicated]
        if steps[rfi].operation.is_predicated:
            problems.append('return state: rfi is predicated')
        for register in ('cr.ifs', 'cr.ipsr', 'cr.iip'):
            writes = [index for index, step in enumerate(steps) if register in step.operation.writes]
            if len(writes) != 1:
                problems.append('return state: %s is written %d times, expected once' % (register, len(writes)))
            elif not (gates and gates[0] < writes[0] < rfi and steps[writes[0]].group < steps[rfi].group):
                problems.append('return state: %s is not written after the PSR gate in a group before rfi' % register)

        if landing is not None:
            address = steps[landings[0]]
            expected = (RelocationFact(address.bundle + address.instruction.slot, 'IMM64', LANDING, 0),)
            if code.relocations != expected:
                problems.append('landing relocation: relocations %s, expected %s' % (list(code.relocations), list(expected)))
            if address.operation.base != 'movl' or address.operation.reads:
                problems.append('landing relocation: the landing address is not an absolute relocatable movl')
        elif code.relocations:
            problems.append('landing relocation: relocations %s without a landing address' % (list(code.relocations),))
        return problems

    def mutants(self, code):
        steps = code.steps
        switch = next(index for index, step in enumerate(steps) if step.mnemonic == 'bsw.1')
        carriers = pivot_carriers(steps, switch)
        cpu, stack = carriers[CPU_INPUT][1], carriers[STACK_INPUT][1]
        landing = steps[_landing_address(steps)[0]]
        cover = code.first(lambda step: step.operation.base == 'cover')
        invala = code.first(lambda step: step.operation.base == 'invala')
        rfi = code.first(lambda step: step.operation.base == 'rfi')

        def writer(register):
            return code.first(lambda step: register in step.operation.writes)

        unwritten = next(register for register in sorted(Ia64.BANKED_GENERAL, key=lambda name: int(name[1:]))
                         if all(register not in step.operation.writes for step in steps[switch + 1:]))
        yield 'CPU value carried in a banked register', code.rewrite(steps[carriers[CPU_INPUT][0]],
                                                                    'mov r18=%s' % CPU_INPUT)
        yield 'landing address in a banked register', code.rewrite(landing, landing.text.replace(
            Ia64.single_write(landing.operation) + '=', 'r18=', 1))
        yield 'banked register read after bsw.1', code.rewrite(writer('cr.ipsr'), 'mov cr.ipsr=%s' % unwritten)
        yield 'stacked register after cover', code.rewrite(code.free_slot(cover), 'mov r8=%s' % CPU_INPUT)
        yield 'sp from the CPU value', code.rewrite(writer('r12'), 'mov r12=%s' % cpu)
        yield 'backing store from the stack value', code.rewrite(writer('ar.bspstore'), 'mov.m ar.bspstore=%s' % stack)
        yield 'rfi to the stack value', code.rewrite(writer('cr.iip'), 'mov cr.iip=%s' % stack)
        yield 'CPU value overwritten after bsw.1', code.rewrite(code.free_slot(steps[switch]), 'mov %s=r0' % cpu)
        yield 'flushrs before cover', code.swap(cover, code.first(lambda step: step.operation.base == 'flushrs'))
        yield 'invala in the ar.rsc group', code.without_stops(writer('ar.rsc'), invala)
        yield 'invala removed', code.rewrite(invala, 'nop.m 0x0')
        yield 'eager register-stack mode', code.rewrite(writer('ar.rsc'), 'mov.m ar.rsc=%s' % cpu)
        after_cutoff = code.free_slot(writer('ar.bspstore'), 'nop.m')
        yield 'load after the BSPSTORE cutoff', code.rewrite(after_cutoff, 'ld8 r8=[r12]')
        yield 'store after the BSPSTORE cutoff', code.rewrite(after_cutoff, 'st8 [r12]=r0')
        yield 'cr.iip written in the rfi group', code.without_stops(writer('cr.iip'), rfi)
        yield 'cr.ifs not written', code.rewrite(writer('cr.ifs'), 'nop.m 0x0')
        yield 'predicated rfi', code.replace(rfi, predicate='p06')
        yield 'landing address not an absolute movl', code.rewrite(landing, 'mov r10=r1')
        yield 'landing relocation on another slot', dataclasses.replace(code, relocations=Mutate.replace_at(
            code.relocations, 0, offset=code.relocations[0].offset + 1))
        for description, relocations in Mutate.relocation_mutants(code.relocations, (PIVOT,)):
            yield 'landing relocation: %s' % description, dataclasses.replace(code, relocations=relocations)


# The translated landing -------------------------------------------------------------------------

@dataclasses.dataclass(frozen=True)
class LandingFacts:
    code: Code
    cpu_carrier: str | None     # the register in which the pivot carries its CPU input


@register
class Ia64ApTranslatedLanding(CodeProperty):
    """AsteriaIa64ApplicationProcessorTranslatedLanding builds the
    continuation's frame and argument and never returns: `alloc` of exactly
    one output (not clobbering the carried CPU value), then
    `mov r32=<carried CPU value>`, one unpredicated
    `br.call.sptk.many b0=AsteriaIa64RunApplicationProcessorTranslatedKernel`
    (the only relocation, PCREL21B in slot 2 of the call bundle), then a
    `hint.m 0` self-loop. A wrong frame or argument runs the AP kernel with
    garbage; a missing park falls through into unrelated code."""
    SYMBOL = LANDING

    def extract(self, context, subject):
        pivot = Subjects.code(context, context.defining_object(subject.variant, PIVOT), PIVOT)
        return LandingFacts(super().extract(context, subject), cpu_carrier(pivot))

    def verify(self, facts):
        code = facts.code
        steps = code.steps
        calls = [index for index, step in enumerate(steps) if step.operation.is_call]
        if len(calls) != 1:
            return ['%d calls, expected one' % len(calls)]
        index = calls[0]
        call = steps[index]
        problems = []
        allocs = [position for position, step in enumerate(steps) if step.operation.base == 'alloc']
        if allocs != [0]:
            problems.append('the landing does not begin with its only alloc')
        elif Ia64.alloc_frame(steps[0].operation) != (1, 0, 0):
            problems.append('%s is not a fresh frame with exactly one output' % steps[0].operation.text)
        arguments = [position for position in range(index) if 'r32' in steps[position].operation.writes]
        if (len(arguments) != 1 or steps[arguments[0]].operation.base != 'mov' or facts.cpu_carrier is None
                or Ia64.single_read(steps[arguments[0]].operation) != facts.cpu_carrier):
            problems.append('r32 is not set from the carried CPU value %s before the call' % facts.cpu_carrier)
        elif writes_between(steps, facts.cpu_carrier, -1, arguments[0]):
            problems.append('the carried CPU value %s is overwritten before it reaches r32' % facts.cpu_carrier)
        target = call.operation.target
        if (call.operation.is_predicated or call.mnemonic != 'br.call.sptk.many'
                or 'b0' not in call.operation.writes or target is None or target.kind == 'internal'
                or target.symbol != CONTINUATION or target.offset):
            problems.append('%s is not an unpredicated br.call.sptk.many b0=%s' % (call.operation.text, CONTINUATION))
        expected = (RelocationFact(call.bundle + 2, 'PCREL21B', CONTINUATION, 0),)
        if call.instruction.slot != 2 or code.relocations != expected:
            problems.append('relocations %s, expected only PCREL21B in slot 2 of the call bundle'
                            % (list(code.relocations),))
        park = steps[index + 1:]
        branches = [step for step in park if step.operation.is_branch]
        if not park or len(branches) != 1 or branches[0] is not park[-1]:
            return problems + ['the call is not followed by one park loop that ends the landing']
        loop = park[-1]
        if loop.operation.is_predicated or loop.target is None or not park[0].bundle <= loop.target <= loop.bundle:
            problems.append('%s does not branch back into the park loop' % loop.operation.text)
        return problems + ['%s (#%d) runs in the park loop' % (step.operation.text, step.position)
                           for step in park[:-1] if step.mnemonic != 'hint.m']

    def mutants(self, facts):
        code = facts.code
        alloc, loop = code.steps[0], code.steps[-1]
        call = code.first(lambda step: step.operation.is_call)
        argument = code.first(lambda step: 'r32' in step.operation.writes)
        hint = code.first(lambda step: step.mnemonic == 'hint.m')
        destination = Ia64.single_write(alloc.operation)

        def edited(changed: Code) -> LandingFacts:
            return dataclasses.replace(facts, code=changed)

        yield 'two-output frame', edited(code.rewrite(alloc, 'alloc %s=ar.pfs,2,0,0' % destination))
        yield 'frame with a local', edited(code.rewrite(alloc, 'alloc %s=ar.pfs,2,1,0' % destination))
        yield 'argument from the alloc result', edited(code.rewrite(argument, 'mov r32=%s' % destination))
        yield 'CPU value clobbered by the alloc', edited(code.rewrite(
            alloc, 'alloc %s=ar.pfs,1,0,0' % facts.cpu_carrier))
        yield 'predicated call', edited(code.replace(call, predicate='p06'))
        yield 'branch instead of call', edited(code.rewrite(call, call.text.replace(
            'br.call.sptk.many b0=', 'br.sptk.many ', 1)))
        yield 'park loop re-enters the landing', edited(code.retarget(loop, 0))
        yield 'return after the call', edited(code.rewrite(hint, 'br.ret.sptk.many b0'))
        yield 'work in the park loop', edited(code.rewrite(code.free_slot(hint), 'mov r8=r0'))
        yield 'continuation relocation in slot 1', edited(dataclasses.replace(code, relocations=Mutate.replace_at(
            code.relocations, 0, offset=code.relocations[0].offset - 1)))
        for description, relocations in Mutate.relocation_mutants(code.relocations, (LANDING,)):
            yield 'continuation relocation: %s' % description, edited(
                dataclasses.replace(code, relocations=relocations))
