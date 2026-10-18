// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Scheduler/SchedulingContext.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Amd64/Scheduler/Amd64ProcessorRunLoop.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

using Asteria::Foundation::Size;
using Asteria::Foundation::UInt8;
using Asteria::Foundation::UInt64;
using Asteria::Hal::Memory::AddressSpacePageTableActivationError;
using Asteria::Hal::Memory::AddressSpacePageTableError;
using Asteria::Hal::Memory::AddressSpacePageTableMapResult;
using Asteria::Hal::Memory::AddressSpacePageTableOwnershipError;
using Asteria::Hal::Memory::IAddressSpacePageTable;
using Asteria::Hal::Time::ILocalTimer;
using Asteria::Hal::Time::LocalTimerError;
using Asteria::Kernel::Ipc::EndpointErrorCode;
using Asteria::Kernel::Object::MonotonicTime;
using Asteria::Kernel::Object::CapabilityError;
using Asteria::Kernel::Object::CapabilityHandle;
using Asteria::Kernel::Object::CapabilityRight;
using Asteria::Kernel::Object::CapabilityRights;
using Asteria::Kernel::Object::CapabilitySpace;
using Asteria::Kernel::Object::IKernelObject;
using Asteria::Kernel::Object::ObjectId;
using Asteria::Kernel::Object::ObjectRegistry;
using Asteria::Kernel::Object::ObjectType;
using Asteria::Kernel::Object::ResourceDomainId;
using Asteria::Kernel::Object::RevocationRegistry;
using Asteria::Kernel::Object::RootCapabilityIssuer;
using Asteria::Kernel::Memory::AddressSpaceId;
using Asteria::Kernel::Memory::AddressSpace;
using Asteria::Kernel::Memory::AddressSpaceError;
using Asteria::Kernel::Memory::AddressSpaceMappingRecord;
using Asteria::Kernel::Memory::InitialMapping;
using Asteria::Kernel::Memory::InitialMappingOwner;
using Asteria::Kernel::Memory::InitialMappingPlan;
using Asteria::Kernel::Memory::InitialMappingPlanError;
using Asteria::Kernel::Memory::UserMemoryAccessError;
using Asteria::Kernel::Memory::MappingPermission;
using Asteria::Kernel::Memory::MappingPermissions;
using Asteria::Kernel::Memory::MemoryCachePolicy;
using Asteria::Kernel::Memory::PageCount;
using Asteria::Kernel::Memory::PhysicalAddress;
using Asteria::Kernel::Memory::ProcessorAddressSpaceController;
using Asteria::Kernel::Memory::ProcessorAddressSpaceControllerError;
using Asteria::Kernel::Memory::ProcessorAddressSpaceControllerState;
using Asteria::Kernel::Memory::VirtualAddress;
using Asteria::Kernel::Protection::ProtectionDomain;
using Asteria::Kernel::Protection::ProtectionDomainError;
using Asteria::Kernel::Protection::ProtectionDomainId;
using Asteria::Kernel::Resource::ResourceDomain;
using Asteria::Kernel::Resource::ResourceDomainError;
using Asteria::Kernel::Resource::ResourceDomainLedger;
using Asteria::Kernel::Resource::ResourceDomainLimits;
using Asteria::Kernel::Resource::ResourceDomainObjectId;
using Asteria::Kernel::Scheduler::CpuId;
using Asteria::Kernel::Scheduler::ProcessorKernelRunLoopError;
using Asteria::Kernel::Scheduler::ProcessorKernelRunLoopFailure;
using Asteria::Kernel::Scheduler::ProcessorKernelRunLoopReceipt;
using Asteria::Kernel::Scheduler::ProcessorKernelRunLoopState;
using Asteria::Kernel::Scheduler::ProcessorRunController;
using Asteria::Kernel::Scheduler::ProcessorRunError;
using Asteria::Kernel::Scheduler::ProcessorRunState;
using Asteria::Kernel::Scheduler::ProcessorSchedulerState;
using Asteria::Kernel::Scheduler::SchedulingClass;
using Asteria::Kernel::Scheduler::SchedulingContext;
using Asteria::Kernel::Scheduler::SchedulingContextConfiguration;
using Asteria::Kernel::Scheduler::SchedulingContextError;
using Asteria::Kernel::Scheduler::SchedulingContextId;
using Asteria::Kernel::Scheduler::Thread;
using Asteria::Kernel::Scheduler::ThreadBlockToken;
using Asteria::Kernel::Scheduler::ThreadError;
using Asteria::Kernel::Scheduler::ThreadId;
using Asteria::Kernel::Scheduler::ThreadRunDisposition;
using Asteria::Kernel::Scheduler::ThreadState;
using Asteria::Kernel::System::SystemCallError;
using Asteria::Kernel::System::IpcSystemCallDispatchError;
using Asteria::Kernel::System::SystemCallIngress;
using Asteria::Kernel::System::SystemCallIngressError;
using Asteria::Kernel::System::SystemCallIngressFailure;
using Asteria::Kernel::System::SystemCallNumber;
using Asteria::Kernel::System::SystemCallRequest;
using Asteria::Kernel::System::SystemCallResponse;
using Asteria::Kernel::Time::TimerQueue;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContext;
using Asteria::Platform::Amd64::Scheduler::Amd64KernelContextError;
using Asteria::Platform::Amd64::Scheduler::Amd64ProcessorRunLoop;
using Asteria::Platform::Amd64::Scheduler::WritableAmd64KernelStackView;
using Asteria::Platform::Synchronization::SpinLock;

namespace
{
class ManualLocalTimer final : public ILocalTimer
{
public:
    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        return m_ticks;
    }

