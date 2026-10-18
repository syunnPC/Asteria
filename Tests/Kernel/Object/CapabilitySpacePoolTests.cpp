// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "Kernel/Object/CapabilitySpacePool.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/CapabilityTransfer.hpp"
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
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::CapabilitySpaceCreateRequest;
using Kernel::Object::CapabilitySpaceCreation;
using Kernel::Object::CapabilitySpaceObject;
using Kernel::Object::CapabilitySpacePool;
using Kernel::Object::CapabilitySpacePoolState;
using Kernel::Object::CapabilityTransferDisposition;
using Kernel::Object::CapabilityTransferRequest;
using Kernel::Object::CapabilityTransferRequestView;
using Kernel::Object::CapabilityValidationKey;
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

using CreateResult = Foundation::Result<CapabilitySpaceCreation, ObjectPoolFailure>;

constexpr ResourceDomainId Charged{5U};
constexpr CapabilityValidationKey Keys[CapabilitySpacePool::Capacity]{
    {0x1111'2222'3333'4444ULL, 0x5555'6666'7777'8888ULL}, {0x9999'AAAA'BBBB'CCCCULL, 0xDDDD'EEEE'FFFF'0001ULL},
    {0x1357'9BDF'2468'ACE0ULL, 0x0ECA'8642'FDB9'7531ULL}, {0x0F1E'2D3C'4B5A'6978ULL, 0x8796'A5B4'C3D2'E1F0ULL},
    {0x2B3C'4D5E'6F70'8192ULL, 0xA3B4'C5D6'E7F8'091AULL}, {0xED2E'F1C1'13D1'E9E3ULL, 0x2507'759B'36AF'971FULL},
    {0xB2C7'5357'4D99'D19DULL, 0x3CE0'216C'E674'6773ULL}, {0x0639'F08B'7F0A'674DULL, 0x09DE'6E53'B861'AFB7ULL},
    {0x5320'DFF0'19A9'0675ULL, 0x5491'3BE5'8249'0B3BULL}, {0x0E9B'A56D'D7D3'A0AFULL, 0xB0C9'049E'85D6'2CF3ULL},
    {0xFE1F'014E'F1D7'E893ULL, 0x7801'5F97'E1BD'A755ULL}, {0x99E9'0C3B'5EF7'4753ULL, 0x8ADB'5A90'0030'E565ULL},
    {0xC812'AB06'C159'30B7ULL, 0x8650'2637'205C'5A85ULL}, {0x7C47'BA50'0268'BFA9ULL, 0xC5E2'EC79'BB0E'1DC5ULL},
    {0x33F5'8438'D7C4'7D97ULL, 0x49AF'3AA5'D629'F1F1ULL}, {0xECD2'8F49'F414'602BULL, 0x723D'EAA9'33A0'A95DULL},
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
    PoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Pool{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize(const ResourceDomainLimits& limits = Limits(), const bool configure = true) noexcept
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
        InspectOnly = Mint(DomainObject, CapabilityRights{CapabilityRight::Inspect});
        return Authority.IsValid() && InspectOnly.IsValid() && (!configure || Pool.Configure(Revocations, Clock, Keys));
    }

    [[nodiscard]] CapabilityHandle Mint(const ObjectId object, const CapabilityRights rights) noexcept
    {
        const auto minted = Issuer.Mint(Space, object, rights, RevocationRoot, MonotonicTime::Infinite(), 0U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] CreateResult Create(const CapabilityHandle authority) noexcept
    {
        return Pool.Create(CapabilitySpaceCreateRequest{.Space = &Space, .Authority = authority});
    }

    [[nodiscard]] CreateResult Create() noexcept
    {
        return Create(Authority);
    }

    /// The created space a handle in Space names, or null.
    [[nodiscard]] CapabilitySpace* Resolve(const CapabilityHandle handle) noexcept
    {
        const auto lease = Space.Lookup(handle, ObjectType::CapabilitySpace, CapabilityOperation::Manage);
        return lease.HasValue() ? &static_cast<CapabilitySpaceObject&>(lease.GetValue().GetObject()).GetSpace()
                                : nullptr;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    CapabilitySpacePool Pool;
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
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize(Limits(), false);
    const CreateResult unconfigured = fixture.Create();
    const auto before = fixture.Pool.GetSnapshot();
    constexpr CapabilityValidationKey ZeroKey[CapabilitySpacePool::Capacity]{
        {0x1U, 0x2U},
        {0U, 0U},
    };
    const bool zero = fixture.Pool.Configure(fixture.Revocations, fixture.Clock, ZeroKey);
    const bool configured = fixture.Pool.Configure(fixture.Revocations, fixture.Clock, Keys);
    const bool again = fixture.Pool.Configure(fixture.Revocations, fixture.Clock, Keys);
    ASTERIA_EXPECT(runner, "an unconfigured pool creates no space, and a zero key or a second Configure is refused",
                   ready, IsFailure(unconfigured, ObjectPoolError::CapacityExceeded),
                   before.State == CapabilitySpacePoolState::Unconfigured, before.FreeSlots == 0U, !zero, configured,
                   !again, fixture.Pool.GetSnapshot().State == CapabilitySpacePoolState::Ready,
                   fixture.Pool.GetSnapshot().FreeSlots == CapabilitySpacePool::Capacity,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);
}

void TestCreateChargesAndMints(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult first = fixture.Create();
    const CreateResult second = fixture.Create();
    bool others{true};
    for (Size index{2U}; index < CapabilitySpacePool::Capacity; ++index)
    {
        others = others && fixture.Create().HasValue();
    }
    const CreateResult beyond = fixture.Create();
    const CapabilityHandle firstHandle = first.HasValue() ? first.GetValue().Capability : CapabilityHandle{};
    const CapabilityHandle secondHandle = second.HasValue() ? second.GetValue().Capability : CapabilityHandle{};
    const auto queried = fixture.Space.Query(firstHandle);
    CapabilitySpace* const firstSpace = fixture.Resolve(firstHandle);
    CapabilitySpace* const secondSpace = fixture.Resolve(secondHandle);
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "each creation charges one space, mints its capability and builds a ready space", ready,
                   first.HasValue(), second.HasValue(), others, IsFailure(beyond, ObjectPoolError::CapacityExceeded),
                   queried.HasValue(), queried.HasValue() && (queried.GetValue().Type == ObjectType::CapabilitySpace),
                   queried.HasValue() && queried.GetValue().Rights.IsEqualTo(CapabilitySpacePool::CreatedRights),
                   queried.HasValue() && (queried.GetValue().ChargeOwner.GetValue() == Charged.GetValue()),
                   firstSpace != nullptr, secondSpace != nullptr, firstSpace != secondSpace,
                   (firstSpace != nullptr) && firstSpace->IsReady() && firstSpace->IsFromRegistry(fixture.Registry) &&
                       firstSpace->IsFromRevocationRegistry(fixture.Revocations),
                   (firstSpace != nullptr) && (secondSpace != nullptr) &&
                       (firstSpace->GetIdentifier().GetValue() != secondSpace->GetIdentifier().GetValue()) &&
                       (firstSpace->GetIdentifier().GetValue() != fixture.Space.GetIdentifier().GetValue()),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes ==
                       CapabilitySpacePool::Capacity * CapabilitySpacePool::GetReservation().KernelMetadataBytes,
                   fixture.Domain.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::CapabilitySpace)] ==
                       CapabilitySpacePool::Capacity,
                   snapshot.ActiveSlots == CapabilitySpacePool::Capacity, snapshot.FreeSlots == 0U,
                   snapshot.Created == CapabilitySpacePool::Capacity);
}

