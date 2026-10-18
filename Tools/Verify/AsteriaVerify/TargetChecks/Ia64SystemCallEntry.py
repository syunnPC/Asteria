# SPDX-License-Identifier: Apache-2.0
"""IA-64 system-call entry `AsteriaIa64SystemCallEntry`,
the hand-written `break 0x100000` handler of
Source/Platform/Ia64/System/Ia64SystemCallEntry.S that the runtime IVT break
slot branches to. With PSR.ic off it validates the per-CPU entry context
published through ar.k3, captures the interrupted user state NaT-safely,
covers the frame and switches the register stack; then it calls the P64
bridge `AsteriaIa64SystemCallBridge` with collection on, bank 1 and the kernel
GP, and restores every control register before `rfi`. The golden
Tests/Golden/Ia64/System/AsteriaIa64SystemCallEntry.dis pins the exact
instructions of the objects; these properties state the rules a reviewed
golden update must keep, on the objects and on the linked images that
contain the entry. Their mutants edit the decoded instructions.
"""
from __future__ import annotations

from AsteriaVerify import Ia64StopViews

import dataclasses
import re
from collections import Counter

from AsteriaVerify import Ia64, Layout, Mutate, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code, Step
from AsteriaVerify.Disassembly import Instruction, describe_address
from AsteriaVerify.Elf import SHN_ABS, load

ENTRY = 'AsteriaIa64SystemCallEntry'
BRIDGE = 'AsteriaIa64SystemCallBridge'
GLOBAL_POINTER = '__gp'
# The local leaf that reports a break with another immediate, and the C++
# reporter it calls.
UNKNOWN_BREAK = 'AsteriaIa64SystemCallUnknownBreak'
UNKNOWN_BREAK_REPORTER = 'AsteriaIa64FailStopUnknownSystemCallBreak'


def _is(base: str):
    return lambda step: step.operation.base == base


def _psr_ic(base: str):
    return lambda step: step.operation.base == base and any(value & Ia64.PSR_IC for value in step.operation.immediates)


def _is_call(step: Step) -> bool:
    return step.operation.is_call and 'b6' not in step.operation.writes


def _is_movl_r1(step: Step) -> bool:
    return step.operation.base == 'movl' and step.writes('r1')


def _is_release(step: Step) -> bool:
    return step.operation.memory == 'store' and 'rel' in step.operation.completers


def _is_acquire(step: Step) -> bool:
    return step.operation.memory == 'load' and 'acq' in step.operation.completers


def _is_unknown_break_gate(step: Step) -> bool:
    """The branch to the unknown-break leaf, outside the entry's extent."""
    target = step.operation.target
    return (step.operation.mnemonic == 'br.cond.dpnt.few' and target is not None and target.kind == 'symbol'
            and (target.symbol, target.offset) == (UNKNOWN_BREAK, 0))


def _is_rejection(step: Step) -> bool:
    return step.operation.mnemonic == 'br.cond.dpnt.few' and not _is_unknown_break_gate(step)


def _is_gate(step: Step) -> bool:
    return step.instruction.predicate is not None and _is_rejection(step)


class _EntryProperty(Ia64StopViews.CoreCodeProperty):
    SYMBOL = ENTRY
    IMAGES = True


@register
class Ia64SystemCallEntrySymbol(Subjects.SymbolContract):
    """The IVT break slot branches directly to the entry, so the
    entry is exactly one GLOBAL HIDDEN FUNC definition (never exported) with
    a non-zero size, bundle-aligned value and size, inside an ALLOC|EXECINSTR
    section. Checked in the objects and in the linked images."""
    SYMBOLS = (ENTRY,)
    IMAGES = True


@dataclasses.dataclass(frozen=True)
class RelocationSite:
    relocation: Properties.RelocationFact
    template: str | None            # Template of the relocated bundle
    instruction: Instruction | None  # The instruction in the relocated slot


@register
class Ia64SystemCallEntryRelocations(_EntryProperty):
    """The entry extent carries exactly three relocations, read from
    the ELF `.rela` sections: IMM64 → `__gp` in slot 1 of an MLX bundle whose
    instruction is an unpredicated `movl r1=` (so the bridge runs with the
    kernel GP), and PCREL21B → `AsteriaIa64SystemCallBridge` in slot 2 of an
    MIB bundle whose instruction is an unpredicated `br.call b0=` (so the
    entry calls exactly the bridge), and a PCREL21B branch to the complete
    user exception entry for rejected user state. The GP precedes the call.
    Checked in the objects and in the --emit-relocs firmware image."""
    # (type, target, slot, template, the instruction it feeds and its test)
    EXPECTED = (('IMM64', GLOBAL_POINTER, 1, 'MLX', 'movl r1=', lambda op: op.base == 'movl' and op.writes == {'r1'}),
                ('PCREL21B', BRIDGE, 2, 'MIB', 'br.call b0=', lambda op: op.is_call and 'b0' in op.writes),
                ('PCREL21B', 'AsteriaIa64UserExceptionEntryNormalized', 2, 'MIB', 'br', lambda op: op.is_branch and not op.is_call))

    def subjects(self, context):
        return Subjects.artifacts(context, ENTRY, images=True, image_filter=lambda elf: bool(elf.relocation_sections()))

    def extract(self, context, subject):
        code = super().extract(context, subject)
        by_address = {instruction.address: instruction for instruction in code.instructions}
        templates = {instruction.address & ~0xF: instruction.template for instruction in code.instructions}
        return tuple(RelocationSite(fact, templates.get((code.start + fact.offset) & ~0xF), by_address.get(
            ((code.start + fact.offset) & ~0xF) + 6 * (fact.offset & 0xF))) for fact in code.relocations)

    def verify(self, facts):
        kinds = Counter((site.relocation.type, site.relocation.target, site.relocation.addend) for site in facts)
        expected = Counter((kind, target, 0) for kind, target, *_ in self.EXPECTED)
        problems = ['unexpected relocation %s → %s%+#x' % key for key in sorted((kinds - expected).elements())]
        problems += ['missing relocation %s → %s%+#x' % key for key in sorted((expected - kinds).elements())]
        offsets = {}
        for kind, target, slot, template, form, feeds in self.EXPECTED:
            for site in facts:
                fact, instruction = site.relocation, site.instruction
                if (fact.type, fact.target) != (kind, target):
                    continue
                offsets[kind, target] = fact.offset
                label = '%s → %s at +%#x' % (kind, target, fact.offset)
                if fact.offset & 0xF != slot:
                    problems.append('%s is not in bundle slot %d' % (label, slot))
                if site.template != template:
                    problems.append('%s lies in a %s bundle, not %s' % (label, site.template, template))
                if instruction is None or instruction.predicate or not feeds(Ia64.parse(instruction)):
                    problems.append('%s does not feed an unpredicated %s (%s)'
                                    % (label, form, instruction.rendered() if instruction else 'no instruction'))
        if (('IMM64', GLOBAL_POINTER) in offsets and ('PCREL21B', BRIDGE) in offsets
                and offsets['IMM64', GLOBAL_POINTER] >= offsets['PCREL21B', BRIDGE]):
            problems.append('the __gp relocation does not precede the bridge call relocation')
        return problems

    def mutants(self, facts):
        gp, call = (next(index for index, site in enumerate(facts) if site.relocation.type == kind)
                    for kind in ('IMM64', 'PCREL21B'))

        def relocation(index, **changes):
            return Mutate.replace_at(facts, index, relocation=dataclasses.replace(facts[index].relocation, **changes))

        def instruction(index, **changes):
            return Mutate.replace_at(facts, index, instruction=dataclasses.replace(facts[index].instruction, **changes))

        yield 'IMM64 against the wrong symbol', relocation(gp, target='__wrong_gp')
        yield 'IMM64 feeding movl r2', instruction(gp, text=facts[gp].instruction.text.replace('movl r1=', 'movl r2='))
        yield 'IMM64 in bundle slot +3', relocation(gp, offset=(facts[gp].relocation.offset & ~0xF) + 3)
        yield 'PCREL21B against the wrong symbol', relocation(call, target='AsteriaIa64WrongSystemCallBridge')
        yield 'PCREL21B in bundle slot +3', relocation(call, offset=(facts[call].relocation.offset & ~0xF) + 3)
        yield 'PCREL21B feeding a predicated call', instruction(call, predicate='p06')
        yield 'IMM64 in a non-MLX bundle', Mutate.replace_at(facts, gp, template='MII')
        yield 'extra absolute relocation', facts + (RelocationSite(
            Properties.RelocationFact(0x10, 'DIR64LSB', 'AsteriaWrongTarget', 0), 'MII', None),)
        yield 'GP relocation after the call', relocation(gp, offset=(facts[call].relocation.offset & ~0xF) + 0x11)


def _spills(base: str, registers) -> list[str]:
    return ['st8.spill [%s]=r%d,8' % (base, number) for number in registers]


TNATS = ['tnat.z p7,p6=r%d' % number for number in (15, *range(32, 38))]
# IC-sensitive instructions allowed while PSR.ic is off (before `ssm 0x2000`
# and after `rsm 0x2000`), with their exact counts: context and frame
# accesses, the NaT-safe captures and the RSE control of the entry and tail.
IC_OFF_ALLOWLIST = Counter([
    'ld8.acq r30=[r17]', 'ld8 r29=[r24],8', 'ld8 r28=[r24],8', 'ld8 r27=[r24],8', 'ld8 r26=[r24],8', 'ld8 r25=[r24],8',
    'ld8 r17=[r24],8', 'ld8 r16=[r24],8', 'ld8 r24=[r24]', 'ld8 r16=[r17]', 'ld8.acq r16=[r29]', 'st8.rel [r29]=r16',
    'st8 [r17]=r20', 'st8 [r17]=r21', 'st8 [r17]=r22', 'st8 [r17]=r23', 'st8 [r17]=r31', 'st8 [r14]=r15',
    *_spills('r17', (1, 2, 3, 4, 5, 6, 7, 11, 12, 13, 14, *range(32, 38))), *_spills('r14', range(16, 32)), *TNATS,
    'mov.i r16=ar.pfs', 'mov.i ar.pfs=r22', 'mov.m r16=ar.rsc', 'mov.m ar.k6=r16', 'mov.m ar.rsc=r17', 'mov.m r16=ar.k6', 'mov.m r16=ar.k6', 'ld8 r16=[r17]', 'ld8 r16=[r17]', 'mov.i ar.pfs=r16', 'mov.i r18=ar.pfs', 'invala', 'mov.m ar.rsc=r0', 'mov.m r16=ar.bspstore',
    'mov.m r19=ar.rnat', 'mov.m ar.bspstore=r26', 'mov.m ar.rnat=r0', 'mov.m r15=ar.unat', 'cover', 'mov.m r18=ar.bsp',
    'mov.m ar.rsc=r22', 'mov.m ar.rsc=r23', 'alloc r22=ar.pfs,2,0,0', 'mov.m ar.unat=r20',
]) + Counter({'st8 [r17]=r16': 17, 'st8 [r17]=r18': 4, 'st8 [r17]=r19': 2, 'st8.spill [r17]=r15,8': 2,
              'mov.m r16=ar.unat': 2, 'mov.m ar.unat=r0': 3})
