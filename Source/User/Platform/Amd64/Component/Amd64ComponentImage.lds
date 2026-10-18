/* SPDX-License-Identifier: Apache-2.0 */

/*A component's text, read-only data and zero-initialized data (.bss, no
   file bytes): the root service copies the first two into fresh Frames and
   maps the third from zeroed Frames.
   It has no initialized writable data and no constructors to run: its state
   lives in.bss, on its stack and in the start block the root service maps. */
ENTRY(AsteriaAmd64ComponentImageEntry)
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
    .rodata : { *(.rodata*) *(.data.rel.ro*) } :ReadOnlyData
    .data : { *(.data*) } :ReadOnlyData
    ASSERT(SIZEOF(.data) == 0, "the component image has no initialized writable data")
    .init_array : { *(.init_array*) *(.ctors*) *(.preinit_array*) } :ReadOnlyData
    ASSERT(SIZEOF(.init_array) == 0, "the component image has no constructors to run")
    . = ALIGN(0x1000);
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } :Data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
