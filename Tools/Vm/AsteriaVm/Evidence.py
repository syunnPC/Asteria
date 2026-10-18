# SPDX-License-Identifier: Apache-2.0
"""The kernel's evidence record: locate, read and decode it.

Where the kernel runs a scripted diagnostic scenario (a device read, an
application-processor timer, a TLB shootdown, a preemption proof), it
stores what it observed in one plain record per ISA instead of judging the
outcome, and reports the record's address once on the debug channel:
`ASTERIA:EVIDENCE:0x<16 upper-case hex digits>`
(Source/Kernel/Diagnostics/EvidenceRecord.hpp). The kernel image is
identity-mapped, so that address is the record's physical address and QMP
`pmemsave` reads the record whenever the guest is stopped.

The layout is never copied into Python. `Schema.load` parses the record's
struct declarations from the C++ headers of the tree and checks every
computed field offset and struct size against the headers' own
`static_assert(__builtin_offsetof(T, F) == N)` and
`static_assert(sizeof(T) == N)` assertions (AsteriaVerify.Layout); a
declaration it does not understand, or a layout that disagrees with the
assertions, raises SchemaError. The common header (magic, schema, size)
must match the ISA header's schema number and the record's size before
anything is decoded. A decoded record is a tree of `Record`s (attribute and
item access by field name), integers, bytes (UInt8 arrays) and lists (other
arrays).

Each sub-record has a publication word the kernel sets last, with a release
store (`Published`, or one `...Published` word per part); a sub-record
whose word is zero was not reached, and its other fields are zero.
"""
from __future__ import annotations

import dataclasses
import pathlib
import re
import struct as binary
from typing import Iterable, Mapping, Sequence

from AsteriaVm import Build  # Noqa: F401 (puts Tools/Verify on sys.path)
from AsteriaVerify import Layout

FACT_PREFIX = 'ASTERIA:EVIDENCE:'
_FACT = re.compile(r'^ASTERIA:EVIDENCE:0x(?P<address>[0-9A-F]{16})$')
COMMON_HEADER = 'Source/Kernel/Diagnostics/EvidenceRecord.hpp'
MAGIC_CONSTANT = 'EvidenceRecordMagic'
HEADER_TYPE = 'EvidenceRecordHeader'
HEADER_BYTES = 16
ARTIFACT = 'Evidence.bin'          # The record's bytes in a case's run directory
MAXIMUM_ARTIFACT_BYTES = 1 << 20   # What `save` copies of a record whose header it cannot vouch for

_PRIMITIVES = {'char': 1, 'UInt8': 1, 'UInt16': 2, 'UInt32': 4, 'UInt64': 8, 'Int64': 8, 'ProcessorMask': 8}
_FIELD = re.compile(r'^(?P<type>[A-Za-z_][\w:]*)\s+(?P<name>\w+)\s*(?:\[(?P<count>[^\]]+)\])?\s*;$')
_COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


class SchemaError(Exception):
    """The headers do not describe a record layout this module can decode."""


class EvidenceError(Exception):
    """The guest's record cannot be read or is not the record the build describes."""


def locate(lines: Iterable[str]) -> tuple[int | None, list[str]]:
    """The record's address from the single well-formed fact line."""
    candidates = [text for text in lines if text.startswith(FACT_PREFIX)]
    if len(candidates) != 1:
        return None, ['%d evidence fact lines (%r...), expected exactly one' % (len(candidates), FACT_PREFIX)]
    match = _FACT.match(candidates[0])
    if match is None:
        return None, ['malformed evidence fact line %r' % candidates[0]]
    return int(match.group('address'), 16), []