RSE_REGISTERS = frozenset({'ar.k6', 'ar.rsc', 'ar.bsp', 'ar.bspstore', 'ar.rnat', 'ar.pfs', 'ar.unat'})
IC_SENSITIVE_BASES = frozenset({'probe', 'alloc', 'cover', 'loadrs', 'flushrs', 'invala', 'tnat'})


def _accesses_memory(step: Step) -> bool:
    return step.operation.memory is not None or step.operation.base == 'probe'


@register
class Ia64SystemCallEntryIcOffAccesses(_EntryProperty):
    """PSR.ic is off at entry, set by the only `ssm` (exactly
    `ssm 0x2000`) and cleared by the only `rsm` (exactly `rsm 0x2000`); the
    ssm precedes the rsm, which precedes the only `rfi`, and no ssm/rsm
    touches another PSR bit. While collection is off, every memory access,
    register-stack operation, `tnat` and ar.rsc/bsp/bspstore/rnat/pfs/unat
    access is one of the reviewed forms of IC_OFF_ALLOWLIST with its exact
    count (a fault there is an unrecoverable nested interruption). No memory
    access follows the release store `st8.rel` that ends the frame accesses,
    none follows `rsm 0x2000` (fail-stop tail included), and `flushrs`
    never appears."""

    def verify(self, code):
        problems = ['%s changes PSR bits other than psr.ic' % step.describe() for step in code.steps
                    if step.operation.base in ('ssm', 'rsm') and step.operation.immediates != (Ia64.PSR_IC,)]
        enable = code.unique(_psr_ic('ssm'), 'psr.ic enables (ssm 0x2000)', problems)
        disable = code.unique(_psr_ic('rsm'), 'psr.ic disables (rsm 0x2000)', problems)
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        if not (enable and disable and rfi):
            return problems
        if not enable.position < disable.position < rfi.position:
            problems.append('psr.ic is not enabled, disabled and then left by rfi in this order')
        sensitive = Counter(step.rendered for step in code.window(before=enable) + code.window(after=disable, before=rfi) + code.window(after=code.first(_is('hint')))
                            if _accesses_memory(step) or step.operation.base in IC_SENSITIVE_BASES
                            or (step.operation.reads | step.operation.writes) & RSE_REGISTERS if not (step.operation.is_call and 'b6' in step.operation.writes))
        problems += Properties.multiset_problems(sensitive, IC_OFF_ALLOWLIST, 'IC-off access')
        problems += ['%s accesses memory after rsm 0x2000' % step.describe()
                     for step in code.window(after=disable, before=rfi) if _accesses_memory(step)]
        call = code.first(_is_call)
        release = call and code.first(lambda s: s is call, _is_release)
        if release is None:
            problems.append('no release store (st8.rel) after the bridge call')
        else:
            problems += ['%s accesses memory between the release store and rfi' % step.describe()
                         for step in code.window(after=release, before=rfi) if _accesses_memory(step)]
        return problems + ['%s: flushrs is forbidden' % step.describe() for step in code.all('flushrs')]

    def mutants(self, code):
        disable, call = code.site(_psr_ic('rsm')), code.site(_is_call)
        for description, anchor, text in (
                ('unclassified IC-off load after the context acquire', code.site(_is_acquire), 'ld8 r11=[r30]'),
                ('early unclassified alloc after the ar.k3 read', code.site(lambda s: 'ar.k3' in s.operation.reads),
                 'alloc r11=ar.pfs,0,0,0'),
                ('alloc in the IC-off fail-stop tail', code.site(_is('hint')), 'alloc r11=ar.pfs,0,0,0'),
                ('predicated load after rsm psr.ic', disable, '(p06) ld8 r11=[r12]'),
                ('flushrs after the bridge call', call, 'flushrs'),
                ('load between the release store and rfi', code.site(lambda s: s is call, _is_release),
                 'ld8 r11=[r7]')):
            yield description, code.insert(anchor, text, after=True)
        for register, name in (('ar.pfs', 'PFS'), ('ar.rsc', 'RSC'), ('ar.unat', 'UNAT')):
            final = code.site(lambda s: s.position > disable.position and s.writes(register))
            yield 'wrong %s final restoration source' % name, code.replace(final, text=re.sub(r'=r\d+$', '=r0',
                                                                                                final.text))
        yield 'psr.ic enable also enables psr.i', code.replace(code.site(_psr_ic('ssm')), text='ssm 0x6000')


PRE_COVER_BRANCHES = {'rejection (p6)': 34, 'rejection': 3, 'scan (p6)': 6, 'unknown break (p6)': 1, 'user fault (p6)': 5, 'CFM sample': 1}
PRE_COVER_SCAN_TARGETS = 3
PRE_COVER_COMPARES = Counter([
    'cmp.eq p6,p7=r17,r16', 'cmp.eq p6,p7=r26,r0', 'cmp.eq p6,p7=r28,r0', 'cmp.eq p6,p7=r29,r0', 'cmp.eq p6,p7=r30,r0',
    'cmp.eq p7,p6=r17,r0', 'cmp.eq p7,p6=r19,r16', 'cmp.eq p7,p6=r24,r0', 'cmp.ltu p6,p7=r16,r17',
    'cmp.ltu p6,p7=r16,r24', 'cmp.ltu p6,p7=r16,r25', 'cmp.ltu p6,p7=r16,r27', 'cmp.ltu p6,p7=r16,r30',
    'cmp.ltu p7,p6=r16,r24', 'cmp.ltu p7,p6=r17,r24', 'cmp.ltu p7,p6=r26,r24', 'cmp.ltu p7,p6=r26,r25',
    'cmp.ltu p7,p6=r26,r27', 'cmp.ltu p7,p6=r28,r24', 'cmp.ltu p7,p6=r28,r25', 'cmp.ltu p7,p6=r28,r27',
    'cmp.ltu p7,p6=r29,r24', 'cmp.ltu p7,p6=r29,r25', 'cmp.ltu p7,p6=r29,r27', 'cmp.ne.or p6,p7=r16,r0', *TNATS,
]) + Counter({'cmp.eq p6,p7=r17,r0': 2, 'cmp.eq p7,p6=r16,r0': 10, 'cmp.eq p7,p6=r16,r17': 3,
              'cmp.eq p7,p6=r17,r16': 2}) + Counter({'cmp.eq p7,p6=r16,r17': 1, 'cmp.eq p7,p6=r16,r0': 2, 'cmp.ltu p6,p7=r16,r17': 1, 'cmp.ltu p6,p7=r17,r16': 1})
# Writes of the static registers r1-r15 before cover: the bank-1 save pointer
# (r14, post-incremented by its 16 spills) and the bank-1 UNAT image (r15).
PRE_COVER_STATIC_WRITES = Counter(['mov r14=r28', 'mov.m r15=ar.unat', *_spills('r14', range(16, 32))])
PRE_COVER_ARCHITECTURAL = Counter([
    'mov r31=pr', 'mov r23=psr', 'mov r22=cr.iip', 'mov r21=cr.ipsr', 'mov r20=cr.isr', 'mov r19=cr.iim',
    'mov r18=cr.ifs', 'mov.m r17=ar.k3', 'mov.i r16=ar.pfs', 'mov.m r16=ar.rsc', 'mov.m ar.k6=r16', 'mov.m ar.rsc=r17', 'mov.m r16=ar.k6', 'mov.m r16=ar.k6', 'mov.i r18=ar.pfs', 'mov.i ar.pfs=r16', 'mov b6=r16', 'mov.i r16=ar.ec', 'mov.i r16=ar.lc',
    'mov.m r16=ar.ccv', 'mov.m r16=ar.fpsr', 'mov.m r15=ar.unat', *['mov r16=b%d' % number for number in range(8)],
    'bsw.0', 'bsw.1', 'cover',
]) + Counter({'mov.m r16=ar.unat': 2, 'mov.m ar.unat=r0': 3})
SYSTEM_BASES = frozenset({
    'alloc', 'break', 'bsw', 'cover', 'fc', 'flushrs', 'invala', 'itc', 'itr', 'loadrs', 'ptc', 'ptr', 'rfi',
    'rsm', 'rum', 'srlz', 'ssm', 'sum', 'sync', 'tak', 'thash', 'ttag', 'epc', 'clrrrb', 'vmsw', 'probe'})
CONTROL_BASES = frozenset({'br', 'brl', 'brp', 'chk', 'epc', 'clrrrb', 'vmsw'})
STACKED_INPUTS = frozenset('r%d' % number for number in range(32, 38))
STATIC_REGISTERS = frozenset('r%d' % number for number in range(1, 16))
ALLOWED_PREDICATED = re.compile(r'^(br\.cond\.(dpnt|sptk|spnt)\.few <\+0x[0-9a-f]+>|br\.cond\.dpnt\.few <%s>'
                                r'|adds r18=(1|2|4|8|16|32|64),r18)$' % UNKNOWN_BREAK)


def architectural(step: Step) -> bool:
    """Touches application, control, PSR, predicate-file or branch registers,
    or is a system, RSE or bank instruction."""
    return step.operation.base in SYSTEM_BASES or any(
        register.startswith(('ar.', 'cr.', 'psr')) or register in ('pr', 'pr.rot') or re.fullmatch(r'b[0-7]', register)
        for register in step.operation.reads | step.operation.writes)


def failure_target(code: Code) -> int | None:
    """The fail-stop offset F: the common target of all dpnt rejections."""
    targets = {step.target for step in code.all(_is_rejection)}
    return targets.pop() if len(targets) == 1 else None


