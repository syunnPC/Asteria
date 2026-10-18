# SPDX-License-Identifier: Apache-2.0
"""BOOT.CFG trust entries for the trusted-driver profile's cases.

A machine without a usable IOMMU runs a DMA driver without isolation only for a function BOOT.CFG names
with its driver and the SHA-256 of the module that carries the driver. The
digests come from the build (`ModuleDigests.txt`, written by
Tools/Build/WriteModuleDigests.cpp with the boot's own SHA-256 beside each
firmware's ESP), checked here against Python's hashlib over the case ESP's
module, so no case hard-codes a digest. The functions are the QEMU machines'
fixed ones (Machine.py places them).
"""
from __future__ import annotations

import hashlib
import pathlib

from AsteriaVm import Build  # Noqa: F401 (puts Tools/Verify on sys.path)
from AsteriaVerify import BootFiles as Manifest

DIGESTS = 'ModuleDigests.txt'
# The module every driver runs from (Abi::PagerDriverModuleName), but the USB host group driver's,
# which runs from its own (Abi::PagerUsbHostModuleName).
DRIVER_MODULE = 'component'
DRIVER_MODULES = {'usb': 'usb'}
# (address or None, (vendor, device) or None) of each trusted function, by ISA
# and driver: q35's ICH9 AHCI at 00:1f.2, Machine.py's qemu-xhci at 08.0 and
# e1000e at 10.0; on itanium2-vpc the built-in AHCI at 01.0, named by its
# identifier alone, and Machine.py's e1000e at 10.0, named by both (every
# selector form works). A driver of several functions has a list: the USB
# host group's ICH9 EHCI and UHCI companions (Machine.USB_HOST_CONTROLLERS),
# one entry each.
FUNCTIONS = {
    ('Amd64', 'ahci'): ((0, 0, 0x1f, 2), (0x8086, 0x2922)),
    ('Amd64', 'xhci'): ((0, 0, 0x08, 0), (0x1b36, 0x000d)),
    ('Amd64', 'e1000e'): ((0, 0, 0x10, 0), None),
    ('Amd64', 'usb'): [((0, 0, 0x1d, 7), None), ((0, 0, 0x1d, 0), None), ((0, 0, 0x1d, 1), None)],
    ('Ia64', 'ahci'): (None, (0x8086, 0x2922)),
    ('Ia64', 'e1000e'): ((0, 0, 0x10, 0), (0x8086, 0x10d3)),
}


def entries(isa: str, drivers: tuple[str,...]) -> list[tuple[str, tuple]]:
    """(driver, (address, identifier)) of each trust line `drivers` give, in
    BOOT.CFG order: one per function a driver serves."""
    found = []
    for driver in drivers:
        functions = FUNCTIONS[(isa, driver)]
        found += [(driver, function) for function in (functions if isinstance(functions, list) else [functions])]
    return found


def module_digests(esp: pathlib.Path) -> dict[str, tuple[str, bytes]]:
    """The build's digests beside `esp`, by module name: (file, digest)."""
    digests = {}
    for line in (esp.parent / DIGESTS).read_text().splitlines():
        name, file, digest = line.split(' ')
        if not digest.startswith('sha256=') or len(digest) != 7 + 64:
            raise ValueError('malformed module digest line %r' % line)
        digests[name] = file, bytes.fromhex(digest[7:])
    return digests


def lines(isa: str, drivers: tuple[str,...], esp: pathlib.Path, case_esp: pathlib.Path) -> list[str]:
    """The trust lines naming `drivers`' functions and the driver module's
    build digest, which must be hashlib's over the case ESP's module."""
    if not drivers:
        return []
    digests = module_digests(esp)
    lines = []
    for driver, function in entries(isa, drivers):
        name = DRIVER_MODULES.get(driver, DRIVER_MODULE)
        file, digest = digests[name]
        module = case_esp / 'EFI' / 'ASTERIA' / 'MODULES' / file
        if hashlib.sha256(module.read_bytes()).digest() != digest:
            raise ValueError('%s: build digest differs from SHA-256 of %s' % (DIGESTS, module))
        lines.append(Manifest.trust_line(name, digest, driver, *function))
    return lines
