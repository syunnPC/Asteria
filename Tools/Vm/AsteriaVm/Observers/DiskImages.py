# SPDX-License-Identifier: Apache-2.0
"""Read-only host checks of the filesystem demo's FAT, exFAT and AsteriaFS disks.

The native decoder is independent of the C++ implementation; offsets and
canonical constraints are the v0 disk contract in FsFormat.hpp and filesystem protocol.
"""
from __future__ import annotations

import pathlib
import re
import struct
import subprocess

from AsteriaVm import DiskRecipes, ExfatImages, Machine

_FSCK_HEADER = re.compile(r'^fsck\.fat \d')
_FSCK_SUMMARY = re.compile(r'^\S+: \d+ files, \d+/\d+ clusters$')
# Expected dosfstools 4.2 messages for
_CRASH_MESSAGES = tuple(re.compile(pattern) for pattern in (
    r'^$',
    r'^Dirty bit is set\. Fs was not properly unmounted and some data may be corrupt\.$',
    r'^\s*Automatically removing dirty bit\.$',
    r'^Reclaimed \d+ unused clusters? \(\d+ bytes\)\.$',
    r'^Free cluster summary wrong \(\d+ vs\. really \d+\)$',
    r'^Free cluster summary uninitialized \(should be \d+\)$',
    r'^\s*Auto-correcting\.$',
    r'^Leaving filesystem unchanged\.$',
    r'^FATs differ but appear to be intact\.$',
    r'^\s*Using first FAT\.$',
))


def _token_problems(path: pathlib.Path) -> list[str]:
    with path.open('rb') as handle:
        handle.seek(Machine.DISK_TOKEN_SECTOR * 512)
        intact = handle.read(512) == Machine.DISK_TOKEN * 16
    return [] if intact else ['the token at sector 8 changed']


