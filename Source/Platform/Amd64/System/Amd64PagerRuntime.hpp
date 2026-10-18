// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_PAGER_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_PAGER_RUNTIME_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/PagerChannel.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Platform/Amd64/Interrupt/Amd64UserEntry.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Amd64/System/Amd64UserScheduling.hpp"
#include "Platform/Amd64/System/Amd64UserThreadTable.hpp"
#include "Platform/Amd64/Time/Amd64ApicTimer.hpp"

namespace Asteria::Platform::Amd64::System
{
/// An assignable device's PCI function and BAR region; invalid identities
/// name none.
struct Amd64PagerDevice
{
    Kernel::Object::ObjectId Function;
    Kernel::Object::ObjectId Region;
};

/// The scanout: the framebuffer's DeviceRegion and its mode words (Abi:
/// PagerBootstrapScanoutModeWord); an invalid region names none.
struct Amd64PagerScanout
{
    Kernel::Object::ObjectId Region;
    Foundation::UInt64 Mode[2];
};

/// What the boot runtime prepared for the pager before Initialize: its own
/// root and image, the client root and lazy region it serves, and the CPU0
/// entry state its thread enters CPL3 through.
struct Amd64PagerRuntimeInput
{
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Resource::ResourceDomain* Resources;
    /// The pager's bound private AddressSpace and the client's.
    Kernel::Memory::AddressSpaceId PagerSpace;
    Kernel::Memory::AddressSpaceId ClientSpace;
    /// The kernel DiagnosticChannel object the pager may write to.
    Kernel::Object::ObjectId Diagnostic;
    /// The pager image's user mappings, for its system calls.
    Kernel::Memory::IUserMemoryAccess* UserMemory;
    /// The lazy region: its Frame's RAM and the client range it backs.
    Foundation::UInt64 LazyPhysicalStart;
    Foundation::UInt64 LazyVirtualStart;
    Foundation::UInt64 LazyPages;
    /// The pager thread's kernel stack, mapped in every root.
    Scheduler::WritableAmd64KernelStackView KernelStack;
    /// The pager image's entry and the top of its initial user stack.
    Foundation::UInt64 Entry;
    Foundation::UInt64 UserStackTop;
    Interrupt::Amd64DescriptorTables* Tables;
    Interrupt::Amd64EntryContext* EntryContext;
    /// The ResourceDomain the pager may charge through ObjectCreate, which it receives in its start
    /// block with Manage and Inspect, and the object management its ObjectCreate and ObjectDestroy
    /// reach; an invalid identity grants nothing.
    Kernel::Object::ObjectId Management;
    Kernel::System::IObjectManagement* ObjectManagement;
    /// The PagerManagement* features of that grant, for the start block.
    Foundation::UInt64 ManagementFeatures;
    /// With PagerManagementMapping: the window of the pager's own space its
    /// MemoryMap may fill, named in the start block with a Map capability to
    /// that space.
    Foundation::UInt64 MappingWindowStart;
    Foundation::UInt64 MappingWindowPages;
    /// The TSC ticks of one microsecond, which lets the pager's WaitSetWait take a timeout; zero
    /// refuses timeouts.
    Foundation::UInt64 TicksPerMicrosecond;
    Foundation::UInt64 WatchdogSeconds{Abi::WatchdogDefaultSeconds};
    Foundation::UInt64 SessionMode{0U};
    Foundation::UInt64 LoginEnabled{0U};
    /// With a grant, the start block's device table, indexed by Abi::PagerDevice*.
    Amd64PagerDevice Devices[Abi::PagerBootstrapDevices];
    /// With a grant, the scanout the System Display Broker owns.
    Amd64PagerScanout Scanout;
    Kernel::Object::ObjectId I8042[4U]{};
    Foundation::UInt64 I8042Declared{0U};
    Foundation::UInt64 InventoryPhysicalStart{0U};
    Foundation::UInt64 InventoryPages{0U};
    Foundation::UInt64 ModuleBytes{0U};
};

enum class Amd64PagerIdleProgress : Foundation::UInt8
{
    /// The pager's wait completed and its thread is Ready.
    Woken,
    /// The pager is not woken: it runs, has not waited yet, or has no signal.
    Waiting,
    Failed,
};

/// The boot pager's domain on CPU0:
/// its capability space with the start-block capabilities, its thread and
/// kernel stack, the pager Endpoint and the WaitSet holding the readiness
/// Notification, the lazy region's MemoryObject and Frame, and the
/// PagerChannel that joins the client's faults to the pager.
/// Channel fault capture, resolution and home-facade wakes can be reached
/// from any processor with interrupts disabled. The pager's own native continuation and system
/// calls remain on its CPU-0 home.
class Amd64PagerRuntime final : private Kernel::Memory::IPagerShootdownProgress, public IAmd64FaultSupervisor
{
public:
    Amd64PagerRuntime(Kernel::Object::ObjectRegistry& registry, Kernel::Object::RevocationRegistry& revocations,
                      Time::Amd64ApicTimer& timer, Kernel::Scheduler::ProcessorRunController& controller,
                      Scheduler::Amd64ProcessorRunLoop& loop, Kernel::Memory::TlbShootdownDispatcher& shootdown,
                      Hal::Memory::ITlbShootdownTransport& transport,
                      const Kernel::Object::CapabilityValidationKey& pagerKey,
                      const Kernel::Object::CapabilityValidationKey& tokenKey) noexcept;

