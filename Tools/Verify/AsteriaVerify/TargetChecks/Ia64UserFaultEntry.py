# SPDX-License-Identifier: Apache-2.0
"""IA-64 user-fault entry `AsteriaIa64UserFaultEntry`, user interrupt
entry `AsteriaIa64UserInterruptEntry` and user exception entry
`AsteriaIa64UserExceptionEntry`
(Source/Platform/Ia64/Interrupt/Ia64UserFaultEntry.S), three expansions of
one assembler body. The user-fault entry is the target of the alternate
data-TLB refill handler's escape for a PL3 access its ledger does not map
(selector 4 in bank-zero r16); the user interrupt entry is the target of the
PL3 interrupt claim (selector 5), which has already claimed a due timer
interrupt (Ia64UserInterruptClaim.py); the user exception entry is the
target of the General Exception vector's trampoline (selector 6).

With PSR.ic off each entry admits only its selector, its interrupted PSR (the
user-fault and exception ones with PSR.i either, the interrupt one with
PSR.i set) and the running thread's selected user-fault context, claims the
thread's Ia64PageFaultFrame, records its vector offset (0x1000, 0x3000 or
0x5400, by which the bridge picks the fault or interrupt handler and the
decoder the fault's cause), captures the complete
interrupted PL3 image (interruption registers, every application and branch
register, static r1-r15 and bank-one r16-r31 with their NaT bits, PR,
f2-f127), covers the frame and switches the register stack onto the thread's
entry storage. It calls the P64 bridge
`AsteriaIa64UserFaultBridge(frame, handler)` with collection on, bank one and
the kernel GP; on a zero result it scrubs the stacked registers, restores
the complete image and returns with rfi to the exact interrupted slot. The
goldens Tests/Golden/Ia64/Interrupt/AsteriaIa64UserFaultEntry.dis,
AsteriaIa64UserInterruptEntry.dis and AsteriaIa64UserExceptionEntry.dis pin
the instructions; these properties state the rules a golden update
must keep on the objects and linked images. Each `Ia64UserFaultEntry*`
property has `Ia64UserInterruptEntry*` and `Ia64UserExceptionEntry*` variants
that judge the other entries. Their mutants edit the decoded instructions.
"""
from __future__ import annotations

from AsteriaVerify import Ia64StopViews

import dataclasses
import re
from collections import Counter

from AsteriaVerify import Ia64, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code, Step
from AsteriaVerify.Elf import SHN_ABS, load

BRIDGE = 'AsteriaIa64UserFaultBridge'
GLOBAL_POINTER = '__gp'
FRAME = 'r22'           # The frame while collection is off before the call
CARRIED_FRAME = 'r4'    # The frame across the bridge
# Frame offsets (Ia64PageFaultFrame, pinned by its static_asserts).
STATE, VECTOR_OFFSET, IIP, IPSR, COVERED_IFS, HARDWARE_IFS = 24, 48, 64, 72, 88, 80


@dataclasses.dataclass(frozen=True)
class Variant:
    """What distinguishes the expansions of the entry body."""
    entry: str
    selector: int               # Bank-zero r16 on entry
    interrupted_mask: int       # The interrupted-PSR admission mask
    interrupted_value: int      # And value
    vector_offset: int          # The frame's VectorOffset


# Ia64UserFaultAlternateDataTlbSelector, Ia64UserFaultInterruptedProcessorStatusMask/Value (PSR.i
# either) and Ia64PageFaultAlternateDataTlbVectorOffset; Ia64UserInterruptSelector,
# Ia64UserInterruptInterruptedProcessorStatusMask/Value (PSR.i set) and Ia64UserInterruptVectorOffset;
# Ia64UserExceptionSelector, the user-fault admission and Ia64UserGeneralExceptionVectorOffset.
FAULT = Variant('AsteriaIa64UserFaultEntry', 4, 0x501708022000, 0x101308022000, 0x1000)
INTERRUPT = Variant('AsteriaIa64UserInterruptEntry', 5, 0x501708026000, 0x101308026000, 0x3000)
EXCEPTION = Variant('AsteriaIa64UserExceptionEntry', 6, 0x501708022000, 0x101308022000, 0x5400)
VARIANTS = (FAULT, INTERRUPT, EXCEPTION)
STATIC_REGISTERS = tuple('r%d' % number for number in range(1, 16))
BANKED_REGISTERS = tuple('r%d' % number for number in range(16, 32))
FLOATING_REGISTERS = tuple('f%d' % number for number in range(2, 128))
APPLICATION_REGISTERS = ('ar.pfs', 'ar.rsc', 'ar.bsp', 'ar.unat', 'ar.lc', 'ar.ec', 'ar.csd', 'ar.ccv', 'ar.fpsr',
                         'ar.bspstore', 'ar.rnat')
RESTORED_APPLICATION_REGISTERS = ('ar.lc', 'ar.ec', 'ar.csd', 'ar.ccv', 'ar.fpsr', 'ar.pfs', 'ar.rsc', 'ar.unat',
                                  'ar.bspstore', 'ar.rnat')
BRANCH_REGISTERS = tuple('b%d' % number for number in range(8))
INTERRUPTION_REGISTERS = ('cr.iip', 'cr.ipsr', 'cr.isr', 'cr.ifa', 'cr.ifs', 'cr.itir', 'cr.iipa', 'cr.iim')


def _is(base: str):
    return lambda step: step.operation.base == base


def _psr_ic(base: str):
    return lambda step: step.operation.base == base and any(value & Ia64.PSR_IC for value in step.operation.immediates)


def _is_call(step: Step) -> bool:
    return step.operation.is_call


def _is_release(step: Step) -> bool:
    return step.operation.memory == 'store' and 'rel' in step.operation.completers


def _is_acquire(step: Step) -> bool:
    return step.operation.memory == 'load' and 'acq' in step.operation.completers


def _is_rejection(step: Step) -> bool:
    return step.operation.mnemonic == 'br.cond.dpnt.few'


def _accesses_memory(step: Step) -> bool:
    return step.operation.memory is not None or step.operation.base == 'probe'


def failure_target(code: Code) -> int | None:
    """The fail-stop offset F: the common target of all dpnt rejections."""
    targets = {step.target for step in code.all(_is_rejection)}
    return targets.pop() if len(targets) == 1 else None


def banks(code: Code) -> dict[int, int]:
    """The general-register bank each step addresses: bank zero at entry,
    switched by `bsw`, which ends its instruction group, so the
    next step uses the new bank. The bridge returns in the bank it was
    called in."""
    bank, result = 0, {}
    for step in code.steps:
        result[step.position] = bank
        if step.text in ('bsw.0', 'bsw.1'):
            bank = int(step.text[-1])
    return result


def writes_between(code: Code, register: str, after: Step, before: Step) -> list[Step]:
    """The steps strictly between `after` and `before` that write
    `register` in the bank `before` addresses."""
    active = banks(code)
    return [step for step in code.window(after=after, before=before) if step.writes(register)
            and (register not in Ia64.BANKED_GENERAL or active[step.position] == active[before.position])]


def _last_writer(code: Code, register: str, before: Step) -> Step | None:
    """The last step before `before` that writes `register` in the bank
    `before` addresses (r16-r31 are banked)."""
    active = banks(code)
    writers = [step for step in code.window(before=before) if step.writes(register)
               and (register not in Ia64.BANKED_GENERAL or active[step.position] == active[before.position])]
    return writers[-1] if writers else None


def claim(code: Code) -> Step | None:
    """The State claim: the first release store."""
    return code.first(_is_release)


class _EntryProperty(Ia64StopViews.CoreCodeProperty):
    """A property of the user-fault entry; its `Ia64UserInterruptEntry*`
    subclass sets VARIANT to INTERRUPT or EXCEPTION and judges that entry."""
    VARIANT = FAULT
    IMAGES = True

    @property
    def SYMBOL(self) -> str:
        return self.VARIANT.entry


@register
class Ia64UserFaultEntrySymbol(Subjects.SymbolContract):
    """The refill handler branches directly to the user-fault entry
    and the interrupt claim to the user interrupt entry, so each is exactly
    one GLOBAL HIDDEN FUNC definition (never exported), bundle-aligned with a
    non-zero whole number of bundles, in an ALLOC|EXECINSTR section, in the
    objects and in the linked images."""
    SYMBOLS = tuple(variant.entry for variant in VARIANTS)
    IMAGES = True


