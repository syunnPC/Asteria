# SPDX-License-Identifier: Apache-2.0
"""Readiness-gated hardware input injection through PS/2 or USB HID."""
from __future__ import annotations

import json
import time

from AsteriaVm import Evidence, Guest
from AsteriaVm.Observers import Amd64Evidence, Ia64Evidence

READY = '# input: PS/2 ready, QMP may inject'
DONE = "# input: typed 'asteria', pointer moved (5,-3), left button, PS/2, secure attention reported, second client empty"
SKIPPED = '# input: i8042 not published, PS/2 demo skipped'
EVENT_COUNT = 25  # eight keys down/up, motion, button down/up, six chord edges
# A woken IA-64 driver may wait behind other threads and then drain several
# host-paced edges in one delivery. There each edge waits for the guest's own
# next delivery, bounded so that a driver that never takes its line still
# finishes the script and fails the check.
DELIVERY_SECONDS = 5.0


class Injection:
    def __init__(self, context, *, ready=READY):
        self.context = context
        self.ready = ready
        self.sent = False
        self.before = None

    def poll(self):
        context = self.context
        if self.sent or self.ready not in Guest.channel_lines(context.console.lines):
            return
        self.sent = True
        baseline = context.artifact('Ps2Before.bin')
        problem = Evidence.save(context.qmp, context.console.lines, baseline)
        context.require('host.ps2.baseline', problem is None, problem or '')
        ia64 = context.case.isa == 'Ia64'
        decode = (Ia64Evidence.schema if ia64 else Amd64Evidence.schema)(context.target.root).decode
        self.before = decode(baseline.read_bytes()).Interrupts
        delivered = self.before.Delivered
        events = []
        for key in ('a', 's', 't', 'e', 'r', 'i', 'a', 'ret'):
            events += [dict(type='key', data=dict(down=down, key=dict(type='qcode', data=key)))
                       for down in (True, False)]
        # Relative screen y is negative up. QEMU converts to PS/2 positive up.
        events.append(dict(type='rel', data=dict(axis='x', value=5)))
        motion_y = dict(type='rel', data=dict(axis='y', value=-3))
        events += [dict(type='btn', data=dict(down=down, button='left')) for down in (True, False)]
        events += [dict(type='key', data=dict(down=down, key=dict(type='qcode', data=key)))
                   for key, down in (('ctrl', True), ('alt', True), ('delete', True),
                                     ('delete', False), ('alt', False), ('ctrl', False))]
        transcript = []
        for event in events:
            batch = [event, motion_y] if event['type'] == 'rel' else [event]
            arguments = {'events': batch}
            if context.case.xhci:
                arguments['device'] = 'usbconsole'
            context.qmp.execute('input-send-event', arguments)
            transcript.append(batch)
            # Separate hardware events so a line delivery proves each injected
            # edge; controller packet bytes may generate further deliveries.
            if ia64:
                delivered = self._await_delivery(decode, delivered)
            else:
                time.sleep(0.04)
        context.artifact('Ps2Injection.json').write_text(json.dumps(transcript, indent=2) + '\n')

    def _await_delivery(self, decode, delivered: int) -> int:
        """The guest's delivery count once it exceeds `delivered`, or
        `delivered` after DELIVERY_SECONDS (scaled) without one."""
        context = self.context
        scratch = context.artifact('Ps2Pacing.bin')
        deadline = time.monotonic() + context.seconds(DELIVERY_SECONDS)
        while True:
            if Evidence.save(context.qmp, context.console.lines, scratch) is None:
                current = decode(scratch.read_bytes()).Interrupts.Delivered
                if current > delivered:
                    return current
            if time.monotonic() >= deadline:
                return delivered
            time.sleep(0.05)

    def check(self, record):
        context = self.context
        channel = Guest.channel_lines(context.console.lines)
        done = DONE if self.ready == READY else DONE.replace('PS/2', 'xHCI')
        prefix = 'host.ps2.' if self.ready == READY else 'host.xhci.'
        context.check(prefix + 'client', self.sent and done in channel,
                      'typed text, exact pointer values, attention and empty second client')
        if self.before is not None and record is not None:
            delivered = record.Interrupts.Delivered - self.before.Delivered
            spurious = record.Interrupts.Spurious - self.before.Spurious
            # The lines follow the firmware's evidence (QEMU: edge, active
            # high) and the driver drains the controller after each re-arm,
            # so a delivery the guest takes late can carry two injected
            # events' bytes: require
            # most events to have their own delivery (polling would give
            # none), and the client check proves every byte arrived.
            least = (EVENT_COUNT + 1) // 2
            context.check(prefix + 'interrupts', delivered >= least and spurious == 0,
                          '%d delivered for %d injected events (at least %d); %d spurious'
                          % (delivered, EVENT_COUNT, least, spurious))


def edge_pause(context) -> float:
    """Seconds after a scripted input edge that completes a key press (its
    release) or carries pointer input. Single-threaded TCG runs an IA-64
    machine's processors round-robin, each for a whole emulator time slice,
    so with more than one CPU edges 40 ms apart can pile up behind the BSP
    beyond what the emulated i8042 queues; they are spaced 250 ms there, and
    for IA-64's USB host group cases, whose emulated controllers drain one
    report per poll more slowly than 40 ms edges arrive. A press is released
    after 40 ms either way, well inside the 500 ms repeat delay even when the
    guest sees both edges late."""
    ia64 = getattr(context.case, 'isa', 'Amd64') == 'Ia64'
    slow = getattr(context.case, 'cpus', 1) > 1 or bool(getattr(context.case, 'usb_host', ''))
    return 0.25 if ia64 and slow else 0.04


def usb_console(case) -> bool:
    """Whether QMP input names the `usbconsole` display (Scenarios.Case.usb_console),
    for case records that predate the property: a working xHCI case."""
    named = getattr(case, 'usb_console', None)
    if named is not None:
        return named
    return bool(getattr(case, 'xhci', False)) and not getattr(case, 'xhci_failure', False)


def inject_terminal(context) -> None:
    """Probe a ready product terminal without the scenario's attention chord."""
    transcript = []
    events = [[dict(type='key', data=dict(down=down, key=dict(type='qcode', data=key)))]
              for key in (*'asteria', 'ret') for down in (True, False)]
    events.append([dict(type='rel', data=dict(axis='x', value=5)),
                   dict(type='rel', data=dict(axis='y', value=-3))])
    events += [[dict(type='btn', data=dict(down=down, button='left'))] for down in (True, False)]
    for batch in events:
        arguments = {'events': batch}
        if usb_console(context.case):
            arguments['device'] = 'usbconsole'
        if batch[0]['type'] != 'key' and batch is events[16] and getattr(context.case, 'usb_host', ''):
            # A USB host group case's keyboard and mouse are separate
            # devices, polled each at its controller's pace, perhaps in turns
            # on a shared one (UsbHostDriver.md): the mouse's report may
            # overtake the keyboard's queued ones, so the line is typed
            # first for the terminal to show it whole.
            context.wait_for_line('USER:asteria', 10.0)
        context.qmp.execute('input-send-event', arguments)
        transcript.append(batch)
        press = batch[0]['type'] == 'key' and batch[0]['data']['down']
        time.sleep(0.04 if press else edge_pause(context))
    context.artifact('Ps2Injection.json').write_text(json.dumps(transcript, indent=2) + '\n')
