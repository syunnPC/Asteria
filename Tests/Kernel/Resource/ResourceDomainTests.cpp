// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>

#include "Kernel/Object/RootCapabilityIssuer.hpp"
#include "Kernel/Resource/ResourceDomain.hpp"

#include "Support/ObjectRegistryFixture.hpp"
#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt64;
using Kernel::Object::IKernelObject;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
using Kernel::Object::ObjectType;
using Kernel::Object::ResourceDomainId;
using Kernel::Object::RootCapabilityIssuer;
using Kernel::Resource::MaximumResourceDomainDepth;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Kernel::Resource::ResourceDomainObjectId;
using Kernel::Resource::ResourceDomainState;
using Kernel::Resource::ResourceDomainUsage;
using Kernel::Resource::ResourceReservation;
using Platform::Synchronization::SpinLock;

class TestKernelObject final : public IKernelObject
{
public:
    constexpr TestKernelObject(const ObjectType type, const ResourceDomainId owner) noexcept
        : m_type{type}, m_owner{owner}
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

class RegistryFixture final
{
public:
    RegistryFixture() noexcept : Registry{Synchronization}, Issuer{Registry}
    {
    }

    SpinLock Synchronization;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RootCapabilityIssuer Issuer;
};

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits(const UInt64 value) noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = value,
        .ResidentMemoryPages = value,
        .KernelMetadataBytes = value,
        .CapabilityHandles = value,
        .IpcQueueBytes = value,
        .Threads = value,
        .ChildDomains = value,
    };
}

[[nodiscard]] constexpr ResourceReservation MakeReservation(const UInt64 committed, const UInt64 resident,
                                                            const UInt64 metadata, const UInt64 handles,
                                                            const UInt64 ipcBytes, const UInt64 threads) noexcept
{
    return ResourceReservation{
        .CommittedMemoryPages = committed,
        .ResidentMemoryPages = resident,
        .KernelMetadataBytes = metadata,
        .CapabilityHandles = handles,
        .IpcQueueBytes = ipcBytes,
        .Threads = threads,
    };
}

[[nodiscard]] constexpr bool UsageIs(const ResourceDomainUsage& usage, const ResourceReservation& expected,
                                     const UInt64 children) noexcept
{
    return (usage.CommittedMemoryPages == expected.CommittedMemoryPages) &&
           (usage.ResidentMemoryPages == expected.ResidentMemoryPages) &&
           (usage.KernelMetadataBytes == expected.KernelMetadataBytes) &&
           (usage.CapabilityHandles == expected.CapabilityHandles) && (usage.IpcQueueBytes == expected.IpcQueueBytes) &&
           (usage.Threads == expected.Threads) && (usage.ChildDomains == children);
}

[[nodiscard]] bool DestroyAndCollect(RootCapabilityIssuer& issuer, const ObjectId object,
                                     IKernelObject& expected) noexcept
{
    if (issuer.DestroyObject(object) != ObjectRegistryError::Ok)
    {
        return false;
    }
    const auto collected = issuer.CollectDestroyedObject();
    return collected.HasValue() && (collected.GetValue().Object.GetValue() == object.GetValue()) &&
           (collected.GetValue().Instance == &expected);
}

