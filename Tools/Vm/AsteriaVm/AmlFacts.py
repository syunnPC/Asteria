# SPDX-License-Identifier: Apache-2.0
"""Boot AML facts checked against identities/resources and the shared host formatter.

Only case-local firmware dumps are read. Third-party AML is never a fixture.
"""
from __future__ import annotations

import collections
import pathlib
import re
import subprocess

from AsteriaVm import AcpiDump


def files(esp: pathlib.Path) -> dict[str, pathlib.Path]:
    return {p.relative_to(esp).as_posix().casefold(): p for p in esp.rglob('*') if p.is_file()}


def definition_blocks(esp: pathlib.Path, report: str) -> list[pathlib.Path]:
    """DSDT first, then SSDTs in the report's firmware table order (numeric suffixes)."""
    records = [m.groups() for m in AcpiDump.TABLE.finditer(report.replace('\r\n', '\n'))]
    valid = [r for r in records if r[-1] == 'valid']
    counts = collections.Counter(r[0] for r in valid)
    ordinals = collections.Counter()
    paths = files(esp)
    dsdt = []
    ssdts = []
    for signature, *_ in valid:
        ordinals[signature] += 1
        suffix = str(ordinals[signature]) if counts[signature] > 1 else ''
        if signature in ('DSDT', 'SSDT'):
            path = paths['efi/asteria/acpi/%s%s.aml' % (signature.lower(), suffix)]
            (dsdt if signature == 'DSDT' else ssdts).append(path)
    return dsdt[:1] + ssdts


def identity_problems(data: bytes, isa: str, i8042: bool | None = None) -> list[str]:
    """q35 and itanium2-vpc both declare a PCI root and the legacy PS/2 pair."""
    text = data.decode('ascii', errors='replace')
    devices = re.findall(r'^device (.*?)(?=^device |^_S5=)', text, re.MULTILINE | re.DOTALL)
    errors = []
    root_id = 'PNP0A08' if isa == 'Amd64' else 'PNP0A03'
    if not any(root_id in d and 'pci-root=yes' in d and '_STA=present' in d
               and '_PRT=128 entries' in d for d in devices):
        errors.append('missing present PCI root %s with 128 routes' % root_id)
    ps2_status = '_STA=present' if (isa == 'Amd64' if i8042 is None else i8042) else '_STA=absent'
    keyboard = next((d for d in devices if 'PNP0303' in d), '')
    mouse = next((d for d in devices if 'PNP0F13' in d), '')
    if not all(token in keyboard for token in (ps2_status, 'keyboard=yes', 'min=0x60 max=0x60 length=0x1',
                                             'min=0x64 max=0x64 length=0x1', 'resource IRQ mask=0x2')):
        errors.append('missing PNP0303 keyboard status/ports 0x60/0x64 IRQ 1')
    if not all(token in mouse for token in (ps2_status, 'mouse=yes', 'resource IRQ mask=0x1000')):
        errors.append('missing PNP0F13 mouse status/IRQ 12')
    return errors


def record(context) -> None:
    if not context.case.aml:
        return
    esp = context.run_directory / 'Esp'
    if context.case.isa == 'Ia64':
        # The readonly EFI file driver is bypassed through Block I/O. Read the
        # case's raw ESP after the driver paused the guest, without synthesizing
        # any firmware output in the host directory.
        import os
        image = context.run_directory / 'BootMedia/Asteria-mbr.img'
        env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
        result = subprocess.run(['mcopy', '-s', '-o', '-i', str(image) + '@@1048576',
                                 '::/EFI/ASTERIA', str(esp / 'EFI')], capture_output=True, env=env, timeout=30)
        context.check('aml.esp-extract', result.returncode == 0, result.stderr.decode(errors='replace'))
    paths = files(esp)
    report = paths.get('efi/asteria/report.txt')
    facts = paths.get('efi/asteria/acpi.txt')
    context.check('aml.report', report is not None, 'REPORT.TXT exists on writable ESP')
    if report is None:
        return
    text = report.read_text('ascii')
    if context.case.aml == 'skip':
        context.check('aml.noaml', facts is None and 'AML skipped: NOAML' in text,
                      'NOAML marker skips arena/evaluation/file creation while boot succeeds')
        context.check('aml.dump-before-skip', bool(definition_blocks(esp, text)), 'raw tables still dumped')
        return
    context.check('aml.file', facts is not None, 'ACPI.TXT exists on writable ESP')
    if facts is None:
        return
    data = facts.read_bytes()
    context.check_problems('aml.identities', identity_problems(data, context.case.isa, context.case.i8042))
    context.check_problems('aml.raw-tables', AcpiDump.problems(esp, text, context.qmp.pmemsave))
    tool = pathlib.Path(__file__).resolve().parents[3] / 'Build/CMake/host-debug/Tools/AmlFacts'
    arguments = ['--reduced-arena'] if b'arena reduced node-budget=' in data else []
    result = subprocess.run([str(tool), *arguments, *map(str, definition_blocks(esp, text))],
                            capture_output=True, timeout=context.seconds(120))
    context.check('aml.host-equality', result.returncode in (0, 1) and result.stdout == data,
                  'ACPI.TXT equals AmlFacts over dumped DSDT/SSDTs byte for byte; ' + result.stderr.decode(errors='replace'))
    context.check('aml.timing', re.search(r'AML saved ACPI.TXT .*elapsed-ticks=\d+ elapsed-us=\d+', text) is not None,
                  'step outcome includes arena, summary and calibrated elapsed time')