@register
class Ia64SystemCallEntryAdmission(_EntryProperty):
    """Before the only `cover` (user state not yet captured):
    (a) the control transfers are exactly 34 `(p6) br.cond.dpnt.few F`, 3
    unpredicated `br.cond.dpnt.few F`, 6 forward `(p6) br.cond.sptk.few L`
    overlap scans to 3 distinct in-entry targets L ≠ F used twice each, each
    L resuming right after its scan's unpredicated rejection, and the one
    `(p6) br.cond.dpnt.few` to the unknown-break leaf;
    (b) the cmp/tnat instructions equal the reviewed multiset; (c) the writes
    of r1-r15 (destinations and post-increments) equal the reviewed multiset;
    (d) the architectural instructions (ar/cr/psr/pr/b registers, system,
    RSE and bank instructions) equal the reviewed multiset. Everywhere, only
    the rejection and scan branches and the NaT accumulation adds may be
    predicated, and only by p6 (e); no stacked input r32-r37 is touched before
    the State publication `st8.rel` (f)."""

    def verify(self, code):
        problems = ['%s may not be predicated' % step.describe() for step in code.steps
                    if step.instruction.predicate is not None
                    and not (step.predicate == 'p6' and ALLOWED_PREDICATED.match(step.text))]
        cover = code.unique(_is('cover'), 'cover', problems)
        if cover is None:
            return problems
        pre = code.window(before=cover) + [cover]       # The closing cover belongs to the capture
        fail_stop = failure_target(code)
        kinds, scans = Counter(), Counter()
        for step in (step for step in pre if step.operation.base in CONTROL_BASES):
            target, predicate = step.target, step.predicate
            if _is_rejection(step) and target is not None and target == fail_stop and predicate in (None, 'p6'):
                kinds['rejection (p6)' if predicate else 'rejection'] += 1
            elif step.operation.mnemonic == 'br.cond.spnt.few' and predicate == 'p6' and target is not None:
                kinds['user fault (p6)'] += 1
            elif step.operation.is_call and 'b6' in step.operation.writes and target is not None:
                kinds['CFM sample'] += 1
            elif _is_unknown_break_gate(step) and predicate == 'p6':
                kinds['unknown break (p6)'] += 1
            elif (step.operation.mnemonic == 'br.cond.sptk.few' and predicate == 'p6'
                  and target is not None and target != fail_stop and step.offset < target < cover.offset):
                kinds['scan (p6)'] += 1
                scans[target] += 1
            else:
                problems.append('%s is not an admitted pre-cover control transfer' % step.describe())
        if kinds != Counter(PRE_COVER_BRANCHES):
            problems.append('pre-cover branches %s, expected %s' % (dict(kinds), PRE_COVER_BRANCHES))
        if len(scans) != PRE_COVER_SCAN_TARGETS or any(count != 2 for count in scans.values()):
            problems.append('overlap scans reach %s, expected %d targets used twice each'
                            % ({hex(k): v for k, v in scans.items()}, PRE_COVER_SCAN_TARGETS))
        for target in scans:        # A scan's "disjoint" exit resumes right after its own unpredicated rejection
            landing = code.at_offset(target)
            before = landing and code.previous(landing)
            if not (before and _is_rejection(before) and before.instruction.predicate is None
                    and before.target == fail_stop):
                problems.append('the overlap scan target +%#x does not follow its unpredicated rejection' % target)
        for what, expected, selected in (
                ('pre-cover compare', PRE_COVER_COMPARES, lambda s: s.operation.base in ('cmp', 'tnat')),
                ('pre-cover static-register write', PRE_COVER_STATIC_WRITES,
                 lambda s: s.operation.writes & STATIC_REGISTERS),
                ('pre-cover architectural instruction', PRE_COVER_ARCHITECTURAL, architectural)):
            problems += Properties.multiset_problems(Counter(s.rendered for s in pre if selected(s) and not (s.operation.is_call and 'b6' in s.operation.writes)), expected, what)
        publication = code.first(_is_release)
        if publication is None or publication.position > cover.position:
            return problems + ['no State publication (st8.rel) before cover']
        return problems + ['%s uses a stacked input before the State publication' % step.describe()
                           for step in code.window(before=publication)
                           if (step.operation.reads | step.operation.writes) & STACKED_INPUTS]

    def mutants(self, code):
        def gate(step):
            return step.instruction.predicate is not None and step.operation.base == 'br'

        root = code.site(lambda s: 'ar.k3' in s.operation.reads)
        second_gate = code.site(lambda s: s is root, gate, gate)
        for description, text in (
                ('break', 'break.i 0x100000'), ('static r11 write', 'mov r11=r0'),
                ('branch register write', 'mov b0=r16'), ('ar.lc write', 'mov.i ar.lc=r16'),
                ('ar.fpsr write', 'mov.m ar.fpsr=r16'), ('status enable', 'ssm 0x6000'), ('cache flush', 'fc.i r16'),
                ('extra unpredicated rejection', 'br.cond.dpnt.few <+%#x>' % failure_target(code)),
                ('indirect branch', 'br.ia b0'), ('long call', 'brl.call.sptk.few b0=<+0x0>'),
                ('counted loop', 'br.wtop.sptk.few <+0x0>'), ('speculation check', 'chk.s.i r16,<+0x0>'),
                ('advanced-load check', 'chk.a.clr r16,<+0x0>'), ('branch predict', 'brp.loop.imp <+0x0>,<+0x0>'),
                ('enter privileged code', 'epc'), ('clear register rotation', 'clrrrb'),
                ('virtual machine switch', 'vmsw.0')):
            yield 'pre-capture side effect: %s' % description, code.insert(second_gate, text, after=True)
        yield 'predicated mandatory context acquire', code.replace(code.site(_is_acquire), predicate='p06')
        yield 'predicate overwrite before the ar.k3 null rejection', code.insert(
            code.site(lambda s: s is root, _is('cmp')), 'cmp.ne p6,p7=r0,r0', after=True)
        for description, path in (
                ('ar.k3 null gate', (lambda s: s is root, 'cmp.eq p6,p7=r17,r0')),
                ('Reserved-field gate', ('cmp.eq p7,p6=r24,r0',)), ('frame-extent gate', ('cmp.ltu p7,p6=r29,r24',)),
                ('ProcessorIndex gate', ('mov r24=4', 'cmp.ltu p7,p6=r16,r24')),
                ('root-extent compare', ('movl r16=0x1fffffffffffffd0', 'cmp.ltu p6,p7=r16,r17')),
                ('memory-stack extent', ('mov r17=16384', 'cmp.eq p7,p6=r16,r17')),
                ('RSE-corridor extent', ('mov r17=32768', 'cmp.eq p7,p6=r16,r17'))):
            yield 'missing %s' % description, code.nop(code.site(*path))
        yield 'stacked input read before the State publication', code.insert(code.site(_is_release), 'mov r16=r32')
        scan = code.site(lambda s: s.operation.mnemonic == 'br.cond.sptk.few')
        retargeted = code
        for branch in code.all(lambda s: s.operation.mnemonic == 'br.cond.sptk.few' and s.target == scan.target):
            retargeted = retargeted.replace(branch, text='br.cond.sptk.few <+%#x>' % (scan.target + 0x10))
        yield 'overlap scan resumes past its landing', retargeted


@register
class Ia64SystemCallEntryUserStateIsolation(_EntryProperty):
    """PL3 RSC/CFM rejection redirects to complete capture before
    touching stacked arguments. The local call/return samples CFM without
    allocating or accessing a stacked register, then restores PFS and b6.
    The rejection releases the syscall frame and restores UNAT/PR.
    """

    def extract(self, context, subject):
        code = super().extract(context, subject)
        elf = load(subject.path)
        alias = elf.symbol('AsteriaIa64UserExceptionEntryNormalized')
        if alias is not None and alias.section_index:
            # Linked disassembly names this NOTYPE interior entry through its
            # containing function. Resolve the exact ELF address, not a fixed
            # offset that could silently drift from the capture landing.
            rendered = '<%s>' % describe_address(elf, alias.section_index, alias.value)
            code = dataclasses.replace(code, instructions=tuple(dataclasses.replace(
                instruction, text=instruction.text.replace(rendered,
                    '<AsteriaIa64UserExceptionEntryNormalized>')) for instruction in code.instructions))
        return code

    def verify(self, code):
        problems = []
        sample = code.unique(lambda s: s.operation.is_call and 'b6' in s.operation.writes,
                             'local CFM samples', problems)
        inputs = code.first(lambda s: (s.operation.reads | s.operation.writes) & STACKED_INPUTS)
        cold = code.all(lambda s: s.operation.mnemonic == 'br.cond.spnt.few')
        if sample is None or inputs is None or len(cold) != 5:
            return problems + ['CFM sample, stacked capture and five user-state rejection gates are required']
        landing = code.target(sample)
        if landing is None or landing.text != 'br.ret.sptk.many b6' or landing.predicate is not None:
            problems.append('CFM sample does not immediately return through its own b6 link')
        if sample.predicate is not None or not sample.position < inputs.position:
            problems.append('CFM is not sampled unconditionally before touching arguments')
        restored = _chain(code, ('mov.i r18=ar.pfs', 'mov.i ar.pfs=r16', 'mov b6=r16'),
                          problems, 'sample restoration', after=sample, before=inputs)
        guards = ('cmp.eq p7,p6=r16,r0', 'cmp.eq p7,p6=r16,r0',
                  'cmp.ltu p6,p7=r17,r16', 'cmp.ltu p6,p7=r16,r17')
        for gate, expected in zip(cold[1:], guards):
            previous = code.previous(gate)
            if previous is None or previous.text != expected or gate.predicate != 'p6' or gate.position >= inputs.position:
                problems.append('a CFM guard does not precede the argument capture')
        preflight = code.window(after=sample, before=inputs)
        for text in ('extr.u r16=r18,7,7', 'extr.u r16=r18,14,24', 'and r16=127,r18', 'mov r17=96', 'mov r17=6'):
            if sum(step.text == text for step in preflight) != 1:
                problems.append('missing CFM admission operand ' + text)
        prefix = ('mov.m r16=ar.rsc', 'mov.m ar.k6=r16', 'mov r17=12', 'mov.m ar.rsc=r17')
        if tuple(step.rendered for step in code.steps[:4]) != prefix:
            problems.append('RSC is not normalized before the stop poll and admission')
        rsc = code.first('mov.m r16=ar.k6')
        root = code.first('mov.m r17=ar.k3')
        if rsc is None or root is None or not rsc.position < cold[0].position < root.position:
            problems.append('RSC is not rejected before context/stack accesses')
        for text in ('mov r17=12', 'cmp.eq p7,p6=r16,r17'):
            if not any(step.text == text for step in code.window(after=rsc, before=cold[0])):
                problems.append('missing enforced-lazy RSC admission operand ' + text)
        fault = code.target(cold[0])
        if fault is None:
            return problems + ['no complete-state redirect']
        redirect = code.window(after=fault, before=code.first(_is('hint')))
        expected = ('mov.m ar.k4=44', 'mov r16=6')
        if fault.text != 'mov pr=r31,0xfffffffffffffffe':
            problems.append('fault redirect does not restore user PR')
        for text in expected:
            if not any(step.text == text for step in redirect):
                problems.append('missing complete-state redirect operand ' + text)
        branches = [step for step in redirect if step.operation.is_branch]
        if len(branches) != 1 or branches[0].predicate is not None or branches[0].operation.target is None or (
                branches[0].operation.target.symbol, branches[0].operation.target.offset) != ('AsteriaIa64UserExceptionEntryNormalized', 0):
            problems.append('user-state rejection does not redirect to complete capture')
        cfm_fault = code.target(cold[1])
        if cfm_fault is None or any(gate.target != cold[1].target for gate in cold[1:]):
            problems.append('CFM rejections do not share the original-state restoration')
        else:
            tail = code.window(after=cfm_fault, before=fault)
            if not any(step.text == 'mov.m ar.unat=r16' for step in tail) or not any(
                    step.text == 'st8.rel [r29]=r0' for step in tail):
                problems.append('CFM rejection does not restore UNAT and release its syscall frame')
        return problems

    def mutants(self, code):
        sample = code.site(lambda s: s.operation.is_call and 'b6' in s.operation.writes)
        yield 'missing CFM sample', code.nop(sample)
        yield 'CFM sample returns through user b0', code.replace(code.target(sample), text='br.ret.sptk.many b0')
        for gate in code.all(lambda s: s.operation.mnemonic == 'br.cond.spnt.few'):
            yield 'missing user-state rejection at %#x' % gate.offset, code.nop(gate)
        for text in ('mov r17=6', 'mov r17=96', 'mov.i ar.pfs=r16', 'mov b6=r16', 'st8.rel [r29]=r0'):
            yield 'missing admission/restoration ' + text, code.nop(code.site(text))


