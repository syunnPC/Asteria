// SPDX-License-Identifier: Apache-2.0
// External metadata after a definition must leave its method body intact.
DefinitionBlock ("", "SSDT", 2, "ASTERI", "EXAFTER1", 1)
{
    External (\OUTP, MethodObj)
    Method (AGAN, 0) { Return (\OUTP ()) }
}
