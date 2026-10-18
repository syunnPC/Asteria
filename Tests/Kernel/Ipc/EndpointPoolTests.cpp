// SPDX-License-Identifier: Apache-2.0
#include <memory>

#include "Kernel/Ipc/EndpointPool.hpp"
#include "Kernel/Object/CapabilitySpacePool.hpp"
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
using Kernel::Ipc::CancellationState;
using Kernel::Ipc::CapabilityAttachment;
using Kernel::Ipc::CapabilityTransferMode;
using Kernel::Ipc::Endpoint;
using Kernel::Ipc::EndpointConfiguration;
using Kernel::Ipc::EndpointCreateRequest;
using Kernel::Ipc::EndpointCreation;
using Kernel::Ipc::EndpointPool;
using Kernel::Ipc::EndpointPoolError;
using Kernel::Ipc::EndpointPoolFailure;
using Kernel::Ipc::EndpointPoolState;
using Kernel::Ipc::EndpointSendClass;
using Kernel::Ipc::InterfaceIdentifier;
using Kernel::Ipc::MessageEnvelope;
using Kernel::Ipc::MessageKind;
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
using Kernel::Resource::ResourceDomainUsage;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

using CreateResult = Foundation::Result<EndpointCreation, EndpointPoolFailure>;

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

[[nodiscard]] MessageEnvelope MakeMessage(const UInt64 transaction) noexcept
{
    MessageEnvelope message{};
    message.Interface = InterfaceIdentifier{0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL};
    message.TransactionIdentifier = transaction;
    message.Deadline = MonotonicTime::Infinite();
    message.MethodIdentifier = 7U;
    message.InterfaceVersionMajor = 1U;
    message.Kind = MessageKind::Asynchronous;
    message.Cancellation = CancellationState::None;
    return message;
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
        InspectOnly = Mint(DomainObject, CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Duplicate));
        Revoker = Mint(DomainObject, CapabilityRights{CapabilityRight::Revoke});
        return Authority.IsValid() && InspectOnly.IsValid() && Revoker.IsValid();
    }

    [[nodiscard]] CapabilityHandle Mint(const ObjectId object, const CapabilityRights rights) noexcept
    {
        const auto minted = Issuer.Mint(Space, object, rights, RevocationRoot, MonotonicTime::Infinite(), 0U);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    }

    [[nodiscard]] CreateResult
    Create(const CapabilityHandle authority,
           const EndpointConfiguration configuration = EndpointConfiguration::Default()) noexcept
    {
        return Pool.Create(EndpointCreateRequest{
            .Space = &Space,
            .Authority = authority,
            .Configuration = configuration,
            .Receiver = nullptr,
            .ReceiverObject = {},
            .SealedSelector = 0U,
        });
    }

    [[nodiscard]] CreateResult Create() noexcept
    {
        return Create(Authority);
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
    EndpointPool Pool;
    ObjectId DomainObject{};
    RevocationDomainId RevocationRoot{};
    CapabilityHandle Authority{};
    CapabilityHandle InspectOnly{};
    CapabilityHandle Revoker{};
};

[[nodiscard]] bool IsFailure(const CreateResult& result, const EndpointPoolError error) noexcept
{
    return !result.HasValue() && (result.GetError().Error == error);
}

[[nodiscard]] bool UsageIsZero(const ResourceDomainUsage& usage) noexcept
{
    return (usage.KernelMetadataBytes == 0U) && (usage.IpcQueueBytes == 0U);
}

