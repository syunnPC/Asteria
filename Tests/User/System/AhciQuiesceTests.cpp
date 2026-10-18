// SPDX-License-Identifier: Apache-2.0
#include "User/System/AhciQuiesce.hpp"

#include "Support/Random.hpp"
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
namespace Ahci = User::System;
using Ahci::AhciQuiescePhase;
using Ahci::AhciQuiesceProgress;
using Ahci::AhciQuiesceState;

// Independent AHCI offsets and masks; no large objects on the host stack.
constexpr Size Ghc{1U};
constexpr Size HbaIs{2U};
constexpr Size Pi{3U};
constexpr Size Cmd{6U};
constexpr Size Is{4U};
constexpr Size Ie{5U};
constexpr Size Sctl{11U};
constexpr Size Ssts{10U};
constexpr Size Ci{14U};
constexpr Size Serr{12U};
constexpr UInt32 St{1U};
constexpr UInt32 Fre{0x10U};
constexpr UInt32 Fr{0x4000U};
constexpr UInt32 Cr{0x8000U};
TestSupport::FakeAhciHba g_hba;
bool g_resetWhileRunning{false};

[[nodiscard]] constexpr Size PortWord(const UInt32 port) noexcept
{
    return (0x100U >> 2U) + (static_cast<Size>(port) << 5U);
}

void WriteRegister(volatile UInt32* const registers, const Size word, const UInt32 value) noexcept
{
    static_cast<void>(registers);
    if ((word >= PortWord(0U)) && (((word - PortWord(0U)) & 31U) == Sctl))
    {
        const Size base = word - Sctl;
        g_resetWhileRunning = g_resetWhileRunning || ((g_hba.Words[base + Cmd] & (St | Cr | Fr)) != 0U);
    }
    g_hba.Write(word, value);
}

void Prepare() noexcept
{
    g_hba.Clear(0U);
    g_resetWhileRunning = false;
    g_hba.Words[Ghc] = 0x80000002U;
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        const Size base = PortWord(port);
        g_hba.Words[base + Ie] = 0xFFFFFFFFU;
        g_hba.Words[base + Sctl] = 0xA530U + (port << 8U);
    }
}

[[nodiscard]] bool InterruptsMasked() noexcept
{
    if ((g_hba.Words[Ghc] & 2U) != 0U)
    {
        return false;
    }
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        if (g_hba.Words[PortWord(port) + Ie] != 0U)
        {
            return false;
        }
    }
    return true;
}

