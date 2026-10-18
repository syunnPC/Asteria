// SPDX-License-Identifier: Apache-2.0
#include "User/System/AhciPort.hpp"

#include "Support/TestContext.hpp"
#include "User/System/FakeAhciHba.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;
namespace Ahci = User::System;

// Port 2, using literal spec offsets independently of the library constants.
constexpr UInt32 Port{2U};
constexpr Size Base{0x200U >> 2U};
constexpr Size Cmd{Base + 6U};
constexpr Size Ci{Base + 14U};
constexpr Size Sctl{Base + 11U};
constexpr Size Ssts{Base + 10U};
constexpr Size Sig{Base + 9U};
constexpr Size Tfd{Base + 8U};
constexpr Size Is{Base + 4U};
constexpr Size Serr{Base + 12U};
constexpr UInt32 St{0x01U};
constexpr UInt32 Fre{0x10U};
constexpr UInt32 Fr{0x4000U};
constexpr UInt32 Cr{0x8000U};

// One active device is enough for the function-pointer clock; tests reset
// it between cases. Static DMA storage keeps every test's stack bounded.
TestSupport::FakeAhciHba g_hba;

/// The supplied monotonic clock and synchronous MMIO write adapter.
[[nodiscard]] UInt64 Clock() noexcept
{
    return g_hba.Tick();
}

/// Applies the fake device's write side effects before returning to the driver.
void WriteRegister(volatile UInt32* const registers, const Size word, const UInt32 value) noexcept
{
    static_cast<void>(registers);
    g_hba.Write(word, value);
}

/// Compares encoded bytes including reserved fields, without native layouts.
[[nodiscard]] bool EqualBytes(const UInt8* const actual, const UInt8* const expected, const Size bytes) noexcept
{
    for (Size index{0U}; index < bytes; ++index)
    {
        if (actual[index] != expected[index])
        {
            return false;
        }
    }
    return true;
}

void TestStartStop(TestContext& runner) noexcept
{
    g_hba.Clear();
    g_hba.Words[1U] = 2U;
    g_hba.Words[Cmd] = 0x10000000U;
    g_hba.Words[Serr] = 0x1234U;
    g_hba.Words[Is] = 0x40000000U;
    ASTERIA_EXPECT(runner, "start enables AE, programs 64-bit bases, acknowledges status, then enables FRE and ST",
                   Ahci::StartAhciPort(g_hba.Words, Port, 0x1234567800001000U, 0x1234567800001400U, WriteRegister),
                   g_hba.WriteCount == 9U, g_hba.Writes[0U].Word == 1U, g_hba.Writes[0U].Value == 0x80000002U,
                   g_hba.Writes[1U].Word == Base, g_hba.Writes[1U].Value == 0x1000U, g_hba.Writes[2U].Word == Base + 1U,
                   g_hba.Writes[2U].Value == 0x12345678U, g_hba.Writes[3U].Word == Base + 2U,
                   g_hba.Writes[3U].Value == 0x1400U, g_hba.Writes[4U].Word == Base + 3U,
                   g_hba.Writes[4U].Value == 0x12345678U);
    ASTERIA_EXPECT(runner, "start acknowledges W1C bits and preserves other CMD bits with FRE preceding ST",
                   g_hba.Writes[5U].Word == Serr, g_hba.Writes[5U].Value == 0xFFFFFFFFU, g_hba.Writes[6U].Word == Is,
                   g_hba.Writes[6U].Value == 0xFFFFFFFFU, g_hba.Writes[7U].Word == Cmd,
                   g_hba.Writes[7U].Value == 0x10000010U, g_hba.Writes[8U].Word == Cmd,
                   (g_hba.Writes[8U].Value & 0x11U) == 0x11U, g_hba.Words[Serr] == 0U, g_hba.Words[Sig] == 0x101U);
    const Size startedWrites = g_hba.WriteCount;
    ASTERIA_EXPECT(runner, "a running port refuses to change DMA bases",
                   !Ahci::StartAhciPort(g_hba.Words, Port, 0x2000U, 0x2400U, WriteRegister),
                   g_hba.WriteCount == startedWrites);
    g_hba.Words[Ci] = 8U;
    ASTERIA_EXPECT(runner, "stop clears ST and waits CR before clearing FRE and waiting FR",
                   Ahci::StopAhciPort(g_hba.Words, Port, Clock, 10U, WriteRegister), g_hba.Writes[9U].Word == Cmd,
                   (g_hba.Writes[9U].Value & St) == 0U, (g_hba.Writes[9U].Value & Fre) != 0U,
                   g_hba.Writes[10U].Word == Cmd, (g_hba.Writes[10U].Value & (St | Cr | Fre)) == 0U,
                   g_hba.Words[Cmd] == 0x10000000U, g_hba.Words[Ci] == 0U);
    g_hba.Clear();
    g_hba.Words[1U] = 0x80000002U;
    ASTERIA_EXPECT(runner, "AE already enabled is preserved without a control write",
                   Ahci::StartAhciPort(g_hba.Words, Port, 0x1000U, 0x1400U, WriteRegister), g_hba.WriteCount == 8U,
                   g_hba.Writes[0U].Word == Base);
}

