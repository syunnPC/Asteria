# SPDX-License-Identifier: Apache-2.0
"""Linked-code checks for scrub-window ownership, CR4 and leaf control flow.

The seed builders store virtual addresses as mapping metadata; architectural
window addresses are formed only in the scrub leaf, from its own GS index.
The leaf's control-flow proof tracks repoint/invalidate/use/restore on every
path, including its bounded page and sample loops.
"""
from __future__ import annotations

import dataclasses
import re

from AsteriaVerify import Subjects
from AsteriaVerify.Elf import load
from AsteriaVerify.Checks import Property, register
from AsteriaVerify.TargetChecks.Amd64ExtendedState import decode, KernelInstruction

LEAF = 'AsteriaAmd64ZeroScrubChunk'
CR4_SITES = ('Amd64UserEntry7Install', 'InitialAddressSpacePreparation8Activate',
             'EstablishCurrentControlState', 'AsteriaAmd64ActivateInitialAddressSpace')
SEED_SITES = ('ConfigureScrubber', 'PrepareAmd64BootRuntime', 'BootstrapMappingPlan')
DISABLED = (1 << 7) | (1 << 17)  # Intel SDM Vol. 3A 253668-093US, section 2.5


@dataclasses.dataclass(frozen=True)
class Facts:
    code: tuple[KernelInstruction,...]
    startup: bytes = b''


def leaf_problems(code):
    leaf = [step for step in code if step.symbol == LEAF]
    if not leaf:
        return ['no scrub leaf']
    problems = []
    texts = [step.text for step in leaf]
    gs = [i for i, text in enumerate(texts) if text == 'mov %gs:0x10,%r9']
    address = [i for i, text in enumerate(texts) if text == 'lea 0x70001000(%r9),%r10']
    if len(gs) != 1 or len(address) != 1 or address[0] <= gs[0]:
        problems.append('window ownership: window must be formed once from GS:16')
    elif texts[address[0] - 1] != 'shl $0xc,%r9' or any(
            re.search(r',%r9[dwb]?$', text) and not text.startswith(('cmp ', 'test ')) for text in texts[gs[0] + 1:address[0] - 1]):
        problems.append('window ownership: GS index overwritten before window formation')
    if address and any(re.search(r',%r10[dwb]?$', text) and not text.startswith(('cmp ', 'test '))
                       for text in texts[address[0] + 1:]):
        problems.append('window ownership: own window address overwritten')
    for index, text in enumerate(texts):
        if text.startswith('rep stos'):
            destinations = [prior for prior in texts[:index]
                            if re.search(r',%(?:rdi|edi|di|dil)$', prior)
                            and not prior.startswith(('cmp ', 'test '))]
            if not destinations or destinations[-1] != 'mov %r10,%rdi':
                problems.append('window ownership: zero destination does not come from own window')
    for step in code:
        if '70001000' in step.text and step.symbol != LEAF and not any(site in step.symbol for site in SEED_SITES):
            problems.append('window ownership: foreign window materialization in ' + step.symbol)
    if any(text.startswith('call') or text.split()[0] in ('sti', 'popf', 'popfq', 'iret', 'iretq') for text in texts):
        problems.append('leaf control flow: call or interrupt-enable in leaf')

    positions = {step.address: index for index, step in enumerate(leaf)}
    # (instruction, interrupts masked, seed repointed, invalidated, window used)
    pending = [(0, False, False, False, False)]
    seen = set()
    while pending:
        state = pending.pop()
        if state in seen:
            continue
        seen.add(state)
        index, masked, dirty, invalidated, used = state
        if index >= len(leaf):
            problems.append('leaf control flow: path leaves leaf extent')
            continue
        text = texts[index]
        mnemonic = text.split()[0]
        if mnemonic == 'cli':
            masked = True
        if text in ('mov %rax,(%r12)', 'mov %r13,(%r12)'):
            if not masked:
                problems.append('leaf control flow: leaf write with interrupts enabled')
            dirty = text == 'mov %rax,(%r12)'
            invalidated = False
        if text == 'invlpg (%r10)':
            invalidated = True
        if ('(%r10' in text and mnemonic != 'invlpg') or text.startswith('rep stos'):
            if not masked or not dirty or not invalidated:
                problems.append('leaf control flow: window used before repoint/invlpg')
            used = True
        if mnemonic in ('ret', 'retq'):
            if dirty or (used and not invalidated):
                problems.append('leaf control flow: return before seed restore/invlpg')
            continue
        successors = [index + 1]
        if mnemonic.startswith('j'):
            target = re.match(r'j\w+ ([0-9a-f]+) ', text)
            if target is None or int(target[1], 16) not in positions:
                problems.append('leaf control flow: branch outside leaf')
                continue
            successors = [positions[int(target[1], 16)]] + ([] if mnemonic == 'jmp' else successors)
        pending.extend((next_index, masked, dirty, invalidated, used) for next_index in successors)
    return sorted(set(problems))


