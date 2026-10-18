# SPDX-License-Identifier: Apache-2.0
"""Per-case sparse disks and filesystem byte contracts."""
from __future__ import annotations

import dataclasses
import os
import pathlib
import shutil
import subprocess
import tempfile

from AsteriaVm import ExfatImages, Machine

FAT_SCENARIO_LINES = {
    'fat12': 'asteria fat12 clean\n',
    'fat16': 'asteria fat16 crash-before\n',
    'fat32': 'asteria fat32 clean\n',
    'exfat': 'asteria exfat clean\n',
    'fat32-v2': 'asteria fat32 v2\n',
    'exfat-v2': 'asteria exfat v2\n',
}
LONG_NAME = 'a-long-lower-name.txt'
LONG_CONTENT = (
    b'Asteria filesystem fixture: a long lowercase name.\n'
    b'The host checks every byte after the filesystem service runs.\n'
    b'Clusters may be smaller or larger than a page.\n'
    b'Copy-on-write commits preserve these original files.\n'
    b'This ASCII text has fixed contents and several lines.\n'
    b'Timestamps are deliberately outside the verification contract.\n'
)
FAT_FORMATS = {
    'fat12': (2 << 20, ['-F', '12', '-s', '1']),
    'fat16': (64 << 20, ['-F', '16', '-s', '16']),
    'fat32': (64 << 20, ['-F', '32', '-s', '1', '-b', '32']),
}
FAT_COMMON_FLAGS = ['-S', '512', '-f', '2', '-R', '128', '-i', '41535445', '-n', 'ASTERIA', '--invariant']


@dataclasses.dataclass(frozen=True)
class FsDemo:
    """What the root service's filesystem demonstration (FsDemo.cpp) does on a
    disk recipe, on either ISA: its service's incarnations, the first
    `crashes` of which crash on a commit and are started again; the root
    service's mappings of its data Frame, to fill or check a page; and the
    service's mappings of that Frame for the FAT path's Reads and Writes."""
    services: int
    crashes: int
    root_maps: int
    service_maps: int


# Hello's two pages written and read back (four fills and checks; on FAT four Reads and Writes the
# service maps), FAT's readme read (its scenario checked, its page mapped by the service) and, on
# fat16, crash.txt's page written before the crash before its directory entry; AsteriaFS rewrites
# page 0 twice, each time read back after the crash before and after the superblock, zero-copy.
FS_DEMOS = {
    '': FsDemo(services=0, crashes=0, root_maps=0, service_maps=0),
    'raw': FsDemo(services=3, crashes=2, root_maps=8, service_maps=0),
    'fat12': FsDemo(services=1, crashes=0, root_maps=5, service_maps=5),
    'fat16': FsDemo(services=2, crashes=1, root_maps=6, service_maps=6),
    'fat32': FsDemo(services=1, crashes=0, root_maps=5, service_maps=5),
    'exfat': FsDemo(services=1, crashes=0, root_maps=5, service_maps=5),
    'fat32-v2': FsDemo(services=1, crashes=0, root_maps=22, service_maps=22),
    'exfat-v2': FsDemo(services=1, crashes=0, root_maps=22, service_maps=22),
}


def hello_page(k: int) -> bytes:
    """Guest demo page k: a repeated little-endian HELLO word, 4096 bytes."""
    return (0x48454C4C4F000000 | k).to_bytes(8, 'little') * 512


def fat_expected(recipe: str) -> dict[str, bytes]:
    """Original files, independent of mcopy's host-clock timestamps."""
    return {'ASTERIA.RW': b'Asteria filesystem write opt-in\n', 'README.TXT': FAT_SCENARIO_LINES[recipe].encode('ascii'), LONG_NAME: LONG_CONTENT,
            **({'ASTERIA.OK': b'Asteria inventory consent\n'} if recipe == 'fat32' else {})}


def after_run_expected(recipe: str) -> dict[str, bytes]:
    expected = fat_expected(recipe)
    if recipe.endswith('-v2'):
        expected['apps/日本-😀/résumé-😀.txt'] = b''.join(hello_page(page + 10) for page in range(8))
    return expected


def find_tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for directory in ('/usr/sbin', '/sbin'):
        candidate = pathlib.Path(directory) / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    raise FileNotFoundError('%s not found; filesystem disks require dosfstools/mtools or exfatprogs' % name)


def mtools_environment() -> dict[str, str]:
    return {**os.environ, 'MTOOLS_SKIP_CHECK': '1', 'MTOOLSRC': os.devnull, 'LC_ALL': 'C.UTF-8'}


def run_tool(name: str, arguments: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([find_tool(name), *arguments], capture_output=True,
                          env=mtools_environment(), timeout=30)


def _require_tool(name: str, arguments: list[str]) -> None:
    result = run_tool(name, arguments)
    if result.returncode:
        raise ValueError('%s exited %d: %s' % (name, result.returncode,
                         (result.stdout + result.stderr).decode(errors='replace').strip()))


def write_disk_recipe(path: pathlib.Path, recipe: str) -> dict[str, bytes]:
    """Create Disk.raw; return the original files' expected bytes. Raw is unchanged."""
    if recipe == 'raw':
        Machine.write_disk_fixture(path)
        return {}
    if recipe in ('exfat', 'exfat-v2'):
        with path.open('wb') as handle:
            handle.truncate(64 << 20)
        _require_tool('mkfs.exfat', ['-c', '4K', '-L', 'ASTERIA', str(path)])
        ExfatImages.stamp_boot_token(path, Machine.DISK_TOKEN * 16)
        expected = fat_expected(recipe)
        for name, content in expected.items():
            ExfatImages.write_file(path, name, content)
        _require_tool('fsck.exfat', ['-n', str(path)])
        return expected
    base = recipe.removesuffix('-v2')
    if base not in FAT_FORMATS:
        raise ValueError('unknown disk recipe %r' % recipe)
    size, flags = FAT_FORMATS[base]
    with path.open('wb') as handle:
        handle.truncate(size)
    _require_tool('mkfs.fat', [*FAT_COMMON_FLAGS, *flags, str(path)])
    # Mkfs zero-fills reserved sectors; restore the boot diagnostic's token afterwards.
    with path.open('r+b') as handle:
        handle.seek(Machine.DISK_TOKEN_SECTOR * 512)
        handle.write(Machine.DISK_TOKEN * 16)
    expected = fat_expected(recipe)
    with tempfile.TemporaryDirectory(prefix='FatFiles-', dir=path.parent) as scratch:
        for name, content in expected.items():
            source = pathlib.Path(scratch) / name
            source.write_bytes(content)
            _require_tool('mcopy', ['-i', str(path), str(source), '::' + name])
    return expected
