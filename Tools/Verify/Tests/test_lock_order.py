# SPDX-License-Identifier: Apache-2.0
"""Held-scope regression tests for processor scheduling's GCC dump gate."""
import unittest
import pathlib
import tempfile
from CheckLockOrder import Analysis, GUARD, DESTRUCTOR, native_ia64


def function(name, body):
    return 'void %s ()\n{\n%s\n}\n' % (name, body)


def held(body):
    return function('Asteria::Owner::Run', '%s (&guard, &lock);\ntry\n{\n%s\n}\nfinally\n{\n%s (&guard);\n}'
                    % (GUARD, body, DESTRUCTOR))


class LockOrderTests(unittest.TestCase):
    def test_transitive_wait_is_forbidden(self):
        analysis = Analysis()
        analysis.gimple(held('Asteria::Helper::Run ();') + function(
            'Asteria::Helper::Run', 'Asteria::Platform::Synchronization::ProcessorWaitProgress ();'))
        self.assertEqual(1, len(analysis.violations()))

    def test_remote_request_service_and_kick_are_forbidden_under_lock(self):
        for sink in ('ServiceThreadRequests', 'ServiceWaitingThreadRequests', 'KickProcessor'):
            with self.subTest(sink=sink):
                analysis = Analysis()
                analysis.gimple(held('Asteria::Platform::Synchronization::%s ();' % sink))
                self.assertEqual(1, len(analysis.violations()))

    def test_wait_after_finally_is_allowed(self):
        analysis = Analysis()
        body = held('Asteria::Helper::Run ();')
        closing = body.rfind('\n}')
        analysis.gimple(body[:closing] + '\nAsteria::Platform::Synchronization::ProcessorWaitProgress ();' + body[closing:])
        self.assertEqual([], analysis.violations())

    def test_virtual_interface_wait_is_forbidden(self):
        analysis = Analysis()
        analysis.virtuals['IPagerShootdownProgress', 0].add('Asteria::Pager::Wait')
        analysis.gimple(held('OBJ_TYPE_REF(_1;(struct IPagerShootdownProgress)progress->0B) (progress);'))
        self.assertEqual(1, len(analysis.violations()))

    def test_nested_acquisition_reaches_its_constructor(self):
        analysis = Analysis()
        analysis.gimple(held('%s (&inner, &other);\n%s (&inner);' % (GUARD, DESTRUCTOR))
                        + function(GUARD, 'Asteria::Platform::Synchronization::ProcessorWaitProgress ();'))
        self.assertEqual(1, len(analysis.violations()))

    def test_unresolved_virtual_under_lock_fails(self):
        analysis = Analysis()
        analysis.gimple(held('OBJ_TYPE_REF(_1;(struct Unknown)progress->0B) (progress);'))
        self.assertEqual(1, len(analysis.violations()))

    def test_native_ia64_slots_are_descriptor_pairs(self):
        analysis = Analysis()
        analysis.virtuals['IPagerShootdownProgress', 1].add('Asteria::Pager::Wait')
        analysis.gimple(held('OBJ_TYPE_REF(_1;(struct IPagerShootdownProgress)progress->2B) (progress);'), True)
        self.assertEqual(1, len(analysis.violations()))

    def test_clock_pointer_binding_cannot_hide_a_wait(self):
        analysis = Analysis()
        analysis.gimple(held('Asteria::Platform::Synchronization::ProcessorWaitTicks ();')
                        + function('Asteria::Platform::Synchronization::ProcessorWaitTicks', '_2 (_3);')
                        + function('Asteria::BadClock', 'Asteria::Platform::Synchronization::ProcessorWaitProgress ();')
                        + function('Asteria::Bind', 'hooks.Ticks = Asteria::BadClock;'))
        self.assertEqual(1, len(analysis.violations()))

    def test_class_primary_address_point(self):
        analysis = Analysis()
        analysis.classes('''Vtable for Asteria::Pager
Asteria::Pager::_ZTV: 3 entries
0     (int (*)(...))0
8     (int (*)(...))0
16    (int (*)(...))Asteria::Pager::Wait

Class Asteria::Pager
   size=8 align=8
Asteria::Pager (0x123) 0
    vptr=((& Asteria::Pager::_ZTV) + 16)
Asteria::IPagerShootdownProgress (0x124) 0
      primary-for Asteria::Pager (0x123)
''')
        self.assertEqual({'Asteria::Pager::Wait'}, analysis.virtuals['IPagerShootdownProgress', 0])

    def test_native_probe_directory_uses_its_build_target(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = pathlib.Path(scratch) / 'ia64-watchdog-probe'
            root.mkdir()
            (root / 'CMakeCache.txt').write_text('ASTERIA_TARGET:STRING=Ia64\n')
            self.assertTrue(native_ia64(root / 'Source'))

    def test_hosted_ia64_analysis_keeps_host_virtual_slots(self):
        with tempfile.TemporaryDirectory() as scratch:
            root = pathlib.Path(scratch) / 'ia64' / 'host-analyze'
            root.mkdir(parents=True)
            (root / 'CMakeCache.txt').write_text('ASTERIA_TARGET:STRING=Host\n')
            self.assertFalse(native_ia64(root / 'Source'))
