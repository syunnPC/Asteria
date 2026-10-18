#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the foreign executables of the loader demonstration.

The root service is given executables its native machine cannot run.
They are data to it and are never executed, so each is the smallest image
of its kind that the loader's probes identify and an interpreter can parse:

- ia64-sample: an IA-64 ELF64 executable of the LP64 ABI with one read and
  execute PT_LOAD in IA-64 component space whose file bytes hold a bundle
  and the {ip, gp} entry descriptor the ELF entry names;
- amd64-sample: an AMD64 ELF64 executable with one PT_LOAD and a code entry;
- aarch64-sample: an AArch64 ELF64 executable with one PT_LOAD;
- pe-sample: a PE32+ executable for the native ISA (console subsystem, no sections).
"""
import argparse
import pathlib
import struct

# IA-64 component space (Source/Abi/ComponentBootstrap.hpp): the segment
# lies in [Ia64ComponentUserStart, Ia64ComponentUserLimit).
IA64_TEXT = 0x2000_0000_0100_0000
IA64_BUNDLE_OFFSET = 0x80
IA64_DESCRIPTOR_OFFSET = 0x90
IA64_GP = IA64_TEXT + 0x1000
AARCH64_TEXT = 0x0040_0000
AMD64_TEXT = 0x0040_0000


def elf64(machine: int, flags: int, address: int, entry: int, body: bytes) -> bytes:
    """An ELF64 little-endian ET_EXEC whose single R+X PT_LOAD maps the whole
    file (headers included) at address."""
    header_bytes, program_header_bytes = 64, 56
    size = len(body)
    identification = b'\x7fELF' + bytes([2, 1, 1, 0]) + bytes(8)
    header = identification + struct.pack('<HHIQQQIHHHHHH', 2, machine, 1, entry, header_bytes, 0, flags,
                                           header_bytes, program_header_bytes, 1, 0, 0, 0)
    load = struct.pack('<IIQQQQQQ', 1, 5, 0, address, address, size, size, 0x1000)
    image = bytearray(body)
    image[0:header_bytes + program_header_bytes] = header + load
    return bytes(image)


def ia64_sample() -> bytes:
    body = bytearray(0xA0)
    # A bundle of three no-ops (MII template 0): never executed natively.
    body[IA64_BUNDLE_OFFSET:IA64_BUNDLE_OFFSET + 16] = bytes.fromhex('00000000010000000002000000000400')
    body[IA64_DESCRIPTOR_OFFSET:IA64_DESCRIPTOR_OFFSET + 16] = struct.pack('<QQ', IA64_TEXT + IA64_BUNDLE_OFFSET,
                                                                           IA64_GP)
    return elf64(50, 0x10, IA64_TEXT, IA64_TEXT + IA64_DESCRIPTOR_OFFSET, bytes(body))


def aarch64_sample() -> bytes:
    body = bytearray(0x80)
    body[0x78:0x7C] = struct.pack('<I', 0xD65F03C0)  # Ret
    return elf64(183, 0, AARCH64_TEXT, AARCH64_TEXT + 0x78, bytes(body))


def amd64_sample() -> bytes:
    body = bytearray(0x80)
    body[0x78] = 0xC3  # Ret; data to the interpreter, never executed
    return elf64(62, 0, AMD64_TEXT, AMD64_TEXT + 0x78, bytes(body))


def pe_sample(machine: int = 0x8664) -> bytes:
    """MZ stub with e_lfanew 0x80, the PE signature, a COFF header for the selected native ISA
    with no sections and a 240-byte PE32+ optional header."""
    image = bytearray(0x80)
    image[0:2] = b'MZ'
    image[0x3C:0x40] = struct.pack('<I', 0x80)
    coff = struct.pack('<HHIIIHH', machine, 0, 0, 0, 0, 0xF0, 0x22)
    optional = bytearray(0xF0)
    optional[0:2] = struct.pack('<H', 0x20B)
    optional[16:20] = struct.pack('<I', 0x1000)            # AddressOfEntryPoint
    optional[24:32] = struct.pack('<Q', 0x1_4000_0000)     # ImageBase
    optional[32:40] = struct.pack('<II', 0x1000, 0x200)    # SectionAlignment, FileAlignment
    optional[56:64] = struct.pack('<II', 0x1000, 0x200)    # SizeOfImage, SizeOfHeaders
    optional[68:70] = struct.pack('<H', 3)                 # Subsystem: console
    return bytes(image) + b'PE\0\0' + coff + bytes(optional)


SAMPLES = {'amd64-sample': amd64_sample, 'ia64-sample': ia64_sample, 'aarch64-sample': aarch64_sample, 'pe-sample': pe_sample}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True, help='directory of <name>.bin files')
    parser.add_argument('--native-machine', choices=('Amd64', 'Ia64'), default='Amd64')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, make in SAMPLES.items():
        (args.output / ('%s.bin' % name)).write_bytes(
            pe_sample(0x200) if name == 'pe-sample' and args.native_machine == 'Ia64' else make())
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
