// SPDX-License-Identifier: Apache-2.0
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <type_traits>

#include "Kernel/Device/DeviceManager.hpp"
#include "Kernel/Object/CapabilitySpace.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"
#include "Kernel/Object/RootCapabilityIssuer.hpp"

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
using Kernel::Device::DeviceAccessWidth;
using Kernel::Device::DeviceAccessWidths;
using Kernel::Device::DeviceManager;
using Kernel::Device::DeviceManagerError;
using Kernel::Device::DeviceManagerState;
using Kernel::Device::DeviceRegionCreateRequest;
using Kernel::Device::DeviceRegionCreation;
using Kernel::Device::DeviceRegionDescriptor;
using Kernel::Device::DeviceRegionError;
using Kernel::Device::DeviceRegionId;
using Kernel::Device::DeviceRegionKind;
using Kernel::Device::DeviceRegionPermission;
using Kernel::Device::DeviceRegionPermissions;
using Kernel::Device::DeviceRegionState;
using Kernel::Memory::MemoryCachePolicy;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityOperation;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectDestructionRecord;
using Kernel::Object::ObjectId;
using Kernel::Object::ObjectRegistry;
using Kernel::Object::ObjectRegistryError;
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
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

static_assert(!std::is_aggregate_v<ObjectDestructionRecord>);
static_assert(
    !std::is_constructible_v<ObjectDestructionRecord, const ObjectRegistry&, ObjectId, Kernel::Object::IKernelObject*>);

[[nodiscard]] constexpr ResourceDomainLimits MakeLimits() noexcept
{
    return ResourceDomainLimits{
        .CommittedMemoryPages = 0U,
        .ResidentMemoryPages = 0U,
        .KernelMetadataBytes = 1U << 20U,
        .CapabilityHandles = 256U,
        .IpcQueueBytes = 0U,
        .Threads = 0U,
        .ChildDomains = 0U,
    };
}

[[nodiscard]] constexpr DeviceRegionPermissions ReadWrite() noexcept
{
    return DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write);
}

[[nodiscard]] constexpr DeviceAccessWidths AllWidths() noexcept
{
    return DeviceAccessWidths{DeviceAccessWidth::Byte}
        .With(DeviceAccessWidth::Word)
        .With(DeviceAccessWidth::DoubleWord)
        .With(DeviceAccessWidth::QuadWord);
}

[[nodiscard]] constexpr DeviceRegionDescriptor MakeMmioDescriptor(const UInt64 base,
                                                                  const UInt64 length = 0x1000U) noexcept
{
    return DeviceRegionDescriptor{
        .BaseAddress = base,
        .LengthBytes = length,
        .Kind = DeviceRegionKind::MemoryMappedIo,
        .Permissions = ReadWrite(),
        .Widths = AllWidths(),
        .CachePolicy = MemoryCachePolicy::Uncached,
        .Reserved = {},
    };
}

[[nodiscard]] constexpr DeviceRegionDescriptor MakePortDescriptor(const UInt64 base) noexcept
{
    return DeviceRegionDescriptor{
        .BaseAddress = base,
        .LengthBytes = 8U,
        .Kind = DeviceRegionKind::PortIo,
        .Permissions = ReadWrite(),
        .Widths = DeviceAccessWidths{DeviceAccessWidth::Byte}
                      .With(DeviceAccessWidth::Word)
                      .With(DeviceAccessWidth::DoubleWord),
        .CachePolicy = MemoryCachePolicy::Uncached,
        .Reserved = {},
    };
}