void TestTimeouts(TestContext& runner) noexcept
{
    g_hba.Clear();
    g_hba.Words[Cmd] = St | Cr | Fre | Fr;
    g_hba.Ports[Port].StuckCr = true;
    ASTERIA_EXPECT(runner, "CR stuck times out without illegally clearing FRE",
                   !Ahci::StopAhciPort(g_hba.Words, Port, Clock, 4U, WriteRegister), g_hba.Now == 5U,
                   g_hba.WriteCount == 1U, (g_hba.Words[Cmd] & Fre) != 0U);
    g_hba.Clear();
    g_hba.Words[Cmd] = St | Cr | Fre | Fr;
    g_hba.Ports[Port].StuckFr = true;
    ASTERIA_EXPECT(runner, "FR stuck also prevents a successful stop",
                   !Ahci::StopAhciPort(g_hba.Words, Port, Clock, 4U, WriteRegister), g_hba.WriteCount == 2U,
                   (g_hba.Words[Cmd] & Fr) != 0U);
    g_hba.Clear();
    g_hba.Words[Cmd] = St | Cr | Fre | Fr;
    g_hba.Ports[Port].CrDelay = 2U;
    g_hba.Ports[Port].FrDelay = 2U;
    ASTERIA_EXPECT(runner, "both stop waits share a budget and handle delayed engine shutdown",
                   Ahci::StopAhciPort(g_hba.Words, Port, Clock, 8U, WriteRegister), g_hba.Now == 5U,
                   (g_hba.Writes[1U].Value & (Cr | Fre | St)) == 0U);
    g_hba.Clear();
    g_hba.Words[Cmd] = St | Cr | Fre | Fr;
    g_hba.Ports[Port].CrDelay = 3U;
    g_hba.Ports[Port].FrDelay = 3U;
    ASTERIA_EXPECT(runner, "the FRE wait does not receive a fresh stop budget",
                   !Ahci::StopAhciPort(g_hba.Words, Port, Clock, 4U, WriteRegister), g_hba.Now == 5U);
    g_hba.Clear();
    g_hba.Now = 0xFFFFFFFFFFFFFFFEU;
    g_hba.Words[Cmd] = Cr;
    g_hba.Ports[Port].CrDelay = 3U;
    ASTERIA_EXPECT(runner, "tick subtraction handles unsigned clock wrap",
                   Ahci::StopAhciPort(g_hba.Words, Port, Clock, 4U, WriteRegister), g_hba.Now == 1U);
    g_hba.Clear();
    ASTERIA_EXPECT(runner, "already stopped succeeds with zero budget using the default MMIO writer",
                   Ahci::StopAhciPort(g_hba.Words, Port, Clock, 0U));
}

