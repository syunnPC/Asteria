# SPDX-License-Identifier: Apache-2.0
"""IA-64 native page-fault entry `AsteriaIa64PageFaultNativeEntry`
(Source/Platform/Ia64/Interrupt/Ia64PageFaultEntryContract.S).

The four heavyweight page-fault vectors of the runtime IVT branch here
directly, with the selector 0..3 in r16, on bank 0 with PSR.ic=0. The entry
first captures the interruption state, admits only native PL3, bank-one,
fully translated faults from the four vectors with a configured one-shot
control, claims that control with one `cmpxchg8.acq`, writes the
frame layout one store per instruction group, release-publishes Capturing
and parks; every rejection parks in a fail-stop pause loop.

The golden Tests/Golden/Ia64/Interrupt/AsteriaIa64PageFaultNativeEntry.dis
pins the entry exactly in the objects and in the linked images. The
properties run on O0, O2 and firmware objects, `NucleusCore.elf` O0/O2
and firmware `AsteriaBoot.elf`.
"""
from __future__ import annotations

from AsteriaVerify import Ia64StopViews

import collections
import dataclasses
import re

from AsteriaVerify import Ia64, Properties
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code, writes_between
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import CodeProperty, SymbolContract

ENTRY = 'AsteriaIa64PageFaultNativeEntry'
BANK_ZERO_REGISTERS = Ia64.BANKED_GENERAL | {'r0'}
# The selector and the interruption state the entry must capture first.
CAPTURED = ('r16', 'pr', 'psr', 'cr.iip', 'cr.ipsr', 'cr.ifs', 'cr.isr', 'cr.ifa', 'ar.ccv')
FREE_BANK_ZERO = BANK_ZERO_REGISTERS - {'r0', 'r16'}


def _form(pattern: str) -> re.Pattern:
    return re.compile('^%s$' % pattern.replace('GR', r'r\d+').replace('OP', r'(?:r\d+|-?(?:0x[0-9a-f]+|\d+))')
.replace('TARGET', r'<\+0x[0-9a-f]+>'))


# The instruction forms the stackless entry may use (the awk allowlist):
# moves from GRs, immediates, PSR, PR and the captured CRs/ARs; ALU on GRs;
# 8-byte (acquire) loads and (release) stores; the one CCV compare-exchange;
# mf; the pause hint; branches inside the entry.
UNPREDICATED_FORMS = tuple(_form(pattern) for pattern in (
    r'mov(?:\.m)? GR=(?:GR|-?(?:0x[0-9a-f]+|\d+)|psr|pr|cr\.(?:iip|ipsr|ifs|isr|ifa|itir|iipa|iim)|ar\.(?:ccv|k3))',
    r'mov\.m ar\.ccv=GR',
    r'movl GR=0x[0-9a-f]+',
    r'extr\.u GR=GR,\d+,\d+',
    r'(?:and|or|xor|adds|shladd) GR=OP(?:,OP){1,2}',
    r'cmp\.(?:eq|ltu) p\d+,p\d+=GR,GR',
    r'ld8(?:\.acq)? GR=\[GR\](?:,8)?',
    r'st8(?:\.rel)? \[GR\]=GR(?:,8)?',
    r'cmpxchg8\.acq GR=\[GR\],GR,ar\.ccv',
    r'mf',
    r'hint\.m 0',
    r'br\.few TARGET',
))
PREDICATED_FORMS = (_form(r'br\.cond\.(?:dpnt|sptk)\.few TARGET'),)


class _EntryProperty(Ia64StopViews.CoreCodeProperty):
    """Runs on the entry in the objects and in the linked images."""
    SYMBOL = ENTRY
    IMAGES = True


@register
class Ia64PageFaultEntrySymbol(SymbolContract):
    """The raw IVT target is exactly one GLOBAL HIDDEN function, defined,
    bundle-aligned with a non-zero whole number of bundles, in a section
    whose flags are exactly ALLOC|EXECINSTR (objects and linked images): the
    page-fault slots branch to it directly."""
    SYMBOLS = (ENTRY,)
    IMAGES = True


