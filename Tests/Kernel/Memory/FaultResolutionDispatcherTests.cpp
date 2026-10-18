// SPDX-License-Identifier: Apache-2.0
#include <cstddef>
#include <cstdio>
#include <new>

#include "Kernel/Memory/FaultResolutionDispatcher.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Hal::Memory::AddressSpacePageTableActivationError;
using Hal::Memory::AddressSpacePageTableError;
using Hal::Memory::AddressSpacePageTableMapResult;
using Hal::Memory::AddressSpacePageTableOwnershipError;
using Hal::Memory::IAddressSpacePageTable;
using Hal::Memory::ILocalTlbInvalidator;
using Hal::Memory::ITlbShootdownTransport;
using Hal::Memory::LocalTlbInvalidationError;
using Hal::Memory::TlbShootdownTransportError;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultFrameMappingError;
using Kernel::Memory::FaultFrameMappingRequest;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionClaimToken;
using Kernel::Memory::FaultResolutionDispatcher;
using Kernel::Memory::FaultResolutionDispatcherError;
using Kernel::Memory::FaultResolutionDispatcherState;
using Kernel::Memory::FaultResolutionMapRequest;
using Kernel::Memory::FaultResolutionNativeState;
using Kernel::Memory::FaultToken;
using Kernel::Memory::FaultTokenError;
using Kernel::Memory::FaultTokenState;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameId;
using Kernel::Memory::FrameKind;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectDescriptor;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::MemoryObjectId;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PagerFaultDispatcher;
using Kernel::Memory::PagerFaultDispatcherError;
using Kernel::Memory::PagerFaultDispatcherState;
using Kernel::Memory::PagerFaultDeliveryRequest;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::TlbShootdownBatch;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::TlbShootdownError;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Scheduler::CpuId;
using Kernel::Scheduler::ProcessorScheduler;
using Kernel::Scheduler::SchedulingClass;
using Kernel::Scheduler::SchedulingContext;
using Kernel::Scheduler::SchedulingContextConfiguration;
using Kernel::Scheduler::SchedulingContextError;
using Kernel::Scheduler::SchedulingContextId;
using Kernel::Scheduler::SchedulingCriticality;
using Kernel::Scheduler::Thread;
using Kernel::Scheduler::ThreadBlockToken;
using Kernel::Scheduler::ThreadError;
using Kernel::Scheduler::ThreadId;
using Kernel::Scheduler::ThreadRunDisposition;
using Kernel::Scheduler::ThreadState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

enum class PageTableMode : Foundation::UInt8
{
    Exact,
    WrongStart,
};