void TestRootReservations(TestContext& runner) noexcept
{
    ResourceDomainLedger ledger;
    ResourceDomain invalid{ledger};
    ResourceDomainLimits malformed = MakeLimits(10U);
    malformed.CommittedMemoryPages = 4U;
    malformed.ResidentMemoryPages = 5U;
    ASTERIA_EXPECT(
        runner, "invalid root configuration is failure-atomic",
        invalid.InitializeRoot(ResourceDomainId{}, MakeLimits(10U)).Error == ResourceDomainError::InvalidConfiguration,
        invalid.InitializeRoot(ResourceDomainId{1U}, malformed).Error == ResourceDomainError::InvalidConfiguration,
        invalid.GetSnapshot().State == ResourceDomainState::Uninitialized);

    ResourceDomain root{ledger};
    ResourceDomainLimits limits = MakeLimits(100U);
    limits.CapabilityHandles = 4U;
    const ResourceReservation reservation = MakeReservation(4U, 2U, 20U, 2U, 16U, 1U);
    const bool initialized = root.InitializeRoot(ResourceDomainId{7U}, limits).Error == ResourceDomainError::None;
    const bool reserved = root.TryReserve(reservation) == ResourceDomainError::None;
    const auto reservedSnapshot = root.GetSnapshot();
    ASTERIA_EXPECT(runner, "a root publishes and reserves every reclaimable resource together", initialized,
                   root.IsReady(), root.GetIdentifier().GetValue() == 7U, root.GetChargeOwner().GetValue() == 7U,
                   root.GetObjectType() == ObjectType::ResourceDomain, reserved,
                   UsageIs(reservedSnapshot.Usage, reservation, 0U));

    const ResourceReservation zero{};
    const ResourceReservation overLimit = MakeReservation(0U, 0U, 0U, 3U, 0U, 0U);
    const ResourceReservation invalidRelease = MakeReservation(4U, 0U, 0U, 0U, 0U, 0U);
    ASTERIA_EXPECT(runner, "rejected reserve and release operations preserve every counter",
                   root.TryReserve(zero) == ResourceDomainError::InvalidReservation,
                   root.TryReserve(overLimit) == ResourceDomainError::LimitExceeded,
                   root.Release(invalidRelease) == ResourceDomainError::ReservationUnderflow,
                   UsageIs(root.GetSnapshot().Usage, reservation, 0U));

    ASTERIA_EXPECT(runner, "Quiesce requires an empty domain and is terminal",
                   root.Quiesce() == ResourceDomainError::InUse, root.Release(reservation) == ResourceDomainError::None,
                   root.Quiesce() == ResourceDomainError::None, root.Quiesce() == ResourceDomainError::Quiesced,
                   root.GetSnapshot().State == ResourceDomainState::Quiesced);

    ResourceDomain overflow{ledger};
    constexpr UInt64 maximum{~UInt64{0U}};
    const ResourceReservation maximumMetadata = MakeReservation(0U, 0U, maximum, 0U, 0U, 0U);
    const ResourceReservation oneMetadata = MakeReservation(0U, 0U, 1U, 0U, 0U, 0U);
    ASTERIA_EXPECT(runner, "counter arithmetic rejects the UInt64 wrap boundary",
                   overflow.InitializeRoot(ResourceDomainId{8U}, MakeLimits(maximum)).Error ==
                       ResourceDomainError::None,
                   overflow.TryReserve(maximumMetadata) == ResourceDomainError::None,
                   overflow.TryReserve(oneMetadata) == ResourceDomainError::ArithmeticOverflow,
                   overflow.Release(maximumMetadata) == ResourceDomainError::None,
                   overflow.Quiesce() == ResourceDomainError::None);
}

