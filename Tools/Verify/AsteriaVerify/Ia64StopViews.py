# SPDX-License-Identifier: Apache-2.0
"""Separate processor stop policy polls from existing entry admission proofs.

Ia64StopPolling checks every labelled poll on the raw code, including its
immutable ar.k3 corridor, latch acquire, preserved PR and terminal branch.
The established capture/admission properties operate on the remaining
instructions with original addresses, groups and branch targets. Terminal
edges remain in that view; their PCREL21B relocations are checked by
Ia64StopEntryEdges rather than the core's bridge-relocation property.
"""
from __future__ import annotations

import dataclasses

from AsteriaVerify import Subjects
from AsteriaVerify.Elf import load

TERMINAL_STUBS = frozenset({'AsteriaIa64StopBreak', 'AsteriaIa64StopPageFault',
    'AsteriaIa64StopExternal', 'AsteriaIa64StopUserFault', 'AsteriaIa64StopException'})


def terminal(step) -> bool:
    target = step.operation.target
    return (step.mnemonic in ('br.many', 'br.sptk.many') and not step.predicate
            and target is not None and target.symbol in TERMINAL_STUBS and not target.offset)


def poll_ranges(path, code):
    elf = load(path)
    # These are local ELF marker symbols, retained in objects and images.
    starts = [symbol for symbol in elf.symbols if symbol.name.startswith('AsteriaIa64StopPollStart')
              or symbol.name.startswith('AsteriaIa64RuntimeRefillStopPollStart')]
    ranges = []
    for start in starts:
        if not code.start <= start.value < code.end:
            continue
        end_name = start.name.replace('PollStart', 'PollEnd')
        ends = [symbol for symbol in elf.symbols if symbol.name == end_name
                and symbol.section_index == start.section_index and symbol.value >= start.value]
        if ends:
            ranges.append((start.value, min(symbol.value for symbol in ends)))
    return tuple(ranges)


def core(path, code):
    ranges = poll_ranges(path, code)
    instructions = tuple(instruction for instruction in code.instructions
        if not any(start <= instruction.address < end for start, end in ranges))
    relocations = tuple(fact for fact in code.relocations if fact.target not in TERMINAL_STUBS
        and not any(start <= code.start + fact.offset < end for start, end in ranges))
    return dataclasses.replace(code, instructions=instructions, relocations=relocations)


class CoreCodeProperty(Subjects.CodeProperty):
    def extract(self, context, subject):
        return core(subject.path, super().extract(context, subject))