void TestIssuedRead(TestContext& runner) noexcept
{
    Prepare();
    ASTERIA_EXPECT(runner, "port zero starts a read among six implemented ports",
                   Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 1U, 0x2000U, 512U, 0x1500U),
                   Ahci::StartAhciPort(g_hba.Words, 0U, 0x1000U, 0x1400U, WriteRegister));
    Ahci::IssueAhciCommand(g_hba.Words, 0U, 0U, WriteRegister);
    const Size beforeBegin = g_hba.WriteCount;
    AhciQuiesceState state{};
    Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
    ASTERIA_EXPECT(runner, "begin captures PI and running before QEMU's ST stop clears CI",
                   state.ImplementedPorts == 0x3FU, state.Running, state.Issued,
                   state.Phase == AhciQuiescePhase::WaitingForCr, g_hba.Words[PortWord(0U) + Ci] == 0U, g_hba.Pending,
                   g_hba.CompletionWrites == 0U, InterruptsMasked(), g_hba.Words[Ghc] == 0x80000000U,
                   g_hba.Writes[beforeBegin].Word == Ghc, g_hba.Writes[beforeBegin].Value == 0x80000000U);
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        ASTERIA_EXPECT(runner, "begin clears ST on every implemented port",
                       (g_hba.Words[PortWord(port) + Cmd] & St) == 0U);
    }
    const Size beforeFirst = g_hba.WriteCount;
    const AhciQuiesceProgress first = Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister);
    const Size afterFirst = g_hba.WriteCount;
    ASTERIA_EXPECT(runner, "first poll stops FRE and asserts COMRESET without releasing it or advancing a clock",
                   first == AhciQuiesceProgress::Running, state.Phase == AhciQuiescePhase::ResetAsserted,
                   afterFirst == beforeFirst + 12U, g_hba.Now == 0U, g_hba.Pending, g_hba.ResetCount == 0U,
                   g_hba.CompletionWrites == 0U, g_hba.Buffer[0U] == 0U, InterruptsMasked(), !g_resetWhileRunning);
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        const Size base = PortWord(port);
        ASTERIA_EXPECT(runner, "first poll writes only DET one and preserves speed and power bits on all ports",
                       g_hba.Words[base + Sctl] == (0xA531U + (port << 8U)),
                       g_hba.Writes[beforeFirst + 6U + port].Word == base + Sctl,
                       g_hba.Writes[beforeFirst + 6U + port].Value == g_hba.Words[base + Sctl],
                       (g_hba.Words[base + Cmd] & (St | Fre | Cr | Fr)) == 0U);
    }
    // One tick represents 1 ms; the Device Manager will pause 10 ms instead.
    static_cast<void>(g_hba.Tick());
    const AhciQuiesceProgress second = Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister);
    ASTERIA_EXPECT(runner, "QEMU quiesces in exactly two polls and drains retained data and PRDBC before Quiet",
                   second == AhciQuiesceProgress::Quiet, state.Phase == AhciQuiescePhase::Quiet, !g_hba.Pending,
                   g_hba.Now == 1U, g_hba.Now < g_hba.Due, g_hba.ResetCount == 6U, g_hba.CompletionWrites == 2U,
                   g_hba.Page[4U] == 0U, g_hba.Page[5U] == 2U, g_hba.Page[6U] == 0U, g_hba.Page[7U] == 0U,
                   InterruptsMasked(), !g_resetWhileRunning);
    bool landed{true};
    for (const UInt8 byte : g_hba.Buffer)
    {
        landed = landed && (byte == 0xA5U);
    }
    ASTERIA_EXPECT(runner, "the whole read landed", landed);
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        const Size base = PortWord(port);
        ASTERIA_EXPECT(runner, "DET zero is written in the second poll after the pulse, including empty ports",
                       g_hba.Writes[afterFirst + port].Word == base + Sctl,
                       g_hba.Writes[afterFirst + port].Value == (0xA530U + (port << 8U)),
                       g_hba.Ports[port].ResetCount == 1U, g_hba.Ports[port].PulseTicks == 1U,
                       g_hba.Words[base + Ssts] == (port == 0U ? 0x113U : 0U), g_hba.Words[base + 9U] == 0xFFFFFFFFU);
    }
    ASTERIA_EXPECT(runner, "QEMU's recomputed HBA IS is zero after masking and port acknowledgement",
                   Ahci::AcknowledgeAhci(g_hba.Words, state, WriteRegister) == 0U, InterruptsMasked());
    const Size quietWrites = g_hba.WriteCount;
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
    ASTERIA_EXPECT(runner, "no later tick writes recycled DMA storage or completion status; Quiet polls write nothing",
                   untouched, g_hba.CompletionWrites == 2U, g_hba.Words[PortWord(0U) + Is] == 0U,
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Quiet,
                   g_hba.WriteCount == quietWrites, InterruptsMasked());
}

