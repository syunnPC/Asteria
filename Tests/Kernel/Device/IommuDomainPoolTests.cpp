// SPDX-License-Identifier: Apache-2.0
#include "Kernel/Device/IommuDomainPool.hpp"
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
using Hal::Device::IIommuDevice;
using Hal::Device::IIommuDomainBackend;
using Hal::Device::IommuBackendError;
using Hal::Device::IommuBackendFaults;
using Hal::Device::IommuBackendMapRequest;
using Hal::Device::IommuBackendOwnershipError;
using Kernel::Device::IIommuDomainQuiescer;
using Kernel::Device::IommuDomain;
using Kernel::Device::IommuDomainCreateRequest;
using Kernel::Device::IommuDomainCreation;
using Kernel::Device::IommuDomainError;
using Kernel::Device::IommuDomainPool;
using Kernel::Device::IommuDomainSlot;
using Kernel::Device::IommuDomainState;
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
using Kernel::Resource::IResourceDomainSubtree;
using Kernel::Resource::ResourceDomain;
using Kernel::Resource::ResourceDomainError;
using Kernel::Resource::ResourceDomainLedger;
using Kernel::Resource::ResourceDomainLimits;
using Platform::Synchronization::SpinLock;
using TestSupport::ManualClock;

using CreateResult = Foundation::Result<IommuDomainCreation, ObjectPoolFailure>;

/// Claims for an owner up to MaximumBits input bits; nothing else is used.
class FakeBackend final : public IIommuDomainBackend
{
public:
    [[nodiscard]] bool IsReady() const noexcept override
    {
        return true;
    }

    [[nodiscard]] IommuBackendOwnershipError Claim(const IommuDomain& owner,
                                                   const Foundation::UInt8 inputAddressBits) noexcept override
    {
        if (m_owner != nullptr)
        {
            return IommuBackendOwnershipError::AlreadyOwned;
        }
        if (inputAddressBits > MaximumBits)
        {
            return IommuBackendOwnershipError::UnsupportedConfiguration;
        }
        m_owner = &owner;
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] IommuBackendOwnershipError Release(const IommuDomain& owner) noexcept override
    {
        if (m_owner != &owner)
        {
            return IommuBackendOwnershipError::InvalidOwner;
        }
        m_owner = nullptr;
        return IommuBackendOwnershipError::None;
    }

    [[nodiscard]] Kernel::Device::DmaAddress
    ChooseDeviceAddress(Kernel::Device::DmaAddress requested, Kernel::Memory::PhysicalAddress) const noexcept override
    {
        return requested;
    }

