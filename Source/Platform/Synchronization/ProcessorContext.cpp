// SPDX-License-Identifier: Apache-2.0

#include "Platform/Synchronization/ProcessorContext.hpp"
#include "Foundation/Compiler.hpp"
#include "Kernel/Scheduler/ProcessorSet.hpp"
#include "Kernel/Scheduler/CrossProcessorWait.hpp"
#if defined(ASTERIA_TARGET_AMD64)
#include "Platform/Amd64/Diagnostics/Amd64Stop.hpp"
#include "Platform/Amd64/Time/Amd64TscClock.hpp"
#endif
#if defined(ASTERIA_TARGET_IA64)
#include "Platform/Ia64/Diagnostics/Ia64Stop.hpp"
#include "Kernel/Memory/TlbShootdownDrain.hpp"
#endif

#if defined(ASTERIA_TARGET_IA64)
#include "Platform/Ia64/System/Ia64UserThreadTable.hpp"
#endif

namespace Asteria::Platform::Synchronization
{
namespace
{
Kernel::Scheduler::ProcessorSet g_processorSet{};
Kernel::Time::ProcessorClockRendezvous g_clockRendezvous{};
Foundation::UInt64 g_clockFrequency{0U};
Kernel::Diagnostics::Watchdog g_watchdog{};
Foundation::UInt64 g_watchdogFailureAge[Kernel::Scheduler::MaximumProcessorCount]{};
Foundation::UInt64 g_watchdogNextCheck[Kernel::Scheduler::MaximumProcessorCount]{};
bool g_clockOnlineInProgress[Kernel::Scheduler::MaximumProcessorCount]{};
// A timestamp must bracket publication/observation. Intel SDM rev. 093 Vol. 2B, RDTSC Description:
// LFENCE after RDTSC orders subsequent accesses. Itanium SDM Vol. 2 rev. 1.1 §§3.2.2, 13.1.4:
// serialize processor state and order the rendezvous's accesses.
void SerializeOnlineClockSample() noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    __asm__ volatile("lfence" ::: "memory");
#elif defined(ASTERIA_TARGET_IA64)
    __asm__ volatile("mf;; srlz.d;;" ::: "memory");
#endif
}
struct Context
{
    ProcessorProgressHooks Hooks{};
    Foundation::Size Depth{};
    bool Draining{};
    bool ServingRequests{};
    bool SendingKicks{};
    Foundation::ProcessorMask PendingKicks{};
};
#if defined(ASTERIA_TARGET_AMD64) || defined(ASTERIA_TARGET_IA64)
Context g_contexts[Kernel::Scheduler::MaximumProcessorCount]{};
#if !defined(ASTERIA_TARGET_IA64)
Kernel::Scheduler::CpuId (*g_current)() noexcept {};
#endif
[[nodiscard]] Context* Local() noexcept
{
    const auto cpu = CurrentProcessor();
    return cpu.IsValid() ? &g_contexts[cpu.GetValue()] : nullptr;
}
#else
thread_local Context t_hostContext{};
thread_local Kernel::Scheduler::CpuId t_hostProcessor{};
[[nodiscard]] Context* Local() noexcept
{
    return &t_hostContext;
}
#endif
} // Namespace
void InstallCurrentProcessor(Kernel::Scheduler::CpuId (*current)() noexcept) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    __atomic_store_n(&g_current, current, __ATOMIC_RELEASE);
#else
    static_cast<void>(current);
#endif
}
Kernel::Scheduler::CpuId CurrentProcessor() noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    const auto cpu = Ia64::Diagnostics::Ia64ProgressProcessor();
    return cpu < Kernel::Scheduler::MaximumProcessorCount
               ? Kernel::Scheduler::CpuId{static_cast<Foundation::UInt8>(cpu)}
               : Kernel::Scheduler::CpuId{};
#elif defined(ASTERIA_TARGET_AMD64)
    const auto current = __atomic_load_n(&g_current, __ATOMIC_ACQUIRE);
    return current != nullptr ? current() : Kernel::Scheduler::CpuId{};
