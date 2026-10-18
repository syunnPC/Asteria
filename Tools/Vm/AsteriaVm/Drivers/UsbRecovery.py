# SPDX-License-Identifier: Apache-2.0
"""The USB host group driver's recovery on the test firmware.

The test pager starts the group alone beside display and broker, waits for its
interfaces and asks for input; the runner types `asteria`, Enter, moves the
pointer and clicks. The pager kills the driver with its interrupt polls queued
and the product's recovery proves every controller quiet, registers the source
again and restarts it; the runner types again through the restarted driver.
Two more kills restart it, a fourth gives it up. The lines name each step; the
kernel's evidence shows every drained assignment quarantined and confirmed,
no DMA fault and every Interrupt and IommuDomain reclaimed.
"""
from __future__ import annotations

import re

from AsteriaVm import Display, Evidence, Guest, Ps2
from AsteriaVm.Drivers import Production
from AsteriaVm.Observers import Amd64Evidence, Ia64Evidence

READY = '# input: USB host ready, two boot interfaces, QMP may type'
RESTARTED = '# input: USB host killed with polls queued, quiet, restarted, QMP may type'
GIVEN_UP = '# input: USB host typed after its restart, killed four times, restarted three, given up'
KILLED = '# init: usb failed, continuing: killed'
QUIET = re.compile(r'# usb: (ehci|ohci|uhci)(\d) quiet \((?:was running, )?halted and reset, \d+ polls, '
                   r'status 0x[0-9a-f]{8}\), released')


def run(context) -> None:
    ia64 = context.case.isa == 'Ia64'
    reached, detail = (Production._ia64 if ia64 else Production._amd64)(context)
    context.check('lifecycle.user-idle', reached, detail)
    # Every scenario before it runs on IA-64's test firmware too.
    context.wait_for_line('USER:' + READY, 600.0 if ia64 else 120.0)
    Ps2.inject_terminal(context)
    context.wait_for_line('USER:' + RESTARTED, 300.0 if ia64 else 120.0)
    Ps2.inject_terminal(context)
    context.wait_for_line('USER:' + GIVEN_UP, 600.0 if ia64 else 240.0)
    context.wait_for_line('USER:# root service: done', 120.0)
    lines = Guest.channel_lines(context.console.lines)
    context.check('pager.frame-rights', Guest.FRAME_RIGHTS_LINE in lines)
    published = sum(1 for line in lines if line.startswith('# devices: ') and 'usb host driver publication=published'
                    in line)
    quiet = [QUIET.fullmatch(line) for line in lines]
    controllers = sorted({(match[1], match[2]) for match in quiet if match})
    context.check('usb.typed-twice', lines.count('asteria') >= 2 and READY in lines and RESTARTED in lines,
                  'typed and clicked before the kill and through the restarted driver')
    context.check('usb.killed-and-reported', lines.count(KILLED) == 4, 'four kills reported as `%s`' % KILLED)
    context.check('usb.quiet-proofs', len(controllers) == published > 0
                  and sum(1 for match in quiet if match) == 4 * published,
                  '%d controllers proven quiet after each of the four kills: %s' % (published, controllers))
    restarts = [line for line in lines if line.startswith('# init: usb restart')]
    context.check('usb.restarted-three-times',
                  restarts == ['# init: usb restarting (1 of 3)', '# init: usb restarted (1 of 3)',
                               '# init: usb restarting (2 of 3)', '# init: usb restarted (2 of 3)',
                               '# init: usb restarting (3 of 3)', '# init: usb restarted (3 of 3)'],
                  '; '.join(restarts))
    context.check('usb.given-up', '# init: usb gave up after 3 restarts' in lines and GIVEN_UP in lines,
                  'the fourth kill gives the group up, its controllers released')
    path = context.artifact('UsbRecoveryEvidence.bin')
    problem = Evidence.save(context.qmp, context.console.lines, path)
    context.require('usb.evidence', problem is None, problem or '')
    record = (Ia64Evidence if ia64 else Amd64Evidence).schema(context.target.root).decode(path.read_bytes())
    context.check('usb.drained', record.DeviceControl.Quarantines >= 4 * published
                  and record.DeviceControl.Confirmations >= 4 * published,
                  '%d quarantines and %d confirmations for %d controllers killed four times'
                  % (record.DeviceControl.Quarantines, record.DeviceControl.Confirmations, published))
    context.check('usb.dma-confined', record.Iommu.DomainsCreated >= 4 and record.Iommu.DomainsReclaimed >= 4
                  and record.Iommu.Faults == 0,
                  'every incarnation confined, no DMA fault (%d domains created, %d reclaimed)'
                  % (record.Iommu.DomainsCreated, record.Iommu.DomainsReclaimed))
    context.check('usb.interrupts-reclaimed', record.Interrupts.Live == 0,
                  'every incarnation\'s Interrupts collected (%d live)' % record.Interrupts.Live)
    screen = context.artifact('Screen.ppm')
    context.qmp.screendump(screen, format='ppm')
    context.check_problems('usb.display', Display.display_problems(
        screen.read_bytes(), Guest.root_service_lines(context.console.lines), context.target.root))
