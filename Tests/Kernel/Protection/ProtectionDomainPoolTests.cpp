// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/CapabilitySpacePool.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Protection/ProtectionDomainPool.hpp"
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
using Kernel::Memory::AddressSpace;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpaceId;
using Kernel::Memory::InitialMapping;
using Kernel::Memory::InitialMappingOwner;
using Kernel::Memory::InitialMappingPlan;
using Kernel::Memory::InitialMappingPlanError;
using Kernel::Memory::MappingPermission;
using Kernel::Memory::MappingPermissions;
using Kernel::Memory::PageCount;
using Kernel::Memory::PhysicalAddress;
using Kernel::Memory::VirtualAddress;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::CapabilitySpaceObject;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPoolError;
using Kernel::Object::ObjectPoolFailure;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RevocationDomainId;
using Kernel::Object::RevocationRegistry;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Protection::CreatedDomain;
using Kernel::Protection::ICreatedDomainRecords;
using Kernel::Protection::ProtectionDomain;
using Kernel::Protection::ProtectionDomainCreateRequest;
using Kernel::Protection::ProtectionDomainCreation;
using Kernel::Protection::ProtectionDomainPool;
using Kernel::Protection::ProtectionDomainState;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

using CreateResult = Foundation::Result<ProtectionDomainCreation, ObjectPoolFailure>;