void TestCreateChargesAndMints(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const auto queried = fixture.Space.Query(handle);
    const ResourceDomainUsage usage = fixture.GetUsage();
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(
        runner, "a created Endpoint is charged and its capability minted under the grant", ready, created.HasValue(),
        queried.HasValue(), queried.HasValue() && (queried.GetValue().Type == ObjectType::Endpoint),
        queried.HasValue() && queried.GetValue().Rights.IsEqualTo(EndpointPool::CreatedRights),
        queried.HasValue() && (queried.GetValue().RevocationDomain.GetValue() == fixture.RevocationRoot.GetValue()),
        queried.HasValue() && queried.GetValue().LeaseDeadline.IsInfinite(),
        queried.HasValue() && (queried.GetValue().ChargeOwner.GetValue() == Charged.GetValue()),
        usage.KernelMetadataBytes == EndpointPool::MetadataBytes,
        usage.IpcQueueBytes == EndpointConfiguration::Default().MaximumQueuedBytes, snapshot.ActiveSlots == 1U,
        snapshot.FreeSlots == (EndpointPool::Capacity - 1U), snapshot.Created == 1U,
        snapshot.State == EndpointPoolState::Ready);

    // The creator's space is the receiver: a message it sends to itself is
    // received there.
    bool sent{false};
    {
        const auto invoke = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
        sent = invoke.HasValue() &&
               static_cast<Endpoint&>(invoke.GetValue().GetObject())
                   .SendFromInvocation(fixture.Space, handle, Charged, EndpointSendClass::Normal, MakeMessage(41U))
                   .HasValue();
    }
    MessageEnvelope received{};
    bool receivedBack{false};
    {
        const auto read = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Read);
        receivedBack =
            read.HasValue() && static_cast<Endpoint&>(read.GetValue().GetObject()).Receive(received).HasValue();
    }
    ASTERIA_EXPECT(runner, "a created Endpoint loops a message back to its creator", sent, receivedBack,
                   received.TransactionIdentifier == 41U);
}

void TestDestroyAndCollect(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    Size beforeRelease{0U};
    bool destroyed{false};
    {
        // A lease taken before the destruction keeps the Endpoint referenced.
        const auto held = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
        destroyed = held.HasValue() && fixture.Pool.Destroy(fixture.Space, handle).HasValue();
        const auto collected = fixture.Pool.Collect();
        beforeRelease = collected.HasValue() ? collected.GetValue() : 99U;
    }
    const auto lookup = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
    const auto quiescing = fixture.Pool.GetSnapshot();
    const auto again = fixture.Pool.Destroy(fixture.Space, handle);
    ASTERIA_EXPECT(runner, "a destroyed Endpoint is unreachable and waits for its last reference", ready,
                   created.HasValue(), destroyed, beforeRelease == 0U,
                   !lookup.HasValue() && (lookup.GetError() == CapabilityError::ObjectNotFound),
                   quiescing.QuiescingSlots == 1U, quiescing.Destroyed == 1U,
                   !again.HasValue() && (again.GetError().Error == EndpointPoolError::CapabilityRejected),
                   !again.HasValue() && (again.GetError().Capability == CapabilityError::ObjectNotFound));

    const auto collected = fixture.Pool.Collect();
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "collection reclaims the slot and returns the charge",
                   collected.HasValue() && (collected.GetValue() == 1U), UsageIsZero(fixture.GetUsage()),
                   snapshot.FreeSlots == EndpointPool::Capacity, snapshot.Reclaimed == 1U,
                   fixture.Space.Close(handle) == CapabilityError::Ok);

    const CreateResult reused = fixture.Create();
    ASTERIA_EXPECT(runner, "a reclaimed slot serves a new creation under a new identity", reused.HasValue(),
                   reused.HasValue() && created.HasValue() &&
                       (reused.GetValue().Endpoint.GetValue() != created.GetValue().Endpoint.GetValue()),
                   fixture.GetUsage().KernelMetadataBytes == EndpointPool::MetadataBytes);
}

[[nodiscard]] bool IsPending(const Kernel::Ipc::Notification* const readiness) noexcept
{
    return (readiness != nullptr) && (readiness->GetSnapshot().Pending != 0U);
}

// What a WaitSet's poll does to a signalled Notification.
[[nodiscard]] bool ConsumeSignal(Kernel::Ipc::Notification* const readiness) noexcept
{
    if (readiness == nullptr)
    {
        return false;
    }
    const auto claimed = readiness->Claim();
    return claimed.HasValue() && readiness->CommitClaim(claimed.GetValue()).HasValue();
}

