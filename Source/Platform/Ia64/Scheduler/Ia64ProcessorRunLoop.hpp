// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_PROCESSOR_RUN_LOOP_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_PROCESSOR_RUN_LOOP_HPP_INCLUDED

#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Platform/Ia64/Scheduler/Ia64KernelContext.hpp"

namespace Asteria::Platform::Ia64::Scheduler
{
using Ia64ProcessorRunLoop = Kernel::Scheduler::ProcessorKernelRunLoop<Ia64KernelContext>;
} // namespace Asteria::Platform::Ia64::Scheduler

namespace Asteria::Kernel::Scheduler
{
extern template class ProcessorKernelRunLoop<Platform::Ia64::Scheduler::Ia64KernelContext>;
} // namespace Asteria::Kernel::Scheduler

#endif // ASTERIA_PLATFORM_IA64_SCHEDULER_IA64_PROCESSOR_RUN_LOOP_HPP_INCLUDED
