# SPDX-License-Identifier: Apache-2.0
"""IA-64 cooperative terminal stop and physical scanout observers."""
from __future__ import annotations

import time

from AsteriaVm import Display, Evidence, Lan, Ps2
from AsteriaVm.Observers import Ia64Evidence, Ia64State
from AsteriaVm.Qmp import wait_until


def run(context) -> None:
    # As on AMD64, the runner is the e1000e's LAN while the root service's
    # network demonstration runs before the display fence (Source/User/Network/NetworkStack.hpp,
    # ): the peer answers ARP and plays its script once the
    # root service listens.
    peer = (Lan.LanPeer(context.lan, context.artifact(Lan.PCAP_ARTIFACT).parent).start()
            if getattr(context, 'lan', None) is not None and context.case.expected_online_mask != 1 else None)
    try:
        _run(context, peer)
    finally:
        if peer is not None:
            peer.stop()


def _run(context, peer: Lan.LanPeer | None) -> None:
    early = context.case.expected_online_mask == 1
    option = '2' if early else ('3' if context.case.expected_stop == 'Lockup' else '1')
    context.require('lifecycle.preset-stop-probe', context.target.option('ia64_stop_probe') == option,
                    'named stop probe option ' + option)
    if not early:
        # As on AMD64, a published i8042 makes the root's hardware phase wait
        # for scripted input before the display fence. The fence follows the
        # root service's storage and network demonstrations too (DMA isolation
        # Source/User/Network/NetworkStack.hpp ), so it may come some
        # minutes after the boot on a loaded host.
        injection = Ps2.Injection(context)

        def fenced() -> bool:
            injection.poll()
            if peer is not None and any(line == 'USER:' + Lan.LISTENING_LINE for line in context.console.lines):
                peer.start_script()  # Idempotent
            return bool(context.exited() or context.console.failures()
                        or any('USER:# display: stopped' in line for line in context.console.lines))

        wait_until(fenced, context.seconds(480), 0.1)
        # The injected terminal line may arrive in the same console read as
        # the display fence, so this expected-stop driver permits it here.
        context.console.wait_for('USER:# display: stopped', context.seconds(30),
                                 abort=context.exited, allow_stop=True)
    context.console.wait_for('ASTERIA:STOP-COMPLETE', context.seconds(30),
                             abort=context.exited, allow_stop=True)
    path = context.artifact(Evidence.ARTIFACT)
    deadline = time.monotonic() + context.seconds(30)
    while time.monotonic() < deadline:
        problem = Evidence.save(context.qmp, context.console.lines, path)
        if problem is None and Ia64Evidence.schema(context.target.root).decode(path.read_bytes()).Stops.DrawPublished:
            break
        time.sleep(0.05)
    context.qmp.stop()
    problem = Evidence.save(context.qmp, context.console.lines, path)
    context.require('host.ia64.stop.evidence', problem is None, problem or '')
    evidence = Ia64Evidence.schema(context.target.root).decode(path.read_bytes())
    stops = evidence.Stops
    context.require('host.ia64.stop.draw-completed', stops.DrawPublished == 1,
                    'evidence survived but drawing did not finish')
    mask = 1 if early else (1 << context.case.cpus) - 1
    expected_unconfirmed = 1 if context.case.expected_stop == 'Lockup' else 0
    context.check('host.ia64.stop.all-frozen', stops.ConfirmedMask == mask & ~expected_unconfirmed and stops.UnconfirmedMask == expected_unconfirmed,
                  'confirmed=%#x unconfirmed=%#x' % (stops.ConfirmedMask, stops.UnconfirmedMask))
    if context.case.expected_stop == 'Lockup':
        context.check('host.ia64.watchdog.stalled-cpu', evidence.Watchdog.StalledMask == 1
                      and stops.Processors[stops.Winner].Stop.Parameters[0] == 1 and stops.Winner != 0
                      and evidence.Watchdog.Processors[0].Captured == 0
                      and evidence.Watchdog.Processors[0].HeartbeatSite != 0
                      and evidence.Watchdog.Processors[0].Thread != 0, 'CPU 0 unconfirmed with last heartbeat and root thread')
        from AsteriaVerify import Layout
        watchdog_constants = Layout.constants((context.target.root / 'Source/Abi/Watchdog.hpp').read_text())
        period = watchdog_constants['WatchdogDefaultSeconds'] * 1000
        sample = evidence.Watchdog.Processors[0]
        parameters = stops.Processors[stops.Winner].Stop.Parameters
        context.check('host.ia64.watchdog.period-and-component', evidence.Watchdog.PeriodSeconds * 1000 == period
                      and sample.AgeMilliseconds >= period and parameters[1] == period
                      and parameters[2] == stops.Winner and parameters[3] >= period
                      and sample.Component == watchdog_constants['WatchdogRootComponent'],
                      'configured period, retained age, detector and running root component')
    else:
        context.check('host.ia64.stop.injection-point', stops.Processors[stops.Winner].Stop.Parameters == [int(option), 0, 0, 0],
                      'named injection payload')
    issues = []
    for cpu in range(context.case.cpus):
        if not mask & (1 << cpu):
            if stops.Processors[cpu].Stop.State != 0:
                issues.append('CPU %d was not online but has a terminal record' % cpu)
            continue
        text = context.qmp.hmp('info registers', cpu=cpu)
        context.artifact('StopRegisters.Cpu%d.txt' % cpu).write_text(text)
        state = Ia64State.parse(text, cpu)
        psr = state.get('IP')['psr']
        if psr & (1 << 14) or (psr >> 32) & 3:
            issues.append('CPU %d is not parked at PL0 with PSR.i clear' % cpu)
        # Scanout installs no TR; each admitted runtime CPU keeps its budget.
        dtrs = len(state.translation_registers('DTLB'))
        itrs = len(state.translation_registers('ITLB'))
        if (dtrs, itrs) != (4, 1):
            issues.append('CPU %d has %d DTRs / %d ITRs, expected 4 / 1 active (plan reserves 4 / 2)' % (cpu, dtrs, itrs))
    context.check_problems('host.ia64.stop.parked', issues)
    serial = context.artifact('Debug.log').read_text(errors='replace')
    context.check('host.ia64.stop.uart', 'ASTERIA:STOP:CPU=' in serial and 'ASTERIA:STOP-COMPLETE' in serial,
                  'physical UART carries initiating stop and completion')
    ppm = context.artifact('StopScreen.ppm')
    context.qmp.screendump(ppm, format='ppm')
    context.check_problems('host.display.stop-screen',
        Display.stop_display_problems(ppm.read_bytes(), stops, context.target.root, context.case.cpus, 'Ia64', watchdog=evidence.Watchdog, text_bounds=Display.stop_text_bounds(context.image('kernel_elf'), context.console.lines)))
    context.qmp.screendump(context.artifact('StopScreen.png'), format='png')
