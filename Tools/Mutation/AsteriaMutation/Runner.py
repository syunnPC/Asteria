# SPDX-License-Identifier: Apache-2.0
"""Mutant execution: compile one translation unit, relink the tests, run them.

Every mutant gets its own directory below the work directory holding the
mutated source copy, its object and the relinked test executables; the
directory is removed after the verdict unless artifacts are kept. Nothing is
written to Source/ or to the build tree.
"""
from __future__ import annotations

import concurrent.futures
import dataclasses
import os
import pathlib
import shutil
import signal
import subprocess
import time

from AsteriaMutation.Build import BuildTree, BuildTreeError, CompileEntry, LinkPlan, rewrite_compile, rewrite_link
from AsteriaMutation.Generator import Mutant, apply

KILLED = 'killed'
SURVIVED = 'survived'
TIMEOUT = 'timeout'
COMPILE_ERROR = 'compile-error'
STATUSES = (KILLED, SURVIVED, TIMEOUT, COMPILE_ERROR)


@dataclasses.dataclass(frozen=True)
class Outcome:
    mutant: Mutant
    status: str
    test: str | None
    detail: str
    seconds: float


@dataclasses.dataclass(frozen=True)
class ProcessResult:
    returncode: int | None
    output: str
    seconds: float

    @property
    def timed_out(self) -> bool:
        return self.returncode is None


def run_process(arguments: list[str], directory: pathlib.Path, timeout: float,
                environment: dict[str, str] | None = None) -> ProcessResult:
    """Runs a command in its own process group; kills the whole group on timeout."""
    started = time.monotonic()
    process = subprocess.Popen(arguments, cwd=directory, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout)
        returncode: int | None = process.returncode
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        output, _ = process.communicate()
        returncode = None
    text = output.decode(errors='replace')
    return ProcessResult(returncode, text, time.monotonic() - started)


def _first_line(text: str, needle: str = '') -> str:
    for line in text.splitlines():
        if needle in line and line.strip():
            return line.strip()[:200]
    return text.strip().splitlines()[-1][:200] if text.strip() else ''


def _failure_detail(result: ProcessResult) -> str:
    if result.returncode is not None and result.returncode < 0:
        prefix = 'signal %d' % -result.returncode
    else:
        prefix = 'exit %s' % result.returncode
    interesting = ''
    for needle in ('ERROR: AddressSanitizer', 'runtime error', 'FAIL', 'FUZZ PROPERTY', 'MISMATCH'):
        interesting = _first_line(result.output, needle) if needle in result.output else ''
        if interesting:
            break
    return prefix + (': ' + interesting if interesting else '')


class MutationRun:
    """Compiles and tests mutants of one source file against a set of tests."""

    def __init__(self, tree: BuildTree, source: pathlib.Path, test_names: list[str], work: pathlib.Path,
                 compile_timeout: float = 600.0, timeout_factor: float = 10.0, timeout_slack: float = 5.0,
                 keep_artifacts: bool = False, keep_werror: bool = False) -> None:
        self.tree = tree
        self.keep_werror = keep_werror
        self.source = source.resolve()
        self.work = work.resolve()
        self.compile_timeout = compile_timeout
        self.timeout_factor = timeout_factor
        self.timeout_slack = timeout_slack
        self.keep_artifacts = keep_artifacts
        entries = tree.compile_entries(self.source)
        if not entries:
            raise BuildTreeError('%s has no compile command in %s' % (source, tree.directory))
        self.plans: list[LinkPlan] = [tree.link_plan(tree.test(name), entries) for name in test_names]
        self.entries: list[CompileEntry] = []
        for plan in self.plans:
            if plan.entry not in self.entries:
                self.entries.append(plan.entry)
        self.test_timeouts: dict[str, float] = {}
        self.original_text = self.source.read_text()

    def _environment(self, plan: LinkPlan) -> dict[str, str]:
        environment = dict(os.environ)
        environment.update(dict(plan.test.environment))
        return environment

    def _build_and_test(self, directory: pathlib.Path, text: str,
                        timeouts: dict[str, float] | None) -> tuple[str, str | None, str, dict[str, float]]:
        """Returns (status, killing test, detail, per-test seconds) for one source text."""
        directory.mkdir(parents=True, exist_ok=True)
        copy = directory / self.source.name
        copy.write_text(text)
        objects: dict[CompileEntry, pathlib.Path] = {}
        for number, entry in enumerate(self.entries):
            obj = directory / ('%s.%d.o' % (self.source.stem, number))
            result = run_process(rewrite_compile(entry, copy, obj, self.keep_werror), entry.directory,
                                 self.compile_timeout)
            if result.timed_out or result.returncode != 0:
                message = _first_line(result.output, 'error') or 'compiler timeout'
                return COMPILE_ERROR, None, message.replace(str(directory) + '/', ''), {}
            objects[entry] = obj
        seconds: dict[str, float] = {}
        for number, plan in enumerate(self.plans):
            executable = directory / ('test%d' % number)
            link = run_process(rewrite_link(plan, objects[plan.entry], executable), self.tree.directory,
                               self.compile_timeout)
            if link.timed_out or link.returncode != 0:
                return COMPILE_ERROR, plan.test.name, 'link: ' + _first_line(link.output, 'error'), seconds
            command = [str(executable)] + list(plan.test.command[1:])
            limit = timeouts[plan.test.name] if timeouts else self.compile_timeout
            result = run_process(command, plan.test.working_directory, limit, self._environment(plan))
            seconds[plan.test.name] = result.seconds
            if result.timed_out:
                return TIMEOUT, plan.test.name, 'no result after %.1f s' % limit, seconds
            if result.returncode != 0:
                return KILLED, plan.test.name, _failure_detail(result), seconds
        return SURVIVED, None, '', seconds

    def baseline(self) -> dict[str, float]:
        """Builds and runs the unmutated source; every test must pass. Returns test timeouts."""
        status, test, detail, seconds = self._build_and_test(self.work / 'baseline', self.original_text, None)
        if status != SURVIVED:
            raise BuildTreeError('baseline %s (%s): %s' % (status, test or 'build', detail))
        self.test_timeouts = {name: value * self.timeout_factor + self.timeout_slack for name, value in seconds.items()}
        if not self.keep_artifacts:
            shutil.rmtree(self.work / 'baseline', ignore_errors=True)
        return seconds

    def run_mutant(self, mutant: Mutant) -> Outcome:
        started = time.monotonic()
        directory = self.work / 'mutants' / mutant.identifier
        try:
            status, test, detail, _ = self._build_and_test(directory, apply(self.original_text, mutant),
                                                           self.test_timeouts)
        finally:
            if not self.keep_artifacts:
                shutil.rmtree(directory, ignore_errors=True)
        return Outcome(mutant, status, test, detail, time.monotonic() - started)

    def run(self, mutants: list[Mutant], jobs: int, progress=None) -> list[Outcome]:
        """Runs every mutant with at most `jobs` concurrent mutants (one process each)."""
        outcomes: dict[str, Outcome] = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, jobs)) as pool:
            futures = {pool.submit(self.run_mutant, mutant): mutant for mutant in mutants}
            for future in concurrent.futures.as_completed(futures):
                outcome = future.result()
                outcomes[outcome.mutant.identifier] = outcome
                if progress is not None:
                    progress(outcome, len(outcomes), len(mutants))
        return [outcomes[mutant.identifier] for mutant in mutants]
