# SPDX-License-Identifier: Apache-2.0
"""Readiness, single injection, exact QMP actions and interrupt evidence."""
import pathlib
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

from AsteriaVm import Ps2


class InjectionTests(unittest.TestCase):
    def test_ready_gates_one_exact_input_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            context = SimpleNamespace(case=SimpleNamespace(isa='Amd64', xhci=False), console=SimpleNamespace(lines=[]), qmp=mock.Mock(),
                                      target=SimpleNamespace(root=path),
                                      artifact=lambda name: path / name,
                                      require=mock.Mock(), check=mock.Mock())
            before = SimpleNamespace(Delivered=29, Spurious=0)
            schema = SimpleNamespace(decode=lambda data: SimpleNamespace(Interrupts=before))
            def save(qmp, lines, destination):
                destination.write_bytes(b'baseline')
                return None
            with mock.patch.object(Ps2.Evidence, 'save', side_effect=save), \
                    mock.patch.object(Ps2.Amd64Evidence, 'schema', return_value=schema), \
                    mock.patch.object(Ps2.time, 'sleep'):
                injection = Ps2.Injection(context)
                injection.poll()
                context.qmp.execute.assert_not_called()
                context.console.lines.append('USER:' + Ps2.READY)
                injection.poll()
                injection.poll()
            calls = context.qmp.execute.call_args_list
            self.assertEqual(len(calls), Ps2.EVENT_COUNT)
            self.assertTrue(all(call.args[0] == 'input-send-event' for call in calls))
            events = [event for call in calls for event in call.args[1]['events']]
            keys = [event['data'] for event in events if event['type'] == 'key']
            self.assertEqual([key['key']['data'] for key in keys[::2][:8]], list('asteria') + ['ret'])
            self.assertEqual([key['down'] for key in keys[:16]], [True, False] * 8)
            self.assertEqual([key['key']['data'] for key in keys[16:]],
                             ['ctrl', 'alt', 'delete', 'delete', 'alt', 'ctrl'])
            self.assertEqual([event['data'] for event in events if event['type'] == 'rel'],
                             [dict(axis='x', value=5), dict(axis='y', value=-3)])
            self.assertEqual([event['data'] for event in events if event['type'] == 'btn'],
                             [dict(down=True, button='left'), dict(down=False, button='left')])
            context.console.lines.append('USER:' + Ps2.DONE)
            injection.check(SimpleNamespace(Interrupts=SimpleNamespace(Delivered=54, Spurious=0)))
            self.assertTrue(all(call.args[1] for call in context.check.call_args_list))
            # 29 before; 25 events need at least 13 deliveries of their own.
            for delivered, spurious in ((41, 0), (54, 1)):
                context.check.reset_mock()
                injection.check(SimpleNamespace(Interrupts=SimpleNamespace(Delivered=delivered, Spurious=spurious)))
                self.assertFalse(context.check.call_args_list[-1].args[1])

    def test_ia64_paces_each_edge_by_the_guest_delivery(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            guest = {'delivered': 7, 'polls': 0}
            order = []
            clock = [0.0]

            def execute(command, arguments):
                order.append(('send', guest['delivered']))
                guest['polls'] = 0

            def save(qmp, lines, destination):
                # The driver takes each edge's delivery on the third poll,
                # except the ninth edge, whose bytes are never delivered.
                guest['polls'] += 1
                if guest['polls'] == 3 and len(order) != 9:
                    guest['delivered'] += 1
                destination.write_bytes(str(guest['delivered']).encode())
                return None

            def sleep(seconds):
                clock[0] += seconds

            schema = SimpleNamespace(decode=lambda data: SimpleNamespace(
                Interrupts=SimpleNamespace(Delivered=int(data), Spurious=0)))
            context = SimpleNamespace(case=SimpleNamespace(isa='Ia64', xhci=False),
                                      console=SimpleNamespace(lines=['USER:' + Ps2.READY]),
                                      qmp=SimpleNamespace(execute=execute), target=SimpleNamespace(root=path),
                                      artifact=lambda name: path / name, seconds=lambda value: value,
                                      require=mock.Mock(), check=mock.Mock())
            with mock.patch.object(Ps2.Evidence, 'save', side_effect=save), \
                    mock.patch.object(Ps2.Ia64Evidence, 'schema', return_value=schema), \
                    mock.patch.object(Ps2.time, 'sleep', side_effect=sleep), \
                    mock.patch.object(Ps2.time, 'monotonic', side_effect=lambda: clock[0]):
                Ps2.Injection(context).poll()
            self.assertEqual(len(order), Ps2.EVENT_COUNT)
            # Every edge waits for the previous edge's delivery; the undelivered
            # ninth edge releases the tenth only at the bounded deadline.
            self.assertEqual([seen for _, seen in order], [7 + index - (index >= 9) for index in range(25)])
            self.assertGreaterEqual(clock[0], Ps2.DELIVERY_SECONDS)
            self.assertLess(clock[0], Ps2.DELIVERY_SECONDS + 25 * 0.2)

    def test_usb_terminal_targets_named_console(self):
        with tempfile.TemporaryDirectory() as directory:
            context = SimpleNamespace(case=SimpleNamespace(isa='Amd64', xhci=True), qmp=mock.Mock(),
                                      artifact=lambda name: pathlib.Path(directory) / name)
            with mock.patch.object(Ps2.time, 'sleep'):
                Ps2.inject_terminal(context)
            self.assertTrue(context.qmp.execute.called)
            self.assertTrue(all(call.args[1]['device'] == 'usbconsole'
                                for call in context.qmp.execute.call_args_list))

    def test_unready_driver_fails_client_proof(self):
        context = SimpleNamespace(console=SimpleNamespace(lines=[]), check=mock.Mock())
        Ps2.Injection(context).check(None)
        self.assertFalse(context.check.call_args.args[1])


class TerminalInjectionTests(unittest.TestCase):
    def test_product_probe_has_no_attention_or_scenario_handshake(self):
        with tempfile.TemporaryDirectory() as directory:
            context = SimpleNamespace(case=SimpleNamespace(isa='Amd64', xhci=False), qmp=mock.Mock(),
                                      artifact=lambda name: pathlib.Path(directory) / name)
            with mock.patch.object(Ps2.time, 'sleep'):
                Ps2.inject_terminal(context)
            events = [event for call in context.qmp.execute.call_args_list
                      for event in call.args[1]['events']]
            keys = [event['data']['key']['data'] for event in events if event['type'] == 'key']
            self.assertEqual(keys, [key for key in (*'asteria', 'ret') for _ in range(2)])
            self.assertEqual([event['data'] for event in events if event['type'] == 'btn'],
                             [dict(down=True, button='left'), dict(down=False, button='left')])
            self.assertEqual([event['data'] for event in events if event['type'] == 'rel'],
                             [dict(axis='x', value=5), dict(axis='y', value=-3)])