// Independent byte goldens include reserved header/FIS/PRDT fields.
constexpr UInt8 ReadHeader[32U]{
    0x05U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x00U, 0x00U, 0x78U, 0x56U, 0x34U, 0x12U,
};
constexpr UInt8 ReadFis[20U]{
    0x27U, 0x80U, 0x25U, 0x00U, 0x08U, 0x00U, 0x00U, 0x40U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr UInt8 ReadPrdt[16U]{
    0x02U, 0x10U, 0x00U, 0x00U, 0x21U, 0x43U, 0x65U, 0x87U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0x01U, 0x00U, 0x80U,
};
constexpr UInt8 WriteHeader[32U]{
    0x45U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x24U, 0x00U, 0x00U, 0xEFU, 0xCDU, 0xABU, 0x89U,
};
constexpr UInt8 WriteFis[20U]{
    0x27U, 0x80U, 0x35U, 0x00U, 0xBCU, 0x9AU, 0x78U, 0x40U, 0x56U, 0x34U,
    0x12U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr UInt8 WritePrdt[16U]{
    0x02U, 0x20U, 0x00U, 0x00U, 0x10U, 0x32U, 0x54U, 0x76U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0x03U, 0x00U, 0x80U,
};

void TestEncoding(TestContext& runner) noexcept
{
    g_hba.Clear();
    for (UInt8& byte : g_hba.Page)
    {
        byte = 0xCCU;
    }
    ASTERIA_EXPECT(
        runner, "read sector 8 encodes the header, LBA48 FIS and word-aligned 64-bit buffer PRDT",
        Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 1U, 0x8765432100001002U, 512U, 0x1234567800000500U),
        EqualBytes(g_hba.Page, ReadHeader, 32U), EqualBytes(g_hba.Page + 0x500U, ReadFis, 20U),
        EqualBytes(g_hba.Page + 0x580U, ReadPrdt, 16U), g_hba.Page[32U] == 0xCCU, g_hba.Page[0x400U] == 0xCCU,
        g_hba.Page[0x600U] == 0xCCU);
    ASTERIA_EXPECT(runner, "write two sectors at a 48-bit LBA uses slot 31 and its supplied table device address",
                   Ahci::BuildAhciReadWrite(g_hba.Page, 31U, true, 0x123456789ABCU, 2U, 0x7654321000002002U, 1024U,
                                            0x89ABCDEF00002400U),
                   EqualBytes(g_hba.Page + 0x3E0U, WriteHeader, 32U), EqualBytes(g_hba.Page + 0x2400U, WriteFis, 20U),
                   EqualBytes(g_hba.Page + 0x2480U, WritePrdt, 16U));
    bool reservedClear{true};
    for (Size index{20U}; index < 0x100U; ++index)
    {
        if ((index < 0x80U) || (index >= 0x90U))
        {
            reservedClear = reservedClear && (g_hba.Page[0x500U + index] == 0U) && (g_hba.Page[0x2400U + index] == 0U);
        }
    }
    ASTERIA_EXPECT(runner, "reused command tables have zero reserved bytes and unused command areas", reservedClear);
    ASTERIA_EXPECT(runner, "maximum transfer is 128 sectors and 64 KiB with zero count high byte",
                   Ahci::BuildAhciReadWrite(g_hba.Page, 1U, false, 0xFFFFFFFFFFFFU, 128U, 0x1002U, 65536U, 0x600U),
                   g_hba.Page[0x60CU] == 0x80U, g_hba.Page[0x60DU] == 0U, g_hba.Page[0x68CU] == 0xFFU,
                   g_hba.Page[0x68DU] == 0xFFU, g_hba.Page[0x68EU] == 0U, g_hba.Page[0x68FU] == 0x80U);
}

// IDENTIFY DEVICE as the kernel's boot diagnostic encodes it: no write bit,
// no device bits or count, 512 bytes without an interrupt on completion.
constexpr UInt8 IdentifyHeader[32U]{
    0x05U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x05U, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr UInt8 IdentifyFis[20U]{
    0x27U, 0x80U, 0xECU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
};
constexpr UInt8 IdentifyPrdt[16U]{
    0x00U, 0x0AU, 0x10U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0x01U, 0x00U, 0x00U,
};
constexpr UInt8 HighIdentifyHeader[16U]{
    0x05U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U, 0x67U, 0x45U, 0x23U, 0x01U, 0x00U, 0x00U, 0x00U,
};
constexpr UInt8 HighIdentifyPrdt[16U]{
    0x02U, 0x00U, 0xDEU, 0xBCU, 0x0AU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0x01U, 0x00U, 0x00U,
};

void TestIdentify(TestContext& runner) noexcept
{
    g_hba.Clear();
    for (UInt8& byte : g_hba.Page)
    {
        byte = 0xCCU;
    }
    ASTERIA_EXPECT(runner, "IDENTIFY DEVICE encodes its header, command FIS and one 512-byte PRDT entry",
                   Ahci::BuildAhciIdentify(g_hba.Page, 0U, 0x0010'0A00U, 0x0010'0500U),
                   EqualBytes(g_hba.Page, IdentifyHeader, 32U), EqualBytes(g_hba.Page + 0x500U, IdentifyFis, 20U),
                   EqualBytes(g_hba.Page + 0x580U, IdentifyPrdt, 16U));
    bool kept{true};
    for (Size index{0x600U}; index < sizeof(g_hba.Page); ++index)
    {
        kept = kept && (g_hba.Page[index] == 0xCCU);
    }
    bool cleared{true};
    for (Size index{20U}; index < 0x100U; ++index)
    {
        if ((index < 0x80U) || (index >= 0x90U))
        {
            cleared = cleared && (g_hba.Page[0x500U + index] == 0U);
        }
    }
    ASTERIA_EXPECT(runner, "IDENTIFY clears its slot's table but leaves the data buffer and other slots as they were",
                   kept, cleared, g_hba.Page[32U] == 0xCCU, g_hba.Page[0x400U] == 0xCCU);
    ASTERIA_EXPECT(runner, "IDENTIFY on a later slot carries 64-bit table and buffer addresses",
                   Ahci::BuildAhciIdentify(g_hba.Page, 5U, 0x0000'000A'BCDE'0002ULL, 0x0000'0001'2345'6780ULL),
                   EqualBytes(g_hba.Page + 0xA0U, HighIdentifyHeader, 16U), g_hba.Page[0xA00U] == 0x27U,
                   g_hba.Page[0xA02U] == 0xECU, EqualBytes(g_hba.Page + 0xA80U, HighIdentifyPrdt, 16U));
    for (UInt8& byte : g_hba.Page)
    {
        byte = 0xA7U;
    }
    ASTERIA_EXPECT(runner, "an invalid slot, an odd buffer or an unaligned table refuse IDENTIFY",
                   !Ahci::BuildAhciIdentify(g_hba.Page, 32U, 0x1000U, 0x500U),
                   !Ahci::BuildAhciIdentify(g_hba.Page, 0U, 0x1001U, 0x500U),
                   !Ahci::BuildAhciIdentify(g_hba.Page, 0U, 0x1000U, 0x540U));
    bool unchanged{true};
    for (const UInt8 byte : g_hba.Page)
    {
        unchanged = unchanged && (byte == 0xA7U);
    }
    ASTERIA_EXPECT(runner, "IDENTIFY refusals leave storage untouched", unchanged);
}

void TestValidation(TestContext& runner) noexcept
{
    g_hba.Clear();
    for (UInt8& byte : g_hba.Page)
    {
        byte = 0xA7U;
    }
    ASTERIA_EXPECT(runner, "invalid sector counts, byte counts, slot and DBA alignment refuse encoding",
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 0U, 0x1000U, 0U, 0x500U),
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 129U, 0x1000U, 66048U, 0x500U),
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 0xFFFFFFFFU, 0x1000U, 512U, 0x500U),
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 1U, 0x1000U, 511U, 0x500U),
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 1U, 0x1001U, 512U, 0x500U),
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 32U, false, 8U, 1U, 0x1000U, 512U, 0x500U));
    ASTERIA_EXPECT(runner, "out-of-range LBA and unaligned command table refuse encoding",
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, true, 0x1000000000000U, 1U, 0x1000U, 512U, 0x500U),
                   !Ahci::BuildAhciReadWrite(g_hba.Page, 0U, true, 8U, 1U, 0x1000U, 512U, 0x501U));
    bool unchanged{true};
    for (const UInt8 byte : g_hba.Page)
    {
        unchanged = unchanged && (byte == 0xA7U);
    }
    ASTERIA_EXPECT(runner, "all encoding refusals leave storage untouched", unchanged);
    ASTERIA_EXPECT(runner, "invalid port, reset interval and base alignment refuse register writes",
                   !Ahci::StartAhciPort(g_hba.Words, 32U, 0x1000U, 0x1400U, WriteRegister),
                   !Ahci::StartAhciPort(g_hba.Words, Port, 0x1001U, 0x1400U, WriteRegister),
                   !Ahci::StartAhciPort(g_hba.Words, Port, 0x1000U, 0x1401U, WriteRegister),
                   !Ahci::StopAhciPort(g_hba.Words, 32U, Clock, 10U, WriteRegister),
                   !Ahci::ResetAhciPort(g_hba.Words, 32U, Clock, 10U, 2U, WriteRegister),
                   !Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 0U, WriteRegister),
                   !Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 1U, 2U, WriteRegister), g_hba.WriteCount == 0U,
                   g_hba.Now == 0U);
    constexpr UInt32 engineBits[]{St, Cr, Fre, Fr};
    for (const UInt32 bit : engineBits)
    {
        g_hba.Words[Cmd] = bit;
        ASTERIA_EXPECT(runner, "every engine control or running bit prevents reprogramming DMA bases",
                       !Ahci::StartAhciPort(g_hba.Words, Port, 0x1000U, 0x1400U, WriteRegister),
                       g_hba.WriteCount == 0U);
    }
}