@register
class Ia64SystemCallEntryFailStop(_EntryProperty):
    """Every `br.cond.dpnt.few` rejection, predicated or not, targets
    the same fail-stop F inside the entry (the unknown-break branch
    is not a rejection); F lies after the only `rfi`, the
    first instruction at F is `hint.m 0`, and the next is the only
    `br.few F`, an unpredicated self-loop. A misdirected rejection would
    resume unvalidated state instead of stopping."""

    def verify(self, code):
        targets = Counter(step.target for step in code.all(_is_rejection))
        if not targets:
            return ['no br.cond.dpnt.few rejection']
        if len(targets) != 1 or None in targets:
            return ['rejections target %s, expected one fail-stop inside the entry'
                    % ', '.join('%s ×%d' % ('outside' if t is None else '+%#x' % t, n) for t, n in targets.items())]
        fail_stop = next(iter(targets))
        hint = code.at_offset(fail_stop)
        if hint is None or hint.text != 'hint.m 0' or hint.instruction.predicate is not None:
            return ['the fail-stop at +%#x does not start with hint.m 0' % fail_stop]
        problems = []
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        if rfi is not None and hint.position < rfi.position:
            problems.append('the fail-stop at +%#x precedes rfi' % fail_stop)
        loop = code.next(hint)
        if (loop is None or not Ia64StopViews.terminal(loop)
                or loop.instruction.predicate is not None):
            problems.append('hint.m 0 at the fail-stop is not followed by br.few to itself')
        loops = code.all(lambda s: Ia64StopViews.terminal(s))
        return problems + (['%d br.few loops to the fail-stop, expected one' % len(loops)] if len(loops) != 1 else [])

    def mutants(self, code):
        for description, path in (
                ('context-alignment', (_is_acquire, 'cmp.eq p7,p6=r16,r0', _is_gate)),
                ('bridge-context', ('cmp.ltu p7,p6=r17,r24', _is_gate)),
                ('context-extent', ('movl r16=0x1fffffffffffffc0', 'cmp.ltu p6,p7=r16,r30', _is_gate)),
                ('overlap', ('cmp.ltu p7,p6=r26,r27', lambda s: s.instruction.predicate is None and _is_rejection(s)))):
            yield 'misdirected %s rejection' % description, code.replace(code.site(*path),
                                                                         text='br.cond.dpnt.few <+0x0>')
        yield 'fail-stop loop escapes', code.replace(code.site(lambda s: Ia64StopViews.terminal(s)),
                                                     text='br.few <+0x0>')
        yield 'fail-stop without its pause', code.nop(code.site(_is('hint')))


@register
class Ia64SystemCallEntryBridgeCall(_EntryProperty):
    """The entry has exactly one compiled bridge call: unpredicated `br.call b0=` to
    `AsteriaIa64SystemCallBridge` (by relocation in objects, by symbol in
    linked images), after `adds r12=-16,r27` < `alloc r22=ar.pfs,2,0,0` (the
    only alloc since the stack switch and the only one of that form) <
    `mov r4=r29` < `mov r32=r4` < `mov r33=r30` < `ssm 0x2000` < `bsw.1` <
    `srlz.d` < the only, unpredicated `movl r1=`; between `mov r33=r30` and
    the call there is exactly one ssm, one bsw (bsw.1) and one srlz.d. The
    bridge context flows from `ld8 r17=[r24],8` through `mov r30=r17` (the
    only one) to `mov r33=r30`, and the frame from `ld8 r29=[r24],8` through
    `mov r4=r29`, without intervening writes; r4, r32, r33 and r1 are not
    written again before the call."""
    ORDER = ('adds r12=-16,r27', 'alloc r22=ar.pfs,2,0,0', 'mov r4=r29', 'mov r32=r4', 'mov r33=r30')

    def verify(self, code):
        problems = []
        call = code.unique(_is_call, 'calls', problems)
        if call is None:
            return problems
        if call.instruction.predicate is not None:
            problems.append('the bridge call is predicated')
        target = call.operation.target
        if target is None or (target.symbol, target.offset) != (BRIDGE, 0) or 'b0' not in call.operation.writes:
            problems.append('%s is not br.call b0= to %s' % (call.describe(), BRIDGE))
        chain = [code.unique(text, text, problems) for text in self.ORDER]
        gp = code.unique(_is_movl_r1, 'movl r1= loads', problems)
        if None in chain or gp is None:
            return problems
        stack, frame_alloc, frame, frame_argument, context_argument = chain
        ssm, bsw = (code.all(_is(base), after=context_argument, before=call) for base in ('ssm', 'bsw'))
        if len(ssm) != 1 or ssm[0].text != 'ssm 0x2000':
            problems.append('%d ssm before the call, expected exactly ssm 0x2000' % len(ssm))
        if len(bsw) != 1 or bsw[0].text != 'bsw.1':
            problems.append('bank switches before the call are %s, expected exactly bsw.1'
                            % [step.text for step in bsw])
        if len(ssm) == 1:
            serialize = [step for step in code.window(after=ssm[0], before=call) if step.text == 'srlz.d']
            if len(serialize) != 1:
                problems.append('%d srlz.d between ssm and the call, expected one' % len(serialize))
            elif len(bsw) == 1:
                chain = chain + [ssm[0], bsw[0], serialize[0]]
        chain += [gp, call]
        if len(chain) == 10 and [step.position for step in chain] != sorted(step.position for step in chain):
            problems.append('the bridge transition is out of order: %s' % ' < '.join(step.text for step in chain))
        allocs = code.all(_is('alloc'), after=stack, before=call)
        if allocs != [frame_alloc]:
            problems.append('allocs between the stack switch and the call: %s' % [step.text for step in allocs])
        if gp.instruction.predicate is not None:
            problems.append('the GP load is predicated')
        load = code.unique('ld8 r17=[r24],8', 'bridge-context loads', problems)
        preserve = code.unique('mov r30=r17', 'bridge-context preservations', problems)
        for register, start, end in (('r17', load, preserve), ('r30', preserve, context_argument),
                                     ('r29', code.first('ld8 r29=[r24],8'), frame), ('r4', frame, call),
                                     ('r32', frame_argument, call), ('r33', context_argument, call), ('r1', gp, call)):
            if start is None or end is None:
                continue
            if start.position > end.position:
                problems.append('%s is consumed before it is produced' % register)
            problems += ['%s writes %s between %s and %s' % (step.describe(), register, start.text, end.text)
                         for step in code.window(after=start, before=end) if step.writes(register)]
        return problems

    def mutants(self, code):
        preserve, context_argument = code.site('mov r30=r17'), code.site('mov r33=r30')
        call, gp, frame_argument = code.site(_is_call), code.site(_is_movl_r1), code.site('mov r32=r4')
        yield 'bridge-context r17 clobber', code.insert(code.site('cmp.ltu p7,p6=r17,r24'), 'mov r17=r0', after=True)
        yield 'wrong bridge-context preservation source', code.replace(preserve, text='mov r30=r16')
        yield 'missing bridge-context preservation', code.nop(preserve)
        yield 'missing bridge-context argument', code.replace(context_argument, text='mov r33=r0')
        yield 'frame r29 clobber', code.insert(code.site('ld8 r29=[r24],8'), 'mov r29=r0', after=True)
        yield 'wrong frame preservation source', code.replace(code.site('mov r4=r29'), text='mov r4=r0')
        yield 'frame argument r32 clobber', code.insert(frame_argument, 'mov r32=r0', after=True)
        yield 'r4 post-increment before the bridge', code.insert(frame_argument, 'ld8 r11=[r4],8', after=True)
        yield 'r32 post-increment before the bridge', code.insert(frame_argument, 'ld8 r11=[r32],8', after=True)
        yield 'wrong P64 output arity', code.replace(code.site('alloc r22=ar.pfs,2,0,0'), text='alloc r22=ar.pfs,1,0,0')
        yield 'later alloc before the bridge', code.insert(context_argument, 'alloc r11=ar.pfs,0,0,0', after=True)
        yield 'missing bridge bank switch', code.replace(code.site(lambda s: s is context_argument, 'bsw.1'),
                                                         text='nop.b 0x0')
        yield 'r1 clobber before the call', code.insert(gp, 'mov r1=r0', after=True)
        yield 'predicated GP load', code.replace(gp, predicate='p06')
        yield 'predicated bridge call', code.replace(call, predicate='p06')
        yield 'call to another function', code.replace(call, text=call.text.replace(BRIDGE, 'AsteriaWrongBridge'))
        yield 'second call', code.insert(call, call.text, after=True)
        ssm = code.site(lambda s: s is context_argument, 'ssm 0x2000')
        yield 'srlz.d moved before the ssm', code.nop(code.site(lambda s: s is ssm, 'srlz.d')).insert(ssm, 'srlz.d')


