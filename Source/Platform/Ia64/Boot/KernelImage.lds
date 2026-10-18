/* SPDX-License-Identifier: Apache-2.0 */

/* Kernel ownership and entry ABI: Documentation/Architecture/KernelElf.md. */
ENTRY(AsteriaKernelEnterDescriptor)
PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}
SECTIONS
{
    AsteriaImageBase = 0x4000000;
    AsteriaConfiguredImageBase = AsteriaImageBase;
    . = AsteriaImageBase + 0x400000;
    .text :
    {
        AsteriaKernelTextStart = .;
        *(.text*)
        AsteriaKernelTextEnd = .;
        . = ALIGN(0x1000);
    }
    :Text
    .textpad (NOLOAD) :
    {
        *(.asteria_text_padding)
        . = ABSOLUTE(AsteriaImageBase + 0x800000);
    }
    :Text
    .note.asteria :
    {
        KEEP(*(.asteria_kernel_note))
        LONG(@ASTERIA_KERNEL_ENTRY_ABI_VERSION@)
        LONG(@ASTERIA_KERNEL_BOOT_INFO_VERSION@)
        LONG(2)
        LONG(6)
        QUAD(AsteriaImageBase)
        QUAD(AsteriaImageEnd - AsteriaImageBase)
        QUAD(16777216)
        QUAD(AsteriaKernelPrepareDescriptor - AsteriaImageBase)
        QUAD(AsteriaKernelEnterDescriptor - AsteriaImageBase)
        QUAD(AsteriaKernelQueryDescriptor - AsteriaImageBase)
        QUAD(__gp - AsteriaImageBase)
        QUAD(AsteriaDescriptorStart - AsteriaImageBase)
        QUAD(AsteriaDescriptorEnd - AsteriaDescriptorStart)
        QUAD(0)
        QUAD(ADDR(.text) - AsteriaImageBase)
        QUAD(0x400)
        LONG(0)
        LONG(0)
        QUAD(ADDR(.note.asteria) - AsteriaImageBase)
        QUAD(SIZEOF(.note.asteria) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.rodata) - AsteriaImageBase)
        QUAD((AsteriaImageBase + 0xBFF000 - ADDR(.rodata)) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.ropad) - AsteriaImageBase)
        QUAD(SIZEOF(.ropad) / 0x1000)
        LONG(1)
        LONG(0)
        QUAD(ADDR(.data) - AsteriaImageBase)
        QUAD(SIZEOF(.data) / 0x1000)
        LONG(2)
        LONG(0)
        QUAD(ADDR(.bss) - AsteriaImageBase)
        QUAD(SIZEOF(.bss) / 0x1000)
        LONG(2)
        LONG(0)
        QUAD(0)
        QUAD(0)
        LONG(0)
        LONG(0)
        QUAD(@ASTERIA_KERNEL_BOOTSTRAP_PAGES@)
        QUAD(1)
        QUAD(0xFFFFFFFF)
        LONG(0)
        LONG(1)
        QUAD(@ASTERIA_KERNEL_RUNTIME_PAGES@)
        QUAD(@ASTERIA_KERNEL_RUNTIME_PAGES@)
        QUAD(0xFFFFFFFF)
        LONG(3)
        LONG(2)
        QUAD(0)
        QUAD(0)
        QUAD(0)
        LONG(0)
        LONG(0)
        . = ALIGN(0x1000);
    }
    :ReadOnlyData
    .rodata (READONLY) :
    {
        *(.rodata*)
        *(.data.rel.ro*)
        . = ALIGN(8);
        *(.got*)
        . = ALIGN(0x1000);
    }
    :ReadOnlyData
    PROVIDE(__gp = ABSOLUTE(ADDR(.rodata) + 0x80000));
    .opd (READONLY) :
    {
        AsteriaDescriptorStart = .;
        AsteriaKernelPrepareDescriptor = .;
        QUAD(AsteriaKernelPrepare)
        QUAD(__gp)
        AsteriaKernelEnterDescriptor = .;
        QUAD(AsteriaKernelEnter)
        QUAD(__gp)
        AsteriaKernelQueryDescriptor = .;
        QUAD(AsteriaKernelQuery)
        QUAD(__gp)
        *(.opd)
        AsteriaDescriptorEnd = .;
    }
    :ReadOnlyData
    .rofill (NOLOAD) :
    {
        *(.asteria_ro_padding)
        . = ABSOLUTE(AsteriaImageBase + 0xBFF000);
    }
    :ReadOnlyData
    .ropad (NOLOAD) :
    {
        *(.asteria_ro_tail)
        . = ALIGN(0x1000);
    }
    :ReadOnlyData
    .data :
    {
        *(.data*)
        . = ALIGN(0x1000);
    }
    :Data
    .bss (NOLOAD) :
    {
        *(.bss*)
        *(COMMON)
        . = ABSOLUTE(AsteriaImageBase + 0x1000000);
    }
    :Data
    AsteriaImageEnd = .;
    /DISCARD/ :
    {
        *(.comment .note.* .eh_frame* .gnu.lto_* .gnu_object_only .IA_64.unwind* .asteria_text_padding .asteria_ro_padding .asteria_ro_tail)
    }
}
