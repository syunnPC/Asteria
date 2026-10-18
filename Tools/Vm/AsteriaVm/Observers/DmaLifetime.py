# SPDX-License-Identifier: Apache-2.0
"""AHCI block I/O: active and retired DMA storage share a bound."""
import functools
import pathlib

from AsteriaVerify import Layout


@functools.lru_cache(maxsize=1)
def mapping_capacity() -> int:
    root = pathlib.Path(__file__).resolve().parents[4]
    return Layout.constants(Layout.read(root / 'Source/Kernel/Device/IommuDomain.hpp'))['MaximumMappings']


def maximum_pins(storage_drivers: int, ordinary_pins: int, translation_only_drivers: int = 0) -> int:
    # The edu diagnostic's source and target remain retained after its
    # attempted Detach on translation-only domains. Services/NICs keep
    # their fixed arrangements; storage drivers can retire further extents.
    return ordinary_pins + mapping_capacity() * storage_drivers + 2 * translation_only_drivers


def pin_problems(evidence, least: int, most: int) -> list[str]:
    problems = []
    if not least <= evidence.PinsParked <= most:
        problems.append('%d Frame pins parked, expected %d to %d' % (evidence.PinsParked, least, most))
    if evidence.PinsReleased != evidence.PinsParked or evidence.ParkedPins != 0:
        problems.append('the Frame pins parked, released and still parked is %r, expected (%d, %d, 0)' % (
            (evidence.PinsParked, evidence.PinsReleased, evidence.ParkedPins),
            evidence.PinsParked, evidence.PinsParked))
    return problems
