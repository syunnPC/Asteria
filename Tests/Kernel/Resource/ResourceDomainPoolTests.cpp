// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include "Abi/SystemCallAbi.hpp"

#include "Kernel/Ipc/WaitSetPool.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"
#include "Kernel/Resource/ResourceDomainPool.hpp"

#include "Support/ManualClock.hpp"
#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::UInt64;
using Kernel::Ipc::WaitSetCreateRequest;
using Kernel::Ipc::WaitSetPool;
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
using Kernel::Resource::ResourceDomainCreateRequest;
using Kernel::Resource::ResourceDomainCreation;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainPool;
using Kernel::Resource::ResourceDomainPoolError;
using Kernel::Resource::ResourceDomainPoolFailure;
using Kernel::Resource::ResourceDomainPoolState;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

using CreateResult = Foundation::Result<ResourceDomainCreation, ResourceDomainPoolFailure>;

constexpr ResourceDomainId RootIdentity{1U};
constexpr ResourceDomainId FirstIdentity{3U};
constexpr UInt64 RootMetadata{1U << 20U};

[[nodiscard]] constexpr ResourceDomainLimits RootLimits(const UInt64 children) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 64U,
        .ResidentMemoryPages = 64U,
        .KernelMetadataBytes = RootMetadata,
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 1U << 16U,
        .Threads = 4U,
        .ChildDomains = children,
    };
}

[[nodiscard]] constexpr ResourceDomainLimits ChildLimits(const UInt64 pages = 16U, const UInt64 children = 0U) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = pages,
        .ResidentMemoryPages = pages,
        .KernelMetadataBytes = 0U,
        .CapabilityHandles = 0U,
        .IpcQueueBytes = 1U << 12U,
        .Threads = 1U,
        .ChildDomains = children,
    };
}

class PoolFixture final
{
public:
    PoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Root{Ledger},
          Pool{Registry, Issuer}, WaitSets{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const UInt64 children = 4U) noexcept
    {
        if ((Root.InitializeRoot(RootIdentity, RootLimits(children)).Error != ResourceDomainError::None) ||
            !Pool.Configure(Ledger, FirstIdentity))
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Root);
        const auto revocations = Revocations.CreateRoot();
        if (!registered.HasValue() || !revocations.HasValue())
        {
            return false;
        }
        RevocationRoot = revocations.GetValue();
        const auto minted = Issuer.Mint(Space, registered.GetValue(),
                                        CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect),
                                        RevocationRoot, MonotonicTime::Infinite(), 0U);
        Authority = minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
        return Authority.IsValid();
    }

    [[nodiscard]] CreateResult Create(const ResourceDomainLimits& limits, const CapabilityHandle authority) noexcept
    {
        return Pool.Create(ResourceDomainCreateRequest{.Space = &Space, .Authority = authority, .Limits = limits});
    }

    [[nodiscard]] CreateResult Create(const ResourceDomainLimits& limits = ChildLimits()) noexcept
    {
        return Create(limits, Authority);
    }

    [[nodiscard]] ResourceDomain* Find(const CapabilityHandle handle) noexcept
    {
        const auto lease = Space.Lookup(handle, ObjectType::ResourceDomain, CapabilityOperation::Manage);
        return lease.HasValue() ? &static_cast<ResourceDomain&>(lease.GetValue().GetObject()) : nullptr;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Root;
    ResourceDomainPool Pool;
    WaitSetPool WaitSets;
    RevocationDomainId RevocationRoot{};
    CapabilityHandle Authority{};
};

[[nodiscard]] bool IsFailure(const CreateResult& result, const ResourceDomainPoolError error) noexcept
{
    return !result.HasValue() && (result.GetError().Error == error);
}

