# SPDX-License-Identifier: Apache-2.0
"""Decoded IA-64 code as facts: navigation, editing and control flow.

`Code` is the fact that instruction properties extract: every instruction of
one extent in address order (nop slots included, so bundles, slots and stops
stay visible), its start and end addresses, and the relocations inside it.
It is immutable; mutants are built with its editing methods, which return
new Code. The derived views (parsed operations, instruction groups, the
meaningful steps and the control-flow graph) are computed once per Code.

A branch target names a bundle; it resolves to the first meaningful
(non-nop) instruction at or after that address, which executes first there.

`Graph` is the control-flow graph over the positions of meaningful
instructions. A non-branch continues with the next meaningful instruction; a
call resumes at the bundle after its own (its return address); a
conditional branch (predicated, or a loop branch) has its target and the
next instruction as successors, an unconditional one only its target.
Returns and rfi lead to RETURN; branches that leave the extent, and indirect
branches (also recorded in `indirect`), to EXIT; execution past the last
instruction, or a target beyond it, to END.
"""
from __future__ import annotations

import bisect
import dataclasses
import functools
import re
from typing import Callable, Iterable

from AsteriaVerify import Ia64
from AsteriaVerify.Disassembly import Instruction, group_indices

RETURN, EXIT, END = 'return', 'exit', 'end'
MARKERS = frozenset({RETURN, EXIT, END})
_PREDICATED = re.compile(r'^\((?P<predicate>p\d+)\)\s*(?P<text>.*)$')
_TARGET = re.compile(r'<[^>]*>$')


class SiteError(LookupError):
    """A mutant's anchor is missing from the facts: the code changed, so the
    property needs review."""


@dataclasses.dataclass(frozen=True)
class Step:
    """A meaningful instruction and its position in Code.instructions."""
    position: int
    instruction: Instruction
    operation: Ia64.Operation
    group: int
    offset: int                 # address - Code.start

    @property
    def text(self) -> str:
        return self.instruction.text

    @property
    def rendered(self) -> str:
        return self.instruction.rendered()

    @property
    def mnemonic(self) -> str:
        return self.instruction.mnemonic

    @property
    def predicate(self) -> str | None:
        return self.operation.predicate

    @property
    def target(self) -> int | None:
        """The offset of an internal branch target."""
        target = self.operation.target
        return target.offset if target is not None and target.kind == 'internal' else None

    @property
    def bundle(self) -> int:
        """The offset of the bundle holding the instruction."""
        return self.offset & ~0xF

    def writes(self, register: str) -> bool:
        return register in self.operation.writes

    def reads(self, register: str) -> bool:
        return register in self.operation.reads

    def uses(self, register: str) -> bool:
        return register in self.operation.reads or register in self.operation.writes

    def describe(self) -> str:
        return '%s (+%#x)' % (self.rendered, self.offset)


Matcher = str | Callable[[Step], bool]
At = Step | int


def written(instruction: Instruction, rendered: str) -> Instruction:
    """`instruction` with the text of `rendered`; a `(pN) ` prefix sets the
    predicate, and a rewritten instruction carries no relocation."""
    match = _PREDICATED.match(rendered)
    predicate, text = (match.group('predicate'), match.group('text')) if match else (None, rendered)
    return dataclasses.replace(instruction, text=text, predicate=predicate, relocation=None)


