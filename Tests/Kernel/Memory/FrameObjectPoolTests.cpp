// SPDX-License-Identifier: Apache-2.0
#include <memory>
#include <cstring>
#include <vector>

#include "Kernel/Device/DeviceRegion.hpp"
#include "Kernel/Memory/FrameObjectPool.hpp"
#include "Kernel/Memory/MemoryObjectPool.hpp"
#include "Kernel/Object/RevocationRegistry.hpp"

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
using Foundation::UInt64;
using Kernel::Memory::Frame;
using Kernel::Memory::FrameCreateRequest;
using Kernel::Memory::FrameObjectPool;
using Kernel::Memory::FramePool;
using Kernel::Memory::FramePoolError;
using Kernel::Memory::MemoryObject;
using Kernel::Memory::MemoryObjectCreateRequest;
using Kernel::Memory::MemoryObjectPool;
using Kernel::Memory::PageCount;
using Kernel::Memory::PagePermission;
using Kernel::Memory::PagePermissions;
using Kernel::Memory::PhysicalAddress;
using Kernel::Object::CapabilityError;
using Kernel::Object::CapabilityHandle;
using Kernel::Object::CapabilityRight;
using Kernel::Object::CapabilityRights;
using Kernel::Object::CapabilitySpace;
using Kernel::Object::MonotonicTime;
using Kernel::Object::ObjectPin;
using Kernel::Object::ObjectPinError;
using Kernel::Object::ObjectPoolError;
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

class EntireSubtree final : public Kernel::Resource::IResourceDomainSubtree
{
public:
    [[nodiscard]] bool Contains(const ResourceDomain*) const noexcept override
    {
        return true;
    }
};

