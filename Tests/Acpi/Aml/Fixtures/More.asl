// SPDX-License-Identifier: Apache-2.0
/* Project-authored operator and grammar cases, with optimization disabled. */
DefinitionBlock ("", "SSDT", 2, "ASTERI", "AMLMORE1", 1)
{
    Name (LAZY, Package () { One, 2, 3 })
    Name (LINT, 19)
    Name (COPY, Zero)
    Name (GBUF, Buffer () {0x12, 0x34})
    CreateWordField (GBUF, Zero, GWRD)
    Method (SIZE, 0) { Return (SizeOf (LAZY)) }
    Method (TYPE, 0) { Return (ObjectType (LAZY)) }
    Method (DREF, 0) { Store ("LINT", Local0) Return (DerefOf (Local0)) }
    Method (SETS, 0) { Store ("0x1234", LINT) Return (LINT) }
    Method (CPYO, 0) { CopyObject ("changed", COPY) Return (COPY) }
    Method (ARGW, 1) { Store (Add (Arg0, One), Arg0) Return (Arg0) }
    Method (MISS, 0) { Return (CondRefOf (\NONE)) }
    Method (SHFL, 0) { Return (ShiftLeft (3, 5)) }
    Method (SHFR, 0) { Return (ShiftRight (0x80, 4)) }
    Method (SH64, 0) { Return (ShiftLeft (One, 64)) }
    Method (SUBT, 0) { Return (Subtract (3, 5)) }
    Method (BAND, 0) { Return (And (0xF0, 0x55)) }
    Method (BNAN, 0) { Return (NAnd (0xF0, 0x55)) }
    Method (BORS, 0) { Return (Or (0xF0, 0x55)) }
    Method (BNOR, 0) { Return (NOr (0xF0, 0x55)) }
    Method (BNOT, 0) { Return (Not (0x55)) }
    Method (FSLB, 0) { Return (FindSetLeftBit (0x10)) }
    Method (FSRB, 0) { Return (FindSetRightBit (0x10)) }
    Method (MODU, 0) { Return (Mod (20, 6)) }
    Method (DIVD, 0)
    {
        Divide (20, 6, Local0, Local1)
        Return (Add (Local0, Local1))
    }
    Method (DECR, 0) { Store (5, Local0) Decrement (Local0) Return (Local0) }
    Method (CMPS, 0) { Return (LGreater ("xyz", "abc")) }
    Method (CMPB, 0) { Return (LLess (Buffer () {1, 2}, Buffer () {1, 3})) }
    Method (NEQU, 0) { Return (LNotEqual (5, 4)) }
    Method (LGEQ, 0) { Return (LGreaterEqual (5, 5)) }
    Method (LLEQ, 0) { Return (LLessEqual (5, 5)) }
    Method (LOGA, 0) { Return (LAnd (7, 9)) }
    Method (HXBF, 0) { Return (ToHexString (Buffer () {0, 0xAB, 0xFF})) }
    Method (DCBF, 0) { Return (ToDecimalString (Buffer () {0, 0xAB, 0xFF})) }
    Method (BCAT, 0) { Return (Concatenate (Buffer () {1, 2}, Buffer () {3, 4})) }
    Method (BMID, 0) { Return (Mid (Buffer () {1, 2, 3, 4}, 1, 2)) }
    Method (VPCK, 1) { Return (Package (Arg0) { 1, 2 }) }
    Method (FAMS, 0)
    {
        Store (Buffer () {0, 0, 0, 0, 0, 0, 0, 0}, Local0)
        CreateBitField (Local0, 0, BIT0)
        CreateWordField (Local0, 0, WRD0)
        CreateDWordField (Local0, 0, DWD0)
        CreateQWordField (Local0, 0, QWD0)
        Store (0x123456789ABCDEF0, QWD0)
        Store (1, BIT0)
        Return (Add (WRD0, DWD0))
    }
    Method (WNAM, 0)
    {
        Name (TEMP, Buffer () {1, 2, 3})
        Store (7, Index (TEMP, One))
        Return (TEMP)
    }
    Method (WIDE, 0)
    {
        Name (TEMP, Buffer (16) {})
        CreateField (TEMP, 3, 72, FLDS)
        Store (Buffer () {1, 2, 3, 4, 5, 6, 7, 8, 9}, FLDS)
        Return (FLDS)
    }
    Method (FAIL, 0) { Store (42, LINT) Store (1, FLD0) }
    Method (SYNL, 0, Serialized, 8) { Return (One) }
    Mutex (MTX0, 0)
    Event (EVN0)
    OperationRegion (REG0, GenericSerialBus, 0, 4)
    Name (CON0, Buffer () {1, 2, 3})
    Field (REG0, BufferAcc, NoLock, Preserve)
    {
        AccessAs (BufferAcc, AttribBytes (4)),
        Connection (CON0),
        FLD0, 8,
        FLD1, 8
    }
    Method (BCD0, 0) { Return (FromBCD (0x1234)) }
    Method (BCD1, 0) { Return (ToBCD (1234)) }
    Method (CRES, 0) { Return (ConcatenateResTemplate (Buffer () {0x79, 0}, Buffer () {0x79, 0})) }
    Method (LODA, 0) { Load (REG0, Local0) Return (Local0) }
    Method (LDTB, 0) { Return (LoadTable ("SSDT", "ASTERI", "TABLE001", "\\", "", Zero)) }
    Method (DBUG, 0) { Store (1, Debug) }
}