@register
class Ia64UserFaultEntryRelocations(_EntryProperty):
    """The entry extent carries exactly two relocations: IMM64 →
    `__gp` in slot 1 of an MLX bundle whose instruction is an unpredicated
    `movl r1=`, and PCREL21B → `AsteriaIa64UserFaultBridge` in slot 2 of a
    bundle whose instruction is an unpredicated `br.call b0=`; the GP
    relocation precedes the call's. No data, GP-relative or LTOFF relocation
    exists: collection is off for most of the entry. Checked in the objects
    and in the --emit-relocs firmware image."""

    def subjects(self, context):
        return Subjects.artifacts(context, self.SYMBOL, images=True,
                                  image_filter=lambda elf: bool(elf.relocation_sections()))

    def extract(self, context, subject):
        code = super().extract(context, subject)
        by_address = {instruction.address: instruction for instruction in code.instructions}
        sites = []
        for fact in code.relocations:
            bundle = (code.start + fact.offset) & ~0xF
            instruction = by_address.get(bundle + 6 * (fact.offset & 0xF))
            sites.append((fact, instruction.template if instruction else None, instruction))
        return tuple(sites)

    def verify(self, facts):
        expected = {('IMM64', GLOBAL_POINTER): (1, 'MLX', lambda op: op.base == 'movl' and op.writes == {'r1'}),
                    ('PCREL21B', BRIDGE): (2, None, lambda op: op.is_call and 'b0' in op.writes)}
        found = Counter((fact.type, fact.target, fact.addend) for fact, _, _ in facts)
        problems = Properties.multiset_problems(found, [(kind, target, 0) for kind, target in expected], 'relocation')
        offsets = {}
        for fact, template, instruction in facts:
            rule = expected.get((fact.type, fact.target))
            if rule is None:
                continue
            slot, wanted, feeds = rule
            offsets[fact.type] = fact.offset
            if fact.offset & 0xF != slot:
                problems.append('%s → %s is not in bundle slot %d' % (fact.type, fact.target, slot))
            if wanted is not None and template != wanted:
                problems.append('%s → %s lies in a %s bundle, not %s' % (fact.type, fact.target, template, wanted))
            if instruction is None or instruction.predicate or not feeds(Ia64.parse(instruction)):
                problems.append('%s → %s does not feed its unpredicated instruction' % (fact.type, fact.target))
        if len(offsets) == 2 and offsets['IMM64'] >= offsets['PCREL21B']:
            problems.append('the __gp relocation does not precede the bridge call relocation')
        return problems

    def mutants(self, facts):
        gp = next(index for index, (fact, _, _) in enumerate(facts) if fact.type == 'IMM64')
        call = next(index for index, (fact, _, _) in enumerate(facts) if fact.type == 'PCREL21B')

        def relocation(index, **changes):
            fact, template, instruction = facts[index]
            return facts[:index] + ((dataclasses.replace(fact, **changes), template, instruction),) + facts[index + 1:]

        def site(index, template=None, instruction=None):
            fact, old_template, old_instruction = facts[index]
            return facts[:index] + ((fact, template or old_template, instruction or old_instruction),) \
                + facts[index + 1:]

        yield 'IMM64 against another symbol', relocation(gp, target='__wrong_gp')
        yield 'PCREL21B against another symbol', relocation(call, target='AsteriaIa64SystemCallBridge')
        yield 'PCREL21B in slot 1', relocation(call, offset=(facts[call][0].offset & ~0xF) + 1)
        yield 'IMM64 in an MII bundle', site(gp, template='MII')
        yield 'IMM64 feeding movl r2', site(gp, instruction=dataclasses.replace(
            facts[gp][2], text=facts[gp][2].text.replace('movl r1=', 'movl r2=')))
        yield 'predicated bridge call', site(call, instruction=dataclasses.replace(facts[call][2], predicate='p06'))
        yield 'extra data relocation', facts + ((Properties.RelocationFact(0x20, 'DIR64LSB', 'AsteriaWrong', 0),
                                                 None, None),)
        yield 'GP relocation after the call', relocation(gp, offset=(facts[call][0].offset & ~0xF) + 0x11)


# The forms of the IC-off accesses with their exact counts: the root, context
# and frame accesses of the admission and capture, the static, bank-one and
# floating spills, the RSE switch, and the final restore.
def _repeat(fmt: str, items, last: str | None = None) -> list[str]:
    items = list(items)
    return [(last if last is not None and index == len(items) - 1 else fmt) % item for index, item in enumerate(items)]


IC_OFF_ALLOWLIST = Counter([
    'mov.m r18=ar.rsc', 'mov.m ar.k6=r18', 'mov.m ar.rsc=r18', 'ld8 r23=[r18]', 'ld8.acq r24=[r24]', 'ld8 r22=[r18],8',
    'ld8 r21=[r18],8', 'ld8 r20=[r18],8', 'ld8 r19=[r18],8', 'ld8 r17=[r18],8', 'ld8 r16=[r18],8',
    'ld8 r24=[r18],8', 'ld8 r18=[r18]', 'ld8.acq r18=[r18]', 'ld8.acq r18=[r23]', 'st8.rel [r23]=r18',
    'st8 [r18]=r27', 'st8 [r18]=r27', 'st8 [r18]=r30', 'st8 [r18]=r29', 'st8 [r18]=r28', 'st8 [r18]=r25',
    'st8 [r18]=r26', 'st8 [r18]=r31', 'st8 [r17]=r16', 'mov.i r23=ar.pfs', 'mov.m r23=ar.k6', 'mov.m r23=ar.bsp',
    'mov.m r23=ar.bsp', 'mov.m r23=ar.unat', 'mov.m r16=ar.unat', 'cover', 'mov.m r24=ar.bsp', 'invala',
    'mov.m ar.rsc=r0', 'mov.m r23=ar.bspstore', 'mov.m r24=ar.rnat', 'st8 [r18]=r24', 'st8 [r18]=r24',
    'mov.m ar.bspstore=r19', 'mov.m ar.rnat=r0', 'mov.m ar.rsc=r24', 'alloc r24=ar.pfs,2,0,0',
    *_repeat('st8.spill [r18]=r%d,8', range(1, 16), 'st8.spill [r18]=r%d'),
    *_repeat('st8.spill [r14]=r%d,8', range(16, 32), 'st8.spill [r14]=r%d'),
    *_repeat('stf.spill [r14]=f%d,16', range(2, 128), 'stf.spill [r14]=f%d'),
    # The final restore.
    'ld8 r17=[r24],8', 'ld8 r18=[r24]', 'ld8 r19=[r24]', 'ld8 r20=[r24]', 'ld8 r21=[r24],8', 'ld8 r22=[r24]',
    'ld8 r23=[r24]', 'mov.m ar.unat=r15', 'mov.m ar.unat=r15',
    *_repeat('ld8.fill r%d=[r14],8', range(16, 32), 'ld8.fill r%d=[r14]'),
    *_repeat('ld8.fill r%d=[r15],8', range(1, 16), 'ld8.fill r%d=[r15]'),
    'st8.rel [r16]=r0', 'mov.i ar.pfs=r21', 'mov.m ar.rsc=r22', 'mov.m ar.unat=r23',
]) + Counter({'st8 [r18]=r23': 25, 'mov.m ar.unat=r0': 3, 'mov.m r23=ar.unat': 1, 'ld8 r15=[r14]': 2})
IC_SENSITIVE_BASES = frozenset({'probe', 'alloc', 'cover', 'loadrs', 'flushrs', 'invala', 'tnat'})
RSE_REGISTERS = frozenset({'ar.k6', 'ar.rsc', 'ar.bsp', 'ar.bspstore', 'ar.rnat', 'ar.pfs', 'ar.unat'})