# Register domain, instruction allowlist and relocations --------------------------------------------

@register
class Ia64PageFaultEntryAllowlist(_EntryProperty):
    """The stackless entry touches only bank-0 r16..r31 (and r0), so the
    interrupted r1..r15, stacked, branch and floating-point registers stay
    intact; it uses only the allowlisted forms (moves from GRs, immediates,
    PSR, PR, cr.iip/ipsr/ifs/isr/ifa/itir/iipa/iim and ar.ccv/k3; ar.ccv
    writes; movl, extr.u, and/or/xor/adds/shladd, cmp.eq/ltu; ld8(.acq);
    st8(.rel); one `cmpxchg8.acq...,ar.ccv`; mf; `hint.m 0`; predicated
    `br.cond.{dpnt,sptk}.few` and `br.few` inside the entry), so it makes no
    call, rfi, RSE or PSR write; and it has no relocation."""

    def verify(self, code):
        problems = Properties.register_domain_problems(list(code.instructions), BANK_ZERO_REGISTERS)
        problems += ['%s (#%d) is not an allowed entry instruction' % (step.operation.text, step.position)
                     for step in code.steps if not Ia64StopViews.terminal(step) and not any(form.match(step.text) for form in (
                         PREDICATED_FORMS if step.instruction.predicate is not None else UNPREDICATED_FORMS))]
        return problems + ['relocation %s' % (fact,) for fact in code.relocations]

    def mutants(self, code):
        first, nop = code.steps[0], code.free_slot(kind='nop.m')
        loop = code.first(lambda step: step.mnemonic == 'br.few')
        reject = code.first(lambda step: step.instruction.predicate is not None)
        movl = code.first(lambda step: step.mnemonic == 'movl')
        yield 'capture into r15', code.rewrite(first, re.sub(r'^(\S+ )r\d+=', r'\1r15=', first.text))
        yield 'alloc inserted', code.rewrite(nop, 'alloc r16=ar.pfs,0,0,0')
        yield 'rfi instead of the pause', code.rewrite(code.first(lambda step: step.mnemonic == 'hint.m'), 'rfi')
        yield 'PSR write', code.rewrite(nop, 'mov psr.l=r16')
        yield 'control-register write', code.rewrite(nop, 'mov cr.ipsr=r16')
        yield 'floating-point register', code.rewrite(nop, 'setf.sig f6=r16')
        yield 'call', code.rewrite(loop, loop.text.replace('br.few ', 'br.call.sptk.many b0=', 1))
        yield 'branch out of the entry', code.retarget(reject, 'AsteriaIa64SystemCallEntry')
        yield 'predicated store', code.replace(code.first(lambda step: step.operation.memory == 'store'),
                                               predicate='p06')
        yield 'relocation', dataclasses.replace(code, relocations=code.relocations + (
            RelocationFact(movl.bundle + movl.instruction.slot, 'IMM64', ENTRY, 0),))


# Capture prefix ------------------------------------------------------------------------------------