// A created space shares the boot spaces' transfer domain: a capability moves
// into it, and its handle does not validate in the space it came from.
void TestTransfersReachACreatedSpace(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    CapabilitySpace* const child =
        fixture.Resolve(created.HasValue() ? created.GetValue().Capability : CapabilityHandle{});
    const CapabilityHandle granted =
        fixture.Mint(fixture.DomainObject, CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate));
    const CapabilityTransferRequest request{
        .Source = granted,
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Inspect},
        .ExpectedType = ObjectType::ResourceDomain,
        .Disposition = CapabilityTransferDisposition::Copy,
        .Reserved = 0U,
    };
    const auto transferred =
        (child != nullptr)
            ? fixture.Space.TransferBatchTo(*child, CapabilityTransferRequestView{&request, 1U})
            : decltype(fixture.Space.TransferBatchTo(fixture.Space, CapabilityTransferRequestView{}))::Failure(
                  CapabilityError::InvalidConfiguration);
    const CapabilityHandle copy =
        transferred.HasValue() ? transferred.GetValue().DestinationHandles[0U] : CapabilityHandle{};
    const auto inChild = (child != nullptr)
                             ? child->Query(copy)
                             : decltype(fixture.Space.Query(copy))::Failure(CapabilityError::InvalidHandle);
    const auto inParent = fixture.Space.Query(copy);
    ASTERIA_EXPECT(runner, "a capability copied into a created space resolves there and nowhere else", ready,
                   child != nullptr, transferred.HasValue(), inChild.HasValue(),
                   inChild.HasValue() && (inChild.GetValue().Object.GetValue() == fixture.DomainObject.GetValue()),
                   !inParent.HasValue(), (child != nullptr) && (child->GetActiveCount() == 1U));
}

