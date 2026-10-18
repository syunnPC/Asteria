#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile hand mutations of xHCI enumeration and diagnostics."""
from pathlib import Path
import os
import sys

from HandMutations import ROOT, run

SCRATCH = ROOT / 'Build' / 'Tmp' / 'XhciHardwareMutations'
MUTANTS = (
    ('speed-gates-connection', 'Usb/Xhci.cpp',
     'const bool connected = ((status & 1U) != 0U) && !port.PoweredOff;',
     'const bool connected = ((status & 1U) != 0U) && valid && !port.PoweredOff;', 'User/Usb/XhciHardwareTests'),
    ('ped-alone-completes-reset', 'Usb/Xhci.cpp',
     'if (port.ResetSeen && ((status & 2U) != 0U) && ((status & 0x80000010U) == 0U))',
     'if (((status & 2U) != 0U) && ((status & 0x80000010U) == 0U))', 'User/Usb/XhciHardwareTests'),
    ('no-attach-debounce', 'Usb/Xhci.cpp',
     '(m_now - m_attachedAt[index] < AttachDebounceMs)', '(m_now - m_attachedAt[index] < 1U)',
     'User/Usb/XhciHardwareTests'),
    ('undecodable-reset-speed-addressed', 'Usb/Xhci.cpp',
     'Finish(port, Input::TransferStatus::Error);\n                break;\n            }\n            // ScanPort',
     'static_cast<void>(valid);\n            }\n            // ScanPort', 'User/Usb/XhciHardwareTests'),
    ('ep0-stall-not-reset', 'Usb/Xhci.cpp',
     'port.StallRecovery = true;\n            port.Stage = Phase::ResetEndpoint;\n            return;',
     'Finish(port, Input::TransferStatus::Stall);\n            return;', 'User/Usb/XhciHardwareTests'),
    ('babble-fatal', 'Usb/Xhci.cpp', 'if ((event.Code == 4U) || (event.Code == 3U))', 'if (event.Code == 4U)',
     'User/Usb/XhciHardwareTests'),
    ('setup-trt-on-096', 'Usb/Xhci.cpp',
     '(length == 0U) || (m_version < 0x100U) ? 0U : (in ? 3U : 2U)', 'length == 0U ? 0U : (in ? 3U : 2U)',
     'User/Usb/XhciHardwareTests'),
    ('evaluate-minimal-context', 'Usb/Xhci.cpp',
     'Store32(ep0, Load32(ep0) & ~7U);\n        Store32(ep0 + 4U, (Load32(ep0 + 4U) & 0xFFFFU) | (UInt32{request.PacketSize0} << 16U));',
     'Clear({.Address = 0U, .Cpu = ep0, .Bytes = 20U});\n        Store32(ep0 + 4U, UInt32{request.PacketSize0} << 16U);',
     'User/Usb/XhciHardwareTests'),
    ('set-idle-stall-fatal', 'Input/UsbDevice.cpp', 'if (m_state == UsbState::SetIdle)\n        {',
     'if (m_state == UsbState::Failed)\n        {', 'User/Usb/XhciHardwareTests'),
    ('set-protocol-stall-fatal', 'Input/UsbDevice.cpp', 'if (m_state == UsbState::SetProtocol)\n        {',
     'if (m_state == UsbState::Failed)\n        {', 'User/Usb/XhciHardwareTests'),
    ('first-report-not-recorded', 'Input/UsbDevice.cpp', 'if (!pipe.Reported || rejected)',
     'if (rejected)', 'User/Usb/XhciHardwareTests'),
    ('connect-record-speed-protocol', 'Usb/Xhci.cpp', 'root.SuperPort ? 3U : 2U, 0U, status);',
     'root.SuperPort ? 3U : 3U, 0U, status);', 'User/Usb/XhciHardwareTests'),
    ('endpoint-record-payload', 'Usb/Xhci.cpp',
     '(Load32(ep + 4U) >> 16U) | (Load32(ep + 16U) & 0xFFFF0000U)', '(Load32(ep + 4U) >> 16U)',
     'User/Usb/XhciHardwareTests'),
    ('interface-record-skipped', 'Input/UsbDevice.cpp', 'if (configuration.Interface(i, shown) && (shown.Alternate == 0U))',
     'if (configuration.Interface(i, shown) && (bound == BindingError::None))', 'User/Usb/XhciHardwareTests'),
    ('progress-port-budget', 'Component/XhciProgress.cpp', 'if ((record.Port != 0U) && (port > PortRecords))',
     'if ((record.Port != 0U) && (port > TotalRecords))', 'User/Component/XhciProgressTests'),
    ('progress-ring-loss', 'Component/XhciProgress.cpp', '++m_lost;', 'static_cast<void>(m_lost);',
     'User/Component/XhciProgressTests'),
    ('progress-keeps-refused', 'Component/XhciProgress.cpp',
     'if ((error == Abi::SystemCallError::WouldBlock) || (error == Abi::SystemCallError::ResourceExhausted))',
     'if (error == Abi::SystemCallError::ResourceExhausted)', 'User/Component/XhciProgressTests'),
    ('format-portsc', 'Pager/UsbProgress.cpp', 'text.Hex(value, 8U);', 'text.Hex(value, 4U);',
     'User/Pager/UsbProgressTests'),
    ('terminal-erases-partial-line', 'Pager/RootContext.cpp', 'for (Foundation::Size left{partial}; left != 0U;)',
     'for (Foundation::Size left{0U}; left != 0U;)', 'User/Pager/InputTerminalTests'),
    ('terminal-partial-line-published', 'Pager/InputTerminal.cpp', 'context.Display->PartialBytes = g_length;',
     'context.Display->PartialBytes = 0U;', 'User/Pager/InputTerminalTests'),
)


def main():
    SCRATCH.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'Build' / 'Tmp')
    failed = []
    selected = [mutant for mutant in MUTANTS if len(sys.argv) < 2 or mutant[0] in sys.argv[1:]]
    for name, filename, original, replacement, suite in selected:
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
        baseline = run(['ctest', '--preset', 'host-debug', '--output-on-failure', '-R',
                        'Xhci|UsbDevice|UsbProgress|InputTerminal'], SCRATCH / 'baseline-test.log')
    if failed or baseline != 0:
        raise RuntimeError(f'failed mutants={failed}; baseline={baseline}')
    print(f'{len(selected)}/{len(selected)} compiled mutants killed; restored baseline passed', flush=True)


if __name__ == '__main__':
    main()
