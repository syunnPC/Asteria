/* SPDX-License-Identifier: Apache-2.0 */

ENTRY(AsteriaIa64PagerImageEntryDescriptor)

PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}

SECTIONS
{
    AsteriaIa64PagerImageBase = 0x2000000004000000;
    . = AsteriaIa64PagerImageBase;

    /*Pack the separate pager image without fixed holes: both optimization
       levels must fit without growing kernel
       read-only bytes with unused reservations. */
    .text : ALIGN(0x1000)
    {
        *(.text*)
    } :Text

    . = ALIGN(0x1000);
    .rodata :
    {
        *(.rodata*)
    } :ReadOnlyData

    . = ALIGN(16);
    .AsteriaUserEntryDescriptor :
    {
        KEEP(*(.AsteriaUserEntryDescriptor))
    } :ReadOnlyData

    . = ALIGN(16);
    .opd :
    {
        *(.opd)
    } :ReadOnlyData

    /*The component image the root service builds (AsteriaTarget.cmake),
       read-only, up to the data segment. */
    . = ALIGN(0x1000);

    . = ALIGN(0x1000);
    .data :
    {
        *(.data*)
    } :Data

    . = ALIGN(16);
    .got :
    {
        *(.got*)
    } :Data

    . = ALIGN(0x1000);
    .bss :
    {
        *(.bss*)
        *(COMMON)
    } :Data

    __gp = ADDR(.got);

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

ASSERT(ADDR(.bss) + SIZEOF(.bss) <= 0x2000000004800000,
    "IA-64 pager image must end before the lazy, window, stack and module ranges")
