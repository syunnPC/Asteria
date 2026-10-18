# SPDX-License-Identifier: Apache-2.0
"""Result reporting: console summary, JUnit XML and JSON."""
from __future__ import annotations

import json
import pathlib
import xml.etree.ElementTree as ElementTree

from AsteriaVm.Executor import CaseResult


def console_line(result: CaseResult) -> str:
    status = 'PASS' if result.passed else 'FAIL'
    failed = [check for check in result.checks if not check.passed]
    skipped = sum(1 for check in result.checks if check.skipped)
    detail = ''
    if result.error:
        detail = ': ' + result.error.strip().splitlines()[-1]
    elif failed:
        detail = ': ' + ', '.join(check.name for check in failed)
    counts = '%d checks' % len(result.checks) + (', %d skipped' % skipped if skipped else '')
    return '%s %s (%s, %.0fs)%s' % (status, result.case.name, counts, result.seconds, detail)


def failure_details(result: CaseResult) -> str:
    lines = ['--- %s (%s)' % (result.case.name, result.run_directory)]
    if result.error:
        lines.append('error: %s' % result.error.rstrip())
    for check in result.checks:
        if not check.passed:
            lines.append('failed %s' % check.name)
            lines.extend('    ' + line for line in check.detail.splitlines())
    return '\n'.join(lines)


def write_junit(results: list[CaseResult], path: pathlib.Path) -> None:
    suites = ElementTree.Element('testsuites')
    for result in results:
        suite = ElementTree.SubElement(suites, 'testsuite', name=result.case.name,
                                       tests=str(len(result.checks) + (1 if result.error else 0)),
                                       failures=str(sum(1 for check in result.checks if not check.passed)),
                                       errors='1' if result.error else '0',
                                       skipped=str(sum(1 for check in result.checks if check.skipped)),
                                       time='%.3f' % result.seconds)
        for check in result.checks:
            testcase = ElementTree.SubElement(suite, 'testcase', classname=result.case.name, name=check.name)
            if not check.passed:
                ElementTree.SubElement(testcase, 'failure', message=check.name).text = check.detail
            elif check.skipped:
                ElementTree.SubElement(testcase, 'skipped', message=check.skipped)
        if result.error:
            testcase = ElementTree.SubElement(suite, 'testcase', classname=result.case.name, name='lifecycle')
            ElementTree.SubElement(testcase, 'error', message='case error').text = result.error
        ElementTree.SubElement(suite, 'system-out').text = str(result.run_directory)
    ElementTree.ElementTree(suites).write(path, encoding='utf-8', xml_declaration=True)


def write_json(results: list[CaseResult], path: pathlib.Path) -> None:
    path.write_text(json.dumps([{
        'case': result.case.name, 'preset': result.case.preset, 'firmware': result.case.firmware,
        'cpus': result.case.cpus, 'disk': result.case.disk,
        'passed': result.passed, 'error': result.error, 'seconds': round(result.seconds, 3),
        'run_directory': str(result.run_directory),
        'checks': [{'name': check.name, 'passed': check.passed, 'detail': check.detail,
                    **({'skipped': check.skipped} if check.skipped else {})} for check in result.checks],
    } for result in results], indent=2) + '\n')