void TestAcknowledge(TestContext& runner) noexcept
{
    Prepare();
    AhciQuiesceState state{};
    Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
    ASTERIA_EXPECT(runner, "idle controller still asserts reset in its first poll", !state.Running, !state.Issued,
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "idle controller also becomes quiet in two polls",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Quiet);
    // Exercise a latched HBA IS as on hardware; QEMU recomputes it on PxIS
    // writes and has zero pending by the time AcknowledgeAhci reads HBA IS.
    g_hba.RecomputeHbaStatus = false;
    g_hba.Words[HbaIs] = 0x25U;
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        g_hba.Words[PortWord(port) + Is] = 0x40000001U;
        g_hba.Words[PortWord(port) + Serr] = 0x00FF1234U;
    }
    const Size before = g_hba.WriteCount;
    const UInt32 acknowledged = Ahci::AcknowledgeAhci(g_hba.Words, state, WriteRegister);
    ASTERIA_EXPECT(runner, "acknowledgement returns the HBA IS it read and writes that value after all ports",
                   acknowledged == 0x25U, g_hba.Words[HbaIs] == 0U, g_hba.WriteCount == before + 13U,
                   g_hba.Writes[before + 12U].Word == HbaIs, g_hba.Writes[before + 12U].Value == 0x25U,
                   InterruptsMasked());
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        const Size base = PortWord(port);
        const Size offset = before + (static_cast<Size>(port) << 1U);
        ASTERIA_EXPECT(runner, "each port acknowledges SERR before IS with write ones", g_hba.Words[base + Serr] == 0U,
                       g_hba.Words[base + Is] == 0U, g_hba.Writes[offset].Word == base + Serr,
                       g_hba.Writes[offset].Value == 0xFFFFFFFFU, g_hba.Writes[offset + 1U].Word == base + Is,
                       g_hba.Writes[offset + 1U].Value == 0xFFFFFFFFU);
    }
}

void TestDelayedEngines(TestContext& runner) noexcept
{
    Prepare();
    for (UInt32 port{0U}; port < 6U; ++port)
    {
        g_hba.Words[PortWord(port) + Cmd] = St | Cr | Fre | Fr;
    }
    g_hba.Ports[5U].CrDelay = 2U;
    g_hba.Ports[4U].FrDelay = 2U;
    AhciQuiesceState state{};
    Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
    const Size begunWrites = g_hba.WriteCount;
    for (UInt32 poll{0U}; poll < 2U; ++poll)
    {
        ASTERIA_EXPECT(runner, "a later port's CR keeps every FIS engine enabled, with no spinning or DET writes",
                       Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running,
                       state.Phase == AhciQuiescePhase::WaitingForCr, g_hba.WriteCount == begunWrites);
        for (UInt32 port{0U}; port < 6U; ++port)
        {
            ASTERIA_EXPECT(runner, "FRE stays set on all ports while any CR is set",
                           (g_hba.Words[PortWord(port) + Cmd] & Fre) != 0U);
        }
        static_cast<void>(g_hba.Tick());
    }
    ASTERIA_EXPECT(runner, "third poll clears every FRE but waits for the delayed FR before COMRESET",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running,
                   state.Phase == AhciQuiescePhase::WaitingForFr, g_hba.WriteCount == begunWrites + 6U);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "fourth poll does not repeat FRE writes or reset while FR is set",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running,
                   g_hba.WriteCount == begunWrites + 6U);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "fifth poll asserts reset once every engine is stopped",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running,
                   state.Phase == AhciQuiescePhase::ResetAsserted, !g_resetWhileRunning);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "sixth poll releases reset and becomes quiet",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Quiet,
                   g_hba.ResetCount == 6U, InterruptsMasked(), !g_resetWhileRunning);
}

