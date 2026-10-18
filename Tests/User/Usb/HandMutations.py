#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Replay hand-selected USB1 mutants, restoring every source in finally.

Run after the host-debug build. Scratch/logs stay under this worktree's Build/.
Each mutant must compile and make the specified observable test fail.
"""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[3]
SCRATCH = ROOT / 'Build' / 'Tmp' / 'Usb1Mutations'
MUTANTS = (
    ('ohci-ownership-request', 'Ohci.cpp', 'm_mmio.Write32(8U, 1U << 3U);',
     'm_mmio.Write32(8U, 0U);', 'Ohci'),
    ('ohci-low-speed', 'Ohci.cpp',
     '(request.Speed == Input::UsbSpeed::Low ? 1U << 13U : 0U)', '0U', 'Ohci'),
    ('ohci-done-low-bit', 'Ohci.cpp',
     'Load32(m_memory.Hcca.Cpu + 0x84U) & ~UInt32{1U}',
     'Load32(m_memory.Hcca.Cpu + 0x84U)', 'Ohci'),
    ('ohci-stall-status', 'OhciStatus.cpp', 'status = Input::TransferStatus::Stall;',
     'status = Input::TransferStatus::Retryable;', 'Usb1Status'),
    ('uhci-legacy-status', 'Uhci.cpp', 'm_config.WriteConfig16(0xC0U, pirq | acknowledgements)',
     'm_config.WriteConfig16(0xC0U, pirq | (acknowledgements & 0U))', 'Uhci'),
    ('uhci-short-status', 'Uhci.cpp',
     'Store32(m_memory.Queues.Cpu + 20U, Address(m_memory.Descriptors.Address + m_scan * 16U));',
     'Store32(m_memory.Queues.Cpu + 20U, 1U);', 'Uhci'),
    ('uhci-babble-status', 'UhciStatus.cpp', 'result = Input::TransferStatus::Error;',
     'result = Input::TransferStatus::Stall;', 'Usb1Status'),
    ('uhci-toggle', 'Uhci.cpp', 'if (m_interrupt)\n            {\n                m_toggle = !m_toggle;\n            }',
     'if (m_interrupt)\n            {\n                m_toggle = m_toggle;\n            }', 'Uhci'),
    ('reset-assert-timing', 'Usb1.cpp', '(m_now - m_phaseAt) >= 50U',
     '(m_now - m_phaseAt) >= 1U', 'Uhci'),
    ('endpoint-halt-toggle', 'Usb1.cpp',
     'pipe.Toggle = false;\n                    }',
     'pipe.Toggle = true;\n                    }', 'Ohci'),
    ('ohci-nak-interval', 'Ohci.cpp', '(elapsed + 1U >= m_interval)',
     '(elapsed + 2U >= m_interval)', 'Ohci'),
    ('ohci-skipped-ed-reachability', 'Ohci.cpp',
     'm_mmio.Write32(4U, m_control | (m_interrupt ? 4U : 16U));',
     'm_mmio.Write32(4U, m_control);', 'Ohci'),
    ('uhci-unlink-cancel', 'Uhci.cpp',
     'Store32(m_memory.Frames.Cpu + i * 4U, 1U);',
     'Store32(m_memory.Frames.Cpu + i * 4U, Address(m_memory.Queues.Address + 16U) | 2U);', 'Uhci'),
    ('uhci-halt-cancel-barrier', 'Uhci.cpp', 'm_io.Write16(0U, 2U);',
     'm_io.Write16(0U, 0xC0U);', 'Uhci'),
    ('ohci-root-change-ack', 'Ohci.cpp',
     'm_mmio.Write32(PortOffset(port), value & 0x1F0000U);',
     'm_mmio.Write32(PortOffset(port), 0U);', 'Ohci'),
    ('uhci-root-change-ack', 'Uhci.cpp',
     'm_io.Write16(PortOffset(port), status & 0x124EU);',
     'm_io.Write16(PortOffset(port), status & 0x1244U);', 'Uhci'),
    ('uhci-nak-interval', 'Uhci.cpp', '(elapsed + 1U >= m_interval)',
     '(elapsed + 2U >= m_interval)', 'Uhci'),
)


def run(command, log):
    with log.open('w') as output:
        return subprocess.run(command, cwd=ROOT, stdout=output,
                              stderr=subprocess.STDOUT, check=False).returncode


def main():
    SCRATCH.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'Build' / 'Tmp')
    failed = []
    for name, filename, original, replacement, suite in MUTANTS:
        source = ROOT / 'Source' / 'User' / 'Usb' / filename
        saved = source.read_text()
        if saved.count(original) != 1:
            raise RuntimeError(f'{name}: mutation anchor is not unique')
        (SCRATCH / (filename + '.original')).write_text(saved)
        try:
            source.write_text(saved.replace(original, replacement))
            built = run(['cmake', '--build', '--preset', 'host-debug', '-j', '6',
                         '--target', f'User_Usb_{suite}Tests'], SCRATCH / (name + '-build.log'))
            tested = 0
            if built == 0:
                tested = run(['ctest', '--preset', 'host-debug', '--output-on-failure',
                              '-R', f'^User/Usb/{suite}Tests$'], SCRATCH / (name + '-test.log'))
            killed = built == 0 and tested != 0
            print(f'{name}: {"KILLED" if killed else "FAIL"}', flush=True)
            if not killed:
                failed.append(name)
        finally:
            source.write_text(saved)
    restored = run(['cmake', '--build', '--preset', 'host-debug', '-j', '6'],
                   SCRATCH / 'restored-build.log')
    tested = run(['ctest', '--preset', 'host-debug', '--output-on-failure', '-R',
                  '^(User/Usb/(Ohci|Uhci|Usb1Status)Tests|Fuzz/(Ohci|Uhci)StatusFuzz)$'],
                 SCRATCH / 'restored-test.log')
    if failed or restored != 0 or tested != 0:
        raise SystemExit(f'mutation failures: {failed}; restored build={restored}, test={tested}')
    print(f'{len(MUTANTS)}/{len(MUTANTS)} mutants killed; restored build/tests PASS', flush=True)


if __name__ == '__main__':
    main()