#else
    return t_hostProcessor;
#endif
}
void BindProcessorProgress(const Kernel::Scheduler::CpuId processor, const ProcessorProgressHooks hooks) noexcept
{
#if defined(ASTERIA_TARGET_AMD64) || defined(ASTERIA_TARGET_IA64)
    if (processor.IsValid())
    {
        g_contexts[processor.GetValue()].Hooks = hooks;
    }
#else
    t_hostProcessor = processor;
    t_hostContext.Hooks = hooks;
#endif
}
#if !defined(ASTERIA_TARGET_AMD64) && !defined(ASTERIA_TARGET_IA64)
void SetHostedProcessor(const Kernel::Scheduler::CpuId processor) noexcept
{
    t_hostProcessor = processor;
}
#endif
void BindProcessorDrain(void* context, void (*drain)(void*) noexcept) noexcept
{
    if (auto* local = Local())
    {
        local->Hooks.Context = context;
        local->Hooks.Drain = drain;
    }
}
Foundation::Size HeldLockDepth() noexcept
{
    const auto* local = Local();
    return local != nullptr ? local->Depth : 0U;
}
void EnterProcessorLock() noexcept
{
    if (auto* local = Local())
    {
        ++local->Depth;
    }
}
void LeaveProcessorLock() noexcept
{
    if (auto* local = Local())
    {
        if (local->Depth == 0U)
        {
            StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, 0U);
        }
        --local->Depth;
    }
}
void DrainProcessorMailbox() noexcept
{
    auto* local = Local();
    // Nested lock spins cannot call a drain: LockOrder forbids that edge.
    // Drain itself is bounded and must not recursively drain its mailbox.
    if (local != nullptr && local->Depth == 0U && !local->Draining &&
        (local->Hooks.Drain != nullptr
#if defined(ASTERIA_TARGET_IA64)
         || local->Hooks.Context != nullptr
#endif
         ))
    {
        local->Draining = true;
#if defined(ASTERIA_TARGET_IA64)
        Kernel::Memory::DrainProcessorShootdowns(local->Hooks.Context);
#else
        local->Hooks.Drain(local->Hooks.Context);
#endif
        local->Draining = false;
    }
}
namespace
{
void* g_requestOwner{};
void (*g_serveRequests)(void*, bool) noexcept {};
void* g_kickOwner{};
void (*g_kick)(void*, Kernel::Scheduler::CpuId) noexcept {};
} // Namespace
void BindThreadRequests(void* owner, void (*serve)(void*, bool) noexcept) noexcept
{
    __atomic_store_n(&g_requestOwner, owner, __ATOMIC_RELEASE);
    __atomic_store_n(&g_serveRequests, serve, __ATOMIC_RELEASE);
}
void BindProcessorKick(void* owner, void (*kick)(void*, Kernel::Scheduler::CpuId) noexcept) noexcept
{
    __atomic_store_n(&g_kickOwner, owner, __ATOMIC_RELEASE);
    __atomic_store_n(&g_kick, kick, __ATOMIC_RELEASE);
}
void QueueProcessorKick(const Kernel::Scheduler::CpuId home) noexcept
{
    if (ProcessorPowerStopping())
    {
        return;
    }
    const auto current = CurrentProcessor();
    if (home.IsValid() && current.IsValid() && home.GetValue() != current.GetValue())
    {
        if (auto* local = Local())
        {
            local->PendingKicks |= Foundation::ProcessorMask{1U} << home.GetValue();
        }
    }
}
void KickProcessor(const Kernel::Scheduler::CpuId home) noexcept
{
    if (ProcessorPowerStopping())
    {
        return;
    }
    if (HeldLockDepth() != 0U)
    {
        StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, HeldLockDepth());
    }
#if defined(ASTERIA_TARGET_IA64)
    // IA-64 polls for posted work; this path sends no IPI.
    static_cast<void>(home);
#else
    const auto kick = __atomic_load_n(&g_kick, __ATOMIC_ACQUIRE);
    if (home.IsValid() && kick != nullptr)
    {
        kick(__atomic_load_n(&g_kickOwner, __ATOMIC_ACQUIRE), home);
    }
