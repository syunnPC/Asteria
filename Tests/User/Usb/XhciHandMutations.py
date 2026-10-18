#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile hand mutations of descriptor binding, endpoint ownership and 0.96 support."""
from pathlib import Path
import os

from HandMutations import ROOT, run

SCRATCH = ROOT / 'Build' / 'Tmp' / 'XhciPipeMutations'
MUTANTS = (
    ('descriptor-endpoint-owner', 'Input/UsbConfiguration.cpp',
     '(owner == interfaceAt) || (bytes[owner + 2U] != bytes[interfaceAt + 2U])',
     '(owner == interfaceAt)', 'User/Input/UsbConfigurationTests'),
    ('alternate-binding', 'Input/UsbConfiguration.cpp', 'if (interface.Alternate != 0U)',
     'if (interface.Alternate == 255U)', 'User/Input/UsbConfigurationTests'),
    ('bot-subclass', 'Input/UsbConfiguration.cpp', '(interface.Subclass == 6U)',
     '(interface.Subclass == 5U)', 'User/Input/UsbConfigurationTests'),
    ('bios-ownership', 'Usb/Xhci.cpp', 'm_mmio.WriteOsOwned(offset);',
     'static_cast<void>(offset);', 'User/Usb/XhciCompatibilityTests'),
    ('legacy-smi-enables', 'Usb/Xhci.cpp', '(control & ~0xE000E011U) | 0xE0000000U',
     '(control & ~0xE0000000U) | 0xE0000000U', 'User/Usb/XhciCompatibilityTests'),
    ('reject-096', 'Usb/Xhci.cpp', '((cap >> 16U) != 0x96U)',
     '((cap >> 16U) != 0x100U)', 'User/Usb/XhciCompatibilityTests'),
    ('context-size', 'Usb/Xhci.cpp', 'm_contextBytes = (hcc & 4U) != 0U ? 64U : 32U;',
     'm_contextBytes = (hcc & 4U) != 0U ? 32U : 32U;', 'User/Usb/XhciTests'),
    ('scratchpad-high-count', 'Usb/Xhci.cpp',
     '((hcs2 >> 27U) & 31U) | (((hcs2 >> 21U) & 31U) << 5U)',
     '((hcs2 >> 27U) & 31U)', 'User/Usb/XhciTests'),
    ('bulk-out-context', 'Usb/Xhci.cpp', 'const UInt32 bulkType = in ? 6U : 2U;',
     'const UInt32 bulkType = in ? 6U : 6U;', 'User/Usb/XhciPipeTests'),
    ('bulk-esit-payload', 'Usb/Xhci.cpp', 'payload = 0U; // ESIT payload applies only to periodic endpoints.',
     'payload = packet; // Incorrect periodic payload on bulk.', 'User/Usb/XhciPipeTests'),
    ('context-entries', 'Usb/Xhci.cpp', 'oldEntries > port.Endpoint ? oldEntries : port.Endpoint',
     'oldEntries > port.Endpoint ? UInt32{port.Endpoint} : UInt32{port.Endpoint}', 'User/Usb/XhciPipeTests'),
    ('pipe-completion-route', 'Usb/Xhci.cpp', '(pipe.ActiveEndpoint == event.Endpoint)',
     '(pipe.ActiveEndpoint == 1U)', 'User/Usb/XhciPipeTests'),
    ('abort-drain', 'Usb/Xhci.cpp',
     '(Load32(m_memory.EventRing.Cpu + m_eventIndex * 16U + 12U) & 1U) != m_eventCycle',
     '(Load32(m_memory.EventRing.Cpu + m_eventIndex * 16U + 12U) & 1U) == m_eventCycle', 'User/Usb/XhciPipeTests'),
    ('idle-deadline', 'Input/UsbDevice.cpp', '.TimeoutMilliseconds = 0U};',
     '.TimeoutMilliseconds = 1000U};', 'User/Usb/XhciPipeTests'),
    ('mouse-set-idle', 'Input/UsbDevice.cpp', 'm_state = UsbState::SetIdle;',
     'm_state = m_interface.Protocol == BootProtocol::Mouse ? UsbState::ConfigureEndpoint : UsbState::SetIdle;',
     'User/Usb/XhciPipeTests'),
)


def main():
    SCRATCH.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'Build' / 'Tmp')
    failed = []
    for name, filename, original, replacement, suite in MUTANTS:
        source = ROOT / 'Source' / 'User' / filename
        saved = source.read_text()
        if saved.count(original) != 1:
            raise RuntimeError(f'{name}: mutation anchor is not unique')
        (SCRATCH / (name + '.original')).write_text(saved)
        try:
            source.write_text(saved.replace(original, replacement))
            target = suite.replace('/', '_')
            built = run(['cmake', '--build', '--preset', 'host-debug', '-j', '6', '--target', target],
                        SCRATCH / (name + '-build.log'))
            tested = 0
            if built == 0:
                tested = run(['ctest', '--preset', 'host-debug', '--output-on-failure', '-R', f'^{suite}$'],
                             SCRATCH / (name + '-test.log'))
            killed = built == 0 and tested != 0
            print(f'{name}: {"KILLED" if killed else "FAIL"}', flush=True)
            if not killed:
                failed.append(name)
        finally:
            source.write_text(saved)
    baseline = run(['cmake', '--build', '--preset', 'host-debug', '-j', '6'], SCRATCH / 'baseline-build.log')
    if baseline == 0:
        baseline = run(['ctest', '--preset', 'host-debug', '--output-on-failure', '-R', 'UsbConfiguration|UsbDevice|Xhci'],
                       SCRATCH / 'baseline-test.log')
    if failed or baseline != 0:
        raise RuntimeError(f'failed mutants={failed}; baseline={baseline}')
    print(f'{len(MUTANTS)}/{len(MUTANTS)} compiled mutants killed; restored baseline passed', flush=True)


if __name__ == '__main__':
    main()