def save(qmp, lines: Iterable[str], path: pathlib.Path) -> str | None:
    """Keeps the record the fact line names as a failure artifact, as many
    bytes as its own header states (no schema is needed); returns why it
    could not, or None."""
    address, problems = locate(lines)
    if address is None:
        return '; '.join(problems)
    try:
        size = binary.unpack_from('<QII', qmp.pmemsave(address, HEADER_BYTES))[2]
        if not HEADER_BYTES <= size <= MAXIMUM_ARTIFACT_BYTES:
            return 'the record at %#x states an implausible size of %d bytes' % (address, size)
        path.write_bytes(qmp.pmemsave(address, size))
    except Exception as error:  # An artifact of a failed case: never mask the failure
        return '%s: %s' % (type(error).__name__, error)
    return None


class Record(dict):
    """One decoded structure: its fields by name, as items or attributes."""

    def __getattr__(self, name: str):
        try:
            return self[name]
        except KeyError:
            raise AttributeError(name) from None


@dataclasses.dataclass(frozen=True)
class Field:
    name: str
    offset: int
    element: 'str | Structure'    # A primitive name (UInt8..UInt64) or a structure
    count: int | None             # Array length, None for a scalar

    @property
    def element_size(self) -> int:
        return _PRIMITIVES[self.element] if isinstance(self.element, str) else self.element.size

    @property
    def size(self) -> int:
        return self.element_size * (self.count or 1)

    def decode(self, data: bytes, base: int):
        start = base + self.offset
        if isinstance(self.element, Structure):
            if self.count is None:
                return self.element.decode(data, start)
            return [self.element.decode(data, start + index * self.element.size) for index in range(self.count)]
        width = _PRIMITIVES[self.element]
        if self.count is None:
            return int.from_bytes(data[start:start + width], 'little', signed=self.element == 'Int64')
        if width == 1:
            return bytes(data[start:start + self.count])
        return [int.from_bytes(data[start + index * width:start + (index + 1) * width], 'little', signed=self.element == 'Int64')
                for index in range(self.count)]


@dataclasses.dataclass(frozen=True)
class Structure:
    name: str
    size: int
    alignment: int
    fields: tuple[Field,...]

    def field(self, name: str) -> Field:
        for field in self.fields:
            if field.name == name:
                return field
        raise KeyError('%s has no field %s' % (self.name, name))

    def decode(self, data: bytes, base: int = 0) -> Record:
        if len(data) < base + self.size:
            raise EvidenceError('%d bytes cannot hold %s (%d bytes at %d)' % (len(data), self.name, self.size, base))
        return Record((field.name, field.decode(data, base)) for field in self.fields)


def _declaration(sources: Mapping[str, str], name: str) -> tuple[str, str]:
    """(path, body) of the one `struct <name> {... };` among the sources."""
    pattern = re.compile(r'\bstruct\s+%s\s*\{(?P<body>.*?)\n\};' % re.escape(name), re.S)
    found = [(path, match.group('body')) for path, text in sources.items() for match in pattern.finditer(text)]
    if len(found) != 1:
        raise SchemaError('%d definitions of struct %s in %s, expected one' % (len(found), name, ', '.join(sources)))
    return found[0]