#endif
}
void LeaveThreadRequestService() noexcept
{
    if (auto* local = Local())
    {
        local->ServingRequests = false;
    }
}
namespace
{
void FlushProcessorKicks(Context& local) noexcept
{
    if (!local.SendingKicks)
    {
        local.SendingKicks = true;
        const auto pending = local.PendingKicks;
        local.PendingKicks = 0U;
        for (Foundation::UInt8 cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
        {
            if ((pending & (Foundation::ProcessorMask{1U} << cpu)) != 0U)
            {
                KickProcessor(Kernel::Scheduler::CpuId{cpu});
            }
        }
        local.SendingKicks = false;
    }
}
} // Namespace
void ServiceThreadRequests(const bool schedulingPoint) noexcept
{
    if (ProcessorPowerStopping())
    {
        return;
    }
    auto* local = Local();
    if (local == nullptr || local->Depth != 0U || local->ServingRequests)
    {
        return;
    }
    FlushProcessorKicks(*local);
#if defined(ASTERIA_TARGET_IA64)
    const auto owner = __atomic_load_n(&g_requestOwner, __ATOMIC_ACQUIRE);
    if (owner != nullptr)
    {
        local->ServingRequests = true;
        static_cast<Ia64::System::Ia64UserThreadTable*>(owner)->ServiceRequests(schedulingPoint);
        local->ServingRequests = false;
    }
#else
    const auto serve = __atomic_load_n(&g_serveRequests, __ATOMIC_ACQUIRE);
    if (serve != nullptr)
    {
        local->ServingRequests = true;
        serve(__atomic_load_n(&g_requestOwner, __ATOMIC_ACQUIRE), schedulingPoint);
        local->ServingRequests = false;
    }
#endif
}

void ServiceWaitingThreadRequests() noexcept
{
    if (ProcessorPowerStopping())
    {
        return;
    }
#if defined(ASTERIA_TARGET_IA64)
    auto* local = Local();
    if (local == nullptr || local->Depth != 0U || local->ServingRequests)
    {
        return;
    }
    FlushProcessorKicks(*local);
    const auto owner = __atomic_load_n(&g_requestOwner, __ATOMIC_ACQUIRE);
    if (owner != nullptr)
    {
        local->ServingRequests = true;
        static_cast<Ia64::System::Ia64UserThreadTable*>(owner)->ServiceWaitingRequests();
        local->ServingRequests = false;
    }
#else
    ServiceThreadRequests(false);
#endif
}

// Keep this stack-proof boundary distinct from merged cold kick checks.
ASTERIA_NOINLINE_NOIPA void ProcessorWaitProgress() noexcept
{
    // Terminal parking must reach the stop poll without servicing a request
    // whose completion could depend on an already parked owner (PowerOff.md).
    if (!ProcessorPowerStopping())
    {
        ProcessorHeartbeat(reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0)));
#if !defined(NDEBUG)
        if (HeldLockDepth() != 0U)
        {
            StopProcessorWait(Kernel::Diagnostics::StopCode::InvariantViolated, HeldLockDepth());
        }
#endif
        ServiceProcessorClock();
        DrainProcessorMailbox();
        ServiceWaitingThreadRequests();
        PollProcessorWatchdog();
    }
#if defined(ASTERIA_TARGET_IA64)
    Ia64::Diagnostics::PollIa64Stop();
#else
    auto* local = Local();
    if (local != nullptr && local->Hooks.PollStop != nullptr)
    {
        local->Hooks.PollStop(local->Hooks.Context);
    }
#endif
#if defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__ia64__)
    __asm__ __volatile__("hint @pause" ::: "memory");
#endif
}
void ServiceProcessorClock() noexcept
{
    const auto cpu = CurrentProcessor();
    if (!cpu.IsValid() || cpu.GetValue() != 0U)
    {
        return;
    }
    for (Foundation::Size peer{1U}; peer < Kernel::Scheduler::MaximumProcessorCount; ++peer)
    {
        if (g_clockRendezvous.Pending(peer))
        {
            SerializeOnlineClockSample();
            const auto reference = ProcessorWaitTicks();
            SerializeOnlineClockSample();
            g_clockRendezvous.Respond(peer, reference);
        }
    }
}