// A child under its parent, the parent charged its slot, the next identity, the parent's metadata
// limit, the creator's capability with Manage, Inspect, Duplicate, Transfer and Derive.
void TestCreate(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool initialized = fixture.Initialize();
    const CreateResult first = fixture.Create();
    const CreateResult second = fixture.Create(ChildLimits(8U));
    ResourceDomain* const child = first.HasValue() ? fixture.Find(first.GetValue().Capability) : nullptr;
    const auto rootSnapshot = fixture.Root.GetSnapshot();
    const auto childSnapshot = (child != nullptr) ? child->GetSnapshot() : decltype(rootSnapshot){};
    const auto rights = fixture.Space.Lookup(first.HasValue() ? first.GetValue().Capability : CapabilityHandle{},
                                             ObjectType::ResourceDomain, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(
        runner, "a child domain is created under its parent with the next identity and its parent's metadata limit",
        initialized, first.HasValue(), second.HasValue(), child != nullptr,
        first.HasValue() && (first.GetValue().Identity.GetValue() == 3U),
        second.HasValue() && (second.GetValue().Identity.GetValue() == 4U),
        childSnapshot.Limits.KernelMetadataBytes == RootMetadata, childSnapshot.Limits.CommittedMemoryPages == 16U,
        childSnapshot.Depth == 1U, rootSnapshot.Usage.ChildDomains == 2U,
        rootSnapshot.Usage.KernelMetadataBytes == (2U * sizeof(ResourceDomain)),
        rights.HasValue() && rights.GetValue().GetDescriptor().Rights.IsEqualTo(ResourceDomainPool::CreatedRights),
        fixture.Pool.GetSnapshot().ActiveSlots == 2U, fixture.Pool.GetSnapshot().NextIdentity == 5U);

    // A child is itself an authority: what it creates is charged to it and
    // to every ancestor.
    const auto charged =
        (child != nullptr)
            ? fixture.WaitSets.Create(
                  WaitSetCreateRequest{.Space = &fixture.Space, .Authority = first.GetValue().Capability})
            : decltype(fixture.WaitSets.Create(WaitSetCreateRequest{}))::Failure(Kernel::Object::MakeObjectPoolFailure(
                  ResourceDomainPoolError::InvalidRequest, Kernel::Object::CapabilityError::Ok,
                  ResourceDomainError::None, Kernel::Object::ObjectRegistryError::Ok, 0U));
    ASTERIA_EXPECT(runner, "an object created under the child charges the child and its parent", charged.HasValue(),
                   (child != nullptr) && (child->GetSnapshot().Usage.KernelMetadataBytes ==
                                          WaitSetPool::GetReservation().KernelMetadataBytes),
                   fixture.Root.GetSnapshot().Usage.KernelMetadataBytes ==
                       ((2U * sizeof(ResourceDomain)) + WaitSetPool::GetReservation().KernelMetadataBytes));
}

// The child's limits stay within its parent's; metadata and handles are the
// kernel's to set; the parent's ChildDomains, the pool's capacity and the
// authority's rights bound creation; a refused creation charges nothing and
// spends no identity.
void TestRefusals(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool initialized = fixture.Initialize(2U);
    ResourceDomainLimits metadata = ChildLimits();
    metadata.KernelMetadataBytes = 64U;
    ResourceDomainLimits handles = ChildLimits();
    handles.CapabilityHandles = 1U;
    const CreateResult tooLarge = fixture.Create(ChildLimits(65U));
    const CreateResult withMetadata = fixture.Create(metadata);
    const CreateResult withHandles = fixture.Create(handles);
    const UInt64 identityAfterRefusals = fixture.Pool.GetSnapshot().NextIdentity;
    const CreateResult first = fixture.Create();
    const CreateResult second = fixture.Create();
    const CreateResult third = fixture.Create();
    const CapabilityHandle inspectOnly = first.HasValue() ? [&]() noexcept
    {
        const auto lease =
            fixture.Space.Lookup(first.GetValue().Capability, ObjectType::ResourceDomain, CapabilityOperation::Inspect);
        const auto minted =
            lease.HasValue()
                ? fixture.Issuer.Mint(fixture.Space, lease.GetValue().GetDescriptor().Object,
                                      CapabilityRights{CapabilityRight::Inspect}, fixture.RevocationRoot,
                                      MonotonicTime::Infinite(), 0U)
                : decltype(fixture.Issuer.Mint(fixture.Space, ObjectId{}, CapabilityRights{}, fixture.RevocationRoot,
                                               MonotonicTime::Infinite(),
                                               0U))::Failure(Kernel::Object::CapabilityError::InvalidHandle);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }()
                                                          : CapabilityHandle{};
    const CreateResult withoutManage = fixture.Create(ChildLimits(), inspectOnly);
    const CreateResult grandchild = first.HasValue() ? fixture.Create(ChildLimits(), first.GetValue().Capability)
                                                     : CreateResult::Failure(ResourceDomainPoolFailure{});
    ASTERIA_EXPECT(
        runner, "limits beyond the parent's, set metadata or handles, a third child of two, or no Manage are refused",
        initialized, IsFailure(tooLarge, ResourceDomainPoolError::InvalidRequest),
        IsFailure(withMetadata, ResourceDomainPoolError::InvalidRequest),
        IsFailure(withHandles, ResourceDomainPoolError::InvalidRequest), identityAfterRefusals == 3U, first.HasValue(),
        second.HasValue(), IsFailure(third, ResourceDomainPoolError::InvalidRequest),
        third.HasValue() || (third.GetError().Resource == ResourceDomainError::LimitExceeded),
        IsFailure(withoutManage, ResourceDomainPoolError::CapabilityRejected),
        IsFailure(grandchild, ResourceDomainPoolError::InvalidRequest),
        fixture.Root.GetSnapshot().Usage.ChildDomains == 2U,
        fixture.Root.GetSnapshot().Usage.KernelMetadataBytes == (2U * sizeof(ResourceDomain)));

    // The pool is too large for a test function's stack budget.
    const auto ownedFull = std::make_unique<PoolFixture>();
    PoolFixture& full = *ownedFull;
    const bool fullInitialized = full.Initialize(ResourceDomainPool::Capacity + 1U);
    bool created{true};
    for (Foundation::Size index{0U}; index < ResourceDomainPool::Capacity; ++index)
    {
        created = created && full.Create().HasValue();
    }
    ASTERIA_EXPECT(runner, "a full pool refuses a child beyond its capacity", fullInitialized, created,
                   IsFailure(full.Create(), ResourceDomainPoolError::CapacityExceeded));

    // The pool is too large for a test function's stack budget.
    const auto ownedUnconfigured = std::make_unique<PoolFixture>();
    PoolFixture& unconfigured = *ownedUnconfigured;
    ASTERIA_EXPECT(
        runner, "an unconfigured pool creates nothing, and configures once",
        IsFailure(unconfigured.Pool.Create(ResourceDomainCreateRequest{
                      .Space = &unconfigured.Space, .Authority = CapabilityHandle{}, .Limits = ChildLimits()}),
                  ResourceDomainPoolError::InvalidConfiguration),
        unconfigured.Pool.GetSnapshot().State == ResourceDomainPoolState::Unconfigured,
        !unconfigured.Pool.Configure(unconfigured.Ledger, ResourceDomainId{}),
        unconfigured.Pool.Configure(unconfigured.Ledger, FirstIdentity),
        !unconfigured.Pool.Configure(unconfigured.Ledger, FirstIdentity));
}

// The subtree a drain destroys is named by the child's registry identity. Destroying it marks the
// child and its descendants, Collect reclaims descendants before ancestors, and a successor in a
// reclaimed slot is never in the old subtree.
void TestDestroySubtree(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto storage = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *storage;
    const bool initialized = fixture.Initialize();
    const CreateResult outer = fixture.Create(ChildLimits(16U, 1U));
    const CreateResult sibling = fixture.Create();
    const CreateResult inner = outer.HasValue() ? fixture.Create(ChildLimits(8U), outer.GetValue().Capability)
                                                : CreateResult::Failure(ResourceDomainPoolFailure{});
    const CapabilityHandle outerHandle = outer.HasValue() ? outer.GetValue().Capability : CapabilityHandle{};
    const CapabilityHandle innerHandle = inner.HasValue() ? inner.GetValue().Capability : CapabilityHandle{};
    ResourceDomain* const outerDomain = fixture.Find(outerHandle);
    ResourceDomain* const innerDomain = fixture.Find(innerHandle);
    ResourceDomain* const siblingDomain =
        fixture.Find(sibling.HasValue() ? sibling.GetValue().Capability : CapabilityHandle{});
    const auto owned = fixture.Pool.FindOwned(fixture.Space, outerHandle);
    const auto notOwned = fixture.Pool.FindOwned(fixture.Space, fixture.Authority);
    const ObjectId root = owned.HasValue() ? owned.GetValue() : ObjectId{};
    ASTERIA_EXPECT(runner, "a child's identity names it and its descendants, not its sibling or its parent",
                   initialized, outer.HasValue(), sibling.HasValue(), inner.HasValue(), owned.HasValue(),
                   !notOwned.HasValue() && (notOwned.GetError().Error == ResourceDomainPoolError::NotOwned),
                   fixture.Pool.IsInSubtree(outerDomain, root), fixture.Pool.IsInSubtree(innerDomain, root),
                   !fixture.Pool.IsInSubtree(siblingDomain, root), !fixture.Pool.IsInSubtree(&fixture.Root, root),
                   !fixture.Pool.IsInSubtree(nullptr, root));

    const ResourceDomainPoolFailure destroyed = fixture.Pool.DestroySubtree(root);
    const auto marked = fixture.Pool.GetSnapshot();
    const auto collected = fixture.Pool.Collect();
    const auto stale = fixture.Space.Lookup(innerHandle, ObjectType::ResourceDomain, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "destroying it marks both, and Collect reclaims the inner before the outer",
                   destroyed.Error == ResourceDomainPoolError::None, marked.QuiescingSlots == 2U,
                   marked.Destroyed == 2U, collected.HasValue() && (collected.GetValue() == 2U),
                   !stale.HasValue() && (stale.GetError() == Kernel::Object::CapabilityError::ObjectNotFound),
                   fixture.Find(sibling.HasValue() ? sibling.GetValue().Capability : CapabilityHandle{}) ==
                       siblingDomain,
                   fixture.Pool.GetSnapshot().ActiveSlots == 1U, fixture.Pool.GetSnapshot().Reclaimed == 2U,
                   fixture.Root.GetSnapshot().Usage.ChildDomains == 1U,
                   fixture.Root.GetSnapshot().Usage.KernelMetadataBytes == sizeof(ResourceDomain));

    const ResourceDomainPoolFailure again = fixture.Pool.DestroySubtree(root);
    const CreateResult successor = fixture.Create();
    ResourceDomain* const successorDomain =
        fixture.Find(successor.HasValue() ? successor.GetValue().Capability : CapabilityHandle{});
    ASTERIA_EXPECT(runner, "the old identity names nothing any more, a successor in its slot included",
                   again.Error == ResourceDomainPoolError::NotOwned, successor.HasValue(), successorDomain != nullptr,
                   !fixture.Pool.IsInSubtree(successorDomain, root), fixture.Pool.GetSnapshot().ActiveSlots == 2U);
}
void TestCodeAndTypeLimits(TestContext& runner) noexcept
{
    static PoolFixture fixture;
    const bool initialized = fixture.Initialize();
    const auto before = fixture.Root.GetSnapshot().Usage;
    const auto denied = fixture.Pool.Create({.Space = &fixture.Space,
                                             .Authority = fixture.Authority,
                                             .Limits = ChildLimits(),
                                             .Flags = Abi::ObjectCreateDomainFlagCode});
    const auto unknown = fixture.Pool.Create(
        {.Space = &fixture.Space, .Authority = fixture.Authority, .Limits = ChildLimits(), .Flags = 2U});
    const auto descriptor = fixture.Space.Query(fixture.Authority);
    const auto codeAuthority =
        fixture.Issuer.Mint(fixture.Space, descriptor.GetValue().Object,
                            CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Code),
                            fixture.RevocationRoot, MonotonicTime::Infinite(), 0U);
    ResourceDomainLimits limits = ChildLimits();
    limits.Objects.Counts[static_cast<Foundation::Size>(Abi::LimitedObject::WaitSet)] = 1U;
    limits.Objects.LargePageTableArenas = 1U;
    const auto code = fixture.Pool.Create({.Space = &fixture.Space,
                                           .Authority = codeAuthority.GetValue(),
                                           .Limits = limits,
                                           .Flags = Abi::ObjectCreateDomainFlagCode,
                                           .ExplicitObjectLimits = true});
    const auto child = code.HasValue() ? fixture.Find(code.GetValue().Capability) : nullptr;
    const auto queried = fixture.Space.Query(code.HasValue() ? code.GetValue().Capability : CapabilityHandle{});
    const auto first =
        code.HasValue() ? fixture.WaitSets.Create({.Space = &fixture.Space, .Authority = code.GetValue().Capability})
                        : decltype(fixture.WaitSets.Create({}))::Failure(
                              Kernel::Object::MakeObjectPoolFailure(Kernel::Object::ObjectPoolError::InvalidRequest));
    const auto beyond =
        code.HasValue() ? fixture.WaitSets.Create({.Space = &fixture.Space, .Authority = code.GetValue().Capability})
                        : decltype(fixture.WaitSets.Create({}))::Failure(
                              Kernel::Object::MakeObjectPoolFailure(Kernel::Object::ObjectPoolError::InvalidRequest));
    ASTERIA_EXPECT(
        runner, "Code denial creates nothing; only a Code authority mints Code, with explicit type and arena limits",
        initialized, IsFailure(denied, ResourceDomainPoolError::CapabilityRejected),
        !denied.HasValue() && denied.GetError().Capability == Kernel::Object::CapabilityError::RightsDenied,
        IsFailure(unknown, ResourceDomainPoolError::InvalidRequest), before.KernelMetadataBytes == 0U, code.HasValue(),
        queried.HasValue() && queried.GetValue().Rights.ContainsAll(CapabilityRights{CapabilityRight::Code}),
        child != nullptr && child->GetSnapshot().Limits.Objects.LargePageTableArenas == 1U, first.HasValue(),
        !beyond.HasValue() && beyond.GetError().Resource == ResourceDomainError::ObjectLimitExceeded);
    if (first.HasValue())
    {
        static_cast<void>(fixture.WaitSets.Destroy(fixture.Space, first.GetValue().Capability));
        static_cast<void>(fixture.WaitSets.Collect());
    }
    ASTERIA_EXPECT(
        runner, "collection releases the per-type unit for another creation",
        fixture.WaitSets.Create({.Space = &fixture.Space, .Authority = code.GetValue().Capability}).HasValue());
    limits.Objects.Counts[0U] = static_cast<Foundation::UInt16>(Abi::ObjectPoolCapacities[0U] + 1U);
    const auto tooLarge = fixture.Pool.Create(
        {.Space = &fixture.Space, .Authority = fixture.Authority, .Limits = limits, .ExplicitObjectLimits = true});
    const auto legacy = fixture.Create();
    ASTERIA_EXPECT(
        runner, "limits must fit the parent; the legacy request inherits eight limits and gives no large arenas",
        IsFailure(tooLarge, ResourceDomainPoolError::InvalidRequest), legacy.HasValue(),
        legacy.HasValue() && fixture.Find(legacy.GetValue().Capability)->GetSnapshot().Limits.Objects.Counts[0U] ==
                                 Abi::ObjectPoolCapacities[0U],
        legacy.HasValue() &&
            fixture.Find(legacy.GetValue().Capability)->GetSnapshot().Limits.Objects.LargePageTableArenas == 0U);
}

} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ResourceDomainPoolTests"};
    Asteria::Test::TestCodeAndTypeLimits(runner);
    Asteria::Test::TestCreate(runner);
    Asteria::Test::TestRefusals(runner);
    Asteria::Test::TestDestroySubtree(runner);
    return runner.Finish();
}
