# SPDX-License-Identifier: Apache-2.0
"""processor stop policy: raw poll corridors, terminal landing and physical scanout.

The core entry proofs remove only these independently checked poll ranges.
Assembly goldens pin pixel traversal; these properties keep the privilege,
ordering, ownership and access rules invariant through a golden update.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import Ia64, Ia64StopViews, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.Subjects import HelperGroup, HelperProperty, HelperSymbols, HelperExtents
from AsteriaVerify.TargetChecks.Ia64PhysicalAccess import CorridorSpec, corridor_problems
from AsteriaVerify.TargetChecks.Ia64InterruptVectorTable import TABLE, REFILL_VECTORS, STOP_VECTOR_OFFSETS

SCANOUT = ('AsteriaIa64FillStopPhysical', 'AsteriaIa64DrawStopGlyphPhysical')
# The user entries' stubs classify their exit there before the raw entry.
USER_EXIT = 'AsteriaIa64StopUserExit'
STOP = HelperGroup('Stops', ('AsteriaIa64StopPark', 'AsteriaIa64CaptureStopSnapshot',
    'AsteriaIa64StopEntry', 'AsteriaIa64SendPowerInit', *SCANOUT, 'AsteriaIa64StopFromIvt', USER_EXIT, *sorted(Ia64StopViews.TERMINAL_STUBS)))
ENTRIES = HelperGroup('StopPolls', ('AsteriaIa64SystemCallEntry', 'AsteriaIa64PageFaultNativeEntry',
    'AsteriaIa64ExternalInterruptEntry', 'AsteriaIa64ExternalInterruptDispatchEntry',
    'AsteriaIa64PollForExternalInterruptSequence', 'AsteriaIa64KernelIdle',
    'AsteriaIa64UserFaultEntry', 'AsteriaIa64UserExceptionEntry', 'AsteriaIa64UserInterruptEntry'))


@register
class Ia64StopSymbols(HelperSymbols):
    GROUPS = (STOP,)


@register
class Ia64StopExtents(HelperExtents):
    GROUPS = (STOP,)


def physical_problems(name, code):
    # The common corridor establishes PSR, lazy RSE, serialization, fences,
    # framelessness, no virtual read and exact st4 access. Its single-write
    # address analysis does not cover bulk traversal; prove that separately.
    problems = [problem for problem in corridor_problems(code, CorridorSpec((('store', 'st4'),), True))
                if 'does not address an uncacheable physical alias' not in problem]
    glyph = name == SCANOUT[1]
    base, pitch = ('r25', 'r16') if glyph else ('r23', 'r18')
    expected_row = ['mov r15=r32', 'or r15=r15,r27', 'add r15=r15,' + pitch]
    actual_row = [step.text for step in code.steps if 'r15' in step.operation.general_writes]
    if actual_row != expected_row:
        problems.append('physical row address is not the captured base, UC alias and pitch induction')
    if len(code.all('movl r27=0x8000000000000000')) != 1:
        problems.append('uncacheable alias bit 63 missing')
    expected_pixel = ['mov %s=r15' % base, 'st4 [%s]=%s,4' % (base, 'r26' if glyph else 'r19')]
    if [step.text for step in code.steps if base in step.operation.general_writes] != expected_pixel:
        problems.append('pixel cursor is not derived from the UC row and incremented by four')
    disable = code.first('rsm 0x20000')
    if disable is None or any(step.uses('r%d' % reg) for step in code.steps if step.position > disable.position
                             for reg in range(32, 128)):
        problems.append('stacked argument read after DT is disabled')
    if code.relocations:
        problems.append('physical scanout has linked data or control relocations')
    if glyph:
        for form in ('mov r22=16', 'mov r24=8', 'adds r24=-1,r24', 'adds r22=-1,r22',
                     'extr.u r23=r17,0,8', 'shr.u r17=r17,8', 'extr.u r26=r23,7,1', 'shl r23=r23,1'):
            if len(code.all(form)) != 1:
                problems.append('glyph traversal lacks ' + form)
    return problems


@register
class Ia64StopPhysicalScanout(HelperProperty):
    GROUPS = (STOP.subset(*SCANOUT),)

    def verify(self, facts):
        return [name + ': ' + problem for name, code in facts.items() for problem in physical_problems(name, code)]

    def mutants(self, facts):
        for name, code in facts.items():
            store = code.site(lambda step: step.operation.memory == 'store')
            variants = [('64-bit scanout', code.rewrite(store, store.text.replace('st4', 'st8'))),
                ('virtual glyph fetch', code.rewrite(code.free_slot(kind='nop.m'), 'ld8 r16=[r12]')),
                ('cacheable alias', code.rewrite(code.site('movl r27=0x8000000000000000'), 'movl r27=0')),
                ('instruction translation disabled', code.rewrite(code.site('rsm 0x20000'), 'rsm 0x1000000000')),
                ('no write fence', code.nop(code.site('mf.a'))),
                ('RSE eager', code.rewrite(code.site('dep r27=0,r21,0,2'), 'mov r27=r21')),
                ('stacked font row in physical mode', code.rewrite(store, 'st4 [r25]=r34,4'))]
            for description, mutant in variants:
                yield name + ': ' + description, {**facts, name: mutant}


@register
class Ia64StopPolling(HelperProperty):
    GROUPS = (ENTRIES,)

    def item(self, context, variant, name):
        path = context.defining_object(variant, name)
        code = Subjects.code(context, path, name)
        ranges = Ia64StopViews.poll_ranges(path, code)
        return tuple(dataclasses.replace(code, instructions=tuple(instruction for instruction in code.instructions
            if start <= instruction.address < end)) for start, end in ranges)

    def verify(self, facts):
        problems = []
        for name, polls in facts.items():
            if len(polls) != 1:
                problems.append(name + ': requires one cooperative latch poll')
            for code in polls:
                problems += [name + ': ' + problem for problem in Properties.register_domain_problems(
                    list(code.instructions), {'r0', 'r16', 'r24', 'r25', 'r26'})]
                forms = [step.text for step in code.steps if not step.operation.is_branch]
                required = ['mov r24=pr', 'mov.m r25=ar.k3', 'cmp.eq p6,p7=r25,r0', 'adds r25=48,r25',
                    'ld8 r25=[r25]', 'cmp.eq p6,p7=r25,r0', 'ld8.acq r26=[r25]', 'cmp.eq p6,p7=r26,r0']
                if forms[:8] != required or forms[-1:] != ['mov pr=r24,0xfffffffffffffffe']:
                    problems.append(name + ': immutable context, acquire latch or PR preservation missing')
                branches = code.all(lambda step: step.operation.is_branch)
                if len(branches) != 4 or any(step.mnemonic != 'br.cond.sptk.few' or step.predicate != 'p6'
                    or step.target != branches[0].target for step in branches[:3]):
                    problems.append(name + ': three null/zero guards must reach the PR restore')
                if len(branches) < 4 or branches[-1].predicate or branches[-1].operation.target is None \
                    or branches[-1].operation.target.symbol != 'AsteriaIa64StopEntry':
                    problems.append(name + ': latch handoff must be terminal')
        return problems

    def mutants(self, facts):
        for name, polls in facts.items():
            if not polls:
                continue
            code = polls[0]
            for label, mutant in [('latch not acquire', code.rewrite(code.site('ld8.acq r26=[r25]'), 'ld8 r26=[r25]')),
                ('PR lost', code.nop(code.steps[-1])), ('context from user register',
                 code.rewrite(code.site('mov.m r25=ar.k3'), 'mov r25=r32'))]:
                yield name + ': ' + label, {**facts, name: (mutant,)}
            yield name + ': missing poll', {**facts, name: ()}


@register
class Ia64StopTerminalLanding(HelperProperty):
    GROUPS = (STOP.subset('AsteriaIa64StopEntry', 'AsteriaIa64CaptureStopSnapshot', 'AsteriaIa64StopPark'),)

    def verify(self, facts):
        problems = []
        entry = facts['AsteriaIa64StopEntry']
        cover = entry.first('cover')
        if cover is None:
            return ['terminal landing lacks cover']
        before = tuple(instruction for instruction in entry.instructions if instruction.address < cover.instruction.address)
        problems += Properties.register_domain_problems([instruction for instruction in before if instruction.text != 'mov r20=b0'], Ia64.BANKED_GENERAL | {'r0'})
        for form in ('adds r18=144,r18', 'adds r19=40,r18', 'adds r18=48,r18', 'shl r18=r31,7',
                     'st8.rel [r19]=r20', 'rsm 0x6000'):
            if len(entry.all(form)) != 1:
                problems.append('terminal capture lacks ' + form)
        # PowerOff.md: inspect the power latch before touching a fatal record,
        # and again at the capture race; one release acknowledgement then parks.
        for form, count in (('ld8.acq r20=[r30]', 2), ('tbit.z p7,p6=r20,63', 2),
                            ('adds r18=792,r30', 1), ('fetchadd8.rel r19=[r18],1', 1)):
            if len(entry.all(form)) != count:
                problems.append('power parking lacks ' + form)
        if any(step.operation.base in ('itr', 'itc') for step in entry.steps):
            problems.append('terminal landing installs a translation')
        snapshot = facts['AsteriaIa64CaptureStopSnapshot']
        start = snapshot.first('rsm 0x2000')
        restore = snapshot.first(lambda step: step.text == 'ssm 0x2000')
        if start is None or restore is None:
            problems.append('snapshot interruption registers require an IC-clear interval')
        else:
            for step in snapshot.steps:
                if any(reg.startswith('cr.') for reg in step.operation.reads) and not start.position < step.position < restore.position:
                    problems.append('snapshot reads interruption register with IC set')
        park = facts['AsteriaIa64StopPark']
        if [step.text for step in park.steps[:3]] != ['rsm 0x4000', 'srlz.d', 'hint.m 0'] \
                or park.steps[-1].target != park.steps[-2].bundle:
            problems.append('park does not mask interrupts and pause forever')
        return problems

    def mutants(self, facts):
        entry = facts['AsteriaIa64StopEntry']
        for label, mutant in [('shared record', entry.rewrite(entry.site('shl r18=r31,7'), 'mov r18=r0')),
            ('TR consumed', entry.rewrite(entry.free_slot(kind='nop.m'), 'itr.d dtr[r0]=r20')),
            ('stack before capture', entry.rewrite(entry.site('ld8.acq r20=[r30]'), 'ld8 r20=[r12]'))]:
            yield label, {**facts, 'AsteriaIa64StopEntry': mutant}
        capture = facts['AsteriaIa64CaptureStopSnapshot']
        yield 'snapshot IC stays set', {**facts, 'AsteriaIa64CaptureStopSnapshot': capture.nop(capture.site('rsm 0x2000'))}
        yield 'power acknowledgement not released', {**facts, 'AsteriaIa64StopEntry':
            entry.nop(entry.site('fetchadd8.rel r19=[r18],1'))}


@register
class Ia64PowerInitCorridor(HelperProperty):
    """PowerOff.md / SAL 3.0 §5.1: terminal architectural INIT, not a freeze."""
    GROUPS = (STOP.subset('AsteriaIa64SendPowerInit'),)

    def verify(self, facts):
        code = facts['AsteriaIa64SendPowerInit']
        forms = ('mov r16=r32', 'rsm 0x4000', 'mov.m ar.rsc=r0', 'rsm 0x20000',
                 'movl r17=0x8000000000000000', 'mov r19=1280', 'or r18=r16,r17',
                 'mf', 'st8 [r18]=r19', 'mf.a')
        steps = [code.first(form) for form in forms]
        problems = []
        if any(step is None for step in steps) or [step.position for step in steps] != sorted(step.position for step in steps):
            problems.append('INIT must capture the address, mask IRQ/RSE/DT, use UC and fence its exact st8')
        disable = code.first('rsm 0x20000')
        if disable is not None and any(step.uses('r%d' % reg) for step in code.steps
                                      if step.position > disable.position for reg in range(32, 128)):
            problems.append('INIT touches a stacked register with DT disabled')
        if any(step.uses(reg) for step in code.steps for reg in ('r1', 'r12', 'r13')):
            problems.append('INIT touches GP, stack or thread pointer')
        if len([step for step in code.steps if step.operation.memory is not None]) != 1:
            problems.append('INIT must perform only its PIB write')
        if any(step.operation.is_call or step.operation.base in ('itr', 'itc', 'alloc', 'cover', 'flushrs', 'loadrs')
               for step in code.steps):
            problems.append('INIT must not call, install translations or activate the RSE')
        last = code.steps[-1]
        if (not last.operation.is_branch or last.operation.is_call or last.predicate
                or last.operation.target is None or last.operation.target.symbol != 'AsteriaIa64StopPark'):
            problems.append('INIT return must park without further I/O')
        return problems

    def mutants(self, facts):
        code = facts['AsteriaIa64SendPowerInit']
        for form in ('rsm 0x20000', 'movl r17=0x8000000000000000', 'mf.a', 'mov r19=1280'):
            yield 'INIT loses ' + form, {**facts, 'AsteriaIa64SendPowerInit': code.nop(code.site(form))}


@register
class Ia64StopEntryEdges(HelperProperty):
    GROUPS = (ENTRIES, STOP, HelperGroup('ClaimStopEdges', ('AsteriaIa64UserInterruptClaim',)))

    def verify(self, facts):
        problems = []
        terminal_targets = Ia64StopViews.TERMINAL_STUBS | {'AsteriaIa64StopEntry', 'AsteriaIa64StopPark',
                                                         'AsteriaIa64StopCaptured', USER_EXIT}
        for name, code in facts.items():
            for relocation in code.relocations:
                if relocation.target in terminal_targets and (relocation.type != 'PCREL21B' or relocation.addend):
                    problems.append(name + ': stop edge must be a direct PCREL21B with no addend')
            for step in code.steps:
                target = step.operation.target
                if target is None or target.symbol not in terminal_targets:
                    continue
                if step.operation.is_call:
                    if name != 'AsteriaIa64StopEntry' or target.symbol != 'AsteriaIa64StopCaptured':
                        problems.append(name + ': stop handoff unexpectedly calls')
                elif step.predicate and not (name == 'AsteriaIa64StopEntry' and target.symbol == 'AsteriaIa64StopPark'):
                    problems.append(name + ': terminal edge is predicated')
                if target.offset:
                    problems.append(name + ': terminal edge enters inside a stop helper')
        return problems

    def mutants(self, facts):
        for name, code in facts.items():
            facts_ = [fact for fact in code.relocations if fact.target == 'AsteriaIa64StopEntry']
            if facts_:
                fact = facts_[0]
                bad = dataclasses.replace(fact, type='LTOFF22')
                yield name + ': GP relative stop edge', {**facts, name: dataclasses.replace(code,
                    relocations=tuple(bad if value == fact else value for value in code.relocations))}


@register
class Ia64StopUserExit(HelperProperty):
    """The vector word of a user entry's stop names its exit (Ia64StopExit in
    Ia64Stop.hpp). AsteriaIa64StopUserFault and AsteriaIa64StopException each
    end in an unpredicated branch to AsteriaIa64StopUserExit, which reads PSR
    once (`mov r17=psr`), splits on PSR.ic (`tbit.z p7,p6=r17,13`), puts
    UserEntry << 16 in r18 under p7 (collection off: before the bridge) and
    UserFaultBridge << 16 under p6, deposits r8's low eight bits (the bridge
    status) at bit 20 under p6, ORs r18 into r16 (the vector), writes r16
    nowhere else and ends in an unpredicated branch to AsteriaIa64StopEntry.
    A swapped or lost exit would point the next stop's reader at the wrong
    code."""
    GROUPS = (STOP.subset(USER_EXIT, 'AsteriaIa64StopUserFault', 'AsteriaIa64StopException'),)

    def extract(self, context, subject):
        from AsteriaVerify import Layout
        text = Layout.read(context.root / 'Source/Platform/Ia64/Diagnostics/Ia64Stop.hpp')
        return super().extract(context, subject), Layout.constants(text), Layout.enumerators(text, 'Ia64StopExit')

    @staticmethod
    def _branches_to(code, symbol: str) -> bool:
        last = code.steps[-1] if code.steps else None
        return (last is not None and last.operation.is_branch and not last.predicate
                and last.operation.target is not None and last.operation.target.symbol == symbol
                and not last.operation.target.offset)

    def verify(self, facts):
        codes, constants, exits = facts
        shift, status_shift, status_bits = (constants.get(name) for name in (
            'Ia64StopExitShift', 'Ia64StopBridgeStatusShift', 'Ia64StopBridgeStatusBits'))
        if None in (shift, status_shift, status_bits) or not {'UserEntry', 'UserFaultBridge'} <= set(exits):
            return ['the vector word layout is not declared in Ia64Stop.hpp']
        problems = ['%s does not end in an unpredicated branch to %s' % (name, USER_EXIT)
                    for name in ('AsteriaIa64StopUserFault', 'AsteriaIa64StopException')
                    if not self._branches_to(codes[name], USER_EXIT)]
        code = codes[USER_EXIT]
        expected = [('mov r17=psr', None), ('tbit.z p7,p6=r17,13', None),
                    ('mov r18=%d' % (exits['UserEntry'] << shift), 'p7'),
                    ('mov r18=%d' % (exits['UserFaultBridge'] << shift), 'p6'),
                    ('dep r18=r8,r18,%d,%d' % (status_shift, status_bits), 'p6'), ('or r16=r16,r18', None)]
        actual = [(step.text, step.predicate) for step in code.steps if not step.operation.is_branch]
        if actual != expected:
            problems.append('the exit is not classified by PSR.ic into r16: %s' % actual)
        if not self._branches_to(code, 'AsteriaIa64StopEntry'):
            problems.append('%s does not end in an unpredicated branch to AsteriaIa64StopEntry' % USER_EXIT)
        return problems

    def mutants(self, facts):
        codes, constants, exits = facts
        code = codes[USER_EXIT]
        for label, mutant in (('exits swapped', code.swap(code.site('tbit.z p7,p6=r17,13'), code.site('or r16=r16,r18'))),
                              ('wrong PSR bit', code.rewrite(code.site('tbit.z p7,p6=r17,13'), 'tbit.z p7,p6=r17,14')),
                              ('bridge status lost', code.nop(code.site(lambda step: step.text.startswith('dep ')))),
                              ('exit not merged', code.nop(code.site('or r16=r16,r18'))),
                              ('exit skipped', code.retarget(code.steps[-1], 'AsteriaIa64StopPark'))):
            yield label, ({**codes, USER_EXIT: mutant}, constants, exits)
        stub = codes['AsteriaIa64StopUserFault']
        yield 'stub bypasses the exit', ({**codes, 'AsteriaIa64StopUserFault': stub.retarget(stub.steps[-1],
                                         'AsteriaIa64StopEntry')}, constants, exits)


@register
class Ia64StopRefillPolling(HelperProperty):
    GROUPS = (HelperGroup('RefillStopPolls', (TABLE,)),)

    def item(self, context, variant, name):
        path = context.defining_object(variant, TABLE)
        result = []
        for vector in REFILL_VECTORS:
            code = Subjects.code(context, path, start=vector.start, end=vector.end)
            ranges = Ia64StopViews.poll_ranges(path, code)
            result.append((code, tuple(dataclasses.replace(code, instructions=tuple(instruction
                for instruction in code.instructions if start <= instruction.address < end)) for start, end in ranges)))
        return tuple(result)

    def verify(self, facts):
        problems = []
        for code, polls in facts[TABLE]:
            if len(polls) != 1:
                problems.append('each alternate refill requires exactly one latch poll')
                continue
            steps = polls[0].steps
            expected = ['adds r28=48,r17', 'ld8 r28=[r28]', 'ld8.acq r28=[r28]', 'cmp.eq p7,p6=r28,r0']
            if [step.text for step in steps[:4]] != expected or len(steps) != 5:
                problems.append('refill does not derive and acquire the immutable stop latch')
            last = code.steps[-1]
            branch = steps[-1]
            if branch.mnemonic != 'br.cond.dpnt.few' or branch.predicate != 'p6' or branch.target != last.bundle:
                problems.append('refill latch does not converge on its terminal capture')
        return problems

    def mutants(self, facts):
        rows = facts[TABLE]
        code, polls = rows[0]
        poll = polls[0]
        for label, mutant in [('non-acquire latch', poll.rewrite(poll.site('ld8.acq r28=[r28]'), 'ld8 r28=[r28]')),
                               ('lost terminal handoff', poll.nop(poll.steps[-1]))]:
            yield label, {TABLE: ((code, (mutant,)),) + rows[1:]}
        yield 'missing refill poll', {TABLE: ((code, ()),) + rows[1:]}


@register
class Ia64StopUnusedVectors(HelperProperty):
    GROUPS = (HelperGroup('UnusedStopVectors', (TABLE,)),)

    def item(self, context, variant, name):
        path = context.defining_object(variant, TABLE)
        code = Subjects.code(context, path, TABLE)
        return tuple(dataclasses.replace(code, instructions=tuple(instruction for instruction in code.instructions
            if offset <= instruction.address - code.start < offset + 16)) for offset in STOP_VECTOR_OFFSETS)

    def verify(self, facts):
        problems = []
        for code in facts[TABLE]:
            steps = code.steps
            if len(steps) != 2 or steps[0].text != 'mov r16=ip' or steps[1].mnemonic not in ('br.many','br.sptk.many') \
                    or steps[1].predicate or steps[1].operation.target is None \
                    or steps[1].operation.target.symbol != 'AsteriaIa64StopFromIvt' or steps[1].operation.target.offset:
                problems.append('unused architected vector does not preserve IP and branch directly to stop capture')
        return problems

    def mutants(self, facts):
        first = facts[TABLE][0]
        for label, mutant in [('lost vector', first.nop(first.steps[0])), ('lost handoff', first.nop(first.steps[1]))]:
            yield label, {TABLE: (mutant,) + facts[TABLE][1:]}


@register
class Ia64StopStackBound(HelperProperty):
    """The fresh terminal C++ call tree fits both 8 KiB per-CPU areas.

    No indirect-call binding is permitted on this terminal path. The raw
    landing's alloc is independently pinned and its 16-byte ABI scratch
    reservation is included in the computed memory bound.
    """
    GROUPS = (STOP.subset('AsteriaIa64StopEntry'),)

    def extract(self, context, subject):
        from AsteriaVerify import CallGraph, Layout
        from AsteriaVerify.CallGraph import Policy, Root
        from AsteriaVerify.TargetChecks.Ia64PreemptionCorridor import POLICY
        variant = subject[0]
        paths = [context.side_file(path, '.ci') for source, path in context.objects(variant).items()
                 if not source.endswith(('.S','.s'))]
        graph = CallGraph.load(paths)
        image = CallGraph.decode_image(context.tool('objdump'), context.artifact(Subjects.image_key(variant)))
        constants = Layout.constants(Layout.read(context.root / 'Source/Platform/Ia64/Diagnostics/Ia64Stop.hpp'))
        policy = Policy(roots=(Root('AsteriaIa64StopCaptured', 16, 2, 'AsteriaIa64StopEntry'),),
                        leaves=POLICY.leaves, bindings=(), bound_bytes=8192)
        return graph, image, policy, constants

    def verify(self, facts):
        from AsteriaVerify import CallGraph
        graph,image,policy,constants = facts
        problems = []
        if constants.get('Ia64StopMemoryStackSizeBytes') != policy.bound_bytes \
            or constants.get('Ia64StopBackingStoreSizeBytes') != policy.bound_bytes:
            problems.append('terminal stack and backing store do not match the proved bound')
        return problems + CallGraph.analyze(graph,image,policy).problems

    def summary(self, facts):
        from AsteriaVerify import CallGraph
        return CallGraph.analyze(*facts[:3]).summary() + ' (bound 8192)'

    def mutants(self, facts):
        graph,image,policy,constants=facts
        index=next(i for i,node in enumerate(graph.nodes) if node.title == 'AsteriaIa64StopCaptured' and node.frame is not None)
        nodes=graph.nodes[:index]+(dataclasses.replace(graph.nodes[index],frame=8192),)+graph.nodes[index+1:]
        yield 'terminal memory stack overflow', (dataclasses.replace(graph,nodes=nodes),image,policy,constants)
        yield 'undersized RBS', (graph,image,dataclasses.replace(policy,bound_bytes=128),constants)