class TestPageTable final : public IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Claim(const AddressSpace& owner,
                                                            const InitialMappingPlan&) noexcept override
    {
        if (Owner != nullptr)
        {
            return AddressSpacePageTableOwnershipError::AlreadyOwned;
        }
        Owner = &owner;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Release(const AddressSpace& owner) noexcept override
    {
        if (Owner != &owner)
        {
            return AddressSpacePageTableOwnershipError::InvalidOwner;
        }
        Owner = nullptr;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableActivationError Activate(const AddressSpace& owner, CpuId) noexcept override
    {
        return (Owner == &owner) ? AddressSpacePageTableActivationError::None
                                 : AddressSpacePageTableActivationError::InvalidOwner;
    }

    [[nodiscard]] AddressSpacePageTableError Unmap(const AddressSpaceMappingRecord&,
                                                   AddressSpacePageTableMapResult&) noexcept override
    {
        return AddressSpacePageTableError::UnsupportedConfiguration;
    }

    [[nodiscard]] AddressSpacePageTableError Map(const AddressSpaceMappingRecord& mapping,
                                                 AddressSpacePageTableMapResult& output) noexcept override
    {
        ++MapCalls;
        if (NextError != AddressSpacePageTableError::None)
        {
            if (NextError == AddressSpacePageTableError::CommittedStateUnknown)
            {
                ++InstalledMappings;
            }
            return NextError;
        }

        output = AddressSpacePageTableMapResult{
            .InvalidationStart = (Mode == PageTableMode::Exact)
                                     ? mapping.VirtualStart
                                     : VirtualAddress{mapping.VirtualStart.GetValue() + 0x1000U},
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        ++InstalledMappings;
        return AddressSpacePageTableError::None;
    }

    const AddressSpace* Owner{nullptr};
    AddressSpacePageTableError NextError{AddressSpacePageTableError::None};
    PageTableMode Mode{PageTableMode::Exact};
    Foundation::Size MapCalls{0U};
    Foundation::Size InstalledMappings{0U};
};

class TestLocalTlbInvalidator final : public ILocalTlbInvalidator
{
public:
    [[nodiscard]] LocalTlbInvalidationError Invalidate(const VirtualAddress start,
                                                       const PageCount pages) noexcept override
    {
        ++Calls;
        LastStart = start;
        LastPages = pages;
        return NextError;
    }

    LocalTlbInvalidationError NextError{LocalTlbInvalidationError::None};
    Foundation::Size Calls{0U};
    VirtualAddress LastStart{};
    PageCount LastPages{};
};

class TestTransport final : public ITlbShootdownTransport
{
public:
    [[nodiscard]] TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override
    {
        ++PublishCalls;
        if (NextError != TlbShootdownTransportError::None)
        {
            return NextError;
        }
        Captured = batch;
        HasBatch = true;
        return TlbShootdownTransportError::None;
    }

    TlbShootdownTransportError NextError{TlbShootdownTransportError::None};
    Foundation::Size PublishCalls{0U};
    TlbShootdownBatch Captured{};
    bool HasBatch{false};
};

[[nodiscard]] constexpr SchedulingContextConfiguration MakeSchedulingConfiguration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x0FU,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 1024U,
        .ResidentMemoryPages = 1024U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = 64U,
        .IpcQueueBytes = 64U * 1024U,
        .Threads = 64U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr MappingPermissions ReadWrite() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
}

[[nodiscard]] constexpr MappingPermissions ReadExecute() noexcept
{
    return MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute);
}

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr InitialMapping MakeSeed(const UInt64 virtualStart, const UInt64 physicalStart,
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
    return (plan.AddMapping(MakeSeed(base + 0x1000U, 0x0010'0000U, 2U, ReadExecute(),
                                     InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeed(base + 0x4000U, 0x0010'4000U, 4U, ReadWrite(),
                                     InitialMappingOwner::NucleusStack)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(MakeSeed(base + 0x8000U, 0x0010'8000U, 16U, ReadWrite(), InitialMappingOwner::PageTable)) ==
            InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

[[nodiscard]] constexpr FaultRecord MakeFault(const UInt64 virtualStart) noexcept
{
    return FaultRecord{
        .VirtualAddress = virtualStart + 0x123U,
        .InstructionPointer = 0x0080'0000U,
        .ArchitectureSyndrome = 0xA55AU,
        .Access = FaultAccess::Write,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr FaultFrameMappingRequest
MakeMapping(const UInt64 virtualStart, const PagePermissions permissions = ReadWriteUser()) noexcept
{
    return FaultFrameMappingRequest{
        .VirtualStart = VirtualAddress{virtualStart},
        .Pages = PageCount{2U},
        .MemoryObjectOffsetPages = 0U,
        .FrameOffsetPages = 0U,
        .Permissions = permissions,
        .Reserved = {},
    };
}

class ResolutionFixture final
{
public:
    explicit ResolutionFixture(const UInt64 virtualStart) noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          KernelSpace{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          PagerSpace{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          PagerEndpoint{PagerSpace, ResourceDomainId{9U}}, Scheduler{Registry, Clock, CpuId{0U}}, Domain{Ledger},
          Context{Registry}, VirtualStart{virtualStart}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if (Domain.InitializeRoot(ResourceDomainId{13U}, MakeLimits()).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto domain = Issuer.CreateObject(Domain);
        if (!domain.HasValue())
        {
            return false;
        }
        DomainObject = domain.GetValue();

        if (!BuildSeedPlan(SeedPlan) ||
            (Space.Initialize(ResourceDomainId{10U}, PageTable, SeedPlan) != AddressSpaceError::None))
        {
            return false;
        }
        const auto address = Issuer.CreateObject(Space);
        if (!address.HasValue())
        {
            return false;
        }
        AddressObject = address.GetValue();
        AddressIdentity = AddressSpaceId{Registry, AddressObject};
        if (Space.BindIdentity(Registry, AddressIdentity).Error != AddressSpaceError::None)
        {
            return false;
        }

        const MemoryObjectDescriptor memoryDescriptor{
            .Pages = PageCount{32U},
            .CachePolicy = MemoryCachePolicy::WriteBack,
            .MaximumPermissions = PagePermissions{PagePermission::Read}
                                      .With(PagePermission::Write)
                                      .With(PagePermission::Execute)
                                      .With(PagePermission::User),
            .Commitment = MemoryCommitment::BestEffort,
            .Reserved = {},
        };
        if (Memory.Initialize(Registry, ResourceDomainObjectId{Registry, DomainObject}, Domain, memoryDescriptor)
                .Error != MemoryObjectError::None)
        {
            return false;
        }
        const auto memory = Issuer.CreateObject(Memory);
        if (!memory.HasValue())
        {
            return false;
        }
        MemoryObjectValue = memory.GetValue();
        MemoryIdentity = MemoryObjectId{Registry, MemoryObjectValue};

        const FrameDescriptor frameDescriptor{
            .PhysicalStart = PhysicalAddress{0x0200'0000U},
            .Pages = PageCount{32U},
            .CachePolicy = MemoryCachePolicy::WriteBack,
            .Kind = FrameKind::Ram,
            .Reserved = {},
        };
        if (FrameStorage.Initialize(ResourceDomainId{12U}, frameDescriptor) != FrameError::None)
        {
            return false;
        }
        const auto frame = Issuer.CreateObject(FrameStorage);
        if (!frame.HasValue())
        {
            return false;
        }
        FrameObject = frame.GetValue();
        FrameIdentity = FrameId{Registry, FrameObject};

        if ((Caller.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (Context.Initialize(MakeSchedulingConfiguration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }
        const auto caller = Issuer.CreateObject(Caller);
        const auto context = Issuer.CreateObject(Context);
        if (!caller.HasValue() || !context.HasValue())
        {
            return false;
        }
        CallerObject = caller.GetValue();
        ContextObject = context.GetValue();
        CallerIdentity = ThreadId{Registry, CallerObject};
        ContextIdentity = SchedulingContextId{Registry, ContextObject};
        if ((Context.Bind(CallerIdentity) != SchedulingContextError::None) ||
            (Caller.MakeReady() != ThreadError::None) || (Caller.BeginRun(CpuId{0U}) != ThreadError::None))
        {
            return false;
        }
        const auto blocked = Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!blocked.HasValue())
        {
            return false;
        }
        BlockToken = blocked.GetValue().BlockToken;

        if (Token.Initialize(Registry, ResourceDomainId{7U}, CallerIdentity, AddressIdentity,
                             MakeFault(VirtualStart)) != FaultTokenError::None)
        {
            return false;
        }
        const auto token = Issuer.CreateObject(Token);
        const auto revocation = Revocations.CreateRoot();
        if (!token.HasValue() || !revocation.HasValue())
        {
            return false;
        }
        TokenObject = token.GetValue();
        RevocationDomain = revocation.GetValue();
        const CapabilityRights rights = CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Transfer);
        const auto handle =
            Issuer.Mint(KernelSpace, TokenObject, rights, RevocationDomain, MonotonicTime::Infinite(), 0xFA17U);
        if (!handle.HasValue())
        {
            return false;
        }
        SourceHandle = handle.GetValue();
        return true;
    }

    [[nodiscard]] bool DeliverAndClaim(PagerFaultDispatcher& dispatcher) noexcept
    {
        const auto delivered = dispatcher.Deliver(PagerFaultDeliveryRequest{
            .SourceSpace = &KernelSpace,
            .FaultToken = TokenObject,
            .SchedulingContext = ContextIdentity,
            .BlockToken = BlockToken,
            .Sender = ResourceDomainId{7U},
            .FaultTokenCapability = SourceHandle,
            .Trace = {},
            .TransactionIdentifier = 0x1122'3344'5566'7788ULL,
            .Deadline = MonotonicTime::Infinite(),
            .SupervisionTag = 0U,
        });
        MessageEnvelope message{};
        const auto received = PagerEndpoint.Receive(message);
        if (!delivered.HasValue() || !received.HasValue())
        {
            return false;
        }
        ReceiverHandle = message.Attachments[0U].Handle;
        const auto claim = Token.ClaimResolution();
        if (!claim.HasValue())
        {
            return false;
        }
        ActiveClaim = claim.GetValue().Token;
        return true;
    }

    [[nodiscard]] FaultResolutionMapRequest MakeRequest(const PagePermissions permissions = ReadWriteUser()) noexcept
    {
        return FaultResolutionMapRequest{
            .FaultTokenStorage = &Token,
            .AddressSpaceStorage = &Space,
            .MemoryObjectStorage = &Memory,
            .DeviceRegionStorage = nullptr,
            .FrameStorage = &FrameStorage,
            .FaultTokenIdentity = TokenObject,
            .Claim = ActiveClaim,
            .MemoryObjectIdentity = MemoryIdentity,
            .DeviceRegionIdentity = {},
            .FrameIdentity = FrameIdentity,
            .Mapping = MakeMapping(VirtualStart, permissions),
            .Reserved = {},
        };
    }

    [[nodiscard]] bool Activate(const Foundation::UInt8 cpu) noexcept
    {
        return Space.ActivateProcessor(CpuId{cpu}).HasValue();
    }

    [[nodiscard]] bool InitializeAlternateToken() noexcept
    {
        if (AlternateToken.Initialize(Registry, ResourceDomainId{7U}, CallerIdentity, AddressIdentity,
                                      MakeFault(VirtualStart)) != FaultTokenError::None)
        {
            return false;
        }
        const auto token = Issuer.CreateObject(AlternateToken);
        if (!token.HasValue())
        {
            return false;
        }
        AlternateTokenObject = token.GetValue();
        return true;
    }

    [[nodiscard]] bool Retire() noexcept
    {
        bool ok{true};
        const auto token = Token.GetSnapshot();
        if (token.State == FaultTokenState::Claimed)
        {
            ok = (Token.CancelResolution(ActiveClaim) == FaultTokenError::None) && ok;
        }
        static_cast<void>(PagerEndpoint.DiscardUnread());
        if (ReceiverHandle.IsValid())
        {
            ok = (PagerSpace.Close(ReceiverHandle) == Kernel::Object::CapabilityError::Ok) && ok;
            ReceiverHandle = CapabilityHandle{};
        }

        const auto queue = Scheduler.GetSnapshot().Queue;
        if (queue.ReadyEntries != 0U)
        {
            ok = Scheduler.Remove(CallerIdentity).HasValue() && ok;
        }
        const auto thread = Caller.GetSnapshot();
        if (thread.State == ThreadState::Blocked)
        {
            ok = (Caller.Wake(thread.BlockToken) == ThreadError::None) && ok;
        }
        if (Caller.GetSnapshot().State != ThreadState::Exited)
        {
            ok = (Caller.Terminate(0U) == ThreadError::None) && ok;
        }
        ok = (Context.Unbind(CallerIdentity) == SchedulingContextError::None) && ok;

        const Foundation::ProcessorMask residents = Space.GetSnapshot().ResidentProcessorMask;
        for (Foundation::UInt8 cpu{0U}; cpu < Kernel::Scheduler::MaximumProcessorCount; ++cpu)
        {
            const Foundation::UInt8 bit = static_cast<Foundation::UInt8>(Foundation::UInt8{1U} << cpu);
            if ((residents & bit) != 0U)
            {
                ok = (Space.DetachProcessor(CpuId{cpu}) == AddressSpaceError::None) && ok;
            }
        }
        const AddressSpaceState state = Space.GetSnapshot().State;
        if ((state == AddressSpaceState::Ready) || (state == AddressSpaceState::Faulted))
        {
            ok = (Space.Quiesce() == AddressSpaceError::None) && ok;
        }
        ok = (Memory.Quiesce().Error == MemoryObjectError::None) && ok;
        ok = (Domain.Quiesce() == ResourceDomainError::None) && ok;
        return ok;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace KernelSpace;
    CapabilitySpace PagerSpace;
    Endpoint PagerEndpoint;
    ProcessorScheduler Scheduler;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    TestPageTable PageTable;
    InitialMappingPlan SeedPlan;
    AddressSpace Space;
    MemoryObject Memory;
    BootFrame FrameStorage;
    Thread Caller;
    SchedulingContext Context;
    FaultToken Token;
    FaultToken AlternateToken;
    UInt64 VirtualStart;
    ObjectId DomainObject{};
    ObjectId AddressObject{};
    ObjectId MemoryObjectValue{};
    ObjectId FrameObject{};
    ObjectId CallerObject{};
    ObjectId ContextObject{};
    ObjectId TokenObject{};
    ObjectId AlternateTokenObject{};
    AddressSpaceId AddressIdentity{};
    MemoryObjectId MemoryIdentity{};
    FrameId FrameIdentity{};
    ThreadId CallerIdentity{};
    SchedulingContextId ContextIdentity{};
    ThreadBlockToken BlockToken{};
    RevocationDomainId RevocationDomain{};
    CapabilityHandle SourceHandle{};
    CapabilityHandle ReceiverHandle{};
    FaultResolutionClaimToken ActiveClaim{};
};

class ReadyQueuePressureEntry final
{
public:
    explicit ReadyQueuePressureEntry(ObjectRegistry& registry) noexcept : m_registry{registry}, m_context{registry}
    {
    }

    [[nodiscard]] bool Enqueue(RootCapabilityIssuer& issuer, ProcessorScheduler& scheduler) noexcept
    {
        if ((m_thread.Initialize(ResourceDomainId{21U}) != ThreadError::None) ||
            (m_context.Initialize(MakeSchedulingConfiguration(), ResourceDomainId{21U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }
        const auto thread = issuer.CreateObject(m_thread);
        const auto context = issuer.CreateObject(m_context);
        if (!thread.HasValue() || !context.HasValue())
        {
            return false;
        }
        m_threadIdentity = ThreadId{m_registry, thread.GetValue()};
        m_contextIdentity = SchedulingContextId{m_registry, context.GetValue()};
        if ((m_context.Bind(m_threadIdentity) != SchedulingContextError::None) ||
            (m_thread.MakeReady() != ThreadError::None) ||
            !scheduler.Enqueue(m_contextIdentity, m_context, m_threadIdentity, m_thread).HasValue())
        {
            return false;
        }
        m_isActive = true;
        m_isQueued = true;
        return true;
    }

    [[nodiscard]] bool Retire(ProcessorScheduler& scheduler) noexcept
    {
        if (!m_isActive)
        {
            return true;
        }
        bool ok{true};
        if (m_isQueued)
        {
            ok = scheduler.Remove(m_threadIdentity).HasValue() && ok;
            m_isQueued = false;
        }
        if (m_thread.GetSnapshot().State != ThreadState::Exited)
        {
            ok = (m_thread.Terminate(0U) == ThreadError::None) && ok;
        }
        ok = (m_context.Unbind(m_threadIdentity) == SchedulingContextError::None) && ok;
        m_isActive = false;
        return ok;
    }

private:
    ObjectRegistry& m_registry;
    Thread m_thread;
    SchedulingContext m_context;
    ThreadId m_threadIdentity{};
    SchedulingContextId m_contextIdentity{};
    bool m_isActive{false};
    bool m_isQueued{false};
};

class ReadyQueuePressure final
{
public:
    ReadyQueuePressure(ObjectRegistry& registry, RootCapabilityIssuer& issuer, ProcessorScheduler& scheduler) noexcept
        : m_issuer{issuer}, m_scheduler{scheduler},
          m_entries{
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry},
              ReadyQueuePressureEntry{registry}, ReadyQueuePressureEntry{registry}}
    {
    }

    [[nodiscard]] bool Fill() noexcept
    {
        for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumReadyQueueEntries; ++index)
        {
            if (!m_entries[index].Enqueue(m_issuer, m_scheduler))
            {
                return false;
            }
            ++m_activeEntries;
        }
        return true;
    }

    [[nodiscard]] bool ReleaseOne() noexcept
    {
        if (m_activeEntries == 0U)
        {
            return false;
        }
        if (!m_entries[0U].Retire(m_scheduler))
        {
            return false;
        }
        --m_activeEntries;
        return true;
    }

    [[nodiscard]] bool Retire() noexcept
    {
        bool ok{true};
        for (Foundation::Size index{0U}; index < Kernel::Scheduler::MaximumReadyQueueEntries; ++index)
        {
            ok = m_entries[index].Retire(m_scheduler) && ok;
        }
        m_activeEntries = 0U;
        return ok;
    }

private:
    RootCapabilityIssuer& m_issuer;
    ProcessorScheduler& m_scheduler;
    ReadyQueuePressureEntry m_entries[Kernel::Scheduler::MaximumReadyQueueEntries];
    Foundation::Size m_activeEntries{0U};
};

void TestSuccess(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0060'0000U};
    ASTERIA_EXPECT(runner, "success fixture initializes", fixture.Initialize());
    ASTERIA_EXPECT(runner, "success fixture activates the origin CPU", fixture.Activate(0U));
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "success fixture delivers and claims its exact fault", fixture.DeliverAndClaim(pager));
        TestLocalTlbInvalidator invalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
        const auto resolved = dispatcher.BeginMap(fixture.MakeRequest());
        const auto token = fixture.Token.GetSnapshot();
        const auto thread = fixture.Caller.GetSnapshot();
        const auto queue = fixture.Scheduler.GetSnapshot().Queue;
        ASTERIA_EXPECT(runner, "success commits the mapping before the pager performs the exact wake",
                       resolved.HasValue(), resolved.GetValue().State == FaultResolutionDispatcherState::Completed,
                       resolved.GetValue().NativeState == FaultResolutionNativeState::Committed,
                       !resolved.GetValue().RemoteWorkPublished,
                       resolved.GetValue().Mapping.VirtualStart.GetValue() == fixture.VirtualStart,
                       resolved.GetValue().Shootdown.AcknowledgedProcessorMask == 0x01U,
                       resolved.GetValue().Pager.State == PagerFaultDispatcherState::Completed, invalidator.Calls == 1U,
                       invalidator.LastStart.GetValue() == fixture.VirtualStart,
                       token.State == FaultTokenState::Resolved, thread.State == ThreadState::Ready,
                       queue.ReadyEntries == 1U);
        const auto replay = dispatcher.ContinueMap();
        ASTERIA_EXPECT(runner, "a completed resolution rejects dispatcher replay", !replay.HasValue(),
                       replay.GetError().Error == FaultResolutionDispatcherError::AlreadyCompleted);
    }
    ASTERIA_EXPECT(runner, "success fixture retires cleanly", fixture.Retire());
}

void TestAsyncRemoteSuccess(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0061'0000U};
    ASTERIA_EXPECT(runner, "async success fixture initializes", fixture.Initialize());
    ASTERIA_EXPECT(runner, "async success activates two remote CPUs", fixture.Activate(1U), fixture.Activate(2U));
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "async success delivers and claims its fault", fixture.DeliverAndClaim(pager));
        TestLocalTlbInvalidator originInvalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, originInvalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
        const auto pending = dispatcher.BeginMap(fixture.MakeRequest());
        const bool resetRefused = dispatcher.Reset() == FaultResolutionDispatcherError::AlreadyStarted;
        const bool exactBatch = transport.HasBatch && (transport.Captured.GetCount() == 2U);
        const auto tokenBefore = fixture.Token.GetSnapshot();
        const auto threadBefore = fixture.Caller.GetSnapshot();
        const auto queueBefore = fixture.Scheduler.GetSnapshot().Queue;
        const auto addressBefore = fixture.Space.GetSnapshot();

        bool firstAcknowledged{false};
        bool secondAcknowledged{false};
        if (exactBatch)
        {
            TestLocalTlbInvalidator firstInvalidator;
            TlbShootdownDispatcher firstRemote{transport.Captured.GetWork(0U).GetTargetProcessor(), firstInvalidator};
            firstAcknowledged = firstRemote.HandleRemote(transport.Captured.GetWork(0U)).HasValue();
        }
        const auto afterFirst = dispatcher.ContinueMap();
        const auto tokenAfterFirst = fixture.Token.GetSnapshot();
        const auto threadAfterFirst = fixture.Caller.GetSnapshot();
        const auto queueAfterFirst = fixture.Scheduler.GetSnapshot().Queue;
        if (exactBatch)
        {
            TestLocalTlbInvalidator secondInvalidator;
            TlbShootdownDispatcher secondRemote{transport.Captured.GetWork(1U).GetTargetProcessor(), secondInvalidator};
            secondAcknowledged = secondRemote.HandleRemote(transport.Captured.GetWork(1U)).HasValue();
        }
        const auto completed = dispatcher.ContinueMap();
        const auto tokenCompleted = fixture.Token.GetSnapshot();
        const auto threadCompleted = fixture.Caller.GetSnapshot();
        const auto queueCompleted = fixture.Scheduler.GetSnapshot().Queue;
        const auto replay = dispatcher.ContinueMap();
        const auto queueAfterReplay = fixture.Scheduler.GetSnapshot().Queue;
        const bool resetCompleted = (dispatcher.Reset() == FaultResolutionDispatcherError::None) &&
                                    (dispatcher.GetState() == FaultResolutionDispatcherState::Fresh);
        ASTERIA_EXPECT(
            runner, "async resolution remains blocked through a partial ack and commits one wake after the final ack",
            resetRefused, resetCompleted, pending.HasValue(),
            pending.GetValue().State == FaultResolutionDispatcherState::ShootdownPending,
            pending.GetValue().RemoteWorkPublished, exactBatch,
            transport.Captured.GetWork(0U).GetTargetProcessor().GetValue() == 1U,
            transport.Captured.GetWork(1U).GetTargetProcessor().GetValue() == 2U,
            tokenBefore.State == FaultTokenState::Claimed, threadBefore.State == ThreadState::Blocked,
            queueBefore.ReadyEntries == 0U, addressBefore.MappingCount == 3U, firstAcknowledged, afterFirst.HasValue(),
            afterFirst.GetValue().State == FaultResolutionDispatcherState::ShootdownPending,
            tokenAfterFirst.State == FaultTokenState::Claimed, threadAfterFirst.State == ThreadState::Blocked,
            queueAfterFirst.ReadyEntries == 0U, secondAcknowledged, completed.HasValue(),
            completed.GetValue().State == FaultResolutionDispatcherState::Completed,
            tokenCompleted.State == FaultTokenState::Resolved, threadCompleted.State == ThreadState::Ready,
            queueCompleted.ReadyEntries == 1U, !replay.HasValue(),
            replay.GetError().Error == FaultResolutionDispatcherError::AlreadyCompleted,
            queueAfterReplay.ReadyEntries == 1U, fixture.PageTable.MapCalls == 1U);
    }
    ASTERIA_EXPECT(runner, "async success fixture retires cleanly", fixture.Retire());
}

void TestClaimGenerationRejections(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0062'0000U};
    ASTERIA_EXPECT(runner, "claim fixture initializes", fixture.Initialize());
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "claim fixture delivers and claims its exact fault", fixture.DeliverAndClaim(pager));
        TestLocalTlbInvalidator invalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};

        FaultToken foreignToken;
        const bool foreignReady =
            foreignToken.Initialize(fixture.Registry, ResourceDomainId{7U}, fixture.CallerIdentity,
                                    fixture.AddressIdentity, MakeFault(fixture.VirtualStart)) == FaultTokenError::None;
        const auto foreignClaim = foreignToken.ClaimResolution();
        FaultResolutionMapRequest request = fixture.MakeRequest();
        request.Claim = foreignClaim.HasValue() ? foreignClaim.GetValue().Token : FaultResolutionClaimToken{};
        const auto foreign = dispatcher.BeginMap(request);
        ASTERIA_EXPECT(runner, "a foreign claim cannot cancel the exact active generation", foreignReady,
                       foreignClaim.HasValue(), !foreign.HasValue(),
                       foreign.GetError().Error == FaultResolutionDispatcherError::ClaimRejected,
                       foreign.GetError().Token == FaultTokenError::InvalidClaim, !foreign.GetError().ClaimCancelled,
                       fixture.Token.GetSnapshot().State == FaultTokenState::Claimed);
        if (foreignClaim.HasValue())
        {
            static_cast<void>(foreignToken.CancelResolution(foreignClaim.GetValue().Token));
        }

        const FaultResolutionClaimToken stale = fixture.ActiveClaim;
        const bool cancelled = fixture.Token.CancelResolution(stale) == FaultTokenError::None;
        const auto nextClaim = cancelled
                                   ? fixture.Token.ClaimResolution()
                                   : decltype(fixture.Token.ClaimResolution())::Failure(FaultTokenError::InvalidClaim);
        if (nextClaim.HasValue())
        {
            fixture.ActiveClaim = nextClaim.GetValue().Token;
        }
        request = fixture.MakeRequest();
        request.Claim = stale;
        const auto staleResult = dispatcher.BeginMap(request);
        const bool resolved =
            nextClaim.HasValue() &&
            (fixture.Token.CommitResolution(fixture.ActiveClaim, Kernel::Memory::FaultResolutionKind::Redirect) ==
             FaultTokenError::None);
        request = fixture.MakeRequest();
        const auto replay = dispatcher.BeginMap(request);
        ASTERIA_EXPECT(runner, "stale and replayed claims preserve the newer or resolved generation without mapping",
                       cancelled, nextClaim.HasValue(), !staleResult.HasValue(),
                       staleResult.GetError().Error == FaultResolutionDispatcherError::ClaimRejected,
                       staleResult.GetError().Token == FaultTokenError::InvalidClaim,
                       !staleResult.GetError().ClaimCancelled, resolved, !replay.HasValue(),
                       replay.GetError().Error == FaultResolutionDispatcherError::ClaimRejected,
                       replay.GetError().Token == FaultTokenError::AlreadyResolved, !replay.GetError().ClaimCancelled,
                       fixture.PageTable.MapCalls == 0U);
    }
    ASTERIA_EXPECT(runner, "claim fixture retires cleanly", fixture.Retire());
}

void TestFaultTokenInstanceMismatch(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0062'8000U};
    ASTERIA_EXPECT(runner, "instance fixture initializes", fixture.Initialize());
    ASTERIA_EXPECT(runner, "instance fixture registers a second FaultToken instance",
                   fixture.InitializeAlternateToken());
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "instance fixture delivers and claims its primary token",
                       fixture.DeliverAndClaim(pager));
        TestLocalTlbInvalidator invalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
        FaultResolutionMapRequest request = fixture.MakeRequest();
        request.FaultTokenIdentity = fixture.AlternateTokenObject;
        const auto rejected = dispatcher.BeginMap(request);
        ASTERIA_EXPECT(
            runner, "a correct-type different FaultToken instance cancels only the authenticated primary claim",
            !rejected.HasValue(),
            rejected.GetError().Error == FaultResolutionDispatcherError::FaultTokenInstanceMismatch,
            rejected.GetError().ClaimCancelled, !rejected.GetError().Terminal,
            rejected.GetError().NativeState == FaultResolutionNativeState::Unchanged,
            fixture.Token.GetSnapshot().State == FaultTokenState::Pending,
            fixture.AlternateToken.GetSnapshot().State == FaultTokenState::Pending, fixture.PageTable.MapCalls == 0U);
    }
    ASTERIA_EXPECT(runner, "instance fixture retires cleanly", fixture.Retire());
}

void TestPreflightRollback(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x0063'0000U};
        ASTERIA_EXPECT(runner, "preflight fixture initializes", fixture.Initialize());
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "preflight fixture delivers its fault", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator invalidator;
            TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
            TestTransport transport;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            FaultResolutionMapRequest request = fixture.MakeRequest();
            request.FaultTokenIdentity = fixture.MemoryObjectValue;
            const auto rejected = dispatcher.BeginMap(request);
            ASTERIA_EXPECT(
                runner, "exact-storage preflight rejection cancels only its authenticated claim", !rejected.HasValue(),
                rejected.GetError().Error == FaultResolutionDispatcherError::FaultTokenPinRejected,
                rejected.GetError().Pin == Kernel::Object::ObjectPinError::TypeMismatch,
                rejected.GetError().ClaimCancelled, !rejected.GetError().Terminal,
                fixture.Token.GetSnapshot().State == FaultTokenState::Pending, fixture.PageTable.MapCalls == 0U);
        }
        ASTERIA_EXPECT(runner, "preflight fixture retires cleanly", fixture.Retire());
    }
}

void TestMappingPreparationRollback(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x0064'0000U};
        ASTERIA_EXPECT(runner, "preparation fixture initializes", fixture.Initialize());
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "preparation fixture delivers its fault", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator invalidator;
            TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
            TestTransport transport;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            const PagePermissions supervisorOnly = PagePermissions{PagePermission::Read}.With(PagePermission::Write);
            const auto rejected = dispatcher.BeginMap(fixture.MakeRequest(supervisorOnly));
            ASTERIA_EXPECT(
                runner, "mapping preparation rejection cancels the exact claim without waking", !rejected.HasValue(),
                rejected.GetError().Error == FaultResolutionDispatcherError::MappingPreparationRejected,
                rejected.GetError().Preparation == FaultFrameMappingError::AccessDenied,
                rejected.GetError().ClaimCancelled, !rejected.GetError().Terminal,
                rejected.GetError().NativeState == FaultResolutionNativeState::Unchanged,
                fixture.Token.GetSnapshot().State == FaultTokenState::Pending, fixture.PageTable.MapCalls == 0U,
                fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
        }
        ASTERIA_EXPECT(runner, "preparation fixture retires cleanly", fixture.Retire());
    }
}

void TestBackendPrecommitRollback(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x0066'0000U};
        ASTERIA_EXPECT(runner, "backend rollback fixture initializes", fixture.Initialize());
        fixture.PageTable.NextError = AddressSpacePageTableError::InvalidPlan;
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "backend rollback fixture delivers its fault", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator invalidator;
            TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
            TestTransport transport;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            const auto rejected = dispatcher.BeginMap(fixture.MakeRequest());
            ASTERIA_EXPECT(
                runner, "pre-native backend rejection rolls back every retained mapping reference",
                !rejected.HasValue(), rejected.GetError().Error == FaultResolutionDispatcherError::AddressSpaceRejected,
                rejected.GetError().AddressSpace.Error == AddressSpaceError::PageTableRejected,
                rejected.GetError().ClaimCancelled, !rejected.GetError().Terminal,
                fixture.Token.GetSnapshot().State == FaultTokenState::Pending, fixture.PageTable.MapCalls == 1U,
                fixture.PageTable.InstalledMappings == 0U, fixture.Memory.GetSnapshot().ActiveMappings == 0U,
                fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);
        }
        ASTERIA_EXPECT(runner, "backend rollback fixture retires cleanly", fixture.Retire());
    }
}