void TestIssueStatus(TestContext& runner) noexcept
{
    g_hba.Clear();
    g_hba.Words[Ci] = 2U;
    Ahci::IssueAhciCommand(g_hba.Words, Port, 31U, WriteRegister);
    ASTERIA_EXPECT(runner, "issue writes only its slot bit and hardware retains other issued slots",
                   g_hba.Writes[0U].Word == Ci, g_hba.Writes[0U].Value == 0x80000000U, g_hba.Words[Ci] == 0x80000002U,
                   !Ahci::IsAhciCommandDone(g_hba.Words, Port, 31U), Ahci::IsAhciCommandDone(g_hba.Words, Port, 0U));
    g_hba.Words[Ci] &= ~0x80000000U;
    ASTERIA_EXPECT(runner, "only the queried slot determines completion",
                   Ahci::IsAhciCommandDone(g_hba.Words, Port, 31U), !Ahci::IsAhciCommandDone(g_hba.Words, Port, 1U));
    g_hba.Words[Is] = 0x3FFFFFFFU;
    g_hba.Words[Tfd] = 0x188U;
    ASTERIA_EXPECT(runner, "other interrupt, BSY, DRQ and error-register bits are not task-file failure",
                   !Ahci::AhciCommandFailed(g_hba.Words, Port));
    g_hba.Words[Is] = 0x40000000U;
    ASTERIA_EXPECT(runner, "TFES independently reports failure", Ahci::AhciCommandFailed(g_hba.Words, Port));
    g_hba.Words[Is] = 0U;
    g_hba.Words[Tfd] = 1U;
    ASTERIA_EXPECT(runner, "PxTFD status ERR independently reports failure", Ahci::AhciCommandFailed(g_hba.Words, Port),
                   !Ahci::IsAhciCommandDone(g_hba.Words, Port, 1U));
}