void TestHierarchyAndAggregateLimits(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    ResourceDomainLedger ledger;
    ResourceDomain root{ledger};
    ResourceDomain child{ledger};
    ResourceDomain grandchild{ledger};
    ResourceDomain sibling{ledger};
    ResourceDomainLimits rootLimits = MakeLimits(100U);
    rootLimits.CapabilityHandles = 3U;
    rootLimits.ChildDomains = 3U;
    ResourceDomainLimits childLimits = rootLimits;
    childLimits.ChildDomains = 2U;
    ResourceDomainLimits leafLimits = childLimits;
    leafLimits.ChildDomains = 0U;

    const bool rootInitialized =
        root.InitializeRoot(ResourceDomainId{10U}, rootLimits).Error == ResourceDomainError::None;
    const auto rootObject = fixture.Issuer.CreateObject(root);
    const ResourceDomainObjectId rootIdentity = rootObject.HasValue()
                                                    ? ResourceDomainObjectId{fixture.Registry, rootObject.GetValue()}
                                                    : ResourceDomainObjectId{};
    const bool childInitialized =
        child.InitializeChild(fixture.Registry, ResourceDomainId{11U}, rootIdentity, root, childLimits).Error ==
        ResourceDomainError::None;
    const auto childObject = fixture.Issuer.CreateObject(child);
    const ResourceDomainObjectId childIdentity = childObject.HasValue()
                                                     ? ResourceDomainObjectId{fixture.Registry, childObject.GetValue()}
                                                     : ResourceDomainObjectId{};
    const bool grandchildInitialized =
        grandchild.InitializeChild(fixture.Registry, ResourceDomainId{12U}, childIdentity, child, leafLimits).Error ==
        ResourceDomainError::None;
    const auto grandchildObject = fixture.Issuer.CreateObject(grandchild);
    const bool siblingInitialized =
        sibling.InitializeChild(fixture.Registry, ResourceDomainId{13U}, rootIdentity, root, leafLimits).Error ==
        ResourceDomainError::None;
    const auto siblingObject = fixture.Issuer.CreateObject(sibling);

    const ResourceReservation twoHandles = MakeReservation(0U, 0U, 0U, 2U, 0U, 0U);
    const bool grandchildReserved = grandchild.TryReserve(twoHandles) == ResourceDomainError::None;
    const ResourceReservation twoHandlesSnapshot = MakeReservation(0U, 0U, 0U, 2U, 0U, 0U);
    ASTERIA_EXPECT(runner, "a descendant reservation is charged once to every ancestor", rootInitialized,
                   rootObject.HasValue(), childInitialized, childObject.HasValue(), grandchildInitialized,
                   grandchildObject.HasValue(), siblingInitialized, siblingObject.HasValue(), grandchildReserved,
                   UsageIs(root.GetSnapshot().Usage, twoHandlesSnapshot, 3U),
                   UsageIs(child.GetSnapshot().Usage, twoHandlesSnapshot, 1U),
                   UsageIs(grandchild.GetSnapshot().Usage, twoHandlesSnapshot, 0U),
                   UsageIs(sibling.GetSnapshot().Usage, ResourceReservation{}, 0U));

    const ResourceReservation competing = MakeReservation(0U, 0U, 0U, 2U, 0U, 0U);
    ASTERIA_EXPECT(runner, "an ancestor limit rejects the whole descendant transaction",
                   sibling.TryReserve(competing) == ResourceDomainError::LimitExceeded,
                   UsageIs(root.GetSnapshot().Usage, twoHandlesSnapshot, 3U),
                   UsageIs(sibling.GetSnapshot().Usage, ResourceReservation{}, 0U),
                   root.Quiesce() == ResourceDomainError::InUse, child.Quiesce() == ResourceDomainError::InUse);

    const bool released = grandchild.Release(twoHandles) == ResourceDomainError::None;
    const bool grandchildQuiesced = grandchild.Quiesce() == ResourceDomainError::None;
    const bool grandchildCollected =
        grandchildObject.HasValue() && DestroyAndCollect(fixture.Issuer, grandchildObject.GetValue(), grandchild);
    const bool childQuiesced = child.Quiesce() == ResourceDomainError::None;
    const bool childCollected =
        childObject.HasValue() && DestroyAndCollect(fixture.Issuer, childObject.GetValue(), child);
    const bool siblingQuiesced = sibling.Quiesce() == ResourceDomainError::None;
    const bool siblingCollected =
        siblingObject.HasValue() && DestroyAndCollect(fixture.Issuer, siblingObject.GetValue(), sibling);
    const bool rootQuiesced = root.Quiesce() == ResourceDomainError::None;
    const bool rootCollected = rootObject.HasValue() && DestroyAndCollect(fixture.Issuer, rootObject.GetValue(), root);
    ASTERIA_EXPECT(runner, "reverse hierarchy teardown releases every parent pin and child charge", released,
                   grandchildQuiesced, grandchildCollected, childQuiesced, childCollected, siblingQuiesced,
                   siblingCollected, rootQuiesced, rootCollected);
}

