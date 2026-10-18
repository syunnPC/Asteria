# SPDX-License-Identifier: Apache-2.0
"""Injected stop after the display fence, and a monitor NMI on normal firmware."""
from AsteriaVm import Display, Evidence, Lan, Ps2
from AsteriaVm.Observers import Amd64Evidence


def run(context) -> None:
    if context.case.expected_online_mask == 1:
        context.require('lifecycle.preset-early-stop', context.target.option('amd64_stop_probe') == '2', 'early probe option')
        _stop(context, early=True)
        return
    if context.case.expected_stop == 'TestInjected':
        context.require('lifecycle.preset-stop-probe', context.target.option('amd64_stop_probe') == '1',
                        'stop probe preset must enable amd64_stop_probe')
    peer = Lan.LanPeer(context.lan, context.artifact(Lan.PCAP_ARTIFACT).parent).start()
    try:
        context.wait_for_line(Lan.LISTENING_LINE, 180)
        peer.start_script()
        _stop(context)
    finally:
        peer.stop()


def _stop(context, early=False) -> None:
    if not early:
        from AsteriaVm.Qmp import wait_until
        injection = Ps2.Injection(context)
        wait_until(lambda: injection.poll() or 'USER:# display: stopped' in context.console.lines,
                   context.seconds(180), 0.1)
        context.wait_for_line('USER:# display: stopped', 1)
    if context.case.expected_stop == 'UnexpectedNmi':
        response = context.qmp.hmp('nmi', cpu=0)
        context.artifact('NmiMonitor.txt').write_text(response)
        context.artifact('NmiRegisters.txt').write_text(context.qmp.hmp('info registers', cpu=0))
    context.console.wait_for('ASTERIA:STOP-COMPLETE', context.seconds(30),
                             abort=context.exited, allow_stop=True)
    if context.case.expected_stop == 'TestInjected' and not early:
        # An outside NMI must not park the owner or discard its screen.
        context.artifact('WinnerNmi.txt').write_text(context.qmp.hmp('nmi', cpu=0))
    # STOP-COMPLETE precedes drawing. Poll the separate draw publication while
    # the guest runs; a fault still leaves the terminal evidence artifact.
    import time
    path = context.artifact(Evidence.ARTIFACT)
    deadline = time.monotonic() + context.seconds(30)
    while time.monotonic() < deadline:
        problem = Evidence.save(context.qmp, context.console.lines, path)
        if problem is None and Amd64Evidence.schema(context.target.root).decode(path.read_bytes()).Stops.DrawPublished:
            break
        time.sleep(0.05)
    context.qmp.stop()
    path = context.artifact(Evidence.ARTIFACT)
    problem = Evidence.save(context.qmp, context.console.lines, path)
    context.require('host.amd64.stop.evidence', problem is None, problem or '')
    evidence = Amd64Evidence.schema(context.target.root).decode(path.read_bytes())
    stops = evidence.Stops
    context.require("host.amd64.stop.draw-completed", stops.DrawPublished == 1, "stop evidence survived but drawing did not complete")
    serial = context.artifact('Serial.log').read_text(errors='replace')
    context.check('host.amd64.stop.selected-uart', 'ASTERIA:STOP:CPU=' in serial and 'ASTERIA:STOP-COMPLETE' in serial,
                  'selected 16550 must receive the stop and completion')
    _inspect_machine(context, evidence)
    if context.case.expected_stop == 'Lockup':
        from AsteriaVerify.Elf import ElfFile
        elf = ElfFile(context.image('kernel_elf'))
        delta = Display.kernel_load_delta(context.image('kernel_elf'), context.console.lines)
        begin, end = (elf.symbol('AsteriaAmd64WatchdogStall' + name).value + delta for name in ('Begin', 'End'))
        target = stops.Processors[0].Snapshot
        context.check('host.amd64.watchdog.stalled-cpu', evidence.Watchdog.StalledMask == 1
                      and stops.Processors[stops.Winner].Stop.Parameters[0] == 1 and stops.Winner != 0,
                      'CPU 0 identified by a surviving AP')
        from AsteriaVerify import Layout
        watchdog_constants = Layout.constants((context.target.root / 'Source/Abi/Watchdog.hpp').read_text())
        period = watchdog_constants['WatchdogDefaultSeconds'] * 1000
        sample = evidence.Watchdog.Processors[0]
        parameters = stops.Processors[stops.Winner].Stop.Parameters
        context.check('host.amd64.watchdog.period-and-component', evidence.Watchdog.PeriodSeconds * 1000 == period
                      and sample.AgeMilliseconds >= period and parameters[1] == period
                      and parameters[2] == stops.Winner and parameters[3] >= period
                      and sample.Component == watchdog_constants['WatchdogRootComponent'],
                      'configured period, retained age, detector and running root component')
        context.check('host.amd64.watchdog.nmi-rip', begin <= target.Rip < end and not target.Rflags & (1 << 9)
                      and target.Cr3 != 0 and evidence.Watchdog.Processors[0].Thread != 0,
                      'NMI captured the IF-clear probe and running root thread')
    mask = context.case.expected_online_mask if early else (1 << context.case.cpus) - 1
    context.check('host.amd64.stop.all-frozen', stops.ConfirmedMask == mask and stops.UnconfirmedMask == 0,
                  'confirmed=%#x unconfirmed=%#x' % (stops.ConfirmedMask, stops.UnconfirmedMask))
    ppm = context.artifact('StopScreen.ppm')
    context.qmp.screendump(ppm, format='ppm')
    issues = Display.stop_display_problems(ppm.read_bytes(), stops, context.target.root, context.case.cpus, watchdog=evidence.Watchdog, text_bounds=Display.stop_text_bounds(context.image("kernel_elf"), context.console.lines))
    context.check_problems('host.display.stop-screen', issues)


