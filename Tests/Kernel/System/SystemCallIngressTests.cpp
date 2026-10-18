// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include <cstdio>

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
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::IUserMemoryAccess;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::ProcessorAddressSpaceController;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::IKernelObject;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
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
using Kernel::Scheduler::ProcessorKernelRunLoop;
using Kernel::Scheduler::ProcessorKernelRunLoopError;
using Kernel::Scheduler::ProcessorKernelRunLoopFailure;
using Kernel::Scheduler::ProcessorKernelRunLoopReceipt;
using Kernel::Scheduler::ProcessorRunController;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallIngress;
using Kernel::System::SystemCallIngressError;
using Kernel::System::SystemCallIngressFailure;
using Kernel::System::IpcSystemCallDispatchError;
using Kernel::System::SystemCallNumber;
using Kernel::System::SystemCallRequest;
using Kernel::System::SystemCallResponse;
using Kernel::System::SystemCallScratch;
using Kernel::Time::TimerQueue;
using Platform::Synchronization::SpinLock;

[[nodiscard]] constexpr bool HasCanonicalReservedFailureFields(const SystemCallIngressFailure& failure) noexcept
{
    return (failure.Ipc.Error == IpcSystemCallDispatchError::None) &&
           (failure.Ipc.Endpoint == Kernel::Ipc::EndpointErrorCode::None) &&
           (failure.Ipc.UserMemory == Kernel::Memory::UserMemoryAccessError::None) && (failure.Ipc.Reserved == 0U) &&
           (failure.Reserved == 0U);
}

[[nodiscard]] constexpr SystemCallIngressFailure IngressFailureSentinel() noexcept
{
    return SystemCallIngressFailure{
        .Error = SystemCallIngressError::CapabilitySpaceIdentifierMismatch,
        .ProcessorRunLoop = ProcessorKernelRunLoopError::NativeSwitchRejected,
        .ProtectionDomain = ProtectionDomainError::InUse,
        .Ipc =
            {
                .Error = IpcSystemCallDispatchError::InvalidContext,
                .Endpoint = Kernel::Ipc::EndpointErrorCode::QueueCorrupt,
                .UserMemory = Kernel::Memory::UserMemoryAccessError::Fault,
                .Reserved = 0xA5U,
            },
        .Reserved = 0x5AU,
    };
}

[[nodiscard]] constexpr bool FailureMatchesSentinel(const SystemCallIngressFailure& failure) noexcept
{
    const SystemCallIngressFailure expected = IngressFailureSentinel();
    return (failure.Error == expected.Error) && (failure.ProcessorRunLoop == expected.ProcessorRunLoop) &&
           (failure.ProtectionDomain == expected.ProtectionDomain) && (failure.Ipc.Error == expected.Ipc.Error) &&
           (failure.Ipc.Endpoint == expected.Ipc.Endpoint) && (failure.Ipc.UserMemory == expected.Ipc.UserMemory) &&
           (failure.Ipc.Reserved == expected.Ipc.Reserved) && (failure.Reserved == expected.Reserved);
}

[[nodiscard]] constexpr bool IsUnavailableIpcResponse(const SystemCallResponse& response) noexcept
{
    return (response.Error == SystemCallError::InvalidConfiguration) && (response.Value0 == 0U) &&
           (response.Value1 == 0U);
}

class ManualLocalTimer final : public ILocalTimer
{
public:
    [[nodiscard]] UInt64 GetTicks() const noexcept override
    {
        return 10U;
    }

