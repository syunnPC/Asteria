# SPDX-License-Identifier: Apache-2.0
"""Image records and mapping identities reported by the root loader."""
from __future__ import annotations

import pathlib
import struct

from AsteriaVm import Guest


def image_name(behavior: str) -> str:
    return {'display': 'display', 'directory': 'directory', 'client': 'client', 'interpret': 'interpreter',
            'interpreter': 'interpreter'}.get(behavior, 'component')


def layout(path: pathlib.Path) -> tuple[tuple[int, int], int]:
    """Staged/private segment counts and retained shared pages from the linked ELF."""
    data = path.read_bytes()
    offset, = struct.unpack_from('<Q', data, 32)
    stride, count = struct.unpack_from('<HH', data, 54)
    staged = private = pages = 0
    for index in range(count):
        kind, flags, _, address, _, file_bytes, memory_bytes, _ = struct.unpack_from(
            '<IIQQQQQQ', data, offset + index * stride)
        if kind != 1 or not memory_bytes:
            continue
        if flags & 2:
            private += 1
        else:
            if not file_bytes:
                raise ValueError('embedded shared segment has no file bytes')
            staged += 1
            pages += (address % 4096 + memory_bytes + 4095) // 4096
    return (staged, private), pages



def private_pages(path: pathlib.Path) -> int:
    """Private writable load pages each resident incarnation keeps."""
    from AsteriaVerify.Elf import ElfFile, PF_W, PT_LOAD
    return sum((segment.virtual_address % 4096 + segment.memory_size + 4095) // 4096
               for segment in ElfFile(path).segments
               if segment.type == PT_LOAD and segment.flags & PF_W)


def problems(lines: list[str], behaviors: tuple[str,...], images, *, extra=None) -> list[str]:
    """Require the same complete handles and generation after every restart.

    Each image segment stages on its first mapping only. Tags include the
    display broker's reserved 0x100, and all ordinary incarnation tags.
    """
    expected = {256 if behavior == 'display' else index + 1: image_name(behavior)
                for index, behavior in enumerate(behaviors)}
    expected.update(extra or {})
    counts = dict(images)
    found = {}
    identities = {}
    generations = {}
    errors = []
    for line in Guest.channel_lines(lines):
        if not line.startswith('# shared image:'):
            continue
        try:
            generation, segment, tag, frame, frame_auth, memory, memory_auth, staged = map(int, line[15:].split())
        except ValueError:
            errors.append('malformed shared image fact: %s' % line)
            continue
        name = expected.get(tag)
        if name is None or not 0 <= segment < counts[name][0] or staged not in (0, 1):
            errors.append('unexpected shared mapping: %s' % line)
            continue
        key = (name, segment)
        identity = (generation, frame, frame_auth, memory, memory_auth)
        if not all(identity):
            errors.append('zero generation or shared handle: %s' % line)
        if (tag, segment) in found:
            errors.append('duplicate shared mapping for incarnation %d segment %d' % (tag, segment))
        found[tag, segment] = identity
        if key in identities:
            if staged or identity != identities[key]:
                errors.append('restarted %s segment %d changed shared objects or staged again' % key)
        else:
            if not staged:
                errors.append('%s segment %d was never staged' % key)
            if any(identity[1:] == other[1:] for other in identities.values()):
                errors.append('different image segments reuse the same objects')
            identities[key] = identity
        if name in generations and generation != generations[name]:
            errors.append('%s changed image generation' % name)
        generations[name] = generation
    wanted = {(tag, segment) for tag, name in expected.items() for segment in range(counts[name][0])}
    if set(found) != wanted:
        errors.append('shared mapping facts missing: %s' % sorted(wanted - set(found)))
    return errors
