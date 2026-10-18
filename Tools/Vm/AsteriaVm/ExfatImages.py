# SPDX-License-Identifier: Apache-2.0
"""Small host exFAT fixture writer and bounded, read-only layout helpers.

Format source: https://learn.microsoft.com/en-us/windows/win32/fileio/exfat-specification
The writer creates ASCII root files with contiguous allocations, never grows
the root, and places each entry set in one sector. The observer
validates entry sets independently; it does not call the writer's encoder.
"""
from __future__ import annotations

import dataclasses
import pathlib
import struct

SECTOR_SIZE = 512
ENTRY_SIZE = 32


def uint(data: bytes, offset: int, width: int = 4) -> int:
    if offset < 0 or offset + width > len(data):
        raise ValueError('truncated exFAT field')
    return int.from_bytes(data[offset:offset + width], 'little')


def rotate_sum(data: bytes, bits: int, skip: tuple[int,...] = ()) -> int:
    value = 0
    for index, byte in enumerate(data):
        if index not in skip:
            value = (((value & 1) << (bits - 1)) + (value >> 1) + byte) & ((1 << bits) - 1)
    return value


def boot_checksum(region: bytes) -> int:
    if len(region) != 11 * SECTOR_SIZE:
        raise ValueError('boot checksum needs eleven sectors')
    return rotate_sum(region, 32, (106, 107, 112))


def entry_set_checksum(entries: bytes) -> int:
    return rotate_sum(entries, 16, (2, 3))


def name_hash(name: str) -> int:
    name.encode('ascii')  # Fixture names use only the mandatory ASCII up-case mappings.
    return rotate_sum(name.upper().encode('utf-16le'), 16)


@dataclasses.dataclass(frozen=True)
class Geometry:
    sectors: int
    fat_sector: int
    fat_sectors: int
    heap_sector: int
    cluster_count: int
    root_cluster: int
    sectors_per_cluster: int

    @property
    def cluster_bytes(self) -> int:
        return self.sectors_per_cluster * SECTOR_SIZE


def parse_boot(boot: bytes, image_size: int) -> Geometry:
    """Validate the single-FAT, 512-byte-sector geometry before following pointers."""
    if (len(boot) != SECTOR_SIZE or boot[:3] != b'\xeb\x76\x90' or
            boot[3:11] != b'EXFAT   ' or any(boot[11:64]) or
            boot[510:512] != b'\x55\xaa' or uint(boot, 104, 2) != 0x100 or
            boot[108] != 9 or boot[109] > 16 or boot[110] != 1 or
            uint(boot, 64, 8) != 0 or uint(boot, 106, 2) & ~0xE or
            (boot[112] > 100 and boot[112] != 255) or any(boot[113:120])):
        raise ValueError('invalid exFAT boot sector or unsupported geometry')
    geometry = Geometry(uint(boot, 72, 8), uint(boot, 80), uint(boot, 84),
                        uint(boot, 88), uint(boot, 92), uint(boot, 96), 1 << boot[109])
    if not (24 <= geometry.fat_sector and geometry.fat_sectors > 0 and
            0 < geometry.cluster_count <= 0xFFFFFFF5 and
            (geometry.cluster_count + 2) * 4 <= geometry.fat_sectors * SECTOR_SIZE and
            geometry.fat_sector + geometry.fat_sectors <= geometry.heap_sector and
            geometry.heap_sector + geometry.cluster_count * geometry.sectors_per_cluster <= geometry.sectors and
            geometry.sectors * SECTOR_SIZE <= image_size and
            2 <= geometry.root_cluster < geometry.cluster_count + 2):
        raise ValueError('exFAT geometry lies outside the image')
    return geometry


