// SPDX-License-Identifier: Apache-2.0
/* Asteria-authored declaration, for a VM with no port decoder. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "PS2FLOAT", 1)
{
    Scope (\_SB)
    {
        Device (KEY0)
        {
            Name (_HID, EisaId ("PNP0303"))
            Name (_CRS, ResourceTemplate () {
                FixedIO (0x60, 1)
                FixedIO (0x64, 1)
                IRQNoFlags () { 1 }
            })
        }
    }
}