    [[nodiscard]] IommuBackendError Map(const IommuBackendMapRequest&) noexcept override
    {
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Unmap(const IommuBackendMapRequest&) noexcept override
    {
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Attach(IIommuDevice&) noexcept override
    {
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendError Detach(IIommuDevice&) noexcept override
    {
        return IommuBackendError::None;
    }

    [[nodiscard]] IommuBackendFaults GetFaults() noexcept override
    {
        return {};
    }

    [[nodiscard]] bool IsClaimed() const noexcept
    {
        return m_owner != nullptr;
    }

    Foundation::UInt8 MaximumBits{39U};

private:
    const IommuDomain* m_owner{nullptr};
};

/// Quiesces each domain it is given, unless told to refuse.
class FakeQuiescer final : public IIommuDomainQuiescer
{
public:
    [[nodiscard]] bool QuiesceDomain(IommuDomain& domain) noexcept override
    {
        ++Calls;
        return !Refuse && (domain.Quiesce().Error == IommuDomainError::None);
    }

    [[nodiscard]] bool QuiesceDrainedDomain(IommuDomain& domain,
                                            const Kernel::Resource::IResourceDomainSubtree&) noexcept override
    {
        ++Drained;
        return QuiesceDomain(domain);
    }

    Size Calls{0U};
    Size Drained{0U};
    bool Refuse{false};
};

class OneDomain final : public IResourceDomainSubtree
{
public:
    explicit OneDomain(const ResourceDomain& domain) noexcept : m_domain{domain}
    {
    }

    [[nodiscard]] bool Contains(const ResourceDomain* const domain) const noexcept override
    {
        return domain == &m_domain;
    }

private:
    const ResourceDomain& m_domain;
};

class PoolFixture final
{
public:
    PoolFixture() noexcept
        : Registry{Synchronization}, Revocations{Synchronization}, Issuer{Registry},
          Space{Registry, Revocations, Clock, 0x0123'4567'89AB'CDEFULL, 0xFEDC'BA98'7654'3210ULL}, Domain{Ledger},
          Pool{Registry, Issuer}
    {
    }

    [[nodiscard]] bool Initialize() noexcept
    {
        const ResourceDomainLimits limits{.CommittedMemoryPages = 0U,
                                          .ResidentMemoryPages = 0U,
                                          .KernelMetadataBytes = 4U * sizeof(IommuDomain),
                                          .CapabilityHandles = 0U,
                                          .IpcQueueBytes = 0U,
                                          .Threads = 0U,
                                          .ChildDomains = 0U};
        if (Domain.InitializeRoot(ResourceDomainId{5U}, limits).Error != ResourceDomainError::None)
        {
            return false;
        }
        const auto registered = Issuer.CreateObject(Domain);
        const auto root = Revocations.CreateRoot();
        if (!registered.HasValue() || !root.HasValue())
        {
            return false;
        }
        const auto minted = Issuer.Mint(Space, registered.GetValue(),
                                        CapabilityRights{CapabilityRight::Manage}.With(CapabilityRight::Inspect),
                                        root.GetValue(), MonotonicTime::Infinite(), 0U);
        Authority = minted.HasValue() ? minted.GetValue() : CapabilityHandle{};
        return Authority.IsValid();
    }

    [[nodiscard]] bool Configure() noexcept
    {
        IIommuDomainBackend* const backends[IommuDomainPool::Capacity]{&Backends[0U], &Backends[1U], &Backends[2U],
                                                                       &Backends[3U]};
        return Pool.Configure(Slots, backends);
    }

    [[nodiscard]] CreateResult Create(const Foundation::UInt8 bits = 32U) noexcept
    {
        return Pool.Create(IommuDomainCreateRequest{
            .Space = &Space, .Authority = Authority, .InputAddressBits = bits, .Reserved = {}});
    }

    [[nodiscard]] IommuDomain* Find(const CapabilityHandle handle) noexcept
    {
        const auto lease = Space.Lookup(handle, ObjectType::IommuDomain, CapabilityOperation::Map);
        return lease.HasValue() ? &static_cast<IommuDomain&>(lease.GetValue().GetObject()) : nullptr;
    }

    SpinLock Synchronization;
    ManualClock Clock;
    Asteria::TestSupport::TestObjectRegistry Registry;
    RevocationRegistry Revocations;
    RootCapabilityIssuer Issuer;
    CapabilitySpace Space;
    ResourceDomainLedger Ledger;
    ResourceDomain Domain;
    IommuDomainPool Pool;
    FakeBackend Backends[IommuDomainPool::Capacity];
    IommuDomainSlot Slots[IommuDomainPool::Capacity]{};
    FakeQuiescer Quiescer;
    CapabilityHandle Authority{};
};

[[nodiscard]] bool IsFailure(const CreateResult& result, const ObjectPoolError error) noexcept
{
    return !result.HasValue() && (result.GetError().Error == error);
}

// ObjectCreate type 16 initializes an IommuDomain on its slot's backend, charged to the authority's
// domain; without platform slots there is none to create.
void TestCreate(TestContext& runner) noexcept
{
    static PoolFixture fixture;
    const bool ready = fixture.Initialize();
    const CreateResult unconfigured = fixture.Create();
    const bool configured = fixture.Configure();
    const bool again = fixture.Configure();
    const CreateResult narrow = fixture.Create(31U);
    const CreateResult wide = fixture.Create(40U);
    const CreateResult created = fixture.Create();
    IommuDomain* const domain = created.HasValue() ? fixture.Find(created.GetValue().Capability) : nullptr;
    const auto usage = fixture.Domain.GetSnapshot().Usage;
    ASTERIA_EXPECT(runner, "an IommuDomain is created on a slot's backend and charged to its authority", ready,
                   IsFailure(unconfigured, ObjectPoolError::InvalidConfiguration), configured, !again,
                   IsFailure(narrow, ObjectPoolError::InvalidRequest), IsFailure(wide, ObjectPoolError::InvalidRequest),
                   created.HasValue(), domain != nullptr,
                   (domain != nullptr) && (domain->GetSnapshot().State == IommuDomainState::Ready),
                   (domain != nullptr) && (domain->GetSnapshot().Configuration.InputAddressBits == 32U),
                   fixture.Backends[0U].IsClaimed(), usage.KernelMetadataBytes == sizeof(IommuDomain),
                   fixture.Pool.GetSnapshot().ActiveSlots == 1U, fixture.Pool.GetSnapshot().Created == 1U);

    bool filled{true};
    for (Size index{1U}; index < IommuDomainPool::Capacity; ++index)
    {
        filled = filled && fixture.Create().HasValue();
    }
    ASTERIA_EXPECT(runner, "the pool holds one domain per backend", filled,
                   IsFailure(fixture.Create(), ObjectPoolError::CapacityExceeded),
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == (4U * sizeof(IommuDomain)));
}

// Destroy has the quiescer stop the domain first; a refusal keeps it live.
// The drain destroys every domain charged into its subtree, and Collect
// frees each slot once the registry lets it go.
void TestDestroyAndDrain(TestContext& runner) noexcept
{
    static PoolFixture fixture;
    const bool ready = fixture.Initialize() && fixture.Configure();
    const CreateResult first = fixture.Create();
    const CreateResult second = fixture.Create();
    const CapabilityHandle handle = first.HasValue() ? first.GetValue().Capability : CapabilityHandle{};
    fixture.Quiescer.Refuse = true;
    const auto refused = fixture.Pool.Destroy(fixture.Space, handle, fixture.Quiescer);
    const bool stillLive = fixture.Find(handle) != nullptr;
    fixture.Quiescer.Refuse = false;
    const auto destroyed = fixture.Pool.Destroy(fixture.Space, handle, fixture.Quiescer);
    const bool gone = fixture.Find(handle) == nullptr;
    const auto twice = fixture.Pool.Destroy(fixture.Space, handle, fixture.Quiescer);
    const auto collected = fixture.Pool.Collect();
    ASTERIA_EXPECT(runner, "a destroyed IommuDomain is quiesced first, then collected", ready, first.HasValue(),
                   second.HasValue(), !refused.HasValue(), refused.GetError().Error == ObjectPoolError::DestroyRejected,
                   stillLive, destroyed.HasValue(), gone, !twice.HasValue(), fixture.Quiescer.Calls == 2U,
                   !fixture.Backends[0U].IsClaimed(), collected.HasValue() && (collected.GetValue() == 1U),
                   fixture.Pool.GetSnapshot().ActiveSlots == 1U,
                   fixture.Domain.GetSnapshot().Usage.KernelMetadataBytes == sizeof(IommuDomain));

    const OneDomain subtree{fixture.Domain};
    const ObjectPoolFailure drained = fixture.Pool.DestroyCharged(subtree, fixture.Quiescer);
    const auto reclaimed = fixture.Pool.Collect();
    const CreateResult reused = fixture.Create();
    ASTERIA_EXPECT(runner, "the drain quiesces and destroys every domain charged into the subtree",
                   drained.Error == ObjectPoolError::None, fixture.Quiescer.Calls == 3U,
                   !fixture.Backends[1U].IsClaimed(), reclaimed.HasValue() && (reclaimed.GetValue() == 1U),
                   fixture.Pool.GetSnapshot().Reclaimed == 2U, reused.HasValue(),
                   fixture.Pool.GetSnapshot().ActiveSlots == 1U);
}
} // Namespace
} // Namespace Asteria::Test

int main()
{
    TestContext runner{"IommuDomainPoolTests"};
    Asteria::Test::TestCreate(runner);
    Asteria::Test::TestDestroyAndDrain(runner);
    return runner.Finish();
}
