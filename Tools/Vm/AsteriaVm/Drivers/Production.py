# SPDX-License-Identifier: Apache-2.0
"""Product boots keep console services resident.

Wait for display and init readiness, probe the resident PS/2 terminal on
serial and screen, and retain the initial user image's idle check.
"""
from __future__ import annotations

import time
import re

from AsteriaVerify.Elf import ElfFile
from AsteriaVm import Amd64Layout, Display, Evidence, Guest, Ia64Layout, Ps2, Stages
from AsteriaVm.Drivers import Amd64Default, Ia64Default
from AsteriaVm.Observers import Amd64Cpu, Amd64Evidence

CHECK = 'lifecycle.user-idle'


def xhci_failure_reported(lines: list[str]) -> bool:
    for line in lines:
        match = re.fullmatch(r'# init: xhci failed, continuing [0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7] '
                             r'step=(\d+) err=0x([0-9a-f]{2}) code=0x([0-9a-f]{16})', line)
        if match:
            step, error, code = int(match[1]), int(match[2], 16), int(match[3], 16)
            if 0 < step < 256 and error != 0 and code == (error << 40 | step << 32 | 0xc0de00e9):
                return True
    return False


USB_POLLING = re.compile(r'# usb: xhci\d+ port \d+ polling \d+ interfaces?')
USB_HOST_POLLING = re.compile(r'# usb: (ehci|ohci|uhci)\d+ port \d+ polling \d+ interfaces?')
USB_COMPANION = re.compile(r'# usb: ehci\d+ port \d+ full/low speed, released to companion portsc=0x[0-9a-f]{8}')


def usb_progress(context) -> None:
    """The USB driver's enumeration diagnostics reach the product console.

    Product init prints them as `# usb:` lines (the screen is the only
    channel on a machine without a serial port); the keyboard and the mouse
    each end their enumeration with a polling line. The screen comparison
    (`_display`) checks that the same lines are drawn. A USB host group case
    also names the controller each device ended on: a full-speed device on
    an EHCI port is released to its companion, which polls it.
    """
    # Beside an xHCI, a USB host group without devices (layout `none`) polls
    # nothing: the keyboard and the mouse are the xHCI's.
    usb_host = getattr(context.case, 'usb_host', '') if getattr(context.case, 'usb_layout', '') != 'none' else ''
    polling = USB_HOST_POLLING if usb_host else USB_POLLING
    index = -1
    for _ in range(2):
        index, _ = context.wait_for_line(lambda line: line.startswith('USER:') and polling.fullmatch(line[5:]),
                                         60.0, since=index + 1)
    lines = Guest.channel_lines(context.console.lines)
    connects = [line for line in lines if line.startswith('# usb: ') and ' connect usb' in line]
    context.check('product.usb-progress', len([line for line in lines if polling.fullmatch(line)]) >= 2
                  and len(connects) >= 2, '%d connect lines; keyboard and mouse reached polling' % len(connects))
    if usb_host:
        from AsteriaVm import Machine
        controllers = sorted({polling.fullmatch(line)[1] for line in lines if polling.fullmatch(line)})
        full = sum(1 for _, speed in Machine.USB_HOST_LAYOUTS[context.case.usb_layout] if speed)
        companion = 'uhci' if usb_host == 'ich9' else 'ohci'
        wanted = sorted({'ehci'} if full == 0 else ({companion} if full == 2 else {'ehci', companion}))
        released = [line for line in lines if USB_COMPANION.fullmatch(line)]
        # Itanium2-vpc's own OHCI may poll the machine's keyboard and tablet beside the case's
        # devices, so the wanted controllers are a subset.
        context.check('product.usb-controllers', set(wanted) <= set(controllers) and len(released) >= full,
                      'polling on %s (wanted %s); %d release(s) to a companion' % (controllers, wanted, len(released)))


