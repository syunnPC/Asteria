# SPDX-License-Identifier: Apache-2.0
"""An authored ACPI DMAR for the synthetic-RMRR case.

QEMU's intel-iommu writes a DMAR of its own without RMRRs, and the kernel
uses a DMAR only when the firmware offers exactly one, so the case attaches
no IOMMU device and gives the guest this table through `-acpitable` instead.
It contains one INCLUDE_PCI_ALL remapping unit and one RMRR whose scope
names both Q35 EHCIs. The unit's register page has nothing behind it,
so the kernel refuses translation and boots
without DMA devices, while the RMRR still reserves both EHCIs. The case
checks the firmware report and kernel quarantine without assigning the EHCIs.

Layout from the Intel VT-d specification, chapter 8 (DMAR header 8.1, DRHD
8.3, RMRR 8.4, device scope 8.3.1).
"""
from __future__ import annotations

import struct

# QEMU's q35 intel-iommu register base; nothing decodes it in the case.
REGISTER_BASE = 0xFED9_0000
# A page range the guest's UEFI map leaves undescribed (the boot report names
# it a gap), clear of RAM and of every BAR QEMU's firmware assigns.
RMRR_BASE = 0xFEFF_C000
RMRR_LIMIT = 0xFEFF_FFFF
# The ICH9 EHCI functions explicitly attached by Machine.prepare.
EHCI_FUNCTIONS = ((0, 0x1A, 0), (0, 0x1D, 0))
# DMAR host address width field: the width minus one (39 bits).
HOST_ADDRESS_WIDTH = 38


def _scope(bus: int, device: int, function: int) -> bytes:
    """A PCI endpoint device scope (type 1) of one hop."""
    return struct.pack('<BBHBBBB', 1, 8, 0, 0, bus, device, function)


def table(register_base: int | None = REGISTER_BASE, rmrr_base: int = RMRR_BASE, rmrr_limit: int = RMRR_LIMIT,
          functions: tuple[tuple[int, int, int],...] = EHCI_FUNCTIONS) -> bytes:
    """The complete DMAR, header and checksum included."""
    if rmrr_base & 0xFFF or (rmrr_limit + 1) & 0xFFF or rmrr_limit < rmrr_base or (register_base is not None and register_base & 0xFFF):
        raise ValueError('RMRR and register base must be page aligned')
    unit = b'' if register_base is None else struct.pack('<HHBBHQ', 0, 16, 1, 0, 0, register_base)
    scopes = b''.join(_scope(*function) for function in functions)
    reserved = struct.pack('<HHHHQQ', 1, 24 + len(scopes), 0, 0, rmrr_base, rmrr_limit) + scopes
    body = struct.pack('<BB10x', HOST_ADDRESS_WIDTH, 0) + unit + reserved
    header = struct.pack('<4sIBB6s8sI4sI', b'DMAR', 36 + len(body), 1, 0, b'ASTRIA', b'RMRREVID', 1, b'ASTR', 1)
    data = bytearray(header + body)
    data[9] = -sum(data) & 0xFF
    return bytes(data)

# A page range inside q35's retained BootServicesData descriptor at 00900000h.
# The VM check verifies the final map itself rather than assume provenance.
BOOTSERVICES_RMRR_BASE = 0x00A0_0000
BOOTSERVICES_RMRR_LIMIT = BOOTSERVICES_RMRR_BASE + 4 * 4096 - 1


def boot_services_table() -> bytes:
    """RMRR-only DMAR; no absent register aperture holds BootServices out.

    Intel VT-d Architecture Specification rev. 3.4 §§8.1/8.4/8.3.1. The
    reservation names an unpopulated endpoint; the gate stops real masters.
    """
    return table(None, BOOTSERVICES_RMRR_BASE, BOOTSERVICES_RMRR_LIMIT, ((0, 0x1A, 0),))