class PoolFixture final
{
public:
    explicit PoolFixture(const Foundation::UInt32 maximumReferences = 0xFFFF'FFFFU) noexcept
        : Registry{Synchronization, maximumReferences}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Frames{Registry, Issuer, Pages}, MemoryObjects{Registry, Issuer}
    {
        static_cast<void>(Frames.AttachSlab(Slabs.Frames));
        static_cast<void>(MemoryObjects.AttachSlab(Slabs.MemoryObjects));
    }

    [[nodiscard]] bool Initialize(const UInt64 poolPages = 8U, const UInt64 limit = 16U) noexcept
    {
        const ResourceDomainLimits limits{
            .CommittedMemoryPages = limit,
            .ResidentMemoryPages = limit,
            .KernelMetadataBytes = 1U << 20U,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .ChildDomains = 0U,
        };
        const auto root = Revocations.CreateRoot();
        if ((Domain.InitializeRoot(ResourceDomainId{5U}, limits).Error != ResourceDomainError::None) ||
            !root.HasValue() || (Pages.Initialize(PhysicalAddress{0x0040'0000U}, poolPages) != FramePoolError::None))
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        if (!registered.HasValue())
        {
            return false;
        }
        const auto minted = Issuer.Mint(
            Space, registered.GetValue(),
            CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect).With(CapabilityRight::Duplicate),
            root.GetValue(), MonotonicTime::Infinite(), 0U);
        Authority = minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
        return Authority.IsValid();
    }

    [[nodiscard]] auto CreateFrame(const UInt64 pages) noexcept
    {
        return Frames.Create(FrameCreateRequest{
            .Space = &Space, .Authority = Authority, .Pages = PageCount{pages}, .Region = {}, .RegionOffsetPages = 0U});
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    FramePool Pages;
    Asteria::TestSupport::PoolSlabs Slabs;
    FrameObjectPool Frames;
    MemoryObjectPool MemoryObjects;
    CapabilityHandle Authority{};
};

// Fake scrubber re-enters snapshot/collection/drain APIs while zeroing. A held pool lock would
// deadlock; an exposed slot would be reclaimed.
class CreationScrubber final : public Kernel::Memory::IFrameScrubber
{
public:
    explicit CreationScrubber(PoolFixture& fixture) : Fixture{fixture}, Bytes(256U * 4096U, 0U)
    {
    }
    [[nodiscard]] Kernel::Memory::FrameScrubError Zero(const PhysicalAddress start,
                                                       const UInt64 pages) noexcept override
    {
        ++Calls;
        const auto slots = Fixture.Frames.GetSnapshot();
        const auto charge = Fixture.Domain.GetSnapshot().Usage;
        const auto collected = Fixture.Frames.Collect();
        EntireSubtree subtree;
        Consistent = Consistent && (pages <= 16U) && (slots.ZeroingSlots == 1U) &&
                     (charge.CommittedMemoryPages >= pages) && collected.HasValue() && (collected.GetValue() == 0U) &&
                     (Fixture.Frames.DestroyCharged(subtree).Error == ObjectPoolError::Busy);
        if (Revoke)
        {
            Consistent = Consistent && (Fixture.Space.Close(Fixture.Authority) == CapabilityError::Ok);
            Revoke = false;
        }
        if ((Calls == FailAt) && (Answer != Kernel::Memory::FrameScrubError::None))
        {
            return Answer;
        }
        const Size offset = static_cast<Size>(start.GetValue() - 0x0040'0000U);
        std::memset(Bytes.data() + offset, 0, static_cast<Size>(pages * 4096U));
        return Kernel::Memory::FrameScrubError::None;
    }
    PoolFixture& Fixture;
    std::vector<unsigned char> Bytes;
    UInt64 Calls{0U};
    UInt64 FailAt{0U};
    Kernel::Memory::FrameScrubError Answer{Kernel::Memory::FrameScrubError::None};
    bool Consistent{true};
    bool Revoke{false};
};

void TestCreationZeroing(TestContext& runner)
{
    for (const auto answer : {Kernel::Memory::FrameScrubError::None, Kernel::Memory::FrameScrubError::Deferred,
                              Kernel::Memory::FrameScrubError::Faulted})
    {
        for (const UInt64 cleanPages : {0ULL, 128ULL})
        {
            auto fixture = std::make_unique<PoolFixture>();
            const bool ready = fixture->Initialize(256U, 256U);
            bool dirtied{true};
            for (UInt64 pages{0U}; pages < (256U - cleanPages); pages += 128U)
            {
                const auto dirty = fixture->Pages.Allocate(128U);
                dirtied = dirtied && dirty.HasValue() &&
                          fixture->Pages.Release(dirty.GetValue(), 128U) == FramePoolError::None;
            }
            CreationScrubber scrubber{*fixture};
            std::memset(scrubber.Bytes.data(), 0xA5, static_cast<Size>((256U - cleanPages) * 4096U));
            scrubber.Answer = answer;
            scrubber.FailAt = 2U; // the first 16-page chunk completed
            const bool configured = fixture->Frames.ConfigureScrubber(scrubber);
            const auto created = fixture->CreateFrame(128U);
            const bool expectedSuccess = answer == Kernel::Memory::FrameScrubError::None || cleanPages != 0U;
            bool zero{true};
            if (created.HasValue())
            {
                const Size offset = static_cast<Size>(created.GetValue().PhysicalStart.GetValue() - 0x0040'0000U);
                for (Size byte{offset}; byte < offset + 128U * 4096U; ++byte)
                {
                    zero = zero && scrubber.Bytes[byte] == 0U;
                }
                static_cast<void>(fixture->Frames.Destroy(fixture->Space, created.GetValue().Capability));
                static_cast<void>(fixture->Frames.Collect());
            }
            const auto slots = fixture->Frames.GetSnapshot();
            const auto usage = fixture->Domain.GetSnapshot().Usage;
            ASTERIA_EXPECT(runner, "creation chunks zero outside locks; partial Deferred/Faulted runs retry clean once",
                           ready, dirtied, configured, created.HasValue() == expectedSuccess, zero, scrubber.Consistent,
                           slots.ZeroingSlots == 0U, slots.PeakZeroingSlots == 1U,
                           slots.FreeSlots == FrameObjectPool::Capacity, usage.CommittedMemoryPages == 0U,
                           usage.KernelMetadataBytes == 0U,
                           slots.ZeroDeferrals == (answer == Kernel::Memory::FrameScrubError::Deferred ? 1U : 0U),
                           slots.ZeroFaults == (answer == Kernel::Memory::FrameScrubError::Faulted ? 1U : 0U));
        }
    }
}

void TestDirtyMintRollback(TestContext& runner)
{
    auto fixture = std::make_unique<PoolFixture>();
    const bool ready = fixture->Initialize(64U, 128U);
    const auto dirty = fixture->Pages.Allocate(64U);
    const bool released = dirty.HasValue() && fixture->Pages.Release(dirty.GetValue(), 64U) == FramePoolError::None;
    CreationScrubber scrubber{*fixture};
    std::memset(scrubber.Bytes.data(), 0xA5, scrubber.Bytes.size());
    scrubber.Revoke = true;
    const bool configured = fixture->Frames.ConfigureScrubber(scrubber);
    const auto created = fixture->CreateFrame(32U);
    const auto snapshot = fixture->Pages.GetSnapshot();
    ASTERIA_EXPECT(runner, "authority lost while zeroing rolls registration and charge back with ReturnVerified", ready,
                   released, configured,
                   !created.HasValue() && created.GetError().Error == ObjectPoolError::MintRejected,
                   scrubber.Calls == 2U, scrubber.Consistent, snapshot.FreePages == 32U, snapshot.DirtyPages == 32U,
                   snapshot.AllocatedPages == 0U, fixture->Registry.GetActiveCount() == 1U,
                   fixture->Domain.GetSnapshot().Usage.CommittedMemoryPages == 0U);
}

void TestBeforeZeroRollbacks(TestContext& runner)
{
    auto pinned = std::make_unique<PoolFixture>(1U);
    const bool pinnedReady = pinned->Initialize();
    const auto refusedPin = pinned->CreateFrame(1U);
    auto fixture = std::make_unique<PoolFixture>();
    const bool ready = fixture->Initialize(8U, 16U);
    CreationScrubber scrubber{*fixture};
    const bool configured = fixture->Frames.ConfigureScrubber(scrubber);
    const auto invalid = fixture->CreateFrame(0U);
    const auto overCharge = fixture->CreateFrame(32U);
    const auto first = fixture->CreateFrame(8U);
    const auto exhausted = fixture->CreateFrame(1U);
    ASTERIA_EXPECT(
        runner, "invalid request, domain pin and allocator exhaustion perform no zeroing", pinnedReady,
        !refusedPin.HasValue() && refusedPin.GetError().Error == ObjectPoolError::ResourceDomainRejected, ready,
        configured, !invalid.HasValue() && invalid.GetError().Error == ObjectPoolError::InvalidRequest,
        !overCharge.HasValue() && overCharge.GetError().Error == ObjectPoolError::ChargeRejected, first.HasValue(),
        !exhausted.HasValue() && exhausted.GetError().Error == ObjectPoolError::CapacityExceeded, scrubber.Calls == 0U,
        fixture->Domain.GetSnapshot().Usage.CommittedMemoryPages == 8U,
        fixture->Frames.GetSnapshot().ZeroingSlots == 0U);
}

void TestRetryMintRollback(TestContext& runner)
{
    auto fixture = std::make_unique<PoolFixture>();
    const bool ready = fixture->Initialize(256U, 256U);
    const auto dirty = fixture->Pages.Allocate(128U);
    const bool released = dirty.HasValue() && fixture->Pages.Release(dirty.GetValue(), 128U) == FramePoolError::None;
    CreationScrubber scrubber{*fixture};
    scrubber.Revoke = true;
    scrubber.Answer = Kernel::Memory::FrameScrubError::Deferred;
    scrubber.FailAt = 2U;
    const bool configured = fixture->Frames.ConfigureScrubber(scrubber);
    const auto created = fixture->CreateFrame(128U);
    const auto snapshot = fixture->Pages.GetSnapshot();
    ASTERIA_EXPECT(runner, "mint failure after a clean retry clears the old dirty mask before ReturnVerified", ready,
                   released, configured,
                   !created.HasValue() && created.GetError().Error == ObjectPoolError::MintRejected,
                   scrubber.Consistent, scrubber.Calls == 2U, snapshot.FreePages == 128U, snapshot.DirtyPages == 128U,
                   snapshot.AllocatedPages == 0U, fixture->Frames.GetSnapshot().ZeroingSlots == 0U,
                   fixture->Domain.GetSnapshot().Usage.CommittedMemoryPages == 0U);
}

void TestCreationProbe(TestContext& runner)
{
    auto fixture = std::make_unique<PoolFixture>();
    const bool ready = fixture->Initialize(1024U, 128U);
    CreationScrubber scrubber{*fixture};
    const bool configured = fixture->Frames.ConfigureScrubber(scrubber);
    fixture->Frames.EnableCreationZeroProbe();
    const auto created = fixture->CreateFrame(1U);
    const auto authority = created.HasValue() ? fixture->Space.Query(created.GetValue().Capability)
                                              : fixture->Space.Query(CapabilityHandle{});
    ASTERIA_EXPECT(
        runner, "desktop session gives every Frame creator Read, Write and Map", authority.HasValue(),
        authority.HasValue() &&
            authority.GetValue().Rights.ContainsAll(
                CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Read).With(CapabilityRight::Write)));
    const auto pages = fixture->Pages.GetSnapshot();
    const auto slots = fixture->Frames.GetSnapshot();
    ASTERIA_EXPECT(runner, "test firmware's initial dirty return exercises the production charged zero path", ready,
                   configured, created.HasValue(), scrubber.Consistent, scrubber.Calls == 1U,
                   pages.AllocatedPages == 1U, pages.DirtyPages == 1023U, pages.FreePages == 0U,
                   slots.CreationZeroedPages == 1U, slots.ZeroingSlots == 0U, slots.PeakZeroingSlots == 1U);
}

void TestDirtyRegistrationRollback(TestContext& runner)
{
    auto fixture = std::make_unique<PoolFixture>();
    const bool ready = fixture->Initialize(64U, 128U);
    std::vector<std::unique_ptr<Kernel::Memory::CreatedFrame>> blockers;
    bool full{true};
    for (Size index{1U}; index < fixture->Registry.GetCapacity(); ++index)
    {
        auto object = std::make_unique<Kernel::Memory::CreatedFrame>();
        full = full && object->Initialize(
                           Kernel::Object::ResourceDomainId{5U},
                           Kernel::Memory::FrameDescriptor{.PhysicalStart = PhysicalAddress{0x0040'0000U},
                                                           .Pages = PageCount{1U},
                                                           .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
                                                           .Kind = Kernel::Memory::FrameKind::Ram,
                                                           .Reserved = {}}) == Kernel::Memory::FrameError::None;
        full = full && fixture->Issuer.CreateObject(*object).HasValue();
        blockers.push_back(std::move(object));
    }
    const auto dirty = fixture->Pages.Allocate(64U);
    const bool released = dirty.HasValue() && fixture->Pages.Release(dirty.GetValue(), 64U) == FramePoolError::None;
    CreationScrubber scrubber{*fixture};
    std::memset(scrubber.Bytes.data(), 0xA5, scrubber.Bytes.size());
    const bool configured = fixture->Frames.ConfigureScrubber(scrubber);
    const auto created = fixture->CreateFrame(32U);
    const auto snapshot = fixture->Pages.GetSnapshot();
    ASTERIA_EXPECT(runner, "registration failure after zeroing returns verified clean pages and releases charge", ready,
                   full, released, configured,
                   !created.HasValue() && created.GetError().Error == ObjectPoolError::RegistrationRejected,
                   scrubber.Calls == 2U, scrubber.Consistent, snapshot.FreePages == 32U, snapshot.DirtyPages == 32U,
                   snapshot.AllocatedPages == 0U, fixture->Domain.GetSnapshot().Usage.CommittedMemoryPages == 0U,
                   fixture->Frames.GetSnapshot().ZeroingSlots == 0U);
}

// A mint that fails after the charge, the page run and the registration
// rolls all three back, and the pages it never published return clean.
void TestMintRollbackReturnsCleanPages(TestContext& runner) noexcept
{
    // The pools are too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    bool full{false};
    for (Size index{0U}; (index < CapabilitySpace::MaximumSlots) && !full; ++index)
    {
        const auto duplicated = fixture.Space.Duplicate(fixture.Authority, CapabilityRights{CapabilityRight::Inspect},
                                                        MonotonicTime::Infinite());
        full = !duplicated.HasValue() && (duplicated.GetError() == CapabilityError::SpaceExhausted);
    }
    const Size registered = fixture.Registry.GetActiveCount();
    const auto frame = fixture.CreateFrame(2U);
    const auto memory = fixture.MemoryObjects.Create(MemoryObjectCreateRequest{
        .Space = &fixture.Space,
        .Authority = fixture.Authority,
        .Pages = PageCount{2U},
        .MaximumPermissions = PagePermissions{PagePermission::Read}.With(PagePermission::User),
        .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack,
    });
    const auto pool = fixture.Pages.GetSnapshot();
    const auto usage = fixture.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "a failed mint returns the charge, the registration and clean pages", ready, full,
                   !frame.HasValue() && (frame.GetError().Error == ObjectPoolError::MintRejected),
                   !frame.HasValue() && (frame.GetError().Capability == CapabilityError::SpaceExhausted),
                   !memory.HasValue() && (memory.GetError().Error == ObjectPoolError::MintRejected),
                   fixture.Registry.GetActiveCount() == registered, pool.FreePages == 8U, pool.DirtyPages == 0U,
                   usage.KernelMetadataBytes == 0U, usage.CommittedMemoryPages == 0U, usage.ResidentMemoryPages == 0U,
                   fixture.Frames.GetSnapshot().FreeSlots == FrameObjectPool::Capacity,
                   fixture.MemoryObjects.GetSnapshot().FreeSlots == MemoryObjectPool::Capacity);
}

// An AddressSpace pins a Frame per mapping: a destroyed Frame stays
// quiescing, its pages allocated, until the last pin is gone.
void TestPinnedFrameWaitsForCollection(TestContext& runner) noexcept
{
    // The pools are too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const auto frame = fixture.CreateFrame(3U);
    ObjectPin pin;
    const bool pinned = frame.HasValue() && (fixture.Registry.PinObject(frame.GetValue().Frame, ObjectType::Frame,
                                                                        pin) == ObjectPinError::Ok);
    const bool destroyed =
        frame.HasValue() && fixture.Frames.Destroy(fixture.Space, frame.GetValue().Capability).HasValue();
    const auto held = fixture.Frames.Collect();
    const auto heldPool = fixture.Pages.GetSnapshot();
    pin.Reset();
    const auto collected = fixture.Frames.Collect();
    const auto pool = fixture.Pages.GetSnapshot();
    ASTERIA_EXPECT(runner, "a pinned Frame is collected only after its last pin, its pages then dirty", ready,
                   frame.HasValue(), pinned, destroyed, held.HasValue() && (held.GetValue() == 0U),
                   heldPool.AllocatedPages == 3U, collected.HasValue() && (collected.GetValue() == 1U),
                   pool.DirtyPages == 3U, pool.AllocatedPages == 0U, pool.FreePages == 5U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == 0U,
                   frame.HasValue() && (frame.GetValue().PhysicalStart.GetValue() == 0x0040'0000U));

    const auto exhausted = fixture.CreateFrame(6U);
    const auto fits = fixture.CreateFrame(2U);
    ASTERIA_EXPECT(runner, "dirty pages are not reused",
                   !exhausted.HasValue() && (exhausted.GetError().Error == ObjectPoolError::Busy),
                   fits.HasValue() && (fits.GetValue().PhysicalStart.GetValue() == 0x0040'6000U),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(Kernel::Memory::CreatedFrame));
}

// A device Frame names pages of a memory-mapped DeviceRegion the caller may Map, with the region's
// cache policy, takes no frame-pool pages, charges its metadata only, and pins its region until it
// is collected.
void TestDeviceFrames(TestContext& runner) noexcept
{
    using Kernel::Device::DeviceAccessWidth;
    using Kernel::Device::DeviceAccessWidths;
    using Kernel::Device::DeviceRegion;
    using Kernel::Device::DeviceRegionDescriptor;
    using Kernel::Device::DeviceRegionError;
    using Kernel::Device::DeviceRegionKind;
    using Kernel::Device::DeviceRegionPermission;
    using Kernel::Device::DeviceRegionPermissions;
    using Kernel::Memory::FrameKind;
    using Kernel::Memory::MemoryCachePolicy;
    // The pools are too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const auto domainObject = fixture.Registry.GetActiveCount() != 0U;
    DeviceRegion region;
    const auto revocations = fixture.Revocations.CreateRoot();
    const auto domainIdentity = fixture.Issuer.CreateObject(fixture.Domain);
    const bool initialized =
        domainIdentity.HasValue() &&
        (region
             .Initialize(
                 fixture.Registry,
                 Kernel::Resource::ResourceDomainObjectId{fixture.Registry, domainIdentity.GetValue()}, fixture.Domain,
                 DeviceRegionDescriptor{
                     .BaseAddress = 0x0000'0000'FEB0'0000ULL,
                     .LengthBytes = 0x4000U,
                     .Kind = DeviceRegionKind::MemoryMappedIo,
                     .Permissions =
                         DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write),
                     .Widths = DeviceAccessWidths{DeviceAccessWidth::DoubleWord},
                     .CachePolicy = MemoryCachePolicy::Uncached,
                     .Reserved = {},
                 })
             .Error == DeviceRegionError::None);
    const auto registered = fixture.Issuer.CreateObject(region);
    const auto mint = [&](const CapabilityRights rights) noexcept
    {
        const auto minted =
            (registered.HasValue() && revocations.HasValue())
                ? fixture.Issuer.Mint(fixture.Space, registered.GetValue(), rights, revocations.GetValue(),
                                      MonotonicTime::Infinite(), 0U)
                : decltype(fixture.Issuer.Mint(fixture.Space, Kernel::Object::ObjectId{}, rights, RevocationDomainId{},
                                               MonotonicTime::Infinite(), 0U))::Failure(CapabilityError::InvalidHandle);
        return minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    };
    const CapabilityHandle mappable = mint(CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Inspect));
    const CapabilityHandle inspectOnly = mint(CapabilityRights{CapabilityRight::Inspect});
    const auto usage = fixture.Domain.GetSnapshot().Usage;
    const auto create = [&](const CapabilityHandle handle, const UInt64 offset, const UInt64 pages) noexcept
    {
        return fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                        .Authority = fixture.Authority,
                                                        .Pages = PageCount{pages},
                                                        .Region = handle,
                                                        .RegionOffsetPages = offset});
    };
    const auto device = create(mappable, 1U, 2U);
    const auto refused = create(inspectOnly, 0U, 1U);
    const auto beyond = create(mappable, 3U, 2U);
    ObjectPin framePin;
    const bool pinned = device.HasValue() && (fixture.Registry.PinObject(device.GetValue().Frame, ObjectType::Frame,
                                                                         framePin) == ObjectPinError::Ok);
    const auto snapshot = pinned ? static_cast<const Frame&>(framePin.GetObject()).GetSnapshot()
                                 : decltype(static_cast<const Frame&>(framePin.GetObject()).GetSnapshot()){};
    const auto named = pinned ? fixture.Frames.GetDeviceRegion(static_cast<const Frame&>(framePin.GetObject()))
                              : Kernel::Device::DeviceRegionId{};
    framePin.Reset();
    const auto charged = fixture.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(
        runner, "a device Frame names its region's pages uncached and charges metadata only", ready, domainObject,
        initialized, registered.HasValue(), mappable.IsValid(), device.HasValue(), pinned,
        snapshot.Descriptor.Kind == FrameKind::Device, snapshot.Descriptor.CachePolicy == MemoryCachePolicy::Uncached,
        snapshot.Descriptor.PhysicalStart.GetValue() == 0xFEB0'1000ULL, snapshot.Descriptor.Pages.GetValue() == 2U,
        registered.HasValue() && (named.GetObjectId().GetValue() == registered.GetValue().GetValue()),
        charged.CommittedMemoryPages == usage.CommittedMemoryPages,
        charged.KernelMetadataBytes == (usage.KernelMetadataBytes + sizeof(Kernel::Memory::CreatedFrame)),
        fixture.Pages.GetSnapshot().AllocatedPages == 0U,
        !refused.HasValue() && (refused.GetError().Error == ObjectPoolError::CapabilityRejected),
        !beyond.HasValue() && (beyond.GetError().Error == ObjectPoolError::InvalidRequest));

    // The Frame's pin keeps the retired region's registration, and so its
    // slot and range, until the Frame is collected.
    bool retired{false};
    bool held{false};
    if (registered.HasValue())
    {
        retired = fixture.Issuer.DestroyObject(registered.GetValue()) == Kernel::Object::ObjectRegistryError::Ok;
        const auto early = fixture.Issuer.CollectDestroyedObject(registered.GetValue());
        held = !early.HasValue() && (early.GetError() == Kernel::Object::ObjectRegistryError::NoDestructionReady);
    }
    const bool destroyed =
        device.HasValue() && fixture.Frames.Destroy(fixture.Space, device.GetValue().Capability).HasValue();
    const auto collected = fixture.Frames.Collect();
    const auto released = fixture.Domain.GetSnapshot().Usage;
    const bool freed = registered.HasValue() && fixture.Issuer.CollectDestroyedObject(registered.GetValue()).HasValue();
    ASTERIA_EXPECT(runner, "the collected device Frame returns its charge and no pages, and lets the region go",
                   retired, held, destroyed, collected.HasValue() && (collected.GetValue() == 1U),
                   released.KernelMetadataBytes == usage.KernelMetadataBytes,
                   fixture.Pages.GetSnapshot().FreePages == 8U, fixture.Pages.GetSnapshot().DirtyPages == 0U, freed);
    static_cast<void>(region.Quiesce());
}
// A device Frame is bounded by its region, not by a frame-pool run, so a framebuffer of 1000 pages
// is one Frame; a Frame of RAM keeps the run's bound.
void TestLargeDeviceFrame(TestContext& runner) noexcept
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
    // The pools are too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    DeviceRegion region;
    const auto revocations = fixture.Revocations.CreateRoot();
    const auto domainIdentity = fixture.Issuer.CreateObject(fixture.Domain);
    const bool initialized =
        domainIdentity.HasValue() &&
        (region
             .Initialize(
                 fixture.Registry,
                 Kernel::Resource::ResourceDomainObjectId{fixture.Registry, domainIdentity.GetValue()}, fixture.Domain,
                 DeviceRegionDescriptor{
                     .BaseAddress = 0x0000'0000'8000'0000ULL,
                     .LengthBytes = 1000U * 4096U,
                     .Kind = DeviceRegionKind::MemoryMappedIo,
                     .Permissions =
                         DeviceRegionPermissions{DeviceRegionPermission::Read}.With(DeviceRegionPermission::Write),
                     .Widths = DeviceAccessWidths{DeviceAccessWidth::DoubleWord},
                     .CachePolicy = MemoryCachePolicy::Uncached,
                     .Reserved = {},
                 })
             .Error == DeviceRegionError::None);
    const auto registered = fixture.Issuer.CreateObject(region);
    const auto minted = (registered.HasValue() && revocations.HasValue())
                            ? fixture.Issuer.Mint(fixture.Space, registered.GetValue(),
                                                  CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Inspect),
                                                  revocations.GetValue(), MonotonicTime::Infinite(), 0U)
                            : decltype(fixture.Issuer.Mint(
                                  fixture.Space, Kernel::Object::ObjectId{}, CapabilityRights{}, RevocationDomainId{},
                                  MonotonicTime::Infinite(), 0U))::Failure(CapabilityError::InvalidHandle);
    const CapabilityHandle mappable = minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
    const auto whole = fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                                .Authority = fixture.Authority,
                                                                .Pages = PageCount{1000U},
                                                                .Region = mappable,
                                                                .RegionOffsetPages = 0U});
    const auto beyond = fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                                 .Authority = fixture.Authority,
                                                                 .Pages = PageCount{1001U},
                                                                 .Region = mappable,
                                                                 .RegionOffsetPages = 0U});
    const auto ram =
        fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                 .Authority = fixture.Authority,
                                                 .Pages = PageCount{Kernel::Memory::FramePool::MaximumRunPages + 1U},
                                                 .Region = CapabilityHandle{},
                                                 .RegionOffsetPages = 0U});
    ASTERIA_EXPECT(runner, "a device Frame may span its whole region past a frame-pool run; RAM may not", ready,
                   initialized, registered.HasValue(), mappable.IsValid(), whole.HasValue(),
                   !beyond.HasValue() && (beyond.GetError().Error == ObjectPoolError::InvalidRequest),
                   !ram.HasValue() && (ram.GetError().Error == ObjectPoolError::InvalidRequest),
                   fixture.Pages.GetSnapshot().AllocatedPages == 0U);
    // A Frame of RAM may ask for a physical alignment; a device Frame's pages are its region's.
    const auto alignedDevice = fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                                        .Authority = fixture.Authority,
                                                                        .Pages = PageCount{1U},
                                                                        .Region = mappable,
                                                                        .RegionOffsetPages = 0U,
                                                                        .AlignmentPages = 2U});
    const auto oddRam = fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                                 .Authority = fixture.Authority,
                                                                 .Pages = PageCount{1U},
                                                                 .Region = CapabilityHandle{},
                                                                 .RegionOffsetPages = 0U,
                                                                 .AlignmentPages = 3U});
    const auto wideRam = fixture.Frames.Create(
        FrameCreateRequest{.Space = &fixture.Space,
                           .Authority = fixture.Authority,
                           .Pages = PageCount{1U},
                           .Region = CapabilityHandle{},
                           .RegionOffsetPages = 0U,
                           .AlignmentPages = Kernel::Memory::FramePool::MaximumAlignmentPages * 2U});
    const auto alignedRam = fixture.Frames.Create(FrameCreateRequest{.Space = &fixture.Space,
                                                                     .Authority = fixture.Authority,
                                                                     .Pages = PageCount{2U},
                                                                     .Region = CapabilityHandle{},
                                                                     .RegionOffsetPages = 0U,
                                                                     .AlignmentPages = 4U});
    const bool alignedDestroyed =
        alignedRam.HasValue() && fixture.Frames.Destroy(fixture.Space, alignedRam.GetValue().Capability).HasValue();
    ASTERIA_EXPECT(runner, "a Frame of RAM takes a power-of-two physical alignment to 512 pages; a device Frame none",
                   !alignedDevice.HasValue() && (alignedDevice.GetError().Error == ObjectPoolError::InvalidRequest),
                   !oddRam.HasValue() && (oddRam.GetError().Error == ObjectPoolError::InvalidRequest),
                   !wideRam.HasValue() && (wideRam.GetError().Error == ObjectPoolError::InvalidRequest),
                   alignedRam.HasValue() && ((alignedRam.GetValue().PhysicalStart.GetValue() % 16384U) == 0U),
                   alignedDestroyed);
    const bool destroyed =
        whole.HasValue() && fixture.Frames.Destroy(fixture.Space, whole.GetValue().Capability).HasValue();
    static_cast<void>(fixture.Frames.Collect());
    if (registered.HasValue())
    {
        static_cast<void>(fixture.Issuer.DestroyObject(registered.GetValue()));
        static_cast<void>(fixture.Issuer.CollectDestroyedObject(registered.GetValue()));
    }
    static_cast<void>(region.Quiesce());
    ASTERIA_EXPECT(runner, "the large device Frame is destroyed like any other", destroyed);
}
// A MemoryObject charges metadata only; its mapping range can span up to
// MaximumPages without committing physical pages.
void TestLargeMemoryObject(TestContext& runner) noexcept
{
    // The pools are too large for a test function's stack budget.
    const auto owned = std::make_unique<PoolFixture>();
    PoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    const auto usage = fixture.Domain.GetSnapshot().Usage;
    const auto create = [&fixture](const Kernel::Memory::PageCount pages)
    {
        return fixture.MemoryObjects.Create(MemoryObjectCreateRequest{
            .Space = &fixture.Space,
            .Authority = fixture.Authority,
            .Pages = pages,
            .MaximumPermissions =
                PagePermissions{PagePermission::Read}.With(PagePermission::Write).With(PagePermission::User),
            .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
        });
    };
    const auto largest = create(PageCount{MemoryObjectPool::MaximumPages});
    const auto charged = fixture.Domain.GetSnapshot().Usage;
    const auto beyond = create(PageCount{MemoryObjectPool::MaximumPages + 1U});
    ASTERIA_EXPECT(runner, "a MemoryObject spans up to MaximumPages, charging metadata only",
                   ready, largest.HasValue(),
                   charged.CommittedMemoryPages == usage.CommittedMemoryPages,
                   charged.ResidentMemoryPages == usage.ResidentMemoryPages,
                   !beyond.HasValue() && (beyond.GetError().Error == ObjectPoolError::InvalidRequest));
}