NAT_REGISTERS = ('r15', 'r32', 'r33', 'r34', 'r35', 'r36', 'r37')
NAT_BITS = (1, 2, 4, 8, 16, 32, 64)


def _nat_accumulation(step: Step) -> bool:
    return re.fullmatch(r'adds r18=\d+,r18', step.text) is not None


@register
class Ia64SystemCallEntryNatCapture(_EntryProperty):
    """After `mov r16=b7`, one `mov r18=r0` starts the NaT bitmap;
    for k = 1..7 over (r15, r32..r37), `tnat.z p7,p6=R[k]` is followed by
    `(p6) adds r18=2^(k-1),r18`, strictly interleaved and in order (the only
    tnat instructions of the entry). Then the only `adds r17=320,r29`, the
    spills `st8.spill [r17]=R[k],8` in the same order and `st8 [r17]=r18`,
    all before `cover`. R[k] is not written between its tnat and its spill,
    and r18 only by the seven accumulations until the bitmap store: the
    user's stacked inputs and their NaT bits survive the capture."""

    def verify(self, code):
        problems = []
        saved = code.unique('mov r16=b7', 'branch-register b7 saves', problems)
        cover = code.unique(_is('cover'), 'cover', problems)
        base = code.unique('adds r17=320,r29', 'NaT spill bases', problems)
        if None in (saved, cover, base):
            return problems
        tnats = code.all(_is('tnat'))
        if [step.text for step in tnats] != ['tnat.z p7,p6=%s' % register for register in NAT_REGISTERS]:
            return problems + ['the tnat sequence is %s' % [step.text for step in tnats]]
        start = code.first(lambda s: s is saved, lambda s: s.text == 'mov r18=r0' and s.position < tnats[0].position)
        if start is None:
            return problems + ['no mov r18=r0 between the b7 save and the first tnat']
        store = code.first(lambda s: s is base, 'st8 [r17]=r18')
        if store is None or store.position > cover.position:
            return problems + ['no NaT bitmap store st8 [r17]=r18 before cover']
        spills = code.all(lambda s: s.operation.base == 'st8' and 'spill' in s.operation.completers
                          and s.operation.memory_base == 'r17', after=base, before=store)
        if [step.text for step in spills] != ['st8.spill [r17]=%s,8' % register for register in NAT_REGISTERS]:
            return problems + ['the NaT-capture spills are %s' % [step.text for step in spills]]
        adds = code.all(_nat_accumulation, after=start, before=store)
        if len(adds) != len(NAT_REGISTERS):
            return problems + ['%d NaT accumulations, expected %d' % (len(adds), len(NAT_REGISTERS))]
        for index, (register, bit, add, tnat, spill) in enumerate(zip(NAT_REGISTERS, NAT_BITS, adds, tnats, spills)):
            following = tnats[index + 1] if index + 1 < len(tnats) else base
            if add.text != 'adds r18=%d,r18' % bit or add.predicate != 'p6':
                problems.append('%s does not accumulate bit %d of %s under p6' % (add.describe(), bit, register))
            if not tnat.position < add.position < following.position:
                problems.append('the accumulation of %s is not between its tnat and the next capture step'
                                % register)
            problems += ['%s writes %s between its tnat and its spill' % (step.describe(), register)
                         for step in code.window(after=tnat, before=spill) if step.writes(register)]
        if not start.position < tnats[0].position:
            problems.append('the NaT bitmap is not initialized before the first tnat')
        return problems + ['%s writes the NaT bitmap r18' % step.describe() for step in
                           code.window(after=start, before=store) if step.writes('r18') and step not in adds]

    def mutants(self, code):
        tnats = code.all(_is('tnat'))
        adds = [code.site(lambda s: s is tnat, _nat_accumulation) for tnat in tnats]
        for tnat, add, register, bit in zip(tnats, adds, NAT_REGISTERS, NAT_BITS):
            yield '%s clobber between tnat and spill' % register, code.insert(tnat, 'mov %s=r0' % register, after=True)
            yield 'missing NaT bit %d accumulation' % bit, code.nop(add)
            yield 'corrupted NaT bit %d accumulation' % bit, code.replace(
                add, text='adds r18=%d,r18' % (1 if bit == 64 else bit * 2))
        yield 'NaT bitmap reset before its store', code.insert(adds[-1], 'mov r18=r0', after=True)
        yield 'tnat order swapped', code.swap(tnats[0], tnats[1])
        yield 'last accumulation after the spill base', code.nop(adds[-1]).insert(
            code.site('adds r17=320,r29'), adds[-1].rendered, after=True)


CFM_GATES = 5


@register
class Ia64SystemCallEntryCoveredIfs(_EntryProperty):
    """The only `cover` is followed by the only `mov r18=cr.ifs`
    after it, then `adds r17=56,r29` and `st8 [r17]=r18`, with no write to
    r17 or r18 in between: the frame records the IFS produced by cover,
    which rfi later consumes. r18 is not written from that store to the last
    of the five CFM validation gates (predicated rejections before the RSE
    switch's `invala`); their exact text is pinned by the golden."""

    def verify(self, code):
        problems = []
        cover = code.unique(_is('cover'), 'cover', problems)
        read = cover and code.unique('mov r18=cr.ifs', 'covered IFS reads after cover', problems, after=cover)
        if read is None:
            return problems
        address = code.first(lambda s: s is read, 'adds r17=56,r29')
        store = address and code.first(lambda s: s is address, 'st8 [r17]=r18')
        if store is None:
            return problems + ['the covered IFS is not stored through adds r17=56,r29; st8 [r17]=r18']
        problems += ['%s writes %s before the covered IFS is stored' % (step.describe(), register)
                     for register, after in (('r18', read), ('r17', address))
                     for step in code.window(after=after, before=store) if step.writes(register)]
        gates = code.all(_is_gate, after=store, before=code.first(lambda s: s is store, 'invala'))
        if len(gates) != CFM_GATES:
            return problems + ['%d CFM validation gates after the IFS store, expected %d' % (len(gates), CFM_GATES)]
        return problems + ['%s writes r18 before the CFM validation completes' % step.describe()
                           for step in code.window(after=store, before=gates[-1]) if step.writes('r18')]

    def mutants(self, code):
        read = code.site(_is('cover'), 'mov r18=cr.ifs')
        address = code.site(lambda s: s is read, 'adds r17=56,r29')
        yield 'missing covered-IFS capture', code.replace(read, text='mov r18=r0')
        yield 'covered-IFS value clobber before its store', code.insert(address, 'mov r18=r0', after=True)
        yield 'covered-IFS address clobber before its store', code.insert(address, 'mov r17=r0', after=True)
        yield 'covered-IFS clobber before validation', code.insert(
            code.site(lambda s: s is address, 'st8 [r17]=r18'), 'mov r18=r0', after=True)
        yield 'second cr.ifs read after cover', code.insert(read, read.text, after=True)


SAVE_ORDER = ('mov.i r16=ar.pfs', 'mov.m r16=ar.k6', 'mov r16=b7')
SWITCH_ORDER = ('invala', 'mov.m ar.rsc=r0', 'mov.m r16=ar.bspstore', 'mov.m r19=ar.rnat', 'mov.m ar.bspstore=r26',
                'mov.m ar.rnat=r0', 'mov.m r18=ar.bsp', 'mov r22=3', 'mov.m ar.rsc=r22', 'adds r12=-16,r27')
RESTORE_ORDER = ('mov.m ar.rsc=r16', 'loadrs', 'mov.m ar.bspstore=r17', 'mov.m ar.rnat=r18')


def _chain(code: Code, texts, problems: list[str], what: str, **window) -> list[Step] | None:
    """The unique steps of `texts`, reporting a missing or out-of-order one."""
    steps = []
    for text in texts:
        # RSC admission precedes the PFS capture; this chain checks the
        # later saved original RSC, not that separate admission read.
        candidates = code.all(text, **window)
        if text == 'mov.m r16=ar.k6' and len(candidates) == 2:
            steps.append(candidates[-1])
        else:
            steps.append(code.unique(text, '%s step %s' % (what, text), problems, **window))
    if None in steps:
        return None
    if [step.position for step in steps] != sorted(step.position for step in steps):
        problems.append('the %s is out of order: %s' % (what, ' < '.join(
            step.text for step in sorted(steps, key=lambda s: s.position))))
    return steps