@register
class Ia64UserFaultEntryCollection(_EntryProperty):
    """PSR.ic is off at entry, set by the only `ssm` (exactly
    `ssm 0x2000`) before the bridge call and cleared by the only `rsm`
    (exactly `rsm 0x2000`) after it, which precedes the only `rfi`. While
    collection is off, every memory access, register-stack operation and
    RSE or UNAT register access is one of the reviewed forms of
    IC_OFF_ALLOWLIST with its exact count (any other access could fault into
    an unrecoverable nested interruption). After `rsm 0x2000` every memory
    access is based on the frame: a load or fill through a register last set
    from the carried frame r4 (directly or by post-increment), or the final
    release `st8.rel [r16]=r0`, which is the last memory access before rfi.
    `flushrs` never appears."""

    def verify(self, code):
        problems = ['%s changes unreviewed PSR bits' % step.describe() for step in code.steps
                    if step.operation.base in ('ssm', 'rsm')
                    and not (step.operation.immediates == (Ia64.PSR_IC,)
                             or (step.operation.base == 'rsm' and step.operation.immediates == (0x8000,)))]
        key_disable = code.unique('rsm 0x8000', 'protection-key disable', problems)
        first_access = code.first(_accesses_memory)
        if (key_disable is None or first_access is None or key_disable.position >= first_access.position
                or code.next(key_disable) is None or code.next(key_disable).text != 'srlz.d'):
            problems.append('protection keys are not disabled and serialized before kernel memory is touched')
        enable = code.unique(_psr_ic('ssm'), 'psr.ic enables (ssm 0x2000)', problems)
        disable = code.unique(_psr_ic('rsm'), 'psr.ic disables (rsm 0x2000)', problems)
        call = code.unique(_is_call, 'calls', problems)
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        if not (enable and disable and call and rfi):
            return problems
        if not enable.position < call.position < disable.position < rfi.position:
            problems.append('psr.ic is not enabled before the call, disabled after it and left by rfi in this order')
        sensitive = Counter(step.rendered for step in code.window(before=enable) + code.window(after=disable)
                            if _accesses_memory(step) or step.operation.base in IC_SENSITIVE_BASES
                            or (step.operation.reads | step.operation.writes) & RSE_REGISTERS)
        problems += Properties.multiset_problems(sensitive, IC_OFF_ALLOWLIST, 'IC-off access')
        accesses = [step for step in code.window(after=disable) if _accesses_memory(step)]
        release = accesses[-1] if accesses else None
        if release is None or release.text != 'st8.rel [r16]=r0' or release.position > rfi.position:
            problems.append('the last memory access after rsm 0x2000 is not the release st8.rel [r16]=r0')
        for step in accesses:
            if step is release or step.position > rfi.position:
                continue
            base = step.operation.memory_base
            writer = _last_writer(code, base, step) if base else None
            if step.operation.memory != 'load' or writer is None or not (
                    writer.reads(CARRIED_FRAME) or writer.operation.memory_base == base):
                problems.append('%s after rsm 0x2000 is not a frame load' % step.describe())
        return problems + ['%s: flushrs is forbidden' % step.describe() for step in code.all('flushrs')]

    def mutants(self, code):
        disable, release = code.site(_psr_ic('rsm')), code.site('st8.rel [r16]=r0')
        root = code.site(lambda step: 'ar.k3' in step.operation.reads)
        yield 'protection keys enabled in the kernel', code.replace(code.site('rsm 0x8000'), text='ssm 0x8000')
        yield 'protection-key disable not serialized', code.nop(code.next(code.site('rsm 0x8000')))
        yield 'unreviewed IC-off load through the root', code.insert(root, 'ld8 r11=[r17]', after=True)
        yield 'alloc while collection is off', code.insert(root, 'alloc r11=ar.pfs,0,0,0', after=True)
        yield 'stack store after rsm psr.ic', code.insert(disable, 'st8 [r12]=r0', after=True)
        yield 'load after the release', code.insert(release, 'ld8 r24=[r4]', after=True)
        yield 'flushrs before the call', code.insert(code.site(_is_call), 'flushrs')
        yield 'psr.ic enable also enables psr.i', code.replace(code.site(_psr_ic('ssm')), text='ssm 0x6000')
        yield 'second rsm', code.insert(release, 'rsm 0x2000', after=True)
        yield 'collection disabled before the call', code.swap(code.site(_psr_ic('rsm')), code.site(_is('mf')))


# The admission before the State claim ----------------------------------------------------------------

CAPTURE_PREFIX = ('mov.m r18=ar.rsc', 'mov.m ar.k6=r18', 'mov r18=12', 'mov.m ar.rsc=r18', 'mov r31=pr', 'mov r30=psr', 'mov r29=cr.iip', 'mov r28=cr.ipsr', 'mov r27=cr.isr',
                  'mov r26=cr.ifa', 'mov r25=cr.ifs')
# The guards of the admission after the selector guard, in their reviewed
# rendering (GAS renders cmp.ne p6,p7 as cmp.eq p7,p6 and cmp.gtu as cmp.ltu
# with swapped operands).
ADMISSION_COMPARES = Counter([
    'cmp.eq p7,p6=r18,r17', 'cmp.eq p7,p6=r18,r17', 'cmp.eq p7,p6=r18,r0',
    'cmp.eq p7,p6=r18,r0', 'cmp.eq p6,p7=3,r17', 'cmp.eq p7,p6=r18,r0',
    'cmp.eq p6,p7=r17,r0', 'cmp.eq p7,p6=r18,r0', 'cmp.ltu p6,p7=r18,r17', 'cmp.eq p6,p7=r24,r0',
    'cmp.eq p7,p6=r18,r0', 'cmp.ltu p6,p7=r18,r24', 'cmp.eq p7,p6=r18,r0', 'cmp.eq p7,p6=r24,r23',
    'cmp.ltu p7,p6=r24,r18', 'cmp.eq p6,p7=r22,r0', 'cmp.eq p7,p6=r18,r0', 'cmp.ltu p6,p7=r18,r22',
    'cmp.eq p6,p7=r21,r0', 'cmp.eq p7,p6=r18,r0', 'cmp.ltu p7,p6=r21,r20', 'cmp.eq p7,p6=r18,r24',
    'cmp.ltu p6,p7=r18,r20', 'cmp.eq p6,p7=r19,r0', 'cmp.eq p7,p6=r18,r0', 'cmp.ltu p7,p6=r19,r17',
    'cmp.eq p7,p6=r18,r24', 'cmp.ltu p6,p7=r18,r17', 'cmp.eq p6,p7=r16,r0', 'cmp.eq p7,p6=r18,r0',
    'cmp.ltu p7,p6=r16,r18', 'cmp.eq p7,p6=r18,r0', 'cmp.eq p7,p6=0,r18'])
# The constants the guards compare against besides the interrupted-PSR mask
# and value: the current-PSR mask and value, the rejected ISR
# fields, enforced-lazy PL3 RSC, the Region-0 extents of the root, context and
# frame, the processor count, the page mask, the storage sizes and the
# Region-0 limit.
ADMISSION_CONSTANTS = Counter([
    'movl r17=0x10080e6000', 'movl r17=0x1008020000',
    'movl r17=0x8000000000', 'mov r18=12', 'movl r18=0x1fffffffffffffc0', 'movl r18=0x1fffffffffffffc0',
    'mov r18=4', 'movl r18=0x1ffffffffffff580', 'mov r23=4095', 'mov r24=16384', 'mov r24=32768',
    'movl r18=0x2000000000000000', 'movl r18=0x2000000000000000', 'movl r18=0x2000000000000000'])
ADMISSION_ARCHITECTURAL = Counter([*(text for text in CAPTURE_PREFIX if text != 'mov r18=12'), 'mov.m r17=ar.k3', 'rsm 0x8000', 'srlz.d'])


def selector_guard(variant: Variant) -> str:
    return 'cmp.eq p7,p6=%d,r16' % variant.selector


def interrupted_value(variant: Variant) -> str:
    return 'movl r17=%#x' % variant.interrupted_value


def admission_compares(variant: Variant) -> Counter:
    return ADMISSION_COMPARES + Counter([selector_guard(variant)])


def interrupted_mask(variant: Variant) -> str:
    return 'movl r17=%#x' % variant.interrupted_mask


def admission_constants(variant: Variant) -> Counter:
    return ADMISSION_CONSTANTS + Counter([interrupted_mask(variant), interrupted_value(variant)])


def _architectural(step: Step) -> bool:
    return step.operation.base in ('alloc', 'bsw', 'cover', 'rfi', 'rsm', 'ssm', 'srlz', 'itc', 'ptc', 'loadrs',
                                   'flushrs', 'invala') or any(
        register.startswith(('ar.', 'cr.', 'psr')) or register in ('pr', 'pr.rot')
        or re.fullmatch(r'[bf]\d+', register) for register in step.operation.reads | step.operation.writes)


