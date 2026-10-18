# SPDX-License-Identifier: Apache-2.0
"""IA-64 scheduler, AP-runtime, local-timer, kernel-context and IPI asm helpers.

Checked code: the hand-written asm of the `.S` files below (helpers are
located by symbol), byte-identical at O0, O2 and in the firmware build.

- `Time/Ia64LocalTimer.S`: the ITC read and the local-timer initialize,
  program and cancel helpers.
- `Scheduler/Ia64ApplicationProcessorKernelRuntime.S`: the AP evidence
  capture, the AP worker descriptor and the AP local-timer TPR install.
- `Scheduler/Ia64KernelContext.S`: the start trampoline, its address getter
  and the cooperative context switch.
- `Scheduler/Ia64BspKernelRuntime.S`: the BSP twins of the evidence capture
  and the worker descriptor, the native-preemption descriptor and the
  native-preemption observation.
- `Interrupt/Ia64LocalSapicIpiController.S`: the Processor Interrupt Block
  delivery store.

Goldens under Tests/Golden/Ia64/{Time,Scheduler,Interrupt}/ pin the exact code. The
properties here state the rules that a golden update must still
obey: the asm-leaf symbol contract (P-HELPER-ELF), extents that cover exactly
the helper's code (P-EXTENT), the expected relocations (P-RELOC), serialized
control-register writes (P-SERIALIZE), leaf discipline (P-LEAF), the
evidence publication order (P-PUBLICATION), the descriptor words, and the
context switch's `flushrs` and publication phases (P-FLUSHRS, P-ORDER).
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import Ia64, Properties
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code
from AsteriaVerify.Properties import RelocationFact
from AsteriaVerify.Subjects import HelperExtents, HelperGroup, HelperProperty, HelperRelocations, HelperSymbols, leaf

READ_ITC = 'AsteriaIa64ReadIntervalTimeCounter'
WRITE_ITC = 'AsteriaIa64WriteOnlineIntervalCounter'
INITIALIZE_TIMER = 'AsteriaIa64InitializeLocalTimer'
PROGRAM_TIMER = 'AsteriaIa64ProgramLocalTimer'
CANCEL_TIMER = 'AsteriaIa64CancelLocalTimer'
AP_EVIDENCE = 'AsteriaIa64CaptureApplicationProcessorKernelRuntimeEvidence'
AP_DESCRIPTOR = 'AsteriaIa64InitializeApplicationProcessorKernelWorkerDescriptor'
AP_TASK_PRIORITY = 'AsteriaIa64InstallApplicationProcessorLocalTimerTaskPriority'
AP_WORKER_ENTRY = 'AsteriaIa64ApplicationProcessorKernelRuntimeWorkerEntry'
START_CONTEXT = 'AsteriaIa64StartKernelContext'
START_CONTEXT_ADDRESS = 'AsteriaIa64GetStartKernelContextAddress'
SWITCH_CONTEXT = 'AsteriaIa64SwitchKernelContext'
BSP_EVIDENCE = 'AsteriaIa64CaptureBspKernelWorkerEvidence'
BSP_DESCRIPTOR = 'AsteriaIa64InitializeBspKernelWorkerDescriptor'
BSP_WORKER_ENTRY = '_ZN7Asteria8Platform4Ia649Scheduler20Ia64BspKernelRuntime11WorkerEntryEPv'
BSP_PREEMPTION_DESCRIPTOR = 'AsteriaIa64InitializeBspNativePreemptionDescriptor'
BSP_OBSERVATION = 'AsteriaIa64CaptureBspNativePreemptionObservation'
COORDINATOR = 'AsteriaIa64BspNativePreemptionCoordinator'
LANDING_START = 'AsteriaIa64BspNativePreemptionSentinelLandingStart'
LANDING_END = 'AsteriaIa64BspNativePreemptionSentinelLandingEnd'
IDLE_LANDING = 'AsteriaIa64BspNativePreemptionIdleLanding'
WRITE_PIB = 'AsteriaIa64WriteProcessorInterruptBlock'

LOCAL_TIMER = HelperGroup('LocalTimer', (READ_ITC, WRITE_ITC, INITIALIZE_TIMER, PROGRAM_TIMER, CANCEL_TIMER))
AP_RUNTIME = HelperGroup('ApplicationProcessorKernelRuntime', (AP_EVIDENCE, AP_DESCRIPTOR, AP_TASK_PRIORITY))
KERNEL_CONTEXT = HelperGroup('KernelContext', (START_CONTEXT, START_CONTEXT_ADDRESS, SWITCH_CONTEXT))
BSP_RUNTIME = HelperGroup('BspKernelRuntime', (BSP_EVIDENCE, BSP_DESCRIPTOR))
# The one-shot native-preemption probe's helpers: a production firmware
# (ASTERIA_IA64_NATIVE_PREEMPTION_PROBE=0) does not contain them.
PROBE_OPTION = 'ia64_native_preemption_probe'
BSP_PREEMPTION = HelperGroup('BspNativePreemptionProbe', (BSP_PREEMPTION_DESCRIPTOR, BSP_OBSERVATION), PROBE_OPTION)
IPI = HelperGroup('LocalSapicIpiController', (WRITE_PIB,))
RUNTIME_GROUPS = (LOCAL_TIMER, AP_RUNTIME, KERNEL_CONTEXT, BSP_RUNTIME, BSP_PREEMPTION, IPI)


@register
class Ia64RuntimeHelperSymbols(HelperSymbols):
    GROUPS = RUNTIME_GROUPS


@register
class Ia64RuntimeHelperExtents(HelperExtents):
    GROUPS = RUNTIME_GROUPS


@register
class Ia64RuntimeHelperRelocations(HelperRelocations):
    GROUPS = RUNTIME_GROUPS
    EXPECTED = {
        AP_DESCRIPTOR: ((0x1, 'IMM64', AP_WORKER_ENTRY, 0),),
        START_CONTEXT_ADDRESS: ((0x1, 'IMM64', START_CONTEXT, 0),),
        BSP_DESCRIPTOR: ((0x1, 'IMM64', BSP_WORKER_ENTRY, 0),),
        BSP_PREEMPTION_DESCRIPTOR: ((0x1, 'IMM64', COORDINATOR, 0), (0x21, 'IMM64', LANDING_START, 0),
                                    (0x31, 'IMM64', LANDING_END, 0), (0x41, 'IMM64', IDLE_LANDING, 0)),
        BSP_OBSERVATION: ((0x1, 'IMM64', LANDING_START, 0),),
    }


def _control_writes(step) -> set[str]:
    return {w for w in step.operation.writes if w.startswith('cr.') or w == 'ar.itc'}


def _last_return(code: Code):
    return [step for step in code.steps if step.operation.is_return][-1]


# P-SERIALIZE: control-register writes -----------------------------------------------------

@register
class Ia64RuntimeHelperCrSerialization(HelperProperty):
    """P-SERIALIZE for the online ITC write, local-timer helpers (cr.itm, cr.itv) and AP TPR
    install (cr.tpr): every control-register write is immediately followed by
    an unpredicated `srlz.d` in a later instruction group, and every later
    read of that register and the next control-register write lie in a group
    after the `srlz.d`. The online ITC helper also returns in a later group.
    GAS dependency-violation checking (`-x`, fatal with
    `-Wa,--fatal-warnings`) enforces the same at build time."""
    GROUPS = (LOCAL_TIMER.subset(WRITE_ITC, INITIALIZE_TIMER, PROGRAM_TIMER, CANCEL_TIMER), AP_RUNTIME.subset(AP_TASK_PRIORITY))

    def item_problems(self, code):
        writes = [step for step in code.steps if _control_writes(step)]
        if not writes:
            return ['no control-register write']
        problems = []
        for write in writes:
            label = '%s (#%d)' % (write.operation.text, write.position)
            serialize = code.next(write)
            if serialize is None or serialize.mnemonic != 'srlz.d' or serialize.operation.is_predicated:
                problems.append('%s is not immediately followed by srlz.d' % label)
                continue
            if serialize.group <= write.group:
                problems.append('%s and its srlz.d share an instruction group' % label)
            if 'ar.itc' in _control_writes(write):
                following_operation = code.next(serialize)
                if following_operation is not None and following_operation.group <= serialize.group:
                    problems.append('online ITC serialization and its return share an instruction group')
            problems += ['%s (#%d) reads %s in the group of its srlz.d (#%d)'
                         % (later.operation.text, later.position, ', '.join(sorted(_control_writes(write))),
                            serialize.position) for later in code.window(after=serialize)
                         if _control_writes(write) & later.operation.reads and later.group <= serialize.group]
            following = [later for later in writes if later.position > write.position]
            if following and following[0].group <= serialize.group:
                problems.append('%s (#%d) shares the group of the previous srlz.d (#%d)'
                                % (following[0].operation.text, following[0].position, serialize.position))
        return problems

    def item_mutants(self, code):
        write = code.first(_control_writes)
        serialize = code.next(write)
        yield 'srlz.d after the first CR write replaced by nop.m', code.rewrite(serialize, 'nop.m 0x0')
        yield 'no stop between the first CR write and its srlz.d', code.without_boundary(write)
        yield 'no stop after the first srlz.d', code.without_boundary(serialize)
        resource = min(_control_writes(write))
        read = code.first(lambda step: step is write, lambda step: resource in step.operation.reads)
        if read is not None:
            yield 'first CR write swapped with its readback', code.swap(write, read)


# P-LEAF: stackless, memoryless leaves -------------------------------------------------------

FORBIDDEN_LEAF_OPERATIONS = (
    'alloc r14=ar.pfs,0,0,0', 'br.call.sptk.many b0=<AsteriaIa64UnexpectedCallee>', 'mov r12=r14',
    'mov.m ar.rsc=r14', 'mov r1=r14', 'ld8 r14=[r15]', 'ssm 0x4000', 'rsm 0x4000', 'mov r14=psr',
    'mov cr.eoi=r14', 'mov r14=cr.ivr', 'mov r14=cr.irr0')


@register
class Ia64RuntimeHelperLeafDiscipline(HelperProperty):
    """P-LEAF for the ITC read, the local-timer helpers and the AP TPR
    install: no `alloc`, call, gp (r1) or sp (r12) use, RSE control, memory
    access, relocation or PSR access (so no `rsm`/`ssm`), and no application
    or control register except the helper's own: ar.itc for the ITC read and
    Program, cr.itm/cr.itv for the timer helpers, cr.tpr for the TPR install.
    In particular no cr.eoi, cr.ivr or cr.irr0-3 access."""
    GROUPS = (LOCAL_TIMER, AP_RUNTIME.subset(AP_TASK_PRIORITY))
    ALLOWED = {
        READ_ITC: frozenset({'ar.itc'}),
        WRITE_ITC: frozenset({'ar.itc'}),
        INITIALIZE_TIMER: frozenset({'cr.itm', 'cr.itv'}),
        PROGRAM_TIMER: frozenset({'cr.itm', 'cr.itv', 'ar.itc'}),
        CANCEL_TIMER: frozenset({'cr.itv'}),
        AP_TASK_PRIORITY: frozenset({'cr.tpr'}),
    }

    def item(self, context, variant, name):
        return self.ALLOWED[name], leaf(context, variant, name).code

    def item_problems(self, item):
        allowed, code = item
        problems = Properties.leaf_problems(list(code.instructions), relocations=code.relocations)
        for step in code.steps:
            used = step.operation.reads | step.operation.writes
            foreign = sorted(register for register in used
                             if register.startswith(('ar.', 'cr.')) and register not in allowed)
            if foreign:
                problems.append('%s (#%d) uses %s' % (step.operation.text, step.position, ', '.join(foreign)))
            if any(register == 'psr' or register.startswith('psr.') for register in used):
                problems.append('%s (#%d) accesses the PSR' % (step.operation.text, step.position))
        return problems

    def item_mutants(self, item):
        allowed, code = item
        before_return = _last_return(code)
        for text in FORBIDDEN_LEAF_OPERATIONS:
            yield '%s inserted' % text, (allowed, code.insert(before_return, text, stop=True))
        yield 'gains a relocation', (allowed, dataclasses.replace(code, relocations=code.relocations + (
            RelocationFact(0, 'LTOFF22X', 'AsteriaIa64UnexpectedDatum', 0),)))


# P-PUBLICATION: evidence capture ------------------------------------------------------------

@register
class Ia64RuntimeHelperEvidencePublication(HelperProperty):
    """P-PUBLICATION for the AP evidence capture and its BSP twin: the
    evidence words are stored before an `mf` in a later group, and the one-byte
    completion flag (a register set to 1) is stored through the same pointer
    only in a group after the `mf`, so the observer can never see the flag
    before the evidence on weakly ordered IA-64 (one `mf`, one `st1` of a
    register last set to 1 no later than the `mf`'s group). Nothing is
    stored after the flag and nothing is loaded, the pointer changes only by
    the stores' post-increments, and the leaf has no alloc, call or sp write
    and returns once, after the flag."""
    GROUPS = (AP_RUNTIME.subset(AP_EVIDENCE), BSP_RUNTIME.subset(BSP_EVIDENCE))

    def item_problems(self, code):
        problems = []
        for step in code.steps:
            operation = step.operation
            if operation.memory not in (None, 'store'):
                problems.append('%s (#%d) accesses memory other than by a store' % (operation.text, step.position))
            if operation.base == 'alloc' or operation.is_call or Ia64.STACK_POINTER in operation.writes:
                problems.append('%s (#%d) allocates, calls or writes sp' % (operation.text, step.position))
        stores = code.all(lambda step: step.operation.memory == 'store')
        completions = [step for step in stores if step.operation.base == 'st1']
        evidence = [step for step in stores if step.operation.base == 'st8']
        problems += ['%s (#%d) is neither an st8 evidence store nor the st1 completion store'
                     % (step.operation.text, step.position)
                     for step in stores if step.operation.base not in ('st1', 'st8')]
        if len(completions) != 1:
            return problems + ['%d st1 completion stores, expected exactly one' % len(completions)]
        completion = completions[0]
        base = completion.operation.memory_base
        if not evidence:
            problems.append('no evidence store')
        problems += ['%s (#%d) stores through another base than the completion store'
                     % (step.operation.text, step.position) for step in evidence if step.operation.memory_base != base]
        problems += ['%s (#%d) changes the evidence base %s' % (step.operation.text, step.position, base)
                     for step in code.steps if base in step.operation.writes and step.operation.memory != 'store']
        fences = code.all(lambda step: step.operation.mnemonic == 'mf')
        if len(fences) != 1:
            return problems + ['%d mf fences, expected exactly one' % len(fences)]
        fence = fences[0]
        if evidence and not evidence[-1].group < fence.group:
            problems.append('the mf (#%d) is not in a group after the last evidence store (#%d)'
                            % (fence.position, evidence[-1].position))
        if not fence.group < completion.group:
            problems.append('the completion store (#%d) is not in a group after the mf (#%d)'
                            % (completion.position, fence.position))
        later_stores = [step for step in stores if step.position > completion.position]
        if later_stores:
            problems.append('%s (#%d) stores after the completion store'
                            % (later_stores[0].operation.text, later_stores[0].position))
        values = sorted(completion.operation.reads - {base})
        if len(values) != 1:
            problems.append('the completion store does not store one register')
        else:
            definitions = [step for step in code.window(before=completion) if values[0] in step.operation.writes]
            last = definitions[-1].operation if definitions else None
            if last is None or last.base != 'mov' or last.immediates != (1,) or last.reads:
                problems.append('the completion value %s is not last set to 1' % values[0])
            elif definitions[-1].group > fence.group:
                problems.append('the completion value %s is set to 1 after the mf' % values[0])
        returns = code.all(lambda step: step.operation.is_return)
        if len(returns) != 1 or returns[0].position < completion.position:
            problems.append('%d returns; expected exactly one, after the completion store' % len(returns))
        return problems

    def item_mutants(self, code):
        fence = code.first(lambda step: step.operation.mnemonic == 'mf')
        completion = code.first(lambda step: step.operation.base == 'st1')
        evidence = code.all(lambda step: step.operation.base == 'st8')
        value = sorted(completion.operation.reads - {completion.operation.memory_base})[0]
        setter = [step for step in code.window(before=completion) if value in step.operation.writes][-1]
        yield 'mf replaced by nop.m', code.rewrite(fence, 'nop.m 0x0')
        yield 'completion store replaced by nop.m', code.rewrite(completion, 'nop.m 0x0')
        yield 'completion store before the mf', code.swap(fence, completion)
        yield 'return replaced by nop.b', code.rewrite(_last_return(code), 'nop.b 0x0')
        yield 'mf in the group of the last evidence store', code.without_boundary(evidence[-1])
        yield 'evidence store after the completion store', code.insert(code.next(completion), evidence[-1].rendered,
                                                                       stop=True)
        yield 'completion value 0', code.rewrite(setter, 'mov %s=0' % value)


# Descriptor words ---------------------------------------------------------------------------

GP = ('gp',)


def address_of(symbol: str) -> tuple[str, str]:
    return ('address', symbol)


@register
class Ia64RuntimeHelperDescriptorWords(HelperProperty):
    """The descriptor leaves store exactly their consumers' layout: the AP and
    BSP worker descriptors {raw worker-entry bundle address, gp}, and the BSP
    native-preemption descriptor {coordinator entry, gp, sentinel landing
    start, sentinel landing end, idle-landing entry, gp}. Every code address
    comes from one `movl` with an IMM64 relocation (never a function
    descriptor), every gp word is a copy of r1, and each word is stored by its
    own `st8` through r32 (post-increment 8 except the last) in its own
    instruction group; nothing else touches memory, r1 or r32, and the leaf
    returns once."""
    GROUPS = (AP_RUNTIME.subset(AP_DESCRIPTOR), BSP_RUNTIME.subset(BSP_DESCRIPTOR),
              BSP_PREEMPTION.subset(BSP_PREEMPTION_DESCRIPTOR))
    EXPECTED = {
        AP_DESCRIPTOR: (address_of(AP_WORKER_ENTRY), GP),
        BSP_DESCRIPTOR: (address_of(BSP_WORKER_ENTRY), GP),
        BSP_PREEMPTION_DESCRIPTOR: (address_of(COORDINATOR), GP, address_of(LANDING_START),
                                    address_of(LANDING_END), address_of(IDLE_LANDING), GP),
    }

    def item(self, context, variant, name):
        return self.EXPECTED[name], leaf(context, variant, name).code

    def item_problems(self, item):
        expected, code = item
        base = 'r32'
        values: dict[str, tuple] = {}
        words, problems = [], []
        finished, last_group, returns = False, None, 0
        for step in code.steps:
            operation, label = step.operation, '%s (#%d)' % (step.operation.text, step.position)
            if operation.is_predicated:
                problems.append('%s is predicated' % label)
            if operation.memory == 'store' and operation.base == 'st8' and operation.memory_base == base:
                if finished:
                    problems.append('%s stores after the final word' % label)
                sources = sorted(operation.reads - {base})
                words.append(values.get(sources[0], ('unknown', sources[0])) if len(sources) == 1 else ('unknown',))
                if last_group is not None and step.group == last_group:
                    problems.append('%s shares an instruction group with the previous descriptor store' % label)
                last_group = step.group
                if base not in operation.writes:
                    finished = True
                elif operation.immediates != (8,):
                    problems.append('%s does not advance by one word' % label)
            elif operation.memory is not None:
                problems.append('%s accesses memory other than by a descriptor store' % label)
            elif operation.is_return:
                returns += 1
            elif operation.is_branch:
                problems.append('%s branches' % label)
            else:
                if base in operation.writes or Ia64.GLOBAL_POINTER in operation.writes:
                    problems.append('%s changes r32 or gp' % label)
                target = operation.target
                for register in operation.general_writes:
                    if operation.base == 'movl' and target is not None and target.kind in ('reloc', 'symbol') \
                            and target.offset == 0 and target.relocation in (None, 'IMM64'):
                        values[register] = address_of(target.symbol)
                    elif (operation.base == 'mov' and operation.reads == {Ia64.GLOBAL_POINTER}
                          and not operation.immediates):
                        values[register] = GP
                    elif operation.base == 'mov' and len(operation.reads) == 1 and not operation.immediates \
                            and next(iter(operation.reads)) in values:
                        values[register] = values[next(iter(operation.reads))]
                    else:
                        values[register] = ('unknown', operation.text)
        if returns != 1:
            problems.append('%d returns, expected exactly one' % returns)
        if tuple(words) != tuple(expected):
            problems.append('stores the words %s, expected %s' % (words, list(expected)))
        return problems

    def item_mutants(self, item):
        expected, code = item
        gp_copy = code.first(lambda step: step.operation.reads == {Ia64.GLOBAL_POINTER})
        addresses = code.all(lambda step: step.operation.base == 'movl')
        stores = code.all(lambda step: step.operation.memory == 'store')

        def materialize(step, symbol):
            return '%s=<reloc:IMM64 %s>' % (step.operation.text.split('=')[0], symbol)

        for description, mutant in (
                ('gp word copied from r0', code.rewrite(gp_copy, gp_copy.rendered.replace('=r1', '=r0'))),
                ('first code address retargeted',
                 code.rewrite(addresses[0], materialize(addresses[0], START_CONTEXT)))):
            yield description, (expected, mutant)
        if len(addresses) > 1:
            first, second = (step.operation.target.symbol for step in addresses[:2])
            yield 'two code addresses exchanged', (expected, code.rewrite(addresses[0], materialize(
                addresses[0], second)).rewrite(addresses[1], materialize(addresses[1], first)))
        for description, mutant in (
                ('first word stores the gp register', code.rewrite(
                    stores[0], 'st8 [r32]=%s,8' % sorted(gp_copy.operation.general_writes)[0])),
                ('last word store replaced by nop.m', code.rewrite(stores[-1], 'nop.m 0x0')),
                ('return replaced by nop.b', code.rewrite(_last_return(code), 'nop.b 0x0')),
                ('two descriptor stores in one group', code.without_boundary(stores[0]))):
            yield description, (expected, mutant)


# P-FLUSHRS and P-ORDER: the cooperative context switch ----------------------------------------

PUBLICATION_FENCES = ('mf', 'invala', 'sync.i')


@register
class Ia64KernelContextSwitchPhases(HelperProperty):
    """The context switch's ordering gates that GAS dependency checking does
    not enforce: `flushrs` starts an instruction group (P-FLUSHRS), and the
    save is published by one each of `mf`, `invala` and `sync.i`, in that
    order, each alone in its group, after the last save store and before the
    first restore load (P-ORDER)."""
    GROUPS = (KERNEL_CONTEXT.subset(SWITCH_CONTEXT),)

    def item_problems(self, code):
        flushes = code.all(lambda step: step.operation.base == 'flushrs')
        problems = [] if flushes else ['no flushrs']
        problems += ['flushrs (#%d) does not start an instruction group' % flush.position for flush in flushes
                     if code.previous(flush) is not None and code.previous(flush).group == flush.group]
        positions = {}
        for mnemonic in PUBLICATION_FENCES:
            found = code.all(lambda step: step.operation.mnemonic == mnemonic)
            if len(found) != 1:
                problems.append('%d %s, expected exactly one' % (len(found), mnemonic))
                continue
            positions[mnemonic] = found[0]
            companions = [other for other in code.steps if other is not found[0] and other.group == found[0].group]
            if companions:
                problems.append('%s (#%d) shares its instruction group with %s'
                                % (mnemonic, found[0].position, companions[0].operation.text))
        if len(positions) != len(PUBLICATION_FENCES):
            return problems
        order = [positions[mnemonic].position for mnemonic in PUBLICATION_FENCES]
        if order != sorted(order):
            problems.append('the publication fences are not in the order mf, invala, sync.i')
        saves = code.all(lambda step: step.operation.memory == 'store')
        restores = code.all(lambda step: step.operation.memory == 'load')
        if saves and saves[-1].position > positions['mf'].position:
            problems.append('%s (#%d) saves after the mf' % (saves[-1].operation.text, saves[-1].position))
        if restores and restores[0].position < positions['sync.i'].position:
            problems.append('%s (#%d) restores before sync.i' % (restores[0].operation.text, restores[0].position))
        return problems

    def item_mutants(self, code):
        def first(mnemonic):
            return code.first(lambda step: step.operation.mnemonic == mnemonic)
        fence, invalidate, synchronize = first('mf'), first('invala'), first('sync.i')
        yield 'no stop before flushrs', code.without_boundary(code.previous(first('flushrs')))
        yield 'mf shares a group with invala', code.without_boundary(fence)
        yield 'sync.i replaced by srlz.i', code.rewrite(synchronize, 'srlz.i')
        yield 'invala before mf', code.swap(fence, invalidate)
        yield 'last save after mf', code.swap(code.all(lambda step: step.operation.memory == 'store')[-1], fence)
        yield 'first restore before sync.i', code.swap(synchronize, code.first(
            lambda step: step.operation.memory == 'load'))
