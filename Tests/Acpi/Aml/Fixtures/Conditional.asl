// SPDX-License-Identifier: Apache-2.0
/* Project-authored module-level branch and store execution. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "AMLIF001", 1)
{
    Name (CHOS, Zero)
    If (_OSI ("Windows 2022"))
    {
        Store (One, CHOS)
        Device (GOOD) { Name (_HID, "PNP0303") }
    }
    Else
    {
        Device (BAD0) { Name (_HID, "PNP0F13") }
    }
    If (Zero)
    {
        Device (SKIP) { Name (_HID, "PNP030B") }
    }
    Name (STEP, Zero)
    While (STEP < 3) { Increment (STEP) }
}
