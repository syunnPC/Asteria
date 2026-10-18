// SPDX-License-Identifier: Apache-2.0
/* Project-authored ASL, first table in the forward-reference test. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "AMLFWD01", 1)
{
    External (\LATE, DeviceObj)
    External (\LATE.MTH0, MethodObj, IntObj, {IntObj})
    Scope (\LATE)
    {
        Name (EARL, 42)
        Method (CALL, 0) { Return (MTH0 (EARL)) }
    }
    Name (DUPE, 11)
}
