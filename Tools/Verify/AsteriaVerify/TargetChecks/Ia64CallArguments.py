# SPDX-License-Identifier: Apache-2.0
"""IA-64 calls from hand-written assembly into C++.

Hand-written code passes arguments in the output registers of the frame its
governing `alloc` declares: with `alloc rX = ar.pfs, i, l, o, r`, out0 is
r(32 + i + l). The compiler checks this for C++ callers, but nothing checks it
for assembly, and a frame edit silently shifts every argument (one more local
turns out0 into a local and passes the next register instead). A golden pins
the code; this property states what must survive a golden update.
"""
from __future__ import annotations

import dataclasses
import functools
import pathlib
import re

from AsteriaVerify import Ia64, Subjects
from AsteriaVerify.Checks import ItemProperty, register

# Hand-written routines that call C++ directly, by the group suffix of the
# objects that define them ('' for kernel units, '-user' for the user image).
# The test firmware's user image calls the guest program's C++ entry, which
# Tests/Guest declares, from the same entry stub.
CALLERS = (
    ('AsteriaIa64SalBootRendezvousHandlerEntry', ''),
    ('AsteriaIa64SystemCallEntry', ''),
    ('AsteriaIa64UserFaultEntry', ''),
    ('AsteriaIa64UserInterruptEntry', ''),
    ('AsteriaIa64ApplicationProcessorTranslatedLanding', ''),
    ('AsteriaIa64InitialTranslatedExecution', ''),
    ('AsteriaIa64SwitchInitialExecutionContext', ''),
    ('AsteriaIa64MinimalUserImageEntry', '-user'),
)
_EXTERN_C = re.compile(r'extern\s+"C"')
_NAME = re.compile(r'\b([A-Za-z_]\w*)\s*\(')


@dataclasses.dataclass(frozen=True)
class CallSite:
    parameters: tuple[int, ...]         # the parameter count of every C++ declaration of the callee
    frame: tuple[int, int] | None       # (sof, sol) of the governing alloc; None when no alloc dominates the call
    written: frozenset[str]             # registers written on a path from that alloc (or the entry) to the call


DECLARING_DIRECTORIES = ('Source', 'Tests/Guest')


@functools.lru_cache(maxsize=None)
def declarations(root: str) -> dict[str, tuple[int, ...]]:
    """The parameter count of each `extern "C"` function declared or defined
    under Source/ or Tests/Guest/, once per declaration."""
    found: dict[str, list[int]] = {}
    paths = sorted(path for directory in DECLARING_DIRECTORIES for path in pathlib.Path(root, directory).rglob('*'))
    for path in paths:
        if path.suffix not in ('.hpp', '.cpp', '.h'):
            continue
        text = path.read_text()
        for marker in _EXTERN_C.finditer(text):
            head = text[marker.end():]
            end = min((index for index in (head.find(';'), head.find('{')) if index >= 0), default=-1)
            match = _NAME.search(head, 0, end if end >= 0 else len(head))
            if match is None or match.group(1) == '__attribute__':
                continue
            depth, index = 1, match.end()
            while depth and index < len(head):
                depth += {'(': 1, ')': -1}.get(head[index], 0)
                index += 1
            parameters = head[match.end():index - 1].strip()
            count = 0 if parameters in ('', 'void') else _top_level_commas(parameters) + 1
            found.setdefault(match.group(1), []).append(count)
    return {name: tuple(counts) for name, counts in found.items()}


def _top_level_commas(text: str) -> int:
    depth, commas = 0, 0
    for character in text:
        depth += {'(': 1, '<': 1, '[': 1, ')': -1, '>': -1, ']': -1}.get(character, 0)
        commas += character == ',' and depth == 0
    return commas


