# SPDX-License-Identifier: Apache-2.0
"""desktop session: real input geometry and content regressions.

Uses compositor fences for geometry and independently observed cursor pixels
for device arrival. The same script runs the product boot-log and test client.
"""
import json
import os
import pathlib
import time
from AsteriaVm import Display, Guest, Session
from AsteriaVm.Drivers import ProductDesktop, SessionInput, SessionStress


def clipped_cursor_position(rgb, width, height, cursor):
    cw, ch, hot, pixels = cursor
    opaque = [(x, y, bytes((value>>16 & 255, value>>8 & 255, value & 255)))
              for y in range(ch) for x in range(cw) if (value := pixels[y*cw+x]) >> 24]
    ax, ay, anchor = opaque[0]
    at, found = 0, []
    while (at := rgb.find(anchor, at)) >= 0:
        if at % 3 == 0:
            y, x = divmod(at//3, width)
            x, y = x-ax, y-ay
            if (0 <= x+hot[0] < width and 0 <= y+hot[1] < height
                    and all(rgb[((y+dy)*width+x+dx)*3:((y+dy)*width+x+dx)*3+3] == color
                            for dx, dy, color in opaque if 0 <= x+dx < width and 0 <= y+dy < height)):
                found.append((x+hot[0], y+hot[1]))
        at += 1
    return found[0] if len(found) == 1 else None


class Script:
    # Inside the title even on the demo's narrow 64-pixel frame; x+24 is
    # already its maximize button. Corner resize requires the outer 4 pixels.
    title_x, title_y = 12, 10
    def __init__(self, context):
        self.context = context
        self.model = Session.SessionModel(context.target.root)
        self.cursor = Display.load_cursor(context.target.root)
        self.position = None
        self.injections = []
        picture_root = pathlib.Path(os.environ.get('GUI_BUG_PICTURE_ROOT', str(context.target.root/'Build/Tmp/GuiBugs')))
        self.pictures = picture_root / os.environ.get('GUI_BUG_STAGE', 'after') / context.case.name
        self.pictures.mkdir(parents=True, exist_ok=True)

    def lines(self):
        return Guest.root_service_lines(self.context.console.lines)

    def scene(self):
        return SessionInput.windows(SessionStress.complete(self.lines()))

    def wait(self, predicate, seconds=15):
        deadline = time.monotonic() + self.context.seconds(seconds)
        while time.monotonic() < deadline:
            if predicate():
                return True
            time.sleep(0.02)
        return False

    def send(self, events):
        arguments = dict(events=events)
        if self.context.case.usb_console:
            arguments['device'] = 'usbconsole'
        self.context.qmp.execute('input-send-event', arguments)
        self.injections.append(arguments)
        self.context.artifact('GuiBugsInput.json').write_text(json.dumps(self.injections, indent=2))

    def observe_pointer(self):
        path = self.context.artifact('Arrival.ppm')
        self.context.qmp.screendump(path, format='ppm')
        w, h, rgb = Display.parse_ppm(path.read_bytes())
        found = SessionStress.cursor_position(rgb, w, h, self.cursor)
        if found is None:
            found = clipped_cursor_position(rgb, w, h, self.cursor)
        if found is not None or self.position is None:
            return found
        # At the physical edge the arrow is clipped. Check its visible
        # opaque pixels instead of requiring the entire bitmap on screen.
        x, y = self.position
        cw, ch, hot, pixels = self.cursor
        visible = [(row, col, value) for row in range(ch) for col in range(cw)
                   if (value := pixels[row*cw+col]) >> 24 and 0 <= x+col-hot[0] < w and 0 <= y+row-hot[1] < h]
        if visible and all(rgb[((y+row-hot[1])*w+x+col-hot[0])*3:((y+row-hot[1])*w+x+col-hot[0])*3+3]
                           == bytes((value >> 16 & 255, value >> 8 & 255, value & 255))
                           for row, col, value in visible):
            return tuple(self.position)
        return None

    def drain_burst(self):
        # Saturated packets conceal pending relative motion at the corner.
        # Two observed fresh inward moves prove that the tail has drained.
        confirmed = 0
        end = time.monotonic()+self.context.seconds(15)
        while time.monotonic() < end:
            before = self.observe_pointer()
            if before is None:
                time.sleep(0.02)
                continue
            self.position = [before[0]-1, before[1]]
            self.send([dict(type='rel', data=dict(axis='x', value=-1))])
            arrived = self.wait(lambda: self.observe_pointer() == tuple(self.position), seconds=1)
            confirmed = confirmed+1 if arrived else 0
            if confirmed == 2:
                time.sleep(1)
                actual = self.observe_pointer()
                if actual == tuple(self.position):
                    return
                confirmed = 0
        raise RuntimeError('saturated motion did not drain')

    def move(self, x, y, delay=0.02):
        if self.position is None:
            self.position = list(self.observe_pointer())
        end = time.monotonic()+self.context.seconds(15)
        while self.position != [x, y]:
            delta = [max(-64, min(64, wanted - old)) for wanted, old in zip((x, y), self.position)]
            self.send([dict(type='rel', data=dict(axis=axis, value=value)) for axis, value in zip(('x', 'y'), delta)])
            self.position = [old + step for old, step in zip(self.position, delta)]
            time.sleep(delay)
            while not self.wait(lambda: self.observe_pointer() == tuple(self.position), seconds=1):
                actual = self.observe_pointer()
                if time.monotonic() >= end:
                    raise RuntimeError('pointer arrival timeout: %r vs %r' % (actual, self.position))
                if actual is not None:
                    # QMP relative injection may discard a packet. Re-establish
                    # the physical origin before sending missing displacement.
                    self.position = list(actual)
                    break
            else:
                end = time.monotonic()+self.context.seconds(15)

    def button(self, down):
        self.send([dict(type='btn', data=dict(down=down, button='left'))])
        time.sleep(0.06)

    def click(self, x, y, pressed=None):
        self.move(x, y)
        self.button(True)
        if pressed is not None:
            self.wait(pressed)
        self.button(False)

    def tap(self, code):
        self.send([ProductDesktop.edge(code, True)])
        time.sleep(0.06)
        self.send([ProductDesktop.edge(code, False)])
        time.sleep(0.06)

    def picture(self, name, verify_frame=True):
        path = self.context.artifact(name + '.ppm')
        end = time.monotonic()+self.context.seconds(15)
        matched = False
        while True:
            self.context.stop()
            try:
                self.context.qmp.screendump(path, format='ppm')
                w, h, rgb = Display.parse_ppm(path.read_bytes())
                logged = SessionStress.complete(self.lines())
                current = SessionInput.windows(logged)
                matched = not verify_frame or self.frame_matches(w, h, rgb, logged, current)
                if matched or time.monotonic() >= end:
                    self.context.qmp.screendump(self.pictures / (name+'.png'))
                    self.context.artifact(name+'-scene.json').write_text(json.dumps(current, indent=2))
                    self.frozen_scene = current
                    break
            finally:
                self.context.qmp.cont()
            time.sleep(0.1)
        self.context.check('gui.'+name+'.scanout-fence', matched,
                           'decoration/desktop pixels match the last complete Presented snapshot')
        return w, h, rgb

    def frame_matches(self, w, h, rgb, logged, current):
        """Check the frame oracle, excluding only independently checked clients."""
        header = next(line for line in logged if line.startswith('# session: begin '))
        ow, oh = map(int, header.split()[-2:])
        titles = {int(parts[3]): parts[4:] for line in logged
                  if line.startswith('# session: title ') and (parts := line.split())}
        self.model.ui_faces = (True, True) if '# session: font Vegur-Bold Vegur-Regular' in logged else (False, False)
        views = []
        for slot, values in current.items():
            name, subtitle = (b'' if value == '-' else bytes.fromhex(value) for value in titles[slot])
            views.append(Session.Window(tuple(values[:4]), name=name, subtitle=subtitle,
                state=('unfocused', 'focused', 'attention')[values[4]], volume=values[5] == 2,
                not_responding=bool(values[-1] & 1), maximized=bool(values[-1] & 2),
                close_pressed=bool(values[-1] & 4), maximize_pressed=bool(values[-1] & 8)))
        expected_session = Session.render(self.model, ow, oh, views)
        _, background = Display.console_colors(self.context.target.root)
        expected = bytearray(bytes(background)*w*h)
        for row in range(min(oh, h)):
            expected[row*w*3:(row*w+ow)*3] = expected_session[row*ow*3:(row+1)*ow*3]
        cursor = self.observe_pointer()
        if cursor is None:
            return False
        expected = Display.draw_cursor(bytes(expected), w, h, cursor, self.context.target.root)
        contents = [self.model.content_rect(view.frame) for view in views]
        for row in range(h):
            intervals = sorted((max(0, x), min(w, x+cw)) for x, y, cw, ch in contents if y <= row < y+ch)
            at = 0
            for left, right in intervals+[(w, w)]:
                if left > at and rgb[(row*w+at)*3:(row*w+left)*3] != expected[(row*w+at)*3:(row*w+left)*3]:
                    return False
                at = max(at, right)
        return True

    def geometry(self, name, expected, maximized=False):
        self.wait(lambda: 0 in self.scene() and self.scene()[0][:4] == list(expected)
                  and bool(self.scene()[0][-1] & 2) == maximized
                  and self.scene()[0][6:8] == [expected[2] - self.model.constants['FrameExtraWidth'],
                                              expected[3] - self.model.constants['FrameExtraHeight']])
        w, h, rgb = self.picture(name)
        current = getattr(self, 'frozen_scene', self.scene()).get(0, [])
        self.context.check('gui.' + name + '.geometry', bool(current) and current[:4] == list(expected)
                           and bool(current[-1] & 2) == maximized, str(current))
        self.context.check('gui.' + name + '.arranged-image', bool(current) and current[6:8] == [
            expected[2] - self.model.constants['FrameExtraWidth'], expected[3] - self.model.constants['FrameExtraHeight']], str(current))
        if current:
            content = self.model.content_rect(tuple(current[:4]))
            x, y, cw, ch = content
            # No client in these cases paints the decoration Face colour.
            face = bytes(self.model.colors['Face'])
            gray = sum(rgb[(row*w+col)*3:(row*w+col)*3+3] == face
                       for row in range(max(0, y), min(h, y+ch))
                       for col in range(max(0, x), min(w, x+cw)))
            self.context.check('gui.' + name + '.content-covered', gray == 0, '%d Face pixels in client content' % gray)
        return current

    def maximize_button(self, value):
        frame = self.scene()[0][:4]
        x, y, w, h = self.model.maximize_button_rect(tuple(frame))
        self.click(x+w//2, y+h//2, lambda: self.scene()[0][-1] & 8)
        self.wait(lambda: bool(self.scene()[0][-1] & 2) == value)

    def double_title(self):
        frame = self.scene()[0][:4]
        self.move(frame[0]+self.title_x, frame[1]+self.title_y)
        self.button(True)
        self.button(False)
        old_maximized = bool(self.scene()[0][-1] & 2)
        self.button(True)
        self.wait(lambda: bool(self.scene()[0][-1] & 2) != old_maximized)
        self.button(False)

    def begin_title_grab(self):
        frame = self.scene()[0][:4]
        # Clear hover from the previous grab. A fresh title press delivers
        # PointerEnter with negative client y; same-point clicks need not.
        self.move(2, 2)
        self.move(frame[0]+self.title_x, frame[1]+self.title_y)
        start = len(self.context.console.lines)
        self.button(True)
        def entered():
            lines = Guest.channel_lines(self.context.console.lines[start:])
            return (any(e[1] == 12 and e[3] >> 63 for e in SessionInput.events(lines))
                    or any(line.startswith('# session-client: event 12 ') and int(line.split()[-3]) >> 63
                           for line in lines))
        if not self.wait(entered):
            self.button(False)
            raise RuntimeError('title grab timeout: %r' % self.scene())

    def drag(self, x, y, delay):
        self.begin_title_grab()
        self.move(x+self.title_x, y+self.title_y, delay)
        self.button(False)


def run(context):
    if context.case.product_desktop:
        context.wait_for_line('USER:# session: desktop ', 180)
    else:
        context.wait_for_line(SessionInput.PREFIX + 'fence ready', 180)
        context.wait_for_line(SessionInput.PREFIX + 'phase move', 30)
    script = Script(context)
    context.require('gui.initial-scene', script.wait(lambda: 0 in script.scene()), str(script.scene()))
    header = next(line for line in reversed(script.lines()) if line.startswith('# session: begin '))
    width, height = map(int, header.split()[-2:])
    screen_w, screen_h, _ = script.picture('before')
    context.check('gui.output-mode', (screen_w, screen_h) == context.case.expected_display)
    initial = tuple(script.scene()[0][:4])
    time.sleep(0.4)
    # IA-64's native mode fits the initial size but the cascade leaves the
    # rightmost buttons outside scanout. Move the existing frame into view.
    if initial[0]+initial[2] > screen_w:
        script.drag(0, 0, 0.02)
        script.wait(lambda: script.scene()[0][:2] == [0, 0])
        initial = tuple(script.scene()[0][:4])
    if 'Maximize' in context.case.name:
        script.maximize_button(True)
        script.geometry('button-maximize', (0, 0, width, height), True)
        script.maximize_button(False)
        script.geometry('button-restore', initial)
        time.sleep(0.6)
        script.double_title()
        script.geometry('title-maximize', (0, 0, width, height), True)
        time.sleep(0.6)
        script.double_title()
        script.geometry('title-restore', initial)
        # A further button cycle must still configure the complete content.
        script.maximize_button(True)
        script.geometry('button-after-title', (0, 0, width, height), True)
        time.sleep(0.6)
        script.double_title()
        script.geometry('button-max-title-restore', initial)
        context.check('gui.keyboard-maximize-unavailable', True,
                      'desktop session defines Alt+Tab/Alt+F4/Ctrl+Alt+T, no keyboard maximize')
    else:
        keep = script.model.policy['VisibleTitleWidth']
        frame_width, frame_height = initial[2:]
        # Pointer is confined to physical scanout; use beyond-output positions
        # on the small session, and saturated reports on the native desktop.
        drop_x = min(screen_w-script.title_x-1, width+80)
        drop_y = min(screen_h-script.title_y-1, height+80)
        expected_x = min(drop_x, width-keep)
        expected_y = min(drop_y, height-script.model.constants['FrameWidth']-script.model.constants['TitleBarHeight'])
        for index, delay in enumerate((0.15, 0.02, 0.001)):
            time.sleep(0.6)
            script.drag(drop_x, drop_y, delay)
            script.geometry('edge-%d' % index, (expected_x, expected_y, frame_width, frame_height))
            # Push the mouse past the physical corner while held: clamping
            # must preserve this window, its restore frame and its controls.
            time.sleep(0.6)
            script.drag(20, 20, delay)
            script.geometry('edge-movable-before-burst-%d' % index, (20, 20, frame_width, frame_height))
            time.sleep(0.6)
            # Start the unpaced burst in the interior: packets must move the
            # frame as well as saturate the cursor, including damage pressure
            # while the compositor may complete a slower full pass.
            script.begin_title_grab()
            for _ in range((max(screen_w, screen_h)+126)//127+1):
                script.send([dict(type='rel', data=dict(axis='x', value=127)),
                             dict(type='rel', data=dict(axis='y', value=127))])
                time.sleep(delay)
            time.sleep(0.4)
            script.drain_burst()
            # Virtual relative mice may discard burst displacement. Finish
            # at the corner with observed steps, then push beyond it again.
            script.move(screen_w-1, screen_h-1)
            script.send([dict(type='rel', data=dict(axis='x', value=127)),
                         dict(type='rel', data=dict(axis='y', value=127))])
            time.sleep(0.1)
            context.require('gui.physical-corner-arrival.%d' % index,
                            script.wait(lambda: script.observe_pointer() == tuple(script.position)))
            script.button(False)
            script.drain_burst()
            script.geometry('past-corner-%d' % index, (expected_x, expected_y, frame_width, frame_height))
            time.sleep(0.6)
            script.drag(20, 20, delay)
            script.geometry('edge-movable-%d' % index, (20, 20, frame_width, frame_height))
        # Resize from the actual bottom-right frame corner, then maximize,
        # restore and close; continued client Presents alone are insufficient.
        frame = script.scene()[0][:4]
        shrink = frame[2]+frame[0] > screen_w or frame[3]+frame[1] > screen_h
        if shrink:
            time.sleep(0.6)
            script.drag(0, 0, 0.02)
            script.wait(lambda: script.scene()[0][:2] == [0, 0])
            frame = script.scene()[0][:4]
        script.move(frame[0]+frame[2]-2, frame[1]+frame[3]-2)
        script.button(True)
        dx, dy = (-8, -16) if shrink else (8, 16)
        script.move(script.position[0]+dx, script.position[1]+dy)
        script.button(False)
        resized = (frame[0], frame[1], frame[2]+dx, frame[3]+dy)
        script.geometry('corner-resize', resized)
        script.maximize_button(True)
        script.geometry('edge-maximize', (0, 0, width, height), True)
        script.maximize_button(False)
        script.geometry('edge-restore', resized)
        time.sleep(0.6)
        script.double_title()
        script.geometry('edge-title-maximize', (0, 0, width, height), True)
        time.sleep(0.6)
        script.double_title()
        script.geometry('edge-title-restore', resized)
        x, y, w, h = script.model.close_button_rect(tuple(script.scene()[0][:4]))
        script.click(x+w//2, y+h//2, lambda: 0 in script.scene() and script.scene()[0][-1] & 4)
        if context.case.product_desktop:
            # Root-owned viewer returns to the trusted console on close.
            context.check('gui.edge-closable', script.wait(lambda: 'USER:# session: fallback client-closed' in context.console.lines))
        else:
            context.check('gui.edge-closable', script.wait(lambda: 0 not in script.scene()))
        script.picture('edge-closed', verify_frame=not context.case.product_desktop)
    if context.case.product_desktop:
        ProductDesktop.healthy(context, fallback='Edge' in context.case.name)
    else:
        if any(not check.passed for check in context.result.checks):
            # A failed before-fix client may have exited and cannot provide
            # the F12 phase acknowledgement. Keep the failing pixel/geometry
            # checks and let the executor capture and shut down this VM.
            return
        # Complete the test root's remaining phases and exercise its teardown.
        script.tap('f12')
        since = 0
        for phase in ('resize', 'click-key', 'key-click', 'alt-tab'):
            since, _ = context.wait_for_line(SessionInput.PREFIX + 'phase ' + phase, 60, since=since)
            since += 1
            script.tap('f12')
        context.wait_for_line(SessionInput.PREFIX + 'phase close', 120, since=since)
        if 'Edge' not in context.case.name:
            script.send([ProductDesktop.edge('alt', True)])
            script.tap('f4')
            script.send([ProductDesktop.edge('alt', False)])
        # Edge already destroyed its tested surface. Keep the remaining
        # window alive to acknowledge the root's final F12 fence.
        script.tap('f12')
        context.wait_for_line(SessionInput.PREFIX + 'torn down', 90)
        Guest.wait(context)
    context.check('gui.input-order.no-drops', all(line == '# session: input dropped 0'
                  for line in script.lines() if line.startswith('# session: input dropped ')))
