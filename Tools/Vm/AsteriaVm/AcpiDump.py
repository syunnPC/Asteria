# SPDX-License-Identifier: Apache-2.0
"""ACPI files compared with the firmware bytes through QMP physical reads."""
from __future__ import annotations

import collections
import pathlib
import re

TABLE = re.compile(r'^ACPI ([!-~]{4}) OEM-table=(.{8}) address=0x([0-9a-f]{16}) '
                   r'length=(\d+) revision=(\d+) checksum=(valid|BAD)$', re.MULTILINE)
REQUIRED = {'DSDT', 'FACP', 'APIC'}


def problems(esp: pathlib.Path, report: str, read) -> list[str]:
    """Require the q35 tables and compare every validated SDT byte for byte.

    QMP has no portable ACPI-table query. REPORT.TXT supplies physical addresses;
    independent QMP reads prove file content, header fields and checksum.
    """
    tables = [match.groups() for match in TABLE.finditer(report.replace('\r\n', '\n'))]
    valid = [table for table in tables if table[-1] == 'valid']
    errors = ['missing recorded %s' % signature for signature in sorted(REQUIRED - {t[0] for t in valid})]
    counts = collections.Counter(t[0] for t in valid)
    ordinals = collections.Counter()
    files = {p.relative_to(esp).as_posix().casefold(): p for p in esp.rglob('*') if p.is_file()}
    for signature, oem, address, length, revision, _ in valid:
        ordinals[signature] += 1
        suffix = str(ordinals[signature]) if counts[signature] > 1 else ''
        name = '%s%s.AML' % (signature, suffix)
        path = files.get(('efi/asteria/acpi/' + name).casefold())
        if path is None:
            errors.append('missing dump %s' % name)
            continue
        size = int(length)
        if not 36 <= size <= 1024 * 1024:
            errors.append('%s recorded length outside bounds' % name)
            continue
        data = path.read_bytes()
        if (len(data) != size or data[:4] != signature.encode('ascii')
                or int.from_bytes(data[4:8], 'little') != size or data[8] != int(revision)
                or data[16:24] != oem.encode('ascii') or sum(data) % 256 != 0):
            errors.append('%s header/length/checksum differs from parser record' % name)
        if data != read(int(address, 16), size):
            errors.append('%s differs from guest physical table bytes' % name)
    if not re.search(r'^ACPI dump saved=\d+ failed=0$', report.replace('\r\n', '\n'), re.MULTILINE):
        errors.append('dump completion did not report zero failures')
    return errors
