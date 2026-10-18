# SPDX-License-Identifier: Apache-2.0
"""UEFI system-table facts after exit, observed through physical memory.

Both target profiles use the UEFI 64-bit table ABI. The loader clears all
expired boot/console fields and refreshes IEEE CRC32 after exit; the kernel
receives only BootInfo. No firmware pointer is an input to kernel entry.
"""
from __future__ import annotations

import re
import struct
import zlib

PREFIX = 'ASTERIA:BOOT-SERVICES:ABSENT system-table='


def system_table(lines: list[str]) -> tuple[int | None, list[str]]:
    candidates = [line for line in lines if line.startswith(PREFIX)]
    if len(candidates) != 1 or not re.fullmatch(re.escape(PREFIX) + r'0x[0-9A-Fa-f]{16}', candidates[0]):
        return None, ['missing, duplicate or malformed absent-boot-services fact']
    address = int(candidates[0].split('0x')[1], 16)
    if not address or address & 7:
        return None, ['system-table address is zero or unaligned']
    return address, []


def problems(data: bytes) -> list[str]:
    if len(data) < 120:
        return ['system table is truncated']
    signature, revision, size, crc, reserved = struct.unpack_from('<QIIII', data)
    if signature != 0x5453595320494249 or revision < 0x1000A or reserved or not 120 <= size <= 512:
        return ['system table header is invalid']
    if len(data) != size:
        return ['system table does not match its declared extent']
    cleared_crc = data[:16] + bytes(4) + data[20:]
    errors = [] if zlib.crc32(cleared_crc) == crc else ['system table CRC32 is invalid']
    if any(data[40:88]) or any(data[96:104]):
        errors.append('boot services or console fields remain available')
    return errors
