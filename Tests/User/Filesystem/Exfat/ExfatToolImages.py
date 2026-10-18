#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""exFAT images made and checked with exfatprogs for the exFAT volume
library's tests.
CTest runs `make` before the tests that read the images and `check` after
them.

make DIR: writes exfat-4k.img and exfat-512.img, 64 MiB sparse files made by
mkfs.exfat -c 4K and -c 512 (-L ASTERIA), then writes the boot diagnostic's
token into sector 8 and the backup boot region's sector 20 and recomputes
both boot checksums, as the runner's recipe does. fsck.exfat -n
must pass on each.

check DIR: every clean-*.img and crash-*.img the C++ tests kept must pass
fsck.exfat -n with exit status 0 (fsck.exfat is silent on clusters allocated
but unreferenced and on VolumeDirty, which the C++ tests' invariant checker
judges) and keep the token; this script's own decoder, written from the
specification apart from the C++, must read every file of a clean image as
clean-<variant>.<file>.expected holds.

Both commands print "SKIP:..." and exit 0 when a tool is missing.
"""
import argparse
import pathlib
import shutil
import struct
import subprocess
import sys

TOOLS = ('mkfs.exfat', 'fsck.exfat')
FORMATS = {
    'exfat-4k': ['-c', '4K'],
    'exfat-512': ['-c', '512'],
}
SIZE = 64 << 20
# Tools/Vm/AsteriaVm/Machine.py's DISK_TOKEN, 16 times, in sector 8 and the
# backup boot region's sector 20.
TOKEN = b'ASTERIA-READ-DMA-EXT-SECTOR-0001' * 16
TOKEN_SECTORS = (8, 20)


def find_tool(name: str) -> str | None:
    found = shutil.which(name)
    for directory in ('/usr/sbin', '/sbin'):
        candidate = pathlib.Path(directory) / name
        if found is None and candidate.is_file():
            found = str(candidate)
    return found


def tools() -> dict[str, str] | None:
    paths = {name: find_tool(name) for name in TOOLS}
    missing = [name for name, path in paths.items() if path is None]
    if missing:
        print('SKIP: %s not found; the exFAT tool-interop tests need exfatprogs' % ', '.join(missing))
        return None
    return paths


def run(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, check=False)


def boot_checksum(region: bytes) -> int:
    """The checksum of a boot region's sectors 0 to 10, skipping VolumeFlags
    (106, 107) and PercentInUse (112) of its boot sector."""
    checksum = 0
    for index, value in enumerate(region[:11 * 512]):
        if index in (106, 107, 112):
            continue
        checksum = ((checksum >> 1) | ((checksum & 1) << 31)) + value & 0xFFFFFFFF
    return checksum


def write_token(image: pathlib.Path) -> None:
    with image.open('r+b') as handle:
        for sector in TOKEN_SECTORS:
            handle.seek(sector * 512)
            handle.write(TOKEN)
        for base in (0, 12):
            handle.seek(base * 512)
            region = handle.read(11 * 512)
            handle.seek((base + 11) * 512)
            handle.write(struct.pack('<I', boot_checksum(region)) * 128)


def token_intact(image: pathlib.Path) -> bool:
    with image.open('rb') as handle:
        intact = True
        for sector in TOKEN_SECTORS:
            handle.seek(sector * 512)
            intact = intact and handle.read(512) == TOKEN
        return intact


def make(directory: pathlib.Path) -> int:
    paths = tools()
    if paths is None:
        return 0
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    for name, flags in FORMATS.items():
        image = directory / ('%s.img' % name)
        with image.open('wb') as handle:
            handle.truncate(SIZE)
        result = run([paths['mkfs.exfat'], *flags, '-L', 'ASTERIA', str(image)])
        if result.returncode == 0:
            write_token(image)
            result = run([paths['fsck.exfat'], '-n', str(image)])
        if result.returncode != 0:
            print('FAIL: %s: exit %d\n%s%s' % (image.name, result.returncode, result.stdout.decode(errors='replace'),
                                               result.stderr.decode(errors='replace')))
            return 1
    print('PASS: made %s in %s' % (', '.join('%s.img' % name for name in FORMATS), directory))
    return 0


class Volume:
    """A minimal reader of a volume's root directory files."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        boot = data[:512]
        self.fat = struct.unpack_from('<I', boot, 80)[0] * 512
        self.heap = struct.unpack_from('<I', boot, 88)[0]
        self.count = struct.unpack_from('<I', boot, 92)[0]
        self.root = struct.unpack_from('<I', boot, 96)[0]
        self.cluster = 512 << boot[109]

    def offset(self, cluster: int) -> int:
        return self.heap * 512 + (cluster - 2) * self.cluster

    def chain(self, first: int) -> list[int]:
        clusters = []
        while 2 <= first <= self.count + 1 and len(clusters) <= self.count:
            clusters.append(first)
            first = struct.unpack_from('<I', self.data, self.fat + first * 4)[0]
        return clusters

    def files(self) -> dict[str, bytes]:
        root = b''.join(self.data[self.offset(c):self.offset(c) + self.cluster] for c in self.chain(self.root))
        found = {}
        index = 0
        while index + 32 <= len(root) and root[index] != 0:
            if root[index] != 0x85:
                index += 32
                continue
            count = root[index + 1] + 1
            stream = root[index + 32:index + 64]
            length = stream[3]
            name = b''.join(root[index + 64 + 32 * k + 2:index + 96 + 32 * k] for k in range((length + 14) // 15))
            name = name.decode('utf-16-le')[:length]
            first = struct.unpack_from('<I', stream, 20)[0]
            size = struct.unpack_from('<Q', stream, 24)[0]
            needed = (size + self.cluster - 1) // self.cluster
            if stream[1] & 0x02:
                clusters = list(range(first, first + needed))
            else:
                clusters = self.chain(first)[:needed]
            content = b''.join(self.data[self.offset(c):self.offset(c) + self.cluster] for c in clusters)
            found[name.lower()] = content[:size]
            index += 32 * count
        return found


def fsck(path: str, image: pathlib.Path) -> tuple[int, str]:
    result = run([path, '-n', str(image)])
    return result.returncode, (result.stdout + result.stderr).decode(errors='replace').strip().replace('\n', ' | ')


def check(directory: pathlib.Path) -> int:
    paths = tools()
    if paths is None:
        return 0
    failures = []
    cleans = sorted(directory.glob('clean-*.img'))
    if len(cleans) != len(FORMATS):
        failures.append('expected %d clean images in %s, found %d' % (len(FORMATS), directory, len(cleans)))
    for image in cleans:
        status, output = fsck(paths['fsck.exfat'], image)
        if status != 0:
            failures.append('%s: fsck.exfat -n exited %d: %s' % (image.name, status, output))
        if not token_intact(image):
            failures.append('%s: the token changed' % image.name)
        stem = image.name[:-len('.img')]
        files = Volume(image.read_bytes()).files()
        expectations = sorted(directory.glob('%s.*.expected' % stem))
        if not expectations:
            failures.append('%s: no expected files' % image.name)
        for expected in expectations:
            name = expected.name[len(stem) + 1:-len('.expected')]
            if files.get(name) != expected.read_bytes():
                failures.append('%s: %s reads %s bytes, expected %d' % (
                    image.name, name, len(files[name]) if name in files else 'no', len(expected.read_bytes())))
    crashes = sorted(directory.glob('crash-*.img'))
    for image in crashes:
        status, output = fsck(paths['fsck.exfat'], image)
        if status != 0:
            failures.append('%s: fsck.exfat -n exited %d: %s' % (image.name, status, output))
        if not token_intact(image):
            failures.append('%s: the token changed' % image.name)
    if len(crashes) < 100:
        failures.append('only %d crashed images in %s' % (len(crashes), directory))
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / 'Tools' / 'Vm'))
    from AsteriaVm.Observers.DiskImages import decode_exfat
    for image in sorted(directory.glob('tree-*.img')):
        status, output = fsck(paths['fsck.exfat'], image)
        if status or not token_intact(image):
            failures.append('%s: fsck/token: %s' % (image.name, output))
        try:
            files, findings = decode_exfat(image)
            crash = image.name.startswith('tree-crash-')
            forbidden = [finding for finding in findings if not (crash and
                (finding == 'exFAT VolumeDirty is set' or finding.startswith('exFAT bitmap has leaked clusters:')))]
            if forbidden:
                failures.append('%s: %s' % (image.name, forbidden))
            if not crash:
                expected = bytearray()
                for seed in range(100, 108):
                    state = seed
                    for _ in range(4096):
                        state = (state * 1103515245 + 12345) & 0xffffffff
                        expected.append((state >> 16) & 255)
                if files.get('apps/日本-😀/résumé-😀.txt') != expected:
                    failures.append('%s: nested UTF-16 name/data mismatch' % image.name)
        except (ValueError, OSError) as error:
            failures.append('%s: %s' % (image.name, error))
    for failure in failures[:20]:
        print('FAIL:', failure)
    if failures:
        print('FAIL: %d problems in %s' % (len(failures), directory))
        return 1
    print('PASS: %d clean images pass fsck.exfat and the decoder; %d crashed images pass fsck.exfat'
          % (len(cleans), len(crashes)))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('make', 'check'))
    parser.add_argument('directory', type=pathlib.Path)
    arguments = parser.parse_args()
    if arguments.command == 'make':
        return make(arguments.directory)
    return check(arguments.directory)


if __name__ == '__main__':
    sys.exit(main())
