// SPDX-License-Identifier: Apache-2.0
#include "Abi/SystemCallAbi.hpp"
#include "Hal/Device/IPciConfigurationSpace.hpp"
#include "Hal/Device/IPciConfigurationWriter.hpp"
#include "Hal/Interrupt/IInterruptLines.hpp"
#include "Hal/Interrupt/IMessageVectors.hpp"
#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Device/PciFunction.hpp"
#include "Kernel/Device/DmaReservations.hpp"
#include "Platform/Device/IdentityDomainBackend.hpp"
#include "Kernel/Interrupt/Interrupt.hpp"
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/System/ObjectManager.hpp"
#include "Platform/Synchronization/ProcessorContext.hpp"

#include <new>

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/PoolSlabFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointConfiguration;
using Kernel::Ipc::EndpointPool;
using Kernel::Memory::Frame;
using Kernel::Memory::FramePoolError;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Resource::ResourceDomainUsage;
using Foundation::UInt64;
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
using Kernel::System::ObjectCreateRequest;
using Kernel::System::ObjectManagementError;
using Kernel::System::ObjectManagementResult;
using Kernel::System::ObjectManager;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

// A backend that installs and removes every record as asked.
class TestPageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Claim(const Kernel::Memory::AddressSpace& owner, const Kernel::Memory::InitialMappingPlan&) noexcept override
    {
        Owner = &owner;
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError
    Release(const Kernel::Memory::AddressSpace&) noexcept override
    {
        Owner = nullptr;
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableActivationError Activate(const Kernel::Memory::AddressSpace&,
                                                                             Kernel::Scheduler::CpuId) noexcept override
    {
        return Hal::Memory::AddressSpacePageTableActivationError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Map(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
        Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override
    {
        output = Hal::Memory::AddressSpacePageTableMapResult{mapping.VirtualStart, mapping.Pages, 0U};
        return Hal::Memory::AddressSpacePageTableError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableError
    Unmap(const Kernel::Memory::AddressSpaceMappingRecord& mapping,
          Hal::Memory::AddressSpacePageTableMapResult& output) noexcept override
    {
        output = Hal::Memory::AddressSpacePageTableMapResult{mapping.VirtualStart, mapping.Pages, 0U};
        return Hal::Memory::AddressSpacePageTableError::None;
    }

    const Kernel::Memory::AddressSpace* Owner{nullptr};
};

class TestInvalidator final : public Hal::Memory::ILocalTlbInvalidator
{
public:
    [[nodiscard]] Hal::Memory::LocalTlbInvalidationError Invalidate(const Kernel::Memory::VirtualAddress,
                                                                    const PageCount) noexcept override
    {
        return Hal::Memory::LocalTlbInvalidationError::None;
    }
};

class TestTransport final : public Hal::Memory::ITlbShootdownTransport
{
public:
    [[nodiscard]] Hal::Memory::TlbShootdownTransportError
    Publish(const Kernel::Memory::TlbShootdownBatch&) noexcept override
    {
        return Hal::Memory::TlbShootdownTransportError::None;
    }
};

class TestProgress final : public Kernel::Memory::IPagerShootdownProgress
{
public:
    void Wait() noexcept override
    {
    }
};

// Zeroes runs of a host buffer that stands for the frame pool, or answers as
// told.
class TestScrubber final : public Kernel::Memory::IFrameScrubber
{
public:
    [[nodiscard]] Kernel::Memory::FrameScrubError Zero(const PhysicalAddress physicalStart,
                                                       const UInt64 pages) noexcept override
    {
        ++Calls;
        LastStart = physicalStart.GetValue();
        LastPages = pages;
        if (Answer == Kernel::Memory::FrameScrubError::None)
        {
            auto* const bytes = Storage + (LastStart - PhysicalBase);
            for (UInt64 index{0U}; index < (pages * 4096U); ++index)
            {
                bytes[index] = 0U;
            }
        }
        return Answer;
    }

    unsigned char* Storage{nullptr};
    UInt64 PhysicalBase{0U};
    Kernel::Memory::FrameScrubError Answer{Kernel::Memory::FrameScrubError::None};
    Size Calls{0U};
    UInt64 LastStart{0U};
    UInt64 LastPages{0U};
};

class ManagerFixture final
{
public:
    ManagerFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Manager{Registry, Issuer}
    {
        static_cast<void>(Manager.AttachSlabs(Slabs.Frames, Slabs.MemoryObjects));
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        constexpr ResourceDomainLimits limits{
            .CommittedMemoryPages = 8U,
            .ResidentMemoryPages = 8U,
            .KernelMetadataBytes = 1U << 20U,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 1U << 20U,
            .Threads = 2U,
            .ChildDomains = 2U,
        };
        const auto root = Revocations.CreateRoot();
        if ((Domain.InitializeRoot(ResourceDomainId{5U}, limits).Error != ResourceDomainError::None) ||
            !root.HasValue())
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        if (!registered.HasValue())
        {
            return false;
        }
        RevocationRoot = root.GetValue();
        DomainObject = registered.GetValue();
        Authority = Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Manage});
        InspectOnly = Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Inspect});
        return Authority.IsValid() && InspectOnly.IsValid();
    }

    [[nodiscard]] CapabilityHandle Mint(const ObjectId object, const CapabilityRights rights) noexcept
    {
        const auto minted = Issuer.Mint(Space, object, rights, RevocationRoot, MonotonicTime::Infinite(), 0U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] ObjectManagementResult
    Create(const CapabilityHandle authority, const ObjectType type = ObjectType::Endpoint,
           const EndpointConfiguration configuration = EndpointConfiguration::Default()) noexcept
    {
        return Manager.Create(Space, ObjectCreateRequest{
                                         .Authority = authority,
                                         .Type = type,
                                         .Reserved = {},
                                         .Endpoint = configuration,
                                         .Pages = PageCount{0U},
                                         .MaximumPermissions = {},
                                         .ReservedMemory = {},
                                         .Thread = {},
                                         .DomainAddressSpace = {},
                                         .DomainCapabilities = {},
                                         .SupervisionTag = 0U,
                                         .EndpointReceiver = {},
                                         .ThreadDomain = {},
                                         .EndpointSealedSelector = 0U,
                                         .DomainLimits = {},
                                         .FrameRegion = {},
                                         .FrameRegionOffsetPages = 0U,
                                         .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                         .IommuInputAddressBits = 0U,
                                         .ReservedCache = {},
                                         .InterruptFunction = {},
                                         .InterruptSource = 0U,
                                         .Caller = {},
                                     });
    }

    [[nodiscard]] ObjectManagementResult
    CreateMemory(const ObjectType type, const UInt64 pages,
                 const PagePermissions permissions =
                     PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User),
                 const CapabilityHandle authority = {}) noexcept
    {
        return Manager.Create(Space, ObjectCreateRequest{
                                         .Authority = authority.IsValid() ? authority : Authority,
                                         .Type = type,
                                         .Reserved = {},
                                         .Endpoint = {},
                                         .Pages = PageCount{pages},
                                         .MaximumPermissions = permissions,
                                         .ReservedMemory = {},
                                         .Thread = {},
                                         .DomainAddressSpace = {},
                                         .DomainCapabilities = {},
                                         .SupervisionTag = 0U,
                                         .EndpointReceiver = {},
                                         .ThreadDomain = {},
                                         .EndpointSealedSelector = 0U,
                                         .DomainLimits = {},
                                         .FrameRegion = {},
                                         .FrameRegionOffsetPages = 0U,
                                         .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                         .IommuInputAddressBits = 0U,
                                         .ReservedCache = {},
                                         .InterruptFunction = {},
                                         .InterruptSource = 0U,
                                         .Caller = {},
                                     });
    }

    [[nodiscard]] ResourceDomainUsage GetUsage() const noexcept
    {
        return Domain.GetSnapshot().Usage;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    Asteria::TestSupport::PoolSlabs Slabs;
    ObjectManager Manager;
    RevocationDomainId RevocationRoot{};
    ObjectId DomainObject{};
    CapabilityHandle Authority{};
    CapabilityHandle InspectOnly{};
};

[[nodiscard]] bool Is(const ObjectManagementResult& result, const ObjectManagementError error) noexcept
{
    return result.Error == error;
}

/// Creates an object of type under authority: pages for a Frame or a
/// MemoryObject, limits for a ResourceDomain.
[[nodiscard]] ObjectManagementResult CreateUnder(ManagerFixture& fixture, const CapabilityHandle authority,
                                                 const ObjectType type, const UInt64 pages = 0U,
                                                 const ResourceDomainLimits limits = {},
                                                 const Foundation::UInt8 iommuBits = 0U) noexcept
{
    return fixture.Manager.Create(
        fixture.Space,
        ObjectCreateRequest{
            .Authority = authority,
            .Type = type,
            .Reserved = {},
            .Endpoint = EndpointConfiguration::Default(),
            .Pages = PageCount{pages},
            .MaximumPermissions =
                PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User),
            .ReservedMemory = {},
            .Thread =
                Kernel::Scheduler::UserThreadStart{
                    .Entry = 0x4000'2000U, .UserStackTop = 0x4100'0000U, .Argument = 0U, .Domain = nullptr},
            .DomainAddressSpace = {},
            .DomainCapabilities = {},
            .SupervisionTag = 0U,
            .EndpointReceiver = {},
            .ThreadDomain = {},
            .EndpointSealedSelector = 0U,
            .DomainLimits = limits,
            .FrameRegion = {},
            .FrameRegionOffsetPages = 0U,
            .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
            .IommuInputAddressBits = iommuBits,
            .ReservedCache = {},
            .InterruptFunction = {},
            .InterruptSource = 0U,
            .Caller = {},
        });
}

/// A child of the fixture's domain: four pages, one thread, children.
[[nodiscard]] constexpr ResourceDomainLimits ChildLimits(const UInt64 children) noexcept
{
    return ResourceDomainLimits{.CommittedMemoryPages = 4U,
                                .ResidentMemoryPages = 4U,
                                .KernelMetadataBytes = 0U,
                                .CapabilityHandles = 0U,
                                .IpcQueueBytes = 1U << 16U,
                                .Threads = 1U,
                                .ChildDomains = children};
}

/// Whether handle's object was destroyed: its lookups fail ObjectNotFound.
[[nodiscard]] bool IsStale(ManagerFixture& fixture, const CapabilityHandle handle, const ObjectType type) noexcept
{
    const auto lookup = fixture.Space.Lookup(handle, type, CapabilityOperation::Inspect);
    return !lookup.HasValue() && (lookup.GetError() == CapabilityError::ObjectNotFound);
}

[[nodiscard]] bool IsSameUsage(const ResourceDomainUsage& left, const ResourceDomainUsage& right) noexcept
{
    for (Size type{0U}; type < Abi::LimitedObjectCount; ++type)
    {
        if (left.Objects[type] != right.Objects[type])
        {
            return false;
        }
    }
    return (left.CommittedMemoryPages == right.CommittedMemoryPages) &&
           (left.ResidentMemoryPages == right.ResidentMemoryPages) &&
           (left.KernelMetadataBytes == right.KernelMetadataBytes) &&
           (left.CapabilityHandles == right.CapabilityHandles) && (left.IpcQueueBytes == right.IpcQueueBytes) &&
           (left.Threads == right.Threads) && (left.ChildDomains == right.ChildDomains);
}

void TestCreateDestroyAndCollect(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    const ObjectManagementResult created = fixture.Create(fixture.Authority);
    const bool live = fixture.Space.Lookup(created.Created, ObjectType::Endpoint, CapabilityOperation::Read).HasValue();
    ASTERIA_EXPECT(runner, "the manager creates an Endpoint charged to the grant's domain", ready,
                   Is(created, ObjectManagementError::None), created.Created.IsValid(), live,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == EndpointPool::MetadataBytes,
                   fixture.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::Endpoint)] == 1U,
                   fixture.Manager.GetEndpointSnapshot().ActiveSlots == 1U);

    const ObjectManagementResult destroyed =
        fixture.Manager.Destroy(fixture.Space, created.Created, ObjectType::Endpoint);
    const auto reclaimed = fixture.Manager.GetEndpointSnapshot();
    ASTERIA_EXPECT(runner, "an unreferenced Endpoint is reclaimed before its destruction returns",
                   Is(destroyed, ObjectManagementError::None), reclaimed.QuiescingSlots == 0U,
                   reclaimed.Reclaimed == 1U, reclaimed.FreeSlots == EndpointPool::Capacity,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U,
                   fixture.Domain.GetSnapshot().Usage.IpcQueueBytes == 0U);
    ASTERIA_EXPECT(runner, "Endpoint collection releases its creation unit",
                   fixture.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::Endpoint)] == 0U);

    // A lease another holder keeps defers the reclamation to the next
    // management call.
    const ObjectManagementResult next = fixture.Create(fixture.Authority);
    ObjectManagementResult held{};
    Kernel::Ipc::EndpointPoolSnapshot quiescing{};
    {
        const auto lease = fixture.Space.Lookup(next.Created, ObjectType::Endpoint, CapabilityOperation::Invoke);
        held = fixture.Manager.Destroy(fixture.Space, next.Created, ObjectType::Endpoint);
        quiescing = fixture.Manager.GetEndpointSnapshot();
        static_cast<void>(lease.HasValue());
    }
    const ObjectManagementResult later = fixture.Create(fixture.Authority);
    const auto collected = fixture.Manager.GetEndpointSnapshot();
    const ObjectManagementResult last = fixture.Manager.Destroy(fixture.Space, later.Created, ObjectType::Endpoint);
    const bool swept = fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "a referenced Endpoint is reclaimed by the next management call",
                   Is(next, ObjectManagementError::None), Is(held, ObjectManagementError::None),
                   quiescing.QuiescingSlots == 1U, quiescing.Reclaimed == 1U, Is(later, ObjectManagementError::None),
                   collected.Reclaimed == 2U, collected.ActiveSlots == 1U, collected.QuiescingSlots == 0U,
                   Is(last, ObjectManagementError::None), swept, fixture.Manager.GetEndpointSnapshot().Reclaimed == 3U,
                   fixture.Manager.GetEndpointSnapshot().FreeSlots == EndpointPool::Capacity,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}

void TestErrors(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    EndpointConfiguration invalid = EndpointConfiguration::Default();
    invalid.MaximumQueuedMessages = 0U;
    const ObjectManagementResult unsupported = fixture.Create(fixture.Authority, ObjectType::Notification);
    const ObjectManagementResult unauthorized = fixture.Create(fixture.InspectOnly);
    const ObjectManagementResult misconfigured = fixture.Create(fixture.Authority, ObjectType::Endpoint, invalid);
    const ObjectManagementResult unsupportedDestroy =
        fixture.Manager.Destroy(fixture.Space, fixture.Authority, ObjectType::Notification);
    ASTERIA_EXPECT(runner, "the manager reports unsupported types, authority and requests", ready,
                   Is(unsupported, ObjectManagementError::UnsupportedType),
                   Is(unauthorized, ObjectManagementError::CapabilityRejected),
                   unauthorized.Capability == CapabilityError::RightsDenied,
                   Is(misconfigured, ObjectManagementError::InvalidRequest),
                   Is(unsupportedDestroy, ObjectManagementError::UnsupportedType), !unauthorized.Created.IsValid());

    bool filled{true};
    for (Size index{0U}; index < EndpointPool::Capacity; ++index)
    {
        filled = filled && Is(fixture.Create(fixture.Authority), ObjectManagementError::None);
    }
    const ObjectManagementResult exhausted = fixture.Create(fixture.Authority);

    Endpoint foreign{fixture.Space, ResourceDomainId{5U}};
    const auto foreignObject = fixture.Issuer.CreateObject(foreign);
    const CapabilityHandle foreignHandle = fixture.Mint(
        foreignObject.HasValue() ? foreignObject.GetValue() : ObjectId{}, CapabilityRights{CapabilityRight::Manage});
    const ObjectManagementResult notOwned = fixture.Manager.Destroy(fixture.Space, foreignHandle, ObjectType::Endpoint);
    ASTERIA_EXPECT(runner, "a full pool is exhausted and a foreign Endpoint is not the manager's", filled,
                   Is(exhausted, ObjectManagementError::ResourceExhausted),
                   Is(notOwned, ObjectManagementError::NotOwned));
    if (foreignObject.HasValue())
    {
        static_cast<void>(fixture.Issuer.DestroyObject(foreignObject.GetValue()));
        static_cast<void>(fixture.Issuer.CollectDestroyedObject(foreignObject.GetValue()));
    }
}
// Frames come from the frame pool; MemoryObjects are Pinned. A Frame charges its pages, committed
// and resident, and its metadata, a MemoryObject its metadata only; destroying either, in any
// order, returns its charge, and a destroyed Frame's pages stay dirty.
void TestFramesAndMemoryObjects(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    const ObjectManagementResult unpooled = fixture.CreateMemory(ObjectType::Frame, 1U);
    const bool pooled = fixture.Manager.InitializeFramePool(PhysicalAddress{0x0020'0000U}, 4U) == FramePoolError::None;
    const ObjectManagementResult frame = fixture.CreateMemory(ObjectType::Frame, 2U);
    const ObjectManagementResult memory = fixture.CreateMemory(ObjectType::MemoryObject, 3U);
    const auto frameInfo = fixture.Space.Query(frame.Created);
    const auto memoryInfo = fixture.Space.Query(memory.Created);
    const ResourceDomainUsage charged = fixture.GetUsage();
    ASTERIA_EXPECT(runner, "a Frame charges its pages and a MemoryObject only its metadata", ready,
                   Is(unpooled, ObjectManagementError::ResourceExhausted), pooled,
                   Is(frame, ObjectManagementError::None), Is(memory, ObjectManagementError::None),
                   frameInfo.HasValue() && (frameInfo.GetValue().Type == ObjectType::Frame),
                   memoryInfo.HasValue() && (memoryInfo.GetValue().Type == ObjectType::MemoryObject),
                   frameInfo.HasValue() &&
                       frameInfo.GetValue().Rights.IsEqualTo(Kernel::Memory::FrameObjectPool::CreatedRights),
                   charged.ResidentMemoryPages == 2U, charged.CommittedMemoryPages == 2U,
                   charged.Objects[static_cast<Size>(Abi::LimitedObject::Frame)] == 1U,
                   charged.Objects[static_cast<Size>(Abi::LimitedObject::MemoryObject)] == 1U,
                   charged.KernelMetadataBytes == (sizeof(Kernel::Memory::CreatedFrame) + sizeof(MemoryObject)),
                   fixture.Manager.GetFramePoolSnapshot().AllocatedPages == 2U);

    const ObjectManagementResult tooLarge = fixture.CreateMemory(ObjectType::Frame, 3U);
    const ObjectManagementResult empty = fixture.CreateMemory(ObjectType::Frame, 0U);
    // Seven more pages exceed the domain's eight: the charge refuses the
    // Frame before the pool is asked.
    const ObjectManagementResult overLimit = fixture.CreateMemory(ObjectType::Frame, 7U);
    const ObjectManagementResult noAccess = fixture.CreateMemory(ObjectType::MemoryObject, 1U, PagePermissions{});
    ASTERIA_EXPECT(
        runner, "the frame pool, the page count, the domain and the permissions bound creation",
        Is(tooLarge, ObjectManagementError::ResourceExhausted), Is(empty, ObjectManagementError::InvalidRequest),
        Is(overLimit, ObjectManagementError::ResourceExhausted), Is(noAccess, ObjectManagementError::InvalidRequest),
        fixture.GetUsage().ResidentMemoryPages == 2U, fixture.GetUsage().CommittedMemoryPages == 2U,
        fixture.Manager.GetFramePoolSnapshot().FreePages == 2U);

    // The MemoryObject goes first: no order between the two is required.
    const ObjectManagementResult memoryGone =
        fixture.Manager.Destroy(fixture.Space, memory.Created, ObjectType::MemoryObject);
    const ObjectManagementResult frameGone = fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame);
    const ObjectManagementResult wrongType =
        fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::MemoryObject);
    const auto pool = fixture.Manager.GetFramePoolSnapshot();
    ASTERIA_EXPECT(runner, "destroyed Frames and MemoryObjects return their charge, and Frame pages stay dirty",
                   Is(frameGone, ObjectManagementError::None), Is(memoryGone, ObjectManagementError::None),
                   Is(wrongType, ObjectManagementError::CapabilityRejected),
                   fixture.GetUsage().ResidentMemoryPages == 0U, fixture.GetUsage().CommittedMemoryPages == 0U,
                   fixture.GetUsage().KernelMetadataBytes == 0U, pool.DirtyPages == 2U, pool.FreePages == 2U,
                   pool.AllocatedPages == 0U, fixture.Manager.GetFrameSnapshot().Reclaimed == 1U,
                   fixture.Manager.GetMemoryObjectSnapshot().Reclaimed == 1U);
}

