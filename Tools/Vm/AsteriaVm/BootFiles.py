# SPDX-License-Identifier: Apache-2.0
"""Whole-file boot provenance, checked against each run's copied ESP."""
from __future__ import annotations

import importlib
import pathlib

from AsteriaVm import Evidence
from AsteriaVerify import BootFiles as Manifest

KEYS = ('init', 'user', 'spinner')
PREFIX = 'ASTERIA:STAGE:BOOT-FILE:'


def problems(region, expected: dict[str, tuple[str, int, int]], lines: list[str]) -> list[str]:
    found = []
    if region.PhysicalStart == 0 or region.PhysicalStart % 4096 or region.PageCount == 0:
        found.append('invalid LoaderData receipt')
    if not 0 < region.ManifestBytes <= Manifest.MAXIMUM_MANIFEST_BYTES:
        found.append('invalid retained manifest size')
    end = 4096
    events = [line for line in lines if line.startswith(PREFIX)]
    wanted = []
    for key, record in zip(KEYS, region.Files, strict=True):
        name = bytes(record.Name).split(b'\0', 1)[0].decode('ascii', errors='replace')
        actual = name, record.SizeBytes, record.Crc32c
        if any(record.Reserved):
            found.append('%s: nonzero reserved provenance bytes' % key)
        if key not in expected:
            if any(actual) or record.Offset:
                found.append('%s: unexpected file' % key)
            continue
        if actual != expected[key]:
            found.append('%s: recorded %r, ESP has %r' % (key, actual, expected[key]))
        if record.Offset < end or record.Offset % 16 or record.Offset + record.SizeBytes > region.PageCount * 4096:
            found.append('%s: file outside ordered region slices' % key)
        end = record.Offset + record.SizeBytes
        wanted.append('%s%s size=%d crc32c=0x%08x' % ((PREFIX,) + expected[key]))
    module_events = [line for line in lines if line.startswith('ASTERIA:STAGE:BOOT-MODULE:')]
    module_expected = {key: value for key, value in expected.items() if key.startswith('module:')}
    if region.ModuleCount != len(module_expected) or region.ModuleCount > Manifest.MAXIMUM_MODULES:
        found.append('module count differs from ESP')
    if region.ModuleOffset % 4096 or region.ModuleOffset < end or region.ModuleBytes % 4096:
        found.append('module table is not page isolated')
    if region.ModuleBytes == 0 or region.ModuleBytes > 2 * 1024 * 1024:
        found.append('module table size out of bounds')
    if region.ModuleOffset + region.ModuleBytes > region.PageCount * 4096:
        found.append('module table outside loader receipt')
    end = region.ModuleOffset + 16 + region.ModuleCount * 32
    module_wanted = []
    for module, (key, value) in zip(region.Modules[:region.ModuleCount], module_expected.items()):
        logical = bytes(module.Name).split(b'\0', 1)[0].decode('ascii', errors='replace')
        record = module.File
        name = bytes(record.Name).split(b'\0', 1)[0].decode('ascii', errors='replace')
        if (logical != key[7:] or ('MODULES/' + name, record.SizeBytes, record.Crc32c) != value
                or any(record.Reserved)):
            found.append('%s: module provenance differs from ESP' % key)
        if (record.Offset % 16 or record.Offset < end
                or record.Offset + record.SizeBytes > region.ModuleOffset + region.ModuleBytes):
            found.append('%s: module outside ordered table slice' % key)
        end = record.Offset + record.SizeBytes
        module_wanted.append('ASTERIA:STAGE:BOOT-MODULE:%s %s size=%d crc32c=0x%08x' %
                             (logical, name, record.SizeBytes, record.Crc32c))
    if module_events != module_wanted:
        found.append('module stage events differ from ESP')
    if 'kernel' not in expected:
        found.append('missing kernel file provenance')
    else:
        wanted.append('%s%s size=%d crc32c=0x%08x' % ((PREFIX,) + expected['kernel']))
    if events != wanted:
        found.append('file stage events %r, expected %r' % (events, wanted))
    return found


def record(context) -> None:
    directory = context.run_directory / 'Esp' / 'EFI' / 'ASTERIA'
    entries = Manifest.manifest((directory / 'BOOT.CFG').read_bytes())
    expected = {}
    for key, (name, crc) in entries.items():
        data = (directory / name).read_bytes()
        if Manifest.crc32c(data) != crc:
            context.check('host.boot-files', False, '%s: ESP CRC mismatch' % name)
            return
        expected[key] = name, len(data), crc
    observer = importlib.import_module('AsteriaVm.Observers.%sEvidence' % context.case.isa)
    schema = observer.schema(pathlib.Path(context.target.root))
    address, reasons = Evidence.locate(context.console.lines)
    if address is None:
        context.check_problems('host.boot-files', reasons)
        return
    data = schema.read(context.qmp, address)
    reasons = schema.header_problems(data)
    if not reasons:
        reasons = problems(schema.decode(data).BootFiles, expected, context.console.lines)
    context.check_problems('host.boot-files', reasons)
