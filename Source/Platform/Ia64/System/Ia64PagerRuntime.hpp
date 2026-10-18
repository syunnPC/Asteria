// SPDX-License-Identifier: Apache-2.0

#ifndef ASTERIA_PLATFORM_IA64_SYSTEM_IA64_PAGER_RUNTIME_HPP_INCLUDED
#define ASTERIA_PLATFORM_IA64_SYSTEM_IA64_PAGER_RUNTIME_HPP_INCLUDED

#include "Abi/PagerBootstrap.hpp"
#include "Boot/BootKernelInput.hpp"
#include "Kernel/Interrupt/InterruptLine.hpp"
#include "Platform/Ia64/Device/Ia64PortIo.hpp"
#include "Kernel/Device/PciEcamConfigurationSpace.hpp"
#include "Kernel/Device/PciFunctionControl.hpp"
#include "Kernel/Ipc/WaitSet.hpp"
#include "Kernel/Ipc/WaitSetThreadDispatcher.hpp"
#include "Kernel/Memory/DirectUserMemoryAccess.hpp"
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
#include "Kernel/Memory/PagerChannel.hpp"
#include "Kernel/Object/CapabilityKeyDerivation.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/System/ObjectManager.hpp"
#include "Kernel/System/WaitSetSystemCallIngress.hpp"
#include "Platform/Ia64/Device/Ia64IommuRuntime.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMappingOwner.hpp"
#include "Platform/Ia64/Executable/Ia64InitialUserImageMemoryOwner.hpp"
#include "Platform/Ia64/Interrupt/Ia64InterruptLines.hpp"
#include "Platform/Ia64/Interrupt/Ia64UserFaultEntry.hpp"
#include "Platform/Ia64/Memory/Ia64BspRuntimeMemory.hpp"
#include "Platform/Ia64/Memory/Ia64FrameScrubber.hpp"
#include "Platform/Ia64/Scheduler/Ia64UserModeLaunch.hpp"
#include "Platform/Ia64/System/Ia64SystemCallEntry.hpp"
#include "Platform/Ia64/System/Ia64UserPreemption.hpp"
#include "Platform/Ia64/System/Ia64UserThreadTable.hpp"