// Code is required alongside Manage, and ordinary derivation and duplication may preserve or drop
// it but never add it.
void TestCodeAuthority(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    constexpr PagePermissions executable =
        PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
    const CapabilityRights rights = CapabilityRights{CapabilityRight::Manage}
                                        .With(CapabilityRight::Inspect)
                                        .With(CapabilityRight::Code)
                                        .With(CapabilityRight::Derive)
                                        .With(CapabilityRight::Duplicate);
    const CapabilityHandle code = fixture.Mint(fixture.DomainObject, rights);
    const ResourceDomainUsage before = fixture.GetUsage();
    const auto refused = fixture.CreateMemory(ObjectType::MemoryObject, 1U, executable);
    const auto attenuated = fixture.Space.Derive(
        code, CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Duplicate), MonotonicTime::Infinite());
    const auto stripped = fixture.CreateMemory(ObjectType::MemoryObject, 1U, executable,
                                               attenuated.HasValue() ? attenuated.GetValue() : fixture.Authority);
    const auto amplified =
        attenuated.HasValue()
            ? fixture.Space.Duplicate(attenuated.GetValue(),
                                      CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Code),
                                      MonotonicTime::Infinite())
            : fixture.Space.Duplicate(fixture.Authority, rights, MonotonicTime::Infinite());
    ASTERIA_EXPECT(runner, "executable MemoryObjects require Code even after attenuation, with no charge on refusal",
                   ready, code.IsValid(), Is(refused, ObjectManagementError::CapabilityRejected),
                   refused.Capability == CapabilityError::RightsDenied, !refused.Created.IsValid(),
                   attenuated.HasValue(), Is(stripped, ObjectManagementError::CapabilityRejected),
                   stripped.Capability == CapabilityError::RightsDenied, !stripped.Created.IsValid(),
                   IsSameUsage(before, fixture.GetUsage()), !amplified.HasValue(),
                   amplified.GetError() == CapabilityError::RightsAmplification);
    const auto copy = fixture.Space.Duplicate(code, rights, MonotonicTime::Infinite());
    const auto admitted = fixture.CreateMemory(ObjectType::MemoryObject, 1U, executable, code);
    const auto copied = fixture.CreateMemory(ObjectType::MemoryObject, 1U, executable,
                                             copy.HasValue() ? copy.GetValue() : fixture.Authority);
    const auto dropped =
        fixture.Space.Duplicate(code, CapabilityRights{CapabilityRight::Manage}, MonotonicTime::Infinite());
    const auto copyRefused = fixture.CreateMemory(ObjectType::MemoryObject, 1U, executable,
                                                  dropped.HasValue() ? dropped.GetValue() : fixture.Authority);
    ASTERIA_EXPECT(runner, "Code survives duplication and can be dropped, with its ABI encoding at bit 13",
                   CapabilityRights{CapabilityRight::Code}.GetRawValue() == Abi::CapabilityRightCode,
                   Abi::CapabilityRightCode == 0x2000U, copy.HasValue(), dropped.HasValue(),
                   Is(admitted, ObjectManagementError::None), Is(copied, ObjectManagementError::None),
                   Is(copyRefused, ObjectManagementError::CapabilityRejected),
                   copyRefused.Capability == CapabilityError::RightsDenied,
                   fixture.Manager.GetMemoryObjectSnapshot().ActiveSlots == 2U);
}