def structure(sources: Mapping[str, str], name: str, _parsed: dict[str, Structure] | None = None) -> Structure:
    """The layout of `struct <name>` (declared in one of `sources`, a map of
    path to text), with nested structures declared in any of them; checked
    against the declaring header's layout assertions."""
    parsed = {} if _parsed is None else _parsed
    if name in parsed:
        return parsed[name]
    path, body = _declaration(sources, name)
    text = sources[path]
    # Shared ABI bounds may be declared in another selected header. Read
    # those first, then the declaring header so its local names take priority.
    constants = Layout.constants('\n'.join([source for source_path, source in sources.items()
                                          if source_path != path] + [text]))
    fields: list[Field] = []
    offset, alignment = 0, 1
    for line in _COMMENT.sub('', body).splitlines():
        line = line.strip()
        if not line:
            continue
        match = _FIELD.match(line)
        if match is None:
            raise SchemaError('%s: struct %s declares %r, which is not a plain field' % (path, name, line))
        type_name = match.group('type').split('::')[-1]
        element: str | Structure = type_name if type_name in _PRIMITIVES else structure(sources, type_name, parsed)
        count = None
        if match.group('count') is not None:
            count = Layout.evaluate(match.group('count'), constants)
            if not count:
                raise SchemaError('%s: cannot evaluate the length of %s.%s' % (path, name, match.group('name')))
        field_alignment = _PRIMITIVES[element] if isinstance(element, str) else element.alignment
        offset = (offset + field_alignment - 1) // field_alignment * field_alignment
        field = Field(match.group('name'), offset, element, count)
        fields.append(field)
        offset += field.size
        alignment = max(alignment, field_alignment)
    if not fields:
        raise SchemaError('%s: struct %s has no fields' % (path, name))
    size = (offset + alignment - 1) // alignment * alignment
    try:
        asserted = Layout.offsets(text, name, [field.name for field in fields])
        asserted_size = Layout.size(text, name)
    except Layout.LayoutError as error:
        raise SchemaError('%s: %s' % (path, error)) from error
    for field in fields:
        if asserted[field.name] != field.offset:
            raise SchemaError('%s: %s.%s is asserted at %d, the declaration places it at %d' % (
                path, name, field.name, asserted[field.name], field.offset))
    if asserted_size != size:
        raise SchemaError('%s: sizeof(%s) is asserted as %d, the declaration gives %d' % (path, name, asserted_size,
                                                                                         size))
    parsed[name] = Structure(name, size, alignment, tuple(fields))
    return parsed[name]


@dataclasses.dataclass(frozen=True)
class Schema:
    """One ISA's record: the expected header values and the decoded layout."""
    magic: int
    version: int
    record: Structure

    @classmethod
    def load(cls, root: pathlib.Path, headers: Sequence[str], record: str, version: str) -> 'Schema':
        """The layout of `struct <record>` from `headers` (repository-relative;
        the first one declares the constant `version`, the record's schema
        number); the common evidence header is always included."""
        paths = list(dict.fromkeys([*headers, COMMON_HEADER]))
        sources = {path: Layout.read(pathlib.Path(root) / path) for path in paths}
        magic = Layout.constants(sources[COMMON_HEADER]).get(MAGIC_CONSTANT)
        schema = Layout.constants(sources[headers[0]]).get(version)
        if magic is None or schema is None:
            raise SchemaError('the headers lack %s or %s' % (MAGIC_CONSTANT, version))
        layout = structure(sources, record)
        header = layout.fields[0]
        if header.offset != 0 or not isinstance(header.element, Structure) or header.element.name != HEADER_TYPE:
            raise SchemaError('struct %s does not start with a %s' % (record, HEADER_TYPE))
        return cls(magic, schema, layout)

    def header_problems(self, data: bytes) -> list[str]:
        """Whether `data` starts with this record's header."""
        if len(data) < HEADER_BYTES:
            return ['%d bytes hold no evidence header' % len(data)]
        magic, version, size = binary.unpack_from('<QII', data)
        problems = []
        if magic != self.magic:
            problems.append('evidence magic %#018x, expected %#018x' % (magic, self.magic))
        if version != self.version:
            problems.append('evidence schema %d, the build describes schema %d' % (version, self.version))
        if size != self.record.size:
            problems.append('evidence record of %d bytes, the build describes %d' % (size, self.record.size))
        return problems

    def read(self, qmp, address: int) -> bytes:
        """The complete record at `address` of the stopped guest."""
        problems = self.header_problems(qmp.pmemsave(address, HEADER_BYTES))
        if problems:
            raise EvidenceError('%#x: %s' % (address, '; '.join(problems)))
        return qmp.pmemsave(address, self.record.size)

    def decode(self, data: bytes) -> Record:
        problems = self.header_problems(data)
        if problems or len(data) != self.record.size:
            raise EvidenceError('; '.join(problems) or '%d bytes, expected %d' % (len(data), self.record.size))
        return self.record.decode(data)
