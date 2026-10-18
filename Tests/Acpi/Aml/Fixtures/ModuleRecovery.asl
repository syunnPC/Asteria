// SPDX-License-Identifier: Apache-2.0
// Independent declarations survive a hardware-dependent module statement.
DefinitionBlock ("", "SSDT", 2, "ASTERI", "RECOVER1", 1)
{
    Name (KEEP, 7)
    OperationRegion (PORT, SystemIo, 0x80, 1)
    Field (PORT, ByteAcc, NoLock, Preserve) { FLAG, 8 }
    Method (FAIL, 0)
    {
        Store (11, KEEP)
        Return (FLAG)
    }
    If (FLAG)
    {
        Device (MAYB) { Name (_HID, "PNP0303") }
    }
    Else
    {
        Device (OTHR) { Name (_HID, "PNP0F13") }
    }
    If (One)
    {
        Store (9, KEEP)
        Name (TEMP, One)
        FAIL ()
    }
    Name (LATE, 5)
}