@register
class Ia64SystemCallEntryRseSwitch(_EntryProperty):
    """`mov.i r16=ar.pfs` < `mov.m r16=ar.rsc` < `mov r16=b7` (each
    exactly once). After the covered-IFS validation (its fifth gate) and
    before the call, the register stack switches in the architected order,
    each step exactly once: `invala` < `mov.m ar.rsc=r0` < BSPSTORE and RNAT
    reads < `mov.m ar.bspstore=r26` < `mov.m ar.rnat=r0` <
    `mov.m r18=ar.bsp` < `mov r22=3` < `mov.m ar.rsc=r22` <
    `adds r12=-16,r27`. After the call: `mov.m ar.rsc=r16` < `loadrs` <
    `mov.m ar.bspstore=r17` < `mov.m ar.rnat=r18` < the release `st8.rel` <
    `rsm 0x2000` < `rfi`, each exactly once. `flushrs` never appears."""

    def verify(self, code):
        problems = []
        call = code.unique(_is_call, 'calls', problems)
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        if call is None or rfi is None:
            return problems
        saves = _chain(code, SAVE_ORDER, problems, 'user RSE save')
        switch = _chain(code, SWITCH_ORDER, problems, 'RSE switch', before=call)
        restore = _chain(code, RESTORE_ORDER, problems, 'RSE restore', after=call)
        release = code.unique(_is_release, 'release stores after the call', problems, after=call, before=code.first(_is('rfi')))
        disable = code.unique(_psr_ic('rsm'), 'rsm 0x2000', problems, after=call)
        if restore and release and disable and not (
                restore[-1].position < release.position < disable.position < rfi.position):
            problems.append('the RSE restore does not precede the release store, rsm 0x2000 and rfi')
        cover = code.first(_is('cover'))
        ifs = cover and code.first(lambda s: s is cover, 'st8 [r17]=r18')
        gates = ifs and code.all(_is_gate, after=ifs)
        if switch and (not gates or len(gates) < CFM_GATES or switch[0].position < gates[CFM_GATES - 1].position):
            problems.append('the RSE switch starts before the covered-IFS validation completes')
        if saves and switch and saves[-1].position > switch[0].position:
            problems.append('the user RSE state is saved after the switch starts')
        return problems + ['%s: flushrs is forbidden' % step.describe() for step in code.all('flushrs')]

    def mutants(self, code):
        call, invala = code.site(_is_call), code.site('invala')
        yield 'loadrs after the BSPSTORE restore', code.swap(code.site(lambda s: s is call, 'loadrs'),
                                                             code.site(lambda s: s is call, 'mov.m ar.bspstore=r17'))
        yield 'missing pre-call invala', code.nop(invala)
        yield 'RSC quiesced after the BSPSTORE switch', code.swap(code.site('mov.m ar.rsc=r0'),
                                                                  code.site('mov.m ar.bspstore=r26'))
        yield 'RSC saved before PFS', code.swap(code.site(SAVE_ORDER[0]), code.site(SAVE_ORDER[0], SAVE_ORDER[1]))
        yield 'flushrs before the switch', code.insert(invala, 'flushrs')
        yield 'RSE switch before the last CFM gate', code.nop(invala).insert(code.site('cmp.ltu p6,p7=r16,r19'),
                                                                               'invala')


# (f register, general register loaded from the frame, general register recovered after the release, the
# resource it is installed into).
CARRIERS = (('f10', 'r6', 'r22', 'ar.pfs'), ('f11', 'r7', 'r23', 'ar.rsc'), ('f12', 'r6', 'r20', 'ar.unat'),
            ('f13', 'r10', 'r10', None), ('f6', 'r2', 'r24', 'cr.iip'), ('f7', 'r3', 'r25', 'cr.ipsr'),
            ('f8', 'r6', 'r26', 'cr.ifs'), ('f9', 'r5', 'r27', 'pr'))
EARLY_CARRIERS = frozenset({'f10', 'f11', 'f12', 'f13'})   # Recovered before rsm 0x2000
# The producer chain after `mov.m ar.rnat=r18`: each instruction, and the
# registers no other instruction may write while it is pending.
PRODUCER_CHAIN = (
    ('adds r15=64,r4', ()), ('ld8 r6=[r15],8', ('r15', 'r6')), ('ld8 r7=[r15]', ('r15', 'r6', 'r7')),
    ('setf.sig f10=r6', ('r6', 'r7')), ('setf.sig f11=r7', ('r7',)), ('adds r14=104,r4', ()),
    ('ld8 r6=[r14]', ('r14',)), ('setf.sig f12=r6', ('r6',)), ('adds r15=384,r4', ()), ('ld8 r8=[r15],8', ('r15',)),
    ('ld8 r9=[r15],8', ('r15',)), ('ld8 r10=[r15]', ('r15',)), ('setf.sig f13=r10', ('r10',)), ('adds r15=16,r4', ()),
    ('ld8 r2=[r15],8', ('r15',)), ('ld8 r3=[r15]', ('r15', 'r2')), ('setf.sig f6=r2', ('r2', 'r3')),
    ('setf.sig f7=r3', ('r3',)), ('adds r15=56,r4', ()), ('ld8 r6=[r15]', ('r15',)), ('setf.sig f8=r6', ('r6',)),
    ('adds r15=152,r4', ()), ('ld8 r5=[r15]', ('r15',)), ('setf.sig f9=r5', ('r5',)), ('adds r15=224,r4', ()),
)
def _staging(floating: str, source: str, destination: str) -> tuple[str, str]:
    """The `setf.sig` that stages a carrier and the `getf.sig` that recovers it."""
    return 'setf.sig %s=%s' % (floating, source), 'getf.sig %s=%s' % (destination, floating)


FLOATING_ARITHMETIC = re.compile(r'^(f(add|sub|ma|ms|nma|mpy|select|merge|c|class|cmp|min|max|pcmp|rsqrta|rcpa|sqrt)'
                                 r'|xma|getf|setf)')


def producer_problems(code: Code, call: Step) -> list[str]:
    """The PRODUCER_CHAIN rules of Ia64SystemCallEntryCarriers."""
    anchor = code.first(lambda s: s is call, 'mov.m ar.rnat=r18')
    if anchor is None:
        return ['no mov.m ar.rnat=r18 after the call']
    problems, previous, loads = [], anchor, {}
    for text, pending in PRODUCER_CHAIN:
        found = code.first(lambda s: s is previous, text)
        if found is None:
            return problems + ['the carrier producer chain stops before %s' % text]
        problems += ['%s writes pending %s before %s' % (step.describe(), register, text)
                     for step in code.window(after=previous, before=found) for register in pending
                     if step.writes(register)]
        previous = loads[text] = found
    for text in (chain_text for chain_text, _ in PRODUCER_CHAIN if chain_text.startswith('adds')):
        count = len(code.all(text, after=anchor))
        if count != 1:
            problems.append('%s computed %d times after the RNAT restore' % (text, count))
    copy = code.first(lambda s: s is loads['adds r15=224,r4'], 'mov r7=r4')
    if copy is None:
        problems.append('the frame address is not copied by mov r7=r4')
    else:
        problems += ['%s writes the frame base r4 before mov r7=r4' % step.describe()
                     for step in code.window(after=call, before=copy) if step.writes('r4')]
    return problems + ['%s overwrites the response %s' % (step.describe(), register)
                       for register, text in (('r8', 'ld8 r8=[r15],8'), ('r9', 'ld8 r9=[r15],8'))
                       for step in code.window(after=loads[text]) if step.writes(register)]


@register
class Ia64SystemCallEntryCarriers(_EntryProperty):
    """Floating-point registers appear only in the 16 carrier
    instructions (setf.sig f10=r6, f11=r7, f12=r6, f13=r10, f6=r2, f7=r3,
    f8=r6, f9=r5 and the matching getf.sig into r22, r23, r20, r10, r24,
    r25, r26, r27). Each setf appears once, before the release store; each
    getf once, after it; f10-f13 are recovered before `rsm 0x2000` and f6-f9
    after it and before `rfi`. A recovered register is not written again
    before `rfi` and reaches its consumer (ar.pfs, ar.rsc, ar.unat, cr.iip,
    cr.ipsr, cr.ifs, pr; r10 is final). The producers after
    `mov.m ar.rnat=r18` follow the reviewed chain from base r4 with no write
    to a pending base or value register; r4 is not written from the call to
    `mov r7=r4`, and the response r8/r9 are not written after their loads."""

    def verify(self, code):
        carrier_texts = {text for carrier in CARRIERS for text in _staging(*carrier[:3])}
        problems = ['%s is not a reviewed floating-point carrier' % step.describe() for step in code.steps
                    if (any(re.fullmatch(r'f\d+', name) for name in step.operation.reads | step.operation.writes)
                        or FLOATING_ARITHMETIC.match(step.operation.mnemonic)) and step.rendered not in carrier_texts]
        call = code.unique(_is_call, 'calls', problems)
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        release = call and code.unique(_is_release, 'release stores after the call', problems, after=call, before=code.first(_is('rfi')))
        disable = code.unique(_psr_ic('rsm'), 'rsm 0x2000', problems)
        if None in (call, rfi, release, disable):
            return problems
        for floating, source, destination, consumer in CARRIERS:
            setter, getter = (code.unique(text, text, problems) for text in _staging(floating, source, destination))
            if setter is None or getter is None:
                continue
            if setter.position > release.position or getter.position < release.position:
                problems.append('%s is not staged before and recovered after the release store' % floating)
            if floating in EARLY_CARRIERS and getter.position > disable.position:
                problems.append('%s is recovered after rsm 0x2000' % floating)
            if floating not in EARLY_CARRIERS and not disable.position < getter.position < rfi.position:
                problems.append('%s is recovered while collection is on' % floating)
            problems += ['%s overwrites the recovered %s' % (step.describe(), destination)
                         for step in code.window(after=getter, before=rfi) if step.writes(destination)]
            if consumer and not code.all(lambda s: s.reads(destination) and s.writes(consumer), after=getter,
                                         before=rfi):
                problems.append('recovered %s never reaches %s' % (destination, consumer))
        return problems + producer_problems(code, call)

    def mutants(self, code):
        disable, staging = code.site(_psr_ic('rsm')), code.site('getf.sig r23=f11', 'bsw.1')
        for floating, source, destination, consumer in CARRIERS:
            staged, recovery = _staging(floating, source, destination)
            setter, getter = code.site(staged), code.site(recovery)
            yield 'wrong %s carrier source' % floating, code.replace(setter, text='setf.sig %s=r0' % floating)
            yield '%s clobber before restoration' % floating, code.insert(setter, 'setf.sig %s=r0' % floating,
                                                                           after=True)
            yield 'wrong %s restoration source' % floating, code.replace(
                getter, text='getf.sig %s=%s' % (destination, 'f7' if floating == 'f6' else 'f6'))
            yield '%s clobber after carrier recovery' % destination, code.insert(getter, 'mov %s=r0' % destination,
                                                                                 after=True)
            if floating not in EARLY_CARRIERS:
                moved = code.insert(staging, recovery)
                yield 'early IC-on %s carrier staging' % destination, moved.nop(
                    moved.site(lambda s: s.position > staging.position + 1, recovery))
            if consumer is not None:
                final = code.site(lambda s: s.position > disable.position and s.reads(destination)
                                  and s.writes(consumer))
                yield 'wrong %s final restoration source' % consumer, code.replace(
                    final, text=final.text.replace('=%s' % destination, '=r0'))
        anchor = code.site(_is_call, 'mov.m ar.rnat=r18')
        for text, load in (('adds r15=64,r4', 'ld8 r6=[r15],8'), ('adds r14=104,r4', 'ld8 r6=[r14]'),
                           ('adds r15=384,r4', 'ld8 r8=[r15],8'), ('adds r15=16,r4', 'ld8 r2=[r15],8'),
                           ('adds r15=56,r4', 'ld8 r6=[r15]'), ('adds r15=152,r4', 'ld8 r5=[r15]')):
            address = code.site(lambda s: s is anchor, text)
            base = text.split('=')[0].split()[1]
            yield 'wrong frame source for %s' % text, code.replace(address, text=text.replace(',r4', ',r0'))
            yield '%s clobber before the load after %s' % (base, text), code.insert(
                code.site(lambda s: s is address, load), 'mov %s=r0' % base)
        for register in ('r8', 'r9'):
            yield '%s response clobber' % register, code.insert(
                code.site(lambda s: s is anchor, 'ld8 %s=[r15],8' % register), 'mov %s=r0' % register, after=True)
        yield 'frame base clobber after the call', code.insert(code.site(_is_call), 'mov r4=r0', after=True)


