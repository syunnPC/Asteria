// SPDX-License-Identifier: Apache-2.0
/* Asteria-authored ACPI 6.5 declaration matrix; no hardware operations run. */
DefinitionBlock ("", "DSDT", 2, "ASTERI", "PS2FACTS", 1)
{
    OperationRegion (REGN, SystemMemory, 0x1000, 1)
    Field (REGN, ByteAcc, NoLock, Preserve) { FLD0, 8 }
    Scope (\_SB)
    {
        Device (I300) { Name (_HID, EisaId ("PNP0300"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I301) { Name (_HID, EisaId ("PNP0301"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I302) { Name (_HID, EisaId ("PNP0302"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I303) { Name (_HID, EisaId ("PNP0303"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I304) { Name (_HID, EisaId ("PNP0304"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I305) { Name (_HID, EisaId ("PNP0305"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I306) { Name (_HID, EisaId ("PNP0306"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I307) { Name (_HID, EisaId ("PNP0307"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I308) { Name (_HID, EisaId ("PNP0308"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I309) { Name (_HID, EisaId ("PNP0309"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I30A) { Name (_HID, EisaId ("PNP030A"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I30B) { Name (_HID, EisaId ("PNP030B"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (I320) { Name (_HID, EisaId ("PNP0320"))  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (CID0) { Name (_HID, "ATK3001") Name (_CID, Package () { "OTHER", EisaId ("PNP030B") })  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (BAD0) { Name (_HID, "PNP030C")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (OFF0) { Name (_HID, "PNP0303") Name (_STA, Zero) Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (DIS0) { Name (_HID, "PNP0303") Name (_STA, One) Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (UNKN) { Name (_HID, "PNP0303") Method (_STA, 0) { Return (\FLD0) } Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (MISS) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) IRQNoFlags () { 1 } }) }
        Device (LEN0) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 2) IRQNoFlags () { 1 } }) }
        Device (RANG) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x61, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (PORT) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) FixedIO (0x80, 1) IRQNoFlags () { 1 } }) }
        Device (MEM0) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) Memory32Fixed (ReadWrite, 0x1000, 0x1000) IRQNoFlags () { 1 } }) }
        Device (DMA0) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) DMA (Compatibility, NotBusMaster, Transfer8) { 2 } IRQNoFlags () { 1 } }) }
        Device (IRQ0) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 2 } }) }
        Device (IRQF) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQ (Level, ActiveLow, Shared) { 1, 3 } }) }
        Device (EXT0) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) Interrupt (ResourceConsumer, Level, ActiveLow, Shared) { 99, 1 } }) }
        Device (EXT1) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 99, 2 } }) }
        Device (VEND) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } IRQNoFlags () { 4 } VendorShort () { 1 } VendorLong () { 2, 3 } }) }
        Device (FIO0) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) }
        Device (EDLO) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) IRQ (Edge, ActiveLow, Exclusive) { 1 } }) }
        Device (EXHI) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 1 } }) }
        Device (FRST) { Name (_HID, "PNP0303")  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) IRQ (Level, ActiveHigh, Exclusive) { 1 } IRQ (Edge, ActiveLow, Exclusive) { 1 } }) }
        Device (MOU0) { Name (_HID, "PNP0F03")  Name (_CRS, ResourceTemplate () { IRQNoFlags () { 12 } }) }
        Device (MOU1) { Name (_HID, "IBM3780") Name (_CID, "PNP0F13")  Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) { 10, 12 } }) }
        Device (MOU2) { Name (_HID, "PNP0F13")  Name (_CRS, ResourceTemplate () { IRQNoFlags () { 1 } }) }
        Device (MOU3) { Name (_HID, "PNP0F13")  Name (_CRS, ResourceTemplate () { FixedIO (0x80, 1) IRQNoFlags () { 12 } }) }
        Device (MOU4) { Name (_HID, "PNP0F14")  Name (_CRS, ResourceTemplate () { IRQNoFlags () { 12 } }) }
        Device (AOFF) { Name (_ADR, Zero) Method (_STA, 0) { Return (Zero) } Device (KEY0) { Name (_HID, "PNP0303") Name (_STA, 0x0F) Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) } }
        Device (ADIS) { Name (_ADR, Zero) Method (_STA, 0) { Return (One) } Device (KEY0) { Name (_HID, "PNP0303") Name (_STA, 0x0F) Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) } }
        Device (AUNK) { Name (_ADR, Zero) Method (_STA, 0) { Return (\FLD0) } Device (KEY0) { Name (_HID, "PNP0303") Name (_STA, 0x0F) Name (_CRS, ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) FixedIO (0x64, 1) IRQNoFlags () { 1 } }) } }
        Device (CRSU) { Name (_HID, "PNP0303") Method (_CRS, 0) { Return (\FLD0) } }
        Device (CRSM) { Name (_HID, "PNP0303") }
        Device (CRST) { Name (_HID, "PNP0303") Name (WRNG, One) Method (_CRS, 0) { Return (WRNG) } }
        Device (BOTH) {
            Name (_HID, "PNP0F03") Name (_CID, "PNP0303")
            Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) IRQNoFlags () { 1, 12 } })
        }
    }
}
