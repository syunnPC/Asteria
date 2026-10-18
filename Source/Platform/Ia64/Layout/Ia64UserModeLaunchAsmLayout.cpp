// SPDX-License-Identifier: Apache-2.0

// Ia64UserModeLaunchPlan and Ia64UserModeSupervisorContinuation offsets and
// the launch constants for Scheduler/Ia64UserModeLaunch.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Ia64/Firmware/PalBootstrapCapabilityCapture.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"
#include "Platform/Ia64/System/Ia64UserPreemption.hpp"

namespace
{
using Asteria::Foundation::UInt64;
using Asteria::Platform::Ia64::Firmware::Ia64PhysicalStackedRegisterCount;
using Asteria::Platform::Ia64::Scheduler::Ia64UserModeInitialFloatingPointStatus;
using Asteria::Platform::Ia64::Scheduler::Ia64UserModeLaunchPlan;
using Asteria::Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuation;
using Asteria::Platform::Ia64::Scheduler::Ia64UserModeSupervisorContinuationState;
using Asteria::Platform::Ia64::System::Ia64UserSchedulingPeriodTicks;
} // namespace

extern "C" void AsteriaIa64UserModeLaunchAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_ENTRY_BUNDLE_ADDRESS,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, EntryBundleAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_STACK_POINTER,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, UserStackPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, UserRseBackingStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_PROCESSOR_STATUS,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, UserProcessorStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_USER_RSE_CONFIGURATION,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, UserRseConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_CURRENT_PROCESSOR_STATUS_MASK,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, CurrentProcessorStatusMask));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_CURRENT_PROCESSOR_STATUS_VALUE,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, CurrentProcessorStatusValue));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_SUPERVISOR_CONTINUATION_ADDRESS,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, SupervisorContinuationAddress));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER4,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, InitialArguments.Register4));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER5,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, InitialArguments.Register5));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER6,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, InitialArguments.Register6));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_ARGUMENTS_REGISTER7,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, InitialArguments.Register7));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_LAUNCH_PLAN_INITIAL_GLOBAL_POINTER,
                       __builtin_offsetof(Ia64UserModeLaunchPlan, InitialGlobalPointer));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_ALIGNMENT,
                       alignof(Ia64UserModeSupervisorContinuation));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_R4,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.R4));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_GLOBAL_POINTER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.GlobalPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_STACK_POINTER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.StackPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_THREAD_POINTER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.ThreadPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_INSTRUCTION_POINTER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.InstructionPointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_PREVIOUS_FUNCTION_STATE,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.PreviousFunctionState));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_RSE_CONFIGURATION,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.RseConfiguration));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_RSE_BACKING_STORE_POINTER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.RseBackingStorePointer));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_RSE_NAT_COLLECTION,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.RseNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_USER_NAT_COLLECTION,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.UserNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_PRESERVED_GENERAL_NAT_COLLECTION,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.PreservedGeneralNatCollection));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_LOOP_COUNTER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.LoopCounter));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_FLOATING_POINT_STATUS,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.FloatingPointStatus));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_PREDICATES,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.Predicates));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B1,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.B1));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B2,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.B2));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B3,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.B3));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B4,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.B4));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_B5,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.B5));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_REGISTERS_FLOATING_REGISTERS,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Registers.FloatingRegisters));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_MEMORY_STACK_VIRTUAL_START,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, MemoryStack.VirtualStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_MEMORY_STACK_SIZE_BYTES,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, MemoryStack.SizeBytes));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_RSE_BACKING_STORE_VIRTUAL_START,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, RseBackingStore.VirtualStart));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_RSE_BACKING_STORE_SIZE_BYTES,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, RseBackingStore.SizeBytes));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_STATE,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, State));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_OWNER,
                       __builtin_offsetof(Ia64UserModeSupervisorContinuation, Owner));
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_SUPERVISOR_CONTINUATION_STATE_RETAINED,
                       static_cast<UInt64>(Ia64UserModeSupervisorContinuationState::Retained));

    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_INITIAL_FLOATING_POINT_STATUS, Ia64UserModeInitialFloatingPointStatus);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_INITIAL_RSE_CONFIGURATION,
                       Asteria::Platform::Ia64::Scheduler::Ia64UserModeInitialRseConfiguration);
    // A boot or created user thread's time slice, for test programs that must
    // outlast one (Tests/Guest/Pager/Ia64Spinner.S).
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_SCHEDULING_PERIOD_TICKS, Ia64UserSchedulingPeriodTicks);
    ASTERIA_ASM_LAYOUT(ASTERIA_IA64_USER_MODE_PHYSICAL_STACKED_REGISTERS, Ia64PhysicalStackedRegisterCount);
}
