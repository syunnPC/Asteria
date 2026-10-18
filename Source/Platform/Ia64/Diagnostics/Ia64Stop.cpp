// SPDX-License-Identifier: Apache-2.0

#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Platform/Firmware/AcpiPowerControl.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Kernel/Scheduler/CrossProcessorWaitBudget.hpp"

#include "Kernel/Diagnostics/StopDisplay.hpp"
#include "Kernel/Diagnostics/StopFont.hpp"
#include "Kernel/Diagnostics/StopScreenLayout.hpp"
#include "Kernel/Diagnostics/StopTextWriter.hpp"
#include "Platform/Firmware/Diagnostic.hpp"
#include "Platform/Ia64/Diagnostics/Ia64EvidenceRecord.hpp"
#include "Platform/Ia64/Diagnostics/Ia64StopStage.hpp"
#include "Platform/Ia64/Firmware/Ia64DiagnosticWriter.hpp"
#include "Platform/Ia64/Memory/Ia64RuntimeInterruptVectorTable.hpp"

#if __has_include("Generated/StopBuildIdentity.hpp")
#include "Generated/StopBuildIdentity.hpp"
#else
#define ASTERIA_STOP_BUILD_IDENTITY "host-analysis"
#endif
#if !defined(ASTERIA_IA64_STOP_PROBE)
#define ASTERIA_IA64_STOP_PROBE 0
#endif

#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
extern "C" Asteria::Foundation::UInt64 AsteriaIa64ReadPhysicalPort(Asteria::Foundation::UInt64,
                                                                   Asteria::Foundation::UInt64) noexcept;
extern "C" void AsteriaIa64WritePhysicalPort(Asteria::Foundation::UInt64, Asteria::Foundation::UInt64,
                                             Asteria::Foundation::UInt64) noexcept;
extern "C" [[noreturn]] void AsteriaIa64SendPowerInit(Asteria::Foundation::UInt64) noexcept;
extern "C" void
AsteriaIa64CaptureStopSnapshot(Asteria::Platform::Ia64::Diagnostics::Ia64StopSnapshot* snapshot) noexcept;
extern "C" void AsteriaIa64FillStopPhysical(Asteria::Foundation::UInt64 address, Asteria::Foundation::UInt64 width,
                                            Asteria::Foundation::UInt64 height, Asteria::Foundation::UInt64 pitchBytes,
                                            Asteria::Foundation::UInt64 pixel) noexcept;
extern "C" void AsteriaIa64DrawStopGlyphPhysical(Asteria::Foundation::UInt64 address,
                                                 Asteria::Foundation::UInt64 pitchBytes,
                                                 Asteria::Foundation::UInt64 rowsLow,
                                                 Asteria::Foundation::UInt64 rowsHigh,
                                                 Asteria::Foundation::UInt64 foreground,
                                                 Asteria::Foundation::UInt64 background) noexcept;
extern "C" [[noreturn]] void AsteriaIa64StopPark() noexcept;
#endif

namespace Asteria::Platform::Ia64::Diagnostics
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;
using Kernel::Diagnostics::StopCode;
using Kernel::Diagnostics::StopState;
using Synchronization::AtomicUInt64Reference;
constinit Ia64StopStorage* g_storage{nullptr};
constinit bool g_injectionPending{false};

[[nodiscard]] UInt64 Ticks() noexcept
{
    UInt64 ticks{0U};
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    __asm__ volatile("mov %0 = ar.itc" : "=r"(ticks));
#endif
    return ticks;
}
[[nodiscard]] UInt64 CurrentProcessor() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    UInt64 address{0U};
    __asm__ volatile("mov %0 = ar.k3" : "=r"(address));
    if (address != 0U)
    {
        const auto* context = reinterpret_cast<const Memory::Ia64RuntimeInterruptVectorTableContext*>(address);
        if (context->StopStorageAddress == reinterpret_cast<Foundation::UIntPointer>(g_storage))
        {
            return context->ProcessorIndex;
        }
    }
    return 4U;