@register
class Ia64UserFaultEntryAdmission(_EntryProperty):
    """The prefix preserves PR and original RSC in ar.k6, installs
    enforced-lazy PL3 RSC, then copies PSR and the interruption registers
    unpredicated into bank-zero r30..r25. IA-64 fault delivery,
.
    Before the State claim (the first `st8.rel`), nothing is written but
    bank-zero r16-r31, p6 and p7: no static, stacked, floating or branch
    register, no store; the compares are exactly the entry's selector guard
    plus ADMISSION_COMPARES, each unpredicated and immediately followed by
    `(p6) br.cond.dpnt.few F`, and these are the only control transfers; the
    immediates they compare with are exactly the entry's interrupted-PSR value
    (PSR.i clear for the user-fault entry, set for the user interrupt entry)
    plus ADMISSION_CONSTANTS (the PSR masks, the current-PSR value, the
    rejected ISR fields na/sp/rs/ir/ni, enforced-lazy PL3 RSC 0xc, the
    Region-0 extents, the processor count and the storage sizes); the
    selector guard compares r16 with the entry's selector (4 or 5) before r16
    is written; the only architectural instructions are the capture prefix,
    the ar.rsc read and the ar.k3 read; the context is acquired from offset 40
    of the root and the State with an acquire load compared with Inactive
    (0)."""

    def verify(self, code):
        problems = []
        steps = code.steps
        prefix = [step.rendered for step in steps[:len(CAPTURE_PREFIX)]]
        if prefix != list(CAPTURE_PREFIX):
            problems.append('the capture prefix is %s, expected %s' % (prefix, list(CAPTURE_PREFIX)))
        claimed = claim(code)
        if claimed is None:
            return problems + ['no State claim (st8.rel)']
        window = code.window(before=claimed)
        fail = failure_target(code)
        allowed = Ia64.BANKED_GENERAL | {'p6', 'p7'}
        for step in window:
            written = {r for r in step.operation.writes if re.fullmatch(r'[rbfp]\d+', r)}
            if written - allowed:
                problems.append('%s writes %s before the claim' % (step.describe(), ', '.join(sorted(written - allowed))))
            if step.operation.memory not in (None, 'load'):
                problems.append('%s stores before the claim' % step.describe())
            if step.operation.is_branch and not (_is_rejection(step) and step.predicate == 'p6'
                                                 and step.target == fail and fail is not None):
                problems.append('%s is not an admission rejection' % step.describe())
        compares = [index for index, step in enumerate(window) if step.operation.base == 'cmp']
        problems += Properties.multiset_problems(Counter(window[index].rendered for index in compares),
                                                 admission_compares(self.VARIANT), 'admission compare')
        for index in compares:
            following = window[index + 1] if index + 1 < len(window) else None
            if (window[index].instruction.predicate is not None or following is None
                    or not _is_rejection(following) or following.predicate != 'p6'):
                problems.append('%s is not immediately consumed by (p6) br.cond.dpnt.few' % window[index].describe())
        rejections = [step for step in window if step.operation.is_branch]
        if len(rejections) != len(compares):
            problems.append('%d admission branches for %d compares' % (len(rejections), len(compares)))
        constants = Counter(step.rendered for step in window
                            if step.operation.base in ('mov', 'movl') and step.operation.immediates
                            and not step.operation.reads and len(step.operation.writes) == 1
                            and step.text not in ('mov r18=1', 'mov r23=%d' % self.VARIANT.vector_offset))
        problems += Properties.multiset_problems(constants, admission_constants(self.VARIANT),
                                                 'admission constant')
        problems += Properties.multiset_problems(Counter(step.rendered for step in window if _architectural(step)),
                                                 ADMISSION_ARCHITECTURAL, 'admission architectural instruction')
        selector = code.first(selector_guard(self.VARIANT))
        if selector is None or any(step.writes('r16') for step in code.window(before=selector)):
            problems.append('the selector r16 is not compared with %d before it is written' % self.VARIANT.selector)
        acquires = [step for step in window if _is_acquire(step)]
        if [step.text for step in acquires] != ['ld8.acq r24=[r24]', 'ld8.acq r18=[r18]', 'ld8.acq r18=[r23]']:
            problems.append('the admission acquires are %s' % [step.text for step in acquires])
        else:
            context = _last_writer(code, 'r24', acquires[0])
            state = _last_writer(code, 'r23', acquires[2])
            if context is None or context.text != 'adds r24=40,r17':
                problems.append('the context is not acquired from offset 40 of the ar.k3 root')
            if state is None or state.text != 'adds r23=%d,%s' % (STATE, FRAME):
                problems.append('the State acquire is not of the frame State')
        return problems

    def mutants(self, code):
        root = code.site(lambda step: 'ar.k3' in step.operation.reads)
        claimed = claim(code)
        variant = self.VARIANT
        other = next(candidate for candidate in VARIANTS
                     if (candidate.interrupted_mask, candidate.interrupted_value)
                     != (variant.interrupted_mask, variant.interrupted_value))
        yield 'static register written before the claim', code.insert(root, 'mov r11=r0', after=True)
        yield 'store before the claim', code.insert(root, 'st8 [r17]=r0', after=True)
        yield 'branch-register write before the claim', code.insert(root, 'mov b6=r17', after=True)
        yield 'missing selector guard', code.nop(code.site(selector_guard(variant)))
        yield 'selector guard against %d' % (variant.selector - 1), code.replace(
            code.site(selector_guard(variant)), text='cmp.eq p7,p6=%d,r16' % (variant.selector - 1))
        yield 'IPSR mask without psr.ic', code.replace(code.site(interrupted_mask(variant)),
                                                       text='movl r17=%#x' % (variant.interrupted_mask & ~0x2000))
        yield 'interrupted-PSR mask of %s' % other.entry, code.replace(code.site(interrupted_mask(variant)),
                                                                       text=interrupted_mask(other))
        yield 'interrupted PSR of %s' % other.entry, code.replace(code.site(interrupted_value(variant)),
                                                                  text=interrupted_value(other))
        yield 'nested ISR admitted', code.replace(code.site('movl r17=0x8000000000'),
                                                      text='movl r17=0')
        yield 'Reserved guard removed', code.nop(code.site('ld8 r18=[r18]', 'cmp.eq p7,p6=r18,r0'))
        yield 'ProcessorIndex guard removed', code.nop(code.site('cmp.eq p7,p6=r24,r23'))
        yield 'eager RSC retained in the kernel', code.replace(code.site('mov r18=12'), text='mov r18=15')
        yield 'predicated guard', code.replace(code.site('cmp.eq p6,p7=3,r17'), predicate='p07')
        yield 'context read without acquire', code.replace(code.site('ld8.acq r24=[r24]'), text='ld8 r24=[r24]')
        yield 'context from the system-call word', code.replace(code.site('adds r24=40,r17'),
                                                                text='adds r24=16,r17')
        yield 'claim without the Inactive test', code.nop(code.site('cmp.eq p7,p6=0,r18'))
        yield 'IFS read late', code.swap(code.site('mov r25=cr.ifs'), code.site('mov r26=cr.ifa'))
        yield 'admission branch escapes', code.retarget(code.site(lambda s: s.position < claimed.position,
                                                                  _is_rejection), 'AsteriaIa64SystemCallEntry')


@register
class Ia64UserFaultEntryFailStop(_EntryProperty):
    """Every `br.cond.dpnt.few` (admission, covered-IFS, RSE
    distance, bridge result, committed state and BSP convergence) targets one
    fail-stop F inside the entry, after the only `rfi`; F starts with
    `hint.m 0`, followed by the only unpredicated `br.few F`. A misdirected
    rejection would resume unvalidated state instead of stopping."""

    def verify(self, code):
        targets = Counter(step.target for step in code.all(_is_rejection))
        if len(targets) != 1 or None in targets:
            return ['rejections target %s, expected one fail-stop inside the entry'
                    % ', '.join('%s ×%d' % ('outside' if t is None else '+%#x' % t, n) for t, n in targets.items())]
        fail = next(iter(targets))
        hint = code.at_offset(fail)
        if hint is None or hint.text != 'hint.m 0' or hint.instruction.predicate is not None:
            return ['the fail-stop at +%#x does not start with hint.m 0' % fail]
        problems = []
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        if rfi is not None and hint.position < rfi.position:
            problems.append('the fail-stop precedes rfi')
        loop = code.next(hint)
        if loop is None or not Ia64StopViews.terminal(loop) or loop.instruction.predicate:
            problems.append('hint.m 0 at the fail-stop is not followed by br.few to itself')
        loops = code.all(lambda step: Ia64StopViews.terminal(step))
        return problems + (['%d br.few, expected only the fail-stop loop' % len(loops)] if len(loops) != 1 else [])

    def mutants(self, code):
        call = code.site(_is_call)
        result = code.site(lambda step: step is call, _is_rejection)
        yield 'bridge failure resumes', code.retarget(result, code.site(_is('rfi')).bundle)
        yield 'fail-stop loop escapes', code.retarget(code.site(lambda step: Ia64StopViews.terminal(step)), 0)
        yield 'fail-stop without its pause', code.nop(code.site(_is('hint')))
        yield 'misdirected admission rejection', code.retarget(code.site(_is_rejection), 0)