void TestParentValidation(TestContext& runner) noexcept
{
    RegistryFixture local;
    RegistryFixture foreign;
    ResourceDomainLedger ledger;
    ResourceDomainLedger otherLedger;
    ResourceDomain first{ledger};
    ResourceDomain second{ledger};
    ResourceDomain otherTree{otherLedger};
    ResourceDomain candidate{ledger};
    TestKernelObject wrongType{ObjectType::Thread, ResourceDomainId{1U}};
    const ResourceDomainLimits limits = MakeLimits(8U);
    static_cast<void>(first.InitializeRoot(ResourceDomainId{1U}, limits));
    static_cast<void>(second.InitializeRoot(ResourceDomainId{2U}, limits));
    static_cast<void>(otherTree.InitializeRoot(ResourceDomainId{3U}, limits));
    const auto firstObject = local.Issuer.CreateObject(first);
    const auto secondObject = local.Issuer.CreateObject(second);
    const auto otherObject = local.Issuer.CreateObject(otherTree);
    const auto wrongObject = local.Issuer.CreateObject(wrongType);
    const ResourceDomainObjectId firstIdentity = firstObject.HasValue()
                                                     ? ResourceDomainObjectId{local.Registry, firstObject.GetValue()}
                                                     : ResourceDomainObjectId{};
    const ResourceDomainObjectId otherIdentity = otherObject.HasValue()
                                                     ? ResourceDomainObjectId{local.Registry, otherObject.GetValue()}
                                                     : ResourceDomainObjectId{};
    const ResourceDomainObjectId wrongIdentity = wrongObject.HasValue()
                                                     ? ResourceDomainObjectId{local.Registry, wrongObject.GetValue()}
                                                     : ResourceDomainObjectId{};

    const auto foreignIdentity = firstObject.HasValue()
                                     ? ResourceDomainObjectId{foreign.Registry, firstObject.GetValue()}
                                     : ResourceDomainObjectId{};
    const auto foreignResult =
        candidate.InitializeChild(local.Registry, ResourceDomainId{4U}, foreignIdentity, first, limits);
    const auto wrongTypeResult =
        candidate.InitializeChild(local.Registry, ResourceDomainId{4U}, wrongIdentity, first, limits);
    const auto wrongInstanceResult =
        candidate.InitializeChild(local.Registry, ResourceDomainId{4U}, firstIdentity, second, limits);
    const auto wrongLedgerResult =
        candidate.InitializeChild(local.Registry, ResourceDomainId{4U}, otherIdentity, otherTree, limits);
    ASTERIA_EXPECT(runner, "parent provenance, type, instance, and ledger are all exact", firstObject.HasValue(),
                   secondObject.HasValue(), otherObject.HasValue(), wrongObject.HasValue(),
                   foreignResult.Error == ResourceDomainError::InvalidConfiguration,
                   wrongTypeResult.Error == ResourceDomainError::ParentPinRejected,
                   wrongTypeResult.Pin == ObjectPinError::TypeMismatch,
                   wrongInstanceResult.Error == ResourceDomainError::ParentInstanceMismatch,
                   wrongLedgerResult.Error == ResourceDomainError::ParentNotReady,
                   candidate.GetSnapshot().State == ResourceDomainState::Uninitialized);

    const bool cleaned = (first.Quiesce() == ResourceDomainError::None) &&
                         (second.Quiesce() == ResourceDomainError::None) &&
                         (otherTree.Quiesce() == ResourceDomainError::None) && firstObject.HasValue() &&
                         secondObject.HasValue() && otherObject.HasValue() && wrongObject.HasValue() &&
                         DestroyAndCollect(local.Issuer, firstObject.GetValue(), first) &&
                         DestroyAndCollect(local.Issuer, secondObject.GetValue(), second) &&
                         DestroyAndCollect(local.Issuer, otherObject.GetValue(), otherTree) &&
                         DestroyAndCollect(local.Issuer, wrongObject.GetValue(), wrongType);
    ASTERIA_EXPECT(runner, "rejected parent candidates retain no pin or quota", cleaned);
}

class DomainStorage final
{
public:
    explicit constexpr DomainStorage(ResourceDomainLedger& ledger) noexcept : Domain{ledger}
    {
    }

    ResourceDomain Domain;
};

