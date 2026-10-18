# SPDX-License-Identifier: Apache-2.0
"""The guest test program's TAP report (AsteriaVm.Guest) on recorded transcripts and their mutants."""
from __future__ import annotations

import pathlib
import re
import socket
import tempfile
import threading
import unittest

from AsteriaVm import Guest
from AsteriaVm.Console import Console, ConsoleTimeout
from AsteriaVm.Executor import CaseFailure, CheckResult
from Tests import test_ia64_fixtures
from Tests.test_amd64_fixtures import REPOSITORY, debug_lines

NEUTRAL_POINTS = (
    'bss.zero', 'query.endpoint', 'query.diagnostic', 'debugwrite.rejects-control-bytes',
    'debugwrite.rejects-a-handle-without-write', 'ipc.send', 'ipc.receive', 'ipc.delivery', 'waitset.wait',
    'waitset.wait')
AMD64_POINTS = NEUTRAL_POINTS + ('abi.code-selector', 'abi.stack-selector', 'abi.interrupts', 'abi.io-privilege',
                                 'abi.stack-bounds', 'abi.syscall-user-state', 'abi.syscall-flags',
                                 'abi.syscall-flags-set', 'abi.syscall-stack', 'pager.read', 'pager.write',
                                 'pager.resident', 'abi.extended-state')
AMD64_PLAN = 'USER:1..%d' % len(AMD64_POINTS)
IA64_POINTS = NEUTRAL_POINTS + ('abi.gp-preserved', 'abi.stack-bounds', 'abi.stacked-registers',
                                'pager.read', 'pager.write', 'pager.resident', 'abi.extended-state')


def names(points: tuple[str,...]) -> list[str]:
    return [Guest.STREAM_CHECK] + ['guest.%d.%s' % (number, name) for number, name in enumerate(points, 1)]


def edited(lines: list[str], prefix: str, *replacements: str) -> list[str]:
    """`lines` with the one line starting with `prefix` replaced by `replacements`."""
    [index] = [index for index, line in enumerate(lines) if line.startswith(prefix)]
    return lines[:index] + list(replacements) + lines[index + 1:]


def failing(lines: list[str]) -> dict[str, str]:
    return {result.name: result.detail for result in Guest.checks(lines) if not result.passed}


