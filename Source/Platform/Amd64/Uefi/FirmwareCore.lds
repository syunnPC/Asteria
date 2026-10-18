/* SPDX-License-Identifier: Apache-2.0 */

/*Resolve COMDAT selection before the ELF-to-PE link. All symbol values remain
   section-relative; there is no preferred image base in this closed aggregate. */
SECTIONS
{
    .text 0 : { *(.text .text.* .gnu.linkonce.t.*) }
    .rodata 0 : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
    .data 0 : { *(.data .data.* .gnu.linkonce.d.*) }
    .bss 0 : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment .note.* .eh_frame* .gnu.lto_* .gnu_object_only) }
}