    Amd64PagerRuntime(const Amd64PagerRuntime&) = delete;
    Amd64PagerRuntime& operator=(const Amd64PagerRuntime&) = delete;

    /// Creates the pager's objects, its capabilities and the start block, and
    /// prepares its thread and kernel context. Once only, on the idle stack.
    [[nodiscard]] bool Initialize(const Amd64PagerRuntimeInput& input) noexcept;

    /// Readies, binds and enqueues the pager thread; run-loop idle only.
    [[nodiscard]] bool Admit(Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;

    /// Whether the run loop's active thread is the pager's.
    [[nodiscard]] bool IsRunning() const noexcept;

    /// Whether thread is the pager's.
    [[nodiscard]] bool Runs(const Kernel::Scheduler::Thread* const thread) const noexcept
    {
        return thread == &m_thread;
    }

    /// The pager's system call: its own WaitSet wait, or the common ingress
    /// with FaultResolve bound to the channel and ObjectCreate and
    /// ObjectDestroy to the object management. False only on a kernel
    /// invariant failure; a rejected call is a response.
    [[nodiscard]] bool DispatchSystemCall(const Kernel::System::SystemCallRequest& request,
                                          Kernel::System::SystemCallResponse& response,
                                          Kernel::System::SystemCallScratch& scratch) noexcept;

    /// Delivers a supervised thread's running fault to the pager and blocks
    /// the thread until a resolution wakes it; Resolved once the thread runs
    /// again, so its trap returns to the faulting instruction, Busy while the
    /// channel holds another fault.
    [[nodiscard]] Kernel::Memory::FaultDelivery
    DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Amd64KernelContext& context,
                 const Kernel::Memory::PagerFaultSource& source,
                 Scheduler::Amd64ProcessorFaultState& state) noexcept override;

    /// ProgressFromIdle for a deferred fault: false only when it failed.
    [[nodiscard]] bool Progress() noexcept override;

    /// Signals the readiness Notification, so the pager's wait returns at the
    /// next scheduling point and it can reclaim the exited thread.
    void OnExit() noexcept override;

    /// Ends the running pager's run Ready at a fired run stop and dispatches
    /// the next thread; returns once the pager runs again.
    [[nodiscard]] bool Preempt() noexcept;

    /// A scheduling point (idle, or a user tick): retries a deferred delivery and progresses the
    /// pager's blocked wait, waking the pager once a delivered fault signalled it or, with nothing
    /// ready, once its deadline passed.
    [[nodiscard]] Amd64PagerIdleProgress ProgressFromIdle() noexcept;

    /// The deadline of the pager's blocked wait, or zero: the idle loop
    /// stops waiting for a tick there.
    [[nodiscard]] Foundation::UInt64 GetWaitDeadline() const noexcept
    {
        return (m_wait.GetSnapshot().State == Kernel::Ipc::WaitSetThreadDispatcherState::Blocked) ? m_wait.GetDeadline()
                                                                                                  : 0U;
    }

    /// The pager's waits that timed out.
    [[nodiscard]] Foundation::UInt64 GetWaitExpiries() const noexcept
    {
        return m_waitExpiries;
    }

    [[nodiscard]] Kernel::Memory::PagerChannelSnapshot GetChannelSnapshot() const noexcept
    {
        return m_channel.GetSnapshot();
    }