class TranscriptTests(unittest.TestCase):
    def test_normal_component_audit_rejects_unexpected_liveness_reports(self):
        for ia64 in (False, True):
            expected = list(Guest.COMPONENT_LINES)
            if ia64:
                expected = [Guest.IA64_DISPLAY_LINE] + expected + [Guest.DISPLAY_STOP_LINE]
            lines = ['USER:' + line for line in expected + [Guest.ROOT_DONE_LINE]]
            self.assertEqual(Guest.component_problems(lines, ia64=ia64), [])
            for component in ('display-broker', 'input-broker', 'session-compositor'):
                report = 'USER:# watchdog: %s unresponsive for 5000 ms' % component
                with self.subTest(ia64=ia64, component=component):
                    self.assertTrue(Guest.component_problems(lines + [report], ia64=ia64))

    def test_frame_rights_probe_is_visible_outside_the_guest_tap_stream(self):
        lines = ['USER:' + Guest.FRAME_RIGHTS_LINE, 'USER:TAP version 13']
        self.assertIn(Guest.FRAME_RIGHTS_LINE, Guest.channel_lines(lines))
        self.assertEqual(Guest.user_lines(lines), ['TAP version 13'])

    def test_recorded_amd64_stream(self):
        lines = debug_lines('Smp4')
        results = Guest.checks(lines)
        self.assertEqual([result.name for result in results], names(AMD64_POINTS))
        self.assertEqual([result for result in results if not result.passed or result.skipped], [])
        self.assertEqual(results[1].detail, "bss.zero: the loader clears the image's zero-initialized storage")
        # Kernel and firmware lines between the guest's lines are not part of the stream.
        first, last = lines.index('USER:TAP version 14'), lines.index(AMD64_PLAN)
        middle = (first + last) // 2
        interleaved = lines[:middle] + ['ASTERIA:STAGE:AMD64-USER-LAUNCH', 'firmware text'] + lines[middle:]
        self.assertEqual(Guest.checks(interleaved), results)

    def test_recorded_ia64_streams(self):
        """IA-64 skips the WaitSet points: its initial image receives no WaitSet yet."""
        for case in ('Smp1', 'Smp2', 'Smp3'):
            results = Guest.checks(test_ia64_fixtures.lines(case))
            self.assertEqual([result.name for result in results], names(IA64_POINTS), case)
            self.assertEqual([result.name for result in results if not result.passed], [], case)
            self.assertEqual({result.name: result.skipped for result in results if result.skipped},
                             {'guest.9.waitset.wait': 'this image receives no WaitSet',
                              'guest.10.waitset.wait': 'this image receives no WaitSet'}, case)

    def test_a_failing_point_fails_its_check_with_its_diagnostics(self):
        lines = edited(debug_lines('Smp4'), 'USER:ok 6 - ipc.send',
                       'USER:not ok 6 - ipc.send: the canonical 88-byte frame is queued as sequence 1',
                       'USER:# value0: expected 0x0000000000000001 observed 0x0000000000000000')
        self.assertEqual(failing(lines), {'guest.6.ipc.send': 'ipc.send: the canonical 88-byte frame is queued as '
                                          'sequence 1\nvalue0: expected 0x0000000000000001 observed '
                                          '0x0000000000000000'})

    def test_an_invalid_stream_fails_the_stream_check(self):
        lines = debug_lines('Smp4')
        for label, mutant in (
                ('no plan', edited(lines, AMD64_PLAN)),
                ('a lost point', edited(lines, 'USER:ok 7 - ipc.receive')),
                ('a short plan', edited(lines, AMD64_PLAN, 'USER:1..%d' % (len(AMD64_POINTS) - 1))),
                ('a bail-out', edited(lines, AMD64_PLAN, 'USER:Bail out! no DiagnosticChannel')),
                ('a line that is not TAP', edited(lines, AMD64_PLAN, 'USER:garbage', AMD64_PLAN)),
                ('no version line', edited(lines, 'USER:TAP version 14'))):
            with self.subTest(label):
                self.assertIn(Guest.STREAM_CHECK, failing(mutant))

    def test_a_skipped_point_is_recorded_as_skipped(self):
        lines = edited(debug_lines('Smp4'), 'USER:ok 9 - waitset.wait',
                       'USER:ok 9 - waitset.wait # SKIP this image receives no WaitSet')
        result = next(result for result in Guest.checks(lines) if result.name == 'guest.9.waitset.wait')
        self.assertEqual((result.passed, result.skipped), (True, 'this image receives no WaitSet'))

    def test_identifiers(self):
        for description, name in (('bss.zero: the loader clears', 'bss.zero'), ('ipc.send', 'ipc.send'),
                                  ('', 'point'), (': no name', 'point')):
            self.assertEqual(Guest.identifier(description), name, description)

    def test_record(self):
        class Context:
            def __init__(self):
                self.checks = []

            def check(self, name, passed, detail='', *, skipped=''):
                self.checks.append(CheckResult(name, passed, detail, skipped))
                return passed

        context = Context()
        Guest.record(context, debug_lines('Smp4'))
        self.assertEqual(context.checks, Guest.checks(debug_lines('Smp4')))


class SpinnerTests(unittest.TestCase):
    """The AMD64 spinner's line must follow the guest's complete stream."""

    def test_the_line_is_the_one_the_spinner_writes(self):
        header = (REPOSITORY / 'Source/User/Spinner/SpinnerProgram.hpp').read_text()
        self.assertIn('SpinnerDoneLine[] = "%s\\n";' % Guest.SPINNER_LINE, header)
        self.assertIn('SpinnerStateLostLine[] = "%s\\n";' % Guest.SPINNER_STATE_LOST_LINE, header)

    def test_recorded_amd64_spinner(self):
        lines = debug_lines('Smp4')
        self.assertEqual(Guest.spinner_problems(lines), [])
        spinner = 'USER:' + Guest.SPINNER_LINE
        without = [line for line in lines if line != spinner]
        self.assertEqual(Guest.spinner_problems(without), ["the spinner line '# spinner: done' appears 0 times"])
        self.assertEqual(Guest.spinner_problems(lines + [spinner]),
                         ["the spinner line '# spinner: done' appears 2 times"])
        # Before the plan, the line is still a TAP comment, but preemption failed.
        early = edited(without, AMD64_PLAN, spinner, AMD64_PLAN)
        self.assertEqual(len(Guest.spinner_problems(early)), 1)
        self.assertIn('before the stream was complete', Guest.spinner_problems(early)[0])
        # A spinner whose extended state changed writes another line.
        lost = [('USER:' + Guest.SPINNER_STATE_LOST_LINE) if line == spinner else line for line in lines]
        self.assertEqual(Guest.spinner_problems(lost), ["the spinner line '# spinner: done' appears 0 times"])


