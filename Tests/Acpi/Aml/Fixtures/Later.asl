// SPDX-License-Identifier: Apache-2.0
/* Project-authored ASL, second table in the forward-reference test. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "AMLFWD02", 1)
{
    Device (LATE)
    {
        Name (_HID, "AST00002")
        Method (MTH0, 1) { Return (Add (Arg0, One)) }
    }
    Name (DUPE, 99)
}
