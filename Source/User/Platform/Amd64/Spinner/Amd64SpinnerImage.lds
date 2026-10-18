/* SPDX-License-Identifier: Apache-2.0 */

/*The spinner keeps its state in registers and on its stack: the image has
   no writable segment, since an empty one would be a PT_LOAD whose offset
   and address disagree modulo its alignment, which the kernel rejects. */
ENTRY(AsteriaAmd64SpinnerImageEntry)
PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
}
SECTIONS
{
    . = 0x40000000;
    .text : { *(.text*) } :Text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata*) } :ReadOnlyData
    .data : { *(.data*) *(.bss*) *(COMMON) } :ReadOnlyData
    ASSERT(SIZEOF(.data) == 0, "the spinner image has no writable data")
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