void TestAmbiguousNativeCommit(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x0068'0000U};
        ASTERIA_EXPECT(runner, "uncertain fixture initializes", fixture.Initialize());
        fixture.PageTable.NextError = AddressSpacePageTableError::CommittedStateUnknown;
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "uncertain fixture delivers its fault", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator invalidator;
            TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
            TestTransport transport;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            const auto rejected = dispatcher.BeginMap(fixture.MakeRequest());
            ASTERIA_EXPECT(runner, "ambiguous native commit is explicit, terminal, and never misreported as rollback",
                           !rejected.HasValue(), rejected.GetError().Terminal, !rejected.GetError().ClaimCancelled,
                           rejected.GetError().NativeState == FaultResolutionNativeState::StateUncertain,
                           rejected.GetError().AddressSpace.PageTableError ==
                               AddressSpacePageTableError::CommittedStateUnknown,
                           fixture.Token.GetSnapshot().State == FaultTokenState::Claimed,
                           fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted);
        }
        ASTERIA_EXPECT(runner, "uncertain fixture retires cleanly", fixture.Retire());
    }
}

void TestMalformedNativeSuccess(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0069'0000U};
    ASTERIA_EXPECT(runner, "malformed native fixture initializes", fixture.Initialize());
    fixture.PageTable.Mode = PageTableMode::WrongStart;
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "malformed native fixture delivers its fault", fixture.DeliverAndClaim(pager));
        TestLocalTlbInvalidator invalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
        const auto rejected = dispatcher.BeginMap(fixture.MakeRequest());
        ASTERIA_EXPECT(
            runner, "malformed backend success is terminal StateUncertain and retains its exact claim",
            !rejected.HasValue(), rejected.GetError().Error == FaultResolutionDispatcherError::AddressSpaceRejected,
            rejected.GetError().AddressSpace.Error == AddressSpaceError::PageTableProtocolViolation,
            rejected.GetError().Terminal, !rejected.GetError().ClaimCancelled,
            rejected.GetError().NativeState == FaultResolutionNativeState::StateUncertain,
            fixture.PageTable.InstalledMappings == 1U, fixture.Token.GetSnapshot().State == FaultTokenState::Claimed,
            fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted,
            fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
    }
    ASTERIA_EXPECT(runner, "malformed native fixture retires cleanly", fixture.Retire());
}

