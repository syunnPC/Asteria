// SPDX-License-Identifier: Apache-2.0
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <atomic>
#include <thread>
#include "Platform/Synchronization/ProcessorContext.hpp"

#include "Abi/SystemCallAbi.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/PagerChannel.hpp"
#include "Kernel/Memory/FaultChannelPool.hpp"
#include "Kernel/System/ObjectManager.hpp"
#include "Kernel/Protection/ProtectionDomainPool.hpp"
#include "Kernel/Object/CapabilitySpacePool.hpp"
#include "Kernel/Memory/PagerProtocol.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Scheduler/ProcessorScheduler.hpp"

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
using Kernel::Ipc::Notification;
using Kernel::Ipc::NotificationError;
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::FaultAccess;
using Kernel::Memory::FaultCause;
using Kernel::Memory::FaultFrameMappingRequest;
using Kernel::Memory::FaultPrivilege;
using Kernel::Memory::FaultRecord;
using Kernel::Memory::FaultResolutionKind;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
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
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PagerChannel;
using Kernel::Memory::PagerChannelConfiguration;
using Kernel::Memory::PagerChannelError;
using Kernel::Memory::PagerChannelState;
using Kernel::Memory::PagerFaultSource;
using Kernel::Memory::PagerResolutionError;
using Kernel::Memory::PagerResolutionResult;
using Kernel::Memory::PagerResolveRequest;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::TlbShootdownBatch;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
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

constexpr UInt64 LazyStart{0x0000'0000'4000'0000ULL};

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
        output = AddressSpacePageTableMapResult{
            .InvalidationStart = mapping.VirtualStart,
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        LastStart = mapping.VirtualStart;
        LastPermissions = mapping.Permissions;
        ++InstalledMappings;
        return AddressSpacePageTableError::None;
    }

    const AddressSpace* Owner{nullptr};
    VirtualAddress LastStart{};
    PagePermissions LastPermissions{};
    Foundation::Size InstalledMappings{0U};
};

class TestLocalTlbInvalidator final : public ILocalTlbInvalidator
{
public:
    [[nodiscard]] LocalTlbInvalidationError Invalidate(const VirtualAddress, const PageCount) noexcept override
    {
        return LocalTlbInvalidationError::None;
    }
};

class TestTransport final : public ITlbShootdownTransport
{
public:
    [[nodiscard]] TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override
    {
        Batch = batch;
        ++PublishCalls;
        return TlbShootdownTransportError::None;
    }

    TlbShootdownBatch Batch{};
    Foundation::Size PublishCalls{0U};
};

class TestChannelProgress final : public Kernel::Memory::IPagerShootdownProgress
{
public:
    void Wait() noexcept override
    {
        Unlocked = Platform::Synchronization::HeldLockDepth() == 0U;
        Entered.store(true, std::memory_order_release);
        for (Foundation::Size spin{0U}; spin < 1'000'000U && !Released.load(std::memory_order_acquire); ++spin)
        {
            std::this_thread::yield();
        }
    }
    std::atomic<bool> Entered{false};
    std::atomic<bool> Released{false};
    bool Unlocked{false};
};

[[nodiscard]] constexpr SchedulingContextConfiguration Configuration() noexcept
{
    return SchedulingContextConfiguration{
        .BudgetTicks = 20U,
        .PeriodTicks = 100U,
        .RelativeDeadlineTicks = 80U,
        .Weight = 10U,
        .Priority = 8U,
        .Criticality = SchedulingCriticality::Normal,
        .CpuAffinityMask = 0x01U,
        .Class = SchedulingClass::DeadlineSporadic,
        .Reserved = 0U,
    };
}

