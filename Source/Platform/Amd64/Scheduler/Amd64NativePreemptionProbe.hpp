// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_NATIVE_PREEMPTION_PROBE_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SCHEDULER_AMD64_NATIVE_PREEMPTION_PROBE_HPP_INCLUDED

#include "Kernel/Resource/ResourceDomain.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::Scheduler
{
/// The values the preemptible loop (Amd64NativePreemptionProbe.S, through its
/// generated layout header) loads into the thirteen general registers that
/// carry neither its arguments (RDI, RSI) nor its stack (RSP) before it spins
/// with interrupts enabled. The loop stores every register once it resumed;
/// the proof records those words and does not compare them.
inline constexpr Foundation::UInt64 NativePreemptionSentinelRax{0x101U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelRbx{0x102U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelRcx{0x103U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelRdx{0x104U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelRbp{0x105U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR8{0x108U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR9{0x109U};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR10{0x10AU};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR11{0x10BU};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR12{0x10CU};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR13{0x10DU};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR14{0x10EU};
inline constexpr Foundation::UInt64 NativePreemptionSentinelR15{0x10FU};

struct Amd64NativePreemptionInput
{
    Kernel::Object::ObjectRegistry* Registry;
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Resource::ResourceDomain* Resources;
    Kernel::Memory::AddressSpace* SharedSpace;
    Time::Amd64ApicTimer* Timer;
    Kernel::Scheduler::ProcessorRunController* Controller;
    Amd64ProcessorRunLoop* Loop;
    Amd64KernelContext* Idle;
    WritableAmd64KernelStackView WorkerStack;
    WritableAmd64KernelStackView CoordinatorStack;
    WritableAmd64KernelStackView FreshIdleStack;
    void (*Complete)(void*) noexcept;
    void* Context;
};

/// Optional one-shot BSP proof. Only its fixed, lock-free CPL0 assembly loop
/// may be displaced. The IRQ captures integer state and returns to a dedicated
/// coordinator with IF clear. Common scheduling runs outside the IRQ, exhausts
/// and replenishes the budget, readmits the retained frame, then resumes it.
/// Fresh idle landing retires the exited probe and continues normal user boot.
/// An idle dispatch of the worker that the run loop rolled back because the
/// vCPU was not scheduled in time is repeated after the next replenishment.
/// A rejected operation, a contradictory frame or a scheduler state the next
/// step cannot use fails the boot (BootStage with a NativePreemption stage ID); what the
/// proof observed (counts, sequences, the captured frame and the registers the
/// loop stored after resuming) is recorded in the evidence record
/// (Diagnostics::Amd64NativePreemptionEvidence) before the user launch.
[[noreturn]] void RunAmd64NativePreemption(const Amd64NativePreemptionInput& input) noexcept;

/// Called on CPU0 only, after a due timer was masked/acknowledged. Returns false
/// outside the armed diagnostic. An armed but contradictory frame is terminal.
[[nodiscard]] bool CaptureAmd64NativePreemption(Interrupt::Amd64TrapFrame& frame) noexcept;
} // namespace Asteria::Platform::Amd64::Scheduler
#endif