class ThreadTests(unittest.TestCase):
    """The AMD64 pager's created thread writes one line, which is not part of the guest's stream."""

    def test_the_lines_are_the_ones_the_thread_writes(self):
        header = (REPOSITORY / 'Source/User/Pager/PagerProgram.hpp').read_text()
        self.assertIn('PagerThreadDoneLine[] = "%s\\n";' % Guest.THREAD_LINE, header)
        for name in ('PagerThreadStateLostLine', 'PagerThreadTimeoutLine'):
            self.assertIn('%s[] = "%s' % (name, Guest.THREAD_PREFIX), header)

    def test_recorded_amd64_thread(self):
        lines = debug_lines('Smp4')
        self.assertEqual(Guest.thread_problems(lines), [])
        thread = 'USER:' + Guest.THREAD_LINE
        without = [line for line in lines if line != thread]
        self.assertEqual(len(Guest.thread_problems(without)), 1)
        self.assertEqual(len(Guest.thread_problems(lines + [thread])), 1)
        lost = [('USER:# thread: extended state lost') if line == thread else line for line in lines]
        self.assertIn('extended state lost', Guest.thread_problems(lost)[0])

    def test_a_thread_line_before_the_version_line_leaves_the_stream_valid(self):
        lines = [line for line in debug_lines('Smp4') if line != 'USER:' + Guest.THREAD_LINE]
        first = lines.index('USER:TAP version 14')
        early = lines[:first] + ['USER:' + Guest.THREAD_LINE] + lines[first:]
        self.assertEqual(failing(early), {})
        self.assertEqual(Guest.thread_problems(early), [])