@register
class Ia64PageFaultEntryCapturePrefix(_EntryProperty):
    """Before anything else the entry copies the selector r16 and the
    interruption state pr, psr, cr.iip, cr.ipsr, cr.ifs, cr.isr, cr.ifa and
    ar.ccv, each exactly once, into distinct free bank-0 registers (not
    r16), so no later instruction can clobber the tuple before it is
    captured. The exact registers and order are golden."""

    def verify(self, code):
        problems, sources, destinations = [], [], []
        for step in code.steps[:len(CAPTURED)]:
            operation = step.operation
            destination = Ia64.single_write(operation)
            if (operation.base != 'mov' or operation.is_predicated or destination is None
                    or len(operation.writes) != 1 or len(operation.reads) != 1):
                problems.append('%s (#%d) is not a capture move' % (operation.text, step.position))
                continue
            sources.append(next(iter(operation.reads)))
            destinations.append(destination)
        if collections.Counter(sources) != collections.Counter(CAPTURED):
            problems.append('the first %d instructions capture %s, expected each of %s once'
                            % (len(CAPTURED), ', '.join(sources), ', '.join(CAPTURED)))
        if len(set(destinations)) != len(destinations):
            problems.append('the captures share registers: %s' % ', '.join(destinations))
        return problems + ['a capture writes %s, not a free bank-0 register' % register
                           for register in destinations if register not in FREE_BANK_ZERO]

    def mutants(self, code):
        prefix = code.steps[:len(CAPTURED)]

        def capture(source):
            return next(step for step in prefix if step.operation.reads == {source})

        fault, predicates = capture('cr.ifa'), capture('pr')
        yield 'cr.ifa capture reads cr.iip', code.rewrite(fault, fault.text.replace('cr.ifa', 'cr.iip'))
        yield 'PR captured into the selector', code.rewrite(predicates, 'mov r16=pr')
        yield 'capture into an interrupted register', code.rewrite(predicates, 'mov r15=pr')
        yield 'captures share a register', code.rewrite(
            capture('cr.isr'), 'mov %s=cr.isr' % Ia64.single_write(fault.operation))
        yield 'late ar.ccv capture', code.swap(prefix[-1], code.steps[len(CAPTURED)])


# Acquire loads -----------------------------------------------------------------------------------

def _aliases_after(steps, register: str) -> list:
    """Walks `steps` tracking registers equal to `register` (copies by
    `mov`); returns each load whose base is such a register, before any
    post-increment changes it."""
    aliases, loads = {register}, []
    for step in steps:
        operation = step.operation
        if operation.memory == 'load' and operation.memory_base in aliases:
            loads.append(step)
        if operation.base == 'mov' and Ia64.single_read(operation) in aliases and len(operation.general_writes) == 1:
            aliases |= operation.general_writes
        else:
            aliases -= operation.general_writes
    return loads


def _dereferenced(steps, register: str) -> bool:
    aliases = {register}
    for step in steps:
        operation = step.operation
        if operation.memory_base in aliases:
            return True
        if (operation.base in ('adds', 'mov', 'shladd') and operation.reads & aliases
                and len(operation.general_writes) == 1):
            aliases |= operation.general_writes
        else:
            aliases -= operation.general_writes
        if not aliases:
            return False
    return False


def context_pointer_loads(steps) -> list:
    """Loads of a pointer published in the per-CPU context: a load from
    ar.k3 plus an immediate whose result the entry later dereferences."""
    derived: set[str] = set()
    loads = []
    for index, step in enumerate(steps):
        operation = step.operation
        written = operation.general_writes
        if operation.memory == 'load' and operation.memory_base in derived:
            destination = next(iter(written - {operation.memory_base}), None)
            if destination is not None and _dereferenced(steps[index + 1:], destination):
                loads.append(step)
        if operation.reads == {'ar.k3'}:
            derived |= written
        elif operation.base in ('adds', 'mov') and Ia64.single_read(operation) in derived and len(written) == 1:
            derived |= written
        else:
            derived -= written
    return loads


def acquire_sites(code: Code) -> dict[str, list]:
    """The loads that must be acquire loads, by role."""
    steps = code.steps
    claims = [index for index, step in enumerate(steps) if step.operation.memory == 'atomic']
    sites = {'context pointer': context_pointer_loads(steps), 'control word': [], 'frame word 0': []}
    if len(claims) != 1:
        return sites
    claim = claims[0]
    base = steps[claim].operation.memory_base
    definition = max((index for index in range(claim) if base in steps[index].operation.writes), default=-1)
    sites['control word'] = [step for step in steps[definition + 1:claim] if step.operation.memory == 'load'
                             and step.operation.memory_base == base and base not in step.operation.writes]
    stores = [index for index, step in enumerate(steps) if step.operation.memory == 'store' and index > claim]
    if stores:
        address = steps[stores[0]].operation.memory_base
        producer = max((index for index in range(stores[0]) if address in steps[index].operation.writes), default=None)
        frame = Ia64.single_read(steps[producer].operation) if producer is not None else None
        if frame is not None and steps[producer].operation.base in ('adds', 'mov'):
            origin = max((index for index in range(producer) if frame in steps[index].operation.writes), default=-1)
            sites['frame word 0'] = _aliases_after(steps[origin + 1:claim], frame)
    return sites