void TestLocalShootdownFailure(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x006A'0000U};
        ASTERIA_EXPECT(runner, "local failure fixture initializes", fixture.Initialize());
        ASTERIA_EXPECT(runner, "local failure activates the origin CPU", fixture.Activate(0U));
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "local failure delivers", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator invalidator;
            invalidator.NextError = LocalTlbInvalidationError::InvalidRange;
            TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
            TestTransport transport;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            const auto rejected = dispatcher.BeginMap(fixture.MakeRequest());
            ASTERIA_EXPECT(runner, "post-native local invalidation failure is terminal and cannot cancel the claim",
                           !rejected.HasValue(),
                           rejected.GetError().Error == FaultResolutionDispatcherError::ShootdownRejected,
                           rejected.GetError().Shootdown.Error == TlbShootdownError::LocalInvalidationRejected,
                           rejected.GetError().Terminal, !rejected.GetError().ClaimCancelled,
                           rejected.GetError().NativeState == FaultResolutionNativeState::InstalledUnexposed,
                           fixture.Token.GetSnapshot().State == FaultTokenState::Claimed,
                           fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted,
                           fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
        }
        ASTERIA_EXPECT(runner, "local failure fixture retires cleanly", fixture.Retire());
    }
}

void TestTransportFailure(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x006C'0000U};
        ASTERIA_EXPECT(runner, "transport failure fixture initializes", fixture.Initialize());
        ASTERIA_EXPECT(runner, "transport failure activates one remote CPU", fixture.Activate(1U));
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "transport failure delivers", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator invalidator;
            TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
            TestTransport transport;
            transport.NextError = TlbShootdownTransportError::QueueFull;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            const auto rejected = dispatcher.BeginMap(fixture.MakeRequest());
            ASTERIA_EXPECT(
                runner, "all-or-none transport rejection is explicit and terminal after native install",
                !rejected.HasValue(), rejected.GetError().Shootdown.Error == TlbShootdownError::TransportRejected,
                rejected.GetError().Shootdown.TransportError == TlbShootdownTransportError::QueueFull,
                rejected.GetError().Terminal, !rejected.GetError().RemoteWorkPublished,
                !rejected.GetError().ClaimCancelled, fixture.Token.GetSnapshot().State == FaultTokenState::Claimed,
                fixture.Space.GetSnapshot().State == AddressSpaceState::Faulted);
        }
        ASTERIA_EXPECT(runner, "transport failure fixture retires cleanly", fixture.Retire());
    }
}

