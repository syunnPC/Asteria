// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Ipc/Endpoint.hpp"
#include "Kernel/Ipc/IpcDeliveryCodec.hpp"
#include "Kernel/Memory/IUserMemoryAccess.hpp"
#include "Kernel/Object/ObjectRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomain.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/ProcessorKernelRunLoop.hpp"
#include "Kernel/System/SystemCallIngress.hpp"
#include "Kernel/System/SystemCallScratch.hpp"
#include "Kernel/Time/TimerQueue.hpp"
#include "Platform/Synchronization/SpinLock.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"
#include <cstddef>
#include <cstdio>

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt8;
using Foundation::UInt32;
using Foundation::UInt64;
using Hal::Memory::AddressSpacePageTableActivationError;
using Hal::Memory::AddressSpacePageTableError;
using Hal::Memory::AddressSpacePageTableMapResult;
using Hal::Memory::AddressSpacePageTableOwnershipError;
using Hal::Memory::IAddressSpacePageTable;
using Hal::Time::ILocalTimer;
using Hal::Time::LocalTimerError;
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointReceiveMetadata;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::IpcDeliveryCodec;
using Kernel::Ipc::IpcDeliveryCodecError;
using Kernel::Ipc::MessageCodec;
using Kernel::Ipc::MessageCodecError;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::AddressSpaceState;
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
using Kernel::Memory::ReadOnlyKernelBuffer;
using Kernel::Memory::UserMemoryAccessError;
using Kernel::Memory::UserMemoryRange;
using Kernel::Memory::UserMemoryWriteReservation;
using Kernel::Memory::VirtualAddress;
using Kernel::Memory::WritableKernelBuffer;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::IKernelObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainError;
using Kernel::Protection::ProtectionDomainId;
using Kernel::Protection::ProtectionDomainState;
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
using Kernel::Scheduler::ProcessorKernelRunLoopState;
using Kernel::Scheduler::ProcessorRunController;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::System::IpcSystemCallDispatchError;
using Kernel::System::SystemCallError;
using Kernel::System::SystemCallIngress;
using Kernel::System::SystemCallIngressError;
using Kernel::System::SystemCallIngressFailure;
using Kernel::System::SystemCallNumber;
using Kernel::System::SystemCallRequest;
using Kernel::System::SystemCallResponse;
using Kernel::System::SystemCallScratch;
using Kernel::Time::TimerQueue;
using Platform::Synchronization::SpinLock;