void TestRejections(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize(Limits(CapabilitySpacePool::GetReservation().KernelMetadataBytes));
    const CreateResult inspectOnly = fixture.Create(fixture.InspectOnly);
    const CreateResult forged =
        fixture.Create(CapabilityHandle{fixture.Authority.GetSelector(), fixture.Authority.GetAuthenticator() ^ 1U});
    const CreateResult first = fixture.Create();
    const CreateResult overCharged = fixture.Create();
    const CreateResult unready =
        fixture.Pool.Create(CapabilitySpaceCreateRequest{.Space = nullptr, .Authority = fixture.Authority});
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a creation without Manage, beyond the charge or without a space leaves nothing behind",
                   ready, IsFailure(inspectOnly, ObjectPoolError::CapabilityRejected),
                   !inspectOnly.HasValue() && (inspectOnly.GetError().Capability == CapabilityError::RightsDenied),
                   IsFailure(forged, ObjectPoolError::CapabilityRejected), first.HasValue(),
                   IsFailure(overCharged, ObjectPoolError::ChargeRejected),
                   IsFailure(unready, ObjectPoolError::InvalidConfiguration), snapshot.ActiveSlots == 1U,
                   snapshot.FreeSlots == (CapabilitySpacePool::Capacity - 1U), snapshot.Created == 1U);
}