#else
    return 0U;
#endif
}
[[noreturn]] void Park() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64StopPark();
#else
    __builtin_trap();
#endif
}
void Capture(Ia64StopSnapshot& snapshot) noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    AsteriaIa64CaptureStopSnapshot(&snapshot);
#else
    snapshot = {};
#endif
}
void DisableInterrupts() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    __asm__ volatile("rsm psr.i;; srlz.d;;" ::: "memory");
#endif
}
void Pause() noexcept
{
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
    __asm__ volatile("hint @pause" ::: "memory");
#endif
}
class PhysicalScreen final : public Kernel::Diagnostics::StopTextWriter<PhysicalScreen>
{
public:
    explicit PhysicalScreen(const Boot::BootDisplay& display) noexcept
        : m_display{display}, m_background{display.Format == Boot::BootDisplayFormatRgbx ? 0x00A68C00U : 0x00008CA6U}
    {
    }
    void Clear() noexcept
    {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        AsteriaIa64FillStopPhysical(m_display.FrameBufferBase, m_display.Width, m_display.Height,
                                    UInt64{m_display.PixelsPerScanLine} * 4U, m_background);
#endif
    }
    void Put(const UInt8 code) noexcept
    {
        if (code == '\n')
        {
            m_column = 0U;
            ++m_row;
            return;
        }
        if (m_column < m_display.Width / 8U && m_row < m_display.Height / 16U && code >= 32U && code < 127U)
        {
            // Read the virtual font before entering the physical leaf. Only
            // the packed sixteen rows and pixel values cross that boundary.
            UInt64 low{0U};
            UInt64 high{0U};
            for (Size row{0U}; row < 8U; ++row)
            {
                low |= UInt64{Kernel::Diagnostics::StopGlyphs[(code - 32U) * 16U + row]} << (row * 8U);
                high |= UInt64{Kernel::Diagnostics::StopGlyphs[(code - 32U) * 16U + row + 8U]} << (row * 8U);
            }
            const UInt64 pixel = UInt64{m_row} * 16U * m_display.PixelsPerScanLine + m_column * 8U;
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
            AsteriaIa64DrawStopGlyphPhysical(m_display.FrameBufferBase + pixel * 4U,
                                             UInt64{m_display.PixelsPerScanLine} * 4U, low, high, 0x00FFFFFFU,
                                             m_background);
#else
            static_cast<void>(pixel);
            static_cast<void>(low);
            static_cast<void>(high);
#endif
        }
        ++m_column;
    }

private:
    const Boot::BootDisplay& m_display;
    UInt32 m_background;
    UInt32 m_column{0U};
    UInt32 m_row{0U};
};
void Draw(Ia64StopStorage& storage) noexcept
{
    auto& evidence = storage.Evidence;
    const auto& display = storage.Display;
    if (Kernel::Diagnostics::StopDisplayBytes(display, (UInt64{1U} << 63U) - 1U) == 0U ||
        display.FrameBufferBase >= (UInt64{1U} << 63U) ||
        display.FrameBufferBytes > (UInt64{1U} << 63U) - display.FrameBufferBase)
    {
        return;
    }
    PhysicalScreen screen{display};
    screen.Clear();
    const auto& record = evidence.Processors[evidence.Winner];
    const auto& snapshot = record.Snapshot;
    Kernel::Diagnostics::StopScreenHeading(screen, record, evidence.Winner,
                                           [](const bool ap, const Foundation::UInt64 stage) noexcept
                                           {
                                               return Ia64StageNameFor(ap, stage);
                                           });
    screen.Text("\nVector ");
    screen.Hex(snapshot.VectorOffset);
    screen.Text("  ISR ");
    screen.Hex(snapshot.Isr);
    screen.Text("\nIIP ");
    screen.Hex(snapshot.Iip);
    screen.Text("  IPSR ");
    screen.Hex(snapshot.Ipsr);
    screen.Text("\nIFA ");
    screen.Hex(snapshot.Ifa);
    screen.Text("  IIM ");
    screen.Hex(snapshot.Iim);
    screen.Text("\nIIPA ");
    screen.Hex(snapshot.Iipa);
    screen.Text("  b0 ");
    screen.Hex(snapshot.B0);
    screen.Text("\nRR ");
    screen.Hex(snapshot.RegionRegister);
    screen.Text("  RID ");
    screen.Hex(snapshot.Rid);
    Kernel::Diagnostics::StopScreenFooter(screen, evidence, storage.ProcessorCount, "ITC", ASTERIA_STOP_BUILD_IDENTITY,
                                          &GetIa64EvidenceRecord().Watchdog);
}
void Hex(const UInt64 value) noexcept
{
    char digits[19U]{'0', 'x'};
    constexpr char alphabet[]{"0123456789ABCDEF"};
    for (Size i{0U}; i < 16U; ++i)
    {
        digits[i + 2U] = alphabet[(value >> ((15U - i) * 4U)) & 15U];
    }
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(digits);
}
void Emit(const UInt64 cpu, const Ia64ProcessorStopRecord& record) noexcept
{
    // Shares exclusion with ordinary debug writers; exactly one attempt,
    // including when an unconfirmed peer still owns the serial channel.
    if (!Platform::Firmware::TryBeginIa64TerminalDiagnostic())
    {
        return;
    }
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii("\r\nASTERIA:STOP:CPU=");
    const char number[2U]{static_cast<char>('0' + cpu), '\0'};
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(number);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":CODE=");
    Hex(record.Stop.Code);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":");
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(
        Kernel::Diagnostics::StopName(static_cast<StopCode>(record.Stop.Code)));
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":P=");
    for (Size i{0U}; i < 4U; ++i)
    {
        if (i != 0U)
        {
            Platform::Firmware::WriteIa64TerminalDiagnosticAscii(",");
        }
        Hex(record.Stop.Parameters[i]);
    }
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii("\r\nASTERIA:STOP-SNAPSHOT:CPU=");
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(number);
    const auto& snapshot = record.Snapshot;
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":VECTOR=");
    Hex(snapshot.VectorOffset);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":IIP=");
    Hex(snapshot.Iip);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":IPSR=");
    Hex(snapshot.Ipsr);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":ISR=");
    Hex(snapshot.Isr);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":IFA=");
    Hex(snapshot.Ifa);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":IIM=");
    Hex(snapshot.Iim);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":IIPA=");
    Hex(snapshot.Iipa);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":RR=");
    Hex(snapshot.RegionRegister);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":RID=");
    Hex(snapshot.Rid);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii(":B0=");
    Hex(snapshot.B0);
    Platform::Firmware::WriteIa64TerminalDiagnosticAscii("\r\nASTERIA:STOP-COMPLETE\r\n");
    Platform::Firmware::EndIa64TerminalDiagnostic();
}
[[noreturn]] void FinishStop(Ia64StopStorage& storage, const UInt64 cpu) noexcept
{
    auto& evidence = storage.Evidence;
    auto& record = evidence.Processors[cpu];
    UInt64 expected{0U};
    const bool winner = AtomicUInt64Reference{storage.Latch}.CompareExchangeStrongAcquireRelease(expected, cpu + 1U);
    Kernel::Diagnostics::FreezeStop(record.Stop);
    if (!winner)
    {
        Park();
    }
    evidence.Winner = cpu;
    evidence.OnlineMask = AtomicUInt64Reference{storage.OnlineMask}.LoadAcquire() | (UInt64{1U} << cpu);
    // Hardware ITC wraparound intentionally uses unsigned counter arithmetic.
    const UInt64 start = Ticks();
    evidence.UptimeTicks = start - storage.StartTicks;
    UInt64 confirmed{0U};
    do
    {
        confirmed = 0U;
        for (Size processor{0U}; processor < storage.ProcessorCount; ++processor)
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
        Pause();
    } while (Ticks() - start < Ia64StopFreezeTicks);
    evidence.ConfirmedMask = confirmed & evidence.OnlineMask;
    evidence.UnconfirmedMask = evidence.OnlineMask & ~confirmed;
    for (Size processor{0U}; processor < storage.ProcessorCount; ++processor)
    {
        if ((evidence.UnconfirmedMask & (UInt64{1U} << processor)) != 0U)
        {
            Kernel::Diagnostics::MarkStopUnconfirmed(evidence.Processors[processor].Stop);
        }
        evidence.DrawStates[processor] = AtomicUInt64Reference{evidence.Processors[processor].Stop.State}.LoadAcquire();
    }
    evidence.RuntimeAddress = reinterpret_cast<Foundation::UIntPointer>(&storage);
    evidence.FrameBufferBase = storage.Display.FrameBufferBase;
    evidence.FrameBufferBytes = Kernel::Diagnostics::StopDisplayBytes(storage.Display, (UInt64{1U} << 63U) - 1U);
    auto& published = GetIa64EvidenceRecord().Stops;
    // Only acquire-confirmed slots have readable payloads. Unconfirmed peers
    // may still be writing; copying their snapshot would be a data race.
    published.Winner = evidence.Winner;
    published.OnlineMask = evidence.OnlineMask;
    published.ConfirmedMask = evidence.ConfirmedMask;
    published.UnconfirmedMask = evidence.UnconfirmedMask;
    published.UptimeTicks = evidence.UptimeTicks;
    published.RuntimeAddress = evidence.RuntimeAddress;
    published.FrameBufferBase = evidence.FrameBufferBase;
    published.FrameBufferBytes = evidence.FrameBufferBytes;
    for (Size processor{0U}; processor < storage.ProcessorCount; ++processor)
    {
        if ((confirmed & (UInt64{1U} << processor)) != 0U)
        {
            published.Processors[processor] = evidence.Processors[processor];
        }
        else
        {
            published.Processors[processor].Stop.State = evidence.DrawStates[processor];
        }
        published.DrawStates[processor] = evidence.DrawStates[processor];
    }
    auto& watchdog = GetIa64EvidenceRecord().Watchdog;
    Synchronization::CaptureProcessorWatchdog(
        watchdog, cpu, record.Stop.Code == static_cast<UInt32>(StopCode::Lockup) ? record.Stop.Parameters[0U] : 0U);
    for (Size peer{0U}; peer < storage.ProcessorCount; ++peer)
    {
        if ((evidence.ConfirmedMask & (UInt64{1U} << peer)) != 0U)
        {
            watchdog.Processors[peer].Location = evidence.Processors[peer].Snapshot.Iip;
            watchdog.Processors[peer].Captured = 1U;
        }
    }
    PublishIa64Evidence(watchdog.Published);
    PublishIa64Evidence(published.Published);
    Emit(cpu, record);
    Draw(storage);
    PublishIa64Evidence(published.DrawPublished);
    Park();
}
} // Namespace

