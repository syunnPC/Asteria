# SPDX-License-Identifier: Apache-2.0
"""Supervisor-visible native fault probes reported by the test pager."""
from AsteriaVm import Guest

COUNT_START = '# spinner: counting'
COUNT_DONE = '# spinner: done, complete IA64 state preserved'

LINES = (
    '# ia64 fault: break immediate and slot delivered, terminated',
    '# ia64 fault: privileged operation delivered, terminated',
    '# ia64 fault: NaT register consumption delivered, terminated',
    '# ia64 fault: unaligned IFA delivered, terminated',
    '# ia64 fault: data access rights IFA delivered, terminated',
    '# ia64 fault: instruction access rights IFA delivered, terminated',
    '# ia64 fault: chk.s branched to recovery, no fault',
    '# ia64 fault: lazy instruction resolved and retried',
    '# ia64 fault: lazy RSE spills and incomplete-frame fills resolved and resumed',
    '# ia64 fault: busy pager yielded and retried, both delivered',
    COUNT_START,
    COUNT_DONE,
)


def problems(lines: list[str]) -> list[str]:
    observed = [line for line in Guest.channel_lines(lines)
                if line.startswith(('# ia64 fault:', '# spinner:'))]
    return [] if tuple(observed) == LINES else ['native fault probes are %r, expected %r' % (observed, LINES)]