// A destroyed space is reclaimed once nothing pins it, and its slot builds a space with a new
// identifier.
void TestDestroyAndCollect(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    CapabilitySpace* const child = fixture.Resolve(handle);
    const auto oldIdentifier = (child != nullptr) ? child->GetIdentifier().GetValue() : 0U;
    const CapabilityHandle granted =
        fixture.Mint(fixture.DomainObject, CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate));
    const CapabilityTransferRequest request{
        .Source = granted,
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Inspect},
        .ExpectedType = ObjectType::ResourceDomain,
        .Disposition = CapabilityTransferDisposition::Copy,
        .Reserved = 0U,
    };
    const bool copied = (child != nullptr) &&
                        fixture.Space.TransferBatchTo(*child, CapabilityTransferRequestView{&request, 1U}).HasValue() &&
                        (child->GetHeldCount() == 0U);
    Kernel::Object::ObjectPin pin;
    const bool pinned =
        created.HasValue() && (fixture.Registry.PinObject(created.GetValue().Space, ObjectType::CapabilitySpace, pin) ==
                               Kernel::Object::ObjectPinError::Ok);

    const ObjectId foundBefore = (child != nullptr) ? fixture.Pool.FindIdentity(*child) : ObjectId{};
    const ObjectPoolFailure wrongType = fixture.Pool.Destroy(fixture.Space, fixture.Authority);
    const ObjectPoolFailure destroyed = fixture.Pool.Destroy(fixture.Space, handle);
    const ObjectId foundAfterDestroy = (child != nullptr) ? fixture.Pool.FindIdentity(*child) : ObjectId{};
    const auto gone = fixture.Space.Lookup(handle, ObjectType::CapabilitySpace, CapabilityOperation::Manage);
    const ObjectPoolFailure again = fixture.Pool.Destroy(fixture.Space, handle);
    const auto whilePinned = fixture.Pool.Collect();
    const auto quiescing = fixture.Pool.GetSnapshot();
    const UInt64 chargedWhilePinned = fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes;
    pin.Reset();
    const auto collected = fixture.Pool.Collect();
    const auto reclaimed = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "only a created, undestroyed space has a pool identity",
                   created.HasValue() && (foundBefore.GetValue() == created.GetValue().Space.GetValue()),
                   !foundAfterDestroy.IsValid(), !fixture.Pool.FindIdentity(fixture.Space).IsValid());
    ASTERIA_EXPECT(runner, "a destroyed space stops resolving and is reclaimed, charge and slot, once unpinned", ready,
                   created.HasValue(), copied, pinned, wrongType.Error == ObjectPoolError::CapabilityRejected,
                   wrongType.Capability == CapabilityError::TypeMismatch, destroyed.Error == ObjectPoolError::None,
                   !gone.HasValue(), again.Error == ObjectPoolError::CapabilityRejected,
                   whilePinned.HasValue() && (whilePinned.GetValue() == 0U), quiescing.QuiescingSlots == 1U,
                   quiescing.ActiveSlots == 0U, quiescing.FreeSlots == (CapabilitySpacePool::Capacity - 1U),
                   quiescing.Destroyed == 1U, quiescing.Reclaimed == 0U,
                   chargedWhilePinned == CapabilitySpacePool::GetReservation().KernelMetadataBytes,
                   collected.HasValue() && (collected.GetValue() == 1U), reclaimed.QuiescingSlots == 0U,
                   reclaimed.FreeSlots == CapabilitySpacePool::Capacity, reclaimed.Reclaimed == 1U,
                   fixture.Domain.GetUsage().Objects[static_cast<Size>(Abi::LimitedObject::CapabilitySpace)] == 0U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    const CreateResult first = fixture.Create();
    const CreateResult second = fixture.Create();
    CapabilitySpace* const reused =
        fixture.Resolve(first.HasValue() ? first.GetValue().Capability : CapabilityHandle{});
    ASTERIA_EXPECT(runner, "a reclaimed slot builds an empty space with a new identifier", first.HasValue(),
                   second.HasValue(), reused != nullptr,
                   (reused != nullptr) && (reused->GetIdentifier().GetValue() != oldIdentifier) &&
                       (reused->GetActiveCount() == 0U),
                   fixture.Pool.GetSnapshot().Created == 3U, fixture.Pool.GetSnapshot().ActiveSlots == 2U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"CapabilitySpacePoolTests"};
    Asteria::Test::TestConfiguration(runner);
    Asteria::Test::TestCreateChargesAndMints(runner);
    Asteria::Test::TestTransfersReachACreatedSpace(runner);
    Asteria::Test::TestRejections(runner);
    Asteria::Test::TestDestroyAndCollect(runner);
    return runner.Finish();
}