class ComponentTests(unittest.TestCase):
    """The AMD64 root service reports its components outside the guest's stream."""

    def test_unpublished_8042_requires_the_skip_line(self):
        ps2 = ['USER:# input: PS/2 ready, QMP may inject', 'USER:asteria',
               "USER:# input: typed 'asteria', pointer moved (5,-3), left button, PS/2, secure attention reported, "
               'second client empty']
        lines = [line for line in debug_lines('Smp4') if line != 'USER:' + Guest.PORTS_LINE]
        lines.insert(lines.index('USER:' + Guest.ECHO_LINE), 'USER:' + Guest.PORTS_SKIPPED_LINE)
        # Without an i8042 the PS/2 demonstration is skipped too.
        at = lines.index(ps2[0])
        lines = [line for line in lines if line not in ps2]
        lines.insert(at, 'USER:# input: i8042 not published, PS/2 demo skipped')
        self.assertEqual(Guest.component_problems(lines, echo=True, ports=False), [])
        self.assertTrue(Guest.component_problems(lines, echo=True, ports=True))

    def updated_net_lines(self, name):
        # The recordings carry the network demonstration's lines.
        lines = debug_lines(name)
        for net in Guest.NET_LINES:
            self.assertIn('USER:' + net, lines)
        return lines

    def test_describe_capacity_is_required_and_matches_the_recipe(self):
        # The recordings carry the Describe line before the recipe's FS line.
        for name, disk, sectors in [('Smp4', '', 0), ('Ahci', 'raw', 4096)]:
            lines = self.updated_net_lines(name)
            at = lines.index('USER:# fs: Describe 512-byte blocks, sectors 0x%016X' % sectors)
            self.assertLess(at, lines.index('USER:' + Guest.FS_LINES[disk]))
            self.assertEqual(Guest.component_problems(lines, echo=True, disk=disk), [])
            changed = list(lines)
            changed[at] = 'USER:# fs: Describe 512-byte blocks, sectors 0x%016X' % (sectors + 1)
            self.assertTrue(Guest.component_problems(changed, echo=True, disk=disk))
            missing = lines[:at] + lines[at + 1:]
            self.assertTrue(Guest.component_problems(missing, echo=True, disk=disk))

    def test_the_lines_are_the_ones_the_root_service_writes(self):
        header = (REPOSITORY / 'Tests/Guest/Pager/RootScenarios.hpp').read_text()
        for line in Guest.COMPONENT_LINES + [Guest.ACCOUNT_LINE, Guest.DRIVER_LINE, Guest.DRIVER_RECOVERED_LINE,
                                             Guest.DRIVER_FACTORIAL_RECOVERED_LINE,
                                             Guest.DRIVER_REGISTERS_LINE, Guest.ECHO_LINE,
                                             Guest.BLOCK_THROTTLE_LINE, Guest.BLOCK_SERVED_LINE,
                                             Guest.BLOCK_NO_DISK_LINE, Guest.ROOT_DONE_LINE] \
                + list(Guest.FS_LINES.values()) \
                + Guest.SERVICE_LINES \
                + Guest.LOADER_LINES:
            self.assertRegex(header, re.escape('[]') + r'\s*=\s*' + re.escape('"%s\\n";' % line))

    def test_recorded_amd64_component(self):
        lines = self.updated_net_lines('Smp4')
        self.assertEqual(Guest.component_problems(lines, echo=True), [])
        # AMD64's fourth incarnation, the echo, is required where it runs.
        self.assertEqual(len(Guest.component_problems(lines)), 1, 'an echo where none runs')
        self.assertEqual(len(Guest.component_problems(lines, echo=True, disk=True)), 1, 'a disk this case lacks')
        for missing in (Guest.ACCOUNT_LINE, Guest.DRIVER_LINE, Guest.DRIVER_RECOVERED_LINE, Guest.BLOCK_NO_DISK_LINE,
                        *Guest.NET_LINES, Guest.ECHO_LINE, Guest.ROOT_DONE_LINE):
            dropped = [line for line in lines if line != 'USER:' + missing]
            self.assertEqual(len(Guest.component_problems(dropped, echo=True)), 1, missing)
        crashed, reported = ('USER:' + line for line in (Guest.COMPONENT_LINES[0], Guest.COMPONENT_LINES[-1]))
        self.assertEqual(len(Guest.component_problems([line for line in lines if line != reported], echo=True)), 1)
        once = [line for index, line in enumerate(lines) if line != crashed or index != lines.index(crashed)]
        self.assertEqual(len(Guest.component_problems(once, echo=True)), 1, 'one crash missing')
        swapped = [reported if line == crashed else crashed if line == reported else line for line in lines]
        self.assertEqual(len(Guest.component_problems(swapped, echo=True)), 1)
        self.assertEqual(failing(lines), {})
        first = lines.index('USER:' + Guest.NET_LINES[0])
        last = first + len(Guest.NET_LINES)
        self.assertEqual(Guest.root_service_lines(lines)[
            Guest.root_service_lines(lines).index(Guest.NET_LINES[0]):][:len(Guest.NET_LINES)], Guest.NET_LINES)
        self.assertTrue(Guest.component_problems(lines[:first] + list(reversed(lines[first:last]))
                                                + lines[last:], echo=True))

    def test_ia64_dma_and_registers_variants_keep_input(self):
        recorded = test_ia64_fixtures.lines('Smp4')
        for extra in (Guest.IA64_COMPONENT_LINES, Guest.IA64_REGISTERS_COMPONENT_LINES):
            # Without an IOMMU edu drives its registers only and no NIC is
            # attached, so the network demonstration's lines are absent.
            registers = extra == Guest.IA64_REGISTERS_COMPONENT_LINES
            lines = [line.replace(Guest.DRIVER_INTX_LINE, Guest.DRIVER_REGISTERS_LINE) if registers else line
                     for line in recorded
                     if not (registers and line.removeprefix('USER:') in Guest.NET_LINES)]
            self.assertEqual(Guest.component_problems(lines, extra=extra), [])
            for required in (Guest.INPUT_LINE, '# input: i8042 not published, PS/2 demo skipped'):
                dropped = [line for line in lines if line != 'USER:' + required]
                self.assertTrue(Guest.component_problems(dropped, extra=extra), required)

    def test_ia64_storage_lines_follow_the_disk_recipe(self):
        # The IA-64 root service runs the filesystem and block demonstrations
        # on its AHCI driver: the disk case's lines
        # are the raw recipe's, neither the diskless nor another format's.
        lines = test_ia64_fixtures.lines('Ahci')
        expected = Guest.ia64_component_lines('raw', network=True)
        self.assertEqual(Guest.component_problems(lines, extra=expected, ia64=True), [])
        for recipe in ('', 'fat16'):
            self.assertTrue(Guest.component_problems(lines, extra=Guest.ia64_component_lines(recipe, network=True),
                                                     ia64=True), recipe)
        for line in (Guest.BLOCK_THROTTLE_LINE, Guest.BLOCK_SERVED_LINE, Guest.FS_ASTERIA_LINE):
            dropped = [recorded for recorded in lines if recorded != 'USER:' + line]
            self.assertTrue(Guest.component_problems(dropped, extra=expected, ia64=True), line)

    def test_ia64_network_lines_sit_between_the_filesystem_and_block_lines(self):
        # Source/User/Network/NetworkStack.hpp, the case that attaches the e1000e needs the network
        # demonstration's lines, in their place, and one without it must not show them.
        lines = test_ia64_fixtures.lines('Ahci')
        self.assertTrue(Guest.component_problems(lines, extra=Guest.ia64_component_lines('raw'), ia64=True))
        for line in Guest.NET_LINES:
            dropped = [recorded for recorded in lines if recorded != 'USER:' + line]
            self.assertTrue(Guest.component_problems(
                dropped, extra=Guest.ia64_component_lines('raw', network=True), ia64=True), line)

    def test_ia64_display_metrics_preserve_exact_mode_check(self):
        lines = test_ia64_fixtures.lines('Smp4')
        metrics = ('USER:# display: scanout uc, fill 1024000 bytes 19363 us, '
                   'scroll 256000 px 41000 us, slowest 256000 px 41000 us')
        extended = lines + [metrics]
        self.assertEqual(Guest.component_problems(extended, extra=Guest.IA64_COMPONENT_LINES), [])
        mode = 'USER:' + Guest.IA64_DISPLAY_LINE
        missing = [line for line in extended if line != mode]
        self.assertTrue(Guest.component_problems(missing, extra=Guest.IA64_COMPONENT_LINES))
        changed = [line.replace('640x400', '640x480') if line == mode else line for line in extended]
        self.assertTrue(Guest.component_problems(changed, extra=Guest.IA64_COMPONENT_LINES))

    def test_recorded_amd64_block_with_a_disk(self):
        lines = self.updated_net_lines('Ahci')
        self.assertEqual(Guest.component_problems(lines, echo=True, disk=True), [])
        for missing in (Guest.BLOCK_THROTTLE_LINE, Guest.BLOCK_SERVED_LINE):
            dropped = [line for line in lines if line != 'USER:' + missing]
            self.assertEqual(len(Guest.component_problems(dropped, echo=True, disk=True)), 1, missing)


