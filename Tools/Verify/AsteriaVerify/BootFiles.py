# SPDX-License-Identifier: Apache-2.0
"""boot file contract ESP manifest and CRC-32C facts, shared with VM observers."""
from __future__ import annotations

import re
import pathlib
from AsteriaVerify import Layout

MAXIMUM_MODULES = Layout.constants(Layout.read(pathlib.Path(__file__).resolve().parents[3]
    / "Source/Abi/ComponentImageTable.hpp"))["ComponentImageMaximumCount"]

MAXIMUM_WATCHDOG_SECONDS = Layout.constants(Layout.read(pathlib.Path(__file__).resolve().parents[3]
    / "Source/Abi/Watchdog.hpp"))["WatchdogMaximumSeconds"]

MAXIMUM_MANIFEST_BYTES = 4096
MAXIMUM_MODULE_BYTES = 2 * 1024 * 1024
MAXIMUM_FILE_BYTES = 16 * 1024 * 1024
MODULE = re.compile(r'module\s*=\s*([a-z0-9-]{1,15})\s+([A-Z0-9_]{1,8}\.[A-Z0-9_]{1,3})\s+crc32c=0x([0-9a-fA-F]{8})')
ENTRY = re.compile(r'(kernel|init|user|spinner)\s*=\s*([A-Z0-9_]{1,8}\.[A-Z0-9_]{1,3})\s+crc32c=0x([0-9a-fA-F]{8})')
# The trusted-driver allowlist.
MAXIMUM_TRUST_ENTRIES = 8
TRUST_DRIVERS = ('ahci', 'e1000e', 'xhci', 'usb')
TRUST = re.compile(r'trust[ \t\r]*=[ \t\r]*'
                   r'(?:pci=([0-9a-fA-F]{4}):([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])[ \t\r]+)?'
                   r'(?:id=([0-9a-fA-F]{4}):([0-9a-fA-F]{4})[ \t\r]+)?'
                   r'driver=(ahci|e1000e|xhci|usb)[ \t\r]+module=([a-z0-9-]{1,15})[ \t\r]+sha256=([0-9a-fA-F]{64})')


def trust_line(module: str, digest: bytes, driver: str, address: tuple[int, int, int, int] | None = None,
               identifier: tuple[int, int] | None = None) -> str:
    """One canonical `trust =` line for a function, a driver and the module's digest."""
    if driver not in TRUST_DRIVERS or (address is None and identifier is None) or len(digest) != 32:
        raise ValueError('invalid trust entry')
    words = ['trust =']
    if address is not None:
        words.append('pci=%04x:%02x:%02x.%x' % address)
    if identifier is not None:
        words.append('id=%04x:%04x' % identifier)
    words += ['driver=' + driver, 'module=' + module, 'sha256=' + digest.hex()]
    return ' '.join(words)


def trust_entries(data: bytes) -> list[dict]:
    """The manifest's trust lines in order, each a dict of its fields (address and identifier None when absent)."""
    entries = []
    for line in _lines(data):
        match = TRUST.fullmatch(line)
        if match is None:
            continue
        groups = match.groups()
        entries.append({
            'address': None if groups[0] is None else tuple(int(value, 16) for value in groups[0:4]),
            'identifier': None if groups[4] is None else (int(groups[4], 16), int(groups[5], 16)),
            'driver': groups[6], 'module': groups[7], 'digest': bytes.fromhex(groups[8])})
    return entries


def _table() -> tuple[int,...]:
    values = []
    for byte in range(256):
        state = byte
        for _ in range(8):
            state = (state >> 1) ^ (0x82F63B78 if state & 1 else 0)
        values.append(state)
    return tuple(values)


_TABLE = _table()


def crc32c(data: bytes) -> int:
    """Reflected Castagnoli, initial/final XOR 0xffffffff, as Crc32c.hpp."""
    state = 0xffffffff
    for byte in data:
        state = (state >> 8) ^ _TABLE[(state ^ byte) & 255]
    return state ^ 0xffffffff


