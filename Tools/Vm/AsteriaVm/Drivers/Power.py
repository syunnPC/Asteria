# SPDX-License-Identifier: Apache-2.0
"""PowerOff.md: product keys, guest flush and terminal platform transitions.

No host powerdown/reset QMP command can satisfy the test: only keyboard input
is injected. Off requires process status 0; reboot requires a second kernel
entry and product init readiness in the same QEMU process.
"""
from __future__ import annotations
import struct
import subprocess
import time
from AsteriaVm import BootFiles, BootEntropy, I8042Evidence, Inventory, AmlFacts, Ps2
from AsteriaVm.Qmp import QmpError

def chord(context, key, allow_exit=False):
    for code, down in [('ctrl', True), ('alt', True), (key, True),
                       (key, False), ('alt', False), ('ctrl', False)]:
        event = dict(type='key', data=dict(down=down, key=dict(type='qcode', data=code)))
        args = {'events': [event]}
        if Ps2.usb_console(context.case):
            args['device'] = 'usbconsole'
        try:
            context.qmp.execute('input-send-event', args)
        except QmpError:
            # The guest can complete S5 before the key's release is sent.
            # The caller still requires flush evidence and process status 0.
            if allow_exit and (code == key or not down):
                try:
                    if context.process.wait(timeout=context.seconds(1)) == 0:
                        return
                except subprocess.TimeoutExpired:
                    pass
            raise
        time.sleep(Ps2.edge_pause(context))

def fat_clean(path):
    with path.open('rb') as disk:
        boot = disk.read(512)
        reserved = struct.unpack_from('<H', boot, 14)[0]
        disk.seek(reserved * 512 + 2)
        entry = struct.unpack('<H', disk.read(2))[0]
    return bool(entry & 0x8000)

def run(context):
    ready = lambda line: line.startswith('USER:# init: ready:')
    first, _ = context.wait_for_line(ready, 120.0)
    BootFiles.record(context)
    BootEntropy.record(context)
    I8042Evidence.record(context)
    Inventory.record(context)
    AmlFacts.record(context)
    chord(context, 'f9')
    staged, _ = context.wait_for_line(lambda line: line == 'USER:# power: test FAT session staged (uncommitted)',
                                     30.0, since=first)
    disk = context.run_directory / 'Disk.raw'
    context.require('power.dirty-before', not fat_clean(disk), 'FAT16 session marked dirty before the request')
    chord(context, 'f11' if context.case.power == 'reboot' else 'f10', allow_exit=context.case.power == 'off')
    # An exited process can race the serial reader; wait on the console without
    # its ordinary abort-on-exit predicate for terminal evidence.
    context.console.wait_for(lambda line: line == 'USER:# power: devices quiet; transition',
                             context.seconds(30), since=staged)
    lines = context.console.lines
    clean = lines.index('USER:# power: filesystems clean; parking devices and CPUs', staged)
    quiet = lines.index('USER:# power: devices quiet; transition', clean)
    context.check('power.sequence', staged < clean < quiet, 'staged -> flush -> device quiesce -> kernel')
    if context.case.power == 'off':
        status = context.process.wait(timeout=context.seconds(30))
        context.check('power.off-exit', status == 0, 'QEMU exited with status %d' % status)
    else:
        second, _ = context.wait_for_line(ready, 120.0, since=quiet + 1)
        context.check('power.reboot-started', second > quiet and context.process.poll() is None,
                      'next product boot reached init in the same QEMU process')
        context.stop()
    context.check('power.fat-clean-after', fat_clean(disk), 'FAT16 clean-shutdown bit set after durable flush')