// Each created Endpoint registers a readiness Notification, signalled when a message is published
// and again when a receive leaves messages queued.
void TestReadiness(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const Size before = fixture.Registry.GetActiveCount();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const auto lease = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
    Endpoint* const endpoint = lease.HasValue() ? &static_cast<Endpoint&>(lease.GetValue().GetObject()) : nullptr;
    Kernel::Ipc::Notification* const readiness = (endpoint != nullptr) ? endpoint->GetReadiness() : nullptr;
    const bool quiet = (readiness != nullptr) && !IsPending(readiness);
    const auto send = [&](const UInt64 transaction) noexcept
    {
        return (endpoint != nullptr) && endpoint
                                            ->SendFromInvocation(fixture.Space, handle, Charged,
                                                                 EndpointSendClass::Normal, MakeMessage(transaction))
                                            .HasValue();
    };
    const auto receive = [&]() noexcept
    {
        MessageEnvelope received{};
        return (endpoint != nullptr) && endpoint->Receive(received).HasValue();
    };
    const bool sent = send(1U) && send(2U);
    const bool signalled = IsPending(readiness);
    const bool claimed = ConsumeSignal(readiness);
    const bool first = receive();
    const bool remaining = IsPending(readiness);
    const bool claimedAgain = ConsumeSignal(readiness);
    const bool second = receive();
    const bool drained = !IsPending(readiness);
    ASTERIA_EXPECT(runner, "a created Endpoint's readiness is signalled by a send and while messages remain", ready,
                   created.HasValue(), quiet, fixture.Registry.GetActiveCount() == (before + 2U),
                   (endpoint != nullptr) && endpoint->GetReadinessId().IsValid() &&
                       endpoint->GetReadinessId().IsFromRegistry(fixture.Registry),
                   sent, signalled, claimed, first, remaining, claimedAgain, second, drained);
}

// A WaitSet registration pins the readiness Notification: the slot is
// reclaimed only once the Endpoint and its Notification are both released.
void TestPinnedReadiness(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const Size before = fixture.Registry.GetActiveCount();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    Kernel::Object::ObjectPin pin{};
    bool pinned{false};
    {
        const auto lease = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Read);
        pinned = lease.HasValue() &&
                 (fixture.Registry.PinObject(
                      static_cast<Endpoint&>(lease.GetValue().GetObject()).GetReadinessId().GetObjectId(),
                      ObjectType::Notification, pin) == Kernel::Object::ObjectPinError::Ok);
    }
    const bool destroyed = fixture.Pool.Destroy(fixture.Space, handle).HasValue();
    const auto first = fixture.Pool.Collect();
    const auto second = fixture.Pool.Collect();
    const auto held = fixture.Pool.GetSnapshot();
    pin.Reset();
    const auto released = fixture.Pool.Collect();
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a pinned readiness Notification holds its Endpoint's slot until it is released", ready,
                   created.HasValue(), pinned, destroyed, first.HasValue() && (first.GetValue() == 0U),
                   second.HasValue() && (second.GetValue() == 0U), held.QuiescingSlots == 1U,
                   held.State == EndpointPoolState::Ready, released.HasValue() && (released.GetValue() == 1U),
                   snapshot.FreeSlots == EndpointPool::Capacity, UsageIsZero(fixture.GetUsage()),
                   fixture.Registry.GetActiveCount() == before, fixture.Space.Close(handle) == CapabilityError::Ok);
}

// The creator's capability carries the requested sealed selector, a copy inherits it, and a
// delivery reports it.
void TestSealedSelector(TestContext& runner) noexcept
{
    constexpr UInt64 Selector{0x5EA1'ED00'0000'0042ULL};
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Pool.Create(EndpointCreateRequest{
        .Space = &fixture.Space,
        .Authority = fixture.Authority,
        .Configuration = EndpointConfiguration::Default(),
        .Receiver = nullptr,
        .ReceiverObject = {},
        .SealedSelector = Selector,
    });
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const auto copy = fixture.Space.Duplicate(
        handle, CapabilityRights{CapabilityRight::Invoke}.With(CapabilityRight::Inspect), MonotonicTime::Infinite());
    const CapabilityHandle sender = copy.HasValue() ? copy.GetValue() : CapabilityHandle{};
    const auto original = fixture.Space.Query(handle);
    const auto inherited = fixture.Space.Query(sender);
    bool sent{false};
    UInt64 delivered{0U};
    {
        const auto invoke = fixture.Space.Lookup(sender, ObjectType::Endpoint, CapabilityOperation::Invoke);
        sent = invoke.HasValue() &&
               static_cast<Endpoint&>(invoke.GetValue().GetObject())
                   .SendFromInvocation(fixture.Space, sender, Charged, EndpointSendClass::Normal, MakeMessage(5U))
                   .HasValue();
        const auto read = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Read);
        MessageEnvelope received{};
        const auto metadata =
            read.HasValue()
                ? static_cast<Endpoint&>(read.GetValue().GetObject()).Receive(received)
                : Foundation::Result<Kernel::Ipc::EndpointReceiveMetadata, Kernel::Ipc::EndpointError>::Failure({});
        delivered = metadata.HasValue() ? metadata.GetValue().SenderEndpointSealedObjectSelector : 0U;
    }
    const CreateResult plain = fixture.Create();
    const auto unsealed = fixture.Space.Query(plain.HasValue() ? plain.GetValue().Capability : CapabilityHandle{});
    ASTERIA_EXPECT(runner, "a created Endpoint's capability is sealed as requested, and its copies and deliveries too",
                   ready, created.HasValue(), copy.HasValue(),
                   original.HasValue() && (original.GetValue().SealedObjectSelector == Selector),
                   inherited.HasValue() && (inherited.GetValue().SealedObjectSelector == Selector), sent,
                   delivered == Selector, unsealed.HasValue() && (unsealed.GetValue().SealedObjectSelector == 0U));
}

