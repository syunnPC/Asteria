# SPDX-License-Identifier: Apache-2.0
"""Unit tests of build-command rewriting and report summaries (no build tree needed).

Run with: python3 -m unittest discover -s Tests -t. (from Tools/Mutation)
"""
from __future__ import annotations

import pathlib
import unittest

from AsteriaMutation import Build, Report
from AsteriaMutation.Generator import Mutant
from AsteriaMutation.Runner import Outcome

ENTRY = Build.CompileEntry(
    directory=pathlib.Path('/build'),
    arguments=('/usr/bin/c++', '-I/src/Source', '-std=c++23', '-Werror', '-MD', '-MT', 'obj.o', '-MF', 'obj.o.d',
               '-o', 'Source/CMakeFiles/Lib.dir/Kernel/X.cpp.o', '-c', '/src/Source/Kernel/X.cpp'),
    source=pathlib.Path('/src/Source/Kernel/X.cpp'),
    output=pathlib.Path('/build/Source/CMakeFiles/Lib.dir/Kernel/X.cpp.o'))
TEST = Build.TestCase(name='Kernel/XTests', command=('/build/Tests/XTests', '--flag'),
                      working_directory=pathlib.Path('/build/Tests'), environment=())


class CommandTests(unittest.TestCase):
    def test_shell_wrapper_is_removed(self) -> None:
        self.assertEqual(Build.strip_shell_wrapper(': && /usr/bin/c++ a.o -o "out file" lib.a && :'),
                         ['/usr/bin/c++', 'a.o', '-o', 'out file', 'lib.a'])
        self.assertEqual(Build.strip_shell_wrapper('cd /build && /usr/bin/ar qc x.a y.o'),
                         ['/usr/bin/ar', 'qc', 'x.a', 'y.o'])
        with self.assertRaises(Build.BuildTreeError):
            Build.strip_shell_wrapper('/usr/bin/c++ a.o && /usr/bin/strip a.out')

    def test_compile_command_is_redirected(self) -> None:
        arguments = Build.rewrite_compile(ENTRY, pathlib.Path('/work/M1/X.cpp'), pathlib.Path('/work/M1/X.o'))
        self.assertEqual(arguments, ['/usr/bin/c++', '-iquote', '/src/Source/Kernel', '-I/src/Source', '-std=c++23',
                                     '-Werror', '-o', '/work/M1/X.o', '-c', '/work/M1/X.cpp', '-Wno-error'])
        strict = Build.rewrite_compile(ENTRY, pathlib.Path('/w/X.cpp'), pathlib.Path('/w/X.o'), keep_werror=True)
        self.assertNotIn('-Wno-error', strict)

    def test_query_inputs_exclude_outputs_and_order_only_inputs(self) -> None:
        query = ('Source/libLib.a:\n  input: CXX_STATIC_LIBRARY_LINKER__Lib_\n    Source/A.cpp.o\n'
                 '    Source/B.cpp.o\n    || cmake_object_order_depends_target_Lib\n'
                 '  outputs:\n    Tests/XTests\n')
        self.assertEqual(Build.parse_query_inputs(query), frozenset({'Source/A.cpp.o', 'Source/B.cpp.o'}))

    def test_link_inserts_the_object_before_its_archive(self) -> None:
        plan = Build.LinkPlan(test=TEST, entry=ENTRY,
                              arguments=('/usr/bin/c++', 'Tests/X.o', '-o', 'Tests/XTests', 'Source/libLib.a'),
                              output_index=3, insert_index=4, replaces_object=False)
        self.assertEqual(Build.rewrite_link(plan, pathlib.Path('/work/X.o'), pathlib.Path('/work/test0')),
                         ['/usr/bin/c++', 'Tests/X.o', '-o', '/work/test0', '/work/X.o', 'Source/libLib.a'])

    def test_link_replaces_a_directly_linked_object(self) -> None:
        plan = Build.LinkPlan(test=TEST, entry=ENTRY,
                              arguments=('/usr/bin/c++', 'Source/CMakeFiles/Lib.dir/Kernel/X.cpp.o', '-o', 'T'),
                              output_index=3, insert_index=1, replaces_object=True)
        self.assertEqual(Build.rewrite_link(plan, pathlib.Path('/work/X.o'), pathlib.Path('/work/T')),
                         ['/usr/bin/c++', '/work/X.o', '-o', '/work/T'])


def outcome(status: str, operator: str = 'equality') -> Outcome:
    mutant = Mutant('M0001', operator, operator, 0, 2, '==', '!=', 1, 1)
    return Outcome(mutant, status, None, '', 0.1)


class ReportTests(unittest.TestCase):
    def test_kill_rate_excludes_compile_errors(self) -> None:
        summary = Report.summarize([outcome('killed'), outcome('killed'), outcome('survived'), outcome('timeout'),
                                    outcome('compile-error')])
        self.assertEqual((summary['killed'], summary['survived'], summary['timeout'], summary['compile-error']),
                         (2, 1, 1, 1))
        self.assertAlmostEqual(summary['kill_rate'], 0.5)

    def test_empty_run_has_no_kill_rate(self) -> None:
        self.assertIsNone(Report.summarize([outcome('compile-error')])['kill_rate'])


if __name__ == '__main__':
    unittest.main()
