// SPDX-License-Identifier: Apache-2.0

#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Platform/Ia64/Scheduler/Ia64ProcessorRunLoop.hpp"

template bool
Asteria::Kernel::System::SystemCallIngress::Dispatch<Asteria::Platform::Ia64::Scheduler::Ia64KernelContext>(
    Asteria::Platform::Ia64::Scheduler::Ia64ProcessorRunLoop&, Asteria::Kernel::Memory::IUserMemoryAccess&,
    Asteria::Kernel::System::SystemCallScratch&, const Asteria::Kernel::System::SystemCallRequest&,
    Asteria::Kernel::System::SystemCallResponse&, Asteria::Kernel::System::SystemCallIngressFailure&,
    Asteria::Kernel::Memory::IPagerResolution*, Asteria::Kernel::System::IObjectManagement*) noexcept;
