// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_PROCESSOR_RUN_LOOP_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_PROCESSOR_RUN_LOOP_HPP_INCLUDED

#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Platform/Amd64/Scheduler/Amd64KernelContext.hpp"

namespace Asteria::Platform::Amd64::Scheduler
{
using Amd64ProcessorRunLoop = Kernel::Scheduler::ProcessorKernelRunLoop<Amd64KernelContext>;
struct Amd64ProcessorFaultState
{
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt Receipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure Failure{};
};
// Continuations use their current processor's run loop; hosted owners retain the explicit loop
// supplied by their model.
void BindAmd64ProcessorRunLoop(Kernel::Scheduler::CpuId processor, Amd64ProcessorRunLoop& loop) noexcept;
[[nodiscard]] Amd64ProcessorRunLoop& CurrentAmd64ProcessorRunLoop(Amd64ProcessorRunLoop& fallback) noexcept;
/// Out-of-line native idle admission keeps the caller stack independent of
/// the template transaction and its root-switch rollback scratch.
[[nodiscard]] bool Amd64DispatchFromIdle(Amd64ProcessorRunLoop& loop,
                                         Kernel::Scheduler::ProcessorKernelRunLoopReceipt& receipt,
                                         Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;
} // Namespace Asteria::Platform::Amd64::Scheduler

namespace Asteria::Kernel::Scheduler
{
extern template class ProcessorKernelRunLoop<Platform::Amd64::Scheduler::Amd64KernelContext>;
} // Namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_PROCESSOR_RUN_LOOP_HPP_INCLUDED