void TestAcknowledgementFailure(TestContext& runner) noexcept
{
    {
        ResolutionFixture fixture{0x006E'0000U};
        ASTERIA_EXPECT(runner, "ack failure fixture initializes", fixture.Initialize());
        ASTERIA_EXPECT(runner, "ack failure activates one remote CPU", fixture.Activate(1U));
        {
            PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
            ASTERIA_EXPECT(runner, "ack failure delivers", fixture.DeliverAndClaim(pager));
            TestLocalTlbInvalidator originInvalidator;
            TlbShootdownDispatcher shootdown{CpuId{0U}, originInvalidator};
            TestTransport transport;
            FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
            const auto pending = dispatcher.BeginMap(fixture.MakeRequest());
            const auto stillPending = dispatcher.ContinueMap();
            TestLocalTlbInvalidator remoteInvalidator;
            remoteInvalidator.NextError = LocalTlbInvalidationError::RangeTooLarge;
            TlbShootdownDispatcher remote{CpuId{1U}, remoteInvalidator};
            const auto remoteResult = transport.HasBatch
                                          ? remote.HandleRemote(transport.Captured.GetWork(0U))
                                          : decltype(remote.HandleRemote(transport.Captured.GetWork(0U)))::Failure({});
            const auto rejected = dispatcher.ContinueMap();
            ASTERIA_EXPECT(runner, "remote rejection remains pending until joined, then becomes terminal without wake",
                           pending.HasValue(),
                           pending.GetValue().State == FaultResolutionDispatcherState::ShootdownPending,
                           pending.GetValue().RemoteWorkPublished, stillPending.HasValue(),
                           stillPending.GetValue().State == FaultResolutionDispatcherState::ShootdownPending,
                           !remoteResult.HasValue(), !rejected.HasValue(),
                           rejected.GetError().Error == FaultResolutionDispatcherError::MappingCompletionRejected,
                           rejected.GetError().AddressSpace.Error == AddressSpaceError::ShootdownFailed,
                           rejected.GetError().Terminal, rejected.GetError().RemoteWorkPublished,
                           fixture.Token.GetSnapshot().State == FaultTokenState::Claimed,
                           fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
        }
        ASTERIA_EXPECT(runner, "ack failure fixture retires cleanly", fixture.Retire());
    }
}

void TestPagerCompletionFailure(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0070'0000U};
    ASTERIA_EXPECT(runner, "completion failure fixture initializes", fixture.Initialize());
    ASTERIA_EXPECT(runner, "completion failure activates one remote CPU", fixture.Activate(1U));
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "completion failure delivers its fault", fixture.DeliverAndClaim(pager));
        TestLocalTlbInvalidator originInvalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, originInvalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
        const auto pending = dispatcher.BeginMap(fixture.MakeRequest());
        TestLocalTlbInvalidator remoteInvalidator;
        TlbShootdownDispatcher remote{CpuId{1U}, remoteInvalidator};
        const auto acknowledged = transport.HasBatch
                                      ? remote.HandleRemote(transport.Captured.GetWork(0U))
                                      : decltype(remote.HandleRemote(transport.Captured.GetWork(0U)))::Failure({});
        const ThreadError externalWake = fixture.Caller.Wake(fixture.BlockToken);
        const auto rejected = dispatcher.ContinueMap();
        ASTERIA_EXPECT(
            runner, "post-mapping pager correlation failure is terminal and never performs a second wake",
            pending.HasValue(), acknowledged.HasValue(), externalWake == ThreadError::None, !rejected.HasValue(),
            rejected.GetError().Error == FaultResolutionDispatcherError::PagerCompletionRejected,
            rejected.GetError().Pager.Error == PagerFaultDispatcherError::CorrelatedStateChanged,
            rejected.GetError().Terminal, rejected.GetError().NativeState == FaultResolutionNativeState::Committed,
            fixture.Space.GetSnapshot().MappingCount == 4U,
            fixture.Token.GetSnapshot().State == FaultTokenState::Claimed,
            pager.GetSnapshot().State == PagerFaultDispatcherState::Faulted,
            fixture.Scheduler.GetSnapshot().Queue.ReadyEntries == 0U);
    }
    ASTERIA_EXPECT(runner, "completion failure fixture retires cleanly", fixture.Retire());
}