def run(context) -> None:
    reached, detail = (_ia64 if getattr(context.case, 'isa', 'Amd64') == 'Ia64' else _amd64)(context)
    context.check(CHECK, reached, detail)
    _, display = context.wait_for_line(
        lambda line: line.startswith('USER:') and Display.parse_start(line[5:]) is not None, 60.0)
    failed = getattr(context.case, 'xhci_failure', False)
    usb_host = bool(getattr(context.case, 'usb_host', ''))
    # Itanium2-vpc has an OHCI (00:02.0) and a UHCI (00:03.0) of its own, which the USB host group
    # driver serves beside any the case adds.
    usb_ready = usb_host or (getattr(context.case, 'isa', 'Amd64') == 'Ia64'
                             and not getattr(context.case, 'machine', ''))
    ready = '# init: ready: display' + (', input' if (context.case.i8042 or context.case.xhci or usb_ready) else '') + (
        ', ps2' if context.case.i8042 else '') + (', xhci' if context.case.xhci and not failed else '') + (
        ', usb' if usb_ready else '')
    if failed:
        context.wait_for_line('USER:# init: xhci failed, continuing', 60.0)
    context.wait_for_line('USER:' + ready, 60.0)
    channel = Guest.channel_lines(context.console.lines)
    context.check('product.init-ready', ready in channel, display)
    inventory = next((index for index, line in enumerate(channel)
                      if line.startswith('# devices: input 8042=')), None)
    context.check('product.inventory', inventory is not None and inventory < channel.index(ready),
                  'boot inventory precedes init readiness')
    if (context.case.xhci and not failed) or usb_host:
        usb_progress(context)
    if context.case.xhci and usb_host:
        # The fifth component slot : display, broker, PS/2, the xHCI driver and the USB host group
        # run at once.
        started = sorted({match[1] for match in (re.fullmatch(r'# usb: (ehci|ohci|uhci)\d+ started, \d+ ports, '
                                                               r'(?:polled|interrupt)', line) for line in channel)
                          if match})
        context.check('product.five-components', ready in channel and started == ['ehci', 'uhci'],
                      'ready with ps2, xhci and usb; the group started %s' % started)
        path = context.artifact('Evidence.bin')
        problem = Evidence.save(context.qmp, context.console.lines, path)
        context.require('product.five-components-evidence', problem is None, problem or '')
        record = Amd64Evidence.schema(context.target.root).decode(path.read_bytes())
        endpoints = record.ObjectManagement.Created - record.ObjectManagement.Destroyed
        from AsteriaVerify import Layout
        watchdog_slots = Layout.constants((context.target.root / 'Source/Abi/Watchdog.hpp').read_text())['WatchdogServiceCount']
        endpoint_limit = 12 + watchdog_slots
        context.check('product.five-components-evidence',
                      record.Components.AddressSpacesLive == 5 and record.Components.ResourceDomainsLive == 5
                      and record.Threads.ThreadsUsage == 5 and endpoints <= endpoint_limit,
                      '%d spaces, %d ResourceDomains and %d threads live, %d of %d Endpoints'
                      % (record.Components.AddressSpacesLive, record.Components.ResourceDomainsLive,
                         record.Threads.ThreadsUsage, endpoints, endpoint_limit))
    if context.case.i8042 or usb_host:
        if failed:
            # Remove unowned USB input so QMP selects the resident PS/2 pair.
            for device in ('usbkbd', 'usbmouse'):
                context.qmp.execute('device_del', {'id': device})
                context.qmp.wait_event('DEVICE_DELETED', 10.0)
            context.check('product.optional-driver', xhci_failure_reported(channel),
                          'BDF, step, error and DriverFailure code reported; PS/2 and brokers remain resident')
        Ps2.inject_terminal(context)
        for line in ('asteria', '[L-- 5,-3]', '[--- 5,-3]'):
            context.wait_for_line('USER:' + line, 10.0)
        serial = context.artifact('Debug.log' if context.case.isa == 'Ia64' else 'Serial.log').read_text(errors='replace').splitlines()
        context.check('product.terminal-serial',
                      'USER:asteria' in serial and 'USER:[L-- 5,-3]' in serial,
                      'typed asteria and clicked at (5,-3)')
        # Keyboard and mouse on one UHCI share it in turns, each idle poll
        # yielding through a 33-frame cancellation (UsbHostDriver.md): a key
        # edge may wait about a tenth of a second, longer than the repeat
        # probe's settle allows. Repeats are checked on every other layout.
        if not (usb_host and context.case.usb_host == 'ich9' and context.case.usb_layout == 'companion'):
            repeat_terminal(context)
    _display(context)
    if context.case.i8042 or usb_host:
        _pointer_checks(context)


