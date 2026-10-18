/* SPDX-License-Identifier: Apache-2.0 */

ENTRY(AsteriaEfiEntryDescriptor)

PHDRS
{
    Text PT_LOAD FLAGS(5);
    ReadOnlyData PT_LOAD FLAGS(4);
    Data PT_LOAD FLAGS(6);
}

SECTIONS
{
    AsteriaImageBase = DEFINED(AsteriaConfiguredImageBase)
        ? AsteriaConfiguredImageBase
        : 0x04000000;
    . = AsteriaImageBase + 0x1000;

    /* Group RVAs are 4 MiB aligned even at a different preferred PE base. */
    . = AsteriaImageBase + 0x400000;
    .text :
    {
        *(.text*)
        . = ABSOLUTE(AsteriaImageBase + ALIGN(ABSOLUTE(.) - AsteriaImageBase, 0x400000));
    } :Text

    . = ALIGN(0x1000);
    .rodata (READONLY) :
    {
        PROVIDE_HIDDEN(AsteriaIa64LoadedReadOnlyDataStart = .);
        *(.rodata*)
        *(.data.rel.ro*)
        . = ALIGN(8);
        *(.got*)
        . = ALIGN(8);
        PROVIDE_HIDDEN(AsteriaIa64LoadedReadOnlyDataEnd = .);
    } :ReadOnlyData

    /* All GP-relative loads use the read-only GOT, beside.rodata. */
    PROVIDE(__gp = ABSOLUTE(ADDR(.rodata) + 0x80000));

    . = ALIGN(0x1000);
    .opd :
    {
        AsteriaEfiEntryDescriptor = .;
        KEEP(*(.asteria_efi_entry))
        *(.opd)
        /* The relocation page closes the complete read-only 4 MiB group. */
        . = ABSOLUTE(AsteriaImageBase + ALIGN(ABSOLUTE(.) - AsteriaImageBase + 0x1000, 0x400000) - 0x1000);
    } :ReadOnlyData

    . = ALIGN(0x1000);
    .reloc (READONLY) :
    {
        FILL(0)
        BYTE(0)
        . = . + 0xFFF;
    } :ReadOnlyData

    . = ALIGN(0x1000);
    .data :
    {
        *(.data*)
    } :Data

    . = ALIGN(0x1000);
    .bss :
    {
        *(.bss*)
        *(COMMON)
        . = ABSOLUTE(AsteriaImageBase + ALIGN(ABSOLUTE(.) - AsteriaImageBase, 0x400000));
    } :Data

    AsteriaIa64LoadedImageEnd = .;

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.IA_64.unwind*)
    }
}