// An AddressSpace on a test page table, registered and bound in fixture's
// registry, with a Map capability in its space, after fixture's frame pool,
// mapper and ResourceDomain pool are configured.
class MappedSpace final
{
public:
    [[nodiscard]] bool Initialize(ManagerFixture& fixture) noexcept
    {
        using Kernel::Memory::AddressSpaceError;
        using Kernel::Memory::InitialMapping;
        using Kernel::Memory::InitialMappingOwner;
        using Kernel::Memory::InitialMappingPlanError;
        using Kernel::Memory::MappingPermission;
        using Kernel::Memory::MappingPermissions;
        using Kernel::Memory::VirtualAddress;
        const auto seedMapping = [](const UInt64 offset, const UInt64 pages, const MappingPermissions permissions,
                                    const InitialMappingOwner owner) noexcept
        {
            return InitialMapping{
                .VirtualStart = VirtualAddress{0xFFFF'8000'0000'0000ULL + offset},
                .PhysicalStart = PhysicalAddress{0x0010'0000U + offset},
                .Pages = PageCount{pages},
                .Permissions = permissions,
                .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                .Owner = owner,
                .Reserved = 0U,
            };
        };
        const MappingPermissions readWrite = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
        const bool seeded =
            (Seed.AddMapping(seedMapping(0x1000U, 2U,
                                         MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
                                         InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
            (Seed.AddMapping(seedMapping(0x4000U, 4U, readWrite, InitialMappingOwner::NucleusStack)) ==
             InitialMappingPlanError::None) &&
            (Seed.AddMapping(seedMapping(0x8000U, 16U, readWrite, InitialMappingOwner::PageTable)) ==
             InitialMappingPlanError::None) &&
            (Seed.Seal() == InitialMappingPlanError::None);
        if (!seeded ||
            (fixture.Manager.InitializeFramePool(PhysicalAddress{0x0020'0000U}, 4U) != FramePoolError::None) ||
            !fixture.Manager.ConfigureMapping(Shootdown, Transport, Progress) ||
            !fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U}) ||
            (Space.Initialize(ResourceDomainId{10U}, Table, Seed) != AddressSpaceError::None))
        {
            return false;
        }
        const auto registered = fixture.Issuer.CreateObject(Space);
        if (!registered.HasValue() ||
            (Space
                 .BindIdentity(fixture.Registry,
                               Kernel::Memory::AddressSpaceId{fixture.Registry, registered.GetValue()})
                 .Error != AddressSpaceError::None))
        {
            return false;
        }
        Registered = registered.GetValue();
        Handle = fixture.Mint(Registered, CapabilityRights{CapabilityRight::Map});
        return Handle.IsValid();
    }

    void Release(ManagerFixture& fixture) noexcept
    {
        if (Registered.IsValid())
        {
            static_cast<void>(Space.Quiesce());
            static_cast<void>(fixture.Issuer.DestroyObject(Registered));
            static_cast<void>(fixture.Issuer.CollectDestroyedObject(Registered));
        }
    }

    Kernel::Memory::InitialMappingPlan Seed;
    TestPageTable Table;
    Kernel::Memory::AddressSpace Space;
    TestInvalidator Invalidator;
    TestTransport Transport;
    TestProgress Progress;
    Kernel::Memory::TlbShootdownDispatcher Shootdown{Kernel::Scheduler::CpuId{0U}, Invalidator};
    ObjectId Registered{};
    CapabilityHandle Handle{};
};

// Two prepared spaces on the fixture's sealed seed, owned by the manager
// So a ResourceDomain drain exercises AddressSpacePool's quiesce path.
class SharedSpaceProvider final : public Kernel::Memory::IAddressSpaceProvider
{
public:
    explicit SharedSpaceProvider(const Kernel::Memory::InitialMappingPlan& seed) noexcept : m_seed{seed}
    {
    }

    [[nodiscard]] Size GetSlotCount() const noexcept override
    {
        return 2U;
    }
    [[nodiscard]] UInt64 GetSlotBytes(bool = false) const noexcept override
    {
        return sizeof(Kernel::Memory::AddressSpace);
    }

    [[nodiscard]] Kernel::Memory::AddressSpace* InitializeSlot(const Size slot, const ResourceDomainId owner,
                                                               bool = false) noexcept override
    {
        return (slot < 2U) &&
                       (Spaces[slot].Initialize(owner, Tables[slot], m_seed) == Kernel::Memory::AddressSpaceError::None)
                   ? &Spaces[slot]
                   : nullptr;
    }

    [[nodiscard]] bool ReleaseSlot(const Size slot) noexcept override
    {
        if ((slot >= 2U) || (Spaces[slot].GetSnapshot().State != Kernel::Memory::AddressSpaceState::Quiesced))
        {
            return false;
        }
        Spaces[slot].~AddressSpace();
        ::new (static_cast<void*>(&Spaces[slot])) Kernel::Memory::AddressSpace{};
        return true;
    }

    Kernel::Memory::AddressSpace Spaces[2U];
    TestPageTable Tables[2U];

private:
    const Kernel::Memory::InitialMappingPlan& m_seed;
};

// Shared executable segments first step: one sealed RX object and Frame charged to a surviving
// third domain, mapped into two child domains.
void TestSharedCodeSurvivesDrain(TestContext& runner) noexcept
{
    using Kernel::Memory::VirtualAddress;
    static ManagerFixture fixture;
    static MappedSpace bootstrap;
    static SharedSpaceProvider provider{bootstrap.Seed};
    const bool ready =
        fixture.Initialize() && bootstrap.Initialize(fixture) && fixture.Manager.ConfigureAddressSpaces(provider);
    const auto first = CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const auto second = CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const auto firstSpace = CreateUnder(fixture, first.Created, ObjectType::AddressSpace);
    const auto secondSpace = CreateUnder(fixture, second.Created, ObjectType::AddressSpace);
    const CapabilityHandle code =
        fixture.Mint(fixture.DomainObject, CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Code));
    constexpr PagePermissions executable =
        PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User);
    constexpr PagePermissions writable =
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
    const auto frame = fixture.CreateMemory(ObjectType::Frame, 1U);
    const auto sealed = fixture.CreateMemory(ObjectType::MemoryObject, 1U, executable, code);
    const auto staging = fixture.CreateMemory(ObjectType::MemoryObject, 1U, writable);
    const auto map = [&](const CapabilityHandle space, const CapabilityHandle memory, const PagePermissions permissions,
                         const UInt64 address = 0x4C00'0000U) noexcept
    {
        return fixture.Manager.Map(fixture.Space, Kernel::Memory::MemoryMapRequest{
                                                      .AddressSpace = space,
                                                      .MemoryObject = memory,
                                                      .Frame = frame.Created,
                                                      .Mapping =
                                                          Kernel::Memory::PinnedFrameMappingRequest{
                                                              .VirtualStart = VirtualAddress{address},
                                                              .Pages = PageCount{1U},
                                                              .MemoryObjectOffsetPages = 0U,
                                                              .FrameOffsetPages = 0U,
                                                              .Permissions = permissions,
                                                              .Reserved = {},
                                                          },
                                                  });
    };
    const auto mappedFirst = map(firstSpace.Created, sealed.Created, executable);
    const auto mappedSecond = map(secondSpace.Created, sealed.Created, executable);
    const auto memoryLease = fixture.Space.Lookup(sealed.Created, ObjectType::MemoryObject, CapabilityOperation::Map);
    const auto frameLease = fixture.Space.Lookup(frame.Created, ObjectType::Frame, CapabilityOperation::Map);
    ASTERIA_EXPECT(runner, "one sealed RX MemoryObject maps one Frame into two independently charged spaces", ready,
                   Is(first, ObjectManagementError::None), Is(second, ObjectManagementError::None),
                   Is(firstSpace, ObjectManagementError::None), Is(secondSpace, ObjectManagementError::None),
                   Is(frame, ObjectManagementError::None), Is(sealed, ObjectManagementError::None),
                   Is(staging, ObjectManagementError::None), Is(mappedFirst, ObjectManagementError::None),
                   Is(mappedSecond, ObjectManagementError::None), memoryLease.HasValue(), frameLease.HasValue());
    if (!memoryLease.HasValue() || !frameLease.HasValue())
    {
        return;
    }
    auto& memory = static_cast<MemoryObject&>(memoryLease.GetValue().GetObject());
    auto& backing = static_cast<Frame&>(frameLease.GetValue().GetObject());
    const auto refused = map(secondSpace.Created, staging.Created, writable, 0x4D00'0000U);
    const auto sealedRefused = map(secondSpace.Created, sealed.Created, writable, 0x4E00'0000U);
    ASTERIA_EXPECT(runner, "two executable aliases forbid writing the Frame, and the sealed object forbids Write",
                   memory.GetSnapshot().ActiveMappings == 2U,
                   memory.GetSnapshot().Descriptor.MaximumPermissions.GetBits() == executable.GetBits(),
                   Is(refused, ObjectManagementError::InvalidRequest),
                   Is(sealedRefused, ObjectManagementError::InvalidRequest), memory.GetSnapshot().ActiveMappings == 2U);
    const auto drained = fixture.Manager.Destroy(fixture.Space, first.Created, ObjectType::ResourceDomain);
    const auto survivor = provider.Spaces[1U].FindUserMapping(0x4C00'0000U, 4096U, false);
    const auto stillRefused = map(secondSpace.Created, staging.Created, writable, 0x4D00'0000U);
    ASTERIA_EXPECT(runner, "draining one domain releases one mapping and leaves the other, the Frame and code alive",
                   Is(drained, ObjectManagementError::None),
                   IsStale(fixture, first.Created, ObjectType::ResourceDomain),
                   IsStale(fixture, firstSpace.Created, ObjectType::AddressSpace), survivor.HasValue(),
                   survivor.HasValue() && survivor.GetValue().Permissions.Contains(PagePermission::Execute),
                   memory.GetSnapshot().ActiveMappings == 1U,
                   memory.GetSnapshot().State == Kernel::Memory::MemoryObjectState::Ready,
                   memory.GetChargeOwner().GetValue() == 5U, backing.GetChargeOwner().GetValue() == 5U,
                   backing.IsInitialized(), fixture.Space.Query(frame.Created).HasValue(),
                   fixture.Space.Query(sealed.Created).HasValue(), fixture.Manager.GetFrameSnapshot().ActiveSlots == 1U,
                   fixture.Manager.GetMemoryObjectSnapshot().ActiveSlots == 2U,
                   Is(stillRefused, ObjectManagementError::InvalidRequest));
    const auto unmapped =
        fixture.Manager.Unmap(fixture.Space, secondSpace.Created, VirtualAddress{0x4C00'0000U}, PageCount{1U});
    const auto nowWritable = map(secondSpace.Created, staging.Created, writable, 0x4D00'0000U);
    ASTERIA_EXPECT(runner, "writing is admitted only after the final executable mapping is gone",
                   Is(unmapped, ObjectManagementError::None), memory.GetSnapshot().ActiveMappings == 0U,
                   Is(nowWritable, ObjectManagementError::None));
}

// A Frame destroyed while a space maps it stays allocated, pinned by the AddressSpace; MemoryUnmap
// releases the pin and reclaims it at once. A drained domain's mapped objects, and so the domain,
// Stay the same way.
void TestUnmapReclaimsADestroyedFrame(TestContext& runner) noexcept
{
    using Kernel::Memory::VirtualAddress;
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    static MappedSpace target{};
    const bool ready = fixture.Initialize() && target.Initialize(fixture);
    const CapabilityHandle spaceHandle = target.Handle;
    const ObjectManagementResult frame = fixture.CreateMemory(ObjectType::Frame, 2U);
    const ObjectManagementResult memory = fixture.CreateMemory(ObjectType::MemoryObject, 2U);
    const ObjectManagementResult mapped = fixture.Manager.Map(
        fixture.Space,
        Kernel::Memory::MemoryMapRequest{
            .AddressSpace = spaceHandle,
            .MemoryObject = memory.Created,
            .Frame = frame.Created,
            .Mapping =
                Kernel::Memory::PinnedFrameMappingRequest{
                    .VirtualStart = VirtualAddress{0x4C00'0000U},
                    .Pages = PageCount{2U},
                    .MemoryObjectOffsetPages = 0U,
                    .FrameOffsetPages = 0U,
                    .Permissions =
                        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User),
                    .Reserved = {},
                },
        });
    const ObjectManagementResult frameGone = fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame);
    const auto held = fixture.Manager.GetFramePoolSnapshot();
    const auto heldSlots = fixture.Manager.GetFrameSnapshot();
    ASTERIA_EXPECT(runner, "a destroyed Frame stays allocated while a space maps it", ready, spaceHandle.IsValid(),
                   Is(frame, ObjectManagementError::None), Is(memory, ObjectManagementError::None),
                   Is(mapped, ObjectManagementError::None), Is(frameGone, ObjectManagementError::None),
                   held.AllocatedPages == 2U, held.DirtyPages == 0U, heldSlots.QuiescingSlots == 1U,
                   heldSlots.Reclaimed == 0U);

    const ObjectManagementResult absent =
        fixture.Manager.Unmap(fixture.Space, spaceHandle, VirtualAddress{0x4C00'1000U}, PageCount{1U});
    const ObjectManagementResult unmapped =
        fixture.Manager.Unmap(fixture.Space, spaceHandle, VirtualAddress{0x4C00'0000U}, PageCount{2U});
    const auto pool = fixture.Manager.GetFramePoolSnapshot();
    const ObjectManagementResult memoryGone =
        fixture.Manager.Destroy(fixture.Space, memory.Created, ObjectType::MemoryObject);
    ASTERIA_EXPECT(runner, "unmapping it reclaims the Frame before the call returns, its pages dirty",
                   Is(absent, ObjectManagementError::InvalidRequest), Is(unmapped, ObjectManagementError::None),
                   pool.AllocatedPages == 0U, pool.DirtyPages == 2U, fixture.Manager.GetFrameSnapshot().Reclaimed == 1U,
                   fixture.Manager.GetFrameSnapshot().QuiescingSlots == 0U, Is(memoryGone, ObjectManagementError::None),
                   fixture.GetUsage().ResidentMemoryPages == 0U, fixture.GetUsage().KernelMetadataBytes == 0U,
                   fixture.Manager.GetMapperSnapshot().Unmappings == 1U);

    const ObjectManagementResult child =
        CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const ObjectManagementResult childFrame = CreateUnder(fixture, child.Created, ObjectType::Frame, 2U);
    const ObjectManagementResult childMemory = CreateUnder(fixture, child.Created, ObjectType::MemoryObject, 2U);
    const ObjectManagementResult childMapped = fixture.Manager.Map(
        fixture.Space,
        Kernel::Memory::MemoryMapRequest{
            .AddressSpace = spaceHandle,
            .MemoryObject = childMemory.Created,
            .Frame = childFrame.Created,
            .Mapping =
                Kernel::Memory::PinnedFrameMappingRequest{
                    .VirtualStart = VirtualAddress{0x4C00'0000U},
                    .Pages = PageCount{2U},
                    .MemoryObjectOffsetPages = 0U,
                    .FrameOffsetPages = 0U,
                    .Permissions =
                        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User),
                    .Reserved = {},
                },
        });
    const ObjectManagementResult drained =
        fixture.Manager.Destroy(fixture.Space, child.Created, ObjectType::ResourceDomain);
    const auto stalled = fixture.Manager.GetResourceDomainSnapshot();
    ASTERIA_EXPECT(runner, "a drained domain whose objects a surviving space maps stays until it unmaps them",
                   Is(child, ObjectManagementError::None), Is(childFrame, ObjectManagementError::None),
                   Is(childMemory, ObjectManagementError::None), Is(childMapped, ObjectManagementError::None),
                   Is(drained, ObjectManagementError::None),
                   IsStale(fixture, child.Created, ObjectType::ResourceDomain),
                   IsStale(fixture, childFrame.Created, ObjectType::Frame), stalled.QuiescingSlots == 1U,
                   fixture.Manager.GetFrameSnapshot().QuiescingSlots == 1U,
                   fixture.Manager.GetMemoryObjectSnapshot().QuiescingSlots == 1U,
                   fixture.GetUsage().ChildDomains == 1U, fixture.Manager.GetFramePoolSnapshot().AllocatedPages == 2U);

    const ObjectManagementResult childUnmapped =
        fixture.Manager.Unmap(fixture.Space, spaceHandle, VirtualAddress{0x4C00'0000U}, PageCount{2U});
    const auto domains = fixture.Manager.GetResourceDomainSnapshot();
    ASTERIA_EXPECT(runner, "the unmap reclaims the drained objects and then their domain",
                   Is(childUnmapped, ObjectManagementError::None), domains.QuiescingSlots == 0U,
                   domains.Reclaimed == 1U, fixture.Manager.GetFrameSnapshot().QuiescingSlots == 0U,
                   fixture.Manager.GetMemoryObjectSnapshot().QuiescingSlots == 0U,
                   fixture.GetUsage().ChildDomains == 0U, fixture.GetUsage().ResidentMemoryPages == 0U,
                   fixture.GetUsage().KernelMetadataBytes == 0U,
                   fixture.Manager.GetFramePoolSnapshot().AllocatedPages == 0U);
    target.Release(fixture);
}

// A device Frame of a memory-mapped region the caller may Map maps with an uncached MemoryObject
// through MemoryMap, never executable and never with a write-back MemoryObject; it charges no
// pages, and the region counts the mapping until MemoryUnmap.
void TestDeviceMapping(TestContext& runner, const Kernel::Memory::MemoryCachePolicy publication) noexcept
{
    using Kernel::Device::DeviceAccessWidth;
    using Kernel::Device::DeviceAccessWidths;
    using Kernel::Device::DeviceRegion;
    using Kernel::Device::DeviceRegionDescriptor;
    using Kernel::Device::DeviceRegionError;
    using Kernel::Device::DeviceRegionKind;
    using Kernel::Device::DeviceRegionPermission;
    using Kernel::Device::DeviceRegionPermissions;
    using Kernel::Memory::MemoryCachePolicy;
    using Kernel::Memory::VirtualAddress;
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixtures[2U];
    ManagerFixture& fixture = fixtures[publication == MemoryCachePolicy::Uncached ? 0U : 1U];
    static MappedSpace targets[2U];
    MappedSpace& target = targets[publication == MemoryCachePolicy::Uncached ? 0U : 1U];
    static DeviceRegion regions[2U];
    DeviceRegion& region = regions[publication == MemoryCachePolicy::Uncached ? 0U : 1U];
    const bool ready = fixture.Initialize() && target.Initialize(fixture);
    const auto domain = fixture.Issuer.CreateObject(fixture.Domain);
    const bool initialized =
        domain.HasValue() &&
        (region
             .Initialize(fixture.Registry,
                         Kernel::Resource::ResourceDomainObjectId{fixture.Registry, domain.GetValue()}, fixture.Domain,
                         DeviceRegionDescriptor{
                             .BaseAddress = 0x0000'0000'FEB0'0000ULL,
                             .LengthBytes = 0x2000U,
                             .Kind = DeviceRegionKind::MemoryMappedIo,
                             .Permissions = DeviceRegionPermissions{DeviceRegionPermission::Read}.With(
                                 DeviceRegionPermission::Write),
                             .Widths = DeviceAccessWidths{DeviceAccessWidth::DoubleWord},
                             .CachePolicy = publication,
                             .Reserved = {},
                         })
             .Error == DeviceRegionError::None);
    const auto registered = fixture.Issuer.CreateObject(region);
    const CapabilityHandle regionHandle =
        fixture.Mint(registered.HasValue() ? registered.GetValue() : ObjectId{},
                     CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Inspect));
    const PagePermissions readWrite =
        PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User);
    const auto create =
        [&](const ObjectType type, const CapabilityHandle frameRegion, const MemoryCachePolicy policy) noexcept
    {
        return fixture.Manager.Create(fixture.Space, ObjectCreateRequest{
                                                         .Authority = fixture.Authority,
                                                         .Type = type,
                                                         .Reserved = {},
                                                         .Endpoint = {},
                                                         .Pages = PageCount{1U},
                                                         .MaximumPermissions = readWrite,
                                                         .ReservedMemory = {},
                                                         .Thread = {},
                                                         .DomainAddressSpace = {},
                                                         .DomainCapabilities = {},
                                                         .SupervisionTag = 0U,
                                                         .EndpointReceiver = {},
                                                         .ThreadDomain = {},
                                                         .EndpointSealedSelector = 0U,
                                                         .DomainLimits = {},
                                                         .FrameRegion = frameRegion,
                                                         .FrameRegionOffsetPages = 1U,
                                                         .MemoryCachePolicy = policy,
                                                         .IommuInputAddressBits = 0U,
                                                         .ReservedCache = {},
                                                         .InterruptFunction = {},
                                                         .InterruptSource = 0U,
                                                         .Caller = {},
                                                     });
    };
    const ObjectManagementResult frame = create(ObjectType::Frame, regionHandle, MemoryCachePolicy::WriteBack);
    const ObjectManagementResult uncached = create(ObjectType::MemoryObject, {}, MemoryCachePolicy::Uncached);
    const ObjectManagementResult combining = create(ObjectType::MemoryObject, {}, MemoryCachePolicy::WriteCombining);
    const ObjectManagementResult cached = create(ObjectType::MemoryObject, {}, MemoryCachePolicy::WriteBack);
    const auto map =
        [&](const CapabilityHandle memory, const UInt64 virtualStart, const PagePermissions permissions) noexcept
    {
        return fixture.Manager.Map(fixture.Space, Kernel::Memory::MemoryMapRequest{
                                                      .AddressSpace = target.Handle,
                                                      .MemoryObject = memory,
                                                      .Frame = frame.Created,
                                                      .Mapping =
                                                          Kernel::Memory::PinnedFrameMappingRequest{
                                                              .VirtualStart = VirtualAddress{virtualStart},
                                                              .Pages = PageCount{1U},
                                                              .MemoryObjectOffsetPages = 0U,
                                                              .FrameOffsetPages = 0U,
                                                              .Permissions = permissions,
                                                              .Reserved = {},
                                                          },
                                                  });
    };
    const ObjectManagementResult combined = map(combining.Created, 0x4D00'0000U, readWrite);
    if (publication == MemoryCachePolicy::WriteCombining)
    {
        ASTERIA_EXPECT(
            runner, "WC publication maps a WC MemoryObject end to end and refuses a live UC alias",
            Is(combined, ObjectManagementError::None),
            region.GetMappingCachePolicy() == MemoryCachePolicy::WriteCombining,
            Is(map(uncached.Created, 0x4D00'1000U, readWrite), ObjectManagementError::InvalidRequest),
            Is(fixture.Manager.Unmap(fixture.Space, target.Handle, VirtualAddress{0x4D00'0000U}, PageCount{1U}),
               ObjectManagementError::None));
    }
    else
    {
        ASTERIA_EXPECT(runner, "UC register publication refuses WC",
                       Is(combined, ObjectManagementError::InvalidRequest));
    }
    const ObjectManagementResult mismatched = map(cached.Created, 0x4D00'0000U, readWrite);
    const ObjectManagementResult executable =
        map(uncached.Created, 0x4D00'0000U,
            PagePermissions{PagePermission::Read}.With(PagePermission::Execute).With(PagePermission::User));
    const ObjectManagementResult mapped = map(uncached.Created, 0x4D00'0000U, readWrite);
    const auto last = fixture.Manager.GetMapperSnapshot().LastMapping;
    // The kernel's user-memory access never reads through a device mapping.
    const auto userMapping = target.Space.FindUserMapping(0x4D00'0000U, 8U, false);
    const UInt64 retained = region.GetSnapshot().ActiveMappings;
    ASTERIA_EXPECT(
        runner, "a device Frame maps uncached, never executable or with a write-back MemoryObject", ready, initialized,
        regionHandle.IsValid(), Is(frame, ObjectManagementError::None), Is(uncached, ObjectManagementError::None),
        Is(cached, ObjectManagementError::None), Is(mismatched, ObjectManagementError::InvalidRequest),
        Is(executable, ObjectManagementError::InvalidRequest), Is(mapped, ObjectManagementError::None), retained == 1U,
        region.GetMappingCachePolicy() == MemoryCachePolicy::Uncached, last.VirtualStart.GetValue() == 0x4D00'0000U,
        last.PhysicalStart.GetValue() == 0xFEB0'1000ULL, !userMapping.HasValue(),
        fixture.GetUsage().CommittedMemoryPages == 0U, fixture.GetUsage().ResidentMemoryPages == 0U);

    const ObjectManagementResult unmapped =
        fixture.Manager.Unmap(fixture.Space, target.Handle, VirtualAddress{0x4D00'0000U}, PageCount{1U});
    const ObjectManagementResult frameGone = fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame);
    ASTERIA_EXPECT(runner, "unmapping it releases the region, and the device Frame goes with its metadata",
                   Is(unmapped, ObjectManagementError::None), region.GetSnapshot().ActiveMappings == 0U,
                   Is(frameGone, ObjectManagementError::None), fixture.Manager.GetFrameSnapshot().Reclaimed == 1U,
                   fixture.Manager.GetFramePoolSnapshot().AllocatedPages == 0U);
    static_cast<void>(fixture.Manager.Destroy(fixture.Space, combining.Created, ObjectType::MemoryObject));
    static_cast<void>(fixture.Manager.Destroy(fixture.Space, uncached.Created, ObjectType::MemoryObject));
    static_cast<void>(fixture.Manager.Destroy(fixture.Space, cached.Created, ObjectType::MemoryObject));
    target.Release(fixture);
}

// Collect visits released pages within its budget. Initial dirty RAM remains
// available for zeroing when a frame is created.
void TestImportedCollectionBudget(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    alignas(4096) static unsigned char pool[128U * 4096U];
    using Allocator = Kernel::Memory::PhysicalPageAllocator;
    alignas(8) static UInt64 metadata[(Allocator::MetadataBytes(512U) + 7U) / 8U]{};
    const Kernel::Memory::PhysicalPageImportRange range{PhysicalAddress{0x200000U}, 128U};
    TestScrubber scrubber;
    scrubber.Storage = pool;
    scrubber.PhysicalBase = 0x200000U;
    auto& allocator = fixture.Manager.GetPhysicalPageAllocator();
    const bool ready = fixture.Initialize() && fixture.Manager.ConfigureScrubber(scrubber) &&
                       allocator.Import(&range, 1U, metadata, sizeof(metadata)).HasValue();
    for (auto& byte : pool)
    {
        byte = 0xA5U;
    }
    const bool initial = fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "imported dirtiness does not add work to management collection", ready, initial,
                   scrubber.Calls == 0U, allocator.GetSnapshot().DirtyPages == 128U);
    const auto owned = allocator.Allocate(32U);
    const bool released =
        owned.HasValue() && allocator.Release(owned.GetValue()) == Kernel::Memory::PhysicalPageAllocatorError::None;
    const bool first = fixture.Manager.Collect();
    const auto after = allocator.GetSnapshot();
    ASTERIA_EXPECT(runner, "collection services only the released run and bounds one visit to sixteen pages", released,
                   first, scrubber.Calls == 1U, scrubber.LastPages == 16U, after.CleanPages == 16U,
                   after.DirtyPages == 112U, pool[0U] == 0xA5U);
    const bool second = fixture.Manager.Collect() && fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "the rest of the released run is scrubbed without clearing initial RAM", second,
                   scrubber.Calls == 2U, allocator.GetSnapshot().CleanPages == 32U,
                   allocator.GetSnapshot().DirtyPages == 96U);
    const auto background = allocator.ClaimDirty();
    ASTERIA_EXPECT(runner, "the general dirty claim still sees initial RAM for idle scrubbing",
                   background.HasValue() && background.GetValue().PhysicalStart.GetValue() == range.Start.GetValue());
    if (background.HasValue())
    {
        static_cast<void>(allocator.AbortScrub(background.GetValue()));
    }
}

// A destroyed Frame's pages go back to the free set through the scrubber, zeroed, at the collection
// that reclaims them.
void TestScrubbedFramesAreReused(TestContext& runner) noexcept
{
    alignas(2U * 1024U * 1024U) static unsigned char pool[4U * 4096U];
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    TestScrubber scrubber;
    constexpr UInt64 base{0x0020'0000U};
    scrubber.Storage = pool;
    scrubber.PhysicalBase = base;
    const bool ready = fixture.Initialize() &&
                       (fixture.Manager.InitializeFramePool(PhysicalAddress{base}, 4U) == FramePoolError::None) &&
                       fixture.Manager.ConfigureScrubber(scrubber) && !fixture.Manager.ConfigureScrubber(scrubber);
    const ObjectManagementResult frame = fixture.CreateMemory(ObjectType::Frame, 3U);
    for (unsigned char& byte : pool)
    {
        byte = 0xA5U;
    }
    const ObjectManagementResult destroyed = fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame);
    const auto afterDestroy = fixture.Manager.GetFramePoolSnapshot();
    bool zero{true};
    for (Size index{0U}; index < (3U * 4096U); ++index)
    {
        zero = zero && (pool[index] == 0U);
    }
    const ObjectManagementResult reused = fixture.CreateMemory(ObjectType::Frame, 4U);
    // Every collection and scrub is counted.
    const auto collected = fixture.Manager.GetCollectionSnapshot();
    ASTERIA_EXPECT(runner, "collections, the slots they reclaimed and the runs scrubbed are counted",
                   collected.Collections >= 3U, collected.Reclaimed == 1U, collected.ScrubRuns == 1U,
                   collected.ScrubbedPages == 3U);
    ASTERIA_EXPECT(runner, "the collection that reclaims a Frame zeroes its run and frees it", ready,
                   Is(frame, ObjectManagementError::None), Is(destroyed, ObjectManagementError::None),
                   scrubber.Calls == 1U, scrubber.LastStart == base, scrubber.LastPages == 3U, zero,
                   pool[3U * 4096U] == 0xA5U, afterDestroy.FreePages == 4U, afterDestroy.DirtyPages == 0U,
                   afterDestroy.ScrubbingPages == 0U, Is(reused, ObjectManagementError::None),
                   fixture.Manager.GetFramePoolSnapshot().AllocatedPages == 4U);

    const ObjectManagementResult reusedGone = fixture.Manager.Destroy(fixture.Space, reused.Created, ObjectType::Frame);
    scrubber.Answer = Kernel::Memory::FrameScrubError::Deferred;
    const ObjectManagementResult deferringFrame = fixture.CreateMemory(ObjectType::Frame, 1U);
    const ObjectManagementResult deferred =
        fixture.Manager.Destroy(fixture.Space, deferringFrame.Created, ObjectType::Frame);
    const auto afterDeferral = fixture.Manager.GetFramePoolSnapshot();
    scrubber.Answer = Kernel::Memory::FrameScrubError::None;
    const bool drained = fixture.Manager.Collect();
    const auto afterDrain = fixture.Manager.GetFramePoolSnapshot();
    ASTERIA_EXPECT(runner, "a deferred scrub leaves the run dirty for a later collection",
                   Is(reusedGone, ObjectManagementError::None), Is(deferringFrame, ObjectManagementError::None),
                   Is(deferred, ObjectManagementError::None), afterDeferral.DirtyPages == 1U,
                   afterDeferral.ScrubbingPages == 0U, drained, afterDrain.DirtyPages == 0U,
                   afterDrain.FreePages == 4U);

    const ObjectManagementResult faultingFrame = fixture.CreateMemory(ObjectType::Frame, 2U);
    scrubber.Answer = Kernel::Memory::FrameScrubError::Faulted;
    const ObjectManagementResult faulted =
        fixture.Manager.Destroy(fixture.Space, faultingFrame.Created, ObjectType::Frame);
    const auto afterFault = fixture.Manager.GetFramePoolSnapshot();
    ASTERIA_EXPECT(runner, "a faulted scrub fails the management call and keeps the run out of the pool",
                   Is(faultingFrame, ObjectManagementError::None), Is(faulted, ObjectManagementError::Faulted),
                   afterFault.ScrubbingPages == 2U, afterFault.FreePages == 2U);
}

// A backend whose threads start at once and whose native slots release when the test says so.
// ObjectCreate and ObjectDestroy for WaitSet (type 12).
void TestWaitSets(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    const ObjectManagementResult created = fixture.Create(fixture.Authority, ObjectType::WaitSet);
    const bool live = fixture.Space.Lookup(created.Created, ObjectType::WaitSet, CapabilityOperation::Wait).HasValue();
    const auto active = fixture.Manager.GetWaitSetSnapshot();
    const ObjectManagementResult destroyed =
        fixture.Manager.Destroy(fixture.Space, created.Created, ObjectType::WaitSet);
    const auto reclaimed = fixture.Manager.GetWaitSetSnapshot();
    ASTERIA_EXPECT(runner, "the manager creates a WaitSet and reclaims it before its destruction returns", ready,
                   Is(created, ObjectManagementError::None), live, active.ActiveSlots == 1U,
                   Is(destroyed, ObjectManagementError::None), reclaimed.Reclaimed == 1U,
                   reclaimed.FreeSlots == Kernel::Ipc::WaitSetPool::Capacity,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}

[[nodiscard]] Kernel::Ipc::MessageEnvelope MakeMessage() noexcept
{
    Kernel::Ipc::MessageEnvelope message{};
    message.Interface = Kernel::Ipc::InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = 1U;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 7U;
    message.InterfaceVersionMajor = 1U;
    message.Kind = Kernel::Ipc::MessageKind::Asynchronous;
    message.Cancellation = Kernel::Ipc::CancellationState::None;
    return message;
}

// Service IPC: WaitSetBind registers a pooled Endpoint's readiness under a tag in any WaitSet the
// Caller manages, a boot one too.
void TestBindWaitSet(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    const ObjectManagementResult waitSet = fixture.Create(fixture.Authority, ObjectType::WaitSet);
    const ObjectManagementResult endpoint = fixture.Create(fixture.Authority);
    const ObjectManagementResult zeroTag =
        fixture.Manager.BindWaitSet(fixture.Space, waitSet.Created, endpoint.Created, 0U);
    const ObjectManagementResult bound =
        fixture.Manager.BindWaitSet(fixture.Space, waitSet.Created, endpoint.Created, 7U);
    const ObjectManagementResult again =
        fixture.Manager.BindWaitSet(fixture.Space, waitSet.Created, endpoint.Created, 8U);
    bool sent{false};
    UInt64 tag{0U};
    {
        const auto invoke = fixture.Space.Lookup(endpoint.Created, ObjectType::Endpoint, CapabilityOperation::Invoke);
        sent = invoke.HasValue() && static_cast<Endpoint&>(invoke.GetValue().GetObject())
                                        .SendFromInvocation(fixture.Space, endpoint.Created, ResourceDomainId{5U},
                                                            Kernel::Ipc::EndpointSendClass::Normal, MakeMessage())
                                        .HasValue();
        const auto set = fixture.Space.Lookup(waitSet.Created, ObjectType::WaitSet, CapabilityOperation::Wait);
        if (set.HasValue())
        {
            auto& armed = static_cast<Kernel::Ipc::WaitSet&>(set.GetValue().GetObject());
            const auto claimed = armed.ClaimReady();
            tag = claimed.HasValue() ? claimed.GetValue().Tag : 0U;
            if (claimed.HasValue())
            {
                static_cast<void>(armed.CommitClaim(claimed.GetValue().Claim));
            }
        }
    }
    const auto waitOnly =
        fixture.Space.Duplicate(waitSet.Created, CapabilityRights{CapabilityRight::Wait}, MonotonicTime::Infinite());
    const auto invokeOnly =
        fixture.Space.Duplicate(endpoint.Created, CapabilityRights{CapabilityRight::Invoke}, MonotonicTime::Infinite());
    const ObjectManagementResult second = fixture.Create(fixture.Authority);
    const ObjectManagementResult unmanaged = fixture.Manager.BindWaitSet(
        fixture.Space, waitOnly.HasValue() ? waitOnly.GetValue() : CapabilityHandle{}, second.Created, 9U);
    const ObjectManagementResult unreadable = fixture.Manager.BindWaitSet(
        fixture.Space, waitSet.Created, invokeOnly.HasValue() ? invokeOnly.GetValue() : CapabilityHandle{}, 9U);
    ASTERIA_EXPECT(runner, "WaitSetBind registers an Endpoint's readiness once, under its tag, with Manage and Read",
                   ready, Is(waitSet, ObjectManagementError::None), Is(endpoint, ObjectManagementError::None),
                   Is(zeroTag, ObjectManagementError::InvalidRequest), Is(bound, ObjectManagementError::None),
                   Is(again, ObjectManagementError::InvalidRequest), sent, tag == 7U,
                   Is(unmanaged, ObjectManagementError::CapabilityRejected),
                   Is(unreadable, ObjectManagementError::CapabilityRejected), fixture.Manager.GetBinds() == 1U);

    // The boot pager's WaitSet is no pool slot: it binds, and ObjectDestroy
    // refuses it.
    Kernel::Ipc::WaitSet boot{fixture.Registry};
    const bool bootReady = boot.Initialize(ResourceDomainId{5U}) == Kernel::Ipc::WaitSetError::None;
    const auto registered = fixture.Issuer.CreateObject(boot);
    const CapabilityHandle bootHandle =
        registered.HasValue()
            ? fixture.Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Wait))
            : CapabilityHandle{};
    const ObjectManagementResult bootBound = fixture.Manager.BindWaitSet(fixture.Space, bootHandle, second.Created, 9U);
    const ObjectManagementResult foreign = fixture.Manager.Destroy(fixture.Space, bootHandle, ObjectType::WaitSet);
    const ObjectManagementResult endpointGone =
        fixture.Manager.Destroy(fixture.Space, second.Created, ObjectType::Endpoint);
    const auto held = fixture.Manager.GetEndpointSnapshot();
    bool bootCollected{false};
    if (registered.HasValue())
    {
        bootCollected =
            (fixture.Issuer.DestroyObject(registered.GetValue()) == Kernel::Object::ObjectRegistryError::Ok) &&
            fixture.Issuer.CollectDestroyedObject(registered.GetValue()).HasValue();
    }
    ASTERIA_EXPECT(runner, "a boot WaitSet binds an Endpoint, holds it once destroyed, and is not ObjectDestroy's",
                   bootReady, registered.HasValue(), Is(bootBound, ObjectManagementError::None),
                   Is(foreign, ObjectManagementError::NotOwned), Is(endpointGone, ObjectManagementError::None),
                   held.QuiescingSlots == 1U, bootCollected, fixture.Manager.GetBinds() == 2U);
}

// ObjectCreate type 6, once the owner gave the pool its ledger, makes a child of the authorizing
// domain.
void TestResourceDomains(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    const auto request = [&]() noexcept
    {
        return fixture.Manager.Create(fixture.Space,
                                      ObjectCreateRequest{
                                          .Authority = fixture.Authority,
                                          .Type = ObjectType::ResourceDomain,
                                          .Reserved = {},
                                          .Endpoint = EndpointConfiguration::Default(),
                                          .Pages = PageCount{0U},
                                          .MaximumPermissions = {},
                                          .ReservedMemory = {},
                                          .Thread = {},
                                          .DomainAddressSpace = {},
                                          .DomainCapabilities = {},
                                          .SupervisionTag = 0U,
                                          .EndpointReceiver = {},
                                          .ThreadDomain = {},
                                          .EndpointSealedSelector = 0U,
                                          .DomainLimits = ResourceDomainLimits{.CommittedMemoryPages = 4U,
                                                                               .ResidentMemoryPages = 4U,
                                                                               .KernelMetadataBytes = 0U,
                                                                               .CapabilityHandles = 0U,
                                                                               .IpcQueueBytes = 4096U,
                                                                               .Threads = 1U,
                                                                               .ChildDomains = 0U},
                                          .FrameRegion = {},
                                          .FrameRegionOffsetPages = 0U,
                                          .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                          .IommuInputAddressBits = 0U,
                                          .ReservedCache = {},
                                          .InterruptFunction = {},
                                          .InterruptSource = 0U,
                                          .Caller = {},
                                      });
    };
    const ObjectManagementResult unconfigured = request();
    const bool configured = fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U});
    const ObjectManagementResult created = request();
    const bool live =
        fixture.Space.Lookup(created.Created, ObjectType::ResourceDomain, CapabilityOperation::Manage).HasValue();
    ASTERIA_EXPECT(runner, "the manager creates a child domain once configured, charged to its parent", ready,
                   !Is(unconfigured, ObjectManagementError::None), configured, Is(created, ObjectManagementError::None),
                   live, fixture.Manager.GetResourceDomainSnapshot().ActiveSlots == 1U,
                   fixture.Manager.GetResourceDomainSnapshot().NextIdentity == 7U,
                   fixture.Domain.GetSnapshot().Usage.ChildDomains == 1U);
}

// Destroying a child drains it and its descendants. Everything charged to them is destroyed,
// whatever space holds the capability (a Frame moved to a peer included), every capability goes
// stale, and the parent's usage returns to what it was. A drain scrubs once its Frames are gone,
// run after run until nothing is dirty (at most a bound), not one run after each of its phases;
// every collection and scrub is counted.
void TestDrainScrubsItsFrames(TestContext& runner) noexcept
{
    alignas(2U * 1024U * 1024U) static unsigned char pool[6U * 4096U];
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    TestScrubber scrubber;
    constexpr UInt64 base{0x0020'0000U};
    scrubber.Storage = pool;
    scrubber.PhysicalBase = base;
    const bool ready = fixture.Initialize() &&
                       fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U}) &&
                       (fixture.Manager.InitializeFramePool(PhysicalAddress{base}, 6U) == FramePoolError::None) &&
                       fixture.Manager.ConfigureScrubber(scrubber);
    const ObjectManagementResult child =
        CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    // The child's three pages lie apart, between pages of the fixture's own
    // Frames: three dirty runs once drained.
    bool created = Is(child, ObjectManagementError::None);
    for (Size page{0U}; page < 5U; ++page)
    {
        const ObjectManagementResult frame = ((page % 2U) == 0U)
                                                 ? CreateUnder(fixture, child.Created, ObjectType::Frame, 1U)
                                                 : fixture.CreateMemory(ObjectType::Frame, 1U);
        created = created && Is(frame, ObjectManagementError::None);
    }
    for (unsigned char& byte : pool)
    {
        byte = 0xA5U;
    }
    const auto before = fixture.Manager.GetCollectionSnapshot();
    const ObjectManagementResult drained =
        fixture.Manager.Destroy(fixture.Space, child.Created, ObjectType::ResourceDomain);
    const auto after = fixture.Manager.GetCollectionSnapshot();
    const auto frames = fixture.Manager.GetFramePoolSnapshot();
    ASTERIA_EXPECT(runner, "a drain scrubs the runs its Frames dirtied before it returns, and counts them", ready,
                   created, Is(drained, ObjectManagementError::None), scrubber.Calls == 3U, pool[0U] == 0U,
                   pool[4096U] == 0xA5U, pool[2U * 4096U] == 0U, pool[4U * 4096U] == 0U, frames.DirtyPages == 0U,
                   frames.FreePages == 4U, frames.AllocatedPages == 2U, after.ScrubRuns == (before.ScrubRuns + 3U),
                   after.ScrubbedPages == (before.ScrubbedPages + 3U), after.Collections > before.Collections,
                   after.Reclaimed >= (before.Reclaimed + 4U));
}

void TestDrainNestedDomains(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize() &&
                       fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U}) &&
                       (fixture.Manager.InitializeFramePool(PhysicalAddress{0x0020'0000U}, 4U) == FramePoolError::None);
    // An object of the fixture's own domain survives the drain.
    const ObjectManagementResult survivor = fixture.Create(fixture.Authority);
    const ResourceDomainUsage baseline = fixture.GetUsage();
    const ObjectManagementResult outer =
        CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(1U));
    const ObjectManagementResult inner =
        CreateUnder(fixture, outer.Created, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const ObjectManagementResult endpoint = CreateUnder(fixture, inner.Created, ObjectType::Endpoint);
    const ObjectManagementResult frame = CreateUnder(fixture, inner.Created, ObjectType::Frame, 2U);
    const ObjectManagementResult waitSet = CreateUnder(fixture, outer.Created, ObjectType::WaitSet);
    const ObjectManagementResult memory = CreateUnder(fixture, outer.Created, ObjectType::MemoryObject, 2U);
    // A peer holds the Frame: the charge stays with its creator.
    static CapabilitySpace peer{fixture.Registry, fixture.Revocations, fixture.Clock, 0x0F1E'2D3C'4B5A'6978ULL,
                                0x8796'A5B4'C3D2'E1F0ULL};
    const Kernel::Object::CapabilityTransferRequest move{
        .Source = frame.Created,
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = Kernel::Memory::FrameObjectPool::CreatedRights,
        .ExpectedType = ObjectType::Frame,
        .Disposition = Kernel::Object::CapabilityTransferDisposition::Move,
        .Reserved = 0U,
    };
    const auto moved =
        fixture.Space.TransferBatchTo(peer, Kernel::Object::CapabilityTransferRequestView{.Data = &move, .Count = 1U});
    const CapabilityHandle peerFrame = moved.HasValue() ? moved.GetValue().DestinationHandles[0U] : CapabilityHandle{};
    const ResourceDomainUsage charged = fixture.GetUsage();
    const ObjectManagementResult refused =
        fixture.Manager.Destroy(fixture.Space, survivor.Created, ObjectType::ResourceDomain);
    const ObjectManagementResult notAChild =
        fixture.Manager.Destroy(fixture.Space, fixture.Authority, ObjectType::ResourceDomain);
    ASTERIA_EXPECT(runner, "objects are created under a child and a grandchild and charged up the tree", ready,
                   Is(outer, ObjectManagementError::None), Is(inner, ObjectManagementError::None),
                   Is(endpoint, ObjectManagementError::None), Is(frame, ObjectManagementError::None),
                   Is(waitSet, ObjectManagementError::None), Is(memory, ObjectManagementError::None),
                   Is(survivor, ObjectManagementError::None), moved.HasValue(), peerFrame.IsValid(),
                   peer.Lookup(peerFrame, ObjectType::Frame, CapabilityOperation::Map).HasValue(),
                   charged.ResidentMemoryPages == 2U, charged.ChildDomains == 2U, charged.IpcQueueBytes > 0U,
                   Is(refused, ObjectManagementError::CapabilityRejected),
                   Is(notAChild, ObjectManagementError::NotOwned),
                   fixture.Manager.GetResourceDomainSnapshot().ActiveSlots == 2U);

    const ObjectManagementResult drained =
        fixture.Manager.Destroy(fixture.Space, outer.Created, ObjectType::ResourceDomain);
    const ResourceDomainUsage left = fixture.GetUsage();
    const auto domains = fixture.Manager.GetResourceDomainSnapshot();
    ASTERIA_EXPECT(
        runner, "one destruction drains both and returns the parent to its survivor's charge",
        Is(drained, ObjectManagementError::None), IsStale(fixture, outer.Created, ObjectType::ResourceDomain),
        IsStale(fixture, inner.Created, ObjectType::ResourceDomain),
        IsStale(fixture, endpoint.Created, ObjectType::Endpoint),
        !peer.Lookup(peerFrame, ObjectType::Frame, CapabilityOperation::Map).HasValue() &&
            (peer.Lookup(peerFrame, ObjectType::Frame, CapabilityOperation::Map).GetError() ==
             CapabilityError::ObjectNotFound),
        IsStale(fixture, waitSet.Created, ObjectType::WaitSet),
        IsStale(fixture, memory.Created, ObjectType::MemoryObject),
        fixture.Space.Lookup(survivor.Created, ObjectType::Endpoint, CapabilityOperation::Inspect).HasValue(),
        IsSameUsage(left, baseline), domains.ActiveSlots == 0U, domains.QuiescingSlots == 0U, domains.Reclaimed == 2U,
        fixture.Manager.GetFramePoolSnapshot().AllocatedPages == 0U,
        fixture.Manager.GetEndpointSnapshot().ActiveSlots == 1U);

    const ObjectManagementResult again =
        fixture.Manager.Destroy(fixture.Space, outer.Created, ObjectType::ResourceDomain);
    const ObjectManagementResult survivorGone =
        fixture.Manager.Destroy(fixture.Space, survivor.Created, ObjectType::Endpoint);
    const ObjectManagementResult reused =
        CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    ASTERIA_EXPECT(runner, "the drained capability is refused and a reclaimed slot takes a fresh identity",
                   Is(again, ObjectManagementError::CapabilityRejected),
                   again.Capability == CapabilityError::ObjectNotFound, Is(survivorGone, ObjectManagementError::None),
                   Is(reused, ObjectManagementError::None),
                   fixture.Manager.GetResourceDomainSnapshot().NextIdentity == 9U);
}

class TestThreadBackend final : public Kernel::Scheduler::IUserThreadBackend
{
public:
    [[nodiscard]] UInt64 GetNativeSlotBytes() const noexcept override
    {
        return 0x800U;
    }

    [[nodiscard]] Kernel::Scheduler::UserThreadSchedule GetSchedule() const noexcept override
    {
        return Kernel::Scheduler::UserThreadSchedule{
            .Configuration =
                Kernel::Scheduler::SchedulingContextConfiguration{
                    .BudgetTicks = 20U,
                    .PeriodTicks = 100U,
                    .RelativeDeadlineTicks = 80U,
                    .Weight = 10U,
                    .Priority = 8U,
                    .Criticality = Kernel::Scheduler::SchedulingCriticality::Normal,
                    .CpuAffinityMask = 0x01U,
                    .Class = Kernel::Scheduler::SchedulingClass::DeadlineSporadic,
                    .Reserved = 0U},
            .Now = MonotonicTime{0U},
        };
    }

    [[nodiscard]] Kernel::Scheduler::UserThreadBackendError
    Reserve(Kernel::Scheduler::Thread&, Kernel::Scheduler::ThreadId, Kernel::Scheduler::SchedulingContext&,
            Kernel::Scheduler::SchedulingContextId, const Kernel::Scheduler::UserThreadStart&) noexcept override
    {
        return Kernel::Scheduler::UserThreadBackendError::None;
    }

    [[nodiscard]] Kernel::Scheduler::UserThreadBackendError
    Start(Kernel::Scheduler::Thread& thread, Kernel::Scheduler::ThreadId, Kernel::Scheduler::SchedulingContext&,
          Kernel::Scheduler::SchedulingContextId, const Kernel::Scheduler::UserThreadStart& start) noexcept override
    {
        Started = &thread;
        Entry = start.Entry;
        Domain = start.Domain;
        return thread.MakeReady() == Kernel::Scheduler::ThreadError::None
                   ? Kernel::Scheduler::UserThreadBackendError::None
                   : Kernel::Scheduler::UserThreadBackendError::Faulted;
    }

    [[nodiscard]] bool IsReleased(const Kernel::Scheduler::Thread& thread) const noexcept override
    {
        return Released && (&thread == Started);
    }

    [[nodiscard]] Kernel::Scheduler::UserThreadTerminateError Terminate(Kernel::Scheduler::Thread&,
                                                                        const UInt64) noexcept override
    {
        return Kernel::Scheduler::UserThreadTerminateError::Unsupported;
    }

    Kernel::Scheduler::Thread* Started{nullptr};
    UInt64 Entry{0U};
    const Kernel::Protection::ProtectionDomain* Domain{nullptr};
    bool Released{false};
};

// A Thread is created Ready through the backend, reclaimed by ObjectDestroy once Exited (its exit
// code returned), and freed once its native slot is released.
void TestThreads(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    TestThreadBackend backend;
    const bool ready = fixture.Initialize() && fixture.Manager.ConfigureThreads(backend);
    const ObjectManagementResult created = fixture.Manager.Create(
        fixture.Space,
        ObjectCreateRequest{
            .Authority = fixture.Authority,
            .Type = ObjectType::Thread,
            .Reserved = {},
            .Endpoint = {},
            .Pages = PageCount{0U},
            .MaximumPermissions = {},
            .ReservedMemory = {},
            .Thread =
                Kernel::Scheduler::UserThreadStart{
                    .Entry = 0x4000'2000U, .UserStackTop = 0x4100'0000U, .Argument = 0U, .Domain = nullptr},
            .DomainAddressSpace = {},
            .DomainCapabilities = {},
            .SupervisionTag = 0U,
            .EndpointReceiver = {},
            .ThreadDomain = {},
            .EndpointSealedSelector = 0U,
            .DomainLimits = {},
            .FrameRegion = {},
            .FrameRegionOffsetPages = 0U,
            .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
            .IommuInputAddressBits = 0U,
            .ReservedCache = {},
            .InterruptFunction = {},
            .InterruptSource = 0U,
            .Caller = {},
        });
    const Kernel::Scheduler::Thread* const first = backend.Started;
    // A domain handle that names no ProtectionDomain reaches no backend.
    const ObjectManagementResult notADomain = fixture.Manager.Create(
        fixture.Space,
        ObjectCreateRequest{
            .Authority = fixture.Authority,
            .Type = ObjectType::Thread,
            .Reserved = {},
            .Endpoint = {},
            .Pages = PageCount{0U},
            .MaximumPermissions = {},
            .ReservedMemory = {},
            .Thread =
                Kernel::Scheduler::UserThreadStart{
                    .Entry = 0x4000'2000U, .UserStackTop = 0x4100'0000U, .Argument = 0U, .Domain = nullptr},
            .DomainAddressSpace = {},
            .DomainCapabilities = {},
            .SupervisionTag = 0U,
            .EndpointReceiver = {},
            .ThreadDomain = fixture.Authority,
            .EndpointSealedSelector = 0U,
            .DomainLimits = {},
            .FrameRegion = {},
            .FrameRegionOffsetPages = 0U,
            .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
            .IommuInputAddressBits = 0U,
            .ReservedCache = {},
            .InterruptFunction = {},
            .InterruptSource = 0U,
            .Caller = {},
        });
    const ObjectManagementResult running = fixture.Manager.Destroy(fixture.Space, created.Created, ObjectType::Thread);
    const bool exited =
        (backend.Started != nullptr) &&
        (backend.Started->BeginRun(Kernel::Scheduler::CpuId{0U}) == Kernel::Scheduler::ThreadError::None) &&
        backend.Started->FinishRun(Kernel::Scheduler::CpuId{0U}, Kernel::Scheduler::ThreadRunDisposition::Exited, 0x77U)
            .HasValue();
    const ObjectManagementResult reclaimed =
        fixture.Manager.Destroy(fixture.Space, created.Created, ObjectType::Thread);
    const auto awaiting = fixture.Manager.GetThreadSnapshot();
    backend.Released = true;
    const bool collected = fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "a thread is created, refused destruction while it runs, and reclaimed after its exit",
                   ready, Is(created, ObjectManagementError::None), created.Created.IsValid(),
                   backend.Entry == 0x4000'2000U, backend.Domain == nullptr,
                   Is(notADomain, ObjectManagementError::CapabilityRejected),
                   notADomain.Capability == CapabilityError::TypeMismatch, backend.Started == first,
                   Is(running, ObjectManagementError::Busy), exited, Is(reclaimed, ObjectManagementError::None),
                   reclaimed.Value == 0x77U, awaiting.QuiescingSlots == 1U, collected,
                   fixture.Manager.GetThreadSnapshot().FreeSlots == Kernel::Scheduler::ThreadPool::Capacity,
                   fixture.GetUsage().Threads == 0U, fixture.GetUsage().KernelMetadataBytes == 0U);
}

// Resource accounting: a drain waits (WouldBlock) while a thread charged
// to the domain has not exited, and an exited thread whose native slot is still held keeps the
// domain until the slot is released.
void TestDrainWaitsForThreads(TestContext& runner) noexcept
{
    // The manager holds every pool: fixtures live in static storage.
    static ManagerFixture fixture;
    TestThreadBackend backend;
    const bool ready = fixture.Initialize() && fixture.Manager.ConfigureThreads(backend) &&
                       fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U});
    const ResourceDomainUsage baseline = fixture.GetUsage();
    const ObjectManagementResult child =
        CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const ObjectManagementResult thread = CreateUnder(fixture, child.Created, ObjectType::Thread);
    const ObjectManagementResult second = CreateUnder(fixture, child.Created, ObjectType::Thread);
    const ObjectManagementResult running =
        fixture.Manager.Destroy(fixture.Space, child.Created, ObjectType::ResourceDomain);
    const bool held =
        fixture.Space.Lookup(child.Created, ObjectType::ResourceDomain, CapabilityOperation::Manage).HasValue();
    const bool exited =
        (backend.Started != nullptr) &&
        (backend.Started->BeginRun(Kernel::Scheduler::CpuId{0U}) == Kernel::Scheduler::ThreadError::None) &&
        backend.Started->FinishRun(Kernel::Scheduler::CpuId{0U}, Kernel::Scheduler::ThreadRunDisposition::Exited, 0x77U)
            .HasValue();
    const ObjectManagementResult drained =
        fixture.Manager.Destroy(fixture.Space, child.Created, ObjectType::ResourceDomain);
    const auto pending = fixture.Manager.GetResourceDomainSnapshot();
    const auto threads = fixture.Manager.GetThreadSnapshot();
    backend.Released = true;
    const bool collected = fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "the drain waits for the domain's thread to exit and its slot to be released", ready,
                   Is(child, ObjectManagementError::None), Is(thread, ObjectManagementError::None),
                   Is(second, ObjectManagementError::ResourceExhausted), Is(running, ObjectManagementError::Busy), held,
                   exited, Is(drained, ObjectManagementError::None),
                   IsStale(fixture, thread.Created, ObjectType::Thread),
                   IsStale(fixture, child.Created, ObjectType::ResourceDomain), threads.QuiescingSlots == 1U,
                   pending.QuiescingSlots == 1U, collected, fixture.Manager.GetResourceDomainSnapshot().Reclaimed == 1U,
                   fixture.Manager.GetThreadSnapshot().FreeSlots == Kernel::Scheduler::ThreadPool::Capacity,
                   IsSameUsage(fixture.GetUsage(), baseline));
}
} // namespace
} // namespace Asteria::Test

namespace Asteria::Test
{
namespace
{
/// Claims for one owner and logs the mappings it is given.
class RecordingIommuBackend final : public Hal::Device::IIommuDomainBackend
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Device::IommuBackendOwnershipError
    Claim(const Kernel::Device::IommuDomain& owner, const Foundation::UInt8 inputAddressBits) noexcept override
    {
        if ((m_owner != nullptr) || (inputAddressBits > 39U))
        {
            return (m_owner != nullptr) ? Hal::Device::IommuBackendOwnershipError::AlreadyOwned
                                        : Hal::Device::IommuBackendOwnershipError::UnsupportedConfiguration;
        }
        m_owner = &owner;
        return Hal::Device::IommuBackendOwnershipError::None;
    }

    [[nodiscard]] Hal::Device::IommuBackendOwnershipError
    Release(const Kernel::Device::IommuDomain& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return Hal::Device::IommuBackendOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        ++Releases;
        return Hal::Device::IommuBackendOwnershipError::None;
    }

    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested, Kernel::Memory::PhysicalAddress) const noexcept override
    {
        return requested;
    }

    [[nodiscard]] Hal::Device::IommuBackendError
    Map(const Hal::Device::IommuBackendMapRequest& request) noexcept override
    {
        ++Maps;
        LastMap = request;
        return Hal::Device::IommuBackendError::None;
    }

    [[nodiscard]] Hal::Device::IommuBackendError Unmap(const Hal::Device::IommuBackendMapRequest&) noexcept override
    {
        ++Unmaps;
        return Hal::Device::IommuBackendError::None;
    }

    [[nodiscard]] Hal::Device::IommuBackendError Attach(Hal::Device::IIommuDevice&) noexcept override
    {
        return Hal::Device::IommuBackendError::UnsupportedConfiguration;
    }

    [[nodiscard]] Hal::Device::IommuBackendError Detach(Hal::Device::IIommuDevice&) noexcept override
    {
        return Hal::Device::IommuBackendError::InvalidRequest;
    }

    [[nodiscard]] Hal::Device::IommuBackendFaults GetFaults() noexcept override
    {
        return Hal::Device::IommuBackendFaults{
            .Count = 3U, .LastSource = 0x0010U, .LastReason = 5U, .Reserved = {}, .LastAddress = 0x200000U};
    }

    Size Maps{0U};
    Size Unmaps{0U};
    Size Releases{0U};
    Hal::Device::IommuBackendMapRequest LastMap{};

private:
    const Kernel::Device::IommuDomain* m_owner{nullptr};
};

[[nodiscard]] ObjectManagementResult ControlIommu(ManagerFixture& fixture, const CapabilityHandle domain,
                                                  const UInt64 operation, const CapabilityHandle frame = {},
                                                  const UInt64 address = 0U) noexcept
{
    return fixture.Manager.ControlIommu(fixture.Space, Kernel::Protection::ProtectionDomainId{},
                                        Kernel::System::IommuControlRequest{.Domain = domain,
                                                                            .Operation = operation,
                                                                            .Function = {},
                                                                            .Frame = frame,
                                                                            .FramePageOffset = 1U,
                                                                            .Pages = 1U,
                                                                            .DeviceAddress = address,
                                                                            .Permissions = Abi::IommuMapDeviceWrite,
                                                                            .Reserved = {}});
}

// ObjectCreate type 16 is not configured until the platform gives the pool its slots; then an
// IommuDomain maps the caller's Frames, reports its faults, and is quiesced at ObjectDestroy and at
// the drain of the domain charged for it.
void TestIommuDomains(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    static Kernel::Device::IommuDomainSlot slots[Kernel::Device::IommuDomainPool::Capacity]{};
    static RecordingIommuBackend backends[Kernel::Device::IommuDomainPool::Capacity];
    Hal::Device::IIommuDomainBackend* const pointers[Kernel::Device::IommuDomainPool::Capacity]{
        &backends[0U], &backends[1U], &backends[2U], &backends[3U]};
    const bool ready = fixture.Initialize() &&
                       fixture.Manager.InitializeFramePool(PhysicalAddress{0x0080'0000U}, 16U) ==
                           Kernel::Memory::FramePoolError::None &&
                       fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U});
    const ObjectManagementResult unconfigured =
        CreateUnder(fixture, fixture.Authority, ObjectType::IommuDomain, 0U, {}, 32U);
    const bool configured = fixture.Manager.ConfigureIommu(slots, pointers);
    const ObjectManagementResult narrow = CreateUnder(fixture, fixture.Authority, ObjectType::IommuDomain, 0U, {}, 31U);
    const ObjectManagementResult domain = CreateUnder(fixture, fixture.Authority, ObjectType::IommuDomain, 0U, {}, 32U);
    const ObjectManagementResult frame = CreateUnder(fixture, fixture.Authority, ObjectType::Frame, 2U);
    const ObjectManagementResult mapped =
        ControlIommu(fixture, domain.Created, Abi::IommuMap, frame.Created, 0x0020'0000U);
    const ObjectManagementResult beyond =
        ControlIommu(fixture, domain.Created, Abi::IommuMap, frame.Created, 0x1'0000'0000U);
    const ObjectManagementResult faults = ControlIommu(fixture, domain.Created, Abi::IommuFaults);
    const ObjectManagementResult stray = ControlIommu(fixture, domain.Created, Abi::IommuUnmap, {}, 0x0030'0000U);
    const ObjectManagementResult unmapped = ControlIommu(fixture, domain.Created, Abi::IommuUnmap, {}, 0x0020'0000U);
    const ObjectManagementResult destroyed =
        fixture.Manager.Destroy(fixture.Space, domain.Created, ObjectType::IommuDomain);
    ASTERIA_EXPECT(runner, "an IommuDomain maps its creator's Frame, reports faults and is quiesced at its destruction",
                   ready, Is(unconfigured, ObjectManagementError::NotConfigured), configured,
                   Is(narrow, ObjectManagementError::InvalidRequest), Is(domain, ObjectManagementError::None),
                   Is(frame, ObjectManagementError::None), Is(mapped, ObjectManagementError::None),
                   mapped.Value == 0x0020'0000U, backends[0U].Maps == 1U,
                   backends[0U].LastMap.DmaStart.GetValue() == 0x0020'0000U,
                   backends[0U].LastMap.PhysicalStart.GetValue() == 0x0080'1000U,
                   backends[0U].LastMap.Permissions.Contains(Kernel::Device::DmaPermission::Write),
                   Is(beyond, ObjectManagementError::InvalidRequest), Is(faults, ObjectManagementError::None),
                   faults.Value == (3U | (UInt64{0x0010U} << 32U) | (UInt64{5U} << 48U)), faults.Value1 == 0x200000U,
                   Is(stray, ObjectManagementError::InvalidRequest), Is(unmapped, ObjectManagementError::None),
                   backends[0U].Unmaps == 1U, Is(destroyed, ObjectManagementError::None), backends[0U].Releases == 1U,
                   IsStale(fixture, domain.Created, ObjectType::IommuDomain),
                   fixture.Manager.GetIommuDomainSnapshot().Reclaimed == 1U);

    // A domain charged to a child goes with the child's drain, its mapping
    // with it.
    const ObjectManagementResult child =
        CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const ObjectManagementResult childDomain =
        CreateUnder(fixture, child.Created, ObjectType::IommuDomain, 0U, {}, 32U);
    const ObjectManagementResult childMapped =
        ControlIommu(fixture, childDomain.Created, Abi::IommuMap, frame.Created, 0x0040'0000U);
    const Size releases = backends[0U].Releases + backends[1U].Releases;
    const ObjectManagementResult drained =
        fixture.Manager.Destroy(fixture.Space, child.Created, ObjectType::ResourceDomain);
    ASTERIA_EXPECT(
        runner, "the drain quiesces and destroys the child's IommuDomain", Is(child, ObjectManagementError::None),
        Is(childDomain, ObjectManagementError::None), Is(childMapped, ObjectManagementError::None),
        Is(drained, ObjectManagementError::None), (backends[0U].Releases + backends[1U].Releases) == (releases + 1U),
        IsStale(fixture, childDomain.Created, ObjectType::IommuDomain),
        fixture.Manager.GetIommuDomainSnapshot().ActiveSlots == 0U,
        fixture.Manager.GetIommuDomainSnapshot().Reclaimed == 2U,
        Is(fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame), ObjectManagementError::None));
}

// ObjectCreate type 14 is not configured until the platform gives the pool its message vectors, and
// InterruptControl and WaitSetBind refuse a capability that names no Interrupt.
void TestInterruptsRefused(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    const bool ready = fixture.Initialize();
    const ObjectManagementResult unconfigured = CreateUnder(fixture, fixture.Authority, ObjectType::Interrupt);
    const ObjectManagementResult waitSet = fixture.Create(fixture.Authority, ObjectType::WaitSet);
    const ObjectManagementResult notInterrupt =
        fixture.Manager.ControlInterrupt(fixture.Space, waitSet.Created, Abi::InterruptRearm);
    const ObjectManagementResult unknownOperation =
        fixture.Manager.ControlInterrupt(fixture.Space, waitSet.Created, 3U);
    const ObjectManagementResult notSource =
        fixture.Manager.BindWaitSet(fixture.Space, waitSet.Created, waitSet.Created, 7U);
    ASTERIA_EXPECT(runner, "Interrupts wait for their vectors and only an Interrupt takes InterruptControl", ready,
                   Is(unconfigured, ObjectManagementError::NotConfigured), Is(waitSet, ObjectManagementError::None),
                   Is(notInterrupt, ObjectManagementError::CapabilityRejected),
                   notInterrupt.Capability == CapabilityError::TypeMismatch,
                   Is(unknownOperation, ObjectManagementError::InvalidRequest),
                   Is(notSource, ObjectManagementError::CapabilityRejected),
                   notSource.Capability != CapabilityError::Ok, fixture.Manager.GetInterruptSnapshot().Created == 0U);
    Platform::Synchronization::SetHostedProcessor(Kernel::Scheduler::CpuId{1U});
    const auto foreign = fixture.Manager.ControlInterrupt(fixture.Space, waitSet.Created, Abi::InterruptRearm);
    const Abi::SystemCallRequest ports{};
    ASTERIA_EXPECT(runner, "device use on an AP fails before authority lookup or programming",
                   Is(foreign, ObjectManagementError::NotConfigured),
                   fixture.Manager.AccessPorts(fixture.Space, ports).Error ==
                       Abi::SystemCallError::InvalidConfiguration);
    Platform::Synchronization::SetHostedProcessor(Kernel::Scheduler::CpuId{});
}

// A registered object of one type, standing in for a driver's AddressSpace.
class StandInObject final : public Kernel::Object::IKernelObject
{
public:
    constexpr StandInObject(const ObjectType type, const ResourceDomainId owner) noexcept : m_type{type}, m_owner{owner}
    {
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
    ObjectType m_type;
    ResourceDomainId m_owner;
};

// A function's configuration space with a 64-bit MSI capability at 0x50,
// recording what the kernel writes.
class MessageConfiguration final : public Hal::Device::IPciConfigurationSpace,
                                   public Hal::Device::IPciConfigurationWriter
{
public:
    [[nodiscard]] Hal::Device::PciConfigurationReadError Read32(Kernel::Device::PciFunctionAddress,
                                                                const Foundation::UInt16 offset,
                                                                Foundation::UInt32& output) noexcept override
    {
        output = Space[(offset / 4U) % 64U];
        return Hal::Device::PciConfigurationReadError::None;
    }

    [[nodiscard]] Hal::Device::PciConfigurationWriteError Write32(Kernel::Device::PciFunctionAddress,
                                                                  const Foundation::UInt16 offset,
                                                                  const Foundation::UInt32 value) noexcept override
    {
        const Size index{(offset / 4U) % 64U};
        // The status half and the capability header are read-only.
        const Foundation::UInt32 fixed{((offset == 0x04U) || (offset == 0x50U)) ? 0xFFFFU : 0U};
        Space[index] = (offset == 0x04U) ? ((Space[index] & 0xFFFF'0000U) | (value & 0xFFFFU))
                                         : ((offset == 0x50U) ? ((Space[index] & fixed) | (value & ~fixed)) : value);
        return Hal::Device::PciConfigurationWriteError::None;
    }

    [[nodiscard]] bool MessagesEnabled() const noexcept
    {
        return ((Space[0x50U / 4U] >> 16U) & 1U) != 0U;
    }

    Foundation::UInt32 Space[64U]{0U, 0x0010'0006U, 0U,    0U, 0U, 0U, 0U, 0U, 0U, 0U,          0U,
                                  0U, 0U,           0x50U, 0U, 0U, 0U, 0U, 0U, 0U, 0x0080'0005U};
};

/// One vector, 0x40, raised by the message 0xFEE00000 with data 0x40.
class OneVector final : public Hal::Interrupt::IMessageVectors
{
public:
    [[nodiscard]] Hal::Interrupt::MessageTarget Bind(Kernel::Interrupt::Interrupt& interrupt) noexcept override
    {
        if (Bound != nullptr)
        {
            return {};
        }
        Bound = &interrupt;
        return Hal::Interrupt::MessageTarget{.Vector = 0x40U, .Address = 0xFEE0'0000U, .Data = 0x40U, .Reserved = {}};
    }

    [[nodiscard]] bool Unbind(const UInt64 vector, const Kernel::Interrupt::Interrupt& interrupt) noexcept override
    {
        if ((vector != 0x40U) || (Bound != &interrupt))
        {
            return false;
        }
        Bound = nullptr;
        ++Unbinds;
        return true;
    }

    Kernel::Interrupt::Interrupt* Bound{nullptr};
    Size Unbinds{0U};
};

/// The tag of a ready registration of the WaitSet set names, claimed and
/// committed; zero when none is ready.
[[nodiscard]] UInt64 ClaimTag(ManagerFixture& fixture, const CapabilityHandle set) noexcept
{
    const auto lease = fixture.Space.Lookup(set, ObjectType::WaitSet, CapabilityOperation::Wait);
    if (!lease.HasValue())
    {
        return 0U;
    }
    auto& waitSet = static_cast<Kernel::Ipc::WaitSet&>(lease.GetValue().GetObject());
    const auto claimed = waitSet.ClaimReady();
    if (!claimed.HasValue())
    {
        return 0U;
    }
    const UInt64 tag{claimed.GetValue().Tag};
    static_cast<void>(waitSet.CommitClaim(claimed.GetValue().Claim));
    return tag;
}

// Documentation/Architecture/IommuDomain.md, end to end through the manager: the assignee creates
// an Interrupt for its function, binds it to a WaitSet with only the rights it was created with,
// arms it, and a message readies the WaitSet; ObjectDestroy turns the messages off and Collect
// frees the vector once the WaitSet is gone.
void TestInterrupts(TestContext& runner) noexcept
{
    static OneVector vectors;
    static ManagerFixture fixture;
    static StandInObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{5U}};
    static Kernel::Protection::ProtectionDomain driver;
    static Kernel::Device::PciFunction function;
    static MessageConfiguration configuration;
    const bool ready = fixture.Initialize();
    const auto addressObject = fixture.Issuer.CreateObject(addressSpace);
    const Kernel::Resource::ResourceDomainObjectId resourceId{fixture.Registry, fixture.DomainObject};
    const bool driverReady =
        addressObject.HasValue() &&
        (driver
             .Initialize(fixture.Registry, resourceId, fixture.Domain,
                         Kernel::Memory::AddressSpaceId{fixture.Registry, addressObject.GetValue()}, fixture.Space)
             .Error == Kernel::Protection::ProtectionDomainError::None);
    const auto driverObject = fixture.Issuer.CreateObject(driver);
    const Kernel::Protection::ProtectionDomainId driverId =
        driverObject.HasValue() ? Kernel::Protection::ProtectionDomainId{fixture.Registry, driverObject.GetValue()}
                                : Kernel::Protection::ProtectionDomainId{};
    const bool bound = driverObject.HasValue() && (driver.BindIdentity(fixture.Registry, driverId).Error ==
                                                   Kernel::Protection::ProtectionDomainError::None);
    const bool functionReady = function
                                   .Initialize(fixture.Registry, resourceId, fixture.Domain,
                                               Kernel::Device::PciFunctionDescriptor{
                                                   .Address = Kernel::Device::PciFunctionAddress{0U, 0U, 2U, 0U},
                                                   .VendorId = 0x1234U,
                                                   .DeviceId = 0x11E8U,
                                                   .RevisionId = 0x10U,
                                                   .ProgrammingInterface = 0U,
                                                   .Subclass = 0xFFU,
                                                   .ClassCode = 0x00U,
                                                   .HeaderLayout = 0U,
                                                   .Multifunction = false})
                                   .Error == Kernel::Device::PciFunctionError::None;
    const auto functionObject = fixture.Issuer.CreateObject(function);
    const CapabilityHandle driverHandle =
        driverObject.HasValue() ? fixture.Mint(driverObject.GetValue(), CapabilityRights{CapabilityRight::Manage})
                                : CapabilityHandle{};
    const CapabilityHandle functionHandle =
        functionObject.HasValue()
            ? fixture.Mint(functionObject.GetValue(),
                           CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Invoke))
            : CapabilityHandle{};
    const bool configured = fixture.Manager.ConfigureDeviceControl(configuration, configuration) &&
                            fixture.Manager.ConfigureInterrupts(vectors);
    const ObjectManagementResult assigned =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceAssign, driverHandle);
    ObjectCreateRequest request{
        .Authority = fixture.Authority,
        .Type = ObjectType::Interrupt,
        .Reserved = {},
        .Endpoint = {},
        .Pages = PageCount{0U},
        .MaximumPermissions = {},
        .ReservedMemory = {},
        .Thread = {},
        .DomainAddressSpace = {},
        .DomainCapabilities = {},
        .SupervisionTag = 0U,
        .EndpointReceiver = {},
        .ThreadDomain = {},
        .EndpointSealedSelector = 0U,
        .DomainLimits = {},
        .FrameRegion = {},
        .FrameRegionOffsetPages = 0U,
        .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
        .IommuInputAddressBits = 0U,
        .ReservedCache = {},
        .InterruptFunction = functionHandle,
        .InterruptSource = 0U,
        .Caller = Kernel::Protection::ProtectionDomainId{},
    };
    const ObjectManagementResult stranger = fixture.Manager.Create(fixture.Space, request);
    request.Caller = driverId;
    const ObjectManagementResult interrupt = fixture.Manager.Create(fixture.Space, request);
    const ObjectManagementResult waitSet = fixture.Create(fixture.Authority, ObjectType::WaitSet);
    const ObjectManagementResult bind =
        fixture.Manager.BindWaitSet(fixture.Space, waitSet.Created, interrupt.Created, 5U);
    const ObjectManagementResult armed =
        fixture.Manager.ControlInterrupt(fixture.Space, interrupt.Created, Abi::InterruptRearm);
    const bool delivered = (vectors.Bound != nullptr) && vectors.Bound->HandleInterrupt().HasValue();
    const UInt64 tag = ClaimTag(fixture, waitSet.Created);
    const ObjectManagementResult acknowledged =
        fixture.Manager.ControlInterrupt(fixture.Space, interrupt.Created, Abi::InterruptKeepMasked);
    ASTERIA_EXPECT(runner, "the assignee's Interrupt readies a WaitSet it is bound to and is acknowledged", ready,
                   addressObject.HasValue(), driverReady, bound, functionReady, functionObject.HasValue(),
                   driverHandle.IsValid(), functionHandle.IsValid(), configured,
                   Is(assigned, ObjectManagementError::None), !Is(stranger, ObjectManagementError::None),
                   Is(interrupt, ObjectManagementError::None), configuration.MessagesEnabled(),
                   configuration.Space[0x5CU / 4U] == 0x40U, Is(waitSet, ObjectManagementError::None),
                   Is(bind, ObjectManagementError::None), Is(armed, ObjectManagementError::None), armed.Value == 0U,
                   delivered, tag == 5U, Is(acknowledged, ObjectManagementError::None), acknowledged.Value == 1U);

    // The vector stays bound while the WaitSet's registration holds the
    // Notification, and returns with the slot once the WaitSet goes.
    const ObjectManagementResult destroyed =
        fixture.Manager.Destroy(fixture.Space, interrupt.Created, ObjectType::Interrupt);
    const bool messagesOff = !configuration.MessagesEnabled();
    const bool held = fixture.Manager.Collect() && (vectors.Bound != nullptr);
    const ObjectManagementResult setDestroyed =
        fixture.Manager.Destroy(fixture.Space, waitSet.Created, ObjectType::WaitSet);
    const bool collected = fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "a destroyed Interrupt's vector returns once its WaitSet is gone",
                   Is(destroyed, ObjectManagementError::None), messagesOff, held,
                   Is(setDestroyed, ObjectManagementError::None), collected, vectors.Bound == nullptr,
                   // The stranger's refused creation bound the vector and gave it back.
                   vectors.Unbinds == 2U, fixture.Manager.GetInterruptSnapshot().Reclaimed == 1U,
                   IsStale(fixture, interrupt.Created, ObjectType::Interrupt),
                   Is(fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceRelease, {}),
                      ObjectManagementError::None));
}
// The assignee reaches its USB host controller's legacy-support dwords through ControlDevice, the
// dword read coming back as the result's value; a refused access is an invalid request.
void TestLegacySupportAccess(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    static StandInObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{5U}};
    static Kernel::Protection::ProtectionDomain driver;
    static Kernel::Device::PciFunction function;
    static MessageConfiguration configuration;
    configuration.Space[0x08U / 4U] = 0x0C03'2003U;
    const bool ready = fixture.Initialize();
    const auto addressObject = fixture.Issuer.CreateObject(addressSpace);
    const Kernel::Resource::ResourceDomainObjectId resourceId{fixture.Registry, fixture.DomainObject};
    const bool driverReady =
        addressObject.HasValue() &&
        (driver
             .Initialize(fixture.Registry, resourceId, fixture.Domain,
                         Kernel::Memory::AddressSpaceId{fixture.Registry, addressObject.GetValue()}, fixture.Space)
             .Error == Kernel::Protection::ProtectionDomainError::None);
    const auto driverObject = fixture.Issuer.CreateObject(driver);
    const Kernel::Protection::ProtectionDomainId driverId =
        driverObject.HasValue() ? Kernel::Protection::ProtectionDomainId{fixture.Registry, driverObject.GetValue()}
                                : Kernel::Protection::ProtectionDomainId{};
    const bool bound = driverObject.HasValue() && (driver.BindIdentity(fixture.Registry, driverId).Error ==
                                                   Kernel::Protection::ProtectionDomainError::None);
    const bool functionReady = function
                                   .Initialize(fixture.Registry, resourceId, fixture.Domain,
                                               Kernel::Device::PciFunctionDescriptor{
                                                   .Address = Kernel::Device::PciFunctionAddress{0U, 0U, 0x1DU, 7U},
                                                   .VendorId = 0x8086U,
                                                   .DeviceId = 0x293AU,
                                                   .RevisionId = 3U,
                                                   .ProgrammingInterface = 0x20U,
                                                   .Subclass = 3U,
                                                   .ClassCode = 0x0CU,
                                                   .HeaderLayout = 0U,
                                                   .Multifunction = true})
                                   .Error == Kernel::Device::PciFunctionError::None;
    const auto functionObject = fixture.Issuer.CreateObject(function);
    const CapabilityHandle driverHandle =
        driverObject.HasValue() ? fixture.Mint(driverObject.GetValue(), CapabilityRights{CapabilityRight::Manage})
                                : CapabilityHandle{};
    const CapabilityHandle functionHandle =
        functionObject.HasValue()
            ? fixture.Mint(functionObject.GetValue(),
                           CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Invoke))
            : CapabilityHandle{};
    const bool configured = fixture.Manager.ConfigureDeviceControl(configuration, configuration);
    const ObjectManagementResult assigned =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceAssign, driverHandle);
    const ObjectManagementResult read = fixture.Manager.ControlDevice(
        fixture.Space, driverId, functionHandle, Abi::DeviceReadConfiguration, CapabilityHandle{0x08U, 0U});
    const ObjectManagementResult refused = fixture.Manager.ControlDevice(
        fixture.Space, driverId, functionHandle, Abi::DeviceWriteLegacySupport, CapabilityHandle{0x04U, 0x0006U});
    ASTERIA_EXPECT(runner, "legacy-support access returns the dword read and refuses other dwords", ready, driverReady,
                   bound, functionReady, functionObject.HasValue(), configured,
                   Is(assigned, ObjectManagementError::None), Is(read, ObjectManagementError::None),
                   read.Value == 0x0C03'2003U, Is(refused, ObjectManagementError::InvalidRequest),
                   Is(fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceRelease, {}),
                      ObjectManagementError::None));
    // Released, the function is the Device Manager's to read (Manage), and
    // an Invoke-only holder reads nothing.
    const ObjectManagementResult managed = fixture.Manager.ControlDevice(
        fixture.Space, driverId, functionHandle, Abi::DeviceReadConfiguration, CapabilityHandle{0x08U, 0U});
    const ObjectManagementResult outside = fixture.Manager.ControlDevice(
        fixture.Space, driverId, functionHandle, Abi::DeviceReadConfiguration, CapabilityHandle{0x04U, 0U});
    const CapabilityHandle invokeOnly =
        functionObject.HasValue() ? fixture.Mint(functionObject.GetValue(), CapabilityRights{CapabilityRight::Invoke})
                                  : CapabilityHandle{};
    const ObjectManagementResult unheld = fixture.Manager.ControlDevice(
        fixture.Space, driverId, invokeOnly, Abi::DeviceReadConfiguration, CapabilityHandle{0x08U, 0U});
    ASTERIA_EXPECT(runner, "the manager reads a released controller's whitelisted dwords",
                   Is(managed, ObjectManagementError::None), managed.Value == 0x0C03'2003U,
                   Is(outside, ObjectManagementError::InvalidRequest),
                   Is(unheld, ObjectManagementError::InvalidRequest));
}
/// An IommuDomain creation request of 32 bits, under the fixture's authority, as caller.
[[nodiscard]] ObjectCreateRequest IommuDomainRequest(ManagerFixture& fixture,
                                                     const Kernel::Protection::ProtectionDomainId caller) noexcept
{
    return ObjectCreateRequest{.Authority = fixture.Authority,
                               .Type = ObjectType::IommuDomain,
                               .Reserved = {},
                               .Endpoint = {},
                               .Pages = PageCount{0U},
                               .MaximumPermissions = {},
                               .ReservedMemory = {},
                               .Thread = {},
                               .DomainAddressSpace = {},
                               .DomainCapabilities = {},
                               .SupervisionTag = 0U,
                               .EndpointReceiver = {},
                               .ThreadDomain = {},
                               .EndpointSealedSelector = 0U,
                               .DomainLimits = {},
                               .FrameRegion = {},
                               .FrameRegionOffsetPages = 0U,
                               .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                               .IommuInputAddressBits = 32U,
                               .ReservedCache = {},
                               .InterruptFunction = {},
                               .InterruptSource = 0U,
                               .Caller = caller};
}

// The trusted-driver profile end to end through the manager: on identity backends only the assignee
// of a function granted trusted DMA creates an IommuDomain; it attaches that function, IommuMap
// answers the Frame's physical address, which IommuUnmap names, and bus mastering follows the
// attachment. Every other caller sees what it sees without an IOMMU.
void TestTrustedIommuDomains(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    static StandInObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{5U}};
    static Kernel::Protection::ProtectionDomain driver;
    static Kernel::Device::PciFunction function;
    static MessageConfiguration configuration;
    static Kernel::Device::IommuDomainSlot slots[Kernel::Device::IommuDomainPool::Capacity]{};
    static Platform::Device::IdentityDomainBackend backends[Kernel::Device::IommuDomainPool::Capacity];
    Hal::Device::IIommuDomainBackend* const pointers[Kernel::Device::IommuDomainPool::Capacity]{
        &backends[0U], &backends[1U], &backends[2U], &backends[3U]};
    const bool ready = fixture.Initialize() &&
                       (fixture.Manager.InitializeFramePool(PhysicalAddress{0x0080'0000U}, 16U) ==
                        Kernel::Memory::FramePoolError::None) &&
                       fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U});
    const auto addressObject = fixture.Issuer.CreateObject(addressSpace);
    const Kernel::Resource::ResourceDomainObjectId resourceId{fixture.Registry, fixture.DomainObject};
    const bool driverReady =
        addressObject.HasValue() &&
        (driver
             .Initialize(fixture.Registry, resourceId, fixture.Domain,
                         Kernel::Memory::AddressSpaceId{fixture.Registry, addressObject.GetValue()}, fixture.Space)
             .Error == Kernel::Protection::ProtectionDomainError::None);
    const auto driverObject = fixture.Issuer.CreateObject(driver);
    const Kernel::Protection::ProtectionDomainId driverId =
        driverObject.HasValue() ? Kernel::Protection::ProtectionDomainId{fixture.Registry, driverObject.GetValue()}
                                : Kernel::Protection::ProtectionDomainId{};
    const bool bound = driverObject.HasValue() && (driver.BindIdentity(fixture.Registry, driverId).Error ==
                                                   Kernel::Protection::ProtectionDomainError::None);
    const bool functionReady = function
                                   .Initialize(fixture.Registry, resourceId, fixture.Domain,
                                               Kernel::Device::PciFunctionDescriptor{
                                                   .Address = Kernel::Device::PciFunctionAddress{0U, 0U, 0x1FU, 2U},
                                                   .VendorId = 0x8086U,
                                                   .DeviceId = 0x2922U,
                                                   .RevisionId = 2U,
                                                   .ProgrammingInterface = 1U,
                                                   .Subclass = 6U,
                                                   .ClassCode = 1U,
                                                   .HeaderLayout = 0U,
                                                   .Multifunction = true})
                                   .Error == Kernel::Device::PciFunctionError::None;
    const auto functionObject = fixture.Issuer.CreateObject(function);
    const CapabilityHandle driverHandle =
        driverObject.HasValue() ? fixture.Mint(driverObject.GetValue(), CapabilityRights{CapabilityRight::Manage})
                                : CapabilityHandle{};
    const CapabilityHandle functionHandle =
        functionObject.HasValue()
            ? fixture.Mint(functionObject.GetValue(),
                           CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Invoke))
            : CapabilityHandle{};
    const bool configured =
        fixture.Manager.ConfigureDeviceControl(configuration, configuration) &&
        fixture.Manager.ConfigureIommu(slots, pointers, Kernel::System::IommuAdmission::TrustedAssignees);
    const ObjectManagementResult assigned =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceAssign, driverHandle);
    const ObjectManagementResult untrusted =
        fixture.Manager.Create(fixture.Space, IommuDomainRequest(fixture, driverId));
    const ObjectManagementResult release =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceRelease, {});
    const bool granted = function.GrantTrustedDma() && function.HasTrustedDma();
    const ObjectManagementResult reassigned =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceAssign, driverHandle);
    const bool assignedGrant = function.GrantTrustedDma();
    const ObjectManagementResult stranger =
        fixture.Manager.Create(fixture.Space, IommuDomainRequest(fixture, Kernel::Protection::ProtectionDomainId{}));
    const ObjectManagementResult domain = fixture.Manager.Create(fixture.Space, IommuDomainRequest(fixture, driverId));
    const ObjectManagementResult masterless =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceEnableBusMaster, {});
    const ObjectManagementResult attached =
        fixture.Manager.ControlIommu(fixture.Space, driverId,
                                     Kernel::System::IommuControlRequest{.Domain = domain.Created,
                                                                         .Operation = Abi::IommuAttach,
                                                                         .Function = functionHandle,
                                                                         .Frame = {},
                                                                         .FramePageOffset = 0U,
                                                                         .Pages = 0U,
                                                                         .DeviceAddress = 0U,
                                                                         .Permissions = 0U,
                                                                         .Reserved = {}});
    const ObjectManagementResult frame = CreateUnder(fixture, fixture.Authority, ObjectType::Frame, 2U);
    const ObjectManagementResult mapped =
        ControlIommu(fixture, domain.Created, Abi::IommuMap, frame.Created, 0x0020'0000U);
    const auto commandBefore = configuration.Space[1U];
    constexpr UInt64 unknownOperations[]{0U, 8U, 12U, ~UInt64{0U}};
    for (const UInt64 operation : unknownOperations)
    {
        const auto rejected = fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, operation, {});
        ASTERIA_EXPECT(runner, "undefined device operations cannot enable bus mastering or change PCI command",
                       Is(rejected, ObjectManagementError::InvalidRequest),
                       configuration.Space[1U] == commandBefore, (configuration.Space[1U] & 4U) == 0U);
    }
    const ObjectManagementResult master =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceEnableBusMaster, {});
    const bool mastering = (configuration.Space[1U] & 4U) != 0U;
    const ObjectManagementResult asked = ControlIommu(fixture, domain.Created, Abi::IommuUnmap, {}, 0x0020'0000U);
    const ObjectManagementResult unmapped = ControlIommu(fixture, domain.Created, Abi::IommuUnmap, {}, mapped.Value);
    const ObjectManagementResult deferred =
        fixture.Manager.ControlIommu(fixture.Space, driverId,
                                     Kernel::System::IommuControlRequest{.Domain = domain.Created,
                                                                         .Operation = Abi::IommuDetach,
                                                                         .Function = functionHandle,
                                                                         .Frame = {},
                                                                         .FramePageOffset = 0U,
                                                                         .Pages = 0U,
                                                                         .DeviceAddress = 0U,
                                                                         .Permissions = 0U,
                                                                         .Reserved = {}});
    ASTERIA_EXPECT(runner, "DMA storage awaiting device quiet makes Detach Busy and preserves the association",
                   Is(deferred, ObjectManagementError::Busy), function.GetSnapshot().IommuClaimed,
                   (configuration.Space[1U] & 4U) == 0U);
    const ObjectManagementResult destroyed =
        fixture.Manager.Destroy(fixture.Space, domain.Created, ObjectType::IommuDomain);
    const bool stopped = (configuration.Space[1U] & 4U) == 0U;
    ASTERIA_EXPECT(runner, "on identity backends only a trusted function's assignee gets an IommuDomain", ready,
                   driverReady, bound, functionReady, functionObject.HasValue(), configured,
                   Is(assigned, ObjectManagementError::None), Is(untrusted, ObjectManagementError::NotConfigured),
                   Is(release, ObjectManagementError::None), granted, Is(reassigned, ObjectManagementError::None),
                   !assignedGrant, Is(stranger, ObjectManagementError::NotConfigured),
                   Is(domain, ObjectManagementError::None));
    ASTERIA_EXPECT(
        runner, "the trusted function attaches, maps at the physical address and masters only then",
        Is(masterless, ObjectManagementError::NotConfigured), Is(attached, ObjectManagementError::None),
        Is(frame, ObjectManagementError::None), Is(mapped, ObjectManagementError::None), mapped.Value == 0x0080'1000U,
        Is(master, ObjectManagementError::None), mastering, Is(asked, ObjectManagementError::InvalidRequest),
        Is(unmapped, ObjectManagementError::None), Is(destroyed, ObjectManagementError::None), stopped,
        Is(fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceRelease, {}),
           ObjectManagementError::None),
        Is(fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame), ObjectManagementError::None));
}

/// One line, GSI 16 on vector 0x41, whose source records what the
/// Interrupt does with it.
class OneLine final : public Hal::Interrupt::IInterruptLines, public Hal::Interrupt::IInterruptSource
{
public:
    [[nodiscard]] Hal::Interrupt::LineReservation Reserve(const Foundation::UInt32 line) noexcept override
    {
        if ((line != 16U) || Reserved)
        {
            return {};
        }
        Reserved = true;
        return Hal::Interrupt::LineReservation{.Source = this, .Vector = 0x41U};
    }

    [[nodiscard]] bool Publish(const UInt64 vector, Kernel::Interrupt::Interrupt& interrupt) noexcept override
    {
        Bound = (vector == 0x41U) ? &interrupt : nullptr;
        return Bound != nullptr;
    }

    void Release(const UInt64 vector) noexcept override
    {
        Reserved = Reserved && (vector != 0x41U);
        Bound = nullptr;
        ++Releases;
    }

    [[nodiscard]] Hal::Interrupt::InterruptSourceError Claim() noexcept override
    {
        ++Claims;
        return {};
    }
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Release() noexcept override
    {
        ++Unclaims;
        return {};
    }
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Unmask() noexcept override
    {
        ++Unmasks;
        return {};
    }
    [[nodiscard]] Hal::Interrupt::InterruptSourceError Mask() noexcept override
    {
        ++Masks;
        return {};
    }
    [[nodiscard]] Hal::Interrupt::InterruptSourceError MaskAndAcknowledge() noexcept override
    {
        ++Acknowledges;
        return {};
    }

    Kernel::Interrupt::Interrupt* Bound{nullptr};
    bool Reserved{false};
    Size Releases{0U};
    Size Claims{0U};
    Size Unclaims{0U};
    Size Unmasks{0U};
    Size Masks{0U};
    Size Acknowledges{0U};
};

// where the pool has lines, the assignee's
// Interrupt takes its function's INTx line (configuration byte 0x3C): the line's source is reserved
// before the Interrupt is built on it, published once it is, and the function's INTx is enabled;
// destroying the Interrupt disables it again, and the line returns once the WaitSet goes. A
// function without an interrupt pin has no Interrupt.
void TestInterruptLines(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    static StandInObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{5U}};
    static Kernel::Protection::ProtectionDomain driver;
    static Kernel::Device::PciFunction function;
    static MessageConfiguration configuration;
    static OneLine lines;
    // Line-only configuration retains the PCI INTx ABI.
    const bool ready = fixture.Initialize();
    const auto addressObject = fixture.Issuer.CreateObject(addressSpace);
    const Kernel::Resource::ResourceDomainObjectId resourceId{fixture.Registry, fixture.DomainObject};
    const bool driverReady =
        addressObject.HasValue() &&
        (driver
             .Initialize(fixture.Registry, resourceId, fixture.Domain,
                         Kernel::Memory::AddressSpaceId{fixture.Registry, addressObject.GetValue()}, fixture.Space)
             .Error == Kernel::Protection::ProtectionDomainError::None);
    const auto driverObject = fixture.Issuer.CreateObject(driver);
    const Kernel::Protection::ProtectionDomainId driverId =
        driverObject.HasValue() ? Kernel::Protection::ProtectionDomainId{fixture.Registry, driverObject.GetValue()}
                                : Kernel::Protection::ProtectionDomainId{};
    const bool bound = driverObject.HasValue() && (driver.BindIdentity(fixture.Registry, driverId).Error ==
                                                   Kernel::Protection::ProtectionDomainError::None);
    const bool functionReady = function
                                   .Initialize(fixture.Registry, resourceId, fixture.Domain,
                                               Kernel::Device::PciFunctionDescriptor{
                                                   .Address = Kernel::Device::PciFunctionAddress{0U, 0U, 0U, 0U},
                                                   .VendorId = 0x1234U,
                                                   .DeviceId = 0x11E8U,
                                                   .RevisionId = 0x10U,
                                                   .ProgrammingInterface = 0U,
                                                   .Subclass = 0xFFU,
                                                   .ClassCode = 0x00U,
                                                   .HeaderLayout = 0U,
                                                   .Multifunction = false})
                                   .Error == Kernel::Device::PciFunctionError::None;
    const auto functionObject = fixture.Issuer.CreateObject(function);
    const CapabilityHandle driverHandle =
        driverObject.HasValue() ? fixture.Mint(driverObject.GetValue(), CapabilityRights{CapabilityRight::Manage})
                                : CapabilityHandle{};
    const CapabilityHandle functionHandle =
        functionObject.HasValue()
            ? fixture.Mint(functionObject.GetValue(),
                           CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Invoke))
            : CapabilityHandle{};
    const bool configured = fixture.Manager.ConfigureDeviceControl(configuration, configuration) &&
                            fixture.Manager.ConfigureInterruptLines(lines);
    const ObjectManagementResult assigned =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceAssign, driverHandle);
    const bool disabledByAssign = (configuration.Space[1U] & 0x0400U) != 0U;
    ObjectCreateRequest request{
        .Authority = fixture.Authority,
        .Type = ObjectType::Interrupt,
        .Reserved = {},
        .Endpoint = {},
        .Pages = PageCount{0U},
        .MaximumPermissions = {},
        .ReservedMemory = {},
        .Thread = {},
        .DomainAddressSpace = {},
        .DomainCapabilities = {},
        .SupervisionTag = 0U,
        .EndpointReceiver = {},
        .ThreadDomain = {},
        .EndpointSealedSelector = 0U,
        .DomainLimits = {},
        .FrameRegion = {},
        .FrameRegionOffsetPages = 0U,
        .MemoryCachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
        .IommuInputAddressBits = 0U,
        .ReservedCache = {},
        .InterruptFunction = functionHandle,
        .InterruptSource = 0U,
        .Caller = driverId,
    };
    // No interrupt pin yet: nothing is created.
    const ObjectManagementResult pinless = fixture.Manager.Create(fixture.Space, request);
    configuration.Space[0x3CU / 4U] = 0x0000'0110U;
    const ObjectManagementResult interrupt = fixture.Manager.Create(fixture.Space, request);
    const bool lineEnabled = (configuration.Space[1U] & 0x0400U) == 0U;
    const ObjectManagementResult waitSet = fixture.Create(fixture.Authority, ObjectType::WaitSet);
    const ObjectManagementResult bind =
        fixture.Manager.BindWaitSet(fixture.Space, waitSet.Created, interrupt.Created, 6U);
    const ObjectManagementResult armed =
        fixture.Manager.ControlInterrupt(fixture.Space, interrupt.Created, Abi::InterruptRearm);
    const bool delivered = (lines.Bound != nullptr) && lines.Bound->HandleInterrupt().HasValue();
    const UInt64 tag = ClaimTag(fixture, waitSet.Created);
    ASTERIA_EXPECT(
        runner, "the assignee's Interrupt takes its function's INTx line and readies a WaitSet", ready, driverReady,
        bound, functionReady, configured, Is(assigned, ObjectManagementError::None), disabledByAssign,
        !Is(pinless, ObjectManagementError::None), Is(interrupt, ObjectManagementError::None), lines.Reserved,
        lines.Bound != nullptr, lines.Claims == 1U, lineEnabled, Is(bind, ObjectManagementError::None),
        Is(armed, ObjectManagementError::None), lines.Unmasks == 1U, delivered, lines.Acknowledges == 1U, tag == 6U);

    const ObjectManagementResult destroyed =
        fixture.Manager.Destroy(fixture.Space, interrupt.Created, ObjectType::Interrupt);
    const bool lineDisabled = (configuration.Space[1U] & 0x0400U) != 0U;
    const bool held = fixture.Manager.Collect() && lines.Reserved;
    const ObjectManagementResult setDestroyed =
        fixture.Manager.Destroy(fixture.Space, waitSet.Created, ObjectType::WaitSet);
    const bool collected = fixture.Manager.Collect();
    ASTERIA_EXPECT(runner, "a destroyed Interrupt disables the INTx and returns its line once its WaitSet is gone",
                   Is(destroyed, ObjectManagementError::None), lineDisabled, held, lines.Unclaims == 1U,
                   Is(setDestroyed, ObjectManagementError::None), collected, !lines.Reserved, lines.Bound == nullptr,
                   lines.Releases == 1U, fixture.Manager.GetInterruptSnapshot().Reclaimed == 1U,
                   Is(fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceRelease, {}),
                      ObjectManagementError::None));
}
class TestPorts final : public Hal::Device::IPortIo
{
public:
    [[nodiscard]] Foundation::UInt32 Read(const Foundation::UInt16 port,
                                          const Foundation::UInt8 width) noexcept override
    {
        LastPort = port;
        LastWidth = width;
        ++Reads;
        return 0x55U;
    }
    void Write(const Foundation::UInt16 port, const Foundation::UInt8 width,
               const Foundation::UInt32 value) noexcept override
    {
        LastPort = port;
        LastWidth = width;
        LastValue = value;
        ++Writes;
    }
    Foundation::UInt16 LastPort{0U};
    Foundation::UInt8 LastWidth{0U};
    Foundation::UInt32 LastValue{0U};
    Foundation::Size Reads{0U};
    Foundation::Size Writes{0U};
};

void TestPortsAbi(TestContext& context) noexcept
{
    static ManagerFixture fixture;
    static Kernel::Device::DeviceRegion region;
    static TestPorts ports;
    const bool ready = fixture.Initialize();
    static constexpr Foundation::UInt32 values[]{0xADU, 0xAEU};
    static constexpr Kernel::Device::DevicePortWriteList lists[]{
        {.Offset = 0U, .Width = Kernel::Device::DeviceAccessWidth::Byte, .Values = values, .Count = 2U}};
    const Kernel::Device::DeviceRegionDescriptor descriptor{
        .BaseAddress = 0x64U,
        .LengthBytes = 4U,
        .Kind = Kernel::Device::DeviceRegionKind::PortIo,
        .Permissions = Kernel::Device::DeviceRegionPermissions{Kernel::Device::DeviceRegionPermission::Read}.With(
            Kernel::Device::DeviceRegionPermission::Write),
        .Widths = Kernel::Device::DeviceAccessWidths{Kernel::Device::DeviceAccessWidth::Byte}
                      .With(Kernel::Device::DeviceAccessWidth::Word)
                      .With(Kernel::Device::DeviceAccessWidth::DoubleWord),
        .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
        .Reserved = {}};
    const bool initialized =
        region
            .Initialize(fixture.Registry,
                        Kernel::Resource::ResourceDomainObjectId{fixture.Registry, fixture.DomainObject},
                        fixture.Domain, descriptor, {.Lists = lists, .Count = 1U})
            .Error == Kernel::Device::DeviceRegionError::None;
    const auto registered = fixture.Issuer.CreateObject(region);
    const auto handle =
        registered.HasValue()
            ? fixture.Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Read}.With(CapabilityRight::Write))
            : CapabilityHandle{};
    const auto readOnly = registered.HasValue()
                              ? fixture.Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Read})
                              : CapabilityHandle{};
    fixture.Manager.ConfigurePorts(ports);
    Abi::SystemCallRequest request{.Number = static_cast<UInt64>(Abi::SystemCallNumber::PortIo),
                                   .Arguments = {handle.GetSelector(), handle.GetAuthenticator(), 0U, 1U, 0U, 0U}};
    ASTERIA_EXPECT(context, "port read checks authority and reaches exact hardware width", ready, initialized,
                   fixture.Manager.AccessPorts(fixture.Space, request).Value0 == 0x55U, ports.Reads == 1U,
                   ports.LastPort == 0x64U, ports.LastWidth == 1U);
    request.Arguments[4U] = 1U;
    for (UInt64 value{0U}; value < 256U; ++value)
    {
        request.Arguments[5U] = value;
        const auto response = fixture.Manager.AccessPorts(fixture.Space, request);
        ASTERIA_EXPECT(context, "publisher write list allows exactly its values",
                       response.Error == ((value == 0xADU || value == 0xAEU) ? Abi::SystemCallError::None
                                                                             : Abi::SystemCallError::WriteValueDenied));
    }
    ASTERIA_EXPECT(context, "refused reset never reaches hardware", ports.Writes == 2U);
    request.Arguments[3U] = 2U;
    request.Arguments[5U] = 0xADU;
    ASTERIA_EXPECT(context, "wider write cannot bypass the listed offset",
                   fixture.Manager.AccessPorts(fixture.Space, request).Error == Abi::SystemCallError::WriteValueDenied);
    request.Arguments[0U] = readOnly.GetSelector();
    request.Arguments[1U] = readOnly.GetAuthenticator();
    ASTERIA_EXPECT(context, "write right is required independently of region policy",
                   fixture.Manager.AccessPorts(fixture.Space, request).Error == Abi::SystemCallError::RightsDenied);
    request.Arguments[0U] = handle.GetSelector();
    request.Arguments[1U] = handle.GetAuthenticator();
    request.Arguments[4U] = 0U;
    request.Arguments[5U] = 0U;
    constexpr UInt64 validWidths[]{1U, 2U, 4U};
    for (const UInt64 width : validWidths)
    {
        request.Arguments[3U] = width;
        request.Arguments[2U] = 4U;
        ASTERIA_EXPECT(context, "offset plus width must fit the region",
                       fixture.Manager.AccessPorts(fixture.Space, request).Error ==
                           Abi::SystemCallError::InvalidArguments);
        request.Arguments[2U] = ~UInt64{0U};
        ASTERIA_EXPECT(context, "overflowing offset is rejected before port conversion",
                       fixture.Manager.AccessPorts(fixture.Space, request).Error ==
                           Abi::SystemCallError::InvalidArguments);
    }
    request.Arguments[2U] = 0U;
    constexpr UInt64 invalidWidths[]{0U, 3U, 8U, 256U};
    for (const UInt64 width : invalidWidths)
    {
        request.Arguments[3U] = width;
        ASTERIA_EXPECT(context, "unknown width is rejected before enum conversion",
                       fixture.Manager.AccessPorts(fixture.Space, request).Error ==
                           Abi::SystemCallError::InvalidArguments);
    }
    request.Arguments[3U] = 1U;
    request.Arguments[4U] = 2U;
    ASTERIA_EXPECT(context, "unknown operation is rejected",
                   fixture.Manager.AccessPorts(fixture.Space, request).Error == Abi::SystemCallError::InvalidArguments);
    request.Arguments[4U] = 1U;
    request.Arguments[5U] = 256U;
    ASTERIA_EXPECT(context, "byte values are not silently truncated",
                   fixture.Manager.AccessPorts(fixture.Space, request).Error == Abi::SystemCallError::InvalidArguments,
                   ports.Writes == 2U, region.GetSnapshot().ActiveRegisterAccesses == 0U);
    request.Arguments[4U] = 0U;
    request.Arguments[5U] = 0U;
    fixture.Clock.Set(~UInt64{0U});
    ASTERIA_EXPECT(context, "capability lookup preserves clock configuration errors",
                   fixture.Manager.AccessPorts(fixture.Space, request).Error ==
                       Abi::SystemCallError::InvalidConfiguration,
                   ports.Reads == 1U, ports.Writes == 2U);
    static_cast<void>(region.Quiesce());
}

