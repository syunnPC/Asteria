// SPDX-License-Identifier: Apache-2.0

// Register sentinels for Scheduler/Amd64NativePreemptionProbe.S (see
// Platform/AsmLayout.hpp).

#include "Platform/AsmLayout.hpp"
#include "Platform/Amd64/Scheduler/Amd64NativePreemptionProbe.hpp"

namespace
{
namespace Scheduler = Asteria::Platform::Amd64::Scheduler;
} // namespace

extern "C" void AsteriaAmd64NativePreemptionProbeAsmLayout() noexcept
{
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_RAX, Scheduler::NativePreemptionSentinelRax);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_RBX, Scheduler::NativePreemptionSentinelRbx);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_RCX, Scheduler::NativePreemptionSentinelRcx);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_RDX, Scheduler::NativePreemptionSentinelRdx);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_RBP, Scheduler::NativePreemptionSentinelRbp);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R8, Scheduler::NativePreemptionSentinelR8);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R9, Scheduler::NativePreemptionSentinelR9);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R10, Scheduler::NativePreemptionSentinelR10);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R11, Scheduler::NativePreemptionSentinelR11);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R12, Scheduler::NativePreemptionSentinelR12);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R13, Scheduler::NativePreemptionSentinelR13);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R14, Scheduler::NativePreemptionSentinelR14);
    ASTERIA_ASM_LAYOUT(ASTERIA_AMD64_PREEMPTION_SENTINEL_R15, Scheduler::NativePreemptionSentinelR15);
}