class ManagerFixture final
{
public:
    ManagerFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Manager{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        if (Domain.InitializeRoot(ResourceDomainId{71U}, MakeLimits()).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        const auto revocation = Revocations.CreateRoot();
        if (!registered.HasValue() || !revocation.HasValue())
        {
            return false;
        }
        DomainObject = registered.GetValue();
        DomainIdentity = ResourceDomainObjectId{Registry, DomainObject};
        Revocation = revocation.GetValue();
        return true;
    }

    [[nodiscard]] DeviceRegionCreateRequest MakeRequest(const DeviceRegionDescriptor& descriptor) noexcept
    {
        return DeviceRegionCreateRequest{
            .Destination = &Space,
            .ResourceDomainStorage = &Domain,
            .ResourceDomainIdentity = DomainIdentity,
            .Descriptor = descriptor,
            .RevocationDomain = Revocation,
            .LeaseDeadline = MonotonicTime::Infinite(),
        };
    }

    [[nodiscard]] bool Finish() noexcept
    {
        if ((Manager.GetSnapshot().FreeSlots != DeviceManager::MaximumRegions) ||
            (Domain.Quiesce() != ResourceDomainError::None) ||
            (Issuer.DestroyObject(DomainObject) != ObjectRegistryError::Ok))
        {
            return false;
        }
        const auto collected = Issuer.CollectDestroyedObject();
        return collected.HasValue() && (collected.GetValue().Object.GetValue() == DomainObject.GetValue()) &&
               (collected.GetValue().Instance == &Domain);
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    DeviceManager Manager;
    ObjectId DomainObject{};
    ResourceDomainObjectId DomainIdentity{};
    RevocationDomainId Revocation{};
};

[[nodiscard]] bool RetireAndReclaim(ManagerFixture& fixture, const DeviceRegionCreation& creation) noexcept
{
    if ((fixture.Space.Close(creation.Capability) != CapabilityError::Ok) ||
        !fixture.Manager.Retire(creation.Region).HasValue())
    {
        return false;
    }
    const auto collected = fixture.Issuer.CollectDestroyedObject();
    if (!collected.HasValue())
    {
        return false;
    }
    const auto reclaimed = fixture.Manager.Reclaim(collected.GetValue());
    return reclaimed.HasValue() && (reclaimed.GetValue().GetValue() == creation.Region.GetValue()) &&
           (creation.Storage->GetSnapshot().State == DeviceRegionState::Uninitialized);
}

[[nodiscard]] bool OperationIsAllowed(ManagerFixture& fixture, const DeviceRegionCreation& creation,
                                      const CapabilityOperation operation) noexcept
{
    auto lookup = fixture.Space.Lookup(creation.Capability, ObjectType::DeviceRegion, operation);
    return lookup.HasValue() && (&lookup.GetValue().GetObject() == creation.Storage) &&
           (lookup.GetValue().GetDescriptor().Object.GetValue() == creation.Region.GetValue());
}

void TestCreationAuthorityAndReuse(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    ASTERIA_EXPECT(runner, "device-manager fixture initializes", fixture.Initialize());

    const DeviceRegionDescriptor firstDescriptor = MakeMmioDescriptor(0x1000U, 0x2000U);
    const auto firstResult = fixture.Manager.Create(fixture.MakeRequest(firstDescriptor));
    if (!firstResult.HasValue())
    {
        ASTERIA_EXPECT(runner, "DeviceManager creates its first MMIO region", false);
        return;
    }
    const DeviceRegionCreation first = firstResult.GetValue();
    constexpr CapabilityRights expectedRights = CapabilityRights{CapabilityRight::Inspect}
                                                    .With(CapabilityRight::Transfer)
                                                    .With(CapabilityRight::Read)
                                                    .With(CapabilityRight::Write)
                                                    .With(CapabilityRight::Map);
    bool exactRights{false};
    {
        auto lookup = fixture.Space.Lookup(first.Capability, ObjectType::DeviceRegion, CapabilityOperation::Inspect);
        exactRights = lookup.HasValue() && lookup.GetValue().GetDescriptor().Rights.IsEqualTo(expectedRights);
    }
    ASTERIA_EXPECT(runner, "Create publishes one exact range and bounded initial capability", exactRights,
                   OperationIsAllowed(fixture, first, CapabilityOperation::Read),
                   OperationIsAllowed(fixture, first, CapabilityOperation::Write),
                   OperationIsAllowed(fixture, first, CapabilityOperation::Map),
                   OperationIsAllowed(fixture, first, CapabilityOperation::Transfer),
                   fixture.Manager.GetSnapshot().ActiveSlots == 1U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(Kernel::Device::DeviceRegion));

    const auto invoke = fixture.Space.Lookup(first.Capability, ObjectType::DeviceRegion, CapabilityOperation::Invoke);
    ASTERIA_EXPECT(runner, "DeviceRegion capability grants no unrelated operation", !invoke.HasValue(),
                   invoke.GetError() == CapabilityError::RightsDenied);

    const auto overlap = fixture.Manager.Create(fixture.MakeRequest(MakeMmioDescriptor(0x2000U, 0x2000U)));
    ASTERIA_EXPECT(runner, "same-kind overlapping hardware ranges are never issued twice", !overlap.HasValue(),
                   overlap.GetError().Error == DeviceManagerError::RegionOverlap,
                   fixture.Manager.GetSnapshot().ActiveSlots == 1U);

    const auto adjacentResult = fixture.Manager.Create(fixture.MakeRequest(MakeMmioDescriptor(0x3000U)));
    const auto portResult = fixture.Manager.Create(fixture.MakeRequest(MakePortDescriptor(0x1000U)));
    ASTERIA_EXPECT(runner, "adjacent ranges and distinct MMIO/port address spaces remain usable",
                   adjacentResult.HasValue(), portResult.HasValue(),
                   !OperationIsAllowed(fixture, portResult.GetValue(), CapabilityOperation::Map),
                   fixture.Manager.GetSnapshot().ActiveSlots == 3U);

    DeviceRegionDescriptor readOnlyDescriptor = MakeMmioDescriptor(0x4000U);
    readOnlyDescriptor.Permissions = DeviceRegionPermissions{DeviceRegionPermission::Read};
    const auto readOnlyResult = fixture.Manager.Create(fixture.MakeRequest(readOnlyDescriptor));
    constexpr CapabilityRights readOnlyRights = CapabilityRights{CapabilityRight::Inspect}
                                                    .With(CapabilityRight::Transfer)
                                                    .With(CapabilityRight::Read)
                                                    .With(CapabilityRight::Map);
    bool exactReadOnlyRights{false};
    bool writeDenied{false};
    if (readOnlyResult.HasValue())
    {
        auto inspect = fixture.Space.Lookup(readOnlyResult.GetValue().Capability, ObjectType::DeviceRegion,
                                            CapabilityOperation::Inspect);
        exactReadOnlyRights = inspect.HasValue() && inspect.GetValue().GetDescriptor().Rights.IsEqualTo(readOnlyRights);
        const auto write = fixture.Space.Lookup(readOnlyResult.GetValue().Capability, ObjectType::DeviceRegion,
                                                CapabilityOperation::Write);
        writeDenied = !write.HasValue() && (write.GetError() == CapabilityError::RightsDenied);
    }
    ASTERIA_EXPECT(runner, "initial rights never exceed a read-only MMIO descriptor", exactReadOnlyRights, writeDenied,
                   fixture.Manager.GetSnapshot().ActiveSlots == 4U);

    ASTERIA_EXPECT(runner, "Retire prevents new lookup and begins exact generation destruction",
                   fixture.Manager.Retire(first.Region).HasValue());
    const auto stillReserved = fixture.Manager.Create(fixture.MakeRequest(firstDescriptor));
    ASTERIA_EXPECT(runner, "quiescing hardware range remains reserved until collection", !stillReserved.HasValue(),
                   stillReserved.GetError().Error == DeviceManagerError::RegionOverlap);

    const auto collected = fixture.Issuer.CollectDestroyedObject();
    const ObjectDestructionRecord firstRecord = collected.HasValue() ? collected.GetValue() : ObjectDestructionRecord{};
    const auto reclaimed = collected.HasValue() ? fixture.Manager.Reclaim(collected.GetValue())
                                                : decltype(fixture.Manager.Reclaim({}))::Failure({});
    const auto replacementResult = fixture.Manager.Create(fixture.MakeRequest(firstDescriptor));
    ASTERIA_EXPECT(runner, "collector-approved slot reuse advances registry generation without ABA",
                   reclaimed.HasValue(), replacementResult.HasValue(),
                   replacementResult.GetValue().Storage == first.Storage,
                   !replacementResult.GetValue().Region.IsEqualTo(first.Region));

    const auto staleCapability =
        fixture.Space.Lookup(first.Capability, ObjectType::DeviceRegion, CapabilityOperation::Inspect);
    ASTERIA_EXPECT(runner, "an unclosed capability cannot follow registry slot reuse", !staleCapability.HasValue(),
                   staleCapability.GetError() == CapabilityError::ObjectNotFound,
                   fixture.Space.Close(first.Capability) == CapabilityError::Ok);

    const auto staleRetire = fixture.Manager.Retire(first.Region);
    ASTERIA_EXPECT(runner, "a stale generation cannot retire a replacement in the same slot",
                   replacementResult.HasValue(), !staleRetire.HasValue(),
                   staleRetire.GetError().Error == DeviceManagerError::RegionNotOwned,
                   fixture.Manager.GetSnapshot().ActiveSlots == 4U,
                   OperationIsAllowed(fixture, replacementResult.GetValue(), CapabilityOperation::Inspect));

    const bool otherCleanup = adjacentResult.HasValue() && portResult.HasValue() && readOnlyResult.HasValue() &&
                              RetireAndReclaim(fixture, adjacentResult.GetValue()) &&
                              RetireAndReclaim(fixture, portResult.GetValue()) &&
                              RetireAndReclaim(fixture, readOnlyResult.GetValue());
    bool staleRecordRejected{false};
    bool replacementReclaimed{false};
    if (otherCleanup && replacementResult.HasValue())
    {
        const DeviceRegionCreation replacement = replacementResult.GetValue();
        if ((fixture.Space.Close(replacement.Capability) == CapabilityError::Ok) &&
            fixture.Manager.Retire(replacement.Region).HasValue())
        {
            const auto staleRecord = fixture.Manager.Reclaim(firstRecord);
            staleRecordRejected = !staleRecord.HasValue() &&
                                  (staleRecord.GetError().Error == DeviceManagerError::InvalidDestructionRecord) &&
                                  (fixture.Manager.GetSnapshot().QuiescingSlots == 1U);
            const auto replacementRecord = fixture.Issuer.CollectDestroyedObject();
            replacementReclaimed =
                replacementRecord.HasValue() && fixture.Manager.Reclaim(replacementRecord.GetValue()).HasValue();
        }
    }
    ASTERIA_EXPECT(runner, "an authentic old receipt cannot reclaim a newer slot generation", staleRecordRejected,
                   replacementReclaimed);
    ASTERIA_EXPECT(runner, "all region charges and fixed slots return after exact reclamation", otherCleanup,
                   replacementReclaimed, fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U,
                   fixture.Finish());
}

void TestFailureRollback(TestContext& runner) noexcept
{
    ManagerFixture local;
    ManagerFixture foreign;
    ASTERIA_EXPECT(runner, "device-manager rollback fixtures initialize", local.Initialize(), foreign.Initialize());

    DeviceRegionDescriptor invalid = MakeMmioDescriptor(0x8000U);
    invalid.Reserved[2U] = 1U;
    const auto invalidResult = local.Manager.Create(local.MakeRequest(invalid));
    ASTERIA_EXPECT(runner, "invalid discovery record changes no slot or ResourceDomain charge",
                   !invalidResult.HasValue(), invalidResult.GetError().Error == DeviceManagerError::InvalidDescriptor,
                   local.Manager.GetSnapshot().FreeSlots == DeviceManager::MaximumRegions,
                   local.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U);

    ResourceDomainLedger otherLedger;
    ResourceDomain otherDomain{otherLedger};
    static_cast<void>(otherDomain.InitializeRoot(ResourceDomainId{72U}, MakeLimits()));
    DeviceRegionCreateRequest wrongInstance = local.MakeRequest(MakeMmioDescriptor(0x9000U));
    wrongInstance.ResourceDomainStorage = &otherDomain;
    const auto wrongResult = local.Manager.Create(wrongInstance);
    ASTERIA_EXPECT(runner, "exact ResourceDomain instance is checked before publication", !wrongResult.HasValue(),
                   wrongResult.GetError().Error == DeviceManagerError::RegionRejected,
                   wrongResult.GetError().RegionFailure.Error == DeviceRegionError::ResourceDomainInstanceMismatch,
                   local.Manager.GetSnapshot().FreeSlots == DeviceManager::MaximumRegions);
    static_cast<void>(otherDomain.Quiesce());

    DeviceRegionCreateRequest foreignDestination = local.MakeRequest(MakeMmioDescriptor(0xA000U));
    foreignDestination.Destination = &foreign.Space;
    foreignDestination.RevocationDomain = foreign.Revocation;
    const auto mintFailure = local.Manager.Create(foreignDestination);
    const auto pending = local.Manager.GetSnapshot();
    const auto record = local.Issuer.CollectDestroyedObject();
    bool wrongRecordRejected{false};
    bool exactReclaimed{false};
    if (record.HasValue())
    {
        const ObjectDestructionRecord wrongRecord{};
        const auto rejected = local.Manager.Reclaim(wrongRecord);
        wrongRecordRejected =
            !rejected.HasValue() && (rejected.GetError().Error == DeviceManagerError::InvalidDestructionRecord);
        exactReclaimed = local.Manager.Reclaim(record.GetValue()).HasValue();
    }
    ASTERIA_EXPECT(runner, "mint failure and forged collector record preserve exact ownership", !mintFailure.HasValue(),
                   mintFailure.GetError().Error == DeviceManagerError::CapabilityRejected,
                   mintFailure.GetError().CapabilityError == CapabilityError::InvalidConfiguration,
                   pending.QuiescingSlots == 1U, wrongRecordRejected, exactReclaimed,
                   local.Manager.GetSnapshot().FreeSlots == DeviceManager::MaximumRegions,
                   local.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U, foreign.Space.GetActiveCount() == 0U);

    const auto valid = local.Manager.Create(local.MakeRequest(MakeMmioDescriptor(0xB000U)));
    ASTERIA_EXPECT(runner, "manager remains usable after complete rollback and reclamation", valid.HasValue(),
                   RetireAndReclaim(local, valid.GetValue()), local.Finish(), foreign.Finish());
}

void TestCollectorRegistryProvenance(TestContext& runner) noexcept
{
    ManagerFixture local;
    ManagerFixture foreign;
    ASTERIA_EXPECT(runner, "collector-provenance fixtures initialize", local.Initialize(), foreign.Initialize());

    const auto created = local.Manager.Create(local.MakeRequest(MakeMmioDescriptor(0x00F0'0000U)));
    if (!created.HasValue())
    {
        ASTERIA_EXPECT(runner, "collector-provenance region is created", false);
        return;
    }
    const DeviceRegionCreation creation = created.GetValue();

    // This deliberately exercises RootCapabilityIssuer's privileged caller
    // precondition by registering the same storage in a second registry. The
    // destruction receipt must still carry enough provenance to fail closed.
    const auto duplicate = foreign.Issuer.CreateObject(*creation.Storage);
    const bool rawIdentityCollision =
        duplicate.HasValue() && (duplicate.GetValue().GetValue() == creation.Region.GetValue());
    const bool duplicateDestroyed =
        duplicate.HasValue() && (foreign.Issuer.DestroyObject(duplicate.GetValue()) == ObjectRegistryError::Ok);
    const auto foreignReceipt = foreign.Issuer.CollectDestroyedObject();
    const bool foreignReceiptReady = duplicateDestroyed && foreignReceipt.HasValue();

    bool localCollectionBlocked{false};
    bool foreignReceiptRejected{false};
    if (foreignReceipt.HasValue())
    {
        {
            auto retained =
                local.Space.Lookup(creation.Capability, ObjectType::DeviceRegion, CapabilityOperation::Inspect);
            if (retained.HasValue() && (local.Space.Close(creation.Capability) == CapabilityError::Ok) &&
                local.Manager.Retire(creation.Region).HasValue())
            {
                const auto premature = local.Issuer.CollectDestroyedObject();
                localCollectionBlocked =
                    !premature.HasValue() && (premature.GetError() == ObjectRegistryError::NoDestructionReady);
                const auto rejected = local.Manager.Reclaim(foreignReceipt.GetValue());
                foreignReceiptRejected = !rejected.HasValue() &&
                                         (rejected.GetError().Error == DeviceManagerError::InvalidDestructionRecord) &&
                                         (local.Manager.GetSnapshot().QuiescingSlots == 1U) &&
                                         (creation.Storage->GetSnapshot().State == DeviceRegionState::Ready);
            }
        }
    }

    const auto localReceipt = local.Issuer.CollectDestroyedObject();
    const bool reclaimed = localReceipt.HasValue() && local.Manager.Reclaim(localReceipt.GetValue()).HasValue();
    ASTERIA_EXPECT(runner, "a foreign registry receipt cannot reclaim a live local generation", rawIdentityCollision,
                   foreignReceiptReady, localCollectionBlocked, foreignReceiptRejected, reclaimed, local.Finish(),
                   foreign.Finish());
}

void TestConcurrentCapacity(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    ASTERIA_EXPECT(runner, "concurrent capacity fixture initializes", fixture.Initialize());

    DeviceRegionCreation creations[DeviceManager::MaximumRegions]{};
    std::atomic<Size> ready{0U};
    std::atomic<bool> start{false};
    std::atomic<Size> failures{0U};
    const auto worker = [&fixture, &creations, &ready, &start, &failures](const Size workerIndex) noexcept
    {
        static_cast<void>(ready.fetch_add(1U, std::memory_order_release));
        bool released{false};
        for (Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (start.load(std::memory_order_acquire))
            {
                released = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!released)
        {
            static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
            return;
        }
        constexpr Size PerWorker{DeviceManager::MaximumRegions / 4U};
        for (Size localIndex{0U}; localIndex < PerWorker; ++localIndex)
        {
            const Size index = (workerIndex * PerWorker) + localIndex;
            const UInt64 base = 0x0100'0000U + (static_cast<UInt64>(index) * 0x2000U);
            const auto created = fixture.Manager.Create(fixture.MakeRequest(MakeMmioDescriptor(base)));
            if (!created.HasValue())
            {
                static_cast<void>(failures.fetch_add(1U, std::memory_order_relaxed));
                continue;
            }
            creations[index] = created.GetValue();
        }
    };

    std::thread first{worker, 0U};
    std::thread second{worker, 1U};
    std::thread third{worker, 2U};
    std::thread fourth{worker, 3U};
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
    first.join();
    second.join();
    third.join();
    fourth.join();

    const auto overflow = fixture.Manager.Create(fixture.MakeRequest(MakeMmioDescriptor(0x0200'0000U)));
    ASTERIA_EXPECT(runner, "concurrent disjoint discovery fills each bounded slot exactly once", allReady,
                   failures.load(std::memory_order_acquire) == 0U,
                   fixture.Manager.GetSnapshot().ActiveSlots == DeviceManager::MaximumRegions, !overflow.HasValue(),
                   overflow.GetError().Error == DeviceManagerError::CapacityExceeded);

    bool retiredAll{true};
    for (const DeviceRegionCreation& creation : creations)
    {
        if ((fixture.Space.Close(creation.Capability) != CapabilityError::Ok) ||
            !fixture.Manager.Retire(creation.Region).HasValue())
        {
            retiredAll = false;
        }
    }
    bool reclaimedAll{retiredAll};
    for (Size index{0U}; index < DeviceManager::MaximumRegions; ++index)
    {
        const auto collected = fixture.Issuer.CollectDestroyedObject();
        if (!collected.HasValue() || !fixture.Manager.Reclaim(collected.GetValue()).HasValue())
        {
            reclaimedAll = false;
            break;
        }
    }
    ASTERIA_EXPECT(runner, "all concurrently issued regions reclaim without leaks or duplicate slots", reclaimedAll,
                   fixture.Manager.GetSnapshot().State == DeviceManagerState::Ready,
                   fixture.Manager.GetSnapshot().FreeSlots == DeviceManager::MaximumRegions,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U, fixture.Finish());
}

void TestConcurrentDuplicateExclusion(TestContext& runner) noexcept
{
    ManagerFixture fixture;
    ASTERIA_EXPECT(runner, "concurrent duplicate fixture initializes", fixture.Initialize());

    DeviceRegionCreation creations[2U]{};
    DeviceManagerError errors[2U]{DeviceManagerError::None, DeviceManagerError::None};
    bool succeeded[2U]{};
    std::atomic<Size> ready{0U};
    std::atomic<bool> start{false};
    const auto worker = [&fixture, &creations, &errors, &succeeded, &ready, &start](const Size index) noexcept
    {
        static_cast<void>(ready.fetch_add(1U, std::memory_order_release));
        for (Size wait{0U}; wait < 1'000'000U; ++wait)
        {
            if (start.load(std::memory_order_acquire))
            {
                const auto result = fixture.Manager.Create(fixture.MakeRequest(MakeMmioDescriptor(0x0300'0000U)));
                succeeded[index] = result.HasValue();
                if (result.HasValue())
                {
                    creations[index] = result.GetValue();
                }
                else
                {
                    errors[index] = result.GetError().Error;
                }
                return;
            }
            std::this_thread::yield();
        }
        errors[index] = DeviceManagerError::Faulted;
    };

    std::thread first{worker, 0U};
    std::thread second{worker, 1U};
    bool bothReady{false};
    for (Size wait{0U}; wait < 1'000'000U; ++wait)
    {
        if (ready.load(std::memory_order_acquire) == 2U)
        {
            bothReady = true;
            break;
        }
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    const Size winner = succeeded[0U] ? 0U : 1U;
    const Size loser = succeeded[0U] ? 1U : 0U;
    ASTERIA_EXPECT(runner, "concurrent duplicate discovery publishes exactly one region", bothReady,
                   succeeded[0U] != succeeded[1U], errors[loser] == DeviceManagerError::RegionOverlap,
                   fixture.Manager.GetSnapshot().ActiveSlots == 1U);
    ASTERIA_EXPECT(runner, "concurrent duplicate winner retires without leaked loser state", succeeded[winner],
                   RetireAndReclaim(fixture, creations[winner]), fixture.Finish());
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"DeviceManagerTests"};
    Asteria::Test::TestCreationAuthorityAndReuse(runner);
    Asteria::Test::TestFailureRollback(runner);
    Asteria::Test::TestCollectorRegistryProvenance(runner);
    Asteria::Test::TestConcurrentCapacity(runner);
    Asteria::Test::TestConcurrentDuplicateExclusion(runner);
    return runner.Finish();
}
