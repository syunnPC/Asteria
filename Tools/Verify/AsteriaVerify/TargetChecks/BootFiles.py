# SPDX-License-Identifier: Apache-2.0
"""Validate module files installed on the ESP."""
from __future__ import annotations

import dataclasses

from AsteriaVerify import BootFiles
from AsteriaVerify.Elf import load, SHT_SYMTAB
from AsteriaVerify.Checks import Property, register


@dataclasses.dataclass(frozen=True)
class ManifestFacts:
    manifest: bytes
    files: dict[str, tuple[int, int]]  # name -> whole-file size, CRC-32C
    spinner: bool
    modules: dict[str, str]
    unstripped: tuple[str, ...]
    embedded: tuple[str, ...]


@register
class FirmwareEspManifest(Property):
    """Every firmware ESP has a version-1 manifest, required images and matching
    whole-file CRC-32Cs. The AMD64 test ESP additionally carries the spinner."""
    variants = ('firmware', 'firmware-test')

    def extract(self, context, variant):
        directory = context.artifact('%s:esp' % variant) / 'EFI' / 'ASTERIA'
        manifest = directory / 'BOOT.CFG'
        files = {}
        for path in directory.rglob('*'):
            if not path.is_file() or path.suffix not in ('.ELF', '.OTF'): continue
            data = path.read_bytes()
            files[path.relative_to(directory).as_posix()] = len(data), BootFiles.crc32c(data)
        return ManifestFacts(manifest.read_bytes() if manifest.is_file() else b'', files,
                             context.isa == 'Amd64' and variant == 'firmware-test',
                             {'module:ui-bold': 'MODULES/VEGURB.OTF', 'module:ui-regular': 'MODULES/VEGURR.OTF',
                              'module:component': 'MODULES/COMP.ELF', 'module:directory': 'MODULES/DIR.ELF',
                              'module:toolkit': 'MODULES/TOOLKIT.ELF', 'module:usb': 'MODULES/USB.ELF', 'module:display': 'MODULES/DISPLAY.ELF',
                              **({'module:compositor': 'MODULES/COMPOSIT.ELF', 'module:auth': 'MODULES/AUTH.ELF', 'module:login': 'MODULES/LOGIN.ELF'} if variant == 'firmware' else {}),
                              **({'module:compositor': 'MODULES/COMPOSIT.ELF', 'module:pattern': 'MODULES/PATTERN.ELF', 'module:input-seat': 'MODULES/SEAT.ELF', 'module:face-probe': 'MODULES/FACEPROB.ELF', 'module:client': 'MODULES/CLIENT.ELF',
                                  'module:' + ('ia64' if context.isa == 'Amd64' else 'amd64') + '-stub': 'MODULES/INTERP.ELF',
                                  'module:' + ('ia64' if context.isa == 'Amd64' else 'amd64') + '-sample': 'MODULES/' + ('IA64' if context.isa == 'Amd64' else 'AMD64') + '.ELF',
                                  'module:aarch64-sample': 'MODULES/AARCH64.ELF',
                                  'module:pe-sample': 'MODULES/PE.ELF'} if variant == 'firmware-test' else {})},
                             tuple(name for name in ('COMP', 'DIR', 'CLIENT', 'INTERP', 'USB', 'DISPLAY', 'FACEPROB', 'COMPOSIT', 'PATTERN', 'SEAT', 'TOOLKIT')
                                   if (directory / 'MODULES' / (name + '.ELF')).is_file()
                                   and any(section.type == SHT_SYMTAB for section in
                                           load(directory / 'MODULES' / (name + '.ELF')).sections)),
                             tuple(symbol.name for symbol in load(context.artifact('%s:pager_elf' % variant)).symbols
                                   if symbol.name.startswith('AsteriaComponentImage')) +
                             tuple(symbol.name for symbol in load(context.artifact('%s:loader_elf' % variant)).symbols
                                   if symbol.name.startswith('AsteriaEmbeddedKernel')))

    def verify(self, facts):
        try:
            entries = BootFiles.manifest(facts.manifest)
        except ValueError as error:
            return [str(error)]
        required = {'kernel': 'KERNEL.ELF', 'init': 'INIT.ELF', 'user': 'USER.ELF'}
        if facts.spinner:
            required['spinner'] = 'SPINNER.ELF'
        required.update(facts.modules)
        problems = []
        if facts.unstripped:
            problems.append('module ELFs retain symbols: ' + ', '.join(facts.unstripped))
        if facts.embedded:
            problems.append('firmware retains embedded payload symbols')
        if {key: value[0] for key, value in entries.items()} != required:
            problems.append('installed manifest does not select the preset image set')
        for key, (name, crc) in entries.items():
            if name not in facts.files:
                problems.append('%s: missing %s' % (key, name))
                continue
            size, actual_crc = facts.files[name]
            maximum = BootFiles.MAXIMUM_MODULE_BYTES if key.startswith('module:') else BootFiles.MAXIMUM_FILE_BYTES
            if size == 0 or size > maximum:
                problems.append('%s: invalid file size %d' % (name, size))
            if actual_crc != crc:
                problems.append('%s: CRC-32C mismatch' % name)
        table_bytes = 16 + 32 * len(facts.modules) + sum((facts.files[name][0] + 15) & ~15
                                                       for name in facts.modules.values() if name in facts.files)
        if (table_bytes + 4095) & ~4095 > BootFiles.MAXIMUM_MODULE_BYTES:
            problems.append('module table exceeds fixed kernel range')
        return problems

    def mutants(self, facts):
        for name, value in (('absent manifest', b''), ('wrong version', facts.manifest.replace(b'version = 1', b'version = 2')),
                            ('unknown key', facts.manifest + b'unknown = 1\n'),
                            ('duplicate key', facts.manifest + facts.manifest.splitlines(keepends=True)[1]),
                            ('oversized manifest', b'#' * 4097)):
            yield name, dataclasses.replace(facts, manifest=value)
        yield 'unstripped module', dataclasses.replace(facts, unstripped=('COMP',))
        module = 'MODULES/COMP.ELF'
        size, crc = facts.files[module]
        yield 'module CRC mismatch', dataclasses.replace(facts, files={**facts.files, module: (size, crc ^ 1)})
        yield 'oversized module', dataclasses.replace(facts, files={
            **facts.files, module: (BootFiles.MAXIMUM_MODULE_BYTES + 1, crc)})
        for font in ('MODULES/VEGURB.OTF', 'MODULES/VEGURR.OTF'):
            if font in facts.files:
                size, checksum = facts.files[font]
                yield 'font CRC mismatch ' + font, dataclasses.replace(facts, files={**facts.files, font: (size, checksum ^ 1)})
                yield 'missing font data ' + font, dataclasses.replace(facts, files={key: value for key, value in facts.files.items() if key != font})
        yield 'embedded table', dataclasses.replace(facts, embedded=('AsteriaComponentImageStart',))
        yield 'missing module entry', dataclasses.replace(facts, manifest=b'\n'.join(
            line for line in facts.manifest.split(b'\n') if not line.startswith(b'module = component ')))
        yield 'missing kernel entry', dataclasses.replace(facts, manifest=b'\n'.join(
            line for line in facts.manifest.split(b'\n') if not line.startswith(b'kernel = ')))
        yield 'embedded kernel', dataclasses.replace(facts, embedded=('AsteriaEmbeddedKernelStart',))
        name = 'KERNEL.ELF'
        size, crc = facts.files[name]
        yield 'missing image', dataclasses.replace(facts, files={key: value for key, value in facts.files.items()
                                                                 if key != name})
        for label, value in (('CRC mismatch', (size, crc ^ 1)), ('empty file', (0, crc)),
                             ('oversized file', (BootFiles.MAXIMUM_FILE_BYTES + 1, crc))):
            yield label, dataclasses.replace(facts, files={**facts.files, name: value})
