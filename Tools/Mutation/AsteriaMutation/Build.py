# SPDX-License-Identifier: Apache-2.0
"""Queries and command rewriting for a configured CMake/Ninja host tree.

The mutation runner never builds inside the tree. It reads the exact compile
command of one translation unit from compile_commands.json (or `ninja -t
compdb`), the CTest command of each test from `ctest --show-only=json-v1`,
and each test's link command from `ninja -t commands`, then rewrites those
commands so that every output lands in the mutation work directory.
"""
from __future__ import annotations

import dataclasses
import json
import os
import pathlib
import shlex
import subprocess


class BuildTreeError(RuntimeError):
    """The build tree cannot provide what a mutation run needs."""


@dataclasses.dataclass(frozen=True)
class CompileEntry:
    directory: pathlib.Path
    arguments: tuple[str,...]
    source: pathlib.Path
    output: pathlib.Path


@dataclasses.dataclass(frozen=True)
class TestCase:
    name: str
    command: tuple[str,...]
    working_directory: pathlib.Path
    environment: tuple[tuple[str, str],...]


@dataclasses.dataclass(frozen=True)
class LinkPlan:
    """How to relink one test executable around a replacement object."""
    test: TestCase
    entry: CompileEntry
    arguments: tuple[str,...]
    output_index: int
    insert_index: int
    replaces_object: bool


def _run(arguments: list[str], directory: pathlib.Path) -> str:
    result = subprocess.run(arguments, cwd=directory, capture_output=True, text=True)
    if result.returncode != 0:
        raise BuildTreeError('%s failed: %s' % (' '.join(arguments), result.stderr.strip()[-500:]))
    return result.stdout


def strip_shell_wrapper(command: str) -> list[str]:
    """Returns the arguments of the single tool invocation in a Ninja command.

    CMake wraps link commands as `: && <tool> <arguments> &&:`; the wrapper
    and any leading `cd <dir> &&` are removed. A command that chains several
    real tools is rejected.
    """
    words = shlex.split(command)
    segments: list[list[str]] = [[]]
    for word in words:
        if word == '&&':
            segments.append([])
        else:
            segments[-1].append(word)
    tools = [segment for segment in segments if segment and segment != [':'] and segment[0] != 'cd']
    if len(tools) != 1:
        raise BuildTreeError('cannot isolate one tool invocation in: %s' % command)
    return tools[0]


def parse_query_inputs(query: str) -> frozenset[str]:
    """The explicit inputs listed by `ninja -t query <target>` (not its outputs or order-only inputs)."""
    members = set()
    in_inputs = False
    for line in query.splitlines():
        if line.startswith('  ') and not line.startswith('    '):
            in_inputs = line.strip().startswith('input:')
        elif in_inputs and line.startswith('    ') and not line.strip().startswith('|'):
            members.add(line.strip())
    return frozenset(members)


def rewrite_compile(entry: CompileEntry, source: pathlib.Path, output: pathlib.Path,
                    keep_werror: bool = False) -> list[str]:
    """The entry's compile arguments with a new input, output and include path.

    The original directory of the source is added as a quote-include path
    because the mutated copy lives elsewhere; dependency-file options are
    dropped so nothing is written into the build tree. Unless `keep_werror`,
    `-Wno-error` is appended: a mutant that merely provokes a warning (for
    example -Wparentheses after `||` -> `&&`) still changes behavior and must
    be tested, not discarded as a compile error.
    """
    arguments = list(entry.arguments)
    rewritten: list[str] = []
    index = 0
    replaced_source = replaced_output = False
    while index < len(arguments):
        argument = arguments[index]
        if argument in ('-MF', '-MT', '-MQ'):
            index += 2
            continue
        if argument in ('-MD', '-MMD'):
            index += 1
            continue
        if argument == '-o' and index + 1 < len(arguments):
            rewritten.extend(['-o', str(output)])
            replaced_output = True
            index += 2
            continue
        if argument == '-c' and index + 1 < len(arguments):
            rewritten.extend(['-c', str(source)])
            replaced_source = True
            index += 2
            continue
        rewritten.append(argument)
        index += 1
    if not (replaced_source and replaced_output):
        raise BuildTreeError('compile command of %s has no -c/-o pair' % entry.source)
    rewritten[1:1] = ['-iquote', str(entry.source.parent)]
    if not keep_werror:
        rewritten.append('-Wno-error')
    return rewritten