    [[nodiscard]] LocalTimerError ProgramDeadline(const UInt64 deadlineTicks) noexcept override
    {
        static_cast<void>(deadlineTicks);
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError CancelDeadline() noexcept override
    {
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError AcknowledgeExternallyMaskedDeadline(const UInt64 deadlineTicks) noexcept override
    {
        static_cast<void>(deadlineTicks);
        return LocalTimerError::InvalidDeadline;
    }
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
        return ((m_owner == &owner) && processor.IsValid()) ? AddressSpacePageTableActivationError::None
                                                            : AddressSpacePageTableActivationError::InvalidOwner;
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
    constexpr UInt64 Base{0xFFFF'8000'0000'0000ULL};
    return (plan.AddMapping(MakeSeedMapping(Base + 0x1000U, 0x0010'0000U, 2U, ReadExecute(),
                                            InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(Base + 0x4000U, 0x0010'4000U, 4U, ReadWrite(),
                                            InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeedMapping(Base + 0x8000U, 0x0010'8000U, 16U, ReadWrite(),
                                            InitialMappingOwner::PageTable)) == InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

enum class FakeContextError : Foundation::UInt8
{
    None,
    Rejected,
};

class FakeKernelContext final
{
public:
    using EntryHandler = void (*)(void* argument) noexcept;

    [[nodiscard]] constexpr bool IsPrepared() const noexcept
    {
        return true;
    }

    void SetEntry(const EntryHandler entry, void* const argument) noexcept
    {
        m_entry = entry;
        m_argument = argument;
    }

    void SetSwitchError(const FakeContextError error) noexcept
    {
        m_switchError = error;
    }

    [[nodiscard]] constexpr FakeContextError SwitchTo(FakeKernelContext& next) noexcept
    {
        if (next.m_entry != nullptr)
        {
            next.m_entry(next.m_argument);
        }
        return m_switchError;
    }

    [[nodiscard]] constexpr bool SaveExternalFrameIdleContinuation(const Foundation::UInt64) noexcept
    {
        return false;
    }

    [[nodiscard]] constexpr Foundation::UInt64 GetExternalFrameIdleContinuationGeneration() const noexcept
    {
        return 0U;
    }

private:
    EntryHandler m_entry{nullptr};
    void* m_argument{nullptr};
    FakeContextError m_switchError{FakeContextError::None};
};

[[nodiscard]] constexpr ResourceDomainLimits ResourceLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 4096U,
        .ResidentMemoryPages = 4096U,
        .KernelMetadataBytes = 64U * 1024U,
        .CapabilityHandles = 4096U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 256U,
        .ChildDomains = 16U,
    };
}

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

[[nodiscard]] bool DestroyAndCollect(RootCapabilityIssuer& issuer, const ObjectId object,
                                     IKernelObject& expected) noexcept
{
    if (issuer.DestroyObject(object) != ObjectRegistryError::Ok)
    {
        return false;
    }
    const auto collected = issuer.CollectDestroyedObject();
    return collected.HasValue() && (collected.GetValue().Object.GetValue() == object.GetValue()) &&
           (collected.GetValue().Instance == &expected);
}

class KernelOnlyIngressProbe final
{
public:
    explicit KernelOnlyIngressProbe(ProcessorKernelRunLoop<FakeKernelContext>& runLoop) noexcept : m_runLoop{runLoop}
    {
    }

    void Run() noexcept
    {
        m_called = true;
        const auto active = m_runLoop.GetActiveProtectionDomainBinding();
        m_sawActiveDomain = active.HasValue();
        m_dispatched = SystemCallIngress::Dispatch(m_runLoop, m_request, m_response, m_failure);
        m_ipcDispatched = SystemCallIngress::Dispatch(m_runLoop, m_ipcRequest, m_ipcResponse, m_ipcFailure);
    }

    [[nodiscard]] bool Passed() const noexcept
    {
        return m_called && m_sawActiveDomain && !m_dispatched && (m_response.Error == SystemCallError::RightsDenied) &&
               (m_response.Value0 == 0x0123'4567'89AB'CDEFULL) && (m_response.Value1 == 0xFEDC'BA98'7654'3210ULL) &&
               (m_failure.Error == SystemCallIngressError::CapabilitySpaceMissing) &&
               (m_failure.ProcessorRunLoop == ProcessorKernelRunLoopError::None) &&
               (m_failure.ProtectionDomain == ProtectionDomainError::None) &&
               HasCanonicalReservedFailureFields(m_failure) && IpcRequestPassed();
    }

    [[nodiscard]] constexpr bool IpcRequestPassed() const noexcept
    {
        return !m_ipcDispatched && (m_ipcResponse.Error == SystemCallError::RightsDenied) &&
               (m_ipcResponse.Value0 == 0x0123'4567'89AB'CDEFULL) &&
               (m_ipcResponse.Value1 == 0xFEDC'BA98'7654'3210ULL) &&
               (m_ipcFailure.Error == SystemCallIngressError::CapabilitySpaceMissing) &&
               (m_ipcFailure.ProcessorRunLoop == ProcessorKernelRunLoopError::None) &&
               (m_ipcFailure.ProtectionDomain == ProtectionDomainError::None) &&
               HasCanonicalReservedFailureFields(m_ipcFailure);
    }

    [[nodiscard]] constexpr bool WasCalled() const noexcept
    {
        return m_called;
    }

    [[nodiscard]] constexpr bool SawActiveDomain() const noexcept
    {
        return m_sawActiveDomain;
    }

    [[nodiscard]] constexpr bool WasDispatched() const noexcept
    {
        return m_dispatched;
    }

    [[nodiscard]] constexpr const SystemCallIngressFailure& GetFailure() const noexcept
    {
        return m_failure;
    }

private:
    ProcessorKernelRunLoop<FakeKernelContext>& m_runLoop;
    SystemCallRequest m_request{
        .Number = 0xFFFF'FFFF'FFFF'FFFFULL,
        .Arguments = {},
    };
    SystemCallResponse m_response{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
    SystemCallIngressFailure m_failure{
        .Error = SystemCallIngressError::CapabilitySpaceIdentifierMismatch,
        .ProcessorRunLoop = ProcessorKernelRunLoopError::NativeSwitchRejected,
        .ProtectionDomain = ProtectionDomainError::InUse,
        .Ipc =
            {
                .Error = IpcSystemCallDispatchError::InvalidContext,
                .Endpoint = Kernel::Ipc::EndpointErrorCode::QueueCorrupt,
                .UserMemory = Kernel::Memory::UserMemoryAccessError::Fault,
                .Reserved = 1U,
            },
        .Reserved = 1U,
    };
    SystemCallRequest m_ipcRequest{
        .Number = static_cast<UInt64>(SystemCallNumber::IpcSend),
        .Arguments =
            {
                0x1111'2222'3333'4444ULL,
                0x5555'6666'7777'8888ULL,
                0x9999'AAAA'BBBB'CCCCULL,
                0xDDDD'EEEE'FFFF'0000ULL,
                0x0123'4567'89AB'CDEFULL,
                0xFEDC'BA98'7654'3210ULL,
            },
    };
    SystemCallResponse m_ipcResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
    SystemCallIngressFailure m_ipcFailure{IngressFailureSentinel()};
    bool m_called{false};
    bool m_sawActiveDomain{false};
    bool m_dispatched{false};
    bool m_ipcDispatched{false};
};

class UnreadyUserMemoryAccess final : public IUserMemoryAccess
{
public:
    constexpr UnreadyUserMemoryAccess() noexcept = default;

    [[nodiscard]] Kernel::Memory::AddressSpaceId GetAddressSpaceId() const noexcept override
    {
        return {};
    }

    [[nodiscard]] bool IsReady() const noexcept override
    {
        return false;
    }

private:
    [[nodiscard]] Kernel::Memory::UserMemoryAccessError
    CopyFromUserValidated(Kernel::Memory::UserMemoryRange, Kernel::Memory::WritableKernelBuffer) noexcept override
    {
        return Kernel::Memory::UserMemoryAccessError::NotReady;
    }

    [[nodiscard]] Foundation::Result<Kernel::Memory::UserMemoryWriteReservation, Kernel::Memory::UserMemoryAccessError>
    PrepareCopyToUserValidated(Kernel::Memory::UserMemoryRange) noexcept override
    {
        return Foundation::Result<Kernel::Memory::UserMemoryWriteReservation, Kernel::Memory::UserMemoryAccessError>::
            Failure(Kernel::Memory::UserMemoryAccessError::NotReady);
    }

    [[nodiscard]] Kernel::Memory::UserMemoryAccessError
    CancelCopyToUserValidated(Kernel::Memory::UserMemoryWriteReservation) noexcept override
    {
        return Kernel::Memory::UserMemoryAccessError::NotReady;
    }

    [[nodiscard]] Kernel::Memory::UserMemoryAccessError
    CommitCopyToUserValidated(Kernel::Memory::UserMemoryWriteReservation,
                              Kernel::Memory::ReadOnlyKernelBuffer) noexcept override
    {
        return Kernel::Memory::UserMemoryAccessError::NotReady;
    }
};

class UserCapableIpcIngressProbe final
{
public:
    UserCapableIpcIngressProbe(ProcessorKernelRunLoop<FakeKernelContext>& runLoop,
                               CapabilitySpace& capabilities) noexcept
        : m_runLoop{runLoop}, m_capabilities{capabilities}
    {
    }

    void Run() noexcept
    {
        m_called = true;
        const auto active = m_runLoop.GetActiveProtectionDomainBinding();
        m_sawActiveDomain = active.HasValue();

        m_queryDispatched = SystemCallIngress::Dispatch(m_runLoop, m_unreadyUserMemory, m_scratch, m_queryRequest,
                                                        m_queryResponse, m_queryFailure);

        const Size beforeSend = m_capabilities.GetActiveCount();
        m_sendDispatched = SystemCallIngress::Dispatch(m_runLoop, m_sendRequest, m_sendResponse, m_sendFailure);
        m_sendLeftCapabilitiesUnchanged = m_capabilities.GetActiveCount() == beforeSend;

        const Size beforeReceive = m_capabilities.GetActiveCount();
        m_receiveDispatched =
            SystemCallIngress::Dispatch(m_runLoop, m_receiveRequest, m_receiveResponse, m_receiveFailure);
        m_receiveLeftCapabilitiesUnchanged = m_capabilities.GetActiveCount() == beforeReceive;

        const Size beforeWait = m_capabilities.GetActiveCount();
        m_waitDispatched = SystemCallIngress::Dispatch(m_runLoop, m_waitRequest, m_waitResponse, m_waitFailure);
        m_waitLeftCapabilitiesUnchanged = m_capabilities.GetActiveCount() == beforeWait;
    }

    [[nodiscard]] bool Passed() const noexcept
    {
        return m_called && m_sawActiveDomain && m_queryDispatched &&
               (m_queryResponse.Error == SystemCallError::InvalidHandle) && (m_queryResponse.Value0 == 0U) &&
               (m_queryResponse.Value1 == 0U) && FailureMatchesSentinel(m_queryFailure) &&
               !m_unreadyUserMemory.IsReady() && m_sendDispatched && IsUnavailableIpcResponse(m_sendResponse) &&
               FailureMatchesSentinel(m_sendFailure) && m_sendLeftCapabilitiesUnchanged && m_receiveDispatched &&
               IsUnavailableIpcResponse(m_receiveResponse) && FailureMatchesSentinel(m_receiveFailure) &&
               m_receiveLeftCapabilitiesUnchanged && WaitPassed();
    }

    [[nodiscard]] constexpr bool SendPassed() const noexcept
    {
        return m_sendDispatched && IsUnavailableIpcResponse(m_sendResponse) && FailureMatchesSentinel(m_sendFailure) &&
               m_sendLeftCapabilitiesUnchanged;
    }

    [[nodiscard]] bool QueryPassed() const noexcept
    {
        return m_queryDispatched && (m_queryResponse.Error == SystemCallError::InvalidHandle) &&
               (m_queryResponse.Value0 == 0U) && (m_queryResponse.Value1 == 0U) &&
               FailureMatchesSentinel(m_queryFailure) && !m_unreadyUserMemory.IsReady();
    }

    [[nodiscard]] constexpr bool ReceivePassed() const noexcept
    {
        return m_receiveDispatched && IsUnavailableIpcResponse(m_receiveResponse) &&
               FailureMatchesSentinel(m_receiveFailure) && m_receiveLeftCapabilitiesUnchanged;
    }

    [[nodiscard]] constexpr bool WaitPassed() const noexcept
    {
        return m_waitDispatched && IsUnavailableIpcResponse(m_waitResponse) && FailureMatchesSentinel(m_waitFailure) &&
               m_waitLeftCapabilitiesUnchanged;
    }

private:
    ProcessorKernelRunLoop<FakeKernelContext>& m_runLoop;
    CapabilitySpace& m_capabilities;
    UnreadyUserMemoryAccess m_unreadyUserMemory;
    SystemCallScratch m_scratch;
    SystemCallRequest m_queryRequest{
        .Number = static_cast<UInt64>(SystemCallNumber::CapabilityQuery),
        .Arguments = {},
    };
    SystemCallRequest m_sendRequest{
        .Number = static_cast<UInt64>(SystemCallNumber::IpcSend),
        .Arguments =
            {
                0x1111'2222'3333'4444ULL,
                0x5555'6666'7777'8888ULL,
                0x9999'AAAA'BBBB'CCCCULL,
                0xDDDD'EEEE'FFFF'0000ULL,
                0x0123'4567'89AB'CDEFULL,
                0xFEDC'BA98'7654'3210ULL,
            },
    };
    SystemCallRequest m_receiveRequest{
        .Number = static_cast<UInt64>(SystemCallNumber::IpcReceive),
        .Arguments =
            {
                0xFEDC'BA98'7654'3210ULL,
                0x0123'4567'89AB'CDEFULL,
                0xDDDD'EEEE'FFFF'0000ULL,
                0x9999'AAAA'BBBB'CCCCULL,
                0x5555'6666'7777'8888ULL,
                0x1111'2222'3333'4444ULL,
            },
    };
    SystemCallRequest m_waitRequest{
        .Number = static_cast<UInt64>(SystemCallNumber::WaitSetWait),
        .Arguments =
            {
                0xAAAA'BBBB'CCCC'DDDDULL,
                0x1111'2222'3333'4444ULL,
                0U,
                0U,
                0U,
                0U,
            },
    };
    SystemCallResponse m_sendResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
    SystemCallResponse m_queryResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
    SystemCallResponse m_receiveResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0xFEDC'BA98'7654'3210ULL,
        .Value1 = 0x0123'4567'89AB'CDEFULL,
    };
    SystemCallResponse m_waitResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
    SystemCallIngressFailure m_sendFailure{IngressFailureSentinel()};
    SystemCallIngressFailure m_queryFailure{IngressFailureSentinel()};
    SystemCallIngressFailure m_receiveFailure{IngressFailureSentinel()};
    SystemCallIngressFailure m_waitFailure{IngressFailureSentinel()};
    bool m_called{false};
    bool m_sawActiveDomain{false};
    bool m_sendDispatched{false};
    bool m_queryDispatched{false};
    bool m_receiveDispatched{false};
    bool m_waitDispatched{false};
    bool m_sendLeftCapabilitiesUnchanged{false};
    bool m_receiveLeftCapabilitiesUnchanged{false};
    bool m_waitLeftCapabilitiesUnchanged{false};
};

void RunKernelOnlyIngressProbe(void* const argument) noexcept
{
    if (argument != nullptr)
    {
        static_cast<KernelOnlyIngressProbe*>(argument)->Run();
    }
}

void RunUserCapableIpcIngressProbe(void* const argument) noexcept
{
    if (argument != nullptr)
    {
        static_cast<UserCapableIpcIngressProbe*>(argument)->Run();
    }
}

void TestIdleRejectionIsAtomic(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    TestAddressSpacePageTable pageTable;
    InitialMappingPlan seedPlan;
    AddressSpace idleAddressSpace;
    const bool planReady = BuildSeedPlan(seedPlan);
    const bool addressInitialized = planReady && (idleAddressSpace.Initialize(ResourceDomainId{7U}, pageTable,
                                                                              seedPlan) == AddressSpaceError::None);
    const auto addressObject =
        addressInitialized ? issuer.CreateObject(idleAddressSpace)
                           : Foundation::Result<Kernel::Object::ObjectId, Kernel::Object::ObjectRegistryError>::Failure(
                                 Kernel::Object::ObjectRegistryError::InvalidObject);
    const AddressSpaceId addressIdentity =
        addressObject.HasValue() ? AddressSpaceId{registry, addressObject.GetValue()} : AddressSpaceId{};
    const bool addressReady =
        addressObject.HasValue() &&
        (idleAddressSpace.BindIdentity(registry, addressIdentity).Error == AddressSpaceError::None);
    ProcessorAddressSpaceController addressSpaces;
    const auto addressController = addressReady
                                       ? addressSpaces.Initialize(CpuId{0U}, idleAddressSpace)
                                       : Foundation::Result<Kernel::Memory::ProcessorAddressSpaceControllerReceipt,
                                                            Kernel::Memory::ProcessorAddressSpaceControllerFailure>::
                                             Failure(Kernel::Memory::ProcessorAddressSpaceControllerFailure{});
    FakeKernelContext idle;
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    const SystemCallRequest request{
        .Number = 0xFFFF'FFFF'FFFF'FFFFULL,
        .Arguments = {},
    };
    SystemCallResponse response{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
    SystemCallIngressFailure failure{
        .Error = SystemCallIngressError::CapabilitySpaceIdentifierMismatch,
        .ProcessorRunLoop = ProcessorKernelRunLoopError::NativeSwitchRejected,
        .ProtectionDomain = ProtectionDomainError::InUse,
        .Ipc =
            {
                .Error = IpcSystemCallDispatchError::InvalidContext,
                .Endpoint = Kernel::Ipc::EndpointErrorCode::QueueCorrupt,
                .UserMemory = Kernel::Memory::UserMemoryAccessError::Fault,
                .Reserved = 1U,
            },
        .Reserved = 1U,
    };

    const bool dispatched = SystemCallIngress::Dispatch(runLoop, request, response, failure);
    ASTERIA_EXPECT(runner, "idle rejection leaves the syscall response byte-for-byte unchanged",
                   addressController.HasValue(), !dispatched, response.Error == SystemCallError::RightsDenied,
                   response.Value0 == 0x0123'4567'89AB'CDEFULL, response.Value1 == 0xFEDC'BA98'7654'3210ULL);
    ASTERIA_EXPECT(runner, "idle rejection reports canonical run-loop error provenance",
                   failure.Error == SystemCallIngressError::ProcessorRunLoopRejected,
                   failure.ProcessorRunLoop == ProcessorKernelRunLoopError::NotActive,
                   failure.ProtectionDomain == ProtectionDomainError::None, HasCanonicalReservedFailureFields(failure));

    const bool cleaned = addressController.HasValue() &&
                         (idleAddressSpace.DetachProcessor(CpuId{0U}) == AddressSpaceError::None) &&
                         (idleAddressSpace.Quiesce() == AddressSpaceError::None) && addressObject.HasValue() &&
                         (issuer.DestroyObject(addressObject.GetValue()) == Kernel::Object::ObjectRegistryError::Ok) &&
                         issuer.CollectDestroyedObject().HasValue();
    ASTERIA_EXPECT(runner, "the idle activation fixture drains exactly", cleaned);
}

void TestKernelOnlyDomainRejectionIsAtomic(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    TestAddressSpacePageTable pageTable;
    InitialMappingPlan seedPlan;
    AddressSpace addressSpace;
    ProtectionDomain protectionDomain;
    Thread thread;
    SchedulingContext schedulingContext{registry};

    const bool resourceInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{9U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resourceObject =
        resourceInitialized
            ? issuer.CreateObject(resourceDomain)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const ResourceDomainObjectId resourceIdentity = resourceObject.HasValue()
                                                        ? ResourceDomainObjectId{registry, resourceObject.GetValue()}
                                                        : ResourceDomainObjectId{};
    const bool planReady = BuildSeedPlan(seedPlan);
    const bool addressInitialized =
        planReady && (addressSpace.Initialize(ResourceDomainId{7U}, pageTable, seedPlan) == AddressSpaceError::None);
    const auto addressObject =
        addressInitialized
            ? issuer.CreateObject(addressSpace)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const AddressSpaceId addressIdentity =
        addressObject.HasValue() ? AddressSpaceId{registry, addressObject.GetValue()} : AddressSpaceId{};
    const bool addressReady = addressObject.HasValue() &&
                              (addressSpace.BindIdentity(registry, addressIdentity).Error == AddressSpaceError::None);
    const bool protectionInitialized =
        addressReady && resourceObject.HasValue() &&
        (protectionDomain.InitializeKernelOnly(registry, resourceIdentity, resourceDomain, addressIdentity).Error ==
         ProtectionDomainError::None);
    const auto protectionObject =
        protectionInitialized
            ? issuer.CreateObject(protectionDomain)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const ProtectionDomainId protectionIdentity =
        protectionObject.HasValue() ? ProtectionDomainId{registry, protectionObject.GetValue()} : ProtectionDomainId{};
    const bool protectionReady =
        protectionObject.HasValue() &&
        (protectionDomain.BindIdentity(registry, protectionIdentity).Error == ProtectionDomainError::None);

    ProcessorAddressSpaceController addressSpaces;
    const auto addressController = protectionReady
                                       ? addressSpaces.Initialize(CpuId{0U}, addressSpace)
                                       : Foundation::Result<Kernel::Memory::ProcessorAddressSpaceControllerReceipt,
                                                            Kernel::Memory::ProcessorAddressSpaceControllerFailure>::
                                             Failure(Kernel::Memory::ProcessorAddressSpaceControllerFailure{});
    FakeKernelContext idle;
    FakeKernelContext threadContext;
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    KernelOnlyIngressProbe probe{runLoop};
    threadContext.SetEntry(RunKernelOnlyIngressProbe, &probe);
    idle.SetSwitchError(FakeContextError::Rejected);

    const bool threadInitialized = thread.Initialize(ResourceDomainId{9U}) == ThreadError::None;
    const bool schedulingInitialized = schedulingContext.Initialize(Configuration(), ResourceDomainId{9U},
                                                                    MonotonicTime{0U}) == SchedulingContextError::None;
    const auto threadObject =
        threadInitialized
            ? issuer.CreateObject(thread)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const auto schedulingObject =
        schedulingInitialized
            ? issuer.CreateObject(schedulingContext)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const ThreadId threadIdentity = threadObject.HasValue() ? ThreadId{registry, threadObject.GetValue()} : ThreadId{};
    const SchedulingContextId schedulingIdentity = schedulingObject.HasValue()
                                                       ? SchedulingContextId{registry, schedulingObject.GetValue()}
                                                       : SchedulingContextId{};
    const bool pairReady = threadObject.HasValue() && schedulingObject.HasValue() &&
                           (schedulingContext.Bind(threadIdentity) == SchedulingContextError::None) &&
                           (thread.MakeReady() == ThreadError::None);
    const bool runLoopBound =
        pairReady && addressController.HasValue() &&
        (runLoop.Bind(thread, threadContext, protectionDomain) == ProcessorKernelRunLoopError::None);
    const auto enqueued =
        runLoopBound
            ? controller.Enqueue(schedulingIdentity, schedulingContext, threadIdentity, thread)
            : Foundation::Result<Kernel::Scheduler::ReadyQueueEnqueueReceipt, Kernel::Scheduler::ProcessorRunFailure>::
                  Failure(Kernel::Scheduler::ProcessorRunFailure{});

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatched = enqueued.HasValue() && runLoop.DispatchFromIdle(receipt, failure);
    ASTERIA_EXPECT(runner, "the kernel-only ingress fixture activates registered dependencies", protectionReady,
                   addressController.HasValue(), pairReady, runLoopBound, enqueued.HasValue());
    ASTERIA_EXPECT(runner, "an active kernel-only domain cannot reach user syscall dispatch", !dispatched,
                   probe.Passed());
    ASTERIA_EXPECT(runner, "the active ingress probe observes the fail-closed capability result", probe.WasCalled(),
                   probe.SawActiveDomain(), !probe.WasDispatched(),
                   probe.GetFailure().Error == SystemCallIngressError::CapabilitySpaceMissing);
    ASTERIA_EXPECT(runner, "IpcSend cannot bypass active capability-authority resolution", probe.IpcRequestPassed());
    ASTERIA_EXPECT(runner, "the rejected native test switch restores the idle run loop", !dispatched,
                   enqueued.HasValue(), failure.Error == ProcessorKernelRunLoopError::NativeSwitchRejected);

    bool cleaned = controller.Remove(threadIdentity).HasValue();
    cleaned = (runLoop.Unbind(thread) == ProcessorKernelRunLoopError::None) && cleaned;
    cleaned = (schedulingContext.Unbind(threadIdentity) == SchedulingContextError::None) && cleaned;
    cleaned = (addressSpace.DetachProcessor(CpuId{0U}) == AddressSpaceError::None) && cleaned;
    cleaned = (protectionDomain.Quiesce() == ProtectionDomainError::None) && cleaned;
    cleaned = (addressSpace.Quiesce() == AddressSpaceError::None) && cleaned;
    cleaned = (resourceDomain.Quiesce() == ResourceDomainError::None) && cleaned;
    cleaned = protectionObject.HasValue() && DestroyAndCollect(issuer, protectionObject.GetValue(), protectionDomain) &&
              cleaned;
    cleaned = addressObject.HasValue() && DestroyAndCollect(issuer, addressObject.GetValue(), addressSpace) && cleaned;
    cleaned =
        resourceObject.HasValue() && DestroyAndCollect(issuer, resourceObject.GetValue(), resourceDomain) && cleaned;
    cleaned = schedulingObject.HasValue() &&
              DestroyAndCollect(issuer, schedulingObject.GetValue(), schedulingContext) && cleaned;
    cleaned = threadObject.HasValue() && DestroyAndCollect(issuer, threadObject.GetValue(), thread) && cleaned;
    ASTERIA_EXPECT(runner, "the kernel-only ingress fixture drains exactly", cleaned);
}

void TestUserCapableIpcWithoutCollaboratorsCompletes(TestContext& runner) noexcept
{
    SpinLock registryLock;
    Asteria::TestSupport::TestObjectRegistry registry{registryLock};
    RevocationRegistry revocations{registryLock};
    RootCapabilityIssuer issuer{registry};
    ManualLocalTimer timer;
    CapabilitySpace capabilities{registry, revocations, timer, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    TimerQueue timerQueue{registry, timer};
    ProcessorRunController controller{registry, timerQueue, timer, CpuId{0U}};
    ResourceDomainLedger resourceLedger;
    ResourceDomain resourceDomain{resourceLedger};
    TestAddressSpacePageTable pageTable;
    InitialMappingPlan seedPlan;
    AddressSpace addressSpace;
    ProtectionDomain protectionDomain;
    Thread thread;
    SchedulingContext schedulingContext{registry};

    const bool resourceInitialized =
        resourceDomain.InitializeRoot(ResourceDomainId{19U}, ResourceLimits()).Error == ResourceDomainError::None;
    const auto resourceObject =
        resourceInitialized
            ? issuer.CreateObject(resourceDomain)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const ResourceDomainObjectId resourceIdentity = resourceObject.HasValue()
                                                        ? ResourceDomainObjectId{registry, resourceObject.GetValue()}
                                                        : ResourceDomainObjectId{};
    const bool planReady = BuildSeedPlan(seedPlan);
    const bool addressInitialized =
        planReady && (addressSpace.Initialize(ResourceDomainId{19U}, pageTable, seedPlan) == AddressSpaceError::None);
    const auto addressObject =
        addressInitialized
            ? issuer.CreateObject(addressSpace)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const AddressSpaceId addressIdentity =
        addressObject.HasValue() ? AddressSpaceId{registry, addressObject.GetValue()} : AddressSpaceId{};
    const bool addressReady = addressObject.HasValue() &&
                              (addressSpace.BindIdentity(registry, addressIdentity).Error == AddressSpaceError::None);
    const bool protectionInitialized =
        addressReady && resourceObject.HasValue() && capabilities.IsReady() &&
        (protectionDomain.Initialize(registry, resourceIdentity, resourceDomain, addressIdentity, capabilities).Error ==
         ProtectionDomainError::None);
    const auto protectionObject =
        protectionInitialized
            ? issuer.CreateObject(protectionDomain)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const ProtectionDomainId protectionIdentity =
        protectionObject.HasValue() ? ProtectionDomainId{registry, protectionObject.GetValue()} : ProtectionDomainId{};
    const bool protectionReady =
        protectionObject.HasValue() &&
        (protectionDomain.BindIdentity(registry, protectionIdentity).Error == ProtectionDomainError::None);

    ProcessorAddressSpaceController addressSpaces;
    const auto addressController = protectionReady
                                       ? addressSpaces.Initialize(CpuId{0U}, addressSpace)
                                       : Foundation::Result<Kernel::Memory::ProcessorAddressSpaceControllerReceipt,
                                                            Kernel::Memory::ProcessorAddressSpaceControllerFailure>::
                                             Failure(Kernel::Memory::ProcessorAddressSpaceControllerFailure{});
    FakeKernelContext idle;
    FakeKernelContext threadContext;
    ProcessorKernelRunLoop<FakeKernelContext> runLoop{controller, addressSpaces, idle};
    UserCapableIpcIngressProbe probe{runLoop, capabilities};
    threadContext.SetEntry(RunUserCapableIpcIngressProbe, &probe);
    idle.SetSwitchError(FakeContextError::Rejected);

    const bool threadInitialized = thread.Initialize(ResourceDomainId{19U}) == ThreadError::None;
    const bool schedulingInitialized = schedulingContext.Initialize(Configuration(), ResourceDomainId{19U},
                                                                    MonotonicTime{0U}) == SchedulingContextError::None;
    const auto threadObject =
        threadInitialized
            ? issuer.CreateObject(thread)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const auto schedulingObject =
        schedulingInitialized
            ? issuer.CreateObject(schedulingContext)
            : Foundation::Result<ObjectId, ObjectRegistryError>::Failure(ObjectRegistryError::InvalidObject);
    const ThreadId threadIdentity = threadObject.HasValue() ? ThreadId{registry, threadObject.GetValue()} : ThreadId{};
    const SchedulingContextId schedulingIdentity = schedulingObject.HasValue()
                                                       ? SchedulingContextId{registry, schedulingObject.GetValue()}
                                                       : SchedulingContextId{};
    const bool pairReady = threadObject.HasValue() && schedulingObject.HasValue() &&
                           (schedulingContext.Bind(threadIdentity) == SchedulingContextError::None) &&
                           (thread.MakeReady() == ThreadError::None);
    const bool runLoopBound =
        pairReady && addressController.HasValue() &&
        (runLoop.Bind(thread, threadContext, protectionDomain) == ProcessorKernelRunLoopError::None);
    const auto enqueued =
        runLoopBound
            ? controller.Enqueue(schedulingIdentity, schedulingContext, threadIdentity, thread)
            : Foundation::Result<Kernel::Scheduler::ReadyQueueEnqueueReceipt, Kernel::Scheduler::ProcessorRunFailure>::
                  Failure(Kernel::Scheduler::ProcessorRunFailure{});

    ProcessorKernelRunLoopReceipt receipt{};
    ProcessorKernelRunLoopFailure failure{};
    const bool dispatched = enqueued.HasValue() && runLoop.DispatchFromIdle(receipt, failure);
    ASTERIA_EXPECT(runner, "the user-capable IPC ingress fixture activates exact authority", protectionReady,
                   addressController.HasValue(), pairReady, runLoopBound, enqueued.HasValue());
    ASTERIA_EXPECT(runner, "defined IPC numbers complete normally without collaborators", !dispatched, probe.Passed());
    ASTERIA_EXPECT(runner, "unready user memory does not block a non-IPC CapabilityQuery", probe.QueryPassed());
    ASTERIA_EXPECT(runner, "IpcSend returns canonical unavailable response and preserves failure", probe.SendPassed());
    ASTERIA_EXPECT(runner, "IpcReceive returns canonical unavailable response and preserves failure",
                   probe.ReceivePassed());
    ASTERIA_EXPECT(runner, "WaitSetWait returns canonical unavailable response without resumable collaborators",
                   probe.WaitPassed());
    ASTERIA_EXPECT(runner, "unavailable IPC does not accidentally dispatch a capability syscall",
                   capabilities.GetActiveCount() == 0U);
    ASTERIA_EXPECT(runner, "the user-capable probe restores the idle run loop", !dispatched, enqueued.HasValue(),
                   failure.Error == ProcessorKernelRunLoopError::NativeSwitchRejected);

    bool cleaned = controller.Remove(threadIdentity).HasValue();
    cleaned = (runLoop.Unbind(thread) == ProcessorKernelRunLoopError::None) && cleaned;
    cleaned = (schedulingContext.Unbind(threadIdentity) == SchedulingContextError::None) && cleaned;
    cleaned = (addressSpace.DetachProcessor(CpuId{0U}) == AddressSpaceError::None) && cleaned;
    cleaned = (protectionDomain.Quiesce() == ProtectionDomainError::None) && cleaned;
    cleaned = (addressSpace.Quiesce() == AddressSpaceError::None) && cleaned;
    cleaned = (resourceDomain.Quiesce() == ResourceDomainError::None) && cleaned;
    cleaned = protectionObject.HasValue() && DestroyAndCollect(issuer, protectionObject.GetValue(), protectionDomain) &&
              cleaned;
    cleaned = addressObject.HasValue() && DestroyAndCollect(issuer, addressObject.GetValue(), addressSpace) && cleaned;
    cleaned =
        resourceObject.HasValue() && DestroyAndCollect(issuer, resourceObject.GetValue(), resourceDomain) && cleaned;
    cleaned = schedulingObject.HasValue() &&
              DestroyAndCollect(issuer, schedulingObject.GetValue(), schedulingContext) && cleaned;
    cleaned = threadObject.HasValue() && DestroyAndCollect(issuer, threadObject.GetValue(), thread) && cleaned;
    ASTERIA_EXPECT(runner, "the user-capable IPC ingress fixture drains exactly", cleaned);
}
} // namespace Asteria::Test

int main()
{
    TestContext runner{"SystemCallIngressTests"};
    Asteria::Test::TestIdleRejectionIsAtomic(runner);
    Asteria::Test::TestKernelOnlyDomainRejectionIsAtomic(runner);
    Asteria::Test::TestUserCapableIpcWithoutCollaboratorsCompletes(runner);
    return runner.Finish();
}