namespace
{
[[nodiscard]] Kernel::Time::ProcessorClockResidual MeasureClock(Hal::Time::ILocalTimer& timer,
                                                                const Foundation::Size cpu) noexcept
{
    SerializeOnlineClockSample();
    const auto before = timer.GetTicks();
    SerializeOnlineClockSample();
    const auto sequence = g_clockRendezvous.Request(cpu);
    for (Kernel::Scheduler::CrossProcessorWait wait; !g_clockRendezvous.Completed(cpu, sequence);)
    {
        if (!wait.Poll())
        {
            Kernel::Scheduler::CrossProcessorWait::Timeout(1U);
        }
    }
    SerializeOnlineClockSample();
    const auto after = timer.GetTicks();
    SerializeOnlineClockSample();
    return Kernel::Time::ClockResidual(before, g_clockRendezvous.Reference(cpu), after);
}
} // Namespace
#if defined(ASTERIA_TARGET_IA64)
extern "C" void AsteriaIa64WriteOnlineIntervalCounter(Foundation::UInt64 value) noexcept;
#endif
bool SynchronizeOnlineProcessorClock(Hal::Time::ILocalTimer& timer, const Foundation::UInt64 requestedFrequency,
                                     Kernel::Time::ProcessorClockEvidence& evidence) noexcept
{
    const auto processor = CurrentProcessor();
    const auto frequency = requestedFrequency != 0U ? requestedFrequency : g_clockFrequency;
    if (!processor.IsValid() || frequency == 0U)
    {
        return false;
    }
    const auto cpu = processor.GetValue();
    g_clockOnlineInProgress[cpu] = true;
    bool invariant{true};
    bool hypervisor{false};
    bool adjustable{false};
#if defined(ASTERIA_TARGET_AMD64)
    Foundation::UInt32 a{0U};
    Foundation::UInt32 b{0U};
    Foundation::UInt32 c{0U};
    Foundation::UInt32 d{0U};
    // CPUID.1:ECX[31], as specified by contract.
    // Hypervisor Top-Level Functional Specification v6.0b, section 2.2.
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1U), "c"(0U));
    hypervisor = (c & (1U << 31U)) != 0U;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000U), "c"(0U));
    invariant = a >= 0x80000007U;
    if (invariant)
    {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000007U), "c"(0U));
        invariant = (d & (1U << 8U)) != 0U;
    }
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0U), "c"(0U));
    if (a >= 7U)
    {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7U), "c"(0U));
        adjustable = (b & (1U << 1U)) != 0U;
    }
#elif defined(ASTERIA_TARGET_IA64)
    adjustable = true;