void TestRetryablePagerQueuePressure(TestContext& runner) noexcept
{
    ResolutionFixture fixture{0x0072'0000U};
    ASTERIA_EXPECT(runner, "queue pressure fixture initializes", fixture.Initialize());
    {
        PagerFaultDispatcher pager{fixture.Registry, fixture.PagerEndpoint, fixture.Scheduler.GetWakeFacade()};
        ASTERIA_EXPECT(runner, "queue pressure fixture delivers its fault", fixture.DeliverAndClaim(pager));
        ReadyQueuePressure pressure{fixture.Registry, fixture.Issuer, fixture.Scheduler};
        ASTERIA_EXPECT(runner, "queue pressure fills every bounded ready slot", pressure.Fill(),
                       fixture.Scheduler.GetSnapshot().Queue.ReadyEntries ==
                           Kernel::Scheduler::MaximumReadyQueueEntries);
        TestLocalTlbInvalidator invalidator;
        TlbShootdownDispatcher shootdown{CpuId{0U}, invalidator};
        TestTransport transport;
        FaultResolutionDispatcher dispatcher{fixture.Registry, shootdown, transport, pager};
        const auto pressured = dispatcher.BeginMap(fixture.MakeRequest());
        const auto mappingAfterPressure = fixture.Space.GetSnapshot();
        const auto tokenAfterPressure = fixture.Token.GetSnapshot();
        const auto threadAfterPressure = fixture.Caller.GetSnapshot();
        const bool released = pressure.ReleaseOne();
        const auto completed = dispatcher.ContinueMap();
        const auto tokenCompleted = fixture.Token.GetSnapshot();
        const auto threadCompleted = fixture.Caller.GetSnapshot();
        const auto queueCompleted = fixture.Scheduler.GetSnapshot().Queue;
        ASTERIA_EXPECT(runner, "post-commit queue pressure is retryable and ContinueMap wakes without remapping",
                       !pressured.HasValue(),
                       pressured.GetError().Error == FaultResolutionDispatcherError::PagerCompletionRejected,
                       pressured.GetError().Pager.Error == PagerFaultDispatcherError::QueueReservationRejected,
                       !pressured.GetError().Terminal,
                       pressured.GetError().State == FaultResolutionDispatcherState::MappingCommitted,
                       pressured.GetError().NativeState == FaultResolutionNativeState::Committed,
                       mappingAfterPressure.MappingCount == 4U, tokenAfterPressure.State == FaultTokenState::Claimed,
                       threadAfterPressure.State == ThreadState::Blocked, fixture.PageTable.MapCalls == 1U, released,
                       completed.HasValue(), completed.GetValue().State == FaultResolutionDispatcherState::Completed,
                       tokenCompleted.State == FaultTokenState::Resolved, threadCompleted.State == ThreadState::Ready,
                       queueCompleted.ReadyEntries == Kernel::Scheduler::MaximumReadyQueueEntries,
                       fixture.PageTable.MapCalls == 1U);
        ASTERIA_EXPECT(runner, "queue pressure retires helper entries without removing the resolved caller",
                       pressure.Retire(), fixture.Scheduler.GetSnapshot().Queue.ReadyEntries == 1U);
    }
    ASTERIA_EXPECT(runner, "queue pressure fixture retires cleanly", fixture.Retire());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FaultResolutionDispatcherTests"};
    Asteria::Test::TestSuccess(runner);
    Asteria::Test::TestAsyncRemoteSuccess(runner);
    Asteria::Test::TestClaimGenerationRejections(runner);
    Asteria::Test::TestFaultTokenInstanceMismatch(runner);
    Asteria::Test::TestPreflightRollback(runner);
    Asteria::Test::TestMappingPreparationRollback(runner);
    Asteria::Test::TestBackendPrecommitRollback(runner);
    Asteria::Test::TestAmbiguousNativeCommit(runner);
    Asteria::Test::TestMalformedNativeSuccess(runner);
    Asteria::Test::TestLocalShootdownFailure(runner);
    Asteria::Test::TestTransportFailure(runner);
    Asteria::Test::TestAcknowledgementFailure(runner);
    Asteria::Test::TestPagerCompletionFailure(runner);
    Asteria::Test::TestRetryablePagerQueuePressure(runner);
    return runner.Finish();
}