constexpr ResourceDomainId FirstResourceDomain{101U};
constexpr ResourceDomainId SecondResourceDomain{202U};
constexpr ResourceDomainId IdleResourceDomain{303U};
constexpr UInt64 FirstUserBase{0x0000'0000'1000'0000ULL};
constexpr UInt64 SecondUserBase{0x0000'0000'2000'0000ULL};
constexpr UInt64 FirstRequestFrameAddress{FirstUserBase + 0x100U};
constexpr UInt64 FirstReplyDeliveryAddress{FirstUserBase + 0x800U};
constexpr UInt64 SecondRequestDeliveryAddress{SecondUserBase + 0x100U};
constexpr UInt64 SecondReplyFrameAddress{SecondUserBase + 0x800U};
constexpr UInt64 RequestEndpointSelector{0xA57E'1000'0000'0001ULL};
constexpr UInt64 ReplyEndpointSelector{0xA57E'2000'0000'0002ULL};
constexpr UInt64 AttachmentSelector{0xA57E'3000'0000'0003ULL};
constexpr UInt64 TransactionIdentifier{0x1122'3344'5566'7788ULL};
constexpr UInt64 InterfaceLow{0x0123'4567'89AB'CDEFULL};
constexpr UInt64 InterfaceHigh{0xFEDC'BA98'7654'3210ULL};
constexpr UInt32 MethodIdentifier{0xA57E'0001U};
constexpr UInt64 RequestLeft{0x0123'4567'89AB'CDEFULL};
constexpr UInt64 RequestRight{0x1111'2222'3333'4444ULL};
constexpr UInt64 ReplyValue{RequestLeft + RequestRight};

[[nodiscard]] constexpr CapabilityRights RequestInvokeRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Inspect).With(CapabilityRight::Duplicate);
}

[[nodiscard]] constexpr CapabilityRights ReplyInvokeRights() noexcept
{
    return CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Read).With(CapabilityRight::Debug);
}

[[nodiscard]] constexpr CapabilityRights EndpointReadRights() noexcept
{
    return CapabilityRights{CapabilityRight::Read}.With(CapabilityRight::Inspect);
}

[[nodiscard]] constexpr CapabilityRights AttachmentRights() noexcept
{
    return CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Transfer);
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
        if (deadlineTicks <= 10U)
        {
            return LocalTimerError::DeadlineReached;
        }
        m_programmedDeadline = deadlineTicks;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError CancelDeadline() noexcept override
    {
        m_programmedDeadline = 0U;
        return LocalTimerError::None;
    }

    [[nodiscard]] LocalTimerError AcknowledgeExternallyMaskedDeadline(const UInt64 deadlineTicks) noexcept override
    {
        if ((deadlineTicks == 0U) || (deadlineTicks != m_programmedDeadline))
        {
            return LocalTimerError::InvalidDeadline;
        }
        m_programmedDeadline = 0U;
        return LocalTimerError::None;
    }

private:
    UInt64 m_programmedDeadline{0U};
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

enum class FakeContextError : UInt8
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

    [[nodiscard]] FakeContextError SwitchTo(FakeKernelContext& next) noexcept
    {
        if (next.m_entry != nullptr)
        {
            next.m_entry(next.m_argument);
        }
        return m_switchError;
    }

    [[nodiscard]] constexpr bool SaveExternalFrameIdleContinuation(const UInt64) noexcept
    {
        return false;
    }

    [[nodiscard]] constexpr UInt64 GetExternalFrameIdleContinuationGeneration() const noexcept
    {
        return 0U;
    }

private:
    EntryHandler m_entry{nullptr};
    void* m_argument{nullptr};
    FakeContextError m_switchError{FakeContextError::None};
};

class TestUserMemoryAccess final : public IUserMemoryAccess
{
public:
    explicit constexpr TestUserMemoryAccess(const UInt64 baseAddress) noexcept : m_baseAddress{baseAddress}
    {
    }

    void BindAddressSpace(const AddressSpaceId addressSpace) noexcept
    {
        m_addressSpace = addressSpace;
    }

    [[nodiscard]] AddressSpaceId GetAddressSpaceId() const noexcept override
    {
        return m_addressSpace;
    }

    [[nodiscard]] bool IsReady() const noexcept override
    {
        return m_addressSpace.IsValid();
    }

    [[nodiscard]] bool Store(const UInt64 address, const UInt8* const source, const Size sizeBytes) noexcept
    {
        Size offset{0U};
        if ((source == nullptr) || !Resolve(address, sizeBytes, offset))
        {
            return false;
        }
        for (Size index{0U}; index < sizeBytes; ++index)
        {
            m_bytes[offset + index] = source[index];
        }
        return true;
    }

    [[nodiscard]] const UInt8* GetBytes(const UInt64 address, const Size sizeBytes) const noexcept
    {
        Size offset{0U};
        return Resolve(address, sizeBytes, offset) ? m_bytes + offset : nullptr;
    }

    [[nodiscard]] constexpr Size GetAccessCount() const noexcept
    {
        return m_copyFromCount + m_prepareCount + m_commitCount + m_cancelCount;
    }

    [[nodiscard]] constexpr bool HasActiveReservation() const noexcept
    {
        return m_hasActiveReservation;
    }

protected:
    [[nodiscard]] UserMemoryAccessError CopyFromUserValidated(const UserMemoryRange source,
                                                              const WritableKernelBuffer destination) noexcept override
    {
        ++m_copyFromCount;
        Size offset{0U};
        if (!Resolve(source.GetStartAddress(), source.GetSizeBytes(), offset))
        {
            return UserMemoryAccessError::AccessDenied;
        }
        for (Size index{0U}; index < source.GetSizeBytes(); ++index)
        {
            destination.Data[index] = m_bytes[offset + index];
        }
        return UserMemoryAccessError::None;
    }

    [[nodiscard]] Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>
    PrepareCopyToUserValidated(const UserMemoryRange destination) noexcept override
    {
        using ResultType = Foundation::Result<UserMemoryWriteReservation, UserMemoryAccessError>;

        ++m_prepareCount;
        Size offset{0U};
        if (!Resolve(destination.GetStartAddress(), destination.GetSizeBytes(), offset))
        {
            return ResultType::Failure(UserMemoryAccessError::AccessDenied);
        }
        if (m_hasActiveReservation)
        {
            return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
        }
        const UInt64 generation = m_nextGeneration;
        if (m_nextGeneration == ~UInt64{0U})
        {
            m_nextGeneration = 0U;
        }
        else
        {
            ++m_nextGeneration;
        }
        if (generation == 0U)
        {
            return ResultType::Failure(UserMemoryAccessError::ReservationExhausted);
        }
        m_activeRange = destination;
        m_activeGeneration = generation;
        m_hasActiveReservation = true;
        return ResultType::Success(MakeWriteReservation(destination, generation));
    }

    [[nodiscard]] UserMemoryAccessError
    CancelCopyToUserValidated(const UserMemoryWriteReservation reservation) noexcept override
    {
        ++m_cancelCount;
        if (!IsCurrent(reservation))
        {
            return UserMemoryAccessError::StaleReservation;
        }
        ClearReservation();
        return UserMemoryAccessError::None;
    }

    [[nodiscard]] UserMemoryAccessError CommitCopyToUserValidated(const UserMemoryWriteReservation reservation,
                                                                  const ReadOnlyKernelBuffer source) noexcept override
    {
        ++m_commitCount;
        if (!IsCurrent(reservation))
        {
            return UserMemoryAccessError::StaleReservation;
        }
        Size offset{0U};
        if (!Resolve(reservation.GetRange().GetStartAddress(), source.SizeBytes, offset))
        {
            return UserMemoryAccessError::BackendProtocolViolation;
        }
        for (Size index{0U}; index < source.SizeBytes; ++index)
        {
            m_bytes[offset + index] = source.Data[index];
        }
        ClearReservation();
        return UserMemoryAccessError::None;
    }

private:
    [[nodiscard]] bool Resolve(const UInt64 address, const Size sizeBytes, Size& offset) const noexcept
    {
        if ((address < m_baseAddress) || (sizeBytes > sizeof(m_bytes)))
        {
            return false;
        }
        const UInt64 rawOffset = address - m_baseAddress;
        if (rawOffset > sizeof(m_bytes))
        {
            return false;
        }
        offset = static_cast<Size>(rawOffset);
        return offset <= (sizeof(m_bytes) - sizeBytes);
    }

    [[nodiscard]] bool IsCurrent(const UserMemoryWriteReservation reservation) const noexcept
    {
        return m_hasActiveReservation && (reservation.GetGeneration() == m_activeGeneration) &&
               reservation.GetRange().IsEqualTo(m_activeRange);
    }

    void ClearReservation() noexcept
    {
        m_activeRange = UserMemoryRange{};
        m_activeGeneration = 0U;
        m_hasActiveReservation = false;
    }

    UInt64 m_baseAddress;
    AddressSpaceId m_addressSpace{};
    UInt8 m_bytes[UserMemoryRange::MaximumSizeBytes]{};
    UserMemoryRange m_activeRange{};
    UInt64 m_nextGeneration{1U};
    UInt64 m_activeGeneration{0U};
    Size m_copyFromCount{0U};
    Size m_prepareCount{0U};
    Size m_commitCount{0U};
    Size m_cancelCount{0U};
    bool m_hasActiveReservation{false};
};

class TestKernelObject final : public IKernelObject
{
public:
    void Configure(const ObjectType type, const ResourceDomainId owner) noexcept
    {
        m_type = type;
        m_owner = owner;
    }

    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return m_type;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return m_owner;
    }

private:
    ObjectType m_type{ObjectType::Invalid};
    ResourceDomainId m_owner{};
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

[[nodiscard]] constexpr SchedulingContextConfiguration SchedulingConfiguration() noexcept
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

constexpr void WriteLittleEndian64(UInt8* const destination, const UInt64 value) noexcept
{
    for (UInt32 index{0U}; index < 8U; ++index)
    {
        destination[index] = static_cast<UInt8>(value >> (index * 8U));
    }
}

[[nodiscard]] constexpr UInt64 ReadLittleEndian64(const UInt8* const source) noexcept
{
    UInt64 value{0U};
    for (UInt32 index{0U}; index < 8U; ++index)
    {
        value |= static_cast<UInt64>(source[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr SystemCallResponse ResponseSentinel() noexcept
{
    return SystemCallResponse{
        .Error = SystemCallError::RightsDenied,
        .Value0 = 0x0123'4567'89AB'CDEFULL,
        .Value1 = 0xFEDC'BA98'7654'3210ULL,
    };
}

[[nodiscard]] constexpr bool ResponseIsSentinel(const SystemCallResponse& response) noexcept
{
    const SystemCallResponse expected = ResponseSentinel();
    return (response.Error == expected.Error) && (response.Value0 == expected.Value0) &&
           (response.Value1 == expected.Value1);
}

[[nodiscard]] constexpr SystemCallIngressFailure FailureSentinel() noexcept
{
    return SystemCallIngressFailure{
        .Error = SystemCallIngressError::CapabilitySpaceIdentifierMismatch,
        .ProcessorRunLoop = ProcessorKernelRunLoopError::NativeSwitchRejected,
        .ProtectionDomain = ProtectionDomainError::InUse,
        .Ipc =
            {
                .Error = IpcSystemCallDispatchError::InvalidContext,
                .Endpoint = Kernel::Ipc::EndpointErrorCode::QueueCorrupt,
                .UserMemory = UserMemoryAccessError::Fault,
                .Reserved = 0xA5U,
            },
        .Reserved = 0x5AU,
    };
}

[[nodiscard]] constexpr bool FailureIsSentinel(const SystemCallIngressFailure& failure) noexcept
{
    const SystemCallIngressFailure expected = FailureSentinel();
    return (failure.Error == expected.Error) && (failure.ProcessorRunLoop == expected.ProcessorRunLoop) &&
           (failure.ProtectionDomain == expected.ProtectionDomain) && (failure.Ipc.Error == expected.Ipc.Error) &&
           (failure.Ipc.Endpoint == expected.Ipc.Endpoint) && (failure.Ipc.UserMemory == expected.Ipc.UserMemory) &&
           (failure.Ipc.Reserved == expected.Ipc.Reserved) && (failure.Reserved == expected.Reserved);
}

[[nodiscard]] constexpr bool IsCanonicalIngressFailure(const SystemCallIngressFailure& failure,
                                                       const SystemCallIngressError expected) noexcept
{
    return (failure.Error == expected) && (failure.ProcessorRunLoop == ProcessorKernelRunLoopError::None) &&
           (failure.ProtectionDomain == ProtectionDomainError::None) &&
           (failure.Ipc.Error == IpcSystemCallDispatchError::None) &&
           (failure.Ipc.Endpoint == Kernel::Ipc::EndpointErrorCode::None) &&
           (failure.Ipc.UserMemory == UserMemoryAccessError::None) && (failure.Ipc.Reserved == 0U) &&
           (failure.Reserved == 0U);
}

[[nodiscard]] constexpr SystemCallRequest MakeIpcRequest(const SystemCallNumber number, const CapabilityHandle endpoint,
                                                         const UInt64 userAddress, const Size sizeBytes) noexcept
{
    return SystemCallRequest{
        .Number = static_cast<UInt64>(number),
        .Arguments =
            {
                endpoint.GetSelector(),
                endpoint.GetAuthenticator(),
                userAddress,
                static_cast<UInt64>(sizeBytes),
                0U,
                0U,
            },
    };
}

class IpcSystemCallIntegrationFixture final
{
public:
    IpcSystemCallIntegrationFixture() noexcept
        : m_registry{m_registryLock}, m_revocations{m_registryLock}, m_issuer{m_registry},
          m_firstCapabilities{m_registry, m_revocations, m_timer, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          m_secondCapabilities{m_registry, m_revocations, m_timer, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          m_timerQueue{m_registry, m_timer}, m_controller{m_registry, m_timerQueue, m_timer, CpuId{0U}},
          m_runLoop{m_controller, m_addressSpaces, m_idleContext}, m_firstResource{m_firstResourceLedger},
          m_secondResource{m_secondResourceLedger}, m_firstScheduling{m_registry}, m_secondScheduling{m_registry},
          m_requestEndpoint{m_secondCapabilities, SecondResourceDomain},
          m_replyEndpoint{m_firstCapabilities, FirstResourceDomain}, m_firstUserMemory{FirstUserBase},
          m_secondUserMemory{SecondUserBase}
    {
        m_idleContext.SetSwitchError(FakeContextError::Rejected);
        m_firstContext.SetEntry(RunFirstContext, this);
        m_secondContext.SetEntry(RunSecondContext, this);
    }

    ~IpcSystemCallIntegrationFixture() noexcept
    {
        if (!m_cleaned)
        {
            static_cast<void>(Cleanup());
        }
    }

    IpcSystemCallIntegrationFixture(const IpcSystemCallIntegrationFixture&) = delete;
    IpcSystemCallIntegrationFixture& operator=(const IpcSystemCallIntegrationFixture&) = delete;
    IpcSystemCallIntegrationFixture(IpcSystemCallIntegrationFixture&&) = delete;
    IpcSystemCallIntegrationFixture& operator=(IpcSystemCallIntegrationFixture&&) = delete;

    [[nodiscard]] bool Initialize() noexcept
    {
        if (!m_firstCapabilities.IsReady() || !m_secondCapabilities.IsReady() || !BuildSeedPlan(m_seedPlan) ||
            !InitializeResourceDomain(m_firstResource, FirstResourceDomain, m_firstResourceObject,
                                      m_firstResourceIdentity) ||
            !InitializeResourceDomain(m_secondResource, SecondResourceDomain, m_secondResourceObject,
                                      m_secondResourceIdentity) ||
            !InitializeAddressSpace(m_idleAddressSpace, m_idlePageTable, IdleResourceDomain, m_idleAddressSpaceObject,
                                    m_idleAddressSpaceIdentity) ||
            !InitializeAddressSpace(m_firstAddressSpace, m_firstPageTable, FirstResourceDomain,
                                    m_firstAddressSpaceObject, m_firstAddressSpaceIdentity) ||
            !InitializeAddressSpace(m_secondAddressSpace, m_secondPageTable, SecondResourceDomain,
                                    m_secondAddressSpaceObject, m_secondAddressSpaceIdentity) ||
            !InitializeProtectionDomain(m_firstProtection, m_firstResourceIdentity, m_firstResource,
                                        m_firstAddressSpaceIdentity, m_firstCapabilities, m_firstProtectionObject,
                                        m_firstProtectionIdentity) ||
            !InitializeProtectionDomain(m_secondProtection, m_secondResourceIdentity, m_secondResource,
                                        m_secondAddressSpaceIdentity, m_secondCapabilities, m_secondProtectionObject,
                                        m_secondProtectionIdentity))
        {
            return false;
        }

        const auto addressController = m_addressSpaces.Initialize(CpuId{0U}, m_idleAddressSpace);
        m_addressControllerReady = addressController.HasValue();
        if (!m_addressControllerReady ||
            !InitializeThreadPair(m_firstThread, m_firstScheduling, FirstResourceDomain, m_firstThreadObject,
                                  m_firstSchedulingObject, m_firstThreadIdentity, m_firstSchedulingIdentity) ||
            !InitializeThreadPair(m_secondThread, m_secondScheduling, SecondResourceDomain, m_secondThreadObject,
                                  m_secondSchedulingObject, m_secondThreadIdentity, m_secondSchedulingIdentity))
        {
            return false;
        }

        m_firstRunLoopBound =
            m_runLoop.Bind(m_firstThread, m_firstContext, m_firstProtection) == ProcessorKernelRunLoopError::None;
        m_secondRunLoopBound =
            m_firstRunLoopBound &&
            (m_runLoop.Bind(m_secondThread, m_secondContext, m_secondProtection) == ProcessorKernelRunLoopError::None);
        if (!m_firstRunLoopBound || !m_secondRunLoopBound)
        {
            return false;
        }

        if (!RegisterObject(m_requestEndpoint, m_requestEndpointObject) ||
            !RegisterObject(m_replyEndpoint, m_replyEndpointObject))
        {
            return false;
        }
        m_attachmentObject.Configure(ObjectType::MemoryObject, FirstResourceDomain);
        if (!RegisterObject(m_attachmentObject, m_attachmentObjectIdentity))
        {
            return false;
        }

        const auto revocation = m_revocations.CreateRoot();
        if (!revocation.HasValue())
        {
            return false;
        }
        m_revocationDomain = revocation.GetValue();
        if (!MintCapabilities())
        {
            return false;
        }

        m_firstUserMemory.BindAddressSpace(m_firstAddressSpaceIdentity);
        m_secondUserMemory.BindAddressSpace(m_secondAddressSpaceIdentity);
        const auto firstBinding = m_firstProtection.GetBinding();
        const auto secondBinding = m_secondProtection.GetBinding();
        if (!firstBinding.HasValue() || !secondBinding.HasValue() ||
            (m_firstCapabilities.GetIdentifier().GetValue() == m_secondCapabilities.GetIdentifier().GetValue()) ||
            m_firstAddressSpaceIdentity.IsEqualTo(m_secondAddressSpaceIdentity) ||
            m_firstProtectionIdentity.IsEqualTo(m_secondProtectionIdentity) ||
            (firstBinding.GetValue().Capabilities != &m_firstCapabilities) ||
            (secondBinding.GetValue().Capabilities != &m_secondCapabilities) ||
            (firstBinding.GetValue().AddressSpaceStorage != &m_firstAddressSpace) ||
            (secondBinding.GetValue().AddressSpaceStorage != &m_secondAddressSpace) || !StageRequestFrame())
        {
            return false;
        }

        m_initialized = true;
        return true;
    }

    [[nodiscard]] bool RunExchange() noexcept
    {
        if (!m_initialized)
        {
            return false;
        }
        const bool firstSendActivation =
            Activate(m_firstThread, m_firstScheduling, m_firstThreadIdentity, m_firstSchedulingIdentity, m_firstQueued);
        const bool secondActivation =
            firstSendActivation && Activate(m_secondThread, m_secondScheduling, m_secondThreadIdentity,
                                            m_secondSchedulingIdentity, m_secondQueued);
        const bool firstReplyActivation =
            secondActivation &&
            Activate(m_firstThread, m_firstScheduling, m_firstThreadIdentity, m_firstSchedulingIdentity, m_firstQueued);
        return firstSendActivation && secondActivation && firstReplyActivation;
    }

    [[nodiscard]] constexpr bool NegativeIngressPassed() const noexcept
    {
        return m_negativeIngressPassed;
    }

    [[nodiscard]] constexpr bool RequestSendPassed() const noexcept
    {
        return m_requestSendPassed;
    }

    [[nodiscard]] constexpr bool RequestDeliveryPassed() const noexcept
    {
        return m_requestDeliveryPassed;
    }

    [[nodiscard]] constexpr bool ReplySendPassed() const noexcept
    {
        return m_replySendPassed;
    }

    [[nodiscard]] constexpr bool ReplyDeliveryPassed() const noexcept
    {
        return m_replyDeliveryPassed;
    }

    [[nodiscard]] bool FinalStatePassed() noexcept
    {
        return (m_firstEntryCalls == 2U) && (m_secondEntryCalls == 1U) &&
               m_requestEndpoint.HasReceiverCapabilitySpace(m_secondCapabilities) &&
               !m_requestEndpoint.HasReceiverCapabilitySpace(m_firstCapabilities) &&
               m_replyEndpoint.HasReceiverCapabilitySpace(m_firstCapabilities) &&
               !m_replyEndpoint.HasReceiverCapabilitySpace(m_secondCapabilities) &&
               (m_requestEndpoint.GetQueueState().QueuedMessages == 0U) &&
               (m_replyEndpoint.GetQueueState().QueuedMessages == 0U) && !m_firstUserMemory.HasActiveReservation() &&
               !m_secondUserMemory.HasActiveReservation() &&
               HandleBelongsOnlyTo(m_firstCapabilities, m_secondCapabilities, m_finalAttachmentHandle);
    }

    [[nodiscard]] bool Cleanup() noexcept
    {
        if (m_cleaned)
        {
            return m_cleanupPassed;
        }

        bool cleaned{true};
        if (m_firstQueued && m_firstThreadIdentity.IsValid())
        {
            const auto removed = m_controller.Remove(m_firstThreadIdentity);
            cleaned = removed.HasValue() && cleaned;
            m_firstQueued = false;
        }
        if (m_secondQueued && m_secondThreadIdentity.IsValid())
        {
            const auto removed = m_controller.Remove(m_secondThreadIdentity);
            cleaned = removed.HasValue() && cleaned;
            m_secondQueued = false;
        }
        if (m_secondRunLoopBound)
        {
            cleaned = (m_runLoop.Unbind(m_secondThread) == ProcessorKernelRunLoopError::None) && cleaned;
            m_secondRunLoopBound = false;
        }
        if (m_firstRunLoopBound)
        {
            cleaned = (m_runLoop.Unbind(m_firstThread) == ProcessorKernelRunLoopError::None) && cleaned;
            m_firstRunLoopBound = false;
        }
        if (m_secondSchedulingBound)
        {
            cleaned = (m_secondScheduling.Unbind(m_secondThreadIdentity) == SchedulingContextError::None) && cleaned;
            m_secondSchedulingBound = false;
        }
        if (m_firstSchedulingBound)
        {
            cleaned = (m_firstScheduling.Unbind(m_firstThreadIdentity) == SchedulingContextError::None) && cleaned;
            m_firstSchedulingBound = false;
        }

        const auto requestDiscarded = m_requestEndpoint.DiscardUnread();
        const auto replyDiscarded = m_replyEndpoint.DiscardUnread();
        cleaned = requestDiscarded.HasValue() && replyDiscarded.HasValue() &&
                  !m_firstUserMemory.HasActiveReservation() && !m_secondUserMemory.HasActiveReservation() && cleaned;

        cleaned = ClosePossible(m_firstCapabilities, m_requestInvokeHandle) && cleaned;
        cleaned = ClosePossible(m_firstCapabilities, m_replyReadHandle) && cleaned;
        cleaned = ClosePossible(m_secondCapabilities, m_requestReadHandle) && cleaned;
        cleaned = ClosePossible(m_secondCapabilities, m_replyInvokeHandle) && cleaned;

        cleaned = ClosePossible(m_firstCapabilities, m_originalAttachmentHandle) && cleaned;
        cleaned = ClosePossible(m_firstCapabilities, m_finalAttachmentHandle) && cleaned;
        cleaned = ClosePossible(m_secondCapabilities, m_requestAttachmentHandle) && cleaned;
        cleaned = ClosePossible(m_secondCapabilities, m_replySourceAttachmentHandle) && cleaned;
        cleaned =
            (m_firstCapabilities.GetActiveCount() == 0U) && (m_secondCapabilities.GetActiveCount() == 0U) && cleaned;

        cleaned = DestroyIfRegistered(m_requestEndpointObject, m_requestEndpoint) && cleaned;
        cleaned = DestroyIfRegistered(m_replyEndpointObject, m_replyEndpoint) && cleaned;
        cleaned = DestroyIfRegistered(m_attachmentObjectIdentity, m_attachmentObject) && cleaned;

        cleaned = QuiesceProtectionDomain(m_secondProtection) && cleaned;
        cleaned = QuiesceProtectionDomain(m_firstProtection) && cleaned;

        if (m_addressControllerReady)
        {
            cleaned = (m_idleAddressSpace.DetachProcessor(CpuId{0U}) == AddressSpaceError::None) && cleaned;
            m_addressControllerReady = false;
        }
        cleaned = QuiesceAddressSpace(m_secondAddressSpace) && cleaned;
        cleaned = QuiesceAddressSpace(m_firstAddressSpace) && cleaned;
        cleaned = QuiesceAddressSpace(m_idleAddressSpace) && cleaned;
        cleaned = QuiesceResourceDomain(m_secondResource) && cleaned;
        cleaned = QuiesceResourceDomain(m_firstResource) && cleaned;

        cleaned = DestroyIfRegistered(m_secondProtectionObject, m_secondProtection) && cleaned;
        cleaned = DestroyIfRegistered(m_firstProtectionObject, m_firstProtection) && cleaned;
        cleaned = DestroyIfRegistered(m_secondAddressSpaceObject, m_secondAddressSpace) && cleaned;
        cleaned = DestroyIfRegistered(m_firstAddressSpaceObject, m_firstAddressSpace) && cleaned;
        cleaned = DestroyIfRegistered(m_idleAddressSpaceObject, m_idleAddressSpace) && cleaned;
        cleaned = DestroyIfRegistered(m_secondResourceObject, m_secondResource) && cleaned;
        cleaned = DestroyIfRegistered(m_firstResourceObject, m_firstResource) && cleaned;
        cleaned = DestroyIfRegistered(m_secondSchedulingObject, m_secondScheduling) && cleaned;
        cleaned = DestroyIfRegistered(m_firstSchedulingObject, m_firstScheduling) && cleaned;
        cleaned = DestroyIfRegistered(m_secondThreadObject, m_secondThread) && cleaned;
        cleaned = DestroyIfRegistered(m_firstThreadObject, m_firstThread) && cleaned;

        m_cleanupPassed = cleaned;
        m_cleaned = true;
        return cleaned;
    }

private:
    static void RunFirstContext(void* const argument) noexcept
    {
        if (argument != nullptr)
        {
            static_cast<IpcSystemCallIntegrationFixture*>(argument)->FirstContextEntry();
        }
    }

    static void RunSecondContext(void* const argument) noexcept
    {
        if (argument != nullptr)
        {
            static_cast<IpcSystemCallIntegrationFixture*>(argument)->SecondContextEntry();
        }
    }

    void FirstContextEntry() noexcept
    {
        const Size phase = m_firstEntryCalls;
        ++m_firstEntryCalls;
        if (phase == 0U)
        {
            RunNegativeIngressCases();
            SendRequest();
            return;
        }
        if (phase == 1U)
        {
            ReceiveReply();
        }
    }

    void SecondContextEntry() noexcept
    {
        ++m_secondEntryCalls;
        ReceiveRequestAndSendReply();
    }

    void RunNegativeIngressCases() noexcept
    {
        const bool active = ActiveBindingMatches(m_firstProtection, m_firstProtectionIdentity, m_firstAddressSpace,
                                                 m_firstAddressSpaceIdentity);
        const SystemCallRequest request = MakeIpcRequest(SystemCallNumber::IpcSend, m_requestInvokeHandle,
                                                         FirstRequestFrameAddress, m_requestFrameSize);
        const Size firstAccesses = m_firstUserMemory.GetAccessCount();
        const Size secondAccesses = m_secondUserMemory.GetAccessCount();

        SystemCallResponse legacyResponse = ResponseSentinel();
        SystemCallIngressFailure legacyFailure = FailureSentinel();
        const bool legacyDispatched = SystemCallIngress::Dispatch(m_runLoop, request, legacyResponse, legacyFailure);

        SystemCallResponse foreignResponse = ResponseSentinel();
        SystemCallIngressFailure foreignFailure = FailureSentinel();
        const bool foreignDispatched = SystemCallIngress::Dispatch(m_runLoop, m_secondUserMemory, m_firstScratch,
                                                                   request, foreignResponse, foreignFailure);

        m_negativeIngressPassed =
            active && legacyDispatched && (legacyResponse.Error == SystemCallError::InvalidConfiguration) &&
            (legacyResponse.Value0 == 0U) && (legacyResponse.Value1 == 0U) && FailureIsSentinel(legacyFailure) &&
            !foreignDispatched && ResponseIsSentinel(foreignResponse) &&
            IsCanonicalIngressFailure(foreignFailure, SystemCallIngressError::UserMemoryAddressSpaceMismatch) &&
            (m_firstUserMemory.GetAccessCount() == firstAccesses) &&
            (m_secondUserMemory.GetAccessCount() == secondAccesses) &&
            (m_requestEndpoint.GetQueueState().QueuedMessages == 0U);
    }

    void SendRequest() noexcept
    {
        const SystemCallRequest request = MakeIpcRequest(SystemCallNumber::IpcSend, m_requestInvokeHandle,
                                                         FirstRequestFrameAddress, m_requestFrameSize);
        SystemCallResponse response = ResponseSentinel();
        SystemCallIngressFailure failure = FailureSentinel();
        const bool dispatched =
            SystemCallIngress::Dispatch(m_runLoop, m_firstUserMemory, m_firstScratch, request, response, failure);
        m_requestSendPassed = dispatched && (response.Error == SystemCallError::None) && (response.Value0 == 1U) &&
                              (response.Value1 == 0U) && FailureIsSentinel(failure) &&
                              (m_requestEndpoint.GetQueueState().QueuedMessages == 1U) &&
                              !HandleIsUsable(m_firstCapabilities, m_originalAttachmentHandle);
    }

    void ReceiveRequestAndSendReply() noexcept
    {
        const bool active = ActiveBindingMatches(m_secondProtection, m_secondProtectionIdentity, m_secondAddressSpace,
                                                 m_secondAddressSpaceIdentity);
        const SystemCallRequest receiveRequest =
            MakeIpcRequest(SystemCallNumber::IpcReceive, m_requestReadHandle, SecondRequestDeliveryAddress,
                           IpcDeliveryCodec::MaximumWireSizeBytes);
        SystemCallResponse receiveResponse = ResponseSentinel();
        SystemCallIngressFailure receiveFailure = FailureSentinel();
        const bool received = SystemCallIngress::Dispatch(m_runLoop, m_secondUserMemory, m_secondScratch,
                                                          receiveRequest, receiveResponse, receiveFailure);

        bool decoded{false};
        if (received && (receiveResponse.Error == SystemCallError::None) &&
            (receiveResponse.Value1 <= IpcDeliveryCodec::MaximumWireSizeBytes))
        {
            const Size deliverySize = static_cast<Size>(receiveResponse.Value1);
            const UInt8* const delivery = m_secondUserMemory.GetBytes(SecondRequestDeliveryAddress, deliverySize);
            if (delivery != nullptr)
            {
                const auto decode = IpcDeliveryCodec::Decode(
                    Kernel::Ipc::ReadOnlyByteView{
                        .Data = delivery,
                        .SizeBytes = deliverySize,
                    },
                    m_requestMetadata, m_decodedRequest);
                decoded = (decode.Error == IpcDeliveryCodecError::None) && (decode.Frame == MessageCodecError::None) &&
                          DeliveryIsCanonical(delivery, deliverySize, m_requestMetadata, m_decodedRequest);
            }
        }

        m_requestAttachmentHandle = m_decodedRequest.Attachments[0U].Handle;
        m_replySourceAttachmentHandle = m_requestAttachmentHandle;
        const bool metadata =
            decoded && (receiveResponse.Value0 == 1U) && (m_requestMetadata.Sequence == 1U) &&
            (m_requestMetadata.Sender.GetValue() == FirstResourceDomain.GetValue()) &&
            m_requestMetadata.SenderEndpointRights.IsEqualTo(RequestInvokeRights()) &&
            (m_requestMetadata.SenderEndpointSealedObjectSelector == RequestEndpointSelector) &&
            (m_requestMetadata.SendClass == EndpointSendClass::Normal) && (m_requestMetadata.AttachmentCount == 1U) &&
            (receiveResponse.Value1 == (IpcDeliveryCodec::WireHeaderSizeBytes + m_requestMetadata.FrameSizeBytes));
        const bool message = decoded && TypedRequestMatches(m_decodedRequest) &&
                             HandleBelongsOnlyTo(m_secondCapabilities, m_firstCapabilities, m_requestAttachmentHandle);
        m_requestDeliveryPassed = active && received && (receiveResponse.Error == SystemCallError::None) &&
                                  FailureIsSentinel(receiveFailure) && metadata && message &&
                                  m_requestEndpoint.HasReceiverCapabilitySpace(m_secondCapabilities) &&
                                  !m_requestEndpoint.HasReceiverCapabilitySpace(m_firstCapabilities) &&
                                  (m_requestEndpoint.GetQueueState().QueuedMessages == 0U);
        if (!m_requestDeliveryPassed || !StageReplyFrame())
        {
            return;
        }

        const SystemCallRequest sendRequest =
            MakeIpcRequest(SystemCallNumber::IpcSend, m_replyInvokeHandle, SecondReplyFrameAddress, m_replyFrameSize);
        SystemCallResponse sendResponse = ResponseSentinel();
        SystemCallIngressFailure sendFailure = FailureSentinel();
        const bool sent = SystemCallIngress::Dispatch(m_runLoop, m_secondUserMemory, m_secondScratch, sendRequest,
                                                      sendResponse, sendFailure);
        m_replySendPassed = sent && (sendResponse.Error == SystemCallError::None) && (sendResponse.Value0 == 1U) &&
                            (sendResponse.Value1 == 0U) && FailureIsSentinel(sendFailure) &&
                            (m_replyEndpoint.GetQueueState().QueuedMessages == 1U) &&
                            !HandleIsUsable(m_secondCapabilities, m_replySourceAttachmentHandle);
    }

    void ReceiveReply() noexcept
    {
        const bool active = ActiveBindingMatches(m_firstProtection, m_firstProtectionIdentity, m_firstAddressSpace,
                                                 m_firstAddressSpaceIdentity);
        const SystemCallRequest receiveRequest =
            MakeIpcRequest(SystemCallNumber::IpcReceive, m_replyReadHandle, FirstReplyDeliveryAddress,
                           IpcDeliveryCodec::MaximumWireSizeBytes);
        SystemCallResponse response = ResponseSentinel();
        SystemCallIngressFailure failure = FailureSentinel();
        const bool received = SystemCallIngress::Dispatch(m_runLoop, m_firstUserMemory, m_firstScratch, receiveRequest,
                                                          response, failure);

        bool decoded{false};
        if (received && (response.Error == SystemCallError::None) &&
            (response.Value1 <= IpcDeliveryCodec::MaximumWireSizeBytes))
        {
            const Size deliverySize = static_cast<Size>(response.Value1);
            const UInt8* const delivery = m_firstUserMemory.GetBytes(FirstReplyDeliveryAddress, deliverySize);
            if (delivery != nullptr)
            {
                const auto decode = IpcDeliveryCodec::Decode(
                    Kernel::Ipc::ReadOnlyByteView{
                        .Data = delivery,
                        .SizeBytes = deliverySize,
                    },
                    m_replyMetadata, m_decodedReply);
                decoded = (decode.Error == IpcDeliveryCodecError::None) && (decode.Frame == MessageCodecError::None) &&
                          DeliveryIsCanonical(delivery, deliverySize, m_replyMetadata, m_decodedReply);
            }
        }

        m_finalAttachmentHandle = m_decodedReply.Attachments[0U].Handle;
        m_replyDeliveryPassed =
            active && received && (response.Error == SystemCallError::None) && (response.Value0 == 1U) &&
            FailureIsSentinel(failure) && decoded && (m_replyMetadata.Sequence == 1U) &&
            (m_replyMetadata.Sender.GetValue() == SecondResourceDomain.GetValue()) &&
            m_replyMetadata.SenderEndpointRights.IsEqualTo(ReplyInvokeRights()) &&
            (m_replyMetadata.SenderEndpointSealedObjectSelector == ReplyEndpointSelector) &&
            (m_replyMetadata.SendClass == EndpointSendClass::Normal) && (m_replyMetadata.AttachmentCount == 1U) &&
            (response.Value1 == (IpcDeliveryCodec::WireHeaderSizeBytes + m_replyMetadata.FrameSizeBytes)) &&
            TypedReplyMatches(m_decodedReply) &&
            HandleBelongsOnlyTo(m_firstCapabilities, m_secondCapabilities, m_finalAttachmentHandle) &&
            m_replyEndpoint.HasReceiverCapabilitySpace(m_firstCapabilities) &&
            !m_replyEndpoint.HasReceiverCapabilitySpace(m_secondCapabilities) &&
            (m_replyEndpoint.GetQueueState().QueuedMessages == 0U);
    }

    [[nodiscard]] bool InitializeResourceDomain(ResourceDomain& resource, const ResourceDomainId identifier,
                                                ObjectId& object, ResourceDomainObjectId& identity) noexcept
    {
        if (resource.InitializeRoot(identifier, ResourceLimits()).Error != ResourceDomainError::None)
        {
            return false;
        }
        if (!RegisterObject(resource, object))
        {
            return false;
        }
        identity = ResourceDomainObjectId{m_registry, object};
        return identity.IsValid();
    }

    [[nodiscard]] bool InitializeAddressSpace(AddressSpace& addressSpace, TestAddressSpacePageTable& pageTable,
                                              const ResourceDomainId owner, ObjectId& object,
                                              AddressSpaceId& identity) noexcept
    {
        if (addressSpace.Initialize(owner, pageTable, m_seedPlan) != AddressSpaceError::None)
        {
            return false;
        }
        if (!RegisterObject(addressSpace, object))
        {
            return false;
        }
        identity = AddressSpaceId{m_registry, object};
        return addressSpace.BindIdentity(m_registry, identity).Error == AddressSpaceError::None;
    }

    [[nodiscard]] bool InitializeProtectionDomain(ProtectionDomain& protection,
                                                  const ResourceDomainObjectId resourceIdentity,
                                                  ResourceDomain& resource, const AddressSpaceId addressIdentity,
                                                  CapabilitySpace& capabilities, ObjectId& object,
                                                  ProtectionDomainId& identity) noexcept
    {
        if (protection.Initialize(m_registry, resourceIdentity, resource, addressIdentity, capabilities).Error !=
            ProtectionDomainError::None)
        {
            return false;
        }
        if (!RegisterObject(protection, object))
        {
            return false;
        }
        identity = ProtectionDomainId{m_registry, object};
        return protection.BindIdentity(m_registry, identity).Error == ProtectionDomainError::None;
    }

    [[nodiscard]] bool InitializeThreadPair(Thread& thread, SchedulingContext& scheduling, const ResourceDomainId owner,
                                            ObjectId& threadObject, ObjectId& schedulingObject,
                                            ThreadId& threadIdentity, SchedulingContextId& schedulingIdentity) noexcept
    {
        if ((thread.Initialize(owner) != ThreadError::None) ||
            (scheduling.Initialize(SchedulingConfiguration(), owner, MonotonicTime{0U}) !=
             SchedulingContextError::None) ||
            !RegisterObject(thread, threadObject) || !RegisterObject(scheduling, schedulingObject))
        {
            return false;
        }
        threadIdentity = ThreadId{m_registry, threadObject};
        schedulingIdentity = SchedulingContextId{m_registry, schedulingObject};
        if ((scheduling.Bind(threadIdentity) != SchedulingContextError::None) ||
            (thread.MakeReady() != ThreadError::None))
        {
            return false;
        }
        if (&thread == &m_firstThread)
        {
            m_firstSchedulingBound = true;
        }
        else
        {
            m_secondSchedulingBound = true;
        }
        return true;
    }

    [[nodiscard]] bool RegisterObject(IKernelObject& object, ObjectId& identity) noexcept
    {
        const auto registered = m_issuer.CreateObject(object);
        if (!registered.HasValue())
        {
            return false;
        }
        identity = registered.GetValue();
        return true;
    }

    [[nodiscard]] bool MintCapabilities() noexcept
    {
        const auto requestInvoke =
            m_issuer.Mint(m_firstCapabilities, m_requestEndpointObject, RequestInvokeRights(), m_revocationDomain,
                          MonotonicTime::Infinite(), RequestEndpointSelector);
        if (!requestInvoke.HasValue())
        {
            return false;
        }
        m_requestInvokeHandle = requestInvoke.GetValue();

        const auto requestRead = m_issuer.Mint(m_secondCapabilities, m_requestEndpointObject, EndpointReadRights(),
                                               m_revocationDomain, MonotonicTime::Infinite(), 0xA57E'1000'0000'0011ULL);
        if (!requestRead.HasValue())
        {
            return false;
        }
        m_requestReadHandle = requestRead.GetValue();

        const auto replyInvoke = m_issuer.Mint(m_secondCapabilities, m_replyEndpointObject, ReplyInvokeRights(),
                                               m_revocationDomain, MonotonicTime::Infinite(), ReplyEndpointSelector);
        if (!replyInvoke.HasValue())
        {
            return false;
        }
        m_replyInvokeHandle = replyInvoke.GetValue();

        const auto replyRead = m_issuer.Mint(m_firstCapabilities, m_replyEndpointObject, EndpointReadRights(),
                                             m_revocationDomain, MonotonicTime::Infinite(), 0xA57E'2000'0000'0022ULL);
        if (!replyRead.HasValue())
        {
            return false;
        }
        m_replyReadHandle = replyRead.GetValue();

        const auto attachment = m_issuer.Mint(m_firstCapabilities, m_attachmentObjectIdentity, AttachmentRights(),
                                              m_revocationDomain, MonotonicTime::Infinite(), AttachmentSelector);
        if (!attachment.HasValue())
        {
            return false;
        }
        m_originalAttachmentHandle = attachment.GetValue();
        return true;
    }

    [[nodiscard]] bool StageRequestFrame() noexcept
    {
        m_requestMessage = MakeRequest(m_originalAttachmentHandle);
        const auto encoded = MessageCodec::Encode(m_requestMessage, Kernel::Ipc::WritableByteView{
                                                                        .Data = m_frameBuffer,
                                                                        .SizeBytes = sizeof(m_frameBuffer),
                                                                    });
        if (!encoded.HasValue())
        {
            return false;
        }
        m_requestFrameSize = encoded.GetValue();
        return m_firstUserMemory.Store(FirstRequestFrameAddress, m_frameBuffer, m_requestFrameSize);
    }

    [[nodiscard]] bool StageReplyFrame() noexcept
    {
        m_replyMessage = MakeReply(m_replySourceAttachmentHandle);
        const auto encoded = MessageCodec::Encode(m_replyMessage, Kernel::Ipc::WritableByteView{
                                                                      .Data = m_frameBuffer,
                                                                      .SizeBytes = sizeof(m_frameBuffer),
                                                                  });
        if (!encoded.HasValue())
        {
            return false;
        }
        m_replyFrameSize = encoded.GetValue();
        return m_secondUserMemory.Store(SecondReplyFrameAddress, m_frameBuffer, m_replyFrameSize);
    }

    [[nodiscard]] static MessageEnvelope MakeRequest(const CapabilityHandle attachment) noexcept
    {
        MessageEnvelope message{};
        message.Interface = InterfaceIdentifier{InterfaceLow, InterfaceHigh};
        message.TransactionIdentifier = TransactionIdentifier;
        message.Deadline = MonotonicTime::Infinite();
        message.Trace = {
            .TraceIdentifierHigh = 0x0102'0304'0506'0708ULL,
            .TraceIdentifierLow = 0x1112'1314'1516'1718ULL,
            .SpanIdentifier = 0x2122'2324'2526'2728ULL,
        };
        message.MethodIdentifier = MethodIdentifier;
        message.InterfaceVersionMajor = 1U;
        message.InterfaceVersionMinor = 0U;
        message.FixedPayloadSizeBytes = 16U;
        message.PayloadSizeBytes = 16U;
        message.Kind = MessageKind::SynchronousCall;
        message.Cancellation = CancellationState::Cancellable;
        message.AttachmentCount = 1U;
        message.Attachments[0U] = CapabilityAttachment{
            .Handle = attachment,
            .RequestedDeadline = MonotonicTime::Infinite(),
            .RequestedRights = AttachmentRights(),
            .ExpectedType = ObjectType::MemoryObject,
            .TransferMode = CapabilityTransferMode::Move,
            .Reserved = 0U,
        };
        WriteLittleEndian64(message.Payload, RequestLeft);
        WriteLittleEndian64(message.Payload + 8U, RequestRight);
        return message;
    }

    [[nodiscard]] static MessageEnvelope MakeReply(const CapabilityHandle attachment) noexcept
    {
        MessageEnvelope message{};
        message.Interface = InterfaceIdentifier{InterfaceLow, InterfaceHigh};
        message.TransactionIdentifier = TransactionIdentifier;
        message.Deadline = MonotonicTime::Infinite();
        message.Trace = {
            .TraceIdentifierHigh = 0x0102'0304'0506'0708ULL,
            .TraceIdentifierLow = 0x1112'1314'1516'1718ULL,
            .SpanIdentifier = 0x3132'3334'3536'3738ULL,
        };
        message.MethodIdentifier = MethodIdentifier;
        message.InterfaceVersionMajor = 1U;
        message.InterfaceVersionMinor = 0U;
        message.FixedPayloadSizeBytes = 8U;
        message.PayloadSizeBytes = 8U;
        message.Kind = MessageKind::Reply;
        message.Cancellation = CancellationState::None;
        message.AttachmentCount = 1U;
        message.Attachments[0U] = CapabilityAttachment{
            .Handle = attachment,
            .RequestedDeadline = MonotonicTime::Infinite(),
            .RequestedRights = AttachmentRights(),
            .ExpectedType = ObjectType::MemoryObject,
            .TransferMode = CapabilityTransferMode::Move,
            .Reserved = 0U,
        };
        WriteLittleEndian64(message.Payload, ReplyValue);
        return message;
    }

    [[nodiscard]] static bool TypedRequestMatches(const MessageEnvelope& message) noexcept
    {
        return (message.Interface.GetLow() == InterfaceLow) && (message.Interface.GetHigh() == InterfaceHigh) &&
               (message.TransactionIdentifier == TransactionIdentifier) &&
               (message.Deadline.GetTicks() == MonotonicTime::Infinite().GetTicks()) &&
               (message.Trace.TraceIdentifierHigh == 0x0102'0304'0506'0708ULL) &&
               (message.Trace.TraceIdentifierLow == 0x1112'1314'1516'1718ULL) &&
               (message.Trace.SpanIdentifier == 0x2122'2324'2526'2728ULL) &&
               (message.MethodIdentifier == MethodIdentifier) && (message.InterfaceVersionMajor == 1U) &&
               (message.InterfaceVersionMinor == 0U) && (message.FixedPayloadSizeBytes == 16U) &&
               (message.PayloadSizeBytes == 16U) && (message.Kind == MessageKind::SynchronousCall) &&
               (message.Cancellation == CancellationState::Cancellable) && (message.VariablePayloadCount == 0U) &&
               (message.AttachmentCount == 1U) &&
               message.Attachments[0U].RequestedRights.IsEqualTo(AttachmentRights()) &&
               (message.Attachments[0U].ExpectedType == ObjectType::MemoryObject) &&
               (message.Attachments[0U].TransferMode == CapabilityTransferMode::Move) &&
               (message.Attachments[0U].Reserved == 0U) && (ReadLittleEndian64(message.Payload) == RequestLeft) &&
               (ReadLittleEndian64(message.Payload + 8U) == RequestRight);
    }

    [[nodiscard]] static bool TypedReplyMatches(const MessageEnvelope& message) noexcept
    {
        return (message.Interface.GetLow() == InterfaceLow) && (message.Interface.GetHigh() == InterfaceHigh) &&
               (message.TransactionIdentifier == TransactionIdentifier) &&
               (message.Deadline.GetTicks() == MonotonicTime::Infinite().GetTicks()) &&
               (message.Trace.TraceIdentifierHigh == 0x0102'0304'0506'0708ULL) &&
               (message.Trace.TraceIdentifierLow == 0x1112'1314'1516'1718ULL) &&
               (message.Trace.SpanIdentifier == 0x3132'3334'3536'3738ULL) &&
               (message.MethodIdentifier == MethodIdentifier) && (message.InterfaceVersionMajor == 1U) &&
               (message.InterfaceVersionMinor == 0U) && (message.FixedPayloadSizeBytes == 8U) &&
               (message.PayloadSizeBytes == 8U) && (message.Kind == MessageKind::Reply) &&
               (message.Cancellation == CancellationState::None) && (message.VariablePayloadCount == 0U) &&
               (message.AttachmentCount == 1U) &&
               message.Attachments[0U].RequestedRights.IsEqualTo(AttachmentRights()) &&
               (message.Attachments[0U].ExpectedType == ObjectType::MemoryObject) &&
               (message.Attachments[0U].TransferMode == CapabilityTransferMode::Move) &&
               (message.Attachments[0U].Reserved == 0U) && (ReadLittleEndian64(message.Payload) == ReplyValue);
    }

    [[nodiscard]] bool DeliveryIsCanonical(const UInt8* const delivery, const Size deliverySize,
                                           const EndpointReceiveMetadata& metadata,
                                           const MessageEnvelope& message) noexcept
    {
        const auto encoded = IpcDeliveryCodec::Encode(metadata, message,
                                                      Kernel::Ipc::WritableByteView{
                                                          .Data = m_deliveryBuffer,
                                                          .SizeBytes = sizeof(m_deliveryBuffer),
                                                      });
        if (!encoded.HasValue() || (encoded.GetValue() != deliverySize))
        {
            return false;
        }
        for (Size index{0U}; index < deliverySize; ++index)
        {
            if (m_deliveryBuffer[index] != delivery[index])
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ActiveBindingMatches(ProtectionDomain& protection, const ProtectionDomainId protectionIdentity,
                                            AddressSpace& addressSpace,
                                            const AddressSpaceId addressIdentity) const noexcept
    {
        const auto active = m_runLoop.GetActiveProtectionDomainBinding();
        return active.HasValue() && (active.GetValue().Storage == &protection) &&
               active.GetValue().Identity.IsEqualTo(protectionIdentity) &&
               (active.GetValue().AddressSpaceStorage == &addressSpace) &&
               active.GetValue().AddressSpaceIdentity.IsEqualTo(addressIdentity);
    }

    [[nodiscard]] bool Activate(Thread& thread, SchedulingContext& scheduling, const ThreadId threadIdentity,
                                const SchedulingContextId schedulingIdentity, bool& queued) noexcept
    {
        const auto enqueued = m_controller.Enqueue(schedulingIdentity, scheduling, threadIdentity, thread);
        if (!enqueued.HasValue())
        {
            return false;
        }
        queued = true;
        ProcessorKernelRunLoopReceipt receipt{};
        ProcessorKernelRunLoopFailure failure{};
        const bool dispatched = m_runLoop.DispatchFromIdle(receipt, failure);
        const bool returnedToIdle = !dispatched &&
                                    (failure.Error == ProcessorKernelRunLoopError::NativeSwitchRejected) &&
                                    (m_runLoop.GetSnapshot().State == ProcessorKernelRunLoopState::Idle);
        const auto removed = m_controller.Remove(threadIdentity);
        queued = !removed.HasValue();
        return returnedToIdle && removed.HasValue();
    }

    [[nodiscard]] static bool HandleIsUsable(CapabilitySpace& capabilities, const CapabilityHandle handle) noexcept
    {
        if (!handle.IsValid())
        {
            return false;
        }
        const auto lookup = capabilities.Lookup(handle, ObjectType::MemoryObject, CapabilityOperation::Inspect);
        return lookup.HasValue();
    }

    [[nodiscard]] static bool HandleBelongsOnlyTo(CapabilitySpace& owner, CapabilitySpace& foreign,
                                                  const CapabilityHandle handle) noexcept
    {
        if (!handle.IsValid())
        {
            return false;
        }
        const auto ownerLookup = owner.Lookup(handle, ObjectType::MemoryObject, CapabilityOperation::Inspect);
        const auto foreignLookup = foreign.Lookup(handle, ObjectType::MemoryObject, CapabilityOperation::Inspect);
        return ownerLookup.HasValue() && !foreignLookup.HasValue() &&
               (foreignLookup.GetError() == CapabilityError::InvalidHandle);
    }

    [[nodiscard]] static bool ClosePossible(CapabilitySpace& capabilities, CapabilityHandle& handle) noexcept
    {
        if (!handle.IsValid())
        {
            return true;
        }
        const CapabilityError closed = capabilities.Close(handle);
        handle = CapabilityHandle{};
        return (closed == CapabilityError::Ok) || (closed == CapabilityError::InvalidHandle);
    }

    [[nodiscard]] bool DestroyIfRegistered(ObjectId& object, IKernelObject& expected) noexcept
    {
        if (!object.IsValid())
        {
            return true;
        }
        if (m_issuer.DestroyObject(object) != ObjectRegistryError::Ok)
        {
            return false;
        }
        const auto collected = m_issuer.CollectDestroyedObject();
        if (!collected.HasValue() || (collected.GetValue().Object.GetValue() != object.GetValue()) ||
            (collected.GetValue().Instance != &expected))
        {
            return false;
        }
        object = ObjectId{};
        return true;
    }

    [[nodiscard]] static bool QuiesceProtectionDomain(ProtectionDomain& protection) noexcept
    {
        const ProtectionDomainState state = protection.GetSnapshot().State;
        if ((state == ProtectionDomainState::Uninitialized) || (state == ProtectionDomainState::Quiesced))
        {
            return true;
        }
        return protection.Quiesce() == ProtectionDomainError::None;
    }

    [[nodiscard]] static bool QuiesceAddressSpace(AddressSpace& addressSpace) noexcept
    {
        const AddressSpaceState state = addressSpace.GetSnapshot().State;
        if ((state == AddressSpaceState::Uninitialized) || (state == AddressSpaceState::Quiesced))
        {
            return true;
        }
        return addressSpace.Quiesce() == AddressSpaceError::None;
    }

    [[nodiscard]] static bool QuiesceResourceDomain(ResourceDomain& resource) noexcept
    {
        return !resource.IsReady() || (resource.Quiesce() == ResourceDomainError::None);
    }

    SpinLock m_registryLock;
    Asteria::TestSupport::TestObjectRegistry m_registry;
    RevocationRegistry m_revocations;
    ManualLocalTimer m_timer;
    RootCapabilityIssuer m_issuer;
    CapabilitySpace m_firstCapabilities;
    CapabilitySpace m_secondCapabilities;
    TimerQueue m_timerQueue;
    ProcessorRunController m_controller;
    TestAddressSpacePageTable m_idlePageTable;
    TestAddressSpacePageTable m_firstPageTable;
    TestAddressSpacePageTable m_secondPageTable;
    InitialMappingPlan m_seedPlan;
    AddressSpace m_idleAddressSpace;
    AddressSpace m_firstAddressSpace;
    AddressSpace m_secondAddressSpace;
    ProcessorAddressSpaceController m_addressSpaces;
    FakeKernelContext m_idleContext;
    FakeKernelContext m_firstContext;
    FakeKernelContext m_secondContext;
    ProcessorKernelRunLoop<FakeKernelContext> m_runLoop;
    ResourceDomainLedger m_firstResourceLedger;
    ResourceDomainLedger m_secondResourceLedger;
    ResourceDomain m_firstResource;
    ResourceDomain m_secondResource;
    ProtectionDomain m_firstProtection;
    ProtectionDomain m_secondProtection;
    Thread m_firstThread;
    Thread m_secondThread;
    SchedulingContext m_firstScheduling;
    SchedulingContext m_secondScheduling;
    Endpoint m_requestEndpoint;
    Endpoint m_replyEndpoint;
    TestKernelObject m_attachmentObject;
    TestUserMemoryAccess m_firstUserMemory;
    TestUserMemoryAccess m_secondUserMemory;
    SystemCallScratch m_firstScratch;
    SystemCallScratch m_secondScratch;
    MessageEnvelope m_requestMessage{};
    MessageEnvelope m_replyMessage{};
    MessageEnvelope m_decodedRequest{};
    MessageEnvelope m_decodedReply{};
    EndpointReceiveMetadata m_requestMetadata{};
    EndpointReceiveMetadata m_replyMetadata{};
    UInt8 m_frameBuffer[MessageCodec::MaximumWireSizeBytes]{};
    UInt8 m_deliveryBuffer[IpcDeliveryCodec::MaximumWireSizeBytes]{};
    ResourceDomainObjectId m_firstResourceIdentity{};
    ResourceDomainObjectId m_secondResourceIdentity{};
    AddressSpaceId m_idleAddressSpaceIdentity{};
    AddressSpaceId m_firstAddressSpaceIdentity{};
    AddressSpaceId m_secondAddressSpaceIdentity{};
    ProtectionDomainId m_firstProtectionIdentity{};
    ProtectionDomainId m_secondProtectionIdentity{};
    ThreadId m_firstThreadIdentity{};
    ThreadId m_secondThreadIdentity{};
    SchedulingContextId m_firstSchedulingIdentity{};
    SchedulingContextId m_secondSchedulingIdentity{};
    RevocationDomainId m_revocationDomain{};
    ObjectId m_firstResourceObject{};
    ObjectId m_secondResourceObject{};
    ObjectId m_idleAddressSpaceObject{};
    ObjectId m_firstAddressSpaceObject{};
    ObjectId m_secondAddressSpaceObject{};
    ObjectId m_firstProtectionObject{};
    ObjectId m_secondProtectionObject{};
    ObjectId m_firstThreadObject{};
    ObjectId m_secondThreadObject{};
    ObjectId m_firstSchedulingObject{};
    ObjectId m_secondSchedulingObject{};
    ObjectId m_requestEndpointObject{};
    ObjectId m_replyEndpointObject{};
    ObjectId m_attachmentObjectIdentity{};
    CapabilityHandle m_requestInvokeHandle{};
    CapabilityHandle m_requestReadHandle{};
    CapabilityHandle m_replyInvokeHandle{};
    CapabilityHandle m_replyReadHandle{};
    CapabilityHandle m_originalAttachmentHandle{};
    CapabilityHandle m_requestAttachmentHandle{};
    CapabilityHandle m_replySourceAttachmentHandle{};
    CapabilityHandle m_finalAttachmentHandle{};
    Size m_requestFrameSize{0U};
    Size m_replyFrameSize{0U};
    Size m_firstEntryCalls{0U};
    Size m_secondEntryCalls{0U};
    bool m_initialized{false};
    bool m_addressControllerReady{false};
    bool m_firstSchedulingBound{false};
    bool m_secondSchedulingBound{false};
    bool m_firstRunLoopBound{false};
    bool m_secondRunLoopBound{false};
    bool m_firstQueued{false};
    bool m_secondQueued{false};
    bool m_negativeIngressPassed{false};
    bool m_requestSendPassed{false};
    bool m_requestDeliveryPassed{false};
    bool m_replySendPassed{false};
    bool m_replyDeliveryPassed{false};
    bool m_cleaned{false};
    bool m_cleanupPassed{false};
};

IpcSystemCallIntegrationFixture g_fixture;

void TestIpcSystemCallCorridor(TestContext& runner) noexcept
{
    const bool initialized = g_fixture.Initialize();
    ASTERIA_EXPECT(runner, "two user-capable domains bind distinct capability and address spaces", initialized);
    if (!initialized)
    {
        ASTERIA_EXPECT(runner, "a partially initialized IPC corridor drains exactly", g_fixture.Cleanup());
        return;
    }

    const bool activated = g_fixture.RunExchange();
    ASTERIA_EXPECT(runner, "active ingress rejects foreign memory and the legacy IPC overload atomically", activated,
                   g_fixture.NegativeIngressPassed());
    ASTERIA_EXPECT(runner, "the typed request crosses ingress with exact trusted sender metadata and ownership",
                   g_fixture.RequestSendPassed(), g_fixture.RequestDeliveryPassed());
    ASTERIA_EXPECT(runner, "the typed reply returns through ingress with its capability attachment",
                   g_fixture.ReplySendPassed(), g_fixture.ReplyDeliveryPassed());
    ASTERIA_EXPECT(runner, "both canonical deliveries are consumed once in their exact receiver spaces", activated,
                   g_fixture.FinalStatePassed());
    ASTERIA_EXPECT(runner, "the integrated syscall corridor releases every queue, handle, pin, and object",
                   g_fixture.Cleanup());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"IpcSystemCallIntegrationTests"};
    Asteria::Test::TestIpcSystemCallCorridor(runner);
    return runner.Finish();
}
