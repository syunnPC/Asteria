# SPDX-License-Identifier: Apache-2.0
"""desktop session: resize and stalled-seat isolation."""
import json
import time
from AsteriaVm import Display, Guest, Session
from AsteriaVm.Drivers.SessionInput import PREFIX, events, windows
from AsteriaVm.Drivers.SessionProbe import _record, _teardown_record, _retained_pairs

CYCLES = 24
BURST_POINTS = ((190, 60), (270, 90), (210, 45), (200, 70), (280, 100),
                (190, 70), (270, 50), (200, 100), (280, 60), (210, 90))


# Shared observers also fence the native-output input suite.
cursor_position = Display.cursor_position
complete = Session.complete_transcript


def run(context):
    position = [0, 0]
    injections = []
    since = 0
    session_width, session_height = 0, 0
    model = Session.SessionModel(context.target.root)
    cursor = Display.load_cursor(context.target.root)

    def lines():
        return Guest.root_service_lines(context.console.lines)

    def scene():
        return windows(complete(lines()))

    def wait_scene(predicate, seconds=15):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            current = scene()
            if predicate(current):
                return current
            time.sleep(0.02)
        raise RuntimeError('session stress scene timeout: %r' % scene())

    def send(batch):
        context.qmp.execute('input-send-event', {'events': batch})
        injections.append(batch)
        # Preserve diagnostics even when the driver times out before teardown.
        with context.artifact('SessionStressInjection.jsonl').open('a') as journal:
            journal.write(json.dumps(dict(time=time.monotonic(), events=batch)) + '\n')

    def wait_pointer(expected, seconds=15):
        end = time.monotonic() + seconds
        actual = None
        path = context.artifact('PointerArrival.ppm')
        while time.monotonic() < end:
            context.qmp.screendump(path, format='ppm')
            width, height, rgb = Display.parse_ppm(path.read_bytes())
            actual = cursor_position(rgb, width, height, cursor)
            if actual == tuple(expected):
                return
            time.sleep(0.02)
        raise RuntimeError('session stress pointer timeout: expected %r, actual %r'
                           % (expected, actual))

    def finish_burst():
        # Rapid virtual-device injection can combine or discard motion. Two
        # consecutive exact fresh moves prove progress and establish the next
        # click's physical origin, independently of the assumed burst path.
        nominal = tuple(position)
        end = time.monotonic() + 15
        confirmed = 0
        path = context.artifact('BurstArrival.ppm')

        def observed():
            context.qmp.screendump(path, format='ppm')
            width, height, rgb = Display.parse_ppm(path.read_bytes())
            return cursor_position(rgb, width, height, cursor)

        while time.monotonic() < end:
            before = observed()
            if before is None:
                time.sleep(0.02)
                continue
            send([dict(type='rel', data=dict(axis='x', value=1)),
                  dict(type='rel', data=dict(axis='y', value=0))])
            probe_end = min(end, time.monotonic() + 1)
            after = before
            while time.monotonic() < probe_end and after == before:
                time.sleep(0.02)
                after = observed()
            with context.artifact('SessionStressBurstProgress.jsonl').open('a') as journal:
                journal.write(json.dumps(dict(nominal=nominal, before=before, after=after)) + '\n')
            confirmed = confirmed + 1 if after == (before[0] + 1, before[1]) else 0
            if confirmed == 2:
                # A probe sent while earlier burst packets were still arriving
                # can itself arrive after these two acknowledgements. Drain
                # that tail before using the observed origin for a click.
                quiet_since = time.monotonic()
                while time.monotonic() < end:
                    time.sleep(0.02)
                    current = observed()
                    if current != after:
                        after = current
                        quiet_since = time.monotonic()
                    if after is not None and time.monotonic() - quiet_since >= 1:
                        position[:] = after
                        return
                break
            time.sleep(0.1)
        raise RuntimeError('session stress cursor made no two consecutive fresh moves')

    def edge(key, down):
        return dict(type='key', data=dict(down=down, key=dict(type='qcode', data=key)))

    def tap(key):
        send([edge(key, True)])
        time.sleep(0.05)
        send([edge(key, False)])
        time.sleep(0.05)

    def shortcut(key):
        send([edge('alt', True)])
        tap(key)
        send([edge('alt', False)])
        time.sleep(0.05)

    def move(x, y, delay=0.03):
        while position != [x, y]:
            dx, dy = (max(-64, min(64, target - old)) for target, old in zip((x, y), position))
            send([dict(type='rel', data=dict(axis='x', value=dx)),
                  dict(type='rel', data=dict(axis='y', value=dy))])
            position[0] += dx
            position[1] += dy
            time.sleep(delay)
            if delay >= 0.01:
                # A slow SMP VM can accumulate/clamp multiple PS/2 deltas.
                # Observe each positioning step before assuming its arrival.
                wait_pointer(position)

    def button(down):
        send([dict(type='btn', data=dict(down=down, button='left'))])
        time.sleep(0.06)

    def click(x, y, pressed=None):
        move(x, y)
        button(True)
        try:
            if pressed is not None:
                wait_scene(pressed)
        finally:
            button(False)

    def maximize(slot, value):
        old = scene()[slot]
        button_rect = model.maximize_button_rect(tuple(old[:4]))
        click(button_rect[0] + button_rect[2] // 2, button_rect[1] + button_rect[3] // 2,
              lambda s: slot in s and s[slot][-1] & 8)
        return wait_scene(lambda s: slot in s and bool(s[slot][-1] & 2) == value
                          and (s[slot][6:8] == [session_width - model.constants['FrameExtraWidth'], session_height - model.constants['FrameExtraHeight']] if value else
                               s[slot][6:8] == [s[slot][2] - model.constants['FrameExtraWidth'], s[slot][3] - model.constants['FrameExtraHeight']]))

    def drag(slot, x, y):
        old = scene()[slot]
        move(old[0] + 12, old[1] + 10)
        start = len(lines())
        button(True)
        try:
            # Title grabs have no pressed decoration. PointerEnter with a
            # negative content y proves the press arrived at the title before
            # a slow virtual mouse can combine it with the first drag motion.
            end = time.monotonic() + 15
            while time.monotonic() < end:
                if any(e[0] == (1 if slot == 0 else 2) and e[1] == 12 and e[3] >> 63
                       for e in events(lines()[start:])):
                    break
                time.sleep(0.02)
            else:
                raise RuntimeError('session stress title press timeout: %r' % scene())
            move(x + 12, y + 10)
        finally:
            button(False)
        return wait_scene(lambda s: slot in s and s[slot][:2] == [x, y])

    def pointer_picture(name, x, y):
        move(x, y, 0.01)
        time.sleep(0.15)
        path = context.artifact(name + '.ppm')
        context.qmp.screendump(path, format='ppm')
        width, height, rgb = Display.parse_ppm(path.read_bytes())
        # Both positions are outside the sub-screen session on unchanged
        # broker background: pixel-exact cursor proof independent of clients.
        _, background = Display.console_colors(context.target.root)
        expected = Display.draw_cursor(bytes(background) * width * height, width, height,
                                       (x, y), context.target.root)
        cw, ch, hot, _ = Display.load_cursor(context.target.root)
        different = 0
        for row in range(y - hot[1], y - hot[1] + ch):
            start = (row * width + x - hot[0]) * 3
            different += rgb[start:start + cw * 3] != expected[start:start + cw * 3]
        context.check('session.stress.cursor.' + name, different == 0,
                      '%d cursor rows differ at %r' % (different, (x, y)))
        return rgb

    def fence(phase):
        nonlocal since
        index, _ = context.wait_for_line(PREFIX + 'fence ' + phase, 120, since=since)
        pointer, _ = context.wait_for_line('USER:# pointer:', 30, since=index + 1)
        since = pointer + 1
        logged = Guest.root_service_lines(context.console.lines[:since])
        position[:] = Display.pointer_position(logged, 1024, 768)
        context.stop()
        try:
            path = context.artifact(phase + '.ppm')
            context.qmp.screendump(path, format='ppm')
            width, height, rgb = Display.parse_ppm(path.read_bytes())
            expected = Session.transcript_output(context.target.root, width, height,
                                                 complete(logged), tuple(position))
            different = sum(rgb[i:i + 3] != expected[i:i + 3] for i in range(0, len(rgb), 3))
            context.check('session.stress.scene.' + phase, different == 0, '%d pixels differ' % different)
        finally:
            context.qmp.cont()
        return windows(complete(logged))

    current = fence('ready')
    header = next(line for line in complete(lines()) if line.startswith('# session: begin '))
    session_width, session_height = map(int, header.split()[-2:])
    context.check('session.stress.independent-clients', set(current) == {0, 4}, str(current))
    context.wait_for_line(PREFIX + 'phase cycles', 30, since=since)
    start_presented = sum(line.startswith('# session: presented ') for line in lines())
    for cycle in range(CYCLES):
        maximize(0, True)
        maximize(0, False)
        clipped = drag(0, session_width - scene()[0][2] + 16, 35)
        context.check('session.stress.clipped.%02d' % cycle,
                      clipped[0][0] + clipped[0][2] > session_width, str(clipped))
        maximize(0, True)
        # Touch the client and the area beyond the session rapidly, including
        # the old clipped frame, while the test's Present is delayed 200 ms.
        # Keep the rapid path in a small region: a slow virtual mouse can
        # combine steps, so a 250-pixel excursion can lose displacement.
        # This still crosses the clipped frame and the session's right edge.
        for x, y in BURST_POINTS:
            move(x + session_width - 256, y, 0.005)
        finish_burst()
        maximize(0, False)
        drag(0, 20, 4)
        current = scene()
        context.check('session.stress.healthy.%02d' % cycle,
                      len(current) == 2 and all(not w[-1] & 1 for w in current.values()), str(current))
        if cycle in (0, CYCLES - 1):
            first = pointer_picture('cycles-%02d-a' % cycle, 380, 190)
            second = pointer_picture('cycles-%02d-b' % cycle, 430, 230)
            context.check('session.stress.pointer-diff.%02d' % cycle, first != second)
    context.check('session.stress.presents',
                  sum(line.startswith('# session: presented ') for line in lines()) - start_presented >= 4 * CYCLES)
    context.check('session.stress.no-unexpected-not-responding',
                  all(not int(line.split()[-1]) & 1 for line in lines()
                      if line.startswith('# session: window ')))
    # Focus the second independent client before the root suspends its reads.
    click(70, 86, lambda s: s[4][4] == 1)
    wait_scene(lambda s: s[4][4] == 1)
    tap('f12')
    fence('cycles')
    context.wait_for_line(PREFIX + 'phase stalled', 30, since=since)
    for _ in range(40):
        tap('q')
    wait_scene(lambda s: s[4][-1] & 1)
    shortcut('tab')
    current = wait_scene(lambda s: s[0][4] == 1)
    context.check('session.stress.stalled-isolation', current[4][-1] & 1 and not current[0][-1] & 1)
    maximize(0, True)
    maximize(0, False)
    first = pointer_picture('stalled-a', 380, 190)
    second = pointer_picture('stalled-b', 430, 230)
    context.check('session.stress.stalled-pointer-diff', first != second)
    tap('f12')
    fence('stalled')
    context.wait_for_line(PREFIX + 'phase recovered', 30, since=since)
    wait_scene(lambda s: all(not w[-1] & 1 for w in s.values()))
    shortcut('tab')
    wait_scene(lambda s: s[4][4] == 1)
    maximize(4, True)
    maximize(4, False)
    tap('f12')
    fence('recovered')
    context.wait_for_line(PREFIX + 'phase close', 30, since=since)
    # CloseAsked is actually drained and destroys its surface. Repeated clicks
    # at the old button cannot strand credit or stop the surviving window.
    old = scene()[4]
    close = model.close_button_rect(tuple(old[:4]))
    for index in range(5):
        click(close[0] + close[2] // 2, close[1] + close[3] // 2,
              (lambda s: 4 in s and s[4][-1] & 4) if index == 0 else None)
    wait_scene(lambda s: set(s) == {0})
    tap('f12')
    current = fence('close')
    context.check('session.stress.close-drained', set(current) == {0} and not current[0][-1] & 1)
    transcript = events(lines())
    context.check('session.stress.close-asked', any(e[0] == 2 and e[1] == 11 for e in transcript))
    context.check('session.stress.no-seat-loss', all(line == '# session: input dropped 0'
                  for line in lines() if line.startswith('# session: input dropped ')))
    context.wait_for_line(PREFIX + 'phase fault', 30, since=since)
    context.wait_for_line(PREFIX + 'client fault dropped', 30, since=since)
    current = fence('faulted')
    context.check('session.stress.fault-dropped', not current, str(current))
    first = pointer_picture('faulted-a', 380, 190)
    second = pointer_picture('faulted-b', 430, 230)
    context.check('session.stress.faulted-pointer-diff', first != second)
    context.wait_for_line(PREFIX + 'compositor fault detached', 30, since=since)
    context.wait_for_line(PREFIX + 'phase compositor-dead', 30, since=since)
    # Exceed the dead compositor's 64-entry seat key queue. Revoked credits
    # must retire its focus, rather than obstructing mixed PS/2 mouse batches.
    for _ in range(48):
        tap('q')
    first = pointer_picture('compositor-dead-a', 380, 190)
    second = pointer_picture('compositor-dead-b', 430, 230)
    context.check('session.stress.compositor-dead-pointer-diff', first != second)
    context.artifact('SessionStressInjection.json').write_text(json.dumps(injections, indent=2))
    context.wait_for_line(PREFIX + 'torn down', 90, since=since)
    context.wait_for_line('USER:# root service: done', 90, since=since)
    Guest.wait(context)
    pairs = _retained_pairs(context, ('firmware:display_elf', 'firmware-test:compositor_elf',
                                    'firmware-test:pattern_elf', 'firmware-test:input_seat_elf'))
    record = _teardown_record(context, 'StressTeardownEvidence', pairs)
    m = record.ObjectManagement
    context.check('session.stress.teardown-counts',
                  (m.FramesCreated - m.FramesReclaimed, m.MemoryObjectsCreated - m.MemoryObjectsReclaimed) == (pairs, pairs),
                  '%r vs %d' % ((m.FramesCreated - m.FramesReclaimed,
                                 m.MemoryObjectsCreated - m.MemoryObjectsReclaimed), pairs))
