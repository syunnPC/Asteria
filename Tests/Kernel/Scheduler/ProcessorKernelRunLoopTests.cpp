// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/SynchronousCallDispatcher.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <new>
#include <thread>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
using Foundation::Size;
using Foundation::UInt64;
using Hal::Memory::AddressSpacePageTableActivationError;
using Hal::Memory::AddressSpacePageTableError;
using Hal::Memory::AddressSpacePageTableMapResult;
using Hal::Memory::AddressSpacePageTableOwnershipError;
using Hal::Memory::IAddressSpacePageTable;
using Hal::Time::ILocalTimer;
using Hal::Time::LocalTimerError;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Ipc::ReplyToken;
using Kernel::Ipc::ReplyTokenError;
using Kernel::Ipc::ReplyTokenState;
using Kernel::Ipc::SynchronousCallDispatcher;
using Kernel::Ipc::SynchronousCallDispatcherError;
using Kernel::Ipc::SynchronousCallDispatcherState;
using Kernel::Ipc::SynchronousCallPrepareRequest;
using Kernel::Ipc::SynchronousReplyDispatcher;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::AddressSpaceTransitionReceipt;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::ProcessorAddressSpaceController;
using Kernel::Memory::ProcessorAddressSpaceControllerError;
using Kernel::Memory::ProcessorAddressSpaceControllerState;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::MonotonicTime;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainError;
using Kernel::Protection::ProtectionDomainId;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::BlockedRunCommitOutcome;
using Kernel::Scheduler::MaximumIdleRootTransitionAttempts;
using Kernel::Scheduler::ProcessorKernelRunLoop;
using Kernel::Scheduler::ProcessorKernelRunLoopError;
using Kernel::Scheduler::ProcessorKernelRunLoopFailure;
using Kernel::Scheduler::ProcessorKernelRunLoopReceipt;
using Kernel::Scheduler::ProcessorKernelRunLoopResumeKind;
using Kernel::Scheduler::ProcessorKernelRunLoopSnapshot;
using Kernel::Scheduler::ProcessorKernelRunLoopState;
using Kernel::Scheduler::ProcessorRunController;
using Kernel::Scheduler::ProcessorRunDispatchMode;
using Kernel::Scheduler::ProcessorRunError;
using Kernel::Scheduler::ProcessorRunExternalStopError;
using Kernel::Scheduler::ProcessorRunExternalStopHardwareState;
using Kernel::Scheduler::ProcessorRunExternalStopObservation;
using Kernel::Scheduler::ProcessorRunState;
using Kernel::Scheduler::ReadyQueueState;
using Kernel::Scheduler::ReadyQueue;
using Kernel::Scheduler::ReadyQueueError;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Kernel::Time::ProcessorDeadlineState;
using Kernel::Time::TimerQueue;
using Platform::Synchronization::SpinLock;

class ManualLocalTimer final : public ILocalTimer
{
public:
    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        ++m_readCalls;
        const UInt64 observedTicks = m_ticks;
        if (m_readsBeforeTicksChange == 1U)
        {
            m_ticks = m_ticksAfterReads;
        }
        m_readsBeforeTicksChange -= (m_readsBeforeTicksChange != 0U) ? 1U : 0U;
        return observedTicks;
    }

    [[nodiscard]] LocalTimerError ProgramDeadline(const UInt64 deadlineTicks) noexcept override
    {
        ++m_programCalls;
        if (deadlineTicks <= m_ticks)
        {
            return LocalTimerError::DeadlineReached;
        }
        m_deadline = deadlineTicks;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError CancelDeadline() noexcept override
    {
        ++m_cancelCalls;
        m_deadline = 0U;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError AcknowledgeExternallyMaskedDeadline(const UInt64 deadlineTicks) noexcept override
    {
        ++m_acknowledgeCalls;
        if (m_nextAcknowledgeError != LocalTimerError::None)
        {
            const LocalTimerError error = m_nextAcknowledgeError;
            m_nextAcknowledgeError = LocalTimerError::None;
            return error;
        }
        if ((deadlineTicks == 0U) || (deadlineTicks != m_deadline))
        {
            return LocalTimerError::InvalidDeadline;
        }
        m_deadline = 0U;
        if (m_endRunOnAcknowledgeContext != nullptr)
        {
            m_endRunOnAcknowledgeAccepted =
                m_endRunOnAcknowledgeContext->EndRun(m_endRunOnAcknowledgeProcessor, m_endRunOnAcknowledgeTime)
                    .HasValue();
            m_endRunOnAcknowledgeContext = nullptr;
        }
        return LocalTimerError::None;
    }

    void SetTicks(const UInt64 ticks) noexcept
    {
        m_ticks = ticks;
    }

    /// The ticks change to the given value after the given number of reads,
    /// as a processor that stalls between two samples observes them.
    void SetTicksAfterReads(const Size reads, const UInt64 ticks) noexcept
    {
        m_ticksAfterReads = ticks;
        m_readsBeforeTicksChange = reads;
    }

    void FailNextAcknowledge(const LocalTimerError error) noexcept
    {
        m_nextAcknowledgeError = error;
    }

    void EndRunOnNextAcknowledge(SchedulingContext& context, const CpuId processor, const MonotonicTime time) noexcept
    {
        m_endRunOnAcknowledgeContext = &context;
        m_endRunOnAcknowledgeProcessor = processor;
        m_endRunOnAcknowledgeTime = time;
        m_endRunOnAcknowledgeAccepted = false;
    }

    [[nodiscard]] constexpr bool EndRunOnAcknowledgeAccepted() const noexcept
    {
        return m_endRunOnAcknowledgeAccepted;
    }

    [[nodiscard]] constexpr Size GetProgramCalls() const noexcept
    {
        return m_programCalls;
    }

    [[nodiscard]] constexpr Size GetReadCalls() const noexcept
    {
        return m_readCalls;
    }

    [[nodiscard]] constexpr Size GetCancelCalls() const noexcept
    {
        return m_cancelCalls;
    }

    [[nodiscard]] constexpr Size GetAcknowledgeCalls() const noexcept
    {
        return m_acknowledgeCalls;
    }

    [[nodiscard]] constexpr UInt64 GetArmedDeadline() const noexcept
    {
        return m_deadline;
    }

private:
    mutable UInt64 m_ticks{10U};
    UInt64 m_ticksAfterReads{0U};
    mutable Size m_readsBeforeTicksChange{0U};
    UInt64 m_deadline{0U};
    mutable Size m_readCalls{0U};
    Size m_programCalls{0U};
    Size m_cancelCalls{0U};
    Size m_acknowledgeCalls{0U};
    LocalTimerError m_nextAcknowledgeError{LocalTimerError::None};
    SchedulingContext* m_endRunOnAcknowledgeContext{nullptr};
    CpuId m_endRunOnAcknowledgeProcessor{};
    MonotonicTime m_endRunOnAcknowledgeTime{};
    bool m_endRunOnAcknowledgeAccepted{false};
};

class TestAddressSpacePageTable final : public IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Claim(const AddressSpace& owner,
                                                            const InitialMappingPlan&) noexcept override
    {
        if (m_owner != nullptr)
        {
            return AddressSpacePageTableOwnershipError::AlreadyOwned;
        }
        m_owner = &owner;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Release(const AddressSpace& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return AddressSpacePageTableOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableActivationError Activate(const AddressSpace& owner,
                                                                const CpuId processor) noexcept override
    {
        ++ActivationCalls;
        if ((m_owner != &owner) || !processor.IsValid())
        {
            return AddressSpacePageTableActivationError::InvalidOwner;
        }
        if (ActivationTimer != nullptr)
        {
            ActivationTimer->SetTicks(ActivationTicks);
        }
        if (ThreadToObserve != nullptr)
        {
            ThreadWasRunningDuringActivation = ThreadToObserve->GetSnapshot().State == ThreadState::Running;
        }
        if (RejectedActivations != 0U)
        {
            --RejectedActivations;
            return AddressSpacePageTableActivationError::RejectedBeforeCommit;
        }
        return NextActivationError;
    }

    [[nodiscard]] AddressSpacePageTableError Unmap(const AddressSpaceMappingRecord&,
                                                   AddressSpacePageTableMapResult&) noexcept override
    {
        return AddressSpacePageTableError::UnsupportedConfiguration;
    }

    [[nodiscard]] AddressSpacePageTableError Map(const AddressSpaceMappingRecord&,
                                                 AddressSpacePageTableMapResult&) noexcept override
    {
        return AddressSpacePageTableError::InternalFailure;
    }

    AddressSpacePageTableActivationError NextActivationError{AddressSpacePageTableActivationError::None};
    // The next activations to refuse before commit, then NextActivationError.
    Size RejectedActivations{0U};
    ManualLocalTimer* ActivationTimer{nullptr};
    Thread* ThreadToObserve{nullptr};
    UInt64 ActivationTicks{0U};
    Size ActivationCalls{0U};
    bool ThreadWasRunningDuringActivation{false};

private:
    const AddressSpace* m_owner{nullptr};
};

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute);
}

