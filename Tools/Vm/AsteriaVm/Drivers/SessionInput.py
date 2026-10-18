# SPDX-License-Identifier: Apache-2.0
"""desktop session: hardware input, two-way ordering and bounded clients."""
import json
import time
from AsteriaVm import Display, Guest, Session
from AsteriaVerify import Layout
from AsteriaVm.Drivers.SessionProbe import _record, _teardown_record, _retained_pairs

PREFIX = 'USER:# session-input: '


def windows(lines):
    result = {}
    for line in lines:
        if line.startswith('# session: begin '):
            result = {}
        elif line.startswith('# session: window '):
            values = list(map(int, line.split()[3:]))
            result[values[0]] = values[1:]
    return result


def events(lines):
    return [tuple(map(int, line[len('# window-event: '):].split()))
            for line in lines if line.startswith('# window-event: ')]


def maxima(context):
    from AsteriaVerify import Layout
    from AsteriaVm import Amd64Layout
    from AsteriaVm.Drivers import Amd64Default
    from AsteriaVm.Observers import Amd64Paging
    from AsteriaVm.Drivers.SessionProbe import _ia64_ledgers
    constants = Layout.constants((context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text())
    plan = Layout.constants((context.target.root / 'Source/User/Session/SessionFrame.hpp').read_text()
                            + (context.target.root / 'Source/User/Session/AddressPlan.hpp').read_text())
    base = constants[('Ia64' if context.case.isa == 'Ia64' else '') + 'ComponentCompositorAddress']
    ranges = [base + slot * plan['AddressClientSpan'] + pool * plan['AddressPoolSpan']
              for slot in range(4) for pool in range(2)] + [base + plan['AddressOutputOffset']]
    previous = None
    counts = []
    for phase in ('first', 'second'):
        context.wait_for_line(PREFIX + 'maxima ' + phase + ' mapped', 180)
        context.stop()
        try:
            if context.case.isa == 'Amd64':
                record = _record(context, 'Maxima-' + phase)
                layout = Layout.constants((context.target.root / 'Source/Abi/ObjectCreationLimits.hpp').read_text()
                    + (context.target.root / 'Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp').read_text())
                c = Amd64Layout.headers(context.target.root)
                observation = Amd64Default.Observation.live(context.qmp)
                roots = [c.pool_page(record.Runtime.PoolPhysicalStart, layout['LargeRootArenaFirstPage']
                          + slot * layout['LargeRootArenaPages']) for slot in range(layout['LargePageTableArenaMaximumCount'])]
                holders = [root for root in Amd64Paging.candidate_roots(observation.walker, roots)
                           if observation.walker.walk(root, base)]
                context.require('session.maxima.root.' + phase, len(holders) == 1, str(holders))
                root = holders[0]
                pages = set()
                def visit(table, level):
                    context.require('session.maxima.unique-table.' + phase, table not in pages)
                    pages.add(table)
                    if level > 1:
                        for entry in observation.walker.entries(table):
                            if entry & Amd64Paging.PTE_PRESENT:
                                visit(entry & Amd64Paging.PTE_ADDRESS, level - 1)
                visit(root, 4)
                used = len(pages)
                errors = []
                for start in ranges:
                    for page in range(plan['SessionMaximumPoolPages']):
                        leaf = observation.walker.walk(root, start + page * 4096)
                        if not leaf or not leaf.user or not leaf.writable or leaf.executable or leaf.pat_index:
                            errors.append('invalid maximum leaf %#x' % (start + page * 4096))
                    if observation.walker.walk(root, start + plan['SessionMaximumPoolBytes']):
                        errors.append('mapped fixed gap')
                context.check_problems('session.maxima.all-leaves.' + phase, errors)
                context.check('session.maxima.large-capacity.' + phase, 64 < used <= 256, str(used))
                observation.save(context.artifact('Maxima-' + phase + '.json.gz'))
            else:
                ledgers = _ia64_ledgers(context)
                holders = [records for _, records in ledgers if any(va == base for va, _, _ in records)]
                context.require('session.maxima.ledger.' + phase, len(holders) == 1)
                records = holders[0]; used = len(records)
                context.check('session.maxima.all-records.' + phase,
                    all(any(va == start + frame * plan['SessionMaximumFramePages'] * 4096
                            and pages == plan['SessionMaximumFramePages'] for va, _, pages in records)
                        for start in ranges for frame in range(plan['SessionMaximumPoolFrames'])), str(records))
                context.check('session.maxima.record-capacity.' + phase, used <= 256, str(used))
                context.artifact('Maxima-' + phase + '.json').write_text(json.dumps(ledgers, indent=2))
            counts.append(used)
            if previous is not None:
                context.check('session.maxima.reuse', used == previous, '%d -> %d' % (previous, used))
            previous = used
        finally:
            context.qmp.cont()
    context.artifact('ArenaCapacity.json').write_text(json.dumps({'used': counts}))


def run(context):
    from AsteriaVm.Drivers import SessionCompositor, SessionStress
    if context.case.session_large:
        maxima(context)

    constants = Layout.constants((context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text())
    model = Session.SessionModel(context.target.root)
    content = [constants['ComponentPatternInputMaximumWidth'], constants['ComponentPatternInputHeight']]
    frame = model.frame_around_content(0,0,*content)[2:]
    position = [0, 0]
    injections = []
    cursor_image = Display.load_cursor(context.target.root)
    since = 0

    def send(batch):
        context.qmp.execute('input-send-event', {'events': batch})
        injections.append(batch)

    def edge(key, down):
        return dict(type='key', data=dict(down=down, key=dict(type='qcode', data=key)))

    def tap(key):
        send([edge(key, True)])
        time.sleep(0.05)
        send([edge(key, False)])
        time.sleep(0.05)

    def move(x, y):
        while position != [x, y]:
            dx, dy = (max(-64, min(64, target - old)) for target, old in zip((x, y), position))
            send([dict(type='rel', data=dict(axis='x', value=dx)),
                  dict(type='rel', data=dict(axis='y', value=dy))])
            position[0] += dx
            position[1] += dy
            time.sleep(0.05)
        # A QMP request acknowledges injection, not delivery of a PS/2
        # packet. Establish the physical origin before clicking; this does
        # not observe compositor focus or order the following key for it.
        end = time.monotonic() + 30
        path = context.artifact('PointerArrival.ppm')
        while time.monotonic() < end:
            context.qmp.screendump(path, format='ppm')
            width, height, rgb = Display.parse_ppm(path.read_bytes())
            if Display.cursor_position(rgb, width, height, cursor_image) == tuple(position):
                return
            time.sleep(0.02)
        raise RuntimeError('session input pointer did not arrive at %r' % position)

    def button(down):
        send([dict(type='btn', data=dict(down=down, button='left'))])
        time.sleep(0.05)

    def wait_scene(predicate):
        end = time.monotonic()+context.seconds(15)
        while time.monotonic() < end:
            current = windows(SessionStress.complete(Guest.root_service_lines(context.console.lines)))
            if predicate(current):
                return
            time.sleep(0.02)
        raise RuntimeError('session input geometry timeout: %r' % current)

    def click(x, y, pressed=None):
        move(x, y)
        button(True)
        if pressed is not None:
            wait_scene(pressed)
        button(False)

    def shortcut(key):
        send([edge('alt', True)])
        tap(key)
        send([edge('alt', False)])
        time.sleep(0.05)

    def capture(phase):
        nonlocal since
        index, _ = context.wait_for_line(PREFIX + 'fence ' + phase, 90, since=since)
        fence, _ = context.wait_for_line('USER:# pointer:', 30, since=index + 1)
        since = SessionCompositor.complete_snapshot(context, fence)
        # FrameShown and the broker Query must remain independent of slow
        # diagnostics. A native-size compose
        # can finish after the root's test delay. Fence the observation by a
        # complete diagnostic snapshot matching the actual displayed pixels.
        # Each mismatch resumes the guest so its queued transcript can drain.
        end = time.monotonic() + 90
        while True:
            context.stop()
            path = context.artifact(phase + '.ppm')
            context.qmp.screendump(path, format='ppm')
            width, height, actual = Display.parse_ppm(path.read_bytes())
            observed = Guest.root_service_lines(context.console.lines)
            lines = ([line for line in observed if not line.startswith('# session: ')]
                     + Session.complete_transcript(observed))
            cursor = Display.pointer_position(lines, width, height)
            expected = (Session.transcript_output(context.target.root, width, height, lines, cursor)
                        if Session.complete_transcript(observed) else b'')
            current = windows(lines)
            arranged = True
            if phase.startswith('maximize-') or phase.startswith('restore-'):
                slot = 0 if phase.endswith('first') else 1
                window = current.get(slot)
                arranged = (window is not None
                            and bool(window[-1] & 2) == phase.startswith('maximize-')
                            and window[6:8] == list(model.content_rect(tuple(window[:4]))[2:]))
            if (arranged and actual == expected) or time.monotonic() >= end:
                break
            context.qmp.cont()
            time.sleep(0.05)
        try:
            if context.case.session_large and phase == 'ready':
                mode = next(tuple(map(int, line.split()[-2:])) for line in reversed(lines) if line.startswith('# session: begin '))
                context.check('session.large.native-output', mode == (width, height), '%r vs %r' % (mode, (width, height)))
                if context.case.expected_display:
                    context.check('session.large.requested-mode', (width, height) == context.case.expected_display, '%dx%d' % (width, height))
                if context.case.isa == 'Amd64':
                    from AsteriaVerify import Layout
                    from AsteriaVm import Amd64Layout
                    from AsteriaVm.Drivers import Amd64Default
                    from AsteriaVm.Observers import Amd64Paging
                    record = _record(context, 'NativeRoot')
                    layout = Layout.constants((context.target.root / 'Source/Abi/ObjectCreationLimits.hpp').read_text()
                        + (context.target.root / 'Source/Platform/Amd64/Boot/Amd64RuntimePoolLayout.hpp').read_text())
                    address = Layout.constants((context.target.root / 'Source/Abi/ComponentBootstrap.hpp').read_text()
                        + (context.target.root / 'Source/User/Session/SessionFrame.hpp').read_text()
                        + (context.target.root / 'Source/User/Session/AddressPlan.hpp').read_text())
                    c = Amd64Layout.headers(context.target.root)
                    observation = Amd64Default.Observation.live(context.qmp)
                    output = address['ComponentCompositorAddress'] + address['AddressOutputOffset']
                    roots = [c.pool_page(record.Runtime.PoolPhysicalStart, layout['LargeRootArenaFirstPage']
                        + slot * layout['LargeRootArenaPages']) for slot in range(layout['LargePageTableArenaMaximumCount'])]
                    context.check('session.large.compositor-arena',
                        sum(bool(observation.walker.walk(root, output))
                            for root in Amd64Paging.candidate_roots(observation.walker, roots)) == 1)
                    observation.save(context.artifact('NativeRoot.json.gz'))
            cursor = Display.pointer_position(lines, width, height)
            position[:] = cursor
            expected = Session.transcript_output(context.target.root, width, height, lines, cursor)
            context.artifact(phase + '-expected.ppm').write_bytes(Display.write_ppm(width, height, expected))
            different = sum(actual[at:at + 3] != expected[at:at + 3] for at in range(0, len(actual), 3))
            context.check('session.input.scene.' + phase, not different, '%d pixels differ' % different)
            if phase == 'ready':
                context.check('session.input.ui-font', '# session: font Vegur-Bold Vegur-Regular' in lines)
            return windows(lines), events(lines)
        finally:
            context.qmp.cont()

    scene, transcript = capture('ready')
    restored = {slot: window[:4] for slot, window in scene.items()}
    header = next(line for line in reversed(Guest.root_service_lines(context.console.lines[:since]))
                  if line.startswith('# session: begin '))
    output_width, output_height = map(int, header.split()[-2:])
    context.check('session.input.stale-launch', len(scene) == 2 and scene[0][4] == 1 and scene[1][4] == 2)
    if context.case.session_large:
        proof = Layout.constants((context.target.root / 'Tests/Guest/PatternClient.hpp').read_text())
        content_size = [proof['PatternNativeInitialWidth'], proof['PatternNativeInitialHeight']]
        captions = [line for line in context.console.lines[:since] if line.startswith('USER:# session: title ')]
        context.check('session.large.wide-title', all(window[6:8] == content_size for window in scene.values())
                      and sum(line.endswith(' ' + b'office ffi AV'.hex()) for line in captions) >= 2,
                      'wide native content and complete shaping subtitle in the pixel-exact ready capture')
    phases = ('maximize-first', 'restore-first', 'maximize-second', 'restore-second', 'hung', 'close') if context.case.session_large else (
        'move', 'resize', 'click-key', 'key-click', 'alt-tab', 'hung', 'close')
    for phase in phases:
        start, _ = context.wait_for_line(PREFIX + 'phase ' + phase, 30, since=since)
        if phase.startswith('maximize-') or phase.startswith('restore-'):
            slot = 0 if phase.endswith('first') else 1
            if scene[slot][4] != 1:
                shortcut('tab')
                time.sleep(0.4)  # 300 ms focus-press ignore.
            x, y, w, h = scene[slot][:4]
            # Decorations.cpp: maximize button is second from right.
            click(x + w - 30, y + 10, lambda s: slot in s and s[slot][-1] & 8)
            maximized = phase.startswith('maximize-')
            expected = [0, 0, output_width, output_height] if maximized else restored[slot]
            # F12 acknowledges input, not an outstanding
            # Arrange/Present or an asynchronous diagnostic report. Observe
            # the released button and settled image before the next click
            # chooses coordinates from the previous capture.
            wait_scene(lambda s: slot in s and s[slot][:4] == expected
                       and bool(s[slot][-1] & 2) == maximized and not s[slot][-1] & 8
                       and s[slot][6:8] == [expected[2]-model.constants['FrameExtraWidth'],
                                           expected[3]-model.constants['FrameExtraHeight']])
        elif phase == 'move':
            move(42, 42)
            button(True)
            move(30, 14)
            button(False)
        elif phase == 'resize':
            x, y, w, h = scene[0][:4]
            # Grow the content by eight columns and four rows.
            content = [scene[0][6] + 8, scene[0][7] + 4]
            frame = model.frame_around_content(0, 0, *content)[2:]
            move(x+w-2, y+h-2)
            button(True)
            move(x+w+6, y+h+2)
            button(False)
        elif phase == 'click-key':
            move(70, 86)
            # A successful source publish establishes pointer-before-key,
            # independently of compositor focus and its presented frames.
            send([dict(type='btn', data=dict(down=True, button='left'))])
            context.wait_for_line(PREFIX + 'source pointer-down', 30, since=start + 1)
            send([edge('a', True)])
            time.sleep(0.1)
            send([edge('a', False), dict(type='btn', data=dict(down=False, button='left'))])
        elif phase == 'key-click':
            move(30, 34)
            send([edge('b', True)])
            context.wait_for_line(PREFIX + 'source key-b', 30, since=start + 1)
            send([dict(type='btn', data=dict(down=True, button='left'))])
            time.sleep(0.1)
            send([dict(type='btn', data=dict(down=False, button='left')), edge('b', False)])
        elif phase == 'alt-tab':
            shortcut('tab')
        elif phase == 'hung':
            for _ in range(48):
                tap('q')
            shortcut('tab')
        else:
            shortcut('f4')
        if phase != 'hung':
            tap('f12')
        scene, transcript = capture(phase)
        # Diagnostic snapshots may lag behind their independent FrameShown
        # fence. Use the root's phase boundary, rather than the previous
        # capture's event count, to retain early ordered edges in this phase.
        received = events(Guest.root_service_lines(context.console.lines[start + 1:]))
        if phase == 'move':
            context.check('session.input.move', scene[0][:2] == [20, 4], str(scene))
        if phase.startswith('maximize-') or phase.startswith('restore-'):
            slot = 0 if phase.endswith('first') else 1
            maximized = phase.startswith('maximize-')
            context.check('session.large.' + phase, bool(scene[slot][-1] & 2) == maximized, str(scene))
            context.check('session.large.arrange.' + phase, any(e[1] == 9 and e[2] == slot + 1 for e in received), str(received))
        if phase == 'resize':
            context.check('session.input.resize', scene[0][2:4] == list(frame) and scene[0][6:8] == content, str(scene))
            context.check('session.input.arrange-present', any(e[1] == 9 and e[2] == 1 for e in received))
        if phase in ('click-key', 'key-click'):
            usage = 4 if phase == 'click-key' else 5
            context.check('session.input.order.' + phase, any(e[1:5] == (17, 2, 1, usage) for e in received), str(received))
        if phase == 'alt-tab':
            context.check('session.input.alt-tab', scene[1][4] == 1 and scene[0][4] == 0, str(scene))
            context.check('session.input.shortcut-consumed', not any(e[1] == 17 and e[4] == 0x2b for e in received))
        if phase == 'hung':
            context.check('session.input.hung-shortcut', scene[0][4] == 1 and scene[1][4] == 0, str(scene))
            context.check('session.input.not-responding', all(w[-1] & 1 for w in scene.values()), str(scene))
        if phase == 'close':
            context.check('session.input.close-asked', any(e[1] == 11 and e[2] == 1 for e in received), str(received))
            context.check('session.input.close-destroy', set(scene) == {1}, str(scene))
            context.check('session.input.overflow-counted', any(e[-1] > 0 for e in received), str(received))
    context.artifact('SessionInputInjection.json').write_text(json.dumps(injections, indent=2))
    context.wait_for_line(PREFIX + 'torn down', 90, since=since)
    context.wait_for_line('USER:# root service: done', 90, since=since)
    Guest.wait(context)
    pairs = _retained_pairs(context, ('firmware:display_elf', 'firmware-test:compositor_elf',
                                    'firmware-test:pattern_elf', 'firmware-test:input_seat_elf'))
    record = _teardown_record(context, 'InputTeardownEvidence', pairs)
    management = record.ObjectManagement
    live = (management.FramesCreated - management.FramesReclaimed,
            management.MemoryObjectsCreated - management.MemoryObjectsReclaimed)
    context.check('session.input.teardown-counts', live == (pairs, pairs), '%r vs %d' % (live, pairs))