@register
class Ia64UserFaultEntryBridgeCall(_EntryProperty):
    """The entry has exactly one call, an unpredicated `br.call b0=`
    to `AsteriaIa64UserFaultBridge`, made in this order: the thread's stack
    `adds r12=-16,r20` (r20 the context's MemoryStackEnd), the only
    two-output frame `alloc r24=ar.pfs,2,0,0` (no other alloc before the
    call), `mov r4=r22` (the callee-saved frame), out0 `mov r32=r4`, out1
    `mov r33=r16` (the context's HandlerAddress), `ssm 0x2000`, `bsw.1`,
    `srlz.d`, the only `movl r1=` (the kernel GP) and the call. r22 and r16
    flow unchanged from their context loads `ld8 r22=[r18],8` and
    `ld8 r16=[r18],8`, r20 from `ld8 r20=[r18],8`; r4, r32, r33, r1 and r12
    are not written between their setup and the call. The call's result r8
    is tested first after it (`cmp.eq p7,p6=r8,r0`, `(p6)` rejection)."""
    ORDER = ('adds r12=-16,r20', 'alloc r24=ar.pfs,2,0,0', 'mov r4=r22', 'mov r32=r4', 'mov r33=r16', 'ssm 0x2000',
             'bsw.1', 'srlz.d')

    def verify(self, code):
        problems = []
        call = code.unique(_is_call, 'calls', problems)
        gp = code.unique(lambda step: step.operation.base == 'movl' and step.writes('r1'), 'movl r1= loads', problems)
        if call is None or gp is None:
            return problems
        target = call.operation.target
        if (call.instruction.predicate is not None or 'b0' not in call.operation.writes or target is None
                or (target.symbol, target.offset) != (BRIDGE, 0)):
            problems.append('%s is not an unpredicated br.call b0= to %s' % (call.describe(), BRIDGE))
        if gp.instruction.predicate is not None:
            problems.append('the GP load is predicated')
        chain = []
        for text in self.ORDER:
            found = code.all(text, before=call)
            if not found:
                return problems + ['no %s before the call' % text]
            chain.append(found[-1])
        chain += [gp, call]
        if [step.position for step in chain] != sorted(step.position for step in chain):
            problems.append('the bridge transition is out of order: %s' % ' < '.join(step.text for step in chain))
        allocs = code.all(_is('alloc'), before=call)
        if [step.text for step in allocs] != ['alloc r24=ar.pfs,2,0,0']:
            problems.append('allocs before the call: %s' % [step.text for step in allocs])
        stack, _, frame, out0, out1 = chain[:5]
        for register, load in (('r22', 'ld8 r22=[r18],8'), ('r16', 'ld8 r16=[r18],8'), ('r20', 'ld8 r20=[r18],8')):
            source = code.first(load)
            consumer = {'r22': frame, 'r16': out1, 'r20': stack}[register]
            if source is None or source.position > consumer.position:
                problems.append('%s is not loaded from the context before its use' % register)
                continue
            problems += ['%s writes %s between its context load and its use' % (step.describe(), register)
                         for step in writes_between(code, register, source, consumer)]
        for register, setup in (('r4', frame), ('r32', out0), ('r33', out1), ('r1', gp), ('r12', stack)):
            problems += ['%s writes %s between its setup and the call' % (step.describe(), register)
                         for step in writes_between(code, register, setup, call)]
        result = code.next(call)
        rejection = result and code.next(result)
        if (result is None or result.text != 'cmp.eq p7,p6=r8,r0' or rejection is None
                or not _is_rejection(rejection) or rejection.predicate != 'p6'):
            problems.append('the bridge result r8 is not tested first after the call')
        return problems

    def mutants(self, code):
        call = code.site(_is_call)
        for text, replacement in (('mov r32=r4', 'mov r32=r22'), ('mov r33=r16', 'mov r33=r17'),
                                  ('adds r12=-16,r20', 'adds r12=-16,r21'), ('alloc r24=ar.pfs,2,0,0',
                                                                             'alloc r24=ar.pfs,1,0,0')):
            yield '%s instead of %s' % (replacement, text), code.replace(code.site(text), text=replacement)
        yield 'handler clobbered before the call', code.insert(code.site('mov r33=r16'), 'mov r33=r0', after=True)
        yield 'frame clobbered after its load', code.insert(code.site('ld8 r22=[r18],8'), 'mov r22=r0', after=True)
        yield 'r4 clobbered before the call', code.insert(code.site('mov r32=r4'), 'mov r4=r0', after=True)
        yield 'collection left off for the call', code.nop(code.site('ssm 0x2000'))
        yield 'bank zero kept for the call', code.nop(code.site(lambda step: step is code.site('ssm 0x2000'), 'bsw.1'))
        yield 'GP clobbered before the call', code.insert(call, 'mov r1=r0')
        yield 'call to another function', code.replace(call, text=call.text.replace(BRIDGE, 'AsteriaWrongBridge'))
        yield 'bridge result ignored', code.nop(code.site(lambda step: step is call, 'cmp.eq p7,p6=r8,r0'))
        yield 'second call', code.insert(call, call.text, after=True)


def _spill_order(code: Code, texts: list[str], after: Step | None, before: Step | None) -> list[str]:
    problems = []
    found = [code.all(text, after=after, before=before) for text in texts]
    for text, steps in zip(texts, found):
        if len(steps) != 1:
            problems.append('%d %s, expected one' % (len(steps), text))
    if not problems:
        positions = [steps[0].position for steps in found]
        if positions != sorted(positions):
            problems.append('%s are out of order' % texts[0].split()[0])
    return problems


@register
class Ia64UserFaultEntryCompleteImage(_EntryProperty):
    """The capture and restore move the complete PL3 image. Before
    the call: static r1..r15 are each spilled once with `st8.spill` (in
    order, after `mov.m ar.unat=r0`) before any write of them, and the spill
    collection is read and stored; bank-one r16..r31 are each spilled once
    between the only `bsw.1` and `bsw.0` before the call, with their own
    collection; f2..f127 are each spilled once with `stf.spill`, after the
    only `cover` (with the rename bases reset), and no floating register is
    otherwise touched before the call; every application register of
    APPLICATION_REGISTERS, b0..b7 and the interruption registers
    cr.iip/ipsr/isr/ifa/ifs/itir/iipa/iim are read; PR is read before cover
    and again after it, and the frame's PR is the `dep` of the entry sample's
    p0-p15 into the post-cover read. After the call, every f2..f127 is
    filled once with `ldf.fill`, bank-one r16..r31 once each after the last
    `bsw.1`, static r1..r15 once each after those, and the application
    registers of RESTORED_APPLICATION_REGISTERS, b0..b7 and PR are each
    written."""

    def verify(self, code):
        problems = []
        call = code.unique(_is_call, 'calls', problems)
        cover = code.unique(_is('cover'), 'cover', problems)
        if call is None or cover is None:
            return problems
        # The first bsw.1 opens the bank-one capture; the second is the
        # bridge transition's.
        banks = code.all('bsw.1', before=call)
        if len(banks) != 2:
            return problems + ['%d bsw.1 before the call, expected the capture and the bridge transition'
                               % len(banks)]
        return problems + self._capture(code, call, cover, banks[0])

    def _capture(self, code, call, cover, bank):
        problems = _spill_order(code, ['st8.spill [r18]=%s%s' % (r, ',8' if r != 'r15' else '')
                                       for r in STATIC_REGISTERS], None, call)
        first = code.first(lambda step: step.text.startswith('st8.spill [r18]=r1,'))
        clear = first and code.all('mov.m ar.unat=r0', before=first)
        if not clear:
            problems.append('the static spills do not start with a clear ar.unat')
        for register in STATIC_REGISTERS:
            spill = code.first(lambda step, r=register: step.text.startswith('st8.spill [r18]=%s' % r)
                               and (step.text.endswith(r) or step.text.endswith(r + ',8')))
            writes = [step for step in code.window(before=call) if step.writes(register)
                      and not (step.operation.memory == 'store' and register == 'r18')]
            if spill is not None and any(step.position < spill.position for step in writes):
                problems.append('%s is written before its spill' % register)
        back = code.first(lambda step: step is bank, 'bsw.0')
        if back is None or back.position > call.position:
            return problems + ['no bsw.0 closes the bank-one capture']
        problems += _spill_order(code, ['st8.spill [r14]=%s%s' % (r, ',8' if r != 'r31' else '')
                                        for r in BANKED_REGISTERS], bank, back)
        problems += _spill_order(code, ['stf.spill [r14]=%s%s' % (f, ',16' if f != 'f127' else '')
                                        for f in FLOATING_REGISTERS], cover, call)
        floating = [step for step in code.window(before=call) if any(re.fullmatch(r'f\d+', r) for r in
                                                                     step.operation.reads | step.operation.writes)]
        problems += ['%s touches a floating register outside its spill' % step.describe() for step in floating
                     if step.operation.base != 'stf' or step.position < cover.position]
        reads = set().union(*(step.operation.reads for step in code.window(before=call)))
        problems += ['%s is never captured' % register for register in
                     APPLICATION_REGISTERS + BRANCH_REGISTERS + INTERRUPTION_REGISTERS if register not in reads]
        predicate_reads = code.all('mov r31=pr', before=cover) + code.all(lambda step: step.operation.reads == {'pr'},
                                                                          after=cover, before=call)
        if len(predicate_reads) != 2:
            problems.append('PR is not read once before and once after cover')
        else:
            merge = code.first(lambda step: step is predicate_reads[1], _is('dep'))
            post = Ia64.single_write(predicate_reads[1].operation)
            if merge is None or merge.text != 'dep r31=r31,%s,0,16' % post:
                problems.append('the frame PR is not p0-p15 of the entry sample over the post-cover read')
        return problems + self._restore(code, call)

    def _restore(self, code, call):
        problems = _spill_order(code, ['ldf.fill %s=[r14]%s' % (f, ',16' if f != 'f127' else '')
                                       for f in FLOATING_REGISTERS], call, None)
        bank = code.all('bsw.1', after=call)
        if len(bank) != 1:
            return problems + ['%d bsw.1 after the call, expected the bank-one restore' % len(bank)]
        problems += _spill_order(code, ['ld8.fill %s=[r14]%s' % (r, ',8' if r != 'r31' else '')
                                        for r in BANKED_REGISTERS], bank[0], None)
        last_banked = code.first('ld8.fill r31=[r14]')
        problems += _spill_order(code, ['ld8.fill %s=[r15]%s' % (r, ',8' if r != 'r15' else '')
                                        for r in STATIC_REGISTERS], last_banked, None)
        writes = set().union(*(step.operation.writes for step in code.window(after=call)))
        return problems + ['%s is never restored' % register for register in
                           RESTORED_APPLICATION_REGISTERS + BRANCH_REGISTERS + ('pr',) if register not in writes]

    def mutants(self, code):
        call, cover = code.site(_is_call), code.site(_is('cover'))
        yield 'r7 not spilled', code.nop(code.site('st8.spill [r18]=r7,8'))
        yield 'r23 of bank one not spilled', code.nop(code.site('st8.spill [r14]=r23,8'))
        yield 'f64 not spilled', code.nop(code.site('stf.spill [r14]=f64,16'))
        yield 'f40 spilled before cover', code.nop(code.site('stf.spill [r14]=f40,16')).insert(
            cover, 'stf.spill [r14]=f40,16')
        yield 'f100 not filled', code.nop(code.site('ldf.fill f100=[r14],16'))
        yield 'r20 of bank one not filled', code.nop(code.site('ld8.fill r20=[r14],8'))
        yield 'r9 not filled', code.nop(code.site('ld8.fill r9=[r15],8'))
        yield 'ar.csd not captured', code.nop(code.site('mov.m r23=ar.csd'))
        yield 'ar.ec not restored', code.nop(code.site(lambda step: step is call, lambda s: s.writes('ar.ec')))
        yield 'b5 not restored', code.nop(code.site(lambda step: step is call, lambda s: s.writes('b5')))
        yield 'r12 clobbered before its spill', code.insert(code.site('mov r31=pr'), 'mov r12=r0', after=True)
        yield 'rotating predicates from the entry sample', code.replace(code.site(_is('dep')),
                                                                        text='dep r31=r31,r31,0,16')
        yield 'post-cover PR read removed', code.nop(code.site(lambda step: step is cover,
                                                               lambda s: s.operation.reads == {'pr'}))
        yield 'floating arithmetic before the call', code.insert(call, 'fadd f6=f6,f6')


