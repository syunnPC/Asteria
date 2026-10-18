# SPDX-License-Identifier: Apache-2.0
"""ELF relocation type names for IA-64 and x86-64.

Names omit the `R_IA64_` / `R_X86_64_` prefix, matching how the checks and the
golden files refer to them.
"""
from __future__ import annotations

IA64 = {
    0x00: 'NONE', 0x21: 'IMM14', 0x22: 'IMM22', 0x23: 'IMM64', 0x24: 'DIR32MSB',
    0x25: 'DIR32LSB', 0x26: 'DIR64MSB', 0x27: 'DIR64LSB', 0x2a: 'GPREL22',
    0x2b: 'GPREL64I', 0x2c: 'GPREL32MSB', 0x2d: 'GPREL32LSB', 0x2e: 'GPREL64MSB',
    0x2f: 'GPREL64LSB', 0x32: 'LTOFF22', 0x33: 'LTOFF64I', 0x3a: 'PLTOFF22',
    0x3b: 'PLTOFF64I', 0x3e: 'PLTOFF64MSB', 0x3f: 'PLTOFF64LSB', 0x43: 'FPTR64I',
    0x44: 'FPTR32MSB', 0x45: 'FPTR32LSB', 0x46: 'FPTR64MSB', 0x47: 'FPTR64LSB',
    0x48: 'PCREL60B', 0x49: 'PCREL21B', 0x4a: 'PCREL21M', 0x4b: 'PCREL21F',
    0x4c: 'PCREL32MSB', 0x4d: 'PCREL32LSB', 0x4e: 'PCREL64MSB', 0x4f: 'PCREL64LSB',
    0x52: 'LTOFF_FPTR22', 0x53: 'LTOFF_FPTR64I', 0x54: 'LTOFF_FPTR32MSB',
    0x55: 'LTOFF_FPTR32LSB', 0x56: 'LTOFF_FPTR64MSB', 0x57: 'LTOFF_FPTR64LSB',
    0x5c: 'SEGREL32MSB', 0x5d: 'SEGREL32LSB', 0x5e: 'SEGREL64MSB', 0x5f: 'SEGREL64LSB',
    0x64: 'SECREL32MSB', 0x65: 'SECREL32LSB', 0x66: 'SECREL64MSB', 0x67: 'SECREL64LSB',
    0x6c: 'REL32MSB', 0x6d: 'REL32LSB', 0x6e: 'REL64MSB', 0x6f: 'REL64LSB',
    0x74: 'LTV32MSB', 0x75: 'LTV32LSB', 0x76: 'LTV64MSB', 0x77: 'LTV64LSB',
    0x79: 'PCREL21BI', 0x7a: 'PCREL22', 0x7b: 'PCREL64I', 0x80: 'IPLTMSB',
    0x81: 'IPLTLSB', 0x84: 'COPY', 0x85: 'SUB', 0x86: 'LTOFF22X', 0x87: 'LDXMOV',
    0x91: 'TPREL14', 0x92: 'TPREL22', 0x93: 'TPREL64I', 0x96: 'TPREL64MSB',
    0x97: 'TPREL64LSB', 0x9a: 'LTOFF_TPREL22', 0xa6: 'DTPMOD64MSB', 0xa7: 'DTPMOD64LSB',
    0xaa: 'LTOFF_DTPMOD22', 0xb1: 'DTPREL14', 0xb2: 'DTPREL22', 0xb3: 'DTPREL64I',
    0xb4: 'DTPREL32MSB', 0xb5: 'DTPREL32LSB', 0xb6: 'DTPREL64MSB', 0xb7: 'DTPREL64LSB',
    0xba: 'LTOFF_DTPREL22',
}

X86_64 = {
    0: 'NONE', 1: '64', 2: 'PC32', 3: 'GOT32', 4: 'PLT32', 5: 'COPY', 6: 'GLOB_DAT',
    7: 'JUMP_SLOT', 8: 'RELATIVE', 9: 'GOTPCREL', 10: '32', 11: '32S', 12: '16',
    13: 'PC16', 14: '8', 15: 'PC8', 24: 'PC64', 25: 'GOTOFF64', 26: 'GOTPC32',
    41: 'GOTPCRELX', 42: 'REX_GOTPCRELX',
}

# Relocation kinds that patch an instruction's branch target rather than an
# immediate operand.
IA64_BRANCH = {'PCREL21B', 'PCREL21BI', 'PCREL21M', 'PCREL21F', 'PCREL60B'}


def name(machine: int, kind: int) -> str:
    table = IA64 if machine == 50 else X86_64
    return table.get(kind, 'TYPE_%#x' % kind)
