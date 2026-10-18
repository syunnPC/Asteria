#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Replay deadline/abort mutations against public controller/core regressions.

All compiled mutants must fail their selected CTest. Originals are restored in
finally; logs and saved files remain under this worktree's Build/Tmp.
"""
from pathlib import Path
import os

from HandMutations import ROOT, run

SCRATCH = ROOT / 'Build' / 'Tmp' / 'UsbAbortMutations'
MUTANTS = (
    ('core-idle-deadline', 'Input/UsbDevice.cpp', '.TimeoutMilliseconds = 0U};',
     '.TimeoutMilliseconds = 1000U};', 'Usb/Xhci'),
    ('xhci-stop-command', 'Usb/Xhci.cpp', 'Command(port, number, 15U, 0U,',
     'Command(port, number, 14U, 0U,', 'Usb/Xhci'),
    ('xhci-dequeue-cycle', 'Usb/Xhci.cpp', 'Command(port, number, 16U, ring.Dequeue(),',
     'Command(port, number, 16U, ring.Dequeue() & ~UInt64{1U},', 'Usb/Xhci'),
    ('xhci-late-event-completion', 'Usb/Xhci.cpp',
     '// the request or start endpoint recovery a second time.\n        return;',
     '// mutant: late event releases the borrow before dequeue.\n        Finish(port, Input::TransferStatus::Success);\n        return;', 'Usb/Xhci'),
    ('xhci-event-drain', 'Usb/Xhci.cpp',
     '(Load32(m_memory.EventRing.Cpu + m_eventIndex * 16U + 12U) & 1U) != m_eventCycle',
     'port.Busy', 'Usb/Xhci'),
    ('ehci-unlink-async', 'Usb/Ehci.cpp',
     'Store32(m_memory.AsyncHead.Cpu, Link(m_memory.AsyncHead.Address) | 2U);',
     'Store32(m_memory.AsyncHead.Cpu, Link(m_memory.Queue.Address) | 2U);', 'Usb/Ehci'),
    ('ehci-advance-doorbell', 'Usb/Ehci.cpp',
     'm_mmio.Read32(m_operational) | (1U << 6U)',
     'm_mmio.Read32(m_operational)', 'Usb/Ehci'),
    ('ehci-stale-iaa', 'Usb/Ehci.cpp',
     'm_mmio.Write32(m_operational + 4U, 1U << 5U); // discard stale IAA',
     'm_mmio.Write32(m_operational + 4U, 0U); // retain stale IAA', 'Usb/Ehci'),
    ('ehci-periodic-frame', 'Usb/Ehci.cpp',
     '((frame - m_unlinkFrame) & 0x7FFU) >= 2U',
     '(frame == m_unlinkFrame) || (((frame - m_unlinkFrame) & 0x7FFU) >= 2U)', 'Usb/Ehci'),
    ('ehci-timeout-stale-snapshot', 'Usb/Ehci.cpp',
     'return; // IAA sampled before this unlink cannot acknowledge its doorbell.',
     'static_cast<void>(controllerStatus); // old IAA incorrectly reused', 'Usb/Ehci'),
    ('ehci-tt-timeout-halt', 'Usb/Ehci.cpp',
     '        if (reset)\n        {\n            PortWrite',
     '        if (m_recovering)\n        {\n            Fail(Input::TransferStatus::Error);\n            return;\n        }\n        if (reset)\n        {\n            PortWrite', 'Usb/Ehci'),
    ('ehci-cancel-status-loss', 'Usb/Ehci.cpp',
     'Retire(m_recovering && (m_recoveryStatus == Input::TransferStatus::Cancelled)\n            ? Input::TransferStatus::Cancelled : Input::TransferStatus::TimedOut);',
     'Retire(Input::TransferStatus::TimedOut);', 'Usb/Ehci'),
    ('ohci-abort-skip', 'Usb/Ohci.cpp',
     'Store32(ed, Load32(ed) | (1U << 14U));',
     'Store32(ed, Load32(ed));', 'Usb/Ohci'),
    ('ohci-hardware-done-drain', 'Usb/Ohci.cpp',
     '(m_mmio.Read32(0x30U) != 0U)', '(m_offFrame == 0xFFFFFFFFU)', 'Usb/Ohci'),
    ('uhci-abort-unlink', 'Usb/Uhci.cpp',
     'Store32(m_memory.Frames.Cpu + i * 4U, 1U);',
     'Store32(m_memory.Frames.Cpu + i * 4U, Address(m_memory.Queues.Address + 16U) | 2U);', 'Usb/Uhci'),
    ('uhci-unlink-frame-wait', 'Usb/Uhci.cpp',
     '(m_interrupt && (m_scan == m_count)) ? 2U : 33U',
     '(m_interrupt && (m_scan == m_count)) ? 2U : 1U', 'Usb/Uhci'),
)


def main():
    SCRATCH.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'Build' / 'Tmp')
    failed = []
    try:
        for name, filename, original, replacement, suite in MUTANTS:
            source = ROOT / 'Source' / 'User' / filename
            saved = source.read_text()
            if saved.count(original) != 1:
                raise RuntimeError(f'{name}: mutation anchor is not unique')
            (SCRATCH / (name + '.original')).write_text(saved)
            try:
                source.write_text(saved.replace(original, replacement))
                target = 'User_' + suite.replace('/', '_') + 'Tests'
                built = run(['cmake', '--build', '--preset', 'host-debug', '-j', '6',
                             '--target', target], SCRATCH / (name + '-build.log'))
                tested = 0
                if built == 0:
                    tested = run(['ctest', '--preset', 'host-debug', '--output-on-failure',
                                  '-R', f'^User/{suite}Tests$'], SCRATCH / (name + '-test.log'))
                killed = built == 0 and tested != 0
                print(f'{name}: {"KILLED" if killed else "FAIL"}', flush=True)
                if not killed:
                    failed.append(name)
            finally:
                source.write_text(saved)
    finally:
        restored = run(['cmake', '--build', '--preset', 'host-debug', '-j', '6'],
                       SCRATCH / 'restored-build.log')
        tested = run(['ctest', '--preset', 'host-debug', '--output-on-failure', '-R',
                      '^(User/Usb/.*Tests|User/Input/UsbDeviceTests)$'], SCRATCH / 'restored-test.log')
    if failed or restored != 0 or tested != 0:
        raise SystemExit(f'mutation failures: {failed}; restored build={restored}, test={tested}')
    print(f'{len(MUTANTS)}/{len(MUTANTS)} mutants killed; restored build/tests PASS', flush=True)


if __name__ == '__main__':
    main()