#endif
    Kernel::Time::ProcessorClockResidual residual{};
    if (cpu != 0U)
    {
        residual = MeasureClock(timer, cpu);
        if (adjustable)
        {
            const auto midpoint = residual.Lower + static_cast<Foundation::Int64>(residual.RoundTrip / 2U);
            const auto value = timer.GetTicks() - static_cast<Foundation::UInt64>(midpoint);
            if (!Kernel::Time::InitializeOnlineCounter(
                    timer,
                    [](const Foundation::UInt64 ticks) noexcept
                    {
#if defined(ASTERIA_TARGET_AMD64)
                        // Intel SDM rev. 093 Vol. 3B §20.17.3: adjust adds its delta to TSC.
                        Foundation::UInt32 low{0U};
                        Foundation::UInt32 high{0U};
                        __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0x3BU));
                        const auto previous = (static_cast<Foundation::UInt64>(high) << 32U) | low;
                        Foundation::UInt32 tscLow{0U};
                        Foundation::UInt32 tscHigh{0U};
                        __asm__ volatile("lfence; rdtsc; lfence" : "=a"(tscLow), "=d"(tscHigh)::"memory");
                        const auto now = (static_cast<Foundation::UInt64>(tscHigh) << 32U) | tscLow;
                        const auto adjusted = previous + ticks - now;
                        __asm__ volatile("wrmsr" ::"c"(0x3BU), "a"(static_cast<Foundation::UInt32>(adjusted)),
                                         "d"(static_cast<Foundation::UInt32>(adjusted >> 32U))
                                         : "memory");
#elif defined(ASTERIA_TARGET_IA64)
                        AsteriaIa64WriteOnlineIntervalCounter(ticks);
#else
                        static_cast<void>(ticks);
#endif
                    },
                    value))
            {
                return false;
            }
        }
        residual = MeasureClock(timer, cpu);
    }
    const auto epsilon = Kernel::Time::ClockEpsilonTicks(frequency);
    residual.Flags = Kernel::Time::ProcessorClockMeasured | (adjustable ? Kernel::Time::ProcessorClockAdjustable : 0U) |
                     (invariant ? Kernel::Time::ProcessorClockInvariant : 0U) |
                     (hypervisor ? Kernel::Time::ProcessorClockHypervisor : 0U);
    evidence.Processors[cpu] = residual;
    if (Kernel::Time::ClockPlacementExcluded(residual, epsilon))
    {
        g_processorSet.ExcludeUserPlacement(processor);
    }
    __atomic_store_n(&evidence.Frequency, frequency, __ATOMIC_RELEASE);
    __atomic_store_n(&evidence.EpsilonTicks, epsilon, __ATOMIC_RELEASE);
    __atomic_fetch_or(&evidence.ExcludedMask, g_processorSet.ClockExcludedMask(), __ATOMIC_RELEASE);
    __atomic_fetch_or(&evidence.Published, Foundation::UInt64{1U} << cpu, __ATOMIC_RELEASE);
    // Watchdog.md: never age observations sampled before a counter adjustment.
    // Heartbeats continued throughout the rendezvous, so peers retain detection.
    ProcessorWatchdogOnline();
    g_watchdogNextCheck[cpu] = 0U;
    g_clockOnlineInProgress[cpu] = false;
    return true;
}