SWITCH_ORDER = ('cover', 'mov r23=cr.ifs', 'invala', 'mov.m ar.rsc=r0', 'mov.m r23=ar.bspstore', 'mov.m r24=ar.rnat',
                'mov.m ar.bspstore=r19', 'mov.m ar.rnat=r0', 'mov.m r23=ar.bsp', 'mov.m ar.rsc=r24',
                'adds r12=-16,r20')
RESTORE_ORDER = ('mov.m ar.rsc=r16', 'loadrs', 'invala', 'mov.m ar.bspstore=r17', 'mov.m ar.rnat=r18',
                 'mov.m r16=ar.bsp')


@dataclasses.dataclass(frozen=True)
class RseFacts:
    code: Code
    count: int | None       # Ia64PhysicalStackedRegisterCount


@register
class Ia64UserFaultEntryRegisterStack(_EntryProperty):
    """Before the call, after the static and bank-one capture, the
    register stack switches in the architected order of Intel SDM Vol. 2
    section 6.11.1, each step exactly once: `cover` < the covered IFS read
    `mov r23=cr.ifs` < `invala` < `mov.m ar.rsc=r0` < BSPSTORE and RNAT
    reads < `mov.m ar.bspstore=r19` (r19 the context's RseBackingStoreStart)
    < `mov.m ar.rnat=r0` < `mov.m r23=ar.bsp` < `mov.m ar.rsc=r24` (kernel
    RSC 3) < `adds r12=-16,r20`. After the call (section 6.11.2):
    `mov.m ar.rsc=r16` (the stored loadrs configuration) < the stacked-
    register scrub (Properties.stacked_register_scrub over
    Ia64PhysicalStackedRegisterCount registers) < `loadrs` < `invala` <
    `mov.m ar.bspstore=r17` < `mov.m ar.rnat=r18` < the BSP convergence read
    `mov.m r16=ar.bsp`, compared with the post-cover BSP. `flushrs` never
    appears, and no other write of ar.bspstore exists."""

    def extract(self, context, subject):
        return RseFacts(super().extract(context, subject), Properties.physical_stacked_register_count(context.root))

    def verify(self, facts):
        if facts.count is None:
            return ['%s no longer states %s' % (Properties.PHYSICAL_STACKED_HEADER,
                                               Properties.PHYSICAL_STACKED_CONSTANT)]
        code, problems = facts.code, []
        call = code.unique(_is_call, 'calls', problems)
        if call is None:
            return problems
        cover = code.unique('cover', 'cover', problems, before=call)
        if cover is None:
            return problems
        switch = [cover] + [code.unique(text, 'RSE switch step %s' % text, problems, after=cover, before=call)
                            for text in SWITCH_ORDER[1:]]
        restore = [code.unique(text, 'RSE restore step %s' % text, problems, after=call) for text in RESTORE_ORDER]
        if None in switch or None in restore:
            return problems
        for steps, what in ((switch, 'switch'), (restore, 'restore')):
            if [step.position for step in steps] != sorted(step.position for step in steps):
                problems.append('the RSE %s is out of order: %s' % (what, ' < '.join(
                    step.text for step in sorted(steps, key=lambda s: s.position))))
        bank = code.first('bsw.0')
        if bank is None or bank.position > switch[0].position:
            problems.append('cover precedes the bank-one capture')
        found, sites = Properties.stacked_register_scrub(code, restore[0], restore[1], facts.count)
        problems += found
        writes = code.all(lambda step: step.writes('ar.bspstore'))
        if len(writes) != 2:
            problems.append('%d ar.bspstore writes, expected the switch and the restore' % len(writes))
        return problems + ['%s: flushrs is forbidden' % step.describe() for step in code.all('flushrs')]

    def mutants(self, facts):
        code = facts.code
        call = code.site(_is_call)
        loadrs = code.site(lambda step: step is call, 'loadrs')

        def edited(changed):
            return dataclasses.replace(facts, code=changed)

        lazy = code.site(lambda step: step is call, 'mov.m ar.rsc=r16')
        _, (grow, drop) = Properties.stacked_register_scrub(code, lazy, loadrs, facts.count)
        for description, changed in Properties.stacked_register_scrub_mutants(code, grow, drop, facts.count):
            yield description, edited(changed)
        yield 'loadrs after the BSPSTORE restore', edited(code.swap(loadrs, code.site(
            lambda step: step is call, 'mov.m ar.bspstore=r17')))
        yield 'RSC quiesced after the BSPSTORE switch', edited(code.swap(code.site('mov.m ar.rsc=r0'),
                                                                         code.site('mov.m ar.bspstore=r19')))
        yield 'cover after the IFS read', edited(code.swap(code.site('cover'), code.site('mov r23=cr.ifs')))
        yield 'flushrs before the switch', edited(code.insert(code.site('invala'), 'flushrs'))
        yield 'no BSP convergence check', edited(code.nop(code.site('mov.m r16=ar.bsp')))
        yield 'a physical file larger than the frame', dataclasses.replace(facts, count=facts.count + 8)


