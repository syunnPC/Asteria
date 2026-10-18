// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Memory/Frame.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Memory/MemoryMapper.hpp"
#include "Kernel/Memory/MemoryObject.hpp"
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
using Foundation::Size;
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
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::AddressSpaceMappingRecord;
using Kernel::Memory::AddressSpaceState;
using Kernel::Memory::BootFrame;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameDescriptor;
using Kernel::Memory::FrameError;
using Kernel::Memory::FrameKind;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::IPagerShootdownProgress;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Memory::MemoryCommitment;
using Kernel::Memory::MemoryMapper;
using Kernel::Memory::MemoryMapperError;
using Kernel::Memory::MemoryMapperResult;
using Kernel::Memory::MemoryMapRequest;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectDescriptor;
using Kernel::Memory::MemoryObjectError;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::PinnedFrameMappingError;
using Kernel::Memory::PinnedFrameMappingRequest;
using Kernel::Memory::TlbShootdownBatch;
using Kernel::Memory::TlbShootdownDispatcher;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
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
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

constexpr UInt64 WindowStart{0x0000'0000'4C00'0000ULL};
constexpr UInt64 FrameStart{0x0200'0000U};

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
        Owner = &owner;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableOwnershipError Release(const AddressSpace&) noexcept override
    {
        Owner = nullptr;
        return AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] AddressSpacePageTableActivationError Activate(const AddressSpace& owner, CpuId) noexcept override
    {
        return (Owner == &owner) ? AddressSpacePageTableActivationError::None
                                 : AddressSpacePageTableActivationError::InvalidOwner;
    }

    [[nodiscard]] AddressSpacePageTableError Unmap(const AddressSpaceMappingRecord& mapping,
                                                   AddressSpacePageTableMapResult& output) noexcept override
    {
        if (UnmapRefusal != AddressSpacePageTableError::None)
        {
            return UnmapRefusal;
        }
        output = AddressSpacePageTableMapResult{
            .InvalidationStart = mapping.VirtualStart,
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        --Installed;
        return AddressSpacePageTableError::None;
    }

    [[nodiscard]] AddressSpacePageTableError Map(const AddressSpaceMappingRecord& mapping,
                                                 AddressSpacePageTableMapResult& output) noexcept override
    {
        if (Refusal != AddressSpacePageTableError::None)
        {
            return Refusal;
        }
        output = AddressSpacePageTableMapResult{
            .InvalidationStart = mapping.VirtualStart,
            .InvalidationPages = mapping.Pages,
            .Reserved = 0U,
        };
        Last = mapping;
        ++Installed;
        return AddressSpacePageTableError::None;
    }

    const AddressSpace* Owner{nullptr};
    AddressSpaceMappingRecord Last{};
    Size Installed{0U};
    AddressSpacePageTableError Refusal{AddressSpacePageTableError::None};
    AddressSpacePageTableError UnmapRefusal{AddressSpacePageTableError::None};
};

class TestLocalTlbInvalidator final : public ILocalTlbInvalidator
{
public:
    [[nodiscard]] LocalTlbInvalidationError Invalidate(const VirtualAddress, const PageCount) noexcept override
    {
        ++Invalidations;
        return LocalTlbInvalidationError::None;
    }

    Size Invalidations{0U};
};

class TestTransport final : public ITlbShootdownTransport
{
public:
    [[nodiscard]] TlbShootdownTransportError Publish(const TlbShootdownBatch& batch) noexcept override
    {
        Batch = batch;
        ++Published;
        return TlbShootdownTransportError::None;
    }

    TlbShootdownBatch Batch{};
    Size Published{0U};
};

/// Each wait lets CPU 1 drain the published batch once Drain is set.
class TestProgress final : public IPagerShootdownProgress
{
public:
    TestProgress(TestTransport& transport, TlbShootdownDispatcher& remote) noexcept
        : m_transport{transport}, m_remote{remote}
    {
    }

    void Wait() noexcept override
    {
        ++Waits;
        if (!Drain || Waits < DrainAfter)
        {
            return;
        }
        for (Size index{0U}; index < m_transport.Batch.GetCount(); ++index)
        {
            Handled = Handled && m_remote.HandleRemote(m_transport.Batch.GetWork(index)).HasValue();
        }
        m_transport.Batch = TlbShootdownBatch{};
    }

    Size Waits{0U};
    bool Drain{true};
    Size DrainAfter{0U};
    bool Handled{true};

private:
    TestTransport& m_transport;
    TlbShootdownDispatcher& m_remote;
};

[[nodiscard]] constexpr PagePermissions ReadWriteUser() noexcept
{
    return PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
}

[[nodiscard]] bool BuildSeedPlan(InitialMappingPlan& plan) noexcept
{
    const MappingPermissions readWrite = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
    return (plan.AddMapping(InitialMapping{
                .VirtualStart = VirtualAddress{0xFFFF'8000'0000'1000ULL},
                .PhysicalStart = PhysicalAddress{0x0010'0000U},
                .Pages = PageCount{2U},
                .Permissions = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
                .CachePolicy = MemoryCachePolicy::WriteBack,
                .Owner = InitialMappingOwner::NucleusCode,
                .Reserved = 0U,
            }) == InitialMappingPlanError::None) &&
           (plan.AddMapping(InitialMapping{
                .VirtualStart = VirtualAddress{0xFFFF'8000'0000'4000ULL},
                .PhysicalStart = PhysicalAddress{0x0010'4000U},
                .Pages = PageCount{4U},
                .Permissions = readWrite,
                .CachePolicy = MemoryCachePolicy::WriteBack,
                .Owner = InitialMappingOwner::NucleusStack,
                .Reserved = 0U,
            }) == InitialMappingPlanError::None) &&
           (plan.AddMapping(InitialMapping{
                .VirtualStart = VirtualAddress{0xFFFF'8000'0000'8000ULL},
                .PhysicalStart = PhysicalAddress{0x0010'8000U},
                .Pages = PageCount{16U},
                .Permissions = readWrite,
                .CachePolicy = MemoryCachePolicy::WriteBack,
                .Owner = InitialMappingOwner::PageTable,
                .Reserved = 0U,
            }) == InitialMappingPlanError::None) &&
           (plan.Seal() == InitialMappingPlanError::None);
}

class MapperFixture final
{
public:
    MapperFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Caller{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL}, Domain{Ledger},
          Local{CpuId{0U}, Invalidator}, Remote{CpuId{1U}, Invalidator}, Progress{Transport, Remote}, Mapper{Registry}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        constexpr ResourceDomainLimits limits{
            .CommittedMemoryPages = 64U,
            .ResidentMemoryPages = 64U,
            .KernelMetadataBytes = 1U << 20U,
            .CapabilityHandles = 64U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .ChildDomains = 0U,
        };
        const auto root = Revocations.CreateRoot();
        if ((Domain.InitializeRoot(ResourceDomainId{13U}, limits).Error != ResourceDomainError::None) ||
            !root.HasValue() || !BuildSeedPlan(SeedPlan) ||
            (Space.Initialize(ResourceDomainId{10U}, PageTable, SeedPlan) != AddressSpaceError::None))
        {
            return false;
        }
        const auto domain = Issuer.CreateObject(Domain);
        const auto space = Issuer.CreateObject(Space);
        if (!domain.HasValue() || !space.HasValue())
        {
            return false;
        }
        SpaceIdentity = AddressSpaceId{Registry, space.GetValue()};
        if ((Space.BindIdentity(Registry, SpaceIdentity).Error != AddressSpaceError::None) ||
            (Memory
                 .Initialize(Registry, ResourceDomainObjectId{Registry, domain.GetValue()}, Domain,
                             MemoryObjectDescriptor{
                                 .Pages = PageCount{4U},
                                 .CachePolicy = MemoryCachePolicy::WriteBack,
                                 .MaximumPermissions = ReadWriteUser(),
                                 .Commitment = MemoryCommitment::Pinned,
                                 .Reserved = {},
                             })
                 .Error != MemoryObjectError::None) ||
            (FrameStorage.Initialize(ResourceDomainId{12U}, FrameDescriptor{
                                                                .PhysicalStart = PhysicalAddress{FrameStart},
                                                                .Pages = PageCount{4U},
                                                                .CachePolicy = MemoryCachePolicy::WriteBack,
                                                                .Kind = FrameKind::Ram,
                                                                .Reserved = {},
                                                            }) != FrameError::None))
        {
            return false;
        }
        const auto memory = Issuer.CreateObject(Memory);
        const auto frame = Issuer.CreateObject(FrameStorage);
        if (!memory.HasValue() || !frame.HasValue())
        {
            return false;
        }
        const CapabilityRights map{CapabilityRight::Map};
        const auto spaceHandle = Mint(space.GetValue(), map, root.GetValue());
        const auto spaceInspect = Mint(space.GetValue(), CapabilityRights{CapabilityRight::Inspect}, root.GetValue());
        const auto memoryHandle = Mint(memory.GetValue(), map, root.GetValue());
        const auto frameHandle =
            Mint(frame.GetValue(), map.With(CapabilityRight::Write).With(CapabilityRight::Derive), root.GetValue());
        if (!spaceHandle.HasValue() || !spaceInspect.HasValue() || !memoryHandle.HasValue() || !frameHandle.HasValue())
        {
            return false;
        }
        SpaceHandle = spaceHandle.GetValue();
        SpaceInspectHandle = spaceInspect.GetValue();
        MemoryHandle = memoryHandle.GetValue();
        FrameHandle = frameHandle.GetValue();
        return Space.ActivateProcessor(CpuId{0U}).HasValue();
    }

    [[nodiscard]] bool Configure() noexcept
    {
        return Mapper.Configure(Local, Transport, Progress);
    }

    [[nodiscard]] MemoryMapRequest Request(const UInt64 virtualStart, const UInt64 pages, const UInt64 frameOffset,
                                           const PagePermissions permissions) const noexcept
    {
        return MemoryMapRequest{
            .AddressSpace = SpaceHandle,
            .MemoryObject = MemoryHandle,
            .Frame = FrameHandle,
            .Mapping =
                PinnedFrameMappingRequest{
                    .VirtualStart = VirtualAddress{virtualStart},
                    .Pages = PageCount{pages},
                    .MemoryObjectOffsetPages = 0U,
                    .FrameOffsetPages = frameOffset,
                    .Permissions = permissions,
                    .Reserved = {},
                },
        };
    }

    [[nodiscard]] MemoryMapperResult Map(const MemoryMapRequest& request) noexcept
    {
        return Mapper.Map(Caller, request);
    }

    [[nodiscard]] Foundation::Result<CapabilityHandle, CapabilityError>
    Mint(const ObjectId object, const CapabilityRights rights, const RevocationDomainId domain) noexcept
    {
        return Issuer.Mint(Caller, object, rights, domain, MonotonicTime::Infinite(), 0x5EA1U);
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Caller;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    InitialMappingPlan SeedPlan;
    TestPageTable PageTable;
    AddressSpace Space;
    AddressSpaceId SpaceIdentity{};
    MemoryObject Memory;
    BootFrame FrameStorage;
    TestLocalTlbInvalidator Invalidator;
    TestTransport Transport;
    TlbShootdownDispatcher Local;
    TlbShootdownDispatcher Remote;
    TestProgress Progress;
    MemoryMapper Mapper;
    CapabilityHandle SpaceHandle{};
    CapabilityHandle SpaceInspectHandle{};
    CapabilityHandle MemoryHandle{};
    CapabilityHandle FrameHandle{};
};

[[nodiscard]] bool Is(const MemoryMapperResult& result, const MemoryMapperError error) noexcept
{
    return result.Error == error;
}

// Attenuation is enforced even with a writable MemoryObject created by the consumer. Refusal must
// leave no mapping.
void TestFrameWriteAuthority(TestContext& runner) noexcept
{
    MapperFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Configure();
    const auto copy =
        fixture.Caller.Derive(fixture.FrameHandle, CapabilityRights{CapabilityRight::Map}, MonotonicTime::Infinite());
    auto request = fixture.Request(WindowStart, 1U, 0U, ReadWriteUser());
    request.Frame = copy.HasValue() ? copy.GetValue() : CapabilityHandle{};
    const auto denied = fixture.Map(request);
    ASTERIA_EXPECT(runner, "a Map-only Frame cannot back a writable CPU mapping", ready, copy.HasValue(),
                   denied.Error == MemoryMapperError::CapabilityRejected,
                   denied.Capability == CapabilityError::RightsDenied, fixture.PageTable.Installed == 0U,
                   fixture.Mapper.GetSnapshot().Mappings == 0U, fixture.Progress.Waits == 0U);
    const auto writeOnly =
        fixture.Caller.Derive(fixture.FrameHandle, CapabilityRights{CapabilityRight::Write}, MonotonicTime::Infinite());
    auto withoutMap = request;
    withoutMap.Frame = writeOnly.HasValue() ? writeOnly.GetValue() : CapabilityHandle{};
    const auto noMap = fixture.Map(withoutMap);
    ASTERIA_EXPECT(runner, "Write alone does not confer Map", writeOnly.HasValue(),
                   noMap.Error == MemoryMapperError::CapabilityRejected,
                   noMap.Capability == CapabilityError::RightsDenied, fixture.PageTable.Installed == 0U);
    request.Mapping.Permissions = PagePermissions{PagePermission::Read}.With(PagePermission::User);
    const auto read = fixture.Map(request);
    ASTERIA_EXPECT(runner, "Map alone permits reading without a Read capability right",
                   read.Error == MemoryMapperError::None, fixture.PageTable.Installed == 1U,
                   !fixture.PageTable.Last.Permissions.Contains(PagePermission::Write));
    const auto removed =
        fixture.Mapper.Unmap(fixture.Caller, fixture.SpaceHandle, VirtualAddress{WindowStart}, PageCount{1U});
    const auto writer = fixture.Map(fixture.Request(WindowStart, 1U, 0U, ReadWriteUser()));
    ASTERIA_EXPECT(runner, "Map and Write permit a writable CPU mapping", removed.Error == MemoryMapperError::None,
                   writer.Error == MemoryMapperError::None,
                   fixture.PageTable.Last.Permissions.Contains(PagePermission::Write));
}

void TestMapsUserMemory(TestContext& runner) noexcept
{
    MapperFixture fixture;
    const bool ready = fixture.Initialize();
    const auto unconfigured = fixture.Map(fixture.Request(WindowStart, 2U, 1U, ReadWriteUser()));
    const bool configured = fixture.Configure();
    const bool again = fixture.Configure();
    const Size before = fixture.Space.GetSnapshot().MappingCount;
    const auto mapped = fixture.Map(fixture.Request(WindowStart, 2U, 1U, ReadWriteUser()));
    const auto& last = fixture.PageTable.Last;
    ASTERIA_EXPECT(
        runner, "a configured mapper maps the Frame's pages into the space", ready,
        Is(unconfigured, MemoryMapperError::NotConfigured), configured, !again, Is(mapped, MemoryMapperError::None),
        fixture.PageTable.Installed == 1U, last.VirtualStart.GetValue() == WindowStart, last.Pages.GetValue() == 2U,
        last.PhysicalStart.GetValue() == FrameStart + 0x1000U, last.Permissions.Contains(PagePermission::User),
        !last.Permissions.Contains(PagePermission::Execute), fixture.Space.GetSnapshot().MappingCount == before + 1U,
        fixture.Space.GetSnapshot().State == AddressSpaceState::Ready, fixture.Mapper.GetSnapshot().Mappings == 1U,
        fixture.Transport.Published == 0U, fixture.Progress.Waits == 0U,
        fixture.Mapper.GetSnapshot().LastMapping.VirtualStart.GetValue() == WindowStart,
        fixture.Mapper.GetSnapshot().LastMapping.PhysicalStart.GetValue() == FrameStart + 0x1000U,
        fixture.Mapper.GetSnapshot().LastMapping.Pages.GetValue() == 2U);

    const auto overlap = fixture.Map(fixture.Request(WindowStart + 0x1000U, 1U, 0U, ReadWriteUser()));
    const auto supervisor = fixture.Map(fixture.Request(
        WindowStart + 0x4000U, 1U, 0U, PagePermissions{PagePermission::Read}.With(PagePermission::Write)));
    const auto writeExecute =
        fixture.Map(fixture.Request(WindowStart + 0x4000U, 1U, 0U, ReadWriteUser().With(PagePermission::Execute)));
    const auto beyond = fixture.Map(fixture.Request(WindowStart + 0x4000U, 2U, 3U, ReadWriteUser()));
    ASTERIA_EXPECT(runner, "an overlapping, supervisor, writable-executable or oversized mapping is invalid",
                   Is(overlap, MemoryMapperError::InvalidRequest),
                   overlap.AddressSpace == AddressSpaceError::VirtualOverlap,
                   Is(supervisor, MemoryMapperError::InvalidRequest),
                   supervisor.Preparation == PinnedFrameMappingError::InvalidRequest,
                   Is(writeExecute, MemoryMapperError::InvalidRequest), Is(beyond, MemoryMapperError::InvalidRequest),
                   fixture.PageTable.Installed == 1U, fixture.Mapper.GetSnapshot().Mappings == 1U,
                   fixture.Mapper.GetSnapshot().LastMapping.VirtualStart.GetValue() == WindowStart,
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);

    MemoryMapRequest inspectOnly = fixture.Request(WindowStart + 0x4000U, 1U, 0U, ReadWriteUser());
    inspectOnly.AddressSpace = fixture.SpaceInspectHandle;
    MemoryMapRequest swapped = fixture.Request(WindowStart + 0x4000U, 1U, 0U, ReadWriteUser());
    swapped.MemoryObject = fixture.FrameHandle;
    const auto denied = fixture.Map(inspectOnly);
    const auto mistyped = fixture.Map(swapped);
    fixture.PageTable.Refusal = AddressSpacePageTableError::CapacityExceeded;
    const auto full = fixture.Map(fixture.Request(WindowStart + 0x4000U, 1U, 0U, ReadWriteUser()));
    fixture.PageTable.Refusal = AddressSpacePageTableError::None;
    ASTERIA_EXPECT(
        runner, "a handle without Map or of another type, or a full page table, is refused",
        Is(denied, MemoryMapperError::CapabilityRejected), denied.Capability == CapabilityError::RightsDenied,
        Is(mistyped, MemoryMapperError::CapabilityRejected), mistyped.Capability == CapabilityError::TypeMismatch,
        Is(full, MemoryMapperError::CapacityExceeded), fixture.Space.GetSnapshot().State == AddressSpaceState::Ready,
        Is(fixture.Map(fixture.Request(WindowStart + 0x4000U, 1U, 0U, ReadWriteUser())), MemoryMapperError::None),
        fixture.Mapper.GetSnapshot().Mappings == 2U);
}

// Unmap removes exactly one mapping, after the remote processor's
// acknowledgement, and frees its range for the next mapping.
void TestUnmapsUserMemory(TestContext& runner) noexcept
{
    MapperFixture fixture;
    const bool ready =
        fixture.Initialize() && fixture.Configure() && fixture.Space.ActivateProcessor(CpuId{1U}).HasValue();
    const auto mapped = fixture.Map(fixture.Request(WindowStart, 2U, 0U, ReadWriteUser()));
    const auto unmap = [&](const CapabilityHandle space, const UInt64 start, const UInt64 pages) noexcept
    {
        return fixture.Mapper.Unmap(fixture.Caller, space, VirtualAddress{start}, PageCount{pages});
    };
    const auto partial = unmap(fixture.SpaceHandle, WindowStart, 1U);
    const auto elsewhere = unmap(fixture.SpaceHandle, WindowStart + 0x1000U, 2U);
    const auto denied = unmap(fixture.SpaceInspectHandle, WindowStart, 2U);
    fixture.PageTable.UnmapRefusal = AddressSpacePageTableError::UnsupportedConfiguration;
    const auto unsupported = unmap(fixture.SpaceHandle, WindowStart, 2U);
    fixture.PageTable.UnmapRefusal = AddressSpacePageTableError::None;
    const Size waitsBefore = fixture.Progress.Waits;
    const auto unmapped = unmap(fixture.SpaceHandle, WindowStart, 2U);
    const Size waitsAfter = fixture.Progress.Waits;
    const Size installedAfter = fixture.PageTable.Installed;
    const auto activeAfter = fixture.Memory.GetSnapshot().ActiveMappings;
    const auto again = unmap(fixture.SpaceHandle, WindowStart, 2U);
    const auto remapped = fixture.Map(fixture.Request(WindowStart, 2U, 2U, ReadWriteUser()));
    ASTERIA_EXPECT(runner, "Unmap removes exactly one mapping after the remote acknowledgement", ready,
                   Is(mapped, MemoryMapperError::None), Is(partial, MemoryMapperError::InvalidRequest),
                   partial.AddressSpace == AddressSpaceError::MappingNotFound,
                   Is(elsewhere, MemoryMapperError::InvalidRequest), Is(denied, MemoryMapperError::CapabilityRejected),
                   denied.Capability == CapabilityError::RightsDenied,
                   Is(unsupported, MemoryMapperError::NotConfigured),
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Ready, Is(unmapped, MemoryMapperError::None),
                   waitsAfter == waitsBefore + 1U, installedAfter == 0U, activeAfter == 0U,
                   Is(again, MemoryMapperError::InvalidRequest), Is(remapped, MemoryMapperError::None),
                   fixture.Mapper.GetSnapshot().Mappings == 2U, fixture.Mapper.GetSnapshot().Unmappings == 1U);
}

// A space resident on another CPU completes once that CPU acknowledged; a
// space whose acknowledgement never comes faults the mapper.
void TestRemoteShootdown(TestContext& runner) noexcept
{
    MapperFixture fixture;
    const bool ready =
        fixture.Initialize() && fixture.Configure() && fixture.Space.ActivateProcessor(CpuId{1U}).HasValue();
    const auto mapped = fixture.Map(fixture.Request(WindowStart, 1U, 0U, ReadWriteUser()));
    ASTERIA_EXPECT(runner, "a mapping waits for the remote processor's acknowledgement", ready,
                   Is(mapped, MemoryMapperError::None), fixture.Transport.Published == 1U, fixture.Progress.Waits == 1U,
                   fixture.Progress.Handled, fixture.Space.GetSnapshot().State == AddressSpaceState::Ready);

    fixture.Progress.DrainAfter = 257U;
    const auto delayed = fixture.Map(fixture.Request(WindowStart + 0x1000U, 1U, 1U, ReadWriteUser()));
    const auto after = fixture.Map(fixture.Request(WindowStart + 0x2000U, 1U, 2U, ReadWriteUser()));
    ASTERIA_EXPECT(runner, "a slow acknowledgement preserves the mapper for subsequent calls",
                   Is(delayed, MemoryMapperError::None), fixture.Progress.Waits >= 257U,
                   fixture.Space.GetSnapshot().State == AddressSpaceState::Ready, Is(after, MemoryMapperError::None),
                   fixture.Mapper.GetSnapshot().Mappings == 3U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"MemoryMapperTests"};
    Asteria::Test::TestFrameWriteAuthority(runner);
    Asteria::Test::TestMapsUserMemory(runner);
    Asteria::Test::TestRemoteShootdown(runner);
    Asteria::Test::TestUnmapsUserMemory(runner);
    return runner.Finish();
}
