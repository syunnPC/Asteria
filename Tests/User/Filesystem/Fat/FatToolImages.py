#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""FAT images made and checked with dosfstools and mtools for the FAT volume
library's tool-interop test (Source/User/Filesystem/Fat/FatVolume.hpp to 10). CTest runs `make` before FatToolInteropTests and `check` after it.

make DIR: writes fat12.img, fat16.img and fat32.img as disks
(sparse; mkfs.fat -S 512 -f 2 -R 128 -i 41535445 -n ASTERIA --invariant with
-F 12 -s 1 on 2 MiB, -F 16 -s 16 and -F 32 -s 1 -b 32 on 64 MiB), the boot
diagnostic's token at sector 8 after formatting, then README.TXT (a short
name) and a-long-lower-name.txt (a long name) copied in with mcopy. The two
files stay in DIR for the C++ test to compare.

check DIR: every clean-<type>.img the C++ test kept must pass fsck.fat -n with
exit status 0 and no message, and mtype must return each file's bytes as its
clean-<type>.<file>.expected; every crash-<type>-<budget>.img may make
fsck.fat -n report only the dirty state, lost clusters and a wrong
free-cluster summary, plus FATs that differ where crash-<type>.txt says the
crash fell between a FAT sector's two copies. Every image keeps the token.

