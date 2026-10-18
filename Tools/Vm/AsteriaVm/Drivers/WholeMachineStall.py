# SPDX-License-Identifier: Apache-2.0
"""Watchdog.md: SIGSTOP the hypervisor without freezing its virtual clock."""
import json
import signal
import time

from AsteriaVm import Evidence
from AsteriaVm.Drivers import ProductDesktop
from AsteriaVm.Observers import Amd64Evidence, Ia64Evidence, UserProcessors
from AsteriaVerify import Layout


def pause(process, seconds):
    """Always resume our own child, including when the observation is interrupted."""
    process.send_signal(signal.SIGSTOP)
    started = time.monotonic()
    try:
        time.sleep(seconds)
    finally:
        process.send_signal(signal.SIGCONT)
    return time.monotonic() - started


def run(context):
    early = context.case.whole_machine_pause == 'early'
    marker = ('ASTERIA:STAGE:AMD64-USER-LAUNCH' if context.case.isa == 'Amd64'
              else 'ASTERIA:STAGE:INITIAL-USER-IMAGE-LAUNCH') if early else 'USER:# session: desktop '
    context.wait_for_line((lambda line: line == marker) if early else marker, 180)
    schema = (Amd64Evidence if context.case.isa == 'Amd64' else Ia64Evidence).schema(context.target.root)
    before = context.artifact('BeforePause.bin')
    problem = Evidence.save(context.qmp, context.console.lines, before)
    context.require('pause.evidence.before', problem is None, problem or '')
    record = schema.decode(before.read_bytes())
    context.require('pause.four-processors', record.Processors.ProcessorCount == 4
                    if context.case.isa == 'Ia64' else record.Runtime.ProcessorCount == 4,
                    'all four processors must be admitted')
    context.require('pause.clock-admission', record.Clocks.Published == (1 << context.case.cpus) - 1,
                    repr(record.Clocks))
    constants = Layout.constants((context.target.root / 'Source/Abi/Watchdog.hpp').read_text())
    seconds = constants['WatchdogDefaultSeconds'] + 2
    elapsed = pause(context.process, seconds)
    context.artifact('Pause.json').write_text(json.dumps(dict(phase=context.case.whole_machine_pause,
        seconds=seconds, elapsed=elapsed, signals=['SIGSTOP', 'SIGCONT']), indent=2))
    context.check('pause.longer-than-period', elapsed > constants['WatchdogDefaultSeconds'])
    context.pause_seconds = elapsed
    ProductDesktop.run(context)
    context.stop()
    after = context.artifact(Evidence.ARTIFACT)
    problem = Evidence.save(context.qmp, context.console.lines, after)
    context.require('pause.evidence.after', problem is None, problem or '')
    observed = schema.decode(after.read_bytes())
    context.check('pause.no-lockup', not any(line.startswith('ASTERIA:STOP:') for line in context.console.lines))
    if context.case.isa == 'Amd64':
        context.check_problems('pause.desktop.on-ap', UserProcessors.session_problems(observed.UserProcessors))
