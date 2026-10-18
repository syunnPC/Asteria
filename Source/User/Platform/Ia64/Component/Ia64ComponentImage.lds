/* SPDX-License-Identifier: Apache-2.0 */

/*A component's text, read-only data and zero-initialized data (.bss, no
   file bytes): the root service copies the first two into fresh Frames and
   maps the third from zeroed Frames.
   It has no initialized writable data: its state lives in.bss (its
   services' buffers, Source/User/System/AhciPort.hpp ), in registers, on its stack and
   in the start block the root service maps. A static image's function
   descriptors and linkage table hold link-time addresses that nothing
   writes, so they are read-only data too, and gp addresses the table there. */
ENTRY(AsteriaIa64ComponentImageEntryDescriptor)

PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}

SECTIONS
{
    AsteriaIa64ComponentImageBase = 0x2000000001000000;
    . = AsteriaIa64ComponentImageBase;

    .text : ALIGN(0x1000)
    {
        *(.text*)
    } :Text

    . = ALIGN(0x1000);
    .rodata :
    {
        *(.rodata*) *(.data.rel.ro*)
    } :ReadOnlyData

    .AsteriaUserEntryDescriptor : ALIGN(16)
    {
        KEEP(*(.AsteriaUserEntryDescriptor))
    } :ReadOnlyData

    .opd : ALIGN(16)
    {
        *(.opd)
    } :ReadOnlyData

    .got : ALIGN(16)
    {
        *(.got*)
    } :ReadOnlyData

    __gp = ADDR(.got);

    .data :
    {
        *(.data*)
        *(.sdata*)
    } :ReadOnlyData

    . = ALIGN(0x1000);
    .bss (NOLOAD) :
    {
        *(.sbss*)
        *(.bss*)
        *(COMMON)
    } :Data

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
    }
}

ASSERT(SIZEOF(.data) == 0, "the component image has no initialized writable data")
ASSERT(
    SIZEOF(.AsteriaUserEntryDescriptor) == 16,
    "IA-64 user entry descriptor must be exactly two words")