@dataclasses.dataclass(frozen=True)
class UnknownBreakFacts:
    code: Code
    immediate: int | None       # Ia64SystemCallBreakImmediate


def _last_writer(code: Code, register: str, before: Step) -> Step | None:
    writers = [step for step in code.window(before=before) if step.writes(register)]
    return writers[-1] if writers else None


@register
class Ia64SystemCallEntryUnknownBreak(_EntryProperty):
    """A break with another immediate never stops silently: the
    entry has exactly one branch to the unknown-break leaf
    `AsteriaIa64SystemCallUnknownBreak`, a `(p6) br.cond.dpnt.few` whose p6
    is set, last, by `cmp.eq p7,p6=r19,r16` with r19 last written by
    `mov r19=cr.iim` and r16 by `movl r16=` Ia64SystemCallBreakImmediate
    (from the header), so it is taken exactly when cr.iim differs. It follows
    the last overlap-scan landing (the complete context admission) and
    precedes the State publication, and at the branch cr.iip (r22) and the
    admitted kernel RSE start (r26) and memory-stack end (r27) are still the
    values loaded for them. The leaf itself is Ia64SystemCallUnknownBreakLeaf."""

    def extract(self, context, subject):
        header = Layout.constants(Layout.read(context.root / 'Source/Platform/Ia64/System/Ia64SystemCallEntry.hpp'))
        return UnknownBreakFacts(super().extract(context, subject), header.get('Ia64SystemCallBreakImmediate'))

    def verify(self, facts):
        code = facts.code
        if facts.immediate is None:
            return ['Ia64SystemCallEntry.hpp no longer states Ia64SystemCallBreakImmediate']
        gates = code.all(_is_unknown_break_gate)
        if len(gates) != 1:
            return ['%d branches to %s, expected one' % (len(gates), UNKNOWN_BREAK)]
        gate, problems = gates[0], []
        if gate.predicate != 'p6':
            problems.append('%s is not predicated by p6' % gate.describe())
        compare = next((step for step in reversed(code.window(before=gate)) if step.operation.writes_predicate('p6')),
                       None)
        if compare is None or compare.text != 'cmp.eq p7,p6=r19,r16' or compare.instruction.predicate is not None:
            problems.append('p6 of the unknown-break branch is set by %s, not cmp.eq p7,p6=r19,r16'
                            % (compare.describe() if compare else 'nothing'))
        else:
            expected = {'r19': 'mov r19=cr.iim', 'r16': 'movl r16=%#x' % facts.immediate}
            for register, text in expected.items():
                writer = _last_writer(code, register, compare)
                if writer is None or writer.text != text:
                    problems.append('the unknown-break compare reads %s from %s, expected %s'
                                    % (register, writer.describe() if writer else 'nothing', text))
        for register, text in (('r22', 'mov r22=cr.iip'), ('r26', 'ld8 r26=[r24],8'), ('r27', 'ld8 r27=[r24],8')):
            writer = _last_writer(code, register, gate)
            if writer is None or writer.text != text:
                problems.append('%s reaches the unknown-break branch from %s, expected %s'
                                % (register, writer.describe() if writer else 'nothing', text))
        landings = [step.target for step in code.all(lambda s: s.operation.mnemonic == 'br.cond.sptk.few')]
        if not landings or gate.offset < max(landings):
            problems.append('the unknown-break branch precedes the complete context admission')
        publication = code.first(_is_release)
        if publication is None or gate.position > publication.position:
            problems.append('the unknown-break branch does not precede the State publication')
        return problems

    def mutants(self, facts):
        code = facts.code
        gate = code.site(_is_unknown_break_gate)
        compare = code.site('cmp.eq p7,p6=r19,r16')

        def edited(changed):
            return dataclasses.replace(facts, code=changed)

        yield 'unknown immediates stop silently', edited(code.replace(gate, text='br.cond.dpnt.few <+%#x>'
                                                                      % failure_target(code)))
        yield 'no unknown-break branch', edited(code.nop(gate))
        yield 'unpredicated unknown-break branch', edited(code.replace(gate, predicate=None))
        yield 'immediate compared with cr.isr', edited(code.replace(compare, text='cmp.eq p7,p6=r20,r16'))
        yield 'another break immediate', dataclasses.replace(facts, immediate=facts.immediate + 1)
        yield 'predicate overwritten before the branch', edited(code.insert(gate, 'cmp.ne p6,p7=r0,r0'))
        yield 'kernel stack end clobbered', edited(code.insert(gate, 'mov r27=r0'))
        yield 'cr.iip clobbered', edited(code.insert(gate, 'mov r22=r0'))
        yield 'branch before the context admission', edited(code.nop(gate).insert(
            code.site(lambda s: s.operation.mnemonic == 'br.cond.sptk.few'), gate.rendered))
        yield 'branch after the State publication', edited(code.nop(gate).insert(
            code.site(_is_release), gate.rendered, after=True))


UNKNOWN_BREAK_ORDER = ('cover', 'invala', 'mov.m ar.rsc=r0', 'mov.m ar.bspstore=r26', 'mov.m ar.rnat=r0', 'mov r16=3',
                       'mov.m ar.rsc=r16', 'adds r12=-16,r27', 'alloc r16=ar.pfs,2,0,0', 'mov r32=r19',
                       'mov r33=r22', 'ssm 0x2000', 'bsw.1', 'srlz.d')


@register
class Ia64SystemCallUnknownBreakLeaf(Subjects.CodeProperty):
    """The unknown-break leaf reports and stops: it starts with
    `cover`, then, each once and in this order, `invala`, enforced-lazy
    `mov.m ar.rsc=r0`, the switch to the admitted kernel RSE
    (`mov.m ar.bspstore=r26`, `mov.m ar.rnat=r0`), the kernel RSE
    configuration (`mov r16=3`; `mov.m ar.rsc=r16`), the kernel stack
    (`adds r12=-16,r27`), a two-output frame, out0 = cr.iim (`mov r32=r19`)
    and out1 = cr.iip (`mov r33=r22`), `ssm 0x2000`, `bsw.1` and `srlz.d`;
    then the kernel GP (`movl r1=` against `__gp`) and the only call, an
    unpredicated `br.call b0=` to AsteriaIa64FailStopUnknownSystemCallBreak,
    with none of r1, r12, r32 and r33 written in between. After the call
    comes only the stop: `hint.m 0` and an unpredicated `br.few` to itself.
    The leaf accesses no memory and has no `rfi`, return or other PSR write."""
    SYMBOL = UNKNOWN_BREAK

    def verify(self, code):
        problems = []
        steps = code.steps
        if not steps or steps[0].text != 'cover' or steps[0].instruction.predicate is not None:
            problems.append('the leaf does not start with an unpredicated cover')
        chain = _chain(code, UNKNOWN_BREAK_ORDER, problems, 'unknown-break transition')
        gp = code.unique(_is_movl_r1, 'movl r1= loads', problems)
        call = code.unique(_is_call, 'calls', problems)
        if chain is None or gp is None or call is None:
            return problems
        if not chain[-1].position < gp.position < call.position:
            problems.append('the kernel GP and the call do not follow srlz.d in this order')
        target = call.operation.target
        if (call.instruction.predicate is not None or 'b0' not in call.operation.writes or target is None
                or (target.symbol, target.offset) != (UNKNOWN_BREAK_REPORTER, 0)):
            problems.append('%s is not an unpredicated br.call b0= to %s' % (call.describe(), UNKNOWN_BREAK_REPORTER))
        loaded = gp.operation.target
        if loaded is None or (loaded.kind, loaded.symbol, loaded.offset) != ('reloc', GLOBAL_POINTER, 0):
            problems.append('%s does not load __gp' % gp.describe())
        problems += ['%s writes %s between its setup and the call' % (step.describe(), register)
                     for register, setup in (('r1', gp), ('r12', chain[7]), ('r32', chain[9]), ('r33', chain[10]))
                     for step in code.window(after=setup, before=call) if step.writes(register)]
        problems += ['%s writes %s before the leaf reads it' % (step.describe(), register)
                     for register, use in (('r19', chain[9]), ('r22', chain[10]), ('r26', chain[3]), ('r27', chain[7]))
                     for step in code.window(before=use) if step.writes(register)]
        after = code.window(after=call)
        stop = after[0] if after else None
        loop = after[1] if len(after) > 1 else None
        if (len(after) != 2 or stop.text != 'hint.m 0' or loop.operation.mnemonic != 'br.few'
                or loop.target != stop.offset or loop.instruction.predicate is not None):
            problems.append('the call is not followed by exactly hint.m 0 and a br.few to it')
        problems += ['%s accesses memory' % step.describe() for step in steps if step.operation.memory is not None]
        problems += ['%s: the leaf never returns or writes PSR other than ssm 0x2000' % step.describe()
                     for step in steps if step.operation.base in ('rfi', 'rsm', 'sum', 'rum') or step.operation.is_return
                     or (step.operation.base == 'ssm' and step.text != 'ssm 0x2000')]
        return problems

    def mutants(self, code):
        call = code.site(_is_call)
        for description, text, replacement in (
                ('out0 from cr.isr', 'mov r32=r19', 'mov r32=r20'), ('out1 from cr.ipsr', 'mov r33=r22', 'mov r33=r21'),
                ('user stack kept', 'adds r12=-16,r27', 'nop.i 0x0'),
                ('user backing store kept', 'mov.m ar.bspstore=r26', 'nop.m 0x0'),
                ('collection left off', 'ssm 0x2000', 'nop.m 0x0'), ('bank zero kept', 'bsw.1', 'nop.b 0x0')):
            yield description, code.replace(code.site(text), text=replacement)
        yield 'RSE switched after the frame', code.swap(code.site('mov.m ar.bspstore=r26'),
                                                        code.site('alloc r16=ar.pfs,2,0,0'))
        yield 'call to another function', code.replace(call, text=call.text.replace(UNKNOWN_BREAK_REPORTER,
                                                                                     'AsteriaWrongReporter'))
        yield 'the reporter returns to user mode', code.insert(call, 'rfi', after=True)
        yield 'out0 clobbered before the call', code.insert(call, 'mov r32=r0')
        yield 'memory access in the leaf', code.insert(call, 'st8 [r12]=r0')
        yield 'second call', code.insert(call, call.text, after=True)
        gp = code.site(_is_movl_r1)
        yield 'GP from another symbol', code.replace(gp, text=gp.text.replace(GLOBAL_POINTER, '__wrong_gp'))