    /// What a thread created in the pager's domain reaches through its system calls: the pager's
    /// ProtectionDomain, image mappings, channel and object management. After Initialize.
    [[nodiscard]] Amd64UserDomainRecord GetDomainRecord() noexcept
    {
        return Amd64UserDomainRecord{
            .Protection = &m_protection,
            .UserMemory = m_userMemory,
            .ObjectManagement = m_objectManagement,
            .PagerResolution = &m_channel,
            .Supervisor = this,
            .SupervisionTag = 0U,
        };
    }

private:
    // A resolution that leaves remote invalidation pending gives the
    // application processors' drains a bounded delay (IPagerShootdownProgress).
    void Wait() noexcept override;
    [[nodiscard, gnu::noinline]] bool CreateObjects() noexcept;
    [[nodiscard, gnu::noinline]] bool CreateLazyRegion(const Amd64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool MintStartBlock(const Amd64PagerRuntimeInput& input) noexcept;
    /// The assignable device's capabilities, where the input names one.
    [[nodiscard, gnu::noinline]] bool MintDevice(const Amd64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool MintScanout(const Amd64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool MintInto(Foundation::Size word, Kernel::Object::ObjectId object,
                                               Kernel::Object::CapabilityRights rights) noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareThread(const Amd64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool DispatchWait(const Kernel::System::SystemCallRequest& request,
                                                   Kernel::System::SystemCallResponse& response) noexcept;
    /// Writes the resolver failure behind a rejected FaultResolve as a
    /// diagnostic line: dispatcher error, preparation error, AddressSpace,
    /// page-table and pin errors, one byte each.
    void ReportRejectedResolution() const noexcept;
    /// Writes why the pager's wait failed as a diagnostic line: the stage
    /// (1 ingress, 2 switch, 3 receipt, 4 completion), the ingress, run-loop
    /// and controller errors and the receipt state, one byte each; false.
    [[nodiscard, gnu::noinline]] bool RejectWait(Foundation::UInt8 stage) const noexcept;
    /// Retries a delivery that pressure deferred; false once the channel faulted.
    [[nodiscard, gnu::noinline]] bool RetryDelivery() noexcept;
    [[noreturn]] static void Launch(void* argument) noexcept;

    Kernel::Object::ObjectRegistry& m_registry;
    Kernel::Object::RevocationRegistry& m_revocations;
    Time::Amd64ApicTimer& m_timer;
    Kernel::Scheduler::ProcessorRunController& m_controller;
    [[nodiscard]] Scheduler::Amd64ProcessorRunLoop& LocalLoop() const noexcept
    {
        return Scheduler::CurrentAmd64ProcessorRunLoop(m_loop);
    }
    Scheduler::Amd64ProcessorRunLoop& m_loop;
    Kernel::Object::RootCapabilityIssuer m_issuer;
    Kernel::Object::CapabilitySpace m_space;
    Kernel::Object::CapabilitySpace m_tokens;
    Kernel::Ipc::Endpoint m_endpoint;
    Kernel::Ipc::Notification m_readiness;
    Kernel::Ipc::WaitSet m_waitSet;
    Kernel::Ipc::WaitSetThreadDispatcher m_wait;
    Kernel::System::WaitSetSystemCallDispatcher m_waitDispatcher;
    Kernel::Memory::PagerChannel m_channel;
    Kernel::Memory::BootFrame m_lazyFrame;
    Kernel::Memory::BootFrame m_inventoryFrame;
    Kernel::Memory::MemoryObject m_lazyMemory;
    Kernel::Protection::ProtectionDomain m_protection;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_scheduling;
    Kernel::Scheduler::ThreadId m_threadId{};
    Kernel::Scheduler::SchedulingContextId m_schedulingId{};
    Scheduler::Amd64KernelContext m_context;
    Scheduler::Amd64ExtendedStateArea m_extendedState{};
    Kernel::Memory::IUserMemoryAccess* m_userMemory{nullptr};
    Kernel::System::IObjectManagement* m_objectManagement{nullptr};
    Interrupt::Amd64DescriptorTables* m_tables{nullptr};
    Interrupt::Amd64EntryContext* m_entryContext{nullptr};
    Kernel::Object::RevocationDomainId m_root{};
    Foundation::UInt64 m_kernelStackTop{0U};
    Foundation::UInt64 m_entry{0U};
    Foundation::UInt64 m_userStackTop{0U};
    Foundation::UInt64 m_startBlock[Abi::PagerBootstrapWords]{};
    Interrupt::Amd64TrapFrame m_launchFrame{};
    Kernel::System::WaitSetSystemCallDispatchResult m_result{};
    Kernel::System::WaitSetSystemCallIngressFailure m_waitFailure{};
    Kernel::System::SystemCallIngressFailure m_ingressFailure{};
    // The pager's wait and the client's fault each keep their own receipt: a
    // blocked transaction completes into it only after the other ran.
    Foundation::UInt64 m_waitExpiries{0U};
    [[nodiscard, gnu::noinline]] Amd64PagerIdleProgress ExpireWait() noexcept;
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_waitReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_waitSwitchFailure{};
    // Fault-return receipts belong to their CPU.

    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_preemptReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_preemptFailure{};
    bool m_initialized{false};
};
} // Namespace Asteria::Platform::Amd64::System

#endif // ASTERIA_PLATFORM_AMD64_SYSTEM_AMD64_PAGER_RUNTIME_HPP_INCLUDED
