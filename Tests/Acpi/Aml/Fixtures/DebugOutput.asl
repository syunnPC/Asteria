// SPDX-License-Identifier: Apache-2.0
// Debug writes consume data; an unavailable timer must never be fabricated.
DefinitionBlock ("", "SSDT", 2, "ASTERI", "DEBUG001", 1)
{
    Store ("module", Debug)
    Store (Timer, Debug)
    Name (DONE, 3)
    Method (OUTP, 0)
    {
        Store (Package (2) { 1, "message" }, Debug)
        Return (ObjectType (Debug))
    }
    Method (TIME, 0) { Return (Timer) }
}