namespace
{
class Power final : public ::Asteria::Platform::Firmware::AcpiPowerControl
{
public:
    void ConfigureSal(const Boot::BootSalInformation* sal, const Boot::Acpi::AcpiTopology* topology) noexcept
    {
        m_pib = 0U;
        // SAL 2.9/3.0 §5.1 / processor stop policy: this kernel registers OS_BOOT_RENDEZ only.
        // Registering OS_INIT must revoke this fallback.
        if (sal != nullptr && topology != nullptr && Ia64PowerSalRevision(sal->SalRevision) &&
            sal->SalProcedurePhysicalAddress != 0U && sal->PalProcedurePhysicalAddress != 0U &&
            topology->LocalControllerPhysicalAddress != 0U &&
            (topology->LocalControllerPhysicalAddress & ((2U * 1024U * 1024U) - 1U)) == 0U &&
            topology->LocalControllerPhysicalAddress < (UInt64{1U} << 47U))
        {
            m_pib = topology->LocalControllerPhysicalAddress;
        }
    }
    [[nodiscard]] UInt64 Available() const noexcept override
    {
        return AcpiPowerControl::Available() | (m_pib != 0U ? Abi::DeviceRebootAvailable : 0U);
    }
    [[nodiscard]] bool Transition(const bool reboot) noexcept override
    {
        if (!reboot || (AcpiPowerControl::Available() & Abi::DeviceRebootAvailable) != 0U || m_pib == 0U)
        {
            return AcpiPowerControl::Transition(reboot);
        }
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        UInt64 psr{0U};
        __asm__ __volatile__("mov %0 = psr" : "=r"(psr));
        if ((psr & (UInt64{1U} << 35U)) != 0U)
        {
            return false;
        }
        if (!ParkOthers())
        {
            Returned(::Asteria::Platform::Firmware::PowerResult::CpuUnconfirmed);
        }
        UInt64 lid{0U};
        __asm__ __volatile__("mov %0 = cr.lid" : "=r"(lid));
        AsteriaIa64SendPowerInit(Ia64PowerInitAddress(m_pib, lid));
#else
        return false;
#endif
    }

private:
    UInt64 m_pib{0U};
    [[nodiscard]] bool ParkOthers() noexcept override
    {
        DisableInterrupts();
        const UInt64 cpu = CurrentProcessor();
        if (g_storage == nullptr || cpu != 0U || cpu >= g_storage->ProcessorCount)
        {
            return false;
        }
        const UInt64 targets = AtomicUInt64Reference{g_storage->OnlineMask}.LoadAcquire() & ~(UInt64{1U} << cpu);
        Platform::Synchronization::BeginProcessorPowerPark();
        UInt64 expected{0U};
        if (!AtomicUInt64Reference{g_storage->Latch}.CompareExchangeStrongAcquireRelease(expected, Ia64PowerLatch))
        {
            return false;
        }
        UInt64 expectedCount{0U};
        for (Size target{0U}; target < g_storage->ProcessorCount; ++target)
        {
            expectedCount += (targets >> target) & 1U;
        }
        const UInt64 start = Ticks();
        for (Size poll{0U}; poll < 8'000'000U && Ticks() - start < Ia64StopFreezeTicks; ++poll)
        {
            if (AtomicUInt64Reference{g_storage->PowerConfirmed}.LoadAcquire() == expectedCount)
            {
                return true;
            }
            Pause();
        }
        return false;
    }
    [[nodiscard]] Foundation::UInt32 ReadMemory(const Boot::Acpi::PowerRegister& reg) noexcept override
    {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        return static_cast<Foundation::UInt32>(AsteriaIa64ReadPhysicalPort(reg.Address, reg.Width));
#else
        static_cast<void>(reg);
        return 0U;
#endif
    }
    void WriteMemory(const Boot::Acpi::PowerRegister& reg, const Foundation::UInt32 value) noexcept override
    {
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        AsteriaIa64WritePhysicalPort(reg.Address, reg.Width, value);
#else
        static_cast<void>(reg);
        static_cast<void>(value);
#endif
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
        static_cast<void>(Platform::Synchronization::MarkProcessorParkedForPower(Kernel::Scheduler::CpuId{0U}));
        Park();
    }
};
constinit Power g_power{};
} // Namespace
Hal::IPowerControl& ConfigureIa64Power(const Boot::Acpi::PowerRegisters& facts, Hal::Device::IPortIo* const ports,
                                       const Boot::BootSalInformation* const sal,
                                       const Boot::Acpi::AcpiTopology* const topology) noexcept
{
    g_power.Configure(facts, ports);
    g_power.ConfigureSal(sal, topology);
    return g_power;
}

