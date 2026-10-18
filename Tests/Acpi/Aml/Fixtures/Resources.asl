// SPDX-License-Identifier: Apache-2.0
/* Project-authored resource templates, generated solely from ACPI macros. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "AMLRES01", 1)
{
    Name (RES0, ResourceTemplate () {
        IRQ (Level, ActiveLow, Shared) { 3, 5 }
        DMA (Compatibility, NotBusMaster, Transfer8) { 2 }
        Interrupt (ResourceConsumer, Level, ActiveLow, Shared, 0, "\\_SB.PCI0") { 20, 21 }
        Memory24 (ReadWrite, 0x1000, 0x2000, 0x100, 0x100)
        Memory32 (ReadWrite, 0x100000, 0x200000, 0x1000, 0x100000)
        Memory32Fixed (ReadWrite, 0x200000, 0x1000)
        WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange, 0, 0x1000, 0x1FFF, 0, 0x1000)
        DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                     0, 0x100000, 0x1FFFFF, 0, 0x100000)
        QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                     0, 0x100000000, 0x10000FFFF, 0, 0x10000)
        ExtendedMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                        0, 0x200000000, 0x20000FFFF, 0, 0x10000, 0)
        VendorShort () { 1, 2, 3 }
        VendorLong () { 4, 5, 6, 7, 8, 9, 10, 11 }
    })
}