void TestLegacyLine(TestContext& context) noexcept
{
    static ManagerFixture fixture;
    static OneLine lines;
    static OneVector vectors;
    static Kernel::Interrupt::InterruptLine line{16U, ResourceDomainId{5U}};
    const bool ready = fixture.Initialize() && fixture.Manager.ConfigureInterruptLines(lines) &&
                       fixture.Manager.ConfigureInterrupts(vectors);
    const auto registered = fixture.Issuer.CreateObject(line);
    const auto handle = registered.HasValue()
                            ? fixture.Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Invoke})
                            : CapabilityHandle{};
    ObjectCreateRequest request{};
    request.Authority = fixture.Authority;
    request.Type = ObjectType::Interrupt;
    request.InterruptFunction = handle;
    request.InterruptSource = Abi::InterruptSourceLine;
    const auto created = fixture.Manager.Create(fixture.Space, request);
    const auto duplicate = fixture.Manager.Create(fixture.Space, request);
    ASTERIA_EXPECT(context, "line authority creates without a PCI function or assignment", ready,
                   created.Error == ObjectManagementError::None, duplicate.Error != ObjectManagementError::None,
                   lines.Reserved, vectors.Bound == nullptr);
    ASTERIA_EXPECT(context, "arming and destroying legacy line masks and releases the source",
                   fixture.Manager.ControlInterrupt(fixture.Space, created.Created, Abi::InterruptRearm).Error ==
                       ObjectManagementError::None,
                   fixture.Manager.Destroy(fixture.Space, created.Created, ObjectType::Interrupt).Error ==
                       ObjectManagementError::None,
                   fixture.Manager.Collect(), lines.Unmasks == 1U, lines.Masks != 0U, lines.Unclaims == 1U,
                   lines.Releases == 1U, !lines.Reserved);
}

