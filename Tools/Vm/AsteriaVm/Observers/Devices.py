# SPDX-License-Identifier: Apache-2.0
"""PCI and AHCI host checks.

Ground truth comes from outside the guest: QEMU's own device model
(`query-pci`) and the disk fixture plus `-device ide-hd,serial=,model=`
arguments the runner itself wrote (`AsteriaVm.Machine`). What the guest
did is read from guest-physical memory: the ATA IDENTIFY DEVICE data and
the READ DMA EXT buffer its AHCI driver received by DMA. Nothing here is
specific to one ISA.
"""
from __future__ import annotations

import dataclasses
import pathlib
import struct

SECTOR_SIZE = 512


@dataclasses.dataclass(frozen=True)
class PciFunction:
    bus: int
    slot: int
    function: int
    vendor: int
    device: int
    class_code: int   # base class << 8 | subclass, as query-pci reports it

    def describe(self) -> str:
        return '%02x:%02x.%x %04x:%04x class %04x' % (self.bus, self.slot, self.function, self.vendor,
                                                     self.device, self.class_code)


def pci_functions(query_pci: list[dict]) -> list[PciFunction]:
    """Every function of every bus in a `query-pci` result, bridges' secondary buses included."""
    functions: list[PciFunction] = []

    def visit(devices: list[dict]) -> None:
        for device in devices:
            functions.append(PciFunction(device['bus'], device['slot'], device['function'],
                                         device['id']['vendor'], device['id']['device'],
                                         device['class_info']['class']))
            bridge = device.get('pci_bridge')
            if bridge:
                visit(bridge.get('devices', []))

    for bus in query_pci:
        visit(bus.get('devices', []))
    return functions


def function_count_problems(reported: int, functions: list[PciFunction]) -> list[str]:
    if reported == len(functions):
        return []
    return ['the guest enumerated %d PCI functions, QEMU has %d: %s' % (
        reported, len(functions), ', '.join(function.describe() for function in functions))]


# The disk fixture ----------------------------------------------------------

def fixture_sector(path: pathlib.Path, lba: int) -> bytes:
    with path.open('rb') as handle:
        handle.seek(lba * SECTOR_SIZE)
        data = handle.read(SECTOR_SIZE)
    if len(data) != SECTOR_SIZE:
        raise ValueError('%s has no sector %d' % (path, lba))
    return data


def fixture_sector_count(path: pathlib.Path) -> int:
    size = path.stat().st_size
    if size % SECTOR_SIZE:
        raise ValueError('%s is not a whole number of sectors' % path)
    return size // SECTOR_SIZE


# ATA IDENTIFY DEVICE data (ACS-3 7.12.7) ------------------------------------

@dataclasses.dataclass(frozen=True)
class IdentifyData:
    serial: str
    model: str
    lba48: bool
    sectors: int


def _ata_string(data: bytes, first_word: int, words: int) -> str:
    raw = bytearray()
    for index in range(first_word, first_word + words):
        raw += data[index * 2 + 1:index * 2 + 2] + data[index * 2:index * 2 + 1]
    return raw.decode('ascii', errors='replace').rstrip(' \0')


def decode_identify(data: bytes) -> IdentifyData:
    if len(data) != SECTOR_SIZE:
        raise ValueError('IDENTIFY data must be %d bytes, got %d' % (SECTOR_SIZE, len(data)))
    words = struct.unpack('<256H', data)
    command_sets = words[83]
    lba48 = command_sets & 0xC000 == 0x4000 and bool(command_sets & (1 << 10))
    if lba48:
        sectors = words[100] | words[101] << 16 | words[102] << 32 | words[103] << 48
    else:
        sectors = words[60] | words[61] << 16
    return IdentifyData(_ata_string(data, 10, 10), _ata_string(data, 27, 20), lba48, sectors)


def identify_problems(identify: IdentifyData, *, serial: str, model: str, sectors: int) -> list[str]:
    problems = []
    if identify.serial != serial:
        problems.append('IDENTIFY serial %r, expected %r' % (identify.serial, serial))
    if identify.model != model:
        problems.append('IDENTIFY model %r, expected %r' % (identify.model, model))
    if identify.sectors != sectors:
        problems.append('IDENTIFY reports %d sectors (%s), expected %d' % (
            identify.sectors, 'LBA48' if identify.lba48 else 'LBA28', sectors))
    return problems


def buffer_problems(observed: bytes, expected: bytes, what: str) -> list[str]:
    if observed == expected:
        return []
    if len(observed) != len(expected):
        return ['%s has %d bytes, expected %d' % (what, len(observed), len(expected))]
    offset = next(index for index, (left, right) in enumerate(zip(observed, expected)) if left != right)
    return ['%s differs from the fixture from offset %#x: %s vs %s' % (
        what, offset, observed[offset:offset + 16].hex(), expected[offset:offset + 16].hex())]
