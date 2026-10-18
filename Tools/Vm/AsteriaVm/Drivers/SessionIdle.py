# SPDX-License-Identifier: Apache-2.0
"""Verify the hardware demo stays alive while idle."""
import time
from AsteriaVm import Display, Guest
from AsteriaVm.Drivers import SessionInput, SessionStress, SessionProbe

IDLE_SECONDS = 65


def run(context):
    prefix = SessionInput.PREFIX
    index, _ = context.wait_for_line(prefix + 'fence ready', 120)
    pointer, _ = context.wait_for_line('USER:# pointer:', 30, since=index + 1)
    position = list(Display.pointer_position(Guest.root_service_lines(context.console.lines[:pointer + 1]), 1024, 768))
    cursor = Display.load_cursor(context.target.root)

    def key(code, down):
        context.qmp.execute('input-send-event', {'events': [dict(
            type='key', data=dict(down=down, key=dict(type='qcode', data=code)))]})
        time.sleep(0.05)

    def tap(code):
        key(code, True)
        key(code, False)

    def picture(name):
        path = context.artifact(name + '.ppm')
        context.qmp.screendump(path, format='ppm')
        width, height, rgb = Display.parse_ppm(path.read_bytes())
        return rgb, SessionStress.cursor_position(rgb, width, height, cursor)

    def move(x, y):
        while position != [x, y]:
            dx, dy = (max(-64, min(64, target - old)) for target, old in zip((x, y), position))
            context.qmp.execute('input-send-event', {'events': [
                dict(type='rel', data=dict(axis='x', value=dx)),
                dict(type='rel', data=dict(axis='y', value=dy))]})
            position[0] += dx
            position[1] += dy
            end = time.monotonic() + 15
            while time.monotonic() < end:
                _, actual = picture('PointerArrival')
                if actual == tuple(position):
                    break
                time.sleep(0.02)
            else:
                raise RuntimeError('idle pointer did not arrive: %r vs %r' % (actual, position))

    start, _ = context.wait_for_line(prefix + 'phase move', 30, since=pointer + 1)
    move(380, 190)
    first, _ = picture('idle-before')
    end = time.monotonic() + IDLE_SECONDS
    while time.monotonic() < end:
        time.sleep(min(30, max(0, end - time.monotonic())))
    move(430, 230)
    second, actual = picture('idle-after')
    context.check('session.idle.cursor', first != second and actual == (430, 230))
    # This case checks lifetime. The existing one-CPU SessionInput case keeps
    # its independent click/key ordering checks; SMP stress covers geometry.
    before = SessionInput.windows(SessionStress.complete(Guest.root_service_lines(context.console.lines)))
    focused = next(slot for slot, window in before.items() if window[4] == 1)
    key('alt', True)
    tap('tab')
    key('alt', False)
    end = time.monotonic() + 15
    current = before
    while time.monotonic() < end:
        current = SessionInput.windows(SessionStress.complete(Guest.root_service_lines(context.console.lines)))
        if current[focused][4] != 1:
            break
        time.sleep(0.02)
    context.check('session.idle.compositor', len(current) == 2 and current[focused][4] != 1
                  and all(not w[-1] & 1 for w in current.values()), str(current))
    tap('f12')
    since = start + 1
    for phase in ('resize', 'click-key', 'key-click', 'alt-tab'):
        since, _ = context.wait_for_line(prefix + 'phase ' + phase, 60, since=since)
        tap('f12')
        since += 1
    since, _ = context.wait_for_line(prefix + 'phase close', 120, since=since)
    key('alt', True)
    tap('f4')
    key('alt', False)
    tap('f12')
    context.wait_for_line(prefix + 'torn down', 90, since=since)
    context.wait_for_line('USER:# root service: done', 90, since=since)
    Guest.wait(context)
    SessionProbe._record(context, 'IdleTeardownEvidence')