// Documentation/Architecture/IommuDomain.md, through
// the system-call layer: a firmware reservation of 00:1d.0 covers the page an identity domain maps
// the Frame's second page at. The function is not restricted (as one firmware released would not
// be), yet IommuAttach while that page is mapped and IommuMap of it while the function is attached
// answer ReservedMemory, and nothing changes.
class FunctionReservation final : public Kernel::Device::IDmaReservations
{
public:
    [[nodiscard]] bool Overlaps(const Foundation::UInt32 requester, const UInt64 first,
                                const UInt64 last) const noexcept override
    {
        return (requester == 0x00E8U) && (first <= 0x0080'1FFFU) && (0x0080'1000U <= last);
    }
};

void TestFirmwareDmaReservations(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    static StandInObject addressSpace{ObjectType::AddressSpace, ResourceDomainId{5U}};
    static Kernel::Protection::ProtectionDomain driver;
    static Kernel::Device::PciFunction function;
    static MessageConfiguration configuration;
    static const FunctionReservation reservation{};
    static Kernel::Device::IommuDomainSlot slots[Kernel::Device::IommuDomainPool::Capacity]{};
    static Platform::Device::IdentityDomainBackend backends[Kernel::Device::IommuDomainPool::Capacity];
    Hal::Device::IIommuDomainBackend* const pointers[Kernel::Device::IommuDomainPool::Capacity]{
        &backends[0U], &backends[1U], &backends[2U], &backends[3U]};
    const bool ready = fixture.Initialize() &&
                       (fixture.Manager.InitializeFramePool(PhysicalAddress{0x0080'0000U}, 16U) ==
                        Kernel::Memory::FramePoolError::None) &&
                       fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U});
    const auto addressObject = fixture.Issuer.CreateObject(addressSpace);
    const Kernel::Resource::ResourceDomainObjectId resourceId{fixture.Registry, fixture.DomainObject};
    const bool driverReady =
        addressObject.HasValue() &&
        (driver
             .Initialize(fixture.Registry, resourceId, fixture.Domain,
                         Kernel::Memory::AddressSpaceId{fixture.Registry, addressObject.GetValue()}, fixture.Space)
             .Error == Kernel::Protection::ProtectionDomainError::None);
    const auto driverObject = fixture.Issuer.CreateObject(driver);
    const Kernel::Protection::ProtectionDomainId driverId =
        driverObject.HasValue() ? Kernel::Protection::ProtectionDomainId{fixture.Registry, driverObject.GetValue()}
                                : Kernel::Protection::ProtectionDomainId{};
    const bool bound = driverObject.HasValue() && (driver.BindIdentity(fixture.Registry, driverId).Error ==
                                                   Kernel::Protection::ProtectionDomainError::None);
    const bool functionReady = function
                                   .Initialize(fixture.Registry, resourceId, fixture.Domain,
                                               Kernel::Device::PciFunctionDescriptor{
                                                   .Address = Kernel::Device::PciFunctionAddress{0U, 0U, 0x1DU, 0U},
                                                   .VendorId = 0x8086U,
                                                   .DeviceId = 0x1C26U,
                                                   .RevisionId = 5U,
                                                   .ProgrammingInterface = 0x20U,
                                                   .Subclass = 3U,
                                                   .ClassCode = 0x0CU,
                                                   .HeaderLayout = 0U,
                                                   .Multifunction = false})
                                   .Error == Kernel::Device::PciFunctionError::None;
    const bool granted = function.GrantTrustedDma();
    const auto functionObject = fixture.Issuer.CreateObject(function);
    const CapabilityHandle driverHandle =
        driverObject.HasValue() ? fixture.Mint(driverObject.GetValue(), CapabilityRights{CapabilityRight::Manage})
                                : CapabilityHandle{};
    const CapabilityHandle functionHandle =
        functionObject.HasValue()
            ? fixture.Mint(functionObject.GetValue(),
                           CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Invoke))
            : CapabilityHandle{};
    const bool configured =
        fixture.Manager.ConfigureDeviceControl(configuration, configuration) &&
        fixture.Manager.ConfigureIommu(slots, pointers, Kernel::System::IommuAdmission::TrustedAssignees, &reservation);
    const ObjectManagementResult assigned =
        fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceAssign, driverHandle);
    const ObjectManagementResult domain = fixture.Manager.Create(fixture.Space, IommuDomainRequest(fixture, driverId));
    const ObjectManagementResult frame = CreateUnder(fixture, fixture.Authority, ObjectType::Frame, 2U);
    const auto attach = [&]() noexcept
    {
        return fixture.Manager.ControlIommu(fixture.Space, driverId,
                                            Kernel::System::IommuControlRequest{.Domain = domain.Created,
                                                                                .Operation = Abi::IommuAttach,
                                                                                .Function = functionHandle,
                                                                                .Frame = {},
                                                                                .FramePageOffset = 0U,
                                                                                .Pages = 0U,
                                                                                .DeviceAddress = 0U,
                                                                                .Permissions = 0U,
                                                                                .Reserved = {}});
    };
    const ObjectManagementResult premapped =
        ControlIommu(fixture, domain.Created, Abi::IommuMap, frame.Created, 0x0020'0000U);
    const ObjectManagementResult refusedAttach = attach();
    const ObjectManagementResult unmapped = ControlIommu(fixture, domain.Created, Abi::IommuUnmap, {}, premapped.Value);
    const ObjectManagementResult attached = attach();
    const ObjectManagementResult refusedMap =
        ControlIommu(fixture, domain.Created, Abi::IommuMap, frame.Created, 0x0020'0000U);
    ASTERIA_EXPECT(
        runner, "attach and map refuse a firmware-reserved device range of the function", ready, driverReady, bound,
        functionReady, granted, configured, Is(assigned, ObjectManagementError::None),
        Is(domain, ObjectManagementError::None), Is(frame, ObjectManagementError::None),
        Is(premapped, ObjectManagementError::None), premapped.Value == 0x0080'1000U,
        Is(refusedAttach, ObjectManagementError::ReservedMemory), Is(unmapped, ObjectManagementError::None),
        Is(attached, ObjectManagementError::None), Is(refusedMap, ObjectManagementError::ReservedMemory),
        Is(fixture.Manager.Destroy(fixture.Space, domain.Created, ObjectType::IommuDomain),
           ObjectManagementError::None),
        Is(fixture.Manager.ControlDevice(fixture.Space, driverId, functionHandle, Abi::DeviceRelease, {}),
           ObjectManagementError::None),
        Is(fixture.Manager.Destroy(fixture.Space, frame.Created, ObjectType::Frame), ObjectManagementError::None));
}

class PowerModel final : public Hal::IPowerControl
{
public:
    UInt64 Calls{0U};
    [[nodiscard]] UInt64 Available() const noexcept override
    {
        return 3U;
    }
    [[nodiscard]] bool Transition(bool) noexcept override
    {
        ++Calls;
        return false;
    }
};
void TestPowerAuthority(TestContext& runner) noexcept
{
    static ManagerFixture fixture;
    static PowerModel power;
    const bool ready =
        fixture.Initialize() && fixture.Manager.ConfigureResourceDomains(fixture.Ledger, ResourceDomainId{6U});
    fixture.Manager.ConfigurePower(power, fixture.DomainObject);
    const auto call = [&](CapabilityHandle handle, UInt64 operation) noexcept
    {
        return fixture.Manager.ControlDevice(fixture.Space, {}, handle, operation, {});
    };
    const auto query = call(fixture.Authority, Abi::DevicePowerQuery);
    const auto inspect = call(fixture.InspectOnly, Abi::DevicePowerOff);
    const auto child = CreateUnder(fixture, fixture.Authority, ObjectType::ResourceDomain, 0U, ChildLimits(0U));
    const auto denied = call(child.Created, Abi::DeviceReboot);
    const auto beforeTransition = fixture.Manager.GetCollectionSnapshot();
    const auto unsupported = call(fixture.Authority, Abi::DevicePowerOff);
    const auto afterTransition = fixture.Manager.GetCollectionSnapshot();
    ASTERIA_EXPECT(runner, "power control performs no page collection or scrub after entering the adapter",
                   beforeTransition.Collections == afterTransition.Collections,
                   beforeTransition.ScrubRuns == afterTransition.ScrubRuns,
                   beforeTransition.ScrubbedPages == afterTransition.ScrubbedPages);
    ASTERIA_EXPECT(runner, "power requires Manage on exact boot grant; child Manage cannot reboot", ready,
                   Is(child, ObjectManagementError::None), Is(query, ObjectManagementError::None), query.Value == 3U,
                   Is(inspect, ObjectManagementError::CapabilityRejected), Is(denied, ObjectManagementError::NotOwned),
                   power.Calls == 1U, Is(unsupported, ObjectManagementError::NotConfigured));
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ObjectManagerTests"};
    Asteria::Test::TestCreateDestroyAndCollect(runner);
    Asteria::Test::TestErrors(runner);
    Asteria::Test::TestFramesAndMemoryObjects(runner);
    Asteria::Test::TestCodeAuthority(runner);
    Asteria::Test::TestSharedCodeSurvivesDrain(runner);
    Asteria::Test::TestUnmapReclaimsADestroyedFrame(runner);
    Asteria::Test::TestDeviceMapping(runner, Asteria::Kernel::Memory::MemoryCachePolicy::Uncached);
    Asteria::Test::TestDeviceMapping(runner, Asteria::Kernel::Memory::MemoryCachePolicy::WriteCombining);
    Asteria::Test::TestScrubbedFramesAreReused(runner);
    Asteria::Test::TestImportedCollectionBudget(runner);
    Asteria::Test::TestThreads(runner);
    Asteria::Test::TestWaitSets(runner);
    Asteria::Test::TestResourceDomains(runner);
    Asteria::Test::TestDrainNestedDomains(runner);
    Asteria::Test::TestDrainScrubsItsFrames(runner);
    Asteria::Test::TestDrainWaitsForThreads(runner);
    Asteria::Test::TestBindWaitSet(runner);
    Asteria::Test::TestIommuDomains(runner);
    Asteria::Test::TestInterruptsRefused(runner);
    Asteria::Test::TestInterrupts(runner);
    Asteria::Test::TestLegacySupportAccess(runner);
    Asteria::Test::TestTrustedIommuDomains(runner);
    Asteria::Test::TestInterruptLines(runner);
    Asteria::Test::TestPortsAbi(runner);
    Asteria::Test::TestLegacyLine(runner);
    Asteria::Test::TestPowerAuthority(runner);
    Asteria::Test::TestFirmwareDmaReservations(runner);
    return runner.Finish();
}