void TestDepthBound(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    ResourceDomainLedger ledger;
    DomainStorage domains[10]{
        DomainStorage{ledger}, DomainStorage{ledger}, DomainStorage{ledger}, DomainStorage{ledger},
        DomainStorage{ledger}, DomainStorage{ledger}, DomainStorage{ledger}, DomainStorage{ledger},
        DomainStorage{ledger}, DomainStorage{ledger},
    };
    constexpr UInt64 maximum{~UInt64{0U}};
    const ResourceDomainLimits limits = MakeLimits(maximum);
    ObjectId objects[9]{};
    bool exact = domains[0U].Domain.InitializeRoot(ResourceDomainId{100U}, limits).Error == ResourceDomainError::None;
    if (exact)
    {
        const auto object = fixture.Issuer.CreateObject(domains[0U].Domain);
        exact = object.HasValue();
        if (exact)
        {
            objects[0U] = object.GetValue();
        }
    }
    for (Size index{1U}; (index < 9U) && exact; ++index)
    {
        const ResourceDomainObjectId parent{fixture.Registry, objects[index - 1U]};
        exact = domains[index]
                    .Domain
                    .InitializeChild(fixture.Registry, ResourceDomainId{100U + index}, parent,
                                     domains[index - 1U].Domain, limits)
                    .Error == ResourceDomainError::None;
        if (exact)
        {
            const auto object = fixture.Issuer.CreateObject(domains[index].Domain);
            exact = object.HasValue();
            if (exact)
            {
                objects[index] = object.GetValue();
            }
        }
    }
    const ResourceDomainObjectId deepest{fixture.Registry, objects[8U]};
    const auto overflow = domains[9U].Domain.InitializeChild(fixture.Registry, ResourceDomainId{109U}, deepest,
                                                             domains[8U].Domain, limits);
    exact = exact && (domains[8U].Domain.GetSnapshot().Depth == MaximumResourceDomainDepth) &&
            (overflow.Error == ResourceDomainError::DepthExceeded) &&
            (domains[9U].Domain.GetSnapshot().State == ResourceDomainState::Uninitialized);

    for (Size remaining{9U}; remaining != 0U; --remaining)
    {
        const Size index = remaining - 1U;
        exact = exact && (domains[index].Domain.Quiesce() == ResourceDomainError::None) &&
                DestroyAndCollect(fixture.Issuer, objects[index], domains[index].Domain);
    }
    ASTERIA_EXPECT(runner, "the hierarchy admits exactly eight parent edges", exact);
}

