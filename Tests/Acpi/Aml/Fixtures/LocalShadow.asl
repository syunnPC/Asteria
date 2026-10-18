// SPDX-License-Identifier: Apache-2.0
// Authored from ACPI 6.5: method Names and buffer fields shadow outer methods.
DefinitionBlock ("", "DSDT", 2, "ASTERI", "SHADOW01", 1)
{
    Method (DATA, 1) { Return (Arg0) }
    Method (BITS, 1) { Return (Arg0) }
    Method (PACK, 0)
    {
        Name (DATA, Package (3) { 2, 4, 6 })
        Return (DATA)
    }
    Method (BYTE, 0)
    {
        Name (TEMP, Buffer (2) { 0x25, 0x72 })
        CreateByteField (TEMP, One, BITS)
        Return (BITS)
    }
}
