# SPDX-License-Identifier: Apache-2.0
"""IA-64 extended state.

The kernel leaves the high floating-point partition f32-f127 to user threads:
it saves and restores those registers only in the user entries' capture and
restore and in the native context switch, and may not otherwise read or write
them, since a user thread's values stay live in them while the kernel runs.
The kernel is compiled with `-mfixed-range=f32-f127`; this checks the result,
in the firmware kernel and in the compile-check closure images. The low
partition f2-f31 is the kernel's own (f6-f15 are system-call scratch).
"""
from __future__ import annotations

import dataclasses
import re
import subprocess

from AsteriaVerify import Layout, Subjects
from AsteriaVerify.Elf import load, STT_FUNC
from AsteriaVerify.Checks import Property, register

# The symbols (substrings of their names) that may save and restore the high
# partition: the PL3 fault, exception and interrupt entries, which spill
# f2-f127 into the thread's Ia64PageFaultFrame and fill them back, the native
# preemption probe's capture and restore of a kernel loop, and the context
# switch's spill and fill of a thread's f32-f127 area.
SPILL_FILL_SYMBOLS: tuple[str,...] = ('AsteriaIa64UserFaultEntry', 'AsteriaIa64UserExceptionEntry',
                                       'AsteriaIa64UserInterruptEntry',
                                       'AsteriaIa64ExternalPreemptionNativeEntry',
                                       'AsteriaIa64ExternalPreemptionRestoreTail',
                                       'AsteriaIa64SaveHighFloatingPartition',
                                       'AsteriaIa64RestoreHighFloatingPartition')

_LABEL = re.compile(r'^[0-9a-f]+ <(?P<symbol>[^>]+)>:$')
_INSTRUCTION = re.compile(r'^\s*(?P<address>[0-9a-f]+):\s+(?:\[[A-Z]{3}\]\s+)?(?P<text>\S.*?)(?:;;)?$')
_TARGET = re.compile(r'<[^>]*>')
# F32-f127 as an operand: not part of a hexadecimal number or a name.
_HIGH = re.compile(r'(?<![0-9A-Za-z_])f(3[2-9]|[4-9][0-9]|1[01][0-9]|12[0-7])(?![0-9])')


@dataclasses.dataclass(frozen=True)
class KernelInstruction:
    symbol: str
    address: int
    text: str


@dataclasses.dataclass(frozen=True)
class HighPartitionFacts:
    instructions: tuple[KernelInstruction,...]


def decode(objdump: str, path) -> tuple[KernelInstruction,...]:
    """Every instruction slot of every executable section, with its symbol."""
    result = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(path)], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError('%s failed for %s: %s' % (objdump, path, result.stderr.strip()))
    functions = [entry for entry in load(path).defined_symbols() if entry.type == STT_FUNC and entry.size]
    symbol = '?'
    instructions = []
    for line in result.stdout.splitlines():
        label = _LABEL.match(line)
        if label:
            # PF01's normalized landing is an interior label of the complete
            # exception entry, not a separate function or state owner.
            if (not label.group('symbol').startswith(('AsteriaIa64StopPoll', 'AsteriaIa64RuntimeRefillStopPoll'))
                    and label.group('symbol') != 'AsteriaIa64UserExceptionEntryNormalized'):
                symbol = label.group('symbol')
            continue
        match = _INSTRUCTION.match(line)
        if match:
            instructions.append(KernelInstruction(symbol, int(match.group('address'), 16),
                                                  ' '.join(match.group('text').split())))
    return tuple(instructions)


def high_registers(text: str) -> tuple[str,...]:
    return tuple('f' + number for number in _HIGH.findall(_TARGET.sub('', text)))


def allowed(instruction: KernelInstruction) -> bool:
    return any(name in instruction.symbol for name in SPILL_FILL_SYMBOLS)


@register
class Ia64KernelLowFloatingPartition(Property):
    """No IA-64 kernel instruction reads or writes f32-f127, except in the PL3
    fault and interrupt entries, the native preemption probe and the context
    switch's area spill and fill, which save and restore them."""

    isas = ('Ia64',)

    def extract(self, context, variant):
        return HighPartitionFacts(decode(context.tool('objdump'), context.artifact(Subjects.image_key(variant))))

    def verify(self, facts):
        if not facts.instructions:
            return ['the image decodes to no instruction']
        return ['"%s" at %#x in %s uses %s' % (instruction.text, instruction.address, instruction.symbol,
                                               ', '.join(high_registers(instruction.text)))
                for instruction in facts.instructions
                if high_registers(instruction.text) and not allowed(instruction)]

    def mutants(self, facts):
        first = next(instruction for instruction in facts.instructions if not allowed(instruction))
        index = facts.instructions.index(first)
        for text in ('setf.sig f32=r14', 'xmpy.l f40=f32,f33', 'getf.sig r8=f127', 'ldf.fill f64=[r14]',
                     'stf.spill [r12]=f100', 'mov f45=f6'):
            yield 'uses the high partition: %s' % text, dataclasses.replace(
                facts, instructions=facts.instructions[:index] + (dataclasses.replace(first, text=text),)
                + facts.instructions[index + 1:])
        yield 'nothing decoded', dataclasses.replace(facts, instructions=())