@register
class Ia64PageFaultEntryAcquireLoads(_EntryProperty):
    """The entry reads concurrently published words with acquire loads: the
    page-fault context pointer published in the per-CPU context (the load
    from ar.k3 plus an offset whose result it dereferences), the control
    word it later claims, and the publication word 0 of the frame it later
    fills; the ordinary field loads stay plain. The offsets are golden."""

    def verify(self, code):
        problems = []
        for role, loads in acquire_sites(code).items():
            if not loads:
                problems.append('no load of the %s' % role)
            problems += ['%s (#%d) reads the %s without acquire' % (step.operation.text, step.position, role)
                         for step in loads if 'acq' not in step.operation.completers]
        return problems

    def mutants(self, code):
        for role, loads in acquire_sites(code).items():
            yield '%s read without acquire' % role, code.rewrite(loads[0], loads[0].text.replace('.acq', '', 1))


# Store discipline ---------------------------------------------------------------------------------

@register
class Ia64PageFaultEntryStoreDiscipline(_EntryProperty):
    """Frame and control bytes are written only after the single
    `cmpxchg8.acq` ownership claim; the interrupted ar.ccv is restored right
    after the claim from its prefix capture; each store is its own
    instruction group; and Capturing is published by a final `st8.rel` to
    the claimed control word."""

    def verify(self, code):
        steps = code.steps
        claims = [index for index, step in enumerate(steps) if step.operation.memory == 'atomic']
        if len(claims) != 1:
            return ['%d atomic operations, expected the one cmpxchg claim' % len(claims)]
        index = claims[0]
        claim = steps[index]
        problems = []
        if not claim.mnemonic.startswith('cmpxchg') or 'acq' not in claim.operation.completers:
            problems.append('the claim %s is not a cmpxchg with acquire semantics' % claim.operation.text)
        stores = [position for position, step in enumerate(steps) if step.operation.memory == 'store']
        problems += ['%s (#%d) stores before the ownership claim' % (steps[position].operation.text,
                                                                      steps[position].position)
                     for position in stores if position < index]
        problems += ['%s (#%d) shares its instruction group with %s' % (
            steps[position].operation.text, steps[position].position, code.next(steps[position]).operation.text)
                     for position in stores if not code.ends_group(steps[position])]
        captures = [position for position, step in enumerate(steps) if step.operation.base == 'mov'
                    and step.operation.reads == {'ar.ccv'}]
        saved = Ia64.single_write(steps[captures[0]].operation) if captures else None
        restore = steps[index + 1] if index + 1 < len(steps) else None
        if (saved is None or restore is None or restore.operation.writes != {'ar.ccv'}
                or Ia64.single_read(restore.operation) != saved
                or writes_between(steps, saved, captures[0], index + 1)):
            problems.append('the interrupted ar.ccv is not restored from its capture right after the claim')
        if not stores:
            return problems + ['no store publishes the claimed control']
        last = steps[stores[-1]]
        base = claim.operation.memory_base
        if (last.mnemonic != 'st8.rel' or last.operation.memory_base != base
                or writes_between(steps, base, index, stores[-1])):
            problems.append('the last store %s is not a release store to the claimed control word'
                            % last.operation.text)
        return problems

    def mutants(self, code):
        claim = code.first(lambda step: step.operation.memory == 'atomic')
        stores = code.all(lambda step: step.operation.memory == 'store')
        base = claim.operation.memory_base
        value = sorted(claim.operation.general_reads - {base})[0]
        yield 'claim replaced by a plain store', code.rewrite(claim, 'st8 [%s]=%s' % (base, value))
        yield 'claim without acquire', code.rewrite(claim, claim.text.replace('.acq', '.rel', 1))
        yield 'ar.ccv restored from r0', code.rewrite(code.next(claim), 'mov.m ar.ccv=r0')
        yield 'plain store publishes the state', code.rewrite(stores[-1], stores[-1].text.replace('st8.rel', 'st8', 1))
        yield 'store before the claim', code.rewrite(code.free_slot(kind='nop.m'), 'st8 [%s]=r0' % base)
        yield 'unstopped frame store', code.without_boundary(stores[0])
        yield 'state published to the frame', code.rewrite(stores[-1], 'st8.rel [%s]=%s' % (
            stores[0].operation.memory_base, Ia64.single_read(stores[-1].operation) or 'r0'))