constexpr ResourceDomainId Charged{5U};
constexpr UInt64 Tag{0x7A67'0001U};

class TestPageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError Claim(const AddressSpace&,
                                                                         const InitialMappingPlan&) noexcept override
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError Release(const AddressSpace&) noexcept override
    {
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableActivationError Activate(const AddressSpace&,
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
};

// The platform's record table, accepting or refusing as told.
class TestRecords final : public ICreatedDomainRecords
{
public:
    [[nodiscard]] bool RecordDomain(const CreatedDomain& domain) noexcept override
    {
        ++Recorded;
        Last = domain;
        return Accept;
    }

    [[nodiscard]] bool RetireDomain(const ProtectionDomain&) noexcept override
    {
        ++Retired;
        return Retire;
    }

    bool Accept{true};
    bool Retire{true};
    Size Recorded{0U};
    Size Retired{0U};
    CreatedDomain Last{};
};

[[nodiscard]] InitialMapping Seed(const UInt64 offset, const UInt64 pages, const MappingPermissions permissions,
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
}

class PoolFixture final
{
public:
    PoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL},
          Child{Registry, Revocations, Clock, 0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL},
          ChildObject{Child, Charged}, Domain{Ledger}, Pool{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const UInt64 metadataBytes = 1U << 20U, const bool configure = true) noexcept
    {
        const MappingPermissions readWrite = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
        const bool seeded =
            (Plan.AddMapping(Seed(0x1000U, 2U,
                                  MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
                                  InitialMappingOwner::NucleusCode)) == InitialMappingPlanError::None) &&
            (Plan.AddMapping(Seed(0x4000U, 4U, readWrite, InitialMappingOwner::NucleusStack)) ==
             InitialMappingPlanError::None) &&
            (Plan.AddMapping(Seed(0x8000U, 16U, readWrite, InitialMappingOwner::PageTable)) ==
             InitialMappingPlanError::None) &&
            (Plan.Seal() == InitialMappingPlanError::None);
        const ResourceDomainLimits limits{
            .CommittedMemoryPages = 0U,
            .ResidentMemoryPages = 0U,
            .KernelMetadataBytes = metadataBytes,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .ChildDomains = 0U,
        };
        if (!seeded || (Domain.InitializeRoot(Charged, limits).Error != ResourceDomainError::None) ||
            (Memory.Initialize(Charged, Table, Plan) != AddressSpaceError::None))
        {
            return false;
        }
        const auto domain = Issuer.CreateObject(Domain);
        const auto memory = Issuer.CreateObject(Memory);
        const auto child = Issuer.CreateObject(ChildObject);
        const auto root = Revocations.CreateRoot();
        if (!domain.HasValue() || !memory.HasValue() || !child.HasValue() || !root.HasValue() ||
            (Memory.BindIdentity(Registry, AddressSpaceId{Registry, memory.GetValue()}).Error !=
             AddressSpaceError::None))
        {
            return false;
        }
        MemoryObject = memory.GetValue();
        RevocationRoot = root.GetValue();
        const CapabilityRights manage = CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect);
        Authority = Mint(domain.GetValue(), manage);
        MemoryHandle = Mint(memory.GetValue(), manage);
        MemoryInspect = Mint(memory.GetValue(), CapabilityRights{CapabilityRight::Inspect});
        ChildHandle = Mint(child.GetValue(), manage);
        return Authority.IsValid() && MemoryHandle.IsValid() && MemoryInspect.IsValid() && ChildHandle.IsValid() &&
               (!configure || Pool.Configure(Records));
    }

    [[nodiscard]] CapabilityHandle Mint(const ObjectId object, const CapabilityRights rights) noexcept
    {
        const auto minted = Issuer.Mint(Space, object, rights, RevocationRoot, MonotonicTime::Infinite(), 0U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] CreateResult Create(const CapabilityHandle memory, const CapabilityHandle capabilities,
                                      const UInt64 tag = Tag) noexcept
    {
        return Pool.Create(ProtectionDomainCreateRequest{
            .Space = &Space,
            .Authority = Authority,
            .AddressSpace = memory,
            .Capabilities = capabilities,
            .SupervisionTag = tag,
        });
    }

    [[nodiscard]] CreateResult Create() noexcept
    {
        return Create(MemoryHandle, ChildHandle);
    }

    [[nodiscard]] UInt64 Usage() const noexcept
    {
        return Domain.GetSnapshot().Usage.KernelMetadataBytes;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    CapabilitySpace Child;
    CapabilitySpaceObject ChildObject;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    InitialMappingPlan Plan{};
    TestPageTable Table;
    AddressSpace Memory;
    TestRecords Records;
    ProtectionDomainPool Pool;
    ObjectId MemoryObject{};
    RevocationDomainId RevocationRoot{};
    CapabilityHandle Authority{};
    CapabilityHandle MemoryHandle{};
    CapabilityHandle MemoryInspect{};
    CapabilityHandle ChildHandle{};
};

[[nodiscard]] bool IsFailure(const CreateResult& result, const ObjectPoolError error) noexcept
{
    return !result.HasValue() && (result.GetError().Error == error);
}

void TestCreateBindsAndRecords(TestContext& runner) noexcept
{
    // Each AddressSpace is tens of KiB: fixtures live in static storage.
    static PoolFixture fixture;
    const bool ready = fixture.Initialize();
    const Size registered = fixture.Registry.GetActiveCount();
    const CreateResult first = fixture.Create();
    bool others{true};
    for (Size index{1U}; index < ProtectionDomainPool::Capacity; ++index)
    {
        others = others && fixture.Create(fixture.MemoryHandle, fixture.ChildHandle, Tag + index).HasValue();
    }
    const CreateResult duplicate = fixture.Create();
    const CreateResult beyond =
        fixture.Create(fixture.MemoryHandle, fixture.ChildHandle, Tag + ProtectionDomainPool::Capacity);
    const ProtectionDomain* const domain = fixture.Records.Last.Protection;
    const auto binding =
        (domain != nullptr)
            ? domain->GetBinding()
            : decltype(domain->GetBinding())::Failure(Kernel::Protection::ProtectionDomainError::NotInitialized);
    const auto queried = fixture.Space.Query(first.HasValue() ? first.GetValue().Capability : CapabilityHandle{});
    ASTERIA_EXPECT(
        runner, "a created domain binds both spaces, charges its size, is recorded with its tag and minted", ready,
        first.HasValue(), others, IsFailure(beyond, ObjectPoolError::CapacityExceeded),
        fixture.Records.Recorded == ProtectionDomainPool::Capacity,
        fixture.Records.Last.SupervisionTag == Tag + ProtectionDomainPool::Capacity - 1U,
        IsFailure(duplicate, ObjectPoolError::InvalidRequest), fixture.Pool.GetTagRefusals() == 1U,
        fixture.Domain.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::ProtectionDomain)] ==
            ProtectionDomainPool::Capacity,
        fixture.Records.Last.Capabilities == &fixture.Child, fixture.Records.Last.AddressSpace == &fixture.Memory,
        fixture.Records.Last.AddressSpaceIdentity.GetValue() == fixture.MemoryObject.GetValue(),
        (domain != nullptr) && (domain->GetSnapshot().State == ProtectionDomainState::Ready),
        binding.HasValue() && (binding.GetValue().Capabilities == &fixture.Child),
        binding.HasValue() && (binding.GetValue().AddressSpace.GetValue() == fixture.MemoryObject.GetValue()),
        queried.HasValue() && (queried.GetValue().Type == ObjectType::ProtectionDomain),
        queried.HasValue() && queried.GetValue().Rights.IsEqualTo(ProtectionDomainPool::CreatedRights),
        fixture.Usage() == ProtectionDomainPool::Capacity * sizeof(ProtectionDomain),
        fixture.Registry.GetActiveCount() == registered + ProtectionDomainPool::Capacity,
        fixture.Pool.GetSnapshot().ActiveSlots == ProtectionDomainPool::Capacity);
}