void PrepareIa64Stop(Ia64StopStorage& storage, const Boot::BootDisplay& display, const Size processors) noexcept
{
    storage.PowerConfirmed = 0U;
    storage.Display = display;
    storage.ProcessorCount = processors >= 1U && processors <= 4U ? processors : 1U;
    storage.StartTicks = Ticks();
    storage.OnlineMask = 1U;
    for (Size cpu{0U}; cpu < storage.ProcessorCount; ++cpu)
    {
        storage.Terminal[cpu].StackTop =
            reinterpret_cast<Foundation::UIntPointer>(&storage.MemoryStacks[cpu][Ia64StopMemoryStackSizeBytes]) - 16U;
        storage.Terminal[cpu].RbsBase = reinterpret_cast<Foundation::UIntPointer>(&storage.BackingStores[cpu][0U]);
#if defined(ASTERIA_TARGET_IA64) && defined(__ia64__)
        __asm__ volatile("mov %0 = gp" : "=r"(storage.Terminal[cpu].GlobalPointer));
#endif
    }
    g_storage = &storage;
}
void PollIa64Stop() noexcept
{
    Platform::Synchronization::ProcessorHeartbeat(
        reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0)));
    Platform::Synchronization::PollProcessorWatchdog();
    if (g_storage == nullptr || AtomicUInt64Reference{g_storage->Latch}.LoadAcquire() == 0U)
    {
        return;
    }
    DisableInterrupts();
    const UInt64 cpu = CurrentProcessor();
    if (cpu >= g_storage->ProcessorCount)
    {
        Park();
    }
    if (AtomicUInt64Reference{g_storage->Latch}.LoadAcquire() == Ia64PowerLatch)
    {
        if (Platform::Synchronization::MarkProcessorParkedForPower(Kernel::Scheduler::CpuId{static_cast<UInt8>(cpu)}))
        {
            AtomicUInt64Reference{g_storage->PowerConfirmed}.IncrementRelease();
        }
        Park();
    }
    auto& record = g_storage->Evidence.Processors[cpu];
    Capture(record.Snapshot);
    Kernel::Diagnostics::FreezeStop(record.Stop);
    Park();
}
Foundation::UInt64 Ia64ProgressProcessor() noexcept
{
    return CurrentProcessor();
}
Foundation::UInt64 Ia64ProgressTicks() noexcept
{
    return Ticks();
}
void Ia64StopProcessorOnline() noexcept
{
    if (g_storage == nullptr)
    {
        return;
    }
    PollIa64Stop();
    const UInt64 cpu = CurrentProcessor();
    if (cpu >= g_storage->ProcessorCount)
    {
        Park();
    }
    using Platform::Synchronization::BindProcessorProgress;
    using Platform::Synchronization::ProcessorProgressHooks;
    using Platform::Synchronization::RuntimeProcessorSet;
    BindProcessorProgress(Kernel::Scheduler::CpuId{static_cast<Foundation::UInt8>(cpu)},
                          ProcessorProgressHooks{nullptr, nullptr, nullptr, nullptr, nullptr,
                                                 Kernel::Scheduler::Ia64CrossProcessorMinimumTicks,
                                                 &RuntimeProcessorSet()});
    if (!RuntimeProcessorSet().Admit(Kernel::Scheduler::CpuId{static_cast<Foundation::UInt8>(cpu)}))
    {
        StopIa64(StopCode::InvariantViolated, cpu);
    }
    Platform::Synchronization::ProcessorWatchdogOnline();
    AtomicUInt64Reference{g_storage->OnlineMask}.FetchOrRelease(UInt64{1U} << cpu);
    PollIa64Stop();
}
[[noreturn]] void StopIa64(const StopCode code, const UInt64 p1, const UInt64 p2, const UInt64 p3,
                           const UInt64 p4) noexcept
{
    DisableInterrupts();
    if (g_storage == nullptr)
    {
        Park();
    }
    const UInt64 cpu = CurrentProcessor();
    if (cpu >= g_storage->ProcessorCount)
    {
        Park();
    }
    if (AtomicUInt64Reference{g_storage->Latch}.LoadAcquire() == Ia64PowerLatch)
    {
        if (Platform::Synchronization::MarkProcessorParkedForPower(Kernel::Scheduler::CpuId{static_cast<UInt8>(cpu)}))
        {
            AtomicUInt64Reference{g_storage->PowerConfirmed}.IncrementRelease();
        }
        Park();
    }
    auto& record = g_storage->Evidence.Processors[cpu];
    Capture(record.Snapshot);
    if (!Kernel::Diagnostics::BeginStop(record.Stop, code, p1, p2, p3, p4))
    {
        Park();
    }
    FinishStop(*g_storage, cpu);
}
/// Terminal bank-zero adapter has already captured the owner's snapshot,
/// installed a fresh PL0 stack/RBS and left the interrupt entry corridor.
[[noreturn]] void StopIa64Captured(Ia64StopStorage& storage, const UInt64 cpu) noexcept
{
    if (AtomicUInt64Reference{storage.Latch}.LoadAcquire() == Ia64PowerLatch)
    {
        if (Platform::Synchronization::MarkProcessorParkedForPower(Kernel::Scheduler::CpuId{static_cast<UInt8>(cpu)}))
        {
            AtomicUInt64Reference{storage.PowerConfirmed}.IncrementRelease();
        }
        Park();
    }
    auto& record = storage.Evidence.Processors[cpu];
    if (AtomicUInt64Reference{storage.Latch}.LoadAcquire() != 0U)
    {
        Kernel::Diagnostics::FreezeStop(record.Stop);
        Park();
    }
    static_cast<void>(Kernel::Diagnostics::BeginStop(record.Stop, StopCode::KernelTrap, record.Snapshot.VectorOffset,
                                                     record.Snapshot.Ifa, record.Snapshot.Isr, record.Snapshot.Iim));
    FinishStop(storage, cpu);
}
void ObserveIa64StopPoint(const UInt8* const text, const Size bytes) noexcept
{
#if ASTERIA_IA64_STOP_PROBE == 1 || ASTERIA_IA64_STOP_PROBE == 3
    constexpr char point[]{"# display: stopped"};
    if (bytes != sizeof(point) - 1U)
    {
        return;
    }
    for (Size i{0U}; i < bytes; ++i)
    {
        if (text[i] != static_cast<UInt8>(point[i]))
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
void CheckIa64StopPoint() noexcept
{
    PollIa64Stop();
    if (g_injectionPending)
    {
#if ASTERIA_IA64_STOP_PROBE == 3 && defined(__ia64__)
        __asm__ volatile(
            "rsm psr.i;; srlz.d;; .align 16; .global AsteriaIa64WatchdogStallBegin; AsteriaIa64WatchdogStallBegin: hint @pause;; br.sptk.many AsteriaIa64WatchdogStallBegin;; .global AsteriaIa64WatchdogStallEnd; AsteriaIa64WatchdogStallEnd:" ::
                : "memory");
        __builtin_unreachable();
#else
        StopIa64(StopCode::TestInjected, 1U);
#endif
    }
}
void CheckIa64EarlyStopPoint() noexcept
{
#if ASTERIA_IA64_STOP_PROBE == 2
    StopIa64(StopCode::TestInjected, 2U);
#endif
}
} // Namespace Asteria::Platform::Ia64::Diagnostics

extern "C" [[noreturn]] void
AsteriaIa64StopCaptured(Asteria::Platform::Ia64::Diagnostics::Ia64StopStorage* const storage,
                        const Asteria::Foundation::UInt64 cpu) noexcept
{
    Asteria::Platform::Ia64::Diagnostics::StopIa64Captured(*storage, cpu);
}
