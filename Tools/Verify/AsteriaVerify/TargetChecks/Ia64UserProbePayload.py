# SPDX-License-Identifier: Apache-2.0
"""IA-64 user-execution probe payload: the hand-written
code blob `AsteriaIa64UserExecutionProbePayloadStart`
(Source/Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.S) that the
kernel copies byte for byte from the linked image into the probe's executable
user page. It holds the parent frame, the terminal loop and the local spill
child, which issues four stopped slot-2 `break.i 0x100000` system calls from
a flushrs'd frame and publishes completion after the suspended WaitSetWait
resumes. The golden AsteriaIa64UserExecutionProbePayloadStart.dis pins the
extent in the objects; these properties run on the objects and the linked
images and cross-check Ia64UserExecutionProbePayload.hpp and the hosted copy.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Layout, Properties, Subjects
from AsteriaVerify.Checks import register
from AsteriaVerify.ControlFlow import Code, Step
from AsteriaVerify.Elf import (BINDING_NAMES, STB_GLOBAL, STB_WEAK, STT_FUNC, STT_OBJECT, STV_DEFAULT, TYPE_NAMES,
                               VISIBILITY_NAMES, Symbol, load)

PAYLOAD = 'AsteriaIa64UserExecutionProbePayloadStart'
LOOP = 'AsteriaIa64UserExecutionProbePostSystemCallLoop'
CHILD = 'AsteriaIa64UserExecutionProbeSpillChild'
HEADER = 'Source/Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.hpp'
SOURCE = 'Source/Platform/Ia64/Scheduler/Ia64UserExecutionProbePayload.cpp'
USER_PAGE_BYTES = 4096
PREFIX = 'Ia64UserExecutionProbe'
SYSTEM_CALL_BREAK = 'break.i 0x100000'
# (header constant of the child-relative break offset, of the system-call number)
SYSTEM_CALLS = (
    ('ChildQueryBreakInstructionOffset', 'CapabilityQuerySystemCallNumber'),
    ('ChildSendBreakInstructionOffset', 'IpcSendSystemCallNumber'),
    ('ChildReceiveBreakInstructionOffset', 'IpcReceiveSystemCallNumber'),
    ('ChildWaitBreakInstructionOffset', 'WaitSetWaitSystemCallNumber'),
)
MASK64 = (1 << 64) - 1


def hosted_bytes(text: str) -> bytes | None:
    """The `HostedPayloadBytes` initializer of the payload source."""
    match = re.search(r'HostedPayloadBytes\s*\[[^\]]*\]\s*\{(?P<body>.*?)\}\s*;', text, re.S)
    if match is None:
        return None
    return bytes(int(token, 16) for token in re.findall(r'0[xX]([0-9A-Fa-f]{1,2})[uU]?\b', match.group('body')))


def _header(context, *names: str) -> tuple[tuple[str, int | None], ...]:
    constants = Layout.constants(Layout.read(str(context.root / HEADER)))
    return tuple((name, constants.get(PREFIX + name)) for name in names)


def _missing(header: dict[str, int | None]) -> list[str]:
    return ['%s lacks the constant %s%s' % (HEADER, PREFIX, name) for name, value in header.items() if value is None]


def _with_header(facts, name: str, change: int):
    header = dict(facts.header)
    header[name] += change
    return dataclasses.replace(facts, header=tuple(header.items()))


class _PayloadProperty(Subjects.CodeProperty):
    SYMBOL = PAYLOAD
    IMAGES = True


GEOMETRY = ('PayloadSizeBytes', 'PayloadBundleCount', 'TerminalLoopOffsetBytes', 'ChildOffsetBytes',
            'ChildSizeBytes')


@dataclasses.dataclass(frozen=True)
class SymbolFacts:
    payload: tuple[Symbol, ...]     # every definition, for uniqueness
    loop: tuple[Symbol, ...]
    child: tuple[Symbol, ...]
    header: tuple[tuple[str, int | None], ...]


@register
class Ia64UserProbePayloadSymbols(_PayloadProperty):
    """The byte range the kernel copies into the single 4 KiB user code page
    is one hidden, self-contained code blob: the payload and the terminal
    loop are GLOBAL HIDDEN FUNC, the spill child a FUNC internal to the image
    (LOCAL, or GLOBAL HIDDEN once the asm moves into its own object), each
    defined once and all in one section; 0 < size <= 4096 and the payload is
    bundle aligned; the loop lies inside the payload and the child after the
    loop, inside the payload (so the copy contains the child call and the
    terminal loop). The ELF geometry equals Ia64UserExecutionProbePayload.hpp:
    size == PayloadSizeBytes == 16 * PayloadBundleCount (the kernel copies
    PayloadSizeBytes), loop at TerminalLoopOffsetBytes, child at
    ChildOffsetBytes with ChildSizeBytes."""

    def extract(self, context, subject):
        elf = load(subject.path)
        return SymbolFacts(elf.definitions(PAYLOAD), elf.definitions(LOOP), elf.definitions(CHILD),
                           _header(context, *GEOMETRY))

    def verify(self, facts):
        problems = ['%s has %d definitions, expected one' % (name, len(definitions))
                    for name, definitions in ((PAYLOAD, facts.payload), (LOOP, facts.loop), (CHILD, facts.child))
                    if len(definitions) != 1]
        if problems:
            return problems
        (payload,), (loop,), (child,) = facts.payload, facts.loop, facts.child
        for symbol, linkage, expected in ((payload, Properties.HIDDEN, 'GLOBAL HIDDEN'),
                                          (loop, Properties.HIDDEN, 'GLOBAL HIDDEN'),
                                          (child, Properties.INTERNAL, 'internal (LOCAL, or GLOBAL HIDDEN)')):
            if symbol.type != STT_FUNC:
                problems.append('%s is %s, not FUNC' % (symbol.name, TYPE_NAMES.get(symbol.type, symbol.type)))
            if (symbol.binding, symbol.visibility) not in linkage:
                problems.append('%s is %s %s, expected %s' % (
                    symbol.name, BINDING_NAMES.get(symbol.binding, symbol.binding),
                    VISIBILITY_NAMES.get(symbol.visibility, symbol.visibility), expected))
        problems += ['%s is not in the payload section' % symbol.name for symbol in (loop, child)
                     if symbol.section_index != payload.section_index]
        end = payload.value + payload.size
        if not 0 < payload.size <= USER_PAGE_BYTES:
            problems.append('the payload size %#x does not fit one user page' % payload.size)
        if payload.value % 16:
            problems.append('the payload at %#x is not bundle aligned' % payload.value)
        if loop.size == 0 or child.size == 0:
            problems.append('the terminal loop or the child has size zero')
        if not (payload.value <= loop.value and loop.value + loop.size <= end):
            problems.append('the terminal loop lies outside the payload')
        if not (loop.value + loop.size <= child.value and child.value + child.size <= end):
            problems.append('the child does not follow the terminal loop inside the payload')
        header = dict(facts.header)
        if _missing(header):
            return problems + _missing(header)
        return problems + ['payload %s is %#x, the header says %#x' % (label, actual, expected)
                           for label, actual, expected in (
                               ('size', payload.size, header['PayloadSizeBytes']),
                               ('size', payload.size, 16 * header['PayloadBundleCount']),
                               ('terminal-loop offset', loop.value - payload.value, header['TerminalLoopOffsetBytes']),
                               ('child offset', child.value - payload.value, header['ChildOffsetBytes']),
                               ('child size', child.size, header['ChildSizeBytes']))
                           if actual != expected]

    def mutants(self, facts):
        payload, loop, child = facts.payload[0], facts.loop[0], facts.child[0]

        def changed(field, symbol, **changes):
            return dataclasses.replace(facts, **{field: (dataclasses.replace(symbol, **changes),)})

        yield 'terminal loop in another section', changed('loop', loop, section_index=loop.section_index + 1)
        yield 'exported child (GLOBAL DEFAULT)', changed('child', child, binding=STB_GLOBAL, visibility=STV_DEFAULT)
        yield 'weak child', changed('child', child, binding=STB_WEAK, visibility=STV_DEFAULT)
        yield 'child as data', changed('child', child, type=STT_OBJECT)
        yield 'payload with default visibility', changed('payload', payload, visibility=STV_DEFAULT)
        yield 'payload larger than a page', changed('payload', payload, size=USER_PAGE_BYTES + 1)
        yield 'payload off a bundle boundary', changed('payload', payload, value=payload.value + 8)
        yield 'terminal loop beyond the payload', changed(
            'loop', loop, size=payload.value + payload.size - loop.value + 0x10)
        yield 'duplicate terminal loop', dataclasses.replace(facts, loop=facts.loop * 2)
        yield 'child offset differs from the header', changed(
            'child', child, value=child.value + 0x10, size=child.size - 0x10)
        yield 'header payload size differs', _with_header(facts, 'PayloadSizeBytes', 0x10)


# Privileged instructions, and epc, which raises the privilege level.
PRIVILEGED_BASES = frozenset({'rsm', 'ssm', 'rfi', 'bsw', 'itc', 'itr', 'ptc', 'ptr', 'epc', 'vmsw', 'tak', 'tpa'})


@dataclasses.dataclass(frozen=True)
class SelfContainedFacts:
    code: Code                                      # the payload extent with its relocations
    child_offset: int
    loop_offset: int
    parent_resume: int | None                       # header ParentResumeOffsetBytes
    copies: tuple[bytes, bytes] | None              # (linked image bytes, object bytes)


@register
class Ia64UserProbePayloadSelfContained(_PayloadProperty):
    """The copied page must run unchanged at its user address. (a) No
    relocation, read from the ELF `.rela` sections, lies in the payload
    extent (the old grep of `R_IA64_` in objdump text never fired). (b) No
    instruction uses r1 (gp). (d) The only call is an unpredicated
    `br.call b0=` to the child inside the payload, returning to the header's
    ParentResumeOffsetBytes; every direct branch stays inside the payload,
    the terminal loop branches to itself, and the only indirect branch is the
    child's single `br.ret`. New: no privileged instruction, control-register
    or PSR access, or kernel-register write appears (user mode); in linked
    images the payload bytes equal the object's (nothing was patched at link
    time)."""

    def extract(self, context, subject):
        elf = load(subject.path)
        payload, loop, child = (elf.symbol(name) for name in (PAYLOAD, LOOP, CHILD))
        copies = None
        if subject.linked:
            original = load(context.defining_object(subject.variant, PAYLOAD))
            copies = (elf.symbol_bytes(payload), original.symbol_bytes(original.symbol(PAYLOAD)))
        return SelfContainedFacts(super().extract(context, subject), child.value - payload.value,
                                  loop.value - payload.value,
                                  dict(_header(context, 'ParentResumeOffsetBytes'))['ParentResumeOffsetBytes'], copies)

    def verify(self, facts):
        code = facts.code
        problems = ['relocation %s lies in the payload' % (fact,) for fact in code.relocations]
        problems += ['%s uses gp (r1)' % step.describe() for step in code.steps if step.uses('r1')]
        # User code may only read ar.k0-7.
        problems += ['%s is privileged' % step.describe() for step in code.steps
                     if step.operation.base in PRIVILEGED_BASES
                     or any(name.startswith(('cr.', 'psr')) for name in step.operation.reads | step.operation.writes)
                     or any(name.startswith('ar.k') for name in step.operation.writes)]
        calls = code.all(lambda s: s.operation.is_call)
        if len(calls) != 1:
            problems.append('%d calls, expected one' % len(calls))
        else:
            call = calls[0]
            if call.target != facts.child_offset or call.instruction.predicate is not None \
                    or 'b0' not in call.operation.writes:
                problems.append('%s is not an unpredicated br.call b0= to the child' % call.describe())
            resume = call.bundle + 16
            if facts.parent_resume is None:
                problems.append('%s lacks the constant %sParentResumeOffsetBytes' % (HEADER, PREFIX))
            elif resume != facts.parent_resume:
                problems.append('the child returns to +%#x, the header says +%#x' % (resume, facts.parent_resume))
        problems += ['%s leaves the payload' % step.describe() for step in code.steps
                     if step.operation.is_direct_branch and step.target is None]
        loop = code.at_offset(facts.loop_offset)
        if (loop is None or loop.bundle != facts.loop_offset or loop.operation.mnemonic != 'br.few'
                or loop.target != facts.loop_offset or loop.instruction.predicate is not None):
            problems.append('the terminal loop at +%#x does not branch to itself' % facts.loop_offset)
        indirect = code.all(lambda s: s.operation.is_indirect_branch)
        if len(indirect) != 1 or not indirect[0].operation.is_return or not (
                facts.child_offset <= indirect[0].offset < code.instructions[-1].address - code.start + 16):
            problems.append('indirect branches %s, expected the child\'s single br.ret'
                            % [step.describe() for step in indirect])
        if facts.copies is not None and facts.copies[0] != facts.copies[1]:
            differing = next(i for i, (a, b) in enumerate(zip(*facts.copies)) if a != b) \
                if len(facts.copies[0]) == len(facts.copies[1]) else 0
            problems.append('the linked payload differs from the object at +%#x' % differing)
        return problems

    def mutants(self, facts):
        code = facts.code
        call = code.site(lambda s: s.operation.is_call)

        def with_code(changed):
            return dataclasses.replace(facts, code=changed)

        yield 'absolute relocation in the payload', with_code(dataclasses.replace(code, relocations=code.relocations + (
            Properties.RelocationFact(0x11, 'IMM64', PAYLOAD, 0),)))
        yield 'call to external memset', with_code(code.replace(call, text='br.call.sptk.many b0=<memset>'))
        yield 'predicated child call', with_code(code.replace(call, predicate='p06'))
        yield 'child call from another bundle', with_code(code.replace(call, address=call.instruction.address + 0x10))
        yield 'terminal loop escapes', with_code(code.replace(code.site(lambda s: s.offset >= facts.loop_offset),
                                                              text='br.few <+0x0>'))
        for description, text in (('gp operand', 'mov r14=r1'), ('second call', call.text),
                                  ('parent returns', 'br.ret.sptk.many b0'), ('interrupt mask change', 'rsm 0x4000'),
                                  ('control-register write', 'mov cr.iva=r14'),
                                  ('kernel-register write', 'mov.m ar.k3=r14')):
            yield description, with_code(code.insert(call, text, after=True))
        if facts.copies is not None:
            image, original = facts.copies
            yield 'linked payload patched', dataclasses.replace(
                facts, copies=(image[:0x17] + bytes([image[0x17] ^ 0x01]) + image[0x18:], original))


CONSTANTS = tuple(name for pair in SYSTEM_CALLS for name in pair) + (
    'ChildFlushInstructionOffset', 'IpcEvidenceVirtualAddress', 'IpcCompletionOffsetBytes',
    'RseSentinelAddress', 'Sentinel')


@dataclasses.dataclass(frozen=True)
class SystemCallFacts:
    code: Code
    values: tuple[int | None, ...]      # movl immediates from the artifact bytes, per instruction
    child_offset: int
    header: tuple[tuple[str, int | None], ...]


def _constants(code: Code, values: tuple[int | None, ...]) -> list[dict[str, int]]:
    """Known general-register constants before each step (straight-line
    propagation of movl, mov, adds and post-increments; r0 is 0)."""
    known: dict[str, int] = {'r0': 0}
    states = []
    for step in code.steps:
        states.append(dict(known))
        operation = step.operation
        written, sources = set(operation.general_writes), list(operation.general_reads)
        result = None
        if step.instruction.predicate is not None:
            pass                                            # conditional write
        elif operation.base == 'movl':
            result = values[step.position]
        elif (operation.base in ('mov', 'adds', 'addl') and operation.memory is None
              and len(sources) == len(operation.reads) and len(operation.immediates) <= 1):
            offset = sum(operation.immediates)
            if not sources:
                result = offset
            elif len(sources) == 1 and sources[0] in known:
                result = (known[sources[0]] + offset) & MASK64
        if operation.memory is not None and operation.memory_base in written:          # post-increment
            base = operation.memory_base
            written.discard(base)
            increment = operation.immediates[-1] if operation.immediates else None
            if base in known and increment is not None:
                known[base] = (known[base] + increment) & MASK64
            else:
                known.pop(base, None)
        for register in written:
            if result is not None and len(written) == 1:
                known[register] = result
            else:
                known.pop(register, None)
    return states


def _last_writer(code: Code, step: Step, test) -> Step | None:
    return next((earlier for earlier in reversed(code.window(before=step)) if test(earlier)), None)


@register
class Ia64UserProbePayloadSystemCalls(_PayloadProperty):
    """(c) The child issues exactly four system calls, each a
    `break.i 0x100000` in bundle slot 2 followed by a stop, at the child
    offsets of Ia64UserExecutionProbePayload.hpp (f); the `mov r15=imm` that
    reaches each break (the last write of r15 before it) carries
    CapabilityQuery, IpcSend, IpcReceive and WaitSetWait, in that order, with
    the header's numbers. The child executes exactly one `flushrs`, first in
    its group and with no other instruction after it in the group, at
    ChildFlushInstructionOffset, before the first call (f). (e) Completion is
    published by the only predicated store, `(p6) st8 [r16]=r18`, after the
    WaitSetWait break under a predicate computed after it, at
    IpcEvidenceVirtualAddress + IpcCompletionOffsetBytes, which the child
    cleared with r0 before its first call. New: the parent stores the header
    Sentinel before the call and its RSE verdict at RseSentinelAddress after
    it."""

    def extract(self, context, subject):
        elf = load(subject.path)
        payload, child = elf.symbol(PAYLOAD), elf.symbol(CHILD)
        code = super().extract(context, subject)
        return SystemCallFacts(code, Properties.resolved_values(elf, payload.section_index, code.instructions),
                               child.value - payload.value, _header(context, *CONSTANTS))

    def verify(self, facts):
        header = dict(facts.header)
        if _missing(header):
            return _missing(header)
        code = facts.code
        constants = dict(zip((step.position for step in code.steps), _constants(code, facts.values)))
        breaks = code.all(lambda s: s.operation.base == 'break')
        if len(breaks) != len(SYSTEM_CALLS):
            return ['%d breaks, expected %d system calls' % (len(breaks), len(SYSTEM_CALLS))]
        problems = []
        for step, (offset_name, number_name) in zip(breaks, SYSTEM_CALLS):
            label = '%s (%s)' % (step.describe(), number_name)
            if step.rendered != SYSTEM_CALL_BREAK or step.instruction.slot != 2 or not step.instruction.stop_after:
                problems.append('%s is not a stopped slot-2 %s' % (label, SYSTEM_CALL_BREAK))
            if step.offset - facts.child_offset != header[offset_name]:
                problems.append('%s lies at child+%#x, the header says +%#x' % (
                    label, step.offset - facts.child_offset, header[offset_name]))
            writer = _last_writer(code, step, lambda s: s.writes('r15'))
            if writer is None or writer.operation.mnemonic != 'mov' or writer.operation.reads \
                    or writer.operation.immediates != (header[number_name],):
                problems.append('%s is reached by %s, not mov r15=%d' % (
                    label, writer.describe() if writer else 'no r15 write', header[number_name]))
        flushes = code.all(lambda s: s.operation.base == 'flushrs')
        if len(flushes) != 1:
            problems.append('%d flushrs, expected one' % len(flushes))
        else:
            flush = flushes[0]
            if flush.offset - facts.child_offset != header['ChildFlushInstructionOffset']:
                problems.append('flushrs lies at child+%#x, the header says +%#x' % (
                    flush.offset - facts.child_offset, header['ChildFlushInstructionOffset']))
            if not code.starts_group(flush):
                problems.append('flushrs is not the first instruction of its group')
            if not code.ends_group(flush):
                problems.append('flushrs is not group-stopped: another instruction shares its group')
            if flush.position > breaks[0].position:
                problems.append('flushrs follows the first system call')
        completion_address = (header['IpcEvidenceVirtualAddress'] + header['IpcCompletionOffsetBytes']) & MASK64
        stores = code.all(lambda s: s.operation.memory == 'store')
        predicated = [step for step in stores if step.instruction.predicate is not None]
        if len(predicated) != 1:
            problems.append('%d predicated stores, expected the completion store' % len(predicated))
        else:
            completion, last = predicated[0], breaks[-1]
            setter = _last_writer(code, completion, lambda s: s.operation.writes_predicate(completion.predicate))
            if completion.text != 'st8 [r16]=r18' or completion.predicate != 'p6':
                problems.append('the completion store is %s, not (p6) st8 [r16]=r18' % completion.rendered)
            if completion.position < last.position or setter is None or setter.position < last.position:
                problems.append('completion is not decided and published after the WaitSetWait break')
            address = constants[completion.position].get(completion.operation.memory_base)
            if address != completion_address:
                problems.append('completion is published at %s, the header says %#x' % (
                    'an unknown address' if address is None else '%#x' % address, completion_address))
            if not [step for step in stores if step.position < breaks[0].position and step.text.endswith('=r0')
                    and constants[step.position].get(step.operation.memory_base) == completion_address]:
                problems.append('the completion word is not cleared before the first system call')
        call = code.first(lambda s: s.operation.is_call)
        parent = [step for step in stores if step.offset < facts.child_offset and call is not None]
        before = [step for step in parent if step.position < call.position]
        after = [step for step in parent if step.position > call.position]
        # The register a parent store writes to memory.
        stored = before and sorted(before[0].operation.general_reads - {before[0].operation.memory_base})
        if not stored or len(stored) != 1 or constants[before[0].position].get(stored[0]) != header['Sentinel']:
            problems.append('the parent does not store the header Sentinel before the child call')
        if len(after) != 1 or constants[after[0].position].get(
                after[0].operation.memory_base) != header['RseSentinelAddress']:
            problems.append('the parent does not publish its RSE verdict once at RseSentinelAddress')
        return problems

    def mutants(self, facts):
        code = facts.code
        breaks = code.all(lambda s: s.operation.base == 'break')

        def with_code(changed, values=None):
            return dataclasses.replace(facts, code=changed, values=facts.values if values is None else values)

        def inserted_values(position):
            return facts.values[:position] + (None,) + facts.values[position:]

        def with_value(step, change):
            values = list(facts.values)
            values[step.position] = change(values[step.position] or 0)
            return with_code(code, tuple(values))

        yield 'break.i 0x100001', with_code(code.replace(breaks[1], text='break.i 0x100001'))
        send = code.site(lambda s: s is breaks[0], lambda s: s.writes('r15'))
        receive = code.site(lambda s: s is breaks[1], lambda s: s.writes('r15'))
        yield 'IpcSend and IpcReceive numbers swapped', with_code(
            code.replace(send, text=receive.text).replace(receive, text=send.text))
        yield 'no stop after a system call', with_code(code.without_stops(breaks[2], breaks[2].position + 1))
        yield 'system call in slot 1', with_code(code.replace(breaks[3], slot=1))
        yield 'fifth system call', with_code(code.insert(breaks[3], breaks[3].text, after=True),
                                             inserted_values(breaks[3].position + 1))
        completion = code.site(lambda s: s.operation.memory == 'store' and s.instruction.predicate is not None)
        yield 'unpredicated completion store', with_code(code.replace(completion, predicate=None))
        yield 'completion published before WaitSetWait', with_code(
            code.nop(completion).insert(breaks[3], completion.rendered), inserted_values(breaks[3].position))
        yield 'completion at another address', with_code(code.insert(completion, 'adds r16=8,r16'),
                                                         inserted_values(completion.position))
        flush = code.site(lambda s: s.operation.base == 'flushrs')
        yield 'missing flushrs', with_code(code.nop(flush))
        yield 'flushrs shares its group with the next instruction', with_code(code.without_boundary(flush))
        yield 'flushrs after another instruction of its group', with_code(
            code.without_stops(flush.position - 1, flush))
        yield 'RSE verdict at another address', with_value(
            code.site(lambda s: s.operation.is_call, lambda s: s.operation.base == 'movl'), lambda value: value + 8)
        yield 'parent sentinel differs from the header', with_value(
            code.site(lambda s: s.operation.base == 'movl'), lambda value: value ^ 1)
        for name, change in (('ChildWaitBreakInstructionOffset', 0x10), ('IpcSendSystemCallNumber', -1),
                             ('ChildFlushInstructionOffset', 6)):
            yield 'header %s differs' % name, _with_header(facts, name, change)


@register
class Ia64UserProbePayloadHostedCopy(_PayloadProperty):
    """New. Host builds use the `HostedPayloadBytes` array of
    Ia64UserExecutionProbePayload.cpp in place of the assembled payload,
    and the host tests exercise that copy; it must equal the target
    payload bytes, or the host tests validate a stale payload."""
    IMAGES = False

    def extract(self, context, subject):
        """(target payload bytes, the hosted copy or None)."""
        elf = load(subject.path)
        return elf.symbol_bytes(elf.symbol(PAYLOAD)), hosted_bytes((context.root / SOURCE).read_text())

    def verify(self, facts):
        target, hosted = facts
        if hosted is None:
            return ['%s has no HostedPayloadBytes initializer' % SOURCE]
        if hosted == target:
            return []
        if len(hosted) != len(target):
            return ['HostedPayloadBytes has %d bytes, the payload %d' % (len(hosted), len(target))]
        first = next(i for i, (a, b) in enumerate(zip(hosted, target)) if a != b)
        return ['HostedPayloadBytes differs from the payload from byte %#x on' % first]

    def mutants(self, facts):
        target = facts[0]
        yield 'hosted copy with a flipped bit', (target, target[:0x100] + bytes([target[0x100] ^ 0x80])
                                                 + target[0x101:])
        yield 'hosted copy one bundle short', (target, target[:-16])
        yield 'no hosted copy', (target, None)
