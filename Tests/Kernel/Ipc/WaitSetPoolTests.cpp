// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "Kernel/Ipc/EndpointPool.hpp"
#include "Kernel/Ipc/WaitSetPool.hpp"
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
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointConfiguration;
using Kernel::Ipc::EndpointCreateRequest;
using Kernel::Ipc::EndpointPool;
using Kernel::Ipc::WaitSet;
using Kernel::Ipc::WaitSetCreateRequest;
using Kernel::Ipc::WaitSetCreation;
using Kernel::Ipc::WaitSetError;
using Kernel::Ipc::WaitSetPool;
using Kernel::Ipc::WaitSetPoolError;
using Kernel::Ipc::WaitSetPoolFailure;
using Kernel::Ipc::WaitSetPoolState;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
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

using CreateResult = Foundation::Result<WaitSetCreation, WaitSetPoolFailure>;

constexpr ResourceDomainId Charged{5U};

[[nodiscard]] constexpr ResourceDomainLimits Limits(const UInt64 metadataBytes = 1U << 20U) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = metadataBytes,
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 1U << 20U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

class PoolFixture final
{
public:
    PoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Pool{Registry, Issuer}, Endpoints{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const ResourceDomainLimits& limits = Limits()) noexcept
    {
        if (Domain.InitializeRoot(Charged, limits).Error != ResourceDomainError::None)
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
        Authority = Mint(DomainObject, CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect));
        return Authority.IsValid();
    }

    [[nodiscard]] CapabilityHandle Mint(const ObjectId object, const CapabilityRights rights) noexcept
    {
        const auto minted = Issuer.Mint(Space, object, rights, RevocationRoot, MonotonicTime::Infinite(), 0U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] CreateResult Create() noexcept
    {
        return Pool.Create(WaitSetCreateRequest{.Space = &Space, .Authority = Authority});
    }

    [[nodiscard]] WaitSet* Find(const CapabilityHandle handle) noexcept
    {
        const auto lease = Space.Lookup(handle, ObjectType::WaitSet, CapabilityOperation::Manage);
        return lease.HasValue() ? &static_cast<WaitSet&>(lease.GetValue().GetObject()) : nullptr;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    WaitSetPool Pool;
    EndpointPool Endpoints;
    ObjectId DomainObject{};
    RevocationDomainId RevocationRoot{};
    CapabilityHandle Authority{};
};

[[nodiscard]] bool IsFailure(const CreateResult& result, const WaitSetPoolError error) noexcept
{
    return !result.HasValue() && (result.GetError().Error == error);
}

// ObjectCreate type 12 charges a WaitSet to the authorizing domain and mints its first capability.
[[gnu::noinline]] void TestChargeLimit(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto ownedLimited = std::make_unique<PoolFixture>();
    PoolFixture& limited = *ownedLimited;
    const bool limitedReady = limited.Initialize(Limits(sizeof(WaitSet) - 1U));
    const Size limitedBefore = limited.Registry.GetActiveCount();
    const CreateResult overLimit = limited.Create();
    ASTERIA_EXPECT(runner, "a domain limit refuses the charge and leaves no object", limitedReady,
                   IsFailure(overLimit, WaitSetPoolError::ChargeRejected),
                   limited.Registry.GetActiveCount() == limitedBefore,
                   limited.Pool.GetSnapshot().FreeSlots == WaitSetPool::Capacity);
}

void TestCreateChargesAndMints(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const Size before = fixture.Registry.GetActiveCount();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const auto queried = fixture.Space.Query(handle);
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a created WaitSet is charged, registered and its capability minted", ready,
                   created.HasValue(), queried.HasValue(),
                   queried.HasValue() && (queried.GetValue().Type == ObjectType::WaitSet),
                   queried.HasValue() && queried.GetValue().Rights.IsEqualTo(WaitSetPool::CreatedRights),
                   queried.HasValue() && (queried.GetValue().ChargeOwner.GetValue() == Charged.GetValue()),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(WaitSet),
                   fixture.Registry.GetActiveCount() == (before + 1U), snapshot.ActiveSlots == 1U,
                   snapshot.Created == 1U, snapshot.State == WaitSetPoolState::Ready,
                   !WaitSetPool::CreatedRights.ContainsAll(CapabilityRights{CapabilityRight::Revoke}));

    // The pool is full after Capacity creations, and charges nothing more.
    bool filled{created.HasValue()};
    for (Size index{1U}; index < WaitSetPool::Capacity; ++index)
    {
        filled = filled && fixture.Create().HasValue();
    }
    const CreateResult full = fixture.Create();
    ASTERIA_EXPECT(runner, "a full pool refuses a creation without charging", filled,
                   IsFailure(full, WaitSetPoolError::CapacityExceeded),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == (WaitSetPool::Capacity * sizeof(WaitSet)));

    TestChargeLimit(runner);
}