void TestConcurrentReservations(TestContext& runner) noexcept
{
    ResourceDomainLedger ledger;
    ResourceDomain root{ledger};
    ResourceDomainLimits limits = MakeLimits(4U);
    limits.CommittedMemoryPages = 0U;
    limits.ResidentMemoryPages = 0U;
    limits.KernelMetadataBytes = 0U;
    limits.IpcQueueBytes = 0U;
    limits.Threads = 0U;
    limits.ChildDomains = 0U;
    const bool initialized = root.InitializeRoot(ResourceDomainId{200U}, limits).Error == ResourceDomainError::None;
    const ResourceReservation oneHandle = MakeReservation(0U, 0U, 0U, 1U, 0U, 0U);
    std::atomic<bool> start{false};
    std::atomic<Size> ready{0U};
    std::atomic<UInt64> completed{0U};
    std::atomic<bool> exact{true};
    const auto worker = [&]() noexcept
    {
        static_cast<void>(ready.fetch_add(1U, std::memory_order_release));
        bool started{false};
        for (Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (start.load(std::memory_order_acquire))
            {
                started = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!started)
        {
            exact.store(false, std::memory_order_relaxed);
            return;
        }
        for (Size iteration{0U}; iteration < 1'000U; ++iteration)
        {
            if ((root.TryReserve(oneHandle) != ResourceDomainError::None) ||
                (root.Release(oneHandle) != ResourceDomainError::None))
            {
                exact.store(false, std::memory_order_relaxed);
                return;
            }
            static_cast<void>(completed.fetch_add(1U, std::memory_order_relaxed));
        }
    };
    std::thread workers[4]{
        std::thread{worker},
        std::thread{worker},
        std::thread{worker},
        std::thread{worker},
    };
    bool allReady{false};
    for (Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (ready.load(std::memory_order_acquire) == 4U)
        {
            allReady = true;
            break;
        }
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    for (std::thread& workerThread : workers)
    {
        workerThread.join();
    }
    ASTERIA_EXPECT(runner, "the shared ledger serializes concurrent reserve and release pairs", initialized, allReady,
                   exact.load(std::memory_order_acquire), completed.load(std::memory_order_acquire) == 4'000U,
                   UsageIs(root.GetSnapshot().Usage, ResourceReservation{}, 0U),
                   root.Quiesce() == ResourceDomainError::None);
}
// Once a drain marks its root Draining, nothing new is charged to it or below it and no child is
// created there, while releases and the quiesces still run; the domains above are unaffected.
void TestDraining(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    ResourceDomainLedger ledger;
    ResourceDomain root{ledger};
    ResourceDomain child{ledger};
    ResourceDomain grandchild{ledger};
    ResourceDomain late{ledger};
    ResourceDomainLimits limits = MakeLimits(100U);
    limits.ChildDomains = 3U;
    const bool rootReady = root.InitializeRoot(ResourceDomainId{20U}, limits).Error == ResourceDomainError::None;
    const auto rootObject = fixture.Issuer.CreateObject(root);
    const bool childReady =
        rootObject.HasValue() &&
        (child
             .InitializeChild(fixture.Registry, ResourceDomainId{21U},
                              ResourceDomainObjectId{fixture.Registry, rootObject.GetValue()}, root, limits)
             .Error == ResourceDomainError::None);
    const auto childObject = fixture.Issuer.CreateObject(child);
    const bool grandchildReady =
        childObject.HasValue() &&
        (grandchild
             .InitializeChild(fixture.Registry, ResourceDomainId{22U},
                              ResourceDomainObjectId{fixture.Registry, childObject.GetValue()}, child, limits)
             .Error == ResourceDomainError::None);
    const ResourceReservation charge = MakeReservation(1U, 1U, 8U, 0U, 0U, 0U);
    const bool charged = grandchild.TryReserve(charge) == ResourceDomainError::None;

    const ResourceDomainError marked = child.BeginDrain();
    const ResourceDomainError again = child.BeginDrain();
    const ResourceDomainError belowRefused = grandchild.TryReserve(charge);
    const ResourceDomainError rootRefused = child.TryReserve(charge);
    const ResourceDomainError aboveAllowed = root.TryReserve(charge);
    const bool aboveReleased = root.Release(charge) == ResourceDomainError::None;
    const ResourceDomainError childRefused =
        childObject.HasValue()
            ? late.InitializeChild(fixture.Registry, ResourceDomainId{23U},
                                   ResourceDomainObjectId{fixture.Registry, childObject.GetValue()}, child, limits)
                  .Error
            : ResourceDomainError::InvariantViolation;
    const bool identified = child.GetIdentifier().GetValue() == 21U;
    const bool notReady = !child.IsReady();
    const ResourceDomainError released = grandchild.Release(charge);
    const ResourceDomainError grandchildQuiesced = grandchild.Quiesce();
    const ResourceDomainError childQuiesced = child.Quiesce();
    ASTERIA_EXPECT(runner, "a draining domain refuses new charges and children below it and still releases", rootReady,
                   childReady, grandchildReady, charged, marked == ResourceDomainError::None,
                   again == ResourceDomainError::None, belowRefused == ResourceDomainError::Draining,
                   rootRefused == ResourceDomainError::Draining, aboveAllowed == ResourceDomainError::None,
                   aboveReleased, childRefused == ResourceDomainError::Draining, identified, notReady,
                   released == ResourceDomainError::None, grandchildQuiesced == ResourceDomainError::None,
                   childQuiesced == ResourceDomainError::None, child.BeginDrain() == ResourceDomainError::Quiesced,
                   UsageIs(root.GetSnapshot().Usage, {}, 0U));
}
void TestEveryTypeLimit(TestContext& runner) noexcept
{
    RegistryFixture fixture;
    ResourceDomainLedger ledger;
    ResourceDomain root{ledger};
    ResourceDomain child{ledger};
    ResourceDomain sibling{ledger};
    ResourceDomainLimits limits{.CommittedMemoryPages = 0U,
                                .ResidentMemoryPages = 0U,
                                .KernelMetadataBytes = 0U,
                                .CapabilityHandles = 0U,
                                .IpcQueueBytes = 0U,
                                .Threads = 0U,
                                .ChildDomains = 2U};
    for (auto& count : limits.Objects.Counts)
    {
        count = 1U;
    }
    const bool ready = root.InitializeRoot(ResourceDomainId{100U}, limits).Error == ResourceDomainError::None;
    const auto registered = fixture.Issuer.CreateObject(root);
    const ResourceDomainObjectId identity{fixture.Registry, registered.GetValue()};
    bool bounds{true};
    for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
    {
        ResourceDomain invalid{ledger};
        auto excessive = limits;
        excessive.Objects.Counts[type] = 2U;
        bounds = bounds &&
                 invalid.InitializeChild(fixture.Registry, ResourceDomainId{103U}, identity, root, excessive).Error ==
                     ResourceDomainError::InvalidConfiguration &&
                 root.GetUsage().ChildDomains == 0U;
    }
    ResourceDomain invalidArena{ledger};
    auto excessiveArena = limits;
    ++excessiveArena.Objects.LargePageTableArenas;
    bounds =
        bounds &&
        invalidArena.InitializeChild(fixture.Registry, ResourceDomainId{103U}, identity, root, excessiveArena).Error ==
            ResourceDomainError::InvalidConfiguration &&
        root.GetUsage().ChildDomains == 0U;
    auto childLimits = limits;
    childLimits.Objects.LargePageTableArenas = 1U;
    const bool children =
        child.InitializeChild(fixture.Registry, ResourceDomainId{101U}, identity, root, childLimits).Error ==
            ResourceDomainError::None &&
        sibling.InitializeChild(fixture.Registry, ResourceDomainId{102U}, identity, root, childLimits).Error ==
            ResourceDomainError::None;
    bool all{ready && bounds && children};
    for (Foundation::Size type{0U}; type < Abi::LimitedObjectCount; ++type)
    {
        ResourceReservation one{};
        one.Objects[type] = 1U;
        all = all && child.TryReserve(one) == ResourceDomainError::None &&
              child.TryReserve(one) == ResourceDomainError::ObjectLimitExceeded &&
              sibling.TryReserve(one) == ResourceDomainError::ObjectLimitExceeded &&
              sibling.GetSnapshot().Usage.Objects[type] == 0U && root.GetSnapshot().Usage.Objects[type] == 1U &&
              child.Quiesce() == ResourceDomainError::InUse &&
              sibling.Release(one) == ResourceDomainError::ReservationUnderflow &&
              child.Release(one) == ResourceDomainError::None && sibling.TryReserve(one) == ResourceDomainError::None &&
              sibling.Release(one) == ResourceDomainError::None;
    }
    ASTERIA_EXPECT(runner,
                   "all eight type quotas bound a subtree atomically and collection-sized releases reopen each quota",
                   all);
    ResourceReservation arena{};
    arena.LargePageTableArenas = 1U;
    const bool arenas =
        child.TryReserve(arena) == ResourceDomainError::None &&
        child.TryReserve(arena) == ResourceDomainError::ObjectLimitExceeded &&
        root.GetUsage().LargePageTableArenas == 1U &&
        sibling.Release(arena) == ResourceDomainError::ReservationUnderflow &&
        sibling.TryReserve(arena) == ResourceDomainError::None &&
        root.TryReserve(arena) == ResourceDomainError::ObjectLimitExceeded &&
        root.GetUsage().LargePageTableArenas == 2U && child.GetUsage().LargePageTableArenas == 1U &&
        sibling.Quiesce() == ResourceDomainError::InUse && child.Release(arena) == ResourceDomainError::None &&
        sibling.Release(arena) == ResourceDomainError::None && root.GetUsage().LargePageTableArenas == 0U;
    ASTERIA_EXPECT(runner, "large arena units aggregate through ancestors and release atomically", arenas);
    static_cast<void>(sibling.Quiesce());
    static_cast<void>(child.Quiesce());
}

} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"ResourceDomainTests"};
    Asteria::Test::TestEveryTypeLimit(runner);
    Asteria::Test::TestRootReservations(runner);
    Asteria::Test::TestHierarchyAndAggregateLimits(runner);
    Asteria::Test::TestParentValidation(runner);
    Asteria::Test::TestDepthBound(runner);
    Asteria::Test::TestConcurrentReservations(runner);
    Asteria::Test::TestDraining(runner);
    return runner.Finish();
}
