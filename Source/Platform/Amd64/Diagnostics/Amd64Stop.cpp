// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Firmware/AcpiPowerControl.hpp"
#include "Platform/Amd64/Interrupt/Amd64LocalApic.hpp"

#include "Kernel/Diagnostics/StopScreen.hpp"
#include "Kernel/Diagnostics/StopDisplay.hpp"
#include "Kernel/Diagnostics/StopScreenLayout.hpp"
#include "Platform/Amd64/Diagnostics/Amd64EvidenceRecord.hpp"
#include "Platform/Amd64/Diagnostics/Amd64StopStage.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Amd64/Memory/InitialPageTableBuilder.hpp"
#include "Platform/Synchronization/DeviceMemoryBarrier.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#if defined(ASTERIA_TARGET_AMD64)
#if __has_include("Generated/StopBuildIdentity.hpp")
#include "Generated/StopBuildIdentity.hpp"
#else
// Hosted analyzer compiles target adapters without a firmware build tree.
#define ASTERIA_STOP_BUILD_IDENTITY "host-analysis"
#endif
#endif
#if !defined(ASTERIA_AMD64_STOP_PROBE)
#define ASTERIA_AMD64_STOP_PROBE 0
#endif

namespace Asteria::Platform::Amd64::Diagnostics
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using Kernel::Diagnostics::StopCode;
using Kernel::Diagnostics::StopState;
using Synchronization::AtomicUInt64Reference;
constexpr UInt64 ApertureBase{0xFFFF'8000'0000'0000ULL};
constexpr UInt64 PageBytes{4096U};
constexpr Size FreezePolls{2'000'000U};
constinit Kernel::Diagnostics::StopLatch g_latch;
constinit Synchronization::AtomicUInt64 g_writer;
constinit Synchronization::AtomicUInt64 g_online;
constinit Synchronization::AtomicUInt64 g_powerRequested;
constinit Synchronization::AtomicUInt64 g_powerConfirmed;
Boot::BootDisplay g_display{};
Kernel::Device::DeviceRegion* g_scanoutRegion{nullptr};
UInt64 g_root{0U};
UInt64 g_tables{0U};
UInt64 g_apic{0U};
constexpr UInt64 UnknownProcessor{4U};
#if defined(ASTERIA_TARGET_AMD64)
UInt64 g_startTicks{0U};
#endif
UInt64 g_frameBytes{0U};
UInt32 g_hardwareIds[4U]{};
Size g_processors{1U};
bool g_injectionPending{false};

#if defined(ASTERIA_TARGET_AMD64)
[[nodiscard]] UInt64 Ticks() noexcept
{
    UInt32 low{0U};
    UInt32 high{0U};
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return (UInt64{high} << 32U) | low;
}
[[nodiscard]] UInt64 CurrentProcessor() noexcept
{
    // Neither backend uses GS: an NMI may enter during the SWAPGS window.
    // CPUID leaf 1 truncates x2APIC IDs, including IDs that alias the BSP.
    UInt32 eax{0U};
    UInt32 ebx{0U};
    UInt32 ecx{0U};
    UInt32 edx{0U};
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1U), "c"(0U));
    UInt32 baseLow{0U};
    UInt32 baseHigh{0U};
    __asm__ __volatile__("rdmsr" : "=a"(baseLow), "=d"(baseHigh) : "c"(0x1BU));
    UInt32 id{ebx >> 24U};
    if ((baseLow & 0x400U) != 0U)
    {
        __asm__ __volatile__("rdmsr" : "=a"(id), "=d"(baseHigh) : "c"(0x802U));
    }
    for (Size cpu{0U}; cpu < g_processors; ++cpu)
    {
        if (g_hardwareIds[cpu] == id)
        {
            return cpu;
        }
    }
    return UnknownProcessor;
}
void Capture(Amd64StopSnapshot& snapshot, const Interrupt::Amd64TrapFrame* const frame) noexcept
{
    __asm__ __volatile__("mov %%cr2,%0" : "=r"(snapshot.Cr2));
    __asm__ __volatile__("mov %%cr3,%0" : "=r"(snapshot.Cr3));
    if (frame != nullptr)
    {
        snapshot.Vector = frame->Vector;
        snapshot.Error = frame->Error;
        snapshot.Rip = frame->Rip;
        snapshot.Cs = frame->Cs;
        snapshot.Rflags = frame->Rflags;
        snapshot.Rsp = frame->Rsp;
    }
    else
    {
        snapshot.Rip = reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0));
        __asm__ __volatile__("mov %%cs,%w0" : "=r"(snapshot.Cs));
        snapshot.Cs &= 0xFFFFU;
        __asm__ __volatile__("pushfq; popq %0" : "=r"(snapshot.Rflags));
        __asm__ __volatile__("mov %%rsp,%0" : "=r"(snapshot.Rsp));
    }
}
[[noreturn]] void Park() noexcept
{
    // NMI blocking remains active when entered from vector 2. A further NMI
    // stays pending; no IRET or stack reuse is needed in this terminal state.
    for (;;)
    {
        __asm__ __volatile__("cli; hlt" : : : "memory");
    }
}
[[nodiscard]] UInt64* Table(const UInt64 address) noexcept
{
    // Boot-reserved page-aligned UInt64 page tables, identity mapped RW/NX.
    return reinterpret_cast<UInt64*>(static_cast<Foundation::UIntPointer>(address));
}
void SendFreeze(const UInt64 cpu, const UInt64 online) noexcept
{
    if (g_apic == 0U)
    {
        return;
    }
    Interrupt::Amd64LocalApic apic;
    if (!apic.Initialize(g_apic))
    {
        return;
    }
    for (Size target{0U}; target < g_processors; ++target)
    {
        if (target == cpu || (online & (UInt64{1U} << target)) == 0U)
        {
            continue;
        }
        static_cast<void>(apic.TrySendNmi(g_hardwareIds[target]));
    }
}
void EnsureScanoutPat() noexcept
{
    UInt32 low{0U};
    UInt32 high{0U};
    __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(0x277U));
    const UInt64 expected = Memory::ExpectedInitialPatValue;
    if ((low != static_cast<UInt32>(expected & 0xFFFF'FFFFU)) || (high != (expected >> 32U)))
    {
        // Early stops may precede normal PAT installation. Match its layout
        // using the cache-disable/WBINVD sequence before installing any leaves.
        UInt64 cr0{0U};
        __asm__ __volatile__("mov %%cr0,%0" : "=r"(cr0));
        const UInt64 disabled = (cr0 | (UInt64{1U} << 30U)) & ~(UInt64{1U} << 29U);
        __asm__ __volatile__("mov %0,%%cr0; wbinvd" : : "r"(disabled) : "memory");
        low = static_cast<UInt32>(expected & 0xFFFF'FFFFU);
        high = static_cast<UInt32>(expected >> 32U);
        __asm__ __volatile__("wrmsr; wbinvd" : : "a"(low), "d"(high), "c"(0x277U) : "memory");
        __asm__ __volatile__("mov %0,%%cr0" : : "r"(cr0) : "memory");
    }
}
void Draw(const UInt64 cpu, Amd64StopsEvidence& evidence) noexcept
{
    if (g_frameBytes == 0U || g_root == 0U)
    {
        return;
    }
    __asm__ __volatile__("mov %0,%%cr3" : : "r"(g_root) : "memory");
    EnsureScanoutPat();
    Kernel::Memory::MemoryCachePolicy selected{Kernel::Memory::MemoryCachePolicy::WriteCombining};
    if ((g_scanoutRegion != nullptr) && !g_scanoutRegion->FreezeMappingCachePolicy(selected))
    {
        return;
    }
    const UInt64 attributes = Amd64StopScanoutAttributes(selected);
    if (attributes == 0U)
    {
        return;
    }
    Table(g_root)[256U] = g_tables | 3U;
    const UInt64 pages = (g_frameBytes + PageBytes - 1U) / PageBytes;
    for (UInt64 page{0U}; page < pages; ++page)
    {
        Table(g_tables + 2U * PageBytes)[page] = (g_display.FrameBufferBase + page * PageBytes) | attributes;
    }
    __asm__ __volatile__("mov %0,%%cr3" : : "r"(g_root) : "memory");
    Kernel::Diagnostics::StopScreen screen{
        reinterpret_cast<volatile UInt32*>(static_cast<Foundation::UIntPointer>(ApertureBase)), g_display.Width,
        g_display.Height, g_display.PixelsPerScanLine, g_display.Format == Boot::BootDisplayFormatRgbx};
    // Retain precisely the state values this drawing consumes. Late freezes
    // may change the live records; the screen model uses these retained words.
    for (Size processor{0U}; processor < g_processors; ++processor)
    {
        evidence.DrawStates[processor] = AtomicUInt64Reference{evidence.Processors[processor].Stop.State}.LoadAcquire();
    }
    screen.Clear();
    const auto& record = evidence.Processors[cpu];
    const auto& snapshot = record.Snapshot;
    Kernel::Diagnostics::StopScreenHeading(screen, record, cpu,
                                           [](const bool ap, const Foundation::UInt64 stage) noexcept
                                           {
                                               return Amd64StageNameFor(ap, stage);
                                           });
    screen.Text("\nVector ");
    screen.Hex(snapshot.Vector);
    screen.Text("  Error ");
    screen.Hex(snapshot.Error);
    screen.Text("\nRIP ");
    screen.Hex(snapshot.Rip);
    screen.Text("  CS ");
    screen.Hex(snapshot.Cs);
    screen.Text("\nRFLAGS ");
    screen.Hex(snapshot.Rflags);
    screen.Text("\nRSP ");
    screen.Hex(snapshot.Rsp);
    screen.Text("\nCR2 ");
    screen.Hex(snapshot.Cr2);
    screen.Text("  CR3 ");
    screen.Hex(snapshot.Cr3);
    Kernel::Diagnostics::StopScreenFooter(screen, evidence, g_processors, "TSC", ASTERIA_STOP_BUILD_IDENTITY,
                                          &GetAmd64EvidenceRecord().Watchdog);
}
void Hex(const UInt64 value) noexcept
{
    char digits[19U]{'0', 'x'};
    constexpr char alphabet[]{"0123456789ABCDEF"};
    for (Size i{0U}; i < 16U; ++i)
    {
        digits[i + 2U] = alphabet[(value >> ((15U - i) * 4U)) & 15U];
    }
    Firmware::WriteDiagnosticAscii(digits);
}
void Emit(const UInt64 cpu, const Amd64ProcessorStopRecord& record) noexcept
{
    UInt64 expected{0U};
    if (!g_writer.CompareExchangeStrongAcquireRelease(expected, 1U))
    {
        return;
    }
    Firmware::WriteDiagnosticAscii("\r\nASTERIA:STOP:CPU=");
    // The profile admits exactly four slots.
    const char number[2U]{static_cast<char>('0' + cpu), '\0'};
    Firmware::WriteDiagnosticAscii(number);
    Firmware::WriteDiagnosticAscii(":CODE=");
    Hex(record.Stop.Code);
    Firmware::WriteDiagnosticAscii(":");
    Firmware::WriteDiagnosticAscii(Kernel::Diagnostics::StopName(static_cast<StopCode>(record.Stop.Code)));
    Firmware::WriteDiagnosticAscii(":P=");
    for (Size i{0U}; i < 4U; ++i)
    {
        if (i != 0U)
        {
            Firmware::WriteDiagnosticAscii(",");
        }
        Hex(record.Stop.Parameters[i]);
    }
    Firmware::WriteDiagnosticAscii("\r\nASTERIA:STOP-SNAPSHOT:CPU=");
    Firmware::WriteDiagnosticAscii(number);
    const auto& snapshot = record.Snapshot;
    Firmware::WriteDiagnosticAscii(":VECTOR=");
    Hex(snapshot.Vector);
    Firmware::WriteDiagnosticAscii(":EC=");
    Hex(snapshot.Error);
    Firmware::WriteDiagnosticAscii(":RIP=");
    Hex(snapshot.Rip);
    Firmware::WriteDiagnosticAscii(":CS=");
    Hex(snapshot.Cs);
    Firmware::WriteDiagnosticAscii(":RFLAGS=");
    Hex(snapshot.Rflags);
    Firmware::WriteDiagnosticAscii(":RSP=");
    Hex(snapshot.Rsp);
    Firmware::WriteDiagnosticAscii(":CR2=");
    Hex(snapshot.Cr2);
    Firmware::WriteDiagnosticAscii(":CR3=");
    Hex(snapshot.Cr3);
    Firmware::WriteDiagnosticAscii("\r\n");
    Firmware::WriteDiagnosticAscii("ASTERIA:STOP-COMPLETE\r\n");
    g_writer.StoreRelease(0U);
}
#endif
} // namespace

namespace
{
class Power final : public ::Asteria::Platform::Firmware::AcpiPowerControl
{
    [[nodiscard]] bool ParkOthers() noexcept override
    {
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("cli" ::: "memory");
        const UInt64 cpu = CurrentProcessor();
        if (cpu != 0U || cpu >= g_processors)
        {
            return false;
        }
        const UInt64 targets = g_online.LoadAcquire() & ~(UInt64{1U} << cpu);
        Synchronization::BeginProcessorPowerPark();
        g_powerRequested.StoreRelease(1U);
        SendFreeze(cpu, targets);
        for (Size poll{0U}; poll < FreezePolls; ++poll)
        {
            if ((g_powerConfirmed.LoadAcquire() & targets) == targets)
            {
                return true;
            }
            __asm__ __volatile__("pause" ::: "memory");
        }
#endif
        return false;
    }
    [[nodiscard]] Foundation::UIntPointer Map(const Boot::Acpi::PowerRegister& reg) noexcept
    {
#if defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__("mov %0,%%cr3" ::"r"(g_root) : "memory");
        Table(g_root)[256U] = g_tables | 3U;
        Table(g_tables + 2U * PageBytes)[0U] =
            (reg.Address & ~(PageBytes - 1U)) | Amd64StopScanoutAttributes(Kernel::Memory::MemoryCachePolicy::Uncached);
        __asm__ __volatile__("mov %0,%%cr3" ::"r"(g_root) : "memory");
        return ApertureBase + (reg.Address & (PageBytes - 1U));
#else
        return reg.Address;
#endif
    }
    [[nodiscard]] Foundation::UInt32 ReadMemory(const Boot::Acpi::PowerRegister& reg) noexcept override
    {
        return *reinterpret_cast<const volatile Foundation::UInt16*>(Map(reg));
    }
    void WriteMemory(const Boot::Acpi::PowerRegister& reg, const Foundation::UInt32 value) noexcept override
    {
        const auto address = Map(reg);
        if (reg.Width == 1U)
        {
            *reinterpret_cast<volatile Foundation::UInt8*>(address) = static_cast<Foundation::UInt8>(value);
        }
        else
        {
            *reinterpret_cast<volatile Foundation::UInt16*>(address) = static_cast<Foundation::UInt16>(value);
        }
        Synchronization::OrderScanoutStorage();
    }
    [[noreturn]] void Returned(const ::Asteria::Platform::Firmware::PowerResult result) noexcept override
    {
        if (result != ::Asteria::Platform::Firmware::PowerResult::Issued)
        {
            ::Asteria::Platform::Firmware::WriteDiagnosticAscii(
                result == ::Asteria::Platform::Firmware::PowerResult::CpuUnconfirmed
                    ? "ASTERIA:POWER:CPU-UNCONFIRMED\n"
                    : "ASTERIA:POWER:ACPI-UNCONFIRMED\n");
        }
#if defined(ASTERIA_TARGET_AMD64)
        // Only atomic memory stores, then no further I/O (ACPI 6.5 §4.8.3.6).
        static_cast<void>(Synchronization::MarkProcessorParkedForPower(Kernel::Scheduler::CpuId{0U}));
        for (;;)
        {
            __asm__ __volatile__("pause" ::: "memory");
        }
#else
        __builtin_trap();
#endif
    }
};
constinit Power g_power{};
} // namespace
Hal::IPowerControl& ConfigureAmd64Power(const Boot::Acpi::PowerRegisters& facts,
                                        Hal::Device::IPortIo* const ports) noexcept
{
    g_power.Configure(facts, ports);
    return g_power;
}

bool PrepareAmd64Stop(const Boot::BootDisplay& display, const UInt64 root, const UInt64 tables, const Size tablePages,
                      const UInt32* const hardwareIds, const Size processors, const UInt64 localApicAddress) noexcept
{
    if (processors == 0U || processors > 4U || hardwareIds == nullptr || tablePages < 3U || tablePages > 514U)
    {
        return false;
    }
    for (Size cpu{0U}; cpu < processors; ++cpu)
    {
        g_hardwareIds[cpu] = hardwareIds[cpu];
    }
    g_processors = processors;
    g_apic = localApicAddress;
    g_root = root;
    g_tables = tables;
    g_display = display;
#if defined(ASTERIA_TARGET_AMD64)
    g_startTicks = Ticks();
    for (Size word{0U}; word < tablePages * 512U; ++word)
    {
        Table(tables)[word] = 0U;
    }
    // The aperture has its own PML4 branch and no framebuffer leaves yet.
    if (Table(root)[256U] != 0U)
    {
        return false;
    }
    Table(tables)[0U] = (tables + PageBytes) | 3U;
    for (Size pt{0U}; pt < tablePages - 2U; ++pt)
    {
        Table(tables + PageBytes)[pt] = (tables + (pt + 2U) * PageBytes) | 3U;
    }
#endif
    g_frameBytes = Kernel::Diagnostics::StopDisplayBytes(display, (tablePages - 2U) * 512U * PageBytes);
    g_online.StoreRelease(1U);
    return true;
}

void SetAmd64StopScanoutRegion(Kernel::Device::DeviceRegion& region) noexcept
{
    g_scanoutRegion = &region;
}

void Amd64StopProcessorOnline(const UInt64 processor) noexcept
{
    if (processor < 4U)
    {
#if defined(ASTERIA_TARGET_AMD64)
        if (CurrentProcessor() != processor)
        {
            Park();
        }
        if (g_latch.Owner() != 0U)
        {
            auto& record = GetAmd64EvidenceRecord().Stops.Processors[processor];
            Capture(record.Snapshot, nullptr);
            Synchronization::OrderScanoutStorage();
            Kernel::Diagnostics::FreezeStop(record.Stop);
            Park();
        }
        // Firmware/external NMIs (including QEMU HMP nmi) enter through LINT1.
        // Timer initialization masked it; enable NMI delivery only once the
        // CPU's private IDT and IST2 are installed.
        Interrupt::Amd64LocalApic apic;
        if (!apic.Initialize(g_apic))
        {
            Park();
        }
        apic.Write(0x360U, 4U << 8U);
#endif
        // APs publish after installing their private descriptor tables/IST2.
        UInt64 expected = g_online.LoadAcquire();
        while (!g_online.CompareExchangeStrongAcquireRelease(expected, expected | (UInt64{1U} << processor)))
        {
        }
#if defined(ASTERIA_TARGET_AMD64)
        // Close the online-publication race with a winner taking its mask.
        if (g_latch.Owner() != 0U)
        {
            auto& record = GetAmd64EvidenceRecord().Stops.Processors[processor];
            Capture(record.Snapshot, nullptr);
            Synchronization::OrderScanoutStorage();
            Kernel::Diagnostics::FreezeStop(record.Stop);
            Park();
        }
#endif
    }
}

[[noreturn]] void StopAmd64(const StopCode code, const UInt64 p1, const UInt64 p2, const UInt64 p3, const UInt64 p4,
                            const Interrupt::Amd64TrapFrame* const frame) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("cli" : : : "memory");
    const UInt64 cpu = CurrentProcessor();
    if (cpu == UnknownProcessor)
    {
        Park();
    }
    auto& evidence = GetAmd64EvidenceRecord().Stops;
    auto& record = evidence.Processors[cpu];
    const UInt64 state = AtomicUInt64Reference{record.Stop.State}.LoadAcquire();
    if (state == static_cast<UInt64>(StopState::Running))
    {
        Capture(record.Snapshot, frame);
        static_cast<void>(Kernel::Diagnostics::BeginStop(record.Stop, code, p1, p2, p3, p4));
    }
    else if (state != static_cast<UInt64>(StopState::Stopping) || g_latch.Owner() != 0U)
    {
        Park();
    }
    const bool winner = g_latch.TryLatch(cpu);
    Synchronization::OrderScanoutStorage();
    Kernel::Diagnostics::FreezeStop(record.Stop);
    if (!winner)
    {
        Park();
    }
    evidence.Winner = cpu;
    evidence.OnlineMask = g_online.LoadAcquire() | (UInt64{1U} << cpu);
    // TSC subtraction intentionally uses modulo-2^64 counter arithmetic.
    evidence.UptimeTicks = Ticks() - g_startTicks;
    if (code == StopCode::Lockup)
    {
        SendFreeze(cpu, p1);
    }
    SendFreeze(cpu, evidence.OnlineMask);
    UInt64 confirmed{0U};
    for (Size poll{0U}; poll < FreezePolls; ++poll)
    {
        confirmed = 0U;
        for (Size processor{0U}; processor < g_processors; ++processor)
        {
            if (AtomicUInt64Reference{evidence.Processors[processor].Stop.State}.LoadAcquire() ==
                static_cast<UInt64>(StopState::Frozen))
            {
                confirmed |= UInt64{1U} << processor;
            }
        }
        if ((confirmed & evidence.OnlineMask) == evidence.OnlineMask)
        {
            break;
        }
        __asm__ __volatile__("pause");
    }
    evidence.ConfirmedMask = confirmed & evidence.OnlineMask;
    evidence.UnconfirmedMask = evidence.OnlineMask & ~confirmed;
    // Only after the latch/bound may the winner change a peer's state. The
    // bounded CAS never replaces Frozen or writes a peer's payload/snapshot.
    for (Size processor{0U}; processor < g_processors; ++processor)
    {
        if ((evidence.UnconfirmedMask & (UInt64{1U} << processor)) != 0U)
        {
            Kernel::Diagnostics::MarkStopUnconfirmed(evidence.Processors[processor].Stop);
        }
    }
    auto& watchdog = GetAmd64EvidenceRecord().Watchdog;
    Synchronization::CaptureProcessorWatchdog(watchdog, cpu, code == StopCode::Lockup ? p1 : 0U);
    for (Size peer{0U}; peer < g_processors; ++peer)
    {
        if ((evidence.ConfirmedMask & (UInt64{1U} << peer)) != 0U)
        {
            watchdog.Processors[peer].Location = evidence.Processors[peer].Snapshot.Rip;
            watchdog.Processors[peer].Captured = 1U;
        }
    }
    PublishAmd64Evidence(watchdog.Published);
    evidence.ApertureRoot = g_root;
    evidence.ApertureBase = ApertureBase;
    evidence.ApertureBytes = g_frameBytes;
    // Publish evidence and serial completion before any framebuffer access.
    // Draw separately retains the CPU states used by the screen model.
    PublishAmd64Evidence(evidence.Published);
    Emit(cpu, record);
    Draw(cpu, evidence);
    Synchronization::OrderScanoutStorage();
    PublishAmd64Evidence(evidence.DrawPublished);
    Park();
#else
    static_cast<void>(code);
    static_cast<void>(p1);
    static_cast<void>(p2);
    static_cast<void>(p3);
    static_cast<void>(p4);
    static_cast<void>(frame);
    __builtin_trap();
#endif
}

[[noreturn]] void StopAmd64Trap(const Interrupt::Amd64TrapFrame& frame) noexcept
{
    UInt64 address{0U};
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ __volatile__("mov %%cr2,%0" : "=r"(address));
#endif
    StopAmd64(StopCode::KernelTrap, frame.Vector, address, frame.Error, 0U, &frame);
}

void HandleAmd64Nmi(const Interrupt::Amd64TrapFrame& frame) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    const UInt64 cpu = CurrentProcessor();
    if (cpu == UnknownProcessor)
    {
        Park();
    }
    if (g_powerRequested.LoadAcquire() != 0U)
    {
        if (!Synchronization::MarkProcessorParkedForPower(
                Kernel::Scheduler::CpuId{static_cast<Foundation::UInt8>(cpu)}))
        {
            Park();
        }
        Synchronization::OrderScanoutStorage();
        UInt64 confirmed = g_powerConfirmed.LoadAcquire();
        for (Size attempt{0U}; attempt < g_processors; ++attempt)
        {
            if (g_powerConfirmed.CompareExchangeStrongAcquireRelease(confirmed, confirmed | (UInt64{1U} << cpu)))
            {
                break;
            }
        }
        Park();
    }
    const UInt64 owner = g_latch.Owner();
    if (owner == cpu + 1U)
    {
        return;
    }
    if (owner == 0U)
    {
        Foundation::UInt8 port61{0U};
        __asm__ __volatile__("inb %1,%0" : "=a"(port61) : "Nd"(Foundation::UInt16{0x61U}) : "memory");
        StopAmd64(StopCode::UnexpectedNmi, port61, 0U, 0U, 0U, &frame);
    }
    auto& record = GetAmd64EvidenceRecord().Stops.Processors[cpu];
    const UInt64 state = AtomicUInt64Reference{record.Stop.State}.LoadAcquire();
    if ((state == static_cast<UInt64>(StopState::Running)) || (state == static_cast<UInt64>(StopState::Unconfirmed)))
    {
        Capture(record.Snapshot, &frame);
    }
    Synchronization::OrderScanoutStorage();
    Kernel::Diagnostics::FreezeStop(record.Stop);
    Park();
#else
    static_cast<void>(frame);
    __builtin_trap();
#endif
}