def repeat_terminal(context) -> None:
    """Hold through the delay; a trailing marker exposes repeats after release.

    Time bounds include QMP round trips and use an upper rate allowance. The
    screen comparison consumes the edited serial lines, checking erasure too.
    The detail records the measured rate against the configured 30 per second.
    """
    def edge(key, down):
        arguments = {'events': [dict(type='key', data=dict(down=down, key=dict(type='qcode', data=key)))]}
        if Ps2.usb_console(context.case):
            arguments['device'] = 'usbconsole'
        context.qmp.execute('input-send-event', arguments)

    def tap(key):
        edge(key, True)
        time.sleep(0.04)
        edge(key, False)
        time.sleep(Ps2.edge_pause(context))

    for prefix, key, duration, text in (('repeat ', 'r', 1.2, ''), ('erase ', 'backspace', 0.9, 'x' * 32)):
        for character in prefix + text:
            tap('spc' if character == ' ' else character)
        started = time.monotonic()
        edge(key, True)
        time.sleep(duration)
        edge(key, False)
        held = time.monotonic() - started
        # Redraws queued before the release settle first. IA-64's uncached
        # scanout takes 0.24 to 1 s per full screen (the guest's own `#
        # display:` line), so it waits a second there.
        time.sleep(1.0 if getattr(context.case, 'isa', 'Amd64') == 'Ia64' else 0.15)
        before = context.artifact('Repeat-' + key + '-released.ppm')
        after = context.artifact('Repeat-' + key + '-settled.ppm')
        context.qmp.screendump(before, format='ppm')
        quiet = 0.75
        time.sleep(quiet)
        context.qmp.screendump(after, format='ppm')
        context.check('terminal.repeat.stopped.' + key, before.read_bytes() == after.read_bytes(),
                      'screen remains unchanged for %.0f ms after release' % (quiet * 1000))
        tap('z')
        tap('ret')
        _, line = context.wait_for_line(lambda line: line.startswith('USER:' + prefix), 20.0)
        payload = line[5 + len(prefix):]
        match = re.fullmatch(r'(r+)z' if key == 'r' else r'(x*)z', payload)
        count = (len(match[1]) - 1 if key == 'r' else len(text) - len(match[1])) if match else -1
        upper = 1 + int(max(0.0, held - 0.5) * 30) + 2
        rate = count / (held - 0.5) if held > 0.5 else 0.0
        context.check('terminal.repeat.' + key, match is not None and 3 <= count <= upper,
                      '%d repeats/erasures during %.3f s hold (%.1f per second after the 0.5 s delay; '
                      'configured 30), allowed 3..%d; release marker %r'
                      % (count, held, rate, upper, payload))


def _console_lines(lines: list[str]) -> list[str]:
    # Shared-code and management diagnostics are serial-only. These are the
    # product init and terminal writes mirrored through the display broker.
    return [line for line in Guest.channel_lines(lines)
            if line.startswith(('# display:', '# devices:', '# init:', '# input:', '# pointer:', '# usb:', '[',
                                'repeat ', 'erase ')) or line == 'asteria']


def _display(context) -> None:
    """Rendering follows queued console writes; poll the observable screen."""
    path = context.artifact('Screen.ppm')
    deadline = time.monotonic() + context.seconds(10.0)
    while True:
        context.qmp.screendump(path, format='ppm')
        problems = Display.display_problems(path.read_bytes(), _console_lines(context.console.lines),
                                            context.target.root, isa=getattr(context.case, 'isa', 'Amd64'))
        if not problems or time.monotonic() >= deadline:
            break
        time.sleep(0.1)
    context.check_problems('product.display', problems
                           + Display.resolution_problems(_console_lines(context.console.lines), context.case.expected_display))