void TestCollectDiscardsQueuedCapabilities(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const CapabilityHandle moved =
        fixture.Mint(fixture.DomainObject, CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Transfer));
    MessageEnvelope message = MakeMessage(42U);
    message.Attachments[0U] = CapabilityAttachment{
        .Handle = moved,
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Inspect},
        .ExpectedType = ObjectType::ResourceDomain,
        .TransferMode = CapabilityTransferMode::Move,
        .Reserved = 0U,
    };
    message.AttachmentCount = 1U;
    bool sent{false};
    {
        const auto invoke = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
        sent = invoke.HasValue() &&
               static_cast<Endpoint&>(invoke.GetValue().GetObject())
                   .SendFromInvocation(fixture.Space, handle, Charged, EndpointSendClass::Normal, message)
                   .HasValue();
    }
    const Size queued = fixture.Space.GetActiveCount();
    const bool destroyed = fixture.Pool.Destroy(fixture.Space, handle).HasValue();
    const auto collected = fixture.Pool.Collect();
    ASTERIA_EXPECT(runner, "collecting an Endpoint closes the capabilities its unread messages carry", ready,
                   created.HasValue(), moved.IsValid(), sent, destroyed,
                   collected.HasValue() && (collected.GetValue() == 1U),
                   fixture.Space.GetActiveCount() == (queued - 1U), UsageIsZero(fixture.GetUsage()));
}