def call_sites(code, declared: dict[str, tuple[int, ...]]) -> dict[str, CallSite]:
    """Every direct call of `code` to a function declared `extern "C"`."""
    graph = code.graph
    allocs = [step for step in code.steps if step.operation.base == 'alloc']
    sites = {}
    for call in code.steps:
        target = call.operation.target
        if not call.operation.is_call or target is None or target.symbol not in declared:
            continue
        dominating = [alloc for alloc in allocs if graph.dominates(alloc.position, call.position)]
        governing = next((alloc for alloc in dominating
                          if all(graph.dominates(other.position, alloc.position) for other in dominating)), None)
        others = {alloc.position for alloc in allocs} - ({governing.position} if governing else set())
        sources = graph.successors[governing.position] if governing else (graph.entry,)
        after = graph.reachable(sources, avoiding=others | {call.position})
        before = graph.reaching((call.position,), avoiding=others)
        written = frozenset(register for step in code.steps if step.position in after & before
                            for register in step.operation.writes)
        frame = Ia64.alloc_frame(governing.operation)[:2] if governing else None
        sites['%s at +%#x' % (target.symbol, call.offset)] = CallSite(declared[target.symbol], frame, written)
    return sites


def parameter_registers(site: CallSite, parameters: int) -> list[str]:
    sol = site.frame[1] if site.frame else 0
    return ['r%d' % (32 + sol + index) for index in range(parameters)]


@register
class Ia64CallArguments(ItemProperty):
    """Every direct call from the hand-written routines of CALLERS to a
    function declared `extern "C"` under Source/ or Tests/Guest/ passes the
    callee's parameters where the IA-64 calling convention expects them, in
    both firmwares and the compile-check variants: all declarations of the
    callee agree on its parameter count (so the guest program's entry takes
    what the shared entry stub passes, like the production one); a callee with
    parameters is called under an `alloc` that dominates the call (the
    nearest one governs); that frame declares at least as many output
    registers as the callee has parameters; and each parameter register
    out0..outN-1, r(32 + sol + k), is written on some path from the governing
    alloc to the call."""
    isas = ('Ia64',)
    variants = ('firmware', 'firmware-test', 'O0', 'O2')

    def subjects(self, context):
        return [variant for variant in self.variants if context.has_variant(variant)]

    def extract(self, context, variant):
        declared = declarations(str(context.root))
        facts = {}
        for caller, suffix in CALLERS:
            path = Subjects.defining(context, variant + suffix, caller)
            if path is None:
                facts[caller] = None
                continue
            for label, site in call_sites(Subjects.code(context, path, caller), declared).items():
                facts['%s -> %s' % (caller, label)] = site
        return facts

    def item_problems(self, site):
        if site is None:
            return ['no unique object defines it']
        if len(set(site.parameters)) != 1:
            return ['its declarations disagree on the parameter count: %s' % ', '.join(map(str, site.parameters))]
        parameters = site.parameters[0]
        if parameters == 0:
            return []
        if site.frame is None:
            return ['the callee takes %d parameters but no alloc governs the call' % parameters]
        sof, sol = site.frame
        if sof - sol < parameters:
            return ['the frame declares %d output registers for %d parameters' % (sof - sol, parameters)]
        return ['parameter %d (%s) is not written before the call' % (index, register)
                for index, register in enumerate(parameter_registers(site, parameters))
                if register not in site.written]

    def item_mutants(self, site):
        parameters = site.parameters[0]
        yield 'the callee gains a parameter', dataclasses.replace(
            site, parameters=tuple(count + 1 for count in site.parameters))
        yield 'a declaration disagrees', dataclasses.replace(site, parameters=site.parameters + (parameters + 1,))
        if parameters == 0:
            return
        sof, sol = site.frame
        yield 'one more local shifts the outputs', dataclasses.replace(site, frame=(sof + 1, sol + 1))
        yield 'one output register too few', dataclasses.replace(site, frame=(sol + parameters - 1, sol))
        yield 'no governing alloc', dataclasses.replace(site, frame=None)
        last = parameter_registers(site, parameters)[-1]
        yield 'the last parameter is not written', dataclasses.replace(site, written=site.written - {last})
