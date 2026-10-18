// SPDX-License-Identifier: Apache-2.0

// Dispatcher result, takeover-state and takeover-record constants for
// Interrupt/Ia64SalBootRendezvousHandler.S (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Kernel/Scheduler/SchedulerTypes.hpp"
#include "Platform/Ia64/Interrupt/Ia64ApplicationProcessorTakeoverCoordinator.hpp"
#include "Platform/Ia64/Interrupt/Ia64SalBootRendezvousHandler.hpp"
#include "Platform/Ia64/Memory/ApplicationProcessorExecutionStoragePreparer.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Kernel::Scheduler::MaximumProcessorCount;
using Asteria::Platform::Ia64::Interrupt::Ia64ApplicationProcessorTakeoverRecord;
using Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousDisposition;
using Asteria::Platform::Ia64::Interrupt::Ia64SalBootRendezvousTakeoverState;
using Asteria::Platform::Ia64::Memory::ApplicationProcessorInitialStackScratchSizeBytes;
} // namespace

extern "C" void AsteriaIa64SalBootRendezvousHandlerAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SAL_BOOT_RENDEZVOUS_TAKEOVER_STATE_VALID,
                       static_cast<UInt64>(Ia64SalBootRendezvousTakeoverState::Valid));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_SAL_BOOT_RENDEZVOUS_DISPOSITION_TAKE_OVER,
                       static_cast<UInt64>(Ia64SalBootRendezvousDisposition::TakeOver));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_APPLICATION_PROCESSOR_TAKEOVER_RECORD_ALIGNMENT_MASK,
                       alignof(Ia64ApplicationProcessorTakeoverRecord) - 1U);
    ASTERIA_ASM_LAYOUT(ASTERIA_APPLICATION_PROCESSOR_INITIAL_STACK_SCRATCH_SIZE_BYTES,
                       ApplicationProcessorInitialStackScratchSizeBytes);
    ASTERIA_ASM_LAYOUT(ASTERIA_MAXIMUM_PROCESSOR_COUNT, UInt64{MaximumProcessorCount});
}