@dataclasses.dataclass(frozen=True)
class ScrubFacts:
    code: Code
    count: int | None       # Ia64PhysicalStackedRegisterCount


@register
class Ia64SystemCallEntryStackedRegisterScrub(_EntryProperty):
    """PL3 never receives a kernel value in a stacked register:
    between the bridge call and `loadrs`, after `mov.m ar.rsc=r16` has
    selected enforced-lazy mode with the dirty-partition distance, the entry
    scrubs the physical stacked-register file (Properties.
    stacked_register_scrub over Ia64PhysicalStackedRegisterCount registers,
    the count PAL_RSE_INFO must report on every CPU, which one frame spans).
    loadrs then reloads only the interrupted dirty partition, so every other
    stacked register, which PL3 may allocate later, holds zero."""

    def extract(self, context, subject):
        return ScrubFacts(super().extract(context, subject),
                          Properties.physical_stacked_register_count(context.root))

    def _sites(self, code: Code, problems: list[str]):
        call = code.unique(_is_call, 'calls', problems)
        loadrs = call and code.unique('loadrs', 'loadrs after the call', problems, after=call)
        return call, loadrs

    def verify(self, facts):
        if facts.count is None:
            return ['%s no longer states %s' % (Properties.PHYSICAL_STACKED_HEADER,
                                               Properties.PHYSICAL_STACKED_CONSTANT)]
        code, problems = facts.code, []
        call, loadrs = self._sites(code, problems)
        if loadrs is None:
            return problems
        found, sites = Properties.stacked_register_scrub(code, call, loadrs, facts.count)
        problems += found
        lazy = code.first(lambda s: s is call, 'mov.m ar.rsc=r16')
        if sites is not None and (lazy is None or lazy.position > sites[0].position):
            problems.append('the scrub does not run in the enforced-lazy mode of mov.m ar.rsc=r16')
        return problems

    def mutants(self, facts):
        code = facts.code
        call, loadrs = self._sites(code, [])
        _, (grow, drop) = Properties.stacked_register_scrub(code, call, loadrs, facts.count)
        for description, changed in Properties.stacked_register_scrub_mutants(code, grow, drop, facts.count):
            yield description, dataclasses.replace(facts, code=changed)
        yield 'the scrub before enforced-lazy mode', dataclasses.replace(facts, code=code.nop(
            code.site(lambda s: s is call, 'mov.m ar.rsc=r16')).insert(drop, 'mov.m ar.rsc=r16', after=True))
        yield 'a physical file larger than the frame', dataclasses.replace(facts, count=facts.count + 8)


FIRST_IN_GROUP = frozenset({'alloc', 'loadrs', 'flushrs'})
LAST_IN_GROUP = frozenset({'cover', 'bsw', 'rfi'})
RFI_STATE = frozenset({'cr.ipsr', 'cr.iip', 'cr.ifs'})


@register
class Ia64SystemCallEntryGroupPlacement(_EntryProperty):
    """New. The architected instruction-group rules of the entry: `alloc`,
    `loadrs` and `flushrs` start an instruction group; `cover`, `bsw` and
    `rfi` end one; every PSR write (`ssm`/`rsm`) is followed by `srlz.d` in a
    later group before any dependent instruction (P-SERIALIZE); and the
    cr.ipsr/cr.iip/cr.ifs writes that `rfi` consumes lie in earlier groups
    than the `rfi`."""

    def verify(self, code):
        problems = ['%s is not the first instruction of its group' % step.describe()
                    for step in code.steps if step.operation.base in FIRST_IN_GROUP and not code.starts_group(step)]
        problems += ['%s is not the last instruction of its group' % step.describe()
                     for step in code.steps if step.operation.base in LAST_IN_GROUP and not step.instruction.stop_after]
        problems += Properties.serialization_problems(list(code.instructions),
                                                      is_write=lambda op: op.base in ('ssm', 'rsm'))
        rfis = code.all(_is('rfi'))
        return problems + ['%s is not followed by rfi in a later group' % step.describe() for step in code.steps
                           if step.operation.writes & RFI_STATE
                           and not any(rfi.position > step.position and rfi.group > step.group for rfi in rfis)]

    def mutants(self, code):
        call = code.site(_is_call)
        # Clear the stop after the instruction `shift` slots before the step.
        for description, step, shift in (('loadrs inside a group', code.site(lambda s: s is call, 'loadrs'), 1),
                                         ('alloc inside a group', code.site('alloc r22=ar.pfs,2,0,0'), 1),
                                         ('cover inside a group', code.site('cover'), 0),
                                         ('rfi inside a group', code.site('rfi'), 0),
                                         ('bsw.1 inside a group', code.site(lambda s: s is call, 'bsw.1'), 0)):
            yield description, code.without_stops(step.position - shift, step.position - shift + 1)
        for write in ('ssm 0x2000', 'rsm 0x2000'):
            # Exchange the bank switch that closes the PSR write's group with
            # the srlz.d that opens the next one: the switch still ends a
            # group, but the serialization now shares the write's group.
            psr = code.site(write)
            yield 'srlz.d in the %s group' % write.split()[0], code.swap(
                code.site(lambda s: s is psr, _is('bsw')), code.site(lambda s: s is psr, 'srlz.d'))
        yield 'cr.iip/cr.ipsr written in the rfi group', code.without_stops(code.site('mov cr.iip=r24'),
                                                                             code.site('rfi'))


@dataclasses.dataclass(frozen=True)
class GlobalPointerFacts:
    code: Code
    values: dict                                    # (address, slot) -> movl immediate or call target in the image
    global_pointer: tuple[tuple[int, int],...]     # (value, section index) of each __gp definition
    bridge: int | None

    def value(self, step: Step) -> int | None:
        return self.values.get((step.instruction.address, step.instruction.slot))


@register
class Ia64SystemCallEntryGlobalPointer(_EntryProperty):
    """Resolved global pointer and bridge call targets. In every
    linked image that contains the entry (the O0/O2 `NucleusCore.elf` and the
    firmware `AsteriaBoot.elf`), `__gp` is a single absolute symbol; the entry
    has exactly one unpredicated `movl r1=imm` with imm == `__gp` (decoded
    from the image bytes), exactly one unpredicated `br.call b0=` whose
    resolved target is `AsteriaIa64SystemCallBridge`, the load precedes the
    call, and nothing writes r1 in between: the bridge and its callees
    address globals through the image's final GP."""

    def subjects(self, context):
        return Subjects.artifacts(context, ENTRY, objects=False, images=True)

    def extract(self, context, subject):
        elf, code = load(subject.path), super().extract(context, subject)
        values = Properties.resolved_values(elf, elf.symbol(ENTRY).section_index, code.instructions)
        bridge = elf.symbol(BRIDGE)
        return GlobalPointerFacts(code, {(instruction.address, instruction.slot): value for instruction, value
                                         in zip(code.instructions, values) if value is not None},
                                  tuple((symbol.value, symbol.section_index) for symbol in elf.definitions(
                                      GLOBAL_POINTER)), bridge.value if bridge is not None else None)

    def verify(self, facts):
        if len(facts.global_pointer) != 1:
            return ['__gp has %d definitions, expected one' % len(facts.global_pointer)]
        value, section = facts.global_pointer[0]
        problems = [] if section == SHN_ABS else ['__gp is not an absolute symbol']
        code = facts.code
        loads, calls = code.all(_is_movl_r1), code.all(_is_call)
        if len(loads) != 1:
            problems.append('%d movl r1= loads, expected one' % len(loads))
        if len(calls) != 1:
            problems.append('%d calls, expected one' % len(calls))
        if problems:
            return problems
        (load,), (call,) = loads, calls
        loaded, target = facts.value(load), facts.value(call)
        problems = [problem for failed, problem in (
            (load.instruction.predicate is not None, 'the GP load is predicated'),
            (loaded != value, 'movl r1= loads %s, but __gp is %#x' % (
                'nothing' if loaded is None else '%#x' % loaded, value)),
            (call.instruction.predicate is not None or 'b0' not in call.operation.writes,
             'the bridge call is not an unpredicated br.call b0='),
            (facts.bridge is None or target != facts.bridge, 'the call resolves to %s, not %s' % (
                'nothing' if target is None else '%#x' % target, BRIDGE)),
            (load.position > call.position, 'the GP is loaded after the bridge call')) if failed]
        return problems + ['%s writes r1 between the GP load and the call' % step.rendered
                           for step in code.window(after=load, before=call) if step.writes('r1')]

    def mutants(self, facts):
        code = facts.code
        load, call = code.site(_is_movl_r1), code.site(_is_call)

        def with_code(changed):
            return dataclasses.replace(facts, code=changed)

        def with_value(step, value):
            key = step.instruction.address, step.instruction.slot
            return dataclasses.replace(facts, values={**facts.values, key: value})

        yield 'GP loaded into r2', with_code(code.replace(load, text=load.text.replace('movl r1=', 'movl r2=')))
        yield 'wrong fixed GP 0x0', with_value(load, 0)
        yield 'predicated GP load', with_code(code.replace(load, predicate='p06'))
        yield 'r1 clobber before the bridge', with_code(code.insert(load, 'mov r1=r0', after=True))
        yield 'implicit r1 post-increment before the bridge', with_code(code.insert(load, 'ld8 r11=[r1],8', after=True))
        yield 'call resolves elsewhere', with_value(call, (facts.bridge or 0) + 0x10)
        yield '__gp relative to a section', dataclasses.replace(
            facts, global_pointer=((facts.global_pointer[0][0], 1),))
        yield 'second call', with_code(code.insert(call, call.text, after=True))
        moved = code.delete(load)
        yield 'GP loaded after the call', with_code(moved.with_instructions(
            moved.instructions[:call.position] + (load.instruction,) + moved.instructions[call.position:]))