@dataclasses.dataclass(frozen=True)
class Code:
    start: int
    instructions: tuple[Instruction, ...]
    relocations: tuple = ()
    end: int = 0
    label: str = ''

    # Views -----------------------------------------------------------------

    @functools.cached_property
    def operations(self) -> tuple[Ia64.Operation, ...]:
        return tuple(Ia64.parse(instruction) for instruction in self.instructions)

    @functools.cached_property
    def groups(self) -> tuple[int, ...]:
        return tuple(group_indices(list(self.instructions)))

    @functools.cached_property
    def steps(self) -> tuple[Step, ...]:
        return tuple(Step(position, instruction, self.operations[position], self.groups[position],
                          instruction.address - self.start)
                     for position, instruction in enumerate(self.instructions) if not instruction.is_nop)

    @functools.cached_property
    def _positions(self) -> list[int]:
        return [step.position for step in self.steps]

    @functools.cached_property
    def _offsets(self) -> list[int]:
        return [step.offset for step in self.steps]

    @functools.cached_property
    def _texts(self) -> dict[str, list[int]]:
        texts: dict[str, list[int]] = {}
        for index, step in enumerate(self.steps):
            texts.setdefault(step.rendered, []).append(index)
        return texts

    @functools.cached_property
    def graph(self) -> 'Graph':
        return Graph(self)

    # Navigation --------------------------------------------------------------

    def step(self, at: At) -> Step | None:
        """The step at a position (None for a nop)."""
        position = _position(at)
        index = bisect.bisect_left(self._positions, position)
        return self.steps[index] if index < len(self.steps) and self._positions[index] == position else None

    def next(self, at: At) -> Step | None:
        """The first step after `at`."""
        index = bisect.bisect_right(self._positions, _position(at))
        return self.steps[index] if index < len(self.steps) else None

    def previous(self, at: At) -> Step | None:
        """The last step before `at`."""
        index = bisect.bisect_left(self._positions, _position(at)) - 1
        return self.steps[index] if index >= 0 else None

    def at_offset(self, offset: int) -> Step | None:
        """The first step at or after start + offset."""
        index = bisect.bisect_left(self._offsets, offset)
        return self.steps[index] if index < len(self.steps) else None

    def target(self, at: At) -> Step | None:
        """The step an internal branch reaches."""
        step = at if isinstance(at, Step) else self.step(at)
        return None if step is None or step.target is None else self.at_offset(step.target)

    def _span(self, after: At | None, before: At | None) -> tuple[int, int]:
        return (0 if after is None else bisect.bisect_right(self._positions, _position(after)),
                len(self.steps) if before is None else bisect.bisect_left(self._positions, _position(before)))

    def _matches(self, matcher: Matcher, after: At | None, before: At | None) -> Iterable[Step]:
        low, high = self._span(after, before)
        if isinstance(matcher, str):
            indices = self._texts.get(matcher, [])
            return (self.steps[index] for index in indices[bisect.bisect_left(indices, low):
                                                           bisect.bisect_left(indices, high)])
        return (self.steps[index] for index in range(low, high) if matcher(self.steps[index]))

    def window(self, after: At | None = None, before: At | None = None) -> list[Step]:
        """Steps strictly between `after` and `before` (either may be None)."""
        low, high = self._span(after, before)
        return list(self.steps[low:high])

    def all(self, matcher: Matcher, after: At | None = None, before: At | None = None) -> list[Step]:
        """Steps in the window that match: an exact rendered text or a
        predicate over steps."""
        return list(self._matches(matcher, after, before))

    def first(self, *path: Matcher) -> Step | None:
        """Walks the matchers in turn, each after the previous match."""
        found = None
        for matcher in path:
            found = next(iter(self._matches(matcher, found, None)), None)
            if found is None:
                return None
        return found

    def site(self, *path: Matcher) -> Step:
        """`first`, for mutation sites, which must exist."""
        found = self.first(*path)
        if found is None:
            raise SiteError('mutant site %s not found' % ' -> '.join(
                matcher if isinstance(matcher, str) else getattr(matcher, '__name__', 'predicate')
                for matcher in path))
        return found

    def unique(self, matcher: Matcher, what: str, problems: list[str], **window) -> Step | None:
        """The single match, or None after recording a problem."""
        found = self.all(matcher, **window)
        if len(found) != 1:
            problems.append('%d %s, expected exactly one' % (len(found), what))
            return None
        return found[0]

    def free_slot(self, after: At | None = None, kind: str = 'nop') -> int:
        """The position of the first nop slot of `kind` (nop, nop.m, nop.i,
        ...) after `after`, where a mutant can place an instruction."""
        low = -1 if after is None else _position(after)
        for position in range(low + 1, len(self.instructions)):
            if self.instructions[position].text.startswith(kind):
                return position
        raise SiteError('no %s slot after position %d' % (kind, low))

    def starts_group(self, at: At) -> bool:
        """No instruction, nop or not, precedes `at` in its group."""
        position = _position(at)
        return position == 0 or self.instructions[position - 1].stop_after

    def ends_group(self, at: At) -> bool:
        """No meaningful instruction follows `at` in its group."""
        following = self.next(at)
        return following is None or following.group != self.groups[_position(at)]

    # Editing (mutants) -------------------------------------------------------

    def with_instructions(self, instructions: Iterable[Instruction]) -> 'Code':
        return dataclasses.replace(self, instructions=tuple(instructions))

    def replace(self, at: At, **changes) -> 'Code':
        position = _position(at)
        return self.with_instructions(self.instructions[:position]
                                      + (dataclasses.replace(self.instructions[position], **changes),)
                                      + self.instructions[position + 1:])

    def rewrite(self, at: At, rendered: str) -> 'Code':
        """Replaces an instruction (or nop slot) by `rendered`, `(pN) text`
        or `text`, keeping its slot and stop."""
        position = _position(at)
        return self.with_instructions(self.instructions[:position] + (written(self.instructions[position], rendered),)
                                      + self.instructions[position + 1:])

    def insert(self, at: At, *rendered: str, after: bool = False, stop: bool = False) -> 'Code':
        """Inserts instructions before (or after) `at`, at its address, template
        and slot, so branch targets keep resolving to what they named."""
        position = _position(at)
        anchor = self.instructions[min(position, len(self.instructions) - 1)]
        new = tuple(written(Instruction(anchor.address, '', anchor.template, anchor.slot, stop), text)
                    for text in rendered)
        position += int(after)
        return self.with_instructions(self.instructions[:position] + new + self.instructions[position:])

    def swap(self, first: At, second: At) -> 'Code':
        """Exchanges two instructions' rendered texts, keeping slots and stops."""
        a, b = _position(first), _position(second)
        return self.rewrite(a, self.instructions[b].rendered()).rewrite(b, self.instructions[a].rendered())

    def delete(self, at: At) -> 'Code':
        position = _position(at)
        return self.with_instructions(self.instructions[:position] + self.instructions[position + 1:])

    def duplicate(self, at: At) -> 'Code':
        position = _position(at)
        return self.with_instructions(self.instructions[:position + 1] + self.instructions[position:])

    def nop(self, at: At) -> 'Code':
        """Replaces an instruction by the nop of its unit, keeping its stop."""
        position = _position(at)
        instruction = self.instructions[position]
        unit = 'b' if Ia64.parse(instruction).is_branch else 'm' if instruction.slot == 0 else 'i'
        return self.rewrite(position, 'nop.%s 0x0' % unit)

    def without_stops(self, first: At, last: At) -> 'Code':
        """Clears the stops of positions [first, last): merges their groups."""
        low, high = _position(first), _position(last)
        return self.with_instructions(dataclasses.replace(instruction, stop_after=False) if low <= index < high
                                      else instruction for index, instruction in enumerate(self.instructions))

    def without_boundary(self, at: At) -> 'Code':
        """Merges the group of `at` with the next meaningful instruction's."""
        following = self.next(at)
        return self.without_stops(at, following.position if following is not None else len(self.instructions))

    def retarget(self, at: At, target: int | str) -> 'Code':
        """Points a direct branch at an offset of the extent or a symbol."""
        position = _position(at)
        token = '<+%#x>' % target if isinstance(target, int) else '<%s>' % target
        return self.replace(position, text=_TARGET.sub(token, self.instructions[position].text), relocation=None)