def _settle_pointer(context) -> None:
    """Wait until no pointer fence has followed for a quiet interval. A clamped
    position is reached before QEMU's mouse has handed over all of a phase's
    motion, and a mouse sharing a USB 1.1 controller with the keyboard drains
    it at the time-sliced controller's pace; the next phase's small move would otherwise merge with
    the remainder. The check that follows still requires the fenced position."""
    quiet = context.seconds(1.0 if getattr(context.case, 'isa', 'Amd64') == 'Ia64' else 0.5)
    deadline = time.monotonic() + context.seconds(10.0)
    count = sum(line.startswith('USER:# pointer:') for line in context.console.lines)
    changed = time.monotonic()
    while time.monotonic() - changed < quiet and time.monotonic() < deadline:
        time.sleep(0.05)
        now = sum(line.startswith('USER:# pointer:') for line in context.console.lines)
        if now != count:
            count, changed = now, time.monotonic()


def _pointer_checks(context, *, initial_y: int | None = None) -> None:
    """Fence each QMP move, then compare the complete screen at clamped positions."""
    mode = next(Display.parse_start(line) for line in Guest.channel_lines(context.console.lines)
                if Display.parse_start(line) is not None)
    width, height = mode[:2]
    if initial_y is None:
        initial_y = height // 2 - 3
    for name, dx, dy, wanted in (
            ('edge', -20000, 0, (0, initial_y)),
            ('corner', 0, -20000, (0, 0)),
            ('text', 20, 20, (20, 20))):
        # Standard PS/2 packets have signed 8-bit deltas. Keep each QMP
        # injection in range instead of relying on emulator overflow policy.
        count = (width // 100 + 2) if dx < -100 else ((height // 100 + 2) if dy < -100 else 1)
        for _ in range(count):
            usb = Ps2.usb_console(context.case)
            context.qmp.execute('input-send-event', {**({'device': 'usbconsole'} if usb else {}), 'events': [
                dict(type='rel', data=dict(axis='x', value=-100 if dx < -100 else dx)),
                dict(type='rel', data=dict(axis='y', value=-100 if dy < -100 else dy))]})
            time.sleep(0.25 if getattr(context.case, 'isa', 'Amd64') == 'Ia64' else 0.04)
        context.wait_for_line(lambda line: line.startswith('USER:# pointer:')
                              and Display.pointer_position([line[5:]], width, height) == wanted, 20.0)
        if getattr(context.case, 'usb_host', None):
            _settle_pointer(context)
        context.check('product.pointer.' + name,
                      Display.pointer_position(Guest.channel_lines(context.console.lines), width, height) == wanted,
                      'fenced pointer at %d,%d' % wanted)
        _display(context)
        context.artifact('Screen-' + name + '.ppm').write_bytes(context.artifact('Screen.ppm').read_bytes())


def _ia64(context) -> tuple[bool, str]:
    """After the kernel's last stage event, the BSP settles in the idle loop
    with the launch PSR (PL3), as the test firmware's
    `host.ia64.cpu.user-idle-reached` requires."""
    idle = ElfFile(context.image('user_elf')).symbol(Ia64Layout.USER_IDLE_SYMBOL).value
    psr = Ia64Layout.headers(context.target.root).user_psr
    Ia64Default.gate(context, Ia64Default.LAUNCH_STAGE, Ia64Default.BOOT_TIMEOUT)
    loop = Ia64Default.wait_for_user_loop(context, idle, psr)
    return loop.reached, loop.describe(idle, psr)


def _amd64(context) -> tuple[bool, str]:
    """After the kernel's last stage event, CPU 0 executes the user image's
    idle loop, which the image enters after `main` returned (both of its
    WaitSet waits included)."""
    loop = Amd64Cpu.idle_loop(context.target.tool('objdump'), context.image('user_elf'),
                              Amd64Layout.USER_IDLE_SYMBOL)
    context.wait_for_line(Stages.predicate(Amd64Default.LAUNCH_STAGE), Amd64Default.BOOT_SECONDS)
    return Amd64Cpu.wait_for_idle(
        context.qmp, context.artifact('CpuPoll.elf'), loop,
        context.seconds(Amd64Default.WAIT_SECONDS + Amd64Default.IDLE_SECONDS),
        abort=lambda: context.exited() or next(iter(context.console.failures()), None))