void ObserveAmd64StopPoint(const Foundation::UInt8* const text, const Size bytes) noexcept
{
#if ASTERIA_AMD64_STOP_PROBE == 1 || ASTERIA_AMD64_STOP_PROBE == 3
    constexpr char point[]{"# display: stopped"};
    if (bytes != sizeof(point) - 1U)
    {
        return;
    }
    for (Size i{0U}; i < bytes; ++i)
    {
        if (text[i] != static_cast<Foundation::UInt8>(point[i]))
        {
            return;
        }
    }
    g_injectionPending = true;
#else
    static_cast<void>(text);
    static_cast<void>(bytes);
#endif
}
void CheckAmd64StopPoint() noexcept
{
    // Ordinary syscall boundary: all diagnostic channel borrows have ended.
    if (g_injectionPending)
    {
#if ASTERIA_AMD64_STOP_PROBE == 3 && defined(ASTERIA_TARGET_AMD64)
        __asm__ __volatile__(
            "cli; .globl AsteriaAmd64WatchdogStallBegin; AsteriaAmd64WatchdogStallBegin: pause; jmp AsteriaAmd64WatchdogStallBegin; .globl AsteriaAmd64WatchdogStallEnd; AsteriaAmd64WatchdogStallEnd:" ::
                : "memory");
        __builtin_unreachable();
#else
        StopAmd64(StopCode::TestInjected, 1U);
#endif
    }
}
void CheckAmd64EarlyStopPoint() noexcept
{
#if ASTERIA_AMD64_STOP_PROBE == 2
    // Named BeforeStartProcessors point: no AP has installed our IDT yet.
    StopAmd64(StopCode::TestInjected, 2U);
#endif
}
void RecordAmd64StrayInterrupt() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    const UInt64 cpu = CurrentProcessor();
    if (cpu == UnknownProcessor)
    {
        Park();
    }
    // Owning CPU, interrupt gate has IF clear; no cross-CPU counter writer.
    ++GetAmd64EvidenceRecord().Stops.StrayInterrupts[cpu];
#endif
}
} // namespace Asteria::Platform::Amd64::Diagnostics