def _inspect_machine(context, evidence) -> None:
    from AsteriaVm import Amd64Layout
    from AsteriaVm.Observers import Amd64Cpu, Amd64Paging
    import struct

    snapshot = Amd64Cpu.capture(context.qmp, context.artifact('StoppedCpuState.elf'))
    constants = Amd64Layout.headers(context.target.root)
    stops = evidence.Stops
    pool = evidence.Runtime.PoolPhysicalStart
    issues = []
    if len(snapshot.cpus) != context.case.cpus:
        issues.append('CPU snapshot count differs from the case')
    tops = []
    for cpu in snapshot.cpus:
        if not stops.OnlineMask & (1 << cpu.index):
            continue
        top = int.from_bytes(context.qmp.pmemsave(cpu.segments['tr'].base + 44, 8), 'little')
        offset, pages = constants.machine_check_stack
        from AsteriaVerify.Layout import constants as source_constants
        layout = source_constants((context.target.root / 'Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp').read_text())
        expected = pool + (layout['NmiStackFirstPage'] + cpu.index * layout['NmiStackStridePages'] + pages) * constants.page_size
        mc = int.from_bytes(context.qmp.pmemsave(cpu.segments['tr'].base + 52, 8), 'little')
        if mc != constants.stack_extent(pool, cpu.index) + (offset + pages) * constants.page_size or mc == top:
            issues.append('CPU %d machine check IST3 is not distinct' % cpu.index)
        tops.append(top)
        if top != expected:
            issues.append('CPU %d IST2 top %#x differs from its reserved stack %#x' % (cpu.index, top, expected))
        if cpu.index != stops.Winner or context.case.expected_stop == 'UnexpectedNmi':
            if not top - pages * constants.page_size < cpu.rsp <= top:
                issues.append('CPU %d parked outside its private IST2 stack' % cpu.index)
        if cpu.rflags & (1 << 9) or cpu.cpl != 0:
            issues.append('CPU %d is not parked at CPL0 with interrupts disabled' % cpu.index)
        if cpu.index == stops.Winner and cpu.cr3 != stops.ApertureRoot:
            issues.append('winner is not on the stop root')
    if len(tops) != len(set(tops)):
        issues.append('CPUs share an IST2 top')
    context.check_problems('host.amd64.stop.private-ist2', issues)

    memory = Amd64Paging.PageCache(lambda page: context.qmp.pmemsave(page, 4096))
    walker = Amd64Paging.PageWalker(memory)
    issues = []
    if stops.ApertureRoot != evidence.Runtime.SharedRootPhysicalAddress or not stops.ApertureBytes:
        issues.append('aperture root/extent differs from the boot reservation')
    count = (stops.ApertureBytes + 4095) // 4096
    first = walker.walk(stops.ApertureRoot, stops.ApertureBase)
    if first is None:
        issues.append('aperture has no first page')
    else:
        for page in range(count):
            address = stops.ApertureBase + page * 4096
            leaf = walker.walk(stops.ApertureRoot, address)
            if leaf is None or (leaf.writable, leaf.user, leaf.executable) != (True, False, False):
                issues.append('aperture page %d is not supervisor RW/NX' % page)
                break
            if leaf.physical != first.physical + page * 4096:
                issues.append('aperture page %d does not map consecutive framebuffer memory' % page)
                break
            if Amd64Paging.memory_type(leaf, constants.expected_pat) != 1:
                issues.append('aperture page %d does not select the kernel WC PAT entry' % page)
                break
        issues += Amd64Paging.expect_unmapped(walker, stops.ApertureRoot, stops.ApertureBase + count * 4096,
                                             'the page after the mode extent')
    context.check_problems('host.amd64.stop.aperture', issues)
