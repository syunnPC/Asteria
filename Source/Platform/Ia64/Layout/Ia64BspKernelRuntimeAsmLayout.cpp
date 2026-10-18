// SPDX-License-Identifier: Apache-2.0

// Ia64ExternalPreemptionControl offsets and the native-preemption sentinel
// constants for Scheduler/Ia64BspKernelRuntime.S (see Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalInterruptEntry.hpp"
#include "Platform/Ia64/Interrupt/Ia64ExternalPreemptionEntry.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollProcessorStatusMask;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptPollProcessorStatusValue;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalInterruptTimerVector;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionControl;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionControlState;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes;
using Asteria::Platform::Ia64::Interrupt::Ia64ExternalPreemptionMode;
} // namespace

extern "C" void AsteriaIa64BspKernelRuntimeAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, State));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_PROCESSOR_INDEX,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, ProcessorIndex));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_MEMORY_STACK_START,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, WorkerMemoryStackStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_WORKER_RSE_BACKING_STORE_START,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, WorkerRseBackingStoreStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_SENTINEL_ENTRY_POINT,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, SentinelEntryPoint));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_ARMED_DEADLINE_TICKS,
                       __builtin_offsetof(Ia64ExternalPreemptionControl, ArmedDeadlineTicks));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_MODE_BSP_ONE_SHOT_PREEMPTION,
                       static_cast<UInt64>(Ia64ExternalPreemptionMode::BspOneShotPreemption));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_ARMED,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::Armed));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_CONTROL_STATE_COMPLETE,
                       static_cast<UInt64>(Ia64ExternalPreemptionControlState::Complete));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_PREEMPTION_COORDINATOR_RSE_TRANSITION_SIZE_BYTES,
                       Ia64ExternalPreemptionCoordinatorRseTransitionSizeBytes);
    // Widened: the compiler prints an 8-bit operand as a signed byte (0xE8 as -24).
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_TIMER_VECTOR,
                       static_cast<UInt64>(Ia64ExternalInterruptTimerVector));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_MASK,
                       Ia64ExternalInterruptPollProcessorStatusMask);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_EXTERNAL_INTERRUPT_POLL_PROCESSOR_STATUS_VALUE,
                       Ia64ExternalInterruptPollProcessorStatusValue);
}
