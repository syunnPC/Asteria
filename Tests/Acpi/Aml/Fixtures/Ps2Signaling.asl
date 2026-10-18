// SPDX-License-Identifier: Apache-2.0
/*Asteria-authored ACPI 6.5 6.4.2.1/6.4.3.6 signaling declarations; the first
   qualified keyboard and mouse carry the i8042 IRQ evidence. */
DefinitionBlock ("", "DSDT", 2, "ASTERI", "PS2SIGNL", 1)
{
    Scope (\_SB)
    {
        Device (KBD0) { Name (_HID, EisaId ("PNP0303"))  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) IRQ (Level, ActiveHigh, Exclusive) { 1 } }) }
        Device (KBD1) { Name (_HID, EisaId ("PNP0303"))  Name (_CRS, ResourceTemplate () { FixedIO (0x60, 1) FixedIO (0x64, 1) IRQ (Edge, ActiveLow, Exclusive) { 1 } }) }
        Device (MOU0) { Name (_HID, EisaId ("PNP0F13"))  Name (_CRS, ResourceTemplate () { Interrupt (ResourceConsumer, Edge, ActiveLow, Exclusive) { 12 } }) }
        Device (MOU1) { Name (_HID, EisaId ("PNP0F13"))  Name (_CRS, ResourceTemplate () { Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 12 } }) }
    }
}
