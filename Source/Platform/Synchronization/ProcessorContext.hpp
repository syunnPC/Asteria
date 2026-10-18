// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_SYNCHRONIZATION_PROCESSOR_CONTEXT_HPP_INCLUDED
#define ASTERIA_PLATFORM_SYNCHRONIZATION_PROCESSOR_CONTEXT_HPP_INCLUDED

#include "Kernel/Diagnostics/StopCode.hpp"
#include "Kernel/Time/ProcessorClock.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Kernel/Diagnostics/Watchdog.hpp"
namespace Asteria::Kernel::Scheduler
{
class ProcessorSet;
}

namespace Asteria::Platform::Synchronization
{
// Hooks are installed by the owning processor before it is admitted online. They outlive the
// processor.
struct ProcessorProgressHooks
{
    void* Context{};
    void (*Drain)(void*) noexcept {};
    void (*PollStop)(void*) noexcept {};
    Foundation::UInt64 (*Ticks)(void*) noexcept {};
    void (*Stop)(Kernel::Diagnostics::StopCode, Foundation::UInt64) noexcept {};
    Foundation::UInt64 MinimumTicks{};
    Kernel::Scheduler::ProcessorSet* Set{};
};
void InstallCurrentProcessor(Kernel::Scheduler::CpuId (*current)() noexcept) noexcept;
void BindProcessorProgress(Kernel::Scheduler::CpuId processor, ProcessorProgressHooks hooks) noexcept;
void BindProcessorDrain(void* context, void (*drain)(void*) noexcept) noexcept;
[[nodiscard]] Kernel::Scheduler::CpuId CurrentProcessor() noexcept;
[[nodiscard]] Foundation::Size HeldLockDepth() noexcept;
void EnterProcessorLock() noexcept;
void LeaveProcessorLock() noexcept;
void DrainProcessorMailbox() noexcept;
void ServiceProcessorClock() noexcept;
[[nodiscard]] bool SynchronizeOnlineProcessorClock(Hal::Time::ILocalTimer& timer, Foundation::UInt64 frequency,
                                                   Kernel::Time::ProcessorClockEvidence& evidence) noexcept;
// Registered once before user execution. Call service only outside locks; false preserves the
// running continuation during a wait.
void BindThreadRequests(void* owner, void (*serve)(void*, bool) noexcept) noexcept;
void ServiceThreadRequests(bool schedulingPoint) noexcept;
// The common wait has a separate, statically non-switching IA-64 closure.
void ServiceWaitingThreadRequests() noexcept;
void LeaveThreadRequestService() noexcept;
void BindProcessorKick(void* owner, void (*kick)(void*, Kernel::Scheduler::CpuId) noexcept) noexcept;
void QueueProcessorKick(Kernel::Scheduler::CpuId home) noexcept;
void KickProcessor(Kernel::Scheduler::CpuId home) noexcept;
void ProcessorWaitProgress() noexcept;
// Terminal power fence: no locks, request waits, reclamation or I/O.
void BeginProcessorPowerPark() noexcept;
[[nodiscard]] bool ProcessorPowerStopping() noexcept;
[[nodiscard]] bool MarkProcessorParkedForPower(Kernel::Scheduler::CpuId processor) noexcept;
void ConfigureProcessorWatchdog(Foundation::UInt64 seconds, Foundation::UInt64 frequency) noexcept;
void ProcessorWatchdogOnline() noexcept;
void ProcessorHeartbeat(Foundation::UInt64 site) noexcept;
void ProcessorWatchdogRunning(Foundation::UInt64 thread, Foundation::UInt64 component) noexcept;
void ProcessorWatchdogTick(Foundation::UInt64 now, Foundation::UInt64 site) noexcept;
[[nodiscard]] Foundation::UInt64 ProcessorWatchdogPeriodMilliseconds() noexcept;
[[nodiscard]] Foundation::UInt64 ProcessorWatchdogFailureAge() noexcept;
void PollProcessorWatchdog() noexcept;
void CaptureProcessorWatchdog(Kernel::Diagnostics::WatchdogEvidence& evidence, Foundation::Size detector,
                              Foundation::UInt64 stalled) noexcept;
[[nodiscard]] Foundation::UInt64 ProcessorWatchdogCheckTicks() noexcept;
[[nodiscard]] Foundation::UInt64 ProcessorWaitTicks() noexcept;
[[nodiscard]] Foundation::UInt64 ProcessorWaitMinimumTicks() noexcept;
[[nodiscard]] Kernel::Scheduler::ProcessorSet* CurrentProcessorSet() noexcept;
[[nodiscard]] Kernel::Scheduler::ProcessorSet& RuntimeProcessorSet() noexcept;
[[noreturn]] void StopProcessorWait(Kernel::Diagnostics::StopCode code, Foundation::UInt64 detail) noexcept;
#if !defined(ASTERIA_TARGET_AMD64) && !defined(ASTERIA_TARGET_IA64)
// Each hosted worker has independent CPU identity, depth and progress hooks.
void SetHostedProcessor(Kernel::Scheduler::CpuId processor) noexcept;
#endif
}
#endif