def _lines(data: bytes) -> list[str]:
    if len(data) > MAXIMUM_MANIFEST_BYTES:
        raise ValueError('manifest exceeds 4 KiB')
    try:
        text = data.decode('ascii')
    except UnicodeError as error:
        raise ValueError('manifest is not ASCII') from error
    lines = [line.split('#', 1)[0].strip(' \t\r') for line in text.split('\n')]
    return [line for line in lines if line]


def manifest(data: bytes) -> dict[str, tuple[str, int]]:
    """The manifest's files by key (`module:<name>` for modules), checked as strictly as the loader checks it."""
    lines = _lines(data)
    if not lines or not re.fullmatch(r'version[ \t\r]*=[ \t\r]*1', lines[0]):
        raise ValueError('first key must be version = 1')
    entries = {}
    display_seen = False
    watchdog_seen = False
    session_seen = False
    login_seen = False
    trusted = []
    for line in lines[1:]:
        if re.match(r'login[ \t\r]*=', line):
            if login_seen or not re.fullmatch(r'login[ \t\r]*=[ \t\r]*(on|off)', line):
                raise ValueError('invalid or duplicate login setting')
            login_seen = True
            continue
        if re.match(r'session[ \t\r]*=', line):
            if session_seen or not re.fullmatch(r'session[ \t\r]*=[ \t\r]*(desktop|console)', line):
                raise ValueError('invalid or duplicate session setting')
            session_seen = True
            continue
        if re.match(r'watchdog[ \t\r]*=', line):
            if watchdog_seen or not re.fullmatch(r'watchdog[ \t\r]*=[ \t\r]*(off|[0-9]{1,4})', line):
                raise ValueError('invalid or duplicate watchdog setting')
            value = line.split('=', 1)[1].strip()
            if value != 'off' and not 1 <= int(value) <= MAXIMUM_WATCHDOG_SECONDS:
                raise ValueError('watchdog seconds outside 1-60')
            watchdog_seen = True
            continue
        if re.match(r'trust[ \t\r]*=', line):
            match = TRUST.fullmatch(line)
            if match is None:
                raise ValueError('invalid trust entry %r' % line)
            groups = match.groups()
            if groups[0] is None and groups[4] is None:
                raise ValueError('trust entry %r names no function' % line)
            if groups[0] is not None and int(groups[2], 16) > 0x1f:
                raise ValueError('trust entry %r names device beyond 1f' % line)
            if groups[4] is not None and int(groups[4], 16) == 0xffff:
                raise ValueError('trust entry %r names vendor ffff' % line)
            address = None if groups[0] is None else tuple(int(value, 16) for value in groups[0:4])
            identifier = None if groups[4] is None else (int(groups[4], 16), int(groups[5], 16))
            for other_address, other_identifier, _ in trusted:
                if (address is not None and address == other_address) or (
                        address is None and other_address is None and identifier == other_identifier):
                    raise ValueError('duplicate trust entry %r' % line)
            if len(trusted) == MAXIMUM_TRUST_ENTRIES:
                raise ValueError('too many trust entries')
            trusted.append((address, identifier, groups[7]))
            continue
        display = re.fullmatch(r'display[ \t\r]*=[ \t\r]*(.*)', line)
        if display:
            if display_seen:
                raise ValueError('duplicate key display')
            display_seen = True
            value = display[1]
            if value not in ('native', 'keep'):
                size = re.fullmatch(r'([0-9]+)x([0-9]+)', value)
                if size is None or not all(0 < int(axis) <= 0xffffffff for axis in size.groups()):
                    raise ValueError('invalid display policy %r' % value)
            continue
        module = MODULE.fullmatch(line)
        entry = module or ENTRY.fullmatch(line)
        if entry is None:
            raise ValueError('invalid manifest entry %r' % line)
        key, name, crc = entry.groups()
        if module:
            key = 'module:' + key
            name = 'MODULES/' + name
            if sum(item.startswith('module:') for item in entries) == MAXIMUM_MODULES:
                raise ValueError('too many modules')
        if key in entries:
            raise ValueError('duplicate key %s' % key)
        entries[key] = name, int(crc, 16)
    if not {'kernel', 'init', 'user'} <= entries.keys():
        raise ValueError('missing kernel, init or user')
    for _, _, module in trusted:
        if 'module:' + module not in entries:
            raise ValueError('trust entry names module %s, which no module line names' % module)
    return entries
