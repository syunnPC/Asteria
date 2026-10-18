# SPDX-License-Identifier: Apache-2.0
"""Manual-derived guest generators: fault opcode and bounded RSE frames."""
from collections import Counter

from AsteriaVerify import Subjects
from AsteriaVerify.Checks import register

REQUIRED = {
    'Break': ('break.i 0x0',),
    'Privileged': ('mov r8=psr',),
    'NatRegister': ('mov r15=-1', 'mov.m ar.unat=r15', 'ld8.fill r15=[r4]', 'st8 [r4]=r15'),
    'Unaligned': ('sum 0x8', 'adds r15=1,r4', 'ld8 r8=[r15]'),
    'DataRights': ('st8 [r4]=r0',),
    'Speculation': ('mov r15=-1', 'mov.m ar.unat=r15', 'ld8.fill r15=[r4]', 'mov r8=0', 'mov r8=77'),
    'Instruction': ('mov b6=r4',),
    'Rse': ('mov.m ar.rsc=r0', 'mov.m ar.bspstore=r4', 'mov.m ar.rnat=r0', 'mov r15=12',
            'mov.m ar.rsc=r15', 'mov r32=12'),
    'RseGrow': ('alloc r34=ar.pfs,82,81,0', 'adds r113=-1,r32', 'mov r36=r32', 'cmp.eq p7,p6=r36,r32',
               'mov.i ar.pfs=r34', 'mov b0=r35', 'br.ret.sptk.many b0'),
}


def _is_check(step) -> bool:
    return step.mnemonic == 'chk.s.i'


def _checks_into_recovery(code) -> bool:
    """One chk.s.i of r15, whose target is the recovery's exit code 77."""
    checks = code.all(_is_check)
    if len(checks) != 1 or checks[0].operation.reads != frozenset({'r15'}):
        return False
    recovery = code.target(checks[0])
    return recovery is not None and recovery.text == 'mov r8=77'


@register
class Ia64GuestFaultGenerators(Subjects.CodeProperty):
    def subjects(self, context):
        key = 'firmware-test:pager_elf'
        return [Subjects.Subject(key, 'firmware-test', context.artifact(key), True)] if context.has_artifact(key) else []

    def extract(self, context, subject):
        return tuple((name, Subjects.code(context, subject.path, 'AsteriaIa64Fault' + name)) for name in REQUIRED)

    def verify(self, facts):
        problems = []
        for name, code in facts:
            operations = Counter(step.text for step in code.steps)
            for required, count in Counter(REQUIRED[name]).items():
                if operations[required] != count:
                    problems.append('%s requires exactly %d %s' % (name, count, required))
            if any(step.operation.memory_base == 'r12' for step in code.steps):
                problems.append('%s uses an unbounded memory-stack recursion' % name)
            if name == 'Speculation' and not _checks_into_recovery(code):
                problems.append('Speculation has no chk.s of the NaT register branching to its recovery exit')
        return problems

    def mutants(self, facts):
        for index, (name, code) in enumerate(facts):
            for required in REQUIRED[name]:
                yield name + ': missing ' + required, facts[:index] + ((name, code.nop(code.site(required))),) + facts[index + 1:]
            if name == 'Speculation':
                yield name + ': no check', facts[:index] + ((name, code.nop(code.site(_is_check))),) + facts[index + 1:]