void TestStuckEngines(TestContext& runner) noexcept
{
    for (UInt32 engine{0U}; engine < 2U; ++engine)
    {
        Prepare();
        const Size base = PortWord(5U);
        g_hba.Words[base + Cmd] = St | Cr | Fre | Fr;
        g_hba.Ports[5U].StuckCr = engine == 0U;
        g_hba.Ports[5U].StuckFr = engine == 1U;
        AhciQuiesceState state{};
        Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
        bool running{true};
        for (UInt32 poll{0U}; poll < 100U; ++poll)
        {
            running =
                (Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running) && running;
            static_cast<void>(g_hba.Tick());
        }
        bool noDetWrites{true};
        for (Size index{0U}; index < g_hba.WriteCount; ++index)
        {
            const Size word = g_hba.Writes[index].Word;
            noDetWrites = noDetWrites && ((word < PortWord(0U)) || (((word - PortWord(0U)) & 31U) != Sctl));
        }
        ASTERIA_EXPECT(runner, "a stuck engine remains Running for the caller's bound without any DET write", running,
                       noDetWrites, g_hba.ResetCount == 0U, !g_resetWhileRunning, InterruptsMasked(),
                       (g_hba.Words[base + Cmd] & St) == 0U,
                       (g_hba.Words[base + Cmd] & (engine == 0U ? Fre : Fr)) != 0U);
    }
}

void TestQuietConditions(TestContext& runner) noexcept
{
    Prepare();
    g_hba.Ports[5U].StuckLink = true;
    AhciQuiesceState state{};
    Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
    ASTERIA_EXPECT(runner, "begin link test with a reset pulse",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "released reset does not imply Quiet when a later port's PHY has not settled",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running,
                   state.Phase == AhciQuiescePhase::WaitingForQuiet, g_hba.ResetCount == 6U);
    const Size resetWrites = g_hba.WriteCount;
    const Size base = PortWord(5U);
    constexpr UInt32 unsettled[]{1U, 2U, 4U};
    for (const UInt32 det : unsettled)
    {
        g_hba.Words[base + Ssts] = det;
        static_cast<void>(g_hba.Tick());
        ASTERIA_EXPECT(runner, "only DET zero or three counts as settled; later polls never repeat reset",
                       Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running,
                       g_hba.WriteCount == resetWrites);
    }
    g_hba.Words[base + Ssts] = 0U;
    g_hba.Words[base + Ci] = 0x80000000U;
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "any issued slot prevents Quiet even on an empty port",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running);
    g_hba.Words[base + Ci] = 0U;
    constexpr UInt32 engines[]{Cr, Fr};
    for (const UInt32 bit : engines)
    {
        g_hba.Words[base + Cmd] = bit;
        static_cast<void>(g_hba.Tick());
        ASTERIA_EXPECT(runner, "both CR and FR independently prevent Quiet after reset",
                       Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running);
    }
    g_hba.Words[base + Cmd] = 0U;
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "a later poll reports Quiet once the final empty port and engines settle",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Quiet,
                   g_hba.WriteCount == resetWrites, !g_resetWhileRunning);
}

