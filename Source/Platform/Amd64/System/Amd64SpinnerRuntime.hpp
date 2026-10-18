// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_SPINNER_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_SPINNER_RUNTIME_HPP_INCLUDED

#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Amd64/System/Amd64PagerRuntime.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::System
{
/// What the boot runtime prepared for the spinner before Initialize: its own
/// root and image, and the CPU0 entry state its thread enters CPL3 through.
struct Amd64SpinnerRuntimeInput
{
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Resource::ResourceDomain* Resources;
    /// The spinner's bound private AddressSpace.
    Kernel::Memory::AddressSpaceId Space;
    /// The kernel DiagnosticChannel object the spinner writes its line to.
    Kernel::Object::ObjectId Diagnostic;
    /// The spinner image's user mappings, for its system calls.
    Kernel::Memory::IUserMemoryAccess* UserMemory;
    /// The spinner thread's kernel stack, mapped in every root.
    Scheduler::WritableAmd64KernelStackView KernelStack;
    /// The spinner image's entry and the top of its initial user stack.
    Foundation::UInt64 Entry;
    Foundation::UInt64 UserStackTop;
    Interrupt::Amd64DescriptorTables* Tables;
    Interrupt::Amd64EntryContext* EntryContext;
};

/// The spinner's domain on CPU0, which only the test firmware has: a CPU-bound boot thread with its
/// capability space, holding a DiagnosticChannel capability and a WaitSet capability, the WaitSet
/// with no registration, and its thread, time slice and kernel stack. Its image counts for a fixed
/// time, writes one line and waits on the WaitSet for good; only user preemption lets the other
/// boot threads run before that. Every operation runs on CPU0 with interrupts disabled, outside an
/// ISR: the boot path, and the trap path for the spinner's system calls and ticks.
class Amd64SpinnerRuntime final
{
public:
    Amd64SpinnerRuntime(Kernel::Object::ObjectRegistry& registry, Kernel::Object::RevocationRegistry& revocations,
                        Time::Amd64ApicTimer& timer, Kernel::Scheduler::ProcessorRunController& controller,
                        Scheduler::Amd64ProcessorRunLoop& loop,
                        const Kernel::Object::CapabilityValidationKey& key) noexcept;

    Amd64SpinnerRuntime(const Amd64SpinnerRuntime&) = delete;
    Amd64SpinnerRuntime& operator=(const Amd64SpinnerRuntime&) = delete;

    /// Creates the spinner's objects and capabilities and prepares its thread
    /// and kernel context. Once only, on the idle stack.
    [[nodiscard]] bool Initialize(const Amd64SpinnerRuntimeInput& input) noexcept;

    /// Readies, binds and enqueues the spinner thread; run-loop idle only.
    [[nodiscard]] bool Admit(Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;

    /// Whether the run loop's active thread is the spinner's.
    [[nodiscard]] bool IsRunning() const noexcept;

    /// Whether thread is the spinner's.
    [[nodiscard]] bool Runs(const Kernel::Scheduler::Thread* const thread) const noexcept
    {
        return thread == &m_thread;
    }

    /// The spinner's system call: its own WaitSet wait, which blocks for
    /// good, or the common ingress on its image's mappings. False only on a
    /// kernel invariant failure; a rejected call is a response.
    [[nodiscard]] bool DispatchSystemCall(const Kernel::System::SystemCallRequest& request,
                                          Kernel::System::SystemCallResponse& response,
                                          Kernel::System::SystemCallScratch& scratch) noexcept;

    /// Ends the running spinner's run Ready at a fired deadline and dispatches
    /// the next thread; returns once the spinner runs again.
    [[nodiscard]] bool Preempt() noexcept;

    /// The running spinner's page fault, delivered to the boot pager, its supervisor: blocks the
    /// spinner until a resolution wakes it, which then returns, or a Terminate exits it, which
    /// never does.
    [[nodiscard]] Kernel::Memory::FaultDelivery DeliverFault(IAmd64FaultSupervisor& supervisor,
                                                             const Kernel::Memory::FaultRecord& fault) noexcept;

    /// The spinner thread's state and exit code.
    [[nodiscard]] Kernel::Scheduler::ThreadSnapshot GetThreadSnapshot() const noexcept
    {
        return m_thread.GetSnapshot();
    }

private:
    [[nodiscard, gnu::noinline]] bool CreateObjects(const Amd64SpinnerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareThread(const Amd64SpinnerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool DispatchWait(const Kernel::System::SystemCallRequest& request,
                                                   Kernel::System::SystemCallResponse& response) noexcept;
    [[noreturn]] static void Launch(void* argument) noexcept;

    Kernel::Object::ObjectRegistry& m_registry;
    Kernel::Object::RevocationRegistry& m_revocations;
    Time::Amd64ApicTimer& m_timer;
    [[nodiscard]] Scheduler::Amd64ProcessorRunLoop& LocalLoop() const noexcept
    {
        return Scheduler::CurrentAmd64ProcessorRunLoop(m_loop);
    }
    Scheduler::Amd64ProcessorRunLoop& m_loop;
    Kernel::Object::RootCapabilityIssuer m_issuer;
    Kernel::Object::CapabilitySpace m_space;
    Kernel::Ipc::WaitSet m_waitSet;
    Kernel::Ipc::WaitSetThreadDispatcher m_wait;
    Kernel::System::WaitSetSystemCallDispatcher m_waitDispatcher;
    Kernel::Protection::ProtectionDomain m_protection;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_scheduling;
    Kernel::Scheduler::ThreadId m_threadId{};
    Kernel::Scheduler::SchedulingContextId m_schedulingId{};
    Kernel::Memory::AddressSpaceId m_spaceId{};
    Scheduler::Amd64KernelContext m_context;
    Scheduler::Amd64ExtendedStateArea m_extendedState{};
    Kernel::Memory::IUserMemoryAccess* m_userMemory{nullptr};
    Interrupt::Amd64DescriptorTables* m_tables{nullptr};
    Interrupt::Amd64EntryContext* m_entryContext{nullptr};
    Kernel::Object::CapabilityHandle m_diagnostic{};
    Kernel::Object::CapabilityHandle m_waitSetCapability{};
    Foundation::UInt64 m_kernelStackTop{0U};
    Foundation::UInt64 m_entry{0U};
    Foundation::UInt64 m_userStackTop{0U};
    Interrupt::Amd64TrapFrame m_launchFrame{};
    Kernel::System::WaitSetSystemCallDispatchResult m_result{};
    Kernel::System::WaitSetSystemCallIngressFailure m_waitFailure{};
    Kernel::System::SystemCallIngressFailure m_ingressFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_waitReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_waitSwitchFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_preemptReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_preemptFailure{};
    Scheduler::Amd64ProcessorFaultState m_faultState{};
    bool m_initialized{false};
};
} // Namespace Asteria::Platform::Amd64::System

#endif // ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_SPINNER_RUNTIME_HPP_INCLUDED
