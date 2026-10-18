/* SPDX-License-Identifier: Apache-2.0 */

/*The firmware loader accepts this exact seven-section, 4 KiB W^X layout.
   Initialized writable storage follows relocations so existing section indices
   retain their meanings. No hosted CRT, TLS, import, or constructor is admitted. */
OUTPUT_FORMAT(pei-x86-64)
ENTRY(EfiMain)
SECTIONS
{
    . = __image_base__ + __section_alignment__;
    .text : { *(.text .text.* .gnu.linkonce.t.*) }
    .rdata BLOCK(__section_alignment__) : { *(.rdata .rdata$*) QUAD(0) }
    .rodata BLOCK(__section_alignment__) : { *(.rodata .rodata.* .gnu.linkonce.r.*) }
    .bss BLOCK(__section_alignment__) : { *(.bss .bss.*) *(COMMON) }
    .idata BLOCK(__section_alignment__) : { LONG(0) LONG(0) LONG(0) LONG(0) LONG(0) LONG(0) }
    .reloc BLOCK(__section_alignment__) : { *(.reloc) }
    .data BLOCK(__section_alignment__) : { *(.data .data.* .gnu.linkonce.d.*) }
    /DISCARD/ : { *(.comment .note.* .eh_frame* .gnu.lto_* .gnu_object_only) }
}
