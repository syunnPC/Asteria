/* SPDX-License-Identifier: Apache-2.0 */

ENTRY(EfiMain)

PHDRS
{
    Text PT_LOAD FLAGS(5);
    Data PT_LOAD FLAGS(6);
}

SECTIONS
{
    AsteriaImageBase = DEFINED(AsteriaConfiguredImageBase)
        ? AsteriaConfiguredImageBase
        : 0x04000000;
    . = AsteriaImageBase;

    .text :
    {
        *(.text*)
    } :Text

    . = ALIGN(16);
    .rodata :
    {
        *(.rodata*)
    } :Text

    . = ALIGN(16);
    .data :
    {
        *(.data*)
    } :Data

    . = ALIGN(16);
    .bss :
    {
        *(.bss*)
        *(COMMON)
    } :Data

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