void TestRejections(TestContext& runner) noexcept
{
    static PoolFixture fixture;
    const bool ready = fixture.Initialize(1U << 20U, false);
    const CreateResult unconfigured = fixture.Create();
    const bool configured = fixture.Pool.Configure(fixture.Records);
    const CreateResult untagged = fixture.Create(fixture.MemoryHandle, fixture.ChildHandle, 0U);
    const CreateResult inspectOnly = fixture.Create(fixture.MemoryInspect, fixture.ChildHandle);
    const CreateResult swapped = fixture.Create(fixture.ChildHandle, fixture.MemoryHandle);
    fixture.Records.Accept = false;
    const Size registered = fixture.Registry.GetActiveCount();
    const CreateResult unrecorded = fixture.Create();
    ASTERIA_EXPECT(runner,
                   "an unconfigured pool, a zero tag, a space without Manage, a swapped space or a refused "
                   "record leaves nothing behind",
                   ready, IsFailure(unconfigured, ObjectPoolError::CapacityExceeded), configured,
                   IsFailure(untagged, ObjectPoolError::InvalidRequest),
                   IsFailure(inspectOnly, ObjectPoolError::CapabilityRejected),
                   !inspectOnly.HasValue() && (inspectOnly.GetError().Capability == CapabilityError::RightsDenied),
                   IsFailure(swapped, ObjectPoolError::CapabilityRejected),
                   !swapped.HasValue() && (swapped.GetError().Capability == CapabilityError::TypeMismatch),
                   IsFailure(unrecorded, ObjectPoolError::CapacityExceeded), fixture.Records.Recorded == 1U,
                   fixture.Registry.GetActiveCount() == registered, fixture.Usage() == 0U,
                   fixture.Domain.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::ProtectionDomain)] == 0U,
                   fixture.Pool.GetSnapshot().FreeSlots == ProtectionDomainPool::Capacity);
}

// A domain with a bound thread cannot be destroyed; once the platform
// retires its record, ObjectDestroy makes it quiescing and the collection
// quiesces it and returns its charge and slot.
void TestDestroyAfterRetirement(TestContext& runner) noexcept
{
    static PoolFixture fixture;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    ProtectionDomain* const domain = fixture.Records.Last.Protection;
    fixture.Records.Retire = false;
    const ObjectPoolFailure busy = fixture.Pool.Destroy(fixture.Space, handle);
    const auto stillActive = fixture.Pool.GetSnapshot();
    fixture.Records.Retire = true;
    Kernel::Object::ObjectPin retained;
    const auto pinned =
        fixture.Registry.PinObject(created.GetValue().Domain.GetObjectId(), ObjectType::ProtectionDomain, retained);
    const ObjectPoolFailure destroyed = fixture.Pool.Destroy(fixture.Space, handle);
    const auto deferred = fixture.Pool.Collect();
    const auto duplicate = fixture.Create();
    ASTERIA_EXPECT(runner, "a destroyed but pinned domain keeps its live tag until collection",
                   pinned == Kernel::Object::ObjectPinError::Ok, deferred.HasValue() && deferred.GetValue() == 0U,
                   IsFailure(duplicate, ObjectPoolError::InvalidRequest));
    retained.Reset();
    const auto quiescing = fixture.Pool.GetSnapshot();
    const auto stale = fixture.Space.Query(handle);
    const auto collected = fixture.Pool.Collect();
    const CreateResult again = fixture.Create();
    ASTERIA_EXPECT(
        runner, "a domain is destroyed only after its record retires, then collected and its slot reused", ready,
        created.HasValue(), domain != nullptr, busy.Error == ObjectPoolError::Busy, stillActive.ActiveSlots == 1U,
        destroyed.Error == ObjectPoolError::None, quiescing.QuiescingSlots == 1U, !stale.HasValue(),
        stale.GetError() == CapabilityError::ObjectNotFound, collected.HasValue() && (collected.GetValue() == 1U),
        again.HasValue(), fixture.Usage() == sizeof(ProtectionDomain), fixture.Pool.GetSnapshot().Reclaimed == 1U,
        fixture.Records.Retired == 2U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"ProtectionDomainPoolTests"};
    Asteria::Test::TestCreateBindsAndRecords(runner);
    Asteria::Test::TestRejections(runner);
    Asteria::Test::TestDestroyAfterRetirement(runner);
    return runner.Finish();
}