class WaitTests(unittest.TestCase):
    """`Guest.wait` on a live console: the stream is complete with its plan and every point, in either order."""

    def setUp(self):
        self.ours, self.theirs = socket.socketpair()
        self.directory = tempfile.TemporaryDirectory()
        self.console = Console(self.ours, pathlib.Path(self.directory.name) / 'Debug.log')
        test = self

        class Context:
            console = self.console

            def wait_for_line(self, predicate, timeout, *, since=0):
                try:
                    return test.console.wait_for(predicate, timeout, since=since)
                except ConsoleTimeout as error:
                    raise CaseFailure(str(error)) from error
        self.context = Context()

    def tearDown(self):
        self.theirs.close()
        self.console.close()
        self.directory.cleanup()

    def send_later(self, *lines: str) -> threading.Thread:
        def send():
            for line in lines:
                self.theirs.sendall(b'\r\n' + line.encode() + b'\r\n')
        thread = threading.Thread(target=send)
        thread.start()
        return thread

    def test_the_plan_last_completes_the_stream(self):
        thread = self.send_later('ASTERIA:LAUNCH', 'USER:TAP version 14', 'USER:ok 1 - a', 'USER:ok 2 - b',
                                 'USER:1..2')
        Guest.wait(self.context, 5)
        thread.join()
        self.assertEqual(self.console.user_lines()[-1], '1..2')

    def test_the_last_point_completes_a_stream_with_its_plan_first(self):
        self.theirs.sendall(b'USER:TAP version 14\nUSER:1..2\nUSER:ok 1 - a\n')
        with self.assertRaises(CaseFailure):
            Guest.wait(self.context, 0.3)
        self.send_later('USER:ok 2 - b').join()
        Guest.wait(self.context, 5)

    def test_a_thread_line_does_not_hold_up_the_stream(self):
        thread = self.send_later('USER:# thread: done', 'USER:TAP version 14', 'USER:ok 1 - a', 'USER:1..1')
        Guest.wait(self.context, 5)
        thread.join()

    def test_a_kernel_failure_ends_the_wait(self):
        self.theirs.sendall(b'USER:TAP version 14\nASTERIA:PAGE-ALLOCATOR:FAIL\n')
        with self.assertRaises(CaseFailure) as raised:
            Guest.wait(self.context, 5)
        self.assertIn('kernel failure', str(raised.exception))


if __name__ == '__main__':
    unittest.main()