// Physical memory management: 4,096 Frames and 4,096 MemoryObjects fill their slabs in a registry
// of ObjectRegistry::MaximumObjects entries. Each creation takes the lowest free slot from the
// bitmap and each destruction finds its slot from the object's address, so filling, draining in a
// scattered order and refilling take one bounded step per object; Collect visits only the quiescing
// slots of each batch.
class FullPoolFixture final
{
public:
    // A space holds the domain and region capabilities and 254 created ones.
    static constexpr Size CreatedPerSpace{CapabilitySpace::MaximumSlots - 2U};
    static constexpr Size Objects{FrameObjectPool::Capacity};
    static constexpr UInt64 RegionBase{0x0000'0001'0000'0000ULL};

    FullPoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry}, Domain{Ledger},
          Frames{Registry, Issuer, Pages}, MemoryObjects{Registry, Issuer}
    {
        static_cast<void>(Frames.AttachSlab(Slabs.Frames));
        static_cast<void>(MemoryObjects.AttachSlab(Slabs.MemoryObjects));
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        using Kernel::Device::DeviceAccessWidth;
        using Kernel::Device::DeviceAccessWidths;
        using Kernel::Device::DeviceRegionDescriptor;
        using Kernel::Device::DeviceRegionError;
        using Kernel::Device::DeviceRegionKind;
        using Kernel::Device::DeviceRegionPermission;
        using Kernel::Device::DeviceRegionPermissions;
        constexpr ResourceDomainLimits limits{
            .CommittedMemoryPages = 1U << 16U,
            .ResidentMemoryPages = 1U << 16U,
            .KernelMetadataBytes = 1U << 26U,
            .CapabilityHandles = 0U,
            .IpcQueueBytes = 0U,
            .Threads = 0U,
            .ChildDomains = 0U,
        };
        const auto root = Revocations.CreateRoot();
        if ((Domain.InitializeRoot(ResourceDomainId{5U}, limits).Error != ResourceDomainError::None) ||
            !root.HasValue() || (Pages.Initialize(PhysicalAddress{0x0040'0000U}, 8U) != FramePoolError::None))
        {
            return false;
        }
        Root = root.GetValue();
        const auto domain = Issuer.CreateObject(Domain);
        if (!domain.HasValue())
        {
            return false;
        }
        DomainObject = domain.GetValue();
        // One page more than the pool holds, so the region never refuses a
        // Frame the pool would take.
        if (Region
                .Initialize(Registry, Kernel::Resource::ResourceDomainObjectId{Registry, DomainObject}, Domain,
                            DeviceRegionDescriptor{
                                .BaseAddress = RegionBase,
                                .LengthBytes = (Objects + 1U) * 4096U,
                                .Kind = DeviceRegionKind::MemoryMappedIo,
                                .Permissions = DeviceRegionPermissions{DeviceRegionPermission::Read}.With(
                                    DeviceRegionPermission::Write),
                                .Widths = DeviceAccessWidths{DeviceAccessWidth::DoubleWord},
                                .CachePolicy = Kernel::Memory::MemoryCachePolicy::Uncached,
                                .Reserved = {},
                            })
                .Error != DeviceRegionError::None)
        {
            return false;
        }
        const auto region = Issuer.CreateObject(Region);
        RegionObject = region.HasValue() ? region.GetValue() : Kernel::Object::ObjectId{};
        return RegionObject.IsValid();
    }

    // The space for the next creation, with its domain and region
    // capabilities; a new one every CreatedPerSpace creations.
    [[nodiscard]] CapabilitySpace* NextSpace() noexcept
    {
        if ((m_created % CreatedPerSpace) == 0U)
        {
            Spaces.push_back(std::make_unique<CapabilitySpace>(
                Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL + Spaces.size(), 0xFEDC'BA98'7654'3210ULL));
            CapabilitySpace& space = *Spaces.back();
            const auto authority = Issuer.Mint(space, DomainObject,
                                               CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect),
                                               Root, MonotonicTime::Infinite(), 0U);
            const auto region =
                Issuer.Mint(space, RegionObject, CapabilityRights{CapabilityRight::Map}.With(CapabilityRight::Inspect),
                            Root, MonotonicTime::Infinite(), 0U);
            Authorities.push_back(authority.HasValue() ? authority.GetValue() : CapabilityHandle{});
            Regions.push_back(region.HasValue() ? region.GetValue() : CapabilityHandle{});
        }
        ++m_created;
        return Spaces.back().get();
    }

    [[nodiscard]] auto CreateFrame(const UInt64 page) noexcept
    {
        CapabilitySpace* const space = NextSpace();
        return Frames.Create(FrameCreateRequest{.Space = space,
                                                .Authority = Authorities.back(),
                                                .Pages = PageCount{1U},
                                                .Region = Regions.back(),
                                                .RegionOffsetPages = page});
    }

    [[nodiscard]] auto CreateMemoryObject() noexcept
    {
        CapabilitySpace* const space = NextSpace();
        return MemoryObjects.Create(MemoryObjectCreateRequest{
            .Space = space,
            .Authority = Authorities.back(),
            .Pages = PageCount{1U},
            .MaximumPermissions = PagePermissions{PagePermission::Read}.With(PagePermission::User),
            .CachePolicy = Kernel::Memory::MemoryCachePolicy::WriteBack});
    }

    // The slab slot an object lives in, from its registered instance.
    [[nodiscard]] Size SlotOf(const Kernel::Object::ObjectId object, const ObjectType type,
                              const Foundation::UInt8* const slab, const Size slotBytes) noexcept
    {
        ObjectPin pin;
        if (Registry.PinObject(object, type, pin) != ObjectPinError::Ok)
        {
            return ~Size{0U};
        }
        const auto* const instance = reinterpret_cast<const Foundation::UInt8*>(&pin.GetObject());
        const Size offset = static_cast<Size>(instance - slab);
        return ((offset % slotBytes) == 0U) ? (offset / slotBytes) : ~Size{0U};
    }

    SpinLock Synchronization;
    ManualClock Clock;
    TestSupport::SizedObjectRegistry<ObjectRegistry::MaximumObjects> Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    FramePool Pages;
    Asteria::TestSupport::PoolSlabs Slabs;
    FrameObjectPool Frames;
    MemoryObjectPool MemoryObjects;
    Kernel::Device::DeviceRegion Region;
    RevocationDomainId Root{};
    Kernel::Object::ObjectId DomainObject{};
    Kernel::Object::ObjectId RegionObject{};
    std::vector<std::unique_ptr<CapabilitySpace>> Spaces;
    std::vector<CapabilityHandle> Authorities;
    std::vector<CapabilityHandle> Regions;

private:
    Size m_created{0U};
};

void TestFullPools(TestContext& runner) noexcept
{
    struct Created
    {
        Kernel::Object::ObjectId Object;
        CapabilityHandle Capability;
        CapabilitySpace* Space;
    };
    // The registry and slabs are too large for a test function's stack.
    const auto owned = std::make_unique<FullPoolFixture>();
    FullPoolFixture& fixture = *owned;
    const bool ready = fixture.Initialize();
    // The region's own charge.
    const UInt64 baseline = fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes;
    constexpr Size Objects{FullPoolFixture::Objects};
    constexpr Size FrameSlotBytes{FrameObjectPool::SlabBytes / FrameObjectPool::Capacity};
    constexpr Size MemorySlotBytes{MemoryObjectPool::SlabBytes / MemoryObjectPool::Capacity};
    static_assert(FrameObjectPool::Capacity == MemoryObjectPool::Capacity);
    std::vector<Created> frames(Objects);
    std::vector<Created> memoryObjects(Objects);

    // Fill: each creation lands in the next slot of its slab.
    bool filled{ready};
    for (Size index{0U}; filled && (index < Objects); ++index)
    {
        const auto frame = fixture.CreateFrame(index);
        CapabilitySpace* const frameSpace = fixture.Spaces.back().get();
        const auto memory = fixture.CreateMemoryObject();
        filled = frame.HasValue() && memory.HasValue();
        if (filled)
        {
            frames[index] = Created{frame.GetValue().Frame, frame.GetValue().Capability, frameSpace};
            memoryObjects[index] =
                Created{memory.GetValue().MemoryObject, memory.GetValue().Capability, fixture.Spaces.back().get()};
            filled = (fixture.SlotOf(frame.GetValue().Frame, ObjectType::Frame, fixture.Slabs.Frames, FrameSlotBytes) ==
                      index) &&
                     (fixture.SlotOf(memory.GetValue().MemoryObject, ObjectType::MemoryObject,
                                     fixture.Slabs.MemoryObjects, MemorySlotBytes) == index);
        }
    }
    const auto beyondFrame = filled ? fixture.CreateFrame(Objects) : decltype(fixture.CreateFrame(0U))::Failure({});
    const auto beyondMemory =
        filled ? fixture.CreateMemoryObject() : decltype(fixture.CreateMemoryObject())::Failure({});
    const auto fullFrames = fixture.Frames.GetSnapshot();
    const auto fullMemory = fixture.MemoryObjects.GetSnapshot();
    ASTERIA_EXPECT(runner, "4,096 Frames and 4,096 MemoryObjects fill their slabs slot by slot", ready, filled,
                   fullFrames.ActiveSlots == Objects, fullFrames.FreeSlots == 0U, fullMemory.ActiveSlots == Objects,
                   fullMemory.FreeSlots == 0U, fixture.Registry.GetActiveCount() == (2U * Objects) + 2U,
                   !beyondFrame.HasValue() && (beyondFrame.GetError().Error == ObjectPoolError::CapacityExceeded),
                   !beyondMemory.HasValue() && (beyondMemory.GetError().Error == ObjectPoolError::CapacityExceeded),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes ==
                       baseline + (Objects * (sizeof(Kernel::Memory::CreatedFrame) + sizeof(MemoryObject))));

    // Drain in a scattered order (1,031 is prime to 4,096), 256 at a time;
    // each Collect reclaims exactly the batch it follows.
    constexpr Size Stride{1031U};
    constexpr Size Batch{256U};
    bool drained{filled};
    for (Size first{0U}; drained && (first < Objects); first += Batch)
    {
        for (Size step{first}; drained && (step < first + Batch); ++step)
        {
            const Size index = (step * Stride) % Objects;
            drained =
                fixture.Frames.Destroy(*frames[index].Space, frames[index].Capability).HasValue() &&
                fixture.MemoryObjects.Destroy(*memoryObjects[index].Space, memoryObjects[index].Capability).HasValue();
        }
        const auto frameQuiescing = fixture.Frames.GetSnapshot();
        const auto collectedFrames = fixture.Frames.Collect();
        const auto collectedMemory = fixture.MemoryObjects.Collect();
        drained = drained && (frameQuiescing.QuiescingSlots == Batch) && collectedFrames.HasValue() &&
                  (collectedFrames.GetValue() == Batch) && collectedMemory.HasValue() &&
                  (collectedMemory.GetValue() == Batch) && (fixture.Frames.GetSnapshot().FreeSlots == first + Batch) &&
                  (fixture.MemoryObjects.GetSnapshot().FreeSlots == first + Batch);
    }
    const auto emptyFrames = fixture.Frames.GetSnapshot();
    const auto emptyMemory = fixture.MemoryObjects.GetSnapshot();
    ASTERIA_EXPECT(runner, "destroying and collecting them in a scattered order returns every slot and charge", drained,
                   emptyFrames.ActiveSlots == 0U, emptyFrames.QuiescingSlots == 0U, emptyFrames.FreeSlots == Objects,
                   emptyFrames.Reclaimed == Objects, emptyMemory.FreeSlots == Objects, emptyMemory.Reclaimed == Objects,
                   fixture.Registry.GetActiveCount() == 2U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == baseline);

    // Refill: the slots come back lowest first.
    bool refilled{drained};
    for (Size index{0U}; refilled && (index < Objects); ++index)
    {
        const auto frame = fixture.CreateFrame(index);
        refilled = frame.HasValue() && (fixture.SlotOf(frame.GetValue().Frame, ObjectType::Frame, fixture.Slabs.Frames,
                                                       FrameSlotBytes) == index);
    }
    ASTERIA_EXPECT(runner, "a drained slab refills from its lowest slot to its last", refilled,
                   fixture.Frames.GetSnapshot().ActiveSlots == Objects);
}
void TestLargeRamFrame(TestContext& runner) noexcept
{
    auto fixture = std::make_unique<PoolFixture>();
    const bool ready = fixture->Initialize(1024U, 1024U);
    const auto created = fixture->Frames.Create({.Space = &fixture->Space,
                                                 .Authority = fixture->Authority,
                                                 .Pages = PageCount{512U},
                                                 .Region = {},
                                                 .RegionOffsetPages = 0U,
                                                 .AlignmentPages = 512U});
    ASTERIA_EXPECT(runner, "the native run maximum charges 512 pages and metadata at a 2 MiB alignment", ready,
                   created.HasValue(), fixture->Domain.GetUsage().CommittedMemoryPages == 512U,
                   fixture->Domain.GetUsage().ResidentMemoryPages == 512U,
                   fixture->Domain.GetUsage().KernelMetadataBytes == sizeof(Kernel::Memory::CreatedFrame),
                   created.HasValue() && created.GetValue().PhysicalStart.GetValue() % (512U * 4096U) == 0U);
    const bool destroyed =
        created.HasValue() && fixture->Frames.Destroy(fixture->Space, created.GetValue().Capability).HasValue();
    static_cast<void>(fixture->Frames.Collect());
    ASTERIA_EXPECT(
        runner, "destroying the large Frame releases its complete charge and run", destroyed,
        fixture->Domain.GetUsage().CommittedMemoryPages == 0U, fixture->Domain.GetUsage().ResidentMemoryPages == 0U,
        fixture->Domain.GetUsage().KernelMetadataBytes == 0U, fixture->Pages.GetSnapshot().AllocatedPages == 0U);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"FrameObjectPoolTests"};
    Asteria::Test::TestLargeRamFrame(runner);
    Asteria::Test::TestMintRollbackReturnsCleanPages(runner);
    Asteria::Test::TestCreationZeroing(runner);
    Asteria::Test::TestDirtyMintRollback(runner);
    Asteria::Test::TestBeforeZeroRollbacks(runner);
    Asteria::Test::TestRetryMintRollback(runner);
    Asteria::Test::TestCreationProbe(runner);
    Asteria::Test::TestDirtyRegistrationRollback(runner);
    Asteria::Test::TestPinnedFrameWaitsForCollection(runner);
    Asteria::Test::TestDeviceFrames(runner);
    Asteria::Test::TestLargeDeviceFrame(runner);
    Asteria::Test::TestLargeMemoryObject(runner);
    Asteria::Test::TestFullPools(runner);
    return runner.Finish();
}
