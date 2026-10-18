# SPDX-License-Identifier: Apache-2.0
"""The IA-64 interruption vector table the CPU actually uses.

Three views of the live table (read through QMP at the relocated address of
`AsteriaIa64RuntimeInterruptVectorTableImage`):

- IVA holds exactly that relocated address.
- The live bytes equal the linked table's ELF bytes: the table
  is position-independent (its branches are IP-relative), so relocation and
  installation must not change a byte.
- The live table routes the break, external-interrupt, page-fault and
  exception vectors to the intended relocated entries and fail-stops
  everywhere else. The static IVT check applies the same rules to the ELF.
  The decoder below knows just the IA-64 encodings those trampolines use; it
  decodes the trampoline instructions from their IA-64 encodings.

Architectural facts (IA-64 SDM Vol. 2, IVT; Vol. 3, formats A5 and B1): the
table is 32 KiB; vectors below 0x5000 own 0x400-byte slots, the rest
0x100-byte slots; a bundle is 16 bytes with a 5-bit template and three 41-bit
slots.
"""
from __future__ import annotations

import dataclasses

from AsteriaVm.Observers.Ia64State import CpuState, StateError

TABLE_SIZE = 0x8000
BUNDLE_SIZE = 16
LONG_SLOT_LIMIT = 0x5000
LONG_SLOT_SIZE = 0x400
SHORT_SLOT_SIZE = 0x100
TEMPLATES_MIB = (0x10, 0x11)     # MIB, and MIB with a stop after slot 2
_SLOT_MASK = (1 << 41) - 1
_MOV_R16_IP = 0x180000400        # I25: mov r16=ip; no predicate, destination r16.
_NOP = 1 << 27                   # Nop.m 0 / nop.i 0 (M48/I18 x4|x6 = 1, qp 0, imm 0)


@dataclasses.dataclass(frozen=True)
class Route:
    """A vector whose first bundle is `nop.m 0 | mov.m ar.k4=<vector>;
    nop.i 0 | mov r16=<selector>; br.sptk.many <target>`, followed by
    fail-stop padding. The exception vectors leave their offset >> 8 in
    ar.k4 for the entry they share."""
    name: str
    offset: int
    target: int
    selector: int | None = None
    vector: int | None = None
    capture_ip: bool = False


def slot_size(offset: int) -> int:
    return LONG_SLOT_SIZE if offset < LONG_SLOT_LIMIT else SHORT_SLOT_SIZE


def decode(bundle: bytes) -> tuple[int, tuple[int, int, int]]:
    value = int.from_bytes(bundle, 'little')
    return value & 0x1F, tuple((value >> (5 + 41 * slot)) & _SLOT_MASK for slot in range(3))


def addl_immediate(slot: int, register: int) -> int | None:
    """imm22 of `addl r<register> = imm22, r0` (A5, printed `mov r<register>=imm`), else None."""
    if slot >> 37 != 9 or slot & 0x3F or (slot >> 6) & 0x7F != register or (slot >> 20) & 0x3:
        return None
    immediate = ((slot >> 36) & 1) << 21 | ((slot >> 22) & 0x1F) << 16 | ((slot >> 27) & 0x1FF) << 7 | (
        (slot >> 13) & 0x7F)
    return immediate - (1 << 22) if immediate & (1 << 21) else immediate


def move_to_application_register(slot: int, register: int) -> int | None:
    """imm8 of `mov.m ar<register> = imm8` (M30: opcode 0, x3 0, x2 2, x4 8),
    else None."""
    if slot >> 37 != 0 or (slot >> 33) & 0x7 or (slot >> 31) & 0x3 != 2 or (slot >> 27) & 0xF != 8:
        return None
    if slot & 0x3F or (slot >> 6) & 0x7F or (slot >> 20) & 0x7F != register:
        return None
    immediate = ((slot >> 36) & 1) << 7 | (slot >> 13) & 0x7F
    return immediate - (1 << 8) if immediate & (1 << 7) else immediate


def branch_target(slot: int, address: int) -> int | None:
    """Target of an unpredicated IP-relative `br.sptk.many` (B1, btype cond,
    printed `br.many`) in the bundle at `address`, else None."""
    if slot >> 37 != 4 or slot & 0x3F or (slot >> 6) & 0x7 or not (slot >> 12) & 1:
        return None
    if (slot >> 33) & 0x3 or (slot >> 35) & 1:     # Whether hint sptk; no deallocation hint
        return None
    displacement = ((slot >> 36) & 1) << 20 | (slot >> 13) & 0xFFFFF
    if displacement & (1 << 20):
        displacement -= 1 << 21
    return (address + (displacement << 4)) & ((1 << 64) - 1)