@register
class Ia64UserFaultEntryNoSlotAdvance(_EntryProperty):
    """The interrupted slot runs: the faulting instruction again, or
    the instruction an interrupt preceded. cr.iip, cr.ipsr and
    cr.ifs are each written exactly once, after `rsm 0x2000`, and each value
    is exactly the frame word, unmodified: cr.iip from `ld8 X=[A],8` and
    cr.ipsr from the following `ld8 Y=[A]` with A last set by
    `adds A=64,r4` (InterruptedInstructionPointer, then
    InterruptedProcessorStatus), cr.ifs from `ld8 Z=[B]` with B last set by
    `adds B=88,r4` (the covered IFS, not the hardware IFS at 80), and no
    instruction writes X, Y or Z between the load and the control-register
    write. These writes lie in instruction groups before rfi."""

    def verify(self, code):
        problems = []
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        disable = code.unique(_psr_ic('rsm'), 'rsm 0x2000', problems)
        if rfi is None or disable is None:
            return problems
        for control, offset, increment in (('cr.iip', IIP, True), ('cr.ipsr', IIP, False),
                                           ('cr.ifs', COVERED_IFS, False)):
            write = code.unique(lambda step, c=control: step.writes(c), '%s writes' % control, problems)
            if write is None:
                continue
            if not disable.position < write.position < rfi.position or write.group >= rfi.group:
                problems.append('%s is not written after rsm 0x2000 in a group before rfi' % control)
            value = Ia64.single_read(write.operation)
            load = value and _last_writer(code, value, write)
            if load is None or load.operation.memory != 'load' or load.mnemonic != 'ld8' or load.predicate:
                problems.append('%s is not written from a plain frame load' % control)
                continue
            base = load.operation.memory_base
            if increment != (base in load.operation.writes):
                problems.append('%s is not the %s frame word' % (control, 'first' if increment else 'second'))
                continue
            address = _last_writer(code, base, load)
            if control == 'cr.ipsr' and address is not None and address.operation.memory == 'load':
                address = _last_writer(code, base, address)
            if address is None or address.text != 'adds %s=%d,%s' % (base, offset, CARRIED_FRAME):
                problems.append('%s is not loaded from frame offset %d' % (control, offset))
        return problems

    def mutants(self, code):
        disable = code.site(_psr_ic('rsm'))
        iip = code.site(lambda step: step.writes('cr.iip'))
        ipsr = code.site(lambda step: step.writes('cr.ipsr'))
        yield 'slot advanced', code.insert(ipsr, 'dep r18=r0,r18,41,2')
        yield 'bundle advanced', code.insert(iip, 'adds r17=16,r17')
        yield 'hardware IFS restored', code.replace(code.site(lambda step: step is disable, 'adds r24=88,r4'),
                                                    text='adds r24=80,r4')
        yield 'IIP written from the IPSR word', code.replace(iip, text='mov cr.iip=r18')
        yield 'IPSR written from the IIP word', code.replace(ipsr, text='mov cr.ipsr=r17')
        yield 'IIP from another frame word', code.replace(code.site(lambda step: step is disable, 'adds r24=64,r4'),
                                                          text='adds r24=120,r4')
        yield 'cr.ifs written twice', code.insert(iip, 'mov cr.ifs=r19')


@register
class Ia64UserFaultEntryFrameState(_EntryProperty):
    """The frame lifecycle: after the admission the entry
    release-stores Capturing (1) through `adds r23=24,r22` before any other
    store; after the bridge it acquires the State through `adds r14=24,r4`
    and rejects it unless it is ResumeCommitted (3); the final release
    `st8.rel [r16]=r0` (Inactive) goes through `adds r16=24,r4`, follows
    every fill and frame load, and is the last memory access, followed only
    by register, application and control-register writes and `rfi`."""

    def verify(self, code):
        problems = []
        call = code.unique(_is_call, 'calls', problems)
        rfi = code.unique(_is('rfi'), 'rfi', problems)
        if call is None or rfi is None:
            return problems
        releases = code.all(_is_release)
        if [step.text for step in releases] != ['st8.rel [r23]=r18', 'st8.rel [r16]=r0']:
            return problems + ['the release stores are %s' % [step.text for step in releases]]
        captured, inactive = releases
        stores = code.all(lambda step: step.operation.memory == 'store', before=captured)
        if stores:
            problems.append('%s stores before the Capturing claim' % stores[0].describe())
        for register, step, text in (('r18', captured, 'mov r18=1'), ('r23', captured, 'adds r23=24,r22'),
                                     ('r16', inactive, 'adds r16=24,r4')):
            writer = _last_writer(code, register, step)
            if writer is None or writer.text != text:
                problems.append('%s of %s does not come from %s' % (register, step.text, text))
        committed = code.first(lambda step: step is call, _is_acquire)
        if committed is None or committed.text != 'ld8.acq r15=[r14]' or (
                _last_writer(code, 'r14', committed) or committed).text != 'adds r14=24,r4':
            problems.append('the State is not acquired after the bridge')
        else:
            test = code.next(committed)
            rejection = test and code.next(test)
            if (test is None or test.text != 'cmp.eq p7,p6=3,r15' or rejection is None
                    or not _is_rejection(rejection) or rejection.predicate != 'p6'):
                problems.append('the acquired State is not required to be ResumeCommitted')
        later = [step for step in code.window(after=inactive, before=rfi)
                 if _accesses_memory(step) or step.operation.is_branch]
        problems += ['%s follows the final release' % step.describe() for step in later]
        loads = code.all(lambda step: step.operation.memory == 'load', after=call)
        if loads and loads[-1].position > inactive.position:
            problems.append('%s reads the frame after its release' % loads[-1].describe())
        return problems

    def mutants(self, code):
        call = code.site(_is_call)
        inactive = code.site('st8.rel [r16]=r0')
        yield 'Capturing published as Retained', code.replace(code.site('mov r18=1'), text='mov r18=2')
        yield 'commit not checked', code.nop(code.site('cmp.eq p7,p6=3,r15'))
        yield 'commit checked as Retained', code.replace(code.site('cmp.eq p7,p6=3,r15'), text='cmp.eq p7,p6=2,r15')
        yield 'State released early', code.nop(inactive).insert(code.site(lambda step: step is call, 'bsw.1'),
                                                                'st8.rel [r16]=r0')
        yield 'State released through another word', code.replace(code.site('adds r16=24,r4'),
                                                                  text='adds r16=16,r4')
        yield 'branch after the release', code.insert(inactive, 'br.cond.sptk.few <+0x0>', after=True)
        yield 'release as a plain store', code.replace(inactive, text='st8 [r16]=r0')


@register
class Ia64UserFaultEntryVectorOffset(_EntryProperty):
    """The bridge hands a frame to the fault decoder or to the
    interrupt handler by its VectorOffset, so each entry records its own:
    exactly one store reaches the frame word at offset 48, `st8 [A]=V` with A
    last set by `adds A=48,r22`, between the Capturing claim and the bridge
    call, and V is last set by `mov V=N` with N the entry's vector offset
    (0x1000, the Alternate Data TLB vector, for the user-fault entry). The
    user exception entry serves the General Exception, Floating-point Fault
    and Floating-point Trap vectors: V is last set by
    `shl V=V,8` of `mov.m V=ar.k4`, the vector offset its trampoline left
    there, which PL3 cannot write. The user interrupt entry serves the timer
    and the device vectors: V is last set by
    `mov.m V=ar.k5`, the External Interrupt vector offset 0x3000, with a
    device vector in its low byte, that the claim left there."""

    def verify(self, code):
        problems = []
        call = code.unique(_is_call, 'calls', problems)
        claimed = claim(code)
        if call is None or claimed is None:
            return problems + ([] if claimed else ['no State claim (st8.rel)'])
        stores = []
        for step in code.all(lambda candidate: candidate.operation.memory == 'store'):
            base = step.operation.memory_base
            address = _last_writer(code, base, step) if base else None
            if address is not None and address.text == 'adds %s=%d,%s' % (base, VECTOR_OFFSET, FRAME):
                stores.append(step)
        if len(stores) != 1:
            return problems + ['%d stores to the frame VectorOffset, expected one' % len(stores)]
        store = stores[0]
        if not claimed.position < store.position < call.position:
            problems.append('%s is not between the Capturing claim and the call' % store.describe())
        values = store.operation.reads - {store.operation.memory_base}
        register = next(iter(values)) if len(values) == 1 else None
        value = _last_writer(code, register, store) if register else None
        if self.VARIANT is EXCEPTION:
            source = _last_writer(code, register, value) if value is not None else None
            if (value is None or value.rendered != 'shl %s=%s,8' % (register, register) or source is None
                    or source.rendered != 'mov.m %s=ar.k4' % register):
                problems.append('the frame VectorOffset is not the vector offset ar.k4 holds')
            return problems
        if self.VARIANT is INTERRUPT:
            if value is None or value.rendered != 'mov.m %s=ar.k5' % register:
                problems.append('the frame VectorOffset is not the vector offset ar.k5 holds')
            return problems
        expected = 'mov %s=%d' % (register, self.VARIANT.vector_offset) if register else None
        if value is None or value.rendered != expected:
            problems.append('the frame VectorOffset is not %#x' % self.VARIANT.vector_offset)
        return problems

    def mutants(self, code):
        other = next(candidate for candidate in VARIANTS if candidate.vector_offset != self.VARIANT.vector_offset)
        address = code.site('adds r18=%d,%s' % (VECTOR_OFFSET, FRAME))
        if self.VARIANT is EXCEPTION:
            yield 'vector offset fixed at %#x' % EXCEPTION.vector_offset, code.replace(
                code.site('shl r23=r23,8'), text='mov r23=%d' % EXCEPTION.vector_offset)
            yield 'vector offset unshifted', code.nop(code.site('shl r23=r23,8'))
            yield 'vector offset from another kernel register', code.replace(code.site('mov.m r23=ar.k4'),
                                                                             text='mov.m r23=ar.k5')
        elif self.VARIANT is INTERRUPT:
            yield 'vector offset fixed at %#x' % INTERRUPT.vector_offset, code.replace(
                code.site('mov.m r23=ar.k5'), text='mov r23=%d' % INTERRUPT.vector_offset)
            yield 'vector offset from the exception kernel register', code.replace(
                code.site('mov.m r23=ar.k5'), text='mov.m r23=ar.k4')
        else:
            offset = code.site('mov r23=%d' % self.VARIANT.vector_offset)
            yield 'vector offset of %s' % other.entry, code.replace(offset, text='mov r23=%d' % other.vector_offset)
        yield 'vector offset stored one word early', code.replace(address, text='adds r18=%d,%s' % (
            VECTOR_OFFSET - 8, FRAME))
        yield 'vector offset store removed', code.nop(code.site(lambda step: step is address, 'st8 [r18]=r23'))
        yield 'vector offset taken from the IPSR copy', code.replace(
            code.site(lambda step: step is address, 'st8 [r18]=r23'), text='st8 [r18]=r28')


