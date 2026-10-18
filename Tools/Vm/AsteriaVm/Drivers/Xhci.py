# SPDX-License-Identifier: Apache-2.0
"""Isolated xHCI HID scenario: boot reports, MSI events and kill/reassignment."""
from AsteriaVm import Display, Evidence, Guest, Ps2
from AsteriaVm.Drivers import Production
from AsteriaVm.Observers import Amd64Evidence

READY = '# input: xHCI ready, two boot interfaces, QMP may inject'
RECOVERED = '# input: xHCI killed with polls queued, quiet, reassigned, two interfaces rebound'
# The product's recovery restarted it; typing and the pointer resume through it, and three more
# kills restart it twice and give it up.
TYPE = '# input: xHCI restarted, QMP may type'
GIVEN_UP = '# input: xHCI typed after its restart, killed four times, restarted three, given up'


def run(context):
    reached, detail = Production._amd64(context)
    context.check('lifecycle.user-idle', reached, detail)
    context.wait_for_line('USER:' + READY, 60.0)
    injection = Ps2.Injection(context, ready=READY)
    injection.poll()
    context.wait_for_line('USER:# input: xHCI rebound, QMP may unplug mouse', 30.0)
    context.qmp.execute('device_del', {'id': 'usbmouse'})
    context.qmp.wait_event('DEVICE_DELETED', 10.0)
    context.wait_for_line('USER:# input: xHCI disconnected, QMP may reconnect mouse', 20.0)
    context.qmp.execute('device_add', {'driver': 'usb-mouse', 'id': 'usbmouse', 'bus': 'xhci.0'})
    context.wait_for_line('USER:' + RECOVERED, 30.0)
    context.wait_for_line('USER:' + TYPE, 30.0)
    Ps2.inject_terminal(context)
    context.wait_for_line('USER:' + GIVEN_UP, 240.0)
    context.wait_for_line('USER:# root service: done', 30.0)
    lines = Guest.channel_lines(context.console.lines)
    context.check('xhci.hid-rebound', RECOVERED in lines,
                  'killed with interrupt IN queued; manager reset; new instance enumerated both devices')
    restarts = [line for line in lines if line.startswith('# init: xhci ') and (' restarting (' in line
                                                                              or ' restarted (' in line)]
    context.check('xhci.restarts', len(restarts) == 6 and restarts[-1].endswith(' restarted (3 of 3)')
                  and any(line.startswith('# init: xhci ') and line.endswith(' gave up after 3 restarts')
                          for line in lines)
                  and sum(1 for line in lines if line.startswith('# usb: xhci0 quiet (')) == 4
                  and sum(1 for line in lines if line.startswith('# init: xhci failed, continuing ')
                          and line.endswith(': killed')) == 4
                  and lines.count('asteria') >= 2 and GIVEN_UP in lines,
                  'typed after the first restart; restarted three times, each after a quiet proof; given up')
    context.check('pager.frame-rights', Guest.FRAME_RIGHTS_LINE in Guest.channel_lines(context.console.lines))
    Guest.record(context, context.console.lines)
    path = context.artifact('XhciEvidence.bin')
    problem = Evidence.save(context.qmp, context.console.lines, path)
    context.require('xhci.evidence', problem is None, problem or '')
    record = Amd64Evidence.schema(context.target.root).decode(path.read_bytes())
    injection.check(record)
    context.check('xhci.publication', 'ASTERIA:XHCI:PUBLISHED' in context.console.lines,
                  'PCI class 0C/03/30 controller published')
    if getattr(context.case, 'trust', ()):
        # The trusted-driver profile (Drivers.Trusted): no IOMMU translates.
        context.check('xhci.dma-trusted', record.Iommu.Enabled == 0 and record.IommuBackend.Kind == 0,
                      'both incarnations mastered the bus without an IOMMU, under the BOOT.CFG trust entry')
    else:
        context.check('xhci.dma-confined', record.Iommu.Enabled == 1 and record.Iommu.DomainsCreated >= 2
                      and record.Iommu.DomainsReclaimed >= 2 and record.Iommu.Faults == 0,
                      'both incarnations used IOMMU mappings without DMA faults')
    context.check('xhci.drained', record.DeviceControl.Quarantines >= 2,
                  'both killed assignments were quarantined')
    context.check('xhci.interrupts-reclaimed', record.Interrupts.Live == 0,
                  'MSI authority collected after both instances ended')
    screen = context.artifact('Screen.ppm')
    context.qmp.screendump(screen, format='ppm')
    context.check_problems('xhci.display', Display.display_problems(
        screen.read_bytes(), Guest.root_service_lines(context.console.lines), context.target.root))