void ConfigureProcessorWatchdog(const Foundation::UInt64 seconds, const Foundation::UInt64 frequency) noexcept
{
    g_clockFrequency = frequency;
    g_watchdog.Configure(seconds, frequency);
}
Foundation::UInt64 ProcessorWatchdogCheckTicks() noexcept
{
    return g_watchdog.Seconds() == 0U ? 0U : g_watchdog.CheckTicks();
}
void ProcessorWatchdogOnline() noexcept
{
    const auto cpu = CurrentProcessor();
    if (cpu.IsValid())
    {
        g_watchdog.Online(cpu.GetValue(), reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0)));
    }
}
void ProcessorHeartbeat(const Foundation::UInt64 site) noexcept
{
    const auto cpu = CurrentProcessor();
    if (cpu.IsValid())
    {
        g_watchdog.Beat(cpu.GetValue(), site);
    }
}
void ProcessorWatchdogRunning(const Foundation::UInt64 thread, const Foundation::UInt64 component) noexcept
{
    const auto cpu = CurrentProcessor();
    if (cpu.IsValid())
    {
        g_watchdog.Running(cpu.GetValue(), thread, component);
    }
    ProcessorHeartbeat(reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0)));
}
void ProcessorWatchdogTick(const Foundation::UInt64 now, const Foundation::UInt64 site) noexcept
{
    if (ProcessorPowerStopping())
    {
        return;
    }
    const auto cpu = CurrentProcessor();
    if (!cpu.IsValid())
    {
        return;
    }
    g_watchdog.Beat(cpu.GetValue(), site);
    if (g_clockOnlineInProgress[cpu.GetValue()])
    {
        return;
    }
    const auto stalled = g_watchdog.Check(cpu.GetValue(), now, g_processorSet.OnlineMask());
    if (stalled == 0U)
    {
        return;
    }
    Foundation::UInt64 maximumAge{0U};
    for (Foundation::Size peer{0U}; peer < Kernel::Scheduler::MaximumProcessorCount; ++peer)
    {
        const auto age = g_watchdog.AgeMilliseconds(cpu.GetValue(), peer, now);
        if (age > maximumAge)
        {
            maximumAge = age;
        }
    }
#if defined(ASTERIA_TARGET_AMD64)
    g_watchdogFailureAge[cpu.GetValue()] = maximumAge;
    StopProcessorWait(Kernel::Diagnostics::StopCode::Lockup, stalled);
#elif defined(ASTERIA_TARGET_IA64)
    Ia64::Diagnostics::StopIa64(Kernel::Diagnostics::StopCode::Lockup, stalled, g_watchdog.Seconds() * 1000U,
                                cpu.GetValue(), maximumAge);
#endif
}
Foundation::UInt64 ProcessorWatchdogPeriodMilliseconds() noexcept
{
    return g_watchdog.Seconds() * 1000U;
}
Foundation::UInt64 ProcessorWatchdogFailureAge() noexcept
{
    const auto cpu = CurrentProcessor();
    return cpu.IsValid() ? g_watchdogFailureAge[cpu.GetValue()] : 0U;
}
void PollProcessorWatchdog() noexcept
{
    if (ProcessorPowerStopping())
    {
        return;
    }
    const auto cpu = CurrentProcessor();
    const auto interval = ProcessorWatchdogCheckTicks();
    if (!cpu.IsValid() || interval == 0U)
    {
        return;
    }
    const auto now = ProcessorWaitTicks();
    auto& next = g_watchdogNextCheck[cpu.GetValue()];
    if (now < next)
    {
        return;
    }
    next = now + interval;
    ProcessorWatchdogTick(now, reinterpret_cast<Foundation::UIntPointer>(__builtin_return_address(0)));
}
void CaptureProcessorWatchdog(Kernel::Diagnostics::WatchdogEvidence& evidence, const Foundation::Size detector,
                              const Foundation::UInt64 stalled) noexcept
{
    g_watchdog.Snapshot(evidence, detector, ProcessorWaitTicks(), stalled);
}
Foundation::UInt64 ProcessorWaitTicks() noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    return Ia64::Diagnostics::Ia64ProgressTicks();
#else
    const auto* local = Local();
    return local != nullptr && local->Hooks.Ticks != nullptr ? local->Hooks.Ticks(local->Hooks.Context) : 0U;
#endif
}
Foundation::UInt64 ProcessorWaitMinimumTicks() noexcept
{
    const auto* local = Local();
    return local != nullptr ? local->Hooks.MinimumTicks : 0U;
}
Kernel::Scheduler::ProcessorSet* CurrentProcessorSet() noexcept
{
    const auto* local = Local();
    return local != nullptr ? local->Hooks.Set : nullptr;
}
Kernel::Scheduler::ProcessorSet& RuntimeProcessorSet() noexcept
{
    return g_processorSet;
}
void BeginProcessorPowerPark() noexcept
{
    g_processorSet.BeginPowerPark();
}
bool ProcessorPowerStopping() noexcept
{
    return g_processorSet.PowerStopping();
}
bool MarkProcessorParkedForPower(const Kernel::Scheduler::CpuId processor) noexcept
{
    if (!processor.IsValid())
    {
        return false;
    }
#if defined(ASTERIA_TARGET_AMD64) || defined(ASTERIA_TARGET_IA64)
    g_contexts[processor.GetValue()].PendingKicks = 0U;
#else
    t_hostContext.PendingKicks = 0U;
#endif
    return g_processorSet.MarkPowerParked(processor);
}

[[noreturn]] void StopProcessorWait(const Kernel::Diagnostics::StopCode code, const Foundation::UInt64 detail) noexcept
{
#if defined(ASTERIA_TARGET_IA64)
    Ia64::Diagnostics::StopIa64(code, detail);
#else
    const auto* local = Local();
    if (local != nullptr && local->Hooks.Stop != nullptr)
    {
        local->Hooks.Stop(code, detail);
    }
    __builtin_trap();
#endif
}
} // Namespace Asteria::Platform::Synchronization
