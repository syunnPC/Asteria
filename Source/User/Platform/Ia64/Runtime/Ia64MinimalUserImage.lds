/* SPDX-License-Identifier: Apache-2.0 */

ENTRY(AsteriaIa64MinimalUserImageEntryDescriptor)

PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}

SECTIONS
{
    AsteriaIa64MinimalUserImageBase = 0x2000000004000000;
    . = AsteriaIa64MinimalUserImageBase;

    .text : ALIGN(0x1000)
    {
        *(.text*)
    } :Text

    . = AsteriaIa64MinimalUserImageBase + 0x10000;
    .rodata :
    {
        *(.rodata*)
    } :ReadOnlyData

    . = AsteriaIa64MinimalUserImageBase + 0x11000;
    .AsteriaUserEntryDescriptor :
    {
        KEEP(*(.AsteriaUserEntryDescriptor))
    } :ReadOnlyData

    . = AsteriaIa64MinimalUserImageBase + 0x12000;
    .opd :
    {
        *(.opd)
    } :ReadOnlyData

    . = AsteriaIa64MinimalUserImageBase + 0x20000;
    .data :
    {
        *(.data*)
    } :Data

    . = AsteriaIa64MinimalUserImageBase + 0x21000;
    .got :
    {
        *(.got*)
    } :Data

    . = AsteriaIa64MinimalUserImageBase + 0x22000;
    .bss :
    {
        *(.bss*)
        *(COMMON)
    } :Data

    __gp = AsteriaIa64MinimalUserImageBase + 0x20800;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}

ASSERT(
    SIZEOF(.AsteriaUserEntryDescriptor) == 16,
    "IA-64 user entry descriptor must be exactly two words")
ASSERT(
    (ADDR(.AsteriaUserEntryDescriptor) & 15) == 0,
    "IA-64 user entry descriptor must be 16-byte aligned")
