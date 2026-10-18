# SPDX-License-Identifier: Apache-2.0
"""The terminal probe rejects absent, excessive and post-release repeats."""
import unittest
import itertools
import tempfile
from pathlib import Path
import sys
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'Verify'))

from AsteriaVm.Drivers import Production


class RepeatProbeTests(unittest.TestCase):
    def test_hold_release_erasure_and_transport(self):
        for usb, isa in itertools.product((False, True), ('Amd64', 'Ia64')):
            for fault in ('', 'missing', 'excessive', 'after-release', 'erase'):
                with self.subTest(usb=usb, isa=isa, fault=fault):
                    clock = [0.0]
                    pending = ['']
                    lines = []
                    events = []
                    checks = {}
                    def execute(command, arguments):
                        self.assertEqual(command, 'input-send-event')
                        self.assertEqual(arguments.get('device'), 'usbconsole' if usb else None)
                        event = arguments['events'][0]['data']
                        key = event['key']['data']
                        events.append((key, event['down'], clock[0]))
                        if event['down']:
                            if key == 'ret':
                                lines.append('USER:' + pending[0])
                                pending[0] = ''
                            elif key == 'backspace':
                                pending[0] = pending[0][:-1]
                            else:
                                pending[0] += ' ' if key == 'spc' else key
                                if key == 'z' and fault == 'after-release':
                                    pending[0] += 'r'
                        elif key == 'r' and pending[0].startswith('repeat r'):
                            pending[0] += 'r' * (0 if fault == 'missing' else 1000 if fault == 'excessive' else 10)
                        elif key == 'backspace' and fault != 'erase':
                            pending[0] = pending[0][:-9]
                    temporary = tempfile.TemporaryDirectory()
                    self.addCleanup(temporary.cleanup)
                    def screen(path, **options):
                        if fault == 'after-release' and 'settled' in path.name:
                            pending[0] += 'r'
                        path.write_bytes(pending[0].encode())
                    context = SimpleNamespace(case=SimpleNamespace(xhci=usb, isa=isa),
                        qmp=SimpleNamespace(execute=execute, screendump=screen),
                        artifact=lambda name: Path(temporary.name) / name,
                        wait_for_line=lambda predicate, timeout: next((index, line)
                            for index, line in enumerate(lines) if predicate(line)),
                        check=lambda name, passed, detail: checks.update({name: passed}))
                    def sleep(duration):
                        clock[0] += duration
                    with mock.patch.object(Production.time, 'sleep', side_effect=sleep), \
                            mock.patch.object(Production.time, 'monotonic', side_effect=lambda: clock[0]):
                        Production.repeat_terminal(context)
                    self.assertEqual(checks['terminal.repeat.r'], fault not in ('missing', 'excessive', 'after-release'))
                    self.assertEqual(checks['terminal.repeat.backspace'], fault not in ('erase', 'after-release'))
                    for key in ('r', 'backspace'):
                        self.assertEqual(checks['terminal.repeat.stopped.' + key], fault != 'after-release')
                    releases = [(key, when) for key, down, when in events if not down]
                    for key, when in releases:
                        if key == 'backspace':
                            down_at = next(at for name, down, at in events if name == key and down)
                            self.assertAlmostEqual(when - down_at, 0.9, places=6)
