# SPDX-License-Identifier: Apache-2.0
"""Product readiness gates input and the screen contains the serial echo."""
import pathlib
import tempfile
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'Tools' / 'Verify'))

from AsteriaVm import Display
from AsteriaVm.Drivers import Production
from AsteriaVm.Executor import CaseFailure


class ProductTests(unittest.TestCase):
    def test_failed_xhci_keeps_ps2_echo_and_requires_the_failure_report(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            prefix = 'USER:# init: xhci failed, continuing'
            failure = prefix + ' 0000:00:08.0 step=9 err=0x03 code=0x00000309c0de00e9'
            display = 'USER:# display: 1280x800 bgrx, console 160x50'
            ready = 'USER:# init: ready: display, input, ps2'
            lines = [display, 'USER:# devices: input 8042=present', failure, ready]
            context = SimpleNamespace(case=SimpleNamespace(isa='Amd64', i8042=True, xhci=True, xhci_failure=True),
                console=SimpleNamespace(lines=lines), check=mock.Mock(), artifact=lambda name: path / name,
                qmp=SimpleNamespace(execute=mock.Mock(), wait_event=mock.Mock()))
            seen = []
            def wait(predicate, timeout):
                seen.append(predicate)
                return 0, display
            context.wait_for_line = wait
            def inject(current):
                self.assertLess(seen.index(prefix), seen.index(ready))
                self.assertEqual(context.qmp.execute.call_args_list,
                    [mock.call('device_del', {'id': 'usbkbd'}), mock.call('device_del', {'id': 'usbmouse'})])
                current.artifact('Serial.log').write_text('USER:asteria\nUSER:[L-- 5,-3]\n')
            with mock.patch.object(Production, '_amd64', return_value=(True, 'idle')), \
                    mock.patch.object(Production.Ps2, 'inject_terminal', side_effect=inject), \
                    mock.patch.object(Production, 'repeat_terminal'), \
                    mock.patch.object(Production, '_display'), mock.patch.object(Production, '_pointer_checks'):
                Production.run(context)
            checks = {call.args[0]: call.args[1] for call in context.check.call_args_list}
            self.assertTrue(checks['product.optional-driver'])
            self.assertTrue(checks['product.terminal-serial'])

    def test_xhci_failure_fields_match_the_exit_code(self):
        line = '# init: xhci failed, continuing 0000:00:08.0 step=9 err=0x03 code=0x00000309c0de00e9'
        self.assertTrue(Production.xhci_failure_reported([line]))
        for mutant in (line.split(' 0000:')[0], line.replace('step=9', 'step=8'),
                       line.replace('err=0x03', 'err=0x04'), line.replace('c0de00e9', 'c0de00ea'),
                       line.replace('0000:00:08.0', '')):
            self.assertFalse(Production.xhci_failure_reported([mutant]))

    def test_readiness_precedes_input_and_serial_is_required(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            ready = 'USER:# init: ready: display, input, ps2'
            display = 'USER:# display: 1280x800 bgrx, console 160x50'
            for missing in ('ready', 'serial', 'inventory', 'inventory-late', ''):
                inventory = 'USER:# devices: input 8042=present'
                lines = ([display, ready] if missing == 'inventory'
                         else [display, ready, inventory] if missing == 'inventory-late'
                         else [display, inventory, ready])
                context = SimpleNamespace(case=SimpleNamespace(isa='Amd64', i8042=True, xhci=False),
                                          console=SimpleNamespace(lines=lines),
                                          check=mock.Mock(), artifact=lambda name: path / name)
                seen = []
                def wait(predicate, timeout):
                    seen.append(predicate)
                    if predicate == ready and missing == 'ready':
                        raise CaseFailure('no readiness')
                    return 0, display
                context.wait_for_line = wait
                def inject(current):
                    self.assertIn(ready, seen)
                    current.artifact('Serial.log').write_text(
                        'USER:asteria\n' + ('' if missing == 'serial' else 'USER:[L-- 5,-3]\n'))
                with mock.patch.object(Production, '_amd64', return_value=(True, 'idle')), \
                        mock.patch.object(Production.Ps2, 'inject_terminal', side_effect=inject) as injection, \
                        mock.patch.object(Production, 'repeat_terminal'), \
                        mock.patch.object(Production, '_display'), mock.patch.object(Production, '_pointer_checks'):
                    if missing == 'ready':
                        with self.assertRaises(CaseFailure):
                            Production.run(context)
                        injection.assert_not_called()
                        continue
                    Production.run(context)
                    checks = {call.args[0]: call.args[1] for call in context.check.call_args_list}
                    self.assertEqual(checks['product.terminal-serial'], missing != 'serial')
                    self.assertEqual(checks['product.inventory'], missing not in ('inventory', 'inventory-late'))

    def test_product_screen_excludes_serial_only_diagnostics_and_checks_pixels(self):
        lines = ['USER:# display: 640x400 bgrx, console 80x25',
                 'USER:# shared image: diagnostic only', 'USER:# devices: cpu 0',
                 'USER:# usb: xhci0 port 3 connect usb2 portsc=0x00000603',
                 'USER:# init: ready: display, input, ps2',
                 'USER:# usb: xhci0 port 3 polling 2 interfaces',
                 'USER:asteria', 'USER:[L-- 5,-3]', 'USER:[--- 5,-3]']
        expected = [line[5:] for line in lines if 'shared image' not in line]
        console = Display.TextConsole(640, 400)
        console.write(('\n'.join(expected) + '\n').encode('ascii'))
        pixels = console.render(640, 400, Display.load_font(ROOT), Display.console_colors(ROOT))
        pixels = Display.draw_cursor(pixels, 640, 400, (320, 200), ROOT)
        with tempfile.TemporaryDirectory() as directory:
            for altered in (False, True):
                actual = pixels if not altered else bytes([pixels[0] ^ 1]) + pixels[1:]
                context = SimpleNamespace(case=SimpleNamespace(expected_display=None), artifact=lambda name: pathlib.Path(directory) / name,
                                          console=SimpleNamespace(lines=lines), target=SimpleNamespace(root=ROOT),
                                          seconds=lambda value: 0.0, check_problems=mock.Mock(),
                                          qmp=SimpleNamespace(screendump=lambda path, **options:
                                                              path.write_bytes(Display.write_ppm(640, 400, actual))))
                Production._display(context)
                self.assertEqual(bool(context.check_problems.call_args.args[1]), altered)

    def test_xhci_product_requires_usb_progress_lines(self):
        complete = ['USER:# usb: xhci0 port 5 connect usb2 portsc=0x00020ee1',
                    'USER:# usb: xhci0 port 6 connect usb2 portsc=0x00020ee1',
                    'USER:# usb: xhci0 port 5 polling 1 interface',
                    'USER:# init: ready: display, input, ps2, xhci',
                    'USER:# usb: xhci0 port 6 polling 1 interface']
        for name, lines in (('complete', complete), ('one-device', complete[:4]),
                            ('one-connect', complete[1:]), ('none', complete[3:4])):
            def wait(predicate, timeout, since=0, lines=lines):
                for index in range(since, len(lines)):
                    if predicate(lines[index]):
                        return index, lines[index]
                raise CaseFailure('no line')
            context = SimpleNamespace(case=SimpleNamespace(usb_host=''), console=SimpleNamespace(lines=lines),
                                      check=mock.Mock(), wait_for_line=wait)
            if name in ('one-device', 'none'):
                with self.assertRaises(CaseFailure):
                    Production.usb_progress(context)
                continue
            Production.usb_progress(context)
            self.assertEqual(context.check.call_args.args[:2], ('product.usb-progress', name == 'complete'), name)

    def test_pointer_settles_after_the_last_fence_or_at_the_deadline(self):
        # A fake clock: each sleep advances it, and fences arrive while it runs.
        for name, isa, arrivals, settled in (('quiet', 'Amd64', (), 0.5),
                                             ('draining', 'Amd64', (0.2, 0.4, 0.7), 1.2),
                                             ('ia64', 'Ia64', (0.3,), 1.3),
                                             ('endless', 'Amd64', tuple(0.3 * n for n in range(1, 60)), 10.0)):
            clock = SimpleNamespace(now=0.0)
            def sleep(seconds, clock=clock):
                clock.now = round(clock.now + seconds, 6)
            def lines(clock=clock, arrivals=arrivals):
                return ['USER:# pointer: 0,0 sequence %d' % index
                        for index, at in enumerate(arrivals) if at <= clock.now]
            console = mock.Mock()
            type(console).lines = mock.PropertyMock(side_effect=lines)
            context = SimpleNamespace(case=SimpleNamespace(isa=isa), console=console, seconds=lambda value: value)
            fake = SimpleNamespace(monotonic=lambda clock=clock: clock.now, sleep=sleep)
            with mock.patch.object(Production, 'time', fake):
                Production._settle_pointer(context)
            self.assertAlmostEqual(clock.now, settled, delta=0.051, msg=name)

    def test_usb_host_product_names_the_controllers_its_devices_ended_on(self):
        # The split layout: a high-speed keyboard on the EHCI, a full-speed
        # mouse released to its UHCI companion.
        complete = ['USER:# usb: ehci0 port 5 connect usb2 portsc=0x00001005',
                    'USER:# usb: ehci0 port 3 connect usb2 portsc=0x00001001',
                    'USER:# usb: ehci0 port 3 full/low speed, released to companion portsc=0x00003001',
                    'USER:# usb: uhci2 port 1 connect usb1 portsc=0x00000083',
                    'USER:# usb: ehci0 port 5 polling 1 interface',
                    'USER:# usb: uhci2 port 1 polling 1 interface']
        missing = complete[:5] + ['USER:# usb: ehci0 port 6 polling 1 interface']
        for name, lines, layout in (('complete', complete, 'split'),
                                    ('unreleased', complete[:2] + complete[3:], 'split'),
                                    ('missing-controller', missing, 'split')):
            def wait(predicate, timeout, since=0, lines=lines):
                for index in range(since, len(lines)):
                    if predicate(lines[index]):
                        return index, lines[index]
                raise CaseFailure('no line')
            context = SimpleNamespace(case=SimpleNamespace(usb_host='ich9', usb_layout=layout),
                                      console=SimpleNamespace(lines=lines), check=mock.Mock(), wait_for_line=wait)
            Production.usb_progress(context)
            self.assertEqual(context.check.call_args_list[0].args[:2], ('product.usb-progress', True), name)
            self.assertEqual(context.check.call_args_list[1].args[:2],
                             ('product.usb-controllers', name == 'complete'), name)