class Volume:
    """Bounded I/O and FAT traversal shared by the writer and independent observer."""

    def __init__(self, path: pathlib.Path, mode: str = 'rb'):
        self.handle = path.open(mode)
        try:
            self.size = path.stat().st_size
            self.boot = self.read(0, SECTOR_SIZE)
            self.geometry = parse_boot(self.boot, self.size)
        except Exception:
            self.handle.close()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.handle.close()

    def read(self, offset: int, length: int) -> bytes:
        if offset < 0 or length < 0 or offset + length > self.size:
            raise ValueError('exFAT read outside image')
        self.handle.seek(offset)
        data = self.handle.read(length)
        if len(data) != length:
            raise ValueError('truncated exFAT image')
        return data

    def write(self, offset: int, data: bytes) -> None:
        if offset < 0 or offset + len(data) > self.size:
            raise ValueError('exFAT write outside image')
        self.handle.seek(offset)
        self.handle.write(data)
        self.handle.flush()

    def cluster_offset(self, cluster: int) -> int:
        g = self.geometry
        if not 2 <= cluster < g.cluster_count + 2:
            raise ValueError('invalid exFAT cluster %d' % cluster)
        return g.heap_sector * SECTOR_SIZE + (cluster - 2) * g.cluster_bytes

    def chain(self, first: int) -> list[int]:
        clusters, seen = [], set()
        cluster = first
        while cluster != 0xFFFFFFFF:
            self.cluster_offset(cluster)
            if cluster in seen:
                raise ValueError('cyclic exFAT FAT chain')
            seen.add(cluster)
            clusters.append(cluster)
            cluster = uint(self.read(self.geometry.fat_sector * SECTOR_SIZE + cluster * 4, 4), 0)
        return clusters

    def allocation(self, first: int, length: int, no_fat_chain: bool = False) -> list[int]:
        g = self.geometry
        count = (length + g.cluster_bytes - 1) // g.cluster_bytes
        if not count:
            if first or no_fat_chain:
                raise ValueError('invalid empty exFAT allocation')
            return []
        if count > g.cluster_count:
            raise ValueError('exFAT allocation exceeds cluster heap')
        clusters = list(range(first, first + count)) if no_fat_chain else self.chain(first)
        if len(clusters) != count:
            raise ValueError('exFAT FAT chain length differs from DataLength')
        for cluster in clusters:
            self.cluster_offset(cluster)
        return clusters

    def data(self, clusters: list[int], length: int) -> bytes:
        return b''.join(self.read(self.cluster_offset(c), self.geometry.cluster_bytes)
                        for c in clusters)[:length]

    def root_entries(self) -> list[tuple[int, bytes]]:
        return [(self.cluster_offset(c) + offset,
                 self.read(self.cluster_offset(c) + offset, ENTRY_SIZE))
                for c in self.chain(self.geometry.root_cluster)
                for offset in range(0, self.geometry.cluster_bytes, ENTRY_SIZE)]


def stamp_boot_token(path: pathlib.Path, token: bytes) -> None:
    """Restore sector 8 and its backup, then recompute both boot checksums."""
    if len(token) != SECTOR_SIZE:
        raise ValueError('exFAT token must fill one sector')
    with Volume(path, 'r+b') as volume:
        for base in (0, 12):
            volume.write((base + 8) * SECTOR_SIZE, token)
            checksum = boot_checksum(volume.read(base * SECTOR_SIZE, 11 * SECTOR_SIZE))
            volume.write((base + 11) * SECTOR_SIZE, struct.pack('<I', checksum) * 128)


def _entry_set(name: str, first: int, length: int) -> bytes:
    if not 1 <= len(name) <= 210 or any(ord(c) < 32 or c in '"*/:<>?\\|' for c in name):
        raise ValueError('invalid ASCII exFAT fixture name or entry set exceeds one sector')
    name.encode('ascii')
    encoded = name.encode('utf-16le')
    names = (len(name) + 14) // 15
    entries = bytearray((2 + names) * ENTRY_SIZE)
    entries[0:2] = bytes((0x85, 1 + names))
    struct.pack_into('<H', entries, 4, 0x20)  # Archive, regular file.
    timestamp = ((2024 - 1980) << 25) | (1 << 21) | (1 << 16)  # 2024-01-01 00:00:00.
    for offset in (8, 12, 16):
        struct.pack_into('<I', entries, offset, timestamp)
    entries[22:25] = b'\x80' * 3  # Valid UTC offsets, UTC+0.
    entries[32:36] = bytes((0xC0, 3 if length else 1, 0, len(name)))
    struct.pack_into('<H', entries, 36, name_hash(name))
    struct.pack_into('<Q', entries, 40, length)
    struct.pack_into('<I', entries, 52, first)
    struct.pack_into('<Q', entries, 56, length)
    for index in range(names):
        offset = (index + 2) * ENTRY_SIZE
        entries[offset] = 0xC1
        part = encoded[index * 30:(index + 1) * 30]
        entries[offset + 2:offset + 2 + len(part)] = part
    struct.pack_into('<H', entries, 2, entry_set_checksum(entries))
    return bytes(entries)


