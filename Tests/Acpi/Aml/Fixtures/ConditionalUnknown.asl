// SPDX-License-Identifier: Apache-2.0
/* Unknown module predicates must never invent an active device. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "AMLIF002", 1)
{
    OperationRegion (REG0, SystemIo, 0x80, 1)
    Field (REG0, ByteAcc, NoLock, Preserve) { FLD0, 8 }
    If (FLD0)
    {
        Device (UNKN) { Name (_HID, "PNP0303") }
    }
}
