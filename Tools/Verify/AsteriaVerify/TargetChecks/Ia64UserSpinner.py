# SPDX-License-Identifier: Apache-2.0
"""The CPU-bound user spinner must check its state after a count that
outlasts a time slice and neither yields nor touches memory."""
from collections import Counter

from AsteriaVerify import Subjects
from AsteriaVerify.Checks import register


# The count loop: two iteration counters, an ITC sample and its deadline
# compare, branching back while the deadline lies ahead (p6).
LOOP = ('adds r8=1,r8', 'adds r9=1,r9', 'mov.m r10=ar.itc', 'cmp.ltu p6,p7=r10,r11')
# The deadline, an ITC sample plus the count's ticks, set once before the loop.
DEADLINE = ('mov.m r11=ar.itc', 'add r11=r11,r10')
# After the loop both counters agree and are nonzero (cmp.ne/cmp.eq rendered).
COUNT_CHECKS = ('cmp.eq p7,p6=r8,r9', 'cmp.eq p6,p7=r0,r8')


@register
class Ia64UserSpinnerState(Subjects.CodeProperty):
    def subjects(self, context):
        key = 'firmware-test:spinner_elf'
        return [Subjects.Subject(key, 'firmware-test', context.artifact(key), True)] if context.has_artifact(key) else []

    def extract(self, context, subject):
        return Subjects.code(context, subject.path, 'AsteriaIa64SpinnerEntry')

    def verify(self, code):
        problems = []
        counts = Counter(step.text for step in code.steps)
        required = ['alloc r2=ar.pfs,96,96,0', 'mov r32=pr', 'mov r33=pr', 'mov.m r16=ar.bsp',
                    'mov.m r16=ar.bspstore', 'mov.m r16=ar.rnat', 'mov.m r16=ar.fpsr', 'mov.i r16=ar.pfs',
                    'mov.i r16=ar.lc', 'mov.i r16=ar.ec', 'mov.m r16=ar.unat', 'mov.m r16=ar.rsc',
                    'break.i 0x1234', 'break.i 0x4321']
        required += ['setf.sig f%d=r16' % number for number in range(2, 128)]
        required += ['getf.sig r16=f%d' % number for number in range(2, 128)]
        required += ['getf.exp r16=f%d' % number for number in range(2, 128)]
        required += ['mov r16=b%d' % number for number in range(8)]
        for text in required:
            if counts[text] != 1:
                problems.append('the state probe requires exactly one %s' % text)
        # r9-r11 carry the count (LOOP); every other writable register a canary.
        for number in (1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
                       26, 27, 28, 29, 30, 31, *range(34, 128)):
            if counts['cmp.eq p7,p6=%d,r%d' % (number, number)] != (2 if number == 16 else 1):
                problems.append('general register %d has no canary check' % number)
        for text in DEADLINE + COUNT_CHECKS:
            if counts[text] != 1:
                problems.append('the count requires exactly one %s' % text)
        loop = code.unique(lambda step: step.operation.is_branch and step.target is not None
                           and step.target < step.offset, 'count loop', problems)
        if loop is not None:
            start = code.at_offset(loop.target)
            body = [] if start is None else [start] + code.window(after=start, before=loop)
            if tuple(step.text for step in body) != LOOP or loop.predicate != 'p6':
                problems.append('the count loop is not exactly %s and a branch on its deadline compare' % (LOOP,))
        if any(step.operation.memory is not None or step.operation.is_call for step in code.steps):
            problems.append('the spinner accesses memory or calls another routine')
        loss = code.first('break.i 0x4321')
        for compare in code.all(lambda step: step.operation.base == 'cmp' and step.text != LOOP[-1]):
            following = code.next(compare)
            if (following is None or following.predicate != 'p6' or loss is None
                    or following.target != loss.bundle):
                problems.append('a state comparison does not branch to the lost-state marker')
        return problems

    def mutants(self, code):
        for text in ('getf.sig r16=f2', 'getf.exp r16=f127', 'mov r33=pr', 'mov r16=b7',
                     'mov.i r16=ar.ec', 'mov.m r16=ar.rnat', 'cmp.eq p7,p6=127,r127'):
            yield 'missing ' + text, code.nop(code.site(text))
        yield 'yield in the CPU-bound loop', code.insert(code.site('adds r8=1,r8'), 'break.i 0x100000', after=True)
        yield 'count bounded without the ITC', code.nop(code.site('mov.m r10=ar.itc'))
        yield 'deadline not set from the ITC', code.nop(code.site('mov.m r11=ar.itc'))
        yield 'counter mirror unchecked', code.nop(code.site('cmp.eq p7,p6=r8,r9'))
        yield 'empty count accepted', code.nop(code.site('cmp.eq p6,p7=r0,r8'))
        yield 'lost state reported successful', code.replace(code.site('break.i 0x4321'), text='break.i 0x1234')