def _placement(volume: Volume, entries: bytes) -> tuple[int, list[tuple[int, bytes]]]:
    """Plan one-sector placement, deleting unused gaps before the new set."""
    gaps = []
    for cluster in volume.chain(volume.geometry.root_cluster):
        for relative in range(0, volume.geometry.cluster_bytes, SECTOR_SIZE):
            offset = volume.cluster_offset(cluster) + relative
            sector = bytearray(volume.read(offset, SECTOR_SIZE))
            for slot in range(0, SECTOR_SIZE - len(entries) + 1, ENTRY_SIZE):
                if all(not sector[i] & 0x80 for i in range(slot, slot + len(entries), ENTRY_SIZE)):
                    for i in range(0, slot, ENTRY_SIZE):
                        if sector[i] == 0:
                            sector[i] = 0x05
                    sector[slot:slot + len(entries)] = entries
                    return offset + slot, gaps + [(offset, bytes(sector))]
            # A subsequent sector must not follow an end-of-directory marker.
            for slot in range(0, SECTOR_SIZE, ENTRY_SIZE):
                if sector[slot] == 0:
                    sector[slot] = 0x05
            gaps.append((offset, bytes(sector)))
    raise ValueError('exFAT root has no sector space for the entry set')


def write_file(path: pathlib.Path, name: str, content: bytes) -> int:
    """Create an ASCII root file with NoFatChain; return its entry-set byte offset.

    Empty files have no allocation and no NoFatChain. Existing files are refused.
    This fixture helper is not a crash/replace implementation.
    """
    with Volume(path, 'r+b') as volume:
        root = volume.root_entries()
        bitmaps = [entry for _, entry in root if entry[0] == 0x81]
        if len(bitmaps) != 1 or bitmaps[0][1] != 0:
            raise ValueError('exFAT fixture needs one allocation bitmap')
        for index, (_, entry) in enumerate(root):
            if entry[0] == 0:
                break
            if entry[0] == 0x85:
                stream = root[index + 1][1]
                raw_name = b''.join(item[2:32] for _, item in root[index + 2:index + 1 + entry[1]])
                existing = raw_name[:stream[3] * 2].decode('utf-16le')
                if existing.upper() == name.upper():
                    raise ValueError('exFAT file already exists: %s' % name)
        bitmap_entry = bitmaps[0]
        bitmap_length = uint(bitmap_entry, 24, 8)
        if bitmap_length != (volume.geometry.cluster_count + 7) // 8:
            raise ValueError('invalid exFAT allocation bitmap length')
        bitmap_clusters = volume.allocation(uint(bitmap_entry, 20), bitmap_length)
        bitmap = bytearray(volume.data(bitmap_clusters, bitmap_length))
        count = (len(content) + volume.geometry.cluster_bytes - 1) // volume.geometry.cluster_bytes
        first, run = 0, 0
        for bit in range(volume.geometry.cluster_count) if count else ():
            run = 0 if bitmap[bit // 8] & (1 << (bit % 8)) else run + 1
            if run == count:
                first = bit + 3 - count
                break
        if count and not first:
            raise ValueError('exFAT has no contiguous free run')
        entry_offset, writes = _placement(volume, _entry_set(name, first, len(content)))
        if content:
            volume.write(volume.cluster_offset(first), content + bytes(count * volume.geometry.cluster_bytes - len(content)))
        for cluster in range(first, first + count):
            bit = cluster - 2
            bitmap[bit // 8] |= 1 << (bit % 8)
        for index, cluster in enumerate(bitmap_clusters):
            part = bitmap[index * volume.geometry.cluster_bytes:(index + 1) * volume.geometry.cluster_bytes]
            volume.write(volume.cluster_offset(cluster), part)
        for offset, sector in writes:
            volume.write(offset, sector)
        return entry_offset