Both commands print "SKIP:..." and exit 0 when a tool is missing.
"""
import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys

TOOLS = ('mkfs.fat', 'fsck.fat', 'mcopy', 'mtype')
COMMON = ['-S', '512', '-f', '2', '-R', '128', '-i', '41535445', '-n', 'ASTERIA', '--invariant']
FORMATS = {
    'fat12': (2 << 20, ['-F', '12', '-s', '1']),
    'fat16': (64 << 20, ['-F', '16', '-s', '16']),
    'fat32': (64 << 20, ['-F', '32', '-s', '1', '-b', '32']),
}
# Tools/Vm/AsteriaVm/Machine.py's DISK_TOKEN, 16 times at sector 8.
TOKEN = b'ASTERIA-READ-DMA-EXT-SECTOR-0001' * 16
TOKEN_SECTOR = 8
README = b'hello readme\n'
LONG_NAME = 'a-long-lower-name.txt'
LONG = bytes(((index * 7) + 3) & 0xFF for index in range(6000))
# The workload's files (FatWorkload.hpp) as mtype names them.
WORKLOAD_FILES = ('hello', 'a-much-longer-file-name.data', 'readme.txt')

# What fsck.fat -n may print after a crash; FATS_DIFFER only
# where the manifest allows it.
HEADER = re.compile(r'^fsck\.fat \d')
SUMMARY = re.compile(r'^\S+: \d+ files, \d+/\d+ clusters$')
ALLOWED = [re.compile(pattern) for pattern in (
    r'^$',
    r'^Dirty bit is set\. Fs was not properly unmounted and some data may be corrupt\.$',
    r'^\s*Automatically removing dirty bit\.$',
    r'^Reclaimed \d+ unused clusters? \(\d+ bytes\)\.$',
    r'^Free cluster summary wrong \(\d+ vs\. really \d+\)$',
    r'^Free cluster summary uninitialized \(should be \d+\)$',
    r'^\s*Auto-correcting\.$',
    r'^Leaving filesystem unchanged\.$',
)]
FATS_DIFFER = [re.compile(pattern) for pattern in (
    r'^FATs differ but appear to be intact\.$',
    r'^\s*Using first FAT\.$',
)]


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
        print('SKIP: %s not found; the FAT tool-interop tests need dosfstools and mtools' % ', '.join(missing))
        return None
    return paths


def mtools_environment() -> dict[str, str]:
    environment = dict(os.environ)
    environment['MTOOLS_SKIP_CHECK'] = '1'
    environment['MTOOLSRC'] = os.devnull
    return environment


def run(command: list[str], **arguments) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, check=False, **arguments)


def token_intact(image: pathlib.Path) -> bool:
    with image.open('rb') as handle:
        handle.seek(TOKEN_SECTOR * 512)
        return handle.read(512) == TOKEN


def make(directory: pathlib.Path) -> int:
    paths = tools()
    if paths is None:
        return 0
    if directory.exists():
        shutil.rmtree(directory)
    directory.mkdir(parents=True)
    (directory / 'README.TXT').write_bytes(README)
    (directory / LONG_NAME).write_bytes(LONG)
    for name, (size, flags) in FORMATS.items():
        image = directory / ('%s.img' % name)
        with image.open('wb') as handle:
            handle.truncate(size)
        steps = [
            [paths['mkfs.fat'], *COMMON, *flags, str(image)],
            None,
            [paths['mcopy'], '-i', str(image), str(directory / 'README.TXT'), '::README.TXT'],
            [paths['mcopy'], '-i', str(image), str(directory / LONG_NAME), '::' + LONG_NAME],
            [paths['fsck.fat'], '-n', str(image)],
        ]
        for step in steps:
            if step is None:
                # Mkfs.fat zero-fills the reserved sectors, so the token goes in after it.
                with image.open('r+b') as handle:
                    handle.seek(TOKEN_SECTOR * 512)
                    handle.write(TOKEN)
                continue
            result = run(step, env=mtools_environment())
            if result.returncode != 0:
                print('FAIL: %s exited %d\n%s%s' % (' '.join(step), result.returncode,
                                                     result.stdout.decode(errors='replace'),
                                                     result.stderr.decode(errors='replace')))
                return 1
    print('PASS: made %s in %s' % (', '.join('%s.img' % name for name in FORMATS), directory))
    return 0


def fsck_lines(fsck: str, image: pathlib.Path) -> tuple[int, list[str]]:
    result = run([fsck, '-n', str(image)])
    lines = (result.stdout + result.stderr).decode(errors='replace').splitlines()
    return result.returncode, [line for line in lines if not HEADER.match(line) and not SUMMARY.match(line)]


def check(directory: pathlib.Path) -> int:
    paths = tools()
    if paths is None:
        return 0
    failures = []
    cleans = sorted(directory.glob('clean-*.img'))
    if len(cleans) != len(FORMATS):
        failures.append('expected %d clean images in %s, found %d' % (len(FORMATS), directory, len(cleans)))
    for image in cleans:
        status, lines = fsck_lines(paths['fsck.fat'], image)
        if status != 0 or lines:
            failures.append('%s: fsck.fat -n exited %d: %s' % (image.name, status, lines))
        if not token_intact(image):
            failures.append('%s: the token at sector 8 changed' % image.name)
        stem = image.name[:-len('.img')]
        for name in WORKLOAD_FILES:
            expected = (directory / ('%s.%s.expected' % (stem, name))).read_bytes()
            result = run([paths['mtype'], '-i', str(image), '::' + name], env=mtools_environment())
            if result.returncode != 0 or result.stdout != expected:
                failures.append('%s: mtype ::%s exited %d with %d bytes, expected %d' % (
                    image.name, name, result.returncode, len(result.stdout), len(expected)))
        result = run([paths['mtype'], '-i', str(image), '::' + LONG_NAME], env=mtools_environment())
        if result.stdout != LONG:
            failures.append('%s: mtype ::%s changed' % (image.name, LONG_NAME))
    crashes = 0
    for name in FORMATS:
        manifest = directory / ('crash-%s.txt' % name)
        if not manifest.exists():
            failures.append('%s is missing' % manifest.name)
            continue
        for line in manifest.read_text().splitlines():
            budget, _, differ = line.split()
            image = directory / ('crash-%s-%03d.img' % (name, int(budget)))
            status, lines = fsck_lines(paths['fsck.fat'], image)
            allowed = ALLOWED + (FATS_DIFFER if differ == '1' else [])
            unexpected = [text for text in lines if not any(pattern.match(text) for pattern in allowed)]
            if status not in (0, 1) or unexpected:
                failures.append('%s: fsck.fat -n exited %d: %s' % (image.name, status, unexpected))
            if not token_intact(image):
                failures.append('%s: the token at sector 8 changed' % image.name)
            crashes += 1
    for image in sorted(directory.glob('tree-*.img')):
        status, lines = fsck_lines(paths['fsck.fat'], image)
        crash = image.name.startswith('tree-crash-')
        with image.open('rb') as handle:
            boot = handle.read(512)
            reserved = int.from_bytes(boot[14:16], 'little')
            sectors = int.from_bytes(boot[22:24], 'little') or int.from_bytes(boot[36:40], 'little')
            handle.seek(reserved * 512)
            first, second = handle.read(sectors * 512), handle.read(sectors * 512)
        differences = sum(first[at:at + 512] != second[at:at + 512] for at in range(0, len(first), 512))
        # The writer flushes one sector's first copy, then its second. Even
        # directory growth must never leave two outstanding differing sectors.
        allowed = ALLOWED + (FATS_DIFFER if crash and differences == 1 else [])
        unexpected = [text for text in lines if not any(pattern.match(text) for pattern in allowed)]
        if differences > (1 if crash else 0):
            failures.append('%s: unauthenticated FAT-copy divergence (%d sectors)' % (image.name, differences))
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / 'Tools' / 'Vm'))
        from AsteriaVm.Observers.DiskImages import decode_fat
        try:
            files, aliases = decode_fat(image)
        except ValueError as error:
            failures.append('%s: invalid reachable UTF-16 tree: %s' % (image.name, error))
            files, aliases = {}, {}
        if (status not in ((0, 1) if crash else (0,))) or unexpected:
            failures.append('%s: invalid tree crash state: %s' % (image.name, lines))
        if not token_intact(image):
            failures.append('%s: token changed' % image.name)
        if not crash:
            expected = bytearray()
            for seed in range(100, 108):
                state = seed
                for _ in range(4096):
                    state = (state * 1103515245 + 12345) & 0xffffffff
                    expected.append((state >> 16) & 255)
            name = 'apps/日本-😀/résumé-😀.txt'
            if files.get(name) != expected:
                failures.append('%s: original UTF-16 name/data mismatch' % image.name)
            result = run([paths['mtype'], '-i', str(image), '::' + aliases.get(name, name)],
                         env={**mtools_environment(), 'LC_ALL': 'C.UTF-8'})
            if result.returncode or result.stdout != expected:
                failures.append('%s: nested UTF-16 name/data mismatch' % image.name)
    for failure in failures[:20]:
        print('FAIL:', failure)
    if failures:
        print('FAIL: %d problems in %s' % (len(failures), directory))
        return 1
    print('PASS: %d clean images pass fsck.fat and mtype; %d crashed images report only the expected inconsistencies'
          % (len(cleans), crashes))
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