def rewrite_link(plan: LinkPlan, replacement: pathlib.Path, output: pathlib.Path) -> list[str]:
    """The test's link arguments linking `replacement` and writing `output`."""
    arguments = list(plan.arguments)
    arguments[plan.output_index] = str(output)
    if plan.replaces_object:
        arguments[plan.insert_index] = str(replacement)
    else:
        arguments.insert(plan.insert_index, str(replacement))
    return arguments


class BuildTree:
    def __init__(self, directory: pathlib.Path) -> None:
        self.directory = directory.resolve()
        if not (self.directory / 'build.ninja').is_file():
            raise BuildTreeError('%s is not a configured Ninja build tree' % self.directory)
        self._entries: list[CompileEntry] | None = None
        self._tests: dict[str, TestCase] | None = None
        self._archives: dict[str, frozenset[str]] = {}

    def compile_entries(self, source: pathlib.Path) -> list[CompileEntry]:
        """Every compile command of `source` (a file can be built into several libraries)."""
        if self._entries is None:
            database = self.directory / 'compile_commands.json'
            if database.is_file():
                records = json.loads(database.read_text())
            else:
                records = json.loads(_run(['ninja', '-C', str(self.directory), '-t', 'compdb'], self.directory))
            entries = []
            for record in records:
                directory = pathlib.Path(record['directory'])
                arguments = record.get('arguments') or shlex.split(record['command'])
                output = record.get('output')
                if output is None:
                    output = arguments[arguments.index('-o') + 1]
                entries.append(CompileEntry(
                    directory=directory,
                    arguments=tuple(arguments),
                    source=(directory / record['file']).resolve(),
                    output=(directory / output).resolve()))
            self._entries = entries
        wanted = source.resolve()
        return [entry for entry in self._entries if entry.source == wanted]

    def test(self, name: str) -> TestCase:
        if self._tests is None:
            listing = json.loads(_run(['ctest', '--show-only=json-v1', '--test-dir', str(self.directory)],
                                      self.directory))
            tests = {}
            for record in listing['tests']:
                properties = {item['name']: item['value'] for item in record.get('properties', [])}
                environment = []
                for assignment in properties.get('ENVIRONMENT', []):
                    key, _, value = assignment.partition('=')
                    environment.append((key, value))
                tests[record['name']] = TestCase(
                    name=record['name'],
                    command=tuple(record.get('command', ())),
                    working_directory=pathlib.Path(properties.get('WORKING_DIRECTORY', str(self.directory))),
                    environment=tuple(environment))
            self._tests = tests
        if name not in self._tests:
            raise BuildTreeError('no CTest test named %s in %s' % (name, self.directory))
        test = self._tests[name]
        if not test.command:
            raise BuildTreeError('test %s has no command; build its executable first' % name)
        return test

    def archive_members(self, archive: str) -> frozenset[str]:
        """Build-directory-relative object paths that `archive` is made from."""
        if archive not in self._archives:
            query = _run(['ninja', '-C', str(self.directory), '-t', 'query', archive], self.directory)
            self._archives[archive] = parse_query_inputs(query)
        return self._archives[archive]

    def link_plan(self, test: TestCase, entries: list[CompileEntry]) -> LinkPlan:
        """Finds the link command of the test and where its copy of the source enters it."""
        executable = pathlib.Path(test.command[0]).resolve()
        try:
            target = executable.relative_to(self.directory).as_posix()
        except ValueError:
            raise BuildTreeError('%s is not built in %s' % (executable, self.directory)) from None
        commands = _run(['ninja', '-C', str(self.directory), '-t', 'commands', target], self.directory)
        lines = [line for line in commands.splitlines() if line.strip()]
        if not lines:
            raise BuildTreeError('ninja knows no command for %s' % target)
        arguments = strip_shell_wrapper(lines[-1])
        arguments = [argument for argument in arguments if not argument.startswith('-Wl,--dependency-file=')]
        if '-o' not in arguments:
            raise BuildTreeError('link command of %s has no -o' % target)
        output_index = arguments.index('-o') + 1
        for entry in entries:
            relative = os.path.relpath(entry.output, self.directory)
            for index, argument in enumerate(arguments):
                if argument == relative or pathlib.Path(self.directory, argument).resolve() == entry.output:
                    return LinkPlan(test, entry, tuple(arguments), output_index, index, True)
            for index, argument in enumerate(arguments):
                if argument.endswith('.a') and relative in self.archive_members(argument):
                    return LinkPlan(test, entry, tuple(arguments), output_index, index, False)
        raise BuildTreeError('test %s does not link any object built from %s'
                             % (test.name, entries[0].source if entries else 'the source'))
