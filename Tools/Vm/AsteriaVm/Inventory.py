# SPDX-License-Identifier: Apache-2.0
"""Boot-file and root-console inventory checked against QEMU's own PCI model."""
from __future__ import annotations

import pathlib
import re
import subprocess

from AsteriaVm.Observers import Devices
from AsteriaVm import AcpiDump, DiskRecipes, Guest

PCI = re.compile(r'^(?:PCI |# devices: )([0-9a-f]{4}):([0-9a-f]{2}):([0-9a-f]{2})\.([0-9a-f]) '
                 r'([0-9a-f]{4}):([0-9a-f]{4}) class=([0-9a-f]{2})/([0-9a-f]{2})/([0-9a-f]{2})\b')


def pci_problems(text: str, query: list[dict]) -> list[str]:
    reported = {}
    problems = []
    for line in text.splitlines():
        match = PCI.match(line)
        if match is None:
            continue
        segment, bus, slot, function, vendor, device, cls, sub, interface = (
            int(value, 16) for value in match.groups())
        key = (segment, bus, slot, function)
        if key in reported:
            problems.append('duplicate PCI function %s' % (key,))
        reported[key] = (vendor, device, cls << 8 | sub)
    expected = {(0, f.bus, f.slot, f.function): (f.vendor, f.device, f.class_code)
                for f in Devices.pci_functions(query)}
    for key, identity in expected.items():
        if key not in reported:
            problems.append('missing PCI function %s' % (key,))
        elif reported[key] != identity:
            problems.append('PCI function %s reports %s, QEMU has %s' % (key, reported[key], identity))
    for key in reported.keys() - expected.keys():
        problems.append('unexpected PCI function %s' % (key,))
    return problems


def xhci_report_problems(text: str, large: bool = False) -> list[str]:
    errors = []
    for field in ('CAPLENGTH', 'HCIVERSION', 'HCSPARAMS1', 'HCSPARAMS2', 'HCSPARAMS3',
                  'HCCPARAMS1', 'HCCPARAMS2', 'DBOFF', 'RTSOFF', 'PAGESIZE', 'USBSTS'):
        if field + '=0x' not in text:
            errors.append('missing xHCI register ' + field)
    if 'XHCI legacy' not in text:
        errors.append('missing xHCI legacy state or absence')
    if 'XHCI protocol' not in text:
        errors.append('missing xHCI supported protocols')
    if large:
        hcs = re.search(r'HCSPARAMS1=0x([0-9a-f]{8})', text)
        if hcs is None or int(hcs[1], 16) >> 24 != 30:
            errors.append('large controller does not report 30 root ports')
        protocols = re.findall(r'XHCI protocol.*revision=([23])\.[0-9a-f]{2} port-offset=(\d+) port-count=(\d+)', text)
        if sorted((major, int(count)) for major, _, count in protocols) != [('2', 15), ('3', 15)]:
            errors.append('large controller does not report 15 USB2 and 15 USB3 roots')
        connected = {int(port) for port in re.findall(r'XHCI port=(\d+) PORTSC=', text)}
        if not {29, 30}.issubset(connected):
            errors.append('high USB2 HID roots 29/30 are not connected in the firmware snapshot')
    return errors


def report_path(esp: pathlib.Path) -> pathlib.Path | None:
    # vvfat exports the firmware's names in lowercase on a case-sensitive host.
    return next((p for p in esp.rglob('*') if p.is_file()
                 and p.relative_to(esp).as_posix().casefold() == 'efi/asteria/report.txt'), None)


def record(context) -> None:
    query = context.qmp.execute('query-pci')
    path = report_path(context.run_directory / 'Esp')
    if path is None:
        # IA64's current firmware uses a read-only ESP (or an ISO for AHCI).
        # Early stop probes may stop before the firmware report runs.
        if context.case.isa == 'Ia64' or context.case.driver == 'Amd64StopEarly':
            context.check('inventory.report', True, skipped='firmware volume is read-only or boot stopped early')
        else:
            context.check('inventory.report', False, 'EFI/ASTERIA/REPORT.TXT was not exported to the case ESP')
    else:
        data = path.read_bytes()
        ascii_text = all(byte in (10, 13) or 32 <= byte <= 126 for byte in data)
        crlf = b'\n' not in data.replace(b'\r\n', b'')
        context.check('inventory.report-format', ascii_text and crlf and len(data) <= 65536,
                      'ASCII, CRLF and 64 KiB bound')
        context.check_problems('inventory.report-pci', pci_problems(data.decode('ascii', errors='replace'), query))
        if getattr(context.case, 'xhci', False):
            context.check_problems('inventory.xhci-registers', xhci_report_problems(
                data.decode('ascii', errors='replace'), getattr(context.case, 'xhci_large', False)))
        if context.case.isa == 'Amd64':
            context.check_problems('inventory.acpi-dump', AcpiDump.problems(
                context.run_directory / 'Esp', data.decode('ascii', errors='replace'), context.qmp.pmemsave))
    lines = Guest.channel_lines(context.console.lines)
    text = '\n'.join(line for line in lines if line.startswith('# devices:'))
    if getattr(context.case, 'xhci', False):
        context.check('inventory.xhci-publication', any(
            'class=0c/03/30' in line and 'publication=published step=0 err=0x00' in line for line in lines),
            'per-function publication outcome reaches the screen inventory')
    if context.case.guest and not context.case.expected_stop:
        context.check_problems('inventory.console-pci', pci_problems(text, query))
        context.check('inventory.console-input', any(line.startswith('# devices: input 8042=') for line in lines))
    if context.case.disk == 'fat32' and context.case.driver == 'Amd64Default':
        context.check('inventory.save-status', '# devices: saved DEVICES.TXT' in lines,
                      'the writable FAT32 case with ASTERIA.OK must save its inventory')
    if context.case.disk in ('fat12', 'fat32') and '# devices: saved DEVICES.TXT' in lines:
        result = subprocess.run([DiskRecipes.find_tool('mtype'), '-i', str(context.run_directory / 'Disk.raw'),
                                 '::DEVICES.TXT'], capture_output=True, env=DiskRecipes.mtools_environment(), timeout=30)
        expected = '\n'.join(line for line in lines if PCI.match(line) or line.startswith('# devices: disk ')
                             or line.startswith('# devices: input ') or line == '# devices: inventory truncated') + '\n'
        context.check('inventory.saved-file', result.returncode == 0
                      and result.stdout.decode('ascii', errors='replace').rstrip() == expected.rstrip(),
                      'DEVICES.TXT matches the console inventory (whole-page ASCII whitespace tail)')