def _fat_copy_problems(path: pathlib.Path, recipe: str, crash: bool) -> list[str]:
    """Authenticate the allowed crash divergence, including which copy is newer.

    The crash-before demo creates a new file: every changed entry must
    allocate a formerly free cluster. Checking bytes alone cannot establish
    that the first FAT is newer. Its new chains must end and remain within
    those fresh allocations.
    """
    with path.open('rb') as handle:
        boot = handle.read(512)
        if len(boot) != 512:
            return ['FAT image has a truncated boot sector']
        reserved = int.from_bytes(boot[14:16], 'little')
        fat_sectors = int.from_bytes(boot[22:24], 'little') or int.from_bytes(boot[36:40], 'little')
        total = int.from_bytes(boot[19:21], 'little') or int.from_bytes(boot[32:36], 'little')
        roots = int.from_bytes(boot[17:19], 'little')
        if (int.from_bytes(boot[11:13], 'little') != 512 or boot[16] != 2 or
                not boot[13] or boot[13] & (boot[13] - 1) or not reserved or not fat_sectors or
                total * 512 > path.stat().st_size or
                (reserved + 2 * fat_sectors) >= total):
            return ['invalid FAT geometry or number of FAT copies']
        clusters = (total - reserved - 2 * fat_sectors - (roots * 32 + 511) // 512) // boot[13]
        kind = 'fat12' if clusters < 4085 else 'fat16' if clusters < 65525 else 'fat32'
        if kind != recipe:
            return ['FAT cluster count identifies %s, expected %s' % (kind, recipe)]
        handle.seek(reserved * 512)
        first, second = handle.read(fat_sectors * 512), handle.read(fat_sectors * 512)
    differences = [sector for sector in range(fat_sectors)
                   if first[sector * 512:(sector + 1) * 512] != second[sector * 512:(sector + 1) * 512]]
    if not differences:
        return []
    if not crash or len(differences) != 1:
        return ['FAT copies differ in %d sectors; only one sector in crash-before is allowed' % len(differences)]
    # Only FAT16 has a crash-before scenario. The formatter and scenario are fixed.
    if recipe != 'fat16':
        return ['FAT divergence has no crash-before scenario for %s' % recipe]
    allocated = {}
    for index in range(len(first) // 2):
        new = int.from_bytes(first[index * 2:index * 2 + 2], 'little')
        old = int.from_bytes(second[index * 2:index * 2 + 2], 'little')
        if new != old:
            if not (2 <= index < clusters + 2) or old != 0 or new == 0:
                return ['differing FAT sector does not show fresh allocations in the first copy']
            allocated[index] = new
    links = [cluster for cluster in allocated.values() if cluster < 0xFFF8]
    if len(links) != len(set(links)):
        return ['first FAT has cross-linked fresh allocation chains']
    for start in allocated:
        seen = set()
        cluster = start
        while cluster < 0xFFF8:
            if cluster in seen or cluster not in allocated:
                return ['first FAT has a broken or cyclic fresh allocation chain']
            seen.add(cluster)
            cluster = allocated[cluster]
    return []


def decode_fat(image_path: pathlib.Path) -> tuple[dict[str, bytes], dict[str, str]]:
    """FAT 1.03, FAT directory/long-name entries: validate UTF-16 at the edge.

    mtools renders surrogate pairs as underscores on some hosts. Authenticate
    the original names independently, then use its short aliases to check bytes.
    """
    raw = image_path.read_bytes()
    u16 = lambda at: int.from_bytes(raw[at:at + 2], 'little')
    u32 = lambda at: int.from_bytes(raw[at:at + 4], 'little')
    sectors, reserved, copies, roots = raw[13], u16(14), raw[16], u16(17)
    fat_size = u16(22) or u32(36)
    total = u16(19) or u32(32)
    root_sector = reserved + copies * fat_size
    heap = root_sector + (roots * 32 + 511) // 512
    clusters = (total - heap) // sectors
    bits = 12 if clusters < 4085 else 16 if clusters < 65525 else 32
    fat = reserved * 512
    def link(cluster):
        if bits == 12:
            value = u16(fat + cluster * 3 // 2)
            return (value >> 4) if cluster & 1 else value & 4095
        return (u16(fat + cluster * 2) if bits == 16 else u32(fat + cluster * 4) & 0xfffffff)
    end = {12: 0xff8, 16: 0xfff8, 32: 0xffffff8}[bits]
    owned = set()
    def chain(first):
        data = bytearray()
        while first < end:
            _need(2 <= first < clusters + 2 and first not in owned, 'FAT cross-linked or invalid chain')
            owned.add(first)
            start = (heap + (first - 2) * sectors) * 512
            data += raw[start:start + sectors * 512]
            first = link(first)
        return bytes(data)
    pending = [('', '', chain(u32(44)) if bits == 32 else raw[root_sector * 512:root_sector * 512 + roots * 32])]
    files, aliases = {}, {}
    visited = 0
    while pending:
        prefix, alias_prefix, directory = pending.pop()
        visited += 1
        _need(visited <= 128, 'FAT directory walk exceeds bound')
        long_name, ordinal, checksum = {}, 0, 0
        for at in range(0, len(directory), 32):
            entry = directory[at:at + 32]
            if entry[0] == 0:
                break
            if entry[0] == 0xe5:
                long_name, ordinal = {}, 0
                continue
            if entry[11] == 15:
                part = entry[0] & 31
                if entry[0] & 64:
                    long_name, ordinal, checksum = {}, part, entry[13]
                _need(part == ordinal and part > 0 and entry[13] == checksum and entry[12] == 0
                      and entry[26:28] == b'\0\0', 'invalid FAT long-name sequence')
                long_name[part] = entry[1:11] + entry[14:26] + entry[28:32]
                ordinal -= 1
                continue
            short = entry[:8].rstrip(b' ').decode('ascii')
            extension = entry[8:11].rstrip(b' ').decode('ascii')
            alias = short + ('.' + extension if extension else '')
            name = alias
            if long_name:
                folded = 0
                for byte in entry[:11]:
                    folded = (((folded >> 1) | ((folded & 1) << 7)) + byte) & 255
                _need(ordinal == 0 and folded == checksum, 'FAT long-name checksum mismatch')
                units = b''.join(long_name[n] for n in range(1, len(long_name) + 1))
                words = struct.unpack('<%dH' % (len(units) // 2), units)
                length = words.index(0) if 0 in words else len(words)
                _need(all(word == 0xffff for word in words[length + 1:]), 'invalid FAT long-name padding')
                name = units[:length * 2].decode('utf-16le')
            long_name, ordinal = {}, 0
            if entry[11] & 8 or alias in ('.', '..'):
                continue
            first = int.from_bytes(entry[26:28], 'little') | (int.from_bytes(entry[20:22], 'little') << 16 if bits == 32 else 0)
            name = ''.join(chr(ord(c) + 32) if 'A' <= c <= 'Z' else c for c in name)
            path, alias_path = prefix + name, alias_prefix + alias
            if entry[11] & 16:
                pending.append((path + '/', alias_path + '/', chain(first)))
            else:
                size = int.from_bytes(entry[28:32], 'little')
                contents = chain(first) if first else b''
                _need(size <= len(contents), 'FAT file exceeds chain')
                files[path] = contents[:size]
                aliases[path] = alias_path
    return files, aliases


def fat_problems(image_path: pathlib.Path, recipe: str, expected: dict[str, bytes]) -> list[str]:
    """fsck, directory/file bytes and token; missing host tools fail a live check."""
    if recipe.removesuffix('-v2') not in DiskRecipes.FAT_FORMATS:
        return ['unknown FAT recipe %r' % recipe]
    crash = 'crash-before' in DiskRecipes.FAT_SCENARIO_LINES[recipe]
    try:
        problems = _token_problems(image_path) + _fat_copy_problems(image_path, recipe.removesuffix('-v2'), crash)
        result = DiskRecipes.run_tool('fsck.fat', ['-n', str(image_path)])
        lines = (result.stdout + result.stderr).decode(errors='replace').splitlines()
        findings = [line for line in lines if not _FSCK_HEADER.match(line) and not _FSCK_SUMMARY.match(line)]
        unexpected = ([line for line in findings if not any(pattern.fullmatch(line) for pattern in _CRASH_MESSAGES)]
                      if crash else [line for line in findings if line.strip()])
        if result.returncode not in ((0, 1) if crash else (0,)) or unexpected:
            problems.append('fsck.fat -n exited %d: %s' % (result.returncode, '; '.join(unexpected or findings)))
        decoded, aliases = decode_fat(image_path) if recipe.endswith('-v2') else ({}, {})
        files = {**expected, 'HELLO': DiskRecipes.hello_page(0) + DiskRecipes.hello_page(1)}
        for name, content in files.items():
            if any(ord(c) > 127 for c in name):
                if decoded.get(name) != content:
                    problems.append('FAT UTF-16 name/data differs: %s' % name)
                name = aliases.get(name, name)
            listing = DiskRecipes.run_tool('mdir', ['-i', str(image_path), '::' + name])
            if listing.returncode:
                problems.append('mdir: %s is missing or unreadable' % name)
            result = DiskRecipes.run_tool('mtype', ['-i', str(image_path), '::' + name])
            if result.returncode or result.stdout != content:
                problems.append('mtype: %s differs (status %d, %d bytes, expected %d)' % (
                    name, result.returncode, len(result.stdout), len(content)))
        if crash:
            # Mdir's nonzero status alone could also be an I/O error. A full listing must work, then
            # a successful targeted lookup is forbidden.
            listing = DiskRecipes.run_tool('mdir', ['-i', str(image_path), '::'])
            if listing.returncode:
                problems.append('mdir cannot enumerate the root directory')
            found = DiskRecipes.run_tool('mdir', ['-i', str(image_path), '::crash.txt'])
            if found.returncode == 0:
                problems.append('crash.txt exists after crash-before')
        return problems
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        return ['cannot check FAT image: %s' % error]


def _exfat_sum(data: bytes, bits: int, skip: tuple[int,...] = ()) -> int:
    """Observer implementation of the spec's rotate/add, independent of the encoder."""
    total, mask = 0, (1 << bits) - 1
    for offset, byte in enumerate(data):
        if offset not in skip:
            total = ((total >> 1) | ((total & 1) << (bits - 1)))
            total = (total + byte) & mask
    return total


def _exfat_upcase(raw: bytes) -> list[int]:
    _need(len(raw) % 2 == 0, 'odd exFAT up-case table length')
    words = list(struct.unpack('<%dH' % (len(raw) // 2), raw))
    table, index = [], 0
    while index < len(words):
        word = words[index]
        index += 1
        if word == 0xFFFF and len(table) != 0xFFFF:
            _need(index < len(words), 'truncated exFAT up-case identity run')
            count = words[index]
            index += 1
            _need(count > 0 and len(table) + count <= 65536, 'invalid exFAT up-case identity run')
            table.extend(range(len(table), len(table) + count))
        else:
            table.append(word)
        _need(len(table) <= 65536, 'exFAT up-case table too long')
    _need(len(table) == 65536, 'incomplete exFAT up-case table')
    return table


def decode_exfat(image_path: pathlib.Path) -> tuple[dict[str, bytes], list[str]]:
    """Read root files and check clean-volume invariants without host tools.

    Only bounded geometry/I/O/FAT traversal is shared with the writer. Directory
    parsing, checksums, up-case decoding and bitmap accounting are the observer's
    own. Malformed structures raise ValueError; inspectable violations are returned.
    """
    u = ExfatImages.uint
    problems = []
    with ExfatImages.Volume(image_path) as volume:
        g = volume.geometry
        for base in (0, 12):
            boot = volume.read(base * 512, 512)
            _need(ExfatImages.parse_boot(boot, volume.size) == g, 'exFAT backup geometry differs')
            region = volume.read(base * 512, 11 * 512)
            checksum = _exfat_sum(region, 32, (106, 107, 112))
            if volume.read((base + 11) * 512, 512) != struct.pack('<I', checksum) * 128:
                problems.append('exFAT boot checksum at sector %d is wrong' % (base + 11))
        if u(volume.boot, 106, 2) & 2:
            problems.append('exFAT VolumeDirty is set')
        root_clusters = volume.chain(g.root_cluster)
        root = volume.root_entries()
        live = []
        ended = False
        for offset, entry in root:
            if entry[0] == 0:
                ended = True
            if entry[0] & 0x80:
                _need(not ended, 'exFAT in-use entry follows unused directory entry')
                live.append((offset, entry))
        bitmaps = [entry for _, entry in live if entry[0] == 0x81]
        upcases = [entry for _, entry in live if entry[0] == 0x82]
        _need(len(bitmaps) == len(upcases) == 1, 'exFAT needs exactly one bitmap and up-case table')
        bitmap_entry, upcase_entry = bitmaps[0], upcases[0]
        _need(bitmap_entry[1] == 0, 'invalid exFAT BitmapFlags')
        bitmap_length = u(bitmap_entry, 24, 8)
        _need(bitmap_length == (g.cluster_count + 7) // 8, 'invalid exFAT bitmap length')
        bitmap_clusters = volume.allocation(u(bitmap_entry, 20), bitmap_length)
        upcase_length = u(upcase_entry, 24, 8)
        _need(0 < upcase_length <= 131072, 'invalid exFAT up-case length')
        upcase_clusters = volume.allocation(u(upcase_entry, 20), upcase_length)
        upcase_raw = volume.data(upcase_clusters, upcase_length)
        _need(_exfat_sum(upcase_raw, 32) == u(upcase_entry, 4), 'exFAT up-case TableChecksum is wrong')
        upcase = _exfat_upcase(upcase_raw)
        reachable = set()

        def add(clusters: list[int], owner: str) -> None:
            overlap = reachable.intersection(clusters)
            if overlap:
                problems.append('exFAT cross-linked clusters for %s: %s' % (owner, sorted(overlap)))
            reachable.update(clusters)

        add(root_clusters, 'root directory')
        add(bitmap_clusters, 'allocation bitmap')
        add(upcase_clusters, 'up-case table')
        files = {}
        pending = [('', root)]
        visited_directories = set(root_clusters)
        directories = 0
        while pending:
            directories += 1
            _need(directories <= 128, 'directory traversal bound exceeded')
            prefix, root = pending.pop()
            ended = False
            for _, item in root:
                if item[0] == 0:
                    ended = True
                _need(not (ended and item[0] & 0x80), 'live entry follows directory end')
            folded_names = set()
            index = 0
            while index < len(root):
                offset, entry = root[index]
                kind = entry[0]
                if kind == 0:
                    break
                if not kind & 0x80 or kind in (0x81, 0x82, 0x83):
                    index += 1
                    continue
                if kind != 0x85:
                    _need(bool(kind & 0x20) and not kind & 0x40, 'unexpected critical exFAT directory entry')
                    index += 1 + entry[1]  # Skip benign primary entry sets.
                    continue
                count = entry[1] + 1
                _need(count >= 3 and index + count <= len(root), 'truncated exFAT file entry set')
                items = root[index:index + count]
                entries = b''.join(item for _, item in items)
                _need(all(item[0] & 0x80 for _, item in items), 'deleted/unused entry inside exFAT file set')
                stream = items[1][1]
                _need(stream[0] == 0xC0 and 1 <= stream[3] <= 255, 'invalid exFAT Stream Extension')
                name_entries = [item for _, item in items[2:] if item[0] == 0xC1]
                _need(len(name_entries) == (stream[3] + 14) // 15, 'invalid exFAT File Name entry count')
                _need(all(item[0] == 0xC1 or item[0] & 0x20 for _, item in items[2:]),
                      'unexpected critical exFAT secondary entry')
                raw_name = b''.join(item[2:32] for item in name_entries)[:stream[3] * 2]
                name = raw_name.decode('utf-16le')
                folded = b''.join(struct.pack('<H', upcase[word]) for (word,) in struct.iter_unpack('<H', raw_name))
                if folded in folded_names:
                    problems.append('duplicate exFAT file name: %s' % name)
                folded_names.add(folded)
                if _exfat_sum(entries, 16, (2, 3)) != u(entry, 2, 2):
                    problems.append('exFAT %s SetChecksum is wrong' % name)
                if _exfat_sum(folded, 16) != u(stream, 4, 2):
                    problems.append('exFAT %s NameHash is wrong' % name)
                if offset // 512 != (items[-1][0] + 31) // 512:
                    problems.append('exFAT %s entry set crosses a sector' % name)
                length, valid = u(stream, 24, 8), u(stream, 8, 8)
                if valid != length:
                    problems.append('exFAT %s ValidDataLength differs from DataLength' % name)
                _need(stream[1] in (1, 3), 'invalid exFAT stream allocation flags')
                clusters = volume.allocation(u(stream, 20), length, bool(stream[1] & 2))
                full_name = prefix + name
                add(clusters, full_name)
                if u(entry, 4, 2) & 0x10:
                    _need(length > 0 and length % g.cluster_bytes == 0, 'invalid directory allocation')
                    _need(not reachable.intersection(visited_directories).intersection(clusters), 'directory cycle')
                    visited_directories.update(clusters)
                    pending.append((full_name + '/', [(volume.cluster_offset(c) + offset,
                        volume.read(volume.cluster_offset(c) + offset, 32))
                        for c in clusters for offset in range(0, g.cluster_bytes, 32)]))
                else:
                    files[full_name] = volume.data(clusters, length)
                index += count
        bitmap = volume.data(bitmap_clusters, bitmap_length)
        allocated = {bit + 2 for bit in range(g.cluster_count) if bitmap[bit // 8] & (1 << (bit % 8))}
        leaked, free = allocated - reachable, reachable - allocated
        if leaked:
            problems.append('exFAT bitmap has leaked clusters: %s' % sorted(leaked))
        if free:
            problems.append('exFAT referenced clusters are marked free in bitmap: %s' % sorted(free))
        return files, problems


def exfat_problems(image_path: pathlib.Path, expected: dict[str, bytes]) -> list[str]:
    """fsck plus the clean-run bitmap/dirty/entry invariants and all expected bytes."""
    problems = []
    try:
        problems.extend(_token_problems(image_path))
        result = DiskRecipes.run_tool('fsck.exfat', ['-n', str(image_path)])
        if result.returncode:
            problems.append('fsck.exfat -n exited %d: %s' % (
                result.returncode, (result.stdout + result.stderr).decode(errors='replace').strip()))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        problems.append('cannot run exFAT host check: %s' % error)
    try:
        files, findings = decode_exfat(image_path)
        problems.extend(findings)
        wanted = {**expected, 'hello': DiskRecipes.hello_page(0) + DiskRecipes.hello_page(1)}
        folded = {name.upper(): content for name, content in files.items()}
        for name, content in wanted.items():
            if name.upper() not in folded:
                problems.append('exFAT %s is missing' % name)
            elif folded[name.upper()] != content:
                problems.append('exFAT %s differs from expected bytes' % name)
    except (OSError, ValueError) as error:
        problems.append('cannot decode exFAT image: %s' % error)
    return problems


def _crc_table() -> tuple[int,...]:
    table = []
    for byte in range(256):
        state = byte
        for _ in range(8):
            state = (state >> 1) ^ (0x82F63B78 if state & 1 else 0)
        table.append(state)
    return tuple(table)


_CRC_TABLE = _crc_table()


def crc32c(data: bytes, seed: int = 0) -> int:
    """Reflected Castagnoli; seed is the previous *finalized* CRC (Crc32c.hpp)."""
    state = seed ^ 0xFFFFFFFF
    for byte in data:
        state = (state >> 8) ^ _CRC_TABLE[(state ^ byte) & 255]
    return state ^ 0xFFFFFFFF


def checksum(data: bytes, crc_offset: int, location: int, generation: int) -> int:
    """LE u64 location, LE u64 generation, encoded bytes with CRC logically zero."""
    return crc32c(struct.pack('<QQ', location, generation) + data[:crc_offset] +
                  bytes(4) + data[crc_offset + 4:])


def _u(data: bytes, offset: int, width: int = 4) -> int:
    return int.from_bytes(data[offset:offset + width], 'little')


def _need(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _superblock(data: bytes, sector: int) -> tuple[int, int, int]:
    _need(data[:8] == b'ASTRFS00', 'superblock magic')
    _need((_u(data, 8), _u(data, 12), _u(data, 16)) == (0, 4096, 512), 'superblock version/geometry')
    generation, table, next_id = _u(data, 20, 8), _u(data, 28), _u(data, 40, 8)
    _need(generation > 0 and 16 <= table < 512 and 1 <= next_id <= 127, 'superblock generation/table/object count')
    _need(not any(data[36:40] + data[208:508]), 'superblock reserved fields')
    _need(_u(data, 508) == checksum(data, 508, sector, generation), 'superblock CRC')
    previous = generation + 1
    for slot in range(4):
        committed = data[48 + slot * 16:64 + slot * 16]
        record_generation = _u(data, 176 + slot * 8, 8)
        _need(record_generation <= generation and (record_generation > 0 or not any(committed)), 'commit record generation')
        _need(not record_generation or record_generation < previous, 'commit generations out of order')
        previous = record_generation
    return generation, table, next_id


def _metadata(image: bytes, block: int, generation: int, kind: int) -> bytes:
    _need(16 <= block < 512, 'metadata block %d lies outside CoW sectors (0-127 reserved)' % block)
    data = image[block * 4096:(block + 1) * 4096]
    _need(data[:8] == b'ASTRMET0' and _u(data, 8) == kind, 'metadata magic/kind at block %d' % block)
    _need(_u(data, 12, 8) == generation and not any(data[24:32]), 'metadata generation/padding at block %d' % block)
    _need(_u(data, 20) == checksum(data, 20, block, generation), 'metadata CRC at block %d' % block)
    return data


def _tree(image: bytes, sb: bytes, generation: int, table_block: int, next_id: int) -> dict[bytes, bytes]:
    table = _metadata(image, table_block, generation, 1)
    _need(_u(table, 20) == _u(sb, 32), 'object table CRC differs from superblock')
    objects = []
    for index in range(127):
        entry = table[32 + index * 32:64 + index * 32]
        birth, size, kind = _u(entry, 0, 8), _u(entry, 8, 8), _u(entry, 16)
        direct = [_u(entry, 20 + k * 2, 2) for k in range(6)]
        if index >= next_id:
            _need(not any(entry), 'nonempty unused object %d' % index)
        else:
            _need(0 < birth <= generation and kind == (2 if index == 0 else 1), 'object %d birth/kind' % index)
            _need(size <= 6 * 4096 and size % 4096 == 0, 'object %d size' % index)
        objects.append((size, direct))
    size, root = objects[0]
    _need(size == 4096 and not any(root[1:]), 'root directory size/pointers')
    directory = _metadata(image, root[0], generation, 2)
    _need(not any(directory[4032:]), 'directory tail padding')
    reachable = set(range(16))

    def add(block: int) -> None:
        _need(16 <= block < 512, 'reachable block %d lies in reserved sectors or outside disk' % block)
        _need(block not in reachable, 'cross-linked block %d' % block)
        reachable.add(block)

    add(table_block)
    add(root[0])
    files = {}
    owners = set()
    ended = False
    previous = b''
    for index in range(50):
        entry = directory[32 + index * 80:112 + index * 80]
        length, object_id = _u(entry, 0), _u(entry, 72, 8)
        if length == 0:
            _need(not any(entry), 'nonzero unused directory entry')
            ended = True
            continue
        _need(not ended and 1 <= length <= 64, 'directory entry order/name length')
        name = entry[4:4 + length]
        _need(re.fullmatch(rb'[a-z0-9](?:[a-z0-9._-]*[a-z0-9])?', name) is not None, 'invalid directory name')
        _need(not any(entry[4 + length:72]), 'directory name/reserved padding')
        _need(name > previous and 0 < object_id < next_id and object_id not in owners, 'directory ordering/ownership')
        previous = name
        owners.add(object_id)
        size, direct = objects[object_id]
        pages = []
        for k, block in enumerate(direct):
            if block:
                _need(k * 4096 < size, 'object pointer past file size')
                add(block)
            if k * 4096 < size:
                pages.append(image[block * 4096:(block + 1) * 4096] if block else bytes(4096))
        files[name] = b''.join(pages)
    _need(owners == set(range(1, next_id)), 'directory does not name every allocated file exactly once')
    bitmap = {block for block in range(512) if sb[112 + block // 8] & (1 << (block % 8))}
    _need(bitmap == reachable, 'bitmap differs from reserved blocks plus reachable set')
    return files


def asteriafs_problems(image_path: pathlib.Path, expected: list[bytes]) -> list[str]:
    """Mount like C++ (generation descending, A wins ties), compare final hello pages.

    A damaged/blank superblock or stale older tree can be ignored at mount.
    A checksummed newer superblock pointing at corrupt metadata is reported,
    even when fallback is possible: the demo must leave a valid committed tree.
    Sector 64 belongs to the later block demo and is deliberately not compared.
    """
    try:
        image = image_path.read_bytes()
        problems = _token_problems(image_path)
        if len(image) != Machine.DISK_SIZE_BYTES:
            return problems + ['AsteriaFS image must be exactly 2 MiB']
        for block in range(16):
            if image[block * 4096:block * 4096 + 8] == b'ASTRMET0':
                problems.append('AsteriaFS metadata at block %d lies in reserved sectors 0-127' % block)
        candidates, rejected = [], []
        for sector in (0, 16):
            sb = image[sector * 512:(sector + 1) * 512]
            try:
                generation, table, next_id = _superblock(sb, sector)
                candidates.append((generation, sector, sb, table, next_id))
            except ValueError as error:
                rejected.append('superblock at sector %d: %s' % (sector, error))
        for generation, sector, sb, table, next_id in sorted(candidates, key=lambda row: (-row[0], row[1])):
            try:
                files = _tree(image, sb, generation, table, next_id)
            except ValueError as error:
                message = 'generation %d at sector %d: %s' % (generation, sector, error)
                rejected.append(message)
                problems.append(message)
                continue
            if any(len(page) != 4096 for page in expected):
                problems.append('expected hello pages must each be 4096 bytes')
            if files.get(b'hello') != b''.join(expected):
                problems.append('hello differs from final committed pages at generation %d (sector %d)' % (generation, sector))
            return problems
        return problems + ['no mountable AsteriaFS superblock: ' + '; '.join(rejected)]
    except (OSError, ValueError) as error:
        return ['cannot check AsteriaFS image: %s' % error]


def check(context, isa: str, asteriafs_pages: list[bytes]) -> None:
    """The disk after the filesystem demonstration, checked with tools and models of its own on
    either ISA (`isa` names the checks: amd64 or ia64): the stopped guest has
    finished its disk I/O. On AsteriaFS hello holds `asteriafs_pages`, the
    pages its last commit, the one crashed after its superblock, left."""
    if not context.case.guest or not context.case.disk:
        return
    image = context.artifact('Disk.raw')
    if context.case.disk == 'raw':
        context.check_problems('host.%s.disk.asteriafs' % isa, asteriafs_problems(image, asteriafs_pages))
    elif context.case.disk in ('exfat', 'exfat-v2'):
        context.check_problems('host.%s.disk.exfat' % isa, exfat_problems(image, DiskRecipes.after_run_expected(context.case.disk)))
    else:
        context.check_problems('host.%s.disk.fat' % isa, fat_problems(
            image, context.case.disk, DiskRecipes.after_run_expected(context.case.disk)))