def _position(at: At) -> int:
    return at.position if isinstance(at, Step) else at


def writes_between(steps, register: str, first: int, last: int) -> list[Step]:
    """The steps strictly between step indices `first` and `last` that write `register`."""
    return [step for step in steps[first + 1:last] if register in step.operation.writes]


class Graph:
    """The control-flow graph of a Code (see the module docstring)."""

    def __init__(self, code: Code):
        self.code = code
        self.successors: dict[int, tuple] = {}
        self.indirect: list[int] = []
        positions = [step.position for step in code.steps]
        addresses = [step.instruction.address for step in code.steps]

        def at(address: int):
            index = bisect.bisect_left(addresses, address)
            return positions[index] if index < len(positions) else END

        for index, step in enumerate(code.steps):
            operation = step.operation
            fall = positions[index + 1] if index + 1 < len(positions) else END
            if operation.base == 'rfi' or operation.is_return:
                taken = RETURN
            elif not operation.is_branch:
                self.successors[step.position] = (fall,)
                continue
            elif operation.is_call:
                self.successors[step.position] = (at((step.instruction.address & ~0xF) + 16),)
                continue
            elif step.target is not None:
                taken = at(code.start + step.target)
            else:
                taken = EXIT
                if operation.target is None:
                    self.indirect.append(step.position)
            self.successors[step.position] = (taken, fall) if operation.is_conditional_branch else (taken,)
        self.entry = positions[0] if positions else None

    def reachable(self, sources: Iterable, *, avoiding: Iterable = (), removed: Iterable = ()) -> set:
        """Nodes (and markers) reachable from `sources` without entering the
        nodes in `avoiding` or taking the (from, to) edges in `removed`."""
        avoiding, removed = set(avoiding), set(removed)
        seen: set = set()
        pending = [source for source in sources if source not in avoiding]
        while pending:
            node = pending.pop()
            if node in seen:
                continue
            seen.add(node)
            pending.extend(successor for successor in self.successors.get(node, ())
                           if successor not in avoiding and (node, successor) not in removed)
        return seen

    def reaching(self, targets: Iterable, *, avoiding: Iterable = ()) -> set:
        """Nodes from which a node of `targets` is reachable (the targets
        included) without entering the nodes in `avoiding`."""
        avoiding = set(avoiding)
        seen: set = set()
        pending = [target for target in targets if target not in avoiding]
        while pending:
            node = pending.pop()
            if node in seen:
                continue
            seen.add(node)
            pending.extend(predecessor for predecessor in self._predecessors.get(node, ())
                           if predecessor not in avoiding)
        return seen

    def leaving(self) -> set[int]:
        """Nodes that may leave the extent: returns, exits and the ends."""
        return {node for node, successors in self.successors.items() if MARKERS & set(successors)}

    @functools.cached_property
    def _predecessors(self) -> dict[int, list[int]]:
        predecessors: dict[int, list[int]] = {node: [] for node in self.successors}
        for node, successors in self.successors.items():
            for successor in successors:
                if successor not in MARKERS:
                    predecessors[successor].append(node)
        return predecessors

    @functools.cached_property
    def dominators(self) -> dict[int, int]:
        """The dominators of each node reachable from the entry, as a bit set
        over positions."""
        if self.entry is None:
            return {}
        order, seen = [], {self.entry}
        stack = [(self.entry, iter(self.successors[self.entry]))]
        while stack:
            node, successors = stack[-1]
            successor = next(successors, None)
            if successor is None:
                order.append(node)
                stack.pop()
            elif successor not in MARKERS and successor not in seen:
                seen.add(successor)
                stack.append((successor, iter(self.successors[successor])))
        order.reverse()
        full = (1 << (max(seen) + 1)) - 1
        dominators = {node: full for node in order}
        dominators[self.entry] = 1 << self.entry
        changed = True
        while changed:
            changed = False
            for node in order[1:]:
                value = full
                for predecessor in self._predecessors[node]:
                    if predecessor in seen:
                        value &= dominators[predecessor]
                value |= 1 << node
                if value != dominators[node]:
                    dominators[node], changed = value, True
        return dominators

    def dominates(self, first: int, second: int) -> bool:
        return bool(self.dominators.get(second, 0) >> first & 1)

    @functools.cached_property
    def loops(self) -> dict[int, frozenset[int]]:
        """Natural loops: header -> body (the union over its back edges)."""
        loops: dict[int, set[int]] = {}
        for node, successors in self.successors.items():
            for header in successors:
                if header not in MARKERS and node in self.dominators and self.dominates(header, node):
                    body, stack = loops.setdefault(header, {header}), [node]
                    while stack:
                        member = stack.pop()
                        if member not in body:
                            body.add(member)
                            stack.extend(self._predecessors[member])
        return {header: frozenset(body) for header, body in loops.items()}

    def exits(self, body: Iterable[int]) -> list[tuple[int, int]]:
        """The edges from `body` to instructions outside it."""
        body = set(body)
        return [(node, successor) for node in sorted(body) for successor in self.successors[node]
                if successor not in MARKERS and successor not in body]
