# SPDX-License-Identifier: Apache-2.0
"""Healthy product sessions reject startup misses, restarts and fallback."""
import unittest
import pathlib
from types import SimpleNamespace
from unittest import mock
from AsteriaVm.Drivers import ProductDesktop
from AsteriaVerify import Layout


class ProductDesktopTests(unittest.TestCase):
    def judge(self, lines, *, pause=False, **options):
        context = SimpleNamespace(console=SimpleNamespace(lines=lines), check=mock.Mock(),
            case=SimpleNamespace(whole_machine_pause='session' if pause else ''), pause_seconds=7,
            target=SimpleNamespace(root=pathlib.Path(__file__).resolve().parents[3]))
        ProductDesktop.healthy(context, **options)
        return {call.args[0]: call.args[1] for call in context.check.call_args_list}

    def test_native_boot_and_reboot_have_no_misses_or_restarts(self):
        ready = 'USER:# session: desktop 2560x1440 output-pages=3600 client-pages=7200'
        self.assertTrue(all(self.judge([ready]).values()))
        self.assertTrue(all(self.judge([ready, ready], boots=2).values()))
        self.assertFalse(self.judge([ready, ready])['desktop.healthy.no-restarts'])

    def test_healthy_boot_rejects_a_watchdog_miss_or_startup_expiry(self):
        ready = 'USER:# session: desktop 640x400 output-pages=250 client-pages=500'
        for diagnostic in ('USER:# watchdog: session-compositor unresponsive for 5401 ms',
                           'USER:# startup: session-compositor not ready within 60000 ms'):
            with self.subTest(diagnostic=diagnostic):
                self.assertFalse(self.judge([ready, diagnostic])['desktop.healthy.watchdog'])

    def test_only_the_withheld_face_probe_accepts_console_fallback(self):
        line = 'USER:# session: fallback session-face'
        self.assertFalse(self.judge([line])['desktop.healthy.fallback'])
        self.assertTrue(all(self.judge([line], boots=0, fallback=True).values()))

    def test_recovery_waits_for_three_incarnations_before_console_fallback(self):
        ready = 'USER:# session: desktop 640x400 output-pages=250 client-pages=500'
        for ages in ([5000, 10000, 15000] * 3, [15000] * 3):
            with self.subTest(ages=ages):
                reports = ['USER:# watchdog: session-compositor unresponsive for %d ms' % age for age in ages]
                context = SimpleNamespace(
                    case=SimpleNamespace(session_compositor_fault=True),
                    console=SimpleNamespace(lines=[ready, ready, ready] + reports),
                    wait_for_line=mock.Mock(side_effect=[(index, '') for index in range(5)]),
                    check=mock.Mock())
                with mock.patch.object(ProductDesktop.Ps2, 'inject_terminal') as inject, mock.patch.object(ProductDesktop, 'recovery_input', create=True) as recovered:
                    ProductDesktop.run(context)
                inject.assert_called_once_with(context)
                self.assertEqual(recovered.call_count, 3)
                calls = context.wait_for_line.call_args_list
                self.assertEqual([call.args[0] for call in calls[:3]], ['USER:# session: desktop '] * 3)
                self.assertEqual(calls[3].args[0], 'USER:# session: fallback compositor')
                context.check.assert_any_call('desktop.two-restarts', True)
                context.check.assert_any_call('desktop.three-deaths', True)

    def test_pause_retains_one_expired_query_but_requires_the_same_incarnation(self):
        ready = 'USER:# session: desktop 640x400 output-pages=250 client-pages=500'
        report = 'USER:# watchdog: session-compositor unresponsive for 7014 ms'
        self.assertTrue(all(self.judge([ready, report], pause=True).values()))
        self.assertFalse(self.judge([ready, report])['desktop.healthy.watchdog'])
        self.assertFalse(self.judge([ready, ready, report], pause=True)['desktop.healthy.no-restarts'])

    def test_pause_cannot_hide_startup_expiry_or_persistent_service_failure(self):
        ready = 'USER:# session: desktop 640x400 output-pages=250 client-pages=500'
        limits = Layout.constants((pathlib.Path(__file__).resolve().parents[3]
                                   / 'Source/Abi/Watchdog.hpp').read_text())
        reports = [
            ['USER:# startup: session-compositor not ready within 60000 ms'],
            ['USER:# watchdog: session-compositor unresponsive for 15000 ms'],
            ['USER:# watchdog: session-compositor unresponsive for 7014 ms']
            * (limits['WatchdogServiceCount'] + 1),
        ]
        for diagnostic in reports:
            with self.subTest(diagnostic=diagnostic):
                self.assertFalse(self.judge([ready] + diagnostic, pause=True)['pause.service-query-diagnostics'])