# Control flow ------------------------------------------------------------------------------------

@register
class Ia64PageFaultEntryControlFlow(_EntryProperty):
    """Every validation failure ends in the fail-stop pause loop, success
    parks in the checkpoint pause loop, and the entry never returns or
    escapes: all branches target the entry and end their instruction group;
    the predicated `br.cond.dpnt.few` rejections share one target, the
    terminal `hint.m 0; br.few` self-loop; one predicated forward skip
    (key permission) exists; exactly two unpredicated `br.few`, each a pause
    self-loop to the `hint.m 0` just before it: the checkpoint loop, then the
    reject loop, which ends the entry."""

    def verify(self, code):
        steps = code.steps
        problems = []
        rejects, skips, loops = [], [], []
        for position, step in enumerate(steps):
            operation = step.operation
            if not operation.is_branch:
                continue
            if Ia64StopViews.terminal(step):
                loops.append((position, code.previous(step).bundle))
                continue
            if step.target is None:
                problems.append('%s (#%d) leaves the entry' % (operation.text, step.position))
                continue
            following = steps[position + 1].group if position + 1 < len(steps) else None
            if following == step.group or following is None and not step.instruction.stop_after:
                problems.append('%s (#%d) does not end its instruction group' % (operation.text, step.position))
            if operation.is_predicated and step.mnemonic == 'br.cond.dpnt.few':
                rejects.append(step.target)
            elif operation.is_predicated:
                skips.append(step)
            elif step.mnemonic == 'br.few':
                loops.append((position, step.target))
            else:
                problems.append('%s (#%d) is not an entry branch form' % (operation.text, step.position))
        if len(set(rejects)) != 1:
            problems.append('the rejecting branches reach %d targets, expected one' % len(set(rejects)))
        if len(skips) != 1 or skips[0].target <= skips[0].bundle:
            problems.append('%d other predicated branches, expected one forward key-permission skip' % len(skips))
        if len(loops) != 2:
            return problems + ['%d unpredicated br.few, expected the checkpoint and reject loops' % len(loops)]
        for position, target in loops:
            previous = steps[position - 1] if position else None
            if previous is None or previous.mnemonic != 'hint.m' or target != previous.bundle:
                problems.append('%s (#%d) is not a pause self-loop' % (steps[position].operation.text,
                                                                       steps[position].position))
        (_, checkpoint), (reject_position, reject) = loops
        if rejects and (reject not in rejects[:1] or checkpoint in rejects):
            problems.append('the rejecting branches do not reach the terminal pause loop')
        if reject_position != len(steps) - 1:
            problems.append('the reject loop does not end the entry')
        return problems

    def mutants(self, code):
        reject = code.first(lambda step: step.mnemonic == 'br.cond.dpnt.few')
        skip = code.first(lambda step: step.mnemonic == 'br.cond.sptk.few')
        loops = code.all(lambda step: step.mnemonic == 'br.few' or Ia64StopViews.terminal(step))
        hints = code.all(lambda step: step.mnemonic == 'hint.m')
        yield 'rejection reaches the checkpoint loop', code.retarget(reject, hints[0].bundle)
        yield 'checkpoint loop moved to the reject loop', code.retarget(loops[0], hints[1].bundle)
        yield 'unstopped rejection', code.without_boundary(reject)
        yield 'rfi instead of the checkpoint pause', code.rewrite(hints[0], 'rfi')
        yield 'key-permission skip removed', code.rewrite(skip, 'nop.b 0x0')
        yield 'entry falls through', code.rewrite(loops[-1], 'nop.b 0x0')
        yield 'backward key-permission skip', code.retarget(skip, 0)
