/* SPDX-License-Identifier: Apache-2.0 */

/* Kernel ownership and entry ABI: Documentation/Architecture/KernelElf.md. */
ENTRY(AsteriaKernelEnter)
PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Bss PT_LOAD FLAGS(6);
    Metadata PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}
SECTIONS
{
    AsteriaImageBase = 0x4000000;
    AsteriaConfiguredImageBase = AsteriaImageBase;
    . = AsteriaImageBase + 0x1000;
    .text :
    {
        AsteriaKernelTextStart = .;
        *(.text*)
        AsteriaKernelTextEnd = .;
        . = ALIGN(0x1000);
    }
    :Text
    .note.asteria :
    {
        KEEP(*(.asteria_kernel_note))
        LONG(@ASTERIA_KERNEL_ENTRY_ABI_VERSION@)
        LONG(@ASTERIA_KERNEL_BOOT_INFO_VERSION@)
        LONG(1)
        LONG(7)
        QUAD(AsteriaImageBase)
        QUAD(AsteriaImageEnd - AsteriaImageBase)
        QUAD(2097152)
        QUAD(AsteriaKernelPrepare - AsteriaImageBase)
        QUAD(AsteriaKernelEnter - AsteriaImageBase)
        QUAD(AsteriaKernelQuery - AsteriaImageBase)
        QUAD(0)
        QUAD(0)
        QUAD(0)
        QUAD(0)
        QUAD(ADDR(.text) - AsteriaImageBase)
        QUAD(SIZEOF(.text) / 0x1000)
        LONG(0)
        LONG(0)
        QUAD(ADDR(.note.asteria) - AsteriaImageBase)
        QUAD(SIZEOF(.note.asteria) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.rodata) - AsteriaImageBase)
        QUAD(SIZEOF(.rodata) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.bss) - AsteriaImageBase)
        QUAD(SIZEOF(.bss) / 0x1000)
        LONG(2)
        LONG(0)
        QUAD(ADDR(.got) - AsteriaImageBase)
        QUAD(SIZEOF(.got) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.opd) - AsteriaImageBase)
        QUAD(SIZEOF(.opd) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.data) - AsteriaImageBase)
        QUAD(SIZEOF(.data) / 0x1000)
        LONG(2)
        LONG(0)
        QUAD(@ASTERIA_KERNEL_BOOTSTRAP_PAGES@)
        QUAD(1)
        QUAD(0xFFFFFFFFFFFFFFFF)
        LONG(0)
        LONG(0)
        QUAD(@ASTERIA_KERNEL_RUNTIME_PAGES@)
        QUAD(1)
        QUAD(0xFFFFFFFF)
        LONG(1)
        LONG(0)
        QUAD(2)
        QUAD(1)
        QUAD(0xFFFFF)
        LONG(2)
        LONG(0)
        . = ALIGN(0x1000);
    }
    :ReadOnlyData
    .rodata (READONLY) :
    {
        *(.rodata*)
        *(.data.rel.ro*)
        . = ALIGN(0x1000);
    }
    :ReadOnlyData
    .bss (NOLOAD) :
    {
        *(.bss*)
        *(COMMON)
        . = ALIGN(0x1000);
    }
    :Bss
    .got (READONLY) :
    {
        QUAD(0)
        *(.got*)
        . = ALIGN(0x1000);
    }
    :Metadata
    .opd (READONLY) :
    {
        QUAD(0)
        . = ALIGN(0x1000);
    }
    :Metadata
    .data :
    {
        *(.data*)
        . = ALIGN(0x1000);
    }
    :Data
    AsteriaImageEnd = .;
    /DISCARD/ :
    {
        *(.comment .note.* .eh_frame* .gnu.lto_* .gnu_object_only .IA_64.unwind* .asteria_text_padding .asteria_ro_padding .asteria_ro_tail)
    }
}
