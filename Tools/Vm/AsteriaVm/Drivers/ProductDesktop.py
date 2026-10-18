# SPDX-License-Identifier: Apache-2.0
"""product desktop, real broker seats and console fallback."""
import json
import re
import time
from AsteriaVm import Display, Guest, Ps2


def run(context):
    if context.case.session_compositor_fault:
        first,_ = context.wait_for_line('USER:# session: desktop ',180)
        position = recovery_input(context, first + 1)
        second,_ = context.wait_for_line('USER:# session: desktop ',120,since=first+1)
        position = recovery_input(context, second + 1, position)
        third,_ = context.wait_for_line('USER:# session: desktop ',120,since=second+1)
        recovery_input(context, third + 1, position)
        context.wait_for_line('USER:# session: fallback compositor',120,since=third+1)
        context.check('desktop.two-restarts',sum(line.startswith('USER:# session: desktop ') for line in context.console.lines) == 3)
        # The default five-second period kills at the third miss. A late
        # poll may coalesce earlier reports; require three actual kill ages.
        ages = [int(match.group(1)) for line in context.console.lines
                if (match := re.fullmatch(r'USER:# watchdog: session-compositor unresponsive for (\d+) ms', line))]
        context.check('desktop.three-deaths',sum(age >= 15000 for age in ages) == 3)
        Ps2.inject_terminal(context)
        context.wait_for_line('USER:asteria',30)
        context.check('desktop.recovery.console',True)
        return
    if context.case.power:
        from AsteriaVm.Drivers import Power
        context.wait_for_line('USER:# session: desktop ',180)
        Power.run(context)
        if context.case.power == 'reboot':
            context.qmp.cont()
            context.wait_for_line('USER:# session: desktop ',180,since=len(context.console.lines)-1)
        healthy(context, boots=2 if context.case.power == 'reboot' else 1)
        return
    if context.case.session_face_withheld:
        context.wait_for_line('USER:# session: fallback session-face', 120)
        Ps2.inject_terminal(context)
        context.wait_for_line('USER:asteria', 30)
        context.check('desktop.fallback.once', sum(line.startswith('USER:# session: fallback')
                      for line in context.console.lines) == 1)
        chord(context)
        context.wait_for_line('USER:# session: secure attention', 30)
        context.check('desktop.fallback.input', True, 'console echo and root secure attention')
        healthy(context, boots=0, fallback=True)
        return
    _, line = context.wait_for_line('USER:# session: desktop ', 180)
    match = re.fullmatch(r'USER:# session: desktop (\d+)x(\d+) output-pages=(\d+) client-pages=(\d+)', line)
    context.require('desktop.ready', match is not None, line)
    width,height,pages,client = map(int,match.groups())
    context.check('desktop.native', (width,height) == context.case.expected_display)
    context.check('desktop.full-mode-pools', pages == (width*height*4+4095)//4096 and client == 2*pages)
    context.wait_for_line('USER:# session: font Vegur-Bold Vegur-Regular', 30)
    context.check('desktop.vegur', True)
    events = []
    def send(batch):
        arguments = {'events':batch}
        if context.case.usb_console: arguments['device'] = 'usbconsole'
        context.qmp.execute('input-send-event',arguments)
        events.append(arguments)
        time.sleep(0.25 if context.case.isa == 'Ia64' else 0.06)
    # Root-owned window starts at (32,32); its content starts below the title.
    # Relative reports drive the broker pointer, including the USB mouse.
    position = [width//2,height//2]
    for destination in ((60,80),(100,100)):
        while position != list(destination):
            delta = [max(-60,min(60,wanted-old)) for wanted,old in zip(destination,position)]
            send([dict(type='rel',data=dict(axis='x',value=delta[0])),
                  dict(type='rel',data=dict(axis='y',value=delta[1]))])
            position = [old+step for old,step in zip(position,delta)]
        context.wait_for_line('USER:# session-client: event 14 ',30)
        cursor(context,width,height,destination)
        send([dict(type='btn',data=dict(down=True,button='left'))])
        send([dict(type='btn',data=dict(down=False,button='left'))])
    context.wait_for_line(lambda line: line.startswith('USER:# session-client: event 15 ') and line.split()[-2] == '1',30)
    context.check('desktop.mouse.click', True, 'real broker pointer buttons reach content')
    start = len(context.console.lines)
    for key in ('a','ret'):
        send([edge(key,True)])
        send([edge(key,False)])
    context.wait_for_line('USER:# session-client: event 17 1 1 4 ',30,since=start)
    context.check('desktop.keyboard.focus', True, 'focused window receives HID usage 4')
    if context.case.usb_console or context.case.usb_host:
        context.wait_for_line(lambda line: line.startswith('USER:# usb: ')
            and 'report length 8: 00 00 04 ' in line,30,since=start)
        context.wait_for_line(lambda line: line.startswith('USER:# usb: ')
            and 'report length 4:' in line,30)
        context.check('desktop.usb.reports',True,'keyboard and mouse accepted by the real USB core')
    time.sleep(1)
    context.qmp.screendump(context.artifact('Desktop.ppm'),format='ppm')
    w,h,_ = Display.parse_ppm(context.artifact('Desktop.ppm').read_bytes())
    context.check('desktop.scanout', (w,h) == (width,height))
    context.artifact('Input.json').write_text(json.dumps(events,indent=2))
    chord(context)
    context.wait_for_line('USER:# session: trusted console',30)
    context.check('desktop.secure-attention', True)
    start = len(context.console.lines)
    chord(context)
    context.wait_for_line('USER:# session: secure attention',30,since=start)
    healthy(context)


def recovery_input(context, since, position=None):
    """Exercise both ordered streams before and after every replacement."""
    width, height = context.case.expected_display
    position = list(position or (width // 2, height // 2))
    destination = (100, 100) if position != [100, 100] else (101, 101)
    def send(events):
        context.qmp.execute('input-send-event', {'events': events})
        time.sleep(0.25 if context.case.isa == 'Ia64' else 0.06)
    while position != list(destination):
        delta = [max(-60, min(60, wanted-old)) for wanted, old in zip(destination, position)]
        send([dict(type='rel', data=dict(axis='x', value=delta[0])),
              dict(type='rel', data=dict(axis='y', value=delta[1]))])
        position = [old+step for old, step in zip(position, delta)]
    context.wait_for_line('USER:# session-client: event 14 ', 30, since=since)
    # Desktop session suppresses clicks just after a system raise. A replacement needs only one
    # motion report, unlike the initial move.
    time.sleep(0.5)
    send([dict(type='btn', data=dict(down=True, button='left'))])
    send([dict(type='btn', data=dict(down=False, button='left'))])
    context.wait_for_line(lambda line: line.startswith('USER:# session-client: event 15 ')
                          and line.split()[-2] == '1', 30, since=since)
    send([edge('a', True)]); send([edge('a', False)])
    context.wait_for_line('USER:# session-client: event 17 1 1 4 ', 30, since=since)
    context.check('desktop.recovery.input.%d' % since, True,
                  'real motion, button and key reach this compositor incarnation')
    return position


def healthy(context, boots=1, fallback=False):
    lines = context.console.lines
    misses = [line for line in lines if '# watchdog:' in line or '# startup:' in line]
    if getattr(getattr(context, 'case', None), 'whole_machine_pause', ''):
        # The test deliberately suspends all service owners. One outstanding
        # Query can expire on resumption; require resumed input and no restart,
        # while retaining and bounding those diagnostics in the case result.
        from AsteriaVerify import Layout
        limits = Layout.constants((context.target.root / 'Source/Abi/Watchdog.hpp').read_text())
        maximum_age = int(context.pause_seconds * 1000) + 2 * limits['WatchdogCheckMicroseconds'] // 1000
        ages = [re.fullmatch(r'USER:# watchdog: [^ ]+ unresponsive for (\d+) ms', line) for line in misses]
        bounded = len(ages) <= limits['WatchdogServiceCount'] and all(
            match is not None and int(match[1]) <= maximum_age for match in ages)
        context.check('pause.service-query-diagnostics', bounded, '\n'.join(misses))
    else:
        context.check('desktop.healthy.watchdog', not misses, '\n'.join(misses))
    context.check('desktop.healthy.no-restarts',
                  sum(line.startswith('USER:# session: desktop ') for line in lines) == boots)
    context.check('desktop.healthy.fallback',
                  sum(line.startswith('USER:# session: fallback ') for line in lines) == int(fallback))


def edge(key,down):
    return dict(type='key',data=dict(down=down,key=dict(type='qcode',data=key)))


def chord(context):
    arguments = {'events':[edge('ctrl',True),edge('alt',True),edge('delete',True)]}
    if context.case.usb_console: arguments['device'] = 'usbconsole'
    context.qmp.execute('input-send-event',arguments)
    time.sleep(0.2)
    arguments['events'] = [edge('delete',False),edge('alt',False),edge('ctrl',False)]
    context.qmp.execute('input-send-event',arguments)
    time.sleep(0.4)


def cursor(context,width,height,position):
    cw,ch,hot,pixels = Display.load_cursor(context.target.root)
    path = context.artifact('Mouse-%dx%d.ppm' % position)
    deadline = time.monotonic()+context.seconds(10)
    while True:
        context.qmp.screendump(path,format='ppm')
        w,h,rgb = Display.parse_ppm(path.read_bytes())
        mismatch = []
        for y in range(ch):
            for x in range(cw):
                pixel = pixels[y*cw+x]
                if pixel>>24 != 255: continue
                at = ((position[1]+y-hot[1])*width+position[0]+x-hot[0])*3
                expected = bytes((pixel>>16&255,pixel>>8&255,pixel&255))
                if rgb[at:at+3] != expected: mismatch.append((x,y))
        if not mismatch or time.monotonic() >= deadline: break
        time.sleep(0.1)
    context.check('desktop.cursor.%dx%d' % position,(w,h)==(width,height) and not mismatch,
                  '%d opaque cursor pixels differ' % len(mismatch))