FIRST_IN_GROUP = frozenset({'alloc', 'loadrs', 'flushrs'})
LAST_IN_GROUP = frozenset({'cover', 'bsw', 'rfi'})
RFI_STATE = frozenset({'cr.ipsr', 'cr.iip', 'cr.ifs'})


@register
class Ia64UserFaultEntryGroupPlacement(_EntryProperty):
    """The architected instruction-group rules: `alloc`, `loadrs`
    and `flushrs` start an instruction group; `cover`, `bsw` and `rfi` end
    one; every PSR write (`ssm`/`rsm`) is followed by `srlz.d` in a later
    group before any dependent instruction (P-SERIALIZE); and the
    cr.ipsr/cr.iip/cr.ifs writes that rfi consumes lie in earlier groups than
    the rfi."""

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
        for description, step, shift in (('loadrs inside a group', code.site(lambda s: s is call, 'loadrs'), 1),
                                         ('alloc inside a group', code.site('alloc r24=ar.pfs,2,0,0'), 1),
                                         ('cover inside a group', code.site('cover'), 0),
                                         ('rfi inside a group', code.site('rfi'), 0),
                                         ('bsw.0 inside a group', code.site(lambda s: s is call, 'bsw.0'), 0)):
            yield description, code.without_stops(step.position - shift, step.position - shift + 1)
        rsm = code.site(_psr_ic('rsm'))
        yield 'srlz.d in the rsm group', code.without_stops(rsm, code.site(lambda s: s is rsm, 'srlz.d'))
        yield 'cr.iip written in the rfi group', code.without_stops(code.site(lambda s: s.writes('cr.iip')),
                                                                     code.site('rfi'))


@dataclasses.dataclass(frozen=True)
class GlobalPointerFacts:
    code: Code
    values: dict                                    # (address, slot) -> movl immediate or call target
    global_pointer: tuple[tuple[int, int],...]     # (value, section index) of each __gp definition
    bridge: int | None

    def value(self, step: Step) -> int | None:
        return self.values.get((step.instruction.address, step.instruction.slot))


@register
class Ia64UserFaultEntryGlobalPointer(_EntryProperty):
    """In every linked image that contains the entry, `__gp` is one
    absolute symbol, the entry's only `movl r1=` loads exactly its value
    (decoded from the image bytes), and its only call resolves to
    `AsteriaIa64UserFaultBridge`: the bridge addresses globals through the
    image's final GP."""

    def subjects(self, context):
        return Subjects.artifacts(context, self.SYMBOL, objects=False, images=True)

    def extract(self, context, subject):
        elf, code = load(subject.path), super().extract(context, subject)
        values = Properties.resolved_values(elf, elf.symbol(self.SYMBOL).section_index, code.instructions)
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
        loads = facts.code.all(lambda step: step.operation.base == 'movl' and step.writes('r1'))
        calls = facts.code.all(_is_call)
        if len(loads) != 1 or len(calls) != 1:
            return problems + ['%d GP loads and %d calls, expected one each' % (len(loads), len(calls))]
        if facts.value(loads[0]) != value:
            problems.append('movl r1= does not load __gp')
        if facts.bridge is None or facts.value(calls[0]) != facts.bridge:
            problems.append('the call does not resolve to %s' % BRIDGE)
        return problems

    def mutants(self, facts):
        load, call = (facts.code.site(lambda step: step.operation.base == 'movl' and step.writes('r1')),
                      facts.code.site(_is_call))

        def with_value(step, value):
            key = step.instruction.address, step.instruction.slot
            return dataclasses.replace(facts, values={**facts.values, key: value})

        yield 'wrong fixed GP', with_value(load, 0)
        yield 'call resolves elsewhere', with_value(call, (facts.bridge or 0) + 0x10)
        yield '__gp relative to a section', dataclasses.replace(
            facts, global_pointer=((facts.global_pointer[0][0], 1),))


# The user interrupt entry --------------------------------------------------------------------------------
# Each property above, judging AsteriaIa64UserInterruptEntry under its Variant.

@register
class Ia64UserInterruptEntryRelocations(Ia64UserFaultEntryRelocations):
    """Relocation targets and bundle slots of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryCollection(Ia64UserFaultEntryCollection):
    """Interrupt collection and IC-off accesses of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryAdmission(Ia64UserFaultEntryAdmission):
    """Admission of the user interrupt entry: selector 5 and the interrupted PSR
    with PSR.i set."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryFailStop(Ia64UserFaultEntryFailStop):
    """Rejection branches and fail-stop tail of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryBridgeCall(Ia64UserFaultEntryBridgeCall):
    """Bridge call and bank transition of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryCompleteImage(Ia64UserFaultEntryCompleteImage):
    """Complete register capture and restore of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryRegisterStack(Ia64UserFaultEntryRegisterStack):
    """Register-stack capture and restore of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryNoSlotAdvance(Ia64UserFaultEntryNoSlotAdvance):
    """Interrupted instruction-slot preservation of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryFrameState(Ia64UserFaultEntryFrameState):
    """Frame claim, publication and release of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryGroupPlacement(Ia64UserFaultEntryGroupPlacement):
    """Instruction-group placement of the user interrupt entry."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryVectorOffset(Ia64UserFaultEntryVectorOffset):
    """Vector offset of the user interrupt entry, read from ar.k5."""
    VARIANT = INTERRUPT


@register
class Ia64UserInterruptEntryGlobalPointer(Ia64UserFaultEntryGlobalPointer):
    """Resolved global pointer and bridge address of the user interrupt entry."""
    VARIANT = INTERRUPT


# The user exception entry --------------------------------------------------------------------------------
# Each property above, judging AsteriaIa64UserExceptionEntry under its Variant.

@register
class Ia64UserExceptionEntryRelocations(Ia64UserFaultEntryRelocations):
    """Relocation targets and bundle slots of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryCollection(Ia64UserFaultEntryCollection):
    """Interrupt collection and IC-off accesses of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryAdmission(Ia64UserFaultEntryAdmission):
    """Admission of the user exception entry: selector 6 and the user-fault
    entry's interrupted PSR (PSR.i either)."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryFailStop(Ia64UserFaultEntryFailStop):
    """Rejection branches and fail-stop tail of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryBridgeCall(Ia64UserFaultEntryBridgeCall):
    """Bridge call and bank transition of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryCompleteImage(Ia64UserFaultEntryCompleteImage):
    """Complete register capture and restore of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryRegisterStack(Ia64UserFaultEntryRegisterStack):
    """Register-stack capture and restore of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryNoSlotAdvance(Ia64UserFaultEntryNoSlotAdvance):
    """Interrupted instruction-slot preservation of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryFrameState(Ia64UserFaultEntryFrameState):
    """Frame claim, publication and release of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryGroupPlacement(Ia64UserFaultEntryGroupPlacement):
    """Instruction-group placement of the user exception entry."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryVectorOffset(Ia64UserFaultEntryVectorOffset):
    """Vector offset of the user exception entry: 0x5400."""
    VARIANT = EXCEPTION


@register
class Ia64UserExceptionEntryGlobalPointer(Ia64UserFaultEntryGlobalPointer):
    """Resolved global pointer and bridge address of the user exception entry."""
    VARIANT = EXCEPTION