[[nodiscard]] constexpr InitialMapping MakeSeedMapping(const UInt64 virtualStart, const UInt64 physicalStart,
                                                       const UInt64 pages, const MappingPermissions permissions,
                                                       const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{virtualStart},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

[[nodiscard]] bool BuildSeedPlan(InitialMappingPlan& plan) noexcept
{
    constexpr UInt64 base{0xFFFF'8000'0000'0000ULL};
    return (plan.AddMapping(MakeSeedMapping(base + 0x1000U, 0x0010'0000U, 2U, ReadExecute(),
                                            InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(base + 0x4000U, 0x0010'4000U, 4U, ReadWrite(),
                                            InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(base + 0x8000U, 0x0010'8000U, 16U, ReadWrite(),
                                            InitialMappingOwner::PageTable)) == InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

class RegisteredProtectionDomain final
{
public:
    RegisteredProtectionDomain(SpinLock& registryLock, ObjectRegistry& registry, RootCapabilityIssuer& issuer,
                               ManualLocalTimer& clock) noexcept
        : Revocations{registryLock},
          Capabilities{registry, Revocations, clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          ResourceStorage{ResourceLedger}, m_issuer{issuer}
    {
        if (!BuildSeedPlan(SeedPlan) ||
            (AddressSpaceStorage.Initialize(ResourceDomainId{7U}, PageTable, SeedPlan) != AddressSpaceError::None))
        {
            return;
        }
        const ResourceDomainLimits resourceLimits{
            .CommittedMemoryPages = 4096U,
            .ResidentMemoryPages = 4096U,
            .KernelMetadataBytes = 64U * 1024U,
            .CapabilityHandles = 4096U,
            .IpcQueueBytes = 64U * 1024U,
            .Threads = 256U,
            .ChildDomains = 16U,
        };
        if (ResourceStorage.InitializeRoot(ResourceDomainId{9U}, resourceLimits).Error != ResourceDomainError::None)
        {
            return;
        }
        const auto resource = m_issuer.CreateObject(ResourceStorage);
        if (!resource.HasValue())
        {
            return;
        }
        ResourceObject = resource.GetValue();
        const ResourceDomainObjectId resourceIdentity{registry, ResourceObject};
        const auto address = m_issuer.CreateObject(AddressSpaceStorage);
        if (!address.HasValue())
        {
            return;
        }
        AddressSpaceObject = address.GetValue();
        const AddressSpaceId addressIdentity{registry, AddressSpaceObject};
        if (AddressSpaceStorage.BindIdentity(registry, addressIdentity).Error != AddressSpaceError::None)
        {
            return;
        }
        if (Domain.Initialize(registry, resourceIdentity, ResourceStorage, addressIdentity, Capabilities).Error !=
            ProtectionDomainError::None)
        {
            return;
        }
        const auto domain = m_issuer.CreateObject(Domain);
        if (!domain.HasValue())
        {
            return;
        }
        DomainObject = domain.GetValue();
        Identity = ProtectionDomainId{registry, DomainObject};
        Ready = Domain.BindIdentity(registry, Identity).Error == ProtectionDomainError::None;
    }

    ~RegisteredProtectionDomain() noexcept
    {
        if (Domain.GetSnapshot().State != Kernel::Protection::ProtectionDomainState::Uninitialized)
        {
            static_cast<void>(Domain.Quiesce());
        }
        const auto addressState = AddressSpaceStorage.GetSnapshot().State;
        if ((addressState != Kernel::Memory::AddressSpaceState::Uninitialized) &&
            (addressState != Kernel::Memory::AddressSpaceState::Quiesced))
        {
            for (Foundation::UInt8 cpu{0U}; cpu < 4U; ++cpu)
            {
                if ((AddressSpaceStorage.GetSnapshot().ResidentProcessorMask &
                     static_cast<Foundation::UInt8>(1U << cpu)) != 0U)
                {
                    static_cast<void>(AddressSpaceStorage.DetachProcessor(CpuId{cpu}));
                }
            }
            static_cast<void>(AddressSpaceStorage.Quiesce());
        }
        if (ResourceStorage.IsReady())
        {
            static_cast<void>(ResourceStorage.Quiesce());
        }
        if (DomainObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(DomainObject));
        }
        if (AddressSpaceObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(AddressSpaceObject));
        }
        if (ResourceObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ResourceObject));
        }
        for (Size index{0U}; index < 3U; ++index)
        {
            static_cast<void>(m_issuer.CollectDestroyedObject());
        }
    }

    RegisteredProtectionDomain(const RegisteredProtectionDomain&) = delete;
    RegisteredProtectionDomain& operator=(const RegisteredProtectionDomain&) = delete;
    RegisteredProtectionDomain(RegisteredProtectionDomain&&) = delete;
    RegisteredProtectionDomain& operator=(RegisteredProtectionDomain&&) = delete;

    RevocationRegistry Revocations;
    CapabilitySpace Capabilities;
    ResourceDomainLedger ResourceLedger;
    ResourceDomain ResourceStorage;
    TestAddressSpacePageTable PageTable;
    InitialMappingPlan SeedPlan;
    AddressSpace AddressSpaceStorage;
    ProtectionDomain Domain;
    ObjectId AddressSpaceObject{};
    ObjectId ResourceObject{};
    ObjectId DomainObject{};
    ProtectionDomainId Identity{};
    bool Ready{false};

private:
    RootCapabilityIssuer& m_issuer;
};

enum class FakeContextError : Foundation::UInt8
{
    None,
    Rejected,
};

class FakeKernelContext final
{
public:
    using SwitchHook = void (*)(void*) noexcept;

    explicit constexpr FakeKernelContext(const bool prepared = false) noexcept : m_prepared{prepared}
    {
    }

    [[nodiscard]] constexpr bool IsPrepared() const noexcept
    {
        return m_prepared;
    }

    void SetSwitchError(const FakeContextError error) noexcept
    {
        m_switchError = error;
    }

    [[nodiscard]] FakeContextError SwitchTo(FakeKernelContext& next) noexcept
    {
        m_lastTarget = &next;
        ++m_switchCount;
        if (m_nextSwitchHook != nullptr)
        {
            SwitchHook const hook = m_nextSwitchHook;
            m_nextSwitchHook = nullptr;
            hook(m_nextSwitchHookContext);
        }
        return m_switchError;
    }

    void InvokeOnNextSwitch(SwitchHook const hook, void* const context) noexcept
    {
        m_nextSwitchHook = hook;
        m_nextSwitchHookContext = context;
    }

    [[nodiscard]] constexpr Size GetSwitchCount() const noexcept
    {
        return m_switchCount;
    }

    [[nodiscard]] constexpr bool SaveExternalFrameIdleContinuation(const UInt64 generation) noexcept
    {
        if ((generation == 0U) || (generation <= m_externalIdleContinuationGeneration))
        {
            return false;
        }
        m_externalIdleContinuationGeneration = generation;
        return true;
    }

    [[nodiscard]] constexpr UInt64 GetExternalFrameIdleContinuationGeneration() const noexcept
    {
        return m_externalIdleContinuationGeneration;
    }

private:
    FakeKernelContext* m_lastTarget{nullptr};
    Size m_switchCount{0U};
    FakeContextError m_switchError{FakeContextError::None};
    SwitchHook m_nextSwitchHook{nullptr};
    void* m_nextSwitchHookContext{nullptr};
    UInt64 m_externalIdleContinuationGeneration{0U};
    bool m_prepared{false};
};

class CooperativeKernelContext final
{
public:
    using SwitchHook = void (*)(void*) noexcept;

    explicit CooperativeKernelContext(const bool prepared = false) noexcept : m_prepared{prepared}
    {
    }

    [[nodiscard]] bool IsPrepared() const noexcept
    {
        return m_prepared.load(std::memory_order_acquire);
    }

    [[nodiscard]] FakeContextError SwitchTo(CooperativeKernelContext& next) noexcept
    {
        m_switchCount.fetch_add(1U, std::memory_order_relaxed);
        next.m_destinationSwitchCount.fetch_add(1U, std::memory_order_relaxed);
        if (m_nextSwitchHook != nullptr)
        {
            SwitchHook const hook = m_nextSwitchHook;
            void* const context = m_nextSwitchHookContext;
            m_nextSwitchHook = nullptr;
            m_nextSwitchHookContext = nullptr;
            hook(context);
        }
        next.Resume();
        return WaitForResume();
    }

    void InvokeOnNextSwitch(SwitchHook const hook, void* const context) noexcept
    {
        m_nextSwitchHook = hook;
        m_nextSwitchHookContext = context;
    }

    [[nodiscard]] Size GetSwitchCount() const noexcept
    {
        return m_switchCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Size GetDestinationSwitchCount() const noexcept
    {
        return m_destinationSwitchCount.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool SaveExternalFrameIdleContinuation(const UInt64 generation) noexcept
    {
        if (m_rejectNextExternalIdleContinuation.exchange(false, std::memory_order_acq_rel))
        {
            return false;
        }
        const UInt64 current = m_externalIdleContinuationGeneration.load(std::memory_order_acquire);
        if ((generation == 0U) || (generation <= current))
        {
            return false;
        }
        const bool publishMismatch = m_mismatchNextExternalIdleContinuation.exchange(false, std::memory_order_acq_rel);
        if (publishMismatch && (generation == ~UInt64{0U}))
        {
            return false;
        }
        m_externalIdleContinuationGeneration.store(publishMismatch ? generation + 1U : generation,
                                                   std::memory_order_release);
        m_externalIdleContinuationLandingGeneration.store(0U, std::memory_order_release);
        m_externalIdleContinuationArmed.store(true, std::memory_order_release);
        return true;
    }

    void RejectNextExternalFrameIdleContinuationSave() noexcept
    {
        m_rejectNextExternalIdleContinuation.store(true, std::memory_order_release);
    }

    void MismatchNextExternalFrameIdleContinuationSave() noexcept
    {
        m_mismatchNextExternalIdleContinuation.store(true, std::memory_order_release);
    }

    void SeedExternalFrameIdleContinuationGeneration(const UInt64 generation) noexcept
    {
        m_externalIdleContinuationGeneration.store(generation, std::memory_order_release);
        m_externalIdleContinuationLandingGeneration.store(0U, std::memory_order_release);
        m_externalIdleContinuationArmed.store(false, std::memory_order_release);
    }

    [[nodiscard]] UInt64 GetExternalFrameIdleContinuationGeneration() const noexcept
    {
        return m_externalIdleContinuationGeneration.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool ConsumeExternalFrameIdleContinuationLanding(const UInt64 generation) noexcept
    {
        UInt64 expected{generation};
        return (generation != 0U) && m_externalIdleContinuationLandingGeneration.compare_exchange_strong(
                                         expected, 0U, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    void SetPrepared(const bool prepared) noexcept
    {
        m_prepared.store(prepared, std::memory_order_release);
    }

    [[nodiscard]] FakeContextError WaitForResume() noexcept
    {
        for (;;)
        {
            if (m_stopped.load(std::memory_order_acquire))
            {
                return FakeContextError::Rejected;
            }
            if (m_resumed.exchange(false, std::memory_order_acquire))
            {
                return FakeContextError::None;
            }
            m_resumed.wait(false, std::memory_order_acquire);
        }
    }

    void Resume() noexcept
    {
        if (m_externalIdleContinuationArmed.exchange(false, std::memory_order_acq_rel))
        {
            m_externalIdleContinuationLandingGeneration.store(
                m_externalIdleContinuationGeneration.load(std::memory_order_acquire), std::memory_order_release);
        }
        m_resumed.store(true, std::memory_order_release);
        m_resumed.notify_one();
    }

    void Stop() noexcept
    {
        m_stopped.store(true, std::memory_order_release);
        m_resumed.store(true, std::memory_order_release);
        m_resumed.notify_one();
    }

private:
    std::atomic<bool> m_resumed{false};
    std::atomic<bool> m_stopped{false};
    std::atomic<Size> m_switchCount{0U};
    std::atomic<Size> m_destinationSwitchCount{0U};
    std::atomic<bool> m_prepared{false};
    std::atomic<UInt64> m_externalIdleContinuationGeneration{0U};
    std::atomic<UInt64> m_externalIdleContinuationLandingGeneration{0U};
    std::atomic<bool> m_externalIdleContinuationArmed{false};
    std::atomic<bool> m_rejectNextExternalIdleContinuation{false};
    std::atomic<bool> m_mismatchNextExternalIdleContinuation{false};
    SwitchHook m_nextSwitchHook{nullptr};
    void* m_nextSwitchHookContext{nullptr};
};

class RecordedBlockObserver final
{
public:
    explicit constexpr RecordedBlockObserver(const BlockedRunCommitOutcome outcome) noexcept : m_outcome{outcome}
    {
    }

    [[nodiscard]] BlockedRunCommitOutcome
    CommitBlockedFromFinishedRun(const Kernel::Scheduler::ThreadBlockToken token) noexcept
    {
        Token = token;
        ++Calls;
        return m_outcome;
    }

    Kernel::Scheduler::ThreadBlockToken Token{};
    Size Calls{0U};

private:
    BlockedRunCommitOutcome m_outcome;
};

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

class RegisteredPair final
{
public:
    RegisteredPair(ObjectRegistry& registry, RootCapabilityIssuer& issuer) noexcept
        : Context{registry}, m_issuer{issuer}
    {
        if ((ThreadStorage.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Context.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return;
        }
        const auto threadResult = m_issuer.CreateObject(ThreadStorage);
        const auto contextResult = m_issuer.CreateObject(Context);
        if (!threadResult.HasValue() || !contextResult.HasValue())
        {
            return;
        }
        ThreadObject = threadResult.GetValue();
        ContextObject = contextResult.GetValue();
        ThreadIdentity = ThreadId{registry, ThreadObject};
        ContextIdentity = SchedulingContextId{registry, ContextObject};
        Ready = (Context.Bind(ThreadIdentity) == SchedulingContextError::None) &&
                (ThreadStorage.MakeReady() == ThreadError::None);
    }

    ~RegisteredPair() noexcept
    {
        if (ThreadIdentity.IsValid())
        {
            static_cast<void>(Context.Unbind(ThreadIdentity));
        }
        if (ContextObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ContextObject));
        }
        if (ThreadObject.IsValid())
        {
            static_cast<void>(m_issuer.DestroyObject(ThreadObject));
        }
        for (Size index{0U}; index < 2U; ++index)
        {
            static_cast<void>(m_issuer.CollectDestroyedObject());
        }
    }

    RegisteredPair(const RegisteredPair&) = delete;
    RegisteredPair& operator=(const RegisteredPair&) = delete;
    RegisteredPair(RegisteredPair&&) = delete;
    RegisteredPair& operator=(RegisteredPair&&) = delete;

    Thread ThreadStorage;
    SchedulingContext Context;
    ObjectId ThreadObject{};
    ObjectId ContextObject{};
    ThreadId ThreadIdentity{};
    SchedulingContextId ContextIdentity{};
    bool Ready{false};

private:
    RootCapabilityIssuer& m_issuer;
};

class TerminalRunLoopFixture final
{
public:
    TerminalRunLoopFixture() noexcept
        : Registry{RegistryLock}, Issuer{Registry}, IdleProtection{RegistryLock, Registry, Issuer, Timer},
          DestinationProtection{RegistryLock, Registry, Issuer, Timer}, DeadlineQueue{Registry, Timer},
          Controller{Registry, DeadlineQueue, Timer, CpuId{0U}}, IdleContext{true}, DestinationContext{true},
          RunLoop{Controller, AddressSpaces, IdleContext}, Pair{Registry, Issuer}
    {
        const auto initialization = AddressSpaces.Initialize(CpuId{0U}, IdleProtection.AddressSpaceStorage);
        Ready = IdleProtection.Ready && DestinationProtection.Ready && Pair.Ready && initialization.HasValue();
    }

    [[nodiscard]] bool PrepareAdmission() noexcept
    {
        return Ready &&
               (RunLoop.Bind(Pair.ThreadStorage, DestinationContext, DestinationProtection.Domain) ==
                ProcessorKernelRunLoopError::None) &&
               Controller.Enqueue(Pair.ContextIdentity, Pair.Context, Pair.ThreadIdentity, Pair.ThreadStorage)
                   .HasValue();
    }

    TerminalRunLoopFixture(const TerminalRunLoopFixture&) = delete;
    TerminalRunLoopFixture& operator=(const TerminalRunLoopFixture&) = delete;
    TerminalRunLoopFixture(TerminalRunLoopFixture&&) = delete;
    TerminalRunLoopFixture& operator=(TerminalRunLoopFixture&&) = delete;

    SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    ManualLocalTimer Timer;
    RegisteredProtectionDomain IdleProtection;
    RegisteredProtectionDomain DestinationProtection;
    TimerQueue DeadlineQueue;
    ProcessorRunController Controller;
    FakeKernelContext IdleContext;
    FakeKernelContext DestinationContext;
    ProcessorAddressSpaceController AddressSpaces;
    ProcessorKernelRunLoop<FakeKernelContext> RunLoop;
    RegisteredPair Pair;
    bool Ready{false};
};

class ExternalRunLoopFixture final
{
public:
    explicit ExternalRunLoopFixture(
        const ProcessorRunDispatchMode mode = ProcessorRunDispatchMode::HardwareDeadline) noexcept
        : Registry{RegistryLock}, Issuer{Registry}, IdleProtection{RegistryLock, Registry, Issuer, Timer},
          TargetProtection{RegistryLock, Registry, Issuer, Timer}, DeadlineQueue{Registry, Timer},
          Controller{Registry, DeadlineQueue, Timer, CpuId{0U}, mode}, IdleContext{true}, TargetContext{true},
          RunLoop{Controller, AddressSpaces, IdleContext}, Pair{Registry, Issuer}
    {
        const auto initialization = AddressSpaces.Initialize(CpuId{0U}, IdleProtection.AddressSpaceStorage);
        Ready = IdleProtection.Ready && TargetProtection.Ready && Pair.Ready && initialization.HasValue();
    }

    [[nodiscard]] bool PrepareAdmission() noexcept
    {
        ProcessorKernelRunLoopFailure failure{};
        return Ready &&
               (RunLoop.Bind(Pair.ThreadStorage, TargetContext, TargetProtection.Domain) ==
                ProcessorKernelRunLoopError::None) &&
               RunLoop.EnqueueBoundThread(Pair.ContextIdentity, Pair.Context, Pair.ThreadIdentity, Pair.ThreadStorage,
                                          failure);
    }

    ExternalRunLoopFixture(const ExternalRunLoopFixture&) = delete;
    ExternalRunLoopFixture& operator=(const ExternalRunLoopFixture&) = delete;
    ExternalRunLoopFixture(ExternalRunLoopFixture&&) = delete;
    ExternalRunLoopFixture& operator=(ExternalRunLoopFixture&&) = delete;

    SpinLock RegistryLock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
    ManualLocalTimer Timer;
    RegisteredProtectionDomain IdleProtection;
    RegisteredProtectionDomain TargetProtection;
    TimerQueue DeadlineQueue;
    ProcessorRunController Controller;
    CooperativeKernelContext IdleContext;
    CooperativeKernelContext TargetContext;
    ProcessorAddressSpaceController AddressSpaces;
    ProcessorKernelRunLoop<CooperativeKernelContext> RunLoop;
    RegisteredPair Pair;
    bool Ready{false};
};

class RecordedFaultCommit final
{
public:
    explicit RecordedFaultCommit(const ProcessorAddressSpaceController* const addressSpaces = nullptr,
                                 const AddressSpace* const expectedActiveAddressSpace = nullptr) noexcept
        : m_addressSpaces{addressSpaces}, m_expectedActiveAddressSpace{expectedActiveAddressSpace}
    {
    }

    void CommitBlocked(const Kernel::Scheduler::ThreadBlockToken blockToken) noexcept
    {
        ++Calls;
        BlockToken = blockToken;
        if ((m_addressSpaces != nullptr) && (m_expectedActiveAddressSpace != nullptr))
        {
            const auto snapshot = m_addressSpaces->GetSnapshot();
            ExpectedAddressSpaceWasActive = (snapshot.State == ProcessorAddressSpaceControllerState::Ready) &&
                                            (snapshot.ActiveStorage == m_expectedActiveAddressSpace);
        }
    }

    Size Calls{0U};
    Kernel::Scheduler::ThreadBlockToken BlockToken{};
    bool ExpectedAddressSpaceWasActive{false};

private:
    const ProcessorAddressSpaceController* m_addressSpaces;
    const AddressSpace* m_expectedActiveAddressSpace;
};

[[nodiscard]] ProcessorRunExternalStopObservation MakeExternalStop(ProcessorRunController& controller,
                                                                   const UInt64 observedTicks,
                                                                   const UInt64 externalFrameToken = 1U) noexcept
{
    const auto active = controller.GetActiveExecutionBinding();
    const auto snapshot = controller.GetSnapshot();
    if (!active.HasValue())
    {
        return {};
    }
    return ProcessorRunExternalStopObservation{
        .Thread = active.GetValue().ThreadIdentity,
        .SchedulingContext = active.GetValue().SchedulingContextIdentity,
        .RunSequence = active.GetValue().RunSequence,
        .ProgrammedDeadlineTicks = snapshot.Deadline.SelectedDeadlineTicks,
        .ObservedTicks = observedTicks,
        .ExternalFrameToken = externalFrameToken,
        .Processor = active.GetValue().Processor,
        .HardwareState = ProcessorRunExternalStopHardwareState::TimerMaskedReadBackAndInterruptAcknowledged,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

[[nodiscard]] ProcessorRunExternalStopObservation MakePageFaultStop(ProcessorRunController& controller,
                                                                    const UInt64 externalFrameToken) noexcept
{
    const auto active = controller.GetActiveExecutionBinding();
    if (!active.HasValue())
    {
        return {};
    }
    return ProcessorRunExternalStopObservation{
        .Thread = active.GetValue().ThreadIdentity,
        .SchedulingContext = active.GetValue().SchedulingContextIdentity,
        .RunSequence = active.GetValue().RunSequence,
        .ProgrammedDeadlineTicks = 0U,
        .ObservedTicks = 0U,
        .ExternalFrameToken = externalFrameToken,
        .Processor = active.GetValue().Processor,
        .HardwareState = ProcessorRunExternalStopHardwareState::SynchronousPageFaultCaptured,
        .Reserved = {0U, 0U, 0U, 0U, 0U, 0U},
    };
}

[[nodiscard]] bool ExternalStopObservationsAreEqual(const ProcessorRunExternalStopObservation& left,
                                                    const ProcessorRunExternalStopObservation& right) noexcept
{
    return left.Thread.IsEqualTo(right.Thread) && left.SchedulingContext.IsEqualTo(right.SchedulingContext) &&
           (left.RunSequence == right.RunSequence) && (left.ProgrammedDeadlineTicks == right.ProgrammedDeadlineTicks) &&
           (left.ObservedTicks == right.ObservedTicks) && (left.ExternalFrameToken == right.ExternalFrameToken) &&
           left.Processor.IsValid() && right.Processor.IsValid() &&
           (left.Processor.GetValue() == right.Processor.GetValue()) && (left.HardwareState == right.HardwareState) &&
           (left.Reserved[0U] == right.Reserved[0U]) && (left.Reserved[1U] == right.Reserved[1U]) &&
           (left.Reserved[2U] == right.Reserved[2U]) && (left.Reserved[3U] == right.Reserved[3U]) &&
           (left.Reserved[4U] == right.Reserved[4U]) && (left.Reserved[5U] == right.Reserved[5U]);
}

[[nodiscard, gnu::noinline]] bool RetainExternalFrameAtIdle(ExternalRunLoopFixture& fixture,
                                                            const UInt64 frameToken) noexcept
{
    if (!fixture.PrepareAdmission())
    {
        return false;
    }
    std::atomic<bool> completionAccepted{false};
    std::thread worker{[&]() noexcept
                       {
                           if (fixture.TargetContext.WaitForResume() != FakeContextError::None)
                           {
                               fixture.IdleContext.Resume();
                               return;
                           }
                           completionAccepted.store(fixture.RunLoop
                                                        .CompleteExternallyStoppedRunAsReady(
                                                            fixture.Pair.ThreadStorage, fixture.TargetContext,
                                                            MakeExternalStop(fixture.Controller, 30U, frameToken))
                                                        .HasValue(),
                                                    std::memory_order_release);
                           fixture.IdleContext.Resume();
                       }};
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool returnedToIdle = fixture.RunLoop.DispatchFromIdle(receipt, failure);
    worker.join();
    const auto snapshot = fixture.RunLoop.GetSnapshot();
    return returnedToIdle && completionAccepted.load(std::memory_order_acquire) &&
           (receipt.State == ProcessorKernelRunLoopState::Idle) &&
           (receipt.ResumeKind == ProcessorKernelRunLoopResumeKind::None) &&
           (snapshot.State == ProcessorKernelRunLoopState::Idle) && (snapshot.ExternalFrameRetained == 1U) &&
           (snapshot.ExternalFrameResumePending == 0U);
}

void TestPageFaultFrameCompletionBlocksAndRetains(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) ExternalRunLoopFixture{};
    RecordedFaultCommit ingress{&fixture->AddressSpaces, &fixture->TargetProtection.AddressSpaceStorage};
    Kernel::Scheduler::DispatchCompletion completion{};
    std::atomic<bool> completionAccepted{false};
    const bool prepared = fixture->PrepareAdmission();
    if (!prepared)
    {
        ASTERIA_EXPECT(runner, "page-fault frame fixture prepares admission", false);
        return;
    }

    std::thread worker{[&]() noexcept
                       {
                           if (fixture->TargetContext.WaitForResume() != FakeContextError::None)
                           {
                               fixture->IdleContext.Resume();
                               return;
                           }
                           const auto result = fixture->RunLoop.CompletePageFaultedRunAsBlocked(
                               fixture->Pair.ThreadStorage, fixture->TargetContext,
                               MakePageFaultStop(fixture->Controller, 0xF1U), ingress);
                           if (result.HasValue())
                           {
                               completion = result.GetValue();
                               completionAccepted.store(true, std::memory_order_release);
                           }
                           fixture->IdleContext.Resume();
                       }};
    ProcessorKernelRunLoopReceipt resumeReceipt{};
    ProcessorKernelRunLoopFailure resumeFailure{};
    const bool returnedToIdle = fixture->RunLoop.DispatchFromIdle(resumeReceipt, resumeFailure);
    if (!returnedToIdle)
    {
        fixture->TargetContext.Stop();
    }
    worker.join();

    ProcessorKernelRunLoopReceipt emptyReceipt{};
    ProcessorKernelRunLoopFailure emptyFailure{};
    const bool dispatchedFromEmptyIdle = fixture->RunLoop.DispatchFromIdle(emptyReceipt, emptyFailure);
    const auto retained = fixture->RunLoop.GetRetainedExternalFrame(fixture->Pair.ThreadStorage);
    const auto runLoop = fixture->RunLoop.GetSnapshot();
    const auto controller = fixture->Controller.GetSnapshot();
    const auto addressSpace = fixture->AddressSpaces.GetSnapshot();
    const Size targetDestinationSwitches = fixture->TargetContext.GetDestinationSwitchCount();

    const ThreadError wakeResult = fixture->Pair.ThreadStorage.Wake(completion.Thread.BlockToken);
    ProcessorKernelRunLoopFailure enqueueFailure{};
    const bool reenqueued =
        (wakeResult == ThreadError::None) &&
        fixture->RunLoop.EnqueueBoundThread(fixture->Pair.ContextIdentity, fixture->Pair.Context,
                                            fixture->Pair.ThreadIdentity, fixture->Pair.ThreadStorage, enqueueFailure);
    fixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt externalReceipt{};
    ProcessorKernelRunLoopFailure externalFailure{};
    const bool externalSelected = reenqueued && fixture->RunLoop.DispatchFromIdle(externalReceipt, externalFailure);
    const auto selectedController = fixture->Controller.GetSnapshot();
    const auto selectedAddressSpace = fixture->AddressSpaces.GetSnapshot();
    ProcessorKernelRunLoopFailure refreshFailure{};
    const bool continuationRefreshed =
        externalSelected && fixture->RunLoop.RefreshExternalFrameIdleContinuation(externalReceipt, refreshFailure);
    ProcessorKernelRunLoopFailure commitFailure{};
    const bool frameCommitted = continuationRefreshed && fixture->RunLoop.CommitExternalFrameResume(
                                                             fixture->Pair.ThreadStorage, fixture->TargetContext,
                                                             externalReceipt, commitFailure);
    const auto afterCommit = fixture->RunLoop.GetSnapshot();
    const auto retainedAfterCommit = fixture->RunLoop.GetRetainedExternalFrame(fixture->Pair.ThreadStorage);

    ASTERIA_EXPECT(
        runner,
        "a captured page-fault frame blocks once, commits the exact token, "
        "then readmits through the retained-frame path without a cooperative "
        "target switch",
        returnedToIdle, completionAccepted.load(std::memory_order_acquire),
        completion.Thread.State == ThreadState::Blocked, completion.Thread.BlockToken.IsValid(), ingress.Calls == 1U,
        ingress.BlockToken.IsEqualTo(completion.Thread.BlockToken), ingress.ExpectedAddressSpaceWasActive,
        retained.HasValue(), retained.GetValue().ExternalFrameToken == 0xF1U,
        retained.GetValue().HardwareState == ProcessorRunExternalStopHardwareState::SynchronousPageFaultCaptured,
        retained.GetValue().ProgrammedDeadlineTicks == 0U, retained.GetValue().ObservedTicks == 0U,
        runLoop.State == ProcessorKernelRunLoopState::Idle, runLoop.ExternalFrameRetained == 1U,
        runLoop.ExternalFrameResumePending == 0U, controller.State == ProcessorRunState::Idle,
        fixture->IdleContext.GetSwitchCount() == 1U, fixture->TargetContext.GetSwitchCount() == 0U,
        addressSpace.State == ProcessorAddressSpaceControllerState::Ready,
        addressSpace.ActiveStorage == &fixture->IdleProtection.AddressSpaceStorage, !dispatchedFromEmptyIdle,
        emptyFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
        emptyFailure.ControllerError == ProcessorRunError::NoRunnableThread,
        emptyFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed, wakeResult == ThreadError::None,
        reenqueued, externalSelected, externalReceipt.ThreadStorage == &fixture->Pair.ThreadStorage,
        externalReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
        externalReceipt.ExternalFrameToken == 0xF1U,
        externalReceipt.StoppedRunSequence == completion.Thread.RunSequence,
        externalReceipt.AdmissionRunSequence == completion.Thread.RunSequence + 1U,
        externalReceipt.AdmissionDeadlineTicks != 0U, externalReceipt.ExternalFrameResumeGeneration != 0U,
        selectedController.State == ProcessorRunState::Active,
        selectedAddressSpace.ActiveStorage == &fixture->TargetProtection.AddressSpaceStorage, continuationRefreshed,
        frameCommitted, afterCommit.State == ProcessorKernelRunLoopState::Active,
        afterCommit.ExternalFrameRetained == 0U, afterCommit.ExternalFrameResumePending == 0U,
        !retainedAfterCommit.HasValue(),
        retainedAfterCommit.GetError() == ProcessorKernelRunLoopError::ExternalFrameNotRetained,
        fixture->TargetContext.GetDestinationSwitchCount() == targetDestinationSwitches);

    // The architecture restore that follows a successful common commit is not
    // executed by this hosted test. Static placement preserves that active
    // lifetime without invoking invalid retirement preconditions.
}

void TestPageFaultFrameRejectsCooperativeMode(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage))
        ExternalRunLoopFixture{ProcessorRunDispatchMode::CooperativeBootstrap};
    RecordedFaultCommit ingress;
    ProcessorKernelRunLoopFailure pageFaultFailure{};
    std::atomic<bool> pageFaultReturned{false};
    if (!fixture->PrepareAdmission())
    {
        ASTERIA_EXPECT(runner, "cooperative page-fault fixture prepares admission", false);
        return;
    }

    std::thread worker{[&]() noexcept
                       {
                           if (fixture->TargetContext.WaitForResume() != FakeContextError::None)
                           {
                               fixture->IdleContext.Resume();
                               return;
                           }
                           const auto result = fixture->RunLoop.CompletePageFaultedRunAsBlocked(
                               fixture->Pair.ThreadStorage, fixture->TargetContext,
                               MakePageFaultStop(fixture->Controller, 0xF2U), ingress);
                           if (!result.HasValue())
                           {
                               pageFaultFailure = result.GetError();
                           }
                           pageFaultReturned.store(true, std::memory_order_release);
                           fixture->IdleContext.Resume();
                       }};
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure dispatchFailure{};
    const bool dispatch = fixture->RunLoop.DispatchFromIdle(receipt, dispatchFailure);
    if (!pageFaultReturned.load(std::memory_order_acquire))
    {
        fixture->TargetContext.Stop();
    }
    worker.join();

    const auto retained = fixture->RunLoop.GetRetainedExternalFrame(fixture->Pair.ThreadStorage);
    const auto runLoop = fixture->RunLoop.GetSnapshot();
    const auto controller = fixture->Controller.GetSnapshot();
    const auto addressSpace = fixture->AddressSpaces.GetSnapshot();

    ASTERIA_EXPECT(runner,
                   "a cooperative controller rejects page-fault settlement while "
                   "preserving the captured frame as terminal ownership evidence",
                   !dispatch, pageFaultReturned.load(std::memory_order_acquire),
                   pageFaultFailure.Error == ProcessorKernelRunLoopError::ExternalFrameOwnershipMismatch,
                   dispatchFailure.Error == pageFaultFailure.Error, ingress.Calls == 0U, retained.HasValue(),
                   retained.GetValue().ExternalFrameToken == 0xF2U,
                   retained.GetValue().HardwareState ==
                       ProcessorRunExternalStopHardwareState::SynchronousPageFaultCaptured,
                   runLoop.State == ProcessorKernelRunLoopState::Faulted, runLoop.ExternalFrameRetained == 1U,
                   controller.State == ProcessorRunState::Active,
                   controller.Mode == ProcessorRunDispatchMode::CooperativeBootstrap,
                   fixture->Pair.ThreadStorage.GetSnapshot().State == ThreadState::Running,
                   fixture->TargetContext.GetSwitchCount() == 0U,
                   addressSpace.ActiveStorage == &fixture->TargetProtection.AddressSpaceStorage);

    // The controller's active run and the target-owned capture are terminal
    // evidence, so this placement fixture is intentionally not destroyed.
}

template <typename TMutation>
[[nodiscard]] bool PendingExternalMutationIsTerminal(ExternalRunLoopFixture& fixture, const UInt64 frameToken,
                                                     const TMutation& mutation) noexcept
{
    if (!fixture.PrepareAdmission())
    {
        return false;
    }
    std::thread worker{[&]() noexcept
                       {
                           if (fixture.TargetContext.WaitForResume() != FakeContextError::None)
                           {
                               fixture.IdleContext.Resume();
                               return;
                           }
                           static_cast<void>(fixture.RunLoop.CompleteExternallyStoppedRunAsReady(
                               fixture.Pair.ThreadStorage, fixture.TargetContext,
                               MakeExternalStop(fixture.Controller, 30U, frameToken)));
                           fixture.IdleContext.Resume();
                       }};
    ProcessorKernelRunLoopReceipt idleReceipt{};
    ProcessorKernelRunLoopFailure idleFailure{};
    const bool reachedIdle = fixture.RunLoop.DispatchFromIdle(idleReceipt, idleFailure);
    worker.join();
    fixture.Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt resume{};
    ProcessorKernelRunLoopFailure resumeFailure{};
    const bool selected = fixture.RunLoop.DispatchFromIdle(resume, resumeFailure);
    const auto controllerBefore = fixture.Controller.GetSnapshot();
    const auto addressBefore = fixture.AddressSpaces.GetSnapshot();
    const Size reads = fixture.Timer.GetReadCalls();
    const Size programs = fixture.Timer.GetProgramCalls();
    const Size cancels = fixture.Timer.GetCancelCalls();
    const Size acknowledgements = fixture.Timer.GetAcknowledgeCalls();
    const Size switches = fixture.TargetContext.GetSwitchCount();
    ProcessorKernelRunLoopFailure mutationFailure{};
    const bool mutationSucceeded = mutation(fixture, resume, mutationFailure);
    const auto controllerAfter = fixture.Controller.GetSnapshot();
    const auto addressAfter = fixture.AddressSpaces.GetSnapshot();
    const auto runLoopAfter = fixture.RunLoop.GetSnapshot();

    return reachedIdle && selected && (resume.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame) &&
           !mutationSucceeded && (mutationFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumePending) &&
           (runLoopAfter.State == ProcessorKernelRunLoopState::Faulted) && (runLoopAfter.ExternalFrameRetained == 1U) &&
           (runLoopAfter.ExternalFrameResumePending == 1U) && (controllerAfter.State == ProcessorRunState::Active) &&
           controllerAfter.Scheduler.ActiveThread.IsEqualTo(fixture.Pair.ThreadIdentity) &&
           (controllerAfter.Scheduler.Queue.ReadyEntries == controllerBefore.Scheduler.Queue.ReadyEntries) &&
           (controllerAfter.Scheduler.Queue.NextSequence == controllerBefore.Scheduler.Queue.NextSequence) &&
           (controllerAfter.Deadline.SelectedDeadlineTicks == controllerBefore.Deadline.SelectedDeadlineTicks) &&
           (addressAfter.ActiveStorage == addressBefore.ActiveStorage) &&
           addressAfter.Active.IsEqualTo(addressBefore.Active) && (fixture.Timer.GetReadCalls() == reads) &&
           (fixture.Timer.GetProgramCalls() == programs) && (fixture.Timer.GetCancelCalls() == cancels) &&
           (fixture.Timer.GetAcknowledgeCalls() == acknowledgements) &&
           (fixture.TargetContext.GetSwitchCount() == switches);
}

template <typename TPreparation>
[[nodiscard, gnu::noinline]] bool
ExternalStopFailureRetainsFrame(ExternalRunLoopFixture& fixture, const UInt64 frameToken,
                                const ProcessorRunExternalStopError expectedExternalStop,
                                const TPreparation& preparation) noexcept
{
    if (!fixture.PrepareAdmission())
    {
        return false;
    }
    bool preparationAccepted{false};
    bool completionAccepted{false};
    Size readsBeforeCompletion{0U};
    ProcessorRunExternalStopObservation submittedObservation{};
    ProcessorKernelRunLoopFailure stopFailure{};
    std::thread worker{[&]() noexcept
                       {
                           if (fixture.TargetContext.WaitForResume() != FakeContextError::None)
                           {
                               fixture.IdleContext.Resume();
                               return;
                           }
                           ProcessorRunExternalStopObservation observation =
                               MakeExternalStop(fixture.Controller, 30U, frameToken);
                           preparationAccepted = preparation(fixture, observation);
                           submittedObservation = observation;
                           readsBeforeCompletion = fixture.Timer.GetReadCalls();
                           const auto completion = fixture.RunLoop.CompleteExternallyStoppedRunAsReady(
                               fixture.Pair.ThreadStorage, fixture.TargetContext, observation);
                           completionAccepted = completion.HasValue();
                           if (!completionAccepted)
                           {
                               stopFailure = completion.GetError();
                           }
                           fixture.IdleContext.Resume();
                       }};
    ProcessorKernelRunLoopReceipt dispatchReceipt{};
    ProcessorKernelRunLoopFailure dispatchFailure{};
    const bool dispatch = fixture.RunLoop.DispatchFromIdle(dispatchReceipt, dispatchFailure);
    worker.join();
    const auto retained = fixture.RunLoop.GetRetainedExternalFrame(fixture.Pair.ThreadStorage);
    const auto beforeRemoval = fixture.Controller.GetSnapshot();
    const auto address = fixture.AddressSpaces.GetSnapshot();
    const auto runLoop = fixture.RunLoop.GetSnapshot();
    const Size programs = fixture.Timer.GetProgramCalls();
    const Size cancels = fixture.Timer.GetCancelCalls();
    const Size acknowledgements = fixture.Timer.GetAcknowledgeCalls();
    ReadyQueueState removalState{};
    ProcessorKernelRunLoopFailure removalFailure{};
    const bool removed = fixture.RunLoop.RemoveBoundThread(fixture.Pair.ThreadStorage, removalState, removalFailure);
    const auto afterRemoval = fixture.Controller.GetSnapshot();
    const auto unbound = fixture.RunLoop.Unbind(fixture.Pair.ThreadStorage);

    return preparationAccepted && !completionAccepted && !dispatch &&
           (stopFailure.Error == ProcessorKernelRunLoopError::ControllerRejected) &&
           (stopFailure.ControllerError == ProcessorRunError::ExternalStopRejected) &&
           (stopFailure.ExternalStop == expectedExternalStop) && retained.HasValue() &&
           ExternalStopObservationsAreEqual(retained.GetValue(), submittedObservation) &&
           (runLoop.State == ProcessorKernelRunLoopState::Faulted) && (runLoop.ExternalFrameRetained == 1U) &&
           (beforeRemoval.State == ProcessorRunState::Faulted) &&
           (beforeRemoval.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Active) &&
           beforeRemoval.Scheduler.ActiveThread.IsEqualTo(fixture.Pair.ThreadIdentity) &&
           (address.ActiveStorage == &fixture.TargetProtection.AddressSpaceStorage) &&
           (fixture.TargetContext.GetDestinationSwitchCount() == 1U) &&
           (fixture.Timer.GetReadCalls() == readsBeforeCompletion) && !removed &&
           (fixture.Timer.GetProgramCalls() == programs) && (fixture.Timer.GetCancelCalls() == cancels) &&
           (fixture.Timer.GetAcknowledgeCalls() == acknowledgements) &&
           (afterRemoval.Scheduler.Queue.ReadyEntries == beforeRemoval.Scheduler.Queue.ReadyEntries) &&
           (afterRemoval.Scheduler.Queue.NextSequence == beforeRemoval.Scheduler.Queue.NextSequence) &&
           (unbound == ProcessorKernelRunLoopError::NotIdle);
}

template <typename TConfiguration>
[[nodiscard, gnu::noinline]] bool
ExternalContinuationRefreshFailureRetainsFrame(ExternalRunLoopFixture& fixture, const UInt64 frameToken,
                                               const TConfiguration& configuration) noexcept
{
    if (!RetainExternalFrameAtIdle(fixture, frameToken))
    {
        return false;
    }
    fixture.Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure admissionFailure{};
    if (!fixture.RunLoop.DispatchFromIdle(receipt, admissionFailure))
    {
        return false;
    }
    configuration(fixture.IdleContext);
    const auto controllerBefore = fixture.Controller.GetSnapshot();
    const auto addressBefore = fixture.AddressSpaces.GetSnapshot();
    const Size reads = fixture.Timer.GetReadCalls();
    const Size programs = fixture.Timer.GetProgramCalls();
    const Size cancels = fixture.Timer.GetCancelCalls();
    const Size acknowledgements = fixture.Timer.GetAcknowledgeCalls();
    const Size switches = fixture.TargetContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopFailure refreshFailure{};
    const bool refreshed = fixture.RunLoop.RefreshExternalFrameIdleContinuation(receipt, refreshFailure);
    const auto retained = fixture.RunLoop.GetRetainedExternalFrame(fixture.Pair.ThreadStorage);
    const auto controllerAfter = fixture.Controller.GetSnapshot();
    const auto addressAfter = fixture.AddressSpaces.GetSnapshot();
    const auto runLoopAfter = fixture.RunLoop.GetSnapshot();

    return !refreshed && (refreshFailure.Error == ProcessorKernelRunLoopError::ExternalFrameContinuationRejected) &&
           retained.HasValue() && (retained.GetValue().ExternalFrameToken == frameToken) &&
           (runLoopAfter.State == ProcessorKernelRunLoopState::Faulted) && (runLoopAfter.ExternalFrameRetained == 1U) &&
           (runLoopAfter.ExternalFrameResumePending == 1U) && (controllerAfter.State == ProcessorRunState::Active) &&
           controllerAfter.Scheduler.ActiveThread.IsEqualTo(fixture.Pair.ThreadIdentity) &&
           (controllerAfter.Scheduler.Queue.ReadyEntries == controllerBefore.Scheduler.Queue.ReadyEntries) &&
           (controllerAfter.Scheduler.Queue.NextSequence == controllerBefore.Scheduler.Queue.NextSequence) &&
           (controllerAfter.Deadline.SelectedDeadlineTicks == controllerBefore.Deadline.SelectedDeadlineTicks) &&
           (addressAfter.ActiveStorage == addressBefore.ActiveStorage) &&
           addressAfter.Active.IsEqualTo(addressBefore.Active) && (fixture.Timer.GetReadCalls() == reads) &&
           (fixture.Timer.GetProgramCalls() == programs) && (fixture.Timer.GetCancelCalls() == cancels) &&
           (fixture.Timer.GetAcknowledgeCalls() == acknowledgements) &&
           (fixture.TargetContext.GetDestinationSwitchCount() == switches);
}

void TestCooperativeBootstrapWorker(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair pair{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    const bool setup =
        protection.Ready && pair.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(pair.ThreadStorage, workerContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    std::atomic<bool> activeBindingExact{false};

    std::thread worker{
        [&]() noexcept
        {
            if (workerContext.WaitForResume() == FakeContextError::Rejected)
            {
                idleContext.Resume();
                return;
            }
            const auto activeBinding = runLoop->GetActiveExecutionBinding();
            activeBindingExact.store(
                activeBinding.HasValue() && activeBinding.GetValue().ThreadIdentity.IsEqualTo(pair.ThreadIdentity) &&
                    (activeBinding.GetValue().ThreadStorage == &pair.ThreadStorage) &&
                    activeBinding.GetValue().SchedulingContextIdentity.IsEqualTo(pair.ContextIdentity) &&
                    (activeBinding.GetValue().SchedulingContextStorage == &pair.Context) &&
                    (activeBinding.GetValue().Processor.GetValue() == 0U) &&
                    (activeBinding.GetValue().RunSequence == pair.ThreadStorage.GetSnapshot().RunSequence),
                std::memory_order_release);
            ProcessorKernelRunLoopReceipt workerReceipt{};
            ProcessorKernelRunLoopFailure workerFailure{};
            static_cast<void>(runLoop->FinishAndDispatch(
                pair.ThreadStorage, workerContext, ThreadRunDisposition::Exited, 37U, workerReceipt, workerFailure));
            idleContext.Resume();
        }};
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop->DispatchFromIdle(receipt, failure);
    const auto controllerSnapshot = controller.GetSnapshot();
    const auto runLoopSnapshot = runLoop->GetSnapshot();
    const auto threadSnapshot = pair.ThreadStorage.GetSnapshot();
    const auto unbound = runLoop->Unbind(pair.ThreadStorage);

    ASTERIA_EXPECT(runner, "one BSP bootstrap worker exits cooperatively without timer hardware", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::ControllerRejected,
                   failure.ControllerError == ProcessorRunError::NoRunnableThread,
                   failure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
                   failure.DeadlineReceipt.SelectedDeadlineTicks == 0U, failure.DeadlineReceipt.SelectedSources == 0U,
                   failure.DeadlineReceipt.Reprogrammed == 0U, controllerSnapshot.State == ProcessorRunState::Idle,
                   controllerSnapshot.Mode == ProcessorRunDispatchMode::CooperativeBootstrap,
                   controllerSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed,
                   runLoopSnapshot.State == ProcessorKernelRunLoopState::Idle,
                   threadSnapshot.State == ThreadState::Exited, threadSnapshot.ExitCode == 37U,
                   activeBindingExact.load(std::memory_order_acquire), timer.GetProgramCalls() == 0U,
                   timer.GetCancelCalls() == 0U, unbound == ProcessorKernelRunLoopError::None);

    workerContext.Stop();
    worker.join();
    // Stopping the hosted continuation retires the exited worker after the
    // verified idle return. The placement run loop is intentionally not
    // destroyed because that stop makes its suspended frame terminal.
}

void TestWaitCommitRetryableStopsDispatch(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair pair{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    RecordedBlockObserver observer{BlockedRunCommitOutcome::RetryableRejected};
    std::atomic<bool> waitResumed{false};
    const bool setup =
        protection.Ready && pair.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(pair.ThreadStorage, workerContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();

    std::thread worker{
        [&]() noexcept
        {
            if (workerContext.WaitForResume() == FakeContextError::Rejected)
            {
                return;
            }
            ProcessorKernelRunLoopReceipt waitReceipt{};
            ProcessorKernelRunLoopFailure waitFailure{};
            const bool waited =
                runLoop->FinishWaitAndDispatch(pair.ThreadStorage, workerContext, observer, waitReceipt, waitFailure);
            waitResumed.store(waited && (waitReceipt.State == ProcessorKernelRunLoopState::Active),
                              std::memory_order_release);
            if (!waited)
            {
                return;
            }
            ProcessorKernelRunLoopReceipt exitReceipt{};
            ProcessorKernelRunLoopFailure exitFailure{};
            static_cast<void>(runLoop->FinishAndDispatch(pair.ThreadStorage, workerContext,
                                                         ThreadRunDisposition::Exited, 51U, exitReceipt, exitFailure));
        }};

    ProcessorKernelRunLoopReceipt firstReceipt{};
    ProcessorKernelRunLoopFailure firstFailure{};
    const bool firstDispatch = runLoop->DispatchFromIdle(firstReceipt, firstFailure);
    const auto firstRunLoop = runLoop->GetSnapshot();
    const auto firstController = controller.GetSnapshot();
    const ThreadError wake = pair.ThreadStorage.Wake(observer.Token);
    const auto enqueued =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    ProcessorKernelRunLoopReceipt secondReceipt{};
    ProcessorKernelRunLoopFailure secondFailure{};
    const bool secondDispatch = runLoop->DispatchFromIdle(secondReceipt, secondFailure);
    const auto afterWake = pair.ThreadStorage.GetSnapshot();
    const auto unbound = runLoop->Unbind(pair.ThreadStorage);

    ASTERIA_EXPECT(runner, "retryable wait commitment stops dispatch until explicit wake retry", setup, !firstDispatch,
                   firstFailure.Error == ProcessorKernelRunLoopError::CompletionObserverRetryableRejected,
                   firstFailure.BlockedRunCommit == BlockedRunCommitOutcome::RetryableRejected, observer.Calls == 1U,
                   observer.Token.IsValid(), firstRunLoop.State == ProcessorKernelRunLoopState::Idle,
                   firstController.State == ProcessorRunState::Idle, wake == ThreadError::None, enqueued.HasValue(),
                   !secondDispatch, secondFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   waitResumed.load(std::memory_order_acquire), afterWake.State == ThreadState::Exited,
                   afterWake.ExitCode == 51U, unbound == ProcessorKernelRunLoopError::None);

    workerContext.Stop();
    worker.join();
    // Stopping the hosted exited continuation intentionally retires this
    // placement run loop without invoking its no-saved-continuation destructor.
}

void TestSynchronousCallReferenceRetryUsesIdlePoll(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock, 4U};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair pair{registry, issuer};
    CapabilitySpace serverSpace{registry, protection.Revocations, timer, 0x2468'ACE0'1357'9BDFULL,
                                0xFDB9'7531'0ECA'8642ULL};
    Endpoint callEndpoint{serverSpace, ResourceDomainId{8U}};
    Endpoint replyEndpoint{protection.Capabilities, ResourceDomainId{7U}};
    ReadyQueue wakeQueue{registry, CpuId{0U}};
    ReplyToken token;

    ObjectId callEndpointObject{};
    ObjectId replyEndpointObject{};
    ObjectId tokenObject{};
    CapabilityHandle callEndpointCapability{};
    CapabilityHandle tokenCapability{};
    const auto endpointDomain = protection.Revocations.CreateRoot();
    const bool tokenConfigured = token.Configure(ResourceDomainId{7U}).Error == ReplyTokenError::None;
    const auto callEndpointRegistration = issuer.CreateObject(callEndpoint);
    const auto replyEndpointRegistration = issuer.CreateObject(replyEndpoint);
    const auto tokenRegistration = issuer.CreateObject(token);
    bool objectsReady = endpointDomain.HasValue() && tokenConfigured && callEndpointRegistration.HasValue() &&
                        replyEndpointRegistration.HasValue() && tokenRegistration.HasValue();
    if (objectsReady)
    {
        callEndpointObject = callEndpointRegistration.GetValue();
        replyEndpointObject = replyEndpointRegistration.GetValue();
        tokenObject = tokenRegistration.GetValue();
        const auto callAuthority =
            issuer.Mint(protection.Capabilities, callEndpointObject, CapabilityRights{CapabilityRight::Invoke},
                        endpointDomain.GetValue(), MonotonicTime::Infinite(), 0xCA11'E11DU);
        const auto tokenAuthority =
            issuer.Mint(protection.Capabilities, tokenObject,
                        CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer),
                        endpointDomain.GetValue(), MonotonicTime::Infinite(), 0xCA11'70C0U);
        objectsReady = callAuthority.HasValue() && tokenAuthority.HasValue();
        if (objectsReady)
        {
            callEndpointCapability = callAuthority.GetValue();
            tokenCapability = tokenAuthority.GetValue();
        }
    }

    SynchronousReplyDispatcher replyDispatcher{registry, replyEndpoint, wakeQueue};
    SynchronousCallDispatcher callDispatcher{registry, callEndpoint, token, tokenObject, replyDispatcher};
    MessageEnvelope call{};
    call.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    call.TransactionIdentifier = 0x8899'AABB'CCDD'EEFFULL;
    call.Deadline = MonotonicTime::Infinite();
    call.MethodIdentifier = 3U;
    call.InterfaceVersionMajor = 1U;
    call.FixedPayloadSizeBytes = 1U;
    call.PayloadSizeBytes = 1U;
    call.Kind = MessageKind::SynchronousCall;
    call.Cancellation = CancellationState::None;
    call.Payload[0U] = 0x5AU;

    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    ObjectPin firstTokenPressure;
    ObjectPin secondTokenPressure;
    ObjectPin thirdTokenPressure;
    SynchronousCallDispatcherError prepareError{SynchronousCallDispatcherError::InvalidState};
    ObjectPinError firstPinError{ObjectPinError::InvalidArgument};
    ObjectPinError secondPinError{ObjectPinError::InvalidArgument};
    ObjectPinError thirdPinError{ObjectPinError::InvalidArgument};
    std::atomic<bool> waitResumed{false};
    ProcessorKernelRunLoopFailure setupEnqueueFailure{};
    const bool setup =
        protection.Ready && pair.Ready && addressInitialization.HasValue() && objectsReady &&
        (runLoop->Bind(pair.ThreadStorage, workerContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        runLoop->EnqueueBoundThread(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage,
                                    setupEnqueueFailure);

    std::thread worker{
        [&]() noexcept
        {
            if (workerContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            const SynchronousCallPrepareRequest request{
                .CallerSpace = &protection.Capabilities,
                .Call = &call,
                .Caller = pair.ThreadIdentity,
                .SchedulingContext = pair.ContextIdentity,
                .CallEndpointCapability = callEndpointCapability,
                .ReplyTokenCapability = tokenCapability,
                .Sender = ResourceDomainId{7U},
            };
            prepareError = callDispatcher.Prepare(request).Error;
            firstPinError = registry.PinObject(tokenObject, ObjectType::ReplyToken, firstTokenPressure);
            secondPinError = registry.PinObject(tokenObject, ObjectType::ReplyToken, secondTokenPressure);
            thirdPinError = registry.PinObject(tokenObject, ObjectType::ReplyToken, thirdTokenPressure);
            ProcessorKernelRunLoopReceipt waitReceipt{};
            ProcessorKernelRunLoopFailure waitFailure{};
            const bool waited = runLoop->FinishWaitAndDispatch(pair.ThreadStorage, workerContext, callDispatcher,
                                                               waitReceipt, waitFailure);
            waitResumed.store(waited && (waitReceipt.State == ProcessorKernelRunLoopState::Active),
                              std::memory_order_release);
            if (!waited)
            {
                return;
            }
            ProcessorKernelRunLoopReceipt exitReceipt{};
            ProcessorKernelRunLoopFailure exitFailure{};
            static_cast<void>(runLoop->FinishAndDispatch(pair.ThreadStorage, workerContext,
                                                         ThreadRunDisposition::Exited, 73U, exitReceipt, exitFailure));
        }};

    ProcessorKernelRunLoopReceipt firstReceipt{};
    ProcessorKernelRunLoopFailure firstFailure{};
    const bool firstDispatch = runLoop->DispatchFromIdle(firstReceipt, firstFailure);
    const ProcessorKernelRunLoopSnapshot runLoopAtRetry = runLoop->GetSnapshot();
    const auto retained = callDispatcher.GetSnapshot();
    firstTokenPressure.Reset();
    secondTokenPressure.Reset();
    thirdTokenPressure.Reset();
    const auto progressed = callDispatcher.Poll();
    MessageEnvelope receivedCall{};
    const auto received = callEndpoint.Receive(receivedCall);
    CapabilityHandle serverTokenCapability{};
    if (received.HasValue() && (receivedCall.AttachmentCount == 1U))
    {
        serverTokenCapability = receivedCall.Attachments[0U].Handle;
    }
    const auto cancelled = replyDispatcher.Cancel();
    const ReadyQueueError removed = wakeQueue.Remove(pair.ThreadIdentity);
    ProcessorKernelRunLoopFailure reenqueueFailure{};
    const bool enqueued = runLoop->EnqueueBoundThread(pair.ContextIdentity, pair.Context, pair.ThreadIdentity,
                                                      pair.ThreadStorage, reenqueueFailure);
    ProcessorKernelRunLoopReceipt secondReceipt{};
    ProcessorKernelRunLoopFailure secondFailure{};
    const bool secondDispatch = runLoop->DispatchFromIdle(secondReceipt, secondFailure);
    const auto threadAfter = pair.ThreadStorage.GetSnapshot();
    const auto unbound = runLoop->Unbind(pair.ThreadStorage);

    ASTERIA_EXPECT(
        runner, "real run-loop retryable observer returns to idle for phase-safe Poll and readmission", setup,
        !firstDispatch, firstFailure.Error == ProcessorKernelRunLoopError::CompletionObserverRetryableRejected,
        firstFailure.BlockedRunCommit == BlockedRunCommitOutcome::RetryableRejected,
        runLoopAtRetry.State == ProcessorKernelRunLoopState::Idle, prepareError == SynchronousCallDispatcherError::None,
        firstPinError == ObjectPinError::Ok, secondPinError == ObjectPinError::Ok, thirdPinError == ObjectPinError::Ok,
        retained.State == SynchronousCallDispatcherState::ReplyInitializationPending,
        progressed.Error == SynchronousCallDispatcherError::None, received.HasValue(),
        receivedCall.TransactionIdentifier == call.TransactionIdentifier, serverTokenCapability.IsValid(),
        cancelled.HasValue(), removed == ReadyQueueError::None, enqueued, !secondDispatch,
        waitResumed.load(std::memory_order_acquire), threadAfter.State == ThreadState::Exited,
        threadAfter.ExitCode == 73U, unbound == ProcessorKernelRunLoopError::None);

    workerContext.Stop();
    worker.join();
    bool cleaned = serverTokenCapability.IsValid() && (serverSpace.Close(serverTokenCapability) == CapabilityError::Ok);
    cleaned = (protection.Capabilities.Close(callEndpointCapability) == CapabilityError::Ok) && cleaned;
    cleaned = (issuer.DestroyObject(tokenObject) == Kernel::Object::ObjectRegistryError::Ok) && cleaned;
    cleaned = (issuer.DestroyObject(replyEndpointObject) == Kernel::Object::ObjectRegistryError::Ok) && cleaned;
    cleaned = (issuer.DestroyObject(callEndpointObject) == Kernel::Object::ObjectRegistryError::Ok) && cleaned;
    for (Size index{0U}; index < 3U; ++index)
    {
        cleaned = issuer.CollectDestroyedObject().HasValue() && cleaned;
    }
    ASTERIA_EXPECT(runner, "run-loop synchronous Call fixture releases endpoint and token authority", cleaned,
                   token.GetSnapshot().State == ReplyTokenState::Cancelled);

    // Stop retires the hosted post-exit continuation; the placement run loop
    // intentionally keeps that no-longer-resumable frame out of its destructor.
}

void TestRepeatedBlockedWakeReadmission(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair pair{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    const bool setup =
        protection.Ready && pair.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(pair.ThreadStorage, workerContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    std::atomic<UInt64> firstRunSequence{0U};
    std::atomic<UInt64> secondRunSequence{0U};
    std::atomic<UInt64> thirdRunSequence{0U};
    std::atomic<bool> firstReadmission{false};
    std::atomic<bool> secondReadmission{false};

    std::thread worker{
        [&]() noexcept
        {
            if (workerContext.WaitForResume() == FakeContextError::Rejected)
            {
                idleContext.Resume();
                return;
            }
            firstRunSequence.store(pair.ThreadStorage.GetSnapshot().RunSequence, std::memory_order_release);

            ProcessorKernelRunLoopReceipt firstReceipt{};
            ProcessorKernelRunLoopFailure firstFailure{};
            const bool firstResumed = runLoop->FinishAndDispatch(
                pair.ThreadStorage, workerContext, ThreadRunDisposition::Blocked, 0U, firstReceipt, firstFailure);
            firstReadmission.store(firstResumed && (firstReceipt.ThreadStorage == &pair.ThreadStorage) &&
                                       (firstReceipt.State == ProcessorKernelRunLoopState::Active),
                                   std::memory_order_release);
            if (!firstResumed)
            {
                idleContext.Resume();
                return;
            }
            secondRunSequence.store(pair.ThreadStorage.GetSnapshot().RunSequence, std::memory_order_release);

            ProcessorKernelRunLoopReceipt secondReceipt{};
            ProcessorKernelRunLoopFailure secondFailure{};
            const bool secondResumed = runLoop->FinishAndDispatch(
                pair.ThreadStorage, workerContext, ThreadRunDisposition::Blocked, 0U, secondReceipt, secondFailure);
            secondReadmission.store(secondResumed && (secondReceipt.ThreadStorage == &pair.ThreadStorage) &&
                                        (secondReceipt.State == ProcessorKernelRunLoopState::Active),
                                    std::memory_order_release);
            if (!secondResumed)
            {
                idleContext.Resume();
                return;
            }
            thirdRunSequence.store(pair.ThreadStorage.GetSnapshot().RunSequence, std::memory_order_release);

            ProcessorKernelRunLoopReceipt exitReceipt{};
            ProcessorKernelRunLoopFailure exitFailure{};
            static_cast<void>(runLoop->FinishAndDispatch(pair.ThreadStorage, workerContext,
                                                         ThreadRunDisposition::Exited, 73U, exitReceipt, exitFailure));
            idleContext.Resume();
        }};

    const auto expectedIdleReturn = [](const ProcessorKernelRunLoopFailure& failure) noexcept
    {
        return (failure.Error == ProcessorKernelRunLoopError::ControllerRejected) &&
               (failure.ControllerError == ProcessorRunError::NoRunnableThread) &&
               (failure.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread) &&
               (failure.DeadlineError == Kernel::Time::ProcessorDeadlineError::None) &&
               (failure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed) &&
               (failure.DeadlineReceipt.SelectedDeadlineTicks == 0U) &&
               (failure.DeadlineReceipt.SelectedSources == 0U) && (failure.DeadlineReceipt.Reprogrammed == 1U) &&
               (failure.ActiveRunRetained == 0U) && (failure.NativeError == 0U);
    };

    ProcessorKernelRunLoopReceipt firstIdleReceipt{};
    ProcessorKernelRunLoopFailure firstIdleFailure{};
    const bool firstDispatch = runLoop->DispatchFromIdle(firstIdleReceipt, firstIdleFailure);
    const auto firstBlocked = pair.ThreadStorage.GetSnapshot();
    const auto firstIdleRunLoop = runLoop->GetSnapshot();
    const auto firstIdleController = controller.GetSnapshot();
    const auto firstBlockToken = firstBlocked.BlockToken;
    const ThreadError firstWake = pair.ThreadStorage.Wake(firstBlockToken);
    const auto firstReenqueue =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);

    ProcessorKernelRunLoopReceipt secondIdleReceipt{};
    ProcessorKernelRunLoopFailure secondIdleFailure{};
    const bool secondDispatch = runLoop->DispatchFromIdle(secondIdleReceipt, secondIdleFailure);
    const auto secondBlocked = pair.ThreadStorage.GetSnapshot();
    const auto secondIdleRunLoop = runLoop->GetSnapshot();
    const auto secondIdleController = controller.GetSnapshot();
    const auto secondBlockToken = secondBlocked.BlockToken;
    const ThreadError secondWake = pair.ThreadStorage.Wake(secondBlockToken);
    const auto secondReenqueue =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);

    ProcessorKernelRunLoopReceipt finalIdleReceipt{};
    ProcessorKernelRunLoopFailure finalIdleFailure{};
    const bool finalDispatch = runLoop->DispatchFromIdle(finalIdleReceipt, finalIdleFailure);
    const auto finalThread = pair.ThreadStorage.GetSnapshot();
    const auto finalContext = pair.Context.GetSnapshot();
    const auto finalRunLoop = runLoop->GetSnapshot();
    const auto finalController = controller.GetSnapshot();

    ASTERIA_EXPECT(runner, "the first block returns idle and its exact token republishes the worker", setup,
                   !firstDispatch, expectedIdleReturn(firstIdleFailure), firstBlocked.State == ThreadState::Blocked,
                   firstBlocked.RunSequence == 1U, firstBlockToken.IsValid(),
                   firstIdleRunLoop.State == ProcessorKernelRunLoopState::Idle,
                   firstIdleRunLoop.IdleFailurePending == 0U, firstIdleController.State == ProcessorRunState::Idle,
                   firstWake == ThreadError::None, firstReenqueue.HasValue(), firstReenqueue.GetValue().Sequence == 2U);

    ASTERIA_EXPECT(
        runner, "the second block advances its token and permits an independent readmission", !secondDispatch,
        expectedIdleReturn(secondIdleFailure), firstReadmission.load(std::memory_order_acquire),
        secondBlocked.State == ThreadState::Blocked, secondBlocked.RunSequence == 2U, secondBlockToken.IsValid(),
        !secondBlockToken.IsEqualTo(firstBlockToken), secondIdleRunLoop.State == ProcessorKernelRunLoopState::Idle,
        secondIdleRunLoop.IdleFailurePending == 0U, secondIdleController.State == ProcessorRunState::Idle,
        secondWake == ThreadError::None, secondReenqueue.HasValue(), secondReenqueue.GetValue().Sequence == 3U);

    ASTERIA_EXPECT(
        runner, "three admissions leave one exited worker and an empty disarmed idle owner before retirement",
        !finalDispatch, expectedIdleReturn(finalIdleFailure), secondReadmission.load(std::memory_order_acquire),
        firstRunSequence.load(std::memory_order_acquire) == 1U, secondRunSequence.load(std::memory_order_acquire) == 2U,
        thirdRunSequence.load(std::memory_order_acquire) == 3U, finalThread.State == ThreadState::Exited,
        finalThread.RunSequence == 3U, finalThread.ExitCode == 73U, !finalThread.BlockToken.IsValid(),
        finalContext.State == Kernel::Scheduler::SchedulingContextState::Bound,
        finalController.State == ProcessorRunState::Idle,
        finalController.Scheduler.State == Kernel::Scheduler::ProcessorSchedulerState::Idle,
        finalController.Scheduler.Queue.ReadyEntries == 0U, finalController.Scheduler.Queue.ClaimedEntries == 0U,
        finalController.Scheduler.Queue.ReservedEntries == 0U,
        finalController.Deadline.State == ProcessorDeadlineState::Disarmed,
        finalRunLoop.State == ProcessorKernelRunLoopState::Idle, finalRunLoop.IdleFailurePending == 0U,
        finalRunLoop.ActiveThread == nullptr, finalRunLoop.ActiveProtectionDomain == nullptr,
        finalRunLoop.ActiveAddressSpace == nullptr, finalRunLoop.BindingCount == 1U, timer.GetArmedDeadline() == 0U,
        timer.GetProgramCalls() == 3U, timer.GetCancelCalls() == 3U);

    // Stop releases the hosted worker while its binding still exists. The fake
    // native switch then reports Rejected, and join orders observation of the
    // resulting terminal run-loop state. A Faulted run loop cannot be unbound
    // or destroyed, so its placement lifetime ends only with this backing
    // storage after the saved continuation has returned.
    workerContext.Stop();
    worker.join();

    ProcessorKernelRunLoopReceipt retirementReceipt{};
    ProcessorKernelRunLoopFailure retirementFailure{};
    const bool retirementDispatch = runLoop->DispatchFromIdle(retirementReceipt, retirementFailure);
    const auto retiredRunLoop = runLoop->GetSnapshot();
    const auto retiredController = controller.GetSnapshot();
    const auto retiredThread = pair.ThreadStorage.GetSnapshot();
    ASTERIA_EXPECT(runner, "hosted retirement exposes the expected terminal native rejection", !retirementDispatch,
                   retirementFailure.Error == ProcessorKernelRunLoopError::NativeSwitchRejected,
                   retirementFailure.ThreadStorage == &pair.ThreadStorage,
                   retirementFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   retirementFailure.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread,
                   retirementFailure.DeadlineError == Kernel::Time::ProcessorDeadlineError::None,
                   retirementFailure.ActiveRunRetained == 0U,
                   retirementFailure.NativeError == static_cast<Foundation::UInt8>(FakeContextError::Rejected),
                   retiredRunLoop.State == ProcessorKernelRunLoopState::Faulted,
                   retiredRunLoop.IdleFailurePending == 0U, retiredRunLoop.ActiveThread == nullptr,
                   retiredRunLoop.ActiveProtectionDomain == nullptr, retiredRunLoop.ActiveAddressSpace == nullptr,
                   retiredRunLoop.BindingCount == 1U, retiredController.State == ProcessorRunState::Idle,
                   retiredThread.State == ThreadState::Exited, retiredThread.RunSequence == 3U,
                   !retiredThread.BlockToken.IsValid());
}

void TestWaitCommitTerminalStopsDispatch(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair pair{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    RecordedBlockObserver observer{BlockedRunCommitOutcome::TerminalRejected};
    const bool setup =
        protection.Ready && pair.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(pair.ThreadStorage, workerContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    std::thread worker{[&]() noexcept
                       {
                           if (workerContext.WaitForResume() == FakeContextError::Rejected)
                           {
                               return;
                           }
                           ProcessorKernelRunLoopReceipt receipt{};
                           ProcessorKernelRunLoopFailure failure{};
                           static_cast<void>(runLoop->FinishWaitAndDispatch(pair.ThreadStorage, workerContext, observer,
                                                                            receipt, failure));
                       }};

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop->DispatchFromIdle(receipt, failure);
    const auto runLoopSnapshot = runLoop->GetSnapshot();
    const auto controllerSnapshot = controller.GetSnapshot();
    const auto threadSnapshot = pair.ThreadStorage.GetSnapshot();
    ASTERIA_EXPECT(runner, "terminal wait commitment faults the run loop without dispatching", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::CompletionObserverTerminalRejected,
                   failure.BlockedRunCommit == BlockedRunCommitOutcome::TerminalRejected, observer.Calls == 1U,
                   observer.Token.IsValid(), runLoopSnapshot.State == ProcessorKernelRunLoopState::Faulted,
                   controllerSnapshot.State == ProcessorRunState::Idle, threadSnapshot.State == ThreadState::Blocked);

    workerContext.Stop();
    worker.join();
    static_cast<void>(pair.ThreadStorage.Terminate(1U));
    // The terminal run loop retains diagnostics and is intentionally not
    // destroyed; all externally owned scheduler and thread state is quiesced.
}

void TestCooperativeDeadlineReachedDuringRootActivation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain targetProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U},
                                      ProcessorRunDispatchMode::CooperativeBootstrap};
    FakeKernelContext idle{true};
    FakeKernelContext threadContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    RegisteredPair pair{registry, issuer};

    targetProtection.PageTable.ActivationTimer = &timer;
    targetProtection.PageTable.ActivationTicks = 30U;
    const bool setup =
        pair.Ready && idleProtection.Ready && targetProtection.Ready && addressInitialization.HasValue() &&
        (runLoop.Bind(pair.ThreadStorage, threadContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    // Reads: the scheduler's selection sample, the admission start, the
    // Final-entry validation, and the rollback's completion.
    const Size readsBeforeDispatch = timer.GetReadCalls();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop.DispatchFromIdle(receipt, failure);
    const Size readsAfterDispatch = timer.GetReadCalls();
    const auto controllerSnapshot = controller.GetSnapshot();
    const auto idleRoot = idleProtection.AddressSpaceStorage.GetSnapshot();
    const auto targetRoot = targetProtection.AddressSpaceStorage.GetSnapshot();
    const auto removed = controller.Remove(pair.ThreadIdentity);
    ProcessorKernelRunLoopReceipt cleanupReceipt{};
    ProcessorKernelRunLoopFailure cleanupFailure{};
    const bool cleanupDispatch = runLoop.DispatchFromIdle(cleanupReceipt, cleanupFailure);
    const bool unbound = runLoop.Unbind(pair.ThreadStorage) == ProcessorKernelRunLoopError::None;

    ASTERIA_EXPECT(runner, "cooperative deadline crossing restores idle root and requeues without entry", setup,
                   !dispatch, failure.Error == ProcessorKernelRunLoopError::ControllerEntryRejected,
                   failure.ControllerError == ProcessorRunError::DeadlineReached,
                   failure.DeadlineError == Kernel::Time::ProcessorDeadlineError::DeadlineReached,
                   failure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
                   failure.DeadlineReceipt.ObservedTicks == 10U, failure.ActiveRunRetained == 0U,
                   readsAfterDispatch == (readsBeforeDispatch + 4U), timer.GetReadCalls() == readsAfterDispatch,
                   timer.GetProgramCalls() == 0U, timer.GetCancelCalls() == 0U, idle.GetSwitchCount() == 0U,
                   threadContext.GetSwitchCount() == 0U, idleProtection.PageTable.ActivationCalls == 2U,
                   targetProtection.PageTable.ActivationCalls == 1U, idleRoot.ResidentProcessorMask == 0x01U,
                   targetRoot.ResidentProcessorMask == 0U, controllerSnapshot.State == ProcessorRunState::Idle,
                   controllerSnapshot.Scheduler.Queue.ReadyEntries == 1U,
                   !controllerSnapshot.Scheduler.ActiveThread.IsValid(),
                   pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle, removed.HasValue(),
                   !cleanupDispatch, cleanupFailure.ControllerError == ProcessorRunError::NoRunnableThread, unbound);
}

void TestExternalFrameCompletionAndReadmission(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain targetProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext firstContext{true};
    CooperativeKernelContext secondContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    RegisteredPair first{registry, issuer};
    RegisteredPair second{registry, issuer};
    const bool setup =
        first.Ready && second.Ready && idleProtection.Ready && targetProtection.Ready &&
        addressInitialization.HasValue() &&
        (runLoop->Bind(first.ThreadStorage, firstContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        (runLoop->Bind(second.ThreadStorage, secondContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(first.ContextIdentity, first.Context, first.ThreadIdentity, first.ThreadStorage)
            .HasValue() &&
        controller.Enqueue(second.ContextIdentity, second.Context, second.ThreadIdentity, second.ThreadStorage)
            .HasValue();
    if (!setup)
    {
        ASTERIA_EXPECT(runner, "external-frame test setup succeeds", false);
        return;
    }

    std::atomic<bool> firstFrameCompleted{false};
    std::atomic<bool> firstFrameReadNoClock{false};
    std::atomic<bool> secondResumedAfterExternalSelection{false};
    std::atomic<UInt64> firstElapsedTicks{0U};
    std::atomic<UInt64> firstChargedTicks{0U};
    std::thread firstWorker{
        [&]() noexcept
        {
            if (firstContext.WaitForResume() != FakeContextError::None)
            {
                idleContext.Resume();
                return;
            }
            const Size readCount = timer.GetReadCalls();
            const auto completion = runLoop->CompleteExternallyStoppedRunAsReady(
                first.ThreadStorage, firstContext, MakeExternalStop(controller, 30U, 0xA1U));
            firstFrameCompleted.store(completion.HasValue(), std::memory_order_release);
            firstFrameReadNoClock.store(timer.GetReadCalls() == readCount, std::memory_order_release);
            if (completion.HasValue())
            {
                firstElapsedTicks.store(completion.GetValue().Context.ElapsedTicks, std::memory_order_release);
                firstChargedTicks.store(completion.GetValue().Context.ChargedTicks, std::memory_order_release);
            }
            idleContext.Resume();

            if (firstContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            ProcessorKernelRunLoopReceipt receipt{};
            ProcessorKernelRunLoopFailure failure{};
            static_cast<void>(runLoop->FinishAndDispatch(first.ThreadStorage, firstContext,
                                                         ThreadRunDisposition::Exited, 0U, receipt, failure));
        }};

    std::thread secondWorker{
        [&]() noexcept
        {
            if (secondContext.WaitForResume() != FakeContextError::None)
            {
                idleContext.Resume();
                return;
            }
            ProcessorKernelRunLoopReceipt firstReceipt{};
            ProcessorKernelRunLoopFailure firstFailure{};
            const bool firstFinish = runLoop->FinishAndDispatch(
                second.ThreadStorage, secondContext, ThreadRunDisposition::Ready, 0U, firstReceipt, firstFailure);
            secondResumedAfterExternalSelection.store(
                firstFinish && (firstReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::Cooperative),
                std::memory_order_release);
            if (!firstFinish)
            {
                return;
            }

            ProcessorKernelRunLoopReceipt exitReceipt{};
            ProcessorKernelRunLoopFailure exitFailure{};
            static_cast<void>(runLoop->FinishAndDispatch(second.ThreadStorage, secondContext,
                                                         ThreadRunDisposition::Exited, 0U, exitReceipt, exitFailure));
        }};

    ProcessorKernelRunLoopReceipt idleReceipt{};
    ProcessorKernelRunLoopFailure idleFailure{};
    const bool idleResumed = runLoop->DispatchFromIdle(idleReceipt, idleFailure);
    const auto afterFirstFrame = runLoop->GetSnapshot();
    const auto afterFirstController = controller.GetSnapshot();
    const auto afterFirstAddress = addressSpaces.GetSnapshot();
    const Size acknowledgementsAfterFirst = timer.GetAcknowledgeCalls();

    timer.SetTicks(100U);
    firstContext.SetPrepared(false);
    const Size firstDestinationSwitches = firstContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopReceipt externalReceipt{};
    ProcessorKernelRunLoopFailure externalFailure{};
    const bool externalSelected = runLoop->DispatchFromIdle(externalReceipt, externalFailure);
    const auto pendingController = controller.GetSnapshot();
    const auto pendingRunLoop = runLoop->GetSnapshot();
    ProcessorKernelRunLoopFailure refreshFailure{};
    const bool continuationRefreshed = runLoop->RefreshExternalFrameIdleContinuation(externalReceipt, refreshFailure);
    ProcessorKernelRunLoopFailure commitFailure{};
    const bool committed =
        runLoop->CommitExternalFrameResume(first.ThreadStorage, firstContext, externalReceipt, commitFailure);
    const auto afterCommit = runLoop->GetSnapshot();
    const bool noSwitchToRetainedFirst = firstContext.GetDestinationSwitchCount() == firstDestinationSwitches;

    firstContext.Resume();
    static_cast<void>(idleContext.WaitForResume());
    const bool idleContinuationLanded =
        idleContext.ConsumeExternalFrameIdleContinuationLanding(externalReceipt.ExternalFrameResumeGeneration);
    ProcessorKernelRunLoopReceipt landingReceipt{};
    ProcessorKernelRunLoopFailure landingFailure{};
    const bool freshIdleFinished = runLoop->FinishSelectedFreshIdleLanding(landingReceipt, landingFailure);
    ProcessorKernelRunLoopReceipt replayedLandingReceipt{};
    ProcessorKernelRunLoopFailure replayedLandingFailure{};
    const bool replayedLandingFinished =
        runLoop->FinishSelectedFreshIdleLanding(replayedLandingReceipt, replayedLandingFailure);
    const auto finalRunLoop = runLoop->GetSnapshot();
    const auto finalController = controller.GetSnapshot();
    const bool unbound = (runLoop->Unbind(first.ThreadStorage) == ProcessorKernelRunLoopError::None) &&
                         (runLoop->Unbind(second.ThreadStorage) == ProcessorKernelRunLoopError::None);

    ASTERIA_EXPECT(
        runner,
        "external-frame completion returns logical and address-space ownership "
        "to idle without a fake native switch or clock read",
        setup, idleResumed, idleReceipt.State == ProcessorKernelRunLoopState::Idle,
        idleReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::None,
        firstFrameCompleted.load(std::memory_order_acquire), firstFrameReadNoClock.load(std::memory_order_acquire),
        firstElapsedTicks.load(std::memory_order_acquire) == 20U,
        firstChargedTicks.load(std::memory_order_acquire) == 20U,
        afterFirstFrame.State == ProcessorKernelRunLoopState::Idle, afterFirstFrame.ExternalFrameRetained == 1U,
        afterFirstController.State == ProcessorRunState::Idle, afterFirstController.Scheduler.Queue.ReadyEntries == 2U,
        afterFirstController.Deadline.State == ProcessorDeadlineState::Disarmed,
        afterFirstAddress.ActiveStorage == &idleProtection.AddressSpaceStorage, acknowledgementsAfterFirst == 1U);

    ASTERIA_EXPECT(
        runner,
        "B-to-retained-A routes through idle with the selected admission "
        "unchanged and publishes an exact no-SwitchTo resume receipt",
        externalSelected, externalReceipt.ThreadStorage == &first.ThreadStorage,
        externalReceipt.State == ProcessorKernelRunLoopState::Active,
        externalReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
        externalReceipt.ExternalFrameToken == 0xA1U, externalReceipt.StoppedRunSequence == 1U,
        externalReceipt.AdmissionRunSequence == 2U, externalReceipt.AdmissionDeadlineTicks == 120U,
        pendingRunLoop.State == ProcessorKernelRunLoopState::Active, pendingRunLoop.ExternalFrameRetained == 1U,
        pendingRunLoop.ExternalFrameResumePending == 1U, pendingController.State == ProcessorRunState::Active,
        pendingController.Scheduler.ActiveThread.IsEqualTo(first.ThreadIdentity), continuationRefreshed, committed,
        afterCommit.ExternalFrameRetained == 0U, afterCommit.ExternalFrameResumePending == 0U, noSwitchToRetainedFirst);

    ASTERIA_EXPECT(
        runner,
        "the fresh idle landing consumes the exact pending failure once "
        "after FIFO order remains A then B and both workers exit",
        secondResumedAfterExternalSelection.load(std::memory_order_acquire), idleContinuationLanded, !freshIdleFinished,
        landingFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
        landingFailure.ControllerError == ProcessorRunError::NoRunnableThread,
        landingFailure.SchedulerError == Kernel::Scheduler::ProcessorSchedulerError::NoRunnableThread,
        landingFailure.DeadlineError == Kernel::Time::ProcessorDeadlineError::None,
        landingFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
        landingFailure.DeadlineReceipt.SelectedDeadlineTicks == 0U,
        landingFailure.DeadlineReceipt.SelectedSources == 0U, landingFailure.DeadlineReceipt.Reprogrammed == 1U,
        landingFailure.ActiveRunRetained == 0U, landingFailure.NativeError == 0U, !replayedLandingFinished,
        replayedLandingFailure.Error == ProcessorKernelRunLoopError::InvariantViolation,
        first.ThreadStorage.GetSnapshot().State == ThreadState::Exited,
        first.ThreadStorage.GetSnapshot().RunSequence == 2U,
        second.ThreadStorage.GetSnapshot().State == ThreadState::Exited,
        second.ThreadStorage.GetSnapshot().RunSequence == 2U, finalRunLoop.State == ProcessorKernelRunLoopState::Idle,
        finalRunLoop.IdleFailurePending == 0U, finalController.State == ProcessorRunState::Idle, unbound);

    firstContext.Stop();
    firstWorker.join();
    secondContext.Stop();
    secondWorker.join();
    // Retire each hosted continuation before waking the next one. A native
    // processor cannot execute both suspended post-exit frames concurrently.
    // Their terminal run loop is intentionally not destroyed.
}

void TestCoordinatorOnlyExternalFrameDispatch(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte successStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const successFixture = ::new (static_cast<void*>(successStorage)) ExternalRunLoopFixture{};
    const bool retained = RetainExternalFrameAtIdle(*successFixture, 0xC001U);
    successFixture->Timer.SetTicks(100U);
    const Size idleSwitchesBefore = successFixture->IdleContext.GetSwitchCount();
    const Size targetSwitchesBefore = successFixture->TargetContext.GetSwitchCount();
    const Size targetDestinationSwitchesBefore = successFixture->TargetContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatched = successFixture->RunLoop.DispatchRetainedExternalFrameFromCoordinator(
        successFixture->Pair.ThreadStorage, successFixture->TargetContext, receipt, failure);
    const ProcessorKernelRunLoopSnapshot successRunLoop = successFixture->RunLoop.GetSnapshot();
    const auto successController = successFixture->Controller.GetSnapshot();

    ASTERIA_EXPECT(runner,
                   "the external-frame coordinator admits its sole retained owner "
                   "without a cooperative native switch",
                   retained, dispatched, failure.Error == ProcessorKernelRunLoopError::None,
                   receipt.State == ProcessorKernelRunLoopState::Active,
                   receipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
                   receipt.ThreadStorage == &successFixture->Pair.ThreadStorage, receipt.ExternalFrameToken == 0xC001U,
                   receipt.ExternalFrameResumeGeneration != 0U,
                   successRunLoop.State == ProcessorKernelRunLoopState::Active,
                   successRunLoop.ExternalFrameRetained == 1U, successRunLoop.ExternalFrameResumePending == 1U,
                   successController.State == ProcessorRunState::Active,
                   successFixture->IdleContext.GetSwitchCount() == idleSwitchesBefore,
                   successFixture->TargetContext.GetSwitchCount() == targetSwitchesBefore,
                   successFixture->TargetContext.GetDestinationSwitchCount() == targetDestinationSwitchesBefore);

    alignas(ExternalRunLoopFixture) static std::byte conflictStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const conflictFixture =
        ::new (static_cast<void*>(conflictStorage)) ExternalRunLoopFixture{};
    alignas(RegisteredPair) static std::byte peerPairStorage[sizeof(RegisteredPair)]{};
    RegisteredPair* const peerPair =
        ::new (static_cast<void*>(peerPairStorage)) RegisteredPair{conflictFixture->Registry, conflictFixture->Issuer};
    alignas(CooperativeKernelContext) static std::byte peerContextStorage[sizeof(CooperativeKernelContext)]{};
    CooperativeKernelContext* const peerContext =
        ::new (static_cast<void*>(peerContextStorage)) CooperativeKernelContext{true};
    const bool conflictRetained = RetainExternalFrameAtIdle(*conflictFixture, 0xC002U);
    ProcessorKernelRunLoopFailure enqueueFailure{};
    const bool peerReady =
        conflictRetained && peerPair->Ready &&
        (conflictFixture->RunLoop.Bind(peerPair->ThreadStorage, *peerContext,
                                       conflictFixture->TargetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        conflictFixture->RunLoop.EnqueueBoundThread(peerPair->ContextIdentity, peerPair->Context,
                                                    peerPair->ThreadIdentity, peerPair->ThreadStorage, enqueueFailure);
    const auto controllerBefore = conflictFixture->Controller.GetSnapshot();
    const auto addressBefore = conflictFixture->AddressSpaces.GetSnapshot();
    const Size readsBefore = conflictFixture->Timer.GetReadCalls();
    const Size programsBefore = conflictFixture->Timer.GetProgramCalls();
    const Size cancelsBefore = conflictFixture->Timer.GetCancelCalls();
    const Size acknowledgementsBefore = conflictFixture->Timer.GetAcknowledgeCalls();
    const Size idleSwitchesAtConflict = conflictFixture->IdleContext.GetSwitchCount();
    const Size targetSwitchesAtConflict = conflictFixture->TargetContext.GetSwitchCount();
    const Size peerSwitchesAtConflict = peerContext->GetSwitchCount();
    ProcessorKernelRunLoopReceipt conflictReceipt{};
    ProcessorKernelRunLoopFailure conflictFailure{};
    const bool conflictDispatch = conflictFixture->RunLoop.DispatchRetainedExternalFrameFromCoordinator(
        conflictFixture->Pair.ThreadStorage, conflictFixture->TargetContext, conflictReceipt, conflictFailure);
    const auto controllerAfter = conflictFixture->Controller.GetSnapshot();
    const auto addressAfter = conflictFixture->AddressSpaces.GetSnapshot();
    const ProcessorKernelRunLoopSnapshot conflictRunLoop = conflictFixture->RunLoop.GetSnapshot();
    const auto retainedAfter = conflictFixture->RunLoop.GetRetainedExternalFrame(conflictFixture->Pair.ThreadStorage);

    ASTERIA_EXPECT(
        runner,
        "the external-frame coordinator rejects a competing cooperative "
        "ready owner before scheduler, timer, root, or native mutation",
        peerReady, !conflictDispatch,
        conflictFailure.Error == ProcessorKernelRunLoopError::ExternalFrameCoordinatorScopeMismatch,
        conflictRunLoop.State == ProcessorKernelRunLoopState::Faulted, conflictRunLoop.ExternalFrameRetained == 1U,
        conflictRunLoop.ExternalFrameResumePending == 0U, retainedAfter.HasValue(),
        retainedAfter.GetValue().ExternalFrameToken == 0xC002U, controllerAfter.State == controllerBefore.State,
        controllerAfter.Scheduler.Queue.ReadyEntries == controllerBefore.Scheduler.Queue.ReadyEntries,
        controllerAfter.Scheduler.Queue.ClaimedEntries == controllerBefore.Scheduler.Queue.ClaimedEntries,
        controllerAfter.Scheduler.Queue.NextSequence == controllerBefore.Scheduler.Queue.NextSequence,
        controllerAfter.Deadline.State == controllerBefore.Deadline.State, addressAfter.State == addressBefore.State,
        addressAfter.ActiveStorage == addressBefore.ActiveStorage, addressAfter.Active.IsEqualTo(addressBefore.Active),
        conflictFixture->Timer.GetReadCalls() == readsBefore,
        conflictFixture->Timer.GetProgramCalls() == programsBefore,
        conflictFixture->Timer.GetCancelCalls() == cancelsBefore,
        conflictFixture->Timer.GetAcknowledgeCalls() == acknowledgementsBefore,
        conflictFixture->IdleContext.GetSwitchCount() == idleSwitchesAtConflict,
        conflictFixture->TargetContext.GetSwitchCount() == targetSwitchesAtConflict,
        peerContext->GetSwitchCount() == peerSwitchesAtConflict);
}

// The coordinator readmits a retained owner at tick 100 with the run stop 120
// (budget 20 per period 100). The processor stalls past that stop first
// between admission and timer programming, then between root activation and
// the final entry check. Each time the admission is rolled back as from idle,
// and after the next replenishment (tick 200) the same call succeeds.
void TestCoordinatorDispatchRollsBackPassedStop(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte programStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const program = ::new (static_cast<void*>(programStorage)) ExternalRunLoopFixture{};
    const bool programRetained = RetainExternalFrameAtIdle(*program, 0xC101U);
    program->Timer.SetTicks(100U);
    // Reads: the selection sample and the admission sample precede the stall.
    program->Timer.SetTicksAfterReads(2U, 125U);
    const Size programsBefore = program->Timer.GetProgramCalls();
    ProcessorKernelRunLoopReceipt programReceipt{};
    ProcessorKernelRunLoopFailure programFailure{};
    const bool programDispatch = program->RunLoop.DispatchRetainedExternalFrameFromCoordinator(
        program->Pair.ThreadStorage, program->TargetContext, programReceipt, programFailure);
    const ProcessorKernelRunLoopSnapshot programRunLoop = program->RunLoop.GetSnapshot();
    const auto programController = program->Controller.GetSnapshot();
    const auto programRetainedAfter = program->RunLoop.GetRetainedExternalFrame(program->Pair.ThreadStorage);
    const Size programsAfterStall = program->Timer.GetProgramCalls();
    const ThreadState programThread = program->Pair.ThreadStorage.GetSnapshot().State;
    program->Timer.SetTicks(200U);
    ProcessorKernelRunLoopReceipt programRetryReceipt{};
    ProcessorKernelRunLoopFailure programRetryFailure{};
    const bool programRetry = program->RunLoop.DispatchRetainedExternalFrameFromCoordinator(
        program->Pair.ThreadStorage, program->TargetContext, programRetryReceipt, programRetryFailure);
    ASTERIA_EXPECT(
        runner,
        "a coordinator admission whose stop passes before the timer is "
        "programmed is rolled back, keeps its frame, and is retried",
        programRetained, !programDispatch, programFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
        programFailure.ControllerError == ProcessorRunError::DeadlineReached, programFailure.ActiveRunRetained == 0U,
        programRunLoop.State == ProcessorKernelRunLoopState::Idle, programRunLoop.ExternalFrameRetained == 1U,
        programRunLoop.ExternalFrameResumePending == 0U, programRetainedAfter.HasValue(),
        programRetainedAfter.GetValue().ExternalFrameToken == 0xC101U,
        programController.State == ProcessorRunState::Idle,
        programController.Deadline.State == ProcessorDeadlineState::Disarmed,
        programController.Scheduler.Queue.ReadyEntries == 1U, programsAfterStall == programsBefore,
        programThread == ThreadState::Ready, programRetry,
        programRetryReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
        programRetryReceipt.ExternalFrameToken == 0xC101U, programRetryReceipt.AdmissionDeadlineTicks == 220U);

    alignas(ExternalRunLoopFixture) static std::byte entryStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const entry = ::new (static_cast<void*>(entryStorage)) ExternalRunLoopFixture{};
    const bool entryRetained = RetainExternalFrameAtIdle(*entry, 0xC102U);
    entry->Timer.SetTicks(100U);
    entry->TargetProtection.PageTable.ActivationTimer = &entry->Timer;
    entry->TargetProtection.PageTable.ActivationTicks = 125U;
    ProcessorKernelRunLoopReceipt entryReceipt{};
    ProcessorKernelRunLoopFailure entryFailure{};
    const bool entryDispatch = entry->RunLoop.DispatchRetainedExternalFrameFromCoordinator(
        entry->Pair.ThreadStorage, entry->TargetContext, entryReceipt, entryFailure);
    const ProcessorKernelRunLoopSnapshot entryRunLoop = entry->RunLoop.GetSnapshot();
    const auto entryController = entry->Controller.GetSnapshot();
    const auto entryAddress = entry->AddressSpaces.GetSnapshot();
    const ThreadState entryThread = entry->Pair.ThreadStorage.GetSnapshot().State;
    entry->TargetProtection.PageTable.ActivationTimer = nullptr;
    entry->Timer.SetTicks(200U);
    ProcessorKernelRunLoopReceipt entryRetryReceipt{};
    ProcessorKernelRunLoopFailure entryRetryFailure{};
    const bool entryRetry = entry->RunLoop.DispatchRetainedExternalFrameFromCoordinator(
        entry->Pair.ThreadStorage, entry->TargetContext, entryRetryReceipt, entryRetryFailure);
    ASTERIA_EXPECT(
        runner,
        "a coordinator admission whose stop passes during root activation is "
        "rolled back to the idle root, keeps its frame, and is retried",
        entryRetained, !entryDispatch, entryFailure.Error == ProcessorKernelRunLoopError::ControllerEntryRejected,
        entryFailure.ControllerError == ProcessorRunError::DeadlineReached, entryFailure.ActiveRunRetained == 0U,
        entryRunLoop.State == ProcessorKernelRunLoopState::Idle, entryRunLoop.ExternalFrameRetained == 1U,
        entryRunLoop.ExternalFrameResumePending == 0U, entryController.State == ProcessorRunState::Idle,
        entryController.Deadline.State == ProcessorDeadlineState::Disarmed,
        entryController.Scheduler.Queue.ReadyEntries == 1U,
        entryAddress.ActiveStorage == &entry->IdleProtection.AddressSpaceStorage, entryThread == ThreadState::Ready,
        entryRetry, entryRetryReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
        entryRetryReceipt.ExternalFrameToken == 0xC102U, entryRetryReceipt.AdmissionDeadlineTicks == 220U);
    // Both fixtures end with a selected resume that awaits the target's frame
    // restore, so, like the coordinator test above, they are not destroyed.
}

void TestMultipleRetainedFramesFollowPolicy(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain targetProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext firstContext{true};
    CooperativeKernelContext secondContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    RegisteredPair first{registry, issuer};
    RegisteredPair second{registry, issuer};
    const bool setup =
        first.Ready && second.Ready && idleProtection.Ready && targetProtection.Ready &&
        addressInitialization.HasValue() &&
        (runLoop->Bind(first.ThreadStorage, firstContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        (runLoop->Bind(second.ThreadStorage, secondContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(first.ContextIdentity, first.Context, first.ThreadIdentity, first.ThreadStorage)
            .HasValue() &&
        controller.Enqueue(second.ContextIdentity, second.Context, second.ThreadIdentity, second.ThreadStorage)
            .HasValue();
    if (!setup)
    {
        ASTERIA_EXPECT(runner, "multiple-frame test setup succeeds", false);
        return;
    }

    std::atomic<bool> firstStopped{false};
    std::atomic<bool> secondStopped{false};
    std::thread firstWorker{
        [&]() noexcept
        {
            if (firstContext.WaitForResume() != FakeContextError::None)
            {
                idleContext.Resume();
                return;
            }
            const auto completion = runLoop->CompleteExternallyStoppedRunAsReady(
                first.ThreadStorage, firstContext, MakeExternalStop(controller, 30U, 0x55U));
            firstStopped.store(completion.HasValue(), std::memory_order_release);
            idleContext.Resume();
            if (firstContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            ProcessorKernelRunLoopReceipt receipt{};
            ProcessorKernelRunLoopFailure failure{};
            static_cast<void>(runLoop->FinishAndDispatch(first.ThreadStorage, firstContext,
                                                         ThreadRunDisposition::Exited, 0U, receipt, failure));
        }};
    std::thread secondWorker{
        [&]() noexcept
        {
            if (secondContext.WaitForResume() != FakeContextError::None)
            {
                idleContext.Resume();
                return;
            }
            const auto completion = runLoop->CompleteExternallyStoppedRunAsReady(
                second.ThreadStorage, secondContext, MakeExternalStop(controller, 120U, 0x55U));
            secondStopped.store(completion.HasValue(), std::memory_order_release);
            idleContext.Resume();
            if (secondContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            ProcessorKernelRunLoopReceipt receipt{};
            ProcessorKernelRunLoopFailure failure{};
            static_cast<void>(runLoop->FinishAndDispatch(second.ThreadStorage, secondContext,
                                                         ThreadRunDisposition::Exited, 0U, receipt, failure));
        }};

    ProcessorKernelRunLoopReceipt firstIdleReceipt{};
    ProcessorKernelRunLoopFailure firstIdleFailure{};
    const bool firstIdle = runLoop->DispatchFromIdle(firstIdleReceipt, firstIdleFailure);
    timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt secondIdleReceipt{};
    ProcessorKernelRunLoopFailure secondIdleFailure{};
    const bool secondIdle = runLoop->DispatchFromIdle(secondIdleReceipt, secondIdleFailure);
    const auto beforePreflight = controller.GetSnapshot();
    const auto retainedSnapshot = runLoop->GetSnapshot();
    ReadyQueueState removalState{};
    ProcessorKernelRunLoopFailure removalFailure{};
    const bool removedRetained = runLoop->RemoveBoundThread(first.ThreadStorage, removalState, removalFailure);
    const auto rejectedUnbind = runLoop->Unbind(first.ThreadStorage);
    const auto afterPreflight = controller.GetSnapshot();
    const auto afterRejectedUnbind = runLoop->GetSnapshot();

    timer.SetTicks(200U);
    const Size firstDestinations = firstContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopReceipt firstResume{};
    ProcessorKernelRunLoopFailure firstResumeFailure{};
    const bool firstSelected = runLoop->DispatchFromIdle(firstResume, firstResumeFailure);
    ProcessorKernelRunLoopFailure firstRefreshFailure{};
    const bool firstRefreshed = runLoop->RefreshExternalFrameIdleContinuation(firstResume, firstRefreshFailure);
    ProcessorKernelRunLoopFailure firstCommitFailure{};
    const bool firstCommitted =
        runLoop->CommitExternalFrameResume(first.ThreadStorage, firstContext, firstResume, firstCommitFailure);
    firstContext.Resume();
    static_cast<void>(idleContext.WaitForResume());
    const bool firstIdleContinuationLanded =
        idleContext.ConsumeExternalFrameIdleContinuationLanding(firstResume.ExternalFrameResumeGeneration);

    const auto pendingSecond = runLoop->GetSnapshot();
    const auto pendingSecondController = controller.GetSnapshot();
    const Size secondDestinations = secondContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopReceipt secondResume{};
    ProcessorKernelRunLoopFailure secondResumeFailure{};
    const bool secondSelected = runLoop->DispatchFromIdle(secondResume, secondResumeFailure);
    ProcessorKernelRunLoopFailure secondRefreshFailure{};
    const bool secondRefreshed = runLoop->RefreshExternalFrameIdleContinuation(secondResume, secondRefreshFailure);
    ProcessorKernelRunLoopFailure secondCommitFailure{};
    const bool secondCommitted =
        runLoop->CommitExternalFrameResume(second.ThreadStorage, secondContext, secondResume, secondCommitFailure);
    secondContext.Resume();
    static_cast<void>(idleContext.WaitForResume());
    const bool secondIdleContinuationLanded =
        idleContext.ConsumeExternalFrameIdleContinuationLanding(secondResume.ExternalFrameResumeGeneration);

    const auto finalRunLoop = runLoop->GetSnapshot();
    const auto finalController = controller.GetSnapshot();
    const bool unbound = (runLoop->Unbind(first.ThreadStorage) == ProcessorKernelRunLoopError::None) &&
                         (runLoop->Unbind(second.ThreadStorage) == ProcessorKernelRunLoopError::None);

    ASTERIA_EXPECT(runner,
                   "two bindings retain independent frames and retirement rejection "
                   "does not mutate FIFO state",
                   firstIdle, secondIdle, firstStopped.load(std::memory_order_acquire),
                   secondStopped.load(std::memory_order_acquire),
                   retainedSnapshot.State == ProcessorKernelRunLoopState::Idle,
                   retainedSnapshot.ExternalFrameRetained == 1U, beforePreflight.Scheduler.Queue.ReadyEntries == 2U,
                   !removedRetained, removalFailure.Error == ProcessorKernelRunLoopError::ExternalFrameRetained,
                   rejectedUnbind == ProcessorKernelRunLoopError::ExternalFrameRetained,
                   afterPreflight.Scheduler.Queue.ReadyEntries == 2U,
                   afterPreflight.Scheduler.Queue.NextSequence == beforePreflight.Scheduler.Queue.NextSequence,
                   afterRejectedUnbind.BindingCount == 2U);

    ASTERIA_EXPECT(
        runner,
        "normal policy admits the first retained FIFO owner and preserves "
        "the independently selected second admission through idle",
        firstSelected, firstResume.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
        firstResume.ThreadStorage == &first.ThreadStorage, firstResume.ExternalFrameToken == 0x55U,
        firstResume.StoppedRunSequence == 1U, firstResume.AdmissionRunSequence == 2U, firstRefreshed, firstCommitted,
        firstContext.GetDestinationSwitchCount() == firstDestinations,
        pendingSecond.State == ProcessorKernelRunLoopState::ExternalFramePending,
        pendingSecond.ExternalFrameRetained == 1U, pendingSecondController.State == ProcessorRunState::Active,
        pendingSecondController.Scheduler.ActiveThread.IsEqualTo(second.ThreadIdentity), firstIdleContinuationLanded);

    ASTERIA_EXPECT(runner,
                   "the second retained owner commits with an independent generation "
                   "and neither retained destination receives a cooperative SwitchTo",
                   secondSelected, secondResume.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
                   secondResume.ThreadStorage == &second.ThreadStorage, secondResume.ExternalFrameToken == 0x55U,
                   secondResume.StoppedRunSequence == 1U, secondResume.AdmissionRunSequence == 2U,
                   secondResume.ExternalFrameResumeGeneration > firstResume.ExternalFrameResumeGeneration,
                   secondRefreshed, secondCommitted, secondContext.GetDestinationSwitchCount() == secondDestinations,
                   finalRunLoop.State == ProcessorKernelRunLoopState::Idle, finalRunLoop.ExternalFrameRetained == 0U,
                   finalController.State == ProcessorRunState::Idle,
                   first.ThreadStorage.GetSnapshot().State == ThreadState::Exited,
                   second.ThreadStorage.GetSnapshot().State == ThreadState::Exited, secondIdleContinuationLanded,
                   unbound);

    firstContext.Stop();
    firstWorker.join();
    secondContext.Stop();
    secondWorker.join();
    // Retire each hosted continuation before waking the next one. A native
    // processor cannot execute both suspended post-exit frames concurrently.
    // Their terminal run loop is intentionally not destroyed.
}

void TestPendingExternalResumeRejectsActiveMutations(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[4U][sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const finishFixture =
        ::new (static_cast<void*>(fixtureStorage[0U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const waitFixture = ::new (static_cast<void*>(fixtureStorage[1U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const faultFixture =
        ::new (static_cast<void*>(fixtureStorage[2U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const stopFixture = ::new (static_cast<void*>(fixtureStorage[3U])) ExternalRunLoopFixture{};
    RecordedBlockObserver waitObserver{BlockedRunCommitOutcome::Accepted};
    RecordedFaultCommit faultCommit;

    const bool finishRejected = PendingExternalMutationIsTerminal(
        *finishFixture, 0x101U,
        [](ExternalRunLoopFixture& fixture, const ProcessorKernelRunLoopReceipt&,
           ProcessorKernelRunLoopFailure& failure) noexcept
        {
            ProcessorKernelRunLoopReceipt receipt{};
            return fixture.RunLoop.FinishAndDispatch(fixture.Pair.ThreadStorage, fixture.TargetContext,
                                                     ThreadRunDisposition::Ready, 0U, receipt, failure);
        });
    const bool waitRejected = PendingExternalMutationIsTerminal(
        *waitFixture, 0x102U,
        [&waitObserver](ExternalRunLoopFixture& fixture, const ProcessorKernelRunLoopReceipt&,
                        ProcessorKernelRunLoopFailure& failure) noexcept
        {
            ProcessorKernelRunLoopReceipt receipt{};
            return fixture.RunLoop.FinishWaitAndDispatch(fixture.Pair.ThreadStorage, fixture.TargetContext,
                                                         waitObserver, receipt, failure);
        });
    const bool faultRejected = PendingExternalMutationIsTerminal(
        *faultFixture, 0x103U,
        [&faultCommit](ExternalRunLoopFixture& fixture, const ProcessorKernelRunLoopReceipt&,
                       ProcessorKernelRunLoopFailure& failure) noexcept
        {
            ProcessorKernelRunLoopReceipt receipt{};
            return fixture.RunLoop.FinishFaultAndDispatch(fixture.Pair.ThreadStorage, fixture.TargetContext,
                                                          faultCommit, receipt, failure);
        });
    const bool stopRejected = PendingExternalMutationIsTerminal(
        *stopFixture, 0x104U,
        [](ExternalRunLoopFixture& fixture, const ProcessorKernelRunLoopReceipt&,
           ProcessorKernelRunLoopFailure& failure) noexcept
        {
            const auto result = fixture.RunLoop.CompleteExternallyStoppedRunAsReady(
                fixture.Pair.ThreadStorage, fixture.TargetContext, MakeExternalStop(fixture.Controller, 120U, 0x204U));
            if (!result.HasValue())
            {
                failure = result.GetError();
            }
            return result.HasValue();
        });

    ASTERIA_EXPECT(runner,
                   "selected external-frame actions reject Finish, Wait, fault, and "
                   "second-stop mutations before touching controller or native state",
                   finishRejected, waitRejected, faultRejected, stopRejected, waitObserver.Calls == 0U,
                   faultCommit.Calls == 0U);

    // Every fixture intentionally retains its terminal frame/admission
    // evidence and therefore is not destroyed.
}

void TestExternalStopFailuresRetainFrameOwnership(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[3U][sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const protocolFixture =
        ::new (static_cast<void*>(fixtureStorage[0U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const backendFixture =
        ::new (static_cast<void*>(fixtureStorage[1U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const schedulerFixture =
        ::new (static_cast<void*>(fixtureStorage[2U])) ExternalRunLoopFixture{};

    const bool protocolRetained = ExternalStopFailureRetainsFrame(
        *protocolFixture, 0x211U, ProcessorRunExternalStopError::HardwareEvidenceMissing,
        [](ExternalRunLoopFixture&, ProcessorRunExternalStopObservation& observation) noexcept
        {
            observation.HardwareState = ProcessorRunExternalStopHardwareState::Unverified;
            return true;
        });
    const bool backendRetained = ExternalStopFailureRetainsFrame(
        *backendFixture, 0x212U, ProcessorRunExternalStopError::DeadlineConsumptionRejected,
        [](ExternalRunLoopFixture& fixture, ProcessorRunExternalStopObservation&) noexcept
        {
            fixture.Timer.FailNextAcknowledge(LocalTimerError::InvalidDeadline);
            return true;
        });
    const bool schedulerRetained = ExternalStopFailureRetainsFrame(
        *schedulerFixture, 0x213U, ProcessorRunExternalStopError::SchedulerCompletionRejected,
        [](ExternalRunLoopFixture& fixture, ProcessorRunExternalStopObservation&) noexcept
        {
            fixture.Timer.EndRunOnNextAcknowledge(fixture.Pair.Context, CpuId{0U}, MonotonicTime{30U});
            return true;
        });

    ASTERIA_EXPECT(runner,
                   "protocol, backend, and scheduler settlement failures retain the "
                   "exact external-frame token and reject retirement",
                   protocolRetained, backendRetained, schedulerRetained,
                   schedulerFixture->Timer.EndRunOnAcknowledgeAccepted());

    // All fixtures intentionally retain terminal frame/admission evidence.
}

void TestExternalContinuationRefreshFailuresRetainOwnership(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[2U][sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const rejectedFixture =
        ::new (static_cast<void*>(fixtureStorage[0U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const mismatchFixture =
        ::new (static_cast<void*>(fixtureStorage[1U])) ExternalRunLoopFixture{};

    const bool rejected =
        ExternalContinuationRefreshFailureRetainsFrame(*rejectedFixture, 0x221U,
                                                       [](CooperativeKernelContext& idle) noexcept
                                                       {
                                                           idle.RejectNextExternalFrameIdleContinuationSave();
                                                       });
    const bool mismatched =
        ExternalContinuationRefreshFailureRetainsFrame(*mismatchFixture, 0x222U,
                                                       [](CooperativeKernelContext& idle) noexcept
                                                       {
                                                           idle.MismatchNextExternalFrameIdleContinuationSave();
                                                       });

    ASTERIA_EXPECT(runner,
                   "idle-context save rejection and mismatched publication retain the "
                   "selected frame, admission, root, and hardware state",
                   rejected, mismatched);

    // Both fixtures intentionally retain terminal frame/admission evidence.
}

void TestExternalResumeCommitRejectsReplayAndStaleContinuation(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[2U][sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const replayFixture =
        ::new (static_cast<void*>(fixtureStorage[0U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const continuationFixture =
        ::new (static_cast<void*>(fixtureStorage[1U])) ExternalRunLoopFixture{};

    const bool firstRetained = RetainExternalFrameAtIdle(*replayFixture, 0x301U);
    replayFixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt firstReceipt{};
    ProcessorKernelRunLoopFailure firstAdmissionFailure{};
    const bool firstSelected = replayFixture->RunLoop.DispatchFromIdle(firstReceipt, firstAdmissionFailure);
    ProcessorKernelRunLoopFailure firstRefreshFailure{};
    const bool firstRefreshed =
        replayFixture->RunLoop.RefreshExternalFrameIdleContinuation(firstReceipt, firstRefreshFailure);
    ProcessorKernelRunLoopFailure firstCommitFailure{};
    const bool firstCommitted = replayFixture->RunLoop.CommitExternalFrameResume(
        replayFixture->Pair.ThreadStorage, replayFixture->TargetContext, firstReceipt, firstCommitFailure);
    const auto secondStop = replayFixture->RunLoop.CompleteExternallyStoppedRunAsReady(
        replayFixture->Pair.ThreadStorage, replayFixture->TargetContext,
        MakeExternalStop(replayFixture->Controller, 120U, 0x301U));
    replayFixture->Timer.SetTicks(200U);
    ProcessorKernelRunLoopReceipt secondReceipt{};
    ProcessorKernelRunLoopFailure secondAdmissionFailure{};
    const bool secondSelected = replayFixture->RunLoop.DispatchFromIdle(secondReceipt, secondAdmissionFailure);
    ProcessorKernelRunLoopFailure secondRefreshFailure{};
    const bool secondRefreshed =
        replayFixture->RunLoop.RefreshExternalFrameIdleContinuation(secondReceipt, secondRefreshFailure);
    const auto replayControllerBefore = replayFixture->Controller.GetSnapshot();
    const auto replayAddressBefore = replayFixture->AddressSpaces.GetSnapshot();
    const Size replayReads = replayFixture->Timer.GetReadCalls();
    const Size replayPrograms = replayFixture->Timer.GetProgramCalls();
    const Size replayCancels = replayFixture->Timer.GetCancelCalls();
    const Size replayAcknowledgements = replayFixture->Timer.GetAcknowledgeCalls();
    ProcessorKernelRunLoopFailure replayFailure{};
    const bool replayCommitted = replayFixture->RunLoop.CommitExternalFrameResume(
        replayFixture->Pair.ThreadStorage, replayFixture->TargetContext, firstReceipt, replayFailure);
    const auto replayControllerAfter = replayFixture->Controller.GetSnapshot();
    const auto replayAddressAfter = replayFixture->AddressSpaces.GetSnapshot();
    const auto replayRunLoopAfter = replayFixture->RunLoop.GetSnapshot();

    const bool continuationRetained = RetainExternalFrameAtIdle(*continuationFixture, 0x302U);
    continuationFixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt continuationReceipt{};
    ProcessorKernelRunLoopFailure continuationAdmissionFailure{};
    const bool continuationSelected =
        continuationFixture->RunLoop.DispatchFromIdle(continuationReceipt, continuationAdmissionFailure);
    const auto continuationControllerBefore = continuationFixture->Controller.GetSnapshot();
    const auto continuationAddressBefore = continuationFixture->AddressSpaces.GetSnapshot();
    const Size continuationReads = continuationFixture->Timer.GetReadCalls();
    const Size continuationPrograms = continuationFixture->Timer.GetProgramCalls();
    const Size continuationCancels = continuationFixture->Timer.GetCancelCalls();
    const Size continuationAcknowledgements = continuationFixture->Timer.GetAcknowledgeCalls();
    ProcessorKernelRunLoopFailure continuationFailure{};
    const bool continuationCommitted = continuationFixture->RunLoop.CommitExternalFrameResume(
        continuationFixture->Pair.ThreadStorage, continuationFixture->TargetContext, continuationReceipt,
        continuationFailure);
    const auto continuationControllerAfter = continuationFixture->Controller.GetSnapshot();
    const auto continuationAddressAfter = continuationFixture->AddressSpaces.GetSnapshot();
    const auto continuationRunLoopAfter = continuationFixture->RunLoop.GetSnapshot();

    ASTERIA_EXPECT(
        runner,
        "a reused target token cannot replay an older common generation over "
        "a newer retained frame",
        firstRetained, firstSelected, firstRefreshed, firstCommitted, secondStop.HasValue(), secondSelected,
        secondRefreshed, firstReceipt.ExternalFrameToken == secondReceipt.ExternalFrameToken,
        secondReceipt.ExternalFrameResumeGeneration > firstReceipt.ExternalFrameResumeGeneration,
        secondReceipt.StoppedRunSequence > firstReceipt.StoppedRunSequence, !replayCommitted,
        replayFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumeMismatch,
        replayRunLoopAfter.State == ProcessorKernelRunLoopState::Faulted,
        replayRunLoopAfter.ExternalFrameRetained == 1U, replayRunLoopAfter.ExternalFrameResumePending == 1U,
        replayControllerAfter.State == ProcessorRunState::Active,
        replayControllerAfter.Scheduler.ActiveThread.IsEqualTo(replayFixture->Pair.ThreadIdentity),
        replayControllerAfter.Scheduler.Queue.NextSequence == replayControllerBefore.Scheduler.Queue.NextSequence,
        replayControllerAfter.Deadline.SelectedDeadlineTicks == replayControllerBefore.Deadline.SelectedDeadlineTicks,
        replayAddressAfter.ActiveStorage == replayAddressBefore.ActiveStorage,
        replayAddressAfter.Active.IsEqualTo(replayAddressBefore.Active),
        replayFixture->Timer.GetReadCalls() == replayReads, replayFixture->Timer.GetProgramCalls() == replayPrograms,
        replayFixture->Timer.GetCancelCalls() == replayCancels,
        replayFixture->Timer.GetAcknowledgeCalls() == replayAcknowledgements);

    ASTERIA_EXPECT(runner,
                   "commit rejects a missing idle-context refresh without clearing the "
                   "selected frame or admission",
                   continuationRetained, continuationSelected, !continuationCommitted,
                   continuationFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumeMismatch,
                   continuationRunLoopAfter.State == ProcessorKernelRunLoopState::Faulted,
                   continuationRunLoopAfter.ExternalFrameRetained == 1U,
                   continuationRunLoopAfter.ExternalFrameResumePending == 1U,
                   continuationControllerAfter.State == ProcessorRunState::Active,
                   continuationControllerAfter.Scheduler.Queue.NextSequence ==
                       continuationControllerBefore.Scheduler.Queue.NextSequence,
                   continuationControllerAfter.Deadline.SelectedDeadlineTicks ==
                       continuationControllerBefore.Deadline.SelectedDeadlineTicks,
                   continuationAddressAfter.ActiveStorage == continuationAddressBefore.ActiveStorage,
                   continuationAddressAfter.Active.IsEqualTo(continuationAddressBefore.Active),
                   continuationFixture->Timer.GetReadCalls() == continuationReads,
                   continuationFixture->Timer.GetProgramCalls() == continuationPrograms,
                   continuationFixture->Timer.GetCancelCalls() == continuationCancels,
                   continuationFixture->Timer.GetAcknowledgeCalls() == continuationAcknowledgements);

    // Both fixtures intentionally retain terminal frame/admission evidence.
}

void TestExternalResumeGenerationSurvivesRunLoopRecreation(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) ExternalRunLoopFixture{};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;

    const bool firstGenerationSaved = fixture->IdleContext.SaveExternalFrameIdleContinuation(1U);
    fixture->IdleContext.Resume();
    const bool firstGenerationEntered = fixture->IdleContext.WaitForResume() == FakeContextError::None;
    const bool firstGenerationLanded = fixture->IdleContext.ConsumeExternalFrameIdleContinuationLanding(1U);
    const auto firstLifetimeRunLoop = fixture->RunLoop.GetSnapshot();
    const auto firstLifetimeController = fixture->Controller.GetSnapshot();
    const auto firstLifetimeAddress = fixture->AddressSpaces.GetSnapshot();

    fixture->RunLoop.~RunLoop();
    RunLoop* const secondRunLoop = ::new (static_cast<void*>(&fixture->RunLoop))
        RunLoop{fixture->Controller, fixture->AddressSpaces, fixture->IdleContext};
    ProcessorKernelRunLoopFailure enqueueFailure{};
    const bool prepared =
        fixture->Ready && fixture->Pair.Ready &&
        (secondRunLoop->Bind(fixture->Pair.ThreadStorage, fixture->TargetContext, fixture->TargetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        secondRunLoop->EnqueueBoundThread(fixture->Pair.ContextIdentity, fixture->Pair.Context,
                                          fixture->Pair.ThreadIdentity, fixture->Pair.ThreadStorage, enqueueFailure);

    std::atomic<bool> stopAccepted{false};
    ProcessorRunExternalStopObservation submittedObservation{};
    std::thread worker{
        [&]() noexcept
        {
            if (fixture->TargetContext.WaitForResume() != FakeContextError::None)
            {
                fixture->IdleContext.Resume();
                return;
            }
            submittedObservation = MakeExternalStop(fixture->Controller, 30U, 0x601U);
            stopAccepted.store(secondRunLoop
                                   ->CompleteExternallyStoppedRunAsReady(fixture->Pair.ThreadStorage,
                                                                         fixture->TargetContext, submittedObservation)
                                   .HasValue(),
                               std::memory_order_release);
            fixture->IdleContext.Resume();
        }};
    ProcessorKernelRunLoopReceipt idleReceipt{};
    ProcessorKernelRunLoopFailure idleFailure{};
    const bool returnedToIdle = prepared && secondRunLoop->DispatchFromIdle(idleReceipt, idleFailure);
    worker.join();
    fixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt currentReceipt{};
    ProcessorKernelRunLoopFailure admissionFailure{};
    const bool selected = returnedToIdle && secondRunLoop->DispatchFromIdle(currentReceipt, admissionFailure);
    ProcessorKernelRunLoopFailure refreshFailure{};
    const bool refreshed =
        selected && secondRunLoop->RefreshExternalFrameIdleContinuation(currentReceipt, refreshFailure);

    ProcessorKernelRunLoopReceipt staleReceipt = currentReceipt;
    staleReceipt.ExternalFrameResumeGeneration = 1U;
    const auto beforeReplay = fixture->Controller.GetSnapshot();
    const auto addressBeforeReplay = fixture->AddressSpaces.GetSnapshot();
    const Size readsBeforeReplay = fixture->Timer.GetReadCalls();
    const Size programsBeforeReplay = fixture->Timer.GetProgramCalls();
    const Size cancelsBeforeReplay = fixture->Timer.GetCancelCalls();
    const Size acknowledgementsBeforeReplay = fixture->Timer.GetAcknowledgeCalls();
    const Size idleSwitchesBeforeReplay = fixture->IdleContext.GetSwitchCount();
    const Size targetSwitchesBeforeReplay = fixture->TargetContext.GetSwitchCount();
    ProcessorKernelRunLoopFailure replayFailure{};
    const bool replayCommitted =
        refreshed && secondRunLoop->CommitExternalFrameResume(fixture->Pair.ThreadStorage, fixture->TargetContext,
                                                              staleReceipt, replayFailure);
    const auto afterReplay = fixture->Controller.GetSnapshot();
    const auto addressAfterReplay = fixture->AddressSpaces.GetSnapshot();
    const auto runLoopAfterReplay = secondRunLoop->GetSnapshot();
    const auto retainedAfterReplay = secondRunLoop->GetRetainedExternalFrame(fixture->Pair.ThreadStorage);

    ASTERIA_EXPECT(runner,
                   "the exact idle context lands generation one while the first run-loop "
                   "lifetime remains quiescent and legally destructible",
                   firstGenerationSaved, firstGenerationEntered, firstGenerationLanded,
                   firstLifetimeRunLoop.State == ProcessorKernelRunLoopState::Idle,
                   firstLifetimeRunLoop.BindingCount == 0U, firstLifetimeRunLoop.ExternalFrameRetained == 0U,
                   firstLifetimeRunLoop.ExternalFrameResumePending == 0U,
                   firstLifetimeController.State == ProcessorRunState::Idle,
                   firstLifetimeAddress.ActiveStorage == &fixture->IdleProtection.AddressSpaceStorage);

    ASTERIA_EXPECT(runner,
                   "a recreated run loop seeds from the exact idle-context lifetime and "
                   "rejects an otherwise-current stale-generation receipt",
                   prepared, returnedToIdle, stopAccepted.load(std::memory_order_acquire), selected, refreshed,
                   currentReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
                   currentReceipt.ExternalFrameResumeGeneration == 2U,
                   fixture->IdleContext.GetExternalFrameIdleContinuationGeneration() == 2U, !replayCommitted,
                   replayFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumeMismatch,
                   runLoopAfterReplay.State == ProcessorKernelRunLoopState::Faulted,
                   runLoopAfterReplay.ExternalFrameRetained == 1U, runLoopAfterReplay.ExternalFrameResumePending == 1U,
                   retainedAfterReplay.HasValue(),
                   ExternalStopObservationsAreEqual(retainedAfterReplay.GetValue(), submittedObservation),
                   afterReplay.State == beforeReplay.State,
                   afterReplay.Scheduler.ActiveThread.IsEqualTo(beforeReplay.Scheduler.ActiveThread),
                   afterReplay.Scheduler.Queue.ReadyEntries == beforeReplay.Scheduler.Queue.ReadyEntries,
                   afterReplay.Scheduler.Queue.NextSequence == beforeReplay.Scheduler.Queue.NextSequence,
                   afterReplay.Deadline.SelectedDeadlineTicks == beforeReplay.Deadline.SelectedDeadlineTicks,
                   addressAfterReplay.ActiveStorage == addressBeforeReplay.ActiveStorage,
                   addressAfterReplay.Active.IsEqualTo(addressBeforeReplay.Active),
                   fixture->Timer.GetReadCalls() == readsBeforeReplay,
                   fixture->Timer.GetProgramCalls() == programsBeforeReplay,
                   fixture->Timer.GetCancelCalls() == cancelsBeforeReplay,
                   fixture->Timer.GetAcknowledgeCalls() == acknowledgementsBeforeReplay,
                   fixture->IdleContext.GetSwitchCount() == idleSwitchesBeforeReplay,
                   fixture->TargetContext.GetSwitchCount() == targetSwitchesBeforeReplay);

    // The recreated run loop intentionally retains terminal replay evidence.
}

void TestExternalResumeGenerationExhaustionRetainsFrames(TestContext& runner) noexcept
{
    constexpr UInt64 maximumGeneration{~UInt64{0U}};
    alignas(ExternalRunLoopFixture) static std::byte maximumStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const maximumFixture = ::new (static_cast<void*>(maximumStorage)) ExternalRunLoopFixture{};
    const bool maximumPrepared = maximumFixture->PrepareAdmission();
    maximumFixture->IdleContext.SeedExternalFrameIdleContinuationGeneration(maximumGeneration);
    ProcessorRunExternalStopObservation maximumObservation{};
    ProcessorKernelRunLoopFailure maximumStopFailure{};
    bool maximumCompletionAccepted{false};
    Kernel::Scheduler::ProcessorRunSnapshot maximumBefore{};
    Kernel::Scheduler::ProcessorRunSnapshot maximumAfter{};
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot maximumAddressBefore{};
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot maximumAddressAfter{};
    Size maximumReadsBefore{0U};
    Size maximumProgramsBefore{0U};
    Size maximumCancelsBefore{0U};
    Size maximumAcknowledgementsBefore{0U};
    Size maximumIdleSwitchesBefore{0U};
    Size maximumTargetSwitchesBefore{0U};
    Size maximumReadsAfter{0U};
    Size maximumProgramsAfter{0U};
    Size maximumCancelsAfter{0U};
    Size maximumAcknowledgementsAfter{0U};
    Size maximumIdleSwitchesAfter{0U};
    Size maximumTargetSwitchesAfter{0U};
    std::thread maximumWorker{[&]() noexcept
                              {
                                  if (maximumFixture->TargetContext.WaitForResume() != FakeContextError::None)
                                  {
                                      maximumFixture->IdleContext.Resume();
                                      return;
                                  }
                                  maximumObservation = MakeExternalStop(maximumFixture->Controller, 30U, 0x701U);
                                  maximumBefore = maximumFixture->Controller.GetSnapshot();
                                  maximumAddressBefore = maximumFixture->AddressSpaces.GetSnapshot();
                                  maximumReadsBefore = maximumFixture->Timer.GetReadCalls();
                                  maximumProgramsBefore = maximumFixture->Timer.GetProgramCalls();
                                  maximumCancelsBefore = maximumFixture->Timer.GetCancelCalls();
                                  maximumAcknowledgementsBefore = maximumFixture->Timer.GetAcknowledgeCalls();
                                  maximumIdleSwitchesBefore = maximumFixture->IdleContext.GetSwitchCount();
                                  maximumTargetSwitchesBefore = maximumFixture->TargetContext.GetSwitchCount();
                                  const auto completion = maximumFixture->RunLoop.CompleteExternallyStoppedRunAsReady(
                                      maximumFixture->Pair.ThreadStorage, maximumFixture->TargetContext,
                                      maximumObservation);
                                  maximumCompletionAccepted = completion.HasValue();
                                  if (!maximumCompletionAccepted)
                                  {
                                      maximumStopFailure = completion.GetError();
                                  }
                                  maximumAfter = maximumFixture->Controller.GetSnapshot();
                                  maximumAddressAfter = maximumFixture->AddressSpaces.GetSnapshot();
                                  maximumReadsAfter = maximumFixture->Timer.GetReadCalls();
                                  maximumProgramsAfter = maximumFixture->Timer.GetProgramCalls();
                                  maximumCancelsAfter = maximumFixture->Timer.GetCancelCalls();
                                  maximumAcknowledgementsAfter = maximumFixture->Timer.GetAcknowledgeCalls();
                                  maximumIdleSwitchesAfter = maximumFixture->IdleContext.GetSwitchCount();
                                  maximumTargetSwitchesAfter = maximumFixture->TargetContext.GetSwitchCount();
                                  maximumFixture->IdleContext.Resume();
                              }};
    ProcessorKernelRunLoopReceipt maximumReceipt{};
    ProcessorKernelRunLoopFailure maximumDispatchFailure{};
    const bool maximumDispatch =
        maximumPrepared && maximumFixture->RunLoop.DispatchFromIdle(maximumReceipt, maximumDispatchFailure);
    maximumWorker.join();
    const auto maximumRetained = maximumFixture->RunLoop.GetRetainedExternalFrame(maximumFixture->Pair.ThreadStorage);
    const auto maximumRunLoop = maximumFixture->RunLoop.GetSnapshot();
    const auto maximumBeforeRemoval = maximumFixture->Controller.GetSnapshot();
    ReadyQueueState maximumRemovalState{};
    ProcessorKernelRunLoopFailure maximumRemovalFailure{};
    const bool maximumRemoved = maximumFixture->RunLoop.RemoveBoundThread(maximumFixture->Pair.ThreadStorage,
                                                                          maximumRemovalState, maximumRemovalFailure);
    const auto maximumAfterRemoval = maximumFixture->Controller.GetSnapshot();
    const auto maximumUnbound = maximumFixture->RunLoop.Unbind(maximumFixture->Pair.ThreadStorage);

    ASTERIA_EXPECT(
        runner,
        "a context-published maximum generation fails before controller, "
        "root, timer, or native mutation and retains the exact frame",
        maximumPrepared, !maximumCompletionAccepted, !maximumDispatch,
        maximumStopFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumeGenerationExhausted,
        maximumRunLoop.State == ProcessorKernelRunLoopState::Faulted, maximumRunLoop.ExternalFrameRetained == 1U,
        maximumRetained.HasValue(), ExternalStopObservationsAreEqual(maximumRetained.GetValue(), maximumObservation),
        maximumAfter.State == maximumBefore.State,
        maximumAfter.Scheduler.ActiveThread.IsEqualTo(maximumBefore.Scheduler.ActiveThread),
        maximumAfter.Scheduler.Queue.ReadyEntries == maximumBefore.Scheduler.Queue.ReadyEntries,
        maximumAfter.Scheduler.Queue.NextSequence == maximumBefore.Scheduler.Queue.NextSequence,
        maximumAfter.Deadline.SelectedDeadlineTicks == maximumBefore.Deadline.SelectedDeadlineTicks,
        maximumAddressAfter.ActiveStorage == maximumAddressBefore.ActiveStorage,
        maximumAddressAfter.Active.IsEqualTo(maximumAddressBefore.Active), maximumReadsAfter == maximumReadsBefore,
        maximumProgramsAfter == maximumProgramsBefore, maximumCancelsAfter == maximumCancelsBefore,
        maximumAcknowledgementsAfter == maximumAcknowledgementsBefore,
        maximumIdleSwitchesAfter == maximumIdleSwitchesBefore,
        maximumTargetSwitchesAfter == maximumTargetSwitchesBefore, !maximumRemoved,
        maximumAfterRemoval.Scheduler.Queue.NextSequence == maximumBeforeRemoval.Scheduler.Queue.NextSequence,
        maximumUnbound == ProcessorKernelRunLoopError::NotIdle);

    alignas(ExternalRunLoopFixture) static std::byte multiStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const multiFixture = ::new (static_cast<void*>(multiStorage)) ExternalRunLoopFixture{};
    alignas(RegisteredPair) static std::byte secondPairStorage[sizeof(RegisteredPair)]{};
    RegisteredPair* const secondPair =
        ::new (static_cast<void*>(secondPairStorage)) RegisteredPair{multiFixture->Registry, multiFixture->Issuer};
    static CooperativeKernelContext secondContext{true};
    ProcessorKernelRunLoopFailure secondEnqueueFailure{};
    const bool multiPrepared =
        multiFixture->PrepareAdmission() && secondPair->Ready &&
        (multiFixture->RunLoop.Bind(secondPair->ThreadStorage, secondContext, multiFixture->TargetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        multiFixture->RunLoop.EnqueueBoundThread(secondPair->ContextIdentity, secondPair->Context,
                                                 secondPair->ThreadIdentity, secondPair->ThreadStorage,
                                                 secondEnqueueFailure);
    multiFixture->IdleContext.SeedExternalFrameIdleContinuationGeneration(maximumGeneration - 1U);
    std::atomic<bool> firstMultiStop{false};
    std::atomic<bool> secondMultiStop{false};
    bool exhaustedFinishAccepted{false};
    ProcessorKernelRunLoopFailure exhaustedFinishFailure{};
    Kernel::Scheduler::ProcessorRunSnapshot exhaustedBefore{};
    Kernel::Scheduler::ProcessorRunSnapshot exhaustedAfter{};
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot exhaustedAddressBefore{};
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot exhaustedAddressAfter{};
    Size exhaustedReadsBefore{0U};
    Size exhaustedProgramsBefore{0U};
    Size exhaustedCancelsBefore{0U};
    Size exhaustedAcknowledgementsBefore{0U};
    Size exhaustedSwitchesBefore{0U};
    Size exhaustedReadsAfter{0U};
    Size exhaustedProgramsAfter{0U};
    Size exhaustedCancelsAfter{0U};
    Size exhaustedAcknowledgementsAfter{0U};
    Size exhaustedSwitchesAfter{0U};
    std::thread firstMultiWorker{
        [&]() noexcept
        {
            if (multiFixture->TargetContext.WaitForResume() != FakeContextError::None)
            {
                multiFixture->IdleContext.Resume();
                return;
            }
            firstMultiStop.store(
                multiFixture->RunLoop
                    .CompleteExternallyStoppedRunAsReady(multiFixture->Pair.ThreadStorage, multiFixture->TargetContext,
                                                         MakeExternalStop(multiFixture->Controller, 30U, 0x702U))
                    .HasValue(),
                std::memory_order_release);
            multiFixture->IdleContext.Resume();
            if (multiFixture->TargetContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            exhaustedBefore = multiFixture->Controller.GetSnapshot();
            exhaustedAddressBefore = multiFixture->AddressSpaces.GetSnapshot();
            exhaustedReadsBefore = multiFixture->Timer.GetReadCalls();
            exhaustedProgramsBefore = multiFixture->Timer.GetProgramCalls();
            exhaustedCancelsBefore = multiFixture->Timer.GetCancelCalls();
            exhaustedAcknowledgementsBefore = multiFixture->Timer.GetAcknowledgeCalls();
            exhaustedSwitchesBefore = multiFixture->TargetContext.GetSwitchCount();
            ProcessorKernelRunLoopReceipt receipt{};
            exhaustedFinishAccepted = multiFixture->RunLoop.FinishAndDispatch(
                multiFixture->Pair.ThreadStorage, multiFixture->TargetContext, ThreadRunDisposition::Exited, 0U,
                receipt, exhaustedFinishFailure);
            exhaustedAfter = multiFixture->Controller.GetSnapshot();
            exhaustedAddressAfter = multiFixture->AddressSpaces.GetSnapshot();
            exhaustedReadsAfter = multiFixture->Timer.GetReadCalls();
            exhaustedProgramsAfter = multiFixture->Timer.GetProgramCalls();
            exhaustedCancelsAfter = multiFixture->Timer.GetCancelCalls();
            exhaustedAcknowledgementsAfter = multiFixture->Timer.GetAcknowledgeCalls();
            exhaustedSwitchesAfter = multiFixture->TargetContext.GetSwitchCount();
        }};
    std::thread secondMultiWorker{
        [&]() noexcept
        {
            if (secondContext.WaitForResume() != FakeContextError::None)
            {
                multiFixture->IdleContext.Resume();
                return;
            }
            secondMultiStop.store(
                multiFixture->RunLoop
                    .CompleteExternallyStoppedRunAsReady(secondPair->ThreadStorage, secondContext,
                                                         MakeExternalStop(multiFixture->Controller, 120U, 0x703U))
                    .HasValue(),
                std::memory_order_release);
            multiFixture->IdleContext.Resume();
        }};
    ProcessorKernelRunLoopReceipt firstMultiIdleReceipt{};
    ProcessorKernelRunLoopFailure firstMultiIdleFailure{};
    const bool firstMultiIdle =
        multiPrepared && multiFixture->RunLoop.DispatchFromIdle(firstMultiIdleReceipt, firstMultiIdleFailure);
    multiFixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt secondMultiIdleReceipt{};
    ProcessorKernelRunLoopFailure secondMultiIdleFailure{};
    const bool secondMultiIdle =
        firstMultiIdle && multiFixture->RunLoop.DispatchFromIdle(secondMultiIdleReceipt, secondMultiIdleFailure);
    secondMultiWorker.join();
    const auto firstRetainedBeforeSelection =
        multiFixture->RunLoop.GetRetainedExternalFrame(multiFixture->Pair.ThreadStorage);
    const auto secondRetainedBeforeSelection =
        multiFixture->RunLoop.GetRetainedExternalFrame(secondPair->ThreadStorage);
    multiFixture->Timer.SetTicks(200U);
    ProcessorKernelRunLoopReceipt maximumResume{};
    ProcessorKernelRunLoopFailure maximumAdmissionFailure{};
    const bool maximumSelected =
        secondMultiIdle && multiFixture->RunLoop.DispatchFromIdle(maximumResume, maximumAdmissionFailure);
    ProcessorKernelRunLoopFailure maximumRefreshFailure{};
    const bool maximumRefreshed = maximumSelected && multiFixture->RunLoop.RefreshExternalFrameIdleContinuation(
                                                         maximumResume, maximumRefreshFailure);
    ProcessorKernelRunLoopFailure maximumCommitFailure{};
    const bool maximumCommitted = maximumRefreshed && multiFixture->RunLoop.CommitExternalFrameResume(
                                                          multiFixture->Pair.ThreadStorage, multiFixture->TargetContext,
                                                          maximumResume, maximumCommitFailure);
    if (maximumCommitted)
    {
        multiFixture->TargetContext.Resume();
    }
    firstMultiWorker.join();
    const auto strandedRetained = multiFixture->RunLoop.GetRetainedExternalFrame(secondPair->ThreadStorage);
    const auto exhaustedRunLoop = multiFixture->RunLoop.GetSnapshot();
    const auto exhaustedBeforeRemoval = multiFixture->Controller.GetSnapshot();
    ReadyQueueState strandedRemovalState{};
    ProcessorKernelRunLoopFailure strandedRemovalFailure{};
    const bool strandedRemoved = multiFixture->RunLoop.RemoveBoundThread(secondPair->ThreadStorage,
                                                                         strandedRemovalState, strandedRemovalFailure);
    const auto exhaustedAfterRemoval = multiFixture->Controller.GetSnapshot();
    const auto strandedUnbind = multiFixture->RunLoop.Unbind(secondPair->ThreadStorage);

    ASTERIA_EXPECT(
        runner,
        "generation exhaustion after one maximum receipt fail-stops without "
        "discarding or removing another binding's retained frame",
        multiPrepared, firstMultiIdle, secondMultiIdle, firstMultiStop.load(std::memory_order_acquire),
        secondMultiStop.load(std::memory_order_acquire), firstRetainedBeforeSelection.HasValue(),
        secondRetainedBeforeSelection.HasValue(), maximumSelected,
        maximumResume.ExternalFrameResumeGeneration == maximumGeneration, maximumRefreshed, maximumCommitted,
        !exhaustedFinishAccepted,
        exhaustedFinishFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumeGenerationExhausted,
        exhaustedRunLoop.State == ProcessorKernelRunLoopState::Faulted, exhaustedRunLoop.ExternalFrameRetained == 1U,
        strandedRetained.HasValue(), strandedRetained.GetValue().ExternalFrameToken == 0x703U,
        exhaustedAfter.State == exhaustedBefore.State,
        exhaustedAfter.Scheduler.ActiveThread.IsEqualTo(exhaustedBefore.Scheduler.ActiveThread),
        exhaustedAfter.Scheduler.Queue.ReadyEntries == exhaustedBefore.Scheduler.Queue.ReadyEntries,
        exhaustedAfter.Scheduler.Queue.NextSequence == exhaustedBefore.Scheduler.Queue.NextSequence,
        exhaustedAfter.Deadline.SelectedDeadlineTicks == exhaustedBefore.Deadline.SelectedDeadlineTicks,
        exhaustedAddressAfter.ActiveStorage == exhaustedAddressBefore.ActiveStorage,
        exhaustedAddressAfter.Active.IsEqualTo(exhaustedAddressBefore.Active),
        exhaustedReadsAfter == exhaustedReadsBefore, exhaustedProgramsAfter == exhaustedProgramsBefore,
        exhaustedCancelsAfter == exhaustedCancelsBefore,
        exhaustedAcknowledgementsAfter == exhaustedAcknowledgementsBefore,
        exhaustedSwitchesAfter == exhaustedSwitchesBefore, !strandedRemoved,
        exhaustedAfterRemoval.Scheduler.Queue.NextSequence == exhaustedBeforeRemoval.Scheduler.Queue.NextSequence,
        strandedUnbind == ProcessorKernelRunLoopError::NotIdle);

    // Both fixtures intentionally preserve terminal generation-exhaustion
    // evidence, including the independently retained second frame.
}

void TestExternalFrameAdmissionRollbackRetainsOwnership(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte fixtureStorage[2U][sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const rootFixture = ::new (static_cast<void*>(fixtureStorage[0U])) ExternalRunLoopFixture{};
    ExternalRunLoopFixture* const deadlineFixture =
        ::new (static_cast<void*>(fixtureStorage[1U])) ExternalRunLoopFixture{};

    const bool rootRetained = RetainExternalFrameAtIdle(*rootFixture, 0x401U);
    const auto rootBefore = rootFixture->Controller.GetSnapshot();
    rootFixture->TargetProtection.PageTable.NextActivationError =
        AddressSpacePageTableActivationError::RejectedBeforeCommit;
    rootFixture->Timer.SetTicks(100U);
    const Size rootDestinations = rootFixture->TargetContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopReceipt rootReceipt{};
    ProcessorKernelRunLoopFailure rootFailure{};
    const bool rootSelected = rootFixture->RunLoop.DispatchFromIdle(rootReceipt, rootFailure);
    const auto rootAfter = rootFixture->Controller.GetSnapshot();
    const auto rootRunLoopAfter = rootFixture->RunLoop.GetSnapshot();
    const auto rootAddressAfter = rootFixture->AddressSpaces.GetSnapshot();
    ReadyQueueState rootRemovalState{};
    ProcessorKernelRunLoopFailure rootRemovalFailure{};
    const bool rootRemoved =
        rootFixture->RunLoop.RemoveBoundThread(rootFixture->Pair.ThreadStorage, rootRemovalState, rootRemovalFailure);
    const auto rootAfterRemoval = rootFixture->Controller.GetSnapshot();

    const bool deadlineRetained = RetainExternalFrameAtIdle(*deadlineFixture, 0x402U);
    const auto deadlineBefore = deadlineFixture->Controller.GetSnapshot();
    deadlineFixture->TargetProtection.PageTable.ActivationTimer = &deadlineFixture->Timer;
    deadlineFixture->TargetProtection.PageTable.ActivationTicks = 120U;
    deadlineFixture->Timer.SetTicks(100U);
    const Size deadlineDestinations = deadlineFixture->TargetContext.GetDestinationSwitchCount();
    ProcessorKernelRunLoopReceipt deadlineReceipt{};
    ProcessorKernelRunLoopFailure deadlineFailure{};
    const bool deadlineSelected = deadlineFixture->RunLoop.DispatchFromIdle(deadlineReceipt, deadlineFailure);
    const auto deadlineAfter = deadlineFixture->Controller.GetSnapshot();
    const auto deadlineRunLoopAfter = deadlineFixture->RunLoop.GetSnapshot();
    const auto deadlineAddressAfter = deadlineFixture->AddressSpaces.GetSnapshot();
    ReadyQueueState deadlineRemovalState{};
    ProcessorKernelRunLoopFailure deadlineRemovalFailure{};
    const bool deadlineRemoved = deadlineFixture->RunLoop.RemoveBoundThread(
        deadlineFixture->Pair.ThreadStorage, deadlineRemovalState, deadlineRemovalFailure);
    const auto deadlineAfterRemoval = deadlineFixture->Controller.GetSnapshot();

    ASTERIA_EXPECT(runner,
                   "a rollback-safe root rejection takes one fresh FIFO position "
                   "without discarding retained-frame ownership",
                   rootRetained, !rootSelected,
                   rootFailure.Error == ProcessorKernelRunLoopError::AddressSpaceControllerRejected,
                   rootFailure.AddressSpaceController.AdmissionRollbackSafe,
                   rootRunLoopAfter.State == ProcessorKernelRunLoopState::Idle,
                   rootRunLoopAfter.ExternalFrameRetained == 1U, rootRunLoopAfter.ExternalFrameResumePending == 0U,
                   rootAfter.State == ProcessorRunState::Idle, rootAfter.Scheduler.Queue.ReadyEntries == 1U,
                   rootAfter.Scheduler.Queue.NextSequence == (rootBefore.Scheduler.Queue.NextSequence + 1U),
                   rootFixture->Pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   rootAddressAfter.ActiveStorage == &rootFixture->IdleProtection.AddressSpaceStorage,
                   rootFixture->TargetContext.GetDestinationSwitchCount() == rootDestinations, !rootRemoved,
                   rootRemovalFailure.Error == ProcessorKernelRunLoopError::ExternalFrameRetained,
                   rootAfterRemoval.Scheduler.Queue.NextSequence == rootAfter.Scheduler.Queue.NextSequence);

    ASTERIA_EXPECT(runner,
                   "a final deadline rollback takes one fresh FIFO position and "
                   "retains external-frame ownership for normal-policy retry",
                   deadlineRetained, !deadlineSelected,
                   deadlineFailure.Error == ProcessorKernelRunLoopError::ControllerEntryRejected,
                   deadlineFailure.ControllerError == ProcessorRunError::DeadlineReached,
                   deadlineRunLoopAfter.State == ProcessorKernelRunLoopState::Idle,
                   deadlineRunLoopAfter.ExternalFrameRetained == 1U,
                   deadlineRunLoopAfter.ExternalFrameResumePending == 0U,
                   deadlineAfter.State == ProcessorRunState::Idle, deadlineAfter.Scheduler.Queue.ReadyEntries == 1U,
                   deadlineAfter.Scheduler.Queue.NextSequence == (deadlineBefore.Scheduler.Queue.NextSequence + 1U),
                   deadlineFixture->Pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   deadlineAddressAfter.ActiveStorage == &deadlineFixture->IdleProtection.AddressSpaceStorage,
                   deadlineFixture->TargetContext.GetDestinationSwitchCount() == deadlineDestinations, !deadlineRemoved,
                   deadlineRemovalFailure.Error == ProcessorKernelRunLoopError::ExternalFrameRetained,
                   deadlineAfterRemoval.Scheduler.Queue.NextSequence == deadlineAfter.Scheduler.Queue.NextSequence);

    // Both fixtures intentionally retain their rollback-safe frame evidence.
}

void TestBoundRemovalGuards(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) static std::byte activeStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const activeFixture = ::new (static_cast<void*>(activeStorage)) ExternalRunLoopFixture{};
    const bool activePrepared = activeFixture->PrepareAdmission();
    bool activeRemoved{false};
    bool activeStopAccepted{false};
    ProcessorKernelRunLoopFailure activeRemovalFailure{};
    Kernel::Scheduler::ProcessorRunSnapshot activeBefore{};
    Kernel::Scheduler::ProcessorRunSnapshot activeAfter{};
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot activeAddressBefore{};
    Kernel::Memory::ProcessorAddressSpaceControllerSnapshot activeAddressAfter{};
    Size activeReadsBefore{0U};
    Size activeProgramsBefore{0U};
    Size activeCancelsBefore{0U};
    Size activeAcknowledgementsBefore{0U};
    Size activeIdleSwitchesBefore{0U};
    Size activeTargetSwitchesBefore{0U};
    Size activeReadsAfter{0U};
    Size activeProgramsAfter{0U};
    Size activeCancelsAfter{0U};
    Size activeAcknowledgementsAfter{0U};
    Size activeIdleSwitchesAfter{0U};
    Size activeTargetSwitchesAfter{0U};
    std::thread activeWorker{[&]() noexcept
                             {
                                 if (activeFixture->TargetContext.WaitForResume() != FakeContextError::None)
                                 {
                                     activeFixture->IdleContext.Resume();
                                     return;
                                 }
                                 activeBefore = activeFixture->Controller.GetSnapshot();
                                 activeAddressBefore = activeFixture->AddressSpaces.GetSnapshot();
                                 activeReadsBefore = activeFixture->Timer.GetReadCalls();
                                 activeProgramsBefore = activeFixture->Timer.GetProgramCalls();
                                 activeCancelsBefore = activeFixture->Timer.GetCancelCalls();
                                 activeAcknowledgementsBefore = activeFixture->Timer.GetAcknowledgeCalls();
                                 activeIdleSwitchesBefore = activeFixture->IdleContext.GetSwitchCount();
                                 activeTargetSwitchesBefore = activeFixture->TargetContext.GetSwitchCount();
                                 ReadyQueueState removalState{};
                                 activeRemoved = activeFixture->RunLoop.RemoveBoundThread(
                                     activeFixture->Pair.ThreadStorage, removalState, activeRemovalFailure);
                                 activeAfter = activeFixture->Controller.GetSnapshot();
                                 activeAddressAfter = activeFixture->AddressSpaces.GetSnapshot();
                                 activeReadsAfter = activeFixture->Timer.GetReadCalls();
                                 activeProgramsAfter = activeFixture->Timer.GetProgramCalls();
                                 activeCancelsAfter = activeFixture->Timer.GetCancelCalls();
                                 activeAcknowledgementsAfter = activeFixture->Timer.GetAcknowledgeCalls();
                                 activeIdleSwitchesAfter = activeFixture->IdleContext.GetSwitchCount();
                                 activeTargetSwitchesAfter = activeFixture->TargetContext.GetSwitchCount();
                                 activeStopAccepted =
                                     activeFixture->RunLoop
                                         .CompleteExternallyStoppedRunAsReady(
                                             activeFixture->Pair.ThreadStorage, activeFixture->TargetContext,
                                             MakeExternalStop(activeFixture->Controller, 30U, 0x511U))
                                         .HasValue();
                                 activeFixture->IdleContext.Resume();
                             }};
    ProcessorKernelRunLoopReceipt activeIdleReceipt{};
    ProcessorKernelRunLoopFailure activeIdleFailure{};
    const bool activeReturned =
        activePrepared && activeFixture->RunLoop.DispatchFromIdle(activeIdleReceipt, activeIdleFailure);
    activeWorker.join();

    ASTERIA_EXPECT(
        runner,
        "removal of the exact active owner is rejected before queue, timer, "
        "root, or native context mutation",
        activeReturned, activeStopAccepted, !activeRemoved,
        activeRemovalFailure.Error == ProcessorKernelRunLoopError::WrongActiveThread,
        activeAfter.State == activeBefore.State,
        activeAfter.Scheduler.ActiveThread.IsEqualTo(activeBefore.Scheduler.ActiveThread),
        activeAfter.Scheduler.Queue.ReadyEntries == activeBefore.Scheduler.Queue.ReadyEntries,
        activeAfter.Scheduler.Queue.NextSequence == activeBefore.Scheduler.Queue.NextSequence,
        activeAfter.Deadline.SelectedDeadlineTicks == activeBefore.Deadline.SelectedDeadlineTicks,
        activeAddressAfter.ActiveStorage == activeAddressBefore.ActiveStorage,
        activeAddressAfter.Active.IsEqualTo(activeAddressBefore.Active), activeReadsAfter == activeReadsBefore,
        activeProgramsAfter == activeProgramsBefore, activeCancelsAfter == activeCancelsBefore,
        activeAcknowledgementsAfter == activeAcknowledgementsBefore,
        activeIdleSwitchesAfter == activeIdleSwitchesBefore, activeTargetSwitchesAfter == activeTargetSwitchesBefore);

    alignas(ExternalRunLoopFixture) static std::byte pendingStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const pendingFixture = ::new (static_cast<void*>(pendingStorage)) ExternalRunLoopFixture{};
    alignas(RegisteredPair) static std::byte unrelatedPairStorage[sizeof(RegisteredPair)]{};
    RegisteredPair* const unrelatedPair = ::new (static_cast<void*>(unrelatedPairStorage))
        RegisteredPair{pendingFixture->Registry, pendingFixture->Issuer};
    static CooperativeKernelContext unrelatedContext{true};
    ProcessorKernelRunLoopFailure unrelatedEnqueueFailure{};
    const bool pendingSetup =
        RetainExternalFrameAtIdle(*pendingFixture, 0x512U) && unrelatedPair->Ready &&
        (pendingFixture->RunLoop.Bind(unrelatedPair->ThreadStorage, unrelatedContext,
                                      pendingFixture->TargetProtection.Domain) == ProcessorKernelRunLoopError::None) &&
        pendingFixture->RunLoop.EnqueueBoundThread(unrelatedPair->ContextIdentity, unrelatedPair->Context,
                                                   unrelatedPair->ThreadIdentity, unrelatedPair->ThreadStorage,
                                                   unrelatedEnqueueFailure);
    pendingFixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt pendingReceipt{};
    ProcessorKernelRunLoopFailure pendingAdmissionFailure{};
    const bool pendingSelected =
        pendingSetup && pendingFixture->RunLoop.DispatchFromIdle(pendingReceipt, pendingAdmissionFailure);
    const auto pendingBefore = pendingFixture->Controller.GetSnapshot();
    const auto pendingAddressBefore = pendingFixture->AddressSpaces.GetSnapshot();
    const Size pendingReads = pendingFixture->Timer.GetReadCalls();
    const Size pendingPrograms = pendingFixture->Timer.GetProgramCalls();
    const Size pendingCancels = pendingFixture->Timer.GetCancelCalls();
    const Size pendingAcknowledgements = pendingFixture->Timer.GetAcknowledgeCalls();
    const Size pendingIdleSwitches = pendingFixture->IdleContext.GetSwitchCount();
    const Size pendingTargetSwitches = pendingFixture->TargetContext.GetSwitchCount();
    const Size pendingUnrelatedSwitches = unrelatedContext.GetSwitchCount();
    ReadyQueueState pendingRemovalState{};
    ProcessorKernelRunLoopFailure pendingRemovalFailure{};
    const bool pendingRemoved =
        pendingSelected && pendingFixture->RunLoop.RemoveBoundThread(unrelatedPair->ThreadStorage, pendingRemovalState,
                                                                     pendingRemovalFailure);
    const auto pendingAfter = pendingFixture->Controller.GetSnapshot();
    const auto pendingAddressAfter = pendingFixture->AddressSpaces.GetSnapshot();
    const auto pendingRunLoopAfter = pendingFixture->RunLoop.GetSnapshot();
    const auto pendingRetained = pendingFixture->RunLoop.GetRetainedExternalFrame(pendingFixture->Pair.ThreadStorage);

    ASTERIA_EXPECT(runner,
                   "an unrelated Ready owner cannot be removed while an exact external "
                   "resume action is pending",
                   pendingSelected, pendingReceipt.ResumeKind == ProcessorKernelRunLoopResumeKind::ExternalFrame,
                   !pendingRemoved,
                   pendingRemovalFailure.Error == ProcessorKernelRunLoopError::ExternalFrameResumePending,
                   pendingRunLoopAfter.State == ProcessorKernelRunLoopState::Faulted, pendingRetained.HasValue(),
                   pendingRetained.GetValue().ExternalFrameToken == 0x512U, pendingAfter.State == pendingBefore.State,
                   pendingAfter.Scheduler.ActiveThread.IsEqualTo(pendingBefore.Scheduler.ActiveThread),
                   pendingAfter.Scheduler.Queue.ReadyEntries == pendingBefore.Scheduler.Queue.ReadyEntries,
                   pendingAfter.Scheduler.Queue.NextSequence == pendingBefore.Scheduler.Queue.NextSequence,
                   pendingAfter.Deadline.SelectedDeadlineTicks == pendingBefore.Deadline.SelectedDeadlineTicks,
                   pendingAddressAfter.ActiveStorage == pendingAddressBefore.ActiveStorage,
                   pendingAddressAfter.Active.IsEqualTo(pendingAddressBefore.Active),
                   pendingFixture->Timer.GetReadCalls() == pendingReads,
                   pendingFixture->Timer.GetProgramCalls() == pendingPrograms,
                   pendingFixture->Timer.GetCancelCalls() == pendingCancels,
                   pendingFixture->Timer.GetAcknowledgeCalls() == pendingAcknowledgements,
                   pendingFixture->IdleContext.GetSwitchCount() == pendingIdleSwitches,
                   pendingFixture->TargetContext.GetSwitchCount() == pendingTargetSwitches,
                   unrelatedContext.GetSwitchCount() == pendingUnrelatedSwitches);

    alignas(ExternalRunLoopFixture) static std::byte routedStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const routedFixture = ::new (static_cast<void*>(routedStorage)) ExternalRunLoopFixture{};
    alignas(RegisteredPair) static std::byte routedPeerStorage[sizeof(RegisteredPair)]{};
    RegisteredPair* const routedPeer =
        ::new (static_cast<void*>(routedPeerStorage)) RegisteredPair{routedFixture->Registry, routedFixture->Issuer};
    static CooperativeKernelContext routedPeerContext{true};
    ProcessorKernelRunLoopFailure routedEnqueueFailure{};
    const bool routedSetup =
        routedFixture->PrepareAdmission() && routedPeer->Ready &&
        (routedFixture->RunLoop.Bind(routedPeer->ThreadStorage, routedPeerContext,
                                     routedFixture->TargetProtection.Domain) == ProcessorKernelRunLoopError::None) &&
        routedFixture->RunLoop.EnqueueBoundThread(routedPeer->ContextIdentity, routedPeer->Context,
                                                  routedPeer->ThreadIdentity, routedPeer->ThreadStorage,
                                                  routedEnqueueFailure);
    struct PendingRemovalAttempt final
    {
        ExternalRunLoopFixture* Fixture;
        Thread* ThreadStorage;
        CooperativeKernelContext* PeerContext;
        ProcessorKernelRunLoopFailure Failure;
        Kernel::Scheduler::ProcessorRunSnapshot ControllerBefore;
        Kernel::Scheduler::ProcessorRunSnapshot ControllerAfter;
        Kernel::Memory::ProcessorAddressSpaceControllerSnapshot AddressBefore;
        Kernel::Memory::ProcessorAddressSpaceControllerSnapshot AddressAfter;
        ProcessorKernelRunLoopSnapshot RunLoopBefore;
        Size ReadsBefore;
        Size ProgramsBefore;
        Size CancelsBefore;
        Size AcknowledgementsBefore;
        Size IdleSwitchesBefore;
        Size PeerSwitchesBefore;
        Size ReadsAfter;
        Size ProgramsAfter;
        Size CancelsAfter;
        Size AcknowledgementsAfter;
        Size IdleSwitchesAfter;
        Size PeerSwitchesAfter;
        bool Removed;
    } pendingAttempt{
        .Fixture = routedFixture,
        .ThreadStorage = &routedPeer->ThreadStorage,
        .PeerContext = &routedPeerContext,
        .Failure = {},
        .ControllerBefore = {},
        .ControllerAfter = {},
        .AddressBefore = {},
        .AddressAfter = {},
        .RunLoopBefore = {},
        .ReadsBefore = 0U,
        .ProgramsBefore = 0U,
        .CancelsBefore = 0U,
        .AcknowledgementsBefore = 0U,
        .IdleSwitchesBefore = 0U,
        .PeerSwitchesBefore = 0U,
        .ReadsAfter = 0U,
        .ProgramsAfter = 0U,
        .CancelsAfter = 0U,
        .AcknowledgementsAfter = 0U,
        .IdleSwitchesAfter = 0U,
        .PeerSwitchesAfter = 0U,
        .Removed = false,
    };
    routedPeerContext.InvokeOnNextSwitch(
        [](void* const opaque) noexcept
        {
            auto& attempt = *static_cast<PendingRemovalAttempt*>(opaque);
            attempt.ControllerBefore = attempt.Fixture->Controller.GetSnapshot();
            attempt.AddressBefore = attempt.Fixture->AddressSpaces.GetSnapshot();
            attempt.RunLoopBefore = attempt.Fixture->RunLoop.GetSnapshot();
            attempt.ReadsBefore = attempt.Fixture->Timer.GetReadCalls();
            attempt.ProgramsBefore = attempt.Fixture->Timer.GetProgramCalls();
            attempt.CancelsBefore = attempt.Fixture->Timer.GetCancelCalls();
            attempt.AcknowledgementsBefore = attempt.Fixture->Timer.GetAcknowledgeCalls();
            attempt.IdleSwitchesBefore = attempt.Fixture->IdleContext.GetSwitchCount();
            attempt.PeerSwitchesBefore = attempt.PeerContext->GetSwitchCount();
            ReadyQueueState state{};
            attempt.Removed =
                attempt.Fixture->RunLoop.RemoveBoundThread(*attempt.ThreadStorage, state, attempt.Failure);
            attempt.ControllerAfter = attempt.Fixture->Controller.GetSnapshot();
            attempt.AddressAfter = attempt.Fixture->AddressSpaces.GetSnapshot();
            attempt.ReadsAfter = attempt.Fixture->Timer.GetReadCalls();
            attempt.ProgramsAfter = attempt.Fixture->Timer.GetProgramCalls();
            attempt.CancelsAfter = attempt.Fixture->Timer.GetCancelCalls();
            attempt.AcknowledgementsAfter = attempt.Fixture->Timer.GetAcknowledgeCalls();
            attempt.IdleSwitchesAfter = attempt.Fixture->IdleContext.GetSwitchCount();
            attempt.PeerSwitchesAfter = attempt.PeerContext->GetSwitchCount();
        },
        &pendingAttempt);
    std::atomic<bool> routedStopAccepted{false};
    std::thread routedWorker{
        [&]() noexcept
        {
            if (routedFixture->TargetContext.WaitForResume() != FakeContextError::None)
            {
                routedFixture->IdleContext.Resume();
                return;
            }
            routedStopAccepted.store(routedFixture->RunLoop
                                         .CompleteExternallyStoppedRunAsReady(
                                             routedFixture->Pair.ThreadStorage, routedFixture->TargetContext,
                                             MakeExternalStop(routedFixture->Controller, 30U, 0x513U))
                                         .HasValue(),
                                     std::memory_order_release);
            routedFixture->IdleContext.Resume();
        }};
    std::thread routedPeerWorker{
        [&]() noexcept
        {
            if (routedPeerContext.WaitForResume() != FakeContextError::None)
            {
                routedFixture->IdleContext.Resume();
                return;
            }
            ProcessorKernelRunLoopReceipt receipt{};
            ProcessorKernelRunLoopFailure failure{};
            static_cast<void>(routedFixture->RunLoop.FinishAndDispatch(
                routedPeer->ThreadStorage, routedPeerContext, ThreadRunDisposition::Exited, 0U, receipt, failure));
        }};
    ProcessorKernelRunLoopReceipt routedFirstIdleReceipt{};
    ProcessorKernelRunLoopFailure routedFirstIdleFailure{};
    const bool routedFirstIdle =
        routedSetup && routedFixture->RunLoop.DispatchFromIdle(routedFirstIdleReceipt, routedFirstIdleFailure);
    routedWorker.join();
    routedFixture->Timer.SetTicks(100U);
    ProcessorKernelRunLoopReceipt routedSecondIdleReceipt{};
    ProcessorKernelRunLoopFailure routedSecondIdleFailure{};
    const bool routedSecondIdle =
        routedFirstIdle && routedFixture->RunLoop.DispatchFromIdle(routedSecondIdleReceipt, routedSecondIdleFailure);
    routedPeerWorker.detach();
    const auto routedRunLoopAfter = routedFixture->RunLoop.GetSnapshot();
    const auto routedRetained = routedFixture->RunLoop.GetRetainedExternalFrame(routedFixture->Pair.ThreadStorage);

    ASTERIA_EXPECT(runner,
                   "removal during the B-to-idle ExternalFramePending corridor is "
                   "terminal before the selected admission is activated or mutated",
                   routedFirstIdle, routedStopAccepted.load(std::memory_order_acquire), !routedSecondIdle,
                   pendingAttempt.RunLoopBefore.State == ProcessorKernelRunLoopState::ExternalFramePending,
                   !pendingAttempt.Removed,
                   pendingAttempt.Failure.Error == ProcessorKernelRunLoopError::ExternalFrameResumePending,
                   routedRunLoopAfter.State == ProcessorKernelRunLoopState::Faulted, routedRetained.HasValue(),
                   routedRetained.GetValue().ExternalFrameToken == 0x513U,
                   pendingAttempt.ControllerAfter.State == pendingAttempt.ControllerBefore.State,
                   pendingAttempt.ControllerAfter.Scheduler.ActiveThread.IsEqualTo(
                       pendingAttempt.ControllerBefore.Scheduler.ActiveThread),
                   pendingAttempt.ControllerAfter.Scheduler.Queue.ReadyEntries ==
                       pendingAttempt.ControllerBefore.Scheduler.Queue.ReadyEntries,
                   pendingAttempt.ControllerAfter.Scheduler.Queue.NextSequence ==
                       pendingAttempt.ControllerBefore.Scheduler.Queue.NextSequence,
                   pendingAttempt.ControllerAfter.Deadline.SelectedDeadlineTicks ==
                       pendingAttempt.ControllerBefore.Deadline.SelectedDeadlineTicks,
                   pendingAttempt.AddressAfter.ActiveStorage == pendingAttempt.AddressBefore.ActiveStorage,
                   pendingAttempt.AddressAfter.Active.IsEqualTo(pendingAttempt.AddressBefore.Active),
                   pendingAttempt.ReadsAfter == pendingAttempt.ReadsBefore,
                   pendingAttempt.ProgramsAfter == pendingAttempt.ProgramsBefore,
                   pendingAttempt.CancelsAfter == pendingAttempt.CancelsBefore,
                   pendingAttempt.AcknowledgementsAfter == pendingAttempt.AcknowledgementsBefore,
                   pendingAttempt.IdleSwitchesAfter == pendingAttempt.IdleSwitchesBefore,
                   pendingAttempt.PeerSwitchesAfter == pendingAttempt.PeerSwitchesBefore);

    // All fixtures deliberately retain architecture-frame evidence.
}

void TestBindings(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    FakeKernelContext first{true};
    FakeKernelContext second{true};
    FakeKernelContext unprepared;
    Thread firstThread;
    Thread secondThread;
    Thread uninitialized;
    ProtectionDomain unreadyDomain;
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    const auto idleBinding = runLoop.GetActiveProtectionDomainBinding();

    ASTERIA_EXPECT(runner, "idle run loops reject active protection-domain binding access", !idleBinding.HasValue(),
                   idleBinding.GetError() == ProcessorKernelRunLoopError::NotActive);

    ASTERIA_EXPECT(
        runner, "bindings are prepared, initialized, unique, and idle-owned", protection.Ready,
        addressInitialization.HasValue(),
        runLoop.Bind(uninitialized, first, protection.Domain) == ProcessorKernelRunLoopError::ThreadNotInitialized,
        firstThread.Initialize(ResourceDomainId{1U}) == ThreadError::None,
        secondThread.Initialize(ResourceDomainId{2U}) == ThreadError::None,
        runLoop.Bind(firstThread, unprepared, protection.Domain) == ProcessorKernelRunLoopError::ContextNotPrepared,
        runLoop.Bind(firstThread, first, unreadyDomain) == ProcessorKernelRunLoopError::ProtectionDomainNotReady,
        runLoop.Bind(firstThread, first, protection.Domain) == ProcessorKernelRunLoopError::None,
        runLoop.Bind(firstThread, second, protection.Domain) == ProcessorKernelRunLoopError::ThreadAlreadyBound,
        runLoop.Bind(secondThread, first, protection.Domain) == ProcessorKernelRunLoopError::ContextAlreadyBound,
        runLoop.Bind(secondThread, idle, protection.Domain) == ProcessorKernelRunLoopError::ContextAlreadyBound,
        runLoop.GetSnapshot().BindingCount == 1U,
        runLoop.Unbind(secondThread) == ProcessorKernelRunLoopError::ThreadNotBound,
        runLoop.Unbind(firstThread) == ProcessorKernelRunLoopError::None, runLoop.GetSnapshot().BindingCount == 0U);

    const auto unavailableBinding = runLoop.Bind(secondThread, second, protection.Domain);
    const auto unavailableBefore = controller.GetSnapshot();
    const auto unavailableAddressBefore = addressSpaces.GetSnapshot();
    const Size unavailableReads = timer.GetReadCalls();
    const Size unavailablePrograms = timer.GetProgramCalls();
    const Size unavailableCancels = timer.GetCancelCalls();
    const Size unavailableAcknowledgements = timer.GetAcknowledgeCalls();
    const Size unavailableSwitches = idle.GetSwitchCount();
    ReadyQueueState unavailableState{};
    ProcessorKernelRunLoopFailure unavailableFailure{};
    const bool unavailableRemoved = runLoop.RemoveBoundThread(secondThread, unavailableState, unavailableFailure);
    const auto unavailableAfter = controller.GetSnapshot();
    const auto unavailableAddressAfter = addressSpaces.GetSnapshot();
    const auto unavailableUnbind = runLoop.Unbind(secondThread);
    ASTERIA_EXPECT(
        runner,
        "removal rejects a bound identity that was never accepted by the "
        "composed enqueue owner without mutating scheduler state",
        unavailableBinding == ProcessorKernelRunLoopError::None, !unavailableRemoved,
        unavailableFailure.Error == ProcessorKernelRunLoopError::BindingIdentityUnavailable,
        unavailableAfter.Scheduler.Queue.ReadyEntries == unavailableBefore.Scheduler.Queue.ReadyEntries,
        unavailableAfter.Scheduler.Queue.NextSequence == unavailableBefore.Scheduler.Queue.NextSequence,
        unavailableAfter.Deadline.SelectedDeadlineTicks == unavailableBefore.Deadline.SelectedDeadlineTicks,
        unavailableAddressAfter.ActiveStorage == unavailableAddressBefore.ActiveStorage,
        unavailableAddressAfter.Active.IsEqualTo(unavailableAddressBefore.Active),
        timer.GetReadCalls() == unavailableReads, timer.GetProgramCalls() == unavailablePrograms,
        timer.GetCancelCalls() == unavailableCancels, timer.GetAcknowledgeCalls() == unavailableAcknowledgements,
        idle.GetSwitchCount() == unavailableSwitches, unavailableUnbind == ProcessorKernelRunLoopError::None);

    alignas(ExternalRunLoopFixture) static std::byte ownerStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const owner = ::new (static_cast<void*>(ownerStorage)) ExternalRunLoopFixture{};
    const bool ownerPrepared = owner->PrepareAdmission();
    const auto ownerBefore = owner->Controller.GetSnapshot();
    ReadyQueueState removedState{};
    ProcessorKernelRunLoopFailure removalFailure{};
    const bool ownerRemoved = owner->RunLoop.RemoveBoundThread(owner->Pair.ThreadStorage, removedState, removalFailure);
    const auto ownerAfter = owner->Controller.GetSnapshot();
    const bool ownerUnbound = owner->RunLoop.Unbind(owner->Pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    ASTERIA_EXPECT(runner,
                   "the composed owner removes a never-selected binding through its "
                   "stored enqueue identity",
                   ownerPrepared, ownerBefore.Scheduler.Queue.ReadyEntries == 1U, ownerRemoved,
                   removedState.ReadyEntries == 0U, ownerAfter.Scheduler.Queue.ReadyEntries == 0U, ownerUnbound);
    owner->~ExternalRunLoopFixture();
}

void TestIdleAndAdmissionRollback(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};

    ProcessorKernelRunLoopReceipt emptyReceipt{};
    ProcessorKernelRunLoopFailure emptyFailure{};
    const bool empty = runLoop.DispatchFromIdle(emptyReceipt, emptyFailure);
    ASTERIA_EXPECT(runner, "an empty dispatch remains idle and exposes its wakeup receipt", idleProtection.Ready,
                   addressInitialization.HasValue(), !empty,
                   emptyFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
                   emptyFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   emptyFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
                   runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle);

    RegisteredPair pair{registry, issuer};
    const auto enqueue =
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage);
    ProcessorKernelRunLoopReceipt unboundReceipt{};
    ProcessorKernelRunLoopFailure unboundFailure{};
    const bool unbound = runLoop.DispatchFromIdle(unboundReceipt, unboundFailure);
    const auto afterUnbound = controller.GetSnapshot();
    ProcessorKernelRunLoopReceipt repeatReceipt{};
    ProcessorKernelRunLoopFailure repeatFailure{};
    const bool repeatDispatch = runLoop.DispatchFromIdle(repeatReceipt, repeatFailure);
    const auto removed = controller.Remove(pair.ThreadIdentity);
    ASTERIA_EXPECT(
        runner,
        "an unbound admission rolls back its scheduler and armed deadline and "
        "leaves this processor's queue, so it is not selected again",
        pair.Ready, enqueue.HasValue(), !unbound, unboundFailure.Error == ProcessorKernelRunLoopError::ThreadNotBound,
        unboundFailure.ThreadStorage == &pair.ThreadStorage,
        unboundFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
        unboundFailure.DeadlineReceipt.SelectedDeadlineTicks == 0U,
        unboundFailure.DeadlineReceipt.SelectedSources == 0U, unboundFailure.DeadlineReceipt.Reprogrammed == 1U,
        unboundFailure.ActiveRunRetained == 0U, afterUnbound.State == ProcessorRunState::Idle,
        afterUnbound.Deadline.State == ProcessorDeadlineState::Disarmed,
        afterUnbound.Scheduler.Queue.ReadyEntries == 0U, timer.GetArmedDeadline() == 0U, timer.GetCancelCalls() == 1U,
        !repeatDispatch, repeatFailure.ControllerError == ProcessorRunError::NoRunnableThread,
        repeatFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
        pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
        pair.ThreadStorage.GetSnapshot().RunSequence == 1U, !removed.HasValue(),
        removed.GetError().Scheduler.Queue.Error == ReadyQueueError::NotFound);
}

void TestUnboundCandidateAfterActiveCompletion(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair worker{registry, issuer};
    RegisteredPair unbound{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    const bool setup =
        protection.Ready && worker.Ready && unbound.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(worker.ThreadStorage, workerContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(worker.ContextIdentity, worker.Context, worker.ThreadIdentity, worker.ThreadStorage)
            .HasValue();

    // The running worker places a Thread this run loop has no binding for,
    // then blocks; the next selection is that unbound candidate.
    std::atomic<bool> unboundPlaced{false};
    std::thread workerThread{[&]() noexcept
                             {
                                 if (workerContext.WaitForResume() != FakeContextError::None)
                                 {
                                     return;
                                 }
                                 unboundPlaced.store(controller
                                                         .Enqueue(unbound.ContextIdentity, unbound.Context,
                                                                  unbound.ThreadIdentity, unbound.ThreadStorage)
                                                         .HasValue(),
                                                     std::memory_order_release);
                                 ProcessorKernelRunLoopReceipt receipt{};
                                 ProcessorKernelRunLoopFailure failure{};
                                 if (!runLoop->FinishAndDispatch(worker.ThreadStorage, workerContext,
                                                                 ThreadRunDisposition::Blocked, 0U, receipt, failure))
                                 {
                                     idleContext.Resume();
                                 }
                             }};

    ProcessorKernelRunLoopReceipt firstReceipt{};
    ProcessorKernelRunLoopFailure firstFailure{};
    const bool firstDispatch = runLoop->DispatchFromIdle(firstReceipt, firstFailure);
    const auto afterFirst = controller.GetSnapshot();
    const auto firstRunLoop = runLoop->GetSnapshot();
    ProcessorKernelRunLoopReceipt repeatReceipt{};
    ProcessorKernelRunLoopFailure repeatFailure{};
    const bool repeatDispatch = runLoop->DispatchFromIdle(repeatReceipt, repeatFailure);
    const auto unboundSnapshot = unbound.ThreadStorage.GetSnapshot();
    ASTERIA_EXPECT(runner,
                   "an unbound candidate selected by an active completion leaves this "
                   "processor's queue instead of being selected again from idle",
                   setup, !firstDispatch, unboundPlaced.load(std::memory_order_acquire),
                   firstFailure.Error == ProcessorKernelRunLoopError::ThreadNotBound,
                   firstFailure.ThreadStorage == &unbound.ThreadStorage, firstFailure.ActiveRunRetained == 0U,
                   firstRunLoop.State == ProcessorKernelRunLoopState::Idle, afterFirst.State == ProcessorRunState::Idle,
                   afterFirst.Scheduler.Queue.ReadyEntries == 0U,
                   worker.ThreadStorage.GetSnapshot().State == ThreadState::Blocked, !repeatDispatch,
                   repeatFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
                   repeatFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   unboundSnapshot.State == ThreadState::Ready, unboundSnapshot.RunSequence == 1U);

    workerContext.Stop();
    workerThread.join();
    static_cast<void>(controller.Remove(unbound.ThreadIdentity));
    // Stopping the suspended worker retires this placement run loop without
    // invoking its no-saved-continuation destructor.
}

// A running thread binds and enqueues another: Bind only appends, so it is legal while the loop is
// Active; a Ready peer is not unbound until it exits. The new thread runs when the creator blocks.
void TestBindWhileActive(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext creatorContext{true};
    CooperativeKernelContext createdContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair creator{registry, issuer};
    RegisteredPair created{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    const bool setup =
        protection.Ready && creator.Ready && created.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(creator.ThreadStorage, creatorContext, protection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(creator.ContextIdentity, creator.Context, creator.ThreadIdentity, creator.ThreadStorage)
            .HasValue();

    std::atomic<ProcessorKernelRunLoopError> boundWhileActive{ProcessorKernelRunLoopError::NotIdle};
    std::atomic<ProcessorKernelRunLoopError> unboundWhileActive{ProcessorKernelRunLoopError::None};
    std::atomic<bool> enqueued{false};
    std::atomic<bool> createdRan{false};
    std::thread creatorThread{
        [&]() noexcept
        {
            if (creatorContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            boundWhileActive.store(runLoop->Bind(created.ThreadStorage, createdContext, protection.Domain),
                                   std::memory_order_release);
            ProcessorKernelRunLoopFailure enqueueFailure{};
            enqueued.store(runLoop->EnqueueBoundThread(created.ContextIdentity, created.Context, created.ThreadIdentity,
                                                       created.ThreadStorage, enqueueFailure),
                           std::memory_order_release);
            unboundWhileActive.store(runLoop->Unbind(created.ThreadStorage), std::memory_order_release);
            ProcessorKernelRunLoopReceipt receipt{};
            ProcessorKernelRunLoopFailure failure{};
            if (!runLoop->FinishAndDispatch(creator.ThreadStorage, creatorContext, ThreadRunDisposition::Blocked, 0U,
                                            receipt, failure))
            {
                idleContext.Resume();
            }
        }};
    std::thread createdThread{[&]() noexcept
                              {
                                  if (createdContext.WaitForResume() != FakeContextError::None)
                                  {
                                      return;
                                  }
                                  createdRan.store(true, std::memory_order_release);
                                  ProcessorKernelRunLoopReceipt receipt{};
                                  ProcessorKernelRunLoopFailure failure{};
                                  if (!runLoop->FinishAndDispatch(created.ThreadStorage, createdContext,
                                                                  ThreadRunDisposition::Exited, 7U, receipt, failure))
                                  {
                                      idleContext.Resume();
                                  }
                              }};

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    static_cast<void>(runLoop->DispatchFromIdle(receipt, failure));
    const auto createdSnapshot = created.ThreadStorage.GetSnapshot();
    const auto unbound = runLoop->Unbind(created.ThreadStorage);
    ASTERIA_EXPECT(runner,
                   "a running thread binds and enqueues another, which runs once it blocks; a Ready peer stays bound",
                   setup, boundWhileActive.load(std::memory_order_acquire) == ProcessorKernelRunLoopError::None,
                   enqueued.load(std::memory_order_acquire),
                   unboundWhileActive.load(std::memory_order_acquire) == ProcessorKernelRunLoopError::NotIdle,
                   createdRan.load(std::memory_order_acquire), createdSnapshot.State == ThreadState::Exited,
                   createdSnapshot.ExitCode == 7U, creator.ThreadStorage.GetSnapshot().State == ThreadState::Blocked,
                   runLoop->GetSnapshot().State == ProcessorKernelRunLoopState::Idle,
                   unbound == ProcessorKernelRunLoopError::None);

    creatorContext.Stop();
    creatorThread.join();
    createdContext.Stop();
    createdThread.join();
    // Stopping the suspended creator retires this placement run loop without
    // invoking its no-saved-continuation destructor.
}

// While a thread runs, Unbind removes only another thread's binding that was never enqueued or
// whose thread exited: the creator undoes a binding it never enqueued, cannot unbind itself or a
// Ready peer, and unbinds its peer once that exited.
void TestUnbindWhileActive(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext creatorContext{true};
    CooperativeKernelContext abandonedContext{true};
    CooperativeKernelContext peerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    RegisteredPair creator{registry, issuer};
    RegisteredPair abandoned{registry, issuer};
    RegisteredPair peer{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    const bool setup =
        protection.Ready && creator.Ready && abandoned.Ready && peer.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(creator.ThreadStorage, creatorContext, protection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(creator.ContextIdentity, creator.Context, creator.ThreadIdentity, creator.ThreadStorage)
            .HasValue();

    constexpr auto Unset = ProcessorKernelRunLoopError::InvariantViolation;
    std::atomic<ProcessorKernelRunLoopError> abandonedUnbound{Unset};
    std::atomic<ProcessorKernelRunLoopError> selfUnbound{Unset};
    std::atomic<ProcessorKernelRunLoopError> readyPeerUnbound{Unset};
    std::atomic<ProcessorKernelRunLoopError> exitedPeerUnbound{Unset};
    std::atomic<bool> peerRan{false};
    std::thread creatorThread{
        [&]() noexcept
        {
            if (creatorContext.WaitForResume() != FakeContextError::None)
            {
                return;
            }
            ProcessorKernelRunLoopFailure enqueueFailure{};
            const bool bound = (runLoop->Bind(abandoned.ThreadStorage, abandonedContext, protection.Domain) ==
                                ProcessorKernelRunLoopError::None) &&
                               (runLoop->Bind(peer.ThreadStorage, peerContext, protection.Domain) ==
                                ProcessorKernelRunLoopError::None) &&
                               runLoop->EnqueueBoundThread(peer.ContextIdentity, peer.Context, peer.ThreadIdentity,
                                                           peer.ThreadStorage, enqueueFailure);
            abandonedUnbound.store(bound ? runLoop->Unbind(abandoned.ThreadStorage) : Unset, std::memory_order_release);
            selfUnbound.store(runLoop->Unbind(creator.ThreadStorage), std::memory_order_release);
            readyPeerUnbound.store(runLoop->Unbind(peer.ThreadStorage), std::memory_order_release);
            // Ending Ready queues the creator behind its peer, which runs and
            // exits; the creator resumes here.
            ProcessorKernelRunLoopReceipt receipt{};
            ProcessorKernelRunLoopFailure failure{};
            if (!runLoop->FinishAndDispatch(creator.ThreadStorage, creatorContext, ThreadRunDisposition::Ready, 0U,
                                            receipt, failure))
            {
                idleContext.Resume();
                return;
            }
            exitedPeerUnbound.store(runLoop->Unbind(peer.ThreadStorage), std::memory_order_release);
            if (!runLoop->FinishAndDispatch(creator.ThreadStorage, creatorContext, ThreadRunDisposition::Blocked, 0U,
                                            receipt, failure))
            {
                idleContext.Resume();
            }
        }};
    std::thread peerThread{[&]() noexcept
                           {
                               if (peerContext.WaitForResume() != FakeContextError::None)
                               {
                                   return;
                               }
                               peerRan.store(true, std::memory_order_release);
                               ProcessorKernelRunLoopReceipt receipt{};
                               ProcessorKernelRunLoopFailure failure{};
                               if (!runLoop->FinishAndDispatch(peer.ThreadStorage, peerContext,
                                                               ThreadRunDisposition::Exited, 9U, receipt, failure))
                               {
                                   idleContext.Resume();
                               }
                           }};

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    static_cast<void>(runLoop->DispatchFromIdle(receipt, failure));
    const auto peerSnapshot = peer.ThreadStorage.GetSnapshot();
    ASTERIA_EXPECT(runner, "a running thread unbinds a never-enqueued or exited peer but not itself or a Ready peer",
                   setup, abandonedUnbound.load(std::memory_order_acquire) == ProcessorKernelRunLoopError::None,
                   selfUnbound.load(std::memory_order_acquire) == ProcessorKernelRunLoopError::WrongActiveThread,
                   readyPeerUnbound.load(std::memory_order_acquire) == ProcessorKernelRunLoopError::NotIdle,
                   peerRan.load(std::memory_order_acquire), peerSnapshot.State == ThreadState::Exited,
                   peerSnapshot.ExitCode == 9U,
                   exitedPeerUnbound.load(std::memory_order_acquire) == ProcessorKernelRunLoopError::None,
                   creator.ThreadStorage.GetSnapshot().State == ThreadState::Blocked,
                   runLoop->GetSnapshot().State == ProcessorKernelRunLoopState::Idle,
                   runLoop->Unbind(peer.ThreadStorage) == ProcessorKernelRunLoopError::ThreadNotBound,
                   runLoop->Unbind(creator.ThreadStorage) == ProcessorKernelRunLoopError::None);

    creatorContext.Stop();
    creatorThread.join();
    peerContext.Stop();
    peerThread.join();
    // Stopping the suspended creator retires this placement run loop without
    // invoking its no-saved-continuation destructor.
}

void TestUnpreparedBindingLeavesQueue(TestContext& runner) noexcept
{
    alignas(ExternalRunLoopFixture) std::byte fixtureStorage[sizeof(ExternalRunLoopFixture)]{};
    ExternalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) ExternalRunLoopFixture{};
    const bool setup = fixture->PrepareAdmission();
    fixture->TargetContext.SetPrepared(false);
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = fixture->RunLoop.DispatchFromIdle(receipt, failure);
    const auto afterDispatch = fixture->Controller.GetSnapshot();
    ProcessorKernelRunLoopReceipt repeatReceipt{};
    ProcessorKernelRunLoopFailure repeatFailure{};
    const bool repeatDispatch = fixture->RunLoop.DispatchFromIdle(repeatReceipt, repeatFailure);
    ReadyQueueState removedState{};
    ProcessorKernelRunLoopFailure removalFailure{};
    const bool removed = fixture->RunLoop.RemoveBoundThread(fixture->Pair.ThreadStorage, removedState, removalFailure);
    const auto thread = fixture->Pair.ThreadStorage.GetSnapshot();
    const bool unbound = fixture->RunLoop.Unbind(fixture->Pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    ASTERIA_EXPECT(runner,
                   "a bound candidate whose context cannot be entered leaves this "
                   "processor's queue instead of being selected again",
                   setup, !dispatch, failure.Error == ProcessorKernelRunLoopError::ContextNotPrepared,
                   failure.ThreadStorage == &fixture->Pair.ThreadStorage, failure.ActiveRunRetained == 0U,
                   afterDispatch.State == ProcessorRunState::Idle, afterDispatch.Scheduler.Queue.ReadyEntries == 0U,
                   !repeatDispatch, repeatFailure.ControllerError == ProcessorRunError::NoRunnableThread, !removed,
                   removalFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
                   thread.State == ThreadState::Ready, thread.RunSequence == 1U,
                   fixture->IdleContext.GetSwitchCount() == 0U,
                   fixture->RunLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle, unbound);
    if (unbound)
    {
        fixture->~ExternalRunLoopFixture();
    }
}

void TestIdleRootRestorationRetriesRollbackSafeRejections(TestContext& runner) noexcept
{
    alignas(TerminalRunLoopFixture) std::byte fixtureStorage[sizeof(TerminalRunLoopFixture)]{};
    TerminalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) TerminalRunLoopFixture{};
    fixture->IdleContext.SetSwitchError(FakeContextError::Rejected);
    const bool setup = fixture->PrepareAdmission();
    const Size idleActivations = fixture->IdleProtection.PageTable.ActivationCalls;
    // Pre-commit refusals leave the destination root active, like
    // contention for the shared idle root's transition reservation.
    fixture->IdleProtection.PageTable.RejectedActivations = 3U;
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = fixture->RunLoop.DispatchFromIdle(receipt, failure);
    const auto runLoopSnapshot = fixture->RunLoop.GetSnapshot();
    const auto addressSnapshot = fixture->AddressSpaces.GetSnapshot();
    const auto removed = fixture->Controller.Remove(fixture->Pair.ThreadIdentity);
    const bool unbound = fixture->RunLoop.Unbind(fixture->Pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    ASTERIA_EXPECT(runner, "rollback-safe rejections of the idle-root restoration are retried", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::NativeSwitchRejected, failure.ActiveRunRetained == 0U,
                   fixture->IdleProtection.PageTable.ActivationCalls == idleActivations + 4U,
                   runLoopSnapshot.State == ProcessorKernelRunLoopState::Idle,
                   addressSnapshot.State == ProcessorAddressSpaceControllerState::Ready,
                   addressSnapshot.ActiveStorage == &fixture->IdleProtection.AddressSpaceStorage,
                   fixture->Pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready, removed.HasValue(), unbound);
    if (runLoopSnapshot.State == ProcessorKernelRunLoopState::Idle)
    {
        fixture->~TerminalRunLoopFixture();
    }
}

void TestIdleRootContentionRetryIsBounded(TestContext& runner) noexcept
{
    alignas(TerminalRunLoopFixture) std::byte fixtureStorage[sizeof(TerminalRunLoopFixture)]{};
    TerminalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) TerminalRunLoopFixture{};
    struct Contention
    {
        AddressSpace* IdleRoot;
        AddressSpaceTransitionReceipt Receipt;
        bool Reserved;
    };
    Contention contention{&fixture->IdleProtection.AddressSpaceStorage, {}, false};
    // Contention requires another reservation for this same CPU; other processors have independent
    // receipts.
    fixture->IdleContext.SetSwitchError(FakeContextError::Rejected);
    fixture->IdleContext.InvokeOnNextSwitch(
        [](void* const argument) noexcept
        {
            Contention& held = *static_cast<Contention*>(argument);
            const auto reserved = held.IdleRoot->ReserveProcessorTransition(CpuId{0U});
            held.Reserved = reserved.HasValue();
            held.Receipt = reserved.GetValue();
        },
        &contention);
    const bool setup = fixture->PrepareAdmission();
    // Every reservation consumes the root's next transition sequence, so a
    // probe before and after counts the reservations in between.
    AddressSpace& destination = fixture->DestinationProtection.AddressSpaceStorage;
    const auto firstProbe = destination.ReserveProcessorTransition(CpuId{3U});
    const bool firstProbeReleased =
        firstProbe.HasValue() &&
        (destination.ReleaseProcessorTransition(firstProbe.GetValue()) == AddressSpaceError::None);
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = fixture->RunLoop.DispatchFromIdle(receipt, failure);
    const auto addressSnapshot = fixture->AddressSpaces.GetSnapshot();
    const auto secondProbe = destination.ReserveProcessorTransition(CpuId{3U});
    const bool secondProbeReleased =
        secondProbe.HasValue() &&
        (destination.ReleaseProcessorTransition(secondProbe.GetValue()) == AddressSpaceError::None);
    const AddressSpaceError released = contention.Reserved
                                           ? contention.IdleRoot->ReleaseProcessorTransition(contention.Receipt)
                                           : AddressSpaceError::InvalidProcessorTransitionReceipt;
    // Between the probes: the admission reserves the destination once, then
    // each restoration attempt reserves it as its source.
    ASTERIA_EXPECT(runner,
                   "idle-root contention is retried a bounded number of times before "
                   "the retained admission becomes terminal",
                   setup, !dispatch, contention.Reserved,
                   failure.Error == ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected,
                   failure.AddressSpaceController.Error == ProcessorAddressSpaceControllerError::TransitionRejected,
                   failure.AddressSpaceController.Transition == AddressSpaceError::ProcessorTransitionPending,
                   failure.AddressSpaceController.AdmissionRollbackSafe, failure.ActiveRunRetained == 1U,
                   firstProbeReleased, secondProbeReleased,
                   secondProbe.GetValue().GetSequence() ==
                       firstProbe.GetValue().GetSequence() + 2U + MaximumIdleRootTransitionAttempts,
                   addressSnapshot.State == ProcessorAddressSpaceControllerState::Ready,
                   addressSnapshot.ActiveStorage == &fixture->DestinationProtection.AddressSpaceStorage,
                   fixture->RunLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Faulted,
                   released == AddressSpaceError::None);

    // The retained scheduler claim is the terminal evidence under test, so
    // this placement fixture is not destroyed.
}

void TestIdleRootUnknownCommitIsNotRetried(TestContext& runner) noexcept
{
    alignas(TerminalRunLoopFixture) std::byte fixtureStorage[sizeof(TerminalRunLoopFixture)]{};
    TerminalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) TerminalRunLoopFixture{};
    fixture->IdleContext.SetSwitchError(FakeContextError::Rejected);
    const bool setup = fixture->PrepareAdmission();
    const Size idleActivations = fixture->IdleProtection.PageTable.ActivationCalls;
    fixture->IdleProtection.PageTable.NextActivationError = AddressSpacePageTableActivationError::CommittedStateUnknown;
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = fixture->RunLoop.DispatchFromIdle(receipt, failure);
    ASTERIA_EXPECT(runner, "an idle-root restoration that may have committed is not retried", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::AddressSpaceControllerRollbackRejected,
                   failure.AddressSpaceController.Error == ProcessorAddressSpaceControllerError::ActivationRejected,
                   !failure.AddressSpaceController.AdmissionRollbackSafe, failure.ActiveRunRetained == 1U,
                   fixture->IdleProtection.PageTable.ActivationCalls == idleActivations + 1U,
                   fixture->AddressSpaces.GetSnapshot().State == ProcessorAddressSpaceControllerState::Faulted,
                   fixture->RunLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Faulted);

    // The retained scheduler claim and conservative residency are terminal
    // evidence, so this placement fixture is not destroyed.
}

void TestBlockedCompletionRetriesIdleRoot(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain workerProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext workerContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    RegisteredPair worker{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};
    const bool setup =
        idleProtection.Ready && workerProtection.Ready && worker.Ready && addressInitialization.HasValue() &&
        (runLoop->Bind(worker.ThreadStorage, workerContext, workerProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(worker.ContextIdentity, worker.Context, worker.ThreadIdentity, worker.ThreadStorage)
            .HasValue();
    const Size idleActivations = idleProtection.PageTable.ActivationCalls;

    std::thread workerThread{[&]() noexcept
                             {
                                 if (workerContext.WaitForResume() != FakeContextError::None)
                                 {
                                     return;
                                 }
                                 // The worker's Blocked completion leaves nothing runnable, so the run
                                 // loop must return this CPU to the idle root, which refuses twice.
                                 idleProtection.PageTable.RejectedActivations = 2U;
                                 ProcessorKernelRunLoopReceipt receipt{};
                                 ProcessorKernelRunLoopFailure failure{};
                                 if (!runLoop->FinishAndDispatch(worker.ThreadStorage, workerContext,
                                                                 ThreadRunDisposition::Blocked, 0U, receipt, failure))
                                 {
                                     idleContext.Resume();
                                 }
                             }};

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop->DispatchFromIdle(receipt, failure);
    const auto runLoopSnapshot = runLoop->GetSnapshot();
    const auto addressSnapshot = addressSpaces.GetSnapshot();
    ASTERIA_EXPECT(runner, "a Blocked completion retries a refused idle-root switch and idles", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::ControllerRejected,
                   failure.ControllerError == ProcessorRunError::NoRunnableThread,
                   idleProtection.PageTable.ActivationCalls == idleActivations + 3U,
                   runLoopSnapshot.State == ProcessorKernelRunLoopState::Idle,
                   addressSnapshot.ActiveStorage == &idleProtection.AddressSpaceStorage,
                   worker.ThreadStorage.GetSnapshot().State == ThreadState::Blocked);

    workerContext.Stop();
    workerThread.join();
    // Stopping the suspended worker retires this placement run loop without
    // invoking its no-saved-continuation destructor.
}

void TestProcessorMismatch(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{1U}, protection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatched = runLoop.DispatchFromIdle(receipt, failure);
    ASTERIA_EXPECT(runner, "a run loop rejects mismatched scheduler and address-space CPUs", protection.Ready,
                   addressInitialization.HasValue(), !dispatched,
                   failure.Error == ProcessorKernelRunLoopError::InvariantViolation,
                   controller.GetSnapshot().State == ProcessorRunState::Idle,
                   addressSpaces.GetSnapshot().Processor.GetValue() == 1U, protection.PageTable.ActivationCalls == 1U,
                   idle.GetSwitchCount() == 0U, runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Faulted);
}

void TestNativeRejectionRollback(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain protection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    FakeKernelContext threadContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, protection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    RegisteredPair pair{registry, issuer};

    idle.SetSwitchError(FakeContextError::Rejected);
    const bool setup =
        pair.Ready && protection.Ready && addressInitialization.HasValue() &&
        (runLoop.Bind(pair.ThreadStorage, threadContext, protection.Domain) == ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop.DispatchFromIdle(receipt, failure);
    const auto removed = controller.Remove(pair.ThreadIdentity);
    ProcessorKernelRunLoopReceipt cleanupReceipt{};
    ProcessorKernelRunLoopFailure cleanupFailure{};
    const bool cleanupDispatch = runLoop.DispatchFromIdle(cleanupReceipt, cleanupFailure);
    const bool unbound = runLoop.Unbind(pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    ASTERIA_EXPECT(runner, "a rejected first switch rolls the scheduler admission back", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::NativeSwitchRejected,
                   failure.NativeError == static_cast<Foundation::UInt8>(FakeContextError::Rejected),
                   failure.ActiveRunRetained == 0U, idle.GetSwitchCount() == 1U,
                   protection.PageTable.ActivationCalls == 1U,
                   pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle, removed.HasValue(),
                   !cleanupDispatch, cleanupFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   cleanupFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed, unbound);
}

void TestAddressSpaceRejectionRollback(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain targetProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    FakeKernelContext threadContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    RegisteredPair pair{registry, issuer};

    targetProtection.PageTable.NextActivationError = AddressSpacePageTableActivationError::RejectedBeforeCommit;
    const bool setup =
        pair.Ready && idleProtection.Ready && targetProtection.Ready && addressInitialization.HasValue() &&
        (runLoop.Bind(pair.ThreadStorage, threadContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop.DispatchFromIdle(receipt, failure);
    const auto removed = controller.Remove(pair.ThreadIdentity);
    ProcessorKernelRunLoopReceipt cleanupReceipt{};
    ProcessorKernelRunLoopFailure cleanupFailure{};
    const bool cleanupDispatch = runLoop.DispatchFromIdle(cleanupReceipt, cleanupFailure);
    const bool unbound = runLoop.Unbind(pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    const auto activeRoot = addressSpaces.GetSnapshot();
    ASTERIA_EXPECT(runner, "a pre-commit root rejection prevents native context entry", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::AddressSpaceControllerRejected,
                   failure.AddressSpaceController.Error == ProcessorAddressSpaceControllerError::ActivationRejected,
                   failure.AddressSpaceController.AdmissionRollbackSafe, failure.ActiveRunRetained == 0U,
                   idle.GetSwitchCount() == 0U, threadContext.GetSwitchCount() == 0U,
                   targetProtection.PageTable.ActivationCalls == 1U,
                   pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle,
                   activeRoot.ActiveStorage == &idleProtection.AddressSpaceStorage, removed.HasValue(),
                   !cleanupDispatch, cleanupFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   cleanupFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed, unbound);
}

// A TimerQueue deadline that falls due while the target root activates
// rejects the entry, and the rollback then finds it already reached. The
// controller completed the rollback (reaching a deadline is rollback-safe,
// ProcessorRunController.md), so the loop returns to idle instead of
// faulting, and the next dispatch reports the due deadline.
void TestRollbackFindsTimerQueueDeadlineDue(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain targetProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    FakeKernelContext threadContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    RegisteredPair pair{registry, issuer};
    Kernel::Time::Timer queueTimer{timer};
    const bool timerReady = queueTimer.Initialize(ResourceDomainId{9U}) == Kernel::Time::TimerError::None;
    const auto timerObject = issuer.CreateObject(queueTimer);
    const Kernel::Time::TimerId timerId{registry, timerObject.HasValue() ? timerObject.GetValue() : ObjectId{}};
    const auto armed = timerQueue.Arm(timerId, queueTimer, MonotonicTime{20U});

    // The clock starts at 10: the queue's deadline is at 20 and the run stop
    // at 30; activation moves the clock to 30, past both.
    targetProtection.PageTable.ActivationTimer = &timer;
    targetProtection.PageTable.ActivationTicks = 30U;
    const bool setup =
        pair.Ready && idleProtection.Ready && targetProtection.Ready && timerReady && timerObject.HasValue() &&
        armed.HasValue() && addressInitialization.HasValue() &&
        (runLoop.Bind(pair.ThreadStorage, threadContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop.DispatchFromIdle(receipt, failure);
    const auto afterRollback = runLoop.GetSnapshot();
    ProcessorKernelRunLoopReceipt dueReceipt{};
    ProcessorKernelRunLoopFailure dueFailure{};
    const bool dueDispatch = runLoop.DispatchFromIdle(dueReceipt, dueFailure);
    const auto cancelled = armed.HasValue() ? timerQueue.Cancel(timerId, queueTimer, armed.GetValue().Timer.Token)
                                            : timerQueue.Cancel(timerId, queueTimer, {});
    const auto removed = controller.Remove(pair.ThreadIdentity);
    const bool unbound = runLoop.Unbind(pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    const bool retired = timerObject.HasValue() &&
                         (issuer.DestroyObject(timerObject.GetValue()) == Kernel::Object::ObjectRegistryError::Ok) &&
                         issuer.CollectDestroyedObject().HasValue();
    ASTERIA_EXPECT(runner,
                   "a rollback that finds a TimerQueue deadline due returns to idle and the next dispatch reports it",
                   setup, !dispatch, failure.Error == ProcessorKernelRunLoopError::ControllerEntryRejected,
                   failure.ControllerError == ProcessorRunError::DeadlineReached, failure.ActiveRunRetained == 0U,
                   afterRollback.State == ProcessorKernelRunLoopState::Idle, threadContext.GetSwitchCount() == 0U,
                   pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready, !dueDispatch,
                   dueFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
                   dueFailure.ControllerError == ProcessorRunError::DeadlineReached,
                   runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle, cancelled.HasValue(),
                   removed.HasValue(), unbound, retired);
}

void TestDeadlineReachedDuringRootActivation(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain idleProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain targetProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    FakeKernelContext idle{true};
    FakeKernelContext threadContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, idleProtection.AddressSpaceStorage);
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    RegisteredPair pair{registry, issuer};

    targetProtection.PageTable.ActivationTimer = &timer;
    targetProtection.PageTable.ActivationTicks = 30U;
    const bool setup =
        pair.Ready && idleProtection.Ready && targetProtection.Ready && addressInitialization.HasValue() &&
        (runLoop.Bind(pair.ThreadStorage, threadContext, targetProtection.Domain) ==
         ProcessorKernelRunLoopError::None) &&
        controller.Enqueue(pair.ContextIdentity, pair.Context, pair.ThreadIdentity, pair.ThreadStorage).HasValue();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop.DispatchFromIdle(receipt, failure);
    const auto removed = controller.Remove(pair.ThreadIdentity);
    ProcessorKernelRunLoopReceipt cleanupReceipt{};
    ProcessorKernelRunLoopFailure cleanupFailure{};
    const bool cleanupDispatch = runLoop.DispatchFromIdle(cleanupReceipt, cleanupFailure);
    const auto idleRoot = idleProtection.AddressSpaceStorage.GetSnapshot();
    const auto targetRoot = targetProtection.AddressSpaceStorage.GetSnapshot();
    const bool unbound = runLoop.Unbind(pair.ThreadStorage) == ProcessorKernelRunLoopError::None;
    ASTERIA_EXPECT(runner, "a deadline crossed during root activation unwinds before entry", setup, !dispatch,
                   failure.Error == ProcessorKernelRunLoopError::ControllerEntryRejected,
                   failure.ControllerError == ProcessorRunError::DeadlineReached,
                   failure.DeadlineError == Kernel::Time::ProcessorDeadlineError::DeadlineReached,
                   failure.ActiveRunRetained == 0U, idle.GetSwitchCount() == 0U, threadContext.GetSwitchCount() == 0U,
                   idleProtection.PageTable.ActivationCalls == 2U, targetProtection.PageTable.ActivationCalls == 1U,
                   idleRoot.ResidentProcessorMask == 0x01U, targetRoot.ResidentProcessorMask == 0U,
                   pair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
                   runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle, removed.HasValue(),
                   !cleanupDispatch, cleanupFailure.ControllerError == ProcessorRunError::NoRunnableThread,
                   cleanupFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed, unbound);
}

void TestActiveDeadlineRecoveryRestoresCurrentRoot(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    RegisteredProtectionDomain currentProtection{registryLock, registry, issuer, timer};
    RegisteredProtectionDomain destinationProtection{registryLock, registry, issuer, timer};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    CooperativeKernelContext idleContext{true};
    CooperativeKernelContext currentContext{true};
    CooperativeKernelContext destinationContext{true};
    ProcessorAddressSpaceController addressSpaces;
    const auto addressInitialization = addressSpaces.Initialize(CpuId{0U}, currentProtection.AddressSpaceStorage);
    RegisteredPair currentPair{registry, issuer};
    RegisteredPair destinationPair{registry, issuer};
    using RunLoop = ProcessorKernelRunLoop<CooperativeKernelContext>;
    alignas(RunLoop) std::byte runLoopStorage[sizeof(RunLoop)]{};
    RunLoop* const runLoop = ::new (static_cast<void*>(runLoopStorage)) RunLoop{controller, addressSpaces, idleContext};

    destinationProtection.PageTable.ActivationTimer = &timer;
    destinationProtection.PageTable.ActivationTicks = 30U;
    currentProtection.PageTable.ThreadToObserve = &destinationPair.ThreadStorage;
    const bool setup = currentProtection.Ready && destinationProtection.Ready && currentPair.Ready &&
                       destinationPair.Ready && addressInitialization.HasValue() &&
                       (runLoop->Bind(currentPair.ThreadStorage, currentContext, currentProtection.Domain) ==
                        ProcessorKernelRunLoopError::None) &&
                       (runLoop->Bind(destinationPair.ThreadStorage, destinationContext,
                                      destinationProtection.Domain) == ProcessorKernelRunLoopError::None) &&
                       controller
                           .Enqueue(currentPair.ContextIdentity, currentPair.Context, currentPair.ThreadIdentity,
                                    currentPair.ThreadStorage)
                           .HasValue() &&
                       controller
                           .Enqueue(destinationPair.ContextIdentity, destinationPair.Context,
                                    destinationPair.ThreadIdentity, destinationPair.ThreadStorage)
                           .HasValue();

    std::thread currentWorker{[&]() noexcept
                              {
                                  if (currentContext.WaitForResume() == FakeContextError::Rejected)
                                  {
                                      idleContext.Resume();
                                      return;
                                  }
                                  ProcessorKernelRunLoopReceipt workerReceipt{};
                                  ProcessorKernelRunLoopFailure workerFailure{};
                                  static_cast<void>(runLoop->FinishAndDispatch(
                                      currentPair.ThreadStorage, currentContext, ThreadRunDisposition::Ready, 0U,
                                      workerReceipt, workerFailure));
                                  idleContext.Resume();
                              }};
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = runLoop->DispatchFromIdle(receipt, failure);
    const auto runLoopSnapshot = runLoop->GetSnapshot();
    const auto controllerSnapshot = controller.GetSnapshot();
    const auto addressSnapshot = addressSpaces.GetSnapshot();
    const auto currentRoot = currentProtection.AddressSpaceStorage.GetSnapshot();
    const auto destinationRoot = destinationProtection.AddressSpaceStorage.GetSnapshot();
    const auto addressProbe = addressSpaces.SwitchTo(currentProtection.AddressSpaceStorage);
    const auto destinationRemoved = controller.Remove(destinationPair.ThreadIdentity);
    const auto currentRemoved = controller.Remove(currentPair.ThreadIdentity);
    ProcessorKernelRunLoopReceipt cleanupReceipt{};
    ProcessorKernelRunLoopFailure cleanupFailure{};
    const bool cleanupDispatch = runLoop->DispatchFromIdle(cleanupReceipt, cleanupFailure);
    const auto cleanupControllerSnapshot = controller.GetSnapshot();

    ASTERIA_EXPECT(
        runner, "active deadline recovery restores the current root before rolling back admission", setup, !dispatch,
        failure.Error == ProcessorKernelRunLoopError::ControllerEntryRejected,
        failure.ControllerError == ProcessorRunError::DeadlineReached,
        failure.DeadlineError == Kernel::Time::ProcessorDeadlineError::DeadlineReached,
        failure.ThreadStorage == &destinationPair.ThreadStorage, failure.ActiveRunRetained == 0U,
        currentProtection.PageTable.ThreadWasRunningDuringActivation, currentProtection.PageTable.ActivationCalls == 2U,
        destinationProtection.PageTable.ActivationCalls == 1U, currentRoot.ResidentProcessorMask == 0x01U,
        destinationRoot.ResidentProcessorMask == 0U,
        addressSnapshot.State == ProcessorAddressSpaceControllerState::Ready,
        addressSnapshot.ActiveStorage == &currentProtection.AddressSpaceStorage,
        addressSnapshot.Active.IsEqualTo(currentRoot.Identity), addressSnapshot.PreviousStorage == nullptr,
        !addressSnapshot.Previous.IsValid(), controllerSnapshot.State == ProcessorRunState::Idle,
        !controllerSnapshot.Scheduler.ActiveThread.IsValid(),
        currentPair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
        destinationPair.ThreadStorage.GetSnapshot().State == ThreadState::Ready,
        runLoopSnapshot.State == ProcessorKernelRunLoopState::Idle, addressProbe.HasValue(),
        !addressProbe.GetValue().NativeActivationPerformed, destinationRemoved.HasValue(), currentRemoved.HasValue(),
        !cleanupDispatch, cleanupFailure.Error == ProcessorKernelRunLoopError::ControllerRejected,
        cleanupFailure.ControllerError == ProcessorRunError::NoRunnableThread,
        cleanupFailure.DeadlineReceipt.State == ProcessorDeadlineState::Disarmed,
        cleanupControllerSnapshot.State == ProcessorRunState::Idle,
        cleanupControllerSnapshot.Deadline.State == ProcessorDeadlineState::Disarmed);

    currentContext.Stop();
    currentWorker.join();
    // The stopped hosted continuation intentionally makes the run loop
    // terminal after the verified recovery. Its placement lifetime prevents a
    // destructor call whose Idle/no-saved-continuation precondition is false.
}

void TestDestinationUnknownCommitIsTerminal(TestContext& runner) noexcept
{
    alignas(TerminalRunLoopFixture) std::byte fixtureStorage[sizeof(TerminalRunLoopFixture)]{};
    TerminalRunLoopFixture* const fixture = ::new (static_cast<void*>(fixtureStorage)) TerminalRunLoopFixture{};
    fixture->DestinationProtection.PageTable.NextActivationError =
        AddressSpacePageTableActivationError::CommittedStateUnknown;
    const bool setup = fixture->PrepareAdmission();
    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatch = fixture->RunLoop.DispatchFromIdle(receipt, failure);
    const auto runLoopSnapshot = fixture->RunLoop.GetSnapshot();
    const auto controllerSnapshot = fixture->Controller.GetSnapshot();
    const auto addressSnapshot = fixture->AddressSpaces.GetSnapshot();
    const auto idleRoot = fixture->IdleProtection.AddressSpaceStorage.GetSnapshot();
    const auto destinationRoot = fixture->DestinationProtection.AddressSpaceStorage.GetSnapshot();
    ProcessorKernelRunLoopReceipt repeatedReceipt{};
    ProcessorKernelRunLoopFailure repeatedFailure{};
    const bool repeatedDispatch = fixture->RunLoop.DispatchFromIdle(repeatedReceipt, repeatedFailure);

    ASTERIA_EXPECT(runner, "an unknown destination commit retains terminal scheduler and root evidence", setup,
                   !dispatch, failure.Error == ProcessorKernelRunLoopError::AddressSpaceControllerRejected,
                   failure.AddressSpaceController.Error == ProcessorAddressSpaceControllerError::ActivationRejected,
                   !failure.AddressSpaceController.AdmissionRollbackSafe,
                   failure.AddressSpaceController.Activation.ProcessorMayBeResident,
                   failure.AddressSpaceController.Activation.PageTable ==
                       AddressSpacePageTableActivationError::CommittedStateUnknown,
                   failure.ActiveRunRetained == 1U, failure.ThreadStorage == &fixture->Pair.ThreadStorage,
                   runLoopSnapshot.State == ProcessorKernelRunLoopState::Faulted,
                   controllerSnapshot.State == ProcessorRunState::Active,
                   controllerSnapshot.Scheduler.ActiveThread.IsEqualTo(fixture->Pair.ThreadIdentity),
                   fixture->Pair.ThreadStorage.GetSnapshot().State == ThreadState::Running,
                   addressSnapshot.State == ProcessorAddressSpaceControllerState::Faulted,
                   addressSnapshot.ActiveStorage == &fixture->DestinationProtection.AddressSpaceStorage,
                   addressSnapshot.Active.IsEqualTo(destinationRoot.Identity),
                   addressSnapshot.PreviousStorage == &fixture->IdleProtection.AddressSpaceStorage,
                   addressSnapshot.Previous.IsEqualTo(idleRoot.Identity), idleRoot.ResidentProcessorMask == 0x01U,
                   destinationRoot.ResidentProcessorMask == 0x01U, !repeatedDispatch,
                   repeatedFailure.Error == failure.Error, repeatedFailure.ActiveRunRetained == 1U,
                   repeatedFailure.ThreadStorage == &fixture->Pair.ThreadStorage);

    // The retained scheduler claim and conservative dual residency are the
    // terminal evidence under test, so this placement fixture is not destroyed.
}

void TestBindingReservations(TestContext& runner) noexcept
{
    alignas(TerminalRunLoopFixture) static std::byte storage[2U][sizeof(TerminalRunLoopFixture)]{};
    auto& fixture = *::new (static_cast<void*>(storage[0U])) TerminalRunLoopFixture{};
    auto& other = *::new (static_cast<void*>(storage[1U])) TerminalRunLoopFixture{};
    Kernel::Scheduler::ProcessorBindingReservation reservations[Kernel::Scheduler::MaximumReadyQueueEntries];
    Kernel::Scheduler::ProcessorBindingReservation excess;
    bool reserved{true};
    for (auto& reservation : reservations)
    {
        reserved = fixture.RunLoop.ReserveBinding(reservation) && reserved;
    }
    const bool full = !fixture.RunLoop.ReserveBinding(excess);
    const auto direct = fixture.RunLoop.Bind(fixture.Pair.ThreadStorage, fixture.DestinationContext,
                                             fixture.DestinationProtection.Domain);
    const auto wrong = other.RunLoop.Bind(fixture.Pair.ThreadStorage, fixture.DestinationContext,
                                          fixture.DestinationProtection.Domain, &reservations[0]);
    other.RunLoop.CancelBinding(reservations[0]);
    const auto bound = fixture.RunLoop.Bind(fixture.Pair.ThreadStorage, fixture.DestinationContext,
                                            fixture.DestinationProtection.Domain, &reservations[0]);
    const bool consumedStillFull = !fixture.RunLoop.ReserveBinding(excess);
    const auto unbound = fixture.RunLoop.Unbind(fixture.Pair.ThreadStorage);
    const bool released = fixture.RunLoop.ReserveBinding(excess);
    fixture.RunLoop.CancelBinding(excess);
    fixture.RunLoop.CancelBinding(excess);
    for (auto& reservation : reservations)
    {
        fixture.RunLoop.CancelBinding(reservation);
    }
    const bool available = fixture.RunLoop.ReserveBinding(excess);
    fixture.RunLoop.CancelBinding(excess);
    ASTERIA_EXPECT(runner, "reserved bindings share capacity with direct binds and belong to one home until consumed",
                   fixture.Ready, other.Ready, reserved, full, consumedStillFull, released, available,
                   direct == ProcessorKernelRunLoopError::BindingCapacityExceeded,
                   wrong == ProcessorKernelRunLoopError::BindingCapacityExceeded,
                   bound == ProcessorKernelRunLoopError::None, unbound == ProcessorKernelRunLoopError::None,
                   fixture.RunLoop.GetSnapshot().BindingCount == 0U);
    other.~TerminalRunLoopFixture();
    fixture.~TerminalRunLoopFixture();
}

} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ProcessorKernelRunLoopTests"};
    Asteria::Test::TestBindingReservations(runner);
    Asteria::Test::TestCooperativeBootstrapWorker(runner);
    Asteria::Test::TestWaitCommitRetryableStopsDispatch(runner);
    Asteria::Test::TestSynchronousCallReferenceRetryUsesIdlePoll(runner);
    Asteria::Test::TestRepeatedBlockedWakeReadmission(runner);
    Asteria::Test::TestWaitCommitTerminalStopsDispatch(runner);
    Asteria::Test::TestCooperativeDeadlineReachedDuringRootActivation(runner);
    Asteria::Test::TestExternalFrameCompletionAndReadmission(runner);
    Asteria::Test::TestPageFaultFrameCompletionBlocksAndRetains(runner);
    Asteria::Test::TestPageFaultFrameRejectsCooperativeMode(runner);
    Asteria::Test::TestCoordinatorOnlyExternalFrameDispatch(runner);
    Asteria::Test::TestCoordinatorDispatchRollsBackPassedStop(runner);
    Asteria::Test::TestMultipleRetainedFramesFollowPolicy(runner);
    Asteria::Test::TestPendingExternalResumeRejectsActiveMutations(runner);
    Asteria::Test::TestExternalStopFailuresRetainFrameOwnership(runner);
    Asteria::Test::TestExternalContinuationRefreshFailuresRetainOwnership(runner);
    Asteria::Test::TestExternalResumeCommitRejectsReplayAndStaleContinuation(runner);
    Asteria::Test::TestExternalResumeGenerationSurvivesRunLoopRecreation(runner);
    Asteria::Test::TestExternalResumeGenerationExhaustionRetainsFrames(runner);
    Asteria::Test::TestExternalFrameAdmissionRollbackRetainsOwnership(runner);
    Asteria::Test::TestBoundRemovalGuards(runner);
    Asteria::Test::TestBindings(runner);
    Asteria::Test::TestIdleAndAdmissionRollback(runner);
    Asteria::Test::TestBindWhileActive(runner);
    Asteria::Test::TestUnbindWhileActive(runner);
    Asteria::Test::TestRollbackFindsTimerQueueDeadlineDue(runner);
    Asteria::Test::TestUnboundCandidateAfterActiveCompletion(runner);
    Asteria::Test::TestUnpreparedBindingLeavesQueue(runner);
    Asteria::Test::TestIdleRootRestorationRetriesRollbackSafeRejections(runner);
    Asteria::Test::TestIdleRootContentionRetryIsBounded(runner);
    Asteria::Test::TestIdleRootUnknownCommitIsNotRetried(runner);
    Asteria::Test::TestBlockedCompletionRetriesIdleRoot(runner);
    Asteria::Test::TestProcessorMismatch(runner);
    Asteria::Test::TestNativeRejectionRollback(runner);
    Asteria::Test::TestAddressSpaceRejectionRollback(runner);
    Asteria::Test::TestDeadlineReachedDuringRootActivation(runner);
    Asteria::Test::TestActiveDeadlineRecoveryRestoresCurrentRoot(runner);
    Asteria::Test::TestDestinationUnknownCommitIsTerminal(runner);
    return runner.Finish();
}
