# SPDX-License-Identifier: Apache-2.0
"""The independently linked kernel's note, entries and relocation closure."""
from __future__ import annotations

import dataclasses
import struct

from AsteriaVerify.Checks import Property, register
from AsteriaVerify.Elf import ElfFile


@dataclasses.dataclass(frozen=True)
class KernelFacts:
    machine: int
    kind: int
    entry: int
    note: bytes
    loads: tuple
    symbols: tuple
    descriptor: bytes
    relocations: tuple
    undefined: tuple


@register
class KernelElfImageContract(Property):
    """The kernel owns its entry metadata, reservations and W^X geometry.

    The source SHT_NOTE assembly is checked through its final exact bytes,
    including zero padding. IA-64 entries are owned descriptor pairs; their
    code/GP values and the SAL rendezvous are inside the placed kernel ranges.
    All retained relocations use the documented closed-kernel profile.
    """
    variants = ('firmware', 'firmware-test')

    def extract(self, context, variant):
        elf = ElfFile(context.artifact(variant + ':kernel_elf'))
        note = elf.section('.note.asteria')
        descriptor = elf.section('.opd')
        return KernelFacts(elf.machine, elf.type, elf.entry,
                           elf.data[note.offset:note.offset + note.size] if note else b'',
                           tuple((p.type, p.flags, p.virtual_address, p.memory_size) for p in elf.segments),
                           tuple((s.name, s.value) for s in elf.symbols if s.section_index),
                           elf.data[descriptor.offset:descriptor.offset + descriptor.size] if descriptor else b'',
                           tuple(r.type for section in elf.relocation_sections() for r in elf.relocations(section)),
                           tuple(s.name for s in elf.symbols if not s.section_index and s.name))

    def verify(self, facts):
        problems = []
        if facts.kind != 2 or facts.machine not in (50, 62) or facts.undefined:
            problems.append('kernel must be closed ELF64 ET_EXEC for the target ISA')
        if len(facts.note) != 4096 or facts.note[:20] != struct.pack('<III8s', 8, 360, 0x41535401, b'ASTERIA\0'):
            return problems + ['kernel note header/extent differs from the ABI']
        data = facts.note[20:380]
        if any(facts.note[380:]):
            problems.append('kernel note padding must be zero')
        version, boot, arch, count = struct.unpack_from('<IIII', data)
        base, size, alignment, prepare, enter, query, gp, descriptors, descriptor_bytes, reserved = struct.unpack_from(
            '<10Q', data, 16)
        ia64 = facts.machine == 50
        if (version, boot, arch, count, alignment, reserved) != (1, 22, 2 if ia64 else 1, 6 if ia64 else 7,
                                                                0x1000000 if ia64 else 0x200000, 0):
            problems.append('kernel note version, ISA or placement alignment differs')
        records = [struct.unpack_from('<QQII', data, 96 + index * 24) for index in range(7)]
        purposes = (0, 1, 1, 1, 2, 2) if ia64 else (0, 1, 1, 2, 1, 1, 2)
        if any(record != (0, 0, 0, 0) for record in records[count:]):
            problems.append('unused ownership records must be zero')
        if count != len(purposes) or any(pages == 0 or rva % 4096 or tail or purpose != expected
                                        or rva + pages * 4096 > size
                                        for (rva, pages, purpose, tail), expected in zip(records, purposes)):
            problems.append('ownership ranges must be aligned, bounded and preserve permission roles')
        loads = facts.loads
        if any(kind != 1 or flags not in (4, 5, 6) for kind, flags, _, _ in loads):
            problems.append('kernel has a non-load or W+X segment')
        actual = []
        for _, flags, start, extent in loads:
            cursor = start
            for rva, pages, purpose, _ in records[:count]:
                if start <= base + rva < start + extent:
                    if cursor != base + rva or purpose > 2 or flags != (5, 4, 6)[purpose]:
                        problems.append('ownership ranges differ from PT_LOAD geometry')
                    cursor = base + rva + pages * 4096
                    actual.append((rva, pages))
            if cursor != start + extent:
                problems.append('ownership ranges do not cover a load segment')
        if len(actual) != count or not loads or loads[-1][2] + loads[-1][3] != base + size:
            problems.append('image extent/ownership count differs')
        if ia64 and (size != 0x1000000 or loads != tuple((1, flags, base + (index + 1) * 0x400000, 0x400000)
                                                       for index, flags in enumerate((5, 4, 6)))):
            problems.append('IA-64 needs three 4 MiB permission groups')
        symbols = dict(facts.symbols)
        entries = ((prepare, 'AsteriaKernelPrepare'), (enter, 'AsteriaKernelEnter'), (query, 'AsteriaKernelQuery'))
        if facts.entry != base + enter or 'EfiMain' in symbols:
            problems.append('kernel entry or independent link boundary differs')
        def code(address):
            return any(flags == 5 and start <= address < start + extent for _, flags, start, extent in loads)
        if ia64:
            if len(facts.descriptor) != descriptor_bytes or not descriptor_bytes or descriptor_bytes % 16:
                problems.append('descriptor ownership extent differs')
            else:
                for offset in range(0, descriptor_bytes, 16):
                    target, pair_gp = struct.unpack_from('<QQ', facts.descriptor, offset)
                    if not code(target) or target % 16 or pair_gp != base + gp:
                        problems.append('descriptor pair contains foreign code or GP')
                for rva, name in entries:
                    offset = rva - descriptors
                    if offset < 0 or offset % 16 or offset + 16 > descriptor_bytes:
                        problems.append('entry does not name an owned descriptor')
                    elif struct.unpack_from('<Q', facts.descriptor, offset)[0] != symbols.get(name):
                        problems.append('entry descriptor does not name its exported function')
            if not code(symbols.get('AsteriaIa64SalBootRendezvousHandlerEntry', 0)):
                problems.append('SAL rendezvous does not name kernel text')
        elif gp or descriptors or descriptor_bytes or any(base + rva != symbols.get(name) or not code(base + rva)
                                                        for rva, name in entries):
            problems.append('AMD64 entries must name kernel code without descriptors')
        declarations = [struct.unpack_from('<QQQII', data, 264 + index * 32) for index in range(3)]
        if ia64:
            if (declarations[0][3:] != (0, 1) or declarations[1][3:] != (3, 2)
                    or declarations[1][0] != declarations[1][1] or declarations[0][0] >= declarations[1][0]
                    or declarations[2] != (0, 0, 0, 0, 0)):
                problems.append('IA-64 runtime guard/subview declarations differ')
        elif any(kind != index or flags or not pages or not align or align & (align - 1)
                 or (index == 1 and ceiling > 0xffffffff) or (index == 2 and ceiling > 0xfffff)
                 for index, (pages, align, ceiling, kind, flags) in enumerate(declarations)):
            problems.append('AMD64 reservation declarations differ')
        allowed = {0x23, 0x27, 0x47, 0x81, 0x49, 0x2a, 0x2b} if ia64 else {1, 2, 4, 8}
        if not facts.relocations or set(facts.relocations) - allowed:
            problems.append('kernel relocation closure differs from its documented profile')
        return problems

    def mutants(self, facts):
        yield 'firmware entry in kernel', dataclasses.replace(facts, symbols=facts.symbols + (('EfiMain', facts.entry),))
        yield 'undefined firmware import', dataclasses.replace(facts, undefined=('EfiMain',))
        yield 'unsupported relocation', dataclasses.replace(facts, relocations=facts.relocations + (0xffff,))
        yield 'wrong entry', dataclasses.replace(facts, entry=facts.entry + 8)
        for offset in (4, 20, 24, 28, 52, 116, 132, 312, 380):
            note = bytearray(facts.note)
            note[offset] ^= 0xff
            yield 'note mutation at %#x' % offset, dataclasses.replace(facts, note=bytes(note))
        if facts.machine == 50:
            descriptors = bytearray(facts.descriptor)
            descriptors[8] ^= 0x80
            yield 'stale descriptor GP', dataclasses.replace(facts, descriptor=bytes(descriptors))