def installed_address_problems(state: CpuState, expected: int) -> list[str]:
    try:
        iva = state.value('IVA', 'iva')
    except StateError as error:
        return [str(error)]
    return [] if iva == expected else ['IVA %#018x, expected the relocated runtime IVT %#018x' % (iva, expected)]


def live_image_problems(live: bytes, image: bytes) -> list[str]:
    """The live table is byte-identical to the linked one."""
    if len(live) != len(image):
        return ['the live table has %d bytes, the linked one %d' % (len(live), len(image))]
    for offset in range(0, len(live), BUNDLE_SIZE):
        if live[offset:offset + BUNDLE_SIZE] != image[offset:offset + BUNDLE_SIZE]:
            differing = sum(1 for index in range(0, len(live), BUNDLE_SIZE)
                            if live[index:index + BUNDLE_SIZE] != image[index:index + BUNDLE_SIZE])
            return ['%d bundles differ from the linked table, first at offset %#06x: %s, linked %s' % (
                differing, offset, live[offset:offset + BUNDLE_SIZE].hex(), image[offset:offset + BUNDLE_SIZE].hex())]
    return []


def route_problems(bundle: bytes, address: int, route: Route) -> list[str]:
    template, (memory, integer, branch) = decode(bundle)
    problems = []
    if template not in TEMPLATES_MIB:
        problems.append('template %#x is not MIB' % template)
    if route.vector is None:
        if memory != _NOP:
            problems.append('slot 0 %#013x is not nop.m 0' % memory)
    elif move_to_application_register(memory, 4) != route.vector:
        problems.append('slot 0 %#013x is not mov.m ar.k4=%d' % (memory, route.vector))
    if route.capture_ip:
        if integer != _MOV_R16_IP:
            problems.append('slot 1 does not capture the vector IP in r16')
    elif route.selector is None:
        if integer != _NOP:
            problems.append('slot 1 %#013x is not nop.i 0' % integer)
    elif addl_immediate(integer, 16) != route.selector:
        problems.append('slot 1 %#013x is not mov r16=%d' % (integer, route.selector))
    target = branch_target(branch, address)
    if target is None:
        problems.append('slot 2 %#013x is not an IP-relative br.sptk.many' % branch)
    elif target != route.target:
        problems.append('br.many %#018x, expected %#018x' % (target, route.target))
    return ['%s vector %#06x: %s' % (route.name, route.offset, problem) for problem in problems]


def vector_routing_problems(live: bytes, table_address: int, *, fail_stop: bytes, handlers: tuple[int,...],
                            routes: tuple[Route,...]) -> list[str]:
    """Validate routing and fail-stop padding in the live table.

    `handlers` are vectors whose whole slot holds handler code (only its first
    bundle is constrained: it must not be the fail-stop bundle); each route's
    first bundle must be its trampoline; every other bundle, including the
    padding after each trampoline, must be the fail-stop bundle."""
    if len(live) != TABLE_SIZE:
        return ['the live table has %d bytes, expected %d' % (len(live), TABLE_SIZE)]
    problems = []
    handler_bundles = {offset: range(offset, offset + slot_size(offset), BUNDLE_SIZE) for offset in handlers}
    by_offset = {route.offset: route for route in routes}
    unexpected = []
    for offset in range(0, TABLE_SIZE, BUNDLE_SIZE):
        bundle = live[offset:offset + BUNDLE_SIZE]
        if offset in by_offset:
            problems += route_problems(bundle, table_address + offset, by_offset[offset])
        elif offset in handler_bundles:
            if bundle == fail_stop:
                problems.append('handler vector %#06x starts with the fail-stop bundle' % offset)
        elif any(offset in bundles for bundles in handler_bundles.values()):
            continue
        elif bundle != fail_stop:
            unexpected.append(offset)
    if unexpected:
        problems.append('%d bundles outside the implemented vectors are not the fail-stop bundle %s, first at '
                        'offset %#06x: %s' % (len(unexpected), fail_stop.hex(), unexpected[0],
                                             live[unexpected[0]:unexpected[0] + BUNDLE_SIZE].hex()))
    return problems
