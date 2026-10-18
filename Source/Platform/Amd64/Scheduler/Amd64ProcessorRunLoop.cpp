// SPDX-License-Identifier: Apache-2.0

#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Kernel/System/SystemCallIngress.hpp"

template class Asteria::Kernel::Scheduler::ProcessorKernelRunLoop<
    Asteria::Platform::Amd64::Scheduler::Amd64KernelContext>;

template bool
Asteria::Kernel::System::SystemCallIngress::Dispatch<Asteria::Platform::Amd64::Scheduler::Amd64KernelContext>(
    Asteria::Platform::Amd64::Scheduler::Amd64ProcessorRunLoop&, const Asteria::Kernel::System::SystemCallRequest&,
    Asteria::Kernel::System::SystemCallResponse&, Asteria::Kernel::System::SystemCallIngressFailure&) noexcept;

namespace Asteria::Platform::Amd64::Scheduler
{
namespace
{
#if defined(ASTERIA_TARGET_AMD64)
Amd64ProcessorRunLoop* g_processorLoops[Kernel::Scheduler::MaximumProcessorCount]{};
#endif
} // namespace
void BindAmd64ProcessorRunLoop(const Kernel::Scheduler::CpuId processor, Amd64ProcessorRunLoop& loop) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    if (processor.IsValid())
    {
        g_processorLoops[processor.GetValue()] = &loop;
    }
#else
    static_cast<void>(processor);
    static_cast<void>(loop);
#endif
}
Amd64ProcessorRunLoop& CurrentAmd64ProcessorRunLoop(Amd64ProcessorRunLoop& fallback) noexcept
{
#if defined(ASTERIA_TARGET_AMD64)
    const auto cpu = Platform::Synchronization::CurrentProcessor();
    if (cpu.IsValid() && g_processorLoops[cpu.GetValue()] != nullptr)
    {
        return *g_processorLoops[cpu.GetValue()];
    }
#endif
    return fallback;
}
bool Amd64DispatchFromIdle(Amd64ProcessorRunLoop& loop, Kernel::Scheduler::ProcessorKernelRunLoopReceipt& receipt,
                           Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept
{
    return loop.DispatchFromIdle(receipt, failure);
}
} // namespace Asteria::Platform::Amd64::Scheduler