# The PL3 entries that call into compiled kernel code: the
# system call and the fault, exception and interrupt expansions of one
# macro; and where the kernel's default fpsr is stated.
FPSR_ENTRIES: tuple[str,...] = ('AsteriaIa64SystemCallEntry', 'AsteriaIa64UserFaultEntry',
                                 'AsteriaIa64UserExceptionEntry', 'AsteriaIa64UserInterruptEntry')
CONTEXT_HEADER = 'Source/Platform/Ia64/Scheduler/Ia64KernelContext.hpp'
KERNEL_FPSR = 'Ia64KernelDefaultFloatingPointStatus'
_FPSR_WRITE = re.compile(r'^mov(?:\.m)? ar\.fpsr=(?P<register>r\d+)$')


@dataclasses.dataclass(frozen=True)
class FloatingStatusFacts:
    default: int | None
    instructions: tuple[KernelInstruction,...]


def entry_problems(name: str, body: tuple[KernelInstruction,...], default: int) -> list[str]:
    """The entry loads the kernel's default fpsr after `ssm psr.ic` and before
    its first call, through a register it set to the constant, and writes
    ar.fpsr again after the call (the user's, on the way out)."""
    calls = [index for index, instruction in enumerate(body) if instruction.text.startswith('br.call') and 'b6=' not in instruction.text]
    if not calls:
        return ['%s calls nothing' % name]
    call = calls[0]
    # Objdump renders `ssm psr.ic` as the mask it sets (PSR.ic is bit 13).
    enables = [index for index in range(call) if body[index].text in ('ssm psr.ic', 'ssm 0x2000')]
    if not enables:
        return ['%s calls the kernel before it enables collection' % name]
    writes = [index for index in range(call) if _FPSR_WRITE.match(body[index].text)]
    if len(writes) != 1 or writes[0] < enables[-1]:
        return ['%s writes ar.fpsr %d times before its first call, expected once after ssm psr.ic'
                % (name, len(writes))]
    register = _FPSR_WRITE.match(body[writes[0]].text).group('register')
    loads = [index for index in range(enables[-1], writes[0])
             if re.match(r'^movl %s=0x%x$' % (register, default), body[index].text)]
    problems = [] if loads else ['%s loads ar.fpsr from %s, which holds no %#x' % (name, register, default)]
    if not any(_FPSR_WRITE.match(instruction.text) for instruction in body[call + 1:]):
        problems.append('%s never restores the interrupted fpsr' % name)
    return problems


@register
class Ia64KernelFloatingPointStatus(Property):
    """Compiled kernel code runs under the kernel's default fpsr, all traps
    disabled, never under the interrupted user's: each PL3
    entry that calls the kernel loads Ia64KernelDefaultFloatingPointStatus
    after `ssm psr.ic` (the pre-cover capture stays untouched) and before
    its first call, and writes ar.fpsr again after it, restoring the user's
    captured value."""

    isas = ('Ia64',)

    def extract(self, context, variant):
        constants = Layout.constants(Layout.read(context.root / CONTEXT_HEADER))
        instructions = decode(context.tool('objdump'), context.artifact(Subjects.image_key(variant)))
        return FloatingStatusFacts(constants.get(KERNEL_FPSR),
                                   tuple(instruction for instruction in instructions
                                         if instruction.symbol in FPSR_ENTRIES))

    def verify(self, facts):
        if facts.default is None:
            return ['%s is no longer stated in %s' % (KERNEL_FPSR, CONTEXT_HEADER)]
        problems = []
        for name in FPSR_ENTRIES:
            body = tuple(instruction for instruction in facts.instructions if instruction.symbol == name)
            problems += entry_problems(name, body, facts.default) if body else ['%s is missing' % name]
        return problems

    def mutants(self, facts):
        def edited(change):
            return dataclasses.replace(facts, instructions=tuple(change(instruction)
                                                                 for instruction in facts.instructions))

        first_write = next(instruction for instruction in facts.instructions
                           if instruction.symbol == FPSR_ENTRIES[0] and _FPSR_WRITE.match(instruction.text))
        yield 'no kernel fpsr', edited(lambda instruction: dataclasses.replace(instruction, text='nop.m 0x0')
                                       if instruction == first_write else instruction)
        yield 'the user fpsr kept', dataclasses.replace(facts, default=(facts.default or 0) ^ 0x3F)
        yield 'no default stated', dataclasses.replace(facts, default=None)
        yield 'entries missing', dataclasses.replace(facts, instructions=())