namespace Asteria::Platform::Ia64::System
{
/// What the BSP kernel runtime lends the boot pager: its registry and
/// issuer, the revocation registry and clock its capability spaces share,
/// the run loop and controller of the BSP, the shootdown dispatcher and the
/// remote-work transport, the runtime memory that owns the pager's root and
/// entry storage, the charged ResourceDomain, the client's AddressSpace, the
/// DiagnosticChannel the pager may write to, the system-call scratch, and the
/// scheduling point both boot threads' PL3 timer ticks enter.
struct Ia64PagerCollaborators
{
    Kernel::Object::ObjectRegistry* Registry;
    Kernel::Object::RootCapabilityIssuer* Issuer;
    Kernel::Object::RevocationRegistry* Revocations;
    Kernel::Time::IMonotonicClock* Clock;
    Kernel::Scheduler::ProcessorRunController* Controller;
    Scheduler::Ia64ProcessorRunLoop* RunLoop;
    Kernel::Memory::TlbShootdownDispatcher* Shootdown;
    Memory::Ia64BspRuntimeMemory* RuntimeMemory;
    Kernel::Resource::ResourceDomain* Resources;
    Kernel::Resource::ResourceDomainObjectId ResourceIdentity;
    Kernel::Memory::AddressSpaceId ClientSpace;
    Kernel::Object::ObjectId Diagnostic;
    Kernel::System::SystemCallScratch* Scratch;
    IIa64UserPreemption* Preemption;
    Kernel::Scheduler::CpuId Processor;
    Foundation::UInt8 ImplementedVirtualAddressMostSignificantBit;
};

/// Legacy host-fixture initialization; native boot imports all usable RAM and allocates no separate
/// pool.
inline constexpr Foundation::UInt64 Ia64FramePoolPages{1024U};

// Only a published function/BAR pair is assignable.
struct Ia64PagerDevice
{
    Kernel::Object::ObjectId Function{};
    Kernel::Object::ObjectId Region{};
    [[nodiscard]] bool IsPublished() const noexcept
    {
        return Function.IsValid() && Region.IsValid();
    }
};

/// The pager's staged image, lazy backing and one-shot physical-memory
/// import. FramePool fields remain for legacy host fixtures.
struct Ia64PagerRuntimeInput
{
    Ia64PagerCollaborators Collaborators;
    Hal::Memory::ITlbShootdownTransport* Transport;
    Executable::Ia64InitialUserImageMemoryOwner* ImageMemory;
    const Executable::Ia64InitialUserImageMappingOwner* ImageMapping;
    Foundation::UInt64 LazyPhysicalStart;
    Foundation::UInt64 LazyVirtualStart;
    Foundation::UInt64 LazyPages;
    Foundation::UInt64 FramePoolPhysicalStart;
    Foundation::UInt64 FramePoolPages;
    const Kernel::Memory::PhysicalPageImportRange* ImportRanges{nullptr};
    Foundation::Size ImportRangeCount{0U};
    void* BitmapStorage{nullptr};
    Foundation::Size BitmapBytes{0U};
    Foundation::UInt64 LowMemoryBlocks{0U};
    /// The Frame and MemoryObject pools' slabs in the runtime page, attached with the frame pool.
    void* FrameSlab;
    void* MemoryObjectSlab;
    /// The validation keys of the CapabilitySpaces the root service creates, one per
    /// CapabilitySpacePool slot, or null for no created domains.
    const Kernel::Object::CapabilityValidationKey* SpaceKeys;
    /// The ITC ticks of one microsecond the boot measured, which convert the pager's wait timeouts;
    /// zero leaves its waits without timeouts.
    Foundation::UInt64 TicksPerMicrosecond;
    Foundation::UInt64 WatchdogSeconds{Abi::WatchdogDefaultSeconds};
    Foundation::UInt64 SessionMode{0U};
    Foundation::UInt64 LoginEnabled{0U};
    /// The assignable device's function and the DeviceRegion of its BAR, which the grant hands the
    /// pager as Device Manager in the device table's edu entry, and the ECAM configuration space
    /// through which the kernel controls the function; invalid and null for none.
    Kernel::Object::ObjectId DeviceFunction;
    Kernel::Object::ObjectId DeviceRegion;
    /// The AHCI controller's function and the DeviceRegion of its ABAR, the device table's AHCI
    /// entry, invalid for none; and whether it holds no disk but the emulator's (Abi:
    /// PagerManagementDiskWritable).
    Kernel::Object::ObjectId AhciFunction{};
    Kernel::Object::ObjectId AhciRegion{};
    bool AhciDiskWritable{false};
    /// The USB 1.1/2.0 host controllers' functions and register regions, the
    /// device table's USB host entries (Platform/Device/UsbHostPublication.hpp);
    /// invalid for none.
    Kernel::Object::ObjectId UsbHostFunctions[Abi::PagerUsbHostControllers]{};
    Kernel::Object::ObjectId UsbHostRegions[Abi::PagerUsbHostControllers]{};
    /// The e1000e's function and the DeviceRegion of its register BAR, the device table's NIC
    /// entry, invalid for none.
    Kernel::Object::ObjectId NicFunction{};
    Kernel::Object::ObjectId NicRegion{};
    /// The system scanout and packed mode, invalid for none.
    Kernel::Object::ObjectId ScanoutRegion;
    Foundation::UInt64 ScanoutMode[2U];
    Kernel::Device::PciEcamConfigurationSpace* DeviceConfiguration;
    /// The INTx lines the device's Interrupts take, or null where no I/O SAPIC routes its line.
    Interrupt::Ia64InterruptLines* DeviceInterrupts;
    /// The boot's ACPI facts for the i8042 rule (FADT IAPC_BOOT_ARCH, bound
    /// namespace evidence, MADT overrides), and whether the kernel's re-read
    /// of the DSDT still matched that evidence; null publishes nothing.
    const Boot::Acpi::AcpiTopology* I8042Topology{nullptr};
    const Boot::Acpi::PowerRegisters* PowerFacts{nullptr};
    const Boot::BootSalInformation* PowerSalFacts{nullptr};
    bool I8042DsdtMatches{false};
    /// The firmware's one EfiMemoryMappedIOPortSpace window, or zero.
    Foundation::UInt64 PortSpaceBase{0U};
    Foundation::UInt64 PortSpaceBytes{0U};
    Device::Ia64IommuRuntime* Iommu{nullptr};
    Foundation::UInt64 InventoryPhysicalStart{0U};
    Foundation::UInt64 InventoryPages{0U};
    Foundation::UInt64 ModuleBytes{0U};
    /// One enumeration drives DeviceControl eligibility and capability minting; USB has no
    /// unrelated-device dependency.
    [[nodiscard]] Ia64PagerDevice PublishedDevice(const Foundation::Size index) const noexcept
    {
        if (index == Abi::PagerDeviceEdu)
        {
            return {DeviceFunction, DeviceRegion};
        }
        if (index == Abi::PagerDeviceAhci)
        {
            return {AhciFunction, AhciRegion};
        }
        if (index == Abi::PagerDeviceNic)
        {
            return {NicFunction, NicRegion};
        }
        if (index >= Abi::PagerDeviceUsbHost && index < Abi::PagerBootstrapDevices)
        {
            return {UsbHostFunctions[index - Abi::PagerDeviceUsbHost], UsbHostRegions[index - Abi::PagerDeviceUsbHost]};
        }
        return {};
    }
    [[nodiscard]] bool HasPublishedDevices() const noexcept
    {
        for (Foundation::Size index{0U}; index < Abi::PagerBootstrapDevices; ++index)
        {
            if (PublishedDevice(index).IsPublished())
            {
                return true;
            }
        }
        return false;
    }
};

enum class Ia64PagerIdleProgress : Foundation::UInt8
{
    /// The pager's wait completed and its thread is Ready.
    Woken,
    /// The pager is not woken: it has no delivered fault to serve.
    Waiting,
    Failed,
};

enum class Ia64PagerRuntimeError : Foundation::UInt8
{
    None,
    InvalidInput,
    AlreadyInitialized,
    ObjectRejected,
    CapabilityRejected,
    ChannelRejected,
    ImageRejected,
    ThreadRejected,
    ContextRejected,
    EntryContextRejected,
    LaunchRequestRejected,
};

/// The IA-64 boot pager's domain on the BSP : its capability space with the start-block
/// capabilities, the kernel-only FaultToken space, the pager Endpoint and the WaitSet holding the
/// readiness Notification, the lazy region's MemoryObject and Frame, the PagerChannel, and the
/// pager's ProtectionDomain, Thread, SchedulingContext and kernel context. The kernel context runs
/// on the pager's entry storage, so the pager's WaitSetWait blocks there (IIa64SystemCallThread).
/// Native continuation operations run on the BSP with external interrupts disabled, outside an ISR.
/// The object is immovable and lives for the boot. The initial thread whose faults the pager
/// serves: its Thread and the kernel context the run loop binds it to, and its identities.
struct Ia64PagerClient
{
    Kernel::Scheduler::Thread* Thread;
    Scheduler::Ia64KernelContext* Context;
    Kernel::Scheduler::ThreadId ThreadIdentity;
    Kernel::Scheduler::SchedulingContextId SchedulingContextIdentity;
    Foundation::UInt64 ProtectionDomainIdentity{0U};
};

class Ia64PagerRuntime final : public IIa64SystemCallThread,
                               public Interrupt::IIa64UserFaultHandler,
                               public IIa64FaultSupervisor,
                               private Kernel::Memory::IPagerShootdownProgress
{
public:
    /// Ledger serializes the tree of collaborators.Resources, which the pager's management grant
    /// joins as a child.
    Ia64PagerRuntime(const Ia64PagerCollaborators& collaborators, Hal::Memory::ITlbShootdownTransport& transport,
                     Kernel::Resource::ResourceDomainLedger& ledger,
                     const Kernel::Object::CapabilityValidationKey& pagerKey,
                     const Kernel::Object::CapabilityValidationKey& tokenKey) noexcept;

    Ia64PagerRuntime(const Ia64PagerRuntime&) = delete;
    Ia64PagerRuntime& operator=(const Ia64PagerRuntime&) = delete;

    /// Creates the pager's objects and capabilities, its user-memory access
    /// and launch request, prepares its kernel context on its entry storage
    /// and admits its system-call entry context. Once only.
    [[nodiscard]] Ia64PagerRuntimeError Initialize(const Ia64PagerRuntimeInput& input) noexcept;

    [[nodiscard]] Foundation::UInt64 GetI8042Declared() const noexcept
    {
        return m_startBlock[Abi::PagerBootstrapI8042DeclaredWord];
    }

    /// Attaches the initial thread whose faults the pager serves and admits
    /// both domains' user-fault entry contexts: the initial thread's names
    /// this runtime, the pager's a handler that fail-stops. Once, after
    /// Initialize and before either thread enters PL3.
    [[nodiscard]] bool AttachClient(const Ia64PagerClient& client) noexcept;

    /// The initial thread's user fault: delivers it to the pager through the channel and blocks the
    /// thread until a resolution wakes it, then selects its entry contexts again so the fault entry
    /// resumes it. A fault behind another yields and faults again, however long the pager holds the
    /// other (Kernel::Memory::DecideFaultOutcome).
    [[nodiscard]] Interrupt::Ia64UserFaultDisposition
    HandleUserFault(Interrupt::Ia64PageFaultFrame& frame, const Kernel::Memory::FaultRecord& fault) noexcept override;

    /// The initial thread's PL3 timer tick: the scheduling point of
    /// IIa64UserPreemption, which returns once the thread runs again.
    [[nodiscard]] Interrupt::Ia64UserFaultDisposition
    HandleUserInterrupt(Interrupt::Ia64PageFaultFrame& frame) noexcept override;

    /// Ends the running pager's run Ready at a fired deadline and dispatches
    /// the next thread; returns once the pager runs again.
    [[nodiscard]] bool Preempt() noexcept;

    /// Readies, binds and enqueues the pager thread; run loop idle only.
    [[nodiscard]] bool Admit(Kernel::Scheduler::ProcessorKernelRunLoopFailure& failure) noexcept;

    [[nodiscard]] bool DispatchWait(const Kernel::System::SystemCallRequest& request,
                                    Kernel::System::SystemCallResponse& response) noexcept override;

    /// A boot thread does not end with ThreadExit: UnknownSystemCall.
    [[nodiscard]] bool DispatchExit(const Kernel::System::SystemCallRequest& request,
                                    Kernel::System::SystemCallResponse& response) noexcept override;

    /// Reaps the exited threads the pager created before its management call, so the call's
    /// collection frees their slots.
    [[nodiscard]] bool PrepareManagement() noexcept override;

    /// A supervised thread's running fault (the initial thread's or a
    /// created one's), delivered through the channel: blocks the thread
    /// until a resolution wakes it; Busy while the channel holds another.
    [[nodiscard]] Kernel::Memory::FaultDelivery
    DeliverFault(Kernel::Scheduler::Thread& thread, Scheduler::Ia64KernelContext& context,
                 const Kernel::Memory::PagerFaultSource& source) noexcept override;

    /// ProgressFromIdle for a deferred fault: false only when it failed.
    [[nodiscard]] bool Progress() noexcept override;

    /// Signals the readiness the pager waits on: a thread of a domain it created exited.
    void OnExit() noexcept override;

    /// What a thread created in the pager's domain reaches: its
    /// ProtectionDomain, image mappings, channel, object management and the
    /// pager as its supervisor. After Initialize.
    [[nodiscard]] Ia64UserDomainRecord GetDomainRecord() noexcept;

    /// Gives the pager's object management the created-thread table and
    /// reaps it before the pager's management calls; once.
    [[nodiscard]] bool AttachThreads(Ia64UserThreadTable& threads) noexcept;

    /// Idle context, run loop Idle: retries a delivery that pressure deferred and progresses the
    /// pager's blocked wait, which a delivered fault's readiness signal completes, or its passed
    /// deadline times out, and the blocked waits of the threads it created: Woken when any of them
    /// woke.
    [[nodiscard]] Ia64PagerIdleProgress ProgressFromIdle() noexcept;

    /// The earliest ITC deadline of the pager's blocked wait and its created
    /// threads', or zero.
    [[nodiscard]] Foundation::UInt64 GetWaitDeadline() const noexcept;

    /// The pager's launch on its kernel context: writes the start block
    /// through its own stack mapping, selects its entry contexts and enters
    /// PL3. Returns only if the launch is rejected.
    void Launch() noexcept;

    [[nodiscard]] constexpr Ia64PagerRuntimeError GetError() const noexcept
    {
        return m_error;
    }

    [[nodiscard]] Kernel::Memory::PagerChannelSnapshot GetChannelSnapshot() const noexcept
    {
        return m_channel.GetSnapshot();
    }

    /// The Endpoint pool behind the pager's ObjectCreate and ObjectDestroy
    /// and the current charge of its management grant.
    [[nodiscard]] Kernel::Ipc::EndpointPoolSnapshot GetEndpointSnapshot() const noexcept
    {
        return m_objects.GetEndpointSnapshot();
    }

    [[nodiscard]] Kernel::Resource::ResourceDomainUsage GetGrantUsage() const noexcept
    {
        return m_grant.GetSnapshot().Usage;
    }

private:
    /// The pager's own faults, which nothing serves yet, and its PL3 timer
    /// ticks, the scheduling point of IIa64UserPreemption.
    class PagerFaults final : public Interrupt::IIa64UserFaultHandler
    {
    public:
        explicit constexpr PagerFaults(IIa64UserPreemption* const preemption) noexcept : m_preemption{preemption}
        {
        }

        [[nodiscard]] Interrupt::Ia64UserFaultDisposition
        HandleUserFault(Interrupt::Ia64PageFaultFrame&, const Kernel::Memory::FaultRecord&) noexcept override
        {
            return Interrupt::Ia64UserFaultDisposition::FailStop;
        }

        [[nodiscard]] Interrupt::Ia64UserFaultDisposition
        HandleUserInterrupt(Interrupt::Ia64PageFaultFrame&) noexcept override
        {
            return (m_preemption != nullptr) && m_preemption->PreemptUser(Memory::Ia64BspRuntimeUserDomain::Pager)
                       ? Interrupt::Ia64UserFaultDisposition::Resume
                       : Interrupt::Ia64UserFaultDisposition::FailStop;
        }

    private:
        IIa64UserPreemption* m_preemption;
    };

    /// Records each parked Frame pin a Device Manager's confirmation releases in the block
    /// evidence, the last two by turns, and has the scrubber keep the first words of its page as
    /// the frame pool zeroes it: the late write the pin kept from reused memory landed there.
    /// Called under PciFunctionControl's lock on the BSP; it takes none.
    class ParkedReleases final : public Kernel::Device::IParkedPinObserver
    {
    public:
        explicit constexpr ParkedReleases(Memory::Ia64FrameScrubber& scrubber) noexcept : m_scrubber{scrubber}
        {
        }

        void Released(Kernel::Object::ObjectId frame, Kernel::Memory::PhysicalAddress physicalStart,
                      Kernel::Memory::PageCount pages) noexcept override;

    private:
        Memory::Ia64FrameScrubber& m_scrubber;
    };

    void Wait() noexcept override;
    [[nodiscard]] Ia64PagerRuntimeError Fail(Ia64PagerRuntimeError error) noexcept;
    [[nodiscard, gnu::noinline]] bool CreateObjects() noexcept;
    [[nodiscard, gnu::noinline]] bool ImportMemory(const Ia64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool CreateGrant(const Ia64PagerRuntimeInput& input) noexcept;
    /// The optional i8042 authorities: all four handles and
    /// declaration word 68, or none.
    [[gnu::noinline]] void PublishI8042(const Ia64PagerRuntimeInput& input) noexcept;
    /// The CapabilitySpace and AddressSpace pools of the domains the pager
    /// creates: input's keys and the runtime memory's slots.
    [[nodiscard, gnu::noinline]] bool ConfigureDomainPools(const Ia64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool MintScanout(const Ia64PagerRuntimeInput& input) noexcept;
    /// Mints each published assignable device's function and region into its device-table entry of
    /// the start block.
    [[nodiscard, gnu::noinline]] bool MintDevices(const Ia64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool MintDevice(Foundation::Size device, Kernel::Object::ObjectId function,
                                                 Kernel::Object::ObjectId region) noexcept;
    /// Times the blocked wait out once its deadline passed.
    [[nodiscard]] Ia64PagerIdleProgress ExpireWait() noexcept;
    /// The pager's own wait's part of ProgressFromIdle.
    [[nodiscard]] Ia64PagerIdleProgress ProgressOwnWait() noexcept;
    [[nodiscard, gnu::noinline]] bool CheckRegistryBudget() noexcept;
    /// Publishes the object management and created-thread evidence: the
    /// pools' counts and the grant's charge as the pager's last management
    /// call left them.
    [[gnu::noinline]] void RecordManagement() const noexcept;
    [[nodiscard, gnu::noinline]] bool CreateLazyRegion(const Ia64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool MintInto(Foundation::Size word, Kernel::Object::ObjectId object,
                                               Kernel::Object::CapabilityRights rights) noexcept;
    [[nodiscard, gnu::noinline]] bool CaptureImage(const Ia64PagerRuntimeInput& input) noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareThread() noexcept;
    [[nodiscard, gnu::noinline]] bool PrepareContext() noexcept;
    [[nodiscard, gnu::noinline]] bool RetryDelivery() noexcept;

    Ia64PagerCollaborators m_collaborators;
    Kernel::Object::CapabilitySpace m_space;
    Kernel::Object::CapabilitySpace m_tokens;
    Kernel::Ipc::Endpoint m_endpoint;
    Kernel::Ipc::Notification m_readiness;
    Kernel::Ipc::WaitSet m_waitSet;
    Kernel::Ipc::WaitSetThreadDispatcher m_wait;
    Kernel::System::WaitSetSystemCallDispatcher m_waitDispatcher;
    Kernel::Memory::PagerChannel m_channel;
    /// The boot tree's ledger, which the components' ResourceDomains join.
    Kernel::Resource::ResourceDomainLedger& m_ledger;
    Kernel::Resource::ResourceDomain m_grant;
    Kernel::System::ObjectManager m_objects;
    // Zeroes the frame pool's dirty runs for m_objects' Collect.
    Memory::Ia64FrameScrubber m_scrubber;
    ParkedReleases m_releases{m_scrubber};
    Kernel::Memory::BootFrame m_lazyFrame;
    Kernel::Memory::BootFrame m_inventoryFrame;
    Kernel::Memory::MemoryObject m_lazyMemory;
    Kernel::Protection::ProtectionDomain m_protection;
    Kernel::Scheduler::Thread m_thread;
    Kernel::Scheduler::SchedulingContext m_scheduling;
    Kernel::Scheduler::ThreadId m_threadId{};
    Kernel::Scheduler::SchedulingContextId m_schedulingId{};
    Scheduler::Ia64KernelContext m_context;
    // The pager thread's f32-f127 across switches.
    Scheduler::Ia64HighFloatingArea m_highFloating{};
    Kernel::Memory::DirectUserMemoryAccess m_userMemory;
    Kernel::Memory::DirectUserMemoryMapping m_mappings[Kernel::Memory::MaximumDirectUserMemoryMappings]{};
    Foundation::Size m_mappingCount{0U};
    Ia64SystemCallBridgeContext m_bridge{};
    Kernel::Memory::AddressSpaceId m_pagerSpace{};
    Kernel::Object::RevocationDomainId m_root{};
    Scheduler::Ia64UserModeSupervisorContinuation m_supervisor{};
    Scheduler::Ia64UserModeLaunchRequest m_launchRequest{};
    Foundation::UInt64 m_entryDescriptor[2U]{};
    Foundation::UInt64 m_startBlock[Abi::PagerBootstrapWords]{};
    Device::Ia64PortIo m_ports;
    Kernel::Device::DeviceRegion m_i8042Ports[2U]{};
    alignas(Kernel::Interrupt::InterruptLine)
        Foundation::UInt8 m_i8042LineStorage[2U][sizeof(Kernel::Interrupt::InterruptLine)]{};
    Kernel::Object::ObjectId m_i8042[4U]{};
    Foundation::UInt64 m_startBlockAddress{0U};
    Kernel::System::WaitSetSystemCallDispatchResult m_result{};
    Kernel::System::WaitSetSystemCallIngressFailure m_waitFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_waitReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_waitSwitchFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_faultReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_faultSwitchFailure{};
    Kernel::Scheduler::ProcessorKernelRunLoopReceipt m_preemptReceipt{};
    Kernel::Scheduler::ProcessorKernelRunLoopFailure m_preemptFailure{};
    Ia64PagerClient m_client{};
    // Consecutive deferrals of the initial thread's current fault, all its
    // deferrals and the most consecutive ones one of its faults had.
    Foundation::UInt64 m_clientDeferrals{0U};
    Foundation::UInt64 m_clientDeferralTotal{0U};
    Foundation::UInt64 m_clientLongestDeferral{0U};
    Ia64UserThreadTable* m_threads{nullptr};
    /// Whether the grant covers created domains, whose records the thread
    /// table keeps once attached.
    bool m_domains{false};
    /// The device's INTx lines, once its Interrupts may be created.
    const Interrupt::Ia64InterruptLines* m_deviceInterrupts{nullptr};
    Device::Ia64IommuRuntime* m_iommu{nullptr};
    /// The measured ITC rate and the pager's waits that timed out.
    Foundation::UInt64 m_ticksPerMicrosecond{0U};
    Foundation::UInt64 m_waitExpiries{0U};
    PagerFaults m_pagerFaults;
    Ia64PagerRuntimeError m_error{Ia64PagerRuntimeError::None};
    bool m_initialized{false};
};
} // Namespace Asteria::Platform::Ia64::System

#endif // ASTERIA_PLATFORM_IA64_SYSTEM_IA64_PAGER_RUNTIME_HPP_INCLUDED