[[nodiscard]] constexpr ResourceDomainLimits Limits() noexcept
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

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] constexpr InitialMapping Seed(const UInt64 offset, const UInt64 physicalStart, const UInt64 pages,
                                            const MappingPermissions permissions,
                                            const InitialMappingOwner owner) noexcept
{
    return InitialMapping{
        .VirtualStart = VirtualAddress{0xFFFF'8000'0000'0000ULL + offset},
        .PhysicalStart = PhysicalAddress{physicalStart},
        .Pages = PageCount{pages},
        .Permissions = permissions,
        .CachePolicy = MemoryCachePolicy::WriteBack,
        .Owner = owner,
        .Reserved = 0U,
    };
}

/// The nucleus code, stack and page-table mappings every root shares.
[[nodiscard]] bool BuildSeedPlan(InitialMappingPlan& plan) noexcept
{
    const MappingPermissions readWrite = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
    return (plan.AddMapping(Seed(0x1000U, 0x0010'0000U, 2U,
                                 MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
                                 InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
           (plan.AddMapping(Seed(0x4000U, 0x0010'4000U, 4U, readWrite, InitialMappingOwner::NucleusStack)) ==
            InitialMappingPlanError::None) &&
           (plan.AddMapping(Seed(0x8000U, 0x0010'8000U, 16U, readWrite, InitialMappingOwner::PageTable)) ==
            InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

[[nodiscard]] constexpr FaultRecord LazyFault(const UInt64 offset) noexcept
{
    return FaultRecord{
        .VirtualAddress = LazyStart + offset,
        .InstructionPointer = 0x0040'1000U,
        .ArchitectureSyndrome = 0x6U,
        .Access = FaultAccess::Write,
        .Cause = FaultCause::MissingMapping,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
}

class ChannelFixture final
{
public:
    ChannelFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          KernelSpace{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          PagerSpace{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          PagerEndpoint{PagerSpace, ResourceDomainId{9U}}, Scheduler{Registry, Clock, CpuId{0U}}, Domain{Ledger},
          Context{Registry}, SecondContext{Registry}, Shootdown{CpuId{0U}, Invalidator},
          Channel{Registry, Issuer, PagerEndpoint, Scheduler.GetWakeFacade(), Shootdown, Transport}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if (Domain.InitializeRoot(ResourceDomainId{13U}, Limits()).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto domain = Issuer.CreateObject(Domain);
        if (!domain.HasValue() || !InitializeSpace(Space, PageTable, SeedPlan, SpaceIdentity) ||
            !InitializeSpace(OtherSpace, OtherPageTable, OtherSeedPlan, OtherIdentity))
        {
            return false;
        }
        DomainIdentity = domain.GetValue();
        const ResourceDomainObjectId domainIdentity{Registry, domain.GetValue()};
        if (Memory.Initialize(Registry, domainIdentity, Domain,
                              MemoryObjectDescriptor{
                                  .Pages = PageCount{8U},
                                  .CachePolicy = MemoryCachePolicy::WriteBack,
                                  .MaximumPermissions = ReadWriteUser(),
                                  .Commitment = MemoryCommitment::Pinned,
                                  .Reserved = {},
                              })
                    .Error != MemoryObjectError::None ||
            FrameStorage.Initialize(ResourceDomainId{12U},
                                    FrameDescriptor{
                                        .PhysicalStart = PhysicalAddress{0x0200'0000U},
                                        .Pages = PageCount{8U},
                                        .CachePolicy = MemoryCachePolicy::WriteBack,
                                        .Kind = FrameKind::Ram,
                                        .Reserved = {},
                                    }) != FrameError::None ||
            Readiness.Initialize(ResourceDomainId{9U}) != NotificationError::None ||
            Caller.Initialize(ResourceDomainId{7U}) != ThreadError::None ||
            Context.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
                SchedulingContextError::None)
        {
            return false;
        }
        const auto memory = Issuer.CreateObject(Memory);
        const auto frame = Issuer.CreateObject(FrameStorage);
        const auto readiness = Issuer.CreateObject(Readiness);
        const auto caller = Issuer.CreateObject(Caller);
        const auto context = Issuer.CreateObject(Context);
        const auto tokenRoot = Revocations.CreateRoot();
        const auto pagerRoot = Revocations.CreateRoot();
        if (!memory.HasValue() || !frame.HasValue() || !readiness.HasValue() || !caller.HasValue() ||
            !context.HasValue() || !tokenRoot.HasValue() || !pagerRoot.HasValue())
        {
            return false;
        }
        CallerIdentity = ThreadId{Registry, caller.GetValue()};
        ContextIdentity = SchedulingContextId{Registry, context.GetValue()};
        const CapabilityRights map{CapabilityRight::Map};
        const auto spaceHandle = Mint(SpaceIdentity.GetObjectId(), map, pagerRoot.GetValue());
        const auto otherHandle = Mint(OtherIdentity.GetObjectId(), map, pagerRoot.GetValue());
        const auto memoryHandle = Mint(memory.GetValue(), map, pagerRoot.GetValue());
        const auto memoryInspect =
            Mint(memory.GetValue(), CapabilityRights{CapabilityRight::Inspect}, pagerRoot.GetValue());
        const auto frameHandle = Mint(frame.GetValue(), map.With(CapabilityRight::Write).With(CapabilityRight::Derive),
                                      pagerRoot.GetValue());
        if (!spaceHandle.HasValue() || !otherHandle.HasValue() || !memoryHandle.HasValue() ||
            !memoryInspect.HasValue() || !frameHandle.HasValue())
        {
            return false;
        }
        SpaceHandle = spaceHandle.GetValue();
        OtherSpaceHandle = otherHandle.GetValue();
        MemoryHandle = memoryHandle.GetValue();
        MemoryInspectHandle = memoryInspect.GetValue();
        FrameHandle = frameHandle.GetValue();
        return (Context.Bind(CallerIdentity) == SchedulingContextError::None) &&
               (Caller.MakeReady() == ThreadError::None) &&
               (Channel.Configure(PagerChannelConfiguration{
                    .TokenSpace = &KernelSpace,
                    .Readiness = &Readiness,
                    .ShootdownProgress = &Progress,
                    .ChargeOwner = ResourceDomainId{7U},
                    .Sender = ResourceDomainId{7U},
                    .RevocationDomain = tokenRoot.GetValue(),
                }) == PagerChannelError::None);
    }

    /// A second thread of the same space with its own SchedulingContext.
    [[nodiscard]] bool InitializeSecondCaller() noexcept
    {
        if ((Second.Initialize(ResourceDomainId{7U}) != ThreadError::None) ||
            (SecondContext.Initialize(Configuration(), ResourceDomainId{7U}, MonotonicTime{0U}) !=
             SchedulingContextError::None))
        {
            return false;
        }
        const auto thread = Issuer.CreateObject(Second);
        const auto context = Issuer.CreateObject(SecondContext);
        if (!thread.HasValue() || !context.HasValue())
        {
            return false;
        }
        SecondIdentity = ThreadId{Registry, thread.GetValue()};
        SecondContextIdentity = SchedulingContextId{Registry, context.GetValue()};
        return (SecondContext.Bind(SecondIdentity) == SchedulingContextError::None) &&
               (Second.MakeReady() == ThreadError::None);
    }

    /// Runs the caller, faults it at the lazy page and blocks it; the pager
    /// then receives the delivered token. Returns the pager's token handle.
    [[nodiscard]] CapabilityHandle FaultAndReceive(const UInt64 offset, const UInt64 now) noexcept
    {
        return FaultAndReceive(LazyFault(offset), now, Caller, CallerIdentity, Context, ContextIdentity);
    }

    [[nodiscard]] CapabilityHandle FaultAndReceive(const UInt64 offset, const UInt64 now, Thread& thread,
                                                   const ThreadId threadIdentity, SchedulingContext& context,
                                                   const SchedulingContextId contextIdentity) noexcept
    {
        return FaultAndReceive(LazyFault(offset), now, thread, threadIdentity, context, contextIdentity);
    }

    [[nodiscard]] CapabilityHandle FaultAndReceive(const FaultRecord& fault, const UInt64 now, Thread& thread,
                                                   const ThreadId threadIdentity, SchedulingContext& context,
                                                   const SchedulingContextId contextIdentity) noexcept
    {
        if ((thread.BeginRun(CpuId{0U}) != ThreadError::None) ||
            !context.StartRun(threadIdentity, CpuId{0U}, MonotonicTime{now}).HasValue() ||
            (ActiveChannel->PrepareFault(PagerFaultSource{
                 .Thread = threadIdentity,
                 .AddressSpace = SpaceIdentity,
                 .SchedulingContext = contextIdentity,
                 .Fault = fault,
                 .SupervisionTag = 0U,
             }) != PagerChannelError::None) ||
            !context.EndRun(CpuId{0U}, MonotonicTime{now + 1U}).HasValue())
        {
            return {};
        }
        const auto finished = thread.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        if (!finished.HasValue())
        {
            return {};
        }
        ActiveChannel->CommitBlocked(finished.GetValue().BlockToken);
        MessageEnvelope message{};
        const auto received = ActiveEndpoint->Receive(message);
        return (received.HasValue() && (message.AttachmentCount == 1U)) ? message.Attachments[0U].Handle
                                                                        : CapabilityHandle{};
    }

    [[nodiscard]] PagerResolveRequest Request(const CapabilityHandle token,
                                              const PagePermissions permissions = ReadWriteUser(),
                                              const UInt64 virtualStart = LazyStart,
                                              const UInt64 objectPage = 0U) const noexcept
    {
        return PagerResolveRequest{
            .FaultToken = token,
            .AddressSpace = SpaceHandle,
            .MemoryObject = MemoryHandle,
            .Frame = FrameHandle,
            .Mapping =
                FaultFrameMappingRequest{
                    .VirtualStart = VirtualAddress{virtualStart},
                    .Pages = PageCount{1U},
                    .MemoryObjectOffsetPages = objectPage,
                    .FrameOffsetPages = objectPage,
                    .Permissions = permissions,
                    .Reserved = {},
                },
            .Kind = FaultResolutionKind::Map,
            .Reserved = {},
        };
    }

    /// Takes the woken caller back off the ready queue so it may run again.
    [[nodiscard]] bool TakeReadyCaller() noexcept
    {
        return Scheduler.Remove(CallerIdentity).HasValue() && (Caller.GetSnapshot().State == ThreadState::Ready);
    }

    [[nodiscard]] bool InitializeSpace(AddressSpace& space, TestPageTable& table, InitialMappingPlan& plan,
                                       AddressSpaceId& identity) noexcept
    {
        if (!BuildSeedPlan(plan) || (space.Initialize(ResourceDomainId{10U}, table, plan) != AddressSpaceError::None))
        {
            return false;
        }
        const auto object = Issuer.CreateObject(space);
        if (!object.HasValue())
        {
            return false;
        }
        identity = AddressSpaceId{Registry, object.GetValue()};
        return space.BindIdentity(Registry, identity).Error == AddressSpaceError::None;
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Mint(const ObjectId object, const CapabilityRights rights, const RevocationDomainId domain) noexcept
    {
        return Issuer.Mint(PagerSpace, object, rights, domain, MonotonicTime::Infinite(), 0x5EA1U);
    }

    ObjectId DomainIdentity{};
    PagerChannel* ActiveChannel{&Channel};
    Endpoint* ActiveEndpoint{&PagerEndpoint};
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
    InitialMappingPlan SeedPlan;
    InitialMappingPlan OtherSeedPlan;
    TestPageTable PageTable;
    TestPageTable OtherPageTable;
    AddressSpace Space;
    AddressSpace OtherSpace;
    AddressSpaceId SpaceIdentity{};
    AddressSpaceId OtherIdentity{};
    MemoryObject Memory;
    BootFrame FrameStorage;
    Notification Readiness;
    Thread Caller;
    SchedulingContext Context;
    ThreadId CallerIdentity{};
    SchedulingContextId ContextIdentity{};
    Thread Second;
    SchedulingContext SecondContext;
    ThreadId SecondIdentity{};
    SchedulingContextId SecondContextIdentity{};
    TestLocalTlbInvalidator Invalidator;
    TlbShootdownDispatcher Shootdown;
    TestTransport Transport;
    PagerChannel Channel;
    TestChannelProgress Progress{};
    CapabilityHandle SpaceHandle{};
    CapabilityHandle OtherSpaceHandle{};
    CapabilityHandle MemoryHandle{};
    CapabilityHandle MemoryInspectHandle{};
    CapabilityHandle FrameHandle{};
};

void TestConcurrentPrepare(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Caller.BeginRun(CpuId{0U}) == ThreadError::None &&
                       fixture.Context.StartRun(fixture.CallerIdentity, CpuId{0U}, MonotonicTime{10U}).HasValue();
    const PagerFaultSource source{fixture.CallerIdentity, fixture.SpaceIdentity, fixture.ContextIdentity,
                                  LazyFault(0x234U), 0U};
    std::atomic<unsigned> arrived{0U};
    std::atomic<bool> go{false};
    PagerChannelError results[4]{};
    std::thread workers[4];
    for (unsigned cpu{0U}; cpu < 4U; ++cpu)
    {
        workers[cpu] = std::thread(
            [&, cpu]
            {
                Platform::Synchronization::SetHostedProcessor(CpuId{static_cast<Foundation::UInt8>(cpu)});
                arrived.fetch_add(1U, std::memory_order_release);
                while (!go.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
                results[cpu] = fixture.Channel.PrepareFault(source);
            });
    }
    while (arrived.load(std::memory_order_acquire) != 4U)
    {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    unsigned winners{0U};
    unsigned busy{0U};
    for (unsigned cpu{0U}; cpu < 4U; ++cpu)
    {
        workers[cpu].join();
        winners += results[cpu] == PagerChannelError::None ? 1U : 0U;
        busy += results[cpu] == PagerChannelError::Busy ? 1U : 0U;
    }
    const bool ended = fixture.Context.EndRun(CpuId{0U}, MonotonicTime{11U}).HasValue();
    const auto blocked = fixture.Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
    if (blocked.HasValue())
    {
        fixture.Channel.CommitBlocked(blocked.GetValue().BlockToken);
    }
    ASTERIA_EXPECT(runner, "four CPUs claim exactly one fault and defer three contenders", ready, ended,
                   blocked.HasValue(), winners == 1U, busy == 3U,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::FaultPending);
}

void TestSnapshotDoesNotLoseCommit(TestContext& runner) noexcept
{
    bool valid{true};
    static ChannelFixture fixtures[16U];
    for (unsigned round{0U}; round < 16U; ++round)
    {
        ChannelFixture& fixture = fixtures[round];
        valid = valid && fixture.Initialize() && fixture.Caller.BeginRun(CpuId{0U}) == ThreadError::None &&
                fixture.Context.StartRun(fixture.CallerIdentity, CpuId{0U}, MonotonicTime{10U}).HasValue() &&
                fixture.Channel.PrepareFault({fixture.CallerIdentity, fixture.SpaceIdentity, fixture.ContextIdentity,
                                              LazyFault(0x234U), 0U}) == PagerChannelError::None &&
                fixture.Context.EndRun(CpuId{0U}, MonotonicTime{11U}).HasValue();
        const auto blocked = fixture.Caller.FinishRun(CpuId{0U}, ThreadRunDisposition::Blocked);
        std::atomic<unsigned> reading{0U};
        std::atomic<bool> done{false};
        std::thread readers[3];
        for (unsigned cpu{0U}; cpu < 3U; ++cpu)
        {
            readers[cpu] = std::thread(
                [&, cpu]
                {
                    Platform::Synchronization::SetHostedProcessor(CpuId{static_cast<Foundation::UInt8>(cpu + 1U)});
                    for (unsigned pass{0U}; pass < 100'000U && !done.load(std::memory_order_acquire); ++pass)
                    {
                        static_cast<void>(fixture.Channel.GetSnapshot());
                        reading.fetch_add(1U, std::memory_order_release);
                    }
                });
        }
        while (reading.load(std::memory_order_acquire) < 64U)
        {
            std::this_thread::yield();
        }
        if (blocked.HasValue())
        {
            fixture.Channel.CommitBlocked(blocked.GetValue().BlockToken);
        }
        done.store(true, std::memory_order_release);
        for (auto& reader : readers)
        {
            reader.join();
        }
        const auto committed = fixture.Channel.GetSnapshot();
        valid = valid && blocked.HasValue() &&
                committed.Ingress.State == Kernel::Memory::PageFaultIngressState::Delivered &&
                committed.Deliveries == 1U;
    }
    ASTERIA_EXPECT(runner, "diagnostic readers cannot lose the faulter's one block commit", valid);
}

void TestResolvingDoesNotBlockContender(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Space.ActivateProcessor(CpuId{1U}).HasValue();
    const auto token = fixture.FaultAndReceive(0x234U, 10U);
    bool contenderBusy{false};
    bool drained{false};
    bool sawResolving{false};
    std::thread peer(
        [&]
        {
            Platform::Synchronization::SetHostedProcessor(CpuId{1U});
            while (!fixture.Progress.Entered.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            sawResolving = fixture.Channel.GetSnapshot().State == PagerChannelState::Resolving;
            contenderBusy = true;
            for (unsigned retry{0U}; retry < 1024U; ++retry)
            {
                contenderBusy = contenderBusy && fixture.Channel.PrepareFault({}) == PagerChannelError::Busy;
                fixture.Channel.Retry();
                fixture.Channel.CommitBlocked({});
            }
            TestLocalTlbInvalidator invalidator;
            TlbShootdownDispatcher remote{CpuId{1U}, invalidator};
            drained = fixture.Transport.Batch.GetCount() == 1U &&
                      remote.HandleRemote(fixture.Transport.Batch.GetWork(0U)).HasValue();
            fixture.Progress.Released.store(true, std::memory_order_release);
        });
    Platform::Synchronization::SetHostedProcessor(CpuId{0U});
    const auto result = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    peer.join();
    ASTERIA_EXPECT(runner, "Resolving owns the fields while a contender retries and acknowledges the initiator", ready,
                   token.IsValid(), contenderBusy, drained, sawResolving, fixture.Progress.Unlocked,
                   result.Error == PagerResolutionError::None,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::Idle);
}

// Desktop session: a refused write does not claim the pending fault token; the same attenuated
// Frame can then resolve a read fault.
void TestFrameWriteAuthority(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize();
    const auto copy = fixture.PagerSpace.Derive(fixture.FrameHandle, CapabilityRights{CapabilityRight::Map},
                                                MonotonicTime::Infinite());
    auto fault = LazyFault(0U);
    fault.Access = FaultAccess::Read;
    const auto token = fixture.FaultAndReceive(fault, 10U, fixture.Caller, fixture.CallerIdentity, fixture.Context,
                                               fixture.ContextIdentity);
    auto request = fixture.Request(token);
    request.Frame = copy.HasValue() ? copy.GetValue() : CapabilityHandle{};
    const auto denied = fixture.Channel.Resolve(fixture.PagerSpace, request);
    ASTERIA_EXPECT(runner, "FaultResolve refuses Write without consuming the read fault", ready, copy.HasValue(),
                   token.IsValid(), denied.Error == PagerResolutionError::CapabilityRejected,
                   denied.Capability == CapabilityError::RightsDenied, fixture.PageTable.InstalledMappings == 0U,
                   fixture.Caller.GetSnapshot().State == ThreadState::Blocked,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::FaultPending);
    const auto writeOnly = fixture.PagerSpace.Derive(fixture.FrameHandle, CapabilityRights{CapabilityRight::Write},
                                                     MonotonicTime::Infinite());
    auto withoutMap = request;
    withoutMap.Frame = writeOnly.HasValue() ? writeOnly.GetValue() : CapabilityHandle{};
    const auto noMap = fixture.Channel.Resolve(fixture.PagerSpace, withoutMap);
    ASTERIA_EXPECT(runner, "FaultResolve requires Map even when the Frame carries Write", writeOnly.HasValue(),
                   noMap.Error == PagerResolutionError::CapabilityRejected,
                   noMap.Capability == CapabilityError::RightsDenied,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::FaultPending);
    request.Mapping.Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::User);
    const auto read = fixture.Channel.Resolve(fixture.PagerSpace, request);
    ASTERIA_EXPECT(runner, "FaultResolve accepts Map alone for a read-only mapping",
                   read.Error == PagerResolutionError::None, fixture.PageTable.InstalledMappings == 1U,
                   !fixture.PageTable.LastPermissions.Contains(PagePermission::Write),
                   fixture.Caller.GetSnapshot().State == ThreadState::Ready,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::Idle);
    const bool taken = fixture.TakeReadyCaller();
    const auto second = fixture.FaultAndReceive(0x1000U, 20U);
    const auto writer =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(second, ReadWriteUser(), LazyStart + 0x1000U, 1U));
    ASTERIA_EXPECT(runner, "FaultResolve accepts Map and Write for a writable mapping", taken, second.IsValid(),
                   writer.Error == PagerResolutionError::None, fixture.PageTable.InstalledMappings == 2U,
                   fixture.PageTable.LastPermissions.Contains(PagePermission::Write));
}

void TestRoundTrip(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize();
    const CapabilityHandle token = fixture.FaultAndReceive(0x234U, 10U);
    const auto blocked = fixture.Caller.GetSnapshot().State;
    const auto signalled = fixture.Readiness.Claim();
    const bool claimed = signalled.HasValue() && fixture.Readiness.CommitClaim(signalled.GetValue()).HasValue();
    const PagerResolutionResult resolved = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    const auto snapshot = fixture.Channel.GetSnapshot();
    const auto stale = fixture.PagerSpace.Lookup(token, ObjectType::FaultToken, CapabilityOperation::Invoke);
    ASTERIA_EXPECT(runner, "a delivered fault resolves, maps the page, wakes the thread and retires the token", ready,
                   token.IsValid(), blocked == ThreadState::Blocked, claimed,
                   resolved.Error == PagerResolutionError::None, fixture.PageTable.InstalledMappings == 1U,
                   fixture.PageTable.LastStart.GetValue() == LazyStart,
                   fixture.Caller.GetSnapshot().State == ThreadState::Ready, snapshot.State == PagerChannelState::Idle,
                   snapshot.Deliveries == 1U, snapshot.Resolutions == 1U, !stale.HasValue());

    const PagerResolutionResult replay = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    ASTERIA_EXPECT(runner, "replaying a resolved token finds no pending fault",
                   replay.Error == PagerResolutionError::NotPending, fixture.PageTable.InstalledMappings == 1U);

    // The token slot was reclaimed, so the thread can fault again.
    const bool taken = fixture.TakeReadyCaller();
    const CapabilityHandle second = fixture.FaultAndReceive(0x1000U + 0x10U, 20U);
    const PagerResolutionResult secondResolved =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(second, ReadWriteUser(), LazyStart + 0x1000U));
    ASTERIA_EXPECT(runner, "a second fault after a resolution delivers and resolves again", taken, second.IsValid(),
                   secondResolved.Error == PagerResolutionError::None, fixture.PageTable.InstalledMappings == 2U,
                   fixture.Channel.GetSnapshot().Resolutions == 2U);
}

class ForeignObject final : public Kernel::Object::IKernelObject
{
public:
    [[nodiscard]] ObjectType GetObjectType() const noexcept override
    {
        return ObjectType::Notification;
    }

    [[nodiscard]] ResourceDomainId GetChargeOwner() const noexcept override
    {
        return ResourceDomainId{13U};
    }
};

// Another owner's object, quiescing and collectable in an earlier registry
// slot than the token, is not the channel's to collect: the token retires
// by its exact identity and the foreign object waits for its own owner.
void TestRetirementLeavesForeignObjects(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    ForeignObject foreign;
    const bool ready = fixture.Initialize();
    const auto registered = fixture.Issuer.CreateObject(foreign);
    const ObjectId foreignId = registered.HasValue() ? registered.GetValue() : ObjectId{};
    const bool destroyed =
        registered.HasValue() && (fixture.Issuer.DestroyObject(foreignId) == Kernel::Object::ObjectRegistryError::Ok);
    const CapabilityHandle token = fixture.FaultAndReceive(0x234U, 10U);
    const PagerResolutionResult resolved = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    const auto collected = fixture.Issuer.CollectDestroyedObject(foreignId);
    ASTERIA_EXPECT(runner, "a token retires by its identity while another owner's object is collectable", ready,
                   destroyed, token.IsValid(), resolved.Error == PagerResolutionError::None,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::Idle, collected.HasValue(),
                   collected.HasValue() && (collected.GetValue().Instance == &foreign),
                   collected.HasValue() && (collected.GetValue().Object.GetValue() == foreignId.GetValue()));
}

void TestRetirementWaitsForRemoteLease(TestContext& runner) noexcept
{
    static ChannelFixture fixtures[2U];
    for (Foundation::Size kind{0U}; kind < 2U; ++kind)
    {
        auto& fixture = fixtures[kind];
        const bool ready = fixture.Initialize() && fixture.InitializeSecondCaller();
        const CapabilityHandle token = fixture.FaultAndReceive(0x234U, 10U);
        if (!ASTERIA_EXPECT(runner, "remote retirement fixture delivers a token", ready, token.IsValid()))
        {
            return;
        }
        std::atomic<bool> arrived{false};
        std::atomic<bool> held{false};
        std::atomic<bool> release{false};
        std::thread remote{[&]() noexcept
                           {
                               Platform::Synchronization::SetHostedProcessor(CpuId{1U});
                               auto lease = fixture.PagerSpace.Lookup(token, ObjectType::FaultToken,
                                                                      CapabilityOperation::Invoke);
                               held.store(lease.HasValue(), std::memory_order_release);
                               arrived.store(true, std::memory_order_release);
                               while (!release.load(std::memory_order_acquire))
                               {
                                   std::this_thread::yield();
                               }
                           }};
        while (!arrived.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        const auto identity = fixture.Channel.GetSnapshot().Ingress.FaultToken;
        auto request = kind == 0U ? fixture.Request(token) : PagerResolveRequest{};
        request.FaultToken = token;
        request.Kind = kind == 0U ? FaultResolutionKind::Map : FaultResolutionKind::Terminate;
        const auto resolved = fixture.Channel.Resolve(fixture.PagerSpace, request);
        const auto stale = fixture.PagerSpace.Lookup(token, ObjectType::FaultToken, CapabilityOperation::Invoke);
        const auto snapshot = fixture.Channel.GetSnapshot();
        release.store(true, std::memory_order_release);
        remote.join();
        fixture.Channel.Retry();
        const auto collectedAgain = fixture.Issuer.CollectDestroyedObject(identity);
        ASTERIA_EXPECT(runner, "Map and Terminate revoke immediately; idle Retry reclaims after a remote lease ends",
                       held.load(std::memory_order_acquire), resolved.Error == PagerResolutionError::None,
                       snapshot.State == PagerChannelState::Idle, !stale.HasValue(),
                       fixture.Caller.GetSnapshot().State == (kind == 0U ? ThreadState::Ready : ThreadState::Exited),
                       !collectedAgain.HasValue(), fixture.Channel.GetSnapshot().State == PagerChannelState::Idle);
        const auto next = fixture.FaultAndReceive(0x1234U, 100U, fixture.Second, fixture.SecondIdentity,
                                                  fixture.SecondContext, fixture.SecondContextIdentity);
        const auto mapped = fixture.Channel.Resolve(fixture.PagerSpace,
                                                    fixture.Request(next, ReadWriteUser(), LazyStart + 0x1000U, 1U));
        ASTERIA_EXPECT(runner, "a later fault resolves after delayed token retirement", next.IsValid(),
                       mapped.Error == PagerResolutionError::None,
                       fixture.Second.GetSnapshot().State == ThreadState::Ready);
    }
}

void TestRetirementPressure(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    constexpr Foundation::Size Capacity{Kernel::Memory::FaultTokenFactory::MaximumTokens};
    Kernel::Object::ObjectPin held[Capacity];
    if (!ASTERIA_EXPECT(runner, "retirement pressure fixture initializes", fixture.Initialize()))
    {
        return;
    }
    for (Foundation::Size index{0U}; index < Capacity; ++index)
    {
        const auto token = fixture.FaultAndReceive(index * 4096U, 10U + index * 2U);
        const auto identity = fixture.Channel.GetSnapshot().Ingress.FaultToken;
        const bool pinned = fixture.Registry.PinObject(identity, ObjectType::FaultToken, held[index]) ==
                            Kernel::Object::ObjectPinError::Ok;
        const auto resolved = fixture.Channel.Resolve(
            fixture.PagerSpace, fixture.Request(token, ReadWriteUser(), LazyStart + index * 4096U));
        if (!ASTERIA_EXPECT(runner, "all factory slots may await collection without faulting the channel",
                            token.IsValid(), pinned, resolved.Error == PagerResolutionError::None,
                            fixture.Channel.GetSnapshot().State == PagerChannelState::Idle, fixture.TakeReadyCaller(),
                            fixture.PagerSpace.Close(token) == CapabilityError::Ok))
        {
            const auto snapshot = fixture.Channel.GetSnapshot();
            std::fprintf(stderr, "  retirement index %zu: channel %u, ingress %u, factory %u, thread %u\n", index,
                         static_cast<unsigned>(snapshot.State), static_cast<unsigned>(snapshot.Ingress.State),
                         static_cast<unsigned>(snapshot.Ingress.Factory),
                         static_cast<unsigned>(fixture.Caller.GetSnapshot().State));
            return;
        }
    }
    const auto deferred = fixture.FaultAndReceive(Capacity * 4096U, 10U + Capacity * 2U);
    ASTERIA_EXPECT(runner, "full retirement storage defers the next token instead of faulting", !deferred.IsValid(),
                   fixture.Channel.GetSnapshot().State == PagerChannelState::FaultPending,
                   fixture.Caller.GetSnapshot().State == ThreadState::Blocked);
    for (auto& pin : held)
    {
        pin.Reset();
    }
    fixture.Channel.Retry();
    Kernel::Ipc::MessageEnvelope message{};
    const auto received = fixture.PagerEndpoint.Receive(message);
    const auto resumed =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(message.Attachments[0U].Handle, ReadWriteUser(),
                                                                    LazyStart + Capacity * 4096U));
    ASTERIA_EXPECT(runner, "pending Retry collects the full retirement set and delivers the deferred fault",
                   received.HasValue(), message.AttachmentCount == 1U, resumed.Error == PagerResolutionError::None,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::Idle,
                   fixture.Channel.GetSnapshot().Resolutions == Capacity + 1U);
}

void TestRejectedRequestsKeepTheFaultPending(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize();
    const CapabilityHandle token = fixture.FaultAndReceive(0x8U, 10U);

    PagerResolveRequest foreignSpace = fixture.Request(token);
    foreignSpace.AddressSpace = fixture.OtherSpaceHandle;
    const PagerResolutionResult foreign = fixture.Channel.Resolve(fixture.PagerSpace, foreignSpace);

    PagerResolveRequest withoutMap = fixture.Request(token);
    withoutMap.MemoryObject = fixture.MemoryInspectHandle;
    const PagerResolutionResult denied = fixture.Channel.Resolve(fixture.PagerSpace, withoutMap);

    const PagerResolutionResult uncovered =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token, ReadWriteUser(), LazyStart + 0x2000U));
    const PagerResolutionResult writableExecutable = fixture.Channel.Resolve(
        fixture.PagerSpace, fixture.Request(token, ReadWriteUser().With(PagePermission::Execute)));

    // A Terminate names only its token: one carrying a mapping is malformed.
    PagerResolveRequest terminate = fixture.Request(token);
    terminate.Kind = FaultResolutionKind::Terminate;
    const PagerResolutionResult unsupported = fixture.Channel.Resolve(fixture.PagerSpace, terminate);

    const PagerResolutionResult forged = fixture.Channel.Resolve(
        fixture.PagerSpace, fixture.Request(CapabilityHandle{token.GetSelector(), token.GetAuthenticator() ^ 1U}));
    const PagerChannelError busy = fixture.Channel.PrepareFault(PagerFaultSource{
        .Thread = fixture.CallerIdentity,
        .AddressSpace = fixture.SpaceIdentity,
        .SchedulingContext = fixture.ContextIdentity,
        .Fault = LazyFault(0U),
        .SupervisionTag = 0U,
    });
    ASTERIA_EXPECT(
        runner, "unauthorized, uncovering, W^X and unsupported requests leave the fault pending", ready,
        token.IsValid(), foreign.Error == PagerResolutionError::AddressSpaceMismatch,
        denied.Error == PagerResolutionError::CapabilityRejected, denied.Capability == CapabilityError::RightsDenied,
        uncovered.Error == PagerResolutionError::MappingRejected,
        writableExecutable.Error == PagerResolutionError::MappingRejected,
        unsupported.Error == PagerResolutionError::InvalidRequest,
        forged.Error == PagerResolutionError::CapabilityRejected, forged.Capability == CapabilityError::InvalidHandle,
        busy == PagerChannelError::Busy, fixture.PageTable.InstalledMappings == 0U,
        fixture.Caller.GetSnapshot().State == ThreadState::Blocked,
        fixture.Channel.GetSnapshot().State == PagerChannelState::FaultPending);

    const PagerResolutionResult corrected = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    ASTERIA_EXPECT(runner, "a corrected request then resolves the same fault",
                   corrected.Error == PagerResolutionError::None, fixture.PageTable.InstalledMappings == 1U,
                   fixture.Caller.GetSnapshot().State == ThreadState::Ready);
}

/// The boot pager's order: each page of the region maps from the same page of
/// One MemoryObject and Frame, a later page first.
[[nodiscard]] PagerResolveRequest TerminateRequest(const CapabilityHandle token) noexcept
{
    return PagerResolveRequest{
        .FaultToken = token,
        .AddressSpace = {},
        .MemoryObject = {},
        .Frame = {},
        .Mapping = {},
        .Kind = FaultResolutionKind::Terminate,
        .Reserved = {},
    };
}

// Terminate exits the Blocked faulting thread without waking it, retires the token and leaves the
// channel ready for the next fault, which another thread of the served space then takes through the
// usual Map.
void TestTerminate(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize() && fixture.InitializeSecondCaller();
    const CapabilityHandle token = fixture.FaultAndReceive(0x8U, 10U);
    const PagerResolutionResult forged = fixture.Channel.Resolve(
        fixture.PagerSpace, TerminateRequest(CapabilityHandle{token.GetSelector(), token.GetAuthenticator() ^ 1U}));
    const PagerResolutionResult terminated = fixture.Channel.Resolve(fixture.PagerSpace, TerminateRequest(token));
    const auto exited = fixture.Caller.GetSnapshot();
    const PagerResolutionResult again = fixture.Channel.Resolve(fixture.PagerSpace, TerminateRequest(token));
    const PagerResolutionResult mapAfter = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    // The exited thread never runs again: it is in no ready queue, cannot be
    // enqueued, and the scheduler finds nothing to run as time passes.
    fixture.Clock.Advance(1'000U);
    const auto dispatched = fixture.Scheduler.DispatchNext();
    const auto requeued =
        fixture.Scheduler.Enqueue(fixture.ContextIdentity, fixture.Context, fixture.CallerIdentity, fixture.Caller);
    ASTERIA_EXPECT(runner, "Terminate exits the faulting thread, retires the token and returns to Idle", ready,
                   token.IsValid(), forged.Error == PagerResolutionError::CapabilityRejected,
                   terminated.Error == PagerResolutionError::None, exited.State == ThreadState::Exited,
                   exited.ExitCode == Abi::FaultTerminatedExitCode, again.Error == PagerResolutionError::NotPending,
                   mapAfter.Error == PagerResolutionError::NotPending, fixture.PageTable.InstalledMappings == 0U,
                   fixture.Channel.GetSnapshot().State == PagerChannelState::Idle,
                   fixture.Channel.GetSnapshot().Resolutions == 1U, fixture.Channel.GetSnapshot().Terminations == 1U,
                   !dispatched.HasValue(), !requeued.HasValue());

    const CapabilityHandle second = fixture.FaultAndReceive(0x8U, 2'000U, fixture.Second, fixture.SecondIdentity,
                                                            fixture.SecondContext, fixture.SecondContextIdentity);
    const PagerResolutionResult mapped = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(second));
    ASTERIA_EXPECT(runner, "after a Terminate the next fault is delivered and resolved by Map", second.IsValid(),
                   mapped.Error == PagerResolutionError::None, fixture.Second.GetSnapshot().State == ThreadState::Ready,
                   fixture.Channel.GetSnapshot().Resolutions == 2U, fixture.Channel.GetSnapshot().Terminations == 1U,
                   fixture.PageTable.InstalledMappings == 1U);
}

// An exception reaches the pager like a page fault, but no mapping resolves it, even one that would
// cover its address; the pager can only terminate the thread.
void TestExceptionsOnlyTerminate(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize();
    const FaultRecord exception{
        .VirtualAddress = LazyStart + 0x8U,
        .InstructionPointer = LazyStart + 0x8U,
        .ArchitectureSyndrome = UInt64{6U} << 32U,
        .Access = FaultAccess::Execute,
        .Cause = FaultCause::Exception,
        .Privilege = FaultPrivilege::User,
        .Reserved = {},
    };
    const CapabilityHandle token = fixture.FaultAndReceive(exception, 10U, fixture.Caller, fixture.CallerIdentity,
                                                           fixture.Context, fixture.ContextIdentity);
    const PagerResolutionResult mapped = fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(token));
    const auto pending = fixture.Channel.GetSnapshot().State;
    const auto blocked = fixture.Caller.GetSnapshot().State;
    const PagerResolutionResult terminated = fixture.Channel.Resolve(fixture.PagerSpace, TerminateRequest(token));
    ASTERIA_EXPECT(
        runner, "an exception is delivered, refuses every Map and ends in a Terminate", ready, token.IsValid(),
        mapped.Error == PagerResolutionError::MappingRejected, pending == PagerChannelState::FaultPending,
        blocked == ThreadState::Blocked, terminated.Error == PagerResolutionError::None,
        fixture.Caller.GetSnapshot().State == ThreadState::Exited,
        fixture.Caller.GetSnapshot().ExitCode == Abi::FaultTerminatedExitCode,
        fixture.PageTable.InstalledMappings == 0U, fixture.Channel.GetSnapshot().State == PagerChannelState::Idle);
}

void TestServesEveryPageOfTheRegion(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const bool ready = fixture.Initialize();
    const CapabilityHandle first = fixture.FaultAndReceive(0x1000U, 10U);
    const PagerResolutionResult upper =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(first, ReadWriteUser(), LazyStart + 0x1000U, 1U));
    const bool taken = fixture.TakeReadyCaller();
    const CapabilityHandle second = fixture.FaultAndReceive(0x10U, 20U);
    const PagerResolutionResult lower =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(second, ReadWriteUser(), LazyStart, 0U));
    ASTERIA_EXPECT(runner, "the pages of one MemoryObject resolve in any order", ready, first.IsValid(),
                   upper.Error == PagerResolutionError::None, taken, second.IsValid(),
                   lower.Error == PagerResolutionError::None, fixture.PageTable.InstalledMappings == 2U,
                   fixture.Channel.GetSnapshot().Resolutions == 2U);
}

void TestConfigurationAndIdle(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    const PagerResolutionResult unconfigured =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(CapabilityHandle{}));
    const PagerChannelError notConfigured = fixture.Channel.PrepareFault(PagerFaultSource{});
    const bool ready = fixture.Initialize();
    const PagerChannelError again = fixture.Channel.Configure(PagerChannelConfiguration{
        .TokenSpace = &fixture.KernelSpace,
        .Readiness = &fixture.Readiness,
        .ShootdownProgress = nullptr,
        .ChargeOwner = ResourceDomainId{7U},
        .Sender = ResourceDomainId{7U},
        .RevocationDomain = RevocationDomainId{1U},
    });
    const PagerResolutionResult idle =
        fixture.Channel.Resolve(fixture.PagerSpace, fixture.Request(fixture.SpaceHandle));
    ASTERIA_EXPECT(runner, "an unconfigured or idle channel resolves nothing and configures once",
                   unconfigured.Error == PagerResolutionError::NotPending,
                   notConfigured == PagerChannelError::NotConfigured, ready,
                   again == PagerChannelError::AlreadyConfigured, idle.Error == PagerResolutionError::NotPending);
}
class ChannelRecords final : public Kernel::Protection::ICreatedDomainRecords
{
public:
    [[nodiscard]] bool RecordDomain(const Kernel::Protection::CreatedDomain& domain) noexcept override
    {
        Last = domain;
        return Record;
    }
    [[nodiscard]] bool RetireDomain(const Kernel::Protection::ProtectionDomain&) noexcept override
    {
        return true;
    }
    Kernel::Protection::CreatedDomain Last{};
    bool Record{true};
};

void TestPooledChannels(TestContext& runner) noexcept
{
    static ChannelFixture fixture;
    static Kernel::Memory::FaultChannelPool pool{fixture.Registry, fixture.Issuer};
    static Kernel::Protection::ProtectionDomainPool domains{fixture.Registry, fixture.Issuer};
    static Kernel::Object::CapabilitySpaceObject receiver{fixture.PagerSpace, ResourceDomainId{13U}};
    ChannelRecords records;
    const bool ready = fixture.Initialize();
    const auto root = fixture.Revocations.CreateRoot();
    const auto authority =
        fixture.Mint(fixture.DomainIdentity, CapabilityRights{CapabilityRight::Manage}, root.GetValue());
    const Kernel::Object::CapabilityValidationKey keys[pool.Capacity]{{1U, 2U}, {3U, 4U}, {5U, 6U}, {7U, 8U}};
    const auto unavailable = pool.Create(fixture.PagerSpace, authority.GetValue(), {});
    const bool badConfiguration =
        pool.Configure(fixture.Revocations, fixture.Clock, nullptr, fixture.Scheduler.GetWakeFacade(),
                       fixture.Shootdown, fixture.Transport, fixture.Progress);
    const Kernel::Object::CapabilityValidationKey zeroKeys[pool.Capacity]{};
    const bool zeroConfiguration =
        pool.Configure(fixture.Revocations, fixture.Clock, zeroKeys, fixture.Scheduler.GetWakeFacade(),
                       fixture.Shootdown, fixture.Transport, fixture.Progress);
    const bool configured = pool.Configure(fixture.Revocations, fixture.Clock, keys, fixture.Scheduler.GetWakeFacade(),
                                           fixture.Shootdown, fixture.Transport, fixture.Progress);
    const auto caps = fixture.Issuer.CreateObject(receiver);
    const auto badReceiver = pool.Create(fixture.PagerSpace, authority.GetValue(), fixture.DomainIdentity);
    const auto channelHandle = pool.Create(fixture.PagerSpace, authority.GetValue(), caps.GetValue());
    const auto secondHandle = pool.Create(fixture.PagerSpace, authority.GetValue(), caps.GetValue());
    ASTERIA_EXPECT(
        runner, "FaultChannel pool configures once and creates two independent channels", ready, configured,
        !badConfiguration, !zeroConfiguration,
        !unavailable.HasValue() && unavailable.GetError().Error == Kernel::Object::ObjectPoolError::CapacityExceeded,
        !badReceiver.HasValue() && badReceiver.GetError().Capability == CapabilityError::ObjectNotFound,
        channelHandle.HasValue(), secondHandle.HasValue(),
        fixture.Domain.GetUsage().Objects[static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel)] == 2U,
        !pool.Configure(fixture.Revocations, fixture.Clock, keys, fixture.Scheduler.GetWakeFacade(), fixture.Shootdown,
                        fixture.Transport, fixture.Progress));
    if (!channelHandle.HasValue() || !secondHandle.HasValue())
    {
        return;
    }
    {
        auto channelLease =
            fixture.PagerSpace.Lookup(channelHandle.GetValue(), ObjectType::FaultChannel, CapabilityOperation::Manage);
        auto secondLease =
            fixture.PagerSpace.Lookup(secondHandle.GetValue(), ObjectType::FaultChannel, CapabilityOperation::Manage);
        auto& channel = static_cast<Kernel::Memory::FaultChannel&>(channelLease.GetValue().GetObject());
        auto& second = static_cast<Kernel::Memory::FaultChannel&>(secondLease.GetValue().GetObject());
        const auto capsHandle =
            fixture.Mint(caps.GetValue(), CapabilityRights{CapabilityRight::Manage}, root.GetValue());
        const auto spaceHandle = fixture.Mint(fixture.SpaceIdentity.GetObjectId(),
                                              CapabilityRights{CapabilityRight::Manage}, root.GetValue());
        const bool domainConfigured = domains.Configure(records);
        const auto makeDomain = [&](const CapabilityHandle handle) noexcept
        {
            return domains.Create({.Space = &fixture.PagerSpace,
                                   .Authority = authority.GetValue(),
                                   .AddressSpace = spaceHandle.GetValue(),
                                   .Capabilities = capsHandle.GetValue(),
                                   .SupervisionTag = 99U,
                                   .Channel = handle});
        };
        records.Record = false;
        const auto unrecorded = makeDomain(channelHandle.GetValue());
        records.Record = true;
        const auto firstDomain = makeDomain(channelHandle.GetValue());
        const auto duplicate = makeDomain(channelHandle.GetValue());
        const auto secondDomain = makeDomain(secondHandle.GetValue());
        ASTERIA_EXPECT(
            runner, "live tags are scoped to their channel and domains pin the channel through collection",
            domainConfigured, !unrecorded.HasValue(), firstDomain.HasValue(), secondDomain.HasValue(),
            !duplicate.HasValue(),
            !duplicate.HasValue() && duplicate.GetError().Error == Kernel::Object::ObjectPoolError::InvalidRequest,
            pool.Destroy(fixture.PagerSpace, channelHandle.GetValue()).Error == Kernel::Object::ObjectPoolError::Busy,
            records.Last.Channel == &second);
        fixture.ActiveChannel = &channel.GetChannel();
        fixture.ActiveEndpoint = &channel.GetEndpoint();
        const CapabilityHandle token = fixture.FaultAndReceive(0U, 0U);
        const auto empty = second.GetEndpoint().GetQueueState();
        ASTERIA_EXPECT(runner, "fault delivery reaches only the selected channel and its own factory issues the token",
                       token.IsValid(), channel.GetChannel().GetSnapshot().Deliveries == 1U,
                       second.GetChannel().GetSnapshot().Deliveries == 0U, empty.QueuedMessages == 0U);
        const auto wrong = second.GetChannel().Resolve(fixture.PagerSpace, fixture.Request(token));
        Kernel::Object::ObjectPin heldToken;
        const bool tokenPinned =
            fixture.Registry.PinObject(channel.GetChannel().GetSnapshot().Ingress.FaultToken, ObjectType::FaultToken,
                                       heldToken) == Kernel::Object::ObjectPinError::Ok;
        const auto resolved = channel.GetChannel().Resolve(fixture.PagerSpace, fixture.Request(token));
        ASTERIA_EXPECT(runner, "a token resolves only on its issuer and the channel returns to Idle",
                       wrong.Error == PagerResolutionError::NotPending, resolved.Error == PagerResolutionError::None,
                       channel.GetChannel().GetSnapshot().State == PagerChannelState::Idle);
        const auto readOnly = fixture.Mint(channelLease.GetValue().GetDescriptor().Object,
                                           CapabilityRights{CapabilityRight::Read}, root.GetValue());
        const auto noManage = makeDomain(readOnly.GetValue());
        const auto noAuthority = pool.Create(fixture.PagerSpace, readOnly.GetValue(), {});
        static ResourceDomainLedger unrelatedLedger;
        static ResourceDomain unrelated{unrelatedLedger};
        const bool unrelatedReady =
            unrelated.InitializeRoot(ResourceDomainId{31U}, Limits()).Error == ResourceDomainError::None;
        const auto unrelatedObject = fixture.Issuer.CreateObject(unrelated);
        const auto unrelatedHandle =
            fixture.Mint(unrelatedObject.GetValue(), CapabilityRights{CapabilityRight::Manage}, root.GetValue());
        const auto crossDomain = domains.Create({.Space = &fixture.PagerSpace,
                                                 .Authority = unrelatedHandle.GetValue(),
                                                 .AddressSpace = spaceHandle.GetValue(),
                                                 .Capabilities = capsHandle.GetValue(),
                                                 .SupervisionTag = 100U,
                                                 .Channel = channelHandle.GetValue()});
        ASTERIA_EXPECT(
            runner, "binding needs channel Manage and confines the charge to its domain tree", unrelatedReady,
            !noManage.HasValue() && noManage.GetError().Capability == CapabilityError::RightsDenied,
            !noAuthority.HasValue() &&
                noAuthority.GetError().Error == Kernel::Object::ObjectPoolError::CapabilityRejected,
            !crossDomain.HasValue() && crossDomain.GetError().Error == Kernel::Object::ObjectPoolError::InvalidRequest);
        const auto destroyed = domains.Destroy(fixture.PagerSpace, firstDomain.GetValue().Capability);
        // Retain the exact domain generation across its destroy: its tag cannot
        // be reused until collection has really released the relationship.
        // The other channel's equal tag remains independent.
        ASTERIA_EXPECT(runner, "collecting a domain frees its channel-local tag",
                       destroyed.Error == Kernel::Object::ObjectPoolError::None, domains.Collect().HasValue());
        const auto reused = makeDomain(channelHandle.GetValue());
        ASTERIA_EXPECT(runner, "the collected tag is reusable", reused.HasValue());
        static Kernel::System::ObjectManager manager{fixture.Registry, fixture.Issuer};
        const bool managerReady = manager.ConfigureDomains(records);
        Kernel::System::ObjectCreateRequest request{};
        request.Type = ObjectType::ProtectionDomain;
        request.Authority = authority.GetValue();
        request.DomainAddressSpace = spaceHandle.GetValue();
        request.DomainCapabilities = capsHandle.GetValue();
        request.SupervisionTag = 777U;
        const auto bootDomain = manager.Create(fixture.PagerSpace, request);
        request.Caller = records.Last.Identity;
        request.SupervisionTag = 778U;
        const auto legacy = manager.Create(fixture.PagerSpace, request);
        request.DomainChannel = channelHandle.GetValue();
        const auto explicitChannel = manager.Create(fixture.PagerSpace, request);
        ASTERIA_EXPECT(runner, "only a boot caller may select its channel with the legacy ProtectionDomain block",
                       managerReady, bootDomain.Error == Kernel::System::ObjectManagementError::None,
                       legacy.Error == Kernel::System::ObjectManagementError::InvalidRequest,
                       explicitChannel.Error == Kernel::System::ObjectManagementError::None);
        static Kernel::Ipc::WaitSetPool sets{fixture.Registry, fixture.Issuer};
        const auto set = sets.Create({.Space = &fixture.PagerSpace, .Authority = authority.GetValue()});
        const auto bound = manager.BindWaitSet(fixture.PagerSpace, set.GetValue().Capability, readOnly.GetValue(), 7U);
        const auto manageOnly = fixture.Mint(channelLease.GetValue().GetDescriptor().Object,
                                             CapabilityRights{CapabilityRight::Manage}, root.GetValue());
        const auto denied =
            manager.BindWaitSet(fixture.PagerSpace, set.GetValue().Capability, manageOnly.GetValue(), 8U);
        const auto rights = fixture.PagerSpace.Query(channelHandle.GetValue());
        ASTERIA_EXPECT(runner, "FaultChannel Read binds readiness; creation grants no Invoke send authority",
                       set.HasValue(), bound.Error == Kernel::System::ObjectManagementError::None,
                       denied.Error == Kernel::System::ObjectManagementError::CapabilityRejected &&
                           denied.Capability == CapabilityError::RightsDenied,
                       rights.HasValue() &&
                           rights.GetValue().Rights.IsEqualTo(Kernel::Memory::FaultChannelPool::CreatedRights),
                       !rights.GetValue().Rights.ContainsAll(CapabilityRights{CapabilityRight::Invoke}));
        const auto firstClosed = domains.Destroy(fixture.PagerSpace, reused.GetValue().Capability);
        const auto secondClosed = domains.Destroy(fixture.PagerSpace, secondDomain.GetValue().Capability);
        const auto collectedDomains = domains.Collect();
        const auto explicitClosed =
            manager.Destroy(fixture.PagerSpace, explicitChannel.Created, ObjectType::ProtectionDomain);
        const auto bootClosed = manager.Destroy(fixture.PagerSpace, bootDomain.Created, ObjectType::ProtectionDomain);
        ASTERIA_EXPECT(
            runner, "an idle FaultChannel cannot destroy storage still retained by a retired token", tokenPinned,
            pool.Destroy(fixture.PagerSpace, channelHandle.GetValue()).Error == Kernel::Object::ObjectPoolError::Busy);
        heldToken.Reset();
        const bool progressedRetirement = pool.Progress();
        const auto channelClosed = pool.Destroy(fixture.PagerSpace, channelHandle.GetValue());
        const auto secondChannelClosed = pool.Destroy(fixture.PagerSpace, secondHandle.GetValue());
        const auto pinned = pool.Collect();
        const auto setClosed = sets.Destroy(fixture.PagerSpace, set.GetValue().Capability);
        const auto collectedSet = sets.Collect();
        const bool receiverDestroyed =
            fixture.Issuer.DestroyObject(caps.GetValue()) == Kernel::Object::ObjectRegistryError::Ok;
        const auto retainedReceiver = fixture.Issuer.CollectDestroyedObject(caps.GetValue());
        ASTERIA_EXPECT(runner, "WaitSet readiness pins and live channel leases delay collection after destruction",
                       progressedRetirement, firstClosed.Error == Kernel::Object::ObjectPoolError::None,
                       secondClosed.Error == Kernel::Object::ObjectPoolError::None,
                       collectedDomains.HasValue() && collectedDomains.GetValue() == 2U,
                       explicitClosed.Error == Kernel::System::ObjectManagementError::None,
                       bootClosed.Error == Kernel::System::ObjectManagementError::None,
                       channelClosed.Error == Kernel::Object::ObjectPoolError::None,
                       secondChannelClosed.Error == Kernel::Object::ObjectPoolError::None,
                       pinned.HasValue() && pinned.GetValue() == 0U, setClosed.HasValue(), receiverDestroyed,
                       !retainedReceiver.HasValue() &&
                           retainedReceiver.GetError() == Kernel::Object::ObjectRegistryError::NoDestructionReady,
                       collectedSet.HasValue() && collectedSet.GetValue() == 1U);
    }
    const auto collected = pool.Collect();
    const auto receiverCollected = fixture.Issuer.CollectDestroyedObject(caps.GetValue());
    ASTERIA_EXPECT(runner, "channel collection returns storage, queues and type units after the last lease",
                   collected.HasValue() && collected.GetValue() == 2U, pool.GetSnapshot().Reclaimed == 2U,
                   receiverCollected.HasValue(),
                   fixture.Domain.GetUsage().Objects[static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel)] ==
                       0U,
                   pool.GetMetrics(0U).Deliveries == 1U, pool.GetMetrics(0U).Resolutions == 1U);
    CapabilityHandle handles[Kernel::Memory::FaultChannelPool::Capacity]{};
    bool filled{true};
    for (Foundation::Size index{0U}; index < Kernel::Memory::FaultChannelPool::Capacity; ++index)
    {
        const auto created = pool.Create(fixture.PagerSpace, authority.GetValue(), {});
        filled = filled && created.HasValue();
        if (created.HasValue())
        {
            handles[index] = created.GetValue();
        }
    }
    const auto exhausted = pool.Create(fixture.PagerSpace, authority.GetValue(), {});
    bool drained{true};
    for (const auto handle : handles)
    {
        drained = drained && pool.Destroy(fixture.PagerSpace, handle).Error == Kernel::Object::ObjectPoolError::None;
    }
    const auto reclaimed = pool.Collect();
    ASTERIA_EXPECT(runner, "four channels exhaust the pool and every freed slot can be reused", filled,
                   !exhausted.HasValue() &&
                       exhausted.GetError().Error == Kernel::Object::ObjectPoolError::CapacityExceeded,
                   drained, reclaimed.HasValue() && reclaimed.GetValue() == Kernel::Memory::FaultChannelPool::Capacity,
                   pool.GetSnapshot().FreeSlots == Kernel::Memory::FaultChannelPool::Capacity);
    // Publication consumes two registry entries. Exhaustion must return
    // both the type unit and the token root, without faulting the pool.
    static Kernel::Ipc::Notification notifications[TestSupport::TestObjectRegistry::EntryCapacity];
    ObjectId identifiers[TestSupport::TestObjectRegistry::EntryCapacity]{};
    Foundation::Size occupied{0U};
    for (auto& notification : notifications)
    {
        const auto initialized = notification.Initialize(ResourceDomainId{13U});
        const auto object = initialized == Kernel::Ipc::NotificationError::None
                                ? fixture.Issuer.CreateObject(notification)
                                : decltype(fixture.Issuer.CreateObject(notification))::Failure(
                                      Kernel::Object::ObjectRegistryError::InvalidObject);
        if (!object.HasValue())
        {
            break;
        }
        identifiers[occupied++] = object.GetValue();
    }
    const auto failed = pool.Create(fixture.PagerSpace, authority.GetValue(), {});
    bool restored{true};
    for (Foundation::Size index{0U}; index < occupied; ++index)
    {
        restored =
            restored && fixture.Issuer.DestroyObject(identifiers[index]) == Kernel::Object::ObjectRegistryError::Ok;
        restored = restored && fixture.Issuer.CollectDestroyedObject(identifiers[index]).HasValue();
    }
    const auto recovered = pool.Create(fixture.PagerSpace, authority.GetValue(), {});
    ASTERIA_EXPECT(
        runner, "failed registry publication rolls back its charge and root and permits another create", occupied != 0U,
        !failed.HasValue(), restored, recovered.HasValue(),
        pool.Destroy(fixture.PagerSpace, recovered.GetValue()).Error == Kernel::Object::ObjectPoolError::None,
        pool.Collect().HasValue(),
        fixture.Domain.GetUsage().Objects[static_cast<Foundation::Size>(Abi::LimitedObject::FaultChannel)] == 0U);
}

} // Namespace
} // Namespace Asteria::Test

int main(const int argc, const char* const argv[])
{
    TestContext runner{"PagerChannelTests"};
    Asteria::Test::TestPooledChannels(runner);
    if ((argc == 2) && (std::strcmp(argv[1U], "frame-rights") == 0))
    {
        Asteria::Test::TestFrameWriteAuthority(runner);
        return runner.Finish();
    }
    Asteria::Test::TestConcurrentPrepare(runner);
    Asteria::Test::TestSnapshotDoesNotLoseCommit(runner);
    Asteria::Test::TestResolvingDoesNotBlockContender(runner);
    Asteria::Test::TestFrameWriteAuthority(runner);
    Asteria::Test::TestRoundTrip(runner);
    Asteria::Test::TestRetirementLeavesForeignObjects(runner);
    Asteria::Test::TestRetirementWaitsForRemoteLease(runner);
    Asteria::Test::TestRetirementPressure(runner);
    Asteria::Test::TestRejectedRequestsKeepTheFaultPending(runner);
    Asteria::Test::TestTerminate(runner);
    Asteria::Test::TestExceptionsOnlyTerminate(runner);
    Asteria::Test::TestServesEveryPageOfTheRegion(runner);
    Asteria::Test::TestConfigurationAndIdle(runner);
    return runner.Finish();
}