def cr4_problems(code):
    problems = []
    writes = 0
    for index, step in enumerate(code):
        if step.text.startswith('invpcid'):
            problems.append('CR4: INVPCID present')
        write = re.fullmatch(r'mov (%\w+),%cr4', step.text)
        if not write:
            continue
        writes += 1
        if not any(site in step.symbol for site in CR4_SITES):
            problems.append('CR4: unlisted CR4 writer ' + step.symbol)
        prior = code[index - 1] if index else None
        mask = prior and re.fullmatch(r'and \$0x([0-9a-f]+),' + re.escape(write[1]), prior.text)
        if not mask or (int(mask[1], 16) & DISABLED):
            problems.append('CR4: CR4 write without adjacent PGE/PCIDE clear')
    if not writes:
        problems.append('CR4: no CR4 sites decoded')
    return problems


@register
class Amd64ScrubWindow(Property):
    isas = ('Amd64',)
    variants = ('firmware', 'firmware-test', 'O0', 'O2')

    def extract(self, context, variant):
        path = context.artifact(variant + ':kernel_elf' if variant.startswith('firmware') else Subjects.image_key(variant))
        elf = load(path)
        start, end = elf.symbol('AsteriaAmd64StartupCodeStart'), elf.symbol('AsteriaAmd64StartupCodeEnd')
        return Facts(decode(context.tool('objdump'), path), elf.read_address(start.value, end.value - start.value))

    def verify(self, facts):
        # The startup blob executes as 16-bit code, although stored read-only
        # in a 64-bit image. Check its sole MOV to CR4 and adjacent ANDL.
        write = bytes.fromhex('0f22e0')
        mask = bytes.fromhex('6625') + ((~DISABLED) & 0xffffffff).to_bytes(4, 'little')
        startup = [] if facts.startup.count(write) == 1 and mask + write in facts.startup else [
            'CR4: startup CR4 writer does not clear PGE/PCIDE']
        return leaf_problems(facts.code) + cr4_problems(facts.code) + startup

    def mutants(self, facts):
        for index, step in enumerate(facts.code):
            substitutions = []
            if step.symbol == LEAF:
                if step.text == 'mov %gs:0x10,%r9':
                    substitutions += ['mov %rsi,%r9', 'mov %gs:0x8,%r9']
                if step.text == 'cli':
                    substitutions += ['nop', 'sti', 'call 0 <foreign>']
                if step.text == 'mov %r10,%rdi':
                    substitutions += ['mov %rsi,%rdi', 'mov %rsi,%r10']
                if step.text == 'invlpg (%r10)' or step.text == 'mov %r13,(%r12)':
                    substitutions += ['nop']
            if re.fullmatch(r'mov %\w+,%cr4', step.text) and index:
                prior = facts.code[index - 1]
                changed = list(facts.code)
                changed[index - 1] = dataclasses.replace(prior, text='nop')
                yield 'CR4: CR4 clear removed at %#x' % step.address, dataclasses.replace(facts, code=tuple(changed))
            for text in substitutions:
                changed = list(facts.code)
                changed[index] = dataclasses.replace(step, text=text)
                yield '%#x: %s -> %s' % (step.address, step.text, text), dataclasses.replace(facts, code=tuple(changed))
        first = facts.code[0]
        yield 'CR4: INVPCID added', dataclasses.replace(facts, code=(dataclasses.replace(first, text='invpcid (%rax),%rbx'),) + facts.code)
        yield 'window ownership: foreign slot address', dataclasses.replace(facts, code=(dataclasses.replace(first,
            text='lea 0x70001000(%rsi),%rax', symbol='foreign'),) + facts.code)
        for bit in (7, 17):
            old = ((~DISABLED) & 0xffffffff).to_bytes(4, 'little')
            new = (((~DISABLED) & 0xffffffff) | (1 << bit)).to_bytes(4, 'little')
            yield 'CR4: startup enables bit %d' % bit, dataclasses.replace(facts, startup=facts.startup.replace(old, new))
