# SPDX-License-Identifier: Apache-2.0
"""IA-64 processor and device facts against QEMU's own view.

The kernel records what it found in its evidence record
(`Observers.Ia64Evidence`): how many processors joined the rendezvous, the
first PCI function its ECAM scan saw, how many functions it published and
which one is the AHCI controller. Ground truth comes from outside the guest:
`query-cpus-fast`, `query-pci` and the case itself. The AHCI READ DMA EXT
buffer, read at the address the record names, is compared with the fixture
sector the runner wrote (`Machine.write_disk_fixture`).

The `query-pci` walk mirrors `Observers.Devices` (shared with AMD64, still
changing while this was written); the two can merge once both settle.
"""
from __future__ import annotations

import dataclasses
import pathlib

SECTOR_SIZE = 512
AHCI_CLASS = (0x01, 0x06, 0x01)   # mass storage / SATA / AHCI 1.0 programming interface


@dataclasses.dataclass(frozen=True)
class PciAddress:
    segment: int
    bus: int
    device: int
    function: int

    def describe(self) -> str:
        return '%04x:%02x:%02x.%x' % (self.segment, self.bus, self.device, self.function)


@dataclasses.dataclass(frozen=True)
class PciFunction:
    """One function as `query-pci` reports it (QEMU has one PCI segment)."""
    address: PciAddress
    identifier: int          # device << 16 | vendor
    class_code: int          # base class << 8 | subclass

    def describe(self) -> str:
        return '%s id %#010x class %04x' % (self.address.describe(), self.identifier, self.class_code)


@dataclasses.dataclass(frozen=True)
class PciEcamFact:
    """The first function the kernel's ECAM scan found and its vendor/device word."""
    address: PciAddress
    identifier: int          # device << 16 | vendor


@dataclasses.dataclass(frozen=True)
class PciDiscoveryFact:
    functions: int           # functions the kernel published
    ahci: PciAddress
    identifier: int          # device << 16 | vendor
    class_code: tuple[int, int, int]


def pci_functions(query_pci: list[dict]) -> list[PciFunction]:
    """Every function of a `query-pci` result (bridges' secondary buses included), in address order."""
    functions: list[PciFunction] = []

    def visit(devices: list[dict]) -> None:
        for device in devices:
            functions.append(PciFunction(PciAddress(0, device['bus'], device['slot'], device['function']),
                                         device['id']['device'] << 16 | device['id']['vendor'],
                                         device['class_info']['class']))
            visit((device.get('pci_bridge') or {}).get('devices', []))

    for bus in query_pci:
        visit(bus.get('devices', []))
    return sorted(functions, key=lambda function: dataclasses.astuple(function.address))


def processor_problems(reported: int | None, query_cpus: list[dict], expected: int) -> list[str]:
    """The kernel's rendezvous count, QEMU's CPU list and the case agree."""
    problems = []
    if len(query_cpus) != expected:
        problems.append('QEMU reports %d CPUs, the case requests %d' % (len(query_cpus), expected))
    if reported != expected:
        problems.append('the kernel counted %s processors, expected %d' % (reported, expected))
    return problems


def ecam_problems(fact: PciEcamFact, query_pci: list[dict]) -> list[str]:
    """The kernel's first ECAM function is QEMU's lowest-addressed function."""
    functions = pci_functions(query_pci)
    if not functions:
        return ['QEMU reports no PCI function']
    first = functions[0]
    problems = []
    if fact.address != first.address:
        problems.append('the kernel reports %s as the first function, QEMU has %s first' % (
            fact.address.describe(), first.describe()))
    if fact.identifier != first.identifier:
        problems.append('the kernel read ID %#010x at %s, QEMU has %#010x' % (
            fact.identifier, fact.address.describe(), first.identifier))
    return problems


def discovery_problems(fact: PciDiscoveryFact, query_pci: list[dict]) -> list[str]:
    """The kernel published every QEMU function and chose QEMU's AHCI controller."""
    functions = pci_functions(query_pci)
    problems = []
    if fact.functions != len(functions):
        problems.append('the kernel published %d PCI functions, QEMU has %d: %s' % (
            fact.functions, len(functions), '; '.join(function.describe() for function in functions)))
    storage = [function for function in functions if function.class_code == AHCI_CLASS[0] << 8 | AHCI_CLASS[1]]
    if len(storage) != 1:
        return problems + ['QEMU has %d SATA functions, expected exactly one' % len(storage)]
    ahci = storage[0]
    if fact.ahci != ahci.address:
        problems.append('the kernel chose %s as the AHCI function, QEMU has it at %s' % (
            fact.ahci.describe(), ahci.address.describe()))
    if fact.identifier != ahci.identifier:
        problems.append('the kernel read AHCI ID %#010x, QEMU has %#010x' % (fact.identifier, ahci.identifier))
    if fact.class_code != AHCI_CLASS:
        problems.append('the kernel reports class %02x:%02x:%02x, expected AHCI %02x:%02x:%02x' % (
            *fact.class_code, *AHCI_CLASS))
    return problems


def fixture_geometry(path: pathlib.Path, lba: int) -> tuple[bytes, int]:
    """(sector `lba`, size in sectors) of the disk fixture the runner wrote."""
    data = path.read_bytes()
    if len(data) % SECTOR_SIZE or len(data) < (lba + 1) * SECTOR_SIZE:
        raise ValueError('%s is not a disk image holding sector %d' % (path, lba))
    return data[lba * SECTOR_SIZE:(lba + 1) * SECTOR_SIZE], len(data) // SECTOR_SIZE


def read_buffer_problems(buffer: bytes, sector: bytes) -> list[str]:
    """The kernel's READ DMA EXT buffer holds exactly the fixture sector."""
    if buffer == sector:
        return []
    if len(buffer) != len(sector):
        return ['the AHCI read buffer dump has %d bytes, expected %d' % (len(buffer), len(sector))]
    offset = next(index for index, (left, right) in enumerate(zip(buffer, sector)) if left != right)
    return ['the AHCI READ DMA EXT buffer differs from the fixture sector from offset %#x: %s, expected %s' % (
        offset, buffer[offset:offset + 16].hex(), sector[offset:offset + 16].hex())]