void PreparePendingRead(TestContext& runner) noexcept
{
    g_hba.Clear();
    ASTERIA_EXPECT(runner, "prepare and start an issued read with retained DMA storage",
                   Ahci::BuildAhciReadWrite(g_hba.Page, 3U, false, 8U, 1U, 0x2000U, 512U, 0x1800U),
                   Ahci::StartAhciPort(g_hba.Words, Port, 0x1000U, 0x1400U, WriteRegister));
    g_hba.Words[Is] = 0U;
    g_hba.Page[0x440U] = 0U;
    Ahci::IssueAhciCommand(g_hba.Words, Port, 3U, WriteRegister);
    ASTERIA_EXPECT(runner, "command is pending before either stop or reset", g_hba.Pending,
                   !Ahci::IsAhciCommandDone(g_hba.Words, Port, 3U));
}

void TestStopIsNotQuiescence(TestContext& runner) noexcept
{
    PreparePendingRead(runner);
    ASTERIA_EXPECT(runner, "successful stop and empty CI do not prove QEMU DMA quiescence",
                   Ahci::StopAhciPort(g_hba.Words, Port, Clock, 10U, WriteRegister), g_hba.Words[Ci] == 0U,
                   Ahci::IsAhciCommandDone(g_hba.Words, Port, 3U), g_hba.Pending, g_hba.CompletionWrites == 0U,
                   g_hba.Buffer[0U] == 0U);
    for (UInt32 tick{0U}; tick < 110U; ++tick)
    {
        static_cast<void>(g_hba.Tick());
    }
    ASTERIA_EXPECT(runner, "late completion writes data and captured PRDBC after StopAhciPort returns", !g_hba.Pending,
                   g_hba.Buffer[0U] == 0xA5U, g_hba.Buffer[511U] == 0xA5U, g_hba.Page[0x65U] == 2U,
                   g_hba.CompletionWrites == 2U, g_hba.Page[0x440U] == 0U, g_hba.Words[Is] == 0U);
    PreparePendingRead(runner);
    WriteRegister(g_hba.Words, Cmd, g_hba.Words[Cmd] & ~St);
    ASTERIA_EXPECT(runner, "ST alone clears CI and CR immediately while leaving the FIS engine receiving",
                   g_hba.Words[Ci] == 0U, (g_hba.Words[Cmd] & Cr) == 0U, (g_hba.Words[Cmd] & Fre) != 0U);
    for (UInt32 tick{0U}; tick < 110U; ++tick)
    {
        static_cast<void>(g_hba.Tick());
    }
    ASTERIA_EXPECT(runner, "ST-only stop still receives the late D2H FIS and completion interrupt",
                   g_hba.Buffer[0U] == 0xA5U, g_hba.Page[0x65U] == 2U, g_hba.Page[0x440U] == 0x34U,
                   g_hba.Page[0x441U] == 0x40U, g_hba.Page[0x442U] == 0x50U, (g_hba.Words[Is] & 1U) != 0U,
                   g_hba.CompletionWrites == 3U);
}