void TestRunningAndCapturedPorts(TestContext& runner) noexcept
{
    constexpr UInt32 commands[]{0U, St, 0U, Fre | Fr, Cr};
    constexpr UInt32 issued[]{0U, 0U, 0x80000000U, 0U, 0U};
    for (UInt32 item{0U}; item < 5U; ++item)
    {
        Prepare();
        g_hba.Words[PortWord(5U) + Cmd] = commands[item];
        g_hba.Words[PortWord(5U) + Ci] = issued[item];
        g_hba.Words[PortWord(6U) + Cmd] = St | Cr;
        g_hba.Words[PortWord(6U) + Ci] = 1U;
        AhciQuiesceState state{
            .ImplementedPorts = 0xFFFFFFFFU,
            .Phase = AhciQuiescePhase::Quiet,
            .Running = true,
            .Issued = true,
        };
        Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
        ASTERIA_EXPECT(runner, "Running records only implemented ST or CI at begin and replaces old state",
                       state.Running == ((item == 1U) || (item == 2U)), state.Issued == (item == 2U),
                       state.ImplementedPorts == 0x3FU, state.Phase == AhciQuiescePhase::WaitingForCr,
                       g_hba.Words[PortWord(6U) + Cmd] == (St | Cr), g_hba.Words[PortWord(6U) + Ci] == 1U);
    }
    Prepare();
    g_hba.Words[Pi] = 0x80000002U;
    g_hba.Words[PortWord(31U) + Cmd] = St | Cr | Fre | Fr;
    g_hba.Words[PortWord(31U) + Ie] = 0xFFFFFFFFU;
    AhciQuiesceState state{};
    Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
    g_hba.Words[Pi] = 0U;
    ASTERIA_EXPECT(runner, "a sparse PI includes port 31 and later polls use the captured mask",
                   state.ImplementedPorts == 0x80000002U, state.Running,
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Running);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "both sparse empty ports reset and settle without touching unimplemented port zero",
                   Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister) == AhciQuiesceProgress::Quiet,
                   g_hba.ResetCount == 2U, g_hba.Ports[31U].ResetCount == 1U, g_hba.Ports[1U].ResetCount == 1U,
                   g_hba.Words[PortWord(0U) + Ie] == 0xFFFFFFFFU, g_hba.Words[PortWord(31U) + Ie] == 0U,
                   !g_resetWhileRunning);
    g_hba.RecomputeHbaStatus = false;
    g_hba.Words[HbaIs] = 0x80000002U;
    g_hba.Words[PortWord(0U) + Is] = 0x100U;
    g_hba.Words[PortWord(0U) + Serr] = 0x200U;
    ASTERIA_EXPECT(runner, "acknowledgement also uses captured PI and skips other ports",
                   Ahci::AcknowledgeAhci(g_hba.Words, state, WriteRegister) == 0x80000002U,
                   g_hba.Words[PortWord(0U) + Is] == 0x100U, g_hba.Words[PortWord(0U) + Serr] == 0x200U);
}

void TestDefaultWriter(TestContext& runner) noexcept
{
    Prepare();
    g_hba.Words[Pi] = 1U;
    g_hba.Words[PortWord(0U) + Ssts] = 0U;
    AhciQuiesceState state{};
    Ahci::BeginAhciQuiesce(g_hba.Words, state);
    ASTERIA_EXPECT(runner, "ordinary two-argument calls use direct MMIO writes", !state.Running,
                   (g_hba.Words[Ghc] & 2U) == 0U, g_hba.Words[PortWord(0U) + Ie] == 0U,
                   Ahci::PollAhciQuiesce(g_hba.Words, state) == AhciQuiesceProgress::Running,
                   g_hba.Words[PortWord(0U) + Sctl] == 0xA531U);
    static_cast<void>(g_hba.Tick());
    ASTERIA_EXPECT(runner, "direct writer releases DET on the next poll and acknowledges the exact HBA value",
                   Ahci::PollAhciQuiesce(g_hba.Words, state) == AhciQuiesceProgress::Quiet,
                   g_hba.Words[PortWord(0U) + Sctl] == 0xA530U);
    g_hba.Words[HbaIs] = 1U;
    ASTERIA_EXPECT(runner, "direct acknowledgement writes port ones and the read HBA IS",
                   Ahci::AcknowledgeAhci(g_hba.Words, state) == 1U, g_hba.Words[PortWord(0U) + Is] == 0xFFFFFFFFU,
                   g_hba.Words[PortWord(0U) + Serr] == 0xFFFFFFFFU, g_hba.Words[HbaIs] == 1U);
}

