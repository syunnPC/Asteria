# SPDX-License-Identifier: Apache-2.0
"""Native PL3 break dispatch: preserve PR and select only the ABI immediate."""
from AsteriaVerify import Subjects
from AsteriaVerify.Checks import register


@register
class Ia64UserBreakEntrySymbol(Subjects.SymbolContract):
    SYMBOLS = ('AsteriaIa64UserBreakEntry',)
    IMAGES = True


@register
class Ia64UserBreakDispatch(Subjects.CodeProperty):
    SYMBOL = 'AsteriaIa64UserBreakEntry'
    IMAGES = True

    def verify(self, code):
        steps = code.steps
        expected = ('mov r31=pr', 'mov r17=cr.iim', 'movl r18=0x100000', 'cmp.eq p6,p7=r17,r18',
                    None, 'mov pr=r31,0xfffffffffffffffe', 'mov.m ar.k4=44', 'mov r16=6', None,
                    'mov pr=r31,0xfffffffffffffffe', None)
        if len(steps) != len(expected):
            return ['break dispatch does not contain the eleven required operations']
        problems = []
        for step, text in zip(steps, expected):
            if text is not None and (step.text != text or step.predicate):
                problems.append('missing unpredicated %s' % text)
        branch = steps[4]
        if (branch.mnemonic != 'br.cond.sptk.few' or branch.predicate != 'p6'
                or branch.target != steps[9].bundle):
            problems.append('the ABI immediate does not branch to PR restoration before the syscall entry')
        for index, target in ((8, 'AsteriaIa64UserExceptionEntry'), (10, 'AsteriaIa64SystemCallEntry')):
            step = steps[index]
            edge = step.operation.target
            if (step.operation.base != 'br' or step.predicate or edge is None
                    or edge.symbol != target or edge.offset or step.operation.is_call):
                problems.append('dispatch edge does not tail branch to %s' % target)
        return problems

    def mutants(self, code):
        yield 'another ABI immediate', code.replace(code.site('movl r18=0x100000'), text='movl r18=0x100001')
        yield 'PR clobbered before the exception', code.nop(code.steps[5])
        yield 'PR clobbered before the syscall', code.nop(code.steps[9])
        yield 'wrong vector', code.replace(code.steps[6], text='mov.m ar.k4=84')
        yield 'wrong selector', code.replace(code.steps[7], text='mov r16=4')
        yield 'ABI branch into exception route', code.retarget(code.steps[4], code.steps[5].bundle)
        yield 'static register clobbered', code.insert(code.steps[2], 'mov r4=r0', after=True)