void TestResetIsQuiescence(TestContext& runner) noexcept
{
    PreparePendingRead(runner);
    g_hba.Words[Sctl] = 0x320U;
    g_hba.Words[Serr] = 0xFFFFU;
    g_hba.Words[Is] = 0x40000000U;
    ASTERIA_EXPECT(runner, "COMRESET drains pending DMA synchronously and acknowledges status before success",
                   Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 20U, 3U, WriteRegister), !g_hba.Pending,
                   g_hba.Now < g_hba.Due, g_hba.Buffer[0U] == 0xA5U, g_hba.Page[0x65U] == 2U, g_hba.ResetCount == 1U,
                   g_hba.Ports[Port].PulseTicks >= 3U, g_hba.Words[Sctl] == 0x320U, g_hba.Words[Ssts] == 0x113U,
                   g_hba.Words[Ci] == 0U, g_hba.Words[Serr] == 0U, g_hba.Words[Is] == 0U);
    ASTERIA_EXPECT(runner, "reset writes DET 1 then 0 preserving speed and power policy",
                   g_hba.Writes[12U].Word == Sctl, g_hba.Writes[12U].Value == 0x321U, g_hba.Writes[13U].Word == Sctl,
                   g_hba.Writes[13U].Value == 0x320U, g_hba.Writes[14U].Word == Serr, g_hba.Writes[15U].Word == Is);
    const UInt32 drainedWrites = g_hba.CompletionWrites;
    // Scrub all DMA targets after recovery, then pass the original due tick.
    for (UInt8& byte : g_hba.Buffer)
    {
        byte = 0x77U;
    }
    for (UInt8& byte : g_hba.Page)
    {
        byte = 0x77U;
    }
    for (UInt32 tick{0U}; tick < 110U; ++tick)
    {
        static_cast<void>(g_hba.Tick());
    }
    bool untouched{true};
    for (const UInt8 byte : g_hba.Buffer)
    {
        untouched = untouched && (byte == 0x77U);
    }
    for (const UInt8 byte : g_hba.Page)
    {
        untouched = untouched && (byte == 0x77U);
    }
    ASTERIA_EXPECT(runner, "after successful COMRESET no later tick writes any DMA storage or completion status",
                   untouched, g_hba.CompletionWrites == drainedWrites, g_hba.Words[Is] == 0U,
                   g_hba.Words[Sig] == 0xFFFFFFFFU);
    ASTERIA_EXPECT(runner, "QEMU defers the reset signature until FIS reception restarts",
                   Ahci::StartAhciPort(g_hba.Words, Port, 0x1000U, 0x1400U, WriteRegister), g_hba.Words[Sig] == 0x101U,
                   g_hba.Page[0x440U] == 0x34U);
}

