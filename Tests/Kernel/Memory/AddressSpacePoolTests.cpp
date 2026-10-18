// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Memory/AddressSpace.hpp"
#include "Kernel/Memory/AddressSpacePool.hpp"
#include "Platform/Amd64/Memory/LargePageTableArenaPool.hpp"
#include "Kernel/Memory/InitialMappingPlan.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"

#include <new>

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
using Kernel::Memory::AddressSpaceCreateRequest;
using Kernel::Memory::AddressSpaceCreation;
using Kernel::Memory::AddressSpaceError;
using Kernel::Memory::AddressSpacePool;
using Kernel::Memory::AddressSpacePoolState;
using Kernel::Memory::IAddressSpaceProvider;
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
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
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
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

using CreateResult = Foundation::Result<AddressSpaceCreation, ObjectPoolFailure>;

constexpr ResourceDomainId Charged{5U};
constexpr UInt64 SlotBytes{0x1'0000U};

class TestPageTable final : public Hal::Memory::IAddressSpacePageTable
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError Claim(const AddressSpace& owner,
                                                                         const InitialMappingPlan&) noexcept override
    {
        Owner = &owner;
        return Hal::Memory::AddressSpacePageTableOwnershipError::None;
    }

    [[nodiscard]] Hal::Memory::AddressSpacePageTableOwnershipError Release(const AddressSpace&) noexcept override
    {
        Owner = nullptr;
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

    const AddressSpace* Owner{nullptr};
};

// Stands for a platform's prepared roots: a slot initializes from one sealed
// Seed while it holds no space, and ReleaseSlot rebuilds its objects; either refuses when told.
class TestProvider final : public IAddressSpaceProvider
{
public:
    explicit TestProvider(const Size slots) noexcept : Slots{slots}
    {
        const MappingPermissions readWrite = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Write);
        Sealed = (Seed.AddMapping(InitialMapping{
                      .VirtualStart = VirtualAddress{0xFFFF'8000'0000'1000ULL},
                      .PhysicalStart = PhysicalAddress{0x0010'1000U},
                      .Pages = PageCount{2U},
                      .Permissions = MappingPermissions{MappingPermission::Read}.With(MappingPermission::Execute),
                      .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                      .Owner = InitialMappingOwner::NucleusCode,
                      .Reserved = 0U,
                  }) == InitialMappingPlanError::None) &&
                 (Seed.AddMapping(InitialMapping{
                      .VirtualStart = VirtualAddress{0xFFFF'8000'0000'4000ULL},
                      .PhysicalStart = PhysicalAddress{0x0010'4000U},
                      .Pages = PageCount{4U},
                      .Permissions = readWrite,
                      .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                      .Owner = InitialMappingOwner::NucleusStack,
                      .Reserved = 0U,
                  }) == InitialMappingPlanError::None) &&
                 (Seed.AddMapping(InitialMapping{
                      .VirtualStart = VirtualAddress{0xFFFF'8000'0000'8000ULL},
                      .PhysicalStart = PhysicalAddress{0x0010'8000U},
                      .Pages = PageCount{16U},
                      .Permissions = readWrite,
                      .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                      .Owner = InitialMappingOwner::PageTable,
                      .Reserved = 0U,
                  }) == InitialMappingPlanError::None) &&
                 (Seed.Seal() == InitialMappingPlanError::None);
    }

    [[nodiscard]] Size GetSlotCount() const noexcept override
    {
        return Slots;
    }

    [[nodiscard]] UInt64 GetSlotBytes(const bool large = false) const noexcept override
    {
        return large ? LargeBytes : SlotBytes;
    }

    [[nodiscard]] AddressSpace* InitializeSlot(const Size slot, const ResourceDomainId chargeOwner,
                                               const bool large = false) noexcept override
    {
        ++Calls;
        LastCharge = chargeOwner;
        if (Refuse || (slot >= Slots) || Held[slot] ||
            (Spaces[slot].Initialize(chargeOwner, Tables[slot], Seed) != AddressSpaceError::None))
        {
            return nullptr;
        }
        Held[slot] = true;
        Large[slot] = large;
        if (large)
        {
            LargeIndex[slot] = LargeArenas.TryAcquire();
        }
        return &Spaces[slot];
    }

    [[nodiscard]] bool ReleaseSlot(const Size slot) noexcept override
    {
        ++Releases;
        if (RefuseRelease || (slot >= Slots) || !Held[slot] ||
            (Spaces[slot].GetSnapshot().State != Kernel::Memory::AddressSpaceState::Quiesced))
        {
            return false;
        }
        Spaces[slot].~AddressSpace();
        ::new (static_cast<void*>(&Spaces[slot])) AddressSpace{};
        Held[slot] = false;
        if (Large[slot])
        {
            if (!LargeArenas.Release(LargeIndex[slot]))
            {
                return false;
            }
            Large[slot] = false;
        }
        return true;
    }

    [[nodiscard]] bool HasAvailableArena(const bool large) const noexcept override
    {
        return !large || LargeArenas.HasAvailable();
    }
    static constexpr UInt64 LargeBytes{sizeof(AddressSpace) + 256U * 4096U};
    Platform::Amd64::Memory::LargePageTableArenaPool LargeArenas{};
    Size LargeIndex[AddressSpacePool::Capacity]{};
    bool Large[AddressSpacePool::Capacity]{};
    Size Slots;
    InitialMappingPlan Seed{};
    bool Sealed{false};
    TestPageTable Tables[AddressSpacePool::Capacity]{};
    AddressSpace Spaces[AddressSpacePool::Capacity]{};
    bool Held[AddressSpacePool::Capacity]{};
    bool Refuse{false};
    bool RefuseRelease{false};
    Size Calls{0U};
    Size Releases{0U};
    ResourceDomainId LastCharge{};
};

[[nodiscard]] constexpr ResourceDomainLimits Limits(const UInt64 metadataBytes = 1U << 20U) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = metadataBytes,
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

class PoolFixture final
{
public:
    explicit PoolFixture(const Size slots = AddressSpacePool::Capacity) noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Provider{slots}, Pool{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const ResourceDomainLimits& limits = Limits(), const bool configure = true) noexcept
    {
        if (!Provider.Sealed || (Domain.InitializeRoot(Charged, limits).Error != ResourceDomainError::None))
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        const auto root = Revocations.CreateRoot();
        if (!registered.HasValue() || !root.HasValue())
        {
            return false;
        }
        DomainObject = registered.GetValue();
        RevocationRoot = root.GetValue();
        const auto authority =
            Issuer.Mint(Space, DomainObject, CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect),
                        RevocationRoot, MonotonicTime::Infinite(), 0U);
        const auto inspect = Issuer.Mint(Space, DomainObject, CapabilityRights{CapabilityRight::Inspect},
                                         RevocationRoot, MonotonicTime::Infinite(), 0U);
        Authority = authority.HasValue() ? authority.GetValue() : CapabilityHandle{};
        InspectOnly = inspect.HasValue() ? inspect.GetValue() : CapabilityHandle{};
        return Authority.IsValid() && InspectOnly.IsValid() && (!configure || Pool.Configure(Provider));
    }

    [[nodiscard]] CreateResult Create(const CapabilityHandle authority) noexcept
    {
        return Pool.Create(AddressSpaceCreateRequest{.Space = &Space, .Authority = authority});
    }

    [[nodiscard]] CreateResult Create() noexcept
    {
        return Create(Authority);
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    TestProvider Provider;
    AddressSpacePool Pool;
    ObjectId DomainObject{};
    RevocationDomainId RevocationRoot{};
    CapabilityHandle Authority{};
    CapabilityHandle InspectOnly{};
};

[[nodiscard]] bool IsFailure(const CreateResult& result, const ObjectPoolError error) noexcept
{
    return !result.HasValue() && (result.GetError().Error == error);
}

void TestConfiguration(TestContext& runner) noexcept
{
    // Each AddressSpace is tens of KiB: fixtures live in static storage.
    static PoolFixture fixture;
    const bool ready = fixture.Initialize(Limits(), false);
    const CreateResult unconfigured = fixture.Create();
    static TestProvider none{0U};
    static TestProvider many{AddressSpacePool::Capacity + 1U};
    const bool empty = fixture.Pool.Configure(none);
    const bool tooMany = fixture.Pool.Configure(many);
    const bool configured = fixture.Pool.Configure(fixture.Provider);
    const bool again = fixture.Pool.Configure(fixture.Provider);
    ASTERIA_EXPECT(runner, "an unconfigured pool creates no space; a provider configures once, with 1 to 2 slots",
                   ready, IsFailure(unconfigured, ObjectPoolError::CapacityExceeded), fixture.Provider.Calls == 0U,
                   !empty, !tooMany, configured, !again,
                   fixture.Pool.GetSnapshot().State == AddressSpacePoolState::Ready,
                   fixture.Pool.GetSnapshot().FreeSlots == AddressSpacePool::Capacity);
}

void TestCreateChargesAndMints(TestContext& runner) noexcept
{
    // Each AddressSpace is tens of KiB: fixtures live in static storage. A
    // two-slot provider fills before the pool does.
    static PoolFixture fixture{2U};
    const bool ready = fixture.Initialize();
    const CreateResult first = fixture.Create();
    const CreateResult second = fixture.Create();
    const CreateResult third = fixture.Create();
    const CapabilityHandle handle = first.HasValue() ? first.GetValue().Capability : CapabilityHandle{};
    const auto queried = fixture.Space.Query(handle);
    const auto mapped = fixture.Space.Lookup(handle, ObjectType::AddressSpace, CapabilityOperation::Map);
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "each creation charges a slot, binds its space to its identity and mints Map and Manage", ready,
        first.HasValue(), second.HasValue(), IsFailure(third, ObjectPoolError::CapacityExceeded),
        fixture.Provider.Calls == 2U, fixture.Provider.LastCharge.GetValue() == Charged.GetValue(),
        queried.HasValue() && (queried.GetValue().Type == ObjectType::AddressSpace),
        queried.HasValue() && queried.GetValue().Rights.IsEqualTo(AddressSpacePool::CreatedRights), mapped.HasValue(),
        mapped.HasValue() && (&mapped.GetValue().GetObject() ==
                              static_cast<const Kernel::Object::IKernelObject*>(&fixture.Provider.Spaces[0U])),
        first.HasValue() && first.GetValue().Space.IsEqualTo(fixture.Provider.Spaces[0U].GetSnapshot().Identity),
        fixture.Provider.Tables[0U].Owner == &fixture.Provider.Spaces[0U],
        fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 2U * SlotBytes,
        fixture.Domain.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::AddressSpace)] == 2U,
        snapshot.ActiveSlots == 2U, snapshot.FreeSlots == 0U, snapshot.Created == 2U);
}

// A slot whose space cannot be initialized is spent for the boot; the charge
// returns and the next creation takes the next slot.
void TestRefusedSlotIsSpent(TestContext& runner) noexcept
{
    // Each AddressSpace is tens of KiB: fixtures live in static storage.
    static PoolFixture fixture{2U};
    const bool ready = fixture.Initialize();
    fixture.Provider.Refuse = true;
    const CreateResult refused = fixture.Create();
    const UInt64 usage = fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes;
    fixture.Provider.Refuse = false;
    const CreateResult next = fixture.Create();
    const CreateResult none = fixture.Create();
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a refused slot is spent, its charge returned, and the next slot serves", ready,
                   IsFailure(refused, ObjectPoolError::CapacityExceeded), usage == 0U, next.HasValue(),
                   fixture.Provider.Tables[1U].Owner == &fixture.Provider.Spaces[1U],
                   IsFailure(none, ObjectPoolError::CapacityExceeded), snapshot.SpentSlots == 1U,
                   snapshot.ActiveSlots == 1U, snapshot.FreeSlots == 0U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == SlotBytes);
}

void TestRejections(TestContext& runner) noexcept
{
    // Each AddressSpace is tens of KiB: fixtures live in static storage.
    static PoolFixture fixture;
    const bool ready = fixture.Initialize(Limits(SlotBytes - 1U));
    const CreateResult inspectOnly = fixture.Create(fixture.InspectOnly);
    const CreateResult overCharged = fixture.Create();
    const CreateResult unready =
        fixture.Pool.Create(AddressSpaceCreateRequest{.Space = nullptr, .Authority = fixture.Authority});
    ASTERIA_EXPECT(runner, "a creation without Manage, beyond the charge or without a space touches no slot", ready,
                   IsFailure(inspectOnly, ObjectPoolError::CapabilityRejected),
                   !inspectOnly.HasValue() && (inspectOnly.GetError().Capability == CapabilityError::RightsDenied),
                   IsFailure(overCharged, ObjectPoolError::ChargeRejected),
                   IsFailure(unready, ObjectPoolError::InvalidConfiguration), fixture.Provider.Calls == 0U,
                   fixture.Pool.GetSnapshot().FreeSlots == AddressSpacePool::Capacity);
}

// A destroyed space is reclaimed once nothing pins it: quiesced, its charge returned and its slot
// released, so the next creation builds the slot's space again.
void TestDestroyAndCollect(TestContext& runner) noexcept
{
    // Each AddressSpace is tens of KiB: fixtures live in static storage.
    static PoolFixture fixture{2U};
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const ObjectId identity = created.HasValue() ? created.GetValue().Space.GetObjectId() : ObjectId{};
    Kernel::Object::ObjectPin pin;
    const bool pinned =
        fixture.Registry.PinObject(identity, ObjectType::AddressSpace, pin) == Kernel::Object::ObjectPinError::Ok;
    const ObjectPoolFailure wrongType = fixture.Pool.Destroy(fixture.Space, fixture.Authority);
    const ObjectPoolFailure destroyed = fixture.Pool.Destroy(fixture.Space, handle);
    const bool gone = !fixture.Space.Lookup(handle, ObjectType::AddressSpace, CapabilityOperation::Map).HasValue();
    const auto whilePinned = fixture.Pool.Collect();
    const auto quiescing = fixture.Pool.GetSnapshot();
    const bool stillClaimed = fixture.Provider.Tables[0U].Owner == &fixture.Provider.Spaces[0U];
    pin.Reset();
    const auto collected = fixture.Pool.Collect();
    const auto reclaimed = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a destroyed space stops resolving and is quiesced, uncharged and released once unpinned",
                   ready, created.HasValue(), pinned, wrongType.Error == ObjectPoolError::CapabilityRejected,
                   destroyed.Error == ObjectPoolError::None, gone,
                   whilePinned.HasValue() && (whilePinned.GetValue() == 0U), quiescing.QuiescingSlots == 1U,
                   quiescing.Destroyed == 1U, quiescing.Reclaimed == 0U, stillClaimed,
                   collected.HasValue() && (collected.GetValue() == 1U), fixture.Provider.Releases == 1U,
                   fixture.Provider.Tables[0U].Owner == nullptr, reclaimed.FreeSlots == 2U,
                   reclaimed.QuiescingSlots == 0U, reclaimed.Reclaimed == 1U,
                   fixture.Domain.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::AddressSpace)] == 0U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    const CreateResult again = fixture.Create();
    ASTERIA_EXPECT(
        runner, "a released slot builds a space with a new identity", again.HasValue(), fixture.Provider.Calls == 2U,
        again.HasValue() && (again.GetValue().Space.GetObjectId().GetValue() != identity.GetValue()),
        fixture.Provider.Tables[0U].Owner == &fixture.Provider.Spaces[0U], fixture.Pool.GetSnapshot().Created == 2U);

    // A slot the provider cannot release is spent; the charge still returns.
    fixture.Provider.RefuseRelease = true;
    const ObjectPoolFailure second =
        fixture.Pool.Destroy(fixture.Space, again.HasValue() ? again.GetValue().Capability : CapabilityHandle{});
    const auto spent = fixture.Pool.Collect();
    ASTERIA_EXPECT(runner, "a slot the provider cannot release is spent", second.Error == ObjectPoolError::None,
                   spent.HasValue() && (spent.GetValue() == 1U), fixture.Pool.GetSnapshot().SpentSlots == 1U,
                   fixture.Pool.GetSnapshot().FreeSlots == 1U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}
// Limit refusal, exact metadata charge and two concurrently live roots during a restart.
// Destruction waits for the pin.
void TestLargeArenas(TestContext& runner) noexcept
{
    Platform::Amd64::Memory::LargePageTableArenaPool arenas;
    const bool emptyRelease = !arenas.Release(0U) && !arenas.Release(arenas.Capacity);
    const auto a = arenas.TryAcquire();
    const auto b = arenas.TryAcquire();
    const auto beyond = arenas.TryAcquire();
    const bool releasedArena = arenas.Release(a) && !arenas.Release(a);
    ASTERIA_EXPECT(runner, "the native arena pool rejects invalid releases and reuses exactly the released arena",
                   emptyRelease, a == 0U, b == 1U, beyond == arenas.Capacity, releasedArena, arenas.TryAcquire() == a);
    static PoolFixture fixture{4U};
    const bool ready = fixture.Initialize(Limits(8U * 1024U * 1024U));
    const auto create = [&]() noexcept
    {
        return fixture.Pool.Create(
            {.Space = &fixture.Space, .Authority = fixture.Authority, .LargePageTableArena = true});
    };
    const auto old = create();
    Kernel::Object::ObjectPin pin;
    const bool pinned = old.HasValue() &&
                        fixture.Registry.PinObject(old.GetValue().Space.GetObjectId(), ObjectType::AddressSpace, pin) ==
                            Kernel::Object::ObjectPinError::Ok;
    const auto replacement = create();
    const auto exhausted = create();
    ASTERIA_EXPECT(runner, "two large roots fit restart overlap; the third refuses without spending a slot", ready,
                   old.HasValue(), replacement.HasValue(), pinned, !exhausted.HasValue(),
                   exhausted.GetError().Error == ObjectPoolError::CapacityExceeded,
                   fixture.Domain.GetUsage().LargePageTableArenas == 2U,
                   fixture.Domain.GetUsage().KernelMetadataBytes == 2U * TestProvider::LargeBytes,
                   fixture.Pool.GetSnapshot().FreeSlots == 2U, fixture.Pool.GetSnapshot().SpentSlots == 0U);
    const auto destroyed =
        fixture.Pool.Destroy(fixture.Space, old.HasValue() ? old.GetValue().Capability : CapabilityHandle{});
    const auto held = fixture.Pool.Collect();
    const auto stillExhausted = create();
    ASTERIA_EXPECT(runner, "quiescing pinned predecessor retains both its charge and arena",
                   destroyed.Error == ObjectPoolError::None, held.HasValue() && held.GetValue() == 0U,
                   !stillExhausted.HasValue(), fixture.Domain.GetUsage().LargePageTableArenas == 2U);
    pin.Reset();
    const auto released = fixture.Pool.Collect();
    const auto reused = create();
    ASTERIA_EXPECT(runner, "collection releases a large arena and successor reuses it",
                   released.HasValue() && released.GetValue() == 1U, reused.HasValue(),
                   reused.HasValue() && reused.GetValue().Space.GetObjectId().GetValue() !=
                                            old.GetValue().Space.GetObjectId().GetValue(),
                   fixture.Domain.GetUsage().LargePageTableArenas == 2U,
                   fixture.Domain.GetUsage().KernelMetadataBytes == 2U * TestProvider::LargeBytes);
    static PoolFixture application;
    auto limits = Limits(8U * 1024U * 1024U);
    limits.Objects.LargePageTableArenas = 0U;
    const bool appReady = application.Initialize(limits);
    const auto refused = application.Pool.Create(
        {.Space = &application.Space, .Authority = application.Authority, .LargePageTableArena = true});
    const auto ordinary = application.Create();
    ASTERIA_EXPECT(
        runner, "an application with zero large units cannot select one; an ordinary root is still available", appReady,
        !refused.HasValue(), refused.GetError().Resource == ResourceDomainError::ObjectLimitExceeded,
        ordinary.HasValue(), application.Provider.Calls == 1U, application.Domain.GetUsage().LargePageTableArenas == 0U,
        application.Domain.GetUsage().KernelMetadataBytes == SlotBytes);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"AddressSpacePoolTests"};
    Asteria::Test::TestLargeArenas(runner);
    Asteria::Test::TestConfiguration(runner);
    Asteria::Test::TestCreateChargesAndMints(runner);
    Asteria::Test::TestRefusedSlotIsSpent(runner);
    Asteria::Test::TestRejections(runner);
    Asteria::Test::TestDestroyAndCollect(runner);
    return runner.Finish();
}