// Independent schedule model: all CR delays run from Begin; FR delays run
// from the last CR's stop; link delays run from COMRESET release. Comparing
// absolute ticks, rather than duplicating the phase switch, catches an early
// reset or Quiet, a repeated register write and waits that waste a poll.
void TestRecoverySchedules(TestContext& runner) noexcept
{
    const TestSupport::SeedRange seeds = TestSupport::GetSeedRange(0xA1C1U, 32U);
    for (Foundation::UInt64 seedIndex{0U}; seedIndex < seeds.Count; ++seedIndex)
    {
        const Foundation::UInt64 seed = seeds.First + seedIndex;
        TestSupport::Random random{seed};
        Prepare();
        ASTERIA_EXPECT(runner, "schedule model prepares an issued read",
                       Ahci::BuildAhciReadWrite(g_hba.Page, 0U, false, 8U, 1U, 0x2000U, 512U, 0x1500U),
                       Ahci::StartAhciPort(g_hba.Words, 0U, 0x1000U, 0x1400U, WriteRegister));
        Ahci::IssueAhciCommand(g_hba.Words, 0U, 0U, WriteRegister);
        UInt32 lastCr{0U};
        UInt32 lastFr{0U};
        UInt32 lastLink{0U};
        for (UInt32 port{0U}; port < 6U; ++port)
        {
            TestSupport::FakeAhciPortState& modelPort = g_hba.Ports[port];
            modelPort.CrDelay = static_cast<UInt32>(random.Below(4U));
            modelPort.FrDelay = static_cast<UInt32>(random.Below(4U));
            modelPort.LinkDelay = static_cast<UInt32>(random.Below(4U));
            lastCr = lastCr < modelPort.CrDelay ? modelPort.CrDelay : lastCr;
            lastFr = lastFr < modelPort.FrDelay ? modelPort.FrDelay : lastFr;
            lastLink = lastLink < modelPort.LinkDelay ? modelPort.LinkDelay : lastLink;
            g_hba.Words[PortWord(port) + Cmd] = St | Cr | Fre | Fr;
        }
        const UInt32 assertAt = lastCr + lastFr;
        const UInt32 quietAt = assertAt + 1U + lastLink;
        AhciQuiesceState state{};
        Ahci::BeginAhciQuiesce(g_hba.Words, state, WriteRegister);
        const Size begunWrites = g_hba.WriteCount;
        for (UInt32 step{0U}; step <= quietAt + 1U; ++step)
        {
            const AhciQuiesceProgress actual = Ahci::PollAhciQuiesce(g_hba.Words, state, WriteRegister);
            const bool quiet = step >= quietAt;
            const bool drained = step > assertAt;
            const Size expectedWrites =
                begunWrites + (step >= lastCr ? 6U : 0U) + (step >= assertAt ? 6U : 0U) + (drained ? 6U : 0U);
            bool registersMatch{true};
            for (UInt32 port{0U}; port < 6U; ++port)
            {
                registersMatch =
                    registersMatch && ((g_hba.Words[PortWord(port) + Sctl] & 0x0FU) == (step == assertAt ? 1U : 0U));
            }
            const bool matches = (actual == (quiet ? AhciQuiesceProgress::Quiet : AhciQuiesceProgress::Running)) &&
                                 (g_hba.WriteCount == expectedWrites) && (g_hba.Pending == !drained) &&
                                 (g_hba.CompletionWrites == (drained ? 2U : 0U)) &&
                                 (g_hba.ResetCount == (drained ? 6U : 0U)) && registersMatch && state.Running &&
                                 InterruptsMasked() && !g_resetWhileRunning;
            if (!matches)
            {
                static_cast<void>(std::fprintf(stderr, "seed=%llu step=%u operation=PollAhciQuiesce\n",
                                               static_cast<unsigned long long>(seed), step));
            }
            ASTERIA_EXPECT(runner, "poll result, writes and DMA lifetime match the independent recovery schedule",
                           matches);
            static_cast<void>(g_hba.Tick());
        }
    }
}
} // namespace
} // namespace Asteria::Test

int main() noexcept
{
    TestContext runner{"AhciQuiesceTests"};
    Asteria::Test::TestIssuedRead(runner);
    Asteria::Test::TestAcknowledge(runner);
    Asteria::Test::TestDelayedEngines(runner);
    Asteria::Test::TestStuckEngines(runner);
    Asteria::Test::TestQuietConditions(runner);
    Asteria::Test::TestRunningAndCapturedPorts(runner);
    Asteria::Test::TestDefaultWriter(runner);
    Asteria::Test::TestRecoverySchedules(runner);
    return runner.Finish();
}
