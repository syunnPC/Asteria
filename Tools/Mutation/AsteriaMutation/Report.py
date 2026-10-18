# SPDX-License-Identifier: Apache-2.0
"""Text and JSON reports of a mutation run."""
from __future__ import annotations

import collections
import json

from AsteriaMutation.Generator import Mutant
from AsteriaMutation.Runner import COMPILE_ERROR, KILLED, STATUSES, SURVIVED, TIMEOUT, Outcome


def summarize(outcomes: list[Outcome]) -> dict[str, object]:
    """Counts per status and the kill rate killed / (killed + survived + timeout)."""
    counts = collections.Counter(outcome.status for outcome in outcomes)
    detectable = counts[KILLED] + counts[SURVIVED] + counts[TIMEOUT]
    return {
        'mutants': len(outcomes),
        **{status: counts[status] for status in STATUSES},
        'kill_rate': (counts[KILLED] / detectable) if detectable else None,
        'by_operator': {
            operator: dict(collections.Counter(o.status for o in outcomes if o.mutant.operator == operator))
            for operator in sorted({o.mutant.operator for o in outcomes})
        },
    }


def _source_line(lines: list[str], mutant: Mutant) -> str:
    return lines[mutant.line - 1].strip() if 0 < mutant.line <= len(lines) else ''


def format_listing(mutants: list[Mutant], text: str) -> str:
    lines = text.split('\n')
    rows = ['%-6s %-9s %-20s %-30s %s' % ('ID', 'Line:Col', 'Operator', 'Mutation', 'Source line')]
    for mutant in mutants:
        rows.append('%-6s %-9s %-20s %-30s %s' % (
            mutant.identifier, '%d:%d' % (mutant.line, mutant.column), mutant.operator,
            mutant.description, _source_line(lines, mutant)[:100]))
    return '\n'.join(rows)


def format_text(source: str, tests: list[str], outcomes: list[Outcome], text: str, seconds: float) -> str:
    lines = text.split('\n')
    summary = summarize(outcomes)
    rate = summary['kill_rate']
    report = [
        'Mutation report for %s' % source,
        'Tests: %s' % ', '.join(tests),
        '%d mutants: %d killed, %d survived, %d timeout, %d compile-error; kill rate %s; %.0f s' % (
            summary['mutants'], summary[KILLED], summary[SURVIVED], summary[TIMEOUT], summary[COMPILE_ERROR],
            '%.1f%%' % (100.0 * rate) if rate is not None else 'n/a', seconds),
        '',
        '%-6s %-9s %-20s %-28s %-13s %s' % ('ID', 'Line:Col', 'Operator', 'Mutation', 'Status', 'Test / detail'),
    ]
    for outcome in outcomes:
        mutant = outcome.mutant
        detail = outcome.test or ''
        if outcome.status in (COMPILE_ERROR, TIMEOUT) and outcome.detail:
            detail = (detail + ': ' if detail else '') + outcome.detail
        report.append('%-6s %-9s %-20s %-28s %-13s %s' % (
            mutant.identifier, '%d:%d' % (mutant.line, mutant.column), mutant.operator, mutant.description,
            outcome.status, detail[:110]))
    survivors = [outcome for outcome in outcomes if outcome.status in (SURVIVED, TIMEOUT)]
    if survivors:
        report += ['', 'Surviving and timed-out mutants:']
        for outcome in survivors:
            mutant = outcome.mutant
            report.append('  %s %s:%d:%d %s %s | %s' % (
                mutant.identifier, source, mutant.line, mutant.column, outcome.status, mutant.description,
                _source_line(lines, mutant)[:110]))
    report += ['', 'Per operator:']
    for operator, counts in summary['by_operator'].items():
        report.append('  %-20s %s' % (operator, ', '.join('%s %d' % item for item in sorted(counts.items()))))
    return '\n'.join(report) + '\n'


def to_json(source: str, tests: list[str], build_dir: str, outcomes: list[Outcome], text: str,
            seconds: float) -> str:
    lines = text.split('\n')
    document = {
        'source': source,
        'tests': tests,
        'build_dir': build_dir,
        'seconds': round(seconds, 1),
        'summary': summarize(outcomes),
        'mutants': [
            {
                'id': outcome.mutant.identifier,
                'line': outcome.mutant.line,
                'column': outcome.mutant.column,
                'operator': outcome.mutant.operator,
                'group': outcome.mutant.group,
                'original': outcome.mutant.original,
                'replacement': outcome.mutant.replacement,
                'source_line': _source_line(lines, outcome.mutant),
                'status': outcome.status,
                'test': outcome.test,
                'detail': outcome.detail,
                'seconds': round(outcome.seconds, 2),
            }
            for outcome in outcomes
        ],
    }
    return json.dumps(document, indent=2) + '\n'
