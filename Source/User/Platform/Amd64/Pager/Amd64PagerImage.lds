/* SPDX-License-Identifier: Apache-2.0 */

ENTRY(AsteriaAmd64PagerImageEntry)
PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x40000000;
    .text : { *(.text*) } :Text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) } :ReadOnlyData
    . = ALIGN(0x1000);
    .data : { *(.data*) } :Data
    .bss : { *(.bss*) *(COMMON) } :Data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