// An Endpoint created for another space receives there, so a capability its creator sends moves
// into that space.
void TestCreatedReceiverSpace(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    CapabilitySpace child{fixture.Registry, fixture.Revocations, fixture.Clock, 0x1357'9BDF'2468'ACE0ULL,
                          0x0ECA'8642'FDB9'7531ULL};
    Kernel::Object::CapabilitySpaceObject childObject{child, Charged};
    const bool ready = fixture.Initialize();
    const auto childRegistered = fixture.Issuer.CreateObject(childObject);
    const CreateResult created =
        childRegistered.HasValue()
            ? fixture.Pool.Create(EndpointCreateRequest{
                  .Space = &fixture.Space,
                  .Authority = fixture.Authority,
                  .Configuration = EndpointConfiguration::Default(),
                  .Receiver = &child,
                  .ReceiverObject = childRegistered.GetValue(),
                  .SealedSelector = 0U,
              })
            : CreateResult::Failure(Kernel::Object::MakeObjectPoolFailure(EndpointPoolError::InvalidConfiguration));
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const CapabilityHandle moved =
        fixture.Mint(fixture.DomainObject, CapabilityRights{CapabilityRight::Inspect}.With(CapabilityRight::Transfer));
    MessageEnvelope message = MakeMessage(43U);
    message.Attachments[0U] = CapabilityAttachment{
        .Handle = moved,
        .RequestedDeadline = MonotonicTime::Infinite(),
        .RequestedRights = CapabilityRights{CapabilityRight::Inspect},
        .ExpectedType = ObjectType::ResourceDomain,
        .TransferMode = CapabilityTransferMode::Move,
        .Reserved = 0U,
    };
    message.AttachmentCount = 1U;
    bool sent{false};
    {
        const auto invoke = fixture.Space.Lookup(handle, ObjectType::Endpoint, CapabilityOperation::Invoke);
        sent = invoke.HasValue() &&
               static_cast<Endpoint&>(invoke.GetValue().GetObject())
                   .SendFromInvocation(fixture.Space, handle, Charged, EndpointSendClass::Normal, message)
                   .HasValue();
    }
    const auto unpinned =
        fixture.Issuer.DestroyObject(childRegistered.HasValue() ? childRegistered.GetValue() : ObjectId{});
    const auto pinned =
        fixture.Issuer.CollectDestroyedObject(childRegistered.HasValue() ? childRegistered.GetValue() : ObjectId{});
    ASTERIA_EXPECT(runner, "an Endpoint receiving in a created space moves a sent capability there and pins it", ready,
                   childRegistered.HasValue(), created.HasValue(), sent, child.GetActiveCount() == 1U,
                   !fixture.Space.Query(moved).HasValue(), unpinned == Kernel::Object::ObjectRegistryError::Ok,
                   !pinned.HasValue());
}

void TestCreateFailuresLeaveNothing(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const Size registered = fixture.Registry.GetActiveCount();
    const Size handles = fixture.Space.GetActiveCount();
    const CreateResult unauthorized = fixture.Create(fixture.InspectOnly);
    EndpointConfiguration invalid = EndpointConfiguration::Default();
    invalid.MaximumQueuedMessages = 0U;
    const CreateResult misconfigured = fixture.Create(fixture.Authority, invalid);
    const CreateResult forged = fixture.Create(CapabilityHandle{fixture.Authority.GetSelector(), 1U});
    ASTERIA_EXPECT(runner, "a creation needs Manage on a ResourceDomain and a valid configuration", ready,
                   IsFailure(unauthorized, EndpointPoolError::CapabilityRejected),
                   !unauthorized.HasValue() && (unauthorized.GetError().Capability == CapabilityError::RightsDenied),
                   IsFailure(misconfigured, EndpointPoolError::InvalidRequest),
                   IsFailure(forged, EndpointPoolError::CapabilityRejected),
                   !forged.HasValue() && (forged.GetError().Capability == CapabilityError::InvalidHandle),
                   UsageIsZero(fixture.GetUsage()), fixture.Registry.GetActiveCount() == registered,
                   fixture.Space.GetActiveCount() == handles,
                   fixture.Pool.GetSnapshot().FreeSlots == EndpointPool::Capacity);

    // An Endpoint capability is no grant.
    const CreateResult first = fixture.Create();
    const CreateResult wrongType = fixture.Create(first.HasValue() ? first.GetValue().Capability : CapabilityHandle{});
    ASTERIA_EXPECT(runner, "an Endpoint capability does not authorize a creation", first.HasValue(),
                   IsFailure(wrongType, EndpointPoolError::CapabilityRejected),
                   !wrongType.HasValue() && (wrongType.GetError().Capability == CapabilityError::TypeMismatch));

    // The pool bounds creations; a full pool leaves the charge unchanged.
    bool filled{first.HasValue()};
    for (Size other{1U}; other < EndpointPool::Capacity; ++other)
    {
        filled = filled && fixture.Create().HasValue();
    }
    const ResourceDomainUsage full = fixture.GetUsage();
    const CreateResult exhausted = fixture.Create();
    ASTERIA_EXPECT(runner, "a full pool refuses a creation without charging", filled,
                   IsFailure(exhausted, EndpointPoolError::CapacityExceeded),
                   full.KernelMetadataBytes == (EndpointPool::Capacity * EndpointPool::MetadataBytes),
                   fixture.GetUsage().KernelMetadataBytes == full.KernelMetadataBytes);

    // Revoking the grant's domain ends the grant and everything created under it.
    const bool revoked = fixture.Space.Revoke(fixture.Revoker) == CapabilityError::Ok;
    const CreateResult afterRevoke = fixture.Create();
    const auto revokedLookup = fixture.Space.Lookup(first.HasValue() ? first.GetValue().Capability : CapabilityHandle{},
                                                    ObjectType::Endpoint, CapabilityOperation::Invoke);
    ASTERIA_EXPECT(runner, "a revoked grant creates nothing and its creations report Revoked", revoked,
                   IsFailure(afterRevoke, EndpointPoolError::CapabilityRejected),
                   !afterRevoke.HasValue() && (afterRevoke.GetError().Capability == CapabilityError::Revoked),
                   !revokedLookup.HasValue() && (revokedLookup.GetError() == CapabilityError::Revoked));
}

[[gnu::noinline]] void TestChargeLimit(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto ownedLimited = std::make_unique<PoolFixture>();
    PoolFixture& limited = *ownedLimited;
    const bool limitedReady = limited.Initialize(Limits(EndpointPool::MetadataBytes - 1U));
    const Size limitedRegistered = limited.Registry.GetActiveCount();
    const CreateResult overLimit = limited.Create();
    ASTERIA_EXPECT(runner, "a domain limit refuses the charge and leaves no object", limitedReady,
                   IsFailure(overLimit, EndpointPoolError::ChargeRejected),
                   !overLimit.HasValue() && (overLimit.GetError().Resource == ResourceDomainError::LimitExceeded),
                   limited.Registry.GetActiveCount() == limitedRegistered,
                   limited.Pool.GetSnapshot().FreeSlots == EndpointPool::Capacity);
}

void TestChargeAndMintRollback(TestContext& runner) noexcept
{
    TestChargeLimit(runner);

    // Fill the creator's space so the mint fails after the charge and the
    // registration; both roll back.
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    bool full{false};
    for (Size index{0U}; (index < CapabilitySpace::MaximumSlots) && !full; ++index)
    {
        const auto duplicated = fixture.Space.Duplicate(fixture.InspectOnly, CapabilityRights{CapabilityRight::Inspect},
                                                        MonotonicTime::Infinite());
        full = !duplicated.HasValue() && (duplicated.GetError() == CapabilityError::SpaceExhausted);
    }
    const Size registered = fixture.Registry.GetActiveCount();
    const CreateResult unminted = fixture.Create();
    const auto snapshot = fixture.Pool.GetSnapshot();
    ASTERIA_EXPECT(runner, "a failed mint rolls the registration and the charge back", ready, full,
                   IsFailure(unminted, EndpointPoolError::MintRejected),
                   !unminted.HasValue() && (unminted.GetError().Capability == CapabilityError::SpaceExhausted),
                   fixture.Registry.GetActiveCount() == registered, UsageIsZero(fixture.GetUsage()),
                   snapshot.FreeSlots == EndpointPool::Capacity, snapshot.Created == 0U,
                   snapshot.State == EndpointPoolState::Ready);
}

void TestDestroyAuthority(TestContext& runner) noexcept
{
    // The pool is too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const CreateResult created = fixture.Create();
    const CapabilityHandle handle = created.HasValue() ? created.GetValue().Capability : CapabilityHandle{};
    const auto attenuated =
        fixture.Space.Duplicate(handle, CapabilityRights{CapabilityRight::Invoke}, MonotonicTime::Infinite());
    const auto withoutManage =
        fixture.Pool.Destroy(fixture.Space, attenuated.HasValue() ? attenuated.GetValue() : CapabilityHandle{});

    // An Endpoint the pool did not create is not the pool's to destroy, even
    // through a capability with Manage.
    Endpoint foreign{fixture.Space, Charged};
    const auto foreignObject = fixture.Issuer.CreateObject(foreign);
    const CapabilityHandle foreignHandle = fixture.Mint(
        foreignObject.HasValue() ? foreignObject.GetValue() : ObjectId{}, CapabilityRights{CapabilityRight::Manage});
    const auto notOwned = fixture.Pool.Destroy(fixture.Space, foreignHandle);
    ASTERIA_EXPECT(
        runner, "a destruction needs Manage on an Endpoint the pool owns", ready, created.HasValue(),
        attenuated.HasValue(),
        !withoutManage.HasValue() && (withoutManage.GetError().Error == EndpointPoolError::CapabilityRejected),
        !withoutManage.HasValue() && (withoutManage.GetError().Capability == CapabilityError::RightsDenied),
        foreignHandle.IsValid(), !notOwned.HasValue() && (notOwned.GetError().Error == EndpointPoolError::NotOwned),
        fixture.Pool.GetSnapshot().ActiveSlots == 1U);
    if (foreignObject.HasValue())
    {
        static_cast<void>(fixture.Issuer.DestroyObject(foreignObject.GetValue()));
        static_cast<void>(fixture.Issuer.CollectDestroyedObject(foreignObject.GetValue()));
    }
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"EndpointPoolTests"};
    Asteria::Test::TestCreateChargesAndMints(runner);
    Asteria::Test::TestDestroyAndCollect(runner);
    Asteria::Test::TestCollectDiscardsQueuedCapabilities(runner);
    Asteria::Test::TestCreatedReceiverSpace(runner);
    Asteria::Test::TestCreateFailuresLeaveNothing(runner);
    Asteria::Test::TestChargeAndMintRollback(runner);
    Asteria::Test::TestDestroyAuthority(runner);
    Asteria::Test::TestReadiness(runner);
    Asteria::Test::TestPinnedReadiness(runner);
    Asteria::Test::TestSealedSelector(runner);
    return runner.Finish();
}