// A destroyed WaitSet goes when its last pin does: a blocked waiter's pin
// keeps it until the wait completes.
void TestDestroyAndCollect(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const Size before = fixture.Registry.GetActiveCount();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    ObjectPin waiter{};
    const bool pinned =
        created.HasValue() &&
        (fixture.Registry.PinObject(created.GetValue().WaitSet, ObjectType::WaitSet, waiter) == ObjectPinError::Ok);
    const bool destroyed = fixture.Pool.Destroy(fixture.Space, handle).HasValue();
    const auto lookup = fixture.Space.Lookup(handle, ObjectType::WaitSet, CapabilityOperation::Wait);
    const auto held = fixture.Pool.Collect();
    const auto quiescing = fixture.Pool.GetSnapshot();
    waiter.Reset();
    const auto collected = fixture.Pool.Collect();
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a destroyed WaitSet is unreachable and reclaimed once its waiter's pin is gone", ready,
                   created.HasValue(), pinned, destroyed,
                   !lookup.HasValue() && (lookup.GetError() == CapabilityError::ObjectNotFound),
                   held.HasValue() && (held.GetValue() == 0U), quiescing.QuiescingSlots == 1U,
                   collected.HasValue() && (collected.GetValue() == 1U), snapshot.FreeSlots == WaitSetPool::Capacity,
                   snapshot.Reclaimed == 1U, fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U,
                   fixture.Registry.GetActiveCount() == before, fixture.Space.Close(handle) == CapabilityError::Ok);
}

// Only a pool WaitSet with Manage can be destroyed: a boot WaitSet the pool
// does not own is NotOwned, and a Wait-only copy is refused.
void TestDestroyAuthority(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    WaitSet boot{fixture.Registry};
    const bool bootReady = boot.Initialize(Charged) == WaitSetError::None;
    const auto registered = fixture.Issuer.CreateObject(boot);
    const CapabilityHandle bootHandle =
        registered.HasValue()
            ? fixture.Mint(registered.GetValue(), CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Wait))
            : CapabilityHandle{};
    const auto foreign = fixture.Pool.Destroy(fixture.Space, bootHandle);

    const CreateResult created = fixture.Create();
    const auto waitOnly =
        created.HasValue()
            ? fixture.Space.Duplicate(created.GetValue().Capability, CapabilityRights{CapabilityRight::Wait},
                                      MonotonicTime::Infinite())
            : Foundation::Result<CapabilityHandle, CapabilityError>::Failure(CapabilityError::InvalidHandle);
    const auto refused = waitOnly.HasValue() ? fixture.Pool.Destroy(fixture.Space, waitOnly.GetValue())
                                             : fixture.Pool.Destroy(fixture.Space, CapabilityHandle{});
    ASTERIA_EXPECT(runner, "a boot WaitSet is not the pool's to destroy, and destroying needs Manage", ready, bootReady,
                   registered.HasValue(),
                   !foreign.HasValue() && (foreign.GetError().Error == WaitSetPoolError::NotOwned), created.HasValue(),
                   waitOnly.HasValue(),
                   !refused.HasValue() && (refused.GetError().Error == WaitSetPoolError::CapabilityRejected),
                   fixture.Pool.GetSnapshot().State == WaitSetPoolState::Ready);
    if (registered.HasValue())
    {
        static_cast<void>(fixture.Issuer.DestroyObject(registered.GetValue()));
        static_cast<void>(fixture.Issuer.CollectDestroyedObject(registered.GetValue()));
    }
}

// A WaitSet's registration pins a bound Endpoint's readiness, so a destroyed Endpoint stays until
// the WaitSet is collected; collecting WaitSets first then frees both in one pass.
void TestRegistrationHoldsEndpoint(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const Size before = fixture.Registry.GetActiveCount();
    const auto endpoint = fixture.Endpoints.Create(EndpointCreateRequest{
        .Space = &fixture.Space,
        .Authority = fixture.Authority,
        .Configuration = EndpointConfiguration::Default(),
        .Receiver = nullptr,
        .ReceiverObject = {},
        .SealedSelector = 0U,
    });
    const CreateResult created = fixture.Create();
    const CapabilityHandle endpointHandle = endpoint.HasValue() ? endpoint.GetValue().Capability : CapabilityHandle{};
    const CapabilityHandle waitSetHandle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    bool bound{false};
    {
        const auto readable = fixture.Space.Lookup(endpointHandle, ObjectType::Endpoint, CapabilityOperation::Read);
        WaitSet* const waitSet = fixture.Find(waitSetHandle);
        if (readable.HasValue() && (waitSet != nullptr))
        {
            Endpoint& bindable = static_cast<Endpoint&>(readable.GetValue().GetObject());
            bound = (bindable.GetReadiness() != nullptr) &&
                    waitSet->RegisterNotification(bindable.GetReadinessId(), *bindable.GetReadiness(), 7U).HasValue();
        }
    }
    const bool endpointDestroyed = fixture.Endpoints.Destroy(fixture.Space, endpointHandle).HasValue();
    const auto held = fixture.Endpoints.Collect();
    const bool waitSetDestroyed = fixture.Pool.Destroy(fixture.Space, waitSetHandle).HasValue();
    const auto waitSets = fixture.Pool.Collect();
    const auto endpoints = fixture.Endpoints.Collect();
    ASTERIA_EXPECT(
        runner, "a bound Endpoint's slot is freed once its WaitSet is collected first", ready, endpoint.HasValue(),
        created.HasValue(), bound, endpointDestroyed, held.HasValue() && (held.GetValue() == 0U), waitSetDestroyed,
        waitSets.HasValue() && (waitSets.GetValue() == 1U), endpoints.HasValue() && (endpoints.GetValue() == 1U),
        fixture.Endpoints.GetSnapshot().FreeSlots == EndpointPool::Capacity,
        fixture.Pool.GetSnapshot().FreeSlots == WaitSetPool::Capacity, fixture.Registry.GetActiveCount() == before,
        fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"WaitSetPoolTests"};
    Asteria::Test::TestCreateChargesAndMints(runner);
    Asteria::Test::TestDestroyAndCollect(runner);
    Asteria::Test::TestDestroyAuthority(runner);
    Asteria::Test::TestRegistrationHoldsEndpoint(runner);
    return runner.Finish();
}