void TestResetFailures(TestContext& runner) noexcept
{
    PreparePendingRead(runner);
    g_hba.Ports[Port].StuckCr = true;
    ASTERIA_EXPECT(runner, "recovery still COMRESETs after CR stop timeout and drains before returning",
                   Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 3U, WriteRegister), g_hba.ResetCount == 1U,
                   !g_hba.Pending, g_hba.Buffer[0U] == 0xA5U, g_hba.Words[Sig] == 0x101U, g_hba.CompletionWrites == 3U,
                   g_hba.Words[Is] == 0U);
    g_hba.Clear();
    g_hba.Ports[Port].Device = false;
    g_hba.Words[Ssts] = 0U;
    ASTERIA_EXPECT(runner, "an empty port resets successfully without requiring a device signature",
                   Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 3U, WriteRegister), g_hba.Words[Ssts] == 0U,
                   g_hba.Words[Sig] == 0xFFFFFFFFU);
    g_hba.Clear();
    g_hba.Ports[Port].LinkDelay = 3U;
    ASTERIA_EXPECT(runner, "link recovery can take later clock ticks",
                   Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 3U, WriteRegister),
                   g_hba.Ports[Port].LinkDelay == 0U, g_hba.Words[Ssts] == 0x113U);
    g_hba.Clear();
    g_hba.Ports[Port].StuckLink = true;
    ASTERIA_EXPECT(runner, "a PHY that never settles times out with DET released",
                   !Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 3U, WriteRegister), g_hba.ResetCount == 1U,
                   (g_hba.Words[Sctl] & 0x0FU) == 0U, g_hba.Now == 12U);
    g_hba.Clear();
    g_hba.Ports[Port].StuckCi = true;
    ASTERIA_EXPECT(runner, "reset refuses quiescence when commands remain issued",
                   !Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 3U, WriteRegister), g_hba.Words[Ci] == 1U);
    g_hba.Clear();
    g_hba.TickStep = 20U;
    ASTERIA_EXPECT(runner, "a clock jump past the reset budget releases DET and reports timeout",
                   !Ahci::ResetAhciPort(g_hba.Words, Port, Clock, 10U, 3U, WriteRegister), g_hba.ResetCount == 1U,
                   (g_hba.Words[Sctl] & 0x0FU) == 0U);
}
} // namespace
} // namespace Asteria::Test

int main() noexcept
{
    TestContext runner{"AhciPortTests"};
    Asteria::Test::TestStartStop(runner);
    Asteria::Test::TestTimeouts(runner);
    Asteria::Test::TestEncoding(runner);
    Asteria::Test::TestIdentify(runner);
    Asteria::Test::TestValidation(runner);
    Asteria::Test::TestIssueStatus(runner);
    Asteria::Test::TestStopIsNotQuiescence(runner);
    Asteria::Test::TestResetIsQuiescence(runner);
    Asteria::Test::TestResetFailures(runner);
    return runner.Finish();
}