    [[nodiscard]] LocalTimerError ProgramDeadline(const UInt64 deadlineTicks) noexcept override
    {
        if (deadlineTicks <= m_ticks)
        {
            return LocalTimerError::DeadlineReached;
        }
        m_deadline = deadlineTicks;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError CancelDeadline() noexcept override
    {
        m_deadline = 0U;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError AcknowledgeExternallyMaskedDeadline(const UInt64 deadlineTicks) noexcept override
    {
        if ((deadlineTicks == 0U) || (deadlineTicks != m_deadline))
        {
            return LocalTimerError::InvalidDeadline;
        }
        m_deadline = 0U;
        return LocalTimerError::None;
    }

    void SetTicks(const UInt64 ticks) noexcept
    {
        m_ticks = ticks;
    }

    [[nodiscard]] UInt64 GetDeadline() const noexcept
    {
        return m_deadline;
    }

private:
    UInt64 m_ticks{10U};
    UInt64 m_deadline{0U};
};

struct ActivationObservation
{
    Size Sequence;
    AddressSpacePageTableActivationError Result;
    bool ExpectedPreviousWasNativeActive;
    bool NativeRootChanged;
};

struct NativeRootEvidence
{
    const IAddressSpacePageTable* ActiveBackend;
    Size EventSequence;
    Size RejectionEvent;
    Size RollbackEvent;
    Size DestinationEntryEvent;
};

class TestAddressSpacePageTable final : public IAddressSpacePageTable
{
public:
    explicit constexpr TestAddressSpacePageTable(Size& activationSequence,
                                                 NativeRootEvidence& nativeRootEvidence) noexcept
        : m_activationSequence{activationSequence}, m_nativeRootEvidence{nativeRootEvidence}
    {
    }

    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Claim(const AddressSpace& owner,
                                                            const InitialMappingPlan& seedPlan) noexcept override
    {
        if ((m_owner != nullptr) || (m_seedPlan != nullptr))
        {
            return AddressSpacePageTableOwnershipError::AlreadyOwned;
        }
        if (!seedPlan.IsSealed())
        {
            return AddressSpacePageTableOwnershipError::SeedMismatch;
        }
        m_owner = &owner;
        m_seedPlan = &seedPlan;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Release(const AddressSpace& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return AddressSpacePageTableOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        m_seedPlan = nullptr;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableActivationError Activate(const AddressSpace& owner,
                                                                const CpuId processor) noexcept override
    {
        AddressSpacePageTableActivationError result = m_nextActivationError;
        if ((m_owner != &owner) || !processor.IsValid())
        {
            result = AddressSpacePageTableActivationError::InvalidOwner;
        }
        const bool expectedPreviousWasNativeActive = m_nativeRootEvidence.ActiveBackend == m_expectedPreviousBackend;
        const bool nativeRootChanged = result == AddressSpacePageTableActivationError::None;
        ++m_activationSequence;
        if (m_activationCalls < MaximumActivationObservations)
        {
            m_observations[m_activationCalls] = ActivationObservation{
                .Sequence = m_activationSequence,
                .Result = result,
                .ExpectedPreviousWasNativeActive = expectedPreviousWasNativeActive,
                .NativeRootChanged = nativeRootChanged,
            };
        }
        ++m_activationCalls;
        if (result == AddressSpacePageTableActivationError::None)
        {
            m_nativeRootEvidence.ActiveBackend = this;
            ++m_successfulActivationCalls;
        }
        else
        {
            if (result == AddressSpacePageTableActivationError::RejectedBeforeCommit)
            {
                ++m_nativeRootEvidence.EventSequence;
                m_nativeRootEvidence.RejectionEvent = m_nativeRootEvidence.EventSequence;
            }
            ++m_rejectedActivationCalls;
        }
        m_nextActivationError = AddressSpacePageTableActivationError::None;
        return result;
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

    void ObservePreviousBackend(const TestAddressSpacePageTable& previousBackend) noexcept
    {
        m_expectedPreviousBackend = &previousBackend;
    }

    void RejectNextActivationBeforeCommit() noexcept
    {
        m_nextActivationError = AddressSpacePageTableActivationError::RejectedBeforeCommit;
    }

    [[nodiscard]] constexpr const AddressSpace* GetOwner() const noexcept
    {
        return m_owner;
    }

    [[nodiscard]] constexpr const InitialMappingPlan* GetSeedPlan() const noexcept
    {
        return m_seedPlan;
    }

    [[nodiscard]] constexpr Size GetActivationCalls() const noexcept
    {
        return m_activationCalls;
    }

    [[nodiscard]] constexpr Size GetSuccessfulActivationCalls() const noexcept
    {
        return m_successfulActivationCalls;
    }

    [[nodiscard]] constexpr Size GetRejectedActivationCalls() const noexcept
    {
        return m_rejectedActivationCalls;
    }

    [[nodiscard]] constexpr ActivationObservation GetObservation(const Size index) const noexcept
    {
        return index < MaximumActivationObservations ? m_observations[index] : ActivationObservation{};
    }

private:
    static constexpr Size MaximumActivationObservations{8U};

    Size& m_activationSequence;
    NativeRootEvidence& m_nativeRootEvidence;
    const AddressSpace* m_owner{nullptr};
    const InitialMappingPlan* m_seedPlan{nullptr};
    const TestAddressSpacePageTable* m_expectedPreviousBackend{nullptr};
    ActivationObservation m_observations[MaximumActivationObservations]{};
    AddressSpacePageTableActivationError m_nextActivationError{AddressSpacePageTableActivationError::None};
    Size m_activationCalls{0U};
    Size m_successfulActivationCalls{0U};
    Size m_rejectedActivationCalls{0U};
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

class FaultIngressRecorder final
{
public:
    constexpr FaultIngressRecorder(Thread& thread, SchedulingContext& context,
                                   ProcessorRunController& controller) noexcept
        : m_thread{thread}, m_context{context}, m_controller{controller}
    {
    }

    void CommitBlocked(const ThreadBlockToken token) noexcept
    {
        const auto thread = m_thread.GetSnapshot();
        const auto context = m_context.GetSnapshot();
        m_token = token;
        m_sawBlocked = (thread.State == ThreadState::Blocked) && thread.BlockToken.IsEqualTo(token) &&
                       (context.State != Asteria::Kernel::Scheduler::SchedulingContextState::Running) &&
                       (m_controller.GetSnapshot().State == Asteria::Kernel::Scheduler::ProcessorRunState::Idle);
        ++m_calls;
    }

    [[nodiscard]] constexpr Size GetCalls() const noexcept
    {
        return m_calls;
    }

    [[nodiscard]] constexpr bool SawBlocked() const noexcept
    {
        return m_sawBlocked;
    }

    [[nodiscard]] constexpr ThreadBlockToken GetToken() const noexcept
    {
        return m_token;
    }

private:
    Thread& m_thread;
    SchedulingContext& m_context;
    ProcessorRunController& m_controller;
    ThreadBlockToken m_token{};
    Size m_calls{0U};
    bool m_sawBlocked{false};
};

SpinLock g_registryLock;
Asteria::TestSupport::TestObjectRegistry g_registry{g_registryLock};
RootCapabilityIssuer g_issuer{g_registry};
ManualLocalTimer g_timer;
RevocationRegistry g_revocations{g_registryLock};
CapabilitySpace g_capabilities{g_registry, g_revocations, g_timer, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
CapabilitySpace g_otherCapabilities{g_registry, g_revocations, g_timer, 0x1357'9BDF'2468'ACE0ULL,
                                    0x0246'8ACE'1357'9BDFULL};
ResourceDomainLedger g_resourceLedger;
ResourceDomain g_resourceDomain{g_resourceLedger};
Size g_rootActivationSequence{0U};
NativeRootEvidence g_nativeRootEvidence{};
TestAddressSpacePageTable g_pageTable{g_rootActivationSequence, g_nativeRootEvidence};
TestAddressSpacePageTable g_otherPageTable{g_rootActivationSequence, g_nativeRootEvidence};
InitialMappingPlan g_seedPlan;
InitialMappingPlan g_otherSeedPlan;
AddressSpace g_addressSpace;
AddressSpace g_otherAddressSpace;
ProtectionDomain g_protectionDomain;
ProtectionDomain g_otherProtectionDomain;
TimerQueue g_timerQueue{g_registry, g_timer};
Thread g_thread;
SchedulingContext g_schedulingContext{g_registry};
Thread g_secondThread;
SchedulingContext g_secondSchedulingContext{g_registry};
Amd64KernelContext g_idleContext;
Amd64KernelContext g_threadContext;
Amd64KernelContext g_secondThreadContext;
ProcessorRunController* g_controller{nullptr};
ProcessorAddressSpaceController* g_addressSpaces{nullptr};
Amd64ProcessorRunLoop* g_runLoop{nullptr};
FaultIngressRecorder* g_faultIngress{nullptr};
alignas(4096) UInt8 g_threadStack[16U * 1024U]{};
alignas(4096) UInt8 g_secondThreadStack[16U * 1024U]{};
ThreadId g_threadId;
SchedulingContextId g_schedulingContextId;
ThreadId g_secondThreadId;
SchedulingContextId g_secondSchedulingContextId;
ObjectId g_addressSpaceObject;
ObjectId g_otherAddressSpaceObject;
ObjectId g_resourceDomainObject;
ObjectId g_protectionDomainObject;
ObjectId g_otherProtectionDomainObject;
ProtectionDomainId g_protectionDomainId;
ProtectionDomainId g_otherProtectionDomainId;
AddressSpaceId g_addressSpaceId;
AddressSpaceId g_otherAddressSpaceId;
CapabilityHandle g_ingressCapability;
int g_phase{0};
int g_crossThreadPhase{0};
Size g_threadEntryCalls{0U};
Size g_secondThreadEntryCalls{0U};
int g_failure{0};

[[nodiscard]] ProcessorRunController& Controller() noexcept
{
    return *g_controller;
}

[[nodiscard]] ProcessorAddressSpaceController& AddressSpaces() noexcept
{
    return *g_addressSpaces;
}

[[nodiscard]] Amd64ProcessorRunLoop& RunLoop() noexcept
{
    return *g_runLoop;
}

[[nodiscard]] FaultIngressRecorder& FaultIngress() noexcept
{
    return *g_faultIngress;
}

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = Asteria::Kernel::Scheduler::SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr SchedulingContextConfiguration SecondConfiguration() noexcept
{
    SchedulingContextConfiguration configuration = Configuration();
    configuration.Priority = 9U;
    return configuration;
}

[[nodiscard]] WritableAmd64KernelStackView ThreadStack() noexcept
{
    return WritableAmd64KernelStackView{
        .Data = g_threadStack,
        .SizeBytes = sizeof(g_threadStack),
        .VirtualStart = static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(g_threadStack)),
    };
}

[[nodiscard]] WritableAmd64KernelStackView SecondThreadStack() noexcept
{
    return WritableAmd64KernelStackView{
        .Data = g_secondThreadStack,
        .SizeBytes = sizeof(g_secondThreadStack),
        .VirtualStart = static_cast<UInt64>(reinterpret_cast<Asteria::Foundation::UIntPointer>(g_secondThreadStack)),
    };
}

[[nodiscard]] constexpr SystemCallRequest MakeSystemCallRequest(const SystemCallNumber number,
                                                                const CapabilityHandle handle = {}) noexcept
{
    return SystemCallRequest{
        .Number = static_cast<UInt64>(number),
        .Arguments =
            {
                handle.GetSelector(),
                handle.GetAuthenticator(),
                0U,
                0U,
                0U,
                0U,
            },
    };
}

[[nodiscard]] constexpr SystemCallIngressFailure MakeIngressFailureSentinel() noexcept
{
    return SystemCallIngressFailure{
        .Error = SystemCallIngressError::CapabilitySpaceIdentifierMismatch,
        .ProcessorRunLoop = ProcessorKernelRunLoopError::NativeSwitchRejected,
        .ProtectionDomain = ProtectionDomainError::InUse,
        .Ipc =
            {
                .Error = IpcSystemCallDispatchError::InvalidContext,
                .Endpoint = EndpointErrorCode::QueueCorrupt,
                .UserMemory = UserMemoryAccessError::Fault,
                .Reserved = 4U,
            },
        .Reserved = 5U,
    };
}

[[nodiscard]] constexpr bool IngressFailureIsSentinel(const SystemCallIngressFailure& failure) noexcept
{
    return (failure.Error == SystemCallIngressError::CapabilitySpaceIdentifierMismatch) &&
           (failure.ProcessorRunLoop == ProcessorKernelRunLoopError::NativeSwitchRejected) &&
           (failure.ProtectionDomain == ProtectionDomainError::InUse) &&
           (failure.Ipc.Error == IpcSystemCallDispatchError::InvalidContext) &&
           (failure.Ipc.Endpoint == EndpointErrorCode::QueueCorrupt) &&
           (failure.Ipc.UserMemory == UserMemoryAccessError::Fault) && (failure.Ipc.Reserved == 4U) &&
           (failure.Reserved == 5U);
}

void ThreadEntry(void* const argument) noexcept
{
    ++g_threadEntryCalls;
    const auto active = RunLoop().GetSnapshot();
    const auto activeBinding = RunLoop().GetActiveProtectionDomainBinding();
    if ((argument != &g_phase) || (g_phase != 0) || (active.ActiveProtectionDomain != &g_protectionDomain) ||
        !active.ActiveProtectionDomainIdentity.IsEqualTo(g_protectionDomainId) ||
        (active.ActiveAddressSpace != &g_addressSpace) ||
        !active.ActiveAddressSpaceIdentity.IsEqualTo(g_addressSpaceId) || !activeBinding.HasValue() ||
        (activeBinding.GetValue().Storage != &g_protectionDomain) ||
        !activeBinding.GetValue().Identity.IsEqualTo(g_protectionDomainId) ||
        (activeBinding.GetValue().AddressSpaceStorage != &g_addressSpace) ||
        !activeBinding.GetValue().AddressSpaceIdentity.IsEqualTo(g_addressSpaceId))
    {
        g_failure = 1;
    }

    SystemCallRequest unknown = MakeSystemCallRequest(SystemCallNumber::CapabilityQuery);
    unknown.Number = 0xFFFF'FFFF'FFFF'FFFFULL;
    unknown.Arguments[5U] = 0x55AAU;
    SystemCallResponse unknownResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 1U,
        .Value1 = 2U,
    };
    SystemCallIngressFailure unknownFailure = MakeIngressFailureSentinel();
    if (!SystemCallIngress::Dispatch(RunLoop(), unknown, unknownResponse, unknownFailure) ||
        (unknownResponse.Error != SystemCallError::UnknownSystemCall) || (unknownResponse.Value0 != 0U) ||
        (unknownResponse.Value1 != 0U) || !IngressFailureIsSentinel(unknownFailure))
    {
        g_failure = 10;
    }

    SystemCallResponse queryResponse{
        .Error = SystemCallError::InvalidConfiguration,
        .Value0 = 0U,
        .Value1 = 0U,
    };
    SystemCallIngressFailure queryFailure = MakeIngressFailureSentinel();
    if (!SystemCallIngress::Dispatch(RunLoop(),
                                     MakeSystemCallRequest(SystemCallNumber::CapabilityQuery, g_ingressCapability),
                                     queryResponse, queryFailure) ||
        (queryResponse.Error != SystemCallError::None) || !IngressFailureIsSentinel(queryFailure))
    {
        g_failure = 11;
    }
    if ((g_secondThread.MakeReady() != ThreadError::None) ||
        !Controller()
             .Enqueue(g_secondSchedulingContextId, g_secondSchedulingContext, g_secondThreadId, g_secondThread)
             .HasValue())
    {
        g_failure = 2;
    }

    ProcessorKernelRunLoopReceipt readyReceipt{};
    ProcessorKernelRunLoopFailure readyFailure{};
    const bool ready = RunLoop().FinishAndDispatch(g_thread, g_threadContext, ThreadRunDisposition::Ready, 0U,
                                                   readyReceipt, readyFailure);
    if (!ready || (readyReceipt.ThreadStorage != &g_thread) ||
        (readyReceipt.State != ProcessorKernelRunLoopState::Active) || (g_crossThreadPhase != 1))
    {
        g_failure = 3;
    }
    g_phase = 1;

    const auto secondBlocked = g_secondThread.GetSnapshot();
    if (!secondBlocked.BlockToken.IsValid() || (g_secondThread.Wake(secondBlocked.BlockToken) != ThreadError::None) ||
        !Controller()
             .Enqueue(g_secondSchedulingContextId, g_secondSchedulingContext, g_secondThreadId, g_secondThread)
             .HasValue())
    {
        g_failure = 4;
    }

    ProcessorKernelRunLoopReceipt blockedReceipt{};
    ProcessorKernelRunLoopFailure blockedFailure{};
    const bool blocked =
        RunLoop().FinishFaultAndDispatch(g_thread, g_threadContext, FaultIngress(), blockedReceipt, blockedFailure);
    if (!blocked || (blockedReceipt.ThreadStorage != &g_thread) ||
        (blockedReceipt.State != ProcessorKernelRunLoopState::Active))
    {
        g_failure = 5;
    }

    g_phase = 2;
    ProcessorKernelRunLoopReceipt exitedReceipt{};
    ProcessorKernelRunLoopFailure exitedFailure{};
    const bool exited = RunLoop().FinishAndDispatch(g_thread, g_threadContext, ThreadRunDisposition::Exited, 0xA57E'12U,
                                                    exitedReceipt, exitedFailure);
    static_cast<void>(exited);
    g_failure = 6;
    __builtin_trap();
}

void SecondThreadEntry(void* const argument) noexcept
{
    ++g_nativeRootEvidence.EventSequence;
    g_nativeRootEvidence.DestinationEntryEvent = g_nativeRootEvidence.EventSequence;
    ++g_secondThreadEntryCalls;
    const auto active = RunLoop().GetSnapshot();
    const auto activeBinding = RunLoop().GetActiveProtectionDomainBinding();
    if ((argument != &g_crossThreadPhase) || (g_crossThreadPhase != 0) ||
        (active.ActiveProtectionDomain != &g_otherProtectionDomain) ||
        !active.ActiveProtectionDomainIdentity.IsEqualTo(g_otherProtectionDomainId) ||
        (active.ActiveAddressSpace != &g_otherAddressSpace) ||
        !active.ActiveAddressSpaceIdentity.IsEqualTo(g_otherAddressSpaceId) || !activeBinding.HasValue() ||
        (activeBinding.GetValue().Storage != &g_otherProtectionDomain) ||
        !activeBinding.GetValue().Identity.IsEqualTo(g_otherProtectionDomainId) ||
        (activeBinding.GetValue().AddressSpaceStorage != &g_otherAddressSpace) ||
        !activeBinding.GetValue().AddressSpaceIdentity.IsEqualTo(g_otherAddressSpaceId))
    {
        g_failure = 7;
    }

    SystemCallResponse foreignQueryResponse{
        .Error = SystemCallError::None,
        .Value0 = 1U,
        .Value1 = 2U,
    };
    SystemCallIngressFailure foreignQueryFailure = MakeIngressFailureSentinel();
    if (!SystemCallIngress::Dispatch(RunLoop(),
                                     MakeSystemCallRequest(SystemCallNumber::CapabilityQuery, g_ingressCapability),
                                     foreignQueryResponse, foreignQueryFailure) ||
        (foreignQueryResponse.Error != SystemCallError::InvalidHandle) || (foreignQueryResponse.Value0 != 0U) ||
        (foreignQueryResponse.Value1 != 0U) || !IngressFailureIsSentinel(foreignQueryFailure))
    {
        g_failure = 12;
    }
    g_crossThreadPhase = 1;

    ProcessorKernelRunLoopReceipt blockedReceipt{};
    ProcessorKernelRunLoopFailure blockedFailure{};
    const bool blocked = RunLoop().FinishAndDispatch(g_secondThread, g_secondThreadContext,
                                                     ThreadRunDisposition::Blocked, 0U, blockedReceipt, blockedFailure);
    if (!blocked || (blockedReceipt.ThreadStorage != &g_secondThread) ||
        (blockedReceipt.State != ProcessorKernelRunLoopState::Active))
    {
        g_failure = 8;
    }

    g_crossThreadPhase = 2;
    ProcessorKernelRunLoopReceipt exitedReceipt{};
    ProcessorKernelRunLoopFailure exitedFailure{};
    const bool exited = RunLoop().FinishAndDispatch(g_secondThread, g_secondThreadContext, ThreadRunDisposition::Exited,
                                                    0xB57E'34U, exitedReceipt, exitedFailure);
    static_cast<void>(exited);
    g_failure = 9;
    __builtin_trap();
}

[[nodiscard]] bool InitializeObjects(ObjectId& threadObject, ObjectId& contextObject, ObjectId& secondThreadObject,
                                     ObjectId& secondContextObject) noexcept
{
    if (!BuildSeedPlan(g_seedPlan) || !BuildSeedPlan(g_otherSeedPlan) ||
        (g_addressSpace.Initialize(ResourceDomainId{7U}, g_pageTable, g_seedPlan) != AddressSpaceError::None) ||
        (g_otherAddressSpace.Initialize(ResourceDomainId{8U}, g_otherPageTable, g_otherSeedPlan) !=
         AddressSpaceError::None) ||
        (g_thread.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
        (g_secondThread.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
        (g_schedulingContext.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
         SchedulingContextError::None) ||
        (g_secondSchedulingContext.Initialize(SecondConfiguration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
         SchedulingContextError::None))
    {
        return false;
    }

    const auto threadResult = g_issuer.CreateObject(g_thread);
    const auto contextResult = g_issuer.CreateObject(g_schedulingContext);
    const auto secondThreadResult = g_issuer.CreateObject(g_secondThread);
    const auto secondContextResult = g_issuer.CreateObject(g_secondSchedulingContext);
    const ResourceDomainLimits resourceLimits{
        .CommittedMemoryPages = 4096U,
        .ResidentMemoryPages = 4096U,
        .KernelMetadataBytes = 64U * 1024U,
        .CapabilityHandles = 4096U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 256U,
        .ChildDomains = 16U,
    };
    if (g_resourceDomain.InitializeRoot(ResourceDomainId{9U}, resourceLimits).Error != ResourceDomainError::None)
    {
        return false;
    }
    const auto resourceDomainResult = g_issuer.CreateObject(g_resourceDomain);
    const auto addressSpaceResult = g_issuer.CreateObject(g_addressSpace);
    const auto otherAddressSpaceResult = g_issuer.CreateObject(g_otherAddressSpace);
    if (!threadResult.HasValue() || !contextResult.HasValue() || !secondThreadResult.HasValue() ||
        !secondContextResult.HasValue() || !resourceDomainResult.HasValue() || !addressSpaceResult.HasValue() ||
        !otherAddressSpaceResult.HasValue())
    {
        return false;
    }
    threadObject = threadResult.GetValue();
    contextObject = contextResult.GetValue();
    secondThreadObject = secondThreadResult.GetValue();
    secondContextObject = secondContextResult.GetValue();
    g_resourceDomainObject = resourceDomainResult.GetValue();
    g_addressSpaceObject = addressSpaceResult.GetValue();
    g_otherAddressSpaceObject = otherAddressSpaceResult.GetValue();
    const ResourceDomainObjectId resourceDomainId{g_registry, g_resourceDomainObject};
    g_addressSpaceId = AddressSpaceId{g_registry, g_addressSpaceObject};
    g_otherAddressSpaceId = AddressSpaceId{g_registry, g_otherAddressSpaceObject};
    g_pageTable.ObservePreviousBackend(g_otherPageTable);
    g_otherPageTable.ObservePreviousBackend(g_pageTable);
    if ((g_addressSpace.BindIdentity(g_registry, g_addressSpaceId).Error != AddressSpaceError::None) ||
        (g_otherAddressSpace.BindIdentity(g_registry, g_otherAddressSpaceId).Error != AddressSpaceError::None) ||
        g_addressSpaceId.IsEqualTo(g_otherAddressSpaceId) || (g_pageTable.GetOwner() != &g_addressSpace) ||
        (g_otherPageTable.GetOwner() != &g_otherAddressSpace) || (g_pageTable.GetSeedPlan() != &g_seedPlan) ||
        (g_otherPageTable.GetSeedPlan() != &g_otherSeedPlan) ||
        (g_pageTable.GetSeedPlan() == g_otherPageTable.GetSeedPlan()) || !g_seedPlan.IsSealed() ||
        !g_otherSeedPlan.IsSealed() ||
        g_protectionDomain.Initialize(g_registry, resourceDomainId, g_resourceDomain, g_addressSpaceId, g_capabilities)
                .Error != ProtectionDomainError::None ||
        g_otherProtectionDomain
                .Initialize(g_registry, resourceDomainId, g_resourceDomain, g_otherAddressSpaceId, g_otherCapabilities)
                .Error != ProtectionDomainError::None)
    {
        return false;
    }
    const auto protectionDomainResult = g_issuer.CreateObject(g_protectionDomain);
    const auto otherProtectionDomainResult = g_issuer.CreateObject(g_otherProtectionDomain);
    if (!protectionDomainResult.HasValue() || !otherProtectionDomainResult.HasValue())
    {
        return false;
    }
    g_protectionDomainObject = protectionDomainResult.GetValue();
    g_otherProtectionDomainObject = otherProtectionDomainResult.GetValue();
    g_protectionDomainId = ProtectionDomainId{g_registry, g_protectionDomainObject};
    g_otherProtectionDomainId = ProtectionDomainId{g_registry, g_otherProtectionDomainObject};
    if (g_protectionDomain.BindIdentity(g_registry, g_protectionDomainId).Error != ProtectionDomainError::None ||
        g_otherProtectionDomain.BindIdentity(g_registry, g_otherProtectionDomainId).Error !=
            ProtectionDomainError::None)
    {
        return false;
    }
    const auto revocation = g_revocations.CreateRoot();
    if (!revocation.HasValue())
    {
        return false;
    }
    const auto capability =
        g_issuer.Mint(g_capabilities, g_addressSpaceObject, CapabilityRights{CapabilityRight::Inspect},
                      revocation.GetValue(), MonotonicTime::Infinite(), 0x1234U);
    if (!capability.HasValue())
    {
        return false;
    }
    g_ingressCapability = capability.GetValue();
    g_threadId = ThreadId{g_registry, threadObject};
    g_schedulingContextId = SchedulingContextId{g_registry, contextObject};
    g_secondThreadId = ThreadId{g_registry, secondThreadObject};
    g_secondSchedulingContextId = SchedulingContextId{g_registry, secondContextObject};
    return (g_schedulingContext.Bind(g_threadId) == SchedulingContextError::None) &&
           (g_secondSchedulingContext.Bind(g_secondThreadId) == SchedulingContextError::None) &&
           (g_thread.MakeReady() == ThreadError::None);
}

[[nodiscard]] bool VerifyDistinctRootBindings() noexcept
{
    const auto firstBinding = g_protectionDomain.GetBinding();
    const auto secondBinding = g_otherProtectionDomain.GetBinding();
    const auto firstRoot = g_addressSpace.GetSnapshot();
    const auto secondRoot = g_otherAddressSpace.GetSnapshot();
    const auto controller = AddressSpaces().GetSnapshot();
    const ActivationObservation initialActivation = g_pageTable.GetObservation(0U);
    return firstBinding.HasValue() && secondBinding.HasValue() &&
           (firstBinding.GetValue().AddressSpaceStorage == &g_addressSpace) &&
           firstBinding.GetValue().AddressSpace.IsEqualTo(g_addressSpaceId) &&
           (secondBinding.GetValue().AddressSpaceStorage == &g_otherAddressSpace) &&
           secondBinding.GetValue().AddressSpace.IsEqualTo(g_otherAddressSpaceId) &&
           !firstBinding.GetValue().AddressSpace.IsEqualTo(secondBinding.GetValue().AddressSpace) &&
           firstRoot.Identity.IsEqualTo(g_addressSpaceId) && secondRoot.Identity.IsEqualTo(g_otherAddressSpaceId) &&
           !firstRoot.Identity.IsEqualTo(secondRoot.Identity) && (g_pageTable.GetOwner() == &g_addressSpace) &&
           (g_otherPageTable.GetOwner() == &g_otherAddressSpace) && (g_pageTable.GetSeedPlan() == &g_seedPlan) &&
           (g_otherPageTable.GetSeedPlan() == &g_otherSeedPlan) &&
           (g_pageTable.GetSeedPlan() != g_otherPageTable.GetSeedPlan()) && g_seedPlan.IsSealed() &&
           g_otherSeedPlan.IsSealed() && (controller.State == ProcessorAddressSpaceControllerState::Ready) &&
           (controller.IdleStorage == &g_addressSpace) && controller.Idle.IsEqualTo(g_addressSpaceId) &&
           (controller.ActiveStorage == &g_addressSpace) && controller.Active.IsEqualTo(g_addressSpaceId) &&
           !controller.SourceTransition.IsValid() && !controller.DestinationTransition.IsValid() &&
           (firstRoot.ResidentProcessorMask == 0x01U) && (firstRoot.TransitionSequence == 0U) &&
           !firstRoot.TransitionProcessor.IsValid() && (secondRoot.ResidentProcessorMask == 0U) &&
           (secondRoot.TransitionSequence == 0U) && !secondRoot.TransitionProcessor.IsValid() &&
           (g_rootActivationSequence == 1U) && (g_pageTable.GetActivationCalls() == 1U) &&
           (g_pageTable.GetSuccessfulActivationCalls() == 1U) && (g_pageTable.GetRejectedActivationCalls() == 0U) &&
           (g_otherPageTable.GetActivationCalls() == 0U) && (initialActivation.Sequence == 1U) &&
           (initialActivation.Result == AddressSpacePageTableActivationError::None) &&
           !initialActivation.ExpectedPreviousWasNativeActive && initialActivation.NativeRootChanged &&
           (g_nativeRootEvidence.ActiveBackend == &g_pageTable) && (g_nativeRootEvidence.EventSequence == 0U) &&
           (g_nativeRootEvidence.RejectionEvent == 0U) && (g_nativeRootEvidence.RollbackEvent == 0U) &&
           (g_nativeRootEvidence.DestinationEntryEvent == 0U);
}

[[nodiscard]] bool VerifyRejectedDestinationActivation(const ProcessorKernelRunLoopFailure& failure) noexcept
{
    const auto controller = Controller().GetSnapshot();
    const auto roots = AddressSpaces().GetSnapshot();
    const auto sourceRoot = g_addressSpace.GetSnapshot();
    const auto destinationRoot = g_otherAddressSpace.GetSnapshot();
    const auto sourceThread = g_thread.GetSnapshot();
    const auto destinationThread = g_secondThread.GetSnapshot();
    const ActivationObservation rejection = g_otherPageTable.GetObservation(0U);
    return (failure.Error == ProcessorKernelRunLoopError::AddressSpaceControllerRejected) &&
           (failure.ThreadStorage == &g_secondThread) &&
           (failure.AddressSpaceController.Error == ProcessorAddressSpaceControllerError::ActivationRejected) &&
           failure.AddressSpaceController.AdmissionRollbackSafe &&
           !failure.AddressSpaceController.Activation.ProcessorMayBeResident &&
           (failure.AddressSpaceController.Activation.PageTable ==
            AddressSpacePageTableActivationError::RejectedBeforeCommit) &&
           (failure.ActiveRunRetained == 0U) && (g_threadEntryCalls == 1U) && (g_secondThreadEntryCalls == 0U) &&
           (g_phase == 0) && (g_crossThreadPhase == 0) && (g_failure == 0) &&
           (sourceThread.State == ThreadState::Ready) && (destinationThread.State == ThreadState::Ready) &&
           (controller.State == ProcessorRunState::Idle) &&
           (controller.Scheduler.State == ProcessorSchedulerState::Idle) &&
           !controller.Scheduler.ActiveThread.IsValid() && !controller.Scheduler.ActiveContext.IsValid() &&
           (controller.Scheduler.Queue.ReadyEntries == 2U) && (controller.Scheduler.Queue.ClaimedEntries == 0U) &&
           (controller.Scheduler.Queue.ReservedEntries == 0U) &&
           (roots.State == ProcessorAddressSpaceControllerState::Ready) && (roots.IdleStorage == &g_addressSpace) &&
           (roots.ActiveStorage == &g_addressSpace) && roots.Active.IsEqualTo(g_addressSpaceId) &&
           (roots.PreviousStorage == nullptr) && !roots.Previous.IsValid() && !roots.SourceTransition.IsValid() &&
           !roots.DestinationTransition.IsValid() && (sourceRoot.ResidentProcessorMask == 0x01U) &&
           (sourceRoot.TransitionSequence == 0U) && !sourceRoot.TransitionProcessor.IsValid() &&
           (destinationRoot.ResidentProcessorMask == 0U) && (destinationRoot.TransitionSequence == 0U) &&
           !destinationRoot.TransitionProcessor.IsValid() &&
           (RunLoop().GetSnapshot().State == ProcessorKernelRunLoopState::Idle) && (g_rootActivationSequence == 2U) &&
           (g_pageTable.GetActivationCalls() == 1U) && (g_otherPageTable.GetActivationCalls() == 1U) &&
           (g_otherPageTable.GetSuccessfulActivationCalls() == 0U) &&
           (g_otherPageTable.GetRejectedActivationCalls() == 1U) && (rejection.Sequence == 2U) &&
           (rejection.Result == AddressSpacePageTableActivationError::RejectedBeforeCommit) &&
           rejection.ExpectedPreviousWasNativeActive && !rejection.NativeRootChanged &&
           (g_nativeRootEvidence.ActiveBackend == &g_pageTable) && (g_nativeRootEvidence.EventSequence == 2U) &&
           (g_nativeRootEvidence.RejectionEvent == 1U) && (g_nativeRootEvidence.RollbackEvent == 2U) &&
           (g_nativeRootEvidence.DestinationEntryEvent == 0U);
}

[[nodiscard]] bool VerifySuccessfulRootTransitions() noexcept
{
    const auto roots = AddressSpaces().GetSnapshot();
    const auto firstRoot = g_addressSpace.GetSnapshot();
    const auto secondRoot = g_otherAddressSpace.GetSnapshot();
    const ActivationObservation firstRetry = g_otherPageTable.GetObservation(1U);
    const ActivationObservation returnToFirst = g_pageTable.GetObservation(1U);
    const ActivationObservation secondSelection = g_otherPageTable.GetObservation(2U);
    const ActivationObservation returnToIdle = g_pageTable.GetObservation(2U);
    return (g_rootActivationSequence == 6U) && (g_pageTable.GetActivationCalls() == 3U) &&
           (g_pageTable.GetSuccessfulActivationCalls() == 3U) && (g_pageTable.GetRejectedActivationCalls() == 0U) &&
           (g_otherPageTable.GetActivationCalls() == 3U) && (g_otherPageTable.GetSuccessfulActivationCalls() == 2U) &&
           (g_otherPageTable.GetRejectedActivationCalls() == 1U) && (firstRetry.Sequence == 3U) &&
           (firstRetry.Result == AddressSpacePageTableActivationError::None) &&
           firstRetry.ExpectedPreviousWasNativeActive && firstRetry.NativeRootChanged &&
           (returnToFirst.Sequence == 4U) && (returnToFirst.Result == AddressSpacePageTableActivationError::None) &&
           returnToFirst.ExpectedPreviousWasNativeActive && returnToFirst.NativeRootChanged &&
           (secondSelection.Sequence == 5U) && (secondSelection.Result == AddressSpacePageTableActivationError::None) &&
           secondSelection.ExpectedPreviousWasNativeActive && secondSelection.NativeRootChanged &&
           (returnToIdle.Sequence == 6U) && (returnToIdle.Result == AddressSpacePageTableActivationError::None) &&
           returnToIdle.ExpectedPreviousWasNativeActive && returnToIdle.NativeRootChanged &&
           (roots.State == ProcessorAddressSpaceControllerState::Ready) && (roots.IdleStorage == &g_addressSpace) &&
           roots.Idle.IsEqualTo(g_addressSpaceId) && (roots.ActiveStorage == &g_addressSpace) &&
           roots.Active.IsEqualTo(g_addressSpaceId) && (roots.PreviousStorage == nullptr) &&
           !roots.Previous.IsValid() && !roots.SourceTransition.IsValid() && !roots.DestinationTransition.IsValid() &&
           (firstRoot.ResidentProcessorMask == 0x01U) && (firstRoot.TransitionSequence == 0U) &&
           !firstRoot.TransitionProcessor.IsValid() && (secondRoot.ResidentProcessorMask == 0U) &&
           (secondRoot.TransitionSequence == 0U) && !secondRoot.TransitionProcessor.IsValid() &&
           (g_nativeRootEvidence.ActiveBackend == &g_pageTable) && (g_nativeRootEvidence.EventSequence == 3U) &&
           (g_nativeRootEvidence.RejectionEvent == 1U) && (g_nativeRootEvidence.RollbackEvent == 2U) &&
           (g_nativeRootEvidence.DestinationEntryEvent == 3U);
}

[[nodiscard]] bool RetireObjects(const ObjectId threadObject, const ObjectId contextObject,
                                 const ObjectId secondThreadObject, const ObjectId secondContextObject) noexcept
{
    if ((g_runLoop != nullptr) || (g_controller != nullptr) || (g_addressSpaces != nullptr) ||
        (g_faultIngress != nullptr) || (g_schedulingContext.Unbind(g_threadId) != SchedulingContextError::None) ||
        (g_secondSchedulingContext.Unbind(g_secondThreadId) != SchedulingContextError::None) ||
        (g_capabilities.Close(g_ingressCapability) != CapabilityError::Ok) ||
        (g_protectionDomain.Quiesce() != ProtectionDomainError::None) ||
        (g_otherProtectionDomain.Quiesce() != ProtectionDomainError::None) ||
        (g_pageTable.GetActivationCalls() != 3U) || (g_otherPageTable.GetActivationCalls() != 3U) ||
        (g_nativeRootEvidence.ActiveBackend != &g_pageTable))
    {
        return false;
    }
    if (g_addressSpace.DetachProcessor(CpuId{0U}) != AddressSpaceError::None)
    {
        return false;
    }
    g_nativeRootEvidence.ActiveBackend = nullptr;
    if ((g_addressSpace.Quiesce() != AddressSpaceError::None) ||
        (g_otherAddressSpace.Quiesce() != AddressSpaceError::None) || (g_pageTable.GetOwner() != nullptr) ||
        (g_otherPageTable.GetOwner() != nullptr) || (g_pageTable.GetSeedPlan() != nullptr) ||
        (g_otherPageTable.GetSeedPlan() != nullptr) || (g_resourceDomain.Quiesce() != ResourceDomainError::None) ||
        (g_issuer.DestroyObject(contextObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(secondContextObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(threadObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(secondThreadObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(g_protectionDomainObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(g_otherProtectionDomainObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(g_resourceDomainObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(g_addressSpaceObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok) ||
        (g_issuer.DestroyObject(g_otherAddressSpaceObject) != Asteria::Kernel::Object::ObjectRegistryError::Ok))
    {
        return false;
    }
    for (UInt8 index{0U}; index < 9U; ++index)
    {
        if (!g_issuer.CollectDestroyedObject().HasValue())
        {
            return false;
        }
    }
    return true;
}
[[nodiscard]] int ExecuteNativeFlow() noexcept
{
    if (!VerifyDistinctRootBindings())
    {
        return 1;
    }
    if (g_threadContext.Prepare(ThreadStack(), &ThreadEntry, &g_phase) != Amd64KernelContextError::None ||
        g_secondThreadContext.Prepare(SecondThreadStack(), &SecondThreadEntry, &g_crossThreadPhase) !=
            Amd64KernelContextError::None)
    {
        return 2;
    }
    if (!Controller().Enqueue(g_schedulingContextId, g_schedulingContext, g_threadId, g_thread).HasValue())
    {
        return 3;
    }

    ProcessorKernelRunLoopReceipt unboundReceipt{};
    ProcessorKernelRunLoopFailure unboundFailure{};
    const bool unbound = RunLoop().DispatchFromIdle(unboundReceipt, unboundFailure);
    // The unbound candidate leaves this processor's queue; its owner places
    // it again once the run loop binds it.
    if (unbound || (unboundFailure.Error != ProcessorKernelRunLoopError::ThreadNotBound) ||
        (g_thread.GetSnapshot().State != ThreadState::Ready) ||
        (Controller().GetSnapshot().State != Asteria::Kernel::Scheduler::ProcessorRunState::Idle) ||
        (Controller().GetSnapshot().Scheduler.Queue.ReadyEntries != 0U))
    {
        return 4;
    }
    if (RunLoop().Bind(g_secondThread, g_secondThreadContext, g_otherProtectionDomain) !=
            ProcessorKernelRunLoopError::None ||
        RunLoop().Bind(g_thread, g_threadContext, g_protectionDomain) != ProcessorKernelRunLoopError::None ||
        !Controller().Enqueue(g_schedulingContextId, g_schedulingContext, g_threadId, g_thread).HasValue())
    {
        return 5;
    }

    g_otherPageTable.RejectNextActivationBeforeCommit();
    ProcessorKernelRunLoopReceipt rejectedReceipt{};
    ProcessorKernelRunLoopFailure rejectedFailure{};
    const bool rejected = RunLoop().DispatchFromIdle(rejectedReceipt, rejectedFailure);
    ++g_nativeRootEvidence.EventSequence;
    g_nativeRootEvidence.RollbackEvent = g_nativeRootEvidence.EventSequence;
    if (rejected || !VerifyRejectedDestinationActivation(rejectedFailure))
    {
        return 6;
    }

    ProcessorKernelRunLoopReceipt firstIdleReceipt{};
    ProcessorKernelRunLoopFailure firstIdleFailure{};
    const bool firstIdle = RunLoop().DispatchFromIdle(firstIdleReceipt, firstIdleFailure);
    const auto blockedSnapshot = g_thread.GetSnapshot();
    const auto secondExitedSnapshot = g_secondThread.GetSnapshot();
    if (firstIdle || (firstIdleFailure.Error != ProcessorKernelRunLoopError::ControllerRejected) ||
        (firstIdleFailure.ControllerError != ProcessorRunError::NoRunnableThread) || (g_phase != 1) ||
        (g_crossThreadPhase != 2) || (g_threadEntryCalls != 1U) || (g_secondThreadEntryCalls != 1U) ||
        (g_failure != 0) || (blockedSnapshot.State != ThreadState::Blocked) || !blockedSnapshot.BlockToken.IsValid() ||
        (FaultIngress().GetCalls() != 1U) || !FaultIngress().SawBlocked() ||
        !FaultIngress().GetToken().IsEqualTo(blockedSnapshot.BlockToken) ||
        (secondExitedSnapshot.State != ThreadState::Exited) || (secondExitedSnapshot.ExitCode != 0xB57E'34U) ||
        (RunLoop().GetSnapshot().State != ProcessorKernelRunLoopState::Idle) || (g_timer.GetDeadline() != 0U) ||
        !VerifySuccessfulRootTransitions())
    {
        return 7;
    }

    const ThreadBlockToken blockToken = blockedSnapshot.BlockToken;
    if ((g_thread.Wake(blockToken) != ThreadError::None) ||
        (RunLoop().Unbind(g_secondThread) != ProcessorKernelRunLoopError::None) ||
        !Controller().Enqueue(g_schedulingContextId, g_schedulingContext, g_threadId, g_thread).HasValue())
    {
        return 8;
    }
    g_timer.SetTicks(11U);

    ProcessorKernelRunLoopReceipt secondIdleReceipt{};
    ProcessorKernelRunLoopFailure secondIdleFailure{};
    const bool secondIdle = RunLoop().DispatchFromIdle(secondIdleReceipt, secondIdleFailure);
    const auto exitedSnapshot = g_thread.GetSnapshot();
    if (secondIdle || (secondIdleFailure.Error != ProcessorKernelRunLoopError::ControllerRejected) ||
        (secondIdleFailure.ControllerError != ProcessorRunError::NoRunnableThread) || (g_phase != 2) ||
        (g_failure != 0) || (exitedSnapshot.State != ThreadState::Exited) || (exitedSnapshot.ExitCode != 0xA57E'12U) ||
        (RunLoop().GetSnapshot().State != ProcessorKernelRunLoopState::Idle) || (g_timer.GetDeadline() != 0U))
    {
        return 9;
    }

    return 0;
}

[[nodiscard]] int RunNativeFlow() noexcept
{
    int result{1};
    {
        // These policy objects cache root storage. Their scope ends before
        // RetireObjects detaches, quiesces, or deregisters either root.
        ProcessorRunController controller{g_registry, g_timerQueue, g_timer, CpuId{0U}};
        ProcessorAddressSpaceController addressSpaces;
        if (!addressSpaces.Initialize(CpuId{0U}, g_addressSpace).HasValue())
        {
            return 1;
        }
        Amd64ProcessorRunLoop runLoop{controller, addressSpaces, g_idleContext};
        FaultIngressRecorder faultIngress{g_thread, g_schedulingContext, controller};
        g_controller = &controller;
        g_addressSpaces = &addressSpaces;
        g_runLoop = &runLoop;
        g_faultIngress = &faultIngress;

        result = ExecuteNativeFlow();
        if ((result == 0) && (runLoop.Unbind(g_thread) != ProcessorKernelRunLoopError::None))
        {
            result = 10;
        }

        g_faultIngress = nullptr;
        g_runLoop = nullptr;
        g_addressSpaces = nullptr;
        g_controller = nullptr;
    }
    return result;
}
} // namespace

int main()
{
    TestContext runner{"Amd64ProcessorRunLoopNativeTests"};
    ObjectId threadObject{};
    ObjectId contextObject{};
    ObjectId secondThreadObject{};
    ObjectId secondContextObject{};
    const bool initialized =
        ASTERIA_EXPECT(runner, "native run-loop objects register",
                       InitializeObjects(threadObject, contextObject, secondThreadObject, secondContextObject));
    // RunNativeFlow returns zero or the first failed native-flow stage.
    if (initialized && ASTERIA_CHECK_EQ(runner, RunNativeFlow(), 0))
    {
        ASTERIA_EXPECT(runner, "native run-loop objects retire after the flow",
                       RetireObjects(threadObject, contextObject, secondThreadObject, secondContextObject));
    }
    return runner.Finish();
}
